// SPDX-License-Identifier: GPL-2.0
/* powerfs_write_predict.c - 写预测 + 内容指纹去重 (kernel 客户端)
 *
 * 详见 docs/write-prediction-dedup-design.md §C-1.5/C-1.6.
 *
 * Phase C-1.5 (本文件): kernel 端指纹计算 + dedup lookup/record
 *
 * ======= 方向②: 先写后问 (async, best-effort) =======
 *
 * 热路径 (writeback submit / DIO write) 完全零阻塞:
 *   - 正常异步提交 write_needle, 不做任何同步 SHA-256 / RPC
 *   - 仅 lockless 查 pi->dedup_chunks 表: 若异步 worker 已为该 chunk
 *     命中过 dedup (来自之前覆盖写的后台 lookup), 才跳过 volume 写
 *
 * 去重流水线 (全异步, workqueue 执行):
 *   write_cb (RDMA CQ 回调) / DIO write 成功后
 *     → memcpy needle_buf → schedule_work(lookup_record)
 *         → compute SHA-256(data)          // 只算一次
 *         → FingerprintLookup RPC (filer)
 *           Match → store_dedup(chunk_idx, matched_nid, matched_vid)
 *         → FingerprintRecord RPC (filer)  // 无论 Match/NoMatch
 *     → iput + kfree
 *
 * 收益:
 *   - 基线 (no dedup) 性能不变, 因为热路径没有任何额外 work
 *   - 首次写新数据: 同基线, 后台做一次 lookup+record 缓存指纹
 *   - 二次覆盖写相同内容: dedup_chunks 已命中 → 跳过 volume 写
 *   - NoMatch 场景: 零成本 (只后台一次 record, 下次可 dedup)
 *
 * 安全回退:
 *   - xattr 缺失/"off"/阈值≤0 → is_enabled=false → 异步 worker 不调度
 *   - crypto API 不可用 → init 失败, sha256_tfm=NULL → dedup 静默跳过
 *   - RPC 失败 → best-effort, 下次写会重试
 *
 * 编译开关: 本文件仅在 make WRITE_PREDICT=y (定义
 * CONFIG_POWERFS_WRITE_PREDICT) 时由 Makefile 编入模块; 缺省不编译,
 * hook 全部走 powerfs_write_predict.h 中的 static inline 空实现.
 */

#ifndef CONFIG_POWERFS_WRITE_PREDICT
#error "powerfs_write_predict.c requires CONFIG_POWERFS_WRITE_PREDICT (build with: make WRITE_PREDICT=y)"
#endif

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/printk.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/crc32.h>
#include <linux/xxhash.h>
#include <crypto/hash.h>

#include "powerfs.h"
#include "powerfs_write_predict.h"
#include "powerfs_net.h"

/* 全局 SHA-256 transform (init 时分配, 整个模块生命周期复用;
 * shash transform 无状态, 可被多 CPU 并发使用, 各自持独立 shash_desc) */
static struct crypto_shash *sha256_tfm;

/* 指纹 worker 专用有界 workqueue: 不再使用系统 events 队列.
 * WQ_UNBOUND: 不钉死 CPU, 避免饥饿; max_active=4: 全局限并发, RPC 串行
 * (~500ms timeout) 时最多 4 个在执行, 其余在队列内. 入队另有全局/per-inode
 * 水位 (WP_INFLIGHT_MAX / WP_PER_INODE_MAX) 兜底, 队列不会无限增长. */
static struct workqueue_struct *wp_wq;

/* 在途 work (含排队 + 运行) 全局水位: 超出直接丢弃新 work (best-effort) */
#define WP_INFLIGHT_MAX      256
/* 单 inode 在途 work 水位 (32 chunks 已可覆盖典型连续写的窗口) */
#define WP_PER_INODE_MAX     32

static atomic_t wp_inflight = ATOMIC_INIT(0);

/* FingerprintRecord 抽样率 (1/N). 本地去重不依赖 Record; 抽样只为持续
 * 维护 Filer 侧跨客户端指纹索引, 同时避免热路径每个 needle 都付出
 * 1MB 拷贝 + SHA-256 + RPC 的代价. */
#define WP_RECORD_SAMPLE    8
static atomic_t wp_record_seq = ATOMIC_INIT(0);

/* 同步计算 SHA-256. 调用方须处于 process context (desc 用 GFP_NOFS).
 * 返回 0 成功. */
static int wp_compute_sha256(const u8 *data, size_t data_len, u8 out[POWERFS_FP_SIZE])
{
    struct shash_desc *desc;
    int ret;

    if (!sha256_tfm)
        return -ENODEV;

    desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(sha256_tfm),
                   GFP_NOFS);
    if (!desc)
        return -ENOMEM;

    desc->tfm = sha256_tfm;
    ret = crypto_shash_init(desc);
    if (!ret)
        ret = crypto_shash_update(desc, data, data_len);
    if (!ret)
        ret = crypto_shash_final(desc, out);
    kfree(desc);
    return ret;
}

/* ================================================================
 * === 异步 lookup + record worker (方向②核心) ===
 *
 * 合并 FingerprintLookup + FingerprintRecord 到同一个 workqueue worker,
 * 复用一次 SHA-256 计算. 从 write_cb (RDMA CQ 回调) 或 DIO write 成功
 * 后调度 — 这两个上下文都不能做同步 RPC, 必须异步.
 *
 * 生命周期: write_cb / DIO 成功 → igrab + memcpy → kzalloc work →
 *   schedule_work → worker 在 process context 执行 lookup + record →
 *   iput + kfree → done.
 * ================================================================ */

struct powerfs_wp_lookup_work {
    struct work_struct work;
    struct inode *inode;
    u64 needle_id;       /* 刚写的 needle_id */
    u64 volume_id;       /* 刚写的 volume_id */
    u64 chunk_idx;       /* chunk 索引, per-chunk 单飞去重键 */
    u64 offset;          /* needle 起始 offset, 记录用 */
    u32 crc32;           /* crc32_le 预计算, 避免 worker 再算 */
    u8 *data;            /* needle_buf 的深拷贝 */
    size_t data_len;
};

/* 锁内确保 chunk 条目存在 (数组扩容). 只能用 GFP_ATOMIC (持自旋锁).
 * 返回条目指针 (已持锁), 失败返回 NULL. */
static struct powerfs_chunk_map *wp_entry_ensure_locked(
    struct powerfs_inode_info *pi, u32 chunk_idx, gfp_t gfp)
{
    if (chunk_idx < pi->dedup_chunk_count)
        return &pi->dedup_chunks[chunk_idx];

    {
        u32 old_count = pi->dedup_chunk_count;
        u32 new_count = chunk_idx + 1;
        struct powerfs_chunk_map *new_arr, *old_arr;

        if (new_count > 4096)
            return NULL;
        new_arr = kzalloc(array_size(new_count, sizeof(*new_arr)), gfp);
        if (!new_arr)
            return NULL;
        old_arr = pi->dedup_chunks;
        if (old_arr)
            memcpy(new_arr, old_arr, old_count * sizeof(*new_arr));
        pi->dedup_chunks = new_arr;
        pi->dedup_chunk_count = new_count;
        kfree(old_arr);
        return &new_arr[chunk_idx];
    }
}

static void powerfs_wp_lookup_worker(struct work_struct *work)
{
    struct powerfs_wp_lookup_work *lw =
        container_of(work, struct powerfs_wp_lookup_work, work);
    struct inode *inode = lw->inode;
    u64 shard_id = powerfs_calc_shard_id(inode->i_ino);
    u64 data_size = (u64)lw->data_len;
    u8 fp[POWERFS_FP_SIZE];
    u8 prefix[POWERFS_FP_PREFIX_SIZE];

    if (!lw->data || lw->data_len == 0)
        goto out;

    /* 1. 计算 SHA-256 — 只算一次, lookup + record + 本地缓存复用 */
    if (wp_compute_sha256(lw->data, lw->data_len, fp)) {
        pr_debug_ratelimited("powerfs: write_predict SHA-256 failed ino=%lu\n",
                             inode->i_ino);
        goto out;
    }

    /* 2. 提取 prefix (与 filer FingerprintIndex 碰撞校验对齐) */
    memset(prefix, 0, sizeof(prefix));
    memcpy(prefix, lw->data, min_t(size_t, lw->data_len, sizeof(prefix)));

    /* 本地指纹缓存已由 check_hit + powerfs_write_predict_commit 在写
     * 提交/CQ 路径上零拷贝完成, worker 不再回写 dedup 表. */

    /* 3. 不发起 FingerprintLookup:
     * 跨文件 Match 的外部 needle 不能被本客户端复用 (位置固定槽位会被
     * 属主原地覆写, 无 COW 独立 needle 分配通道), 热路径跳写只依赖本地
     * 指纹缓存 (步骤 2.5), Lookup 结果无人消费, 纯属每 chunk 一次的额外
     * RTT (fio 随机覆写场景吞吐被腰斩的主因之一). 跨文件语义仍由
     * FingerprintRecord 维护, 供 FUSE 等其它客户端使用. */

    /* 4. FingerprintRecord — 把刚写的自有 needle 登记到 filer 指纹索引
     * (best-effort, 500ms timeout), 供其它客户端做跨文件去重查询. */
    {
        u8 body[POWERFS_FP_RECORD_BODY_SIZE];
        u8 resp[16];
        size_t resp_len = 0;
        int ret;

        memcpy(body, fp, POWERFS_FP_SIZE);
        put_unaligned_le64(lw->needle_id, body + 32);
        put_unaligned_le64(lw->volume_id, body + 40);
        put_unaligned_le32(lw->crc32, body + 48);
        put_unaligned_le64(data_size, body + 52);
        memcpy(body + 60, prefix, POWERFS_FP_PREFIX_SIZE);

        ret = powerfs_net_send_request(POWERFS_NET_MSG_FINGERPRINT_RECORD,
                                        inode->i_ino,
                                        body, sizeof(body),
                                        NULL, 0,
                                        resp, sizeof(resp),
                                        NULL, 0,
                                        500, &resp_len, NULL);
        if (ret < 0)
            pr_debug_ratelimited("powerfs: write_predict record RPC failed ino=%lu: %d\n",
                                 inode->i_ino, ret);
    }

out:
    /* 摘除 per-chunk 单飞标记并释放水位 (无论中途是否提前退出) */
    {
        struct powerfs_inode_info *pi = POWERFS_I(inode);

        spin_lock(&pi->wp_lock);
        if (xa_erase(&pi->wp_pending, lw->chunk_idx) == lw)
            pi->wp_pending_cnt--;
        spin_unlock(&pi->wp_lock);
        atomic_dec(&wp_inflight);
    }
    iput(inode);
    kfree(lw->data);
    kfree(lw);
}

/**
 * powerfs_write_predict_lookup_record_async - 异步 lookup + record (方向②核心入口).
 *
 * 在 write_cb (RDMA CQ 回调) 或 DIO write 成功后调用. context 不能做同步 RPC,
 * 所以这里拷贝数据 + igrab inode + schedule_work, 真正的 SHA-256 + RPC 在 worker
 * 线程中异步执行. 一次 SHA-256 同时服务 lookup 和 record, 避免重复计算.
 *
 * 触发条件: powerfs_write_predict_is_enabled(pi) == true (xattr 已启用).
 * 若未启用, 调用方不应调此函数.
 *
 * @inode: 文件 inode (内部 igrab, worker 结束后 iput)
 * @needle_id: 刚写的 needle_id
 * @volume_id: 刚写的 volume_id
 * @chunk_idx: 对应 dedup_chunks 的索引 (offset / POWERFS_CHUNK_SIZE)
 * @offset: needle 起始 offset, 记录用
 * @data: 刚写的数据 (内部 memcpy 深拷贝, 调用方后续释放不影响)
 * @data_len: 数据长度 (通常 = POWERFS_CHUNK_SIZE, 即 needle 整体覆盖)
 */
void powerfs_write_predict_lookup_record_async(struct inode *inode,
                                                u64 needle_id, u64 volume_id,
                                                u64 chunk_idx, loff_t offset,
                                                const u8 *data, size_t data_len)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_wp_lookup_work *lw;
    u8 *data_copy;
    bool admitted = false;

    if (!data || data_len == 0)
        return;
    if (!sha256_tfm || !wp_wq)
        return;

    /* 抽样登记 Filer 指纹索引. 本地去重跳写不依赖 Record (见 check_hit/
     * commit); Record 只服务 FUSE 等客户端的跨文件查询. 对每个 needle 做
     * 1MB 深拷贝 + SHA-256 + RPC 在大块随机覆写热路径上代价显著, 全局限
     * 速到 1/WP_RECORD_SAMPLE: 既持续喂索引, 又把数据面开销压低一个
     * 数量级. 单 chunk 同一时刻最多一个 worker (xarray 单飞). */
    if (atomic_inc_return(&wp_record_seq) % WP_RECORD_SAMPLE != 1)
        return;

    /* 快速水位预检 (持锁仅做判断, 不做分配). */
    spin_lock(&pi->wp_lock);
    if (pi->wp_pending_cnt >= WP_PER_INODE_MAX ||
        atomic_read(&wp_inflight) >= WP_INFLIGHT_MAX) {
        spin_unlock(&pi->wp_lock);
        pr_info_ratelimited("powerfs: write_predict work dropped ino=%lu chunk=%llu (pending=%u inflight=%d)\n",
                            inode->i_ino, chunk_idx, pi->wp_pending_cnt,
                            atomic_read(&wp_inflight));
        return;
    }
    spin_unlock(&pi->wp_lock);

    /* 调用方可能在 RDMA CQ 回调 (原子上下文), 一律 GFP_ATOMIC.
     * 1MB 深拷贝失败即丢, 不产生背压传导. */
    lw = kzalloc(sizeof(*lw), GFP_ATOMIC);
    if (!lw)
        return;

    data_copy = kmalloc(data_len, GFP_ATOMIC);
    if (!data_copy) {
        kfree(lw);
        return;
    }
    memcpy(data_copy, data, data_len);

    lw->inode = igrab(inode);
    if (!lw->inode) {
        kfree(data_copy);
        kfree(lw);
        return;
    }
    lw->needle_id = needle_id;
    lw->volume_id = volume_id;
    lw->chunk_idx = chunk_idx;
    lw->offset = offset;
    lw->crc32 = crc32_le(0, data, data_len);
    lw->data = data_copy;
    lw->data_len = data_len;
    INIT_WORK(&lw->work, powerfs_wp_lookup_worker);

    /* 二次确认水位 + per-chunk 单飞 */
    spin_lock(&pi->wp_lock);
    if (pi->wp_pending_cnt < WP_PER_INODE_MAX &&
        atomic_read(&wp_inflight) < WP_INFLIGHT_MAX &&
        xa_insert(&pi->wp_pending, chunk_idx, lw, GFP_ATOMIC) == 0) {
        pi->wp_pending_cnt++;
        atomic_inc(&wp_inflight);
        admitted = true;
    }
    spin_unlock(&pi->wp_lock);

    if (!admitted) {
        iput(inode);
        kfree(data_copy);
        kfree(lw);
        return;
    }

    queue_work(wp_wq, &lw->work);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_lookup_record_async);

/* ================================================================
 * === API 实现 (方向②保留的函数) ===
 * ================================================================ */

/* xattr value 最大长度 (NN:0.75 / RULE:16 等都 <32 字节) */
#define POLICY_XATTR_MAX_LEN 64

/**
 * powerfs_write_predict_inode_init - inode 分配时初始化字段.
 */
void powerfs_write_predict_inode_init(struct powerfs_inode_info *pi)
{
    pi->write_predict_enabled = false;
    pi->write_predict_cached = false;
    pi->write_predict_querying = false;
    pi->dedup_chunks = NULL;
    pi->dedup_chunk_count = 0;
    pi->wp_pending_cnt = 0;
    pi->wp_map_dirty = false;
    xa_init(&pi->wp_pending);
    spin_lock_init(&pi->wp_lock);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_inode_init);

/**
 * powerfs_write_predict_inode_destroy - evict/free_inode 清理.
 *
 * 在途 worker 都持有 inode 引用, evict 能到达说明 wp_pending 必为空;
 * 若 WARN 触发说明引用计数有误, 仍安全释放 (worker 侧 wp_lock 串行,
 * 最坏丢一次缓存更新, 不涉及 UAF: worker 持有独立 lw).
 */
void powerfs_write_predict_inode_destroy(struct powerfs_inode_info *pi)
{
    spin_lock(&pi->wp_lock);
    if (pi->wp_pending_cnt)
        pr_warn("powerfs: write_predict destroy ino with %u pending works\n",
                pi->wp_pending_cnt);
    kfree(pi->dedup_chunks);
    pi->dedup_chunks = NULL;
    pi->dedup_chunk_count = 0;
    pi->wp_pending_cnt = 0;
    spin_unlock(&pi->wp_lock);
    xa_destroy(&pi->wp_pending);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_inode_destroy);

/**
 * powerfs_write_predict_check_hit - 热路径跳写判定 + provisional 预植.
 *
 * 只回答一个问题: "本 chunk 的自身 needle 槽位, 上一轮已提交的字节与
 * 当前待写字节是否完全相同?" 相同 (xxh64 一致 + fp_valid + 自身槽位)
 * 返回 true, 调用方跳过物理写.
 *
 * 返回 false 时 (无条目 / 内容变化 / 仅有 provisional / 外部引用), 已在
 * 锁内把条目更新为 provisional: fp_valid=0, fp_xxh=当前数据指纹,
 * nid/vid=自身槽位, wp_gen+1, 并通过 out_* 输出供调用方在写成功后调
 * powerfs_write_predict_commit(). 写失败则条目永不 validate, 下轮照写.
 *
 * 这是陈旧 Match 数据错乱 bug 的根因修复:
 *   - 旧实现只看 needle_id != 0 就复用引用, 同 chunk A→B 换写会复用 A;
 *   - 跨文件复用位置固定、会被属主原地覆写的 needle 本质不安全, 禁用.
 * 本地指纹用 xxh64 (非加密, ~15GB/s); 64bit 碰撞概率可忽略.
 */
bool powerfs_write_predict_check_hit(struct inode *inode, u32 chunk_idx,
                                     const u8 *data, size_t data_len,
                                     u64 *out_needle_id,
                                     u64 *out_volume_id,
                                     u64 *out_xxh, u64 *out_gen)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    u64 own_nid, own_vid;
    u64 actual;
    bool hit = false;

    if (!powerfs_write_predict_is_enabled(pi) || !data || data_len == 0)
        return false;

    own_nid = pi->file_key + chunk_idx;
    own_vid = pi->volume_id;
    if (!own_nid || !own_vid)
        return false;  /* 尚未分配 Flat/Stripe 槽位 (inline 期), 不跟踪 */

    /* 锁外算当前数据指纹 (xxh64 ~15GB/s, 1MB 约 70us) */
    actual = xxh64(data, data_len, 0);

    spin_lock(&pi->wp_lock);
    if (pi->dedup_chunks && chunk_idx < pi->dedup_chunk_count) {
        struct powerfs_chunk_map *e = &pi->dedup_chunks[chunk_idx];

        if (e->fp_valid && e->needle_id != 0 &&
            e->fp_xxh == actual &&
            e->needle_id == own_nid && e->volume_id == own_vid) {
            hit = true;
        }
    }

    if (!hit) {
        /* 预植/刷新 provisional 条目供写成功后 commit */
        struct powerfs_chunk_map *e =
            wp_entry_ensure_locked(pi, chunk_idx, GFP_ATOMIC);

        if (e) {
            /* 内容指纹与已提交条目相同却仍未命中: 只可能是外部引用;
             * 无论如何本次写自身槽位, 推进代数. 内容指纹变化同样推进. */
            e->wp_gen++;
            e->chunk_idx = chunk_idx;
            e->needle_id = own_nid;
            e->volume_id = own_vid;
            e->size = POWERFS_CHUNK_SIZE;
            e->fp_xxh = actual;
            e->fp_valid = 0;

            if (out_needle_id)
                *out_needle_id = own_nid;
            if (out_volume_id)
                *out_volume_id = own_vid;
            if (out_xxh)
                *out_xxh = actual;
            if (out_gen)
                *out_gen = e->wp_gen;
        }
    } else {
        if (out_needle_id)
            *out_needle_id = own_nid;
        if (out_volume_id)
            *out_volume_id = own_vid;
    }
    spin_unlock(&pi->wp_lock);

    return hit;
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_check_hit);

/**
 * powerfs_write_predict_commit - 物理写成功后提交 provisional 条目.
 *
 * CQ 回调可直接调用: 一次自旋锁 + 标量比较, 无内存拷贝/RPC/哈希.
 * 代数不匹配 (写期间该 chunk 又被更新一代的写覆盖) 则静默丢弃.
 */
void powerfs_write_predict_commit(struct inode *inode, u32 chunk_idx,
                                  u64 needle_id, u64 volume_id,
                                  u64 fp_xxh, u64 gen)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    if (needle_id == 0)
        return;

    spin_lock(&pi->wp_lock);
    if (pi->dedup_chunks && chunk_idx < pi->dedup_chunk_count) {
        struct powerfs_chunk_map *e = &pi->dedup_chunks[chunk_idx];

        if (e->wp_gen == gen && !e->fp_valid &&
            e->needle_id == needle_id && e->volume_id == volume_id &&
            e->fp_xxh == fp_xxh) {
            /* 发布内容指纹后再置 valid (热路径在同一把 wp_lock 下读) */
            smp_wmb();
            e->fp_valid = 1;
        }
    }
    spin_unlock(&pi->wp_lock);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_commit);

bool powerfs_write_predict_map_dirty(struct powerfs_inode_info *pi)
{
    bool dirty;

    spin_lock(&pi->wp_lock);
    dirty = pi->wp_map_dirty;
    spin_unlock(&pi->wp_lock);
    return dirty;
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_map_dirty);

void powerfs_write_predict_map_dirty_clear(struct powerfs_inode_info *pi)
{
    spin_lock(&pi->wp_lock);
    pi->wp_map_dirty = false;
    spin_unlock(&pi->wp_lock);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_map_dirty_clear);

/**
 * powerfs_write_predict_invalidate - 标记 per-inode 策略缓存失效.
 */
void powerfs_write_predict_invalidate(struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    WRITE_ONCE(pi->write_predict_cached, false);
    WRITE_ONCE(pi->write_predict_querying, false);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_invalidate);

/**
 * parse_policy_threshold - 解析 xattr value → 是否启用.
 *
 * 格式:
 *   "off"           → false (禁用)
 *   "NN:<float>"    → float > 0.0 → true
 *   "RULE:<float>"  → float > 0.0 → true
 *   "<float>"       → float > 0.0 → true (无前缀, 兼容)
 *   缺失/解析失败   → false (安全回退)
 *
 * 返回: true=启用, false=跳过
 */
static bool parse_policy_threshold(const u8 *value, size_t value_len)
{
    char buf[POLICY_XATTR_MAX_LEN + 1];
    const char *s;
    size_t len;

    if (!value || value_len == 0)
        return false;

    len = min_t(size_t, value_len, POLICY_XATTR_MAX_LEN);
    memcpy(buf, value, len);
    buf[len] = '\0';
    s = buf;

    /* trim leading spaces */
    while (*s == ' ' || *s == '\t')
        s++;

    /* "off" (case-insensitive) */
    if (strncasecmp(s, "off", 3) == 0)
        return false;

    /* skip NN: / RULE: prefix */
    if (strncasecmp(s, "NN:", 3) == 0)
        s += 3;
    else if (strncasecmp(s, "RULE:", 5) == 0)
        s += 5;

    /* parse float threshold: any value > 0 → enable */
    {
        int int_part = 0;
        int frac_part = 0;
        int divisor = 1;
        int sign = 1;
        bool has_digits = false;

        if (*s == '-') {
            sign = -1;
            s++;
        } else if (*s == '+') {
            s++;
        }

        while (*s >= '0' && *s <= '9') {
            int_part = int_part * 10 + (*s - '0');
            has_digits = true;
            s++;
        }

        if (*s == '.') {
            s++;
            while (*s >= '0' && *s <= '9') {
                frac_part = frac_part * 10 + (*s - '0');
                divisor *= 10;
                has_digits = true;
                s++;
            }
        }

        if (!has_digits)
            return false;

        if (sign < 0)
            return false;

        return (int_part > 0 || frac_part > 0);
    }
}

/**
 * powerfs_write_predict_should_dedup - 检查 inode 是否启用写预测去重.
 *
 * 缓存策略: 首次调用查 xattr RPC, 结果缓存在 pi->write_predict_enabled.
 * 后续直接读缓存. PushDelta 失效后重新查.
 *
 * 惊群防护: writeback_wq (WQ_UNBOUND, max_active=32) 可并发提交多个
 * batch, 首次写时 32 个 batch 同时发现 cached=false 会发起 32 次相同
 * xattr RPC. 用 write_predict_querying 标志确保仅一个线程执行 slow path,
 * 其余线程在 fast path 重检时命中缓存.
 */
bool powerfs_write_predict_should_dedup(struct inode *inode,
                                          struct dentry *dentry)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    /* fast path: lockless read of cached result */
    if (READ_ONCE(pi->write_predict_cached))
        return READ_ONCE(pi->write_predict_enabled);

    /* thundering herd guard: only one thread does the slow path */
    if (xchg(&pi->write_predict_querying, true)) {
        /* another thread is querying; wait for result via re-read loop */
        int i;
        for (i = 0; i < 1000; i++) {
            if (READ_ONCE(pi->write_predict_cached))
                return READ_ONCE(pi->write_predict_enabled);
            cpu_relax();
            udelay(1);
        }
        /* timeout: fall through and do our own query (rare) */
    }

    /* slow path: query xattr from filer */
    {
        u64 shard_id = powerfs_calc_shard_id(inode->i_ino);
        u8 xattr_val[POLICY_XATTR_MAX_LEN];
        size_t xattr_len = 0;
        int ret;
        bool enabled;

        ret = powerfs_net_getxattr(shard_id, inode->i_ino,
                                    POWERFS_WRITE_PREDICT_XATTR_NAME,
                                    strlen(POWERFS_WRITE_PREDICT_XATTR_NAME),
                                    xattr_val, sizeof(xattr_val),
                                    &xattr_len);
        if (ret == -ENODATA || ret < 0) {
            /* xattr not set on file → try parent directory inheritance. */
            struct dentry *parent = dentry;
            int depth;

            enabled = false;
            for (depth = 0; depth < 10 && parent; depth++) {
                struct dentry *p = parent->d_parent;

                if (p == parent || p == NULL)
                    break;  /* root */

                {
                    struct inode *p_inode = d_inode(p);

                    if (p_inode && p_inode != inode &&
                        p_inode->i_sb == inode->i_sb) {
                        u64 p_shard = powerfs_calc_shard_id(p_inode->i_ino);
                        int p_ret;

                        p_ret = powerfs_net_getxattr(p_shard, p_inode->i_ino,
                                                     POWERFS_WRITE_PREDICT_XATTR_NAME,
                                                     strlen(POWERFS_WRITE_PREDICT_XATTR_NAME),
                                                     xattr_val, sizeof(xattr_val),
                                                     &xattr_len);
                        if (p_ret >= 0 && xattr_len > 0) {
                            enabled = parse_policy_threshold(xattr_val, xattr_len);
                            break;
                        }
                    }
                }
                parent = p;
            }
        } else {
            enabled = parse_policy_threshold(xattr_val, xattr_len);
        }

        /* cache result */
        WRITE_ONCE(pi->write_predict_enabled, enabled);
        smp_wmb();
        WRITE_ONCE(pi->write_predict_cached, true);
        WRITE_ONCE(pi->write_predict_querying, false);

        pr_debug_ratelimited("powerfs: write_predict ino=%lu enabled=%d (xattr_len=%zu)\n",
                             inode->i_ino, enabled, xattr_len);
        return enabled;
    }
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_should_dedup);

/**
 * powerfs_write_predict_is_enabled - 快速检查 (仅读缓存, 无需 dentry).
 *
 * 写回热路径专用: 无锁读 write_predict_cached && write_predict_enabled,
 * 不查 xattr, 不查 dentry. 缓存由首次 should_dedup 调用预热.
 * 关闭时直接返回 false, 调用方跳过 dedup 相关的所有 work.
 */
bool powerfs_write_predict_is_enabled(struct powerfs_inode_info *pi)
{
    return READ_ONCE(pi->write_predict_cached) &&
           READ_ONCE(pi->write_predict_enabled);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_is_enabled);

/**
 * powerfs_write_predict_init - 模块 init 时初始化 SHA-256.
 *
 * 返回: 0 成功, <0 失败 (禁用去重功能, 不阻止模块加载)
 */
int powerfs_write_predict_init(void)
{
    /* WQ_MEM_RECLAIM: 内存回收期间保证有 rescuer 可推进, 避免与回写互锁 */
    wp_wq = alloc_workqueue("powerfs_fprintk",
                            WQ_UNBOUND | WQ_MEM_RECLAIM, 4);
    if (!wp_wq) {
        pr_warn("powerfs: fingerprint workqueue alloc failed, write predict disabled\n");
        return -ENOMEM;
    }

    sha256_tfm = crypto_alloc_shash("sha256", 0, 0);
    if (IS_ERR(sha256_tfm)) {
        int err = PTR_ERR(sha256_tfm);
        pr_warn("powerfs: SHA-256 crypto alloc failed (%d), write predict disabled\n",
                err);
        sha256_tfm = NULL;
        destroy_workqueue(wp_wq);
        wp_wq = NULL;
        return err;
    }
    pr_info("powerfs: write predict initialized (SHA-256, block_size=%u)\n",
            crypto_shash_blocksize(sha256_tfm));
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_init);

/**
 * powerfs_write_predict_exit - 模块 exit: 先排空 worker 再释放资源.
 *
 * destroy_workqueue 内部先 flush: 保证所有 lookup/record RPC 完成、
 * iput 已执行后才返回, 此后再释放 SHA-256 transform.
 */
void powerfs_write_predict_exit(void)
{
    if (wp_wq) {
        destroy_workqueue(wp_wq);
        wp_wq = NULL;
    }
    if (sha256_tfm) {
        crypto_free_shash(sha256_tfm);
        sha256_tfm = NULL;
    }
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_exit);
