#include "file.h"

#pragma region File Operations

static int yukifs_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t yukifs_read(struct file *filp, char __user *buf, size_t len, loff_t *offset)
{
    const unsigned char *data = YUKIFS_VERSION_STRING "\n";
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

struct file_operations yukifs_file_ops = {
    .owner = THIS_MODULE,
    .open = yukifs_open,
    .read = yukifs_read,
};

#pragma endregion

static struct inode *yukifs_make_inode(struct super_block *sb, struct file_object *fo)
{
    struct inode *inode = new_inode(sb);
    if (inode) {
        inode->i_mode = fo->descriptor;
        inode->i_uid.val = 0;
        inode->i_gid.val = 0;
        inode->i_size = fo->size;
        inode->i_blocks = inode->i_size / sb->s_blocksize;
        inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
        inode->i_ino = get_next_ino();
        if (S_ISDIR(inode->i_mode)) {
            inode->i_op = &simple_dir_inode_operations;
            inode->i_fop = &simple_dir_operations;
            // Initialize directory specific stuff if needed
        } else if (S_ISREG(inode->i_mode)) {
            //inode->i_op = &yukifs_file_inode_operations; // You'll need to create this
            inode->i_fop = &yukifs_file_ops;
            // Initialize file specific stuff (e.g., first block)
        } else {
            printk(KERN_ERR "YukiFS: Unknown inode type\n");
            iput(inode);
            return NULL;
        }
        inode->i_private = fo;
    }

    return inode;
}

int yukifs_init_root(struct super_block *sb)
{
    struct inode *root;
    struct dentry *root_dentry;

    // read root inode from the device inode table then create a dentry for it
    struct superblock_info *sbi = sb->s_fs_info;
    uint32_t inode_table_offset = sbi->inode_table_offset; 
    uint32_t inode_size = sizeof(struct file_object);

    struct file_object *root_fo = kmalloc(inode_size, GFP_KERNEL);
    if (!root_fo)
        return -ENOMEM;

    loff_t offset = inode_table_offset;
    sector_t block_nr = offset / sb->s_blocksize;
    unsigned int block_offset = offset % sb->s_blocksize;
    struct buffer_head *bh;

    bh = sb_bread(sb, block_nr);
    if (!bh) {
        printk(KERN_ERR "YukiFS: Error reading root inode block\n");
        kfree(root_fo);
        return -EIO;
    }

    memcpy(root_fo, bh->b_data + block_offset, inode_size);
    brelse(bh);

    root = yukifs_make_inode(sb,root_fo);
    if (!root) {
        printk(KERN_ERR "YukiFS: inode allocation failed\n");
        return -ENOMEM;
    }

    root_dentry = d_make_root(root);
    if (!root_dentry) {
        iput(root);
        printk(KERN_ERR "YukiFS: root creation failed\n");
        return -ENOMEM;
    }
    sb->s_root = root_dentry;
    
    return 0;

}

