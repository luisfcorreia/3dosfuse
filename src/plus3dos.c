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
	// Calculate disk offset: partition start (in sectors) * sector_size + block_num * block_size
	off_t disk_offset = (off_t)ctx->partition_start * ctx->sector_size + (off_t)block_num * ctx->block_size;
	// Interleaved image format: file offset = disk offset * 2
	off_t file_offset = disk_offset * 2;
	size_t read_size = ctx->block_size * 2;
	
	uint8_t *temp = malloc(read_size);
	if (!temp) return -1;
	
	if (lseek(ctx->fd, file_offset, SEEK_SET) < 0) {
		free(temp);
		return -1;
	}
	if (read(ctx->fd, temp, read_size) != (ssize_t)read_size) {
		free(temp);
		return -1;
	}
	
	for (uint32_t i = 0; i < ctx->block_size; i++) {
		buf[i] = temp[i * 2];
	}
	
	free(temp);
	return 0;
}

static int parse_idedos_header(plus3dos_ctx *ctx) {
	// IDEDOS partition table starts at disk offset 0
	// Each entry is 64 bytes, stored interleaved (1 data byte + 1 padding)
	// File offset = disk_offset * 2

	// Read first entry (System Partition)
	uint8_t raw_entry0[128];  // 64 bytes * 2 for interleaving

	if (lseek(ctx->fd, 0, SEEK_SET) < 0) return -1;
	if (read(ctx->fd, raw_entry0, 128) != 128) return -1;

	uint8_t entry0[64];
	for (int j = 0; j < 64; j++) entry0[j] = raw_entry0[j * 2];

	// Check for IDEDOS signature in first entry's name field
	if (memcmp(entry0, IDEDOS_SIGNATURE, 10) != 0) {
		DBG("Not an IDEDOS image (no signature in first entry)");
		return -1;
	}

	DBG("Found IDEDOS image");

	// First pass: Read System Partition (entry 0) to get drive geometry
	// Geometry is at bytes 32-39 of entry (entry[32]-entry[39])
	// entry[34] = number of heads (NH)
	// entry[35] = sectors per track (ST)
	ctx->drive_heads = entry0[34];
	ctx->drive_spt = entry0[35];
	DBG("System Partition: heads=%u, sectors_per_track=%u", ctx->drive_heads, ctx->drive_spt);

	if (ctx->drive_heads == 0 || ctx->drive_spt == 0) {
		DBG("Invalid drive geometry, using defaults (16 heads, 63 spt)");
		if (ctx->drive_heads == 0) ctx->drive_heads = 16;
		if (ctx->drive_spt == 0) ctx->drive_spt = 63;
	}

	// Second pass: Find +3DOS partition (type 0x03)
	// Entry 0 is System Partition, start from entry 1
	for (int i = 1; i < IDEDOS_NUM_PARTITIONS; i++) {
		uint8_t raw_entry[128];
		// File offset = disk_offset * 2 = (i * 64) * 2 = i * 128
		off_t entry_offset = (i * 64) * 2;

		if (lseek(ctx->fd, entry_offset, SEEK_SET) < 0) break;
		if (read(ctx->fd, raw_entry, 128) != 128) break;

		uint8_t entry[64];
		for (int j = 0; j < 64; j++) entry[j] = raw_entry[j * 2];

		uint8_t *name = entry;
		uint8_t *sysinfo = entry + 16;

		if (name[0] == 0x00 || name[0] == 0xFF) continue;

		uint8_t part_type = sysinfo[0];
		DBG("Partition %d: name=%.16s, type=0x%02X", i, name, part_type);

		// Only process +3DOS partitions (type 0x03)
		if (part_type != 0x03) continue;

		// Parse CHS start using drive geometry
		uint16_t cyl = sysinfo[1] | (sysinfo[2] << 8);
		uint8_t head = sysinfo[3];
		// LBA = (cylinder * heads_per_cylinder + head) * sectors_per_track
		uint32_t start_sector = (uint32_t)(cyl * ctx->drive_heads + head) * ctx->drive_spt;

		DBG("  CHS: cyl=%u, head=%u -> LBA=%u", cyl, head, start_sector);

		// Read XDPB from bytes 32-59 of partition entry (28 bytes of eXDPB)
		uint8_t *xdpb = entry + 32;

		// Parse key XDPB fields (little-endian, from +3e spec)
		uint16_t drm = xdpb[7] | (xdpb[8] << 8);
		uint16_t off = xdpb[13] | (xdpb[14] << 8);
		uint8_t spt = xdpb[19];
		uint16_t sector_size = xdpb[21] | (xdpb[22] << 8);
		uint8_t first_sector = xdpb[20];

		DBG("  XDPB: DRM=%u, OFF=%u, SPT=%u, sector_size=%u, first_sector=%u",
			drm, off, spt, sector_size, first_sector);

		if (spt == 0 || sector_size == 0) {
			DBG("  Invalid XDPB, skipping");
			continue;
		}

		// Set context for read_sector() to work during validation
		ctx->is_idedos = 1;
		ctx->partition_start = start_sector;
		ctx->sector_size = sector_size;
		ctx->sectors_per_track = spt;
		ctx->first_sector = first_sector;

		// Test if directory has any entries
		uint8_t test_sector[512];
		uint32_t dir_track = off;
		uint8_t dir_sector = first_sector;

		int dir_valid = 0;
		if (read_sector(ctx, dir_track, dir_sector, test_sector) == 0) {
			DBG("  Read dir sector, first 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
				test_sector[0], test_sector[1], test_sector[2], test_sector[3],
				test_sector[4], test_sector[5], test_sector[6], test_sector[7],
				test_sector[8], test_sector[9], test_sector[10], test_sector[11],
				test_sector[12], test_sector[13], test_sector[14], test_sector[15]);

			if (test_sector[0] != 0x00 && test_sector[0] != 0xE5) {
				dir_valid = 1;
			}
		}

		if (!dir_valid) {
			// Reset context since we're skipping this partition
			ctx->is_idedos = 0;
			ctx->partition_start = 0;
			DBG("  Partition '%s' has empty directory, skipping", name);
			continue;
		}

		// Keep the context values set above
		ctx->tracks = 0;
		ctx->spt = spt;
		ctx->off = off;
		ctx->drm = drm;

		DBG("  Using partition '%s'", name);
		return 0;
	}

	DBG("No valid +3DOS partition found");
	return -1;
}

static void build_filename(const cpm_dirent *ent, char *name) {
	int i, pos = 0;
	for (i = 0; i < 8; i++) {
		if (ent->name[i] == ' ' || ent->name[i] == 0) break;
		name[pos++] = ent->name[i];
	}
	if (ent->ext[0] != ' ' && ent->ext[0] != 0) {
		name[pos++] = '.';
		for (i = 0; i < 3; i++) {
			if (ent->ext[i] == ' ' || ent->ext[i] == 0) break;
			name[pos++] = ent->ext[i];
		}
	}
	name[pos] = '\0';
}

static int is_valid_dirent(const cpm_dirent *ent) {
	if (ent->name[0] == 0xE5) return 0;
	if (ent->name[0] == 0x00) return 0;
	return 1;
}

static int load_directory(plus3dos_ctx *ctx) {
	uint8_t sector_buf[512];
	uint32_t sector_size = ctx->sector_size;
	uint32_t dir_start_track = ctx->off;
	uint32_t dir_entries = ctx->drm + 1;
	uint32_t dir_sectors = ((dir_entries * CPM_DIR_ENTRY_SIZE) + sector_size - 1) / sector_size;

	ctx->files_capacity = dir_entries;
	ctx->files = calloc(dir_entries, sizeof(cpm_file_info));
	if (!ctx->files) return -ENOMEM;

	ctx->num_files = 0;

	for (uint32_t sec = 0; sec < dir_sectors; sec++) {
		uint32_t track = dir_start_track + (sec / ctx->sectors_per_track);
		uint8_t sector = ctx->first_sector + (sec % ctx->sectors_per_track);

		if (read_sector(ctx, track, sector, sector_buf) < 0) {
			DBG("Failed to read directory sector %u/%u", track, sector);
			break;
		}

		DBG("Read dir sector, first 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
			sector_buf[0], sector_buf[1], sector_buf[2], sector_buf[3], sector_buf[4], sector_buf[5], sector_buf[6], sector_buf[7],
			sector_buf[8], sector_buf[9], sector_buf[10], sector_buf[11], sector_buf[12], sector_buf[13], sector_buf[14], sector_buf[15]);

		int entries_per_sector = sector_size / CPM_DIR_ENTRY_SIZE;
		for (int j = 0; j < entries_per_sector && (sec * entries_per_sector + j) < dir_entries; j++) {
			cpm_dirent *ent = (cpm_dirent *)(sector_buf + j * CPM_DIR_ENTRY_SIZE);

			if (!is_valid_dirent(ent)) continue;

			char name[CPM_FILENAME_LEN + 1];
			build_filename(ent, name);

			int found = 0;
			for (uint32_t k = 0; k < ctx->num_files; k++) {
				if (strcmp((char *)ctx->files[k].name, name) == 0) {
					found = 1;
					uint8_t ext = ent->extent & 0x1F;
					if (ext < CPM_MAX_EXTENTS) {
						memcpy(ctx->files[k].extents[ext].alloc_blocks, ent->alloc_blocks, 16);
						ctx->files[k].extents[ext].record_count = ent->record_count;
						if (ext >= ctx->files[k].num_extents) ctx->files[k].num_extents = ext + 1;
						ctx->files[k].size += ent->record_count * 128;
					}
					break;
				}
			}

			if (!found) {
				cpm_file_info *fi = &ctx->files[ctx->num_files];
				memset(fi, 0, sizeof(cpm_file_info));
				snprintf((char *)fi->name, CPM_FILENAME_LEN + 1, "%s", name);
				fi->valid = 1;

				uint8_t ext = ent->extent & 0x1F;
				if (ext < CPM_MAX_EXTENTS) {
					memcpy(fi->extents[ext].alloc_blocks, ent->alloc_blocks, 16);
					fi->extents[ext].record_count = ent->record_count;
					fi->num_extents = ext + 1;
				}
				fi->size = ent->record_count * 128;
				ctx->num_files++;
}
	}

	return 0;
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

	// Parse IDEDOS header - fail if not IDEDOS
	if (parse_idedos_header(ctx) != 0) {
		DBG("Failed to parse IDEDOS image");
		close(ctx->fd);
		free(ctx);
		return NULL;
	}

	DBG("Using IDEDOS partition, start=%u", ctx->partition_start);

// Validate spt before use to avoid division by zero
	if (ctx->spt == 0) {
		DBG("WARNING: spt is 0, setting to 1 to avoid division by zero");
		ctx->spt = 1;
	}

	if (ctx->drm == 0) {
		DBG("WARNING: drm is 0, setting to 32");
		ctx->drm = 32;
	}

	DBG("Loading directory: off=%u, drm=%u", ctx->off, ctx->drm);

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

			uint32_t record_offset = offset / 128;
			uint32_t bytes_read = 0;

			while (bytes_read < size) {
				uint32_t rec = record_offset + (bytes_read / 128);
				uint32_t rec_offset = bytes_read % 128;
				uint32_t rec_bytes = 128 - rec_offset;
				if (rec_bytes > size - bytes_read) rec_bytes = size - bytes_read;

				uint32_t extent = rec / 128;
				uint32_t rec_in_extent = rec % 128;

				if (extent >= CPM_MAX_EXTENTS || rec_in_extent >= 128) break;

				cpm_extent_info *ext_info = &fi_info->extents[extent];
				if (ext_info->record_count == 0) break;

				uint8_t alloc_idx = rec_in_extent / 8;
				uint8_t block_num = (alloc_idx < 16) ? ext_info->alloc_blocks[alloc_idx] : 0;

				if (block_num == 0) break;

				uint32_t block_size = 1024;
				uint32_t sector_in_block = rec_in_extent % 8;
				uint32_t data_start_sector = (uint32_t)block_num * (block_size / ctx->sector_size) + sector_in_block;

				uint32_t track = data_start_sector / ctx->sectors_per_track;
				uint8_t sector = ctx->first_sector + (data_start_sector % ctx->sectors_per_track);

				uint8_t record_buf[128];
				if (read_sector(ctx, track, sector, record_buf) < 0) break;

				memcpy(buf + bytes_read, record_buf + rec_offset, rec_bytes);
				bytes_read += rec_bytes;
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
