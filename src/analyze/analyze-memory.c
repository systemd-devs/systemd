/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-bus.h"

#include "analyze-memory.h"
#include "analyze.h"
#include "bus-error.h"
#include "bus-internal.h"

static int dump_malloc_info(sd_bus *bus, char *service) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        int r;

        assert(bus);
        assert(service);

        r = sd_bus_call_method(bus,
                               service,
                               "/org/freedesktop/MemoryAllocation1",
                               "org.freedesktop.MemoryAllocation1",
                               "GetMallocInfo",
                               &error,
                               &reply,
                               "",
                               NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to call GetMallocInfo on '%s': %s", service, bus_error_message(&error, r));

        return dump_fd_reply(reply);
}

int verb_memory(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_strv_free_ char **services = NULL;
        int r;

        STRV_FOREACH(arg, strv_skip(argv, 1)) {
                if (!service_name_is_valid(*arg))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "D-Bus service name '%s' is not valid.", *arg);

                r = strv_extend(&services, *arg);
                if (r < 0)
                        return log_oom();
        }

        if (strv_isempty(services)) {
                r = strv_extend(&services, "org.freedesktop.systemd1");
                if (r < 0)
                        return log_oom();
        }

        r = acquire_bus(&bus, NULL);
        if (r < 0)
                return bus_log_connect_error(r, arg_transport);

        pager_open(arg_pager_flags);

        r = sd_bus_can_send(bus, SD_BUS_TYPE_UNIX_FD);
        if (r < 0)
                return log_error_errno(r, "Unable to determine if bus connection supports fd passing: %m");
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Unable to receive FDs over D-Bus.");

        STRV_FOREACH(service, services) {
                r = dump_malloc_info(bus, *service);
                if (r < 0)
                        return r;
        }

        return EXIT_SUCCESS;
}
