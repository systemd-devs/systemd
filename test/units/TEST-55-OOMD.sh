#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

# shellcheck source=test/units/test-control.sh
. "$(dirname "$0")"/test-control.sh
# shellcheck source=test/units/util.sh
. "$(dirname "$0")"/util.sh

systemd-analyze log-level debug

# Ensure that the init.scope.d drop-in is applied on boot
test "$(cat /sys/fs/cgroup/init.scope/memory.high)" != "max"

# Loose checks to ensure the environment has the necessary features for systemd-oomd
[[ -e /proc/pressure ]] || echo "no PSI" >>/skipped
[[ "$(get_cgroup_hierarchy)" == "unified" ]] || echo "no cgroupsv2" >>/skipped
[[ -x /usr/lib/systemd/systemd-oomd ]] || echo "no oomd" >>/skipped
if [[ -s /skipped ]]; then
    exit 77
fi

# Activate swap file if we are in a VM
if systemd-detect-virt --vm --quiet; then
    swapoff --all
    if [[ "$(findmnt -n -o FSTYPE /)" == btrfs ]]; then
        btrfs filesystem mkswapfile -s 64M /swapfile
    else
        dd if=/dev/zero of=/swapfile bs=1M count=64
        chmod 0600 /swapfile
        mkswap /swapfile
    fi

    swapon /swapfile
    swapon --show
fi

# Configure oomd explicitly to avoid conflicts with distro dropins
mkdir -p /run/systemd/oomd.conf.d/
cat >/run/systemd/oomd.conf.d/99-oomd-test.conf <<EOF
[OOM]
DefaultMemoryPressureDurationSec=2s
EOF

mkdir -p /run/systemd/system/-.slice.d/
cat >/run/systemd/system/-.slice.d/99-oomd-test.conf <<EOF
[Slice]
ManagedOOMSwap=auto
EOF

mkdir -p /run/systemd/system/user@.service.d/
cat >/run/systemd/system/user@.service.d/99-oomd-test.conf <<EOF
[Service]
ManagedOOMMemoryPressure=auto
ManagedOOMMemoryPressureLimit=0%
EOF

mkdir -p /run/systemd/system/systemd-oomd.service.d/
cat >/run/systemd/system/systemd-oomd.service.d/debug.conf <<EOF
[Service]
Environment=SYSTEMD_LOG_LEVEL=debug
EOF

systemctl daemon-reload

# enable the service to ensure dbus-org.freedesktop.oom1.service exists
# and D-Bus activation works
systemctl enable systemd-oomd.service

# if oomd is already running for some reasons, then restart it to make sure the above settings to be applied
if systemctl is-active systemd-oomd.service; then
    systemctl restart systemd-oomd.service
fi

# Check if the oomd.conf drop-in config is loaded.
oomctl | grep -q -F 'Default Memory Pressure Duration: 2s'

if [[ -v ASAN_OPTIONS || -v UBSAN_OPTIONS ]]; then
    # If we're running with sanitizers, sd-executor might pull in quite a significant chunk of shared
    # libraries, which in turn causes a lot of pressure that can put us in the front when sd-oomd decides to
    # go on a killing spree. This fact is exacerbated further on Arch Linux which ships unstripped gcc-libs,
    # so sd-executor pulls in over 30M of libs on startup. Let's make the MemoryHigh= limit a bit more
    # generous when running with sanitizers to make the test happy.
    systemctl edit --runtime --stdin --drop-in=99-MemoryHigh.conf TEST-55-OOMD-testchill.service <<EOF
[Service]
MemoryHigh=60M
EOF
    # Do the same for the user instance as well
    mkdir -p /run/systemd/user/
    cp -rfv /run/systemd/system/TEST-55-OOMD-testchill.service.d/ /run/systemd/user/
else
    # Ensure that we can start services even with a very low hard memory cap without oom-kills, but skip
    # under sanitizers as they balloon memory usage.
    systemd-run -t -p MemoryMax=10M -p MemorySwapMax=0 -p MemoryZSwapMax=0 /bin/true
fi

NEEDS_COOL_DOWN=$(mktemp --dry-run /tmp/TEST-OOM-NEEDS-COOL-DOWN-XXXXXX)

cool_down() {
    if [[ -e "$NEEDS_COOL_DOWN" ]]; then
        sleep 120s
    fi

    rm -f "$NEEDS_COOL_DOWN"
}

test_basic() {
    local cgroup_path="${1:?}"
    shift

    systemctl "$@" start TEST-55-OOMD-testchill.service
    systemctl "$@" status TEST-55-OOMD-testchill.service
    systemctl "$@" status TEST-55-OOMD-workload.slice

    # Verify systemd-oomd is monitoring the expected units
    timeout 1m bash -xec "until oomctl | grep -q -F 'Path: $cgroup_path'; do sleep 1; done"
    oomctl | grep -A7 "Path: $cgroup_path" | grep -q -F '20.00%'

    systemctl "$@" start TEST-55-OOMD-testbloat.service

    # systemd-oomd watches for elevated pressure for 2 seconds before acting.
    # It can take time to build up pressure so either wait 2 minutes or for the service to fail.
    for _ in {0..59}; do
        oomctl
        if ! systemctl "$@" status TEST-55-OOMD-testbloat.service; then
            break
        fi
        sleep 2
    done

    # testbloat should be killed and testchill should be fine
    if systemctl "$@" status TEST-55-OOMD-testbloat.service; then
        # FIXME: This may be a regression in kernel 6.12.
        # Hopefully, it will be fixed before the final release.
        if uname -r | grep -q '6\.12\.0.*rc[0-9]'; then
            echo "testbloat.service is unexpectedly still alive. Running on kernel $(uname -r), ignoring."
        else
            exit 42;
        fi
    fi
    if ! systemctl "$@" status TEST-55-OOMD-testchill.service; then exit 24; fi

    systemctl "$@" kill --signal=KILL TEST-55-OOMD-testbloat.service || :
    systemctl "$@" stop TEST-55-OOMD-testbloat.service
    systemctl "$@" stop TEST-55-OOMD-testchill.service
    systemctl "$@" stop TEST-55-OOMD-workload.slice
}

testcase_basic_system() {
    cool_down

    test_basic /TEST.slice/TEST-55.slice/TEST-55-OOMD.slice/TEST-55-OOMD-workload.slice

    touch "$NEEDS_COOL_DOWN"
}

testcase_basic_user() {
    # Make sure we also work correctly on user units.
    loginctl enable-linger testuser

    test_basic "/user.slice/user-$(id -u testuser).slice/user@$(id -u testuser).service/TEST.slice/TEST-55.slice/TEST-55-OOMD.slice/TEST-55-OOMD-workload.slice" \
               --machine "testuser@.host" --user

    loginctl disable-linger testuser
}

testcase_preference_avoid() {
    # only run this portion of the test if we can set xattrs
    if ! cgroupfs_supports_user_xattrs; then
        echo "cgroup does not support user xattrs, skipping test for ManagedOOMPreference=avoid"
        return 0
    fi

    cool_down

    mkdir -p /run/systemd/system/TEST-55-OOMD-testbloat.service.d/
    cat >/run/systemd/system/TEST-55-OOMD-testbloat.service.d/override.conf <<EOF
[Service]
ManagedOOMPreference=avoid
EOF

    systemctl daemon-reload
    systemctl start TEST-55-OOMD-testchill.service
    systemctl start TEST-55-OOMD-testmunch.service
    systemctl start TEST-55-OOMD-testbloat.service

    for _ in {0..59}; do
        oomctl
        if ! systemctl status TEST-55-OOMD-testmunch.service; then
            break
        fi
        sleep 2
    done

    # testmunch should be killed since testbloat had the avoid xattr on it
    if ! systemctl status TEST-55-OOMD-testbloat.service; then exit 25; fi
    if systemctl status TEST-55-OOMD-testmunch.service; then
        # FIXME: This may be a regression in kernel 6.12.
        # Hopefully, it will be fixed before the final release.
        if uname -r | grep -q '6\.12\.0.*rc[0-9]'; then
            echo "testmunch.service is unexpectedly still alive. Running on kernel $(uname -r), ignoring."
        else
            exit 43;
        fi
    fi
    if ! systemctl status TEST-55-OOMD-testchill.service; then exit 24; fi

    systemctl kill --signal=KILL TEST-55-OOMD-testbloat.service || :
    systemctl kill --signal=KILL TEST-55-OOMD-testmunch.service || :
    systemctl stop TEST-55-OOMD-testbloat.service
    systemctl stop TEST-55-OOMD-testmunch.service
    systemctl stop TEST-55-OOMD-testchill.service
    systemctl stop TEST-55-OOMD-workload.slice

    touch "$NEEDS_COOL_DOWN"
}

run_testcases

systemd-analyze log-level info

touch /testok
