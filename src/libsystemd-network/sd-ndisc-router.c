/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2014 Intel Corporation. All rights reserved.
***/

#include <netinet/icmp6.h>

#include "sd-ndisc.h"

#include "alloc-util.h"
#include "dns-domain.h"
#include "escape.h"
#include "hostname-util.h"
#include "memory-util.h"
#include "missing_network.h"
#include "ndisc-internal.h"
#include "ndisc-protocol.h"
#include "ndisc-router-internal.h"
#include "strv.h"

static sd_ndisc_router* ndisc_router_free(sd_ndisc_router *rt) {
        if (!rt)
                return NULL;

        icmp6_packet_unref(rt->packet);
        return mfree(rt);
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_ndisc_router, sd_ndisc_router, ndisc_router_free);

sd_ndisc_router* ndisc_router_new(ICMP6Packet *packet) {
        sd_ndisc_router *rt;

        assert(packet);

        rt = new(sd_ndisc_router, 1);
        if (!rt)
                return NULL;

        *rt = (sd_ndisc_router) {
                .n_ref = 1,
                .packet = icmp6_packet_ref(packet),
        };

        return rt;
}

int sd_ndisc_router_get_address(sd_ndisc_router *rt, struct in6_addr *ret) {
        assert_return(rt, -EINVAL);

        return icmp6_packet_get_sender_address(rt->packet, ret);
}

int sd_ndisc_router_get_timestamp(sd_ndisc_router *rt, clockid_t clock, uint64_t *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        return icmp6_packet_get_timestamp(rt->packet, clock, ret);
}

#define DEFINE_GET_TIMESTAMP(name)                                      \
        int sd_ndisc_router_##name##_timestamp(                         \
                        sd_ndisc_router *rt,                            \
                        clockid_t clock,                                \
                        uint64_t *ret) {                                \
                                                                        \
                usec_t s, t;                                            \
                int r;                                                  \
                                                                        \
                assert_return(rt, -EINVAL);                             \
                assert_return(ret, -EINVAL);                            \
                                                                        \
                r = sd_ndisc_router_##name(rt, &s);                     \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                r = sd_ndisc_router_get_timestamp(rt, clock, &t);       \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                *ret = time_span_to_stamp(s, t);                        \
                return 0;                                               \
        }

DEFINE_GET_TIMESTAMP(get_lifetime);
DEFINE_GET_TIMESTAMP(prefix_get_valid_lifetime);
DEFINE_GET_TIMESTAMP(prefix_get_preferred_lifetime);
DEFINE_GET_TIMESTAMP(route_get_lifetime);
DEFINE_GET_TIMESTAMP(rdnss_get_lifetime);
DEFINE_GET_TIMESTAMP(dnssl_get_lifetime);
DEFINE_GET_TIMESTAMP(prefix64_get_lifetime);

static bool pref64_option_verify(const struct nd_opt_prefix64_info *p, size_t length) {
        uint16_t lifetime_and_plc;

        assert(p);

        if (length != sizeof(struct nd_opt_prefix64_info))
                return false;

        lifetime_and_plc = be16toh(p->lifetime_and_plc);
        if (pref64_plc_to_prefix_length(lifetime_and_plc, NULL) < 0)
                return false;

        return true;
}

int ndisc_router_parse(sd_ndisc *nd, sd_ndisc_router *rt) {
        const struct nd_router_advert *a;
        bool has_mtu = false, has_flag_extension = false;
        int r;

        assert(rt);
        assert(rt->packet);

        if (rt->packet->raw_size < sizeof(struct nd_router_advert))
                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                       "Too small to be a router advertisement, ignoring.");

        a = (const struct nd_router_advert*) rt->packet->raw_packet;
        assert(a->nd_ra_type == ND_ROUTER_ADVERT);
        assert(a->nd_ra_code == 0);

        rt->hop_limit = a->nd_ra_curhoplimit;
        rt->flags = a->nd_ra_flags_reserved; /* the first 8 bits */
        rt->lifetime_usec = be16_sec_to_usec(a->nd_ra_router_lifetime, /* max_as_infinity = */ false);
        rt->icmp6_ratelimit_usec = be32_msec_to_usec(a->nd_ra_retransmit, /* max_as_infinity = */ false);
        rt->reachable_time_usec = be32_msec_to_usec(a->nd_ra_reachable, /* mas_as_infinity = */ false);
        rt->retransmission_time_usec = be32_msec_to_usec(a->nd_ra_retransmit, /* max_as_infinity = */ false);

        rt->preference = (rt->flags >> 3) & 3;
        if (!IN_SET(rt->preference, SD_NDISC_PREFERENCE_LOW, SD_NDISC_PREFERENCE_HIGH))
                rt->preference = SD_NDISC_PREFERENCE_MEDIUM;

        for (size_t offset = sizeof(struct nd_router_advert), length; offset < rt->packet->raw_size; offset += length) {
                uint8_t type;
                const uint8_t *p;

                r = ndisc_option_parse(rt->packet, offset, &type, &length, &p);
                if (r < 0)
                        return log_ndisc_errno(nd, r, "Failed to parse NDisc option header, ignoring: %m");

                switch (type) {

                case SD_NDISC_OPTION_PREFIX_INFORMATION:

                        if (length != 4*8)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "Prefix option of invalid size, ignoring datagram.");

                        if (p[2] > 128)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "Bad prefix length, ignoring datagram.");

                        break;

                case SD_NDISC_OPTION_MTU: {
                        uint32_t m;

                        if (has_mtu) {
                                log_ndisc(nd, "MTU option specified twice, ignoring.");
                                break;
                        }

                        if (length != 8)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "MTU option of invalid size, ignoring datagram.");

                        m = be32toh(*(uint32_t*) (p + 4));
                        if (m >= IPV6_MIN_MTU) /* ignore invalidly small MTUs */
                                rt->mtu = m;

                        has_mtu = true;
                        break;
                }

                case SD_NDISC_OPTION_ROUTE_INFORMATION:
                        if (length < 1*8 || length > 3*8)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "Route information option of invalid size, ignoring datagram.");

                        if (p[2] > 128)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "Bad route prefix length, ignoring datagram.");

                        break;

                case SD_NDISC_OPTION_RDNSS:
                        if (length < 3*8 || (length % (2*8)) != 1*8)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG), "RDNSS option has invalid size.");

                        break;

                case SD_NDISC_OPTION_FLAGS_EXTENSION:

                        if (has_flag_extension) {
                                log_ndisc(nd, "Flags extension option specified twice, ignoring.");
                                break;
                        }

                        if (length < 1*8)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "Flags extension option has invalid size.");

                        /* Add in the additional flags bits */
                        rt->flags |=
                                ((uint64_t) p[2] << 8) |
                                ((uint64_t) p[3] << 16) |
                                ((uint64_t) p[4] << 24) |
                                ((uint64_t) p[5] << 32) |
                                ((uint64_t) p[6] << 40) |
                                ((uint64_t) p[7] << 48);

                        has_flag_extension = true;
                        break;

                case SD_NDISC_OPTION_DNSSL:
                        if (length < 2*8)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "DNSSL option has invalid size.");

                        break;
                case SD_NDISC_OPTION_PREF64: {
                        if (!pref64_option_verify((struct nd_opt_prefix64_info *) p, length))
                                log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                "PREF64 prefix has invalid prefix length.");
                        break;
                }}
        }

        rt->rindex = sizeof(struct nd_router_advert);
        return 0;
}

int sd_ndisc_router_get_hop_limit(sd_ndisc_router *rt, uint8_t *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        *ret = rt->hop_limit;
        return 0;
}

int sd_ndisc_router_get_reachable_time(sd_ndisc_router *rt, uint64_t *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        *ret = rt->reachable_time_usec;
        return 0;
}

int sd_ndisc_router_get_retransmission_time(sd_ndisc_router *rt, uint64_t *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        *ret = rt->retransmission_time_usec;
        return 0;
}

int sd_ndisc_router_get_icmp6_ratelimit(sd_ndisc_router *rt, uint64_t *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        *ret = rt->icmp6_ratelimit_usec;
        return 0;
}

int sd_ndisc_router_get_flags(sd_ndisc_router *rt, uint64_t *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        *ret = rt->flags;
        return 0;
}

int sd_ndisc_router_get_lifetime(sd_ndisc_router *rt, uint64_t *ret) {
        assert_return(rt, -EINVAL);

        if (ret)
                *ret = rt->lifetime_usec;

        return rt->lifetime_usec > 0; /* Indicate if the router is still valid or not. */
}

int sd_ndisc_router_get_preference(sd_ndisc_router *rt, unsigned *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        *ret = rt->preference;
        return 0;
}

int sd_ndisc_router_get_mtu(sd_ndisc_router *rt, uint32_t *ret) {
        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        if (rt->mtu <= 0)
                return -ENODATA;

        *ret = rt->mtu;
        return 0;
}

int sd_ndisc_router_option_rewind(sd_ndisc_router *rt) {
        assert_return(rt, -EINVAL);

        assert(rt->packet);
        assert(rt->packet->raw_size >= sizeof(struct nd_router_advert));

        rt->rindex = sizeof(struct nd_router_advert);
        return rt->rindex < rt->packet->raw_size;
}

int sd_ndisc_router_option_next(sd_ndisc_router *rt) {
        size_t length;
        int r;

        assert_return(rt, -EINVAL);

        r = ndisc_option_parse(rt->packet, rt->rindex, NULL, &length, NULL);
        if (r < 0)
                return r;

        rt->rindex += length;
        return rt->rindex < rt->packet->raw_size;
}

int sd_ndisc_router_option_get_type(sd_ndisc_router *rt, uint8_t *ret) {
        assert_return(rt, -EINVAL);
        return ndisc_option_parse(rt->packet, rt->rindex, ret, NULL, NULL);
}

int sd_ndisc_router_option_is_type(sd_ndisc_router *rt, uint8_t type) {
        uint8_t k;
        int r;

        assert_return(rt, -EINVAL);

        r = sd_ndisc_router_option_get_type(rt, &k);
        if (r < 0)
                return r;

        return type == k;
}

int sd_ndisc_router_option_get_raw(sd_ndisc_router *rt, const uint8_t **ret, size_t *ret_size) {
        assert_return(rt, -EINVAL);
        return ndisc_option_parse(rt->packet, rt->rindex, NULL, ret_size, ret);
}

static int get_prefix_info(sd_ndisc_router *rt, const struct nd_opt_prefix_info **ret) {
        const struct nd_opt_prefix_info *ri;
        const uint8_t *p;
        size_t length;
        uint8_t type;
        int r;

        assert(rt);
        assert(ret);

        r = ndisc_option_parse(rt->packet, rt->rindex, &type, &length, &p);
        if (r < 0)
                return r;

        if (type != SD_NDISC_OPTION_PREFIX_INFORMATION)
                return -EMEDIUMTYPE;

        if (length != sizeof(struct nd_opt_prefix_info))
                return -EBADMSG;

        ri = (const struct nd_opt_prefix_info*) p;
        if (ri->nd_opt_pi_prefix_len > 128)
                return -EBADMSG;

        *ret = ri;
        return 0;
}

int sd_ndisc_router_prefix_get_valid_lifetime(sd_ndisc_router *rt, uint64_t *ret) {
        const struct nd_opt_prefix_info *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_prefix_info(rt, &ri);
        if (r < 0)
                return r;

        *ret = be32_sec_to_usec(ri->nd_opt_pi_valid_time, /* max_as_infinity = */ true);
        return 0;
}

int sd_ndisc_router_prefix_get_preferred_lifetime(sd_ndisc_router *rt, uint64_t *ret) {
        const struct nd_opt_prefix_info *pi;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_prefix_info(rt, &pi);
        if (r < 0)
                return r;

        *ret = be32_sec_to_usec(pi->nd_opt_pi_preferred_time, /* max_as_infinity = */ true);
        return 0;
}

int sd_ndisc_router_prefix_get_flags(sd_ndisc_router *rt, uint8_t *ret) {
        const struct nd_opt_prefix_info *pi;
        uint8_t flags;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_prefix_info(rt, &pi);
        if (r < 0)
                return r;

        flags = pi->nd_opt_pi_flags_reserved;

        if ((flags & ND_OPT_PI_FLAG_AUTO) && (pi->nd_opt_pi_prefix_len != 64)) {
                log_ndisc(NULL, "Invalid prefix length, ignoring prefix for stateless autoconfiguration.");
                flags &= ~ND_OPT_PI_FLAG_AUTO;
        }

        *ret = flags;
        return 0;
}

int sd_ndisc_router_prefix_get_address(sd_ndisc_router *rt, struct in6_addr *ret) {
        const struct nd_opt_prefix_info *pi;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_prefix_info(rt, &pi);
        if (r < 0)
                return r;

        *ret = pi->nd_opt_pi_prefix;
        return 0;
}

int sd_ndisc_router_prefix_get_prefixlen(sd_ndisc_router *rt, unsigned *ret) {
        const struct nd_opt_prefix_info *pi;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_prefix_info(rt, &pi);
        if (r < 0)
                return r;

        *ret = pi->nd_opt_pi_prefix_len;
        return 0;
}

static int get_route_info(sd_ndisc_router *rt, const uint8_t **ret) {
        const uint8_t *p;
        size_t length;
        uint8_t type;
        int r;

        assert(rt);
        assert(ret);

        r = ndisc_option_parse(rt->packet, rt->rindex, &type, &length, &p);
        if (r < 0)
                return r;

        if (type != SD_NDISC_OPTION_ROUTE_INFORMATION)
                return -EMEDIUMTYPE;

        if (length != 16)
                return -EBADMSG;

        if (p[2] > 128)
                return -EBADMSG;

        *ret = p;
        return 0;
}

int sd_ndisc_router_route_get_lifetime(sd_ndisc_router *rt, uint64_t *ret) {
        const uint8_t *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_route_info(rt, &ri);
        if (r < 0)
                return r;

        *ret = unaligned_be32_sec_to_usec(ri + 4, /* max_as_infinity = */ true);
        return 0;
}

int sd_ndisc_router_route_get_address(sd_ndisc_router *rt, struct in6_addr *ret) {
        const uint8_t *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_route_info(rt, &ri);
        if (r < 0)
                return r;

        zero(*ret);
        memcpy(ret, ri + 8, ri[1] * 8 - 8);

        return 0;
}

int sd_ndisc_router_route_get_prefixlen(sd_ndisc_router *rt, unsigned *ret) {
        const uint8_t *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_route_info(rt, &ri);
        if (r < 0)
                return r;

        *ret = ri[2];
        return 0;
}

int sd_ndisc_router_route_get_preference(sd_ndisc_router *rt, unsigned *ret) {
        const uint8_t *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_route_info(rt, &ri);
        if (r < 0)
                return r;

        if (!IN_SET((ri[3] >> 3) & 3, SD_NDISC_PREFERENCE_LOW, SD_NDISC_PREFERENCE_MEDIUM, SD_NDISC_PREFERENCE_HIGH))
                return -EOPNOTSUPP;

        *ret = (ri[3] >> 3) & 3;
        return 0;
}

static int get_rdnss_info(sd_ndisc_router *rt, const uint8_t **ret) {
        const uint8_t *p;
        size_t length;
        uint8_t type;
        int r;

        assert(rt);
        assert(ret);

        r = ndisc_option_parse(rt->packet, rt->rindex, &type, &length, &p);
        if (r < 0)
                return r;

        if (type != SD_NDISC_OPTION_RDNSS)
                return -EMEDIUMTYPE;

        if (length < 3*8 || (length % (2*8)) != 1*8)
                return -EBADMSG;

        *ret = p;
        return 0;
}

int sd_ndisc_router_rdnss_get_addresses(sd_ndisc_router *rt, const struct in6_addr **ret) {
        const uint8_t *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_rdnss_info(rt, &ri);
        if (r < 0)
                return r;

        *ret = (const struct in6_addr*) (ri + 8);
        return (ri[1] - 1) / 2;
}

int sd_ndisc_router_rdnss_get_lifetime(sd_ndisc_router *rt, uint64_t *ret) {
        const uint8_t *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_rdnss_info(rt, &ri);
        if (r < 0)
                return r;

        *ret = unaligned_be32_sec_to_usec(ri + 4, /* max_as_infinity = */ true);
        return 0;
}

static int get_dnssl_info(sd_ndisc_router *rt, const uint8_t **ret) {
        const uint8_t *p;
        size_t length;
        uint8_t type;
        int r;

        assert(rt);
        assert(ret);

        r = ndisc_option_parse(rt->packet, rt->rindex, &type, &length, &p);
        if (r < 0)
                return r;

        if (type != SD_NDISC_OPTION_DNSSL)
                return -EMEDIUMTYPE;

        if (length < 2*8)
                return -EBADMSG;

        *ret = p;
        return 0;
}

int sd_ndisc_router_dnssl_get_domains(sd_ndisc_router *rt, char ***ret) {
        _cleanup_strv_free_ char **l = NULL;
        _cleanup_free_ char *e = NULL;
        size_t n = 0, left;
        const uint8_t *ri, *p;
        bool first = true;
        int r;
        unsigned k = 0;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_dnssl_info(rt, &ri);
        if (r < 0)
                return r;

        p = ri + 8;
        left = (ri[1] - 1) * 8;

        for (;;) {
                if (left == 0) {

                        if (n > 0) /* Not properly NUL terminated */
                                return -EBADMSG;

                        break;
                }

                if (*p == 0) {
                        /* Found NUL termination */

                        if (n > 0) {
                                _cleanup_free_ char *normalized = NULL;

                                e[n] = 0;
                                r = dns_name_normalize(e, 0, &normalized);
                                if (r < 0) {
                                        _cleanup_free_ char *escaped = cescape(e);
                                        log_debug_errno(r, "Failed to normalize advertised domain name \"%s\": %m", strna(escaped));
                                        /* Here, do not propagate error code from dns_name_normalize() except for ENOMEM. */
                                        return r == -ENOMEM ? -ENOMEM : -EBADMSG;
                                }

                                /* Ignore the root domain name or "localhost" and friends */
                                if (!is_localhost(normalized) &&
                                    !dns_name_is_root(normalized)) {

                                        if (strv_push(&l, normalized) < 0)
                                                return -ENOMEM;

                                        normalized = NULL;
                                        k++;
                                }
                        }

                        n = 0;
                        first = true;
                        p++, left--;
                        continue;
                }

                /* Check for compression (which is not allowed) */
                if (*p > 63)
                        return -EBADMSG;

                if (1U + *p + 1U > left)
                        return -EBADMSG;

                if (!GREEDY_REALLOC(e, n + !first + DNS_LABEL_ESCAPED_MAX + 1U))
                        return -ENOMEM;

                if (first)
                        first = false;
                else
                        e[n++] = '.';

                r = dns_label_escape((char*) p+1, *p, e + n, DNS_LABEL_ESCAPED_MAX);
                if (r < 0) {
                        _cleanup_free_ char *escaped = cescape_length((const char*) p+1, *p);
                        log_debug_errno(r, "Failed to escape advertised domain name \"%s\": %m", strna(escaped));
                        /* Here, do not propagate error code from dns_label_escape() except for ENOMEM. */
                        return r == -ENOMEM ? -ENOMEM : -EBADMSG;
                }

                n += r;

                left -= 1 + *p;
                p += 1 + *p;
        }

        if (strv_isempty(l)) {
                *ret = NULL;
                return 0;
        }

        *ret = TAKE_PTR(l);

        return k;
}

int sd_ndisc_router_dnssl_get_lifetime(sd_ndisc_router *rt, uint64_t *ret) {
        const uint8_t *ri;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_dnssl_info(rt, &ri);
        if (r < 0)
                return r;

        *ret = unaligned_be32_sec_to_usec(ri + 4, /* max_as_infinity = */ true);
        return 0;
}

int sd_ndisc_router_captive_portal_get_uri(sd_ndisc_router *rt, const char **ret, size_t *ret_size) {
        const uint8_t *p;
        size_t length;
        uint8_t type;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);
        assert_return(ret_size, -EINVAL);

        r = ndisc_option_parse(rt->packet, rt->rindex, &type, &length, &p);
        if (r < 0)
                return r;

        if (type != SD_NDISC_OPTION_CAPTIVE_PORTAL)
                return -EMEDIUMTYPE;

        /* The length field has units of 8 octets */
        assert(length % 8 == 0);
        if (length == 0)
                return -EBADMSG;

        /* Check that the message is not truncated by an embedded NUL.
         * NUL padding to a multiple of 8 is expected. */
        size_t size = strnlen((const char*) (p + 2), length - 2);
        if (DIV_ROUND_UP(size + 2, 8) != length / 8)
                return -EBADMSG;

        /* Let's not return an empty buffer */
        if (size == 0) {
                *ret = NULL;
                *ret_size = 0;
                return 0;
        }

        *ret = (char*) (p + 2);
        *ret_size = size;
        return 0;
}

static int get_pref64_prefix_info(sd_ndisc_router *rt, const struct nd_opt_prefix64_info **ret) {
        const struct nd_opt_prefix64_info *ri;
        const uint8_t *p;
        size_t length;
        uint8_t type;
        int r;

        assert(rt);
        assert(ret);

        r = ndisc_option_parse(rt->packet, rt->rindex, &type, &length, &p);
        if (r < 0)
                return r;

        if (type != SD_NDISC_OPTION_PREF64)
                return -EMEDIUMTYPE;

        if (length != sizeof(struct nd_opt_prefix64_info))
                return -EBADMSG;

        ri = (const struct nd_opt_prefix64_info *) p;
        if (!pref64_option_verify(ri, length))
                return -EBADMSG;

        *ret = ri;
        return 0;
}

int sd_ndisc_router_prefix64_get_prefix(sd_ndisc_router *rt, struct in6_addr *ret) {
        const struct nd_opt_prefix64_info *pi;
        struct in6_addr a = {};
        unsigned prefixlen;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_pref64_prefix_info(rt, &pi);
        if (r < 0)
                return r;

        r = sd_ndisc_router_prefix64_get_prefixlen(rt, &prefixlen);
        if (r < 0)
                return r;

        memcpy(&a, pi->prefix, sizeof(pi->prefix));
        in6_addr_mask(&a, prefixlen);
        /* extra safety check for refusing malformed prefix. */
        if (memcmp(&a, pi->prefix, sizeof(pi->prefix)) != 0)
                return -EBADMSG;

        *ret = a;
        return 0;
}

int sd_ndisc_router_prefix64_get_prefixlen(sd_ndisc_router *rt, unsigned *ret) {
        const struct nd_opt_prefix64_info *pi;
        uint16_t lifetime_prefix_len;
        uint8_t prefix_len;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_pref64_prefix_info(rt, &pi);
        if (r < 0)
              return r;

        lifetime_prefix_len = be16toh(pi->lifetime_and_plc);
        pref64_plc_to_prefix_length(lifetime_prefix_len, &prefix_len);

        *ret = prefix_len;
        return 0;
}

int sd_ndisc_router_prefix64_get_lifetime(sd_ndisc_router *rt, uint64_t *ret) {
        const struct nd_opt_prefix64_info *pi;
        uint16_t lifetime_prefix_len;
        int r;

        assert_return(rt, -EINVAL);
        assert_return(ret, -EINVAL);

        r = get_pref64_prefix_info(rt, &pi);
        if (r < 0)
                return r;

        lifetime_prefix_len = be16toh(pi->lifetime_and_plc);

        *ret = (lifetime_prefix_len & PREF64_SCALED_LIFETIME_MASK) * USEC_PER_SEC;
        return 0;
}
