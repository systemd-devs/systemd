/* SPDX-License-Identifier: GPL-2.0+ */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/sem.h>

#include "alloc-util.h"
#include "device-nodes.h"
#include "device-private.h"
#include "device-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "format-util.h"
#include "fs-util.h"
#include "libudev-util.h"
#include "mkdir.h"
#include "path-util.h"
#include "selinux-util.h"
#include "smack-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strxcpyx.h"
#include "udev-node.h"
#include "siphash24.h"
#include "hash-funcs.h"
#include "udev.h"

/*
 * Size of the semaphore set used for locking the access to a given
 * symlink. The index into this array is derived from the symlink name
 * using a hash function. N_SEMAPHORES must be a power of 2.
 * The default maximum semaphore set size under Linux (SEMMSL) is 32000.
 */
#define N_SEMAPHORES 1024
static unsigned int n_semaphores = N_SEMAPHORES;
static const char links_dirname[] = "/run/udev/links/";

static int node_symlink(sd_device *dev, const char *node, const char *slink) {
        _cleanup_free_ char *slink_dirname = NULL, *target = NULL;
        const char *id_filename, *slink_tmp;
        struct stat stats;
        int r;

        assert(dev);
        assert(node);
        assert(slink);

        slink_dirname = dirname_malloc(slink);
        if (!slink_dirname)
                return log_oom();

        /* use relative link */
        r = path_make_relative(slink_dirname, node, &target);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to get relative path from '%s' to '%s': %m", slink, node);

        /* preserve link with correct target, do not replace node of other device */
        if (lstat(slink, &stats) == 0) {
                if (S_ISBLK(stats.st_mode) || S_ISCHR(stats.st_mode)) {
                        log_device_error(dev, "Conflicting device node '%s' found, link to '%s' will not be created.", slink, node);
                        return -EOPNOTSUPP;
                } else if (S_ISLNK(stats.st_mode)) {
                        _cleanup_free_ char *buf = NULL;

                        if (readlink_malloc(slink, &buf) >= 0 &&
                            streq(target, buf)) {
                                log_device_debug(dev, "Preserve already existing symlink '%s' to '%s'", slink, target);
                                (void) label_fix(slink, LABEL_IGNORE_ENOENT);
                                (void) utimensat(AT_FDCWD, slink, NULL, AT_SYMLINK_NOFOLLOW);
                                return 0;
                        }
                }
        } else {
                log_device_debug(dev, "Creating symlink '%s' to '%s'", slink, target);
                do {
                        r = mkdir_parents_label(slink, 0755);
                        if (!IN_SET(r, 0, -ENOENT))
                                break;
                        mac_selinux_create_file_prepare(slink, S_IFLNK);
                        if (symlink(target, slink) < 0)
                                r = -errno;
                        mac_selinux_create_file_clear();
                } while (r == -ENOENT);
                if (r == 0)
                        return 0;
                if (r < 0)
                        log_device_debug_errno(dev, r, "Failed to create symlink '%s' to '%s', trying to replace '%s': %m", slink, target, slink);
        }

        log_device_debug(dev, "Atomically replace '%s'", slink);
        r = device_get_id_filename(dev, &id_filename);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to get id_filename: %m");
        slink_tmp = strjoina(slink, ".tmp-", id_filename);
        (void) unlink(slink_tmp);
        do {
                r = mkdir_parents_label(slink_tmp, 0755);
                if (!IN_SET(r, 0, -ENOENT))
                        break;
                mac_selinux_create_file_prepare(slink_tmp, S_IFLNK);
                if (symlink(target, slink_tmp) < 0)
                        r = -errno;
                mac_selinux_create_file_clear();
        } while (r == -ENOENT);
        if (r < 0)
                return log_device_error_errno(dev, r, "Failed to create symlink '%s' to '%s': %m", slink_tmp, target);

        if (rename(slink_tmp, slink) < 0) {
                r = log_device_error_errno(dev, errno, "Failed to rename '%s' to '%s' failed: %m", slink_tmp, slink);
                (void) unlink(slink_tmp);
        }

        return r;
}

/* find device node of device with highest priority */
static int link_find_prioritized(sd_device *dev, bool add, const char *stackdir, char **ret) {
        _cleanup_closedir_ DIR *dir = NULL;
        _cleanup_free_ char *target = NULL;
        struct dirent *dent;
        int r, priority = 0;

        assert(!add || dev);
        assert(stackdir);
        assert(ret);

        if (add) {
                const char *devnode;

                r = device_get_devlink_priority(dev, &priority);
                if (r < 0)
                        return r;

                r = sd_device_get_devname(dev, &devnode);
                if (r < 0)
                        return r;

                target = strdup(devnode);
                if (!target)
                        return -ENOMEM;
        }

        dir = opendir(stackdir);
        if (!dir) {
                if (target) {
                        *ret = TAKE_PTR(target);
                        return 0;
                }

                return -errno;
        }

        FOREACH_DIRENT_ALL(dent, dir, break) {
                _cleanup_(sd_device_unrefp) sd_device *dev_db = NULL;
                const char *devnode, *id_filename;
                int db_prio = 0;

                if (dent->d_name[0] == '\0')
                        break;
                if (dent->d_name[0] == '.')
                        continue;

                log_device_debug(dev, "Found '%s' claiming '%s'", dent->d_name, stackdir);

                if (device_get_id_filename(dev, &id_filename) < 0)
                        continue;

                /* did we find ourself? */
                if (streq(dent->d_name, id_filename))
                        continue;

                if (sd_device_new_from_device_id(&dev_db, dent->d_name) < 0)
                        continue;

                if (sd_device_get_devname(dev_db, &devnode) < 0)
                        continue;

                if (device_get_devlink_priority(dev_db, &db_prio) < 0)
                        continue;

                if (target && db_prio <= priority)
                        continue;

                log_device_debug(dev_db, "Device claims priority %i for '%s'", db_prio, stackdir);

                r = free_and_strdup(&target, devnode);
                if (r < 0)
                        return r;
                priority = db_prio;
        }

        if (!target)
                return -ENOENT;

        *ret = TAKE_PTR(target);
        return 0;
}

static int init_link_semaphores(const char *path) {
        key_t key;
        struct seminfo si;
        int semid;

        /* make sure n_semaphores starts out as a power of 2 */
        assert((n_semaphores & (n_semaphores - 1)) == 0);

        if (semctl(0, 0, IPC_INFO, &si) < 0)
                return log_error_errno(-errno, "Failed to query IPC_INFO: %m");

        if (si.semmsl <= 0)
                return log_error_errno(SYNTHETIC_ERRNO(ERANGE), "SEMMSL is 0");

        while (n_semaphores > (unsigned int)si.semmsl)
                n_semaphores >>= 1;

        key = ftok(path, 0);
        semid = semget(key, n_semaphores, 0600|IPC_CREAT|IPC_EXCL);

        if (semid >= 0) {
                _cleanup_free_ unsigned short *val = NULL;
                unsigned int i;
                int err;

                val = malloc(n_semaphores * sizeof(*val));
                if (val) {
                        for (i = 0; i < n_semaphores; i++)
                                val[i] = 1;
                        if (semctl(semid, 0, SETALL, val) == 0) {
                                /* Dummy semop to initialize sem_otime */
                                struct sembuf dummy_op[]  = {
                                        { .sem_op = -1, },
                                        { .sem_op = 1, },
                                };
                                if (semop(semid, dummy_op, (sizeof(dummy_op)/sizeof(*dummy_op))) == 0) {
                                        log_debug("Created semaphore set with %u members",
                                                  n_semaphores);
                                        return semid;
                                } else
                                        log_error_errno(-errno, "Failed to set sem_otime: %m");
                        } else
                                log_error_errno(-errno, "Failed to initialize semaphores: %m");
                };

                err = -errno;
                /* Cleanup after error */
                if (semctl(semid, 0, IPC_RMID) != 0)
                        log_error_errno(-errno, "Failed to remove semaphore set: %m");
                return err;

        } else if (errno == EEXIST) {
                const unsigned int RETRIES = 10, SLEEP_US = 10000;
                unsigned int i;

                semid = semget(key, 0, 0);
                if (semid == -1)
                        return log_error_errno(-errno, "Failed to get semaphore set: %m");

                for (i = 0; i < RETRIES; i++) {
                        struct semid_ds ds;

                        /* Wait for initialization to finish */
                        if (semctl(semid, 0, IPC_STAT, &ds) == 0 && ds.sem_otime != 0) {
                                if (ds.sem_nsems == 0 || (ds.sem_nsems & (ds.sem_nsems - 1)) != 0)
                                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                               "Semaphore set has invalid size %lu",
                                                               ds.sem_nsems);
                                n_semaphores = ds.sem_nsems;
                                return semid;
                        }
                        usleep(SLEEP_US);
                }
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT),
                                       "Semaphore set not initialized after %d us",
                                       RETRIES * SLEEP_US);
        } else
                return log_error_errno(-errno, "Failed to create semaphore set: %m");
}

static unsigned short get_sema_index(const char *link) {
        static const unsigned char seed[16] = { 0x6b, 0xb0, 0xb1, 0x28,
                                                0xf7, 0x8c, 0x59, 0xb2,
                                                0x05, 0x1d, 0xd1, 0xa2,
                                                0xcc, 0x12, 0xae, 0xb7 };
        struct siphash state;
        uint64_t hash;

        if (n_semaphores == 1)
                return 0;

        siphash24_init(&state, seed);
        path_hash_func(link, &state);
        hash = siphash24_finalize(&state);

        return hash & (n_semaphores-1);
}

static int _slink_semop(int semid, unsigned short semidx, int op, const char *msg) {
        struct sembuf sb = { .sem_num = semidx, .sem_op = op, .sem_flg = 0 };

        /* semid < 0: semaphore setup failed, locking is disabled */
        if (semid < 0)
                return 0;
        if (semop(semid, &sb, 1) == -1)
                return log_warning_errno(-errno, "Failed to %s semaphore: %m", msg);
        return 0;
}

#define lock_slink(semid, semidx) \
        _slink_semop((semid), (semidx), -1, "acquire")
#define unlock_slink(semid, semidx) \
        _slink_semop((semid), (semidx), 1, "release")

enum {
        SEMID_UNSET = -1,
        SEMID_BAD = -2,
};

/* manage "stack of names" with possibly specified device priorities */
static int link_update(sd_device *dev, const char *slink, bool add) {
        _cleanup_free_ char *target = NULL, *filename = NULL, *dirname = NULL;
        char name_enc[PATH_MAX];
        const char *id_filename;
        static int semid = SEMID_UNSET;
        unsigned short semidx;
        int r, lock_failed;

        assert(dev);
        assert(slink);

        if (semid == SEMID_UNSET) {
                semid = init_link_semaphores(links_dirname);
                if (semid < 0) {
                        log_error_errno(semid, "Locking under %s is disabled: %m",
                                        links_dirname);
                        semid = SEMID_BAD;
                }
        }

        r = device_get_id_filename(dev, &id_filename);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get id_filename: %m");

        util_path_encode(slink + STRLEN("/dev"), name_enc, sizeof(name_enc));
        dirname = path_join(links_dirname, name_enc);
        if (!dirname)
                return log_oom();
        filename = path_join(dirname, id_filename);
        if (!filename)
                return log_oom();

        mkdir_parents(dirname, 0755);
        semidx = get_sema_index(slink);
        lock_failed = lock_slink(semid, semidx);
        if (lock_failed)
                log_error_errno(-errno, "Failed to lock %s: %m", slink);

        if (!add && unlink(filename) == 0)
                (void) rmdir(dirname);

        r = link_find_prioritized(dev, add, dirname, &target);
        if (r < 0) {
                log_device_debug(dev, "No reference left, removing '%s'", slink);
                if (unlink(slink) == 0)
                        (void) rmdir_parents(slink, "/");
        } else
                (void) node_symlink(dev, target, slink);

        if (add)
                do {
                        _cleanup_close_ int fd = -1;

                        r = mkdir_parents(filename, 0755);
                        if (!IN_SET(r, 0, -ENOENT))
                                break;
                        fd = open(filename, O_WRONLY|O_CREAT|O_CLOEXEC|O_TRUNC|O_NOFOLLOW, 0444);
                        if (fd < 0)
                                r = -errno;
                } while (r == -ENOENT);

        if (!lock_failed)
                unlock_slink(semid, semidx);

        return 0;
}

int udev_node_update_old_links(sd_device *dev, sd_device *dev_old) {
        const char *name, *devpath;
        int r;

        assert(dev);
        assert(dev_old);

        r = sd_device_get_devpath(dev, &devpath);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devpath: %m");

        /* update possible left-over symlinks */
        FOREACH_DEVICE_DEVLINK(dev_old, name) {
                const char *name_current;
                bool found = false;

                /* check if old link name still belongs to this device */
                FOREACH_DEVICE_DEVLINK(dev, name_current)
                        if (streq(name, name_current)) {
                                found = true;
                                break;
                        }

                if (found)
                        continue;

                log_device_debug(dev, "Updating old name, '%s' no longer belonging to '%s'",
                                 name, devpath);
                link_update(dev, name, false);
        }

        return 0;
}

static int node_permissions_apply(sd_device *dev, bool apply,
                                  mode_t mode, uid_t uid, gid_t gid,
                                  Hashmap *seclabel_list) {
        const char *devnode, *subsystem, *id_filename = NULL;
        struct stat stats;
        dev_t devnum;
        int r = 0;

        assert(dev);

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devname: %m");
        r = sd_device_get_subsystem(dev, &subsystem);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get subsystem: %m");
        r = sd_device_get_devnum(dev, &devnum);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devnum: %m");
        (void) device_get_id_filename(dev, &id_filename);

        if (streq(subsystem, "block"))
                mode |= S_IFBLK;
        else
                mode |= S_IFCHR;

        if (lstat(devnode, &stats) < 0)
                return log_device_debug_errno(dev, errno, "cannot stat() node '%s' (%m)", devnode);

        if (((stats.st_mode & S_IFMT) != (mode & S_IFMT)) || (stats.st_rdev != devnum))
                return log_device_debug_errno(dev, EEXIST, "Found node '%s' with non-matching devnum %s, skip handling",
                                              devnode, id_filename);

        if (apply) {
                bool selinux = false, smack = false;
                const char *name, *label;
                Iterator i;

                if ((stats.st_mode & 0777) != (mode & 0777) || stats.st_uid != uid || stats.st_gid != gid) {
                        log_device_debug(dev, "Setting permissions %s, %#o, uid=%u, gid=%u", devnode, mode, uid, gid);
                        if (chmod(devnode, mode) < 0)
                                r = log_device_warning_errno(dev, errno, "Failed to set mode of %s to %#o: %m", devnode, mode);
                        if (chown(devnode, uid, gid) < 0)
                                r = log_device_warning_errno(dev, errno, "Failed to set owner of %s to uid=%u, gid=%u: %m", devnode, uid, gid);
                } else
                        log_device_debug(dev, "Preserve permissions of %s, %#o, uid=%u, gid=%u", devnode, mode, uid, gid);

                /* apply SECLABEL{$module}=$label */
                HASHMAP_FOREACH_KEY(label, name, seclabel_list, i) {
                        int q;

                        if (streq(name, "selinux")) {
                                selinux = true;

                                q = mac_selinux_apply(devnode, label);
                                if (q < 0)
                                        log_device_error_errno(dev, q, "SECLABEL: failed to set SELinux label '%s': %m", label);
                                else
                                        log_device_debug(dev, "SECLABEL: set SELinux label '%s'", label);

                        } else if (streq(name, "smack")) {
                                smack = true;

                                q = mac_smack_apply(devnode, SMACK_ATTR_ACCESS, label);
                                if (q < 0)
                                        log_device_error_errno(dev, q, "SECLABEL: failed to set SMACK label '%s': %m", label);
                                else
                                        log_device_debug(dev, "SECLABEL: set SMACK label '%s'", label);

                        } else
                                log_device_error(dev, "SECLABEL: unknown subsystem, ignoring '%s'='%s'", name, label);
                }

                /* set the defaults */
                if (!selinux)
                        (void) mac_selinux_fix(devnode, LABEL_IGNORE_ENOENT);
                if (!smack)
                        (void) mac_smack_apply(devnode, SMACK_ATTR_ACCESS, NULL);
        }

        /* always update timestamp when we re-use the node, like on media change events */
        (void) utimensat(AT_FDCWD, devnode, NULL, 0);

        return r;
}

static int xsprintf_dev_num_path_from_sd_device(sd_device *dev, char **ret) {
        char filename[DEV_NUM_PATH_MAX], *s;
        const char *subsystem;
        dev_t devnum;
        int r;

        assert(ret);

        r = sd_device_get_subsystem(dev, &subsystem);
        if (r < 0)
                return r;

        r = sd_device_get_devnum(dev, &devnum);
        if (r < 0)
                return r;

        xsprintf_dev_num_path(filename,
                              streq(subsystem, "block") ? "block" : "char",
                              devnum);

        s = strdup(filename);
        if (!s)
                return -ENOMEM;

        *ret = s;
        return 0;
}

int udev_node_add(sd_device *dev, bool apply,
                  mode_t mode, uid_t uid, gid_t gid,
                  Hashmap *seclabel_list) {
        const char *devnode, *devlink;
        _cleanup_free_ char *filename = NULL;
        int r;

        assert(dev);

        r = sd_device_get_devname(dev, &devnode);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get devnode: %m");

        if (DEBUG_LOGGING) {
                const char *id_filename = NULL;

                (void) device_get_id_filename(dev, &id_filename);
                log_device_debug(dev, "Handling device node '%s', devnum=%s, mode=%#o, uid="UID_FMT", gid="GID_FMT,
                                 devnode, strnull(id_filename), mode, uid, gid);
        }

        r = node_permissions_apply(dev, apply, mode, uid, gid, seclabel_list);
        if (r < 0)
                return r;

        r = xsprintf_dev_num_path_from_sd_device(dev, &filename);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get device path: %m");

        /* always add /dev/{block,char}/$major:$minor */
        (void) node_symlink(dev, devnode, filename);

        /* create/update symlinks, add symlinks to name index */
        FOREACH_DEVICE_DEVLINK(dev, devlink)
                (void) link_update(dev, devlink, true);

        return 0;
}

int udev_node_remove(sd_device *dev) {
        _cleanup_free_ char *filename = NULL;
        const char *devlink;
        int r;

        assert(dev);

        /* remove/update symlinks, remove symlinks from name index */
        FOREACH_DEVICE_DEVLINK(dev, devlink)
                (void) link_update(dev, devlink, false);

        r = xsprintf_dev_num_path_from_sd_device(dev, &filename);
        if (r < 0)
                return log_device_debug_errno(dev, r, "Failed to get device path: %m");

        /* remove /dev/{block,char}/$major:$minor */
        (void) unlink(filename);

        return 0;
}
