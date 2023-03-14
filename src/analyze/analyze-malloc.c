/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-bus.h"

#include "analyze-malloc.h"
#include "analyze.h"
#include "bus-error.h"
#include "bus-internal.h"
#include "bus-locator.h"

static int dump_malloc_info(sd_bus *bus, char *service) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        int r;

        assert(bus);
        assert(service);

        const BusLocator* const bus_host = &(BusLocator){
        .destination = service,
        .path = "/org/freedesktop/MemoryAllocation1",
        .interface = "org.freedesktop.MemoryAllocation1"
};

        r= bus_call_method(bus,
                           bus_host,
                           "GetMallocInfo",
                           &error,
                           &reply,
                           NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to call GetMallocInfo on '%s': %s", service, bus_error_message(&error, r));

        return dump_fd_reply(reply);
}

int verb_malloc(int argc, char *argv[], void *userdata) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        char **services = STRV_MAKE("org.freedesktop.systemd1");
        int r;

        if (!strv_isempty(strv_skip(argv, 1))) {
                services = strv_skip(argv, 1);
                STRV_FOREACH(service, services)
                        if (!service_name_is_valid(*service))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "D-Bus service name '%s' is not valid.", *service);
        }

        r = acquire_bus(&bus, NULL);
        if (r < 0)
                return bus_log_connect_error(r, arg_transport);

        r = sd_bus_can_send(bus, SD_BUS_TYPE_UNIX_FD);
        if (r < 0)
                return log_error_errno(r, "Unable to determine if bus connection supports fd passing: %m");
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Unable to receive FDs over D-Bus.");

        pager_open(arg_pager_flags);

        STRV_FOREACH(service, services) {
                r = dump_malloc_info(bus, *service);
                if (r < 0)
                        return r;
        }

        return EXIT_SUCCESS;
}
