/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/wait.h>
#include <unistd.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "operation.h"
#include "process-util.h"

static int read_operation_errno(const siginfo_t *si, Operation *o) {
        int r;

        assert(si);
        assert(o);

        if (si->si_code != CLD_EXITED)
                return log_debug_errno(SYNTHETIC_ERRNO(ESHUTDOWN), "Child died abnormally");

        if (si->si_status == EXIT_SUCCESS)
                r = 0;
        else {
                ssize_t n = read(o->errno_fd, &r, sizeof(r));
                if (n < 0)
                        return log_debug_errno(errno, "Failed to read operation's errno: %m");
                if (n != sizeof(r))
                        return log_debug_errno(SYNTHETIC_ERRNO(EIO), "Received unexpectedly short message when reading operation's errno");
        }

        return r;
}

static int operation_done(sd_event_source *s, const siginfo_t *si, void *userdata) {
        Operation *o = ASSERT_PTR(userdata);
        int r;

        assert(si);

        log_debug("Operation " PID_FMT " is now complete with code=%s status=%i",
                  o->pid,
                  sigchld_code_to_string(si->si_code), si->si_status);

        o->pid = 0;

        r = read_operation_errno(si, o);
        if (r < 0)
                log_debug_errno(r, "Operation failed: %m");

        /* If a completion routine (o->done) is set for this operation, call it. It sends a response, but can return an error in which case it expect us to reply.
         * Otherwise, the default action is to simply return an error on failure or an empty success message on success. */

        if (o->message) {
                _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
                if (o->done)
                        r = o->done(o, r, &error);

                if (r < 0) {
                        if (!sd_bus_error_is_set(&error))
                                sd_bus_error_set_errno(&error, r);

                        r = sd_bus_reply_method_error(o->message, &error);
                        if (r < 0)
                                log_error_errno(r, "Failed to reply to dbus message: %m");
                } else if (!o->done) {
                        /* when o->done set it's responsible for sending reply in a happy-path case */
                        r = sd_bus_reply_method_return(o->message, NULL);
                        if (r < 0)
                                log_error_errno(r, "Failed to reply to dbus message: %m");
                }
        } else if (o->link) {
                if (o->done)
                        r = o->done(o, r, /* error = */ NULL);

                if (r < 0)
                        (void) sd_varlink_error_errno(o->link, r);
                else if (!o->done)
                        /* when o->done set it's responsible for sending reply in a happy-path case */
                        (void) sd_varlink_reply(o->link, NULL);
        } else
                assert_not_reached();

        operation_free(o);
        return 0;
}

int operation_new(Manager *manager, Machine *machine, pid_t child, sd_bus_message *message, sd_varlink *link, int errno_fd, Operation **ret) {
        Operation *o;
        int r;

        assert(manager);
        assert(child > 1);
        assert(errno_fd >= 0);
        assert(message || link);
        assert(!(message && link));

        o = new0(Operation, 1);
        if (!o)
                return -ENOMEM;

        o->extra_fd = -EBADF;

        r = sd_event_add_child(manager->event, &o->event_source, child, WEXITED, operation_done, o);
        if (r < 0) {
                free(o);
                return r;
        }

        o->pid = child;
        o->message = sd_bus_message_ref(message);
        o->link = sd_varlink_ref(link);
        o->errno_fd = errno_fd;

        LIST_PREPEND(operations, manager->operations, o);
        manager->n_operations++;
        o->manager = manager;

        if (machine) {
                LIST_PREPEND(operations_by_machine, machine->operations, o);
                o->machine = machine;
        }

        log_debug("Started new operation " PID_FMT ".", child);

        /* At this point we took ownership of both the child and the errno file descriptor! */

        if (ret)
                *ret = o;

        return 0;
}

Operation *operation_free(Operation *o) {
        if (!o)
                return NULL;

        sd_event_source_unref(o->event_source);

        safe_close(o->errno_fd);
        safe_close(o->extra_fd);

        if (o->pid > 1)
                (void) sigkill_wait(o->pid);

        sd_bus_message_unref(o->message);
        sd_varlink_unref(o->link);

        if (o->manager) {
                LIST_REMOVE(operations, o->manager->operations, o);
                o->manager->n_operations--;
        }

        if (o->machine)
                LIST_REMOVE(operations_by_machine, o->machine->operations, o);

        return mfree(o);
}

_noreturn_ void report_errno_and_exit(int errno_fd, int r) {
        if (r >= 0)
                _exit(EXIT_SUCCESS);

        assert(errno_fd >= 0);

        ssize_t n = write(errno_fd, &r, sizeof(r));
        if (n < 0)
                log_debug_errno(errno, "Failed to write operation's errno: %m");
        if (n != sizeof(r))
                log_debug_errno(SYNTHETIC_ERRNO(EIO), "Sent unexpectedly short message");

        _exit(EXIT_FAILURE);
}
