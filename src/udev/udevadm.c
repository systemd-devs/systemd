/* SPDX-License-Identifier: GPL-2.0+ */

#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>

#include "alloc-util.h"
#include "selinux-util.h"
#include "string-util.h"
#include "terminal-util.h"
#include "udevadm.h"
#include "udev-util.h"
#include "verbs.h"
#include "util.h"

static int help(void) {
        static const char * short_descriptions[][2] = {
                { "info",         "Query sysfs or the udev database" },
                { "trigger",      "Request events from the kernel"   },
                { "settle",       "Wait for pending udev events"     },
                { "control",      "Control the udev daemon"          },
                { "monitor",      "Listen to kernel and udev events" },
                { "test",         "Test an event run"                },
                { "test-builtin", "Test a built-in command"          },
        };

        _cleanup_free_ char *link = NULL;
        size_t i;
        int r;

        r = terminal_urlify_man("udevadm", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s [--help] [--version] [--debug] COMMAND [COMMAND OPTIONS]\n\n"
               "Send control commands or test the device manager.\n\n"
               "Commands:\n"
               , program_invocation_short_name);

        for (i = 0; i < ELEMENTSOF(short_descriptions); i++)
                printf("  %-12s  %s\n", short_descriptions[i][0], short_descriptions[i][1]);

        printf("\nSee the %s for details.\n", link);
        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        static const struct option options[] = {
                { "debug",   no_argument, NULL, 'd' },
                { "help",    no_argument, NULL, 'h' },
                { "version", no_argument, NULL, 'V' },
                {}
        };
        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "+dhV", options, NULL)) >= 0)
                switch (c) {

                case 'd':
                        log_set_max_level(LOG_DEBUG);
                        break;

                case 'h':
                        return help();

                case 'V':
                        return print_version();

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        return 1; /* work to do */
}

static int version_main(int argc, char *argv[], void *userdata) {
        return print_version();
}

static int help_main(int argc, char *argv[], void *userdata) {
        return help();
}

static int udevadm_main(int argc, char *argv[]) {
        static const Verb verbs[] = {
                { "info",         VERB_ANY, VERB_ANY, 0, info_main    },
                { "trigger",      VERB_ANY, VERB_ANY, 0, trigger_main },
                { "settle",       VERB_ANY, VERB_ANY, 0, settle_main  },
                { "control",      VERB_ANY, VERB_ANY, 0, control_main },
                { "monitor",      VERB_ANY, VERB_ANY, 0, monitor_main },
                { "hwdb",         VERB_ANY, VERB_ANY, 0, hwdb_main    },
                { "test",         VERB_ANY, VERB_ANY, 0, test_main    },
                { "test-builtin", VERB_ANY, VERB_ANY, 0, builtin_main },
                { "version",      VERB_ANY, VERB_ANY, 0, version_main },
                { "help",         VERB_ANY, VERB_ANY, 0, help_main    },
                {}
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

int main(int argc, char *argv[]) {
        int r;

        udev_parse_config();
        log_parse_environment();
        log_open();
        log_set_max_level_realm(LOG_REALM_SYSTEMD, log_get_max_level());
        mac_selinux_init();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        r = udevadm_main(argc, argv);

finish:
        mac_selinux_finish();
        log_close();

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
