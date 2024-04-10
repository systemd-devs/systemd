/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "af-list.h"
#include "fd-util.h"
#include "fileio.h"
#include "log.h"
#include "macro.h"
#include "path-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "sysctl-util.h"

char *sysctl_normalize(char *s) {
        char *n;

        n = strpbrk(s, "/.");

        /* If the first separator is a slash, the path is
         * assumed to be normalized and slashes remain slashes
         * and dots remains dots. */

        if (n && *n == '.')
                /* Dots become slashes and slashes become dots. Fun. */
                do {
                        if (*n == '.')
                                *n = '/';
                        else
                                *n = '.';

                        n = strpbrk(n + 1, "/.");
                } while (n);

        path_simplify(s);

        /* Kill the leading slash, but keep the first character of the string in the same place. */
        if (s[0] == '/' && s[1] != 0)
                memmove(s, s+1, strlen(s));

        return s;
}

static int sysctl_f_ofd_lk(const char *p, bool set) {
        struct flock flock = {
                .l_type = F_WRLCK,
                .l_whence = SEEK_SET,
        };
        _cleanup_close_ int fd = -EBADF;
        int ret;

        fd = open(p, O_RDWR);
        if (fd < 0)
                return -ENOENT;

        ret = fcntl(fd, set ? F_OFD_SETLK : F_OFD_GETLK, &flock);
        if (ret)
                return -EBUSY;

        return 0;
}

int sysctl_write(const char *property, const char *value, bool lock) {
        char *p;
        int ret;

        assert(property);
        assert(value);

        p = strjoina("/proc/sys/", property);

        path_simplify(p);
        if (!path_is_normalized(p))
                return -EINVAL;

        ret = sysctl_f_ofd_lk(p, lock);
        if (ret < 0) {
                log_warning("Skipping sysctl write to '%s', it's handled elsewhere.", p);
                return -EBUSY;
        }

        log_debug("Setting '%s' to '%s'", p, value);

        return write_string_file(p, value, WRITE_STRING_FILE_VERIFY_ON_FAILURE | WRITE_STRING_FILE_DISABLE_BUFFER | WRITE_STRING_FILE_SUPPRESS_REDUNDANT_VIRTUAL);
}

int sysctl_writef(const char *property, bool lock, const char *format, ...) {
        _cleanup_free_ char *v = NULL;
        va_list ap;
        int r;

        va_start(ap, format);
        r = vasprintf(&v, format, ap);
        va_end(ap);

        if (r < 0)
                return -ENOMEM;

        return sysctl_write(property, v, lock);
}

int sysctl_write_ip_property(int af, const char *ifname, const char *property, const char *value, bool lock) {
        const char *p;

        assert(property);
        assert(value);

        if (!IN_SET(af, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        if (ifname) {
                if (!ifname_valid_full(ifname, IFNAME_VALID_SPECIAL))
                        return -EINVAL;

                p = strjoina("net/", af_to_ipv4_ipv6(af), "/conf/", ifname, "/", property);
        } else
                p = strjoina("net/", af_to_ipv4_ipv6(af), "/", property);

        return sysctl_write(p, value, lock);
}

int sysctl_write_ip_neighbor_property(int af, const char *ifname, const char *property, const char *value, bool lock) {
        const char *p;

        assert(property);
        assert(value);
        assert(ifname);

        if (!IN_SET(af, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        if (ifname) {
                if (!ifname_valid_full(ifname, IFNAME_VALID_SPECIAL))
                        return -EINVAL;
                p = strjoina("net/", af_to_ipv4_ipv6(af), "/neigh/", ifname, "/", property);
        } else
                p = strjoina("net/", af_to_ipv4_ipv6(af), "/neigh/default/", property);

        return sysctl_write(p, value, lock);
}

int sysctl_read(const char *property, char **ret) {
        char *p;
        int r;

        assert(property);

        p = strjoina("/proc/sys/", property);

        path_simplify(p);
        if (!path_is_normalized(p)) /* Filter out attempts to write to /proc/sys/../../…, just in case */
                return -EINVAL;

        r = read_full_virtual_file(p, ret, NULL);
        if (r < 0)
                return r;
        if (ret)
                delete_trailing_chars(*ret, NEWLINE);

        return r;
}

int sysctl_read_ip_property(int af, const char *ifname, const char *property, char **ret) {
        const char *p;

        assert(property);

        if (!IN_SET(af, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        if (ifname) {
                if (!ifname_valid_full(ifname, IFNAME_VALID_SPECIAL))
                        return -EINVAL;

                p = strjoina("net/", af_to_ipv4_ipv6(af), "/conf/", ifname, "/", property);
        } else
                p = strjoina("net/", af_to_ipv4_ipv6(af), "/", property);

        return sysctl_read(p, ret);
}
