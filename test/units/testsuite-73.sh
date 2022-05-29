#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eux
set -o pipefail

setup_cron() {
    # Setup test user and cron
    useradd test
    echo "test" | passwd --stdin test
    crond -s -n &
    # Install crontab for the test user that runs sleep every minute. But let's sleep for
    # 65 seconds to make sure there is overlap between two consecutive runs, i.e. we have
    # always a cron session running.
    crontab -u test - <<EOF
RANDOM_DELAY=0
* * * * * /bin/sleep 65
EOF
    # Let's a bit more than one interval to make sure that cron session is started already
    sleep 70
}

teardown_cron() {
    set +e

    pkill -11  -u "$(id -u test)"
    pkill crond
    crontab -r -u test
    userdel -r test
}

test_no_user_instance_for_cron() {
    trap teardown_cron EXIT
    setup_cron

    if [[ $(loginctl --no-legend list-sessions | grep -c test) -lt 1 ]]; then
        echo >&2 '"test" user should have at least one session'
        loginctl list-sessions
        return 1
    fi

    # Check that all sessions of test user have class=background and no user instance was started
    # for the test user.
    while read -r s _; do
        local class

        class=$(loginctl --property Class --value show-session "$s")
        if [[ "$class" != "background" ]]; then
            echo >&2 "Session has incorrect class, expected \"background\", got \"$class\"."
            return 1
        fi
    done < <(loginctl --no-legend list-sessions | grep test)

    state=$(systemctl --property ActiveState --value show user@"$(id -u test)".service)
    if [[ "$state" != "inactive" ]]; then
        echo >&2 "User instance state is unexpected, expected \"inactive\", got \"$state\""
        return 1
    fi

    state=$(systemctl --property SubState --value show user@"$(id -u test)".service)
    if [[ "$state" != "dead" ]]; then
        echo >&2 "User instance state is unexpected, expected \"dead\", got \"$state\""
        return 1
    fi

    return 0
}

: >/failed

if command -v passwd && command -v useradd && command -v userdel && command -v crond && command -v crontab ; then
    test_no_user_instance_for_cron
else
    echo >&2 "Skipping test for background cron sessions because required binaries are missing."
fi

rm /failed
: >/testok
