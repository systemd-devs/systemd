/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <stdbool.h>

#include "build.h"
#include "chase.h"
#include "conf-files.h"
#include "dissect-image.h"
#include "env-file.h"
#include "env-util.h"
#include "exec-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "find-esp.h"
#include "id128-util.h"
#include "kernel-image.h"
#include "main-func.h"
#include "mkdir.h"
#include "mount-util.h"
#include "mountpoint-util.h"
#include "os-util.h"
#include "parse-argument.h"
#include "path-util.h"
#include "pretty-print.h"
#include "rm-rf.h"
#include "stat-util.h"
#include "string-table.h"
#include "strv.h"
#include "tmpfile-util.h"
#include "verbs.h"

static bool arg_verbose = false;
static char *arg_esp_path = NULL;
static char *arg_xbootldr_path = NULL;
static char *arg_root = NULL;
static char *arg_image = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_esp_path, freep);
STATIC_DESTRUCTOR_REGISTER(arg_xbootldr_path, freep);
STATIC_DESTRUCTOR_REGISTER(arg_root, freep);
STATIC_DESTRUCTOR_REGISTER(arg_image, freep);

typedef enum Action {
        ACTION_ADD,
        ACTION_REMOVE,
        ACTION_INSPECT,
        _ACTION_MAX,
        _ACTION_INVALID = -EINVAL,
} Action;

typedef enum Layout {
        LAYOUT_AUTO,
        LAYOUT_UKI,
        LAYOUT_BLS,
        LAYOUT_OTHER,
        _LAYOUT_MAX,
        _LAYOUT_INVALID = -EINVAL,
} Layout;

static const  char * const layout_table[_LAYOUT_MAX] = {
        [LAYOUT_AUTO]  = "auto",
        [LAYOUT_UKI]   = "uki",
        [LAYOUT_BLS]   = "bls",
        [LAYOUT_OTHER] = "other",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP(layout, Layout);

typedef struct Context {
        Action action;
        sd_id128_t machine_id;
        KernelImageType kernel_image_type;
        Layout layout;
        char *layout_other;
        char *conf_root;
        char *boot_root;
        char *entry_token;
        char *entry_dir;
        char *version;
        char *kernel;
        char **initrds;
        char *initrd_generator;
        char *staging_area;
        char **plugins;
        const char **args;
        char **envs;
} Context;

static void context_done(Context *c) {
        assert(c);

        free(c->layout_other);
        free(c->conf_root);
        free(c->boot_root);
        free(c->entry_token);
        free(c->entry_dir);
        free(c->version);
        free(c->kernel);
        strv_free(c->initrds);
        free(c->initrd_generator);
        if (c->action == ACTION_INSPECT)
                free(c->staging_area);
        else
                rm_rf_physical_and_free(c->staging_area);
        strv_free(c->plugins);
        free(c->args); /* Don't use strv_free(). */
        strv_free(c->envs);
}

static const char *context_get_layout(Context *c) {
        assert(c);
        assert(c->layout >= 0);

        return c->layout_other ?: layout_to_string(c->layout);
}

static int context_set_layout(Context *c, const char *s, const char *source) {
        Layout t;

        assert(c);
        assert(source);

        if (c->layout >= 0 || !s)
                return 0;

        assert(!c->layout_other);

        t = layout_from_string(s);
        if (t >= 0)
                c->layout = t;
        else if (isempty(s))
                c->layout = LAYOUT_AUTO;
        else {
                c->layout_other = strdup(s);
                if (!c->layout_other)
                        return log_oom();

                c->layout = LAYOUT_OTHER;
        }

        log_debug("layout=%s set via %s", context_get_layout(c), source);
        return 1;
}

static int context_set_machine_id(Context *c, const char *s, const char *source) {
        int r;

        assert(c);
        assert(source);

        if (!sd_id128_is_null(c->machine_id) || !s)
                return 0;

        r = sd_id128_from_string(s, &c->machine_id);
        if (r < 0)
                return log_warning_errno(r, "Failed to parse machine ID specified in %s, ignoring.", source);

        if (sd_id128_is_null(c->machine_id))
                return 0;

        log_debug("MACHINE_ID=%s set via %s.", SD_ID128_TO_STRING(c->machine_id), source);
        return 1;
}

static int context_set_string(const char *s, const char *source, const char *name, char **ret) {
        char *p;

        assert(source);
        assert(name);
        assert(ret);

        if (!s)
                return 0;

        p = strdup(s);
        if (!p)
                return log_oom();

        log_debug("%s=%s set via %s.", name, p, source);

        *ret = p;
        return 1;
}

static int context_set_initrd_generator(Context *c, const char *s, const char *source) {
        assert(c);

        if (c->initrd_generator)
                return 0;

        return context_set_string(s, source, "INITRD_GENERATOR", &c->initrd_generator);
}

static int context_set_path(const char *s, const char *source, const char *name, char **ret) {
        char *p;
        int r;

        assert(source);
        assert(name);
        assert(ret);

        if (!s)
                return 0;

        if (!path_is_absolute(s) || !path_is_safe(s))
                return log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                         "Invalid path for %s specified in %s, ignoring.", name, source);

        r = chase(s, arg_root, CHASE_PREFIX_ROOT, &p, /* ret_fd = */ NULL);
        if (r < 0)
                return log_warning_errno(r, "Failed to chase %s=%s specified in %s, ignoring: %m", name, s, source);

        log_debug("%s=%s set via %s.", name, p, source);

        *ret = p;
        return 1;
}

static int context_set_boot_root(Context *c, const char *s, const char *source) {
        assert(c);

        if (c->boot_root)
                return 0;

        return context_set_path(s, source, "BOOT_ROOT", &c->boot_root);
}

static int context_set_conf_root(Context *c, const char *s, const char *source) {
        assert(c);

        if (c->conf_root)
                return 0;

        return context_set_path(s, source, "CONF_ROOT", &c->conf_root);
}

static int context_set_plugins(Context *c, const char *s, const char *source) {
        _cleanup_strv_free_ char **v = NULL, **w = NULL;
        int r;

        assert(c);

        if (c->plugins || !s)
                return 0;

        v = strv_split(s, NULL);
        if (!v)
                return log_oom();

        STRV_FOREACH(p, v) {
                char *q;

                r = chase(*p, arg_root, CHASE_PREFIX_ROOT, &q, /* ret_fd = */ NULL);
                if (r < 0) {
                        log_warning_errno(r, "Failed to chase plugin '%s' specified in %s, ignoring: %m", *p, source);
                        continue;
                }

                r = strv_consume(&w, q);
                if (r < 0)
                        return log_oom();
        }

        log_debug("Plugins set via %s", source);

        c->plugins = TAKE_PTR(w);
        return 1;
}

static int context_load_environment(Context *c) {
        assert(c);

        (void) context_set_machine_id(c, getenv("MACHINE_ID"), "environment");
        (void) context_set_boot_root(c, getenv("BOOT_ROOT"), "environment");
        (void) context_set_conf_root(c, getenv("KERNEL_INSTALL_CONF_ROOT"), "environment");
        (void) context_set_plugins(c, getenv("KERNEL_INSTALL_PLUGINS"), "environment");
        return 0;
}

static int context_ensure_conf_root(Context *c) {
        int r;

        assert(c);

        if (c->conf_root)
                return 0;

        r = chase("/etc/kernel", arg_root, CHASE_PREFIX_ROOT, &c->conf_root, /* ret_fd = */ NULL);
        if (r < 0)
                log_debug_errno(r, "Failed to chase /etc/kernel, ignoring: %m");

        return 0;
}

static int context_load_install_conf_one(Context *c, const char *root, const char *path) {
        _cleanup_free_ char
                *conf = NULL, *machine_id = NULL, *boot_root = NULL,
                *layout = NULL, *initrd_generator = NULL;
        int r;

        assert(c);
        assert(path);

        conf = path_join(root, path, "install.conf");
        if (!conf)
                return log_oom();

        log_debug("Loading %s…", conf);

        r = parse_env_file(NULL, conf,
                           "MACHINE_ID",       &machine_id,
                           "BOOT_ROOT",        &boot_root,
                           "layout",           &layout,
                           "initrd_generator", &initrd_generator);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return log_error_errno(r, "Failed to parse '%s': %m", conf);

        (void) context_set_machine_id(c, machine_id, conf);
        (void) context_set_boot_root(c, boot_root, conf);
        (void) context_set_layout(c, layout, conf);
        (void) context_set_initrd_generator(c, initrd_generator, conf);

        log_debug("Loaded %s.", conf);
        return 1;
}

static int context_load_install_conf(Context *c) {
        int r;

        assert(c);

        if (c->conf_root) {
                /* c->conf_root is already prefixed with arg_root. */
                r = context_load_install_conf_one(c, /* root = */ NULL, c->conf_root);
                if (r != 0)
                        return r;
        }

        STRV_FOREACH(p, CONF_PATHS_STRV("kernel")) {
                r = context_load_install_conf_one(c, arg_root, *p);
                if (r != 0)
                        return r;
        }

        return 0;
}

static int context_load_machine_info(Context *c) {
        _cleanup_free_ char *path_alloc = NULL, *machine_id = NULL, *layout = NULL;
        const char *path = "/etc/machine-info";
        int r;

        assert(c);

        /* If the user configured an explicit machine ID to use in /etc/machine-info to use for our purpose,
         * we'll use that instead (for compatibility). */

        if (!sd_id128_is_null(c->machine_id) && c->layout >= 0)
                return 0;

        if (arg_root) {
                path_alloc = strjoin(arg_root, path);
                if (!path_alloc)
                        return log_oom();
                path = path_alloc;
        }

        log_debug("Loading %s…", path);

        r = parse_env_file(NULL, path,
                           "KERNEL_INSTALL_MACHINE_ID", &machine_id,
                           "KERNEL_INSTALL_LAYOUT", &layout);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return log_error_errno(r, "Failed to parse '%s': %m", path);

        (void) context_set_machine_id(c, machine_id, path);
        (void) context_set_layout(c, layout, path);
        return 0;
}

static int context_load_machine_id(Context *c) {
        int r;

        assert(c);

        if (arg_root)
                return 0;

        r = sd_id128_get_machine(&c->machine_id);
        if (r < 0) {
                if (ERRNO_IS_MACHINE_ID_UNSET(r))
                        return 0;
                return log_error_errno(r, "Failed to load machine ID from /etc/machine-id: %m");
        }

        log_debug("MACHINE_ID=%s set via /etc/machine-id.", SD_ID128_TO_STRING(c->machine_id));
        return 1; /* loaded */
}

static int context_ensure_machine_id(Context *c) {
        int r;

        assert(c);

        if (!sd_id128_is_null(c->machine_id))
                return 0;

        /* If /etc/machine-id is initialized we'll use it. */
        r = context_load_machine_id(c);
        if (r != 0)
                return r;

        /* Otherwise we'll use a freshly generated one. */
        r = sd_id128_randomize(&c->machine_id);
        if (r < 0)
                return log_error_errno(r, "Failed to generate random ID: %m");

        log_debug("New machine ID '%s' generated.", SD_ID128_TO_STRING(c->machine_id));
        return 0;
}

static int context_acquire_xbootldr(Context *c) {
        int r;

        assert(c);
        assert(!c->boot_root);

        r = find_xbootldr_and_warn(
                        /* root = */ arg_root,
                        /* path = */ arg_xbootldr_path,
                        /* unprivileged_mode= */ geteuid() != 0,
                        /* ret_path = */ &c->boot_root,
                        /* ret_uuid = */ NULL,
                        /* ret_devid = */ NULL);
        if (IN_SET(r, -ENOKEY, -EACCES)) {
                log_debug_errno(r, "Couldn't find an XBOOTLDR partition.");
                return 0;
        }
        if (r < 0)
                return r;

        log_debug("Using XBOOTLDR partition at %s as $BOOT_ROOT.", c->boot_root);
        return 1; /* found */
}

static int context_acquire_esp(Context *c) {
        int r;

        assert(c);
        assert(!c->boot_root);

        r = find_esp_and_warn(
                        /* root = */ arg_root,
                        /* path = */ arg_esp_path,
                        /* unprivileged_mode= */ geteuid() != 0,
                        /* ret_path = */ &c->boot_root,
                        /* ret_part = */ NULL,
                        /* ret_pstart = */ NULL,
                        /* ret_psize = */ NULL,
                        /* ret_uuid = */ NULL,
                        /* ret_devid = */ NULL);
        if (IN_SET(r, -ENOKEY, -EACCES)) {
                log_debug_errno(r, "Couldn't find EFI system partition, ignoring.");
                return 0;
        }
        if (r < 0)
                return r;

        log_debug("Using EFI System Partition at %s as $BOOT_ROOT.", c->boot_root);
        return 1; /* found */
}

static int context_ensure_boot_root(Context *c) {
        int r;

        assert(c);

        if (c->boot_root)
                return 0;

        r = context_acquire_xbootldr(c);
        if (r != 0)
                return r;

        r = context_acquire_esp(c);
        if (r != 0)
                return r;

        /* If all else fails, use /boot. */
        r = chase("/boot", arg_root, CHASE_PREFIX_ROOT, &c->boot_root, /* ret_fd = */ NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to chase '/boot': %m");

        log_debug("KERNEL_INSTALL_BOOT_ROOT autodetection yielded no candidates, using \"%s\".", c->boot_root);
        return 0;
}

static int context_load_entry_token(Context *c) {
        _cleanup_free_ char *path = NULL, *token = NULL;
        int r;

        assert(c);
        assert(!c->entry_token);

        if (!c->conf_root)
                return 0;

        path = path_join(c->conf_root, "entry-token");
        if (!path)
                return log_oom();

        r = read_one_line_file(path, &token);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return log_error_errno(r, "Failed to read %s: %m", path);

        if (isempty(token))
                return 0;

        if (!string_is_safe(token))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "%s contains unsafe character(s).", path);

        if (!filename_is_valid(token))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid entry token read from %s: %s", path, token);

        log_debug("entry-token \"%s\" acquired from %s.", token, path);
        c->entry_token = TAKE_PTR(token);
        return 1; /* loaded */
}

static int context_find_entry_token(Context *c) {
        _cleanup_free_ char *image_id = NULL, *id = NULL;
        _cleanup_strv_free_ char **candidates = NULL;
        _cleanup_close_ int dfd = -EBADF;
        int r;

        assert(c);
        assert(c->boot_root);
        assert(!c->entry_token);

        r = parse_os_release(arg_root,
                             "IMAGE_ID", &image_id,
                             "ID",       &id);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Failed to parse %s/etc/os-release: %m", strempty(arg_root));

        candidates = strv_new(SD_ID128_TO_STRING(c->machine_id),
                              STRV_IFNOTNULL(image_id),
                              STRV_IFNOTNULL(id),
                              "Default");
        if (!candidates)
                return log_oom();

        dfd = RET_NERRNO(open(c->boot_root, O_CLOEXEC | O_DIRECTORY | O_PATH));
        if (dfd == -ENOENT)
                return 0;
        if (dfd < 0)
                return log_error_errno(dfd, "Failed to open \"%s\": %m", c->boot_root);

        STRV_FOREACH(e, candidates) {
                r = is_dir_full(dfd, *e, /* follow = */ false);
                if (IN_SET(r, 0, -ENOENT))
                        continue;
                if (r < 0)
                        return log_error_errno(r, "Failed to check if '%s/%s' is a directory: %m", c->boot_root, *e);

                log_debug("%s/%s found.", c->boot_root, *e);

                c->entry_token = strdup(*e);
                if (!c->entry_token)
                        return log_oom();

                return 1; /* found */
        }

        return 0; /* not found */
}

static int context_ensure_entry_token(Context *c) {
        int r;

        assert(c);

        /* Now that we determined the machine ID to use, let's determine the "token" for the boot loader
         * entry to generate. We use that for naming the directory below $BOOT where we want to place the
         * kernel/initrd and related resources, as well for naming the .conf boot loader spec entry.
         * Typically this is just the machine ID, but it can be anything else, too, if we are told so. */

        if (c->entry_token)
                return 0;

        r = context_load_entry_token(c);
        if (r != 0)
                return r;

        r = context_find_entry_token(c);
        if (r != 0)
                return r;

        c->entry_token = strdup(SD_ID128_TO_STRING(c->machine_id));
        if (!c->entry_token)
                return log_oom();

        log_debug("No entry-token candidate matched, using \"%s\" from machine-id.", c->entry_token);
        return 0;
}

static int context_load_plugins(Context *c) {
        int r;

        assert(c);

        if (c->plugins)
                return 0;

        r = conf_files_list_strv(&c->plugins,
                                 ".install",
                                 arg_root,
                                 CONF_FILES_EXECUTABLE | CONF_FILES_REGULAR | CONF_FILES_FILTER_MASKED,
                                 (const char**) CONF_PATHS_STRV("kernel/install.d"));
        if (r < 0)
                return log_error_errno(r, "Failed to find plugins: %m");

        return 0;
}

static int context_init(Context *c) {
        int r;

        assert(c);

        r = context_load_environment(c);
        if (r < 0)
                return r;

        r = context_ensure_conf_root(c);
        if (r < 0)
                return r;

        r = context_load_install_conf(c);
        if (r < 0)
                return r;

        r = context_load_machine_info(c);
        if (r < 0)
                return r;

        r = context_ensure_machine_id(c);
        if (r < 0)
                return r;

        r = context_ensure_boot_root(c);
        if (r < 0)
                return r;

        r = context_ensure_entry_token(c);
        if (r < 0)
                return r;

        r = context_load_plugins(c);
        if (r < 0)
                return r;

        return 0;
}

static int context_inspect_kernel(Context *c) {
        assert(c);

        if (!c->kernel)
                return 0;

        return inspect_kernel(c->kernel, &c->kernel_image_type, NULL, NULL, NULL);
}

static int context_ensure_layout(Context *c) {
        _cleanup_free_ char *path = NULL, *srel = NULL;
        int r;

        assert(c);
        assert(c->boot_root);
        assert(c->entry_token);

        if (c->layout >= 0 && c->layout != LAYOUT_AUTO)
                return 0;

        /* No layout configured by the administrator. Let's try to figure it out automatically from metadata
         * already contained in $BOOT_ROOT. */

        if (c->kernel_image_type == KERNEL_IMAGE_TYPE_UKI) {
                c->layout = LAYOUT_UKI;
                log_debug("Kernel image type is %s, using layout=%s.",
                          kernel_image_type_to_string(c->kernel_image_type), layout_to_string(c->layout));
                return 0;
        }

        path = path_join(c->boot_root, "loader/entries.srel");
        if (!path)
                return log_oom();

        r = read_one_line_file(path, &srel);
        if (r >= 0) {
                if (streq(srel, "type1"))
                        /* The loader/entries.srel file clearly indicates that the installed boot loader
                         * implements the proper standard upstream boot loader spec for Type #1 entries.
                         * Let's default to that, then. */
                        c->layout = LAYOUT_BLS;
                else
                        /* The loader/entries.srel file indicates some other spec is implemented and owns the
                         * /loader/entries/ directory. Since we have no idea what that means, let's stay away
                         * from it by default. */
                        c->layout = LAYOUT_OTHER;

                log_debug("%s with '%s' found, using layout=%s.", path, srel, layout_to_string(c->layout));
                return 0;

        } else if (r != -ENOENT)
                return log_error_errno(r, "Failed to read %s: %m", path);

        free(path);
        path = path_join(c->boot_root, c->entry_token);
        if (!path)
                return log_oom();

        r = is_dir(path, /* follow = */ false);
        if (r < 0 && r != -ENOENT)
                return log_error_errno(r, "Failed to check if '%s' is a directory: %m", path);
        if (r > 0) {
                /* If the metadata in $BOOT_ROOT doesn't tell us anything, then check if the entry token
                 * directory already exists. If so, let's assume it's the standard boot loader spec, too. */
                c->layout = LAYOUT_BLS;
                log_debug("%s exists, using layout=%s.", path, layout_to_string(c->layout));
                return 0;
        }

        /* There's no metadata in $BOOT_ROOT, and apparently no entry token directory installed? Then we
         * really don't know anything. */
        c->layout = LAYOUT_OTHER;
        log_debug("Entry-token directory not found, using layout=%s.", layout_to_string(c->layout));
        return 0;
}

static int context_setup_staging_area(Context *c) {
        static const char *template = "/tmp/kernel-install.staging.XXXXXXX";
        _cleanup_free_ char *t = NULL;
        int r;

        assert(c);

        if (c->staging_area)
                return 0;

        t = path_join(arg_root, template);
        if (!t)
                return log_oom();

        if (c->action == ACTION_INSPECT) {
                c->staging_area = TAKE_PTR(t);
                return 0;
        }

        r = mkdtemp_malloc(t, &c->staging_area);
        if (r < 0)
                return log_error_errno(r, "Failed to create staging area: %m");

        return 0;
}

static int context_build_entry_dir(Context *c) {
        assert(c);
        assert(c->boot_root);
        assert(c->entry_token);
        assert(c->version || c->action == ACTION_INSPECT);

        if (c->entry_dir)
                return 0;

        c->entry_dir = path_join(c->boot_root, c->entry_token, c->version ?: "$KERNEL_VERSION");
        if (!c->entry_dir)
                return log_oom();

        assert(path_is_absolute(c->entry_dir));
        log_debug("Using ENTRY_DIR_ABS=%s", c->entry_dir);
        return 0;
}

static int context_make_entry_dir(Context *c) {
        int r;

        assert(c);
        assert(c->entry_dir);

        /* Compatibility with earlier versions that used the presence of $BOOT_ROOT/$ENTRY_TOKEN to signal to
         * 00-entry-directory to create $ENTRY_DIR_ABS to serve as the indication to use or to not use the BLS */

        if (c->action != ACTION_ADD)
                return 0;

        if (c->layout != LAYOUT_BLS)
                return 0;

        log_debug("mkdir -p %s", c->entry_dir);
        r = mkdir_p(c->entry_dir, 0755);
        if (r < 0)
                return log_error_errno(r, "Failed to make directory '%s': %m", c->entry_dir);

        return 0;
}

static void context_remove_entry_dir(Context *c) {
        assert(c);
        assert(c->entry_dir);

        if (c->action != ACTION_REMOVE)
                return;

        if (c->layout != LAYOUT_BLS)
                return;

        log_debug("Removing %s", c->entry_dir);
        (void) rm_rf(c->entry_dir, REMOVE_ROOT|REMOVE_PHYSICAL|REMOVE_MISSING_OK|REMOVE_CHMOD);
}

static int context_build_arguments(Context *c) {
        _cleanup_free_ const char **a = NULL;
        const char *verb;
        size_t n, i = 0;

        assert(c);
        assert(c->entry_dir);

        if (c->envs)
                return 0;

        switch (c->action) {
        case ACTION_ADD:
                assert(c->version);
                assert(c->kernel);

                n = 6 + strv_length(c->initrds);
                verb = "add";
                break;

        case ACTION_REMOVE:
                assert(c->version);
                assert(!c->kernel);
                assert(!c->initrds);

                n = 5;
                verb = "remove";
                break;

        case ACTION_INSPECT:
                assert(!c->version);
                assert(!c->initrds);

                n = 7;
                verb = "<add|remove>";
                break;

        default:
                assert_not_reached();
        }

        a = new(const char*, n);
        if (!a)
                return log_oom();

        a[i++] = NULL;
        a[i++] = verb;
        a[i++] = c->version ?: "$KERNEL_VERSION";
        a[i++] = c->entry_dir;

        if (c->action == ACTION_ADD) {
                a[i++] = c->kernel;

                STRV_FOREACH(p, c->initrds)
                        a[i++] = *p;

        } else if (c->action == ACTION_INSPECT) {
                a[i++] = c->kernel ?: "[$KERNEL_IMAGE]";
                a[i++] = "[$INITRD...]";
        }

        a[i++] = NULL;
        assert(i == n);

        c->args = TAKE_PTR(a);
        return 0;
}

static int context_build_environment(Context *c) {
        _cleanup_strv_free_ char **e = NULL;
        int r;

        assert(c);

        if (c->envs)
                return 0;

        e = strv_new("LC_COLLATE=C");
        if (!e)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_VERBOSE=%i", arg_verbose);
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_IMAGE_TYPE=%s", kernel_image_type_to_string(c->kernel_image_type));
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_MACHINE_ID=%s", SD_ID128_TO_STRING(c->machine_id));
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_ENTRY_TOKEN=%s", c->entry_token);
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_ROOT=%s", strempty(arg_root));
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_BOOT_ROOT=%s", c->boot_root);
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_LAYOUT=%s", context_get_layout(c));
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_INITRD_GENERATOR=%s", strempty(c->initrd_generator));
        if (r < 0)
                return log_oom();

        r = strv_extendf(&e, "KERNEL_INSTALL_STAGING_AREA=%s", c->staging_area);
        if (r < 0)
                return log_oom();

        c->envs = TAKE_PTR(e);
        return 0;
}

static int context_prepare_execution(Context *c) {
        int r;

        assert(c);

        r = context_inspect_kernel(c);
        if (r < 0)
                return r;

        r = context_ensure_layout(c);
        if (r < 0)
                return r;

        r = context_setup_staging_area(c);
        if (r < 0)
                return r;

        r = context_build_entry_dir(c);
        if (r < 0)
                return r;

        r = context_build_arguments(c);
        if (r < 0)
                return r;

        r = context_build_environment(c);
        if (r < 0)
                return r;

        return 0;
}

static int context_execute(Context *c) {
        int r;

        assert(c);

        r = context_make_entry_dir(c);
        if (r < 0)
                return r;

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *joined = NULL;

                joined = strv_join_full(c->plugins, "", "\n  ", /* escape_separator = */ false);
                log_debug("Plugins: %s", strna(joined));

                free(joined);

                joined = strv_join_full(c->envs, "", "\n  ", /* escape_separator = */ false);
                log_debug("Environments: %s", strna(joined));

                free(joined);

                joined = strv_join((char**) c->args + 1, " ");
                log_debug("Plugin arguments: %s", strna(joined));
        }

        r = execute_strv(
                        /* name = */ NULL,
                        c->plugins,
                        USEC_INFINITY,
                        /* callbacks = */ NULL,
                        /* callback_args = */ NULL,
                        (char**) c->args,
                        c->envs,
                        EXEC_DIR_SKIP_REMAINING);
        if (r < 0)
                return r;

        context_remove_entry_dir(c);

        return 0;
}

static int context_assign_kernel(Context *c, const char *version, const char *kernel, char* const* initrds) {
        int r;

        assert(c);

        if (version) {
                if (!filename_is_valid(version))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid version specified: %s", version);

                c->version = strdup(version);
                if (!c->version)
                        return log_oom();
        }

        if (kernel) {
                r = chase(kernel, arg_root, CHASE_PREFIX_ROOT, &c->kernel, /* ret_fd = */ NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to chase kernel image file '%s': %m", kernel);
        }

        STRV_FOREACH(p, initrds) {
                char *q;

                r = chase(*p, arg_root, CHASE_PREFIX_ROOT, &q, /* ret_fd = */ NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to chase initrd file '%s': %m", *p);

                r = strv_consume(&c->initrds, q);
                if (r < 0)
                        return log_oom();
        }

        return 0;
}

static int verb_add(int argc, char *argv[], void *userdata) {
        Context *c = ASSERT_PTR(userdata);
        int r;

        assert(argc >= 3);
        assert(argv);

        c->action = ACTION_ADD;

        r = context_assign_kernel(c, argv[1], argv[2], strv_skip(argv, 3));
        if (r < 0)
                return r;

        r = context_prepare_execution(c);
        if (r < 0)
                return r;

        return context_execute(c);
}

static int run_as_installkernel(int argc, char *argv[], Context *c) {
        /* kernel's install.sh invokes us as
         *   /sbin/installkernel <version> <vmlinuz> <map> <installation-dir>
         * We ignore the last two arguments. */
        if (optind + 2 > argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "'installkernel' command requires at least two arguments.");

        return verb_add(3, STRV_MAKE("add", argv[optind], argv[optind+1]), c);
}

static int verb_remove(int argc, char *argv[], void *userdata) {
        Context *c = ASSERT_PTR(userdata);
        int r;

        assert(argc == 2);
        assert(argv);

        c->action = ACTION_REMOVE;

        r = context_assign_kernel(c, argv[1], /* kernel = */ NULL, /* initrds= */ NULL);
        if (r < 0)
                return r;

        r = context_prepare_execution(c);
        if (r < 0)
                return r;

        return context_execute(c);
}

static int verb_inspect(int argc, char *argv[], void *userdata) {
        Context *c = ASSERT_PTR(userdata);
        _cleanup_free_ char *joined = NULL;
        int r;

        c->action = ACTION_INSPECT;

        r = context_assign_kernel(c,
                                  /* version = */ NULL,
                                  /* kernel = */ argc >= 2 ? argv[1] : NULL,
                                  /* initrds = */ NULL);
        if (r < 0)
                return r;

        r = context_prepare_execution(c);
        if (r < 0)
                return r;

        puts("Plugins:");
        strv_print_full(c->plugins, "  ");
        puts("");

        puts("Environments:");
        strv_print_full(c->envs, "  ");
        puts("");

        puts("Plugin arguments:");
        joined = strv_join((char**) c->args + 1, " ");
        printf("  %s\n", strna(joined));

        return 0;
}

static bool bypass(void) {
        int r;

        r = getenv_bool("KERNEL_INSTALL_BYPASS");
        if (r <= 0) {
                if (r != -ENXIO)
                        log_debug_errno(r, "Failed to parse $KERNEL_INSTALL_BYPASS, assuming no.");
                return false;
        }

        log_debug("$KERNEL_INSTALL_BYPASS is enabled, skipping execution.");
        return true;
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("kernel-install", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s [OPTIONS...] COMMAND ...\n\n"
               "%2$sAdd and remove kernel and initrd images to and from /boot%3$s\n"
               "\nUsage:\n"
               "  %1$s [OPTIONS...] add KERNEL-VERSION KERNEL-IMAGE [INITRD-FILE...]\n"
               "  %1$s [OPTIONS...] remove KERNEL-VERSION\n"
               "  %1$s [OPTIONS...] inspect [KERNEL-IMAGE]\n"
               "\nOptions:\n"
               "  -h --help              Show this help\n"
               "     --version           Show package version\n"
               "  -v --verbose           Increase verbosity\n"
               "     --esp-path=PATH     Path to the EFI System Partition (ESP)\n"
               "     --boot-path=PATH    Path to the $BOOT partition\n"
               "     --root=PATH         Operate on an alternate filesystem root\n"
               "     --image=PATH        Operate on disk image as filesystem root\n"
               "\nSee the %4$s for details.\n",
               program_invocation_short_name,
               ansi_highlight(),
               ansi_normal(),
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_ESP_PATH,
                ARG_BOOT_PATH,
                ARG_ROOT,
                ARG_IMAGE,
        };
        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                { "verbose",   no_argument,       NULL, 'v'           },
                { "esp-path",  required_argument, NULL, ARG_ESP_PATH  },
                { "boot-path", required_argument, NULL, ARG_BOOT_PATH },
                { "root",      required_argument, NULL, ARG_ROOT      },
                { "image",     required_argument, NULL, ARG_IMAGE     },
                {}
        };
        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hv", options, NULL)) >= 0)
                switch (c) {
                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case 'v':
                        log_set_max_level(LOG_DEBUG);
                        arg_verbose = true;
                        break;

                case ARG_ESP_PATH:
                        r = parse_path_argument(optarg, /* suppress_root = */ false, &arg_esp_path);
                        if (r < 0)
                                return log_oom();
                        break;

                case ARG_BOOT_PATH:
                        r = parse_path_argument(optarg, /* suppress_root = */ false, &arg_xbootldr_path);
                        if (r < 0)
                                return log_oom();
                        break;

                case ARG_ROOT:
                        r = parse_path_argument(optarg, /* suppress_root= */ true, &arg_root);
                        if (r < 0)
                                return r;
                        break;

                case ARG_IMAGE:
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_image);
                        if (r < 0)
                                return r;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (arg_root && arg_image)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Please specify either --root= or --image=, the combination of both is not supported.");

        return 1;
}

static int run(int argc, char* argv[]) {
        static const Verb verbs[] = {
                { "add",         3,        VERB_ANY, 0,            verb_add            },
                { "remove",      2,        2,        0,            verb_remove         },
                { "inspect",     1,        2,        VERB_DEFAULT, verb_inspect        },
                {}
        };
        _cleanup_(loop_device_unrefp) LoopDevice *loop_device = NULL;
        _cleanup_(umount_and_rmdir_and_freep) char *unlink_dir = NULL;
        _cleanup_(context_done) Context c = {
                .action = _ACTION_INVALID,
                .kernel_image_type = KERNEL_IMAGE_TYPE_UNKNOWN,
                .layout = _LAYOUT_INVALID,
        };
        int r;

        log_setup();

        if (bypass())
                return 0;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = context_init(&c);
        if (r < 0)
                return r;

        /* Open up and mount the image */
        if (arg_image) {
                assert(!arg_root);

                r = mount_image_privately_interactively(
                                arg_image,
                                DISSECT_IMAGE_GENERIC_ROOT |
                                DISSECT_IMAGE_RELAX_VAR_CHECK,
                                &unlink_dir,
                                /* ret_dir_fd = */ NULL,
                                &loop_device);
                if (r < 0)
                        return r;

                arg_root = strdup(unlink_dir);
                if (!arg_root)
                        return log_oom();
        }

        if (invoked_as(argv, "installkernel"))
                return run_as_installkernel(argc, argv, &c);

        return dispatch_verb(argc, argv, verbs, &c);
}

DEFINE_MAIN_FUNCTION(run);
