/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_write_predict.h - 写预测 + 内容指纹去重 (kernel 客户端)
 *
 * 详见 docs/write-prediction-dedup-design.md §C-1.5/C-1.6.
 *
 * ======= 方向②: 先写后问 (async, best-effort) =======
 *
 * 三层流水线 (kernel 端, 全异步, 热路径零阻塞):
 *   1. 热路径: lockless 查 pi->dedup_chunks 表 — 若异步 worker 已为该 chunk
 *      命中过 dedup (来自之前覆盖写的后台 lookup), 才跳过 volume 写
 *   2. write_cb / DIO write 成功后 → lookup_record_async:
 *      memcpy needle_buf → schedule_work
 *        → compute SHA-256(data)          // 只算一次
 *        → FingerprintLookup RPC (filer)
 *          Match → store_dedup(chunk_idx, matched_nid, matched_vid)
 *        → FingerprintRecord RPC (filer)  // 无论 Match/NoMatch
 *   3. sync_size_chunks: 把 dedup_chunks 命中的引用持久化到 Filer
 *
 * 安全回退 (C-1.6):
 *   - xattr 缺失 / "off" / 阈值≤0 → is_enabled=false → dedup_chunks 永远空 → 正常写
 *   - 指纹计算失败 → 正常写 (worker 静默跳过)
 *   - RPC 失败 → 正常写 (best-effort, 下次写会重试)
 *
 * 指纹算法: SHA-256 (kernel crypto API, CONFIG_CRYPTO_SHA256=y).
 * 与 Rust 端 powerfs-core/src/fingerprint.rs (sha2 crate) 完全对齐.
 */
#ifndef _POWERFS_WRITE_PREDICT_H
#define _POWERFS_WRITE_PREDICT_H

#include <linux/types.h>
#include <linux/fs.h>

struct inode;
struct powerfs_inode_info;

/* xattr 名 — 与 filer 端 write_predict_policy.rs 保持一致 */
#define POWERFS_WRITE_PREDICT_XATTR_NAME "user.powerfs.write_predict_policy"

/* Fingerprint wire format (与 filer handle_fingerprint_lookup 对齐) */
#define POWERFS_FP_SIZE         32   /* SHA-256 = 32 bytes */
#define POWERFS_FP_PREFIX_SIZE  64   /* data prefix for collision check */
#define POWERFS_FP_LOOKUP_BODY_SIZE  120  /* fp(32)+size(8)+prefix(64)+ino(8)+off(8) */
#define POWERFS_FP_RECORD_BODY_SIZE  124  /* fp(32)+nid(8)+vid(8)+crc(4)+size(8)+prefix(64) */

/* Lookup response kinds (与 filer LookupResult 对齐) */
#define POWERFS_FP_LOOKUP_NOMATCH     0
#define POWERFS_FP_LOOKUP_MATCH       1
#define POWERFS_FP_LOOKUP_RECOVERABLE 2

/*
 * 编译期总开关: CONFIG_POWERFS_WRITE_PREDICT 由 Makefile 定义
 * (make WRITE_PREDICT=y), 缺省不定义 — 功能默认关闭.
 *
 * 关闭时 powerfs_write_predict.c 不参与编译, 下列 hook 全部为 static
 * inline 空实现 — 调用点的整个 dedup 分支被编译器常量折叠消除, 热路径
 * 不产生任何额外指令, 也不依赖 crypto/SHA-256.
 * pi->dedup_chunks / write_predict_* 字段在关闭时由 #ifdef 排除出
 * powerfs_inode_info 布局, 零空间残留.
 */
#ifdef CONFIG_POWERFS_WRITE_PREDICT

/**
 * powerfs_write_predict_should_dedup - 检查 inode 是否启用写预测去重.
 *
 * 查询 xattr user.powerfs.write_predict_policy:
 *   - "off" 或 阈值≤0 → 返回 false (跳过去重)
 *   - "NN:<float>" 或 "RULE:<float>" → 阈值>0 返回 true
 *   - 缺失 → 查父目录 xattr (目录级继承, 递归向上)
 *
 * 结果缓存在 pi->write_predict_enabled 中, 避免每次写都查 xattr.
 * Filer PushDelta 通知时通过 powerfs_write_predict_invalidate 失效.
 *
 * 首次调用用于预热 is_enabled 缓存 (在 workqueue 上下文中可阻塞查 xattr RPC).
 * 后续热路径走 is_enabled() 快速路径, 不查 dentry.
 *
 * @inode: 文件 inode
 * @dentry: 文件 dentry (用于查父目录 xattr 继承, 可为 NULL)
 *
 * 返回: true=启用去重, false=跳过 (正常写)
 */
bool powerfs_write_predict_should_dedup(struct inode *inode,
                                          struct dentry *dentry);

/**
 * powerfs_write_predict_is_enabled - 快速检查 inode 是否启用写预测去重.
 *
 * 仅读 pi->write_predict_cached && pi->write_predict_enabled (无锁),
 * 不查 xattr, 不查 dentry. 用于热路径 (writeback submit / write_cb /
 * DIO write), 避免 d_find_any_alias + xattr 查询的性能开销.
 *
 * 缓存由首次 should_dedup 调用预热 (用户进程/workqueue 上下文可同步查 xattr).
 * 若缓存未预热 (cached==false), 返回 false — 本次写不调度 dedup worker, 下次
 * should_dedup 预热后生效.
 *
 * @pi: powerfs inode info
 *
 * 返回: true=启用去重 (可调度 dedup worker / 查 dedup_chunks), false=跳过
 */
bool powerfs_write_predict_is_enabled(struct powerfs_inode_info *pi);

/**
 * powerfs_write_predict_store_dedup - 记录某 chunk 去重命中的 needle 引用.
 *
 * 由异步 lookup worker 在 Match/Recoverable 时调用. 持 i_lock 保护.
 * sync_size_chunks 构建 chunks[] 时优先查此表, 将去重后的引用持久化
 * 到 Filer (跳过 volume 写, 零数据传输).
 *
 * 热路径也 lockless 查此表: 若该 chunk 的 needle_id != 0, 说明之前覆盖写
 * 时异步 lookup 已命中过 dedup, 直接跳过 volume 写 (仅适用于二次覆盖写
 * 相同内容场景).
 *
 * @inode: 文件 inode
 * @chunk_idx: chunk 索引 (offset / POWERFS_CHUNK_SIZE)
 * @matched_needle_id: 匹配到的已有 needle_id
 * @matched_volume_id: 匹配到的 volume_id
 *
 * 返回: 0 成功, <0 失败 (调用方 best-effort, 忽略即可)
 */
int powerfs_write_predict_store_dedup(struct inode *inode, u32 chunk_idx,
                                      __u64 matched_needle_id,
                                      __u64 matched_volume_id);

/**
 * powerfs_write_predict_lookup_record_async - 异步 lookup + record (方向②核心入口).
 *
 * 在 write_cb (RDMA CQ 回调) 或 DIO write 成功后调用. 这两个上下文不能做同步
 * RPC, 所以内部 igrab inode + memcpy 数据 + schedule_work, 真正的 SHA-256 +
 * FingerprintLookup + FingerprintRecord 在 worker 线程中异步执行.
 *
 * 一次 SHA-256 同时服务 lookup 和 record, 避免重复计算.
 *
 * 注意: 调用方必须先调用 powerfs_write_predict_is_enabled(pi) 确认已启用.
 * 若未启用, 调用此函数是 no-op (内部 sha256_tfm=NULL 时静默 return).
 *
 * @inode: 文件 inode (内部 igrab, worker 结束后 iput)
 * @needle_id: 刚写的 needle_id
 * @volume_id: 刚写的 volume_id
 * @chunk_idx: 对应 dedup_chunks 的索引 (offset / POWERFS_CHUNK_SIZE)
 * @offset: needle 起始 offset, 用于 lookup 请求 (filer 可能用 offset 优化)
 * @data: 刚写的数据 (内部 memcpy 深拷贝, 调用方后续释放不影响)
 * @data_len: 数据长度 (通常 = POWERFS_CHUNK_SIZE)
 */
void powerfs_write_predict_lookup_record_async(struct inode *inode,
                                                __u64 needle_id, __u64 volume_id,
                                                __u64 chunk_idx, loff_t offset,
                                                const __u8 *data, size_t data_len);

/**
 * powerfs_write_predict_invalidate - 标记 per-inode 策略缓存失效.
 *
 * Filer PushDelta 通知策略更新时调用, 促使下次 should_dedup 重新查 xattr.
 */
void powerfs_write_predict_invalidate(struct inode *inode);

/**
 * powerfs_write_predict_inode_init - inode 分配时初始化字段.
 */
void powerfs_write_predict_inode_init(struct powerfs_inode_info *pi);

/**
 * powerfs_write_predict_init - 模块 init 时初始化 SHA-256 transform.
 *
 * 返回: 0 成功, <0 失败 (禁用去重功能, 不阻止模块加载)
 */
int powerfs_write_predict_init(void);

/**
 * powerfs_write_predict_exit - 模块 exit 时释放 SHA-256 transform.
 */
void powerfs_write_predict_exit(void);

#else /* !CONFIG_POWERFS_WRITE_PREDICT: 缺省关闭, hook 全部为空实现 */

static inline bool powerfs_write_predict_should_dedup(struct inode *inode,
                                                      struct dentry *dentry)
{
    return false;
}

static inline bool powerfs_write_predict_is_enabled(struct powerfs_inode_info *pi)
{
    return false;
}

static inline int powerfs_write_predict_store_dedup(struct inode *inode,
                                                    u32 chunk_idx,
                                                    __u64 matched_needle_id,
                                                    __u64 matched_volume_id)
{
    return -ENOSYS;
}

static inline void powerfs_write_predict_lookup_record_async(struct inode *inode,
                                                              __u64 needle_id,
                                                              __u64 volume_id,
                                                              __u64 chunk_idx,
                                                              loff_t offset,
                                                              const __u8 *data,
                                                              size_t data_len)
{
}

static inline void powerfs_write_predict_invalidate(struct inode *inode)
{
}

static inline void powerfs_write_predict_inode_init(struct powerfs_inode_info *pi)
{
}

static inline int powerfs_write_predict_init(void)
{
    return 0;
}

static inline void powerfs_write_predict_exit(void)
{
}
#endif /* !CONFIG_POWERFS_WRITE_PREDICT */

#endif /* _POWERFS_WRITE_PREDICT_H */
