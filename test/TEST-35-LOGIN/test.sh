#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="LOGIN"
IMAGE_NAME="default"
TEST_SAVE_JOURNAL="yes"

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

QEMU_TIMEOUT=800

do_test "$@"
