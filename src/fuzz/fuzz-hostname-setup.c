/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fuzz.h"
#include "hostname-setup.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *ret = NULL;

        if (size == 0 || size > 65536)
                return 0;

        f = fmemopen_unlocked((char*) data, size, "re");
        assert_se(f);

        /* We don't want to fill the logs with messages about parse errors.
         * Disable most logging if not running standalone */
        if (!getenv("SYSTEMD_LOG_LEVEL"))
                log_set_max_level(LOG_CRIT);

        (void) read_etc_hostname_stream(f, &ret);

        return 0;
}
