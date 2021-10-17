#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="Test auto restart of exited services which are stuck in reloading state"
TEST_NO_QEMU=1

# shellcheck source=test/test-functions
. "$TEST_BASE_DIR/test-functions"

do_test "$@"
