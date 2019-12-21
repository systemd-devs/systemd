/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include "selinux-access.h"

const char *mac_selinux_overhaul_pidone_class = "systemd_pidone";
const char *const mac_selinux_overhaul_pidone_permissions[] = {
        [MAC_SELINUX_PIDONE_STARTTRANSIENT] = "start_transient",
        [MAC_SELINUX_PIDONE_CLEARJOBS] = "clear_jobs",
        [MAC_SELINUX_PIDONE_RESETFAILED] = "reset_failed",
        [MAC_SELINUX_PIDONE_LISTUNITS] = "list_units",
        [MAC_SELINUX_PIDONE_LISTJOBS] = "list_jobs",
        [MAC_SELINUX_PIDONE_SUBSCRIBE] = "subscribe",
        [MAC_SELINUX_PIDONE_UNSUBSCRIBE] = "unsubscribe",
        [MAC_SELINUX_PIDONE_DUMP] = "dump",
        [MAC_SELINUX_PIDONE_RELOAD] = "reload",
        [MAC_SELINUX_PIDONE_REEXECUTE] = "reexecute",
        [MAC_SELINUX_PIDONE_EXIT] = "exit",
        [MAC_SELINUX_PIDONE_REBOOT] = "reboot",
        [MAC_SELINUX_PIDONE_POWEROFFORHALT] = "poweroff_or_halt",
        [MAC_SELINUX_PIDONE_KEXEC] = "kexec",
        [MAC_SELINUX_PIDONE_SWITCHROOT] = "switch_root",
        [MAC_SELINUX_PIDONE_SETENVIRONMENT] = "set_environment",
        [MAC_SELINUX_PIDONE_UNSETENVIRONMENT] = "unset_environment",
        [MAC_SELINUX_PIDONE_SETEXITCODE] = "set_exit_code",
        [MAC_SELINUX_PIDONE_LISTUNITFILES] = "list_unit_files",
        [MAC_SELINUX_PIDONE_STATEUNITFILE] = "state_unit_file",
        [MAC_SELINUX_PIDONE_GETDEFAULTTARGET] = "get_default_target",
        [MAC_SELINUX_PIDONE_SETDEFAULTTARGET] = "set_default_target",
        [MAC_SELINUX_PIDONE_PRESETALLUNITFILES] = "preset_all_unit_files",
        [MAC_SELINUX_PIDONE_RAWSET] = "raw_set",
        [MAC_SELINUX_PIDONE_RAWSTATUS] = "raw_status",
        [MAC_SELINUX_PIDONE_SETLOGTARGET] = "set_log_target",
        [MAC_SELINUX_PIDONE_SETLOGLEVEL] = "set_log_level",
        [MAC_SELINUX_PIDONE_GETUNITFILELINKS] = "get_unit_file_links",
        [MAC_SELINUX_PIDONE_ADDDEPENDENCYUNITFILES] = "add_dependency_unit_files",
        [MAC_SELINUX_PIDONE_GETDYNAMICUSERS] = "get_dynamic_users",
        [MAC_SELINUX_PIDONE_SETRUNTIMEWATCHDOG] = "set_runtime_watchdog",
        [MAC_SELINUX_PIDONE_SETSERVICEWATCHDOGS] = "set_service_watchdogs",
};
assert_cc(sizeof mac_selinux_overhaul_pidone_permissions / sizeof *mac_selinux_overhaul_pidone_permissions == MAC_SELINUX_PIDONE_PERMISSION_MAX);

const char *mac_selinux_overhaul_unit_class = "systemd_unit";
const char *const mac_selinux_overhaul_unit_permissions[] = {
        [MAC_SELINUX_UNIT_GETJOB] = "get_job",
        [MAC_SELINUX_UNIT_GETUNIT] = "get_unit",
        [MAC_SELINUX_UNIT_START] = "start",
        [MAC_SELINUX_UNIT_VERIFYACTIVE] = "verify_active",
        [MAC_SELINUX_UNIT_STOP] = "stop",
        [MAC_SELINUX_UNIT_RELOAD] = "reload",
        [MAC_SELINUX_UNIT_RESTART] = "restart",
        [MAC_SELINUX_UNIT_NOP] = "nop",
        [MAC_SELINUX_UNIT_CANCEL] = "cancel",
        [MAC_SELINUX_UNIT_ABANDON] = "abandon",
        [MAC_SELINUX_UNIT_KILL] = "kill",
        [MAC_SELINUX_UNIT_RESETFAILED] = "reset_failed",
        [MAC_SELINUX_UNIT_SETPROPERTIES] = "set_properties",
        [MAC_SELINUX_UNIT_REF] = "ref",
        [MAC_SELINUX_UNIT_CLEAN] = "clean",
        [MAC_SELINUX_UNIT_GETPROCESSES] = "get_processes",
        [MAC_SELINUX_UNIT_ATTACHPROCESSES] = "attach_processes",
        [MAC_SELINUX_UNIT_RAWSET] = "raw_set",
        [MAC_SELINUX_UNIT_RAWSTATUS] = "raw_status",
        [MAC_SELINUX_UNIT_GETWAITING_JOBS] = "get_waiting_jobs",
        [MAC_SELINUX_UNIT_UNREF] = "unref",
        [MAC_SELINUX_UNIT_LOADUNIT] = "load_unit",
};
assert_cc(sizeof mac_selinux_overhaul_unit_permissions / sizeof *mac_selinux_overhaul_unit_permissions == MAC_SELINUX_UNIT_PERMISSION_MAX);
