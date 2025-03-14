// SPDX-License-Identifier: MIT

#ifndef INTERNAL_H
#define INTERNAL_H

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#define FILESYSTEM_DISPLAYNAME "yuki"
#define FILESYSTEM_MAGIC_NUMBER 0x59554B49 // FILESYSTEM MAGIC "YUKI"

#define FILE_DEFAULT_PERMISSION 0755

// define a tiny superblock info
#define FS_BLOCK_SIZE 1 // dd bs value
#define FS_BLOCK_COUNT 1 // dd count value
#define FS_BLOCK_FREE 0
#define FS_BLOCK_AVAILABLE 0
#define FS_TOTAL_INODES 1
#define FS_FREE_INODES 0
#define FS_MAX_LEN 8
// end define superblock info

struct yukifs_inode {
    int i_mode;
    unsigned long i_ino;
};

struct yukifs_super_block {
    struct super_block *sb;
    struct inode *root_inode;
};

#endif /* INTERNAL_H */