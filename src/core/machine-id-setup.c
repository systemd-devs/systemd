/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

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
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include "sd-id128.h"

#include "fileio.h"
#include "log.h"
#include "macro.h"
#include "mkdir.h"
#include "path-util.h"
#include "process-util.h"
#include "util.h"
#include "virt.h"
#include "machine-id-setup.h"

static int shorten_uuid(char destination[34], const char source[36]) {
        unsigned i, j;

        assert(destination);
        assert(source);

        /* Converts a UUID into a machine ID, by lowercasing it and
         * removing dashes. Validates everything. */

        for (i = 0, j = 0; i < 36 && j < 32; i++) {
                int t;

                t = unhexchar(source[i]);
                if (t < 0)
                        continue;

                destination[j++] = hexchar(t);
        }

        if (i != 36 || j != 32)
                return -EINVAL;

        destination[32] = '\n';
        destination[33] = 0;
        return 0;
}

static int read_id128(int fd, char id[34]) {
        char id_to_validate[34];
        int r;

        assert(fd >= 0);
        assert(id);

        /* Reads a machine ID from a file, validates it, and returns
         * it. The returned ID ends in a newline. */

        r = loop_read_exact(fd, id_to_validate, 33, false);
        if (r < 0)
                return r;

        if (id_to_validate[32] != '\n')
                return -EINVAL;

        id_to_validate[32] = 0;

        if (!id128_is_valid(id_to_validate))
                return -EINVAL;

        memcpy(id, id_to_validate, 32);
        id[32] = '\n';
        id[33] = 0;
        return 0;
}

static int write_id128(int fd, const char id[34]) {
        assert(fd >= 0);
        assert(id);

        if (lseek(fd, 0, SEEK_SET) < 0)
                return -errno;

        return loop_write(fd, id, 33, false);
}

static int generate_id128(char id[34]) {
        sd_id128_t buf;
        unsigned char *p;
        char  *q;
        int r;

        /* If that didn't work, generate a random machine id */
        r = sd_id128_randomize(&buf);
        if (r < 0)
                return log_error_errno(r, "Failed to open /dev/urandom: %m");

        for (p = buf.bytes, q = id; p < buf.bytes + sizeof(buf); p++, q += 2) {
                q[0] = hexchar(*p >> 4);
                q[1] = hexchar(*p & 15);
        }

        id[32] = '\n';
        id[33] = 0;

        return 0;
}

static int generate_machine_id(char id[34], const char *root) {
        int fd, r;
        const char *dbus_machine_id;

        assert(id);

        if (isempty(root))
                dbus_machine_id = "/var/lib/dbus/machine-id";
        else
                dbus_machine_id = strjoina(root, "/var/lib/dbus/machine-id");

        /* First, try reading the D-Bus machine id, unless it is a symlink */
        fd = open(dbus_machine_id, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
        if (fd >= 0) {
                r = read_id128(fd, id);
                safe_close(fd);

                if (r >= 0) {
                        log_info("Initializing machine ID from D-Bus machine ID.");
                        return 0;
                }
        }

        if (isempty(root)) {
                /* If that didn't work, see if we are running in a container,
                 * and a machine ID was passed in via $container_uuid the way
                 * libvirt/LXC does it */

                if (detect_container() > 0) {
                        _cleanup_free_ char *e = NULL;

                        r = getenv_for_pid(1, "container_uuid", &e);
                        if (r > 0) {
                                r = shorten_uuid(id, e);
                                if (r >= 0) {
                                        log_info("Initializing machine ID from container UUID.");
                                        return 0;
                                }
                        }

                } else if (detect_vm() == VIRTUALIZATION_KVM) {

                        /* If we are not running in a container, see if we are
                         * running in qemu/kvm and a machine ID was passed in
                         * via -uuid on the qemu/kvm command line */

                        char uuid[36];

                        fd = open("/sys/class/dmi/id/product_uuid", O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
                        if (fd >= 0) {
                                r = loop_read_exact(fd, uuid, 36, false);
                                safe_close(fd);

                                if (r >= 0) {
                                        r = shorten_uuid(id, uuid);
                                        if (r >= 0) {
                                                log_info("Initializing machine ID from KVM UUID.");
                                                return 0;
                                        }
                                }
                        }
                }
        }

        /* If that didn't work, generate a random machine id */
        r = generate_id128(id);
        if (r < 0)
                return r;

        log_info("Initializing machine ID from random generator.");

        return 0;
}

static int id128_file_valid(const char *root, const char *file)
{
        _cleanup_close_ int fd = -1;
        const char *etc_file;
        char id[34];

        if (isempty(root))  {
                etc_file = strjoina("/etc/", file);
        } else {
                char *x;

                x = strjoina(root, "/etc/", file);
                etc_file = path_kill_slashes(x);
        }

        fd = open(etc_file, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return -EINVAL;

        return read_id128(fd, id);
}

static int id128_file_setup(const char *root, const char *file, const char id[34], mode_t file_umask) {
        const char *etc_file, *run_file;
        _cleanup_close_ int fd = -1;
        bool writable = true;
        int r;

        if (isempty(root))  {
                etc_file = strjoina("/etc/", file);
                run_file = strjoina("/run/", file);
        } else {
                char *x;

                x = strjoina(root, "/etc/", file);
                etc_file = path_kill_slashes(x);

                x = strjoina(root, "/run/", file);
                run_file = path_kill_slashes(x);
        }

        RUN_WITH_UMASK(file_umask) {
                mkdir_parents(etc_file, 0755);
                fd = open(etc_file, O_RDWR|O_CREAT|O_CLOEXEC|O_NOCTTY, 0444);
                if (fd < 0) {
                        int old_errno = errno;

                        fd = open(etc_file, O_RDONLY|O_CLOEXEC|O_NOCTTY);
                        if (fd < 0) {
                                if (old_errno == EROFS && errno == ENOENT)
                                        log_error_errno(errno,
                                                  "System cannot boot: Missing %s and /etc is mounted read-only.\n"
                                                  "Booting up is supported only when:\n"
                                                  "1) the file exists and is populated.\n"
                                                  "2) the file exists and is empty.\n"
                                                  "3) the file is missing and /etc is writable.\n", etc_file);
                                else
                                        log_error_errno(errno, "Cannot open %s: %m", etc_file);

                                return -errno;
                        }

                        writable = false;
                }
        }

        if (writable)
                if (write_id128(fd, id) >= 0)
                        return 0;

        fd = safe_close(fd);

        /* Hmm, we couldn't write it? So let's write it to /run/ as a replacement */

        RUN_WITH_UMASK(file_umask) {
                r = write_string_file(run_file, id, WRITE_STRING_FILE_CREATE);
        }
        if (r < 0) {
                (void) unlink(run_file);
                return log_error_errno(r, "Cannot write %s: %m", run_file);
        }

        /* And now, let's mount it over */
        if (mount(run_file, etc_file, NULL, MS_BIND, NULL) < 0) {
                (void) unlink_noerrno(run_file);
                return log_error_errno(errno, "Failed to mount %s: %m", etc_file);
        }

        log_info("Installed transient %s file.", etc_file);

        /* Mark the mount read-only */
        if (mount(NULL, etc_file, NULL, MS_BIND|MS_RDONLY|MS_REMOUNT, NULL) < 0)
                log_warning_errno(errno, "Failed to make transient %s read-only: %m", etc_file);

        return 0;
}

int machine_id_setup(const char *root) {
        char id128[34];
       int r;

        if (id128_file_valid(root, "machine-id") == 0)
                return 0;

        r = generate_machine_id(id128, root);
        if (r < 0)
                return r;

        return id128_file_setup(root, "machine-id", id128, 0222);
}

int machine_secret_setup(const char *root) {
        char id128[34];
       int r;

        if (id128_file_valid(root, "machine-secret") == 0)
                return 0;

        r = generate_id128(id128);
        if (r < 0)
                return r;

        log_info("Initializing machine secret from random generator.");

        return id128_file_setup(root, "machine-secret", id128, 0244);
}

int machine_id_commit(const char *root) {
        _cleanup_close_ int fd = -1, initial_mntns_fd = -1;
        const char *etc_machine_id;
        char id[34]; /* 32 + \n + \0 */
        int r;

        if (isempty(root))
                etc_machine_id = "/etc/machine-id";
        else {
                char *x;

                x = strjoina(root, "/etc/machine-id");
                etc_machine_id = path_kill_slashes(x);
        }

        r = path_is_mount_point(etc_machine_id, 0);
        if (r < 0)
                return log_error_errno(r, "Failed to determine whether %s is a mount point: %m", etc_machine_id);
        if (r == 0) {
                log_debug("%s is is not a mount point. Nothing to do.", etc_machine_id);
                return 0;
        }

        /* Read existing machine-id */
        fd = open(etc_machine_id, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return log_error_errno(errno, "Cannot open %s: %m", etc_machine_id);

        r = read_id128(fd, id);
        if (r < 0)
                return log_error_errno(r, "We didn't find a valid machine ID in %s.", etc_machine_id);

        r = fd_is_temporary_fs(fd);
        if (r < 0)
                return log_error_errno(r, "Failed to determine whether %s is on a temporary file system: %m", etc_machine_id);
        if (r == 0) {
                log_error("%s is not on a temporary file system.", etc_machine_id);
                return -EROFS;
        }

        fd = safe_close(fd);

        /* Store current mount namespace */
        r = namespace_open(0, NULL, &initial_mntns_fd, NULL, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Can't fetch current mount namespace: %m");

        /* Switch to a new mount namespace, isolate ourself and unmount etc_machine_id in our new namespace */
        if (unshare(CLONE_NEWNS) < 0)
                return log_error_errno(errno, "Failed to enter new namespace: %m");

        if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) < 0)
                return log_error_errno(errno, "Couldn't make-rslave / mountpoint in our private namespace: %m");

        if (umount(etc_machine_id) < 0)
                return log_error_errno(errno, "Failed to unmount transient %s file in our private namespace: %m", etc_machine_id);

        /* Update a persistent version of etc_machine_id */
        fd = open(etc_machine_id, O_RDWR|O_CREAT|O_CLOEXEC|O_NOCTTY, 0444);
        if (fd < 0)
                return log_error_errno(errno, "Cannot open for writing %s. This is mandatory to get a persistent machine-id: %m", etc_machine_id);

        r = write_id128(fd, id);
        if (r < 0)
                return log_error_errno(r, "Cannot write %s: %m", etc_machine_id);

        fd = safe_close(fd);

        /* Return to initial namespace and proceed a lazy tmpfs unmount */
        r = namespace_enter(-1, initial_mntns_fd, -1, -1, -1);
        if (r < 0)
                return log_warning_errno(r, "Failed to switch back to initial mount namespace: %m.\nWe'll keep transient %s file until next reboot.", etc_machine_id);

        if (umount2(etc_machine_id, MNT_DETACH) < 0)
                return log_warning_errno(errno, "Failed to unmount transient %s file: %m.\nWe keep that mount until next reboot.", etc_machine_id);

        return 0;
}
