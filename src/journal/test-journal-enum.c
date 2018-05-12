/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering
***/

#include <stdio.h>

#include "sd-journal.h"

#include "journal-internal.h"
#include "log.h"
#include "macro.h"

int main(int argc, char *argv[]) {
        unsigned n = 0;
        _cleanup_(sd_journal_closep) sd_journal*j = NULL;

        log_set_max_level(LOG_DEBUG);

        assert_se(sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY) >= 0);

        assert_se(sd_journal_add_match(j, "_TRANSPORT=syslog", 0) >= 0);
        assert_se(sd_journal_add_match(j, "_UID=0", 0) >= 0);

        SD_JOURNAL_FOREACH_BACKWARDS(j) {
                const void *d;
                size_t l;

                assert_se(sd_journal_get_data(j, "MESSAGE", &d, &l) >= 0);

                printf("%.*s\n", (int) l, (char*) d);

                n++;
                if (n >= 10)
                        break;
        }

        return 0;
}
