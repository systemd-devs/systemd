/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "libudev.h"

#define handle_error_errno(error, msg) \
        ({                             \
                errno = abs(error);    \
                perror(msg);           \
                EXIT_FAILURE;          \
        })

static void *thread(void *p) {
        struct udev_device **d = p;

        *d = udev_device_unref(*d);

        return NULL;
}

int main(int argc, char *argv[]) {
        struct udev_device *loopback;
        struct udev_list_entry *entry, *e;
        pthread_t t;
        int r;

        loopback = udev_device_new_from_syspath(NULL, "/sys/class/net/lo");
        if (!loopback)
                return handle_error_errno(errno, "Failed to create loopback device object");

        entry = udev_device_get_properties_list_entry(loopback);
        udev_list_entry_foreach(e, entry)
                        printf("%s=%s\n", udev_list_entry_get_name(e), udev_list_entry_get_value(e));

        r = pthread_create(&t, NULL, thread, &loopback);
        if (r != 0)
                return handle_error_errno(r, "Failed to create thread");

        r = pthread_join(t, NULL);
        if (r != 0)
                return handle_error_errno(r, "Failed to wait thread finished");

        if (loopback)
                return handle_error_errno(r, "loopback device is not unref()ed");

        return 0;
}
