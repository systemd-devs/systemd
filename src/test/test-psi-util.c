/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "parse-util.h"
#include "psi-util.h"
#include "tests.h"

static void test_read_mem_pressure(void) {
        _cleanup_(unlink_tempfilep) char path[] = "/tmp/pressurereadtestXXXXXX";
        ResourcePressure rp;

        if (geteuid() != 0)
                return (void) log_tests_skipped("not root");

        assert_se(mkstemp(path));

        assert_se(read_resource_pressure("/verylikelynonexistentpath", PRESSURE_TYPE_SOME, &rp) < 0);
        assert_se(read_resource_pressure(path, PRESSURE_TYPE_SOME, &rp) < 0);

        assert_se(write_string_file(path, "herpdederp\n", WRITE_STRING_FILE_CREATE) == 0);
        assert_se(read_resource_pressure(path, PRESSURE_TYPE_SOME, &rp) < 0);

        /* Pressure file with some invalid values */
        assert_se(write_string_file(path, "some avg10=0.22=55 avg60=0.17=8 avg300=1.11=00 total=58761459\n"
                                          "full avg10=0.23=55 avg60=0.16=8 avg300=1.08=00 total=58464525", WRITE_STRING_FILE_CREATE) == 0);
        assert_se(read_resource_pressure(path, PRESSURE_TYPE_SOME, &rp) < 0);

        /* Same pressure valid values as below but with duplicate avg60 field */
        assert_se(write_string_file(path, "some avg10=0.22 avg60=0.17 avg60=0.18 avg300=1.11 total=58761459\n"
                                          "full avg10=0.23 avg60=0.16 avg300=1.08 total=58464525", WRITE_STRING_FILE_CREATE) == 0);
        assert_se(read_resource_pressure(path, PRESSURE_TYPE_SOME, &rp) < 0);

        assert_se(write_string_file(path, "some avg10=0.22 avg60=0.17 avg300=1.11 total=58761459\n"
                                          "full avg10=0.23 avg60=0.16 avg300=1.08 total=58464525", WRITE_STRING_FILE_CREATE) == 0);
        assert_se(read_resource_pressure(path, PRESSURE_TYPE_SOME, &rp) == 0);
        assert_se(INT_SIDE(rp.avg10) == 0);
        assert_se(DECIMAL_SIDE(rp.avg10) == 22);
        assert_se(INT_SIDE(rp.avg60) == 0);
        assert_se(DECIMAL_SIDE(rp.avg60) == 17);
        assert_se(INT_SIDE(rp.avg300) == 1);
        assert_se(DECIMAL_SIDE(rp.avg300) == 11);
        assert_se(rp.total == 58761459);
        assert(read_resource_pressure(path, PRESSURE_TYPE_FULL, &rp) == 0);
        assert_se(INT_SIDE(rp.avg10) == 0);
        assert_se(DECIMAL_SIDE(rp.avg10) == 23);
        assert_se(INT_SIDE(rp.avg60) == 0);
        assert_se(DECIMAL_SIDE(rp.avg60) == 16);
        assert_se(INT_SIDE(rp.avg300) == 1);
        assert_se(DECIMAL_SIDE(rp.avg300) == 8);
        assert_se(rp.total == 58464525);

        /* Pressure file with extra unsupported fields */
        assert_se(write_string_file(path, "some avg5=0.55 avg10=0.22 avg60=0.17 avg300=1.11 total=58761459\n"
                                          "full avg10=0.23 avg60=0.16 avg300=1.08 avg600=2.00 total=58464525", WRITE_STRING_FILE_CREATE) == 0);
        assert_se(read_resource_pressure(path, PRESSURE_TYPE_SOME, &rp) == 0);
        assert_se(INT_SIDE(rp.avg10) == 0);
        assert_se(DECIMAL_SIDE(rp.avg10) == 22);
        assert_se(INT_SIDE(rp.avg60) == 0);
        assert_se(DECIMAL_SIDE(rp.avg60) == 17);
        assert_se(INT_SIDE(rp.avg300) == 1);
        assert_se(DECIMAL_SIDE(rp.avg300) == 11);
        assert_se(rp.total == 58761459);
        assert(read_resource_pressure(path, PRESSURE_TYPE_FULL, &rp) == 0);
        assert_se(INT_SIDE(rp.avg10) == 0);
        assert_se(DECIMAL_SIDE(rp.avg10) == 23);
        assert_se(INT_SIDE(rp.avg60) == 0);
        assert_se(DECIMAL_SIDE(rp.avg60) == 16);
        assert_se(INT_SIDE(rp.avg300) == 1);
        assert_se(DECIMAL_SIDE(rp.avg300) == 8);
        assert_se(rp.total == 58464525);
}

int main(void) {
        test_setup_logging(LOG_DEBUG);
        test_read_mem_pressure();
        return 0;
}
