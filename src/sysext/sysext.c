/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <getopt.h>
#include <linux/loop.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <unistd.h>

#include "sd-bus.h"

#include "build.h"
#include "bus-locator.h"
#include "bus-error.h"
#include "bus-unit-util.h"
#include "bus-util.h"
#include "capability-util.h"
#include "chase.h"
#include "constants.h"
#include "devnum-util.h"
#include "discover-image.h"
#include "dissect-image.h"
#include "env-util.h"
#include "escape.h"
#include "extension-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-table.h"
#include "fs-util.h"
#include "hashmap.h"
#include "initrd-util.h"
#include "log.h"
#include "main-func.h"
#include "missing_magic.h"
#include "mkdir.h"
#include "mount-util.h"
#include "mountpoint-util.h"
#include "os-util.h"
#include "pager.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "path-util.h"
#include "pretty-print.h"
#include "process-util.h"
#include "rm-rf.h"
#include "sort-util.h"
#include "string-util.h"
#include "terminal-util.h"
#include "user-util.h"
#include "varlink.h"
#include "varlink-io.systemd.sysext.h"
#include "verbs.h"

typedef enum MutableMode {
        MUTABLE_YES,
        MUTABLE_NO,
        MUTABLE_AUTO,
        MUTABLE_IMPORT,
        MUTABLE_EPHEMERAL,
        MUTABLE_EPHEMERAL_IMPORT,
        _MUTABLE_MAX,
        _MUTABLE_INVALID = -EINVAL,
} MutableMode;

static char **arg_hierarchies = NULL; /* "/usr" + "/opt" by default for sysext and /etc by default for confext */
static char *arg_root = NULL;
static JsonFormatFlags arg_json_format_flags = JSON_FORMAT_OFF;
static PagerFlags arg_pager_flags = 0;
static bool arg_legend = true;
static bool arg_force = false;
static bool arg_no_reload = false;
static int arg_noexec = -1;
static ImagePolicy *arg_image_policy = NULL;
static bool arg_varlink = false;
static MutableMode arg_mutable = MUTABLE_NO;

/* Is set to IMAGE_CONFEXT when systemd is called with the confext functionality instead of the default */
static ImageClass arg_image_class = IMAGE_SYSEXT;

static const char *mutable_extensions_base_dir = "/var/lib/extensions.mutable";

STATIC_DESTRUCTOR_REGISTER(arg_hierarchies, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_root, freep);
STATIC_DESTRUCTOR_REGISTER(arg_image_policy, image_policy_freep);

/* Helper struct for naming simplicity and reusability */
static const struct {
        const char *full_identifier;
        const char *short_identifier;
        const char *short_identifier_plural;
        const char *blurb;
        const char *dot_directory_name;
        const char *directory_name;
        const char *level_env;
        const char *scope_env;
        const char *name_env;
        const char *mode_env;
        const ImagePolicy *default_image_policy;
        unsigned long default_mount_flags;
} image_class_info[_IMAGE_CLASS_MAX] = {
        [IMAGE_SYSEXT] = {
                .full_identifier = "systemd-sysext",
                .short_identifier = "sysext",
                .short_identifier_plural = "extensions",
                .blurb = "Merge system extension images into /usr/ and /opt/.",
                .dot_directory_name = ".systemd-sysext",
                .level_env = "SYSEXT_LEVEL",
                .scope_env = "SYSEXT_SCOPE",
                .name_env = "SYSTEMD_SYSEXT_HIERARCHIES",
                .mode_env = "SYSTEMD_SYSEXT_MUTABLE_MODE",
                .default_image_policy = &image_policy_sysext,
                .default_mount_flags = MS_RDONLY|MS_NODEV,
        },
        [IMAGE_CONFEXT] = {
                .full_identifier = "systemd-confext",
                .short_identifier = "confext",
                .short_identifier_plural = "confexts",
                .blurb = "Merge configuration extension images into /etc/.",
                .dot_directory_name = ".systemd-confext",
                .level_env = "CONFEXT_LEVEL",
                .scope_env = "CONFEXT_SCOPE",
                .name_env = "SYSTEMD_CONFEXT_HIERARCHIES",
                .mode_env = "SYSTEMD_CONFEXT_MUTABLE_MODE",
                .default_image_policy = &image_policy_confext,
                .default_mount_flags = MS_RDONLY|MS_NODEV|MS_NOSUID|MS_NOEXEC,
        }
};

static int parse_mutable_mode(const char *p) {
        int r;

        assert(p);

        if (streq(p, "auto"))
                return MUTABLE_AUTO;

        if (streq(p, "import"))
                return MUTABLE_IMPORT;

        if (streq(p, "ephemeral"))
                return MUTABLE_EPHEMERAL;

        if (streq(p, "ephemeral-import"))
                return MUTABLE_EPHEMERAL_IMPORT;

        r = parse_boolean(p);
        if (r < 0)
                return r;

        return r ? MUTABLE_YES : MUTABLE_NO;
}

static int is_our_mount_point(
                ImageClass image_class,
                const char *p) {

        _cleanup_free_ char *buf = NULL, *f = NULL;
        struct stat st;
        dev_t dev;
        int r;

        assert(p);

        r = path_is_mount_point(p);
        if (r == -ENOENT) {
                log_debug_errno(r, "Hierarchy '%s' doesn't exist.", p);
                return false;
        }
        if (r < 0)
                return log_error_errno(r, "Failed to determine whether '%s' is a mount point: %m", p);
        if (r == 0) {
                log_debug("Hierarchy '%s' is not a mount point, skipping.", p);
                return false;
        }

        /* So we know now that it's a mount point. Now let's check if it's one of ours, so that we don't
         * accidentally unmount the user's own /usr/ but just the mounts we established ourselves. We do this
         * check by looking into the metadata directory we place in merged mounts: if the file
         * ../dev contains the major/minor device pair of the mount we have a good reason to
         * believe this is one of our mounts. This thorough check has the benefit that we aren't easily
         * confused if people tar up one of our merged trees and untar them elsewhere where we might mistake
         * them for a live sysext tree. */

        f = path_join(p, image_class_info[image_class].dot_directory_name, "dev");
        if (!f)
                return log_oom();

        r = read_one_line_file(f, &buf);
        if (r == -ENOENT) {
                log_debug("Hierarchy '%s' does not carry a %s/dev file, not a merged tree.", p, image_class_info[image_class].dot_directory_name);
                return false;
        }
        if (r < 0)
                return log_error_errno(r, "Failed to determine whether hierarchy '%s' contains '%s/dev': %m", p, image_class_info[image_class].dot_directory_name);

        r = parse_devnum(buf, &dev);
        if (r < 0)
                return log_error_errno(r, "Failed to parse device major/minor stored in '%s/dev' file on '%s': %m", image_class_info[image_class].dot_directory_name, p);

        if (lstat(p, &st) < 0)
                return log_error_errno(errno, "Failed to stat %s: %m", p);

        if (st.st_dev != dev) {
                log_debug("Hierarchy '%s' reports a different device major/minor than what we are seeing, assuming offline copy.", p);
                return false;
        }

        return true;
}

static int need_reload(
                ImageClass image_class,
                char **hierarchies,
                bool no_reload) {

        /* Parse the mounted images to find out if we need to reload the daemon. */
        int r;

        if (no_reload)
                return false;

        STRV_FOREACH(p, hierarchies) {
                _cleanup_free_ char *f = NULL, *buf = NULL, *resolved = NULL;
                _cleanup_strv_free_ char **mounted_extensions = NULL;

                r = chase(*p, arg_root, CHASE_PREFIX_ROOT, &resolved, NULL);
                if (r == -ENOENT) {
                        log_debug_errno(r, "Hierarchy '%s%s' does not exist, ignoring.", strempty(arg_root), *p);
                        continue;
                }
                if (r < 0) {
                        log_warning_errno(r, "Failed to resolve path to hierarchy '%s%s': %m, ignoring.", strempty(arg_root), *p);
                        continue;
                }

                r = is_our_mount_point(image_class, resolved);
                if (r < 0)
                        return r;
                if (!r)
                        continue;

                f = path_join(resolved, image_class_info[image_class].dot_directory_name, image_class_info[image_class].short_identifier_plural);
                if (!f)
                        return log_oom();

                r = read_full_file(f, &buf, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to open '%s': %m", f);

                mounted_extensions = strv_split_newlines(buf);
                if (!mounted_extensions)
                        return log_oom();

                STRV_FOREACH(extension, mounted_extensions) {
                        _cleanup_strv_free_ char **extension_release = NULL;
                        const char *extension_reload_manager = NULL;
                        int b;

                        r = load_extension_release_pairs(arg_root, image_class, *extension, /* relax_extension_release_check */ true, &extension_release);
                        if (r < 0) {
                                log_debug_errno(r, "Failed to parse extension-release metadata of %s, ignoring: %m", *extension);
                                continue;
                        }

                        extension_reload_manager = strv_env_pairs_get(extension_release, "EXTENSION_RELOAD_MANAGER");
                        if (isempty(extension_reload_manager))
                                continue;

                        b = parse_boolean(extension_reload_manager);
                        if (b < 0) {
                                log_warning_errno(b, "Failed to parse the extension metadata to know if the manager needs to be reloaded, ignoring: %m");
                                continue;
                        }

                        if (b)
                                /* If at least one extension wants a reload, we reload. */
                                return true;
                }
        }

        return false;
}

static int daemon_reload(void) {
         _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        int r;

        r = bus_connect_system_systemd(&bus);
        if (r < 0)
                return log_error_errno(r, "Failed to get D-Bus connection: %m");

        return bus_service_manager_reload(bus);
}

static int unmerge_hierarchy(
                ImageClass image_class,
                const char *p) {

        _cleanup_free_ char *dot_dir = NULL, *work_dir_info_file = NULL;
        int r;

        assert(p);

        dot_dir = path_join(p, image_class_info[image_class].dot_directory_name);
        if (!dot_dir)
                return log_oom();

        work_dir_info_file = path_join(dot_dir, "work_dir");
        if (!work_dir_info_file)
                return log_oom();

        for (;;) {
                _cleanup_free_ char *escaped_work_dir_in_root = NULL, *work_dir = NULL;

                /* We only unmount /usr/ if it is a mount point and really one of ours, in order not to break
                 * systems where /usr/ is a mount point of its own already. */

                r = is_our_mount_point(image_class, p);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = read_one_line_file(work_dir_info_file, &escaped_work_dir_in_root);
                if (r < 0) {
                        if (r != -ENOENT)
                                return log_error_errno(r, "Failed to read '%s': %m", work_dir_info_file);
                } else {
                        _cleanup_free_ char *work_dir_in_root = NULL;
                        ssize_t l;

                        l = cunescape_length(escaped_work_dir_in_root, r, 0, &work_dir_in_root);
                        if (l < 0)
                                return log_error_errno(l, "Failed to unescape work directory path: %m");
                        work_dir = path_join(arg_root, work_dir_in_root);
                        if (!work_dir)
                                return log_oom();
                }

                r = umount_verbose(LOG_DEBUG, dot_dir, MNT_DETACH|UMOUNT_NOFOLLOW);
                if (r < 0) {
                        /* EINVAL is possibly "not a mount point". Let it slide as it's expected to occur if
                         * the whole hierarchy was read-only, so the dot directory inside it was not
                         * bind-mounted as read-only. */
                        if (r != -EINVAL)
                                return log_error_errno(r, "Failed to unmount '%s': %m", dot_dir);
                }

                r = umount_verbose(LOG_ERR, p, MNT_DETACH|UMOUNT_NOFOLLOW);
                if (r < 0)
                        return r;

                if (work_dir) {
                        r = rm_rf(work_dir, REMOVE_ROOT | REMOVE_MISSING_OK | REMOVE_PHYSICAL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to remove '%s': %m", work_dir);
                }

                log_info("Unmerged '%s'.", p);
        }

        return 0;
}

static int unmerge(
                ImageClass image_class,
                char **hierarchies,
                bool no_reload) {

        int r, ret = 0;
        bool need_to_reload;

        r = need_reload(image_class, hierarchies, no_reload);
        if (r < 0)
                return r;
        need_to_reload = r > 0;

        STRV_FOREACH(p, hierarchies) {
                _cleanup_free_ char *resolved = NULL;

                r = chase(*p, arg_root, CHASE_PREFIX_ROOT, &resolved, NULL);
                if (r == -ENOENT) {
                        log_debug_errno(r, "Hierarchy '%s%s' does not exist, ignoring.", strempty(arg_root), *p);
                        continue;
                }
                if (r < 0) {
                        log_error_errno(r, "Failed to resolve path to hierarchy '%s%s': %m", strempty(arg_root), *p);
                        if (ret == 0)
                                ret = r;

                        continue;
                }

                r = unmerge_hierarchy(image_class, resolved);
                if (r < 0 && ret == 0)
                        ret = r;
        }

        if (need_to_reload) {
                r = daemon_reload();
                if (r < 0)
                        return r;
        }

        return ret;
}

static int verb_unmerge(int argc, char **argv, void *userdata) {
        int r;

        r = have_effective_cap(CAP_SYS_ADMIN);
        if (r < 0)
                return log_error_errno(r, "Failed to check if we have enough privileges: %m");
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EPERM), "Need to be privileged.");

        return unmerge(arg_image_class,
                       arg_hierarchies,
                       arg_no_reload);
}

static int parse_image_class_parameter(Varlink *link, const char *value, ImageClass *image_class, char ***hierarchies) {
        _cleanup_strv_free_ char **h = NULL;
        ImageClass c;
        int r;

        assert(link);
        assert(image_class);

        if (!value)
                return 0;

        c = image_class_from_string(value);
        if (!IN_SET(c, IMAGE_SYSEXT, IMAGE_CONFEXT))
                return varlink_error_invalid_parameter_name(link, "class");

        if (hierarchies) {
                r = parse_env_extension_hierarchies(&h, image_class_info[c].name_env);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse environment variable: %m");

                strv_free_and_replace(*hierarchies, h);
        }

        *image_class = c;
        return 0;
}

typedef struct MethodUnmergeParameters {
        const char *class;
        int no_reload;
} MethodUnmergeParameters;

static int vl_method_unmerge(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {

        static const JsonDispatch dispatch_table[] = {
                { "class",    JSON_VARIANT_STRING,  json_dispatch_const_string, offsetof(MethodUnmergeParameters, class),     0 },
                { "noReload", JSON_VARIANT_BOOLEAN, json_dispatch_boolean,      offsetof(MethodUnmergeParameters, no_reload), 0 },
                {}
        };
        MethodUnmergeParameters p = {
                .no_reload = -1,
        };
        _cleanup_strv_free_ char **hierarchies = NULL;
        ImageClass image_class = arg_image_class;
        int r;

        assert(link);

        r = varlink_dispatch(link, parameters, dispatch_table, &p);
        if (r != 0)
                return r;

        r = parse_image_class_parameter(link, p.class, &image_class, &hierarchies);
        if (r < 0)
                return r;

        r = unmerge(image_class,
                    hierarchies ?: arg_hierarchies,
                    p.no_reload >= 0 ? p.no_reload : arg_no_reload);
        if (r < 0)
                return r;

        return varlink_reply(link, NULL);
}

static int verb_status(int argc, char **argv, void *userdata) {
        _cleanup_(table_unrefp) Table *t = NULL;
        int r, ret = 0;

        t = table_new("hierarchy", "extensions", "since");
        if (!t)
                return log_oom();

        table_set_ersatz_string(t, TABLE_ERSATZ_DASH);

        STRV_FOREACH(p, arg_hierarchies) {
                _cleanup_free_ char *resolved = NULL, *f = NULL, *buf = NULL;
                _cleanup_strv_free_ char **l = NULL;
                struct stat st;

                r = chase(*p, arg_root, CHASE_PREFIX_ROOT, &resolved, NULL);
                if (r == -ENOENT) {
                        log_debug_errno(r, "Hierarchy '%s%s' does not exist, ignoring.", strempty(arg_root), *p);
                        continue;
                }
                if (r < 0) {
                        log_error_errno(r, "Failed to resolve path to hierarchy '%s%s': %m", strempty(arg_root), *p);
                        goto inner_fail;
                }

                r = is_our_mount_point(arg_image_class, resolved);
                if (r < 0)
                        goto inner_fail;
                if (r == 0) {
                        r = table_add_many(
                                        t,
                                        TABLE_PATH, *p,
                                        TABLE_STRING, "none",
                                        TABLE_SET_COLOR, ansi_grey(),
                                        TABLE_EMPTY);
                        if (r < 0)
                                return table_log_add_error(r);

                        continue;
                }

                f = path_join(resolved, image_class_info[arg_image_class].dot_directory_name, image_class_info[arg_image_class].short_identifier_plural);
                if (!f)
                        return log_oom();

                r = read_full_file(f, &buf, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to open '%s': %m", f);

                l = strv_split_newlines(buf);
                if (!l)
                        return log_oom();

                if (stat(*p, &st) < 0)
                        return log_error_errno(errno, "Failed to stat() '%s': %m", *p);

                r = table_add_many(
                                t,
                                TABLE_PATH, *p,
                                TABLE_STRV, l,
                                TABLE_TIMESTAMP, timespec_load(&st.st_mtim));
                if (r < 0)
                        return table_log_add_error(r);

                continue;

        inner_fail:
                if (ret == 0)
                        ret = r;
        }

        (void) table_set_sort(t, (size_t) 0);

        r = table_print_with_pager(t, arg_json_format_flags, arg_pager_flags, arg_legend);
        if (r < 0)
                return r;

        return ret;
}

static int append_overlayfs_path_option(
                char **options,
                const char *separator,
                const char *option,
                const char *path) {

        _cleanup_free_ char *escaped = NULL;

        assert(options);
        assert(separator);
        assert(path);

        escaped = shell_escape(path, ",:");
        if (!escaped)
                return log_oom();

        if (option) {
                if (!strextend(options, separator, option, "=", escaped))
                        return log_oom();
        } else if (!strextend(options, separator, escaped))
                return log_oom();

        return 0;
}

static int mount_overlayfs(
                ImageClass image_class,
                int noexec,
                const char *where,
                char **layers,
                const char *upper_dir,
                const char *work_dir) {

        _cleanup_free_ char *options = NULL;
        bool separator = false;
        unsigned long flags;
        int r;

        assert(where);
        assert((upper_dir && work_dir) || (!upper_dir && !work_dir));

        options = strdup("lowerdir=");
        if (!options)
                return log_oom();

        STRV_FOREACH(l, layers) {
                r = append_overlayfs_path_option(&options, separator ? ":" : "", NULL, *l);
                if (r < 0)
                        return r;

                separator = true;
        }

        flags = image_class_info[image_class].default_mount_flags;
        if (noexec >= 0)
                SET_FLAG(flags, MS_NOEXEC, noexec);

        if (upper_dir && work_dir) {
                r = append_overlayfs_path_option(&options, ",", "upperdir", upper_dir);
                if (r < 0)
                        return r;

                flags &= ~MS_RDONLY;

                r = append_overlayfs_path_option(&options, ",", "workdir", work_dir);
                if (r < 0)
                        return r;
                /* redirect_dir=on and noatime prevent unnecessary upcopies, metacopy=off prevents broken
                 * files from partial upcopies after umount. */
                if (!strextend(&options, ",redirect_dir=on,noatime,metacopy=off"))
                        return log_oom();
        }

        /* Now mount the actual overlayfs */
        r = mount_nofollow_verbose(LOG_ERR, image_class_info[image_class].short_identifier, where, "overlay", flags, options);
        if (r < 0)
                return r;

        return 0;
}

static char *hierarchy_as_single_path_component(const char *hierarchy) {
        /* We normally expect hierarchy to be /usr, /opt or /etc, but for debugging purposes the hierarchy
         * could very well be like /foo/bar/baz/. So for a given hierarchy we generate a directory name by
         * stripping the leading and trailing separators and replacing the rest of separators with dots. This
         * makes the generated name to be the same for /foo/bar/baz and for /foo/bar.baz, but, again,
         * specifying a different hierarchy is a debugging feature, so non-unique mapping should not be an
         * issue in general case. */
        const char *stripped = hierarchy;
        _cleanup_free_ char *dir_name = NULL;

        assert(hierarchy);

        stripped += strspn(stripped, "/");

        dir_name = strdup(stripped);
        if (!dir_name)
                return NULL;
        delete_trailing_chars(dir_name, "/");
        string_replace_char(dir_name, '/', '.');
        return TAKE_PTR(dir_name);
}

static int paths_on_same_fs(const char *path1, const char *path2) {
        struct stat st1, st2;

        assert(path1);
        assert(path2);

        if (stat(path1, &st1) < 0)
                return log_error_errno(errno, "Failed to stat '%s': %m", path1);

        if (stat(path2, &st2) < 0)
                return log_error_errno(errno, "Failed to stat '%s': %m", path2);

        return st1.st_dev == st2.st_dev;
}

static int work_dir_for_hierarchy(
                const char *hierarchy,
                const char *resolved_upper_dir,
                char **ret_work_dir) {

        _cleanup_free_ char *parent = NULL;
        int r;

        assert(hierarchy);
        assert(resolved_upper_dir);
        assert(ret_work_dir);

        r = path_extract_directory(resolved_upper_dir, &parent);
        if (r < 0)
                return log_error_errno(r, "Failed to get parent directory of upperdir '%s': %m", resolved_upper_dir);

        /* TODO: paths_in_same_superblock? partition? device? */
        r = paths_on_same_fs(resolved_upper_dir, parent);
        if (r < 0)
                return r;
        if (!r)
                return log_error_errno(SYNTHETIC_ERRNO(EXDEV), "Unable to find a suitable workdir location for upperdir '%s' for host hierarchy '%s' - parent directory of the upperdir is in a different filesystem", resolved_upper_dir, hierarchy);

        _cleanup_free_ char *f = NULL, *dir_name = NULL;

        f = hierarchy_as_single_path_component(hierarchy);
        if (!f)
                return log_oom();
        dir_name = strjoin(".systemd-", f, "-workdir");
        if (!dir_name)
                return log_oom();

        free(f);
        f = path_join(parent, dir_name);
        if (!f)
                return log_oom();

        *ret_work_dir = TAKE_PTR(f);
        return 0;
}

typedef struct OverlayFSPaths {
        char *hierarchy;
        mode_t hierarchy_mode;
        char *resolved_hierarchy;
        char *resolved_mutable_directory;

        /* NULL if merged fs is read-only */
        char *upper_dir;
        /* NULL if merged fs is read-only */
        char *work_dir;
        /* lowest index is top lowerdir, highest index is bottom lowerdir */
        char **lower_dirs;
} OverlayFSPaths;

static OverlayFSPaths *overlayfs_paths_free(OverlayFSPaths *op) {
        if (!op)
                return NULL;

        free(op->hierarchy);
        free(op->resolved_hierarchy);
        free(op->resolved_mutable_directory);

        free(op->upper_dir);
        free(op->work_dir);
        strv_free(op->lower_dirs);

        return mfree(op);
}
DEFINE_TRIVIAL_CLEANUP_FUNC(OverlayFSPaths *, overlayfs_paths_free);

static int resolve_hierarchy(const char *hierarchy, char **ret_resolved_hierarchy) {
        _cleanup_free_ char *resolved_path = NULL;
        int r;

        assert(hierarchy);
        assert(ret_resolved_hierarchy);

        r = chase(hierarchy, arg_root, CHASE_PREFIX_ROOT, &resolved_path, NULL);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Failed to resolve hierarchy '%s': %m", hierarchy);

        *ret_resolved_hierarchy = TAKE_PTR(resolved_path);
        return 0;
}

static int mutable_directory_mode_matches_hierarchy(
                const char *root_or_null,
                const char *path,
                mode_t hierarchy_mode) {

        _cleanup_free_ char *path_in_root = NULL;
        struct stat st;
        mode_t actual_mode;

        assert(path);

        path_in_root = path_join(root_or_null, path);
        if (!path_in_root)
                return log_oom();

        if (stat(path_in_root, &st) < 0) {
                if (errno == ENOENT)
                        return 0;
                return log_error_errno(errno, "Failed to stat mutable directory '%s': %m", path_in_root);
        }

        actual_mode = st.st_mode & 0777;
        if (actual_mode != hierarchy_mode) {
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Mutable directory '%s' has mode %04o, ought to have mode %04o", path_in_root, actual_mode, hierarchy_mode);
        }

        return 0;
}

static int resolve_mutable_directory(
                const char *hierarchy,
                mode_t hierarchy_mode,
                const char *workspace,
                char **ret_resolved_mutable_directory) {

        _cleanup_free_ char *path = NULL, *resolved_path = NULL, *dir_name = NULL;
        const char *root = arg_root, *base = mutable_extensions_base_dir;
        int r;

        assert(hierarchy);
        assert(ret_resolved_mutable_directory);

        if (arg_mutable == MUTABLE_NO) {
                log_debug("Mutability for hierarchy '%s' is disabled, not resolving mutable directory.", hierarchy);
                *ret_resolved_mutable_directory = NULL;
                return 0;
        }

        if (IN_SET(arg_mutable, MUTABLE_EPHEMERAL, MUTABLE_EPHEMERAL_IMPORT)) {
                /* We create mutable directory inside the temporary tmpfs workspace, which is a fixed
                 * location that ignores arg_root. */
                root = NULL;
                base = workspace;
        }

        dir_name = hierarchy_as_single_path_component(hierarchy);
        if (!dir_name)
                return log_oom();

        path = path_join(base, dir_name);
        if (!path)
                return log_oom();

        if (IN_SET(arg_mutable, MUTABLE_YES, MUTABLE_AUTO)) {
                /* If there already is a mutable directory, check if its mode matches hierarchy. Merged
                 * hierarchy will have the same mode as the mutable directory, so we want no surprising mode
                 * changes here. */
                r = mutable_directory_mode_matches_hierarchy(root, path, hierarchy_mode);
                if (r < 0)
                        return r;
        }

        if (IN_SET(arg_mutable, MUTABLE_YES, MUTABLE_EPHEMERAL, MUTABLE_EPHEMERAL_IMPORT)) {
                _cleanup_free_ char *path_in_root = NULL;

                path_in_root = path_join(root, path);
                if (!path_in_root)
                        return log_oom();

                r = mkdir_p(path_in_root, 0700);
                if (r < 0)
                        return log_error_errno(r, "Failed to create a directory '%s': %m", path_in_root);
        }

        r = chase(path, root, CHASE_PREFIX_ROOT, &resolved_path, NULL);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Failed to resolve mutable directory '%s': %m", path);

        *ret_resolved_mutable_directory = TAKE_PTR(resolved_path);
        return 0;
}

static int overlayfs_paths_new(const char *hierarchy, const char *workspace_path, OverlayFSPaths **ret_op) {
        _cleanup_free_ char *hierarchy_copy = NULL, *resolved_hierarchy = NULL, *resolved_mutable_directory = NULL;
        mode_t hierarchy_mode;

        int r;

        assert (hierarchy);
        assert (ret_op);

        hierarchy_copy = strdup(hierarchy);
        if (!hierarchy_copy)
                return log_oom();

        r = resolve_hierarchy(hierarchy, &resolved_hierarchy);
        if (r < 0)
                return r;

        if (resolved_hierarchy) {
                struct stat st;

                if (stat(resolved_hierarchy, &st) < 0)
                        return log_error_errno(errno, "Failed to stat '%s': %m", resolved_hierarchy);
                hierarchy_mode = st.st_mode & 0777;
        } else
                hierarchy_mode = 0755;

        r = resolve_mutable_directory(hierarchy, hierarchy_mode, workspace_path, &resolved_mutable_directory);
        if (r < 0)
                return r;

        OverlayFSPaths *op;
        op = new(OverlayFSPaths, 1);
        if (!op)
                return log_oom();

        *op = (OverlayFSPaths) {
                .hierarchy = TAKE_PTR(hierarchy_copy),
                .hierarchy_mode = hierarchy_mode,
                .resolved_hierarchy = TAKE_PTR(resolved_hierarchy),
                .resolved_mutable_directory = TAKE_PTR(resolved_mutable_directory),
        };

        *ret_op = TAKE_PTR(op);
        return 0;
}

static int resolved_paths_equal(const char *resolved_a, const char *resolved_b) {
        /* Returns true if paths are of the same entry, false if not, <0 on error. */

        if (path_equal(resolved_a, resolved_b))
                return 1;

        if (!resolved_a || !resolved_b)
                return 0;

        return inode_same(resolved_a, resolved_b, 0);
}

static int maybe_import_mutable_directory(OverlayFSPaths *op) {
        int r;

        assert(op);

        /* If importing mutable layer and it actually exists and is not a hierarchy itself, add it just below
         * the meta path */

        if (arg_mutable != MUTABLE_IMPORT || !op->resolved_mutable_directory)
                return 0;

        r = resolved_paths_equal(op->resolved_hierarchy, op->resolved_mutable_directory);
        if (r < 0)
                return log_error_errno(r, "Failed to check equality of hierarchy %s and its mutable directory %s: %m", op->resolved_hierarchy, op->resolved_mutable_directory);
        if (r > 0)
                return log_error_errno(SYNTHETIC_ERRNO(ELOOP), "Not importing mutable directory for hierarchy %s as a lower dir, because it points to the hierarchy itself", op->hierarchy);

        r = strv_extend(&op->lower_dirs, op->resolved_mutable_directory);
        if (r < 0)
                return log_oom();

        return 0;
}

static int maybe_import_ignored_mutable_directory(OverlayFSPaths *op) {
        _cleanup_free_ char *dir_name = NULL, *path = NULL, *resolved_path = NULL;
        int r;

        assert(op);

        /* If importing the ignored mutable layer and it actually exists and is not a hierarchy itself, add
         * it just below the meta path */
        if (arg_mutable != MUTABLE_EPHEMERAL_IMPORT)
                return 0;

        dir_name = hierarchy_as_single_path_component(op->hierarchy);
        if (!dir_name)
                return log_oom();

        path = path_join(mutable_extensions_base_dir, dir_name);
        if (!path)
                return log_oom();

        r = chase(path, arg_root, CHASE_PREFIX_ROOT, &resolved_path, NULL);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Failed to resolve mutable directory '%s': %m", path);

        r = resolved_paths_equal(op->resolved_hierarchy, resolved_path);
        if (r < 0)
                return log_error_errno(r, "Failed to check equality of hierarchy %s and its mutable directory %s: %m", op->resolved_hierarchy, op->resolved_mutable_directory);

        if (r > 0)
                return log_error_errno(SYNTHETIC_ERRNO(ELOOP), "Not importing mutable directory for hierarchy %s as a lower dir, because it points to the hierarchy itself", op->hierarchy);

        r = strv_consume(&op->lower_dirs, TAKE_PTR(resolved_path));
        if (r < 0)
                return log_oom();

        return 0;
}

static int determine_top_lower_dirs(OverlayFSPaths *op, const char *meta_path) {
        int r;

        assert(op);
        assert(meta_path);

        /* Put the meta path (i.e. our synthesized stuff) at the top of the layer stack */
        r = strv_extend(&op->lower_dirs, meta_path);
        if (r < 0)
                return log_oom();

        r = maybe_import_mutable_directory(op);
        if (r < 0)
                return r;

        r = maybe_import_ignored_mutable_directory(op);
        if (r < 0)
                return r;

        return 0;
}

static int determine_middle_lower_dirs(OverlayFSPaths *op, char **paths, size_t *ret_extensions_used) {
        size_t n = 0;
        int r;

        assert(op);
        assert(paths);
        assert(ret_extensions_used);

        /* Put the extensions in the middle */
        STRV_FOREACH(p, paths) {
                _cleanup_free_ char *resolved = NULL;

                r = chase(op->hierarchy, *p, CHASE_PREFIX_ROOT, &resolved, NULL);
                if (r == -ENOENT) {
                        log_debug_errno(r, "Hierarchy '%s' in extension '%s' doesn't exist, not merging.", op->hierarchy, *p);
                        continue;
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve hierarchy '%s' in extension '%s': %m", op->hierarchy, *p);

                r = dir_is_empty(resolved, /* ignore_hidden_or_backup= */ false);
                if (r < 0)
                        return log_error_errno(r, "Failed to check if hierarchy '%s' in extension '%s' is empty: %m", resolved, *p);
                if (r > 0) {
                        log_debug("Hierarchy '%s' in extension '%s' is empty, not merging.", op->hierarchy, *p);
                        continue;
                }

                r = strv_consume(&op->lower_dirs, TAKE_PTR(resolved));
                if (r < 0)
                        return log_oom();
                ++n;
        }

        *ret_extensions_used = n;
        return 0;
}

static int hierarchy_as_lower_dir(OverlayFSPaths *op) {
        int r;

        /* return 0 if hierarchy should be used as lower dir, >0, if not */

        assert(op);

        if (!op->resolved_hierarchy) {
                log_debug("Host hierarchy '%s' does not exist, will not be used as lowerdir", op->hierarchy);
                return 1;
        }

        r = dir_is_empty(op->resolved_hierarchy, /* ignore_hidden_or_backup= */ false);
        if (r < 0)
                return log_error_errno(r, "Failed to check if host hierarchy '%s' is empty: %m", op->resolved_hierarchy);
        if (r > 0) {
                log_debug("Host hierarchy '%s' is empty, will not be used as lower dir.", op->resolved_hierarchy);
                return 1;
        }

        if (arg_mutable == MUTABLE_IMPORT) {
                log_debug("Mutability for host hierarchy '%s' is disabled, so host hierarchy will be a lowerdir", op->resolved_hierarchy);
                return 0;
        }

        if (arg_mutable == MUTABLE_EPHEMERAL_IMPORT) {
                log_debug("Mutability for host hierarchy '%s' is ephemeral, so host hierarchy will be a lowerdir", op->resolved_hierarchy);
                return 0;
        }

        if (!op->resolved_mutable_directory) {
                log_debug("No mutable directory found, so host hierarchy '%s' will be used as lowerdir", op->resolved_hierarchy);
                return 0;
        }

        r = resolved_paths_equal(op->resolved_hierarchy, op->resolved_mutable_directory);
        if (r < 0)
                return log_error_errno(r, "Failed to check equality of hierarchy %s and its mutable directory %s: %m", op->resolved_hierarchy, op->resolved_mutable_directory);
        if (r > 0) {
                log_debug("Host hierarchy '%s' will serve as upperdir.", op->resolved_hierarchy);
                return 1;
        }

        return 0;
}

static int determine_bottom_lower_dirs(OverlayFSPaths *op) {
        int r;

        assert(op);

        r = hierarchy_as_lower_dir(op);
        if (r < 0)
                return r;
        if (!r) {
                r = strv_extend(&op->lower_dirs, op->resolved_hierarchy);
                if (r < 0)
                        return log_oom();
        }

        return 0;
}

static int determine_lower_dirs(
                OverlayFSPaths *op,
                char **paths,
                const char *meta_path,
                size_t *ret_extensions_used) {

        int r;

        assert(op);
        assert(paths);
        assert(meta_path);
        assert(ret_extensions_used);

        r = determine_top_lower_dirs(op, meta_path);
        if (r < 0)
                return r;

        r = determine_middle_lower_dirs(op, paths, ret_extensions_used);
        if (r < 0)
                return r;

        r = determine_bottom_lower_dirs(op);
        if (r < 0)
                return r;

        return 0;
}

static int determine_upper_dir(OverlayFSPaths *op) {
        int r;

        assert(op);
        assert(!op->upper_dir);

        if (arg_mutable == MUTABLE_IMPORT) {
                log_debug("Mutability is disabled, there will be no upperdir for host hierarchy '%s'", op->hierarchy);
                return 0;
        }

        if (!op->resolved_mutable_directory) {
                log_debug("No mutable directory found for host hierarchy '%s', there will be no upperdir", op->hierarchy);
                return 0;
        }

        /* Require upper dir to be on writable filesystem if it's going to be used as an actual overlayfs
         * upperdir, instead of a lowerdir as an imported path. */
        r = path_is_read_only_fs(op->resolved_mutable_directory);
        if (r < 0)
                return log_error_errno(r, "Failed to determine if mutable directory '%s' is on read-only filesystem: %m", op->resolved_mutable_directory);
        if (r > 0)
                return log_error_errno(SYNTHETIC_ERRNO(EROFS), "Can't use '%s' as an upperdir as it is read-only.", op->resolved_mutable_directory);

        op->upper_dir = strdup(op->resolved_mutable_directory);
        if (!op->upper_dir)
                return log_oom();

        return 0;
}

static int determine_work_dir(OverlayFSPaths *op) {
        _cleanup_free_ char *work_dir = NULL;
        int r;

        assert(op);
        assert(!op->work_dir);

        if (!op->upper_dir)
                return 0;

        if (arg_mutable == MUTABLE_IMPORT)
                return 0;

        r = work_dir_for_hierarchy(op->hierarchy, op->upper_dir, &work_dir);
        if (r < 0)
                return r;

        op->work_dir = TAKE_PTR(work_dir);
        return 0;
}

static int mount_overlayfs_with_op(
                OverlayFSPaths *op,
                ImageClass image_class,
                int noexec,
                const char *overlay_path,
                const char *meta_path) {

        int r;
        const char *top_layer = NULL;

        assert(op);
        assert(overlay_path);

        r = mkdir_p(overlay_path, 0700);
        if (r < 0)
                return log_error_errno(r, "Failed to make directory '%s': %m", overlay_path);

        r = mkdir_p(meta_path, 0700);
        if (r < 0)
                return log_error_errno(r, "Failed to make directory '%s': %m", meta_path);

        if (op->upper_dir && op->work_dir) {
                r = mkdir_p(op->work_dir, 0700);
                if (r < 0)
                        return log_error_errno(r, "Failed to make directory '%s': %m", op->work_dir);
                top_layer = op->upper_dir;
        } else {
                assert(!strv_isempty(op->lower_dirs));
                top_layer = op->lower_dirs[0];
        }

        /* Overlayfs merged directory has the same mode as the top layer (either first lowerdir in options in
         * read-only case, or upperdir for mutable case. Set up top overlayfs layer to the same mode as the
         * unmerged hierarchy, otherwise we might end up with merged hierarchy owned by root and with mode
         * being 0700. */
        if (chmod(top_layer, op->hierarchy_mode) < 0)
                return log_error_errno(errno, "Failed to set permissions of '%s' to %04o: %m", top_layer, op->hierarchy_mode);

        r = mount_overlayfs(image_class, noexec, overlay_path, op->lower_dirs, op->upper_dir, op->work_dir);
        if (r < 0)
                return r;

        return 0;
}

static int write_extensions_file(ImageClass image_class, char **extensions, const char *meta_path) {
        _cleanup_free_ char *f = NULL, *buf = NULL;
        int r;

        assert(extensions);
        assert(meta_path);

        /* Let's generate a metadata file that lists all extensions we took into account for this
         * hierarchy. We include this in the final fs, to make things nicely discoverable and
         * recognizable. */
        f = path_join(meta_path, image_class_info[image_class].dot_directory_name, image_class_info[image_class].short_identifier_plural);
        if (!f)
                return log_oom();

        buf = strv_join(extensions, "\n");
        if (!buf)
                return log_oom();

        r = write_string_file(f, buf, WRITE_STRING_FILE_CREATE|WRITE_STRING_FILE_MKDIR_0755);
        if (r < 0)
                return log_error_errno(r, "Failed to write extension meta file '%s': %m", f);

        return 0;
}

static int write_dev_file(ImageClass image_class, const char *meta_path, const char *overlay_path) {
        _cleanup_free_ char *f = NULL;
        struct stat st;
        int r;

        assert(meta_path);
        assert(overlay_path);

        /* Now we have mounted the new file system. Let's now figure out its .st_dev field, and make that
         * available in the metadata directory. This is useful to detect whether the metadata dir actually
         * belongs to the fs it is found on: if .st_dev of the top-level mount matches it, it's pretty likely
         * we are looking at a live tree, and not an unpacked tar or so of one. */
        if (stat(overlay_path, &st) < 0)
                return log_error_errno(errno, "Failed to stat mount '%s': %m", overlay_path);

        f = path_join(meta_path, image_class_info[image_class].dot_directory_name, "dev");
        if (!f)
                return log_oom();

        /* Modifying the underlying layers while the overlayfs is mounted is technically undefined, but at
         * least it won't crash or deadlock, as per the kernel docs about overlayfs:
         * https://www.kernel.org/doc/html/latest/filesystems/overlayfs.html#changes-to-underlying-filesystems */
        r = write_string_file(f, FORMAT_DEVNUM(st.st_dev), WRITE_STRING_FILE_CREATE);
        if (r < 0)
                return log_error_errno(r, "Failed to write '%s': %m", f);

        return 0;
}

static int write_work_dir_file(ImageClass image_class, const char *meta_path, const char *work_dir) {
        _cleanup_free_ char *escaped_work_dir_in_root = NULL, *f = NULL;
        char *work_dir_in_root = NULL;
        int r;

        assert(meta_path);

        if (!work_dir)
                return 0;

        /* Do not store work dir path for ephemeral mode, it will be gone once this process is done. */
        if (IN_SET(arg_mutable, MUTABLE_EPHEMERAL, MUTABLE_EPHEMERAL_IMPORT))
                return 0;

        work_dir_in_root = path_startswith(work_dir, empty_to_root(arg_root));
        if (!work_dir_in_root)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Workdir '%s' must not be outside root '%s'", work_dir, empty_to_root(arg_root));

        f = path_join(meta_path, image_class_info[image_class].dot_directory_name, "work_dir");
        if (!f)
                return log_oom();

        /* Paths can have newlines for whatever reason, so better escape them to really get a single
         * line file. */
        escaped_work_dir_in_root = cescape(work_dir_in_root);
        if (!escaped_work_dir_in_root)
                return log_oom();
        r = write_string_file(f, escaped_work_dir_in_root, WRITE_STRING_FILE_CREATE);
        if (r < 0)
                return log_error_errno(r, "Failed to write '%s': %m", f);

        return 0;
}

static int store_info_in_meta(
                ImageClass image_class,
                char **extensions,
                const char *meta_path,
                const char *overlay_path,
                const char *work_dir) {

        int r;

        assert(extensions);
        assert(meta_path);
        assert(overlay_path);
        /* work_dir may be NULL */

        r = write_extensions_file(image_class, extensions, meta_path);
        if (r < 0)
                return r;

        r = write_dev_file(image_class, meta_path, overlay_path);
        if (r < 0)
                return r;

        r = write_work_dir_file(image_class, meta_path, work_dir);
        if (r < 0)
                return r;

        /* Make sure the top-level dir has an mtime marking the point we established the merge */
        if (utimensat(AT_FDCWD, meta_path, NULL, AT_SYMLINK_NOFOLLOW) < 0)
                return log_error_errno(r, "Failed fix mtime of '%s': %m", meta_path);

        return 0;
}

static int make_mounts_read_only(ImageClass image_class, const char *overlay_path, bool mutable) {
        int r;

        assert(overlay_path);

        if (mutable) {
                /* Bind mount the meta path as read-only on mutable overlays to avoid accidental
                 * modifications of the contents of meta directory, which could lead to systemd thinking that
                 * this hierarchy is not our mount. */
                _cleanup_free_ char *f = NULL;

                f = path_join(overlay_path, image_class_info[image_class].dot_directory_name);
                if (!f)
                        return log_oom();

                r = mount_nofollow_verbose(LOG_ERR, f, f, NULL, MS_BIND, NULL);
                if (r < 0)
                        return r;

                r = bind_remount_one(f, MS_RDONLY, MS_RDONLY);
                if (r < 0)
                        return log_error_errno(r, "Failed to remount '%s' as read-only: %m", f);
        } else {
                /* The overlayfs superblock is read-only. Let's also mark the bind mount read-only. Extra
                 * turbo safety 😎 */
                r = bind_remount_recursive(overlay_path, MS_RDONLY, MS_RDONLY, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to make bind mount '%s' read-only: %m", overlay_path);
        }

        return 0;
}

static int merge_hierarchy(
                ImageClass image_class,
                const char *hierarchy,
                int noexec,
                char **extensions,
                char **paths,
                const char *meta_path,
                const char *overlay_path,
                const char *workspace_path) {

        _cleanup_(overlayfs_paths_freep) OverlayFSPaths *op = NULL;
        size_t extensions_used = 0;
        int r;

        assert(hierarchy);
        assert(extensions);
        assert(paths);
        assert(meta_path);
        assert(overlay_path);
        assert(workspace_path);

        r = overlayfs_paths_new(hierarchy, workspace_path, &op);
        if (r < 0)
                return r;

        r = determine_lower_dirs(op, paths, meta_path, &extensions_used);
        if (r < 0)
                return r;

        if (extensions_used == 0) /* No extension with files in this hierarchy? Then don't do anything. */
                return 0;

        r = determine_upper_dir(op);
        if (r < 0)
                return r;

        r = determine_work_dir(op);
        if (r < 0)
                return r;

        r = mount_overlayfs_with_op(op, image_class, noexec, overlay_path, meta_path);
        if (r < 0)
                return r;

        r = store_info_in_meta(image_class, extensions, meta_path, overlay_path, op->work_dir);
        if (r < 0)
                return r;

        r = make_mounts_read_only(image_class, overlay_path, op->upper_dir && op->work_dir);
        if (r < 0)
                return r;

        return 1;
}

static int strverscmp_improvedp(char *const* a, char *const* b) {
        /* usable in qsort() for sorting a string array with strverscmp_improved() */
        return strverscmp_improved(*a, *b);
}

static const ImagePolicy *pick_image_policy(const Image *img) {
        assert(img);
        assert(img->path);

        /* Explicitly specified policy always wins */
        if (arg_image_policy)
                return arg_image_policy;

        /* If located in /.extra/sysext/ in the initrd, then it was placed there by systemd-stub, and was
         * picked up from an untrusted ESP. Thus, require a stricter policy by default for them. (For the
         * other directories we assume the appropriate level of trust was already established already.  */

        if (in_initrd()) {
                if (path_startswith(img->path, "/.extra/sysext/"))
                        return &image_policy_sysext_strict;
                if (path_startswith(img->path, "/.extra/confext/"))
                        return &image_policy_confext_strict;

                /* Better safe than sorry, refuse everything else passed in via the untrusted /.extra/ dir */
                if (path_startswith(img->path, "/.extra/"))
                        return &image_policy_deny;
        }

        return image_class_info[img->class].default_image_policy;
}

static int merge_subprocess(
                ImageClass image_class,
                char **hierarchies,
                bool force,
                int noexec,
                Hashmap *images,
                const char *workspace) {

        _cleanup_free_ char *host_os_release_id = NULL, *host_os_release_version_id = NULL, *host_os_release_api_level = NULL, *buf = NULL;
        _cleanup_strv_free_ char **extensions = NULL, **paths = NULL;
        size_t n_extensions = 0;
        unsigned n_ignored = 0;
        Image *img;
        int r;

        /* Mark the whole of /run as MS_SLAVE, so that we can mount stuff below it that doesn't show up on
         * the host otherwise. */
        r = mount_nofollow_verbose(LOG_ERR, NULL, "/run", NULL, MS_SLAVE|MS_REC, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to remount /run/ MS_SLAVE: %m");

        /* Let's create the workspace if it's missing */
        r = mkdir_p(workspace, 0700);
        if (r < 0)
                return log_error_errno(r, "Failed to create '%s': %m", workspace);

        /* Let's mount a tmpfs to our workspace. This way we don't need to clean up the inodes we mount over,
         * but let the kernel do that entirely automatically, once our namespace dies. Note that this file
         * system won't be visible to anyone but us, since we opened our own namespace and then made the
         * /run/ hierarchy (which our workspace is contained in) MS_SLAVE, see above. */
        r = mount_nofollow_verbose(LOG_ERR, image_class_info[image_class].short_identifier, workspace, "tmpfs", 0, "mode=0700");
        if (r < 0)
                return r;

        /* Acquire host OS release info, so that we can compare it with the extension's data */
        r = parse_os_release(
                        arg_root,
                        "ID", &host_os_release_id,
                        "VERSION_ID", &host_os_release_version_id,
                        image_class_info[image_class].level_env, &host_os_release_api_level);
        if (r < 0)
                return log_error_errno(r, "Failed to acquire 'os-release' data of OS tree '%s': %m", empty_to_root(arg_root));
        if (isempty(host_os_release_id))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "'ID' field not found or empty in 'os-release' data of OS tree '%s': %m",
                                       empty_to_root(arg_root));

        /* Let's now mount all images */
        HASHMAP_FOREACH(img, images) {
                _cleanup_free_ char *p = NULL;

                p = path_join(workspace, image_class_info[image_class].short_identifier_plural, img->name);
                if (!p)
                        return log_oom();

                r = mkdir_p(p, 0700);
                if (r < 0)
                        return log_error_errno(r, "Failed to create %s: %m", p);

                switch (img->type) {
                case IMAGE_DIRECTORY:
                case IMAGE_SUBVOLUME:

                        if (!force) {
                                r = extension_has_forbidden_content(p);
                                if (r < 0)
                                        return r;
                                if (r > 0) {
                                        n_ignored++;
                                        continue;
                                }
                        }

                        r = mount_nofollow_verbose(LOG_ERR, img->path, p, NULL, MS_BIND, NULL);
                        if (r < 0)
                                return r;

                        /* Make this a read-only bind mount */
                        r = bind_remount_recursive(p, MS_RDONLY, MS_RDONLY, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to make bind mount '%s' read-only: %m", p);

                        break;

                case IMAGE_RAW:
                case IMAGE_BLOCK: {
                        _cleanup_(dissected_image_unrefp) DissectedImage *m = NULL;
                        _cleanup_(loop_device_unrefp) LoopDevice *d = NULL;
                        _cleanup_(verity_settings_done) VeritySettings verity_settings = VERITY_SETTINGS_DEFAULT;
                        DissectImageFlags flags =
                                DISSECT_IMAGE_READ_ONLY |
                                DISSECT_IMAGE_GENERIC_ROOT |
                                DISSECT_IMAGE_REQUIRE_ROOT |
                                DISSECT_IMAGE_MOUNT_ROOT_ONLY |
                                DISSECT_IMAGE_USR_NO_ROOT |
                                DISSECT_IMAGE_ADD_PARTITION_DEVICES |
                                DISSECT_IMAGE_PIN_PARTITION_DEVICES |
                                DISSECT_IMAGE_ALLOW_USERSPACE_VERITY;

                        r = verity_settings_load(&verity_settings, img->path, NULL, NULL);
                        if (r < 0)
                                return log_error_errno(r, "Failed to read verity artifacts for %s: %m", img->path);

                        if (verity_settings.data_path)
                                flags |= DISSECT_IMAGE_NO_PARTITION_TABLE;

                        if (!force)
                                flags |= DISSECT_IMAGE_VALIDATE_OS_EXT;

                        r = loop_device_make_by_path(
                                        img->path,
                                        O_RDONLY,
                                        /* sector_size= */ UINT32_MAX,
                                        FLAGS_SET(flags, DISSECT_IMAGE_NO_PARTITION_TABLE) ? 0 : LO_FLAGS_PARTSCAN,
                                        LOCK_SH,
                                        &d);
                        if (r < 0)
                                return log_error_errno(r, "Failed to set up loopback device for %s: %m", img->path);

                        r = dissect_loop_device_and_warn(
                                        d,
                                        &verity_settings,
                                        /* mount_options= */ NULL,
                                        pick_image_policy(img),
                                        flags,
                                        &m);
                        if (r < 0)
                                return r;

                        r = dissected_image_load_verity_sig_partition(
                                        m,
                                        d->fd,
                                        &verity_settings);
                        if (r < 0)
                                return r;

                        r = dissected_image_decrypt_interactively(
                                        m, NULL,
                                        &verity_settings,
                                        flags);
                        if (r < 0)
                                return r;

                        r = dissected_image_mount_and_warn(
                                        m,
                                        p,
                                        /* uid_shift= */ UID_INVALID,
                                        /* uid_range= */ UID_INVALID,
                                        /* userns_fd= */ -EBADF,
                                        flags);
                        if (r < 0 && r != -ENOMEDIUM)
                                return r;
                        if (r == -ENOMEDIUM && !force) {
                                n_ignored++;
                                continue;
                        }

                        r = dissected_image_relinquish(m);
                        if (r < 0)
                                return log_error_errno(r, "Failed to relinquish DM and loopback block devices: %m");
                        break;
                }
                default:
                        assert_not_reached();
                }

                if (force)
                        log_debug("Force mode enabled, skipping version validation.");
                else {
                        r = extension_release_validate(
                                        img->name,
                                        host_os_release_id,
                                        host_os_release_version_id,
                                        host_os_release_api_level,
                                        in_initrd() ? "initrd" : "system",
                                        image_extension_release(img, image_class),
                                        image_class);
                        if (r < 0)
                                return r;
                        if (r == 0) {
                                n_ignored++;
                                continue;
                        }
                }

                /* Nice! This one is an extension we want. */
                r = strv_extend(&extensions, img->name);
                if (r < 0)
                        return log_oom();

                n_extensions++;
        }

        /* Nothing left? Then shortcut things */
        if (n_extensions == 0) {
                if (n_ignored > 0)
                        log_info("No suitable extensions found (%u ignored due to incompatible image(s)).", n_ignored);
                else
                        log_info("No extensions found.");
                return 0;
        }

        /* Order by version sort with strverscmp_improved() */
        typesafe_qsort(extensions, n_extensions, strverscmp_improvedp);

        buf = strv_join(extensions, "', '");
        if (!buf)
                return log_oom();

        log_info("Using extensions '%s'.", buf);

        /* Build table of extension paths (in reverse order) */
        paths = new0(char*, n_extensions + 1);
        if (!paths)
                return log_oom();

        for (size_t k = 0; k < n_extensions; k++) {
                _cleanup_free_ char *p = NULL;

                assert_se(img = hashmap_get(images, extensions[n_extensions - 1 - k]));

                p = path_join(workspace, image_class_info[image_class].short_identifier_plural, img->name);
                if (!p)
                        return log_oom();

                paths[k] = TAKE_PTR(p);
        }

        /* Let's now unmerge the status quo ante, since to build the new overlayfs we need a reference to the
         * underlying fs. */
        STRV_FOREACH(h, hierarchies) {
                _cleanup_free_ char *resolved = NULL;

                r = chase(*h, arg_root, CHASE_PREFIX_ROOT|CHASE_NONEXISTENT, &resolved, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve hierarchy '%s%s': %m", strempty(arg_root), *h);

                r = unmerge_hierarchy(image_class, resolved);
                if (r < 0)
                        return r;
        }

        /* Create overlayfs mounts for all hierarchies */
        STRV_FOREACH(h, hierarchies) {
                _cleanup_free_ char *meta_path = NULL, *overlay_path = NULL, *merge_hierarchy_workspace = NULL;

                meta_path = path_join(workspace, "meta", *h); /* The place where to store metadata about this instance */
                if (!meta_path)
                        return log_oom();

                overlay_path = path_join(workspace, "overlay", *h); /* The resulting overlayfs instance */
                if (!overlay_path)
                        return log_oom();

                /* Temporary directory for merge_hierarchy needs, like ephemeral directories. */
                merge_hierarchy_workspace = path_join(workspace, "mh_workspace", *h);
                if (!merge_hierarchy_workspace)
                        return log_oom();

                r = merge_hierarchy(
                                image_class,
                                *h,
                                noexec,
                                extensions,
                                paths,
                                meta_path,
                                overlay_path,
                                merge_hierarchy_workspace);
                if (r < 0)
                        return r;
        }

        /* And move them all into place. This is where things appear in the host namespace */
        STRV_FOREACH(h, hierarchies) {
                _cleanup_free_ char *p = NULL, *resolved = NULL;

                p = path_join(workspace, "overlay", *h);
                if (!p)
                        return log_oom();

                if (laccess(p, F_OK) < 0) {
                        if (errno != ENOENT)
                                return log_error_errno(errno, "Failed to check if '%s' exists: %m", p);

                        /* Hierarchy apparently was empty in all extensions, and wasn't mounted, ignoring. */
                        continue;
                }

                r = chase(*h, arg_root, CHASE_PREFIX_ROOT|CHASE_NONEXISTENT, &resolved, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve hierarchy '%s%s': %m", strempty(arg_root), *h);

                r = mkdir_p(resolved, 0755);
                if (r < 0)
                        return log_error_errno(r, "Failed to create hierarchy mount point '%s': %m", resolved);

                /* Using MS_REC to potentially bring in our read-only bind mount of metadata. */
                r = mount_nofollow_verbose(LOG_ERR, p, resolved, NULL, MS_BIND|MS_REC, NULL);
                if (r < 0)
                        return r;

                log_info("Merged extensions into '%s'.", resolved);
        }

        return 1;
}

static int merge(ImageClass image_class,
                 char **hierarchies,
                 bool force,
                 bool no_reload,
                 int noexec,
                 Hashmap *images) {
        pid_t pid;
        int r;

        r = safe_fork("(sd-merge)", FORK_DEATHSIG_SIGTERM|FORK_LOG|FORK_NEW_MOUNTNS, &pid);
        if (r < 0)
                return log_error_errno(r, "Failed to fork off child: %m");
        if (r == 0) {
                /* Child with its own mount namespace */

                r = merge_subprocess(image_class, hierarchies, force, noexec, images, "/run/systemd/sysext");
                if (r < 0)
                        _exit(EXIT_FAILURE);

                /* Our namespace ceases to exist here, also implicitly detaching all temporary mounts we
                 * created below /run. Nice! */

                _exit(r > 0 ? EXIT_SUCCESS : 123); /* 123 means: didn't find any extensions */
        }

        r = wait_for_terminate_and_check("(sd-merge)", pid, WAIT_LOG_ABNORMAL);
        if (r < 0)
                return r;
        if (r == 123) /* exit code 123 means: didn't do anything */
                return 0;
        if (r > 0)
                return log_error_errno(SYNTHETIC_ERRNO(ENXIO), "Failed to merge hierarchies");

        r = need_reload(image_class, hierarchies, no_reload);
        if (r < 0)
                return r;
        if (r > 0) {
                r = daemon_reload();
                if (r < 0)
                        return r;
        }

        return 1;
}

static int image_discover_and_read_metadata(
                ImageClass image_class,
                Hashmap **ret_images) {
        _cleanup_hashmap_free_ Hashmap *images = NULL;
        Image *img;
        int r;

        assert(ret_images);

        images = hashmap_new(&image_hash_ops);
        if (!images)
                return log_oom();

        r = image_discover(image_class, arg_root, images);
        if (r < 0)
                return log_error_errno(r, "Failed to discover images: %m");

        HASHMAP_FOREACH(img, images) {
                r = image_read_metadata(img, image_class_info[image_class].default_image_policy);
                if (r < 0)
                        return log_error_errno(r, "Failed to read metadata for image %s: %m", img->name);
        }

        *ret_images = TAKE_PTR(images);

        return 0;
}

static int look_for_merged_hierarchies(
                ImageClass image_class,
                char **hierarchies,
                const char **ret_which) {
        int r;

        assert(ret_which);

        /* In merge mode fail if things are already merged. (In --refresh mode below we'll unmerge if we find
         * things are already merged...) */
        STRV_FOREACH(p, hierarchies) {
                _cleanup_free_ char *resolved = NULL;

                r = chase(*p, arg_root, CHASE_PREFIX_ROOT, &resolved, NULL);
                if (r == -ENOENT) {
                        log_debug_errno(r, "Hierarchy '%s%s' does not exist, ignoring.", strempty(arg_root), *p);
                        continue;
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve path to hierarchy '%s%s': %m", strempty(arg_root), *p);

                r = is_our_mount_point(image_class, resolved);
                if (r < 0)
                        return r;
                if (r > 0) {
                        *ret_which = *p;
                        return 1;
                }
        }

        *ret_which = NULL;
        return 0;
}

static int verb_merge(int argc, char **argv, void *userdata) {
        _cleanup_hashmap_free_ Hashmap *images = NULL;
        const char *which;
        int r;

        r = have_effective_cap(CAP_SYS_ADMIN);
        if (r < 0)
                return log_error_errno(r, "Failed to check if we have enough privileges: %m");
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EPERM), "Need to be privileged.");

        r = image_discover_and_read_metadata(arg_image_class, &images);
        if (r < 0)
                return r;

        r = look_for_merged_hierarchies(arg_image_class, arg_hierarchies, &which);
        if (r < 0)
                return r;
        if (r > 0)
                return log_error_errno(SYNTHETIC_ERRNO(EBUSY), "Hierarchy '%s' is already merged.", which);

        return merge(arg_image_class,
                     arg_hierarchies,
                     arg_force,
                     arg_no_reload,
                     arg_noexec,
                     images);
}

typedef struct MethodMergeParameters {
        const char *class;
        int force;
        int no_reload;
        int noexec;
} MethodMergeParameters;

static int parse_merge_parameters(Varlink *link, JsonVariant *parameters, MethodMergeParameters *p) {

        static const JsonDispatch dispatch_table[] = {
                { "class",    JSON_VARIANT_STRING,  json_dispatch_const_string, offsetof(MethodMergeParameters, class),     0 },
                { "force",    JSON_VARIANT_BOOLEAN, json_dispatch_boolean,      offsetof(MethodMergeParameters, force),     0 },
                { "noReload", JSON_VARIANT_BOOLEAN, json_dispatch_boolean,      offsetof(MethodMergeParameters, no_reload), 0 },
                { "noexec",   JSON_VARIANT_BOOLEAN, json_dispatch_boolean,      offsetof(MethodMergeParameters, noexec),    0 },
                {}
        };

        assert(link);
        assert(parameters);
        assert(p);

        return varlink_dispatch(link, parameters, dispatch_table, p);
}

static int vl_method_merge(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        _cleanup_hashmap_free_ Hashmap *images = NULL;
        MethodMergeParameters p = {
                .force = -1,
                .no_reload = -1,
                .noexec = -1,
        };
        _cleanup_strv_free_ char **hierarchies = NULL;
        ImageClass image_class = arg_image_class;
        int r;

        assert(link);

        r = parse_merge_parameters(link, parameters, &p);
        if (r != 0)
                return r;

        r = parse_image_class_parameter(link, p.class, &image_class, &hierarchies);
        if (r < 0)
                return r;

        r = image_discover_and_read_metadata(image_class, &images);
        if (r < 0)
                return r;

        const char *which;
        r = look_for_merged_hierarchies(
                        image_class,
                        hierarchies ?: arg_hierarchies,
                        &which);
        if (r < 0)
                return r;
        if (r > 0)
                return varlink_errorb(link, "io.systemd.sysext.AlreadyMerged", JSON_BUILD_OBJECT(JSON_BUILD_PAIR_STRING("hierarchy", which)));

        r = merge(image_class,
                  hierarchies ?: arg_hierarchies,
                  p.force >= 0 ? p.force : arg_force,
                  p.no_reload >= 0 ? p.no_reload : arg_no_reload,
                  p.noexec >= 0 ? p.noexec : arg_noexec,
                  images);
        if (r < 0)
                return r;

        return varlink_reply(link, NULL);
}

static int refresh(
                ImageClass image_class,
                char **hierarchies,
                bool force,
                bool no_reload,
                int noexec) {

        _cleanup_hashmap_free_ Hashmap *images = NULL;
        int r;

        r = image_discover_and_read_metadata(image_class, &images);
        if (r < 0)
                return r;

        /* Returns > 0 if it did something, i.e. a new overlayfs is mounted now. When it does so it
         * implicitly unmounts any overlayfs placed there before. Returns == 0 if it did nothing, i.e. no
         * extension images found. In this case the old overlayfs remains in place if there was one. */
        r = merge(image_class, hierarchies, force, no_reload, noexec, images);
        if (r < 0)
                return r;
        if (r == 0) /* No images found? Then unmerge. The goal of --refresh is after all that after having
                     * called there's a guarantee that the merge status matches the installed extensions. */
                r = unmerge(image_class, hierarchies, no_reload);

        /* Net result here is that:
         *
         * 1. If an overlayfs was mounted before and no extensions exist anymore, we'll have unmerged things.
         *
         * 2. If an overlayfs was mounted before, and there are still extensions installed' we'll have
         *    unmerged and then merged things again.
         *
         * 3. If an overlayfs so far wasn't mounted, and there are extensions installed, we'll have it
         *    mounted now.
         *
         * 4. If there was no overlayfs mount so far, and no extensions installed, we implement a NOP.
         */

        return 0;
}

static int verb_refresh(int argc, char **argv, void *userdata) {
        int r;

        r = have_effective_cap(CAP_SYS_ADMIN);
        if (r < 0)
                return log_error_errno(r, "Failed to check if we have enough privileges: %m");
        if (r == 0)
                return log_error_errno(SYNTHETIC_ERRNO(EPERM), "Need to be privileged.");

        return refresh(arg_image_class,
                       arg_hierarchies,
                       arg_force,
                       arg_no_reload,
                       arg_noexec);
}

static int vl_method_refresh(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {

        MethodMergeParameters p = {
                .force = -1,
                .no_reload = -1,
                .noexec = -1,
        };
        _cleanup_strv_free_ char **hierarchies = NULL;
        ImageClass image_class = arg_image_class;
        int r;

        assert(link);

        r = parse_merge_parameters(link, parameters, &p);
        if (r != 0)
                return r;

        r = parse_image_class_parameter(link, p.class, &image_class, &hierarchies);
        if (r < 0)
                return r;

        r = refresh(image_class,
                    hierarchies ?: arg_hierarchies,
                    p.force >= 0 ? p.force : arg_force,
                    p.no_reload >= 0 ? p.no_reload : arg_no_reload,
                    p.noexec >= 0 ? p.noexec : arg_noexec);
        if (r < 0)
                return r;

        return varlink_reply(link, NULL);
}

static int verb_list(int argc, char **argv, void *userdata) {
        _cleanup_hashmap_free_ Hashmap *images = NULL;
        _cleanup_(table_unrefp) Table *t = NULL;
        Image *img;
        int r;

        images = hashmap_new(&image_hash_ops);
        if (!images)
                return log_oom();

        r = image_discover(arg_image_class, arg_root, images);
        if (r < 0)
                return log_error_errno(r, "Failed to discover images: %m");

        if ((arg_json_format_flags & JSON_FORMAT_OFF) && hashmap_isempty(images)) {
                log_info("No OS extensions found.");
                return 0;
        }

        t = table_new("name", "type", "path", "time");
        if (!t)
                return log_oom();

        HASHMAP_FOREACH(img, images) {
                r = table_add_many(
                                t,
                                TABLE_STRING, img->name,
                                TABLE_STRING, image_type_to_string(img->type),
                                TABLE_PATH, img->path,
                                TABLE_TIMESTAMP, img->mtime != 0 ? img->mtime : img->crtime);
                if (r < 0)
                        return table_log_add_error(r);
        }

        (void) table_set_sort(t, (size_t) 0);

        return table_print_with_pager(t, arg_json_format_flags, arg_pager_flags, arg_legend);
}

typedef struct MethodListParameters {
        const char *class;
} MethodListParameters;

static int vl_method_list(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {

        static const JsonDispatch dispatch_table[] = {
                { "class",    JSON_VARIANT_STRING,  json_dispatch_const_string, offsetof(MethodListParameters, class),     0 },
                {}
        };
        MethodListParameters p = {
        };
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
        _cleanup_hashmap_free_ Hashmap *images = NULL;
        ImageClass image_class = arg_image_class;
        Image *img;
        int r;

        assert(link);

        r = varlink_dispatch(link, parameters, dispatch_table, &p);
        if (r != 0)
                return r;

        r = parse_image_class_parameter(link, p.class, &image_class, NULL);
        if (r < 0)
                return r;

        images = hashmap_new(&image_hash_ops);
        if (!images)
                return -ENOMEM;

        r = image_discover(image_class, arg_root, images);
        if (r < 0)
                return r;

        HASHMAP_FOREACH(img, images) {
                if (v) {
                        /* Send previous item with more=true */
                        r = varlink_notify(link, v);
                        if (r < 0)
                                return r;
                }

                v = json_variant_unref(v);

                r = image_to_json(img, &v);
                if (r < 0)
                        return r;
        }

        if (v)  /* Send final item with more=false */
                return varlink_reply(link, v);

        return varlink_error(link, "io.systemd.sysext.NoImagesFound", NULL);
}

static int verb_help(int argc, char **argv, void *userdata) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man(image_class_info[arg_image_class].full_identifier, "8", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s [OPTIONS...] COMMAND\n"
               "\n%5$s%7$s%6$s\n"
               "\n%3$sCommands:%4$s\n"
               "  status                  Show current merge status (default)\n"
               "  merge                   Merge extensions into relevant hierarchies\n"
               "  unmerge                 Unmerge extensions from relevant hierarchies\n"
               "  refresh                 Unmerge/merge extensions again\n"
               "  list                    List installed extensions\n"
               "  -h --help               Show this help\n"
               "     --version            Show package version\n"
               "\n%3$sOptions:%4$s\n"
               "     --mutable=yes|no|auto|import|ephemeral|ephemeral-import\n"
               "                          Specify a mutability mode of the merged hierarchy\n"
               "     --no-pager           Do not pipe output into a pager\n"
               "     --no-legend          Do not show the headers and footers\n"
               "     --root=PATH          Operate relative to root path\n"
               "     --json=pretty|short|off\n"
               "                          Generate JSON output\n"
               "     --force              Ignore version incompatibilities\n"
               "     --no-reload          Do not reload the service manager\n"
               "     --image-policy=POLICY\n"
               "                          Specify disk image dissection policy\n"
               "     --noexec=BOOL        Whether to mount extension overlay with noexec\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal(),
               image_class_info[arg_image_class].blurb);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_NO_PAGER,
                ARG_NO_LEGEND,
                ARG_ROOT,
                ARG_JSON,
                ARG_FORCE,
                ARG_IMAGE_POLICY,
                ARG_NOEXEC,
                ARG_NO_RELOAD,
                ARG_MUTABLE,
        };

        static const struct option options[] = {
                { "help",         no_argument,       NULL, 'h'              },
                { "version",      no_argument,       NULL, ARG_VERSION      },
                { "no-pager",     no_argument,       NULL, ARG_NO_PAGER     },
                { "no-legend",    no_argument,       NULL, ARG_NO_LEGEND    },
                { "root",         required_argument, NULL, ARG_ROOT         },
                { "json",         required_argument, NULL, ARG_JSON         },
                { "force",        no_argument,       NULL, ARG_FORCE        },
                { "image-policy", required_argument, NULL, ARG_IMAGE_POLICY },
                { "noexec",       required_argument, NULL, ARG_NOEXEC       },
                { "no-reload",    no_argument,       NULL, ARG_NO_RELOAD    },
                { "mutable",      required_argument, NULL, ARG_MUTABLE      },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return verb_help(argc, argv, NULL);

                case ARG_VERSION:
                        return version();

                case ARG_NO_PAGER:
                        arg_pager_flags |= PAGER_DISABLE;
                        break;

                case ARG_NO_LEGEND:
                        arg_legend = false;
                        break;

                case ARG_ROOT:
                        r = parse_path_argument(optarg, false, &arg_root);
                        if (r < 0)
                                return r;
                        /* If --root= is provided, do not reload the service manager */
                        arg_no_reload = true;
                        break;

                case ARG_JSON:
                        r = parse_json_argument(optarg, &arg_json_format_flags);
                        if (r <= 0)
                                return r;

                        break;

                case ARG_FORCE:
                        arg_force = true;
                        break;

                case ARG_IMAGE_POLICY:
                        r = parse_image_policy_argument(optarg, &arg_image_policy);
                        if (r < 0)
                                return r;
                        break;

                case ARG_NOEXEC:
                        r = parse_boolean_argument("--noexec", optarg, NULL);
                        if (r < 0)
                                return r;

                        arg_noexec = r;
                        break;

                case ARG_NO_RELOAD:
                        arg_no_reload = true;
                        break;

                case ARG_MUTABLE:
                        r = parse_mutable_mode(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse argument to --mutable=: %s", optarg);
                        arg_mutable = r;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        r = varlink_invocation(VARLINK_ALLOW_ACCEPT);
        if (r < 0)
                return log_error_errno(r, "Failed to check if invoked in Varlink mode: %m");
        if (r > 0)
                arg_varlink = true;

        return 1;
}

static int sysext_main(int argc, char *argv[]) {

        static const Verb verbs[] = {
                { "status",   VERB_ANY, 1, VERB_DEFAULT, verb_status  },
                { "merge",    VERB_ANY, 1, 0,            verb_merge   },
                { "unmerge",  VERB_ANY, 1, 0,            verb_unmerge },
                { "refresh",  VERB_ANY, 1, 0,            verb_refresh },
                { "list",     VERB_ANY, 1, 0,            verb_list    },
                { "help",     VERB_ANY, 1, 0,            verb_help    },
                {}
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

static int run(int argc, char *argv[]) {
        const char *env_var;
        int r;

        log_setup();

        arg_image_class = invoked_as(argv, "systemd-confext") ? IMAGE_CONFEXT : IMAGE_SYSEXT;

        env_var = getenv(image_class_info[arg_image_class].mode_env);
        if (env_var) {
                r = parse_mutable_mode(env_var);
                if (r < 0)
                        log_warning("Failed to parse %s environment variable value '%s'. Ignoring.",
                                    image_class_info[arg_image_class].mode_env, env_var);
                else
                        arg_mutable = r;
        }

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        /* For debugging purposes it might make sense to do this for other hierarchies than /usr/ and
         * /opt/, but let's make that a hacker/debugging feature, i.e. env var instead of cmdline
         * switch. */
        r = parse_env_extension_hierarchies(&arg_hierarchies, image_class_info[arg_image_class].name_env);
        if (r < 0)
                return log_error_errno(r, "Failed to parse environment variable: %m");

        if (arg_varlink) {
                _cleanup_(varlink_server_unrefp) VarlinkServer *varlink_server = NULL;

                /* Invocation as Varlink service */

                r = varlink_server_new(&varlink_server, VARLINK_SERVER_ROOT_ONLY);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate Varlink server: %m");

                r = varlink_server_add_interface(varlink_server, &vl_interface_io_systemd_sysext);
                if (r < 0)
                        return log_error_errno(r, "Failed to add Varlink interface: %m");

                r = varlink_server_bind_method_many(
                                varlink_server,
                                "io.systemd.sysext.Merge", vl_method_merge,
                                "io.systemd.sysext.Unmerge", vl_method_unmerge,
                                "io.systemd.sysext.Refresh", vl_method_refresh,
                                "io.systemd.sysext.List", vl_method_list);
                if (r < 0)
                        return log_error_errno(r, "Failed to bind Varlink methods: %m");

                r = varlink_server_loop_auto(varlink_server);
                if (r == -EPERM)
                        return log_error_errno(r, "Invoked by unprivileged Varlink peer, refusing.");
                if (r < 0)
                        return log_error_errno(r, "Failed to run Varlink event loop: %m");

                return EXIT_SUCCESS;
        }

        return sysext_main(argc, argv);
}

DEFINE_MAIN_FUNCTION(run);
