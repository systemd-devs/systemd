/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

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

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/statvfs.h>
#include <linux/sockios.h>

#include "libudev.h"
#include "sd-daemon.h"
#include "sd-journal.h"
#include "sd-messages.h"

#include "acl-util.h"
#include "alloc-util.h"
#include "audit-util.h"
#include "cgroup-util.h"
#include "conf-parser.h"
#include "dirent-util.h"
#include "extract-word.h"
#include "fd-util.h"
#include "fileio.h"
#include "formats-util.h"
#include "fs-util.h"
#include "hashmap.h"
#include "hostname-util.h"
#include "io-util.h"
#include "journal-authenticate.h"
#include "journal-file.h"
#include "journal-internal.h"
#include "journal-vacuum.h"
#include "journald-audit.h"
#include "journald-kmsg.h"
#include "journald-native.h"
#include "journald-rate-limit.h"
#include "journald-server.h"
#include "journald-stream.h"
#include "journald-syslog.h"
#include "missing.h"
#include "mkdir.h"
#include "parse-util.h"
#include "proc-cmdline.h"
#include "process-util.h"
#include "rm-rf.h"
#include "selinux-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "stdio-util.h"
#include "string-table.h"
#include "string-util.h"
#include "user-util.h"
#include "log.h"

#define USER_JOURNALS_MAX 1024

#define DEFAULT_SYNC_INTERVAL_USEC (5*USEC_PER_MINUTE)
#define DEFAULT_RATE_LIMIT_INTERVAL (30*USEC_PER_SEC)
#define DEFAULT_RATE_LIMIT_BURST 1000
#define DEFAULT_MAX_FILE_USEC USEC_PER_MONTH

#define RECHECK_SPACE_USEC (30*USEC_PER_SEC)

#define NOTIFY_SNDBUF_SIZE (8*1024*1024)

/* The period to insert between posting changes for coalescing */
#define POST_CHANGE_TIMER_INTERVAL_USEC (250*USEC_PER_MSEC)

static int determine_space_for(
                Server *s,
                JournalMetrics *metrics,
                const char *path,
                const char *name,
                bool verbose,
                bool patch_min_use,
                uint64_t *available,
                uint64_t *limit) {

        uint64_t sum = 0, ss_avail, avail;
        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;
        struct statvfs ss;
        const char *p;
        usec_t ts;

        assert(s);
        assert(metrics);
        assert(path);
        assert(name);

        ts = now(CLOCK_MONOTONIC);

        if (!verbose && s->cached_space_timestamp + RECHECK_SPACE_USEC > ts) {

                if (available)
                        *available = s->cached_space_available;
                if (limit)
                        *limit = s->cached_space_limit;

                return 0;
        }

        p = strjoina(path, SERVER_MACHINE_ID(s));
        d = opendir(p);
        if (!d)
                return log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_ERR, errno, "Failed to open %s: %m", p);

        if (fstatvfs(dirfd(d), &ss) < 0)
                return log_error_errno(errno, "Failed to fstatvfs(%s): %m", p);

        FOREACH_DIRENT_ALL(de, d, break) {
                struct stat st;

                if (!endswith(de->d_name, ".journal") &&
                    !endswith(de->d_name, ".journal~"))
                        continue;

                if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                        log_debug_errno(errno, "Failed to stat %s/%s, ignoring: %m", p, de->d_name);
                        continue;
                }

                if (!S_ISREG(st.st_mode))
                        continue;

                sum += (uint64_t) st.st_blocks * 512UL;
        }

        /* If requested, then let's bump the min_use limit to the
         * current usage on disk. We do this when starting up and
         * first opening the journal files. This way sudden spikes in
         * disk usage will not cause journald to vacuum files without
         * bounds. Note that this means that only a restart of
         * journald will make it reset this value. */

        if (patch_min_use)
                metrics->min_use = MAX(metrics->min_use, sum);

        ss_avail = ss.f_bsize * ss.f_bavail;
        avail = LESS_BY(ss_avail, metrics->keep_free);

        s->cached_space_limit = MIN(MAX(sum + avail, metrics->min_use), metrics->max_use);
        s->cached_space_available = LESS_BY(s->cached_space_limit, sum);
        s->cached_space_timestamp = ts;

        if (verbose) {
                char    fb1[FORMAT_BYTES_MAX], fb2[FORMAT_BYTES_MAX], fb3[FORMAT_BYTES_MAX],
                        fb4[FORMAT_BYTES_MAX], fb5[FORMAT_BYTES_MAX], fb6[FORMAT_BYTES_MAX];
                format_bytes(fb1, sizeof(fb1), sum);
                format_bytes(fb2, sizeof(fb2), metrics->max_use);
                format_bytes(fb3, sizeof(fb3), metrics->keep_free);
                format_bytes(fb4, sizeof(fb4), ss_avail);
                format_bytes(fb5, sizeof(fb5), s->cached_space_limit);
                format_bytes(fb6, sizeof(fb6), s->cached_space_available);

                server_driver_message(s, SD_MESSAGE_JOURNAL_USAGE,
                                      LOG_MESSAGE("%s (%s) is %s, max %s, %s free.",
                                                  name, path, fb1, fb5, fb6),
                                      "JOURNAL_NAME=%s", name,
                                      "JOURNAL_PATH=%s", path,
                                      "CURRENT_USE=%"PRIu64, sum,
                                      "CURRENT_USE_PRETTY=%s", fb1,
                                      "MAX_USE=%"PRIu64, metrics->max_use,
                                      "MAX_USE_PRETTY=%s", fb2,
                                      "DISK_KEEP_FREE=%"PRIu64, metrics->keep_free,
                                      "DISK_KEEP_FREE_PRETTY=%s", fb3,
                                      "DISK_AVAILABLE=%"PRIu64, ss_avail,
                                      "DISK_AVAILABLE_PRETTY=%s", fb4,
                                      "LIMIT=%"PRIu64, s->cached_space_limit,
                                      "LIMIT_PRETTY=%s", fb5,
                                      "AVAILABLE=%"PRIu64, s->cached_space_available,
                                      "AVAILABLE_PRETTY=%s", fb6,
                                      NULL);
        }

        if (available)
                *available = s->cached_space_available;
        if (limit)
                *limit = s->cached_space_limit;

        return 1;
}

static int determine_space(Server *s, bool verbose, bool patch_min_use, uint64_t *available, uint64_t *limit) {
        JournalMetrics *metrics;
        const char *path, *name;

        assert(s);

        if (s->system_journal) {
                path = "/var/log/journal/";
                metrics = &s->system_metrics;
                name = "System journal";
        } else {
                path = "/run/log/journal/";
                metrics = &s->runtime_metrics;
                name = "Runtime journal";
        }

        return determine_space_for(s, metrics, path, name, verbose, patch_min_use, available, limit);
}

static void server_add_acls(JournalFile *f, uid_t uid) {
#ifdef HAVE_ACL
        int r;
#endif
        assert(f);

#ifdef HAVE_ACL
        if (uid <= SYSTEM_UID_MAX)
                return;

        r = add_acls_for_user(f->fd, uid);
        if (r < 0)
                log_warning_errno(r, "Failed to set ACL on %s, ignoring: %m", f->path);
#endif
}

static int open_journal(
                Server *s,
                bool reliably,
                const char *fname,
                int flags,
                bool seal,
                JournalMetrics *metrics,
                JournalFile *template,
                JournalFile **ret) {
        int r;
        JournalFile *f;

        assert(s);
        assert(fname);
        assert(ret);

        if (reliably)
                r = journal_file_open_reliably(fname, flags, 0640, s->compress, seal, metrics, s->mmap, template, &f);
        else
                r = journal_file_open(fname, flags, 0640, s->compress, seal, metrics, s->mmap, template, &f);
        if (r < 0)
                return r;

        r = journal_file_enable_post_change_timer(f, s->event, POST_CHANGE_TIMER_INTERVAL_USEC);
        if (r < 0) {
                journal_file_close(f);
                return r;
        }

        *ret = f;
        return r;
}

static JournalFile* find_journal(Server *s, uid_t uid) {
        _cleanup_free_ char *p = NULL;
        int r;
        JournalFile *f;
        sd_id128_t machine;

        assert(s);

        /* We split up user logs only on /var, not on /run. If the
         * runtime file is open, we write to it exclusively, in order
         * to guarantee proper order as soon as we flush /run to
         * /var and close the runtime file. */

        if (s->runtime_journal)
                return s->runtime_journal;

        if (uid <= SYSTEM_UID_MAX)
                return s->system_journal;

        r = sd_id128_get_machine(&machine);
        if (r < 0)
                return s->system_journal;

        f = ordered_hashmap_get(s->user_journals, UID_TO_PTR(uid));
        if (f)
                return f;

        if (asprintf(&p, "/var/log/journal/" SD_ID128_FORMAT_STR "/user-"UID_FMT".journal",
                     SD_ID128_FORMAT_VAL(machine), uid) < 0)
                return s->system_journal;

        while (ordered_hashmap_size(s->user_journals) >= USER_JOURNALS_MAX) {
                /* Too many open? Then let's close one */
                f = ordered_hashmap_steal_first(s->user_journals);
                assert(f);
                journal_file_close(f);
        }

        r = open_journal(s, true, p, O_RDWR|O_CREAT, s->seal, &s->system_metrics, NULL, &f);
        if (r < 0)
                return s->system_journal;

        server_add_acls(f, uid);

        r = ordered_hashmap_put(s->user_journals, UID_TO_PTR(uid), f);
        if (r < 0) {
                journal_file_close(f);
                return s->system_journal;
        }

        return f;
}

static int do_rotate(
                Server *s,
                JournalFile **f,
                const char* name,
                bool seal,
                uint32_t uid) {

        int r;
        assert(s);

        if (!*f)
                return -EINVAL;

        r = journal_file_rotate(f, s->compress, seal);
        if (r < 0)
                if (*f)
                        log_error_errno(r, "Failed to rotate %s: %m", (*f)->path);
                else
                        log_error_errno(r, "Failed to create new %s journal: %m", name);
        else
                server_add_acls(*f, uid);

        return r;
}

void server_rotate(Server *s) {
        JournalFile *f;
        void *k;
        Iterator i;
        int r;

        log_debug("Rotating...");

        (void) do_rotate(s, &s->runtime_journal, "runtime", false, 0);
        (void) do_rotate(s, &s->system_journal, "system", s->seal, 0);

        ORDERED_HASHMAP_FOREACH_KEY(f, k, s->user_journals, i) {
                r = do_rotate(s, &f, "user", s->seal, PTR_TO_UID(k));
                if (r >= 0)
                        ordered_hashmap_replace(s->user_journals, k, f);
                else if (!f)
                        /* Old file has been closed and deallocated */
                        ordered_hashmap_remove(s->user_journals, k);
        }
}

void server_sync(Server *s) {
        JournalFile *f;
        Iterator i;
        int r;

        if (s->system_journal) {
                r = journal_file_set_offline(s->system_journal);
                if (r < 0)
                        log_warning_errno(r, "Failed to sync system journal, ignoring: %m");
        }

        ORDERED_HASHMAP_FOREACH(f, s->user_journals, i) {
                r = journal_file_set_offline(f);
                if (r < 0)
                        log_warning_errno(r, "Failed to sync user journal, ignoring: %m");
        }

        if (s->sync_event_source) {
                r = sd_event_source_set_enabled(s->sync_event_source, SD_EVENT_OFF);
                if (r < 0)
                        log_error_errno(r, "Failed to disable sync timer source: %m");
        }

        s->sync_scheduled = false;
}

static void do_vacuum(
                Server *s,
                JournalFile *f,
                JournalMetrics *metrics,
                const char *path,
                const char *name,
                bool verbose,
                bool patch_min_use) {

        const char *p;
        uint64_t limit;
        int r;

        assert(s);
        assert(metrics);
        assert(path);
        assert(name);

        if (!f)
                return;

        p = strjoina(path, SERVER_MACHINE_ID(s));

        limit = metrics->max_use;
        (void) determine_space_for(s, metrics, path, name, verbose, patch_min_use, NULL, &limit);

        r = journal_directory_vacuum(p, limit, metrics->n_max_files, s->max_retention_usec, &s->oldest_file_usec,  verbose);
        if (r < 0 && r != -ENOENT)
                log_warning_errno(r, "Failed to vacuum %s, ignoring: %m", p);
}

int server_vacuum(Server *s, bool verbose, bool patch_min_use) {
        assert(s);

        log_debug("Vacuuming...");

        s->oldest_file_usec = 0;

        do_vacuum(s, s->system_journal, &s->system_metrics, "/var/log/journal/", "System journal", verbose, patch_min_use);
        do_vacuum(s, s->runtime_journal, &s->runtime_metrics, "/run/log/journal/", "Runtime journal", verbose, patch_min_use);

        s->cached_space_limit = 0;
        s->cached_space_available = 0;
        s->cached_space_timestamp = 0;

        return 0;
}

static void server_cache_machine_id(Server *s) {
        sd_id128_t id;
        int r;

        assert(s);

        r = sd_id128_get_machine(&id);
        if (r < 0)
                return;

        sd_id128_to_string(id, stpcpy(s->machine_id_field, "_MACHINE_ID="));
}

static void server_cache_boot_id(Server *s) {
        sd_id128_t id;
        int r;

        assert(s);

        r = sd_id128_get_boot(&id);
        if (r < 0)
                return;

        sd_id128_to_string(id, stpcpy(s->boot_id_field, "_BOOT_ID="));
}

static void server_cache_hostname(Server *s) {
        _cleanup_free_ char *t = NULL;
        char *x;

        assert(s);

        t = gethostname_malloc();
        if (!t)
                return;

        x = strappend("_HOSTNAME=", t);
        if (!x)
                return;

        free(s->hostname_field);
        s->hostname_field = x;
}

static bool shall_try_append_again(JournalFile *f, int r) {

        /* -E2BIG            Hit configured limit
           -EFBIG            Hit fs limit
           -EDQUOT           Quota limit hit
           -ENOSPC           Disk full
           -EIO              I/O error of some kind (mmap)
           -EHOSTDOWN        Other machine
           -EBUSY            Unclean shutdown
           -EPROTONOSUPPORT  Unsupported feature
           -EBADMSG          Corrupted
           -ENODATA          Truncated
           -ESHUTDOWN        Already archived
           -EIDRM            Journal file has been deleted */

        if (r == -E2BIG || r == -EFBIG || r == -EDQUOT || r == -ENOSPC)
                log_debug("%s: Allocation limit reached, rotating.", f->path);
        else if (r == -EHOSTDOWN)
                log_info("%s: Journal file from other machine, rotating.", f->path);
        else if (r == -EBUSY)
                log_info("%s: Unclean shutdown, rotating.", f->path);
        else if (r == -EPROTONOSUPPORT)
                log_info("%s: Unsupported feature, rotating.", f->path);
        else if (r == -EBADMSG || r == -ENODATA || r == ESHUTDOWN)
                log_warning("%s: Journal file corrupted, rotating.", f->path);
        else if (r == -EIO)
                log_warning("%s: IO error, rotating.", f->path);
        else if (r == -EIDRM)
                log_warning("%s: Journal file has been deleted, rotating.", f->path);
        else
                return false;

        return true;
}

static void write_to_journal(Server *s, uid_t uid, struct iovec *iovec, unsigned n, int priority) {
        JournalFile *f;
        bool vacuumed = false;
        int r;

        assert(s);
        assert(iovec);
        assert(n > 0);

        f = find_journal(s, uid);
        if (!f)
                return;

        if (journal_file_rotate_suggested(f, s->max_file_usec)) {
                log_debug("%s: Journal header limits reached or header out-of-date, rotating.", f->path);
                server_rotate(s);
                server_vacuum(s, false, false);
                vacuumed = true;

                f = find_journal(s, uid);
                if (!f)
                        return;
        }

        do {
                r = journal_file_append_entry(f, NULL, iovec, n, &s->seqnum, NULL, NULL);
                if (r >= 0) {
                        server_schedule_sync(s, priority);
                        return;
                }

                /* ENOMEM won't be uncommon with mremap() growing our
                 * MAP_PRIVATE mmap without REMAP_MAYMOVE, when that happens
                 * we'll just synchronously and finalize the offline and
                 * quietly retry in the MAP_SHARED map. */
        } while (r == -ENOMEM &&
                 f->offline_fsync_in_progress &&
                 !(r = journal_file_set_offline_finalize(f, true)));

        if (vacuumed || !shall_try_append_again(f, r)) {
                log_error_errno(r, "Failed to write entry (%d items, %zu bytes), ignoring: %m", n, IOVEC_TOTAL_SIZE(iovec, n));
                return;
        }

        server_rotate(s);
        server_vacuum(s, false, false);

        f = find_journal(s, uid);
        if (!f)
                return;

        log_debug("Retrying write.");
        r = journal_file_append_entry(f, NULL, iovec, n, &s->seqnum, NULL, NULL);
        if (r < 0)
                log_error_errno(r, "Failed to write entry (%d items, %zu bytes) despite vacuuming, ignoring: %m", n, IOVEC_TOTAL_SIZE(iovec, n));
        else
                server_schedule_sync(s, priority);
}

static void dispatch_message_real(
                Server *s,
                struct iovec *iovec, unsigned n, unsigned m,
                const struct ucred *ucred,
                const struct timeval *tv,
                const char *label, size_t label_len,
                const char *unit_id,
                int priority,
                pid_t object_pid) {

        char    pid[sizeof("_PID=") + DECIMAL_STR_MAX(pid_t)],
                uid[sizeof("_UID=") + DECIMAL_STR_MAX(uid_t)],
                gid[sizeof("_GID=") + DECIMAL_STR_MAX(gid_t)],
                owner_uid[sizeof("_SYSTEMD_OWNER_UID=") + DECIMAL_STR_MAX(uid_t)],
                source_time[sizeof("_SOURCE_REALTIME_TIMESTAMP=") + DECIMAL_STR_MAX(usec_t)],
                o_uid[sizeof("OBJECT_UID=") + DECIMAL_STR_MAX(uid_t)],
                o_gid[sizeof("OBJECT_GID=") + DECIMAL_STR_MAX(gid_t)],
                o_owner_uid[sizeof("OBJECT_SYSTEMD_OWNER_UID=") + DECIMAL_STR_MAX(uid_t)];
        uid_t object_uid;
        gid_t object_gid;
        char *x;
        int r;
        char *t, *c;
        uid_t realuid = 0, owner = 0, journal_uid;
        bool owner_valid = false;
#ifdef HAVE_AUDIT
        char    audit_session[sizeof("_AUDIT_SESSION=") + DECIMAL_STR_MAX(uint32_t)],
                audit_loginuid[sizeof("_AUDIT_LOGINUID=") + DECIMAL_STR_MAX(uid_t)],
                o_audit_session[sizeof("OBJECT_AUDIT_SESSION=") + DECIMAL_STR_MAX(uint32_t)],
                o_audit_loginuid[sizeof("OBJECT_AUDIT_LOGINUID=") + DECIMAL_STR_MAX(uid_t)];

        uint32_t audit;
        uid_t loginuid;
#endif

        assert(s);
        assert(iovec);
        assert(n > 0);
        assert(n + N_IOVEC_META_FIELDS + (object_pid ? N_IOVEC_OBJECT_FIELDS : 0) <= m);

        if (ucred) {
                realuid = ucred->uid;

                sprintf(pid, "_PID="PID_FMT, ucred->pid);
                IOVEC_SET_STRING(iovec[n++], pid);

                sprintf(uid, "_UID="UID_FMT, ucred->uid);
                IOVEC_SET_STRING(iovec[n++], uid);

                sprintf(gid, "_GID="GID_FMT, ucred->gid);
                IOVEC_SET_STRING(iovec[n++], gid);

                r = get_process_comm(ucred->pid, &t);
                if (r >= 0) {
                        x = strjoina("_COMM=", t);
                        free(t);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

                r = get_process_exe(ucred->pid, &t);
                if (r >= 0) {
                        x = strjoina("_EXE=", t);
                        free(t);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

                r = get_process_cmdline(ucred->pid, 0, false, &t);
                if (r >= 0) {
                        x = strjoina("_CMDLINE=", t);
                        free(t);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

                r = get_process_capeff(ucred->pid, &t);
                if (r >= 0) {
                        x = strjoina("_CAP_EFFECTIVE=", t);
                        free(t);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

#ifdef HAVE_AUDIT
                r = audit_session_from_pid(ucred->pid, &audit);
                if (r >= 0) {
                        sprintf(audit_session, "_AUDIT_SESSION=%"PRIu32, audit);
                        IOVEC_SET_STRING(iovec[n++], audit_session);
                }

                r = audit_loginuid_from_pid(ucred->pid, &loginuid);
                if (r >= 0) {
                        sprintf(audit_loginuid, "_AUDIT_LOGINUID="UID_FMT, loginuid);
                        IOVEC_SET_STRING(iovec[n++], audit_loginuid);
                }
#endif

                r = cg_pid_get_path_shifted(ucred->pid, s->cgroup_root, &c);
                if (r >= 0) {
                        char *session = NULL;

                        x = strjoina("_SYSTEMD_CGROUP=", c);
                        IOVEC_SET_STRING(iovec[n++], x);

                        r = cg_path_get_session(c, &t);
                        if (r >= 0) {
                                session = strjoina("_SYSTEMD_SESSION=", t);
                                free(t);
                                IOVEC_SET_STRING(iovec[n++], session);
                        }

                        if (cg_path_get_owner_uid(c, &owner) >= 0) {
                                owner_valid = true;

                                sprintf(owner_uid, "_SYSTEMD_OWNER_UID="UID_FMT, owner);
                                IOVEC_SET_STRING(iovec[n++], owner_uid);
                        }

                        if (cg_path_get_unit(c, &t) >= 0) {
                                x = strjoina("_SYSTEMD_UNIT=", t);
                                free(t);
                                IOVEC_SET_STRING(iovec[n++], x);
                        } else if (unit_id && !session) {
                                x = strjoina("_SYSTEMD_UNIT=", unit_id);
                                IOVEC_SET_STRING(iovec[n++], x);
                        }

                        if (cg_path_get_user_unit(c, &t) >= 0) {
                                x = strjoina("_SYSTEMD_USER_UNIT=", t);
                                free(t);
                                IOVEC_SET_STRING(iovec[n++], x);
                        } else if (unit_id && session) {
                                x = strjoina("_SYSTEMD_USER_UNIT=", unit_id);
                                IOVEC_SET_STRING(iovec[n++], x);
                        }

                        if (cg_path_get_slice(c, &t) >= 0) {
                                x = strjoina("_SYSTEMD_SLICE=", t);
                                free(t);
                                IOVEC_SET_STRING(iovec[n++], x);
                        }

                        free(c);
                } else if (unit_id) {
                        x = strjoina("_SYSTEMD_UNIT=", unit_id);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

#ifdef HAVE_SELINUX
                if (mac_selinux_have()) {
                        if (label) {
                                x = alloca(strlen("_SELINUX_CONTEXT=") + label_len + 1);

                                *((char*) mempcpy(stpcpy(x, "_SELINUX_CONTEXT="), label, label_len)) = 0;
                                IOVEC_SET_STRING(iovec[n++], x);
                        } else {
                                security_context_t con;

                                if (getpidcon(ucred->pid, &con) >= 0) {
                                        x = strjoina("_SELINUX_CONTEXT=", con);

                                        freecon(con);
                                        IOVEC_SET_STRING(iovec[n++], x);
                                }
                        }
                }
#endif
        }
        assert(n <= m);

        if (object_pid) {
                r = get_process_uid(object_pid, &object_uid);
                if (r >= 0) {
                        sprintf(o_uid, "OBJECT_UID="UID_FMT, object_uid);
                        IOVEC_SET_STRING(iovec[n++], o_uid);
                }

                r = get_process_gid(object_pid, &object_gid);
                if (r >= 0) {
                        sprintf(o_gid, "OBJECT_GID="GID_FMT, object_gid);
                        IOVEC_SET_STRING(iovec[n++], o_gid);
                }

                r = get_process_comm(object_pid, &t);
                if (r >= 0) {
                        x = strjoina("OBJECT_COMM=", t);
                        free(t);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

                r = get_process_exe(object_pid, &t);
                if (r >= 0) {
                        x = strjoina("OBJECT_EXE=", t);
                        free(t);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

                r = get_process_cmdline(object_pid, 0, false, &t);
                if (r >= 0) {
                        x = strjoina("OBJECT_CMDLINE=", t);
                        free(t);
                        IOVEC_SET_STRING(iovec[n++], x);
                }

#ifdef HAVE_AUDIT
                r = audit_session_from_pid(object_pid, &audit);
                if (r >= 0) {
                        sprintf(o_audit_session, "OBJECT_AUDIT_SESSION=%"PRIu32, audit);
                        IOVEC_SET_STRING(iovec[n++], o_audit_session);
                }

                r = audit_loginuid_from_pid(object_pid, &loginuid);
                if (r >= 0) {
                        sprintf(o_audit_loginuid, "OBJECT_AUDIT_LOGINUID="UID_FMT, loginuid);
                        IOVEC_SET_STRING(iovec[n++], o_audit_loginuid);
                }
#endif

                r = cg_pid_get_path_shifted(object_pid, s->cgroup_root, &c);
                if (r >= 0) {
                        x = strjoina("OBJECT_SYSTEMD_CGROUP=", c);
                        IOVEC_SET_STRING(iovec[n++], x);

                        r = cg_path_get_session(c, &t);
                        if (r >= 0) {
                                x = strjoina("OBJECT_SYSTEMD_SESSION=", t);
                                free(t);
                                IOVEC_SET_STRING(iovec[n++], x);
                        }

                        if (cg_path_get_owner_uid(c, &owner) >= 0) {
                                sprintf(o_owner_uid, "OBJECT_SYSTEMD_OWNER_UID="UID_FMT, owner);
                                IOVEC_SET_STRING(iovec[n++], o_owner_uid);
                        }

                        if (cg_path_get_unit(c, &t) >= 0) {
                                x = strjoina("OBJECT_SYSTEMD_UNIT=", t);
                                free(t);
                                IOVEC_SET_STRING(iovec[n++], x);
                        }

                        if (cg_path_get_user_unit(c, &t) >= 0) {
                                x = strjoina("OBJECT_SYSTEMD_USER_UNIT=", t);
                                free(t);
                                IOVEC_SET_STRING(iovec[n++], x);
                        }

                        free(c);
                }
        }
        assert(n <= m);

        if (tv) {
                sprintf(source_time, "_SOURCE_REALTIME_TIMESTAMP=%llu", (unsigned long long) timeval_load(tv));
                IOVEC_SET_STRING(iovec[n++], source_time);
        }

        /* Note that strictly speaking storing the boot id here is
         * redundant since the entry includes this in-line
         * anyway. However, we need this indexed, too. */
        if (!isempty(s->boot_id_field))
                IOVEC_SET_STRING(iovec[n++], s->boot_id_field);

        if (!isempty(s->machine_id_field))
                IOVEC_SET_STRING(iovec[n++], s->machine_id_field);

        if (!isempty(s->hostname_field))
                IOVEC_SET_STRING(iovec[n++], s->hostname_field);

        assert(n <= m);

        if (s->split_mode == SPLIT_UID && realuid > 0)
                /* Split up strictly by any UID */
                journal_uid = realuid;
        else if (s->split_mode == SPLIT_LOGIN && realuid > 0 && owner_valid && owner > 0)
                /* Split up by login UIDs.  We do this only if the
                 * realuid is not root, in order not to accidentally
                 * leak privileged information to the user that is
                 * logged by a privileged process that is part of an
                 * unprivileged session. */
                journal_uid = owner;
        else
                journal_uid = 0;

        write_to_journal(s, journal_uid, iovec, n, priority);
}

void server_driver_message(Server *s, sd_id128_t message_id, const char *format, ...) {
        char mid[11 + 32 + 1];
        struct iovec iovec[N_IOVEC_META_FIELDS + 5 + N_IOVEC_PAYLOAD_FIELDS];
        unsigned n = 0, m;
        int r;
        va_list ap;
        struct ucred ucred = {};

        assert(s);
        assert(format);

        assert_cc(3 == LOG_FAC(LOG_DAEMON));
        IOVEC_SET_STRING(iovec[n++], "SYSLOG_FACILITY=3");
        IOVEC_SET_STRING(iovec[n++], "SYSLOG_IDENTIFIER=systemd-journald");

        IOVEC_SET_STRING(iovec[n++], "_TRANSPORT=driver");
        assert_cc(6 == LOG_INFO);
        IOVEC_SET_STRING(iovec[n++], "PRIORITY=6");

        if (!sd_id128_equal(message_id, SD_ID128_NULL)) {
                snprintf(mid, sizeof(mid), LOG_MESSAGE_ID(message_id));
                IOVEC_SET_STRING(iovec[n++], mid);
        }

        m = n;

        va_start(ap, format);
        r = log_format_iovec(iovec, ELEMENTSOF(iovec), &n, false, 0, format, ap);
        /* Error handling below */
        va_end(ap);

        ucred.pid = getpid();
        ucred.uid = getuid();
        ucred.gid = getgid();

        if (r >= 0)
                dispatch_message_real(s, iovec, n, ELEMENTSOF(iovec), &ucred, NULL, NULL, 0, NULL, LOG_INFO, 0);

        while (m < n)
                free(iovec[m++].iov_base);

        if (r < 0) {
                /* We failed to format the message. Emit a warning instead. */
                char buf[LINE_MAX];

                xsprintf(buf, "MESSAGE=Entry printing failed: %s", strerror(-r));

                n = 3;
                IOVEC_SET_STRING(iovec[n++], "PRIORITY=4");
                IOVEC_SET_STRING(iovec[n++], buf);
                dispatch_message_real(s, iovec, n, ELEMENTSOF(iovec), &ucred, NULL, NULL, 0, NULL, LOG_INFO, 0);
        }
}

void server_dispatch_message(
                Server *s,
                struct iovec *iovec, unsigned n, unsigned m,
                const struct ucred *ucred,
                const struct timeval *tv,
                const char *label, size_t label_len,
                const char *unit_id,
                int priority,
                pid_t object_pid) {

        int rl, r;
        _cleanup_free_ char *path = NULL;
        uint64_t available = 0;
        char *c;

        assert(s);
        assert(iovec || n == 0);

        if (n == 0)
                return;

        if (LOG_PRI(priority) > s->max_level_store)
                return;

        /* Stop early in case the information will not be stored
         * in a journal. */
        if (s->storage == STORAGE_NONE)
                return;

        if (!ucred)
                goto finish;

        r = cg_pid_get_path_shifted(ucred->pid, s->cgroup_root, &path);
        if (r < 0)
                goto finish;

        /* example: /user/lennart/3/foobar
         *          /system/dbus.service/foobar
         *
         * So let's cut of everything past the third /, since that is
         * where user directories start */

        c = strchr(path, '/');
        if (c) {
                c = strchr(c+1, '/');
                if (c) {
                        c = strchr(c+1, '/');
                        if (c)
                                *c = 0;
                }
        }

        (void) determine_space(s, false, false, &available, NULL);
        rl = journal_rate_limit_test(s->rate_limit, path, priority & LOG_PRIMASK, available);
        if (rl == 0)
                return;

        /* Write a suppression message if we suppressed something */
        if (rl > 1)
                server_driver_message(s, SD_MESSAGE_JOURNAL_DROPPED,
                                      LOG_MESSAGE("Suppressed %u messages from %s", rl - 1, path),
                                      NULL);

finish:
        dispatch_message_real(s, iovec, n, m, ucred, tv, label, label_len, unit_id, priority, object_pid);
}


static int system_journal_open(Server *s, bool flush_requested) {
        const char *fn;
        int r = 0;

        if (!s->system_journal &&
            (s->storage == STORAGE_PERSISTENT || s->storage == STORAGE_AUTO) &&
            (flush_requested
             || access("/run/systemd/journal/flushed", F_OK) >= 0)) {

                /* If in auto mode: first try to create the machine
                 * path, but not the prefix.
                 *
                 * If in persistent mode: create /var/log/journal and
                 * the machine path */

                if (s->storage == STORAGE_PERSISTENT)
                        (void) mkdir_p("/var/log/journal/", 0755);

                fn = strjoina("/var/log/journal/", SERVER_MACHINE_ID(s));
                (void) mkdir(fn, 0755);

                fn = strjoina(fn, "/system.journal");
                r = open_journal(s, true, fn, O_RDWR|O_CREAT, s->seal, &s->system_metrics, NULL, &s->system_journal);
                if (r >= 0) {
                        server_add_acls(s->system_journal, 0);
                        (void) determine_space_for(s, &s->system_metrics, "/var/log/journal/", "System journal", true, true, NULL, NULL);
                } else if (r < 0) {
                        if (r != -ENOENT && r != -EROFS)
                                log_warning_errno(r, "Failed to open system journal: %m");

                        r = 0;
                }
        }

        if (!s->runtime_journal &&
            (s->storage != STORAGE_NONE)) {

                fn = strjoina("/run/log/journal/", SERVER_MACHINE_ID(s), "/system.journal");

                if (s->system_journal) {

                        /* Try to open the runtime journal, but only
                         * if it already exists, so that we can flush
                         * it into the system journal */

                        r = open_journal(s, false, fn, O_RDWR, false, &s->runtime_metrics, NULL, &s->runtime_journal);
                        if (r < 0) {
                                if (r != -ENOENT)
                                        log_warning_errno(r, "Failed to open runtime journal: %m");

                                r = 0;
                        }

                } else {

                        /* OK, we really need the runtime journal, so create
                         * it if necessary. */

                        (void) mkdir("/run/log", 0755);
                        (void) mkdir("/run/log/journal", 0755);
                        (void) mkdir_parents(fn, 0750);

                        r = open_journal(s, true, fn, O_RDWR|O_CREAT, false, &s->runtime_metrics, NULL, &s->runtime_journal);
                        if (r < 0)
                                return log_error_errno(r, "Failed to open runtime journal: %m");
                }

                if (s->runtime_journal) {
                        server_add_acls(s->runtime_journal, 0);
                        (void) determine_space_for(s, &s->runtime_metrics, "/run/log/journal/", "Runtime journal", true, true, NULL, NULL);
                }
        }

        return r;
}

int server_flush_to_var(Server *s) {
        sd_id128_t machine;
        sd_journal *j = NULL;
        char ts[FORMAT_TIMESPAN_MAX];
        usec_t start;
        unsigned n = 0;
        int r;

        assert(s);

        if (s->storage != STORAGE_AUTO &&
            s->storage != STORAGE_PERSISTENT)
                return 0;

        if (!s->runtime_journal)
                return 0;

        (void) system_journal_open(s, true);

        if (!s->system_journal)
                return 0;

        log_debug("Flushing to /var...");

        start = now(CLOCK_MONOTONIC);

        r = sd_id128_get_machine(&machine);
        if (r < 0)
                return r;

        r = sd_journal_open(&j, SD_JOURNAL_RUNTIME_ONLY);
        if (r < 0)
                return log_error_errno(r, "Failed to read runtime journal: %m");

        sd_journal_set_data_threshold(j, 0);

        SD_JOURNAL_FOREACH(j) {
                Object *o = NULL;
                JournalFile *f;

                f = j->current_file;
                assert(f && f->current_offset > 0);

                n++;

                r = journal_file_move_to_object(f, OBJECT_ENTRY, f->current_offset, &o);
                if (r < 0) {
                        log_error_errno(r, "Can't read entry: %m");
                        goto finish;
                }

                r = journal_file_copy_entry(f, s->system_journal, o, f->current_offset, NULL, NULL, NULL);
                if (r >= 0)
                        continue;

                if (!shall_try_append_again(s->system_journal, r)) {
                        log_error_errno(r, "Can't write entry: %m");
                        goto finish;
                }

                server_rotate(s);
                server_vacuum(s, false, false);

                if (!s->system_journal) {
                        log_notice("Didn't flush runtime journal since rotation of system journal wasn't successful.");
                        r = -EIO;
                        goto finish;
                }

                log_debug("Retrying write.");
                r = journal_file_copy_entry(f, s->system_journal, o, f->current_offset, NULL, NULL, NULL);
                if (r < 0) {
                        log_error_errno(r, "Can't write entry: %m");
                        goto finish;
                }
        }

        r = 0;

finish:
        journal_file_post_change(s->system_journal);

        s->runtime_journal = journal_file_close(s->runtime_journal);

        if (r >= 0)
                (void) rm_rf("/run/log/journal", REMOVE_ROOT);

        sd_journal_close(j);

        server_driver_message(s, SD_ID128_NULL,
                              LOG_MESSAGE("Time spent on flushing to /var is %s for %u entries.",
                                          format_timespan(ts, sizeof(ts), now(CLOCK_MONOTONIC) - start, 0),
                                          n),
                              NULL);

        return r;
}

int server_process_datagram(sd_event_source *es, int fd, uint32_t revents, void *userdata) {
        Server *s = userdata;
        struct ucred *ucred = NULL;
        struct timeval *tv = NULL;
        struct cmsghdr *cmsg;
        char *label = NULL;
        size_t label_len = 0, m;
        struct iovec iovec;
        ssize_t n;
        int *fds = NULL, v = 0;
        unsigned n_fds = 0;

        union {
                struct cmsghdr cmsghdr;

                /* We use NAME_MAX space for the SELinux label
                 * here. The kernel currently enforces no
                 * limit, but according to suggestions from
                 * the SELinux people this will change and it
                 * will probably be identical to NAME_MAX. For
                 * now we use that, but this should be updated
                 * one day when the final limit is known. */
                uint8_t buf[CMSG_SPACE(sizeof(struct ucred)) +
                            CMSG_SPACE(sizeof(struct timeval)) +
                            CMSG_SPACE(sizeof(int)) + /* fd */
                            CMSG_SPACE(NAME_MAX)]; /* selinux label */
        } control = {};

        union sockaddr_union sa = {};

        struct msghdr msghdr = {
                .msg_iov = &iovec,
                .msg_iovlen = 1,
                .msg_control = &control,
                .msg_controllen = sizeof(control),
                .msg_name = &sa,
                .msg_namelen = sizeof(sa),
        };

        assert(s);
        assert(fd == s->native_fd || fd == s->syslog_fd || fd == s->audit_fd);

        if (revents != EPOLLIN) {
                log_error("Got invalid event from epoll for datagram fd: %"PRIx32, revents);
                return -EIO;
        }

        /* Try to get the right size, if we can. (Not all
         * sockets support SIOCINQ, hence we just try, but
         * don't rely on it. */
        (void) ioctl(fd, SIOCINQ, &v);

        /* Fix it up, if it is too small. We use the same fixed value as auditd here. Awful! */
        m = PAGE_ALIGN(MAX3((size_t) v + 1,
                            (size_t) LINE_MAX,
                            ALIGN(sizeof(struct nlmsghdr)) + ALIGN((size_t) MAX_AUDIT_MESSAGE_LENGTH)) + 1);

        if (!GREEDY_REALLOC(s->buffer, s->buffer_size, m))
                return log_oom();

        iovec.iov_base = s->buffer;
        iovec.iov_len = s->buffer_size - 1; /* Leave room for trailing NUL we add later */

        n = recvmsg(fd, &msghdr, MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
        if (n < 0) {
                if (errno == EINTR || errno == EAGAIN)
                        return 0;

                return log_error_errno(errno, "recvmsg() failed: %m");
        }

        CMSG_FOREACH(cmsg, &msghdr) {

                if (cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_CREDENTIALS &&
                    cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred)))
                        ucred = (struct ucred*) CMSG_DATA(cmsg);
                else if (cmsg->cmsg_level == SOL_SOCKET &&
                         cmsg->cmsg_type == SCM_SECURITY) {
                        label = (char*) CMSG_DATA(cmsg);
                        label_len = cmsg->cmsg_len - CMSG_LEN(0);
                } else if (cmsg->cmsg_level == SOL_SOCKET &&
                           cmsg->cmsg_type == SO_TIMESTAMP &&
                           cmsg->cmsg_len == CMSG_LEN(sizeof(struct timeval)))
                        tv = (struct timeval*) CMSG_DATA(cmsg);
                else if (cmsg->cmsg_level == SOL_SOCKET &&
                         cmsg->cmsg_type == SCM_RIGHTS) {
                        fds = (int*) CMSG_DATA(cmsg);
                        n_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                }
        }

        /* And a trailing NUL, just in case */
        s->buffer[n] = 0;

        if (fd == s->syslog_fd) {
                if (n > 0 && n_fds == 0)
                        server_process_syslog_message(s, strstrip(s->buffer), ucred, tv, label, label_len);
                else if (n_fds > 0)
                        log_warning("Got file descriptors via syslog socket. Ignoring.");

        } else if (fd == s->native_fd) {
                if (n > 0 && n_fds == 0)
                        server_process_native_message(s, s->buffer, n, ucred, tv, label, label_len);
                else if (n == 0 && n_fds == 1)
                        server_process_native_file(s, fds[0], ucred, tv, label, label_len);
                else if (n_fds > 0)
                        log_warning("Got too many file descriptors via native socket. Ignoring.");

        } else {
                assert(fd == s->audit_fd);

                if (n > 0 && n_fds == 0)
                        server_process_audit_message(s, s->buffer, n, ucred, &sa, msghdr.msg_namelen);
                else if (n_fds > 0)
                        log_warning("Got file descriptors via audit socket. Ignoring.");
        }

        close_many(fds, n_fds);
        return 0;
}

static int dispatch_sigusr1(sd_event_source *es, const struct signalfd_siginfo *si, void *userdata) {
        Server *s = userdata;
        int r;

        assert(s);

        log_info("Received request to flush runtime journal from PID " PID_FMT, si->ssi_pid);

        server_flush_to_var(s);
        server_sync(s);
        server_vacuum(s, false, false);

        r = touch("/run/systemd/journal/flushed");
        if (r < 0)
                log_warning_errno(r, "Failed to touch /run/systemd/journal/flushed, ignoring: %m");

        return 0;
}

static int dispatch_sigusr2(sd_event_source *es, const struct signalfd_siginfo *si, void *userdata) {
        Server *s = userdata;
        int r;

        assert(s);

        log_info("Received request to rotate journal from PID " PID_FMT, si->ssi_pid);
        server_rotate(s);
        server_vacuum(s, true, true);

        /* Let clients know when the most recent rotation happened. */
        r = write_timestamp_file_atomic("/run/systemd/journal/rotated", now(CLOCK_MONOTONIC));
        if (r < 0)
                log_warning_errno(r, "Failed to write /run/systemd/journal/rotated, ignoring: %m");

        return 0;
}

static int dispatch_sigterm(sd_event_source *es, const struct signalfd_siginfo *si, void *userdata) {
        Server *s = userdata;

        assert(s);

        log_received_signal(LOG_INFO, si);

        sd_event_exit(s->event, 0);
        return 0;
}

static int dispatch_sigrtmin1(sd_event_source *es, const struct signalfd_siginfo *si, void *userdata) {
        Server *s = userdata;
        int r;

        assert(s);

        log_debug("Received request to sync from PID " PID_FMT, si->ssi_pid);

        server_sync(s);

        /* Let clients know when the most recent sync happened. */
        r = write_timestamp_file_atomic("/run/systemd/journal/synced", now(CLOCK_MONOTONIC));
        if (r < 0)
                log_warning_errno(r, "Failed to write /run/systemd/journal/synced, ignoring: %m");

        return 0;
}

static int setup_signals(Server *s) {
        int r;

        assert(s);

        assert(sigprocmask_many(SIG_SETMASK, NULL, SIGINT, SIGTERM, SIGUSR1, SIGUSR2, SIGRTMIN+1, -1) >= 0);

        r = sd_event_add_signal(s->event, &s->sigusr1_event_source, SIGUSR1, dispatch_sigusr1, s);
        if (r < 0)
                return r;

        r = sd_event_add_signal(s->event, &s->sigusr2_event_source, SIGUSR2, dispatch_sigusr2, s);
        if (r < 0)
                return r;

        r = sd_event_add_signal(s->event, &s->sigterm_event_source, SIGTERM, dispatch_sigterm, s);
        if (r < 0)
                return r;

        /* Let's process SIGTERM late, so that we flush all queued
         * messages to disk before we exit */
        r = sd_event_source_set_priority(s->sigterm_event_source, SD_EVENT_PRIORITY_NORMAL+20);
        if (r < 0)
                return r;

        /* When journald is invoked on the terminal (when debugging),
         * it's useful if C-c is handled equivalent to SIGTERM. */
        r = sd_event_add_signal(s->event, &s->sigint_event_source, SIGINT, dispatch_sigterm, s);
        if (r < 0)
                return r;

        r = sd_event_source_set_priority(s->sigint_event_source, SD_EVENT_PRIORITY_NORMAL+20);
        if (r < 0)
                return r;

        /* SIGRTMIN+1 causes an immediate sync. We process this very
         * late, so that everything else queued at this point is
         * really written to disk. Clients can watch
         * /run/systemd/journal/synced with inotify until its mtime
         * changes to see when a sync happened. */
        r = sd_event_add_signal(s->event, &s->sigrtmin1_event_source, SIGRTMIN+1, dispatch_sigrtmin1, s);
        if (r < 0)
                return r;

        r = sd_event_source_set_priority(s->sigrtmin1_event_source, SD_EVENT_PRIORITY_NORMAL+15);
        if (r < 0)
                return r;

        return 0;
}

static int server_parse_proc_cmdline(Server *s) {
        _cleanup_free_ char *line = NULL;
        const char *p;
        int r;

        r = proc_cmdline(&line);
        if (r < 0) {
                log_warning_errno(r, "Failed to read /proc/cmdline, ignoring: %m");
                return 0;
        }

        p = line;
        for(;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&p, &word, NULL, 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse journald syntax \"%s\": %m", line);

                if (r == 0)
                        break;

                if (startswith(word, "systemd.journald.forward_to_syslog=")) {
                        r = parse_boolean(word + 35);
                        if (r < 0)
                                log_warning("Failed to parse forward to syslog switch %s. Ignoring.", word + 35);
                        else
                                s->forward_to_syslog = r;
                } else if (startswith(word, "systemd.journald.forward_to_kmsg=")) {
                        r = parse_boolean(word + 33);
                        if (r < 0)
                                log_warning("Failed to parse forward to kmsg switch %s. Ignoring.", word + 33);
                        else
                                s->forward_to_kmsg = r;
                } else if (startswith(word, "systemd.journald.forward_to_console=")) {
                        r = parse_boolean(word + 36);
                        if (r < 0)
                                log_warning("Failed to parse forward to console switch %s. Ignoring.", word + 36);
                        else
                                s->forward_to_console = r;
                } else if (startswith(word, "systemd.journald.forward_to_wall=")) {
                        r = parse_boolean(word + 33);
                        if (r < 0)
                                log_warning("Failed to parse forward to wall switch %s. Ignoring.", word + 33);
                        else
                                s->forward_to_wall = r;
                } else if (startswith(word, "systemd.journald"))
                        log_warning("Invalid systemd.journald parameter. Ignoring.");
        }

        /* do not warn about state here, since probably systemd already did */
        return 0;
}

static int server_parse_config_file(Server *s) {
        assert(s);

        return config_parse_many(PKGSYSCONFDIR "/journald.conf",
                                 CONF_PATHS_NULSTR("systemd/journald.conf.d"),
                                 "Journal\0",
                                 config_item_perf_lookup, journald_gperf_lookup,
                                 false, s);
}

static int server_dispatch_sync(sd_event_source *es, usec_t t, void *userdata) {
        Server *s = userdata;

        assert(s);

        server_sync(s);
        return 0;
}

int server_schedule_sync(Server *s, int priority) {
        int r;

        assert(s);

        if (priority <= LOG_CRIT) {
                /* Immediately sync to disk when this is of priority CRIT, ALERT, EMERG */
                server_sync(s);
                return 0;
        }

        if (s->sync_scheduled)
                return 0;

        if (s->sync_interval_usec > 0) {
                usec_t when;

                r = sd_event_now(s->event, CLOCK_MONOTONIC, &when);
                if (r < 0)
                        return r;

                when += s->sync_interval_usec;

                if (!s->sync_event_source) {
                        r = sd_event_add_time(
                                        s->event,
                                        &s->sync_event_source,
                                        CLOCK_MONOTONIC,
                                        when, 0,
                                        server_dispatch_sync, s);
                        if (r < 0)
                                return r;

                        r = sd_event_source_set_priority(s->sync_event_source, SD_EVENT_PRIORITY_IMPORTANT);
                } else {
                        r = sd_event_source_set_time(s->sync_event_source, when);
                        if (r < 0)
                                return r;

                        r = sd_event_source_set_enabled(s->sync_event_source, SD_EVENT_ONESHOT);
                }
                if (r < 0)
                        return r;

                s->sync_scheduled = true;
        }

        return 0;
}

static int dispatch_hostname_change(sd_event_source *es, int fd, uint32_t revents, void *userdata) {
        Server *s = userdata;

        assert(s);

        server_cache_hostname(s);
        return 0;
}

static int server_open_hostname(Server *s) {
        int r;

        assert(s);

        s->hostname_fd = open("/proc/sys/kernel/hostname", O_RDONLY|O_CLOEXEC|O_NDELAY|O_NOCTTY);
        if (s->hostname_fd < 0)
                return log_error_errno(errno, "Failed to open /proc/sys/kernel/hostname: %m");

        r = sd_event_add_io(s->event, &s->hostname_event_source, s->hostname_fd, 0, dispatch_hostname_change, s);
        if (r < 0) {
                /* kernels prior to 3.2 don't support polling this file. Ignore
                 * the failure. */
                if (r == -EPERM) {
                        log_warning_errno(r, "Failed to register hostname fd in event loop, ignoring: %m");
                        s->hostname_fd = safe_close(s->hostname_fd);
                        return 0;
                }

                return log_error_errno(r, "Failed to register hostname fd in event loop: %m");
        }

        r = sd_event_source_set_priority(s->hostname_event_source, SD_EVENT_PRIORITY_IMPORTANT-10);
        if (r < 0)
                return log_error_errno(r, "Failed to adjust priority of host name event source: %m");

        return 0;
}

static int dispatch_notify_event(sd_event_source *es, int fd, uint32_t revents, void *userdata) {
        Server *s = userdata;
        int r;

        assert(s);
        assert(s->notify_event_source == es);
        assert(s->notify_fd == fd);

        /* The $NOTIFY_SOCKET is writable again, now send exactly one
         * message on it. Either it's the wtachdog event, the initial
         * READY=1 event or an stdout stream event. If there's nothing
         * to write anymore, turn our event source off. The next time
         * there's something to send it will be turned on again. */

        if (!s->sent_notify_ready) {
                static const char p[] =
                        "READY=1\n"
                        "STATUS=Processing requests...";
                ssize_t l;

                l = send(s->notify_fd, p, strlen(p), MSG_DONTWAIT);
                if (l < 0) {
                        if (errno == EAGAIN)
                                return 0;

                        return log_error_errno(errno, "Failed to send READY=1 notification message: %m");
                }

                s->sent_notify_ready = true;
                log_debug("Sent READY=1 notification.");

        } else if (s->send_watchdog) {

                static const char p[] =
                        "WATCHDOG=1";

                ssize_t l;

                l = send(s->notify_fd, p, strlen(p), MSG_DONTWAIT);
                if (l < 0) {
                        if (errno == EAGAIN)
                                return 0;

                        return log_error_errno(errno, "Failed to send WATCHDOG=1 notification message: %m");
                }

                s->send_watchdog = false;
                log_debug("Sent WATCHDOG=1 notification.");

        } else if (s->stdout_streams_notify_queue)
                /* Dispatch one stream notification event */
                stdout_stream_send_notify(s->stdout_streams_notify_queue);

        /* Leave us enabled if there's still more to to do. */
        if (s->send_watchdog || s->stdout_streams_notify_queue)
                return 0;

        /* There was nothing to do anymore, let's turn ourselves off. */
        r = sd_event_source_set_enabled(es, SD_EVENT_OFF);
        if (r < 0)
                return log_error_errno(r, "Failed to turn off notify event source: %m");

        return 0;
}

static int dispatch_watchdog(sd_event_source *es, uint64_t usec, void *userdata) {
        Server *s = userdata;
        int r;

        assert(s);

        s->send_watchdog = true;

        r = sd_event_source_set_enabled(s->notify_event_source, SD_EVENT_ON);
        if (r < 0)
                log_warning_errno(r, "Failed to turn on notify event source: %m");

        r = sd_event_source_set_time(s->watchdog_event_source, usec + s->watchdog_usec / 2);
        if (r < 0)
                return log_error_errno(r, "Failed to restart watchdog event source: %m");

        r = sd_event_source_set_enabled(s->watchdog_event_source, SD_EVENT_ON);
        if (r < 0)
                return log_error_errno(r, "Failed to enable watchdog event source: %m");

        return 0;
}

static int server_connect_notify(Server *s) {
        union sockaddr_union sa = {
                .un.sun_family = AF_UNIX,
        };
        const char *e;
        int r;

        assert(s);
        assert(s->notify_fd < 0);
        assert(!s->notify_event_source);

        /*
          So here's the problem: we'd like to send notification
          messages to PID 1, but we cannot do that via sd_notify(),
          since that's synchronous, and we might end up blocking on
          it. Specifically: given that PID 1 might block on
          dbus-daemon during IPC, and dbus-daemon is logging to us,
          and might hence block on us, we might end up in a deadlock
          if we block on sending PID 1 notification messages -- by
          generating a full blocking circle. To avoid this, let's
          create a non-blocking socket, and connect it to the
          notification socket, and then wait for POLLOUT before we
          send anything. This should efficiently avoid any deadlocks,
          as we'll never block on PID 1, hence PID 1 can safely block
          on dbus-daemon which can safely block on us again.

          Don't think that this issue is real? It is, see:
          https://github.com/systemd/systemd/issues/1505
        */

        e = getenv("NOTIFY_SOCKET");
        if (!e)
                return 0;

        if ((e[0] != '@' && e[0] != '/') || e[1] == 0) {
                log_error("NOTIFY_SOCKET set to an invalid value: %s", e);
                return -EINVAL;
        }

        if (strlen(e) > sizeof(sa.un.sun_path)) {
                log_error("NOTIFY_SOCKET path too long: %s", e);
                return -EINVAL;
        }

        s->notify_fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
        if (s->notify_fd < 0)
                return log_error_errno(errno, "Failed to create notify socket: %m");

        (void) fd_inc_sndbuf(s->notify_fd, NOTIFY_SNDBUF_SIZE);

        strncpy(sa.un.sun_path, e, sizeof(sa.un.sun_path));
        if (sa.un.sun_path[0] == '@')
                sa.un.sun_path[0] = 0;

        r = connect(s->notify_fd, &sa.sa, offsetof(struct sockaddr_un, sun_path) + strlen(e));
        if (r < 0)
                return log_error_errno(errno, "Failed to connect to notify socket: %m");

        r = sd_event_add_io(s->event, &s->notify_event_source, s->notify_fd, EPOLLOUT, dispatch_notify_event, s);
        if (r < 0)
                return log_error_errno(r, "Failed to watch notification socket: %m");

        if (sd_watchdog_enabled(false, &s->watchdog_usec) > 0) {
                s->send_watchdog = true;

                r = sd_event_add_time(s->event, &s->watchdog_event_source, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + s->watchdog_usec/2, s->watchdog_usec/4, dispatch_watchdog, s);
                if (r < 0)
                        return log_error_errno(r, "Failed to add watchdog time event: %m");
        }

        /* This should fire pretty soon, which we'll use to send the
         * READY=1 event. */

        return 0;
}

int server_init(Server *s) {
        _cleanup_fdset_free_ FDSet *fds = NULL;
        int n, r, fd;
        bool no_sockets;

        assert(s);

        zero(*s);
        s->syslog_fd = s->native_fd = s->stdout_fd = s->dev_kmsg_fd = s->audit_fd = s->hostname_fd = s->notify_fd = -1;
        s->compress = true;
        s->seal = true;

        s->watchdog_usec = USEC_INFINITY;

        s->sync_interval_usec = DEFAULT_SYNC_INTERVAL_USEC;
        s->sync_scheduled = false;

        s->rate_limit_interval = DEFAULT_RATE_LIMIT_INTERVAL;
        s->rate_limit_burst = DEFAULT_RATE_LIMIT_BURST;

        s->forward_to_wall = true;

        s->max_file_usec = DEFAULT_MAX_FILE_USEC;

        s->max_level_store = LOG_DEBUG;
        s->max_level_syslog = LOG_DEBUG;
        s->max_level_kmsg = LOG_NOTICE;
        s->max_level_console = LOG_INFO;
        s->max_level_wall = LOG_EMERG;

        journal_reset_metrics(&s->system_metrics);
        journal_reset_metrics(&s->runtime_metrics);

        server_parse_config_file(s);
        server_parse_proc_cmdline(s);

        if (!!s->rate_limit_interval ^ !!s->rate_limit_burst) {
                log_debug("Setting both rate limit interval and burst from "USEC_FMT",%u to 0,0",
                          s->rate_limit_interval, s->rate_limit_burst);
                s->rate_limit_interval = s->rate_limit_burst = 0;
        }

        (void) mkdir_p("/run/systemd/journal", 0755);

        s->user_journals = ordered_hashmap_new(NULL);
        if (!s->user_journals)
                return log_oom();

        s->mmap = mmap_cache_new();
        if (!s->mmap)
                return log_oom();

        r = sd_event_default(&s->event);
        if (r < 0)
                return log_error_errno(r, "Failed to create event loop: %m");

        n = sd_listen_fds(true);
        if (n < 0)
                return log_error_errno(n, "Failed to read listening file descriptors from environment: %m");

        for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; fd++) {

                if (sd_is_socket_unix(fd, SOCK_DGRAM, -1, "/run/systemd/journal/socket", 0) > 0) {

                        if (s->native_fd >= 0) {
                                log_error("Too many native sockets passed.");
                                return -EINVAL;
                        }

                        s->native_fd = fd;

                } else if (sd_is_socket_unix(fd, SOCK_STREAM, 1, "/run/systemd/journal/stdout", 0) > 0) {

                        if (s->stdout_fd >= 0) {
                                log_error("Too many stdout sockets passed.");
                                return -EINVAL;
                        }

                        s->stdout_fd = fd;

                } else if (sd_is_socket_unix(fd, SOCK_DGRAM, -1, "/dev/log", 0) > 0 ||
                           sd_is_socket_unix(fd, SOCK_DGRAM, -1, "/run/systemd/journal/dev-log", 0) > 0) {

                        if (s->syslog_fd >= 0) {
                                log_error("Too many /dev/log sockets passed.");
                                return -EINVAL;
                        }

                        s->syslog_fd = fd;

                } else if (sd_is_socket(fd, AF_NETLINK, SOCK_RAW, -1) > 0) {

                        if (s->audit_fd >= 0) {
                                log_error("Too many audit sockets passed.");
                                return -EINVAL;
                        }

                        s->audit_fd = fd;

                } else {

                        if (!fds) {
                                fds = fdset_new();
                                if (!fds)
                                        return log_oom();
                        }

                        r = fdset_put(fds, fd);
                        if (r < 0)
                                return log_oom();
                }
        }

        /* Try to restore streams, but don't bother if this fails */
        (void) server_restore_streams(s, fds);

        if (fdset_size(fds) > 0) {
                log_warning("%u unknown file descriptors passed, closing.", fdset_size(fds));
                fds = fdset_free(fds);
        }

        no_sockets = s->native_fd < 0 && s->stdout_fd < 0 && s->syslog_fd < 0 && s->audit_fd < 0;

        /* always open stdout, syslog, native, and kmsg sockets */

        /* systemd-journald.socket: /run/systemd/journal/stdout */
        r = server_open_stdout_socket(s);
        if (r < 0)
                return r;

        /* systemd-journald-dev-log.socket: /run/systemd/journal/dev-log */
        r = server_open_syslog_socket(s);
        if (r < 0)
                return r;

        /* systemd-journald.socket: /run/systemd/journal/socket */
        r = server_open_native_socket(s);
        if (r < 0)
                return r;

        /* /dev/ksmg */
        r = server_open_dev_kmsg(s);
        if (r < 0)
                return r;

        /* Unless we got *some* sockets and not audit, open audit socket */
        if (s->audit_fd >= 0 || no_sockets) {
                r = server_open_audit(s);
                if (r < 0)
                        return r;
        }

        r = server_open_kernel_seqnum(s);
        if (r < 0)
                return r;

        r = server_open_hostname(s);
        if (r < 0)
                return r;

        r = setup_signals(s);
        if (r < 0)
                return r;

        s->udev = udev_new();
        if (!s->udev)
                return -ENOMEM;

        s->rate_limit = journal_rate_limit_new(s->rate_limit_interval, s->rate_limit_burst);
        if (!s->rate_limit)
                return -ENOMEM;

        r = cg_get_root_path(&s->cgroup_root);
        if (r < 0)
                return r;

        server_cache_hostname(s);
        server_cache_boot_id(s);
        server_cache_machine_id(s);

        (void) server_connect_notify(s);

        return system_journal_open(s, false);
}

void server_maybe_append_tags(Server *s) {
#ifdef HAVE_GCRYPT
        JournalFile *f;
        Iterator i;
        usec_t n;

        n = now(CLOCK_REALTIME);

        if (s->system_journal)
                journal_file_maybe_append_tag(s->system_journal, n);

        ORDERED_HASHMAP_FOREACH(f, s->user_journals, i)
                journal_file_maybe_append_tag(f, n);
#endif
}

void server_done(Server *s) {
        JournalFile *f;
        assert(s);

        while (s->stdout_streams)
                stdout_stream_free(s->stdout_streams);

        if (s->system_journal)
                journal_file_close(s->system_journal);

        if (s->runtime_journal)
                journal_file_close(s->runtime_journal);

        while ((f = ordered_hashmap_steal_first(s->user_journals)))
                journal_file_close(f);

        ordered_hashmap_free(s->user_journals);

        sd_event_source_unref(s->syslog_event_source);
        sd_event_source_unref(s->native_event_source);
        sd_event_source_unref(s->stdout_event_source);
        sd_event_source_unref(s->dev_kmsg_event_source);
        sd_event_source_unref(s->audit_event_source);
        sd_event_source_unref(s->sync_event_source);
        sd_event_source_unref(s->sigusr1_event_source);
        sd_event_source_unref(s->sigusr2_event_source);
        sd_event_source_unref(s->sigterm_event_source);
        sd_event_source_unref(s->sigint_event_source);
        sd_event_source_unref(s->sigrtmin1_event_source);
        sd_event_source_unref(s->hostname_event_source);
        sd_event_source_unref(s->notify_event_source);
        sd_event_source_unref(s->watchdog_event_source);
        sd_event_unref(s->event);

        safe_close(s->syslog_fd);
        safe_close(s->native_fd);
        safe_close(s->stdout_fd);
        safe_close(s->dev_kmsg_fd);
        safe_close(s->audit_fd);
        safe_close(s->hostname_fd);
        safe_close(s->notify_fd);

        if (s->rate_limit)
                journal_rate_limit_free(s->rate_limit);

        if (s->kernel_seqnum)
                munmap(s->kernel_seqnum, sizeof(uint64_t));

        free(s->buffer);
        free(s->tty_path);
        free(s->cgroup_root);
        free(s->hostname_field);

        if (s->mmap)
                mmap_cache_unref(s->mmap);

        udev_unref(s->udev);
}

static const char* const storage_table[_STORAGE_MAX] = {
        [STORAGE_AUTO] = "auto",
        [STORAGE_VOLATILE] = "volatile",
        [STORAGE_PERSISTENT] = "persistent",
        [STORAGE_NONE] = "none"
};

DEFINE_STRING_TABLE_LOOKUP(storage, Storage);
DEFINE_CONFIG_PARSE_ENUM(config_parse_storage, storage, Storage, "Failed to parse storage setting");

static const char* const split_mode_table[_SPLIT_MAX] = {
        [SPLIT_LOGIN] = "login",
        [SPLIT_UID] = "uid",
        [SPLIT_NONE] = "none",
};

DEFINE_STRING_TABLE_LOOKUP(split_mode, SplitMode);
DEFINE_CONFIG_PARSE_ENUM(config_parse_split_mode, split_mode, SplitMode, "Failed to parse split mode setting");
