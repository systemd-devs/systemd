/***
  This file is part of systemd.

  Copyright 2008-2012 Kay Sievers <kay@vrfy.org>

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

#include <getopt.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "libudev.h"

#include "fd-util.h"
#include "log.h"
#include "stdio-util.h"
#include "string-util.h"
#include "udev-util.h"
#include "util.h"

static void print_device(struct udev_device *device) {
        const char *str;
        dev_t devnum;
        int count;
        struct udev_list_entry *list_entry;

        log_info("*** device: %p ***", device);
        str = udev_device_get_action(device);
        if (str != NULL)
                log_info("action:    '%s'", str);

        str = udev_device_get_syspath(device);
        log_info("syspath:   '%s'", str);

        str = udev_device_get_sysname(device);
        log_info("sysname:   '%s'", str);

        str = udev_device_get_sysnum(device);
        if (str != NULL)
                log_info("sysnum:    '%s'", str);

        str = udev_device_get_devpath(device);
        log_info("devpath:   '%s'", str);

        str = udev_device_get_subsystem(device);
        if (str != NULL)
                log_info("subsystem: '%s'", str);

        str = udev_device_get_devtype(device);
        if (str != NULL)
                log_info("devtype:   '%s'", str);

        str = udev_device_get_driver(device);
        if (str != NULL)
                log_info("driver:    '%s'", str);

        str = udev_device_get_devnode(device);
        if (str != NULL)
                log_info("devname:   '%s'", str);

        devnum = udev_device_get_devnum(device);
        if (major(devnum) > 0)
                log_info("devnum:    %u:%u", major(devnum), minor(devnum));

        count = 0;
        udev_list_entry_foreach(list_entry, udev_device_get_devlinks_list_entry(device)) {
                log_info("link:      '%s'", udev_list_entry_get_name(list_entry));
                count++;
        }
        if (count > 0)
                log_info("found %i links", count);

        count = 0;
        udev_list_entry_foreach(list_entry, udev_device_get_properties_list_entry(device)) {
                log_info("property:  '%s=%s'",
                       udev_list_entry_get_name(list_entry),
                       udev_list_entry_get_value(list_entry));
                count++;
        }
        if (count > 0)
                log_info("found %i properties", count);

        str = udev_device_get_property_value(device, "MAJOR");
        if (str != NULL)
                log_info("MAJOR: '%s'", str);

        str = udev_device_get_sysattr_value(device, "dev");
        if (str != NULL)
                log_info("attr{dev}: '%s'", str);
}

static void test_device(struct udev *udev, const char *syspath) {
        _cleanup_udev_device_unref_ struct udev_device *device;

        log_info("looking at device: %s", syspath);
        device = udev_device_new_from_syspath(udev, syspath);
        if (device == NULL)
                log_warning_errno(errno, "udev_device_new_from_syspath: %m");
        else
                print_device(device);
}

static void test_device_parents(struct udev *udev, const char *syspath) {
        _cleanup_udev_device_unref_ struct udev_device *device;
        struct udev_device *device_parent;

        log_info("looking at device: %s", syspath);
        device = udev_device_new_from_syspath(udev, syspath);
        if (device == NULL)
                return;

        log_info("looking at parents");
        device_parent = device;
        do {
                print_device(device_parent);
                device_parent = udev_device_get_parent(device_parent);
        } while (device_parent != NULL);

        log_info("looking at parents again");
        device_parent = device;
        do {
                print_device(device_parent);
                device_parent = udev_device_get_parent(device_parent);
        } while (device_parent != NULL);
}

static void test_device_devnum(struct udev *udev) {
        dev_t devnum = makedev(1, 3);
        _cleanup_udev_device_unref_ struct udev_device *device;

        log_info("looking up device: %u:%u", major(devnum), minor(devnum));
        device = udev_device_new_from_devnum(udev, 'c', devnum);
        if (device == NULL)
                log_warning_errno(errno, "udev_device_new_from_devnum: %m");
        else
                print_device(device);
}

static void test_device_subsys_name(struct udev *udev, const char *subsys, const char *dev) {
        _cleanup_udev_device_unref_ struct udev_device *device;

        log_info("looking up device: '%s:%s'", subsys, dev);
        device = udev_device_new_from_subsystem_sysname(udev, subsys, dev);
        if (device == NULL)
                log_warning_errno(errno, "udev_device_new_from_subsystem_sysname: %m");
        else
                print_device(device);
}

static int test_enumerate_print_list(struct udev_enumerate *enumerate) {
        struct udev_list_entry *list_entry;
        int count = 0;

        udev_list_entry_foreach(list_entry, udev_enumerate_get_list_entry(enumerate)) {
                struct udev_device *device;

                device = udev_device_new_from_syspath(udev_enumerate_get_udev(enumerate),
                                                      udev_list_entry_get_name(list_entry));
                if (device != NULL) {
                        log_info("device: '%s' (%s)",
                                 udev_device_get_syspath(device),
                                 udev_device_get_subsystem(device));
                        udev_device_unref(device);
                        count++;
                }
        }
        log_info("found %i devices", count);
        return count;
}

static void test_monitor(struct udev *udev) {
        _cleanup_udev_monitor_unref_ struct udev_monitor *udev_monitor;
        _cleanup_close_ int fd_ep;
        int fd_udev;
        struct epoll_event ep_udev = {
                .events = EPOLLIN,
        }, ep_stdin = {
                .events = EPOLLIN,
                .data.fd = STDIN_FILENO,
        };

        fd_ep = epoll_create1(EPOLL_CLOEXEC);
        assert_se(fd_ep >= 0);

        udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
        assert_se(udev_monitor != NULL);

        fd_udev = udev_monitor_get_fd(udev_monitor);
        ep_udev.data.fd = fd_udev;

        assert_se(udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "block", NULL) >= 0);
        assert_se(udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "tty", NULL) >= 0);
        assert_se(udev_monitor_filter_add_match_subsystem_devtype(udev_monitor, "usb", "usb_device") >= 0);

        assert_se(udev_monitor_enable_receiving(udev_monitor) >= 0);

        assert_se(epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_udev, &ep_udev) >= 0);
        assert_se(epoll_ctl(fd_ep, EPOLL_CTL_ADD, STDIN_FILENO, &ep_stdin) >= 0);

        for (;;) {
                int fdcount;
                struct epoll_event ev[4];
                struct udev_device *device;
                int i;

                printf("waiting for events from udev, press ENTER to exit\n");
                fdcount = epoll_wait(fd_ep, ev, ELEMENTSOF(ev), -1);
                printf("epoll fd count: %i\n", fdcount);

                for (i = 0; i < fdcount; i++) {
                        if (ev[i].data.fd == fd_udev && ev[i].events & EPOLLIN) {
                                device = udev_monitor_receive_device(udev_monitor);
                                if (device == NULL) {
                                        printf("no device from socket\n");
                                        continue;
                                }
                                print_device(device);
                                udev_device_unref(device);
                        } else if (ev[i].data.fd == STDIN_FILENO && ev[i].events & EPOLLIN) {
                                printf("exiting loop\n");
                                return;
                        }
                }
        }
}

static void test_queue(struct udev *udev) {
        struct udev_queue *udev_queue;
        bool empty;

        udev_queue = udev_queue_new(udev);
        assert_se(udev_queue);

        empty = udev_queue_get_queue_is_empty(udev_queue);
        log_info("queue is %s", empty ? "empty" : "not empty");
        udev_queue_unref(udev_queue);
}

static int test_enumerate(struct udev *udev, const char *subsystem) {
        struct udev_enumerate *udev_enumerate;
        int r;

        log_info("enumerate '%s'", subsystem == NULL ? "<all>" : subsystem);
        udev_enumerate = udev_enumerate_new(udev);
        if (udev_enumerate == NULL)
                return -1;
        udev_enumerate_add_match_subsystem(udev_enumerate, subsystem);
        udev_enumerate_scan_devices(udev_enumerate);
        test_enumerate_print_list(udev_enumerate);
        udev_enumerate_unref(udev_enumerate);

        log_info("enumerate 'net' + duplicated scan + null + zero");
        udev_enumerate = udev_enumerate_new(udev);
        if (udev_enumerate == NULL)
                return -1;
        udev_enumerate_add_match_subsystem(udev_enumerate, "net");
        udev_enumerate_scan_devices(udev_enumerate);
        udev_enumerate_scan_devices(udev_enumerate);
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/zero");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/null");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/zero");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/null");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/zero");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/null");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/null");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/zero");
        udev_enumerate_add_syspath(udev_enumerate, "/sys/class/mem/zero");
        udev_enumerate_scan_devices(udev_enumerate);
        test_enumerate_print_list(udev_enumerate);
        udev_enumerate_unref(udev_enumerate);

        log_info("enumerate 'block'");
        udev_enumerate = udev_enumerate_new(udev);
        if (udev_enumerate == NULL)
                return -1;
        udev_enumerate_add_match_subsystem(udev_enumerate,"block");
        r = udev_enumerate_add_match_is_initialized(udev_enumerate);
        if (r < 0) {
                udev_enumerate_unref(udev_enumerate);
                return r;
        }
        udev_enumerate_scan_devices(udev_enumerate);
        test_enumerate_print_list(udev_enumerate);
        udev_enumerate_unref(udev_enumerate);

        log_info("enumerate 'not block'");
        udev_enumerate = udev_enumerate_new(udev);
        if (udev_enumerate == NULL)
                return -1;
        udev_enumerate_add_nomatch_subsystem(udev_enumerate, "block");
        udev_enumerate_scan_devices(udev_enumerate);
        test_enumerate_print_list(udev_enumerate);
        udev_enumerate_unref(udev_enumerate);

        log_info("enumerate 'pci, mem, vc'");
        udev_enumerate = udev_enumerate_new(udev);
        if (udev_enumerate == NULL)
                return -1;
        udev_enumerate_add_match_subsystem(udev_enumerate, "pci");
        udev_enumerate_add_match_subsystem(udev_enumerate, "mem");
        udev_enumerate_add_match_subsystem(udev_enumerate, "vc");
        udev_enumerate_scan_devices(udev_enumerate);
        test_enumerate_print_list(udev_enumerate);
        udev_enumerate_unref(udev_enumerate);

        log_info("enumerate 'subsystem'");
        udev_enumerate = udev_enumerate_new(udev);
        if (udev_enumerate == NULL)
                return -1;
        udev_enumerate_scan_subsystems(udev_enumerate);
        test_enumerate_print_list(udev_enumerate);
        udev_enumerate_unref(udev_enumerate);

        log_info("enumerate 'property IF_FS_*=filesystem'");
        udev_enumerate = udev_enumerate_new(udev);
        if (udev_enumerate == NULL)
                return -1;
        udev_enumerate_add_match_property(udev_enumerate, "ID_FS*", "filesystem");
        udev_enumerate_scan_devices(udev_enumerate);
        test_enumerate_print_list(udev_enumerate);
        udev_enumerate_unref(udev_enumerate);
        return 0;
}

static void test_hwdb(struct udev *udev, const char *modalias) {
        struct udev_hwdb *hwdb;
        struct udev_list_entry *entry;

        hwdb = udev_hwdb_new(udev);

        udev_list_entry_foreach(entry, udev_hwdb_get_properties_list_entry(hwdb, modalias, 0))
                log_info("'%s'='%s'", udev_list_entry_get_name(entry), udev_list_entry_get_value(entry));

        hwdb = udev_hwdb_unref(hwdb);
        assert_se(hwdb == NULL);
}

int main(int argc, char *argv[]) {
        _cleanup_udev_unref_ struct udev *udev = NULL;
        bool arg_monitor = false;
        static const struct option options[] = {
                { "syspath",   required_argument, NULL, 'p' },
                { "subsystem", required_argument, NULL, 's' },
                { "debug",     no_argument,       NULL, 'd' },
                { "help",      no_argument,       NULL, 'h' },
                { "version",   no_argument,       NULL, 'V' },
                { "monitor",   no_argument,       NULL, 'm' },
                {}
        };
        const char *syspath = "/devices/virtual/mem/null";
        const char *subsystem = NULL;
        int c;

        udev = udev_new();
        log_info("context: %p", udev);
        if (udev == NULL) {
                log_info("no context");
                return 1;
        }

        while ((c = getopt_long(argc, argv, "p:s:dhV", options, NULL)) >= 0)
                switch (c) {

                case 'p':
                        syspath = optarg;
                        break;

                case 's':
                        subsystem = optarg;
                        break;

                case 'd':
                        if (log_get_max_level() < LOG_INFO)
                                log_set_max_level(LOG_INFO);
                        break;

                case 'h':
                        printf("--debug --syspath= --subsystem= --help\n");
                        return EXIT_SUCCESS;

                case 'V':
                        printf("%s\n", PACKAGE_VERSION);
                        return EXIT_SUCCESS;

                case 'm':
                        arg_monitor = true;
                        break;

                case '?':
                        return EXIT_FAILURE;

                default:
                        assert_not_reached("Unhandled option code.");
                }


        /* add sys path if needed */
        if (!startswith(syspath, "/sys"))
                syspath = strjoina("/sys/", syspath);

        test_device(udev, syspath);
        test_device_devnum(udev);
        test_device_subsys_name(udev, "block", "sda");
        test_device_subsys_name(udev, "subsystem", "pci");
        test_device_subsys_name(udev, "drivers", "scsi:sd");
        test_device_subsys_name(udev, "module", "printk");

        test_device_parents(udev, syspath);

        test_enumerate(udev, subsystem);

        test_queue(udev);

        test_hwdb(udev, "usb:v0D50p0011*");

        if (arg_monitor)
                test_monitor(udev);

        return EXIT_SUCCESS;
}
