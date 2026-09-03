/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_conn.c - split from powerfs_net.c (mechanical refactor) */

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

/* ========== 连接状态查询 (新架构) ========== */

/**
 * powerfs_net_is_connected - 检查网络是否可用
 *
 * 新架构: 检查 g_pool 是否已初始化且有 filer 连接, 且未在 stopping 状态.
 * 供 powerfs_fs.c / powerfs_transport.c 判断是否走网络路径.
 */
bool powerfs_net_is_connected(void)
{
    return g_pool.filer_count > 0 && !atomic_read(&g_pool.stopping);
}

/* Phase 1: 最近断连窗口检查.
 * 返回 true 表示最近 window_ms 内发生过断连 (lookup/readdir 应使用短超时).
 * 实现: 读取 g_pool.last_disconnect_jiffies, 与 jiffies 比对.
 * 0 表示从未断连, 返回 false. */
bool powerfs_net_recently_disconnected(unsigned int window_ms)
{
    unsigned long last = atomic_long_read(&g_pool.last_disconnect_jiffies);
    unsigned long threshold;

    if (last == 0)
        return false;
    threshold = last + msecs_to_jiffies(window_ms);
    return time_before(jiffies, threshold);
}

/* Phase 1: 根据 当前连接状态 + 最近断连情况 选择超时.
 * - 完全离线: 返回 short_timeout_ms (调用方决定是否发请求)
 * - 已连接 + 非最近断连: 返回 POWERFS_NET_RECV_TIMEOUT (10s, RPC 充足时间)
 * - 已连接 + 最近断连窗口内: 返回 short_timeout_ms (快速失败, 让 VFS 重试)
 * - 未连接 + 最近断连: 返回 short_timeout_ms */
int powerfs_net_pick_timeout(int short_timeout_ms)
{
    if (!powerfs_net_is_connected() ||
        powerfs_net_recently_disconnected(POWERFS_RECENT_DISCONNECT_MS))
        return short_timeout_ms;
    return POWERFS_NET_RECV_TIMEOUT;
}

/* ========== 连接池实现 (新架构) ========== */

/* === 1. 辅助函数 === */

/* Get filer index in pool from conn pointer */
int powerfs_conn_get_filer_idx(struct powerfs_net_server_conn *conn)
{
    int i;

    if (!conn)
        return -1;
    for (i = 0; i < g_pool.filer_count; i++) {
        if (&g_pool.filers[i] == conn)
            return i;
    }
    return -1;
}

/* Per-conn handshake (stores in conn, not global g_conn) */
int powerfs_conn_do_handshake(struct socket *sock,
                                     struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_handshake_req req;
    struct powerfs_net_handshake_resp resp;
    struct msghdr msg;
    struct kvec iov;
    int ret;
    __u64 client_id;

    /* 构造握手请求 (20 字节，裸协议) */
    memcpy(req.magic, "PFSN", 4);
    req.version = POWERFS_NET_VERSION;
    req.client_type = POWERFS_NET_CLIENT_KERNEL;
    /* channel: volume meta 连接标记为 META, 其他标记为 DATA.
     * 服务端据此区分通路, 收帧时校验 route_hash channel 位. */
    req.channel = (conn->type == POWERFS_NET_SERVER_VOLUME_META)
                  ? POWERFS_NET_CHANNEL_META : POWERFS_NET_CHANNEL_DATA;
    req.reserved = 0;
    /* ROOT40: 使用 master 签发的 assigned_client_id (经 cert 验证) 作为
     * filer handshake client_id. g_pool.hb_assigned_client_id 由
     * powerfs_net_update_heartbeat_id() 在 RegisterClient 成功后设置.
     * 缺失时回退到 seq_counter+1000000 (仅单客户端 dev 场景可用). */
    client_id = g_pool.hb_assigned_client_id;
    if (client_id == 0)
        client_id = atomic_read(&conn->seq_counter) + 1000000;
    req.client_id = cpu_to_le64(client_id);
    req.features = 0;

    /* 记录到 conn, 供帧头 route_hash 计算用 */
    conn->client_id = client_id;
    conn->channel = req.channel;

    /* 发送裸握手请求 (20 字节) */
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)&req;
    iov.iov_len = sizeof(req);
    ret = kernel_sendmsg(sock, &msg, &iov, 1, sizeof(req));
    if (ret < 0) {
        pr_err("powerfs: conn handshake send failed: %d\n", ret);
        return ret;
    }

    /* 接收裸握手响应 (18 字节) */
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)&resp;
    iov.iov_len = sizeof(resp);
    ret = kernel_recvmsg(sock, &msg, &iov, 1, sizeof(resp), 0);
    if (ret < 0) {
        pr_err("powerfs: conn handshake recv failed: %d\n", ret);
        return ret;
    }

    /* 验证响应 */
    if (memcmp(resp.magic, "PFSN", 4) != 0) {
        pr_err("powerfs: conn handshake response bad magic\n");
        return -EINVAL;
    }

    if (resp.status != 0) {
        pr_err("powerfs: conn handshake rejected, status=%u\n", resp.status);
        return -EPERM;
    }

    conn->server_id = le64_to_cpu(resp.server_id);
    conn->server_features = le32_to_cpu(resp.features);

    pr_debug("powerfs: conn handshake OK, server_id=%llu\n",
            (unsigned long long)conn->server_id);
    return 0;
}

/* === 2. Shard 路由函数 === */

void powerfs_shard_route_on_filer_disconnect(int filer_idx)
{
    int i;

    if (filer_idx < 0 || filer_idx >= g_pool.filer_count)
        return;

    spin_lock(&g_pool.shard_route.lock);
    for (i = 0; i < POWERFS_MAX_SHARDS; i++) {
        if (g_pool.shard_route.entries[i].leader_filer_idx == filer_idx &&
            g_pool.shard_route.entries[i].state == ROUTE_VALID) {
            g_pool.shard_route.entries[i].state = ROUTE_CHECKING;
            pr_debug("powerfs: shard %d route -> CHECKING (filer %d down)\n",
                    i, filer_idx);
        }
    }
    spin_unlock(&g_pool.shard_route.lock);
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_on_filer_disconnect);

void powerfs_shard_route_on_filer_reconnect(int filer_idx)
{
    int i;
    bool need_wakeup[POWERFS_MAX_SHARDS] = {false};

    if (filer_idx < 0 || filer_idx >= g_pool.filer_count)
        return;

    spin_lock(&g_pool.shard_route.lock);
    for (i = 0; i < POWERFS_MAX_SHARDS; i++) {
        if (g_pool.shard_route.entries[i].leader_filer_idx == filer_idx) {
            if (g_pool.shard_route.entries[i].state == ROUTE_UNKNOWN) {
                g_pool.shard_route.entries[i].state = ROUTE_CHECKING;
                pr_debug("powerfs: shard %d route -> CHECKING (filer %d up)\n",
                        i, filer_idx);
            }
        }
        /* Wake up pending requests for ALL shards in CHECKING or UNKNOWN
         * state, not just those where this filer is the leader.
         * Reason: Raft leader may have changed during the outage. The old
         * leader (stored in leader_filer_idx) might still be down, but
         * this reconnected filer might be the new leader. Waking up
         * pending requests lets the submit loop retry with any connected
         * filer, which will either succeed or return REDIRECT to the
         * new leader. */
        if (g_pool.shard_route.entries[i].state == ROUTE_CHECKING ||
            g_pool.shard_route.entries[i].state == ROUTE_UNKNOWN) {
            need_wakeup[i] = true;
        }
    }
    spin_unlock(&g_pool.shard_route.lock);

    /* Wake up pending requests outside the route lock.
     * For shards where this filer is the leader, use dispatch_pending
     * (which sets req->filer to the leader connection).
     * For other shards, directly complete with -EAGAIN to let the
     * submit loop find any connected filer. */
    for (i = 0; i < POWERFS_MAX_SHARDS; i++) {
        if (!need_wakeup[i])
            continue;

        if (g_pool.shard_route.entries[i].leader_filer_idx == filer_idx) {
            /* Leader filer reconnected: use dispatch_pending (sets filer) */
            powerfs_shard_route_dispatch_pending(i);
        } else {
            /* Non-leader filer reconnected: wake up with -EAGAIN so
             * submit loop retries and finds any connected filer */
            struct powerfs_request *req, *tmp;
            LIST_HEAD(wake_list);

            spin_lock(&g_pool.shard_route.entries[i].req_lock);
            list_splice_init(&g_pool.shard_route.entries[i].pending_reqs,
                             &wake_list);
            spin_unlock(&g_pool.shard_route.entries[i].req_lock);

            list_for_each_entry_safe(req, tmp, &wake_list, list_node) {
                list_del_init(&req->list_node);
                req->error = -EAGAIN;
                pr_debug("powerfs: wake req seq=%u shard=%d -> -EAGAIN (non-leader filer %d up)\n",
                         req->seq, i, filer_idx);
                powerfs_req_complete(req);
            }
        }
    }
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_on_filer_reconnect);

void powerfs_shard_route_update(u64 shard_id, int filer_idx)
{
    enum powerfs_shard_route_state old_state;

    if (shard_id >= POWERFS_MAX_SHARDS)
        return;

    spin_lock(&g_pool.shard_route.lock);
    old_state = g_pool.shard_route.entries[shard_id].state;
    g_pool.shard_route.entries[shard_id].leader_filer_idx = filer_idx;
    g_pool.shard_route.entries[shard_id].state = ROUTE_VALID;
    spin_unlock(&g_pool.shard_route.lock);

    pr_debug("powerfs: shard %llu route -> VALID (filer %d)\n",
            (unsigned long long)shard_id, filer_idx);

    /* 若从不为 VALID 的状态切换到 VALID, 派发等待中的请求到新 leader */
    if (old_state != ROUTE_VALID)
        powerfs_shard_route_dispatch_pending(shard_id);
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_update);

/* Calculate shard_id from inode using the same formula as the Filer and FUSE client.
 * Formula: (inode / 1_000_000) % shard_count
 * inode_per_shard = 1_000_000 (must match filer's ShardStrategy)
 * When shard_count <= 1, returns 0 (no sharding).
 */
u64 powerfs_calc_shard_id(u64 inode)
{
    u64 count = g_pool.shard_route.shard_count;
    if (count <= 1)
        return 0;
    return (inode / 1000000ULL) % count;
}
EXPORT_SYMBOL_GPL(powerfs_calc_shard_id);

enum powerfs_shard_route_state powerfs_shard_route_get_state(u64 shard_id)
{
    enum powerfs_shard_route_state s;

    if (shard_id >= POWERFS_MAX_SHARDS)
        return ROUTE_UNKNOWN;

    spin_lock(&g_pool.shard_route.lock);
    s = g_pool.shard_route.entries[shard_id].state;
    spin_unlock(&g_pool.shard_route.lock);
    return s;
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_get_state);

int powerfs_shard_route_find_available_filer(u64 shard_id)
{
    int leader_idx;
    int i, idx;
    int start;

    if (shard_id >= POWERFS_MAX_SHARDS)
        return -1;

    spin_lock(&g_pool.shard_route.lock);
    leader_idx = g_pool.shard_route.entries[shard_id].leader_filer_idx;
    spin_unlock(&g_pool.shard_route.lock);

    if (g_pool.filer_count <= 0)
        return -1;

    /* 从当前 leader 的下一个开始, round-robin */
    start = (leader_idx >= 0) ? (leader_idx + 1) : 0;
    for (i = 0; i < g_pool.filer_count; i++) {
        idx = (start + i) % g_pool.filer_count;
        if (g_pool.filers[idx].in_use &&
            g_pool.filers[idx].state == CONN_CONNECTED)
            return idx;
    }

    return -1;
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_find_available_filer);

struct powerfs_net_server_conn *powerfs_conn_get_filer_for_shard(u64 shard_id)
{
    int leader_idx;

    if (shard_id >= POWERFS_MAX_SHARDS)
        return NULL;

    spin_lock(&g_pool.shard_route.lock);
    leader_idx = g_pool.shard_route.entries[shard_id].leader_filer_idx;
    spin_unlock(&g_pool.shard_route.lock);

    if (leader_idx < 0 || leader_idx >= g_pool.filer_count)
        return NULL;

    return &g_pool.filers[leader_idx];
}
EXPORT_SYMBOL_GPL(powerfs_conn_get_filer_for_shard);

struct powerfs_net_server_conn *powerfs_conn_find_filer(const char *addr,
                                                        __u16 port)
{
    int i;

    if (!addr)
        return NULL;

    for (i = 0; i < g_pool.filer_count; i++) {
        if (g_pool.filers[i].in_use &&
            strcmp(g_pool.filers[i].addr, addr) == 0 &&
            g_pool.filers[i].port == port)
            return &g_pool.filers[i];
    }
    return NULL;
}
EXPORT_SYMBOL_GPL(powerfs_conn_find_filer);

/* === 3. 状态机入口 === */

void powerfs_conn_set_state(struct powerfs_net_server_conn *conn,
                            enum powerfs_conn_state new_state)
{
    enum powerfs_conn_state old_state;
    int filer_idx;

    if (!conn)
        return;

    spin_lock(&conn->state_lock);
    old_state = conn->state;
    conn->state = new_state;
    spin_unlock(&conn->state_lock);

    if (old_state != new_state)
        pr_debug("powerfs: filer %s:%u state %s -> %s\n",
                conn->addr, conn->port,
                powerfs_conn_state_str(old_state),
                powerfs_conn_state_str(new_state));

    filer_idx = powerfs_conn_get_filer_idx(conn);

    /* 事件传播 */
    if (new_state == CONN_RECONNECTING || new_state == CONN_FAULT) {
        if (filer_idx >= 0)
            powerfs_shard_route_on_filer_disconnect(filer_idx);
    } else if (new_state == CONN_CONNECTED &&
               (old_state == CONN_RECONNECTING || old_state == CONN_CONNECTING)) {
        if (filer_idx >= 0)
            powerfs_shard_route_on_filer_reconnect(filer_idx);
        /* 重连成功: 重置退避.
         * 不在此处重发请求: resend 由主线程在 submit 循环中自己重试,
         * 重连线程只负责重建连接. (主线程独占 request 生命周期, 无竞态) */
        conn->reconnect_delay = 0;
    }

    /* Filer 永久故障: 以 -ENOTCONN 取消所有待处理请求 (无法重发).
     * 在 req_lock 下逐个从 req_tree 摘除 (避免与 do_send 的 tree_remove 竞态),
     * complete 在锁外执行. */
    if (new_state == CONN_FAULT) {
        struct powerfs_request *req, *tmp;
        LIST_HEAD(cancel_list);
        bool cancelled = false;

        spin_lock(&conn->req_lock);
        list_splice_init(&conn->pending_reqs, &cancel_list);
        list_for_each_entry(req, &cancel_list, list_node) {
            powerfs_req_tree_remove(conn, req);
            req->error = -ENOTCONN;
            cancelled = true;
        }
        spin_unlock(&conn->req_lock);

        list_for_each_entry_safe(req, tmp, &cancel_list, list_node) {
            list_del_init(&req->list_node);
            pr_debug("powerfs: FAULT cancel req seq=%u msg_type=%u on filer %s:%u\n",
                     req->seq, req->msg_type, conn->addr, conn->port);
            powerfs_req_complete(req);
        }

        if (cancelled)
            pr_debug("powerfs: cancelled pending requests on filer %s:%u FAULT\n",
                    conn->addr, conn->port);
    }

    /* 唤醒等待重连完成的请求 */
    if (new_state == CONN_CONNECTED || new_state == CONN_FAULT)
        wake_up(&conn->reconnect_wq);
}
EXPORT_SYMBOL_GPL(powerfs_conn_set_state);

/* === 3.5 v2: 调度器基础设施 + sk 回调 + 调度器收发 ===
 *
 * 参照 Lustre socklnd (ksocknal_scheduler / ksocknal_data_ready) 和
 * BeeGFS StandardSocket (sock_readable / sock_write_space), 用固定 M 个
 * 调度器线程 (M = num_online_cpus()) + sk 回调驱动收发, 替换 v1 的
 * per-conn RX kthread. 回调在 softirq 只做 set flag + list_add + wake_up,
 * 重活全在调度器 process context 完成.
 *
 * 关键并发安全:
 *   - global_lock (rwlock_t) 保护 sk_user_data 解引用: 回调 read_lock_bh,
 *     set/reset_callbacks write_lock_bh (参照 Lustre socklnd_lib.c:455)
 *   - sched->rx_lock 保护 rx_conns, sched->tx_lock 保护 tx_conns: 回调投递和
 *     调度器消费都 spin_lock_bh; 两把锁独立, 收发路径互不阻塞
 *   - conn kref: 回调投递时 get, 调度器处理完 put; disconnect 等 kref==1 才
 *     sock_release (防调度器在飞时 UAF)
 */

/* conn kref 释放回调: conn 是 g_pool.filers[] 静态数组元素, 不需 kfree.
 * 正常情况下不会调用 (owner 引用常驻, refcount 不会归零); 仅作 kref_put
 * 的 release 回调占位. */
void powerfs_conn_release(struct kref *kref)
{
    struct powerfs_net_server_conn *conn =
        container_of(kref, struct powerfs_net_server_conn, kref);
    pr_warn("powerfs: conn %s:%u kref refcount hit 0 (unexpected for static conn)\n",
            conn->addr, conn->port);
}

static inline void powerfs_conn_get(struct powerfs_net_server_conn *conn)
{
    kref_get(&conn->kref);
}

/* put: 释放引用. 当 refcount 降到 1 (只剩 owner 引用) 时唤醒等待的 disconnect. */
static inline void powerfs_conn_put(struct powerfs_net_server_conn *conn)
{
    if (!kref_put(&conn->kref, powerfs_conn_release) &&
        kref_read(&conn->kref) == 1)
        wake_up(&conn->sock_user_wq);
}

/* 按 addr hash 选 filer 调度器 (元数据) */
struct powerfs_net_sched *pfs_pick_sched(const char *addr)
{
    u32 hash = 0;

    if (!g_pool.num_sched || !g_pool.schedulers)
        return NULL;
    while (addr && *addr) {
        hash = hash * 31 + (u8)(*addr);
        addr++;
    }
    return &g_pool.schedulers[hash % g_pool.num_sched];
}

/* P3.1: 按 addr hash 选 volume 调度器 (数据 I/O, 独立于 filer 调度器) */
struct powerfs_net_sched *pfs_pick_vol_sched(const char *addr,
                                                    enum powerfs_net_server_type type)
{
    u32 hash = 0;

    if (!g_pool.num_vol_sched || !g_pool.vol_schedulers)
        return NULL;
    while (addr && *addr) {
        hash = hash * 31 + (u8)(*addr);
        addr++;
    }
    /* DATA 和 META 连接使用不同调度器, 防止大帧 DATA 发送阻塞 lease 请求.
     * 将调度器池对半分: 前半给 DATA, 后半给 META.
     * num_vol_sched = num_online_cpus(), 每类至少 1 个调度器.
     * 单 CPU (num_vol_sched==1) 时 data/meta 共用 sched[0], 避免越界. */
    if (g_pool.num_vol_sched < 2)
        return &g_pool.vol_schedulers[0];
    if (type == POWERFS_NET_SERVER_VOLUME_META) {
        int half = g_pool.num_vol_sched / 2;
        return &g_pool.vol_schedulers[half + (hash % half)];
    }
    return &g_pool.vol_schedulers[hash % (g_pool.num_vol_sched / 2)];
}

/* 初始化调度器数组: 按 num_online_cpus() 分配 + 启动每 CPU 一个调度器线程
 * P3.1: 同时初始化 filer (元数据) 和 volume (数据) 两个独立调度器池 */
int powerfs_sched_init(void)
{
    int i, ret;

    /* 幂等: 已分配则不重复 (remount 场景) */
    if (g_pool.schedulers)
        return 0;

    g_pool.num_sched = num_online_cpus();
    if (g_pool.num_sched < 1)
        g_pool.num_sched = 1;

    g_pool.schedulers = kcalloc(g_pool.num_sched,
                                sizeof(struct powerfs_net_sched),
                                GFP_KERNEL);
    if (!g_pool.schedulers) {
        g_pool.num_sched = 0;
        return -ENOMEM;
    }

    rwlock_init(&g_pool.global_lock);

    for (i = 0; i < g_pool.num_sched; i++) {
        struct powerfs_net_sched *sched = &g_pool.schedulers[i];

        spin_lock_init(&sched->rx_lock);
        spin_lock_init(&sched->tx_lock);
        INIT_LIST_HEAD(&sched->rx_conns);
        INIT_LIST_HEAD(&sched->tx_conns);
        init_waitqueue_head(&sched->rx_waitq);
        init_waitqueue_head(&sched->tx_waitq);
        sched->cpt = i;

        /* Per-sched 复用收帧缓冲 (避免每帧 kvmalloc 2MB) */
        sched->rx_body_buf = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
        sched->rx_data_buf = kvmalloc(POWERFS_NET_MAX_DATA, GFP_KERNEL);
        if (!sched->rx_body_buf || !sched->rx_data_buf) {
            pr_err("powerfs: OOM rx bufs for filer sched %d\n", i);
            kfree(sched->rx_body_buf);
            kvfree(sched->rx_data_buf);
            sched->rx_body_buf = NULL;
            sched->rx_data_buf = NULL;
            while (--i >= 0) {
                kfree(g_pool.schedulers[i].rx_body_buf);
                kvfree(g_pool.schedulers[i].rx_data_buf);
                g_pool.schedulers[i].rx_body_buf = NULL;
                g_pool.schedulers[i].rx_data_buf = NULL;
            }
            kfree(g_pool.schedulers);
            g_pool.schedulers = NULL;
            g_pool.num_sched = 0;
            return -ENOMEM;
        }

        sched->rx_task = kthread_run(pfs_rx_thread_fn, sched,
                                     "pfs_rx/%d", i);
        if (IS_ERR(sched->rx_task)) {
            ret = PTR_ERR(sched->rx_task);
            pr_err("powerfs: failed to start rx sched %d: %d\n", i, ret);
            sched->rx_task = NULL;
            kfree(sched->rx_body_buf);
            kvfree(sched->rx_data_buf);
            sched->rx_body_buf = NULL;
            sched->rx_data_buf = NULL;
            atomic_set(&g_pool.stopping, 1);
            while (--i >= 0) {
                wake_up_all(&g_pool.schedulers[i].rx_waitq);
                wake_up_all(&g_pool.schedulers[i].tx_waitq);
                if (g_pool.schedulers[i].rx_task) {
                    kthread_stop(g_pool.schedulers[i].rx_task);
                    g_pool.schedulers[i].rx_task = NULL;
                }
                if (g_pool.schedulers[i].tx_task) {
                    kthread_stop(g_pool.schedulers[i].tx_task);
                    g_pool.schedulers[i].tx_task = NULL;
                }
                kfree(g_pool.schedulers[i].rx_body_buf);
                kvfree(g_pool.schedulers[i].rx_data_buf);
                g_pool.schedulers[i].rx_body_buf = NULL;
                g_pool.schedulers[i].rx_data_buf = NULL;
            }
            kfree(g_pool.schedulers);
            g_pool.schedulers = NULL;
            g_pool.num_sched = 0;
            atomic_set(&g_pool.stopping, 0);
            return ret;
        }

        sched->tx_task = kthread_run(pfs_tx_thread_fn, sched,
                                     "pfs_tx/%d", i);
        if (IS_ERR(sched->tx_task)) {
            ret = PTR_ERR(sched->tx_task);
            pr_err("powerfs: failed to start tx sched %d: %d\n", i, ret);
            sched->tx_task = NULL;
            kfree(sched->rx_body_buf);
            kvfree(sched->rx_data_buf);
            sched->rx_body_buf = NULL;
            sched->rx_data_buf = NULL;
            kthread_stop(sched->rx_task);
            sched->rx_task = NULL;
            atomic_set(&g_pool.stopping, 1);
            while (--i >= 0) {
                wake_up_all(&g_pool.schedulers[i].rx_waitq);
                wake_up_all(&g_pool.schedulers[i].tx_waitq);
                if (g_pool.schedulers[i].rx_task) {
                    kthread_stop(g_pool.schedulers[i].rx_task);
                    g_pool.schedulers[i].rx_task = NULL;
                }
                if (g_pool.schedulers[i].tx_task) {
                    kthread_stop(g_pool.schedulers[i].tx_task);
                    g_pool.schedulers[i].tx_task = NULL;
                }
                kfree(g_pool.schedulers[i].rx_body_buf);
                kvfree(g_pool.schedulers[i].rx_data_buf);
                g_pool.schedulers[i].rx_body_buf = NULL;
                g_pool.schedulers[i].rx_data_buf = NULL;
            }
            kfree(g_pool.schedulers);
            g_pool.schedulers = NULL;
            g_pool.num_sched = 0;
            atomic_set(&g_pool.stopping, 0);
            return ret;
        }
    }

    pr_debug("powerfs: started %d filer scheduler threads (num_online_cpus=%d)\n",
            g_pool.num_sched, num_online_cpus());

    /* P3.1: 初始化 volume 调度器池 (独立于 filer, 避免数据 I/O 饿死元数据) */
    g_pool.num_vol_sched = num_online_cpus();
    if (g_pool.num_vol_sched < 1)
        g_pool.num_vol_sched = 1;

    g_pool.vol_schedulers = kcalloc(g_pool.num_vol_sched,
                                     sizeof(struct powerfs_net_sched),
                                     GFP_KERNEL);
    if (!g_pool.vol_schedulers) {
        g_pool.num_vol_sched = 0;
        /* 回滚 filer 调度器 */
        powerfs_sched_exit();
        return -ENOMEM;
    }

    for (i = 0; i < g_pool.num_vol_sched; i++) {
        struct powerfs_net_sched *sched = &g_pool.vol_schedulers[i];

        spin_lock_init(&sched->rx_lock);
        spin_lock_init(&sched->tx_lock);
        INIT_LIST_HEAD(&sched->rx_conns);
        INIT_LIST_HEAD(&sched->tx_conns);
        init_waitqueue_head(&sched->rx_waitq);
        init_waitqueue_head(&sched->tx_waitq);
        sched->cpt = i;

        /* Per-sched 复用收帧缓冲 (同 filer sched) */
        sched->rx_body_buf = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
        sched->rx_data_buf = kvmalloc(POWERFS_NET_MAX_DATA, GFP_KERNEL);
        if (!sched->rx_body_buf || !sched->rx_data_buf) {
            pr_err("powerfs: OOM rx bufs for vol sched %d\n", i);
            kfree(sched->rx_body_buf);
            kvfree(sched->rx_data_buf);
            sched->rx_body_buf = NULL;
            sched->rx_data_buf = NULL;
            while (--i >= 0) {
                kfree(g_pool.vol_schedulers[i].rx_body_buf);
                kvfree(g_pool.vol_schedulers[i].rx_data_buf);
                g_pool.vol_schedulers[i].rx_body_buf = NULL;
                g_pool.vol_schedulers[i].rx_data_buf = NULL;
            }
            kfree(g_pool.vol_schedulers);
            g_pool.vol_schedulers = NULL;
            g_pool.num_vol_sched = 0;
            /* 回滚 filer 调度器 */
            powerfs_sched_exit();
            return -ENOMEM;
        }

        sched->rx_task = kthread_run(pfs_rx_thread_fn, sched,
                                     "pfs_vrx/%d", i);
        if (IS_ERR(sched->rx_task)) {
            ret = PTR_ERR(sched->rx_task);
            pr_err("powerfs: failed to start vol rx sched %d: %d\n", i, ret);
            sched->rx_task = NULL;
            kfree(sched->rx_body_buf);
            kvfree(sched->rx_data_buf);
            sched->rx_body_buf = NULL;
            sched->rx_data_buf = NULL;
            atomic_set(&g_pool.stopping, 1);
            while (--i >= 0) {
                wake_up_all(&g_pool.vol_schedulers[i].rx_waitq);
                wake_up_all(&g_pool.vol_schedulers[i].tx_waitq);
                if (g_pool.vol_schedulers[i].rx_task) {
                    kthread_stop(g_pool.vol_schedulers[i].rx_task);
                    g_pool.vol_schedulers[i].rx_task = NULL;
                }
                if (g_pool.vol_schedulers[i].tx_task) {
                    kthread_stop(g_pool.vol_schedulers[i].tx_task);
                    g_pool.vol_schedulers[i].tx_task = NULL;
                }
                kfree(g_pool.vol_schedulers[i].rx_body_buf);
                kvfree(g_pool.vol_schedulers[i].rx_data_buf);
                g_pool.vol_schedulers[i].rx_body_buf = NULL;
                g_pool.vol_schedulers[i].rx_data_buf = NULL;
            }
            kfree(g_pool.vol_schedulers);
            g_pool.vol_schedulers = NULL;
            g_pool.num_vol_sched = 0;
            atomic_set(&g_pool.stopping, 0);
            /* 回滚 filer 调度器 */
            powerfs_sched_exit();
            return ret;
        }

        sched->tx_task = kthread_run(pfs_tx_thread_fn, sched,
                                     "pfs_vtx/%d", i);
        if (IS_ERR(sched->tx_task)) {
            ret = PTR_ERR(sched->tx_task);
            pr_err("powerfs: failed to start vol tx sched %d: %d\n", i, ret);
            sched->tx_task = NULL;
            kfree(sched->rx_body_buf);
            kvfree(sched->rx_data_buf);
            sched->rx_body_buf = NULL;
            sched->rx_data_buf = NULL;
            kthread_stop(sched->rx_task);
            sched->rx_task = NULL;
            atomic_set(&g_pool.stopping, 1);
            while (--i >= 0) {
                wake_up_all(&g_pool.vol_schedulers[i].rx_waitq);
                wake_up_all(&g_pool.vol_schedulers[i].tx_waitq);
                if (g_pool.vol_schedulers[i].rx_task) {
                    kthread_stop(g_pool.vol_schedulers[i].rx_task);
                    g_pool.vol_schedulers[i].rx_task = NULL;
                }
                if (g_pool.vol_schedulers[i].tx_task) {
                    kthread_stop(g_pool.vol_schedulers[i].tx_task);
                    g_pool.vol_schedulers[i].tx_task = NULL;
                }
                kfree(g_pool.vol_schedulers[i].rx_body_buf);
                kvfree(g_pool.vol_schedulers[i].rx_data_buf);
                g_pool.vol_schedulers[i].rx_body_buf = NULL;
                g_pool.vol_schedulers[i].rx_data_buf = NULL;
            }
            kfree(g_pool.vol_schedulers);
            g_pool.vol_schedulers = NULL;
            g_pool.num_vol_sched = 0;
            atomic_set(&g_pool.stopping, 0);
            /* 回滚 filer 调度器 */
            powerfs_sched_exit();
            return ret;
        }
    }

    pr_debug("powerfs: started %d volume scheduler threads\n",
            g_pool.num_vol_sched);
    return 0;
}

/* 停止所有调度器线程 + 释放数组 (幂等, 重复调用安全)
 * P3.1: 同时清理 filer 和 volume 两个调度器池 */
void powerfs_sched_exit(void)
{
    int i;

    /* P3.1: 先清理 volume 调度器池 */
    if (g_pool.vol_schedulers) {
        atomic_set(&g_pool.stopping, 1);
        for (i = 0; i < g_pool.num_vol_sched; i++) {
            wake_up_all(&g_pool.vol_schedulers[i].rx_waitq);
            wake_up_all(&g_pool.vol_schedulers[i].tx_waitq);
        }

        for (i = 0; i < g_pool.num_vol_sched; i++) {
            if (g_pool.vol_schedulers[i].rx_task) {
                kthread_stop(g_pool.vol_schedulers[i].rx_task);
                g_pool.vol_schedulers[i].rx_task = NULL;
            }
            if (g_pool.vol_schedulers[i].tx_task) {
                kthread_stop(g_pool.vol_schedulers[i].tx_task);
                g_pool.vol_schedulers[i].tx_task = NULL;
            }
            kfree(g_pool.vol_schedulers[i].rx_body_buf);
            kvfree(g_pool.vol_schedulers[i].rx_data_buf);
            g_pool.vol_schedulers[i].rx_body_buf = NULL;
            g_pool.vol_schedulers[i].rx_data_buf = NULL;
        }

        kfree(g_pool.vol_schedulers);
        g_pool.vol_schedulers = NULL;
        g_pool.num_vol_sched = 0;
        pr_debug("powerfs: volume scheduler threads stopped\n");
    }

    if (!g_pool.schedulers)
        return;

    /* 确保调度器线程看到 stopping 并退出 (kthread_stop 也会唤醒) */
    atomic_set(&g_pool.stopping, 1);
    for (i = 0; i < g_pool.num_sched; i++) {
        wake_up_all(&g_pool.schedulers[i].rx_waitq);
        wake_up_all(&g_pool.schedulers[i].tx_waitq);
    }

    for (i = 0; i < g_pool.num_sched; i++) {
        if (g_pool.schedulers[i].rx_task) {
            kthread_stop(g_pool.schedulers[i].rx_task);
            g_pool.schedulers[i].rx_task = NULL;
        }
        if (g_pool.schedulers[i].tx_task) {
            kthread_stop(g_pool.schedulers[i].tx_task);
            g_pool.schedulers[i].tx_task = NULL;
        }
        kfree(g_pool.schedulers[i].rx_body_buf);
        kvfree(g_pool.schedulers[i].rx_data_buf);
        g_pool.schedulers[i].rx_body_buf = NULL;
        g_pool.schedulers[i].rx_data_buf = NULL;
    }

    kfree(g_pool.schedulers);
    g_pool.schedulers = NULL;
    g_pool.num_sched = 0;
    pr_debug("powerfs: filer scheduler threads stopped\n");
}

/* === sk 回调 (softirq 上下文, 仅标记+投递+wake_up, 参照 Lustre socklnd_lib.c:448) === */

/* 数据到达: softirq 上下文 */
void pfs_data_ready(struct sock *sk)
{
    struct powerfs_net_server_conn *conn;

    read_lock_bh(&g_pool.global_lock);     /* 与 reset_callback 的 write_lock 互斥 */
    conn = sk->sk_user_data;
    if (conn)
        pfs_rx_callback(conn);             /* → rx_ready=1 + list_add + wake_up */
    else
        sk->sk_data_ready(sk);             /* NULL: reset 已恢复原始回调, 调原始 (不递归) */
    read_unlock_bh(&g_pool.global_lock);
}

/* 可写空间: softirq 上下文 */
void pfs_write_space(struct sock *sk)
{
    struct powerfs_net_server_conn *conn;

    read_lock_bh(&g_pool.global_lock);
    conn = sk->sk_user_data;
    if (conn)
        pfs_tx_callback(conn);             /* → tx_ready=1 + list_add + wake_up */
    else
        sk->sk_write_space(sk);            /* NULL: 调原始回调 */
    read_unlock_bh(&g_pool.global_lock);
}

/* 状态变化: TCP_CLOSE_WAIT (peer FIN) / TCP_CLOSE (RST) → 即时断连感知 */
void pfs_state_change(struct sock *sk)
{
    struct powerfs_net_server_conn *conn;

    read_lock_bh(&g_pool.global_lock);
    conn = sk->sk_user_data;
    if (conn) {
        pr_debug("powerfs: state_change %s:%u sk_state=%d sk_err=%d\n",
                conn->addr, conn->port, sk->sk_state, sk->sk_err);
        if (sk->sk_state == TCP_CLOSE_WAIT || sk->sk_state == TCP_CLOSE)
            queue_work(g_pool.reconn_wq, &conn->disconnect_work);   /* process context 清理 */
    } else {
        sk->sk_state_change(sk);           /* NULL: 调原始回调 */
    }
    read_unlock_bh(&g_pool.global_lock);
}

/* 错误: keepalive 失败/ICMP 不可达等 → 断连 */
void pfs_error_report(struct sock *sk)
{
    struct powerfs_net_server_conn *conn;

    read_lock_bh(&g_pool.global_lock);
    conn = sk->sk_user_data;
    if (conn) {
        pr_debug("powerfs: error_report %s:%u sk_err=%d\n",
                conn->addr, conn->port, sk->sk_err);
        queue_work(g_pool.reconn_wq, &conn->disconnect_work);
    } else {
        sk->sk_error_report(sk);           /* NULL: 调原始回调 */
    }
    read_unlock_bh(&g_pool.global_lock);
}

/* RX 回调: 标记 rx_ready + 投递到 sched->rx_conns + 唤醒调度器
 *
 * 注意: 调用上下文有两类:
 *   (a) TCP sock sk_data_ready/sk_state_change 回调: 进程或 bh enabled softirq;
 *   (b) RDMA CQ comp_handler: IB_POLL_SOFTIRQ 上下文, 此时 BH 已 disabled,
 *       若再用 spin_lock_bh/unlock_bh 会触发 softirq.c:__local_bh_enable_ip 的
 *       WARNING (不平衡的 bh disable/enable 计数).
 * 因此统一用 irqsave 版本: 无论进程/softirq/hardirq 上下文均安全, 锁持有时间
 * 极短 (list_add_tail + wake_up), 额外关中断性能影响可忽略. */
void pfs_rx_callback(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
    unsigned long flags;
    if (!sched)
        return;

    spin_lock_irqsave(&sched->rx_lock, flags);
    conn->rx_ready = 1;
    if (!conn->rx_scheduled) {
        list_add_tail(&conn->rx_list, &sched->rx_conns);
        conn->rx_scheduled = 1;
        powerfs_conn_get(conn);            /* 调度器持引用 (防收发中拆除) */
        wake_up(&sched->rx_waitq);
    }
    spin_unlock_irqrestore(&sched->rx_lock, flags);
}

/* TX 回调: 标记 tx_ready + 投递到 sched->tx_conns + 唤醒 TX 线程
 * 同上: 统一 irqsave 版本以适配 RDMA softirq 上下文. */
void pfs_tx_callback(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
    unsigned long flags;
    if (!sched)
        return;

    spin_lock_irqsave(&sched->tx_lock, flags);
    conn->tx_ready = 1;
    if (!conn->tx_scheduled) {
        list_add_tail(&conn->tx_list, &sched->tx_conns);
        conn->tx_scheduled = 1;
        powerfs_conn_get(conn);
        wake_up(&sched->tx_waitq);
    }
    spin_unlock_irqrestore(&sched->tx_lock, flags);
}

/* 建连: 保存原始回调 + 安装自定义回调 (参照 Lustre ksocknal_lib_save_callback +
 * ksocknal_lib_set_callback, socklnd_lib.c:511-524) */
void pfs_conn_set_callbacks(struct powerfs_net_server_conn *conn)
{
    struct sock *sk = conn->sock->sk;

    write_lock_bh(&g_pool.global_lock);
    conn->saved_data_ready   = sk->sk_data_ready;
    conn->saved_write_space  = sk->sk_write_space;
    conn->saved_state_change = sk->sk_state_change;
    conn->saved_error_report = sk->sk_error_report;
    sk->sk_user_data         = conn;
    sk->sk_data_ready        = pfs_data_ready;
    sk->sk_write_space       = pfs_write_space;
    sk->sk_state_change      = pfs_state_change;
    sk->sk_error_report      = pfs_error_report;
    write_unlock_bh(&g_pool.global_lock);
}

/* 拆除: 恢复原始回调 + 清 sk_user_data (此后回调 NOOP, 参照 Lustre
 * ksocknal_lib_reset_callback, socklnd_lib.c:526-541).
 * write_unlock_bh 关软中断, 保证此 CPU 上回调不会在释放锁后重入;
 * 已进入回调(持 read_lock)的会安全完成. */
void pfs_conn_reset_callbacks(struct powerfs_net_server_conn *conn)
{
    struct sock *sk = conn->sock->sk;

    write_lock_bh(&g_pool.global_lock);
    sk->sk_data_ready   = conn->saved_data_ready;
    sk->sk_write_space  = conn->saved_write_space;
    sk->sk_state_change = conn->saved_state_change;
    sk->sk_error_report = conn->saved_error_report;
    sk->sk_user_data    = NULL;
    write_unlock_bh(&g_pool.global_lock);
}

/* === 调度器线程 (process context, per-CPU, 参照 Lustre ksocknal_scheduler
 * socklnd_cb.c:1347-1508) === */

/* RX 线程: 只处理 rx_conns (收响应), 不受 TX 阻塞影响.
 * TX 线程在 kernel_sendmsg 阻塞时, RX 线程仍可收响应 → 避免 10s 超时.
 *
 * v2: wait_event_interruptible_timeout(500ms) 防回调 race
 *   (sk_data_ready 在 rx_scheduled=1 期间触发, 可能错过 wake_up).
 *   超时后回到循环顶重新检查 rx_conns, 不依赖唤醒. */
int pfs_rx_thread_fn(void *arg)
{
    struct powerfs_net_sched *sched = arg;
    struct powerfs_net_server_conn *conn;

    spin_lock_bh(&sched->rx_lock);

    while (!kthread_should_stop()) {
        /* stopping 时不处理新工作, 但不退出 — 必须等 kthread_stop()
         * 设置 KTHREAD_SHOULD_STOP 后才退出. 否则线程提前退出 →
         * task_struct 被内核回收 → kthread_stop() 访问已释放内存
         * → UAF → SLUB freelist 损坏 → dentry 哈希链环 → RCU stall. */
        if (atomic_read(&g_pool.stopping)) {
            spin_unlock_bh(&sched->rx_lock);
            wait_event_interruptible(sched->rx_waitq,
                                     kthread_should_stop());
            spin_lock_bh(&sched->rx_lock);
            continue;
        }

        conn = list_first_entry_or_null(&sched->rx_conns,
                                        struct powerfs_net_server_conn,
                                        rx_list);
        if (conn) {
            list_del_init(&conn->rx_list);
            spin_unlock_bh(&sched->rx_lock);

            pfs_process_receive(conn);

            /* 重新检查 rx_ready 必须在 rx_lock 下做, 与 sk_data_ready
             * 回调 (pfs_rx_callback) 互斥. 否则无锁清 rx_ready=0 会
             * clobber 回调刚设的 rx_ready=1, 导致有数据但 conn 不再
             * 被投递 → 响应 stall 直到下次数据到达. */
            spin_lock_bh(&sched->rx_lock);
            conn->rx_ready = 0;
            if (conn->sock && conn->sock->sk &&
                !skb_queue_empty(&conn->sock->sk->sk_receive_queue))
                conn->rx_ready = 1;
            if (conn->rx_ready)
                list_add_tail(&conn->rx_list, &sched->rx_conns);
            else {
                conn->rx_scheduled = 0;
                powerfs_conn_put(conn);
            }
        } else {
            spin_unlock_bh(&sched->rx_lock);
            /* 500ms timeout: 即使没收到 wake_up (回调 race), 也定期
             * 醒来检查 rx_conns, 避免遗漏的 conn 永远等不到处理. */
            wait_event_interruptible_timeout(sched->rx_waitq,
                !list_empty(&sched->rx_conns) ||
                kthread_should_stop(),
                msecs_to_jiffies(500));
            spin_lock_bh(&sched->rx_lock);
        }

        if (need_resched()) {
            spin_unlock_bh(&sched->rx_lock);
            cond_resched();
            spin_lock_bh(&sched->rx_lock);
        }
    }

    spin_unlock_bh(&sched->rx_lock);
    return 0;
}

/* TX 线程: 只处理 tx_conns (发请求), 可在 kernel_sendmsg 阻塞
 * (sk_sndtimeo=10s) 而不影响 RX. */
int pfs_tx_thread_fn(void *arg)
{
    struct powerfs_net_sched *sched = arg;
    struct powerfs_net_server_conn *conn;

    spin_lock_bh(&sched->tx_lock);

    while (!kthread_should_stop()) {
        /* 同 pfs_rx_thread_fn: stopping 不退出, 等 kthread_stop(). */
        if (atomic_read(&g_pool.stopping)) {
            spin_unlock_bh(&sched->tx_lock);
            wait_event_interruptible(sched->tx_waitq,
                                     kthread_should_stop());
            spin_lock_bh(&sched->tx_lock);
            continue;
        }

        conn = list_first_entry_or_null(&sched->tx_conns,
                                        struct powerfs_net_server_conn,
                                        tx_list);
        if (conn) {
            list_del_init(&conn->tx_list);
            conn->tx_ready = 0;
            spin_unlock_bh(&sched->tx_lock);

            pfs_process_transmit(conn);

            spin_lock_bh(&sched->tx_lock);
            if (conn->tx_ready)
                list_add_tail(&conn->tx_list, &sched->tx_conns);
            else {
                conn->tx_scheduled = 0;
                powerfs_conn_put(conn);
            }
        } else {
            spin_unlock_bh(&sched->tx_lock);
            wait_event_interruptible(sched->tx_waitq,
                !list_empty(&sched->tx_conns) ||
                kthread_should_stop());
            spin_lock_bh(&sched->tx_lock);
        }

        if (need_resched()) {
            spin_unlock_bh(&sched->tx_lock);
            cond_resched();
            spin_lock_bh(&sched->tx_lock);
        }
    }

    spin_unlock_bh(&sched->tx_lock);
    return 0;
}

/* === v2 RX 非阻塞状态机 (对齐 TX send_offset 设计) ===
 *
 * 问题: 旧版 pfs_process_receive 用 powerfs_net_frame_recv (MSG_WAITALL +
 *       sk_rcvtimeo=10s) 收一帧. 大响应 (1MB needle) 分多 TCP 段到达时,
 *       MSG_WAITALL 阻塞等剩余字节最长 10s, 期间同一 sched 上其他 conn
 *       的响应全部排队等待 (1MB 文件 10s 延迟根因).
 * 方案: per-conn 状态机, MSG_DONTWAIT + 断点续收:
 *   - 每帧分 3 段: hdr(28B) -> body -> data
 *   - 每段记录已收字节数 (rx_hdr_got/rx_body_got/rx_data_got)
 *   - EAGAIN 时保留 partial 状态退出, 等 sk_data_ready 下次回调续收
 *   - 一帧完整后 dispatch (lookup req + complete) + reset, 继续收下一帧
 *   - 内循环直到 skb_queue 空 (单 conn 最多 64 帧防饿死其他 conn)
 * buffer 必须在 conn 上 (sched 共享 buffer 切 conn 时 partial 会丢失). */

/* 重置 partial 状态 (新帧开始前 / disconnect / 错误恢复) */
void pfs_rx_reset_partial(struct powerfs_net_server_conn *conn)
{
    conn->rx_hdr_got = 0;
    conn->rx_body_got = 0;
    conn->rx_data_got = 0;
    conn->rx_body_total = 0;
    conn->rx_data_total = 0;
    conn->rx_phase = 0;
}

/* 分配 per-conn RX buffer (filer/volume conn init 调用) */
int pfs_conn_alloc_rxbuffers(struct powerfs_net_server_conn *conn)
{
    conn->rx_body_buf = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    if (!conn->rx_body_buf)
        return -ENOMEM;
    conn->rx_data_buf = kvmalloc(POWERFS_NET_MAX_DATA, GFP_KERNEL);
    if (!conn->rx_data_buf) {
        kfree(conn->rx_body_buf);
        conn->rx_body_buf = NULL;
        return -ENOMEM;
    }
    pfs_rx_reset_partial(conn);
    return 0;
}

/* 释放 per-conn RX buffer (pool_exit / conn 退役调用) */
void pfs_conn_free_rxbuffers(struct powerfs_net_server_conn *conn)
{
    kfree(conn->rx_body_buf);
    kvfree(conn->rx_data_buf);
    conn->rx_body_buf = NULL;
    conn->rx_data_buf = NULL;
    pfs_rx_reset_partial(conn);
}

/* 非阻塞推进一节 (收 hdr / body / data 的剩余字节)
 * 返回:
 *   0       - 一帧完整 (rx_phase=3), 调用者应 dispatch + reset
 *   -EAGAIN - 当前没数据, 保留 partial 状态退出
 *   <0      - EOF/RST/错误, 触发断连 (调用者 queue disconnect_work) */
int pfs_rx_step(struct powerfs_net_server_conn *conn)
{
    struct socket *sock = conn->sock;
    struct kvec vec;
    struct msghdr msg = {};
    ssize_t got;
    size_t want;

    if (!sock)
        return -ENOTCONN;

    /* 防御: per-conn buffer 必须已分配 (conn init 时分配).
     * 缺失说明 conn 创建路径漏了 pfs_conn_alloc_rxbuffers, 直接断连避免 oops. */
    if (WARN_ON_ONCE(!conn->rx_body_buf || !conn->rx_data_buf))
        return -ENOMEM;

    /* 阶段 0: 收帧头 (28 字节) */
    if (conn->rx_phase == 0) {
        want = POWERFS_NET_FRAME_HDR_SIZE - conn->rx_hdr_got;
        vec.iov_base = conn->rx_hdr_buf + conn->rx_hdr_got;
        vec.iov_len = want;
        got = kernel_recvmsg(sock, &msg, &vec, 1, want, MSG_DONTWAIT);
        if (got == 0)
            return -ECONNRESET;  /* EOF: 对端关闭 */
        if (got < 0) {
            if (got == -EAGAIN || got == -EWOULDBLOCK)
                return -EAGAIN;
            return got;
        }
        conn->rx_hdr_got += got;
        if (conn->rx_hdr_got < POWERFS_NET_FRAME_HDR_SIZE)
            return -EAGAIN;  /* 帧头未收完, 等下次回调 */

        /* 帧头完整, 解码 */
        if (!powerfs_net_frame_hdr_decode(conn->rx_hdr_buf,
                                          POWERFS_NET_FRAME_HDR_SIZE,
                                          &conn->rx_cur_hdr)) {
            pr_err("powerfs: RX %s:%u invalid frame header\n",
                   conn->addr, conn->port);
            return -EINVAL;
        }
        /* K1 (Layer 1): 帧头不变式严格校验.
         *
         * data_len 是 body + data 总长度, body_len 是 body 段长度,
         * data 段 = data_len - body_len. 因此 data_len >= body_len 是
         * 协议不变式. 违反说明帧头损坏或对端有 bug, 必须拒绝整帧.
         *
         * 旧代码静默钳制 body_sz = total, 掩盖了协议错误, 导致后续
         * TLV 解析消费错误偏移的数据, 产生难以定位的级联故障.
         */
        if (conn->rx_cur_hdr.data_len < conn->rx_cur_hdr.body_len) {
            pr_err("powerfs: RX_HDR_INVARIANT seq=%u msg=0x%04x data_len=%u < body_len=%u peer=%s:%u\n",
                   conn->rx_cur_hdr.seq, conn->rx_cur_hdr.msg_type,
                   conn->rx_cur_hdr.data_len, conn->rx_cur_hdr.body_len,
                   conn->addr, conn->port);
            return -EPROTO;
        }
        if (conn->rx_cur_hdr.data_len > POWERFS_NET_MAX_FRAME) {
            pr_err("powerfs: RX_HDR_INVARIANT seq=%u msg=0x%04x data_len=%u > MAX_FRAME=%u peer=%s:%u\n",
                   conn->rx_cur_hdr.seq, conn->rx_cur_hdr.msg_type,
                   conn->rx_cur_hdr.data_len, POWERFS_NET_MAX_FRAME,
                   conn->addr, conn->port);
            return -EPROTO;
        }
        /* 解析 body/data 长度 (data_len=总长, body_len=body 段长) */
        {
            size_t total = conn->rx_cur_hdr.data_len;
            size_t body_sz = conn->rx_cur_hdr.body_len;
            /* K1 已保证 data_len >= body_len, 无需再钳制 */
            conn->rx_body_total = body_sz;
            conn->rx_data_total = total - body_sz;
            if (conn->rx_body_total > POWERFS_NET_MAX_BODY ||
                conn->rx_data_total > POWERFS_NET_MAX_DATA) {
                pr_err("powerfs: RX %s:%u frame too large body=%zu data=%zu\n",
                       conn->addr, conn->port,
                       conn->rx_body_total, conn->rx_data_total);
                return -E2BIG;
            }
        }
        conn->rx_phase = 1;
        /* fall through: 继续收 body (非阻塞, 立即试) */
    }

    /* 阶段 1: 收 body 段 */
    if (conn->rx_phase == 1) {
        if (conn->rx_body_total > 0) {
            want = conn->rx_body_total - conn->rx_body_got;
            vec.iov_base = conn->rx_body_buf + conn->rx_body_got;
            vec.iov_len = want;
            got = kernel_recvmsg(sock, &msg, &vec, 1, want, MSG_DONTWAIT);
            if (got == 0)
                return -ECONNRESET;
            if (got < 0) {
                if (got == -EAGAIN || got == -EWOULDBLOCK)
                    return -EAGAIN;
                return got;
            }
            conn->rx_body_got += got;
            if (conn->rx_body_got < conn->rx_body_total)
                return -EAGAIN;
        }
        conn->rx_phase = 2;
        /* fall through: 继续收 data */
    }

    /* 阶段 2: 收 data 段 */
    if (conn->rx_phase == 2) {
        if (conn->rx_data_total > 0) {
            want = conn->rx_data_total - conn->rx_data_got;
            vec.iov_base = conn->rx_data_buf + conn->rx_data_got;
            vec.iov_len = want;
            got = kernel_recvmsg(sock, &msg, &vec, 1, want, MSG_DONTWAIT);
            if (got == 0)
                return -ECONNRESET;
            if (got < 0) {
                if (got == -EAGAIN || got == -EWOULDBLOCK)
                    return -EAGAIN;
                return got;
            }
            conn->rx_data_got += got;
            if (conn->rx_data_got < conn->rx_data_total)
                return -EAGAIN;
        }
        conn->rx_phase = 3;
        return 0;  /* 一帧完整 */
    }

    return 0;  /* 已是 phase=3 */
}

/* Phase 2: 计算 conn 在 flow controller 中的索引.
 * filer conns: [0, MAX_FILERS)
 * volume conns: [MAX_FILERS, MAX_FILERS + MAX_VOLUMES)
 * -1=无法映射 (跳过流控) */
int pfs_conn_flow_idx(struct powerfs_net_server_conn *conn)
{
    /* g_pool 是 static, 指针算术计算索引 */
    if (conn >= &g_pool.filers[0] &&
        conn < &g_pool.filers[POWERFS_NET_MAX_FILERS]) {
        return (int)(conn - &g_pool.filers[0]);
    }
    if (conn >= &g_pool.volumes[0] &&
        conn < &g_pool.volumes[POWERFS_NET_MAX_VOLUMES]) {
        return POWERFS_NET_MAX_FILERS +
               (int)(conn - &g_pool.volumes[0]);
    }
    return -1;
}

/* --- §13 Cap NOTIFY forward declarations ---
 * 定义位于文件末尾 §13 (cap dispatcher + decode_cap_*_body).
 * pfs_rx_dispatch 需要提前引用 decode 函数 + g_cap_*_notify_fn 变量. */
/* cap NOTIFY 回调 (fs 层通过 powerfs_net_reg_cap_notify_handlers 注册).
 * __read_mostly: 注册一次后只读, 减少 cache line ping-pong. */
powerfs_cap_recall_notify_fn  g_cap_recall_notify_fn  __read_mostly;
powerfs_cap_upgrade_notify_fn g_cap_upgrade_notify_fn __read_mostly;

/* --- §13b TopologyChanged NOTIFY (Master → Client) ---
 *
 * When the Master's `shard_leaders` table changes (after receiving a
 * ShardLeaderUpdate from a filer whose shard leadership actually flipped),
 * it broadcasts MsgType::TopologyChanged (0x0072, NOTIFY flag, empty body)
 * to all connected TLV clients. On receipt we queue an asynchronous worker
 * on `g_pool.reconn_wq` (WQ_UNBOUND, safe for blocking socket I/O) that
 * re-issues GET_TOPOLOGY → parses ShardLeaderEntries → pre-fills every
 * shard's route to the new leader VALID. This keeps the zero-redirect fast
 * path live across elections instead of waiting for a follower redirect on
 * the next RPC.
 *
 * Debounce: `g_topology_refresh_pending` (test_and_set_bit) guarantees at
 * most one in-flight refresh, since the Master may broadcast
 * TopologyChanged multiple times in quick succession when many shards
 * fail over together (e.g. filer restart → 256 shards elect new leaders
 * in a burst, triggering 256× ShardLeaderUpdate → 256× broadcasts). */
DECLARE_WORK(g_topology_refresh_work, topology_refresh_worker);
unsigned long g_topology_refresh_flags;  /* 0 = pending bit */

/* 一帧完整后处理: NOTIFY 异步帧 或 按 seq 匹配 pending req + complete.
 * 锁外 memcpy 大块 READ 响应 (持 req_lock 会阻塞 do_send 入队). */
void pfs_rx_dispatch(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_frame_hdr *hdr = &conn->rx_cur_hdr;
    void *body = conn->rx_body_buf;
    void *data = conn->rx_data_buf;
    size_t body_len = conn->rx_body_got;
    size_t data_len = conn->rx_data_got;
    struct powerfs_request *req = NULL;

    /* 异步通知帧 (seq=0 或 NOTIFY flag): invalidate 主动推送 / Cap recall / Cap upgrade.
     * Filer 元数据变更后推 Invalidate(inode, version) 到所有订阅客户端;
     * Cap manager 推 CapRecallNotify(撤销) / CapUpgradeNotify(升级) 到持有 cap 的客户端. */
    if ((hdr->flags & POWERFS_NET_FLAG_NOTIFY) || hdr->seq == 0) {
        pr_debug("powerfs: RX %s:%u: async notify msg=0x%04x seq=%u flags=0x%02x body_len=%zu\n",
                 conn->addr, conn->port, hdr->msg_type, hdr->seq, hdr->flags, body_len);

        /* --- Cap recall notify (0x0094): 服务端撤销本客户端 cap 位 --- */
        if (hdr->msg_type == POWERFS_NET_MSG_CAP_RECALL_NOTIFY) {
            powerfs_cap_recall_notify_fn fn;
            u64 ino = 0;
            char token_buf[64];
            size_t token_len = 0;
            __u8 recall_mask = 0, retain_mask = 0;
            __u64 epoch = 0;
            int rc;

            rc = decode_cap_recall_body(body, body_len, &ino,
                                        token_buf, sizeof(token_buf), &token_len,
                                        &recall_mask, &retain_mask, &epoch);
            if (rc < 0) {
                pr_warn("powerfs: CapRecallNotify decode failed rc=%d\n", rc);
                return;
            }
            fn = smp_load_acquire(&g_cap_recall_notify_fn);
            if (fn) {
                fn(ino, token_buf, token_len, recall_mask, retain_mask, epoch);
            } else {
                pr_warn("powerfs: CapRecallNotify for ino=%llu but no handler registered\n",
                        ino);
            }
            return;
        }

        /* --- Cap upgrade notify (0x0095): 存活 writer 被升级回 EXCLUSIVE_WRITE --- */
        if (hdr->msg_type == POWERFS_NET_MSG_CAP_UPGRADE_NOTIFY) {
            powerfs_cap_upgrade_notify_fn fn;
            u64 ino = 0;
            char token_buf[64];
            size_t token_len = 0;
            __u8 new_granted = 0;
            __u64 epoch = 0, sn = 0;
            int rc;

            rc = decode_cap_upgrade_body(body, body_len, &ino,
                                         token_buf, sizeof(token_buf), &token_len,
                                         &new_granted, &epoch, &sn);
            if (rc < 0) {
                pr_warn("powerfs: CapUpgradeNotify decode failed rc=%d\n", rc);
                return;
            }
            fn = smp_load_acquire(&g_cap_upgrade_notify_fn);
            if (fn) {
                fn(ino, token_buf, token_len, new_granted, epoch, sn);
            } else {
                pr_warn("powerfs: CapUpgradeNotify for ino=%llu but no handler registered\n",
                        ino);
            }
            return;
        }

        /* --- TopologyChanged notify (0x0072): Master shard leader table
         * was updated → re-fetch topology and refresh shard_route VALID.
         *
         * This RX-thread callback is NON-BLOCKING: all blocking socket I/O
         * (connect master / send GET_TOPOLOGY / parse response) is queued
         * onto g_pool.reconn_wq — an UNBOUND workqueue specifically for
         * operations that may block on kernel_connect() / recv(). A
         * test_and_set debounce flag collapses the common "many shards
         * failed over together → N broadcasts" burst into a single
         * refresh. */
        if (hdr->msg_type == POWERFS_NET_MSG_TOPOLOGY_CHANGED) {
            if (powerfs_net_is_stopping()) {
                pr_debug("powerfs: TopologyChanged dropped (stopping=1)\n");
                return;
            }
            if (!test_and_set_bit(TOPOLOGY_REFRESH_PENDING_BIT,
                                   &g_topology_refresh_flags)) {
                bool queued = false;
                if (g_pool.reconn_wq) {
                    queued = queue_work(g_pool.reconn_wq,
                                         &g_topology_refresh_work);
                }
                pr_info("powerfs: TopologyChanged NOTIFY received (from %s:%u)%s — debounce-pending=%d queue_work=%d\n",
                        conn->addr, conn->port,
                        (hdr->flags & POWERFS_NET_FLAG_NOTIFY) ? "" : " (seq=0 legacy)",
                        1, queued ? 1 : 0);
                if (!queued && g_pool.reconn_wq == NULL) {
                    /* race: reconn_wq not yet allocated. Pending bit
                     * stays set → dropped; next broadcast retries.
                     * (This window only exists during mount before
                     * discover_volumes, which runs before any RPC.) */
                    clear_bit(TOPOLOGY_REFRESH_PENDING_BIT,
                              &g_topology_refresh_flags);
                    pr_warn("powerfs: TopologyChanged dropped (reconn_wq NULL); will retry next broadcast\n");
                }
            } else {
                pr_debug("powerfs: TopologyChanged debounced — refresh already pending\n");
            }
            return;
        }

        /* --- 通用 Invalidate notify: 元数据变更推 Invalidate ---
         *
         * 支持两种载荷 (兼容旧的 inode-only, 对齐 FUSE 侧新增 dentry-level):
         *   (A) Ino + Version (旧协议, 向后兼容):
         *        → powerfs_invalidate_one(ino) 做 inode-level refresh.
         *   (B) ParentIno + Name + Ino(可选) + Version (新协议, dentry-level):
         *        → powerfs_invalidate_dentry(parent_ino, name, version)
         *          同时清理 VFS dcache 子 dentry (d_drop/d_invalidate)
         *          和父目录 dir_entries 中的 stale active entry,
         *          解决 ar rcs rename-over-replace 遗留 8 字节 stale inode 的 bug.
         *
         * Filer 在 net_handler.rs 中编码 notify 时会同时设置 (ParentIno, Name,
         * Ino, Version) 四个字段; 旧版 Filer 只推 Ino+Version, 这里分支兼容.
         */
        {
            __u64 ino = 0;
            __u64 version = 0;
            __u64 parent_ino = 0;
            char name_buf[POWERFS_MAX_NAME_LEN + 1];
            int name_rc = -ENOENT;

            if (body && body_len > 0) {
                struct powerfs_tlv_dec dec;
                powerfs_tlv_dec_init(&dec, body, body_len);
                /* 顺序不敏感: find 遍历. */
                powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_INO, &ino);
                powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_VERSION, &version);
                powerfs_tlv_dec_find_u64(&dec, POWERFS_NET_FLD_PARENT_INO, &parent_ino);
                /* name 解码: 可能缺省 (旧 inode-only notify, 或 Filer
                 * 广播 inode-level-only event 如 truncate).
                 * powerfs_tlv_dec_find_raw 已 NULL-safe, 单次调用即可
                 * 同时探测存在性并取值, 无需二次扫描. */
                name_buf[0] = '\0';
                {
                    const __u8 *name_ptr = NULL;
                    size_t name_len = 0;
                    name_rc = powerfs_tlv_dec_find_raw(&dec,
                                POWERFS_NET_FLD_NAME, &name_ptr, &name_len);
                    if (name_rc >= 0 && name_ptr && name_len > 0) {
                        size_t copy = name_len;
                        if (copy > POWERFS_MAX_NAME_LEN)
                            copy = POWERFS_MAX_NAME_LEN;
                        memcpy(name_buf, name_ptr, copy);
                        name_buf[copy] = '\0';
                        name_rc = (int)copy;
                    } else {
                        name_rc = -ENOENT;
                    }
                }
            }

            /* ── 派发优先级:
             *   1. ParentIno + Name 存在 → 先 DENTRY-level (MC-401 对齐 FUSE),
             *      这一步已经 remove_dir_entry + d_invalidate/d_drop.
             *   2. Ino != 0 → 再走 inode-level getattr+pagecache inval.
             *      (即使做了 dentry-level, inode-level 仍需:
             *      比如被 rename-over-replace 的目标 inode 仍可能在别处
             *      被打开, pagecache 需要被 inval;
             *      另外 rename 源文件的 source inode 也需要 inode-level
             *      pagecache inval.) */
            if (parent_ino != 0 && name_rc > 0) {
                pr_debug("powerfs: dentry-level inval parent=%llu name=%s ino=%llu ver=%llu\n",
                        parent_ino, name_buf, ino, version);
                powerfs_invalidate_dentry(parent_ino, name_buf,
                                          (size_t)name_rc, version);
            }

            if (ino != 0) {
                pr_debug("powerfs: inode-level inval ino=%llu ver=%llu\n",
                        ino, version);
                /* powerfs_invalidate_one() now defers all work (inode lookup +
                 * getattr + page cache invalidation) to powerfs_refresh_wq.
                 * This is non-blocking and safe to call from the RX dispatcher. */
                powerfs_invalidate_one(ino);
            } else if (parent_ino == 0) {
                pr_warn("powerfs: notify frame missing Ino and ParentIno (msg=0x%04x)\n",
                        hdr->msg_type);
            }
        }
        return;
    }

    /* Phase 2: 从响应帧 flags bits 6-7 提取 load_factor, 更新 per-conn 统计.
     * admit() 据此自适应降低并发上限. 必须在 req 处理前完成,
     * 因为 admit() 在 VFS 入口 (锁外) 调用, 需要最新的 lf 值. */
    {
        u8 lf = POWERFS_NET_FLAG_LOAD_FACTOR_GET(hdr->flags);
        int flow_idx = pfs_conn_flow_idx(conn);
        if (flow_idx >= 0 && lf > 0)
            powerfs_flow_on_load_factor(flow_idx, lf);
    }

    /* 按 seq 查找请求, 摘出队列后放锁, 锁外 memcpy + complete.
     * (参考 pfs_process_transmit 的 "摘出-放锁-处理" 模式) */
    spin_lock(&conn->req_lock);
    req = powerfs_req_tree_lookup(conn, hdr->seq);
    if (req) {
        if (!list_empty(&req->list_node))
            list_del_init(&req->list_node);
        powerfs_req_tree_remove(conn, req);
    }
    spin_unlock(&conn->req_lock);

    if (req) {
        /* 先把日志字段读到本地: complete() 后等待线程可能立即释放 req
         * (kref_put → kfree), 再读 req->* 会 UAF. */
        u64 ts_submit = req->ts_submit;
        u64 ts_sent = req->ts_sent;
        u32 seq = req->seq;
        u16 msg_type = req->msg_type;
        u64 now_ns = ktime_get_ns();

        req->ts_recv = now_ns;
        req->resp_status = hdr->status;
        req->error = 0;
        req->resp_body_len = 0;
        if (req->resp_body && body_len > 0) {
            /* K2 (Layer 2): 截断检测 — 不静默 min() 截断.
             *
             * 旧代码用 min(body_len, cap) 静默截断响应, 导致调用方
             * 收到不完整的 TLV 数据, 后续解析消费错误偏移 → 级联故障.
             * 现在检测到截断时设置 req->error = -E2BIG, 调用方可感知
             * 并传播错误到上层, 而不是继续处理残缺数据.
             */
            if (body_len > req->resp_body_cap) {
                pr_err("powerfs: RX_TRUNCATE seq=%u msg=0x%04x body_len=%zu > cap=%zu peer=%s:%u\n",
                       seq, msg_type, body_len, req->resp_body_cap,
                       conn->addr, conn->port);
                req->error = -E2BIG;
                req->resp_body_len = 0;
            } else {
                memcpy(req->resp_body, body, body_len);
                req->resp_body_len = body_len;
            }
        }
        req->resp_data_len = 0;
        if (req->resp_data && data_len > 0) {
            /* K2: data 段截断检测, 同 body 段逻辑 */
            if (data_len > req->resp_data_cap) {
                pr_err("powerfs: RX_TRUNCATE seq=%u msg=0x%04x data_len=%zu > cap=%zu peer=%s:%u\n",
                       seq, msg_type, data_len, req->resp_data_cap,
                       conn->addr, conn->port);
                req->error = -E2BIG;
                req->resp_data_len = 0;
            } else {
                memcpy(req->resp_data, data, data_len);
                req->resp_data_len = data_len;
            }
        }
        powerfs_req_complete(req);

        /* Phase 2: 流控统计 — record_complete (latency + bytes) */
        {
            int flow_idx = pfs_conn_flow_idx(conn);
            u64 lat_ns = ts_submit ? (now_ns - ts_submit) : 0;
            unsigned int total_bytes = body_len + data_len;
            bool error = (hdr->status != 0);

            powerfs_flow_record_complete(flow_idx, lat_ns,
                                         total_bytes, error);
        }

        /* 响应接收计时: >100ms 打 info (帮助定位大响应延迟).
         * 用本地变量, req 可能已被等待线程释放.
         * 分段计时定位瓶颈:
         *   tx_wait = ts_sent - ts_submit  (TX 队列等待 + 调度器唤醒)
         *   net     = ts_recv - ts_sent    (网络往返 + Filer 处理)
         *   rx_disp = now_ns - ts_recv     (RX dispatch 匹配 + memcpy) */
        if (ts_submit) {
            u64 dur_us = div_u64(now_ns - ts_submit, 1000);
            if (dur_us > 100000) {
                u64 tx_wait_us = ts_sent ? div_u64(ts_sent - ts_submit, 1000) : 0;
                u64 net_us = ts_sent ? div_u64(now_ns - ts_sent, 1000) : 0;
                u64 rx_disp_us = 0;  /* RX dispatch 耗时极小 (now_ns ≈ ts_recv) */
                pr_info("powerfs: SLOW_RX seq=%u msg=0x%04x dur=%llums tx_wait=%llums net=%llums rx_disp=%llums\n",
                        seq, msg_type, div_u64(dur_us, 1000),
                        div_u64(tx_wait_us, 1000), div_u64(net_us, 1000),
                        div_u64(rx_disp_us, 1000));
            }
        }
    } else {
        pr_debug("powerfs: RX %s:%u: no pending req for seq=%u\n",
                 conn->addr, conn->port, hdr->seq);
    }
}

/* === 收: 调度器 RX 线程调用, 非阻塞内循环收完所有可用帧 ===
 *
 * 设计要点:
 *   1. MSG_DONTWAIT 永不阻塞 → 单 conn 最多卡一次 syscall (微秒级)
 *   2. 一帧收不全保留 partial → 大响应可跨多次 sk_data_ready 续收
 *   3. 内循环 while(1) 直到 skb_queue_empty → 队列空才退 (对齐诉求1)
 *   4. 单 conn 最多 64 帧后让出 → 防止饿死 sched 上其他 conn
 *   5. EOF/RST/错误 → queue_work(disconnect_work), reset partial */
void pfs_process_receive(struct powerfs_net_server_conn *conn)
{
    int ret;
    int frames = 0;

    /* [RC17 evidence] RDMA RX 调度: 与 TX DISPATCH 对称, 证明 RX worker
     * 实际轮询到 RDMA conns (否则 "invalid frame header"→断连无法
     * 区分是 recv_frame hdr decode 失败 还是 process_receive 根本没跑). */
    if (conn->transport_type == POWERFS_TRANSPORT_RDMA) {
        static atomic_t pfs_tot_rdma_rx_dispatched;
        if (atomic_inc_return(&pfs_tot_rdma_rx_dispatched) <= 8)
            pr_info("powerfs rx: RDMA DISPATCH enter %s:%u channel=%d frames_in_this_run=BEGIN\n",
                    conn->addr, conn->port, conn->channel);
    }

    while (1) {
        ret = conn->transport->recv_frame(conn);
        if (ret == 0) {
            /* 一帧完整: dispatch + reset, 继续收下一帧 */
            pfs_rx_dispatch(conn);
            pfs_rx_reset_partial(conn);
            frames++;
            if (frames >= 64) {
                /* 防止一个 conn 占用 sched 过久, 让出给其他 conn.
                 * rx_ready 的最终决定交给 pfs_rx_thread_fn 在 rx_lock 下做. */
                break;
            }
            continue;
        }
        if (ret == -EAGAIN) {
            /* 当前没数据. 检查 transport->has_rx_data:
             *   true  → 继续 (数据已就绪, 重试 recv_frame)
             *   false → break, rx_ready 的最终决定交给 pfs_rx_thread_fn
             *           在 rx_lock 下做 (避免与回调 race:
             *           无锁清 rx_ready=0 会 clobber 回调刚设的 rx_ready=1) */
            if (conn->transport->has_rx_data(conn))
                continue;
            break;
        }
        /* EOF/RST/错误 → 断连, reset partial 状态 */
        if (conn->transport_type == POWERFS_TRANSPORT_RDMA)
            pr_info("powerfs rx: RDMA RECV ERROR %s:%u ret=%d scheduling disconnect\n",
                    conn->addr, conn->port, ret);
        else
            pr_debug("powerfs: scheduler RX %s:%u step error %d, scheduling disconnect\n",
                    conn->addr, conn->port, ret);
        pfs_rx_reset_partial(conn);
        queue_work(g_pool.reconn_wq, &conn->disconnect_work);
        return;
    }
}

/* === 发: 从 tx_queue 取请求发送 (调度器调用) ===
 *
 * 取 tx_queue 首个 req → kernel_sendmsg.
 * -EAGAIN: 重挂回 tx_queue head, 等 sk_write_space 回调重新投递.
 * <0: 摘除请求 + complete(-ENOTCONN) + schedule disconnect_work.
 * 成功: req 留在 pending_reqs/req_tree 等响应. 若 tx_queue 还有积压,
 *       设置 tx_ready 让调度器重新投递本 conn. */
void pfs_process_transmit(struct powerfs_net_server_conn *conn)
{
    struct powerfs_request *req = NULL;
    struct powerfs_net_frame_hdr hdr;
    int ret;

    /* [RC17 FIX evidence] 首次 RDMA dispatch 打 info: 证明 Fix A
     * (移除 do_send !conn->sock guard) 生效, RDMA conns 真正被
     * 调度器送入 process_transmit 入口 (否则 0 SEND_ENTER 无法
     * 区分 dispatch 前 vs dispatch 内 send_frame 内部故障). */
    if (conn->transport_type == POWERFS_TRANSPORT_RDMA) {
        static atomic_t pfs_tot_rdma_dispatched;
        if (atomic_inc_return(&pfs_tot_rdma_dispatched) <= 16)
            pr_info("powerfs tx: RDMA DISPATCH enter %s:%u channel=%d "
                    "transport=%s is_conn=%d\n",
                    conn->addr, conn->port, conn->channel,
                    conn->transport ? conn->transport->name : "null",
                    conn->transport && conn->transport->is_connected ? 1 : 0);
    }

    /* [ROOT CAUSE 9 FIX] 统一走 transport ops 接口.
     * 旧代码硬编码 `sock = conn->sock; if (!sock) queue_work disconnect`
     * 对 RDMA conn 来说 sock=NULL → 每个 RDMA filer conn 一上来就被触发
     * 断连 → req 也丢了 (已从 tx_queue 删掉但从未进入发送路径) → 全
     * 部 request deadline exceeded. 现在用 transport->is_connected, 对
     * TCP (sock + state==CONNECTED) / RDMA (qp connected) 语义一致. */
    if (!conn->transport || !conn->transport->is_connected(conn)) {
        queue_work(g_pool.reconn_wq, &conn->disconnect_work);
        return;
    }

    spin_lock(&conn->tx_lock);
    req = list_first_entry_or_null(&conn->tx_queue,
                                   struct powerfs_request, tx_list);
    if (req)
        list_del_init(&req->tx_list);
    spin_unlock(&conn->tx_lock);

    if (!req)
        return;     /* tx_queue 空, 调度器会清 tx_scheduled */

    powerfs_net_frame_hdr_encode(&hdr, req->msg_type,
                                  POWERFS_NET_FLAG_REQUEST,
                                  req->seq, 0,
                                  req->req_body_len,
                                  req->req_body_len + req->req_data_len,
                                  pfs_route_hash(conn->client_id, conn->channel));

    if (req->ts_submit == 0)
        req->ts_submit = ktime_get_ns();

    ret = conn->transport->send_frame(conn, &hdr,
                                       req->req_body, req->req_body_len,
                                       req->req_data, req->req_data_len, req);

    if (ret == -EAGAIN || ret == -ENOMEM) {
        /* TCP 缓冲区满 (MSG_DONTWAIT): 保留 send_offset, 重挂 tx_queue head.
         * 等 sk_write_space 回调触发 pfs_tx_schedule 重新调度.
         * send_offset 保证下次从断点续发, 不会重发 header 导致错位. */
        spin_lock(&conn->tx_lock);
        list_add(&req->tx_list, &conn->tx_queue);
        spin_unlock(&conn->tx_lock);
        return;
    }
    if (ret < 0) {
        /* 发送失败: 摘除请求, complete -ENOTCONN, 触发断连.
         * 用 RB_EMPTY_NODE 检查 (而非 list_empty) 防止与 disconnect 竞态:
         * disconnect 已将 req 从 req_tree 移除时, 此处跳过 complete,
         * 由 disconnect 路径统一完成. */
        bool won = false;
        pr_debug("powerfs: tx failed seq=%u msg_type=%u: %d\n",
                 req->seq, req->msg_type, ret);
        spin_lock(&conn->req_lock);
        if (!RB_EMPTY_NODE(&req->rb_node)) {
            list_del_init(&req->list_node);
            powerfs_req_tree_remove(conn, req);
            req->error = -ENOTCONN;
            won = true;
        }
        spin_unlock(&conn->req_lock);
        if (won)
            powerfs_req_complete(req);
        queue_work(g_pool.reconn_wq, &conn->disconnect_work);
        return;
    }

    /* 发送成功: req 留在 pending_reqs/req_tree 等响应.
     * 若 tx_queue 还有积压, 设置 tx_ready 让调度器重新投递.
     * 不清零 ts_submit: 它记录 do_send 入口时间, RX 收到响应时
     * pfs_rx_dispatch 用它计算端到端延迟 (SLOW_RX 日志). */
    req->ts_sent = ktime_get_ns();
    if (req->ts_submit) {
        u64 dur_us = div_u64(req->ts_sent - req->ts_submit, 1000);
        if (dur_us > 50000)  /* >50ms 的大帧发送打 debug 日志 */
            pr_debug("powerfs: tx done seq=%u msg=%u body=%zu data=%zu dur=%llutus\n",
                     req->seq, req->msg_type, req->req_body_len,
                     req->req_data_len, dur_us);
    }
    spin_lock(&conn->tx_lock);
    if (!list_empty(&conn->tx_queue))
        conn->tx_ready = 1;
    spin_unlock(&conn->tx_lock);
}

/* do_send / 重发路径调用: 标记 tx_ready + 投递到 tx_conns + 唤醒 TX 线程.
 * 与 pfs_tx_callback 的核心逻辑相同, 但可从 process context 调用. */
void pfs_tx_schedule(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
    if (!sched)
        return;

    spin_lock_bh(&sched->tx_lock);
    conn->tx_ready = 1;
    if (!conn->tx_scheduled) {
        list_add_tail(&conn->tx_list, &sched->tx_conns);
        conn->tx_scheduled = 1;
        powerfs_conn_get(conn);
        wake_up(&sched->tx_waitq);
    }
    spin_unlock_bh(&sched->tx_lock);
}

/* disconnect 调用: 从 sched 的 rx_conns/tx_conns 摘除 conn.
 * 仅当 conn 在列表上时才 conn_put (释放调度器投递时获取的引用).
 * 若 conn 正被调度器处理 (不在列表但 rx_scheduled=1), 调度器处理完会
 * 发现 rx_ready=0 (回调已 reset) → 自行 put, disconnect 等 kref==1. */
void pfs_conn_remove_from_sched(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
    bool put_rx = false, put_tx = false;

    if (!sched)
        return;

    /* 分别加 rx_lock/tx_lock, 顺序获取不嵌套 → 无死锁风险.
     * rx_list 和 tx_list 是独立链表节点, 分别归属各自队列, 无共享状态. */
    spin_lock_bh(&sched->rx_lock);
    if (conn->rx_scheduled && !list_empty(&conn->rx_list)) {
        list_del_init(&conn->rx_list);
        conn->rx_scheduled = 0;
        conn->rx_ready = 0;
        put_rx = true;
    }
    spin_unlock_bh(&sched->rx_lock);

    spin_lock_bh(&sched->tx_lock);
    if (conn->tx_scheduled && !list_empty(&conn->tx_list)) {
        list_del_init(&conn->tx_list);
        conn->tx_scheduled = 0;
        conn->tx_ready = 0;
        put_tx = true;
    }
    spin_unlock_bh(&sched->tx_lock);

    if (put_rx)
        powerfs_conn_put(conn);
    if (put_tx)
        powerfs_conn_put(conn);
}

/* disconnect_work: sk_state_change/error_report 回调或收发错误检测断连后
 * 调度, 在 process context 执行清理 (回调在 softirq 不能直接调 disconnect_one,
 * 故通过 work 中转). */
void powerfs_conn_disconnect_work_fn(struct work_struct *work)
{
    struct powerfs_net_server_conn *conn = container_of(
        work, struct powerfs_net_server_conn, disconnect_work);

    powerfs_conn_disconnect_one(conn);
}

/* === 4. 单连接 connect/disconnect === */

int powerfs_conn_connect_one(struct powerfs_net_server_conn *conn)
{
    struct socket *sock;
    int ret;

    if (!conn || !conn->in_use)
        return -EINVAL;
    if (conn->state == CONN_CONNECTED)
        return 0;

    powerfs_conn_set_state(conn, CONN_CONNECTING);

#ifdef CONFIG_INFINIBAND
    /* RDMA 路径: 走 transport ops (cm_id + QP + RTS), 不创建 socket.
     * handshake 在 ops->connect 内部或后续 send_frame/recv_frame 层完成.
     * 当前 Phase 2: 仅建立 RDMA 连接, 验证传输层正确性.
     * TODO: handshake 接入 (需通过 transport->send_frame/recv_frame 收发). */
    if (conn->transport_type == POWERFS_TRANSPORT_RDMA && conn->transport) {
        const struct powerfs_transport_ops *ops = conn->transport;

        /* init_conn: 分配 conn->rdma (cm_id/CQ/QP 容器) */
        ret = ops->init_conn(conn);
        if (ret) {
            pr_err("powerfs: rdma init_conn %s:%u failed: %d\n",
                   conn->addr, conn->port, ret);
            powerfs_conn_set_state(conn, CONN_RECONNECTING);
            return ret;
        }
        /* connect: addr/route resolve → PD/CQ/QP → RTS → CONNECTED 事件 */
        ret = ops->connect(conn);
        if (ret) {
            pr_err("powerfs: rdma connect %s:%u failed: %d\n",
                   conn->addr, conn->port, ret);
            ops->fini_conn(conn);
            powerfs_conn_set_state(conn, CONN_RECONNECTING);
            return ret;
        }
        powerfs_conn_set_state(conn, CONN_CONNECTED);
        conn->reconnect_count = 0;
        conn->reconnect_delay = 0;
        atomic_set(&conn->consecutive_timeouts, 0);
        pr_info("powerfs: rdma filer %s:%u connected\n", conn->addr, conn->port);
        return 0;
    }
#endif /* CONFIG_INFINIBAND */

    /* TCP 路径 (现有逻辑) */
    sock = powerfs_net_create_tcp_socket();
    if (!sock) {
        powerfs_conn_set_state(conn, CONN_RECONNECTING);
        return -ENOMEM;
    }

    ret = powerfs_net_tcp_connect(sock, conn->addr, conn->port);
    if (ret < 0) {
        powerfs_net_close_socket(sock);
        powerfs_conn_set_state(conn, CONN_RECONNECTING);
        return ret;
    }

    ret = powerfs_conn_do_handshake(sock, conn);
    if (ret < 0) {
        powerfs_net_close_socket(sock);
        powerfs_conn_set_state(conn, CONN_RECONNECTING);
        return ret;
    }

    /* 设置 socket recv 超时为 10s (调度器 process context recv 用) */
    sock->sk->sk_rcvtimeo = msecs_to_jiffies(POWERFS_NET_RECV_TIMEOUT);
    /* connect 阶段用 CONNECT_TIMEOUT (3s), 连接成功后改回 SEND_TIMEOUT (10s)
     * 供后续 kernel_sendmsg 使用. */
    sock->sk->sk_sndtimeo = msecs_to_jiffies(POWERFS_NET_SEND_TIMEOUT);

    conn->sock = sock;
    powerfs_conn_set_state(conn, CONN_CONNECTED);
    conn->reconnect_count = 0;
    conn->reconnect_delay = 0;  /* 成功连接: 重置指数退避 */
    atomic_set(&conn->consecutive_timeouts, 0);  /* 重置半开检测计数器 */

    /* v2: 安装 sk 回调 (替换 v1 的 kthread_run(rx_thread)).
     * 必须在状态转为 CONNECTED 后安装, 回调依赖 conn->sock 稳定.
     * set_state(CONN_CONNECTED) 已触发 route 恢复.
     * 断连期间的请求由主线程在 submit 循环中自己重试, 不需要重连线程重发. */
    pfs_conn_set_callbacks(conn);

    pr_debug("powerfs: filer %s:%u connected (v2 scheduler)\n", conn->addr, conn->port);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_conn_connect_one);

void powerfs_conn_disconnect_one(struct powerfs_net_server_conn *conn)
{
    struct socket *sock = NULL;
    int filer_idx;

    if (!conn || !conn->in_use)
        return;

    /* 幂等: 原子检查并转换状态 (CONNECTED → RECONNECTING).
     * 只有第一个调用者通过此检查, 后续调用 (含 re-entrant disconnect_work
     * 由 sk 回调/调度器收发错误调度) 看到 RECONNECTING 直接返回. */
    spin_lock(&conn->state_lock);
    if (conn->state != CONN_CONNECTED) {
        spin_unlock(&conn->state_lock);
        return;
    }
    conn->state = CONN_RECONNECTING;
    sock = conn->sock;  /* 保存 sock 指针, 暂不置 NULL (调度器仍在使用) */
    spin_unlock(&conn->state_lock);

    pr_debug("powerfs: filer %s:%u state CONNECTED -> RECONNECTING (disconnect)\n",
            conn->addr, conn->port);

    /* Phase 1: 记录断连时间戳, 供 powerfs_net_recently_disconnected 判断.
     * 让此后窗口内的 lookup/readdir 走短超时, 避免长时间持 VFS 锁等重连. */
    atomic_long_set(&g_pool.last_disconnect_jiffies, jiffies);

    /* 路由降级: 所有 leader=该filer 的 shard → CHECKING
     * (find_available_filer 会跳过 RECONNECTING 的 filer, 请求路由到其他 filer).
     * 正常由 powerfs_conn_set_state 触发, 此处手动调用 (状态已原子转换). */
    filer_idx = powerfs_conn_get_filer_idx(conn);
    if (filer_idx >= 0)
        powerfs_shard_route_on_filer_disconnect(filer_idx);

#ifdef CONFIG_INFINIBAND
    /* RDMA 路径: 断开 RDMA 连接 + 释放资源, 跳过 socket 操作.
     * pfs_conn_remove_from_sched 对 RDMA 仍需要 (清理调度器引用),
     * 但 sk 回调 reset 仅 TCP 有意义 (RDMA 无 sk 回调).
     *
     * [ROOT CAUSE 8 FIX / slab corruption]
     * RDMA disconnect 必须与 TCP 路径等价: remove_from_sched 之后,
     * 先唤醒 sched->rx/tx_waitq (RDMA 没有 socket shutdown 可以唤醒
     * 在 wait_event_interruptible(rx_conns 空) 上的调度器线程),
     * 然后等待 conn->kref 引用降到 1 (调度器已完全放下 conn 引用,
     * 不会再调用 recv_frame/send_frame 访问 rdma 对象).
     * 之前 RDMA 缺 kref wait, 导致 sched 线程仍在用 rdma 时我们就
     * ops->fini_conn kfree(conn->rdma) → use-after-free → slab freelist
     * 被脏写 → 下次 reconnect alloc_pd 内部 kmalloc_cache memset
     * 踩到非法 RDI → __kmalloc_noprof Oops panic. */
    if (conn->transport_type == POWERFS_TRANSPORT_RDMA && conn->transport) {
        const struct powerfs_transport_ops *ops = conn->transport;
        long wr;
        /* 1) 从调度器队列摘 (列表上的 conn 会立即 put; 不在列表但正被 sched
         *    线程处理的 conn, sched 处理完会自己 put, 需等下方 kref wait) */
        pfs_conn_remove_from_sched(conn);
        /* 2) 唤醒 conn->sched 归属的 rx/tx 调度器线程, 让它们立即退出
         *    wait_event(rx/tx_conns 空), 看到 conn->state=RECONNECTING
         *    立即 put conn 引用 (释放).
         *    RDMA 没有 socket shutdown 隐式唤醒, 必须显式.
         *    同时遍历所有 sched 兜底唤醒 (防止归属 hash 与实际 pick 不一致,
         *    或 vol_sched 通路: 参考 pool_exit 唤醒模式). */
        if (conn->sched) {
            wake_up_all(&conn->sched->rx_waitq);
            wake_up_all(&conn->sched->tx_waitq);
        }
        {
            int i;
            for (i = 0; i < g_pool.num_sched; i++) {
                wake_up_all(&g_pool.schedulers[i].rx_waitq);
                wake_up_all(&g_pool.schedulers[i].tx_waitq);
            }
            for (i = 0; i < g_pool.num_vol_sched; i++) {
                wake_up_all(&g_pool.vol_schedulers[i].rx_waitq);
                wake_up_all(&g_pool.vol_schedulers[i].tx_waitq);
            }
        }
        /* 3) 等待 conn->kref 引用降到 1 (只剩 conn->owner 单引用).
         *    == 调度器线程已经完全放下 conn, 不会再调用
         *       ops->recv_frame / ops->send_frame / ops->has_rx_data 访问 conn->rdma.
         *    == 才安全执行 ops->disconnect (destroy cm_id/qp/cq) + ops->fini_conn
         *       (kfree(conn->rdma)).
         *    [ROOT CAUSE 8 FIX] 之前缺此步骤: sched 线程仍在用 rdma 对象时
         *    fini_conn 就 kfree → use-after-free → slab freelist 脏写
         *    → reconnect alloc_pd → kmalloc_cache memset(illegal_addr,0,512)
         *    → __kmalloc_noprof Oops → panic_on_oops → VM reboot.
         *    超时兜底 15s, 与 TCP 路径 L2082-L2089 一致. */
        wr = wait_event_timeout(conn->sock_user_wq,
            kref_read(&conn->kref) == 1,
            msecs_to_jiffies(15000));
        if (wr == 0)
            pr_warn("powerfs: rdma filer %s:%u disconnect: kref refcount=%d after 15s "
                    "(use-after-free risk)\n",
                    conn->addr, conn->port, kref_read(&conn->kref));
        /* 4) 安全销毁 RDMA 资源 + 私有 rdma conn 对象 (kref==1 → 无并发访问) */
        ops->disconnect(conn);
        ops->fini_conn(conn);
        pr_debug("powerfs: rdma filer %s:%u disconnected\n",
                 conn->addr, conn->port);
        goto disconnect_done;
    }
#endif /* CONFIG_INFINIBAND */

    /* 1. v2: 恢复 sk 回调 + 清 sk_user_data (此后 softirq 回调 NOOP, 调原始回调).
     *    必须在 shutdown 前执行, 防止 shutdown 触发的回调访问已拆的 conn. */
    if (sock)
        pfs_conn_reset_callbacks(conn);

    /* 2. v2: 从 sched->rx_conns/tx_conns 摘除 conn (清 rx/tx_scheduled, put 引用).
     *    若 conn 正被调度器处理 (不在列表), 调度器处理完会自行 put (见下文 kref wait). */
    pfs_conn_remove_from_sched(conn);

    /* 3. shutdown socket → 唤醒可能在 wait_event 的调度器线程 (recv/send 立即返回错误).
     *    回调已 reset, 不会再投递到 sched 队列. */
    if (sock)
        kernel_sock_shutdown(sock, SHUT_RDWR);

disconnect_done:
    /* 4. 唤醒在途请求的主线程: 以 -ENOTCONN complete 所有 pending 请求.
     *    主线程的 do_send 从 wait_for_completion_timeout 醒来, 看到 -ENOTCONN,
     *    返回到 submit 循环, 由主线程自己重试 (重新查路由, 选其他 filer 或等重连).
     *
     *    设计决策: resend 由主线程自己做, 不由重连线程操作.
     *    - 主线程独占 request 生命周期, 无并发访问, 无竞态.
     *    - 重连线程只负责重建连接, 不碰 request.
     *    (参照 FAULT 路径的 cancel 逻辑, 此处复用) */
    {
        struct powerfs_request *req, *tmp;
        LIST_HEAD(cancel_list);

        spin_lock(&conn->req_lock);
        list_splice_init(&conn->pending_reqs, &cancel_list);
        list_for_each_entry(req, &cancel_list, list_node) {
            powerfs_req_tree_remove(conn, req);
            req->error = -ENOTCONN;
        }
        spin_unlock(&conn->req_lock);

        list_for_each_entry_safe(req, tmp, &cancel_list, list_node) {
            list_del_init(&req->list_node);
            pr_debug("powerfs: disconnect cancel req seq=%u msg_type=%u on filer %s:%u\n",
                     req->seq, req->msg_type, conn->addr, conn->port);
            powerfs_req_complete(req);
        }
    }

    /* 5. 排空 tx_queue (调度器可能已取走部分, 剩余的从队列摘除).
     *    请求已在步骤 4 被 complete, 此处仅清理 tx_list 引用. */
    {
        struct powerfs_request *req, *tmp;
        LIST_HEAD(tx_drain);
        spin_lock(&conn->tx_lock);
        list_splice_init(&conn->tx_queue, &tx_drain);
        spin_unlock(&conn->tx_lock);
        list_for_each_entry_safe(req, tmp, &tx_drain, tx_list) {
            list_del_init(&req->tx_list);
        }
    }

    /* 6. v2: 退役 sock 前, 等待调度器放下 conn (kref refcount==1, 即只剩 owner 引用).
     *    调度器若正在 process_receive/transmit (持 conn 引用), 此处等待其完成.
     *    shutdown 已使 recv/send 返回错误, 调度器很快退出处理并 put. */
    spin_lock(&conn->state_lock);
    conn->sock = NULL;
    spin_unlock(&conn->state_lock);

    if (sock) {
        long wr = wait_event_timeout(conn->sock_user_wq,
            kref_read(&conn->kref) == 1,
            msecs_to_jiffies(15000));
        if (wr == 0)
            pr_warn("powerfs: filer %s:%u disconnect: kref refcount=%d after 15s\n",
                    conn->addr, conn->port, kref_read(&conn->kref));
        powerfs_net_close_socket(sock);
    }

    /* v2: 重置 RX partial 状态 (调度器已退出, kref==1, 安全重置).
     * 重连后复用同一 conn, 必须清掉旧连接残留的 partial 收帧进度,
     * 否则下次收帧会从错误偏移开始 → 帧错位. */
    pfs_rx_reset_partial(conn);

    /* 唤醒可能在等待重连完成的请求 (兼容旧 submit 路径, 新设计不再等待) */
    wake_up(&conn->reconnect_wq);

    pr_debug("powerfs: filer %s:%u disconnected (v2 scheduler)\n", conn->addr, conn->port);

    /* 8. 调度重连 (stopping 时不调度, 由 pool_exit 主导清理) */
    if (!atomic_read(&g_pool.stopping))
        queue_delayed_work(g_pool.reconn_wq, &conn->reconnect_work,
                              msecs_to_jiffies(POWERFS_NET_BASE_DELAY));
}
EXPORT_SYMBOL_GPL(powerfs_conn_disconnect_one);

/* === 5. 单连接重连 work === */

void powerfs_conn_reconnect_work_fn(struct work_struct *work)
{
    struct powerfs_net_server_conn *conn = container_of(
        to_delayed_work(work), struct powerfs_net_server_conn, reconnect_work);
    int ret;

    if (atomic_read(&g_pool.stopping))
        return;
    if (conn->state == CONN_CONNECTED)
        return;

    if (conn->reconnect_count >= POWERFS_NET_MAX_RECONNECT) {
        const char *ctype = (conn->type == POWERFS_NET_SERVER_VOLUME) ? "volume" : "filer";
        pr_err("powerfs: %s %s:%u reconnect failed %d times, FAULT, will retry in 5s\n",
               ctype, conn->addr, conn->port, conn->reconnect_count);
        powerfs_conn_set_state(conn, CONN_FAULT);
        /* 5 秒后自动重试, 避免永久 FAULT.
         * 重置计数, 下次从 attempt 1 开始.
         * 注意: 之前是 30s, 但 30s 超过 lookup 的 60s deadline,
         * 导致断连期间排队的请求在 filer 重连前就超时.
         * 5s 加快恢复速度, 配合 60s lookup 超时覆盖 filer 重启场景. */
        conn->reconnect_count = 0;
        conn->reconnect_delay = msecs_to_jiffies(5000);
        if (!atomic_read(&g_pool.stopping))
            queue_delayed_work(g_pool.reconn_wq, &conn->reconnect_work,
                conn->reconnect_delay);
        return;
    }

    conn->reconnect_count++;
    {
        const char *ctype = (conn->type == POWERFS_NET_SERVER_VOLUME) ? "volume" : "filer";
        pr_debug("powerfs: reconnecting %s %s:%u (attempt %d/%d)\n",
                ctype, conn->addr, conn->port, conn->reconnect_count,
                POWERFS_NET_MAX_RECONNECT);
    }

    ret = powerfs_conn_connect_one(conn);
    if (ret == 0) {
        const char *ctype = (conn->type == POWERFS_NET_SERVER_VOLUME) ? "volume" : "filer";
        pr_debug("powerfs: %s %s:%u reconnected\n", ctype, conn->addr, conn->port);
    } else {
        /* 指数退避: BASE_DELAY * 2^(attempt-1), 上限 MAX_DELAY
         * (参照  con->delay). 成功连接时在 connect_one 中归零. */
        if (conn->reconnect_delay == 0)
            conn->reconnect_delay = msecs_to_jiffies(POWERFS_NET_BASE_DELAY);
        else
            conn->reconnect_delay = min(conn->reconnect_delay * 2,
                                         msecs_to_jiffies(POWERFS_NET_MAX_DELAY));

        if (!atomic_read(&g_pool.stopping))
            queue_delayed_work(g_pool.reconn_wq, &conn->reconnect_work,
                conn->reconnect_delay);
    }
}

/* === 5b. KeepConnected 心跳 (B1: 对齐 FUSE MasterStatsReporter 30s 周期) === */

/* 独立 raw socket 发送一次 KeepConnected 请求到指定 master addr/port.
 * 与 register_client (9550+) 的网络模式完全一致: 短连接 raw-socket +
 * handshake → 1次请求/响应 → close. 不介入 g_pool filer 连接池, 网络
 * 失败不影响 VFS 请求 (best-effort, 下轮重试).
 *
 * 返回: 0=成功收到 STATUS_OK/STATUS_OK_REDIRECT 处理完, <0=-errno */
int powerfs_heartbeat_send_one(const char *master_addr, __u16 master_port,
                                      const __u8 *req_body, size_t req_body_len)
{
    struct socket *sock;
    struct powerfs_net_frame_hdr hdr;
    __u8 *resp_body;
    __u8 resp_data[64];
    __u32 seq;
    size_t body_len = 0, data_len = 0;
    int ret, ri;

    resp_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    if (!resp_body)
        return -ENOMEM;

    sock = powerfs_net_create_tcp_socket();
    if (!sock) {
        kfree(resp_body);
        return -ENOMEM;
    }

    ret = powerfs_net_tcp_connect(sock, master_addr, master_port);
    if (ret < 0)
        goto out_close;

    ret = powerfs_net_do_handshake(sock);
    if (ret < 0)
        goto out_close;

    seq = atomic_inc_return(&g_discover_seq);
    powerfs_net_frame_hdr_encode(&hdr,
                                  POWERFS_NET_MSG_KEEP_CONNECTED,
                                  POWERFS_NET_FLAG_REQUEST,
                                  seq, 0, (__u32)req_body_len,
                                  (__u32)req_body_len, 0);

    ret = powerfs_net_frame_send(sock, &hdr, req_body, req_body_len, NULL, 0);
    if (ret < 0)
        goto out_close;

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
    if (ret < 0)
        goto out_close;

    /* REDIRECT: 跟随一次 (和 discover_filers/discover_volumes 一致). */
    if (hdr.status == POWERFS_NET_STATUS_ERR_REDIRECT) {
        struct powerfs_tlv_dec rdec;
        char redirect_addr[64];

        powerfs_net_close_socket(sock);
        sock = NULL;

        powerfs_tlv_dec_init(&rdec, resp_body, body_len);
        if (powerfs_tlv_dec_string(&rdec, POWERFS_NET_FLD_OWNER,
                                    redirect_addr,
                                    sizeof(redirect_addr) - 1) != 0 ||
            redirect_addr[0] == '\0') {
            ret = -EREMOTEIO;
            goto out_close;
        }

        sock = powerfs_net_create_tcp_socket();
        if (!sock) {
            ret = -ENOMEM;
            goto out_close;
        }
        ret = powerfs_net_tcp_connect(sock, redirect_addr, master_port);
        if (ret < 0)
            goto out_close;
        ret = powerfs_net_do_handshake(sock);
        if (ret < 0)
            goto out_close;
        seq = atomic_inc_return(&g_discover_seq);
        powerfs_net_frame_hdr_encode(&hdr,
                                      POWERFS_NET_MSG_KEEP_CONNECTED,
                                      POWERFS_NET_FLAG_REQUEST,
                                      seq, 0, (__u32)req_body_len,
                                      (__u32)req_body_len, 0);
        ret = powerfs_net_frame_send(sock, &hdr, req_body, req_body_len, NULL, 0);
        if (ret < 0)
            goto out_close;
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
        if (ret < 0)
            goto out_close;
    }

    ret = (hdr.status == POWERFS_NET_STATUS_OK) ? 0 : -EREMOTEIO;

out_close:
    if (sock)
        powerfs_net_close_socket(sock);
    kfree(resp_body);
    return ret;
}

void powerfs_heartbeat_work_fn(struct work_struct *work)
{
    struct delayed_work *dw = to_delayed_work(work);
    struct powerfs_net_pool *pool = container_of(dw, struct powerfs_net_pool,
                                                  heartbeat_work);
    __u8 *req_body;
    size_t req_body_len;
    int ret = -ENODEV;
    char *cert_pem = NULL;
    size_t cert_len = 0;
    struct powerfs_tlv_enc enc;
    /* Multi-master comma-list 轮询 (对齐 register_client / discover_volumes).
     * master_addr 字段保存逗号列表 "ip1,ip2,ip3", 逐个 endpoint 尝试.
     * kmalloc 本地副本避免 strsep 修改 g_pool.master_addr. */
    char *addr_copy = NULL;
    char *ap, *tok;

    if (atomic_read(&pool->stopping))
        return;

    if (!pool->master_set || !pool->hb_client_uuid[0])
        goto requeue;

    req_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    if (!req_body)
        goto requeue;

    /* 编码 KeepConnected TLV (与 FUSE topology.rs:589-600 完全对齐).
     * assigned_client_id=0 时不嵌入 ClientId 字段 (和 FUSE Option 一致). */
    powerfs_tlv_enc_init(&enc, req_body, POWERFS_NET_MAX_BODY);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_UUID,
                           pool->hb_client_uuid, strlen(pool->hb_client_uuid));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_BACKEND,
                           pool->hb_client_type, strlen(pool->hb_client_type));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                           pool->hb_mount_point, strlen(pool->hb_mount_point));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_COLLECTION,
                           pool->hb_collection, strlen(pool->hb_collection));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_REPLICATION,
                           pool->hb_replication, strlen(pool->hb_replication));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_OWNER,
                           pool->hb_host, strlen(pool->hb_host));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_LIMIT, pool->hb_pid);
    if (pool->hb_assigned_client_id > 0)
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_CLIENT_ID,
                            pool->hb_assigned_client_id);
    /* ClientCert PEM (生产模式强制校验; 和 register_client 9536-9539 一致).
     * 失败 / 路径空 → 不嵌入 0xD4, master dev mode 允许, enforce mode 拒. */
    if (pool->hb_client_crt[0]) {
        cert_pem = powerfs_read_pem_file(pool->hb_client_crt, GFP_KERNEL,
                                          &cert_len);
        if (cert_pem && cert_len > 0)
            powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_CLIENT_CERT,
                                    cert_pem, cert_len);
    }
    req_body_len = powerfs_tlv_enc_len(&enc);

    /* === Multi-master 轮询: master_addr = "ip1,ip2,...,ipN" === */
    addr_copy = kstrdup(pool->master_addr, GFP_KERNEL);
    if (!addr_copy)
        goto out_req;

    ap = addr_copy;
    while ((tok = strsep(&ap, ",")) != NULL) {
        while (*tok == ' ') tok++;
        if (tok[0] == '\0') continue;

        ret = powerfs_heartbeat_send_one(tok, pool->master_port,
                                          req_body, req_body_len);
        if (ret == 0) {
            /* 成功: 顺便把 leader 记到 g_pool.master_addr 首个字段,
             * 优化下次轮询顺序 (但永久原列表保留在 fill_super context,
             * 这里保持逗号列表不变, 仅记录 endpoint 命中是 best-effort). */
            break;
        }
    }

    if (ret < 0) {
        pr_warn_ratelimited(
            "powerfs: KeepConnected heartbeat (all %u master endpoints tried) "
            "last_ret=%d; will retry in %us (uuid=%s assigned=%llu masters=%s)\n",
            (unsigned)0 /* endpoint_count 简化占位 */,
            ret, POWERFS_HB_INTERVAL_SECS, pool->hb_client_uuid,
            (unsigned long long)pool->hb_assigned_client_id,
            pool->master_addr);
    } else {
        pr_info_ratelimited(
            "powerfs: KeepConnected heartbeat OK (uuid=%s assigned=%llu)\n",
            pool->hb_client_uuid,
            (unsigned long long)pool->hb_assigned_client_id);
    }

    kfree(addr_copy);
out_req:
    kfree(cert_pem);
    kfree(req_body);

requeue:
    if (!atomic_read(&pool->stopping) && pool->heartbeat_started &&
        pool->reconn_wq)
        queue_delayed_work(pool->reconn_wq, &pool->heartbeat_work,
                            POWERFS_HB_INTERVAL_SECS * HZ);
}

/**
 * powerfs_net_start_heartbeat - 启动 KeepConnected 周期心跳.
 * 在 fill_super register_client 成功后调用. 参数存副本到 g_pool. */
int powerfs_net_start_heartbeat(const char *client_uuid,
                                const char *client_type,
                                const char *mount_point,
                                const char *collection,
                                const char *replication,
                                const char *host,
                                __u64 pid,
                                u64 assigned_client_id,
                                const char *client_crt_path)
{
    if (atomic_read(&g_pool.stopping))
        return -ESHUTDOWN;
    if (!g_pool.reconn_wq)
        return -ENODEV;
    if (!client_uuid || !client_uuid[0] || !mount_point)
        return -EINVAL;

    strncpy(g_pool.hb_client_uuid,    client_uuid,    sizeof(g_pool.hb_client_uuid)    - 1);
    strncpy(g_pool.hb_client_type,    client_type ? client_type : "kernel",
            sizeof(g_pool.hb_client_type) - 1);
    strncpy(g_pool.hb_mount_point,   mount_point,    sizeof(g_pool.hb_mount_point)   - 1);
    strncpy(g_pool.hb_collection,    collection  ? collection  : "",  sizeof(g_pool.hb_collection)  - 1);
    strncpy(g_pool.hb_replication,   replication ? replication : "",  sizeof(g_pool.hb_replication) - 1);
    strncpy(g_pool.hb_host,          host        ? host        : "",  sizeof(g_pool.hb_host)        - 1);
    if (client_crt_path)
        strncpy(g_pool.hb_client_crt, client_crt_path, sizeof(g_pool.hb_client_crt) - 1);
    g_pool.hb_pid                 = pid;
    g_pool.hb_assigned_client_id  = assigned_client_id;

    INIT_DELAYED_WORK(&g_pool.heartbeat_work, powerfs_heartbeat_work_fn);
    g_pool.heartbeat_started = true;
    /* 立即排队首次心跳 (delay=0), 之后每 POWERFS_HB_INTERVAL_SECS 重排.
     * 对齐 FUSE MasterStatsReporter.start 立刻发一次 KeepConnected,
     * 避免 ~30s 空窗口内 master last_heartbeat 判定过期. */
    queue_delayed_work(g_pool.reconn_wq, &g_pool.heartbeat_work, 0);
    pr_info("powerfs: KeepConnected heartbeat started (interval=%us uuid=%s assigned=%llu)\n",
            POWERFS_HB_INTERVAL_SECS, g_pool.hb_client_uuid,
            (unsigned long long)g_pool.hb_assigned_client_id);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_start_heartbeat);

/**
 * powerfs_net_update_heartbeat_id - 刷新 assigned_client_id.
 * 若 start 被调时 assigned=0 (register_client 还没回来), 赋值完成后调本函数. */
void powerfs_net_update_heartbeat_id(u64 assigned_client_id)
{
    g_pool.hb_assigned_client_id = assigned_client_id;
}
EXPORT_SYMBOL_GPL(powerfs_net_update_heartbeat_id);

/**
 * powerfs_net_stop_heartbeat - 停止心跳 (umount kill_sb 调用). */
void powerfs_net_stop_heartbeat(void)
{
    if (g_pool.heartbeat_started) {
        g_pool.heartbeat_started = false;
        cancel_delayed_work_sync(&g_pool.heartbeat_work);
        pr_info("powerfs: KeepConnected heartbeat stopped\n");
    }
}
EXPORT_SYMBOL_GPL(powerfs_net_stop_heartbeat);

/* === 6. 连接池 init/exit === */

int powerfs_conn_pool_init(const char *master_addr, __u16 master_port, __u16 shard_count,
                           enum powerfs_transport_type transport_type)
{
    int i, ret;

    /* 防御: shard_count 必须 >= 1, 否则 % shard_count 会除 0. */
    if (shard_count == 0)
        shard_count = 1;

    /* 1. 存储 master 地址 (可选, 用于后续动态发现) */
    if (master_addr) {
        strncpy(g_pool.master_addr, master_addr, sizeof(g_pool.master_addr) - 1);
        g_pool.master_port = master_port;
        g_pool.master_set = true;
    }
    /* 传输层类型: 由 fill_super 从 mount -o transport= 解析传入.
     * conn 初始化 (本函数下方) + powerfs_conn_connect_one 分流都读此字段. */
    g_pool.transport_type = transport_type;
    atomic_set(&g_pool.stopping, 0);
    g_pool.heartbeat_started = false;

    /* v2: 创建独立重连/断连 workqueue.
     * WQ_UNBOUND: work 在不同 CPU 并行, 避免多 filer 重连串行阻塞.
     * (system_wq 是 WQ_UNBOUND 但共享, 多 filer connect 各阻塞 3s 会累积) */
    if (!g_pool.reconn_wq) {
        g_pool.reconn_wq = alloc_workqueue("powerfs_reconn",
                                            WQ_UNBOUND | WQ_FREEZABLE, 0);
        if (!g_pool.reconn_wq) {
            pr_err("powerfs: failed to create reconnect workqueue\n");
            return -ENOMEM;
        }
    }

    /* v2: 初始化 per-CPU 调度器 (必须在创建 conn 之前, 因 conn->sched =
     * pfs_pick_sched(addr) 依赖 schedulers[] 已分配). 幂等: 已分配则跳过. */
    ret = powerfs_sched_init();
    if (ret) {
        pr_err("powerfs: scheduler init failed: %d\n", ret);
        return ret;
    }

    /* 2. 初始化 shard 路由表.
     *
     * shard_count 来自 per-mount ctx → sbi->shard_count (mount -o shard_count=N),
     * 不再从全局 module_param 读取: 同一个 ko 多个 mount 到不同集群
     * 时, 各集群可配置独立分片. 目前连接池为单例 g_pool (不支持多集群并发),
     * 二次 mount 若 master_addr 不同会在 fill_super 中检查冲突并拒绝;
     * 但 shard_count 即便不同, 新 mount 的参数也会覆盖 pool 中的值,
     * 这是 per-sb 参数在单例 pool 中的预期传播行为. */
    spin_lock_init(&g_pool.shard_route.lock);
    for (i = 0; i < POWERFS_MAX_SHARDS; i++) {
        g_pool.shard_route.entries[i].leader_filer_idx = -1;
        g_pool.shard_route.entries[i].state = ROUTE_UNKNOWN;
        INIT_LIST_HEAD(&g_pool.shard_route.entries[i].pending_reqs);
        spin_lock_init(&g_pool.shard_route.entries[i].req_lock);
    }
    g_pool.shard_route.shard_count = shard_count;

    /* Initialize global ShardMap from per-mount mount-option fallback.
     * Overridden by Master topology (0xBD) in discover_volumes when
     * the Master advertises ShardMapEntries. Equivalent to Rust's
     * ShardMap::from_shard_count — equal 1M ranges per shard. */
    shard_map_from_shard_count(shard_count);

    /* 3. 从现有 servers[] 列表初始化 filer 连接
     *    (servers[] 由 powerfs_net_discover_filers 或 powerfs_net_set_filers 填充)
     *    对每个 FILER 类型的 server, 创建一个 conn 条目 */
    g_pool.filer_count = 0;
    for (i = 0; i < g_pool.server_count && g_pool.filer_count < POWERFS_NET_MAX_FILERS; i++) {
        struct powerfs_net_server_entry *srv = &g_pool.servers[i];
        struct powerfs_net_server_conn *conn;

        if (srv->type != POWERFS_NET_SERVER_FILER)
            continue;

        conn = &g_pool.filers[g_pool.filer_count];
        memset(conn, 0, sizeof(*conn));

        strncpy(conn->addr, srv->addr, sizeof(conn->addr) - 1);
        conn->port = srv->port;
        conn->type = srv->type;
        conn->in_use = true;
        /* 按 g_pool.transport_type 选 ops (mount -o transport=tcp|rdma).
         * TCP 走现有 socket 路径, RDMA 走 powerfs_rdma_ops (CONFIG_INFINIBAND). */
        conn->transport = powerfs_transport_pick_ops(g_pool.transport_type);
        conn->transport_type = g_pool.transport_type;
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
        /* v2: sk 回调驱动 + per-CPU 调度器 (替换 v1 per-conn RX 线程) */
        INIT_WORK(&conn->disconnect_work, powerfs_conn_disconnect_work_fn);
        conn->sched = pfs_pick_sched(conn->addr);
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
        /* kref=1: owner 引用 (g_pool 持有, disconnect 等 refcount==1 才 sock_release) */
        kref_init(&conn->kref);

        /* v2: per-conn RX buffer (非阻塞状态机需要保留 partial, 不能用 sched 共享) */
        if (pfs_conn_alloc_rxbuffers(conn)) {
            pr_err("powerfs: filer %s:%u alloc rx buffers failed\n",
                   conn->addr, conn->port);
            conn->in_use = false;
            continue;
        }

        g_pool.filer_count++;
    }

    pr_debug("powerfs: connection pool: %d filers\n", g_pool.filer_count);

    /* 3b. 从现有 servers[] 列表初始化 volume 连接 (与 filer 同模式) */
    g_pool.volume_count = 0;
    spin_lock_init(&g_pool.vol_route_lock);
    g_pool.vol_route_count = 0;
    for (i = 0; i < g_pool.server_count && g_pool.volume_count < POWERFS_NET_MAX_VOLUMES; i++) {
        struct powerfs_net_server_entry *srv = &g_pool.servers[i];
        struct powerfs_net_server_conn *conn;

        if (srv->type != POWERFS_NET_SERVER_VOLUME)
            continue;

        conn = &g_pool.volumes[g_pool.volume_count];
        memset(conn, 0, sizeof(*conn));

        strncpy(conn->addr, srv->addr, sizeof(conn->addr) - 1);
        conn->port = srv->port;
        conn->type = srv->type;
        conn->in_use = true;
        /* Volume connections ALWAYS use TCP regardless of mount -o transport=xxx.
         * volume.toml comment documents: "内核 vol_route 仅支持TCP, volume需
         * TCP listener".  transport=rdma mount-opt controls ONLY the filer
         * metadata channel. BUG: using g_pool.transport_type here propagated
         * RDMA ops into the OSD volume path → immediate EOF on TCP volume
         * listener → ret=-107 (ENOTCONN) on every MIGRATE WriteNeedle. */
        conn->transport = powerfs_transport_pick_ops(POWERFS_TRANSPORT_TCP);
        conn->transport_type = POWERFS_TRANSPORT_TCP;
        pr_info("powerfs: volume[%d] conn %s:%u init transport=tcp (forced, global transport_type=%s per mount)\n",
                g_pool.volume_count, conn->addr, conn->port,
                (g_pool.transport_type == POWERFS_TRANSPORT_RDMA) ? "rdma" : "tcp");
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
        /* P3.1: volume 连接走独立调度器池 (vol_schedulers), 避免数据 I/O 饿死元数据 */
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

        /* v2: per-conn RX buffer (与 filer 同模式, 支持非阻塞断点续收) */
        if (pfs_conn_alloc_rxbuffers(conn)) {
            pr_err("powerfs: volume %s:%u alloc rx buffers failed\n",
                   conn->addr, conn->port);
            conn->in_use = false;
            continue;
        }

        g_pool.volume_count++;
    }

    pr_debug("powerfs: connection pool: %d volumes\n", g_pool.volume_count);

    /* 4. 并行连接所有 filer */
    for (i = 0; i < g_pool.filer_count; i++) {
        queue_delayed_work(g_pool.reconn_wq, &g_pool.filers[i].reconnect_work, 0);
    }

    /* 4b. 并行连接所有 volume (不阻塞 mount, 后台连接) */
    for (i = 0; i < g_pool.volume_count; i++) {
        queue_delayed_work(g_pool.reconn_wq, &g_pool.volumes[i].reconnect_work, 0);
    }

    /* 5. 等待至少一个 filer 连接成功 (30s 超时) */
    {
        int max_wait = 300;  /* 30s, 每 100ms 检查一次 */
        while (max_wait-- > 0) {
            for (i = 0; i < g_pool.filer_count; i++) {
                if (g_pool.filers[i].state == CONN_CONNECTED) {
                    pr_debug("powerfs: at least one filer connected\n");
                    return 0;
                }
            }
            msleep(100);
        }
    }

    pr_warn("powerfs: no filer connected within 30s\n");
    return -ENOTCONN;
}
EXPORT_SYMBOL_GPL(powerfs_conn_pool_init);

void powerfs_conn_pool_exit(void)
{
    int i;

    atomic_set(&g_pool.stopping, 1);
    /* B1: 停止心跳, 保证 cancel_delayed_work_sync 在 reconn_wq 释放前执行. */
    powerfs_net_stop_heartbeat();

    /* 断开并清理所有 filer 连接.
     * disconnect_one 是幂等入口: reset_callbacks → 摘 sched 队列 →
     * shutdown socket → 唤醒在途请求 → 等 kref → 关闭 sock.
     * stopping=1 时不会 schedule reconnect. */
    for (i = 0; i < g_pool.filer_count; i++) {
        struct powerfs_net_server_conn *conn = &g_pool.filers[i];
        if (!conn->in_use)
            continue;

        cancel_delayed_work_sync(&conn->reconnect_work);
        powerfs_conn_disconnect_one(conn);
        /* flush sk 回调/调度器收发错误可能调度的 disconnect_work (幂等, 安全) */
        cancel_work_sync(&conn->disconnect_work);
        conn->in_use = false;
    }

    /* 断开 volume 连接 (与 filer 同模式: cancel work → disconnect → cancel work) */
    for (i = 0; i < g_pool.volume_count; i++) {
        struct powerfs_net_server_conn *conn = &g_pool.volumes[i];
        if (!conn->in_use)
            continue;

        cancel_delayed_work_sync(&conn->reconnect_work);
        powerfs_conn_disconnect_one(conn);
        cancel_work_sync(&conn->disconnect_work);
        conn->in_use = false;
    }

    /* v2: 释放 per-conn RX buffer (count 尚未清零, 用 count 循环).
     * 此时所有连接已 disconnect (kref==1, 调度器已放下 conn), 安全释放. */
    for (i = 0; i < g_pool.filer_count; i++) {
        pfs_conn_free_rxbuffers(&g_pool.filers[i]);
    }
    for (i = 0; i < g_pool.volume_count; i++) {
        pfs_conn_free_rxbuffers(&g_pool.volumes[i]);
    }

    g_pool.filer_count = 0;
    g_pool.volume_count = 0;

    /* v2: 所有连接已断开 (调度器不再访问 conn), 停止调度器线程.
     * 放在此处而非 pool_exit: 确保 disconnect_one 的 kref wait 不会因
     * 调度器线程已退出而死等 (调度器线程退出前会 put 完所有在飞引用). */
    powerfs_sched_exit();

    /* 销毁独立 workqueue (所有 work 已 cancel, 安全销毁).
     * destroy_workqueue 会 flush + 等待所有 pending work 完成. */
    if (g_pool.reconn_wq) {
        destroy_workqueue(g_pool.reconn_wq);
        g_pool.reconn_wq = NULL;
    }
}
EXPORT_SYMBOL_GPL(powerfs_conn_pool_exit);

