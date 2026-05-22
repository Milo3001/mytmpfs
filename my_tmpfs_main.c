#include "my_tmpfs.h"

/* 这个文件提供 Makefile 实际使用的模块入口。 */
static struct dentry *my_tmpfs_mount(struct file_system_type *fs_type,
                                     int flags, const char *dev_name,
                                     void *data)
{
    (void)dev_name;

    return mount_nodev(fs_type, flags, data, my_tmpfs_fill_super);
}

/* 卸载时释放 superblock 私有的统计结构。 */
static void my_tmpfs_kill_sb(struct super_block *sb)
{
    kill_litter_super(sb);
    kfree(sb->s_fs_info);
}

static struct file_system_type my_tmpfs_fs_type = {
    .owner = THIS_MODULE,
    .name = "my_tmpfs",
    .mount = my_tmpfs_mount,
    .kill_sb = my_tmpfs_kill_sb,
    .fs_flags = FS_USERNS_MOUNT,
};

static int __init my_tmpfs_init(void)
{
    return register_filesystem(&my_tmpfs_fs_type);
}

static void __exit my_tmpfs_exit(void)
{
    unregister_filesystem(&my_tmpfs_fs_type);
}

module_init(my_tmpfs_init);
module_exit(my_tmpfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A modular tmpfs-like filesystem");