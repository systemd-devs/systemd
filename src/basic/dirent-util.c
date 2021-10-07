/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <sys/stat.h>

#include "dirent-util.h"
#include "path-util.h"
#include "stat-util.h"
#include "string-util.h"

static int dirent_ensure_type(DIR *d, struct dirent *de) {
        STRUCT_STATX_DEFINE(sx);
        int r;

        assert(d);
        assert(de);

        if (de->d_type != DT_UNKNOWN)
                return 0;

        if (dot_or_dot_dot(de->d_name)) {
                de->d_type = DT_DIR;
                return 0;
        }

        /* Let's ask only for the type, nothing else. */
        r = statx_fallback(dirfd(d), de->d_name, AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT, STATX_TYPE, &sx);
        if (r < 0)
                return r;

        assert(FLAGS_SET(sx.stx_mask, STATX_TYPE));
        de->d_type = IFTODT(sx.stx_mode);

        /* If the inode is passed too, update the field, i.e. report most recent data */
        if (FLAGS_SET(sx.stx_mask, STATX_INO))
                de->d_ino = sx.stx_ino;

        return 0;
}

bool dirent_is_file(const struct dirent *de) {
        assert(de);

        if (!IN_SET(de->d_type, DT_REG, DT_LNK, DT_UNKNOWN))
                return false;

        if (hidden_or_backup_file(de->d_name))
                return false;

        return true;
}

bool dirent_is_file_with_suffix(const struct dirent *de, const char *suffix) {
        assert(de);

        if (!IN_SET(de->d_type, DT_REG, DT_LNK, DT_UNKNOWN))
                return false;

        if (de->d_name[0] == '.')
                return false;

        if (!suffix)
                return true;

        return endswith(de->d_name, suffix);
}

struct dirent *readdir_ensure_type(DIR *d) {
        struct dirent *de;

        assert(d);

        errno = 0;
        de = readdir(d);
        if (de)
                (void) dirent_ensure_type(d, de);
        return de;
}

struct dirent *readdir_no_dot(DIR *dirp) {
        struct dirent *d;

        for (;;) {
                d = readdir_ensure_type(dirp);
                if (d && dot_or_dot_dot(d->d_name))
                        continue;
                return d;
        }
}
