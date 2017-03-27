/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "alloc-util.h"
#include "conf-parser.h"
#include "in-addr-util.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "networkd-route.h"
#include "parse-util.h"
#include "set.h"
#include "string-util.h"
#include "sysctl-util.h"
#include "util.h"

#define ROUTES_DEFAULT_MAX_PER_FAMILY 4096U

static unsigned routes_max(void) {
        static thread_local unsigned cached = 0;

        _cleanup_free_ char *s4 = NULL, *s6 = NULL;
        unsigned val4 = ROUTES_DEFAULT_MAX_PER_FAMILY, val6 = ROUTES_DEFAULT_MAX_PER_FAMILY;

        if (cached > 0)
                return cached;

        if (sysctl_read("net/ipv4/route/max_size", &s4) >= 0) {
                truncate_nl(s4);
                if (safe_atou(s4, &val4) >= 0 &&
                    val4 == 2147483647U)
                        /* This is the default "no limit" value in the kernel */
                        val4 = ROUTES_DEFAULT_MAX_PER_FAMILY;
        }

        if (sysctl_read("net/ipv6/route/max_size", &s6) >= 0) {
                truncate_nl(s6);
                (void) safe_atou(s6, &val6);
        }

        cached = MAX(ROUTES_DEFAULT_MAX_PER_FAMILY, val4) +
                 MAX(ROUTES_DEFAULT_MAX_PER_FAMILY, val6);
        return cached;
}

int route_new(Route **ret) {
        _cleanup_route_free_ Route *route = NULL;

        route = new0(Route, 1);
        if (!route)
                return -ENOMEM;

        route->family = AF_UNSPEC;
        route->scope = RT_SCOPE_UNIVERSE;
        route->protocol = RTPROT_UNSPEC;
        route->table = RT_TABLE_MAIN;
        route->lifetime = USEC_INFINITY;
        route->on_dev = true;

        *ret = route;
        route = NULL;

        return 0;
}

int route_new_static(Network *network, const char *filename, unsigned section_line, Route **ret) {
        _cleanup_network_config_section_free_ NetworkConfigSection *n = NULL;
        _cleanup_route_free_ Route *route = NULL;
        int r;

        assert(network);
        assert(ret);
        assert(!!filename == (section_line > 0));

        if (filename) {
                r = network_config_section_new(filename, section_line, &n);
                if (r < 0)
                        return r;

                route = hashmap_get(network->routes_by_section, n);
                if (route) {
                        *ret = route;
                        route = NULL;

                        return 0;
                }
        }

        if (network->n_static_routes >= routes_max())
                return -E2BIG;

        r = route_new(&route);
        if (r < 0)
                return r;

        route->protocol = RTPROT_STATIC;

        if (filename) {
                route->section = n;
                n = NULL;

                r = hashmap_put(network->routes_by_section, route->section, route);
                if (r < 0)
                        return r;
        }

        route->network = network;
        LIST_PREPEND(routes, network->static_routes, route);
        network->n_static_routes++;

        *ret = route;
        route = NULL;

        return 0;
}

void route_free(Route *route) {
        if (!route)
                return;

        if (route->network) {
                LIST_REMOVE(routes, route->network->static_routes, route);

                assert(route->network->n_static_routes > 0);
                route->network->n_static_routes--;

                if (route->section)
                        hashmap_remove(route->network->routes_by_section, route->section);
        }

        network_config_section_free(route->section);

        if (route->link) {
                set_remove(route->link->routes, route);
                set_remove(route->link->routes_foreign, route);
        }

        sd_event_source_unref(route->expire);

        free(route);
}

static void route_hash_func(const void *b, struct siphash *state) {
        const Route *route = b;

        assert(route);

        siphash24_compress(&route->family, sizeof(route->family), state);

        switch (route->family) {
        case AF_INET:
        case AF_INET6:
                /* Equality of routes are given by the 4-touple
                   (dst_prefix,dst_prefixlen,tos,priority,table) */
                siphash24_compress(&route->dst, FAMILY_ADDRESS_SIZE(route->family), state);
                siphash24_compress(&route->dst_prefixlen, sizeof(route->dst_prefixlen), state);
                siphash24_compress(&route->tos, sizeof(route->tos), state);
                siphash24_compress(&route->priority, sizeof(route->priority), state);
                siphash24_compress(&route->table, sizeof(route->table), state);

                break;
        default:
                /* treat any other address family as AF_UNSPEC */
                break;
        }
}

static int route_compare_func(const void *_a, const void *_b) {
        const Route *a = _a, *b = _b;

        if (a->family < b->family)
                return -1;
        if (a->family > b->family)
                return 1;

        switch (a->family) {
        case AF_INET:
        case AF_INET6:
                if (a->dst_prefixlen < b->dst_prefixlen)
                        return -1;
                if (a->dst_prefixlen > b->dst_prefixlen)
                        return 1;

                if (a->tos < b->tos)
                        return -1;
                if (a->tos > b->tos)
                        return 1;

                if (a->priority < b->priority)
                        return -1;
                if (a->priority > b->priority)
                        return 1;

                if (a->table < b->table)
                        return -1;
                if (a->table > b->table)
                        return 1;

                return memcmp(&a->dst, &b->dst, FAMILY_ADDRESS_SIZE(a->family));
        default:
                /* treat any other address family as AF_UNSPEC */
                return 0;
        }
}

static const struct hash_ops route_hash_ops = {
        .hash = route_hash_func,
        .compare = route_compare_func
};

int route_get(Link *link,
              int family,
              const union in_addr_union *dst,
              unsigned char dst_prefixlen,
              unsigned char tos,
              uint32_t priority,
              unsigned char table,
              Route **ret) {

        Route route, *existing;

        assert(link);
        assert(dst);

        route = (Route) {
                .family = family,
                .dst = *dst,
                .dst_prefixlen = dst_prefixlen,
                .tos = tos,
                .priority = priority,
                .table = table,
        };

        existing = set_get(link->routes, &route);
        if (existing) {
                if (ret)
                        *ret = existing;
                return 1;
        }

        existing = set_get(link->routes_foreign, &route);
        if (existing) {
                if (ret)
                        *ret = existing;
                return 0;
        }

        return -ENOENT;
}

static int route_add_internal(
                Link *link,
                Set **routes,
                int family,
                const union in_addr_union *dst,
                unsigned char dst_prefixlen,
                unsigned char tos,
                uint32_t priority,
                unsigned char table,
                Route **ret) {

        _cleanup_route_free_ Route *route = NULL;
        int r;

        assert(link);
        assert(routes);
        assert(dst);

        r = route_new(&route);
        if (r < 0)
                return r;

        route->family = family;
        route->dst = *dst;
        route->dst_prefixlen = dst_prefixlen;
        route->tos = tos;
        route->priority = priority;
        route->table = table;

        r = set_ensure_allocated(routes, &route_hash_ops);
        if (r < 0)
                return r;

        r = set_put(*routes, route);
        if (r < 0)
                return r;

        route->link = link;

        if (ret)
                *ret = route;

        route = NULL;

        return 0;
}

int route_add_foreign(
                Link *link,
                int family,
                const union in_addr_union *dst,
                unsigned char dst_prefixlen,
                unsigned char tos,
                uint32_t priority,
                unsigned char table,
                Route **ret) {

        return route_add_internal(link, &link->routes_foreign, family, dst, dst_prefixlen, tos, priority, table, ret);
}

int route_add(
              Link *link,
              int family,
              const union in_addr_union *dst,
              unsigned char dst_prefixlen,
              unsigned char tos,
              uint32_t priority,
              unsigned char table,
              Route **ret) {

        Route *route;
        int r;

        r = route_get(link, family, dst, dst_prefixlen, tos, priority, table, &route);
        if (r == -ENOENT) {
                /* Route does not exist, create a new one */
                r = route_add_internal(link, &link->routes, family, dst, dst_prefixlen, tos, priority, table, &route);
                if (r < 0)
                        return r;
        } else if (r == 0) {
                /* Take over a foreign route */
                r = set_ensure_allocated(&link->routes, &route_hash_ops);
                if (r < 0)
                        return r;

                r = set_put(link->routes, route);
                if (r < 0)
                        return r;

                set_remove(link->routes_foreign, route);
        } else if (r == 1) {
                /* Route exists, do nothing */
                ;
        } else
                return r;

        if (ret)
                *ret = route;

        return 0;
}

int route_update(Route *route,
                 const union in_addr_union *src,
                 unsigned char src_prefixlen,
                 const union in_addr_union *gw,
                 const union in_addr_union *prefsrc,
                 unsigned char scope,
                 unsigned char protocol) {

        assert(route);
        assert(src);
        assert(gw);
        assert(prefsrc);

        route->src = *src;
        route->src_prefixlen = src_prefixlen;
        route->gw = *gw;
        route->prefsrc = *prefsrc;
        route->scope = scope;
        route->protocol = protocol;

        return 0;
}

int route_remove(Route *route, Link *link,
               sd_netlink_message_handler_t callback) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);
        assert(link->ifindex > 0);
        assert(route->family == AF_INET || route->family == AF_INET6);

        r = sd_rtnl_message_new_route(link->manager->rtnl, &req,
                                      RTM_DELROUTE, route->family,
                                      route->protocol);
        if (r < 0)
                return log_error_errno(r, "Could not create RTM_DELROUTE message: %m");

        if (!in_addr_is_null(route->family, &route->gw)) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_GATEWAY, &route->gw.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_GATEWAY, &route->gw.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_GATEWAY attribute: %m");
        }

        if (route->dst_prefixlen) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_DST, &route->dst.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_DST, &route->dst.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_DST attribute: %m");

                r = sd_rtnl_message_route_set_dst_prefixlen(req, route->dst_prefixlen);
                if (r < 0)
                        return log_error_errno(r, "Could not set destination prefix length: %m");
        }

        if (route->src_prefixlen) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_SRC, &route->src.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_SRC, &route->src.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_SRC attribute: %m");

                r = sd_rtnl_message_route_set_src_prefixlen(req, route->src_prefixlen);
                if (r < 0)
                        return log_error_errno(r, "Could not set source prefix length: %m");
        }

        if (!in_addr_is_null(route->family, &route->prefsrc)) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_PREFSRC, &route->prefsrc.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_PREFSRC, &route->prefsrc.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_PREFSRC attribute: %m");
        }

        r = sd_rtnl_message_route_set_scope(req, route->scope);
        if (r < 0)
                return log_error_errno(r, "Could not set scope: %m");

        r = sd_netlink_message_append_u32(req, RTA_PRIORITY, route->priority);
        if (r < 0)
                return log_error_errno(r, "Could not append RTA_PRIORITY attribute: %m");

        r = sd_netlink_message_append_u32(req, RTA_OIF, link->ifindex);
        if (r < 0)
                return log_error_errno(r, "Could not append RTA_OIF attribute: %m");

        r = sd_netlink_call_async(link->manager->rtnl, req, callback, link, 0, NULL);
        if (r < 0)
                return log_error_errno(r, "Could not send rtnetlink message: %m");

        link_ref(link);

        return 0;
}

static int route_expire_callback(sd_netlink *rtnl, sd_netlink_message *m, void *userdata) {
        Link *link = userdata;
        int r;

        assert(rtnl);
        assert(m);
        assert(link);
        assert(link->ifname);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST)
                log_link_warning_errno(link, r, "could not remove route: %m");

        return 1;
}

int route_expire_handler(sd_event_source *s, uint64_t usec, void *userdata) {
        Route *route = userdata;
        int r;

        assert(route);

        r = route_remove(route, route->link, route_expire_callback);
        if (r < 0)
                log_warning_errno(r, "Could not remove route: %m");
        else
                route_free(route);

        return 1;
}

int route_configure(
                Route *route,
                Link *link,
                sd_netlink_message_handler_t callback) {

        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        _cleanup_(sd_event_source_unrefp) sd_event_source *expire = NULL;
        usec_t lifetime;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);
        assert(link->ifindex > 0);

        if (route->family == AF_UNSPEC)
                route->family = AF_INET;

        assert(route->family == AF_INET || route->family == AF_INET6);

        if (route_get(link, route->family, &route->dst, route->dst_prefixlen, route->tos, route->priority, route->table, NULL) <= 0 &&
            set_size(link->routes) >= routes_max())
                return -E2BIG;

        r = sd_rtnl_message_new_route(link->manager->rtnl, &req,
                                      RTM_NEWROUTE, route->family,
                                      route->protocol);
        if (r < 0)
                return log_error_errno(r, "Could not create RTM_NEWROUTE message: %m");

        if (!in_addr_is_null(route->family, &route->gw)) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_GATEWAY, &route->gw.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_GATEWAY, &route->gw.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_GATEWAY attribute: %m");

                r = sd_rtnl_message_route_set_family(req, route->family);
                if (r < 0)
                        return log_error_errno(r, "Could not set route family: %m");
        }

        if (route->dst_prefixlen) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_DST, &route->dst.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_DST, &route->dst.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_DST attribute: %m");

                r = sd_rtnl_message_route_set_dst_prefixlen(req, route->dst_prefixlen);
                if (r < 0)
                        return log_error_errno(r, "Could not set destination prefix length: %m");
        }

        if (route->src_prefixlen) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_SRC, &route->src.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_SRC, &route->src.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_SRC attribute: %m");

                r = sd_rtnl_message_route_set_src_prefixlen(req, route->src_prefixlen);
                if (r < 0)
                        return log_error_errno(r, "Could not set source prefix length: %m");
        }

        if (!in_addr_is_null(route->family, &route->prefsrc)) {
                if (route->family == AF_INET)
                        r = sd_netlink_message_append_in_addr(req, RTA_PREFSRC, &route->prefsrc.in);
                else if (route->family == AF_INET6)
                        r = sd_netlink_message_append_in6_addr(req, RTA_PREFSRC, &route->prefsrc.in6);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTA_PREFSRC attribute: %m");
        }

        r = sd_rtnl_message_route_set_scope(req, route->scope);
        if (r < 0)
                return log_error_errno(r, "Could not set scope: %m");

        r = sd_rtnl_message_route_set_flags(req, route->flags);
        if (r < 0)
                return log_error_errno(r, "Could not set flags: %m");

        if (route->table != RT_TABLE_MAIN) {
                if (route->table < 256) {
                        r = sd_rtnl_message_route_set_table(req, route->table);
                        if (r < 0)
                                return log_error_errno(r, "Could not set route table: %m");
                } else {
                        r = sd_rtnl_message_route_set_table(req, RT_TABLE_UNSPEC);
                        if (r < 0)
                                return log_error_errno(r, "Could not set route table: %m");

                        /* Table attribute to allow more than 256. */
                        r = sd_netlink_message_append_data(req, RTA_TABLE, &route->table, sizeof(route->table));
                        if (r < 0)
                                return log_error_errno(r, "Could not append RTA_TABLE attribute: %m");
                }
        }

        r = sd_netlink_message_append_u32(req, RTA_PRIORITY, route->priority);
        if (r < 0)
                return log_error_errno(r, "Could not append RTA_PRIORITY attribute: %m");

        r = sd_netlink_message_append_u8(req, RTA_PREF, route->pref);
        if (r < 0)
                return log_error_errno(r, "Could not append RTA_PREF attribute: %m");

        r = sd_netlink_message_append_u32(req, RTA_OIF, link->ifindex);
        if (r < 0)
                return log_error_errno(r, "Could not append RTA_OIF attribute: %m");

        r = sd_netlink_message_open_container(req, RTA_METRICS);
        if (r < 0)
                return log_error_errno(r, "Could not append RTA_METRICS attribute: %m");

        if (route->mtu > 0) {
                r = sd_netlink_message_append_u32(req, RTAX_MTU, route->mtu);
                if (r < 0)
                        return log_error_errno(r, "Could not append RTAX_MTU attribute: %m");
        }

        r = sd_netlink_message_close_container(req);
        if (r < 0)
                return log_error_errno(r, "Could not append RTA_METRICS attribute: %m");

        r = sd_netlink_call_async(link->manager->rtnl, req, callback, link, 0, NULL);
        if (r < 0)
                return log_error_errno(r, "Could not send rtnetlink message: %m");

        link_ref(link);

        lifetime = route->lifetime;

        r = route_add(link, route->family, &route->dst, route->dst_prefixlen, route->tos, route->priority, route->table, &route);
        if (r < 0)
                return log_error_errno(r, "Could not add route: %m");

        /* TODO: drop expiration handling once it can be pushed into the kernel */
        route->lifetime = lifetime;

        if (route->lifetime != USEC_INFINITY) {
                r = sd_event_add_time(link->manager->event, &expire, clock_boottime_or_monotonic(),
                                      route->lifetime, 0, route_expire_handler, route);
                if (r < 0)
                        return log_error_errno(r, "Could not arm expiration timer: %m");
        }

        sd_event_source_unref(route->expire);
        route->expire = expire;
        expire = NULL;

        return 0;
}

int config_parse_gateway(const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_route_free_ Route *n = NULL;
        union in_addr_union buffer;
        int r, f;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (streq(section, "Network")) {
                /* we are not in an Route section, so treat
                 * this as the special '0' section */
                r = route_new_static(network, NULL, 0, &n);
        } else
                r = route_new_static(network, filename, section_line, &n);

        if (r < 0)
                return r;

        r = in_addr_from_string_auto(rvalue, &f, &buffer);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r, "Route is invalid, ignoring assignment: %s", rvalue);
                return 0;
        }

        n->family = f;
        n->gw = buffer;
        n = NULL;

        return 0;
}

int config_parse_preferred_src(const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_route_free_ Route *n = NULL;
        union in_addr_union buffer;
        int r, f;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        r = in_addr_from_string_auto(rvalue, &f, &buffer);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, EINVAL,
                           "Preferred source is invalid, ignoring assignment: %s", rvalue);
                return 0;
        }

        n->family = f;
        n->prefsrc = buffer;
        n = NULL;

        return 0;
}

int config_parse_destination(const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Network *network = userdata;
        _cleanup_route_free_ Route *n = NULL;
        const char *address, *e;
        union in_addr_union buffer;
        unsigned char prefixlen;
        int r, f;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        /* Destination|Source=address/prefixlen */

        /* address */
        e = strchr(rvalue, '/');
        if (e)
                address = strndupa(rvalue, e - rvalue);
        else
                address = rvalue;

        r = in_addr_from_string_auto(address, &f, &buffer);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r, "Destination is invalid, ignoring assignment: %s", address);
                return 0;
        }

        if (f != AF_INET && f != AF_INET6) {
                log_syntax(unit, LOG_ERR, filename, line, 0, "Unknown address family, ignoring assignment: %s", address);
                return 0;
        }

        /* prefixlen */
        if (e) {
                r = safe_atou8(e + 1, &prefixlen);
                if (r < 0) {
                        log_syntax(unit, LOG_ERR, filename, line, r, "Route destination prefix length is invalid, ignoring assignment: %s", e + 1);
                        return 0;
                }
        } else {
                switch (f) {
                        case AF_INET:
                                prefixlen = 32;
                                break;
                        case AF_INET6:
                                prefixlen = 128;
                                break;
                }
        }

        n->family = f;
        if (streq(lvalue, "Destination")) {
                n->dst = buffer;
                n->dst_prefixlen = prefixlen;
        } else if (streq(lvalue, "Source")) {
                n->src = buffer;
                n->src_prefixlen = prefixlen;
        } else
                assert_not_reached(lvalue);

        n = NULL;

        return 0;
}

int config_parse_route_priority(const char *unit,
                                const char *filename,
                                unsigned line,
                                const char *section,
                                unsigned section_line,
                                const char *lvalue,
                                int ltype,
                                const char *rvalue,
                                void *data,
                                void *userdata) {
        Network *network = userdata;
        _cleanup_route_free_ Route *n = NULL;
        uint32_t k;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        r = safe_atou32(rvalue, &k);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Could not parse route priority \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        n->priority = k;
        n = NULL;

        return 0;
}

int config_parse_route_scope(const char *unit,
                             const char *filename,
                             unsigned line,
                             const char *section,
                             unsigned section_line,
                             const char *lvalue,
                             int ltype,
                             const char *rvalue,
                             void *data,
                             void *userdata) {
        Network *network = userdata;
        _cleanup_route_free_ Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        if (streq(rvalue, "host"))
                n->scope = RT_SCOPE_HOST;
        else if (streq(rvalue, "link"))
                n->scope = RT_SCOPE_LINK;
        else if (streq(rvalue, "global"))
                n->scope = RT_SCOPE_UNIVERSE;
        else {
                log_syntax(unit, LOG_ERR, filename, line, 0, "Unknown route scope: %s", rvalue);
                return 0;
        }

        n = NULL;

        return 0;
}

int config_parse_route_table(const char *unit,
                             const char *filename,
                             unsigned line,
                             const char *section,
                             unsigned section_line,
                             const char *lvalue,
                             int ltype,
                             const char *rvalue,
                             void *data,
                             void *userdata) {
        _cleanup_route_free_ Route *n = NULL;
        Network *network = userdata;
        uint32_t k;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r < 0)
                return r;

        r = safe_atou32(rvalue, &k);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, r,
                           "Could not parse route table number \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        n->table = k;

        n = NULL;

        return 0;
}

int config_parse_route_device(const char *unit,
                           const char *filename,
                           unsigned line,
                           const char *section,
                           unsigned section_line,
                           const char *lvalue,
                           int ltype,
                           const char *rvalue,
                           void *data,
                           void *userdata) {
        _cleanup_route_free_ Route *n = NULL;
        Network *network = userdata;
        bool k;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, section_line, &n);
        if (r < 0)
                return r;

        r = config_parse_bool(unit, filename, line, section, section_line, lvalue, ltype, rvalue, &k, userdata);
        if (r < 0) {
                log_syntax(unit, LOG_ERR, filename, line, 0, "OnDevice= expects a boolean. Cannot parse \"%s\", ignoring", rvalue);
                return 0;
        }

        n->on_dev = k;

        n = NULL;

        return 0;
}
