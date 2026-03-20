#!/bin/bash
# Stronger integration coverage for btrfs-tar on a real btrfs filesystem.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfs
check_prereq btrfs-tar
check_global_prereq tar
check_global_prereq cmp
check_global_prereq readlink

setup_root_helper
prepare_test_dev

tmp=$(_mktemp_dir btrfs-tar-features)
archive="$tmp/features.tar.gz"
archive_snaps="$tmp/features-snaps.tar.gz"
extract_dir="$tmp/extract"
extract_dir_snaps="$tmp/extract-snaps"
expected_sparse="$tmp/expected-sparse.bin"
expected_plain="$tmp/expected-plain.txt"
expected_long="$tmp/expected-long.txt"
expected_tail="$tmp/expected-tail.bin"

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

make_long_name()
{
	local prefix="$1"
	local count="$2"
	local out="$prefix"
	local i

	for ((i = 0; i < count; i++)); do
		out="${out}x"
	done
	printf "%s" "$out"
}

longdir=$(make_long_name longdir-110 110)
longfile=$(make_long_name longfile-110 110).txt

run_check_mkfs_test_dev
run_check_mount_test_dev -o subvolid=5 -o compress=zlib

cd "$TEST_MNT" || _fail "Cannot cd into TEST_MNT $TEST_MNT"

run_check mkdir data
run_check sh -c "printf 'plain-data-line-1\nplain-data-line-2\n' > data/plain.txt"
run_check sh -c "printf 'plain-data-line-1\nplain-data-line-2\n' > '$expected_plain'"

run_check mkdir -p "data/$longdir"
run_check sh -c "printf 'very-long-path\n' > 'data/$longdir/$longfile'"
run_check sh -c "printf 'very-long-path\n' > '$expected_long'"

run_check truncate -s 1048576 data/sparse.bin
run_check dd if=/dev/zero of=data/sparse.bin bs=1 count=0 seek=0
run_check sh -c "printf 'BEGIN' | dd of=data/sparse.bin bs=1 seek=0 conv=notrunc status=none"
run_check sh -c "printf 'MIDDLE' | dd of=data/sparse.bin bs=1 seek=524288 conv=notrunc status=none"
run_check sh -c "printf 'END' | dd of=data/sparse.bin bs=1 seek=1048573 conv=notrunc status=none"

run_check truncate -s 1048576 "$expected_sparse"
run_check sh -c "printf 'BEGIN' | dd of='$expected_sparse' bs=1 seek=0 conv=notrunc status=none"
run_check sh -c "printf 'MIDDLE' | dd of='$expected_sparse' bs=1 seek=524288 conv=notrunc status=none"
run_check sh -c "printf 'END' | dd of='$expected_sparse' bs=1 seek=1048573 conv=notrunc status=none"

run_check sh -c "python3 - <<'PY' > data/tail-extent.bin
import sys
sys.stdout.buffer.write((b'0123456789abcdef' * 631) + b'012')
PY"
run_check cp data/tail-extent.bin "$expected_tail"

run_check $SUDO_HELPER "$TOP/btrfs" subvolume create subvol-src
run_check sh -c "printf 'subvol-live\n' > subvol-src/live.txt"
run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot subvol-src snap-keep
run_check sh -c "printf 'snapshot-live-view\n' > snap-keep/snap.txt"
run_check $SUDO_HELPER "$TOP/btrfs" property set -ts snap-keep ro true

cd /
run_check_umount_test_dev

run_check "$TOP/btrfs-tar" "$TEST_DEV" "$archive"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir"
run_check tar -xzf "$archive" -C "$extract_dir"

if ! cmp -s "$extract_dir/data/plain.txt" "$expected_plain"; then
	_fail "plain file content mismatch"
fi

if ! [ -f "$extract_dir/data/$longdir/$longfile" ]; then
	_fail "long path entry missing from archive"
fi

if ! cmp -s "$extract_dir/data/$longdir/$longfile" "$expected_long"; then
	_fail "long path file content mismatch"
fi

if ! cmp -s "$extract_dir/data/sparse.bin" "$expected_sparse"; then
	_fail "sparse file content mismatch after archive roundtrip"
fi

if ! cmp -s "$extract_dir/data/tail-extent.bin" "$expected_tail"; then
	_fail "tail extent file content mismatch after archive roundtrip"
fi

size=$(stat -c %s "$extract_dir/data/sparse.bin")
if [ "$size" != "1048576" ]; then
	_fail "sparse file size mismatch: $size"
fi

if [ -e "$extract_dir/snap-keep" ]; then
	_fail "snapshot unexpectedly exported without -s"
fi

if ! [ -f "$extract_dir/subvol-src/live.txt" ]; then
	_fail "regular subvolume content missing from archive"
fi

run_check "$TOP/btrfs-tar" -s "$TEST_DEV" "$archive_snaps"
assert_no_extent_buffer_leak

run_check mkdir -p "$extract_dir_snaps"
run_check tar -xzf "$archive_snaps" -C "$extract_dir_snaps"

if ! [ -f "$extract_dir_snaps/snap-keep/live.txt" ]; then
	_fail "snapshot content missing when exporting with -s"
fi

if ! [ -f "$extract_dir_snaps/snap-keep/snap.txt" ]; then
	_fail "snapshot-specific file missing when exporting with -s"
fi
