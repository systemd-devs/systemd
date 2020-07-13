/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include <stdbool.h>
#include <unistd.h>

#define PATH_RUN_SYSTEMD_SEATS "/run/systemd/seats"

bool session_id_valid(const char *id);

static inline bool logind_running(void) {
        return access(PATH_RUN_SYSTEMD_SEATS, F_OK) >= 0;
}
