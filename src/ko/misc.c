// SPDX-License-Identifier: MIT
#include "misc.h"


#pragma region Block IO Operations

// function to convert offset to block count
// this function is not used in the current implementation
/*
uint32_t yukifs_offset2block(struct super_block *sb, uint32_t offset)
{
    struct superblock_info *sbi = sb->s_fs_info;
    uint32_t block_size = sbi->block_size;
    uint32_t block_nr = offset / block_size;
    return block_nr;
};
*/

int yukifs_blocks_read(struct super_block *sb, uint32_t block_nr,uint32_t block_count, char *buf)
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

int yukifs_blocks_write(struct super_block *sb, uint32_t block_nr,uint32_t block_count, char *buf)
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

int yukifs_inode_table_read(struct super_block *sb, char* inode_table)
{
    struct superblock_info *sbi = sb->s_fs_info;

    uint32_t inode_table_offset = sbi->inode_table_offset; 
    //uint32_t inode_table_size = sbi->inode_table_storage_size; // use storage size due to whole blocks read
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_block_nr = inode_table_offset / sbi->block_size;
    
    if(yukifs_blocks_read(sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading inode table\n");
        return -EIO;
    }

    return 0;
}

int yukifs_inode_table_write(struct super_block *sb, char* inode_table)
{
    struct superblock_info *sbi = sb->s_fs_info;

    uint32_t inode_table_offset = sbi->inode_table_offset; 
    //uint32_t inode_table_size = sbi->inode_table_storage_size; // use storage size due to whole blocks read
    uint32_t inode_table_clusters = sbi->inode_table_clusters;
    uint32_t inode_block_nr = inode_table_offset / sbi->block_size;
    
    if(yukifs_blocks_write(sb, inode_block_nr, inode_table_clusters, (char *)inode_table) < 0)
    {
        printk(KERN_ERR "YukiFS: Error writing inode table\n");
        return -EIO;
    }

    return 0;
}

int yukifs_data_blocks_read(struct super_block *sb, struct file_object *fo, char *data_block)
{
    struct superblock_info *sbi = sb->s_fs_info;

    uint32_t data_blocks_offset = sbi->data_blocks_offset;
    uint32_t dir_data_block_num = fo->first_block;

    // read the data blocks from the device data blocks
    uint32_t data_block_size = sbi->block_size;
    uint32_t data_block_count = fo->size / sbi->block_size;
    uint32_t data_block_offset = data_blocks_offset + dir_data_block_num * data_block_size;
    uint32_t data_block_nr = data_block_offset / data_block_size;

    
    if(yukifs_blocks_read(sb, data_block_nr, data_block_count, data_block) < 0)
    {
        printk(KERN_ERR "YukiFS: Error reading data block %d\n", data_block_nr);
        kfree(data_block);
        return -EIO;
    }

    return 0;
}