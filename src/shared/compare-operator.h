/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <errno.h>
#include <stdbool.h>

typedef enum CompareOperator {
        /* Listed in order of checking. Note that some comparators are prefixes of others, hence the longest
         * should be listed first. */

        /* fnmatch() compare operators */
        _COMPARE_OPERATOR_FNMATCH_FIRST,
        COMPARE_FNMATCH_EQUAL = _COMPARE_OPERATOR_FNMATCH_FIRST,
        COMPARE_FNMATCH_UNEQUAL,
        _COMPARE_OPERATOR_FNMATCH_LAST = COMPARE_FNMATCH_UNEQUAL,

        /* Order compare operators */
        _COMPARE_OPERATOR_ORDER_FIRST,
        COMPARE_LOWER_OR_EQUAL = _COMPARE_OPERATOR_ORDER_FIRST,
        COMPARE_GREATER_OR_EQUAL,
        COMPARE_LOWER,
        COMPARE_GREATER,
        COMPARE_EQUAL,
        COMPARE_UNEQUAL,
        _COMPARE_OPERATOR_ORDER_LAST = COMPARE_UNEQUAL,

        _COMPARE_OPERATOR_MAX,
        _COMPARE_OPERATOR_INVALID = -EINVAL,
} CompareOperator;

static inline bool COMPARE_OPERATOR_IS_FNMATCH(CompareOperator c) {
        return c >= _COMPARE_OPERATOR_FNMATCH_FIRST && c <= _COMPARE_OPERATOR_FNMATCH_LAST;
}

static inline bool COMPARE_OPERATOR_IS_ORDER(CompareOperator c) {
        return c >= _COMPARE_OPERATOR_ORDER_FIRST && c <= _COMPARE_OPERATOR_ORDER_LAST;
}

typedef enum CompareOperatorParseFlags {
        COMPARE_ALLOW_FNMATCH   = 1 << 0,
} CompareOperatorParseFlags;

CompareOperator parse_compare_operator(const char **s, CompareOperatorParseFlags flags);

int test_order(int k, CompareOperator op);

int version_or_fnmatch_compare(CompareOperator op, const char *a, const char *b);
