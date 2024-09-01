#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# -*- mode: shell-script; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# ex: ts=8 sw=4 sts=4 et filetype=sh
set -eux
set -o pipefail

# shellcheck source=test/units/util.sh
. "$(dirname "$0")"/util.sh

install_extension_images

# Set longer timeout for slower machines, e.g. non-KVM vm.
mkdir -p /run/systemd/system.conf.d
cat >/run/systemd/system.conf.d/10-timeout.conf <<EOF
[Manager]
DefaultEnvironment=SYSTEMD_DISSECT_VERITY_TIMEOUT_SEC=30
ManagerEnvironment=SYSTEMD_DISSECT_VERITY_TIMEOUT_SEC=30
EOF

systemctl daemon-reexec

export SYSTEMD_DISSECT_VERITY_TIMEOUT_SEC=30

udevadm control --log-level debug

ARGS=()
STATE_DIRECTORY=/var/lib/private/
if [[ -v ASAN_OPTIONS || -v UBSAN_OPTIONS ]]; then
    # If we're running under sanitizers, we need to use a less restrictive
    # profile, otherwise LSan syscall would get blocked by seccomp
    ARGS+=(--profile=trusted)
    # With the trusted profile DynamicUser is disabled, so the storage is not in private/
    STATE_DIRECTORY=/var/lib/
fi

systemd-dissect --no-pager /usr/share/minimal_0.raw | grep -q '✓ portable service'
systemd-dissect --no-pager /usr/share/minimal_1.raw | grep -q '✓ portable service'
systemd-dissect --no-pager /tmp/app0.raw | grep -q '✓ sysext for portable service'
systemd-dissect --no-pager /tmp/app1.raw | grep -q '✓ sysext for portable service'
systemd-dissect --no-pager /tmp/conf0.raw | grep -q '✓ confext for portable service'

export SYSTEMD_LOG_LEVEL=debug
mkdir -p /run/systemd/system/systemd-portabled.service.d/
cat <<EOF >/run/systemd/system/systemd-portabled.service.d/override.conf
[Service]
Environment=SYSTEMD_LOG_LEVEL=debug
EOF

portablectl "${ARGS[@]}" attach --now --runtime /usr/share/minimal_0.raw minimal-app0

portablectl is-attached minimal-app0
portablectl inspect /usr/share/minimal_0.raw minimal-app0.service
systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-foo.service
systemctl is-active minimal-app0-bar.service && exit 1

portablectl "${ARGS[@]}" reattach --now --runtime /usr/share/minimal_1.raw minimal-app0

portablectl is-attached minimal-app0
portablectl inspect /usr/share/minimal_0.raw minimal-app0.service
systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-bar.service
systemctl is-active minimal-app0-foo.service && exit 1

portablectl list | grep -q -F "minimal_1"
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1'

portablectl detach --now --runtime /usr/share/minimal_1.raw minimal-app0

portablectl list | grep -q -F "No images."
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1' && exit 1

# Ensure we don't regress (again) when using --force

portablectl "${ARGS[@]}" attach --force --now --runtime /usr/share/minimal_0.raw minimal-app0

portablectl is-attached --force minimal-app0
portablectl inspect --force /usr/share/minimal_0.raw minimal-app0.service
systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-foo.service
systemctl is-active minimal-app0-bar.service && exit 1

portablectl "${ARGS[@]}" reattach --force --now --runtime /usr/share/minimal_1.raw minimal-app0

portablectl is-attached --force minimal-app0
portablectl inspect --force /usr/share/minimal_0.raw minimal-app0.service
systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-bar.service
systemctl is-active minimal-app0-foo.service && exit 1

portablectl list | grep -q -F "minimal_1"
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1'

portablectl detach --force --now --runtime /usr/share/minimal_1.raw minimal-app0

portablectl list | grep -q -F "No images."
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1' && exit 1

# portablectl also works with directory paths rather than images

unsquashfs -dest /tmp/minimal_0 /usr/share/minimal_0.raw
unsquashfs -dest /tmp/minimal_1 /usr/share/minimal_1.raw

portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime /tmp/minimal_0 minimal-app0

systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-foo.service
systemctl is-active minimal-app0-bar.service && exit 1

portablectl "${ARGS[@]}" reattach --now --enable --runtime /tmp/minimal_1 minimal-app0

systemctl is-active minimal-app0.service
systemctl is-active minimal-app0-bar.service
systemctl is-active minimal-app0-foo.service && exit 1

portablectl list | grep -q -F "minimal_1"
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1'

portablectl detach --now --enable --runtime /tmp/minimal_1 minimal-app0

portablectl list | grep -q -F "No images."
busctl tree org.freedesktop.portable1 --no-pager | grep -q -F '/org/freedesktop/portable1/image/minimal_5f1' && exit 1

portablectl "${ARGS[@]}" attach --now --runtime --extension /tmp/app0.raw /usr/share/minimal_0.raw app0

systemctl is-active app0.service
status="$(portablectl is-attached --extension app0 minimal_0)"
[[ "${status}" == "running-runtime" ]]

grep -q -F "LogExtraFields=PORTABLE_ROOT=minimal_0.raw" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION=app0.raw" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION_NAME_AND_VERSION=app" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "org.freedesktop.app0" /etc/dbus-1/system.d/app0.conf
grep -q -F "org.freedesktop.app0" /etc/dbus-1/system-services/app0.service
grep -q -F "org.freedesktop.app0" /etc/polkit-1/actions/app0.policy

portablectl "${ARGS[@]}" reattach --now --runtime --extension /tmp/app0.raw /usr/share/minimal_1.raw app0

systemctl is-active app0.service
status="$(portablectl is-attached --extension app0 minimal_1)"
[[ "${status}" == "running-runtime" ]]

grep -q -F "LogExtraFields=PORTABLE_ROOT=minimal_1.raw" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION=app0.raw" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION_NAME_AND_VERSION=app" /run/systemd/system.attached/app0.service.d/20-portable.conf

portablectl detach --now --runtime --extension /tmp/app0.raw /usr/share/minimal_1.raw app0

test ! -f /etc/dbus-1/system.d/app0.conf
test ! -f /etc/dbus-1/system-services/app0.service
test ! -f /etc/polkit-1/actions/app0.policy

# Ensure versioned images are accepted without needing to use --force to override the extension-release
# matching

cp /tmp/app0.raw /tmp/app0_1.0.raw
portablectl "${ARGS[@]}" attach --now --runtime --extension /tmp/app0_1.0.raw /usr/share/minimal_0.raw app0

systemctl is-active app0.service
status="$(portablectl is-attached --extension app0_1 minimal_0)"
[[ "${status}" == "running-runtime" ]]

portablectl detach --now --runtime --extension /tmp/app0_1.0.raw /usr/share/minimal_1.raw app0
rm -f /tmp/app0_1.0.raw

portablectl "${ARGS[@]}" attach --now --runtime --extension /tmp/app1.raw /usr/share/minimal_0.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1 minimal_0)"
[[ "${status}" == "running-runtime" ]]

# Ensure that adding or removing a version to the image doesn't break reattaching
cp /tmp/app1.raw /tmp/app1_2.raw
portablectl "${ARGS[@]}" reattach --now --runtime --extension /tmp/app1_2.raw /usr/share/minimal_1.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1_2 minimal_1)"
[[ "${status}" == "running-runtime" ]]

portablectl "${ARGS[@]}" reattach --now --runtime --extension /tmp/app1.raw /usr/share/minimal_1.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1 minimal_1)"
[[ "${status}" == "running-runtime" ]]

portablectl detach --force --no-reload --runtime --extension /tmp/app1.raw /usr/share/minimal_1.raw app1
portablectl "${ARGS[@]}" attach --force --no-reload --runtime --extension /tmp/app1.raw /usr/share/minimal_0.raw app1
systemctl daemon-reload
systemctl restart app1.service

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1 minimal_0)"
[[ "${status}" == "running-runtime" ]]

portablectl detach --now --runtime --extension /tmp/app1.raw /usr/share/minimal_0.raw app1

# Ensure vpick works, including reattaching to a new image
mkdir -p /tmp/app1.v/
cp /tmp/app1.raw /tmp/app1.v/app1_1.0.raw
cp /tmp/app1_2.raw /tmp/app1.v/app1_2.0.raw
portablectl "${ARGS[@]}" attach --now --runtime --extension /tmp/app1.v/ /usr/share/minimal_1.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1_2.0.raw minimal_1)"
[[ "${status}" == "running-runtime" ]]

rm -f /tmp/app1.v/app1_2.0.raw
portablectl "${ARGS[@]}" reattach --now --runtime --extension /tmp/app1.v/ /usr/share/minimal_1.raw app1

systemctl is-active app1.service
status="$(portablectl is-attached --extension app1_1.0.raw minimal_1)"
[[ "${status}" == "running-runtime" ]]

portablectl detach --now --runtime --extension /tmp/app1.v/ /usr/share/minimal_0.raw app1
rm -f /tmp/app1.v/app1_1.0.raw

# Ensure that the combination of read-only images, state directory and dynamic user works, and that
# state is retained. Check after detaching, as on slow systems (eg: sanitizers) it might take a while
# after the service is attached before the file appears.
grep -q -F bar "${STATE_DIRECTORY}/app0/foo"
grep -q -F baz "${STATE_DIRECTORY}/app1/foo"

# Ensure that we can override the check on extension-release.NAME
cp /tmp/app0.raw /tmp/app10.raw
portablectl "${ARGS[@]}" attach --force --now --runtime --extension /tmp/app10.raw /usr/share/minimal_0.raw app0

systemctl is-active app0.service
status="$(portablectl is-attached --extension /tmp/app10.raw /usr/share/minimal_0.raw)"
[[ "${status}" == "running-runtime" ]]

portablectl inspect --force --cat --extension /tmp/app10.raw /usr/share/minimal_0.raw app0 | grep -q -F "Extension Release: /tmp/app10.raw"

# Ensure that we can detach even when an image has been deleted already (stop the unit manually as
# portablectl won't find it)
rm -f /tmp/app10.raw
systemctl stop app0.service
portablectl detach --force --runtime --extension /tmp/app10.raw /usr/share/minimal_0.raw app0

# portablectl also accepts confexts
portablectl "${ARGS[@]}" attach --now --runtime --extension /tmp/app0.raw --extension /tmp/conf0.raw /usr/share/minimal_0.raw app0

systemctl is-active app0.service
status="$(portablectl is-attached --extension /tmp/app0.raw --extension /tmp/conf0.raw /usr/share/minimal_0.raw)"
[[ "${status}" == "running-runtime" ]]

portablectl inspect --force --cat --extension /tmp/app0.raw --extension /tmp/conf0.raw /usr/share/minimal_0.raw app0 | grep -q -F "Extension Release: /tmp/conf0.raw"

portablectl detach --now --runtime --extension /tmp/app0.raw --extension /tmp/conf0.raw /usr/share/minimal_0.raw app0

# Ensure that mixed mode copies the images and units (client-owned) but symlinks the profile (OS owned)
portablectl "${ARGS[@]}" attach --copy=mixed --runtime --extension /tmp/app0.raw /usr/share/minimal_0.raw app0
test -f /run/portables/app0.raw
test -f /run/portables/minimal_0.raw
test -f /run/systemd/system.attached/app0.service
test -L /run/systemd/system.attached/app0.service.d/10-profile.conf
portablectl detach --runtime --extension /tmp/app0.raw /usr/share/minimal_0.raw app0

# Ensure that when two portables share the same base image, removing one doesn't remove the other too

portablectl "${ARGS[@]}" attach --runtime --extension /tmp/app0.raw /usr/share/minimal_0.raw app0
portablectl "${ARGS[@]}" attach --runtime --extension /tmp/app1.raw /usr/share/minimal_0.raw app1

status="$(portablectl is-attached --extension app0 minimal_0)"
[[ "${status}" == "attached-runtime" ]]
status="$(portablectl is-attached --extension app1 minimal_0)"
[[ "${status}" == "attached-runtime" ]]

(! portablectl detach --runtime /usr/share/minimal_0.raw app)

status="$(portablectl is-attached --extension app0 minimal_0)"
[[ "${status}" == "attached-runtime" ]]
status="$(portablectl is-attached --extension app1 minimal_0)"
[[ "${status}" == "attached-runtime" ]]

# Ensure 'portablectl list' shows the correct status for both images
portablectl list
portablectl list | grep -F "minimal_0" | grep -q -F "attached-runtime"
portablectl list | grep -F "app0" | grep -q -F "attached-runtime"
portablectl list | grep -F "app1" | grep -q -F "attached-runtime"

portablectl detach --runtime --extension /tmp/app0.raw /usr/share/minimal_0.raw app

status="$(portablectl is-attached --extension app1 minimal_0)"
[[ "${status}" == "attached-runtime" ]]

portablectl detach --runtime --extension /tmp/app1.raw /usr/share/minimal_0.raw app

# portablectl also works with directory paths rather than images
mkdir -p /tmp/rootdir \
    /tmp/app0 \
    /tmp/app1 \
    /tmp/overlay \
    /tmp/os-release-fix \
    /tmp/os-release-fix/etc \
    /tmp/os-release-fix/usr/share/dbus-1/system.d/ \
    /tmp/os-release-fix/usr/share/dbus-1/system-services/ \
    /tmp/os-release-fix/usr/share/polkit-1/actions/
mount /tmp/app0.raw /tmp/app0
mount /tmp/app1.raw /tmp/app1
mount /usr/share/minimal_0.raw /tmp/rootdir

cat <<EOF >/tmp/os-release-fix/usr/share/dbus-1/system.d/app1.conf
<?xml version="1.0"?> <!--*-nxml-*-->
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
        "https://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
        <policy user="root">
                <allow own="org.freedesktop.app1"/>
                <allow send_destination="org.freedesktop.app1"/>
                <allow receive_sender="org.freedesktop.app1"/>
        </policy>
        <policy context="default">
                <allow send_destination="org.freedesktop.app1"/>
                <allow receive_sender="org.freedesktop.app1"/>
        </policy>
</busconfig>
EOF
cat <<EOF >/tmp/os-release-fix/usr/share/dbus-1/system-services/app1.service
[D-BUS Service]
Name=org.freedesktop.app1
Exec=/bin/false
User=root
SystemdService=dbus-org.freedesktop.app1.service
EOF
cat <<EOF >/tmp/os-release-fix/usr/share/polkit-1/actions/app1.policy
<?xml version="1.0" encoding="UTF-8"?> <!--*-nxml-*-->
<!DOCTYPE policyconfig PUBLIC "-//freedesktop//DTD PolicyKit Policy Configuration 1.0//EN"
        "https://www.freedesktop.org/standards/PolicyKit/1/policyconfig.dtd">
<policyconfig>
        <vendor>The systemd Project</vendor>
        <vendor_url>https://systemd.io</vendor_url>

        <action id="org.freedesktop.app1">
                <description gettext-domain="systemd">app1</description>
                <message gettext-domain="systemd">Authentication is required for app1</message>
                <defaults>
                        <allow_any>auth_admin</allow_any>
                        <allow_inactive>auth_admin</allow_inactive>
                        <allow_active>auth_admin_keep</allow_active>
                </defaults>
        </action>
</policyconfig>
EOF

# Fix up os-release to drop the valid PORTABLE_SERVICES field (because we are
# bypassing the sysext logic in portabled here it will otherwise not see the
# extensions additional valid prefix)
grep -v "^PORTABLE_PREFIXES=" /tmp/rootdir/etc/os-release >/tmp/os-release-fix/etc/os-release

mount -t overlay overlay -o lowerdir=/tmp/os-release-fix:/tmp/app1:/tmp/rootdir /tmp/overlay

grep . /tmp/overlay/usr/lib/extension-release.d/*
grep . /tmp/overlay/etc/os-release

portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime /tmp/overlay app1

systemctl is-active app1.service

grep -q -F "org.freedesktop.app1" /etc/dbus-1/system.d/app1.conf
grep -q -F "org.freedesktop.app1" /etc/dbus-1/system-services/app1.service
grep -q -F "org.freedesktop.app1" /etc/polkit-1/actions/app1.policy

portablectl detach --now --runtime overlay app1

test ! -f /etc/dbus-1/system.d/app1.conf
test ! -f /etc/dbus-1/system-services/app1.service
test ! -f /etc/polkit-1/actions/app1.policy

umount /tmp/overlay

portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime --extension /tmp/app0 --extension /tmp/app1 /tmp/rootdir app0 app1

systemctl is-active app0.service
systemctl is-active app1.service

portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/rootdir/usr/lib/os-release
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app0/usr/lib/extension-release.d/extension-release.app0
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app1/usr/lib/extension-release.d/extension-release.app2
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app1/usr/lib/systemd/system/app1.service
portablectl inspect --cat --extension app0 --extension app1 rootdir app0 app1 | grep -q -f /tmp/app0/usr/lib/systemd/system/app0.service

grep -q -F "LogExtraFields=PORTABLE=app0" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_ROOT=rootdir" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION=app0" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION_NAME_AND_VERSION=app" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION=app1" /run/systemd/system.attached/app0.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION_NAME_AND_VERSION=app_1" /run/systemd/system.attached/app0.service.d/20-portable.conf

grep -q -F "LogExtraFields=PORTABLE=app1" /run/systemd/system.attached/app1.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_ROOT=rootdir" /run/systemd/system.attached/app1.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION=app0" /run/systemd/system.attached/app1.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION_NAME_AND_VERSION=app" /run/systemd/system.attached/app1.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION=app1" /run/systemd/system.attached/app1.service.d/20-portable.conf
grep -q -F "LogExtraFields=PORTABLE_EXTENSION_NAME_AND_VERSION=app_1" /run/systemd/system.attached/app1.service.d/20-portable.conf

portablectl detach --clean --now --runtime --extension /tmp/app0 --extension /tmp/app1 /tmp/rootdir app0 app1

# Ensure --clean remove state and other directories belonging to the portable image being detached
test ! -d /var/lib/app0
test ! -d /run/app0

# Ensure that mixed mode copies the images and units (client-owned) but symlinks the profile (OS owned)
portablectl "${ARGS[@]}" attach --copy=mixed --runtime --extension /tmp/app0 --extension /tmp/app1 /tmp/rootdir app0 app1
test -d /run/portables/app0
test -d /run/portables/app1
test -d /run/portables/rootdir
test -f /run/systemd/system.attached/app0.service
test -f /run/systemd/system.attached/app1.service
test -L /run/systemd/system.attached/app0.service.d/10-profile.conf
test -L /run/systemd/system.attached/app1.service.d/10-profile.conf
portablectl detach --runtime --extension /tmp/app0 --extension /tmp/app1 /tmp/rootdir app0 app1

# Attempt to disable the app unit during detaching. Requires --copy=symlink to reproduce.
# Provides coverage for https://github.com/systemd/systemd/issues/23481
portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime /tmp/rootdir minimal-app0
portablectl detach --now --runtime --enable /tmp/rootdir minimal-app0
# attach and detach again to check if all drop-in configs are removed even if the main unit files are removed
portablectl "${ARGS[@]}" attach --copy=symlink --now --runtime /tmp/rootdir minimal-app0
portablectl detach --now --runtime --enable /tmp/rootdir minimal-app0

# The wrong file should be ignored, given the right one has the xattr set
trap 'rm -rf /var/cache/wrongext' EXIT
mkdir -p /var/cache/wrongext/usr/lib/extension-release.d /var/cache/wrongext/usr/lib/systemd/system/
echo "[Service]" > /var/cache/wrongext/usr/lib/systemd/system/app0.service
touch /var/cache/wrongext/usr/lib/extension-release.d/extension-release.wrongext_somethingwrong.txt
cp /tmp/rootdir/usr/lib/os-release /var/cache/wrongext/usr/lib/extension-release.d/extension-release.app0
setfattr -n user.extension-release.strict -v "false" /var/cache/wrongext/usr/lib/extension-release.d/extension-release.app0
portablectl "${ARGS[@]}" attach --runtime --extension /var/cache/wrongext /tmp/rootdir app0
status="$(portablectl is-attached --extension wrongext rootdir)"
[[ "${status}" == "attached-runtime" ]]
portablectl detach --runtime --extension /var/cache/wrongext /tmp/rootdir app0

umount /tmp/rootdir
umount /tmp/app0
umount /tmp/app1

# Lack of ID field in os-release should be rejected, but it caused a crash in the past instead
mkdir -p /tmp/emptyroot/usr/lib
mkdir -p /tmp/emptyext/usr/lib/extension-release.d
touch /tmp/emptyroot/usr/lib/os-release
touch /tmp/emptyext/usr/lib/extension-release.d/extension-release.emptyext

# Remote peer disconnected -> portabled crashed
res="$(! portablectl attach --extension /tmp/emptyext /tmp/emptyroot 2> >(grep "Remote peer disconnected"))"
test -z "${res}"

touch /testok
