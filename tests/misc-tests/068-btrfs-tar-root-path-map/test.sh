#!/bin/bash
# Verify that btrfs-tar can archive from a subtree root and remap paths,
# including sources outside the root path.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfs-tar
check_global_prereq tar

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir btrfs-tar-root-path-map)
archive="$tmp/root-path-map.tar.gz"
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

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create workspace
run_check mkdir -p workspace/diff/etc
run_check mkdir -p workspace/diff/var/lib/docker
run_check mkdir -p workspace/diff/workspace
run_check mkdir -p workspace/docker
run_check mkdir -p workspace/other

run_check sh -c "printf 'etc-data\n' > workspace/diff/etc/issue"
run_check sh -c "printf 'base-docker\n' > workspace/diff/var/lib/docker/layer.txt"
run_check sh -c "printf 'mapped-docker\n' > workspace/docker/layer.txt"
run_check sh -c "printf 'mapped-extra\n' > workspace/docker/extra.txt"
run_check sh -c "printf 'outside-root\n' > workspace/other/ignore.txt"

cd /
run_check_umount_test_dev

run_check "$TOP/btrfs-tar" --subvol workspace --root-path diff \
	--path-map docker:var/lib/docker \
	"$TEST_DEV" "$archive"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir"
run_check tar -xzf "$archive" -C "$extract_dir"

if ! [ -f "$extract_dir/etc/issue" ]; then
	_fail "root-path content missing from archive"
fi

if ! grep -qx "etc-data" "$extract_dir/etc/issue"; then
	_fail "root-path file content mismatch"
fi

if [ -e "$extract_dir/docker/layer.txt" ]; then
	_fail "path-map source outside root path unexpectedly archived at source path"
fi

if ! [ -f "$extract_dir/var/lib/docker/layer.txt" ]; then
	_fail "mapped destination file missing"
fi

if ! grep -qx "mapped-docker" "$extract_dir/var/lib/docker/layer.txt"; then
	_fail "mapped destination did not override target path"
fi

if ! [ -f "$extract_dir/var/lib/docker/extra.txt" ]; then
	_fail "mapped extra file missing from destination"
fi

if [ -e "$extract_dir/other/ignore.txt" ]; then
	_fail "content outside root path unexpectedly archived"
fi
