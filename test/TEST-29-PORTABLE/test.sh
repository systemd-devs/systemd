#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
set -e

TEST_DESCRIPTION="test systemd-portabled"
IMAGE_NAME="portabled"
TEST_NO_NSPAWN=1
TEST_INSTALL_VERITY_MINIMAL=1

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

# Need loop devices for mounting images
test_append_files() {
    (
        instmods loop =block
        instmods squashfs =squashfs
        instmods dm_verity =md
        instmods overlay =overlayfs
        install_dmevent
        generate_module_dependencies
        inst_binary mksquashfs
        inst_binary unsquashfs
        inst_binary dd
        inst_binary openssl || echo "openssl not found, skipping fsverity test"
        inst_binary fsverity || echo "fsverity not found, skipping fsverity test"
        inst_binary xxd || echo "xxd not found, skipping fsverity test"
        inst_binary tune2fs || echo "tune2fs not found, skipping fsverity test"
        inst_binary keyctl || echo "keyctl not found, skipping fsverity test"
        install_verity_minimal
    )
}

do_test "$@"
