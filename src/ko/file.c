#include "file.h"

#pragma region File Operations

static int yukifs_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t yukifs_read(struct file *filp, char __user *buf, size_t len, loff_t *offset)
{
    const unsigned char *data = YUKIFS_VERSION_STRING "\n";
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

struct file_operations yukifs_file_ops = {
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
        inode->i_blocks = inode->i_size / sb->s_blocksize;
        inode->__i_atime = inode->__i_mtime = inode->__i_ctime = current_time(inode);
        inode->i_ino = get_next_ino();
        inode->i_fop = &yukifs_file_ops; // Add this line to set file operations
    }
    return inode;
}

int yukifs_init_root(struct super_block *sb)
{
    struct inode *root;
    struct dentry *root_dentry;

    #pragma region Initialize the root inode

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

    #pragma region Initialize the root directory

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
    file_dentry->d_inode = yukifs_make_inode(sb, S_IFREG | 0644, YUKIFS_VERSION_STRING "\n");
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

