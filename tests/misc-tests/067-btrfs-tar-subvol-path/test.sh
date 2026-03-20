#!/bin/bash
# Verify that btrfs-tar can export a subvolume selected by path.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfs-tar
check_global_prereq tar

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir btrfs-tar-subvol-path)
archive="$tmp/subvol-path.tar.gz"
extract_dir="$tmp/extract"

cleanup()
{
	cd / || true
	if grep -q "[[:space:]]$TEST_MNT[[:space:]]" /proc/self/mounts; then
		run_mayfail $SUDO_HELPER umount "$TEST_MNT"
	fi
	rm -rf -- "$tmp"
}

trap cleanup EXIT

assert_no_extent_buffer_leak()
{
	if grep -q "extent buffer leak" "$RESULTS"; then
		_fail "btrfs-tar reported extent buffer leak"
	fi
}

run_check_mkfs_test_dev
run_check_mount_test_dev -o subvolid=5

cd "$TEST_MNT" || _fail "Cannot cd into TEST_MNT $TEST_MNT"

run_check mkdir -p outer/dir
run_check sh -c "printf 'top-level\n' > outer/dir/top.txt"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create outer/parent
run_check sh -c "printf 'parent-data\n' > outer/parent/parent.txt"

run_check mkdir -p outer/parent/nested
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create outer/parent/nested/child
run_check sh -c "printf 'child-data\n' > outer/parent/nested/child/selected.txt"
run_check sh -c "printf 'child-only\n' > outer/parent/nested/child/only.txt"

cd /
run_check_umount_test_dev

run_check "$TOP/btrfs-tar" --subvol outer/parent/nested/child "$TEST_DEV" "$archive"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir"
run_check tar -xzf "$archive" -C "$extract_dir"

if ! [ -f "$extract_dir/selected.txt" ]; then
	_fail "selected file missing for subvolume chosen by path"
fi

if ! grep -qx "child-data" "$extract_dir/selected.txt"; then
	_fail "selected file content mismatch for subvolume chosen by path"
fi

if ! [ -f "$extract_dir/only.txt" ]; then
	_fail "subvolume-only file missing for subvolume chosen by path"
fi

if [ -e "$extract_dir/parent.txt" ]; then
	_fail "parent subvolume file unexpectedly present"
fi

if [ -e "$extract_dir/top.txt" ]; then
	_fail "top-level file unexpectedly present"
fi
