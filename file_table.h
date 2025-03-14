#ifndef FILE_TABLE_H
#define FILE_TABLE_H

#define FS_MAX_LEN 8

struct file_table
{
    char name[FS_MAX_LEN]; //file name
    uint8_t size; //file size
    int descriptor; // the drwxrwxrwx thing, permissions & descriptors
    int first_block;
};

#endif