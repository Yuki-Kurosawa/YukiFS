// SPDX-License-Identifier: MIT
#ifndef KO_FILE_H
#define KO_FILE_H

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>
#include <linux/log2.h>
#include <linux/time64.h>
#include <linux/version.h>
#include <linux/kernel.h>

#include "../../include/internal.h"
#include "../../include/version.h"
#include "../../include/file_table.h"
#include "misc.h"


//static int yukifs_open(struct inode *inode, struct file *filp);
//static ssize_t yukifs_read(struct file *filp, char __user *buf, size_t len, loff_t *offset);
extern struct file_operations yukifs_file_ops;
extern int yukifs_init_root(struct super_block *sb);

#endif