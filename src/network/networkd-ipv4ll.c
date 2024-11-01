/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/in.h>
#include <linux/if.h>

#include "netif-util.h"
#include "networkd-address.h"
#include "networkd-ipv4acd.h"
#include "networkd-ipv4ll.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-queue.h"
#include "parse-util.h"

bool link_ipv4ll_enabled(Link *link) {
        assert(link);

        if (!link_ipv4acd_supported(link))
                return false;

        if (!link->network)
                return false;

        if (link->network->bond)
                return false;

        return link->network->link_local & ADDRESS_FAMILY_IPV4;
}

static int address_new_from_ipv4ll(Link *link, Address **ret) {
        _cleanup_(address_unrefp) Address *address = NULL;
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

        address->source = NETWORK_CONFIG_SOURCE_IPV4LL;
        address->family = AF_INET;
        address->in_addr.in = addr;
        address->prefixlen = 16;
        address->scope = RT_SCOPE_LINK;
        address->route_metric = IPV4LL_ROUTE_METRIC;

        *ret = TAKE_PTR(address);
        return 0;
}

static int ipv4ll_address_lost(Link *link) {
        _cleanup_(address_unrefp) Address *address = NULL;
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

        return address_remove_and_cancel(address, link);
}

static int ipv4ll_address_handler(sd_netlink *rtnl, sd_netlink_message *m, Request *req, Link *link, Address *address) {
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
        _cleanup_(address_unrefp) Address *address = NULL;
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

        r = link_request_stacked_netdevs(link, NETDEV_LOCAL_ADDRESS_IPV4LL);
        if (r < 0)
                return r;

        return link_request_address(link, address, NULL, ipv4ll_address_handler, NULL);
}

static void ipv4ll_handler(sd_ipv4ll *ll, int event, void *userdata) {
        Link *link = ASSERT_PTR(userdata);
        int r;

        assert(link->network);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return;

        switch (event) {
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
                        if (r < 0) {
                                log_link_warning_errno(link, r, "Could not acquire IPv4 link-local address: %m");
                                link_enter_failed(link);
                        }
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

static int ipv4ll_check_mac(sd_ipv4ll *ll, const struct ether_addr *mac, void *userdata) {
        Manager *m = ASSERT_PTR(userdata);
        struct hw_addr_data hw_addr;

        assert(mac);

        hw_addr = (struct hw_addr_data) {
                .length = ETH_ALEN,
                .ether = *mac,
        };

        return link_get_by_hw_addr(m, &hw_addr, NULL) >= 0;
}

static int ipv4ll_set_address(Link *link) {
        assert(link);
        assert(link->network);
        assert(link->ipv4ll);

        if (!in4_addr_is_set(&link->network->ipv4ll_start_address))
                return 0;

        return sd_ipv4ll_set_address(link->ipv4ll, &link->network->ipv4ll_start_address);
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

        if (link->dev &&
            net_get_unique_predictable_data(link->dev, true, &seed) >= 0) {
                r = sd_ipv4ll_set_address_seed(link->ipv4ll, seed);
                if (r < 0)
                        return r;
        }

        r = ipv4ll_set_address(link);
        if (r < 0)
                return r;

        r = sd_ipv4ll_set_mac(link->ipv4ll, &link->hw_addr.ether);
        if (r < 0)
                return r;

        r = sd_ipv4ll_set_ifindex(link->ipv4ll, link->ifindex);
        if (r < 0)
                return r;

        r = sd_ipv4ll_set_callback(link->ipv4ll, ipv4ll_handler, link);
        if (r < 0)
                return r;

        return sd_ipv4ll_set_check_mac_callback(link->ipv4ll, ipv4ll_check_mac, link->manager);
}

int ipv4ll_update_mac(Link *link) {
        assert(link);

        if (link->hw_addr.length != ETH_ALEN)
                return 0;
        if (ether_addr_is_null(&link->hw_addr.ether))
                return 0;
        if (!link->ipv4ll)
                return 0;

        return sd_ipv4ll_set_mac(link->ipv4ll, &link->hw_addr.ether);
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

        AddressFamily *link_local = ASSERT_PTR(data);
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);

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

int config_parse_ipv4ll_address(
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

        union in_addr_union a;
        struct in_addr *ipv4ll_address = ASSERT_PTR(data);
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        if (isempty(rvalue)) {
                *ipv4ll_address = (struct in_addr) {};
                return 0;
        }

        r = in_addr_from_string(AF_INET, rvalue, &a);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring assignment: %s", lvalue, rvalue);
                return 0;
        }
        if (!in4_addr_is_link_local_dynamic(&a.in)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Specified address cannot be used as an IPv4 link local address, ignoring assignment: %s",
                           rvalue);
                return 0;
        }

        *ipv4ll_address = a.in;
        return 0;
}
