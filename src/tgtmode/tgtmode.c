/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <sys/file.h>

#include "af-list.h"
#include "alloc-util.h"
#include "blockdev-util.h"
#include "build.h"
#include "daemon-util.h"
#include "device-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "local-addresses.h"
#include "loop-util.h"
#include "main-func.h"
#include "parse-argument.h"
#include "path-util.h"
#include "pretty-print.h"
#include "process-util.h"
#include "random-util.h"
#include "recurse-dir.h"
#include "socket-util.h"
#include "terminal-util.h"
#include "udev-util.h"

static char **arg_devices = NULL;
static char *arg_nqn = NULL;
static int arg_all = 0;

STATIC_DESTRUCTOR_REGISTER(arg_devices, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_nqn, freep);

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-notify", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...] [DEVICE...]\n"
               "\n%sExpose a block device or regular file as NVMe-TCP volume.%s\n\n"
               "  -h --help            Show this help\n"
               "     --version         Show package version\n"
               "     --nqn=STRING      Select NQN (NVMe Qualified Name)\n"
               "  -a --all             Expose all devices\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               ansi_highlight(),
               ansi_normal(),
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_READ_ONLY = 0x100,
                ARG_NAME,
                ARG_VERSION,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                { "name",      required_argument, NULL, ARG_NAME      },
                { "all",       no_argument,       NULL, 'a'           },
                {}
        };

        int r, c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0) {

                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_NAME:
                        if (!filename_is_valid(optarg))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Name invalid: %s", optarg);

                        if (free_and_strdup(&arg_nqn, optarg) < 0)
                                return log_oom();

                        break;

                case 'a':
                        arg_all++;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }
        }

        if (arg_all > 0) {
                if (argc > optind)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expects no further arguments if --all is specified.");
        } else {
                if (optind >= argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expecting device name or --all.");

                for (int i = optind; i < argc; i++)
                        if (!path_is_valid(argv[i]))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid path: %s", argv[i]);

                arg_devices = strv_copy(argv + optind);
        }

        if (!arg_nqn) {
                sd_id128_t id;

                r = sd_id128_get_machine_app_specific(SD_ID128_MAKE(b4,f9,4e,52,b8,e2,45,db,88,84,6e,2e,c3,f4,ef,18), &id);
                if (r < 0)
                        return log_error_errno(r, "Failed to get machine ID: %m");

                /* See NVM Express Base Specification 2.0c, 4.5 "NVMe Qualified Names" */
                if (asprintf(&arg_nqn, "nqn.2023-10.io.systemd:tgtmode." SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id)) < 0)
                        return log_oom();
        }

        return 1;
}

typedef struct NvmeSubsystem {
        char *name;
        struct stat device_stat;
        int device_fd;
        int nvme_subsystems_fd;
        int nvme_subsystem_fd;
        char *device;
} NvmeSubsystem;

static NvmeSubsystem* nvme_subsystem_free(NvmeSubsystem *s) {
        if (!s)
                return NULL;

        free(s->name);
        safe_close(s->nvme_subsystems_fd);
        safe_close(s->nvme_subsystem_fd);
        safe_close(s->device_fd);
        free(s->device);

        return mfree(s);
}

static int nvme_subsystem_unlink(NvmeSubsystem *s) {
        int r;

        assert(s);

        if (s->nvme_subsystem_fd >= 0) {
                _cleanup_close_ int namespaces_fd = -EBADF;

                namespaces_fd = openat(s->nvme_subsystem_fd, "namespaces", O_CLOEXEC|O_DIRECTORY|O_RDONLY);
                if (namespaces_fd < 0)
                        log_warning_errno(errno, "Failed to open 'namespaces' directory of subsystem '%s': %m", s->name);
                else {
                        _cleanup_free_ DirectoryEntries *de = NULL;

                        r = readdir_all(namespaces_fd, RECURSE_DIR_SORT|RECURSE_DIR_IGNORE_DOT, &de);
                        if (r < 0)
                                log_warning_errno(r, "Failed to read 'namspaces' dir of subsystem '%s', ignoring: %m", s->name);
                        else {
                                FOREACH_ARRAY(ee, de->entries, de->n_entries) {
                                        _cleanup_free_ char *enable_fn = NULL;
                                        const struct dirent *e = *ee;

                                        enable_fn = path_join(e->d_name, "enable");
                                        if (!enable_fn)
                                                return log_oom();

                                        r = write_string_file_at(namespaces_fd, enable_fn, "0", WRITE_STRING_FILE_DISABLE_BUFFER);
                                        if (r < 0)
                                                log_warning_errno(errno, "Failed to disable namespace '%s' of NVME subsystem '%s', ignoring: %m", e->d_name, s->name);

                                        if (unlinkat(namespaces_fd, e->d_name, AT_REMOVEDIR) < 0)
                                                log_warning_errno(errno, "Failed to remove namespace '%s' of NVME subsystem '%s', ignoring: %m", e->d_name, s->name);
                                }
                        }
                }

                s->nvme_subsystem_fd = safe_close(s->nvme_subsystem_fd);
        }

        if (s->nvme_subsystems_fd >= 0 && s->name) {
                if (unlinkat(s->nvme_subsystems_fd, s->name, AT_REMOVEDIR) < 0)
                        log_warning_errno(errno, "Failed to remove NVME subsystem '%s', ignoring: %m", s->name);

                s->nvme_subsystems_fd = safe_close(s->nvme_subsystems_fd);

                log_info("NVME subsystem '%s' removed.", s->name);
        }

        return 0;
}

static NvmeSubsystem *nvme_subsystem_destroy(NvmeSubsystem *s) {
        if (!s)
                return NULL;

        (void) nvme_subsystem_unlink(s);

        return nvme_subsystem_free(s);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(NvmeSubsystem*, nvme_subsystem_destroy);

static int nvme_subsystem_add(const char *node, int consumed_fd, NvmeSubsystem **ret) {
        _cleanup_close_ int fd = consumed_fd; /* always take possession of the fd */
        int r;

        assert(node);
        assert(ret);

        _cleanup_free_ char *fname = NULL;
        r = path_extract_filename(node, &fname);
        if (r < 0)
                return log_error_errno(r, "Failed to extract file name from path: %s", node);

        _cleanup_free_ char *j = NULL;
        j = strjoin(arg_nqn, ".", fname);
        if (!j)
                return log_oom();

        if (fd < 0) {
                fd = open(node, O_RDONLY|O_CLOEXEC|O_NONBLOCK);
                if (fd < 0)
                        return log_error_errno(fd, "Failed to open '%s': %m", node);
        }

        struct stat st;
        if (fstat(fd, &st) < 0)
                return log_error_errno(errno, "Failed to fstat '%s': %m", node);
        if (!S_ISBLK(st.st_mode)) {
                r = stat_verify_regular(&st);
                if (r < 0)
                        return log_error_errno(r, "Not a block device or regular file, refusing: %s", node);
        }

        /* Let's lock this device continously while we are operating on it */
        _cleanup_(sigkill_waitp) pid_t pid = 0;
        r = safe_fork("(sd-flock)", FORK_RESET_SIGNALS|FORK_WAIT, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to flock block device in child process: %m");
        if (r == 0) {
                alarm(10);

                if (flock(fd, LOCK_EX) < 0) {
                        log_error_errno(errno, "Unable to get an exclusive lock on the device: %m");
                        _exit(EXIT_FAILURE);
                }

                _exit(EXIT_SUCCESS);
        }

        _cleanup_close_ int subsystems_fd = -EBADF;
        subsystems_fd = open("/sys/kernel/config/nvmet/subsystems", O_DIRECTORY|O_CLOEXEC|O_RDONLY);
        if (subsystems_fd < 0)
                return log_error_errno(errno, "Failed to open /sys/kernel/config/nvmet/subsystems: %m");

        _cleanup_close_ int subsystem_fd = -EBADF;
        subsystem_fd = open_mkdir_at(subsystems_fd, j, O_EXCL|O_RDONLY|O_CLOEXEC, 0777);
        if (subsystem_fd < 0)
                return log_error_errno(subsystem_fd, "Failed to create NVME subsystem '%s': %m", j);

        r = write_string_file_at(subsystem_fd, "attr_allow_any_host", "1", WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to set 'attr_allow_any_host' flag: %m");

        _cleanup_close_ int namespace_fd = -EBADF;
        namespace_fd = open_mkdir_at(subsystem_fd, "namespaces/1", O_EXCL|O_RDONLY|O_CLOEXEC, 0777);
        if (namespace_fd < 0)
                return log_error_errno(namespace_fd, "Failed to create NVME namespace '1': %m");

        /* This is very similar to what FORMAT_PROC_FD_PATH() does, but goes by numeric pid number rather
         * than "self" symlink. This is because this string is visible to others via configs, and by
         * including the PID it's clear to who the stuff belongs. */
        char by_pid_and_fd[STRLEN("/proc//fd/") + DECIMAL_STR_MAX(pid_t) + DECIMAL_STR_MAX(int)];
        xsprintf(by_pid_and_fd, "/proc/" PID_FMT "/fd/%i", getpid_cached(), fd);

        r = write_string_file_at(namespace_fd, "device_path", by_pid_and_fd, WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to write 'device_path' attribute: %m");

        r = write_string_file_at(namespace_fd, "enable", "1", WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to write 'enable' attribute: %m");

        _cleanup_(nvme_subsystem_destroyp) NvmeSubsystem *subsys = NULL;

        subsys = new(NvmeSubsystem, 1);
        if (!subsys)
                return log_oom();

        *subsys = (NvmeSubsystem) {
                .name = TAKE_PTR(j),
                .device_fd = TAKE_FD(fd),
                .nvme_subsystems_fd = TAKE_FD(subsystems_fd),
                .nvme_subsystem_fd = TAKE_FD(subsystem_fd),
                .device_stat = st,
        };

        subsys->device = strdup(node);
        if (!subsys->device)
                return log_oom();

        *ret = TAKE_PTR(subsys);
        return 0;
}

typedef struct NvmePort {
        uint16_t portnr; /* used for both the IP and the NVME port numer */

        int nvme_port_fd;
        int nvme_ports_fd;

        int ip_family;
} NvmePort;

static NvmePort *nvme_port_free(NvmePort *p) {
        if (!p)
                return NULL;

        safe_close(p->nvme_port_fd);
        safe_close(p->nvme_ports_fd);

        return mfree(p);
}

static int nvme_port_unlink(NvmePort *p) {
        _cleanup_close_ int subsystems_dir_fd = -EBADF;
        int r, ret = 0;

        assert(p);

        if (p->nvme_port_fd >= 0) {
                subsystems_dir_fd = openat(p->nvme_port_fd, "subsystems", O_DIRECTORY|O_RDONLY|O_CLOEXEC);
                if (subsystems_dir_fd < 0)
                        log_warning_errno(errno, "Failed to open 'subsystems' dir of port %" PRIu16 ", ignoring: %m", p->portnr);
                else {
                        _cleanup_free_ DirectoryEntries *de = NULL;

                        r = readdir_all(subsystems_dir_fd, RECURSE_DIR_SORT|RECURSE_DIR_IGNORE_DOT, &de);
                        if (r < 0)
                                log_warning_errno(r, "Failed to read 'subsystems' dir of port %" PRIu16 ", ignoring: %m", p->portnr);
                        else {
                                FOREACH_ARRAY(ee, de->entries, de->n_entries) {
                                        const struct dirent *e = *ee;

                                        if (unlinkat(subsystems_dir_fd, e->d_name, 0) < 0 && errno != ENOENT)
                                                log_warning_errno(errno, "Failed to remove 'subsystems' symlink '%s' of port %" PRIu16 ", ignoring: %m", e->d_name, p->portnr);
                                }
                        }
                }

                subsystems_dir_fd = safe_close(subsystems_dir_fd);
                p->nvme_port_fd = safe_close(p->nvme_port_fd);
        }

        if (p->nvme_ports_fd >= 0) {
                _cleanup_free_ char *fn = NULL;
                if (asprintf(&fn, "%" PRIu16, p->portnr) < 0)
                        return log_oom();

                if (unlinkat(p->nvme_ports_fd, fn, AT_REMOVEDIR) < 0) {
                        if (errno == ENOENT)
                                ret = 0;
                        else
                                ret = log_warning_errno(errno, "Failed to remove port '%" PRIu16 ", ignoring: %m", p->portnr);
                } else
                        ret = 1;

                p->nvme_ports_fd = safe_close(p->nvme_ports_fd);
        }

        return ret;
}

static NvmePort *nvme_port_destroy(NvmePort *p) {
        if (!p)
                return NULL;

        (void) nvme_port_unlink(p);

        return nvme_port_free(p);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(NvmePort*, nvme_port_destroy);

static int nvme_port_add_portnr(
                int ports_fd,
                uint16_t portnr,
                int ip_family,
                int *ret_fd) {
        int r;

        assert(ports_fd >= 0);
        assert(IN_SET(ip_family, AF_INET, AF_INET6));
        assert(ret_fd);

        _cleanup_free_ char *fname = NULL;
        if (asprintf(&fname, "%" PRIu16, portnr) < 0)
                return log_oom();

        _cleanup_close_ int port_fd = -EBADF;
        port_fd = open_mkdir_at(ports_fd, fname, O_EXCL|O_RDONLY|O_CLOEXEC, 0777);
        if (port_fd < 0) {
                if (port_fd != -EEXIST)
                        return log_error_errno(port_fd, "Failed to create port %" PRIu16 ": %m", portnr);

                *ret_fd = -EBADF;
                return 0;
        }

        r = write_string_file_at(port_fd, "addr_adrfam", af_to_ipv4_ipv6(ip_family), WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to set address family on NVME port %" PRIu16 ": %m", portnr);

        r = write_string_file_at(port_fd, "addr_trtype", "tcp", WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to set transport type on NVME port %" PRIu16 ": %m", portnr);

        _cleanup_free_ char *sportnr = NULL;
        if (asprintf(&sportnr, "%" PRIu16, portnr) < 0)
                return log_oom();

        r = write_string_file_at(port_fd, "addr_trsvcid", sportnr, WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to set IP port on NVME port %" PRIu16 ": %m", portnr);

        r = write_string_file_at(port_fd, "addr_traddr", ip_family == AF_INET6 ? "::" : "0.0.0.0", WRITE_STRING_FILE_DISABLE_BUFFER);
        if (r < 0)
                return log_error_errno(r, "Failed to set IP port on NVME port %" PRIu16 ": %m", portnr);

        *ret_fd = TAKE_FD(port_fd);
        return 1;
}

static uint16_t calculate_start_port(const char *name, int ip_family) {
        struct siphash state;
        uint16_t nr;

        assert(name);
        assert(IN_SET(ip_family, AF_INET, AF_INET6));

        siphash24_init(&state, SD_ID128_MAKE(d1,0b,67,b5,e2,b7,4a,91,8d,6b,27,b6,35,c1,9f,d9).bytes);
        siphash24_compress_string(name, &state);
        siphash24_compress(&ip_family, sizeof(ip_family), &state);

        nr = 1024U + siphash24_finalize(&state) % (0xFFFFU - 1024U);
        SET_FLAG(nr, 1, ip_family == AF_INET6); /* Lowest bit reflects family */

        return nr;
}

static uint16_t calculate_next_port(int ip_family) {
        uint16_t nr;

        assert(IN_SET(ip_family, AF_INET, AF_INET6));

        nr = 1024U + random_u64_range(0xFFFFU - 1024U);
        SET_FLAG(nr, 1, ip_family == AF_INET6); /* Lowest bit reflects family */

        return nr;
}

static int nvme_port_add(const char *name, int ip_family, NvmePort **ret) {
        int r;

        assert(IN_SET(ip_family, AF_INET, AF_INET6));
        assert(ret);

        _cleanup_close_ int ports_fd = -EBADF;
        ports_fd = open("/sys/kernel/config/nvmet/ports", O_DIRECTORY|O_RDONLY|O_CLOEXEC);
        if (ports_fd < 0)
                return log_error_errno(errno, "Failed to open /sys/kernel/config/nvmet/ports: %m");

        _cleanup_close_ int port_fd = -EBADF;
        uint16_t portnr = calculate_start_port(name, ip_family);
        unsigned attempt = 0;
        for (;;) {
                r = nvme_port_add_portnr(ports_fd, portnr, ip_family, &port_fd);
                if (r < 0)
                        return r;
                if (r > 0)
                        break;

                if (attempt > 16)
                        return log_error_errno(SYNTHETIC_ERRNO(EBUSY), "Can't find free NVME port after %u attempts.", attempt);

                log_debug_errno(port_fd, "NVME port %" PRIu16 " exists already, randomizing port.", portnr);

                portnr = calculate_next_port(ip_family);
                attempt++;
        }

        _cleanup_(nvme_port_destroyp) NvmePort *p = new(NvmePort, 1);
        if (!p)
                return log_oom();

        *p = (NvmePort) {
                .portnr = portnr,
                .nvme_ports_fd = TAKE_FD(ports_fd),
                .nvme_port_fd = TAKE_FD(port_fd),
                .ip_family = ip_family,
        };

        *ret = TAKE_PTR(p);
        return 0;
}

static int nvme_port_link_subsystem(NvmePort *port, NvmeSubsystem *subsys) {
        assert(port);
        assert(subsys);

        _cleanup_free_ char *target = NULL, *linkname = NULL;
        target = path_join("/sys/kernel/config/nvmet/subsystems", subsys->name);
        if (!target)
                return log_oom();

        linkname = path_join("subsystems", subsys->name);
        if (!linkname)
                return log_oom();

        if (symlinkat(target, port->nvme_port_fd, linkname) < 0)
                return log_error_errno(errno, "Failed to link subsystem '%s' to port %" PRIu16 ": %m", subsys->name, port->portnr);

        return 0;
}

static int nvme_port_unlink_subsystem(NvmePort *port, NvmeSubsystem *subsys) {
        assert(port);
        assert(subsys);

        _cleanup_free_ char *linkname = NULL;
        linkname = path_join("subsystems", subsys->name);
        if (!linkname)
                return log_oom();

        if (unlinkat(port->nvme_port_fd, linkname, 0) < 0)
                return log_error_errno(errno, "Failed to unlink subsystem '%s' to port %" PRIu16 ": %m", subsys->name, port->portnr);

        return 0;
}

static int nvme_subsystem_report(NvmeSubsystem *subsystem, NvmePort *ipv4, NvmePort *ipv6) {
        assert(subsystem);

        _cleanup_free_ struct local_address *addresses = NULL;
        int n_addresses;
        n_addresses = local_addresses(NULL, 0, AF_UNSPEC, &addresses);
        if (n_addresses < 0)
                return log_error_errno(n_addresses, "Failed to determine local IP addresses: %m");

        log_notice("NVMe-TCP: %s %s (%s)", special_glyph(SPECIAL_GLYPH_ARROW_RIGHT), subsystem->name, subsystem->device);

        FOREACH_ARRAY(a, addresses, n_addresses) {
                NvmePort *port = a->family == AF_INET ? ipv4 : ipv6;

                if (!port)
                        continue;

                log_info("          %s Try for specific device: nvme connect -t tcp -n '%s' -a %s -s %" PRIu16,
                         special_glyph(a >= addresses + (n_addresses - 1) ? SPECIAL_GLYPH_TREE_RIGHT : SPECIAL_GLYPH_TREE_BRANCH),
                         subsystem->name,
                         IN_ADDR_TO_STRING(a->family, &a->address),
                         port->portnr);
        }

        return 0;
}

static int nvme_port_report(NvmePort *port) {
        assert(port);

        if (!port)
                return 0;

        _cleanup_free_ struct local_address *addresses = NULL;
        int n_addresses;
        n_addresses = local_addresses(NULL, 0, AF_UNSPEC, &addresses);
        if (n_addresses < 0)
                return log_error_errno(n_addresses, "Failed to determine local IP addresses: %m");

        _cleanup_free_ char *ps = NULL;

        log_notice("NVMe-TCP: %s Listening on %s (port %" PRIu16 ")", special_glyph(SPECIAL_GLYPH_ARROW_RIGHT), af_to_ipv4_ipv6(port->ip_family), port->portnr);

        FOREACH_ARRAY(a, addresses, n_addresses) {
                log_info("          %s Try for all devices: nvme connect-all -t tcp -a %s -s %" PRIu16,
                         special_glyph(a >= addresses + (n_addresses - 1) ? SPECIAL_GLYPH_TREE_RIGHT : SPECIAL_GLYPH_TREE_BRANCH),
                         IN_ADDR_TO_STRING(a->family, &a->address),
                         port->portnr);
        }

        return 0;
}

typedef struct Context {
        Hashmap *subsystems;
        NvmePort *ipv4_port, *ipv6_port;

        bool display_refresh_scheduled;
} Context;

static void device_hash_func(const struct stat *q, struct siphash *state) {
        assert(q);

        if (S_ISBLK(q->st_mode) || S_ISCHR(q->st_mode)) {
                mode_t m = q->st_mode & S_IFMT;
                siphash24_compress(&m, sizeof(m), state);
                siphash24_compress(&q->st_rdev, sizeof(q->st_rdev), state);
                return;
        }

        return inode_hash_func(q, state);
}

static int device_compare_func(const struct stat *a, const struct stat *b) {
        int r;

        assert(a);
        assert(b);

        r = CMP(a->st_mode & S_IFMT, b->st_mode & S_IFMT);
        if (r != 0)
                return r;

        if (S_ISBLK(a->st_mode) || S_ISCHR(a->st_mode)) {
                r = CMP(major(a->st_rdev), major(b->st_rdev));
                if (r != 0)
                        return r;

                r = CMP(minor(a->st_rdev), minor(b->st_rdev));
                if (r != 0)
                        return r;

                return 0;
        }

        return inode_compare_func(a, b);
}

DEFINE_PRIVATE_HASH_OPS_WITH_VALUE_DESTRUCTOR(
                nvme_subsystem_hash_ops,
                struct stat,
                device_hash_func,
                device_compare_func,
                NvmeSubsystem,
                nvme_subsystem_destroy);

static void context_done(Context *c) {
        assert(c);

        c->ipv4_port = nvme_port_destroy(c->ipv4_port);
        c->ipv6_port = nvme_port_destroy(c->ipv6_port);

        c->subsystems = hashmap_free(c->subsystems);
}

static void device_track_back(sd_device *d, sd_device **ret) {
        int r;

        assert(d);
        assert(ret);

        const char *devname;
        (void) sd_device_get_devname(d, &devname);

        _cleanup_(sd_device_unrefp) sd_device *d_originating = NULL;
        r = block_device_get_originating(d, &d_originating);
        if (r < 0)
                log_debug_errno(r, "Failed to get originating device for '%s', ignoring: %m", strna(devname));

        sd_device *d_whole = NULL;
        r = block_device_get_whole_disk(d_originating ?: d, &d_whole); /* does not ref returned device */
        if (r < 0)
                log_debug_errno(r, "Failed to get whole device for '%s', ignoring: %m", strna(devname));

        *ret = d_whole ? sd_device_ref(d_whole) : d_originating ? TAKE_PTR(d_originating) : sd_device_ref(d);
}

static int device_is_same(sd_device *a, sd_device *b) {
        dev_t devnum_a, devnum_b;
        int r;

        assert(a);
        assert(b);

        r = sd_device_get_devnum(a, &devnum_a);
        if (r < 0)
                return r;

        r = sd_device_get_devnum(b, &devnum_b);
        if (r < 0)
                return r;

        return devnum_a == devnum_b;
}

static bool device_is_allowed(sd_device *d) {
        int r;

        assert(d);

        if (arg_all >= 2) /* If --all is specified twice we allow even the root fs to shared */
                return true;

        const char *devname;
        r = sd_device_get_devname(d, &devname);
        if (r < 0)
                return log_error_errno(r, "Failed to get device name: %m");

        dev_t root_devnum;
        r = get_block_device("/", &root_devnum);
        if (r < 0) {
                log_warning_errno(r, "Failed to get backing device of the root file system: %m");
                return false; /* Better safe */
        }
        if (root_devnum == 0) /* Not backed by a block device? */
                return true;

        _cleanup_(sd_device_unrefp) sd_device *root_device = NULL;
         r = sd_device_new_from_devnum(&root_device, 'b', root_devnum);
        if (r < 0) {
                log_warning_errno(r, "Failed to get root block device, assuming device '%s' is same as root device: %m", devname);
                return false;
        }

        _cleanup_(sd_device_unrefp) sd_device *whole_root_device = NULL;
        device_track_back(root_device, &whole_root_device);

        _cleanup_(sd_device_unrefp) sd_device *whole_d = NULL;
        device_track_back(d, &whole_d);

        r = device_is_same(whole_root_device, whole_d);
        if (r < 0) {
                log_warning_errno(r, "Failed to determine if root device and device '%s' are the same, assuming they are: %m", devname);
                return false; /* Better safe */
        }

        return !r;
}

static int device_added(Context *c, sd_device *device) {
        _cleanup_close_ int fd = -EBADF;
        int r;

        assert(c);
        assert(device);

        const char *sysname;
        r = sd_device_get_sysname(device, &sysname);
        if (r < 0)
                return log_error_errno(r, "Failed to get device name: %m");
        if (STARTSWITH_SET(sysname, "loop", "zram")) /* Ignore some devices */
                return 0;

        const char *devname;
        r = sd_device_get_devname(device, &devname);
        if (r < 0)
                return log_error_errno(r, "Failed to get device node path: %m");

        struct stat lookup_key = {
                .st_mode = S_IFBLK,
        };

        r = sd_device_get_devnum(device, &lookup_key.st_rdev);
        if (r < 0)
                return log_error_errno(r, "Failed to get major/minor from device: %m");

        if (hashmap_contains(c->subsystems, &lookup_key)) {
                log_debug("Device '%s' already seen.", devname);
                return 0;
        }

        if (!device_is_allowed(device)) {
                log_device_debug(device, "Not exposing device '%s', as it is backed by root disk.", devname);
                return 0;
        }

        fd = sd_device_open(device, O_RDONLY|O_CLOEXEC|O_NONBLOCK);
        if (fd < 0) {
                log_warning_errno(fd, "Failed to open newly acquired device '%s', ignoring device: %m", devname);
                return 0;
        }

        _cleanup_(nvme_subsystem_destroyp) NvmeSubsystem *s = NULL;
        r = nvme_subsystem_add(devname, TAKE_FD(fd), &s);
        if (r < 0)
                return r;

        if (c->ipv4_port) {
                r = nvme_port_link_subsystem(c->ipv4_port, s);
                if (r < 0)
                        return r;
        }

        if (c->ipv6_port) {
                r = nvme_port_link_subsystem(c->ipv6_port, s);
                if (r < 0)
                        return r;
        }

        r = hashmap_ensure_put(&c->subsystems, &nvme_subsystem_hash_ops, &s->device_stat, s);
        if (r < 0)
                return log_error_errno(r, "Failed to add subsystem to hash table: %m");

        (void) nvme_subsystem_report(s, c->ipv4_port, c->ipv6_port);

        TAKE_PTR(s);
        return 1;
}

static int device_removed(Context *c, sd_device *device) {
        int r;

        assert(device);

        struct stat lookup_key = {
                .st_mode = S_IFBLK,
        };

        r = sd_device_get_devnum(device, &lookup_key.st_rdev);
        if (r < 0)
                return log_error_errno(r, "Failed to get major/minor from device: %m");

        NvmeSubsystem *s = hashmap_remove(c->subsystems, &lookup_key);
        if (!s)
                return 0;

        if (c->ipv4_port)
                (void) nvme_port_unlink_subsystem(c->ipv4_port, s);
        if (c->ipv6_port)
                (void) nvme_port_unlink_subsystem(c->ipv6_port, s);

        s = nvme_subsystem_destroy(s);
        return 1;
}

static int device_monitor_handler(sd_device_monitor *monitor, sd_device *device, void *userdata) {
        Context *c = ASSERT_PTR(userdata);

        if (device_for_action(device, SD_DEVICE_REMOVE))
                device_removed(c, device);
        else
                device_added(c, device);

        return 0;
}

static int on_display_refresh(sd_event_source *s, uint64_t usec, void *userdata) {
        Context *c = ASSERT_PTR(userdata);

        assert(s);

        c->display_refresh_scheduled = false;

        if (isatty(STDERR_FILENO) > 0)
                fputs(ANSI_HOME_CLEAR, stderr);

        (void) nvme_port_report(c->ipv6_port);
        (void) nvme_port_report(c->ipv6_port);

        NvmeSubsystem *i;
        HASHMAP_FOREACH(i, c->subsystems)
                (void) nvme_subsystem_report(i, c->ipv4_port, c->ipv6_port);

        return 0;
}

static int on_address_change(sd_netlink *rtnl, sd_netlink_message *mm, void *userdata) {
        Context *c = ASSERT_PTR(userdata);
        int r, family;

        assert(rtnl);
        assert(mm);

        r = sd_rtnl_message_addr_get_family(mm, &family);
        if (r < 0) {
                log_warning_errno(r, "Failed to get address family from netlink address message, ignoring: %m");
                return 0;
        }

        if (!c->display_refresh_scheduled) {
                r = sd_event_add_time_relative(
                                sd_netlink_get_event(rtnl),
                                /* ret_slot= */ NULL,
                                CLOCK_MONOTONIC,
                                750 * USEC_PER_MSEC,
                                0,
                                on_display_refresh,
                                c);
                if (r < 0)
                        log_warning_errno(r, "Failed to schedule display refresh, ignoring: %m");
                else
                        c->display_refresh_scheduled = true;
        }

        return 0;
}

static int run(int argc, char* argv[]) {
         _cleanup_(sd_device_monitor_unrefp) sd_device_monitor *monitor = NULL;
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(context_done) Context context = {};
        int r;

        log_show_color(true);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = sd_event_new(&event);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate event loop: %m");

        r = sd_event_set_signal_exit(event, true);
        if (r < 0)
                return log_error_errno(r, "Failed to install exit signal handlers: %m");

        STRV_FOREACH(i, arg_devices) {
                _cleanup_(nvme_subsystem_destroyp) NvmeSubsystem *subsys = NULL;

                r = nvme_subsystem_add(*i, -EBADF, &subsys);
                if (r < 0)
                        return r;

                r = hashmap_ensure_put(&context.subsystems, &nvme_subsystem_hash_ops, &subsys->device_stat, subsys);
                if (r == -EEXIST) {
                        log_warning_errno(r, "Duplicate device '%s' specified, skipping: %m", *i);
                        continue;
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to add subsystem to hash table: %m");
        }

        r = nvme_port_add(arg_nqn, AF_INET, &context.ipv4_port);
        if (r < 0)
                return r;

        nvme_port_report(context.ipv4_port);

        if (socket_ipv6_is_enabled()) {
                r = nvme_port_add(arg_nqn, AF_INET6, &context.ipv6_port);
                if (r < 0)
                        return r;

                nvme_port_report(context.ipv6_port);
        }

        NvmeSubsystem *i;
        HASHMAP_FOREACH(i, context.subsystems) {
                if (context.ipv4_port) {
                        r = nvme_port_link_subsystem(context.ipv4_port, i);
                        if (r < 0)
                                return r;
                }

                if (context.ipv6_port) {
                        r = nvme_port_link_subsystem(context.ipv6_port, i);
                        if (r < 0)
                                return r;
                }

                (void) nvme_subsystem_report(i, context.ipv4_port, context.ipv6_port);
        }

        if (arg_all > 0) {
                r = sd_device_monitor_new(&monitor);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate device monitor: %m");

                r = sd_device_monitor_filter_add_match_subsystem_devtype(monitor, "block", "disk");
                if (r < 0)
                        return log_error_errno(r, "Failed to configure device monitor match: %m");

                r = sd_device_monitor_attach_event(monitor, event);
                if (r < 0)
                        return log_error_errno(r, "Failed to attach device monitor to event loop: %m");

                r = sd_device_monitor_start(monitor, device_monitor_handler, &context);
                if (r < 0)
                        return log_error_errno(r, "Failed to start device monitor: %m");

                _cleanup_(sd_device_enumerator_unrefp) sd_device_enumerator *enumerator = NULL;
                r = sd_device_enumerator_new(&enumerator);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate enumerator: %m");

                r = sd_device_enumerator_add_match_subsystem(enumerator, "block", /* match= */ true);
                if (r < 0)
                        return log_error_errno(r, "Failed to match block devices: %m");

                r = sd_device_enumerator_add_match_property(enumerator, "DEVTYPE", "disk");
                if (r < 0)
                        return log_error_errno(r, "Failed to match whole block devices: %m");

                r = sd_device_enumerator_add_nomatch_sysname(enumerator, "loop*");
                if (r < 0)
                        return log_error_errno(r, "Failed to exclude loop devices: %m");

                FOREACH_DEVICE(enumerator, device)
                        device_added(&context, device);
        }

        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        r = sd_netlink_attach_event(rtnl, event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return log_error_errno(r, "Failed to attach netlink socket to event loop: %m");

        r = sd_netlink_add_match(rtnl, /* ret_slot= */ NULL, RTM_NEWADDR, on_address_change, /* destroy_callback= */ NULL, &context, "tgtmode-newaddr");
        if (r < 0)
                return log_error_errno(r, "Failed to subscribe to RTM_NEWADDR events: %m");

        r = sd_netlink_add_match(rtnl, /* ret_slot= */ NULL, RTM_DELADDR, on_address_change, /* destroy_callback= */ NULL, &context, "tgtmode-deladdr");
        if (r < 0)
                return log_error_errno(r, "Failed to subscribe to RTM_DELADDR events: %m");

        if (isatty(0) > 0)
                log_info("Hit Ctrl-C to exit target mode.");

        _unused_ _cleanup_(notify_on_cleanup) const char *notify_message =
                notify_start("READY=1\n"
                             "STATUS=Exposing disks in target mode...",
                             NOTIFY_STOPPING);

        r = sd_event_loop(event);
        if (r < 0)
                return log_error_errno(r, "Failed to run event loop: %m");

        log_info("Exiting target mode.");
        return r;
}

DEFINE_MAIN_FUNCTION_WITH_POSITIVE_FAILURE(run);
