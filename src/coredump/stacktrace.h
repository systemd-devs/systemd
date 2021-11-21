/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "json.h"

/* Parse an ELF object in a forked process, so that errors while iterating over
 * untrusted and potentially malicious data do not propagate to the main caller's process.
 * If fork_disable_dump, the child process will not dump core if it crashes. */
int parse_elf_object(int fd, const char *executable, bool fork_disable_dump, char **ret, JsonVariant **ret_package_metadata);
