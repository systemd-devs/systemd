/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <unistd.h>

#define SD_LOGIND_ROOT_CHECK_INHIBITORS           (UINT64_C(1) << 0)
#define SD_LOGIND_REBOOT_VIA_KEXEC                (UINT64_C(1) << 1)
#define SD_LOGIND_SOFT_REBOOT                     (UINT64_C(1) << 2)
#define SD_LOGIND_SOFT_REBOOT_IF_NEXTROOT_SET_UP  (UINT64_C(1) << 3)
#define SD_LOGIND_INTERACTIVE                     (UINT64_C(1) << 63) /* For internal use only */
#define SD_LOGIND_SHUTDOWN_AND_SLEEP_FLAGS_PUBLIC (SD_LOGIND_ROOT_CHECK_INHIBITORS|SD_LOGIND_REBOOT_VIA_KEXEC|SD_LOGIND_SOFT_REBOOT|SD_LOGIND_SOFT_REBOOT_IF_NEXTROOT_SET_UP)
#define SD_LOGIND_SHUTDOWN_AND_SLEEP_FLAGS_ALL    (SD_LOGIND_SHUTDOWN_AND_SLEEP_FLAGS_PUBLIC|SD_LOGIND_INTERACTIVE)

/* These flags are private b/t pam_systemd and logind */
#define SD_LOGIND_ENABLE_SECURE_LOCK           (UINT64_C(1) << 0)
#define SD_LOGIND_CREATE_SESSION_FLAGS_ALL     (SD_LOGIND_ENABLE_SECURE_LOCK)

bool session_id_valid(const char *id);

static inline bool logind_running(void) {
        return access("/run/systemd/seats/", F_OK) >= 0;
}
