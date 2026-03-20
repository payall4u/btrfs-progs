#!/bin/bash
# Verify that btrfs-tar can export an explicitly selected subvolume id.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfs-tar
check_global_prereq tar

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir btrfs-tar-subvolid)
archive="$tmp/subvolid.tar.gz"
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

run_check sh -c "printf 'top-level\n' > root.txt"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create alpha
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create beta
run_check sh -c "printf 'alpha-data\n' > alpha/selected.txt"
run_check sh -c "printf 'beta-data\n' > beta/selected.txt"
run_check sh -c "printf 'alpha-only\n' > alpha/alpha-only.txt"
run_check sh -c "printf 'beta-only\n' > beta/beta-only.txt"

beta_id=$(run_check_stdout "$TOP/btrfs" inspect-internal rootid beta)

cd /
run_check_umount_test_dev

run_check "$TOP/btrfs-tar" --subvolid "$beta_id" "$TEST_DEV" "$archive"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir"
run_check tar -xzf "$archive" -C "$extract_dir"

if ! [ -f "$extract_dir/selected.txt" ]; then
	_fail "selected subvolume file missing"
fi

if ! grep -qx "beta-data" "$extract_dir/selected.txt"; then
	_fail "selected.txt does not match chosen subvolume"
fi

if ! [ -f "$extract_dir/beta-only.txt" ]; then
	_fail "beta-only file missing"
fi

if [ -e "$extract_dir/alpha-only.txt" ]; then
	_fail "alpha-only file unexpectedly present"
fi

if [ -e "$extract_dir/root.txt" ]; then
	_fail "top-level file unexpectedly present when exporting by subvolid"
fi
