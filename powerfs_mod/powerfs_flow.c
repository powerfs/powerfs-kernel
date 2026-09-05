// SPDX-License-Identifier: GPL-2.0
/*
 * powerfs_flow.c - PowerFS kernel client flow control (Phase 2)
 *
 * 独立流控模块: 集中管理 admission / stats / load_factor 自适应.
 *
 * 核心路径:
 *   VFS callback (锁外)
 *     → powerfs_flow_admit(op, flow_idx) → ADMIT/QUEUE/REJECT
 *     → powerfs_flow_record_start(flow_idx, est_bytes)
 *     → [发送请求, 等待响应]
 *     → powerfs_flow_record_complete(flow_idx, lat_ns, bytes, error)
 *
 *   pfs_rx_dispatch (网络层)
 *     → 从响应帧 flags bits 6-7 提取 load_factor
 *     → powerfs_flow_on_load_factor(flow_idx, lf)
 *
 * 设计要点:
 *   - 全原子操作, 无锁 (参照服务端 flow_control.rs)
 *   - admit() 在 VFS 锁外调用, 不持任何 VFS 锁
 *   - load_factor 自适应: lf=0→100%, lf=1→75%, lf=2→50%, lf=3→25%
 *   - EWMA 延迟跟踪 + 慢连接标记
 *   - debugfs 暴露统计 (per-conn + global)
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/jiffies.h>

#include "powerfs_flow.h"
#include "powerfs_net.h"

/* ========== 全局单例 ========== */

static struct powerfs_flow_controller g_flow;

/* 前置声明 */
static void flow_debugfs_init(void);

/* ========== 辅助函数 ========== */

static struct powerfs_flow_conn_stats *flow_get_stats(int flow_idx)
{
    if (flow_idx >= 0 && flow_idx < POWERFS_FLOW_MAX_CONNS)
        return &g_flow.conns[flow_idx];
    return NULL;
}

/* EWMA 更新: ewma = ewma * (1-α) + new * α, α = 1/8 */
static void ewma_update(atomic64_t *ewma, u64 new_val)
{
    u64 old = atomic64_read(ewma);
    u64 updated;

    /* ewma = old - old/8 + new/8 = old * 7/8 + new/8 */
    updated = old - (old >> POWERFS_FLOW_EWMA_ALPHA_SHIFT)
              + (new_val >> POWERFS_FLOW_EWMA_ALPHA_SHIFT);

    atomic64_set(ewma, updated);
}

/* ========== 公开 API ========== */

u8 powerfs_flow_max_load_factor(void)
{
    u8 max_lf = 0;
    int i;

    /* 只扫描 filer conns [0, MAX_FILERS) */
    for (i = 0; i < POWERFS_NET_MAX_FILERS; i++) {
        u8 lf = (u8)atomic_read(&g_flow.conns[i].server_load_factor);
        if (lf > max_lf)
            max_lf = lf;
    }
    return max_lf;
}

enum powerfs_flow_decision powerfs_flow_admit_vfs(enum powerfs_flow_op op)
{
    int global_active;
    int max;
    u8 lf;

    global_active = atomic_read(&g_flow.global.active_reqs);
    lf = powerfs_flow_max_load_factor();
    max = g_flow.max_active_global;

    /* Phase 2: load_factor 自适应降速 */
    switch (lf) {
    case POWERFS_FLOW_LF_OVERLOAD:
        max = max / 4;         /* 25% */
        break;
    case POWERFS_FLOW_LF_BUSY:
        max = max / 2;         /* 50% */
        break;
    case POWERFS_FLOW_LF_NORMAL:
        max = (max * 3) / 4;   /* 75% */
        break;
    case POWERFS_FLOW_LF_IDLE:
    default:
        break;                 /* 100% */
    }

    if (max < 1)
        max = 1;

    if (global_active >= max) {
        if (lf >= POWERFS_FLOW_LF_OVERLOAD)
            return POWERFS_FLOW_REJECT;
        return POWERFS_FLOW_QUEUE;
    }

    return POWERFS_FLOW_ADMIT;
}

int powerfs_flow_admit_wait(enum powerfs_flow_op op, int timeout_ms)
{
    long ret;
    int global_active;

    /* 快速路径: 立即准入 */
    if (powerfs_flow_admit_vfs(op) == POWERFS_FLOW_ADMIT)
        return 0;

    /* 排队路径: 在 admit_wq 上等待, record_complete 唤醒 */
    global_active = atomic_read(&g_flow.global.active_reqs);
    atomic64_inc(&g_flow.global.queue_count);
    pr_debug_ratelimited("powerfs: flow queue op=%d active=%d max=%u\n",
                        op, global_active, g_flow.max_active_global);

    ret = wait_event_timeout(g_flow.admit_wq,
        powerfs_flow_admit_vfs(op) == POWERFS_FLOW_ADMIT,
        msecs_to_jiffies(timeout_ms));

    if (ret > 0) {
        atomic64_inc(&g_flow.global.queue_wakeups);
        return 0;  /* 被唤醒, 条件满足 */
    }

    /* 超时: 最后检查一次 */
    if (powerfs_flow_admit_vfs(op) == POWERFS_FLOW_ADMIT) {
        atomic64_inc(&g_flow.global.queue_wakeups);
        return 0;
    }

    atomic64_inc(&g_flow.global.reject_count);
    pr_debug_ratelimited("powerfs: flow reject op=%d timeout=%dms active=%d\n",
                        op, timeout_ms,
                        atomic_read(&g_flow.global.active_reqs));
    return -EBUSY;  /* 超时仍未准入 */
}

/*
 * powerfs_flow_wait_wr_slot - writeback 数据写的 per-conn 在途阻塞限流.
 *
 * 背景: writeback 的 async WriteNeedle/WriteNeedleBlob 路径只调
 * powerfs_flow_record_start/complete (统计在途数), 从不调 admit 准入,
 * 顺序大块写 (如 fio/ior 100+ 个 1MB chunk) 会在瞬间把整条 volume 连接的
 * 在途请求打到服务端流控硬阈值. 服务端 flow_policy 在 conn_active >=
 * max_active_per_conn (慢连接减半) 时直接 Reject(ConnFull) 并回
 * STATUS_ERR_SERVER_ERROR, 内核 write_cb 不重试 → 这些 chunk 静默丢失,
 * 重读校验读到 0/旧数据 (P0-1 ior-hard-write BW=0 的直接表现之一).
 *
 * 修复: async 写提交前在进程上下文 (writeback workqueue, 可睡眠) 阻塞等待,
 * 把单连接在途写数限制在硬阈值以下. 上限取 max_active_per_conn/2, 与服务端
 * "慢连接 effective=max/2" 对齐并给同连接的 lease/元数据请求留出余量, 保证
 * 任何负载下都不触发服务端 ConnFull reject. record_complete 会 wake_up
 * admit_wq, 写完成即放行排队者.
 *
 * 返回 0: 获得槽位 (调用方接着 record_start + do_send);
 *       -EBUSY: 超时 (极端情况, 调用方按提交失败处理, 页映射错误而非丢数据).
 */
int powerfs_flow_wait_wr_slot(int flow_idx, int timeout_ms)
{
    struct powerfs_flow_conn_stats *stats;
    int cap;
    long ret;

    stats = flow_get_stats(flow_idx);
    if (!stats)
        return 0;  /* 无统计 (兼容/未初始化): 不限流, 安全默认 */

    /* 上限取 max_active_per_conn/4. 服务端流控在连接被标记 SLOW 时
     * effective_per_conn = max/2 (=32 @ max=64), 且 conn_active >= 该值即
     * Reject(slow_conn). 客户端 active 与服务端 active 计数存在口径/时序差,
     * 同连接还混有 lease/元数据请求, 故取服务端最严阈值的一半 (16) 留足余量,
     * 保证任何负载 (含 slow) 下服务端 active 都到不了 reject 临界点.
     * 16 个 1MB 在途写对 RDMA 带宽已绰绰有余 (远未达网络/磁盘上限). */
    cap = g_flow.max_active_per_conn / 4;
    if (cap < 1)
        cap = 1;

    /* 快速路径 */
    if (atomic_read(&stats->active_reqs) < cap)
        return 0;

    atomic64_inc(&g_flow.global.queue_count);
    pr_debug_ratelimited("powerfs: flow wr-slot wait conn=%d active=%d cap=%d\n",
                        flow_idx, atomic_read(&stats->active_reqs), cap);

    ret = wait_event_timeout(g_flow.admit_wq,
        atomic_read(&stats->active_reqs) < cap,
        msecs_to_jiffies(timeout_ms));

    if (ret > 0 || atomic_read(&stats->active_reqs) < cap) {
        atomic64_inc(&g_flow.global.queue_wakeups);
        return 0;
    }

    atomic64_inc(&g_flow.global.reject_count);
    return -EBUSY;
}

int powerfs_flow_init(void)
{
    int i;

    memset(&g_flow, 0, sizeof(g_flow));

    g_flow.max_active_global = POWERFS_FLOW_MAX_ACTIVE_GLOBAL;
    g_flow.max_active_per_conn = POWERFS_FLOW_MAX_ACTIVE_PER_CONN;
    g_flow.slow_lat_thresh_ns = POWERFS_FLOW_SLOW_LAT_THRESH_NS;

    for (i = 0; i < POWERFS_FLOW_MAX_CONNS; i++) {
        atomic_set(&g_flow.conns[i].active_reqs, 0);
        atomic_set(&g_flow.conns[i].server_load_factor, 0);
        atomic_set(&g_flow.conns[i].slow, 0);
        atomic64_set(&g_flow.conns[i].total_reqs, 0);
        atomic64_set(&g_flow.conns[i].total_errs, 0);
        atomic64_set(&g_flow.conns[i].total_bytes, 0);
        atomic64_set(&g_flow.conns[i].total_lat_ns, 0);
        atomic64_set(&g_flow.conns[i].ewma_lat_ns, 0);
        atomic_long_set(&g_flow.conns[i].lf_update_jiffies, 0);
    }

    atomic_set(&g_flow.global.active_reqs, 0);
    atomic_set(&g_flow.global.active_conns, 0);
    atomic_set(&g_flow.global.slow_conns, 0);

    /* 数据路径排队等待队列 */
    init_waitqueue_head(&g_flow.admit_wq);

    /* debugfs: /sys/kernel/debug/powerfs_flow/ */
    g_flow.debugfs_root = debugfs_create_dir("powerfs_flow", NULL);
    if (IS_ERR_OR_NULL(g_flow.debugfs_root)) {
        pr_warn("powerfs: flow debugfs_create_dir failed\n");
        g_flow.debugfs_root = NULL;
    } else {
        flow_debugfs_init();
    }

    pr_info("powerfs: flow controller initialized "
            "(max_global=%u, max_per_conn=%u)\n",
            g_flow.max_active_global, g_flow.max_active_per_conn);

    return 0;
}

void powerfs_flow_exit(void)
{
    if (g_flow.debugfs_root) {
        debugfs_remove_recursive(g_flow.debugfs_root);
        g_flow.debugfs_root = NULL;
    }

    pr_info("powerfs: flow controller exited\n");
}

/*
 * powerfs_flow_admit - VFS 入口准入检查
 *
 * Phase 2: 根据 per-conn server_load_factor 自适应降低并发上限.
 *   lf=0 → 100%, lf=1 → 75%, lf=2 → 50%, lf=3 → 25%
 *
 * 满载 (lf=3) 且超限 → REJECT (调用方返回 -EBUSY)
 * 非满载且超限      → QUEUE  (调用方 usleep 后重试)
 * 未超限            → ADMIT
 */
enum powerfs_flow_decision powerfs_flow_admit(enum powerfs_flow_op op, int flow_idx)
{
    struct powerfs_flow_conn_stats *stats;
    int active, lf, max, global_active;

    stats = flow_get_stats(flow_idx);
    if (!stats)
        return POWERFS_FLOW_ADMIT;  /* 安全默认: 允许 */

    active = atomic_read(&stats->active_reqs);
    lf = atomic_read(&stats->server_load_factor);
    max = g_flow.max_active_per_conn;

    /* Phase 2: load_factor 自适应降速 */
    switch (lf) {
    case POWERFS_FLOW_LF_OVERLOAD:
        max = max / 4;         /* 25% */
        break;
    case POWERFS_FLOW_LF_BUSY:
        max = max / 2;         /* 50% */
        break;
    case POWERFS_FLOW_LF_NORMAL:
        max = (max * 3) / 4;   /* 75% */
        break;
    case POWERFS_FLOW_LF_IDLE:
    default:
        break;                 /* 100% */
    }

    if (max < 1)
        max = 1;  /* 至少允许 1 个 */

    /* 全局并发上限 */
    global_active = atomic_read(&g_flow.global.active_reqs);
    if (global_active >= (int)g_flow.max_active_global) {
        if (lf >= POWERFS_FLOW_LF_OVERLOAD)
            return POWERFS_FLOW_REJECT;
        return POWERFS_FLOW_QUEUE;
    }

    /* per-conn 并发上限 */
    if (active >= max) {
        if (lf >= POWERFS_FLOW_LF_OVERLOAD)
            return POWERFS_FLOW_REJECT;
        return POWERFS_FLOW_QUEUE;
    }

    return POWERFS_FLOW_ADMIT;
}

void powerfs_flow_record_start(int flow_idx, unsigned int est_bytes)
{
    struct powerfs_flow_conn_stats *stats;

    stats = flow_get_stats(flow_idx);
    if (!stats)
        return;

    atomic_inc(&stats->active_reqs);
    atomic_inc(&g_flow.global.active_reqs);

    if (est_bytes > 0)
        atomic64_add(est_bytes, &g_flow.global.total_bytes_sent);
}

void powerfs_flow_record_complete(int flow_idx, u64 lat_ns,
                                   unsigned int bytes, bool error)
{
    struct powerfs_flow_conn_stats *stats;
    u64 ewma;
    bool was_slow, is_slow;

    stats = flow_get_stats(flow_idx);
    if (!stats)
        return;

    /* 递减在途计数 */
    atomic_dec(&stats->active_reqs);
    atomic_dec(&g_flow.global.active_reqs);

    /* 唤醒数据路径 (admit_wait) 排队等待者 */
    wake_up(&g_flow.admit_wq);

    /* 累计统计 */
    atomic64_inc(&stats->total_reqs);
    atomic64_inc(&g_flow.global.total_reqs);
    atomic64_add(lat_ns, &stats->total_lat_ns);
    atomic64_add(bytes, &stats->total_bytes);
    atomic64_add(bytes, &g_flow.global.total_bytes_recv);

    if (error) {
        atomic64_inc(&stats->total_errs);
        atomic64_inc(&g_flow.global.total_errs);
    }

    /* EWMA 延迟更新 */
    ewma_update(&stats->ewma_lat_ns, lat_ns);

    /* 慢连接标记: EWMA 延迟超阈值 → slow=1, 否则 → slow=0 */
    ewma = atomic64_read(&stats->ewma_lat_ns);
    was_slow = atomic_read(&stats->slow) != 0;
    is_slow = ewma > g_flow.slow_lat_thresh_ns;

    if (was_slow != is_slow) {
        atomic_set(&stats->slow, is_slow ? 1 : 0);
        if (is_slow) {
            atomic_inc(&g_flow.global.slow_conns);
            pr_info("powerfs: flow conn[%d] marked SLOW "
                    "(ewma=%lluns, thresh=%luns)\n",
                    flow_idx, ewma, g_flow.slow_lat_thresh_ns);
        } else {
            atomic_dec(&g_flow.global.slow_conns);
            pr_info("powerfs: flow conn[%d] recovered from SLOW\n",
                    flow_idx);
        }
    }
}

void powerfs_flow_on_load_factor(int flow_idx, u8 lf)
{
    struct powerfs_flow_conn_stats *stats;
    u8 old;

    stats = flow_get_stats(flow_idx);
    if (!stats)
        return;

    lf = min_t(u8, lf, 3);  /* clamp 0-3 */

    old = (u8)atomic_read(&stats->server_load_factor);
    if (old != lf) {
        atomic_set(&stats->server_load_factor, lf);
        atomic_long_set(&stats->lf_update_jiffies, jiffies);

        /* 等级上升时打印 info, 帮助定位服务端过载 */
        if (lf > old && lf >= POWERFS_FLOW_LF_BUSY) {
            pr_info("powerfs: flow conn[%d] load_factor %u→%u\n",
                    flow_idx, old, lf);
        }
    }
}

int powerfs_flow_conn_active(int flow_idx)
{
    struct powerfs_flow_conn_stats *stats = flow_get_stats(flow_idx);
    return stats ? atomic_read(&stats->active_reqs) : 0;
}

u8 powerfs_flow_conn_load_factor(int flow_idx)
{
    struct powerfs_flow_conn_stats *stats = flow_get_stats(flow_idx);
    return stats ? (u8)atomic_read(&stats->server_load_factor) : 0;
}

/* ========== debugfs ========== */

static int flow_global_show(struct seq_file *m, void *v)
{
    seq_printf(m, "active_reqs      %d\n", atomic_read(&g_flow.global.active_reqs));
    seq_printf(m, "active_conns     %d\n", atomic_read(&g_flow.global.active_conns));
    seq_printf(m, "slow_conns       %d\n", atomic_read(&g_flow.global.slow_conns));
    seq_printf(m, "total_reqs       %lld\n", atomic64_read(&g_flow.global.total_reqs));
    seq_printf(m, "total_errs       %lld\n", atomic64_read(&g_flow.global.total_errs));
    seq_printf(m, "total_bytes_sent %lld\n", atomic64_read(&g_flow.global.total_bytes_sent));
    seq_printf(m, "total_bytes_recv %lld\n", atomic64_read(&g_flow.global.total_bytes_recv));
    seq_printf(m, "queue_count      %lld\n", atomic64_read(&g_flow.global.queue_count));
    seq_printf(m, "queue_wakeups    %lld\n", atomic64_read(&g_flow.global.queue_wakeups));
    seq_printf(m, "reject_count     %lld\n", atomic64_read(&g_flow.global.reject_count));
    seq_printf(m, "max_active_global %u\n", g_flow.max_active_global);
    seq_printf(m, "max_active_per_conn %u\n", g_flow.max_active_per_conn);
    seq_printf(m, "slow_lat_thresh_ns %lu\n", g_flow.slow_lat_thresh_ns);
    return 0;
}

static int flow_global_open(struct inode *inode, struct file *file)
{
    return single_open(file, flow_global_show, NULL);
}

static const struct file_operations flow_global_fops = {
    .open    = flow_global_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

static int flow_conns_show(struct seq_file *m, void *v)
{
    int i;

    seq_printf(m, "%-4s %-8s %-8s %-6s %-10s %-10s %-12s %-12s\n",
               "idx", "active", "lf", "slow", "total_reqs", "total_errs",
               "total_bytes", "ewma_lat_ms");

    for (i = 0; i < POWERFS_FLOW_MAX_CONNS; i++) {
        struct powerfs_flow_conn_stats *s = &g_flow.conns[i];
        int active = atomic_read(&s->active_reqs);

        /* 跳过从未使用的连接 */
        if (active == 0 && atomic64_read(&s->total_reqs) == 0)
            continue;

        seq_printf(m, "%-4d %-8d %-8u %-6d %-10lld %-10lld %-12lld %-12lld\n",
                   i,
                   active,
                   (u8)atomic_read(&s->server_load_factor),
                   atomic_read(&s->slow),
                   atomic64_read(&s->total_reqs),
                   atomic64_read(&s->total_errs),
                   atomic64_read(&s->total_bytes),
                   div_u64(atomic64_read(&s->ewma_lat_ns), 1000000));
    }
    return 0;
}

static int flow_conns_open(struct inode *inode, struct file *file)
{
    return single_open(file, flow_conns_show, NULL);
}

static const struct file_operations flow_conns_fops = {
    .open    = flow_conns_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* debugfs 写入接口: 调整 max_active_global (用于测试触发排队) */
static ssize_t flow_max_global_write(struct file *file, const char __user *buf,
                                      size_t count, loff_t *ppos)
{
    char kbuf[32];
    unsigned int val;
    size_t len = min(count, sizeof(kbuf) - 1);

    if (copy_from_user(kbuf, buf, len))
        return -EFAULT;
    kbuf[len] = '\0';

    if (kstrtouint(kbuf, 10, &val) || val == 0)
        return -EINVAL;

    g_flow.max_active_global = val;
    pr_info("powerfs: flow max_active_global set to %u\n", val);
    return count;
}

static int flow_max_global_show(struct seq_file *m, void *v)
{
    seq_printf(m, "%u\n", g_flow.max_active_global);
    return 0;
}

static int flow_max_global_open(struct inode *inode, struct file *file)
{
    return single_open(file, flow_max_global_show, NULL);
}

static const struct file_operations flow_max_global_fops = {
    .open    = flow_max_global_open,
    .read    = seq_read,
    .write   = flow_max_global_write,
    .llseek  = seq_lseek,
    .release = single_release,
};

/* 在 powerfs_flow_init 之后注册 debugfs 文件 */
static void flow_debugfs_init(void)
{
    struct dentry *root = g_flow.debugfs_root;
    if (!root)
        return;

    debugfs_create_file("global", 0444, root, NULL, &flow_global_fops);
    debugfs_create_file("connections", 0444, root, NULL, &flow_conns_fops);
    debugfs_create_file("max_active_global", 0644, root, NULL, &flow_max_global_fops);
}

/* ========== 模块加载钩子 (由 powerfs_mod.c 调用) ========== */

/* powerfs_flow_init 在 powerfs_mod.c 的 module_init 中调用.
 * debugfs 文件在 init 后注册. */
