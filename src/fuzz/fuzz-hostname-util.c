/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "fuzz.h"
#include "hostname-util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *ret = NULL;

        if (size == 0)
                return 0;

        f = fmemopen((char*) data, size, "re");
        assert_se(f);

        /* We don't want to fill the logs with messages about parse errors.
         * Disable most logging if not running standalone */
        if (!getenv("SYSTEMD_LOG_LEVEL"))
                log_set_max_level(LOG_CRIT);

        (void) read_etc_hostname_stream(f, &ret);

        return 0;
}
