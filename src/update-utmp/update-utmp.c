/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if HAVE_AUDIT
#include <libaudit.h>
#endif

#include "sd-bus.h"

#include "alloc-util.h"
#include "bus-error.h"
#include "bus-locator.h"
#include "bus-util.h"
#include "format-util.h"
#include "log.h"
#include "macro.h"
#include "main-func.h"
#include "process-util.h"
#include "special.h"
#include "stdio-util.h"
#include "strv.h"
#include "unit-name.h"
#include "utmp-wtmp.h"
#include "verbs.h"

typedef struct Context {
        sd_bus *bus;
#if HAVE_AUDIT
        int audit_fd;
#endif
} Context;

static void context_clear(Context *c) {
        assert(c);

        c->bus = sd_bus_flush_close_unref(c->bus);
#if HAVE_AUDIT
        if (c->audit_fd >= 0)
                audit_close(c->audit_fd);
        c->audit_fd = -EBADF;
#endif
}

static usec_t get_startup_monotonic_time(Context *c, usec_t *ret) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        assert(c);
        assert(ret);

        r = bus_get_property_trivial(
                        c->bus,
                        bus_systemd_mgr,
                        "UserspaceTimestampMonotonic",
                        &error,
                        't', ret);
        if (r < 0)
                return log_warning_errno(r, "Failed to get timestamp, ignoring: %s", bus_error_message(&error, r));

        return 0;
}

static int get_current_runlevel(Context *c) {
        static const struct {
                const int runlevel;
                const char *special;
        } table[] = {
                /* The first target of this list that is active or has a job scheduled wins. We prefer
                 * runlevels 5 and 3 here over the others, since these are the main runlevels used on Fedora.
                 * It might make sense to change the order on some distributions. */
                { '5', SPECIAL_GRAPHICAL_TARGET  },
                { '3', SPECIAL_MULTI_USER_TARGET },
                { '1', SPECIAL_RESCUE_TARGET     },
        };

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        assert(c);

        for (size_t i = 0; i < ELEMENTSOF(table); i++) {
                _cleanup_free_ char *state = NULL, *path = NULL;

                path = unit_dbus_path_from_name(table[i].special);
                if (!path)
                        return log_oom();

                r = sd_bus_get_property_string(
                                c->bus,
                                "org.freedesktop.systemd1",
                                path,
                                "org.freedesktop.systemd1.Unit",
                                "ActiveState",
                                &error,
                                &state);
                if (sd_bus_error_has_names(&error,
                                           SD_BUS_ERROR_NO_REPLY,
                                           SD_BUS_ERROR_DISCONNECTED)) {
                        /* systemd might have dropped off momentarily, let's not make this an error. */
                        log_debug_errno(r, "Failed to get state of %s, ignoring: %s", table[i].special, bus_error_message(&error, r));
                        continue;
                }
                if (r < 0)
                        return log_warning_errno(r, "Failed to get state of %s: %s", table[i].special, bus_error_message(&error, r));

                if (STR_IN_SET(state, "active", "reloading"))
                        return table[i].runlevel;
        }

        return 0;
}

static int on_reboot(int argc, char *argv[], void *userdata) {
        Context *c = ASSERT_PTR(userdata);
        usec_t t = 0, boottime;
        int r, q = 0;

        assert(c);

        /* We finished start-up, so let's write the utmp record and send the audit msg. */

#if HAVE_AUDIT
        if (c->audit_fd >= 0)
                if (audit_log_user_comm_message(c->audit_fd, AUDIT_SYSTEM_BOOT, "", "systemd-update-utmp", NULL, NULL, NULL, 1) < 0 &&
                    errno != EPERM)
                        q = log_error_errno(errno, "Failed to send audit message: %m");
#endif

        /* If this call fails, then utmp_put_reboot() will fix to the current time. */
        (void) get_startup_monotonic_time(c, &t);
        boottime = map_clock_usec(t, CLOCK_MONOTONIC, CLOCK_REALTIME);
        /* We query the recorded monotonic time here (instead of the system clock CLOCK_REALTIME), even
         * though we actually want the system clock time. That's because there's a likely chance that the
         * system clock wasn't set right during early boot. By manually converting the monotonic clock to the
         * system clock here we can compensate for incorrectly set clocks during early boot. */

        r = utmp_put_reboot(boottime);
        if (r < 0)
                return log_error_errno(r, "Failed to write utmp record: %m");

        return q;
}

static int on_shutdown(int argc, char *argv[], void *userdata) {
        Context *c = ASSERT_PTR(userdata);
        int r, q = 0;

        assert(c);

        /* We started shut-down, so let's write the utmp record and send the audit msg. */

#if HAVE_AUDIT
        if (c->audit_fd >= 0)
                if (audit_log_user_comm_message(c->audit_fd, AUDIT_SYSTEM_SHUTDOWN, "", "systemd-update-utmp", NULL, NULL, NULL, 1) < 0 &&
                    errno != EPERM)
                        q = log_error_errno(errno, "Failed to send audit message: %m");
#endif

        r = utmp_put_shutdown();
        if (r < 0)
                return log_error_errno(r, "Failed to write utmp record: %m");

        return q;
}

static int on_runlevel(int argc, char *argv[], void *userdata) {
        Context *c = ASSERT_PTR(userdata);
        int r, q = 0, previous, runlevel;

        assert(c);

        /* We finished changing runlevel, so let's write the utmp record and send the audit msg. */

        /* First, get last runlevel */
        r = utmp_get_runlevel(&previous, NULL);

        if (r < 0) {
                if (!IN_SET(r, -ESRCH, -ENOENT))
                        return log_error_errno(r, "Failed to get current runlevel: %m");

                previous = 0;
        }

        /* Secondly, get new runlevel */
        runlevel = get_current_runlevel(c);
        if (runlevel < 0)
                return runlevel;
        if (runlevel == 0) {
                log_warning("Failed to get new runlevel, utmp update skipped.");
                return 0;
        }

        if (previous == runlevel)
                return 0;

#if HAVE_AUDIT
        if (c->audit_fd >= 0) {
                char s[STRLEN("old-level=_ new-level=_") + 1];

                xsprintf(s, "old-level=%c new-level=%c",
                         previous > 0 ? previous : 'N',
                         runlevel);

                if (audit_log_user_comm_message(c->audit_fd, AUDIT_SYSTEM_RUNLEVEL, s,
                                                "systemd-update-utmp", NULL, NULL, NULL, 1) < 0 && errno != EPERM)
                        q = log_error_errno(errno, "Failed to send audit message: %m");
        }
#endif

        r = utmp_put_runlevel(runlevel, previous);
        if (r < 0 && !IN_SET(r, -ESRCH, -ENOENT))
                return log_error_errno(r, "Failed to write utmp record: %m");

        return q;
}

static int run(int argc, char *argv[]) {
        static const Verb verbs[] = {
                { "reboot",   1, 1, 0, on_reboot   },
                { "shutdown", 1, 1, 0, on_shutdown },
                { "runlevel", 1, 1, 0, on_runlevel },
                {}
        };

        _cleanup_(context_clear) Context c = {
#if HAVE_AUDIT
                .audit_fd = -EBADF,
#endif
        };
        int r;

        log_setup();

        umask(0022);

#if HAVE_AUDIT
        /* If the kernel lacks netlink or audit support, don't worry about it. */
        c.audit_fd = audit_open();
        if (c.audit_fd < 0)
                log_full_errno(IN_SET(errno, EAFNOSUPPORT, EPROTONOSUPPORT) ? LOG_DEBUG : LOG_WARNING,
                               errno, "Failed to connect to audit log, ignoring: %m");
#endif
        r = bus_connect_system_systemd(&c.bus);
        if (r < 0)
                return log_error_errno(r, "Failed to get D-Bus connection: %m");

        return dispatch_verb(argc, argv, verbs, &c);
}

DEFINE_MAIN_FUNCTION(run);
