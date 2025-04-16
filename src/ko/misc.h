// SPDX-License-Identifier: MIT
#ifndef KO_MISC_H
#define KO_MISC_H
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>
#include <linux/log2.h>
#include <linux/time64.h>

#include "../../include/internal.h"
#include "../../include/version.h"
#include "../../include/file_table.h"

//extern uint32_t yukifs_offset2block(struct super_block *sb, uint32_t offset);
extern int yukifs_blocks_read(struct super_block *sb, uint32_t block_nr,uint32_t block_count, char *buf);
extern int yukifs_blocks_write(struct super_block *sb, uint32_t block_nr,uint32_t block_count, char *buf);

extern int yukifs_inode_table_read(struct super_block *sb, char* inode_table);

#endif
