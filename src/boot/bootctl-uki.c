/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "bootctl-uki.h"
#include "uki-util.h"

int verb_kernel_identify(int argc, char *argv[], void *userdata) {
        KernelType t;
        int r;

        r = inspect_kernel(argv[1], &t, NULL, NULL, NULL);
        if (r < 0)
                return r;

        puts(kernel_type_to_string(t));
        return 0;
}

int verb_kernel_inspect(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *cmdline = NULL, *uname = NULL, *pname = NULL;
        KernelType t;
        int r;

        r = inspect_kernel(argv[1], &t, &cmdline, &uname, &pname);
        if (r < 0)
                return r;

        printf("Kernel Type: %s\n", kernel_type_to_string(t));
        if (cmdline)
                printf("    Cmdline: %s\n", cmdline);
        if (uname)
                printf("    Version: %s\n", uname);
        if (pname)
                printf("         OS: %s\n", pname);

        return 0;
}
