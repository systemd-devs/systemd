/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

/***
  This file is part of systemd.

  Copyright 2014 Kay Sievers, Lennart Poettering
***/

#include "conf-parser.h"
#include "timesyncd-manager.h"

const struct ConfigPerfItem* timesyncd_gperf_lookup(const char *key, GPERF_LEN_TYPE length);

int manager_parse_server_string(Manager *m, ServerType type, const char *string);

int config_parse_servers(const char *unit, const char *filename, unsigned line, const char *section, unsigned section_line, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata);

int manager_parse_config_file(Manager *m);
int manager_parse_fallback_string(Manager *m, const char *string);
