#include "my_tmpfs.h"
#include <linux/swap.h>
#include <linux/swapops.h>

/* 按需扩展页指针数组，并在需要时分配零页。 */
struct page *my_tmpfs_alloc_page(struct my_tmpfs_file *mf, int index)
{
    struct page *page;
    struct page **new_pages;
    int new_max;
    void *entry;

    if (!mf)
        return NULL;

    /* 如果该页已被换出，先释放 swap 槽位 */
    entry = xa_erase(&mf->swap_entries, index);
    if (entry)
        put_swap_page(NULL, (swp_entry_t){.val = xa_to_value(entry)});

    if (!mf->pages) {
        mf->max_pages = 16;
        mf->pages = kzalloc(sizeof(struct page *) * mf->max_pages, GFP_KERNEL);
        if (!mf->pages)
            return NULL;
        mf->nr_pages = 0;
    }

    if (index >= mf->max_pages) {
        new_max = max(mf->max_pages * 2, index + 1);
        new_pages = krealloc(mf->pages, new_max * sizeof(struct page *), GFP_KERNEL);
        if (!new_pages)
            return NULL;
        mf->pages = new_pages;
        memset(mf->pages + mf->max_pages, 0,
               (new_max - mf->max_pages) * sizeof(struct page *));
        mf->max_pages = new_max;
    }

    if (mf->pages[index])
        return mf->pages[index];

    page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!page)
        return NULL;

    mf->pages[index] = page;
    mf->nr_pages++;
    return page;
}

/* 释放所有已分配的页，然后释放文件对象本身。 */
void my_tmpfs_free_file(struct my_tmpfs_file *mf)
{
    struct my_tmpfs_dir_entry *entry,*tmp;
    unsigned long i;
    void *entry_val;

    if (!mf)
        return;

    if (!mf->is_dir && !mf->pages && !mf->symlink_target) {
        MY_TMPFS_LOG("free_file already freed size=%lld", (long long)mf->size);
        kfree(mf);
        return;
    }

    MY_TMPFS_LOG("free_file is_dir=%d size=%lld pages=%d children=%s", mf->is_dir,
                 (long long)mf->size, mf->nr_pages,
                 mf->is_dir ? "yes" : "no");

    if (mf->pages) {
        for (i = 0; i < mf->max_pages; i++) {
            if (mf->pages[i])
                __free_page(mf->pages[i]);
        }
        kfree(mf->pages);
    }

    if (mf->symlink_target)
        kfree(mf->symlink_target);

    if (mf->is_dir) {
        list_for_each_entry_safe(entry, tmp, &mf->children, list) {
            list_del(&entry->list);
            kfree(entry->name);
            kfree(entry);
        }
    }

    xa_for_each(&mf->swap_entries, i, entry_val) {
        if (entry_val)
            put_swap_page(NULL, (swp_entry_t){.val = xa_to_value(entry_val)});
    }
    xa_destroy(&mf->swap_entries);

    kfree(mf);
}
