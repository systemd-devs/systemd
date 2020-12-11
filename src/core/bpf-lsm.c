/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include "bpf-lsm.h"
#include "bpf-lsm.h"

#include <fcntl.h>
#include <linux/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "alloc-util.h"
#include "fileio.h"
#include "cgroup-util.h"
#include "filesystems-gperf.h"
#include "log.h"
#include "manager.h"
#include "mkdir.h"
#include "nulstr-util.h"
#include "stat-util.h"
#include "fd-util.h"
#include "strv.h"

#if BPF_FRAMEWORK
/* libbpf, clang and llc compile time dependencies are satisfied */
#include "bpf-dlopen.h"
#include "bpf-link.h"
#include "bpf/restrict_fs/restrict-fs-skel.h"

static struct restrict_fs_bpf *restrict_fs_bpf_free(struct restrict_fs_bpf *obj) {
        /* restrict_fs_bpf__destroy handles object == NULL case */
        (void) restrict_fs_bpf__destroy(obj);

        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(struct restrict_fs_bpf *, restrict_fs_bpf_free);

static bool bpf_can_link_lsm_program(struct bpf_program *prog) {
        _cleanup_(bpf_link_freep) struct bpf_link *link = NULL;

        assert(prog);

        link = sym_bpf_program__attach_lsm(prog);
        if (!link)
                return -ENOMEM;

        return 1;
}

static int prepare_restrict_fs_bpf(struct restrict_fs_bpf **ret_obj) {
        struct restrict_fs_bpf *obj = 0;
        _cleanup_close_ int inner_map_fd = -1;
        int r;

        assert(ret_obj);

        obj = restrict_fs_bpf__open();
        if (!obj)
                return log_error_errno(errno, "Failed to open BPF object: %m");

        /* TODO Maybe choose a number based on runtime information? */
        r = sym_bpf_map__resize(obj->maps.cgroup_hash, 2048);
        if (r != 0)
                return log_error_errno(r,
                                "Failed to resize BPF map '%s': %m",
                                sym_bpf_map__name(obj->maps.cgroup_hash));

        /* Dummy map to satisfy the verifier */
        inner_map_fd = sym_bpf_create_map(BPF_MAP_TYPE_HASH, sizeof(uint32_t), sizeof(uint32_t), 128, 0);
        if (inner_map_fd < 0)
                return log_error_errno(errno, "Failed to create BPF map: %m");

        r = sym_bpf_map__set_inner_map_fd(obj->maps.cgroup_hash, inner_map_fd);
        if (r < 0)
                return log_error_errno(r, "Failed to set inner map fd: %m");

        r = restrict_fs_bpf__load(obj);
        if (r)
                return log_error_errno(r, "Failed to load BPF object");

        /* Close dummy map */
        inner_map_fd = safe_close(inner_map_fd);

        *ret_obj = TAKE_PTR(obj);

        return 0;
}

static int mac_bpf_use(void) {
        _cleanup_free_ char *lsm_list = NULL;
        static int cached_use = -1;
        int r;

        if (cached_use < 0) {
                cached_use = 0;

                r = read_one_line_file("/sys/kernel/security/lsm", &lsm_list);
                if (r < 0) {
                       if (errno != ENOENT)
                               log_debug_errno(r, "Failed to read /sys/kernel/security/lsm: %m");

                       return 0;
                }

                const char *p = lsm_list;

                for (;;) {
                        _cleanup_free_ char *word = NULL;

                        r = extract_first_word(&p, &word, ",", 0);
                        if (r == 0)
                                break;
                        if (r == -ENOMEM)
                                return log_oom();
                        if (r < 0)
                                return 0;

                        if (strstr(word, "bpf")) {
                                cached_use = 1;
                                break;
                        }
                }
        }

        return cached_use;
}

int lsm_bpf_supported(void) {
        _cleanup_(restrict_fs_bpf_freep) struct restrict_fs_bpf *obj = NULL;
        _cleanup_close_ int inner_map_fd = -1;
        static int supported = -1;
        int r;

        if (supported >= 0)
                return supported;

        if (dlopen_bpf() < 0) {
                log_info_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Failed to open libbpf, LSM BPF is not supported");
                return supported = 0;
        }

        r = cg_unified_controller(SYSTEMD_CGROUP_CONTROLLER);
        if (r < 0) {
                log_info_errno(r, "Can't determine whether the unified hierarchy is used: %m");
                return supported = 0;
        }

        if (r == 0) {
                log_info_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                         "Not running with unified cgroup hierarchy, LSM BPF is not supported");
                return supported = 0;
        }

        r = mac_bpf_use();
        if (r < 0) {
                log_info_errno(r, "Can't determine whether the BPF LSM module is used: %m");
                return supported = 0;
        }

        if (r == 0) {
                log_info_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                         "BPF LSM hook not enabled in the kernel, LSM BPF not supported");
                return supported = 0;
        }

        r = prepare_restrict_fs_bpf(&obj);
        if (r < 0)
                return log_info_errno(r, "Failed to load BPF object: %m");

        return bpf_can_link_lsm_program(obj->progs.restrict_filesystems);
}

int lsm_bpf_setup(Manager *m) {
        struct restrict_fs_bpf *obj = NULL;
        _cleanup_(bpf_link_freep) struct bpf_link *link = NULL;
        int r;

        r = prepare_restrict_fs_bpf(&obj);
        if (r < 0)
                return log_error_errno(r, "Failed to load BPF object: %m");

        m->restrict_fs = obj;

        link = sym_bpf_program__attach_lsm(m->restrict_fs->progs.restrict_filesystems);
        r = sym_libbpf_get_error(link);
        if (r != 0)
                return log_error_errno(r, "Failed to link '%s' LSM BPF program: %m",
                                       sym_bpf_program__name(m->restrict_fs->progs.restrict_filesystems));

        log_info("LSM BPF program attached");

        m->restrict_fs->links.restrict_filesystems = TAKE_PTR(link);

        return 0;
}

int bpf_restrict_filesystems(const Set *filesystems, const bool allow_list, Unit *u) {
        int inner_map_fd = -1, outer_map_fd = -1;
        _cleanup_free_ char *path = NULL;
        uint64_t cgroup_id;
        uint32_t dummy_value = 1, zero = 0;
        const char *fs;
        statfs_f_type_t *magic;
        int r;

        assert(filesystems);
        assert(u);

        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, NULL, &path);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get systemd cgroup path: %m");

        r = cg_path_get_cgroupid(path, &cgroup_id);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get cgroup ID for path '%s': %m", path);

        inner_map_fd = sym_bpf_create_map(
            BPF_MAP_TYPE_HASH,
            sizeof(uint32_t),
            sizeof(uint32_t),
            128, /* Should be enough for all filesystem types */
            0);
        if (inner_map_fd < 0)
                return log_unit_error_errno(u, errno, "Failed to create inner LSM map: %m");

        outer_map_fd = sym_bpf_map__fd(u->manager->restrict_fs->maps.cgroup_hash);
        if (outer_map_fd < 0)
                return log_unit_error_errno(u, errno, "Failed to get BPF map fd: %m");

        if (sym_bpf_map_update_elem(outer_map_fd, &cgroup_id, &inner_map_fd, BPF_ANY) != 0)
                return log_unit_error_errno(u, errno, "Error populating LSM BPF map: %m");

        uint32_t allow = allow_list;

        /* Use key 0 to store whether this is an allow list or a deny list */
        if (sym_bpf_map_update_elem(inner_map_fd, &zero, &allow, BPF_ANY) != 0)
                return log_unit_error_errno(u, errno, "Error initializing BPF map: %m");

        SET_FOREACH(fs, filesystems) {
                int i;

                r = fs_type_from_string(fs, &magic);
                if (r < 0) {
                        log_unit_warning(u, "Invalid filesystem name '%s', ignoring.", fs);
                        continue;
                }

                log_unit_debug(u, "Restricting filesystem access to '%s'", fs);

                for (i=0; i<FILESYSTEM_MAGIC_MAX; i++) {
                        if (magic[i] == 0)
                                break;

                        if (sym_bpf_map_update_elem(inner_map_fd, &magic[i], &dummy_value, BPF_ANY) != 0) {
                                r = log_unit_error_errno(u, errno, "Failed to update BPF map: %m");

                                if (sym_bpf_map_delete_elem(outer_map_fd, &cgroup_id) != 0)
                                        log_unit_debug_errno(u, errno, "Failed to delete cgroup entry from LSM BPF map: %m");

                                return r;
                        }
                }
        }

        return 0;
}

int cleanup_lsm_bpf(const Unit *u) {
        _cleanup_free_ char *path = NULL;
        uint64_t cgroup_id;
        int fd = -1;
        int r;

        assert(u);

        if (!lsm_bpf_supported())
                return 0;

        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, NULL, &path);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get cgroup path: %m");

        r = cg_path_get_cgroupid(path, &cgroup_id);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get cgroup ID: %m");

        fd = sym_bpf_map__fd(u->manager->restrict_fs->maps.cgroup_hash);
        if (fd < 0)
                return log_unit_error_errno(u, errno, "Failed to get BPF map fd: %m");

        if (sym_bpf_map_delete_elem(fd, &cgroup_id) != 0)
                return log_unit_debug_errno(u, errno, "Failed to delete cgroup entry from LSM BPF map: %m");

        return 0;
}

int bpf_map_restrict_fs_fd(Unit *unit) {
        assert(unit);

        if (!unit->manager->restrict_fs)
                return -1;

        return sym_bpf_map__fd(unit->manager->restrict_fs->maps.cgroup_hash);
}

void lsm_bpf_destroy(struct restrict_fs_bpf *prog) {
        restrict_fs_bpf__destroy(prog);
}
#else /* ! BPF_FRAMEWORK */
int lsm_bpf_supported(void) {
        return 0;
}

int lsm_bpf_setup(Manager *m) {
        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Failed to set up LSM BPF: %m");
}

int bpf_restrict_filesystems(const Set *filesystems, const bool allow_list, Unit *u) {
        return log_unit_debug_errno(u, SYNTHETIC_ERRNO(EOPNOTSUPP), "Failed to restrict filesystems using LSM BPF: %m");
}

int cleanup_lsm_bpf(const Unit *u) {
        return 0;
}

int bpf_map_restrict_fs_fd(Unit *unit) {
        return -1;
}

void lsm_bpf_destroy(struct restrict_fs_bpf *prog) {
        return;
}
#endif
