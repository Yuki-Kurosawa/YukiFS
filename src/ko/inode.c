// SPDX-License-Identifier: MIT

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>
#include <linux/log2.h>

#include "../../include/internal.h"
#include "../../include/version.h"
#include "../../include/file_table.h"
#include "file.h"

static void yukifs_put_super(struct super_block *sb)
{
    printk(KERN_INFO "YukiFS: put_super called\n");
    printk(KERN_DEBUG "YukiFS super block destroyed\n");
    printk(KERN_INFO "YukiFS: put_super called done\n");
}

static int yukifs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct superblock_info *sbi = (struct superblock_info*)dentry->d_sb->s_fs_info;

    buf->f_type = dentry->d_sb->s_magic;
    buf->f_bsize = dentry->d_sb->s_blocksize;
    buf->f_blocks = sbi->block_count; // Total blocks
    buf->f_bfree = sbi->block_free;   // Free blocks
    buf->f_bavail = sbi->block_free;  // Available blocks
    buf->f_files = sbi->total_inodes;   // Total inodes
    buf->f_ffree = sbi->free_inodes;    // Free inodes
    buf->f_namelen = FS_MAX_LEN; // Maximum filename length
    return 0;
}

static struct super_operations const yukifs_super_ops = {
    .put_super = yukifs_put_super,
    .statfs = yukifs_statfs,
    .drop_inode = generic_delete_inode,
};

static int yukifs_fill_super(struct super_block *sb, void *data, int silent)
{   

    printk(KERN_INFO "YukiFS: fill_super called\n");

    #pragma region Read Headers from devices

    // Read first 16KiB from devices for hidden header
    unsigned char *hidden_header_buffer;
    unsigned long header_size = 16 * 1024;

    hidden_header_buffer = kmalloc(header_size, GFP_KERNEL);
    if (!hidden_header_buffer) {
        printk(KERN_ERR "YukiFS: Failed to allocate memory for hidden header\n");
        return -ENOMEM;
    }

    unsigned long bytes_read = 0;
    unsigned long offset = 0;
    unsigned long block_nr = 0;
    struct buffer_head *bh;
    sb_set_blocksize(sb, 1024);
    int block_size = sb_min_blocksize(sb, 1024);
    
    for (block_nr = 0; bytes_read < header_size; block_nr++) {
        bh = sb_bread(sb, block_nr);
        if (!bh) {
            printk(KERN_ERR "YukiFS: Error reading block %lu\n", block_nr);
            
            return -EIO;
        }        

        unsigned long read_size = block_size;
        if (bytes_read + read_size > header_size) {
            read_size = header_size - bytes_read;
        }

        memcpy(hidden_header_buffer + offset, bh->b_data, read_size);
        bytes_read += read_size;
        offset += read_size;
        brelse(bh);
    }

    printk(KERN_DEBUG "YukiFS: Read %lu bytes of hidden header from device\n", bytes_read);  

    #pragma endregion

    #pragma region find and pop the Hidden Header
    int64_t hidden_data_start = -1;
    int64_t hidden_data_end = -1;

    for (off_t i = 0; i < bytes_read - 1; ++i) {
        if (hidden_header_buffer[i] == 0x55 && hidden_header_buffer[i + 1] == 0xAA) {
            //printk(KERN_DEBUG "YukiFS: Found sequence 0x55AA at offset %ld\n", (long)i);
            hidden_data_start = i;
        }
        if (hidden_header_buffer[i] == 0xAA && hidden_header_buffer[i + 1] == 0x55) {
            //printk(KERN_DEBUG "YukiFS: Found sequence 0xAA55 at offset %ld\n", (long)i);
            hidden_data_end = i;
        }
    }

    if (hidden_data_start == -1 || hidden_data_end == -1) {
        printk(KERN_ERR "YukiFS: Failed to find both hidden data markers (0x55AA and 0xAA55).\n");
        // Handle the case where the markers are not found.
        // You might want to return an error or take other appropriate action.
        return -EINVAL;
    } else {
        printk(KERN_DEBUG "YukiFS: Hidden data markers found. Start at %lld, End at %lld.\n",
               hidden_data_start, hidden_data_end);
    }
    #pragma endregion

    #pragma region go for hidden_data

    struct hidden_data_struct *hidden_data = (struct hidden_data_struct *)(hidden_header_buffer + hidden_data_start);  

    #pragma endregion

    #pragma region Initialize the superblock

    uint64_t superblock_offset = hidden_data->superblock_offset;
   
    // done for hidden data

    // go ahead for superblock reading from devices
    struct superblock_info *sb_info=kmalloc(sizeof(struct superblock_info), GFP_KERNEL);
    printk(KERN_DEBUG "YukiFS: Reading superblock from device\n");
    bytes_read = 0;
    offset = 0;
    unsigned long read_size = 0;

    block_nr = superblock_offset / block_size;
    bh = sb_bread(sb, block_nr);
    if (!bh) {
        printk(KERN_ERR "YukiFS: Error reading block %lu\n", block_nr);
        return -EIO;
    }
    read_size = sizeof(struct superblock_info);

    memcpy(sb_info, bh->b_data + offset, read_size);
    bytes_read += read_size;
    offset += read_size;
    brelse(bh);

    printk(KERN_DEBUG "YukiFS: Read %lu bytes of superblock from device\n", bytes_read);

    #pragma endregion

    #pragma region pop to superblock VFS

    sb->s_blocksize = sb_info->block_size;
    sb->s_blocksize_bits=ilog2(sb_info->block_size);
    sb->s_fs_info = sb_info;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb_set_blocksize(sb, sb_info->block_size);

    #pragma endregion

    #pragma region Free all the temp viariables

    kfree(hidden_header_buffer);

    #pragma endregion

    sb->s_magic = FILESYSTEM_MAGIC_NUMBER;
    sb->s_op = &yukifs_super_ops;

    int ret = yukifs_init_root(sb);
    
    printk(KERN_INFO "YukiFS: fill_super called done\n");

    return 0 | ret;
}

static struct dentry *yukifs_mount(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, yukifs_fill_super);
}

#pragma region  Module Initialization

static struct file_system_type yukifs_type = {
    .owner = THIS_MODULE,
    .name = FILESYSTEM_DISPLAYNAME,
    .mount = yukifs_mount,
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init yukifs_init(void)
{
    printk(KERN_DEBUG "YukiFS module loaded\n");
    return register_filesystem(&yukifs_type);
}

static void __exit yukifs_exit(void)
{
    unregister_filesystem(&yukifs_type);
    printk(KERN_DEBUG "YukiFS module unloaded\n");
}

#pragma endregion

module_init(yukifs_init);
module_exit(yukifs_exit);

MODULE_LICENSE(YUKIFS_LICENSE);
MODULE_AUTHOR(YUKIFS_AUTHOR);
MODULE_DESCRIPTION(YUKIFS_DESCRIPTION);
MODULE_VERSION(YUKIFS_VERSION_STRING);