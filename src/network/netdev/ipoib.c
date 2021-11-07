/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/if_arp.h>
#include <linux/if_link.h>

#include "ipoib.h"
#include "parse-util.h"
#include "string-table.h"

assert_cc((int) IP_OVER_INFINIBAND_MODE_DATAGRAM  == (int) IPOIB_MODE_DATAGRAM);
assert_cc((int) IP_OVER_INFINIBAND_MODE_CONNECTED == (int) IPOIB_MODE_CONNECTED);

static void netdev_ipoib_init(NetDev *netdev) {
        IPoIB *ipoib;

        assert(netdev);

        ipoib = IPOIB(netdev);

        assert(ipoib);

        ipoib->mode = _IP_OVER_INFINIBAND_MODE_INVALID;
        ipoib->umcast = -1;
}

static int netdev_ipoib_fill_message_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        IPoIB *ipoib;
        int r;

        assert(netdev);
        assert(link);
        assert(m);

        ipoib = IPOIB(netdev);

        assert(ipoib);

        if (ipoib->pkey > 0) {
                r = sd_netlink_message_append_u16(m, IFLA_IPOIB_PKEY, ipoib->pkey);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPOIB_PKEY attribute: %m");
        }

        if (ipoib->mode >= 0) {
                r = sd_netlink_message_append_u16(m, IFLA_IPOIB_MODE, ipoib->mode);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPOIB_MODE attribute: %m");
        }

        if (ipoib->umcast >= 0) {
                r = sd_netlink_message_append_u16(m, IFLA_IPOIB_UMCAST, ipoib->umcast);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_IPOIB_UMCAST attribute: %m");
        }

        return 0;
}

static const char * const ipoib_mode_table[_IP_OVER_INFINIBAND_MODE_MAX] = {
        [IP_OVER_INFINIBAND_MODE_DATAGRAM]  = "datagram",
        [IP_OVER_INFINIBAND_MODE_CONNECTED] = "connected",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_FROM_STRING(ipoib_mode, IPoIBMode);
DEFINE_CONFIG_PARSE_ENUM(config_parse_ipoib_mode, ipoib_mode, IPoIBMode, "Failed to parse IPoIB mode");

int config_parse_ipoib_pkey(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint16_t u, *pkey = data;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                *pkey = 0; /* 0 means unset. */
                return 0;
        }

        r = safe_atou16(rvalue, &u);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse IPoIB pkey '%s', ignoring assignment: %m",
                           rvalue);
                return 0;
        }
        if (u == 0 || u == 0x8000) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "IPoIB pkey cannot be 0 nor 0x8000, ignoring assignment: %s",
                           rvalue);
                return 0;
        }

        *pkey = u;
        return 0;
}


const NetDevVTable ipoib_vtable = {
        .object_size = sizeof(IPoIB),
        .sections = NETDEV_COMMON_SECTIONS "IPoIB\0",
        .init = netdev_ipoib_init,
        .fill_message_create = netdev_ipoib_fill_message_create,
        .create_type = NETDEV_CREATE_STACKED,
        .iftype = ARPHRD_INFINIBAND,
        .generate_mac = true,
};
