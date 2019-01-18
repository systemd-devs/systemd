/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef const char* (*lookup_t)(int);
typedef int (*reverse_t)(const char*);

static inline void _test_table(const char *name,
                               lookup_t lookup,
                               reverse_t reverse,
                               int size,
                               bool sparse) {
        int i, boring = 0;

        for (i = -1; i < size + 1; i++) {
                const char* val = lookup(i);
                int rev;

                if (val) {
                        rev = reverse(val);
                        boring = 0;
                } else {
                        rev = reverse("--no-such--value----");
                        boring += i >= 0;
                }

                if (boring < 1 || i == size)
                        printf("%s: %d → %s → %d\n", name, i, val, rev);
                else if (boring == 1)
                        printf("%*s  ...\n", (int) strlen(name), "");

                if (i >= 0 && i < size) {
                        if (sparse)
                                assert_se(rev == i || rev < 0);
                        else
                                assert_se(val != NULL && rev == i);
                } else
                        assert_se(val == NULL && rev < 0);
        }
}

#define test_table(lower, upper) \
        _test_table(STRINGIFY(lower), lower##_to_string, lower##_from_string, _##upper##_MAX, false)

#define test_table_sparse(lower, upper) \
        _test_table(STRINGIFY(lower), lower##_to_string, lower##_from_string, _##upper##_MAX, true)
