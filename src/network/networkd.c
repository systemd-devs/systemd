/* SPDX-License-Identifier: LGPL-2.1+ */

#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sd-daemon.h"
#include "sd-event.h"

#include "capability-util.h"
#include "daemon-util.h"
#include "main-func.h"
#include "mkdir.h"
#include "networkd-conf.h"
#include "networkd-manager.h"
#include "signal-util.h"
#include "user-util.h"

static int run(int argc, char *argv[]) {
        _cleanup_(notify_on_cleanup) const char *notify_message = NULL;
        _cleanup_(manager_freep) Manager *m = NULL;
        const char *namespace, *runtime_directory, *p;
        int r;

        log_setup_service();

        umask(0022);

        if (argc > 2)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "This program takes one or no arguments.");

        namespace = argc > 1 ? empty_to_null(argv[1]) : NULL;

        runtime_directory = getenv("RUNTIME_DIRECTORY");
        if (!runtime_directory) {
                if (namespace)
                        runtime_directory = strjoina("/run/systemd/netif.", namespace);
                else
                        runtime_directory = "/run/systemd/netif";
        }

        /* Drop privileges, but only if we have been started as root. If we are not running as root we assume all
         * privileges are already dropped and we can't create our runtime directory. */
        if (geteuid() == 0) {
                const char *user = "systemd-network";
                uid_t uid;
                gid_t gid;

                r = get_user_creds(&user, &uid, &gid, NULL, NULL, 0);
                if (r < 0)
                        return log_error_errno(r, "Cannot resolve user name %s: %m", user);

                /* Create runtime directory. This is not necessary when networkd is
                 * started with "RuntimeDirectory=systemd/netif", or after
                 * systemd-tmpfiles-setup.service. */
                r = mkdir_safe_label(runtime_directory, 0755, uid, gid, MKDIR_WARN_MODE);
                if (r < 0)
                        log_warning_errno(r, "Could not create runtime directory: %m");

                r = drop_privileges(uid, gid,
                                    (1ULL << CAP_NET_ADMIN) |
                                    (1ULL << CAP_NET_BIND_SERVICE) |
                                    (1ULL << CAP_NET_BROADCAST) |
                                    (1ULL << CAP_NET_RAW));
                if (r < 0)
                        return log_error_errno(r, "Failed to drop privileges: %m");
        }

        /* Always create the directories people can create inotify watches in.
         * It is necessary to create the following subdirectories after drop_privileges()
         * to support old kernels not supporting AmbientCapabilities=. */
        p = strjoina(runtime_directory, "/links");
        r = mkdir_safe_label(p, 0755, UID_INVALID, GID_INVALID, MKDIR_WARN_MODE);
        if (r < 0)
                log_warning_errno(r, "Could not create runtime directory 'links': %m");

        p = strjoina(runtime_directory, "/leases");
        r = mkdir_safe_label(p, 0755, UID_INVALID, GID_INVALID, MKDIR_WARN_MODE);
        if (r < 0)
                log_warning_errno(r, "Could not create runtime directory 'leases': %m");

        p = strjoina(runtime_directory, "/lldp");
        r = mkdir_safe_label(p, 0755, UID_INVALID, GID_INVALID, MKDIR_WARN_MODE);
        if (r < 0)
                log_warning_errno(r, "Could not create runtime directory 'lldp': %m");

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGTERM, SIGINT, -1) >= 0);

        r = manager_new(&m, namespace, true);
        if (r < 0)
                return log_error_errno(r, "Could not create manager: %m");

        r = manager_connect_bus(m);
        if (r < 0)
                return log_error_errno(r, "Could not connect to bus: %m");

        r = manager_parse_config_file(m);
        if (r < 0)
                log_warning_errno(r, "Failed to parse configuration file: %m");

        r = manager_load_config(m);
        if (r < 0)
                return log_error_errno(r, "Could not load configuration files: %m");

        r = manager_rtnl_enumerate_links(m);
        if (r < 0)
                return log_error_errno(r, "Could not enumerate links: %m");

        r = manager_rtnl_enumerate_addresses(m);
        if (r < 0)
                return log_error_errno(r, "Could not enumerate addresses: %m");

        r = manager_rtnl_enumerate_neighbors(m);
        if (r < 0)
                return log_error_errno(r, "Could not enumerate neighbors: %m");

        r = manager_rtnl_enumerate_routes(m);
        if (r < 0)
                return log_error_errno(r, "Could not enumerate routes: %m");

        r = manager_rtnl_enumerate_rules(m);
        if (r < 0)
                return log_error_errno(r, "Could not enumerate rules: %m");

        r = manager_rtnl_enumerate_nexthop(m);
        if (r < 0)
                return log_error_errno(r, "Could not enumerate nexthop: %m");

        r = manager_start(m);
        if (r < 0)
                return log_error_errno(r, "Could not start manager: %m");

        log_info("Enumeration completed");

        notify_message = notify_start(NOTIFY_READY, NOTIFY_STOPPING);

        r = sd_event_loop(m->event);
        if (r < 0)
                return log_error_errno(r, "Event loop failed: %m");

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
