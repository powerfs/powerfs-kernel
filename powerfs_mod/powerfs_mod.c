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

/* ========== fs_context 参数解析 ==========
 *
 * 设计原则: ALL 挂载相关参数 (master_addr/master_port/shard_count
 * /write_batch_kb) 必须通过 mount -o key=value 传递, 不能用
 * module_param 全局共享. 否则同一 ko 被多次 mount 到不同集群时,
 * 全局 module_param 会互相污染.
 *
 * 需要 master_addr: 形如 "172.30.0.11,172.30.0.12,172.30.0.13" (Raft 3 节点).
 * master_port: net_port (9334), 不是 gRPC_port (9333).
 * shard_count: Filer shard 总数, 默认 3 (对齐 3 Filer × 3 shard).
 */

enum powerfs_param {
    Opt_master_addr,
    Opt_master_port,
    Opt_shard_count,
    Opt_write_batch_kb,
    Opt_ca_crt,
    Opt_client_crt,
    Opt_client_key,
    Opt_transport,
};

static const struct fs_parameter_spec powerfs_fs_parameters[] = {
    fsparam_string("master_addr",  Opt_master_addr),
    fsparam_u32("master_port",     Opt_master_port),
    fsparam_u32("shard_count",     Opt_shard_count),
    fsparam_u32("write_batch_kb",  Opt_write_batch_kb),
    fsparam_string("ca_crt",       Opt_ca_crt),
    fsparam_string("client_crt",   Opt_client_crt),
    fsparam_string("client_key",   Opt_client_key),
    fsparam_string("transport",    Opt_transport),
    {}
};

/* Defaults — 按内核常见最佳实践, 网络地址不硬编码为外网可路由默认值,
 * 只给端口/数量等非安全敏感字段合理默认.
 * master_addr 默认留空 → fill_super 检测为空返回 -EINVAL. */
#define POWERFS_DEFAULT_MASTER_PORT  9334
#define POWERFS_DEFAULT_SHARD_COUNT  3

struct powerfs_ctx {
    char master_addr[64];
    u16  master_port;
    u16  shard_count;
    u32  write_batch_kb;
    /* 证书路径: 由 mount -o ca_crt=/... client_crt=/... client_key=/... 传入.
     * 空字符串代表未提供, 由 Master net_handler 在强制模式下拒绝挂载.
     * 路径长度上限 511B (兼容大多数绝对路径场景). */
    char ca_crt[512];
    char client_crt[512];
    char client_key[512];
    /* 传输层选择: "tcp" (默认) 或 "rdma" (CONFIG_INFINIBAND=y 时可用).
     * 由 mount -o transport=tcp|rdma 传入. fill_super 解析为 transport_type
     * 存入 sbi->transport_type 并传播到 g_pool.transport_type. */
    char transport[8];
};

/* ========== 外部函数声明 (在 powerfs_fs.c 中定义) ========== */

extern int  powerfs_init_inode_cache(void);
extern void powerfs_destroy_inode_cache(void);
extern void powerfs_dentry_dedup_destroy_all(void);

/* Phase 2: 流控模块 */
extern int  powerfs_flow_init(void);
extern void powerfs_flow_exit(void);
extern int  powerfs_fill_super(struct super_block *sb, struct fs_context *fc);
extern void powerfs_kill_sb_super(struct super_block *sb);

/* ========== parse_param: 解析挂载参数 ========== */

/* 辅助: 判断字符串是否像 IPv4 地址 "x.x.x.x" (每段 0-255, 4 段).
 * 注意: 只做启发式判定 (digit+dot 组成且有 3 个 dot),
 * 不严格校验范围, 后续 powerfs_net_tcp_connect 内 in4_pton 会严格校验. */
static inline bool powerfs_looks_like_ipv4(const char *s)
{
    int dots = 0;
    const char *p;
    if (!s || !*s)
        return false;
    for (p = s; *p; p++) {
        if (*p == '.')
            dots++;
        else if (*p < '0' || *p > '9')
            return false;
    }
    return (dots == 3);
}

/* 辅助: 把 "master_addr 值" 和后续 "IP 形 stray options" 用逗号
 * 连接到 ctx->master_addr (cap at sizeof-1). 处理两种常见写法:
 *   A) mount -o master_addr=172.30.0.11,172.30.0.12,172.30.0.13,...
 *        → util-linux/VFS 按逗号拆成多个 param:
 *            {key="master_addr", string="172.30.0.11"}
 *            {key="172.30.0.12"}  (无 value, -ENOPARAM 路径)
 *            {key="172.30.0.13"}
 *        → 我们用 L86 -ENOPARAM 分支抓取 stray IP keys 并 append.
 *   B) mount -o master_addr=172.30.0.11:172.30.0.12:172.30.0.13
 *        → 单个 string param, 内部用 ':' 分隔.
 * 我们先把所有 ',' (内部拼接) 和 ':' (B 写法) 统一成 ',' 存 ctx,
 * 后续 discover_filers 里 strsep(&p, ",") 就能正确拆分. */
static int powerfs_ctx_append_master(struct powerfs_ctx *ctx, const char *more)
{
    size_t cur, left, need;
    char *dst;

    if (!ctx || !more || !*more)
        return 0;

    /* B 写法: 把 more 里的 ':' 统一成 ','. */
    /* 先计算 needed 长度: more 原长度 + 可能的 leading ','. */
    cur = strlen(ctx->master_addr);
    need = strlen(more);
    if (cur > 0)
        need += 1;  /* leading comma */

    if (cur + need >= sizeof(ctx->master_addr))
        return -ENAMETOOLONG;

    dst = &ctx->master_addr[cur];
    left = sizeof(ctx->master_addr) - cur - 1;
    if (cur > 0) {
        *dst++ = ',';
        left--;
    }
    strncpy(dst, more, left);
    dst[left] = '\0';

    /* 将新追加部分的 ':' 转成 ','. */
    for (dst = &ctx->master_addr[cur ? cur + 1 : 0]; *dst; dst++) {
        if (*dst == ':')
            *dst = ',';
    }
    return 0;
}

static int powerfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
    struct powerfs_ctx *ctx = fc->s_fs_info;
    struct fs_parse_result result;
    int opt;

    pr_info("powerfs: parse_param: key=%s, ctx=%px\n", param->key, ctx);

    opt = fs_parse(fc, powerfs_fs_parameters, param, &result);
    if (opt == -ENOPARAM) {
        /* 未识别参数: 可能是 A 写法下逗号拆出的 "master_addr 第二个 IP".
         *    key = "172.30.0.12" / "172.30.0.13" 等 IPv4 字符串, 无 value.
         * 若 match, 追加到 ctx->master_addr;
         * 否则交给 vfs source parse, 不识别就静默忽略 (对齐 -ENOPARAM
         * 常见行为: 避免 mount 因拼写错误参数时直接 EINVAL 不好排查,
         * 我们仍然会在 fill_super 强校验 master_addr 非空, 所以不会静默连错). */
        if (powerfs_looks_like_ipv4(param->key)) {
            int rc = powerfs_ctx_append_master(ctx, param->key);
            pr_info("powerfs: parse_param: stray-IP key '%s' appended to master_addr (rc=%d, now='%s')\n",
                    param->key, rc, ctx->master_addr);
            return rc;
        }
        opt = vfs_parse_fs_param_source(fc, param);
        if (opt != -ENOPARAM)
            return opt;
        return 0;
    }
    if (opt < 0)
        return opt;

    switch (opt) {
    case Opt_master_addr: {
        /* 重置再 append: 支持 B 写法内部 ':' 分隔 + 之后再跟 stray keys 逗号追加. */
        ctx->master_addr[0] = '\0';
        powerfs_ctx_append_master(ctx, param->string);
        pr_info("powerfs: master_addr = %s\n", ctx->master_addr);
        break;
    }
    case Opt_master_port:
        ctx->master_port = (u16)result.uint_32;
        pr_info("powerfs: master_port = %u\n", ctx->master_port);
        break;
    case Opt_shard_count:
        if (result.uint_32 == 0) {
            pr_err("powerfs: shard_count must be > 0\n");
            return -EINVAL;
        }
        if (result.uint_32 > 65535) {
            pr_err("powerfs: shard_count %u exceeds u16 max\n", result.uint_32);
            return -ERANGE;
        }
        ctx->shard_count = (u16)result.uint_32;
        pr_info("powerfs: shard_count = %u\n", ctx->shard_count);
        break;
    case Opt_write_batch_kb:
        ctx->write_batch_kb = result.uint_32;
        pr_info("powerfs: write_batch_kb = %u\n", ctx->write_batch_kb);
        break;
    case Opt_ca_crt:
        if (param->string) {
            strncpy(ctx->ca_crt, param->string, sizeof(ctx->ca_crt) - 1);
            ctx->ca_crt[sizeof(ctx->ca_crt) - 1] = '\0';
            pr_info("powerfs: ca_crt = %s\n", ctx->ca_crt);
        }
        break;
    case Opt_client_crt:
        if (param->string) {
            strncpy(ctx->client_crt, param->string, sizeof(ctx->client_crt) - 1);
            ctx->client_crt[sizeof(ctx->client_crt) - 1] = '\0';
            pr_info("powerfs: client_crt = %s\n", ctx->client_crt);
        }
        break;
    case Opt_client_key:
        if (param->string) {
            strncpy(ctx->client_key, param->string, sizeof(ctx->client_key) - 1);
            ctx->client_key[sizeof(ctx->client_key) - 1] = '\0';
            pr_info("powerfs: client_key = %s\n", ctx->client_key);
        }
        break;
    case Opt_transport:
        if (param->string) {
            strncpy(ctx->transport, param->string, sizeof(ctx->transport) - 1);
            ctx->transport[sizeof(ctx->transport) - 1] = '\0';
            /* 校验: 只接受 "tcp" 或 "rdma", 其他值拒绝挂载避免静默走错路径. */
            if (strcmp(ctx->transport, "tcp") != 0 &&
                strcmp(ctx->transport, "rdma") != 0) {
                pr_err("powerfs: invalid transport='%s' (must be tcp or rdma)\n",
                       ctx->transport);
                return -EINVAL;
            }
            pr_info("powerfs: transport = %s\n", ctx->transport);
        }
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

    /* 默认值: 只给端口/shard_count/写批大小等非安全敏感字段合理默认.
     * master_addr 留空, mount 时必须传;
     * fill_super 会检查并在未提供时返回 -EINVAL, 避免连接到意外的集群. */
    ctx->master_port = POWERFS_DEFAULT_MASTER_PORT;
    ctx->shard_count = POWERFS_DEFAULT_SHARD_COUNT;
    ctx->write_batch_kb = POWERFS_WRITE_BATCH_DEFAULT_KB;
    /* 默认传输: tcp (最通用, 无需 CONFIG_INFINIBAND).
     * rdma 需要内核 CONFIG_INFINIBAND=y + 服务端 RDMA 监听. */
    strcpy(ctx->transport, "tcp");

    fc->s_fs_info = ctx;
    fc->ops = &powerfs_ctx_ops;

    pr_info("powerfs: init_fs_context done, ctx=%px (expect mount -o master_addr=...,master_port=,shard_count=)\n",
            ctx);

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
    pr_info("========================================\n");

    /* 创建全局 slab 缓存 */
    ret = powerfs_init_inode_cache();
    if (ret) {
        pr_err("powerfs: failed to create slab caches: %d\n", ret);
        return ret;
    }

    /* Phase 2: 初始化流控模块 */
    ret = powerfs_flow_init();
    if (ret) {
        pr_warn("powerfs: flow controller init failed (continuing): %d\n", ret);
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
        powerfs_flow_exit();
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
    powerfs_flow_exit();

    /* 等待所有 pending RCU 回调完成, 确保 kill_sb 期间排队的
     * call_rcu (dentry_info 释放等) 在 slab 缓存销毁前执行完毕.
     *
     * kill_sb 中 kill_anon_super 后的 rcu_barrier 覆盖单次 mount 的回调,
     * 但如果有多个 mount 或 umount 后仍有延迟回调, 需要此处兜底.
     * 否则 kmem_cache_destroy(dentry_cachep) 后, powerfs_di_free_rcu
     * 执行 kmem_cache_free 到已销毁缓存 → SLUB 元数据损坏 →
     * 随机内存腐败 (表现为 bpf_prog_aux 被覆盖等). */
    rcu_barrier();

    powerfs_destroy_inode_cache();
    powerfs_dentry_dedup_destroy_all();

    pr_info("powerfs: module unloaded\n");
}

module_init(powerfs_init);
module_exit(powerfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PowerFS Team");
MODULE_DESCRIPTION("PowerFS Kernel Filesystem");
MODULE_VERSION(POWERFS_VERSION);
