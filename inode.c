// SPDX-License-Identifier: MIT

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include "internal.h"
#include "version.h"
#include "file_table.h"

// define a tiny superblock info
#define FS_BLOCK_SIZE 1 // dd bs value
#define FS_BLOCK_COUNT 1 // dd count value
#define FS_BLOCK_FREE 0
#define FS_BLOCK_AVAILABLE 0
#define FS_TOTAL_INODES 1
#define FS_FREE_INODES 0
// end define superblock info

#pragma region  File Operations

static int yukifs_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t yukifs_read(struct file *filp, char __user *buf, size_t len, loff_t *offset)
{
    const unsigned char *data = version;
    size_t datalen = strlen(data);

    if (*offset >= datalen)
        return 0;

    if (len > datalen - *offset)
        len = datalen - *offset;

    if (copy_to_user(buf, data + *offset, len))
        return -EFAULT;

    *offset += len;
    return len;
}

static struct file_operations yukifs_file_ops = {
    .owner = THIS_MODULE,
    .open = yukifs_open,
    .read = yukifs_read,
};

#pragma endregion

static struct inode *yukifs_make_inode(struct super_block *sb, int mode, unsigned char* data)
{
    struct inode *inode = new_inode(sb);
    if (inode) {
        inode->i_mode = mode;
        inode->i_uid.val = 0;
        inode->i_gid.val = 0;
        
        if(!data)
            inode->i_size = 0;
        else
            inode->i_size = strlen(data);
        inode->i_blocks = inode->i_size / FS_BLOCK_SIZE;
        inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
        inode->i_ino = get_next_ino();
        inode->i_fop = &yukifs_file_ops; // Add this line to set file operations
    }
    return inode;
}

static void yukifs_put_super(struct super_block *sb)
{
    printk(KERN_DEBUG "YukiFS super block destroyed\n");
}

static int yukifs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    buf->f_type = FILESYSTEM_MAGIC_NUMBER;
    buf->f_bsize = FS_BLOCK_SIZE;
    buf->f_blocks = FS_BLOCK_COUNT; // Total blocks
    buf->f_bfree = FS_BLOCK_FREE;   // Free blocks
    buf->f_bavail = FS_BLOCK_AVAILABLE;  // Available blocks
    buf->f_files = FS_TOTAL_INODES;   // Total inodes
    buf->f_ffree = FS_FREE_INODES;    // Free inodes
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
    struct inode *root;
    struct dentry *root_dentry;

    sb->s_magic = FILESYSTEM_MAGIC_NUMBER;
    sb->s_op = &yukifs_super_ops;

    #pragma region  Initialize the root inode

    root = yukifs_make_inode(sb, S_IFDIR | 0755, NULL);
    if (!root) {
        printk(KERN_ERR "YukiFS: inode allocation failed\n");
        return -ENOMEM;
    }
    root->i_op = &simple_dir_inode_operations;
    root->i_fop = &simple_dir_operations;

    root_dentry = d_make_root(root);
    if (!root_dentry) {
        iput(root);
        printk(KERN_ERR "YukiFS: root creation failed\n");
        return -ENOMEM;
    }
    sb->s_root = root_dentry;

    #pragma endregion

    #pragma region  Initialize the root directory

    //Add a folder to the root directory
    struct dentry *folder_dentry = d_alloc_name(root_dentry, "test_folder");
    if (!folder_dentry) {
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: folder creation failed\n");
        return -ENOMEM;
    }
    folder_dentry->d_inode = yukifs_make_inode(sb, S_IFDIR | 0755 , NULL);
    
    if (!folder_dentry->d_inode) {
        dput(folder_dentry);
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: folder inode allocation failed\n");
        return -ENOMEM;
    }
    folder_dentry->d_inode->i_op = &simple_dir_inode_operations;
    folder_dentry->d_inode->i_fop = &simple_dir_operations;
    d_add(folder_dentry, folder_dentry->d_inode);

    //Add a file to the root directory
    struct dentry *file_dentry = d_alloc_name(root_dentry, "version.txt");
    if (!file_dentry) {
        dput(folder_dentry);
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: file creation failed\n");
        return -ENOMEM;
    }
    file_dentry->d_inode = yukifs_make_inode(sb, S_IFREG | 0644, version);
    if (!file_dentry->d_inode) {
        dput(file_dentry);
        dput(folder_dentry);
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: file inode allocation failed\n");
        return -ENOMEM;
    }
    file_dentry->d_inode->i_op = &simple_dir_inode_operations;
    file_dentry->d_inode->i_fop = &yukifs_file_ops;
    d_add(file_dentry, file_dentry->d_inode);


    #pragma endregion

    return 0;
}

static struct dentry *yukifs_mount(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data)
{
    return mount_nodev(fs_type, flags, data, yukifs_fill_super);
}

#pragma region  Module Initialization

static struct file_system_type yukifs_type = {
    .owner = THIS_MODULE,
    .name = FILESYSTEM_DISPLAYNAME,
    .mount = yukifs_mount,
    .kill_sb = kill_litter_super,
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
MODULE_VERSION(YUKIFS_VERSION);