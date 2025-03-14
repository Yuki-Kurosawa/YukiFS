#include <stdint.h>

#include "version.h"
#include "file_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE_FILENAME "my_filesystem" // Default storage file name

int mkfs(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (fp == NULL) {
        perror("Error opening storage file for writing");
        return 1;
    }

    printf("Creating file system on '%s'...\n", filename);

    // Write the superblock
    size_t superblock_written = fwrite(&fs_superblock, sizeof(struct superblock_info), 1, fp);
    if (superblock_written != 1) {
        perror("Error writing superblock");
        fclose(fp);
        return 1;
    }
    printf("Superblock written successfully.\n");
    printf("  Block Size: %u\n", fs_superblock.block_size);
    printf("  Block Count: %u\n", fs_superblock.block_count);
    printf("  Total Inodes: %u\n", fs_superblock.total_inodes);

    // Initialize data blocks (in this case, only 1 block)
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
        printf("Initialized %u data block(s).\n", fs_superblock.block_count);
    }

    // In a more complex system, you would initialize the inode table here.
    // For this very basic system with only 1 inode, we might not need to
    // explicitly write anything if the superblock indicates the total number.
    // However, for clarity, we could write a dummy inode structure.
    if (fs_superblock.total_inodes > 0) {
        struct file_object initial_inode;
        memset(&initial_inode, 0, sizeof(struct file_object));
        // You might set some default values for the initial inode if needed
        size_t inode_written = fwrite(&initial_inode, sizeof(struct file_object), fs_superblock.total_inodes, fp);
        if (inode_written != fs_superblock.total_inodes) {
            perror("Error writing initial inode(s)");
            fclose(fp);
            return 1;
        }
        printf("Initialized %u inode(s).\n", fs_superblock.total_inodes);
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