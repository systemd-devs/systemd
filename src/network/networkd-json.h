/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-json.h"

#include "set.h"

typedef struct Link Link;
typedef struct Manager Manager;

int addresses_append_json(Set *addresses, bool only_managed, sd_json_variant **v);
int routes_append_json(Manager *manager, int ifindex, sd_json_variant **v);
int link_build_json(Link *link, sd_json_variant **ret);
int manager_build_json(Manager *manager, sd_json_variant **ret);
