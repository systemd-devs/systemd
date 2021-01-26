#!/usr/bin/env bash
set -ex
set -o pipefail

systemd-analyze log-level debug
systemd-analyze log-target console

# Loose checks to ensure the environment has the necessary features for systemd-oomd
[[ -e /proc/pressure ]] || echo "no PSI" >> /skipped
cgroup_type=$(stat -fc %T /sys/fs/cgroup/)
if [[ "$cgroup_type" != *"cgroup2"* ]] && [[ "$cgroup_type" != *"0x63677270"* ]]; then
    echo "no cgroup2" >> /skipped
fi
[[ -e /skipped ]] && exit 0 || true

echo "DefaultMemoryPressureDurationSec=5s" >> /etc/systemd/oomd.conf

systemctl start testsuite-56-testchill.service
systemctl start testsuite-56-testbloat.service

# Verify systemd-oomd is monitoring the expected units
oomctl | grep "/testsuite-56-workload.slice"
oomctl | grep "1.00%"
oomctl | grep "Default Memory Pressure Duration: 5s"

# systemd-oomd watches for elevated pressure for 5 seconds before acting.
# It can take time to build up pressure so either wait 2 minutes or for the service to fail.
timeout=$(date -ud "2 minutes" +%s)
while [[ $(date -u +%s) -le $timeout ]]; do
    if ! systemctl status testsuite-56-testbloat.service; then
        break
    fi
    sleep 5
done

# testbloat should be killed and testchill should be fine
if systemctl status testsuite-56-testbloat.service; then exit 42; fi
if ! systemctl status testsuite-56-testchill.service; then exit 24; fi

# only run this portion of the test if we can set xattrs
if setfattr -n user.xattr_test -v 1 /sys/fs/cgroup/; then
    sleep 120 # wait for systemd-oomd kill cool down and elevated memory pressure to come down

    systemctl start testsuite-56-testchill.service
    systemctl start testsuite-56-testmunch.service
    systemctl start testsuite-56-testbloat.service
    setfattr -n user.oomd_avoid -v 1 /sys/fs/cgroup/testsuite.slice/testsuite-56.slice/testsuite-56-workload.slice/testsuite-56-testbloat.service

    timeout=$(date -ud "2 minutes" +%s)
    while [[ $(date -u +%s) -le $timeout ]]; do
        if ! systemctl status testsuite-56-testmunch.service; then
            break
        fi
        sleep 5
    done

    # testmunch should be killed since testbloat had the avoid xattr on it
    if ! systemctl status testsuite-56-testbloat.service; then exit 25; fi
    if systemctl status testsuite-56-testmunch.service; then exit 43; fi
    if ! systemctl status testsuite-56-testchill.service; then exit 24; fi
fi

systemd-analyze log-level info

echo OK > /testok

exit 0
