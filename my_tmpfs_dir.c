#include "my_tmpfs.h"

static struct my_tmpfs_file *my_tmpfs_get_dir_private(struct inode *inode)
{
    struct my_tmpfs_file *mf = inode->i_private;

    if (mf)
        return mf;

    mf = kzalloc(sizeof(*mf), GFP_KERNEL);
    if (!mf)
        return NULL;

    mf->is_dir = true;
    INIT_LIST_HEAD(&mf->children);
    xa_init(&mf->swap_entries);   // 新增
    inode->i_private = mf;
    return mf;
}

static struct my_tmpfs_file *my_tmpfs_get_file_private(struct inode *inode)
{
    struct my_tmpfs_file *mf = inode->i_private;

    if (mf)
        return mf;

    mf = kzalloc(sizeof(*mf), GFP_KERNEL);
    if (!mf)
        return NULL;

    mf->size = inode->i_size;
    xa_init(&mf->swap_entries);   // 新增
    inode->i_private = mf;
    return mf;
}

static struct my_tmpfs_dir_entry *my_tmpfs_find_entry(struct my_tmpfs_file *dir_mf,
                                                      const struct qstr *name)
{
    struct my_tmpfs_dir_entry *entry;
    size_t entry_len;

    if (!dir_mf || !dir_mf->is_dir)
        return NULL;

    list_for_each_entry(entry, &dir_mf->children, list) {
        entry_len = strlen(entry->name);
        if (entry_len == name->len && !memcmp(entry->name, name->name, name->len))
            return entry;
    }

    return NULL;
}

static int my_tmpfs_add_entry(struct inode *dir, struct dentry *dentry, struct inode *inode)
{
    struct my_tmpfs_file *dir_mf = my_tmpfs_get_dir_private(dir);
    struct my_tmpfs_dir_entry *entry;
    char *name;

    if (!dir_mf)
        return -ENOMEM;

    if (my_tmpfs_find_entry(dir_mf, &dentry->d_name))
        return -EEXIST;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;

    name = kmalloc(dentry->d_name.len + 1, GFP_KERNEL);
    if (!name) {
        kfree(entry);
        return -ENOMEM;
    }

    memcpy(name, dentry->d_name.name, dentry->d_name.len);
    name[dentry->d_name.len] = '\0';

    entry->name = name;
    entry->inode = inode;
    MY_TMPFS_LOG("add_entry dir=%lu inode=%lu name=%.*s", dir->i_ino, inode->i_ino,
                 dentry->d_name.len, dentry->d_name.name);
    list_add_tail(&entry->list, &dir_mf->children);
    return 0;
}

static struct my_tmpfs_dir_entry *my_tmpfs_remove_entry(struct inode *dir,
                                                        const struct qstr *name)
{
    struct my_tmpfs_file *dir_mf = my_tmpfs_get_dir_private(dir);
    struct my_tmpfs_dir_entry *entry;

    if (!dir_mf)
        return NULL;

    entry = my_tmpfs_find_entry(dir_mf, name);
    if (!entry)
        return NULL;

    MY_TMPFS_LOG("remove_entry dir=%lu name=%.*s inode=%lu", dir->i_ino,
                 name->len, name->name, entry->inode ? entry->inode->i_ino : 0);
    list_del(&entry->list);
    return entry;
}

static bool my_tmpfs_dir_empty(struct inode *dir)
{
    struct my_tmpfs_file *dir_mf = my_tmpfs_get_dir_private(dir);

    if (!dir_mf)
        return true;

    return list_empty(&dir_mf->children);
}

/* 创建普通文件，检查 inode 上限，并把它挂到 dentry 上。 */
static int my_tmpfs_create(struct user_namespace *mnt_userns,
                           struct inode *dir, struct dentry *dentry,
                           umode_t mode, bool excl)
{
    struct super_block *sb = dir->i_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;
    struct my_tmpfs_file *mf;
    struct inode *inode;
    int err;

    (void)mnt_userns;
    (void)excl;

    if (sbi->current_inodes >= sbi->max_inodes)
        return -ENOSPC;

    MY_TMPFS_LOG("create dir=%lu name=%.*s mode=%o", dir->i_ino, dentry->d_name.len,
                 dentry->d_name.name, mode);

    inode = my_tmpfs_get_inode(sb, S_IFREG | mode);
    if (!inode)
        return -ENOMEM;

    sbi->current_inodes++;

    mf = my_tmpfs_get_file_private(inode);
    if (!mf) {
        iput(inode);
        return -ENOMEM;
    }

    set_nlink(inode, 1);
    ihold(inode);
    inode->i_size = 0;

    err = my_tmpfs_add_entry(dir, dentry, inode);
    if (err) {
        iput(inode);
        return err;
    }

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
    struct my_tmpfs_file *mf;
    struct inode *inode;
    int err;

    (void)mnt_userns;

    if (sbi->current_inodes >= sbi->max_inodes)
        return -ENOSPC;

    MY_TMPFS_LOG("mkdir dir=%lu name=%.*s mode=%o", dir->i_ino, dentry->d_name.len,
                 dentry->d_name.name, mode);

    inode = my_tmpfs_get_inode(sb, S_IFDIR | mode);
    if (!inode)
        return -ENOMEM;

    sbi->current_inodes++;

    mf = my_tmpfs_get_dir_private(inode);
    if (!mf) {
        iput(inode);
        return -ENOMEM;
    }

    set_nlink(inode, 2);
    inode->i_size = 0;

    err = my_tmpfs_add_entry(dir, dentry, inode);
    if (err) {
        iput(inode);
        return err;
    }

    d_add(dentry, inode);
    inc_nlink(dir);
    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

/* 删除普通文件时，移除目录项并清理链接数。 */
static int my_tmpfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct my_tmpfs_dir_entry *entry;
    struct inode *inode;

    if (!dentry)
        return -EINVAL;

    inode = d_inode(dentry);
    if (!inode)
        return -ENOENT;

    if (S_ISDIR(inode->i_mode))
        return -EISDIR;

    MY_TMPFS_LOG("unlink dir=%lu name=%.*s inode=%lu", dir->i_ino, dentry->d_name.len,
                 dentry->d_name.name, inode->i_ino);

    entry = my_tmpfs_remove_entry(dir, &dentry->d_name);
    if (!entry)
        return -ENOENT;

    kfree(entry->name);
    kfree(entry);

    drop_nlink(inode);

    if (inode->i_nlink > 0) {
        iput(inode);
        d_drop(dentry);
    }

    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

/* 删除空目录时，要求目录为空并同步更新链接计数。 */
static int my_tmpfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct my_tmpfs_dir_entry *entry;
    struct inode *inode;

    if (!dentry)
        return -EINVAL;

    inode = d_inode(dentry);
    if (!inode)
        return -ENOENT;

    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;

    if (!my_tmpfs_dir_empty(inode))
        return -ENOTEMPTY;

    MY_TMPFS_LOG("rmdir dir=%lu name=%.*s inode=%lu", dir->i_ino, dentry->d_name.len,
                 dentry->d_name.name, inode->i_ino);

    entry = my_tmpfs_remove_entry(dir, &dentry->d_name);
    if (!entry)
        return -ENOENT;

    kfree(entry->name);
    kfree(entry);

    drop_nlink(inode);
    drop_nlink(dir);

    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

/* 根据目录私有索引查找路径组件。 */
static struct dentry *my_tmpfs_lookup(struct inode *dir,
                                      struct dentry *dentry,
                                      unsigned int flags)
{
    struct my_tmpfs_file *dir_mf = my_tmpfs_get_dir_private(dir);
    struct my_tmpfs_dir_entry *entry;

    (void)flags;

    if (!dir_mf)
        return ERR_PTR(-ENOMEM);

    entry = my_tmpfs_find_entry(dir_mf, &dentry->d_name);
    if (!entry)
        return NULL;

    MY_TMPFS_LOG("lookup dir=%lu name=%.*s inode=%lu", dir->i_ino, dentry->d_name.len,
                 dentry->d_name.name, entry->inode ? entry->inode->i_ino : 0);

    return d_splice_alias(entry->inode, dentry);
}

static int my_tmpfs_link(struct dentry *old_dentry, struct inode *dir,
                         struct dentry *dentry)
{
    struct inode *inode = d_inode(old_dentry);
    int err;

    if (!inode)
        return -ENOENT;

    if (S_ISDIR(inode->i_mode))
        return -EPERM;

    MY_TMPFS_LOG("link dir=%lu name=%.*s inode=%lu", dir->i_ino, dentry->d_name.len,
                 dentry->d_name.name, inode->i_ino);

    err = my_tmpfs_add_entry(dir, dentry, inode);
    if (err)
        return err;

    inc_nlink(inode);
    ihold(inode);

    inode->i_ctime = current_time(inode);
    dir->i_ctime = dir->i_mtime = current_time(dir);

    d_add(dentry, inode);
    return 0;
}

static int my_tmpfs_symlink(struct user_namespace *mnt_userns, struct inode *dir,
                            struct dentry *dentry, const char *symname)
{
    struct super_block *sb = dir->i_sb;
    struct my_tmpfs_sb_info *sbi = sb->s_fs_info;
    struct my_tmpfs_file *mf;
    struct inode *inode;
    int err;

    (void)mnt_userns;

    if (sbi->current_inodes >= sbi->max_inodes)
        return -ENOSPC;

    MY_TMPFS_LOG("symlink dir=%lu name=%.*s target=%s", dir->i_ino, dentry->d_name.len,
                 dentry->d_name.name, symname);

    inode = my_tmpfs_get_inode(sb, S_IFLNK | 0777);
    if (!inode)
        return -ENOMEM;

    sbi->current_inodes++;

    mf = kzalloc(sizeof(*mf), GFP_KERNEL);
    if (!mf) {
        iput(inode);
        return -ENOMEM;
    }

    mf->symlink_target = kstrdup(symname, GFP_KERNEL);
    if (!mf->symlink_target) {
        kfree(mf);
        iput(inode);
        return -ENOMEM;
    }

    inode->i_private = mf;
    inode->i_link = mf->symlink_target;
    inode->i_size = strlen(symname);
    set_nlink(inode, 1);

    err = my_tmpfs_add_entry(dir, dentry, inode);
    if (err) {
        iput(inode);
        return err;
    }

    d_add(dentry, inode);
    dir->i_mtime = dir->i_ctime = current_time(dir);

    return 0;
}

static const char *my_tmpfs_get_link(struct dentry *dentry, struct inode *inode,
                                     struct delayed_call *done)
{
    (void)dentry;
    (void)done;

    return inode ? inode->i_link : NULL;
}

static int my_tmpfs_readlink(struct dentry *dentry, char __user *buf, int len)
{
    struct inode *inode = d_inode(dentry);
    const char *target;
    size_t target_len;

    if (!inode || !inode->i_link)
        return -EINVAL;

    target = inode->i_link;
    target_len = strlen(target);
    if (len > target_len)
        len = target_len;

    if (copy_to_user(buf, target, len))
        return -EFAULT;

    return len;
}

static int my_tmpfs_setattr(struct user_namespace *mnt_userns, struct dentry *dentry,
                            struct iattr *attr)
{
    struct inode *inode = d_inode(dentry);
    int err;

    (void)mnt_userns;

    if (!inode)
        return -ENOENT;

    err = setattr_prepare(mnt_userns, dentry, attr);
    if (err)
        return err;

    if (attr->ia_valid & ATTR_SIZE)
        return my_tmpfs_truncate_inode(inode, attr->ia_size);

    return 0;
}

static int my_tmpfs_rename(struct user_namespace *mnt_userns,
                           struct inode *old_dir, struct dentry *old_dentry,
                           struct inode *new_dir, struct dentry *new_dentry,
                           unsigned int flags)
{
    struct inode *old_inode;
    struct inode *new_inode;
    struct my_tmpfs_dir_entry *entry;
    struct my_tmpfs_dir_entry *target_entry;
    struct my_tmpfs_file *old_dir_mf;
    struct my_tmpfs_file *new_dir_mf;
    char *new_name;
    bool source_is_dir;
    bool target_is_dir;

    (void)mnt_userns;

    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;

    MY_TMPFS_LOG("rename old_dir=%lu old=%.*s new_dir=%lu new=%.*s flags=%u", old_dir->i_ino,
                 old_dentry->d_name.len, old_dentry->d_name.name, new_dir->i_ino,
                 new_dentry->d_name.len, new_dentry->d_name.name, flags);

    old_inode = d_inode(old_dentry);
    new_inode = d_inode(new_dentry);

    source_is_dir = S_ISDIR(old_inode->i_mode);
    target_is_dir = new_inode && S_ISDIR(new_inode->i_mode);

    if (new_inode && (flags & RENAME_NOREPLACE))
        return -EEXIST;

    if (old_dir == new_dir && old_dentry->d_name.len == new_dentry->d_name.len &&
        !memcmp(old_dentry->d_name.name, new_dentry->d_name.name,
                old_dentry->d_name.len))
        return 0;

    if (source_is_dir) {
        if (new_inode && !target_is_dir)
            return -ENOTDIR;

        if (new_inode && target_is_dir && !my_tmpfs_dir_empty(new_inode))
            return -ENOTEMPTY;
    } else if (target_is_dir) {
        return -EISDIR;
    }

    old_dir_mf = my_tmpfs_get_dir_private(old_dir);
    new_dir_mf = my_tmpfs_get_dir_private(new_dir);
    if (!old_dir_mf || !new_dir_mf)
        return -ENOMEM;

    entry = my_tmpfs_find_entry(old_dir_mf, &old_dentry->d_name);
    if (!entry)
        return -ENOENT;

    target_entry = NULL;
    if (new_inode)
        target_entry = my_tmpfs_find_entry(new_dir_mf, &new_dentry->d_name);

    if (target_entry && (flags & RENAME_NOREPLACE))
        return -EEXIST;

    new_name = kmalloc(new_dentry->d_name.len + 1, GFP_KERNEL);
    if (!new_name)
        return -ENOMEM;

    memcpy(new_name, new_dentry->d_name.name, new_dentry->d_name.len);
    new_name[new_dentry->d_name.len] = '\0';

    if (source_is_dir && old_dir != new_dir) {
        drop_nlink(old_dir);
        if (!target_is_dir)
            inc_nlink(new_dir);
    }

    if (target_entry) {
        list_del(&target_entry->list);
        drop_nlink(target_entry->inode);
        kfree(target_entry->name);
        kfree(target_entry);
    }

    list_del(&entry->list);
    kfree(entry->name);
    entry->name = new_name;

    if (old_dir == new_dir)
        list_add_tail(&entry->list, &old_dir_mf->children);
    else
        list_add_tail(&entry->list, &new_dir_mf->children);

    old_dir->i_mtime = old_dir->i_ctime = current_time(old_dir);
    new_dir->i_mtime = new_dir->i_ctime = current_time(new_dir);
    if (old_inode)
        old_inode->i_ctime = current_time(old_inode);
    MY_TMPFS_LOG("rename done old_inode=%lu new_parent=%lu", old_inode ? old_inode->i_ino : 0,
                 new_dir->i_ino);
    d_move(old_dentry, new_dentry);
    return 0;
}

/* 目录操作覆盖创建、删除普通文件、删除空目录和重命名/链接。 */
const struct inode_operations my_tmpfs_dir_inode_ops = {
    .lookup = my_tmpfs_lookup,
    .create = my_tmpfs_create,
    .mkdir = my_tmpfs_mkdir,
    .unlink = my_tmpfs_unlink,
    .rmdir = my_tmpfs_rmdir,
    .link = my_tmpfs_link,
    .symlink = my_tmpfs_symlink,
    .rename = my_tmpfs_rename,
};

const struct inode_operations my_tmpfs_symlink_inode_ops = {
    .get_link = my_tmpfs_get_link,
    .readlink = my_tmpfs_readlink,
};

/* 普通文件只需要 truncate 支持。 */
const struct inode_operations my_tmpfs_file_inode_ops = {
    .setattr = my_tmpfs_setattr,
};
