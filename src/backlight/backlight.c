/* SPDX-License-Identifier: LGPL-2.1+ */

#include "libudev.h"

#include "alloc-util.h"
#include "def.h"
#include "escape.h"
#include "fileio.h"
#include "mkdir.h"
#include "parse-util.h"
#include "proc-cmdline.h"
#include "string-util.h"
#include "udev-util.h"
#include "util.h"

static struct udev_device *find_pci_or_platform_parent(struct udev_device *device) {
        struct udev_device *parent;
        const char *subsystem, *sysname;

        assert(device);

        parent = udev_device_get_parent(device);
        if (!parent)
                return NULL;

        subsystem = udev_device_get_subsystem(parent);
        if (!subsystem)
                return NULL;

        sysname = udev_device_get_sysname(parent);
        if (!sysname)
                return NULL;

        if (streq(subsystem, "drm")) {
                const char *c;

                c = startswith(sysname, "card");
                if (!c)
                        return NULL;

                c += strspn(c, DIGITS);
                if (*c == '-') {
                        /* A connector DRM device, let's ignore all but LVDS and eDP! */

                        if (!startswith(c, "-LVDS-") &&
                            !startswith(c, "-Embedded DisplayPort-"))
                                return NULL;
                }

        } else if (streq(subsystem, "pci")) {
                const char *value;

                value = udev_device_get_sysattr_value(parent, "class");
                if (value) {
                        unsigned long class = 0;

                        if (safe_atolu(value, &class) < 0) {
                                log_warning("Cannot parse PCI class %s of device %s:%s.",
                                            value, subsystem, sysname);
                                return NULL;
                        }

                        /* Graphics card */
                        if (class == 0x30000)
                                return parent;
                }

        } else if (streq(subsystem, "platform"))
                return parent;

        return find_pci_or_platform_parent(parent);
}

static bool same_device(struct udev_device *a, struct udev_device *b) {
        assert(a);
        assert(b);

        if (!streq_ptr(udev_device_get_subsystem(a), udev_device_get_subsystem(b)))
                return false;

        if (!streq_ptr(udev_device_get_sysname(a), udev_device_get_sysname(b)))
                return false;

        return true;
}

static bool validate_device(struct udev_device *device) {
        _cleanup_(udev_enumerate_unrefp) struct udev_enumerate *enumerate = NULL;
        struct udev_list_entry *item = NULL, *first = NULL;
        struct udev_device *parent;
        const char *v, *subsystem;
        int r;

        assert(device);

        /* Verify whether we should actually care for a specific
         * backlight device. For backlight devices there might be
         * multiple ways to access the same control: "firmware"
         * (i.e. ACPI), "platform" (i.e. via the machine's EC) and
         * "raw" (via the graphics card). In general we should prefer
         * "firmware" (i.e. ACPI) or "platform" access over "raw"
         * access, in order not to confuse the BIOS/EC, and
         * compatibility with possible low-level hotkey handling of
         * screen brightness. The kernel will already make sure to
         * expose only one of "firmware" and "platform" for the same
         * device to userspace. However, we still need to make sure
         * that we use "raw" only if no "firmware" or "platform"
         * device for the same device exists. */

        subsystem = udev_device_get_subsystem(device);
        if (!streq_ptr(subsystem, "backlight"))
                return true;

        v = udev_device_get_sysattr_value(device, "type");
        if (!streq_ptr(v, "raw"))
                return true;

        parent = find_pci_or_platform_parent(device);
        if (!parent)
                return true;

        subsystem = udev_device_get_subsystem(parent);
        if (!subsystem)
                return true;

        enumerate = udev_enumerate_new(NULL);
        if (!enumerate)
                return true;

        r = udev_enumerate_add_match_subsystem(enumerate, "backlight");
        if (r < 0)
                return true;

        r = udev_enumerate_scan_devices(enumerate);
        if (r < 0)
                return true;

        first = udev_enumerate_get_list_entry(enumerate);
        UDEV_LIST_ENTRY_FOREACH(item, first) {
                _cleanup_(udev_device_unrefp) struct udev_device *other;
                struct udev_device *other_parent;
                const char *other_subsystem;

                other = udev_device_new_from_syspath(NULL, udev_list_entry_get_name(item));
                if (!other)
                        return true;

                if (same_device(device, other))
                        continue;

                v = udev_device_get_sysattr_value(other, "type");
                if (!STRPTR_IN_SET(v, "platform", "firmware"))
                        continue;

                /* OK, so there's another backlight device, and it's a
                 * platform or firmware device, so, let's see if we
                 * can verify it belongs to the same device as
                 * ours. */
                other_parent = find_pci_or_platform_parent(other);
                if (!other_parent)
                        continue;

                if (same_device(parent, other_parent)) {
                        /* Both have the same PCI parent, that means
                         * we are out. */
                        log_debug("Skipping backlight device %s, since device %s is on same PCI device and takes precedence.",
                                  udev_device_get_sysname(device),
                                  udev_device_get_sysname(other));
                        return false;
                }

                other_subsystem = udev_device_get_subsystem(other_parent);
                if (streq_ptr(other_subsystem, "platform") && streq_ptr(subsystem, "pci")) {
                        /* The other is connected to the platform bus
                         * and we are a PCI device, that also means we
                         * are out. */
                        log_debug("Skipping backlight device %s, since device %s is a platform device and takes precedence.",
                                  udev_device_get_sysname(device),
                                  udev_device_get_sysname(other));
                        return false;
                }
        }

        return true;
}

static unsigned get_max_brightness(struct udev_device *device) {
        int r;
        const char *max_brightness_str;
        unsigned max_brightness;

        max_brightness_str = udev_device_get_sysattr_value(device, "max_brightness");
        if (!max_brightness_str) {
                log_warning("Failed to read 'max_brightness' attribute.");
                return 0;
        }

        r = safe_atou(max_brightness_str, &max_brightness);
        if (r < 0) {
                log_warning_errno(r, "Failed to parse 'max_brightness' \"%s\": %m", max_brightness_str);
                return 0;
        }

        if (max_brightness <= 0) {
                log_warning("Maximum brightness is 0, ignoring device.");
                return 0;
        }

        return max_brightness;
}

/* Some systems turn the backlight all the way off at the lowest levels.
 * clamp_brightness clamps the saved brightness to at least 1 or 5% of
 * max_brightness in case of 'backlight' subsystem. This avoids preserving
 * an unreadably dim screen, which would otherwise force the user to
 * disable state restoration. */
static void clamp_brightness(struct udev_device *device, char **value, unsigned max_brightness) {
        unsigned brightness, new_brightness, min_brightness;
        const char *subsystem;
        int r;

        r = safe_atou(*value, &brightness);
        if (r < 0) {
                log_warning_errno(r, "Failed to parse brightness \"%s\": %m", *value);
                return;
        }

        subsystem = udev_device_get_subsystem(device);
        if (streq_ptr(subsystem, "backlight"))
                min_brightness = MAX(1U, max_brightness/20);
        else
                min_brightness = 0;

        new_brightness = CLAMP(brightness, min_brightness, max_brightness);
        if (new_brightness != brightness) {
                char *old_value = *value;

                r = asprintf(value, "%u", new_brightness);
                if (r < 0) {
                        log_oom();
                        return;
                }

                log_info("Saved brightness %s %s to %s.", old_value,
                         new_brightness > brightness ?
                         "too low; increasing" : "too high; decreasing",
                         *value);

                free(old_value);
        }
}

static bool shall_clamp(struct udev_device *d) {
        const char *s;
        int r;

        assert(d);

        s = udev_device_get_property_value(d, "ID_BACKLIGHT_CLAMP");
        if (!s)
                return true;

        r = parse_boolean(s);
        if (r < 0) {
                log_debug_errno(r, "Failed to parse ID_BACKLIGHT_CLAMP property, ignoring: %m");
                return true;
        }

        return r;
}

int main(int argc, char *argv[]) {
        _cleanup_(udev_device_unrefp) struct udev_device *device = NULL;
        _cleanup_free_ char *escaped_ss = NULL, *escaped_sysname = NULL, *escaped_path_id = NULL;
        const char *sysname, *path_id, *ss, *saved;
        unsigned max_brightness;
        int r;

        if (argc != 3) {
                log_error("This program requires two arguments.");
                return EXIT_FAILURE;
        }

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        r = mkdir_p("/var/lib/systemd/backlight", 0755);
        if (r < 0) {
                log_error_errno(r, "Failed to create backlight directory /var/lib/systemd/backlight: %m");
                return EXIT_FAILURE;
        }

        sysname = strchr(argv[2], ':');
        if (!sysname) {
                log_error("Requires a subsystem and sysname pair specifying a backlight device.");
                return EXIT_FAILURE;
        }

        ss = strndupa(argv[2], sysname - argv[2]);

        sysname++;

        if (!STR_IN_SET(ss, "backlight", "leds")) {
                log_error("Not a backlight or LED device: '%s:%s'", ss, sysname);
                return EXIT_FAILURE;
        }

        errno = 0;
        device = udev_device_new_from_subsystem_sysname(NULL, ss, sysname);
        if (!device) {
                if (errno > 0)
                        log_error_errno(errno, "Failed to get backlight or LED device '%s:%s': %m", ss, sysname);
                else
                        log_oom();

                return EXIT_FAILURE;
        }

        /* If max_brightness is 0, then there is no actual backlight
         * device. This happens on desktops with Asus mainboards
         * that load the eeepc-wmi module.
         */
        max_brightness = get_max_brightness(device);
        if (max_brightness == 0)
                return EXIT_SUCCESS;

        escaped_ss = cescape(ss);
        if (!escaped_ss) {
                log_oom();
                return EXIT_FAILURE;
        }

        escaped_sysname = cescape(sysname);
        if (!escaped_sysname) {
                log_oom();
                return EXIT_FAILURE;
        }

        path_id = udev_device_get_property_value(device, "ID_PATH");
        if (path_id) {
                escaped_path_id = cescape(path_id);
                if (!escaped_path_id) {
                        log_oom();
                        return EXIT_FAILURE;
                }

                saved = strjoina("/var/lib/systemd/backlight/", escaped_path_id, ":", escaped_ss, ":", escaped_sysname);
        } else
                saved = strjoina("/var/lib/systemd/backlight/", escaped_ss, ":", escaped_sysname);

        /* If there are multiple conflicting backlight devices, then
         * their probing at boot-time might happen in any order. This
         * means the validity checking of the device then is not
         * reliable, since it might not see other devices conflicting
         * with a specific backlight. To deal with this, we will
         * actively delete backlight state files at shutdown (where
         * device probing should be complete), so that the validity
         * check at boot time doesn't have to be reliable. */

        if (streq(argv[1], "load")) {
                _cleanup_free_ char *value = NULL;
                bool clamp;

                if (shall_restore_state() == 0)
                        return EXIT_SUCCESS;

                if (!validate_device(device))
                        return EXIT_SUCCESS;

                clamp = shall_clamp(device);

                r = read_one_line_file(saved, &value);
                if (r == -ENOENT) {
                        const char *curval;

                        /* Fallback to clamping current brightness or exit early if
                         * clamping is not supported/enabled. */
                        if (!clamp)
                                return EXIT_SUCCESS;

                        curval = udev_device_get_sysattr_value(device, "brightness");
                        if (!curval) {
                                log_warning("Failed to read 'brightness' attribute.");
                                return EXIT_FAILURE;
                        }

                        value = strdup(curval);
                        if (!value) {
                                log_oom();
                                return EXIT_FAILURE;
                        }
                } else if (r < 0) {
                        log_error_errno(r, "Failed to read %s: %m", saved);
                        return EXIT_FAILURE;
                }

                if (clamp)
                        clamp_brightness(device, &value, max_brightness);

                r = udev_device_set_sysattr_value(device, "brightness", value);
                if (r < 0) {
                        log_error_errno(r, "Failed to write system 'brightness' attribute: %m");
                        return EXIT_FAILURE;
                }

        } else if (streq(argv[1], "save")) {
                const char *value;

                if (!validate_device(device)) {
                        unlink(saved);
                        return EXIT_SUCCESS;
                }

                value = udev_device_get_sysattr_value(device, "brightness");
                if (!value) {
                        log_error("Failed to read system 'brightness' attribute");
                        return EXIT_FAILURE;
                }

                r = write_string_file(saved, value, WRITE_STRING_FILE_CREATE);
                if (r < 0) {
                        log_error_errno(r, "Failed to write %s: %m", saved);
                        return EXIT_FAILURE;
                }

        } else {
                log_error("Unknown verb %s.", argv[1]);
                return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
}
