/*
 * Copyright (C) 2024.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

/*
 * btrfs-tar: Convert a btrfs block device to a tar.gz archive.
 *
 * Usage: btrfs-tar [options] <device> <output.tar.gz>
 *
 * Traverses the btrfs filesystem tree (offline, device must be unmounted)
 * and packs all files, directories, and symlinks into a gzip-compressed
 * tar archive in POSIX ustar format.
 */

#include "kerncompat.h"
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zlib.h>

#if COMPRESSION_LZO
#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>
#ifndef lzo1x_worst_compress
#define lzo1x_worst_compress(x) ((x) + ((x) / 16) + 64 + 3)
#endif
#endif

#if COMPRESSION_ZSTD
#include <zstd.h>
#endif

#include "kernel-shared/accessors.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/file-item.h"
#include "common/utils.h"
#include "common/help.h"
#include "common/open-utils.h"
#include "common/messages.h"
#include "common/string-utils.h"
#include "common/box.h"
#include "common/cpu-utils.h"
#include "crypto/hash.h"
#include "cmds/commands.h"

/* ====================================================================
 * Tar format (POSIX ustar)
 * ==================================================================== */

/*
 * POSIX ustar header block.  Must be exactly 512 bytes.
 *
 * Layout (byte offsets):
 *   0   name[100]
 *   100 mode[8]
 *   108 uid[8]
 *   116 gid[8]
 *   124 size[12]
 *   136 mtime[12]
 *   148 checksum[8]
 *   156 type[1]
 *   157 linkname[100]
 *   257 magic[6]
 *   263 version[2]
 *   265 uname[32]
 *   297 gname[32]
 *   329 devmajor[8]
 *   337 devminor[8]
 *   345 prefix[155]
 *   500 padding[12]
 */
struct tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char checksum[8];
	char type;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
	char padding[12];
};

_Static_assert(sizeof(struct tar_header) == 512,
	       "struct tar_header must be exactly 512 bytes");

#define TAR_BLOCK_SIZE		512
#define TAR_TYPE_FILE		'0'	/* regular file */
#define TAR_TYPE_SYMLINK	'2'	/* symbolic link */
#define TAR_TYPE_DIR		'5'	/* directory */
#define TAR_TYPE_LONGNAME	'L'	/* GNU extension: long path name */
#define TAR_TYPE_LONGSYMLINK	'K'	/* GNU extension: long symlink target */

/* I/O buffer size for reading btrfs extents */
#define COPY_BUF_SIZE		(1024 * 1024)	/* 1 MiB */

/* LZO framing constant */
#define LZO_HDR_LEN		4

struct tar_ctx {
	gzFile gz;
	bool verbose;
	bool get_snaps;
	int compress_level;		/* gzip compression level 0-9 */
	/* Reusable buffer for extent reads – avoids per-extent malloc */
	char *io_buf;
	size_t io_buf_size;
};

/* ====================================================================
 * Btrfs data decompression helpers
 * (adapted from cmds/restore.c)
 * ==================================================================== */

static inline size_t read_compress_length(const unsigned char *buf)
{
	__le32 v;

	memcpy(&v, buf, sizeof(v));
	return le32_to_cpu(v);
}

static int decompress_zlib(const char *inbuf, char *outbuf,
			   u64 compress_len, u64 decompress_len)
{
	z_stream strm = { 0 };
	int ret;

	ret = inflateInit(&strm);
	if (ret != Z_OK)
		return -1;

	strm.avail_in  = compress_len;
	strm.next_in   = (unsigned char *)inbuf;
	strm.avail_out = decompress_len;
	strm.next_out  = (unsigned char *)outbuf;

	ret = inflate(&strm, Z_NO_FLUSH);
	inflateEnd(&strm);
	return (ret == Z_STREAM_END) ? 0 : -1;
}

#if COMPRESSION_LZO
static int decompress_lzo(struct btrfs_root *root,
			  const unsigned char *inbuf, char *outbuf,
			  u64 compress_len, u64 *decompress_len)
{
	size_t new_len, in_len, out_len = 0, tot_len, tot_in;
	int ret;

	ret = lzo_init();
	if (ret != LZO_E_OK)
		return -1;

	tot_len = read_compress_length(inbuf);
	inbuf  += LZO_HDR_LEN;
	tot_in  = LZO_HDR_LEN;

	while (tot_in < tot_len) {
		size_t mod_page, rem_page;

		in_len = read_compress_length(inbuf);
		if (tot_in + LZO_HDR_LEN + in_len > tot_len)
			return -1;

		inbuf  += LZO_HDR_LEN;
		tot_in += LZO_HDR_LEN;

		new_len = lzo1x_worst_compress(root->fs_info->sectorsize);
		ret = lzo1x_decompress_safe(inbuf, in_len,
					    (unsigned char *)outbuf,
					    (void *)&new_len, NULL);
		if (ret != LZO_E_OK)
			return -1;

		out_len += new_len;
		outbuf  += new_len;
		inbuf   += in_len;
		tot_in  += in_len;

		mod_page = tot_in % root->fs_info->sectorsize;
		rem_page = root->fs_info->sectorsize - mod_page;
		if (rem_page < LZO_HDR_LEN) {
			inbuf  += rem_page;
			tot_in += rem_page;
		}
	}

	*decompress_len = out_len;
	return 0;
}
#endif /* COMPRESSION_LZO */

#if COMPRESSION_ZSTD
static int decompress_zstd(const char *inbuf, char *outbuf,
			   u64 compress_len, u64 decompress_len)
{
	ZSTD_DStream *strm;
	ZSTD_inBuffer  in  = { inbuf,  compress_len,   0 };
	ZSTD_outBuffer out = { outbuf, decompress_len,  0 };
	size_t zret;
	int ret = 0;

	strm = ZSTD_createDStream();
	if (!strm)
		return -1;

	zret = ZSTD_initDStream(strm);
	if (ZSTD_isError(zret)) {
		ret = -1;
		goto out;
	}

	zret = ZSTD_decompressStream(strm, &out, &in);
	if (ZSTD_isError(zret) || zret != 0)
		ret = -1;
out:
	ZSTD_freeDStream(strm);
	return ret;
}
#endif /* COMPRESSION_ZSTD */

static int decompress(struct btrfs_root *root,
		      const char *inbuf, char *outbuf,
		      u64 compress_len, u64 *decompress_len, int type)
{
	switch (type) {
	case BTRFS_COMPRESS_ZLIB:
		return decompress_zlib(inbuf, outbuf, compress_len,
				       *decompress_len);
#if COMPRESSION_LZO
	case BTRFS_COMPRESS_LZO:
		return decompress_lzo(root, (const unsigned char *)inbuf,
				      outbuf, compress_len, decompress_len);
#endif
#if COMPRESSION_ZSTD
	case BTRFS_COMPRESS_ZSTD:
		return decompress_zstd(inbuf, outbuf, compress_len,
				       *decompress_len);
#endif
	default:
		error("unsupported btrfs compression type: %d", type);
		return -1;
	}
}

/* ====================================================================
 * Gzip output helpers
 * ==================================================================== */

static int gz_write(gzFile gz, const void *buf, size_t len)
{
	int ret;

	if (len == 0)
		return 0;
	ret = gzwrite(gz, buf, (unsigned int)len);
	if (ret < 0 || (size_t)ret != len) {
		int errnum;
		const char *msg = gzerror(gz, &errnum);

		error("gzwrite failed: %s (errno %d)", msg, errnum);
		return -1;
	}
	return 0;
}

/* Write @len zero bytes to the gzip stream. */
static int gz_write_zeros(gzFile gz, u64 len)
{
	static const char zero_block[TAR_BLOCK_SIZE];
	u64 done = 0;

	while (done < len) {
		size_t chunk = (size_t)min_t(u64, len - done, sizeof(zero_block));

		if (gz_write(gz, zero_block, chunk) < 0)
			return -1;
		done += chunk;
	}
	return 0;
}

/* ====================================================================
 * Tar header construction
 * ==================================================================== */

static unsigned int tar_checksum(struct tar_header *hdr)
{
	unsigned char *p = (unsigned char *)hdr;
	unsigned int sum = 0;
	int i;

	/* Treat the checksum field itself as spaces during calculation */
	memset(hdr->checksum, ' ', 8);
	for (i = 0; i < TAR_BLOCK_SIZE; i++)
		sum += p[i];
	return sum;
}

/*
 * Write a GNU @LongLink header+data block to handle names longer than 99 chars.
 * @type: TAR_TYPE_LONGNAME or TAR_TYPE_LONGSYMLINK.
 */
static int write_longlink(gzFile gz, const char *longname, char type)
{
	struct tar_header hdr = { 0 };
	/* Include null terminator in the size */
	size_t len    = strlen(longname) + 1;
	size_t padded = (len + TAR_BLOCK_SIZE - 1) & ~(size_t)(TAR_BLOCK_SIZE - 1);
	unsigned int sum;

	strncpy(hdr.name, "././@LongLink", sizeof(hdr.name) - 1);
	memcpy(hdr.mode,  "0000000", 7);
	memcpy(hdr.uid,   "0000000", 7);
	memcpy(hdr.gid,   "0000000", 7);
	snprintf(hdr.size,  sizeof(hdr.size),  "%011zo", len);
	memcpy(hdr.mtime, "00000000000", 11);
	hdr.type = type;
	/* GNU tar magic */
	memcpy(hdr.magic,   "ustar  ", 6);
	hdr.version[0] = ' ';
	hdr.version[1] = '\0';

	sum = tar_checksum(&hdr);
	snprintf(hdr.checksum, sizeof(hdr.checksum), "%06o", sum);
	hdr.checksum[6] = '\0';
	hdr.checksum[7] = ' ';

	if (gz_write(gz, &hdr, TAR_BLOCK_SIZE) < 0)
		return -1;

	/* Long name payload, padded to block boundary */
	if (gz_write(gz, longname, len) < 0)
		return -1;
	if (padded > len)
		return gz_write_zeros(gz, padded - len);
	return 0;
}

/*
 * Write one tar header block to the gzip stream.
 *
 * Paths longer than 99 bytes are handled by emitting a GNU @LongLink entry
 * before the real header.  The ustar prefix field is used for paths between
 * 100 and 254 bytes that can be split at a '/' boundary.
 */
static int write_tar_header(gzFile gz, const char *path, char type,
			    u64 size, u32 mode, u32 uid, u32 gid,
			    u64 mtime, const char *linkname)
{
	struct tar_header hdr = { 0 };
	size_t pathlen = strlen(path);
	size_t lnlen   = linkname ? strlen(linkname) : 0;
	unsigned int sum;
	int ret;

	/* Emit GNU LongLink entries for names that don't fit */
	if (pathlen > 99) {
		ret = write_longlink(gz, path, TAR_TYPE_LONGNAME);
		if (ret < 0)
			return ret;
	}
	if (linkname && lnlen > 99) {
		ret = write_longlink(gz, linkname, TAR_TYPE_LONGSYMLINK);
		if (ret < 0)
			return ret;
	}

	/*
	 * Fill the name field.  For paths between 100–254 bytes try to use the
	 * ustar prefix/name split (split at the last '/' within the first 155
	 * bytes so that the remaining name fits in 99 bytes).
	 */
	if (pathlen > 99 && pathlen <= 254) {
		const char *p;
		size_t max_prefix = min_t(size_t, pathlen - 1, 154);

		p = path + max_prefix;
		while (p > path && *p != '/')
			p--;

		if (*p == '/' &&
		    (size_t)(p - path) <= 154 &&
		    (size_t)(pathlen - (p - path) - 1) <= 99) {
			strncpy(hdr.prefix, path, p - path);
			strncpy(hdr.name, p + 1, 99);
		} else {
			/* Fall back: truncate (LongLink already emitted) */
			strncpy(hdr.name, path, 99);
		}
	} else {
		strncpy(hdr.name, path, 99);
	}

	snprintf(hdr.mode,  sizeof(hdr.mode),  "%07o",  mode & 07777U);
	snprintf(hdr.uid,   sizeof(hdr.uid),   "%07o",  uid  & 07777777U);
	snprintf(hdr.gid,   sizeof(hdr.gid),   "%07o",  gid  & 07777777U);
	snprintf(hdr.size,  sizeof(hdr.size),  "%011llo", (unsigned long long)size);
	snprintf(hdr.mtime, sizeof(hdr.mtime), "%011llo", (unsigned long long)mtime);
	hdr.type = type;

	if (linkname && lnlen <= 99)
		strncpy(hdr.linkname, linkname, 99);

	memcpy(hdr.magic,   "ustar  ", 6);
	hdr.version[0] = ' ';
	hdr.version[1] = '\0';

	sum = tar_checksum(&hdr);
	snprintf(hdr.checksum, sizeof(hdr.checksum), "%06o", sum);
	hdr.checksum[6] = '\0';
	hdr.checksum[7] = ' ';

	return gz_write(gz, &hdr, TAR_BLOCK_SIZE);
}

/* ====================================================================
 * Inode metadata
 * ==================================================================== */

struct inode_info {
	u32 mode;
	u32 uid;
	u32 gid;
	u64 size;
	u64 mtime;	/* seconds since epoch */
};

static int read_default_subvolid(struct btrfs_fs_info *fs_info, u64 *subvolid)
{
	struct btrfs_path path = { 0 };
	struct btrfs_dir_item *di;
	struct btrfs_key location;

	di = btrfs_lookup_dir_item(NULL, fs_info->tree_root, &path,
				   BTRFS_ROOT_TREE_DIR_OBJECTID,
				   "default", strlen("default"), 0);
	if (IS_ERR(di)) {
		int ret = PTR_ERR(di);

		btrfs_release_path(&path);
		return ret;
	}

	if (!di) {
		/*
		 * No explicit default means the top-level subvolume (id 5).
		 */
		btrfs_release_path(&path);
		*subvolid = BTRFS_FS_TREE_OBJECTID;
		return 0;
	}

	btrfs_dir_item_key_to_cpu(path.nodes[0], di, &location);
	btrfs_release_path(&path);

	*subvolid = location.objectid;
	return 0;
}

static int read_inode_info(struct btrfs_root *root, u64 ino,
			   struct inode_info *info)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	struct btrfs_inode_item *ii;
	struct btrfs_timespec *bts;
	int ret;

	key.objectid = ino;
	key.type     = BTRFS_INODE_ITEM_KEY;
	key.offset   = 0;

	ret = btrfs_lookup_inode(NULL, root, &path, &key, 0);
	if (ret)
		goto out;

	ii = btrfs_item_ptr(path.nodes[0], path.slots[0],
			    struct btrfs_inode_item);

	info->mode = btrfs_inode_mode(path.nodes[0], ii);
	info->uid  = btrfs_inode_uid(path.nodes[0], ii);
	info->gid  = btrfs_inode_gid(path.nodes[0], ii);
	info->size = btrfs_inode_size(path.nodes[0], ii);

	bts = btrfs_inode_mtime(ii);
	info->mtime = btrfs_timespec_sec(path.nodes[0], bts);
	ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

/* ====================================================================
 * Symlink target reading
 * ==================================================================== */

static int read_symlink_target(struct btrfs_root *root, u64 ino,
			       char *target, size_t target_size)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_file_extent_item *fi;
	u32 len;
	unsigned long name_offset;
	int ret;

	key.objectid = ino;
	key.type     = BTRFS_EXTENT_DATA_KEY;
	key.offset   = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	leaf = path.nodes[0];
	fi   = btrfs_item_ptr(leaf, path.slots[0],
			      struct btrfs_file_extent_item);

	len = btrfs_file_extent_inline_item_len(leaf, path.slots[0]);
	if (len >= target_size) {
		error("symlink target for inode %llu too long (%u >= %zu)",
		      (unsigned long long)ino, len, target_size);
		ret = -ENAMETOOLONG;
		goto out;
	}

	name_offset = (unsigned long)fi +
		      offsetof(struct btrfs_file_extent_item, disk_bytenr);
	read_extent_buffer(leaf, target, name_offset, len);
	target[len] = '\0';
	ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

/* ====================================================================
 * File data extraction
 * ==================================================================== */

/*
 * Read an inline-compressed or uncompressed inline extent and write its
 * decompressed contents to the gzip stream.  Updates *written.
 */
static int copy_inline_extent(struct btrfs_root *root, gzFile gz,
			      struct btrfs_path *path, u64 *written)
{
	struct extent_buffer *leaf = path->nodes[0];
	struct btrfs_file_extent_item *fi;
	char *buf, *outbuf = NULL;
	u64 ram_size;
	unsigned long ptr;
	int compress, inline_len;
	int ret;

	fi = btrfs_item_ptr(leaf, path->slots[0],
			    struct btrfs_file_extent_item);

	ptr        = btrfs_file_extent_inline_start(fi);
	ram_size   = btrfs_file_extent_ram_bytes(leaf, fi);
	inline_len = btrfs_file_extent_inline_item_len(leaf, path->slots[0]);
	compress   = btrfs_file_extent_compression(leaf, fi);

	buf = malloc(inline_len);
	if (!buf)
		return -ENOMEM;
	read_extent_buffer(leaf, buf, ptr, inline_len);

	if (compress == BTRFS_COMPRESS_NONE) {
		ret = gz_write(gz, buf, ram_size);
		if (ret == 0)
			*written = ram_size;
		free(buf);
		return ret;
	}

	outbuf = calloc(1, ram_size);
	if (!outbuf) {
		free(buf);
		return -ENOMEM;
	}

	ret = decompress(root, buf, outbuf, inline_len, &ram_size, compress);
	if (ret == 0) {
		ret = gz_write(gz, outbuf, ram_size);
		if (ret == 0)
			*written = ram_size;
	}
	free(buf);
	free(outbuf);
	return ret;
}

/*
 * Read a regular (non-inline) extent and write its (decompressed) contents
 * sequentially to the gzip stream.  Holes (disk_bytenr == 0) are skipped here
 * and filled by the caller via gz_write_zeros.  Updates *written.
 *
 * Uses ctx->io_buf as a reusable read buffer to avoid per-extent malloc/free.
 */
static int copy_reg_extent(struct tar_ctx *ctx, struct btrfs_root *root,
			   struct extent_buffer *leaf,
			   struct btrfs_file_extent_item *fi,
			   u64 *written)
{
	gzFile gz = ctx->gz;
	char *inbuf, *outbuf = NULL;
	u64 bytenr, ram_size, disk_size, num_bytes, offset;
	u64 size_left, total = 0, cur, length;
	int compress;
	int mirror_num = 1, num_copies;
	int ret = 0;

	compress  = btrfs_file_extent_compression(leaf, fi);
	bytenr    = btrfs_file_extent_disk_bytenr(leaf, fi);
	disk_size = btrfs_file_extent_disk_num_bytes(leaf, fi);
	ram_size  = btrfs_file_extent_ram_bytes(leaf, fi);
	offset    = btrfs_file_extent_offset(leaf, fi);
	num_bytes = btrfs_file_extent_num_bytes(leaf, fi);

	/* Sparse hole – caller fills with zeros */
	if (disk_size == 0) {
		*written = 0;
		return 0;
	}

	size_left = disk_size;
	if (compress == BTRFS_COMPRESS_NONE && offset < disk_size) {
		bytenr    += offset;
		size_left -= offset;
	}

	/* Grow the reusable buffer only when necessary */
	if (size_left > ctx->io_buf_size) {
		char *p = realloc(ctx->io_buf, size_left);

		if (!p)
			return -ENOMEM;
		ctx->io_buf      = p;
		ctx->io_buf_size = size_left;
	}
	inbuf = ctx->io_buf;

	if (compress != BTRFS_COMPRESS_NONE) {
		outbuf = calloc(1, ram_size);
		if (!outbuf)
			return -ENOMEM;
	}

	num_copies = btrfs_num_copies(root->fs_info, bytenr, disk_size);
again:
	cur = bytenr;
	while (cur < bytenr + size_left) {
		length = bytenr + size_left - cur;
		ret = read_data_from_disk(root->fs_info,
					  inbuf + cur - bytenr,
					  cur, &length, mirror_num);
		if (ret < 0) {
			if (++mirror_num > num_copies) {
				error("failed to read extent at %llu after "
				      "trying all %d mirrors",
				      (unsigned long long)bytenr, num_copies);
				goto out;
			}
			goto again;
		}
		cur += length;
	}

	if (compress == BTRFS_COMPRESS_NONE) {
		while (total < num_bytes) {
			size_t chunk = (size_t)min_t(u64, num_bytes - total,
						     COPY_BUF_SIZE);
			ret = gz_write(gz, inbuf + total, chunk);
			if (ret < 0)
				goto out;
			total += chunk;
		}
		*written = num_bytes;
		ret = 0;
		goto out;
	}

	ret = decompress(root, inbuf, outbuf, disk_size, &ram_size, compress);
	if (ret < 0) {
		if (++mirror_num <= num_copies)
			goto again;
		goto out;
	}

	while (total < num_bytes) {
		size_t chunk = (size_t)min_t(u64, num_bytes - total,
					     COPY_BUF_SIZE);
		ret = gz_write(gz, outbuf + offset + total, chunk);
		if (ret < 0)
			goto out;
		total += chunk;
	}
	*written = num_bytes;
out:
	/* inbuf is ctx->io_buf – do not free it here */
	free(outbuf);
	return ret;
}

/*
 * Write the complete data for file inode @ino (of logical size @file_size) to
 * the gzip stream.  Sparse regions (holes) are emitted as zero bytes so that
 * the tar entry has the correct size.
 */
static int write_file_data(struct tar_ctx *ctx, struct btrfs_root *root,
			   u64 ino, u64 file_size)
{
	gzFile gz = ctx->gz;
	struct btrfs_path path = { 0 };
	struct btrfs_key key, found_key;
	struct extent_buffer *leaf;
	u64 current_offset = 0;
	int ret;

	if (file_size == 0)
		return 0;

	key.objectid = ino;
	key.type     = BTRFS_EXTENT_DATA_KEY;
	key.offset   = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;

	leaf = path.nodes[0];

	while (1) {
		struct btrfs_file_extent_item *fi;
		int extent_type;
		u64 bytes_written = 0;

		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret > 0) {
				ret = 0;
				break;
			}
			if (ret < 0)
				goto out;
			leaf = path.nodes[0];
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (found_key.objectid != ino ||
		    found_key.type != BTRFS_EXTENT_DATA_KEY)
			break;

		/* Fill any sparse hole before this extent */
		if (found_key.offset > current_offset) {
			ret = gz_write_zeros(gz,
					     found_key.offset - current_offset);
			if (ret < 0)
				goto out;
			current_offset = found_key.offset;
		}

		fi          = btrfs_item_ptr(leaf, path.slots[0],
					     struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(leaf, fi);

		if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
			ret = copy_inline_extent(root, gz, &path,
						 &bytes_written);
		} else if (extent_type == BTRFS_FILE_EXTENT_REG) {
			ret = copy_reg_extent(ctx, root, leaf, fi,
					      &bytes_written);
		}
		/* PREALLOC extents contain no initialized data – skip */

		if (ret < 0)
			goto out;

		current_offset += bytes_written;
		path.slots[0]++;
	}

	/* Any trailing sparse region */
	if (current_offset < file_size)
		ret = gz_write_zeros(gz, file_size - current_offset);
out:
	btrfs_release_path(&path);
	return ret;
}

/* ====================================================================
 * Per-entry tar writing
 * ==================================================================== */

/*
 * Compute the tar path from the full in-tree path.
 * Strip a leading '/' so that extraction is relative.
 */
static const char *tar_path(const char *path)
{
	return (path[0] == '/') ? path + 1 : path;
}

static int write_dir_entry(struct tar_ctx *ctx, struct btrfs_root *root,
			   const char *path, u64 ino)
{
	struct inode_info info;
	char dir_path[PATH_MAX + 2];
	int ret;

	ret = read_inode_info(root, ino, &info);
	if (ret) {
		error("failed to read inode %llu for dir '%s': %d",
		      (unsigned long long)ino, path, ret);
		return ret;
	}

	/* Directories in tar archives end with '/' */
	snprintf(dir_path, sizeof(dir_path), "%s/", tar_path(path));

	return write_tar_header(ctx->gz, dir_path, TAR_TYPE_DIR,
				0, info.mode, info.uid, info.gid,
				info.mtime, NULL);
}

static int write_file_entry(struct tar_ctx *ctx, struct btrfs_root *root,
			    const char *path, u64 ino)
{
	struct inode_info info;
	size_t pad;
	int ret;

	ret = read_inode_info(root, ino, &info);
	if (ret) {
		error("failed to read inode %llu for file '%s': %d",
		      (unsigned long long)ino, path, ret);
		return ret;
	}

	ret = write_tar_header(ctx->gz, tar_path(path), TAR_TYPE_FILE,
			       info.size, info.mode, info.uid, info.gid,
			       info.mtime, NULL);
	if (ret < 0)
		return ret;

	ret = write_file_data(ctx, root, ino, info.size);
	if (ret < 0)
		return ret;

	/* Pad data to block boundary */
	pad = (TAR_BLOCK_SIZE - (info.size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
	if (pad > 0)
		ret = gz_write_zeros(ctx->gz, pad);
	return ret;
}

static int write_symlink_entry(struct tar_ctx *ctx, struct btrfs_root *root,
			       const char *path, u64 ino)
{
	struct inode_info info;
	char target[PATH_MAX];
	int ret;

	ret = read_inode_info(root, ino, &info);
	if (ret) {
		error("failed to read inode %llu for symlink '%s': %d",
		      (unsigned long long)ino, path, ret);
		return ret;
	}

	ret = read_symlink_target(root, ino, target, sizeof(target));
	if (ret < 0) {
		error("failed to read symlink target for '%s': %d", path, ret);
		return ret;
	}

	return write_tar_header(ctx->gz, tar_path(path), TAR_TYPE_SYMLINK,
				0, info.mode, info.uid, info.gid,
				info.mtime, target);
}

/* ====================================================================
 * Directory traversal
 * ==================================================================== */

/* Forward declaration – traverse_dir and write_dir_recurse call each other */
static int traverse_dir(struct tar_ctx *ctx, struct btrfs_root *root,
			u64 dir_ino, const char *dir_path);

/*
 * Process a single directory entry found by traverse_dir.
 * Handles subvolumes/snapshots by opening a separate btrfs root.
 */
static int process_dir_entry(struct tar_ctx *ctx, struct btrfs_root *root,
			     const char *entry_path, u8 type,
			     struct btrfs_key *location)
{
	int ret = 0;

	if (ctx->verbose)
		printf("%s\n", entry_path);

	if (type == BTRFS_FT_REG_FILE) {
		ret = write_file_entry(ctx, root, entry_path,
				       location->objectid);
	} else if (type == BTRFS_FT_SYMLINK) {
		ret = write_symlink_entry(ctx, root, entry_path,
					  location->objectid);
	} else if (type == BTRFS_FT_DIR) {
		struct btrfs_root *sub_root = root;
		u64 sub_ino;

		if (location->type == BTRFS_ROOT_ITEM_KEY) {
			struct btrfs_key loc = *location;

			/*
			 * Self-referential snapshot entries appear in the root
			 * directory index; skip them.
			 */
			if (location->objectid == root->root_key.objectid)
				return 0;

			loc.offset = (u64)-1;
			sub_root = btrfs_read_fs_root(root->fs_info, &loc);
			if (IS_ERR(sub_root)) {
				error("failed to open subvolume '%s': %ld",
				      entry_path, PTR_ERR(sub_root));
				return 0;	/* non-fatal, skip */
			}

			/*
			 * A snapshot has a non-zero root_key.offset (the
			 * transaction id at creation time).  Skip snapshots
			 * unless -s was given.
			 */
			if (sub_root->root_key.offset != 0 && !ctx->get_snaps) {
				pr_verbose(LOG_DEFAULT,
					   "Skipping snapshot '%s'\n",
					   entry_path);
				return 0;
			}

			sub_ino = BTRFS_FIRST_FREE_OBJECTID;
		} else {
			sub_ino = location->objectid;
		}

		/* Write the directory tar header */
		ret = write_dir_entry(ctx, sub_root, entry_path, sub_ino);
		if (ret < 0)
			return ret;

		/* Recurse into the directory */
		ret = traverse_dir(ctx, sub_root, sub_ino, entry_path);
	}
	/* Other types (devices, FIFOs, …) are silently skipped. */

	return ret;
}

/*
 * Iterate over all BTRFS_DIR_INDEX_KEY items for directory inode @dir_ino and
 * emit tar entries for each child.
 */
static int traverse_dir(struct tar_ctx *ctx, struct btrfs_root *root,
			u64 dir_ino, const char *dir_path)
{
	struct btrfs_path path = { 0 };
	struct btrfs_key key, found_key, location;
	struct btrfs_dir_item *di;
	struct extent_buffer *leaf;
	char filename[BTRFS_NAME_LEN + 1];
	char entry_path[PATH_MAX];
	unsigned long name_ptr;
	int name_len;
	u8 type;
	int ret = 0;

	key.objectid = dir_ino;
	key.type     = BTRFS_DIR_INDEX_KEY;
	key.offset   = 0;

	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		goto out;
	/* ret > 0 means the key wasn't found but path points at the next slot */
	ret = 0;

	leaf = path.nodes[0];

	while (1) {
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root, &path);
			if (ret > 0) {
				ret = 0;
				break;
			}
			if (ret < 0)
				goto out;
			leaf = path.nodes[0];
			continue;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (found_key.objectid != dir_ino ||
		    found_key.type != BTRFS_DIR_INDEX_KEY)
			break;

		di       = btrfs_item_ptr(leaf, path.slots[0],
					  struct btrfs_dir_item);
		name_ptr = (unsigned long)(di + 1);
		name_len = btrfs_dir_name_len(leaf, di);
		if (name_len > BTRFS_NAME_LEN)
			name_len = BTRFS_NAME_LEN;

		read_extent_buffer(leaf, filename, name_ptr, name_len);
		filename[name_len] = '\0';

		type = btrfs_dir_ftype(leaf, di);
		btrfs_dir_item_key_to_cpu(leaf, di, &location);

		snprintf(entry_path, sizeof(entry_path), "%s/%s",
			 dir_path, filename);

		ret = process_dir_entry(ctx, root, entry_path, type,
					&location);
		if (ret < 0)
			goto out;

		path.slots[0]++;
	}
out:
	btrfs_release_path(&path);
	return ret;
}

/* ====================================================================
 * Entry point
 * ==================================================================== */

static const char * const usage_msg[] = {
	"btrfs-tar [options] <device> <output.tar.gz>",
	"Convert an unmounted btrfs block device to a gzip-compressed tar archive",
	"",
	"The device must not be mounted.  All files, directories, and symlinks",
	"in the default subvolume are written to <output.tar.gz> in POSIX ustar",
	"format.  Sparse file regions are preserved as zero-filled data.",
	"",
	"Options:",
	OPTLINE("-c|--compress <0-9>", "gzip compression level: 0=none 1=fastest(default) 9=best"),
	OPTLINE("-s|--snapshots",      "also include snapshots (default: skipped)"),
	OPTLINE("-v|--verbose",        "print each path as it is added to the archive"),
	OPTLINE("-h|--help",           "show this help and exit"),
	"",
	"Compression support: zlib (output)"
#if COMPRESSION_LZO
	", lzo (btrfs inline)"
#endif
#if COMPRESSION_ZSTD
	", zstd (btrfs inline)"
#endif
	,
	NULL
};

static const struct cmd_struct tar_cmd = {
	.usagestr = usage_msg,
};

int BOX_MAIN(tar)(int argc, char *argv[])
{
	struct tar_ctx ctx   = { .compress_level = 1 };
	struct btrfs_fs_info *fs_info;
	struct btrfs_root *root;
	struct open_ctree_args oca = { 0 };
	struct btrfs_key key;
	u64 default_subvolid = BTRFS_FS_TREE_OBJECTID;
	const char *device;
	const char *output;
	char gz_mode[8];
	/* Two 512-byte zero blocks mark end-of-archive */
	const char end_marker[TAR_BLOCK_SIZE * 2] = { 0 };
	int ret;

	cpu_detect_flags();
	hash_init_accel();
	btrfs_config_init();

	while (1) {
		static const struct option long_opts[] = {
			{ "compress",  required_argument, NULL, 'c' },
			{ "snapshots", no_argument,       NULL, 's' },
			{ "verbose",   no_argument,       NULL, 'v' },
			{ "help",      no_argument,       NULL, 'h' },
			{ NULL, 0, NULL, 0 }
		};
		int c = getopt_long(argc, argv, "c:svh", long_opts, NULL);

		if (c < 0)
			break;
		switch (c) {
		case 'c': {
			char *end;
			long level = strtol(optarg, &end, 10);

			if (*end != '\0' || level < 0 || level > 9) {
				error("compression level must be 0-9");
				return 1;
			}
			ctx.compress_level = (int)level;
			break;
		}
		case 's':
			ctx.get_snaps = true;
			break;
		case 'v':
			ctx.verbose = true;
			break;
		case 'h':
			usage(&tar_cmd, 0);
			/* fallthrough unreachable */
		default:
			usage(&tar_cmd, 1);
		}
	}

	set_argv0(argv);

	if (argc - optind < 2) {
		error("usage: btrfs-tar <device> <output.tar.gz>");
		usage(&tar_cmd, 1);
	}

	device = argv[optind];
	output = argv[optind + 1];

	ret = check_mounted(device);
	if (ret < 0) {
		errno = -ret;
		error("cannot check mount status of '%s': %m", device);
		return 1;
	}
	if (ret) {
		error("'%s' is currently mounted – unmount it first", device);
		return 1;
	}

	/*
	 * Open the filesystem read-only.  We use PARTIAL so that we can
	 * proceed even if some metadata trees are damaged, and NO_BLOCK_GROUPS
	 * because we only need the FS tree.  ALLOW_TRANSID_MISMATCH tolerates
	 * unclean shutdowns (the tree is still readable).
	 */
	oca.filename = device;
	oca.flags    = OPEN_CTREE_PARTIAL | OPEN_CTREE_NO_BLOCK_GROUPS |
		       OPEN_CTREE_ALLOW_TRANSID_MISMATCH;

	fs_info = open_ctree_fs_info(&oca);
	if (!fs_info) {
		error("failed to open btrfs filesystem on '%s'", device);
		return 1;
	}

	ret = read_default_subvolid(fs_info, &default_subvolid);
	if (ret < 0) {
		error("failed to read default subvolume id: %d", ret);
		close_ctree(fs_info->tree_root);
		return 1;
	}

	key.objectid = default_subvolid;
	key.type     = BTRFS_ROOT_ITEM_KEY;
	key.offset   = (u64)-1;

	root = btrfs_read_fs_root(fs_info, &key);
	if (IS_ERR(root)) {
		error("failed to read default subvolume %llu: %ld",
		      (unsigned long long)default_subvolid, PTR_ERR(root));
		close_ctree(fs_info->tree_root);
		return 1;
	}

	snprintf(gz_mode, sizeof(gz_mode), "wb%d", ctx.compress_level);
	ctx.gz = gzopen(output, gz_mode);
	if (!ctx.gz) {
		error("failed to create output file '%s': %m", output);
		close_ctree(root);
		return 1;
	}

	ret = traverse_dir(&ctx, root, BTRFS_FIRST_FREE_OBJECTID, "");
	if (ret) {
		error("filesystem traversal failed: %d", ret);
		goto out;
	}

	/* End-of-archive: two consecutive 512-byte zero blocks */
	ret = gz_write(ctx.gz, end_marker, sizeof(end_marker));
out:
	gzclose(ctx.gz);
	close_ctree(root);
	free(ctx.io_buf);

	if (ret)
		unlink(output);

	return !!ret;
}
