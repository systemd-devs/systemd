/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "selinux-access.h"

#if HAVE_SELINUX

#include <errno.h>
#include <selinux/avc.h>
#include <selinux/selinux.h>
#if HAVE_AUDIT
#include <libaudit.h>
#endif

#include "sd-bus.h"

#include "alloc-util.h"
#include "audit-fd.h"
#include "bus-util.h"
#include "dbus-callbackdata.h"
#include "errno-util.h"
#include "format-util.h"
#include "hostname-util.h"
#include "install.h"
#include "log.h"
#include "path-util.h"
#include "process-util.h"
#include "selinux-util.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "strv.h"
#include "util.h"

static bool initialized = false;

struct audit_info {
        sd_bus_creds *creds;
        const char *unit_name;
        const char *path;
        const char *cmdline;
        const char *function;
};

/*
   Any time an access gets denied this callback will be called
   with the audit data.  We then need to just copy the audit data into the msgbuf.
*/
static int audit_callback(
                void *auditdata,
                security_class_t cls,
                char *msgbuf,
                size_t msgbufsize) {

        const struct audit_info *audit = auditdata;
        uid_t uid = 0, login_uid = 0;
        gid_t gid = 0;
        pid_t pid = 0;
        char login_uid_buf[DECIMAL_STR_MAX(uid_t) + 1] = "n/a";
        char uid_buf[DECIMAL_STR_MAX(uid_t) + 1] = "n/a";
        char gid_buf[DECIMAL_STR_MAX(gid_t) + 1] = "n/a";
        char pid_buf[DECIMAL_STR_MAX(pid_t) + 1] = "n/a";
        _cleanup_free_ char *exe = NULL, *comm = NULL;

        if (sd_bus_creds_get_audit_login_uid(audit->creds, &login_uid) >= 0)
                xsprintf(login_uid_buf, UID_FMT, login_uid);
        if (sd_bus_creds_get_euid(audit->creds, &uid) >= 0)
                xsprintf(uid_buf, UID_FMT, uid);
        if (sd_bus_creds_get_egid(audit->creds, &gid) >= 0)
                xsprintf(gid_buf, GID_FMT, gid);
        if (sd_bus_creds_get_pid(audit->creds, &pid) >= 0) {
                xsprintf(pid_buf, PID_FMT, pid);
                (void) get_process_exe(pid, &exe);
                (void) get_process_comm(pid, &comm);
        }

        (void) snprintf(msgbuf, msgbufsize,
                        "auid=%s uid=%s gid=%s subj_pid=%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                        login_uid_buf, uid_buf, gid_buf, pid_buf,
                        audit->unit_name ? " unit_name=\"" : "", strempty(audit->unit_name), audit->unit_name ? "\"" : "",
                        audit->path ? " path=\"" : "", strempty(audit->path), audit->path ? "\"" : "",
                        exe ? " subj_exe=\"" : "", strempty(exe), exe ? "\"" : "",
                        comm ? " subj_comm=\"" : "", strempty(comm), comm ? "\"" : "",
                        audit->cmdline ? " cmdline=\"" : "", strempty(audit->cmdline), audit->cmdline ? "\"" : "",
                        audit->function ? " function=\"" : "", strempty(audit->function), audit->function ? "\"" : "");

        return 0;
}

static int callback_type_to_priority(int type) {
        switch (type) {

        case SELINUX_ERROR:
                return LOG_ERR;

        case SELINUX_WARNING:
                return LOG_WARNING;

        case SELINUX_INFO:
                return LOG_INFO;

        case SELINUX_AVC:
        default:
                return LOG_NOTICE;
        }
}

/*
   libselinux uses this callback when access gets denied or other
   events happen. If audit is turned on, messages will be reported
   using audit netlink, otherwise they will be logged using the usual
   channels.

   Code copied from dbus and modified.
*/
_printf_(2, 3) static int log_callback(int type, const char *fmt, ...) {
        va_list ap;
        const char *fmt2;

#if HAVE_AUDIT
        int fd;

        fd = get_audit_fd();

        if (fd >= 0) {
                _cleanup_free_ char *buf = NULL;
                int r;

                va_start(ap, fmt);
                r = vasprintf(&buf, fmt, ap);
                va_end(ap);

                if (r >= 0) {
                        _cleanup_free_ char *hn = NULL;

                        hn = gethostname_malloc();
                        if (hn)
                                hostname_cleanup(hn);

                        if (type == SELINUX_AVC)
                                audit_log_user_avc_message(get_audit_fd(), AUDIT_USER_AVC, buf, hn, NULL, NULL, getuid());
                        else if (type == SELINUX_ERROR)
                                audit_log_user_avc_message(get_audit_fd(), AUDIT_USER_SELINUX_ERR, buf, hn, NULL, NULL, getuid());

                        return 0;
                }
        }
#endif

        fmt2 = strjoina("selinux: ", fmt);

        va_start(ap, fmt);

        DISABLE_WARNING_FORMAT_NONLITERAL;
        log_internalv(LOG_AUTH | callback_type_to_priority(type),
                      0, PROJECT_FILE, __LINE__, __func__,
                      fmt2, ap);
        REENABLE_WARNING;
        va_end(ap);

        return 0;
}

static int access_init(sd_bus_error *error) {

        if (!mac_selinux_use())
                return 0;

        if (initialized)
                return 1;

        if (avc_open(NULL, 0) != 0) {
                int saved_errno = errno;
                bool enforce;

                enforce = security_getenforce() != 0;
                log_full_errno(enforce ? LOG_ERR : LOG_WARNING, saved_errno, "Failed to open the SELinux AVC: %m");

                /* If enforcement isn't on, then let's suppress this
                 * error, and just don't do any AVC checks. The
                 * warning we printed is hence all the admin will
                 * see. */
                if (!enforce)
                        return 0;

                /* Return an access denied error, if we couldn't load
                 * the AVC but enforcing mode was on, or we couldn't
                 * determine whether it is one. */
                return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED, "Failed to open the SELinux AVC: %s", strerror_safe(saved_errno));
        }

        selinux_set_callback(SELINUX_CB_AUDIT, (union selinux_callback) { .func_audit = audit_callback });
        selinux_set_callback(SELINUX_CB_LOG, (union selinux_callback) { .func_log = log_callback });

        initialized = true;
        return 1;
}

/*
   This function communicates with the kernel to check whether or not it should
   allow the access.
   If the machine is in permissive mode it will return ok.  Audit messages will
   still be generated if the access would be denied in enforcing mode.
*/
int mac_selinux_access_check_internal(
                sd_bus_message *message,
                const char *unit_name,
                const char *unit_path,
                const char *unit_context,
                const char *permission,
                const char *function,
                sd_bus_error *error) {

        _cleanup_(sd_bus_creds_unrefp) sd_bus_creds *creds = NULL;
        const char *tclass, *scon, *acon;
        _cleanup_free_ char *cl = NULL;
        _cleanup_freecon_ char *fcon = NULL;
        char **cmdline = NULL;
        bool enforce;
        int r = 0;

        assert(message);
        assert(permission);
        assert(function);
        assert(error);

        r = access_init(error);
        if (r <= 0)
                return r;

        /* delay call until we checked in `access_init()` if SELinux is actually enabled */
        enforce = mac_selinux_enforcing();

        r = sd_bus_query_sender_creds(
                        message,
                        SD_BUS_CREDS_PID|SD_BUS_CREDS_EUID|SD_BUS_CREDS_EGID|
                        SD_BUS_CREDS_CMDLINE|SD_BUS_CREDS_AUDIT_LOGIN_UID|
                        SD_BUS_CREDS_SELINUX_CONTEXT|
                        SD_BUS_CREDS_AUGMENT /* get more bits from /proc */,
                        &creds);
        if (r < 0)
                return r;

        /* The SELinux context is something we really should have gotten directly from the message or sender,
         * and not be an augmented field. If it was augmented we cannot use it for authorization, since this
         * is racy and vulnerable. Let's add an extra check, just in case, even though this really shouldn't
         * be possible. */
        assert_return((sd_bus_creds_get_augmented_mask(creds) & SD_BUS_CREDS_SELINUX_CONTEXT) == 0, -EPERM);

        r = sd_bus_creds_get_selinux_context(creds, &scon);
        if (r < 0)
                return r;

        if (unit_context) {
                /* Nice! The unit comes with a SELinux context read from the unit file */
                acon = unit_context;
                tclass = "service";
        } else {
                /* If no unit context is known, use our own */
                if (getcon_raw(&fcon) < 0) {
                        r = -errno;

                        log_warning_errno(r, "SELinux getcon_raw() failed%s (perm=%s): %m",
                                          enforce ? "" : ", ignoring",
                                          permission);
                        if (!enforce)
                                return 0;

                        return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED, "Failed to get current context: %m");
                }

                acon = fcon;
                tclass = "system";
        }

        sd_bus_creds_get_cmdline(creds, &cmdline);
        cl = strv_join(cmdline, " ");

        struct audit_info audit_info = {
                .creds = creds,
                .unit_name = unit_name,
                .path = unit_path,
                .cmdline = cl,
                .function = function,
        };

        r = selinux_check_access(scon, acon, tclass, permission, &audit_info);
        if (r < 0) {
                errno = -(r = errno_or_else(EPERM));

                if (enforce)
                        sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED, "SELinux policy denies access: %m");
        }

        log_full_errno_zerook(LOG_DEBUG, r,
                              "SELinux access check scon=%s tcon=%s tclass=%s perm=%s state=%s function=%s unitname=%s path=%s cmdline=%s: %m",
                              scon, acon, tclass, permission, enforce ? "enforcing" : "permissive", function, strna(unit_name), strna(unit_path), strna(empty_to_null(cl)));
        return enforce ? r : 0;
}

int mac_selinux_unit_callback_check(
        const char *unit_name,
        const MacUnitCallbackUserdata *userdata) {

        _cleanup_freecon_ char *selcon = NULL;
        Unit *u;
        const char *path = NULL, *label = NULL;
        int r;

        assert(unit_name);
        assert(userdata);
        assert(userdata->manager);
        assert(userdata->message);
        assert(userdata->error);
        assert(userdata->function);

        if (!mac_selinux_use())
                return 0;

        /* Skip if the operation should not be checked by SELinux */
        if (!userdata->selinux_permission)
                return 0;

        u = manager_get_unit(userdata->manager, unit_name);
        if (!u)
                (void) manager_load_unit(userdata->manager, unit_name, NULL, NULL, &u);
        if (u) {
                path = u->fragment_path;
                label = u->access_selinux_context;
        }

        if (!label) {
                const char *lookup_path;

                lookup_path = manager_lookup_unit_label_path(userdata->manager, unit_name);
                if (lookup_path) {
                        path = lookup_path;

                        r = getfilecon_raw(lookup_path, &selcon);
                        if (r < 0)
                                log_unit_warning_errno(u, r, "Failed to read SELinux context of '%s', ignoring: %m", lookup_path);
                        else
                                label = selcon;
                }
        }

        return mac_selinux_access_check_internal(
                userdata->message,
                unit_name,
                path,
                label,
                userdata->selinux_permission,
                userdata->function,
                userdata->error);
}

#else /* HAVE_SELINUX */

int mac_selinux_access_check_internal(
                sd_bus_message *message,
                const char *unit_name,
                const char *unit_path,
                const char *unit_label,
                const char *permission,
                const char *function,
                sd_bus_error *error) {

        return 0;
}

int mac_selinux_unit_callback_check(
                const char *unit_name,
                const MacUnitCallbackUserdata *userdata) {

        return 0;
}

#endif /* HAVE_SELINUX */
