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
    
    return 0;
}

static int wfs_mkdir(const char *path, mode_t mode) {
    printf("mkdir called with path: %s\n", path);
    // If pathname already exists, or is a symbolic link, fail with EEXIST
    if (read_path(path) != NULL) return -EEXIST;
    // // If mode is not a directory
    // if (!S_ISDIR(mode)) return -EISNAM;

    printf("creating new log\n");
    // Create a new log entry for the directory
    struct wfs_log_entry *new_log = malloc(sizeof(struct wfs_inode));
    // Set the mode and other attributes based on the provided arguments
    struct wfs_inode inode;
    inode.inode_number = get_largest_inumber() + 1;
    inode.deleted = 0;
    inode.mode = S_IFDIR;
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

    printf("updating parent\n");
    // Update parent
    char name[MAX_FILE_NAME_LEN] = {0};
    char parent_path[MAX_PATH_LEN] = {0};
    printf("parsing path %s\n", path);
    parsepath(name, parent_path, path);
    printf("path parsed to be name: %s, parent path: %s\n", name, parent_path);
    // Create directory entry for new directory
    struct wfs_dentry *new_dentry = malloc(sizeof(struct wfs_dentry));
    strcpy(new_dentry->name, name);
    new_dentry->inode_number = inode.inode_number;
    printf("inode number: %ld, name: %s\n", new_dentry->inode_number, new_dentry->name);
    // Get existing parent inode
    printf("reading path %s\n", parent_path);
    struct wfs_inode *parent_inode = read_path(parent_path);
    if (parent_inode == NULL) return -ENOENT;
    printf("parsepath successful\n");
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
    printf("parent inode initialized\n");
    // Update data
    char *data = malloc(new_parent_inode.size);
    memcpy(data, parent_log->data, parent_log->inode.size);
    memcpy(data + parent_log->inode.size, new_dentry, sizeof(*new_dentry));
    for (struct wfs_dentry *d = (struct wfs_dentry *)data; d < (struct wfs_dentry *)data + new_parent_inode.size; d+=sizeof(struct wfs_dentry)) {
        printf("current entry name: %s, number %ld\n", d->name, d->inode_number);
    }
    printf("data updated\n");
    // Create new log entry for parent
    struct wfs_log_entry *new_parent_log = malloc(sizeof(new_parent_inode) + new_parent_inode.size);
    new_parent_log->inode = new_parent_inode;
    memcpy(new_parent_log->data, data, new_parent_inode.size);
    printf("new parent log inode: %d\n", new_parent_log->inode.inode_number);
    for (struct wfs_dentry *d = (struct wfs_dentry *)new_parent_log->data; d < (struct wfs_dentry *)new_parent_log->data + new_parent_inode.size; d+=sizeof(struct wfs_dentry)) {
        printf("current entry name: %s, number %ld\n", d->name, d->inode_number);
    }
    printf("new log entry copied\n");
    // Update the log
    printf("attempting to copy to disk\n");
    printf("mapped disk location %p\n", mapped_disk);
    printf("current head: %p, new head: %p, max head: %p\n", mapped_disk + ((struct wfs_sb *)mapped_disk)->head, mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_parent_log), mapped_disk + DISK_SIZE);
    if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*new_parent_log) > mapped_disk + DISK_SIZE) return -ENOSPC;
    memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_parent_log, sizeof(new_parent_inode) + new_parent_inode.size);
    ((struct wfs_sb *)mapped_disk)->head += sizeof(new_parent_inode) + new_parent_inode.size;

    // Free allocated space
    free(new_log);
    free(data);
    free(new_parent_log);

    printf("mkdir finished successfully??\n");
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
    new_inode.mtime = time(NULL);
    new_inode.links = inode->links;
    // Update data
    char *new_data = malloc(size + grow_size);
    memcpy(new_data, ((struct wfs_log_entry *)inode)->data, inode->size);
    memcpy(new_data + offset, buf, size);
    // Create a new log entry for the directory
    struct wfs_log_entry *new_log = malloc(sizeof(new_inode) + sizeof(*new_data));
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
    // struct wfs_inode *inode = read_path(path);
    // if (inode == NULL) return -ENOENT;

    // // Mark the entry as deleted in the log
    // struct wfs_log_entry *unlink_log;
    // unlink_log = malloc(sizeof(struct wfs_inode));
    // unlink_log->inode = *inode;
    // unlink_log->inode.deleted = 1;
    // unlink_log->inode.size = 0; // No need to copy old data into new deleted log entry


    // // Update the log
    // if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head > mapped_disk + DISK_SIZE) {
    //     free(unlink_log);
    //     return -ENOSPC;
    // }

    // memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, unlink_log, sizeof(struct wfs_inode));
    // ((struct wfs_sb *)mapped_disk)->head += sizeof(struct wfs_inode);

    // free(unlink_log);

    // char *path_copy = strdup(path);
    // char base_name[MAX_FILE_NAME_LEN]; char dir_name[MAX_PATH_LEN];
    // parsepath(base_name, dir_name, path_copy);
    // free(path_copy);

    // ulong parent_inode_number = get_inumber(dir_name);
    // if (parent_inode_number == -1) return -ENOENT; // Error: Parent directory not found

  
    // struct wfs_log_entry *parent_log_entry = (struct wfs_log_entry *)get_inode(parent_inode_number);
    // struct wfs_dentry *parent_dir_entry = (struct wfs_dentry *)parent_log_entry->data;
    
    // // Make a log entry for the new parent
    // struct wfs_log_entry *new_parent_log_entry;
    // new_parent_log_entry = malloc(sizeof(struct wfs_inode) + parent_log_entry->inode.size - sizeof(struct wfs_dentry));
    // new_parent_log_entry->inode = parent_log_entry->inode;
    // new_parent_log_entry->inode.size = parent_log_entry->inode.size - sizeof(struct wfs_dentry);
    // int directory_offset = 0;
    // while (directory_offset < parent_log_entry->inode.size) {
    //     // Copy over all entries except the deleted one
    //     if (!strcmp(parent_dir_entry->name, base_name)) {
    //         memcpy(new_parent_log_entry->data, parent_dir_entry, sizeof(struct wfs_dentry));
    //     }
    //     directory_offset += sizeof(struct wfs_dentry);
    //     parent_dir_entry++;
    // }

    // // Write the new parent to disk
    // memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, new_parent_log_entry, sizeof(struct wfs_inode) + parent_log_entry->inode.size - sizeof(struct wfs_dentry));
    // ((struct wfs_sb *)mapped_disk)->head += sizeof(struct wfs_inode) + parent_log_entry->inode.size - sizeof(struct wfs_dentry);

    // free(new_parent_log_entry);

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
    // printf("running test on parsepath\n");
    // char name[100]; char parent_path[100];
    // char test_path[11] = "/a";
    // parsepath(name, parent_path, test_path);
    // printf("parses to name: %s, parent path: %s\n", name, parent_path);

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
