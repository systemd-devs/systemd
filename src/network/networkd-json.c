/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "netif-util.h"
#include "networkd-address.h"
#include "networkd-json.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-network.h"
#include "networkd-route-util.h"
#include "sort-util.h"

static int address_build_json(Address *address, JsonVariant **ret) {
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
        _cleanup_free_ char *scope = NULL, *flags = NULL, *state = NULL;
        usec_t usec_now;
        int r;

        assert(address);
        assert(address->link);
        assert(address->link->manager);
        assert(address->link->manager->event);
        assert(ret);

        r = sd_event_now(address->link->manager->event, clock_boottime_or_monotonic(), &usec_now);
        if (r < 0)
                return r;

        r = route_scope_to_string_alloc(address->scope, &scope);
        if (r < 0)
                return r;

        r = address_flags_to_string_alloc(address->flags, address->family, &flags);
        if (r < 0)
                return r;

        r = network_config_state_to_string_alloc(address->state, &state);
        if (r < 0)
                return r;

        r = json_build(&v, JSON_BUILD_OBJECT(
                                JSON_BUILD_PAIR_INTEGER("Family", address->family),
                                JSON_BUILD_PAIR_IN_ADDR("Address", &address->in_addr, address->family),
                                JSON_BUILD_PAIR_IN_ADDR_NON_NULL("Peer", &address->in_addr_peer, address->family),
                                JSON_BUILD_PAIR_IN4_ADDR_NON_NULL("Broadcast", &address->broadcast),
                                JSON_BUILD_PAIR_UNSIGNED("PrefixLength", address->prefixlen),
                                JSON_BUILD_PAIR_UNSIGNED("Scope", address->scope),
                                JSON_BUILD_PAIR_STRING("ScopeString", scope),
                                JSON_BUILD_PAIR_UNSIGNED("Flags", address->flags),
                                JSON_BUILD_PAIR_STRING("FlagsString", flags),
                                JSON_BUILD_PAIR_STRING_NON_EMPTY("Label", address->label),
                                JSON_BUILD_PAIR_FINITE_USEC("PreferredLifetimeUsec", address->lifetime_preferred_usec),
                                JSON_BUILD_PAIR_FINITE_TIMESPAN("PreferredLifetimeString",
                                                                usec_sub_unsigned(address->lifetime_preferred_usec, usec_now),
                                                                USEC_PER_SEC),
                                JSON_BUILD_PAIR_FINITE_USEC("ValidLifetimeUsec", address->lifetime_valid_usec),
                                JSON_BUILD_PAIR_FINITE_TIMESPAN("ValidLifetimeString",
                                                                usec_sub_unsigned(address->lifetime_valid_usec, usec_now),
                                                                USEC_PER_SEC),
                                JSON_BUILD_PAIR_STRING("ConfigSource", network_config_source_to_string(address->source)),
                                JSON_BUILD_PAIR_STRING("ConfigState", state),
                                JSON_BUILD_PAIR_IN_ADDR_NON_NULL("ConfigProvider", &address->provider, address->family)));
        if (r < 0)
                return r;

        *ret = TAKE_PTR(v);
        return 0;
}

static int addresses_build_json(Set *addresses, JsonVariant **ret) {
        JsonVariant **elements;
        Address *address;
        size_t n = 0;
        int r;

        assert(ret);

        if (set_isempty(addresses)) {
                *ret = NULL;
                return 0;
        }

        elements = new(JsonVariant*, set_size(addresses));
        if (!elements)
                return -ENOMEM;

        SET_FOREACH(address, addresses) {
                r = address_build_json(address, elements + n);
                if (r < 0)
                        goto finalize;
                n++;
        }

        r = json_build(ret, JSON_BUILD_OBJECT(JSON_BUILD_PAIR("Addresses", JSON_BUILD_VARIANT_ARRAY(elements, n))));

finalize:
        json_variant_unref_many(elements, n);
        free(elements);
        return r;
}

static int network_build_json(Network *network, JsonVariant **ret) {
        assert(ret);

        if (!network) {
                *ret = NULL;
                return 0;
        }

        return json_build(ret, JSON_BUILD_OBJECT(
                                JSON_BUILD_PAIR_STRING("NetworkFile", network->filename)));
}

static int device_build_json(sd_device *device, JsonVariant **ret) {
        const char *link = NULL, *path = NULL, *vendor = NULL, *model = NULL;

        assert(ret);

        if (!device) {
                *ret = NULL;
                return 0;
        }

        (void) sd_device_get_property_value(device, "ID_NET_LINK_FILE", &link);
        (void) sd_device_get_property_value(device, "ID_PATH", &path);

        if (sd_device_get_property_value(device, "ID_VENDOR_FROM_DATABASE", &vendor) < 0)
                (void) sd_device_get_property_value(device, "ID_VENDOR", &vendor);

        if (sd_device_get_property_value(device, "ID_MODEL_FROM_DATABASE", &model) < 0)
                (void) sd_device_get_property_value(device, "ID_MODEL", &model);

        return json_build(ret, JSON_BUILD_OBJECT(
                                JSON_BUILD_PAIR_STRING_NON_EMPTY("LinkFile", link),
                                JSON_BUILD_PAIR_STRING_NON_EMPTY("Path", path),
                                JSON_BUILD_PAIR_STRING_NON_EMPTY("Vendor", vendor),
                                JSON_BUILD_PAIR_STRING_NON_EMPTY("Model", model)));
}

int link_build_json(Link *link, JsonVariant **ret) {
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL, *w = NULL;
        _cleanup_free_ char *type = NULL;
        int r;

        assert(link);
        assert(ret);

        r = net_get_type_string(link->sd_device, link->iftype, &type);
        if (r == -ENOMEM)
                return r;

        r = json_build(&v, JSON_BUILD_OBJECT(
                                JSON_BUILD_PAIR_INTEGER("Index", link->ifindex),
                                JSON_BUILD_PAIR_STRING("Name", link->ifname),
                                JSON_BUILD_PAIR_STRV_NON_EMPTY("AlternativeNames", link->alternative_names),
                                JSON_BUILD_PAIR_STRING("Type", type),
                                JSON_BUILD_PAIR_STRING_NON_EMPTY("Driver", link->driver),
                                JSON_BUILD_PAIR_STRING("SetupState", link_state_to_string(link->state)),
                                JSON_BUILD_PAIR_STRING("OperationalState", link_operstate_to_string(link->operstate)),
                                JSON_BUILD_PAIR_STRING("CarrierState", link_carrier_state_to_string(link->carrier_state)),
                                JSON_BUILD_PAIR_STRING("AddressState", link_address_state_to_string(link->address_state)),
                                JSON_BUILD_PAIR_STRING("IPv4AddressState", link_address_state_to_string(link->ipv4_address_state)),
                                JSON_BUILD_PAIR_STRING("IPv6AddressState", link_address_state_to_string(link->ipv6_address_state)),
                                JSON_BUILD_PAIR_STRING("OnlineState", link_online_state_to_string(link->online_state))));
        if (r < 0)
                return r;

        r = network_build_json(link->network, &w);
        if (r < 0)
                return r;

        r = json_variant_merge(&v, w);
        if (r < 0)
                return r;

        w = json_variant_unref(w);

        r = device_build_json(link->sd_device, &w);
        if (r < 0)
                return r;

        r = json_variant_merge(&v, w);
        if (r < 0)
                return r;

        w = json_variant_unref(w);

        r = addresses_build_json(link->addresses, &w);
        if (r < 0)
                return r;

        r = json_variant_merge(&v, w);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(v);
        return 0;
}

static int link_json_compare(JsonVariant * const *a, JsonVariant * const *b) {
        intmax_t index_a, index_b;

        assert(a && *a);
        assert(b && *b);

        index_a = json_variant_integer(json_variant_by_key(*a, "Index"));
        index_b = json_variant_integer(json_variant_by_key(*b, "Index"));

        return CMP(index_a, index_b);
}

static int links_build_json(Manager *manager, JsonVariant **ret) {
        JsonVariant **elements;
        Link *link;
        size_t n = 0;
        int r;

        assert(manager);
        assert(ret);

        elements = new(JsonVariant*, hashmap_size(manager->links_by_index));
        if (!elements)
                return -ENOMEM;

        HASHMAP_FOREACH(link, manager->links_by_index) {
                r = link_build_json(link, elements + n);
                if (r < 0)
                        goto finalize;
                n++;
        }

        typesafe_qsort(elements, n, link_json_compare);

        r = json_build(ret, JSON_BUILD_OBJECT(JSON_BUILD_PAIR("Interfaces", JSON_BUILD_VARIANT_ARRAY(elements, n))));

finalize:
        json_variant_unref_many(elements, n);
        free(elements);
        return r;
}

int manager_build_json(Manager *manager, JsonVariant **ret) {
        return links_build_json(manager, ret);
}
