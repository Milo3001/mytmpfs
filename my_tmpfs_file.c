#include "my_tmpfs.h"

/* 从内存页数组读取，遇到稀疏空洞时返回 0。 */
static ssize_t my_tmpfs_read(struct file *filp, char __user *buf,
                             size_t len, loff_t *off)
{
    struct my_tmpfs_file *mf = filp->private_data;
    size_t pos;
    size_t avail;
    size_t ret = 0;
    struct page *page;
    char *kaddr;
    size_t page_idx;
    size_t page_off;
    size_t copy_len;

    if (!mf)
        return -EINVAL;

    pos = *off;
    if (pos >= mf->size)
        return 0;

    avail = mf->size - pos;
    if (len > avail)
        len = avail;

    while (len > 0) {
        page_idx = pos >> PAGE_SHIFT;
        page_off = pos & (PAGE_SIZE - 1);
        copy_len = min(len, PAGE_SIZE - page_off);

        if (!mf->pages || page_idx >= mf->max_pages || !mf->pages[page_idx]) {
            if (clear_user(buf, copy_len))
                return ret ? ret : -EFAULT;
        } else {
            page = mf->pages[page_idx];
            kaddr = kmap(page);
            if (!kaddr)
                return ret ? ret : -ENOMEM;

            if (copy_to_user(buf, kaddr + page_off, copy_len)) {
                kunmap(page);
                return ret ? ret : -EFAULT;
            }
            kunmap(page);
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
        inode->i_private = mf;
    }

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
