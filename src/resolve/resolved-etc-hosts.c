/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fd-util.h"
#include "fileio.h"
#include "hostname-util.h"
#include "resolved-dns-synthesize.h"
#include "resolved-etc-hosts.h"
#include "socket-netlink.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "time-util.h"

/* Recheck /etc/hosts at most once every 2s */
#define ETC_HOSTS_RECHECK_USEC (2*USEC_PER_SEC)

static EtcHostsItemByAddress *etc_hosts_item_by_address_free(EtcHostsItemByAddress *item) {
        if (!item)
                return NULL;

        strv_free(item->names);
        return mfree(item);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(EtcHostsItemByAddress*, etc_hosts_item_by_address_free);

static EtcHostsItemByName *etc_hosts_item_by_name_free(EtcHostsItemByName *item) {
        if (!item)
                return NULL;

        free(item->name);
        free(item->addresses);
        return mfree(item);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(EtcHostsItemByName*, etc_hosts_item_by_name_free);

void etc_hosts_clear(EtcHosts *hosts) {
        assert(hosts);

        hosts->by_address = hashmap_free_with_destructor(hosts->by_address, etc_hosts_item_by_address_free);
        hosts->by_name = hashmap_free_with_destructor(hosts->by_name, etc_hosts_item_by_name_free);
        hosts->no_address = set_free_free(hosts->no_address);
}

void manager_etc_hosts_flush(Manager *m) {
        etc_hosts_clear(&m->etc_hosts);
        m->etc_hosts_stat = (struct stat) {};
}

static int parse_line(EtcHosts *hosts, unsigned nr, const char *line) {
        _cleanup_free_ char *address_str = NULL;
        struct in_addr_data address = {};
        bool found = false;
        EtcHostsItemByAddress *item;
        int r;

        assert(hosts);
        assert(line);

        r = extract_first_word(&line, &address_str, NULL, EXTRACT_RELAX);
        if (r < 0)
                return log_error_errno(r, "/etc/hosts:%u: failed to extract address: %m", nr);
        assert(r > 0); /* We already checked that the line is not empty, so it should contain *something* */

        r = in_addr_ifindex_from_string_auto(address_str, &address.family, &address.address, NULL);
        if (r < 0) {
                log_warning_errno(r, "/etc/hosts:%u: address '%s' is invalid, ignoring: %m", nr, address_str);
                return 0;
        }

        r = in_addr_data_is_null(&address);
        if (r < 0) {
                log_warning_errno(r, "/etc/hosts:%u: address '%s' is invalid, ignoring: %m", nr, address_str);
                return 0;
        }
        if (r > 0)
                /* This is an 0.0.0.0 or :: item, which we assume means that we shall map the specified hostname to
                 * nothing. */
                item = NULL;
        else {
                /* If this is a normal address, then simply add entry mapping it to the specified names */

                item = hashmap_get(hosts->by_address, &address);
                if (!item) {
                        _cleanup_(etc_hosts_item_by_address_freep) EtcHostsItemByAddress *new_item = NULL;

                        new_item = new(EtcHostsItemByAddress, 1);
                        if (!new_item)
                                return log_oom();

                        *new_item = (EtcHostsItemByAddress) {
                                .address = address,
                        };

                        r = hashmap_ensure_put(&hosts->by_address, &in_addr_data_hash_ops, &new_item->address, new_item);
                        if (r < 0)
                                return log_oom();

                        item = TAKE_PTR(new_item);
                }
        }

        for (;;) {
                _cleanup_free_ char *name = NULL;
                EtcHostsItemByName *bn;

                r = extract_first_word(&line, &name, NULL, EXTRACT_RELAX);
                if (r < 0)
                        return log_error_errno(r, "/etc/hosts:%u: couldn't extract hostname: %m", nr);
                if (r == 0)
                        break;

                r = dns_name_is_valid_ldh(name);
                if (r <= 0) {
                        if (r < 0)
                                log_warning_errno(r, "/etc/hosts:%u: Failed to check the validity of hostname \"%s\", ignoring: %m", nr, name);
                        else
                                log_warning("/etc/hosts:%u: hostname \"%s\" is not valid, ignoring.", nr, name);
                        continue;
                }

                found = true;

                if (!item) {
                        /* Optimize the case where we don't need to store any addresses, by storing
                         * only the name in a dedicated Set instead of the hashmap */

                        r = set_ensure_consume(&hosts->no_address, &dns_name_hash_ops, TAKE_PTR(name));
                        if (r < 0)
                                return log_oom();

                        continue;
                }

                r = strv_extend_with_size(&item->names, &item->n_names, name);
                if (r < 0)
                        return log_oom();

                bn = hashmap_get(hosts->by_name, name);
                if (!bn) {
                        _cleanup_(etc_hosts_item_by_name_freep) EtcHostsItemByName *new_item = NULL;

                        new_item = new(EtcHostsItemByName, 1);
                        if (!new_item)
                                return log_oom();

                        *new_item = (EtcHostsItemByName) {
                                .name = TAKE_PTR(name),
                        };

                        r = hashmap_ensure_put(&hosts->by_name, &dns_name_hash_ops, new_item->name, new_item);
                        if (r < 0)
                                return log_oom();

                        bn = TAKE_PTR(new_item);
                }

                if (!GREEDY_REALLOC(bn->addresses, bn->n_addresses + 1))
                        return log_oom();

                bn->addresses[bn->n_addresses++] = &item->address;
        }

        if (!found)
                log_warning("/etc/hosts:%u: line is missing any valid hostnames", nr);

        return 0;
}

static void strip_localhost(EtcHosts *hosts) {
        static const struct in_addr_data local_in_addrs[] = {
                {
                        .family = AF_INET,
#if __BYTE_ORDER == __LITTLE_ENDIAN
                        /* We want constant expressions here, that's why we don't use htole32() here */
                        .address.in.s_addr = UINT32_C(0x0100007F),
#else
                        .address.in.s_addr = UINT32_C(0x7F000001),
#endif
                },
                {
                        .family = AF_INET6,
                        .address.in6 = IN6ADDR_LOOPBACK_INIT,
                },
        };

        assert(hosts);

        /* Removes the 'localhost' entry from what we loaded. But only if the mapping is exclusively between
         * 127.0.0.1 and localhost (or aliases to that we recognize). If there's any other name assigned to
         * it, we leave the entry in.
         *
         * This way our regular synthesizing can take over, but only if it would result in the exact same
         * mappings.  */

        for (size_t j = 0; j < ELEMENTSOF(local_in_addrs); j++) {
                bool all_localhost, all_local_address;
                EtcHostsItemByAddress *item;

                item = hashmap_get(hosts->by_address, local_in_addrs + j);
                if (!item)
                        continue;

                /* Check whether all hostnames the loopback address points to are localhost ones */
                all_localhost = true;
                STRV_FOREACH(i, item->names)
                        if (!is_localhost(*i)) {
                                all_localhost = false;
                                break;
                        }

                if (!all_localhost) /* Not all names are localhost, hence keep the entries for this address. */
                        continue;

                /* Now check if the names listed for this address actually all point back just to this
                 * address (or the other loopback address). If not, let's stay away from this too. */
                all_local_address = true;
                STRV_FOREACH(i, item->names) {
                        EtcHostsItemByName *n;

                        n = hashmap_get(hosts->by_name, *i);
                        if (!n) /* No reverse entry? Then almost certainly the entry already got deleted from
                                 * the previous iteration of this loop, i.e. via the other protocol */
                                break;

                        /* Now check if the addresses of this item are all localhost addresses */
                        for (size_t m = 0; m < n->n_addresses; m++)
                                if (!in_addr_is_localhost(n->addresses[m]->family, &n->addresses[m]->address)) {
                                        all_local_address = false;
                                        break;
                                }

                        if (!all_local_address)
                                break;
                }

                if (!all_local_address)
                        continue;

                STRV_FOREACH(i, item->names)
                        etc_hosts_item_by_name_free(hashmap_remove(hosts->by_name, *i));

                assert_se(hashmap_remove(hosts->by_address, local_in_addrs + j) == item);
                etc_hosts_item_by_address_free(item);
        }
}

int etc_hosts_parse(EtcHosts *hosts, FILE *f) {
        _cleanup_(etc_hosts_clear) EtcHosts t = {};
        unsigned nr = 0;
        int r;

        for (;;) {
                _cleanup_free_ char *line = NULL;
                char *l;

                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return log_error_errno(r, "Failed to read /etc/hosts: %m");
                if (r == 0)
                        break;

                nr++;

                l = strchr(line, '#');
                if (l)
                        *l = '\0';

                l = strstrip(line);
                if (isempty(l))
                        continue;

                r = parse_line(&t, nr, l);
                if (r < 0)
                        return r;
        }

        strip_localhost(&t);

        etc_hosts_clear(hosts);
        *hosts = t;
        t = (EtcHosts) {}; /* prevent cleanup */
        return 0;
}

static int manager_etc_hosts_read(Manager *m) {
        _cleanup_fclose_ FILE *f = NULL;
        struct stat st;
        usec_t ts;
        int r;

        assert_se(sd_event_now(m->event, CLOCK_BOOTTIME, &ts) >= 0);

        /* See if we checked /etc/hosts recently already */
        if (m->etc_hosts_last != USEC_INFINITY && m->etc_hosts_last + ETC_HOSTS_RECHECK_USEC > ts)
                return 0;

        m->etc_hosts_last = ts;

        if (m->etc_hosts_stat.st_mode != 0) {
                if (stat("/etc/hosts", &st) < 0) {
                        if (errno != ENOENT)
                                return log_error_errno(errno, "Failed to stat /etc/hosts: %m");

                        manager_etc_hosts_flush(m);
                        return 0;
                }

                /* Did the mtime or ino/dev change? If not, there's no point in re-reading the file. */
                if (stat_inode_unmodified(&m->etc_hosts_stat, &st))
                        return 0;
        }

        f = fopen("/etc/hosts", "re");
        if (!f) {
                if (errno != ENOENT)
                        return log_error_errno(errno, "Failed to open /etc/hosts: %m");

                manager_etc_hosts_flush(m);
                return 0;
        }

        /* Take the timestamp at the beginning of processing, so that any changes made later are read on the next
         * invocation */
        r = fstat(fileno(f), &st);
        if (r < 0)
                return log_error_errno(errno, "Failed to fstat() /etc/hosts: %m");

        r = etc_hosts_parse(&m->etc_hosts, f);
        if (r < 0)
                return r;

        m->etc_hosts_stat = st;
        m->etc_hosts_last = ts;

        return 1;
}

static int etc_hosts_lookup_by_address(
                EtcHosts *hosts,
                DnsQuestion *q,
                const char *name,
                const struct in_addr_data *address,
                DnsAnswer **answer) {

        DnsResourceKey *t, *found_ptr = NULL;
        EtcHostsItemByAddress *item;
        int r;

        assert(hosts);
        assert(q);
        assert(name);
        assert(address);
        assert(answer);

        item = hashmap_get(hosts->by_address, address);
        if (!item)
                return 0;

        /* We have an address in /etc/hosts that matches the queried name. Let's return successful. Actual data
         * we'll only return if the request was for PTR. */

        DNS_QUESTION_FOREACH(t, q) {
                if (!IN_SET(t->type, DNS_TYPE_PTR, DNS_TYPE_ANY))
                        continue;
                if (!IN_SET(t->class, DNS_CLASS_IN, DNS_CLASS_ANY))
                        continue;

                r = dns_name_equal(dns_resource_key_name(t), name);
                if (r < 0)
                        return r;
                if (r > 0) {
                        found_ptr = t;
                        break;
                }
        }

        if (found_ptr) {
                r = dns_answer_reserve(answer, item->n_names);
                if (r < 0)
                        return r;

                STRV_FOREACH(n, item->names) {
                        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

                        rr = dns_resource_record_new(found_ptr);
                        if (!rr)
                                return -ENOMEM;

                        rr->ptr.name = strdup(*n);
                        if (!rr->ptr.name)
                                return -ENOMEM;

                        r = dns_answer_add(*answer, rr, 0, DNS_ANSWER_AUTHENTICATED, NULL);
                        if (r < 0)
                                return r;
                }
        }

        return 1;
}

static int etc_hosts_lookup_by_name(
                EtcHosts *hosts,
                DnsQuestion *q,
                const char *name,
                DnsAnswer **answer) {

        bool found_a = false, found_aaaa = false;
        EtcHostsItemByName *item;
        DnsResourceKey *t;
        int r;

        assert(hosts);
        assert(q);
        assert(name);
        assert(answer);

        item = hashmap_get(hosts->by_name, name);
        if (item) {
                r = dns_answer_reserve(answer, item->n_addresses);
                if (r < 0)
                        return r;
        } else {
                /* Check if name was listed with no address. If yes, continue to return an answer. */
                if (!set_contains(hosts->no_address, name))
                        return 0;
        }

        DNS_QUESTION_FOREACH(t, q) {
                if (!IN_SET(t->type, DNS_TYPE_A, DNS_TYPE_AAAA, DNS_TYPE_ANY))
                        continue;
                if (!IN_SET(t->class, DNS_CLASS_IN, DNS_CLASS_ANY))
                        continue;

                r = dns_name_equal(dns_resource_key_name(t), name);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                if (IN_SET(t->type, DNS_TYPE_A, DNS_TYPE_ANY))
                        found_a = true;
                if (IN_SET(t->type, DNS_TYPE_AAAA, DNS_TYPE_ANY))
                        found_aaaa = true;

                if (found_a && found_aaaa)
                        break;
        }

        for (unsigned i = 0; item && i < item->n_addresses; i++) {
                _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;

                if ((!found_a && item->addresses[i]->family == AF_INET) ||
                    (!found_aaaa && item->addresses[i]->family == AF_INET6))
                        continue;

                r = dns_resource_record_new_address(&rr, item->addresses[i]->family, &item->addresses[i]->address, item->name);
                if (r < 0)
                        return r;

                r = dns_answer_add(*answer, rr, 0, DNS_ANSWER_AUTHENTICATED, NULL);
                if (r < 0)
                        return r;
        }

        return found_a || found_aaaa;
}

int manager_etc_hosts_lookup(Manager *m, DnsQuestion *q, DnsAnswer **answer) {
        struct in_addr_data k;
        const char *name;

        assert(m);
        assert(q);
        assert(answer);

        if (!m->read_etc_hosts)
                return 0;

        (void) manager_etc_hosts_read(m);

        name = dns_question_first_name(q);
        if (!name)
                return 0;

        if (dns_name_address(name, &k.family, &k.address) > 0)
                return etc_hosts_lookup_by_address(&m->etc_hosts, q, name, &k, answer);

        return etc_hosts_lookup_by_name(&m->etc_hosts, q, name, answer);
}
