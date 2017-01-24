#pragma once

/***
  This file is part of systemd.

  Copyright 2016 Lennart Poettering

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

typedef enum VolatileMode {
        VOLATILE_NO,
        VOLATILE_YES,
        VOLATILE_STATE,
        _VOLATILE_MODE_MAX,
        _VOLATILE_MODE_INVALID = -1
} VolatileMode;

VolatileMode volatile_mode_from_string(const char *s);

int query_volatile_mode(VolatileMode *ret);
