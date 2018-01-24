/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

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

#include <libintl.h>
#include <stdbool.h>
#include <locale.h>

#include "macro.h"

typedef enum LocaleVariable {
        /* We don't list LC_ALL here on purpose. People should be
         * using LANG instead. */

        VARIABLE_LANG,
        VARIABLE_LANGUAGE,
        VARIABLE_LC_CTYPE,
        VARIABLE_LC_NUMERIC,
        VARIABLE_LC_TIME,
        VARIABLE_LC_COLLATE,
        VARIABLE_LC_MONETARY,
        VARIABLE_LC_MESSAGES,
        VARIABLE_LC_PAPER,
        VARIABLE_LC_NAME,
        VARIABLE_LC_ADDRESS,
        VARIABLE_LC_TELEPHONE,
        VARIABLE_LC_MEASUREMENT,
        VARIABLE_LC_IDENTIFICATION,
        _VARIABLE_LC_MAX,
        _VARIABLE_LC_INVALID = -1
} LocaleVariable;

int get_locales(char ***l);
bool locale_is_valid(const char *name);

#define _(String) gettext(String)
#define N_(String) String
void init_gettext(void);

bool is_locale_utf8(void);

typedef enum {
        TREE_VERTICAL,
        TREE_BRANCH,
        TREE_RIGHT,
        TREE_SPACE,
        TRIANGULAR_BULLET,
        BLACK_CIRCLE,
        ARROW,
        MDASH,
        _SPECIAL_GLYPH_MAX
} SpecialGlyph;

const char *special_glyph(SpecialGlyph code) _const_;

const char* locale_variable_to_string(LocaleVariable i) _const_;
LocaleVariable locale_variable_from_string(const char *s) _pure_;

int get_keymaps(char ***l);
bool keymap_is_valid(const char *name);

static inline void freelocalep(locale_t *p) {
        if (*p == (locale_t) 0)
                return;

        freelocale(*p);
}
