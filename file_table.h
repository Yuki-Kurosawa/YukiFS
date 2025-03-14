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

struct file_table
{
    char name[FS_MAX_LEN]; //file name
    uint8_t size; //file size
    int descriptor; // the drwxrwxrwx thing, permissions & descriptors
    int first_block;
};

#endif