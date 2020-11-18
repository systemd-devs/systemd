/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include "bpf-link.h"
#include "macro.h"

typedef struct Unit Unit;

int allow_bind_supported(void);

int allow_bind_install(Unit *u);
