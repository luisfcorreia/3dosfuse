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

static int read_block(plus3dos_ctx *ctx, uint32_t block_num, uint8_t *buf, plus3dos_partition *part) {
	uint32_t partition_start = part ? part->partition_start : ctx->partition_start;
	uint32_t sector_size = part ? part->sector_size : ctx->sector_size;
	uint32_t block_size = part ? part->block_size : ctx->block_size;

	off_t disk_offset = (off_t)partition_start * sector_size + (off_t)block_num * block_size;
	off_t file_offset = disk_offset;
	size_t read_size = block_size;

	DBG("read_block: block_num=%u, disk_offset=%jd, read_size=%zu",
		block_num, (intmax_t)disk_offset, read_size);

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
	if (ent->status == 0xE5) return 0;
	if (ent->status > 0x0F && ent->status != 0x00) return 0;
	return 1;
}

static void read_block_pointers(uint16_t *dest, const uint8_t *src, int manyblocks) {
	if (manyblocks) {
		for (int i = 0; i < 8; i++) {
			dest[i] = src[i * 2] | (src[i * 2 + 1] << 8);
		}
	} else {
		for (int i = 0; i < 16; i++) {
			dest[i] = src[i];
		}
	}
}

static int load_directory(plus3dos_ctx *ctx, plus3dos_partition *part) {
	uint32_t dir_start_block = part->off;
	uint32_t dir_blocks = ((part->drm + 1) * CPM_DIR_ENTRY_SIZE + part->block_size - 1) / part->block_size;

	DBG("load_directory: start_block=%u, dir_blocks=%u, block_size=%u, drm=%u",
		dir_start_block, dir_blocks, part->block_size, part->drm);
	DBG("  partition_start=%u, sector_size=%u", part->partition_start, part->sector_size);

	part->files_capacity = part->drm + 1;
	part->files = calloc(part->files_capacity, sizeof(cpm_file_info));
	if (!part->files) return -ENOMEM;

	part->num_files = 0;

	for (uint32_t blk = 0; blk < dir_blocks; blk++) {
		uint8_t block_buf[8192];
		if (read_block(ctx, dir_start_block + blk, block_buf, part) < 0) {
			DBG("Failed to read directory block %u", dir_start_block + blk);
			break;
		}

		DBG("  Read dir block %u (block %u), first 32 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			blk, dir_start_block + blk,
			block_buf[0], block_buf[1], block_buf[2], block_buf[3],
			block_buf[4], block_buf[5], block_buf[6], block_buf[7],
			block_buf[8], block_buf[9], block_buf[10], block_buf[11],
			block_buf[12], block_buf[13], block_buf[14], block_buf[15]);

		int entries_per_block = part->block_size / CPM_DIR_ENTRY_SIZE;
		for (int j = 0; j < entries_per_block && (blk * entries_per_block + j) <= part->drm; j++) {
			plus3dos_dirent *ent = (plus3dos_dirent *)(block_buf + j * CPM_DIR_ENTRY_SIZE);

			if (!is_valid_dirent(ent)) continue;

			char name[CPM_FILENAME_LEN + 1];
			build_filename(ent, name);

			if (name[0] == '\0') continue;

			uint8_t extent = ent->extent_low & 0x1F;

			int found = 0;
			for (uint32_t k = 0; k < part->num_files; k++) {
				if (strcmp((char *)part->files[k].name, name) == 0) {
					found = 1;
					if (extent < CPM_MAX_EXTENTS) {
						read_block_pointers(part->files[k].extents[extent].alloc_blocks, ent->al, part->manyblocks);
						part->files[k].extents[extent].record_count = ent->rcount;
						if (extent >= part->files[k].num_extents) part->files[k].num_extents = extent + 1;
					}
					break;
				}
			}

			if (!found) {
				cpm_file_info *fi = &part->files[part->num_files];
				memset(fi, 0, sizeof(cpm_file_info));
				snprintf((char *)fi->name, CPM_FILENAME_LEN + 1, "%s", name);
				fi->valid = 1;

				if (extent < CPM_MAX_EXTENTS) {
					read_block_pointers(fi->extents[extent].alloc_blocks, ent->al, part->manyblocks);
					fi->extents[extent].record_count = ent->rcount;
					fi->num_extents = extent + 1;
				}
				part->num_files++;
			}
		}
	}

	// Check for +3DOS headers and calculate file sizes
	for (uint32_t i = 0; i < part->num_files; i++) {
		cpm_file_info *fi = &part->files[i];
		uint32_t total_size = 0;
		int num_blocks = part->manyblocks ? 8 : 16;

		for (uint32_t ext = 0; ext < fi->num_extents && ext < CPM_MAX_EXTENTS; ext++) {
			cpm_extent_info *ei = &fi->extents[ext];
			for (int b = 0; b < num_blocks; b++) {
				if (ei->alloc_blocks[b] != 0) {
					total_size += part->block_size;
				}
			}
		}

		fi->size = total_size;

		// Check first block for +3DOS header
		if (fi->num_extents > 0 && fi->extents[0].alloc_blocks[0] != 0) {
			uint8_t first_block[8192];
			uint32_t block_num = fi->extents[0].alloc_blocks[0];
			if (read_block(ctx, block_num, first_block, part) == 0) {
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

	ctx->partitions_capacity = 4;
	ctx->partitions = calloc(ctx->partitions_capacity, sizeof(plus3dos_partition));
	if (!ctx->partitions) return -1;

	ctx->num_partitions = 0;
	ctx->is_idedos = 1;

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

		uint8_t *xdpb = entry + 32;

		// IDEDOS: sysinfo[4-5] is 16-bit LBA of partition start (1-indexed)
		uint16_t lba16 = sysinfo[4] | (sysinfo[5] << 8);
		uint32_t lba = lba16 > 0 ? (uint32_t)(lba16 - 1) : 0;
		DBG("  LBA from sysinfo[4-5]: %u (lba16=%u)", lba, lba16);

		uint8_t bsh = xdpb[2];
		uint32_t block_size = 128U << bsh;

		uint16_t dsm = xdpb[5] | (xdpb[6] << 8);
		uint16_t drm = xdpb[7] | (xdpb[8] << 8);
		uint16_t off = xdpb[13] | (xdpb[14] << 8);
		uint16_t spt = xdpb[0] | (xdpb[1] << 8);
		uint16_t sector_size = xdpb[21] | (xdpb[22] << 8);

		uint8_t manyblocks = (dsm > 255) ? 1 : 0;

		DBG("  XDPB: BSH=%u (block_size=%u), DSM=%u, DRM=%u, OFF=%u, SPT=%u, sector_size=%u, manyblocks=%u",
			bsh, block_size, dsm, drm, off, spt, sector_size, manyblocks);

		if (ctx->num_partitions >= ctx->partitions_capacity) {
			ctx->partitions_capacity *= 2;
			ctx->partitions = realloc(ctx->partitions, ctx->partitions_capacity * sizeof(plus3dos_partition));
			if (!ctx->partitions) return -1;
		}

		plus3dos_partition *part = &ctx->partitions[ctx->num_partitions];
		memset(part, 0, sizeof(plus3dos_partition));
		part->partition_start = lba;
		part->sector_size = sector_size;
		part->block_size = block_size;
		part->spt = spt;
		part->off = off;
		part->drm = drm;
		part->dsm = dsm;
		part->manyblocks = manyblocks;

		memset(part->name, 0, IDEDOS_PARTITION_NAME_LEN + 1);
		for (int j = 0; j < IDEDOS_PARTITION_NAME_LEN && name[j] != ' '; j++) {
			part->name[j] = name[j];
		}

		DBG("  Added partition '%s' (type=0x%02X)", part->name, part_type);
		ctx->num_partitions++;
	}

	if (ctx->num_partitions == 0) {
		DBG("No valid +3DOS partition found");
		free(ctx->partitions);
		ctx->partitions = NULL;
		return -1;
	}

	ctx->partition_start = ctx->partitions[0].partition_start;
	ctx->sector_size = ctx->partitions[0].sector_size;
	ctx->block_size = ctx->partitions[0].block_size;
	ctx->spt = ctx->partitions[0].spt;
	ctx->off = ctx->partitions[0].off;
	ctx->drm = ctx->partitions[0].drm;
	ctx->dsm = ctx->partitions[0].dsm;
	ctx->manyblocks = ctx->partitions[0].manyblocks;

	DBG("Found %u partitions", ctx->num_partitions);
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

	for (uint32_t i = 0; i < ctx->num_partitions; i++) {
		load_directory(ctx, &ctx->partitions[i]);
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

	// Parse path: "/partition" or "/partition/file"
	char part_name[IDEDOS_PARTITION_NAME_LEN + 1];
	const char *file_name = NULL;
	const char *p = path;
	if (p[0] == '/') p++;

	const char *slash = strchr(p, '/');
	if (slash == NULL) {
		// Just a partition name
		strncpy(part_name, p, IDEDOS_PARTITION_NAME_LEN);
		part_name[IDEDOS_PARTITION_NAME_LEN] = '\0';

		for (uint32_t i = 0; i < ctx->num_partitions; i++) {
			if (strcasecmp((char *)ctx->partitions[i].name, part_name) == 0) {
				st->st_mode = S_IFDIR | 0755;
				st->st_nlink = 2;
				return 0;
			}
		}
	} else {
		// Partition + file
		size_t part_len = slash - p;
		if (part_len > IDEDOS_PARTITION_NAME_LEN) part_len = IDEDOS_PARTITION_NAME_LEN;
		strncpy(part_name, p, part_len);
		part_name[part_len] = '\0';
		file_name = slash + 1;

		for (uint32_t i = 0; i < ctx->num_partitions; i++) {
			if (strcasecmp((char *)ctx->partitions[i].name, part_name) == 0) {
				for (uint32_t j = 0; j < ctx->partitions[i].num_files; j++) {
					if (ctx->partitions[i].files[j].valid &&
						strcasecmp((char *)ctx->partitions[i].files[j].name, file_name) == 0) {
						st->st_mode = S_IFREG | 0644;
						st->st_nlink = 1;
						st->st_size = ctx->partitions[i].files[j].size;
						return 0;
					}
				}
				return -ENOENT;
			}
		}
	}

	return -ENOENT;
}

int plus3dos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	(void)offset;
	(void)fi;
	(void)flags;
	plus3dos_ctx *ctx = get_ctx();

	if (strcmp(path, "/") == 0) {
		filler(buf, ".", NULL, 0, 0);
		filler(buf, "..", NULL, 0, 0);

		// List partition folders
		for (uint32_t i = 0; i < ctx->num_partitions; i++) {
			if (ctx->partitions[i].name[0] != '\0') {
				filler(buf, (char *)ctx->partitions[i].name, NULL, 0, 0);
			}
		}
		return 0;
	}

	// Parse path: "/partition"
	char part_name[IDEDOS_PARTITION_NAME_LEN + 1];
	const char *p = path;
	if (p[0] == '/') p++;
	strncpy(part_name, p, IDEDOS_PARTITION_NAME_LEN);
	part_name[IDEDOS_PARTITION_NAME_LEN] = '\0';

	// Find partition and list its files
	for (uint32_t i = 0; i < ctx->num_partitions; i++) {
		if (strcasecmp((char *)ctx->partitions[i].name, part_name) == 0) {
			for (uint32_t j = 0; j < ctx->partitions[i].num_files; j++) {
				if (ctx->partitions[i].files[j].valid) {
					filler(buf, (char *)ctx->partitions[i].files[j].name, NULL, 0, 0);
				}
			}
			return 0;
		}
	}

	return -ENOENT;
}

int plus3dos_open(const char *path, struct fuse_file_info *fi) {
	(void)fi;
	plus3dos_ctx *ctx = get_ctx();

	if (path[0] == '/') path++;

	// Parse path: "partition/file"
	char part_name[IDEDOS_PARTITION_NAME_LEN + 1];
	const char *file_name = NULL;
	const char *slash = strchr(path, '/');

	if (slash == NULL) return -ENOENT; // Not a file path

	size_t part_len = slash - path;
	if (part_len >= sizeof(part_name)) return -ENOENT;
	strncpy(part_name, path, part_len);
	part_name[part_len] = '\0';
	file_name = slash + 1;

	// Find partition and file
	for (uint32_t p = 0; p < ctx->num_partitions; p++) {
		if (strcasecmp((char *)ctx->partitions[p].name, part_name) == 0) {
			for (uint32_t i = 0; i < ctx->partitions[p].num_files; i++) {
				if (ctx->partitions[p].files[i].valid &&
					strcasecmp((char *)ctx->partitions[p].files[i].name, file_name) == 0) {
					return 0;
				}
			}
			return -ENOENT;
		}
	}

	return -ENOENT;
}

int plus3dos_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	(void)fi;
	plus3dos_ctx *ctx = get_ctx();

	if (path[0] == '/') path++;

	// Parse path: "partition/file"
	char part_name[IDEDOS_PARTITION_NAME_LEN + 1];
	const char *file_name = NULL;
	const char *slash = strchr(path, '/');

	if (slash == NULL) return -ENOENT;

	size_t part_len = slash - path;
	if (part_len >= sizeof(part_name)) return -ENOENT;
	strncpy(part_name, path, part_len);
	part_name[part_len] = '\0';
	file_name = slash + 1;

	// Find partition and file
	for (uint32_t p = 0; p < ctx->num_partitions; p++) {
		if (strcasecmp((char *)ctx->partitions[p].name, part_name) == 0) {
			plus3dos_partition *part = &ctx->partitions[p];

			for (uint32_t i = 0; i < part->num_files; i++) {
				if (part->files[i].valid &&
					strcasecmp((char *)part->files[i].name, file_name) == 0) {

					cpm_file_info *fi_info = &part->files[i];
					if (offset >= fi_info->size) return 0;

					size_t to_read = size;
					if (offset + to_read > fi_info->size) to_read = fi_info->size - offset;

					uint8_t *data = malloc(fi_info->size);
					if (!data) return -ENOMEM;

					uint32_t bytes_read = 0;
					for (uint8_t e = 0; e < fi_info->num_extents; e++) {
						for (int b = 0; b < 8; b++) {
							uint16_t block = fi_info->extents[e].alloc_blocks[b];
							if (block == 0) continue;
							read_block(ctx, block, data + bytes_read, part);
							bytes_read += part->block_size;
						}
					}

					memcpy(buf, data + offset, to_read);
					free(data);
					return to_read;
				}
			}
			return -ENOENT;
		}
	}

	return -ENOENT;
}

void plus3dos_destroy(plus3dos_ctx *ctx) {
	if (ctx) {
		if (ctx->fd >= 0) close(ctx->fd);
		free(ctx->files);
		free(ctx->alloc_map);
		free(ctx->partitions);
		free(ctx);
	}
}
