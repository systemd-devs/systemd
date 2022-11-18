/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parse-util.h"
#include "process-util.h"
#include "syslog-util.h"
#include "time-util.h"
#include "udevadm.h"
#include "udev-connection.h"
#include "udev-ctrl.h"
#include "udev-varlink.h"
#include "virt.h"

static int send_reload(UdevConnection *conn) {
        assert(conn);
        assert(conn->link || conn->uctrl);

        if (!conn->link)
                return udev_ctrl_send_reload(conn->uctrl);

        return udev_varlink_call(conn->link, "io.systemd.service.Reload", NULL, NULL);
}

static int send_set_log_level(UdevConnection *conn, int level) {
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
        int r;

        assert(conn);
        assert(conn->link || conn->uctrl);

        if (!conn->link)
                return udev_ctrl_send_set_log_level(conn->uctrl, level);

        r = json_build(&v, JSON_BUILD_OBJECT(JSON_BUILD_PAIR("level", JSON_BUILD_INTEGER(level))));
        if (r < 0)
                return log_error_errno(r, "Failed to build json object: %m");

        return udev_varlink_call(conn->link, "io.systemd.service.SetLogLevel", v, NULL);
}

static int send_stop_exec_queue(UdevConnection *conn) {
        assert(conn);
        assert(conn->link || conn->uctrl);

        if (!conn->link)
                return udev_ctrl_send_stop_exec_queue(conn->uctrl);

        return udev_varlink_call(conn->link, "io.systemd.udev.StopExecQueue", NULL, NULL);
}

static int send_start_exec_queue(UdevConnection *conn) {
        assert(conn);
        assert(conn->link || conn->uctrl);

        if (!conn->link)
                return udev_ctrl_send_start_exec_queue(conn->uctrl);

        return udev_varlink_call(conn->link, "io.systemd.udev.StartExecQueue", NULL, NULL);
}

static int help(void) {
        printf("%s control OPTION\n\n"
               "Control the udev daemon.\n\n"
               "  -h --help                Show this help\n"
               "  -V --version             Show package version\n"
               "  -e --exit                Instruct the daemon to cleanup and exit\n"
               "  -l --log-level=LEVEL     Set the udev log level for the daemon\n"
               "  -s --stop-exec-queue     Do not execute events, queue only\n"
               "  -S --start-exec-queue    Execute events, flush queue\n"
               "  -R --reload              Reload rules and databases\n"
               "  -p --property=KEY=VALUE  Set a global property for all events\n"
               "  -m --children-max=N      Maximum number of children\n"
               "     --ping                Wait for udev to respond to a ping message\n"
               "  -t --timeout=SECONDS     Maximum time to block for a reply\n",
               program_invocation_short_name);

        return 0;
}

int control_main(int argc, char *argv[], void *userdata) {
        _cleanup_(udev_connection_done) UdevConnection conn = {};
        usec_t timeout = 60 * USEC_PER_SEC;
        int c, r;

        enum {
                ARG_PING = 0x100,
        };

        static const struct option options[] = {
                { "exit",             no_argument,       NULL, 'e'      },
                { "log-level",        required_argument, NULL, 'l'      },
                { "log-priority",     required_argument, NULL, 'l'      }, /* for backward compatibility */
                { "stop-exec-queue",  no_argument,       NULL, 's'      },
                { "start-exec-queue", no_argument,       NULL, 'S'      },
                { "reload",           no_argument,       NULL, 'R'      },
                { "reload-rules",     no_argument,       NULL, 'R'      }, /* alias for -R */
                { "property",         required_argument, NULL, 'p'      },
                { "env",              required_argument, NULL, 'p'      }, /* alias for -p */
                { "children-max",     required_argument, NULL, 'm'      },
                { "ping",             no_argument,       NULL, ARG_PING },
                { "timeout",          required_argument, NULL, 't'      },
                { "version",          no_argument,       NULL, 'V'      },
                { "help",             no_argument,       NULL, 'h'      },
                {}
        };

        if (running_in_chroot() > 0) {
                log_info("Running in chroot, ignoring request.");
                return 0;
        }

        if (argc <= 1)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "This command expects one or more options.");

        while ((c = getopt_long(argc, argv, "el:sSRp:m:t:Vh", options, NULL)) >= 0)
                if (c == 't') {
                        r = parse_sec(optarg, &timeout);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse timeout value '%s': %m", optarg);
                        break;
                }
        optind = 0;

        r = udev_connection_init(&conn, timeout);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize udev control: %m");

        while ((c = getopt_long(argc, argv, "el:sSRp:m:t:Vh", options, NULL)) >= 0)
                switch (c) {
                case 'e':
                        r = udev_ctrl_send_exit(conn.uctrl);
                        if (r == -ENOANO)
                                log_warning("Cannot specify --exit after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send exit request: %m");
                        break;
                case 'l':
                        r = log_level_from_string(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse log level '%s': %m", optarg);

                        r = send_set_log_level(&conn, r);
                        if (r == -ENOANO)
                                log_warning("Cannot specify --log-level after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send request to set log level: %m");
                        break;
                case 's':
                        r = send_stop_exec_queue(&conn);
                        if (r == -ENOANO)
                                log_warning("Cannot specify --stop-exec-queue after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send request to stop exec queue: %m");
                        break;
                case 'S':
                        r = send_start_exec_queue(&conn);
                        if (r == -ENOANO)
                                log_warning("Cannot specify --start-exec-queue after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send request to start exec queue: %m");
                        break;
                case 'R':
                        r = send_reload(&conn);
                        if (r == -ENOANO)
                                log_warning("Cannot specify --reload after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send reload request: %m");
                        break;
                case 'p':
                        if (!strchr(optarg, '='))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "expect <KEY>=<value> instead of '%s'", optarg);

                        r = udev_ctrl_send_set_env(conn.uctrl, optarg);
                        if (r == -ENOANO)
                                log_warning("Cannot specify --property after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send request to update environment: %m");
                        break;
                case 'm': {
                        unsigned i;

                        r = safe_atou(optarg, &i);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse maximum number of children '%s': %m", optarg);

                        r = udev_ctrl_send_set_children_max(conn.uctrl, i);
                        if (r == -ENOANO)
                                log_warning("Cannot specify --children-max after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send request to set number of children: %m");
                        break;
                }
                case ARG_PING:
                        r = udev_connection_send_ping(&conn);
                        if (r == -ENOANO)
                                log_error("Cannot specify --ping after --exit, ignoring.");
                        else if (r < 0)
                                return log_error_errno(r, "Failed to send a ping message: %m");
                        break;
                case 't': /* Already handled, ignore */
                        break;
                case 'V':
                        return print_version();
                case 'h':
                        return help();
                case '?':
                        return -EINVAL;
                default:
                        assert_not_reached();
                }

        if (optind < argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Extraneous argument: %s", argv[optind]);

        r = udev_connection_wait(&conn);
        if (r < 0)
                return log_error_errno(r, "Failed to wait for daemon to reply: %m");

        return 0;
}
