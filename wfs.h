#ifndef MOUNT_WFS_H_
#define MOUNT_WFS_H_

#include "types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#define MAX_FILE_NAME_LEN 32
#define WFS_MAGIC 0xdeadbeef

struct wfs_sb {
    uint32_t magic;
    uint32_t head;
};

struct wfs_inode {
    uint inode_number;
    uint deleted;       // 1 if deleted, 0 otherwise
    uint mode;          // type. S_IFDIR if the inode represents a directory or S_IFREG if it's for a file
    uint uid;           // user id
    uint gid;           // group id
    uint flags;         // flags
    uint size;          // size in bytes
    uint atime;         // last access time
    uint mtime;         // last modify time
    uint ctime;         // inode change time (the last time any field of inode is modified)
    uint links;         // number of hard links to this file (this can always be set to 1)
};

struct wfs_dentry {
    char name[MAX_FILE_NAME_LEN];
    ulong inode_number;
};

struct wfs_log_entry {
    struct wfs_inode inode;
    char data[];
};

#endif // MOUNT_WFS_H_
