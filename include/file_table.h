// SPDX-License-Identifier: MIT

#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#define FS_MAX_LEN 8

#define FILESYSTEM_MAGIC_NUMBER 0x59554B49 // FILESYSTEM MAGIC "YUKI"
#define FILESYSTEM_MAGIC_BYTES {0x59,0x55,0x4B,0x49,0x00,0x00,0x00,0x00} // FILESYSTEM MAGIC "YUKI" FOR SUPERBLOCK INFO STRUCT
#define FILE_DEFAULT_PERMISSION 0755
#define FILE_OBJECT_ALIGN_SIZE 32
#define SUPER_BLOCK_ALIGN_SIZE 512
#define MINIMAL_BLOCK_SIZE 65536
#define MAXIMUM_BLOCK_SIZE 131072
#define FS_PADDING_SIZE 1024
#define MAX_INODE_COUNTS 1 // this value will be calculated by mkfs.yukifs with the actual devices
                           // like /dev/sda1 or /opt/yukifs/fsimage.img
                           // writes to superblock_info.total_inodes
#define DEFAULT_FS_BLOCK_SIZE 512 // this value will be used by mkfs.yukifs with the actual devices 
                                  // like /dev/sda1 or /opt/yukifs/fsimage.img when no block size provided
                                  // writes to superblock_info.block_size
                                  // then calcuate and write superblock_info.block_count with calcuations with size of devices/images minus size of the data_struct_before_actual_data object


struct superblock_info {
    unsigned char magic_number[8];
    uint32_t block_size;
    uint32_t block_count;
    uint32_t block_free;
    uint32_t block_available;
    uint32_t total_inodes;
    uint32_t free_inodes;
};

struct file_object
{
    char name[FS_MAX_LEN]; //file name
    uint32_t size; //file size
    int inner_file;// determine the file is a builtin file.
    int descriptor; // the drwxrwxrwx thing, permissions & descriptors
    unsigned int first_block;
};

//this struct is write to device/image directly begin from byte 0 of the device/image
//this struct will be removed later due to dynamic size
struct fs_header_data_struct
{
    unsigned char FS_PADDING[1024];
    unsigned char HIDDEN_INFO_FOR_FS[DEFAULT_FS_BLOCK_SIZE];
    unsigned char SUPER_BLOCK_INFO[SUPER_BLOCK_ALIGN_SIZE];    
};

#endif