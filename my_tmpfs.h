#ifndef _MY_TMPFS_H
#define _MY_TMPFS_H

/*
 * 模块化 tmpfs 实现的公共声明。
 * 代码拆分为 superblock、inode、目录、文件和页管理几个部分。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/fcntl.h>

#define MY_TMPFS_MAGIC            0x12345678
#define MY_TMPFS_DEFAULT_MAX_SIZE (16UL * 1024 * 1024)
#define MY_TMPFS_DEFAULT_MAX_INODES 1000

/* superblock 私有的全局容量和 inode 计数。 */
struct my_tmpfs_sb_info {
    unsigned long max_size;
    unsigned long current_size;
    unsigned long max_inodes;
    unsigned long current_inodes;
};

/* 单个文件的后端存储：稀疏页数组加逻辑文件大小。 */
struct my_tmpfs_file {
    loff_t size;
    struct page **pages;
    int nr_pages;
    int max_pages;
};

struct page *my_tmpfs_alloc_page(struct my_tmpfs_file *mf, int index);
void my_tmpfs_free_file(struct my_tmpfs_file *mf);

struct inode *my_tmpfs_get_inode(struct super_block *sb, umode_t mode);
void my_tmpfs_free_inode(struct inode *inode);

extern const struct inode_operations my_tmpfs_dir_inode_ops;
extern const struct inode_operations my_tmpfs_file_inode_ops;
extern const struct file_operations my_tmpfs_file_ops;
extern const struct super_operations my_tmpfs_sops;

int my_tmpfs_fill_super(struct super_block *sb, void *data, int silent);

#endif
