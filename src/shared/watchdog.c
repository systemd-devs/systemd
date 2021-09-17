/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <unistd.h>
#include <linux/watchdog.h>

#include "errno-util.h"
#include "fd-util.h"
#include "log.h"
#include "string-util.h"
#include "time-util.h"
#include "watchdog.h"

static int watchdog_fd = -1;
static char *watchdog_device;
static usec_t watchdog_timeout;
static usec_t watchdog_last_ping;


static int disable_watchdog(void) {
        int flags = WDIOS_DISABLECARD;

        assert(watchdog_fd > 0);

        if (ioctl(watchdog_fd, WDIOC_SETOPTIONS, &flags) < 0)
                return log_warning_errno(errno, "Failed to disable hardware watchdog: %m");

        return 0;
}

static int gettimeout_watchdog(usec_t *usec) {
        int sec = 0;

        assert(watchdog_fd > 0);

        if (ioctl(watchdog_fd, WDIOC_GETTIMEOUT, &sec) < 0)
                return log_warning_errno(errno, "Failed to request watchdog HW timeout: %m");

        assert(sec > 0);
        *usec = sec * USEC_PER_SEC;

        return 0;
}

static int settimeout_watchdog(void) {
        usec_t t;
        int sec, r;

        assert(watchdog_fd > 0);
        assert(timestamp_is_set(watchdog_timeout));

        t = DIV_ROUND_UP(watchdog_timeout, USEC_PER_SEC);
        sec = MIN(t, (usec_t) INT_MAX); /* Saturate */

        r = ioctl(watchdog_fd, WDIOC_SETTIMEOUT, &sec);
        if (r < 0)
                return log_warning_errno(errno, "Failed to set timeout to %is: %m", sec);

        assert(sec > 0);/*  buggy driver ? */
        watchdog_timeout = sec * USEC_PER_SEC;

        log_info("Set hardware watchdog to %s.", FORMAT_TIMESPAN(watchdog_timeout, 0));
        return 0;
}

static int ping_watchdog(void) {
        assert(watchdog_fd > 0);

        if (ioctl(watchdog_fd, WDIOC_KEEPALIVE, 0) < 0)
                return log_warning_errno(errno, "Failed to ping hardware watchdog: %m");

        watchdog_last_ping = now(clock_boottime_or_monotonic());
        return 0;
}


static int update_timeout(void) {
        int r;

        assert(watchdog_timeout > 0);

        if (watchdog_fd < 0)
                return 0;

        if (watchdog_timeout != USEC_INFINITY) {
                r = settimeout_watchdog();
                if (r < 0) {
                        if (!ERRNO_IS_NOT_SUPPORTED(errno))
                                return r;

                        log_warning("Modifying the watchdog timeout is not supported by the HW");
                        watchdog_timeout = USEC_INFINITY;
                }
        }

        if (watchdog_timeout == USEC_INFINITY) {
                r = gettimeout_watchdog(&watchdog_timeout);
                if (r < 0)
                        return r;

                log_info("Calculating next watchdog pings based on the programmed timeout %s",
                         FORMAT_TIMESPAN(watchdog_timeout, 0));
        }

        return ping_watchdog();
}

static int open_watchdog(void) {
        struct watchdog_info ident;
        const char *fn;

        if (watchdog_fd >= 0)
                return 0;

        fn = watchdog_device ?: "/dev/watchdog";
        watchdog_fd = open(fn, O_WRONLY|O_CLOEXEC);
        if (watchdog_fd < 0)
                return log_debug_errno(errno, "Failed to open watchdog device %s, ignoring: %m", fn);

        if (ioctl(watchdog_fd, WDIOC_GETSUPPORT, &ident) < 0)
                log_debug_errno(errno, "Hardware watchdog %s does not support WDIOC_GETSUPPORT ioctl, ignoring: %m", fn);
        else
                log_info("Using hardware watchdog '%s', version %x, device %s",
                         ident.identity,
                         ident.firmware_version,
                         fn);

        return update_timeout();
}

int watchdog_set_device(const char *path) {
        int r;

        r = free_and_strdup(&watchdog_device, path);
        if (r < 0)
                return r;

        if (r > 0) /* watchdog_device changed */
                watchdog_fd = safe_close(watchdog_fd);

        return r;
}

int watchdog_setup(usec_t timeout) {

        /* Let's shortcut duplicated requests */
        if (watchdog_fd >= 0 && watchdog_timeout == timeout)
                return 0;

        if (timeout == 0) {
                watchdog_close(true);
                return 0;
        }

        /* Initialize the watchdog timeout with the caller value. This value is
         * going to be updated by update_timeout() with the closest value
         * supported by the driver */
        watchdog_timeout = timeout;

        if (watchdog_fd < 0)
                return open_watchdog();

        return update_timeout();
}

usec_t watchdog_runtime_wait(void) {

        if (!timestamp_is_set(watchdog_timeout))
                return USEC_INFINITY;

        /* Sleep half the watchdog timeout since the last successful ping at most */
        if (timestamp_is_set(watchdog_last_ping)) {
                usec_t ntime = now(clock_boottime_or_monotonic());

                assert(ntime >= watchdog_last_ping);
                return usec_sub_unsigned(watchdog_last_ping + (watchdog_timeout / 2), ntime);
        }

        return watchdog_timeout / 2;
}

int watchdog_ping(void) {
        usec_t ntime;

        if (watchdog_timeout == 0)
                return 0;

        if (watchdog_fd < 0)
                /* open_watchdog() will automatically ping the device for us if necessary */
                return open_watchdog();

        ntime = now(clock_boottime_or_monotonic());

        /* Never ping earlier than watchdog_timeout/4 and try to ping
         * by watchdog_timeout/2 plus scheduling latencies the latest */
        if (timestamp_is_set(watchdog_last_ping)) {
                assert(ntime >= watchdog_last_ping);
                if ((ntime - watchdog_last_ping) < (watchdog_timeout / 4))
                        return 0;
        }

        return ping_watchdog();
}

void watchdog_close(bool disarm) {

        /* Once closed, pinging the device becomes a NOP and we request a new
         * call to watchdog_setup() to open the device again. */
        watchdog_timeout = 0;

        if (watchdog_fd < 0)
                return;

        if (disarm) {
                (void) disable_watchdog();

                /* To be sure, use magic close logic, too */
                for (;;) {
                        static const char v = 'V';

                        if (write(watchdog_fd, &v, 1) > 0)
                                break;

                        if (errno != EINTR) {
                                log_warning_errno(errno, "Failed to disarm watchdog timer, ignoring: %m");
                                break;
                        }
                }
        }

        watchdog_fd = safe_close(watchdog_fd);
}
