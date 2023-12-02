#include "wfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int init_filesystem(const char *path) {
    // Open the file for read-write, create if not exists, truncate to zero length
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("Error opening file");
        return -1;
    }

    // Initialize the superblock
    struct wfs_sb superblock = {
        .magic = WFS_MAGIC,
        .head = (sizeof(struct wfs_sb)+sizeof(struct wfs_log_entry)) // Start of the next available space
    };

    // Write the superblock to the file
    if (write(fd, &superblock, sizeof(struct wfs_sb)) == -1) {
        perror("Error writing superblock");
        close(fd);
        return -1;
    }

    // Create the root log entry
    struct wfs_log_entry root_entry = {
        .inode = {
            .inode_number = 0,  // Root has inode number 0
            .deleted = 0,
            .mode = S_IFDIR,    // Root is a directory
            .flags = 0,
            .size = 0,
            // TODO: Set other inode fields
        }
    };

    // Write the root log entry to the file
    if (write(fd, &root_entry, sizeof(struct wfs_log_entry)) == -1) {
        perror("Error writing root log entry");
        close(fd);
        return -1;
    }

    // Close the file
    close(fd);

    printf("Filesystem initialized successfully at %s\n", path);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_path>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *disk_path = argv[1];

    // Initialize the filesystem
    if (init_filesystem(disk_path) == -1) {
        fprintf(stderr, "Failed to initialize filesystem.\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}
