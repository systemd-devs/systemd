#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="test systemd-repart"
IMAGE_NAME="repart"
TEST_FORCE_NEWIMAGE=1

# shellcheck source=test/test-functions
. "$TEST_BASE_DIR/test-functions"

test_append_files() {
    if ! get_bool "${TEST_NO_QEMU:=}"; then
        install_dmevent
        if command -v openssl >/dev/null 2>&1; then
            inst_binary openssl
        fi
        instmods dm_verity =md
        generate_module_dependencies
        command -v mkfs.btrfs >/dev/null && install_btrfs
        image_install -o /sbin/mksquashfs
    fi
}

do_test "$@"
