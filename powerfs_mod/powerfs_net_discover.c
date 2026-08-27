/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_discover.c - split from powerfs_net.c (mechanical refactor) */

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


/* ========== Master-based Filer Discovery ========== */

/*
 * powerfs_net_discover_filers - Query Master for filer list and add to pool.
 *
 * Flow:
 *   1. Parse comma-separated master_addrs
 *   2. For each master addr: connect → handshake → send LIST_FILERS
 *   3. If REDIRECT, follow redirect to actual master leader
 *   4. Parse response TLV: count + per-filer (addr, port, healthy)
 *   5. add_server each healthy filer
 *
 * Returns filer count added (>0) or negative error.
 */
int powerfs_net_discover_filers(const char *master_addrs, __u16 master_port)
{
    char addr_buf[256];
    char *p, *tok;
    int filers_added = 0;
    int i;
    /* 响应 body 可能较大 (filer 列表), 必须用 kmalloc 避免栈溢出.
     * POWERFS_NET_MAX_BODY=64KB, 内核栈仅 8-16KB. */
    __u8 *resp_body;
    __u8 resp_data[64];

    if (!master_addrs || !master_addrs[0])
        return -EINVAL;

    resp_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    if (!resp_body) {
        pr_err("powerfs: discover_filers: kmalloc resp_body failed\n");
        return -ENOMEM;
    }

    strncpy(addr_buf, master_addrs, sizeof(addr_buf) - 1);
    addr_buf[sizeof(addr_buf) - 1] = '\0';

    p = addr_buf;
    while ((tok = strsep(&p, ",")) != NULL) {
        struct socket *sock = NULL;
        size_t body_len = 0, data_len = 0;
        struct powerfs_net_frame_hdr hdr;
        __u32 seq;
        int ret;

        while (*tok == ' ')
            tok++;
        if (tok[0] == '\0')
            continue;

        pr_debug("powerfs: trying master %s:%u for filer discovery\n",
                tok, master_port);

        /* 1. Connect to Master */
        sock = powerfs_net_create_tcp_socket();
        if (!sock)
            continue;

        ret = powerfs_net_tcp_connect(sock, tok, master_port);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        /* 2. Handshake */
        ret = powerfs_net_do_handshake(sock);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        /* 3. Send LIST_FILERS request (empty body) */
        seq = atomic_inc_return(&g_discover_seq);
        powerfs_net_frame_hdr_encode(&hdr,
                                      POWERFS_NET_MSG_LIST_FILERS,
                                      POWERFS_NET_FLAG_REQUEST,
                                      seq, 0, 0, 0, 0);

        ret = powerfs_net_frame_send(sock, &hdr, NULL, 0, NULL, 0);
        if (ret < 0) {
            pr_warn("powerfs: list_filers send failed: %d\n", ret);
            powerfs_net_close_socket(sock);
            continue;
        }

        /* 4. Receive response (loop to skip NOTIFY frames) */
        for (i = 0; i < 5; i++) {
            ret = powerfs_net_frame_recv(sock, &hdr,
                                         resp_body, POWERFS_NET_MAX_BODY, &body_len,
                                         resp_data, sizeof(resp_data), &data_len,
                                         POWERFS_NET_RECV_TIMEOUT);
            if (ret < 0)
                break;

            /* Skip NOTIFY frames, wait for our response */
            if (hdr.flags & POWERFS_NET_FLAG_NOTIFY)
                continue;
            break;
        }

        powerfs_net_close_socket(sock);

        if (ret < 0) {
            pr_warn("powerfs: list_filers recv failed: %d\n", ret);
            continue;
        }

        /* 5. Check for REDIRECT */
        if (hdr.status == POWERFS_NET_STATUS_ERR_REDIRECT) {
            /* Parse redirect addr from body */
            struct powerfs_tlv_dec dec;
            char redirect_addr[64];
            int rret;

            powerfs_tlv_dec_init(&dec, resp_body, body_len);
            rret = powerfs_tlv_dec_string(&dec,
                                          POWERFS_NET_FLD_OWNER,
                                          redirect_addr,
                                          sizeof(redirect_addr) - 1);
            if (rret == 0) {
                redirect_addr[sizeof(redirect_addr) - 1] = '\0';
                if (redirect_addr[0] == '\0') {
                    pr_warn("powerfs: master redirect with empty leader addr (master election in progress?), skipping\n");
                    powerfs_net_close_socket(sock);
                    continue;
                }
                pr_debug("powerfs: master redirect to %s\n",
                        redirect_addr);

                /* Retry with redirect address */
                sock = powerfs_net_create_tcp_socket();
                if (!sock)
                    continue;

                ret = powerfs_net_tcp_connect(sock, redirect_addr,
                                               master_port);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }

                ret = powerfs_net_do_handshake(sock);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }

                seq = atomic_inc_return(&g_discover_seq);
                powerfs_net_frame_hdr_encode(&hdr,
                                              POWERFS_NET_MSG_LIST_FILERS,
                                              POWERFS_NET_FLAG_REQUEST,
                                              seq, 0, 0, 0, 0);

                ret = powerfs_net_frame_send(sock, &hdr, NULL, 0, NULL, 0);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }

                for (i = 0; i < 5; i++) {
                    ret = powerfs_net_frame_recv(sock, &hdr,
                                                 resp_body, POWERFS_NET_MAX_BODY,
                                                 &body_len,
                                                 resp_data, sizeof(resp_data),
                                                 &data_len,
                                                 POWERFS_NET_RECV_TIMEOUT);
                    if (ret < 0)
                        break;
                    if (hdr.flags & POWERFS_NET_FLAG_NOTIFY)
                        continue;
                    break;
                }

                powerfs_net_close_socket(sock);

                if (ret < 0) {
                    pr_warn("powerfs: list_filers redirect recv failed: %d\n",
                            ret);
                    continue;
                }
            }
        }

        /* 6. Check status */
        if (hdr.status != POWERFS_NET_STATUS_OK) {
            pr_warn("powerfs: list_filers status=%u\n", hdr.status);
            continue;
        }

        /* 7. Parse filer list from TLV response */
        {
            struct powerfs_tlv_dec dec;
            __u64 count = 0;
            __u8 field;
            size_t flen;
            int j;

            powerfs_tlv_dec_init(&dec, resp_body, body_len);

            /* Read filer count */
            if (powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_ENTRIES,
                                    &count) != 0) {
                pr_warn("powerfs: list_filers: no Entries field\n");
                continue;
            }

            pr_debug("powerfs: master returned %llu filers\n",
                    (u64)count);

            for (j = 0; j < (int)count; j++) {
                char faddr[64];
                __u64 fport = 0;
                __u8 healthy = 0;
                int err = 0;

                err |= powerfs_tlv_dec_string(&dec,
                                               POWERFS_NET_FLD_OWNER,
                                               faddr, sizeof(faddr) - 1);
                err |= powerfs_tlv_dec_u64(&dec,
                                           POWERFS_NET_FLD_BLKSIZE,
                                           &fport);
                err |= powerfs_tlv_dec_u8(&dec,
                                          POWERFS_NET_FLD_IS_DIR,
                                          &healthy);

                if (err != 0) {
                    pr_warn("powerfs: list_filers: parse error at filer %d\n",
                            j);
                    break;
                }

                faddr[sizeof(faddr) - 1] = '\0';

                /* master 返回的 address 可能是 "ip:port" 格式 (filer 注册时
                 * 用 advertise_addr), 而连接池期望 addr=纯 IP + port=端口.
                 * 去掉 faddr 中的端口部分, 统一用 Blksize 字段的 fport. */
                {
                    char *colon = strrchr(faddr, ':');
                    if (colon) {
                        /* IPv6 地址可能含多个 ':', 但 filer 地址是 IPv4,
                         * 只处理最后一个 ':' 后面是纯数字的情况 */
                        char *endp;
                        unsigned long vp = simple_strtoul(colon + 1, &endp, 10);
                        if (*endp == '\0' && vp > 0 && vp <= 0xFFFF) {
                            *colon = '\0';
                            /* 若 Blksize 字段未返回端口, 用 addr 中的端口 */
                            if (fport == 0)
                                fport = vp;
                        }
                    }
                }

                if (!healthy) {
                    pr_debug("powerfs: skipping unhealthy filer %s:%llu\n",
                            faddr, (u64)fport);
                    continue;
                }

                powerfs_net_add_server(faddr, (__u16)fport,
                                       POWERFS_NET_SERVER_FILER);
                pr_debug("powerfs: discovered filer %s:%llu\n",
                        faddr, (u64)fport);
                filers_added++;
            }
        }

        /* Success - don't try other masters */
        break;
    }

    kfree(resp_body);

    pr_debug("powerfs: filer discovery complete, %d filers added\n",
            filers_added);
    return filers_added > 0 ? filers_added : -ENOTCONN;
}

/*
 * powerfs_net_discover_volumes - 从 Master GetTopology 获取 volume 路由表
 *
 * 填充 g_pool.vol_routes[]: volume_id → conn_idx 映射.
 * 用于 ReadNeedle 按 volume_id 找到正确的 Volume Server 连接.
 *
 * 请求: GET_TOPOLOGY (空 body)
 * 响应 TLV: Owner(leader) + Entries(count) + [VolumeId + Owner(addr) + Size] × N
 */
int powerfs_net_discover_volumes(const char *master_addrs, __u16 master_port)
{
    char addr_buf[256];
    char *p, *tok;
    __u8 *resp_body;
    __u8 resp_data[64];
    size_t body_len = 0, data_len = 0;
    struct powerfs_net_frame_hdr hdr;
    __u32 seq;
    int ret, i;

    if (!master_addrs || !master_addrs[0])
        return -EINVAL;

    resp_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    if (!resp_body)
        return -ENOMEM;

    strncpy(addr_buf, master_addrs, sizeof(addr_buf) - 1);
    addr_buf[sizeof(addr_buf) - 1] = '\0';

    p = addr_buf;
    while ((tok = strsep(&p, ",")) != NULL) {
        struct socket *sock = NULL;
        struct powerfs_tlv_dec dec;
        __u64 count = 0;
        int routes_added = 0;

        while (*tok == ' ')
            tok++;
        if (tok[0] == '\0')
            continue;

        pr_debug("powerfs: discover_volumes: trying master %s:%u\n", tok, master_port);

        sock = powerfs_net_create_tcp_socket();
        if (!sock)
            continue;

        ret = powerfs_net_tcp_connect(sock, tok, master_port);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        ret = powerfs_net_do_handshake(sock);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        seq = atomic_inc_return(&g_discover_seq);
        powerfs_net_frame_hdr_encode(&hdr,
                                      POWERFS_NET_MSG_GET_TOPOLOGY,
                                      POWERFS_NET_FLAG_REQUEST,
                                      seq, 0, 0, 0, 0);

        ret = powerfs_net_frame_send(sock, &hdr, NULL, 0, NULL, 0);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        for (i = 0; i < 5; i++) {
            ret = powerfs_net_frame_recv(sock, &hdr,
                                          resp_body, POWERFS_NET_MAX_BODY, &body_len,
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
            pr_warn("powerfs: discover_volumes recv failed: %d\n", ret);
            continue;
        }

        /* 处理 REDIRECT */
        if (hdr.status == POWERFS_NET_STATUS_ERR_REDIRECT) {
            struct powerfs_tlv_dec rdec;
            char redirect_addr[64];

            powerfs_tlv_dec_init(&rdec, resp_body, body_len);
            if (powerfs_tlv_dec_string(&rdec, POWERFS_NET_FLD_OWNER,
                                        redirect_addr,
                                        sizeof(redirect_addr) - 1) == 0) {
                redirect_addr[sizeof(redirect_addr) - 1] = '\0';
                pr_debug("powerfs: discover_volumes redirect to %s\n", redirect_addr);

                sock = powerfs_net_create_tcp_socket();
                if (!sock)
                    continue;
                ret = powerfs_net_tcp_connect(sock, redirect_addr, master_port);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }
                ret = powerfs_net_do_handshake(sock);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }
                seq = atomic_inc_return(&g_discover_seq);
                powerfs_net_frame_hdr_encode(&hdr,
                                              POWERFS_NET_MSG_GET_TOPOLOGY,
                                              POWERFS_NET_FLAG_REQUEST,
                                              seq, 0, 0, 0, 0);
                ret = powerfs_net_frame_send(sock, &hdr, NULL, 0, NULL, 0);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }
                for (i = 0; i < 5; i++) {
                    ret = powerfs_net_frame_recv(sock, &hdr,
                                                  resp_body, POWERFS_NET_MAX_BODY,
                                                  &body_len, resp_data, sizeof(resp_data),
                                                  &data_len, POWERFS_NET_RECV_TIMEOUT);
                    if (ret < 0)
                        break;
                    if (hdr.flags & POWERFS_NET_FLAG_NOTIFY)
                        continue;
                    break;
                }
                powerfs_net_close_socket(sock);
                if (ret < 0)
                    continue;
            }
        }

        if (hdr.status != POWERFS_NET_STATUS_OK) {
            pr_warn("powerfs: discover_volumes status=%u\n", hdr.status);
            continue;
        }

        /* 解析拓扑: Owner(leader) + Entries(count) + per-volume(VolumeId + Owner + Size) */
        powerfs_tlv_dec_init(&dec, resp_body, body_len);

        /* 跳过 leader addr (Owner 字段) */
        {
            char leader_addr[64];
            powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_OWNER,
                                    leader_addr, sizeof(leader_addr) - 1);
        }

        if (powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_ENTRIES, &count) != 0) {
            pr_warn("powerfs: discover_volumes: no Entries field\n");
            continue;
        }

        pr_debug("powerfs: master returned %llu volume routes\n", (u64)count);

        /* P3.3a: 解析拓扑到临时数组 (无锁, 避免 spinlock 内调用睡眠函数).
         * 未匹配的 route 自动建立新连接, volume_addr 降级为 fallback. */
        {
            struct {
                __u64 volume_id;
                char addr[64];
            } routes_tmp[POWERFS_NET_MAX_VOLUMES];
            int route_count_tmp = 0;
            int j;

            /* 1. 解析所有 routes 到临时数组.
             * master 编码顺序: VolumeId + Owner(addr) + Size + UsedSpace + FileCount
             * 必须按顺序解析全部 5 个字段, 否则 dec->pos 错位导致后续条目解析失败. */
            for (i = 0; i < (int)count && i < POWERFS_NET_MAX_VOLUMES; i++) {
                __u64 vid = 0, vsize = 0, vused = 0, vfiles = 0;
                char vaddr[64] = {0};

                powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_VOLUME_ID, &vid);
                powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_OWNER, vaddr,
                                        sizeof(vaddr) - 1);
                powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, &vsize);
                powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_USED_SPACE, &vused);
                powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_FILE_COUNT, &vfiles);
                vaddr[sizeof(vaddr) - 1] = '\0';

                /* 跳过 addr 为空的无效条目 (避免 vol_route invalid 警告刷屏) */
                if (vaddr[0] == '\0') {
                    pr_warn("powerfs: discover_volumes: skip empty addr route vid=%llu\n",
                            (unsigned long long)vid);
                    continue;
                }

                routes_tmp[route_count_tmp].volume_id = vid;
                strncpy(routes_tmp[route_count_tmp].addr, vaddr,
                        sizeof(routes_tmp[route_count_tmp].addr) - 1);
                routes_tmp[route_count_tmp].addr[sizeof(routes_tmp[route_count_tmp].addr) - 1] = '\0';
                route_count_tmp++;
            }

            /* 2. 对每个 route: 精确匹配已有连接, 未匹配则自动建连.
             * P3.3a: 移除前缀匹配隐患 (strncmp), 只用 strcmp 精确匹配. */
            spin_lock(&g_pool.vol_route_lock);
            g_pool.vol_route_count = 0;

            for (i = 0; i < route_count_tmp; i++) {
                __u64 vid = routes_tmp[i].volume_id;
                char *vaddr = routes_tmp[i].addr;
                int data_idx = -1, meta_idx = -1;
                char v_ip[64] = {0};
                __u16 v_port = 0;
                char *colon;
                int ip_len;

                /* 解析 "ip:net_port" */
                colon = strrchr(vaddr, ':');
                if (!colon) {
                    pr_warn("powerfs: vol_route: volume_id=%llu addr=%s invalid (no port)\n",
                            (unsigned long long)vid, vaddr);
                    continue;
                }
                ip_len = min_t(int, (int)(colon - vaddr), (int)(sizeof(v_ip) - 1));
                memcpy(v_ip, vaddr, ip_len);
                v_ip[ip_len] = '\0';
                v_port = (__u16)simple_strtoul(colon + 1, NULL, 10);

                /* 释放 vol_route_lock (spinlock 不可睡眠, pfs_ensure_volume_conn
                 * 内部用 pool_lock mutex 保护 volumes[] 修改) */
                spin_unlock(&g_pool.vol_route_lock);

                /* data 通路 (write/read needle 大帧) */
                data_idx = pfs_ensure_volume_conn(v_ip, v_port,
                                                   POWERFS_NET_SERVER_VOLUME);
                /* meta 通路 (lease 小请求) — 与 data 物理分离,
                 * 独立 socket/tx_queue, 不被大帧 kernel_sendmsg 阻塞 */
                meta_idx = pfs_ensure_volume_conn(v_ip, v_port,
                                                   POWERFS_NET_SERVER_VOLUME_META);

                spin_lock(&g_pool.vol_route_lock);

                if (data_idx < 0 && meta_idx < 0) {
                    pr_warn("powerfs: vol_route: failed to create conn for %s\n",
                            vaddr);
                    continue;
                }

                /* 建立 volume_id → (data_idx, meta_idx) 映射 */
                g_pool.vol_routes[g_pool.vol_route_count].volume_id = vid;
                g_pool.vol_routes[g_pool.vol_route_count].conn_idx = data_idx;
                g_pool.vol_routes[g_pool.vol_route_count].meta_conn_idx = meta_idx;
                g_pool.vol_route_count++;
                routes_added++;
                pr_debug("powerfs: vol_route: volume_id=%llu → data[%d] meta[%d] (%s)\n",
                        (unsigned long long)vid, data_idx, meta_idx, vaddr);
            }
            spin_unlock(&g_pool.vol_route_lock);
        }

        /* Parse ShardMap from topology response.
         * Prefer Master-provided entries (0xBD, 25B per entry) to stay
         * in sync with the Filer after shard splits. Fall back to
         * TotalShards (0xB8) → from_shard_count, or the module param
         * (already applied in pool_init) if neither is present. */
        {
            const __u8 *entries_blob = NULL;
            size_t entries_len = 0;
            __u64 total_shards = 0;
            int sm_ret;

            /* powerfs_tlv_dec_find_raw resets dec->pos to 0 and scans
             * from the start, so order of these lookups doesn't matter. */
            sm_ret = powerfs_tlv_dec_find_raw(&dec,
                                               POWERFS_NET_FLD_SHARD_MAP_ENTRIES,
                                               &entries_blob, &entries_len);
            if (sm_ret == 0 && entries_len > 0) {
                if (shard_map_from_entries(entries_blob, entries_len) == 0) {
                    pr_info("powerfs: ShardMap updated from Master entries (0xBD, %zu bytes)\n",
                            entries_len);
                } else {
                    pr_warn("powerfs: ShardMap entries (0xBD) parse failed, keeping fallback map\n");
                }
            } else if (powerfs_tlv_dec_find_u64(&dec,
                                                 POWERFS_NET_FLD_TOTAL_SHARDS,
                                                 &total_shards) == 0
                       && total_shards > 0) {
                shard_map_from_shard_count(total_shards);
                pr_info("powerfs: ShardMap updated from TotalShards (0xB8=%llu)\n",
                        (u64)total_shards);
            } else {
                pr_info("powerfs: Master topology has no ShardMap fields, keeping conn-pool-init fallback (shard_count=%llu)\n",
                        (unsigned long long)g_pool.shard_route.shard_count);
            }
        }

        /* Parse ShardLeaderEntries (0x9F) to pre-fill shard→leader routing.
         *
         * Encoding (matches Master net_handler.rs + powerfs-master-net client.rs):
         *   ShardLeaderEntries = u64(count)
         *   followed by N × (ShardId=0x70 (u64) + FilerAddress=0x9C (string "ip:port"))
         *
         * When present, every shard_id's first RPC to the pool hits the true
         * leader directly (zero-redirect fast path) instead of waiting for a
         * STATUS_ERR_REDIRECT from a follower. */
        {
            u64 leader_count = 0;
            int sl_parsed = 0, sl_applied = 0;

            if (powerfs_tlv_dec_find_u64(&dec,
                                          POWERFS_NET_FLD_SHARD_LEADER_ENTRIES,
                                          &leader_count) == 0 && leader_count > 0) {
                u64 idx;
                pr_info("powerfs: Master topology has %llu ShardLeaderEntries, applying pre-route\n",
                        (unsigned long long)leader_count);

                for (idx = 0; idx < leader_count; idx++) {
                    u64 sid = 0;
                    char leader_addr[64] = {0};
                    char *colon;
                    char ip[64] = {0};
                    __u16 port = 0;
                    int filer_idx = -1;
                    int j;

                    /* NOTE: sequential next_u64 / next_string within the
                     * ShardLeaderEntries group — the Master encoder emits
                     * N × (ShardId + FilerAddress) back-to-back, and the
                     * TLV decoder's linear scan matches emission order.
                     * (Same logic as Rust master-net client.rs L361-L368.) */
                    if (powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SHARD_ID, &sid) < 0) {
                        pr_warn("powerfs: ShardLeaderEntries[%llu]: missing ShardId, stop\n",
                                (unsigned long long)idx);
                        break;
                    }
                    if (powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_FILER_ADDRESS,
                                                leader_addr, sizeof(leader_addr) - 1) < 0) {
                        pr_warn("powerfs: ShardLeaderEntries[%llu]: missing FilerAddress, stop\n",
                                (unsigned long long)idx);
                        break;
                    }
                    sl_parsed++;
                    leader_addr[sizeof(leader_addr) - 1] = '\0';

                    if (leader_addr[0] == '\0') {
                        pr_debug("powerfs: ShardLeaderEntries[%llu] sid=%llu empty addr, skip\n",
                                (unsigned long long)idx, (unsigned long long)sid);
                        continue;
                    }
                    if (sid >= POWERFS_MAX_SHARDS) {
                        pr_warn("powerfs: ShardLeaderEntries[%llu] sid=%llu >= MAX(%u), skip\n",
                                (unsigned long long)idx, (unsigned long long)sid,
                                POWERFS_MAX_SHARDS);
                        continue;
                    }

                    /* parse "ip:net_port" */
                    colon = strrchr(leader_addr, ':');
                    if (!colon) {
                        pr_warn("powerfs: ShardLeaderEntries[%llu] addr=%s invalid (no port), skip\n",
                                (unsigned long long)idx, leader_addr);
                        continue;
                    }
                    {
                        int ip_len = min_t(int, (int)(colon - leader_addr), (int)(sizeof(ip) - 1));
                        memcpy(ip, leader_addr, ip_len);
                        ip[ip_len] = '\0';
                    }
                    port = (__u16)simple_strtoul(colon + 1, NULL, 10);
                    if (port == 0) {
                        pr_warn("powerfs: ShardLeaderEntries[%llu] addr=%s invalid port, skip\n",
                                (unsigned long long)idx, leader_addr);
                        continue;
                    }

                    /* exact-match in g_pool.filers[] by addr+port.
                     *
                     * BUG FIX (previously): we used servers[] index `j` as
                     * filer_idx for powerfs_shard_route_update, but
                     * get_filer_for_shard indexes into g_pool.filers[] (NOT
                     * g_pool.servers[]).  When servers[] contained
                     * non-FILER entries (MASTER, etc.) before the first
                     * FILER entry, or when servers[]/filers[] had different
                     * ordering, the two indices mismatched — yielding an
                     * out-of-range leader_filer_idx that caused
                     * get_filer_for_shard to return NULL and every request
                     * to time out.
                     *
                     * Fix: scan the actual filers[] conn pool which is
                     * populated by conn_pool_init and matches the
                     * leader_filer_idx semantic (index into filers[]). */
                    filer_idx = -1;
                    for (j = 0; j < g_pool.filer_count; j++) {
                        if (g_pool.filers[j].in_use &&
                            strcmp(g_pool.filers[j].addr, ip) == 0 &&
                            g_pool.filers[j].port == port) {
                            filer_idx = j;
                            break;
                        }
                    }
                    if (filer_idx < 0) {
                        /* not yet in filers[] conn pool: skip pre-route for
                         * this shard; submit() fallback to ROUTE_CHECKING +
                         * round-robin will do lazy discovery and update the
                         * leader via REDIRECT handling. */
                        pr_info("powerfs: ShardLeaderEntries[%llu] sid=%llu leader %s:%u not in filers[] pool (count=%d); "
                                "skipping prefill, lazy discovery will apply\n",
                                (unsigned long long)idx, (unsigned long long)sid,
                                ip, (unsigned)port, g_pool.filer_count);
                    }

                    if (filer_idx >= 0) {
                        powerfs_shard_route_update(sid, filer_idx);
                        sl_applied++;
                        pr_debug("powerfs: shard_route prefill sid=%llu → filer[%d] %s:%u VALID\n",
                                (unsigned long long)sid, filer_idx, ip, port);
                    } else {
                        pr_warn("powerfs: ShardLeaderEntries[%llu] sid=%llu addr=%s: pool full, skip\n",
                                (unsigned long long)idx, (unsigned long long)sid, leader_addr);
                    }
                }
                pr_info("powerfs: ShardLeaderEntries: parsed=%d, applied VALID route=%d (zero-redirect fast path)\n",
                        sl_parsed, sl_applied);
            } else {
                pr_info("powerfs: Master topology has no ShardLeaderEntries (0x9F); shard routes will be discovered lazily via redirect\n");
            }
        }

        pr_info("powerfs: discover_volumes complete, %d routes added (vol_route_count=%d)\n",
                routes_added, g_pool.vol_route_count);
        kfree(resp_body);
        return routes_added > 0 ? 0 : -ENOTCONN;
    }

    kfree(resp_body);
    pr_warn("powerfs: discover_volumes failed (no master reachable)\n");
    return -ENOTCONN;
}

/**
 * powerfs_net_set_volume - 设置 Volume 地址
 */
int powerfs_net_set_volume(const char *addr, __u16 port)
{
    return powerfs_net_add_server(addr, port, POWERFS_NET_SERVER_VOLUME);
}

/* topology_refresh_worker — asynchronous handler for TopologyChanged(0x0072).
 *
 * Runs on g_pool.reconn_wq (WQ_UNBOUND) so blocking socket I/O is allowed.
 * Re-issues GET_TOPOLOGY via powerfs_net_discover_volumes() which walks all
 * configured master addresses (for failover), parses TotalShards/ShardMap
 * plus the newly added ShardLeaderEntries(0x9F), and updates every shard's
 * shard_route to the new leader VALID — keeping the zero-redirect fast
 * path live across leader elections.
 *
 * After completion the pending bit is cleared so the next TopologyChanged
 * broadcast can trigger a fresh refresh. If discover_volumes fails (all
 * masters unreachable, REDIRECT storms, etc.) we leave it to subsequent
 * notifications (or the lazy redirect fallback on the next RPC) to heal. */
void topology_refresh_worker(struct work_struct *work)
{
    int rc;
    unsigned int old_failover;

    (void)work;

    if (powerfs_net_is_stopping()) {
        pr_info("powerfs: topology-refresh worker skipped (stopping=1)\n");
        goto out_clear;
    }
    if (!g_pool.master_set) {
        pr_warn("powerfs: topology-refresh worker: master address not set; skip\n");
        goto out_clear;
    }

    pr_info("powerfs: topology-refresh worker START — re-GET_TOPOLOGY from master %s:%u to refresh shard leader routes\n",
            g_pool.master_addr, (unsigned)g_pool.master_port);

    old_failover = atomic_read(&g_pool.failover_count);

    /* powerfs_net_discover_volumes() does the full work:
     *   1. for each master_addr (failover) connect → GET_TOPOLOGY;
     *   2. parse TotalShards + ShardMap (volume route update);
     *   3. parse ShardLeaderEntries(0x9F) → powerfs_shard_route_update
     *      per shard → route_state=VALID;
     *   4. returns filer_count (>0) on success, negative errno otherwise.
     *
     * Even when volume routes didn't change (common case: only per-shard
     * leaders flipped), step (3) refreshes shard_route for every shard,
     * so the zero-redirect fast path stays hot. */
    rc = powerfs_net_discover_volumes(g_pool.master_addr, g_pool.master_port);

    pr_info("powerfs: topology-refresh worker DONE rc=%d failover_delta=%u — zero-redirect shard leader routes now REFRESHED\n",
            rc,
            (unsigned)(atomic_read(&g_pool.failover_count) - old_failover));

out_clear:
    smp_mb__before_atomic();
    clear_bit(TOPOLOGY_REFRESH_PENDING_BIT, &g_topology_refresh_flags);
    smp_mb__after_atomic();
}

/*
 * powerfs_read_pem_file - 内核态读取 PEM 文本文件 (证书/密钥).
 *
 * 使用 filp_open + kernel_read (6.17 不再需要 set_fs), 缓冲区采用
 * 调用方指定 gfp 标记, VFS 回调场景传 GFP_NOFS 避免递归回收.
 *
 * 返回: kmalloc 缓冲区, NUL-terminated (可直接 strlen / TLV 编码).
 *   若 out_len 非 NULL, 写入实际字节数 (不含 trailing NUL).
 *   失败 (路径不存在/IO错误/空文件/alloc 失败) 返回 NULL.
 *   调用方必须 kfree() 返回指针.
 */
char *powerfs_read_pem_file(const char *path, gfp_t gfp, size_t *out_len)
{
    struct file *filp = NULL;
    struct kstat st;
    loff_t pos = 0;
    char *buf = NULL;
    ssize_t nread;
    int rc;

    if (!path || !*path)
        return NULL;

    filp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(filp)) {
        pr_warn("powerfs: pem_open: cannot open %s (ld=%ld)\n",
                path, PTR_ERR(filp));
        return NULL;
    }

    rc = vfs_getattr_nosec(&filp->f_path, &st,
                            STATX_SIZE, AT_STATX_SYNC_AS_STAT);
    if (rc != 0) {
        pr_warn("powerfs: pem_open: vfs_getattr %s failed (%d)\n", path, rc);
        goto out_fput;
    }
    if (st.size == 0 || st.size > 4 * 1024 * 1024) {
        pr_warn("powerfs: pem_open: %s size=%llu invalid (want (0,4MB])\n",
                path, (unsigned long long)st.size);
        goto out_fput;
    }

    /* +1 trailing NUL so caller can use as C string. */
    buf = kmalloc((size_t)st.size + 1, gfp);
    if (!buf) {
        pr_warn("powerfs: pem_open: kmalloc %llu+1 failed\n",
                (unsigned long long)st.size);
        goto out_fput;
    }

    nread = kernel_read(filp, buf, (size_t)st.size, &pos);
    if (nread < 0 || (size_t)nread != (size_t)st.size) {
        pr_warn("powerfs: pem_open: kernel_read %s failed (%zd want %llu)\n",
                path, nread, (unsigned long long)st.size);
        kfree(buf);
        buf = NULL;
        goto out_fput;
    }
    buf[nread] = '\0';
    if (out_len)
        *out_len = (size_t)nread;

out_fput:
    filp_close(filp, NULL);
    return buf;
}
EXPORT_SYMBOL_GPL(powerfs_read_pem_file);

/*
 * powerfs_net_register_client - 向 Master 注册本 mount 的 client_uuid,
 * 获取 master 统一分配的 assigned_client_id(u64), 同时检查黑名单.
 *
 * 请求 TLV: ClientUuid + Backend(type) + Name(mount) + Collection + Replication
 *           + Owner(host) + Limit(pid)
 * 响应 TLV: ClientId(assigned u64) + Owner(leader) + MountAllowed(u8)
 *           + [Message(deny reason)]
 *
 * 状态:
 *   STATUS_OK = 允许挂载
 *   STATUS_ERR_PERMISSION = 黑名单拒绝 (仍解码输出参数)
 *   STATUS_ERR_REDIRECT = 非 leader, 内部自动跟随重定向重试一次
 *
 * 返回: 0 = 成功收到响应 (无论 mount_allowed 与否), <0 = -errno 网络错误
 */
int powerfs_net_register_client(const char *master_addrs, __u16 master_port,
                                const char *client_uuid, const char *client_type,
                                const char *mount_point, const char *collection,
                                const char *replication, const char *host,
                                __u64 pid,
                                const char *client_crt_path,
                                __u64 *out_assigned_id, bool *out_mount_allowed,
                                char *out_reason, size_t reason_cap)
{
    char addr_buf[256];
    char *mp, *mtok;
    __u8 *req_body;
    __u8 *resp_body;
    __u8 resp_data[64];
    size_t req_body_len = 0, body_len = 0, data_len = 0;
    struct powerfs_net_frame_hdr hdr;
    __u32 seq;
    int ret, ri;
    pr_err("powerfs: CRC_TRACE ENTER register_client master_addrs=%s port=%u\n",
           master_addrs ? master_addrs : "(null)", master_port);
    bool handled = false;
    char *cert_pem = NULL;
    size_t cert_len = 0;

    if (!master_addrs || !master_addrs[0] || !client_uuid || !out_assigned_id ||
        !out_mount_allowed)
        return -EINVAL;

    *out_assigned_id = 0;
    *out_mount_allowed = false;
    if (out_reason && reason_cap > 0)
        out_reason[0] = '\0';

    /* 尽早把证书 PEM 读到内核堆, 路径空/读失败仅 warn, master 强制模式
     * 会因缺少 0xD4 字段直接 PERMISSION_DENIED 返回, 不在这里 abort. */
    if (client_crt_path && client_crt_path[0]) {
        cert_pem = powerfs_read_pem_file(client_crt_path, GFP_NOFS, &cert_len);
        if (!cert_pem)
            pr_warn("powerfs: register_client: client_crt not readable %s "
                    "(mount will fail if master enforces cert)\n",
                    client_crt_path);
        else
            pr_info("powerfs: register_client: loaded client_crt '%s' (%zuB)\n",
                    client_crt_path, cert_len);
    }

    req_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    resp_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    if (!req_body || !resp_body) {
        kfree(req_body);
        kfree(resp_body);
        kfree(cert_pem);
        pr_err("powerfs: register_client: kmalloc failed\n");
        return -ENOMEM;
    }

    /* 编码请求 TLV body (一次编码, 主请求和 REDIRECT 重试都复用) */
    {
        struct powerfs_tlv_enc enc;
        powerfs_tlv_enc_init(&enc, req_body, POWERFS_NET_MAX_BODY);
        if (client_uuid)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_UUID,
                                   client_uuid, strlen(client_uuid));
        if (client_type)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_BACKEND,
                                   client_type, strlen(client_type));
        if (mount_point)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                                   mount_point, strlen(mount_point));
        if (collection)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_COLLECTION,
                                   collection, strlen(collection));
        if (replication)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_REPLICATION,
                                   replication, strlen(replication));
        if (host)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_OWNER,
                                   host, strlen(host));
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_LIMIT, pid);
        /* ClientCert 0xD4: 若成功读到 PEM, 嵌入 TLV, 长度 = strlen(PEM). */
        if (cert_pem && cert_len > 0)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_CERT,
                                   cert_pem, cert_len);
        req_body_len = powerfs_tlv_enc_len(&enc);
    }
    /* PEM 编码完成即可释放, 避免后续网络循环期间长期占用堆. */
    kfree(cert_pem);
    cert_pem = NULL;

    strncpy(addr_buf, master_addrs, sizeof(addr_buf) - 1);
    addr_buf[sizeof(addr_buf) - 1] = '\0';

    mp = addr_buf;
    while ((mtok = strsep(&mp, ",")) != NULL) {
        struct socket *sock = NULL;

        while (*mtok == ' ')
            mtok++;
        if (mtok[0] == '\0')
            continue;

        pr_debug("powerfs: register_client: trying master %s:%u\n",
                 mtok, master_port);
        pr_err("powerfs: CRC_TRACE trying master %s:%u\n", mtok, master_port);

        sock = powerfs_net_create_tcp_socket();
        if (!sock) {
            pr_err("powerfs: CRC_TRACE create_tcp_socket failed\n");
            continue;
        }
        pr_err("powerfs: CRC_TRACE tcp_socket ok\n");

        ret = powerfs_net_tcp_connect(sock, mtok, master_port);
        if (ret < 0) {
            pr_err("powerfs: CRC_TRACE tcp_connect failed ret=%d\n", ret);
            powerfs_net_close_socket(sock);
            continue;
        }
        pr_err("powerfs: CRC_TRACE tcp_connect ok\n");

        ret = powerfs_net_do_handshake(sock);
        if (ret < 0) {
            pr_err("powerfs: CRC_TRACE do_handshake failed ret=%d\n", ret);
            powerfs_net_close_socket(sock);
            continue;
        }
        pr_err("powerfs: CRC_TRACE do_handshake ok\n");

        seq = atomic_inc_return(&g_discover_seq);
        pr_err("powerfs: CRC_TRACE before encode, seq=0x%08x req_body_len=%zu\n", seq, req_body_len);
        powerfs_net_frame_hdr_encode(&hdr,
                                      POWERFS_NET_MSG_REGISTER_CLIENT,
                                      POWERFS_NET_FLAG_REQUEST,
                                      seq, 0, (__u32)req_body_len, (__u32)req_body_len, 0);

        ret = powerfs_net_frame_send(sock, &hdr, req_body, req_body_len, NULL, 0);
        if (ret < 0) {
            pr_warn("powerfs: register_client send failed: %d\n", ret);
            powerfs_net_close_socket(sock);
            continue;
        }

        for (ri = 0; ri < 5; ri++) {
            ret = powerfs_net_frame_recv(sock, &hdr,
                                          resp_body, POWERFS_NET_MAX_BODY, &body_len,
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
            pr_warn("powerfs: register_client recv failed: %d\n", ret);
            continue;
        }

        /* 处理 REDIRECT: 跟随一次 */
        if (hdr.status == POWERFS_NET_STATUS_ERR_REDIRECT) {
            struct powerfs_tlv_dec rdec;
            char redirect_addr[64];

            powerfs_tlv_dec_init(&rdec, resp_body, body_len);
            if (powerfs_tlv_dec_string(&rdec, POWERFS_NET_FLD_OWNER,
                                        redirect_addr,
                                        sizeof(redirect_addr) - 1) == 0) {
                redirect_addr[sizeof(redirect_addr) - 1] = '\0';
                if (redirect_addr[0] == '\0') {
                    pr_warn("powerfs: register_client redirect empty (election?)\n");
                    continue;
                }
                pr_debug("powerfs: register_client redirect to %s\n",
                         redirect_addr);

                sock = powerfs_net_create_tcp_socket();
                if (!sock)
                    continue;
                ret = powerfs_net_tcp_connect(sock, redirect_addr, master_port);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }
                ret = powerfs_net_do_handshake(sock);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }
                seq = atomic_inc_return(&g_discover_seq);
                powerfs_net_frame_hdr_encode(&hdr,
                                              POWERFS_NET_MSG_REGISTER_CLIENT,
                                              POWERFS_NET_FLAG_REQUEST,
                                              seq, 0, (__u32)req_body_len, (__u32)req_body_len, 0);

                ret = powerfs_net_frame_send(sock, &hdr, req_body,
                                             req_body_len, NULL, 0);
                if (ret < 0) {
                    powerfs_net_close_socket(sock);
                    continue;
                }
                for (ri = 0; ri < 5; ri++) {
                    ret = powerfs_net_frame_recv(sock, &hdr,
                                                  resp_body, POWERFS_NET_MAX_BODY,
                                                  &body_len, resp_data, sizeof(resp_data),
                                                  &data_len, POWERFS_NET_RECV_TIMEOUT);
                    if (ret < 0)
                        break;
                    if (hdr.flags & POWERFS_NET_FLAG_NOTIFY)
                        continue;
                    break;
                }
                powerfs_net_close_socket(sock);
                if (ret < 0) {
                    pr_warn("powerfs: register_client redirect recv failed: %d\n", ret);
                    continue;
                }
            }
        }

        /* 无论 STATUS_OK 还是 STATUS_ERR_PERMISSION, 都解码响应 */
        if (hdr.status == POWERFS_NET_STATUS_OK ||
            hdr.status == POWERFS_NET_STATUS_ERR_PERMISSION) {
            struct powerfs_tlv_dec dec;
            __u8 mount_allowed_u8 = 0;

            powerfs_tlv_dec_init(&dec, resp_body, body_len);

            (void)powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_CLIENT_ID,
                                            out_assigned_id);

            if (powerfs_tlv_dec_find_u8(&dec, POWERFS_NET_FLD_MOUNT_ALLOWED,
                                         &mount_allowed_u8) == 0)
                *out_mount_allowed = (mount_allowed_u8 != 0);
            else
                *out_mount_allowed = (hdr.status == POWERFS_NET_STATUS_OK);

            /* Message: 拒绝理由 / 提示信息 (可选) */
            if (out_reason && reason_cap > 0) {
                const __u8 *msg_raw = NULL;
                size_t msg_len = 0;
                if (powerfs_tlv_dec_find_raw(&dec, POWERFS_NET_FLD_MESSAGE,
                                             &msg_raw, &msg_len) == 0 && msg_len > 0) {
                    size_t cpy = msg_len < (reason_cap - 1) ?
                                 msg_len : (reason_cap - 1);
                    memcpy(out_reason, msg_raw, cpy);
                    out_reason[cpy] = '\0';
                } else if (!*out_mount_allowed &&
                           reason_cap >= sizeof("client blacklisted by master")) {
                    strcpy(out_reason, "client blacklisted by master");
                }
            }

            handled = true;
            break;
        }

        pr_warn("powerfs: register_client unexpected status=%u\n", hdr.status);
        continue;
    }

    kfree(req_body);
    kfree(resp_body);

    if (!handled) {
        pr_err("powerfs: register_client: no master responded\n");
        return -ENOLINK;
    }
    return 0;
}

/*
 * powerfs_net_deregister_client - umount 时向 Master 优雅下线,
 * 提前移除 client_uuid 的心跳注册表项 (不用等到心跳超时).
 *
 * 请求 TLV: ClientUuid + ClientId(assigned u64)
 * 响应: STATUS_OK 即可
 */
int powerfs_net_deregister_client(const char *master_addrs, __u16 master_port,
                                  const char *client_uuid, __u64 assigned_id,
                                  const char *client_crt_path)
{
    char addr_buf[256];
    char *p, *tok;
    __u8 *req_body;
    __u8 *resp_body;
    __u8 resp_data[64];
    size_t body_len = 0, data_len = 0;
    struct powerfs_net_frame_hdr hdr;
    __u32 seq;
    int ret, i;
    int rc = -ENOLINK;
    char *cert_pem = NULL;
    size_t cert_len = 0;

    if (!master_addrs || !master_addrs[0] || !client_uuid)
        return -EINVAL;

    /* Deregister 同样校验证书: 防止任意主机伪造下线请求. 读失败仅 warn. */
    if (client_crt_path && client_crt_path[0]) {
        cert_pem = powerfs_read_pem_file(client_crt_path, GFP_NOFS, &cert_len);
        if (!cert_pem)
            pr_warn("powerfs: deregister_client: client_crt unreadable '%s' "
                    "(best-effort deregister will proceed; master may reject)\n",
                    client_crt_path);
    }

    req_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    resp_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    if (!req_body || !resp_body) {
        kfree(req_body);
        kfree(resp_body);
        kfree(cert_pem);
        return -ENOMEM;
    }

    {
        struct powerfs_tlv_enc enc;
        powerfs_tlv_enc_init(&enc, req_body, POWERFS_NET_MAX_BODY);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_UUID,
                               client_uuid, strlen(client_uuid));
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_CLIENT_ID, assigned_id);
        if (cert_pem && cert_len > 0)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_CERT,
                                   cert_pem, cert_len);
        body_len = powerfs_tlv_enc_len(&enc);
    }
    kfree(cert_pem);
    cert_pem = NULL;

    strncpy(addr_buf, master_addrs, sizeof(addr_buf) - 1);
    addr_buf[sizeof(addr_buf) - 1] = '\0';

    p = addr_buf;
    while ((tok = strsep(&p, ",")) != NULL) {
        struct socket *sock = NULL;

        while (*tok == ' ')
            tok++;
        if (tok[0] == '\0')
            continue;

        sock = powerfs_net_create_tcp_socket();
        if (!sock)
            continue;

        ret = powerfs_net_tcp_connect(sock, tok, master_port);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        ret = powerfs_net_do_handshake(sock);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        seq = atomic_inc_return(&g_discover_seq);
        powerfs_net_frame_hdr_encode(&hdr,
                                      POWERFS_NET_MSG_DEREGISTER_CLIENT,
                                      POWERFS_NET_FLAG_REQUEST,
                                      seq, 0, (__u32)body_len, (__u32)body_len, 0);

        ret = powerfs_net_frame_send(sock, &hdr, req_body, body_len, NULL, 0);
        if (ret < 0) {
            powerfs_net_close_socket(sock);
            continue;
        }

        for (i = 0; i < 5; i++) {
            ret = powerfs_net_frame_recv(sock, &hdr,
                                          resp_body, POWERFS_NET_MAX_BODY, &body_len,
                                          resp_data, sizeof(resp_data), &data_len,
                                          POWERFS_NET_RECV_TIMEOUT);
            if (ret < 0)
                break;
            if (hdr.flags & POWERFS_NET_FLAG_NOTIFY)
                continue;
            break;
        }
        powerfs_net_close_socket(sock);

        if (ret < 0)
            continue;

        if (hdr.status == POWERFS_NET_STATUS_ERR_REDIRECT) {
            struct powerfs_tlv_dec rdec;
            char redirect_addr[64];

            powerfs_tlv_dec_init(&rdec, resp_body, body_len);
            if (powerfs_tlv_dec_string(&rdec, POWERFS_NET_FLD_OWNER,
                                        redirect_addr,
                                        sizeof(redirect_addr) - 1) != 0 ||
                redirect_addr[0] == '\0')
                continue;
            pr_debug("powerfs: deregister_client redirect to %s\n", redirect_addr);

            sock = powerfs_net_create_tcp_socket();
            if (!sock)
                continue;
            ret = powerfs_net_tcp_connect(sock, redirect_addr, master_port);
            if (ret < 0) {
                powerfs_net_close_socket(sock);
                continue;
            }
            ret = powerfs_net_do_handshake(sock);
            if (ret < 0) {
                powerfs_net_close_socket(sock);
                continue;
            }
            seq = atomic_inc_return(&g_discover_seq);
            powerfs_net_frame_hdr_encode(&hdr,
                                          POWERFS_NET_MSG_DEREGISTER_CLIENT,
                                          POWERFS_NET_FLAG_REQUEST,
                                          seq, 0, (__u32)body_len, (__u32)body_len, 0);
            ret = powerfs_net_frame_send(sock, &hdr, req_body, body_len, NULL, 0);
            if (ret < 0) {
                powerfs_net_close_socket(sock);
                continue;
            }
            for (i = 0; i < 5; i++) {
                ret = powerfs_net_frame_recv(sock, &hdr,
                                              resp_body, POWERFS_NET_MAX_BODY,
                                              &body_len, resp_data, sizeof(resp_data),
                                              &data_len, POWERFS_NET_RECV_TIMEOUT);
                if (ret < 0)
                    break;
                if (hdr.flags & POWERFS_NET_FLAG_NOTIFY)
                    continue;
                break;
            }
            powerfs_net_close_socket(sock);
            if (ret < 0)
                continue;
        }

        if (hdr.status == POWERFS_NET_STATUS_OK) {
            rc = 0;
            break;
        }
    }

    kfree(req_body);
    kfree(resp_body);
    return rc;
}
