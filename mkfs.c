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

#include "version.h"
#include "file_table.h"

// Define a temporary struct without INODE_TABLES for writing the initial header
typedef struct {
    unsigned char FS_PADDING[1024];
    unsigned char HIDDEN_INFO_FOR_FS[SUPER_BLOCK_ALIGN_SIZE]; // Using SUPER_BLOCK_ALIGN_SIZE for consistency
    unsigned char SUPER_BLOCK_INFO[SUPER_BLOCK_ALIGN_SIZE];
} initial_fs_header;

#define MKFS_VERSION "1.0"

void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS] <device_or_image_path>\n", prog_name);
    printf("Create a yukifs filesystem.\n\n");
    printf("Options:\n");
    printf("  -y, --yes           Assume yes to all prompts.\n");
    printf("  -t, --try-run       Perform a dry run (simulate on memory).\n");
    printf("  -b, --block-size=SIZE Specify the block size in bytes.\n");
    printf("                        (Default: %d, Min: %d, Max: %d)\n", ACTUAL_FS_BLOCK_SIZE, MINIMAL_BLOCK_SIZE, MAXIMUM_BLOCK_SIZE);
    printf("  -h, --help          Display this help message.\n");
    printf("  -v, --version       Display the version of mkfs.\n");
    printf("\n");
}

int main(int argc, char *argv) {
    int block_size = ACTUAL_FS_BLOCK_SIZE; // Default block size
    const char *device_path = NULL;
    int force_yes = 0; // Flag for the -y option
    int try_run = 0;   // Flag for the -t option
    uint32_t try_run_size = 0;

    static struct option long_options= {
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
                printf("mkfs version %s\n", MKFS_VERSION);
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

    // Prevent specific device names
    if (strcmp(device_path, "/dev/null") == 0 ||
        strcmp(device_path, "/dev/zero") == 0 ||
        strcmp(device_path, "/dev/random") == 0 ||
        strcmp(device_path, "/dev/urandom") == 0) {
        fprintf(stderr, "Error: Cannot use '%s' as a device for mkfs.\n", device_path);
        return 1;
    }

    struct stat path_stat;
    if (!try_run && stat(device_path, &path_stat) != 0) {
        fprintf(stderr, "Error: Device or image file '%s' does not exist or cannot be read.\n", device_path);
        return 1;
    } else if (try_run && stat(device_path, &path_stat) != 0) {
        fprintf(stderr, "Error (Try Run): Device or image file '%s' does not exist or cannot be read.\n", device_path);
        return 1;
    }

    int fd = -1;
    uint8_t *mem_device = NULL;
    uint32_t device_size = 0;
    const char *effective_device_path = device_path;

    if (!try_run) {
        fd = open(device_path, O_WRONLY, 0644); // Do not create or truncate
        if (fd == -1) {
            perror("Error opening device/image");
            return 1;
        }

        long long temp_size;
        if ((temp_size = lseek64(fd, 0, SEEK_END)) != -1) {
            if (temp_size > UINT32_MAX) {
                fprintf(stderr, "Error: Device or image file '%s' exceeds the maximum supported size of 4GiB.\n", device_path);
                close(fd);
                return 1;
            }
            device_size = (uint32_t)temp_size;
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

    size_t initial_header_size = sizeof(initial_fs_header);
    size_t file_object_align_size = FILE_OBJECT_ALIGN_SIZE; // Get the aligned size

    if (device_size < initial_header_size) {
        fprintf(stderr, "Error: Device or image file '%s' does not have enough space (%u bytes) for the header (%zu bytes).\n", effective_device_path, device_size, initial_header_size);
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
                if (fd != -1) close(fd);
                if (try_run && mem_device != NULL) free(mem_device);
                return 0;
            }
            // Check the first character of the response, ignoring case
            if (tolower(response[0]) != 'y') {
                printf("Operation cancelled by user.\n");
                if (fd != -1) close(fd);
                if (try_run && mem_device != NULL) free(mem_device);
                return 0;
            }
        } else {
            fprintf(stderr, "Error reading user input. Aborting.\n");
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

    initial_fs_header fs_header;
    memset(&fs_header, 0, sizeof(initial_fs_header));

    // Initialize Superblock
    struct superblock_info *superblock = (struct superblock_info *)fs_header.SUPER_BLOCK_INFO;
    memcpy(superblock->magic_number, FILESYSTEM_MAGIC_BYTES, sizeof(FILESYSTEM_MAGIC_BYTES));
    superblock->block_size = block_size;

    // Calculate total_inodes (x) and block_count (x) using the provided formulas
    uint32_t x = 0;
    if (device_size > initial_header_size) {
        uint32_t remaining_space = device_size - initial_header_size;
        uint32_t denominator = file_object_align_size + block_size;
        if (denominator > 0) {
            x = remaining_space / denominator;
        }
    }
    superblock->total_inodes = x;
    superblock->block_count = x;

    superblock->block_free = superblock->block_count; // Initially all data blocks are free
    superblock->block_available = superblock->block_count;
    superblock->free_inodes = superblock->total_inodes; // Initially all inodes are free

    // Calculate the size of the inode table
    size_t inode_table_size = file_object_align_size * x;

    // Allocate and Initialize Inode Table
    struct file_object *inode_table = (struct file_object *)malloc(inode_table_size);
    if (inode_table == NULL) {
        perror("Error allocating memory for inode table");
        if (!try_run && fd != -1) close(fd);
        if (try_run && mem_device != NULL) free(mem_device);
        return 1;
    }
    memset(inode_table, 0, inode_table_size);

    // Initialize the first inode for /fs.info
    if (x > 0) {
        strncpy(inode_table[0].name, "/fs.info", FS_MAX_LEN - 1);
        inode_table[0].name[FS_MAX_LEN - 1] = '\0';
        inode_table[0].size = 0;
        inode_table[0].inner_file = 1;
        inode_table[0].descriptor = FILE_DEFAULT_PERMISSION;
        inode_table[0].first_block = 0;
    }

    if (!try_run) {
        // Write the initial header to the device/image
        ssize_t bytes_written_header = write(fd, &fs_header, sizeof(initial_fs_header));
        if (bytes_written_header == -1) {
            perror("Error writing initial header to device/image");
            free(inode_table);
            close(fd);
            return 1;
        }

        if ((size_t)bytes_written_header < sizeof(initial_fs_header)) {
            fprintf(stderr, "Warning: Only %zd bytes of initial header written, expected %zu.\n", bytes_written_header, sizeof(initial_fs_header));
        }

        // Write the Inode Table to the device/image immediately after the header
        ssize_t bytes_written_inode_table = write(fd, inode_table, inode_table_size);
        free(inode_table); // Free the allocated memory for the inode table
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

        // Simulate writing header to memory
        memcpy(mem_device, &fs_header, sizeof(initial_fs_header));

        // Simulate writing inode table to memory
        memcpy(mem_device + sizeof(initial_fs_header), inode_table, inode_table_size);

        free(inode_table);
        free(mem_device);
    }

    return 0;
}