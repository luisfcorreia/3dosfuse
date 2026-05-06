#include "plus3dos.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define DEBUG 1

#if DEBUG
#define DBG(fmt, ...) fprintf(stderr, "[plus3dos] " fmt "\n", ##__VA_ARGS__)
#else
#define DBG(fmt, ...) do {} while(0)
#endif

static int read_block(plus3dos_ctx *ctx, uint32_t block_num, uint8_t *buf) {
	off_t disk_offset = (off_t)ctx->partition_start * ctx->sector_size + (off_t)block_num * ctx->block_size;
	off_t file_offset = disk_offset;
	size_t read_size = ctx->block_size;

	DBG("read_block: block_num=%u, disk_offset=%jd, file_offset=%jd, read_size=%zu",
		block_num, (intmax_t)disk_offset, (intmax_t)file_offset, read_size);

	if (lseek(ctx->fd, file_offset, SEEK_SET) < 0) {
		return -1;
	}
	if (read(ctx->fd, buf, read_size) != (ssize_t)read_size) {
		return -1;
	}

	return 0;
}

static void build_filename(const plus3dos_dirent *ent, char *name) {
	int i, pos = 0;
	for (i = 0; i < 8; i++) {
		uint8_t c = ent->name[i] & 0x7F;
		if (c == ' ' || c == 0) break;
		name[pos++] = c;
	}
	if (pos == 0) { name[0] = '\0'; return; }
	name[pos++] = '.';
	for (i = 0; i < 3; i++) {
		uint8_t c = ent->ext[i] & 0x7F;
		if (c == ' ' || c == 0) break;
		name[pos++] = c;
	}
	if (name[pos-1] == '.') pos--;
	name[pos] = '\0';
}

static int is_valid_dirent(const plus3dos_dirent *ent) {
	if (ent->status == 0xE5 || ent->status == 0x00) return 0;
	return 1;
}

static int parse_idedos_header(plus3dos_ctx *ctx) {
	uint8_t entry0[64];

	if (lseek(ctx->fd, 0, SEEK_SET) < 0) return -1;
	if (read(ctx->fd, entry0, 64) != 64) return -1;

	if (memcmp(entry0, IDEDOS_SIGNATURE, 10) != 0) {
		DBG("Not an IDEDOS image");
		return -1;
	}

	ctx->drive_heads = entry0[34];
	ctx->drive_spt = entry0[35];
	DBG("System Partition: heads=%u, spt=%u", ctx->drive_heads, ctx->drive_spt);

	if (ctx->drive_heads == 0 || ctx->drive_spt == 0) {
		if (ctx->drive_heads == 0) ctx->drive_heads = 16;
		if (ctx->drive_spt == 0) ctx->drive_spt = 63;
	}

	for (int i = 1; i < IDEDOS_NUM_PARTITIONS; i++) {
		uint8_t entry[64];
		off_t entry_offset = i * 64;

		if (lseek(ctx->fd, entry_offset, SEEK_SET) < 0) break;
		if (read(ctx->fd, entry, 64) != 64) break;

		uint8_t *name = entry;
		uint8_t *sysinfo = entry + 16;

		if (name[0] == 0x00 || name[0] == 0xFF) continue;

		uint8_t part_type = sysinfo[0];
		DBG("Partition %d: name=%.16s, type=0x%02X", i, name, part_type);

		if (part_type != 0x03) continue;

		uint16_t cyl = sysinfo[1] | (sysinfo[2] << 8);
		uint8_t head = sysinfo[3];
		uint32_t start_sector = (uint32_t)(cyl * ctx->drive_heads + head) * ctx->drive_spt;

		DBG("  CHS: cyl=%u, head=%u -> LBA=%u", cyl, head, start_sector);

		uint8_t *xdpb = entry + 32;

		DBG("  Raw XDPB: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			xdpb[0], xdpb[1], xdpb[2], xdpb[3], xdpb[4], xdpb[5], xdpb[6], xdpb[7],
			xdpb[8], xdpb[9], xdpb[10], xdpb[11], xdpb[12], xdpb[13], xdpb[14], xdpb[15],
			xdpb[16], xdpb[17], xdpb[18], xdpb[19], xdpb[20], xdpb[21], xdpb[22], xdpb[23],
			xdpb[24], xdpb[25], xdpb[26], xdpb[27]);

		// +3DOS XDPB layout (from Sinclair Wiki):
		// bytes 0-1: SPT (records per track, 16-bit LE)
		// byte 2: BSH (log2(block size / 128))
		// byte 3: BLM (block size / 128 - 1)
		// byte 4: EXM (extent mask)
		// bytes 5-6: DSM (last block number, 16-bit LE)
		// bytes 7-8: DRM (last directory entry number, 16-bit LE)
		// bytes 9-10: AL0/AL1 (directory bit map)
		// bytes 11-12: CKS (checksum size, 16-bit LE)
		// bytes 13-14: OFF (reserved tracks/blocks, 16-bit LE)
		// byte 15: PSH (log2(sector size / 128))
		// byte 16: PHM (sector size / 128 - 1)
		// bytes 21-22: Sector size (16-bit LE)
		uint8_t bsh = xdpb[2];
		uint32_t block_size = 128U << bsh;

		uint16_t drm = xdpb[7] | (xdpb[8] << 8);     // DRM at 7-8 (last dir entry num = 511)
		uint16_t off = xdpb[13] | (xdpb[14] << 8);    // OFF at 13-14 (reserved blocks before dir)
		uint16_t spt = xdpb[0] | (xdpb[1] << 8);     // SPT at 0-1
		// IDE disks use 512-byte sectors; PSH in XDPB is for floppy
		uint16_t sector_size = 512;  // Standard IDE sector size

		DBG("  XDPB: BSH=%u (block_size=%u), DRM=%u, OFF=%u, SPT=%u, sector_size=%u",
			bsh, block_size, drm, off, spt, sector_size);

		ctx->is_idedos = 1;
		ctx->partition_start = start_sector;
		ctx->sector_size = sector_size;
		ctx->block_size = block_size;
		ctx->spt = spt;
		ctx->off = off;
		ctx->drm = drm;

		DBG("  Using Partition '%s' (type=0x%02X)", name, part_type);
		return 0;
	}

	DBG("No valid +3DOS partition found");
	return -1;
}

static int load_directory(plus3dos_ctx *ctx) {
	uint32_t dir_start_block = ctx->off;
	uint32_t dir_blocks = ((ctx->drm + 1) * CPM_DIR_ENTRY_SIZE + ctx->block_size - 1) / ctx->block_size;

	DBG("load_directory: start_block=%u, dir_blocks=%u, block_size=%u, drm=%u",
		dir_start_block, dir_blocks, ctx->block_size, ctx->drm);
	DBG("  partition_start=%u, sector_size=%u", ctx->partition_start, ctx->sector_size);

	ctx->files_capacity = ctx->drm + 1;
	ctx->files = calloc(ctx->files_capacity, sizeof(cpm_file_info));
	if (!ctx->files) return -ENOMEM;

	ctx->num_files = 0;

	for (uint32_t blk = 0; blk < dir_blocks; blk++) {
		uint8_t block_buf[8192];
		if (read_block(ctx, dir_start_block + blk, block_buf) < 0) {
			DBG("Failed to read directory block %u", dir_start_block + blk);
			break;
		}

		DBG("  Read dir block %u (block %u), first 32 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			blk, dir_start_block + blk,
			block_buf[0], block_buf[1], block_buf[2], block_buf[3],
			block_buf[4], block_buf[5], block_buf[6], block_buf[7],
			block_buf[8], block_buf[9], block_buf[10], block_buf[11],
			block_buf[12], block_buf[13], block_buf[14], block_buf[15]);

		int entries_per_block = ctx->block_size / CPM_DIR_ENTRY_SIZE;
		for (int j = 0; j < entries_per_block && (blk * entries_per_block + j) <= ctx->drm; j++) {
			plus3dos_dirent *ent = (plus3dos_dirent *)(block_buf + j * CPM_DIR_ENTRY_SIZE);

			if (!is_valid_dirent(ent)) continue;

			char name[CPM_FILENAME_LEN + 1];
			build_filename(ent, name);

			if (name[0] == '\0') continue;

			uint8_t extent = ent->extent_low & 0x1F;

			int found = 0;
			for (uint32_t k = 0; k < ctx->num_files; k++) {
				if (strcmp((char *)ctx->files[k].name, name) == 0) {
					found = 1;
					if (extent < CPM_MAX_EXTENTS) {
						memcpy(ctx->files[k].extents[extent].alloc_blocks, ent->al, 16);
						ctx->files[k].extents[extent].record_count = ent->rcount;
						if (extent >= ctx->files[k].num_extents) ctx->files[k].num_extents = extent + 1;
					}
					break;
				}
			}

			if (!found) {
				cpm_file_info *fi = &ctx->files[ctx->num_files];
				memset(fi, 0, sizeof(cpm_file_info));
				snprintf((char *)fi->name, CPM_FILENAME_LEN + 1, "%s", name);
				fi->valid = 1;

				if (extent < CPM_MAX_EXTENTS) {
					memcpy(fi->extents[extent].alloc_blocks, ent->al, 16);
					fi->extents[extent].record_count = ent->rcount;
					fi->num_extents = extent + 1;
				}
				ctx->num_files++;
			}
		}
	}

	// Check for +3DOS headers and calculate file sizes
	for (uint32_t i = 0; i < ctx->num_files; i++) {
		cpm_file_info *fi = &ctx->files[i];
		uint32_t total_size = 0;

		for (uint32_t ext = 0; ext < fi->num_extents && ext < CPM_MAX_EXTENTS; ext++) {
			cpm_extent_info *ei = &fi->extents[ext];
			for (int b = 0; b < 16; b++) {
				if (ei->alloc_blocks[b] != 0) {
					total_size += ctx->block_size;
				}
			}
		}

		fi->size = total_size;

		// Check first block for +3DOS header
		if (fi->num_extents > 0 && fi->extents[0].alloc_blocks[0] != 0) {
			uint8_t first_block[8192];
			uint32_t block_num = fi->extents[0].alloc_blocks[0];
			if (read_block(ctx, block_num, first_block) == 0) {
				if (memcmp(first_block, PLUS3DOS_HEADER_MAGIC, 9) == 0) {
					fi->has_header = 1;
					fi->header_file_size = first_block[9] | (first_block[10] << 8);
					fi->size = fi->header_file_size;
					DBG("File '%s' has +3DOS header, size=%u", fi->name, fi->header_file_size);
				}
			}
		}

		DBG("File: '%s', size=%u, extents=%u", fi->name, fi->size, fi->num_extents);
	}

	return 0;
}

plus3dos_ctx *plus3dos_init(const char *image_path) {
	plus3dos_ctx *ctx = calloc(1, sizeof(plus3dos_ctx));
	if (!ctx) return NULL;

	ctx->fd = open(image_path, O_RDONLY);
	if (ctx->fd < 0) {
		free(ctx);
		return NULL;
	}

	if (parse_idedos_header(ctx) != 0) {
		DBG("Failed to parse IDEDOS image");
		close(ctx->fd);
		free(ctx);
		return NULL;
	}

	DBG("Using IDEDOS partition, start=%u, block_size=%u", ctx->partition_start, ctx->block_size);

	if (load_directory(ctx) < 0) {
		plus3dos_destroy(ctx);
		return NULL;
	}

	return ctx;
}

static plus3dos_ctx *get_ctx(void) {
	return (plus3dos_ctx *)fuse_get_context()->private_data;
}

int plus3dos_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
	(void)fi;
	plus3dos_ctx *ctx = get_ctx();
	memset(st, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
		return 0;
	}

	if (path[0] == '/') path++;

	for (uint32_t i = 0; i < ctx->num_files; i++) {
		if (ctx->files[i].valid && strcasecmp((char *)ctx->files[i].name, path) == 0) {
			st->st_mode = S_IFREG | 0644;
			st->st_nlink = 1;
			st->st_size = ctx->files[i].size;
			return 0;
		}
	}

	return -ENOENT;
}

int plus3dos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	(void)offset;
	(void)fi;
	(void)flags;
	plus3dos_ctx *ctx = get_ctx();

	if (strcmp(path, "/") != 0) return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	for (uint32_t i = 0; i < ctx->num_files; i++) {
		if (ctx->files[i].valid) {
			filler(buf, (char *)ctx->files[i].name, NULL, 0, 0);
		}
	}

	return 0;
}

int plus3dos_open(const char *path, struct fuse_file_info *fi) {
	(void)fi;
	plus3dos_ctx *ctx = get_ctx();

	if (path[0] == '/') path++;

	for (uint32_t i = 0; i < ctx->num_files; i++) {
		if (ctx->files[i].valid && strcasecmp((char *)ctx->files[i].name, path) == 0) {
			return 0;
		}
	}

	return -ENOENT;
}

int plus3dos_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	(void)fi;
	plus3dos_ctx *ctx = get_ctx();

	if (path[0] == '/') path++;

	for (uint32_t i = 0; i < ctx->num_files; i++) {
		if (ctx->files[i].valid && strcasecmp((char *)ctx->files[i].name, path) == 0) {
			cpm_file_info *fi_info = &ctx->files[i];

			if (offset >= (off_t)fi_info->size) return 0;
			if (offset + size > fi_info->size) size = fi_info->size - offset;

			uint32_t bytes_read = 0;
			uint32_t data_offset = offset;

			// If file has +3DOS header, skip it in the first block
			if (fi_info->has_header) {
				// Header is 128 bytes at start of first block
				// Data starts after header
				data_offset += PLUS3DOS_HEADER_SIZE;
			}

			while (bytes_read < size) {
				uint32_t block_offset = data_offset / ctx->block_size;
				uint32_t offset_in_block = data_offset % ctx->block_size;
				uint32_t to_read = ctx->block_size - offset_in_block;
				if (to_read > size - bytes_read) to_read = size - bytes_read;

				// Find which block number corresponds to block_offset
				uint32_t block_num = 0;
				uint32_t found = 0;
				uint32_t cum_blocks = 0;

				for (uint32_t ext = 0; ext < fi_info->num_extents && ext < CPM_MAX_EXTENTS; ext++) {
					cpm_extent_info *ei = &fi_info->extents[ext];
					for (int b = 0; b < 16; b++) {
						if (ei->alloc_blocks[b] != 0) {
							if (cum_blocks == block_offset) {
								block_num = ei->alloc_blocks[b];
								found = 1;
								break;
							}
							cum_blocks++;
						}
					}
					if (found) break;
				}

				if (!found) break;

				uint8_t block_data[8192];
				if (read_block(ctx, block_num, block_data) < 0) break;

				memcpy(buf + bytes_read, block_data + offset_in_block, to_read);
				bytes_read += to_read;
				data_offset += to_read;
			}

			return bytes_read;
		}
	}

	return -ENOENT;
}

void plus3dos_destroy(plus3dos_ctx *ctx) {
	if (ctx) {
		if (ctx->fd >= 0) close(ctx->fd);
		free(ctx->files);
		free(ctx->alloc_map);
		free(ctx);
	}
}
