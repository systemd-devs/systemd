/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>

#include "build.h"
#include "ether-addr-util.h"
#include "fd-util.h"
#include "hexdecoct.h"
#include "icmp6-util.h"
#include "in-addr-util.h"
#include "iovec-util.h"
#include "main-func.h"
#include "ndisc-option.h"
#include "netlink-util.h"
#include "parse-util.h"
#include "socket-util.h"
#include "strv.h"
#include "time-util.h"

static int arg_ifindex = 0;
static int arg_icmp6_type = 0;
static union in_addr_union arg_dest = IN_ADDR_NULL;
static uint8_t arg_hop_limit = 0;
static bool arg_is_managed = false;
static bool arg_is_other = false;
static uint8_t arg_preference = false;
static usec_t arg_lifetime = 0;
static usec_t arg_reachable = 0;
static usec_t arg_retransmit = 0;
static uint32_t arg_na_flags = 0;
static union in_addr_union arg_target_address = IN_ADDR_NULL;
static union in_addr_union arg_redirect_destination = IN_ADDR_NULL;
static bool arg_set_source_mac = false;
static struct ether_addr arg_source_mac = {};
static bool arg_set_target_mac = false;
static struct ether_addr arg_target_mac = {};
static struct ip6_hdr *arg_redirected_header = NULL;
static bool arg_set_mtu = false;
static uint32_t arg_mtu = 0;

STATIC_DESTRUCTOR_REGISTER(arg_redirected_header, freep);

static int parse_icmp6_type(const char *str) {
        if (STR_IN_SET(str, "router-solicit", "rs", "RS"))
                return ND_ROUTER_SOLICIT;
        if (STR_IN_SET(str, "router-advertisement", "ra", "RA"))
                return ND_ROUTER_ADVERT;
        if (STR_IN_SET(str, "neighbor-solicit", "ns", "NS"))
                return ND_NEIGHBOR_SOLICIT;
        if (STR_IN_SET(str, "neighbor-advertisement", "na", "NA"))
                return ND_NEIGHBOR_ADVERT;
        if (STR_IN_SET(str, "redirect", "rd", "RD"))
                return ND_REDIRECT;
        return -EINVAL;
}

static int parse_preference(const char *str) {
        if (streq(str, "low"))
                return SD_NDISC_PREFERENCE_LOW;
        if (streq(str, "medium"))
                return SD_NDISC_PREFERENCE_MEDIUM;
        if (streq(str, "high"))
                return SD_NDISC_PREFERENCE_HIGH;
        if (streq(str, "reserved"))
                return SD_NDISC_PREFERENCE_RESERVED;
        return -EINVAL;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_RA_HOP_LIMIT,
                ARG_RA_MANAGED,
                ARG_RA_OTHER,
                ARG_RA_PREFERENCE,
                ARG_RA_LIFETIME,
                ARG_RA_REACHABLE,
                ARG_RA_RETRANSMIT,
                ARG_NA_ROUTER,
                ARG_NA_SOLICITED,
                ARG_NA_OVERRIDE,
                ARG_TARGET_ADDRESS,
                ARG_REDIRECT_DESTINATION,
                ARG_OPTION_SOURCE_LL,
                ARG_OPTION_TARGET_LL,
                ARG_OPTION_PREFIX,
                ARG_OPTION_REDIRECTED_HEADER,
                ARG_OPTION_MTU,
                ARG_OPTION_ROUTE,
                ARG_OPTION_RDNSS,
                ARG_OPTION_FLAGS_EXTENSION,
                ARG_OPTION_DNSSL,
                ARG_OPTION_CAPTIVE_PORTAL,
                ARG_OPTION_PREFIX64,
        };

        static const struct option options[] = {
                { "version",              no_argument,       NULL, ARG_VERSION                  },
                { "interface",            required_argument, NULL, 'i'                          },
                { "type",                 required_argument, NULL, 't'                          },
                { "dest",                 required_argument, NULL, 'd'                          },
                /* For Router Advertisement */
                { "hop-limit",            required_argument, NULL, ARG_RA_HOP_LIMIT             },
                { "managed",              required_argument, NULL, ARG_RA_MANAGED               },
                { "other",                required_argument, NULL, ARG_RA_OTHER                 },
                { "preference",           required_argument, NULL, ARG_RA_PREFERENCE            },
                { "lifetime",             required_argument, NULL, ARG_RA_LIFETIME              },
                { "reachable-time",       required_argument, NULL, ARG_RA_REACHABLE             },
                { "retransmit-timer",     required_argument, NULL, ARG_RA_RETRANSMIT            },
                /* For Neighbor Advertisement */
                { "is-router",            required_argument, NULL, ARG_NA_ROUTER                },
                { "is-solicited",         required_argument, NULL, ARG_NA_SOLICITED             },
                { "is-override",          required_argument, NULL, ARG_NA_OVERRIDE              },
                /* For Neighbor Solicit, Neighbor Advertisement, and Redirect */
                { "target-address",       required_argument, NULL, ARG_TARGET_ADDRESS           },
                /* For Redirect */
                { "redirect-destination", required_argument, NULL, ARG_REDIRECT_DESTINATION     },
                /* Options */
                { "source-ll-address",    required_argument, NULL, ARG_OPTION_SOURCE_LL         },
                { "target-ll-address",    required_argument, NULL, ARG_OPTION_TARGET_LL         },
                { "prefix",               required_argument, NULL, ARG_OPTION_PREFIX            },
                { "redirected-header",    required_argument, NULL, ARG_OPTION_REDIRECTED_HEADER },
                { "mtu",                  required_argument, NULL, ARG_OPTION_MTU               },
                { "route",                required_argument, NULL, ARG_OPTION_ROUTE             },
                { "rdnss",                required_argument, NULL, ARG_OPTION_RDNSS             },
                { "flags-extension",      required_argument, NULL, ARG_OPTION_FLAGS_EXTENSION   },
                { "dnssl",                required_argument, NULL, ARG_OPTION_DNSSL             },
                { "captive-portal",       required_argument, NULL, ARG_OPTION_CAPTIVE_PORTAL    },
                { "prefix64",             required_argument, NULL, ARG_OPTION_PREFIX64          },
                {}
        };

        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "i:t:d:", options, NULL)) >= 0) {

                switch (c) {

                case ARG_VERSION:
                        return version();

                case 'i':
                        r = rtnl_resolve_interface_or_warn(&rtnl, optarg);
                        if (r < 0)
                                return r;
                        arg_ifindex = r;
                        break;

                case 't':
                        r = parse_icmp6_type(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse message type: %m");
                        arg_icmp6_type = r;
                        break;

                case 'd':
                        r = in_addr_from_string(AF_INET6, optarg, &arg_dest);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse destination address: %m");
                        if (!in6_addr_is_link_local(&arg_dest.in6))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "The destination address %s is not a link-local address.", optarg);
                        break;

                case ARG_RA_HOP_LIMIT:
                        r = safe_atou8(optarg, &arg_hop_limit);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse hop limit: %m");
                        break;

                case ARG_RA_MANAGED:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse managed flag: %m");
                        arg_is_managed = r;
                        break;

                case ARG_RA_OTHER:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse other flag: %m");
                        arg_is_other = r;
                        break;

                case ARG_RA_PREFERENCE:
                        r = parse_preference(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse preference: %m");
                        arg_preference = r;
                        break;

                case ARG_RA_LIFETIME:
                        r = parse_sec(optarg, &arg_lifetime);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse lifetime: %m");
                        break;

                case ARG_RA_REACHABLE:
                        r = parse_sec(optarg, &arg_reachable);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse reachable time: %m");
                        break;

                case ARG_RA_RETRANSMIT:
                        r = parse_sec(optarg, &arg_retransmit);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse retransmit timer: %m");
                        break;

                case ARG_NA_ROUTER:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse is-router flag: %m");
                        SET_FLAG(arg_na_flags, ND_NA_FLAG_ROUTER, r);
                        break;

                case ARG_NA_SOLICITED:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse is-solicited flag: %m");
                        SET_FLAG(arg_na_flags, ND_NA_FLAG_SOLICITED, r);
                        break;

                case ARG_NA_OVERRIDE:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse is-override flag: %m");
                        SET_FLAG(arg_na_flags, ND_NA_FLAG_OVERRIDE, r);
                        break;

                case ARG_TARGET_ADDRESS:
                        r = in_addr_from_string(AF_INET6, optarg, &arg_target_address);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse target address: %m");
                        break;

                case ARG_REDIRECT_DESTINATION:
                        r = in_addr_from_string(AF_INET6, optarg, &arg_redirect_destination);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse destination address: %m");
                        break;

                case ARG_OPTION_SOURCE_LL:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse source LL address option: %m");
                        arg_set_source_mac = r;
                        break;

                case ARG_OPTION_TARGET_LL:
                        r = parse_ether_addr(optarg, &arg_target_mac);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse target LL address option: %m");
                        arg_set_target_mac = true;
                        break;

                case ARG_OPTION_REDIRECTED_HEADER: {
                        _cleanup_free_ void *p = NULL;
                        size_t len;

                        r = unbase64mem(optarg, &p, &len);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse redirected header: %m");

                        if (len < sizeof(struct ip6_hdr))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid redirected header.");

                        arg_redirected_header = TAKE_PTR(p);
                        break;
                }
                case ARG_OPTION_MTU:
                        r = safe_atou32(optarg, &arg_mtu);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse MTU: %m");
                        arg_set_mtu = true;
                        break;

                case ARG_OPTION_PREFIX:
                case ARG_OPTION_ROUTE:
                case ARG_OPTION_RDNSS:
                case ARG_OPTION_FLAGS_EXTENSION:
                case ARG_OPTION_DNSSL:
                case ARG_OPTION_CAPTIVE_PORTAL:
                case ARG_OPTION_PREFIX64:
                        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Unsupported option %i.", c);

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }
        }

        if (arg_ifindex <= 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "--interface/-i option is mandatory.");

        if (arg_icmp6_type <= 0)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "--type/-t option is mandatory.");

        if (in6_addr_is_null(&arg_dest.in6)) {
                if (IN_SET(arg_icmp6_type, ND_ROUTER_ADVERT, ND_NEIGHBOR_ADVERT, ND_REDIRECT))
                        arg_dest.in6 = (struct in6_addr) IN6ADDR_ALL_NODES_MULTICAST_INIT;
                else
                        arg_dest.in6 = (struct in6_addr) IN6ADDR_ALL_ROUTERS_MULTICAST_INIT;
        }

        if (arg_set_source_mac) {
                struct hw_addr_data hw_addr;

                r = rtnl_get_link_info(&rtnl, arg_ifindex,
                                       /* ret_iftype = */ NULL,
                                       /* ret_flags = */ NULL,
                                       /* ret_kind = */ NULL,
                                       &hw_addr,
                                       /* ret_permanent_hw_addr = */ NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to get the source link-layer address: %m");

                if (hw_addr.length != sizeof(struct ether_addr))
                        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                               "Unsupported hardware address length %zu: %m",
                                               hw_addr.length);

                arg_source_mac = hw_addr.ether;
        }

        return 1;
}

static int send_icmp6(int fd, const struct icmp6_hdr *hdr) {
        _cleanup_set_free_ Set *options = NULL;
        int r;

        assert(fd >= 0);
        assert(hdr);

        if (arg_set_source_mac) {
                r = ndisc_option_add_link_layer_address(&options, 0, SD_NDISC_OPTION_SOURCE_LL_ADDRESS, &arg_source_mac);
                if (r < 0)
                        return r;
        }

        if (arg_set_target_mac) {
                r = ndisc_option_add_link_layer_address(&options, 0, SD_NDISC_OPTION_TARGET_LL_ADDRESS, &arg_target_mac);
                if (r < 0)
                        return r;
        }

        if (arg_redirected_header) {
                r = ndisc_option_add_redirected_header(&options, 0, arg_redirected_header);
                if (r < 0)
                        return r;
        }

        if (arg_set_mtu) {
                r = ndisc_option_add_mtu(&options, 0, arg_mtu);
                if (r < 0)
                        return r;
        }

        struct iovec *iov = NULL;
        size_t n_iov = 0;
        CLEANUP_ARRAY(iov, n_iov, iovec_array_free);

        r = ndisc_build_iov(hdr, options, &iov, &n_iov);
        if (r < 0)
                return r;

        struct sockaddr_in6 dst_sockaddr = {
                .sin6_family = AF_INET6,
                .sin6_addr = arg_dest.in6,
        };
        struct msghdr msg = {
                .msg_name = &dst_sockaddr,
                .msg_namelen = sizeof(dst_sockaddr),
                .msg_iov = iov,
                .msg_iovlen = n_iov,
        };

        if (sendmsg(fd, &msg, 0) < 0)
                return log_error_errno(errno, "Failed to send message: %m");

        return 0;
}

static int send_neighbor_advertisement(int fd) {
        struct nd_neighbor_advert hdr = {
                .nd_na_type = ND_NEIGHBOR_ADVERT,
                .nd_na_flags_reserved = arg_na_flags,
                .nd_na_target = arg_target_address.in6,
        };

        assert(fd >= 0);

        return send_icmp6(fd, &hdr.nd_na_hdr);
}

static int send_redirect(int fd) {
        struct nd_redirect hdr = {
                .nd_rd_type = ND_REDIRECT,
                .nd_rd_target = arg_target_address.in6,
                .nd_rd_dst = arg_redirect_destination.in6,
        };

        assert(fd >= 0);

        return send_icmp6(fd, &hdr.nd_rd_hdr);
}

static int run(int argc, char *argv[]) {
        _cleanup_close_ int fd = -EBADF;
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        fd = icmp6_bind(arg_ifindex, /* is_router = */ false);
        if (fd < 0)
                return log_error_errno(fd, "Failed to bind socket to interface: %m");

        switch (arg_icmp6_type) {
        case ND_ROUTER_SOLICIT:
        case ND_ROUTER_ADVERT:
        case ND_NEIGHBOR_SOLICIT:
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Unsupported ICMPv6 type.");
        case ND_NEIGHBOR_ADVERT:
                return send_neighbor_advertisement(fd);
        case ND_REDIRECT:
                return send_redirect(fd);
        default:
                assert_not_reached();
        }

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
