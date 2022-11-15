/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "list.h"

typedef struct OpenFile {
        char *path;
        char *fdname;
        int flags;
        int fd;
        LIST_FIELDS(struct OpenFile, open_files);
} OpenFile;

const char *open_file_to_string(const OpenFile *open_file);
void free_open_file_fields(const OpenFile *open_file);
