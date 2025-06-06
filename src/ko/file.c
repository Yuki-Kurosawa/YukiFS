// SPDX-License-Identifier: MIT

#include "file.h"

#pragma region File Operations

static int yukifs_update_statfs(struct super_block *sb, struct file_object *fo);

static struct inode *yukifs_make_inode(struct super_block *sb, struct file_object *fo);

static int yukifs_open(struct inode *inode, struct file *file)
{
    struct file_object *fo = (struct file_object *)inode->i_private;
    printk(KERN_INFO "YukiFS: open called %s %s\n", file->f_path.dentry->d_name.name,fo->name);

    printk(KERN_INFO "YukiFS: open called %s size:%d\n", fo->name, fo->size);

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
    struct inode *inode = filp->f_inode;
    struct file_object *fo = (struct file_object *)inode->i_private;
    struct super_block *sb = inode->i_sb;
    uint32_t block_size = sb->s_blocksize;

    printk(KERN_INFO "YukiFS: read called for inode %lu, offset %lld, len %zu\n",
           inode->i_ino, *offset, len);

    if (!fo) {
        printk(KERN_ERR "YukiFS: read - file_object is NULL\n");
        return -ENOENT;
    }

    if (*offset > inode->i_size) {
        printk(KERN_ERR "YukiFS: read - offset exceeds file size\n");
        return -EINVAL;
    }

    uint32_t start_block = fo->first_block;

    if(len > inode->i_size - *offset)
    {
        len = inode->i_size - *offset;
    }
    else
    {
        len = 0;
    }

    uint32_t data_blocks_offset = ((struct superblock_info *)sb->s_fs_info)->data_blocks_offset;

    // For simplicity, assuming single block for now
    uint32_t physical_block_number = start_block;
    uint32_t physical_offset = data_blocks_offset + physical_block_number * block_size;
    uint32_t physical_block_index = physical_offset / block_size;
    uint32_t offset_in_physical_block = physical_offset % block_size;

    size_t bytes_to_read = len;
    if (offset_in_physical_block + bytes_to_read > block_size) {
        bytes_to_read = block_size - offset_in_physical_block;
        // Need to handle multi-block writes later
    }

    char *kbuf = kmalloc(bytes_to_read, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    char *block_buf_old = kmalloc(block_size, GFP_KERNEL);
    if (!block_buf_old) {
        kfree(kbuf);
        return -ENOMEM;
    }
    
    // read the block from the physical block
    if (yukifs_blocks_read(sb, physical_block_index, 1, block_buf_old)) {
        printk(KERN_ERR "YukiFS: Error reading block %u\n", physical_block_index);
        kfree(kbuf);
        return -EIO;
    }

    // copy the data from the block to the kernel buffer
    memcpy(kbuf, block_buf_old + *offset, bytes_to_read);
    
    printk(KERN_INFO "YukiFS: read - read %zu bytes from block %u\n", bytes_to_read, physical_block_index);
    printk(KERN_INFO "YukiFS: read data : %s\n", kbuf);
    kfree(block_buf_old);    

    if (copy_to_user(buf, kbuf, len))
        return -EFAULT;

    kfree(kbuf);

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

    char* inode_table = kmalloc(sbi->inode_table_size, GFP_KERNEL);
    
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return -ENOMEM;
    }    
    
    int inode_table_read = yukifs_inode_table_read(dir->i_sb, inode_table);
    if(inode_table_read < 0)
    {
        kfree(inode_table);
        return inode_table_read;
    }

    struct file_object *fo = dirobj;
    printk(KERN_INFO "YukiFS: directory i_mode %d\n", fo->descriptor);
  
    
    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    int data_block_read = yukifs_data_blocks_read(dir->i_sb,fo,data_block);
    if(data_block_read < 0)
    {
        kfree(data_block);
        kfree(inode_table);
        return data_block_read;
    }

    printk(KERN_INFO "YukiFS: directory data block %d\n", data_block[0]);
    
    // treat data block as inode index list type uint32_t*
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = sbi->block_size / sizeof(uint32_t);
    
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
                 kfree(data_block);
                 kfree(inode_table);
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

    char *inode_table = kmalloc(sbi->inode_table_size, GFP_KERNEL);
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return -ENOMEM;
    }

    int inode_table_read = yukifs_inode_table_read(dir->i_sb, inode_table);
    if(inode_table_read < 0)
    {
        kfree(inode_table);
        return inode_table_read;
    }
    
    struct file_object *fo = dirobj;
    printk(KERN_INFO "YukiFS: directory i_mode %d\n", fo->descriptor);

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

    if(yukifs_inode_table_write(dir->i_sb, inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error writing inode table\n");
        kfree(data_block);
        return -EIO;
    }
    
    yukifs_update_statfs(dir->i_sb, new_fo); // update all the statfs info after inode table is updated

    printk(KERN_INFO "YukiFS: file %s created successfully\n", entry->d_name.name);

    kfree(data_block);
    kfree(inode_table);

    return 0;
};

static int yukifs_getattr(struct mnt_idmap *mnt, const struct path *path, struct kstat *stat,u32 mask, unsigned int query_flags)
{
    struct inode *inode = path->dentry->d_inode;
    struct file_object *fo = (struct file_object *)inode->i_private;

    printk(KERN_INFO "YukiFS: getattr dentry: %s inode: %s with inode_num %ld\n", path->dentry->d_name.name,fo->name,inode->i_ino);
    printk("  getattr(): Name: %s, Size: %u, Descriptor: %o, First Block: %u, Inner: %u\n", 
        fo->name,fo->size, fo->descriptor,fo->first_block,fo->inner_file);

    stat->mode = inode->i_mode;
    stat->ino = inode->i_ino;
    stat->size = inode->i_size;
    stat->blocks = inode->i_blocks; // Calculate number of blocks
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

    char *inode_table = kmalloc(sbi->inode_table_size, GFP_KERNEL);
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return ERR_PTR(-ENOMEM);
    }

    int inode_table_read = yukifs_inode_table_read(parent->i_sb, inode_table);
    if(inode_table_read < 0)
    {
        kfree(inode_table);
        return ERR_PTR(inode_table_read);
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
        kfree(inode_table);
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

static int yukifs_unlink(struct inode *parent,struct dentry *dentry)
{
    struct file_object *fo = (struct file_object *)parent->i_private;
    struct super_block *sb = parent->i_sb;
    uint32_t block_size = sb->s_blocksize;
    struct superblock_info *sbi = (struct superblock_info *)sb->s_fs_info;

    if (!fo) {
        printk(KERN_ERR "YukiFS: unlink - file_object is NULL\n");
        return -ENOENT;
    }

    printk(KERN_INFO "YukiFS: unlink called %s %s\n", dentry->d_name.name,fo->name);

    uint32_t dir_data_block_num = fo->first_block;   

    printk(KERN_INFO "YukiFS: unlink dentry index logical start block %d of dir %s\n", dir_data_block_num, fo->name);

    uint32_t data_blocks_offset = sbi->data_blocks_offset;    

    struct file_object *ffo = (struct file_object *)dentry->d_inode->i_private;
    uint32_t dentry_block_index = ffo->first_block;

    printk(KERN_INFO "YukiFS: unlink logical data block %d\n", dentry_block_index);

    // For simplicity, assuming single block for now
    uint32_t physical_block_number = dentry_block_index;
    uint32_t physical_offset = data_blocks_offset + physical_block_number * block_size;
    uint32_t physical_block_index = physical_offset / block_size;    

    size_t bytes_to_write = block_size;

    char *kbuf = kmalloc(bytes_to_write, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    
    memset(kbuf, 0, bytes_to_write);

    if (yukifs_blocks_write(sb, physical_block_index, 1, kbuf)) {
        printk(KERN_ERR "YukiFS: Error unlinking to physical block %u\n", physical_block_index);
        kfree(kbuf);
        return -EIO;
    }
    else
    {
        printk(KERN_INFO "YukiFS: unlinking physical block %d successfully\n", physical_block_index);
    }

    // read the data blocks from the device data blocks
    uint32_t data_block_size = sbi->block_size;
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_offset = data_blocks_offset + dir_data_block_num * data_block_size;
    uint32_t data_block_nr = data_block_offset / data_block_size;

    
    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    if(yukifs_blocks_read(parent->i_sb, data_block_nr, data_block_count, data_block) < 0)
    {
        printk(KERN_ERR "YukiFS: unlink Error reading data block %d\n", data_block_nr);
        kfree(data_block);
        return -EIO;
    }

    // try to remove dentry from the directory
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = data_block_size / sizeof(uint32_t);
    uint32_t dentry_index = UINT32_MAX;
    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] == dentry_block_index) {
            dentry_index = i;
            break;
        }
    }
    if (dentry_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: unlink - dentry not found in directory\n");
        kfree(data_block);
        return -ENOENT;
    }
    inode_index_list[dentry_index] = 0;

    // write the data blocks back to the device data blocks
    if (yukifs_blocks_write(sb, data_block_nr, data_block_count, data_block)) {
        printk(KERN_ERR "YukiFS: unlink Error writing data block %d\n", data_block_nr);
        kfree(data_block);
        return -EIO;
    }
    else
    {
        printk(KERN_INFO "YukiFS: unlinking dentry %s from dir %s successfully\n", dentry->d_name.name, fo->name);
    }

    ffo->size = 0;
    yukifs_update_statfs(sb, ffo);

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
        return -ENOENT;
    }

    if (*offset > inode->i_size) {
        printk(KERN_ERR "YukiFS: write - offset exceeds file size\n");
        return -EINVAL;
    }

    uint32_t start_block = fo->first_block;   

    uint32_t data_blocks_offset = ((struct superblock_info *)sb->s_fs_info)->data_blocks_offset;

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

    char *block_buf_old = kmalloc(block_size, GFP_KERNEL);
    if (!block_buf_old) {
        kfree(kbuf);
        return -ENOMEM;
    }

    // read the block from the physical block
    if (yukifs_blocks_read(sb, physical_block_index, 1, block_buf_old)) {
        printk(KERN_ERR "YukiFS: Error reading block %u\n", physical_block_index);
        kfree(kbuf);
        return -EIO;
    }

    // fill all bytes with zeros
    memset(block_buf, 0, block_size);

    printk(KERN_INFO "YukiFS: write %s called flag %x\n", fo->name, file->f_flags);
    // copy old data to new buffer
    if(file->f_flags & O_APPEND) // copy old data to new buffer if appending
    {
        memcpy(block_buf, block_buf_old, fo->size);
    }

    // copy data from user buffer to kernel buffer
    memcpy(block_buf + *offset, kbuf, bytes_to_write);

    // write the block to the physical block
    if (yukifs_blocks_write(sb, physical_block_index, 1, block_buf)) {
        printk(KERN_ERR "YukiFS: Error writing to block %u\n", physical_block_index);
        kfree(kbuf);
        return -EIO;
    }

    written = bytes_to_write;
    *offset += written;

    inode->i_size = *offset; // update in-memory inode     
    file->f_pos = *offset;

    // prepare for new inode object
    fo->size = *offset;

    yukifs_update_statfs(sb, fo);

    kfree(block_buf);

    kfree(kbuf);
    return written;
}

static int yukifs_update_statfs(struct super_block *sb, struct file_object *fo)
{
    struct superblock_info *sbi = (struct superblock_info *)sb->s_fs_info;
    uint32_t inode_table_offset = sbi->inode_table_offset; 
    uint32_t inode_table_size = sbi->inode_table_storage_size; // use storage size due to whole blocks read
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_block_nr=inode_table_offset/sbi->block_size;

    char *inode_table = kmalloc(inode_table_size, GFP_KERNEL);
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return -ENOMEM;
    }  

    if(yukifs_blocks_read(sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading inode table\n");
        return -EIO;
    }

    uint32_t inode_index_list_size = sbi->total_inodes;

    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (((struct file_object*)inode_table)[i].in_use == 1)
        {
            struct file_object *ffo = &(((struct file_object *)inode_table)[i]);
            if (strncmp(fo->name, ffo->name, strlen(fo->name)) == 0 && strlen(fo->name) == strlen(ffo->name)) 
            {     
                // Update the inode object

                printk(KERN_INFO "YukiFS: updating inode %s \n", ffo->name);
                printk(KERN_INFO "YukiFS: updating inode %s old size %d new size %d \n", ffo->name,ffo->size,fo->size);

                if(ffo->size != 0 && fo->size != 0)
                {
                    // update metadata for file
                    ffo->size = fo->size;
                    printk(KERN_INFO "YukiFS: updating inode %s with size %d\n", ffo->name, fo->size);
                }
                else if(ffo->size != 0 && fo->size == 0)
                {
                    // erase metadata for file                    
                    printk(KERN_INFO "YukiFS: erasing inode %s\n", ffo->name);
                    memset(ffo, 0, sizeof(struct file_object));
                }
                else if(ffo->size == 0 && fo->size != 0)
                {
                    // init metadata for file
                    ffo->size = fo->size;
                    printk(KERN_INFO "YukiFS: initializing inode %s with size %d\n", ffo->name, fo->size);
                }
                else
                {
                    // do nothing if ffo->size == 0 && fo->size == 0
                }
            }
        }
    }

    // write inode table back to disk
    if (yukifs_blocks_write(sb, inode_block_nr, inode_table_clusters, (char *)inode_table))
    {
        printk(KERN_ERR "YukiFS: Error writing inode table\n");
        return -EIO;
    }
    kfree(inode_table);

    // update superblock
    sbi->block_free = sbi->block_count;
    sbi->free_inodes = sbi->total_inodes;

    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (((struct file_object*)inode_table)[i].in_use == 1)
        {
            //struct file_object *ffo = &(((struct file_object *)inode_table)[i]);
            sbi->free_inodes -= 1;
            sbi->block_free -= 1; // assume each inode takes 1 block , later will be changed to real size
        }
    }

    // write superblock back to disk
    // superblock is always before the inode table
    if(yukifs_blocks_write(sb, inode_block_nr - 1, 1, (char *)sbi))
    {
        printk(KERN_ERR "YukiFS: Error writing superblock\n");
        return -EIO;
    }   

    return 0;
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
        #if LINUX_VERSION_CODE < KERNEL_VERSION(6,11,0)
            inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
        #else
            struct timespec64 ts=current_time(inode);
            inode->i_atime_sec = inode->i_mtime_sec = inode->i_ctime_sec = ts.tv_sec;
            inode->i_atime_nsec = inode->i_mtime_nsec = inode->i_ctime_nsec = ts.tv_nsec;
        #endif
        inode->i_ino = 9854 + fo->first_block;
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

