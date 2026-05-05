#ifndef PLUS3DOS_H
#define PLUS3DOS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif

#include <fuse3/fuse.h>

#define CPM_SECTOR_SIZE 128
#define CPM_DEFAULT_TRACKS 80
#define CPM_DEFAULT_SECTORS_PER_TRACK 18
#define CPM_DIR_ENTRY_SIZE 32
#define CPM_MAX_EXTENTS 32
#define CPM_FILENAME_LEN 12

typedef struct {
	uint8_t name[8];
	uint8_t ext[3];
	uint8_t extent;
	uint8_t record_count;
	uint8_t alloc_blocks[16];
	uint8_t reserved[5];
} __attribute__((packed)) cpm_dirent;

typedef struct {
	uint8_t alloc_blocks[16];
	uint8_t record_count;
} cpm_extent_info;

typedef struct {
	uint8_t name[CPM_FILENAME_LEN + 1];
	uint32_t size;
	cpm_extent_info extents[CPM_MAX_EXTENTS];
	uint8_t num_extents;
	int valid;
} cpm_file_info;

typedef struct {
	int fd;
	uint32_t total_sectors;
	uint32_t sector_size;
	uint32_t sectors_per_track;
	uint32_t tracks;
	uint32_t first_sector;
	uint32_t off;
	uint32_t drm;
	uint32_t spt;
	uint8_t *alloc_map;
	uint32_t alloc_map_size;
	cpm_file_info *files;
	uint32_t num_files;
	uint32_t files_capacity;
} plus3dos_ctx;

plus3dos_ctx *plus3dos_init(const char *image_path);
int plus3dos_getattr(const char *path, struct stat *st, struct fuse_file_info *fi);
int plus3dos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int plus3dos_open(const char *path, struct fuse_file_info *fi);
int plus3dos_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
void plus3dos_destroy(plus3dos_ctx *ctx);

#endif
