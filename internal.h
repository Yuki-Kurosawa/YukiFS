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



struct yukifs_inode {
    int i_mode;
    unsigned long i_ino;
};

struct yukifs_super_block {
    struct super_block *sb;
    struct inode *root_inode;
};

#endif /* INTERNAL_H */