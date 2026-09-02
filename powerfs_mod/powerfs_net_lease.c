/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_lease.c - split from powerfs_net.c (mechanical refactor) */

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

/*
 * powerfs_net_renew_lease - RangeLease 续约 (Phase 3)
 *
 * 直连 volume server 发送 RANGE_LEASE 续约请求.
 * TLV 编码: Ino(volume_id) + Inode + LeaseToken + LeaseDuration
 * 响应: LeaseDuration (新有效期, 秒) + LeaseEpoch (新 epoch)
 *
 * 续约超时设为 5s (POWERFS_META_TIMEOUT_MS): lease 续约属管理类操作,
 * 短超时快速失败, 由 lease_renew_work_func 决定是否重试.
 * 断连/超时返回负值, 调用方不更新 expire_jiffies, lease 自然过期后
 * 由 acquire 路径重新获取.
 */
int powerfs_net_renew_lease(__u64 volume_id, __u64 ino,
                            const char *token, size_t token_len,
                            unsigned long *new_expire_jiffies)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[128];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    __u32 lease_duration_sec = 0;
    int ret;

    if (!token || token_len == 0 || token_len >= 64)
        return -EINVAL;

    /* TLV 字段顺序与 FUSE 客户端 renew_lease 一致:
     *   LeaseToken + ClientId + LeaseDuration
     * volume server handle_renew_lease 按相同顺序读取.
     * 之前内核多发 Ino(volume_id) + InodeV2(ino) 在前, 导致 LeaseToken
     * 字段错位, renew 全部失败 (-EREMOTEIO). */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LEASE_TOKEN,
                           token, token_len);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID,
                           "kernel-client", strlen("kernel-client"));
    /* 请求续约有效期: POWERFS_LEASE_DURATION 转换为毫秒 (u64, 与 volume server
     * handle_renew_lease 的 next_u64(LeaseDuration) 一致). 之前用 u32 秒,
     * volume server next_u64 解析失败, 用默认 30000ms. */
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_LEASE_DURATION,
                        jiffies_to_msecs(POWERFS_LEASE_DURATION));

    ret = powerfs_net_send_to_volume(-1, volume_id,
                                      POWERFS_NET_MSG_RENEW_LEASE,
                                      body, powerfs_tlv_enc_len(&enc),
                                      NULL, 0,
                                      resp_body, sizeof(resp_body),
                                      NULL, 0,
                                      POWERFS_META_TIMEOUT_MS,
                                      &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    /* 解析响应: LeaseDuration (volume server 返回 u64 毫秒). */
    {
        __u64 duration_ms = 0;
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_LEASE_DURATION, &duration_ms);
        if (duration_ms > 0)
            lease_duration_sec = duration_ms / 1000;
        else
            lease_duration_sec = jiffies_to_msecs(POWERFS_LEASE_DURATION) / 1000;
    }

    if (new_expire_jiffies)
        *new_expire_jiffies = jiffies +
            msecs_to_jiffies(lease_duration_sec * 1000);

    return 0;
}

/*
 * powerfs_net_acquire_lease - RangeLease 获取 (Phase 3)
 *
 * 直连 volume server 发送 ACQUIRE_LEASE 请求.
 * TLV 编码: Ino(volume_id) + Inode + Offset(stripe_start) + Count(stripe_count) + LeaseDuration
 * 响应: LeaseToken + LeaseDuration + LeaseEpoch + ContentSize
 *
 * 超时设为 5s (POWERFS_META_TIMEOUT_MS): lease 获取属管理类操作.
 * 断连/超时返回负值, 调用方可重试或降级.
 */
int powerfs_net_acquire_lease(__u64 volume_id, __u64 ino,
                              __u64 stripe_start, __u64 stripe_count,
                              const char *client_id,
                              char *token_out, size_t *token_len_out,
                              __u64 *epoch_out, __u64 *content_size_out,
                              unsigned long *expire_jiffies_out)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[256];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    __u32 lease_duration_sec = 0;
    int ret;

    if (!token_out || !token_len_out || *token_len_out < 64)
        return -EINVAL;

    /* TLV 字段顺序必须与 FUSE 客户端 (volume_client.rs acquire_lease) 和
     * volume server (net_handler.rs handle_acquire_lease) 完全一致, 因为
     * TlvDecoder 是按顺序读取的 (next_field → 比对 FieldId → 不匹配则
     * pos 不推进 value, 导致后续全部错位).
     *
     * FUSE 客户端顺序: Ino(inode) + Offset + Limit + ClientId + Mode + LeaseDuration
     * volume server  顺序: Ino(inode) + Offset + Limit + ClientId + Mode + LeaseDuration
     *
     * 注意: Ino(0x07) 发送的是 inode (不是 volume_id), volume server 的
     * range_lease_mgr 按 inode 管理 lease, 不需要 volume_id.
     * 之前内核误发 Ino=volume_id + InodeV2=ino 两个字段, 导致:
     *   1) srv 把 volume_id 当成 inode 注册 lease
     *   2) 第二个字段 InodeV2(0x97) 与 srv 期望的 Offset(0x0E) 不匹配,
     *      pos 卡在 InodeV2 value 开头, 后续 Offset/Limit/ClientId/Mode
     *      全部错位解析失败 (exclusive 误解析为 false, client_id 误解析为空)
     *   3) write_needle 用真实 inode 验证 lease, 与注册的 volume_id 不匹配,
     *      报 "Lease holder mismatch".
     */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, stripe_start);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_LIMIT, stripe_count);
    if (client_id && client_id[0])
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID,
                               client_id, strlen(client_id));
    else
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID,
                               "kernel-client", strlen("kernel-client"));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_MODE, 1); /* exclusive */
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_LEASE_DURATION,
                        jiffies_to_msecs(POWERFS_LEASE_DURATION));

    ret = powerfs_net_send_to_volume(-1, volume_id,
                                      POWERFS_NET_MSG_ACQUIRE_LEASE,
                                      body, powerfs_tlv_enc_len(&enc),
                                      NULL, 0,
                                      resp_body, sizeof(resp_body),
                                      NULL, 0,
                                      POWERFS_META_TIMEOUT_MS,
                                      &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    /* 解析响应: LeaseId. volume server handle_acquire_lease 用
     * FieldId::LeaseId(0x40) 返回 token, 不是 FieldId::LeaseToken(0x80). */
    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
    ret = powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_LEASE_ID,
                                  token_out, *token_len_out);
    if (ret < 0) {
        pr_err("powerfs: acquire_lease: failed to parse token: %d\n", ret);
        return -EIO;
    }
    *token_len_out = strlen(token_out);

    /* 解析响应: LeaseDuration (volume server 返回 u64 毫秒) */
    {
        __u64 duration_ms = 0;
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_LEASE_DURATION, &duration_ms);
        if (duration_ms > 0)
            lease_duration_sec = duration_ms / 1000;
        else
            lease_duration_sec = jiffies_to_msecs(POWERFS_LEASE_DURATION) / 1000;
    }

    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
    if (epoch_out)
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_LEASE_EPOCH, epoch_out);

    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
    if (content_size_out)
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_CONTENT_SIZE,
                            content_size_out);

    if (expire_jiffies_out)
        *expire_jiffies_out = jiffies +
            msecs_to_jiffies(lease_duration_sec * 1000);

    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_acquire_lease);

/*
 * powerfs_net_release_lease - RangeLease 释放 (Phase 3)
 *
 * 直连 volume server 发送 RELEASE_LEASE 请求.
 * TLV 编码: Ino(volume_id) + Inode + LeaseToken + ClientId
 * 响应: 只看 status (无 body)
 */
int powerfs_net_release_lease(__u64 volume_id, __u64 ino,
                              const char *token, size_t token_len,
                              const char *client_id)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    size_t resp_body_len = 0;
    int ret;

    if (!token || token_len == 0 || token_len >= 64)
        return -EINVAL;

    /* TLV 字段顺序与 FUSE 客户端 release_lease_remote_with_token 一致:
     *   LeaseToken + ClientId
     * volume server handle_release_lease 按相同顺序读取.
     * 之前内核多发 Ino(volume_id) + InodeV2(ino) 在前, 导致 LeaseToken
     * 字段错位, release 失败. */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LEASE_TOKEN,
                           token, token_len);
    if (client_id && client_id[0])
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID,
                               client_id, strlen(client_id));
    else
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID,
                               "kernel-client", strlen("kernel-client"));

    ret = powerfs_net_send_to_volume(-1, volume_id,
                                      POWERFS_NET_MSG_RELEASE_LEASE,
                                      body, powerfs_tlv_enc_len(&enc),
                                      NULL, 0,
                                      NULL, 0, NULL, 0,
                                      POWERFS_META_TIMEOUT_MS,
                                      &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_release_lease);

/* ========================================================================
 * §13 Capability (Cap) model: 三条同步 RPC 实现
 * ========================================================================
 *
 * 消息值对齐 Rust MsgType 枚举 (powerfs-net/src/protocol.rs L685):
 *   CapOpenGrant  = 0x0091 (open() 时申请)
 *   CapRecallAck  = 0x0092 (flush 后 ACK)
 *   CapRelease    = 0x0093 (close 时释放)
 *
 * 路由: 全部走 powerfs_calc_shard_id(ino) → shard_leader_map → filer leader,
 * 非 leader 重定向由 send_request 内部自动处理 (REDIRECT → shard_route_update → 重试).
 *
 * ClientId 字符串: 内核态固定用 "powerfs-kernel-<pid>" 还是模块级常量?
 * 这里取调用方传入的 client_id 字符串 (与 FUSE 端一致, 模块级可在 sb_info
 * 内生成唯一 client_id 并由上层传入, 保持灵活).
 */

/* 内核内部分派 TLV 字符串解码: 安全版本.
 * 直接复用 decoder 提供的 dec_string / find_raw, 这里实现一个对 dec 顺序解析
 * token/capset/epoch/sn/duration 的小助手, 用于 CapOpenGrant 响应及
 * CapRelease HasUpgrade 分支. */
int decode_cap_grant_fields(struct powerfs_tlv_dec *dec,
                                    char *token_out, size_t *token_len_out,
                                    __u8 *cap_set_out, __u64 *epoch_out,
                                    __u64 *sn_out, __u64 *duration_ms_out)
{
    /* 字段按 Filer 服务端编码顺序:
     *   LeaseToken → CapSet → CapEpoch → CapSn → LeaseDuration(可选) */
    if (token_out) {
        /* decoder: 在 max_len-1 处填 '\0' (dec_string 实现保证) */
        size_t tlen = 0;
        const __u8 *raw = NULL;
        int rc = powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_LEASE_TOKEN,
                                          &raw, &tlen);
        if (rc == 0 && raw) {
            size_t cp = tlen;
            if (token_len_out && *token_len_out > 0)
                cp = min(tlen, *token_len_out - 1);
            if (cp > 0)
                memcpy(token_out, raw, cp);
            token_out[cp] = '\0';
            if (token_len_out)
                *token_len_out = tlen;
        } else if (token_len_out) {
            token_out[0] = '\0';
            *token_len_out = 0;
        }
    }
    if (cap_set_out)
        powerfs_tlv_dec_find_u8(dec, POWERFS_NET_FLD_CAP_SET, cap_set_out);
    if (epoch_out)
        powerfs_tlv_dec_find_u64(dec, POWERFS_NET_FLD_CAP_EPOCH, epoch_out);
    if (sn_out)
        powerfs_tlv_dec_find_u64(dec, POWERFS_NET_FLD_CAP_SN, sn_out);
    if (duration_ms_out)
        powerfs_tlv_dec_find_u64(dec, POWERFS_NET_FLD_LEASE_DURATION, duration_ms_out);
    return 0;
}

/*
 * powerfs_net_cap_open_grant - §13.3 open() 初始 cap 申请 (同步, 永不阻塞).
 *
 * 对齐 Filer handle_cap_open_grant (net_handler.rs L3146):
 *   - 请求 TLV: Ino + ClientId(string) + IsWriteOpen(u8)
 *   - 响应 TLV: LeaseToken + CapSet + CapEpoch + CapSn + LeaseDuration(ms)
 *
 * 返回: 0 成功, <0 错误. 网络层 STATUS_OK 以外的状态码转 errno.
 */
int powerfs_net_cap_open_grant(__u64 ino,
                               const char *client_id,
                               bool is_write_open,
                               char *lease_token_out, size_t *token_len_out,
                               __u8 *cap_set_out, __u64 *epoch_out,
                               __u64 *sn_out, __u64 *duration_ms_out)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 *resp_body;
    size_t resp_body_len = 0;
    size_t cid_len;
    struct powerfs_tlv_dec dec;
    int ret;

    if (!client_id)
        return -EINVAL;
    cid_len = strlen(client_id);
    if (cid_len == 0 || cid_len > 255)
        return -EINVAL;
    if (ino == 0)
        return -EINVAL;

    /* 输出可选, 但先初始化 (便于调用方判断失败时也有默认值) */
    if (lease_token_out)
        lease_token_out[0] = '\0';
    if (token_len_out)
        *token_len_out = 0;
    if (cap_set_out)
        *cap_set_out = 0;
    if (epoch_out)
        *epoch_out = 0;
    if (sn_out)
        *sn_out = 0;
    if (duration_ms_out)
        *duration_ms_out = 0;

    resp_body = kvmalloc(POWERFS_NET_MAX_BODY, GFP_NOFS);
    if (!resp_body)
        return -ENOMEM;

    /* 编码请求: Ino + ClientId + IsWriteOpen */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID, client_id, cid_len);
    powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_IS_WRITE_OPEN,
                       is_write_open ? 1 : 0);

    /* cap_open_grant 属于元数据一致性类 RPC, 走 POWERFS_META_TIMEOUT_MS (5s),
     * 足够覆盖 Raft 选举 + 重连 + 3 次重试 (回忆 check_caps 不应 hung task). */
    ret = powerfs_net_send_request(POWERFS_NET_MSG_CAP_OPEN_GRANT, ino,
                                   body, powerfs_tlv_enc_len(&enc),
                                   NULL, 0,
                                   resp_body, POWERFS_NET_MAX_BODY,
                                   NULL, 0,
                                   POWERFS_META_TIMEOUT_MS,
                                   &resp_body_len, NULL);
    if (ret < 0)
        goto out;
    if (ret > 0) {
        /* 协议状态码: 按约定 open_grant 总是 STATUS_OK, 但防御式转 errno */
        ret = net_status_to_errno((__u16)ret);
        goto out;
    }

    /* 解码响应: TLV 字段顺序随意 (按 FieldId find). */
    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        decode_cap_grant_fields(&dec,
                                lease_token_out, token_len_out,
                                cap_set_out, epoch_out,
                                sn_out, duration_ms_out);
    }

    ret = 0;
out:
    kvfree(resp_body);
    return ret;
}
EXPORT_SYMBOL_GPL(powerfs_net_cap_open_grant);

/*
 * powerfs_net_cap_recall_ack - §13.4 向 Filer ACK: 已 flush 脏数据并降级.
 *
 * Request TLV:  Ino + ClientId(string) + LeaseToken(string)
 * Response TLV: 仅状态码 (STATUS_OK / ERR_SERVER / ERR_BAD_REQUEST).
 */
int powerfs_net_cap_recall_ack(__u64 ino,
                               const char *client_id,
                               const char *token, size_t token_len)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    size_t cid_len;
    int ret;

    if (!client_id || !token)
        return -EINVAL;
    cid_len = strlen(client_id);
    if (cid_len == 0 || cid_len > 255)
        return -EINVAL;
    if (token_len == 0 || token_len > 255)
        return -EINVAL;
    if (ino == 0)
        return -EINVAL;

    /* 编码请求: Ino + ClientId + LeaseToken */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID, client_id, cid_len);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LEASE_TOKEN, token, token_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_CAP_RECALL_ACK, ino,
                                   body, powerfs_tlv_enc_len(&enc),
                                   NULL, 0,
                                   NULL, 0, NULL, 0,
                                   POWERFS_META_TIMEOUT_MS,
                                   NULL, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_cap_recall_ack);

/*
 * powerfs_net_cap_release - §13.4 close 时主动释放 cap, 触发 upgrade 判定.
 *
 * Request TLV:  Ino + ClientId(string) + LeaseToken(string)
 * Response TLV:
 *   HasUpgrade(u8: 0/1)
 *   [if 1]: LeaseToken + CapSet + CapEpoch + CapSn (升级后的值)
 *
 * 注意: HasUpgrade=1 表示"本次 release 触发了一次 upgrade", 对应的 survivor
 * 客户端会额外收到 CapUpgradeNotify NOTIFY 推送 (异步通道).
 * 响应体内嵌升级信息是为了便于 release 请求的调用方 (survivor 自己) 立即
 * 拿到升级结果, 无需等到 NOTIFY 推送到达.
 */
int powerfs_net_cap_release(__u64 ino,
                            const char *client_id,
                            const char *token, size_t token_len,
                            __u8 *has_upgrade_out,
                            char *upgrade_token_out, size_t *upgrade_token_len_out,
                            __u8 *upgrade_cap_set_out,
                            __u64 *upgrade_epoch_out, __u64 *upgrade_sn_out)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 *resp_body;
    size_t resp_body_len = 0;
    size_t cid_len;
    struct powerfs_tlv_dec dec;
    __u8 has_upg = 0;
    int ret;

    if (!client_id || !token)
        return -EINVAL;
    cid_len = strlen(client_id);
    if (cid_len == 0 || cid_len > 255)
        return -EINVAL;
    if (token_len == 0 || token_len > 255)
        return -EINVAL;
    if (ino == 0)
        return -EINVAL;

    /* 初始化输出 */
    if (has_upgrade_out)
        *has_upgrade_out = 0;
    if (upgrade_token_out)
        upgrade_token_out[0] = '\0';
    if (upgrade_token_len_out)
        *upgrade_token_len_out = 0;
    if (upgrade_cap_set_out)
        *upgrade_cap_set_out = 0;
    if (upgrade_epoch_out)
        *upgrade_epoch_out = 0;
    if (upgrade_sn_out)
        *upgrade_sn_out = 0;

    resp_body = kvmalloc(POWERFS_NET_MAX_BODY, GFP_NOFS);
    if (!resp_body)
        return -ENOMEM;

    /* 编码请求: Ino + ClientId + LeaseToken */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID, client_id, cid_len);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LEASE_TOKEN, token, token_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_CAP_RELEASE, ino,
                                   body, powerfs_tlv_enc_len(&enc),
                                   NULL, 0,
                                   resp_body, POWERFS_NET_MAX_BODY,
                                   NULL, 0,
                                   POWERFS_META_TIMEOUT_MS,
                                   &resp_body_len, NULL);
    if (ret < 0)
        goto out;
    if (ret > 0) {
        ret = net_status_to_errno((__u16)ret);
        goto out;
    }

    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        /* HasUpgrade 是响应首个必字段 (服务端 always encode HasUpgrade
         * 即使为 0, 见 net_handler.rs L3348/L3362). */
        powerfs_tlv_dec_find_u8(&dec, POWERFS_NET_FLD_HAS_UPGRADE, &has_upg);
        if (has_upgrade_out)
            *has_upgrade_out = has_upg;

        if (has_upg) {
            /* 升级字段: LeaseToken + CapSet + CapEpoch + CapSn */
            decode_cap_grant_fields(&dec,
                                    upgrade_token_out, upgrade_token_len_out,
                                    upgrade_cap_set_out, upgrade_epoch_out,
                                    upgrade_sn_out, NULL);
        }
    }

    ret = 0;
out:
    kvfree(resp_body);
    return ret;
}
EXPORT_SYMBOL_GPL(powerfs_net_cap_release);

/*
 * powerfs_net_cap_acquire - §13.5 P0-1: 增量请求升级 cap (同步 RPC).
 *
 * 当客户端 wanted > issued 时 (如 write 打开但只有 FILE_SHARED, 需要 FILE_WR/EXCL),
 * 通过本 RPC 向 Filer 请求升级. Filer 内部走 lock_arbiter 的 wrlock/xlock →
 * 若有其他 holder 则触发 GATHER recall → 返回升级后的新 cap.
 *
 * 对齐 Filer handle_cap_acquire (net_handler.rs):
 *   - 请求 TLV: Ino + ClientId(string) + LeaseToken(string) + CapSet(wanted u8)
 *   - 响应 TLV: LeaseToken + CapSet(granted) + CapEpoch + CapSn + LeaseDuration
 *
 * 返回: 0 成功, <0 错误. 响应体与 cap_open_grant 共用 decode_cap_grant_fields.
 */
int powerfs_net_cap_acquire(__u64 ino,
                             const char *client_id,
                             const char *token, size_t token_len,
                             __u8 wanted_capset,
                             char *grant_token_out, size_t *grant_token_len_out,
                             __u8 *cap_set_out, __u64 *epoch_out,
                             __u64 *sn_out, __u64 *duration_ms_out)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 *resp_body;
    size_t resp_body_len = 0;
    size_t cid_len;
    struct powerfs_tlv_dec dec;
    int ret;

    if (!client_id || !token)
        return -EINVAL;
    cid_len = strlen(client_id);
    if (cid_len == 0 || cid_len > 255)
        return -EINVAL;
    if (token_len == 0 || token_len > 255)
        return -EINVAL;
    if (ino == 0)
        return -EINVAL;

    /* 初始化输出 */
    if (grant_token_out)
        grant_token_out[0] = '\0';
    if (grant_token_len_out)
        *grant_token_len_out = 0;
    if (cap_set_out)
        *cap_set_out = 0;
    if (epoch_out)
        *epoch_out = 0;
    if (sn_out)
        *sn_out = 0;
    if (duration_ms_out)
        *duration_ms_out = 0;

    resp_body = kvmalloc(POWERFS_NET_MAX_BODY, GFP_NOFS);
    if (!resp_body)
        return -ENOMEM;

    /* 编码请求: Ino + ClientId + LeaseToken + CapSet(wanted) */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID, client_id, cid_len);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LEASE_TOKEN, token, token_len);
    powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_CAP_SET, wanted_capset);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_CAP_ACQUIRE, ino,
                                   body, powerfs_tlv_enc_len(&enc),
                                   NULL, 0,
                                   resp_body, POWERFS_NET_MAX_BODY,
                                   NULL, 0,
                                   POWERFS_META_TIMEOUT_MS,
                                   &resp_body_len, NULL);
    if (ret < 0)
        goto out;
    if (ret > 0) {
        ret = net_status_to_errno((__u16)ret);
        goto out;
    }

    /* 解码响应: 与 cap_open_grant 共用 decode_cap_grant_fields */
    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        decode_cap_grant_fields(&dec,
                                grant_token_out, grant_token_len_out,
                                cap_set_out, epoch_out,
                                sn_out, duration_ms_out);
    }

    ret = 0;
out:
    kvfree(resp_body);
    return ret;
}
EXPORT_SYMBOL_GPL(powerfs_net_cap_acquire);

/* ========== §13 Cap NOTIFY dispatch (Filer→Client async push) ==========
 *
 * 回调注册点: 模块 init 时 fs 层注册 recall / upgrade handler.
 * 未注册时 NOTIFY 只打印 warning, 不 panic (防止模块早期阶段 crash).
 * 变量 g_cap_*_notify_fn 定义在 pfs_rx_dispatch 前 (文件 2128 行附近) 以便
 * RX dispatch 引用; 此处提供注册函数 (EXPORTed 供 fs 层调用). */

void powerfs_net_reg_cap_notify_handlers(
        powerfs_cap_recall_notify_fn recall_fn,
        powerfs_cap_upgrade_notify_fn upgrade_fn)
{
    /* smp 内存屏障: 注册后 RX 线程立即可见.
     * 典型调用时机: fill_super() (单线程, 但 barrier 安全无副作用). */
    smp_store_release(&g_cap_recall_notify_fn, recall_fn);
    smp_store_release(&g_cap_upgrade_notify_fn, upgrade_fn);
    pr_info("powerfs: cap notify handlers registered (%p/%p)\n",
            (void *)recall_fn, (void *)upgrade_fn);
}
EXPORT_SYMBOL_GPL(powerfs_net_reg_cap_notify_handlers);

/* 解析 CapRecallNotify body (服务端 net_handler.rs NetCapRevoker::recall 编码).
 *
 * 对齐 Rust net_handler.rs recall() 推送 (net_handler.rs ~L92-L118):
 *   FieldId::Ino             → ino (u64)
 *   FieldId::LeaseToken      → lease_token (string)
 *   FieldId::CapSet          → recall_mask (u8, 完整 8-bit recall capset)
 *   FieldId::IsWriteOpen(0xC6) → packed u8:
 *                                 bits[0:3] = recall_mask & 0x0F (冗余副本)
 *                                 bits[4:7] = retained_caps & 0x0F  ← 真正的 retain_mask
 *   FieldId::CapEpoch (0xC5) → epoch (u64)
 *
 * === BUG FIX (audit 2026-08-23, P1-S5a) ===
 * Previous decoder read FieldId::IsWriteOpen directly into retain_mask_out
 * without nibble-unpacking: for a recall=0x06 (W+X), retain=0x01 (R),
 * Rust encodes IsWriteOpen=0x16 = (recall_lo4=0x6)|(retain_lo4<<4=0x10).
 * Reading 0x16 directly into retain_mask gave wire_capset_to_kernel_bits(0x16)
 * = W+X (low 3 bits = 0x06) — exactly the bits being *recalled*, not retained.
 * The kernel then computed k_recall = issued & ~k_retain and revoked R instead
 * of W+X → client kept dirty WR_DATA/AUTH_EXCL, server never got recall_ack
 * for those bits → GATHER timeout → force-reclaim penalty applied.
 * Fix: mask packed byte, extract high nibble as retain_mask.
 */
int decode_cap_recall_body(const __u8 *body, size_t body_len,
                                  u64 *ino_out,
                                  char *token_out, size_t token_cap, size_t *token_len_out,
                                  __u8 *recall_mask_out, __u8 *retain_mask_out,
                                  __u64 *epoch_out)
{
    struct powerfs_tlv_dec dec;
    const __u8 *raw;
    size_t tlen;
    __u8 packed;

    if (!body || body_len == 0)
        return -EINVAL;

    powerfs_tlv_dec_init(&dec, body, body_len);
    powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_INO, ino_out);

    token_out[0] = '\0';
    if (token_len_out) *token_len_out = 0;
    if (powerfs_tlv_dec_find_raw(&dec, POWERFS_NET_FLD_LEASE_TOKEN, &raw, &tlen) == 0 &&
        raw && token_cap > 0) {
        size_t cp = min(tlen, token_cap - 1);
        memcpy(token_out, raw, cp);
        token_out[cp] = '\0';
        if (token_len_out) *token_len_out = tlen;
    }

    /* Recall mask: full 8-bit value from the first CapSet field. */
    powerfs_tlv_dec_find_u8(&dec, POWERFS_NET_FLD_CAP_SET, recall_mask_out);

    /* Retain mask: Rust packs (recall_lo4 | retain_lo4 << 4) into
     * FieldId::IsWriteOpen. We only care about the high nibble. */
    packed = 0;
    powerfs_tlv_dec_find_u8(&dec, POWERFS_NET_FLD_IS_WRITE_OPEN, &packed);
    *retain_mask_out = (packed >> 4) & 0x0F;

    powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_CAP_EPOCH, epoch_out);

    return 0;
}

/* 解析 CapUpgradeNotify body (服务端 net_handler.rs L3326-L3331:
 * Ino + LeaseToken + CapSet + CapEpoch + CapSn). */
int decode_cap_upgrade_body(const __u8 *body, size_t body_len,
                                   u64 *ino_out,
                                   char *token_out, size_t token_cap, size_t *token_len_out,
                                   __u8 *new_granted_out,
                                   __u64 *epoch_out, __u64 *sn_out)
{
    struct powerfs_tlv_dec dec;
    const __u8 *raw;
    size_t tlen;
    int rc;

    if (!body || body_len == 0)
        return -EINVAL;

    powerfs_tlv_dec_init(&dec, body, body_len);
    powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_INO, ino_out);

    token_out[0] = '\0';
    if (token_len_out) *token_len_out = 0;
    rc = powerfs_tlv_dec_find_raw(&dec, POWERFS_NET_FLD_LEASE_TOKEN, &raw, &tlen);
    if (rc == 0 && raw && token_cap > 0) {
        size_t cp = min(tlen, token_cap - 1);
        memcpy(token_out, raw, cp);
        token_out[cp] = '\0';
        if (token_len_out) *token_len_out = tlen;
    }

    powerfs_tlv_dec_find_u8(&dec, POWERFS_NET_FLD_CAP_SET, new_granted_out);
    powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_CAP_EPOCH, epoch_out);
    powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_CAP_SN, sn_out);
    return 0;
}

