/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sd-daemon.h"
#include "sd-event.h"

#include "bus-log-control-api.h"
#include "capability-util.h"
#include "daemon-util.h"
#include "main-func.h"
#include "mkdir-label.h"
#include "pretty-print.h"
#include "resolved-bus.h"
#include "resolved-conf.h"
#include "resolved-manager.h"
#include "resolved-resolv-conf.h"
#include "selinux-util.h"
#include "service-util.h"
#include "signal-util.h"
#include "terminal-util.h"
#include "util.h"

#include "user-util.h"

static int resolved_resolvconf_test(void) {
        _cleanup_(manager_freep) Manager *m = NULL;
        int r;

        r = manager_new(&m);
        if (r < 0)
                return log_error_errno(r, "Could not create manager: %m");

        if (m->dns_stub_listener_mode != DNS_STUB_LISTENER_NO)
                return 0;
        return -1;
}

static int resolved_resolvconf_start(void) {
        int r = resolved_resolvconf_test();
        if (r != -1)
                return r;
        return resolv_conf_start();
}

static int resolved_resolvconf_stop(void) {
        int r = resolved_resolvconf_test();
        if (r != -1)
                return r;
        return resolv_conf_stop();
}

static int help(const char *program_path, const char *service, const char *description, bool bus_introspect) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man(service, "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...]\n\n"
               "%s%s%s\n\n"
               "This program takes no positional arguments.\n\n"
               "%sOptions%s:\n"
               "  -h --help                 Show this help\n"
               "     --version              Show package version\n"
               "     --bus-introspect=PATH  Write D-Bus XML introspection data\n"
               "     --resolvconf-test      Test if /etc/resolv.conf is needed\n"
               "     --resolvconf-start     Create /etc/resolv.conf if needed\n"
               "     --resolvconf-stop      Remove /etc/resolv.conf if auto created\n"
               "\nSee the %s for details.\n"
               , program_path
               , ansi_highlight(), description, ansi_normal()
               , ansi_underline(), ansi_normal()
               , link
        );

        return 0; /* No further action */
}

static int resolved_service_parse_argv(
                const char *service,
                const char *description,
                const BusObjectImplementation* const* bus_objects,
                int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_BUS_INTROSPECT,
                ARG_RESOLVCONF_TEST,
                ARG_RESOLVCONF_START,
                ARG_RESOLVCONF_STOP,
        };

        static const struct option options[] = {
                { "help",             no_argument,       NULL, 'h'                  },
                { "version",          no_argument,       NULL, ARG_VERSION          },
                { "bus-introspect",   required_argument, NULL, ARG_BUS_INTROSPECT   },
                { "resolvconf-test",  no_argument,       NULL, ARG_RESOLVCONF_TEST  },
                { "resolvconf-start", no_argument,       NULL, ARG_RESOLVCONF_START },
                { "resolvconf-stop",  no_argument,       NULL, ARG_RESOLVCONF_STOP  },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch (c) {

                case 'h':
                        return help(argv[0], service, description, bus_objects);

                case ARG_VERSION:
                        return version();

                case ARG_BUS_INTROSPECT:
                        return bus_introspect_implementations(
                                        stdout,
                                        optarg,
                                        bus_objects);

                case ARG_RESOLVCONF_TEST:
                        return resolved_resolvconf_test();

                case ARG_RESOLVCONF_START:
                        return resolved_resolvconf_start();

                case ARG_RESOLVCONF_STOP:
                        return resolved_resolvconf_stop();

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (optind < argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "This program takes no arguments.");

        return 1; /* Further action */
}

static int run(int argc, char *argv[]) {
        _cleanup_(manager_freep) Manager *m = NULL;
        _unused_ _cleanup_(notify_on_cleanup) const char *notify_stop = NULL;
        int r;

        log_setup();

        r = resolved_service_parse_argv("systemd-resolved.service",
                               "Provide name resolution with caching using DNS, mDNS, LLMNR.",
                               BUS_IMPLEMENTATIONS(&manager_object,
                                                   &log_control_object),
                               argc, argv);
        if (r <= 0)
                return r;

        umask(0022);

        r = mac_selinux_init();
        if (r < 0)
                return r;

        /* Drop privileges, but only if we have been started as root. If we are not running as root we assume most
         * privileges are already dropped and we can't create our directory. */
        if (getuid() == 0) {
                const char *user = "systemd-resolve";
                uid_t uid;
                gid_t gid;

                r = get_user_creds(&user, &uid, &gid, NULL, NULL, 0);
                if (r < 0)
                        return log_error_errno(r, "Cannot resolve user name %s: %m", user);

                /* As we're root, we can create the directory where resolv.conf will live */
                r = mkdir_safe_label("/run/systemd/resolve", 0755, uid, gid, MKDIR_WARN_MODE);
                if (r < 0)
                        return log_error_errno(r, "Could not create runtime directory: %m");

                /* Drop privileges, but keep three caps. Note that we drop two of those too, later on (see below) */
                r = drop_privileges(uid, gid,
                                    (UINT64_C(1) << CAP_NET_RAW)|          /* needed for SO_BINDTODEVICE */
                                    (UINT64_C(1) << CAP_NET_BIND_SERVICE)| /* needed to bind on port 53 */
                                    (UINT64_C(1) << CAP_SETPCAP)           /* needed in order to drop the caps later */);
                if (r < 0)
                        return log_error_errno(r, "Failed to drop privileges: %m");
        }

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGTERM, SIGINT, SIGUSR1, SIGUSR2, SIGRTMIN+1, -1) >= 0);

        r = manager_new(&m);
        if (r < 0)
                return log_error_errno(r, "Could not create manager: %m");

        r = manager_start(m);
        if (r < 0)
                return log_error_errno(r, "Failed to start manager: %m");

        /* Write finish default resolv.conf to avoid a dangling symlink */
        (void) manager_write_resolv_conf(m);

        (void) manager_check_resolv_conf(m);

        /* Let's drop the remaining caps now */
        r = capability_bounding_set_drop((UINT64_C(1) << CAP_NET_RAW), true);
        if (r < 0)
                return log_error_errno(r, "Failed to drop remaining caps: %m");

        notify_stop = notify_start(NOTIFY_READY, NOTIFY_STOPPING);

        r = sd_event_loop(m->event);
        if (r < 0)
                return log_error_errno(r, "Event loop failed: %m");

        /* send queries on shutdown to other servers */
        manager_symlink_stub_to_uplink_resolv_conf();

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
