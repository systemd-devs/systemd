/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "allow-bind.h"
#include "load-fragment.h"
#include "manager.h"
#include "process-util.h"
#include "rm-rf.h"
#include "service.h"
#include "strv.h"
#include "tests.h"
#include "unit.h"
#include "virt.h"

static int test_allow_bind(Manager *m, const char *unit_name, const char *port, char **allowed_ports) {
        _cleanup_free_ char *exec_start = NULL;
        _cleanup_(unit_freep) Unit *u = NULL;
        CGroupContext *cc = NULL;
        char **allow_port;
        int cld_code, r;

        assert_se(u = unit_new(m, sizeof(Service)));
        assert_se(unit_add_name(u, unit_name) == 0);
        assert_se(cc = unit_get_cgroup_context(u));

        STRV_FOREACH(allow_port, allowed_ports) {
                r = config_parse_allow_bind_ports(
                                u->id, "filename", 1, "Service", 1, "AllowBindPorts", 0,
                                *allow_port, &cc->allow_bind_ports, u);
                if (r < 0)
                        return log_unit_error_errno(u, r, "Failed to parse AllowBindPorts: %m");
        }

        exec_start = strjoin("-timeout --preserve-status -sSIGTERM 1s /bin/nc -l -p ", port, " -vv");
        r = config_parse_exec(u->id, "filename", 1, "Service", 1, "ExecStart",
                        SERVICE_EXEC_START, exec_start, SERVICE(u)->exec_command, u);
        if (r < 0)
                return log_error_errno(r, "Failed to parse ExecStart");

        SERVICE(u)->type = SERVICE_ONESHOT;
        u->load_state = UNIT_LOADED;

        r = unit_start(u);
        if (r < 0)
                return log_error_errno(r, "Unit start failed %m");

        while (!IN_SET(SERVICE(u)->state, SERVICE_DEAD, SERVICE_FAILED)) {
                r = sd_event_run(m->event, UINT64_MAX);
                if (r < 0)
                        return log_error_errno(errno, "Event run failed %m");
        }

        cld_code = SERVICE(u)->exec_command[SERVICE_EXEC_START]->exec_status.code;
        if (cld_code != CLD_EXITED) {
                r = -SYNTHETIC_ERRNO(EBUSY);
                return log_error_errno(r, "ExecStart didn't exited, code='%s'", sigchld_code_to_string(cld_code));
        }

        if (SERVICE(u)->state != SERVICE_DEAD) {
                r = -SYNTHETIC_ERRNO(EBUSY);
                return log_error_errno(r, "Service is not dead");
        }

        return 0;
}

int main(int argc, char *argv[]) {
        _cleanup_(rm_rf_physical_and_freep) char *runtime_dir = NULL;
        _cleanup_(manager_freep) Manager *m = NULL;
        _cleanup_free_ char *unit_dir = NULL;
        struct rlimit rl;
        int r;

        test_setup_logging(LOG_DEBUG);

        if (detect_container() > 0)
                return log_tests_skipped("test-bpf fails inside LXC and Docker containers: https://github.com/systemd/systemd/issues/9666");

        if (getuid() != 0)
                return log_tests_skipped("not running as root");

        assert_se(getrlimit(RLIMIT_MEMLOCK, &rl) >= 0);
        rl.rlim_cur = rl.rlim_max = MAX(rl.rlim_max, CAN_MEMLOCK_SIZE);
        (void) setrlimit(RLIMIT_MEMLOCK, &rl);

        if (!can_memlock())
                return log_tests_skipped("Can't use mlock(), skipping.");

        r = allow_bind_supported();
        if (r <= 0)
                return log_tests_skipped("Allow bind based on BPF hooks is not supported: %m");

        r = enter_cgroup_subroot(NULL);
        if (r == -ENOMEDIUM)
                return log_tests_skipped("cgroupfs not available");

        assert_se(get_testdata_dir("units", &unit_dir) >= 0);
        assert_se(set_unit_path(unit_dir) >= 0);
        assert_se(runtime_dir = setup_fake_runtime_dir());

        assert_se(manager_new(UNIT_FILE_USER, MANAGER_TEST_RUN_BASIC, &m) >= 0);
        assert_se(manager_startup(m, NULL, NULL) >= 0);

        assert_se(test_allow_bind(m, "allow_bind_test.service", "2000", STRV_MAKE("2000")) >= 0);
        assert_se(test_allow_bind(m, "allow_bind_test.service", "2000", STRV_MAKE("2001")) >= 0);
        assert_se(test_allow_bind(m, "allow_bind_test.service", "6666", STRV_MAKE("6666", "6667")) >= 0);
        assert_se(test_allow_bind(m, "allow_bind_test.service", "6666", STRV_MAKE("6667", "6668", "")) >= 0);
        assert_se(test_allow_bind(m, "allow_bind_test.service", "7777", STRV_MAKE_EMPTY) >= 0);
        assert_se(test_allow_bind(m, "allow_bind_test.service", "8888", STRV_MAKE("none")) >= 0);

        return 0;
}
