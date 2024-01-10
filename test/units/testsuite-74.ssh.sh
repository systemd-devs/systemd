#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -eux
set -o pipefail

if ! command -v ssh &> /dev/null || ! command -v sshd &> /dev/null ; then
    echo "ssh/sshd not found, skipping test." >&2
    exit 0
fi

systemctl -q is-active sshd-unix-local.socket

if test -e /dev/vsock ; then
    systemctl -q is-active sshd-vsock.socket
fi

if test -d /run/host/unix-export ; then
    systemctl -q is-active sshd-unix-export.socket
fi

ROOTID=$(mktemp -u)

removesshid() {
    rm -f "$ROOTID" "$ROOTID".pub
}

ssh-keygen -N '' -C '' -t rsa -f "$ROOTID"

mkdir -p 0700 /root/.ssh
cat "$ROOTID".pub >> /root/.ssh/authorized_keys

mkdir -p /etc/ssh
test -f /etc/ssh/ssh_host_rsa_key || ssh-keygen -t rsa -C '' -N '' -f /etc/ssh/ssh_host_rsa_key
echo "PermitRootLogin yes" >> /etc/ssh/sshd_config

test -f /etc/ssh/ssh_config || echo 'Include /etc/ssh/ssh_config.d/*.conf' > /etc/ssh/ssh_config

# ssh wants this dir around, but distros cannot agree on a common name for it, let's just create all that are aware of distros use
mkdir -p /usr/share/empty.sshd /var/empty /var/empty/sshd

ssh -o StrictHostKeyChecking=no -v -i $ROOTID .host cat /etc/machine-id | cmp - /etc/machine-id
ssh -o StrictHostKeyChecking=no -v -i $ROOTID unix/run/ssh-unix-local/socket cat /etc/machine-id | cmp - /etc/machine-id

modprobe vsock_loopback ||:
if test -e /dev/vsock -a -d /sys/module/vsock_loopback ; then
    ssh -o StrictHostKeyChecking=no -v -i $ROOTID vsock/1 cat /etc/machine-id | cmp - /etc/machine-id
fi
