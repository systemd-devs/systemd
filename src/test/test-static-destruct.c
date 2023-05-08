/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "static-destruct.h"
#include "strv.h"
#include "tests.h"

static int foo = 0;
static int bar = 0;
static int baz = 0;
static char *memory = NULL;
static char **strings = NULL;
static size_t n_strings = 0;

static void test_destroy(int *b) {
        (*b)++;
}

static void test_array_destroy(char **array, size_t n) {
        FOREACH_ARRAY(e, array, n)
                free(*e);

        free(array);
}

STATIC_DESTRUCTOR_REGISTER(foo, test_destroy);
STATIC_DESTRUCTOR_REGISTER(bar, test_destroy);
STATIC_DESTRUCTOR_REGISTER(bar, test_destroy);
STATIC_DESTRUCTOR_REGISTER(baz, test_destroy);
STATIC_DESTRUCTOR_REGISTER(baz, test_destroy);
STATIC_DESTRUCTOR_REGISTER(baz, test_destroy);
STATIC_DESTRUCTOR_REGISTER(memory, freep);
STATIC_ARRAY_DESTRUCTOR_REGISTER(strings, n_strings, test_array_destroy);

TEST(static_destruct) {
        assert_se(foo == 0 && bar == 0 && baz == 0);
        assert_se(memory = strdup("hallo"));
        assert_se(strings = strv_new("a", "bbb", "ccc"));
        n_strings = strv_length(strings);

        static_destruct();

        assert_se(foo == 1 && bar == 2 && baz == 3);
        assert_se(!memory);
        assert_se(!strings);
        assert_se(n_strings == 0);
}

DEFINE_TEST_MAIN(LOG_INFO);
