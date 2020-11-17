#!/usr/bin/env bash
set -e
TEST_DESCRIPTION="https://github.com/systemd/systemd/issues/2691"
TEST_NO_NSPAWN=1

. $(dirname ${BASH_SOURCE[0]})/../test-functions
QEMU_TIMEOUT=300

do_test "$@" 09
