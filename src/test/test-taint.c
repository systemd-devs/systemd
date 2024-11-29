/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "manager.h"
#include "taint.h"
#include "tests.h"

TEST(taint_string) {
        Manager m = {};
        _cleanup_free_ char *a = taint_string(&m);
        assert_se(a);
        log_debug("taint string: '%s'", a);

        assert_se(!!strstr(a, "cgroupsv1") == (cg_all_unified() == 0));
}

DEFINE_TEST_MAIN(LOG_DEBUG);
