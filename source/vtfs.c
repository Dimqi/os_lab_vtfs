
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
#include <linux/uaccess.h>

#define VTFS_MAGIC 0x76544653
#define VTFS_ROOT_INO 1000
#define VTFS_NAME_LEN 256

struct vtfs_entry {
    ino_t ino;
    ino_t parent_ino;
    char name[VTFS_NAME_LEN];
    umode_t mode;
    char *data;
    size_t size;
    struct list_head list;
};

static LIST_HEAD(vtfs_entries);
static DEFINE_MUTEX(vtfs_mutex);
static unsigned long vtfs_next_ino = VTFS_ROOT_INO + 1;

static const struct file_operations vtfs_file_ops;
static const struct file_operations vtfs_dir_ops;
static const struct inode_operations vtfs_inode_ops;

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
    unsigned int flags)
{
    struct vtfs_entry *entry;
    struct inode *inode = NULL;
    const char *name = child_dentry->d_name.name;
    bool found = false;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->parent_ino == parent_inode->i_ino &&
            !strcmp(entry->name, name)) {

            inode = vtfs_get_inode(
                parent_inode->i_sb,
                parent_inode,
                entry->mode,
                entry->ino
            );

            if (inode) {
                if (S_ISDIR(entry->mode)) {
                    inode->i_op = &vtfs_inode_ops;
                    inode->i_fop = &vtfs_dir_ops;
                    set_nlink(inode, 2);
                } else {
                    inode->i_fop = &vtfs_file_ops;
                }

                i_size_write(inode, entry->size);
            }

            found = true;
            break;
        }
    }

    mutex_unlock(&vtfs_mutex);

    if (found && !inode)
        return ERR_PTR(-ENOMEM);

    d_add(child_dentry, inode);

    return NULL;
}

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

    if (!S_ISDIR(parent_inode->i_mode))
        return -ENOTDIR;

    if (strnlen(name, VTFS_NAME_LEN) >= VTFS_NAME_LEN)
        return -ENAMETOOLONG;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->parent_ino == parent_inode->i_ino &&
            !strcmp(entry->name, name)) {
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

    inode->i_fop = &vtfs_file_ops;

    new_entry->ino = vtfs_next_ino;
    new_entry->parent_ino = parent_inode->i_ino;
    new_entry->mode = S_IFREG | (mode & 0777);
    strscpy(new_entry->name, name, sizeof(new_entry->name));

    list_add_tail(&new_entry->list, &vtfs_entries);

    vtfs_next_ino++;

    d_add(child_dentry, inode);

    mutex_unlock(&vtfs_mutex);

    return 0;
}

static struct dentry *vtfs_mkdir(
    struct mnt_idmap *idmap,
    struct inode *parent_inode,
    struct dentry *child_dentry,
    umode_t mode)
{
    struct vtfs_entry *entry;
    struct vtfs_entry *new_entry;
    struct inode *inode;
    const char *name = child_dentry->d_name.name;

    if (!S_ISDIR(parent_inode->i_mode))
        return ERR_PTR(-ENOTDIR);

    if (strnlen(name, VTFS_NAME_LEN) >= VTFS_NAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->parent_ino == parent_inode->i_ino &&
            !strcmp(entry->name, name)) {
            mutex_unlock(&vtfs_mutex);
            return ERR_PTR(-EEXIST);
        }
    }

    new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);

    if (!new_entry) {
        mutex_unlock(&vtfs_mutex);
        return ERR_PTR(-ENOMEM);
    }

    inode = vtfs_get_inode(
        parent_inode->i_sb,
        parent_inode,
        S_IFDIR | (mode & 0777),
        vtfs_next_ino
    );

    if (!inode) {
        kfree(new_entry);
        mutex_unlock(&vtfs_mutex);
        return ERR_PTR(-ENOMEM);
    }

    inode->i_op = &vtfs_inode_ops;
    inode->i_fop = &vtfs_dir_ops;

    set_nlink(inode, 2);
    inc_nlink(parent_inode);

    new_entry->ino = vtfs_next_ino;
    new_entry->parent_ino = parent_inode->i_ino;
    new_entry->mode = S_IFDIR | (mode & 0777);
    strscpy(new_entry->name, name, sizeof(new_entry->name));

    list_add_tail(&new_entry->list, &vtfs_entries);

    vtfs_next_ino++;

    d_add(child_dentry, inode);

    mutex_unlock(&vtfs_mutex);

    return NULL;
}

static int vtfs_unlink(
    struct inode *parent_inode,
    struct dentry *child_dentry)
{
    struct vtfs_entry *entry;
    struct vtfs_entry *found_entry = NULL;
    struct inode *inode;

    inode = d_inode(child_dentry);

    if (!inode)
        return -ENOENT;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->parent_ino == parent_inode->i_ino &&
            !strcmp(entry->name, child_dentry->d_name.name)) {
            found_entry = entry;
            break;
        }
    }

    if (!found_entry) {
        mutex_unlock(&vtfs_mutex);
        return -ENOENT;
    }

    drop_nlink(inode);

    list_del(&found_entry->list);

    kfree(found_entry->data);
    kfree(found_entry);

    mutex_unlock(&vtfs_mutex);

    return 0;
}

static int vtfs_rmdir(
    struct inode *parent_inode,
    struct dentry *child_dentry)
{
    struct vtfs_entry *entry;
    struct vtfs_entry *found_entry = NULL;
    struct inode *inode;

    inode = d_inode(child_dentry);

    if (!inode)
        return -ENOENT;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->parent_ino == parent_inode->i_ino &&
            !strcmp(entry->name, child_dentry->d_name.name)) {
            found_entry = entry;
            break;
        }
    }

    if (!found_entry) {
        mutex_unlock(&vtfs_mutex);
        return -ENOENT;
    }

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->parent_ino == found_entry->ino) {
            mutex_unlock(&vtfs_mutex);
            return -ENOTEMPTY;
        }
    }

    clear_nlink(inode);
    drop_nlink(parent_inode);

    list_del(&found_entry->list);

    kfree(found_entry->data);
    kfree(found_entry);

    mutex_unlock(&vtfs_mutex);

    return 0;
}

static ssize_t vtfs_read(
    struct file *file,
    char __user *buffer,
    size_t count,
    loff_t *ppos)
{
    struct vtfs_entry *entry;
    ssize_t result = -ENOENT;
    ino_t ino = file_inode(file)->i_ino;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->ino == ino) {
            result = simple_read_from_buffer(
                buffer,
                count,
                ppos,
                entry->data,
                entry->size
            );
            break;
        }
    }

    mutex_unlock(&vtfs_mutex);

    return result;
}

static ssize_t vtfs_write(
    struct file *file,
    const char __user *buffer,
    size_t count,
    loff_t *ppos)
{
    struct vtfs_entry *entry;
    char *new_data;
    size_t new_size;
    size_t not_copied;
    size_t written;
    loff_t end_pos;
    ino_t ino = file_inode(file)->i_ino;
    ssize_t result = -ENOENT;

    if (count == 0)
        return 0;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->ino != ino)
            continue;

        if (file->f_flags & O_APPEND)
            *ppos = entry->size;

        if (*ppos < 0) {
            result = -EINVAL;
            break;
        }

        if (*ppos > MAX_LFS_FILESIZE ||
            count > MAX_LFS_FILESIZE - *ppos) {
            result = -EFBIG;
            break;
        }

        new_size = max_t(size_t, entry->size, *ppos + count);

        new_data = krealloc(entry->data, new_size, GFP_KERNEL);

        if (!new_data) {
            result = -ENOMEM;
            break;
        }

        entry->data = new_data;

        if (*ppos > entry->size) {
            memset(
                entry->data + entry->size,
                0,
                *ppos - entry->size
            );
        }

        not_copied = copy_from_user(
            entry->data + *ppos,
            buffer,
            count
        );

        written = count - not_copied;

        if (written == 0) {
            result = -EFAULT;
            break;
        }

        end_pos = *ppos + written;

        if (end_pos > entry->size)
            entry->size = end_pos;

        *ppos = end_pos;

        i_size_write(file_inode(file), entry->size);

        result = written;
        break;
    }

    mutex_unlock(&vtfs_mutex);

    return result;
}

static int vtfs_iterate(
    struct file *file,
    struct dir_context *ctx)
{
    struct vtfs_entry *entry;
    ino_t parent_ino = file_inode(file)->i_ino;
    int index = 0;

    if (!dir_emit_dots(file, ctx))
        return 0;

    if (!S_ISDIR(file_inode(file)->i_mode))
        return 0;

    mutex_lock(&vtfs_mutex);

    list_for_each_entry(entry, &vtfs_entries, list) {
        if (entry->parent_ino != parent_ino)
            continue;

        if (index < ctx->pos - 2) {
            index++;
            continue;
        }

        if (!dir_emit(
            ctx,
            entry->name,
            strlen(entry->name),
            entry->ino,
            S_ISDIR(entry->mode) ? DT_DIR : DT_REG
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
    .mkdir = vtfs_mkdir,
    .unlink = vtfs_unlink,
    .rmdir = vtfs_rmdir,
};

static const struct file_operations vtfs_file_ops = {
    .read = vtfs_read,
    .write = vtfs_write,
    .llseek = generic_file_llseek,
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

    set_nlink(inode, 2);

    sb->s_root = d_make_root(inode);

    if (!sb->s_root)
        return -ENOMEM;

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
        kfree(entry->data);
        kfree(entry);
    }

    vtfs_next_ino = VTFS_ROOT_INO + 1;

    mutex_unlock(&vtfs_mutex);

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

    return 0;
}

static void __exit vtfs_exit(void)
{
    unregister_filesystem(&vtfs_type);
}

module_init(vtfs_init);
module_exit(vtfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dimqi");
MODULE_DESCRIPTION("Virtual Test File System");
MODULE_VERSION("0.1");
