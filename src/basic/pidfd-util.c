/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/ioctl.h>
#include <unistd.h>

#include "errno-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "macro.h"
#include "memory-util.h"
#include "missing_magic.h"
#include "missing_threads.h"
#include "parse-util.h"
#include "path-util.h"
#include "pidfd-util.h"
#include "process-util.h"
#include "stat-util.h"
#include "string-util.h"

static int have_pidfs = -1;

static int pidfd_check_pidfs(int pid_fd) {

        /* NB: the passed fd *must* be acquired via pidfd_open(), i.e. must be a true pidfd! */

        if (have_pidfs >= 0)
                return have_pidfs;

        return (have_pidfs = fd_is_fs_type(pid_fd, PID_FS_MAGIC));
}

int pidfd_get_namespace(int fd, unsigned long ns_type_cmd) {
        static bool cached_supported = true;

        /* Obtain the namespace fd from pidfd directly through ioctl(PIDFD_GET_*_NAMESPACE).
         *
         * Returns -EOPNOTSUPP if ioctl on pidfds are not supported, -ENOPKG if the requested namespace
         * is disabled in kernel. (The errno used are different from what kernel returns via ioctl(),
         * see below) */

        assert(fd >= 0);

        /* If we know ahead of time that pidfs is unavailable, shortcut things. But otherwise we don't
         * call pidfd_check_pidfs() here, which is kinda extraneous and our own cache is required
         * anyways (pidfs is introduced in kernel 6.9 while ioctl support there is added in 6.11). */
        if (have_pidfs == 0 || !cached_supported)
                return -EOPNOTSUPP;

        int nsfd = ioctl(fd, ns_type_cmd);
        if (nsfd < 0) {
                /* Kernel returns EOPNOTSUPP if the ns type in question is disabled. Hence we need to look
                 * at precise errno instead of generic ERRNO_IS_(IOCTL_)NOT_SUPPORTED. */
                if (IN_SET(errno, ENOTTY, EINVAL)) {
                        cached_supported = false;
                        return -EOPNOTSUPP;
                }
                if (errno == EOPNOTSUPP) /* Translate to something more recognizable */
                        return -ENOPKG;

                return -errno;
        }

        return nsfd;
}

static int pidfd_get_info(int fd, struct pidfd_info *info) {
        static bool cached_supported = true;

        assert(fd >= 0);
        assert(info);

        if (have_pidfs == 0 || !cached_supported)
                return -EOPNOTSUPP;

        if (ioctl(fd, PIDFD_GET_INFO, info) < 0) {
                if (ERRNO_IS_IOCTL_NOT_SUPPORTED(errno)) {
                        cached_supported = false;
                        return -EOPNOTSUPP;
                }

                return -errno;
        }

        return 0;
}

static int pidfd_get_pid_fdinfo(int fd, pid_t *ret) {
        char path[STRLEN("/proc/self/fdinfo/") + DECIMAL_STR_MAX(int)];
        _cleanup_free_ char *fdinfo = NULL;
        int r;

        assert(fd >= 0);

        xsprintf(path, "/proc/self/fdinfo/%i", fd);

        r = read_full_virtual_file(path, &fdinfo, NULL);
        if (r == -ENOENT)
                return proc_fd_enoent_errno();
        if (r < 0)
                return r;

        char *p = find_line_startswith(fdinfo, "Pid:");
        if (!p)
                return -ENOTTY; /* not a pidfd? */

        p = skip_leading_chars(p, /* bad = */ NULL);
        p[strcspn(p, WHITESPACE)] = 0;

        if (streq(p, "0"))
                return -EREMOTE; /* PID is in foreign PID namespace? */
        if (streq(p, "-1"))
                return -ESRCH;   /* refers to reaped process? */

        return parse_pid(p, ret);
}

static int pidfd_get_pid_ioctl(int fd, pid_t *ret) {
        struct pidfd_info info = { .mask = PIDFD_INFO_PID };
        int r;

        assert(fd >= 0);

        r = pidfd_get_info(fd, &info);
        if (r < 0)
                return r;

        assert(FLAGS_SET(info.mask, PIDFD_INFO_PID));

        if (ret)
                *ret = info.pid;
        return 0;
}

int pidfd_get_pid(int fd, pid_t *ret) {
        int r;

        /* Converts a pidfd into a pid. We try ioctl(PIDFD_GET_INFO) (kernel 6.13+) first,
         * /proc/self/fdinfo/ as fallback. Well known errors:
         *
         *    -EBADF   → fd invalid
         *    -ESRCH   → fd valid, but process is already reaped
         *
         * pidfd_get_pid_fdinfo() might additionally fail for other reasons:
         *
         *    -ENOSYS  → /proc/ not mounted
         *    -ENOTTY  → fd valid, but not a pidfd
         *    -EREMOTE → fd valid, but pid is in another namespace we cannot translate to the local one
         *               (when using PIDFD_GET_INFO this is indistinguishable from -ESRCH)
         */

        assert(fd >= 0);

        r = pidfd_get_pid_ioctl(fd, ret);
        if (r != -EOPNOTSUPP)
                return r;

        return pidfd_get_pid_fdinfo(fd, ret);
}

int pidfd_verify_pid(int pidfd, pid_t pid) {
        pid_t current_pid;
        int r;

        assert(pidfd >= 0);
        assert(pid > 0);

        r = pidfd_get_pid(pidfd, &current_pid);
        if (r < 0)
                return r;

        return current_pid != pid ? -ESRCH : 0;
}

int pidfd_get_ppid(int fd, pid_t *ret) {
        struct pidfd_info info = { .mask = PIDFD_INFO_PID };
        int r;

        assert(fd >= 0);

        r = pidfd_get_info(fd, &info);
        if (r < 0)
                return r;

        assert(FLAGS_SET(info.mask, PIDFD_INFO_PID));

        if (info.ppid == 0) /* See comments in pid_get_ppid() */
                return -EADDRNOTAVAIL;

        if (ret)
                *ret = info.ppid;
        return 0;
}

int pidfd_get_uid(int fd, uid_t *ret) {
        struct pidfd_info info = { .mask = PIDFD_INFO_CREDS };
        int r;

        assert(fd >= 0);

        r = pidfd_get_info(fd, &info);
        if (r < 0)
                return r;

        assert(FLAGS_SET(info.mask, PIDFD_INFO_CREDS));

        if (ret)
                *ret = info.ruid;
        return 0;
}

int pidfd_get_cgroupid(int fd, uint64_t *ret) {
        struct pidfd_info info = { .mask = PIDFD_INFO_CGROUPID };
        int r;

        assert(fd >= 0);

        r = pidfd_get_info(fd, &info);
        if (r < 0)
                return r;

        assert(FLAGS_SET(info.mask, PIDFD_INFO_CGROUPID));

        if (ret)
                *ret = info.cgroupid;
        return 0;
}

int pidfd_get_inode_id(int fd, uint64_t *ret) {
        int r;

        assert(fd >= 0);

        r = pidfd_check_pidfs(fd);
        if (r < 0)
                return r;
        if (r == 0)
                return -EOPNOTSUPP;

        struct stat st;
        if (fstat(fd, &st) < 0)
                return -errno;

        if (ret)
                *ret = (uint64_t) st.st_ino;
        return 0;
}

int pidfd_get_inode_id_self_cached(uint64_t *ret) {
        static thread_local uint64_t cached = 0;
        static thread_local pid_t initialized = 0; /* < 0: cached error; == 0: invalid; > 0: valid and pid that was current */
        int r;

        assert(ret);

        if (initialized == getpid_cached()) {
                *ret = cached;
                return 0;
        }
        if (initialized < 0)
                return initialized;

        _cleanup_close_ int fd = pidfd_open(getpid_cached(), 0);
        if (fd < 0)
                return -errno;

        r = pidfd_get_inode_id(fd, &cached);
        if (ERRNO_IS_NEG_NOT_SUPPORTED(r))
                return (initialized = -EOPNOTSUPP);
        if (r < 0)
                return r;

        *ret = cached;
        initialized = getpid_cached();
        return 0;
}
