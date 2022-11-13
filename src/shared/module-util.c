/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>

#include "module-util.h"
#include "strv.h"

int module_load_and_warn_with_blacklist(struct kmod_ctx *ctx, const char *module, char * const *blacklist, bool verbose) {
        const int probe_flags = KMOD_PROBE_APPLY_BLACKLIST;
        struct kmod_list *itr;
        _cleanup_(kmod_module_unref_listp) struct kmod_list *modlist = NULL;
        int r;

        /* verbose==true means we should log at non-debug level if we
         * fail to find or load the module. */

        log_debug("Loading module: %s", module);

        r = kmod_module_new_from_lookup(ctx, module, &modlist);
        if (r < 0)
                return log_full_errno(verbose ? LOG_ERR : LOG_DEBUG, r,
                                      "Failed to look up module alias '%s': %m", module);

        if (!modlist)
                return log_full_errno(verbose ? LOG_ERR : LOG_DEBUG,
                                      SYNTHETIC_ERRNO(ENOENT),
                                      "Failed to find module '%s'", module);

        kmod_list_foreach(itr, modlist) {
                _cleanup_(kmod_module_unrefp) struct kmod_module *mod = NULL;
                int state, err;

                mod = kmod_module_get_module(itr);
                state = kmod_module_get_initstate(mod);

                switch (state) {
                case KMOD_MODULE_BUILTIN:
                        log_full(verbose ? LOG_INFO : LOG_DEBUG,
                                 "Module '%s' is built in", kmod_module_get_name(mod));
                        break;

                case KMOD_MODULE_LIVE:
                        log_debug("Module '%s' is already loaded", kmod_module_get_name(mod));
                        break;

                default:
                        err = kmod_module_probe_insert_module(mod, probe_flags,
                                                              NULL, NULL, NULL, NULL);
                        if (err == 0)
                                log_full(verbose ? LOG_INFO : LOG_DEBUG,
                                         "Inserted module '%s'", kmod_module_get_name(mod));
                        else if (err == KMOD_PROBE_APPLY_BLACKLIST)
                                log_full(verbose ? LOG_INFO : LOG_DEBUG,
                                         "Module '%s' is deny-listed (by kmod)", kmod_module_get_name(mod));
                        else if (err == -EPERM) {
                                STRV_FOREACH(i, blacklist) {
                                        if (streq(*i, kmod_module_get_name(mod))) {
                                                log_full(verbose ? LOG_INFO : LOG_DEBUG,
                                                         "Module '%s' is deny-listed (by kernel)",
                                                         kmod_module_get_name(mod));
                                                r = -err; /* shouldn't be considered as a failure, so we negate the return value */
                                        }
                                }
                                if (r <= 0) {
                                        log_full_errno(!verbose ? LOG_DEBUG : LOG_ERR,
                                                       err,
                                                       "Failed to insert module '%s': %m",
                                                       kmod_module_get_name(mod));
                                        r = err;
                                }
                        } else {
                                assert(err < 0 && err != -EPERM);

                                log_full_errno(!verbose ? LOG_DEBUG :
                                               err == -ENODEV ? LOG_NOTICE :
                                               err == -ENOENT ? LOG_WARNING :
                                                                LOG_ERR,
                                               err,
                                               "Failed to insert module '%s': %m",
                                               kmod_module_get_name(mod));
                                if (!IN_SET(err, -ENODEV, -ENOENT))
                                        r = err;
                        }
                }
        }

        return r;
}
