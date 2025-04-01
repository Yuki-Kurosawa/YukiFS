// SPDX-License-Identifier: MIT

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>

#include "../../include/internal.h"
#include "../../include/version.h"
#include "../../include/file_table.h"

// define a tiny superblock info
#define FS_BLOCK_SIZE 1 // dd bs value
#define FS_BLOCK_COUNT 1 // dd count value
#define FS_BLOCK_FREE 0
#define FS_BLOCK_AVAILABLE 0
#define FS_TOTAL_INODES 1
#define FS_FREE_INODES 0
// end define superblock info

static char* convert_arch_to_string(int arch)
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

#pragma region  File Operations

static int yukifs_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t yukifs_read(struct file *filp, char __user *buf, size_t len, loff_t *offset)
{
    const unsigned char *data = version;
    size_t datalen = strlen(data);

    if (*offset >= datalen)
        return 0;

    if (len > datalen - *offset)
        len = datalen - *offset;

    if (copy_to_user(buf, data + *offset, len))
        return -EFAULT;

    *offset += len;
    return len;
}

static struct file_operations yukifs_file_ops = {
    .owner = THIS_MODULE,
    .open = yukifs_open,
    .read = yukifs_read,
};

#pragma endregion

static struct inode *yukifs_make_inode(struct super_block *sb, int mode, unsigned char* data)
{
    struct inode *inode = new_inode(sb);
    if (inode) {
        inode->i_mode = mode;
        inode->i_uid.val = 0;
        inode->i_gid.val = 0;
        
        if(!data)
            inode->i_size = 0;
        else
            inode->i_size = strlen(data);
        inode->i_blocks = inode->i_size / FS_BLOCK_SIZE;
        inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
        inode->i_ino = get_next_ino();
        inode->i_fop = &yukifs_file_ops; // Add this line to set file operations
    }
    return inode;
}

static void yukifs_put_super(struct super_block *sb)
{
    printk(KERN_DEBUG "YukiFS super block destroyed\n");
}

static int yukifs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    buf->f_type = FILESYSTEM_MAGIC_NUMBER;
    buf->f_bsize = FS_BLOCK_SIZE;
    buf->f_blocks = FS_BLOCK_COUNT; // Total blocks
    buf->f_bfree = FS_BLOCK_FREE;   // Free blocks
    buf->f_bavail = FS_BLOCK_AVAILABLE;  // Available blocks
    buf->f_files = FS_TOTAL_INODES;   // Total inodes
    buf->f_ffree = FS_FREE_INODES;    // Free inodes
    buf->f_namelen = FS_MAX_LEN; // Maximum filename length
    return 0;
}

static struct super_operations const yukifs_super_ops = {
    .put_super = yukifs_put_super,
    .statfs = yukifs_statfs,
    .drop_inode = generic_delete_inode,
};

static int yukifs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct inode *root;
    struct dentry *root_dentry;

    #pragma region Read Headers from devices

    // Read first 16KiB from devices for hidden header
    unsigned char *hidden_header_buffer;
    unsigned long header_size = 16 * 1024;

    hidden_header_buffer = kmalloc(header_size, GFP_KERNEL);
    if (!hidden_header_buffer) {
        printk(KERN_ERR "YukiFS: Failed to allocate memory for hidden header\n");
        return -ENOMEM;
    }

    unsigned long bytes_read = 0;
    unsigned long offset = 0;
    unsigned long block_nr = 0;
    struct buffer_head *bh;
    sb_set_blocksize(sb, 1024);
    int block_size = sb_min_blocksize(sb, 1024);
    
    for (block_nr = 0; bytes_read < header_size; block_nr++) {
        bh = sb_bread(sb, block_nr);
        if (!bh) {
            printk(KERN_ERR "YukiFS: Error reading block %lu\n", block_nr);
            
            return -EIO;
        }        

        unsigned long read_size = block_size;
        if (bytes_read + read_size > header_size) {
            read_size = header_size - bytes_read;
        }

        memcpy(hidden_header_buffer + offset, bh->b_data, read_size);
        bytes_read += read_size;
        offset += read_size;
        brelse(bh);
    }

    printk(KERN_DEBUG "YukiFS: Read %lu bytes of hidden header from device\n", bytes_read);
  

    #pragma endregion

    #pragma region find and Pop the Hidden Header
    int64_t hidden_data_start = -1;
    int64_t hidden_data_end = -1;

    for (off_t i = 0; i < bytes_read - 1; ++i) {
        if (hidden_header_buffer[i] == 0x55 && hidden_header_buffer[i + 1] == 0xAA) {
            printk(KERN_DEBUG "YukiFS: Found sequence 0x55AA at offset %ld\n", (long)i);
            hidden_data_start = i;
        }
        if (hidden_header_buffer[i] == 0xAA && hidden_header_buffer[i + 1] == 0x55) {
            printk(KERN_DEBUG "YukiFS: Found sequence 0xAA55 at offset %ld\n", (long)i);
            hidden_data_end = i;
        }
    }

    if (hidden_data_start == -1 || hidden_data_end == -1) {
        printk(KERN_ERR "YukiFS: Failed to find both hidden data markers (0x55AA and 0xAA55).\n");
        // Handle the case where the markers are not found.
        // You might want to return an error or take other appropriate action.
        return -EINVAL;
    } else {
        printk(KERN_DEBUG "YukiFS: Hidden data markers found. Start at %lld, End at %lld.\n",
               hidden_data_start, hidden_data_end);
        // Now you have the start and end offsets of your hidden data within the
        // hidden_header_buffer. You can now proceed to extract and process
        // the data located between these offsets (or starting at the start offset
        // with a certain size, depending on your hidden data format).

        // Add further code here to extract and use the hidden data.
    }
    #pragma endregion

    #pragma region go for hidden_data

    struct hidden_data_struct *hidden_data = (struct hidden_data_struct *)(hidden_header_buffer + hidden_data_start);

    printk(KERN_DEBUG "YukiFS: FS version: %d.%d.%d\n", hidden_data->fs_version[0], hidden_data->fs_version[1], hidden_data->fs_version[2]);
    printk(KERN_DEBUG "YukiFS: FS build tool name: %s\n", hidden_data->fs_build_tool_name);
    printk(KERN_DEBUG "YukiFS: FS build tool version: %d.%d.%d\n", hidden_data->fs_build_tool_version[0], hidden_data->fs_build_tool_version[1], hidden_data->fs_build_tool_version[2]);
    printk(KERN_DEBUG "YukiFS: Built-in ELF offset: %d\n", hidden_data->built_in_ELF_offset);
    printk(KERN_DEBUG "YukiFS: Built-in ELF size: %d\n", hidden_data->built_in_ELF_size);
    printk(KERN_DEBUG "YukiFS: Built-in ELF storage size: %d\n", hidden_data->built_in_ELF_storage_size);
    printk(KERN_DEBUG "YukiFS: Hidden data offset: %d\n", hidden_data->hidden_data_offset);
    printk(KERN_DEBUG "YukiFS: Hidden data header size: %d\n", hidden_data->hidden_data_header_size);
    printk(KERN_DEBUG "YukiFS: Hidden data header storage size: %d\n", hidden_data->hidden_data_header_storage_size);
    printk(KERN_DEBUG "YukiFS: Hidden data size: %d\n", hidden_data->hidden_data_size);
    printk(KERN_DEBUG "YukiFS: Hidden data storage size: %d\n", hidden_data->hidden_data_storage_size);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module version: %s\n", hidden_data->built_in_kernel_module_version);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module offset: %d\n", hidden_data->built_in_kernel_module_offset);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module size: %d\n", hidden_data->built_in_kernel_module_size);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module storage size: %d\n", hidden_data->built_in_kernel_module_storage_size);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module version: %s\n", hidden_data->built_in_kernel_module_version);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module offset: %d\n", hidden_data->built_in_kernel_module_offset);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module size: %d\n", hidden_data->built_in_kernel_module_size);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module storage size: %d\n", hidden_data->built_in_kernel_module_storage_size);
    printk(KERN_DEBUG "YukiFS: Built-in kernel module architecture: %d (%s)\n", hidden_data->built_in_kernel_architechture,
        convert_arch_to_string(hidden_data->built_in_kernel_architechture));
    printk(KERN_DEBUG "YukiFS: Superblock Offset: %llu \n", hidden_data->superblock_offset);

    #pragma endregion

    sb->s_magic = FILESYSTEM_MAGIC_NUMBER;
    sb->s_op = &yukifs_super_ops;

    #pragma region  Initialize the root inode

    root = yukifs_make_inode(sb, S_IFDIR | 0755, NULL);
    if (!root) {
        printk(KERN_ERR "YukiFS: inode allocation failed\n");
        return -ENOMEM;
    }
    root->i_op = &simple_dir_inode_operations;
    root->i_fop = &simple_dir_operations;

    root_dentry = d_make_root(root);
    if (!root_dentry) {
        iput(root);
        printk(KERN_ERR "YukiFS: root creation failed\n");
        return -ENOMEM;
    }
    sb->s_root = root_dentry;

    #pragma endregion

    #pragma region  Initialize the root directory

    //Add a folder to the root directory
    struct dentry *folder_dentry = d_alloc_name(root_dentry, "test_folder");
    if (!folder_dentry) {
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: folder creation failed\n");
        return -ENOMEM;
    }
    folder_dentry->d_inode = yukifs_make_inode(sb, S_IFDIR | 0755 , NULL);
    
    if (!folder_dentry->d_inode) {
        dput(folder_dentry);
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: folder inode allocation failed\n");
        return -ENOMEM;
    }
    folder_dentry->d_inode->i_op = &simple_dir_inode_operations;
    folder_dentry->d_inode->i_fop = &simple_dir_operations;
    d_add(folder_dentry, folder_dentry->d_inode);

    //Add a file to the root directory
    struct dentry *file_dentry = d_alloc_name(root_dentry, "version.txt");
    if (!file_dentry) {
        dput(folder_dentry);
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: file creation failed\n");
        return -ENOMEM;
    }
    file_dentry->d_inode = yukifs_make_inode(sb, S_IFREG | 0644, version);
    if (!file_dentry->d_inode) {
        dput(file_dentry);
        dput(folder_dentry);
        dput(root_dentry);
        iput(root);
        printk(KERN_ERR "YukiFS: file inode allocation failed\n");
        return -ENOMEM;
    }
    file_dentry->d_inode->i_op = &simple_dir_inode_operations;
    file_dentry->d_inode->i_fop = &yukifs_file_ops;
    d_add(file_dentry, file_dentry->d_inode);


    #pragma endregion

    return 0;
}

static struct dentry *yukifs_mount(struct file_system_type *fs_type,
    int flags, const char *dev_name, void *data)
{
    return mount_bdev(fs_type, flags, dev_name, data, yukifs_fill_super);
}

#pragma region  Module Initialization

static struct file_system_type yukifs_type = {
    .owner = THIS_MODULE,
    .name = FILESYSTEM_DISPLAYNAME,
    .mount = yukifs_mount,
    .kill_sb = kill_litter_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init yukifs_init(void)
{
    printk(KERN_DEBUG "YukiFS module loaded\n");
    return register_filesystem(&yukifs_type);
}

static void __exit yukifs_exit(void)
{
    unregister_filesystem(&yukifs_type);
    printk(KERN_DEBUG "YukiFS module unloaded\n");
}

#pragma endregion

module_init(yukifs_init);
module_exit(yukifs_exit);

MODULE_LICENSE(YUKIFS_LICENSE);
MODULE_AUTHOR(YUKIFS_AUTHOR);
MODULE_DESCRIPTION(YUKIFS_DESCRIPTION);
MODULE_VERSION(YUKIFS_VERSION_STRING);