/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2014-2015 Intel Corporation. All rights reserved.
***/

#include <errno.h>

#include "alloc-util.h"
#include "dhcp6-lease-internal.h"
#include "dhcp6-protocol.h"
#include "strv.h"
#include "util.h"

int sd_dhcp6_lease_get_timestamp(sd_dhcp6_lease *lease, clockid_t clock, uint64_t *ret) {
        assert_return(lease, -EINVAL);
        assert_return(TRIPLE_TIMESTAMP_HAS_CLOCK(clock), -EOPNOTSUPP);
        assert_return(clock_supported(clock), -EOPNOTSUPP);
        assert_return(ret, -EINVAL);

        if (!triple_timestamp_is_set(&lease->timestamp))
                return -ENODATA;

        *ret = triple_timestamp_by_clock(&lease->timestamp, clock);
        return 0;
}

void dhcp6_lease_set_lifetime(sd_dhcp6_lease *lease) {
        uint32_t t1 = UINT32_MAX, t2 = UINT32_MAX, min_valid_lt = UINT32_MAX;
        DHCP6Address *a;

        assert(lease);
        assert(lease->ia_na || lease->ia_pd);

        if (lease->ia_na) {
                t1 = MIN(t1, be32toh(lease->ia_na->header.lifetime_t1));
                t2 = MIN(t2, be32toh(lease->ia_na->header.lifetime_t2));

                LIST_FOREACH(addresses, a, lease->ia_na->addresses)
                        min_valid_lt = MIN(min_valid_lt, be32toh(a->iaaddr.lifetime_valid));
        }

        if (lease->ia_pd) {
                t1 = MIN(t1, be32toh(lease->ia_pd->header.lifetime_t1));
                t2 = MIN(t2, be32toh(lease->ia_pd->header.lifetime_t2));

                LIST_FOREACH(addresses, a, lease->ia_pd->addresses)
                        min_valid_lt = MIN(min_valid_lt, be32toh(a->iapdprefix.lifetime_valid));
        }

        if (t2 == 0 || t2 > min_valid_lt) {
                /* If T2 is zero or longer than the minimum valid lifetime of the addresses or prefixes,
                 * then adjust lifetime with it. */
                t1 = min_valid_lt / 2;
                t2 = min_valid_lt / 10 * 8;
        }

        assert(t2 <= min_valid_lt);
        lease->max_retransmit_duration = (min_valid_lt - t2) * USEC_PER_SEC;

        lease->lifetime_t1 = t1 == UINT32_MAX ? USEC_INFINITY : t1 * USEC_PER_SEC;
        lease->lifetime_t2 = t2 == UINT32_MAX ? USEC_INFINITY : t2 * USEC_PER_SEC;
}

int dhcp6_lease_get_lifetime(sd_dhcp6_lease *lease, usec_t *ret_t1, usec_t *ret_t2) {
        assert(lease);

        if (!lease->ia_na && !lease->ia_pd)
                return -ENODATA;

        if (ret_t1)
                *ret_t1 = lease->lifetime_t1;
        if (ret_t2)
                *ret_t2 = lease->lifetime_t2;
        return 0;
}

int dhcp6_lease_get_max_retransmit_duration(sd_dhcp6_lease *lease, usec_t *ret) {
        assert(lease);

        if (!lease->ia_na && !lease->ia_pd)
                return -ENODATA;

        if (ret)
                *ret = lease->max_retransmit_duration;
        return 0;
}

int sd_dhcp6_lease_get_server_address(sd_dhcp6_lease *lease, struct in6_addr *ret) {
        assert_return(lease, -EINVAL);
        assert_return(ret, -EINVAL);

        *ret = lease->server_address;
        return 0;
}

void dhcp6_ia_clear_addresses(DHCP6IA *ia) {
        DHCP6Address *a, *n;

        assert(ia);

        LIST_FOREACH_SAFE(addresses, a, n, ia->addresses)
                free(a);

        ia->addresses = NULL;
}

DHCP6IA *dhcp6_ia_free(DHCP6IA *ia) {
        if (!ia)
                return NULL;

        dhcp6_ia_clear_addresses(ia);

        return mfree(ia);
}

int dhcp6_lease_set_clientid(sd_dhcp6_lease *lease, const uint8_t *id, size_t len) {
        uint8_t *clientid = NULL;

        assert(lease);
        assert(id || len == 0);

        if (len > 0) {
                clientid = memdup(id, len);
                if (!clientid)
                        return -ENOMEM;
        }

        free_and_replace(lease->clientid, clientid);
        lease->clientid_len = len;

        return 0;
}

int dhcp6_lease_get_clientid(sd_dhcp6_lease *lease, uint8_t **ret_id, size_t *ret_len) {
        assert(lease);

        if (!lease->clientid)
                return -ENODATA;

        if (ret_id)
                *ret_id = lease->clientid;
        if (ret_len)
                *ret_len = lease->clientid_len;

        return 0;
}

int dhcp6_lease_set_serverid(sd_dhcp6_lease *lease, const uint8_t *id, size_t len) {
        uint8_t *serverid = NULL;

        assert(lease);
        assert(id || len == 0);

        if (len > 0) {
                serverid = memdup(id, len);
                if (!serverid)
                        return -ENOMEM;
        }

        free_and_replace(lease->serverid, serverid);
        lease->serverid_len = len;

        return 0;
}

int dhcp6_lease_get_serverid(sd_dhcp6_lease *lease, uint8_t **ret_id, size_t *ret_len) {
        assert(lease);

        if (!lease->serverid)
                return -ENODATA;

        if (ret_id)
                *ret_id = lease->serverid;
        if (ret_len)
                *ret_len = lease->serverid_len;
        return 0;
}

int dhcp6_lease_set_preference(sd_dhcp6_lease *lease, uint8_t preference) {
        assert(lease);

        lease->preference = preference;
        return 0;
}

int dhcp6_lease_get_preference(sd_dhcp6_lease *lease, uint8_t *ret) {
        assert(lease);
        assert(ret);

        *ret = lease->preference;
        return 0;
}

int dhcp6_lease_set_rapid_commit(sd_dhcp6_lease *lease) {
        assert(lease);

        lease->rapid_commit = true;
        return 0;
}

int dhcp6_lease_get_rapid_commit(sd_dhcp6_lease *lease, bool *ret) {
        assert(lease);
        assert(ret);

        *ret = lease->rapid_commit;
        return 0;
}

int sd_dhcp6_lease_get_address(
                sd_dhcp6_lease *lease,
                struct in6_addr *ret_addr,
                uint32_t *ret_lifetime_preferred,
                uint32_t *ret_lifetime_valid) {

        assert_return(lease, -EINVAL);

        if (!lease->addr_iter)
                return -ENODATA;

        if (ret_addr)
                *ret_addr = lease->addr_iter->iaaddr.address;
        if (ret_lifetime_preferred)
                *ret_lifetime_preferred = be32toh(lease->addr_iter->iaaddr.lifetime_preferred);
        if (ret_lifetime_valid)
                *ret_lifetime_valid = be32toh(lease->addr_iter->iaaddr.lifetime_valid);

        lease->addr_iter = lease->addr_iter->addresses_next;
        return 0;
}

void sd_dhcp6_lease_reset_address_iter(sd_dhcp6_lease *lease) {
        if (lease)
                lease->addr_iter = lease->ia_na ? lease->ia_na->addresses : NULL;
}

int sd_dhcp6_lease_get_pd(
                sd_dhcp6_lease *lease,
                struct in6_addr *ret_prefix,
                uint8_t *ret_prefix_len,
                uint32_t *ret_lifetime_preferred,
                uint32_t *ret_lifetime_valid) {

        assert_return(lease, -EINVAL);

        if (!lease->prefix_iter)
                return -ENODATA;

        if (ret_prefix)
                *ret_prefix = lease->prefix_iter->iapdprefix.address;
        if (ret_prefix_len)
                *ret_prefix_len = lease->prefix_iter->iapdprefix.prefixlen;
        if (ret_lifetime_preferred)
                *ret_lifetime_preferred = be32toh(lease->prefix_iter->iapdprefix.lifetime_preferred);
        if (ret_lifetime_valid)
                *ret_lifetime_valid = be32toh(lease->prefix_iter->iapdprefix.lifetime_valid);

        lease->prefix_iter = lease->prefix_iter->addresses_next;
        return 0;
}

void sd_dhcp6_lease_reset_pd_prefix_iter(sd_dhcp6_lease *lease) {
        if (lease)
                lease->prefix_iter = lease->ia_pd ? lease->ia_pd->addresses : NULL;
}

int dhcp6_lease_add_dns(sd_dhcp6_lease *lease, const uint8_t *optval, size_t optlen) {
        assert(lease);
        assert(optval || optlen == 0);

        if (optlen == 0)
                return 0;

        return dhcp6_option_parse_addresses(optval, optlen, &lease->dns, &lease->dns_count);
}

int sd_dhcp6_lease_get_dns(sd_dhcp6_lease *lease, const struct in6_addr **ret) {
        assert_return(lease, -EINVAL);

        if (!lease->dns)
                return -ENODATA;

        if (ret)
                *ret = lease->dns;

        return lease->dns_count;
}

int dhcp6_lease_add_domains(sd_dhcp6_lease *lease, const uint8_t *optval, size_t optlen) {
        _cleanup_strv_free_ char **domains = NULL;
        int r;

        assert(lease);
        assert(optval || optlen == 0);

        if (optlen == 0)
                return 0;

        r = dhcp6_option_parse_domainname_list(optval, optlen, &domains);
        if (r < 0)
                return r;

        return strv_extend_strv(&lease->domains, domains, true);
}

int sd_dhcp6_lease_get_domains(sd_dhcp6_lease *lease, char ***ret) {
        assert_return(lease, -EINVAL);
        assert_return(ret, -EINVAL);

        if (!lease->domains)
                return -ENODATA;

        *ret = lease->domains;
        return strv_length(lease->domains);
}

int dhcp6_lease_add_ntp(sd_dhcp6_lease *lease, const uint8_t *optval, size_t optlen) {
        int r;

        assert(lease);
        assert(optval || optlen == 0);

        for (size_t offset = 0; offset < optlen;) {
                const uint8_t *subval;
                size_t sublen;
                uint16_t subopt;

                r = dhcp6_option_parse(optval, optlen, &offset, &subopt, &sublen, &subval);
                if (r < 0)
                        return r;

                switch(subopt) {
                case DHCP6_NTP_SUBOPTION_SRV_ADDR:
                case DHCP6_NTP_SUBOPTION_MC_ADDR:
                        if (sublen != 16)
                                return 0;

                        r = dhcp6_option_parse_addresses(subval, sublen, &lease->ntp, &lease->ntp_count);
                        if (r < 0)
                                return r;

                        break;

                case DHCP6_NTP_SUBOPTION_SRV_FQDN: {
                        _cleanup_free_ char *server = NULL;

                        r = dhcp6_option_parse_domainname(subval, sublen, &server);
                        if (r < 0)
                                return r;

                        if (strv_contains(lease->ntp_fqdn, server))
                                continue;

                        r = strv_consume(&lease->ntp_fqdn, TAKE_PTR(server));
                        if (r < 0)
                                return r;

                        break;
                }}
        }

        return 0;
}

int dhcp6_lease_add_sntp(sd_dhcp6_lease *lease, const uint8_t *optval, size_t optlen) {
        assert(lease);
        assert(optval || optlen == 0);

        if (optlen == 0)
                return 0;

        /* SNTP option is defined in RFC4075, and deprecated by RFC5908. */
        return dhcp6_option_parse_addresses(optval, optlen, &lease->sntp, &lease->sntp_count);
}

int sd_dhcp6_lease_get_ntp_addrs(sd_dhcp6_lease *lease, const struct in6_addr **ret) {
        assert_return(lease, -EINVAL);

        if (lease->ntp) {
                if (ret)
                        *ret = lease->ntp;
                return lease->ntp_count;
        }

        if (lease->sntp && !lease->ntp_fqdn) {
                /* Fallback to the deprecated SNTP option. */
                if (ret)
                        *ret = lease->sntp;
                return lease->sntp_count;
        }

        return -ENODATA;
}

int sd_dhcp6_lease_get_ntp_fqdn(sd_dhcp6_lease *lease, char ***ret) {
        assert_return(lease, -EINVAL);

        if (!lease->ntp_fqdn)
                return -ENODATA;

        if (ret)
                *ret = lease->ntp_fqdn;
        return strv_length(lease->ntp_fqdn);
}

int dhcp6_lease_set_fqdn(sd_dhcp6_lease *lease, const uint8_t *optval, size_t optlen) {
        char *fqdn;
        int r;

        assert(lease);
        assert(optval || optlen == 0);

        if (optlen == 0)
                return 0;

        if (optlen < 2)
                return -ENODATA;

        /* Ignore the flags field, it doesn't carry any useful
           information for clients. */
        r = dhcp6_option_parse_domainname(optval + 1, optlen - 1, &fqdn);
        if (r < 0)
                return r;

        return free_and_replace(lease->fqdn, fqdn);
}

int sd_dhcp6_lease_get_fqdn(sd_dhcp6_lease *lease, const char **ret) {
        assert_return(lease, -EINVAL);
        assert_return(ret, -EINVAL);

        if (!lease->fqdn)
                return -ENODATA;

        *ret = lease->fqdn;
        return 0;
}

static sd_dhcp6_lease *dhcp6_lease_free(sd_dhcp6_lease *lease) {
        if (!lease)
                return NULL;

        free(lease->clientid);
        free(lease->serverid);
        dhcp6_ia_free(lease->ia_na);
        dhcp6_ia_free(lease->ia_pd);
        free(lease->dns);
        free(lease->fqdn);
        strv_free(lease->domains);
        free(lease->ntp);
        strv_free(lease->ntp_fqdn);
        free(lease->sntp);

        return mfree(lease);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(sd_dhcp6_lease, sd_dhcp6_lease, dhcp6_lease_free);

int dhcp6_lease_new(sd_dhcp6_lease **ret) {
        sd_dhcp6_lease *lease;

        assert(ret);

        lease = new0(sd_dhcp6_lease, 1);
        if (!lease)
                return -ENOMEM;

        lease->n_ref = 1;

        *ret = lease;
        return 0;
}
