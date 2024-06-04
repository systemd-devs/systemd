/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "macro.h"

struct strbuf {
        char *buf;
        size_t len;
        size_t cap;
        struct strbuf_node *root;

        size_t nodes_count;
        size_t in_count;
        size_t in_len;
        size_t dedup_len;
        size_t dedup_count;
};

struct strbuf_node {
        size_t value_off;
        size_t value_len;

        struct strbuf_child_entry *children;
        uint8_t children_count;
};

struct strbuf_child_entry {
        uint8_t c;
        struct strbuf_node *child;
};

struct strbuf* strbuf_new(void);
int strbuf_reserve(struct strbuf *str, size_t cap);
ssize_t strbuf_add_string(struct strbuf *str, const char *s, size_t len);
void strbuf_complete(struct strbuf *str);
struct strbuf* strbuf_free(struct strbuf *str);
DEFINE_TRIVIAL_CLEANUP_FUNC(struct strbuf*, strbuf_free);
