/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include <linux/if_link.h>

typedef enum MacVlanMode {
        NETDEV_MACVLAN_MODE_PRIVATE = MACVLAN_MODE_PRIVATE,
        NETDEV_MACVLAN_MODE_VEPA = MACVLAN_MODE_VEPA,
        NETDEV_MACVLAN_MODE_BRIDGE = MACVLAN_MODE_BRIDGE,
        NETDEV_MACVLAN_MODE_PASSTHRU = MACVLAN_MODE_PASSTHRU,
        _NETDEV_MACVLAN_MODE_MAX,
        _NETDEV_MACVLAN_MODE_INVALID = -1
} MacVlanMode;

const char *macvlan_mode_to_string(MacVlanMode d) _const_;
MacVlanMode macvlan_mode_from_string(const char *d) _pure_;
