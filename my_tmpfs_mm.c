#include "my_tmpfs.h"
#include <linux/writeback.h>

int my_tmpfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = page->mapping->host;
    struct my_tmpfs_file *mf = inode->i_private;
    struct page *backend_page;
    void *kaddr;
    void *backend_addr;

    if (!mf)
        return -EINVAL;

    backend_page = my_tmpfs_alloc_page(mf, page->index);
    if (!backend_page)
        goto out_redirty;

    kaddr = kmap(page);
    if (!kaddr)
        goto out_redirty;

    backend_addr = kmap(backend_page);
    if (!backend_addr) {
        kunmap(page);
        goto out_redirty;
    }

    memcpy(backend_addr, kaddr, PAGE_SIZE);
    kunmap(backend_page);
    kunmap(page);

    clear_page_dirty_for_io(page);
    SetPageUptodate(page);
    unlock_page(page);
    return 0;

out_redirty:
    redirty_page_for_writepage(wbc, page);
    unlock_page(page);
    return -ENOMEM;
}

int my_tmpfs_readpage(struct file *file, struct page *page)
{
    struct inode *inode = page->mapping->host;
    struct my_tmpfs_file *mf = inode->i_private;
    struct page *backend_page;
    void *kaddr;
    void *backend_addr;

    if (!mf)
        return -EINVAL;

    if (page->index < mf->max_pages)
        backend_page = mf->pages[page->index];
    else
        backend_page = NULL;

    if (!backend_page) {
        zero_user(page, 0, PAGE_SIZE);
    } else {
        kaddr = kmap(page);
        if (!kaddr)
            return -ENOMEM;

        backend_addr = kmap(backend_page);
        if (!backend_addr) {
            kunmap(page);
            return -ENOMEM;
        }

        memcpy(kaddr, backend_addr, PAGE_SIZE);
        kunmap(backend_page);
        kunmap(page);
    }

    SetPageUptodate(page);
    unlock_page(page);
    return 0;
}
