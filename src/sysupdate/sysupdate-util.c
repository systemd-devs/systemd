/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sysupdate-util.h"
#include "path-util.h"

bool version_is_valid(const char *s) {
        if (isempty(s))
                return false;

        if (!filename_is_valid(s))
                return false;

        if (!in_charset(s, ALPHANUMERICAL ".,_-+"))
                return false;

        return true;
}
