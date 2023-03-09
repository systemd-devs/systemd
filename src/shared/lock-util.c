/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "lock-util.h"
#include "macro.h"
#include "missing_fcntl.h"
#include "path-util.h"

int make_lock_file(const char *p, int operation, LockFile *ret) {
        _cleanup_close_ int fd = -EBADF;
        _cleanup_free_ char *t = NULL;
        int r;

        assert(p);
        assert(ret);

        /* We use UNPOSIX locks as they have nice semantics, and are mostly compatible with NFS. */

        t = strdup(p);
        if (!t)
                return -ENOMEM;

        for (;;) {
                struct flock fl = {
                        .l_type = (operation & ~LOCK_NB) == LOCK_EX ? F_WRLCK : F_RDLCK,
                        .l_whence = SEEK_SET,
                };
                struct stat st;

                fd = open(p, O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NOCTTY, 0600);
                if (fd < 0)
                        return -errno;

                r = fcntl(fd, (operation & LOCK_NB) ? F_OFD_SETLK : F_OFD_SETLKW, &fl);
                if (r < 0)
                        return errno == EAGAIN ? -EBUSY : -errno;

                /* If we acquired the lock, let's check if the file still exists in the file system. If not,
                 * then the previous exclusive owner removed it and then closed it. In such a case our
                 * acquired lock is worthless, hence try again. */

                if (fstat(fd, &st) < 0)
                        return -errno;
                if (st.st_nlink > 0)
                        break;

                fd = safe_close(fd);
        }

        *ret = (LockFile) {
                .path = TAKE_PTR(t),
                .fd = TAKE_FD(fd),
                .operation = operation,
        };

        return r;
}

int make_lock_file_for(const char *p, int operation, LockFile *ret) {
        _cleanup_free_ char *fn = NULL, *dn = NULL, *t = NULL;
        int r;

        assert(p);
        assert(ret);

        r = path_extract_filename(p, &fn);
        if (r < 0)
                return r;

        r = path_extract_directory(p, &dn);
        if (r < 0)
                return r;

        t = strjoin(dn, "/.#", fn, ".lck");
        if (!t)
                return -ENOMEM;

        return make_lock_file(t, operation, ret);
}

void release_lock_file(LockFile *f) {
        if (!f)
                return;

        if (f->path) {

                /* If we are the exclusive owner we can safely delete
                 * the lock file itself. If we are not the exclusive
                 * owner, we can try becoming it. */

                if (f->fd >= 0 &&
                    (f->operation & ~LOCK_NB) == LOCK_SH) {
                        static const struct flock fl = {
                                .l_type = F_WRLCK,
                                .l_whence = SEEK_SET,
                        };

                        if (fcntl(f->fd, F_OFD_SETLK, &fl) >= 0)
                                f->operation = LOCK_EX|LOCK_NB;
                }

                if ((f->operation & ~LOCK_NB) == LOCK_EX)
                        unlink_noerrno(f->path);

                f->path = mfree(f->path);
        }

        f->fd = safe_close(f->fd);
        f->operation = 0;
}

int lockf_sane(int fd, int cmd, off_t len) {
        struct flock fl = { .l_whence = SEEK_CUR, .l_len = len };

        /* A version of lockf() that uses open file description locks instead of regular POSIX locks. OFD
         * locks are per file descriptor instead of process wide. This function doesn't support F_TEST for
         * now until we have a use case for it somewhere. */

        assert(fd >= 0);

        switch (cmd) {
        case F_ULOCK:
                fl.l_type = F_UNLCK;
                cmd = F_OFD_SETLK;
                break;
        case F_LOCK:
                fl.l_type = F_WRLCK;
                cmd = F_OFD_SETLKW;
                break;
        case F_TLOCK:
                fl.l_type = F_WRLCK;
                cmd = F_OFD_SETLK;
                break;
        default:
                return -EINVAL;
        }

        return RET_NERRNO(fcntl(fd, cmd, &fl));
}
