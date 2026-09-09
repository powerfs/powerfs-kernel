// SPDX-License-Identifier: GPL-2.0
/* powerfs_write_predict.c - 写预测 + 内容指纹去重 (kernel 客户端)
 *
 * 详见 docs/write-prediction-dedup-design.md §C-1.5/C-1.6.
 *
 * Phase C-1.5 (本文件): kernel 端指纹计算 + dedup lookup/record
 *
 * 流水线:
 *   writeback path → should_dedup? → dedup(data) → Match? → skip write
 *                                                   → NoMatch → write + record
 *
 * 指纹: SHA-256 (kernel crypto API)
 *   - CONFIG_CRYPTO_SHA256=y 在所有内核中可用
 *   - 与 Rust sha2 crate 完全对齐 (同算法, 同 32 字节输出)
 *   - 性能 ~500 MB/s (software), 256KB needle ~0.5ms (vs 网络 ~1-10ms)
 *
 * 安全回退 (C-1.6):
 *   - xattr 缺失/"off"/阈值≤0 → 不算指纹, 正常写
 *   - crypto API 不可用 → 正常写 (module init 检测, 全局禁用)
 *   - RPC 失败 → 正常写 (best-effort)
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

/* record 异步 work: write_cb (RDMA CQ 回调上下文) 不能做同步 RPC,
 * 把 FingerprintRecord 提交到系统 workqueue 异步执行. */
struct powerfs_wp_record_work {
    struct work_struct work;
    struct inode *inode;
    u64 needle_id;
    u64 volume_id;
    u32 crc32;
    u8 *data;
    size_t data_len;
};

static void powerfs_wp_record_worker(struct work_struct *work)
{
    struct powerfs_wp_record_work *rw =
        container_of(work, struct powerfs_wp_record_work, work);

    powerfs_write_predict_record(rw->inode, rw->needle_id, rw->volume_id,
                                  rw->crc32, rw->data, rw->data_len);

    iput(rw->inode);
    kfree(rw->data);
    kfree(rw);
}

/**
 * powerfs_write_predict_record_async - 异步记录指纹 (write_cb 安全版本).
 *
 * 在 RDMA CQ 回调 (write_cb) 中调用: 拷贝数据, 提交到系统 workqueue,
 * 由 worker 线程执行同步 FingerprintRecord RPC, 不阻塞 CQ 处理.
 */
void powerfs_write_predict_record_async(struct inode *inode,
                                        u64 needle_id, u64 volume_id,
                                        const u8 *data, size_t data_len)
{
    struct powerfs_wp_record_work *rw;
    u8 *data_copy;

    if (!data || data_len == 0)
        return;

    rw = kzalloc(sizeof(*rw), GFP_ATOMIC);
    if (!rw)
        return;
    data_copy = kmalloc(data_len, GFP_ATOMIC);
    if (!data_copy) {
        kfree(rw);
        return;
    }
    memcpy(data_copy, data, data_len);

    rw->inode = igrab(inode);
    rw->needle_id = needle_id;
    rw->volume_id = volume_id;
    rw->crc32 = crc32_le(0, data, data_len);
    rw->data = data_copy;
    rw->data_len = data_len;
    INIT_WORK(&rw->work, powerfs_wp_record_worker);
    schedule_work(&rw->work);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_record_async);

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

    /* lockless write: invalidate cache, force re-query on next access */
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

        /* threshold = sign * (int_part + frac_part/divisor) > 0 */
        if (sign < 0)
            return false; /* negative threshold → disable */

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
            /* xattr not set on file → try parent directory inheritance.
             * 目录级继承: 在父目录上设一次 xattr, 所有子文件自动继承.
             * 递归向上查 (最多 10 层, 防止循环). */
            struct dentry *parent = dentry;
            int depth;

            enabled = false;
            for (depth = 0; depth < 10 && parent; depth++) {
                struct dentry *p = parent->d_parent;

                if (p == parent || p == NULL)
                    break;  /* root */

                {
                    struct inode *p_inode = d_inode(p);

                    /* 仅查 powerfs 同 sb 的 inode, 避免越界到 VFS root
                     * (非 powerfs inode) 发起无效 xattr RPC. */
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

        /* cache result — write enabled before cached so lockless
         * readers never see cached=true with stale enabled. */
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
 * 写回热路径专用: 持 i_lock 读 write_predict_cached && write_predict_enabled,
 * 不查 xattr, 不查 dentry. 缓存由 write_iter 中 should_dedup 预热.
 */
bool powerfs_write_predict_is_enabled(struct powerfs_inode_info *pi)
{
    /* 无锁读: bool 读写在所有架构上原子. write_cb (RDMA CQ 回调)
     * 不能持 pi->i_lock, 否则与 writepage_work_fn 的 locate_chunk
     * 竞争导致 CQ 处理阻塞、性能暴跌. */
    return READ_ONCE(pi->write_predict_cached) &&
           READ_ONCE(pi->write_predict_enabled);
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_is_enabled);

/**
 * compute_sha256 - 计算 SHA-256 指纹.
 *
 * 用全局 sha256_tfm, 每次分配一个 shash_desc 在栈/堆上计算.
 * 返回: 0 成功 (hash_out 写入 32 字节), <0 失败
 */
static int compute_sha256(const u8 *data, size_t data_len, u8 *hash_out)
{
    struct shash_desc *desc;
    int ret;

    if (!sha256_tfm)
        return -ENODEV;

    /* shash_desc 大小 = crypto_shash_descsize(tfm) + sizeof(struct shash_desc) */
    desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(sha256_tfm),
                   GFP_NOFS);
    if (!desc)
        return -ENOMEM;

    desc->tfm = sha256_tfm;

    ret = crypto_shash_init(desc);
    if (ret)
        goto out;

    ret = crypto_shash_update(desc, data, data_len);
    if (ret)
        goto out;

    ret = crypto_shash_final(desc, hash_out);

out:
    kfree(desc);
    return ret;
}

/**
 * powerfs_write_predict_dedup - 对写数据执行指纹去重.
 *
 * 返回: 1=去重成功, 0=未匹配, <0=错误
 */
int powerfs_write_predict_dedup(struct inode *inode, loff_t offset,
                                 const u8 *data, size_t data_len,
                                 u64 *out_needle_id,
                                 u64 *out_volume_id,
                                 u32 *out_crc32)
{
    u8 fp[POWERFS_FP_SIZE];
    u8 prefix[POWERFS_FP_PREFIX_SIZE];
    u8 body[POWERFS_FP_LOOKUP_BODY_SIZE];
    u8 resp[64];
    size_t resp_len = 0;
    u64 shard_id;
    u64 data_size;
    int ret;
    u8 match_kind;

    if (!data || data_len == 0 || !out_needle_id || !out_volume_id)
        return -EINVAL;

    /* 1. compute SHA-256 fingerprint */
    ret = compute_sha256(data, data_len, fp);
    if (ret) {
        pr_warn_ratelimited("powerfs: write_predict SHA-256 failed ino=%lu: %d\n",
                            inode->i_ino, ret);
        return ret;
    }

    /* 2. extract prefix */
    memset(prefix, 0, sizeof(prefix));
    memcpy(prefix, data, min_t(size_t, data_len, sizeof(prefix)));

    /* 3. build lookup request body */
    data_size = (u64)data_len;
    shard_id = powerfs_calc_shard_id(inode->i_ino);

    memcpy(body, fp, POWERFS_FP_SIZE);
    put_unaligned_le64(data_size, body + 32);
    memcpy(body + 40, prefix, POWERFS_FP_PREFIX_SIZE);
    put_unaligned_le64(inode->i_ino, body + 104);
    put_unaligned_le64((u64)offset, body + 112);

    /* 4. send FingerprintLookup RPC (sync, 500ms timeout) */
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
        return ret;
    }
    if (ret > 0) {
        /* status error from filer */
        pr_debug_ratelimited("powerfs: write_predict lookup status=%d ino=%lu\n",
                             ret, inode->i_ino);
        return -EIO;
    }
    if (resp_len == 0) {
        return 0; /* empty response → NoMatch */
    }

    match_kind = resp[0];

    if (match_kind == POWERFS_FP_LOOKUP_NOMATCH) {
        pr_debug_ratelimited("powerfs: write_predict NoMatch ino=%lu off=%lld\n",
                             inode->i_ino, offset);
        return 0;
    }

    /* Match or Recoverable: parse needle_id, volume_id, crc32 */
    if (match_kind == POWERFS_FP_LOOKUP_MATCH) {
        /* 1 + needle_id(8) + volume_id(8) + crc32(4) + data_size(8) + refcount(4) = 33 */
        if (resp_len < 33) {
            pr_warn_ratelimited("powerfs: write_predict Match resp too short %zu\n",
                                resp_len);
            return -EIO;
        }
        *out_needle_id = get_unaligned_le64(resp + 1);
        *out_volume_id = get_unaligned_le64(resp + 9);
        *out_crc32 = get_unaligned_le32(resp + 17);
        pr_info_ratelimited("powerfs: WRITE_PREDICT_DEDUP ino=%lu off=%lld MATCH needle=%#llx vol=%llu crc=%#x\n",
                            inode->i_ino, offset,
                            (unsigned long long)*out_needle_id,
                            (unsigned long long)*out_volume_id,
                            *out_crc32);
        return 1;
    }

    if (match_kind == POWERFS_FP_LOOKUP_RECOVERABLE) {
        /* 1 + needle_id(8) + volume_id(8) + crc32(4) + data_size(8) = 29 */
        if (resp_len < 29) {
            pr_warn_ratelimited("powerfs: write_predict Recoverable resp too short %zu\n",
                                resp_len);
            return -EIO;
        }
        *out_needle_id = get_unaligned_le64(resp + 1);
        *out_volume_id = get_unaligned_le64(resp + 9);
        *out_crc32 = get_unaligned_le32(resp + 17);
        pr_info_ratelimited("powerfs: WRITE_PREDICT_DEDUP ino=%lu off=%lld RECOVER needle=%#llx vol=%llu\n",
                            inode->i_ino, offset,
                            (unsigned long long)*out_needle_id,
                            (unsigned long long)*out_volume_id);
        return 1;
    }

    pr_warn_ratelimited("powerfs: write_predict unknown match_kind=%d\n",
                        match_kind);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_write_predict_dedup);

/**
 * powerfs_write_predict_record - 写完新 needle 后记录指纹.
 */
void powerfs_write_predict_record(struct inode *inode,
                                   u64 needle_id, u64 volume_id,
                                   u32 crc32,
                                   const u8 *data, size_t data_len)
{
    u8 fp[POWERFS_FP_SIZE];
    u8 prefix[POWERFS_FP_PREFIX_SIZE];
    u8 body[POWERFS_FP_RECORD_BODY_SIZE];
    size_t resp_len = 0;
    u8 resp[16];
    int ret;

    if (!data || data_len == 0)
        return;

    /* compute fingerprint */
    ret = compute_sha256(data, data_len, fp);
    if (ret) {
        pr_debug_ratelimited("powerfs: write_predict record SHA-256 failed: %d\n",
                             ret);
        return;
    }

    /* extract prefix */
    memset(prefix, 0, sizeof(prefix));
    memcpy(prefix, data, min_t(size_t, data_len, sizeof(prefix)));

    /* build record body */
    memcpy(body, fp, POWERFS_FP_SIZE);
    put_unaligned_le64(needle_id, body + 32);
    put_unaligned_le64(volume_id, body + 40);
    put_unaligned_le32(crc32, body + 48);
    put_unaligned_le64((u64)data_len, body + 52);
    memcpy(body + 60, prefix, POWERFS_FP_PREFIX_SIZE);

    /* send FingerprintRecord RPC (best-effort, 500ms timeout) */
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
EXPORT_SYMBOL_GPL(powerfs_write_predict_record);

/**
 * powerfs_write_predict_store_dedup - 记录某 chunk 去重命中的 needle 引用.
 *
 * 命中后 pi->dedup_chunks[chunk_idx] = {matched needle_id, matched volume_id},
 * sync_size_chunks 构建 chunks[] 时优先用此表, 把去重后的引用持久化到 Filer.
 *
 * 由 i_lock 保护; 按需扩展 dedup_chunks 数组 (kalloc + memcpy).
 * 返回 0 成功, <0 失败 (调用方回退到正常写).
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
        /* 新条目初始化为 0 (未命中) */
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
