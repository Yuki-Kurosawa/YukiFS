#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include "internal.h"

static struct inode *yukifs_make_inode(struct super_block *sb, int mode)
{
    struct inode *inode = new_inode(sb);
    if (inode) {
        inode->i_mode = mode;
        inode->i_uid.val = 0;
        inode->i_gid.val = 0;
        inode->i_blocks = 0;
        inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
        inode->i_ino = get_next_ino();
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

    root = yukifs_make_inode(sb, S_IFDIR | 0755);
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
    return 0;
}

static struct dentry *yukifs_mount(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data)
{
    return mount_nodev(fs_type, flags, data, yukifs_fill_super);
}

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

module_init(yukifs_init);
module_exit(yukifs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("GitHub Copilot");
MODULE_DESCRIPTION("A simple filesystem with . and .. inodes");