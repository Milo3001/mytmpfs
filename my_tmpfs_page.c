#include "my_tmpfs.h"

/* 按需扩展页指针数组，并在需要时分配零页。 */
struct page *my_tmpfs_alloc_page(struct my_tmpfs_file *mf, int index)
{
    struct page *page;
    struct page **new_pages;
    int new_max;

    if (!mf)
        return NULL;

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
    int i;

    if (!mf)
        return;

    if (mf->pages) {
        for (i = 0; i < mf->max_pages; i++) {
            if (mf->pages[i])
                __free_page(mf->pages[i]);
        }
        kfree(mf->pages);
    }

    kfree(mf);
}
