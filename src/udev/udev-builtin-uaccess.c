/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * manage device node user ACL
 */

#include "sd-login.h"

#include "device-util.h"
#include "devnode-acl.h"
#include "errno-util.h"
#include "lock-util.h"
#include "login-util.h"
#include "path-util.h"
#include "udev-builtin.h"

static int builtin_uaccess(UdevEvent *event, int argc, char *argv[]) {
        sd_device *dev = ASSERT_PTR(ASSERT_PTR(event)->dev);
        int r;

        if (event->event_mode != EVENT_UDEV_WORKER) {
                log_device_debug(dev, "Running in test mode, skipping execution of 'uaccess' builtin command.");
                return 0;
        }

        umask(0022);

        /* don't muck around with ACLs when the system is not running systemd */
        if (!logind_running())
                return 0;

        const char *node;
        r = sd_device_get_devname(dev, &node);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to get device node: %m");

        const char *seat;
        if (sd_device_get_property_value(dev, "ID_SEAT", &seat) < 0)
                seat = "seat0";

        /* Take a lock for the seat before reading the seat state file and applying ACLs.
         * See comments in seat_set_active() of logind. */

        _cleanup_free_ char *path = path_join("/run/systemd/seat/", seat);
        if (!path)
                return log_oom();

        _cleanup_(release_lock_file) LockFile lockfile = LOCK_FILE_INIT;
        r = make_lock_file_for(path, LOCK_EX, &lockfile);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to create lock file for '%s': %m", path);

        uid_t uid;
        r = sd_seat_get_active(seat, /* ret_session = */ NULL, &uid);
        if (r < 0) {
                if (IN_SET(r, -ENXIO, -ENODATA))
                        /* No active session on this seat */
                        r = 0;
                else
                        log_device_error_errno(dev, r, "Failed to determine active user on seat %s: %m", seat);

                goto reset;
        }

        r = devnode_acl(node,
                        /* flush = */ true,
                        /* del = */ false, /* old_uid = */ 0,
                        /* add = */ true, /* new_uid = */ uid);
        if (r < 0) {
                log_device_full_errno(dev, r == -ENOENT ? LOG_DEBUG : LOG_ERR, r, "Failed to apply ACL: %m");
                goto reset;
        }

        return 0;

reset:
        /* Better be safe than sorry and reset ACL */
        int k = devnode_acl(
                        node,
                        /* flush = */ true,
                        /* del = */ false, /* old_uid = */ 0,
                        /* add = */ false, /* new_uid = */ 0);
        if (k < 0) {
                log_device_full_errno(dev, k == -ENOENT ? LOG_DEBUG : LOG_ERR, k, "Failed to flush ACLs: %m");
                RET_GATHER(r, k);
        }

        return r;
}

const UdevBuiltin udev_builtin_uaccess = {
        .name = "uaccess",
        .cmd = builtin_uaccess,
        .help = "Manage device node user ACL",
};
