#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "plus3dos.h"

static plus3dos_ctx *fs_ctx = NULL;

static int plus3fuse_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
	return plus3dos_getattr(path, st, fi);
}

static int plus3fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	return plus3dos_readdir(path, buf, filler, offset, fi, flags);
}

static int plus3fuse_open(const char *path, struct fuse_file_info *fi) {
	return plus3dos_open(path, fi);
}

static int plus3fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	return plus3dos_read(path, buf, size, offset, fi);
}

static void *plus3fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
	(void) conn;
	cfg->use_ino = 1;
	return fs_ctx;
}

static void plus3fuse_destroy(void *private_data) {
	(void) private_data;
	plus3dos_destroy(fs_ctx);
	fs_ctx = NULL;
}

static struct fuse_operations plus3fuse_ops = {
	.getattr = plus3fuse_getattr,
	.readdir = plus3fuse_readdir,
	.open = plus3fuse_open,
	.read = plus3fuse_read,
	.init = plus3fuse_init,
	.destroy = plus3fuse_destroy,
};

int main(int argc, char *argv[]) {
	if (argc < 3) {
		fprintf(stderr, "Usage: %s <disk_image> <mount_point> [fuse_options...]\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *image_path = argv[1];
	fs_ctx = plus3dos_init(image_path);
	if (!fs_ctx) {
		fprintf(stderr, "Failed to initialize CP/M filesystem from %s: %s\n", image_path, strerror(errno));
		return EXIT_FAILURE;
	}

	argv[1] = argv[0];
	argc--;
	argv++;

	return fuse_main(argc, argv, &plus3fuse_ops, NULL);
}
