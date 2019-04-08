/* SPDX-License-Identifier: LGPL-2.1+ */

#include "alloc-util.h"
#include "bus-label.h"
#include "string-table.h"
#include "unit-def.h"
#include "unit-name.h"

char *unit_dbus_path_from_name(const char *name) {
        _cleanup_free_ char *e = NULL;

        assert(name);

        e = bus_label_escape(name);
        if (!e)
                return NULL;

        return strappend("/org/freedesktop/systemd1/unit/", e);
}

int unit_name_from_dbus_path(const char *path, char **name) {
        const char *e;
        char *n;

        e = startswith(path, "/org/freedesktop/systemd1/unit/");
        if (!e)
                return -EINVAL;

        n = bus_label_unescape(e);
        if (!n)
                return -ENOMEM;

        *name = n;
        return 0;
}

const char* unit_dbus_interface_from_type(UnitType t) {

        static const char *const table[_UNIT_TYPE_MAX] = {
                [UNIT_SERVICE] = "org.freedesktop.systemd1.Service",
                [UNIT_SOCKET] = "org.freedesktop.systemd1.Socket",
                [UNIT_TARGET] = "org.freedesktop.systemd1.Target",
                [UNIT_DEVICE] = "org.freedesktop.systemd1.Device",
                [UNIT_MOUNT] = "org.freedesktop.systemd1.Mount",
                [UNIT_AUTOMOUNT] = "org.freedesktop.systemd1.Automount",
                [UNIT_SWAP] = "org.freedesktop.systemd1.Swap",
                [UNIT_TIMER] = "org.freedesktop.systemd1.Timer",
                [UNIT_PATH] = "org.freedesktop.systemd1.Path",
                [UNIT_SLICE] = "org.freedesktop.systemd1.Slice",
                [UNIT_SCOPE] = "org.freedesktop.systemd1.Scope",
        };

        if (t < 0)
                return NULL;
        if (t >= _UNIT_TYPE_MAX)
                return NULL;

        return table[t];
}

const char *unit_dbus_interface_from_name(const char *name) {
        UnitType t;

        t = unit_name_to_type(name);
        if (t < 0)
                return NULL;

        return unit_dbus_interface_from_type(t);
}

static const char* const unit_type_table[_UNIT_TYPE_MAX] = {
        [UNIT_SERVICE] = "service",
        [UNIT_SOCKET] = "socket",
        [UNIT_TARGET] = "target",
        [UNIT_DEVICE] = "device",
        [UNIT_MOUNT] = "mount",
        [UNIT_AUTOMOUNT] = "automount",
        [UNIT_SWAP] = "swap",
        [UNIT_TIMER] = "timer",
        [UNIT_PATH] = "path",
        [UNIT_SLICE] = "slice",
        [UNIT_SCOPE] = "scope",
};

DEFINE_STRING_TABLE_LOOKUP(unit_type, UnitType);

static const char* const unit_load_state_table[_UNIT_LOAD_STATE_MAX] = {
        [UNIT_STUB] = "stub",
        [UNIT_LOADED] = "loaded",
        [UNIT_NOT_FOUND] = "not-found",
        [UNIT_BAD_SETTING] = "bad-setting",
        [UNIT_ERROR] = "error",
        [UNIT_MERGED] = "merged",
        [UNIT_MASKED] = "masked"
};

DEFINE_STRING_TABLE_LOOKUP(unit_load_state, UnitLoadState);

static const char* const unit_active_state_table[_UNIT_ACTIVE_STATE_MAX] = {
        [UNIT_ACTIVE] = "active",
        [UNIT_RELOADING] = "reloading",
        [UNIT_INACTIVE] = "inactive",
        [UNIT_FAILED] = "failed",
        [UNIT_ACTIVATING] = "activating",
        [UNIT_DEACTIVATING] = "deactivating"
};

DEFINE_STRING_TABLE_LOOKUP(unit_active_state, UnitActiveState);

static const char* const automount_state_table[_AUTOMOUNT_STATE_MAX] = {
        [AUTOMOUNT_DEAD] = "dead",
        [AUTOMOUNT_WAITING] = "waiting",
        [AUTOMOUNT_RUNNING] = "running",
        [AUTOMOUNT_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(automount_state, AutomountState);

static const char* const device_state_table[_DEVICE_STATE_MAX] = {
        [DEVICE_DEAD] = "dead",
        [DEVICE_TENTATIVE] = "tentative",
        [DEVICE_PLUGGED] = "plugged",
};

DEFINE_STRING_TABLE_LOOKUP(device_state, DeviceState);

static const char* const mount_state_table[_MOUNT_STATE_MAX] = {
        [MOUNT_DEAD] = "dead",
        [MOUNT_MOUNTING] = "mounting",
        [MOUNT_MOUNTING_DONE] = "mounting-done",
        [MOUNT_MOUNTED] = "mounted",
        [MOUNT_REMOUNTING] = "remounting",
        [MOUNT_UNMOUNTING] = "unmounting",
        [MOUNT_REMOUNTING_SIGTERM] = "remounting-sigterm",
        [MOUNT_REMOUNTING_SIGKILL] = "remounting-sigkill",
        [MOUNT_UNMOUNTING_SIGTERM] = "unmounting-sigterm",
        [MOUNT_UNMOUNTING_SIGKILL] = "unmounting-sigkill",
        [MOUNT_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(mount_state, MountState);

static const char* const path_state_table[_PATH_STATE_MAX] = {
        [PATH_DEAD] = "dead",
        [PATH_WAITING] = "waiting",
        [PATH_RUNNING] = "running",
        [PATH_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(path_state, PathState);

static const char* const scope_state_table[_SCOPE_STATE_MAX] = {
        [SCOPE_DEAD] = "dead",
        [SCOPE_RUNNING] = "running",
        [SCOPE_ABANDONED] = "abandoned",
        [SCOPE_STOP_SIGTERM] = "stop-sigterm",
        [SCOPE_STOP_SIGKILL] = "stop-sigkill",
        [SCOPE_FAILED] = "failed",
};

DEFINE_STRING_TABLE_LOOKUP(scope_state, ScopeState);

static const char* const service_state_table[_SERVICE_STATE_MAX] = {
        [SERVICE_DEAD] = "dead",
        [SERVICE_START_PRE] = "start-pre",
        [SERVICE_START] = "start",
        [SERVICE_START_POST] = "start-post",
        [SERVICE_RUNNING] = "running",
        [SERVICE_EXITED] = "exited",
        [SERVICE_RELOAD] = "reload",
        [SERVICE_STOP] = "stop",
        [SERVICE_STOP_WATCHDOG] = "stop-watchdog",
        [SERVICE_STOP_SIGTERM] = "stop-sigterm",
        [SERVICE_STOP_SIGKILL] = "stop-sigkill",
        [SERVICE_STOP_POST] = "stop-post",
        [SERVICE_FINAL_SIGTERM] = "final-sigterm",
        [SERVICE_FINAL_SIGKILL] = "final-sigkill",
        [SERVICE_FAILED] = "failed",
        [SERVICE_AUTO_RESTART] = "auto-restart",
};

DEFINE_STRING_TABLE_LOOKUP(service_state, ServiceState);

static const char* const slice_state_table[_SLICE_STATE_MAX] = {
        [SLICE_DEAD] = "dead",
        [SLICE_ACTIVE] = "active"
};

DEFINE_STRING_TABLE_LOOKUP(slice_state, SliceState);

static const char* const socket_state_table[_SOCKET_STATE_MAX] = {
        [SOCKET_DEAD] = "dead",
        [SOCKET_START_PRE] = "start-pre",
        [SOCKET_START_CHOWN] = "start-chown",
        [SOCKET_START_POST] = "start-post",
        [SOCKET_LISTENING] = "listening",
        [SOCKET_RUNNING] = "running",
        [SOCKET_STOP_PRE] = "stop-pre",
        [SOCKET_STOP_PRE_SIGTERM] = "stop-pre-sigterm",
        [SOCKET_STOP_PRE_SIGKILL] = "stop-pre-sigkill",
        [SOCKET_STOP_POST] = "stop-post",
        [SOCKET_FINAL_SIGTERM] = "final-sigterm",
        [SOCKET_FINAL_SIGKILL] = "final-sigkill",
        [SOCKET_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(socket_state, SocketState);

static const char* const swap_state_table[_SWAP_STATE_MAX] = {
        [SWAP_DEAD] = "dead",
        [SWAP_ACTIVATING] = "activating",
        [SWAP_ACTIVATING_DONE] = "activating-done",
        [SWAP_ACTIVE] = "active",
        [SWAP_DEACTIVATING] = "deactivating",
        [SWAP_DEACTIVATING_SIGTERM] = "deactivating-sigterm",
        [SWAP_DEACTIVATING_SIGKILL] = "deactivating-sigkill",
        [SWAP_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(swap_state, SwapState);

static const char* const target_state_table[_TARGET_STATE_MAX] = {
        [TARGET_DEAD] = "dead",
        [TARGET_ACTIVE] = "active"
};

DEFINE_STRING_TABLE_LOOKUP(target_state, TargetState);

static const char* const timer_state_table[_TIMER_STATE_MAX] = {
        [TIMER_DEAD] = "dead",
        [TIMER_WAITING] = "waiting",
        [TIMER_RUNNING] = "running",
        [TIMER_ELAPSED] = "elapsed",
        [TIMER_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(timer_state, TimerState);

static const char* const unit_dependency_table[_UNIT_DEPENDENCY_MAX] = {
        [UNIT_REQUIRES] = "Requires",
        [UNIT_REQUISITE] = "Requisite",
        [UNIT_WANTS] = "Wants",
        [UNIT_BINDS_TO] = "BindsTo",
        [UNIT_PART_OF] = "PartOf",
        [UNIT_REQUIRED_BY] = "RequiredBy",
        [UNIT_REQUISITE_OF] = "RequisiteOf",
        [UNIT_WANTED_BY] = "WantedBy",
        [UNIT_BOUND_BY] = "BoundBy",
        [UNIT_CONSISTS_OF] = "ConsistsOf",
        [UNIT_CONFLICTS] = "Conflicts",
        [UNIT_CONFLICTED_BY] = "ConflictedBy",
        [UNIT_BEFORE] = "Before",
        [UNIT_AFTER] = "After",
        [UNIT_ON_FAILURE] = "OnFailure",
        [UNIT_TRIGGERS] = "Triggers",
        [UNIT_TRIGGERED_BY] = "TriggeredBy",
        [UNIT_PROPAGATES_RELOAD_TO] = "PropagatesReloadTo",
        [UNIT_RELOAD_PROPAGATED_FROM] = "ReloadPropagatedFrom",
        [UNIT_JOINS_NAMESPACE_OF] = "JoinsNamespaceOf",
        [UNIT_REFERENCES] = "References",
        [UNIT_REFERENCED_BY] = "ReferencedBy",
};

DEFINE_STRING_TABLE_LOOKUP(unit_dependency, UnitDependency);

static const char* const notify_access_table[_NOTIFY_ACCESS_MAX] = {
        [NOTIFY_NONE] = "none",
        [NOTIFY_MAIN] = "main",
        [NOTIFY_EXEC] = "exec",
        [NOTIFY_ALL] = "all"
};

DEFINE_STRING_TABLE_LOOKUP(notify_access, NotifyAccess);
