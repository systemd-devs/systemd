#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="Basic systemd setup"
IMAGE_NAME="basic"
RUN_IN_UNPRIVILEGED_CONTAINER=${RUN_IN_UNPRIVILEGED_CONTAINER:-yes}
TEST_REQUIRE_INSTALL_TESTS=0
TEST_MASK_SERVICES=0

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

test_append_files() {
    # install tests manually so the test is functional even when -Dinstall-tests=false
    local dst="${1:?}/usr/lib/systemd/tests/testdata/units/"
    mkdir -p "$dst"
    cp -v "$TEST_UNITS_DIR"/{testsuite-01,end}.service "$TEST_UNITS_DIR/testsuite.target" "$dst"
}

do_test "$@"
