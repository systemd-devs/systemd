/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "generator-setup.h"
#include "macro.h"
#include "mkdir-label.h"
#include "rm-rf.h"

int lookup_paths_mkdir_generator(LookupPaths *p) {
        int r;

        assert(p);

        if (!p->generator || !p->generator_early || !p->generator_late)
                return -EINVAL;

        r = mkdir_p_label(p->generator, 0755);
        RET_GATHER(r, mkdir_p_label(p->generator_early, 0755));
        RET_GATHER(r, mkdir_p_label(p->generator_late, 0755));

        return r;
}

void lookup_paths_trim_generator(LookupPaths *p) {
        assert(p);

        /* Trim empty dirs */

        if (p->generator)
                (void) rmdir(p->generator);
        if (p->generator_early)
                (void) rmdir(p->generator_early);
        if (p->generator_late)
                (void) rmdir(p->generator_late);
}

void lookup_paths_flush_generator(LookupPaths *p, bool test) {
        assert(p);

        /* Flush the generated unit files in full */

        if (p->temporary_dir)
                (void) rm_rf(p->temporary_dir, REMOVE_ROOT|REMOVE_PHYSICAL);

        if (test)
                return; /* Do not remove generated units when runnint in a test mode. */

        if (p->generator)
                (void) rm_rf(p->generator, REMOVE_ROOT|REMOVE_PHYSICAL);
        if (p->generator_early)
                (void) rm_rf(p->generator_early, REMOVE_ROOT|REMOVE_PHYSICAL);
        if (p->generator_late)
                (void) rm_rf(p->generator_late, REMOVE_ROOT|REMOVE_PHYSICAL);
}
