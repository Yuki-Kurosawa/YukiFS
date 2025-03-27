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

#include "../../include/version.h"
#include "../../include/file_table.h"
#include "../../tools/built-in.h"

void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] <device_or_image_path>\n", program_name);
    printf("Options:\n");
    printf("  -h, --help     Show this help message\n");
    printf("  -v, --version  Show version information\n");
}

int extract_info(const char *device_path);
char* convert_arch_to_string(int arch);

int main(int argc, char *argv[])
{
    char *device_path = NULL;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "hv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                printf("infofs version %s\n", INFOFS_VERSION_STRING);
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

    if (optind == argc - 1) {
        device_path = argv[optind];
    } else {
        fprintf(stderr, "Error: You must specify a device or image path.\n");
        print_usage(argv[0]);
        return 1;
    }

    // Check if the device path exists
    struct stat path_stat;
    if (stat(device_path, &path_stat) == -1) {
        fprintf(stderr, "Error: Cannot access '%s': %s\n", device_path, strerror(errno));
        return 1;
    }

    // go for info extraction
    int ret=extract_info(device_path);
    
    return ret;
}

int extract_info(const char *device_path) 
{
    // open the device or image ro
    int fd = open(device_path, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Error: Cannot open '%s': %s\n", device_path, strerror(errno));
        return 1;
    }

    // Get the size of the file
    int64_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size == -1) {
        fprintf(stderr, "Error: Cannot get size of '%s': %s\n", device_path, strerror(errno));
        close(fd);
        return 1;
    }
    if (lseek(fd, 0, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Cannot seek to the beginning of '%s': %s\n", device_path, strerror(errno));
        close(fd);
        return 1;
    }

    // Allocate a buffer to read the file content
    unsigned char *buffer = (unsigned char *)malloc(16*1024);
    if (buffer == NULL) {
        fprintf(stderr, "Error: Cannot allocate memory for file content\n");
        close(fd);
        return 1;
    }

    // Read first 16KB of file into the buffer
    ssize_t bytes_read = read(fd, buffer, 16*1024);
    if (bytes_read == -1) {
        fprintf(stderr, "Error: Cannot read from '%s': %s\n", device_path, strerror(errno));
        free(buffer);
        close(fd);
        return 1;
    }

    // found where 0x55AA and 0xAA55 is to locate hidden data  
    int64_t hidden_data_offset = -1;
    int64_t hidden_data_offset_end = -1;

    for (off_t i = 0; i < bytes_read - 1; ++i) {
        if (buffer[i] == 0x55 && buffer[i + 1] == 0xAA) {
            printf("Found sequence 0x55AA at offset %ld\n", i);
            hidden_data_offset = i;
        }
        if (buffer[i] == 0xAA && buffer[i + 1] == 0x55) {
            printf("Found sequence 0xAA55 at offset %ld\n", i);
            hidden_data_offset_end = i;
        }
    }

    if (hidden_data_offset == -1 && hidden_data_offset_end == -1)
    {
        // failed to find hidden data
        printf("Failed to find hidden data\n");
        return 1;
    }

    // read hidden data from offset
    struct hidden_data_struct *hidden_data = (struct hidden_data_struct *)(buffer + hidden_data_offset);

    // print hidden data
    printf("Hidden data:\n");
    printf("  Magic number: %02X%02X\n", hidden_data->hidden_magic_number[0], hidden_data->hidden_magic_number[1]);
    printf("  FS version: %d.%d.%d\n", hidden_data->fs_version[0], hidden_data->fs_version[1], hidden_data->fs_version[2]);
    printf("  Build tool name: %s\n", hidden_data->fs_build_tool_name);
    printf("  Build tool version: %d.%d.%d\n", hidden_data->fs_build_tool_version[0], hidden_data->fs_build_tool_version[1], hidden_data->fs_build_tool_version[2]);
    printf("  Built-in ELF offset: %u\n", hidden_data->built_in_ELF_offset);
    printf("  Built-in ELF size: %u\n", hidden_data->built_in_ELF_size);
    printf("  Built-in ELF storage size: %u\n", hidden_data->built_in_ELF_storage_size);
    printf("  Hidden data offset: %u\n", hidden_data->hidden_data_offset);
    printf("  Hidden data header size: %u\n", hidden_data->hidden_data_header_size);
    printf("  Hidden data header storage size: %u\n", hidden_data->hidden_data_header_storage_size);
    printf("  Hidden data size: %u\n", hidden_data->hidden_data_size);
    printf("  Hidden data storage size: %u\n", hidden_data->hidden_data_storage_size);    
    printf("  Built-in kernel module version: %s\n", hidden_data->built_in_kernel_module_version);
    printf("  Built-in kernel module offset: %u\n", hidden_data->built_in_kernel_module_offset);
    printf("  Built-in kernel module size: %u\n", hidden_data->built_in_kernel_module_size);
    printf("  Built-in kernel module storage size: %u\n", hidden_data->built_in_kernel_module_storage_size); 
    printf("  Built-in kernel module architecture: %d (%s)\n", hidden_data->built_in_kernel_architechture,
    convert_arch_to_string(hidden_data->built_in_kernel_architechture)); 
    printf("  Superblock offset: %lu\n", hidden_data->superblock_offset);
    printf("  Hidden end magic number: %02X%02X\n", hidden_data->hidden_end_magic_number[0], hidden_data->hidden_end_magic_number[1]);

    // close the device or image
    free(buffer);
    close(fd);
    return 0;
}


char* convert_arch_to_string(int arch)
{
    switch (arch) {
        case ARCH_X86: return "x86/x86_32/i386/i486/i586/i686";
        case ARCH_X86_64: return "x86_64/x64/amd64";
        case ARCH_ARM: return "arm/armv7/armv7l";
        case ARCH_AARCH64: return "aarch64/arm64/armv8";
        case ARCH_RISCV: return "riscv";
        default: return "Unknown";
    }
    return "Unknown";
}