#!/usr/bin/env bash
# vi: ts=4 sw=4 tw=0 et:
#
# TODO:
#   * iSCSI
#   * LVM over iSCSI (?)
#   * SW raid (mdadm)
#   * MD (mdadm) -> DM-CRYPT -> LVM
set -e

TEST_DESCRIPTION="systemd-udev storage tests"
IMAGE_NAME="default"
TEST_NO_NSPAWN=1
# Save only journals of failing test cases by default (to conserve space)
TEST_SAVE_JOURNAL="${TEST_SAVE_JOURNAL:-fail}"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-600}"

# shellcheck source=test/test-functions
. "${TEST_BASE_DIR:?}/test-functions"

USER_QEMU_OPTIONS="${QEMU_OPTIONS:-}"
USER_KERNEL_APPEND="${KERNEL_APPEND:-}"

if ! get_bool "$QEMU_KVM"; then
    echo "This test requires KVM, skipping..."
    exit 0
fi

_host_has_feature() {(
    set -e

    case "${1:?}" in
        btrfs)
            modprobe -nv btrfs && command -v mkfs.btrfs && command -v btrfs || return $?
            ;;
        lvm)
            command -v lvm || return $?
            ;;
        multipath)
            command -v multipath && command -v multipathd || return $?
            ;;
        *)
            echo >&2 "ERROR: Unknown feature '$1'"
            # Make this a hard error to distinguish an invalid feature from
            # a missing feature
            exit 1
    esac
)}

test_append_files() {(
    local feature
    # An associative array of requested (but optional) features and their
    # respective "handlers" from test/test-functions
    #
    # Note: we install cryptsetup unconditionally, hence it's not explicitly
    # checked for here
    local -A features=(
        [btrfs]=install_btrfs
        [lvm]=install_lvm
        [multipath]=install_multipath
    )

    instmods "=block" "=md" "=nvme" "=scsi"
    install_dmevent
    image_install lsblk wc wipefs

    # Install the optional features if the host has the respective tooling
    for feature in "${!features[@]}"; do
        if _host_has_feature "$feature"; then
            "${features[$feature]}"
        fi
    done

    generate_module_dependencies

    for i in {0..127}; do
        dd if=/dev/zero of="${TESTDIR:?}/disk$i.img" bs=1M count=1
        echo "device$i" >"${TESTDIR:?}/disk$i.img"
    done
)}

_image_cleanup() {
    mount_initdir
    # Clean up certain "problematic" files which may be left over by failing tests
    : >"${initdir:?}/etc/fstab"
    : >"${initdir:?}/etc/crypttab"
}

test_run_one() {
    local test_id="${1:?}"

    if run_qemu "$test_id"; then
        check_result_qemu || { echo "QEMU test failed"; return 1; }
    fi

    return 0
}

test_run() {
    local test_id="${1:?}"
    local passed=()
    local failed=()
    local skipped=()
    local ec state

    mount_initdir

    if get_bool "${TEST_NO_QEMU:=}" || ! find_qemu_bin; then
        dwarn "can't run QEMU, skipping"
        return 0
    fi

    # Execute each currently defined function starting with "testcase_"
    for testcase in "${TESTCASES[@]}"; do
        _image_cleanup
        echo "------ $testcase: BEGIN ------"
        # Note for my future frustrated self: `fun && xxx` (as wel as ||, if, while,
        # until, etc.) _DISABLES_ the `set -e` behavior in _ALL_ nested function
        # calls made from `fun()`, i.e. the function _CONTINUES_ even when a called
        # command returned non-zero EC. That may unexpectedly hide failing commands
        # if not handled properly. See: bash(1) man page, `set -e` section.
        #
        # So, be careful when adding clean up snippets in the testcase_*() functions -
        # if the `test_run_one()` function isn't the last command, you have propagate
        # the exit code correctly (e.g. `test_run_one() || return $?`, see below).
        ec=0
        "$testcase" "$test_id" || ec=$?
        case $ec in
            0)
                passed+=("$testcase")
                state="PASS"
                ;;
            77)
                skipped+=("$testcase")
                state="SKIP"
                ;;
            *)
                failed+=("$testcase")
                state="FAIL"
        esac
        echo "------ $testcase: END ($state) ------"
    done

    echo "Passed tests: ${#passed[@]}"
    printf "    * %s\n" "${passed[@]}"
    echo "Skipped tests: ${#skipped[@]}"
    printf "    * %s\n" "${skipped[@]}"
    echo "Failed tests: ${#failed[@]}"
    printf "    * %s\n" "${failed[@]}"

    [[ ${#failed[@]} -eq 0 ]] || return 1

    return 0
}

testcase_megasas2_basic() {
    if ! "${QEMU_BIN:?}" -device help | grep 'name "megasas-gen2"'; then
        echo "megasas-gen2 device driver is not available, skipping test..."
        return 77
    fi

    local qemu_opts=(
        "-device megasas-gen2,id=scsi0"
        "-device megasas-gen2,id=scsi1"
        "-device megasas-gen2,id=scsi2"
        "-device megasas-gen2,id=scsi3"
    )

    for i in {0..127}; do
        # Add 128 drives, 32 per bus
        qemu_opts+=(
            "-device scsi-hd,drive=drive$i,bus=scsi$((i / 32)).0,channel=0,scsi-id=$((i % 32)),lun=0"
            "-drive format=raw,cache=unsafe,file=${TESTDIR:?}/disk$i.img,if=none,id=drive$i"
        )
    done

    KERNEL_APPEND="systemd.setenv=TEST_FUNCTION_NAME=${FUNCNAME[0]} ${USER_KERNEL_APPEND:-}"
    QEMU_OPTIONS="${qemu_opts[*]} ${USER_QEMU_OPTIONS:-}"
    test_run_one "${1:?}"
}

testcase_nvme_basic() {
    if ! "${QEMU_BIN:?}" -device help | grep 'name "nvme"'; then
        echo "nvme device driver is not available, skipping test..."
        return 77
    fi

    for i in {0..27}; do
        qemu_opts+=(
            "-device nvme,drive=nvme$i,serial=deadbeef$i,num_queues=8"
            "-drive format=raw,cache=unsafe,file=${TESTDIR:?}/disk$i.img,if=none,id=nvme$i"
        )
    done

    KERNEL_APPEND="systemd.setenv=TEST_FUNCTION_NAME=${FUNCNAME[0]} ${USER_KERNEL_APPEND:-}"
    QEMU_OPTIONS="${qemu_opts[*]} ${USER_QEMU_OPTIONS:-}"
    test_run_one "${1:?}"
}

# Test for issue https://github.com/systemd/systemd/issues/20212
testcase_virtio_scsi_identically_named_partitions() {
    if ! "${QEMU_BIN:?}" -device help | grep 'name "virtio-scsi-pci"'; then
        echo "virtio-scsi-pci device driver is not available, skipping test..."
        return 77
    fi

    # Create 16 disks, with 8 partitions per disk (all identically named)
    # and attach them to a virtio-scsi controller
    local qemu_opts=("-device virtio-scsi-pci,id=scsi0,num_queues=4")
    local diskpath="${TESTDIR:?}/namedpart0.img"
    local lodev

    dd if=/dev/zero of="$diskpath" bs=1M count=18
    lodev="$(losetup --show -f -P "$diskpath")"
    sfdisk "${lodev:?}" <<EOF
label: gpt

name="Hello world", size=2M
name="Hello world", size=2M
name="Hello world", size=2M
name="Hello world", size=2M
name="Hello world", size=2M
name="Hello world", size=2M
name="Hello world", size=2M
name="Hello world", size=2M
EOF
    losetup -d "$lodev"

    for i in {0..15}; do
        diskpath="${TESTDIR:?}/namedpart$i.img"
        if [[ $i -gt 0 ]]; then
            cp -uv "${TESTDIR:?}/namedpart0.img" "$diskpath"
        fi

        qemu_opts+=(
            "-device scsi-hd,drive=drive$i,bus=scsi0.0,channel=0,scsi-id=0,lun=$i"
            "-drive format=raw,cache=unsafe,file=$diskpath,if=none,id=drive$i"
        )
    done

    KERNEL_APPEND="systemd.setenv=TEST_FUNCTION_NAME=${FUNCNAME[0]} ${USER_KERNEL_APPEND:-}"
    # Limit the number of VCPUs and set a timeout to make sure we trigger the issue
    QEMU_OPTIONS="${qemu_opts[*]} ${USER_QEMU_OPTIONS:-}"
    QEMU_SMP=1 QEMU_TIMEOUT=60 test_run_one "${1:?}" || return $?

    rm -f "${TESTDIR:?}"/namedpart*.img
}

testcase_multipath_basic_failover() {
    if ! _host_has_feature "multipath"; then
        echo "Missing multipath tools, skipping the test..."
        return 77
    fi

    local qemu_opts=("-device virtio-scsi-pci,id=scsi")
    local partdisk="${TESTDIR:?}/multipathpartitioned.img"
    local image lodev nback ndisk wwn

    dd if=/dev/zero of="$partdisk" bs=1M count=16
    lodev="$(losetup --show -f -P "$partdisk")"
    sfdisk "${lodev:?}" <<EOF
label: gpt

name="first_partition", size=5M
uuid="deadbeef-dead-dead-beef-000000000000", name="failover_part", size=5M
EOF
    udevadm settle
    mkfs.ext4 -U "deadbeef-dead-dead-beef-111111111111" -L "failover_vol" "${lodev}p2"
    losetup -d "$lodev"

    # Add 64 multipath devices, each backed by 4 paths
    for ndisk in {0..63}; do
        wwn="0xDEADDEADBEEF$(printf "%.4d" "$ndisk")"
        # Use a partitioned disk for the first device to test failover
        [[ $ndisk -eq 0 ]] && image="$partdisk" || image="${TESTDIR:?}/disk$ndisk.img"

        for nback in {0..3}; do
            qemu_opts+=(
                "-device scsi-hd,drive=drive${ndisk}x${nback},serial=MPIO$ndisk,wwn=$wwn"
                "-drive format=raw,cache=unsafe,file=$image,file.locking=off,if=none,id=drive${ndisk}x${nback}"
            )
        done
    done

    KERNEL_APPEND="systemd.setenv=TEST_FUNCTION_NAME=${FUNCNAME[0]} ${USER_KERNEL_APPEND:-}"
    QEMU_OPTIONS="${qemu_opts[*]} ${USER_QEMU_OPTIONS:-}"
    test_run_one "${1:?}" || return $?

    rm -f "$partdisk"
}

# Test case for issue https://github.com/systemd/systemd/issues/19946
testcase_simultaneous_events() {
    local qemu_opts=("-device virtio-scsi-pci,id=scsi")
    local partdisk="${TESTDIR:?}/simultaneousevents.img"

    dd if=/dev/zero of="$partdisk" bs=1M count=110
    qemu_opts+=(
        "-device scsi-hd,drive=drive1,serial=deadbeeftest"
        "-drive format=raw,cache=unsafe,file=$partdisk,if=none,id=drive1"
    )

    KERNEL_APPEND="systemd.setenv=TEST_FUNCTION_NAME=${FUNCNAME[0]} ${USER_KERNEL_APPEND:-}"
    QEMU_OPTIONS="${qemu_opts[*]} ${USER_QEMU_OPTIONS:-}"
    test_run_one "${1:?}" || return $?

    rm -f "$partdisk"
}

testcase_lvm_basic() {
    if ! _host_has_feature "lvm"; then
        echo "Missing lvm tools, skipping the test..."
        return 77
    fi

    local qemu_opts=("-device ahci,id=ahci0")
    local diskpath

    # Attach 4 SATA disks to the VM (and set their model and serial fields
    # to something predictable, so we can refer to them later)
    for i in {0..3}; do
        diskpath="${TESTDIR:?}/lvmbasic${i}.img"
        dd if=/dev/zero of="$diskpath" bs=1M count=32
        qemu_opts+=(
            "-device ide-hd,bus=ahci0.$i,drive=drive$i,model=foobar,serial=deadbeeflvm$i"
            "-drive format=raw,cache=unsafe,file=$diskpath,if=none,id=drive$i"
        )
    done

    KERNEL_APPEND="systemd.setenv=TEST_FUNCTION_NAME=${FUNCNAME[0]} ${USER_KERNEL_APPEND:-}"
    QEMU_OPTIONS="${qemu_opts[*]} ${USER_QEMU_OPTIONS:-}"
    test_run_one "${1:?}" || return $?

    rm -f "${TESTDIR:?}"/lvmbasic*.img
}

testcase_btrfs_basic() {
    if ! _host_has_feature "btrfs"; then
        echo "Missing btrfs tools/modules, skipping the test..."
        return 77
    fi

    local qemu_opts=("-device ahci,id=ahci0")
    local diskpath i size

    for i in {0..3}; do
        diskpath="${TESTDIR:?}/btrfsbasic${i}.img"
        # Make the first disk larger for multi-partition tests
        [[ $i -eq 0 ]] && size=350 || size=128

        dd if=/dev/zero of="$diskpath" bs=1M count="$size"
        qemu_opts+=(
            "-device ide-hd,bus=ahci0.$i,drive=drive$i,model=foobar,serial=deadbeefbtrfs$i"
            "-drive format=raw,cache=unsafe,file=$diskpath,if=none,id=drive$i"
        )
    done

    KERNEL_APPEND="systemd.setenv=TEST_FUNCTION_NAME=${FUNCNAME[0]} ${USER_KERNEL_APPEND:-}"
    QEMU_OPTIONS="${qemu_opts[*]} ${USER_QEMU_OPTIONS:-}"
    test_run_one "${1:?}" || return $?

    rm -f "${TESTDIR:?}"/btrfsbasic*.img
}

# Allow overriding which tests should be run from the "outside", useful for manual
# testing (make -C test/... TESTCASES="testcase1 testcase2")
if [[ -v "TESTCASES" && -n "$TESTCASES" ]]; then
    read -ra TESTCASES <<< "$TESTCASES"
else
    # This must run after all functions were defined, otherwise `declare -F` won't
    # see them
    mapfile -t TESTCASES < <(declare -F | awk '$3 ~ /^testcase_/ {print $3;}')
fi

do_test "$@"
