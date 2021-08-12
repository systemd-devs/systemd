/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/xattr.h>

#include "alloc-util.h"
#include "dirent-util.h"
#include "env-file.h"
#include "env-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "macro.h"
#include "os-util.h"
#include "path-util.h"
#include "string-util.h"
#include "strv.h"
#include "utf8.h"

bool image_name_is_valid(const char *s) {
        if (!filename_is_valid(s))
                return false;

        if (string_has_cc(s, NULL))
                return false;

        if (!utf8_is_valid(s))
                return false;

        /* Temporary files for atomically creating new files */
        if (startswith(s, ".#"))
                return false;

        return true;
}

int path_is_extension_tree(const char *path, const char *extension) {
        int r;

        assert(path);

        /* Does the path exist at all? If not, generate an error immediately. This is useful so that a missing root dir
         * always results in -ENOENT, and we can properly distinguish the case where the whole root doesn't exist from
         * the case where just the os-release file is missing. */
        if (laccess(path, F_OK) < 0)
                return -errno;

        /* We use /usr/lib/extension-release.d/extension-release[.NAME] as flag for something being a system extension,
         * and {/etc|/usr/lib}/os-release as a flag for something being an OS (when not an extension). */
        r = open_extension_release(path, extension, NULL, NULL);
        if (r == -ENOENT) /* We got nothing */
                return 0;
        if (r < 0)
                return r;

        return 1;
}

int open_extension_release(const char *root, const char *extension, char **ret_path, int *ret_fd) {
        _cleanup_free_ char *q = NULL;
        int r, fd;

        if (extension) {
                const char *extension_full_path;

                if (!image_name_is_valid(extension))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "The extension name %s is invalid.", extension);

                extension_full_path = strjoina("/usr/lib/extension-release.d/extension-release.", extension);
                r = chase_symlinks(extension_full_path, root, CHASE_PREFIX_ROOT,
                                   ret_path ? &q : NULL,
                                   ret_fd ? &fd : NULL);
                /* Cannot find the expected extension-release file? The image filename might have been
                 * mangled on deployment, so fallback to checking for any file in the extension-release.d
                 * directory, and return the first one with a user.extension-release xattr instead.
                 * The xattr is checked to ensure the author of the image considers it OK if names do
                 * not match. */
                if (r == -ENOENT) {
                        _cleanup_free_ char *extension_release_dir_path = NULL;
                        _cleanup_closedir_ DIR *extension_release_dir = NULL;

                        r = chase_symlinks_and_opendir("/usr/lib/extension-release.d/", root, CHASE_PREFIX_ROOT,
                                                       &extension_release_dir_path, &extension_release_dir);
                        if (r < 0)
                                return r;

                        r = -ENOENT;
                        struct dirent *de;
                        FOREACH_DIRENT(de, extension_release_dir, return -errno) {
                                if (de->d_type != DT_REG)
                                        continue;

                                if (!startswith(de->d_name, "extension-release"))
                                        continue;

                                if (!image_name_is_valid(de->d_name))
                                        continue;

                                _cleanup_free_ char *p = NULL;
                                p = path_join(extension_release_dir_path, de->d_name);
                                if (!p)
                                        return -ENOMEM;

                                if (getxattr(p, "user.extension-release", NULL, 0) < 0)
                                        continue;

                                r = 0; /* Found it! */

                                if (ret_fd) {
                                        /* We already chased the directory, and checked that
                                         * this is a real file and not a symlink, so just open it. */
                                        fd = open(p, O_RDONLY|O_CLOEXEC|O_PATH);
                                        if (fd < 0)
                                                r = -errno;
                                }

                                if (ret_path)
                                        q = TAKE_PTR(p);

                                break;
                        }
                }
        } else {
                const char *p;

                FOREACH_STRING(p, "/etc/os-release", "/usr/lib/os-release") {
                        r = chase_symlinks(p, root, CHASE_PREFIX_ROOT,
                                           ret_path ? &q : NULL,
                                           ret_fd ? &fd : NULL);
                        if (r != -ENOENT)
                                break;
                }
        }
        if (r < 0)
                return r;

        if (ret_fd) {
                int real_fd;

                /* Convert the O_PATH fd into a proper, readable one */
                real_fd = fd_reopen(fd, O_RDONLY|O_CLOEXEC|O_NOCTTY);
                safe_close(fd);
                if (real_fd < 0)
                        return real_fd;

                *ret_fd = real_fd;
        }

        if (ret_path)
                *ret_path = TAKE_PTR(q);

        return 0;
}

int fopen_extension_release(const char *root, const char *extension, char **ret_path, FILE **ret_file) {
        _cleanup_free_ char *p = NULL;
        _cleanup_close_ int fd = -1;
        FILE *f;
        int r;

        if (!ret_file)
                return open_extension_release(root, extension, ret_path, NULL);

        r = open_extension_release(root, extension, ret_path ? &p : NULL, &fd);
        if (r < 0)
                return r;

        f = take_fdopen(&fd, "r");
        if (!f)
                return -errno;

        if (ret_path)
                *ret_path = TAKE_PTR(p);
        *ret_file = f;

        return 0;
}

static int parse_release_internal(const char *root, const char *extension, va_list ap) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *p = NULL;
        int r;

        r = fopen_extension_release(root, extension, &p, &f);
        if (r < 0)
                return r;

        return parse_env_filev(f, p, ap);
}

int _parse_extension_release(const char *root, const char *extension, ...) {
        va_list ap;
        int r;

        va_start(ap, extension);
        r = parse_release_internal(root, extension, ap);
        va_end(ap);

        return r;
}

int _parse_os_release(const char *root, ...) {
        va_list ap;
        int r;

        va_start(ap, root);
        r = parse_release_internal(root, NULL, ap);
        va_end(ap);

        return r;
}

int load_os_release_pairs(const char *root, char ***ret) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *p = NULL;
        int r;

        r = fopen_os_release(root, &p, &f);
        if (r < 0)
                return r;

        return load_env_file_pairs(f, p, ret);
}

int load_os_release_pairs_with_prefix(const char *root, const char *prefix, char ***ret) {
        _cleanup_strv_free_ char **os_release_pairs = NULL, **os_release_pairs_prefixed = NULL;
        char **p, **q;
        int r;

        r = load_os_release_pairs(root, &os_release_pairs);
        if (r < 0)
                return r;

        STRV_FOREACH_PAIR(p, q, os_release_pairs) {
                char *line;

                /* We strictly return only the four main ID fields and ignore the rest */
                if (!STR_IN_SET(*p, "ID", "VERSION_ID", "BUILD_ID", "VARIANT_ID"))
                        continue;

                ascii_strlower(*p);
                line = strjoin(prefix, *p, "=", *q);
                if (!line)
                        return -ENOMEM;
                r = strv_consume(&os_release_pairs_prefixed, line);
                if (r < 0)
                        return r;
        }

        *ret = TAKE_PTR(os_release_pairs_prefixed);

        return 0;
}

int load_extension_release_pairs(const char *root, const char *extension, char ***ret) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *p = NULL;
        int r;

        r = fopen_extension_release(root, extension, &p, &f);
        if (r < 0)
                return r;

        return load_env_file_pairs(f, p, ret);
}
