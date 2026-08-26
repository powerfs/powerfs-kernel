/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_data.c - split from powerfs_net.c (mechanical refactor) */

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
 * powerfs_net_read_ec - K4-5/K4-6 EC 模式读取 (降级重建)
 *
 * 对齐 FUSE fuse.rs L2440-2538 EC 读取逻辑:
 *   1. 计算 group_idx = offset / (data_shards × chunk_size)
 *   2. 读取 group 的所有 shards (data + parity)
 *   3. CRC32 校验每个 shard, 不匹配视为缺失
 *   4. Fast path: 所有 data shards 可用 → 直接拼接
 *   5. Slow path: 有缺失 → powerfs_ec_decode 降级重建
 *   6. 从拼接的 group_data 提取请求范围
 */
int powerfs_net_read_ec(struct powerfs_inode_info *pi, __u64 ino,
                                __u64 offset, __u32 length,
                                __u8 *buf, size_t buf_cap, __u32 *read_len)
{
    u32 data_shards = pi->ec_data_shards;
    u32 parity_shards = pi->ec_parity_shards;
    u32 total_shards = data_shards + parity_shards;
    u64 group_data_size = (u64)data_shards * POWERFS_CHUNK_SIZE;
    u64 group_idx = div64_u64(offset, group_data_size);
    u64 group_offset = offset - group_idx * group_data_size;
    u64 group_base = group_idx * total_shards;
    u8 **shards = NULL;
    bool *available = NULL;
    u8 *group_data = NULL;
    size_t copy_len;
    u32 i;
    int ret;

    *read_len = 0;

    if (data_shards == 0 || !pi->ec_chunks ||
        group_base + total_shards > pi->ec_chunk_count)
        return -EINVAL;

    shards = kcalloc(total_shards, sizeof(u8 *), GFP_KERNEL);
    available = kcalloc(total_shards, sizeof(bool), GFP_KERNEL);
    if (!shards || !available) {
        ret = -ENOMEM;
        goto out;
    }

    /* 1. 读取所有 shards (data + parity), 失败/CRC不匹配视为缺失.
     * 持锁快照每个 shard 的 (volume_id, needle_id, crc32), 然后释放锁
     * 做网络 I/O — 避免 ec_chunks 被 GETATTR 并发释放导致 UAF. */
    for (i = 0; i < total_shards; i++) {
        u64 shard_vid, shard_nid;
        u32 shard_crc;
        __u32 shard_len = 0;

        spin_lock(&pi->i_lock);
        if (!pi->ec_chunks || group_base + i >= pi->ec_chunk_count) {
            spin_unlock(&pi->i_lock);
            pr_warn("powerfs: EC read ino=%llu shard=%u ec_chunks changed\n",
                    (unsigned long long)ino, i);
            continue;
        }
        shard_vid = pi->ec_chunks[group_base + i].volume_id;
        shard_nid = pi->ec_chunks[group_base + i].needle_id;
        shard_crc = pi->ec_chunks[group_base + i].crc32;
        spin_unlock(&pi->i_lock);

        shards[i] = kmalloc(POWERFS_CHUNK_SIZE, GFP_KERNEL);
        if (!shards[i])
            continue;

        ret = powerfs_net_read_needle(shard_vid, shard_nid,
                                       shards[i], POWERFS_CHUNK_SIZE,
                                       &shard_len);
        if (ret < 0) {
            pr_warn("powerfs: EC read ino=%llu group=%llu shard=%u "
                    "vid=%llu nid=%llu failed: %d\n",
                    (unsigned long long)ino, (unsigned long long)group_idx,
                    i, (unsigned long long)shard_vid,
                    (unsigned long long)shard_nid, ret);
            kfree(shards[i]);
            shards[i] = NULL;
            continue;
        }

        /* K4-4: CRC32 校验, 不匹配视为缺失 (由 parity 重建). */
        if (shard_crc != 0 && shard_len > 0) {
            u32 crc_actual = crc32_le(~0, shards[i], shard_len) ^ ~0;
            if (crc_actual != shard_crc) {
                pr_warn("powerfs: EC CRC mismatch ino=%llu shard=%u "
                        "expected=%#x actual=%#x — will reconstruct\n",
                        (unsigned long long)ino, i, shard_crc, crc_actual);
                kfree(shards[i]);
                shards[i] = NULL;
                continue;
            }
        }
        available[i] = true;
    }

    /* 2. 统计可用 data shards, 选择 fast/slow path */
    {
        u32 data_available = 0;
        for (i = 0; i < data_shards; i++) {
            if (available[i])
                data_available++;
        }

        if (data_available == data_shards) {
            /* Fast path: 所有 data shards 可用 → 直接拼接 */
            group_data = kvmalloc(group_data_size, GFP_KERNEL);
            if (!group_data) {
                ret = -ENOMEM;
                goto out;
            }
            for (i = 0; i < data_shards; i++) {
                memcpy(group_data + (u64)i * POWERFS_CHUNK_SIZE,
                       shards[i], POWERFS_CHUNK_SIZE);
            }
        } else {
            /* K4-6: Slow path — 降级重建 */
            struct powerfs_ec_codec *codec;

            pr_info("powerfs: EC read ino=%llu group=%llu degraded "
                    "data=%u/%u — reconstructing\n",
                    (unsigned long long)ino, (unsigned long long)group_idx,
                    data_available, data_shards);

            codec = powerfs_ec_init(data_shards, parity_shards);
            if (IS_ERR(codec)) {
                ret = PTR_ERR(codec);
                goto out;
            }

            if (!powerfs_ec_can_recover(codec, available)) {
                pr_warn("powerfs: EC read ino=%llu cannot recover\n",
                        (unsigned long long)ino);
                powerfs_ec_free(codec);
                ret = -EIO;
                goto out;
            }

            ret = powerfs_ec_decode(codec, shards, available,
                                    POWERFS_CHUNK_SIZE);
            powerfs_ec_free(codec);
            if (ret) {
                pr_warn("powerfs: EC decode ino=%llu failed: %d\n",
                        (unsigned long long)ino, ret);
                goto out;
            }

            group_data = kvmalloc(group_data_size, GFP_KERNEL);
            if (!group_data) {
                ret = -ENOMEM;
                goto out;
            }
            for (i = 0; i < data_shards; i++) {
                if (shards[i])
                    memcpy(group_data + (u64)i * POWERFS_CHUNK_SIZE,
                           shards[i], POWERFS_CHUNK_SIZE);
            }
        }
    }

    /* 3. 从 group_data 提取请求范围 */
    copy_len = min_t(size_t, length, group_data_size - group_offset);
    copy_len = min_t(size_t, copy_len, buf_cap);
    memcpy(buf, group_data + group_offset, copy_len);
    *read_len = (__u32)copy_len;
    ret = 0;

out:
    kvfree(group_data);
    if (shards) {
        for (i = 0; i < total_shards; i++)
            kfree(shards[i]);
        kfree(shards);
    }
    kfree(available);
    return ret;
}

/**
 * powerfs_net_read - 读数据 (直连 Volume Server, 不经过 Filer)
 *
 * 数据读写绕过 Filer: 通过 powerfs_locate_chunk 按 offset 定位 (volume_id,
 * needle_id), 直连 Volume Server 读取 needle 内容, 再按 offset/length 截取.
 *
 * K3: 统一 Flat/Stripe/WideStripe 多卷布局.
 *   - Flat:   volume_id 固定, needle_id = file_key + offset / CHUNK_SIZE
 *   - Stripe: volume_id = volume_ids[stripe_unit_idx],
 *             needle_id = file_key + chunk_idx_in_unit
 *
 * needle 模型: 每个 needle = 1 chunk (POWERFS_CHUNK_SIZE=1MB), 整存整取.
 *   offset_in_needle = offset % CHUNK_SIZE
 *
 * 跨 needle 读取: 逐个 needle 读取, 拷贝对应区间到 buf.
 *
 * 参数:
 *   pi: powerfs_inode_info (用于 locate_chunk, 持 i_lock 快照定位信息)
 *   ino: inode 号 (用于 lease 校验, 传给 volume server)
 *   offset/length: 文件内偏移和读取长度
 *   buf/buf_cap: 输出缓冲区
 *   read_len: 输出, 实际读取字节数
 */
int powerfs_net_read(struct powerfs_inode_info *pi, __u64 ino,
                     __u64 offset, __u32 length,
                     __u8 *buf, size_t buf_cap, __u32 *read_len)
{
    __u8 *needle_buf;
    __u32 total_read = 0;
    __u64 cur_offset = offset;
    __u32 remaining = length;
    int ret;

    if (!pi)
        return -EINVAL;

    if (buf_cap < length)
        return -EINVAL;

    /* K4-5: EC 模式走专用读取路径 (降级重建). */
    if (pi->reliability == POWERFS_RELIABILITY_EC && pi->ec_chunks)
        return powerfs_net_read_ec(pi, ino, offset, length,
                                    buf, buf_cap, read_len);

    /* needle_buf 用于接收整个 needle (1MB) 内容.
     * kvmalloc 在大尺寸时自动回退 vmalloc, 适合 1MB. */
    needle_buf = kvmalloc(POWERFS_CHUNK_SIZE, GFP_KERNEL);
    if (!needle_buf)
        return -ENOMEM;

    /* 逐 needle 读取, 拷贝请求区间到 buf.
     * K3: 每个 needle 按 cur_offset 调用 powerfs_locate_chunk 定位,
     *     持 i_lock 快照 (volume_id, needle_id) 后释放锁做网络 I/O. */
    while (remaining > 0) {
        __u64 volume_id, needle_id;
        size_t offset_in_needle = cur_offset % POWERFS_CHUNK_SIZE;
        __u32 chunk_read_len = 0;
        __u32 to_copy;

        spin_lock(&pi->i_lock);
        ret = powerfs_locate_chunk(pi, cur_offset, &volume_id, &needle_id);
        spin_unlock(&pi->i_lock);
        if (ret) {
            pr_warn("powerfs: read locate ino=%llu offset=%llu failed: %d\n",
                    (unsigned long long)ino,
                    (unsigned long long)cur_offset, ret);
            kvfree(needle_buf);
            return ret;
        }

        /* 读取整个 needle (网络 I/O, 无锁) */
        ret = powerfs_net_read_needle(volume_id, needle_id,
                                       needle_buf, POWERFS_CHUNK_SIZE,
                                       &chunk_read_len);
        if (ret < 0 && ret != -ENOENT) {
            /* K4-2: 读 failover — 主 volume 失败时从 replica_chunks 重读.
             * 对齐 FUSE read failover (cache.rs): 遍历 replica_chunks
             * 找到 chunk_idx 匹配的副本, 用其 (volume_id, needle_id) 重读.
             * 对齐项目约束: "所有请求断连时入队列等待, 不降级到本地缓存" —
             * 这里 failover 是切换到副本 volume, 不是降级到本地缓存. */
            if (pi->replica_chunks && pi->replica_count > 0) {
                __u32 chunk_idx = cur_offset / POWERFS_CHUNK_SIZE;
                __u32 i;
                bool failed_over = false;

                spin_lock(&pi->i_lock);
                for (i = 0; i < pi->replica_count; i++) {
                    if (pi->replica_chunks[i].chunk_idx == chunk_idx) {
                        __u64 rep_vid = pi->replica_chunks[i].volume_id;
                        __u64 rep_nid = pi->replica_chunks[i].needle_id;
                        __u32 rep_crc = pi->replica_chunks[i].crc32;
                        spin_unlock(&pi->i_lock);

                        pr_warn("powerfs: read failover ino=%llu chunk=%u "
                                "primary vid=%llu nid=%llu failed=%d → "
                                "replica vid=%llu nid=%llu\n",
                                (unsigned long long)ino, chunk_idx,
                                (unsigned long long)volume_id,
                                (unsigned long long)needle_id, ret,
                                (unsigned long long)rep_vid,
                                (unsigned long long)rep_nid);

                        ret = powerfs_net_read_needle(rep_vid, rep_nid,
                                                       needle_buf,
                                                       POWERFS_CHUNK_SIZE,
                                                       &chunk_read_len);
                        /* 无论成功失败都标记 failed_over, 避免循环外
                         * if (!failed_over) spin_unlock 导致 double-unlock. */
                        failed_over = true;
                        if (ret == 0) {
                            /* K4-4: CRC32 校验 (failover 路径).
                             * rep_crc==0 跳过校验 (对齐项目约束).
                             * 对齐 FUSE crc32fast::hash: init=0xFFFFFFFF,
                             * final XOR 0xFFFFFFFF. */
                            if (rep_crc != 0 && chunk_read_len > 0) {
                                __u32 actual = crc32_le(~0, needle_buf,
                                                        chunk_read_len) ^ ~0;
                                if (actual != rep_crc) {
                                    pr_warn("powerfs: CRC mismatch (failover) "
                                            "ino=%llu chunk=%u expected=%#x "
                                            "actual=%#x\n",
                                            (unsigned long long)ino, chunk_idx,
                                            rep_crc, actual);
                                    kvfree(needle_buf);
                                    return -EIO;
                                }
                            }
                        }
                        break;
                    }
                }
                if (!failed_over)
                    spin_unlock(&pi->i_lock);
            }
        }
        if (ret < 0 && ret != -ENOENT) {
            pr_warn("powerfs: read_needle vid=%llu nid=%llu failed: %d\n",
                    (unsigned long long)volume_id,
                    (unsigned long long)needle_id, ret);
            kvfree(needle_buf);
            return ret;
        }
        if (ret == -ENOENT) {
            /* needle 不存在 (稀疏文件 hole / 未写入的 chunk):
             * 当作空 needle 处理, 后续零填充逻辑生效.
             * 不 break, 因为 hole 后面可能还有已写入的 needle. */
            chunk_read_len = 0;
            ret = 0;  /* 后续 break 判断用 ret==0 */
        }

        /* K4-4: CRC32 校验 (主路径).
         * 读成功后, 若 replica_chunks 中有匹配 chunk_idx 的 crc32, 校验数据完整性.
         * crc32==0 跳过校验 (对齐项目约束).
         * 注意: 仅校验 needle_buf 全量数据 (chunk_read_len), 不校验部分拷贝. */
        if (ret == 0 && chunk_read_len > 0 &&
            pi->replica_chunks && pi->replica_count > 0) {
            __u32 chunk_idx = cur_offset / POWERFS_CHUNK_SIZE;
            __u32 i;
            __u32 expected_crc = 0;
            bool found = false;

            spin_lock(&pi->i_lock);
            for (i = 0; i < pi->replica_count; i++) {
                if (pi->replica_chunks[i].chunk_idx == chunk_idx) {
                    expected_crc = pi->replica_chunks[i].crc32;
                    found = true;
                    break;
                }
            }
            spin_unlock(&pi->i_lock);

            if (found && expected_crc != 0) {
                /* K4-4: 对齐 FUSE crc32fast::hash: init=0xFFFFFFFF, final XOR. */
                __u32 actual_crc = crc32_le(~0, needle_buf, chunk_read_len) ^ ~0;
                if (actual_crc != expected_crc) {
                    pr_warn("powerfs: CRC mismatch (primary) ino=%llu chunk=%u "
                            "expected=%#x actual=%#x\n",
                            (unsigned long long)ino, chunk_idx,
                            expected_crc, actual_crc);
                    kvfree(needle_buf);
                    return -EIO;
                }
            }
        }

        /* 拷贝请求区间: [offset_in_needle, min(offset_in_needle+remaining, chunk_read_len)) */
        to_copy = min_t(__u32, remaining, (__u32)chunk_read_len);
        if (offset_in_needle >= chunk_read_len) {
            /* 请求区间超出 needle 实际内容, 剩余部分填零 (文件尾稀疏区域) */
            to_copy = min_t(__u32, remaining,
                            (__u32)(POWERFS_CHUNK_SIZE - offset_in_needle));
            memset(buf + total_read, 0, to_copy);
        } else {
            to_copy = min_t(__u32, to_copy,
                            (__u32)(chunk_read_len - offset_in_needle));
            memcpy(buf + total_read, needle_buf + offset_in_needle, to_copy);
        }

        total_read += to_copy;
        cur_offset += to_copy;
        remaining -= to_copy;

        /* needle 存在但内容不足且未到 chunk 末尾, 说明文件已结束.
         * 注意: needle 不存在 (ENOENT) 时不 break, 因为稀疏文件
         * hole 后面可能还有已写入的 needle. */
        if (ret == 0 && chunk_read_len < POWERFS_CHUNK_SIZE &&
            offset_in_needle + to_copy >= chunk_read_len)
            break;
    }

    kvfree(needle_buf);

    if (read_len)
        *read_len = total_read;

    return 0;
}


/* ========== Volume 直连 API (数据读写不经过 Filer) ==========
 *
 * 架构: 内核 → powerfs-net → Volume Server (WriteNeedle/ReadNeedle)
 * Filer 只负责元数据. 数据路径完全 bypass Filer.
 */

int powerfs_net_get_volume_count(void)
{
    return g_pool.volume_count;
}

struct powerfs_net_server_conn *powerfs_net_get_volume_conn(int idx)
{
    if (idx < 0 || idx >= g_pool.volume_count)
        return NULL;
    if (!g_pool.volumes[idx].in_use)
        return NULL;
    return &g_pool.volumes[idx];
}

/* 判断 msg_type 是否走 meta 通路 (lease 小请求).
 * meta 通路与 data 通路 (write/read needle 大帧) 物理分离,
 * 避免大帧 kernel_sendmsg 阻塞 lease 续约导致 -110 超时. */
bool pfs_is_meta_msg(__u16 msg_type)
{
    switch (msg_type) {
    case POWERFS_NET_MSG_ACQUIRE_LEASE:
    case POWERFS_NET_MSG_RELEASE_LEASE:
    case POWERFS_NET_MSG_RENEW_LEASE:
    case POWERFS_NET_MSG_RANGE_LEASE:
    case POWERFS_NET_MSG_VOLUME_STATUS:
        return true;
    default:
        return false;
    }
}

/* 在 volumes[] 中按 addr+port+type 精确查找已有连接 (discover_volumes 去重用).
 * 调用者需确保 volume_count 稳定 (持有 pool_lock 或在初始化阶段). */
int pfs_find_vol_conn_by_addr(const char *ip, __u16 port,
                                     enum powerfs_net_server_type type)
{
    int i;

    for (i = 0; i < g_pool.volume_count; i++) {
        struct powerfs_net_server_conn *conn = &g_pool.volumes[i];
        if (conn->in_use && conn->type == type &&
            conn->port == port && strcmp(conn->addr, ip) == 0)
            return i;
    }
    return -1;
}

/* 确保 volume server 连接存在 (查找已有或新建).
 * data 通路用 POWERFS_NET_SERVER_VOLUME, meta 通路用 POWERFS_NET_SERVER_VOLUME_META.
 * 两条连接到同一 addr:port 但 type 不同, 各自独立 socket/tx_queue/sched 投递,
 * 物理隔离 write_needle 大帧与 lease 小请求.
 * 返回 conn_idx (>=0) 或负错误码. */
int pfs_ensure_volume_conn(const char *ip, __u16 port,
                                  enum powerfs_net_server_type type)
{
    struct powerfs_net_server_conn *conn;
    int idx;

    mutex_lock(&g_pool.pool_lock);

    /* 查找已有连接 (按 addr+port+type 去重) */
    idx = pfs_find_vol_conn_by_addr(ip, port, type);
    if (idx >= 0) {
        mutex_unlock(&g_pool.pool_lock);
        return idx;
    }

    /* 新建连接 */
    if (g_pool.volume_count >= POWERFS_NET_MAX_VOLUMES) {
        mutex_unlock(&g_pool.pool_lock);
        pr_warn("powerfs: volume pool full, cannot add %s:%u (type=%d)\n",
                ip, port, type);
        return -ENOSPC;
    }

    idx = g_pool.volume_count;
    conn = &g_pool.volumes[idx];
    memset(conn, 0, sizeof(*conn));

    strncpy(conn->addr, ip, sizeof(conn->addr) - 1);
    conn->port = port;
    conn->type = type;
    conn->in_use = true;
    conn->sock = NULL;
    conn->state = CONN_INIT;
    atomic_set(&conn->seq_counter, 1);
    conn->reconnect_count = 0;
    conn->reconnect_delay = 0;
    atomic_set(&conn->consecutive_timeouts, 0);

    spin_lock_init(&conn->state_lock);
    init_waitqueue_head(&conn->sock_user_wq);
    init_waitqueue_head(&conn->reconnect_wq);
    INIT_DELAYED_WORK(&conn->reconnect_work, powerfs_conn_reconnect_work_fn);
    INIT_LIST_HEAD(&conn->pending_reqs);
    conn->req_tree = RB_ROOT;
    spin_lock_init(&conn->req_lock);
    INIT_WORK(&conn->disconnect_work, powerfs_conn_disconnect_work_fn);
    conn->sched = pfs_pick_vol_sched(conn->addr, conn->type);
    INIT_LIST_HEAD(&conn->rx_list);
    INIT_LIST_HEAD(&conn->tx_list);
    conn->rx_ready = 0;
    conn->rx_scheduled = 0;
    conn->tx_ready = 0;
    conn->tx_scheduled = 0;
    INIT_LIST_HEAD(&conn->tx_queue);
    spin_lock_init(&conn->tx_lock);
    conn->saved_data_ready = NULL;
    conn->saved_write_space = NULL;
    conn->saved_state_change = NULL;
    conn->saved_error_report = NULL;
    kref_init(&conn->kref);

    /* v2: per-conn RX buffer (与 pool_init 路径一致, 支持非阻塞断点续收).
     * 缺失会导致 pfs_rx_step 写入 NULL iov_base → NULL deref oops. */
    if (pfs_conn_alloc_rxbuffers(conn)) {
        pr_err("powerfs: vol_route %s:%u alloc rx buffers failed\n", ip, port);
        conn->in_use = false;
        mutex_unlock(&g_pool.pool_lock);
        return -ENOMEM;
    }

    g_pool.volume_count++;
    mutex_unlock(&g_pool.pool_lock);

    /* 后台建立 TCP 连接 (不阻塞 mount) */
    queue_delayed_work(g_pool.reconn_wq, &conn->reconnect_work, 0);

    pr_info("powerfs: vol_route: auto-connected %s:%u (type=%d, %s)\n",
            ip, port, type,
            type == POWERFS_NET_SERVER_VOLUME_META ? "meta" : "data");
    return idx;
}

/* 按 volume_id 查找 volume 连接 (vol_routes 路由表 → fallback 首个已连接).
 * is_meta=true 返回 meta conn (lease), false 返回 data conn (needle). */
struct powerfs_net_server_conn *powerfs_net_find_volume_conn(__u64 volume_id,
                                                             bool is_meta)
{
    int i;

    spin_lock(&g_pool.vol_route_lock);
    for (i = 0; i < g_pool.vol_route_count; i++) {
        if (g_pool.vol_routes[i].volume_id == volume_id) {
            int idx = is_meta ? g_pool.vol_routes[i].meta_conn_idx
                              : g_pool.vol_routes[i].conn_idx;
            spin_unlock(&g_pool.vol_route_lock);
            if (idx < 0)
                return NULL;
            return powerfs_net_get_volume_conn(idx);
        }
    }
    spin_unlock(&g_pool.vol_route_lock);

    /* Fallback: vol_routes 未命中, 按 type 找首个已连接的 volume conn */
    {
        enum powerfs_net_server_type want = is_meta
            ? POWERFS_NET_SERVER_VOLUME_META : POWERFS_NET_SERVER_VOLUME;

        for (i = 0; i < g_pool.volume_count; i++) {
            struct powerfs_net_server_conn *conn = &g_pool.volumes[i];
            if (conn->in_use && conn->type == want &&
                conn->state == CONN_CONNECTED) {
                pr_warn("powerfs: find_volume_conn: volume_id=%llu not in vol_routes (%d routes), fallback to volumes[%d] %s:%u (meta=%d)\n",
                        (unsigned long long)volume_id, g_pool.vol_route_count,
                        i, conn->addr, conn->port, is_meta);
                return conn;
            }
        }
    }
    return NULL;
}

/*
 * powerfs_net_send_to_volume - 发送请求直连到 volume server
 *
 * 与 powerfs_net_send_request 区别: bypass shard 路由, 直接用 volume 连接.
 * 复用 powerfs_request_do_send (同一套调度器 + 异步收发).
 */
int powerfs_net_send_to_volume(int vol_idx, __u64 volume_id,
                                __u16 msg_type,
                                const __u8 *body, size_t body_len,
                                const __u8 *data, size_t data_len,
                                __u8 *resp_body, size_t resp_body_cap,
                                __u8 *resp_data, size_t resp_data_cap,
                                int timeout_ms,
                                size_t *resp_body_len_out,
                                size_t *resp_data_len_out)
{
    struct powerfs_request *req;
    struct powerfs_net_server_conn *conn;
    int ret;

    if (atomic_read(&g_pool.stopping))
        return -ENOTCONN;

    if (vol_idx >= 0)
        conn = powerfs_net_get_volume_conn(vol_idx);
    else
        conn = powerfs_net_find_volume_conn(volume_id, pfs_is_meta_msg(msg_type));

    if (!conn) {
        pr_warn("powerfs: send_to_volume: no volume conn for volume_id=%llu\n",
                (unsigned long long)volume_id);
        return -ENOTCONN;
    }

    req = powerfs_request_alloc(msg_type, GFP_KERNEL);
    if (!req)
        return -ENOMEM;

    req->req_body = body;
    req->req_body_len = body_len;
    req->req_data = data;
    req->req_data_len = data_len;
    req->resp_body = resp_body;
    req->resp_body_cap = resp_body_cap;
    req->resp_data = resp_data;
    req->resp_data_cap = resp_data_cap;
    req->shard_id = 0;
    if (timeout_ms > 0)
        req->deadline = jiffies + msecs_to_jiffies(timeout_ms);

    /* 直接在 volume 连接上发送 (bypass shard 路由) */
    {
        int flow_idx = pfs_conn_flow_idx(conn);
        powerfs_flow_record_start(flow_idx,
                                  req->req_body_len + req->req_data_len);
    }

    ret = powerfs_request_do_send(req, conn);

    /* Phase 2: do_send 失败时补 record_complete, 防止 active_reqs 泄漏 */
    if (ret != 0) {
        powerfs_flow_record_complete(pfs_conn_flow_idx(conn),
                                     0, 0, true);
    }

    if (resp_body_len_out)
        *resp_body_len_out = req->resp_body_len;
    if (resp_data_len_out)
        *resp_data_len_out = req->resp_data_len;

    if (ret < 0) {
        powerfs_request_free(req);
        return ret;
    }

    ret = req->resp_status;
    powerfs_request_free(req);
    return ret;
}

/*
 * powerfs_net_write_needle - 直连 volume 写数据 (WriteNeedle)
 *
 * TLV 编码: Ino(volume_id) + FileKey + Inode + [LeaseToken] + DataLen(data)
 * 与 Volume Server handle_write_needle 匹配.
 * lease_token: 可选, 非 NULL 且 token_len>0 时发送, Volume Server 校验.
 */
int powerfs_net_write_needle(__u64 volume_id, __u64 file_key, __u64 inode,
                             const __u8 *data, size_t data_len,
                             const char *lease_token, size_t token_len)
{
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[64];
    size_t resp_body_len = 0;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, volume_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_FILE_KEY, file_key);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INODE_V2, inode);
    /* Lease token (可选): Volume Server 校验 lease 有效性 */
    if (lease_token && token_len > 0 && token_len < 64)
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LEASE_TOKEN,
                               lease_token, token_len);
    /* ClientId: 必须与 acquire_lease 的 client_id 一致, 否则 volume server
     * validate_token_with_grace_period 报 "Lease holder mismatch".
     * 之前内核未发送 ClientId, volume server 用 session_client_id (TCP 连接
     * 的数字 session id) 作为 holder, 与 acquire_lease 注册的 "kernel-client"
     * 不匹配, 导致所有带 lease 的写失败. */
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID,
                           "kernel-client", strlen("kernel-client"));
    /* DataLen 字段标识 data 段存在; Volume Server 用 next_bytes(DataLen) 读取 */

    ret = powerfs_net_send_to_volume(-1, volume_id,
                                      POWERFS_NET_MSG_WRITE_NEEDLE,
                                      body, powerfs_tlv_enc_len(&enc),
                                      data, data_len,
                                      resp_body, sizeof(resp_body),
                                      NULL, 0, 30000,
                                      &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}

/*
 * powerfs_net_read_needle - 直连 volume 读数据 (ReadNeedle)
 *
 * TLV 编码: Ino(volume_id) + FileKey
 * 响应: body 为空, data 段为 needle 完整内容 (与 Volume Server handle_read_needle 匹配:
 *   build_response(msg, STATUS_OK, Vec::new(), data) — data 在 data 段).
 */
int powerfs_net_read_needle(__u64 volume_id, __u64 file_key,
                            __u8 *buf, size_t buf_cap, __u32 *read_len)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    size_t resp_data_len = 0;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, volume_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_FILE_KEY, file_key);

    /* buf 作为 resp_data 传入: Volume Server 把 needle 内容放在 data 段.
     * 之前误将 buf 作为 resp_body, 导致 read_len 恒为 0. */
    ret = powerfs_net_send_to_volume(-1, volume_id,
                                      POWERFS_NET_MSG_READ_NEEDLE,
                                      body, powerfs_tlv_enc_len(&enc),
                                      NULL, 0,
                                      NULL, 0,
                                      buf, buf_cap, 30000,
                                      NULL, &resp_data_len);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    if (read_len)
        *read_len = (__u32)resp_data_len;

    return 0;
}

/* ========== 异步 WriteNeedle / ReadNeedle (page writeback 异步提交) ==========
 *
 * 与同步版本共享 powerfs_request_do_send, 但设置 req->callback 使 do_send
 * 入队后立即返回 (不 wait_for_completion). 响应到达时由调度器经
 * powerfs_req_complete 调用 callback.
 *
 * 缓冲区生命周期: 调用方提供的 req_body/resp_body/resp_data/data 必须持久
 * 存活到 callback 触发 (通常放在 callback 所属的 ctx 结构体中).
 */

/* powerfs_net_write_needle_async - 异步写 needle
 *
 * 返回 0: 提交成功, callback 将被调用 (调用方不得再访问 req)
 * 返回 <0: 提交失败, callback 不会被调用, 调用方自行清理 */
int powerfs_net_write_needle_async(__u64 volume_id, __u64 file_key, __u64 inode,
                                   const __u8 *data, size_t data_len,
                                   const char *lease_token, size_t token_len,
                                   __u8 *req_body, size_t req_body_cap,
                                   __u8 *resp_body, size_t resp_body_cap,
                                   int timeout_ms,
                                   int (*callback)(struct powerfs_request *),
                                   void *priv)
{
    struct powerfs_tlv_enc enc;
    struct powerfs_net_server_conn *conn;
    struct powerfs_request *req;
    int ret;

    if (atomic_read(&g_pool.stopping))
        return -ENOTCONN;

    /* TLV 编码到调用方提供的持久缓冲区 (callback 触发前不可释放) */
    powerfs_tlv_enc_init(&enc, req_body, req_body_cap);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, volume_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_FILE_KEY, file_key);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INODE_V2, inode);
    if (lease_token && token_len > 0 && token_len < 64)
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LEASE_TOKEN,
                               lease_token, token_len);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_ID,
                           "kernel-client", strlen("kernel-client"));

    conn = powerfs_net_find_volume_conn(volume_id, false /* data 通路 */);
    if (!conn) {
        pr_warn("powerfs: write_needle_async: no volume conn for volume_id=%llu\n",
                (unsigned long long)volume_id);
        return -ENOTCONN;
    }

    req = powerfs_request_alloc(POWERFS_NET_MSG_WRITE_NEEDLE, GFP_NOFS);
    if (!req)
        return -ENOMEM;

    req->req_body = req_body;
    req->req_body_len = powerfs_tlv_enc_len(&enc);
    req->req_data = data;
    req->req_data_len = data_len;
    req->resp_body = resp_body;
    req->resp_body_cap = resp_body_cap;
    req->resp_data = NULL;
    req->resp_data_cap = 0;
    req->shard_id = 0;
    req->filer = conn;  /* timeout_work 需要从 req->filer 获取 conn */
    req->callback = callback;
    req->priv = priv;
    if (timeout_ms > 0)
        req->deadline = jiffies + msecs_to_jiffies(timeout_ms);

    /* 流控统计 (与 send_to_volume 一致) */
    {
        int flow_idx = pfs_conn_flow_idx(conn);
        powerfs_flow_record_start(flow_idx,
                                  req->req_body_len + req->req_data_len);
    }

    ret = powerfs_request_do_send(req, conn);
    if (ret != 0) {
        /* 提交失败: do_send 未入队 (或入队后立即被拒), callback 不会触发.
         * 补 record_complete 防止 active_reqs 泄漏, 释放 req, 返回错误. */
        powerfs_flow_record_complete(pfs_conn_flow_idx(conn), 0, 0, true);
        powerfs_request_free(req);
        return ret;
    }

    /* 提交成功: do_send 已武装 timeout_work 并返回 0.
     * 响应/超时/断连将触发 powerfs_req_complete → callback.
     * callback 内负责 powerfs_request_free(req). */
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_write_needle_async);

/* powerfs_net_read_needle_async - 异步读 needle (writeback RMW 读)
 *
 * 响应 data 段写入 resp_data (调用方提供, 通常为 needle_buf).
 * callback 内通过 req->resp_data_len 获取实际读取长度.
 *
 * 返回 0: 提交成功, callback 将被调用
 * 返回 <0: 提交失败, callback 不会被调用 */
int powerfs_net_read_needle_async(__u64 volume_id, __u64 file_key,
                                  __u8 *resp_data, size_t resp_data_cap,
                                  __u8 *req_body, size_t req_body_cap,
                                  int timeout_ms,
                                  int (*callback)(struct powerfs_request *),
                                  void *priv)
{
    struct powerfs_tlv_enc enc;
    struct powerfs_net_server_conn *conn;
    struct powerfs_request *req;
    int ret;

    if (atomic_read(&g_pool.stopping))
        return -ENOTCONN;

    powerfs_tlv_enc_init(&enc, req_body, req_body_cap);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, volume_id);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_FILE_KEY, file_key);

    conn = powerfs_net_find_volume_conn(volume_id, false /* data 通路 */);
    if (!conn) {
        pr_warn("powerfs: read_needle_async: no volume conn for volume_id=%llu\n",
                (unsigned long long)volume_id);
        return -ENOTCONN;
    }

    req = powerfs_request_alloc(POWERFS_NET_MSG_READ_NEEDLE, GFP_NOFS);
    if (!req)
        return -ENOMEM;

    req->req_body = req_body;
    req->req_body_len = powerfs_tlv_enc_len(&enc);
    req->req_data = NULL;
    req->req_data_len = 0;
    req->resp_body = NULL;
    req->resp_body_cap = 0;
    req->resp_data = resp_data;
    req->resp_data_cap = resp_data_cap;
    req->shard_id = 0;
    req->filer = conn;
    req->callback = callback;
    req->priv = priv;
    if (timeout_ms > 0)
        req->deadline = jiffies + msecs_to_jiffies(timeout_ms);

    {
        int flow_idx = pfs_conn_flow_idx(conn);
        powerfs_flow_record_start(flow_idx, req->req_body_len);
    }

    ret = powerfs_request_do_send(req, conn);
    if (ret != 0) {
        powerfs_flow_record_complete(pfs_conn_flow_idx(conn), 0, 0, true);
        powerfs_request_free(req);
        return ret;
    }

    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_read_needle_async);

