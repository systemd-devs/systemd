/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "alloc-util.h"
#include "devnum-util.h"
#include "fileio.h"
#include "initrd-util.h"
#include "log.h"

int main(int argc, char *argv[]) {
        const char *device, *offset;
        struct stat st;
        int r;

        if (argc != 2) {
                log_error("This program expects one argument.");
                return EXIT_FAILURE;
        }

        log_setup();

        umask(0022);

        /* Refuse to run unless we are in an initrd() */
        if (!in_initrd())
                return EXIT_SUCCESS;

        device = argv[1];
        offset = getenv("SYSTEMD_HIBERNATE_RESUME_OFFSET");

        if (stat(device, &st) < 0) {
                log_error_errno(errno, "Failed to stat '%s': %m", device);
                return EXIT_FAILURE;
        }

        if (!S_ISBLK(st.st_mode)) {
                log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Resume device '%s' is not a block device.", device);
                return EXIT_FAILURE;
        }

        if (offset) {
                if (access("/sys/power/resume_offset", W_OK) < 0) {
                        if (errno == ENOENT)
                                log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                                "Kernel too old, can't set resume_offset=%s for device '%s'.",
                                                offset, device);
                        else
                                log_error_errno(errno, "/sys/power/resume_offset not writable: %m");

                        return EXIT_FAILURE;
                }

                r = write_string_file("/sys/power/resume_offset", offset, WRITE_STRING_FILE_DISABLE_BUFFER);
                if (r < 0) {
                        log_error_errno(r,
                                        "Failed to write swap file offset %s to /sys/power/resume_offset for device '%s': %m",
                                        offset, device);
                        return EXIT_FAILURE;
                }
        }

        r = write_string_file("/sys/power/resume", FORMAT_DEVNUM(st.st_rdev), WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0) {
                log_error_errno(r, "Failed to write '" DEVNUM_FORMAT_STR "' to /sys/power/resume: %m", DEVNUM_FORMAT_VAL(st.st_rdev));
                return EXIT_FAILURE;
        }

        /*
         * The write above shall not return.
         *
         * However, failed resume is a normal condition (may mean that there is
         * no hibernation image).
         */

        log_info("Could not resume from device '%s' (" DEVNUM_FORMAT_STR ")%s%s.",
                 device,
                 DEVNUM_FORMAT_VAL(st.st_rdev),
                 offset ? " offset " : "",
                 strempty(offset));
        return EXIT_SUCCESS;
}
