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
#include <crypto/hash.h>

#include "powerfs.h"
#include "powerfs_write_predict.h"
#include "powerfs_net.h"

/* 全局 SHA-256 transform (init 时分配, 整个模块生命周期复用) */
static struct crypto_shash *sha256_tfm;

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
    u64 chunk_idx;       /* 对应 dedup_chunks 的索引, Match 时写回 */
    u64 offset;          /* needle 起始 offset, 记录用 */
    u32 crc32;           /* crc32_le 预计算, 避免 worker 再算 */
    u8 *data;            /* needle_buf 的深拷贝 */
    size_t data_len;
};

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

    if (!sha256_tfm)
        goto out;

    /* 1. 计算 SHA-256 — 只算一次, lookup + record 复用 */
    {
        struct shash_desc *desc;
        int ret;

        desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(sha256_tfm),
                       GFP_NOFS);
        if (!desc)
            goto out;

        desc->tfm = sha256_tfm;
        ret = crypto_shash_init(desc);
        if (!ret)
            ret = crypto_shash_update(desc, lw->data, lw->data_len);
        if (!ret)
            ret = crypto_shash_final(desc, fp);
        kfree(desc);

        if (ret) {
            pr_debug_ratelimited("powerfs: write_predict lookup SHA-256 failed ino=%lu: %d\n",
                                 inode->i_ino, ret);
            goto out;
        }
    }

    /* 2. 提取 prefix (与 filer FingerprintIndex 碰撞校验对齐) */
    memset(prefix, 0, sizeof(prefix));
    memcpy(prefix, lw->data, min_t(size_t, lw->data_len, sizeof(prefix)));

    /* 3. FingerprintLookup — 查 filer 是否已存在相同内容的 needle */
    {
        u8 body[POWERFS_FP_LOOKUP_BODY_SIZE];
        u8 resp[64];
        size_t resp_len = 0;
        int ret;
        u8 match_kind;

        memcpy(body, fp, POWERFS_FP_SIZE);
        put_unaligned_le64(data_size, body + 32);
        memcpy(body + 40, prefix, POWERFS_FP_PREFIX_SIZE);
        put_unaligned_le64(inode->i_ino, body + 104);
        put_unaligned_le64(lw->offset, body + 112);

        ret = powerfs_net_send_request(POWERFS_NET_MSG_FINGERPRINT_LOOKUP,
                                        inode->i_ino,
                                        body, sizeof(body),
                                        NULL, 0,
                                        resp, sizeof(resp),
                                        NULL, 0,
                                        500, &resp_len, NULL);
        if (ret < 0) {
            pr_debug_ratelimited("powerfs: write_predict lookup RPC failed ino=%lu: %d\n",
                                 inode->i_ino, ret);
            /* RPC 失败: 继续 record (幂等, 不影响正确性) */
            goto do_record;
        }
        if (resp_len == 0)
            goto do_record;  /* 空响应 → NoMatch, 直接 record */

        match_kind = resp[0];

        if (match_kind == POWERFS_FP_LOOKUP_MATCH ||
            match_kind == POWERFS_FP_LOOKUP_RECOVERABLE) {
            u64 matched_nid, matched_vid;

            if (resp_len < 17 + 8)
                goto do_record;  /* 响应过短 → 跳过 store_dedup */

            matched_nid = get_unaligned_le64(resp + 1);
            matched_vid = get_unaligned_le64(resp + 9);

            /* 4. Match → 缓存到 dedup_chunks, 下次覆盖写该 chunk 时跳过 volume 写 */
            powerfs_write_predict_store_dedup(inode, (u32)lw->chunk_idx,
                                               matched_nid, matched_vid);

            pr_info_ratelimited("powerfs: WRITE_PREDICT_DEDUP ino=%lu chunk=%llu %s needle=%#llx vol=%llu\n",
                                inode->i_ino, lw->chunk_idx,
                                match_kind == POWERFS_FP_LOOKUP_MATCH ? "MATCH" : "RECOVER",
                                (unsigned long long)matched_nid,
                                (unsigned long long)matched_vid);
        }
    }

do_record:
    /* 5. FingerprintRecord — 无论 Match/NoMatch, 把刚写的 needle 登记上
     * (best-effort, 500ms timeout). Match 场景下 filer 端幂等忽略重复登记. */
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
    struct powerfs_wp_lookup_work *lw;
    u8 *data_copy;

    if (!data || data_len == 0)
        return;
    if (!sha256_tfm)
        return;

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
    lw->needle_id = needle_id;
    lw->volume_id = volume_id;
    lw->chunk_idx = chunk_idx;
    lw->offset = offset;
    lw->crc32 = crc32_le(0, data, data_len);
    lw->data = data_copy;
    lw->data_len = data_len;
    INIT_WORK(&lw->work, powerfs_wp_lookup_worker);
    schedule_work(&lw->work);
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
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_inode_init);

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
 * powerfs_write_predict_store_dedup - 记录某 chunk 去重命中的 needle 引用.
 *
 * 由异步 lookup worker 在 Match/Recoverable 时调用. 持 i_lock 保护.
 * 按需扩展 dedup_chunks 数组 (kalloc + memcpy).
 *
 * 返回 0 成功, <0 失败 (调用方 best-effort, 忽略即可).
 */
int powerfs_write_predict_store_dedup(struct inode *inode, u32 chunk_idx,
                                      u64 matched_needle_id,
                                      u64 matched_volume_id)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_chunk_map *new_arr;
    u32 new_count;

    spin_lock(&pi->i_lock);
    if (chunk_idx >= pi->dedup_chunk_count) {
        new_count = chunk_idx + 1;
        new_arr = kmalloc_array(new_count, sizeof(*new_arr), GFP_ATOMIC);
        if (!new_arr) {
            spin_unlock(&pi->i_lock);
            return -ENOMEM;
        }
        if (pi->dedup_chunks) {
            memcpy(new_arr, pi->dedup_chunks,
                   pi->dedup_chunk_count * sizeof(*new_arr));
            kfree(pi->dedup_chunks);
        }
        memset(new_arr + pi->dedup_chunk_count, 0,
               (new_count - pi->dedup_chunk_count) * sizeof(*new_arr));
        pi->dedup_chunks = new_arr;
        pi->dedup_chunk_count = new_count;
    }
    pi->dedup_chunks[chunk_idx].chunk_idx = chunk_idx;
    pi->dedup_chunks[chunk_idx].needle_id = matched_needle_id;
    pi->dedup_chunks[chunk_idx].volume_id = matched_volume_id;
    pi->dedup_chunks[chunk_idx].size = POWERFS_CHUNK_SIZE;
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: write_predict store_dedup ino=%lu chunk=%u -> needle=%llu vol=%llu\n",
            inode->i_ino, chunk_idx,
            (unsigned long long)matched_needle_id,
            (unsigned long long)matched_volume_id);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_store_dedup);

/**
 * powerfs_write_predict_init - 模块 init 时初始化 SHA-256.
 *
 * 返回: 0 成功, <0 失败 (禁用去重功能, 不阻止模块加载)
 */
int powerfs_write_predict_init(void)
{
    sha256_tfm = crypto_alloc_shash("sha256", 0, 0);
    if (IS_ERR(sha256_tfm)) {
        int err = PTR_ERR(sha256_tfm);
        pr_warn("powerfs: SHA-256 crypto alloc failed (%d), write predict disabled\n",
                err);
        sha256_tfm = NULL;
        return err;
    }
    pr_info("powerfs: write predict initialized (SHA-256, block_size=%u)\n",
            crypto_shash_blocksize(sha256_tfm));
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_init);

/**
 * powerfs_write_predict_exit - 模块 exit 时释放 SHA-256.
 */
void powerfs_write_predict_exit(void)
{
    if (sha256_tfm) {
        crypto_free_shash(sha256_tfm);
        sha256_tfm = NULL;
    }
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_exit);
