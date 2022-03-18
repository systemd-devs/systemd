/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/capability.h>

#include "sd-bus.h"

#include "alloc-util.h"
#include "bus-get-properties.h"
#include "bus-internal.h"
#include "bus-log-control-api.h"
#include "bus-polkit.h"
#include "bus-protocol.h"
#include "bus-util.h"
#include "dns-domain.h"
#include "in-addr-util.h"
#include "log.h"
#include "macro.h"
#include "strv.h"
#include "time-util.h"
#include "timesyncd-bus.h"
#include "user-util.h"

static int reply_server_names(ServerName *names, sd_bus_message *reply) {
        ServerName *p;
        int r;

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
                return r;

        LIST_FOREACH(names, p, names) {
                r = sd_bus_message_append(reply, "s", p->string);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_servers(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ServerName **s = userdata;

        assert(s);
        assert(bus);
        assert(reply);

        return reply_server_names(*s, reply);
}

static int property_get_runtime_servers(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Manager *m = userdata;

        assert(m);

        return reply_server_names(m->runtime_servers, reply);
}

static int property_set_runtime_servers(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *message,
                void *userdata,
                sd_bus_error *error) {

        _cleanup_strv_free_ char **msg_names = NULL;
        _cleanup_strv_free_ char **names = NULL;
        Manager *m = userdata;
        char **name;
        int r;

        assert(m);
        assert(bus);
        assert(message);

        r = bus_verify_polkit_async(message, CAP_NET_ADMIN,
                                    "org.freedesktop.timesyncd.set-runtime-servers",
                                    NULL, true, UID_INVALID,
                                    &m->polkit_registry, error);
        if (r < 0)
                return r;

        if (r == 0)
                /* Polkit will call us back */
                return 1;

        r = sd_bus_message_read_strv(message, &msg_names);
        if (r < 0)
                return r;

        while (m->runtime_servers)
                server_name_free(m->runtime_servers);

        STRV_FOREACH(name, msg_names) {
                r = dns_name_is_valid_or_address(*name);
                if (r < 0)
                        return log_error_errno(r, "Failed to check validity of NTP server name or address '%s': %m", *name);

                if (r == 0) {
                        log_error("Invalid NTP server name or address, ignoring: %s", *name);
                        continue;
                }

                strv_push(&names, *name);
        }

        STRV_FOREACH(name, names) {
                r = server_name_new(m, NULL, SERVER_RUNTIME, *name);
                if (r < 0)
                        return log_error_errno(r, "Failed to add runtime server '%s': %m", *name);
        }

        if (m->runtime_servers)
                m->runtime_servers_changed = true;

        if (manager_is_connected(m)) {
                if (m->current_server_address &&
                    m->current_server_address->name &&
                    strv_contains(names, m->current_server_address->name->string)) {
                        (void) manager_connect(m);
                }
        } else
                (void) manager_connect(m);

        return 0;
}

static int property_get_current_server_name(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ServerName **s = userdata;

        assert(s);
        assert(bus);
        assert(reply);

        return sd_bus_message_append(reply, "s", *s ? (*s)->string : NULL);
}

static int property_get_current_server_address(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ServerAddress *a;
        int r;

        assert(bus);
        assert(reply);
        assert(userdata);

        a = *(ServerAddress **) userdata;

        if (!a)
                return sd_bus_message_append(reply, "(iay)", AF_UNSPEC, 0);

        assert(IN_SET(a->sockaddr.sa.sa_family, AF_INET, AF_INET6));

        r = sd_bus_message_open_container(reply, 'r', "iay");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "i", a->sockaddr.sa.sa_family);
        if (r < 0)
                return r;

        r = sd_bus_message_append_array(reply, 'y',
                                        a->sockaddr.sa.sa_family == AF_INET ? (void*) &a->sockaddr.in.sin_addr : (void*) &a->sockaddr.in6.sin6_addr,
                                        FAMILY_ADDRESS_SIZE(a->sockaddr.sa.sa_family));
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static usec_t ntp_ts_short_to_usec(const struct ntp_ts_short *ts) {
        return be16toh(ts->sec) * USEC_PER_SEC + (be16toh(ts->frac) * USEC_PER_SEC) / (usec_t) 0x10000ULL;
}

static usec_t ntp_ts_to_usec(const struct ntp_ts *ts) {
        return (be32toh(ts->sec) - OFFSET_1900_1970) * USEC_PER_SEC + (be32toh(ts->frac) * USEC_PER_SEC) / (usec_t) 0x100000000ULL;
}

static int property_get_ntp_message(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        Manager *m = userdata;
        int r;

        assert(m);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'r', "uuuuittayttttbtt");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "uuuuitt",
                                  NTP_FIELD_LEAP(m->ntpmsg.field),
                                  NTP_FIELD_VERSION(m->ntpmsg.field),
                                  NTP_FIELD_MODE(m->ntpmsg.field),
                                  m->ntpmsg.stratum,
                                  m->ntpmsg.precision,
                                  ntp_ts_short_to_usec(&m->ntpmsg.root_delay),
                                  ntp_ts_short_to_usec(&m->ntpmsg.root_dispersion));
        if (r < 0)
                return r;

        r = sd_bus_message_append_array(reply, 'y', m->ntpmsg.refid, 4);
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "ttttbtt",
                                  timespec_load(&m->origin_time),
                                  ntp_ts_to_usec(&m->ntpmsg.recv_time),
                                  ntp_ts_to_usec(&m->ntpmsg.trans_time),
                                  timespec_load(&m->dest_time),
                                  m->spike,
                                  m->packet_count,
                                  (usec_t) (m->samples_jitter * USEC_PER_SEC));
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static const sd_bus_vtable manager_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("LinkNTPServers", "as", property_get_servers, offsetof(Manager, link_servers), 0),
        SD_BUS_PROPERTY("SystemNTPServers", "as", property_get_servers, offsetof(Manager, system_servers), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("FallbackNTPServers", "as", property_get_servers, offsetof(Manager, fallback_servers), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_WRITABLE_PROPERTY("RuntimeNTPServers", "as", property_get_runtime_servers, property_set_runtime_servers, 0, 0),
        SD_BUS_PROPERTY("ServerName", "s", property_get_current_server_name, offsetof(Manager, current_server_name), 0),
        SD_BUS_PROPERTY("ServerAddress", "(iay)", property_get_current_server_address, offsetof(Manager, current_server_address), 0),
        SD_BUS_PROPERTY("RootDistanceMaxUSec", "t", bus_property_get_usec, offsetof(Manager, root_distance_max_usec), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PollIntervalMinUSec", "t", bus_property_get_usec, offsetof(Manager, poll_interval_min_usec), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PollIntervalMaxUSec", "t", bus_property_get_usec, offsetof(Manager, poll_interval_max_usec), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PollIntervalUSec", "t", bus_property_get_usec, offsetof(Manager, poll_interval_usec), 0),
        SD_BUS_PROPERTY("NTPMessage", "(uuuuittayttttbtt)", property_get_ntp_message, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Frequency", "x", NULL, offsetof(Manager, drift_freq), 0),

        SD_BUS_VTABLE_END
};

int manager_connect_bus(Manager *m) {
        int r;

        assert(m);

        if (m->bus)
                return 0;

        r = bus_open_system_watch_bind_with_description(&m->bus, "bus-api-timesync");
        if (r < 0)
                return log_error_errno(r, "Failed to connect to bus: %m");

        r = sd_bus_add_object_vtable(m->bus, NULL, "/org/freedesktop/timesync1", "org.freedesktop.timesync1.Manager", manager_vtable, m);
        if (r < 0)
                return log_error_errno(r, "Failed to add manager object vtable: %m");

        r = bus_log_control_api_register(m->bus);
        if (r < 0)
                return r;

        r = sd_bus_request_name_async(m->bus, NULL, "org.freedesktop.timesync1", 0, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to request name: %m");

        r = sd_bus_attach_event(m->bus, m->event, 0);
        if (r < 0)
                return log_error_errno(r, "Failed to attach bus to event loop: %m");

        return 0;
}
