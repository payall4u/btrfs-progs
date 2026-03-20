# btrfs-tar

`btrfs-tar` converts an unmounted Btrfs filesystem image or block device to a
`tar.gz` archive without mounting the filesystem.

It is intended for offline extraction-style workflows where the source is a raw
Btrfs device, image, snapshot disk, or NBD export and the result needs to be a
portable tar archive.

## What it does

`btrfs-tar` opens the Btrfs trees directly and walks metadata from userspace.
It reads inode items, directory indexes, symlink targets, and file extents, and
then emits a gzip-compressed POSIX ustar archive.

At a high level the conversion is:

1. open the Btrfs filesystem read-only from the device
2. choose a subvolume to export
3. optionally resolve an archive root inside that subvolume
4. walk directories and emit tar headers
5. read file extents from the Btrfs trees and write file data in tar order
6. optionally replay extra path mappings into alternate archive locations
7. finish the archive with the tar end markers

This means the tool does **not** depend on the kernel mount view for the main
conversion path and can work directly on an offline filesystem.

## Current archive model

The tool currently writes:

- regular files
- directories
- symbolic links

Sparse regions are preserved as zero-filled ranges in the tar stream.

By default nested snapshots/subvolumes encountered during directory traversal
are skipped. Use `--snapshots` if they should also be traversed.

## Subvolume selection

If no explicit subvolume is given, `btrfs-tar` exports:

1. the filesystem default subvolume, if one is configured
2. otherwise the top-level subvolume (`id 5`)

You can override that in two ways:

- `--subvolid <id>`: export a specific subvolume id
- `--subvol <path>`: export the subvolume at a top-level path

`--subvol <path>` is resolved from the top-level Btrfs tree, not from the
current default subvolume.

## Archive root rebasing

`--root-path <path>` changes which directory inside the selected subvolume is
used as the archive root.

For example, if the selected subvolume contains:

- `diff/etc/...`
- `diff/var/...`
- `docker/...`

then:

```bash
btrfs-tar --subvol workspace --root-path diff /dev/nbd100 rwlayer.tar.gz
```

stores `diff/etc/...` as `etc/...` in the archive, and `diff/var/...` as
`var/...`.

In other words, `--root-path` strips the chosen prefix from archive paths.

## Exporting only selected paths

`--export-path <src:dst>` archives only the selected path from the chosen
subvolume and writes it at the requested archive destination.

- `src` is resolved from the **selected subvolume root**
- `dst` is the path written into the tar archive
- if any `--export-path` is present, the normal subvolume traversal is skipped
- multiple `--export-path` options are allowed
- `--export-path` is mutually exclusive with `--root-path` and `--path-map`

Example:

```bash
btrfs-tar \
  --subvol workspace \
  --export-path docker:var/lib/docker \
  /dev/nbd100 workspace.tar.gz
```

This archives only `workspace/docker/*`, and stores it as
`var/lib/docker/*` in the tar stream.

## Path remapping / overlaying

`--path-map <src:dst>` replays an extra source subtree from the selected
subvolume at a different archive destination.

- `src` is resolved from the **selected subvolume root**
- `dst` is the path written into the tar archive
- the mapping is written **after** the main archive tree, so it can override
  earlier archive entries at extraction time
- `src` does **not** need to live under `--root-path`
- multiple `--path-map` options are allowed

Example:

```bash
btrfs-tar \
  --subvol workspace \
  --root-path diff \
  --path-map docker:var/lib/docker \
  /dev/nbd100 rwlayer.tar.gz
```

This archives:

- `workspace/diff/*` as the main root
- then replays `workspace/docker/*` into `var/lib/docker/*`

This is useful for container layer export scenarios where the on-disk Btrfs
layout does not match the desired final tar layout.

## Container rwlayer example

A common case is a container rwlayer stored inside a Btrfs subvolume where:

- `diff/` is the rwlayer root
- some runtime state is stored elsewhere in the same subvolume
- the resulting tar should look like a container filesystem tree, or should
  place selected content under `/var/lib/docker`

Example:

```bash
btrfs-tar \
  --subvol workspace \
  --root-path diff \
  --path-map docker:var/lib/docker \
  /dev/nbd100 workspace.tar.gz
```

## Usage

```text
usage: btrfs-tar [options] <device> <output.tar.gz>
```

Options:

- `-c, --compress <0-9>`: gzip compression level, default `1`
- `-r, --subvolid <id>`: export the specified subvolume id
- `-S, --subvol <path>`: export the subvolume at the given top-level path
- `-R, --root-path <path>`: use the given path inside the selected subvolume as
  archive root
- `-M, --path-map <src:dst>`: archive `src` from the selected subvolume again at
  `dst`
- `-E, --export-path <src:dst>`: archive only `src` from the selected subvolume
  at `dst`
- `-s, --snapshots`: include nested snapshots/subvolumes during traversal
- `-v, --verbose`: print each path as it is archived
- `-h, --help`: show help

## Examples

Export the default subvolume:

```bash
btrfs-tar /dev/nbd100 fs.tar.gz
```

Export a specific subvolume id:

```bash
btrfs-tar --subvolid 256 /dev/nbd100 workspace.tar.gz
```

Export a subvolume by top-level path:

```bash
btrfs-tar --subvol workspace /dev/nbd100 workspace.tar.gz
```

Export only a subtree as archive root:

```bash
btrfs-tar --subvol workspace --root-path diff /dev/nbd100 rwlayer.tar.gz
```

Export only one selected path to a target archive location:

```bash
btrfs-tar --subvol workspace --export-path docker:var/lib/docker /dev/nbd100 docker.tar.gz
```

Export a subtree and replay another directory into a different destination:

```bash
btrfs-tar \
  --subvol workspace \
  --root-path diff \
  --path-map docker:var/lib/docker \
  --path-map extra-config:etc/myapp \
  /dev/nbd100 rwlayer.tar.gz
```

## Operational notes

- The source device should be offline for the conversion. Do not modify it while
  `btrfs-tar` is reading it.
- The output format is gzip-compressed tar in POSIX ustar format.
- If the source contains duplicate archive paths due to `--path-map`, later tar
  entries are written later in the stream and typically win when extracting.
- Leading `/` characters in destination paths are ignored.

## Tests

The implementation is covered by real Btrfs integration tests under
`tests/misc-tests/`, including:

- default subvolume export and fallback to `id 5`
- feature coverage for long paths, sparse files, snapshots, and EOF-sized extents
- `--subvolid`
- `--subvol`
- `--root-path`
- `--path-map`, including remapping content from outside the chosen root path
