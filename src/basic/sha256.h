/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include <stdint.h>

#include "sha256-fundamental.h"
#include "string-util.h"

int sha256_fd(int fd, uint8_t ret[static SHA256_DIGEST_SIZE]);

int parse_sha256(const char *s, uint8_t res[static SHA256_DIGEST_SIZE]);

static inline bool valid_sha256(const char *s) {
        return s && in_charset(s, HEXDIGITS) && (strlen(s) == 64);
}
