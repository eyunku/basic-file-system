#define _POSIX_C_SOURCE 200809L
#include "wfs.h"
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>

static char *mapped_disk = NULL;  // address of the original disk
static char *new_mapped_disk = NULL;  // address of the new disk

static int fsck() {
    ulong max_inode_number = 0;

    struct wfs_sb *superblock = (struct wfs_sb *)mapped_disk;
    char *current_position = mapped_disk + sizeof(struct wfs_sb);

    while (current_position < mapped_disk + superblock->head) {
        struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;
        if (current_entry->inode.inode_number > max_inode_number)
            max_inode_number = current_entry->inode.inode_number;
        current_position += sizeof(struct wfs_inode) + current_entry->inode.size;
    }
    new_mapped_disk = malloc(DISK_SIZE);
    struct wfs_sb *new_superblock = (struct wfs_sb *)new_mapped_disk;
    new_superblock->magic = WFS_MAGIC;
    new_superblock->head = sizeof(struct wfs_sb);

    for (ulong inode_number = 0; inode_number <= max_inode_number; inode_number++) {
        struct wfs_inode *latest_matching_entry = NULL;
        current_position = mapped_disk + sizeof(struct wfs_sb);

        while (current_position < mapped_disk + superblock->head) {
            struct wfs_log_entry *current_entry = (struct wfs_log_entry *)current_position;
            if (current_entry->inode.inode_number == inode_number)
                latest_matching_entry = &(current_entry->inode);
            current_position += sizeof(struct wfs_inode) + current_entry->inode.size;
        }

        if (latest_matching_entry != NULL) {
            memcpy(new_mapped_disk + new_superblock->head, latest_matching_entry, sizeof(*latest_matching_entry) + latest_matching_entry->size);
            new_superblock->head += sizeof(*latest_matching_entry) + latest_matching_entry->size;
        }
    }

    memset(new_mapped_disk + new_superblock->head, 0, DISK_SIZE - new_superblock->head);
    memcpy(mapped_disk, new_mapped_disk, DISK_SIZE);
    free(new_mapped_disk);

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
