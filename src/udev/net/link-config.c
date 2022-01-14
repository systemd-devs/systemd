/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/netdevice.h>
#include <netinet/ether.h>
#include <unistd.h>

#include "sd-device.h"
#include "sd-netlink.h"

#include "alloc-util.h"
#include "arphrd-util.h"
#include "conf-files.h"
#include "conf-parser.h"
#include "creds-util.h"
#include "def.h"
#include "device-private.h"
#include "device-util.h"
#include "ethtool-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "link-config.h"
#include "log-link.h"
#include "memory-util.h"
#include "net-condition.h"
#include "netif-sriov.h"
#include "netif-util.h"
#include "netlink-util.h"
#include "parse-util.h"
#include "path-lookup.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "random-util.h"
#include "stat-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "utf8.h"

struct LinkConfigContext {
        LIST_HEAD(LinkConfig, configs);
        int ethtool_fd;
        bool enable_name_policy;
        usec_t network_dirs_ts_usec;
};

static LinkConfig* link_config_free(LinkConfig *config) {
        if (!config)
                return NULL;

        free(config->filename);

        net_match_clear(&config->match);
        condition_free_list(config->conditions);

        free(config->description);
        free(config->name_policy);
        free(config->name);
        strv_free(config->alternative_names);
        free(config->alternative_names_policy);
        free(config->alias);
        free(config->wol_password_file);
        erase_and_free(config->wol_password);

        ordered_hashmap_free_with_destructor(config->sr_iov_by_section, sr_iov_free);

        return mfree(config);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(LinkConfig*, link_config_free);

static void link_configs_free(LinkConfigContext *ctx) {
        LinkConfig *config, *config_next;

        if (!ctx)
                return;

        LIST_FOREACH_SAFE(configs, config, config_next, ctx->configs)
                link_config_free(config);
}

LinkConfigContext *link_config_ctx_free(LinkConfigContext *ctx) {
        if (!ctx)
                return NULL;

        safe_close(ctx->ethtool_fd);
        link_configs_free(ctx);
        return mfree(ctx);
}

int link_config_ctx_new(LinkConfigContext **ret) {
        _cleanup_(link_config_ctx_freep) LinkConfigContext *ctx = NULL;

        if (!ret)
                return -EINVAL;

        ctx = new(LinkConfigContext, 1);
        if (!ctx)
                return -ENOMEM;

        *ctx = (LinkConfigContext) {
                .ethtool_fd = -1,
                .enable_name_policy = true,
        };

        *ret = TAKE_PTR(ctx);

        return 0;
}

static int link_parse_wol_password(LinkConfig *config, const char *str) {
        _cleanup_(erase_and_freep) uint8_t *p = NULL;
        int r;

        assert(config);
        assert(str);

        assert_cc(sizeof(struct ether_addr) == SOPASS_MAX);

        p = new(uint8_t, SOPASS_MAX);
        if (!p)
                return -ENOMEM;

        /* Reuse parse_ether_addr(), as their formats are equivalent. */
        r = parse_ether_addr(str, (struct ether_addr*) p);
        if (r < 0)
                return r;

        erase_and_free(config->wol_password);
        config->wol_password = TAKE_PTR(p);
        return 0;
}

static int link_read_wol_password_from_file(LinkConfig *config) {
        _cleanup_(erase_and_freep) char *password = NULL;
        int r;

        assert(config);

        if (!config->wol_password_file)
                return 0;

        r = read_full_file_full(
                        AT_FDCWD, config->wol_password_file, UINT64_MAX, SIZE_MAX,
                        READ_FULL_FILE_SECURE | READ_FULL_FILE_WARN_WORLD_READABLE | READ_FULL_FILE_CONNECT_SOCKET,
                        NULL, &password, NULL);
        if (r < 0)
                return r;

        return link_parse_wol_password(config, password);
}

static int link_read_wol_password_from_cred(LinkConfig *config) {
        _cleanup_free_ char *base = NULL, *cred_name = NULL;
        _cleanup_(erase_and_freep) char *password = NULL;
        int r;

        assert(config);
        assert(config->filename);

        if (config->wol == UINT32_MAX)
                return 0; /* WakeOnLan= is not specified. */
        if (!FLAGS_SET(config->wol, WAKE_MAGICSECURE))
                return 0; /* secureon is not specified in WakeOnLan=. */
        if (config->wol_password)
                return 0; /* WakeOnLanPassword= is specified. */
        if (config->wol_password_file)
                return 0; /* a file name is specified in WakeOnLanPassword=, but failed to read it. */

        r = path_extract_filename(config->filename, &base);
        if (r < 0)
                return r;

        cred_name = strjoin(base, ".wol.password");
        if (!cred_name)
                return -ENOMEM;

        r = read_credential(cred_name, (void**) &password, NULL);
        if (r == -ENOENT)
                r = read_credential("wol.password", (void**) &password, NULL);
        if (r < 0)
                return r;

        return link_parse_wol_password(config, password);
}

static int link_adjust_wol_options(LinkConfig *config) {
        int r;

        assert(config);

        r = link_read_wol_password_from_file(config);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0)
                log_warning_errno(r, "Failed to read WakeOnLan password from %s, ignoring: %m", config->wol_password_file);

        r = link_read_wol_password_from_cred(config);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0)
                log_warning_errno(r, "Failed to read WakeOnLan password from credential, ignoring: %m");

        if (config->wol != UINT32_MAX && config->wol_password)
                /* Enable WAKE_MAGICSECURE flag when WakeOnLanPassword=. Note that when
                 * WakeOnLanPassword= is set without WakeOnLan=, then ethtool_set_wol() enables
                 * WAKE_MAGICSECURE flag and other flags are not changed. */
                config->wol |= WAKE_MAGICSECURE;

        return 0;
}

int link_load_one(LinkConfigContext *ctx, const char *filename) {
        _cleanup_(link_config_freep) LinkConfig *config = NULL;
        _cleanup_free_ char *name = NULL;
        const char *dropin_dirname;
        size_t i;
        int r;

        assert(ctx);
        assert(filename);

        r = null_or_empty_path(filename);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;
        if (r > 0) {
                log_debug("Skipping empty file: %s", filename);
                return 0;
        }

        name = strdup(filename);
        if (!name)
                return -ENOMEM;

        config = new(LinkConfig, 1);
        if (!config)
                return -ENOMEM;

        *config = (LinkConfig) {
                .filename = TAKE_PTR(name),
                .mac_address_policy = MAC_ADDRESS_POLICY_NONE,
                .wol = UINT32_MAX, /* UINT32_MAX means do not change WOL setting. */
                .duplex = _DUP_INVALID,
                .port = _NET_DEV_PORT_INVALID,
                .autonegotiation = -1,
                .rx_flow_control = -1,
                .tx_flow_control = -1,
                .autoneg_flow_control = -1,
                .txqueuelen = UINT32_MAX,
                .coalesce.use_adaptive_rx_coalesce = -1,
                .coalesce.use_adaptive_tx_coalesce = -1,
                .sr_iov_num_vfs = UINT32_MAX,
        };

        for (i = 0; i < ELEMENTSOF(config->features); i++)
                config->features[i] = -1;

        dropin_dirname = strjoina(basename(filename), ".d");
        r = config_parse_many(
                        STRV_MAKE_CONST(filename),
                        (const char* const*) CONF_PATHS_STRV("systemd/network"),
                        dropin_dirname,
                        "Match\0"
                        "Link\0"
                        "SR-IOV\0",
                        config_item_perf_lookup, link_config_gperf_lookup,
                        CONFIG_PARSE_WARN, config, NULL);
        if (r < 0)
                return r;

        if (net_match_is_empty(&config->match) && !config->conditions) {
                log_warning("%s: No valid settings found in the [Match] section, ignoring file. "
                            "To match all interfaces, add OriginalName=* in the [Match] section.",
                            filename);
                return 0;
        }

        if (!condition_test_list(config->conditions, environ, NULL, NULL, NULL)) {
                log_debug("%s: Conditions do not match the system environment, skipping.", filename);
                return 0;
        }

        if (IN_SET(config->mac_address_policy, MAC_ADDRESS_POLICY_PERSISTENT, MAC_ADDRESS_POLICY_RANDOM) &&
            config->hw_addr.length > 0)
                log_warning("%s: MACAddress= in [Link] section will be ignored when MACAddressPolicy= "
                            "is set to \"persistent\" or \"random\".",
                            filename);

        r = link_adjust_wol_options(config);
        if (r < 0)
                return r;

        r = sr_iov_drop_invalid_sections(config->sr_iov_num_vfs, config->sr_iov_by_section);
        if (r < 0)
                return r;

        log_debug("Parsed configuration file %s", filename);

        LIST_PREPEND(configs, ctx->configs, TAKE_PTR(config));
        return 0;
}

static bool enable_name_policy(void) {
        bool b;

        return proc_cmdline_get_bool("net.ifnames", &b) <= 0 || b;
}

static int device_unsigned_attribute(sd_device *device, const char *attr, unsigned *type) {
        const char *s;
        int r;

        r = sd_device_get_sysattr_value(device, attr, &s);
        if (r < 0)
                return log_device_debug_errno(device, r, "Failed to query %s: %m", attr);

        r = safe_atou(s, type);
        if (r < 0)
                return log_device_warning_errno(device, r, "Failed to parse %s \"%s\": %m", attr, s);

        log_device_debug(device, "Device has %s=%u", attr, *type);
        return 0;
}

int link_config_load(LinkConfigContext *ctx) {
        _cleanup_strv_free_ char **files = NULL;
        char **f;
        int r;

        link_configs_free(ctx);

        if (!enable_name_policy()) {
                ctx->enable_name_policy = false;
                log_info("Network interface NamePolicy= disabled on kernel command line, ignoring.");
        }

        /* update timestamp */
        paths_check_timestamp(NETWORK_DIRS, &ctx->network_dirs_ts_usec, true);

        r = conf_files_list_strv(&files, ".link", NULL, 0, NETWORK_DIRS);
        if (r < 0)
                return log_error_errno(r, "failed to enumerate link files: %m");

        STRV_FOREACH_BACKWARDS(f, files) {
                r = link_load_one(ctx, *f);
                if (r < 0)
                        log_error_errno(r, "Failed to load %s, ignoring: %m", *f);
        }

        return 0;
}

bool link_config_should_reload(LinkConfigContext *ctx) {
        return paths_check_timestamp(NETWORK_DIRS, &ctx->network_dirs_ts_usec, false);
}

Link *link_free(Link *link) {
        if (!link)
                return NULL;

        sd_device_unref(link->device);
        free(link->driver);
        return mfree(link);
}

int link_new(LinkConfigContext *ctx, sd_netlink **rtnl, sd_device *device, Link **ret) {
        _cleanup_(link_freep) Link *link = NULL;
        int r;

        assert(ctx);
        assert(rtnl);
        assert(device);
        assert(ret);

        link = new(Link, 1);
        if (!link)
                return -ENOMEM;

        *link = (Link) {
                .device = sd_device_ref(device),
        };

        r = sd_device_get_sysname(device, &link->ifname);
        if (r < 0)
                return r;

        r = sd_device_get_ifindex(device, &link->ifindex);
        if (r < 0)
                return r;

        r = sd_device_get_action(device, &link->action);
        if (r < 0)
                return r;

        r = device_unsigned_attribute(device, "name_assign_type", &link->name_assign_type);
        if (r < 0)
                log_link_debug_errno(link, r, "Failed to get \"name_assign_type\" attribute, ignoring: %m");

        r = device_unsigned_attribute(device, "addr_assign_type", &link->addr_assign_type);
        if (r < 0)
                log_link_debug_errno(link, r, "Failed to get \"addr_assign_type\" attribute, ignoring: %m");

        r = rtnl_get_link_info(rtnl, link->ifindex, &link->iftype, &link->flags, &link->hw_addr, &link->permanent_hw_addr);
        if (r < 0)
                return r;

        if (link->hw_addr.length > 0 && link->permanent_hw_addr.length == 0) {
                r = ethtool_get_permanent_hw_addr(&ctx->ethtool_fd, link->ifname, &link->permanent_hw_addr);
                if (r < 0)
                        log_link_debug_errno(link, r, "Failed to get permanent hardware address, ignoring: %m");
        }

        r = ethtool_get_driver(&ctx->ethtool_fd, link->ifname, &link->driver);
        if (r < 0)
                log_link_debug_errno(link, r, "Failed to get driver, ignoring: %m");

        *ret = TAKE_PTR(link);
        return 0;
}

int link_get_config(LinkConfigContext *ctx, Link *link) {
        LinkConfig *config;
        int r;

        assert(ctx);
        assert(link);

        /* Do not configure loopback interfaces by .link files. */
        if (link->flags & IFF_LOOPBACK)
                return -ENOENT;

        LIST_FOREACH(configs, config, ctx->configs) {
                r = net_match_config(
                                &config->match,
                                link->device,
                                &link->hw_addr,
                                &link->permanent_hw_addr,
                                link->driver,
                                link->iftype,
                                link->ifname,
                                /* alternative_names = */ NULL,
                                /* wlan_iftype = */ 0,
                                /* ssid = */ NULL,
                                /* bssid = */ NULL);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                if (config->match.ifname && !strv_contains(config->match.ifname, "*") && link->name_assign_type == NET_NAME_ENUM)
                        log_link_warning(link, "Config file %s is applied to device based on potentially unpredictable interface name.",
                                         config->filename);
                else
                        log_link_debug(link, "Config file %s is applied", config->filename);

                link->config = config;
                return 0;
        }

        return -ENOENT;
}

static int link_apply_ethtool_settings(Link *link, int *ethtool_fd) {
        LinkConfig *config;
        const char *name;
        int r;

        assert(link);
        assert(link->config);
        assert(ethtool_fd);

        config = link->config;
        name = link->ifname;

        r = ethtool_set_glinksettings(ethtool_fd, name,
                                      config->autonegotiation, config->advertise,
                                      config->speed, config->duplex, config->port);
        if (r < 0) {
                if (config->autonegotiation >= 0)
                        log_link_warning_errno(link, r, "Could not %s auto negotiation, ignoring: %m",
                                               enable_disable(config->autonegotiation));

                if (!eqzero(config->advertise))
                        log_link_warning_errno(link, r, "Could not set advertise mode, ignoring: %m");

                if (config->speed > 0)
                        log_link_warning_errno(link, r, "Could not set speed to %"PRIu64"Mbps, ignoring: %m",
                                               DIV_ROUND_UP(config->speed, 1000000));

                if (config->duplex >= 0)
                        log_link_warning_errno(link, r, "Could not set duplex to %s, ignoring: %m",
                                               duplex_to_string(config->duplex));

                if (config->port >= 0)
                        log_link_warning_errno(link, r, "Could not set port to '%s', ignoring: %m",
                                               port_to_string(config->port));
        }

        r = ethtool_set_wol(ethtool_fd, name, config->wol, config->wol_password);
        if (r < 0) {
                _cleanup_free_ char *str = NULL;

                (void) wol_options_to_string_alloc(config->wol, &str);
                log_link_warning_errno(link, r, "Could not set WakeOnLan%s%s, ignoring: %m",
                                       isempty(str) ? "" : " to ", strempty(str));
        }

        r = ethtool_set_features(ethtool_fd, name, config->features);
        if (r < 0)
                log_link_warning_errno(link, r, "Could not set offload features, ignoring: %m");

        r = ethtool_set_channels(ethtool_fd, name, &config->channels);
        if (r < 0)
                log_link_warning_errno(link, r, "Could not set channels, ignoring: %m");

        r = ethtool_set_nic_buffer_size(ethtool_fd, name, &config->ring);
        if (r < 0)
                log_link_warning_errno(link, r, "Could not set ring buffer, ignoring: %m");

        r = ethtool_set_flow_control(ethtool_fd, name, config->rx_flow_control, config->tx_flow_control, config->autoneg_flow_control);
        if (r < 0)
                log_link_warning_errno(link, r, "Could not set flow control, ignoring: %m");

        r = ethtool_set_nic_coalesce_settings(ethtool_fd, name, &config->coalesce);
        if (r < 0)
                log_link_warning_errno(link, r, "Could not set coalesce settings, ignoring: %m");

        return 0;
}

static bool hw_addr_is_valid(Link *link, const struct hw_addr_data *hw_addr) {
        assert(link);
        assert(hw_addr);

        switch (link->iftype) {
        case ARPHRD_ETHER:
                /* Refuse all zero and all 0xFF. */
                assert(hw_addr->length == ETH_ALEN);
                return !ether_addr_is_null(&hw_addr->ether) && !ether_addr_is_broadcast(&hw_addr->ether);

        case ARPHRD_INFINIBAND:
                /* The last 8 bytes cannot be zero*/
                assert(hw_addr->length == INFINIBAND_ALEN);
                return !memeqzero(hw_addr->bytes + INFINIBAND_ALEN - 8, 8);

        default:
                assert_not_reached();
        }
}

static int link_generate_new_hw_addr(Link *link, struct hw_addr_data *ret) {
        struct hw_addr_data hw_addr = HW_ADDR_NULL;
        bool is_static = false;
        uint8_t *p;
        size_t len;
        int r;

        assert(link);
        assert(link->config);
        assert(link->device);
        assert(ret);

        if (link->hw_addr.length == 0)
                goto finalize;

        if (link->config->mac_address_policy == MAC_ADDRESS_POLICY_NONE) {
                log_link_debug(link, "Using static MAC address.");
                hw_addr = link->config->hw_addr;
                is_static = true;
                goto finalize;
        }

        if (!IN_SET(link->iftype, ARPHRD_ETHER, ARPHRD_INFINIBAND))
                goto finalize;

        switch (link->addr_assign_type) {
        case NET_ADDR_SET:
                log_link_debug(link, "MAC address on the device already set by userspace.");
                goto finalize;
        case NET_ADDR_STOLEN:
                log_link_debug(link, "MAC address on the device already set based on another device.");
                goto finalize;
        case NET_ADDR_RANDOM:
        case NET_ADDR_PERM:
                break;
        default:
                log_link_warning(link, "Unknown addr_assign_type %u, ignoring", link->addr_assign_type);
                goto finalize;
        }

        if ((link->config->mac_address_policy == MAC_ADDRESS_POLICY_RANDOM) == (link->addr_assign_type == NET_ADDR_RANDOM)) {
                log_link_debug(link, "MAC address on the device already matches policy \"%s\".",
                               mac_address_policy_to_string(link->config->mac_address_policy));
                goto finalize;
        }

        hw_addr = (struct hw_addr_data) {
                .length = arphrd_to_hw_addr_len(link->iftype),
        };

        switch (link->iftype) {
        case ARPHRD_ETHER:
                p = hw_addr.bytes;
                len = hw_addr.length;
                break;
        case ARPHRD_INFINIBAND:
                p = hw_addr.bytes + INFINIBAND_ALEN - 8;
                len = 8;
                break;
        default:
                assert_not_reached();
        }

        if (link->config->mac_address_policy == MAC_ADDRESS_POLICY_RANDOM)
                /* We require genuine randomness here, since we want to make sure we won't collide with other
                 * systems booting up at the very same time. We do allow RDRAND however, since this is not
                 * cryptographic key material. */
                for (;;) {
                        r = genuine_random_bytes(p, len, RANDOM_ALLOW_RDRAND);
                        if (r < 0)
                                return log_link_warning_errno(link, r, "Failed to acquire random data to generate MAC address: %m");

                        if (hw_addr_is_valid(link, &hw_addr))
                                break;
                }

        else {
                uint64_t result;

                r = net_get_unique_predictable_data(link->device,
                                                    naming_scheme_has(NAMING_STABLE_VIRTUAL_MACS),
                                                    &result);
                if (r < 0)
                        return log_link_warning_errno(link, r, "Could not generate persistent MAC address: %m");

                assert(len <= sizeof(result));
                memcpy(p, &result, len);
                if (!hw_addr_is_valid(link, &hw_addr))
                        return log_link_warning_errno(link, SYNTHETIC_ERRNO(EINVAL),
                                                      "Could not generate valid persistent MAC address: %m");
        }

finalize:

        r = net_verify_hardware_address(link->ifname, is_static, link->iftype, &link->hw_addr, &hw_addr);
        if (r < 0)
                return r;

        if (hw_addr_equal(&link->hw_addr, &hw_addr)) {
                *ret = HW_ADDR_NULL;
                return 0;
        }

        if (hw_addr.length > 0)
                log_link_debug(link, "Applying %s MAC address: %s",
                               link->config->mac_address_policy == MAC_ADDRESS_POLICY_NONE ? "static" :
                               mac_address_policy_to_string(link->config->mac_address_policy),
                               HW_ADDR_TO_STR(&hw_addr));

        *ret = hw_addr;
        return 0;
}

static int link_apply_rtnl_settings(Link *link, sd_netlink **rtnl) {
        struct hw_addr_data hw_addr = {};
        LinkConfig *config;
        int r;

        assert(link);
        assert(link->config);
        assert(rtnl);

        config = link->config;

        (void) link_generate_new_hw_addr(link, &hw_addr);

        r = rtnl_set_link_properties(rtnl, link->ifindex, config->alias, &hw_addr,
                                     config->txqueues, config->rxqueues, config->txqueuelen,
                                     config->mtu, config->gso_max_size, config->gso_max_segments);
        if (r < 0)
                log_link_warning_errno(link, r,
                                       "Could not set Alias=, MACAddress=/MACAddressPolicy=, "
                                       "TransmitQueues=, ReceiveQueues=, TransmitQueueLength=, MTUBytes=, "
                                       "GenericSegmentOffloadMaxBytes= or GenericSegmentOffloadMaxSegments=, "
                                       "ignoring: %m");

        return 0;
}

static int link_generate_new_name(Link *link, bool enable_name_policy) {
        LinkConfig *config;
        sd_device *device;

        assert(link);
        assert(link->config);
        assert(link->device);

        config = link->config;
        device = link->device;

        if (link->action == SD_DEVICE_MOVE) {
                log_link_debug(link, "Skipping to apply Name= and NamePolicy= on '%s' uevent.",
                               device_action_to_string(link->action));
                goto no_rename;
        }

        if (IN_SET(link->name_assign_type, NET_NAME_USER, NET_NAME_RENAMED) &&
            !naming_scheme_has(NAMING_ALLOW_RERENAMES)) {
                log_link_debug(link, "Device already has a name given by userspace, not renaming.");
                goto no_rename;
        }

        if (enable_name_policy && config->name_policy)
                for (NamePolicy *policy = config->name_policy; *policy != _NAMEPOLICY_INVALID; policy++) {
                        const char *new_name = NULL;

                        switch (*policy) {
                        case NAMEPOLICY_KERNEL:
                                if (link->name_assign_type != NET_NAME_PREDICTABLE)
                                        continue;

                                /* The kernel claims to have given a predictable name, keep it. */
                                log_link_debug(link, "Policy *%s*: keeping predictable kernel name",
                                               name_policy_to_string(*policy));
                                goto no_rename;
                        case NAMEPOLICY_KEEP:
                                if (!IN_SET(link->name_assign_type, NET_NAME_USER, NET_NAME_RENAMED))
                                        continue;

                                log_link_debug(link, "Policy *%s*: keeping existing userspace name",
                                               name_policy_to_string(*policy));
                                goto no_rename;
                        case NAMEPOLICY_DATABASE:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_FROM_DATABASE", &new_name);
                                break;
                        case NAMEPOLICY_ONBOARD:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_ONBOARD", &new_name);
                                break;
                        case NAMEPOLICY_SLOT:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_SLOT", &new_name);
                                break;
                        case NAMEPOLICY_PATH:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_PATH", &new_name);
                                break;
                        case NAMEPOLICY_MAC:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_MAC", &new_name);
                                break;
                        default:
                                assert_not_reached();
                        }
                        if (ifname_valid(new_name)) {
                                log_link_debug(link, "Policy *%s* yields \"%s\".", name_policy_to_string(*policy), new_name);
                                link->new_name = new_name;
                                return 0;
                        }
                }

        if (link->config->name) {
                log_link_debug(link, "Policies didn't yield a name, using specified Name=%s.", link->config->name);
                link->new_name = link->config->name;
                return 0;
        }

        log_link_debug(link, "Policies didn't yield a name and Name= is not given, not renaming.");
no_rename:
        link->new_name = link->ifname;
        return 0;
}

static int link_apply_alternative_names(Link *link, sd_netlink **rtnl) {
        _cleanup_strv_free_ char **altnames = NULL, **current_altnames = NULL;
        LinkConfig *config;
        sd_device *device;
        int r;

        assert(link);
        assert(link->config);
        assert(link->device);
        assert(rtnl);

        config = link->config;
        device = link->device;

        if (config->alternative_names) {
                altnames = strv_copy(config->alternative_names);
                if (!altnames)
                        return log_oom();
        }

        if (config->alternative_names_policy)
                for (NamePolicy *p = config->alternative_names_policy; *p != _NAMEPOLICY_INVALID; p++) {
                        const char *n = NULL;

                        switch (*p) {
                        case NAMEPOLICY_DATABASE:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_FROM_DATABASE", &n);
                                break;
                        case NAMEPOLICY_ONBOARD:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_ONBOARD", &n);
                                break;
                        case NAMEPOLICY_SLOT:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_SLOT", &n);
                                break;
                        case NAMEPOLICY_PATH:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_PATH", &n);
                                break;
                        case NAMEPOLICY_MAC:
                                (void) sd_device_get_property_value(device, "ID_NET_NAME_MAC", &n);
                                break;
                        default:
                                assert_not_reached();
                        }
                        if (!isempty(n)) {
                                r = strv_extend(&altnames, n);
                                if (r < 0)
                                        return log_oom();
                        }
                }

        if (link->new_name)
                strv_remove(altnames, link->new_name);
        strv_remove(altnames, link->ifname);

        r = rtnl_get_link_alternative_names(rtnl, link->ifindex, &current_altnames);
        if (r < 0)
                log_link_debug_errno(link, r, "Failed to get alternative names, ignoring: %m");

        char **p;
        STRV_FOREACH(p, current_altnames)
                strv_remove(altnames, *p);

        strv_uniq(altnames);
        strv_sort(altnames);
        r = rtnl_set_link_alternative_names(rtnl, link->ifindex, altnames);
        if (r < 0)
                log_link_full_errno(link, r == -EOPNOTSUPP ? LOG_DEBUG : LOG_WARNING, r,
                                    "Could not set AlternativeName= or apply AlternativeNamesPolicy=, ignoring: %m");

        return 0;
}

static int sr_iov_configure(Link *link, sd_netlink **rtnl, SRIOV *sr_iov) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        int r;

        assert(link);
        assert(rtnl);
        assert(link->ifindex > 0);

        if (!*rtnl) {
                r = sd_netlink_open(rtnl);
                if (r < 0)
                        return r;
        }

        r = sd_rtnl_message_new_link(*rtnl, &req, RTM_SETLINK, link->ifindex);
        if (r < 0)
                return r;

        r = sr_iov_set_netlink_message(sr_iov, req);
        if (r < 0)
                return r;

        r = sd_netlink_call(*rtnl, req, 0, NULL);
        if (r < 0)
                return r;

        return 0;
}

static int link_apply_sr_iov_config(Link *link, sd_netlink **rtnl) {
        SRIOV *sr_iov;
        int r;

        assert(link);
        assert(link->config);
        assert(link->device);

        r = sr_iov_set_num_vfs(link->device, link->config->sr_iov_num_vfs, link->config->sr_iov_by_section);
        if (r < 0)
                log_link_warning_errno(link, r, "Failed to set the number of SR-IOV virtual functions, ignoring: %m");

        ORDERED_HASHMAP_FOREACH(sr_iov, link->config->sr_iov_by_section) {
                r = sr_iov_configure(link, rtnl, sr_iov);
                if (r < 0)
                        log_link_warning_errno(link, r,
                                               "Failed to configure SR-IOV virtual function %"PRIu32", ignoring: %m",
                                               sr_iov->vf);
        }

        return 0;
}

int link_apply_config(LinkConfigContext *ctx, sd_netlink **rtnl, Link *link) {
        int r;

        assert(ctx);
        assert(rtnl);
        assert(link);

        if (!IN_SET(link->action, SD_DEVICE_ADD, SD_DEVICE_BIND, SD_DEVICE_MOVE)) {
                log_link_debug(link, "Skipping to apply .link settings on '%s' uevent.",
                               device_action_to_string(link->action));

                link->new_name = link->ifname;
                return 0;
        }

        r = link_apply_ethtool_settings(link, &ctx->ethtool_fd);
        if (r < 0)
                return r;

        r = link_apply_rtnl_settings(link, rtnl);
        if (r < 0)
                return r;

        r = link_generate_new_name(link, ctx->enable_name_policy);
        if (r < 0)
                return r;

        r = link_apply_alternative_names(link, rtnl);
        if (r < 0)
                return r;

        r = link_apply_sr_iov_config(link, rtnl);
        if (r < 0)
                return r;

        return 0;
}

int config_parse_ifalias(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        char **s = data;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (!isempty(rvalue)) {
                *s = mfree(*s);
                return 0;
        }

        if (!ascii_is_valid(rvalue)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Interface alias is not ASCII clean, ignoring assignment: %s", rvalue);
                return 0;
        }

        if (strlen(rvalue) >= IFALIASZ) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Interface alias is too long, ignoring assignment: %s", rvalue);
                return 0;
        }

        return free_and_strdup_warn(s, rvalue);
}

int config_parse_rx_tx_queues(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint32_t k, *v = data;
        int r;

        if (isempty(rvalue)) {
                *v = 0;
                return 0;
        }

        r = safe_atou32(rvalue, &k);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Failed to parse %s=, ignoring assignment: %s.", lvalue, rvalue);
                return 0;
        }
        if (k == 0 || k > 4096) {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "Invalid %s=, ignoring assignment: %s.", lvalue, rvalue);
                return 0;
        }

        *v = k;
        return 0;
}

int config_parse_txqueuelen(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint32_t k, *v = data;
        int r;

        if (isempty(rvalue)) {
                *v = UINT32_MAX;
                return 0;
        }

        r = safe_atou32(rvalue, &k);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Failed to parse %s=, ignoring assignment: %s.", lvalue, rvalue);
                return 0;
        }
        if (k == UINT32_MAX) {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "Invalid %s=, ignoring assignment: %s.", lvalue, rvalue);
                return 0;
        }

        *v = k;
        return 0;
}

int config_parse_wol_password(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        LinkConfig *config = userdata;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(userdata);

        if (isempty(rvalue)) {
                config->wol_password = erase_and_free(config->wol_password);
                config->wol_password_file = mfree(config->wol_password_file);
                return 0;
        }

        if (path_is_absolute(rvalue) && path_is_safe(rvalue)) {
                config->wol_password = erase_and_free(config->wol_password);
                return free_and_strdup_warn(&config->wol_password_file, rvalue);
        }

        warn_file_is_world_accessible(filename, NULL, unit, line);

        r = link_parse_wol_password(config, rvalue);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring assignment: %s.", lvalue, rvalue);
                return 0;
        }

        config->wol_password_file = mfree(config->wol_password_file);
        return 0;
}

static const char* const mac_address_policy_table[_MAC_ADDRESS_POLICY_MAX] = {
        [MAC_ADDRESS_POLICY_PERSISTENT] = "persistent",
        [MAC_ADDRESS_POLICY_RANDOM] = "random",
        [MAC_ADDRESS_POLICY_NONE] = "none",
};

DEFINE_STRING_TABLE_LOOKUP(mac_address_policy, MACAddressPolicy);
DEFINE_CONFIG_PARSE_ENUM_WITH_DEFAULT(
        config_parse_mac_address_policy,
        mac_address_policy,
        MACAddressPolicy,
        MAC_ADDRESS_POLICY_NONE,
        "Failed to parse MAC address policy");

DEFINE_CONFIG_PARSE_ENUMV(config_parse_name_policy, name_policy, NamePolicy,
                          _NAMEPOLICY_INVALID,
                          "Failed to parse interface name policy");

DEFINE_CONFIG_PARSE_ENUMV(config_parse_alternative_names_policy, alternative_names_policy, NamePolicy,
                          _NAMEPOLICY_INVALID,
                          "Failed to parse alternative names policy");
