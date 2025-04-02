// SPDX-License-Identifier: MIT

#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#define FS_MAX_LEN 8

#define FILESYSTEM_MAGIC_NUMBER 0x59554B49 // FILESYSTEM MAGIC "YUKI"
#define FILESYSTEM_MAGIC_BYTES {0x59,0x55,0x4B,0x49,0x00,0x00,0x00,0x00} // FILESYSTEM MAGIC "YUKI" FOR SUPERBLOCK INFO STRUCT
#define FILE_DEFAULT_PERMISSION 0755
#define FILE_OBJECT_ALIGN_SIZE 32
#define SUPER_BLOCK_ALIGN_SIZE 512
#define MINIMAL_BLOCK_SIZE 1024
#define MAXIMUM_BLOCK_SIZE 8192
#define FS_PADDING_SIZE 1024
#define MAX_INODE_COUNTS 1 // this value will be calculated by mkfs.yukifs with the actual devices
                           // like /dev/sda1 or /opt/yukifs/fsimage.img
                           // writes to superblock_info.total_inodes
#define DEFAULT_FS_BLOCK_SIZE 1024 // this value will be used by mkfs.yukifs with the actual devices 
                                  // like /dev/sda1 or /opt/yukifs/fsimage.img when no block size provided
                                  // writes to superblock_info.block_size
                                  // then calcuate and write superblock_info.block_count with calcuations with size of devices/images minus size of the data_struct_before_actual_data object

// built-in architechtures
#define ARCH_UNKNOWN 0x00
#define ARCH_X86 0x01 // x86, drop support later
#define ARCH_X86_64 0x02 // x86_64
#define ARCH_ARM 0x03 // ARM, drop support later
#define ARCH_AARCH64 0x04 // AARCH64
#define ARCH_RISCV 0x05 // RISCV, add support later

struct superblock_info {
    unsigned char magic_number[8];
    uint32_t block_size;
    uint32_t block_count;
    uint32_t block_free;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t inode_table_size;
    uint32_t inode_table_clusters;
    uint32_t inode_table_storage_size;
    uint32_t inode_table_offset;
    uint32_t data_blocks_offset;
    uint32_t data_blocks_total_size;
    uint32_t data_blocks_end_offset;
    uint32_t unallocated_space_size;
};

struct file_object
{
    char name[FS_MAX_LEN]; //file name
    uint32_t size; //file size
    int inner_file;// determine the file is a builtin file.
    int descriptor; // the drwxrwxrwx thing, permissions & descriptors
    unsigned int first_block;
};


//this struct is write to device/image directly begin from the end of the built-in-data of the device/image
struct hidden_data_struct
{
    unsigned char hidden_magic_number[2]; // always 0x55AA

    unsigned char fs_version[3]; // yukifs version
    unsigned char fs_build_tool_name[10]; // image build tool name
    unsigned char fs_build_tool_version[3]; // image build tool version
    uint32_t built_in_ELF_offset;// the built-in ELF offset in the image, always 0x0000
    uint32_t built_in_ELF_size;// the built-in ELF size 
    uint32_t built_in_ELF_storage_size;// the built-in ELF size after align
    uint32_t hidden_data_offset;// the offset of hidden data in the image
    uint32_t hidden_data_header_size;// the size of hidden data header
    uint32_t hidden_data_header_storage_size;// the size of hidden data header after align
    uint32_t hidden_data_size;// the size of hidden data
    uint32_t hidden_data_storage_size;// the size of hidden data after align
    unsigned char built_in_kernel_module_version[64]; // the built-in kernel module version, actually the kernel version when the image was built
    uint32_t built_in_kernel_module_offset;// the built-in kernel module offset in the image
    uint32_t built_in_kernel_module_size;// the built-in kernel module size
    uint32_t built_in_kernel_module_storage_size;// the built-in kernel module size after align
    uint8_t built_in_kernel_architechture; // the built-in kernel architecture, actually the kernel arch when the image was built
    uint64_t superblock_offset; // the offset of superblock
    
    unsigned char hidden_end_magic_number[2]; //always 0xAA55
};

#endif