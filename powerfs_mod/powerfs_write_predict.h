/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_write_predict.h - 写预测 + 内容指纹去重 (kernel 客户端)
 *
 * 详见 docs/write-prediction-dedup-design.md §C-1.5/C-1.6.
 *
 * 三层流水线 (kernel 端):
 *   1. 读 xattr user.powerfs.write_predict_policy → 阈值
 *   2. 阈值 > 0 → 计算 SHA-256 指纹 → 发 FingerprintLookup RPC 到 filer
 *   3. Match/Recoverable → 引用已有 needle (跳过 volume 写)
 *      NoMatch → 正常写 volume → 发 FingerprintRecord RPC
 *
 * 安全回退 (C-1.6):
 *   - xattr 缺失 / "off" / 阈值≤0 → 不算指纹, 正常写
 *   - 指纹计算失败 → 正常写
 *   - RPC 失败 → 正常写 (best-effort, 不阻塞 I/O)
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
 * inline 空实现: should_dedup 恒返回 false → 调用点的整个去重分支被
 * 编译器常量折叠消除, 写路径 (buffered write_iter / DIO / writeback
 * work_fn) 不产生任何额外指令, 也不依赖 crypto/SHA-256.
 * pi->write_predict_* 缓存字段在关闭时无人读写, 保留在结构体中
 * 以避免布局差异.
 */
#ifdef CONFIG_POWERFS_WRITE_PREDICT

/**
 * powerfs_write_predict_should_dedup - 检查 inode 是否启用了写预测去重.
 *
 * 查询 xattr user.powerfs.write_predict_policy:
 *   - "off" 或 阈值≤0 → 返回 false (跳过去重)
 *   - "NN:<float>" 或 "RULE:<float>" → 阈值>0 返回 true
 *   - 缺失 → 查父目录 xattr (目录级继承, 递归向上)
 *
 * 结果缓存在 pi->write_predict_enabled 中, 避免每次写都查 xattr.
 * Filer PushDelta 通知时通过 powerfs_write_predict_invalidate 失效.
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
 * 仅读 pi->write_predict_cached && pi->write_predict_enabled (持 i_lock),
 * 不查 xattr, 不需要 dentry. 用于 writeback 热路径 (wb_submit_write_direct /
 * write_cb), 避免 d_find_any_alias + xattr 查询的性能开销.
 *
 * 缓存由 write_iter 中 should_dedup 预热 (用户进程上下文可同步查 xattr).
 * 若缓存未预热 (cached==false), 返回 false — 本次写不走 dedup, 下次
 * write_iter 预热后生效.
 *
 * @pi: powerfs inode info
 *
 * 返回: true=启用去重, false=跳过
 */
bool powerfs_write_predict_is_enabled(struct powerfs_inode_info *pi);

/**
 * powerfs_write_predict_dedup - 对一段写数据执行指纹去重.
 *
 * 流程:
 *   1. 计算 SHA-256(data) → 32 字节指纹
 *   2. 发 FingerprintLookup RPC 到 filer (同步, timeout 500ms)
 *   3. Match/Recoverable → 设置 *out_needle_id / *out_volume_id / *out_crc32
 *      返回 1 (去重成功, 调用方跳过 volume 写)
 *   4. NoMatch → 返回 0 (调用方正常写 volume, 之后调 powerfs_write_predict_record)
 *
 * @inode: 文件 inode
 * @offset: chunk offset (bytes)
 * @data: 写数据
 * @data_len: 数据长度
 * @out_needle_id: [out] 匹配的 needle_id
 * @out_volume_id: [out] 匹配的 volume_id
 * @out_crc32: [out] 匹配的 needle crc32
 *
 * 返回: 1=去重成功 (跳过写), 0=未匹配 (正常写), <0=错误 (正常写)
 */
int powerfs_write_predict_dedup(struct inode *inode, loff_t offset,
                                 const __u8 *data, size_t data_len,
                                 __u64 *out_needle_id,
                                 __u64 *out_volume_id,
                                 __u32 *out_crc32);

/**
 * powerfs_write_predict_record - 写完新 needle 后记录指纹.
 *
 * 在 powerfs_write_predict_dedup 返回 0 (NoMatch) 且 volume 写成功后调用.
 * 发 FingerprintRecord RPC 到 filer (best-effort, 失败不影响正确性).
 *
 * @inode: 文件 inode
 * @needle_id: 新写入的 needle_id
 * @volume_id: volume_id
 * @crc32: 数据 crc32
 * @data: 原始写数据 (用于计算指纹和 prefix)
 * @data_len: 数据长度
 */
void powerfs_write_predict_record(struct inode *inode,
                                   __u64 needle_id, __u64 volume_id,
                                   __u32 crc32,
                                   const __u8 *data, size_t data_len);

/**
 * powerfs_write_predict_store_dedup - 记录某 chunk 去重命中的 needle 引用.
 *
 * writeback 整 needle 覆盖写时, 若 FingerprintLookup 命中已有 needle,
 * 调用此函数把 (chunk_idx, matched needle_id, matched volume_id) 记入
 * pi->dedup_chunks. sync_size_chunks 构建 chunks[] 时优先查此表,
 * 将去重后的引用持久化到 Filer (跳过 volume 写, 零数据传输).
 *
 * @inode: 文件 inode
 * @chunk_idx: chunk 索引 (offset / POWERFS_CHUNK_SIZE)
 * @matched_needle_id: 匹配到的已有 needle_id
 * @matched_volume_id: 匹配到的 volume_id
 *
 * 返回: 0 成功, <0 失败 (调用方回退到正常写 volume)
 */
int powerfs_write_predict_store_dedup(struct inode *inode, u32 chunk_idx,
                                      __u64 matched_needle_id,
                                      __u64 matched_volume_id);

/**
 * powerfs_write_predict_record_async - 异步记录指纹 (write_cb 安全版本).
 *
 * write_cb 在 RDMA CQ 回调上下文, 不能执行同步 RPC. 此函数拷贝数据并
 * schedule_work, 由 worker 线程调用 powerfs_write_predict_record.
 *
 * @inode: 文件 inode (内部 igrab, worker 结束后 iput)
 * @needle_id: 新写入的 needle_id
 * @volume_id: volume_id
 * @data: needle 数据 (内部 memcpy)
 * @data_len: 数据长度
 */
void powerfs_write_predict_record_async(struct inode *inode,
                                        __u64 needle_id, __u64 volume_id,
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

static inline int powerfs_write_predict_dedup(struct inode *inode, loff_t offset,
                                              const __u8 *data, size_t data_len,
                                              __u64 *out_needle_id,
                                              __u64 *out_volume_id,
                                              __u32 *out_crc32)
{
    return 0;  /* NoMatch → 调用方走正常写 */
}

static inline void powerfs_write_predict_record(struct inode *inode,
                                                __u64 needle_id, __u64 volume_id,
                                                __u32 crc32,
                                                const __u8 *data, size_t data_len)
{
}

static inline int powerfs_write_predict_store_dedup(struct inode *inode,
                                                    u32 chunk_idx,
                                                    __u64 matched_needle_id,
                                                    __u64 matched_volume_id)
{
    return -ENOSYS;  /* 未启用 → 回退正常写 */
}

static inline void powerfs_write_predict_record_async(struct inode *inode,
                                                      __u64 needle_id,
                                                      __u64 volume_id,
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
