/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <time.h>

typedef enum ClockChangeDirection {
        CLOCK_CHANGE_NOOP,
        CLOCK_CHANGE_FORWARD,
        CLOCK_CHANGE_BACKWARD,
        _CLOCK_CHANGE_MAX,
        _CLOCK_CHANGE_INVALID = -EINVAL,
} ClockChangeDirection;

int clock_is_localtime(const char* adjtime_path);
int clock_set_timezone(int *ret_minutesdelta);
int clock_reset_timewarp(void);
int clock_get_hwclock(struct tm *tm);
int clock_set_hwclock(const struct tm *tm);
int clock_apply_epoch(bool allow_backwards, ClockChangeDirection *ret_attempted_change);

#define EPOCH_CLOCK_FILE "/usr/lib/clock-epoch"
#define TIMESYNCD_CLOCK_FILE_DIR "/var/lib/systemd/timesync/"
#define TIMESYNCD_CLOCK_FILE TIMESYNCD_CLOCK_FILE_DIR "clock"
