#include "my_tmpfs.h"

/* 向用户态报告文件系统容量和 inode 计数。 */
static int my_tmpfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct super_block *sb = dentry->d_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;

    buf->f_type = MY_TMPFS_MAGIC;
    buf->f_bsize = PAGE_SIZE;
    buf->f_blocks = sbi->max_size >> PAGE_SHIFT;
    buf->f_bfree = (sbi->max_size - sbi->current_size) >> PAGE_SHIFT;
    buf->f_bavail = buf->f_bfree;
    buf->f_files = sbi->max_inodes;
    buf->f_ffree = sbi->max_inodes - sbi->current_inodes;
    buf->f_namelen = NAME_MAX;

    return 0;
}

/* 这里的 superblock 操作只需要 statfs 和 inode 销毁。 */
const struct super_operations my_tmpfs_sops = {
    .statfs = my_tmpfs_statfs,
    .destroy_inode = my_tmpfs_free_inode,
};

/* 初始化 superblock、根 inode 以及文件系统计数。 */
int my_tmpfs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct inode *root_inode;
    struct my_tmpfs_file *root_priv;
    struct my_tmpfs_sb_info *sbi;

    (void)data;
    (void)silent;

    sbi = kzalloc(sizeof(struct my_tmpfs_sb_info), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sbi->max_size = MY_TMPFS_DEFAULT_MAX_SIZE;
    sbi->current_size = 0;
    sbi->max_inodes = MY_TMPFS_DEFAULT_MAX_INODES;
    sbi->current_inodes = 0;

    sb->s_fs_info = sbi;
    sb->s_magic = MY_TMPFS_MAGIC;
    sb->s_op = &my_tmpfs_sops;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_maxbytes = MAX_LFS_FILESIZE;

    /* 根目录作为始终存在的 inode 在这里创建。 */
    root_inode = my_tmpfs_get_inode(sb, S_IFDIR | 0777);
    if (!root_inode) {
        kfree(sbi);
        return -ENOMEM;
    }

    root_priv = kzalloc(sizeof(struct my_tmpfs_file), GFP_KERNEL);
    if (!root_priv) {
        iput(root_inode);
        kfree(sbi);
        return -ENOMEM;
    }

    root_priv->is_dir = true;
    INIT_LIST_HEAD(&root_priv->children);
    root_inode->i_private = root_priv;
    set_nlink(root_inode, 2);

    sbi->current_inodes++;
    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        iput(root_inode);
        kfree(sbi);
        return -ENOMEM;
    }

    return 0;
}
