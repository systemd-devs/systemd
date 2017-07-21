/***
  This file is part of systemd.

  Copyright 2013 Tom Gundersen <teg@jklm.no>

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

#include "sd-daemon.h"
#include "sd-event.h"

#include "capability-util.h"
#include "networkd-conf.h"
#include "networkd-manager.h"
#include "signal-util.h"
#include "user-util.h"

static int check_privileges(void) {
        uint64_t required_caps, current_caps;
        int r;

        required_caps = (1ULL << CAP_NET_ADMIN) |
                (1ULL << CAP_NET_BIND_SERVICE) |
                (1ULL << CAP_NET_BROADCAST) |
                (1ULL << CAP_NET_RAW);

        if (geteuid() == 0 || getegid() == 0) {
                const char *user = "systemd-network";
                uid_t uid;
                gid_t gid;

                r = get_user_creds(&user, &uid, &gid, NULL, NULL);
                if (r < 0)
                        return log_error_errno(r, "Cannot resolve user name %s: %m", user);

                r = drop_privileges(uid, gid, required_caps);
                if (r < 0)
                        return log_error_errno(r, "Failed to drop privileges: %m");

                return 0;
        }

        r = get_effective_caps(&current_caps);
        if (r < 0)
                return log_error_errno(r, "Failed to get current capabilities: %m");

        if ((current_caps & required_caps) != required_caps) {
                log_error("Missing required capabilities. This process requires "
                          "CAP_NET_ADMIN, CAP_NET_BIND_SERVICE, CAP_NET_BROADCAST, and CAP_NET_RAW");
                return -EPERM;
        }

        if (current_caps != required_caps) {
                log_warning("This process has unnecessary capabilities. Try to drop them.");

                /* Try to drop unnecessary caps */
                r = capability_bounding_set_drop(required_caps, true);
                if (r < 0)
                        return log_error_errno(r, "Failed to drop capabilities: %m");
        }

        return 0;
}

int main(int argc, char *argv[]) {
        sd_event *event = NULL;
        _cleanup_manager_free_ Manager *m = NULL;
        int r;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        if (argc != 1) {
                log_error("This program takes no arguments.");
                r = -EINVAL;
                goto out;
        }

        r = check_privileges();
        if (r < 0)
                goto out;

        /* Always create the directories people can create inotify
         * watches in. */
        r = mkdir_p_label("/run/systemd/netif/leases", 0755);
        if (r < 0)
                log_warning_errno(r, "Could not create runtime directory 'leases': %m");

        r = mkdir_p_label("/run/systemd/netif/links", 0755);
        if (r < 0)
                log_warning_errno(r, "Could not create runtime directory 'links': %m");

        r = mkdir_p_label("/run/systemd/netif/lldp", 0755);
        if (r < 0)
                log_warning_errno(r, "Could not create runtime directory 'lldp': %m");

        assert_se(sigprocmask_many(SIG_BLOCK, NULL, SIGTERM, SIGINT, -1) >= 0);

        r = sd_event_default(&event);
        if (r < 0)
                goto out;

        sd_event_set_watchdog(event, true);
        sd_event_add_signal(event, NULL, SIGTERM, NULL, NULL);
        sd_event_add_signal(event, NULL, SIGINT, NULL, NULL);

        r = manager_new(&m, event);
        if (r < 0) {
                log_error_errno(r, "Could not create manager: %m");
                goto out;
        }

        r = manager_connect_bus(m);
        if (r < 0) {
                log_error_errno(r, "Could not connect to bus: %m");
                goto out;
        }

        r = manager_parse_config_file(m);
        if (r < 0)
                log_warning_errno(r, "Failed to parse configuration file: %m");

        r = manager_load_config(m);
        if (r < 0) {
                log_error_errno(r, "Could not load configuration files: %m");
                goto out;
        }

        r = manager_rtnl_enumerate_links(m);
        if (r < 0) {
                log_error_errno(r, "Could not enumerate links: %m");
                goto out;
        }

        r = manager_rtnl_enumerate_addresses(m);
        if (r < 0) {
                log_error_errno(r, "Could not enumerate addresses: %m");
                goto out;
        }

        r = manager_rtnl_enumerate_routes(m);
        if (r < 0) {
                log_error_errno(r, "Could not enumerate routes: %m");
                goto out;
        }

        r = manager_start(m);
        if (r < 0) {
                log_error_errno(r, "Could not start manager: %m");
                goto out;
        }

        log_info("Enumeration completed");

        sd_notify(false,
                  "READY=1\n"
                  "STATUS=Processing requests...");

        r = sd_event_loop(event);
        if (r < 0) {
                log_error_errno(r, "Event loop failed: %m");
                goto out;
        }
out:
        sd_notify(false,
                  "STOPPING=1\n"
                  "STATUS=Shutting down...");

        sd_event_unref(event);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
