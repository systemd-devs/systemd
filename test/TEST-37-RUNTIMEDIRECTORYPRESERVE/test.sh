#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
set -e

TEST_DESCRIPTION="test RuntimeDirectoryPreserve=yes"
QEMU_TIMEOUT=600
NSPAWN_TIMEOUT=600
KERNEL_APPEND="systemd.default_standard_output=journal+console systemd.log_target=console"

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

do_test "$@"
