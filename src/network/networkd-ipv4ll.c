/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/in.h>
#include <linux/if.h>

#include "network-internal.h"
#include "networkd-address.h"
#include "networkd-ipv4ll.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-queue.h"
#include "parse-util.h"

static int address_new_from_ipv4ll(Link *link, Address **ret) {
        _cleanup_(address_freep) Address *address = NULL;
        struct in_addr addr;
        int r;

        assert(link);
        assert(link->ipv4ll);
        assert(ret);

        r = sd_ipv4ll_get_address(link->ipv4ll, &addr);
        if (r < 0)
                return r;

        r = address_new(&address);
        if (r < 0)
                return -ENOMEM;

        address->family = AF_INET;
        address->in_addr.in = addr;
        address->prefixlen = 16;
        address->scope = RT_SCOPE_LINK;
        address->route_metric = IPV4LL_ROUTE_METRIC;
        address_set_broadcast(address);

        *ret = TAKE_PTR(address);
        return 0;
}

static int ipv4ll_address_lost(Link *link) {
        _cleanup_(address_freep) Address *address = NULL;
        int r;

        assert(link);

        link->ipv4ll_address_configured = false;

        r = address_new_from_ipv4ll(link, &address);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;

        log_link_debug(link, "IPv4 link-local release "IPV4_ADDRESS_FMT_STR,
                       IPV4_ADDRESS_FMT_VAL(address->in_addr.in));

        return address_remove(address, link);
}

static int ipv4ll_address_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(link);
        assert(!link->ipv4ll_address_configured);

        r = address_configure_handler_internal(rtnl, m, link, "Could not set ipv4ll address");
        if (r <= 0)
                return r;

        link->ipv4ll_address_configured = true;
        link_check_ready(link);

        return 1;
}

static int ipv4ll_address_claimed(sd_ipv4ll *ll, Link *link) {
        _cleanup_(address_freep) Address *address = NULL;
        int r;

        assert(ll);
        assert(link);

        link->ipv4ll_address_configured = false;

        r = address_new_from_ipv4ll(link, &address);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return r;

        log_link_debug(link, "IPv4 link-local claim "IPV4_ADDRESS_FMT_STR,
                       IPV4_ADDRESS_FMT_VAL(address->in_addr.in));

        return link_request_address(link, TAKE_PTR(address), true, NULL, ipv4ll_address_handler, NULL);
}

static void ipv4ll_handler(sd_ipv4ll *ll, int event, void *userdata) {
        Link *link = userdata;
        int r;

        assert(link);
        assert(link->network);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return;

        switch(event) {
                case SD_IPV4LL_EVENT_STOP:
                        r = ipv4ll_address_lost(link);
                        if (r < 0) {
                                link_enter_failed(link);
                                return;
                        }
                        break;
                case SD_IPV4LL_EVENT_CONFLICT:
                        r = ipv4ll_address_lost(link);
                        if (r < 0) {
                                link_enter_failed(link);
                                return;
                        }

                        r = sd_ipv4ll_restart(ll);
                        if (r < 0)
                                log_link_warning_errno(link, r, "Could not acquire IPv4 link-local address: %m");
                        break;
                case SD_IPV4LL_EVENT_BIND:
                        r = ipv4ll_address_claimed(ll, link);
                        if (r < 0) {
                                log_link_error(link, "Failed to configure ipv4ll address: %m");
                                link_enter_failed(link);
                                return;
                        }
                        break;
                default:
                        log_link_warning(link, "IPv4 link-local unknown event: %d", event);
                        break;
        }
}

int ipv4ll_configure(Link *link) {
        uint64_t seed;
        int r;

        assert(link);

        if (!link_ipv4ll_enabled(link))
                return 0;

        if (link->ipv4ll)
                return -EBUSY;

        r = sd_ipv4ll_new(&link->ipv4ll);
        if (r < 0)
                return r;

        r = sd_ipv4ll_attach_event(link->ipv4ll, link->manager->event, 0);
        if (r < 0)
                return r;

        if (link->sd_device &&
            net_get_unique_predictable_data(link->sd_device, true, &seed) >= 0) {
                r = sd_ipv4ll_set_address_seed(link->ipv4ll, seed);
                if (r < 0)
                        return r;
        }

        r = sd_ipv4ll_set_mac(link->ipv4ll, &link->hw_addr.ether);
        if (r < 0)
                return r;

        r = sd_ipv4ll_set_ifindex(link->ipv4ll, link->ifindex);
        if (r < 0)
                return r;

        r = sd_ipv4ll_set_callback(link->ipv4ll, ipv4ll_handler, link);
        if (r < 0)
                return r;

        return 0;
}

int ipv4ll_update_mac(Link *link) {
        bool restart;
        int r;

        assert(link);

        if (!link->ipv4ll)
                return 0;

        restart = sd_ipv4ll_is_running(link->ipv4ll) > 0;

        r = sd_ipv4ll_stop(link->ipv4ll);
        if (r < 0)
                return r;

        r = sd_ipv4ll_set_mac(link->ipv4ll, &link->hw_addr.ether);
        if (r < 0)
                return r;

        if (restart) {
                r = sd_ipv4ll_start(link->ipv4ll);
                if (r < 0)
                        return r;
        }

        return 0;
}

int config_parse_ipv4ll(
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

        AddressFamily *link_local = data;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        /* Note that this is mostly like
         * config_parse_address_family(), except that it
         * applies only to IPv4 */

        r = parse_boolean(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=%s, ignoring assignment. "
                           "Note that the setting %s= is deprecated, please use LinkLocalAddressing= instead.",
                           lvalue, rvalue, lvalue);
                return 0;
        }

        SET_FLAG(*link_local, ADDRESS_FAMILY_IPV4, r);

        log_syntax(unit, LOG_WARNING, filename, line, 0,
                   "%s=%s is deprecated, please use LinkLocalAddressing=%s instead.",
                   lvalue, rvalue, address_family_to_string(*link_local));

        return 0;
}
