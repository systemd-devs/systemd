/* SPDX-License-Identifier: LGPL-2.1+ */

#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-util.h"
#include "networkd-dhcp-server-bus.h"
#include "networkd-link-bus.h"
#include "networkd-manager.h"
#include "strv.h"

#include "dhcp-server-internal.h"

static int property_get_leases(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {
        Link *l = userdata;
        sd_dhcp_server *s;
        DHCPLease *lease;
        Iterator i;
        int r;

        assert(reply);
        assert(l);

        s = l->dhcp_server;
        if (!s)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Link %s has no DHCP server.", l->ifname);

        r = sd_bus_message_open_container(reply, 'a', "(ayayayayt)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH(lease, s->leases_by_client_id, i) {
                r = sd_bus_message_open_container(reply, 'r', "ayayayayt");
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', lease->client_id.data, lease->client_id.length);
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &lease->address, sizeof(lease->address));
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &lease->gateway, sizeof(lease->gateway));
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &lease->chaddr, sizeof(lease->chaddr));
                if (r < 0)
                        return r;

                r = sd_bus_message_append_basic(reply, 't', &lease->expiration);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int dhcp_server_send_changed(Link *link, const char *property, ...) {
        _cleanup_free_ char *path = NULL;
        char **l;

        assert(link);

        path = link_bus_path(link);
        if (!path)
                return log_oom();

        l = strv_from_stdarg_alloca(property);

        return sd_bus_emit_properties_changed_strv(
                        link->manager->bus,
                        path,
                        "org.freedesktop.network1.DHCPServer",
                        l);
}

static void on_leases_changed(sd_dhcp_server *s, void *data)
{
        Link *l = data;

        assert(l);
        (void) dhcp_server_send_changed(l, "Leases", NULL);
}

const sd_dhcp_server_cb dhcp_server_cb = {
        .on_leases_changed = on_leases_changed,
};

const sd_bus_vtable dhcp_server_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("Leases", "a(ayayayayt)", property_get_leases, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

        SD_BUS_VTABLE_END
};
