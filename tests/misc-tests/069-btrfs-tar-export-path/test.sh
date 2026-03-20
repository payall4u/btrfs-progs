#!/bin/bash
# Verify that btrfs-tar can export only selected paths to chosen archive paths.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfs-tar
check_global_prereq tar

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir btrfs-tar-export-path)
archive="$tmp/export-path.tar.gz"
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
run_check mkdir -p workspace/docker/containers
run_check mkdir -p workspace/etc
run_check sh -c "printf 'docker-data\n' > workspace/docker/containers/config.json"
run_check sh -c "printf 'top-etc\n' > workspace/etc/issue"

cd /
run_check_umount_test_dev

run_check "$TOP/btrfs-tar" --subvol workspace \
	--export-path docker:var/lib/docker \
	"$TEST_DEV" "$archive"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir"
run_check tar -xzf "$archive" -C "$extract_dir"

if ! [ -f "$extract_dir/var/lib/docker/containers/config.json" ]; then
	_fail "exported destination file missing"
fi

if ! grep -qx "docker-data" "$extract_dir/var/lib/docker/containers/config.json"; then
	_fail "exported destination content mismatch"
fi

if [ -e "$extract_dir/docker/containers/config.json" ]; then
	_fail "source path unexpectedly kept in archive"
fi

if [ -e "$extract_dir/etc/issue" ]; then
	_fail "content outside export path unexpectedly archived"
fi
