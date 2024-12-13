/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if HAVE_LINUX_MEMFD_H
#include <linux/memfd.h>
#endif

#include "alloc-util.h"
#include "copy.h"
#include "data-fd-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "io-util.h"
#include "memfd-util.h"
#include "missing_mman.h"
#include "missing_syscall.h"
#include "tmpfile-util.h"

/* When the data is smaller or equal to 64K, try to place the copy in a memfd/pipe */
#define DATA_FD_MEMORY_LIMIT (64U * U64_KB)

/* If memfd/pipe didn't work out, then let's use a file in /tmp up to a size of 1M. If it's large than that use /var/tmp instead. */
#define DATA_FD_TMP_LIMIT (1U * U64_MB)

int copy_data_fd(int fd) {
        _cleanup_close_ int copy_fd = -EBADF, tmp_fd = -EBADF;
        _cleanup_free_ void *remains = NULL;
        size_t remains_size = 0;
        const char *td;
        struct stat st;
        int r;

        /* Creates a 'data' fd from the specified source fd, containing all the same data in a read-only fashion, but
         * independent of it (i.e. the source fd can be closed and unmounted after this call succeeded). Tries to be
         * somewhat smart about where to place the data. In the best case uses a memfd(). If memfd() are not supported
         * uses a pipe instead. For larger data will use an unlinked file in /tmp, and for even larger data one in
         * /var/tmp. */

        if (fstat(fd, &st) < 0)
                return -errno;

        /* For now, let's only accept regular files, sockets, pipes and char devices */
        if (S_ISDIR(st.st_mode))
                return -EISDIR;
        if (S_ISLNK(st.st_mode))
                return -ELOOP;
        if (!S_ISREG(st.st_mode) && !S_ISSOCK(st.st_mode) && !S_ISFIFO(st.st_mode) && !S_ISCHR(st.st_mode))
                return -EBADFD;

        /* If we have reason to believe the data is bounded in size, then let's use memfds or pipes as backing fd. Note
         * that we use the reported regular file size only as a hint, given that there are plenty special files in
         * /proc and /sys which report a zero file size but can be read from. */

        if (!S_ISREG(st.st_mode) || (uint64_t) st.st_size < DATA_FD_MEMORY_LIMIT) {

                /* Try a memfd first */
                copy_fd = memfd_new("data-fd");
                if (copy_fd < 0)
                        return copy_fd;

                r = copy_bytes(fd, copy_fd, DATA_FD_MEMORY_LIMIT, 0);
                if (r < 0)
                        return r;

                off_t f = lseek(copy_fd, 0, SEEK_SET);
                if (f != 0)
                        return -errno;

                if (r == 0) {
                        /* Did it fit into the limit? If so, we are done. */
                        r = memfd_set_sealed(copy_fd);
                        if (r < 0)
                                return r;

                        return TAKE_FD(copy_fd);
                }
        }

        /* If we have reason to believe this will fit fine in /tmp, then use that as first fallback. */
        if ((!S_ISREG(st.st_mode) || (uint64_t) st.st_size < DATA_FD_TMP_LIMIT) &&
            (DATA_FD_MEMORY_LIMIT + remains_size) < DATA_FD_TMP_LIMIT) {
                off_t f;

                tmp_fd = open_tmpfile_unlinkable(NULL /* NULL as directory means /tmp */, O_RDWR|O_CLOEXEC);
                if (tmp_fd < 0)
                        return tmp_fd;

                if (copy_fd >= 0) {
                        /* If we tried a memfd/pipe first and it ended up being too large, then copy this into the
                         * temporary file first. */

                        r = copy_bytes(copy_fd, tmp_fd, UINT64_MAX, 0);
                        if (r < 0)
                                return r;

                        assert(r == 0);
                }

                if (remains_size > 0) {
                        /* If there were remaining bytes (i.e. read into memory, but not written out yet) from the
                         * failed copy operation, let's flush them out next. */

                        r = loop_write(tmp_fd, remains, remains_size);
                        if (r < 0)
                                return r;
                }

                r = copy_bytes(fd, tmp_fd, DATA_FD_TMP_LIMIT - DATA_FD_MEMORY_LIMIT - remains_size, COPY_REFLINK);
                if (r < 0)
                        return r;
                if (r == 0)
                        goto finish;  /* Yay, it fit in */

                /* It didn't fit in. Let's not forget to use what we already used */
                f = lseek(tmp_fd, 0, SEEK_SET);
                if (f != 0)
                        return -errno;

                close_and_replace(copy_fd, tmp_fd);

                remains = mfree(remains);
                remains_size = 0;
        }

        /* As last fallback use /var/tmp */
        r = var_tmp_dir(&td);
        if (r < 0)
                return r;

        tmp_fd = open_tmpfile_unlinkable(td, O_RDWR|O_CLOEXEC);
        if (tmp_fd < 0)
                return tmp_fd;

        if (copy_fd >= 0) {
                /* If we tried a memfd/pipe first, or a file in /tmp, and it ended up being too large, than copy this
                 * into the temporary file first. */
                r = copy_bytes(copy_fd, tmp_fd, UINT64_MAX, COPY_REFLINK);
                if (r < 0)
                        return r;

                assert(r == 0);
        }

        if (remains_size > 0) {
                /* Then, copy in any read but not yet written bytes. */
                r = loop_write(tmp_fd, remains, remains_size);
                if (r < 0)
                        return r;
        }

        /* Copy in the rest */
        r = copy_bytes(fd, tmp_fd, UINT64_MAX, COPY_REFLINK);
        if (r < 0)
                return r;

        assert(r == 0);

finish:
        /* Now convert the O_RDWR file descriptor into an O_RDONLY one (and as side effect seek to the beginning of the
         * file again */

        return fd_reopen(tmp_fd, O_RDONLY|O_CLOEXEC);
}

int memfd_clone_fd(int fd, const char *name, int mode) {
        _cleanup_close_ int mfd = -EBADF;
        struct stat st;
        bool ro, exec;
        int r;

        /* Creates a clone of a regular file in a memfd. Unlike copy_data_fd() this returns strictly a memfd
         * (and if it can't it will fail). Thus the resulting fd is seekable, and definitely reports as
         * S_ISREG. */

        assert(fd >= 0);
        assert(name);
        assert(IN_SET(mode & O_ACCMODE, O_RDONLY, O_RDWR));
        assert((mode & ~(O_RDONLY|O_RDWR|O_CLOEXEC)) == 0);

        if (fstat(fd, &st) < 0)
                return -errno;

        ro = (mode & O_ACCMODE) == O_RDONLY;
        exec = st.st_mode & 0111;

        mfd = memfd_create_wrapper(name,
                                   ((FLAGS_SET(mode, O_CLOEXEC) || ro) ? MFD_CLOEXEC : 0) |
                                   (ro ? MFD_ALLOW_SEALING : 0) |
                                   (exec ? MFD_EXEC : MFD_NOEXEC_SEAL));
        if (mfd < 0)
                return mfd;

        r = copy_bytes(fd, mfd, UINT64_MAX, COPY_REFLINK);
        if (r < 0)
                return r;

        if (ro) {
                _cleanup_close_ int rfd = -EBADF;

                r = memfd_set_sealed(mfd);
                if (r < 0)
                        return r;

                rfd = fd_reopen(mfd, mode);
                if (rfd < 0)
                        return rfd;

                return TAKE_FD(rfd);
        }

        off_t f = lseek(mfd, 0, SEEK_SET);
        if (f < 0)
                return -errno;

        return TAKE_FD(mfd);
}
