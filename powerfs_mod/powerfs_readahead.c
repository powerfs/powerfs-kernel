// SPDX-License-Identifier: GPL-2.0
/* powerfs_readahead.c - ML 自适应预取 Phase A-0 (kernel 客户端)
 *
 * 详见 docs/ml-prefetch-kernel-rdma-plan.md §4.5-4.8.
 *
 * Phase A-0 (本文件): 手工基线
 *   - xattr user.powerfs.readahead_policy=<MB 数> 由 setfattr 手工设置
 *   - 首次 read_iter 触发 xattr RPC, 缓存到 pi->readahead_mb
 *   - 后续 read_iter 直接用缓存值改 file->f_ra.ra_pages
 *
 * Phase A-1 (后续): filer NN 训练 + 自动下发, 本文件不动 (只加 version 失效)
 *
 * 安全回退:
 *   - mount readahead=off → 全局跳过, 用 VFS 默认
 *   - xattr 不存在 (-ENODATA) → 不缓存, 用 VFS 默认 (不污染 pi 字段)
 *   - xattr RPC 失败 → 不缓存, 用 VFS 默认 (best-effort, 不阻塞 I/O)
 *   - readahead_mb 解析失败 → 不缓存, 用 VFS 默认
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/printk.h>

#include "powerfs.h"
#include "powerfs_readahead.h"
#include "powerfs_net.h"

/* 解析 xattr value 为 readahead MB 数
 *
 * xattr 值格式 (§4.6.3):
 *   "<bytes>"       纯数字 MB (A-0)
 *   "NN:<bytes>"    A-1 NN 下发 (前缀调试标识, 解析时跳过)
 *   "RULE:<bytes>"  规则引擎下发 (同上)
 *
 * 解析规则:
 *   - 跳过 'NN:' / 'RULE:' 前缀
 *   - 取连续数字, 转 u32
 *   - 0 = 关闭预取; >0 = N MB (kernel 端按 N*1024/PAGE_SIZE 转 ra_pages)
 *   - 上限 256MB (避免恶意/错误值打爆 MR 池)
 *
 * 返回 0 成功 (out 写入 MB 数); -EINVAL 解析失败
 */
static int powerfs_readahead_parse_xattr(const u8 *value, size_t value_len,
                                          u32 *out_mb)
{
    const char *s = (const char *)value;
    size_t len = value_len;
    u32 mb = 0;
    int digits = 0;

    if (!value || value_len == 0 || !out_mb)
        return -EINVAL;

    /* 跳过 'NN:' / 'RULE:' 前缀 (调试标识, kernel 端只看数字) */
    if (len >= 3 && s[2] == ':') {
        if ((s[0] == 'N' || s[0] == 'n') &&
            (s[1] == 'N' || s[1] == 'n')) {
            s += 3; len -= 3;
        }
    }
    if (len >= 5 && s[4] == ':') {
        if ((s[0] == 'R' || s[0] == 'r') &&
            (s[1] == 'U' || s[1] == 'u') &&
            (s[2] == 'L' || s[2] == 'l') &&
            (s[3] == 'E' || s[3] == 'e')) {
            s += 5; len -= 5;
        }
    }

    /* 取连续数字 */
    while (len > 0 && isdigit(*s)) {
        u32 digit = *s - '0';
        if (mb > (256 * 1024 * 1024 - digit) / 10)
            return -EINVAL;  /* 溢出或超过 256MB */
        mb = mb * 10 + digit;
        s++; len--; digits++;
    }

    if (digits == 0)
        return -EINVAL;  /* 无数字 */

    /* mb 是 MB 数, 限制 [0, 256] */
    if (mb > 256)
        return -EINVAL;

    *out_mb = mb;
    return 0;
}

/* 从 filer 查 xattr, 解析为 readahead_mb, 写入 pi 字段
 *
 * 调用上下文: read_iter (进程上下文, 可睡眠, 已 admit 通过流控)
 * 持锁情况: 调用方未持 pi->i_lock (我们用 spin_lock 短时保护字段写入)
 *
 * 实现: 用 __vfs_getxattr (内核导出), 自动走 powerfs_xattr_handler_get 路径
 *       (L1 simple_xattr cache → cache miss 时 net RPC → 回填 L1)
 *       不直接调 powerfs_net_getxattr (绕过 L1 会导致 setfattr 后读不到)
 *
 * 返回 0 = 成功缓存 (pi->readahead_mb 已更新, cached=true)
 *        -ENODATA = xattr 不存在 (VFS 默认, cached=false, 不再重试)
 *        -EINVAL = 解析失败 (同上, 不重试)
 *        <0 = RPC 失败 (cached=false, 下次 read_iter 会重试)
 */
static int powerfs_readahead_load_from_filer(struct file *file,
                                              struct inode *inode,
                                              struct powerfs_inode_info *pi)
{
    /* xattr value 通常 < 32 字节 ("16" / "NN:16" / "RULE:16" 等).
     * 用栈 buffer 避免动态分配, 满足 readahead 路径的低开销要求. */
    u8 value_buf[32];
    ssize_t value_len;
    u32 mb = 0;
    int ret;

    value_len = __vfs_getxattr(file->f_path.dentry, inode,
                                POWERFS_READAHEAD_XATTR_NAME,
                                value_buf, sizeof(value_buf) - 1);
    if (value_len < 0)
        return value_len;  /* -ENODATA / -ERANGE / -其他 */

    /* value_len=0 = 空 xattr (filer 端写空值), 视为未设置 */
    if (value_len == 0)
        return -ENODATA;

    /* 解析失败 = xattr 值格式错, 不缓存 (让 VFS 默认 + dmesg 警告) */
    ret = powerfs_readahead_parse_xattr(value_buf, value_len, &mb);
    if (ret < 0) {
        pr_warn_ratelimited("powerfs: readahead xattr ino=%lu value='%.*s' parse failed: %d\n",
                            inode->i_ino, (int)value_len, value_buf, ret);
        return ret;
    }

    spin_lock(&pi->i_lock);
    pi->readahead_mb = mb;
    pi->readahead_policy_cached = true;
    /* A-1 阶段: pi->readahead_policy_version 由 filer PushDelta 时 bump */
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: readahead cached ino=%lu mb=%u\n",
             inode->i_ino, mb);
    return 0;
}

int powerfs_readahead_apply(struct file *file, struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);
    u32 mb;
    bool cached;
    bool disabled;
    int ret;

    /* 全局禁用 (mount readahead=off): 跳过所有逻辑, 用 VFS 默认 */
    if (sbi->readahead_mode == POWERFS_READAHEAD_OFF)
        return 0;

    /* 读字段 (持锁短时, 不在锁内做 RPC) */
    spin_lock(&pi->i_lock);
    disabled = pi->readahead_policy_disabled;
    cached = pi->readahead_policy_cached;
    mb = pi->readahead_mb;
    spin_unlock(&pi->i_lock);

    /* per-inode 禁用 (A-1 invalidation 时 disabled=true 不再用,
     * A-0 阶段不使用此路径, 此分支是防御性兜底) */
    if (disabled)
        return 0;

    /* 未缓存: 触发 xattr 加载 (best-effort, 失败不阻塞 I/O) */
    if (!cached) {
        ret = powerfs_readahead_load_from_filer(file, inode, pi);
        if (ret < 0) {
            /* -ENODATA / -EINVAL = 永久不存在/格式错, 不重试 (但当前实现每次都查,
             * 简单起见 A-0 不做负缓存; A-1 阶段加 invalidation 后再优化).
             * 其他错误 = RPC 失败, 下次 read_iter 重试.
             * 任何失败都用 VFS 默认. */
            pr_debug("powerfs: readahead load failed ino=%lu ret=%d, using VFS default\n",
                     inode->i_ino, ret);
            return 0;
        }

        /* 重新读 cached 字段 (load_from_filer 已写入) */
        spin_lock(&pi->i_lock);
        mb = pi->readahead_mb;
        spin_unlock(&pi->i_lock);
    }

    /* 用 mb 改 file->f_ra.ra_pages
     *
     * readahead_mb = 0 → ra_pages = 0 关闭预取 (random 工作负载, 关键收益点)
     * readahead_mb = N → ra_pages = N * 1024 / PAGE_SIZE (对齐 RDMA 2MB 帧)
     *
     * A-X1 (§4.3): RDMA MR 池占用感知. data_pool 仅 48 个 2MB MR (32 个
     * pre-post RECV), 预取一次性发起多个并发读可能占满 MR → RNR. 根据当前
     * 空闲 MR 数裁剪 mb (仅 RDMA 传输生效, TCP 原样返回).
     *
     * 注意: file->f_ra 是 per-file descriptor, 不是 per-inode,
     * 多个 open 同一 inode 各自的 f_ra 独立, 此处改的是当前 file.
     */
    if (mb > 0)
        mb = powerfs_rdma_cap_readahead_mb(mb);

    if (mb == 0) {
        file->f_ra.ra_pages = 0;
    } else {
        unsigned long ra_pages = (unsigned long)mb * 1024 * 1024 / PAGE_SIZE;
        /* ra_pages 0 = 关闭, >0 = 最多预读 N 页 */
        file->f_ra.ra_pages = ra_pages;
    }

    pr_debug("powerfs: readahead apply ino=%lu mb=%u ra_pages=%lu\n",
             inode->i_ino, mb, file->f_ra.ra_pages);
    return 0;
}

void powerfs_readahead_invalidate(struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct simple_xattr *old;

    spin_lock(&pi->i_lock);
    pi->readahead_policy_cached = false;
    /* mb 保留 (下次加载前用旧值兜底, 避免空窗期); version 由 filer 端 bump */
    spin_unlock(&pi->i_lock);

    /* A-1.4: Filer 端规则引擎/NN 可能直接改 xattr (不走 kernel setxattr 路径),
     * 此时 kernel 的 L1 xattr cache (pi->xattrs) 仍是旧值. 若不清除,
     * __vfs_getxattr 会命中 L1 返回过期策略 (如 RULE:0), 导致新策略不生效.
     * 从 L1 删除 readahead xattr, 下次 read_iter 重新从 Filer 拉取. */
    old = simple_xattr_set(&pi->xattrs, POWERFS_READAHEAD_XATTR_NAME, NULL, 0, 0);
    if (!IS_ERR_OR_NULL(old))
        simple_xattr_free(old);
}

void powerfs_readahead_inode_init(struct powerfs_inode_info *pi)
{
    /* 字段归零: cached=false (未查 xattr), disabled=false (per-inode 不单独禁用),
     * mb=0, version=0. 全局禁用由 sbi->readahead_mode 在 apply 时检查. */
    pi->readahead_mb = 0;
    pi->readahead_policy_cached = false;
    pi->readahead_policy_disabled = false;
    pi->readahead_policy_version = 0;

    /* A-1.1 trace 字段归零 */
    memset(pi->io_trace_offsets, 0, sizeof(pi->io_trace_offsets));
    memset(pi->io_trace_kinds, 0, sizeof(pi->io_trace_kinds));
    pi->io_trace_idx = 0;
    pi->io_trace_seq_run = 0;
    pi->io_trace_rand_run = 0;
    pi->io_trace_sample_counter = 0;
    pi->io_trace_last_off = 0;
    pi->io_trace_last_valid = 0;
    pi->io_trace_dirty = 0;
}

/* ====================================================================
 * A-1.1 IO trace 采集 + 异步 flush
 * ==================================================================== */

#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/unaligned.h>
#include "powerfs_net.h"

/* 单条 trace snapshot (与 filer 端协议对齐) */
struct powerfs_io_trace_entry {
    __u64 ino;
    __u8  placement;
    __u64 file_size;
    __u16 offsets[POWERFS_IO_TRACE_RING_SIZE];
    __u8  kinds[POWERFS_IO_TRACE_RING_SIZE];
    __u16 seq_run;
    __u16 rand_run;
} __packed;

/* 全局 flush buffer: 采样时写入, workqueue 批量发送到 filer */
static struct powerfs_io_trace_entry trace_flush_buf[POWERFS_IO_TRACE_FLUSH_BATCH];
static DEFINE_SPINLOCK(trace_flush_lock);
static int trace_flush_count;
static struct workqueue_struct *trace_flush_wq;
static struct delayed_work trace_flush_work;
static bool trace_flush_running;

/* 前向声明: enqueue 中可能调用 flush */
void powerfs_io_trace_flush_now(void);

/* 从 inode 提取 placement (0=Inline, 1=Flat, 2=Stripe) */
static u8 powerfs_io_trace_get_placement(struct powerfs_inode_info *pi)
{
    /* pi->placement 是 enum powerfs_placement (0=Inline, 1=Flat, 2=Stripe) */
    return pi->placement;
}

/* 把一条 trace snapshot 加入全局 flush buffer.
 * buffer 满则立即触发 flush (在进程上下文中同步发, 可能阻塞,
 * 但只在 1/100 采样且 64 条满时发生, 频率极低). */
static void powerfs_io_trace_enqueue(struct powerfs_inode_info *pi)
{
    struct powerfs_io_trace_entry e;
    unsigned long flags;
    int idx;

    /* 构造 snapshot */
    e.ino = pi->netfs.inode.i_ino;
    e.placement = powerfs_io_trace_get_placement(pi);
    e.file_size = pi->netfs.inode.i_size;
    memcpy(e.offsets, pi->io_trace_offsets, sizeof(e.offsets));
    memcpy(e.kinds, pi->io_trace_kinds, sizeof(e.kinds));
    e.seq_run = pi->io_trace_seq_run;
    e.rand_run = pi->io_trace_rand_run;

    spin_lock_irqsave(&trace_flush_lock, flags);
    if (trace_flush_count >= POWERFS_IO_TRACE_FLUSH_BATCH) {
        spin_unlock_irqrestore(&trace_flush_lock, flags);
        /* buffer 满, 立即 flush (同步发送) */
        powerfs_io_trace_flush_now();
        spin_lock_irqsave(&trace_flush_lock, flags);
    }
    idx = trace_flush_count++;
    spin_unlock_irqrestore(&trace_flush_lock, flags);

    if (idx < POWERFS_IO_TRACE_FLUSH_BATCH)
        trace_flush_buf[idx] = e;
}

void powerfs_io_trace_record(struct inode *inode, loff_t offset, int kind)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    u32 cnt;
    u8 idx;
    u16 off16;

    /* 1/100 采样: counter 自增, 取模判断 */
    cnt = pi->io_trace_sample_counter + 1;
    pi->io_trace_sample_counter = cnt;
    if (cnt % POWERFS_IO_TRACE_SAMPLE_RATE != 0)
        return;

    /* offset 取高 16 位 (4GB 粒度), 压缩存储 */
    off16 = (u16)(offset >> 16);

    idx = pi->io_trace_idx;
    pi->io_trace_offsets[idx] = off16;
    pi->io_trace_kinds[idx] = (u8)kind;

    /* 计算与上一次采样的 offset 关系, 更新 seq/rand run.
     * 注意: 1/100 采样后, 顺序 I/O 的 offset delta 可能很大 (100*io_size),
     * 因此用单调性而非 proximity 判断: offset 单调递增 → 顺序. */
    if (pi->io_trace_last_valid) {
        if (offset > pi->io_trace_last_off) {
            pi->io_trace_seq_run++;
            pi->io_trace_rand_run = 0;
        } else if (offset < pi->io_trace_last_off) {
            pi->io_trace_rand_run++;
            pi->io_trace_seq_run = 0;
        }
        /* offset == last_off: re-read, 不改变 run */
    }
    pi->io_trace_last_off = offset;
    pi->io_trace_last_valid = 1;

    /* ring buffer 前进 */
    idx++;
    if (idx >= POWERFS_IO_TRACE_RING_SIZE)
        idx = 0;
    pi->io_trace_idx = idx;

    pi->io_trace_dirty = 1;

    pr_debug_ratelimited("powerfs: io_trace sampled ino=%lu off=%lld kind=%d cnt=%u\n",
                        inode->i_ino, offset, kind, cnt);

    /* 加入 flush buffer (best-effort, 失败不阻塞 I/O) */
    powerfs_io_trace_enqueue(pi);
}

/* 序列化 flush buffer 并发送到 filer.
 * 调用上下文: workqueue (可睡眠) 或 trace_enqueue 的进程上下文. */
void powerfs_io_trace_flush_now(void)
{
    struct powerfs_io_trace_entry local_buf[POWERFS_IO_TRACE_FLUSH_BATCH];
    int count;
    unsigned long flags;
    __u8 *body;
    size_t body_len;
    int ret;
    int i;

    /* 取出 buffer, 重置 count */
    spin_lock_irqsave(&trace_flush_lock, flags);
    count = trace_flush_count;
    if (count == 0) {
        spin_unlock_irqrestore(&trace_flush_lock, flags);
        return;
    }
    memcpy(local_buf, trace_flush_buf, count * sizeof(struct powerfs_io_trace_entry));
    trace_flush_count = 0;
    spin_unlock_irqrestore(&trace_flush_lock, flags);

    /* 序列化: ShardId(u64=0) + Count(u32) + entries[]
     * 用 route_inode=0 路由到任意 shard (trace 不绑定特定 shard). */
    body_len = 8 + 4 + count * sizeof(struct powerfs_io_trace_entry);
    body = kmalloc(body_len, GFP_KERNEL);
    if (!body)
        return;

    /* ShardId = 0 (trace 全局上报, 不按 inode 路由) */
    put_unaligned_le64(0, body);
    put_unaligned_le32((u32)count, body + 8);
    for (i = 0; i < count; i++)
        memcpy(body + 12 + i * sizeof(struct powerfs_io_trace_entry),
               &local_buf[i], sizeof(struct powerfs_io_trace_entry));

    /* 同步发送到 filer (workqueue 上下文, 可阻塞).
     * timeout 500ms, 失败丢弃 (trace 是 best-effort). */
    ret = powerfs_net_send_request(POWERFS_NET_MSG_PUSH_IO_TRACE, 0,
                                    body, body_len, NULL, 0,
                                    NULL, 0, NULL, 0,
                                    500, NULL, NULL);
    if (ret < 0)
        pr_info_ratelimited("powerfs: io_trace flush failed: %d (count=%d)\n", ret, count);
    else
        pr_info_ratelimited("powerfs: io_trace flushed %d entries\n", count);

    kfree(body);
}

static void trace_flush_workfn(struct work_struct *work)
{
    if (trace_flush_running) {
        powerfs_io_trace_flush_now();
        /* 重新调度 */
        queue_delayed_work(trace_flush_wq, &trace_flush_work,
                           msecs_to_jiffies(POWERFS_IO_TRACE_FLUSH_INTERVAL_MS));
    }
}

int powerfs_io_trace_flush_init(void)
{
    spin_lock_init(&trace_flush_lock);
    trace_flush_count = 0;

    trace_flush_wq = create_singlethread_workqueue("pfs_trace_flush");
    if (!trace_flush_wq) {
        pr_err("powerfs: io_trace flush workqueue create failed\n");
        return -ENOMEM;
    }

    trace_flush_running = true;
    INIT_DELAYED_WORK(&trace_flush_work, trace_flush_workfn);
    queue_delayed_work(trace_flush_wq, &trace_flush_work,
                       msecs_to_jiffies(POWERFS_IO_TRACE_FLUSH_INTERVAL_MS));
    pr_info("powerfs: io_trace flush workqueue started (interval=%dms, batch=%d)\n",
            POWERFS_IO_TRACE_FLUSH_INTERVAL_MS, POWERFS_IO_TRACE_FLUSH_BATCH);
    return 0;
}

void powerfs_io_trace_flush_exit(void)
{
    trace_flush_running = false;
    if (trace_flush_wq) {
        cancel_delayed_work_sync(&trace_flush_work);
        /* flush 剩余 */
        powerfs_io_trace_flush_now();
        destroy_workqueue(trace_flush_wq);
        trace_flush_wq = NULL;
    }
}

MODULE_DESCRIPTION("PowerFS ML-adaptive readahead (Phase A-0/A-1.1)");
MODULE_LICENSE("GPL");
