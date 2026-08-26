/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_inode.c - split from powerfs_net.c (mechanical refactor) */

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

/* ========== 便捷方法 ========== */

/**
 * powerfs_net_lookup - 查找文件 (返回完整属性含时间戳 + volume_id/file_key)
 *
 * Filer 响应 TLV 字段: Ino, Mode, Uid, Gid, Size, Nlink, Mtime, Atime, Ctime,
 *                      Name, VolumeId, FileKey (via encode_chunks_fields)
 *
 * volume_id/file_key 用于数据直连 Volume Server (WriteNeedle/ReadNeedle).
 * 目录的 volume_id/file_key 为 0 (目录无数据).
 */
int powerfs_net_lookup_timeout(__u64 dir_ino, const char *name, size_t name_len,
                               __u64 *ino, __u32 *mode, __u32 *uid, __u32 *gid,
                               __u64 *size, __u32 *nlink,
                               __u64 *mtime, __u64 *atime, __u64 *ctime,
                               __u64 *volume_id, __u64 *file_key,
                               struct powerfs_file_layout *layout,
                               int timeout_ms)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 *resp_body;
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    /* K2: Inline 文件的 LOOKUP 响应携带 inline_data (最大 8KB),
     * 栈上 512B 缓冲区会被截断 (RX_TRUNCATE → -E2BIG). 用 kvmalloc 动态分配. */
    resp_body = kvmalloc(POWERFS_NET_RESP_INLINE_CAP, GFP_NOFS);
    if (!resp_body)
        return -ENOMEM;

    /* 编码请求: ParentIno + Name */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_LOOKUP, dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, POWERFS_NET_RESP_INLINE_CAP,
                                    NULL, 0, timeout_ms,
                                    &resp_body_len, NULL);
    if (ret < 0)
        goto out;
    if (ret > 0) {
        ret = net_status_to_errno((__u16)ret);
        goto out;
    }

    /* 解码响应 */
    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_INO, ino);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_MODE, mode);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_UID, uid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_GID, gid);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, size);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_NLINK, nlink);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_MTIME, mtime);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_ATIME, atime);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_CTIME, ctime);
        /* 数据直连: 使用 find 方式解析 volume_id/file_key, 不依赖字段顺序.
         * Filer lookup 响应在 CTIME 后还有 Name, Chunks(JSON), Fid, VolumeId,
         * Cookie, FileKey 等字段, 顺序解析会因 Name/Chunks 等中间字段而失败. */
        if (volume_id)
            powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_VOLUME_ID, volume_id);
        if (file_key)
            powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_FILE_KEY, file_key);

        /* K3: 解析 FileLayout (placement/volume_ids 等).
         * Filer encode_chunks_fields 对 Stripe 文件编码 Placement::Stripe +
         * VolumeIds, 内核需在 lookup 时解析以正确路由 read/write. */
        if (layout)
            parse_file_layout(&dec, layout);
    }

    ret = 0;
out:
    kvfree(resp_body);
    return ret;
}

/* 兼容 wrapper: 用默认 10s 超时 (POWERFS_NET_RECV_TIMEOUT). */
int powerfs_net_lookup(__u64 dir_ino, const char *name, size_t name_len,
                       __u64 *ino, __u32 *mode, __u32 *uid, __u32 *gid,
                       __u64 *size, __u32 *nlink,
                       __u64 *mtime, __u64 *atime, __u64 *ctime,
                       __u64 *volume_id, __u64 *file_key,
                       struct powerfs_file_layout *layout)
{
    return powerfs_net_lookup_timeout(dir_ino, name, name_len,
                                      ino, mode, uid, gid,
                                      size, nlink,
                                      mtime, atime, ctime,
                                      volume_id, file_key, layout,
                                      POWERFS_NET_RECV_TIMEOUT);
}

/**
 * powerfs_net_getattr - 获取文件属性 (返回完整属性含时间戳 + volume_id/file_key)
 *
 * Filer 响应 TLV 字段: Ino, Mode, Uid, Gid, Size, Nlink, Mtime, Atime, Ctime,
 *                      VolumeId, FileKey, Placement, Reliability, ReliabilityState, ChunkSize
 *
 * volume_id/file_key 用于数据直连 Volume Server (WriteNeedle/ReadNeedle).
 * layout 携带 FileLayout 元数据 (placement/reliability/chunk_size), 可为 NULL.
 */

/* 解析 Placement TLV (0xA0): u8 tag + 后续字段.
 *   0x00=Inline(5B) 0x01=Flat(1B) 0x02=Stripe(17B) 0x03=WideStripe(17B)
 * 对齐 powerfs-layout/src/codec.rs placement_tag (L52) + encode_placement (L233)
 *
 * 注意: Placement 字段本身只携带 tag + (Inline:max_size | Stripe:三字段).
 * volume_ids 列表通过独立 FieldId::VolumeIds(0xAB) / VolumeIdsRange(0xB6)
 * 传输, 由 parse_file_layout() 单独解析. */
int parse_placement_field(const __u8 *val, size_t len,
                                 struct powerfs_file_layout *layout)
{
    if (len < 1)
        return -EINVAL;

    layout->placement = val[0];
    layout->has_placement = true;

    switch (val[0]) {
    case POWERFS_PLACEMENT_FLAT:
        break;
    case POWERFS_PLACEMENT_INLINE:
        /* max_size: u32 LE, 紧跟 tag */
        if (len >= 5)
            layout->inline_max_size = le32_to_cpup((__le32 *)&val[1]);
        break;
    case POWERFS_PLACEMENT_STRIPE:
    case POWERFS_PLACEMENT_WIDESTRIPE:
        /* stripe_size(8B) + stripe_count(4B) + start_volume_idx(4B) = 16B after tag.
         * 对齐 codec.rs encode_placement Stripe 分支. */
        if (len >= 17) {
            layout->stripe_size = le64_to_cpup((__le64 *)&val[1]);
            layout->stripe_count = le32_to_cpup((__le32 *)&val[9]);
            layout->start_volume_idx = le32_to_cpup((__le32 *)&val[13]);
        } else {
            pr_warn("powerfs: Stripe placement truncated len=%zu\n", len);
        }
        break;
    default:
        pr_warn("powerfs: unknown placement tag %u\n", val[0]);
        layout->placement = POWERFS_PLACEMENT_FLAT;
        break;
    }
    return 0;
}

/* 解析 Reliability TLV (0xA1): u8 tag + count/shards.
 *   0x00=Single(1B) 0x01=Replicated(5B) 0x02=EC(9B)
 * 对齐 powerfs-layout/src/codec.rs decode_reliability (L344).
 *
 * K4-8: EC 分支解析 data_shards/parity_shards. */
int parse_reliability_field(const __u8 *val, size_t len,
                                   struct powerfs_file_layout *layout)
{
    if (len < 1)
        return -EINVAL;

    layout->reliability = val[0];
    layout->has_reliability = true;

    /* K4-8: 解析 EC data_shards/parity_shards */
    if (val[0] == POWERFS_RELIABILITY_EC && len >= 9) {
        __u32 data, parity;
        memcpy(&data, val + 1, sizeof(__u32));
        memcpy(&parity, val + 5, sizeof(__u32));
        layout->ec_data_shards = le32_to_cpu(data);
        layout->ec_parity_shards = le32_to_cpu(parity);
        pr_debug("powerfs: parse_reliability EC data=%u parity=%u\n",
                 layout->ec_data_shards, layout->ec_parity_shards);
    }

    return 0;
}

/* K4-2: 解析 ReplicaChunks TLV (0xB5): [count u32 LE][ChunkRef × count].
 * 每个 ChunkRef 44 字节: offset(u64) + size(u64) + needle_id(u64) +
 * volume_id(u64) + crc32(u32) + mtime(u64), 全部小端.
 * 对齐 powerfs-layout codec.rs decode_chunk_list (L584). */
int parse_replica_chunks_field(const __u8 *val, size_t len,
                                      struct powerfs_file_layout *layout)
{
    __u32 count, i;
    const __u8 *p;
    struct powerfs_chunk_map *chunks;

    if (len < 4)
        return -EINVAL;

    memcpy(&count, val, sizeof(__u32));
    count = le32_to_cpu(count);

    if (count == 0) {
        layout->replica_chunks = NULL;
        layout->replica_count = 0;
        layout->has_replica_chunks = true;
        return 0;
    }

    /* 每个 ChunkRef 44 字节 */
    if (len < 4 + (size_t)count * 44)
        return -EINVAL;

    chunks = kmalloc_array(count, sizeof(struct powerfs_chunk_map),
                           GFP_KERNEL);
    if (!chunks)
        return -ENOMEM;

    p = val + 4;
    for (i = 0; i < count; i++) {
        __u64 offset, size, needle_id, volume_id, mtime;
        __u32 crc32;

        memcpy(&offset, p + 0, 8);
        memcpy(&size, p + 8, 8);
        memcpy(&needle_id, p + 16, 8);
        memcpy(&volume_id, p + 24, 8);
        memcpy(&crc32, p + 32, 4);
        memcpy(&mtime, p + 36, 8);

        chunks[i].chunk_idx = le64_to_cpu(offset) / POWERFS_CHUNK_SIZE;
        chunks[i].needle_id = le64_to_cpu(needle_id);
        chunks[i].volume_id = le64_to_cpu(volume_id);
        chunks[i].crc32 = le32_to_cpu(crc32);
        chunks[i].size = le64_to_cpu(size);

        p += 44;
    }

    layout->replica_chunks = chunks;
    layout->replica_count = count;
    layout->has_replica_chunks = true;
    pr_debug("powerfs: parse_replica_chunks count=%u\n", count);
    return 0;
}

/* 从 TLV 响应解析 FileLayout 字段 (Placement/Reliability/ReliabilityState/ChunkSize).
 * 在 GETATTR/CREATE 响应解析后调用, 使用 find_* 非顺序查找.
 *
 * K3: 额外解析 Stripe 字段:
 *   - StripeSize (0xA8) / StripeCount (0xA9) / StartVolumeIdx (0xAA)
 *     (Placement 字段已携带, 独立字段作为兜底)
 *   - VolumeIds (0xAB): u64 LE 数组
 *   - VolumeIdsRange (0xB6): start_u64 + count_u32 = 12B 范围压缩
 *   - StartNeedleId (0xAC): StripeDescriptor 首 needle (K3-5 预留)
 *
 * volume_ids 解析后通过 layout->volume_ids 返回 (kmalloc), 调用方负责:
 *   - apply 到 inode: powerfs_apply_layout_to_inode (所有权转移)
 *   - 或失败时 kfree(layout.volume_ids) 防止泄漏 */
void parse_file_layout(struct powerfs_tlv_dec *dec,
                              struct powerfs_file_layout *layout)
{
    const __u8 *raw;
    size_t raw_len;
    __u8 u8val;
    __u32 u32val;
    __u64 u64val;

    if (!layout)
        return;

    memset(layout, 0, sizeof(*layout));
    layout->chunk_size = POWERFS_CHUNK_SIZE;  /* 默认值 */

    /* Placement (0xA0) — 二进制 tag + 后续 (Stripe 携带三字段) */
    if (powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_PLACEMENT, &raw, &raw_len) == 0)
        parse_placement_field(raw, raw_len, layout);

    /* Reliability (0xA1) — 二进制 tag + count */
    if (powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_RELIABILITY, &raw, &raw_len) == 0)
        parse_reliability_field(raw, raw_len, layout);

    /* ReliabilityState (0xA2) — u8 */
    if (powerfs_tlv_dec_find_u8(dec, POWERFS_NET_FLD_RELIABILITY_STATE, &u8val) == 0)
        layout->reliability_state = u8val;

    /* ChunkSize (0xAD) — u32, 覆盖默认值 */
    if (powerfs_tlv_dec_find_u32(dec, POWERFS_NET_FLD_CHUNK_SIZE, &u32val) == 0 && u32val > 0)
        layout->chunk_size = u32val;

    /* InlineMaxSize (0xAF) — u32, 可能从 Placement tag 已解析 */
    if (layout->inline_max_size == 0 &&
        powerfs_tlv_dec_find_u32(dec, POWERFS_NET_FLD_INLINE_MAX_SIZE, &u32val) == 0)
        layout->inline_max_size = u32val;

    /* === K2: InlineData (0xAE) — raw bytes, <=8KB ===
     * Filer 在 Inline 模式下将文件数据编码在此字段, 客户端无需走 Volume RPC.
     * 对齐 codec.rs decode_file_layout InlineData 分支.
     * 安全检查: raw_len <= POWERFS_INLINE_MAX_SIZE (8KB), 防止异常大值 OOM. */
    if (powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_INLINE_DATA, &raw, &raw_len) == 0) {
        if (raw_len > 0 && raw_len <= POWERFS_INLINE_MAX_SIZE) {
            /* 释放可能已存在的 (重复解析或 Placement tag 已携带) */
            kfree(layout->inline_data);
            layout->inline_data = kmalloc(raw_len, GFP_KERNEL);
            if (layout->inline_data) {
                memcpy(layout->inline_data, raw, raw_len);
                layout->inline_len = raw_len;
                layout->has_inline_data = true;
            } else {
                pr_warn("powerfs: InlineData kmalloc %zu failed\n", raw_len);
            }
        } else if (raw_len > POWERFS_INLINE_MAX_SIZE) {
            pr_warn("powerfs: InlineData len %zu > %d, ignored\n",
                    raw_len, POWERFS_INLINE_MAX_SIZE);
        }
    }

    /* === K2: ChunkLayout (0xA4) — Filer 通过 encode_encoding 编码 InlineData ===
     * Filer 的 encode_file_layout 将 InlineData 编码在 ChunkLayout 字段中:
     *   [encoding_tag::INLINE_DATA(0x00)] [data_len u32 LE] [data...]
     * 内核需从此字段提取 inline_data (对齐 codec.rs decode_file_layout ChunkLayout 分支).
     * 仅当 InlineData (0xAE) 字段不存在时, 才从 ChunkLayout 提取 (0xAE 优先). */
    if (!layout->has_inline_data &&
        powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_CHUNK_LAYOUT, &raw, &raw_len) == 0) {
        /* ChunkLayout 格式: [tag u8] [变体数据...]
         * tag=0x00 (INLINE_DATA): [0x00] [len u32 LE] [data]
         * tag=0x01 (PER_CHUNK):   [0x01] [count u32 LE] [ChunkRef...] */
        if (raw_len >= 5 && raw[0] == 0x00) {
            __u32 data_len = le32_to_cpup((__le32 *)&raw[1]);
            if (data_len > 0 && data_len <= POWERFS_INLINE_MAX_SIZE &&
                raw_len >= 5 + data_len) {
                kfree(layout->inline_data);
                layout->inline_data = kmalloc(data_len, GFP_KERNEL);
                if (layout->inline_data) {
                    memcpy(layout->inline_data, raw + 5, data_len);
                    layout->inline_len = data_len;
                    layout->has_inline_data = true;
                    pr_info("powerfs: parse_file_layout ChunkLayout InlineData len=%u\n",
                            data_len);
                } else {
                    pr_warn("powerfs: ChunkLayout InlineData kmalloc %u failed\n",
                            data_len);
                }
            }
        } else if (raw_len >= 5 && raw[0] == 0x01) {
            /* K4-5: PER_CHUNK tag=0x01 — EC shards 列表.
             * [0x01] [count u32 LE] [ChunkRef × count]
             * 每个 ChunkRef 44 字节, 与 ReplicaChunks 格式相同.
             * 对齐 FUSE fuse.rs L2465-2473 ec_chunks 读取. */
            __u32 count = le32_to_cpup((__le32 *)&raw[1]);
            if (count > 0 && raw_len >= 5 + (size_t)count * 44) {
                struct powerfs_chunk_map *chunks;
                const __u8 *p = raw + 5;
                __u32 i;

                chunks = kmalloc_array(count,
                                       sizeof(struct powerfs_chunk_map),
                                       GFP_KERNEL);
                if (chunks) {
                    for (i = 0; i < count; i++) {
                        __u64 offset, size, needle_id, volume_id, mtime;
                        __u32 crc32;

                        memcpy(&offset, p + 0, 8);
                        memcpy(&size, p + 8, 8);
                        memcpy(&needle_id, p + 16, 8);
                        memcpy(&volume_id, p + 24, 8);
                        memcpy(&crc32, p + 32, 4);
                        memcpy(&mtime, p + 36, 8);

                        chunks[i].chunk_idx = le64_to_cpu(offset) / POWERFS_CHUNK_SIZE;
                        chunks[i].needle_id = le64_to_cpu(needle_id);
                        chunks[i].volume_id = le64_to_cpu(volume_id);
                        chunks[i].crc32 = le32_to_cpu(crc32);
                        chunks[i].size = le64_to_cpu(size);
                        p += 44;
                    }
                    kfree(layout->ec_chunks);
                    layout->ec_chunks = chunks;
                    layout->ec_chunk_count = count;
                    layout->has_ec_chunks = true;
                    pr_info("powerfs: parse_file_layout ChunkLayout PER_CHUNK count=%u\n",
                            count);
                } else {
                    pr_warn("powerfs: ChunkLayout PER_CHUNK kmalloc %u failed\n",
                            count);
                }
            }
        }
    }

    /* === K3: Stripe 字段 (独立 FieldId 兜底, Placement 字段优先) === */

    /* StripeSize (0xA8) — u64, Placement 字段已携带时跳过 */
    if (layout->stripe_size == 0 &&
        powerfs_tlv_dec_find_u64(dec, POWERFS_NET_FLD_STRIPE_SIZE, &u64val) == 0)
        layout->stripe_size = u64val;

    /* StripeCount (0xA9) — u32, Placement 字段已携带时跳过 */
    if (layout->stripe_count == 0 &&
        powerfs_tlv_dec_find_u32(dec, POWERFS_NET_FLD_STRIPE_COUNT, &u32val) == 0)
        layout->stripe_count = u32val;

    /* StartVolumeIdx (0xAA) — u32, Placement 字段已携带时跳过 */
    if (layout->start_volume_idx == 0 &&
        powerfs_tlv_dec_find_u32(dec, POWERFS_NET_FLD_START_VOLUME_IDX, &u32val) == 0)
        layout->start_volume_idx = u32val;

    /* StartNeedleId (0xAC) — u64, StripeDescriptor 首 needle (K3-5 预留) */
    if (powerfs_tlv_dec_find_u64(dec, POWERFS_NET_FLD_START_NEEDLE_ID, &u64val) == 0)
        layout->start_needle_id = u64val;

    /* VolumeIds (0xAB) — u64 LE 数组. 对齐 codec.rs decode_volume_ids.
     * 仅 Stripe/WideStripe 模式下有意义, 但解析不区分 placement
     * (调用方 apply 时根据 placement 决定是否使用). */
    if (powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_VOLUME_IDS, &raw, &raw_len) == 0) {
        if (raw_len > 0 && (raw_len % 8) == 0) {
            u32 cnt = raw_len / 8;
            /* 限制 count 防止异常大值导致 OOM (256 卷 WideStripe 上限) */
            if (cnt <= 256) {
                u64 *vids = kmalloc_array(cnt, sizeof(u64), GFP_KERNEL);
                if (vids) {
                    u32 i;
                    for (i = 0; i < cnt; i++)
                        vids[i] = le64_to_cpup((__le64 *)&raw[i * 8]);
                    /* 释放可能已存在的 (VolumeIdsRange 先解析的情况) */
                    kfree(layout->volume_ids);
                    layout->volume_ids = vids;
                    layout->volume_ids_count = cnt;
                }
            } else {
                pr_warn("powerfs: VolumeIds count %u > 256, ignored\n", cnt);
            }
        }
    }

    /* VolumeIdsRange (0xB6) — start_u64 + count_u32 = 12B 范围压缩.
     * 对齐 codec.rs decode_file_layout VolumeIdsRange 分支.
     * 仅在 VolumeIds (0xAB) 未解析时使用 (0xAB 优先, 更精确). */
    if (!layout->volume_ids &&
        powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_VOLUME_IDS_RANGE, &raw, &raw_len) == 0) {
        if (raw_len == 12) {
            u64 start = le64_to_cpup((__le64 *)&raw[0]);
            u32 cnt = le32_to_cpup((__le32 *)&raw[8]);
            if (cnt > 0 && cnt <= 256) {
                u64 *vids = kmalloc_array(cnt, sizeof(u64), GFP_KERNEL);
                if (vids) {
                    u32 i;
                    for (i = 0; i < cnt; i++)
                        vids[i] = start + i;
                    layout->volume_ids = vids;
                    layout->volume_ids_count = cnt;
                }
            } else if (cnt > 256) {
                pr_warn("powerfs: VolumeIdsRange count %u > 256, ignored\n", cnt);
            }
        } else {
            pr_warn("powerfs: VolumeIdsRange len %zu != 12\n", raw_len);
        }
    }

    /* K4-2: ReplicaChunks (0xB5) — [count u32 LE][ChunkRef × count].
     * 用于读路径 failover: 主 volume 失败时从副本重读.
     * parse 阶段 kmalloc, apply 阶段所有权转移给 inode. */
    if (powerfs_tlv_dec_find_raw(dec, POWERFS_NET_FLD_REPLICA_CHUNKS, &raw, &raw_len) == 0) {
        parse_replica_chunks_field(raw, raw_len, layout);
    }

    /* K3-DEBUG: log parsed layout for diagnostics.
     * has_ec_chunks/ec_chunk_count 用于验证 remount 读路径修复:
     * Flat 文件的 PER_CHUNK 数据应被正确解析 (has_ec_chunks=1, ec_chunk_count>0). */
    pr_debug("powerfs: parse_file_layout RESULT placement=%u reliability=%u chunk_size=%u "
            "has_placement=%d has_reliability=%d stripe_size=%llu stripe_count=%u "
            "volume_ids_count=%u inline_len=%u ec_data=%u ec_parity=%u replica_count=%u "
            "has_ec_chunks=%d ec_chunk_count=%u\n",
            layout->placement, layout->reliability, layout->chunk_size,
            layout->has_placement ? 1 : 0, layout->has_reliability ? 1 : 0,
            (unsigned long long)layout->stripe_size, layout->stripe_count,
            layout->volume_ids_count, layout->inline_len,
            layout->ec_data_shards, layout->ec_parity_shards, layout->replica_count,
            layout->has_ec_chunks ? 1 : 0, layout->ec_chunk_count);
}

int powerfs_net_getattr(__u64 ino, __u32 *mode, __u32 *uid, __u32 *gid,
                         __u64 *size, __u32 *nlink,
                         __u64 *mtime, __u64 *atime, __u64 *ctime,
                         __u64 *volume_id, __u64 *file_key,
                         struct powerfs_file_layout *layout,
                         __u64 *rbytes_out, __u64 *rfiles_out, __u64 *rsubdirs_out,
                         __u64 *rctime_sec_out, __u32 *rctime_nsec_out)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    __u8 *resp_body;
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    /* K2: Inline 文件的 GETATTR 响应携带 inline_data (最大 8KB). */
    resp_body = kvmalloc(POWERFS_NET_RESP_INLINE_CAP, GFP_NOFS);
    if (!resp_body)
        return -ENOMEM;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_GETATTR, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, POWERFS_NET_RESP_INLINE_CAP,
                                    NULL, 0, 10000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        goto out;
    if (ret > 0) {
        ret = net_status_to_errno((__u16)ret);
        goto out;
    }

    if (resp_body_len > 0) {
        __u64 tmp_ino = 0;
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        /* Filer getattr 响应: Ino, Mode, Uid, Gid, Size, Nlink, Mtime, Atime, Ctime,
         * Name, [Chunks(JSON), Fid, VolumeId, Cookie, FileKey,
         *        Placement, Reliability, ReliabilityState, ChunkSize...]
         * 跳过 Ino (客户端已知), 然后按顺序解析属性字段 */
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_INO, &tmp_ino);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_MODE, mode);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_UID, uid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_GID, gid);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, size);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_NLINK, nlink);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_MTIME, mtime);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_ATIME, atime);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_CTIME, ctime);
        /* 使用 find 方式解析 volume_id/file_key, 不依赖字段顺序.
         * CTIME 后有 Name, Chunks(JSON), Fid 等中间字段. */
        if (volume_id)
            powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_VOLUME_ID, volume_id);
        if (file_key)
            powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_FILE_KEY, file_key);

        /* K1: 解析 FileLayout (placement/reliability/chunk_size) */
        parse_file_layout(&dec, layout);

        /* P1-5: 解析目录递归 rstat 聚合 (RBytes/RFiles/RSubdirs/RCtime).
         * 仅目录 inode 在 GETATTR/LOOKUP 响应中携带这些字段;
         * 文件 inode 缺省时保持 *out 不变 (不写入). find_* 失败 = 0,
         * 所以若不需要时传 NULL 直接跳过; 否则即使缺省也写 0 是 OK 的. */
        if (rbytes_out)
            (void)powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_RBYTES, rbytes_out);
        if (rfiles_out)
            (void)powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_RFILES, rfiles_out);
        if (rsubdirs_out)
            (void)powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_RSUBDIRS, rsubdirs_out);
        if (rctime_sec_out)
            (void)powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_RCTIME_SEC, rctime_sec_out);
        if (rctime_nsec_out)
            (void)powerfs_tlv_dec_find_u32(&dec, POWERFS_NET_FLD_RCTIME_NSEC, rctime_nsec_out);
    }

    ret = 0;
out:
    kvfree(resp_body);
    return ret;
}

/**
 * powerfs_net_create - 创建文件或目录
 *
 * 响应中包含 Filer 自分配的 volume_id + needle_id (file_key),
 * 内核需将其存入 inode 私有数据, 供后续直连 volume 读写使用.
 * 目录无数据, volume_id/file_key 为 0.
 *
 * K1-4: Inline/Stripe 模式下 Filer 通过 encode_file_layout 返回
 *       placement/reliability/chunk_size, 本函数解析后通过 layout 输出.
 *       Flat 模式 Filer 不 encode layout, layout->has_placement 保持 false,
 *       调用方按默认 Flat 处理.
 */
int powerfs_net_create(__u64 dir_ino, const char *name, size_t name_len,
                        __u32 mode, __u32 uid, __u32 gid, bool is_dir,
                        __u64 *ino_ret,
                        __u64 *volume_id_ret, __u64 *file_key_ret,
                        struct powerfs_file_layout *layout)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 *resp_body;
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    __u16 msg_type;
    int ret;

    /* K2: CREATE 响应可能携带 inline_data (Inline 模式新建文件时 Filer 返回 layout). */
    resp_body = kvmalloc(POWERFS_NET_RESP_INLINE_CAP, GFP_NOFS);
    if (!resp_body)
        return -ENOMEM;

    /* ================================================================
     * Two-phase mkdir (align with FUSE MetaShardClient::mkdir):
     *
     * MKDIR legacy single-RPC (handle_mkdir on Filer) only works when
     * shard_ino == shard_dir (both CreateInode + AddDirEntry batched
     * on the same Raft group). Cross-shard mkdir (child_dir placed on
     * a DIFFERENT shard from parent, default for multi-shard clusters)
     * has the parent_shard leader propose CreateInode to the *other*
     * shard's Raft group → "not_leader" → proposal fails.
     *
     * Fix: same algorithm as FUSE meta_shard_client.rs L2276-2395:
     *   shard_count <= 1 OR parent_shard == target_shard → legacy Mkdir
     *   else →
     *     (0) AllocInodeBatch(count=1) → target_shard leader
     *     (1) MkdirPhaseA → target_shard leader (CreateInode only)
     *     (2) MkdirPhaseB → parent_shard leader (AddDirEntry only)
     * Phase A attr response carries Ino/Mode/Uid/Gid/Size/Nlink/mtime.
     * If Phase B fails we orphan the inode (GC will clean it later).
     * ================================================================ */
    if (is_dir) {
        __u64 parent_shard;
        __u64 shard_count;
        __u64 target_shard;

        parent_shard = powerfs_calc_shard_id(dir_ino);
        shard_count = g_pool.shard_route.shard_count;
        if (shard_count < 1) shard_count = 1;
        target_shard = (shard_count <= 1)
                           ? parent_shard
                           : (parent_shard + 1) % shard_count;

        if (target_shard != parent_shard) {
            /* ============ Slow path: Two-phase mkdir ============ */
            __u64 ino_alloc = 0;
            __u8 ab_body[128];
            __u8 ab_resp[128];
            size_t ab_resp_len;
            struct powerfs_tlv_enc ab_enc;

            /* ---- Phase 0: AllocInodeBatch on target_shard.
             * Request: ShardId + Count + ClientId
             * Response: StartInode + EndInode (OK) / Name=error (fail) */
            powerfs_tlv_enc_init(&ab_enc, ab_body, sizeof(ab_body));
            powerfs_tlv_enc_u64(&ab_enc, POWERFS_NET_FLD_SHARD_ID, target_shard);
            powerfs_tlv_enc_u32(&ab_enc, POWERFS_NET_FLD_COUNT, 1);
            powerfs_tlv_enc_string(&ab_enc, POWERFS_NET_FLD_CLIENT_ID,
                                   "powerfs-kernel", 15);

            ab_resp_len = 0;
            ret = powerfs_net_send_request_shard(
                        POWERFS_NET_MSG_ALLOC_INODE_BATCH, target_shard,
                        ab_body, powerfs_tlv_enc_len(&ab_enc),
                        NULL, 0,
                        ab_resp, sizeof(ab_resp),
                        NULL, 0, POWERFS_META_TIMEOUT_MS,
                        &ab_resp_len, NULL);
            if (ret < 0) goto out;
            if (ret > 0) { ret = net_status_to_errno((__u16)ret); goto out; }

            if (ab_resp_len > 0) {
                struct powerfs_tlv_dec ab_dec;
                powerfs_tlv_dec_init(&ab_dec, ab_resp, ab_resp_len);
                if (powerfs_tlv_dec_find_u64(&ab_dec,
                        POWERFS_NET_FLD_START_INODE, &ino_alloc) != 0 ||
                    ino_alloc == 0) {
                    pr_warn("powerfs: two-phase mkdir AllocInodeBatch "
                            "invalid: no StartInode (target_shard=%llu)\n",
                            (unsigned long long)target_shard);
                    ret = -EREMOTEIO;
                    goto out;
                }
            } else {
                ret = -EREMOTEIO;
                goto out;
            }

            /* ---- Phase A: MkdirPhaseA CreateInode → target_shard.
             * Enc: ShardId + Ino + ParentIno + Name + Mode(u64) + Uid(u64) + Gid(u64)
             * Decode same attr format as legacy Mkdir. */
            {
                __u8 pa_body[512];
                struct powerfs_tlv_enc pa_enc;

                powerfs_tlv_enc_init(&pa_enc, pa_body, sizeof(pa_body));
                powerfs_tlv_enc_u64(&pa_enc, POWERFS_NET_FLD_SHARD_ID, target_shard);
                powerfs_tlv_enc_u64(&pa_enc, POWERFS_NET_FLD_INO, ino_alloc);
                powerfs_tlv_enc_u64(&pa_enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
                powerfs_tlv_enc_string(&pa_enc, POWERFS_NET_FLD_NAME, name, name_len);
                powerfs_tlv_enc_u64(&pa_enc, POWERFS_NET_FLD_MODE, (__u64)mode);
                powerfs_tlv_enc_u64(&pa_enc, POWERFS_NET_FLD_UID, (__u64)uid);
                powerfs_tlv_enc_u64(&pa_enc, POWERFS_NET_FLD_GID, (__u64)gid);

                resp_body_len = 0;
                ret = powerfs_net_send_request_shard(
                            POWERFS_NET_MSG_MKDIR_PHASE_A, target_shard,
                            pa_body, powerfs_tlv_enc_len(&pa_enc),
                            NULL, 0,
                            resp_body, POWERFS_NET_RESP_INLINE_CAP,
                            NULL, 0, POWERFS_META_TIMEOUT_MS,
                            &resp_body_len, NULL);
                if (ret < 0) goto out;
                if (ret > 0) { ret = net_status_to_errno((__u16)ret); goto out; }

                /* Phase A already returns full inode attrs.
                 * Parse into output vars now so Phase B failure doesn't
                 * lose the inode number (informational only). */
                if (resp_body_len > 0) {
                    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
                    if (ino_ret) (void)powerfs_tlv_dec_find_u64(
                                        &dec, POWERFS_NET_FLD_INO, ino_ret);
                }
            }

            /* ---- Phase B: MkdirPhaseB AddDirEntry → parent_shard (dir_ino shard).
             * Enc: ShardId + ParentIno + Name + Ino + Mode(u64) + Uid(u64) + Gid(u64)
             * Response: status only, body may contain Ino. */
            {
                __u8 pb_body[512];
                struct powerfs_tlv_enc pb_enc;
                __u8 pb_resp[128];
                size_t pb_resp_len;

                powerfs_tlv_enc_init(&pb_enc, pb_body, sizeof(pb_body));
                powerfs_tlv_enc_u64(&pb_enc, POWERFS_NET_FLD_SHARD_ID, parent_shard);
                powerfs_tlv_enc_u64(&pb_enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
                powerfs_tlv_enc_string(&pb_enc, POWERFS_NET_FLD_NAME, name, name_len);
                powerfs_tlv_enc_u64(&pb_enc, POWERFS_NET_FLD_INO, ino_alloc);
                powerfs_tlv_enc_u64(&pb_enc, POWERFS_NET_FLD_MODE, (__u64)mode);
                powerfs_tlv_enc_u64(&pb_enc, POWERFS_NET_FLD_UID, (__u64)uid);
                powerfs_tlv_enc_u64(&pb_enc, POWERFS_NET_FLD_GID, (__u64)gid);

                pb_resp_len = 0;
                ret = powerfs_net_send_request_shard(
                            POWERFS_NET_MSG_MKDIR_PHASE_B, parent_shard,
                            pb_body, powerfs_tlv_enc_len(&pb_enc),
                            NULL, 0,
                            pb_resp, sizeof(pb_resp),
                            NULL, 0, POWERFS_META_TIMEOUT_MS,
                            &pb_resp_len, NULL);
                if (ret < 0) goto out;
                if (ret > 0) {
                    ret = net_status_to_errno((__u16)ret);
                    /* Phase B failed: orphan inode on target_shard.
                     * GC will clean it later; still propagate error to caller. */
                    pr_warn("powerfs: two-phase mkdir PhaseB failed: ino=%llu "
                            "parent=%llu err=%d (orphan inode left on shard=%llu)\n",
                            (unsigned long long)ino_alloc,
                            (unsigned long long)dir_ino, ret,
                            (unsigned long long)target_shard);
                    goto out;
                }
            }

            /* ---- Two-phase mkdir done.
             * If Phase A already populated ino_ret we're good; Layout for
             * directories is not meaningful so we leave layout untouched
             * (same as legacy single-RPC mkdir path below which never sets
             * volume_id/file_key/layout). */
            ret = 0;
            goto out;
        }
        /* else: target_shard == parent_shard → fall through to legacy
         * single-RPC Mkdir path below. */
    }

    msg_type = is_dir ? POWERFS_NET_MSG_MKDIR : POWERFS_NET_MSG_CREATE;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);
    /*
     * 编码必须与 Filer 解码匹配:
     *   - CREATE: Filer handle_create 用 next_u32 (匹配 FUSE encode_create_req)
     *   - MKDIR:   Filer handle_mkdir  用 next_u64 (匹配 FUSE encode_mkdir_req)
     * 若内核 MKDIR 用 u32 编码, Filer next_u64 解码失败 (期望 8 字节, 实际 4),
     * 游标不前进, 后续字段全部错位, mode 回退默认 0o755 (不含 S_IFDIR),
     * 导致 readdir 返回的目录项 d_type=DT_UNKNOWN, ls 显示 "?rwxr-xr-x".
     * 修复: MKDIR 路径用 u64 编码 mode/uid/gid.
     */
    if (is_dir) {
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_MODE, (__u64)mode);
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_UID, (__u64)uid);
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_GID, (__u64)gid);
    } else {
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_MODE, mode);
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_UID, uid);
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_GID, gid);
    }
    powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_IS_DIR, is_dir ? 1 : 0);

    ret = powerfs_net_send_request(msg_type, dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, POWERFS_NET_RESP_INLINE_CAP,
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    if (ret < 0)
        goto out;
    if (ret > 0) {
        ret = net_status_to_errno((__u16)ret);
        goto out;
    }

    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        /* 使用 find 方式解析, 不依赖字段顺序
         * (Filer create 响应: Ino, Mode, Name, [FileLayout], VolumeId, FileKey) */
        if (ino_ret)
            powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_INO, ino_ret);
        if (volume_id_ret)
            powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_VOLUME_ID, volume_id_ret);
        if (file_key_ret)
            powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_FILE_KEY, file_key_ret);

        /* K1-4: 解析 FileLayout (Inline/Stripe 模式由 Filer encode_file_layout 返回).
         * Flat 模式 Filer 不 encode layout, parse_file_layout 内部保持默认值. */
        if (layout)
            parse_file_layout(&dec, layout);
    }

    ret = 0;
out:
    kvfree(resp_body);
    return ret;
}

/**
 * powerfs_net_unlink - 删除文件或目录
 */
int powerfs_net_unlink(__u64 dir_ino, const char *name, size_t name_len,
                       bool is_dir)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u16 msg_type;
    int ret;
    __u8 resp_body[256];
    size_t resp_body_len = 0;

    msg_type = is_dir ? POWERFS_NET_MSG_RMDIR : POWERFS_NET_MSG_UNLINK;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    ret = powerfs_net_send_request(msg_type, dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0) {
        /* Filer rmdir 非空目录时返回 STATUS_ERR_SERVER_ERROR + FieldId::Name
         * 携带错误描述 "directory not empty".
         * 对齐 FUSE 客户端 (fuse.rs L1680): "not empty" -> ENOTEMPTY. */
        if (ret == POWERFS_NET_STATUS_ERR_SERVER && resp_body_len > 0) {
            struct powerfs_tlv_dec dec;
            char err_str[128];

            powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
            if (powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_NAME,
                                        err_str, sizeof(err_str)) == 0) {
                if (strstr(err_str, "not empty"))
                    return -ENOTEMPTY;
            }
        }
        return net_status_to_errno((__u16)ret);
    }

    return 0;
}

/**
 * powerfs_net_rename - 重命名文件/目录
 */
int powerfs_net_rename(__u64 old_dir_ino, const char *old_name, size_t old_name_len,
                       __u64 new_dir_ino, const char *new_name, size_t new_name_len)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    int ret;
    /* resp_body captures REDIRECT responses (Filer returns leader address in
     * the body via FieldId::Owner). Without this buffer, RX path sees
     * resp_body=NULL and drops the body, leaving resp_body_len=0 — then
     * powerfs_net_parse_redirect fails and rename returns -EAGAIN forever. */
    __u8 resp_body[256];
    size_t resp_body_len = 0;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, old_dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, old_name, old_name_len);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_NEW_PARENT_INO, new_dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NEW_NAME, new_name, new_name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_RENAME, old_dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}

/**
 * powerfs_net_update_inode_size_chunks - K1-6 close/fsync 强一致同步 size+chunks
 *
 * 对齐 FUSE sync_size_chunks_on_close (powerfs-fuse/src/fuse.rs L990) 和
 * Filer handle_update_inode_size_chunks (net_handler.rs L1573).
 *
 * 请求 TLV 字段 (对齐 Filer decode 顺序):
 *   ShardId(0x70) + Ino(0x07) + Size(0x06) + ClientId(0x30)
 *   + [FileLayout: Placement(0xA0) + Reliability(0xA1) + ReliabilityState(0xA2)
 *               + ChunkLayout(0xA4)]
 *
 * FileLayout 编码 (对齐 powerfs-layout codec.rs encode_file_layout):
 *   Placement: u8 tag (Flat=0, Inline=1, Stripe=2)
 *   Reliability: bytes [tag] (SingleReplica=0)
 *   ReliabilityState: u8 (PendingReplicated=0)
 *   ChunkLayout: bytes [tag=1(PerChunk), count u32 LE, ChunkRef * count]
 *     ChunkRef (44B): offset u64 LE, size u64 LE, needle_id u64 LE,
 *                     volume_id u64 LE, crc32 u32 LE, mtime u64 LE
 *
 * 注意: Filer 端会用传入 chunks 覆盖现有 chunks. 调用方必须传入完整 chunks 列表
 *       (或 NULL 表示空列表, 会清空 Filer 端 chunks — 慎用).
 */
int powerfs_net_update_inode_size_chunks(__u64 shard_id, __u64 ino, __u64 size,
                                         const char *client_id,
                                         const struct powerfs_chunk_map *chunks,
                                         __u32 chunk_count,
                                         const __u8 *inline_data,
                                         __u32 inline_len)
{
    /* body 大小估算: 固定头(~80B) + ChunkLayout(5 + 44*chunk_count) + inline_data.
     * chunk_count=0 且无 inline_data 时 256B 足够; 多 chunk/大 inline 动态分配. */
    bool is_inline = (inline_data != NULL && inline_len > 0);
    size_t body_cap = 256 + (size_t)chunk_count * 44 + 64 + inline_len;
    __u8 *body;
    struct powerfs_tlv_enc enc;
    __u8 resp_body[128];
    size_t resp_body_len = 0;
    int ret;

    /* ChunkLayout 动态编码缓冲: 1(tag) + 4(count) + 44*count.
     * Inline 模式不传 ChunkLayout, 但仍分配 (跳过编码). */
    __u8 *layout_buf;
    size_t layout_len = 1 + 4 + (size_t)chunk_count * 44;
    const char *cid = client_id ? client_id : "kernel";
    __u32 i;

    body = kmalloc(body_cap, GFP_KERNEL);
    if (!body)
        return -ENOMEM;

    layout_buf = kmalloc(layout_len, GFP_KERNEL);
    if (!layout_buf) {
        kfree(body);
        return -ENOMEM;
    }

    /* 构建 ChunkLayout: [tag=1(PerChunk)][count u32 LE][ChunkRef * count] */
    layout_buf[0] = 1; /* PER_CHUNK tag */
    put_unaligned_le32(chunk_count, &layout_buf[1]);
    if (chunks && chunk_count > 0) {
        __u8 *p = &layout_buf[5];
        for (i = 0; i < chunk_count; i++) {
            const struct powerfs_chunk_map *cm = &chunks[i];
            put_unaligned_le64((__u64)i * POWERFS_CHUNK_SIZE, p);   /* offset */
            put_unaligned_le64(cm->size, p + 8);                    /* size */
            put_unaligned_le64(cm->needle_id, p + 16);               /* needle_id */
            put_unaligned_le64(cm->volume_id, p + 24);               /* volume_id */
            put_unaligned_le32(cm->crc32, p + 32);                   /* crc32 */
            put_unaligned_le64(0, p + 36);                           /* mtime */
            p += 44;
        }
    }

    powerfs_tlv_enc_init(&enc, body, body_cap);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SHARD_ID, shard_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SIZE, size);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID, cid, strlen(cid));

    /* K2: Inline 模式 — Placement=INLINE + InlineData, 无 ChunkLayout.
     * Flat 模式 — Placement=FLAT + ChunkLayout, 无 InlineData. */
    if (is_inline) {
        powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_PLACEMENT, POWERFS_PLACEMENT_INLINE);
        {
            __u8 rel_buf[1] = { 0 }; /* SingleReplica tag = 0 */
            powerfs_tlv_enc_bytes(&enc, POWERFS_NET_FLD_RELIABILITY, rel_buf, 1);
        }
        powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_RELIABILITY_STATE,
                           POWERFS_RSTATE_PENDING);
        /* InlineData (0xAE) — 文件数据直接编码在 inode 元数据中 */
        powerfs_tlv_enc_bytes(&enc, POWERFS_NET_FLD_INLINE_DATA,
                              inline_data, inline_len);
        /* InlineMaxSize (0xAF) — 通知 Filer 当前 inline 阈值 */
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_INLINE_MAX_SIZE,
                            POWERFS_INLINE_MAX_SIZE);
    } else {
        /* FileLayout: Placement=Flat, Reliability=SingleReplica,
         * ReliabilityState=PendingReplicated, ChunkLayout=PerChunk */
        powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_PLACEMENT, POWERFS_PLACEMENT_FLAT);
        {
            __u8 rel_buf[1] = { 0 }; /* SingleReplica tag = 0 */
            powerfs_tlv_enc_bytes(&enc, POWERFS_NET_FLD_RELIABILITY, rel_buf, 1);
        }
        powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_RELIABILITY_STATE,
                           POWERFS_RSTATE_PENDING);
        powerfs_tlv_enc_bytes(&enc, POWERFS_NET_FLD_CHUNK_LAYOUT, layout_buf, layout_len);
    }

    kfree(layout_buf);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_UPDATE_INODE_SIZE_CHUNKS, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    kfree(body);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_update_inode_size_chunks);

/**
 * powerfs_net_migrate_inline_alloc - K2-6 Inline → Flat 迁移分配
 *
 * 对齐 FUSE migrate_inline_alloc (powerfs-fuse/src/fuse.rs L3469) 和
 * Filer handle_migrate_inline_alloc (net_handler.rs L1832).
 *
 * 客户端 write 累计超 max_size×1.5 时调用. Filer 仅分配 (volume_id,
 * needle_id), 不修改 inode 元数据 (保留 inline_data 用于 crash safety).
 *
 * Request TLV:  ShardId(0x70) + Ino(0x07)
 * Response TLV: VolumeId(0x92) + FileKey(0x94) / Name(0x02)=error
 *
 * crash safety: 若客户端在分配后崩溃, Filer 仍有 inline_data, 文件仍可
 * 作为 Inline 读; 分配的 needle_id 泄漏 (可接受, 同 CREATE 失败).
 */
int powerfs_net_migrate_inline_alloc(__u64 shard_id, __u64 ino,
                                     __u64 *volume_id, __u64 *file_key)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[128];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    __u64 v_id = 0, f_key = 0;
    int ret;

    if (!volume_id || !file_key)
        return -EINVAL;

    *volume_id = 0;
    *file_key = 0;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SHARD_ID, shard_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_MIGRATE_INLINE_ALLOC, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    /* 解析响应: VolumeId + FileKey */
    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
    if (powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_VOLUME_ID, &v_id) != 0) {
        pr_warn("powerfs: MIGRATE_INLINE_ALLOC ino=%llu missing VolumeId in response\n",
                (unsigned long long)ino);
        return -EPROTO;
    }
    if (powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_FILE_KEY, &f_key) != 0) {
        pr_warn("powerfs: MIGRATE_INLINE_ALLOC ino=%llu missing FileKey in response\n",
                (unsigned long long)ino);
        return -EPROTO;
    }

    *volume_id = v_id;
    *file_key = f_key;

    pr_info("powerfs: MIGRATE_INLINE_ALLOC ino=%llu → volume_id=%llu file_key=%#llx\n",
            (unsigned long long)ino, (unsigned long long)v_id,
            (unsigned long long)f_key);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_migrate_inline_alloc);

/**
 * powerfs_net_readdir - 读取目录项 (匹配 Filer 协议)
 *
 * 请求 TLV 字段: ParentIno, Limit, LastName (分页游标)
 * 响应 TLV 字段: Count, HasMore, Entry[] (每个 Entry 是嵌套 TLV)
 *
 * 每个 Entry 嵌套字段: Ino, Name, Mode, Uid, Gid, Size, Atime, Mtime, Ctime, Nlink
 */
int powerfs_net_readdir_timeout(__u64 dir_ino, const char *last_name, __u64 limit,
                                struct powerfs_net_dir_entry *entries,
                                __u32 max_entries, __u32 *actual_count,
                                bool *has_more, int timeout_ms)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 *resp_body;
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    __u32 count = 0;
    __u64 more = 0;
    __u8 field;
    size_t flen;
    int ret;

    /* readdir 响应缓冲: 256KB.
     * 每个 Entry 最坏 ~288B (255B name + 10 字段 * ~5B TLV overhead),
     * 256 entries ≈ 72KB. 256KB 留充足余量, 用 kvmalloc 允许 vmalloc 回退. */
    const size_t resp_cap = 256 * 1024;

    *actual_count = 0;
    *has_more = false;

    /* 动态分配响应缓冲 (避免栈溢出) */
    resp_body = kvmalloc(resp_cap, GFP_KERNEL);
    if (!resp_body)
        return -ENOMEM;

    /* 编码请求: ParentIno + Limit + LastName */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_LIMIT, limit);
    if (last_name && last_name[0])
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LAST_NAME,
                               last_name, strlen(last_name));

    ret = powerfs_net_send_request(POWERFS_NET_MSG_READDIR, dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, resp_cap,
                                    NULL, 0, timeout_ms,
                                    &resp_body_len, NULL);
    if (ret < 0) {
        kvfree(resp_body);
        return ret;
    }
    if (ret > 0) {
        kvfree(resp_body);
        return net_status_to_errno((__u16)ret);
    }

    if (resp_body_len == 0) {
        kvfree(resp_body);
        return 0;
    }

    /* 解码响应: 先读 Count 和 HasMore，再逐个解析 Entry */
    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
    powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_COUNT, &count);
    powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_HAS_MORE, &more);
    *has_more = (more != 0);

    /* 限制返回条目数不超过 max_entries */
    if (count > max_entries)
        count = max_entries;

    /* 遍历 Entry 字段 (嵌套 TLV) */
    while (count > 0 && !powerfs_tlv_dec_is_empty(&dec)) {
        const __u8 *entry_data;
        size_t entry_len;

        ret = powerfs_tlv_dec_next(&dec, &field, &flen);
        if (ret < 0)
            break;

        if (field != POWERFS_NET_FLD_ENTRY) {
            powerfs_tlv_dec_skip(&dec, flen);
            continue;
        }

        entry_data = dec.buf + dec.pos;
        entry_len = flen;

        /* 解码嵌套 TLV 中的条目字段 */
        {
            struct powerfs_tlv_dec edec;
            struct powerfs_net_dir_entry *e = &entries[*actual_count];

            memset(e, 0, sizeof(*e));
            powerfs_tlv_dec_init(&edec, entry_data, entry_len);
            powerfs_tlv_dec_u64(&edec, POWERFS_NET_FLD_INO, &e->ino);
            powerfs_tlv_dec_string(&edec, POWERFS_NET_FLD_NAME,
                                   e->name, sizeof(e->name));
            powerfs_tlv_dec_u32(&edec, POWERFS_NET_FLD_MODE, &e->mode);
            powerfs_tlv_dec_u32(&edec, POWERFS_NET_FLD_UID, &e->uid);
            powerfs_tlv_dec_u32(&edec, POWERFS_NET_FLD_GID, &e->gid);
            powerfs_tlv_dec_u64(&edec, POWERFS_NET_FLD_SIZE, &e->size);
            powerfs_tlv_dec_u64(&edec, POWERFS_NET_FLD_ATIME, &e->atime);
            powerfs_tlv_dec_u64(&edec, POWERFS_NET_FLD_MTIME, &e->mtime);
            powerfs_tlv_dec_u64(&edec, POWERFS_NET_FLD_CTIME, &e->ctime);
            powerfs_tlv_dec_u32(&edec, POWERFS_NET_FLD_NLINK, &e->nlink);

            (*actual_count)++;
            count--;
        }

        /* 跳过已读字段的数据 */
        powerfs_tlv_dec_skip(&dec, flen);
    }

    kvfree(resp_body);
    return 0;
}

/* 兼容 wrapper: 用默认 5s 超时 (POWERFS_READDIR_TIMEOUT_MS). */
int powerfs_net_readdir(__u64 dir_ino, const char *last_name, __u64 limit,
                        struct powerfs_net_dir_entry *entries, __u32 max_entries,
                        __u32 *actual_count, bool *has_more)
{
    return powerfs_net_readdir_timeout(dir_ino, last_name, limit,
                                       entries, max_entries,
                                       actual_count, has_more,
                                       POWERFS_READDIR_TIMEOUT_MS);
}

/**
 * powerfs_net_write - 写数据 (直连 Volume Server, 不经过 Filer)
 *
 * 数据读写绕过 Filer: 从 inode 的 (volume_id, file_key) 直连 Volume Server
 * 写入 needle.
 *
 * needle 模型: write_needle 整体替换 needle 内容 (不支持 partial write).
 * 因此 partial write 需 read-modify-write:
 *   1. 读取整个 needle (若不存在则全零)
 *   2. 将 data 拷贝到 needle_buf 的对应位置
 *   3. 整体写回 needle
 *
 * 参数:
 *   ino: inode 号 (用于 lease 校验, 传给 volume server)
 *   volume_id/file_key: 从 lookup/getattr 获取的数据直连标识
 *   offset/data/data_len: 文件内偏移和写入数据
 *   written: 输出, 实际写入字节数
 */
int powerfs_net_write(__u64 ino, __u64 volume_id, __u64 file_key,
                      __u64 offset, const __u8 *data, size_t data_len,
                      __u32 *written)
{
    __u8 *needle_buf;
    __u64 needle_id;
    size_t offset_in_needle;
    __u32 existing_len = 0;
    int ret;

    if (!volume_id || !file_key) {
        pr_warn("powerfs: write ino=%llu no volume mapping (volume_id=%llu file_key=%llu)\n",
                (unsigned long long)ino,
                (unsigned long long)volume_id,
                (unsigned long long)file_key);
        return -ENOLINK;
    }

    /* 写入不能跨 chunk 边界 (调用方应保证, VFS write_end 按 page 对齐) */
    needle_id = file_key + offset / POWERFS_CHUNK_SIZE;
    offset_in_needle = offset % POWERFS_CHUNK_SIZE;

    if (offset_in_needle + data_len > POWERFS_CHUNK_SIZE) {
        pr_warn("powerfs: write ino=%llu crosses chunk boundary (offset=%llu len=%zu)\n",
                (unsigned long long)ino,
                (unsigned long long)offset, data_len);
        return -EINVAL;
    }

    needle_buf = kvmalloc(POWERFS_CHUNK_SIZE, GFP_KERNEL);
    if (!needle_buf)
        return -ENOMEM;

    /* read-modify-write: 先读现有 needle (不存在则全零) */
    ret = powerfs_net_read_needle(volume_id, needle_id,
                                   needle_buf, POWERFS_CHUNK_SIZE,
                                   &existing_len);
    if (ret < 0 && ret != -ENOENT) {
        /* -ENOENT = needle 不存在 (新文件), 用全零 buffer 继续.
         * 其他错误 = 真实故障, 中止写入. */
        pr_warn("powerfs: write read-modify-write read_needle vid=%llu nid=%llu failed: %d\n",
                (unsigned long long)volume_id,
                (unsigned long long)needle_id, ret);
        kvfree(needle_buf);
        return ret;
    }

    /* 将 data 拷贝到 needle_buf 对应位置 */
    memcpy(needle_buf + offset_in_needle, data, data_len);

    /* 计算写入后的 needle 总长度 (扩展 if 写入超出原有内容) */
    if (offset_in_needle + data_len > existing_len)
        existing_len = offset_in_needle + data_len;

    /* 整体写回 needle (read-modify-write 路径无 lease token, 传 NULL) */
    ret = powerfs_net_write_needle(volume_id, needle_id, ino,
                                    needle_buf, existing_len,
                                    NULL, 0);
    kvfree(needle_buf);

    if (ret < 0) {
        pr_warn("powerfs: write_needle vid=%llu nid=%llu failed: %d\n",
                (unsigned long long)volume_id,
                (unsigned long long)needle_id, ret);
        return ret;
    }

    if (written)
        *written = (__u32)data_len;

    return 0;
}

/**
 * powerfs_net_setattr - 设置文件属性
 *
 * TLV body: Ino + optional {Mode, Uid, Gid, Size, Mtime, Atime}.
 * Each optional field is encoded only when its bit is set in mode_valid,
 * allowing the Filer to distinguish "not set" (None) from "set to 0".
 *
 * mtime/atime are unix seconds. Callers that only sync SIZE (writeback,
 * fsync) pass mode_valid without MTIME/ATIME bits and mtime=atime=0.
 */
int powerfs_net_setattr(__u64 ino, __u32 mode_valid, __u32 mode,
                        __u32 uid, __u32 gid, __u64 size,
                        __u64 mtime, __u64 atime)
{
    __u8 body[128];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[64];
    size_t resp_body_len = 0;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);

    if (mode_valid & POWERFS_ATTR_MODE)
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_MODE, mode);
    if (mode_valid & POWERFS_ATTR_UID)
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_UID, uid);
    if (mode_valid & POWERFS_ATTR_GID)
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_GID, gid);
    if (mode_valid & POWERFS_ATTR_SIZE)
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SIZE, size);
    if (mode_valid & POWERFS_ATTR_MTIME)
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_MTIME, mtime);
    if (mode_valid & POWERFS_ATTR_ATIME)
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_ATIME, atime);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_SETATTR, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 2000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}

/**
 * powerfs_net_statfs - 获取文件系统统计信息 (查询 Master)
 *
 * 向 Master 发送 StatFs 请求, 聚合所有 volume 的 size/used/file_count.
 * 结果缓存 30s (POWERFS_STATFS_CACHE_TTL), 避免频繁查询 Master.
 * 缓存过期后同步查询 Master; 查询失败时回退到上次缓存值 (若有).
 *
 * 填充 kstatfs:
 *   f_type     = POWERFS_SUPER_MAGIC
 *   f_bsize    = block_size (4096)
 *   f_blocks   = total_size / block_size
 *   f_bfree    = free_size / block_size
 *   f_bavail   = free_size / block_size
 *   f_files    = total_files
 *   f_ffree    = free_inodes
 *   f_namelen  = POWERFS_MAX_NAME_LEN
 */

struct powerfs_statfs_cache g_statfs_cache;

DEFINE_SPINLOCK(g_statfs_cache_lock);

int powerfs_net_statfs(struct kstatfs *stats)
{
    unsigned long now = jiffies;
    bool need_refresh = false;
    __u64 total_size = 0, free_size = 0, total_files = 0, free_inodes = 0;
    __u32 block_size = 4096;

    /* 1. Check cache under spinlock */
    spin_lock(&g_statfs_cache_lock);
    if (g_statfs_cache.valid &&
        time_before(now, g_statfs_cache.cached_jiffies + POWERFS_STATFS_CACHE_TTL)) {
        /* Cache is fresh — use it */
        total_size = g_statfs_cache.total_size;
        free_size = g_statfs_cache.free_size;
        total_files = g_statfs_cache.total_files;
        free_inodes = g_statfs_cache.free_inodes;
        block_size = g_statfs_cache.block_size;
        spin_unlock(&g_statfs_cache_lock);

        /* Fill kstatfs from cached data */
        stats->f_type = POWERFS_SUPER_MAGIC;
        stats->f_bsize = block_size;
        stats->f_frsize = block_size;
        stats->f_blocks = block_size ? total_size / block_size : 0;
        stats->f_bfree = block_size ? free_size / block_size : 0;
        stats->f_bavail = stats->f_bfree;
        stats->f_files = total_files;
        stats->f_ffree = free_inodes;
        stats->f_namelen = POWERFS_MAX_NAME_LEN;
        return 0;
    }

    /* Cache expired or invalid — need to query Master */
    need_refresh = true;

    /* If we have stale cached data, use it as fallback */
    if (g_statfs_cache.valid) {
        total_size = g_statfs_cache.total_size;
        free_size = g_statfs_cache.free_size;
        total_files = g_statfs_cache.total_files;
        free_inodes = g_statfs_cache.free_inodes;
        block_size = g_statfs_cache.block_size;
    }
    spin_unlock(&g_statfs_cache_lock);

    if (!need_refresh)
        goto fill_stats;

    /* 2. Query Master for fresh data */
    if (g_pool.master_set) {
        char addr_buf[64];
        char *p, *tok;
        struct socket *sock = NULL;
        __u8 resp_body[128];
        __u8 resp_data[64];
        size_t body_len = 0, data_len = 0;
        struct powerfs_net_frame_hdr hdr;
        __u32 seq;
        int ret, i;

        strncpy(addr_buf, g_pool.master_addr, sizeof(addr_buf) - 1);
        addr_buf[sizeof(addr_buf) - 1] = '\0';

        p = addr_buf;
        while ((tok = strsep(&p, ",")) != NULL) {
            struct powerfs_tlv_dec dec;

            while (*tok == ' ')
                tok++;
            if (tok[0] == '\0')
                continue;

            /* Connect to Master */
            sock = powerfs_net_create_tcp_socket();
            if (!sock)
                continue;

            ret = powerfs_net_tcp_connect(sock, tok, g_pool.master_port);
            if (ret < 0) {
                powerfs_net_close_socket(sock);
                continue;
            }

            /* Handshake */
            ret = powerfs_net_do_handshake(sock);
            if (ret < 0) {
                powerfs_net_close_socket(sock);
                continue;
            }

            /* Send StatFs request (empty body) */
            seq = atomic_inc_return(&g_discover_seq);
            powerfs_net_frame_hdr_encode(&hdr,
                                          POWERFS_NET_MSG_STATFS,
                                          POWERFS_NET_FLAG_REQUEST,
                                          seq, 0, 0, 0, 0);

            ret = powerfs_net_frame_send(sock, &hdr, NULL, 0, NULL, 0);
            if (ret < 0) {
                powerfs_net_close_socket(sock);
                continue;
            }

            /* Receive response (skip NOTIFY frames) */
            for (i = 0; i < 5; i++) {
                ret = powerfs_net_frame_recv(sock, &hdr,
                                              resp_body, sizeof(resp_body), &body_len,
                                              resp_data, sizeof(resp_data), &data_len,
                                              POWERFS_NET_RECV_TIMEOUT);
                if (ret < 0)
                    break;
                if (hdr.flags & POWERFS_NET_FLAG_NOTIFY)
                    continue;
                break;
            }

            powerfs_net_close_socket(sock);

            if (ret < 0) {
                pr_debug("powerfs: statfs recv failed: %d\n", ret);
                continue;
            }

            if (hdr.status != POWERFS_NET_STATUS_OK) {
                pr_debug("powerfs: statfs status=%u\n", hdr.status);
                continue;
            }

            /* Decode TLV response */
            powerfs_tlv_dec_init(&dec, resp_body, body_len);
            powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, &total_size);
            powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_FREE, &free_size);
            powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_NLINK, &total_files);
            powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_FREE_INODES, &free_inodes);
            powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_BLKSIZE, &block_size);

            /* Update cache */
            spin_lock(&g_statfs_cache_lock);
            g_statfs_cache.total_size = total_size;
            g_statfs_cache.free_size = free_size;
            g_statfs_cache.total_files = total_files;
            g_statfs_cache.free_inodes = free_inodes;
            g_statfs_cache.block_size = block_size;
            g_statfs_cache.cached_jiffies = now;
            g_statfs_cache.valid = true;
            spin_unlock(&g_statfs_cache_lock);

            pr_debug("powerfs: statfs from master %s: total=%llu free=%llu files=%llu\n",
                     tok, total_size, free_size, total_files);
            break; /* Success — stop trying other masters */
        }
    }

fill_stats:
    /* 3. Fill kstatfs (from fresh data or stale fallback) */
    stats->f_type = POWERFS_SUPER_MAGIC;
    stats->f_bsize = block_size;
    stats->f_frsize = block_size;
    stats->f_blocks = block_size ? total_size / block_size : 0;
    stats->f_bfree = block_size ? free_size / block_size : 0;
    stats->f_bavail = stats->f_bfree;
    stats->f_files = total_files;
    stats->f_ffree = free_inodes;
    stats->f_namelen = POWERFS_MAX_NAME_LEN;

    return 0;
}

/**
 * powerfs_net_symlink - 创建符号链接
 */
int powerfs_net_symlink(__u64 dir_ino, const char *name, size_t name_len,
                        const char *target, size_t target_len, __u64 *ino_ret)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[128];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_SYMLINK_TARGET, target, target_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_SYMLINK, dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_INO, ino_ret);
    }

    return 0;
}

/**
 * powerfs_net_readlink - 读取符号链接目标
 */
int powerfs_net_readlink(__u64 ino, char *target, size_t target_cap)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[512];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_READLINK, ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 2000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_SYMLINK_TARGET,
                                target, target_cap);
    }

    return 0;
}

/**
 * powerfs_net_link - 创建硬链接
 */
int powerfs_net_link(__u64 ino, __u64 dir_ino, const char *name, size_t name_len)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    int ret;
    /* resp_body captures REDIRECT responses (same rationale as rename). */
    __u8 resp_body[256];
    size_t resp_body_len = 0;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_LINK, dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}

