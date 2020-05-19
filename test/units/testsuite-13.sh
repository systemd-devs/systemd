#!/usr/bin/env bash
set -x
set -e
set -u
set -o pipefail

export SYSTEMD_LOG_LEVEL=debug

# check cgroup-v2
is_v2_supported=no
mkdir -p /tmp/cgroup2
if mount -t cgroup2 cgroup2 /tmp/cgroup2; then
    is_v2_supported=yes
    umount /tmp/cgroup2
fi
rmdir /tmp/cgroup2

# check cgroup namespaces
is_cgns_supported=no
if [[ -f /proc/1/ns/cgroup ]]; then
    is_cgns_supported=yes
fi

is_user_ns_supported=no
# On some systems (e.g. CentOS 7) the default limit for user namespaces
# is set to 0, which causes the following unshare syscall to fail, even
# with enabled user namespaces support. By setting this value explicitly
# we can ensure the user namespaces support to be detected correctly.
sysctl -w user.max_user_namespaces=10000
if unshare -U sh -c :; then
    is_user_ns_supported=yes
fi

function check_bind_tmp_path {
    # https://github.com/systemd/systemd/issues/4789
    local _root="/var/lib/machines/testsuite-13.bind-tmp-path"
    rm -rf "$_root"
    /usr/lib/systemd/tests/testdata/create-busybox-container "$_root"
    >/tmp/bind
    systemd-nspawn --register=no -D "$_root" --bind=/tmp/bind /bin/sh -c 'test -e /tmp/bind'
}

function check_norbind {
    # https://github.com/systemd/systemd/issues/13170
    local _root="/var/lib/machines/testsuite-13.norbind-path"
    rm -rf "$_root"
    mkdir -p /tmp/binddir/subdir
    echo -n "outer" > /tmp/binddir/subdir/file
    mount -t tmpfs tmpfs /tmp/binddir/subdir
    echo -n "inner" > /tmp/binddir/subdir/file
    /usr/lib/systemd/tests/testdata/create-busybox-container "$_root"
    systemd-nspawn --register=no -D "$_root" --bind=/tmp/binddir:/mnt:norbind /bin/sh -c 'CONTENT=$(cat /mnt/subdir/file); if [[ $CONTENT != "outer" ]]; then echo "*** unexpected content: $CONTENT"; return 1; fi'
}

function check_notification_socket {
    # https://github.com/systemd/systemd/issues/4944
    local _cmd='echo a | $(busybox which nc) -U -u -w 1 /run/systemd/nspawn/notify'
    # /testsuite-13.nc-container is prepared by test.sh
    systemd-nspawn --register=no -D /testsuite-13.nc-container /bin/sh -x -c "$_cmd"
    systemd-nspawn --register=no -D /testsuite-13.nc-container -U /bin/sh -x -c "$_cmd"
}

function run {
    if [[ "$1" = "yes" && "$is_v2_supported" = "no" ]]; then
        printf "Unified cgroup hierarchy is not supported. Skipping.\n" >&2
        return 0
    fi
    if [[ "$2" = "yes" && "$is_cgns_supported" = "no" ]];  then
        printf "CGroup namespaces are not supported. Skipping.\n" >&2
        return 0
    fi

    local _root="/var/lib/machines/testsuite-13.unified-$1-cgns-$2-api-vfs-writable-$3"
    rm -rf "$_root"
    /usr/lib/systemd/tests/testdata/create-busybox-container "$_root"
    SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" -b
    SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" --private-network -b

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" -U -b; then
        [[ "$is_user_ns_supported" = "yes" && "$3" = "network" ]] && return 1
    else
        [[ "$is_user_ns_supported" = "no" && "$3" = "network" ]] && return 1
    fi

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" --private-network -U -b; then
        [[ "$is_user_ns_supported" = "yes" && "$3" = "yes" ]] && return 1
    else
        [[ "$is_user_ns_supported" = "no" && "$3" = "yes" ]] && return 1
    fi

    local _netns_opt="--network-namespace-path=/proc/self/ns/net"

    # --network-namespace-path and network-related options cannot be used together
    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --network-interface=lo -b; then
        return 1
    fi

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --network-macvlan=lo -b; then
        return 1
    fi

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --network-ipvlan=lo -b; then
        return 1
    fi

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --network-veth -b; then
        return 1
    fi

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --network-veth-extra=lo -b; then
        return 1
    fi

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --network-bridge=lo -b; then
        return 1
    fi

    if SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --network-zone=zone -b; then
        return 1
    fi

    # allow combination of --network-namespace-path and --private-network
    if ! SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" --private-network -b; then
        return 1
    fi

    # test --network-namespace-path works with a network namespace created by "ip netns"
    ip netns add nspawn_test
    _netns_opt="--network-namespace-path=/run/netns/nspawn_test"
    SYSTEMD_NSPAWN_UNIFIED_HIERARCHY="$1" SYSTEMD_NSPAWN_USE_CGNS="$2" SYSTEMD_NSPAWN_API_VFS_WRITABLE="$3" systemd-nspawn --register=no -D "$_root" "$_netns_opt" /bin/ip a | grep -v -E '^1: lo.*UP'
    local r=$?
    ip netns del nspawn_test

    if [ $r -ne 0 ]; then
        return 1
    fi

    return 0
}

check_bind_tmp_path

check_norbind

check_notification_socket

for api_vfs_writable in yes no network; do
    run no no $api_vfs_writable
    run yes no $api_vfs_writable
    run no yes $api_vfs_writable
    run yes yes $api_vfs_writable
done

touch /testok
