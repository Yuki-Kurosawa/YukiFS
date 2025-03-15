#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#define FS_MAX_LEN 8

#define FILESYSTEM_MAGIC_NUMBER 0x59554B49 // FILESYSTEM MAGIC "YUKI"
#define FILESYSTEM_MAGIC_BYTES {0x59,0x55,0x4B,0x49,0x00,0x00,0x00,0x00} // FILESYSTEM MAGIC "YUKI" FOR SUPERBLOCK INFO STRUCT
#define FILE_DEFAULT_PERMISSION 0755
#define FILE_OBJECT_ALIGN_SIZE 32
#define SUPER_BLOCK_ALIGN_SIZE 512
#define MINIMAL_BLOCK_SIZE 512
#define MAXIMUM_BLOCK_SIZE 2048
#define INODE_COUNTS 1 // this value will be calculated by mkfs.yukifs with the actual devices.
                       // like /dev/sda1 or /opt/yukifs/fsimage.img
#define ACTUAL_FS_BLOCK_SIZE 512 // this value will be calculated by mkfs.yukifs with the actual devices.
                                 // like /dev/sda1 or /opt/yukifs/fsimage.img


struct superblock_info {
    unsigned char magic_number[8];
    uint8_t block_size;
    uint8_t block_count;
    uint8_t block_free;
    uint8_t block_available;
    uint8_t total_inodes;
    uint8_t free_inodes;
};

struct file_object
{
    char name[FS_MAX_LEN]; //file name
    uint8_t size; //file size
    int inner_file;// determine the file is a builtin file.
    int descriptor; // the drwxrwxrwx thing, permissions & descriptors
    unsigned int first_block;
};

struct data_struct_before_actual_data
{
    unsigned char FS_PADDING[1024];
    unsigned char HIDDEN_INFO_FOR_FS[ACTUAL_FS_BLOCK_SIZE];
    unsigned char SUPER_BLOCK_INFO[SUPER_BLOCK_ALIGN_SIZE];
    unsigned char INODE_TABLES[FILE_OBJECT_ALIGN_SIZE*INODE_COUNTS];
    
};

#endif