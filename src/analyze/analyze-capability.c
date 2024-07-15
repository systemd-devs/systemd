/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "analyze.h"
#include "analyze-capability.h"
#include "cap-list.h"
#include "capability-util.h"
#include "format-table.h"
#include "parse-util.h"

static int table_add_capability(Table *table, int c)
{
        int r = table_add_many(table,
                               TABLE_STRING, capability_to_name(c) ?: "cap_???",
                               TABLE_UINT, c);
        if (r < 0)
                return table_log_add_error(r);
        return r;
}

int verb_capabilities(int argc, char *argv[], void *userdata) {
        _cleanup_(table_unrefp) Table *table = NULL;
        unsigned last_cap;
        uint64_t cap_mask;
        int r;

        table = table_new("name", "number");
        if (!table)
                return log_oom();

        (void) table_set_align_percent(table, table_get_cell(table, 0, 1), 100);

        /* Determine the maximum of the last cap known by the kernel and by us */
        last_cap = MAX((unsigned) CAP_LAST_CAP, cap_last_cap());

        if (argc == 1)
                for (unsigned c = 0; c <= last_cap; c++) {
                        r = table_add_capability(table, c);
                        if (r < 0)
                                return r;
                }
        else if (argc == 2 && safe_atoux64(argv[1], &cap_mask) >= 0) {
                int c = 0;

                while (cap_mask != 0) {
                        if (cap_mask & 1) {
                                if ((unsigned) c > last_cap)
                                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Capability %d not known.", c);

                                r = table_add_capability(table, c);
                                if (r < 0)
                                        return r;
                        }
                        ++c;
                        cap_mask >>= 1;
                }

                (void) table_set_sort(table, (size_t) 1);
        } else {
                for (int i = 1; i < argc; i++) {
                        int c;

                        c = capability_from_name(argv[i]);
                        if (c < 0 || (unsigned) c > last_cap)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Capability \"%s\" not known.", argv[i]);

                        r = table_add_capability(table, c);
                        if (r < 0)
                                return r;
                }

                (void) table_set_sort(table, (size_t) 1);
        }

        r = table_print_with_pager(table, arg_json_format_flags, arg_pager_flags, arg_legend);
        if (r < 0)
                return log_error_errno(r, "Failed to output table: %m");

        return EXIT_SUCCESS;
}
