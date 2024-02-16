/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <getopt.h>
#include <linux/vt.h>
#include <stdio.h>
#include <sys/ioctl.h>

#include "sd-id128.h"
#include "sd-journal.h"

#include "alloc-util.h"
#include "build.h"
#include "fd-util.h"
#include "fileio.h"
#include "io-util.h"
#include "log.h"
#include "logs-show.h"
#include "main-func.h"
#include "pretty-print.h"
#include "qrcode-util.h"
#include "sigbus.h"
#include "signal-util.h"
#include "sysctl-util.h"
#include "terminal-util.h"

static bool arg_continuous = false;

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-bsod", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s\n\n"
               "%sFilter the journal to fetch the first message from the\n"
               "current boot with an emergency log level and displays it\n"
               "as a string and a QR code.\n\n%s"
               "   -h --help            Show this help\n"
               "      --version         Show package version\n"
               "   -c --continuous      Make systemd-bsod wait continuously\n"
               "                        for changes in the journal\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               ansi_highlight(),
               ansi_normal(),
               link);

        return 0;
}

static int acquire_first_emergency_log_message(char **ret_message, char **ret_message_id) {
        _cleanup_(sd_journal_closep) sd_journal *j = NULL;
        _cleanup_free_ char *message = NULL, *message_id = NULL;
        const void *d;
        size_t l;
        int r;

        assert(ret_message);
        assert(ret_message_id);

        r = sd_journal_open(&j, SD_JOURNAL_LOCAL_ONLY | (arg_continuous ? 0 : SD_JOURNAL_ASSUME_IMMUTABLE));
        if (r < 0)
                return log_error_errno(r, "Failed to open journal: %m");

        r = add_match_this_boot(j, NULL);
        if (r < 0)
                return log_warning_errno(r, "Failed to add boot ID filter: %m");

        r = sd_journal_add_match(j, "_UID=0", 0);
        if (r < 0)
                return log_warning_errno(r, "Failed to add User ID filter: %m");

        assert_cc(0 == LOG_EMERG);
        r = sd_journal_add_match(j, "PRIORITY=0", 0);
        if (r < 0)
                return log_warning_errno(r, "Failed to add Emergency filter: %m");

        r = sd_journal_seek_head(j);
        if (r < 0)
                return log_error_errno(r, "Failed to seek to start of journal: %m");

        for (;;) {
                r = sd_journal_next(j);
                if (r < 0)
                        return log_error_errno(r, "Failed to read next journal entry: %m");
                if (r > 0)
                        break;

                if (!arg_continuous) {
                        log_debug("No emergency level entries in the journal");
                        *ret_message = NULL;
                        *ret_message_id = NULL;
                        return 0;
                }

                r = sd_journal_wait(j, UINT64_MAX);
                if (r < 0)
                        return log_error_errno(r, "Failed to wait for changes: %m");
        }

        r = sd_journal_get_data(j, "MESSAGE", &d, &l);
        if (r < 0)
                return log_error_errno(r, "Failed to read journal message: %m");

        message = memdup_suffix0((const char*)d + strlen("MESSAGE="), l - strlen("MESSAGE="));
        if (!message)
                return log_oom();

        r = sd_journal_get_data(j, "MESSAGE_ID", &d, &l);
        if (r == -ENOENT) {
                *ret_message = TAKE_PTR(message);
                *ret_message_id = NULL;
                return 0;
        }
        if (r < 0)
                return log_error_errno(r, "Failed to read message ID from journal: %m");

        d = memory_startswith(d, l, "MESSAGE_ID=");
        if (!d)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Wrong message ID.");

        message_id = memdup_suffix0(d, l - STRLEN("MESSAGE_ID="));
        if (!message_id)
                return log_oom();

        *ret_message = TAKE_PTR(message);
        *ret_message_id = TAKE_PTR(message_id);

        return 0;
}

static int find_next_free_vt(int fd, int *ret_free_vt, int *ret_original_vt) {
        struct vt_stat terminal_status;

        assert(fd >= 0);
        assert(ret_free_vt);
        assert(ret_original_vt);

        if (ioctl(fd, VT_GETSTATE, &terminal_status) < 0)
                return -errno;

        for (size_t i = 0; i < sizeof(terminal_status.v_state) * 8; i++)
                if ((terminal_status.v_state & (1 << i)) == 0) {
                        *ret_free_vt = i;
                        *ret_original_vt = terminal_status.v_active;
                        return 0;
                }

        return log_error_errno(SYNTHETIC_ERRNO(ENOTTY), "No free VT found: %m");
}

static int display_emergency_message_fullscreen(const char *message, const char *message_id) {
        int r, ret = 0, free_vt = 0, original_vt = 0;
        unsigned qr_code_start_row = 1, qr_code_start_column = 1;
        char tty[STRLEN("/dev/tty") + DECIMAL_STR_MAX(int) + 1];
        _cleanup_close_ int fd = -EBADF;
        _cleanup_fclose_ FILE *stream = NULL;
        _cleanup_free_ char *combined_message = NULL;
        char read_character_buffer = '\0';
        struct winsize w = {
                .ws_col = 80,
                .ws_row = 25,
        };

        assert(message);
        /* If we have the message ID, we should suffix it to the message we print */
        if (message_id) {
                combined_message = strjoin(message, "\nMessage ID: ", message_id);
                if (!combined_message)
                        return log_oom();
                message = combined_message;
        }

        fd = open_terminal("/dev/tty1", O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (fd < 0)
                return log_error_errno(fd, "Failed to open tty1: %m");

        r = find_next_free_vt(fd, &free_vt, &original_vt);
        if (r < 0)
                return log_error_errno(r, "Failed to find a free VT: %m");

        xsprintf(tty, "/dev/tty%d", free_vt + 1);

        r = open_terminal(tty, O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (r < 0)
                return log_error_errno(fd, "Failed to open tty: %m");

        close_and_replace(fd, r);

        if (ioctl(fd, TIOCGWINSZ, &w) < 0)
                log_warning_errno(errno, "Failed to fetch tty size, ignoring: %m");

        if (ioctl(fd, VT_ACTIVATE, free_vt + 1) < 0)
                return log_error_errno(errno, "Failed to activate tty: %m");

        r = loop_write(fd, ANSI_BACKGROUND_BLUE ANSI_HOME_CLEAR, SIZE_MAX);
        if (r < 0)
                log_warning_errno(r, "Failed to clear terminal, ignoring: %m");

        r = set_terminal_cursor_position(fd, 2, 4);
        if (r < 0)
                log_warning_errno(r, "Failed to move terminal cursor position, ignoring: %m");

        r = loop_write(fd, "The current boot has failed!", SIZE_MAX);
        if (r < 0) {
                ret = log_warning_errno(r, "Failed to write to terminal: %m");
                goto cleanup;
        }

        qr_code_start_row = w.ws_row * 3U / 5U;
        qr_code_start_column = w.ws_col * 3U / 4U;
        r = set_terminal_cursor_position(fd, 4, 4);
        if (r < 0)
                log_warning_errno(r, "Failed to move terminal cursor position, ignoring: %m");

        r = loop_write(fd, message, SIZE_MAX);
        if (r < 0) {
                ret = log_warning_errno(r, "Failed to write emergency message to terminal: %m");
                goto cleanup;
        }

        r = fdopen_independent(fd, "r+", &stream);
        if (r < 0) {
                ret = log_error_errno(errno, "Failed to open output file: %m");
                goto cleanup;
        }

        r = print_qrcode_full(stream,
                              "", message,
                              qr_code_start_row, qr_code_start_column,
                              w.ws_col,
                              w.ws_row);
        if (r < 0)
                log_warning_errno(r, "QR code could not be printed, ignoring: %m");

        r = set_terminal_cursor_position(fd, w.ws_row - 1, w.ws_col * 2U / 5U);
        if (r < 0)
                log_warning_errno(r, "Failed to move terminal cursor position, ignoring: %m");

        r = loop_write(fd, "Press any key to exit...", SIZE_MAX);
        if (r < 0) {
                ret = log_warning_errno(r, "Failed to write to terminal: %m");
                goto cleanup;
        }

        r = read_one_char(stream, &read_character_buffer, USEC_INFINITY, NULL);
        if (r < 0 && r != -EINTR)
                ret = log_error_errno(r, "Failed to read character: %m");

cleanup:
        if (ioctl(fd, VT_ACTIVATE, original_vt) < 0)
                return log_error_errno(errno, "Failed to switch back to original VT: %m");

        return ret;
}

static int parse_argv(int argc, char * argv[]) {

        enum {
                ARG_VERSION = 0x100,
        };

        static const struct option options[] = {
                { "help",       no_argument, NULL, 'h'         },
                { "version",    no_argument, NULL, ARG_VERSION },
                { "continuous", no_argument, NULL, 'c'         },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hc", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case 'c':
                        arg_continuous = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (optind < argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "%s takes no argument.",
                                       program_invocation_short_name);
        return 1;
}

static int run(int argc, char *argv[]) {
        /* Don't use SA_RESTART here, as we don't want to restart syscalls on signal
         * to get out of read_one_char() when needed */
        static const struct sigaction nop_sigaction = {
                .sa_handler = nop_signal_handler,
                .sa_flags = 0,
        };
        int r;
        _cleanup_free_ char *message = NULL, *message_id = NULL;

        log_open();
        log_parse_environment();

        sigbus_install();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = acquire_first_emergency_log_message(&message, &message_id);
        if (r < 0)
                return log_error_errno(r, "Failed to acquire first emergency log message: %m");

        if (!message) {
                log_debug("No emergency-level entries");
                return 0;
        }

        assert_se(sigaction_many(&nop_sigaction, SIGTERM, SIGINT) >= 0);

        r = display_emergency_message_fullscreen(message, message_id);
        if (r < 0)
                return log_error_errno(r, "Failed to display emergency message on terminal: %m");

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
