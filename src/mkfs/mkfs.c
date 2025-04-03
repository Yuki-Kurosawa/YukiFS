// SPDX-License-Identifier: MIT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <ctype.h> // For tolower()
#include <getopt.h> // For getopt_long()
#include <sys/types.h> // For getuid()
#include <unistd.h>    // For getuid()
#include <sys/stat.h> 

#include "../../include/version.h"
#include "../../include/file_table.h"
#include "../../tools/built-in.h"

const char filesystem_magic_bytes[8]=FILESYSTEM_MAGIC_BYTES;

void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS] <device_or_image_path>\n", prog_name);
    printf("Create a yukifs filesystem.\n\n");
    printf("Options:\n");
    printf("  -y, --yes           Assume yes to all prompts.\n");
    printf("  -t, --try-run       Perform a dry run (simulate on memory).\n");
    printf("  -b, --block-size=SIZE Specify the block size in bytes.\n");
    printf("                        (Default: %d, Min: %d, Max: %d)\n", DEFAULT_FS_BLOCK_SIZE, MINIMAL_BLOCK_SIZE, MAXIMUM_BLOCK_SIZE);
    printf("  -h, --help          Display this help message.\n");
    printf("  -v, --version       Display the version of mkfs.\n");
    printf("\n");
}

int get_device_type(const char* device_path)
{
    if(strncmp(device_path, "/dev/memory/", strlen("/dev/memory/")) == 0)
    {
        return S_IFBLK;
    }

    struct stat path_stat;
   
    if (stat(device_path, &path_stat) == -1) {
        return 0; 
    }

    return path_stat.st_mode;
}

size_t calc_hidden_data_size(uint32_t block_size)
{
    uint kml=kernel_module_len;
    size_t hidden_data_size = 0;

    // add one more block for hidden data structure
    hidden_data_size += block_size;

    // calculate how many clusters we need for the kernel module
    uint32_t kernel_module_clusters = 0;
    uint32_t mod=kml % block_size;
    if(mod != 0)
    {
        kernel_module_clusters = kml / block_size + 1;
    }
    else{
        kernel_module_clusters = kml / block_size;
    }

    hidden_data_size += kernel_module_clusters * block_size;  
    
    return hidden_data_size;
}

void gen_hidden_data(unsigned char data[], uint32_t size,uint32_t block_size,int device_type)
{

    // initialize the hidden data structure to zero
    memset(data, 0x00, size);

    // generate the hidden data structure
    struct hidden_data_struct *hidden_data=(struct hidden_data_struct *)malloc(sizeof(struct hidden_data_struct));
    memset(hidden_data, 0, sizeof(struct hidden_data_struct));
    
    //set magic numbers use 55AA begins and AA55 ends
    hidden_data->hidden_magic_number[0] = 0x55;
    hidden_data->hidden_magic_number[1] = 0xAA;
    hidden_data->hidden_end_magic_number[0] = 0xAA;
    hidden_data->hidden_end_magic_number[1] = 0x55;

    //set hidden datas
    unsigned char fs_version[3]={YUKIFS_VERSION_MAJOR,YUKIFS_VERSION_MINOR,YUKIFS_VERSION_PATCH};
    memcpy(hidden_data->fs_version,fs_version,3);
    
    memset(hidden_data->fs_build_tool_name, 0x00, 10);
    memcpy(hidden_data->fs_build_tool_name, MKFS_TOOL_NAME, strlen(MKFS_TOOL_NAME));

    unsigned char fs_build_tool_version[3]={MKFS_VERSION_MAJOR,MKFS_VERSION_MINOR,MKFS_VERSION_PATCH};
    memcpy(hidden_data->fs_build_tool_version,fs_build_tool_version,3);

    hidden_data->built_in_ELF_offset=0x0000;
    hidden_data->built_in_ELF_size=S_ISBLK(device_type)?0x0000:built_in_len;
    hidden_data->built_in_ELF_storage_size=block_size;

    hidden_data->hidden_data_offset=block_size;
    hidden_data->hidden_data_header_size=sizeof(struct hidden_data_struct);
    hidden_data->hidden_data_header_storage_size=block_size;
    hidden_data->hidden_data_size=calc_hidden_data_size(block_size);
    hidden_data->hidden_data_storage_size=calc_hidden_data_size(block_size);

    memset(hidden_data->built_in_kernel_module_version, 0x00, 64);
    memcpy(hidden_data->built_in_kernel_module_version,kernel_version_info,kernel_version_info_len);

    hidden_data->built_in_kernel_module_offset=block_size * 2;
    hidden_data->built_in_kernel_module_size=kernel_module_len;

    #ifdef __i386__
    hidden_data->built_in_kernel_architechture=ARCH_X86;
    #elif __x86_64__
    hidden_data->built_in_kernel_architechture=ARCH_X86_64;
    #elif __arm__
    hidden_data->built_in_kernel_architechture=ARCH_ARM;
    #elif __aarch64__
    hidden_data->built_in_kernel_architechture=ARCH_AARCH64;
    #elif __riscv__
    hidden_data->built_in_kernel_architechture=ARCH_RISCV;
    #else
    hidden_data->built_in_kernel_architechture=ARCH_UNKNOWN;
    #endif


    int ko_size=calc_hidden_data_size(block_size)-block_size;
    hidden_data->built_in_kernel_module_storage_size=ko_size;

    hidden_data->superblock_offset=block_size * 2 + ko_size;

    memcpy(data, hidden_data, sizeof(struct hidden_data_struct));
    free(hidden_data);

    // put the kernel module in the hidden data structure at offset block_size
    memcpy(data + block_size, kernel_module, kernel_module_len);
    
}

void gen_fs_padding_data(unsigned char padding[], size_t size, int device_type)
{
    if(S_ISBLK(device_type))
    {
        memset(padding,0x00,size);
    }
    else
    {
        // DO SOME ELF THINGS HERE;

        // embed built-in from built-in.h
        memcpy(padding, built_in, built_in_len);
        memset(padding + built_in_len, 0, size - built_in_len); //zero-pad if necessary
        
    }
}

int gen_fs_header(unsigned char *header, unsigned char padding[], size_t fs_padding_size, unsigned char hidden_data[], size_t hidden_data_size,
    struct superblock_info *super_block, uint32_t block_size)
{
    size_t current_offset = 0;
    
    // 0: FS_PADDING zeros (Now passed separately as 'padding')
    memcpy(header + current_offset, padding, fs_padding_size);
    current_offset += fs_padding_size;
    
    // 1: HIDDEN_DATA
    memcpy(header + current_offset, hidden_data, hidden_data_size);
    current_offset += hidden_data_size;
    
    // 2: super_block info padding to block size or SUPER_BLOCK_ALIGN_SIZE which is bigger
    size_t superblock_size = sizeof(struct superblock_info);
    memcpy(header + current_offset, super_block, superblock_size);
    current_offset += superblock_size;
    
    // Pad the superblock section to the larger of block_size or SUPER_BLOCK_ALIGN_SIZE
    size_t padding_size = (block_size > SUPER_BLOCK_ALIGN_SIZE ? block_size : SUPER_BLOCK_ALIGN_SIZE);

    if (superblock_size < padding_size) {
        memset(header + current_offset, 0, padding_size - superblock_size);
        current_offset += (padding_size - superblock_size);
    }

    return current_offset;
}

int main(int argc, char *argv[]) {
    int block_size = DEFAULT_FS_BLOCK_SIZE; // Default block size
    char *device_path = NULL;
    static int force_yes = 0; // Flag for the -y option
    static int try_run = 0;     // Flag for the -t option
    uint32_t try_run_size = 0;

    static struct option long_options[]= {
        {"yes", no_argument, &force_yes, 1},
        {"try-run", no_argument, &try_run, 1},
        {"block-size", required_argument, 0, 'b'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "ytb:hv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'y':
                force_yes = 1;
                break;
            case 't':
                try_run = 1;
                break;
            case 'b':
                block_size = atoi(optarg);
                if (block_size < MINIMAL_BLOCK_SIZE || block_size > MAXIMUM_BLOCK_SIZE) {
                    fprintf(stderr, "Error: Block size must be between %d and %d bytes.\n", MINIMAL_BLOCK_SIZE, MAXIMUM_BLOCK_SIZE);
                    return 1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                printf("mkfs version %s\n", MKFS_VERSION_STRING);
                return 0;
            case '?':
                print_usage(argv[0]);
                return 1;
            default:
                fprintf(stderr, "Error: Unknown option.\n");
                print_usage(argv[0]);
                return 1;
        }
    }

    if (try_run && force_yes) {
        fprintf(stderr, "Error: Options -t/--try-run and -y/--yes cannot be used together.\n");
        return 1;
    }

    if (getuid() != 0) {
        fprintf(stderr, "Error: mkfs must be run as root.\n");
        return 1;
    }

    if (optind == argc - 1) {
        device_path = argv[optind];
        if (strncmp(device_path, "/dev/memory/", strlen("/dev/memory/")) == 0) {
            try_run = 1;
            char *size_str = device_path + strlen("/dev/memory/");
            if (size_str[0] != '\0') {
                try_run_size = strtoul(size_str, NULL, 10);
                if (try_run_size == 0) {
                    fprintf(stderr, "Error: Invalid memory size specified in device path '%s'.\n", device_path);
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: Missing memory size in device path '%s'.\n", device_path);
                return 1;
            }
        }
    } else {
        fprintf(stderr, "Error: Missing device or image path.\n");
        print_usage(argv[0]);
        return 1;
    }

    // Prevent specific devices types
    if (!S_ISBLK(get_device_type(device_path)) && !S_ISREG(get_device_type(device_path))) {
        fprintf(stderr, "Error: Cannot use '%s' as a device or a image file for mkfs.\n", device_path);
        return 1;
    }

    struct stat path_stat;
    if(strncmp(device_path, "/dev/memory/", strlen("/dev/memory/")) == -1)
    {
        
        if (!try_run && stat(device_path, &path_stat) != 0) {
            fprintf(stderr, "Error: Device or image file '%s' does not exist or cannot be read.\n", device_path);
            return 1;
        } else if (try_run && stat(device_path, &path_stat) != 0) {
            fprintf(stderr, "Error (Try Run): Device or image file '%s' does not exist or cannot be read.\n", device_path);
            return 1;
        }
    }

    int fd = -1;
    uint8_t *mem_device = NULL;
    size_t device_size = 0;
    const char *effective_device_path = device_path;

    if (!try_run) {
        fd = open(device_path, O_WRONLY, 0644); // Do not create or truncate
        if (fd == -1) {
            perror("Error opening device/image");
            return 1;
        }

        size_t temp_size;
        if ((temp_size = lseek(fd, 0, SEEK_END)) != -1) {
            if (temp_size > UINT32_MAX) {
                fprintf(stderr, "Error: Device or image file '%s' exceeds the maximum supported size of 4GiB.\n", device_path);
                close(fd);
                return 1;
            }
            device_size = temp_size;
            if (lseek(fd, 0, SEEK_SET) == -1) { // Reset to beginning
                perror("Error seeking file");
                close(fd);
                return 1;
            }
        } else {
            perror("Error seeking file size");
            close(fd);
            return 1;
        }
    } else {
        // Try run mode: Allocate memory
        if (path_stat.st_size > UINT32_MAX) {
            fprintf(stderr, "Error (Try Run): Device or image file '%s' exceeds the maximum supported size of 4GiB.\n", device_path);
            return 1;
        }
        device_size = try_run_size > 0 ? try_run_size : (uint32_t)path_stat.st_size;

        mem_device = (uint8_t *)malloc(device_size);
        if (mem_device == NULL) {
            perror("Error allocating memory for try run");
            return 1;
        }
        fd = 1; // Use a dummy fd to indicate memory allocation
        effective_device_path = device_path;
    }

    // Calculate the size of the filesystem header
    size_t fs_padding_size = (block_size > FS_PADDING_SIZE ? block_size : FS_PADDING_SIZE);
    size_t hidden_data_size = calc_hidden_data_size(block_size);
    size_t superblock_padded_size = (block_size > SUPER_BLOCK_ALIGN_SIZE ? block_size : SUPER_BLOCK_ALIGN_SIZE);
    size_t header_data_size = hidden_data_size + superblock_padded_size + fs_padding_size;
    size_t initial_header_size = header_data_size;

    // Allocate memory for the padding
    unsigned char *fs_padding_data = (unsigned char *)malloc(fs_padding_size);
    if (fs_padding_data == NULL) {
        perror("Error allocating memory for filesystem padding");
        if (!try_run && fd != -1) close(fd);
        if (try_run && mem_device != NULL) free(mem_device);
        return 1;
    }

    
    gen_fs_padding_data(fs_padding_data,fs_padding_size,get_device_type(device_path));
    
    // Allocate memory for the header
    unsigned char *fs_header_data = (unsigned char *)malloc(header_data_size);
    if (fs_header_data == NULL) {
        perror("Error allocating memory for filesystem header");
        free(fs_padding_data);
        if (!try_run && fd != -1) close(fd);
        if (try_run && mem_device != NULL) free(mem_device);
        return 1;
    }
    
    memset(fs_header_data, 0, initial_header_size);
    
    // generate hidden_data_for_fs here
    
    unsigned char *hidden_data_buffer = (unsigned char *)malloc(hidden_data_size);
    if (hidden_data_buffer == NULL) {
        perror("Error allocating memory for hidden data");
        free(fs_padding_data);
        free(fs_header_data);
        if (!try_run && fd != -1) close(fd);
        if (try_run && mem_device != NULL) free(mem_device);
        return 1;
    }

    gen_hidden_data(hidden_data_buffer, hidden_data_size, block_size,get_device_type(device_path));
 
    // Initialize Superblock
    struct superblock_info superblock;
    memset(&superblock, 0, sizeof(struct superblock_info));
    memcpy(superblock.magic_number, filesystem_magic_bytes, sizeof(filesystem_magic_bytes));
    superblock.block_size = block_size;

    // Calculate total_inodes (x) and block_count (x) using the provided formulas
    uint32_t x = 0;
    uint32_t file_object_align_size = FILE_OBJECT_ALIGN_SIZE; // Get the aligned size
    if (device_size > initial_header_size) {
        uint32_t remaining_space = device_size - initial_header_size;
        uint32_t block_count = remaining_space / block_size; //block_count means blocks for inode_tables and datas

        // solve x for block_count=file_object_align_size*x /block_size + x
        x = (block_count * block_size) / (file_object_align_size + block_size);

    }    

    superblock.total_inodes = x;
    superblock.block_count = x;

    superblock.block_free = superblock.block_count -1; // Initially all data blocks are free except for /
    superblock.free_inodes = superblock.total_inodes - 1; // Initially all inodes are free except for /
    superblock.inode_table_size = file_object_align_size * x;

    {
        int64_t inode_table_size = superblock.inode_table_size;
        uint32_t inode_table_clusters = 0;
        uint32_t mod=inode_table_size % superblock.block_size;
        if(mod != 0)
        {
            inode_table_clusters = inode_table_size / superblock.block_size + 1;
        }
        else{
            inode_table_clusters = inode_table_size / superblock.block_size;
        }
        superblock.inode_table_clusters = inode_table_clusters;
        superblock.inode_table_storage_size = inode_table_clusters * superblock.block_size;
    }

    // Generate the file system header
    size_t actual_header_size = gen_fs_header(fs_header_data, fs_padding_data, fs_padding_size, hidden_data_buffer, hidden_data_size, &superblock, block_size);
    superblock.inode_table_offset = actual_header_size;
    superblock.data_blocks_offset = actual_header_size + superblock.inode_table_storage_size;
    superblock.data_blocks_total_size = superblock.block_count * block_size;
    superblock.data_blocks_end_offset = superblock.data_blocks_offset + superblock.data_blocks_total_size;
    superblock.unallocated_space_size = device_size - superblock.data_blocks_end_offset;

    // Generate the file system header again to include the actual size of the header
    actual_header_size = gen_fs_header(fs_header_data, fs_padding_data, fs_padding_size, hidden_data_buffer, hidden_data_size, &superblock, block_size);

    // Free the allocated memory for the hidden data buffer
    free(hidden_data_buffer); // Free the hidden data buffer
   
    if (device_size < initial_header_size) {
        fprintf(stderr, "Error: Device or image file '%s' does not have enough space (%zu bytes) for the header (%zu bytes).\n", effective_device_path, device_size, initial_header_size);
        free(fs_padding_data);
        free(fs_header_data);
        if (!try_run && fd != -1) close(fd);
        if (try_run && mem_device != NULL) free(mem_device);
        return 1;
    }

    // Prompt for confirmation before writing zeros if -y is not provided and not a try run
    if (!force_yes && !try_run) {
        printf("This operation will erase all the data from device %s.\n", effective_device_path);
        printf("Operations can't be reverted and data is not recoverable.\n");
        printf("Continue the operation [y/N]: ");

        char response[4]; // To be safe, read up to 3 characters + null terminator
        if (fgets(response, sizeof(response), stdin) != NULL) {
            // Check if only newline was entered (user just pressed Enter)
            if (response[0] == '\n') {
                printf("Operation cancelled by user (Enter pressed).\n");
                free(fs_padding_data);
                free(fs_header_data);
                if (fd != -1) close(fd);
                if (try_run && mem_device != NULL) free(mem_device);
                return 0;
            }
            // Check the first character of the response, ignoring case
            if (tolower(response[0]) != 'y') {
                printf("Operation cancelled by user.\n");
                free(fs_padding_data);
                free(fs_header_data);
                if (fd != -1) close(fd);
                if (try_run && mem_device != NULL) free(mem_device);
                return 0;
            }
        } else {
            fprintf(stderr, "Error reading user input. Aborting.\n");
            free(fs_padding_data);
            free(fs_header_data);
            if (fd != -1) close(fd);
            if (try_run && mem_device != NULL) free(mem_device);
            return 1;
        }
    } else if (!try_run) {
        printf("Force yes enabled, proceeding without confirmation.\n");
    }

    if (!try_run) {
        printf("Writing zeros to the device/image...\n");
        unsigned char zero_buffer[4096];
        memset(zero_buffer, 0, sizeof(zero_buffer));
        uint32_t bytes_written_zero = 0;
        while (bytes_written_zero < device_size) {
            uint32_t write_size = sizeof(zero_buffer);
            if (bytes_written_zero + write_size > device_size) {
                write_size = device_size - bytes_written_zero;
            }
            ssize_t written = write(fd, zero_buffer, write_size);
            if (written == -1) {
                perror("Error writing zeros to device/image");
                free(fs_padding_data);
                free(fs_header_data);
                close(fd);
                return 1;
            }
            bytes_written_zero += written;
        }
        printf("Finished writing zeros.\n");
    } else {
        printf("Writing zeros to the simulated device...\n");
        unsigned char zero_buffer[4096];
        memset(zero_buffer, 0, sizeof(zero_buffer));
        uint32_t bytes_written_zero = 0;
        while (bytes_written_zero < device_size) {
            uint32_t write_size = sizeof(zero_buffer);
            if (bytes_written_zero + write_size > device_size) {
                write_size = device_size - bytes_written_zero;
            }
            memcpy(mem_device + bytes_written_zero, zero_buffer, write_size);
            bytes_written_zero += write_size;
        }
        printf("Finished writing zeros to the simulated device.\n");
    }

    // Calculate the size of the inode table
    size_t inode_table_size = file_object_align_size * x;

    // Allocate and Initialize Inode Table
    struct file_object *inode_table = (struct file_object *)malloc(inode_table_size);
    if (inode_table == NULL) {
        perror("Error allocating memory for inode table");
        free(fs_padding_data);
        free(fs_header_data);
        if (!try_run && fd != -1) close(fd);
        if (try_run && mem_device != NULL) free(mem_device);
        return 1;
    }
    memset(inode_table, 0, inode_table_size);  

    // Initialize the inode table with root directory
    struct file_object root_dir;
    memset(&root_dir, 0, sizeof(struct file_object));
    root_dir.size = block_size;
    root_dir.inner_file = 0;
    root_dir.name[0] = '\0';
    root_dir.descriptor = S_IFDIR | 0777;
    root_dir.first_block = 0;
    inode_table[0] = root_dir;



    if (!try_run) {
        lseek(fd, 0, SEEK_SET); //back to byte 0;
        printf("Writing initial header to the device/image...\n");
        // Write the header data (which now includes the padding at the beginning)        
        ssize_t bytes_written_header = write(fd, fs_header_data, actual_header_size);
        if (bytes_written_header == -1) {
            perror("Error writing filesystem header to device/image");
            free(inode_table);
            free(fs_padding_data);
            free(fs_header_data);
            close(fd);
            return 1;
        }
        if ((size_t)bytes_written_header < actual_header_size) {
            fprintf(stderr, "Warning: Only %zd bytes of filesystem header written, expected %zu.\n", bytes_written_header, actual_header_size);
        }

        printf("Writing inode table to the device/image...\n");
        // Write the Inode Table to the device/image immediately after the header
      
        ssize_t bytes_written_inode_table = write(fd, inode_table, inode_table_size);
        free(inode_table); // Free the allocated memory for the inode table
        free(fs_padding_data); // Free the allocated memory for the filesystem padding
        free(fs_header_data); // Free the allocated memory for the filesystem header
  
        if (bytes_written_inode_table == -1) {
            perror("Error writing inode table to device/image");
            close(fd);
            return 1;
        }

        if ((size_t)bytes_written_inode_table < inode_table_size) {
            fprintf(stderr, "Warning: Only %zd bytes of inode table written, expected %zu.\n", bytes_written_inode_table, inode_table_size);
        }

        printf("yukifs filesystem created successfully on %s with block size %d, total inodes/blocks: %u\n", effective_device_path, block_size, x);

        close(fd);
    } else {
        printf("yukifs filesystem would be created on %s with block size %d, total inodes/blocks: %u\n", effective_device_path, block_size, x);

        // Simulate writing header to memory (which now includes padding)
        memcpy(mem_device, fs_header_data, actual_header_size);

        // Simulate writing inode table to memory
        memcpy(mem_device + actual_header_size, inode_table, inode_table_size);        

        free(inode_table);
        free(fs_padding_data);
        free(fs_header_data);
        free(mem_device);
    }

    return 0;
}
