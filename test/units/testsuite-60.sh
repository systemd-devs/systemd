#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

test_issue_20329() {
    local tmpdir unit
    tmpdir="$(mktemp -d)"
    unit=$(systemd-escape --suffix mount --path "$tmpdir")

    # Set up test mount unit
    cat > /run/systemd/system/"$unit" <<EOF
[Mount]
What=tmpfs
Where=$tmpdir
Type=tmpfs
Options=defaults,nofail
EOF

    # Start the unit
    systemctl daemon-reload
    systemctl start "$unit"

    [[ "$(systemctl show --property SubState --value "$unit")" = "mounted" ]] || {
        echo >&2 "Test mount \"$unit\" unit isn't mounted"
        return 1
    }
    mountpoint -q "$tmpdir"

    trap 'systemctl stop $unit' RETURN

    # Trigger the mount ratelimiting
    cd "$(mktemp -d)"
    mkdir foo
    for ((i = 0; i < 50; i++)); do
        mount --bind foo foo
        umount foo
    done

    # Unmount the test mount and start it immediately again via systemd
    umount "$tmpdir"
    systemctl start "$unit"

    # Make sure it is seen as mounted by systemd and it actually is mounted
    [[ "$(systemctl show --property SubState --value "$unit")" = "mounted" ]] || {
        echo >&2 "Test mount \"$unit\" unit isn't in \"mounted\" state"
        return 1
    }

    mountpoint -q "$tmpdir" || {
        echo >&2 "Test mount \"$unit\" is in \"mounted\" state, actually is not mounted"
        return 1
    }
}

: >/failed

systemd-analyze log-level debug
systemd-analyze log-target journal

NUM_DIRS=20

# make sure we can handle mounts at very long paths such that mount unit name must be hashed to fall within our unit name limit
LONGPATH="$(printf "/$(printf "x%0.s" {1..255})%0.s" {1..7})"
LONGMNT="$(systemd-escape --suffix=mount --path "$LONGPATH")"
TS="$(date '+%H:%M:%S')"

mkdir -p "$LONGPATH"
mount -t tmpfs tmpfs "$LONGPATH"
systemctl daemon-reload

# check that unit is active(mounted)
systemctl --no-pager show -p SubState --value "$LONGPATH" | grep -q mounted

# check that relevant part of journal doesn't contain any errors related to unit
[ "$(journalctl -b --since="$TS" --priority=err | grep -c "$LONGMNT")" = "0" ]

# check that we can successfully stop the mount unit
systemctl stop "$LONGPATH"
rm -rf "$LONGPATH"

# mount/unmount enough times to trigger the /proc/self/mountinfo parsing rate limiting

for ((i = 0; i < NUM_DIRS; i++)); do
    mkdir "/tmp/meow${i}"
done

TS="$(date '+%H:%M:%S')"

for ((i = 0; i < NUM_DIRS; i++)); do
    mount -t tmpfs tmpfs "/tmp/meow${i}"
done

systemctl daemon-reload
systemctl list-units -t mount tmp-meow* | grep -q tmp-meow

for ((i = 0; i < NUM_DIRS; i++)); do
    umount "/tmp/meow${i}"
done

# Figure out if we have entered the rate limit state.
# If the infra is slow we might not enter the rate limit state; in that case skip the exit check.
if timeout 2m bash -c "while ! journalctl -u init.scope --since=$TS | grep -q '(mount-monitor-dispatch) entered rate limit'; do sleep 1; done"; then
    timeout 2m bash -c "while ! journalctl -u init.scope --since=$TS | grep -q '(mount-monitor-dispatch) left rate limit'; do sleep 1; done"
fi

# Verify that the mount units are always cleaned up at the end.
# Give some time for units to settle so we don't race between exiting the rate limit state and cleaning up the units.
timeout 2m bash -c 'while systemctl list-units -t mount tmp-meow* | grep -q tmp-meow; do systemctl daemon-reload; sleep 10; done'

# test that handling of mount start jobs is delayed when /proc/self/mouninfo monitor is rate limited
test_issue_20329

systemd-analyze log-level info

touch /testok
rm /failed
