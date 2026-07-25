/*
 * PowerFS 内核文件系统 - 模块初始化和 fs_context 挂载
 *
 * 参考:
 *   - cephfs (fs/ceph/super.c) - 网络文件系统挂载架构
 *   - ramfs (fs/ramfs/inode.c) - 简单文件系统挂载范例
 *
 * 职责划分:
 *   - powerfs_mod.c: 模块 init/exit, fs_context 挂载参数解析
 *   - powerfs_fs.c:  inode/dentry/page cache 操作, super_operations, dentry_operations
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>

#include "powerfs.h"

/* ========== 模块参数 ========== */

static char *master_addr = "172.20.0.14";
module_param(master_addr, charp, 0644);
MODULE_PARM_DESC(master_addr, "Master server address");

static ushort master_port = 9333;
module_param(master_port, ushort, 0644);
MODULE_PARM_DESC(master_port, "Master server port");

static char *volume_addr = "172.20.0.24";
module_param(volume_addr, charp, 0644);
MODULE_PARM_DESC(volume_addr, "Volume server address");

static ushort volume_port = 8080;
module_param(volume_port, ushort, 0644);
MODULE_PARM_DESC(volume_port, "Volume server port");

static char *filer_addr = "172.20.0.37";
module_param(filer_addr, charp, 0644);
MODULE_PARM_DESC(filer_addr, "Filer server address");

static ushort filer_port = 8889;
module_param(filer_port, ushort, 0644);
MODULE_PARM_DESC(filer_port, "Filer server port");

/* ========== fs_context 参数解析 ========== */

enum powerfs_param {
    Opt_master_addr,
    Opt_master_port,
    Opt_volume_addr,
    Opt_volume_port,
    Opt_filer_addr,
    Opt_filer_port,
};

static const struct fs_parameter_spec powerfs_fs_parameters[] = {
    fsparam_string("master_addr",  Opt_master_addr),
    fsparam_u32("master_port",     Opt_master_port),
    fsparam_string("volume_addr",  Opt_volume_addr),
    fsparam_u32("volume_port",     Opt_volume_port),
    fsparam_string("filer_addr",   Opt_filer_addr),
    fsparam_u32("filer_port",      Opt_filer_port),
    {}
};

struct powerfs_ctx {
    char master_addr[64];
    u16  master_port;
    char volume_addr[64];
    u16  volume_port;
    char filer_addr[64];
    u16  filer_port;
};

/* ========== 外部函数声明 (在 powerfs_fs.c 中定义) ========== */

extern int  powerfs_init_inode_cache(void);
extern void powerfs_destroy_inode_cache(void);
extern int  powerfs_fill_super(struct super_block *sb, struct fs_context *fc);
extern void powerfs_kill_sb_super(struct super_block *sb);

/* ========== parse_param: 解析挂载参数 ========== */

static int powerfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
    struct powerfs_ctx *ctx = fc->s_fs_info;
    struct fs_parse_result result;
    int opt;

    opt = fs_parse(fc, powerfs_fs_parameters, param, &result);
    if (opt == -ENOPARAM) {
        opt = vfs_parse_fs_param_source(fc, param);
        if (opt != -ENOPARAM)
            return opt;
        return 0;
    }
    if (opt < 0)
        return opt;

    switch (opt) {
    case Opt_master_addr:
        strncpy(ctx->master_addr, param->string, sizeof(ctx->master_addr) - 1);
        pr_info("powerfs: master_addr = %s\n", ctx->master_addr);
        break;
    case Opt_master_port:
        ctx->master_port = (u16)result.uint_32;
        pr_info("powerfs: master_port = %u\n", ctx->master_port);
        break;
    case Opt_volume_addr:
        strncpy(ctx->volume_addr, param->string, sizeof(ctx->volume_addr) - 1);
        break;
    case Opt_volume_port:
        ctx->volume_port = (u16)result.uint_32;
        break;
    case Opt_filer_addr:
        strncpy(ctx->filer_addr, param->string, sizeof(ctx->filer_addr) - 1);
        break;
    case Opt_filer_port:
        ctx->filer_port = (u16)result.uint_32;
        break;
    }

    return 0;
}

/* ========== free_context ========== */

static void powerfs_free_context(struct fs_context *fc)
{
    kfree(fc->s_fs_info);
}

/* ========== get_tree ========== */

static int powerfs_get_tree(struct fs_context *fc)
{
    return get_tree_nodev(fc, powerfs_fill_super);
}

/* ========== fs_context operations ========== */

static const struct fs_context_operations powerfs_ctx_ops = {
    .free        = powerfs_free_context,
    .parse_param  = powerfs_parse_param,
    .get_tree     = powerfs_get_tree,
};

/* ========== init_fs_context ========== */

static int powerfs_init_fs_context(struct fs_context *fc)
{
    struct powerfs_ctx *ctx;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    /* 默认值 */
    strncpy(ctx->master_addr, master_addr, sizeof(ctx->master_addr) - 1);
    ctx->master_port = master_port;
    strncpy(ctx->volume_addr, volume_addr, sizeof(ctx->volume_addr) - 1);
    ctx->volume_port = volume_port;
    strncpy(ctx->filer_addr, filer_addr, sizeof(ctx->filer_addr) - 1);
    ctx->filer_port = filer_port;

    fc->s_fs_info = ctx;
    fc->ops = &powerfs_ctx_ops;

    return 0;
}

/* ========== kill_sb ========== */

static void powerfs_kill_sb(struct super_block *sb)
{
    powerfs_kill_sb_super(sb);
}

/* ========== 文件系统类型 ========== */

static struct file_system_type powerfs_fs_type = {
    .name             = "powerfs",
    .fs_flags         = 0,
    .init_fs_context  = powerfs_init_fs_context,
    .parameters       = powerfs_fs_parameters,
    .kill_sb          = powerfs_kill_sb,
    .owner            = THIS_MODULE,
};

/* ========== 模块初始化 ========== */

static int __init powerfs_init(void)
{
    int ret;

    pr_info("========================================\n");
    pr_info("  PowerFS Kernel Module v%s\n", POWERFS_VERSION);
    pr_info("  Ceph-style Architecture\n");
    pr_info("========================================\n");

    /* 创建全局 slab 缓存 */
    ret = powerfs_init_inode_cache();
    if (ret) {
        pr_err("powerfs: failed to create slab caches: %d\n", ret);
        return ret;
    }

    /* 初始化通信设备 */
    ret = powerfs_comm_init();
    if (ret) {
        pr_warn("powerfs: comm device init failed (continuing): %d\n", ret);
    }

    /* 注册文件系统 */
    ret = register_filesystem(&powerfs_fs_type);
    if (ret) {
        pr_err("powerfs: failed to register filesystem: %d\n", ret);
        powerfs_comm_exit();
        powerfs_destroy_inode_cache();
        return ret;
    }

    pr_info("powerfs: filesystem registered\n");
    pr_info("powerfs: mount -t powerfs none /mnt/powerfs\n");

    return 0;
}

/* ========== 模块退出 ========== */

static void __exit powerfs_exit(void)
{
    pr_info("powerfs: module exit\n");

    unregister_filesystem(&powerfs_fs_type);
    powerfs_comm_exit();
    powerfs_destroy_inode_cache();

    pr_info("powerfs: module unloaded\n");
}

module_init(powerfs_init);
module_exit(powerfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PowerFS Team");
MODULE_DESCRIPTION("PowerFS Kernel Filesystem (Ceph-style architecture)");
MODULE_VERSION(POWERFS_VERSION);
