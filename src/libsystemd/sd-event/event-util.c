/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>

#include "event-source.h"
#include "event-util.h"
#include "fd-util.h"
#include "log.h"
#include "string-util.h"

int event_reset_time(
                sd_event *e,
                sd_event_source **s,
                clockid_t clock,
                uint64_t usec,
                uint64_t accuracy,
                sd_event_time_handler_t callback,
                void *userdata,
                int64_t priority,
                const char *description,
                bool force_reset) {

        bool created = false;
        int enabled, r;
        clockid_t c;

        assert(e);
        assert(s);

        if (*s) {
                if (!force_reset) {
                        r = sd_event_source_get_enabled(*s, &enabled);
                        if (r < 0)
                                return log_debug_errno(r, "sd-event: Failed to query whether event source \"%s\" is enabled or not: %m",
                                                       strna((*s)->description ?: description));

                        if (enabled != SD_EVENT_OFF)
                                return 0;
                }

                r = sd_event_source_get_time_clock(*s, &c);
                if (r < 0)
                        return log_debug_errno(r, "sd-event: Failed to get clock id of event source \"%s\": %m", strna((*s)->description ?: description));

                if (c != clock)
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "sd-event: Current clock id %i of event source \"%s\" is different from specified one %i.",
                                               (int)c,
                                               strna((*s)->description ?: description),
                                               (int)clock);

                r = sd_event_source_set_time(*s, usec);
                if (r < 0)
                        return log_debug_errno(r, "sd-event: Failed to set time for event source \"%s\": %m", strna((*s)->description ?: description));

                r = sd_event_source_set_time_accuracy(*s, accuracy);
                if (r < 0)
                        return log_debug_errno(r, "sd-event: Failed to set accuracy for event source \"%s\": %m", strna((*s)->description ?: description));

                /* callback function is not updated, as we do not have sd_event_source_set_time_callback(). */

                (void) sd_event_source_set_userdata(*s, userdata);

                r = sd_event_source_set_enabled(*s, SD_EVENT_ONESHOT);
                if (r < 0)
                        return log_debug_errno(r, "sd-event: Failed to enable event source \"%s\": %m", strna((*s)->description ?: description));
        } else {
                r = sd_event_add_time(e, s, clock, usec, accuracy, callback, userdata);
                if (r < 0)
                        return log_debug_errno(r, "sd-event: Failed to create timer event \"%s\": %m", strna(description));

                created = true;
        }

        r = sd_event_source_set_priority(*s, priority);
        if (r < 0)
                return log_debug_errno(r, "sd-event: Failed to set priority for event source \"%s\": %m", strna((*s)->description ?: description));

        if (description) {
                r = sd_event_source_set_description(*s, description);
                if (r < 0)
                        return log_debug_errno(r, "sd-event: Failed to set description for event source \"%s\": %m", description);
        }

        return created;
}

int event_reset_time_relative(
                sd_event *e,
                sd_event_source **s,
                clockid_t clock,
                uint64_t usec,
                uint64_t accuracy,
                sd_event_time_handler_t callback,
                void *userdata,
                int64_t priority,
                const char *description,
                bool force_reset) {

        int r;

        assert(e);

        if (usec > 0) {
                usec_t usec_now;

                r = sd_event_now(e, clock, &usec_now);
                if (r < 0)
                        return log_debug_errno(r, "sd-event: Failed to get the current time: %m");

                usec = usec_add(usec_now, usec);
        }

        return event_reset_time(e, s, clock, usec, accuracy, callback, userdata, priority, description, force_reset);
}

int event_add_time_change(sd_event *e, sd_event_source **ret, sd_event_io_handler_t callback, void *userdata) {
        _cleanup_(sd_event_source_unrefp) sd_event_source *s = NULL;
        _cleanup_close_ int fd = -EBADF;
        int r;

        assert(e);

        /* Allocates an IO event source that gets woken up whenever the clock changes. Needs to be recreated on each event */

        fd = time_change_fd();
        if (fd < 0)
                return fd;

        r = sd_event_add_io(e, &s, fd, EPOLLIN, callback, userdata);
        if (r < 0)
                return r;

        r = sd_event_source_set_io_fd_own(s, true);
        if (r < 0)
                return r;

        TAKE_FD(fd);

        r = sd_event_source_set_description(s, "time-change");
        if (r < 0)
                return r;

        if (ret)
                *ret = TAKE_PTR(s);
        else {
                r = sd_event_source_set_floating(s, true);
                if (r < 0)
                        return r;
        }

        return 0;
}

int event_add_child_pidref(
                sd_event *e,
                sd_event_source **s,
                const PidRef *pid,
                int options,
                sd_event_child_handler_t callback,
                void *userdata) {

        if (!pidref_is_set(pid))
                return -ESRCH;

        if (pid->fd >= 0)
                return sd_event_add_child_pidfd(e, s, pid->fd, options, callback, userdata);

        return sd_event_add_child(e, s, pid->pid, options, callback, userdata);
}

dual_timestamp* event_dual_timestamp_now(sd_event *e, dual_timestamp *ts) {
        assert(e);
        assert(ts);

        assert_se(sd_event_now(e, CLOCK_REALTIME, &ts->realtime) >= 0);
        assert_se(sd_event_now(e, CLOCK_MONOTONIC, &ts->monotonic) >= 0);
        return ts;
}

void event_source_unref_many(sd_event_source **array, size_t n) {
        FOREACH_ARRAY(v, array, n)
                sd_event_source_unref(*v);

        free(array);
}

static int event_forward_signal_callback(sd_event_source *s, const struct signalfd_siginfo *ssi, void *userdata) {
        sd_event_source *child = ASSERT_PTR(userdata);

        assert(ssi);

        siginfo_t si = {
                .si_signo = ssi->ssi_signo,
                /* This has to be SI_QUEUE as SI_USER and SI_KERNEL are not allowed to be used. */
                .si_code = SI_QUEUE,
                .si_errno = ssi->ssi_errno,
        };

        si.si_pid = ssi->ssi_pid;
        si.si_uid = ssi->ssi_uid;
        si.si_int = ssi->ssi_int;
        si.si_ptr = UINT64_TO_PTR(ssi->ssi_ptr);

        return sd_event_source_send_child_signal(child, ssi->ssi_signo, &si, /* flags = */ 0);
}

static void event_forward_signal_destroy(void *userdata) {
        sd_event_source *child = ASSERT_PTR(userdata);
        sd_event_source_unref(child);
}

int event_forward_signals(
                sd_event *e,
                sd_event_source *child,
                const int *signals,
                size_t n_signals,
                sd_event_source ***ret_sources,
                size_t *ret_n_sources) {

        sd_event_source **sources = NULL;
        size_t n_sources = 0;
        int r;

        CLEANUP_ARRAY(sources, n_sources, event_source_unref_many);

        assert(e);
        assert(child);
        assert(child->type == SOURCE_CHILD);
        assert(signals || n_signals == 0);
        assert(ret_sources);
        assert(ret_n_sources);

        if (n_signals == 0) {
                *ret_sources = NULL;
                *ret_n_sources = 0;
                return 0;
        }

        sources = new0(sd_event_source*, n_signals);
        if (!sources)
                return -ENOMEM;

        FOREACH_ARRAY(sig, signals, n_signals) {
                r = sd_event_add_signal(e, &sources[n_sources], *sig | SD_EVENT_SIGNAL_PROCMASK, event_forward_signal_callback, child);
                if (r < 0)
                        return r;

                r = sd_event_source_set_destroy_callback(sources[n_sources], event_forward_signal_destroy);
                if (r < 0)
                        return r;

                sd_event_source_ref(child);
                n_sources++;
        }

        *ret_sources = TAKE_PTR(sources);
        *ret_n_sources = n_sources;

        return 0;
}
