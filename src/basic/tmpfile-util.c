/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/mman.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "macro.h"
#include "memfd-util.h"
#include "missing_fcntl.h"
#include "missing_syscall.h"
#include "path-util.h"
#include "process-util.h"
#include "random-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "tmpfile-util.h"
#include "umask-util.h"

int fopen_temporary(const char *path, FILE **ret_f, char **ret_temp_path) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *t = NULL;
        _cleanup_close_ int fd = -1;
        int r;

        if (path) {
                r = tempfn_xxxxxx(path, NULL, &t);
                if (r < 0)
                        return r;
        } else {
                const char *d;

                r = tmp_dir(&d);
                if (r < 0)
                        return r;

                t = path_join(d, "XXXXXX");
                if (!t)
                        return -ENOMEM;
        }

        fd = mkostemp_safe(t);
        if (fd < 0)
                return -errno;

        /* This assumes that returned FILE object is short-lived and used within the same single-threaded
         * context and never shared externally, hence locking is not necessary. */

        r = take_fdopen_unlocked(&fd, "w", &f);
        if (r < 0) {
                (void) unlink(t);
                return r;
        }

        if (ret_f)
                *ret_f = TAKE_PTR(f);

        if (ret_temp_path)
                *ret_temp_path = TAKE_PTR(t);

        return 0;
}

/* This is much like mkostemp() but is subject to umask(). */
int mkostemp_safe(char *pattern) {
        assert(pattern);
        BLOCK_WITH_UMASK(0077);
        return RET_NERRNO(mkostemp(pattern, O_CLOEXEC));
}

int fmkostemp_safe(char *pattern, const char *mode, FILE **ret_f) {
        _cleanup_close_ int fd = -1;
        FILE *f;

        fd = mkostemp_safe(pattern);
        if (fd < 0)
                return fd;

        f = take_fdopen(&fd, mode);
        if (!f)
                return -errno;

        *ret_f = f;
        return 0;
}

static int tempfn_build(const char *p, const char *pre, const char *post, char **ret) {
        _cleanup_free_ char *d = NULL, *fn = NULL, *nf = NULL, *result = NULL;
        size_t len_pre, len_post, len_add;
        int r;

        assert(p);
        assert(ret);

        /*
         * Turns this:
         *         /foo/bar/waldo
         *
         * Into this:
         *         /foo/bar/.#<pre>waldo<post>
         */

        if (pre && strchr(post, '/'))
                return -EINVAL;

        if (post && strchr(post, '/'))
                return -EINVAL;

        len_pre = strlen_ptr(pre);
        len_post = strlen_ptr(post);
        /* NAME_MAX is counted *without* the trailing NUL byte. */
        if (len_pre > NAME_MAX - STRLEN(".#") ||
            len_post > NAME_MAX - STRLEN(".#") - len_pre)
                return -EINVAL;

        len_add = len_pre + len_post + STRLEN(".#");

        r = path_extract_directory(p, &d);
        if (r < 0 && r != -EDESTADDRREQ) /* EDESTADDRREQ → No directory specified, just a filename */
                return r;

        r = path_extract_filename(p, &fn);
        if (r < 0)
                return r;

        if (strlen(fn) > NAME_MAX - len_add)
                /* We cannot simply prepend and append strings to the filename. Let's truncate the filename. */
                fn[NAME_MAX - len_add] = '\0';

        nf = strjoin(".#", strempty(pre), fn, strempty(post));
        if (!nf)
                return -ENOMEM;

        if (d) {
                if (!path_extend(&d, nf))
                        return -ENOMEM;

                result = path_simplify(TAKE_PTR(d));
        } else
                result = TAKE_PTR(nf);

        if (!path_is_valid(result)) /* New path is not valid? (Maybe because too long?) Refuse. */
                return -EINVAL;

        *ret = TAKE_PTR(result);
        return 0;
}

int tempfn_xxxxxx(const char *p, const char *extra, char **ret) {
        /*
         * Turns this:
         *         /foo/bar/waldo
         *
         * Into this:
         *         /foo/bar/.#<extra>waldoXXXXXX
         */

        return tempfn_build(p, extra, "XXXXXX", ret);
}

int tempfn_random(const char *p, const char *extra, char **ret) {
        _cleanup_free_ char *s = NULL;

        assert(p);
        assert(ret);

        /*
         * Turns this:
         *         /foo/bar/waldo
         *
         * Into this:
         *         /foo/bar/.#<extra>waldobaa2a261115984a9
         */

        if (asprintf(&s, "%016" PRIx64, random_u64()) < 0)
                return -ENOMEM;

        return tempfn_build(p, extra, s, ret);
}

int tempfn_random_child(const char *p, const char *extra, char **ret) {
        _cleanup_free_ char *t = NULL;
        uint64_t u;
        char *x;
        int r;

        assert(ret);

        /* Turns this:
         *         /foo/bar/waldo
         * Into this:
         *         /foo/bar/waldo/.#<extra>3c2b6219aa75d7d0
         */

        if (!isempty(extra)) {
                const char *e;

                e = strchrnul(extra, '/');
                if (*e != '\0')
                        return -EINVAL;

                if ((size_t) (e - extra) > (size_t) (NAME_MAX - STRLEN(".#") - 16))
                        return -EINVAL;
        }

        if (!p) {
                r = tmp_dir(&p);
                if (r < 0)
                        return r;
        }

        x = t = new(char, strlen(p) + 3 + strlen_ptr(extra) + 16 + 1);
        if (!t)
                return -ENOMEM;

        if (!isempty(p))
                x = stpcpy(stpcpy(x, p), "/");

        x = stpcpy(x, ".#");

        if (!isempty(extra))
                x = stpcpy(x, extra);

        u = random_u64();
        for (unsigned i = 0; i < 16; i++) {
                *(x++) = hexchar(u & 0xF);
                u >>= 4;
        }

        *x = 0;

        path_simplify(t);
        if (!path_is_valid(t))
                return -EINVAL; /* New path is not valid? (Maybe because too long?) Refuse. */

        *ret = TAKE_PTR(t);
        return 0;
}

int open_tmpfile_unlinkable(const char *directory, int flags) {
        char *p;
        int fd, r;

        if (!directory) {
                r = tmp_dir(&directory);
                if (r < 0)
                        return r;
        } else if (isempty(directory))
                return -EINVAL;

        /* Returns an unlinked temporary file that cannot be linked into the file system anymore */

        /* Try O_TMPFILE first, if it is supported */
        fd = open(directory, flags|O_TMPFILE|O_EXCL, S_IRUSR|S_IWUSR);
        if (fd >= 0)
                return fd;

        /* Fall back to unguessable name + unlinking */
        p = strjoina(directory, "/systemd-tmp-XXXXXX");

        fd = mkostemp_safe(p);
        if (fd < 0)
                return fd;

        (void) unlink(p);

        return fd;
}

int open_tmpfile_linkable(const char *target, int flags, char **ret_path) {
        _cleanup_free_ char *tmp = NULL;
        int r, fd;

        assert(target);
        assert(ret_path);

        /* Don't allow O_EXCL, as that has a special meaning for O_TMPFILE */
        assert((flags & O_EXCL) == 0);

        /* Creates a temporary file, that shall be renamed to "target" later. If possible, this uses O_TMPFILE – in
         * which case "ret_path" will be returned as NULL. If not possible the temporary path name used is returned in
         * "ret_path". Use link_tmpfile() below to rename the result after writing the file in full. */

        fd = open_parent(target, O_TMPFILE|flags, 0640);
        if (fd >= 0) {
                *ret_path = NULL;
                return fd;
        }

        log_debug_errno(fd, "Failed to use O_TMPFILE for %s: %m", target);

        r = tempfn_random(target, NULL, &tmp);
        if (r < 0)
                return r;

        fd = open(tmp, O_CREAT|O_EXCL|O_NOFOLLOW|O_NOCTTY|flags, 0640);
        if (fd < 0)
                return -errno;

        *ret_path = TAKE_PTR(tmp);

        return fd;
}

int fopen_tmpfile_linkable(const char *target, int flags, char **ret_path, FILE **ret_file) {
        _cleanup_free_ char *path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_close_ int fd = -1;

        assert(target);
        assert(ret_file);
        assert(ret_path);

        fd = open_tmpfile_linkable(target, flags, &path);
        if (fd < 0)
                return fd;

        f = take_fdopen(&fd, "w");
        if (!f)
                return -ENOMEM;

        *ret_path = TAKE_PTR(path);
        *ret_file = TAKE_PTR(f);
        return 0;
}

int link_tmpfile(int fd, const char *path, const char *target) {
        assert(fd >= 0);
        assert(target);

        /* Moves a temporary file created with open_tmpfile() above into its final place. if "path" is NULL an fd
         * created with O_TMPFILE is assumed, and linkat() is used. Otherwise it is assumed O_TMPFILE is not supported
         * on the directory, and renameat2() is used instead.
         *
         * Note that in both cases we will not replace existing files. This is because linkat() does not support this
         * operation currently (renameat2() does), and there is no nice way to emulate this. */

        if (path)
                return rename_noreplace(AT_FDCWD, path, AT_FDCWD, target);

        return RET_NERRNO(linkat(AT_FDCWD, FORMAT_PROC_FD_PATH(fd), AT_FDCWD, target, AT_SYMLINK_FOLLOW));
}

int flink_tmpfile(FILE *f, const char *path, const char *target) {
        int fd, r;

        assert(f);
        assert(target);

        fd = fileno(f);
        if (fd < 0) /* Not all FILE* objects encapsulate fds */
                return -EBADF;

        r = fflush_sync_and_check(f);
        if (r < 0)
                return r;

        return link_tmpfile(fd, path, target);
}

int mkdtemp_malloc(const char *template, char **ret) {
        _cleanup_free_ char *p = NULL;
        int r;

        assert(ret);

        if (template)
                p = strdup(template);
        else {
                const char *tmp;

                r = tmp_dir(&tmp);
                if (r < 0)
                        return r;

                p = path_join(tmp, "XXXXXX");
        }
        if (!p)
                return -ENOMEM;

        if (!mkdtemp(p))
                return -errno;

        *ret = TAKE_PTR(p);
        return 0;
}
