#include "my_tmpfs.h"
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/writeback.h>

int my_tmpfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = page->mapping->host;
    struct my_tmpfs_file *mf = inode->i_private;
    swp_entry_t swap = {0};
    int error = -ENOMEM;

    /* 1. 为该页面分配一个 Swap 槽位 */
    swap = get_swap_page(page);   // 修正：传入 page 参数
    if (!swap.val)
        goto out_redirty;

    /* 2. 将页面内容写入到刚才分配好的 Swap 槽位 */
    if (add_to_swap_cache(page, swap, GFP_KERNEL, NULL)) {
        put_swap_page(page, swap);
        goto out_redirty;
    }

    error = swap_writepage(page, wbc);
    if (error) {
        delete_from_swap_cache(page);
        put_swap_page(page, swap);
        goto out_redirty;
    }

    /* 3. 记录映射关系 */
    if (mf) {
        pgoff_t index = page->index;
        xa_store(&mf->swap_entries, index, xa_mk_value(swap.val), GFP_KERNEL);
    }

    /* 4. 解除页面与 inode 的关联 */
    ClearPageUptodate(page);
    delete_from_page_cache(page);
    unlock_page(page);
    return 0;

out_redirty:
    redirty_page_for_writepage(wbc, page);
    unlock_page(page);
    return error;
}

int my_tmpfs_readpage(struct file *file, struct page *page)
{
    struct inode *inode = page->mapping->host;
    struct my_tmpfs_file *mf = inode->i_private;
    swp_entry_t swap;
    int error;
    void *entry;

    entry = xa_load(&mf->swap_entries, page->index);
    if (entry)
        swap.val = xa_to_value(entry);
    else
        swap.val = 0;

    if (swap.val) {
        if (add_to_swap_cache(page, swap, GFP_KERNEL, NULL))
            return -ENOMEM;
        error = swap_readpage(page, false);
        if (error) {
            delete_from_swap_cache(page);
            return error;
        }
        put_swap_page(page, swap);
        xa_erase(&mf->swap_entries, page->index);
        SetPageUptodate(page);
        unlock_page(page);
        return 0;
    }

    zero_user(page, 0, PAGE_SIZE);
    SetPageUptodate(page);
    unlock_page(page);
    return 0;
}