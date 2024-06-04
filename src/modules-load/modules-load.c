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

STATIC_DESTRUCTOR_REGISTER(arg_proc_cmdline_modules, strv_freep);

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

static void *worker_thread(void *arg) {
        int sock_fd = *(int *) arg;
        char buffer[32];
        ssize_t bytes_received;
        _cleanup_(sym_kmod_unrefp) struct kmod_ctx *ctx = NULL;
        int r = module_setup_context(&ctx);
        if (r < 0)
                log_error_errno(r, "Failed to initialize libkmod context: %m");

        while ((bytes_received = recv(sock_fd, buffer, sizeof(buffer), 0)) > 0) {
                buffer[bytes_received] = '\0';
                r = module_load_and_warn(ctx, buffer, true);
        }

        // Clean up and close the socket
        safe_close(sock_fd);
        return NULL;
}

static int apply_file(const int *sockets, const char *path, bool ignore_enoent) {
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
        for (;;) {
                _cleanup_free_ char *line = NULL;
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

                if (send(sockets[0], line, strlen(line), 0) == -1)
                        log_error_errno(errno, "send");
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

        _cleanup_close_pair_ int seq[2] = EBADF_PAIR;
        const int max_tasks = MAX(MIN(sysconf(_SC_NPROCESSORS_ONLN) / 2, 10), 2);
        _cleanup_free_ pthread_t *threads = malloc(sizeof(pthread_t) * max_tasks);
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, seq) == -1)
                log_error_errno(errno, "socketpair");

        // Create worker threads
        for (int i = 0; i < max_tasks; ++i)
                if (pthread_create(&threads[i], NULL, worker_thread, &seq[1]) != 0)
                        log_error_errno(errno, "pthread_create");

        r = 0;
        if (argc > optind) {
                for (int i = optind; i < argc; i++)
                        RET_GATHER(r, apply_file(seq, argv[i], false));
        } else {
                _cleanup_strv_free_ char **files = NULL;
                STRV_FOREACH(i, arg_proc_cmdline_modules)
                        if (send(seq[0], *i, strlen(*i), 0))
                                log_error_errno(errno, "send");

                k = conf_files_list_nulstr(&files, ".conf", NULL, 0, conf_file_dirs);
                if (k < 0)
                        return log_error_errno(k, "Failed to enumerate modules-load.d files: %m");

                STRV_FOREACH(fn, files)
                        RET_GATHER(r, apply_file(seq, *fn, true));
        }

        // Close the sending socket
        safe_close(seq[0]);

        // Wait for all threads to finish
        for (int i = 0; i < max_tasks; ++i)
                pthread_join(threads[i], NULL);

        return r;
}

DEFINE_MAIN_FUNCTION(run);
