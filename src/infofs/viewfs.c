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
#include <stdbool.h>

#include "../../include/version.h"
#include "../../include/file_table.h"
#include "../../tools/built-in.h"

#define DEFAULT_COUNT -1 // Indicate that count should default to block size
#define DEFAULT_FORMAT "hex"

// Structure to hold the parsed command-line arguments
struct viewfs_args {
    char *input_file;
    char *output_file;
    uint32_t block_num;
    uint32_t count;
    uint32_t skip;
    char *format;
    int help;
};

// Function to print the usage message
void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -i, --if=<filename>    Specify the input file (disk image).\n");
    printf("  -o, --of=<filename>    Specify the output file (optional, defaults to stdout).\n");
    printf("  -b, --block-num=<number> Specify the starting block number (required).\n");
    printf("  -c, --count=<number>   Specify the number of blocks to read (optional, defaults to block size).\n");
    printf("  -s, --skip=<number>    Specify the number of bytes to skip from the beginning of the read blocks (optional, default 0).\n");
    printf("  -f, --format=<hex|text> Specify the output format (optional, default: hex).\n");
    printf("  -h, --help             Display this help message.\n");
    printf("  -v, --version             Display version info.\n");
}

int parse_args(int argc, char *argv[], struct viewfs_args *args) {
    int option_index = 0;
    int c;

    static struct option long_options[] = {
        {"if",        required_argument, 0, 'i'},
        {"of",        required_argument, 0, 'o'},
        {"block-num", required_argument, 0, 'b'},
        {"count",     required_argument, 0, 'c'},
        {"skip",      required_argument, 0, 's'},
        {"take",      required_argument, 0, 't'},
        {"format",    required_argument, 0, 'f'},
        {"help",      no_argument,       0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    // Initialize arguments with default values
    args->input_file = NULL;
    args->output_file = NULL;
    args->block_num = -1; // Indicate not set
    args->count = DEFAULT_COUNT;
    args->skip = 0;
    args->format = strdup(DEFAULT_FORMAT);
    args->help = 0;

    while ((c = getopt_long(argc, argv, "i:o:b:c:s:t:f:hv", long_options, &option_index)) != -1) {
        switch (c) {
            case 'i':
                args->input_file = strdup(optarg);
                break;
            case 'o':
                args->output_file = strdup(optarg);
                break;
            case 'b':
                args->block_num = strtol(optarg, NULL, 10);
                if (args->block_num < 0 && errno == ERANGE) {
                    fprintf(stderr, "Error: Invalid block number: %s\n", optarg);
                    return -1;
                }
                break;
            case 'c':
                args->count = strtol(optarg, NULL, 10);
                if (args->count < 0 && errno == ERANGE) {
                    fprintf(stderr, "Error: Invalid count: %s\n", optarg);
                    return -1;
                }
                break;
            case 's':
                args->skip = strtol(optarg, NULL, 10);
                if (args->skip < 0 && errno == ERANGE) {
                    fprintf(stderr, "Error: Invalid skip value: %s\n", optarg);
                    return -1;
                }
                break;
            case 'f':
                if (strcmp(optarg, "hex") == 0 || strcmp(optarg, "text") == 0) {
                    free(args->format);
                    args->format = strdup(optarg);
                } else {
                    fprintf(stderr, "Error: Invalid format: %s (must be 'hex' or 'text')\n", optarg);
                    return -1;
                }
                break;
            case 'h':
                args->help = 1;
                break;
            case 'v':
                printf("viewfs version 0.1\n");
                return 0; 
            case '?':
                // getopt_long already printed an error message
                return -1;
            default:
                fprintf(stderr, "Error: Unknown option\n");
                return -1;
        }
    }

    // Basic argument validation
    if (args->help) {
        print_usage(argv[0]);
        return 1; // Indicate help was requested
    }

    if (args->input_file == NULL) {
        fprintf(stderr, "Error: Input file (--if) is required.\n");
        print_usage(argv[0]);
        return -1;
    }

    if (args->block_num == -1) {
        fprintf(stderr, "Error: Block number (--block-num) is required.\n");
        print_usage(argv[0]);
        return -1;
    }

    return 0; // Indicate successful parsing
}

int do_stuff(struct viewfs_args *args);

int main(int argc, char *argv[]) {
    struct viewfs_args args;
    int parse_result = parse_args(argc, argv, &args);

    if (parse_result == 1) {
        return 0; // Help was printed
    }

    if (parse_result < 0) {
        return 1; // Error during parsing
    }

    // You can now proceed with the rest of your viewfs logic using these arguments

    int ret = do_stuff(&args);

    // Remember to free the allocated memory
    free(args.input_file);
    if (args.output_file) {
        free(args.output_file);
    }
    free(args.format);

    return ret;
}

void print_out(unsigned char *data, uint32_t count, char *format, char *output_file);

int do_stuff(struct viewfs_args *args) {

    // Check if the device path exists
    struct stat path_stat;
    char *device_path = args->input_file;

    #pragma region infofs logic for metadatas

    if (stat(device_path, &path_stat) == -1) {
        fprintf(stderr, "Error: Cannot access '%s': %s\n", device_path, strerror(errno));
        return 1;
    }

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
            hidden_data_offset = i;
        }
        if (buffer[i] == 0xAA && buffer[i + 1] == 0x55) {
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

    uint64_t superblock_offset = hidden_data->superblock_offset;

    free(buffer);

    // seek to superblock offset
    if (lseek(fd, hidden_data->superblock_offset, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Cannot seek to the beginning of '%s': %s\n", device_path, strerror(errno));
        free(buffer);
        close(fd);
        return 1;
    }

    // read superblock to buffer
    buffer = (unsigned char *)malloc(8*1024);
    struct superblock_info *superblock=(struct superblock_info *)(buffer);
    bytes_read = read(fd, buffer, 8*1024);
    if (bytes_read == -1) {
        fprintf(stderr, "Error: Cannot read from '%s': %s\n", device_path, strerror(errno));
        free(buffer);
        close(fd);
        return 1;
    }

    int64_t inode_table_size = superblock->total_inodes * FILE_OBJECT_ALIGN_SIZE;
    uint32_t inode_table_clusters = 0;
    uint32_t mod=inode_table_size % superblock->block_size;
    if(mod != 0)
    {
        inode_table_clusters = inode_table_size / superblock->block_size + 1;
    }
    else{
        inode_table_clusters = inode_table_size / superblock->block_size;
    }
  
    // seek to inode table offset
    if (lseek(fd, superblock_offset + superblock->block_size, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Cannot seek to the beginning of '%s': %s\n", device_path, strerror(errno));
        free(buffer);
        close(fd);
        return 1;
    }

    // read inode table to buffer
    buffer = (unsigned char *)malloc(inode_table_size);
    bytes_read = read(fd, buffer, inode_table_size);
    if (bytes_read == -1) {
        fprintf(stderr, "Error: Cannot read from '%s': %s\n", device_path, strerror(errno));
        free(buffer);
        close(fd);
        return 1;
    }
    free(buffer);

    #pragma endregion

    #pragma region viewfs logic for data block reading

    if(args->count < 0)
    {
        args->count = superblock->block_size;
    }

    uint32_t offset = superblock->data_blocks_offset;

    // seek to data block offset
    if (lseek(fd, offset, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Cannot seek to data block offset %u: %s\n", offset, strerror(errno));
        close(fd);
        return 1;
    }

    // seek to specific data block
    offset += args->block_num * superblock->block_size;
    if (lseek(fd, offset, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Cannot seek to block number %u with offset %u: %s\n", args->block_num, offset, strerror(errno));
        close(fd);
        return 1;
    }

    // skip some bytes
    offset += args->skip;
    if (lseek(fd, offset, SEEK_SET) == -1) {
        fprintf(stderr, "Error: Cannot seek to skip %u bytes to offset %u: %s\n", args->skip, offset, strerror(errno));
        close(fd);
        return 1;
    }

    unsigned char *data = (unsigned char *)malloc(args->count);
    if (data == NULL) {
        fprintf(stderr, "Error: Cannot allocate memory for data\n");
        close(fd);
        return 1;
    }
    bytes_read = read(fd, data, args->count);
    if (bytes_read == -1)
    {
        fprintf(stderr, "Error: Cannot read from '%s' at offset %u with size %u: %s\n", device_path, offset, args->count, strerror(errno));
        free(data);
        close(fd);
        return 1;
    }

    print_out(data, args->count, args->format, args->output_file);
  
    #pragma endregion

    // close the device or image    
    close(fd);

    return 0;
}

void print_out(unsigned char *data, uint32_t count, char *format, char *output_file) {
    FILE *output = stdout;

    if (output_file != NULL) {
        output = fopen(output_file, "wb");
        if (output == NULL)
        {
            fprintf(stderr, "Error: Cannot open output file '%s': %s\n", output_file, strerror(errno));
            return;
        }
    }

    if (strcmp(format, "hex") == 0)
    {
        for (uint32_t i = 0; i < count; i++) {
            fprintf(output, "%02X ", data[i]);
            if ((i + 1) % 16 == 0) {
                fprintf(output, "\n");
            }
        }
        
        fputc('\n', output);
    }
    else if (strcmp(format, "text") == 0) 
    {
        for (uint32_t i = 0; i < count; i++) {
            fputc(data[i], output);
        }
        fputc('\n', output);

    }
    else
    {
        fprintf(stderr, "Error: Unknown format '%s'\n", format);
    }

    if (output_file != NULL) {
        fclose(output);
    }
}