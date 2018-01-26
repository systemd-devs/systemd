/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <getopt.h>
#include <stdio.h>

#include "sd-bus.h"
#include "sd-event.h"

#include "alloc-util.h"
#include "bus-error.h"
#include "bus-unit-util.h"
#include "bus-util.h"
#include "calendarspec.h"
#include "env-util.h"
#include "fd-util.h"
#include "format-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "ptyfwd.h"
#include "signal-util.h"
#include "spawn-polkit-agent.h"
#include "strv.h"
#include "terminal-util.h"
#include "unit-def.h"
#include "unit-name.h"
#include "user-util.h"

static bool arg_ask_password = true;
static bool arg_scope = false;
static bool arg_remain_after_exit = false;
static bool arg_no_block = false;
static bool arg_wait = false;
static const char *arg_unit = NULL;
static const char *arg_description = NULL;
static const char *arg_slice = NULL;
static bool arg_send_sighup = false;
static BusTransport arg_transport = BUS_TRANSPORT_LOCAL;
static const char *arg_host = NULL;
static bool arg_user = false;
static const char *arg_service_type = NULL;
static const char *arg_exec_user = NULL;
static const char *arg_exec_group = NULL;
static int arg_nice = 0;
static bool arg_nice_set = false;
static char **arg_environment = NULL;
static char **arg_property = NULL;
static enum {
        ARG_STDIO_NONE,      /* The default, as it is for normal services, stdin connected to /dev/null, and stdout+stderr to the journal */
        ARG_STDIO_PTY,       /* Interactive behaviour, requested by --pty: we allocate a pty and connect it to the TTY we are invoked from */
        ARG_STDIO_DIRECT,    /* Directly pass our stdin/stdout/stderr to the activated service, useful for usage in shell pipelines, requested by --pipe */
        ARG_STDIO_AUTO,      /* If --pipe and --pty are used together we use --pty when invoked on a TTY, and --pipe otherwise */
} arg_stdio = ARG_STDIO_NONE;
static char **arg_path_property = NULL;
static char **arg_socket_property = NULL;
static char **arg_timer_property = NULL;
static bool with_timer = false;
static bool arg_quiet = false;
static bool arg_aggressive_gc = false;

static void help(void) {
        printf("%s [OPTIONS...] {COMMAND} [ARGS...]\n\n"
               "Run the specified command in a transient scope or service.\n\n"
               "  -h --help                       Show this help\n"
               "     --version                    Show package version\n"
               "     --no-ask-password            Do not prompt for password\n"
               "     --user                       Run as user unit\n"
               "  -H --host=[USER@]HOST           Operate on remote host\n"
               "  -M --machine=CONTAINER          Operate on local container\n"
               "     --scope                      Run this as scope rather than service\n"
               "     --unit=UNIT                  Run under the specified unit name\n"
               "  -p --property=NAME=VALUE        Set service or scope unit property\n"
               "     --description=TEXT           Description for unit\n"
               "     --slice=SLICE                Run in the specified slice\n"
               "     --no-block                   Do not wait until operation finished\n"
               "  -r --remain-after-exit          Leave service around until explicitly stopped\n"
               "     --wait                       Wait until service stopped again\n"
               "     --send-sighup                Send SIGHUP when terminating\n"
               "     --service-type=TYPE          Service type\n"
               "     --uid=USER                   Run as system user\n"
               "     --gid=GROUP                  Run as system group\n"
               "     --nice=NICE                  Nice level\n"
               "  -E --setenv=NAME=VALUE          Set environment\n"
               "  -t --pty                        Run service on pseudo TTY as STDIN/STDOUT/\n"
               "                                  STDERR\n"
               "  -P --pipe                       Pass STDIN/STDOUT/STDERR directly to service\n"
               "  -q --quiet                      Suppress information messages during runtime\n"
               "  -G --collect                    Unload unit after it ran, even when failed\n\n"
               "Path options:\n"
               "     --path-property=NAME=VALUE   Set path unit property\n\n"
               "Socket options:\n"
               "     --socket-property=NAME=VALUE Set socket unit property\n\n"
               "Timer options:\n"
               "     --on-active=SECONDS          Run after SECONDS delay\n"
               "     --on-boot=SECONDS            Run SECONDS after machine was booted up\n"
               "     --on-startup=SECONDS         Run SECONDS after systemd activation\n"
               "     --on-unit-active=SECONDS     Run SECONDS after the last activation\n"
               "     --on-unit-inactive=SECONDS   Run SECONDS after the last deactivation\n"
               "     --on-calendar=SPEC           Realtime timer\n"
               "     --timer-property=NAME=VALUE  Set timer unit property\n"
               , program_invocation_short_name);
}

static int add_timer_property(const char *name, const char *val) {
        char *p;

        assert(name);
        assert(val);

        p = strjoin(name, "=", val);
        if (!p)
                return log_oom();

        if (strv_consume(&arg_timer_property, p) < 0)
                return log_oom();

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_USER,
                ARG_SYSTEM,
                ARG_SCOPE,
                ARG_UNIT,
                ARG_DESCRIPTION,
                ARG_SLICE,
                ARG_SEND_SIGHUP,
                ARG_SERVICE_TYPE,
                ARG_EXEC_USER,
                ARG_EXEC_GROUP,
                ARG_NICE,
                ARG_ON_ACTIVE,
                ARG_ON_BOOT,
                ARG_ON_STARTUP,
                ARG_ON_UNIT_ACTIVE,
                ARG_ON_UNIT_INACTIVE,
                ARG_ON_CALENDAR,
                ARG_TIMER_PROPERTY,
                ARG_PATH_PROPERTY,
                ARG_SOCKET_PROPERTY,
                ARG_NO_BLOCK,
                ARG_NO_ASK_PASSWORD,
                ARG_WAIT,
        };

        static const struct option options[] = {
                { "help",              no_argument,       NULL, 'h'                  },
                { "version",           no_argument,       NULL, ARG_VERSION          },
                { "user",              no_argument,       NULL, ARG_USER             },
                { "system",            no_argument,       NULL, ARG_SYSTEM           },
                { "scope",             no_argument,       NULL, ARG_SCOPE            },
                { "unit",              required_argument, NULL, ARG_UNIT             },
                { "description",       required_argument, NULL, ARG_DESCRIPTION      },
                { "slice",             required_argument, NULL, ARG_SLICE            },
                { "remain-after-exit", no_argument,       NULL, 'r'                  },
                { "send-sighup",       no_argument,       NULL, ARG_SEND_SIGHUP      },
                { "host",              required_argument, NULL, 'H'                  },
                { "machine",           required_argument, NULL, 'M'                  },
                { "service-type",      required_argument, NULL, ARG_SERVICE_TYPE     },
                { "wait",              no_argument,       NULL, ARG_WAIT             },
                { "uid",               required_argument, NULL, ARG_EXEC_USER        },
                { "gid",               required_argument, NULL, ARG_EXEC_GROUP       },
                { "nice",              required_argument, NULL, ARG_NICE             },
                { "setenv",            required_argument, NULL, 'E'                  },
                { "property",          required_argument, NULL, 'p'                  },
                { "tty",               no_argument,       NULL, 't'                  }, /* deprecated alias */
                { "pty",               no_argument,       NULL, 't'                  },
                { "pipe",              no_argument,       NULL, 'P'                  },
                { "quiet",             no_argument,       NULL, 'q'                  },
                { "on-active",         required_argument, NULL, ARG_ON_ACTIVE        },
                { "on-boot",           required_argument, NULL, ARG_ON_BOOT          },
                { "on-startup",        required_argument, NULL, ARG_ON_STARTUP       },
                { "on-unit-active",    required_argument, NULL, ARG_ON_UNIT_ACTIVE   },
                { "on-unit-inactive",  required_argument, NULL, ARG_ON_UNIT_INACTIVE },
                { "on-calendar",       required_argument, NULL, ARG_ON_CALENDAR      },
                { "timer-property",    required_argument, NULL, ARG_TIMER_PROPERTY   },
                { "path-property",     required_argument, NULL, ARG_PATH_PROPERTY    },
                { "socket-property",   required_argument, NULL, ARG_SOCKET_PROPERTY  },
                { "no-block",          no_argument,       NULL, ARG_NO_BLOCK         },
                { "no-ask-password",   no_argument,       NULL, ARG_NO_ASK_PASSWORD  },
                { "collect",           no_argument,       NULL, 'G'                  },
                {},
        };

        bool with_trigger = false;
        int r, c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "+hrH:M:E:p:tPqG", options, NULL)) >= 0)

                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_NO_ASK_PASSWORD:
                        arg_ask_password = false;
                        break;

                case ARG_USER:
                        arg_user = true;
                        break;

                case ARG_SYSTEM:
                        arg_user = false;
                        break;

                case ARG_SCOPE:
                        arg_scope = true;
                        break;

                case ARG_UNIT:
                        arg_unit = optarg;
                        break;

                case ARG_DESCRIPTION:
                        arg_description = optarg;
                        break;

                case ARG_SLICE:
                        arg_slice = optarg;
                        break;

                case ARG_SEND_SIGHUP:
                        arg_send_sighup = true;
                        break;

                case 'r':
                        arg_remain_after_exit = true;
                        break;

                case 'H':
                        arg_transport = BUS_TRANSPORT_REMOTE;
                        arg_host = optarg;
                        break;

                case 'M':
                        arg_transport = BUS_TRANSPORT_MACHINE;
                        arg_host = optarg;
                        break;

                case ARG_SERVICE_TYPE:
                        arg_service_type = optarg;
                        break;

                case ARG_EXEC_USER:
                        arg_exec_user = optarg;
                        break;

                case ARG_EXEC_GROUP:
                        arg_exec_group = optarg;
                        break;

                case ARG_NICE:
                        r = parse_nice(optarg, &arg_nice);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse nice value: %s", optarg);

                        arg_nice_set = true;
                        break;

                case 'E':
                        if (strv_extend(&arg_environment, optarg) < 0)
                                return log_oom();

                        break;

                case 'p':
                        if (strv_extend(&arg_property, optarg) < 0)
                                return log_oom();

                        break;

                case 't': /* --pty */
                        if (IN_SET(arg_stdio, ARG_STDIO_DIRECT, ARG_STDIO_AUTO)) /* if --pipe is already used, upgrade to auto mode */
                                arg_stdio = ARG_STDIO_AUTO;
                        else
                                arg_stdio = ARG_STDIO_PTY;
                        break;

                case 'P': /* --pipe */
                        if (IN_SET(arg_stdio, ARG_STDIO_PTY, ARG_STDIO_AUTO)) /* If --pty is already used, upgrade to auto mode */
                                arg_stdio = ARG_STDIO_AUTO;
                        else
                                arg_stdio = ARG_STDIO_DIRECT;
                        break;

                case 'q':
                        arg_quiet = true;
                        break;

                case ARG_ON_ACTIVE:
                        r = add_timer_property("OnActiveSec", optarg);
                        if (r < 0)
                                return r;

                        with_timer = true;
                        break;

                case ARG_ON_BOOT:
                        r = add_timer_property("OnBootSec", optarg);
                        if (r < 0)
                                return r;

                        with_timer = true;
                        break;

                case ARG_ON_STARTUP:
                        r = add_timer_property("OnStartupSec", optarg);
                        if (r < 0)
                                return r;

                        with_timer = true;
                        break;

                case ARG_ON_UNIT_ACTIVE:
                        r = add_timer_property("OnUnitActiveSec", optarg);
                        if (r < 0)
                                return r;

                        with_timer = true;
                        break;

                case ARG_ON_UNIT_INACTIVE:
                        r = add_timer_property("OnUnitInactiveSec", optarg);
                        if (r < 0)
                                return r;

                        with_timer = true;
                        break;

                case ARG_ON_CALENDAR:
                        r = add_timer_property("OnCalendar", optarg);
                        if (r < 0)
                                return r;

                        with_timer = true;
                        break;

                case ARG_TIMER_PROPERTY:

                        if (strv_extend(&arg_timer_property, optarg) < 0)
                                return log_oom();

                        with_timer = with_timer ||
                                !!startswith(optarg, "OnActiveSec=") ||
                                !!startswith(optarg, "OnBootSec=") ||
                                !!startswith(optarg, "OnStartupSec=") ||
                                !!startswith(optarg, "OnUnitActiveSec=") ||
                                !!startswith(optarg, "OnUnitInactiveSec=") ||
                                !!startswith(optarg, "OnCalendar=");
                        break;

                case ARG_PATH_PROPERTY:

                        if (strv_extend(&arg_path_property, optarg) < 0)
                                return log_oom();

                        break;

                case ARG_SOCKET_PROPERTY:

                        if (strv_extend(&arg_socket_property, optarg) < 0)
                                return log_oom();

                        break;

                case ARG_NO_BLOCK:
                        arg_no_block = true;
                        break;

                case ARG_WAIT:
                        arg_wait = true;
                        break;

                case 'G':
                        arg_aggressive_gc = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        with_trigger = !!arg_path_property || !!arg_socket_property || with_timer;

        /* currently, only single trigger (path, socket, timer) unit can be created simultaneously */
        if ((int) !!arg_path_property + (int) !!arg_socket_property + (int) with_timer > 1) {
                log_error("Only single trigger (path, socket, timer) unit can be created.");
                return -EINVAL;
        }

        if (arg_stdio == ARG_STDIO_AUTO) {
                /* If we both --pty and --pipe are specified we'll automatically pick --pty if we are connected fully
                 * to a TTY and pick direct fd passing otherwise. This way, we automatically adapt to usage in a shell
                 * pipeline, but we are neatly interactive with tty-level isolation otherwise. */
                arg_stdio = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && isatty(STDERR_FILENO) ?
                        ARG_STDIO_PTY :
                        ARG_STDIO_DIRECT;
        }

        if ((optind >= argc) && (!arg_unit || !with_trigger)) {
                log_error("Command line to execute required.");
                return -EINVAL;
        }

        if (arg_user && arg_transport != BUS_TRANSPORT_LOCAL) {
                log_error("Execution in user context is not supported on non-local systems.");
                return -EINVAL;
        }

        if (arg_scope && arg_transport != BUS_TRANSPORT_LOCAL) {
                log_error("Scope execution is not supported on non-local systems.");
                return -EINVAL;
        }

        if (arg_scope && (arg_remain_after_exit || arg_service_type)) {
                log_error("--remain-after-exit and --service-type= are not supported in --scope mode.");
                return -EINVAL;
        }

        if (arg_stdio != ARG_STDIO_NONE && (with_trigger || arg_scope)) {
                log_error("--pty/--pipe is not compatible in timer or --scope mode.");
                return -EINVAL;
        }

        if (arg_stdio != ARG_STDIO_NONE && arg_transport == BUS_TRANSPORT_REMOTE) {
                log_error("--pty/--pipe is only supported when connecting to the local system or containers.");
                return -EINVAL;
        }

        if (arg_stdio != ARG_STDIO_NONE && arg_no_block) {
                log_error("--pty/--pipe is not compatible with --no-block.");
                return -EINVAL;
        }

        if (arg_scope && with_trigger) {
                log_error("Path, socket or timer options are not supported in --scope mode.");
                return -EINVAL;
        }

        if (arg_timer_property && !with_timer) {
                log_error("--timer-property= has no effect without any other timer options.");
                return -EINVAL;
        }

        if (arg_wait) {
                if (arg_no_block) {
                        log_error("--wait may not be combined with --no-block.");
                        return -EINVAL;
                }

                if (with_trigger) {
                        log_error("--wait may not be combined with path, socket or timer operations.");
                        return -EINVAL;
                }

                if (arg_scope) {
                        log_error("--wait may not be combined with --scope.");
                        return -EINVAL;
                }
        }

        return 1;
}

static int transient_unit_set_properties(sd_bus_message *m, UnitType t, char **properties) {
        int r;

        r = sd_bus_message_append(m, "(sv)", "Description", "s", arg_description);
        if (r < 0)
                return bus_log_create_error(r);

        if (arg_aggressive_gc) {
                r = sd_bus_message_append(m, "(sv)", "CollectMode", "s", "inactive-or-failed");
                if (r < 0)
                        return bus_log_create_error(r);
        }

        r = bus_append_unit_property_assignment_many(m, t, properties);
        if (r < 0)
                return r;

        return 0;
}

static int transient_cgroup_set_properties(sd_bus_message *m) {
        int r;
        assert(m);

        if (!isempty(arg_slice)) {
                _cleanup_free_ char *slice = NULL;

                r = unit_name_mangle_with_suffix(arg_slice, UNIT_NAME_NOGLOB, ".slice", &slice);
                if (r < 0)
                        return log_error_errno(r, "Failed to mangle name '%s': %m", arg_slice);

                r = sd_bus_message_append(m, "(sv)", "Slice", "s", slice);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        return 0;
}

static int transient_kill_set_properties(sd_bus_message *m) {
        int r;

        assert(m);

        if (arg_send_sighup) {
                r = sd_bus_message_append(m, "(sv)", "SendSIGHUP", "b", arg_send_sighup);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        return 0;
}

static int transient_service_set_properties(sd_bus_message *m, char **argv, const char *pty_path) {
        bool send_term = false;
        int r;

        assert(m);

        r = transient_unit_set_properties(m, UNIT_SERVICE, arg_property);
        if (r < 0)
                return r;

        r = transient_kill_set_properties(m);
        if (r < 0)
                return r;

        r = transient_cgroup_set_properties(m);
        if (r < 0)
                return r;

        if (arg_wait || arg_stdio != ARG_STDIO_NONE) {
                r = sd_bus_message_append(m, "(sv)", "AddRef", "b", 1);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_remain_after_exit) {
                r = sd_bus_message_append(m, "(sv)", "RemainAfterExit", "b", arg_remain_after_exit);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_service_type) {
                r = sd_bus_message_append(m, "(sv)", "Type", "s", arg_service_type);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_exec_user) {
                r = sd_bus_message_append(m, "(sv)", "User", "s", arg_exec_user);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_exec_group) {
                r = sd_bus_message_append(m, "(sv)", "Group", "s", arg_exec_group);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (arg_nice_set) {
                r = sd_bus_message_append(m, "(sv)", "Nice", "i", arg_nice);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        if (pty_path) {
                r = sd_bus_message_append(m,
                                          "(sv)(sv)(sv)(sv)",
                                          "StandardInput", "s", "tty",
                                          "StandardOutput", "s", "tty",
                                          "StandardError", "s", "tty",
                                          "TTYPath", "s", pty_path);
                if (r < 0)
                        return bus_log_create_error(r);

                send_term = true;

        } else if (arg_stdio == ARG_STDIO_DIRECT) {
                r = sd_bus_message_append(m,
                                          "(sv)(sv)(sv)",
                                          "StandardInputFileDescriptor", "h", STDIN_FILENO,
                                          "StandardOutputFileDescriptor", "h", STDOUT_FILENO,
                                          "StandardErrorFileDescriptor", "h", STDERR_FILENO);
                if (r < 0)
                        return bus_log_create_error(r);

                send_term = isatty(STDIN_FILENO) || isatty(STDOUT_FILENO) || isatty(STDERR_FILENO);
        }

        if (send_term) {
                const char *e;

                e = getenv("TERM");
                if (e) {
                        char *n;

                        n = strjoina("TERM=", e);
                        r = sd_bus_message_append(m,
                                                  "(sv)",
                                                  "Environment", "as", 1, n);
                        if (r < 0)
                                return bus_log_create_error(r);
                }
        }

        if (!strv_isempty(arg_environment)) {
                r = sd_bus_message_open_container(m, 'r', "sv");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "s", "Environment");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'v', "as");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, arg_environment);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        /* Exec container */
        {
                r = sd_bus_message_open_container(m, 'r', "sv");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "s", "ExecStart");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'v', "a(sasb)");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'a', "(sasb)");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'r', "sasb");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "s", argv[0]);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_strv(m, argv);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "b", false);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        return 0;
}

static int transient_scope_set_properties(sd_bus_message *m) {
        int r;

        assert(m);

        r = transient_unit_set_properties(m, UNIT_SCOPE, arg_property);
        if (r < 0)
                return r;

        r = transient_kill_set_properties(m);
        if (r < 0)
                return r;

        r = transient_cgroup_set_properties(m);
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "(sv)", "PIDs", "au", 1, (uint32_t) getpid_cached());
        if (r < 0)
                return bus_log_create_error(r);

        return 0;
}

static int transient_timer_set_properties(sd_bus_message *m) {
        int r;

        assert(m);

        r = transient_unit_set_properties(m, UNIT_TIMER, arg_timer_property);
        if (r < 0)
                return r;

        /* Automatically clean up our transient timers */
        r = sd_bus_message_append(m, "(sv)", "RemainAfterElapse", "b", false);
        if (r < 0)
                return bus_log_create_error(r);

        return 0;
}

static int make_unit_name(sd_bus *bus, UnitType t, char **ret) {
        const char *unique, *id;
        char *p;
        int r;

        assert(bus);
        assert(t >= 0);
        assert(t < _UNIT_TYPE_MAX);

        r = sd_bus_get_unique_name(bus, &unique);
        if (r < 0) {
                sd_id128_t rnd;

                /* We couldn't get the unique name, which is a pretty
                 * common case if we are connected to systemd
                 * directly. In that case, just pick a random uuid as
                 * name */

                r = sd_id128_randomize(&rnd);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate random run unit name: %m");

                if (asprintf(ret, "run-r" SD_ID128_FORMAT_STR ".%s", SD_ID128_FORMAT_VAL(rnd), unit_type_to_string(t)) < 0)
                        return log_oom();

                return 0;
        }

        /* We managed to get the unique name, then let's use that to
         * name our transient units. */

        id = startswith(unique, ":1.");
        if (!id) {
                log_error("Unique name %s has unexpected format.", unique);
                return -EINVAL;
        }

        p = strjoin("run-u", id, ".", unit_type_to_string(t));
        if (!p)
                return log_oom();

        *ret = p;
        return 0;
}

typedef struct RunContext {
        sd_bus *bus;
        sd_event *event;
        PTYForward *forward;
        sd_bus_slot *match;

        /* The exit data of the unit */
        char *active_state;
        uint64_t inactive_exit_usec;
        uint64_t inactive_enter_usec;
        char *result;
        uint64_t cpu_usage_nsec;
        uint64_t ip_ingress_bytes;
        uint64_t ip_egress_bytes;
        uint32_t exit_code;
        uint32_t exit_status;
} RunContext;

static void run_context_free(RunContext *c) {
        assert(c);

        c->forward = pty_forward_free(c->forward);
        c->match = sd_bus_slot_unref(c->match);
        c->bus = sd_bus_unref(c->bus);
        c->event = sd_event_unref(c->event);

        free(c->active_state);
        free(c->result);
}

static void run_context_check_done(RunContext *c) {
        bool done;

        assert(c);

        if (c->match)
                done = STRPTR_IN_SET(c->active_state, "inactive", "failed");
        else
                done = true;

        if (c->forward && done) /* If the service is gone, it's time to drain the output */
                done = pty_forward_drain(c->forward);

        if (done)
                sd_event_exit(c->event, EXIT_SUCCESS);
}

static int run_context_update(RunContext *c, const char *path) {

        static const struct bus_properties_map map[] = {
                { "ActiveState",                      "s", NULL, offsetof(RunContext, active_state)        },
                { "InactiveExitTimestampMonotonic",   "t", NULL, offsetof(RunContext, inactive_exit_usec)  },
                { "InactiveEnterTimestampMonotonic",  "t", NULL, offsetof(RunContext, inactive_enter_usec) },
                { "Result",                           "s", NULL, offsetof(RunContext, result)              },
                { "ExecMainCode",                     "i", NULL, offsetof(RunContext, exit_code)           },
                { "ExecMainStatus",                   "i", NULL, offsetof(RunContext, exit_status)         },
                { "CPUUsageNSec",                     "t", NULL, offsetof(RunContext, cpu_usage_nsec)      },
                { "IPIngressBytes",                   "t", NULL, offsetof(RunContext, ip_ingress_bytes)    },
                { "IPEgressBytes",                    "t", NULL, offsetof(RunContext, ip_egress_bytes)     },
                {}
        };

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        r = bus_map_all_properties(c->bus,
                                   "org.freedesktop.systemd1",
                                   path,
                                   map,
                                   &error,
                                   c);
        if (r < 0) {
                sd_event_exit(c->event, EXIT_FAILURE);
                return log_error_errno(r, "Failed to query unit state: %s", bus_error_message(&error, r));
        }

        run_context_check_done(c);
        return 0;
}

static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *error) {
        RunContext *c = userdata;

        assert(m);
        assert(c);

        return run_context_update(c, sd_bus_message_get_path(m));
}

static int pty_forward_handler(PTYForward *f, int rcode, void *userdata) {
        RunContext *c = userdata;

        assert(f);

        if (rcode < 0) {
                sd_event_exit(c->event, EXIT_FAILURE);
                return log_error_errno(rcode, "Error on PTY forwarding logic: %m");
        }

        run_context_check_done(c);
        return 0;
}

static int start_transient_service(
                sd_bus *bus,
                char **argv,
                int *retval) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(bus_wait_for_jobs_freep) BusWaitForJobs *w = NULL;
        _cleanup_free_ char *service = NULL, *pty_path = NULL;
        _cleanup_close_ int master = -1;
        int r;

        assert(bus);
        assert(argv);
        assert(retval);

        if (arg_stdio == ARG_STDIO_PTY) {

                if (arg_transport == BUS_TRANSPORT_LOCAL) {
                        master = posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC|O_NDELAY);
                        if (master < 0)
                                return log_error_errno(errno, "Failed to acquire pseudo tty: %m");

                        r = ptsname_malloc(master, &pty_path);
                        if (r < 0)
                                return log_error_errno(r, "Failed to determine tty name: %m");

                        if (unlockpt(master) < 0)
                                return log_error_errno(errno, "Failed to unlock tty: %m");

                } else if (arg_transport == BUS_TRANSPORT_MACHINE) {
                        _cleanup_(sd_bus_unrefp) sd_bus *system_bus = NULL;
                        _cleanup_(sd_bus_message_unrefp) sd_bus_message *pty_reply = NULL;
                        const char *s;

                        r = sd_bus_default_system(&system_bus);
                        if (r < 0)
                                return log_error_errno(r, "Failed to connect to system bus: %m");

                        r = sd_bus_call_method(system_bus,
                                               "org.freedesktop.machine1",
                                               "/org/freedesktop/machine1",
                                               "org.freedesktop.machine1.Manager",
                                               "OpenMachinePTY",
                                               &error,
                                               &pty_reply,
                                               "s", arg_host);
                        if (r < 0) {
                                log_error("Failed to get machine PTY: %s", bus_error_message(&error, -r));
                                return r;
                        }

                        r = sd_bus_message_read(pty_reply, "hs", &master, &s);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        master = fcntl(master, F_DUPFD_CLOEXEC, 3);
                        if (master < 0)
                                return log_error_errno(errno, "Failed to duplicate master fd: %m");

                        pty_path = strdup(s);
                        if (!pty_path)
                                return log_oom();
                } else
                        assert_not_reached("Can't allocate tty via ssh");
        }

        if (!arg_no_block) {
                r = bus_wait_for_jobs_new(bus, &w);
                if (r < 0)
                        return log_error_errno(r, "Could not watch jobs: %m");
        }

        if (arg_unit) {
                r = unit_name_mangle_with_suffix(arg_unit, UNIT_NAME_NOGLOB, ".service", &service);
                if (r < 0)
                        return log_error_errno(r, "Failed to mangle unit name: %m");
        } else {
                r = make_unit_name(bus, UNIT_SERVICE, &service);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "StartTransientUnit");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_set_allow_interactive_authorization(m, arg_ask_password);
        if (r < 0)
                return bus_log_create_error(r);

        /* Name and mode */
        r = sd_bus_message_append(m, "ss", service, "fail");
        if (r < 0)
                return bus_log_create_error(r);

        /* Properties */
        r = sd_bus_message_open_container(m, 'a', "(sv)");
        if (r < 0)
                return bus_log_create_error(r);

        r = transient_service_set_properties(m, argv, pty_path);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return bus_log_create_error(r);

        /* Auxiliary units */
        r = sd_bus_message_append(m, "a(sa(sv))", 0);
        if (r < 0)
                return bus_log_create_error(r);

        polkit_agent_open_if_enabled(arg_transport, arg_ask_password);

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to start transient service unit: %s", bus_error_message(&error, r));

        if (w) {
                const char *object;

                r = sd_bus_message_read(reply, "o", &object);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = bus_wait_for_jobs_one(w, object, arg_quiet);
                if (r < 0)
                        return r;
        }

        if (!arg_quiet)
                log_info("Running as unit: %s", service);

        if (arg_wait || arg_stdio != ARG_STDIO_NONE) {
                _cleanup_(run_context_free) RunContext c = {
                        .cpu_usage_nsec = NSEC_INFINITY,
                        .ip_ingress_bytes = UINT64_MAX,
                        .ip_egress_bytes = UINT64_MAX,
                        .inactive_exit_usec = USEC_INFINITY,
                        .inactive_enter_usec = USEC_INFINITY,
                };
                _cleanup_free_ char *path = NULL;

                c.bus = sd_bus_ref(bus);

                r = sd_event_default(&c.event);
                if (r < 0)
                        return log_error_errno(r, "Failed to get event loop: %m");

                if (master >= 0) {
                        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGWINCH, SIGTERM, SIGINT, -1) >= 0);
                        (void) sd_event_add_signal(c.event, NULL, SIGINT, NULL, NULL);
                        (void) sd_event_add_signal(c.event, NULL, SIGTERM, NULL, NULL);

                        if (!arg_quiet)
                                log_info("Press ^] three times within 1s to disconnect TTY.");

                        r = pty_forward_new(c.event, master, PTY_FORWARD_IGNORE_INITIAL_VHANGUP, &c.forward);
                        if (r < 0)
                                return log_error_errno(r, "Failed to create PTY forwarder: %m");

                        pty_forward_set_handler(c.forward, pty_forward_handler, &c);

                        /* Make sure to process any TTY events before we process bus events */
                        (void) pty_forward_set_priority(c.forward, SD_EVENT_PRIORITY_IMPORTANT);
                }

                path = unit_dbus_path_from_name(service);
                if (!path)
                        return log_oom();

                r = sd_bus_match_signal_async(
                                bus,
                                &c.match,
                                "org.freedesktop.systemd1",
                                path,
                                "org.freedesktop.DBus.Properties",
                                "PropertiesChanged",
                                on_properties_changed, NULL, &c);
                if (r < 0)
                        return log_error_errno(r, "Failed to request properties changed signal match: %m");

                r = sd_bus_attach_event(bus, c.event, SD_EVENT_PRIORITY_NORMAL);
                if (r < 0)
                        return log_error_errno(r, "Failed to attach bus to event loop: %m");

                r = run_context_update(&c, path);
                if (r < 0)
                        return r;

                r = sd_event_loop(c.event);
                if (r < 0)
                        return log_error_errno(r, "Failed to run event loop: %m");

                if (c.forward) {
                        char last_char = 0;

                        r = pty_forward_get_last_char(c.forward, &last_char);
                        if (r >= 0 && !arg_quiet && last_char != '\n')
                                fputc('\n', stdout);
                }

                if (arg_wait && !arg_quiet) {

                        /* Explicitly destroy the PTY forwarder, so that the PTY device is usable again, in its
                         * original settings (i.e. proper line breaks), so that we can show the summary in a pretty
                         * way. */
                        c.forward = pty_forward_free(c.forward);

                        if (!isempty(c.result))
                                log_info("Finished with result: %s", strna(c.result));

                        if (c.exit_code == CLD_EXITED)
                                log_info("Main processes terminated with: code=%s/status=%i", sigchld_code_to_string(c.exit_code), c.exit_status);
                        else if (c.exit_code > 0)
                                log_info("Main processes terminated with: code=%s/status=%s", sigchld_code_to_string(c.exit_code), signal_to_string(c.exit_status));

                        if (c.inactive_enter_usec > 0 && c.inactive_enter_usec != USEC_INFINITY &&
                            c.inactive_exit_usec > 0 && c.inactive_exit_usec != USEC_INFINITY &&
                            c.inactive_enter_usec > c.inactive_exit_usec) {
                                char ts[FORMAT_TIMESPAN_MAX];
                                log_info("Service runtime: %s", format_timespan(ts, sizeof(ts), c.inactive_enter_usec - c.inactive_exit_usec, USEC_PER_MSEC));
                        }

                        if (c.cpu_usage_nsec != NSEC_INFINITY) {
                                char ts[FORMAT_TIMESPAN_MAX];
                                log_info("CPU time consumed: %s", format_timespan(ts, sizeof(ts), (c.cpu_usage_nsec + NSEC_PER_USEC - 1) / NSEC_PER_USEC, USEC_PER_MSEC));
                        }

                        if (c.ip_ingress_bytes != UINT64_MAX) {
                                char bytes[FORMAT_BYTES_MAX];
                                log_info("IP traffic received: %s", format_bytes(bytes, sizeof(bytes), c.ip_ingress_bytes));
                        }
                        if (c.ip_egress_bytes != UINT64_MAX) {
                                char bytes[FORMAT_BYTES_MAX];
                                log_info("IP traffic sent: %s", format_bytes(bytes, sizeof(bytes), c.ip_egress_bytes));
                        }
                }

                /* Try to propagate the service's return value */
                if (c.result && STR_IN_SET(c.result, "success", "exit-code") && c.exit_code == CLD_EXITED)
                        *retval = c.exit_status;
                else
                        *retval = EXIT_FAILURE;
        }

        return 0;
}

static int start_transient_scope(
                sd_bus *bus,
                char **argv) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL, *reply = NULL;
        _cleanup_(bus_wait_for_jobs_freep) BusWaitForJobs *w = NULL;
        _cleanup_strv_free_ char **env = NULL, **user_env = NULL;
        _cleanup_free_ char *scope = NULL;
        const char *object = NULL;
        int r;

        assert(bus);
        assert(argv);

        r = bus_wait_for_jobs_new(bus, &w);
        if (r < 0)
                return log_oom();

        if (arg_unit) {
                r = unit_name_mangle_with_suffix(arg_unit, UNIT_NAME_NOGLOB, ".scope", &scope);
                if (r < 0)
                        return log_error_errno(r, "Failed to mangle scope name: %m");
        } else {
                r = make_unit_name(bus, UNIT_SCOPE, &scope);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "StartTransientUnit");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_set_allow_interactive_authorization(m, arg_ask_password);
        if (r < 0)
                return bus_log_create_error(r);

        /* Name and Mode */
        r = sd_bus_message_append(m, "ss", scope, "fail");
        if (r < 0)
                return bus_log_create_error(r);

        /* Properties */
        r = sd_bus_message_open_container(m, 'a', "(sv)");
        if (r < 0)
                return bus_log_create_error(r);

        r = transient_scope_set_properties(m);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return bus_log_create_error(r);

        /* Auxiliary units */
        r = sd_bus_message_append(m, "a(sa(sv))", 0);
        if (r < 0)
                return bus_log_create_error(r);

        polkit_agent_open_if_enabled(arg_transport, arg_ask_password);

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0) {
                log_error("Failed to start transient scope unit: %s", bus_error_message(&error, -r));
                return r;
        }

        if (arg_nice_set) {
                if (setpriority(PRIO_PROCESS, 0, arg_nice) < 0)
                        return log_error_errno(errno, "Failed to set nice level: %m");
        }

        if (arg_exec_group) {
                gid_t gid;

                r = get_group_creds(&arg_exec_group, &gid);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve group %s: %m", arg_exec_group);

                if (setresgid(gid, gid, gid) < 0)
                        return log_error_errno(errno, "Failed to change GID to " GID_FMT ": %m", gid);
        }

        if (arg_exec_user) {
                const char *home, *shell;
                uid_t uid;
                gid_t gid;

                r = get_user_creds_clean(&arg_exec_user, &uid, &gid, &home, &shell);
                if (r < 0)
                        return log_error_errno(r, "Failed to resolve user %s: %m", arg_exec_user);

                if (home) {
                        r = strv_extendf(&user_env, "HOME=%s", home);
                        if (r < 0)
                                return log_oom();
                }

                if (shell) {
                        r = strv_extendf(&user_env, "SHELL=%s", shell);
                        if (r < 0)
                                return log_oom();
                }

                r = strv_extendf(&user_env, "USER=%s", arg_exec_user);
                if (r < 0)
                        return log_oom();

                r = strv_extendf(&user_env, "LOGNAME=%s", arg_exec_user);
                if (r < 0)
                        return log_oom();

                if (!arg_exec_group) {
                        if (setresgid(gid, gid, gid) < 0)
                                return log_error_errno(errno, "Failed to change GID to " GID_FMT ": %m", gid);
                }

                if (setresuid(uid, uid, uid) < 0)
                        return log_error_errno(errno, "Failed to change UID to " UID_FMT ": %m", uid);
        }

        env = strv_env_merge(3, environ, user_env, arg_environment);
        if (!env)
                return log_oom();

        r = sd_bus_message_read(reply, "o", &object);
        if (r < 0)
                return bus_log_parse_error(r);

        r = bus_wait_for_jobs_one(w, object, arg_quiet);
        if (r < 0)
                return r;

        if (!arg_quiet)
                log_info("Running scope as unit: %s", scope);

        execvpe(argv[0], argv, env);

        return log_error_errno(errno, "Failed to execute: %m");
}

static int start_transient_trigger(
                sd_bus *bus,
                char **argv,
                const char *suffix) {

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL, *reply = NULL;
        _cleanup_(bus_wait_for_jobs_freep) BusWaitForJobs *w = NULL;
        _cleanup_free_ char *trigger = NULL, *service = NULL;
        const char *object = NULL;
        int r;

        assert(bus);
        assert(argv);

        r = bus_wait_for_jobs_new(bus, &w);
        if (r < 0)
                return log_oom();

        if (arg_unit) {
                switch (unit_name_to_type(arg_unit)) {

                case UNIT_SERVICE:
                        service = strdup(arg_unit);
                        if (!service)
                                return log_oom();

                        r = unit_name_change_suffix(service, suffix, &trigger);
                        if (r < 0)
                                return log_error_errno(r, "Failed to change unit suffix: %m");
                        break;

                case UNIT_TIMER:
                        trigger = strdup(arg_unit);
                        if (!trigger)
                                return log_oom();

                        r = unit_name_change_suffix(trigger, ".service", &service);
                        if (r < 0)
                                return log_error_errno(r, "Failed to change unit suffix: %m");
                        break;

                default:
                        r = unit_name_mangle_with_suffix(arg_unit, UNIT_NAME_NOGLOB, ".service", &service);
                        if (r < 0)
                                return log_error_errno(r, "Failed to mangle unit name: %m");

                        r = unit_name_mangle_with_suffix(arg_unit, UNIT_NAME_NOGLOB, suffix, &trigger);
                        if (r < 0)
                                return log_error_errno(r, "Failed to mangle unit name: %m");

                        break;
                }
        } else {
                r = make_unit_name(bus, UNIT_SERVICE, &service);
                if (r < 0)
                        return r;

                r = unit_name_change_suffix(service, suffix, &trigger);
                if (r < 0)
                        return log_error_errno(r, "Failed to change unit suffix: %m");
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "StartTransientUnit");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_set_allow_interactive_authorization(m, arg_ask_password);
        if (r < 0)
                return bus_log_create_error(r);

        /* Name and Mode */
        r = sd_bus_message_append(m, "ss", trigger, "fail");
        if (r < 0)
                return bus_log_create_error(r);

        /* Properties */
        r = sd_bus_message_open_container(m, 'a', "(sv)");
        if (r < 0)
                return bus_log_create_error(r);

        if (streq(suffix, ".path"))
                r = transient_unit_set_properties(m, UNIT_PATH, arg_path_property);
        else if (streq(suffix, ".socket"))
                r = transient_unit_set_properties(m, UNIT_SOCKET, arg_socket_property);
        else if (streq(suffix, ".timer"))
                r = transient_timer_set_properties(m);
        else
                assert_not_reached("Invalid suffix");
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_open_container(m, 'a', "(sa(sv))");
        if (r < 0)
                return bus_log_create_error(r);

        if (!strv_isempty(argv)) {
                r = sd_bus_message_open_container(m, 'r', "sa(sv)");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(m, "s", service);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_open_container(m, 'a', "(sv)");
                if (r < 0)
                        return bus_log_create_error(r);

                r = transient_service_set_properties(m, argv, NULL);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return bus_log_create_error(r);

        polkit_agent_open_if_enabled(arg_transport, arg_ask_password);

        r = sd_bus_call(bus, m, 0, &error, &reply);
        if (r < 0) {
                log_error("Failed to start transient %s unit: %s", suffix + 1, bus_error_message(&error, -r));
                return r;
        }

        r = sd_bus_message_read(reply, "o", &object);
        if (r < 0)
                return bus_log_parse_error(r);

        r = bus_wait_for_jobs_one(w, object, arg_quiet);
        if (r < 0)
                return r;

        if (!arg_quiet) {
                log_info("Running %s as unit: %s", suffix + 1, trigger);
                if (argv[0])
                        log_info("Will run service as unit: %s", service);
        }

        return 0;
}

int main(int argc, char* argv[]) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_free_ char *description = NULL, *command = NULL;
        int r, retval = EXIT_SUCCESS;

        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        if (argc > optind && arg_transport == BUS_TRANSPORT_LOCAL) {
                /* Patch in an absolute path */

                r = find_binary(argv[optind], &command);
                if (r < 0) {
                        log_error_errno(r, "Failed to find executable %s: %m", argv[optind]);
                        goto finish;
                }

                argv[optind] = command;
        }

        if (!arg_description) {
                description = strv_join(argv + optind, " ");
                if (!description) {
                        r = log_oom();
                        goto finish;
                }

                if (arg_unit && isempty(description)) {
                        r = free_and_strdup(&description, arg_unit);
                        if (r < 0)
                                goto finish;
                }

                arg_description = description;
        }

        /* If --wait is used connect via the bus, unconditionally, as ref/unref is not supported via the limited direct
         * connection */
        if (arg_wait || arg_stdio != ARG_STDIO_NONE)
                r = bus_connect_transport(arg_transport, arg_host, arg_user, &bus);
        else
                r = bus_connect_transport_systemd(arg_transport, arg_host, arg_user, &bus);
        if (r < 0) {
                log_error_errno(r, "Failed to create bus connection: %m");
                goto finish;
        }

        if (arg_scope)
                r = start_transient_scope(bus, argv + optind);
        else if (arg_path_property)
                r = start_transient_trigger(bus, argv + optind, ".path");
        else if (arg_socket_property)
                r = start_transient_trigger(bus, argv + optind, ".socket");
        else if (with_timer)
                r = start_transient_trigger(bus, argv + optind, ".timer");
        else
                r = start_transient_service(bus, argv + optind, &retval);

finish:
        strv_free(arg_environment);
        strv_free(arg_property);
        strv_free(arg_path_property);
        strv_free(arg_socket_property);
        strv_free(arg_timer_property);

        return r < 0 ? EXIT_FAILURE : retval;
}
