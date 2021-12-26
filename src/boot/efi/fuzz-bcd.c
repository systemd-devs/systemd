/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "fuzz.h"
#include "utf8.h"

#include "bcd.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_free_ void *p = NULL;

        /* This limit was borrowed from src/boot/efi/boot.c */
        if (size > 100*1024)
                return 0;

        if (!getenv("SYSTEMD_LOG_LEVEL"))
                log_set_max_level(LOG_CRIT);

        p = memdup(data, size);
        assert_se(p);

        char16_t *title = get_bcd_title(p, size);
        if (title) {
                _cleanup_free_ char *title_utf8 = utf16_to_utf8(title, char16_strlen(title) * 2);
                log_info("%s", title_utf8);
        }
        return 0;
}
