/***
  This file is part of systemd.

  Copyright 2015 Ronny Chevalier

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "conf-parser.h"
#include "fd-util.h"
#include "fileio.h"
#include "log.h"
#include "macro.h"
#include "string-util.h"
#include "strv.h"
#include "util.h"

static void test_config_parse_path_one(const char *rvalue, const char *expected) {
        _cleanup_free_ char *path = NULL;

        assert_se(config_parse_path("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &path, NULL) >= 0);
        assert_se(streq_ptr(expected, path));
}

static void test_config_parse_log_level_one(const char *rvalue, int expected) {
        int log_level = 0;

        assert_se(config_parse_log_level("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &log_level, NULL) >= 0);
        assert_se(expected == log_level);
}

static void test_config_parse_log_facility_one(const char *rvalue, int expected) {
        int log_facility = 0;

        assert_se(config_parse_log_facility("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &log_facility, NULL) >= 0);
        assert_se(expected == log_facility);
}

static void test_config_parse_iec_size_one(const char *rvalue, size_t expected) {
        size_t iec_size = 0;

        assert_se(config_parse_iec_size("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &iec_size, NULL) >= 0);
        assert_se(expected == iec_size);
}

static void test_config_parse_si_size_one(const char *rvalue, size_t expected) {
        size_t si_size = 0;

        assert_se(config_parse_si_size("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &si_size, NULL) >= 0);
        assert_se(expected == si_size);
}

static void test_config_parse_int_one(const char *rvalue, int expected) {
        int v = -1;

        assert_se(config_parse_int("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &v, NULL) >= 0);
        assert_se(expected == v);
}

static void test_config_parse_unsigned_one(const char *rvalue, unsigned expected) {
        unsigned v = 0;

        assert_se(config_parse_unsigned("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &v, NULL) >= 0);
        assert_se(expected == v);
}

static void test_config_parse_strv_one(const char *rvalue, char **expected) {
        _cleanup_strv_free_ char **strv = NULL;

        assert_se(config_parse_strv("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &strv, NULL) >= 0);
        assert_se(strv_equal(expected, strv));
}

static void test_config_parse_mode_one(const char *rvalue, mode_t expected) {
        mode_t v = 0;

        assert_se(config_parse_mode("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &v, NULL) >= 0);
        assert_se(expected == v);
}

static void test_config_parse_sec_one(const char *rvalue, usec_t expected) {
        usec_t v = 0;

        assert_se(config_parse_sec("unit", "filename", 1, "section", 1, "lvalue", 0, rvalue, &v, NULL) >= 0);
        assert_se(expected == v);
}

static void test_config_parse_nsec_one(const char *rvalue, nsec_t expected) {
        nsec_t v = 0;

        assert_se(config_parse_nsec("unit", "filename", 1, "nsection", 1, "lvalue", 0, rvalue, &v, NULL) >= 0);
        assert_se(expected == v);
}

static void test_config_parse_path(void) {
        test_config_parse_path_one("/path", "/path");
        test_config_parse_path_one("/path//////////", "/path");
        test_config_parse_path_one("///path/foo///bar////bar//", "/path/foo/bar/bar");
        test_config_parse_path_one("/path/\xc3\x80", "/path/\xc3\x80");

        test_config_parse_path_one("not_absolute/path", NULL);
        test_config_parse_path_one("/path/\xc3\x7f", NULL);
}

static void test_config_parse_log_level(void) {
        test_config_parse_log_level_one("debug", LOG_DEBUG);
        test_config_parse_log_level_one("info", LOG_INFO);

        test_config_parse_log_level_one("garbage", 0);
}

static void test_config_parse_log_facility(void) {
        test_config_parse_log_facility_one("mail", LOG_MAIL);
        test_config_parse_log_facility_one("user", LOG_USER);

        test_config_parse_log_facility_one("garbage", 0);
}

static void test_config_parse_iec_size(void) {
        test_config_parse_iec_size_one("1024", 1024);
        test_config_parse_iec_size_one("2K", 2048);
        test_config_parse_iec_size_one("10M", 10 * 1024 * 1024);
        test_config_parse_iec_size_one("1G", 1 * 1024 * 1024 * 1024);
        test_config_parse_iec_size_one("0G", 0);
        test_config_parse_iec_size_one("0", 0);

        test_config_parse_iec_size_one("-982", 0);
        test_config_parse_iec_size_one("49874444198739873000000G", 0);
        test_config_parse_iec_size_one("garbage", 0);
}

static void test_config_parse_si_size(void) {
        test_config_parse_si_size_one("1024", 1024);
        test_config_parse_si_size_one("2K", 2000);
        test_config_parse_si_size_one("10M", 10 * 1000 * 1000);
        test_config_parse_si_size_one("1G", 1 * 1000 * 1000 * 1000);
        test_config_parse_si_size_one("0G", 0);
        test_config_parse_si_size_one("0", 0);

        test_config_parse_si_size_one("-982", 0);
        test_config_parse_si_size_one("49874444198739873000000G", 0);
        test_config_parse_si_size_one("garbage", 0);
}

static void test_config_parse_int(void) {
        test_config_parse_int_one("1024", 1024);
        test_config_parse_int_one("-1024", -1024);
        test_config_parse_int_one("0", 0);

        test_config_parse_int_one("99999999999999999999999999999999999999999999999999999999", -1);
        test_config_parse_int_one("-99999999999999999999999999999999999999999999999999999999", -1);
        test_config_parse_int_one("1G", -1);
        test_config_parse_int_one("garbage", -1);
}

static void test_config_parse_unsigned(void) {
        test_config_parse_unsigned_one("10241024", 10241024);
        test_config_parse_unsigned_one("1024", 1024);
        test_config_parse_unsigned_one("0", 0);

        test_config_parse_unsigned_one("99999999999999999999999999999999999999999999999999999999", 0);
        test_config_parse_unsigned_one("1G", 0);
        test_config_parse_unsigned_one("garbage", 0);
        test_config_parse_unsigned_one("1000garbage", 0);
}

static void test_config_parse_strv(void) {
        test_config_parse_strv_one("", STRV_MAKE_EMPTY);
        test_config_parse_strv_one("foo", STRV_MAKE("foo"));
        test_config_parse_strv_one("foo bar foo", STRV_MAKE("foo", "bar", "foo"));
        test_config_parse_strv_one("\"foo bar\" foo", STRV_MAKE("foo bar", "foo"));
        test_config_parse_strv_one("\xc3\x80", STRV_MAKE("\xc3\x80"));
        test_config_parse_strv_one("\xc3\x7f", STRV_MAKE_EMPTY);
}

static void test_config_parse_mode(void) {
        test_config_parse_mode_one("777", 0777);
        test_config_parse_mode_one("644", 0644);

        test_config_parse_mode_one("-777", 0);
        test_config_parse_mode_one("999", 0);
        test_config_parse_mode_one("garbage", 0);
        test_config_parse_mode_one("777garbage", 0);
        test_config_parse_mode_one("777 garbage", 0);
}

static void test_config_parse_sec(void) {
        test_config_parse_sec_one("1", 1 * USEC_PER_SEC);
        test_config_parse_sec_one("1s", 1 * USEC_PER_SEC);
        test_config_parse_sec_one("100ms", 100 * USEC_PER_MSEC);
        test_config_parse_sec_one("5min 20s", 5 * 60 * USEC_PER_SEC + 20 * USEC_PER_SEC);

        test_config_parse_sec_one("-1", 0);
        test_config_parse_sec_one("10foo", 0);
        test_config_parse_sec_one("garbage", 0);
}

static void test_config_parse_nsec(void) {
        test_config_parse_nsec_one("1", 1);
        test_config_parse_nsec_one("1s", 1 * NSEC_PER_SEC);
        test_config_parse_nsec_one("100ms", 100 * NSEC_PER_MSEC);
        test_config_parse_nsec_one("5min 20s", 5 * 60 * NSEC_PER_SEC + 20 * NSEC_PER_SEC);

        test_config_parse_nsec_one("-1", 0);
        test_config_parse_nsec_one("10foo", 0);
        test_config_parse_nsec_one("garbage", 0);
}

static void test_config_parse_iec_uint64(void) {
        uint64_t offset = 0;
        assert_se(config_parse_iec_uint64(NULL, "/this/file", 11, "Section", 22, "Size", 0, "4M", &offset, NULL) == 0);
        assert_se(offset == 4 * 1024 * 1024);

        assert_se(config_parse_iec_uint64(NULL, "/this/file", 11, "Section", 22, "Size", 0, "4.5M", &offset, NULL) == 0);
}

static const char* const config_file[] = {
        "[Section]\n"
        "setting1=1\n",

        "[Section]\n"
        "setting1=1",        /* no terminating newline */

        "\n\n\n\n[Section]\n\n\n"
        "setting1=1",        /* some whitespace, no terminating newline */

        "[Section]\n"
        "[Section]\n"
        "setting1=1\n"
        "setting1=2\n"
        "setting1=1\n",      /* repeated settings */

        "[Section]\n"
        "setting1=1\\\n"     /* normal continuation */
        "2\\\n"
        "3\n",

        "[Section]\n"
        "setting1=1\\\\\\\n" /* continuation with trailing escape symbols */
        "\\\\2\n",           /* note that C requires one level of escaping, so the
                              * parser gets "…1 BS BS BS NL BS BS 2 NL", which
                              * it translates into "…1 BS BS SP BS BS 2" */
};

static void test_config_parse(unsigned i, const char *s) {
        char name[] = "/tmp/test-conf-parser.XXXXXX";
        int fd, r;
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *setting1 = NULL;

        const ConfigTableItem items[] = {
                { "Section", "setting1",  config_parse_string,   0, &setting1},
                {}
        };

        log_info("== %s[%i] ==", __func__, i);

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);
        assert_se((size_t) write(fd, s, strlen(s)) == strlen(s));

        assert_se(lseek(fd, 0, SEEK_SET) == 0);
        assert_se(f = fdopen(fd, "r"));

        /*
        int config_parse(const char *unit,
                         const char *filename,
                         FILE *f,
                         const char *sections,
                         ConfigItemLookup lookup,
                         const void *table,
                         bool relaxed,
                         bool allow_include,
                         bool warn,
                         void *userdata)
        */

        r = config_parse(NULL, name, f,
                         "Section\0",
                         config_item_table_lookup, items,
                         false, false, true, NULL);
        assert_se(r == 0);

        switch (i) {
        case 0 ... 3:
                assert_se(streq(setting1, "1"));
                break;

        case 4:
                assert_se(streq(setting1, "1 2 3"));
                break;

        case 5:
                assert_se(streq(setting1, "1\\\\ \\\\2"));
                break;
        }
}

int main(int argc, char **argv) {
        unsigned i;

        log_parse_environment();
        log_open();

        test_config_parse_path();
        test_config_parse_log_level();
        test_config_parse_log_facility();
        test_config_parse_iec_size();
        test_config_parse_si_size();
        test_config_parse_int();
        test_config_parse_unsigned();
        test_config_parse_strv();
        test_config_parse_mode();
        test_config_parse_sec();
        test_config_parse_nsec();
        test_config_parse_iec_uint64();

        for (i = 0; i < ELEMENTSOF(config_file); i++)
                test_config_parse(i, config_file[i]);

        return 0;
}
