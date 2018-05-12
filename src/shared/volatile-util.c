/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2015 Lennart Poettering
***/

#include <errno.h>

#include "alloc-util.h"
#include "macro.h"
#include "parse-util.h"
#include "proc-cmdline.h"
#include "string-util.h"
#include "volatile-util.h"

VolatileMode volatile_mode_from_string(const char *s) {
        int b;

        if (isempty(s))
                return _VOLATILE_MODE_INVALID;

        b = parse_boolean(s);
        if (b > 0)
                return VOLATILE_YES;
        if (b == 0)
                return VOLATILE_NO;

        if (streq(s, "state"))
                return VOLATILE_STATE;

        return _VOLATILE_MODE_INVALID;
}

int query_volatile_mode(VolatileMode *ret) {
        _cleanup_free_ char *mode = NULL;
        VolatileMode m = VOLATILE_NO;
        int r;

        r = proc_cmdline_get_key("systemd.volatile", PROC_CMDLINE_VALUE_OPTIONAL, &mode);
        if (r < 0)
                return r;
        if (r == 0)
                goto finish;

        if (mode) {
                m = volatile_mode_from_string(mode);
                if (m < 0)
                        return -EINVAL;
        } else
                m = VOLATILE_YES;

        r = 1;

finish:
        *ret = m;
        return r;
}
