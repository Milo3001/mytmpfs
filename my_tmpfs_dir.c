#include "my_tmpfs.h"

/* 创建普通文件，检查 inode 上限，并把它挂到 dentry 上。 */
static int my_tmpfs_create(struct user_namespace *mnt_userns,
                           struct inode *dir, struct dentry *dentry,
                           umode_t mode, bool excl)
{
    struct super_block *sb = dir->i_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;
    struct inode *inode;

    (void)mnt_userns;
    (void)excl;

    if (sbi->current_inodes >= sbi->max_inodes)
        return -ENOSPC;

    inode = my_tmpfs_get_inode(sb, S_IFREG | mode);
    if (!inode)
        return -ENOMEM;

    sbi->current_inodes++;
    inode->i_size = 0;
    d_add(dentry, inode);
    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

/* 创建目录 inode，并链接到父目录。 */
static int my_tmpfs_mkdir(struct user_namespace *mnt_userns,
                          struct inode *dir, struct dentry *dentry,
                          umode_t mode)
{
    struct super_block *sb = dir->i_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;
    struct inode *inode;

    (void)mnt_userns;

    if (sbi->current_inodes >= sbi->max_inodes)
        return -ENOSPC;

    inode = my_tmpfs_get_inode(sb, S_IFDIR | mode);
    if (!inode)
        return -ENOMEM;

    sbi->current_inodes++;
    inode->i_size = 0;
    d_add(dentry, inode);
    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

/* 删除普通文件时，减少 inode 计数并清理链接数。 */
static int my_tmpfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;
    struct inode *inode;

    if (!dentry)
        return -EINVAL;

    inode = d_inode(dentry);
    if (!inode)
        return -ENOENT;

    if (S_ISDIR(inode->i_mode))
        return -EISDIR;

    drop_nlink(inode);

    if (sbi && sbi->current_inodes > 0)
        sbi->current_inodes--;

    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

/* 删除空目录时，要求目录为空并同步更新 inode 计数。 */
static int my_tmpfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;
    struct inode *inode;

    if (!dentry)
        return -EINVAL;

    inode = d_inode(dentry);
    if (!inode)
        return -ENOENT;

    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;

    if (!simple_empty(dentry))
        return -ENOTEMPTY;

    drop_nlink(inode);

    if (sbi && sbi->current_inodes > 0)
        sbi->current_inodes--;

    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

/* 目前还没有持久化目录索引，所以 lookup 直接返回空。 */
static struct dentry *my_tmpfs_lookup(struct inode *dir,
                                      struct dentry *dentry,
                                      unsigned int flags)
{
    (void)dir;
    (void)dentry;
    (void)flags;

    return NULL;
}

/* 目录操作覆盖创建、删除普通文件和删除空目录。 */
const struct inode_operations my_tmpfs_dir_inode_ops = {
    .lookup = my_tmpfs_lookup,
    .create = my_tmpfs_create,
    .mkdir = my_tmpfs_mkdir,
    .unlink = my_tmpfs_unlink,
    .rmdir = my_tmpfs_rmdir,
};

/* 普通文件使用自定义的 file_operations 表。 */
const struct inode_operations my_tmpfs_file_inode_ops = {
};
