#define _POSIX_C_SOURCE 200809L
#include "wfs.h"
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>


static char *mapped_disk = NULL; // address of disk

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

static int fsck() {
    struct wfs_sb superblock = {
        .magic = WFS_MAGIC,
        .head = sizeof(struct wfs_sb),
    };
    memcpy(mapped_disk, &superblock, sizeof(superblock));

    for(ulong inode_number = 0; inode_number < get_largest_inumber(); inode_number++) {
        char *current_position = mapped_disk + sizeof(struct wfs_sb);
        struct wfs_inode *latest_matching_entry = NULL;
        
        while (current_position < mapped_disk + ((struct wfs_sb *)mapped_disk)->head) {
            struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;
            if (current_entry->inode.inode_number == inode_number)
                latest_matching_entry = &(current_entry->inode);
            current_position += current_entry->inode.size + sizeof(struct wfs_inode);
        }
        
        // Update the log
        if (mapped_disk + ((struct wfs_sb *)mapped_disk)->head + sizeof(*latest_matching_entry) + latest_matching_entry->size> mapped_disk + DISK_SIZE) return -ENOSPC;
        memcpy(mapped_disk + ((struct wfs_sb *)mapped_disk)->head, (struct wfs_log_entry *) latest_matching_entry, sizeof(*latest_matching_entry) + latest_matching_entry->size);
        ((struct wfs_sb *)mapped_disk)->head += sizeof(*latest_matching_entry) + latest_matching_entry->size;
    }
    // clear the rest of the file
    for (char* p = mapped_disk + ((struct wfs_sb *)mapped_disk)->head; p < mapped_disk + DISK_SIZE; p++) {
        p = 0;
    }


    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <disk_path>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *disk_path = argv[1];

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

    // Call fsck
    if (fsck() == -1) {
        fprintf(stderr, "Failed to fsck.\n");
        exit(EXIT_FAILURE);
    }

    // Unmap the memory
    munmap(mapped_disk, sb.st_size);

    return 0;
}
