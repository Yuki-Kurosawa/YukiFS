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

struct file_operations yukifs_dir_ops = {
    .owner = THIS_MODULE,    
    .open = generic_file_open,
    .release = NULL,
    .llseek = generic_file_llseek,
};

static int yukifs_create(struct mnt_idmap *mnt, struct inode *dir,struct dentry *entry, ushort umode_t, bool excl)
{
    mnt=&nop_mnt_idmap;
    printk(KERN_INFO "YukiFS: create called %s %s %d\n", entry->d_name.name,((struct file_object*)dir->i_private)->name,umode_t);

    struct superblock_info *sbi = dir->i_sb->s_fs_info;
    struct file_object * dirobj = (struct file_object *)dir->i_private;

    // due to no sub directory support, we only support creating files
    if (S_ISDIR(umode_t)) {
        return -EPERM;
    }

    // physical inode 0 always represents the root directory
    // read the root inode from the device inode table then create a dentry for it
    uint32_t inode_table_offset = sbi->inode_table_offset; 
    uint32_t inode_size = sizeof(struct file_object);
    uint32_t inode_index = 0;
    uint32_t inode_offset = inode_table_offset + inode_size * inode_index;
    uint32_t block_nr = inode_offset / sbi->block_size;
    unsigned int block_offset = inode_offset % sbi->block_size;
    struct buffer_head *bh;
    bh = sb_bread(dir->i_sb, block_nr);
    if (!bh) {
        printk(KERN_ERR "YukiFS: Error reading inode block\n");
        return -EIO;
    }
    struct file_object *fo = (struct file_object *)(bh->b_data + block_offset);
    printk(KERN_INFO "YukiFS: root inode %d\n", fo->descriptor);

    uint32_t data_blocks_offset = sbi->data_blocks_offset;
    uint32_t root_data_block_num = fo->first_block;

    // read the data blocks from the device data blocks
    uint32_t data_block_size = sbi->block_size;
    uint32_t data_block_count = sbi->data_blocks_total_size / data_block_size;
    uint32_t data_block_offset = data_blocks_offset + root_data_block_num * data_block_size;
    uint32_t data_block_nr = data_block_offset / data_block_size;
    uint32_t data_block_offset_in_block = data_block_offset % data_block_size;
    bh = sb_bread(dir->i_sb, data_block_nr);
    if (!bh) {
        printk(KERN_ERR "YukiFS: Error reading data block\n");
        brelse(bh);
        return -EIO;
    }
    char *data_block = bh->b_data + data_block_offset_in_block;
    printk(KERN_INFO "YukiFS: root data block %d\n", data_block[0]);
    brelse(bh);

    // treat data block as inode index list type uint32_t*
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = data_block_size / sizeof(uint32_t);
    uint32_t new_inode_index = UINT32_MAX;
    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] == 0) {
            new_inode_index = i;
            break;
        }
    }
    if (new_inode_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: Error creating file, no free inode index\n");
        
        return -ENOSPC;
    }
    else
    {
        printk(KERN_INFO "YukiFS: new logical inode index %d\n", new_inode_index);
    }

    // read whole inode table from device
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_table_storage_size = sbi->inode_table_storage_size;
    uint32_t inode_table_size = sbi->inode_table_size;
    inode_table_offset = sbi->inode_table_offset;
    uint32_t inode_table_offset_in_block = inode_table_offset % sbi->block_size;
    uint32_t inode_table_block_nr = inode_table_offset / sbi->block_size;
    bh = sb_bread(dir->i_sb, inode_table_block_nr);
    if (!bh) 
    {
        printk(KERN_ERR "YukiFS: Error reading inode table block\n");
        brelse(bh);
        return -EIO;
    }
    struct file_object *new_fo = (struct file_object *)((char *)bh->b_data + inode_table_offset_in_block);

    // find the first free inode in the inode table
    uint32_t ii = UINT32_MAX;
    for (uint32_t i = 0; i < sbi->total_inodes; i++) {
        printk(KERN_INFO "YukiFS: physical inode %d in use %d\n", i, new_fo[i].in_use);
        if (new_fo[i].in_use == 0) {
            new_fo[i].in_use = 1;
            new_fo[i].size = 0;
            new_fo[i].inner_file = 0;
            new_fo[i].descriptor = umode_t;
            new_fo[i].first_block = new_inode_index;
            strncpy(new_fo[i].name, entry->d_name.name, FS_MAX_LEN);
            ii = i;
            break;
        }
    }
    if (ii == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: Error creating file, no free inode\n");
        return -ENOSPC;
    }
    else
    {
        printk(KERN_INFO "YukiFS: new physical inode %d\n", ii);
    }

    // write the new inode to the device inode table

    return -EIO;
};

struct inode_operations yukifs_dir_inode_operations = {
    .lookup = simple_lookup,
    .create = yukifs_create,
    .mkdir = NULL,
    .rmdir = NULL,  
    .link = NULL,
    .unlink = NULL, 
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
            inode->i_op = &yukifs_dir_inode_operations;
            inode->i_fop = &yukifs_dir_ops;
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

