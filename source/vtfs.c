
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/slab.h>

#define VTFS_MAGIC 0x76544653
#define VTFS_ROOT_INO 1000

struct vtfs_entry {
    ino_t ino;
    char name[256];
    umode_t mode;
    struct list_head list;
};

static LIST_HEAD(vtfs_entries);
static DEFINE_MUTEX(vtfs_mutex);
static unsigned long vtfs_next_ino = VTFS_ROOT_INO + 1;

static struct inode *vtfs_get_inode(
    struct super_block *sb,
    const struct inode *dir,
    umode_t mode,
    unsigned long ino)
{
    struct inode *inode = new_inode(sb);

    if (!inode)
        return NULL;

    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_ino = ino;

    return inode;
}

static struct dentry *vtfs_lookup(
    struct inode *parent_inode,
    struct dentry *child_dentry,
    unsigned int flags);

static int vtfs_create(
    struct mnt_idmap *idmap,
    struct inode *parent_inode,
    struct dentry *child_dentry,
    umode_t mode,
    bool excl)
{
    struct vtfs_entry *entry;
    struct vtfs_entry *new_entry;
    struct inode *inode;
    const char *name = child_dentry->d_name.name;

    if (parent_inode->i_ino != VTFS_ROOT_INO)
        return -ENOENT;

    if (strnlen(name, sizeof(new_entry->name)) >= sizeof(new_entry->name))
        return -ENAMETOOLONG;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (!strcmp(entry->name, name)) {
            mutex_unlock(&vtfs_mutex);
            return -EEXIST;
        }
    }

    new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);

    if (!new_entry) {
        mutex_unlock(&vtfs_mutex);
        return -ENOMEM;
    }

    inode = vtfs_get_inode(
        parent_inode->i_sb,
        parent_inode,
        S_IFREG | (mode & 0777),
        vtfs_next_ino
    );

    if (!inode) {
        kfree(new_entry);
        mutex_unlock(&vtfs_mutex);
        return -ENOMEM;
    }

    new_entry->ino = vtfs_next_ino;
    new_entry->mode = S_IFREG | (mode & 0777);
    strscpy(new_entry->name, name, sizeof(new_entry->name));

    list_add_tail(&new_entry->list, &vtfs_entries);

    vtfs_next_ino++;

    d_add(child_dentry, inode);

    mutex_unlock(&vtfs_mutex);

    return 0;
}


static int vtfs_unlink(
    struct inode *parent_inode,
    struct dentry *child_dentry)
{
    struct vtfs_entry *entry;
    struct vtfs_entry *tmp;
    const char *name = child_dentry->d_name.name;
    bool found = false;

    if (parent_inode->i_ino != VTFS_ROOT_INO)
        return -ENOENT;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry_safe(entry, tmp, &vtfs_entries, list) {
        if (!strcmp(entry->name, name)) {
            list_del(&entry->list);
            kfree(entry);
            found = true;
            break;
        }
    }

    mutex_unlock(&vtfs_mutex);

    if (!found)
        return -ENOENT;

    return simple_unlink(parent_inode, child_dentry);
}


static struct dentry *vtfs_lookup(
    struct inode *parent_inode,
    struct dentry *child_dentry,
    unsigned int flags)
{
    struct vtfs_entry *entry;
    struct inode *inode = NULL;
    const char *name = child_dentry->d_name.name;
    bool found = false;

    if (parent_inode->i_ino == VTFS_ROOT_INO) {
        mutex_lock(&vtfs_mutex);

        list_for_each_entry(entry, &vtfs_entries, list) {
            if (!strcmp(entry->name, name)) {
                inode = vtfs_get_inode(
                    parent_inode->i_sb,
                    parent_inode,
                    entry->mode,
                    entry->ino
                );

                found = true;
                break;
            }
        }

        mutex_unlock(&vtfs_mutex);
    }

    if (found && !inode)
        return ERR_PTR(-ENOMEM);

    d_add(child_dentry, inode);

    return NULL;
}

static int vtfs_iterate(
    struct file *file,
    struct dir_context *ctx)
{
    struct vtfs_entry *entry;
    int index = 0;

    if (!dir_emit_dots(file, ctx))
        return 0;

    if (file_inode(file)->i_ino != VTFS_ROOT_INO)
        return 0;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (index < ctx->pos - 2) {
            index++;
            continue;
        }

        if (!dir_emit(
            ctx,
            entry->name,
            strlen(entry->name),
            entry->ino,
            DT_REG
        ))
            break;

        ctx->pos++;
        index++;
    }

    mutex_unlock(&vtfs_mutex);

    return 0;
}

static const struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
    .create = vtfs_create,
    .unlink = vtfs_unlink,
};

static const struct file_operations vtfs_dir_ops = {
    .iterate_shared = vtfs_iterate,
};

static int vtfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct inode *inode;

    sb->s_magic = VTFS_MAGIC;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_blocksize = PAGE_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;

    inode = vtfs_get_inode(
        sb,
        NULL,
        S_IFDIR | 0777,
        VTFS_ROOT_INO
    );

    if (!inode)
        return -ENOMEM;

    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = &vtfs_dir_ops;

    sb->s_root = d_make_root(inode);

    if (!sb->s_root)
        return -ENOMEM;

    pr_info("vtfs: filesystem mounted\n");

    return 0;
}

static int vtfs_get_tree(struct fs_context *fc)
{
    return get_tree_nodev(fc, vtfs_fill_super);
}

static const struct fs_context_operations vtfs_context_operations = {
    .get_tree = vtfs_get_tree,
};

static int vtfs_init_fs_context(struct fs_context *fc)
{
    fc->ops = &vtfs_context_operations;
    return 0;
}

static void vtfs_kill_sb(struct super_block *sb)
{
    struct vtfs_entry *entry;
    struct vtfs_entry *tmp;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry_safe(entry, tmp, &vtfs_entries, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    vtfs_next_ino = VTFS_ROOT_INO + 1;

    mutex_unlock(&vtfs_mutex);

    pr_info("vtfs: filesystem unmounted\n");
    kill_anon_super(sb);
}

static struct file_system_type vtfs_type = {
    .owner = THIS_MODULE,
    .name = "vtfs",
    .init_fs_context = vtfs_init_fs_context,
    .kill_sb = vtfs_kill_sb,
};

static int __init vtfs_init(void)
{
    int error = register_filesystem(&vtfs_type);

    if (error)
        return error;

    pr_info("vtfs: module loaded\n");

    return 0;
}

static void __exit vtfs_exit(void)
{
    unregister_filesystem(&vtfs_type);
    pr_info("vtfs: module unloaded\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dimqi");
MODULE_DESCRIPTION("Virtual Test File System");
MODULE_VERSION("0.1");
