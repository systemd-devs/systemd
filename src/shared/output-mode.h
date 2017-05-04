#pragma once

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

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

#include "macro.h"

typedef enum OutputMode {
        OUTPUT_SHORT,
        OUTPUT_SHORT_FULL,
        OUTPUT_SHORT_ISO,
        OUTPUT_SHORT_ISO_PRECISE,
        OUTPUT_SHORT_PRECISE,
        OUTPUT_SHORT_MONOTONIC,
        OUTPUT_SHORT_UNIX,
        OUTPUT_VERBOSE,
        OUTPUT_EXPORT,
        OUTPUT_JSON,
        OUTPUT_JSON_PRETTY,
        OUTPUT_JSON_SSE,
        OUTPUT_CAT,
        _OUTPUT_MODE_MAX,
        _OUTPUT_MODE_INVALID = -1
} OutputMode;

/* The output flags definitions are shared by the logs and process tree output. Some apply to both, some only to the
 * logs output, others only to the process tree output. */

typedef enum OutputFlags {
        OUTPUT_SHOW_ALL       = 1 << 0,
        OUTPUT_FOLLOW         = 1 << 1,
        OUTPUT_WARN_CUTOFF    = 1 << 2,
        OUTPUT_FULL_WIDTH     = 1 << 3,
        OUTPUT_COLOR          = 1 << 4,
        OUTPUT_CATALOG        = 1 << 5,
        OUTPUT_BEGIN_NEWLINE  = 1 << 6,
        OUTPUT_UTC            = 1 << 7,
        OUTPUT_KERNEL_THREADS = 1 << 8,
        OUTPUT_NO_HOSTNAME    = 1 << 9,
} OutputFlags;

const char* output_mode_to_string(OutputMode m) _const_;
OutputMode output_mode_from_string(const char *s) _pure_;
