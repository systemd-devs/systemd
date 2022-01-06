#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -ex

if [[ -n "$1" ]]; then
    generator=$1
elif [[ -x /usr/lib/systemd/system-generators/systemd-fstab-generator ]]; then
    generator=/usr/lib/systemd/system-generators/systemd-fstab-generator
elif [[ -x /lib/systemd/system-generators/systemd-fstab-generator ]]; then
    generator=/lib/systemd/system-generators/systemd-fstab-generator
else
    exit 1
fi

src="$(dirname "$0")/testdata/test-fstab-generator"

split_usr=0
if [[ ! -d /usr/lib/systemd && -d /lib/systemd ]]; then
    split_usr=1
fi

for f in "$src"/test-*.input; do
    echo "*** Running $f"

    (
        out=$(mktemp --tmpdir --directory "test-fstab-generator.XXXXXXXXXX")
        # shellcheck disable=SC2064
        trap "rm -rf '$out'" EXIT INT QUIT PIPE

        # shellcheck disable=SC2046
        SYSTEMD_LOG_LEVEL=debug SYSTEMD_IN_INITRD=yes SYSTEMD_PROC_CMDLINE="fstab=no $(cat "$f")" $generator "$out" "$out" "$out"

        if [[ "$split_usr" == 1 && -f "$out"/systemd-fsck-root.service ]]; then
            sed -i -e 's:/lib/systemd/systemd-fsck:/usr/lib/systemd/systemd-fsck:' "$out"/systemd-fsck-root.service
        fi

        if ! diff -u "$out" "${f%.input}.expected"; then
            echo "**** Unexpected output for $f"
            exit 1
        fi
    ) || exit 1
done
