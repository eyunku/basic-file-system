#define FUSE_USE_VERSION 30
#include "wfs.h"
#include <fuse.h>
#include <errno.h>
#include <sys/mman.h>

static const char *disk_path = NULL; // absolute path to disk
static char *mapped_disk = NULL; // address of disk

/**
 * Finds the largest inode number in the disk.
 * 
 * Returns:
 *  ulong: the largest inode number in the disk.
*/
static ulong get_largest_inumber() {
    ulong max_inode_number = 0;
    char *current_position = mapped_disk + sizeof(struct wfs_sb);

    while(current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
        struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;
        if (current_entry->inode.inode_number > max_inode_number)
            max_inode_number = current_entry->inode.inode_number;
        current_entry += (sizeof(struct wfs_inode) + current_entry->inode.size);
    }

    return max_inode_number;
}

/**
 * Get the inode number from a given path. Iterates over disk space.
 * 
 * Parameters:
 *  path (const char*): the path, represented as a string.
 * 
 * Returns:
 *  ulong: the inode number associated with the path.
*/
static ulong get_inumber(const char *path) {
    // Start with the root inode number
    ulong current_inode_number = 0;

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
            if (!strcmp(dir_entry->name, token)) {
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

/**
 * Get the live inode associated with the given inode number.
 * 
 * Parameters:
 *  inode_number (uint): inode number of the inode.
 * 
 * Returns:
 *  wfs_inode*: pointer to inode structure associated with inode number.
*/
static struct wfs_inode *get_inode(uint inode_number) {
    char *current_position = mapped_disk + sizeof(struct wfs_sb);
    struct wfs_inode *latest_matching_entry = NULL;
    
    while (current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
        struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;
        if (current_entry->inode.inode_number == inode_number && current_entry->inode.deleted == 0)
            latest_matching_entry = &(current_entry->inode);
        current_position += current_entry->inode.size + sizeof(struct wfs_inode);
    }

    return latest_matching_entry;
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    ulong inode_number = get_inumber(path);
    if (inode_number == -1) return -ENOENT; // Error: File not found

    struct wfs_inode *inode = get_inode(inode_number);
    if (inode == NULL) return -ENOENT; // Error: Inode not found

    // Fill in the struct stat with information from the inode
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atime;
    stbuf->st_mtime = inode->mtime;
    stbuf->st_mode = inode->mode;
    stbuf->st_nlink = inode->links;
    stbuf->st_size = inode->size;

    return 0;
}

static int wfs_mknod(const char *path, mode_t mode, dev_t dev) {
    // If pathname already exists, or is a symbolic link, fail with EEXIST
    if (get_inumber(path) >= 0) return -EEXIST;

    // Create a new log entry for the file
    struct wfs_log_entry *new_log;
    // Set the mode and other attributes based on the provided arguments
    struct wfs_inode inode;
    inode.inode_number = get_largest_inumber() + 1;
    inode.deleted = 0;
    inode.mode = mode;
    inode.uid = getuid();
    inode.gid = getgid();
    inode.flags = 0;
    inode.size = 0;
    inode.atime = time(NULL);
    inode.mtime = time(NULL);
    inode.ctime = time(NULL);
    inode.links = 1;
    new_log->inode = inode;
    // Update the log
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_log, sizeof(*new_log));
    ((struct wfs_sb *)mapped_disk)->head += sizeof(*new_log);
    
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    // If pathname already exists, or is a symbolic link, fail with EEXIST
    if (get_inumber(path) >= 0) return -EEXIST;
    // If mode is not a directory
    if (!S_ISDIR(mode)) return -EISNAM;
    
    // Create a new log entry for the directory
    struct wfs_log_entry *new_log;
    // Set the mode and other attributes based on the provided arguments
    struct wfs_inode inode;
    inode.inode_number = get_largest_inumber() + 1;
    inode.deleted = 0;
    inode.mode = mode;
    inode.uid = getuid();
    inode.gid = getgid();
    inode.flags = 0;
    inode.size = 0;
    inode.atime = time(NULL);
    inode.mtime = time(NULL);
    inode.ctime = time(NULL);
    inode.links = 1;
    new_log->inode = inode;
    // Update the log
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_log, sizeof(*new_log));
    ((struct wfs_sb *)mapped_disk)->head += sizeof(*new_log);
    return 0;
}

// OLD
// static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
//     struct wfs_inode *inode;
//     if (fi && fi->fh) { // file handle provided
//         inode = (struct wfs_inode *)fi->fh;
//         if (inode == NULL || inode->inode_number < 0 || inode->inode_number > get_largest_inumber())
//             return -EBADF;
//     } else {
//         ulong inode_number = get_inumber(path);
//         if (inode_number == -1) return -ENOENT;
        
//         inode = get_inode(inode_number);
//         if (inode == NULL) return -ENOENT;
//     }
//     if (!S_ISREG(inode->mode)) return -EISDIR;

//     // Calculate the maximum number of bytes that can be read
//     size_t max_size = inode->size - offset;
//     size = (size < max_size) ? size : max_size;
//     if (size < 0) return 0;

//     // Copy data from the log entry to the buffer
//     memcpy(buf, ((struct wfs_log_entry *)inode)->data + offset, size);

//     // Update inode metadata since file has been accessed
//     inode->atime = time(NULL);
//     inode->ctime = time(NULL);

//     return size;
// }

// Read data from a file
static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    (void) fi; // Unused parameter

    // Find the inode for the given path using the get_inode helper function
    unsigned long inode_number = get_inumber(path);

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
    size = (size < max_read_size) ? size : max_read_size;

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

    return size; // Return the actual number of bytes read
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode;
    if (fi && fi->fh) { // file handle provided
        inode = (struct wfs_inode *)fi->fh;
        if (inode == NULL || inode->inode_number < 0 || inode->inode_number > get_largest_inumber())
            return -EBADF;
    } else {
        ulong inode_number = get_inumber(path);
        if (inode_number == -1) return -ENOENT;
        
        inode = get_inode(inode_number);
        if (inode == NULL) return -ENOENT;
    }
    if (!S_ISREG(inode->mode)) return -EISDIR;

    // Determine if there's enough space for write
    size_t grow_size = offset + size - inode->size;
    grow_size = (grow_size > 0) ? grow_size : 0;
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + grow_size > mapped_disk + DISK_SIZE) return -ENOSPC;

    // Update inode
    struct wfs_inode new_inode;
    new_inode.inode_number = inode->inode_number;
    new_inode.deleted = inode->deleted;
    new_inode.mode = inode->mode;
    new_inode.uid = inode->uid;
    new_inode.gid = inode->gid;
    new_inode.flags = inode->flags;
    new_inode.size = inode->size + grow_size;
    new_inode.atime = time(NULL);
    new_inode.mtime = time(NULL);
    new_inode.mtime = time(NULL);
    new_inode.links = inode->links;
    // Update data
    char *new_data = calloc(1, size + grow_size);
    memcpy(new_data, ((struct wfs_log_entry *)inode)->data, inode->size);
    memcpy(new_data + offset, buf, size);
    // Create a new log entry for the directory
    struct wfs_log_entry *new_log = calloc(1, sizeof(new_inode) + sizeof(*new_data));
    new_log->inode = new_inode;
    memcpy(new_log->data, new_data, size + grow_size);

    // Add log to disk
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_log, sizeof(*new_log));
    ((struct wfs_sb *)mapped_disk)->head += sizeof(*new_log);

    // Free allocated space
    free(new_data);
    free(new_log);

    return (grow_size > 0) ? 0 : size;
}

// OLD 
// static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
//     struct wfs_inode *inode;
//     if (fi && fi->fh) { // file handle provided
//         inode = (struct wfs_inode *)fi->fh;
//         if (inode == NULL || inode->inode_number < 0 || inode->inode_number > get_largest_inumber())
//             return -EBADF;
//     } else {
//         ulong inode_number = get_inumber(path);
//         if (inode_number == -1) return -ENOENT;
        
//         inode = get_inode(inode_number);
//         if (inode == NULL) return -ENOENT;
//     }
//     if (!S_ISDIR(inode->mode)) return -EISNAM; // Error: Not a directory

//     struct wfs_log_entry *log = (struct wfs_log_entry *)inode;
//     struct wfs_dentry *directory = (struct wfs_dentry *)(log->data + offset);
//     inode->atime = time(NULL);
//     inode->ctime = time(NULL);
//     while ((char*)directory < (char*)log->data + inode->size) {
//         filler(buf, directory->name, NULL, (char*)directory + sizeof(struct wfs_dentry) - mapped_disk);
//         directory += sizeof(struct wfs_dentry);
//     }

//     return 0;
// }

// Read directory entries
static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    (void) offset; // Unused parameter
    (void) fi; // Unused parameter

    // Find the inode for the given path using the get_inode helper function
    unsigned long inode_number = get_inumber(path);

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