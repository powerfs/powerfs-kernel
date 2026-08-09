/* SPDX-License-Identifier: GPL-2.0 */
/*
 * powerfs_ec.h - Reed-Solomon EC 编解码内核移植 (K4-1)
 *
 * 对齐 FUSE powerfs-core/src/ec_thread.rs (reed_solomon_erasure crate):
 *   - GF(2^8) with generator polynomial 0x11d (x^8+x^4+x^3+x^2+1)
 *   - Vandermonde 编码矩阵
 *   - 高斯消元矩阵求逆 (降级重建)
 *
 * 用途: K4-5 (EC 分片读取) / K4-6 (降级重建) / K4-7 (编码写)
 *       的基础编解码能力.
 */
#ifndef POWERFS_EC_H
#define POWERFS_EC_H

#include <linux/types.h>
#include <linux/slab.h>

/* GF(2^8) 运算表 (生成多项式 0x11d) */
struct powerfs_gf_tables {
    u8 exp[512];  /* 指数表 (循环扩展到 512 避免 mod) */
    u8 log[256];  /* 对数表, log[0] 未定义 (0 无对数) */
};

/* EC 编解码器 */
struct powerfs_ec_codec {
    u32 data_shards;       /* 数据分片数 */
    u32 parity_shards;     /* 校验分片数 */
    u32 total_shards;      /* data + parity */
    struct powerfs_gf_tables gf;
    /* Vandermonde 编码矩阵 (parity × data), kmalloc */
    u8 *enc_matrix;
};

/**
 * powerfs_ec_init - 创建 EC 编解码器
 * @data_shards: 数据分片数 (≥1)
 * @parity_shards: 校验分片数 (≥1)
 *
 * 初始化 GF(2^8) 运算表, 生成 Vandermonde 编码矩阵.
 *
 * 返回: 编解码器指针, ERR_PTR(-ENOMEM) 分配失败, ERR_PTR(-EINVAL) 参数无效.
 */
struct powerfs_ec_codec *powerfs_ec_init(u32 data_shards, u32 parity_shards);

/**
 * powerfs_ec_free - 释放 EC 编解码器
 */
void powerfs_ec_free(struct powerfs_ec_codec *codec);

/**
 * powerfs_ec_encode - 从数据分片生成校验分片
 * @codec: 编解码器
 * @shards: 分片数组 [data_shards + parity_shards], 前 data 个为数据,
 *          后 parity 个由本函数填充
 * @shard_size: 每个分片大小 (字节)
 *
 * 返回: 0 成功, 负数错误码.
 */
int powerfs_ec_encode(const struct powerfs_ec_codec *codec,
                      u8 **shards, size_t shard_size);

/**
 * powerfs_ec_decode - 降级重建缺失分片
 * @codec: 编解码器
 * @shards: 分片数组 [total_shards], 缺失的分片为 NULL,
 *          本函数会为缺失分片分配内存 (kmalloc)
 * @available: [total_shards] bool 数组, true=分片可用
 * @shard_size: 每个分片大小
 *
 * 重建逻辑:
 *   1. 从编码矩阵中选取可用分片对应的行, 组成子矩阵
 *   2. 求逆 (高斯消元, GF(2^8))
 *   3. 用逆矩阵重建缺失的数据分片
 *   4. 用 encode 重建缺失的校验分片
 *
 * 返回: 0 成功, -EINVAL 参数错误, -EBADRMSG 可用分片不足, -ENOMEM 分配失败.
 */
int powerfs_ec_decode(struct powerfs_ec_codec *codec,
                      u8 **shards, const bool *available,
                      size_t shard_size);

/**
 * powerfs_ec_can_recover - 检查是否可恢复
 * @codec: 编解码器
 * @available: [total_shards] bool 数组
 *
 * 返回: true 如果可用分片数 ≥ data_shards.
 */
bool powerfs_ec_can_recover(const struct powerfs_ec_codec *codec,
                            const bool *available);

#endif /* POWERFS_EC_H */
