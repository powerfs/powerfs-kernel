// SPDX-License-Identifier: GPL-2.0
/*
 * powerfs_ec.c - Reed-Solomon EC 编解码内核实现 (K4-1)
 *
 * 对齐 FUSE powerfs-core/src/ec_thread.rs (reed_solomon_erasure crate):
 *   - GF(2^8) with generator polynomial 0x11d (x^8+x^4+x^3+x^2+1)
 *   - Vandermonde 编码矩阵
 *   - 高斯消元矩阵求逆 (降级重建)
 *
 * 参考:
 *   - reed_solomon_erasure crate (Rust)
 *   - Linux lib/reed_solomon/ (内核已有 RS 库, 但接口不同, 这里自实现以对齐 FUSE)
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include "powerfs_ec.h"

/* ========== GF(2^8) 运算 ========== */

/* 生成多项式 0x11d (x^8+x^4+x^3+x^2+1), 对齐 reed_solomon_erasure. */
#define POWERFS_GF_POLY 0x11d

static void gf_init(struct powerfs_gf_tables *gf)
{
    u32 i, x;

    x = 1;
    for (i = 0; i < 255; i++) {
        gf->exp[i] = (u8)x;
        gf->log[x] = (u8)i;
        x <<= 1;
        if (x & 0x100)
            x ^= POWERFS_GF_POLY;
    }
    /* 循环扩展: exp[i+255] == exp[i] */
    for (i = 255; i < 512; i++)
        gf->exp[i] = gf->exp[i - 255];
    /* log[0] 未定义, 设为 0 避免未初始化 */
    gf->log[0] = 0;
}

static inline u8 gf_mul(const struct powerfs_gf_tables *gf, u8 a, u8 b)
{
    if (a == 0 || b == 0)
        return 0;
    return gf->exp[gf->log[a] + gf->log[b]];
}

static inline u8 gf_div(const struct powerfs_gf_tables *gf, u8 a, u8 b)
{
    if (a == 0)
        return 0;
    /* b != 0 (调用方保证) */
    return gf->exp[(gf->log[a] + 255 - gf->log[b]) % 255];
}

static inline u8 gf_inv(const struct powerfs_gf_tables *gf, u8 a)
{
    /* a != 0 (调用方保证) */
    return gf->exp[255 - gf->log[a]];
}

static inline u8 gf_pow(const struct powerfs_gf_tables *gf, u8 a, u32 exp)
{
    if (a == 0)
        return 0;
    if (exp == 0)
        return 1;
    return gf->exp[(gf->log[a] * exp) % 255];
}

/* ========== Vandermonde 矩阵 ========== */

/* 生成 Vandermonde 矩阵 (rows × cols), rows = parity, cols = data.
 * V[i][j] = j^i (GF 运算).
 * 对齐 reed_solomon_erasure ReedSolomon::new 中的矩阵生成. */
static void vandermonde_matrix(const struct powerfs_gf_tables *gf,
                               u8 *matrix, u32 rows, u32 cols)
{
    u32 r, c;

    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
            matrix[r * cols + c] = gf_pow(gf, (u8)c, r);
}

/* 将 Vandermonde 矩阵转换为系统形式 [I | P],
 * 其中 P 是 parity 编码矩阵 (parity × data).
 * 对齐 reed_solomon_erasure: 通过行变换使上半部分成为单位矩阵.
 *
 * 输入 matrix: (parity+data) × data 的 Vandermonde 矩阵
 * 输出 enc_matrix: parity × data 的编码矩阵 */
static int vandermonde_to_systematic(const struct powerfs_gf_tables *gf,
                                     u8 *matrix, u32 total_rows, u32 data)
{
    u32 row, col, r;
    u8 *m = matrix;

    /* 前提: Vandermonde 矩阵的前 data 行可逆 (行列式 != 0).
     * 通过高斯消元将前 data 行化为单位矩阵, 后 parity 行变为编码矩阵. */
    for (col = 0; col < data; col++) {
        /* 找到主元行 (col 列非零) */
        u32 pivot = col;
        if (m[col * data + col] == 0) {
            for (pivot = col + 1; pivot < total_rows; pivot++) {
                if (m[pivot * data + col] != 0)
                    break;
            }
            if (pivot >= total_rows)
                return -EINVAL;
            /* 交换行 */
            for (r = 0; r < data; r++) {
                u8 tmp = m[col * data + r];
                m[col * data + r] = m[pivot * data + r];
                m[pivot * data + r] = tmp;
            }
        }

        /* 归一化主元行 (主元位置变为 1) */
        {
            u8 inv = gf_inv(gf, m[col * data + col]);
            for (r = col; r < data; r++)
                m[col * data + r] = gf_mul(gf, m[col * data + r], inv);
        }

        /* 消元: 其他行的 col 列清零 */
        for (row = 0; row < total_rows; row++) {
            if (row == col || m[row * data + col] == 0)
                continue;
            {
                u8 coef = m[row * data + col];
                for (r = col; r < data; r++) {
                    m[row * data + r] ^=
                        gf_mul(gf, m[col * data + r], coef);
                }
            }
        }
    }

    return 0;
}

/* ========== 编解码器 ========== */

struct powerfs_ec_codec *powerfs_ec_init(u32 data_shards, u32 parity_shards)
{
    struct powerfs_ec_codec *codec;
    u8 *vmatrix;
    u32 total;
    int ret;

    if (data_shards == 0 || parity_shards == 0 ||
        data_shards + parity_shards > 255)
        return ERR_PTR(-EINVAL);

    codec = kzalloc(sizeof(*codec), GFP_KERNEL);
    if (!codec)
        return ERR_PTR(-ENOMEM);

    codec->data_shards = data_shards;
    codec->parity_shards = parity_shards;
    codec->total_shards = data_shards + parity_shards;

    /* 初始化 GF(2^8) 运算表 */
    gf_init(&codec->gf);

    /* 生成 Vandermonde 矩阵 (total × data) */
    total = codec->total_shards;
    vmatrix = kmalloc_array(total, data_shards, GFP_KERNEL);
    if (!vmatrix) {
        kfree(codec);
        return ERR_PTR(-ENOMEM);
    }
    vandermonde_matrix(&codec->gf, vmatrix, total, data_shards);

    /* 转换为系统形式, 后 parity 行为编码矩阵 */
    ret = vandermonde_to_systematic(&codec->gf, vmatrix, total, data_shards);
    if (ret) {
        pr_err("powerfs: EC vandermonde_to_systematic failed: %d\n", ret);
        kfree(vmatrix);
        kfree(codec);
        return ERR_PTR(ret);
    }

    /* 提取编码矩阵 (后 parity 行) */
    codec->enc_matrix = kmalloc_array(parity_shards, data_shards, GFP_KERNEL);
    if (!codec->enc_matrix) {
        kfree(vmatrix);
        kfree(codec);
        return ERR_PTR(-ENOMEM);
    }
    memcpy(codec->enc_matrix, vmatrix + data_shards * data_shards,
           parity_shards * data_shards);

    kfree(vmatrix);

    pr_info("powerfs: EC codec init data=%u parity=%u total=%u\n",
            data_shards, parity_shards, total);
    return codec;
}

void powerfs_ec_free(struct powerfs_ec_codec *codec)
{
    if (!codec)
        return;
    kfree(codec->enc_matrix);
    kfree(codec);
}

/* ========== 编码 ========== */

int powerfs_ec_encode(const struct powerfs_ec_codec *codec,
                      u8 **shards, size_t shard_size)
{
    u32 i, p, d;
    const struct powerfs_gf_tables *gf;

    if (!codec || !shards)
        return -EINVAL;

    gf = &codec->gf;

    /* 每个 parity 分片: parity[i] = sum(enc_matrix[i][d] * data[d]) */
    for (p = 0; p < codec->parity_shards; p++) {
        u8 *parity_shard = shards[codec->data_shards + p];
        u8 *coef_row = codec->enc_matrix + p * codec->data_shards;

        if (!parity_shard || !shards[0])
            return -EINVAL;

        /* 初始化 parity 分片为 0 */
        memset(parity_shard, 0, shard_size);

        for (d = 0; d < codec->data_shards; d++) {
            u8 coef = coef_row[d];
            u8 *data_shard = shards[d];

            if (coef == 0 || !data_shard)
                continue;

            /* parity_shard ^= coef * data_shard (逐字节 GF 乘加) */
            for (i = 0; i < shard_size; i++)
                parity_shard[i] ^= gf_mul(gf, data_shard[i], coef);
        }
    }

    return 0;
}

/* ========== 降级重建 ========== */

bool powerfs_ec_can_recover(const struct powerfs_ec_codec *codec,
                            const bool *available)
{
    u32 i, count = 0;

    if (!codec || !available)
        return false;

    for (i = 0; i < codec->total_shards; i++) {
        if (available[i])
            count++;
    }
    return count >= codec->data_shards;
}

/* GF(2^8) 矩阵求逆 (高斯消元), n×n 矩阵原地求逆.
 * matrix: n×n 矩阵 (输入), 求逆后覆盖原矩阵.
 * 返回 0 成功, -EINVAL 矩阵不可逆. */
static int gf_matrix_invert(const struct powerfs_gf_tables *gf,
                            u8 *matrix, u32 n)
{
    u8 *identity;
    u32 row, col, r;
    int ret = 0;

    identity = kmalloc_array(n * n, sizeof(u8), GFP_KERNEL);
    if (!identity)
        return -ENOMEM;

    /* 初始化单位矩阵 */
    memset(identity, 0, n * n);
    for (r = 0; r < n; r++)
        identity[r * n + r] = 1;

    /* 增广矩阵 [matrix | identity], 对 matrix 做高斯消元, identity 跟随 */
    for (col = 0; col < n; col++) {
        /* 找主元 */
        u32 pivot = col;
        if (matrix[col * n + col] == 0) {
            for (pivot = col + 1; pivot < n; pivot++) {
                if (matrix[pivot * n + col] != 0)
                    break;
            }
            if (pivot >= n) {
                ret = -EINVAL;
                goto out;
            }
            /* 交换行 */
            for (r = 0; r < n; r++) {
                u8 tmp = matrix[col * n + r];
                matrix[col * n + r] = matrix[pivot * n + r];
                matrix[pivot * n + r] = tmp;
                tmp = identity[col * n + r];
                identity[col * n + r] = identity[pivot * n + r];
                identity[pivot * n + r] = tmp;
            }
        }

        /* 归一化 */
        {
            u8 inv = gf_inv(gf, matrix[col * n + col]);
            for (r = 0; r < n; r++) {
                matrix[col * n + r] = gf_mul(gf, matrix[col * n + r], inv);
                identity[col * n + r] = gf_mul(gf, identity[col * n + r], inv);
            }
        }

        /* 消元 */
        for (row = 0; row < n; row++) {
            if (row == col || matrix[row * n + col] == 0)
                continue;
            {
                u8 coef = matrix[row * n + col];
                for (r = 0; r < n; r++) {
                    matrix[row * n + r] ^=
                        gf_mul(gf, matrix[col * n + r], coef);
                    identity[row * n + r] ^=
                        gf_mul(gf, identity[col * n + r], coef);
                }
            }
        }
    }

    /* identity 现在是逆矩阵 */
    memcpy(matrix, identity, n * n);

out:
    kfree(identity);
    return ret;
}

int powerfs_ec_decode(struct powerfs_ec_codec *codec,
                      u8 **shards, const bool *available,
                      size_t shard_size)
{
    const struct powerfs_gf_tables *gf;
    u8 *sub_matrix;     /* data × data 子矩阵 */
    u8 *inv_matrix;     /* data × data 逆矩阵 */
    u8 *decode_matrix;  /* missing_data × data 重建矩阵 */
    u32 *avail_indices;  /* 可用分片索引 */
    u32 *missing_data;   /* 缺失的数据分片索引 */
    u32 avail_count = 0, missing_count = 0;
    u32 i, j, k, d;
    int ret;

    if (!codec || !shards || !available)
        return -EINVAL;

    gf = &codec->gf;

    if (!powerfs_ec_can_recover(codec, available))
        return -EBADMSG;

    /* 统计可用/缺失分片 */
    avail_indices = kmalloc_array(codec->total_shards, sizeof(u32), GFP_KERNEL);
    missing_data = kmalloc_array(codec->data_shards, sizeof(u32), GFP_KERNEL);
    if (!avail_indices || !missing_data) {
        kfree(avail_indices);
        kfree(missing_data);
        return -ENOMEM;
    }

    for (i = 0; i < codec->total_shards; i++) {
        if (available[i])
            avail_indices[avail_count++] = i;
        else if (i < codec->data_shards)
            missing_data[missing_count++] = i;
    }

    /* 如果没有缺失的数据分片, 只需重建缺失的校验分片 */
    if (missing_count == 0) {
        /* 重建缺失的 parity 分片 */
        for (i = codec->data_shards; i < codec->total_shards; i++) {
            if (!available[i]) {
                u8 *parity_shard;
                u8 *coef_row = codec->enc_matrix +
                               (i - codec->data_shards) * codec->data_shards;

                if (!shards[i]) {
                    shards[i] = kmalloc(shard_size, GFP_KERNEL);
                    if (!shards[i]) {
                        ret = -ENOMEM;
                        goto out;
                    }
                }
                parity_shard = shards[i];
                memset(parity_shard, 0, shard_size);
                for (d = 0; d < codec->data_shards; d++) {
                    u8 coef = coef_row[d];
                    if (coef == 0 || !shards[d])
                        continue;
                    for (k = 0; k < shard_size; k++)
                        parity_shard[k] ^= gf_mul(gf, shards[d][k], coef);
                }
            }
        }
        ret = 0;
        goto out;
    }

    /* 构建子矩阵: 从编码矩阵中选取 avail_count 个可用分片对应的行.
     * 编码矩阵是 (total × data) 的系统形式:
     *   前 data 行 = 单位矩阵 (对应数据分片)
     *   后 parity 行 = 校验矩阵 (对应校验分片)
     * 选取 data 个可用分片的行组成 data×data 子矩阵. */
    sub_matrix = kmalloc_array(codec->data_shards * codec->data_shards,
                               sizeof(u8), GFP_KERNEL);
    inv_matrix = kmalloc_array(codec->data_shards * codec->data_shards,
                               sizeof(u8), GFP_KERNEL);
    if (!sub_matrix || !inv_matrix) {
        kfree(sub_matrix);
        kfree(inv_matrix);
        ret = -ENOMEM;
        goto out;
    }

    /* 选取前 data 个可用分片构建子矩阵 */
    for (i = 0; i < codec->data_shards; i++) {
        u32 shard_idx = avail_indices[i];
        u8 *src_row;

        if (shard_idx < codec->data_shards) {
            /* 数据分片: 单位矩阵的对应行 */
            memset(sub_matrix + i * codec->data_shards, 0, codec->data_shards);
            sub_matrix[i * codec->data_shards + shard_idx] = 1;
        } else {
            /* 校验分片: 编码矩阵的对应行 */
            src_row = codec->enc_matrix +
                      (shard_idx - codec->data_shards) * codec->data_shards;
            memcpy(sub_matrix + i * codec->data_shards, src_row,
                   codec->data_shards);
        }
    }

    /* 求逆 */
    memcpy(inv_matrix, sub_matrix,
           codec->data_shards * codec->data_shards);
    ret = gf_matrix_invert(gf, inv_matrix, codec->data_shards);
    if (ret) {
        pr_err("powerfs: EC gf_matrix_invert failed: %d\n", ret);
        kfree(sub_matrix);
        kfree(inv_matrix);
        goto out;
    }

    /* 构建重建矩阵: 缺失数据分片对应的逆矩阵行 × 子矩阵
     * 实际上, 逆矩阵的行直接对应可用分片到缺失分片的映射.
     * 对于缺失的数据分片 m, 重建:
     *   shards[m] = sum(inv_matrix[m_row][i] * shards[avail[i]])
     *   for i in 0..data-1 */
    for (j = 0; j < missing_count; j++) {
        u32 missing_idx = missing_data[j];
        u8 *inv_row = inv_matrix + missing_idx * codec->data_shards;
        u8 *reconstructed;

        if (!shards[missing_idx]) {
            shards[missing_idx] = kmalloc(shard_size, GFP_KERNEL);
            if (!shards[missing_idx]) {
                ret = -ENOMEM;
                kfree(sub_matrix);
                kfree(inv_matrix);
                goto out;
            }
        }
        reconstructed = shards[missing_idx];
        memset(reconstructed, 0, shard_size);

        for (i = 0; i < codec->data_shards; i++) {
            u8 coef = inv_row[i];
            u32 avail_shard = avail_indices[i];

            if (coef == 0 || !shards[avail_shard])
                continue;
            for (k = 0; k < shard_size; k++)
                reconstructed[k] ^= gf_mul(gf, shards[avail_shard][k], coef);
        }
    }

    kfree(sub_matrix);
    kfree(inv_matrix);

    /* 重建缺失的数据分片后, 重建缺失的校验分片 */
    for (i = codec->data_shards; i < codec->total_shards; i++) {
        if (!available[i]) {
            u8 *parity_shard;
            u8 *coef_row = codec->enc_matrix +
                           (i - codec->data_shards) * codec->data_shards;

            if (!shards[i]) {
                shards[i] = kmalloc(shard_size, GFP_KERNEL);
                if (!shards[i]) {
                    ret = -ENOMEM;
                    goto out;
                }
            }
            parity_shard = shards[i];
            memset(parity_shard, 0, shard_size);
            for (d = 0; d < codec->data_shards; d++) {
                u8 coef = coef_row[d];
                if (coef == 0 || !shards[d])
                    continue;
                for (k = 0; k < shard_size; k++)
                    parity_shard[k] ^= gf_mul(gf, shards[d][k], coef);
            }
        }
    }

    ret = 0;

out:
    kfree(avail_indices);
    kfree(missing_data);
    return ret;
}
EXPORT_SYMBOL_GPL(powerfs_ec_init);
EXPORT_SYMBOL_GPL(powerfs_ec_free);
EXPORT_SYMBOL_GPL(powerfs_ec_encode);
EXPORT_SYMBOL_GPL(powerfs_ec_decode);
EXPORT_SYMBOL_GPL(powerfs_ec_can_recover);

MODULE_DESCRIPTION("PowerFS kernel EC (Reed-Solomon) codec");
MODULE_LICENSE("GPL");
