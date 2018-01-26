/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

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
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "alloc-util.h"
#include "fs-util.h"
#include "macro.h"
#include "mkdir.h"
#include "path-util.h"
#include "stat-util.h"
#include "user-util.h"

int mkdir_safe_internal(const char *path, mode_t mode, uid_t uid, gid_t gid, bool follow_symlink, mkdir_func_t _mkdir) {
        struct stat st;
        int r;

        assert(_mkdir != mkdir);

        if (_mkdir(path, mode) >= 0) {
                r = chmod_and_chown(path, mode, uid, gid);
                if (r < 0)
                        return r;
        }

        if (lstat(path, &st) < 0)
                return -errno;

        if (follow_symlink && S_ISLNK(st.st_mode)) {
                _cleanup_free_ char *p = NULL;

                r = chase_symlinks(path, NULL, CHASE_NONEXISTENT, &p);
                if (r < 0)
                        return r;
                if (r == 0)
                        return mkdir_safe_internal(p, mode, uid, gid, false, _mkdir);

                if (lstat(p, &st) < 0)
                        return -errno;
        }

        if ((st.st_mode & 0007) > (mode & 0007) ||
            (st.st_mode & 0070) > (mode & 0070) ||
            (st.st_mode & 0700) > (mode & 0700) ||
            (uid != UID_INVALID && st.st_uid != uid) ||
            (gid != GID_INVALID && st.st_gid != gid) ||
            !S_ISDIR(st.st_mode))
                return -EEXIST;

        return 0;
}

int mkdir_errno_wrapper(const char *pathname, mode_t mode) {
        if (mkdir(pathname, mode) < 0)
                return -errno;
        return 0;
}

int mkdir_safe(const char *path, mode_t mode, uid_t uid, gid_t gid, bool follow_symlink) {
        return mkdir_safe_internal(path, mode, uid, gid, follow_symlink, mkdir_errno_wrapper);
}

int mkdir_parents_internal(const char *prefix, const char *path, mode_t mode, mkdir_func_t _mkdir) {
        const char *p, *e;
        int r;

        assert(path);
        assert(_mkdir != mkdir);

        if (prefix && !path_startswith(path, prefix))
                return -ENOTDIR;

        /* return immediately if directory exists */
        e = strrchr(path, '/');
        if (!e)
                return -EINVAL;

        if (e == path)
                return 0;

        p = strndupa(path, e - path);
        r = is_dir(p, true);
        if (r > 0)
                return 0;
        if (r == 0)
                return -ENOTDIR;

        /* create every parent directory in the path, except the last component */
        p = path + strspn(path, "/");
        for (;;) {
                char t[strlen(path) + 1];

                e = p + strcspn(p, "/");
                p = e + strspn(e, "/");

                /* Is this the last component? If so, then we're done */
                if (*p == 0)
                        return 0;

                memcpy(t, path, e - path);
                t[e-path] = 0;

                if (prefix && path_startswith(prefix, t))
                        continue;

                r = _mkdir(t, mode);
                if (r < 0 && r != -EEXIST)
                        return r;
        }
}

int mkdir_parents(const char *path, mode_t mode) {
        return mkdir_parents_internal(NULL, path, mode, mkdir_errno_wrapper);
}

int mkdir_p_internal(const char *prefix, const char *path, mode_t mode, mkdir_func_t _mkdir) {
        int r;

        /* Like mkdir -p */

        assert(_mkdir != mkdir);

        r = mkdir_parents_internal(prefix, path, mode, _mkdir);
        if (r < 0)
                return r;

        r = _mkdir(path, mode);
        if (r < 0 && (r != -EEXIST || is_dir(path, true) <= 0))
                return r;

        return 0;
}

int mkdir_p(const char *path, mode_t mode) {
        return mkdir_p_internal(NULL, path, mode, mkdir_errno_wrapper);
}
