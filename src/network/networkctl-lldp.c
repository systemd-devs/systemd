/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "fd-util.h"
#include "json-util.h"
#include "networkctl.h"
#include "networkctl-dump-util.h"
#include "networkctl-link-info.h"
#include "networkctl-lldp.h"
#include "networkctl-util.h"
#include "sd-lldp-rx.h"
#include "sparse-endian.h"
#include "stdio-util.h"
#include "strv.h"
#include "terminal-util.h"
#include "varlink-util.h"

typedef struct InterfaceInfo {
        int ifindex;
        const char *ifname;
        char **altnames;
        sd_json_variant *v;
} InterfaceInfo;

static void interface_info_done(InterfaceInfo *p) {
        if (!p)
                return;

        strv_free(p->altnames);
        sd_json_variant_unref(p->v);
}

static const sd_json_dispatch_field interface_info_dispatch_table[] = {
        { "InterfaceIndex",            _SD_JSON_VARIANT_TYPE_INVALID, json_dispatch_ifindex,         offsetof(InterfaceInfo, ifindex),  SD_JSON_MANDATORY|SD_JSON_RELAX },
        { "InterfaceName",             SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(InterfaceInfo, ifname),   SD_JSON_MANDATORY               },
        { "InterfaceAlternativeNames", SD_JSON_VARIANT_ARRAY,         sd_json_dispatch_strv,         offsetof(InterfaceInfo, altnames), 0                               },
        { "Neighbors",                 SD_JSON_VARIANT_ARRAY,         sd_json_dispatch_variant,      offsetof(InterfaceInfo, v),        0                               },
        {},
};

typedef struct LLDPNeighborInfo {
        const char *chassis_id;
        const char *port_id;
        const char *port_description;
        const char *system_name;
        const char *system_description;
        uint16_t capabilities;
} LLDPNeighborInfo;

static const sd_json_dispatch_field lldp_neighbor_dispatch_table[] = {
        { "ChassisID",           SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(LLDPNeighborInfo, chassis_id),         0 },
        { "PortID",              SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(LLDPNeighborInfo, port_id),            0 },
        { "PortDescription",     SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(LLDPNeighborInfo, port_description),   0 },
        { "SystemName",          SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(LLDPNeighborInfo, system_name),        0 },
        { "SystemDescription",   SD_JSON_VARIANT_STRING,        sd_json_dispatch_const_string, offsetof(LLDPNeighborInfo, system_description), 0 },
        { "EnabledCapabilities", _SD_JSON_VARIANT_TYPE_INVALID, sd_json_dispatch_uint16,       offsetof(LLDPNeighborInfo, capabilities),       0 },
        {},
};

int dump_lldp_neighbors(sd_varlink *vl, Table *table, int ifindex) {
        _cleanup_strv_free_ char **buf = NULL;
        sd_json_variant *reply;
        int r;

        assert(vl);
        assert(table);
        assert(ifindex > 0);

        r = varlink_callbo_and_log(
                        vl,
                        "io.systemd.Network.GetLLDPNeighbors",
                        &reply,
                        SD_JSON_BUILD_PAIR_INTEGER("InterfaceIndex", ifindex));
        if (r < 0)
                return r;

        sd_json_variant *i;
        JSON_VARIANT_ARRAY_FOREACH(i, sd_json_variant_by_key(reply, "Neighbors")) {
                _cleanup_(interface_info_done) InterfaceInfo info = {};

                r = sd_json_dispatch(i, interface_info_dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &info);
                if (r < 0)
                        return r;

                if (info.ifindex != ifindex)
                        continue;

                sd_json_variant *neighbor;
                JSON_VARIANT_ARRAY_FOREACH(neighbor, info.v) {
                        LLDPNeighborInfo neighbor_info = {};

                        r = sd_json_dispatch(neighbor, lldp_neighbor_dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &neighbor_info);
                        if (r < 0)
                                return r;

                        r = strv_extendf(&buf, "%s%s%s%s on port %s%s%s%s",
                                         strna(neighbor_info.system_name),
                                         isempty(neighbor_info.system_description) ? "" : " (",
                                         strempty(neighbor_info.system_description),
                                         isempty(neighbor_info.system_description) ? "" : ")",
                                         strna(neighbor_info.port_id),
                                         isempty(neighbor_info.port_description) ? "" : " (",
                                         strempty(neighbor_info.port_description),
                                         isempty(neighbor_info.port_description) ? "" : ")");
                        if (r < 0)
                                return log_oom();
                }
        }

        return dump_list(table, "Connected To", buf);
}

static char* lldp_capabilities_to_string(uint64_t x) {
        static const char characters[] = {
                'o', 'p', 'b', 'w', 'r', 't', 'd', 'a', 'c', 's', 'm',
        };
        char *ret;
        unsigned i;

        ret = new(char, ELEMENTSOF(characters) + 1);
        if (!ret)
                return NULL;

        for (i = 0; i < ELEMENTSOF(characters); i++)
                ret[i] = (x & (1U << i)) ? characters[i] : '.';

        ret[i] = 0;
        return ret;
}

static void lldp_capabilities_legend(uint16_t x) {
        unsigned cols = columns();
        static const char* const table[] = {
                "o - Other",
                "p - Repeater",
                "b - Bridge",
                "w - WLAN Access Point",
                "r - Router",
                "t - Telephone",
                "d - DOCSIS cable device",
                "a - Station",
                "c - Customer VLAN",
                "s - Service VLAN",
                "m - Two-port MAC Relay (TPMR)",
        };

        if (x == 0)
                return;

        printf("\nCapability Flags:\n");
        for (unsigned w = 0, i = 0; i < ELEMENTSOF(table); i++)
                if (x & (1U << i) || arg_all) {
                        bool newline;

                        newline = w + strlen(table[i]) + (w == 0 ? 0 : 2) > cols;
                        if (newline)
                                w = 0;
                        w += printf("%s%s%s", newline ? "\n" : "", w == 0 ? "" : "; ", table[i]);
                }
        puts("");
}

static bool interface_match_pattern(const InterfaceInfo *info, char * const *patterns) {
        assert(info);

        if (strv_isempty(patterns))
                return true;

        if (strv_fnmatch(patterns, info->ifname))
                return true;

        char str[DECIMAL_STR_MAX(int)];
        xsprintf(str, "%i", info->ifindex);
        if (strv_fnmatch(patterns, str))
                return true;

        STRV_FOREACH(a, info->altnames)
                if (strv_fnmatch(patterns, *a))
                        return true;

        return false;
}

static int dump_lldp_neighbors_json(sd_json_variant *reply, char * const *patterns) {
        _cleanup_(sd_json_variant_unrefp) sd_json_variant *array = NULL, *v = NULL;
        int r;

        assert(reply);

        if (strv_isempty(patterns))
                return sd_json_variant_dump(reply, arg_json_format_flags, NULL, NULL);

        /* Filter and dump the result. */

        sd_json_variant *i;
        JSON_VARIANT_ARRAY_FOREACH(i, sd_json_variant_by_key(reply, "Neighbors")) {
                _cleanup_(interface_info_done) InterfaceInfo info = {};

                r = sd_json_dispatch(i, interface_info_dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &info);
                if (r < 0)
                        return r;

                if (!interface_match_pattern(&info, patterns))
                        continue;

                r = sd_json_variant_append_array(&array, i);
                if (r < 0)
                        return log_error_errno(r, "Failed to append json variant to array: %m");
        }

        r = sd_json_buildo(
                        &v,
                        SD_JSON_BUILD_PAIR_CONDITION(sd_json_variant_is_blank_array(array), "Neighbors", SD_JSON_BUILD_EMPTY_ARRAY),
                        SD_JSON_BUILD_PAIR_CONDITION(!sd_json_variant_is_blank_array(array), "Neighbors", SD_JSON_BUILD_VARIANT(array)));
        if (r < 0)
                return log_error_errno(r, "Failed to build json varinat: %m");

        return sd_json_variant_dump(v, arg_json_format_flags, NULL, NULL);
}

static int open_lldp_neighbors_legacy(int ifindex, FILE **ret) {
        _cleanup_fclose_ FILE *f = NULL;
        char p[STRLEN("/run/systemd/netif/lldp/") + DECIMAL_STR_MAX(int)];

        assert(ifindex >= 0);
        assert(ret);

        xsprintf(p, "/run/systemd/netif/lldp/%i", ifindex);

        f = fopen(p, "re");
        if (!f)
                return -errno;

        *ret = TAKE_PTR(f);
        return 0;
}

static int next_lldp_neighbor_legacy(FILE *f, sd_lldp_neighbor **ret) {
        _cleanup_free_ void *raw = NULL;
        size_t l;
        le64_t u;
        int r;

        assert(f);
        assert(ret);

        l = fread(&u, 1, sizeof(u), f);
        if (l == 0 && feof(f))
                return 0;
        if (l != sizeof(u))
                return -EBADMSG;

        /* each LLDP packet is at most MTU size, but let's allow up to 4KiB just in case */
        if (le64toh(u) >= 4096)
                return -EBADMSG;

        raw = new(uint8_t, le64toh(u));
        if (!raw)
                return -ENOMEM;

        if (fread(raw, 1, le64toh(u), f) != le64toh(u))
                return -EBADMSG;

        r = sd_lldp_neighbor_from_raw(ret, raw, le64toh(u));
        if (r < 0)
                return r;

        return 1;
}

int dump_lldp_neighbors_legacy(Table *table, const char *prefix, int ifindex) {
        _cleanup_strv_free_ char **buf = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(table);
        assert(prefix);
        assert(ifindex > 0);

        r = open_lldp_neighbors_legacy(ifindex, &f);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;

        for (;;) {
                const char *system_name = NULL, *port_id = NULL, *port_description = NULL;
                _cleanup_(sd_lldp_neighbor_unrefp) sd_lldp_neighbor *n = NULL;

                r = next_lldp_neighbor_legacy(f, &n);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                (void) sd_lldp_neighbor_get_system_name(n, &system_name);
                (void) sd_lldp_neighbor_get_port_id_as_string(n, &port_id);
                (void) sd_lldp_neighbor_get_port_description(n, &port_description);

                r = strv_extendf(&buf, "%s on port %s%s%s%s",
                                 strna(system_name),
                                 strna(port_id),
                                 isempty(port_description) ? "" : " (",
                                 strempty(port_description),
                                 isempty(port_description) ? "" : ")");
                if (r < 0)
                        return log_oom();
        }

        return dump_list(table, prefix, buf);
}
static int link_lldp_status_legacy(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        _cleanup_(link_info_array_freep) LinkInfo *links = NULL;
        _cleanup_(table_unrefp) Table *table = NULL;
        int r, c, m = 0;
        uint16_t all = 0;
        TableCell *cell;

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        c = acquire_link_info(NULL, rtnl, argc > 1 ? argv + 1 : NULL, &links);
        if (c < 0)
                return c;

        pager_open(arg_pager_flags);

        table = table_new("link",
                          "chassis-id",
                          "system-name",
                          "caps",
                          "port-id",
                          "port-description");
        if (!table)
                return log_oom();

        if (arg_full)
                table_set_width(table, 0);

        table_set_header(table, arg_legend);

        assert_se(cell = table_get_cell(table, 0, 3));
        table_set_minimum_width(table, cell, 11);
        table_set_ersatz_string(table, TABLE_ERSATZ_DASH);

        FOREACH_ARRAY(link, links, c) {
                _cleanup_fclose_ FILE *f = NULL;

                r = open_lldp_neighbors_legacy(link->ifindex, &f);
                if (r == -ENOENT)
                        continue;
                if (r < 0) {
                        log_warning_errno(r, "Failed to open LLDP data for %i, ignoring: %m", link->ifindex);
                        continue;
                }

                for (;;) {
                        const char *chassis_id = NULL, *port_id = NULL, *system_name = NULL, *port_description = NULL;
                        _cleanup_(sd_lldp_neighbor_unrefp) sd_lldp_neighbor *n = NULL;
                        _cleanup_free_ char *capabilities = NULL;
                        uint16_t cc;

                        r = next_lldp_neighbor_legacy(f, &n);
                        if (r < 0) {
                                log_warning_errno(r, "Failed to read neighbor data: %m");
                                break;
                        }
                        if (r == 0)
                                break;

                        (void) sd_lldp_neighbor_get_chassis_id_as_string(n, &chassis_id);
                        (void) sd_lldp_neighbor_get_port_id_as_string(n, &port_id);
                        (void) sd_lldp_neighbor_get_system_name(n, &system_name);
                        (void) sd_lldp_neighbor_get_port_description(n, &port_description);

                        if (sd_lldp_neighbor_get_enabled_capabilities(n, &cc) >= 0) {
                                capabilities = lldp_capabilities_to_string(cc);
                                all |= cc;
                        }

                        r = table_add_many(table,
                                           TABLE_STRING, link->name,
                                           TABLE_STRING, chassis_id,
                                           TABLE_STRING, system_name,
                                           TABLE_STRING, capabilities,
                                           TABLE_STRING, port_id,
                                           TABLE_STRING, port_description);
                        if (r < 0)
                                return table_log_add_error(r);

                        m++;
                }
        }

        r = table_print(table, NULL);
        if (r < 0)
                return table_log_print_error(r);

        if (arg_legend) {
                lldp_capabilities_legend(all);
                printf("\n%i neighbors listed.\n", m);
        }

        return 0;
}

int link_lldp_status(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_varlink_flush_close_unrefp) sd_varlink *vl = NULL;
        _cleanup_(table_unrefp) Table *table = NULL;
        sd_json_variant *reply;
        uint64_t all = 0;
        TableCell *cell;
        size_t m = 0;
        int r;

        r = varlink_connect_networkd(&vl);
        if (r < 0) {
                log_warning("Varlink connection failed, fallback to D-Bus.");
                return link_lldp_status_legacy(argc, argv, userdata);
        }

        r = varlink_call_and_log(vl, "io.systemd.Network.GetLLDPNeighbors", NULL, &reply);
        if (r < 0)
                return r;

        if (sd_json_format_enabled(arg_json_format_flags))
                return dump_lldp_neighbors_json(reply, strv_skip(argv, 1));

        pager_open(arg_pager_flags);

        table = table_new("index",
                          "link",
                          "chassis-id",
                          "system-name",
                          "caps",
                          "port-id",
                          "port-description");
        if (!table)
                return log_oom();

        if (arg_full)
                table_set_width(table, 0);

        table_set_header(table, arg_legend);
        table_set_ersatz_string(table, TABLE_ERSATZ_DASH);
        table_set_sort(table, (size_t) 0, (size_t) 2);
        table_hide_column_from_display(table, (size_t) 0);

        /* Make the capabilities not truncated */
        assert_se(cell = table_get_cell(table, 0, 4));
        table_set_minimum_width(table, cell, 11);

        sd_json_variant *i;
        JSON_VARIANT_ARRAY_FOREACH(i, sd_json_variant_by_key(reply, "Neighbors")) {
                _cleanup_(interface_info_done) InterfaceInfo info = {};

                r = sd_json_dispatch(i, interface_info_dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &info);
                if (r < 0)
                        return r;

                if (!interface_match_pattern(&info, strv_skip(argv, 1)))
                        continue;

                sd_json_variant *neighbor;
                JSON_VARIANT_ARRAY_FOREACH(neighbor, info.v) {
                        LLDPNeighborInfo neighbor_info = {};

                        r = sd_json_dispatch(neighbor, lldp_neighbor_dispatch_table, SD_JSON_LOG|SD_JSON_ALLOW_EXTENSIONS, &neighbor_info);
                        if (r < 0)
                                return r;

                        all |= neighbor_info.capabilities;

                        _cleanup_free_ char *cap_str = lldp_capabilities_to_string(neighbor_info.capabilities);

                        r = table_add_many(table,
                                           TABLE_INT,    info.ifindex,
                                           TABLE_STRING, info.ifname,
                                           TABLE_STRING, neighbor_info.chassis_id,
                                           TABLE_STRING, neighbor_info.system_name,
                                           TABLE_STRING, cap_str,
                                           TABLE_STRING, neighbor_info.port_id,
                                           TABLE_STRING, neighbor_info.port_description);
                        if (r < 0)
                                return table_log_add_error(r);

                        m++;
                }
        }

        r = table_print(table, NULL);
        if (r < 0)
                return table_log_print_error(r);

        if (arg_legend) {
                lldp_capabilities_legend(all);
                printf("\n%zu neighbor(s) listed.\n", m);
        }

        return 0;
}
