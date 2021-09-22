/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "label.h"
#include "macro.h"
#include "mkdir.h"
#include "selinux-util.h"
#include "smack-util.h"
#include "user-util.h"

int mkdir_label(const char *path, mode_t mode) {
        int r;

        assert(path);

        r = mac_selinux_create_file_prepare(path, S_IFDIR);
        if (r < 0)
                return r;

        r = mkdir_errno_wrapper(path, mode);
        mac_selinux_create_file_clear();
        if (r < 0)
                return r;

        return mac_smack_fix(path, 0);
}

int mkdirat_label(int dirfd, const char *path, mode_t mode) {
        int r;

        assert(path);

        r = mac_selinux_create_file_prepare_at(dirfd, path, S_IFDIR);
        if (r < 0)
                return r;

        r = mkdirat_errno_wrapper(dirfd, path, mode);
        mac_selinux_create_file_clear();
        if (r < 0)
                return r;

        return mac_smack_fix_at(dirfd, path, 0);
}

int mkdir_safe_label(const char *path, mode_t mode, uid_t uid, gid_t gid, MkdirFlags flags) {
        return mkdir_safe_internal(path, mode, uid, gid, flags, mkdir_label);
}

int mkdir_parents_label(const char *path, mode_t mode) {
        return mkdir_parents_internal(NULL, path, mode, UID_INVALID, UID_INVALID, 0, mkdir_label);
}

int mkdir_p_label(const char *path, mode_t mode) {
        return mkdir_p_internal(NULL, path, mode, UID_INVALID, UID_INVALID, 0, mkdir_label);
}

int mkdir_p_label_and_warn(const char *path, mode_t mode, const char *logsrc) {
        int r;

        r = mkdir_p_label(path, mode);
        if (r < 0 && r != -EEXIST)
                log_warning_errno(r, "%s%sFailed to create dir '%s': %m", logsrc ?: "", logsrc ? ": " : "", path);
        return r;
}
