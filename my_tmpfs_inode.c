#include "my_tmpfs.h"

/* 为普通文件或目录分配并绑定一个新的 inode。 */
struct inode *my_tmpfs_get_inode(struct super_block *sb, umode_t mode)
{
    struct inode *inode;

    inode = new_inode(sb);
    if (!inode)
        return NULL;

    inode->i_ino = get_next_ino();
    inode->i_mode = mode;
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
    inode->i_uid = current_fsuid();
    inode->i_gid = current_fsgid();

    if (S_ISREG(mode)) {
        inode->i_op = &my_tmpfs_file_inode_ops;
        inode->i_fop = &my_tmpfs_file_ops;
        inode->i_mapping->a_ops = &empty_aops;
    } else if (S_ISDIR(mode)) {
        inode->i_op = &my_tmpfs_dir_inode_ops;
        inode->i_fop = &simple_dir_operations;
        mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
    }

    return inode;
}

/* 释放 inode->i_private 中保存的文件私有数据。 */
void my_tmpfs_free_inode(struct inode *inode)
{
    struct my_tmpfs_file *mf;

    if (!inode)
        return;

    mf = inode->i_private;
    if (!mf)
        return;

    my_tmpfs_free_file(mf);
    inode->i_private = NULL;
}
