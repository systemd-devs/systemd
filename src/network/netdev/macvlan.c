/* SPDX-License-Identifier: LGPL-2.1+ */

#include <net/if.h>

#include "conf-parser.h"
#include "macvlan.h"
#include "macvlan-util.h"

DEFINE_CONFIG_PARSE_ENUM(config_parse_macvlan_mode, macvlan_mode, MacVlanMode, "Failed to parse macvlan mode");

static int netdev_macvlan_fill_message_create(NetDev *netdev, Link *link, sd_netlink_message *req) {
        MacVlan *m;
        int r;

        assert(netdev);
        assert(link);
        assert(netdev->ifname);

        if (netdev->kind == NETDEV_KIND_MACVLAN)
                m = MACVLAN(netdev);
        else
                m = MACVTAP(netdev);

        assert(m);

        if (m->mode != _NETDEV_MACVLAN_MODE_INVALID) {
                r = sd_netlink_message_append_u32(req, IFLA_MACVLAN_MODE, m->mode);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_MACVLAN_MODE attribute: %m");
        }

        return 0;
}

static void macvlan_init(NetDev *n) {
        MacVlan *m;

        assert(n);

        if (n->kind == NETDEV_KIND_MACVLAN)
                m = MACVLAN(n);
        else
                m = MACVTAP(n);

        assert(m);

        m->mode = _NETDEV_MACVLAN_MODE_INVALID;
}

const NetDevVTable macvtap_vtable = {
        .object_size = sizeof(MacVlan),
        .init = macvlan_init,
        .sections = NETDEV_COMMON_SECTIONS "MACVTAP\0",
        .fill_message_create = netdev_macvlan_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .generate_mac = true,
};

const NetDevVTable macvlan_vtable = {
        .object_size = sizeof(MacVlan),
        .init = macvlan_init,
        .sections = NETDEV_COMMON_SECTIONS "MACVLAN\0",
        .fill_message_create = netdev_macvlan_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .generate_mac = true,
};
