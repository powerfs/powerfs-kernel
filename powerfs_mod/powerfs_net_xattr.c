/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_xattr.c - split from powerfs_net.c (mechanical refactor) */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/un.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/hashtable.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/inet.h>
#include <linux/statfs.h>
#include <linux/rbtree.h>
#include <linux/kref.h>
#include <linux/unaligned.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include "powerfs_ec.h"

#include <net/sock.h>
#include <net/tcp.h>
#include <net/net_namespace.h>
#include <net/inet_sock.h>

#include "powerfs_net.h"
#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_flow.h"

#include "powerfs_net_internal.h"

/* ===== Xattr TX: SetXattr / GetXattr / RemoveXattr / ListXattr =====
 *
 * 消息/字段值必须与 Rust powerfs-net MsgType/FieldId 枚举严格一致:
 *   MsgType::SetXattr    = 0x0038,
 *   MsgType::GetXattr    = 0x0039,
 *   MsgType::RemoveXattr = 0x003a,
 *   MsgType::ListXattr   = 0x003b.
 *   FieldId::ShardId     = 0x70, Ino = 0x07,
 *   FieldId::XattrKey    = 0xB3 (string), XattrValue = 0xB4 (bytes),
 *   FieldId::XattrKeys   = 0xBC (NUL-separated bytes list).
 *
 * 所有请求需要 ShardId + Ino: Filer 路由层通过 ShardId 哈希到 shard leader,
 * 忽略 Ino 本身 (xattr 与 inode 属同一 shard). Ino 传 0 也能工作, 但为了
 * 日志/调试和将来 Filer 端按 inode 聚合缓存, 这里总是显式传. */

/**
 * powerfs_net_setxattr - 设置/覆盖单个 xattr (Raft 持久化).
 *
 * Request TLV:  ShardId + Ino + XattrKey(string) + XattrValue(bytes)
 * Response TLV: Status only
 */
int powerfs_net_setxattr(__u64 shard_id, __u64 ino,
                         const char *name, size_t name_len,
                         const __u8 *value, size_t value_len)
{
    /* 编码请求 body: ShardId(8+8=16hdr+val) + Ino(16) +
     *                XattrKey(5hdr+name) + XattrValue(5hdr+value).
     * 留 1KB 安全裕量, xattr value 通常 <=64KB. */
    size_t body_cap = 64 + name_len + value_len;
    __u8 *body;
    struct powerfs_tlv_enc enc;
    int ret;

    if (!name || name_len == 0 || name_len > 255)
        return -EINVAL;
    if (value_len > 0 && !value)
        return -EINVAL;

    body = kmalloc(body_cap, GFP_KERNEL);
    if (!body)
        return -ENOMEM;

    powerfs_tlv_enc_init(&enc, body, body_cap);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SHARD_ID, shard_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_XATTR_KEY, name, name_len);
    if (value_len > 0)
        powerfs_tlv_enc_bytes(&enc, POWERFS_NET_FLD_XATTR_VALUE, value, value_len);
    else {
        /* 空值仍然要编码字段 (区分 "未设置" 和 "设置为空字符串").
         * tlv_enc_bytes(NULL, 0) 编码为 field(1) + len(4) = 5 字节头, data=0. */
        powerfs_tlv_enc_bytes(&enc, POWERFS_NET_FLD_XATTR_VALUE, value ? value : (const __u8 *)"", 0);
    }

    ret = powerfs_net_send_request(POWERFS_NET_MSG_SET_XATTR, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0, NULL, 0, NULL, 0,
                                    POWERFS_META_TIMEOUT_MS, NULL, NULL);
    kfree(body);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_setxattr);

/**
 * powerfs_net_getxattr - 查询单个 xattr 值.
 *
 * Request TLV:  ShardId + Ino + XattrKey(string)
 * Response TLV: XattrValue(bytes) 或 Status=NOT_FOUND
 *
 * 若 xattr 未设置 → 返回 -ENODATA (VFS xattr 标准语义).
 * 若 value_cap < 实际 value_len → *value_len_out = 实际 len, 返回 -ERANGE.
 */
int powerfs_net_getxattr(__u64 shard_id, __u64 ino,
                         const char *name, size_t name_len,
                         __u8 *value_out, size_t value_cap,
                         size_t *value_len_out)
{
    size_t body_cap = 64 + name_len;
    __u8 *body;
    struct powerfs_tlv_enc enc;
    /* 响应 body 期望一个 XattrValue bytes 字段, xattr value 通常 <=64KB. */
    __u8 resp_stack[4096];
    __u8 *resp_body = resp_stack;
    size_t resp_body_cap = sizeof(resp_stack);
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    const __u8 *raw_val = NULL;
    size_t raw_len = 0;
    int ret;

    if (!name || name_len == 0 || name_len > 255 || !value_len_out)
        return -EINVAL;

    *value_len_out = 0;

    body = kmalloc(body_cap, GFP_KERNEL);
    if (!body)
        return -ENOMEM;

    powerfs_tlv_enc_init(&enc, body, body_cap);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SHARD_ID, shard_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_XATTR_KEY, name, name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_GET_XATTR, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, resp_body_cap,
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    kfree(body);
    if (ret < 0)
        return ret;
    if (ret > 0) {
        /* STATUS_ERR_NOT_FOUND → -ENODATA (VFS xattr 语义, 区别于 lookup 的 ENOENT).
         * net_status_to_errno 默认把 NOT_FOUND → -ENOENT, 这里覆盖. */
        if ((__u16)ret == POWERFS_NET_STATUS_ERR_NOT_FOUND)
            return -ENODATA;
        return net_status_to_errno((__u16)ret);
    }

    /* 成功响应: 解析 XattrValue bytes. */
    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
    if (powerfs_tlv_dec_find_raw(&dec, POWERFS_NET_FLD_XATTR_VALUE,
                                 &raw_val, &raw_len) != 0) {
        /* 无 XattrValue 字段: Filer 端可能为空值但漏编码, 视为 0 长度. */
        raw_len = 0;
    }

    *value_len_out = raw_len;
    if (raw_len == 0)
        return 0;
    if (raw_len > value_cap)
        return -ERANGE;
    if (value_out)
        memcpy(value_out, raw_val, raw_len);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_getxattr);

/**
 * powerfs_net_removexattr - 删除单个 xattr (Raft 持久化).
 *
 * Request TLV:  ShardId + Ino + XattrKey(string)
 * Response TLV: Status only
 * 未设置 → 返回 -ENODATA.
 */
int powerfs_net_removexattr(__u64 shard_id, __u64 ino,
                            const char *name, size_t name_len)
{
    size_t body_cap = 64 + name_len;
    __u8 *body;
    struct powerfs_tlv_enc enc;
    int ret;

    if (!name || name_len == 0 || name_len > 255)
        return -EINVAL;

    body = kmalloc(body_cap, GFP_KERNEL);
    if (!body)
        return -ENOMEM;

    powerfs_tlv_enc_init(&enc, body, body_cap);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SHARD_ID, shard_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_XATTR_KEY, name, name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_REMOVE_XATTR, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0, NULL, 0, NULL, 0,
                                    POWERFS_META_TIMEOUT_MS, NULL, NULL);
    kfree(body);
    if (ret < 0)
        return ret;
    if (ret > 0) {
        if ((__u16)ret == POWERFS_NET_STATUS_ERR_NOT_FOUND)
            return -ENODATA;
        return net_status_to_errno((__u16)ret);
    }
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_removexattr);

/**
 * powerfs_net_listxattr - 枚举 inode 上所有 xattr 键名.
 *
 * Request TLV:  ShardId + Ino
 * Response TLV: XattrKeys (NUL-separated bytes) 或 empty body = 无 xattr.
 *
 * 若缓冲区不足 → *list_len_out = 实际字节数, 返回 -ERANGE (VFS listxattr
 * 语义: listxattr(fd, NULL, 0) probe 时传入 list_buf=NULL/list_cap=0,
 * 成功返回所需长度).
 */
int powerfs_net_listxattr(__u64 shard_id, __u64 ino,
                          char *list_buf, size_t list_cap,
                          size_t *list_len_out)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    __u8 resp_stack[512];    /* 小响应走栈 (典型场景 < 10 个 xattr 键 < 512B) */
    __u8 *resp_body = resp_stack;
    size_t resp_body_cap = sizeof(resp_stack);
    size_t resp_body_len = 0;
    __u8 *resp_heap = NULL;
    struct powerfs_tlv_dec dec;
    const __u8 *raw_keys = NULL;
    size_t raw_len = 0;
    int ret;

    if (!list_len_out)
        return -EINVAL;
    *list_len_out = 0;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SHARD_ID, shard_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_LIST_XATTR, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, resp_body_cap,
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    /* -ERANGE: 栈缓冲 512 不够, 响应实际长度 > 512 → 切换到堆缓冲重试 */
    if (ret == -ERANGE) {
        resp_body_cap = resp_body_len ? resp_body_len : 8192;
        if (resp_body_cap > 65536)  /* 上限 64KB (xattr 键不可能这么多) */
            resp_body_cap = 65536;
        resp_heap = kmalloc(resp_body_cap, GFP_KERNEL);
        if (!resp_heap)
            return -ENOMEM;
        resp_body = resp_heap;
        resp_body_len = 0;
        ret = powerfs_net_send_request(POWERFS_NET_MSG_LIST_XATTR, ino,
                                        body, powerfs_tlv_enc_len(&enc),
                                        NULL, 0,
                                        resp_body, resp_body_cap,
                                        NULL, 0, POWERFS_META_TIMEOUT_MS,
                                        &resp_body_len, NULL);
    }
    if (ret < 0) {
        kfree(resp_heap);
        return ret;
    }
    if (ret > 0) {
        kfree(resp_heap);
        return net_status_to_errno((__u16)ret);
    }

    if (resp_body_len == 0) {
        kfree(resp_heap);
        *list_len_out = 0;  /* 无 xattr */
        return 0;
    }

    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
    if (powerfs_tlv_dec_find_raw(&dec, POWERFS_NET_FLD_XATTR_KEYS,
                                 &raw_keys, &raw_len) != 0 || raw_len == 0) {
        kfree(resp_heap);
        *list_len_out = 0;  /* 字段缺失或空 → 无 xattr */
        return 0;
    }

    *list_len_out = raw_len;
    /* probe 语义: list_buf == NULL 或容量不足 → 返回 -ERANGE 让 VFS 重分配. */
    if (list_buf == NULL || list_cap == 0) {
        kfree(resp_heap);
        return -ERANGE;
    }
    if (raw_len > list_cap) {
        kfree(resp_heap);
        return -ERANGE;
    }
    memcpy(list_buf, raw_keys, raw_len);
    kfree(resp_heap);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_listxattr);

/**
 * powerfs_net_ping - 连接健康检查
 */
int powerfs_net_ping(void)
{
    int ret;

    if (!powerfs_net_is_connected())
        return -ENOTCONN;

    ret = powerfs_net_send_request(POWERFS_NET_MSG_PING, 0,
                                    NULL, 0, NULL, 0,
                                    NULL, 0, NULL, 0, 2000,
                                    NULL, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}

