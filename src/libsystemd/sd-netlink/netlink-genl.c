/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/genetlink.h>

#include "sd-netlink.h"

#include "alloc-util.h"
#include "netlink-genl.h"
#include "netlink-internal.h"

typedef struct {
        const char* name;
        uint8_t version;
} genl_family;

static const genl_family genl_families[] = {
        [SD_GENL_ID_CTRL]   = { .name = "",          .version = 1 },
        [SD_GENL_WIREGUARD] = { .name = "wireguard", .version = 1 },
        [SD_GENL_FOU]       = { .name = "fou",       .version = 1 },
        [SD_GENL_L2TP]      = { .name = "l2tp",      .version = 1 },
        [SD_GENL_MACSEC]    = { .name = "macsec",    .version = 1 },
        [SD_GENL_NL80211]   = { .name = "nl80211",   .version = 1 },
        [SD_GENL_BATADV]    = { .name = "batadv",    .version = 1 },
};

int sd_genl_socket_open(sd_netlink **ret) {
        return netlink_open_family(ret, NETLINK_GENERIC);
}

static int genl_message_new(sd_netlink *nl, sd_genl_family_t family, uint16_t nlmsg_type, uint8_t cmd, sd_netlink_message **ret) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        const NLType *type;
        size_t size;
        int r;

        assert(nl);
        assert(nl->protocol == NETLINK_GENERIC);
        assert(ret);

        r = type_system_root_get_type(nl, &type, nlmsg_type);
        if (r < 0)
                return r;

        r = message_new_empty(nl, &m);
        if (r < 0)
                return r;

        size = NLMSG_SPACE(sizeof(struct genlmsghdr));
        m->hdr = malloc0(size);
        if (!m->hdr)
                return -ENOMEM;

        m->hdr->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        m->hdr->nlmsg_len = size;
        m->hdr->nlmsg_type = nlmsg_type;

        m->containers[0].type_system = type_get_type_system(type);

        *(struct genlmsghdr *) NLMSG_DATA(m->hdr) = (struct genlmsghdr) {
                .cmd = cmd,
                .version = genl_families[family].version,
        };

        *ret = TAKE_PTR(m);

        return 0;
}

static int lookup_nlmsg_type(sd_netlink *nl, sd_genl_family_t family, uint16_t *ret) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        uint16_t u;
        void *v;
        int r;

        assert(nl);
        assert(nl->protocol == NETLINK_GENERIC);
        assert(ret);

        if (family == SD_GENL_ID_CTRL) {
                *ret = GENL_ID_CTRL;
                return 0;
        }

        v = hashmap_get(nl->genl_family_to_nlmsg_type, INT_TO_PTR(family));
        if (v) {
                *ret = PTR_TO_UINT(v);
                return 0;
        }

        r = genl_message_new(nl, SD_GENL_ID_CTRL, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, &req);
        if (r < 0)
                return r;

        r = sd_netlink_message_append_string(req, CTRL_ATTR_FAMILY_NAME, genl_families[family].name);
        if (r < 0)
                return r;

        r = sd_netlink_call(nl, req, 0, &reply);
        if (r < 0)
                return r;

        r = sd_netlink_message_read_u16(reply, CTRL_ATTR_FAMILY_ID, &u);
        if (r < 0)
                return r;

        r = hashmap_ensure_put(&nl->genl_family_to_nlmsg_type, NULL, INT_TO_PTR(family), UINT_TO_PTR(u));
        if (r < 0)
                return r;

        r = hashmap_ensure_put(&nl->nlmsg_type_to_genl_family, NULL, UINT_TO_PTR(u), INT_TO_PTR(family));
        if (r < 0)
                return r;

        *ret = u;
        return 0;
}

int sd_genl_message_new(sd_netlink *nl, sd_genl_family_t family, uint8_t cmd, sd_netlink_message **ret) {
        uint16_t nlmsg_type = 0;  /* Unnecessary initialization to appease gcc */
        int r;

        assert_return(nl, -EINVAL);
        assert_return(nl->protocol == NETLINK_GENERIC, -EINVAL);
        assert_return(ret, -EINVAL);

        r = lookup_nlmsg_type(nl, family, &nlmsg_type);
        if (r < 0)
                return r;

        return genl_message_new(nl, family, nlmsg_type, cmd, ret);
}

int nlmsg_type_to_genl_family(const sd_netlink *nl, uint16_t nlmsg_type, sd_genl_family_t *ret) {
        void *p;

        assert(nl);
        assert(nl->protocol == NETLINK_GENERIC);
        assert(ret);

        if (nlmsg_type == NLMSG_ERROR)
                *ret = SD_GENL_ERROR;
        else if (nlmsg_type == NLMSG_DONE)
                *ret = SD_GENL_DONE;
        else if (nlmsg_type == GENL_ID_CTRL)
                *ret = SD_GENL_ID_CTRL;
        else {
                p = hashmap_get(nl->nlmsg_type_to_genl_family, UINT_TO_PTR(nlmsg_type));
                if (!p)
                        return -EOPNOTSUPP;

                *ret = PTR_TO_INT(p);
        }

        return 0;
}

int sd_genl_message_get_family(sd_netlink *nl, sd_netlink_message *m, sd_genl_family_t *ret) {
        uint16_t nlmsg_type;
        int r;

        assert_return(nl, -EINVAL);
        assert_return(nl->protocol == NETLINK_GENERIC, -EINVAL);
        assert_return(m, -EINVAL);
        assert_return(ret, -EINVAL);

        r = sd_netlink_message_get_type(m, &nlmsg_type);
        if (r < 0)
                return r;

        return nlmsg_type_to_genl_family(nl, nlmsg_type, ret);
}
