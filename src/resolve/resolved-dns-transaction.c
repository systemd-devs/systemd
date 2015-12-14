/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

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

#include "af-list.h"
#include "alloc-util.h"
#include "dns-domain.h"
#include "fd-util.h"
#include "random-util.h"
#include "resolved-dns-cache.h"
#include "resolved-dns-transaction.h"
#include "resolved-llmnr.h"
#include "string-table.h"

DnsTransaction* dns_transaction_free(DnsTransaction *t) {
        DnsQueryCandidate *c;
        DnsZoneItem *i;
        DnsTransaction *z;

        if (!t)
                return NULL;

        sd_event_source_unref(t->timeout_event_source);

        dns_packet_unref(t->sent);
        dns_packet_unref(t->received);

        dns_answer_unref(t->answer);

        sd_event_source_unref(t->dns_udp_event_source);
        safe_close(t->dns_udp_fd);

        dns_server_unref(t->server);
        dns_stream_free(t->stream);

        if (t->scope) {
                hashmap_remove_value(t->scope->transactions_by_key, t->key, t);
                LIST_REMOVE(transactions_by_scope, t->scope->transactions, t);

                if (t->id != 0)
                        hashmap_remove(t->scope->manager->dns_transactions, UINT_TO_PTR(t->id));
        }

        dns_resource_key_unref(t->key);

        while ((c = set_steal_first(t->notify_query_candidates)))
                set_remove(c->transactions, t);
        set_free(t->notify_query_candidates);

        while ((i = set_steal_first(t->notify_zone_items)))
                i->probe_transaction = NULL;
        set_free(t->notify_zone_items);

        while ((z = set_steal_first(t->notify_transactions)))
                set_remove(z->dnssec_transactions, t);
        set_free(t->notify_transactions);

        while ((z = set_steal_first(t->dnssec_transactions))) {
                set_remove(z->notify_transactions, t);
                dns_transaction_gc(z);
        }
        set_free(t->dnssec_transactions);

        dns_answer_unref(t->validated_keys);

        free(t);
        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(DnsTransaction*, dns_transaction_free);

void dns_transaction_gc(DnsTransaction *t) {
        assert(t);

        if (t->block_gc > 0)
                return;

        if (set_isempty(t->notify_query_candidates) &&
            set_isempty(t->notify_zone_items) &&
            set_isempty(t->notify_transactions))
                dns_transaction_free(t);
}

int dns_transaction_new(DnsTransaction **ret, DnsScope *s, DnsResourceKey *key) {
        _cleanup_(dns_transaction_freep) DnsTransaction *t = NULL;
        int r;

        assert(ret);
        assert(s);
        assert(key);

        /* Don't allow looking up invalid or pseudo RRs */
        if (!dns_type_is_valid_query(key->type))
                return -EINVAL;

        /* We only support the IN class */
        if (key->class != DNS_CLASS_IN && key->class != DNS_CLASS_ANY)
                return -EOPNOTSUPP;

        r = hashmap_ensure_allocated(&s->manager->dns_transactions, NULL);
        if (r < 0)
                return r;

        r = hashmap_ensure_allocated(&s->transactions_by_key, &dns_resource_key_hash_ops);
        if (r < 0)
                return r;

        t = new0(DnsTransaction, 1);
        if (!t)
                return -ENOMEM;

        t->dns_udp_fd = -1;
        t->answer_source = _DNS_TRANSACTION_SOURCE_INVALID;
        t->dnssec_result = _DNSSEC_RESULT_INVALID;
        t->key = dns_resource_key_ref(key);

        /* Find a fresh, unused transaction id */
        do
                random_bytes(&t->id, sizeof(t->id));
        while (t->id == 0 ||
               hashmap_get(s->manager->dns_transactions, UINT_TO_PTR(t->id)));

        r = hashmap_put(s->manager->dns_transactions, UINT_TO_PTR(t->id), t);
        if (r < 0) {
                t->id = 0;
                return r;
        }

        r = hashmap_replace(s->transactions_by_key, t->key, t);
        if (r < 0) {
                hashmap_remove(s->manager->dns_transactions, UINT_TO_PTR(t->id));
                return r;
        }

        LIST_PREPEND(transactions_by_scope, s->transactions, t);
        t->scope = s;

        if (ret)
                *ret = t;

        t = NULL;

        return 0;
}

static void dns_transaction_stop(DnsTransaction *t) {
        assert(t);

        t->timeout_event_source = sd_event_source_unref(t->timeout_event_source);
        t->stream = dns_stream_free(t->stream);

        /* Note that we do not drop the UDP socket here, as we want to
         * reuse it to repeat the interaction. */
}

static void dns_transaction_tentative(DnsTransaction *t, DnsPacket *p) {
        _cleanup_free_ char *pretty = NULL;
        DnsZoneItem *z;

        assert(t);
        assert(p);

        if (manager_our_packet(t->scope->manager, p) != 0)
                return;

        in_addr_to_string(p->family, &p->sender, &pretty);

        log_debug("Transaction on scope %s on %s/%s got tentative packet from %s",
                  dns_protocol_to_string(t->scope->protocol),
                  t->scope->link ? t->scope->link->name : "*",
                  t->scope->family == AF_UNSPEC ? "*" : af_to_name(t->scope->family),
                  pretty);

        /* RFC 4795, Section 4.1 says that the peer with the
         * lexicographically smaller IP address loses */
        if (memcmp(&p->sender, &p->destination, FAMILY_ADDRESS_SIZE(p->family)) >= 0) {
                log_debug("Peer has lexicographically larger IP address and thus lost in the conflict.");
                return;
        }

        log_debug("We have the lexicographically larger IP address and thus lost in the conflict.");

        t->block_gc++;
        while ((z = set_first(t->notify_zone_items))) {
                /* First, make sure the zone item drops the reference
                 * to us */
                dns_zone_item_probe_stop(z);

                /* Secondly, report this as conflict, so that we might
                 * look for a different hostname */
                dns_zone_item_conflict(z);
        }
        t->block_gc--;

        dns_transaction_gc(t);
}

void dns_transaction_complete(DnsTransaction *t, DnsTransactionState state) {
        DnsQueryCandidate *c;
        DnsZoneItem *z;
        DnsTransaction *d;
        Iterator i;

        assert(t);
        assert(!DNS_TRANSACTION_IS_LIVE(state));

        /* Note that this call might invalidate the query. Callers
         * should hence not attempt to access the query or transaction
         * after calling this function. */

        log_debug("Transaction on scope %s on %s/%s now complete with <%s> from %s",
                  dns_protocol_to_string(t->scope->protocol),
                  t->scope->link ? t->scope->link->name : "*",
                  t->scope->family == AF_UNSPEC ? "*" : af_to_name(t->scope->family),
                  dns_transaction_state_to_string(state),
                  t->answer_source < 0 ? "none" : dns_transaction_source_to_string(t->answer_source));

        t->state = state;

        dns_transaction_stop(t);

        /* Notify all queries that are interested, but make sure the
         * transaction isn't freed while we are still looking at it */
        t->block_gc++;
        SET_FOREACH(c, t->notify_query_candidates, i)
                dns_query_candidate_notify(c);
        SET_FOREACH(z, t->notify_zone_items, i)
                dns_zone_item_notify(z);
        SET_FOREACH(d, t->notify_transactions, i)
                dns_transaction_notify(d, t);
        t->block_gc--;

        dns_transaction_gc(t);
}

static int on_stream_complete(DnsStream *s, int error) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        DnsTransaction *t;

        assert(s);
        assert(s->transaction);

        /* Copy the data we care about out of the stream before we
         * destroy it. */
        t = s->transaction;
        p = dns_packet_ref(s->read_packet);

        t->stream = dns_stream_free(t->stream);

        if (error != 0) {
                dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                return 0;
        }

        if (dns_packet_validate_reply(p) <= 0) {
                log_debug("Invalid TCP reply packet.");
                dns_transaction_complete(t, DNS_TRANSACTION_INVALID_REPLY);
                return 0;
        }

        dns_scope_check_conflicts(t->scope, p);

        t->block_gc++;
        dns_transaction_process_reply(t, p);
        t->block_gc--;

        /* If the response wasn't useful, then complete the transition now */
        if (t->state == DNS_TRANSACTION_PENDING)
                dns_transaction_complete(t, DNS_TRANSACTION_INVALID_REPLY);

        return 0;
}

static int dns_transaction_open_tcp(DnsTransaction *t) {
        DnsServer *server = NULL;
        _cleanup_close_ int fd = -1;
        int r;

        assert(t);

        if (t->stream)
                return 0;

        switch (t->scope->protocol) {
        case DNS_PROTOCOL_DNS:
                fd = dns_scope_tcp_socket(t->scope, AF_UNSPEC, NULL, 53, &server);
                break;

        case DNS_PROTOCOL_LLMNR:
                /* When we already received a reply to this (but it was truncated), send to its sender address */
                if (t->received)
                        fd = dns_scope_tcp_socket(t->scope, t->received->family, &t->received->sender, t->received->sender_port, NULL);
                else {
                        union in_addr_union address;
                        int family = AF_UNSPEC;

                        /* Otherwise, try to talk to the owner of a
                         * the IP address, in case this is a reverse
                         * PTR lookup */

                        r = dns_name_address(DNS_RESOURCE_KEY_NAME(t->key), &family, &address);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                return -EINVAL;
                        if (family != t->scope->family)
                                return -ESRCH;

                        fd = dns_scope_tcp_socket(t->scope, family, &address, LLMNR_PORT, NULL);
                }

                break;

        default:
                return -EAFNOSUPPORT;
        }

        if (fd < 0)
                return fd;

        r = dns_stream_new(t->scope->manager, &t->stream, t->scope->protocol, fd);
        if (r < 0)
                return r;

        fd = -1;

        r = dns_stream_write_packet(t->stream, t->sent);
        if (r < 0) {
                t->stream = dns_stream_free(t->stream);
                return r;
        }

        dns_server_unref(t->server);
        t->server = dns_server_ref(server);
        t->received = dns_packet_unref(t->received);
        t->answer = dns_answer_unref(t->answer);
        t->n_answer_cacheable = 0;
        t->answer_rcode = 0;
        t->stream->complete = on_stream_complete;
        t->stream->transaction = t;

        /* The interface index is difficult to determine if we are
         * connecting to the local host, hence fill this in right away
         * instead of determining it from the socket */
        if (t->scope->link)
                t->stream->ifindex = t->scope->link->ifindex;

        return 0;
}

static void dns_transaction_next_dns_server(DnsTransaction *t) {
        assert(t);

        t->server = dns_server_unref(t->server);
        t->dns_udp_event_source = sd_event_source_unref(t->dns_udp_event_source);
        t->dns_udp_fd = safe_close(t->dns_udp_fd);

        dns_scope_next_dns_server(t->scope);
}

static void dns_transaction_cache_answer(DnsTransaction *t) {
        assert(t);

        /* For mDNS we cache whenever we get the packet, rather than
         * in each transaction. */
        if (!IN_SET(t->scope->protocol, DNS_PROTOCOL_DNS, DNS_PROTOCOL_LLMNR))
                return;

        /* We never cache if this packet is from the local host, under
         * the assumption that a locally running DNS server would
         * cache this anyway, and probably knows better when to flush
         * the cache then we could. */
        if (!DNS_PACKET_SHALL_CACHE(t->received))
                return;

        dns_cache_put(&t->scope->cache,
                      t->key,
                      t->answer_rcode,
                      t->answer,
                      t->n_answer_cacheable,
                      t->answer_authenticated,
                      0,
                      t->received->family,
                      &t->received->sender);
}

static void dns_transaction_process_dnssec(DnsTransaction *t) {
        int r;

        assert(t);

        /* Are there ongoing DNSSEC transactions? If so, let's wait for them. */
        if (!set_isempty(t->dnssec_transactions))
                return;

        /* All our auxiliary DNSSEC transactions are complete now. Try
         * to validate our RRset now. */
        r = dns_transaction_validate_dnssec(t);
        if (r < 0) {
                dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                return;
        }

        if (!IN_SET(t->dnssec_result, _DNSSEC_RESULT_INVALID, DNSSEC_VALIDATED, DNSSEC_NO_SIGNATURE /* FOR NOW! */)) {
                dns_transaction_complete(t, DNS_TRANSACTION_DNSSEC_FAILED);
                return;
        }

        dns_transaction_cache_answer(t);

        if (t->answer_rcode == DNS_RCODE_SUCCESS)
                dns_transaction_complete(t, DNS_TRANSACTION_SUCCESS);
        else
                dns_transaction_complete(t, DNS_TRANSACTION_FAILURE);
}

void dns_transaction_process_reply(DnsTransaction *t, DnsPacket *p) {
        usec_t ts;
        int r;

        assert(t);
        assert(p);
        assert(t->state == DNS_TRANSACTION_PENDING);
        assert(t->scope);
        assert(t->scope->manager);

        /* Note that this call might invalidate the query. Callers
         * should hence not attempt to access the query or transaction
         * after calling this function. */

        log_debug("Processing incoming packet on transaction %" PRIu16".", t->id);

        switch (t->scope->protocol) {

        case DNS_PROTOCOL_LLMNR:
                assert(t->scope->link);

                /* For LLMNR we will not accept any packets from other
                 * interfaces */

                if (p->ifindex != t->scope->link->ifindex)
                        return;

                if (p->family != t->scope->family)
                        return;

                /* Tentative packets are not full responses but still
                 * useful for identifying uniqueness conflicts during
                 * probing. */
                if (DNS_PACKET_LLMNR_T(p)) {
                        dns_transaction_tentative(t, p);
                        return;
                }

                break;

        case DNS_PROTOCOL_MDNS:
                assert(t->scope->link);

                /* For mDNS we will not accept any packets from other interfaces */
                if (p->ifindex != t->scope->link->ifindex)
                        return;

                if (p->family != t->scope->family)
                        return;

                break;

        case DNS_PROTOCOL_DNS:
                break;

        default:
                assert_not_reached("Invalid DNS protocol.");
        }

        if (t->received != p) {
                dns_packet_unref(t->received);
                t->received = dns_packet_ref(p);
        }

        t->answer_source = DNS_TRANSACTION_NETWORK;

        if (p->ipproto == IPPROTO_TCP) {
                if (DNS_PACKET_TC(p)) {
                        /* Truncated via TCP? Somebody must be fucking with us */
                        dns_transaction_complete(t, DNS_TRANSACTION_INVALID_REPLY);
                        return;
                }

                if (DNS_PACKET_ID(p) != t->id) {
                        /* Not the reply to our query? Somebody must be fucking with us */
                        dns_transaction_complete(t, DNS_TRANSACTION_INVALID_REPLY);
                        return;
                }
        }

        assert_se(sd_event_now(t->scope->manager->event, clock_boottime_or_monotonic(), &ts) >= 0);

        switch (t->scope->protocol) {

        case DNS_PROTOCOL_DNS:
                assert(t->server);

                if (IN_SET(DNS_PACKET_RCODE(p), DNS_RCODE_FORMERR, DNS_RCODE_SERVFAIL, DNS_RCODE_NOTIMP)) {

                        /* Request failed, immediately try again with reduced features */
                        log_debug("Server returned error: %s", dns_rcode_to_string(DNS_PACKET_RCODE(p)));

                        dns_server_packet_failed(t->server, t->current_features);

                        r = dns_transaction_go(t);
                        if (r < 0) {
                                dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                                return;
                        }

                        return;
                } else
                        dns_server_packet_received(t->server, t->current_features, ts - t->start_usec, p->size);

                break;

        case DNS_PROTOCOL_LLMNR:
        case DNS_PROTOCOL_MDNS:
                dns_scope_packet_received(t->scope, ts - t->start_usec);
                break;

        default:
                assert_not_reached("Invalid DNS protocol.");
        }

        if (DNS_PACKET_TC(p)) {

                /* Truncated packets for mDNS are not allowed. Give up immediately. */
                if (t->scope->protocol == DNS_PROTOCOL_MDNS) {
                        dns_transaction_complete(t, DNS_TRANSACTION_INVALID_REPLY);
                        return;
                }

                /* Response was truncated, let's try again with good old TCP */
                r = dns_transaction_open_tcp(t);
                if (r == -ESRCH) {
                        /* No servers found? Damn! */
                        dns_transaction_complete(t, DNS_TRANSACTION_NO_SERVERS);
                        return;
                }
                if (r < 0) {
                        /* On LLMNR, if we cannot connect to the host,
                         * we immediately give up */
                        if (t->scope->protocol == DNS_PROTOCOL_LLMNR) {
                                dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                                return;
                        }

                        /* On DNS, couldn't send? Try immediately again, with a new server */
                        dns_transaction_next_dns_server(t);

                        r = dns_transaction_go(t);
                        if (r < 0) {
                                dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                                return;
                        }

                        return;
                }
        }

        /* Parse message, if it isn't parsed yet. */
        r = dns_packet_extract(p);
        if (r < 0) {
                dns_transaction_complete(t, DNS_TRANSACTION_INVALID_REPLY);
                return;
        }

        if (IN_SET(t->scope->protocol, DNS_PROTOCOL_DNS, DNS_PROTOCOL_LLMNR)) {

                /* Only consider responses with equivalent query section to the request */
                r = dns_packet_is_reply_for(p, t->key);
                if (r < 0) {
                        dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                        return;
                }
                if (r == 0) {
                        dns_transaction_complete(t, DNS_TRANSACTION_INVALID_REPLY);
                        return;
                }

                /* Install the answer as answer to the transaction */
                dns_answer_unref(t->answer);
                t->answer = dns_answer_ref(p->answer);
                t->answer_rcode = DNS_PACKET_RCODE(p);
                t->answer_authenticated = t->scope->dnssec_mode == DNSSEC_TRUST && DNS_PACKET_AD(p);

                /* According to RFC 4795, section 2.9. only the RRs
                 * from the answer section shall be cached. However,
                 * if we know the message is authenticated, we might
                 * as well cache everything. */
                if (t->answer_authenticated)
                        t->n_answer_cacheable = (unsigned) -1; /* everything! */
                else
                        t->n_answer_cacheable = DNS_PACKET_ANCOUNT(t->received); /* only the answer section */

                r = dns_transaction_request_dnssec_keys(t);
                if (r < 0) {
                        dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                        return;
                }
                if (r > 0) {
                        /* There are DNSSEC transactions pending now. Update the state accordingly. */
                        t->state = DNS_TRANSACTION_VALIDATING;
                        return;
                }
        }

        dns_transaction_process_dnssec(t);
}

static int on_dns_packet(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        DnsTransaction *t = userdata;
        int r;

        assert(t);
        assert(t->scope);

        r = manager_recv(t->scope->manager, fd, DNS_PROTOCOL_DNS, &p);
        if (r <= 0)
                return r;

        if (dns_packet_validate_reply(p) > 0 &&
            DNS_PACKET_ID(p) == t->id)
                dns_transaction_process_reply(t, p);
        else
                log_debug("Invalid DNS packet, ignoring.");

        return 0;
}

static int dns_transaction_emit(DnsTransaction *t) {
        int r;

        assert(t);

        if (t->scope->protocol == DNS_PROTOCOL_DNS && !t->server) {
                DnsServer *server = NULL;
                _cleanup_close_ int fd = -1;

                fd = dns_scope_udp_dns_socket(t->scope, &server);
                if (fd < 0)
                        return fd;

                r = sd_event_add_io(t->scope->manager->event, &t->dns_udp_event_source, fd, EPOLLIN, on_dns_packet, t);
                if (r < 0)
                        return r;

                t->dns_udp_fd = fd;
                fd = -1;
                t->server = dns_server_ref(server);
        }

        r = dns_scope_emit(t->scope, t->dns_udp_fd, t->server, t->sent);
        if (r < 0)
                return r;

        if (t->server)
                t->current_features = t->server->possible_features;

        return 0;
}

static int on_transaction_timeout(sd_event_source *s, usec_t usec, void *userdata) {
        DnsTransaction *t = userdata;
        int r;

        assert(s);
        assert(t);

        if (!t->initial_jitter_scheduled || t->initial_jitter_elapsed) {
                /* Timeout reached? Increase the timeout for the server used */
                switch (t->scope->protocol) {
                case DNS_PROTOCOL_DNS:
                        assert(t->server);

                        dns_server_packet_lost(t->server, t->current_features, usec - t->start_usec);

                        break;
                case DNS_PROTOCOL_LLMNR:
                case DNS_PROTOCOL_MDNS:
                        dns_scope_packet_lost(t->scope, usec - t->start_usec);

                        break;
                default:
                        assert_not_reached("Invalid DNS protocol.");
                }

                if (t->initial_jitter_scheduled)
                        t->initial_jitter_elapsed = true;
        }

        /* ...and try again with a new server */
        dns_transaction_next_dns_server(t);

        r = dns_transaction_go(t);
        if (r < 0)
                dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);

        return 0;
}

static usec_t transaction_get_resend_timeout(DnsTransaction *t) {
        assert(t);
        assert(t->scope);

        switch (t->scope->protocol) {
        case DNS_PROTOCOL_DNS:
                assert(t->server);

                return t->server->resend_timeout;
        case DNS_PROTOCOL_MDNS:
                assert(t->n_attempts > 0);
                return (1 << (t->n_attempts - 1)) * USEC_PER_SEC;
        case DNS_PROTOCOL_LLMNR:
                return t->scope->resend_timeout;
        default:
                assert_not_reached("Invalid DNS protocol.");
        }
}

static int dns_transaction_prepare(DnsTransaction *t, usec_t ts) {
        bool had_stream;
        int r;

        assert(t);

        had_stream = !!t->stream;

        dns_transaction_stop(t);

        if (t->n_attempts >= TRANSACTION_ATTEMPTS_MAX(t->scope->protocol)) {
                dns_transaction_complete(t, DNS_TRANSACTION_ATTEMPTS_MAX_REACHED);
                return 0;
        }

        if (t->scope->protocol == DNS_PROTOCOL_LLMNR && had_stream) {
                /* If we already tried via a stream, then we don't
                 * retry on LLMNR. See RFC 4795, Section 2.7. */
                dns_transaction_complete(t, DNS_TRANSACTION_ATTEMPTS_MAX_REACHED);
                return 0;
        }

        t->n_attempts++;
        t->start_usec = ts;
        t->received = dns_packet_unref(t->received);
        t->answer = dns_answer_unref(t->answer);
        t->n_answer_cacheable = 0;
        t->answer_rcode = 0;
        t->answer_source = _DNS_TRANSACTION_SOURCE_INVALID;

        /* Check the trust anchor. Do so only on classic DNS, since DNSSEC does not apply otherwise. */
        if (t->scope->protocol == DNS_PROTOCOL_DNS) {
                r = dns_trust_anchor_lookup(&t->scope->manager->trust_anchor, t->key, &t->answer);
                if (r < 0)
                        return r;
                if (r > 0) {
                        t->answer_rcode = DNS_RCODE_SUCCESS;
                        t->answer_source = DNS_TRANSACTION_TRUST_ANCHOR;
                        t->answer_authenticated = true;
                        dns_transaction_complete(t, DNS_TRANSACTION_SUCCESS);
                        return 0;
                }
        }

        /* Check the zone, but only if this transaction is not used
         * for probing or verifying a zone item. */
        if (set_isempty(t->notify_zone_items)) {

                r = dns_zone_lookup(&t->scope->zone, t->key, &t->answer, NULL, NULL);
                if (r < 0)
                        return r;
                if (r > 0) {
                        t->answer_rcode = DNS_RCODE_SUCCESS;
                        t->answer_source = DNS_TRANSACTION_ZONE;
                        t->answer_authenticated = true;
                        dns_transaction_complete(t, DNS_TRANSACTION_SUCCESS);
                        return 0;
                }
        }

        /* Check the cache, but only if this transaction is not used
         * for probing or verifying a zone item. */
        if (set_isempty(t->notify_zone_items)) {

                /* Before trying the cache, let's make sure we figured out a
                 * server to use. Should this cause a change of server this
                 * might flush the cache. */
                dns_scope_get_dns_server(t->scope);

                /* Let's then prune all outdated entries */
                dns_cache_prune(&t->scope->cache);

                r = dns_cache_lookup(&t->scope->cache, t->key, &t->answer_rcode, &t->answer, &t->answer_authenticated);
                if (r < 0)
                        return r;
                if (r > 0) {
                        t->answer_source = DNS_TRANSACTION_CACHE;
                        if (t->answer_rcode == DNS_RCODE_SUCCESS)
                                dns_transaction_complete(t, DNS_TRANSACTION_SUCCESS);
                        else
                                dns_transaction_complete(t, DNS_TRANSACTION_FAILURE);
                        return 0;
                }
        }

        return 1;
}

static int dns_transaction_make_packet_mdns(DnsTransaction *t) {

        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        bool add_known_answers = false;
        DnsTransaction *other;
        unsigned qdcount;
        usec_t ts;
        int r;

        assert(t);
        assert(t->scope->protocol == DNS_PROTOCOL_MDNS);

        /* Discard any previously prepared packet, so we can start over and coalesce again */
        t->sent = dns_packet_unref(t->sent);

        r = dns_packet_new_query(&p, t->scope->protocol, 0, false);
        if (r < 0)
                return r;

        r = dns_packet_append_key(p, t->key, NULL);
        if (r < 0)
                return r;

        qdcount = 1;

        if (dns_key_is_shared(t->key))
                add_known_answers = true;

        /*
         * For mDNS, we want to coalesce as many open queries in pending transactions into one single
         * query packet on the wire as possible. To achieve that, we iterate through all pending transactions
         * in our current scope, and see whether their timing contraints allow them to be sent.
         */

        assert_se(sd_event_now(t->scope->manager->event, clock_boottime_or_monotonic(), &ts) >= 0);

        LIST_FOREACH(transactions_by_scope, other, t->scope->transactions) {

                /* Skip ourselves */
                if (other == t)
                        continue;

                if (other->state != DNS_TRANSACTION_PENDING)
                        continue;

                if (other->next_attempt_after > ts)
                        continue;

                if (qdcount >= UINT16_MAX)
                        break;

                r = dns_packet_append_key(p, other->key, NULL);

                /*
                 * If we can't stuff more questions into the packet, just give up.
                 * One of the 'other' transactions will fire later and take care of the rest.
                 */
                if (r == -EMSGSIZE)
                        break;

                if (r < 0)
                        return r;

                r = dns_transaction_prepare(other, ts);
                if (r <= 0)
                        continue;

                ts += transaction_get_resend_timeout(other);

                r = sd_event_add_time(
                                other->scope->manager->event,
                                &other->timeout_event_source,
                                clock_boottime_or_monotonic(),
                                ts, 0,
                                on_transaction_timeout, other);
                if (r < 0)
                        return r;

                other->state = DNS_TRANSACTION_PENDING;
                other->next_attempt_after = ts;

                qdcount ++;

                if (dns_key_is_shared(other->key))
                        add_known_answers = true;
        }

        DNS_PACKET_HEADER(p)->qdcount = htobe16(qdcount);

        /* Append known answer section if we're asking for any shared record */
        if (add_known_answers) {
                r = dns_cache_export_shared_to_packet(&t->scope->cache, p);
                if (r < 0)
                        return r;
        }

        t->sent = p;
        p = NULL;

        return 0;
}

static int dns_transaction_make_packet(DnsTransaction *t) {
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        int r;

        assert(t);

        if (t->scope->protocol == DNS_PROTOCOL_MDNS)
                return dns_transaction_make_packet_mdns(t);

        if (t->sent)
                return 0;

        r = dns_packet_new_query(&p, t->scope->protocol, 0, t->scope->dnssec_mode == DNSSEC_YES);
        if (r < 0)
                return r;

        r = dns_scope_good_key(t->scope, t->key);
        if (r < 0)
                return r;
        if (r == 0)
                return -EDOM;

        r = dns_packet_append_key(p, t->key, NULL);
        if (r < 0)
                return r;

        DNS_PACKET_HEADER(p)->qdcount = htobe16(1);
        DNS_PACKET_HEADER(p)->id = t->id;

        t->sent = p;
        p = NULL;

        return 0;
}

int dns_transaction_go(DnsTransaction *t) {
        usec_t ts;
        int r;

        assert(t);

        assert_se(sd_event_now(t->scope->manager->event, clock_boottime_or_monotonic(), &ts) >= 0);

        r = dns_transaction_prepare(t, ts);
        if (r <= 0)
                return r;

        if (log_get_max_level() >= LOG_DEBUG) {
                _cleanup_free_ char *ks = NULL;

                (void) dns_resource_key_to_string(t->key, &ks);

                log_debug("Excercising transaction for <%s> on scope %s on %s/%s",
                          ks ? strstrip(ks) : "???",
                          dns_protocol_to_string(t->scope->protocol),
                          t->scope->link ? t->scope->link->name : "*",
                          t->scope->family == AF_UNSPEC ? "*" : af_to_name(t->scope->family));
        }

        if (!t->initial_jitter_scheduled &&
            (t->scope->protocol == DNS_PROTOCOL_LLMNR ||
             t->scope->protocol == DNS_PROTOCOL_MDNS)) {
                usec_t jitter, accuracy;

                /* RFC 4795 Section 2.7 suggests all queries should be
                 * delayed by a random time from 0 to JITTER_INTERVAL. */

                t->initial_jitter_scheduled = true;

                random_bytes(&jitter, sizeof(jitter));

                switch (t->scope->protocol) {
                case DNS_PROTOCOL_LLMNR:
                        jitter %= LLMNR_JITTER_INTERVAL_USEC;
                        accuracy = LLMNR_JITTER_INTERVAL_USEC;
                        break;
                case DNS_PROTOCOL_MDNS:
                        jitter %= MDNS_JITTER_RANGE_USEC;
                        jitter += MDNS_JITTER_MIN_USEC;
                        accuracy = MDNS_JITTER_RANGE_USEC;
                        break;
                default:
                        assert_not_reached("bad protocol");
                }

                r = sd_event_add_time(
                                t->scope->manager->event,
                                &t->timeout_event_source,
                                clock_boottime_or_monotonic(),
                                ts + jitter, accuracy,
                                on_transaction_timeout, t);
                if (r < 0)
                        return r;

                t->n_attempts = 0;
                t->next_attempt_after = ts;
                t->state = DNS_TRANSACTION_PENDING;

                log_debug("Delaying %s transaction for " USEC_FMT "us.", dns_protocol_to_string(t->scope->protocol), jitter);
                return 0;
        }

        /* Otherwise, we need to ask the network */
        r = dns_transaction_make_packet(t);
        if (r == -EDOM) {
                /* Not the right request to make on this network?
                 * (i.e. an A request made on IPv6 or an AAAA request
                 * made on IPv4, on LLMNR or mDNS.) */
                dns_transaction_complete(t, DNS_TRANSACTION_NO_SERVERS);
                return 0;
        }
        if (r < 0)
                return r;

        if (t->scope->protocol == DNS_PROTOCOL_LLMNR &&
            (dns_name_endswith(DNS_RESOURCE_KEY_NAME(t->key), "in-addr.arpa") > 0 ||
             dns_name_endswith(DNS_RESOURCE_KEY_NAME(t->key), "ip6.arpa") > 0)) {

                /* RFC 4795, Section 2.4. says reverse lookups shall
                 * always be made via TCP on LLMNR */
                r = dns_transaction_open_tcp(t);
        } else {
                /* Try via UDP, and if that fails due to large size or lack of
                 * support try via TCP */
                r = dns_transaction_emit(t);
                if (r == -EMSGSIZE || r == -EAGAIN)
                        r = dns_transaction_open_tcp(t);
        }

        if (r == -ESRCH) {
                /* No servers to send this to? */
                dns_transaction_complete(t, DNS_TRANSACTION_NO_SERVERS);
                return 0;
        } else if (r < 0) {
                if (t->scope->protocol != DNS_PROTOCOL_DNS) {
                        dns_transaction_complete(t, DNS_TRANSACTION_RESOURCES);
                        return 0;
                }

                /* Couldn't send? Try immediately again, with a new server */
                dns_transaction_next_dns_server(t);

                return dns_transaction_go(t);
        }

        ts += transaction_get_resend_timeout(t);

        r = sd_event_add_time(
                        t->scope->manager->event,
                        &t->timeout_event_source,
                        clock_boottime_or_monotonic(),
                        ts, 0,
                        on_transaction_timeout, t);
        if (r < 0)
                return r;

        t->state = DNS_TRANSACTION_PENDING;
        t->next_attempt_after = ts;

        return 1;
}

static int dns_transaction_add_dnssec_transaction(DnsTransaction *t, DnsResourceKey *key, DnsTransaction **ret) {
        DnsTransaction *aux;
        int r;

        assert(t);
        assert(ret);
        assert(key);

        aux = dns_scope_find_transaction(t->scope, key, true);
        if (!aux) {
                r = dns_transaction_new(&aux, t->scope, key);
                if (r < 0)
                        return r;
        } else {
                if (set_contains(t->dnssec_transactions, aux)) {
                        *ret = aux;
                        return 0;
                }
        }

        r = set_ensure_allocated(&t->dnssec_transactions, NULL);
        if (r < 0)
                goto gc;

        r = set_ensure_allocated(&aux->notify_transactions, NULL);
        if (r < 0)
                goto gc;

        r = set_put(t->dnssec_transactions, aux);
        if (r < 0)
                goto gc;

        r = set_put(aux->notify_transactions, t);
        if (r < 0) {
                (void) set_remove(t->dnssec_transactions, aux);
                goto gc;
        }

        *ret = aux;
        return 1;

gc:
        dns_transaction_gc(aux);
        return r;
}

static int dns_transaction_request_dnssec_rr(DnsTransaction *t, DnsResourceKey *key) {
        _cleanup_(dns_answer_unrefp) DnsAnswer *a = NULL;
        DnsTransaction *aux;
        int r;

        assert(t);
        assert(key);

        /* Try to get the data from the trust anchor */
        r = dns_trust_anchor_lookup(&t->scope->manager->trust_anchor, key, &a);
        if (r < 0)
                return r;
        if (r > 0) {
                r = dns_answer_extend(&t->validated_keys, a);
                if (r < 0)
                        return r;

                return 0;
        }

        /* This didn't work, ask for it via the network/cache then. */
        r = dns_transaction_add_dnssec_transaction(t, key, &aux);
        if (r < 0)
                return r;

        if (aux->state == DNS_TRANSACTION_NULL) {
                r = dns_transaction_go(aux);
                if (r < 0)
                        return r;
        }

        return 0;
}

int dns_transaction_request_dnssec_keys(DnsTransaction *t) {
        DnsResourceRecord *rr;
        int r;

        assert(t);

        if (t->scope->dnssec_mode != DNSSEC_YES)
                return 0;

        DNS_ANSWER_FOREACH(rr, t->answer) {

                switch (rr->key->type) {

                case DNS_TYPE_RRSIG: {
                        /* For each RRSIG we request the matching DNSKEY */
                        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *dnskey = NULL;

                        /* If this RRSIG is about a DNSKEY RR and the
                         * signer is the same as the owner, then we
                         * already have the DNSKEY, and we don't have
                         * to look for more. */
                        if (rr->rrsig.type_covered == DNS_TYPE_DNSKEY) {
                                r = dns_name_equal(rr->rrsig.signer, DNS_RESOURCE_KEY_NAME(rr->key));
                                if (r < 0)
                                        return r;
                                if (r > 0)
                                        continue;
                        }

                        /* If the signer is not a parent of the owner,
                         * then the signature is bogus, let's ignore
                         * it. */
                        r = dns_name_endswith(DNS_RESOURCE_KEY_NAME(rr->key), rr->rrsig.signer);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                continue;

                        dnskey = dns_resource_key_new(rr->key->class, DNS_TYPE_DNSKEY, rr->rrsig.signer);
                        if (!dnskey)
                                return -ENOMEM;

                        log_debug("Requesting DNSKEY to validate transaction %" PRIu16" (key tag: %" PRIu16 ").", t->id, rr->rrsig.key_tag);

                        r = dns_transaction_request_dnssec_rr(t, dnskey);
                        if (r < 0)
                                return r;
                        break;
                }

                case DNS_TYPE_DNSKEY: {
                        /* For each DNSKEY we request the matching DS */
                        _cleanup_(dns_resource_key_unrefp) DnsResourceKey *ds = NULL;

                        ds = dns_resource_key_new(rr->key->class, DNS_TYPE_DS, DNS_RESOURCE_KEY_NAME(rr->key));
                        if (!ds)
                                return -ENOMEM;

                        log_debug("Requesting DS to validate transaction %" PRIu16" (key tag: %" PRIu16 ").", t->id, dnssec_keytag(rr));

                        r = dns_transaction_request_dnssec_rr(t, ds);
                        if (r < 0)
                                return r;

                        break;
                }}
        }

        return !set_isempty(t->dnssec_transactions);
}

void dns_transaction_notify(DnsTransaction *t, DnsTransaction *source) {
        int r;

        assert(t);
        assert(IN_SET(t->state, DNS_TRANSACTION_PENDING, DNS_TRANSACTION_VALIDATING));
        assert(source);

        /* Invoked whenever any of our auxiliary DNSSEC transactions
           completed its work. We simply copy the answer from that
           transaction over. */

        if (source->state != DNS_TRANSACTION_SUCCESS) {
                log_debug("Auxiliary DNSSEC RR query failed.");
                t->dnssec_result = DNSSEC_FAILED_AUXILIARY;
        } else {
                r = dns_answer_extend(&t->validated_keys, source->answer);
                if (r < 0) {
                        log_error_errno(r, "Failed to merge validated DNSSEC key data: %m");
                        t->dnssec_result = DNSSEC_FAILED_AUXILIARY;
                }
        }

        /* Detach us from the DNSSEC transaction. */
        (void) set_remove(t->dnssec_transactions, source);
        (void) set_remove(source->notify_transactions, t);

        /* If the state is still PENDING, we are still in the loop
         * that adds further DNSSEC transactions, hence don't check if
         * we are ready yet. If the state is VALIDATING however, we
         * should check if we are complete now. */
        if (t->state == DNS_TRANSACTION_VALIDATING)
                dns_transaction_process_dnssec(t);
}

static int dns_transaction_is_primary_response(DnsTransaction *t, DnsResourceRecord *rr) {
        int r;

        assert(t);
        assert(rr);

        /* Check if the specified RR is the "primary" response,
         * i.e. either matches the question precisely or is a
         * CNAME/DNAME for it */

        r = dns_resource_key_match_rr(t->key, rr, NULL);
        if (r != 0)
                return r;

        r = dns_resource_key_match_cname_or_dname(t->key, rr->key, NULL);
        if (r != 0)
                return r;

        return 0;
}

static int dns_transaction_validate_dnskey_by_ds(DnsTransaction *t) {
        DnsResourceRecord *rr;
        int ifindex, r;

        assert(t);

        /* Add all DNSKEY RRs from the answer that are validated by DS
         * RRs from the list of validated keys to the lis of validated
         * keys. */

        DNS_ANSWER_FOREACH_IFINDEX(rr, ifindex, t->answer) {

                r = dnssec_verify_dnskey_search(rr, t->validated_keys);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                /* If so, the DNSKEY is validated too. */
                r = dns_answer_add_extend(&t->validated_keys, rr, ifindex);
                if (r < 0)
                        return r;
        }

        return 0;
}

int dns_transaction_validate_dnssec(DnsTransaction *t) {
        _cleanup_(dns_answer_unrefp) DnsAnswer *validated = NULL;
        bool dnskeys_finalized = false;
        DnsResourceRecord *rr;
        int r;

        assert(t);

        /* We have now collected all DS and DNSKEY RRs in
         * t->validated_keys, let's see which RRs we can now
         * authenticate with that. */

        if (t->scope->dnssec_mode != DNSSEC_YES)
                return 0;

        /* Already validated */
        if (t->dnssec_result != _DNSSEC_RESULT_INVALID)
                return 0;

        if (IN_SET(t->answer_source, DNS_TRANSACTION_ZONE, DNS_TRANSACTION_TRUST_ANCHOR)) {
                t->dnssec_result = DNSSEC_VALIDATED;
                t->answer_authenticated = true;
                return 0;
        }

        if (log_get_max_level() >= LOG_DEBUG) {
                _cleanup_free_ char *ks = NULL;

                (void) dns_resource_key_to_string(t->key, &ks);
                log_debug("Validating response from transaction %" PRIu16 " (%s).", t->id, ks ? strstrip(ks) : "???");
        }

        /* First see if there are DNSKEYs we already known a validated DS for. */
        r = dns_transaction_validate_dnskey_by_ds(t);
        if (r < 0)
                return r;

        for (;;) {
                bool changed = false;

                DNS_ANSWER_FOREACH(rr, t->answer) {
                        DnssecResult result;

                        if (rr->key->type == DNS_TYPE_RRSIG)
                                continue;

                        r = dnssec_verify_rrset_search(t->answer, rr->key, t->validated_keys, USEC_INFINITY, &result);
                        if (r < 0)
                                return r;

                        if (log_get_max_level() >= LOG_DEBUG) {
                                _cleanup_free_ char *rrs = NULL;

                                (void) dns_resource_record_to_string(rr, &rrs);
                                log_debug("Looking at %s: %s", rrs ? strstrip(rrs) : "???", dnssec_result_to_string(result));
                        }

                        if (result == DNSSEC_VALIDATED) {

                                /* Add the validated RRset to the new list of validated RRsets */
                                r = dns_answer_copy_by_key(&validated, t->answer, rr->key);
                                if (r < 0)
                                        return r;

                                if (rr->key->type == DNS_TYPE_DNSKEY) {
                                        /* If we just validated a
                                         * DNSKEY RRset, then let's
                                         * add these keys to the set
                                         * of validated keys for this
                                         * transaction. */

                                        r = dns_answer_copy_by_key(&t->validated_keys, t->answer, rr->key);
                                        if (r < 0)
                                                return r;
                                }

                                /* Now, remove this RRset from the RRs still to process */
                                r = dns_answer_remove_by_key(&t->answer, rr->key);
                                if (r < 0)
                                        return r;

                                /* Exit the loop, we dropped something from the answer, start from the beginning */
                                changed = true;
                                break;

                        } else if (dnskeys_finalized) {
                                /* If we haven't read all DNSKEYs yet
                                 * a negative result of the validation
                                 * is irrelevant, as there might be
                                 * more DNSKEYs coming. */

                                r = dns_transaction_is_primary_response(t, rr);
                                if (r < 0)
                                        return r;
                                if (r > 0) {
                                        /* This is a primary response
                                         * to our question, and it
                                         * failed validation. That's
                                         * fatal. */
                                        t->dnssec_result = result;
                                        return 0;
                                }

                                /* This is just some auxiliary
                                 * data. Just remove the RRset and
                                 * continue. */
                                r = dns_answer_remove_by_key(&t->answer, rr->key);
                                if (r < 0)
                                        return r;

                                /* Exit the loop, we dropped something from the answer, start from the beginning */
                                changed = true;
                                break;
                        }
                }

                if (changed)
                        continue;

                if (!dnskeys_finalized) {
                        /* OK, now we know we have added all DNSKEYs
                         * we possibly could to our validated
                         * list. Now run the whole thing once more,
                         * and strip everything we still cannot
                         * validate.
                         */
                        dnskeys_finalized = true;
                        continue;
                }

                /* We're done */
                break;
        }

        dns_answer_unref(t->answer);
        t->answer = validated;
        validated = NULL;

        /* Everything that's now in t->answer is known to be good, hence cacheable. */
        t->n_answer_cacheable = (unsigned) -1; /* everything! */

        t->answer_authenticated = true;
        t->dnssec_result = DNSSEC_VALIDATED;
        return 1;
}

static const char* const dns_transaction_state_table[_DNS_TRANSACTION_STATE_MAX] = {
        [DNS_TRANSACTION_NULL] = "null",
        [DNS_TRANSACTION_PENDING] = "pending",
        [DNS_TRANSACTION_VALIDATING] = "validating",
        [DNS_TRANSACTION_FAILURE] = "failure",
        [DNS_TRANSACTION_SUCCESS] = "success",
        [DNS_TRANSACTION_NO_SERVERS] = "no-servers",
        [DNS_TRANSACTION_TIMEOUT] = "timeout",
        [DNS_TRANSACTION_ATTEMPTS_MAX_REACHED] = "attempts-max-reached",
        [DNS_TRANSACTION_INVALID_REPLY] = "invalid-reply",
        [DNS_TRANSACTION_RESOURCES] = "resources",
        [DNS_TRANSACTION_ABORTED] = "aborted",
        [DNS_TRANSACTION_DNSSEC_FAILED] = "dnssec-failed",
};
DEFINE_STRING_TABLE_LOOKUP(dns_transaction_state, DnsTransactionState);

static const char* const dns_transaction_source_table[_DNS_TRANSACTION_SOURCE_MAX] = {
        [DNS_TRANSACTION_NETWORK] = "network",
        [DNS_TRANSACTION_CACHE] = "cache",
        [DNS_TRANSACTION_ZONE] = "zone",
        [DNS_TRANSACTION_TRUST_ANCHOR] = "trust-anchor",
};
DEFINE_STRING_TABLE_LOOKUP(dns_transaction_source, DnsTransactionSource);
