#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include "wfs.h"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>

static const char *disk_path = NULL;
static char *mapped_disk = NULL;

// Helper function to get inode number from path
static unsigned long get_inode_number_from_path(const char *path) {
    // Start with the root inode number
    unsigned long current_inode_number = 0;

    // Make a copy of the path since strtok modifies the string
    char path_copy[strlen(path) + 1];
    strcpy(path_copy, path);

    // Tokenize the path using '/'
    char *token = strtok(path_copy, "/");
    int found = 1; // This will be reset to 0 unless it is the root directory.
    while (token != NULL) {
        found = 0;
        // Iterate through the log entries until we reach the head
        char *current_position = mapped_disk + sizeof(struct wfs_sb);
        struct wfs_log_entry *latest_matching_entry;
        while (current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
            // Convert the current position to a wfs_log_entry pointer
            struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;

            // Check if the current entry has the desired inode number
            if (S_ISDIR(current_entry->inode.mode) && current_entry->inode.inode_number == current_inode_number && current_entry->inode.deleted == 0) {
                latest_matching_entry = current_entry;
            }

            // Move to the next log entry
            current_position += current_entry->inode.size + sizeof(struct wfs_inode);
        }
        // Found the inode, return a pointer to it
        struct wfs_dentry *dir_entry = (struct wfs_dentry *)latest_matching_entry->data;
        int directory_offset = 0;
        while (directory_offset < latest_matching_entry->inode.size) {
            if (strcmp(dir_entry->name, token) == 0) {
                found = 1;
                current_inode_number = dir_entry->inode_number;
                break;
            }
            // Move to the next directory entry
            directory_offset += sizeof(struct wfs_dentry);  // Corrected line
            dir_entry++;  // Corrected line
        }
        // Get the next token
        token = strtok(NULL, "/");

    }
    if(!found) {
        return -1;
    }

    return current_inode_number;
}

// Helper function to find inode from inode number
static struct wfs_inode *get_inode(unsigned int inode_number) {
    // Start at the end of the superblock
    char *current_position = mapped_disk + sizeof(struct wfs_sb);
    struct wfs_inode *latest_matching_entry = NULL;
    // Iterate through the log entries until we reach the head
    while (current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
        // Convert the current position to a wfs_log_entry pointer
        struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;

        // Check if the current entry has the desired inode number
        if (current_entry->inode.inode_number == inode_number && current_entry->inode.deleted == 0) {
            // Found the inode, return a pointer to it
            latest_matching_entry = &(current_entry->inode);
        }

        // Move to the next log entry
        current_position += current_entry->inode.size + sizeof(struct wfs_inode);
    }

    // Inode not found
    return latest_matching_entry;
}

// Get attributes of a file
static int wfs_getattr(const char *path, struct stat *stbuf) {
    // Find the inode for the given path using the get_inode helper function
    unsigned long inode_number = get_inode_number_from_path(path);

    if (inode_number == -1) {
        return -ENOENT; // Error: File not found
    }

    struct wfs_inode *inode = get_inode(inode_number);

    if (inode == NULL) {
        return -ENOENT; // Error: Inode not found
    }

    // Fill in the struct stat with information from the inode
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atime;
    stbuf->st_mtime = inode->mtime;
    stbuf->st_mode = inode->mode;
    stbuf->st_nlink = inode->links;
    stbuf->st_size = inode->size;

    return 0; // Success
}

// Create a new file node (e.g., regular file, device, etc.)
static int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    // Create a new log entry for the file
    // Set the mode and other attributes based on the provided arguments
    // Update the log
    return 0;
}

// Create a new directory
static int wfs_mkdir(const char *path, mode_t mode) {
    // Create a new log entry for the directory
    // Set the mode and other attributes based on the provided arguments
    // Update the log
    return 0;
}

// Read data from a file
static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi; // Unused parameter

    // Find the inode for the given path using the get_inode helper function
    unsigned long inode_number = get_inode_number_from_path(path);

    if (inode_number == -1) {
        return -ENOENT; // Error: File not found
    }

    struct wfs_inode *inode = get_inode(inode_number);

    if (inode == NULL) {
        return -ENOENT; // Error: Inode not found
    }

    // Check if the inode is a regular file
    if (!S_ISREG(inode->mode)) {
        return -EISDIR; // Error: Not a regular file
    }

    // Make sure that the read offset is within the file size
    if (offset >= inode->size) {
        return 0; // End of file reached
    }

    // Calculate the maximum number of bytes that can be read
    size_t max_read_size = inode->size - offset;
    read_size = (size < max_read_size) ? size : max_read_size;

    // Find the log entry for the specified inode
    char *current_position = mapped_disk + sizeof(struct wfs_sb);
    struct wfs_log_entry *latest_matching_entry;
    while (current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
        // Convert the current position to a wfs_log_entry pointer
        struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;

        // Check if the current entry has the desired inode number
        if (S_ISREG(current_entry->inode.mode) && current_entry->inode.inode_number == inode_number && current_entry->inode.deleted == 0) {
            latest_matching_entry = current_entry;
        }

        // Move to the next log entry
        current_position += current_entry->inode.size + sizeof(struct wfs_inode);
    }

    // Copy data from the log entry to the buffer
    memcpy(buf, latest_matching_entry->data + offset, size);

    return read_size; // Return the actual number of bytes read
}

// Write data to a file
static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    // Use the path to find the corresponding entry in the log
    // Write data to the log entry starting from the specified offset
    // Update the log
    return size;
}

// Read directory entries
static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    (void) offset; // Unused parameter
    (void) fi; // Unused parameter

    // Find the inode for the given path using the get_inode helper function
    unsigned long inode_number = get_inode_number_from_path(path);

    if (inode_number == -1) {
        return -ENOENT; // Error: File not found
    }

    struct wfs_inode *inode = get_inode(inode_number);

    if (inode == NULL) {
        return -ENOENT; // Error: Inode not found
    }

    // Check if the inode is a directory
    if (!S_ISDIR(inode->mode)) {
        return -ENOTDIR; // Error: Not a directory
    }
    // Find the log entry for the specified inode
    char *current_position = mapped_disk + sizeof(struct wfs_sb);
    struct wfs_log_entry *latest_matching_entry;
    while (current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
        // Convert the current position to a wfs_log_entry pointer
        struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;

        // Check if the current entry has the desired inode number
        if (S_ISDIR(current_entry->inode.mode) && current_entry->inode.inode_number == inode_number && current_entry->inode.deleted == 0) {
            latest_matching_entry = current_entry;
        }

        // Move to the next log entry
        current_position += current_entry->inode.size + sizeof(struct wfs_inode);
    }

    // Look through the directory entries to find the filenames
    struct wfs_dentry *dir_entry = (struct wfs_dentry *)latest_matching_entry->data;
    int directory_offset = 0;
    while (directory_offset < latest_matching_entry->inode.size) {
        // Use the filler function to provide directory entries to FUSE
        filler(buf, dir_entry->name, NULL, 0);
        // Move to the next directory entry
        directory_offset += sizeof(struct wfs_dentry);
        dir_entry++;
    }
    return 0;
}

// Remove a file
static int wfs_unlink(const char *path) {
    // Use the path to find the corresponding entry in the log
    // Mark the entry as deleted in the log
    // Update the log
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

    // Open the disk file
    int fd = open(disk_path, O_RDONLY);
    if (fd == -1) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    // Map the entire disk into memory
    struct stat sb;
    if (fstat(fd, &sb) == -1) {
        perror("Error getting file size");
        close(fd);
        exit(EXIT_FAILURE);
    }

    mapped_disk = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped_disk == MAP_FAILED) {
        perror("Error mapping file into memory");
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Close the file
    close(fd);

    // Set up FUSE-specific arguments
    argv[argc - 2] = argv[argc - 1];
    argv[argc - 1] = NULL;
    --argc;

    // Initialize FUSE with specified operations
    int fuse_ret = fuse_main(argc, argv, &wfs_ops, NULL);

    // Unmap the memory
    munmap(mapped_disk, sb.st_size);

    return fuse_ret;
}