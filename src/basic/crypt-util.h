/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2017 Zbigniew Jędrzejewski-Szmek
***/

#if HAVE_LIBCRYPTSETUP
#include <libcryptsetup.h>

#include "macro.h"

/* libcryptsetup define for any LUKS version, compatible with libcryptsetup 1.x */
#ifndef CRYPT_LUKS
#define CRYPT_LUKS NULL
#endif

DEFINE_TRIVIAL_CLEANUP_FUNC(struct crypt_device *, crypt_free);

void cryptsetup_log_glue(int level, const char *msg, void *usrptr);
#endif
