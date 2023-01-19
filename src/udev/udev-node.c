/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <sys/file.h>

#include "sd-id128.h"

#include "alloc-util.h"
#include "device-private.h"
#include "device-util.h"
#include "devnum-util.h"
#include "dirent-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "label.h"
#include "mkdir-label.h"
#include "parse-util.h"
#include "path-util.h"
#include "selinux-util.h"
#include "smack-util.h"
#include "stat-util.h"
#include "string-util.h"
#include "udev-node.h"
#include "user-util.h"

#define UDEV_NODE_HASH_KEY SD_ID128_MAKE(b9,6a,f1,ce,40,31,44,1a,9e,19,ec,8b,ae,f3,e3,2f)

int udev_node_cleanup(void) {
        _cleanup_closedir_ DIR *dir = NULL;

        /* This must not be called when any workers exist. It would cause a race between mkdir() called
         * by link_directory_lock() and unlinkat() called by this. */

        dir = opendir("/run/udev/links");
        if (!dir) {
                if (errno == ENOENT)
                        return 0;

                return log_debug_errno(errno, "Failed to open directory '/run/udev/links', ignoring: %m");
        }

        FOREACH_DIRENT_ALL(de, dir, break) {
                _cleanup_free_ char *lockfile = NULL;

                if (de->d_name[0] == '.')
                        continue;

                if (de->d_type != DT_DIR)
                        continue;

                /* As commented in the above, this is called when no worker exists, hence the file is not
                 * locked. On a later uevent, the lock file will be created if necessary. So, we can safely
                 * remove the file now. */
                lockfile = path_join(de->d_name, ".lock");
                if (!lockfile)
                        return log_oom_debug();

                if (unlinkat(dirfd(dir), lockfile, 0) < 0 && errno != ENOENT) {
                        log_debug_errno(errno, "Failed to remove '/run/udev/links/%s', ignoring: %m", lockfile);
                        continue;
                }

                if (unlinkat(dirfd(dir), de->d_name, AT_REMOVEDIR) < 0 && errno != ENOTEMPTY)
                        log_debug_errno(errno, "Failed to remove '/run/udev/links/%s', ignoring: %m", de->d_name);
        }

        return 0;
}

static int node_symlink(sd_device *dev, const char *devnode, const char *slink) {
        struct stat st;
        int r;

        assert(dev);
        assert(slink);

        if (!devnode) {
                r = sd_device_get_devname(dev, &devnode);
                if (r < 0)
                        return log_device_error_errno(dev, r, "Failed to get device node: %m");
        }

        if (lstat(slink, &st) >= 0) {
                if (!S_ISLNK(st.st_mode))
                        return log_device_debug_errno(dev, SYNTHETIC_ERRNO(EEXIST),
                                                      "Conflicting inode '%s' found, symlink to '%s' will not be created.",
                                                      slink, devnode);
        } else if (errno != ENOENT)
                return log_device_error_errno(dev, errno, "Failed to lstat() '%s': %m", slink);

        r = mkdir_parents_label(slink, 0755);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to create parent directory of '%s': %m", slink);

        /* use relative link */
        r = symlink_atomic_full_label(devnode, slink, /* make_relative = */ true);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to create symlink '%s' to '%s': %m", slink, devnode);

        log_device_debug(dev, "Successfully created symlink '%s' to '%s'", slink, devnode);
        return 0;
}

static int link_directory_read_one(int dirfd, const char *id, char **devnode, int *priority) {
        _cleanup_free_ char *buf = NULL;
        int tmp_prio, r;

        assert(dirfd >= 0);
        assert(id);
        assert(devnode);
        assert(priority);

        /* First, let's try to read the entry with the new format, which should replace the old format pretty
         * quickly. */

        r = readlinkat_malloc(dirfd, id, &buf);
        if (r >= 0) {
                char *colon;

                /* With the new format, the devnode and priority can be obtained from symlink itself. */

                colon = strchr(buf, ':');
                if (!colon || colon == buf)
                        return -EINVAL;

                *colon = '\0';

                r = safe_atoi(buf, &tmp_prio);
                if (r < 0)
                        return r;

                if (*devnode && tmp_prio <= *priority)
                        return 0; /* Unchanged */

                r = free_and_strdup(devnode, colon + 1);
                if (r < 0)
                        return r;

        } else if (r == -EINVAL) { /* Not a symlink ? try the old format */
                _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
                const char *val;

                /* Old format. The devnode and priority must be obtained from uevent and udev database. */

                r = sd_device_new_from_device_id(&dev, id);
                if (r < 0)
                        return r;

                r = device_get_devlink_priority(dev, &tmp_prio);
                if (r < 0)
                        return r;

                if (*devnode && tmp_prio <= *priority)
                        return 0; /* Unchanged */

                r = sd_device_get_devname(dev, &val);
                if (r < 0)
                        return r;

                r = free_and_strdup(devnode, val);
                if (r < 0)
                        return r;

        } else
                return r == -ENOENT ? -ENODEV : r;

        *priority = tmp_prio;
        return 1; /* Updated */
}

static int link_directory_find_prioritized_id(int dirfd, char **ret_id) {
        _cleanup_free_ char *devnode = NULL, *id = NULL;
        _cleanup_closedir_ DIR *dir = NULL;
        int priority, r;

        assert(dirfd >= 0);
        assert(ret_id);

        /* Find device node of device with the highest priority. If a candidate is found it's returned via
         * 'ret' otherwise if no more device left 'ret' is set to NULL. On error this function returns a
         * negative errno. */

        dir = xopendirat(dirfd, ".", O_NOFOLLOW);
        if (!dir)
                return -errno;

        FOREACH_DIRENT(de, dir, break) {

                if (streq(de->d_name, "owner"))
                        continue;

                r = link_directory_read_one(dirfd, de->d_name, &devnode, &priority);
                if (r <= 0) {
                        if (r < 0)
                                log_warning_errno(r, "Failed to read '%s', ignoring: %m", de->d_name);
                        continue;
                }

                /* Of course, this check is racy, but it is not necessary to be perfect. Even if the device
                 * node will be removed after this check, we will receive 'remove' uevent, and the invalid
                 * symlink will be removed during processing the event. The check is just for shortening the
                 * timespan that the symlink points to a non-existing device node. */
                if (access(devnode, F_OK) < 0)
                        continue;

                r = free_and_strdup(&id, de->d_name);
                if (r < 0)
                        return r;
        }

        *ret_id = TAKE_PTR(id);
        return 0;
}

static int link_directory_update(int dirfd, sd_device *dev, bool add, const char **ret_devname, int *ret_prio) {
        _cleanup_free_ char *data = NULL, *buf = NULL;
        const char *devname, *id;
        int priority;
        int r;

        assert(dev);
        assert(dirfd >= 0);
        assert(ret_devname);
        assert(ret_prio);

        r = device_get_device_id(dev, &id);
        if (r < 0)
                return r;

        r = sd_device_get_devname(dev, &devname);
        if (r < 0)
                return r;

        r = device_get_devlink_priority(dev, &priority);
        if (r < 0)
                return r;

        if (add) {
                if (asprintf(&data, "%i:%s", priority, devname) < 0)
                        return -ENOMEM;

                if (readlinkat_malloc(dirfd, id, &buf) < 0 || !streq(buf, data)) {
                        (void) unlinkat(dirfd, id, 0);

                        if (symlinkat(data, dirfd, id) < 0)
                                return -errno;
                }
        } else {
                if (unlinkat(dirfd, id, 0) < 0 && errno != ENOENT)
                        return -errno;
        }

        *ret_devname = devname;
        *ret_prio = priority;

        return 0;
}

size_t udev_node_escape_path(const char *src, char *dest, size_t size) {
        size_t i, j;
        uint64_t h;

        assert(src);
        assert(dest);
        assert(size >= 12);

        for (i = 0, j = 0; src[i] != '\0'; i++) {
                if (src[i] == '/') {
                        if (j+4 >= size - 12 + 1)
                                goto toolong;
                        memcpy(&dest[j], "\\x2f", 4);
                        j += 4;
                } else if (src[i] == '\\') {
                        if (j+4 >= size - 12 + 1)
                                goto toolong;
                        memcpy(&dest[j], "\\x5c", 4);
                        j += 4;
                } else {
                        if (j+1 >= size - 12 + 1)
                                goto toolong;
                        dest[j] = src[i];
                        j++;
                }
        }
        dest[j] = '\0';
        return j;

toolong:
        /* If the input path is too long to encode as a filename, then let's suffix with a string
         * generated from the hash of the path. */

        h = siphash24_string(src, UDEV_NODE_HASH_KEY.bytes);

        for (unsigned k = 0; k <= 10; k++)
                dest[size - k - 2] = urlsafe_base64char((h >> (k * 6)) & 63);

        dest[size - 1] = '\0';
        return size - 1;
}

static int link_directory_get_name(const char *slink, char **ret) {
        _cleanup_free_ char *s = NULL, *dirname = NULL;
        char name_enc[NAME_MAX+1];
        const char *name;

        assert(slink);
        assert(ret);

        s = strdup(slink);
        if (!s)
                return -ENOMEM;

        path_simplify(s);

        if (!path_is_normalized(s))
                return -EINVAL;

        name = path_startswith(s, "/dev");
        if (empty_or_root(name))
                return -EINVAL;

        udev_node_escape_path(name, name_enc, sizeof(name_enc));

        dirname = path_join("/run/udev/links", name_enc);
        if (!dirname)
                return -ENOMEM;

        *ret = TAKE_PTR(dirname);
        return 0;
}

static int link_directory_open(sd_device *dev, const char *slink, int *ret_dirfd, int *ret_lockfd) {
        _cleanup_close_ int dirfd = -EBADF, lockfd = -EBADF;
        _cleanup_free_ char *dirname = NULL;
        int r;

        assert(dev);
        assert(slink);
        assert(ret_dirfd);
        assert(ret_lockfd);

        r = link_directory_get_name(slink, &dirname);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to build stack directory name for '%s': %m", slink);

        r = mkdir_parents(dirname, 0755);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to create stack directory '%s': %m", dirname);

        dirfd = open_mkdir_at(AT_FDCWD, dirname, O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW | O_RDONLY, 0755);
        if (dirfd < 0)
                return log_device_error_errno(dev, dirfd, "Failed to open stack directory '%s': %m", dirname);

        lockfd = openat(dirfd, ".lock", O_CLOEXEC | O_NOFOLLOW | O_RDONLY | O_CREAT, 0600);
        if (lockfd < 0)
                return log_device_error_errno(dev, errno, "Failed to create lock file for stack directory '%s': %m", dirname);

        if (flock(lockfd, LOCK_EX) < 0)
                return log_device_error_errno(dev, errno, "Failed to place a lock on lock file for '%s': %m", dirname);

        *ret_dirfd = TAKE_FD(dirfd);
        *ret_lockfd = TAKE_FD(lockfd);
        return 0;
}

static int link_directory_set_current_owner(int dirfd, const char *slink, sd_device *dev, const char *id) {
        _cleanup_free_ char *devname = NULL;
        int priority;
        int r;

        assert(dev);
        assert(dirfd >= 0);
        assert(slink);

        (void) unlinkat(dirfd, "owner", 0);

        if (!id) {
                log_device_debug(dev, "No reference left for '%s', removing", slink);

                if (unlink(slink) < 0 && errno != ENOENT)
                        log_device_warning_errno(dev, errno, "Failed to remove '%s', ignoring: %m", slink);

                (void) rmdir_parents(slink, "/dev");
                return 0;
        }

        r = link_directory_read_one(dirfd, id, &devname, &priority);
        if (r < 0)
                return r;

        r = node_symlink(dev, devname, slink);
        if (r < 0)
                return r;

        r = symlinkat(id, dirfd, "owner");
        if (r < 0)
                log_device_warning_errno(dev, errno, "Failed to update owner of '%s': %m", slink);

        return r;
}

static int link_directory_get_current_owner(int dirfd, const char *slink, char **ret_devname, int *ret_prio) {
        _cleanup_free_ char *devname = NULL, *id = NULL;
        int prio, r;

        assert(dirfd >= 0);
        assert(slink);
        assert(ret_devname);

        /* Return -ENODEV if the symlink has currently no owner */
        r = readlinkat_malloc(dirfd, "owner", &id);
        if (r < 0)
                return r == -ENOENT ? -ENODEV : r;

        r = link_directory_read_one(dirfd, id, &devname, &prio);
        if (r < 0)
                return r;

        *ret_devname = TAKE_PTR(devname);
        if (ret_prio)
                *ret_prio = prio;

        return 0;
}

static int link_add(int dirfd, sd_device *dev, const char *devname, int devprio, const char *slink) {
        _cleanup_free_ char *owner = NULL;
        int owner_prio;
        int r;

        assert(dirfd >= 0);
        assert(dev);
        assert(devname);
        assert(slink);

        /* We shortcut things if the current owner of 'slink' has a priority higher or equal than the
         * priority of the device being added. Otherwise we take ownership of the symlink. In any cases we
         * don't have to search for the prioritized device, which can be slow if numerous devices are
         * claiming the same symlink (systems with large number of LUNs for example). */

        r = link_directory_get_current_owner(dirfd, slink, &owner, &owner_prio);
        if (r < 0 && r != -ENODEV)
                return log_device_error_errno(dev, r, "Failed to retrieve current priority of %s: %m", slink);

        if (owner_prio < devprio || r == -ENODEV) {
                const char *id;

                assert_se(device_get_device_id(dev, &id) >= 0);

                return link_directory_set_current_owner(dirfd, slink, dev, id);
        }

        log_device_debug(dev, "Symlink %s is owned by %s with %s priority (%d), skipping.",
                         slink,
                         owner,
                         owner_prio > devprio ? "higher" : "equal",
                         owner_prio);
        return 0;
}

static int link_remove(int dirfd, sd_device *dev, const char *devname, const char *slink) {
        _cleanup_free_ char *owner = NULL, *found = NULL;
        int r;

        assert(dirfd >= 0);
        assert(dev);
        assert(devname);
        assert(slink);

        /* Check whether the symlink is owned by another device. If it's the case don't try to replace it. If
         * the symlink is still in place but the claiming device is gone, let the relevant uevent (not yet
         * processed) deals with the symlink handling itself. */

        r = link_directory_get_current_owner(dirfd, slink, &owner, NULL);
        if (r >= 0) {
                /* The symlink is owned by another device. If it was owned by 'dev', we would get -ENODEV
                 * since its entry in the link directory has just been removed. */
                assert(!streq(owner, devname));
                return 0;
        }
        if (r != -ENODEV) /* ENODEV when the owner is 'dev' */
                log_device_warning_errno(dev, r, "Failed to retrieve current owner of %s, ignoring: %m", slink);

        /* Find a substitute. */
        r = link_directory_find_prioritized_id(dirfd, &found);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to find the device with highest priority for '%s': %m", slink);

        return link_directory_set_current_owner(dirfd, slink, dev, found);
}

static int link_update(sd_device *dev, const char *slink, bool add) {
        _cleanup_close_ int dirfd = -EBADF, lockfd = -EBADF;
        const char *devname = NULL; /* avoid false may-be-used-uninitialized warning */
        int priority = 0; /* ditto */
        int r;

        assert(dev);
        assert(slink);

        r = link_directory_open(dev, slink, &dirfd, &lockfd);
        if (r < 0)
                return r;

        r = link_directory_update(dirfd, dev, add, &devname, &priority);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to update link directory for '%s': %m", slink);

        if (add)
                return link_add(dirfd, dev, devname, priority, slink);

        return link_remove(dirfd, dev, devname, slink);
}

static int device_get_devpath_by_devnum(sd_device *dev, char **ret) {
        const char *subsystem;
        dev_t devnum;
        int r;

        assert(dev);
        assert(ret);

        r = sd_device_get_subsystem(dev, &subsystem);
        if (r < 0)
                return r;

        r = sd_device_get_devnum(dev, &devnum);
        if (r < 0)
                return r;

        return device_path_make_major_minor(streq(subsystem, "block") ? S_IFBLK : S_IFCHR, devnum, ret);
}

int udev_node_update(sd_device *dev, sd_device *dev_old) {
        _cleanup_free_ char *filename = NULL;
        const char *devlink;
        int r;

        assert(dev);
        assert(dev_old);

        /* update possible left-over symlinks */
        FOREACH_DEVICE_DEVLINK(dev_old, devlink) {
                /* check if old link name still belongs to this device */
                if (device_has_devlink(dev, devlink))
                        continue;

                log_device_debug(dev,
                                 "Removing/updating old device symlink '%s', which is no longer belonging to this device.",
                                 devlink);

                r = link_update(dev, devlink, /* add = */ false);
                if (r < 0)
                        log_device_warning_errno(dev, r,
                                                 "Failed to remove/update device symlink '%s', ignoring: %m",
                                                 devlink);
        }

        /* create/update symlinks, add symlinks to name index */
        FOREACH_DEVICE_DEVLINK(dev, devlink) {
                r = link_update(dev, devlink, /* add = */ true);
                if (r < 0)
                        log_device_warning_errno(dev, r,
                                                 "Failed to create/update device symlink '%s', ignoring: %m",
                                                 devlink);
        }

        r = device_get_devpath_by_devnum(dev, &filename);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get device path: %m");

        /* always add /dev/{block,char}/$major:$minor */
        r = node_symlink(dev, NULL, filename);
        if (r < 0)
                return log_device_warning_errno(dev, r, "Failed to create device symlink '%s': %m", filename);

        return 0;
}

int udev_node_remove(sd_device *dev) {
        _cleanup_free_ char *filename = NULL;
        const char *devlink;
        int r;

        assert(dev);

        /* remove/update symlinks, remove symlinks from name index */
        FOREACH_DEVICE_DEVLINK(dev, devlink) {
                r = link_update(dev, devlink, /* add = */ false);
                if (r < 0)
                        log_device_warning_errno(dev, r,
                                                 "Failed to remove/update device symlink '%s', ignoring: %m",
                                                 devlink);
        }

        r = device_get_devpath_by_devnum(dev, &filename);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to get device path: %m");

        /* remove /dev/{block,char}/$major:$minor */
        if (unlink(filename) < 0 && errno != ENOENT)
                return log_device_error_errno(dev, errno, "Failed to remove '%s': %m", filename);

        return 0;
}

static int udev_node_apply_permissions_impl(
                sd_device *dev, /* can be NULL, only used for logging. */
                int node_fd,
                const char *devnode,
                bool apply_mac,
                mode_t mode,
                uid_t uid,
                gid_t gid,
                OrderedHashmap *seclabel_list) {

        bool apply_mode, apply_uid, apply_gid;
        struct stat stats;
        int r;

        assert(node_fd >= 0);
        assert(devnode);

        if (fstat(node_fd, &stats) < 0)
                return log_device_error_errno(dev, errno, "cannot stat() node %s: %m", devnode);

        /* If group is set, but mode is not set, "upgrade" mode for the group. */
        if (mode == MODE_INVALID && gid_is_valid(gid) && gid > 0)
                mode = 0660;

        apply_mode = mode != MODE_INVALID && (stats.st_mode & 0777) != (mode & 0777);
        apply_uid = uid_is_valid(uid) && stats.st_uid != uid;
        apply_gid = gid_is_valid(gid) && stats.st_gid != gid;

        if (apply_mode || apply_uid || apply_gid || apply_mac) {
                bool selinux = false, smack = false;
                const char *name, *label;

                if (apply_mode || apply_uid || apply_gid) {
                        log_device_debug(dev, "Setting permissions %s, uid=" UID_FMT ", gid=" GID_FMT ", mode=%#o",
                                         devnode,
                                         uid_is_valid(uid) ? uid : stats.st_uid,
                                         gid_is_valid(gid) ? gid : stats.st_gid,
                                         mode != MODE_INVALID ? mode & 0777 : stats.st_mode & 0777);

                        r = fchmod_and_chown(node_fd, mode, uid, gid);
                        if (r < 0)
                                log_device_full_errno(dev, r == -ENOENT ? LOG_DEBUG : LOG_ERR, r,
                                                      "Failed to set owner/mode of %s to uid=" UID_FMT
                                                      ", gid=" GID_FMT ", mode=%#o: %m",
                                                      devnode,
                                                      uid_is_valid(uid) ? uid : stats.st_uid,
                                                      gid_is_valid(gid) ? gid : stats.st_gid,
                                                      mode != MODE_INVALID ? mode & 0777 : stats.st_mode & 0777);
                } else
                        log_device_debug(dev, "Preserve permissions of %s, uid=" UID_FMT ", gid=" GID_FMT ", mode=%#o",
                                         devnode,
                                         uid_is_valid(uid) ? uid : stats.st_uid,
                                         gid_is_valid(gid) ? gid : stats.st_gid,
                                         mode != MODE_INVALID ? mode & 0777 : stats.st_mode & 0777);

                /* apply SECLABEL{$module}=$label */
                ORDERED_HASHMAP_FOREACH_KEY(label, name, seclabel_list) {
                        int q;

                        if (streq(name, "selinux")) {
                                selinux = true;

                                q = mac_selinux_apply_fd(node_fd, devnode, label);
                                if (q < 0)
                                        log_device_full_errno(dev, q == -ENOENT ? LOG_DEBUG : LOG_ERR, q,
                                                              "SECLABEL: failed to set SELinux label '%s': %m", label);
                                else
                                        log_device_debug(dev, "SECLABEL: set SELinux label '%s'", label);

                        } else if (streq(name, "smack")) {
                                smack = true;

                                q = mac_smack_apply_fd(node_fd, SMACK_ATTR_ACCESS, label);
                                if (q < 0)
                                        log_device_full_errno(dev, q == -ENOENT ? LOG_DEBUG : LOG_ERR, q,
                                                              "SECLABEL: failed to set SMACK label '%s': %m", label);
                                else
                                        log_device_debug(dev, "SECLABEL: set SMACK label '%s'", label);

                        } else
                                log_device_error(dev, "SECLABEL: unknown subsystem, ignoring '%s'='%s'", name, label);
                }

                /* set the defaults */
                if (!selinux)
                        (void) mac_selinux_fix_full(node_fd, NULL, devnode, LABEL_IGNORE_ENOENT);
                if (!smack)
                        (void) mac_smack_apply_fd(node_fd, SMACK_ATTR_ACCESS, NULL);
        }

        /* always update timestamp when we re-use the node, like on media change events */
        r = futimens_opath(node_fd, NULL);
        if (r < 0)
                log_device_debug_errno(dev, r, "Failed to adjust timestamp of node %s: %m", devnode);

        return 0;
}

int udev_node_apply_permissions(
                sd_device *dev,
                bool apply_mac,
                mode_t mode,
                uid_t uid,
                gid_t gid,
                OrderedHashmap *seclabel_list) {

        const char *devnode;
        _cleanup_close_ int node_fd = -EBADF;
        int r;

        assert(dev);

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devname: %m");

        node_fd = sd_device_open(dev, O_PATH|O_CLOEXEC);
        if (node_fd < 0) {
                if (ERRNO_IS_DEVICE_ABSENT(node_fd)) {
                        log_device_debug_errno(dev, node_fd, "Device node %s is missing, skipping handling.", devnode);
                        return 0; /* This is necessarily racey, so ignore missing the device */
                }

                return log_device_debug_errno(dev, node_fd, "Cannot open node %s: %m", devnode);
        }

        return udev_node_apply_permissions_impl(dev, node_fd, devnode, apply_mac, mode, uid, gid, seclabel_list);
}

int static_node_apply_permissions(
                const char *name,
                mode_t mode,
                uid_t uid,
                gid_t gid,
                char **tags) {

        _cleanup_free_ char *unescaped_filename = NULL;
        _cleanup_close_ int node_fd = -EBADF;
        const char *devnode;
        struct stat stats;
        int r;

        assert(name);

        if (uid == UID_INVALID && gid == GID_INVALID && mode == MODE_INVALID && !tags)
                return 0;

        devnode = strjoina("/dev/", name);

        node_fd = open(devnode, O_PATH|O_CLOEXEC);
        if (node_fd < 0) {
                if (errno != ENOENT)
                        return log_error_errno(errno, "Failed to open %s: %m", devnode);
                return 0;
        }

        if (fstat(node_fd, &stats) < 0)
                return log_error_errno(errno, "Failed to stat %s: %m", devnode);

        if (!S_ISBLK(stats.st_mode) && !S_ISCHR(stats.st_mode)) {
                log_warning("%s is neither block nor character device, ignoring.", devnode);
                return 0;
        }

        if (!strv_isempty(tags)) {
                unescaped_filename = xescape(name, "/.");
                if (!unescaped_filename)
                        return log_oom();
        }

        /* export the tags to a directory as symlinks, allowing otherwise dead nodes to be tagged */
        STRV_FOREACH(t, tags) {
                _cleanup_free_ char *p = NULL;

                p = path_join("/run/udev/static_node-tags/", *t, unescaped_filename);
                if (!p)
                        return log_oom();

                r = mkdir_parents(p, 0755);
                if (r < 0)
                        return log_error_errno(r, "Failed to create parent directory for %s: %m", p);

                r = symlink(devnode, p);
                if (r < 0 && errno != EEXIST)
                        return log_error_errno(errno, "Failed to create symlink %s -> %s: %m", p, devnode);
        }

        return udev_node_apply_permissions_impl(NULL, node_fd, devnode, false, mode, uid, gid, NULL);
}
