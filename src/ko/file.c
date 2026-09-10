// SPDX-License-Identifier: MIT

#include "file.h"
#include <linux/string.h>

#pragma region File Operations

static int yukifs_update_statfs(struct super_block *sb, uint32_t inode_idx,
                                const struct file_object *fo, bool erase);

static void yukifs_fill_inode(struct super_block *sb, struct inode *inode,
                              struct file_object *fo, uint32_t inode_index);

extern struct inode_operations yukifs_dir_inode_operations;
extern struct inode_operations yukifs_file_inode_operations;
extern struct inode_operations yukifs_symlink_inode_operations;
extern struct file_operations yukifs_file_ops;
extern struct file_operations yukifs_dir_ops;

// --- block bitmap helpers (one bit per data block) ---

static int yukifs_bitmap_read(struct super_block *sb, char *bitmap)
{
    struct superblock_info *sbi = sb->s_fs_info;
    return yukifs_blocks_read(sb, sbi->bitmap_offset / sbi->block_size,
                              sbi->bitmap_blocks, bitmap);
}

static int yukifs_bitmap_write(struct super_block *sb, char *bitmap)
{
    struct superblock_info *sbi = sb->s_fs_info;
    return yukifs_blocks_write(sb, sbi->bitmap_offset / sbi->block_size,
                               sbi->bitmap_blocks, bitmap);
}

static bool yukifs_bitmap_test(const char *bitmap, uint32_t blk)
{
    return (bitmap[blk / 8] >> (blk % 8)) & 1;
}

static void yukifs_bitmap_set(char *bitmap, uint32_t blk)
{
    bitmap[blk / 8] |= (char)(1u << (blk % 8));
}

static void yukifs_bitmap_clear(char *bitmap, uint32_t blk)
{
    bitmap[blk / 8] &= (char)~(1u << (blk % 8));
}

/* populate a freshly allocated inode from an on-disk file_object; the caller
   hands over the kmemdup'd copy that becomes inode->i_private */
static void yukifs_fill_inode(struct super_block *sb, struct inode *inode,
                              struct file_object *fo, uint32_t inode_index)
{
    inode->i_mode = fo->descriptor;
    inode->i_uid.val = fo->uid;
    inode->i_gid.val = fo->gid;
    inode->i_size = fo->size;
    inode->i_blocks = fo->size ? ((fo->size + sb->s_blocksize - 1) / sb->s_blocksize) : 1;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(6,11,0)
        inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
    #else
        inode->i_atime_sec = fo->atime_sec;
        inode->i_mtime_sec = fo->mtime_sec;
        inode->i_ctime_sec = fo->ctime_sec;
        inode->i_atime_nsec = inode->i_mtime_nsec = inode->i_ctime_nsec = 0;
    #endif
    inode->i_ino = 9854 + inode_index;
    if (S_ISDIR(inode->i_mode)) {
        inode->i_op = &yukifs_dir_inode_operations;
        inode->i_fop = &yukifs_dir_ops;
    } else if (S_ISREG(inode->i_mode)) {
        inode->i_op = &yukifs_file_inode_operations;
        inode->i_fop = &yukifs_file_ops;
    } else if (S_ISLNK(inode->i_mode)) {
        inode->i_op = &yukifs_symlink_inode_operations;
        inode->i_fop = NULL;
    } else {
        printk(KERN_ERR "YukiFS: Unknown inode type 0%o\n", inode->i_mode);
    }
    inode->i_private = fo;
}

static int yukifs_open(struct inode *inode, struct file *file)
{
    struct file_object *fo = (struct file_object *)inode->i_private;
    if (!fo) {
        printk(KERN_ERR "YukiFS: open - inode %lu has no file_object\n", inode->i_ino);
        return -EIO;
    }
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
    struct superblock_info *sbi = sb->s_fs_info;
    uint32_t block_size = sb->s_blocksize;

    if (!fo) {
        printk(KERN_ERR "YukiFS: read - file_object is NULL\n");
        return -ENOENT;
    }

    if (*offset > inode->i_size) {
        printk(KERN_ERR "YukiFS: read - offset exceeds file size\n");
        return -EINVAL;
    }

    /* EOF */
    if (*offset == inode->i_size || len == 0)
        return 0;

    /* clamp the read to the file size (the old reversed logic returned 0
       whenever the request fit inside the file) */
    if (len > inode->i_size - *offset)
        len = inode->i_size - *offset;

    char *kbuf = kmalloc(len, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    char *block_buf = kmalloc(block_size, GFP_KERNEL);
    if (!block_buf) {
        kfree(kbuf);
        return -ENOMEM;
    }

    /* multi-block read: walk block by block */
    size_t pos = 0;
    while (pos < len) {
        loff_t seg_off = *offset + pos;
        uint32_t blk = fo->first_block + seg_off / block_size;
        uint32_t off_in_blk = seg_off % block_size;
        size_t chunk = len - pos;
        if (chunk > block_size - off_in_blk)
            chunk = block_size - off_in_blk;

        uint32_t physical_block_index = (sbi->data_blocks_offset + blk * block_size) / block_size;

        if (yukifs_blocks_read(sb, physical_block_index, 1, block_buf)) {
            printk(KERN_ERR "YukiFS: Error reading block %u\n", physical_block_index);
            kfree(block_buf);
            kfree(kbuf);
            return -EIO;
        }

        memcpy(kbuf + pos, block_buf + off_in_blk, chunk);
        pos += chunk;
    }

    kfree(block_buf);

    if (copy_to_user(buf, kbuf, len)) {
        kfree(kbuf);
        return -EFAULT;
    }

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

    /* "." and ".." are not stored on disk; synthesize them at pos 0 and 1.
       getdents resumption uses ctx->pos, so keep the slot scan starting at 2. */
    if (ctx->pos == 0) {
        if (!dir_emit(ctx, ".", 1, dir->i_ino, DT_DIR))
            return 0;
        ctx->pos = 1;
    }
    if (ctx->pos == 1) {
        struct inode *pdir = file->f_path.dentry->d_parent->d_inode;
        if (!dir_emit(ctx, "..", 2, pdir ? pdir->i_ino : dir->i_ino, DT_DIR))
            return 0;
        ctx->pos = 2;
    }

    char* inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
    
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
    uint32_t inode_index_list_size = fo->size / sizeof(uint32_t);
    
    for (uint32_t i = (ctx->pos - 2) / sizeof(uint32_t); i < inode_index_list_size; i++) {
        if (inode_index_list[i] != 0) 
        {
            struct file_object *ffo = (struct file_object *)inode_table + inode_index_list[i];

            printk("  Inode Index (dentry) %d: Name: %s, Size: %u, Descriptor: %o, First Block: %u, Inner: %u\n", i, 
                strlen(ffo->name) > 0?ffo->name:"<root>", ffo->size, ffo->descriptor, ffo->first_block,
                ffo->inner_file
            ); 
            
            /* hard link records share the real inode number with the file */
            uint32_t emit_idx = inode_index_list[i];
            if (ffo->inner_file > 0 && (uint32_t)ffo->inner_file < sbi->total_inodes)
                emit_idx = (uint32_t)ffo->inner_file;
            unsigned int emit_type;
            if (S_ISDIR(ffo->descriptor))
                emit_type = DT_DIR;
            else if (S_ISLNK(ffo->descriptor))
                emit_type = DT_LNK;
            else
                emit_type = DT_REG;

            if(!dir_emit(ctx, ffo->name, strlen(ffo->name), 9854 + emit_idx, emit_type))
            {
                 ctx->pos = 2 + i * sizeof(uint32_t);
                 kfree(data_block);
                 kfree(inode_table);
                 return 0;
            }
        }
        ctx->pos = 2 + (i + 1) * sizeof(uint32_t);
    }

    kfree(data_block);
    kfree(inode_table);
    
    return 0;
}

static int yukifs_create(struct mnt_idmap *mnt, struct inode *dir,struct dentry *entry, ushort umode_t, bool excl)
{
    (void)mnt;
    (void)excl;
    printk(KERN_INFO "YukiFS: create called %s %s %d\n", entry->d_name.name,((struct file_object*)dir->i_private)->name,umode_t);

    struct superblock_info *sbi = dir->i_sb->s_fs_info;
    struct file_object * dirobj = (struct file_object *)dir->i_private;

    // due to no sub directory support, we only support creating files
    if (S_ISDIR(umode_t)) {
        return -EPERM;
    }

    // the name buffer is FS_MAX_LEN bytes: at most FS_MAX_LEN-1 chars + NUL
    if (entry->d_name.len >= FS_MAX_LEN) {
        printk(KERN_ERR "YukiFS: filename too long (%u >= %d)\n", entry->d_name.len, FS_MAX_LEN);
        return -ENAMETOOLONG;
    }

    char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
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
    uint32_t data_block_nr = (data_blocks_offset + dir_data_block_num * data_block_size) / data_block_size;

    
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
    uint32_t inode_index_list_size = fo->size / sizeof(uint32_t);
    uint32_t new_inode_index = UINT32_MAX;
    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] == 0) {
            new_inode_index = i;
            break;
        }
    }
    if (new_inode_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: Error creating file, no free inode index\n");
        kfree(data_block);
        kfree(inode_table);
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
            ii = i;
            break;
        }
    }
    if (ii == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: Error creating file, no free inode\n");
        kfree(data_block);
        kfree(inode_table);
        return -ENOSPC;
    }
    else
    {
        printk(KERN_INFO "YukiFS: new physical inode %d\n", ii);
    }

    // allocate one free data block for the new file from the bitmap
    uint32_t block_size = sbi->block_size;
    char *bitmap = kmalloc(sbi->bitmap_blocks * block_size, GFP_KERNEL);
    if (!bitmap) {
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }
    if (yukifs_bitmap_read(dir->i_sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    uint32_t new_block = 0;
    for (uint32_t b = 1; b < sbi->block_count; b++) { // block 0 belongs to the root directory
        if (!yukifs_bitmap_test(bitmap, b)) {
            new_block = b;
            break;
        }
    }
    if (new_block == 0) {
        printk(KERN_ERR "YukiFS: Error creating file, no free data block\n");
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -ENOSPC;
    }
    yukifs_bitmap_set(bitmap, new_block);
    if (yukifs_bitmap_write(dir->i_sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    kfree(bitmap);

    // fill the new inode
    memset(&new_fo[ii], 0, sizeof(struct file_object));
    new_fo[ii].in_use = 1;
    new_fo[ii].size = 0;
    new_fo[ii].inner_file = 0;
    new_fo[ii].descriptor = umode_t;
    new_fo[ii].first_block = new_block;
    strscpy(new_fo[ii].name, entry->d_name.name, FS_MAX_LEN);
    struct timespec64 create_ts = current_time(dir);
    new_fo[ii].atime_sec = create_ts.tv_sec;
    new_fo[ii].mtime_sec = create_ts.tv_sec;
    new_fo[ii].ctime_sec = create_ts.tv_sec;

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
    
    yukifs_update_statfs(dir->i_sb, ii, NULL, false); // refresh statfs counters

    printk(KERN_INFO "YukiFS: file %s created successfully\n", entry->d_name.name);

    // The .create callback must bind the new inode to the dentry, otherwise
    // the VFS keeps a negative dentry and do_dentry_open() dereferences a
    // NULL inode -> open() fails even though the file was created on disk.
    struct file_object *fo_copy = kmemdup(&new_fo[ii], sizeof(struct file_object), GFP_KERNEL);
    if (!fo_copy) {
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }

    struct inode *inode = iget_locked(dir->i_sb, 9854 + ii);
    if (!inode) {
        kfree(fo_copy);
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }
    if (inode->i_state & I_NEW) {
        yukifs_fill_inode(dir->i_sb, inode, fo_copy, ii);
        unlock_new_inode(inode);
    } else {
        /* a brand-new inode number cannot already be cached; be safe anyway */
        kfree(fo_copy);
    }

    /* iget_locked() returns the inode with I_NEW cleared after
       unlock_new_inode(): plain d_add() is the correct v6.12 contract here
       (d_instantiate_new() would WARN on a non-I_NEW inode). */
    d_add(entry, inode);

    kfree(data_block);
    kfree(inode_table);

    return 0;
};

/* --- directories --- */

static int yukifs_mkdir(struct mnt_idmap *mnt, struct inode *dir, struct dentry *entry, umode_t mode)
{
    (void)mnt;
    printk(KERN_INFO "YukiFS: mkdir called %s %s %d\n", entry->d_name.name,
           ((struct file_object *)dir->i_private)->name, mode);

    struct superblock_info *sbi = dir->i_sb->s_fs_info;
    struct file_object *dirobj = (struct file_object *)dir->i_private;

    if (entry->d_name.len >= FS_MAX_LEN) {
        printk(KERN_ERR "YukiFS: directory name too long (%u >= %d)\n", entry->d_name.len, FS_MAX_LEN);
        return -ENAMETOOLONG;
    }

    char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
    if (!inode_table)
        return -ENOMEM;
    int inode_table_read = yukifs_inode_table_read(dir->i_sb, inode_table);
    if (inode_table_read < 0) {
        kfree(inode_table);
        return inode_table_read;
    }

    struct file_object *fo = dirobj;
    uint32_t data_blocks_offset = sbi->data_blocks_offset;
    uint32_t dir_data_block_num = fo->first_block;
    uint32_t data_block_size = sbi->block_size;
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_nr = (data_blocks_offset + dir_data_block_num * data_block_size) / data_block_size;

    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    if (!data_block) {
        kfree(inode_table);
        return -ENOMEM;
    }
    if (yukifs_blocks_read(dir->i_sb, data_block_nr, data_block_count, data_block) < 0) {
        printk(KERN_ERR "YukiFS: mkdir Error reading data block %d\n", data_block_nr);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }

    // treat data block as inode index list type uint32_t*
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = fo->size / sizeof(uint32_t);
    uint32_t new_inode_index = UINT32_MAX;
    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] == 0) {
            new_inode_index = i;
            break;
        }
    }
    if (new_inode_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: mkdir - parent directory full\n");
        kfree(data_block);
        kfree(inode_table);
        return -ENOSPC;
    }

    // find the first free inode in the inode table
    struct file_object *new_fo = (struct file_object *)inode_table;
    uint32_t ii = UINT32_MAX;
    for (uint32_t i = 0; i < sbi->total_inodes; i++) {
        if (new_fo[i].in_use == 0) {
            ii = i;
            break;
        }
    }
    if (ii == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: mkdir - no free inode\n");
        kfree(data_block);
        kfree(inode_table);
        return -ENOSPC;
    }

    // allocate one data block for the new directory's slot array
    uint32_t block_size = sbi->block_size;
    char *bitmap = kmalloc(sbi->bitmap_blocks * block_size, GFP_KERNEL);
    if (!bitmap) {
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }
    if (yukifs_bitmap_read(dir->i_sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    uint32_t new_block = 0;
    for (uint32_t b = 1; b < sbi->block_count; b++) { // block 0 belongs to the root directory
        if (!yukifs_bitmap_test(bitmap, b)) {
            new_block = b;
            break;
        }
    }
    if (new_block == 0) {
        printk(KERN_ERR "YukiFS: mkdir - no free data block\n");
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -ENOSPC;
    }
    yukifs_bitmap_set(bitmap, new_block);
    if (yukifs_bitmap_write(dir->i_sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    kfree(bitmap);

    /* the new directory's own slot array must start empty: a reused block may
       still hold stale slots from a deleted directory, which would make rmdir
       believe the directory is non-empty */
    char *zeros = kzalloc(block_size, GFP_KERNEL);
    if (!zeros) {
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }
    uint32_t new_phys = (data_blocks_offset + new_block * block_size) / block_size;
    if (yukifs_blocks_write(dir->i_sb, new_phys, 1, zeros) < 0) {
        kfree(zeros);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    kfree(zeros);

    // fill the new inode
    memset(&new_fo[ii], 0, sizeof(struct file_object));
    new_fo[ii].in_use = 1;
    new_fo[ii].size = block_size; /* one slot-array block; grows later if needed */
    new_fo[ii].inner_file = 0;
    new_fo[ii].descriptor = S_IFDIR | (mode & 0777);
    new_fo[ii].first_block = new_block;
    strscpy(new_fo[ii].name, entry->d_name.name, FS_MAX_LEN);
    struct timespec64 create_ts = current_time(dir);
    new_fo[ii].atime_sec = create_ts.tv_sec;
    new_fo[ii].mtime_sec = create_ts.tv_sec;
    new_fo[ii].ctime_sec = create_ts.tv_sec;

    // write the new inode index to the parent dir data block
    inode_index_list[new_inode_index] = ii;

    // write parent dir data block, inode table, and bitmap back to device
    if (yukifs_blocks_write(dir->i_sb, data_block_nr, data_block_count, data_block) < 0) {
        printk(KERN_ERR "YukiFS: mkdir Error writing data block %d\n", data_block_nr);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    if (yukifs_inode_table_write(dir->i_sb, inode_table) < 0) {
        printk(KERN_ERR "YukiFS: mkdir Error writing inode table\n");
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }

    yukifs_update_statfs(dir->i_sb, ii, NULL, false);

    printk(KERN_INFO "YukiFS: directory %s created successfully\n", entry->d_name.name);

    struct file_object *fo_copy = kmemdup(&new_fo[ii], sizeof(struct file_object), GFP_KERNEL);
    if (!fo_copy) {
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }
    struct inode *inode = iget_locked(dir->i_sb, 9854 + ii);
    if (!inode) {
        kfree(fo_copy);
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }
    if (inode->i_state & I_NEW) {
        yukifs_fill_inode(dir->i_sb, inode, fo_copy, ii);
        unlock_new_inode(inode);
    } else {
        kfree(fo_copy);
    }
    d_add(entry, inode);

    kfree(data_block);
    kfree(inode_table);

    return 0;
}

static int yukifs_rmdir(struct inode *dir, struct dentry *entry)
{
    struct super_block *sb = dir->i_sb;
    struct superblock_info *sbi = (struct superblock_info *)sb->s_fs_info;
    struct file_object *fo = (struct file_object *)dir->i_private;      /* parent */
    struct inode *target = d_inode(entry);
    struct file_object *tfo = (struct file_object *)target->i_private;  /* dir to remove */
    uint32_t inode_idx = (uint32_t)(target->i_ino - 9854);
    uint32_t block_size = sbi->block_size;

    printk(KERN_INFO "YukiFS: rmdir called %s %s (inode idx %u, first block %u)\n",
           entry->d_name.name, fo->name, inode_idx, tfo ? tfo->first_block : 0);

    if (!tfo || !S_ISDIR(tfo->descriptor))
        return -ENOTDIR;

    /* 1) the directory must be empty: every slot in its own array is zero */
    char *dblk = kmalloc(tfo->size, GFP_KERNEL);
    if (!dblk)
        return -ENOMEM;
    if (yukifs_data_blocks_read(sb, tfo, dblk) < 0) {
        kfree(dblk);
        return -EIO;
    }
    uint32_t *slots = (uint32_t *)dblk;
    uint32_t slot_total = tfo->size / sizeof(uint32_t);
    for (uint32_t i = 0; i < slot_total; i++) {
        if (slots[i] != 0) {
            printk(KERN_INFO "YukiFS: rmdir %s - not empty (slot %u)\n", entry->d_name.name, i);
            kfree(dblk);
            return -ENOTEMPTY;
        }
    }
    kfree(dblk);

    /* 2) drop the entry from the parent directory's slot array */
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_nr = (sbi->data_blocks_offset + fo->first_block * block_size) / block_size;
    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    if (!data_block)
        return -ENOMEM;
    if (yukifs_blocks_read(sb, data_block_nr, data_block_count, data_block) < 0) {
        kfree(data_block);
        return -EIO;
    }
    uint32_t *parent_slots = (uint32_t *)data_block;
    uint32_t parent_total = fo->size / sizeof(uint32_t);
    uint32_t dentry_index = UINT32_MAX;
    for (uint32_t i = 0; i < parent_total; i++) {
        if (parent_slots[i] == inode_idx) {
            dentry_index = i;
            break;
        }
    }
    if (dentry_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: rmdir - entry not found in parent directory\n");
        kfree(data_block);
        return -ENOENT;
    }
    parent_slots[dentry_index] = 0;

    /* 3) free the directory's data blocks (bitmap clear + scrub) */
    char *bitmap = kmalloc(sbi->bitmap_blocks * block_size, GFP_KERNEL);
    if (!bitmap) {
        kfree(data_block);
        return -ENOMEM;
    }
    if (yukifs_bitmap_read(sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        return -EIO;
    }
    uint32_t alloc_blocks = tfo->size ? ((tfo->size + block_size - 1) / block_size) : 1;
    for (uint32_t b = 0; b < alloc_blocks; b++) {
        uint32_t blk = tfo->first_block + b;
        if (blk >= sbi->block_count)
            break;
        yukifs_bitmap_clear(bitmap, blk);
        char *zeros = kzalloc(block_size, GFP_KERNEL);
        if (!zeros) {
            kfree(bitmap);
            kfree(data_block);
            return -ENOMEM;
        }
        uint32_t phys_nr = (sbi->data_blocks_offset + blk * block_size) / block_size;
        yukifs_blocks_write(sb, phys_nr, 1, zeros);
        kfree(zeros);
    }
    if (yukifs_bitmap_write(sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        return -EIO;
    }
    kfree(bitmap);

    /* 4) erase the inode-table entry */
    char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
    if (!inode_table) {
        kfree(data_block);
        return -ENOMEM;
    }
    if (yukifs_inode_table_read(sb, inode_table) < 0) {
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    memset(&((struct file_object *)inode_table)[inode_idx], 0, sizeof(struct file_object));

    /* persist: parent dir block, inode table */
    if (yukifs_blocks_write(sb, data_block_nr, data_block_count, data_block) < 0
        || yukifs_inode_table_write(sb, inode_table) < 0) {
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    kfree(data_block);
    kfree(inode_table);

    yukifs_update_statfs(sb, inode_idx, NULL, true);
    set_nlink(target, 0); /* nlink 0 -> generic_delete_inode evicts the inode */

    printk(KERN_INFO "YukiFS: rmdir %s successfully\n", entry->d_name.name);
    return 0;
}

/* --- symbolic links --- */

static int yukifs_symlink(struct mnt_idmap *mnt, struct inode *dir, struct dentry *entry,
                          const char *symname)
{
    (void)mnt;
    size_t len = strlen(symname);
    struct super_block *sb = dir->i_sb;
    struct superblock_info *sbi = sb->s_fs_info;

    /* the target is stored in the file's single data block */
    if (len >= sb->s_blocksize) {
        printk(KERN_ERR "YukiFS: symlink target too long (%zu >= %lu)\n", len, (unsigned long)sb->s_blocksize);
        return -ENAMETOOLONG;
    }

    /* reuse the create() path: allocates the inode-table slot, the data block
       and the directory slot, then binds the dentry (S_IFLNK passes the
       S_ISDIR() guard in create()). */
    int rc = yukifs_create(mnt, dir, entry, S_IFLNK | 0777, false);
    if (rc)
        return rc;

    struct inode *inode = d_inode(entry);
    struct file_object *fo = (struct file_object *)inode->i_private;
    uint32_t inode_idx = (uint32_t)(inode->i_ino - 9854);

    /* write the target string (NUL-terminated) into the first data block */
    char *buf = kzalloc(sb->s_blocksize, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;
    memcpy(buf, symname, len);
    buf[len] = '\0';
    uint32_t phys_nr = (sbi->data_blocks_offset + fo->first_block * sb->s_blocksize) / sb->s_blocksize;
    int wr = yukifs_blocks_write(sb, phys_nr, 1, buf);
    kfree(buf);
    if (wr < 0)
        return -EIO;

    /* POSIX: symlink size = target length, the NUL is not counted */
    fo->size = (uint32_t)len;
    inode->i_size = fo->size;
    inode->i_blocks = 1;
    yukifs_update_statfs(sb, inode_idx, fo, false);

    printk(KERN_INFO "YukiFS: symlink %s -> %s created\n", entry->d_name.name, symname);
    return 0;
}

static void yukifs_put_link(void *buf)
{
    kfree(buf);
}

static const char *yukifs_get_link(struct dentry *dentry, struct inode *inode,
                                   struct delayed_call *done)
{
    (void)dentry;
    struct file_object *fo = (struct file_object *)inode->i_private;
    struct super_block *sb = inode->i_sb;
    struct superblock_info *sbi = sb->s_fs_info;

    if (!fo)
        return ERR_PTR(-ENOENT);

    /* The symlink payload lives in the first data block, but a corrupt
       on-disk size must never drive an allocation smaller than the block:
       yukifs_blocks_read() always fills a whole block. Allocate the full
       block to avoid a heap overflow (kmalloc(size+1) + 1024B write). */
    char *buf = kmalloc(sb->s_blocksize, GFP_KERNEL);
    if (!buf)
        return ERR_PTR(-ENOMEM);

    uint32_t phys_nr = (sbi->data_blocks_offset + fo->first_block * sb->s_blocksize) / sb->s_blocksize;
    if (yukifs_blocks_read(sb, phys_nr, 1, buf) < 0) {
        kfree(buf);
        return ERR_PTR(-EIO);
    }
    size_t n = (size_t)fo->size;
    if (n >= sb->s_blocksize)
        n = sb->s_blocksize - 1; /* defensive cap; valid symlinks are < block size */
    buf[n] = '\0';

    set_delayed_call(done, yukifs_put_link, buf);
    return buf;
}

/* --- hard links (link records in the inode table) --- */

static int yukifs_link(struct dentry *old_dentry, struct inode *dir,
                       struct dentry *new_dentry)
{
    if (new_dentry->d_name.len >= FS_MAX_LEN) {
        printk(KERN_ERR "YukiFS: link name too long (%u >= %d)\n", new_dentry->d_name.len, FS_MAX_LEN);
        return -ENAMETOOLONG;
    }
    /* one directory only */
    if (d_inode(old_dentry->d_parent) != dir)
        return -EINVAL;
    /* the root directory is the only directory and must never be linked */
    if (S_ISDIR(d_inode(old_dentry)->i_mode))
        return -EPERM;

    struct inode *inode = d_inode(old_dentry);
    struct super_block *sb = inode->i_sb;
    struct superblock_info *sbi = sb->s_fs_info;
    struct file_object *fo = (struct file_object *)inode->i_private;
    uint32_t block_size = sb->s_blocksize;
    uint32_t real_idx = (uint32_t)(inode->i_ino - 9854);

    if (!fo)
        return -ENOENT;

    char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
    if (!inode_table)
        return -ENOMEM;
    if (yukifs_inode_table_read(sb, inode_table) < 0) {
        kfree(inode_table);
        return -EIO;
    }
    struct file_object *tab = (struct file_object *)inode_table;

    if (real_idx >= sbi->total_inodes || !tab[real_idx].in_use) {
        kfree(inode_table);
        return -ENOENT;
    }

    /* read the directory data block */
    struct file_object *dirfo = (struct file_object *)dir->i_private;
    uint32_t data_block_count = dirfo->size / block_size;
    uint32_t data_block_nr = (sbi->data_blocks_offset + dirfo->first_block * block_size) / block_size;
    char *data_block = kmalloc(dirfo->size, GFP_KERNEL);
    if (!data_block) {
        kfree(inode_table);
        return -ENOMEM;
    }
    if (yukifs_blocks_read(sb, data_block_nr, data_block_count, data_block) < 0) {
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = block_size / sizeof(uint32_t);

    uint32_t new_slot = UINT32_MAX;
    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] == 0) {
            new_slot = i;
            break;
        }
    }
    if (new_slot == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: link - no free directory slot\n");
        kfree(data_block);
        kfree(inode_table);
        return -ENOSPC;
    }

    uint32_t rec_idx = UINT32_MAX;
    for (uint32_t i = 0; i < sbi->total_inodes; i++) {
        if (tab[i].in_use == 0) {
            rec_idx = i;
            break;
        }
    }
    if (rec_idx == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: link - no free inode table slot\n");
        kfree(data_block);
        kfree(inode_table);
        return -ENOSPC;
    }

    /* the link record: snapshot of the real metadata, its own name, and
       inner_file pointing back at the real inode-table slot */
    tab[rec_idx] = tab[real_idx];
    memset(tab[rec_idx].name, 0, FS_MAX_LEN);
    strscpy(tab[rec_idx].name, new_dentry->d_name.name, FS_MAX_LEN);
    tab[rec_idx].inner_file = (int)real_idx;
    tab[rec_idx].ctime_sec = current_time(inode).tv_sec;

    inode_index_list[new_slot] = rec_idx;

    if (yukifs_blocks_write(sb, data_block_nr, data_block_count, data_block) < 0) {
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    if (yukifs_inode_table_write(sb, inode_table) < 0) {
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    kfree(data_block);
    kfree(inode_table);

    /* VFS bookkeeping: the new dentry aliases the SAME inode object, so both
       names share the cached metadata (size/run/uid/gid/times) */
    inode->i_ctime_sec = current_time(inode).tv_sec;
    inc_nlink(inode);
    ihold(inode);
    new_dentry->d_fsdata = (void *)(uintptr_t)rec_idx;
    d_instantiate(new_dentry, inode);

    printk(KERN_INFO "YukiFS: hard link %s -> %s (inode idx %u, record %u)\n",
           new_dentry->d_name.name, old_dentry->d_name.name, real_idx, rec_idx);
    return 0;
}

static int yukifs_getattr(struct mnt_idmap *mnt, const struct path *path, struct kstat *stat,u32 mask, unsigned int query_flags)
{
    struct inode *inode = path->dentry->d_inode;
    struct file_object *fo = (struct file_object *)inode->i_private;

    if (!fo) {
        printk(KERN_ERR "YukiFS: getattr - inode %lu has no file_object\n", inode->i_ino);
        return -EIO;
    }

    printk(KERN_INFO "YukiFS: getattr dentry: %s inode: %s with inode_num %ld\n", path->dentry->d_name.name,fo->name,inode->i_ino);
    printk("  getattr(): Name: %s, Size: %u, Descriptor: %o, First Block: %u, Inner: %u\n", 
        fo->name,fo->size, fo->descriptor,fo->first_block,fo->inner_file);

    stat->mode = inode->i_mode;
    stat->ino = inode->i_ino;
    stat->size = inode->i_size;
    stat->blocks = inode->i_blocks; // Calculate number of blocks
    stat->blksize = inode->i_sb->s_blocksize;

    /* nlink = live names referencing this inode: the real entry (if its name
       is still present) plus every hard-link record with inner_file == this
       inode. Rebuilt from the on-disk table so it survives remounts.
       Directories: POSIX nlink = 2 + number of subdirectories it contains
       (rebuilt by scanning this directory's own slot array). */
    uint32_t inode_idx = (uint32_t)(inode->i_ino - 9854);
    unsigned int nlink = 1;
    struct superblock_info *sbi = inode->i_sb->s_fs_info;
    if (S_ISDIR(inode->i_mode)) {
        nlink = 2;
        char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
        char *data_block = kmalloc(fo->size, GFP_KERNEL);
        if (inode_table && data_block
            && yukifs_inode_table_read(inode->i_sb, inode_table) >= 0
            && yukifs_data_blocks_read(inode->i_sb, fo, data_block) >= 0) {
            uint32_t *slots = (uint32_t *)data_block;
            struct file_object *tab = (struct file_object *)inode_table;
            uint32_t slot_total = fo->size / sizeof(uint32_t);
            for (uint32_t i = 0; i < slot_total; i++) {
                if (slots[i] != 0 && slots[i] < sbi->total_inodes
                    && S_ISDIR(tab[slots[i]].descriptor))
                    nlink++;
            }
        }
        kfree(data_block);
        kfree(inode_table);
    } else if (path->dentry != path->dentry->d_sb->s_root) {
        char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
        if (inode_table) {
            if (yukifs_inode_table_read(inode->i_sb, inode_table) >= 0) {
                struct file_object *tab = (struct file_object *)inode_table;
                nlink = 0;
                if (inode_idx < sbi->total_inodes && tab[inode_idx].in_use && tab[inode_idx].name[0])
                    nlink = 1;
                for (uint32_t i = 0; i < sbi->total_inodes; i++) {
                    if (i == inode_idx)
                        continue;
                    if (tab[i].in_use && tab[i].name[0] && tab[i].inner_file == (int)inode_idx)
                        nlink++;
                }
            }
            kfree(inode_table);
        }
    }
    stat->nlink = nlink;
    set_nlink(inode, nlink);
    stat->uid = KUIDT_INIT(fo->uid);
    stat->gid = KGIDT_INIT(fo->gid);

    stat->atime = inode_get_atime(inode);
    stat->mtime = inode_get_mtime(inode);
    stat->ctime = inode_get_ctime(inode);

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

    /* names longer than the on-disk buffer can never match a stored name */
    if (len >= FS_MAX_LEN)
        return NULL;

    char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
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
    uint32_t data_block_nr = (data_blocks_offset + dir_data_block_num * data_block_size) / data_block_size;

    
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
    uint32_t inode_index_list_size = fo->size / sizeof(uint32_t);

    // try to find specified file in the directory
    for (i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] != 0) {
            struct file_object *ffo = (struct file_object *)inode_table + inode_index_list[i];
            if (strncmp(name, ffo->name, len) == 0 && len == strlen(ffo->name)) {
                printk(KERN_INFO "YukiFS: Found file %s in directory %s at Inode Index (dentry) %d\n", name, fo->name,i);
                
                // pop the inode from the inode table object
                printk(KERN_INFO "YukiFS: ffo->name %s ffo->size %d ffo->descriptor %o\n", ffo->name, ffo->size,ffo->descriptor);

                /* a hard link record (inner_file != 0) resolves to the real
                   inode: same inode number, same metadata, same data run.
                   The record only carries the additional name. */
                struct file_object *resolved = ffo;
                uint32_t resolved_idx = inode_index_list[i];
                if (ffo->inner_file > 0 && (uint32_t)ffo->inner_file < sbi->total_inodes
                    && ((struct file_object *)inode_table + ffo->inner_file)->in_use) {
                    resolved = (struct file_object *)inode_table + ffo->inner_file;
                    resolved_idx = (uint32_t)ffo->inner_file;
                }

                // Copy the file_object to persistent memory: inode->i_private
                // must not point into the temporary inode_table buffer, which
                // is freed below (use-after-free otherwise).
                struct file_object *ffo_copy = kmemdup(resolved, sizeof(struct file_object), GFP_KERNEL);
                if (!ffo_copy) {
                    printk(KERN_ERR "YukiFS: inode allocation failed (no memory for file_object copy)\n");
                    kfree(data_block);
                    kfree(inode_table);
                    return NULL;
                }

                /* iget_locked() deduplicates inodes by (superblock, ino): all
                   hard link names must alias the SAME inode object, otherwise
                   writes through one name would not be visible via another. */
                struct inode *inode = iget_locked(parent->i_sb, 9854 + resolved_idx);
                if (!inode) {
                    kfree(ffo_copy);
                    printk(KERN_ERR "YukiFS: inode allocation failed\n");
                    kfree(data_block);
                    kfree(inode_table);
                    return ERR_PTR(-ENOMEM);
                }
                if (inode->i_state & I_NEW) {
                    yukifs_fill_inode(parent->i_sb, inode, ffo_copy, resolved_idx);
                    unlock_new_inode(inode);
                } else {
                    kfree(ffo_copy); /* the cached inode already has its copy */
                }

                /* remember the link-record slot so unlink/rename can find the
                   exact name being removed (NULL for the real name) */
                dentry->d_fsdata = (resolved == ffo) ? NULL
                                                     : (void *)(uintptr_t)inode_index_list[i];

                d_add(dentry, inode);

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

    struct file_object *ffo = (struct file_object *)dentry->d_inode->i_private;
    struct inode *file_inode = d_inode(dentry);
    uint32_t inode_idx = (uint32_t)(file_inode->i_ino - 9854);
    uint32_t rec_slot = (uint32_t)(uintptr_t)dentry->d_fsdata; /* 0 = real entry, else link-record slot */

    /* directories are removed via .rmdir (which checks emptiness); never let
       unlink free a directory's blocks while its children still reference them */
    if (S_ISDIR(file_inode->i_mode))
        return -EISDIR;

    printk(KERN_INFO "YukiFS: unlink called %s %s (inode idx %u%s, first block %u)\n",
           dentry->d_name.name, fo->name, inode_idx, rec_slot ? " via link record" : "",
           ffo->first_block);

    // read the data blocks from the device data blocks
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_nr = (sbi->data_blocks_offset + fo->first_block * block_size) / block_size;

    char *data_block = kmalloc(fo->size, GFP_KERNEL);
    if (!data_block)
        return -ENOMEM;
    if (yukifs_blocks_read(parent->i_sb, data_block_nr, data_block_count, data_block) < 0)
    {
        printk(KERN_ERR "YukiFS: unlink Error reading data block %d\n", data_block_nr);
        kfree(data_block);
        return -EIO;
    }

    // try to remove dentry from the directory
    uint32_t *inode_index_list = (uint32_t *)data_block;
    uint32_t inode_index_list_size = fo->size / sizeof(uint32_t);
    uint32_t dentry_index = UINT32_MAX;
    uint32_t slot_val = rec_slot ? rec_slot : inode_idx;
    for (uint32_t i = 0; i < inode_index_list_size; i++) {
        if (inode_index_list[i] == slot_val) {
            dentry_index = i;
            break;
        }
    }
    if (dentry_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: unlink - dentry not found in directory\n");
        kfree(data_block);
        return -ENOENT;
    }

    /* count how many live names still reference this inode */
    char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
    if (!inode_table) {
        kfree(data_block);
        return -ENOMEM;
    }
    if (yukifs_inode_table_read(sb, inode_table) < 0) {
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    struct file_object *tab = (struct file_object *)inode_table;
    uint32_t refs = 0;
    if (inode_idx < sbi->total_inodes && tab[inode_idx].in_use && tab[inode_idx].name[0])
        refs++;
    for (uint32_t i = 0; i < sbi->total_inodes; i++) {
        if (i == inode_idx)
            continue;
        if (tab[i].in_use && tab[i].name[0] && tab[i].inner_file == (int)inode_idx)
            refs++;
    }

    /* this slot always goes away */
    inode_index_list[dentry_index] = 0;

    if (rec_slot) {
        /* unlinking a hard-link name: drop the record only, data stays */
        if (yukifs_blocks_write(sb, data_block_nr, data_block_count, data_block) < 0) {
            printk(KERN_ERR "YukiFS: unlink Error writing data block %d\n", data_block_nr);
            kfree(data_block);
            kfree(inode_table);
            return -EIO;
        }
        memset(&tab[rec_slot], 0, sizeof(struct file_object));
        if (yukifs_inode_table_write(sb, inode_table) < 0) {
            kfree(data_block);
            kfree(inode_table);
            return -EIO;
        }
        printk(KERN_INFO "YukiFS: unlinked link record %u of %s\n", rec_slot, ffo->name);
        kfree(data_block);
        kfree(inode_table);
        drop_nlink(file_inode);
        return 0;
    }

    if (refs > 1) {
        /* the real name is gone but other links remain: keep inode + blocks,
           just blank the name so lookup can no longer match it */
        if (yukifs_blocks_write(sb, data_block_nr, data_block_count, data_block) < 0) {
            printk(KERN_ERR "YukiFS: unlink Error writing data block %d\n", data_block_nr);
            kfree(data_block);
            kfree(inode_table);
            return -EIO;
        }
        tab[inode_idx].name[0] = 0;
        if (yukifs_inode_table_write(sb, inode_table) < 0) {
            kfree(data_block);
            kfree(inode_table);
            return -EIO;
        }
        printk(KERN_INFO "YukiFS: unlinked real name of %s, %u link(s) remain\n", ffo->name, refs - 1);
        kfree(data_block);
        kfree(inode_table);
        drop_nlink(file_inode);
        return 0;
    }

    // last reference: free the file's data blocks in the bitmap and scrub their contents
    uint32_t alloc_blocks = ffo->size ? ((ffo->size + block_size - 1) / block_size) : 1;
    char *bitmap = kmalloc(sbi->bitmap_blocks * block_size, GFP_KERNEL);
    if (!bitmap) {
        kfree(data_block);
        kfree(inode_table);
        return -ENOMEM;
    }
    if (yukifs_bitmap_read(sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    for (uint32_t b = 0; b < alloc_blocks; b++) {
        uint32_t blk = ffo->first_block + b;
        if (blk >= sbi->block_count) break;
        yukifs_bitmap_clear(bitmap, blk);

        char *zeros = kzalloc(block_size, GFP_KERNEL);
        if (!zeros) {
            kfree(bitmap);
            kfree(data_block);
            kfree(inode_table);
            return -ENOMEM;
        }
        uint32_t phys_nr = (sbi->data_blocks_offset + blk * block_size) / block_size;
        yukifs_blocks_write(sb, phys_nr, 1, zeros);
        kfree(zeros);
    }
    if (yukifs_bitmap_write(sb, bitmap) < 0) {
        kfree(bitmap);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    kfree(bitmap);

    // write the data blocks back to the device data blocks
    if (yukifs_blocks_write(sb, data_block_nr, data_block_count, data_block)) {
        printk(KERN_ERR "YukiFS: unlink Error writing data block %d\n", data_block_nr);
        kfree(data_block);
        kfree(inode_table);
        return -EIO;
    }
    else
    {
        printk(KERN_INFO "YukiFS: unlinking dentry %s from dir %s successfully\n", dentry->d_name.name, fo->name);
    }

    ffo->size = 0;
    yukifs_update_statfs(sb, inode_idx, NULL, true);

    kfree(data_block);
    kfree(inode_table);

    drop_nlink(file_inode);
    mark_inode_dirty(file_inode);

    return 0;
}

static int yukifs_rename(struct mnt_idmap *mnt, struct inode *old_dir, struct dentry *old_dentry,
                         struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
    (void)mnt;
    struct super_block *sb = old_dir->i_sb;
    struct superblock_info *sbi = (struct superblock_info *)sb->s_fs_info;
    uint32_t block_size = sbi->block_size;

    printk(KERN_INFO "YukiFS: rename called %s -> %s\n", old_dentry->d_name.name, new_dentry->d_name.name);

    if (old_dentry == new_dentry)
        return 0; /* renaming a file onto itself is a no-op */

    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;

    /* the name buffer is FS_MAX_LEN bytes: at most FS_MAX_LEN-1 chars + NUL */
    if (new_dentry->d_name.len >= FS_MAX_LEN) {
        printk(KERN_ERR "YukiFS: rename target name too long (%u >= %d)\n", new_dentry->d_name.len, FS_MAX_LEN);
        return -ENAMETOOLONG;
    }

    struct inode *old_inode = d_inode(old_dentry);
    if (!old_inode)
        return -ENOENT;
    struct file_object *old_fo = (struct file_object *)old_inode->i_private;
    if (!old_fo)
        return -ENOENT;
    uint32_t old_idx = (uint32_t)(old_inode->i_ino - 9854);

    /* a directory must not be moved into itself or its own subtree */
    if (S_ISDIR(old_inode->i_mode)) {
        struct dentry *walk = new_dentry->d_parent;
        while (walk && walk->d_inode) {
            if (walk->d_inode == old_inode)
                return -EINVAL;
            if (walk == walk->d_sb->s_root)
                break;
            walk = walk->d_parent;
        }
    }

    /* POSIX: renaming over an existing target atomically replaces it */
    struct inode *new_inode = d_inode(new_dentry);
    if (new_inode) {
        if (flags & RENAME_NOREPLACE)
            return -EEXIST;
        if (S_ISDIR(new_inode->i_mode)) {
            /* a directory target must itself be an empty directory */
            int err = yukifs_rmdir(new_dir, new_dentry);
            if (err)
                return err;
        } else {
            int err = yukifs_unlink(new_dir, new_dentry);
            if (err)
                return err;
            drop_nlink(new_inode);
            mark_inode_dirty(new_inode);
        }
    }

    uint32_t rec_slot = (uint32_t)(uintptr_t)old_dentry->d_fsdata;
    uint32_t rename_slot = rec_slot ? rec_slot : old_idx;
    if (rename_slot >= sbi->total_inodes) {
        printk(KERN_ERR "YukiFS: rename - bad slot %u\n", rename_slot);
        return -ENOENT;
    }

    if (old_dir == new_dir) {
        /* same directory: pure name change of the inode-table entry */
        struct file_object *tab = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
        if (!tab)
            return -ENOMEM;
        if (yukifs_inode_table_read(sb, (char *)tab) < 0) {
            kfree(tab);
            return -EIO;
        }
        if (!tab[rename_slot].in_use) {
            kfree(tab);
            return -ENOENT;
        }
        memset(tab[rename_slot].name, 0, FS_MAX_LEN);
        strscpy(tab[rename_slot].name, new_dentry->d_name.name, FS_MAX_LEN);
        tab[rename_slot].ctime_sec = current_time(old_inode).tv_sec;
        if (yukifs_inode_table_write(sb, (char *)tab) < 0) {
            kfree(tab);
            return -EIO;
        }
        kfree(tab);

        /* keep the cached copy in sync (real name only; a record's name lives
           solely on disk and is re-read on the next lookup) */
        if (!rec_slot) {
            memset(old_fo->name, 0, FS_MAX_LEN);
            strscpy(old_fo->name, new_dentry->d_name.name, FS_MAX_LEN);
            old_fo->ctime_sec = current_time(old_inode).tv_sec;
            old_inode->i_ctime_sec = old_fo->ctime_sec;
        }

        printk(KERN_INFO "YukiFS: renamed %s -> %s successfully\n", old_dentry->d_name.name, new_dentry->d_name.name);
        return 0;
    }

    /* cross-directory rename: move the slot between the two directories */
    struct file_object *of = (struct file_object *)old_dir->i_private;
    struct file_object *nf = (struct file_object *)new_dir->i_private;

    char *odir_blk = kmalloc(of->size, GFP_KERNEL);
    char *ndir_blk = kmalloc(nf->size, GFP_KERNEL);
    struct file_object *tab = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
    if (!odir_blk || !ndir_blk || !tab) {
        kfree(odir_blk);
        kfree(ndir_blk);
        kfree(tab);
        return -ENOMEM;
    }
    uint32_t o_nr = (sbi->data_blocks_offset + of->first_block * block_size) / block_size;
    uint32_t n_nr = (sbi->data_blocks_offset + nf->first_block * block_size) / block_size;
    if (yukifs_blocks_read(sb, o_nr, of->size / block_size, odir_blk) < 0
        || yukifs_blocks_read(sb, n_nr, nf->size / block_size, ndir_blk) < 0
        || yukifs_inode_table_read(sb, (char *)tab) < 0) {
        kfree(odir_blk);
        kfree(ndir_blk);
        kfree(tab);
        return -EIO;
    }

    /* find the free slot in the target directory first, so a full directory
       fails without having modified anything */
    uint32_t *nslots = (uint32_t *)ndir_blk;
    uint32_t ntotal = nf->size / sizeof(uint32_t);
    uint32_t new_index = UINT32_MAX;
    for (uint32_t i = 0; i < ntotal; i++) {
        if (nslots[i] == 0) {
            new_index = i;
            break;
        }
    }
    if (new_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: rename - target directory full\n");
        kfree(odir_blk);
        kfree(ndir_blk);
        kfree(tab);
        return -ENOSPC;
    }

    uint32_t *oslots = (uint32_t *)odir_blk;
    uint32_t ototal = of->size / sizeof(uint32_t);
    uint32_t old_index = UINT32_MAX;
    for (uint32_t i = 0; i < ototal; i++) {
        if (oslots[i] == rename_slot) {
            old_index = i;
            break;
        }
    }
    if (old_index == UINT32_MAX) {
        printk(KERN_ERR "YukiFS: rename - entry not found in source directory\n");
        kfree(odir_blk);
        kfree(ndir_blk);
        kfree(tab);
        return -ENOENT;
    }

    oslots[old_index] = 0;
    nslots[new_index] = rename_slot;
    if (!tab[rename_slot].in_use) {
        kfree(odir_blk);
        kfree(ndir_blk);
        kfree(tab);
        return -ENOENT;
    }
    memset(tab[rename_slot].name, 0, FS_MAX_LEN);
    strscpy(tab[rename_slot].name, new_dentry->d_name.name, FS_MAX_LEN);
    tab[rename_slot].ctime_sec = current_time(old_inode).tv_sec;

    if (yukifs_blocks_write(sb, o_nr, of->size / block_size, odir_blk) < 0
        || yukifs_blocks_write(sb, n_nr, nf->size / block_size, ndir_blk) < 0
        || yukifs_inode_table_write(sb, (char *)tab) < 0) {
        kfree(odir_blk);
        kfree(ndir_blk);
        kfree(tab);
        return -EIO;
    }
    kfree(odir_blk);
    kfree(ndir_blk);
    kfree(tab);

    if (!rec_slot) {
        memset(old_fo->name, 0, FS_MAX_LEN);
        strscpy(old_fo->name, new_dentry->d_name.name, FS_MAX_LEN);
        old_fo->ctime_sec = current_time(old_inode).tv_sec;
        old_inode->i_ctime_sec = old_fo->ctime_sec;
    }

    printk(KERN_INFO "YukiFS: renamed %s -> %s (cross-directory) successfully\n",
           old_dentry->d_name.name, new_dentry->d_name.name);
    return 0;
}

static int yukifs_release(struct inode *inode, struct file *file)
{
    struct file_object *fo = (struct file_object *)inode->i_private;
    printk(KERN_INFO "YukiFS: release called %s %s\n",
           file->f_path.dentry->d_name.name, fo ? fo->name : "<null>");
    return 0;
}

/* Grow fo's contiguous block run to need_blocks blocks.
 * Extends in place when the blocks right after the current run are free;
 * otherwise relocates the whole file to a free contiguous run of need_blocks
 * blocks somewhere on disk (the file is never split, since the on-disk
 * format can only represent one run per file).
 * Returns 0, or a negative error (-ENOSPC when no contiguous run exists). */
static int yukifs_grow_blocks(struct super_block *sb, struct file_object *fo, uint32_t need_blocks)
{
    struct superblock_info *sbi = (struct superblock_info *)sb->s_fs_info;
    uint32_t block_size = sbi->block_size;
    uint32_t cur_blocks = fo->size ? ((fo->size + block_size - 1) / block_size) : 1;

    if (need_blocks <= cur_blocks)
        return 0;

    char *bitmap = kmalloc(sbi->bitmap_blocks * block_size, GFP_KERNEL);
    if (!bitmap)
        return -ENOMEM;
    if (yukifs_bitmap_read(sb, bitmap) < 0) {
        kfree(bitmap);
        return -EIO;
    }

    /* fast path: the extension range right after the run is all free */
    bool in_place = true;
    for (uint32_t b = fo->first_block + cur_blocks; b < fo->first_block + need_blocks; b++) {
        if (b >= sbi->block_count || yukifs_bitmap_test(bitmap, b)) {
            in_place = false;
            break;
        }
    }
    if (in_place) {
        for (uint32_t b = fo->first_block + cur_blocks; b < fo->first_block + need_blocks; b++)
            yukifs_bitmap_set(bitmap, b);
        if (yukifs_bitmap_write(sb, bitmap) < 0) {
            kfree(bitmap);
            return -EIO;
        }
        kfree(bitmap);
        return 0;
    }

    /* blocked: scan for a free contiguous run of need_blocks blocks */
    uint32_t new_first = 0;
    uint32_t run = 0;
    for (uint32_t b = 1; b < sbi->block_count; b++) {
        if (!yukifs_bitmap_test(bitmap, b)) {
            if (run == 0)
                new_first = b;
            if (++run >= need_blocks)
                break;
        } else {
            run = 0;
        }
    }
    if (run < need_blocks) {
        printk(KERN_ERR "YukiFS: no contiguous free run of %u blocks for %s\n", need_blocks, fo->name);
        kfree(bitmap);
        return -ENOSPC;
    }

    /* move the existing data to the new run, block by block */
    for (uint32_t i = 0; i < cur_blocks; i++) {
        uint32_t src = fo->first_block + i;
        uint32_t dst = new_first + i;
        uint32_t src_phys = (sbi->data_blocks_offset + src * block_size) / block_size;
        uint32_t dst_phys = (sbi->data_blocks_offset + dst * block_size) / block_size;
        char *tmp = kmalloc(block_size, GFP_KERNEL);
        if (!tmp) {
            kfree(bitmap);
            return -ENOMEM;
        }
        if (yukifs_blocks_read(sb, src_phys, 1, tmp) < 0) {
            printk(KERN_ERR "YukiFS: relocate read failed block %u\n", src);
            kfree(tmp);
            kfree(bitmap);
            return -EIO;
        }
        if (yukifs_blocks_write(sb, dst_phys, 1, tmp) < 0) {
            printk(KERN_ERR "YukiFS: relocate write failed block %u\n", dst);
            kfree(tmp);
            kfree(bitmap);
            return -EIO;
        }
        kfree(tmp);
    }

    /* bitmap: allocate the new run, release the old run */
    for (uint32_t b = new_first; b < new_first + need_blocks; b++)
        yukifs_bitmap_set(bitmap, b);
    for (uint32_t b = fo->first_block; b < fo->first_block + cur_blocks; b++)
        yukifs_bitmap_clear(bitmap, b);
    if (yukifs_bitmap_write(sb, bitmap) < 0) {
        kfree(bitmap);
        return -EIO;
    }
    kfree(bitmap);

    /* scrub the old run so stale data never leaks into future files */
    for (uint32_t b = fo->first_block; b < fo->first_block + cur_blocks; b++) {
        uint32_t phys = (sbi->data_blocks_offset + b * block_size) / block_size;
        char *zeros = kzalloc(block_size, GFP_KERNEL);
        if (zeros) {
            yukifs_blocks_write(sb, phys, 1, zeros);
            kfree(zeros);
        }
    }

    printk(KERN_INFO "YukiFS: relocated %s from block %u to block %u\n", fo->name, fo->first_block, new_first);
    fo->first_block = new_first;
    return 0;
}

static ssize_t yukifs_write(struct file *file, const char __user *buf, size_t len, loff_t *offset)
{
    struct inode *inode = file->f_inode;
    struct file_object *fo = (struct file_object *)inode->i_private;
    struct super_block *sb = inode->i_sb;
    struct superblock_info *sbi = sb->s_fs_info;
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

    if (len == 0)
        return 0;

    uint32_t inode_idx = (uint32_t)(inode->i_ino - 9854);
    loff_t end = *offset + len;

    // grow the file's block run (in place, or relocate when neighbors are taken)
    uint32_t need_blocks = (end + block_size - 1) / block_size;
    if (need_blocks == 0) need_blocks = 1;
    int grow_rc = yukifs_grow_blocks(sb, fo, need_blocks);
    if (grow_rc < 0)
        return grow_rc;

    char *kbuf = kmalloc(block_size, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    char *block_buf = kmalloc(block_size, GFP_KERNEL);
    if (!block_buf) {
        kfree(kbuf);
        return -ENOMEM;
    }

    /* multi-block write: read the old block, overlay the new bytes (so a
       partial overwrite never loses data outside the written range), write back */
    size_t pos = 0;
    while (pos < len) {
        loff_t seg_off = *offset + pos;
        uint32_t blk = fo->first_block + seg_off / block_size;
        uint32_t off_in_blk = seg_off % block_size;
        size_t chunk = len - pos;
        if (chunk > block_size - off_in_blk)
            chunk = block_size - off_in_blk;

        if (copy_from_user(kbuf, buf + pos, chunk)) {
            kfree(block_buf);
            kfree(kbuf);
            return -EFAULT;
        }

        uint32_t physical_block_index = (sbi->data_blocks_offset + blk * block_size) / block_size;
        if (yukifs_blocks_read(sb, physical_block_index, 1, block_buf)) {
            printk(KERN_ERR "YukiFS: Error reading block %u\n", physical_block_index);
            kfree(block_buf);
            kfree(kbuf);
            return -EIO;
        }
        memcpy(block_buf + off_in_blk, kbuf, chunk);
        if (yukifs_blocks_write(sb, physical_block_index, 1, block_buf)) {
            printk(KERN_ERR "YukiFS: Error writing to block %u\n", physical_block_index);
            kfree(block_buf);
            kfree(kbuf);
            return -EIO;
        }
        pos += chunk;
    }

    kfree(block_buf);
    kfree(kbuf);

    if (end > inode->i_size) {
        inode->i_size = end;
        inode->i_blocks = (end + block_size - 1) / block_size;
    }
    fo->size = inode->i_size;

    struct timespec64 write_ts = current_time(inode);
    fo->mtime_sec = write_ts.tv_sec;
    fo->ctime_sec = write_ts.tv_sec;
    inode->i_mtime_sec = fo->mtime_sec;
    inode->i_ctime_sec = fo->ctime_sec;

    file->f_pos = end;
    *offset = end;

    yukifs_update_statfs(sb, inode_idx, fo, false);

    return len;
}

static int yukifs_update_statfs(struct super_block *sb, uint32_t inode_idx,
                                const struct file_object *fo, bool erase)
{
    struct superblock_info *sbi = (struct superblock_info *)sb->s_fs_info;
    uint32_t block_size = sbi->block_size;
    uint32_t inode_table_offset = sbi->inode_table_offset;
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_block_nr = inode_table_offset / block_size;

    char *inode_table = kmalloc(sbi->inode_table_storage_size, GFP_KERNEL);
    if (!inode_table) {
        printk(KERN_ERR "YukiFS: Error allocating inode table\n");
        return -ENOMEM;
    }

    if (yukifs_blocks_read(sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading inode table\n");
        kfree(inode_table);
        return -EIO;
    }

    struct file_object *tab = (struct file_object *)inode_table;
    bool table_changed = false;

    /* persist metadata by inode index, never by name */
    if (inode_idx < sbi->total_inodes) {
        if (erase) {
            if (tab[inode_idx].in_use) {
                memset(&tab[inode_idx], 0, sizeof(struct file_object));
                table_changed = true;
            }
        } else if (fo && tab[inode_idx].in_use) {
            /* persist the whole metadata set (size + mode + owner + times + run) */
            tab[inode_idx].size = fo->size;
            tab[inode_idx].descriptor = fo->descriptor;
            tab[inode_idx].uid = fo->uid;
            tab[inode_idx].gid = fo->gid;
            tab[inode_idx].first_block = fo->first_block;
            tab[inode_idx].atime_sec = fo->atime_sec;
            tab[inode_idx].mtime_sec = fo->mtime_sec;
            tab[inode_idx].ctime_sec = fo->ctime_sec;
            table_changed = true;
        }
    }

    /* real counters: free inodes from the inode table, free blocks from the bitmap */
    uint32_t used_inodes = 0;
    uint32_t used_blocks = 0;
    for (uint32_t i = 0; i < sbi->total_inodes; i++) {
        if (tab[i].in_use) {
            used_inodes++;
            used_blocks += tab[i].size ? ((tab[i].size + block_size - 1) / block_size) : 1;
        }
    }
    sbi->free_inodes = sbi->total_inodes - used_inodes;
    sbi->block_free = sbi->block_count - used_blocks;

    if (table_changed) {
        if (yukifs_blocks_write(sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
        {
            printk(KERN_ERR "YukiFS: Error writing inode table\n");
            kfree(inode_table);
            return -EIO;
        }
    }
    kfree(inode_table);

    /* superblock is always right before the inode table */
    if (yukifs_blocks_write(sb, inode_block_nr - 1, 1, (char *)sbi))
    {
        printk(KERN_ERR "YukiFS: Error writing superblock\n");
        return -EIO;
    }

    return 0;
}

static int yukifs_setattr(struct mnt_idmap *mnt, struct dentry *dentry,
                          struct iattr *attr)
{
    struct inode *inode = d_inode(dentry);
    struct super_block *sb = inode->i_sb;
    struct superblock_info *sbi = sb->s_fs_info;
    struct file_object *fo = (struct file_object *)inode->i_private;
    uint32_t block_size = sb->s_blocksize;
    uint32_t inode_idx = (uint32_t)(inode->i_ino - 9854);

    /* a directory's size is its slot-array length, managed by mkdir/rmdir only */
    if (S_ISDIR(inode->i_mode) && (attr->ia_valid & ATTR_SIZE))
        return -EISDIR;

    if (fo && (attr->ia_valid & ATTR_SIZE)) {
        loff_t new_size = attr->ia_size;
        if (new_size > MAX_LFS_FILESIZE)
            return -EFBIG;

        uint32_t need = new_size ? ((new_size + block_size - 1) / block_size) : 1;
        uint32_t cur = inode->i_size ? ((inode->i_size + block_size - 1) / block_size) : 1;

        if (new_size > inode->i_size) {
            /* grow: extend in place, or relocate when the neighbor blocks are taken */
            if (need > cur) {
                int grow_rc = yukifs_grow_blocks(sb, fo, need);
                if (grow_rc < 0)
                    return grow_rc;
            }
        } else if (new_size < inode->i_size) {
            /* shrink: zero the tail of the kept region and free the tail blocks */
            char *block_buf = kmalloc(block_size, GFP_KERNEL);
            if (!block_buf)
                return -ENOMEM;
            for (uint32_t b = new_size / block_size; b < cur; b++) {
                uint32_t blk = fo->first_block + b;
                uint32_t off0 = (b == (uint32_t)(new_size / block_size)) ? (uint32_t)(new_size % block_size) : 0;
                uint32_t phys_nr = (sbi->data_blocks_offset + blk * block_size) / block_size;
                if (yukifs_blocks_read(sb, phys_nr, 1, block_buf) < 0) {
                    kfree(block_buf);
                    return -EIO;
                }
                memset(block_buf + off0, 0, block_size - off0);
                if (yukifs_blocks_write(sb, phys_nr, 1, block_buf) < 0) {
                    kfree(block_buf);
                    return -EIO;
                }
            }
            kfree(block_buf);

            if (need < cur) {
                char *bitmap = kmalloc(sbi->bitmap_blocks * block_size, GFP_KERNEL);
                if (!bitmap)
                    return -ENOMEM;
                if (yukifs_bitmap_read(sb, bitmap) < 0) {
                    kfree(bitmap);
                    return -EIO;
                }
                for (uint32_t b = fo->first_block + need; b < fo->first_block + cur; b++)
                    yukifs_bitmap_clear(bitmap, b);
                if (yukifs_bitmap_write(sb, bitmap) < 0) {
                    kfree(bitmap);
                    return -EIO;
                }
                kfree(bitmap);
            }
        }

        inode->i_size = new_size;
        inode->i_blocks = need;
        fo->size = new_size;
    }

    /* apply the remaining attribute changes (mode/uid/gid/times) */
    setattr_copy(mnt, inode, attr);
    mark_inode_dirty(inode);

    /* persist the whole metadata set back to the on-disk inode */
    if (fo) {
        fo->descriptor = inode->i_mode;
        fo->uid = inode->i_uid.val;
        fo->gid = inode->i_gid.val;
        fo->atime_sec = inode->i_atime_sec;
        fo->mtime_sec = inode->i_mtime_sec;
        fo->ctime_sec = inode->i_ctime_sec;
        yukifs_update_statfs(sb, inode_idx, fo, false);
    }

    return 0;
}

#pragma endregion

#pragma region File Operation Callback Structures

struct inode_operations yukifs_dir_inode_operations = {
    .lookup = yukifs_lookup,
    .create = yukifs_create,
    .mkdir = yukifs_mkdir,
    .rmdir = yukifs_rmdir,  
    .link = yukifs_link,
    .unlink = yukifs_unlink, 
    .symlink = yukifs_symlink,
    .rename = yukifs_rename,
    .getattr = yukifs_getattr,
    .setattr = yukifs_setattr,
};

struct inode_operations yukifs_file_inode_operations = {
    .unlink = yukifs_unlink, 
    .getattr = yukifs_getattr,
    .setattr = yukifs_setattr,
};

struct inode_operations yukifs_symlink_inode_operations = {
    .get_link = yukifs_get_link,
    .getattr = yukifs_getattr,
    .setattr = yukifs_setattr,
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

    root = iget_locked(sb, 9854 + 0);
    if (!root) {
        kfree(root_fo);
        printk(KERN_ERR "YukiFS: inode allocation failed\n");
        return -ENOMEM;
    }
    if (root->i_state & I_NEW) {
        yukifs_fill_inode(sb, root, root_fo, 0);
        unlock_new_inode(root);
    } else {
        kfree(root_fo);
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

