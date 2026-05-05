#include "plus3dos.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define BOOT_SECTOR_SIZE 512

typedef struct {
	uint16_t spt;
	uint8_t bsh;
	uint8_t blm;
	uint8_t exm;
	uint16_t dsm;
	uint16_t drm;
	uint8_t al0;
	uint8_t al1;
	uint16_t cks;
	uint16_t off;
	uint8_t psh;
	uint8_t phm;
	uint8_t flags;
	uint8_t tracks_per_side;
	uint8_t sectors_per_track;
	uint8_t first_sector;
	uint16_t sector_size;
	uint8_t gap_rw;
	uint8_t gap_fmt;
	uint8_t mfm_flags;
	uint8_t freeze;
} xdpb_t;

static int read_sector(plus3dos_ctx *ctx, uint32_t track, uint8_t sector, uint8_t *buf) {
	if (sector == 0 || sector > ctx->sectors_per_track) return -1;

	uint32_t sector_num = (track * ctx->sectors_per_track) + (sector - ctx->first_sector);
	off_t offset = (off_t)sector_num * ctx->sector_size;

	// Interleaved image format: each disk byte is followed by padding
	// File offset = disk offset * 2, read every other byte
	off_t file_offset = offset * 2;
	size_t read_size = (size_t)ctx->sector_size * 2;
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

	for (uint32_t i = 0; i < ctx->sector_size; i++) {
		buf[i] = temp[i * 2];
	}

	free(temp);
	return 0;
}

static int parse_xdpb(const uint8_t *data, xdpb_t *xdpb) {
	if (!data || !xdpb) return -1;

	xdpb->spt = data[0] | (data[1] << 8);
	xdpb->bsh = data[2];
	xdpb->blm = data[3];
	xdpb->exm = data[4];
	xdpb->dsm = data[5] | (data[6] << 8);
	xdpb->drm = data[7] | (data[8] << 8);
	xdpb->al0 = data[9];
	xdpb->al1 = data[10];
	xdpb->cks = data[11] | (data[12] << 8);
	xdpb->off = data[13] | (data[14] << 8);
	xdpb->psh = data[15];
	xdpb->phm = data[16];
	xdpb->flags = data[17];
	xdpb->tracks_per_side = data[18];
	xdpb->sectors_per_track = data[19];
	xdpb->first_sector = data[20];
	xdpb->sector_size = data[21] | (data[22] << 8);
	xdpb->gap_rw = data[23];
	xdpb->gap_fmt = data[24];
	xdpb->mfm_flags = data[25];
	xdpb->freeze = data[26];

	return 0;
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

		if (read_sector(ctx, track, sector, sector_buf) < 0) break;

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
				strncpy((char *)fi->name, name, CPM_FILENAME_LEN);
				fi->name[CPM_FILENAME_LEN] = '\0';
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

	ctx->sector_size = 512;
	ctx->sectors_per_track = 9;
	ctx->tracks = 40;
	ctx->first_sector = 1;
	ctx->off = 1;
	ctx->drm = 63;
	ctx->spt = 36;

	uint8_t boot_sector[BOOT_SECTOR_SIZE];
	if (read_sector(ctx, 0, 1, boot_sector) == 0) {
		xdpb_t xdpb;
		if (parse_xdpb(boot_sector, &xdpb) == 0) {
			ctx->sector_size = xdpb.sector_size;
			ctx->sectors_per_track = xdpb.sectors_per_track;
			ctx->tracks = xdpb.tracks_per_side;
			ctx->first_sector = xdpb.first_sector;
			ctx->off = xdpb.off;
			ctx->drm = xdpb.drm;
			ctx->spt = xdpb.spt;
		}
	}

	ctx->total_sectors = ctx->tracks * ctx->sectors_per_track;

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
