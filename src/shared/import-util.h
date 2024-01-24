/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "macro.h"

typedef enum ImportVerify {
        IMPORT_VERIFY_NO,
        IMPORT_VERIFY_CHECKSUM,
        IMPORT_VERIFY_SIGNATURE,
        _IMPORT_VERIFY_MAX,
        _IMPORT_VERIFY_INVALID = -EINVAL,
} ImportVerify;

typedef enum ImportCompressType {
        IMPORT_COMPRESS_UNKNOWN,
        IMPORT_COMPRESS_UNCOMPRESSED,
        IMPORT_COMPRESS_XZ,
        IMPORT_COMPRESS_GZIP,
        IMPORT_COMPRESS_BZIP2,
        IMPORT_COMPRESS_ZSTD,
        _IMPORT_COMPRESS_TYPE_MAX,
        _IMPORT_COMPRESS_TYPE_INVALID = -EINVAL,
} ImportCompressType;

typedef enum ImportCompressLevel {
        IMPORT_COMPRESS_LEVEL_UNKNOWN = 0,

        /* We are not using this anywhere, but it makes the enum a signed integer type */
        _IMPORT_COMPRESS_LEVEL_INVALID = INT_MIN,
} ImportCompressLevel;

int import_url_last_component(const char *url, char **ret);

int import_url_change_suffix(const char *url, size_t n_drop_components, const char *suffix, char **ret);

static inline int import_url_change_last_component(const char *url, const char *suffix, char **ret) {
        return import_url_change_suffix(url, 1, suffix, ret);
}

static inline int import_url_append_component(const char *url, const char *suffix, char **ret) {
        return import_url_change_suffix(url, 0, suffix, ret);
}

const char* import_verify_to_string(ImportVerify v) _const_;
ImportVerify import_verify_from_string(const char *s) _pure_;

const char* import_compress_type_to_string(ImportCompressType t) _const_;
ImportCompressType import_compress_type_from_string(const char *s) _pure_;

ImportCompressType tar_filename_to_compression(const char *name);
ImportCompressType raw_filename_to_compression(const char *name);
int tar_strip_suffixes(const char *name, char **ret);
int raw_strip_suffixes(const char *name, char **ret);

int import_assign_pool_quota_and_warn(const char *path);

int import_set_nocow_and_log(int fd, const char *path);
