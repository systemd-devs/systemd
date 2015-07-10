/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright (C) 2014 Tom Gundersen
  Copyright (C) 2014 Susant Sahani

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

#include <net/ethernet.h>
#include <arpa/inet.h>

#include "macro.h"
#include "lldp-tlv.h"

int tlv_section_new(tlv_section **ret) {
        tlv_section *s;

        s = new0(tlv_section, 1);
        if (!s)
                return -ENOMEM;

        *ret = s;

        return 0;
}

void tlv_section_free(tlv_section *m) {

        if (!m)
                return;

        free(m);
}

int tlv_packet_new(tlv_packet **ret) {
        tlv_packet *m;

        m = new0(tlv_packet, 1);
        if (!m)
                return -ENOMEM;

        LIST_HEAD_INIT(m->sections);
        m->n_ref = REFCNT_INIT;

        *ret = m;

        return 0;
}

sd_lldp_tlv *sd_lldp_tlv_unref(sd_lldp_tlv *m) {
        tlv_section *s, *n;

        if (!m)
                return m;

        if (REFCNT_DEC(m->n_ref) == 0) {
                LIST_FOREACH_SAFE(section, s, n, m->sections)
                        tlv_section_free(s);

                free(m);
                return NULL;
        }

        return m;
}

sd_lldp_tlv *sd_lldp_tlv_ref(sd_lldp_tlv *m) {
        if (m)
                assert_se(REFCNT_INC(m->n_ref) >= 2);

        return m;
}

int tlv_packet_append_bytes(tlv_packet *m, const void *data, size_t data_length) {
        uint8_t *p;

        assert_return(m, -EINVAL);
        assert_return(data, -EINVAL);
        assert_return(data_length, -EINVAL);

        if (m->length + data_length > ETHER_MAX_LEN)
                return -ENOMEM;

        p = m->pdu + m->length;
        memcpy(p, data, data_length);
        m->length += data_length;

        return 0;
}

int tlv_packet_append_u8(tlv_packet *m, uint8_t data) {

        assert_return(m, -EINVAL);

        return tlv_packet_append_bytes(m, &data, sizeof(uint8_t));
}

int tlv_packet_append_u16(tlv_packet *m, uint16_t data) {
        uint16_t type;

        assert_return(m, -EINVAL);

        type = htons(data);

        return tlv_packet_append_bytes(m, &type, sizeof(uint16_t));
}

int tlv_packet_append_u32(tlv_packet *m, uint32_t data) {
        uint32_t type;

        assert_return(m, -EINVAL);

        type = htonl(data);

        return tlv_packet_append_bytes(m, &type, sizeof(uint32_t));
}

int tlv_packet_append_string(tlv_packet *m, char *data, uint16_t size) {

        assert_return(m, -EINVAL);

        return tlv_packet_append_bytes(m, data, size);
}

int lldp_tlv_packet_open_container(tlv_packet *m, uint16_t type) {

        assert_return(m, -EINVAL);

        m->container_pos = m->pdu + m->length;

        return tlv_packet_append_u16(m, type << 9);
}

int lldp_tlv_packet_close_container(tlv_packet *m) {
        uint16_t type;

        assert_return(m, -EINVAL);
        assert_return(m->container_pos, -EINVAL);

        memcpy(&type, m->container_pos, sizeof(uint16_t));

        type |= htons(((m->pdu + m->length) - (m->container_pos + 2)) & 0x01ff);
        memcpy(m->container_pos, &type, sizeof(uint16_t));

        return 0;
}

static inline int tlv_packet_read_internal(tlv_section *m, void **data) {

        assert_return(m->read_pos, -EINVAL);

        *data = m->read_pos;

        return 0;
}

int tlv_packet_read_u8(tlv_packet *m, uint8_t *data) {
        void *val = NULL;
        int r;

        assert_return(m, -EINVAL);

        r = tlv_packet_read_internal(m->container,  &val);
        if (r < 0)
                return r;

        memcpy(data, val, sizeof(uint8_t));

        m->container->read_pos ++;

        return 0;
}

int tlv_packet_read_u16(tlv_packet *m, uint16_t *data) {
        uint16_t t;
        void *val = NULL;
        int r;

        assert_return(m, -EINVAL);

        r = tlv_packet_read_internal(m->container, &val);
        if (r < 0)
                return r;

        memcpy(&t, val, sizeof(uint16_t));
        *data = ntohs(t);

        m->container->read_pos += 2;

        return 0;
}

int tlv_packet_read_u32(tlv_packet *m, uint32_t *data) {
        uint32_t t;
        void *val;
        int r;

        assert_return(m, -EINVAL);

        r = tlv_packet_read_internal(m->container, &val);
        if (r < 0)
                return r;

        memcpy(&t, val, sizeof(uint32_t));
        *data = ntohl(t);

        m->container->read_pos += 4;

        return r;
}

int tlv_packet_read_string(tlv_packet *m, char **data, uint16_t *data_length) {
        void *val = NULL;
        int r;

        assert_return(m, -EINVAL);

        r = tlv_packet_read_internal(m->container, &val);
        if (r < 0)
                return r;

        *data = (char *) val;
        *data_length = m->container->length;

        m->container->read_pos += m->container->length;

        return 0;
}

int tlv_packet_read_bytes(tlv_packet *m, uint8_t **data, uint16_t *data_length) {
        void *val = NULL;
        int r;

        assert_return(m, -EINVAL);

        r = tlv_packet_read_internal(m->container, &val);
        if (r < 0)
                return r;

        *data = (uint8_t *) val;
        *data_length = m->container->length;

        m->container->read_pos += m->container->length;

        return 0;
}

/* parse raw TLV packet */
int tlv_packet_parse_pdu(tlv_packet *m, uint16_t size) {
        tlv_section *section, *tail;
        uint16_t t, l;
        uint8_t *p;
        int r;

        assert_return(m, -EINVAL);
        assert_return(size, -EINVAL);

        p = m->pdu;

        /* extract ethernet herader */
        memcpy(&m->mac, p, ETH_ALEN);
        p += sizeof(struct ether_header);

        for (l = 0; l <= size; ) {
                r = tlv_section_new(&section);
                if (r < 0)
                        return r;

                memcpy(&t, p, sizeof(uint16_t));

                section->type = ntohs(t) >> 9;
                section->length = ntohs(t) & 0x01ff;

                if (section->type == LLDP_TYPE_END || section->type >=_LLDP_TYPE_MAX) {
                        tlv_section_free(section);
                        break;
                }

                p += 2;
                section->data = p;

                LIST_FIND_TAIL(section, m->sections, tail);
                LIST_INSERT_AFTER(section, m->sections, tail, section);

                p += section->length;
                l += (section->length + 2);
        }

        return 0;
}

int lldp_tlv_packet_enter_container(tlv_packet *m, uint16_t type) {
        tlv_section *s;

        assert_return(m, -EINVAL);

        LIST_FOREACH(section, s, m->sections)
                if (s->type == type)
                        break;
        if (!s)
                return -1;

        m->container = s;

        m->container->read_pos = s->data;
        if (!m->container->read_pos) {
                m->container = 0;
                return -1;
        }

        return 0;
}

int lldp_tlv_packet_exit_container(tlv_packet *m) {
        assert_return(m, -EINVAL);

        m->container = 0;

        return 0;
}

int sd_lldp_tlv_read_chassis_id(sd_lldp_tlv *tlv,
                                uint8_t *type,
                                uint16_t *length,
                                uint8_t **data) {
        uint8_t subtype;
        int r;

        assert_return(tlv, -EINVAL);

        r = lldp_tlv_packet_enter_container(tlv, LLDP_TYPE_CHASSIS_ID);
        if (r < 0)
                goto out2;

        r = tlv_packet_read_u8(tlv, &subtype);
        if (r < 0)
                goto out1;

        switch (subtype) {
        case LLDP_CHASSIS_SUBTYPE_MAC_ADDRESS:

                r = tlv_packet_read_bytes(tlv, data, length);
                if (r < 0)
                        goto out1;

                break;
        default:
                r = -EOPNOTSUPP;
                break;
        }

        *type = subtype;

 out1:
        (void) lldp_tlv_packet_exit_container(tlv);

 out2:
        return r;
}

int sd_lldp_tlv_read_port_id(sd_lldp_tlv *tlv,
                             uint8_t *type,
                             uint16_t *length,
                             uint8_t **data) {
        uint8_t subtype;
        char *s;
        int r;

        assert_return(tlv, -EINVAL);

        r = lldp_tlv_packet_enter_container(tlv, LLDP_TYPE_PORT_ID);
        if (r < 0)
                goto out2;

        r = tlv_packet_read_u8(tlv, &subtype);
        if (r < 0)
                goto out1;

        switch (subtype) {
        case LLDP_PORT_SUBTYPE_PORT_COMPONENT:
        case LLDP_PORT_SUBTYPE_INTERFACE_ALIAS:
        case LLDP_PORT_SUBTYPE_INTERFACE_NAME:
        case LLDP_PORT_SUBTYPE_LOCALLY_ASSIGNED:

                r = tlv_packet_read_string(tlv, &s, length);
                if (r < 0)
                        goto out1;

                *data = (uint8_t *) s;

                break;
        case LLDP_PORT_SUBTYPE_MAC_ADDRESS:

                r = tlv_packet_read_bytes(tlv, data, length);
                if (r < 0)
                        goto out1;

                break;
        default:
                r = -EOPNOTSUPP;
                break;
        }

        *type = subtype;

 out1:
        (void) lldp_tlv_packet_exit_container(tlv);

 out2:
        return r;
}

int sd_lldp_tlv_read_ttl(sd_lldp_tlv *tlv, uint16_t *ttl) {
        int r;

        assert_return(tlv, -EINVAL);

        r = lldp_tlv_packet_enter_container(tlv, LLDP_TYPE_TTL);
        if (r < 0)
                goto out;

        r = tlv_packet_read_u16(tlv, ttl);

        (void) lldp_tlv_packet_exit_container(tlv);

 out:
        return r;
}

int sd_lldp_tlv_read_system_name(sd_lldp_tlv *tlv,
                                 uint16_t *length,
                                 char **data) {
        char *s;
        int r;

        assert_return(tlv, -EINVAL);

        r = lldp_tlv_packet_enter_container(tlv, LLDP_TYPE_SYSTEM_NAME);
        if (r < 0)
                return r;

        r = tlv_packet_read_string(tlv, &s, length);
        if (r < 0)
                goto out;

        *data = (char *) s;

 out:
        (void) lldp_tlv_packet_exit_container(tlv);

        return r;
}

int sd_lldp_tlv_read_system_description(sd_lldp_tlv *tlv,
                                        uint16_t *length,
                                        char **data) {
        char *s;
        int r;

        assert_return(tlv, -EINVAL);

        r = lldp_tlv_packet_enter_container(tlv, LLDP_TYPE_SYSTEM_DESCRIPTION);
        if (r < 0)
                return r;

        r = tlv_packet_read_string(tlv, &s, length);
        if (r < 0)
                goto out;

        *data = (char *) s;

 out:
        (void) lldp_tlv_packet_exit_container(tlv);

        return r;
}

int sd_lldp_tlv_read_port_description(sd_lldp_tlv *tlv,
                                      uint16_t *length,
                                      char **data) {
        char *s;
        int r;

        assert_return(tlv, -EINVAL);

        r = lldp_tlv_packet_enter_container(tlv, LLDP_TYPE_PORT_DESCRIPTION);
        if (r < 0)
                return r;

        r = tlv_packet_read_string(tlv, &s, length);
        if (r < 0)
                goto out;

        *data = (char *) s;

 out:
        (void) lldp_tlv_packet_exit_container(tlv);

        return r;
}

int sd_lldp_tlv_read_system_capability(sd_lldp_tlv *tlv, uint16_t *data) {
        int r;

        assert_return(tlv, -EINVAL);

        r = lldp_tlv_packet_enter_container(tlv, LLDP_TYPE_SYSTEM_CAPABILITIES);
        if (r < 0)
                return r;

        r = tlv_packet_read_u16(tlv, data);
        if (r < 0)
                goto out;

        return 0;
 out:

        (void) lldp_tlv_packet_exit_container(tlv);

        return r;
}
