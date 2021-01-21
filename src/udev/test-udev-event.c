/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "path-util.h"
#include "signal-util.h"
#include "strv.h"
#include "tests.h"
#include "udev-event.h"
#include "util.h"

#define BUF_SIZE 1024

static void test_event_spawn_core(bool with_pidfd, const char *cmd, char result_buf[BUF_SIZE]) {
        _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
        _cleanup_(udev_event_freep) UdevEvent *event = NULL;

        assert_se(setenv("SYSTEMD_PIDFD", yes_no(with_pidfd), 1) >= 0);

        assert_se(sd_device_new_from_syspath(&dev, "/sys/class/net/lo") >= 0);
        assert_se(event = udev_event_new(dev, 0, NULL, LOG_DEBUG));
        assert_se(udev_event_spawn(event, 5 * USEC_PER_SEC, SIGKILL, false, cmd, result_buf, BUF_SIZE) >= 0);

        assert_se(unsetenv("SYSTEMD_PIDFD") >= 0);
}

static void test_event_spawn_cat(bool with_pidfd) {
        _cleanup_strv_free_ char **lines = NULL;
        _cleanup_free_ char *cmd = NULL;
        char result_buf[BUF_SIZE], **p;

        log_debug("/* %s(%s) */", __func__, yes_no(with_pidfd));

        assert_se(find_executable("cat", &cmd) >= 0);
        assert_se(strextend_with_separator(&cmd, " ", "/sys/class/net/lo/uevent"));

        test_event_spawn_core(with_pidfd, cmd, result_buf);

        assert_se(lines = strv_split_newlines(result_buf));
        STRV_FOREACH(p, lines)
                printf("%s\n", *p);

        assert_se(strv_contains(lines, "INTERFACE=lo"));
        assert_se(strv_contains(lines, "IFINDEX=1"));
}

int main(int argc, char *argv[]) {
        test_setup_logging(LOG_DEBUG);

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGCHLD, -1) >= 0);

        test_event_spawn_cat(true);
        test_event_spawn_cat(false);

        return 0;
}
