#include <stdint.h>

#include "version.h"
#include "file_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For lseek (more portable for file offsets)

#define STORAGE_FILENAME "my_filesystem" // Default storage file name
#define SUPERBLOCK_OFFSET 1024
#define MAGIC_NUMBER_OFFSET 3

int mkfs(const char *filename) {
    FILE *fp = fopen(filename, "wb+"); // Open in read/write binary mode
    if (fp == NULL) {
        perror("Error opening storage file for writing");
        return 1;
    }

    printf("Creating file system on '%s'...\n", filename);
    printf("Placing magic number at offset %d bytes.\n", MAGIC_NUMBER_OFFSET);
    printf("Placing superblock at offset %d bytes.\n", SUPERBLOCK_OFFSET);

    // Write the magic number at the specified offset
    if (fseek(fp, MAGIC_NUMBER_OFFSET, SEEK_SET) != 0) {
        perror("Error seeking to magic number offset");
        fclose(fp);
        return 1;
    }
    unsigned int magic_number = FILESYSTEM_MAGIC_NUMBER;
    size_t magic_written = fwrite(&magic_number, sizeof(unsigned int), 1, fp);
    if (magic_written != 1) {
        perror("Error writing magic number");
        fclose(fp);
        return 1;
    }
    printf("Magic number (0x%X) written successfully at offset %d.\n", magic_number, MAGIC_NUMBER_OFFSET);

    // Pad with zeros from the beginning up to the superblock offset
    if (fseek(fp, MAGIC_NUMBER_OFFSET+4, SEEK_SET) != 0) {
        perror("Error seeking to the beginning for padding");
        fclose(fp);
        return 1;
    }
    unsigned char padding = 0;
    for (int i = 0; i < SUPERBLOCK_OFFSET; i++) {
        if (i != MAGIC_NUMBER_OFFSET && i < MAGIC_NUMBER_OFFSET + sizeof(unsigned int)) {
            // Skip the magic number's location during padding
            continue;
        }
        if (fwrite(&padding, 1, 1, fp) != 1) {
            perror("Error writing padding");
            fclose(fp);
            return 1;
        }
    }
    printf("Padding up to superblock offset done.\n");

    // Seek to the superblock offset
    if (fseek(fp, SUPERBLOCK_OFFSET, SEEK_SET) != 0) {
        perror("Error seeking to superblock offset");
        fclose(fp);
        return 1;
    }

    // Write the superblock
    size_t superblock_written = fwrite(&fs_superblock, sizeof(struct superblock_info), 1, fp);
    if (superblock_written != 1) {
        perror("Error writing superblock");
        fclose(fp);
        return 1;
    }
    printf("Superblock written successfully at offset %d.\n", SUPERBLOCK_OFFSET);
    printf("  Block Size: %u\n", fs_superblock.block_size);
    printf("  Block Count: %u\n", fs_superblock.block_count);
    printf("  Total Inodes: %u\n", fs_superblock.total_inodes);

    // Initialize data blocks (in this case, only 1 block) after the superblock
    long data_start_offset = SUPERBLOCK_OFFSET + sizeof(struct superblock_info);
    if (fseek(fp, data_start_offset, SEEK_SET) != 0) {
        perror("Error seeking to data block start");
        fclose(fp);
        return 1;
    }

    if (fs_superblock.block_count > 0) {
        unsigned char initial_block_data[FS_BLOCK_SIZE];
        memset(initial_block_data, 0, FS_BLOCK_SIZE); // Initialize with zeros
        for (int i = 0; i < fs_superblock.block_count; i++) {
            size_t block_written = fwrite(initial_block_data, FS_BLOCK_SIZE, 1, fp);
            if (block_written != 1) {
                perror("Error writing data block");
                fclose(fp);
                return 1;
            }
        }
        printf("Initialized %u data block(s) starting at offset %ld.\n", fs_superblock.block_count, data_start_offset);
    }

    // Initialize inodes after the data blocks
    long inode_start_offset = data_start_offset + (fs_superblock.block_count * FS_BLOCK_SIZE);
    if (fseek(fp, inode_start_offset, SEEK_SET) != 0) {
        perror("Error seeking to inode start");
        fclose(fp);
        return 1;
    }

    if (fs_superblock.total_inodes > 0) {
        struct file_object initial_inode;
        memset(&initial_inode, 0, sizeof(struct file_object));
        size_t inode_written = fwrite(&initial_inode, sizeof(struct file_object), fs_superblock.total_inodes, fp);
        if (inode_written != fs_superblock.total_inodes) {
            perror("Error writing initial inode(s)");
            fclose(fp);
            return 1;
        }
        printf("Initialized %u inode(s) starting at offset %ld.\n", fs_superblock.total_inodes, inode_start_offset);
    }

    printf("File system creation on '%s' complete.\n", filename);

    fclose(fp);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *filename = STORAGE_FILENAME;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [storage_file]\n", argv[0]);
        return 1;
    }

    if (argc == 2) {
        filename = argv[1];
    }

    return mkfs(filename);
}