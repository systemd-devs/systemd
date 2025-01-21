/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/auxv.h>

#include "escape.h"
#include "hostname-util.h"
#include "osc-context.h"
#include "pidfd-util.h"
#include "process-util.h"
#include "string-util.h"
#include "terminal-util.h"
#include "user-util.h"

/* This currently generates open sequences for OSC 3008 types "boot", "container", "vm", "elevate",
 * "chpriv", "subcontext". */

/* TODO:
 *
 *  → "service" (from the service manager)
 *  → "session" (from pam_systemd?)
 *  → "shell", "command" (from a bash profile drop-in?)
 *
 * Not generated by systemd: "remote" (would have to be generated from the SSH client), "app".
 */

static int strextend_escaped(char **s, const char *prefix, const char *value) {
        assert(s);
        assert(value);

        if (!strextend(s, prefix))
                return -ENOMEM;

        _cleanup_free_ char *e = xescape(value, ";\\");
        if (!e)
                return -ENOMEM;

        if (!strextend(s, e))
                return -ENOMEM;

        return 0;
}

static int osc_append_identity(char **s) {
        int r;

        assert(s);

        _cleanup_free_ char *u = getusername_malloc();
        if (u) {
                r = strextend_escaped(s, ";user=", u);
                if (r < 0)
                        return r;
        }

        _cleanup_free_ char *h = gethostname_malloc();
        if (h) {
                r = strextend_escaped(s, ";hostname=", h);
                if (r < 0)
                        return r;
        }

        sd_id128_t id;
        if (sd_id128_get_machine(&id) >= 0) {
                r = strextendf(s, ";machineid=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id));
                if (r < 0)
                        return r;
        }

        if (sd_id128_get_boot(&id) >= 0) {
                r = strextendf(s, ";bootid=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id));
                if (r < 0)
                        return r;
        }

        r = strextendf(s, ";pid=" PID_FMT, getpid_cached());
        if (r < 0)
                return r;

        uint64_t pidfdid;
        r = pidfd_get_inode_id_self_cached(&pidfdid);
        if (r >= 0) {
                r = strextendf(s, ";pidfdid=%" PRIu64, pidfdid);
                if (r < 0)
                        return r;
        }

        r = strextend_escaped(s, ";comm=", program_invocation_short_name);
        if (r < 0)
                return r;

        return 0;
}

static void osc_context_default_id(sd_id128_t *ret_id) {

        /* Usually we only want one context ID per tool. Since we don't want to store the ID let's just hash
         * one from process credentials */

        struct {
                uint64_t pidfdid;
                uint8_t auxval[16];
                pid_t pid;
        } data = {
                .pid = getpid_cached(),
        };

        assert(ret_id);

        memcpy(data.auxval, ULONG_TO_PTR(getauxval(AT_RANDOM)), sizeof(data.auxval));
        (void) pidfd_get_inode_id_self_cached(&data.pidfdid);

        ret_id->qwords[0] = siphash24(&data, sizeof(data), SD_ID128_MAKE(3f,8c,ee,e1,fd,35,41,ec,b8,b1,90,d4,59,e2,ae,5b).bytes);
        ret_id->qwords[1] = siphash24(&data, sizeof(data), SD_ID128_MAKE(c6,41,ec,1b,d8,85,48,c0,8e,11,d7,e1,e1,fa,9e,03).bytes);
}

static int osc_context_intro(char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        assert(ret_seq);

        /* If the user passed us a buffer for the context ID generate a randomized one, since we have a place
         * to store it. The user should pass the ID back to osc_context_close() later on. if the user did not
         * pass us a buffer, we'll use a session ID hashed from process properties that remain stable as long
         * our process exists. It hence also remains stable across reexec and similar. */
        sd_id128_t id;
        if (ret_context_id) {
                r = sd_id128_randomize(&id);
                if (r < 0)
                        return r;
        } else
                osc_context_default_id(&id);

        _cleanup_free_ char *seq = NULL;
        if (asprintf(&seq, ANSI_OSC "3008;start=" SD_ID128_FORMAT_STR, SD_ID128_FORMAT_VAL(id)) < 0)
                return -ENOMEM;

        r = osc_append_identity(&seq);
        if (r < 0)
                return r;

        if (ret_context_id)
                *ret_context_id = id;

        *ret_seq = TAKE_PTR(seq);
        return 0;
}

static int osc_context_outro(char *_seq, sd_id128_t id, char **ret_seq, sd_id128_t *ret_context_id) {
        _cleanup_free_ char *seq = TAKE_PTR(_seq); /* We take possession of the string no matter what */

        if (ret_seq)
                *ret_seq = TAKE_PTR(seq);
        else {
                fputs(seq, stdout);
                fflush(stdout);
        }

        if (ret_context_id)
                *ret_context_id = id;

        return 0;
}

int osc_context_open_boot(char **ret_seq) {
        int r;

        _cleanup_free_ char *seq = NULL;
        r = osc_context_intro(&seq, /* ret_context_id= */ NULL);
        if (r < 0)
                return r;

        if (!strextend(&seq, ";type=" "boot" ANSI_ST))
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), /* context_id= */ SD_ID128_NULL, ret_seq, /* ret_context_id= */ NULL);
}

int osc_context_open_container(const char *name, char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        _cleanup_free_ char *seq = NULL;
        sd_id128_t id;
        r = osc_context_intro(&seq, ret_context_id ? &id : NULL);
        if (r < 0)
                return r;

        if (name) {
                r = strextend_escaped(&seq, ";container=", name);
                if (r < 0)
                        return r;
        }

        if (!strextend(&seq, ";type=container" ANSI_ST))
                return -ENOMEM;

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, ret_context_id);
}

int osc_context_open_vm(const char *name, char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        assert(name);

        _cleanup_free_ char *seq = NULL;
        sd_id128_t id;
        r = osc_context_intro(&seq, ret_context_id ? &id : NULL);
        if (r < 0)
                return r;

        r = strextend_escaped(&seq, ";vm=", name);
        if (r < 0)
                return r;

        if (!strextend(&seq, ";type=vm" ANSI_ST))
                return r;

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, ret_context_id);
}

int osc_context_open_chpriv(const char *target_user, char **ret_seq, sd_id128_t *ret_context_id) {
        int r;

        assert(target_user);

        _cleanup_free_ char *seq = NULL;
        sd_id128_t id;
        r = osc_context_intro(&seq, ret_context_id ?: &id);
        if (r < 0)
                return r;

        if (STR_IN_SET(target_user, "root", "0")) {
                if (!strextend(&seq, ";type=elevate" ANSI_ST))
                        return -ENOMEM;
        } else if (is_this_me(target_user) > 0) {
                if (!strextend(&seq, ";type=subcontext" ANSI_ST))
                        return -ENOMEM;
        } else {
                r = strextend_escaped(&seq, ";targetuser=", target_user);
                if (r < 0)
                        return r;

                if (!strextend(&seq, ";type=chpriv" ANSI_ST))
                        return -ENOMEM;
        }

        return osc_context_outro(TAKE_PTR(seq), id, ret_seq, ret_context_id);
}

int osc_context_close(sd_id128_t id, char **ret_seq) {

        if (sd_id128_is_null(id)) /* nil uuid: no session opened */
                return 0;

        if (sd_id128_is_allf(id)) /* max uuid: default session opened */
                osc_context_default_id(&id);

        _cleanup_free_ char *seq = NULL;
        if (asprintf(&seq, ANSI_OSC "3008;end=" SD_ID128_FORMAT_STR ANSI_ST, SD_ID128_FORMAT_VAL(id)) < 0)
                return -ENOMEM;

        if (ret_seq)
                *ret_seq = TAKE_PTR(seq);
        else {
                fputs(seq, stdout);
                fflush(stdout);
        }

        return 0;
}
