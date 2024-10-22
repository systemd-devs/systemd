/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <sys/types.h>
#include <stdbool.h>

typedef struct LabelOps {
        int (*pre)(int dir_fd, const char *path, mode_t mode);
        int (*post)(int dir_fd, const char *path, bool created);
} LabelOps;

int label_ops_set(const LabelOps *label_ops);

int label_ops_pre(int dir_fd, const char *path, mode_t mode);
int label_ops_post(int dir_fd, const char *path, bool created);

void label_ops_reset(void);
