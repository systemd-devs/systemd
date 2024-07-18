/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/in.h>
#include <linux/if.h>
#include <linux/if_arp.h>

#include "af-list.h"
#include "fd-util.h"
#include "missing_network.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-network.h"
#include "networkd-sysctl.h"
#include "path-util.h"
#include "socket-util.h"
#include "string-table.h"
#include "sysctl-util.h"

#if HAVE_VMLINUX_H

#include "bpf/sysctl_monitor/sysctl-monitor-skel.h"
#include "bpf/sysctl_monitor/sysctl-write-event.h"

#define CGROUP_MOUNT_DFLT "/sys/fs/cgroup"

static int open_rootcg(void) {
        int fd = open(CGROUP_MOUNT_DFLT, O_PATH | O_DIRECTORY | O_CLOEXEC);

        if (fd < 0)
                log_error_errno(errno, "Failed to open cgroup mount point %s: %m", CGROUP_MOUNT_DFLT);

        return fd;
}

static int sysct_write_event(void *ctx, void *data, size_t data_sz) {
        struct sysctl_write_event *we = ASSERT_PTR(data);
        Hashmap **sysctl_shadow = ASSERT_PTR(ctx);
        _cleanup_free_ char *path = NULL;
        char *value;

        /* Ignore our own writes */
        if (we->pid == getpid())
                return 0;

        path = path_join("/proc/sys", we->name);
        if (!path)
                return log_oom();

        /* If we never managed this handle, ignore it. */
        value = hashmap_get(*sysctl_shadow, path);
        if (!value)
                return 0;

        if (!streq(value, we->newvalue))
                log_warning("'%s' changed sysctl '%s' from '%s' to '%s', we want '%s'", we->comm, we->name, we->current, we->newvalue, value);

        return 0;
}

static int on_ringbuf_io(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        struct ring_buffer *rb = ASSERT_PTR(userdata);
        int r;

        r = ring_buffer__poll(rb, 1000);
        if (r < 0 && errno != EINTR)
                log_error_errno(errno, "Error polling ring buffer: %m");

        return 0;
}

void sysctl_add_monitor(Manager *manager) {
        _cleanup_close_ int cgroup_fd = -EBADF;
        int prog_fd;
        int r;

        assert(manager);

        r = dlopen_bpf();
        if (r < 0) {
                log_warning_errno(r, "Failed to load BPF library: %m");
                return;
        }

        cgroup_fd = open_rootcg();
        if (cgroup_fd < 0)
                return;

        manager->sysctl_skel = sysctl_monitor_bpf__open_and_load();
        if (!manager->sysctl_skel) {
                log_warning_errno(errno, "Failed to load sysctl monitor BPF program: %m");
                return;
        }

        r = sysctl_monitor_bpf__attach(manager->sysctl_skel);
        if (r < 0) {
                log_warning_errno(r, "Failed to attach sysctl monitor BPF program: %m");
                goto out_skel_close;
        }

        manager->sysctl_buffer = ring_buffer__new(
                        bpf_map__fd(manager->sysctl_skel->maps.written_sysctls),
                        sysct_write_event, &manager->sysctl_shadow, NULL);
        if (!manager->sysctl_buffer) {
                log_warning_errno(errno, "Failed to create ring buffer: %m");
                goto out_skel_detach;
        }

        prog_fd = bpf_program__fd(manager->sysctl_skel->progs.sysctl_monitor);
        if (prog_fd < 0) {
                log_warning_errno(prog_fd, "Failed to get sysctl monitor BPF program FD: %m");
                goto out_rb;
        }

        if (bpf_prog_attach(prog_fd, cgroup_fd, BPF_CGROUP_SYSCTL, BPF_F_ALLOW_OVERRIDE) < 0) {
                log_warning_errno(prog_fd, "Failed to attach sysctl monitor BPF program to cgroup: %m");
                goto out_rb;
        }

        r = sd_event_add_io(manager->event, &manager->sysctl_event_source,
                        ring_buffer__epoll_fd(manager->sysctl_buffer), EPOLLIN, on_ringbuf_io, manager->sysctl_buffer);
        if (r < 0) {
                log_warning_errno(r, "Failed to watch sysctl event ringbuffer: %m");
                goto out_detach;
        }

        return;

out_detach:
        bpf_prog_detach(cgroup_fd, BPF_CGROUP_SYSCTL);

out_rb:
        ring_buffer__free(manager->sysctl_buffer);

out_skel_detach:
        sysctl_monitor_bpf__detach(manager->sysctl_skel);

out_skel_close:
        sysctl_monitor_bpf__destroy(manager->sysctl_skel);
}

void sysctl_remove_monitor(Manager *manager) {
        _cleanup_close_ int cgroup_fd = -EBADF;
        int r;

        assert(manager);

        sd_event_source_disable_unref(manager->sysctl_event_source);

        r = dlopen_bpf();
        if (r < 0) {
                log_warning_errno(r, "Failed to load BPF library: %m");
                return;
        }

        cgroup_fd = open_rootcg();
        if (cgroup_fd < 0)
                return;

        bpf_prog_detach(cgroup_fd, BPF_CGROUP_SYSCTL);

        ring_buffer__free(manager->sysctl_buffer);

        sysctl_monitor_bpf__destroy(manager->sysctl_skel);
}

void sysctl_clear_link_shadows(Link *link) {
        _cleanup_free_ char *ipv4 = NULL, *ipv6 = NULL;
        const char *key = NULL, *value = NULL;

        ipv4 = path_join("/proc/sys/net/ipv4/conf", link->ifname);
        if (!ipv4) {
                log_oom();
                return;
        }

        ipv6 = path_join("/proc/sys/net/ipv6/conf", link->ifname);
        if (!ipv6) {
                log_oom();
                return;
        }

        HASHMAP_FOREACH_KEY(value, key, link->manager->sysctl_shadow) {
                if (startswith(key, ipv4) || startswith(key, ipv6)) {
                        hashmap_remove(link->manager->sysctl_shadow, key);
                        free((void*)key);
                        free((void*)value);
                }
        }
}
#endif

static void manager_set_ip_forwarding(Manager *manager, int family) {
        int r, t;

        assert(manager);
        assert(IN_SET(family, AF_INET, AF_INET6));

        if (family == AF_INET6 && !socket_ipv6_is_supported())
                return;

        t = manager->ip_forwarding[family == AF_INET6];
        if (t < 0)
                return; /* keep */

        /* First, set the default value. */
        r = sysctl_write_ip_property_boolean(family, "default", "forwarding", t, &manager->sysctl_shadow);
        if (r < 0)
                log_warning_errno(r, "Failed to %s the default %s forwarding: %m",
                                  enable_disable(t), af_to_ipv4_ipv6(family));

        /* Then, set the value to all interfaces. */
        r = sysctl_write_ip_property_boolean(family, "all", "forwarding", t, &manager->sysctl_shadow);
        if (r < 0)
                log_warning_errno(r, "Failed to %s %s forwarding for all interfaces: %m",
                                  enable_disable(t), af_to_ipv4_ipv6(family));
}

void manager_set_sysctl(Manager *manager) {
        assert(manager);
        assert(!manager->test_mode);

        manager_set_ip_forwarding(manager, AF_INET);
        manager_set_ip_forwarding(manager, AF_INET6);
}

static bool link_is_configured_for_family(Link *link, int family) {
        assert(link);

        if (!link->network)
                return false;

        if (link->flags & IFF_LOOPBACK)
                return false;

        /* CAN devices do not support IP layer. Most of the functions below are never called for CAN devices,
         * but link_set_ipv6_mtu() may be called after setting interface MTU, and warn about the failure. For
         * safety, let's unconditionally check if the interface is not a CAN device. */
        if (IN_SET(family, AF_INET, AF_INET6) && link->iftype == ARPHRD_CAN)
                return false;

        if (family == AF_INET6 && !socket_ipv6_is_supported())
                return false;

        return true;
}

static int link_update_ipv6_sysctl(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        if (!link_ipv6_enabled(link))
                return 0;

        return sysctl_write_ip_property_boolean(AF_INET6, link->ifname, "disable_ipv6", false, &link->manager->sysctl_shadow);
}

static int link_set_proxy_arp(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET))
                return 0;

        if (link->network->proxy_arp < 0)
                return 0;

        return sysctl_write_ip_property_boolean(AF_INET, link->ifname, "proxy_arp", link->network->proxy_arp > 0, &link->manager->sysctl_shadow);
}

static int link_set_proxy_arp_pvlan(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET))
                return 0;

        if (link->network->proxy_arp_pvlan < 0)
                return 0;

        return sysctl_write_ip_property_boolean(AF_INET, link->ifname, "proxy_arp_pvlan", link->network->proxy_arp_pvlan > 0, &link->manager->sysctl_shadow);
}

int link_get_ip_forwarding(Link *link, int family) {
        assert(link);
        assert(link->manager);
        assert(link->network);
        assert(IN_SET(family, AF_INET, AF_INET6));

        /* If it is explicitly specified, then honor the setting. */
        int t = link->network->ip_forwarding[family == AF_INET6];
        if (t >= 0)
                return t;

        /* If IPMasquerade= is enabled, also enable IP forwarding. */
        if (family == AF_INET && FLAGS_SET(link->network->ip_masquerade, ADDRESS_FAMILY_IPV4))
                return true;
        if (family == AF_INET6 && FLAGS_SET(link->network->ip_masquerade, ADDRESS_FAMILY_IPV6))
                return true;

        /* If IPv6SendRA= is enabled, also enable IPv6 forwarding. */
        if (family == AF_INET6 && link_radv_enabled(link))
                return true;

        /* Otherwise, use the global setting. */
        return link->manager->ip_forwarding[family == AF_INET6];
}

static int link_set_ip_forwarding(Link *link, int family) {
        int r, t;

        assert(link);
        assert(IN_SET(family, AF_INET, AF_INET6));

        if (!link_is_configured_for_family(link, family))
                return 0;

        t = link_get_ip_forwarding(link, family);
        if (t < 0)
                return 0; /* keep */

        r = sysctl_write_ip_property_boolean(family, link->ifname, "forwarding", t, &link->manager->sysctl_shadow);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to %s %s forwarding, ignoring: %m",
                                              enable_disable(t), af_to_ipv4_ipv6(family));

        return 0;
}

static int link_set_ipv4_rp_filter(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET))
                return 0;

        if (link->network->ipv4_rp_filter < 0)
                return 0;

        return sysctl_write_ip_property_int(AF_INET, link->ifname, "rp_filter", link->network->ipv4_rp_filter, &link->manager->sysctl_shadow);
}

static int link_set_ipv6_privacy_extensions(Link *link) {
        IPv6PrivacyExtensions val;

        assert(link);
        assert(link->manager);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        val = link->network->ipv6_privacy_extensions;
        if (val < 0) /* If not specified, then use the global setting. */
                val = link->manager->ipv6_privacy_extensions;

        /* When "kernel", do not update the setting. */
        if (val == IPV6_PRIVACY_EXTENSIONS_KERNEL)
                return 0;

        return sysctl_write_ip_property_int(AF_INET6, link->ifname, "use_tempaddr", (int) val, &link->manager->sysctl_shadow);
}

static int link_set_ipv6_accept_ra(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        return sysctl_write_ip_property(AF_INET6, link->ifname, "accept_ra", "0", &link->manager->sysctl_shadow);
}

static int link_set_ipv6_dad_transmits(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        if (link->network->ipv6_dad_transmits < 0)
                return 0;

        return sysctl_write_ip_property_int(AF_INET6, link->ifname, "dad_transmits", link->network->ipv6_dad_transmits, &link->manager->sysctl_shadow);
}

static int link_set_ipv6_hop_limit(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        if (link->network->ipv6_hop_limit <= 0)
                return 0;

        return sysctl_write_ip_property_int(AF_INET6, link->ifname, "hop_limit", link->network->ipv6_hop_limit, &link->manager->sysctl_shadow);
}

static int link_set_ipv6_retransmission_time(Link *link) {
        usec_t retrans_time_ms;

        assert(link);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        if (!timestamp_is_set(link->network->ipv6_retransmission_time))
                return 0;

        retrans_time_ms = DIV_ROUND_UP(link->network->ipv6_retransmission_time, USEC_PER_MSEC);
         if (retrans_time_ms <= 0 || retrans_time_ms > UINT32_MAX)
                return 0;

        return sysctl_write_ip_neighbor_property_uint32(AF_INET6, link->ifname, "retrans_time_ms", retrans_time_ms, &link->manager->sysctl_shadow);
}

static int link_set_ipv6_proxy_ndp(Link *link) {
        bool v;

        assert(link);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        if (link->network->ipv6_proxy_ndp >= 0)
                v = link->network->ipv6_proxy_ndp;
        else
                v = !set_isempty(link->network->ipv6_proxy_ndp_addresses);

        return sysctl_write_ip_property_boolean(AF_INET6, link->ifname, "proxy_ndp", v, &link->manager->sysctl_shadow);
}

int link_set_ipv6_mtu(Link *link, int log_level) {
        uint32_t mtu = 0;

        assert(link);

        if (!link_is_configured_for_family(link, AF_INET6))
                return 0;

        assert(link->network);

        if (link->network->ndisc_use_mtu)
                mtu = link->ndisc_mtu;
        if (mtu == 0)
                mtu = link->network->ipv6_mtu;
        if (mtu == 0)
                return 0;

        if (mtu > link->mtu) {
                log_link_full(link, log_level,
                              "Reducing requested IPv6 MTU %"PRIu32" to the interface's maximum MTU %"PRIu32".",
                              mtu, link->mtu);
                mtu = link->mtu;
        }

        return sysctl_write_ip_property_uint32(AF_INET6, link->ifname, "mtu", mtu, &link->manager->sysctl_shadow);
}

static int link_set_ipv4_accept_local(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET))
                return 0;

        if (link->network->ipv4_accept_local < 0)
                return 0;

        return sysctl_write_ip_property_boolean(AF_INET, link->ifname, "accept_local", link->network->ipv4_accept_local > 0, &link->manager->sysctl_shadow);
}

static int link_set_ipv4_route_localnet(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET))
                return 0;

        if (link->network->ipv4_route_localnet < 0)
                return 0;

        return sysctl_write_ip_property_boolean(AF_INET, link->ifname, "route_localnet", link->network->ipv4_route_localnet > 0, &link->manager->sysctl_shadow);
}

static int link_set_ipv4_promote_secondaries(Link *link) {
        assert(link);

        if (!link_is_configured_for_family(link, AF_INET))
                return 0;

        /* If promote_secondaries is not set, DHCP will work only as long as the IP address does not
         * changes between leases. The kernel will remove all secondary IP addresses of an interface
         * otherwise. The way systemd-networkd works is that the new IP of a lease is added as a
         * secondary IP and when the primary one expires it relies on the kernel to promote the
         * secondary IP. See also https://github.com/systemd/systemd/issues/7163 */
        return sysctl_write_ip_property_boolean(AF_INET, link->ifname, "promote_secondaries", true, &link->manager->sysctl_shadow);
}

int link_set_sysctl(Link *link) {
        int r;

        assert(link);

        /* If IPv6 configured that is static IPv6 address and IPv6LL autoconfiguration is enabled
         * for this interface, then enable IPv6 */
        r = link_update_ipv6_sysctl(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot enable IPv6, ignoring: %m");

        r = link_set_proxy_arp(link);
        if (r < 0)
               log_link_warning_errno(link, r, "Cannot configure proxy ARP for interface, ignoring: %m");

        r = link_set_proxy_arp_pvlan(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot configure proxy ARP private VLAN for interface, ignoring: %m");

        (void) link_set_ip_forwarding(link, AF_INET);
        (void) link_set_ip_forwarding(link, AF_INET6);

        r = link_set_ipv6_privacy_extensions(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot configure IPv6 privacy extensions for interface, ignoring: %m");

        r = link_set_ipv6_accept_ra(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot disable kernel IPv6 accept_ra for interface, ignoring: %m");

        r = link_set_ipv6_dad_transmits(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv6 dad transmits for interface, ignoring: %m");

        r = link_set_ipv6_hop_limit(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv6 hop limit for interface, ignoring: %m");

        r = link_set_ipv6_retransmission_time(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv6 retransmission time for interface, ignoring: %m");

        r = link_set_ipv6_proxy_ndp(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv6 proxy NDP, ignoring: %m");

        r = link_set_ipv6_mtu(link, LOG_INFO);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv6 MTU, ignoring: %m");

        r = link_set_ipv6ll_stable_secret(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set stable secret address for IPv6 link-local address: %m");

        r = link_set_ipv4_accept_local(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv4 accept_local flag for interface, ignoring: %m");

        r = link_set_ipv4_route_localnet(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv4 route_localnet flag for interface, ignoring: %m");

        r = link_set_ipv4_rp_filter(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot set IPv4 reverse path filtering for interface, ignoring: %m");

        r = link_set_ipv4_promote_secondaries(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Cannot enable promote_secondaries for interface, ignoring: %m");

        return 0;
}

static const char* const ipv6_privacy_extensions_table[_IPV6_PRIVACY_EXTENSIONS_MAX] = {
        [IPV6_PRIVACY_EXTENSIONS_NO]            = "no",
        [IPV6_PRIVACY_EXTENSIONS_PREFER_PUBLIC] = "prefer-public",
        [IPV6_PRIVACY_EXTENSIONS_YES]           = "yes",
        [IPV6_PRIVACY_EXTENSIONS_KERNEL]        = "kernel",
};

DEFINE_STRING_TABLE_LOOKUP_WITH_BOOLEAN(ipv6_privacy_extensions, IPv6PrivacyExtensions,
                                        IPV6_PRIVACY_EXTENSIONS_YES);
DEFINE_CONFIG_PARSE_ENUM(config_parse_ipv6_privacy_extensions, ipv6_privacy_extensions, IPv6PrivacyExtensions,
                         "Failed to parse IPv6 privacy extensions option");

static const char* const ip_reverse_path_filter_table[_IP_REVERSE_PATH_FILTER_MAX] = {
        [IP_REVERSE_PATH_FILTER_NO]     = "no",
        [IP_REVERSE_PATH_FILTER_STRICT] = "strict",
        [IP_REVERSE_PATH_FILTER_LOOSE]  = "loose",
};

DEFINE_STRING_TABLE_LOOKUP(ip_reverse_path_filter, IPReversePathFilter);
DEFINE_CONFIG_PARSE_ENUM(config_parse_ip_reverse_path_filter, ip_reverse_path_filter, IPReversePathFilter,
                         "Failed to parse IP reverse path filter option");

int config_parse_ip_forward_deprecated(
                const char* unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        assert(filename);

        log_syntax(unit, LOG_WARNING, filename, line, 0,
                   "IPForward= setting is deprecated. "
                   "Please use IPv4Forwarding= and/or IPv6Forwarding= in networkd.conf for global setting, "
                   "and the same settings in .network files for per-interface setting.");
        return 0;
}
