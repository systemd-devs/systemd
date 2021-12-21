#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eu
set -o pipefail

${1:?} -dM -include errno.h - </dev/null | \
       grep -Ev '^#define[[:space:]]+(ECANCELLED|EREFUSED)' | \
       awk '/^#define[ \t]+E[^ _]+[ \t]+/ { print $2; }'
