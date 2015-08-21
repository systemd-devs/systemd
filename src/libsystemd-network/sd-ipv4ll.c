/***
  This file is part of systemd.

  Copyright (C) 2014 Axis Communications AB. All rights reserved.
  Copyright (C) 2015 Tom Gundersen

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

#include "event-util.h"
#include "list.h"
#include "random-util.h"
#include "refcnt.h"
#include "siphash24.h"
#include "sparse-endian.h"
#include "util.h"

#include "sd-ipv4acd.h"
#include "sd-ipv4ll.h"

#define IPV4LL_NETWORK 0xA9FE0000L
#define IPV4LL_NETMASK 0xFFFF0000L

#define log_ipv4ll(ll, fmt, ...) log_internal(LOG_DEBUG, 0, __FILE__, __LINE__, __func__, "IPv4LL: " fmt, ##__VA_ARGS__)

#define IPV4LL_DONT_DESTROY(ll) \
        _cleanup_ipv4ll_unref_ _unused_ sd_ipv4ll *_dont_destroy_##ll = sd_ipv4ll_ref(ll)

struct sd_ipv4ll {
        unsigned n_ref;

        sd_ipv4acd *acd;
        be32_t address;
        struct random_data *random_data;
        char *random_data_state;
        /* External */
        be32_t claimed_address;
        sd_event *event;
        int event_priority;
        sd_ipv4ll_cb_t cb;
        void* userdata;
};

sd_ipv4ll *sd_ipv4ll_ref(sd_ipv4ll *ll) {
        if (!ll)
                return NULL;

        assert(ll->n_ref >= 1);
        ll->n_ref++;

        return ll;
}

sd_ipv4ll *sd_ipv4ll_unref(sd_ipv4ll *ll) {
        if (!ll)
                return NULL;

        assert(ll->n_ref >= 1);
        ll->n_ref--;

        if (ll->n_ref > 0)
                return NULL;

        sd_ipv4ll_detach_event(ll);

        sd_ipv4acd_unref(ll->acd);

        free(ll->random_data);
        free(ll->random_data_state);
        free(ll);

        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(sd_ipv4ll*, sd_ipv4ll_unref);
#define _cleanup_ipv4ll_unref_ _cleanup_(sd_ipv4ll_unrefp)

static void ipv4ll_on_acd(sd_ipv4acd *ll, int event, void *userdata);

int sd_ipv4ll_new(sd_ipv4ll **ret) {
        _cleanup_ipv4ll_unref_ sd_ipv4ll *ll = NULL;
        int r;

        assert_return(ret, -EINVAL);

        ll = new0(sd_ipv4ll, 1);
        if (!ll)
                return -ENOMEM;

        r = sd_ipv4acd_new(&ll->acd);
        if (r < 0)
                return r;

        r = sd_ipv4acd_set_callback(ll->acd, ipv4ll_on_acd, ll);
        if (r < 0)
                return r;

        ll->n_ref = 1;

        *ret = ll;
        ll = NULL;

        return 0;
}

static void ipv4ll_client_notify(sd_ipv4ll *ll, int event) {
        assert(ll);

        if (ll->cb)
                ll->cb(ll, event, ll->userdata);
}

int sd_ipv4ll_stop(sd_ipv4ll *ll) {
        int r;

        assert_return(ll, -EINVAL);

        r = sd_ipv4acd_stop(ll->acd);
        if (r < 0)
                return r;

        ll->claimed_address = 0;
        ll->address = 0;

        return 0;
}

static int ipv4ll_pick_address(sd_ipv4ll *ll, be32_t *address) {
        be32_t addr;
        int r;
        int32_t random;

        assert(ll);
        assert(address);
        assert(ll->random_data);

        do {
                r = random_r(ll->random_data, &random);
                if (r < 0)
                        return r;
                addr = htonl((random & 0x0000FFFF) | IPV4LL_NETWORK);
        } while (addr == ll->address ||
                (ntohl(addr) & IPV4LL_NETMASK) != IPV4LL_NETWORK ||
                (ntohl(addr) & 0x0000FF00) == 0x0000 ||
                (ntohl(addr) & 0x0000FF00) == 0xFF00);

        *address = addr;
        return 0;
}

int sd_ipv4ll_set_index(sd_ipv4ll *ll, int interface_index) {
        assert_return(ll, -EINVAL);

        return sd_ipv4acd_set_index(ll->acd, interface_index);
}

#define HASH_KEY SD_ID128_MAKE(df,04,22,98,3f,ad,14,52,f9,87,2e,d1,9c,70,e2,f2)

int sd_ipv4ll_set_mac(sd_ipv4ll *ll, const struct ether_addr *addr) {
        int r;

        assert_return(ll, -EINVAL);

        if (!ll->random_data) {
                uint8_t seed[8];

                /* If no random data is set, generate some from the MAC */
                siphash24(seed, &addr->ether_addr_octet,
                          ETH_ALEN, HASH_KEY.bytes);

                r = sd_ipv4ll_set_address_seed(ll, seed);
                if (r < 0)
                        return r;
        }

        return sd_ipv4acd_set_mac(ll->acd, addr);
}

int sd_ipv4ll_detach_event(sd_ipv4ll *ll) {
        assert_return(ll, -EINVAL);

        sd_ipv4acd_detach_event(ll->acd);

        ll->event = sd_event_unref(ll->event);

        return 0;
}

int sd_ipv4ll_attach_event(sd_ipv4ll *ll, sd_event *event, int priority) {
        _cleanup_event_unref_ sd_event *e = NULL;
        int r;

        assert_return(ll, -EINVAL);
        assert_return(!ll->event, -EBUSY);

        if (event)
                e = sd_event_ref(event);
        else {
                r = sd_event_default(&e);
                if (r < 0) {
                        sd_ipv4ll_stop(ll);
                        return r;
                }
        }

        r = sd_ipv4acd_attach_event(ll->acd, event, priority);
        if (r < 0)
                return r;

        ll->event_priority = priority;
        ll->event = e;
        e = NULL;

        return 0;
}

int sd_ipv4ll_set_callback(sd_ipv4ll *ll, sd_ipv4ll_cb_t cb, void *userdata) {
        assert_return(ll, -EINVAL);

        ll->cb = cb;
        ll->userdata = userdata;

        return 0;
}

int sd_ipv4ll_get_address(sd_ipv4ll *ll, struct in_addr *address){
        assert_return(ll, -EINVAL);
        assert_return(address, -EINVAL);

        if (ll->claimed_address == 0) {
                return -ENOENT;
        }

        address->s_addr = ll->claimed_address;
        return 0;
}

int sd_ipv4ll_set_address_seed(sd_ipv4ll *ll, uint8_t seed[8]) {
        unsigned int entropy;
        int r;

        assert_return(ll, -EINVAL);
        assert_return(seed, -EINVAL);

        entropy = *seed;

        free(ll->random_data);
        free(ll->random_data_state);

        ll->random_data = new0(struct random_data, 1);
        ll->random_data_state = new0(char, 128);

        if (!ll->random_data || !ll->random_data_state) {
                r = -ENOMEM;
                goto error;
        }

        r = initstate_r((unsigned int)entropy, ll->random_data_state, 128, ll->random_data);
        if (r < 0)
                goto error;

error:
        if (r < 0){
                free(ll->random_data);
                free(ll->random_data_state);
                ll->random_data = NULL;
                ll->random_data_state = NULL;
        }
        return r;
}

bool sd_ipv4ll_is_running(sd_ipv4ll *ll) {
        assert_return(ll, false);

        return sd_ipv4acd_is_running(ll->acd);
}

int sd_ipv4ll_start(sd_ipv4ll *ll) {
        struct in_addr addr;
        int r;

        assert_return(ll, -EINVAL);
        assert_return(ll->event, -EINVAL);
        assert_return(ll->random_data, -EINVAL);
        assert_return(ll->address == 0, -EBUSY);

        r = ipv4ll_pick_address(ll, &ll->address);
        if (r < 0)
                goto out;

        addr.s_addr = ll->address;

        r = sd_ipv4acd_set_address(ll->acd, &addr);
        if (r < 0)
                goto out;

        r = sd_ipv4acd_start(ll->acd);
out:
        if (r < 0) {
                sd_ipv4ll_stop(ll);
                return r;
        }

        return 0;
}

void ipv4ll_on_acd(sd_ipv4acd *acd, int event, void *userdata) {
        sd_ipv4ll *ll = userdata;
        IPV4LL_DONT_DESTROY(ll);
        int r = 0;

        assert(acd);
        assert(ll);

        switch (event) {
        case IPV4ACD_EVENT_STOP:
                ipv4ll_client_notify(ll, IPV4LL_EVENT_STOP);
                ll->claimed_address = 0;

                break;
        case IPV4ACD_EVENT_BIND:
                ll->claimed_address = ll->address;
                ipv4ll_client_notify(ll, IPV4LL_EVENT_BIND);

                break;
        case IPV4ACD_EVENT_CONFLICT:
                ipv4ll_client_notify(ll, IPV4LL_EVENT_CONFLICT);

                sd_ipv4ll_stop(ll);

                /* Pick a new address */
                r = ipv4ll_pick_address(ll, &ll->address);
                if (r < 0)
                        goto out;

                r = sd_ipv4ll_start(ll);
                if (r < 0)
                        goto out;

                break;
        default:
                assert_not_reached("Invalid IPv4ACD event.");
        }

out:
        if (r < 0)
                ipv4ll_client_notify(ll, IPV4LL_EVENT_STOP);
}
