/* SPDX-License-Identifier: LGPL-2.1+ */

#include <sys/statvfs.h>
#include "dirent-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "string-util.h"
#include "time-util.h"
#include "user-util.h"
#include "util.h"
#include "coredump-ratelimit.h"
#include "sd-id128.h"

#define MAX_COMM_LENGTH 16

static int comm_from_file_name(const char *file_name, char *comm) {
        const char *p;
        int i = 0;

        /* check if the file is a core dump file */
        p = startswith(file_name, "core.");
        if (!p) {
                /* check if the file is an inprogress core dump file */
                p = startswith(file_name, ".#core.");
                if(!p)
                        return -EINVAL;
        }

        /* Retrieve the process name (COMM) */
        while (*p != '.' && i < MAX_COMM_LENGTH){
                comm[i] = *p;
                p++;
                i++;
        }
        comm[i] = '\0';
        return 0;
}

static int bootid_from_file_name(const char *file_name, char *bootid) {
        const char *p;
        int i = 0;

        /* check if the file is a core dump file */
        p = startswith(file_name, "core.");
        if (!p) {
                /* check if the file is an inprogress core dump file */
                p = startswith(file_name, ".#core.");
                if(!p)
                        return -EINVAL;
        }

        /* Skip the comm field */
        p = strchr(p, '.');
        if (!p)
                return -EINVAL;
        p++;

        /* Skip the uid field */
        p = strchr(p, '.');
        if (!p)
                return -EINVAL;
        p++;

        /* Retrieve the Bootid */
        while (*p != '.' && i < SD_ID128_STRING_MAX - 1){
                bootid[i] = *p;
                p++;
                i++;
        }
        bootid[i] = '\0';
        return 0;
}

int coredump_ratelimit(const char* process_name, usec_t interval, unsigned burst) {

        _cleanup_closedir_ DIR *d = NULL;
        struct dirent *de;
        unsigned int count = 0;
        usec_t curr_ts;
        sd_id128_t boot = {};
        char boot_id[SD_ID128_STRING_MAX];
        int r;

        /* If the process name is not given, then return -1. No rate limit happens.*/
        if(process_name == NULL)
                return -1;

        /* either if burst or interval is zero then ratelimit is disabled */
        if(burst == 0 || interval == 0)
                return 0;  /* No ratelimit */

        /* Whenever a daemon crashes, we compare the number of core dump files present in the
         * /var/lib/systemd/coredump belonging to that daemon and generated within the
         * rate limit interval with the rate limit. If the limit is reached, the core dump
         * will not be processed further for generation.
         */

        d = opendir("/var/lib/systemd/coredump");

        if (!d) {
                if (errno == ENOENT)
                        return -1;
                return log_error_errno(errno, "Can't open coredump directory: %m");
        }

        /* get the current timestamp */
        curr_ts = now(CLOCK_REALTIME);
        log_debug("Current time=%"PRIu64" usecs", curr_ts);
        log_debug("Ratelimit interval=%"PRIu64" usecs", interval);
        /* Known limitation :
         * If system time gets changed (advanced or delayed), then it results in a new
         * rate limit window allowing the generation of new core dumps until the limit
         * is reached again.
         */

        r = sd_id128_get_boot(&boot);
        if (r < 0)
                return r;
        snprintf(boot_id , SD_ID128_STRING_MAX, SD_ID128_FORMAT_STR , SD_ID128_FORMAT_VAL(boot));
        FOREACH_DIRENT_ALL(de, d, goto fail) {
                char comm[MAX_COMM_LENGTH + 1];
                char f_boot_id[SD_ID128_STRING_MAX];
                struct stat st;
                usec_t ts;

                r = comm_from_file_name(de->d_name, comm);
                if (r < 0)
                        continue;

                /* Core file belongs to the crashing process */
                if (strcmp(comm, process_name) == 0) {
                        r = bootid_from_file_name(de->d_name, f_boot_id);
                        if(r < 0)
                                continue;

                        log_debug("Processing core file named is %s", de->d_name);
                        /* Core file is generated in the current boot */
                        if(strcmp(f_boot_id, boot_id) == 0) {
                                if (fstatat(dirfd(d), de->d_name, &st,
                                                        AT_NO_AUTOMOUNT|AT_SYMLINK_NOFOLLOW) < 0) {
                                        if (errno == ENOENT)
                                                continue;
                                        log_warning_errno(errno, "Failed to stat /var/lib/systemd/coredump/%s: %m",
                                                        de->d_name);
                                        continue;
                                }

                                if (!S_ISREG(st.st_mode))
                                        continue;

                                ts = timespec_load(&st.st_mtim);

                                log_debug("Core file timestamp=%"PRIu64" usecs", ts);
                                /* If the core file is generated within the rate limit interval then increment the count */
                                if((ts < curr_ts) && (ts > (curr_ts - interval))) {
                                        count++;
                                        /* Enable rate limiting and avoid core dump generation
                                         * if the number of core dumps generated for the crashing process
                                         * is greater or equal to the rate limit burst */
                                        if(count >= burst)
                                                return count;
                                }
                        }
                }
        }
        return 0;  /* process the coredump generation */

fail:
        return log_error_errno(errno, "Failed to read directory: %m");
}
