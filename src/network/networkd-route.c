/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/icmpv6.h>
#include <linux/ipv6_route.h>
#include <linux/nexthop.h>

#include "alloc-util.h"
#include "event-util.h"
#include "netlink-util.h"
#include "networkd-address.h"
#include "networkd-ipv4ll.h"
#include "networkd-manager.h"
#include "networkd-network.h"
#include "networkd-nexthop.h"
#include "networkd-queue.h"
#include "networkd-route-util.h"
#include "networkd-route.h"
#include "parse-util.h"
#include "string-util.h"
#include "strv.h"
#include "vrf.h"
#include "wireguard.h"

static Route* route_detach_impl(Route *route) {
        assert(route);
        assert(!!route->network + !!route->manager + !!route->wireguard <= 1);

        if (route->network) {
                assert(route->section);
                hashmap_remove(route->network->routes_by_section, route->section);
                route->network = NULL;
                return route;
        }

        if (route->manager) {
                set_remove(route->manager->routes, route);
                route->manager = NULL;
                return route;
        }

        if (route->wireguard) {
                set_remove(route->wireguard->routes, route);
                route->wireguard = NULL;
                return route;
        }

        return NULL;
}

static void route_detach(Route *route) {
        route_unref(route_detach_impl(route));
}

static Route* route_free(Route *route) {
        if (!route)
                return NULL;

        route_detach_impl(route);

        config_section_free(route->section);
        route_nexthop_done(&route->nexthop);
        ordered_set_free(route->nexthops);
        free(route->metrics);
        free(route->tcp_congestion_control_algo);
        sd_event_source_disable_unref(route->expire);

        return mfree(route);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(Route, route, route_free);

static void route_hash_func(const Route *route, struct siphash *state) {
        assert(route);

        siphash24_compress_typesafe(route->family, state);

        switch (route->family) {
        case AF_INET:
                /* First, the table, destination prefix, priority, and tos (dscp), are used to find routes.
                 * See fib_table_insert(), fib_find_node(), and fib_find_alias() in net/ipv4/fib_trie.c of the kernel. */
                siphash24_compress_typesafe(route->table, state);
                in_addr_hash_func(&route->dst, route->family, state);
                siphash24_compress_typesafe(route->dst_prefixlen, state);
                siphash24_compress_typesafe(route->priority, state);
                siphash24_compress_typesafe(route->tos, state);

                /* Then, protocol, scope, type, flags, prefsrc, metrics (RTAX_* attributes), and nexthops (gateways)
                 * are used to find routes. See fib_find_info() in net/ipv4/fib_semantics.c of the kernel. */
                siphash24_compress_typesafe(route->protocol, state);
                siphash24_compress_typesafe(route->scope, state);
                siphash24_compress_typesafe(route->type, state);
                unsigned flags = route->flags & ~RTNH_COMPARE_MASK;
                siphash24_compress_typesafe(flags, state);
                in_addr_hash_func(&route->prefsrc, route->family, state);

                /* metrics */
                siphash24_compress_typesafe(route->n_metrics, state);
                for (size_t i = 1; i < route->n_metrics; i++)
                        siphash24_compress_typesafe(route->metrics[i], state);
                siphash24_compress_string(route->tcp_congestion_control_algo, state);

                /* nexthops (id, number of nexthops, nexthop) */
                route_nexthops_hash_func(route, state);
                break;

        case AF_INET6:
                /* First, table and destination prefix are used for classifying routes.
                 * See fib6_add() and fib6_add_1() in net/ipv6/ip6_fib.c of the kernel. */
                siphash24_compress_typesafe(route->table, state);
                in_addr_hash_func(&route->dst, route->family, state);
                siphash24_compress_typesafe(route->dst_prefixlen, state);

                /* Then, source prefix is used. See fib6_add(). */
                in_addr_hash_func(&route->src, route->family, state);
                siphash24_compress_typesafe(route->src_prefixlen, state);

                /* See fib6_add_rt2node(). */
                siphash24_compress_typesafe(route->priority, state);

                /* See rt6_duplicate_nexthop() in include/net/ip6_route.h of the kernel.
                 * Here, we hash nexthop in a similar way as the one for IPv4. */
                route_nexthops_hash_func(route, state);

                /* If the above entries are same, then only the expiration time and MTU can be updated. */
                break;

        default:
                /* treat any other address family as AF_UNSPEC */
                break;
        }
}

static int route_compare_func(const Route *a, const Route *b) {
        int r;

        r = CMP(a->family, b->family);
        if (r != 0)
                return r;

        switch (a->family) {
        case AF_INET:
                r = CMP(a->table, b->table);
                if (r != 0)
                        return r;

                r = memcmp(&a->dst, &b->dst, FAMILY_ADDRESS_SIZE(a->family));
                if (r != 0)
                        return r;

                r = CMP(a->dst_prefixlen, b->dst_prefixlen);
                if (r != 0)
                        return r;

                r = CMP(a->priority, b->priority);
                if (r != 0)
                        return r;

                r = CMP(a->tos, b->tos);
                if (r != 0)
                        return r;

                r = CMP(a->protocol, b->protocol);
                if (r != 0)
                        return r;

                r = CMP(a->scope, b->scope);
                if (r != 0)
                        return r;

                r = CMP(a->type, b->type);
                if (r != 0)
                        return r;

                r = CMP(a->flags & ~RTNH_COMPARE_MASK, b->flags & ~RTNH_COMPARE_MASK);
                if (r != 0)
                        return r;

                r = memcmp(&a->prefsrc, &b->prefsrc, FAMILY_ADDRESS_SIZE(a->family));
                if (r != 0)
                        return r;

                r = CMP(a->n_metrics, b->n_metrics);
                if (r != 0)
                        return r;

                for (size_t i = 1; i < a->n_metrics; i++) {
                        r = CMP(a->metrics[i], b->metrics[i]);
                        if (r != 0)
                                return r;
                }

                r = strcmp_ptr(a->tcp_congestion_control_algo, b->tcp_congestion_control_algo);
                if (r != 0)
                        return r;

                return route_nexthops_compare_func(a, b);

        case AF_INET6:
                r = CMP(a->table, b->table);
                if (r != 0)
                        return r;

                r = memcmp(&a->dst, &b->dst, FAMILY_ADDRESS_SIZE(a->family));
                if (r != 0)
                        return r;

                r = CMP(a->dst_prefixlen, b->dst_prefixlen);
                if (r != 0)
                        return r;

                r = memcmp(&a->src, &b->src, FAMILY_ADDRESS_SIZE(a->family));
                if (r != 0)
                        return r;

                r = CMP(a->src_prefixlen, b->src_prefixlen);
                if (r != 0)
                        return r;

                r = CMP(a->priority, b->priority);
                if (r != 0)
                        return r;

                return route_nexthops_compare_func(a, b);

        default:
                /* treat any other address family as AF_UNSPEC */
                return 0;
        }
}

DEFINE_HASH_OPS_WITH_KEY_DESTRUCTOR(
                route_hash_ops,
                Route,
                route_hash_func,
                route_compare_func,
                route_detach);

DEFINE_HASH_OPS_WITH_VALUE_DESTRUCTOR(
                route_section_hash_ops,
                ConfigSection,
                config_section_hash_func,
                config_section_compare_func,
                Route,
                route_detach);

int route_new(Route **ret) {
        _cleanup_(route_unrefp) Route *route = NULL;

        route = new(Route, 1);
        if (!route)
                return -ENOMEM;

        *route = (Route) {
                .n_ref = 1,
                .family = AF_UNSPEC,
                .scope = RT_SCOPE_UNIVERSE,
                .protocol = RTPROT_UNSPEC,
                .type = RTN_UNICAST,
                .table = RT_TABLE_MAIN,
                .lifetime_usec = USEC_INFINITY,
                .gateway_onlink = -1,
        };

        *ret = TAKE_PTR(route);

        return 0;
}

int route_new_static(Network *network, const char *filename, unsigned section_line, Route **ret) {
        _cleanup_(config_section_freep) ConfigSection *n = NULL;
        _cleanup_(route_unrefp) Route *route = NULL;
        int r;

        assert(network);
        assert(ret);
        assert(filename);
        assert(section_line > 0);

        r = config_section_new(filename, section_line, &n);
        if (r < 0)
                return r;

        route = hashmap_get(network->routes_by_section, n);
        if (route) {
                *ret = TAKE_PTR(route);
                return 0;
        }

        if (hashmap_size(network->routes_by_section) >= routes_max())
                return -E2BIG;

        r = route_new(&route);
        if (r < 0)
                return r;

        route->protocol = RTPROT_STATIC;
        route->network = network;
        route->section = TAKE_PTR(n);
        route->source = NETWORK_CONFIG_SOURCE_STATIC;

        r = hashmap_ensure_put(&network->routes_by_section, &route_section_hash_ops, route->section, route);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(route);
        return 0;
}

static int route_add(Manager *manager, Route *route) {
        int r;

        assert(manager);
        assert(route);
        assert(!route->network);

        r = set_ensure_put(&manager->routes, &route_hash_ops, route);
        if (r < 0)
                return r;
        if (r == 0)
                return -EEXIST;

        route->manager = manager;
        return 0;
}

int route_get(Manager *manager, const Route *route, Route **ret) {
        Route *existing;

        assert(manager);
        assert(route);

        existing = set_get(manager->routes, route);
        if (!existing)
                return -ENOENT;

        if (ret)
                *ret = existing;

        return 0;
}

static int route_get_link(Manager *manager, const Route *route, Link **ret) {
        int r;

        assert(manager);
        assert(route);

        if (route->nexthop_id != 0) {
                NextHop *nh;

                r = nexthop_get_by_id(manager, route->nexthop_id, &nh);
                if (r < 0)
                        return r;

                return link_get_by_index(manager, nh->ifindex, ret);
        }

        return route_nexthop_get_link(manager, &route->nexthop, ret);
}

static int route_get_request(Manager *manager, const Route *route, Request **ret) {
        Request *req;

        assert(manager);
        assert(route);

        req = ordered_set_get(manager->request_queue,
                              &(const Request) {
                                      .type = REQUEST_TYPE_ROUTE,
                                      .userdata = (void*) route,
                                      .hash_func = (hash_func_t) route_hash_func,
                                      .compare_func = (compare_func_t) route_compare_func,
                              });
        if (!req)
                return -ENOENT;

        if (ret)
                *ret = req;
        return 0;
}

bool route_type_is_reject(const Route *route) {
        assert(route);

        return IN_SET(route->type, RTN_UNREACHABLE, RTN_PROHIBIT, RTN_BLACKHOLE, RTN_THROW);
}

int route_dup(const Route *src, const RouteNextHop *nh, Route **ret) {
        _cleanup_(route_unrefp) Route *dest = NULL;
        int r;

        assert(src);
        assert(IN_SET(src->family, AF_INET, AF_INET6));
        assert(ret);

        dest = newdup(Route, src, 1);
        if (!dest)
                return -ENOMEM;

        /* Unset number of reference and all pointers */
        dest->n_ref = 1;
        dest->manager = NULL;
        dest->network = NULL;
        dest->section = NULL;
        dest->nexthop = ROUTE_NEXTHOP_NULL;
        dest->nexthops = NULL;
        dest->metrics = NULL;
        dest->tcp_congestion_control_algo = NULL;
        dest->expire = NULL;

        if (src->n_metrics > 0) {
                assert(src->n_metrics != 1);

                dest->metrics = newdup(uint32_t, src->metrics, src->n_metrics);
                if (!dest->metrics)
                        return -ENOMEM;
        }

        r = free_and_strdup(&dest->tcp_congestion_control_algo, src->tcp_congestion_control_algo);
        if (r < 0)
                return r;

        r = route_nexthops_copy(src, nh, dest);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(dest);
        return 0;
}

static void log_route_debug(const Route *route, const char *str, Manager *manager) {
        _cleanup_free_ char *state = NULL, *nexthop = NULL, *prefsrc = NULL,
                *table = NULL, *scope = NULL, *proto = NULL, *flags = NULL;
        const char *dst, *src;
        Link *link = NULL;

        assert(route);
        assert(str);
        assert(manager);

        if (!DEBUG_LOGGING)
                return;

        (void) route_get_link(manager, route, &link);

        (void) network_config_state_to_string_alloc(route->state, &state);

        dst = in_addr_is_set(route->family, &route->dst) || route->dst_prefixlen > 0 ?
                IN_ADDR_PREFIX_TO_STRING(route->family, &route->dst, route->dst_prefixlen) : NULL;
        src = in_addr_is_set(route->family, &route->src) || route->src_prefixlen > 0 ?
                IN_ADDR_PREFIX_TO_STRING(route->family, &route->src, route->src_prefixlen) : NULL;

        (void) route_nexthops_to_string(route, &nexthop);

        if (in_addr_is_set(route->family, &route->prefsrc))
                (void) in_addr_to_string(route->family, &route->prefsrc, &prefsrc);
        (void) route_scope_to_string_alloc(route->scope, &scope);
        (void) manager_get_route_table_to_string(manager, route->table, /* append_num = */ true, &table);
        (void) route_protocol_full_to_string_alloc(route->protocol, &proto);
        (void) route_flags_to_string_alloc(route->flags, &flags);

        log_link_debug(link,
                       "%s %s route (%s): dst: %s, src: %s, %s, prefsrc: %s, "
                       "table: %s, priority: %"PRIu32", "
                       "proto: %s, scope: %s, type: %s, flags: %s",
                       str, strna(network_config_source_to_string(route->source)), strna(state),
                       strna(dst), strna(src), strna(nexthop), strna(prefsrc),
                       strna(table), route->priority,
                       strna(proto), strna(scope), strna(route_type_to_string(route->type)), strna(flags));
}

static int route_set_netlink_message(const Route *route, sd_netlink_message *m) {
        int r;

        assert(route);
        assert(m);

        /* rtmsg header (and relevant attributes) */
        if (route->dst_prefixlen > 0) {
                r = netlink_message_append_in_addr_union(m, RTA_DST, route->family, &route->dst);
                if (r < 0)
                        return r;

                r = sd_rtnl_message_route_set_dst_prefixlen(m, route->dst_prefixlen);
                if (r < 0)
                        return r;
        }

        if (route->src_prefixlen > 0) {
                r = netlink_message_append_in_addr_union(m, RTA_SRC, route->family, &route->src);
                if (r < 0)
                        return r;

                r = sd_rtnl_message_route_set_src_prefixlen(m, route->src_prefixlen);
                if (r < 0)
                        return r;
        }

        r = sd_rtnl_message_route_set_tos(m, route->tos);
        if (r < 0)
                return r;

        r = sd_rtnl_message_route_set_scope(m, route->scope);
        if (r < 0)
                return r;

        r = sd_rtnl_message_route_set_type(m, route->type);
        if (r < 0)
                return r;

        r = sd_rtnl_message_route_set_flags(m, route->flags & ~RTNH_COMPARE_MASK);
        if (r < 0)
                return r;

        /* attributes */
        r = sd_netlink_message_append_u32(m, RTA_PRIORITY, route->priority);
        if (r < 0)
                return r;

        if (in_addr_is_set(route->family, &route->prefsrc)) {
                r = netlink_message_append_in_addr_union(m, RTA_PREFSRC, route->family, &route->prefsrc);
                if (r < 0)
                        return r;
        }

        if (route->table < 256) {
                r = sd_rtnl_message_route_set_table(m, route->table);
                if (r < 0)
                        return r;
        } else {
                r = sd_rtnl_message_route_set_table(m, RT_TABLE_UNSPEC);
                if (r < 0)
                        return r;

                /* Table attribute to allow more than 256. */
                r = sd_netlink_message_append_u32(m, RTA_TABLE, route->table);
                if (r < 0)
                        return r;
        }

        r = sd_netlink_message_append_u8(m, RTA_PREF, route->pref);
        if (r < 0)
                return r;

        /* nexthops */
        r = route_nexthops_set_netlink_message(route, m);
        if (r < 0)
                return r;

        /* metric */
        r = sd_netlink_message_open_container(m, RTA_METRICS);
        if (r < 0)
                return r;

        for (size_t i = 1; i < route->n_metrics; i++) {
                if (i == RTAX_CC_ALGO)
                        continue;

                if (route->metrics[i] == 0)
                        continue;

                r = sd_netlink_message_append_u32(m, i, route->metrics[i]);
                if (r < 0)
                        return r;
        }

        if (!isempty(route->tcp_congestion_control_algo)) {
                r = sd_netlink_message_append_string(m, RTAX_CC_ALGO, route->tcp_congestion_control_algo);
                if (r < 0)
                        return r;
        }

        r = sd_netlink_message_close_container(m);
        if (r < 0)
                return r;

        return 0;
}

static int route_remove_handler(sd_netlink *rtnl, sd_netlink_message *m, RemoveRequest *rreq) {
        int r;

        assert(m);
        assert(rreq);

        Manager *manager = ASSERT_PTR(rreq->manager);
        Route *route = ASSERT_PTR(rreq->userdata);

        r = sd_netlink_message_get_errno(m);
        if (r < 0) {
                log_message_full_errno(m,
                                       (r == -ESRCH || !route->manager) ? LOG_DEBUG : LOG_WARNING,
                                       r, "Could not drop route, ignoring");

                if (route->manager) {
                        /* If the route cannot be removed, then assume the route is already removed. */
                        log_route_debug(route, "Forgetting", manager);

                        Request *req;
                        if (route_get_request(manager, route, &req) >= 0)
                                route_enter_removed(req->userdata);

                        route_detach(route);
                }
        }

        return 1;
}

int route_remove(Route *route, Manager *manager) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        Link *link = NULL;
        int r;

        assert(route);
        assert(manager);

        log_route_debug(route, "Removing", manager);

        (void) route_get_link(manager, route, &link);

        r = sd_rtnl_message_new_route(manager->rtnl, &m, RTM_DELROUTE, route->family, route->protocol);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not create netlink message: %m");

        r = route_set_netlink_message(route, m);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not fill netlink message: %m");

        r = manager_remove_request_add(manager, route, route, manager->rtnl, m, route_remove_handler);
        if (r < 0)
                return log_link_warning_errno(link, r, "Could not queue rtnetlink message: %m");

        route_enter_removing(route);
        return 0;
}

int route_remove_and_cancel(Route *route, Manager *manager) {
        bool waiting = false;
        Request *req;

        assert(route);
        assert(manager);

        /* If the route is remembered by the manager, then use the remembered object. */
        (void) route_get(manager, route, &route);

        /* Cancel the request for the route. If the request is already called but we have not received the
         * notification about the request, then explicitly remove the route. */
        if (route_get_request(manager, route, &req) >= 0) {
                waiting = req->waiting_reply;
                request_detach(manager, req);
                route_cancel_requesting(route);
        }

        /* If we know that the route will come or already exists, remove it. */
        if (waiting || (route->manager && route_exists(route)))
                return route_remove(route, manager);

        return 0;
}

static int route_expire_handler(sd_event_source *s, uint64_t usec, void *userdata) {
        Route *route = ASSERT_PTR(userdata);
        int r;

        if (!route->manager)
                return 0; /* already detached. */

        r = route_remove(route, route->manager);
        if (r < 0) {
                Link *link = NULL;

                (void) route_get_link(route->manager, route, &link);
                log_link_warning_errno(link, r, "Could not remove route: %m");
                if (link)
                        link_enter_failed(link);
        }

        return 1;
}

static int route_setup_timer(Route *route, const struct rta_cacheinfo *cacheinfo) {
        int r;

        assert(route);

        if (cacheinfo && cacheinfo->rta_expires != 0)
                route->expiration_managed_by_kernel = true;

        if (route->lifetime_usec == USEC_INFINITY || /* We do not request expiration for the route. */
            route->expiration_managed_by_kernel) {   /* We have received nonzero expiration previously. The expiration is managed by the kernel. */
                route->expire = sd_event_source_disable_unref(route->expire);
                return 0;
        }

        Manager *manager = ASSERT_PTR(route->manager);
        r = event_reset_time(manager->event, &route->expire, CLOCK_BOOTTIME,
                             route->lifetime_usec, 0, route_expire_handler, route, 0, "route-expiration", true);
        if (r < 0) {
                Link *link = NULL;
                (void) route_get_link(manager, route, &link);
                log_link_warning_errno(link, r, "Failed to configure expiration timer for route, ignoring: %m");

        } else if (r > 0)
                log_route_debug(route, "Configured expiration timer for", manager);

        return r;
}

int route_configure_handler_internal(sd_netlink *rtnl, sd_netlink_message *m, Link *link, Route *route, const char *error_msg) {
        int r;

        assert(m);
        assert(link);
        assert(link->manager);
        assert(route);
        assert(error_msg);

        r = sd_netlink_message_get_errno(m);
        if (r == -EEXIST) {
                Route *existing;

                if (route_get(link->manager, route, &existing) >= 0) {
                        /* When re-configuring an existing route, kernel does not send RTM_NEWROUTE
                         * notification, so we need to update the timer here. */
                        existing->lifetime_usec = route->lifetime_usec;
                        (void) route_setup_timer(existing, NULL);
                }

        } else if (r < 0) {
                log_link_message_warning_errno(link, m, r, error_msg);
                link_enter_failed(link);
                return 0;
        }

        return 1;
}

static int route_configure(const Route *route, uint32_t lifetime_sec, Link *link, Request *req) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        int r;

        assert(route);
        assert(link);
        assert(link->manager);
        assert(req);

        log_route_debug(route, "Configuring", link->manager);

        r = sd_rtnl_message_new_route(link->manager->rtnl, &m, RTM_NEWROUTE, route->family, route->protocol);
        if (r < 0)
                return r;

        r = route_set_netlink_message(route, m);
        if (r < 0)
                return r;

        if (lifetime_sec != UINT32_MAX) {
                r = sd_netlink_message_append_u32(m, RTA_EXPIRES, lifetime_sec);
                if (r < 0)
                        return r;
        }

        return request_call_netlink_async(link->manager->rtnl, m, req);
}

static int route_is_ready_to_configure(const Route *route, Link *link) {
        int r;

        assert(route);
        assert(link);

        if (!link_is_ready_to_configure(link, /* allow_unmanaged = */ false))
                return false;

        if (in_addr_is_set(route->family, &route->prefsrc) > 0) {
                r = manager_has_address(link->manager, route->family, &route->prefsrc);
                if (r <= 0)
                        return r;
        }

        return route_nexthops_is_ready_to_configure(route, link->manager);
}

static int route_requeue_request(Request *req, Link *link, const Route *route) {
        _cleanup_(route_unrefp) Route *tmp = NULL;
        int r;

        assert(route);
        assert(link);
        assert(link->manager);

        r = route_dup(route, NULL, &tmp);
        if (r < 0)
                return r;

        r = route_adjust_nexthops(tmp, link);
        if (r < 0)
                return r;

        if (route_compare_func(route, tmp) == 0)
                return 0;

        r = link_queue_request_full(link,
                                    req->type,
                                    tmp,
                                    req->free_func,
                                    req->hash_func,
                                    req->compare_func,
                                    req->process,
                                    req->counter,
                                    req->netlink_handler,
                                    NULL);
        if (r < 0)
                return r;
        if (r == 0)
                return 1; /* Already queued?? That's OK. */

        TAKE_PTR(tmp);
        return 1;
}

static int route_process_request(Request *req, Link *link, Route *route) {
        Route *existing;
        int r;

        assert(req);
        assert(link);
        assert(link->manager);
        assert(route);

        r = route_is_ready_to_configure(route, link);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to check if route is ready to configure: %m");
        if (r == 0)
                return 0;

        usec_t now_usec;
        assert_se(sd_event_now(link->manager->event, CLOCK_BOOTTIME, &now_usec) >= 0);
        uint32_t sec = usec_to_sec(route->lifetime_usec, now_usec);
        if (sec == 0) {
                log_link_debug(link, "Refuse to configure %s route with zero lifetime.",
                               network_config_source_to_string(route->source));

                route_cancel_requesting(route);
                if (route_get(link->manager, route, &existing) >= 0)
                        route_cancel_requesting(existing);
                return 1;
        }

        r = route_requeue_request(req, link, route);
        if (r != 0)
                return r;

        r = route_configure(route, sec, link, req);
        if (r < 0)
                return log_link_warning_errno(link, r, "Failed to configure route: %m");

        route_enter_configuring(route);
        if (route_get(link->manager, route, &existing) >= 0)
                route_enter_configuring(existing);
        return 1;
}

static int link_request_route_one(
                Link *link,
                const Route *route,
                const RouteNextHop *nh,
                unsigned *message_counter,
                route_netlink_handler_t netlink_handler,
                Request **ret) {

        _cleanup_(route_unrefp) Route *tmp = NULL;
        Route *existing = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(route);

        r = route_dup(route, nh, &tmp);
        if (r < 0)
                return r;

        r = route_adjust_nexthops(tmp, link);
        if (r < 0)
                return r;

        if (route_get(link->manager, tmp, &existing) >= 0)
                /* Copy state for logging below. */
                tmp->state = existing->state;

        log_route_debug(tmp, "Requesting", link->manager);
        r = link_queue_request_safe(link, REQUEST_TYPE_ROUTE,
                                    tmp,
                                    route_unref,
                                    route_hash_func,
                                    route_compare_func,
                                    route_process_request,
                                    message_counter,
                                    netlink_handler,
                                    NULL);
        if (r <= 0)
                return r;

        route_enter_requesting(tmp);
        if (existing)
                route_enter_requesting(existing);

        TAKE_PTR(tmp);
        return 1;
}

int link_request_route(
                Link *link,
                const Route *route,
                unsigned *message_counter,
                route_netlink_handler_t netlink_handler,
                Request **ret) {

        int r;

        assert(link);
        assert(link->manager);
        assert(route);
        assert(route->source != NETWORK_CONFIG_SOURCE_FOREIGN);

        if (route->family == AF_INET || route_type_is_reject(route) || ordered_set_isempty(route->nexthops))
                return link_request_route_one(link, route, NULL, message_counter, netlink_handler, ret);

        RouteNextHop *nh;
        ORDERED_SET_FOREACH(nh, route->nexthops) {
                r = link_request_route_one(link, route, nh, message_counter, netlink_handler, ret);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int static_route_handler(sd_netlink *rtnl, sd_netlink_message *m, Request *req, Link *link, Route *route) {
        int r;

        assert(link);

        r = route_configure_handler_internal(rtnl, m, link, route, "Could not set route");
        if (r <= 0)
                return r;

        if (link->static_route_messages == 0) {
                log_link_debug(link, "Routes set");
                link->static_routes_configured = true;
                link_check_ready(link);
        }

        return 1;
}

static int link_request_wireguard_routes(Link *link, bool only_ipv4) {
        NetDev *netdev;
        Route *route;
        int r;

        assert(link);

        if (!streq_ptr(link->kind, "wireguard"))
                return 0;

        if (netdev_get(link->manager, link->ifname, &netdev) < 0)
                return 0;

        Wireguard *w = WIREGUARD(netdev);

        SET_FOREACH(route, w->routes) {
                if (only_ipv4 && route->family != AF_INET)
                        continue;

                r = link_request_route(link, route, &link->static_route_messages, static_route_handler, NULL);
                if (r < 0)
                        return r;
        }

        return 0;
}

int link_request_static_routes(Link *link, bool only_ipv4) {
        Route *route;
        int r;

        assert(link);
        assert(link->network);

        link->static_routes_configured = false;

        HASHMAP_FOREACH(route, link->network->routes_by_section) {
                if (route->gateway_from_dhcp_or_ra)
                        continue;

                if (only_ipv4 && route->family != AF_INET)
                        continue;

                r = link_request_route(link, route, &link->static_route_messages, static_route_handler, NULL);
                if (r < 0)
                        return r;
        }

        r = link_request_wireguard_routes(link, only_ipv4);
        if (r < 0)
                return r;

        if (link->static_route_messages == 0) {
                link->static_routes_configured = true;
                link_check_ready(link);
        } else {
                log_link_debug(link, "Requesting routes");
                link_set_state(link, LINK_STATE_CONFIGURING);
        }

        return 0;
}

int route_set_metric(Route *route, uint16_t attr, uint32_t value) {
        assert(route);

        if (value != 0) {
                if (!GREEDY_REALLOC0(route->metrics, attr + 1))
                        return -ENOMEM;

                route->metrics[attr] = value;
                route->n_metrics = MAX(route->n_metrics, (size_t) (attr + 1));
                return 0;
        }

        if (route->n_metrics <= attr)
                return 0;

        route->metrics[attr] = 0;

        for (size_t i = route->n_metrics - 1; i > 0; i--)
                if (route->metrics[i] != 0) {
                        route->n_metrics = i + 1;
                        return 0;
                }

        route->metrics = mfree(route->metrics);
        route->n_metrics = 0;
        return 0;
}

static int process_route_one(
                Manager *manager,
                uint16_t type,
                Route *tmp,
                const struct rta_cacheinfo *cacheinfo) {

        Request *req = NULL;
        Route *route = NULL;
        Link *link = NULL;
        bool is_new = false, update_dhcp4;
        int r;

        assert(manager);
        assert(tmp);
        assert(IN_SET(type, RTM_NEWROUTE, RTM_DELROUTE));

        (void) route_get(manager, tmp, &route);
        (void) route_get_request(manager, tmp, &req);
        (void) route_get_link(manager, tmp, &link);

        update_dhcp4 = link && tmp->family == AF_INET6 && tmp->dst_prefixlen == 0;

        switch (type) {
        case RTM_NEWROUTE:
                if (!route) {
                        if (!manager->manage_foreign_routes && !(req && req->waiting_reply)) {
                                route_enter_configured(tmp);
                                log_route_debug(tmp, "Ignoring received", manager);
                                return 0;
                        }

                        /* If we do not know the route, then save it. */
                        r = route_add(manager, tmp);
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Failed to remember foreign route, ignoring: %m");
                                return 0;
                        }

                        route = route_ref(tmp);
                        is_new = true;

                } else {
                        /* Update remembered route with the received notification. */
                        route->nexthop.weight = tmp->nexthop.weight;
                }

                /* Also update information that cannot be obtained through netlink notification. */
                if (req && req->waiting_reply) {
                        Route *rt = ASSERT_PTR(req->userdata);

                        route->source = rt->source;
                        route->provider = rt->provider;
                        route->lifetime_usec = rt->lifetime_usec;
                }

                route_enter_configured(route);
                log_route_debug(route, is_new ? "Received new" : "Received remembered", manager);

                (void) route_setup_timer(route, cacheinfo);

                break;

        case RTM_DELROUTE:
                if (route) {
                        route_enter_removed(route);
                        log_route_debug(route, "Forgetting removed", manager);
                        route_detach(route);
                } else
                        log_route_debug(tmp,
                                        manager->manage_foreign_routes ? "Kernel removed unknown" : "Ignoring received",
                                        manager);

                if (req)
                        route_enter_removed(req->userdata);

                break;

        default:
                assert_not_reached();
        }

        if (update_dhcp4) {
                r = dhcp4_update_ipv6_connectivity(link);
                if (r < 0) {
                        log_link_warning_errno(link, r, "Failed to notify IPv6 connectivity to DHCPv4 client: %m");
                        link_enter_failed(link);
                }
        }

        return 1;
}

int manager_rtnl_process_route(sd_netlink *rtnl, sd_netlink_message *message, Manager *m) {
        _cleanup_(route_unrefp) Route *tmp = NULL;
        int r;

        assert(rtnl);
        assert(message);
        assert(m);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_message_warning_errno(message, r, "rtnl: failed to receive route message, ignoring");

                return 0;
        }

        uint16_t type;
        r = sd_netlink_message_get_type(message, &type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: could not get message type, ignoring: %m");
                return 0;
        } else if (!IN_SET(type, RTM_NEWROUTE, RTM_DELROUTE)) {
                log_warning("rtnl: received unexpected message type %u when processing route, ignoring.", type);
                return 0;
        }

        r = route_new(&tmp);
        if (r < 0)
                return log_oom();

        r = sd_rtnl_message_route_get_family(message, &tmp->family);
        if (r < 0) {
                log_warning("rtnl: received route message without family, ignoring");
                return 0;
        } else if (!IN_SET(tmp->family, AF_INET, AF_INET6)) {
                log_debug("rtnl: received route message with invalid family '%i', ignoring", tmp->family);
                return 0;
        }

        /* rtmsg header */
        r = sd_rtnl_message_route_get_dst_prefixlen(message, &tmp->dst_prefixlen);
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message with invalid destination prefixlen, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_src_prefixlen(message, &tmp->src_prefixlen);
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message with invalid source prefixlen, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_tos(message, &tmp->tos);
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message with invalid tos, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_protocol(message, &tmp->protocol);
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message without route protocol, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_scope(message, &tmp->scope);
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message with invalid scope, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_type(message, &tmp->type);
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message with invalid type, ignoring: %m");
                return 0;
        }

        r = sd_rtnl_message_route_get_flags(message, &tmp->flags);
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message without route flags, ignoring: %m");
                return 0;
        }

        /* attributes */
        r = netlink_message_read_in_addr_union(message, RTA_DST, tmp->family, &tmp->dst);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: received route message without valid destination, ignoring: %m");
                return 0;
        }

        r = netlink_message_read_in_addr_union(message, RTA_SRC, tmp->family, &tmp->src);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: received route message without valid source, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, RTA_PRIORITY, &tmp->priority);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: received route message with invalid priority, ignoring: %m");
                return 0;
        }

        r = netlink_message_read_in_addr_union(message, RTA_PREFSRC, tmp->family, &tmp->prefsrc);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: received route message without valid preferred source, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, RTA_TABLE, &tmp->table);
        if (r == -ENODATA) {
                unsigned char table;

                r = sd_rtnl_message_route_get_table(message, &table);
                if (r >= 0)
                        tmp->table = table;
        }
        if (r < 0) {
                log_warning_errno(r, "rtnl: received route message with invalid table, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u8(message, RTA_PREF, &tmp->pref);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: received route message with invalid preference, ignoring: %m");
                return 0;
        }

        /* metrics */
        size_t len;
        _cleanup_free_ void *data = NULL;
        r = sd_netlink_message_read_data(message, RTA_METRICS, &len, &data);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: Could not read RTA_METRICS attribute, ignoring: %m");
                return 0;
        }
        if (r >= 0) {
                for (struct rtattr *rta = data; RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
                        size_t rta_type = RTA_TYPE(rta);

                        if (rta_type == RTAX_CC_ALGO) {
                                char *p = memdup_suffix0(RTA_DATA(rta), RTA_PAYLOAD(rta));
                                if (!p)
                                        return log_oom();

                                free_and_replace(tmp->tcp_congestion_control_algo, p);

                        } else {
                                if (RTA_PAYLOAD(rta) != sizeof(uint32_t))
                                        continue;

                                r = route_set_metric(tmp, rta_type, *(uint32_t*) RTA_DATA(rta));
                                if (r < 0)
                                        return log_oom();
                        }
                }
        }

        /* nexthops */
        if (route_nexthops_read_netlink_message(tmp, message) < 0)
                return 0;

        struct rta_cacheinfo cacheinfo;
        bool has_cacheinfo;
        r = sd_netlink_message_read(message, RTA_CACHEINFO, sizeof(cacheinfo), &cacheinfo);
        if (r < 0 && r != -ENODATA) {
                log_warning_errno(r, "rtnl: failed to read RTA_CACHEINFO attribute, ignoring: %m");
                return 0;
        }
        has_cacheinfo = r >= 0;

        if (tmp->family == AF_INET || ordered_set_isempty(tmp->nexthops))
                return process_route_one(m, type, tmp, has_cacheinfo ? &cacheinfo : NULL);

        RouteNextHop *nh;
        ORDERED_SET_FOREACH(nh, tmp->nexthops) {
                _cleanup_(route_unrefp) Route *dup = NULL;

                r = route_dup(tmp, nh, &dup);
                if (r < 0)
                        return log_oom();

                r = process_route_one(m, type, dup, has_cacheinfo ? &cacheinfo : NULL);
                if (r < 0)
                        return r;
        }

        return 1;
}

void manager_mark_routes(Manager *manager, Link *link, NetworkConfigSource source) {
        Route *route;

        assert(manager);

        SET_FOREACH(route, manager->routes) {
                if (route->source != source)
                        continue;

                if (link) {
                        Link *route_link;

                        if (route_get_link(manager, route, &route_link) < 0)
                                continue;
                        if (route_link != link)
                                continue;
                }

                route_mark(route);
        }
}

static bool route_by_kernel(const Route *route) {
        assert(route);

        if (route->protocol == RTPROT_KERNEL)
                return true;

        /* The kernels older than a826b04303a40d52439aa141035fca5654ccaccd (v5.11) create the IPv6
         * multicast with RTPROT_BOOT. Do not touch it. */
        if (route->protocol == RTPROT_BOOT &&
            route->family == AF_INET6 &&
            route->dst_prefixlen == 8 &&
            in6_addr_equal(&route->dst.in6, & (struct in6_addr) {{{ 0xff,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }}}))
                return true;

        return false;
}

static int link_unmark_route(Link *link, const Route *route, const RouteNextHop *nh) {
        _cleanup_(route_unrefp) Route *tmp = NULL;
        Route *existing;
        int r;

        assert(link);
        assert(route);

        r = route_dup(route, nh, &tmp);
        if (r < 0)
                return r;

        r = route_adjust_nexthops(tmp, link);
        if (r < 0)
                return r;

        if (route_get(link->manager, tmp, &existing) < 0)
                return 0;

        route_unmark(existing);
        return 1;
}

static int link_mark_routes(Link *link, bool foreign) {
        Route *route;
        Link *other;
        int r;

        assert(link);
        assert(link->manager);

        /* First, mark all routes. */
        SET_FOREACH(route, link->manager->routes) {
                /* Do not touch routes managed by the kernel. */
                if (route_by_kernel(route))
                        continue;

                /* When 'foreign' is true, mark only foreign routes, and vice versa.
                 * Note, do not touch dynamic routes. They will removed by when e.g. lease is lost. */
                if (route->source != (foreign ? NETWORK_CONFIG_SOURCE_FOREIGN : NETWORK_CONFIG_SOURCE_STATIC))
                        continue;

                /* Ignore routes not assigned yet or already removed. */
                if (!route_exists(route))
                        continue;

                route_mark(route);
        }

        /* Then, unmark all routes requested by active links. */
        HASHMAP_FOREACH(other, link->manager->links_by_index) {
                if (!foreign && other == link)
                        continue;

                if (!IN_SET(other->state, LINK_STATE_CONFIGURING, LINK_STATE_CONFIGURED))
                        continue;

                HASHMAP_FOREACH(route, other->network->routes_by_section) {
                        if (route->family == AF_INET || ordered_set_isempty(route->nexthops)) {
                                r = link_unmark_route(other, route, NULL);
                                if (r < 0)
                                        return r;

                        } else {
                                RouteNextHop *nh;
                                ORDERED_SET_FOREACH(nh, route->nexthops) {
                                        r = link_unmark_route(other, route, nh);
                                        if (r < 0)
                                                return r;
                                }
                        }
                }
        }

        /* Also unmark routes requested in .netdev file. */
        if (foreign && link->netdev && link->netdev->kind == NETDEV_KIND_WIREGUARD) {
                Wireguard *w = WIREGUARD(link->netdev);

                SET_FOREACH(route, w->routes) {
                        r = link_unmark_route(link, route, NULL);
                        if (r < 0)
                                return r;
                }
        }

        return 0;
}

int link_drop_routes(Link *link, bool foreign) {
        Route *route;
        int r;

        assert(link);
        assert(link->manager);

        r = link_mark_routes(link, foreign);
        if (r < 0)
                return r;

        SET_FOREACH(route, link->manager->routes) {
                if (!route_is_marked(route))
                        continue;

                RET_GATHER(r, route_remove(route, link->manager));
        }

        return r;
}

int link_foreignize_routes(Link *link) {
        Route *route;
        int r;

        assert(link);
        assert(link->manager);

        r = link_mark_routes(link, /* foreign = */ false);
        if (r < 0)
                return r;

        SET_FOREACH(route, link->manager->routes) {
                if (!route_is_marked(route))
                        continue;

                route->source = NETWORK_CONFIG_SOURCE_FOREIGN;
        }

        return 0;
}

int network_add_ipv4ll_route(Network *network) {
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        unsigned section_line;
        int r;

        assert(network);

        if (!network->ipv4ll_route)
                return 0;

        r = hashmap_by_section_find_unused_line(network->routes_by_section, network->filename, &section_line);
        if (r < 0)
                return r;

        /* IPv4LLRoute= is in [Network] section. */
        r = route_new_static(network, network->filename, section_line, &n);
        if (r < 0)
                return r;

        r = in_addr_from_string(AF_INET, "169.254.0.0", &n->dst);
        if (r < 0)
                return r;

        n->family = AF_INET;
        n->dst_prefixlen = 16;
        n->scope = RT_SCOPE_LINK;
        n->scope_set = true;
        n->table_set = true;
        n->priority = IPV4LL_ROUTE_METRIC;
        n->protocol = RTPROT_STATIC;

        TAKE_PTR(n);
        return 0;
}

int network_add_default_route_on_device(Network *network) {
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        unsigned section_line;
        int r;

        assert(network);

        if (!network->default_route_on_device)
                return 0;

        r = hashmap_by_section_find_unused_line(network->routes_by_section, network->filename, &section_line);
        if (r < 0)
                return r;

        /* DefaultRouteOnDevice= is in [Network] section. */
        r = route_new_static(network, network->filename, section_line, &n);
        if (r < 0)
                return r;

        n->family = AF_INET;
        n->scope = RT_SCOPE_LINK;
        n->scope_set = true;
        n->protocol = RTPROT_STATIC;

        TAKE_PTR(n);
        return 0;
}

int config_parse_gateway(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (streq(section, "Network")) {
                /* we are not in an Route section, so use line number instead */
                r = route_new_static(network, filename, line, &n);
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to allocate route, ignoring assignment: %m");
                        return 0;
                }
        } else {
                r = route_new_static(network, filename, section_line, &n);
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to allocate route, ignoring assignment: %m");
                        return 0;
                }

                if (isempty(rvalue)) {
                        n->gateway_from_dhcp_or_ra = false;
                        n->nexthop.family = AF_UNSPEC;
                        n->nexthop.gw = IN_ADDR_NULL;
                        TAKE_PTR(n);
                        return 0;
                }

                if (streq(rvalue, "_dhcp")) {
                        n->gateway_from_dhcp_or_ra = true;
                        n->nexthop.family = AF_UNSPEC;
                        n->nexthop.gw = IN_ADDR_NULL;
                        TAKE_PTR(n);
                        return 0;
                }

                if (streq(rvalue, "_dhcp4")) {
                        n->gateway_from_dhcp_or_ra = true;
                        n->nexthop.family = AF_INET;
                        n->nexthop.gw = IN_ADDR_NULL;
                        TAKE_PTR(n);
                        return 0;
                }

                if (streq(rvalue, "_ipv6ra")) {
                        n->gateway_from_dhcp_or_ra = true;
                        n->nexthop.family = AF_INET6;
                        n->nexthop.gw = IN_ADDR_NULL;
                        TAKE_PTR(n);
                        return 0;
                }
        }

        r = in_addr_from_string_auto(rvalue, &n->nexthop.family, &n->nexthop.gw);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Invalid %s='%s', ignoring assignment: %m", lvalue, rvalue);
                return 0;
        }

        n->gateway_from_dhcp_or_ra = false;
        TAKE_PTR(n);
        return 0;
}

int config_parse_preferred_src(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (n->family == AF_UNSPEC)
                r = in_addr_from_string_auto(rvalue, &n->family, &n->prefsrc);
        else
                r = in_addr_from_string(n->family, rvalue, &n->prefsrc);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, EINVAL,
                           "Invalid %s='%s', ignoring assignment: %m", lvalue, rvalue);
                return 0;
        }

        TAKE_PTR(n);
        return 0;
}

int config_parse_destination(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        union in_addr_union *buffer;
        unsigned char *prefixlen;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (streq(lvalue, "Destination")) {
                buffer = &n->dst;
                prefixlen = &n->dst_prefixlen;
        } else if (streq(lvalue, "Source")) {
                buffer = &n->src;
                prefixlen = &n->src_prefixlen;
        } else
                assert_not_reached();

        if (n->family == AF_UNSPEC)
                r = in_addr_prefix_from_string_auto(rvalue, &n->family, buffer, prefixlen);
        else
                r = in_addr_prefix_from_string(rvalue, n->family, buffer, prefixlen);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, EINVAL,
                           "Invalid %s='%s', ignoring assignment: %m", lvalue, rvalue);
                return 0;
        }

        (void) in_addr_mask(n->family, buffer, *prefixlen);

        TAKE_PTR(n);
        return 0;
}

int config_parse_route_priority(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        r = safe_atou32(rvalue, &n->priority);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse route priority \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        n->priority_set = true;
        TAKE_PTR(n);
        return 0;
}

int config_parse_route_scope(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        r = route_scope_from_string(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Unknown route scope: %s", rvalue);
                return 0;
        }

        n->scope = r;
        n->scope_set = true;
        TAKE_PTR(n);
        return 0;
}

int config_parse_route_nexthop(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        uint32_t id;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (isempty(rvalue)) {
                n->nexthop_id = 0;
                TAKE_PTR(n);
                return 0;
        }

        r = safe_atou32(rvalue, &id);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r, "Failed to parse nexthop ID, ignoring assignment: %s", rvalue);
                return 0;
        }
        if (id == 0) {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "Invalid nexthop ID, ignoring assignment: %s", rvalue);
                return 0;
        }

        n->nexthop_id = id;
        TAKE_PTR(n);
        return 0;
}

int config_parse_route_table(
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

        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        Network *network = userdata;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        r = manager_get_route_table_from_string(network->manager, rvalue, &n->table);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse route table \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        n->table_set = true;
        TAKE_PTR(n);
        return 0;
}

int config_parse_route_boolean(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse %s=\"%s\", ignoring assignment: %m", lvalue, rvalue);
                return 0;
        }

        if (STR_IN_SET(lvalue, "GatewayOnLink", "GatewayOnlink")) {
                n->gateway_onlink = r;
                TAKE_PTR(n);
                return 0;
        }

        uint16_t attr_type;
        if (streq(lvalue, "QuickAck"))
                attr_type = RTAX_QUICKACK;
        else if (streq(lvalue, "FastOpenNoCookie"))
                attr_type = RTAX_FASTOPEN_NO_COOKIE;
        else
                assert_not_reached();

        r = route_set_metric(n, attr_type, r);
        if (r < 0)
                return log_oom();

        TAKE_PTR(n);
        return 0;
}

int config_parse_ipv6_route_preference(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (streq(rvalue, "low"))
                n->pref = ICMPV6_ROUTER_PREF_LOW;
        else if (streq(rvalue, "medium"))
                n->pref = ICMPV6_ROUTER_PREF_MEDIUM;
        else if (streq(rvalue, "high"))
                n->pref = ICMPV6_ROUTER_PREF_HIGH;
        else {
                log_syntax(unit, LOG_WARNING, filename, line, 0, "Unknown route preference: %s", rvalue);
                return 0;
        }

        n->pref_set = true;
        TAKE_PTR(n);
        return 0;
}

int config_parse_route_protocol(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        r = route_protocol_from_string(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse route protocol \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        n->protocol = r;

        TAKE_PTR(n);
        return 0;
}

int config_parse_route_type(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int t, r;

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        t = route_type_from_string(rvalue);
        if (t < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse route type \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        n->type = (unsigned char) t;

        TAKE_PTR(n);
        return 0;
}

int config_parse_route_hop_limit(
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

        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        Network *network = userdata;
        uint32_t k;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (isempty(rvalue)) {
                (void) route_set_metric(n, RTAX_HOPLIMIT, 0);
                TAKE_PTR(n);
                return 0;
        }

        r = safe_atou32(rvalue, &k);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse per route hop limit, ignoring assignment: %s", rvalue);
                return 0;
        }
        if (k > 255) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Specified per route hop limit \"%s\" is too large, ignoring assignment: %m", rvalue);
                return 0;
        }
        if (k == 0) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Invalid per route hop limit \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        r = route_set_metric(n, RTAX_HOPLIMIT, k);
        if (r < 0)
                return log_oom();

        TAKE_PTR(n);
        return 0;
}

int config_parse_tcp_congestion(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        r = config_parse_string(unit, filename, line, section, section_line, lvalue, ltype,
                                rvalue, &n->tcp_congestion_control_algo, userdata);
        if (r < 0)
                return r;

        TAKE_PTR(n);
        return 0;
}

int config_parse_tcp_advmss(
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

        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        Network *network = userdata;
        uint64_t u;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (isempty(rvalue)) {
                (void) route_set_metric(n, RTAX_ADVMSS, 0);
                TAKE_PTR(n);
                return 0;
        }

        r = parse_size(rvalue, 1024, &u);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse TCPAdvertisedMaximumSegmentSize= \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        if (u == 0 || u > UINT32_MAX) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Invalid TCPAdvertisedMaximumSegmentSize= \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        r = route_set_metric(n, RTAX_ADVMSS, (uint32_t) u);
        if (r < 0)
                return log_oom();

        TAKE_PTR(n);
        return 0;
}

int config_parse_tcp_window(
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

        uint32_t k, *window = ASSERT_PTR(data);
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = safe_atou32(rvalue, &k);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse TCP %s \"%s\", ignoring assignment: %m", lvalue, rvalue);
                return 0;
        }
        if (k >= 1024) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Specified TCP %s \"%s\" is too large, ignoring assignment: %m", lvalue, rvalue);
                return 0;
        }
        if (k == 0) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Invalid TCP %s \"%s\", ignoring assignment: %m", lvalue, rvalue);
                return 0;
        }

        *window = k;
        return 1;
}

int config_parse_route_tcp_window(
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

        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        Network *network = userdata;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        uint16_t attr_type;
        if (streq(lvalue, "InitialCongestionWindow"))
                attr_type = RTAX_INITCWND;
        else if (streq(lvalue, "InitialAdvertisedReceiveWindow"))
                attr_type = RTAX_INITRWND;
        else
                assert_not_reached();

        if (isempty(rvalue)) {
                (void) route_set_metric(n, attr_type, 0);
                TAKE_PTR(n);
                return 0;
        }

        uint32_t k;
        r = config_parse_tcp_window(unit, filename, line, section, section_line, lvalue, ltype, rvalue, &k, userdata);
        if (r <= 0)
                return r;

        r = route_set_metric(n, attr_type, k);
        if (r < 0)
                return log_oom();

        TAKE_PTR(n);
        return 0;
}

int config_parse_route_mtu(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (isempty(rvalue)) {
                (void) route_set_metric(n, RTAX_MTU, 0);
                TAKE_PTR(n);
                return 0;
        }

        uint32_t k;
        r = config_parse_mtu(unit, filename, line, section, section_line, lvalue, ltype, rvalue, &k, userdata);
        if (r <= 0)
                return r;

        r = route_set_metric(n, RTAX_MTU, k);
        if (r < 0)
                return log_oom();

        TAKE_PTR(n);
        return 0;
}

int config_parse_route_tcp_rto(
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

        Network *network = userdata;
        _cleanup_(route_unref_or_set_invalidp) Route *n = NULL;
        usec_t usec;
        int r;

        assert(filename);
        assert(section);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = route_new_static(network, filename, section_line, &n);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to allocate route, ignoring assignment: %m");
                return 0;
        }

        if (isempty(rvalue)) {
                (void) route_set_metric(n, RTAX_RTO_MIN, 0);
                TAKE_PTR(n);
                return 0;
        }

        r = parse_sec(rvalue, &usec);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse route TCP retransmission timeout (RTO), ignoring assignment: %s", rvalue);
                return 0;
        }

        if (!timestamp_is_set(usec) ||
            DIV_ROUND_UP(usec, USEC_PER_MSEC) > UINT32_MAX) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Route TCP retransmission timeout (RTO) must be in the range 0…%"PRIu32"ms, ignoring assignment: %s", UINT32_MAX, rvalue);
                return 0;
        }

        r = route_set_metric(n, RTAX_RTO_MIN, (uint32_t) DIV_ROUND_UP(usec, USEC_PER_MSEC));
        if (r < 0)
                return log_oom();

        TAKE_PTR(n);
        return 0;
}

int route_section_verify(Route *route, Network *network) {
        assert(route);
        assert(route->section);

        if (section_is_invalid(route->section))
                return -EINVAL;

        /* Currently, we do not support static route with finite lifetime. */
        assert(route->lifetime_usec == USEC_INFINITY);

        if (route->gateway_from_dhcp_or_ra) {
                assert(network);

                if (route->nexthop.family == AF_UNSPEC)
                        /* When deprecated Gateway=_dhcp is set, then assume gateway family based on other settings. */
                        switch (route->family) {
                        case AF_UNSPEC:
                                log_warning("%s: Deprecated value \"_dhcp\" is specified for Gateway= in [Route] section from line %u. "
                                            "Please use \"_dhcp4\" or \"_ipv6ra\" instead. Assuming \"_dhcp4\".",
                                            route->section->filename, route->section->line);

                                route->nexthop.family = route->family = AF_INET;
                                break;
                        case AF_INET:
                        case AF_INET6:
                                log_warning("%s: Deprecated value \"_dhcp\" is specified for Gateway= in [Route] section from line %u. "
                                            "Assuming \"%s\" based on Destination=, Source=, or PreferredSource= setting.",
                                            route->section->filename, route->section->line, route->family == AF_INET ? "_dhcp4" : "_ipv6ra");

                                route->nexthop.family = route->family;
                                break;
                        default:
                                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                                         "%s: Invalid route family. Ignoring [Route] section from line %u.",
                                                         route->section->filename, route->section->line);
                        }

                if (route->nexthop.family == AF_INET && !FLAGS_SET(network->dhcp, ADDRESS_FAMILY_IPV4))
                        return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                                 "%s: Gateway=\"_dhcp4\" is specified but DHCPv4 client is disabled. "
                                                 "Ignoring [Route] section from line %u.",
                                                 route->section->filename, route->section->line);

                if (route->nexthop.family == AF_INET6 && !network->ipv6_accept_ra)
                        return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                                 "%s: Gateway=\"_ipv6ra\" is specified but IPv6AcceptRA= is disabled. "
                                                 "Ignoring [Route] section from line %u.",
                                                 route->section->filename, route->section->line);
        }

        /* When only Gateway= is specified, assume the route family based on the Gateway address. */
        if (route->family == AF_UNSPEC)
                route->family = route->nexthop.family;

        if (route->family == AF_UNSPEC) {
                assert(route->section);

                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Route section without Gateway=, Destination=, Source=, "
                                         "or PreferredSource= field configured. "
                                         "Ignoring [Route] section from line %u.",
                                         route->section->filename, route->section->line);
        }

        /* table */
        if (!route->table_set && network && network->vrf) {
                route->table = VRF(network->vrf)->table;
                route->table_set = true;
        }

        if (!route->table_set && IN_SET(route->type, RTN_LOCAL, RTN_BROADCAST, RTN_ANYCAST, RTN_NAT))
                route->table = RT_TABLE_LOCAL;

        /* scope */
        if (!route->scope_set && route->family == AF_INET) {
                if (IN_SET(route->type, RTN_LOCAL, RTN_NAT))
                        route->scope = RT_SCOPE_HOST;
                else if (IN_SET(route->type, RTN_BROADCAST, RTN_ANYCAST, RTN_MULTICAST))
                        route->scope = RT_SCOPE_LINK;
                else if (IN_SET(route->type, RTN_UNICAST, RTN_UNSPEC) &&
                         !route->gateway_from_dhcp_or_ra &&
                         !in_addr_is_set(route->nexthop.family, &route->nexthop.gw) &&
                         ordered_set_isempty(route->nexthops) &&
                         route->nexthop_id == 0)
                        route->scope = RT_SCOPE_LINK;
        }

        if (route->family == AF_INET6) {
                if (route->scope != RT_SCOPE_UNIVERSE) {
                        log_warning("%s: Scope= is specified for IPv6 route. It will be ignored.", route->section->filename);
                        route->scope = RT_SCOPE_UNIVERSE;
                }

                if (route->priority == 0)
                        route->priority = IP6_RT_PRIO_USER;
        }

        if (route->gateway_onlink < 0 && in_addr_is_set(route->nexthop.family, &route->nexthop.gw) &&
            network && ordered_hashmap_isempty(network->addresses_by_section)) {
                /* If no address is configured, in most cases the gateway cannot be reachable.
                 * TODO: we may need to improve the condition above. */
                log_warning("%s: Gateway= without static address configured. "
                            "Enabling GatewayOnLink= option.",
                            route->section->filename);
                route->gateway_onlink = true;
        }

        if (route->gateway_onlink >= 0)
                SET_FLAG(route->flags, RTNH_F_ONLINK, route->gateway_onlink);

        if (route->family == AF_INET6) {
                if (route->nexthop.family == AF_INET)
                        return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                                 "%s: IPv4 gateway is configured for IPv6 route. "
                                                 "Ignoring [Route] section from line %u.",
                                                 route->section->filename, route->section->line);

                RouteNextHop *nh;
                ORDERED_SET_FOREACH(nh, route->nexthops)
                        if (nh->family == AF_INET)
                                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                                         "%s: IPv4 multipath route is specified for IPv6 route. "
                                                         "Ignoring [Route] section from line %u.",
                                                         route->section->filename, route->section->line);
        }

        if (route->nexthop_id != 0 &&
            (route->gateway_from_dhcp_or_ra ||
             in_addr_is_set(route->nexthop.family, &route->nexthop.gw) ||
             !ordered_set_isempty(route->nexthops)))
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: NextHopId= cannot be specified with Gateway= or MultiPathRoute=. "
                                         "Ignoring [Route] section from line %u.",
                                         route->section->filename, route->section->line);

        if (route_type_is_reject(route) &&
            (route->gateway_from_dhcp_or_ra ||
             in_addr_is_set(route->nexthop.family, &route->nexthop.gw) ||
             !ordered_set_isempty(route->nexthops)))
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: reject type route cannot be specified with Gateway= or MultiPathRoute=. "
                                         "Ignoring [Route] section from line %u.",
                                         route->section->filename, route->section->line);

        if ((route->gateway_from_dhcp_or_ra ||
             in_addr_is_set(route->nexthop.family, &route->nexthop.gw)) &&
            !ordered_set_isempty(route->nexthops))
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "%s: Gateway= cannot be specified with MultiPathRoute=. "
                                         "Ignoring [Route] section from line %u.",
                                         route->section->filename, route->section->line);

        return 0;
}

void network_drop_invalid_routes(Network *network) {
        Route *route;

        assert(network);

        HASHMAP_FOREACH(route, network->routes_by_section)
                if (route_section_verify(route, network) < 0)
                        route_detach(route);
}
