/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "alloc-util.h"
#include "ether-addr-util.h"
#include "hashmap.h"
#include "memory-util.h"
#include "networkd-dhcp-static-lease.h"
#include "networkd-network.h"
#include "networkd-util.h"
#include "strv.h"
#include "utf8.h"

DEFINE_NETWORK_SECTION_FUNCTIONS(DHCPStaticLease, dhcp_static_lease_free);

DHCPStaticLease *dhcp_static_lease_free(DHCPStaticLease *static_lease) {
        if (!static_lease)
                return NULL;

        if (static_lease->network && static_lease->section)
                ordered_hashmap_remove(static_lease->network->dhcp_static_leases_by_section, static_lease->section);

        network_config_section_free(static_lease->section);
        static_lease->static_lease = sd_dhcp_static_lease_unref(static_lease->static_lease);
        return mfree(static_lease);
}

static int dhcp_static_lease_new(DHCPStaticLease **ret) {
        DHCPStaticLease *p = NULL;
        int r;

        assert(ret);

        p = new0(DHCPStaticLease, 1);
        if (!p)
                return -ENOMEM;

        r = sd_dhcp_static_lease_new(&p->static_lease);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(p);
        return 0;
}

static int dhcp_static_lease_set_client_id(DHCPStaticLease *lease, const struct ether_addr *mac_addr) {
        assert(lease);
        assert(mac_addr);

        return sd_dhcp_static_lease_set_client_id_by_mac(lease->static_lease, mac_addr->ether_addr_octet, ETH_ALEN);
}

static int dhcp_static_lease_set_address(DHCPStaticLease *lease, const struct in_addr *address) {
        assert(address);
        assert(lease);

        return sd_dhcp_static_lease_set_address(lease->static_lease, address);
}

static int lease_new_static(Network *network, const char *filename, unsigned section_line, DHCPStaticLease **ret) {
        _cleanup_(network_config_section_freep) NetworkConfigSection *n = NULL;
        _cleanup_(dhcp_static_lease_freep) DHCPStaticLease *static_lease = NULL;
        int r;

        assert(network);
        assert(ret);
        assert(filename);
        assert(section_line > 0);
        
        r = network_config_section_new(filename, section_line, &n);
        if (r < 0)
                return r;

        static_lease = ordered_hashmap_get(network->dhcp_static_leases_by_section, n);
        if (static_lease) {
                *ret = TAKE_PTR(static_lease);
                return 0;
        }

        r = dhcp_static_lease_new(&static_lease);
        if (r < 0)
                return r;

        static_lease->network = network;

        static_lease->section = TAKE_PTR(n);
        r = ordered_hashmap_ensure_allocated(&network->dhcp_static_leases_by_section, &network_config_hash_ops);
        if (r < 0)
                return r;

        r = ordered_hashmap_put(network->dhcp_static_leases_by_section, static_lease->section, static_lease);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(static_lease);

        return 0;
}
