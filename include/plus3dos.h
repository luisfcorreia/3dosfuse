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

#define IDEDOS_SIGNATURE "PLUSIDEDOS"
#define IDEDOS_HEADER_SIZE 512
#define IDEDOS_PARTITION_NAME_LEN 16
#define IDEDOS_PARTITION_INFO_LEN 32
#define IDEDOS_NUM_PARTITIONS 8
#define IDEDOS_PARTITION_TYPE_P3DOS 3

#define PLUS3DOS_HEADER_MAGIC "PLUS3DOS\x1A"
#define PLUS3DOS_HEADER_SIZE 128

typedef struct {
	uint8_t status;           // Byte 0: status (0xE5=deleted, 0x00=unused, <16=valid)
	uint8_t name[8];          // Bytes 1-8: filename (bit 7 cleared)
	uint8_t ext[3];           // Bytes 9-11: extension (bit 7 cleared)
	uint8_t extent_low;       // Byte 12: extent number (bits 0-4)
	uint8_t bcount;           // Byte 13: bytes in last record
	uint8_t extent_high;      // Byte 14: extent number (bits 5-10)
	uint8_t rcount;           // Byte 15: record count for this extent
	uint8_t al[16];           // Bytes 16-31: block numbers (1 byte each in simple mode)
} __attribute__((packed)) plus3dos_dirent;

typedef struct {
	uint16_t alloc_blocks[8];  // 16-bit block pointers when manyblocks is true
	uint8_t record_count;
} cpm_extent_info;

typedef struct {
	uint8_t name[CPM_FILENAME_LEN + 1];
	uint32_t size;
	cpm_extent_info extents[CPM_MAX_EXTENTS];
	uint8_t num_extents;
	int valid;
	int has_header;           // 1 if file has +3DOS header
	uint32_t header_file_size; // File size from +3DOS header
} cpm_file_info;

typedef struct {
	uint8_t name[IDEDOS_PARTITION_NAME_LEN];
	uint8_t sysinfo[IDEDOS_PARTITION_INFO_LEN];
	uint8_t info[IDEDOS_PARTITION_INFO_LEN];
} __attribute__((packed)) idedos_partition_t;

typedef struct {
	uint32_t start_sector;
	uint32_t total_sectors;
	uint8_t xdpb[28];
} idedos_partition_info_t;

typedef struct {
	uint32_t partition_start;
	uint32_t partition_size;
	uint32_t sector_size;
	uint32_t spt;
	uint32_t drm;
	uint32_t block_size;
	uint16_t dsm;
	uint32_t off;
	uint8_t manyblocks;
	uint8_t name[IDEDOS_PARTITION_NAME_LEN + 1];
	cpm_file_info *files;
	uint32_t num_files;
	uint32_t files_capacity;
} plus3dos_partition;

typedef struct {
	int fd;
	uint32_t total_sectors;
	uint32_t sector_size;
	uint32_t sectors_per_track;
	uint32_t tracks;
	uint32_t first_sector;
	uint32_t off;
	uint8_t bsh;
	uint8_t *alloc_map;
	uint32_t alloc_map_size;
	int is_idedos;
	uint8_t drive_heads;
	uint8_t drive_spt;
	uint32_t partition_start;
	uint32_t block_size;
	uint32_t spt;
	uint32_t drm;
	uint32_t dsm;
	uint8_t manyblocks;
	cpm_file_info *files;
	uint32_t num_files;
	uint32_t files_capacity;
	plus3dos_partition *partitions;
	uint32_t num_partitions;
	uint32_t partitions_capacity;
} plus3dos_ctx;

plus3dos_ctx *plus3dos_init(const char *image_path);
int plus3dos_getattr(const char *path, struct stat *st, struct fuse_file_info *fi);
int plus3dos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags);
int plus3dos_open(const char *path, struct fuse_file_info *fi);
int plus3dos_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
void plus3dos_destroy(plus3dos_ctx *ctx);

#endif
