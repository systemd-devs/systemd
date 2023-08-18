/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "sd-messages.h"

#include "alloc-util.h"
#include "dirent-util.h"
#include "exit-status.h"
#include "fd-util.h"
#include "fileio.h"
#include "generator.h"
#include "hashmap.h"
#include "hexdecoct.h"
#include "initrd-util.h"
#include "install.h"
#include "log.h"
#include "main-func.h"
#include "mkdir.h"
#include "path-lookup.h"
#include "path-util.h"
#include "set.h"
#include "special.h"
#include "specifier.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"

/* 🚨 Note: this generator is deprecated! Please do not add new features! Instead, please port remaining SysV
 * scripts over to native unit files! Thank you! 🚨 */

static const struct {
        const char *path;
        const char *target;
} rcnd_table[] = {
        /* Standard SysV runlevels for start-up */
        { "rc1.d",  SPECIAL_RESCUE_TARGET     },
        { "rc2.d",  SPECIAL_MULTI_USER_TARGET },
        { "rc3.d",  SPECIAL_MULTI_USER_TARGET },
        { "rc4.d",  SPECIAL_MULTI_USER_TARGET },
        { "rc5.d",  SPECIAL_GRAPHICAL_TARGET  },

        /* We ignore the SysV runlevels for shutdown here, as SysV services get default dependencies anyway, and that
         * means they are shut down anyway at system power off if running. */
};

static const char *arg_dest = NULL;

typedef struct SysvStub {
        char *name;
        char *path;
        char *description;
        int sysv_start_priority;
        char *pid_file;
        char **before;
        char **after;
        char **wants;
        char **wanted_by;
        bool has_lsb;
        bool reload;
        bool loaded;
} SysvStub;

static SysvStub* free_sysvstub(SysvStub *s) {
        if (!s)
                return NULL;

        free(s->name);
        free(s->path);
        free(s->description);
        free(s->pid_file);
        strv_free(s->before);
        strv_free(s->after);
        strv_free(s->wants);
        strv_free(s->wanted_by);
        return mfree(s);
}
DEFINE_TRIVIAL_CLEANUP_FUNC(SysvStub*, free_sysvstub);

static void free_sysvstub_hashmapp(Hashmap **h) {
        hashmap_free_with_destructor(*h, free_sysvstub);
}

static int add_alias(const char *service, const char *alias) {
        _cleanup_free_ char *link = NULL;

        assert(service);
        assert(alias);

        link = path_join(arg_dest, alias);
        if (!link)
                return -ENOMEM;

        if (symlink(service, link) < 0) {
                if (errno == EEXIST)
                        return 0;

                return -errno;
        }

        return 1;
}

static int generate_unit_file(SysvStub *s) {
        _cleanup_free_ char *path_escaped = NULL, *unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(s);

        if (!s->loaded)
                return 0;

        path_escaped = unit_setting_escape_path(s->path);
        if (!path_escaped)
                return log_oom();

        unit = path_join(arg_dest, s->name);
        if (!unit)
                return log_oom();

        /* We might already have a symlink with the same name from a Provides:,
         * or from backup files like /etc/init.d/foo.bak. Real scripts always win,
         * so remove an existing link */
        if (is_symlink(unit) > 0) {
                log_warning("Overwriting existing symlink %s with real service.", unit);
                (void) unlink(unit);
        }

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-sysv-generator\n\n"
                "[Unit]\n"
                "Documentation=man:systemd-sysv-generator(8)\n"
                "SourcePath=%s\n",
                path_escaped);

        if (s->description) {
                _cleanup_free_ char *t = NULL;

                t = specifier_escape(s->description);
                if (!t)
                        return log_oom();

                fprintf(f, "Description=%s\n", t);
        }

        STRV_FOREACH(p, s->before)
                fprintf(f, "Before=%s\n", *p);
        STRV_FOREACH(p, s->after)
                fprintf(f, "After=%s\n", *p);
        STRV_FOREACH(p, s->wants)
                fprintf(f, "Wants=%s\n", *p);

        fprintf(f,
                "\n[Service]\n"
                "Type=forking\n"
                "Restart=no\n"
                "TimeoutSec=5min\n"
                "IgnoreSIGPIPE=no\n"
                "KillMode=process\n"
                "GuessMainPID=no\n"
                "RemainAfterExit=%s\n",
                yes_no(!s->pid_file));

        if (s->pid_file) {
                _cleanup_free_ char *t = NULL;

                t = unit_setting_escape_path(s->pid_file);
                if (!t)
                        return log_oom();

                fprintf(f, "PIDFile=%s\n", t);
        }

        /* Consider two special LSB exit codes a clean exit */
        if (s->has_lsb)
                fprintf(f,
                        "SuccessExitStatus=%i %i\n",
                        EXIT_NOTINSTALLED,
                        EXIT_NOTCONFIGURED);

        fprintf(f,
                "ExecStart=%s start\n"
                "ExecStop=%s stop\n",
                path_escaped, path_escaped);

        if (s->reload)
                fprintf(f, "ExecReload=%s reload\n", path_escaped);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit %s: %m", unit);

        STRV_FOREACH(p, s->wanted_by)
                (void) generator_add_symlink(arg_dest, *p, "wants", s->name);

        return 1;
}

static bool usage_contains_reload(const char *line) {
        return (strcasestr(line, "{reload|") ||
                strcasestr(line, "{reload}") ||
                strcasestr(line, "{reload\"") ||
                strcasestr(line, "|reload|") ||
                strcasestr(line, "|reload}") ||
                strcasestr(line, "|reload\""));
}

static char *sysv_translate_name(const char *name) {
        _cleanup_free_ char *c = NULL;
        char *res;

        c = strdup(name);
        if (!c)
                return NULL;

        res = endswith(c, ".sh");
        if (res)
                *res = 0;

        if (unit_name_mangle(c, 0, &res) < 0)
                return NULL;

        return res;
}

static int sysv_translate_facility(SysvStub *s, unsigned line, const char *name, char **ret) {

        /* We silently ignore the $ prefix here. According to the LSB
         * spec it simply indicates whether something is a
         * standardized name or a distribution-specific one. Since we
         * just follow what already exists and do not introduce new
         * uses or names we don't care who introduced a new name. */

        static const char * const table[] = {
                /* LSB defined facilities */
                "local_fs",             NULL,
                "network",              SPECIAL_NETWORK_ONLINE_TARGET,
                "named",                SPECIAL_NSS_LOOKUP_TARGET,
                "portmap",              SPECIAL_RPCBIND_TARGET,
                "remote_fs",            SPECIAL_REMOTE_FS_TARGET,
                "syslog",               NULL,
                "time",                 SPECIAL_TIME_SYNC_TARGET,
        };

        _cleanup_free_ char *filename = NULL;
        const char *n;
        char *e, *m;
        int r;

        assert(name);
        assert(s);
        assert(ret);

        r = path_extract_filename(s->path, &filename);
        if (r < 0)
                return log_error_errno(r, "Failed to extract file name from path '%s': %m", s->path);

        n = *name == '$' ? name + 1 : name;

        for (size_t i = 0; i < ELEMENTSOF(table); i += 2) {
                if (!streq(table[i], n))
                        continue;

                if (!table[i+1]) {
                        *ret = NULL;
                        return 0;
                }

                m = strdup(table[i+1]);
                if (!m)
                        return log_oom();

                *ret = m;
                return 1;
        }

        /* If we don't know this name, fallback heuristics to figure
         * out whether something is a target or a service alias. */

        /* Facilities starting with $ are most likely targets */
        if (*name == '$')  {
                r = unit_name_build(n, NULL, ".target", ret);
                if (r < 0)
                        return log_error_errno(r, "[%s:%u] Could not build name for facility %s: %m", s->path, line, name);

                return 1;
        }

        /* Strip ".sh" suffix from file name for comparison */
        e = endswith(filename, ".sh");
        if (e)
                *e = '\0';

        /* Names equaling the file name of the services are redundant */
        if (streq_ptr(n, filename)) {
                *ret = NULL;
                return 0;
        }

        /* Everything else we assume to be normal service names */
        m = sysv_translate_name(n);
        if (!m)
                return log_oom();

        *ret = m;
        return 1;
}

static int handle_provides(SysvStub *s, unsigned line, const char *full_text, const char *text) {
        int r;

        assert(s);
        assert(full_text);
        assert(text);

        for (;;) {
                _cleanup_free_ char *word = NULL, *m = NULL;

                r = extract_first_word(&text, &word, NULL, EXTRACT_UNQUOTE|EXTRACT_RELAX);
                if (r < 0)
                        return log_error_errno(r, "[%s:%u] Failed to parse word from provides string: %m", s->path, line);
                if (r == 0)
                        break;

                r = sysv_translate_facility(s, line, word, &m);
                if (r <= 0) /* continue on error */
                        continue;

                switch (unit_name_to_type(m)) {

                case UNIT_SERVICE:
                        log_debug("Adding Provides: alias '%s' for '%s'", m, s->name);
                        r = add_alias(s->name, m);
                        if (r < 0)
                                log_warning_errno(r, "[%s:%u] Failed to add LSB Provides name %s, ignoring: %m", s->path, line, m);
                        break;

                case UNIT_TARGET:

                        /* NB: SysV targets which are provided by a
                         * service are pulled in by the services, as
                         * an indication that the generic service is
                         * now available. This is strictly one-way.
                         * The targets do NOT pull in SysV services! */

                        r = strv_extend(&s->before, m);
                        if (r < 0)
                                return log_oom();

                        r = strv_extend(&s->wants, m);
                        if (r < 0)
                                return log_oom();

                        if (streq(m, SPECIAL_NETWORK_ONLINE_TARGET)) {
                                r = strv_extend(&s->before, SPECIAL_NETWORK_TARGET);
                                if (r < 0)
                                        return log_oom();
                                r = strv_extend(&s->wants, SPECIAL_NETWORK_TARGET);
                                if (r < 0)
                                        return log_oom();
                        }

                        break;

                case _UNIT_TYPE_INVALID:
                        log_warning("Unit name '%s' is invalid", m);
                        break;

                default:
                        log_warning("Unknown unit type for unit '%s'", m);
                }
        }

        return 0;
}

static int handle_dependencies(SysvStub *s, unsigned line, const char *full_text, const char *text) {
        int r;

        assert(s);
        assert(full_text);
        assert(text);

        for (;;) {
                _cleanup_free_ char *word = NULL, *m = NULL;
                bool is_before;

                r = extract_first_word(&text, &word, NULL, EXTRACT_UNQUOTE|EXTRACT_RELAX);
                if (r < 0)
                        return log_error_errno(r, "[%s:%u] Failed to parse word from provides string: %m", s->path, line);
                if (r == 0)
                        break;

                r = sysv_translate_facility(s, line, word, &m);
                if (r <= 0) /* continue on error */
                        continue;

                is_before = startswith_no_case(full_text, "X-Start-Before:");

                if (streq(m, SPECIAL_NETWORK_ONLINE_TARGET) && !is_before) {
                        /* the network-online target is special, as it needs to be actively pulled in */
                        r = strv_extend(&s->after, m);
                        if (r < 0)
                                return log_oom();

                        r = strv_extend(&s->wants, m);
                } else
                        r = strv_extend(is_before ? &s->before : &s->after, m);
                if (r < 0)
                        return log_oom();
        }

        return 0;
}

static int load_sysv(SysvStub *s) {
        _cleanup_fclose_ FILE *f = NULL;
        unsigned line = 0;
        int r;
        enum {
                NORMAL,
                DESCRIPTION,
                LSB,
                LSB_DESCRIPTION,
                USAGE_CONTINUATION
        } state = NORMAL;
        _cleanup_free_ char *short_description = NULL, *long_description = NULL, *chkconfig_description = NULL;
        char *description;
        bool supports_reload = false;

        assert(s);

        f = fopen(s->path, "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                return log_error_errno(errno, "Failed to open %s: %m", s->path);
        }

        log_debug("Loading SysV script %s", s->path);

        for (;;) {
                _cleanup_free_ char *l = NULL;
                char *t;

                r = read_line(f, LONG_LINE_MAX, &l);
                if (r < 0)
                        return log_error_errno(r, "Failed to read configuration file '%s': %m", s->path);
                if (r == 0)
                        break;

                line++;

                t = strstrip(l);
                if (*t != '#') {
                        /* Try to figure out whether this init script supports
                         * the reload operation. This heuristic looks for
                         * "Usage" lines which include the reload option. */
                        if (state == USAGE_CONTINUATION ||
                            (state == NORMAL && strcasestr(t, "usage"))) {
                                if (usage_contains_reload(t)) {
                                        supports_reload = true;
                                        state = NORMAL;
                                } else if (t[strlen(t)-1] == '\\')
                                        state = USAGE_CONTINUATION;
                                else
                                        state = NORMAL;
                        }

                        continue;
                }

                if (state == NORMAL && streq(t, "### BEGIN INIT INFO")) {
                        state = LSB;
                        s->has_lsb = true;
                        continue;
                }

                if (IN_SET(state, LSB_DESCRIPTION, LSB) && streq(t, "### END INIT INFO")) {
                        state = NORMAL;
                        continue;
                }

                t++;
                t += strspn(t, WHITESPACE);

                if (state == NORMAL) {

                        /* Try to parse Red Hat style description */

                        if (startswith_no_case(t, "description:")) {

                                size_t k;
                                const char *j;

                                k = strlen(t);
                                if (k > 0 && t[k-1] == '\\') {
                                        state = DESCRIPTION;
                                        t[k-1] = 0;
                                }

                                j = empty_to_null(strstrip(t+12));

                                r = free_and_strdup(&chkconfig_description, j);
                                if (r < 0)
                                        return log_oom();

                        } else if (startswith_no_case(t, "pidfile:")) {
                                const char *fn;

                                state = NORMAL;

                                fn = strstrip(t+8);
                                if (!path_is_absolute(fn)) {
                                        log_error("[%s:%u] PID file not absolute. Ignoring.", s->path, line);
                                        continue;
                                }

                                r = free_and_strdup(&s->pid_file, fn);
                                if (r < 0)
                                        return log_oom();
                        }

                } else if (state == DESCRIPTION) {

                        /* Try to parse Red Hat style description
                         * continuation */

                        size_t k;
                        const char *j;

                        k = strlen(t);
                        if (k > 0 && t[k-1] == '\\')
                                t[k-1] = 0;
                        else
                                state = NORMAL;

                        j = strstrip(t);
                        if (!isempty(j) && !strextend_with_separator(&chkconfig_description, " ", j))
                                return log_oom();

                } else if (IN_SET(state, LSB, LSB_DESCRIPTION)) {

                        if (startswith_no_case(t, "Provides:")) {
                                state = LSB;

                                r = handle_provides(s, line, t, t + 9);
                                if (r < 0)
                                        return r;

                        } else if (startswith_no_case(t, "Required-Start:") ||
                                   startswith_no_case(t, "Should-Start:") ||
                                   startswith_no_case(t, "X-Start-Before:") ||
                                   startswith_no_case(t, "X-Start-After:")) {

                                state = LSB;

                                r = handle_dependencies(s, line, t, strchr(t, ':') + 1);
                                if (r < 0)
                                        return r;

                        } else if (startswith_no_case(t, "Description:")) {
                                const char *j;

                                state = LSB_DESCRIPTION;

                                j = empty_to_null(strstrip(t+12));

                                r = free_and_strdup(&long_description, j);
                                if (r < 0)
                                        return log_oom();

                        } else if (startswith_no_case(t, "Short-Description:")) {
                                const char *j;

                                state = LSB;

                                j = empty_to_null(strstrip(t+18));

                                r = free_and_strdup(&short_description, j);
                                if (r < 0)
                                        return log_oom();

                        } else if (state == LSB_DESCRIPTION) {

                                if (startswith(l, "#\t") || startswith(l, "#  ")) {
                                        const char *j;

                                        j = strstrip(t);
                                        if (!isempty(j) && !strextend_with_separator(&long_description, " ", j))
                                                return log_oom();
                                } else
                                        state = LSB;
                        }
                }
        }

        s->reload = supports_reload;

        /* We use the long description only if
         * no short description is set. */

        if (short_description)
                description = short_description;
        else if (chkconfig_description)
                description = chkconfig_description;
        else if (long_description)
                description = long_description;
        else
                description = NULL;

        if (description) {
                char *d;

                d = strjoin(s->has_lsb ? "LSB: " : "SYSV: ", description);
                if (!d)
                        return log_oom();

                s->description = d;
        }

        s->loaded = true;
        return 0;
}

static int fix_order(SysvStub *s, Hashmap *all_services) {
        SysvStub *other;
        int r;

        assert(s);

        if (!s->loaded)
                return 0;

        if (s->sysv_start_priority < 0)
                return 0;

        HASHMAP_FOREACH(other, all_services) {
                if (s == other)
                        continue;

                if (!other->loaded)
                        continue;

                if (other->sysv_start_priority < 0)
                        continue;

                /* If both units have modern headers we don't care
                 * about the priorities */
                if (s->has_lsb && other->has_lsb)
                        continue;

                if (other->sysv_start_priority < s->sysv_start_priority) {
                        r = strv_extend(&s->after, other->name);
                        if (r < 0)
                                return log_oom();

                } else if (other->sysv_start_priority > s->sysv_start_priority) {
                        r = strv_extend(&s->before, other->name);
                        if (r < 0)
                                return log_oom();
                } else
                        continue;

                /* FIXME: Maybe we should compare the name here lexicographically? */
        }

        return 0;
}

static int acquire_search_path(const char *def, const char *envvar, char ***ret) {
        _cleanup_strv_free_ char **l = NULL;
        const char *e;
        int r;

        assert(def);
        assert(envvar);

        e = getenv(envvar);
        if (e) {
                r = path_split_and_make_absolute(e, &l);
                if (r < 0)
                        return log_error_errno(r, "Failed to make $%s search path absolute: %m", envvar);
        }

        if (strv_isempty(l)) {
                strv_free(l);

                l = strv_new(def);
                if (!l)
                        return log_oom();
        }

        if (!path_strv_resolve_uniq(l, NULL))
                return log_oom();

        *ret = TAKE_PTR(l);

        return 0;
}

static int enumerate_sysv(const LookupPaths *lp, Hashmap *all_services) {
        _cleanup_strv_free_ char **sysvinit_path = NULL;
        int r;

        assert(lp);

        r = acquire_search_path(SYSTEM_SYSVINIT_PATH, "SYSTEMD_SYSVINIT_PATH", &sysvinit_path);
        if (r < 0)
                return r;

        STRV_FOREACH(path, sysvinit_path) {
                _cleanup_closedir_ DIR *d = NULL;

                d = opendir(*path);
                if (!d) {
                        if (errno != ENOENT)
                                log_warning_errno(errno, "Opening %s failed, ignoring: %m", *path);
                        continue;
                }

                FOREACH_DIRENT(de, d, log_error_errno(errno, "Failed to enumerate directory %s, ignoring: %m", *path)) {
                        _cleanup_free_ char *fpath = NULL, *name = NULL;
                        _cleanup_(free_sysvstubp) SysvStub *service = NULL;
                        struct stat st;

                        if (fstatat(dirfd(d), de->d_name, &st, 0) < 0) {
                                log_warning_errno(errno, "stat() failed on %s/%s, ignoring: %m", *path, de->d_name);
                                continue;
                        }

                        if (!(st.st_mode & S_IXUSR))
                                continue;

                        if (!S_ISREG(st.st_mode))
                                continue;

                        name = sysv_translate_name(de->d_name);
                        if (!name)
                                return log_oom();

                        if (hashmap_contains(all_services, name))
                                continue;

                        r = unit_file_exists(RUNTIME_SCOPE_SYSTEM, lp, name);
                        if (r < 0 && !IN_SET(r, -ELOOP, -ERFKILL, -EADDRNOTAVAIL)) {
                                log_debug_errno(r, "Failed to detect whether %s exists, skipping: %m", name);
                                continue;
                        } else if (r != 0) {
                                log_debug("Native unit for %s already exists, skipping.", name);
                                continue;
                        }

                        fpath = path_join(*path, de->d_name);
                        if (!fpath)
                                return log_oom();

                        log_struct(LOG_WARNING,
                                   LOG_MESSAGE("SysV service '%s' lacks a native systemd unit file. "
                                               "%s Automatically generating a unit file for compatibility. Please update package to include a native systemd unit file, in order to make it safe, robust and future-proof. "
                                               "%s This compatibility logic is deprecated, expect removal soon. %s",
                                               fpath,
                                               special_glyph(SPECIAL_GLYPH_RECYCLING),
                                               special_glyph(SPECIAL_GLYPH_WARNING_SIGN), special_glyph(SPECIAL_GLYPH_WARNING_SIGN)),
                                   "MESSAGE_ID=" SD_MESSAGE_SYSV_GENERATOR_DEPRECATED_STR,
                                   "SYSVSCRIPT=%s", fpath,
                                   "UNIT=%s", name);

                        service = new(SysvStub, 1);
                        if (!service)
                                return log_oom();

                        *service = (SysvStub) {
                                .sysv_start_priority = -1,
                                .name = TAKE_PTR(name),
                                .path = TAKE_PTR(fpath),
                        };

                        r = hashmap_put(all_services, service->name, service);
                        if (r < 0)
                                return log_oom();

                        TAKE_PTR(service);
                }
        }

        return 0;
}

static int set_dependencies_from_rcnd(const LookupPaths *lp, Hashmap *all_services) {
        Set *runlevel_services[ELEMENTSOF(rcnd_table)] = {};
        _cleanup_strv_free_ char **sysvrcnd_path = NULL;
        SysvStub *service;
        int r;

        assert(lp);

        r = acquire_search_path(SYSTEM_SYSVRCND_PATH, "SYSTEMD_SYSVRCND_PATH", &sysvrcnd_path);
        if (r < 0)
                return r;

        STRV_FOREACH(p, sysvrcnd_path)
                for (unsigned i = 0; i < ELEMENTSOF(rcnd_table); i ++) {
                        _cleanup_closedir_ DIR *d = NULL;
                        _cleanup_free_ char *path = NULL;

                        path = path_join(*p, rcnd_table[i].path);
                        if (!path) {
                                r = log_oom();
                                goto finish;
                        }

                        d = opendir(path);
                        if (!d) {
                                if (errno != ENOENT)
                                        log_warning_errno(errno, "Opening %s failed, ignoring: %m", path);

                                continue;
                        }

                        FOREACH_DIRENT(de, d, log_warning_errno(errno, "Failed to enumerate directory %s, ignoring: %m", path)) {
                                _cleanup_free_ char *name = NULL, *fpath = NULL;
                                int a, b;

                                if (de->d_name[0] != 'S')
                                        continue;

                                if (strlen(de->d_name) < 4)
                                        continue;

                                a = undecchar(de->d_name[1]);
                                b = undecchar(de->d_name[2]);

                                if (a < 0 || b < 0)
                                        continue;

                                fpath = path_join(*p, de->d_name);
                                if (!fpath) {
                                        r = log_oom();
                                        goto finish;
                                }

                                name = sysv_translate_name(de->d_name + 3);
                                if (!name) {
                                        r = log_oom();
                                        goto finish;
                                }

                                service = hashmap_get(all_services, name);
                                if (!service) {
                                        log_debug("Ignoring %s symlink in %s, not generating %s.", de->d_name, rcnd_table[i].path, name);
                                        continue;
                                }

                                service->sysv_start_priority = MAX(a*10 + b, service->sysv_start_priority);

                                r = set_ensure_put(&runlevel_services[i], NULL, service);
                                if (r < 0) {
                                        log_oom();
                                        goto finish;
                                }
                        }
                }

        for (unsigned i = 0; i < ELEMENTSOF(rcnd_table); i++)
                SET_FOREACH(service, runlevel_services[i]) {
                        r = strv_extend(&service->before, rcnd_table[i].target);
                        if (r < 0) {
                                log_oom();
                                goto finish;
                        }
                        r = strv_extend(&service->wanted_by, rcnd_table[i].target);
                        if (r < 0) {
                                log_oom();
                                goto finish;
                        }
                }

        r = 0;

finish:
        for (unsigned i = 0; i < ELEMENTSOF(rcnd_table); i++)
                set_free(runlevel_services[i]);

        return r;
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        _cleanup_(free_sysvstub_hashmapp) Hashmap *all_services = NULL;
        _cleanup_(lookup_paths_free) LookupPaths lp = {};
        SysvStub *service;
        int r;

        if (in_initrd()) {
                log_debug("Skipping generator, running in the initrd.");
                return EXIT_SUCCESS;
        }

        assert_se(arg_dest = dest_late);

        r = lookup_paths_init_or_warn(&lp, RUNTIME_SCOPE_SYSTEM, LOOKUP_PATHS_EXCLUDE_GENERATED, NULL);
        if (r < 0)
                return r;

        all_services = hashmap_new(&string_hash_ops);
        if (!all_services)
                return log_oom();

        r = enumerate_sysv(&lp, all_services);
        if (r < 0)
                return r;

        r = set_dependencies_from_rcnd(&lp, all_services);
        if (r < 0)
                return r;

        HASHMAP_FOREACH(service, all_services)
                (void) load_sysv(service);

        HASHMAP_FOREACH(service, all_services) {
                (void) fix_order(service, all_services);
                (void) generate_unit_file(service);
        }

        return 0;
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
