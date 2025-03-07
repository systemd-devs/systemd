#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# shellcheck disable=SC2016
set -eux
set -o pipefail

# shellcheck source=test/units/test-control.sh
. "$(dirname "$0")"/test-control.sh

if ! command -v /usr/lib/systemd/systemd-sbsign >/dev/null; then
    echo "systemd-sbsign not found, skipping."
    exit 0
fi

if [[ ! -d /usr/lib/systemd/boot/efi ]]; then
    echo "systemd-boot is not installed, skipping."
    exit 0
fi

cat >/tmp/openssl.conf <<EOF
[ req ]
prompt = no
distinguished_name = req_distinguished_name

[ req_distinguished_name ]
C = DE
ST = Test State
L = Test Locality
O = Org Name
OU = Org Unit Name
CN = Common Name
emailAddress = test@email.com
EOF

openssl req -config /tmp/openssl.conf -subj="/CN=waldo" \
        -x509 -sha256 -nodes -days 365 -newkey rsa:4096 \
        -keyout /tmp/sb.key -out /tmp/sb.crt

testcase_sign_systemd_boot() {
    if ! command -v sbverify >/dev/null; then
        echo "sbverify not found, skipping."
        return 0
    fi

    SD_BOOT="$(find /usr/lib/systemd/boot/efi/ -name "systemd-boot*.efi" | head -n1)"
    (! sbverify --cert /tmp/sb.crt "$SD_BOOT")

    /usr/lib/systemd/systemd-sbsign sign --certificate /tmp/sb.crt --private-key /tmp/sb.key --output /tmp/sdboot "$SD_BOOT"
    sbverify --cert /tmp/sb.crt /tmp/sdboot

    # Make sure appending signatures to an existing certificate table works as well.
    /usr/lib/systemd/systemd-sbsign sign --certificate /tmp/sb.crt --private-key /tmp/sb.key --output /tmp/sdboot /tmp/sdboot
    sbverify --cert /tmp/sb.crt /tmp/sdboot
}

testcase_sign_systemd_boot_offline() {
    if ! command -v sbverify >/dev/null; then
        echo "sbverify not found, skipping."
        return 0
    fi

    SD_BOOT="$(find /usr/lib/systemd/boot/efi/ -name "systemd-boot*.efi" | head -n1)"
    (! sbverify --cert /tmp/sb.crt "$SD_BOOT")

    export SOURCE_DATE_EPOCH="123"

    /usr/lib/systemd/systemd-sbsign \
        sign \
        --certificate /tmp/sb.crt \
        --private-key /tmp/sb.key \
        --output /tmp/sdboot-signed-online \
        "$SD_BOOT"

    sbverify --cert /tmp/sb.crt /tmp/sdboot-signed-online

    /usr/lib/systemd/systemd-sbsign \
        sign \
        --certificate /tmp/sb.crt \
        --output /tmp/signed-data.bin \
        --prepare-offline-signing \
        "$SD_BOOT"
    openssl dgst -sha256 -sign /tmp/sb.key -out /tmp/signed-data.sig /tmp/signed-data.bin

    # Make sure systemd-sbsign can't pick up the timestamp from the environment when
    # attaching the signature.
    unset SOURCE_DATE_EPOCH

    /usr/lib/systemd/systemd-sbsign \
        sign \
        --certificate /tmp/sb.crt \
        --output /tmp/sdboot-signed-offline \
        --signed-data /tmp/signed-data.bin \
        --signed-data-signature /tmp/signed-data.sig \
        "$SD_BOOT"

    sbverify --cert /tmp/sb.crt /tmp/sdboot-signed-offline

    cmp /tmp/sdboot-signed-online /tmp/sdboot-signed-offline
}

testcase_sign_secure_boot_database() {
    /usr/lib/systemd/systemd-sbsign \
        sign-secure-boot-database \
        --certificate /tmp/sb.crt \
        --private-key /tmp/sb.key \
        --output /tmp/PK.signed \
        --secure-boot-database PK
}

testcase_sign_secure_boot_database_offline() {
    export SOURCE_DATE_EPOCH="123"

    /usr/lib/systemd/systemd-sbsign \
        sign-secure-boot-database \
        --certificate /tmp/sb.crt \
        --private-key /tmp/sb.key \
        --output /tmp/PK.signed-online \
        --secure-boot-database PK

    /usr/lib/systemd/systemd-sbsign \
        sign-secure-boot-database \
        --certificate /tmp/sb.crt \
        --output /tmp/signed-data.bin \
        --prepare-offline-signing \
        --secure-boot-database PK
    openssl dgst -sha256 -sign /tmp/sb.key -out /tmp/signed-data.sig /tmp/signed-data.bin

    # Make sure systemd-sbsign can't pick up the timestamp from the environment when
    # attaching the signature.
    unset SOURCE_DATE_EPOCH

    /usr/lib/systemd/systemd-sbsign \
        sign-secure-boot-database \
        --certificate /tmp/sb.crt \
        --output /tmp/PK.signed-offline \
        --signed-data /tmp/signed-data.bin \
        --signed-data-signature /tmp/signed-data.sig \
        --secure-boot-database PK

    cmp /tmp/PK.signed-offline /tmp/PK.signed-online
}

run_testcases
