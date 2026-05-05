#include "plus3dos.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

plus3dos_ctx *plus3dos_init(const char *image_path) {
	plus3dos_ctx *ctx = malloc(sizeof(plus3dos_ctx));
	if (!ctx) return NULL;

	ctx->fd = open(image_path, O_RDONLY);
	if (ctx->fd < 0) {
		free(ctx);
		return NULL;
	}

	ctx->total_sectors = 0;
	ctx->dir_start = 0;
	ctx->dir_size = 0;
	ctx->alloc_map = NULL;

	return ctx;
}

int plus3dos_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
	return -ENOSYS;
}

int plus3dos_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	return -ENOSYS;
}

int plus3dos_open(const char *path, struct fuse_file_info *fi) {
	return -ENOSYS;
}

int plus3dos_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	return -ENOSYS;
}

void plus3dos_destroy(plus3dos_ctx *ctx) {
	if (ctx) {
		if (ctx->fd >= 0) close(ctx->fd);
		free(ctx->alloc_map);
		free(ctx);
	}
}
