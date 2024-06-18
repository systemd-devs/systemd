/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "iovec-util.h"

int fido2_generate_salt(struct iovec *ret_salt);
int fido2_read_salt_file(const char *filename, uint64_t offset, size_t size, const char *client, const char *node, struct iovec *ret_salt);
