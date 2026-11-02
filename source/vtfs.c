#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>

#define VTFS_MAGIC 0x76544653

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
    return NULL;
}

static int vtfs_iterate(
    struct file *file,
    struct dir_context *ctx)
{
    if (!dir_emit_dots(file, ctx))
        return 0;

    if (ctx->pos == 2) {
        if (!dir_emit(ctx, "test.txt", 8, 1001, DT_REG))
            return 0;

        ctx->pos++;
    }

    return 0;
}

static const struct inode_operations vtfs_inode_ops = {
    .lookup = vtfs_lookup,
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

    inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 1000);
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