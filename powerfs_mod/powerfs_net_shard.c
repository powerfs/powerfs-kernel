/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_shard.c - split from powerfs_net.c (mechanical refactor) */

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

/* ========== ShardMap implementation ========== */

/* Route an inode to its shard_id via binary search.
 * Returns 0 if the map is empty (should not happen in production). */
__u64 shard_map_route(__u64 inode)
{
    int lo, hi, mid, idx;
    __u64 sid;

    spin_lock(&g_shard_map.lock);
    if (g_shard_map.entry_count == 0) {
        spin_unlock(&g_shard_map.lock);
        return 0;
    }
    /* Binary search: find the last entry whose range_start <= inode */
    lo = 0;
    hi = g_shard_map.entry_count - 1;
    idx = 0;
    while (lo <= hi) {
        mid = lo + (hi - lo) / 2;
        if (g_shard_map.entries[mid].range_start <= inode) {
            idx = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    sid = g_shard_map.entries[idx].shard_id;
    spin_unlock(&g_shard_map.lock);
    return sid;
}
EXPORT_SYMBOL_GPL(shard_map_route);

/* Initialize from shard_count: equal 1M ranges per shard.
 * Equivalent to Rust's ShardMap::from_shard_count. */
void shard_map_from_shard_count(__u64 count)
{
    int i;
    __u64 inode_per_shard = 1000000;  /* 1M per shard */
    __u64 start = 0;

    if (count == 0 || count > POWERFS_MAX_SHARDS)
        count = 1;

    spin_lock(&g_shard_map.lock);
    g_shard_map.entry_count = (int)count;
    for (i = 0; i < (int)count; i++) {
        g_shard_map.entries[i].range_start = start;
        g_shard_map.entries[i].range_end = start + inode_per_shard;
        g_shard_map.entries[i].shard_id = (__u64)i;
        start += inode_per_shard;
    }
    spin_unlock(&g_shard_map.lock);

    pr_info("powerfs: ShardMap initialized from shard_count=%llu (%d ranges)\n",
            (u64)count, (int)count);
}
EXPORT_SYMBOL_GPL(shard_map_from_shard_count);

/* Reconstruct from Master-provided entries blob (0xBD).
 * Each entry = 25 bytes: range_start:u64 LE + range_end:u64 LE +
 * shard_id:u64 LE + state:u8. */
int shard_map_from_entries(const __u8 *blob, size_t len)
{
    int entry_count, i;

    if (!blob || len == 0)
        return -EINVAL;

    entry_count = len / 25;
    if (entry_count == 0 || entry_count > POWERFS_MAX_SHARDS) {
        pr_warn("powerfs: ShardMap entries count %d out of range\n", entry_count);
        return -EINVAL;
    }

    spin_lock(&g_shard_map.lock);
    g_shard_map.entry_count = entry_count;
    for (i = 0; i < entry_count; i++) {
        const __u8 *p = blob + i * 25;
        g_shard_map.entries[i].range_start = get_unaligned_le64(p);
        g_shard_map.entries[i].range_end = get_unaligned_le64(p + 8);
        g_shard_map.entries[i].shard_id = get_unaligned_le64(p + 16);
        /* p[24] = state (0=Active, 1=Draining) — both still routable */
    }
    /* Entries from Master are already sorted by range_start, but sort
     * defensively in case of corruption. */
    /* Simple insertion sort (entry_count is small, <= 64) */
    for (i = 1; i < entry_count; i++) {
        struct shard_map_entry tmp = g_shard_map.entries[i];
        int j = i - 1;
        while (j >= 0 && g_shard_map.entries[j].range_start > tmp.range_start) {
            g_shard_map.entries[j + 1] = g_shard_map.entries[j];
            j--;
        }
        g_shard_map.entries[j + 1] = tmp;
    }
    spin_unlock(&g_shard_map.lock);

    pr_info("powerfs: ShardMap initialized from Master entries (%d ranges)\n",
            entry_count);
    return 0;
}
EXPORT_SYMBOL_GPL(shard_map_from_entries);

/* discover_filers 用的序列号计数器 (旧 g_conn.seq_counter 的替代,
 * 仅用于 master 发现阶段的裸 socket 请求, 不涉及 per-filer 连接) */
atomic_t g_discover_seq = ATOMIC_INIT(0);
/* ========== CRC32C 实现 (软件) ========== */

/* CRC32C 查表 (Castagnoli 多项式) */
__u32 crc32c_table[256];
bool crc32c_table_init = false;

void powerfs_crc32c_init_table(void)
{
    __u32 i, j, c;

    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            if (c & 1)
                c = (c >> 1) ^ 0x82F63B78;
            else
                c = c >> 1;
        }
        crc32c_table[i] = c;
    }
    crc32c_table_init = true;
}

/**
 * powerfs_crc32c - 计算标准 RFC3720 CRC32C (Castagnoli).
 * 初始 0xFFFFFFFF, 输出 ~crc. 用于数据块完整性校验等场景.
 */
__u32 powerfs_crc32c(const __u8 *data, size_t len)
{
    __u32 crc = 0xFFFFFFFF;
    size_t i;

    if (!crc32c_table_init)
        powerfs_crc32c_init_table();

    for (i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);

    return crc ^ 0xFFFFFFFF;
}
EXPORT_SYMBOL_GPL(powerfs_crc32c);

/**
 * powerfs_crc32c_append - 与 Rust crc32c crate (0.6.8) crc32c::crc32c_append 严格对齐.
 *
 * Rust crate 内部语义 (sw.rs): state = ~crc; for_each_byte:
 *   state = table[..] ^ (state>>8); return ~state;
 * 即输入反转 (~crc 作内部 state)、输出反转 (~state 返回), 与 RFC3720 one-shot
 * 一致: crc32c_append(0, data) ≡ crc32c(data) ≡ ~internal(0xFFFFFFFF, data).
 *
 * 因此 Rust 侧 FrameHeader::calc_header_crc() 对 9 个字段链式调用 crc32c_append
 * 等价于对整段 24B header 调一次 crc32c_append(0, hdr, 24), 即 RFC3720 one-shot
 * 对 24 字节的结果. 这里直接对整段 24B 调一次 powerfs_crc32c_append(0, hdr, 24)
 * 即可完全对齐.
 *
 * 历史 bug (RC-4): 之前误以为 Rust crc32c_append 是流式无取反 (init=crc 直累,
 *   无反转), 导致 kernel 算出的 header_crc 与 master 端 FrameHeader::verify_crc()
 *   不一致, master 直接 drop 连接, dmesg 报 "recv header truncated 0 < 28
 *   (peer closed)", master 日志 "invalid frame header: header CRC mismatch".
 *   该 bug 与证书机制无关, 现已修复.
 */
__u32 powerfs_crc32c_append(__u32 crc, const __u8 *data, size_t len)
{
    __u32 state = ~crc;
    size_t i;

    if (!crc32c_table_init)
        powerfs_crc32c_init_table();

    for (i = 0; i < len; i++)
        state = crc32c_table[(state ^ data[i]) & 0xFF] ^ (state >> 8);

    return ~state;
}
EXPORT_SYMBOL_GPL(powerfs_crc32c_append);

/* ========== 帧头编解码 ========== */


/**
 * powerfs_net_frame_hdr_encode - 编码帧头 (28 字节)
 *
 * @body_len: body 段长度 (TLV 部分)
 * @data_len: body + data 总长度 (data 段 = data_len - body_len)
 * @route_hash: 目的地hash+channel (防错乱校验)
 */
void powerfs_net_frame_hdr_encode(struct powerfs_net_frame_hdr *hdr,
                                   __u16 msg_type, __u8 flags,
                                   __u32 seq, __u16 status,
                                   __u32 body_len, __u32 data_len,
                                   __u8 route_hash)
{
    __u32 crc;

    /* 填充字段.
     * 注意: magic 必须用 cpu_to_be32() 真正翻转字节, 使内存布局为
     * "PFSN" (50 46 53 4E), 与 Filer 端 b"PFSN" 一致.
     * 之前用 (__be32) 强制转换不翻转字节, 导致内存为 "NSFP", Filer
     * 端 magic 校验失败 -> "invalid frame header". */
    hdr->magic = cpu_to_be32(POWERFS_NET_MAGIC);
    hdr->version = POWERFS_NET_VERSION;
    hdr->flags = flags;

    /* seq: little-endian */
    hdr->seq = cpu_to_le32(seq);

    /* msg_type: little-endian */
    hdr->msg_type = cpu_to_le16(msg_type);

    /* status: little-endian */
    hdr->status = cpu_to_le16(status);

    /* data_len: body + data 总长度 (little-endian) */
    hdr->data_len = cpu_to_le32(data_len);

    /* body_len: body 段长度 (little-endian), 接收端据此切分 body/data */
    hdr->body_len = cpu_to_le32(body_len);

    /* route_hash: 高7位=client_id hash, 低1位=channel (防错乱校验) */
    hdr->route_hash = route_hash;
    /* protocol_ver: 协议版本 (版本升级一致性检查) */
    hdr->protocol_ver = POWERFS_NET_PROTOCOL_VER;

    /* 计算 header_crc: powerfs_crc32c_append(0, hdr, 24) 等价于
     * Rust crc32c::crc32c_append(0, hdr, 24), 后者由 9 个字段链式调用
     * 等价于 RFC3720 one-shot crc32c(hdr, 24). 与 Rust FrameHeader::
     * calc_header_crc() 完全一致. (RC-4 修复前实现错误导致 master
     * verify_crc 失败 drop 连接, 现已对齐.) */
    crc = powerfs_crc32c_append(0, (const __u8 *)hdr, 24);
    hdr->header_crc = cpu_to_le32(crc);
}
EXPORT_SYMBOL_GPL(powerfs_net_frame_hdr_encode);

/**
 * powerfs_net_frame_hdr_decode - 解码帧头并验证 CRC
 */
bool powerfs_net_frame_hdr_decode(const __u8 *buf, size_t len,
                                   struct powerfs_net_frame_hdr *hdr)
{
    __u32 calc_crc;

    if (len < POWERFS_NET_FRAME_HDR_SIZE)
        return false;

    memcpy(hdr, buf, POWERFS_NET_FRAME_HDR_SIZE);

    /* 验证魔数 */
    if (be32_to_cpu(hdr->magic) != POWERFS_NET_MAGIC)
        return false;

    /* 验证版本 */
    if (hdr->version != POWERFS_NET_VERSION)
        return false;

    /* 验证 CRC: powerfs_crc32c_append(0, buf, 24) ≡ Rust crc32c_append(0, hdr, 24)
     * ≡ FrameHeader::calc_header_crc(). 与 Rust FrameHeader::verify_crc() 对齐. */
    calc_crc = powerfs_crc32c_append(0, buf, 24);
    if (le32_to_cpu(hdr->header_crc) != calc_crc)
        return false;

    /* 转换字段 */
    hdr->seq = le32_to_cpu(hdr->seq);
    hdr->msg_type = le16_to_cpu(hdr->msg_type);
    hdr->status = le16_to_cpu(hdr->status);
    hdr->data_len = le32_to_cpu(hdr->data_len);
    hdr->body_len = le32_to_cpu(hdr->body_len);

    return true;
}
EXPORT_SYMBOL_GPL(powerfs_net_frame_hdr_decode);
