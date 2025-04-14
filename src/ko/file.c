#include "file.h"

#pragma region Block IO Operations

// function to convert offset to block count
// this function is not used in the current implementation
/*
static uint32_t yukifs_offset2block(struct super_block *sb, uint32_t offset)
{
    struct superblock_info *sbi = sb->s_fs_info;
    uint32_t block_size = sbi->block_size;
    uint32_t block_nr = offset / block_size;
    return block_nr;
};
*/

static int yukifs_blocks_read(struct super_block *sb, uint32_t block_nr,uint32_t block_count, char *buf)
{
    struct superblock_info *sbi = sb->s_fs_info;
    uint32_t block_size = sbi->block_size;
    
    // no block count check due to module didn't know the real size of the file

    for (uint32_t i = 0; i < block_count; i++) 
    {
        struct buffer_head *bh;
        bh = sb_bread(sb, block_nr + i);
        if (!bh) {
            printk(KERN_ERR "YukiFS: Error reading block %d\n", block_nr);
            return -EIO;
        }
        memcpy(buf+i * block_size, bh->b_data, block_size);
        brelse(bh);
    }
    return 0;
};

static int yukifs_blocks_write(struct super_block *sb, uint32_t block_nr,uint32_t block_count, char *buf)
{
    struct superblock_info *sbi = sb->s_fs_info;
    uint32_t block_size = sbi->block_size;

    // no block count check due to module didn't know the real size of the file
    
    if (block_count > 0)
    {
        for (uint32_t i = 0; i < block_count; i++)  {
            struct buffer_head *bh;
            bh = sb_getblk(sb, block_nr + i);
            if (!bh) {
                printk(KERN_ERR "YukiFS: Error getting block %d\n", block_nr);
                return -EIO;
            }   
            memcpy(bh->b_data, buf+i * block_size, block_size);

            set_buffer_dirty(bh);
            sync_dirty_buffer(bh);
            
            brelse(bh);
        }
    }
    else
    {
        printk(KERN_ERR "YukiFS: Error writing block %d, block count is 0\n", block_nr);
        return -EIO;
    }
    return 0;
};

#pragma endregion

#pragma region File Operations

static struct inode *yukifs_make_inode(struct super_block *sb, struct file_object *fo);

static int yukifs_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "YukiFS: open called %s %s\n", file->f_path.dentry->d_name.name,((struct file_object*)inode->i_private)->name);

    //Check for O_APPEND flag
    if (file->f_flags & O_APPEND) {
        file->f_pos = i_size_read(inode); // Set file position to the end
        printk(KERN_INFO "YukiFS: open called with O_APPEND, setting offset to %lld\n", file->f_pos);
    } else {
        file->f_pos = 0; // Otherwise, start from the beginning
    }

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

static int yukifs_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct inode *dir = file->f_inode;
    struct superblock_info *sbi = dir->i_sb->s_fs_info;
    struct file_object * dirobj = (struct file_object *)dir->i_private;

    printk(KERN_INFO "YukiFS: Iterating directory %s\n", dirobj->name);
    
    if (ctx->pos >= dirobj->size) // Or however you determine the end of directory entries
    {
        return 0;
    }

    uint32_t inode_table_offset = sbi->inode_table_offset; 
    uint32_t inode_table_size = sbi->inode_table_storage_size; // use storage size due to whole blocks read
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_block_nr=inode_table_offset/sbi->block_size;

    char *inode_table = kmalloc(inode_table_size, GFP_KERNEL);
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return -ENOMEM;
    }    

    if(yukifs_blocks_read(dir->i_sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading inode table\n");
        return -EIO;
    }

    struct file_object *fo = dirobj;
    printk(KERN_INFO "YukiFS: directory inode %d\n", fo->descriptor);

    uint32_t data_blocks_offset = sbi->data_blocks_offset;
    uint32_t dir_data_block_num = fo->first_block;

    // read the data blocks from the device data blocks
    uint32_t data_block_size = sbi->block_size;
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_offset = data_blocks_offset + dir_data_block_num * data_block_size;
    uint32_t data_block_nr = data_block_offset / data_block_size;

    
    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    if(yukifs_blocks_read(dir->i_sb, data_block_nr, data_block_count, data_block) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading data block %d\n", data_block_nr);
        kfree(data_block);
        return -EIO;
    }

    printk(KERN_INFO "YukiFS: directory data block %d\n", data_block[0]);
    
    // treat data block as inode index list type uint32_t*
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = data_block_size / sizeof(uint32_t);
    
    for (uint32_t i = ctx->pos / sizeof(uint32_t); i < inode_index_list_size; i++) {
        if (inode_index_list[i] != 0) 
        {
            struct file_object *ffo = (struct file_object *)inode_table + inode_index_list[i];

            printk("  Inode Index (dentry) %d: Name: %s, Size: %u, Descriptor: %o, First Block: %u, Inner: %u\n", i, 
                strlen(ffo->name) > 0?ffo->name:"<root>", ffo->size, ffo->descriptor, ffo->first_block,
                ffo->inner_file
            ); 
            
            if(!dir_emit(ctx, ffo->name, strlen(ffo->name), inode_index_list[i] , DT_REG))
            {
                 ctx->pos = i * sizeof(uint32_t);
                 return 0;
            }
        }
        ctx->pos += sizeof(uint32_t);
    }

    kfree(data_block);
    kfree(inode_table);
    
    return 0;
}

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
    // read whole inode table from the device 
    uint32_t inode_table_offset = sbi->inode_table_offset; 
    uint32_t inode_table_size = sbi->inode_table_storage_size; // use storage size due to whole blocks read
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_block_nr=inode_table_offset/sbi->block_size;

    char *inode_table = kmalloc(inode_table_size, GFP_KERNEL);
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return -ENOMEM;
    }

    printk(KERN_INFO "YukiFS: inode table offset %d\n", inode_table_offset);
    printk(KERN_INFO "YukiFS: inode table block %d\n", inode_block_nr);
    printk(KERN_INFO "YukiFS: inode table clusters %d\n", inode_table_clusters);    

    if(yukifs_blocks_read(dir->i_sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading inode table\n");
        return -EIO;
    }
    
    struct file_object *fo = dirobj;
    printk(KERN_INFO "YukiFS: directory inode %d\n", fo->descriptor);

    uint32_t data_blocks_offset = sbi->data_blocks_offset;
    uint32_t dir_data_block_num = fo->first_block;

    // read the data blocks from the device data blocks
    uint32_t data_block_size = sbi->block_size;
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_offset = data_blocks_offset + dir_data_block_num * data_block_size;
    uint32_t data_block_nr = data_block_offset / data_block_size;

    
    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    if(yukifs_blocks_read(dir->i_sb, data_block_nr, data_block_count, data_block) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading data block %d\n", data_block_nr);
        kfree(data_block);
        return -EIO;
    }

    printk(KERN_INFO "YukiFS: directory data block %d\n", data_block[0]);
    
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
   
    struct file_object *new_fo = (struct file_object *)inode_table;

    // find the first free inode in the inode table
    uint32_t ii = UINT32_MAX;
    for (uint32_t i = 0; i < sbi->total_inodes; i++) {
        printk(KERN_INFO "YukiFS: physical inode %d current use status %d\n", i, new_fo[i].in_use);
        if (new_fo[i].in_use == 0) {
            new_fo[i].in_use = 1;
            new_fo[i].size = 0;
            new_fo[i].inner_file = 0;
            new_fo[i].descriptor = umode_t;
            new_fo[i].first_block = i;
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


    // write the new inode index to dir data block
    inode_index_list[new_inode_index] = ii;

    // write dir data block back and inode table back to device
    if(yukifs_blocks_write(dir->i_sb, data_block_nr, data_block_count, data_block) < 0)
    {
        printk(KERN_ERR "YukiFS: Error writing data block %d\n", data_block_nr);
        kfree(data_block);
        return -EIO;
    }

    if(yukifs_blocks_write(dir->i_sb, inode_block_nr, inode_table_clusters, inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error writing inode table\n");
        kfree(data_block);
        return -EIO;
    }

    printk(KERN_INFO "YukiFS: file %s created successfully\n", entry->d_name.name);

    kfree(data_block);
    kfree(inode_table);

    return 0;
};

static int yukifs_getattr(struct mnt_idmap *mnt, const struct path *path, struct kstat *stat,u32 mask, unsigned int query_flags)
{
    struct inode *inode = path->dentry->d_inode;
    struct file_object *fo = (struct file_object *)inode->i_private;

    printk(KERN_INFO "YukiFS: getattr dentry: %s inode: %s\n", path->dentry->d_name.name,fo->name);
    printk("  getattr(): Name: %s, Size: %u, Descriptor: %o, First Block: %u, Inner: %u\n", 
        fo->name,fo->size, fo->descriptor,fo->first_block,fo->inner_file);

    stat->mode = fo->descriptor;
    stat->ino = inode->i_ino;
    stat->size = fo->size;
    stat->blocks = (fo->size + inode->i_sb->s_blocksize - 1) / inode->i_sb->s_blocksize; // Calculate number of blocks
    stat->blksize = inode->i_sb->s_blocksize;
    stat->nlink = 1; // For simplicity, assume 1 hard link
    stat->uid = KUIDT_INIT(0);     // Root user for now
    stat->gid = KGIDT_INIT(0);     // Root group for now

    // Set timestamps (you might need to store these in your file_object later)
    struct timespec64 now;
    ktime_get_real_ts64(&now);
    stat->atime = now;
    stat->mtime = now;
    stat->ctime = now;

    return 0;
};

static struct dentry *yukifs_lookup(struct inode *parent, struct dentry *dentry, unsigned int flags)
{    
    struct superblock_info *sbi = parent->i_sb->s_fs_info;
    const char *name = dentry->d_name.name;
    int len = dentry->d_name.len;
    int i;

    struct file_object *fo = (struct file_object*)parent->i_private;

    printk(KERN_INFO "YukiFS: lookup called for '%s' in directory inode %lu\n", name, parent->i_ino);

    // read whole inode table from the device 
    uint32_t inode_table_offset = sbi->inode_table_offset; 
    uint32_t inode_table_size = sbi->inode_table_storage_size; // use storage size due to whole blocks read
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_block_nr=inode_table_offset/sbi->block_size;

    char *inode_table = kmalloc(inode_table_size, GFP_KERNEL);
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return ERR_PTR(-ENOMEM);
    }

    printk(KERN_INFO "YukiFS: inode table offset %d\n", inode_table_offset);
    printk(KERN_INFO "YukiFS: inode table block %d\n", inode_block_nr);
    printk(KERN_INFO "YukiFS: inode table clusters %d\n", inode_table_clusters);    

    if(yukifs_blocks_read(parent->i_sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading inode table\n");
        return ERR_PTR(-EIO);
    }

    uint32_t data_blocks_offset = sbi->data_blocks_offset;
    uint32_t dir_data_block_num = fo->first_block;

    // read the data blocks from the device data blocks
    uint32_t data_block_size = sbi->block_size;
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_offset = data_blocks_offset + dir_data_block_num * data_block_size;
    uint32_t data_block_nr = data_block_offset / data_block_size;

    
    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    if(yukifs_blocks_read(parent->i_sb, data_block_nr, data_block_count, data_block) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading data block %d\n", data_block_nr);
        kfree(data_block);
        return ERR_PTR(-EIO);
    }

    printk(KERN_INFO "YukiFS: directory data block %d\n", data_block[0]);
    
    // treat data block as inode index list type uint32_t*
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = data_block_size / sizeof(uint32_t);

    // try to find specified file in the directory
    for (i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] != 0) {
            struct file_object *ffo = (struct file_object *)inode_table + inode_index_list[i];
            if (strncmp(name, ffo->name, len) == 0 && len == strlen(ffo->name)) {
                printk(KERN_INFO "YukiFS: Found file %s in directory %s at Inode Index (dentry) %d\n", name, fo->name,i);
                
                // pop the inode from the inode table object
                printk(KERN_INFO "YukiFS: ffo->name %s ffo->size %d ffo->descriptor %o\n", ffo->name, ffo->size,ffo->descriptor);
                struct inode *inode = yukifs_make_inode(parent->i_sb, ffo);
                if (!inode) {
                    printk(KERN_ERR "YukiFS: inode allocation failed\n");
                    kfree(data_block);
                    kfree(inode_table);
                    return NULL;
                }

                dentry->d_inode = inode;
                d_add(dentry, dentry->d_inode);

                printk(KERN_INFO "YukiFS: File %s in directory %s at Inode Index (dentry) %d is poped successfully.\n", name, fo->name,i);

                kfree(data_block);
                kfree(inode_table);
                return NULL;

            }
        }
    }

    return NULL;
}

static int yukifs_unlink(struct inode *inode,struct dentry *dentry)
{
    printk(KERN_INFO "YukiFS: unlink called %s %s\n", dentry->d_name.name,((struct file_object*)inode->i_private)->name);
    return 0;
}

static int yukifs_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "YukiFS: release called %s %s\n", file->f_path.dentry->d_name.name,((struct file_object*)inode->i_private)->name);
    return 0;
}

static ssize_t yukifs_write(struct file *file, const char __user *buf, size_t len, loff_t *offset)
{
    struct inode *inode = file->f_inode;
    struct file_object *fo = (struct file_object *)inode->i_private;
    struct super_block *sb = inode->i_sb;
    ssize_t written = 0;
    uint32_t block_size = sb->s_blocksize;

    printk(KERN_INFO "YukiFS: write called for inode %lu, offset %lld, len %zu\n",
           inode->i_ino, *offset, len);

    if (!fo) {
        printk(KERN_ERR "YukiFS: write - file_object is NULL\n");
        return -ENODEV;
    }

    if (*offset > inode->i_size) {
        printk(KERN_ERR "YukiFS: write - offset exceeds file size\n");
        return -EINVAL;
    }

    uint32_t start_block = fo->first_block;
    

    uint32_t data_blocks_offset = ((struct superblock_info *)sb->s_fs_info)->data_blocks_offset;
    loff_t file_offset = *offset;

    // For simplicity, assuming single block for now
    uint32_t physical_block_number = start_block;
    uint32_t physical_offset = data_blocks_offset + physical_block_number * block_size;
    uint32_t physical_block_index = physical_offset / block_size;
    uint32_t offset_in_physical_block = physical_offset % block_size;

    size_t bytes_to_write = len;
    if (offset_in_physical_block + bytes_to_write > block_size) {
        bytes_to_write = block_size - offset_in_physical_block;
        // Need to handle multi-block writes later
    }

    char *kbuf = kmalloc(bytes_to_write, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
   

    if (copy_from_user(kbuf, buf, bytes_to_write)) {
        kfree(kbuf);
        return -EFAULT;
    }    

    printk(KERN_INFO "YukiFS: kbuf %s", kbuf);

    char *block_buf = kmalloc(block_size, GFP_KERNEL);
    if (!block_buf) {
        kfree(kbuf);
        return -ENOMEM;
    }

    // fill all bytes with zeros
    memset(block_buf, 0, block_size);

    // copy data from user buffer to kernel buffer
    memcpy(block_buf, kbuf, bytes_to_write);

    // write the block to the physical block
    if (yukifs_blocks_write(sb, physical_block_index, 1, block_buf)) {
        printk(KERN_ERR "YukiFS: Error writing to block %u\n", physical_block_index);
        kfree(kbuf);
        return -EIO;
    }

    written = bytes_to_write;
    *offset += written;
    
    file->f_pos = *offset;

    // prepare for new inode object
    fo->size = *offset;

    kfree(kbuf);
    return written;
}

#pragma endregion

#pragma region File Operation Callback Structures

struct inode_operations yukifs_dir_inode_operations = {
    .lookup = yukifs_lookup,
    .create = yukifs_create,
    .mkdir = NULL,
    .rmdir = NULL,  
    .link = NULL,
    .unlink = yukifs_unlink, 
    .getattr = yukifs_getattr,
};

struct inode_operations yukifs_file_inode_operations = {
    .unlink = yukifs_unlink, 
    .getattr = yukifs_getattr,
};

struct file_operations yukifs_file_ops = {
    .owner = THIS_MODULE,
    .open = yukifs_open,
    .read = yukifs_read,
    .write = yukifs_write,
    .release = yukifs_release,
};

struct file_operations yukifs_dir_ops = {
    .owner = THIS_MODULE,    
    .open = generic_file_open,
    .release = yukifs_release,
    .llseek = generic_file_llseek, 
    .iterate_shared = yukifs_iterate_shared,   
};

#pragma endregion

static struct inode *yukifs_make_inode(struct super_block *sb, struct file_object *fo)
{
    printk(KERN_INFO "YukiFS: make_inode ffo->name %s ffo->size %d ffo->descriptor %o\n", fo->name, fo->size,fo->descriptor);
    struct inode *inode = new_inode(sb);
    if (inode) {
        inode->i_mode = fo->descriptor;
        inode->i_uid.val = 0;
        inode->i_gid.val = 0;
        inode->i_size = fo->size;
        inode->i_blocks = inode->i_size / sb->s_blocksize;
        inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
        inode->i_ino = 9854+fo->first_block;
        if (S_ISDIR(inode->i_mode)) {
            inode->i_op = &yukifs_dir_inode_operations;
            inode->i_fop = &yukifs_dir_ops;
            // Initialize directory specific stuff if needed
        } else if (S_ISREG(inode->i_mode)) {
            inode->i_op = &yukifs_file_inode_operations; // You'll need to create this
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

