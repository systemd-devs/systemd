/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

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
#include <unistd.h>

#include "alloc-util.h"
#include "dropin.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "log.h"
#include "macro.h"
#include "mkdir.h"
#include "path-util.h"
#include "special.h"
#include "string-util.h"
#include "time-util.h"
#include "unit-name.h"
#include "util.h"

static int write_fsck_sysroot_service(const char *dir, const char *what) {
        _cleanup_free_ char *device = NULL, *escaped = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *unit;
        int r;

        escaped = cescape(what);
        if (!escaped)
                return log_oom();

        unit = strjoina(dir, "/systemd-fsck-root.service");
        log_debug("Creating %s", unit);

        r = unit_name_from_path(what, ".device", &device);
        if (r < 0)
                return log_error_errno(r, "Failed to convert device \"%s\" to unit name: %m", what);

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by %1$s\n\n"
                "[Unit]\n"
                "Documentation=man:systemd-fsck-root.service(8)\n"
                "Description=File System Check on %2$s\n"
                "DefaultDependencies=no\n"
                "BindsTo=%3$s\n"
                "After=initrd-root-device.target local-fs-pre.target %3$s\n"
                "Before=shutdown.target\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart=" SYSTEMD_FSCK_PATH " %4$s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                what,
                device,
                escaped);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        return 0;
}

int generator_write_fsck_deps(
                FILE *f,
                const char *dir,
                const char *what,
                const char *where,
                const char *fstype) {

        int r;

        assert(f);
        assert(dir);
        assert(what);
        assert(where);

        if (!is_device_path(what)) {
                log_warning("Checking was requested for \"%s\", but it is not a device.", what);
                return 0;
        }

        if (!isempty(fstype) && !streq(fstype, "auto")) {
                r = fsck_exists(fstype);
                if (r < 0)
                        log_warning_errno(r, "Checking was requested for %s, but couldn't detect if fsck.%s may be used, proceeding: %m", what, fstype);
                else if (r == 0) {
                        /* treat missing check as essentially OK */
                        log_debug("Checking was requested for %s, but fsck.%s does not exist.", what, fstype);
                        return 0;
                }
        }

        if (path_equal(where, "/")) {
                const char *lnk;

                lnk = strjoina(dir, "/" SPECIAL_LOCAL_FS_TARGET ".wants/systemd-fsck-root.service");

                mkdir_parents(lnk, 0755);
                if (symlink(SYSTEM_DATA_UNIT_PATH "/systemd-fsck-root.service", lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        } else {
                _cleanup_free_ char *_fsck = NULL;
                const char *fsck;

                if (in_initrd() && path_equal(where, "/sysroot")) {
                        r = write_fsck_sysroot_service(dir, what);
                        if (r < 0)
                                return r;

                        fsck = "systemd-fsck-root.service";
                } else {
                        r = unit_name_from_path_instance("systemd-fsck", what, ".service", &_fsck);
                        if (r < 0)
                                return log_error_errno(r, "Failed to create fsck service name: %m");

                        fsck = _fsck;
                }

                fprintf(f,
                        "Requires=%1$s\n"
                        "After=%1$s\n",
                        fsck);
        }

        return 0;
}

int generator_write_timeouts(
                const char *dir,
                const char *what,
                const char *where,
                const char *opts,
                char **filtered) {

        /* Allow configuration how long we wait for a device that
         * backs a mount point to show up. This is useful to support
         * endless device timeouts for devices that show up only after
         * user input, like crypto devices. */

        _cleanup_free_ char *node = NULL, *unit = NULL, *timeout = NULL;
        usec_t u;
        int r;

        r = fstab_filter_options(opts, "comment=systemd.device-timeout\0"
                                       "x-systemd.device-timeout\0",
                                 NULL, &timeout, filtered);
        if (r <= 0)
                return r;

        r = parse_sec_fix_0(timeout, &u);
        if (r < 0) {
                log_warning("Failed to parse timeout for %s, ignoring: %s", where, timeout);
                return 0;
        }

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();
        if (!is_device_path(node)) {
                log_warning("x-systemd.device-timeout ignored for %s", what);
                return 0;
        }

        r = unit_name_from_path(node, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path: %m");

        return write_drop_in_format(dir, unit, 50, "device-timeout",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\nJobRunningTimeoutSec=%s",
                                    program_invocation_short_name, timeout);
}

int generator_write_device_deps(
                const char *dir,
                const char *what,
                const char *where,
                const char *opts) {

        /* fstab records that specify _netdev option should apply the network
         * ordering on the actual device depending on network connection. If we
         * are not mounting real device (NFS, CIFS), we rely on _netdev effect
         * on the mount unit itself. */

        _cleanup_free_ char *node = NULL, *unit = NULL;
        int r;

        if (!fstab_test_option(opts, "_netdev\0"))
                return 0;

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        /* Nothing to apply dependencies to. */
        if (!is_device_path(node))
                return 0;

        r = unit_name_from_path(node, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path: %m");

        /* See mount_add_default_dependencies for explanation why we create such
         * dependencies. */
        return write_drop_in_format(dir, unit, 50, "netdev-dependencies",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\n"
                                    "After=" SPECIAL_NETWORK_ONLINE_TARGET " " SPECIAL_NETWORK_TARGET "\n"
                                    "Wants=" SPECIAL_NETWORK_ONLINE_TARGET "\n",
                                    program_invocation_short_name);
}

int generator_write_initrd_root_device_deps(const char *dir, const char *what) {
        _cleanup_free_ char *unit = NULL;
        int r;

        r = unit_name_from_path(what, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path: %m");

        return write_drop_in_format(dir, SPECIAL_INITRD_ROOT_DEVICE_TARGET, 50, "root-device",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\nRequires=%s\nAfter=%s",
                                    program_invocation_short_name, unit, unit);
}
