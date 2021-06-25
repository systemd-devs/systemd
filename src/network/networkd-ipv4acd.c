/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-dhcp-client.h"
#include "sd-ipv4acd.h"

#include "networkd-address.h"
#include "networkd-dhcp4.h"
#include "networkd-ipv4acd.h"
#include "networkd-link.h"
#include "networkd-manager.h"

static void on_acd(sd_ipv4acd *acd, int event, void *userdata, bool is_static) {
        Address *address = userdata;
        Link *link;
        int r;

        assert(acd);
        assert(address);
        assert(address->acd == acd);
        assert(address->link);
        assert(address->family == AF_INET);

        link = address->link;

        switch (event) {
        case SD_IPV4ACD_EVENT_STOP:
                log_link_debug(link, "Stopping ACD client...");

                if (address_get(link, address, NULL) < 0)
                        return;

                r = address_remove(address, link);
                if (r < 0) {
                        log_link_warning_errno(link, r, "Failed to remove address "IPV4_ADDRESS_FMT_STR": %m",
                                               IPV4_ADDRESS_FMT_VAL(address->in_addr.in));
                        link_enter_failed(link);
                }
                break;

        case SD_IPV4ACD_EVENT_BIND:
                log_link_debug(link, "Successfully claimed address "IPV4_ADDRESS_FMT_STR,
                               IPV4_ADDRESS_FMT_VAL(address->in_addr.in));

                address->acd_announced = true;
                break;

        case SD_IPV4ACD_EVENT_CONFLICT:
                if (!is_static) {
                        assert(link->dhcp_client);

                        r = sd_dhcp_client_send_decline(link->dhcp_client);
                        if (r < 0)
                                log_link_warning_errno(link, r, "Failed to send DHCP DECLINE, ignoring: %m");

                        if (link->dhcp_lease) {
                                log_link_warning(link, "ACD conflict. Dropping DHCPv4 lease.");
                                r = dhcp4_lease_lost(link);
                                if (r < 0) {
                                        link_enter_failed(link);
                                        return;
                                }
                        }
                }

                if (address_get(link, address, NULL) < 0) {
                        log_link_warning(link, "ACD conflict. Cannot configure address "IPV4_ADDRESS_FMT_STR,
                                         IPV4_ADDRESS_FMT_VAL(address->in_addr.in));
                        return;
                }

                log_link_warning(link, "ACD conflict. Dropping address "IPV4_ADDRESS_FMT_STR,
                                 IPV4_ADDRESS_FMT_VAL(address->in_addr.in));

                r = address_remove(address, link);
                if (r < 0) {
                        log_link_warning_errno(link, r, "Failed to drop ACD conflicted address "IPV4_ADDRESS_FMT_STR": %m",
                                               IPV4_ADDRESS_FMT_VAL(address->in_addr.in));
                        link_enter_failed(link);
                }
                break;

        default:
                assert_not_reached("Invalid IPv4ACD event.");
        }
}

static void static_address_on_acd(sd_ipv4acd *acd, int event, void *userdata) {
        on_acd(acd, event, userdata, true);
}

static void dhcp4_address_on_acd(sd_ipv4acd *acd, int event, void *userdata) {
        on_acd(acd, event, userdata, false);
}

static int ipv4acd_check_mac(sd_ipv4acd *acd, const struct ether_addr *mac, void *userdata) {
        Manager *m = userdata;
        struct hw_addr_data hw_addr;

        assert(m);
        assert(mac);

        hw_addr = (struct hw_addr_data) {
                .length = ETH_ALEN,
                .ether = *mac,
        };

        return link_get_by_hw_addr(m, &hw_addr, NULL) >= 0;
}

static int ipv4acd_configure(Link *link, const Address *a) {
        _cleanup_(address_freep) Address *address = NULL;
        int r;

        assert(link);
        assert(a);
        assert(a->family == AF_INET);

        log_link_debug(link, "Configuring IPv4ACD for address "IPV4_ADDRESS_FMT_STR,
                       IPV4_ADDRESS_FMT_VAL(a->in_addr.in));

        r = address_dup(a, &address);
        if (r < 0)
                return r;

        r = set_ensure_put(&link->addresses_ipv4acd, &address_hash_ops, address);
        if (r < 0)
                return r;
        if (r == 0)
                return -EEXIST;
        address->link = link;

        r = sd_ipv4acd_new(&address->acd);
        if (r < 0)
                return r;

        r = sd_ipv4acd_attach_event(address->acd, link->manager->event, 0);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_ifindex(address->acd, link->ifindex);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_mac(address->acd, &link->hw_addr.ether);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_address(address->acd, &address->in_addr.in);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_callback(address->acd,
                                    address->is_static ? static_address_on_acd : dhcp4_address_on_acd,
                                    address);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_check_mac_callback(address->acd, ipv4acd_check_mac, link->manager);
        if (r < 0)
                return r;

        if (link_has_carrier(link)) {
                r = sd_ipv4acd_start(address->acd, true);
                if (r < 0)
                        return r;
        }

        TAKE_PTR(address);
        return 0;
}

int ipv4acd_address_is_ready_to_configure(Link *link, const Address *address) {
        Address *acd_address;
        int r;

        acd_address = set_get(link->addresses_ipv4acd, address);
        if (!acd_address) {
                r = ipv4acd_configure(link, address);
                if (r < 0)
                        return log_link_warning_errno(link, r, "Failed to configure IPv4ACD client: %m");

                return false;
        }

        if (!acd_address->acd_announced)
                return false;

        r = set_ensure_put(&link->addresses, &address_hash_ops, acd_address);
        if (r < 0)
                return log_oom();
        if (r == 0)
                return log_link_warning_errno(link, SYNTHETIC_ERRNO(EEXIST), "Address already exists.");

        acd_address->flags |= IFA_F_TENTATIVE;
        return true;
}

int ipv4acd_update_mac(Link *link) {
        Address *address;
        int k, r = 0;

        assert(link);

        if (link->hw_addr.length != ETH_ALEN)
                return 0;
        if (ether_addr_is_null(&link->hw_addr.ether))
                return 0;

        SET_FOREACH(address, link->addresses_ipv4acd) {
                assert(address->acd);

                k = sd_ipv4acd_set_mac(address->acd, &link->hw_addr.ether);
                if (k < 0)
                        r = k;
        }
        if (r < 0)
                link_enter_failed(link);

        return r;
}

int ipv4acd_start(Link *link) {
        Address *address;
        int r;

        assert(link);

        SET_FOREACH(address, link->addresses_ipv4acd) {
                if (sd_ipv4acd_is_running(address->acd))
                        continue;

                r = sd_ipv4acd_start(address->acd, true);
                if (r < 0)
                        return r;
        }

        return 0;
}

int ipv4acd_stop(Link *link) {
        Address *address;
        int k, r = 0;

        assert(link);

        SET_FOREACH(address, link->addresses_ipv4acd) {
                k = sd_ipv4acd_stop(address->acd);
                if (k < 0)
                        r = k;
        }

        return r;
}
