/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <sys/types.h>

typedef struct UidRangeEntry {
        uid_t start, nr;
} UidRangeEntry;

typedef struct UidRange {
        UidRangeEntry *entries;
        size_t n_entries;
} UidRange;

UidRange *uid_range_free(UidRange *range);
DEFINE_TRIVIAL_CLEANUP_FUNC(UidRange*, uid_range_free);

int uid_range_add_internal(UidRange **range, uid_t start, uid_t nr, bool coalesce);
static inline int uid_range_add(UidRange **range, uid_t start, uid_t nr) {
        return uid_range_add_internal(range, start, nr, true);
}
int uid_range_add_str(UidRange **range, const char *s);

int uid_range_next_lower(const UidRange *p, uid_t *uid);
bool uid_range_covers(const UidRange *p, uid_t start, uid_t nr);

static inline bool uid_range_contains(const UidRange *p, uid_t uid) {
        return uid_range_covers(p, uid, 1);
}

int uid_range_load_userns(UidRange **ret, const char *path);
