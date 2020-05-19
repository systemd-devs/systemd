/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <stdlib.h>

#include "alloc-util.h"
#include "libcrypt-util.h"
#include "log.h"
#include "macro.h"
#include "missing_stdlib.h"
#include "random-util.h"
#include "string-util.h"
#include "strv.h"

int make_salt(char **ret) {

#ifdef XCRYPT_VERSION_MAJOR
        const char *e;
        char *salt;

        /* If we have libxcrypt we default to the "preferred method" (i.e. usually yescrypt), and generate it
         * with crypt_gensalt_ra(). */

        e = secure_getenv("SYSTEMD_CRYPT_PREFIX");
        if (!e)
                e = crypt_preferred_method();

        log_debug("Generating salt for hash prefix: %s", e);

        salt = crypt_gensalt_ra(e, 0, NULL, 0);
        if (!salt)
                return -errno;

        *ret = salt;
        return 0;
#else
        /* If libxcrypt is not used, we use SHA512 and generate the salt on our own since crypt_gensalt_ra()
         * is not available. */

        static const char table[] =
                "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789"
                "./";

        uint8_t raw[16];
        char *salt, *j;
        size_t i;
        int r;

        /* This is a bit like crypt_gensalt_ra(), but doesn't require libcrypt, and doesn't do anything but
         * SHA512, i.e. is legacy-free and minimizes our deps. */

        assert_cc(sizeof(table) == 64U + 1U);

        /* Insist on the best randomness by setting RANDOM_BLOCK, this is about keeping passwords secret after all. */
        r = genuine_random_bytes(raw, sizeof(raw), RANDOM_BLOCK);
        if (r < 0)
                return r;

        salt = new(char, 3+sizeof(raw)+1+1);
        if (!salt)
                return -ENOMEM;

        /* We only bother with SHA512 hashed passwords, the rest is legacy, and we don't do legacy. */
        j = stpcpy(salt, "$6$");
        for (i = 0; i < sizeof(raw); i++)
                j[i] = table[raw[i] & 63];
        j[i++] = '$';
        j[i] = 0;

        *ret = salt;
        return 0;
#endif
}

bool hashed_password_valid(const char *s) {

        /* Returns true if the specified string is a 'valid' hashed UNIX password, i.e. if starts with '$' or
         * with '!$' (the latter being a valid, yet locked password). */

        if (isempty(s))
                return false;

        return STARTSWITH_SET(s, "$", "!$");
}
