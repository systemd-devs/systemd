#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e

TEST_DESCRIPTION="test credentials"

NSPAWN_ARGUMENTS="${NSPAWN_ARGUMENTS:-} "\
"--set-credential=mynspawncredential:strangevalue"
QEMU_OPTIONS="${QEMU_OPTIONS:-} "\
"-fw_cfg name=opt/io.systemd.credentials/myqemucredential,string=othervalue "\
"-smbios type=11,value=io.systemd.credential:smbioscredential=magicdata "\
"-smbios type=11,value=io.systemd.credential.binary:binarysmbioscredential=bWFnaWNiaW5hcnlkYXRh "\
"-smbios type=11,value=io.systemd.credential.binary:sysusers.extra=dSBjcmVkdGVzdHVzZXIK "\
"-smbios type=11,value=io.systemd.credential.binary:tmpfiles.extra=ZiAvdG1wL3NvdXJjZWRmcm9tY3JlZGVudGlhbCAtIC0gLSAtIHRtcGZpbGVzc2VjcmV0Cg== "
KERNEL_APPEND="${KERNEL_APPEND:-} "\
"systemd.set_credential=kernelcmdlinecred:uff "\
"systemd.set_credential=sysctl.extra:kernel.domainname=sysctltest "\
"systemd.set_credential=login.motd:hello "\
"systemd.set_credential=login.issue:welcome "\
"rd.systemd.import_credentials=no"

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

test_append_files() {
    instmods qemu_fw_cfg
    generate_module_dependencies
}

do_test "$@"
