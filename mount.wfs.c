#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "wfs.h"

static const char *disk_path = NULL;
static const char *mount_point = NULL;

static int wfs_getattr(const char *path, struct stat *stbuf) {
    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return 0;
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    return size;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    return 0;
}

static int wfs_unlink(const char *path) {
    return 0;
}

static struct fuse_operations wfs_ops = {
    .getattr    = wfs_getattr,
    .mknod      = wfs_mknod,
    .mkdir      = wfs_mkdir,
    .read       = wfs_read,
    .write      = wfs_write,
    .readdir    = wfs_readdir,
    .unlink     = wfs_unlink,
};

int main(int argc, char *argv[]) {
    if (argc < 3 || argv[argc - 2][0] == '-' || argv[argc - 1][0] == '-') {
        fprintf(stderr, "Usage: %s [FUSE options] disk_path mount_point\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Get the disk_path and mount_point
    disk_path = realpath(argv[argc - 2], NULL);
    mount_point = argv[argc - 1];

    // Set up FUSE-specific arguments
    argv[argc - 2] = argv[argc - 1];
    argv[argc - 1] = NULL;
    --argc;

    // Initialize FUSE with specified operations
    return fuse_main(argc, argv, &wfs_ops, NULL);
}