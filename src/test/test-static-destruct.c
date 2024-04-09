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
static int *integers = NULL;
static size_t n_integers = 0;

static void test_destroy(int *b) {
        (*b)++;
}

static void test_strings_destroy(char **array, size_t n) {
        ASSERT_EQ(n, 3u);
        assert_se(strv_equal(array, STRV_MAKE("a", "bbb", "ccc")));

        strv_free(array);
}

static void test_integers_destroy(int *array, size_t n) {
        ASSERT_EQ(n, 10u);

        for (size_t i = 0; i < n; i++)
                assert_se(array[i] == (int)(i * i));

        free(array);
}

STATIC_DESTRUCTOR_REGISTER(foo, test_destroy);
STATIC_DESTRUCTOR_REGISTER(bar, test_destroy);
STATIC_DESTRUCTOR_REGISTER(bar, test_destroy);
STATIC_DESTRUCTOR_REGISTER(baz, test_destroy);
STATIC_DESTRUCTOR_REGISTER(baz, test_destroy);
STATIC_DESTRUCTOR_REGISTER(baz, test_destroy);
STATIC_DESTRUCTOR_REGISTER(memory, freep);
STATIC_ARRAY_DESTRUCTOR_REGISTER(strings, n_strings, test_strings_destroy);
STATIC_ARRAY_DESTRUCTOR_REGISTER(integers, n_integers, test_integers_destroy);

TEST(static_destruct) {
        assert_se(foo == 0 && bar == 0 && baz == 0);
        assert_se(memory = strdup("hallo"));
        assert_se(strings = strv_new("a", "bbb", "ccc"));
        n_strings = strv_length(strings);
        n_integers = 10;
        assert_se(integers = new(int, n_integers));
        for (size_t i = 0; i < n_integers; i++)
                integers[i] = i * i;

        static_destruct();

        assert_se(foo == 1 && bar == 2 && baz == 3);
        ASSERT_FALSE(memory);
        ASSERT_FALSE(strings);
        ASSERT_EQ(n_strings, 0u);
        ASSERT_FALSE(integers);
        ASSERT_EQ(n_integers, 0u);
}

DEFINE_TEST_MAIN(LOG_INFO);
