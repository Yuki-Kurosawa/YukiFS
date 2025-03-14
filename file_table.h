#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#define FS_MAX_LEN 8

// define a tiny superblock info
#define FS_BLOCK_SIZE 1 // dd bs value
#define FS_BLOCK_COUNT 1 // dd count value
#define FS_BLOCK_FREE 0
#define FS_BLOCK_AVAILABLE 0
#define FS_TOTAL_INODES 1
#define FS_FREE_INODES 0
// end define superblock info

struct superblock_info {
    uint8_t block_size;
    uint8_t block_count;
    uint8_t block_free;
    uint8_t block_available;
    uint8_t total_inodes;
    uint8_t free_inodes;
};

static const struct superblock_info fs_superblock = {
    .block_size = FS_BLOCK_SIZE,
    .block_count = FS_BLOCK_COUNT,
    .block_free = FS_BLOCK_FREE,
    .block_available = FS_BLOCK_AVAILABLE,
    .total_inodes = FS_TOTAL_INODES,
    .free_inodes = FS_FREE_INODES
};

struct file_table
{
    char name[FS_MAX_LEN]; //file name
    uint8_t size; //file size
    int descriptor; // the drwxrwxrwx thing, permissions & descriptors
    int first_block;
};

#endif