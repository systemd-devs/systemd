/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>

#include "build.h"
#include "conf-files.h"
#include "constants.h"
#include "fd-util.h"
#include "fileio.h"
#include "log.h"
#include "main-func.h"
#include "module-util.h"
#include "pretty-print.h"
#include "proc-cmdline.h"
#include "string-util.h"
#include "strv.h"

static char **arg_proc_cmdline_modules = NULL;
static const char conf_file_dirs[] = CONF_PATHS_NULSTR("modules-load.d");

#define MAX_TASKS 4
static const int max_tasks = MAX_TASKS;

typedef struct {
        pthread_t thread;
        struct kmod_ctx *ctx;
        char *line;
        int result;
} thread_data;

STATIC_DESTRUCTOR_REGISTER(arg_proc_cmdline_modules, strv_freep);

static void *thread_func(void *arg) {
        thread_data *data = ASSERT_PTR(arg);
        const int r = module_load_and_warn(data->ctx, data->line, true);
        if (r == -ENOENT)
                return NULL;

        data->result = r;
        return NULL;
}

static int add_modules(const char *p) {
        _cleanup_strv_free_ char **k = NULL;

        k = strv_split(p, ",");
        if (!k)
                return log_oom();

        if (strv_extend_strv(&arg_proc_cmdline_modules, k, true) < 0)
                return log_oom();

        return 0;
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        if (proc_cmdline_key_streq(key, "modules_load")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = add_modules(value);
                if (r < 0)
                        return r;
        }

        return 0;
}

static void exec_task(int *tasks, int *r, thread_data *data, char *line, const bool to_free) {
        int i = *tasks;
        while (*tasks >= max_tasks) {
                for (i = 0; i < max_tasks; ++i) {
                        if (pthread_tryjoin_np(data[i].thread, NULL) == 0) {
                                RET_GATHER(*r, data[i].result);
                                if (to_free)
                                        free(data[i].line);

                                --*tasks;
                                break;
                        }
                }
        }

        data[i].line = line;
        pthread_create(&data[i].thread, NULL, thread_func, &data[i]);
        ++*tasks;
}

static int apply_file(thread_data *data, const char *path, bool ignore_enoent) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_free_ char *pp = NULL;
        int r;

        assert(path);

        r = search_and_fopen_nulstr(path, "re", NULL, conf_file_dirs, &f, &pp);
        if (r < 0) {
                if (ignore_enoent && r == -ENOENT)
                        return 0;

                return log_error_errno(r, "Failed to open %s: %m", path);
        }

        log_debug("apply: %s", pp);
        int tasks = 0;
        for (;;) {
                char *line = NULL;
                int k;

                k = read_stripped_line(f, LONG_LINE_MAX, &line);
                if (k < 0)
                        return log_error_errno(k, "Failed to read file '%s': %m", pp);
                if (k == 0)
                        break;

                if (isempty(line))
                        continue;
                if (strchr(COMMENTS, *line))
                        continue;

                exec_task(&tasks, &r, data, line, true);
        }

        for (int i = 0; i < max_tasks; ++i) {
                pthread_join(data[i].thread, NULL);
                RET_GATHER(r, data[i].result);
                free(data[i].line);
        }

        return r;
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-modules-load.service", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...] [CONFIGURATION FILE...]\n\n"
               "Loads statically configured kernel modules.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        return 1;
}

static void threads_data_cleanup(thread_data (*data)[max_tasks]) {
        for (int i = 0; i < max_tasks; ++i)
                sym_kmod_unref((*data)[i].ctx);
}

static int run(int argc, char *argv[]) {
        int r, k;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        log_setup();

        umask(0022);

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, PROC_CMDLINE_STRIP_RD_PREFIX);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        int tasks = 0;
        _cleanup_(threads_data_cleanup) thread_data data[MAX_TASKS] = { 0 };
        for (int i = 0; i < max_tasks; ++i) {
                r = module_setup_context(&data[i].ctx);
                if (r < 0)
                        return log_error_errno(r, "Failed to initialize libkmod context: %m");
        }

        r = 0;
        if (argc > optind) {
                for (int i = optind; i < argc; i++)
                        RET_GATHER(r, apply_file(data, argv[i], false));
        } else {
                _cleanup_strv_free_ char **files = NULL;
                STRV_FOREACH(i, arg_proc_cmdline_modules)
                        exec_task(&tasks, &r, data, *i, false);

                for (int i = 0; i < tasks; ++i) {
                        pthread_join(data[i].thread, NULL);
                        RET_GATHER(r, data[i].result);
                }

                k = conf_files_list_nulstr(&files, ".conf", NULL, 0, conf_file_dirs);
                if (k < 0)
                        return log_error_errno(k, "Failed to enumerate modules-load.d files: %m");

                STRV_FOREACH(fn, files)
                        RET_GATHER(r, apply_file(data, *fn, true));
        }

        return r;
}

DEFINE_MAIN_FUNCTION(run);
