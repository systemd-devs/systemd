/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fuzz.h"
#include "nspawn-settings.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_(settings_freep) Settings *s = NULL;

        if (size == 0)
                return 0;

        f = fmemopen_unlocked((char*) data, size, "re");
        assert_se(f);

        (void) settings_load(f, "/dev/null", &s);

        return 0;
}
