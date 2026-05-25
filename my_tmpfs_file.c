#include "my_tmpfs.h"
#include <linux/swap.h>        // get_swap_page, swap_writepage, swap_readpage, put_swap_page
#include <linux/swapops.h>     // add_to_swap_cache, delete_from_swap_cache
#include <linux/writeback.h>   // writeback_control


// 一个用于获取页面内容的辅助函数
struct page *my_tmpfs_get_page(struct inode *inode, pgoff_t index)
{
    struct my_tmpfs_file *mf = inode->i_private;
    struct page *page;
    swp_entry_t swap = {0};
    void *entry;
    int error;

    /* 1. 先查 page cache */
    page = find_get_page(inode->i_mapping, index);
    if (page)
        return page;

    /* 2. 检查是否已被换出 */
    entry = xa_load(&mf->swap_entries, index);
    if (entry)
        swap.val = xa_to_value(entry);

    if (swap.val) {
        /* 分配一个新页并换入 */
        page = alloc_page(GFP_NOFS | __GFP_HIGHMEM);
        if (!page)
            return ERR_PTR(-ENOMEM);

        /* 加入 swap cache */
        if (add_to_swap_cache(page, swap, GFP_NOFS, NULL)) {
            put_page(page);
            return ERR_PTR(-ENOMEM);
        }

        /* 从 swap 读入 (同步读, 完成后页面被解锁) */
        error = swap_readpage(page, false);
        if (error) {
            delete_from_swap_cache(page);
            put_page(page);
            return ERR_PTR(error);
        }

        /* 从 swap cache 移除，准备加入 page cache */
        delete_from_swap_cache(page);

        /* 将 page 添加到 page cache */
        error = add_to_page_cache_lru(page, inode->i_mapping, index, GFP_NOFS);
        if (error) {
            put_page(page);
            return ERR_PTR(error);
        }

        /* 释放 swap 槽位，清除映射 */
        put_swap_page(page, swap);
        xa_erase(&mf->swap_entries, index);

        /* 页面已经是 uptodate 且 unlocked (swap_readpage 已解锁) */
        SetPageUptodate(page);
        return page;
    }

    /* 3. 空洞返回 ZERO_PAGE */
    return ZERO_PAGE(0);
}

/* 从内存页数组读取，遇到稀疏空洞时返回 0。 */
static ssize_t my_tmpfs_read(struct file *filp, char __user *buf,
                             size_t len, loff_t *off)
{
    struct inode *inode = file_inode(filp);
    struct my_tmpfs_file *mf = inode->i_private;
    size_t pos = *off;
    size_t avail;
    size_t ret = 0;

    if (!mf)
        return -EINVAL;

    if (pos >= mf->size)
        return 0;

    avail = mf->size - pos;
    if (len > avail)
        len = avail;

    while (len > 0) {
        pgoff_t idx = pos >> PAGE_SHIFT;
        size_t offset = pos & ~PAGE_MASK;
        size_t copy_len = min(len, PAGE_SIZE - offset);
        struct page *page;
        char *kaddr;

        page = my_tmpfs_get_page(inode, idx);
        if (IS_ERR(page))
            return PTR_ERR(page);

        if (page == ZERO_PAGE(0)) {
            if (clear_user(buf, copy_len)) {
                if (ret == 0)
                    ret = -EFAULT;
                break;
            }
        } else {
            kaddr = kmap(page);
            if (!kaddr) {
                put_page(page);
                if (ret == 0)
                    ret = -ENOMEM;
                break;
            }
            if (copy_to_user(buf, kaddr + offset, copy_len)) {
                kunmap(page);
                put_page(page);
                if (ret == 0)
                    ret = -EFAULT;
                break;
            }
            kunmap(page);
            put_page(page);
        }

        ret += copy_len;
        pos += copy_len;
        buf += copy_len;
        len -= copy_len;
    }

    *off = pos;
    return ret;
}

/* 写入页后端存储，同时扩展文件并检查容量限制。 */
static ssize_t my_tmpfs_write(struct file *filp, const char __user *buf,
                              size_t len, loff_t *off)
{
    struct my_tmpfs_file *mf = filp->private_data;
    struct super_block *sb = file_inode(filp)->i_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;
    struct inode *inode = file_inode(filp);
    size_t pos;
    size_t ret = 0;
    struct page *page;
    char *kaddr;
    size_t page_idx;
    size_t page_off;
    size_t copy_len;

    if (!mf)
        return -EINVAL;

    MY_TMPFS_LOG("write inode=%lu len=%zu off=%lld flags=0x%x size=%lld", inode->i_ino,
                 len, (long long)*off, filp->f_flags, (long long)mf->size);

    if (filp->f_flags & O_APPEND) {
        pos = mf->size;
        *off = pos;
    } else {
        pos = *off;
    }

    if (sbi->current_size + len > sbi->max_size)
        return -ENOSPC;

    while (len > 0) {
        page_idx = pos >> PAGE_SHIFT;
        page_off = pos & (PAGE_SIZE - 1);
        copy_len = min(len, PAGE_SIZE - page_off);

        page = my_tmpfs_alloc_page(mf, page_idx);
        if (!page)
            return -ENOMEM;

        kaddr = kmap(page);
        if (!kaddr)
            return -ENOMEM;

        if (copy_from_user(kaddr + page_off, buf, copy_len)) {
            kunmap(page);
            return -EFAULT;
        }

        kunmap(page);

        ret += copy_len;
        pos += copy_len;
        buf += copy_len;
        len -= copy_len;

        if (pos > mf->size) {
            sbi->current_size += (pos - mf->size);
            mf->size = pos;
        }
    }

    *off = pos;
    inode->i_size = mf->size;
    inode->i_mtime = inode->i_ctime = current_time(inode);

    return ret;
}

int my_tmpfs_truncate_inode(struct inode *inode, loff_t newsize)
{
    struct my_tmpfs_file *mf;
    struct my_tmpfs_sb_info *sbi;
    loff_t oldsize;
    size_t old_tail;
    size_t new_tail;
    size_t start_page;
    size_t i;
    char *kaddr;
    struct page *page;

    if (!inode || !S_ISREG(inode->i_mode))
        return -EINVAL;

    if (newsize < 0 || newsize > inode->i_sb->s_maxbytes)
        return -EINVAL;

    MY_TMPFS_LOG("truncate inode=%lu old=%lld new=%lld", inode->i_ino,
                 (long long)inode->i_size, (long long)newsize);

    mf = inode->i_private;
    if (!mf) {
        mf = kzalloc(sizeof(*mf), GFP_KERNEL);
        if (!mf)
            return -ENOMEM;

        mf->is_dir = false;
        inode->i_private = mf;
    }

    sbi = inode->i_sb->s_fs_info;
    oldsize = mf->size;
    if (newsize == oldsize)
        return 0;

    if (newsize < oldsize) {
        old_tail = oldsize & (PAGE_SIZE - 1);
        new_tail = newsize & (PAGE_SIZE - 1);
        start_page = (newsize + PAGE_SIZE - 1) >> PAGE_SHIFT;

        if (mf->pages && new_tail && start_page > 0 && (start_page - 1) < (size_t)mf->max_pages) {
            page = mf->pages[start_page - 1];
            if (page) {
                kaddr = kmap(page);
                if (!kaddr)
                    return -ENOMEM;
                memset(kaddr + new_tail, 0, PAGE_SIZE - new_tail);
                kunmap(page);
            }
        }

        if (mf->pages) {
            for (i = start_page; i < (size_t)mf->max_pages; i++) {
                if (!mf->pages[i])
                    continue;
                __free_page(mf->pages[i]);
                mf->pages[i] = NULL;
                if (mf->nr_pages > 0)
                    mf->nr_pages--;
            }
        }

        if (sbi && sbi->current_size >= (unsigned long)(oldsize - newsize))
            sbi->current_size -= (unsigned long)(oldsize - newsize);
        else if (sbi)
            sbi->current_size = 0;
        
        for (i = start_page; i < ((oldsize + PAGE_SIZE - 1) >> PAGE_SHIFT); i++) {
            // 清除 xarray 中的 swap entry
            void *entry = xa_erase(&mf->swap_entries, i);
            if (entry)
                put_swap_page(NULL, (swp_entry_t){.val = xa_to_value(entry)});
        }
    } else {
        old_tail = oldsize & (PAGE_SIZE - 1);
        if (mf->pages && old_tail) {
            page = mf->pages[oldsize >> PAGE_SHIFT];
            if (page) {
                kaddr = kmap(page);
                if (!kaddr)
                    return -ENOMEM;
                memset(kaddr + old_tail, 0, min_t(size_t, PAGE_SIZE - old_tail,
                                                   newsize - oldsize));
                kunmap(page);
            }
        }
    }

    mf->size = newsize;
    inode->i_size = newsize;
    inode->i_mtime = inode->i_ctime = current_time(inode);

    return 0;
}

/* 第一次打开文件时，懒分配 inode 对应的后端对象。 */
static int my_tmpfs_open(struct inode *inode, struct file *filp)
{
    struct my_tmpfs_file *mf;

    mf = inode->i_private;
    if (!mf) {
        mf = kzalloc(sizeof(struct my_tmpfs_file), GFP_KERNEL);
        if (!mf)
            return -ENOMEM;

        mf->size = inode->i_size;
        mf->pages = NULL;
        mf->nr_pages = 0;
        mf->max_pages = 0;
        mf->is_dir = false;
        xa_init(&mf->swap_entries);
        inode->i_private = mf;
    }

    MY_TMPFS_LOG("open inode=%lu size=%lld", inode->i_ino, (long long)inode->i_size);

    filp->private_data = mf;
    return 0;
}

/* 清理每次打开时的私有指针；后端存储在 inode 销毁时释放。 */
static int my_tmpfs_release(struct inode *inode, struct file *filp)
{
    (void)inode;

    filp->private_data = NULL;
    return 0;
}

/* 按内存中的逻辑文件大小实现标准 seek。 */
static loff_t my_tmpfs_llseek(struct file *filp, loff_t offset, int whence)
{
    struct my_tmpfs_file *mf = filp->private_data;
    loff_t newpos;

    if (!mf)
        return -EINVAL;

    switch (whence) {
    case SEEK_SET:
        newpos = offset;
        break;
    case SEEK_CUR:
        newpos = filp->f_pos + offset;
        break;
    case SEEK_END:
        newpos = mf->size + offset;
        break;
    default:
        return -EINVAL;
    }

    if (newpos < 0)
        return -EINVAL;

    filp->f_pos = newpos;
    return newpos;
}

/* 普通文件的读写、打开和 seek 都由这里的操作表提供。 */
const struct file_operations my_tmpfs_file_ops = {
    .read = my_tmpfs_read,
    .write = my_tmpfs_write,
    .open = my_tmpfs_open,
    .release = my_tmpfs_release,
    .llseek = my_tmpfs_llseek,
};
