/*
 * PowerFS TLV (Type-Length-Value) 编解码实现
 *
 * 从 Rust powerfs-net/src/serialize.rs 移植到 C 内核实现。
 *
 * TLV 格式:
 *   field_id (1B) | length (2B, little-endian) | value (length bytes)
 *
 * 这是简单紧凑的二进制序列化格式，支持字段添加和向后兼容。
 * 比 protobuf 更简单，适合元数据操作。
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "powerfs_net.h"

/* ========== TLV 编码器 ========== */

/**
 * powerfs_tlv_enc_init - 初始化 TLV 编码器
 *
 * @enc: 编码器上下文
 * @buf: 输出缓冲区
 * @cap: 缓冲区容量
 */
void powerfs_tlv_enc_init(struct powerfs_tlv_enc *enc, __u8 *buf, size_t cap)
{
    enc->buf = buf;
    enc->len = 0;
    enc->cap = cap;
}

/* 内部: 写入 TLV 头部 (field_id + length) */
static int powerfs_tlv_enc_write_hdr(struct powerfs_tlv_enc *enc,
                                      __u8 field, __u16 length)
{
    if (enc->len + 3 > enc->cap)
        return -ENOSPC;

    enc->buf[enc->len++] = field;
    enc->buf[enc->len++] = (__u8)(length & 0xFF);       /* length low byte */
    enc->buf[enc->len++] = (__u8)((length >> 8) & 0xFF); /* length high byte */
    return 0;
}

/**
 * powerfs_tlv_enc_u8 - 编码 uint8 字段
 */
int powerfs_tlv_enc_u8(struct powerfs_tlv_enc *enc, __u8 field, __u8 val)
{
    int ret;

    ret = powerfs_tlv_enc_write_hdr(enc, field, 1);
    if (ret < 0)
        return ret;

    if (enc->len + 1 > enc->cap)
        return -ENOSPC;

    enc->buf[enc->len++] = val;
    return 0;
}

/**
 * powerfs_tlv_enc_u16 - 编码 uint16 字段 (little-endian)
 */
int powerfs_tlv_enc_u16(struct powerfs_tlv_enc *enc, __u8 field, __u16 val)
{
    int ret;

    ret = powerfs_tlv_enc_write_hdr(enc, field, 2);
    if (ret < 0)
        return ret;

    if (enc->len + 2 > enc->cap)
        return -ENOSPC;

    /* little-endian */
    enc->buf[enc->len++] = (__u8)(val & 0xFF);
    enc->buf[enc->len++] = (__u8)((val >> 8) & 0xFF);
    return 0;
}

/**
 * powerfs_tlv_enc_u32 - 编码 uint32 字段 (little-endian)
 */
int powerfs_tlv_enc_u32(struct powerfs_tlv_enc *enc, __u8 field, __u32 val)
{
    int ret;
    __u8 *p;

    ret = powerfs_tlv_enc_write_hdr(enc, field, 4);
    if (ret < 0)
        return ret;

    if (enc->len + 4 > enc->cap)
        return -ENOSPC;

    p = &enc->buf[enc->len];
    /* little-endian */
    p[0] = (__u8)(val & 0xFF);
    p[1] = (__u8)((val >> 8) & 0xFF);
    p[2] = (__u8)((val >> 16) & 0xFF);
    p[3] = (__u8)((val >> 24) & 0xFF);
    enc->len += 4;
    return 0;
}

/**
 * powerfs_tlv_enc_u64 - 编码 uint64 字段 (little-endian)
 */
int powerfs_tlv_enc_u64(struct powerfs_tlv_enc *enc, __u8 field, __u64 val)
{
    int ret;
    __u8 *p;
    int i;

    ret = powerfs_tlv_enc_write_hdr(enc, field, 8);
    if (ret < 0)
        return ret;

    if (enc->len + 8 > enc->cap)
        return -ENOSPC;

    p = &enc->buf[enc->len];
    /* little-endian */
    for (i = 0; i < 8; i++)
        p[i] = (__u8)((val >> (i * 8)) & 0xFF);
    enc->len += 8;
    return 0;
}

/**
 * powerfs_tlv_enc_string - 编码字符串字段
 */
int powerfs_tlv_enc_string(struct powerfs_tlv_enc *enc, __u8 field,
                           const char *str, size_t len)
{
    int ret;

    if (len > POWERFS_NET_MAX_TLV)
        return -E2BIG;

    ret = powerfs_tlv_enc_write_hdr(enc, field, (__u16)len);
    if (ret < 0)
        return ret;

    if (enc->len + len > enc->cap)
        return -ENOSPC;

    if (len > 0 && str) {
        memcpy(&enc->buf[enc->len], str, len);
        enc->len += len;
    }
    return 0;
}

/**
 * powerfs_tlv_enc_bytes - 编码原始字节字段
 */
int powerfs_tlv_enc_bytes(struct powerfs_tlv_enc *enc, __u8 field,
                          const __u8 *data, size_t len)
{
    int ret;

    if (len > POWERFS_NET_MAX_TLV)
        return -E2BIG;

    ret = powerfs_tlv_enc_write_hdr(enc, field, (__u16)len);
    if (ret < 0)
        return ret;

    if (enc->len + len > enc->cap)
        return -ENOSPC;

    if (len > 0 && data) {
        memcpy(&enc->buf[enc->len], data, len);
        enc->len += len;
    }
    return 0;
}

/**
 * powerfs_tlv_enc_nested - 编码嵌套 TLV 字段
 *
 * 用于嵌入已编码的 TLV 数据 (如 readdir 中的目录项数组)
 */
int powerfs_tlv_enc_nested(struct powerfs_tlv_enc *enc, __u8 field,
                           const __u8 *data, size_t len)
{
    /* 嵌套数据也有长度限制 */
    if (len > POWERFS_NET_MAX_TLV)
        return -E2BIG;

    return powerfs_tlv_enc_bytes(enc, field, data, len);
}

/**
 * powerfs_tlv_enc_len - 获取编码器当前数据长度
 */
size_t powerfs_tlv_enc_len(const struct powerfs_tlv_enc *enc)
{
    return enc->len;
}

/* ========== TLV 解码器 ========== */

/**
 * powerfs_tlv_dec_init - 初始化 TLV 解码器
 */
void powerfs_tlv_dec_init(struct powerfs_tlv_dec *dec, const __u8 *buf,
                           size_t len)
{
    dec->buf = buf;
    dec->len = len;
    dec->pos = 0;
}

/**
 * powerfs_tlv_dec_next - 读取下一个 TLV 字段的 field_id 和 length
 *
 * 返回: 0 成功, -ENOENT 无更多字段, -EINVAL 格式错误
 */
int powerfs_tlv_dec_next(struct powerfs_tlv_dec *dec, __u8 *field,
                          size_t *length)
{
    if (dec->pos + 3 > dec->len)
        return -ENOENT;

    *field = dec->buf[dec->pos];
    dec->pos++;

    /* little-endian length */
    *length = (size_t)dec->buf[dec->pos] |
              ((size_t)dec->buf[dec->pos + 1] << 8);
    dec->pos += 2;

    /* 检查是否超出缓冲区 */
    if (dec->pos + *length > dec->len)
        return -EINVAL;

    return 0;
}

/* 内部: 读取当前字段的数据并推进位置 */
static const __u8 *powerfs_tlv_dec_read(struct powerfs_tlv_dec *dec,
                                          __u8 expected_field, size_t expected_len,
                                          __u8 *actual_field, size_t *actual_len)
{
    __u8 field;
    size_t length;
    int ret;

    /* 先 peek at current field */
    if (dec->pos + 3 > dec->len)
        return NULL;

    field = dec->buf[dec->pos];
    length = (size_t)dec->buf[dec->pos + 1] |
             ((size_t)dec->buf[dec->pos + 2] << 8);

    /* 检查长度 */
    if (dec->pos + 3 + length > dec->len)
        return NULL;

    if (actual_field)
        *actual_field = field;
    if (actual_len)
        *actual_len = length;

    /* 跳过 TLV 头部 (3 字节) */
    dec->pos += 3;

    /* 如果字段类型不匹配，跳过数据 */
    if (field != expected_field) {
        dec->pos += length;
        return NULL;
    }

    /* 长度必须匹配 (或者 expected_len=0 表示接受任意长度) */
    if (expected_len > 0 && length != expected_len) {
        dec->pos += length;
        return NULL;
    }

    /* 返回数据指针 (不推进 dec->pos, 由调用者决定) */
    return &dec->buf[dec->pos];
}

/**
 * powerfs_tlv_dec_u8 - 解码 uint8 字段
 */
int powerfs_tlv_dec_u8(struct powerfs_tlv_dec *dec, __u8 field, __u8 *val)
{
    const __u8 *data;

    data = powerfs_tlv_dec_read(dec, field, 1, NULL, NULL);
    if (!data)
        return -ENOENT;

    *val = *data;
    dec->pos += 1;
    return 0;
}

/**
 * powerfs_tlv_dec_u16 - 解码 uint16 字段 (little-endian)
 */
int powerfs_tlv_dec_u16(struct powerfs_tlv_dec *dec, __u8 field, __u16 *val)
{
    const __u8 *data;

    data = powerfs_tlv_dec_read(dec, field, 2, NULL, NULL);
    if (!data)
        return -ENOENT;

    *val = (__u16)data[0] | ((__u16)data[1] << 8);
    dec->pos += 2;
    return 0;
}

/**
 * powerfs_tlv_dec_u32 - 解码 uint32 字段 (little-endian)
 */
int powerfs_tlv_dec_u32(struct powerfs_tlv_dec *dec, __u8 field, __u32 *val)
{
    const __u8 *data;

    data = powerfs_tlv_dec_read(dec, field, 4, NULL, NULL);
    if (!data)
        return -ENOENT;

    *val = (__u32)data[0] |
           ((__u32)data[1] << 8) |
           ((__u32)data[2] << 16) |
           ((__u32)data[3] << 24);
    dec->pos += 4;
    return 0;
}

/**
 * powerfs_tlv_dec_u64 - 解码 uint64 字段 (little-endian)
 */
int powerfs_tlv_dec_u64(struct powerfs_tlv_dec *dec, __u8 field, __u64 *val)
{
    const __u8 *data;
    int i;

    data = powerfs_tlv_dec_read(dec, field, 8, NULL, NULL);
    if (!data)
        return -ENOENT;

    *val = 0;
    for (i = 0; i < 8; i++)
        *val |= (__u64)data[i] << (i * 8);
    dec->pos += 8;
    return 0;
}

/**
 * powerfs_tlv_dec_string - 解码字符串字段
 *
 * @dec: 解码器
 * @field: 期望的字段 ID
 * @str: 输出字符串缓冲区
 * @max_len: 缓冲区大小
 *
 * 返回: 0 成功, -ENOENT 未找到, -E2BIG 缓冲区不足
 */
int powerfs_tlv_dec_string(struct powerfs_tlv_dec *dec, __u8 field,
                            char *str, size_t max_len)
{
    const __u8 *data;
    __u8 actual_field;
    size_t actual_len;

    /*
     * 注意: 对于字符串，长度不固定
     * 使用 expected_len=0 表示接受任意长度
     */
    data = powerfs_tlv_dec_read(dec, field, 0, &actual_field, &actual_len);
    if (!data || actual_field != field)
        return -ENOENT;

    if (actual_len > max_len) {
        /* 跳过数据并报错 */
        dec->pos += actual_len;
        return -E2BIG;
    }

    if (actual_len > 0) {
        memcpy(str, data, actual_len);
        str[actual_len] = '\0';  /* null terminate */
    } else {
        str[0] = '\0';
    }
    dec->pos += actual_len;
    return 0;
}

/**
 * powerfs_tlv_dec_skip - 跳过指定长度的数据
 *
 * 用于跳过已读取但不关心的字段数据
 */
int powerfs_tlv_dec_skip(struct powerfs_tlv_dec *dec, size_t length)
{
    if (dec->pos + length > dec->len)
        return -EINVAL;

    dec->pos += length;
    return 0;
}

/**
 * powerfs_tlv_dec_is_empty - 解码器是否已消耗完所有数据
 */
bool powerfs_tlv_dec_is_empty(const struct powerfs_tlv_dec *dec)
{
    return dec->pos >= dec->len;
}

/* ========== 导出符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_tlv_enc_init);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_u8);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_u16);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_u32);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_u64);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_string);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_bytes);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_nested);
EXPORT_SYMBOL_GPL(powerfs_tlv_enc_len);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_init);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_next);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_u8);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_u16);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_u32);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_u64);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_string);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_skip);
EXPORT_SYMBOL_GPL(powerfs_tlv_dec_is_empty);
