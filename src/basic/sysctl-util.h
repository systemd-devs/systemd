/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "macro.h"
#include "stdio-util.h"
#include "string-util.h"

char *sysctl_normalize(char *s);
int sysctl_read(const char *property, char **value);
int sysctl_write(const char *property, const char *value, Hashmap *cache);
int sysctl_writef(const char *property, Hashmap *cache, const char *format, ...) _printf_(3, 4);

int sysctl_read_ip_property(int af, const char *ifname, const char *property, char **ret);
int sysctl_write_ip_property(int af, const char *ifname, const char *property, const char *value, Hashmap *cache);
static inline int sysctl_write_ip_property_boolean(int af, const char *ifname, const char *property, bool value, Hashmap *cache) {
        return sysctl_write_ip_property(af, ifname, property, one_zero(value), cache);
}

int sysctl_write_ip_neighbor_property(int af, const char *ifname, const char *property, const char *value, Hashmap *cache);
static inline int sysctl_write_ip_neighbor_property_uint32(int af, const char *ifname, const char *property, uint32_t value, Hashmap *cache) {
        char buf[DECIMAL_STR_MAX(uint32_t)];
        xsprintf(buf, "%u", value);
        return sysctl_write_ip_neighbor_property(af, ifname, property, buf, cache);
}

#define DEFINE_SYSCTL_WRITE_IP_PROPERTY(name, type, format)           \
        static inline int sysctl_write_ip_property_##name(int af, const char *ifname, const char *property, type value, Hashmap *cache) { \
                char buf[DECIMAL_STR_MAX(type)];                        \
                xsprintf(buf, format, value);                           \
                return sysctl_write_ip_property(af, ifname, property, buf, cache); \
        }

DEFINE_SYSCTL_WRITE_IP_PROPERTY(int, int, "%i");
DEFINE_SYSCTL_WRITE_IP_PROPERTY(uint32, uint32_t, "%" PRIu32);
