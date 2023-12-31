#define FUSE_USE_VERSION 30
#include "wfs.h"
#include <fuse.h>
#include <errno.h>
#include <sys/mman.h>

static const char *disk_path = NULL; // absolute path to disk
static char *mapped_disk = NULL; // address of disk

/**
 * Given a path, gets the basename (name of the file or directory), and the path to the
 * parent directory. Passing NULL into basename or dirname means that buffer will be ignored.
 * 
 * Parameters:
 *  basename (char*): buffer to store the name of file or directory.
 *  dirname (char*): buffer to store path to parent directory.
 *  path (const char*): absolute path to the file or directory.
 * 
 * Returns:
 *  int: 0 on success, -1 on failure.
*/
int parsepath(char* basename, char* dirname, const char* path) {
    char path_copy[strlen(path) + 1];
    strcpy(path_copy, path);
    char *token = strtok(path_copy, "/");
    while (token != NULL) {
        char *nexttok = strtok(NULL, "/");
        if (nexttok == NULL) {
            if (basename != NULL) {
                if (strcpy(basename, token) == NULL) return -1;
            }
            break;
        }
        if (dirname != NULL) {
            if (strcat(dirname, token) == NULL) return -1;
            if (strcat(dirname, "/") == NULL) return -1;
        }
        token = nexttok;
    }
    return 0;
}

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
        current_position += (sizeof(struct wfs_inode) + current_entry->inode.size);
    }

    return max_inode_number;
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
static struct wfs_inode *read_inumber(uint inode_number) {
    char *current_position = mapped_disk + sizeof(struct wfs_sb);
    struct wfs_inode *latest_matching_entry = NULL;
    
    while (current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
        struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;
        if (current_entry->inode.inode_number == inode_number)
            latest_matching_entry = &(current_entry->inode);
        current_position += current_entry->inode.size + sizeof(struct wfs_inode);
    }

    return latest_matching_entry;
}

/**
 * Get the live inode associated with a given path. Iterates over disk space.
 * 
 * Parameters:
 *  path (const char*): the path, represented as a string.
 * 
 * Returns:
 *  wfs_inode*: pointer to inode structure associated with path.
*/
static struct wfs_inode *read_path(const char *path) {
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
        struct wfs_log_entry *latest_matching_entry = (struct wfs_log_entry *)read_inumber(current_inode_number);
        if(!S_ISDIR(latest_matching_entry->inode.mode)) return NULL;
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
            directory_offset += sizeof(struct wfs_dentry); // Corrected line
            dir_entry++; // Corrected line
        }
        // Get the next token
        token = strtok(NULL, "/");
    }
    if(!found)
        return NULL;

    return read_inumber(current_inode_number);
}

static int wfs_getattr(const char *path, struct stat *stbuf) {
    struct wfs_inode *inode = read_path(path);
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
    if (read_path(path) != NULL) return -EEXIST;

    // Create a new log entry for the file
    struct wfs_log_entry *new_log = malloc(sizeof(struct wfs_inode));

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
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_log) > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_log, sizeof(*new_log));
    ((struct wfs_sb *)mapped_disk)->head += sizeof(*new_log);

    free(new_log);

    // Update parent
    char name[MAX_FILE_NAME_LEN] = {0};
    char parent_path[MAX_PATH_LEN] = {0};
    parsepath(name, parent_path, path);

    // Create directory entry for new file
    struct wfs_dentry *new_dentry = malloc(sizeof(struct wfs_dentry));
    strcpy(new_dentry->name, name);
    new_dentry->inode_number = inode.inode_number;

    // Get existing parent inode
    struct wfs_inode *parent_inode = read_path(parent_path);
    if (parent_inode == NULL) return -ENOENT;

    struct wfs_log_entry *parent_log = (struct wfs_log_entry *)parent_inode;

    // Create new inode entry for parent
    struct wfs_inode new_parent_inode;
    new_parent_inode.inode_number = parent_log->inode.inode_number;
    new_parent_inode.deleted = 0;
    new_parent_inode.mode = parent_log->inode.mode;
    new_parent_inode.uid = parent_log->inode.uid;
    new_parent_inode.gid = parent_log->inode.gid;
    new_parent_inode.flags = parent_log->inode.flags;
    new_parent_inode.size = parent_log->inode.size + sizeof(*new_dentry);
    new_parent_inode.atime = time(NULL);
    new_parent_inode.mtime = time(NULL);
    new_parent_inode.ctime = time(NULL);
    new_parent_inode.links = parent_log->inode.links;

    // Update data
    char *data = malloc(new_parent_inode.size);
    memcpy(data, parent_log->data, parent_log->inode.size);
    memcpy(data + parent_log->inode.size, new_dentry, sizeof(*new_dentry));

    // Create new log entry for parent
    struct wfs_log_entry *new_parent_log = malloc(sizeof(new_parent_inode) + new_parent_inode.size);
    new_parent_log->inode = new_parent_inode;
    memcpy(new_parent_log->data, data, new_parent_inode.size);

    // Update the log
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_parent_log) > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_parent_log, sizeof(new_parent_inode) + new_parent_inode.size);
    ((struct wfs_sb *)mapped_disk)->head += sizeof(new_parent_inode) + new_parent_inode.size;

    // Free allocated space
    free(new_dentry);
    free(data);
    free(new_parent_log);

    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    // If pathname already exists, or is a symbolic link, fail with EEXIST
    if (read_path(path) != NULL) return -EEXIST;

    // Create a new log entry for the directory
    struct wfs_log_entry *new_log = malloc(sizeof(struct wfs_inode));

    // Set the mode and other attributes based on the provided arguments
    struct wfs_inode inode;
    inode.inode_number = get_largest_inumber() + 1;
    inode.deleted = 0;
    inode.mode = S_IFDIR | mode;
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
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_log) > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_log, sizeof(*new_log));
    ((struct wfs_sb *)mapped_disk)->head += sizeof(*new_log);

    free(new_log);

    // Update parent
    char name[MAX_FILE_NAME_LEN] = {0};
    char parent_path[MAX_PATH_LEN] = {0};
    parsepath(name, parent_path, path);

    // Create directory entry for new directory
    struct wfs_dentry *new_dentry = malloc(sizeof(struct wfs_dentry));
    strcpy(new_dentry->name, name);
    new_dentry->inode_number = inode.inode_number;

    // Get existing parent inode
    struct wfs_inode *parent_inode = read_path(parent_path);
    if (parent_inode == NULL) return -ENOENT;

    struct wfs_log_entry *parent_log = (struct wfs_log_entry *)parent_inode;

    // Create new inode entry for parent
    struct wfs_inode new_parent_inode;
    new_parent_inode.inode_number = parent_log->inode.inode_number;
    new_parent_inode.deleted = 0;
    new_parent_inode.mode = parent_log->inode.mode;
    new_parent_inode.uid = parent_log->inode.uid;
    new_parent_inode.gid = parent_log->inode.gid;
    new_parent_inode.flags = parent_log->inode.flags;
    new_parent_inode.size = parent_log->inode.size + sizeof(*new_dentry);
    new_parent_inode.atime = time(NULL);
    new_parent_inode.mtime = time(NULL);
    new_parent_inode.ctime = time(NULL);
    new_parent_inode.links = parent_log->inode.links;

    // Update data
    char *data = malloc(new_parent_inode.size);
    memcpy(data, parent_log->data, parent_log->inode.size);
    memcpy(data + parent_log->inode.size, new_dentry, sizeof(*new_dentry));

    // Create new log entry for parent
    struct wfs_log_entry *new_parent_log = malloc(sizeof(new_parent_inode) + new_parent_inode.size);
    new_parent_log->inode = new_parent_inode;
    memcpy(new_parent_log->data, data, new_parent_inode.size);

    // Update the log
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_parent_log) > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_parent_log, sizeof(new_parent_inode) + new_parent_inode.size);
    ((struct wfs_sb *)mapped_disk)->head += sizeof(new_parent_inode) + new_parent_inode.size;

    // Free allocated space
    free(new_dentry);
    free(data);
    free(new_parent_log);

    return 0;
}

static int wfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode;
    if (fi && fi->fh) { // file handle provided
        inode = (struct wfs_inode *)fi->fh;
        if (inode == NULL || inode->inode_number < 0 || inode->inode_number > get_largest_inumber())
            return -EBADF;
    } else {
        inode = read_path(path);
        if (inode == NULL) return -ENOENT;
    }
    if (!S_ISREG(inode->mode)) return -EISDIR;

    // Calculate the maximum number of bytes that can be read
    size_t max_size = inode->size - offset;
    size = (size < max_size) ? size : max_size;
    if (size < 0) return 0;

    // Copy data from the log entry to the buffer
    memcpy(buf, ((struct wfs_log_entry *)inode)->data + offset, size);

    // Update inode metadata since file has been accessed
    uint current_time = time(NULL);
    memcpy(&(inode->atime), &(current_time), sizeof(current_time));
    memcpy(&(inode->ctime), &(current_time), sizeof(current_time));

    return size; // Return the actual number of bytes read
}

static int wfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode;
    if (fi && fi->fh) { // file handle provided
        inode = (struct wfs_inode *)fi->fh;
        if (inode == NULL || inode->inode_number < 0 || inode->inode_number > get_largest_inumber())
            return -EBADF;
    } else {
        inode = read_path(path);
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
    new_inode.ctime = time(NULL);
    new_inode.links = inode->links;

    // Update data
    char *new_data = malloc(new_inode.size);
    if (!new_data) return -ENOMEM;

    // Copy existing data to the new buffer
    memcpy(new_data, ((struct wfs_log_entry *)inode)->data, inode->size);

    // Copy the new data to the appropriate offset
    memcpy(new_data + offset, buf, size);

    // Create a new log entry for the updated file
    struct wfs_log_entry *new_log = malloc(sizeof(new_inode) + new_inode.size);
    new_log->inode = new_inode;
    memcpy(new_log->data, new_data, new_inode.size);
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_log) + new_inode.size> mapped_disk + DISK_SIZE) {
        free(new_data);
        free(new_log);
        return -ENOSPC;
    }

    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_log, sizeof(*new_log) + new_inode.size);
    ((struct wfs_sb *)mapped_disk)->head += sizeof(*new_log) + new_inode.size;

    // Free allocated space
    free(new_data);
    free(new_log);

    return size;
}

static int wfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    struct wfs_inode *inode;
    if (fi && fi->fh) { // file handle provided
        inode = (struct wfs_inode *)fi->fh;
        if (inode == NULL || inode->inode_number < 0 || inode->inode_number > get_largest_inumber())
            return -EBADF;
    } else {
        inode = read_path(path);
        if (inode == NULL) return -ENOENT;
    }
    if (!S_ISDIR(inode->mode)) return -EISNAM; // Error: Not a directory
    
    // Look through the directory entries to find the filenames
    struct wfs_log_entry *log = (struct wfs_log_entry *)inode;
    struct wfs_dentry *dir_entry = (struct wfs_dentry *)(log->data + offset);
    int directory_offset = 0;
    uint current_time = time(NULL);
    memcpy(&(inode->atime), &(current_time), sizeof(current_time));
    memcpy(&(inode->ctime), &(current_time), sizeof(current_time));
    while (directory_offset < inode->size) {
        // Use the filler function to provide directory entries to FUSE
        filler(buf, dir_entry->name, NULL, 0);
        // Move to the next directory entry
        directory_offset += sizeof(struct wfs_dentry);
        dir_entry++;
    }
    return 0;
}

static int wfs_unlink(const char *path) {
    struct wfs_inode *unlink_inode = read_path(path);

    unlink_inode->links--;
    if (unlink_inode->links == 0)
        unlink_inode->deleted = 1;

    // Update parent
    char unlink_name[MAX_FILE_NAME_LEN] = {0};
    char parent_path[MAX_PATH_LEN] = {0};
    parsepath(unlink_name, parent_path, path);

    // Get existing parent inode
    struct wfs_inode *parent_inode = read_path(parent_path);
    if (parent_inode == NULL) return -ENOENT;

    struct wfs_log_entry *parent_log = (struct wfs_log_entry *)parent_inode;

    // Create new inode entry for parent
    struct wfs_inode new_parent_inode;
    new_parent_inode.inode_number = parent_log->inode.inode_number;
    new_parent_inode.deleted = 0;
    new_parent_inode.mode = parent_log->inode.mode;
    new_parent_inode.uid = parent_log->inode.uid;
    new_parent_inode.gid = parent_log->inode.gid;
    new_parent_inode.flags = parent_log->inode.flags;
    new_parent_inode.size = parent_log->inode.size - sizeof(struct wfs_dentry);
    new_parent_inode.atime = time(NULL);
    new_parent_inode.mtime = time(NULL);
    new_parent_inode.ctime = time(NULL);
    new_parent_inode.links = parent_log->inode.links;

    // Update data
    char *data = malloc(new_parent_inode.size);
    int data_position = 0;
    for (struct wfs_dentry *dentry = (struct wfs_dentry *)parent_log->data; (char*)dentry < (char*)parent_log->data + parent_log->inode.size; dentry++) {
        if (!strcmp(dentry->name, unlink_name))
            continue;
        memcpy(data + data_position, dentry, sizeof(struct wfs_dentry));
        data_position += sizeof(struct wfs_dentry);
    }

    // Create new log entry for parent
    struct wfs_log_entry *new_parent_log = malloc(sizeof(new_parent_inode) + new_parent_inode.size);
    new_parent_log->inode = new_parent_inode;
    memcpy(new_parent_log->data, data, new_parent_inode.size);

    // Update the log
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_parent_log) > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_parent_log, sizeof(new_parent_inode) + new_parent_inode.size);
    ((struct wfs_sb *)mapped_disk)->head += sizeof(new_parent_inode) + new_parent_inode.size;

    // Free allocated space
    free(data);
    free(new_parent_log);

    return 0;
}

static int wfs_rmdir(const char *path) {
    struct wfs_inode *unlink_inode = read_path(path);

    unlink_inode->links--;
    if (unlink_inode->links == 0)
        unlink_inode->deleted = 1;

    // Update parent
    char unlink_name[MAX_FILE_NAME_LEN] = {0};
    char parent_path[MAX_PATH_LEN] = {0};
    parsepath(unlink_name, parent_path, path);

    // Get existing parent inode
    struct wfs_inode *parent_inode = read_path(parent_path);
    if (parent_inode == NULL) return -ENOENT;

    struct wfs_log_entry *parent_log = (struct wfs_log_entry *)parent_inode;

    // Create new inode entry for parent
    struct wfs_inode new_parent_inode;
    new_parent_inode.inode_number = parent_log->inode.inode_number;
    new_parent_inode.deleted = 0;
    new_parent_inode.mode = parent_log->inode.mode;
    new_parent_inode.uid = parent_log->inode.uid;
    new_parent_inode.gid = parent_log->inode.gid;
    new_parent_inode.flags = parent_log->inode.flags;
    new_parent_inode.size = parent_log->inode.size - sizeof(struct wfs_dentry);
    new_parent_inode.atime = time(NULL);
    new_parent_inode.mtime = time(NULL);
    new_parent_inode.ctime = time(NULL);
    new_parent_inode.links = parent_log->inode.links;

    // Update data
    char *data = malloc(new_parent_inode.size);
    int data_position = 0;
    for (struct wfs_dentry *dentry = (struct wfs_dentry *)parent_log->data; (char*)dentry < (char*)parent_log->data + parent_log->inode.size; dentry++) {
        if (!strcmp(dentry->name, unlink_name))
            continue;
        memcpy(data + data_position, dentry, sizeof(struct wfs_dentry));
        data_position += sizeof(struct wfs_dentry);
    }

    // Create new log entry for parent
    struct wfs_log_entry *new_parent_log = malloc(sizeof(new_parent_inode) + new_parent_inode.size);
    new_parent_log->inode = new_parent_inode;
    memcpy(new_parent_log->data, data, new_parent_inode.size);

    // Update the log
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_parent_log) > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_parent_log, sizeof(new_parent_inode) + new_parent_inode.size);
    ((struct wfs_sb *)mapped_disk)->head += sizeof(new_parent_inode) + new_parent_inode.size;

    // Free allocated space
    free(data);
    free(new_parent_log);

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
    .rmdir      = wfs_rmdir,
};

int main(int argc, char *argv[]) {
    if (argc < 3 || argv[argc - 2][0] == '-' || argv[argc - 1][0] == '-') {
        fprintf(stderr, "Usage: %s [FUSE options] disk_path mount_point\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Get the disk_path and mount_point
    disk_path = realpath(argv[argc - 2], NULL);

    // Open the disk file
    int fd = open(disk_path, O_RDWR);
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

    mapped_disk = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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
