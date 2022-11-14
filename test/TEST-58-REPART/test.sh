#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="test systemd-repart"

# shellcheck source=test/test-functions
. "$TEST_BASE_DIR/test-functions"

test_append_files() {
    if ! get_bool "${TEST_NO_QEMU:=}"; then
        install_dmevent
        instmods dm_verity =md
        generate_module_dependencies
    fi

    inst_binary mcopy
    if command -v openssl >/dev/null 2>&1; then
        inst_binary openssl
    fi
}

do_test "$@"
