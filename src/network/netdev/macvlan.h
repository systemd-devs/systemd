/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

typedef struct MacVlan MacVlan;

#include "macvlan-util.h"
#include "netdev.h"

struct MacVlan {
        NetDev meta;

        MacVlanMode mode;
};

DEFINE_NETDEV_CAST(MACVLAN, MacVlan);
DEFINE_NETDEV_CAST(MACVTAP, MacVlan);
extern const NetDevVTable macvlan_vtable;
extern const NetDevVTable macvtap_vtable;

CONFIG_PARSER_PROTOTYPE(config_parse_macvlan_mode);
