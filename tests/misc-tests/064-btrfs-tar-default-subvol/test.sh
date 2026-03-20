#!/bin/bash
# Verify that btrfs-tar exports the default subvolume, not the top-level root.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfs-tar
check_global_prereq tar

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir btrfs-tar-default-subvol)
archive="$tmp/archive.tar.gz"
extract_dir="$tmp/extract"
archive2="$tmp/archive-top-level.tar.gz"
extract_dir2="$tmp/extract-top-level"

cleanup()
{
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

run_check sh -c "printf 'top-level\n' > whoami.txt"
run_check sh -c "printf 'top-only\n' > top-only.txt"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create exported
run_check sh -c "printf 'default-subvol\n' > exported/whoami.txt"
run_check sh -c "printf 'default-only\n' > exported/only-in-default.txt"
run_check mkdir -p exported/dir
run_check sh -c "printf 'nested-file\n' > exported/dir/nested.txt"
run_check ln -s dir/nested.txt exported/nested-link

subvol_id=$(run_check_stdout "$TOP/btrfs" inspect-internal rootid exported)
run_check $SUDO_HELPER "$TOP/btrfs" subvolume set-default "$subvol_id" .

cd /
run_check_umount_test_dev

run_check "$TOP/btrfs-tar" "$TEST_DEV" "$archive"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir"
run_check tar -xzf "$archive" -C "$extract_dir"

if [ -e "$extract_dir/top-only.txt" ]; then
	_fail "top-level file unexpectedly exported"
fi

if ! [ -f "$extract_dir/whoami.txt" ]; then
	_fail "whoami.txt missing from extracted archive"
fi

if ! grep -qx "default-subvol" "$extract_dir/whoami.txt"; then
	_fail "whoami.txt content does not match default subvolume"
fi

if ! [ -f "$extract_dir/only-in-default.txt" ]; then
	_fail "default subvolume file missing from extracted archive"
fi

if ! [ -f "$extract_dir/dir/nested.txt" ]; then
	_fail "nested file missing from extracted archive"
fi

target=$(readlink "$extract_dir/nested-link") || _fail "nested-link missing"
if [ "$target" != "dir/nested.txt" ]; then
	_fail "unexpected symlink target: $target"
fi

run_check_mkfs_test_dev
run_check_mount_test_dev -o subvolid=5

cd "$TEST_MNT" || _fail "Cannot cd into TEST_MNT $TEST_MNT"
run_check sh -c "printf 'top-level-fallback\n' > whoami.txt"
run_check sh -c "printf 'fallback-only\n' > top-only.txt"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume create hidden
run_check sh -c "printf 'hidden-subvol\n' > hidden/whoami.txt"

cd /
run_check_umount_test_dev

run_check "$TOP/btrfs-tar" "$TEST_DEV" "$archive2"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir2"
run_check tar -xzf "$archive2" -C "$extract_dir2"

if ! [ -f "$extract_dir2/top-only.txt" ]; then
	_fail "top-level fallback file missing from extracted archive"
fi

if ! grep -qx "top-level-fallback" "$extract_dir2/whoami.txt"; then
	_fail "fallback whoami.txt content does not match top-level subvolume"
fi

if [ -e "$extract_dir2/only-in-default.txt" ]; then
	_fail "file from non-default subvolume unexpectedly exported in fallback case"
fi
