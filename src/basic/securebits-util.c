/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2017 Yu Watanabe

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <stdio.h>

#include "alloc-util.h"
#include "extract-word.h"
#include "securebits.h"
#include "securebits-util.h"
#include "string-util.h"

int secure_bits_to_string_alloc(int i, char **s) {
        _cleanup_free_ char *str = NULL;
        size_t len;
        int r;

        assert(s);

        r = asprintf(&str, "%s%s%s%s%s%s",
                     (i & (1 << SECURE_KEEP_CAPS)) ? "keep-caps " : "",
                     (i & (1 << SECURE_KEEP_CAPS_LOCKED)) ? "keep-caps-locked " : "",
                     (i & (1 << SECURE_NO_SETUID_FIXUP)) ? "no-setuid-fixup " : "",
                     (i & (1 << SECURE_NO_SETUID_FIXUP_LOCKED)) ? "no-setuid-fixup-locked " : "",
                     (i & (1 << SECURE_NOROOT)) ? "noroot " : "",
                     (i & (1 << SECURE_NOROOT_LOCKED)) ? "noroot-locked " : "");
        if (r < 0)
                return -ENOMEM;

        len = strlen(str);
        if (len != 0)
                str[len - 1] = '\0';

        *s = str;
        str = NULL;

        return 0;
}

int secure_bits_from_string(const char *s) {
        int secure_bits = 0;
        const char *p;
        int r;

        for (p = s;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&p, &word, NULL, EXTRACT_QUOTES);
                if (r == -ENOMEM)
                        return r;
                if (r <= 0)
                        break;

                if (streq(word, "keep-caps"))
                        secure_bits |= 1 << SECURE_KEEP_CAPS;
                else if (streq(word, "keep-caps-locked"))
                        secure_bits |= 1 << SECURE_KEEP_CAPS_LOCKED;
                else if (streq(word, "no-setuid-fixup"))
                        secure_bits |= 1 << SECURE_NO_SETUID_FIXUP;
                else if (streq(word, "no-setuid-fixup-locked"))
                        secure_bits |= 1 << SECURE_NO_SETUID_FIXUP_LOCKED;
                else if (streq(word, "noroot"))
                        secure_bits |= 1 << SECURE_NOROOT;
                else if (streq(word, "noroot-locked"))
                        secure_bits |= 1 << SECURE_NOROOT_LOCKED;
        }

        return secure_bits;
}
