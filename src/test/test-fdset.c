/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <unistd.h>

#include "fd-util.h"
#include "fdset.h"
#include "fs-util.h"
#include "macro.h"
#include "tests.h"
#include "tmpfile-util.h"

TEST(fdset_new_fill) {
        int fd = -EBADF;
        _cleanup_fdset_free_ FDSet *fdset = NULL;

        log_close();
        log_set_open_when_needed(true);

        fd = open("/dev/null", O_CLOEXEC|O_RDONLY);
        assert_se(fd >= 0);

        assert_se(fdset_new_fill(/* filter_cloexec= */ -1, &fdset) >= 0);
        assert_se(fdset_contains(fdset, fd));
        fdset = fdset_free(fdset);
        assert_se(fcntl(fd, F_GETFD) < 0);
        assert_se(errno == EBADF);

        fd = open("/dev/null", O_CLOEXEC|O_RDONLY);
        assert_se(fd >= 0);

        assert_se(fdset_new_fill(/* filter_cloexec= */ 0, &fdset) >= 0);
        assert_se(!fdset_contains(fdset, fd));
        fdset = fdset_free(fdset);
        assert_se(fcntl(fd, F_GETFD) >= 0);

        assert_se(fdset_new_fill(/* filter_cloexec= */ 1, &fdset) >= 0);
        assert_se(fdset_contains(fdset, fd));
        fdset = fdset_free(fdset);
        assert_se(fcntl(fd, F_GETFD) < 0);
        assert_se(errno == EBADF);

        fd = open("/dev/null", O_RDONLY);
        assert_se(fd >= 0);

        assert_se(fdset_new_fill(/* filter_cloexec= */ 1, &fdset) >= 0);
        assert_se(!fdset_contains(fdset, fd));
        fdset = fdset_free(fdset);
        assert_se(fcntl(fd, F_GETFD) >= 0);

        assert_se(fdset_new_fill(/* filter_cloexec= */ 0, &fdset) >= 0);
        assert_se(fdset_contains(fdset, fd));
        fdset = fdset_free(fdset);
        assert_se(fcntl(fd, F_GETFD) < 0);
        assert_se(errno == EBADF);

        log_open();
}

TEST(fdset_put_dup) {
        _cleanup_close_ int fd = -EBADF;
        int copyfd = -EBADF;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_put_dup.XXXXXX";

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);
        copyfd = fdset_put_dup(fdset, fd);
        assert_se(copyfd >= 0 && copyfd != fd);
        assert_se(fdset_contains(fdset, copyfd));
        assert_se(!fdset_contains(fdset, fd));
}

TEST(fdset_cloexec) {
        int fd = -EBADF;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        int flags = -1;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_cloexec.XXXXXX";

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);
        assert_se(fdset_put(fdset, fd));

        assert_se(fdset_cloexec(fdset, false) >= 0);
        flags = fcntl(fd, F_GETFD);
        assert_se(flags >= 0);
        assert_se(!(flags & FD_CLOEXEC));

        assert_se(fdset_cloexec(fdset, true) >= 0);
        flags = fcntl(fd, F_GETFD);
        assert_se(flags >= 0);
        assert_se(flags & FD_CLOEXEC);
}

TEST(fdset_close_others) {
        int fd = -EBADF;
        int copyfd = -EBADF;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        int flags = -1;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_close_others.XXXXXX";

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);
        copyfd = fdset_put_dup(fdset, fd);
        assert_se(copyfd >= 0);

        assert_se(fdset_close_others(fdset) >= 0);
        flags = fcntl(fd, F_GETFD);
        assert_se(flags < 0);
        flags = fcntl(copyfd, F_GETFD);
        assert_se(flags >= 0);
}

TEST(fdset_remove) {
        _cleanup_close_ int fd = -EBADF;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_remove.XXXXXX";

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);
        assert_se(fdset_put(fdset, fd) >= 0);
        assert_se(fdset_remove(fdset, fd) >= 0);
        assert_se(!fdset_contains(fdset, fd));

        assert_se(fcntl(fd, F_GETFD) >= 0);
}

TEST(fdset_iterate) {
        int fd = -EBADF;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_iterate.XXXXXX";
        int c = 0;
        int a;

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);
        assert_se(fdset_put(fdset, fd) >= 0);
        assert_se(fdset_put(fdset, fd) >= 0);
        assert_se(fdset_put(fdset, fd) >= 0);

        FDSET_FOREACH(a, fdset) {
                c++;
                assert_se(a == fd);
        }
        assert_se(c == 1);
}

TEST(fdset_isempty) {
        int fd;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_isempty.XXXXXX";

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);

        assert_se(fdset_isempty(fdset));
        assert_se(fdset_put(fdset, fd) >= 0);
        assert_se(!fdset_isempty(fdset));
}

TEST(fdset_steal_first) {
        int fd;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_steal_first.XXXXXX";

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);

        assert_se(fdset_steal_first(fdset) < 0);
        assert_se(fdset_put(fdset, fd) >= 0);
        assert_se(fdset_steal_first(fdset) == fd);
        assert_se(fdset_steal_first(fdset) < 0);
        assert_se(fdset_put(fdset, fd) >= 0);
}

TEST(fdset_new_array) {
        int fds[] = {10, 11, 12, 13};
        _cleanup_fdset_free_ FDSet *fdset = NULL;

        assert_se(fdset_new_array(&fdset, fds, 4) >= 0);
        assert_se(fdset_size(fdset) == 4);
        assert_se(fdset_contains(fdset, 10));
        assert_se(fdset_contains(fdset, 11));
        assert_se(fdset_contains(fdset, 12));
        assert_se(fdset_contains(fdset, 13));
}

TEST(fdset_indexed) {
        _cleanup_close_ int fd = -EBADF;
        int copyindex = -1, index = 0, ret_fd;
        _cleanup_fdset_free_ FDSet *fdset = NULL;
        _cleanup_(unlink_tempfilep) char name[] = "/tmp/test-fdset_indexed.XXXXXX";

        fd = mkostemp_safe(name);
        assert_se(fd >= 0);

        fdset = fdset_new();
        assert_se(fdset);

        copyindex = fdset_put_dup_indexed(fdset, fd, index++);
        assert_se(copyindex >= 0 && copyindex != index);
        assert_se(fdset_contains_index(fdset, copyindex));
        assert_se(!fdset_contains(fdset, fd));

        copyindex = fdset_put_dup_indexed(fdset, fd, index++);
        assert_se(copyindex >= 0 && copyindex != index);
        assert_se(fdset_contains_index(fdset, copyindex));
        assert_se(!fdset_contains(fdset, fd));

        copyindex = fdset_put_dup_indexed(fdset, fd, index++);
        assert_se(copyindex >= 0 && copyindex != index);
        assert_se(fdset_contains_index(fdset, copyindex));
        assert_se(!fdset_contains(fdset, fd));

        _cleanup_free_ int *fds_array = NULL;
        size_t n_fds_array;

        n_fds_array = fdset_to_array_indexed(fdset, &fds_array);
        assert_se(n_fds_array == 3);

        ret_fd = fdset_remove_indexed(fdset, --index);
        assert_se(ret_fd >= 0);
        assert(fds_array[index] == ret_fd);
        ret_fd = safe_close(ret_fd);
        ret_fd = fdset_remove_indexed(fdset, --index);
        assert_se(ret_fd >= 0);
        ret_fd = safe_close(ret_fd);
        ret_fd = fdset_remove_indexed(fdset, --index);
        assert_se(ret_fd >= 0);
        ret_fd = safe_close(ret_fd);

        assert_se(fdset_isempty(fdset));
}

DEFINE_TEST_MAIN(LOG_INFO);
