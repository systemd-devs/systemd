/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "sd-id128.h"

typedef enum MkfsFlags {
        MKFS_QUIET                        = 1 << 0,  /* Suppress mkfs command output */
        MKFS_DISCARD                      = 1 << 1,  /* Enable 'discard' mode on the filesystem */
} MkfsFlags;

int mkfs_exists(const char *fstype);

int mkfs_supports_root_option(const char *fstype);

int make_filesystem(
                const char *node,
                const char *fstype,
                const char *label,
                const char *root,
                sd_id128_t uuid,
                MkfsFlags flags,
                uint64_t sector_size,
                char *compression,
                char *compression_level,
                char * const *extra_mkfs_args);

int mkfs_options_from_env(const char *component, const char *fstype, char ***ret);
