/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

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

#include <blkid/blkid.h>
#include <stdlib.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "libudev.h"
#include "sd-id128.h"

#include "alloc-util.h"
#include "blkid-util.h"
#include "btrfs-util.h"
#include "dirent-util.h"
#include "dissect-image.h"
#include "efivars.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "gpt.h"
#include "missing.h"
#include "mkdir.h"
#include "mount-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "stat-util.h"
#include "string-util.h"
#include "udev-util.h"
#include "unit-name.h"
#include "util.h"
#include "virt.h"

static const char *arg_dest = "/tmp";
static bool arg_enabled = true;
static bool arg_root_enabled = true;
static bool arg_root_rw = false;

static int add_cryptsetup(const char *id, const char *what, bool rw, bool require, char **device) {
        _cleanup_free_ char *e = NULL, *n = NULL, *p = NULL, *d = NULL, *to = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        char *from, *ret;
        int r;

        assert(id);
        assert(what);

        r = unit_name_from_path(what, ".device", &d);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        e = unit_name_escape(id);
        if (!e)
                return log_oom();

        r = unit_name_build("systemd-cryptsetup", e, ".service", &n);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoin(arg_dest, "/", n);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", p);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Cryptography Setup for %%I\n"
                "Documentation=man:systemd-gpt-auto-generator(8) man:systemd-cryptsetup@.service(8)\n"
                "DefaultDependencies=no\n"
                "Conflicts=umount.target\n"
                "BindsTo=dev-mapper-%%i.device %s\n"
                "Before=umount.target cryptsetup.target\n"
                "After=%s\n"
                "IgnoreOnIsolate=true\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "TimeoutSec=0\n" /* the binary handles timeouts anyway */
                "ExecStart=" SYSTEMD_CRYPTSETUP_PATH " attach '%s' '%s' '' '%s'\n"
                "ExecStop=" SYSTEMD_CRYPTSETUP_PATH " detach '%s'\n",
                d, d,
                id, what, rw ? "" : "read-only",
                id);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write file %s: %m", p);

        from = strjoina("../", n);

        to = strjoin(arg_dest, "/", d, ".wants/", n);
        if (!to)
                return log_oom();

        mkdir_parents_label(to, 0755);
        if (symlink(from, to) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", to);

        if (require) {
                free(to);

                to = strjoin(arg_dest, "/cryptsetup.target.requires/", n);
                if (!to)
                        return log_oom();

                mkdir_parents_label(to, 0755);
                if (symlink(from, to) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", to);

                free(to);
                to = strjoin(arg_dest, "/dev-mapper-", e, ".device.requires/", n);
                if (!to)
                        return log_oom();

                mkdir_parents_label(to, 0755);
                if (symlink(from, to) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", to);
        }

        free(p);
        p = strjoin(arg_dest, "/dev-mapper-", e, ".device.d/50-job-timeout-sec-0.conf");
        if (!p)
                return log_oom();

        mkdir_parents_label(p, 0755);
        r = write_string_file(p,
                        "# Automatically generated by systemd-gpt-auto-generator\n\n"
                        "[Unit]\n"
                        "JobTimeoutSec=0\n",
                        WRITE_STRING_FILE_CREATE); /* the binary handles timeouts anyway */
        if (r < 0)
                return log_error_errno(r, "Failed to write device drop-in: %m");

        ret = strappend("/dev/mapper/", id);
        if (!ret)
                return log_oom();

        if (device)
                *device = ret;
        return 0;
}

static int add_mount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                const char *options,
                const char *description,
                const char *post) {

        _cleanup_free_ char *unit = NULL, *lnk = NULL, *crypto_what = NULL, *p = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(id);
        assert(what);
        assert(where);
        assert(description);

        log_debug("Adding %s: %s %s", where, what, strna(fstype));

        if (streq_ptr(fstype, "crypto_LUKS")) {

                r = add_cryptsetup(id, what, rw, true, &crypto_what);
                if (r < 0)
                        return r;

                what = crypto_what;
                fstype = NULL;
        }

        r = unit_name_from_path(where, ".mount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoin(arg_dest, "/", unit);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n",
                description);

        if (post)
                fprintf(f, "Before=%s\n", post);

        r = generator_write_fsck_deps(f, arg_dest, what, where, fstype);
        if (r < 0)
                return r;

        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n",
                what, where);

        if (fstype)
                fprintf(f, "Type=%s\n", fstype);

        if (options)
                fprintf(f, "Options=%s,%s\n", options, rw ? "rw" : "ro");
        else
                fprintf(f, "Options=%s\n", rw ? "rw" : "ro");

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        if (post) {
                lnk = strjoin(arg_dest, "/", post, ".requires/", unit);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(p, lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);
        }

        return 0;
}

static bool path_is_busy(const char *where) {
        int r;

        /* already a mountpoint; generators run during reload */
        r = path_is_mount_point(where, NULL, AT_SYMLINK_FOLLOW);
        if (r > 0)
                return false;

        /* the directory might not exist on a stateless system */
        if (r == -ENOENT)
                return false;

        if (r < 0)
                return true;

        /* not a mountpoint but it contains files */
        if (dir_is_empty(where) <= 0)
                return true;

        return false;
}

static int add_partition_mount(
                DissectedPartition *p,
                const char *id,
                const char *where,
                const char *description) {

        assert(p);

        if (path_is_busy(where)) {
                log_debug("%s already populated, ignoring.", where);
                return 0;
        }

        return add_mount(
                        id,
                        p->node,
                        where,
                        p->fstype,
                        p->rw,
                        NULL,
                        description,
                        SPECIAL_LOCAL_FS_TARGET);
}

static int add_swap(const char *path) {
        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(path);

        log_debug("Adding swap: %s", path);

        r = unit_name_from_path(path, ".swap", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = strjoin(arg_dest, "/", name);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Swap Partition\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n\n"
                "[Swap]\n"
                "What=%s\n",
                path);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET ".wants/", name);
        if (!lnk)
                return log_oom();

        mkdir_parents_label(lnk, 0755);
        if (symlink(unit, lnk) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        return 0;
}

#ifdef ENABLE_EFI
static int add_automount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                const char *options,
                const char *description,
                usec_t timeout) {

        _cleanup_free_ char *unit = NULL, *lnk = NULL;
        _cleanup_free_ char *opt, *p = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(id);
        assert(where);
        assert(description);

        if (options)
                opt = strjoin(options, ",noauto");
        else
                opt = strdup("noauto");
        if (!opt)
                return log_oom();

        r = add_mount(id,
                      what,
                      where,
                      fstype,
                      rw,
                      opt,
                      description,
                      NULL);
        if (r < 0)
                return r;

        r = unit_name_from_path(where, ".automount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoin(arg_dest, "/", unit);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n"
                "[Automount]\n"
                "Where=%s\n"
                "TimeoutIdleSec="USEC_FMT"\n",
                description,
                where,
                timeout / USEC_PER_SEC);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        lnk = strjoin(arg_dest, "/" SPECIAL_LOCAL_FS_TARGET ".wants/", unit);
        if (!lnk)
                return log_oom();
        mkdir_parents_label(lnk, 0755);

        if (symlink(p, lnk) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        return 0;
}

static int add_esp(DissectedPartition *p) {
        const char *esp;
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, ignoring the ESP.");
                return 0;
        }

        /* If /efi exists we'll use that. Otherwise we'll use /boot, as that's usually the better choice */
        esp = access("/efi/", F_OK) >= 0 ? "/efi" : "/boot";

        /* We create an .automount which is not overridden by the .mount from the fstab generator. */
        if (fstab_is_mount_point(esp)) {
                log_debug("%s specified in fstab, ignoring.", esp);
                return 0;
        }

        if (path_is_busy(esp)) {
                log_debug("%s already populated, ignoring.", esp);
                return 0;
        }

        if (is_efi_boot()) {
                sd_id128_t loader_uuid;

                /* If this is an EFI boot, be extra careful, and only mount the ESP if it was the ESP used for booting. */

                r = efi_loader_get_device_part_uuid(&loader_uuid);
                if (r == -ENOENT) {
                        log_debug("EFI loader partition unknown.");
                        return 0;
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to read ESP partition UUID: %m");

                if (!sd_id128_equal(p->uuid, loader_uuid)) {
                        log_debug("Partition for %s does not appear to be the partition we are booted from.", esp);
                        return 0;
                }
        } else
                log_debug("Not an EFI boot, skipping ESP check.");

        return add_automount("boot",
                             p->node,
                             esp,
                             p->fstype,
                             true,
                             "umask=0077",
                             "EFI System Partition Automount",
                             120 * USEC_PER_SEC);
}
#else
static int add_esp(DissectedPartition *p) {
        return 0;
}
#endif

static int open_parent(dev_t devnum, int *ret) {
        _cleanup_udev_device_unref_ struct udev_device *d = NULL;
        _cleanup_udev_unref_ struct udev *udev = NULL;
        const char *name, *devtype, *node;
        struct udev_device *parent;
        dev_t pn;
        int fd;

        assert(ret);

        udev = udev_new();
        if (!udev)
                return log_oom();

        d = udev_device_new_from_devnum(udev, 'b', devnum);
        if (!d)
                return log_oom();

        name = udev_device_get_devnode(d);
        if (!name)
                name = udev_device_get_syspath(d);
        if (!name) {
                log_debug("Device %u:%u does not have a name, ignoring.", major(devnum), minor(devnum));
                goto not_found;
        }

        parent = udev_device_get_parent(d);
        if (!parent) {
                log_debug("%s: not a partitioned device, ignoring.", name);
                goto not_found;
        }

        /* Does it have a devtype? */
        devtype = udev_device_get_devtype(parent);
        if (!devtype) {
                log_debug("%s: parent doesn't have a device type, ignoring.", name);
                goto not_found;
        }

        /* Is this a disk or a partition? We only care for disks... */
        if (!streq(devtype, "disk")) {
                log_debug("%s: parent isn't a raw disk, ignoring.", name);
                goto not_found;
        }

        /* Does it have a device node? */
        node = udev_device_get_devnode(parent);
        if (!node) {
                log_debug("%s: parent device does not have device node, ignoring.", name);
                goto not_found;
        }

        log_debug("%s: root device %s.", name, node);

        pn = udev_device_get_devnum(parent);
        if (major(pn) == 0) {
                log_debug("%s: parent device is not a proper block device, ignoring.", name);
                goto not_found;
        }

        fd = open(node, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return log_error_errno(errno, "Failed to open %s: %m", node);

        *ret = fd;
        return 1;

not_found:
        *ret = -1;
        return 0;
}

static int enumerate_partitions(dev_t devnum) {

        _cleanup_close_ int fd = -1;
        _cleanup_(dissected_image_unrefp) DissectedImage *m = NULL;
        int r, k;

        r = open_parent(devnum, &fd);
        if (r <= 0)
                return r;

        r = dissect_image(fd, NULL, 0, DISSECT_IMAGE_GPT_ONLY, &m);
        if (r == -ENOPKG) {
                log_debug_errno(r, "No suitable partition table found, ignoring.");
                return 0;
        }
        if (r < 0)
                return log_error_errno(r, "Failed to dissect: %m");

        if (m->partitions[PARTITION_SWAP].found) {
                k = add_swap(m->partitions[PARTITION_SWAP].node);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_ESP].found) {
                k = add_esp(m->partitions + PARTITION_ESP);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_HOME].found) {
                k = add_partition_mount(m->partitions + PARTITION_HOME, "home", "/home", "Home Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_SRV].found) {
                k = add_partition_mount(m->partitions + PARTITION_SRV, "srv", "/srv", "Server Data Partition");
                if (k < 0)
                        r = k;
        }

        return r;
}

static int get_block_device(const char *path, dev_t *dev) {
        struct stat st;
        struct statfs sfs;

        assert(path);
        assert(dev);

        /* Get's the block device directly backing a file system. If
         * the block device is encrypted, returns the device mapper
         * block device. */

        if (lstat(path, &st))
                return -errno;

        if (major(st.st_dev) != 0) {
                *dev = st.st_dev;
                return 1;
        }

        if (statfs(path, &sfs) < 0)
                return -errno;

        if (F_TYPE_EQUAL(sfs.f_type, BTRFS_SUPER_MAGIC))
                return btrfs_get_block_device(path, dev);

        return 0;
}

static int get_block_device_harder(const char *path, dev_t *dev) {
        _cleanup_closedir_ DIR *d = NULL;
        _cleanup_free_ char *p = NULL, *t = NULL;
        struct dirent *de, *found = NULL;
        const char *q;
        unsigned maj, min;
        dev_t dt;
        int r;

        assert(path);
        assert(dev);

        /* Gets the backing block device for a file system, and
         * handles LUKS encrypted file systems, looking for its
         * immediate parent, if there is one. */

        r = get_block_device(path, &dt);
        if (r <= 0)
                return r;

        if (asprintf(&p, "/sys/dev/block/%u:%u/slaves", major(dt), minor(dt)) < 0)
                return -ENOMEM;

        d = opendir(p);
        if (!d) {
                if (errno == ENOENT)
                        goto fallback;

                return -errno;
        }

        FOREACH_DIRENT_ALL(de, d, return -errno) {

                if (dot_or_dot_dot(de->d_name))
                        continue;

                if (!IN_SET(de->d_type, DT_LNK, DT_UNKNOWN))
                        continue;

                if (found) {
                        _cleanup_free_ char *u = NULL, *v = NULL, *a = NULL, *b = NULL;

                        /* We found a device backed by multiple other devices. We don't really support automatic
                         * discovery on such setups, with the exception of dm-verity partitions. In this case there are
                         * two backing devices: the data partition and the hash partition. We are fine with such
                         * setups, however, only if both partitions are on the same physical device. Hence, let's
                         * verify this. */

                        u = strjoin(p, "/", de->d_name, "/../dev");
                        if (!u)
                                return -ENOMEM;

                        v = strjoin(p, "/", found->d_name, "/../dev");
                        if (!v)
                                return -ENOMEM;

                        r = read_one_line_file(u, &a);
                        if (r < 0) {
                                log_debug_errno(r, "Failed to read %s: %m", u);
                                goto fallback;
                        }

                        r = read_one_line_file(v, &b);
                        if (r < 0) {
                                log_debug_errno(r, "Failed to read %s: %m", v);
                                goto fallback;
                        }

                        /* Check if the parent device is the same. If not, then the two backing devices are on
                         * different physical devices, and we don't support that. */
                        if (!streq(a, b))
                                goto fallback;
                }

                found = de;
        }

        if (!found)
                goto fallback;

        q = strjoina(p, "/", found->d_name, "/dev");

        r = read_one_line_file(q, &t);
        if (r == -ENOENT)
                goto fallback;
        if (r < 0)
                return r;

        if (sscanf(t, "%u:%u", &maj, &min) != 2)
                return -EINVAL;

        if (maj == 0)
                goto fallback;

        *dev = makedev(maj, min);
        return 1;

fallback:
        *dev = dt;
        return 1;
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (STR_IN_SET(key, "systemd.gpt_auto", "rd.systemd.gpt_auto")) {

                r = value ? parse_boolean(value) : 1;
                if (r < 0)
                        log_warning("Failed to parse gpt-auto switch \"%s\". Ignoring.", value);
                else
                        arg_enabled = r;

        } else if (streq(key, "root")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's a root= value
                 * specified (unless it happens to be "gpt-auto") */

                arg_root_enabled = streq(value, "gpt-auto");

        } else if (streq(key, "roothash")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's roothash= defined (i.e. verity enabled) */

                arg_root_enabled = false;

        } else if (streq(key, "rw") && !value)
                arg_root_rw = true;
        else if (streq(key, "ro") && !value)
                arg_root_rw = false;

        return 0;
}

#ifdef ENABLE_EFI
static int add_root_cryptsetup(void) {

        /* If a device /dev/gpt-auto-root-luks appears, then make it pull in systemd-cryptsetup-root.service, which
         * sets it up, and causes /dev/gpt-auto-root to appear which is all we are looking for. */

        return add_cryptsetup("root", "/dev/gpt-auto-root-luks", true, false, NULL);
}
#endif

static int add_root_mount(void) {

#ifdef ENABLE_EFI
        int r;

        if (!is_efi_boot()) {
                log_debug("Not a EFI boot, not creating root mount.");
                return 0;
        }

        r = efi_loader_get_device_part_uuid(NULL);
        if (r == -ENOENT) {
                log_debug("EFI loader partition unknown, exiting.");
                return 0;
        } else if (r < 0)
                return log_error_errno(r, "Failed to read ESP partition UUID: %m");

        /* OK, we have an ESP partition, this is fantastic, so let's
         * wait for a root device to show up. A udev rule will create
         * the link for us under the right name. */

        if (in_initrd()) {
                r = generator_write_initrd_root_device_deps(arg_dest, "/dev/gpt-auto-root");
                if (r < 0)
                        return 0;

                r = add_root_cryptsetup();
                if (r < 0)
                        return r;
        }

        return add_mount(
                        "root",
                        "/dev/gpt-auto-root",
                        in_initrd() ? "/sysroot" : "/",
                        NULL,
                        arg_root_rw,
                        NULL,
                        "Root Partition",
                        in_initrd() ? SPECIAL_INITRD_ROOT_FS_TARGET : SPECIAL_LOCAL_FS_TARGET);
#else
        return 0;
#endif
}

static int add_mounts(void) {
        dev_t devno;
        int r;

        r = get_block_device_harder("/", &devno);
        if (r < 0)
                return log_error_errno(r, "Failed to determine block device of root file system: %m");
        if (r == 0) {
                r = get_block_device_harder("/usr", &devno);
                if (r < 0)
                        return log_error_errno(r, "Failed to determine block device of /usr file system: %m");
                if (r == 0) {
                        log_debug("Neither root nor /usr file system are on a (single) block device.");
                        return 0;
                }
        }

        return enumerate_partitions(devno);
}

int main(int argc, char *argv[]) {
        int r = 0, k;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (detect_container() > 0) {
                log_debug("In a container, exiting.");
                return EXIT_SUCCESS;
        }

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, 0);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (!arg_enabled) {
                log_debug("Disabled, exiting.");
                return EXIT_SUCCESS;
        }

        if (arg_root_enabled)
                r = add_root_mount();

        if (!in_initrd()) {
                k = add_mounts();
                if (k < 0)
                        r = k;
        }

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
