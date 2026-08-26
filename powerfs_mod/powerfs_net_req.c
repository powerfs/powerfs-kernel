/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_req.c - split from powerfs_net.c (mechanical refactor) */

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

/* ========== 请求对象生命周期 (Phase 1: 新架构) ========== */

/* powerfs_req_timeout_fn - 异步请求超时回调 (delayed_work, process context)
 *
 * 仅用于 callback != NULL 的异步请求. deadline 到期时触发, 检查请求是否
 * 仍在 pending 队列; 若是, 摘除并以 -ETIMEDOUT 完成.
 *
 * 与正常完成路径 (RX dispatch / disconnect) 通过 req_lock + list_empty
 * 互斥: 先摘除者胜, 后者看到 list_empty 直接退出, 避免双重完成.
 *
 * process context (system_wq): 可安全调用 callback (含 iput/kvmfree 等
 * 可能睡眠的操作), 与 RX 调度器线程解耦. */
void powerfs_req_timeout_fn(struct work_struct *work)
{
    struct delayed_work *dwork = to_delayed_work(work);
    struct powerfs_request *req =
        container_of(dwork, struct powerfs_request, timeout_work);
    struct powerfs_net_server_conn *conn = req->filer;
    bool won = false;

    if (!conn)
        return;

    /* 与 RX/disconnect 路径互斥: 用 RB_EMPTY_NODE 检查 req 是否仍在 req_tree.
     *
     * 之前用 list_empty(&req->list_node) 检查, 但 disconnect 路径将 req
     * splice 到 cancel_list 后 list_node 仍非空 (在 cancel_list 上),
     * 导致 timeout 误判为 "仍在 pending" → list_del_init + complete,
     * 与 disconnect 的 complete 形成双重完成 → ctx/req double-free →
     * SLUB freelist 自循环 → dentry 哈希链损坏 → __d_lookup_rcu 死循环.
     *
     * RB_EMPTY_NODE 在 powerfs_req_tree_remove 调用 RB_CLEAR_NODE 后返回 true,
     * 可靠反映 req 是否已被 disconnect/RX 从 req_tree 摘除. */
    spin_lock(&conn->req_lock);
    if (!RB_EMPTY_NODE(&req->rb_node)) {
        list_del_init(&req->list_node);
        powerfs_req_tree_remove(conn, req);
        req->error = -ETIMEDOUT;
        won = true;
    }
    spin_unlock(&conn->req_lock);

    if (!won)
        return;  /* 已被 RX/disconnect 完成, 无需重复 */

    /* 从 tx_queue 摘除 (若仍在) */
    spin_lock(&conn->tx_lock);
    if (!list_empty(&req->tx_list))
        list_del_init(&req->tx_list);
    spin_unlock(&conn->tx_lock);

    pr_warn("powerfs: async req seq=%u msg=0x%04x timed out on %s:%u\n",
            req->seq, req->msg_type, conn->addr, conn->port);

    /* timer_armed 由 powerfs_req_complete 清除; 此处调用 powerfs_req_complete
     * 会 cancel_delayed_work_sync(self) — 内核允许从 work fn 自身调用,
     * 返回 true 且不阻塞. 清除 timer_armed 避免冗余 cancel. */
    req->timer_armed = false;
    powerfs_req_complete(req);
}

/* powerfs_req_complete - 统一完成入口
 *
 * 取消异步超时定时器 (若已武装), 然后分发:
 *   - callback != NULL (异步): 调用 callback(req), 由 callback 负责
 *     powerfs_request_free
 *   - callback == NULL (同步): complete(&req->done), 唤醒等待线程
 *
 * 调用方 (RX dispatch / disconnect / timeout / submit) 必须在调用前
 * 将 req 从 pending_reqs + req_tree 摘除 (或确认已被摘除), 避免迟到
 * 响应误匹配. */
void powerfs_req_complete(struct powerfs_request *req)
{
    if (!req)
        return;

    if (req->timer_armed) {
        cancel_delayed_work_sync(&req->timeout_work);
        req->timer_armed = false;
    }

    if (req->callback)
        req->callback(req);
    else
        complete(&req->done);
}
EXPORT_SYMBOL_GPL(powerfs_req_complete);
EXPORT_SYMBOL_GPL(net_status_to_errno);

struct powerfs_request *powerfs_request_alloc(__u16 msg_type, gfp_t gfp)
{
    struct powerfs_request *req;

    req = kzalloc(sizeof(*req), gfp);
    if (!req)
        return NULL;

    req->msg_type = msg_type;
    req->shard_id = 0;
    req->error = 0;
    req->resp_status = 0;
    req->filer = NULL;
    init_completion(&req->done);
    INIT_LIST_HEAD(&req->list_node);
    INIT_LIST_HEAD(&req->tx_list);   /* v2: tx_queue 链表节点 */
    RB_CLEAR_NODE(&req->rb_node);
    kref_init(&req->kref);
    req->attempts = 0;
    req->needs_resend = false;
    req->callback = NULL;
    req->priv = NULL;
    req->timer_armed = false;

    return req;
}
EXPORT_SYMBOL_GPL(powerfs_request_alloc);

/* kref 释放回调: 引用计数归零时释放请求 */
void powerfs_request_release(struct kref *kref)
{
    struct powerfs_request *req = container_of(kref, struct powerfs_request, kref);

    /* 安全检查: 确保请求不在任何链表/红黑树上 */
    if (!list_empty(&req->list_node)) {
        pr_warn("powerfs: freeing req seq=%u still on pending list\n",
                req->seq);
        list_del_init(&req->list_node);
    }
    if (!list_empty(&req->tx_list)) {
        pr_warn("powerfs: freeing req seq=%u still on tx_queue\n", req->seq);
        list_del_init(&req->tx_list);
    }
    if (!RB_EMPTY_NODE(&req->rb_node)) {
        pr_warn("powerfs: freeing req seq=%u still in req_tree\n", req->seq);
        /* Note: 无法在此处 rb_erase, 因为不知道 root. 调用方应先移除. */
    }
    /* 防御: 异步请求若未正常完成就被释放, 取消超时定时器避免 UAF.
     * 正常路径 powerfs_req_complete 已清除 timer_armed, 此处为兜底. */
    if (req->timer_armed) {
        pr_warn("powerfs: freeing req seq=%u with timer still armed\n",
                req->seq);
        req->timer_armed = false;
        cancel_delayed_work_sync(&req->timeout_work);
    }

    kfree(req);
}
EXPORT_SYMBOL_GPL(powerfs_request_release);

void powerfs_request_free(struct powerfs_request *req)
{
    if (!req)
        return;

    kref_put(&req->kref, powerfs_request_release);
}
EXPORT_SYMBOL_GPL(powerfs_request_free);

/* === 红黑树辅助函数 (按 seq 组织请求, O(log n) 查找) ===
 *
 * req_tree 按 seq 排序, 用于 reply 匹配: 收到响应时按 seq 快速定位请求.
 * 与 pending_reqs (链表, 按发送顺序) 互补.
 */

/* 插入请求到 conn->req_tree (按 seq) */
void powerfs_req_tree_insert(struct powerfs_net_server_conn *conn,
                                     struct powerfs_request *req)
{
    struct rb_node **p = &conn->req_tree.rb_node;
    struct rb_node *parent = NULL;

    while (*p) {
        struct powerfs_request *entry;

        parent = *p;
        entry = rb_entry(parent, struct powerfs_request, rb_node);
        if (req->seq < entry->seq)
            p = &(*p)->rb_left;
        else
            p = &(*p)->rb_right;
    }
    rb_link_node(&req->rb_node, parent, p);
    rb_insert_color(&req->rb_node, &conn->req_tree);
}

/* 按 seq 查找请求 (用于 reply 匹配) */
struct powerfs_request * __maybe_unused
powerfs_req_tree_lookup(struct powerfs_net_server_conn *conn, __u32 seq)
{
    struct rb_node *n = conn->req_tree.rb_node;

    while (n) {
        struct powerfs_request *entry;

        entry = rb_entry(n, struct powerfs_request, rb_node);
        if (seq < entry->seq)
            n = n->rb_left;
        else if (seq > entry->seq)
            n = n->rb_right;
        else
            return entry;
    }
    return NULL;
}

/* 从 req_tree 移除请求 (若已在树中) */
void powerfs_req_tree_remove(struct powerfs_net_server_conn *conn,
                                     struct powerfs_request *req)
{
    if (!RB_EMPTY_NODE(&req->rb_node)) {
        rb_erase(&req->rb_node, &conn->req_tree);
        RB_CLEAR_NODE(&req->rb_node);
    }
}

void powerfs_shard_route_dispatch_pending(u64 shard_id)
{
    struct powerfs_shard_route *route = &g_pool.shard_route;
    struct powerfs_request *req, *tmp;
    LIST_HEAD(dispatch_list);
    int filer_idx;
    struct powerfs_net_server_conn *conn;
    int count = 0;

    if (shard_id >= route->shard_count)
        return;

    /* 1. 获取新 leader filer 索引 */
    spin_lock(&route->lock);
    filer_idx = route->entries[shard_id].leader_filer_idx;
    spin_unlock(&route->lock);

    if (filer_idx < 0 || filer_idx >= g_pool.filer_count)
        return;

    conn = &g_pool.filers[filer_idx];
    if (conn->state != CONN_CONNECTED)
        return;

    /* 2. 将所有待派发请求移到本地链表 */
    spin_lock(&route->entries[shard_id].req_lock);
    list_splice_init(&route->entries[shard_id].pending_reqs,
                     &dispatch_list);
    spin_unlock(&route->entries[shard_id].req_lock);

    if (list_empty(&dispatch_list))
        return;

    /* 统计数量 */
    list_for_each_entry(req, &dispatch_list, list_node)
        count++;

    pr_debug("powerfs: dispatching %d pending requests for shard %llu to filer %s:%u\n",
            count, (unsigned long long)shard_id, conn->addr, conn->port);

    /* 3. 重新派发每个请求.
     * 完整的重新提交需要在新的 filer 连接上执行 send+recv, 这要求持有
     * send_mutex 并管理 sock 引用. 由于派发在 route_update (通常是 recv
     * 线程) 上下文中调用, 直接同步 send 会导致嵌套锁/栈过深.
     * 简化实现: 标记 -EAGAIN 让调用方重试. */
    list_for_each_entry_safe(req, tmp, &dispatch_list, list_node) {
        list_del_init(&req->list_node);
        req->filer = conn;
        req->error = -EAGAIN;
        pr_debug("powerfs: dispatch req seq=%u msg_type=%u -> -EAGAIN (retry)\n",
                 req->seq, req->msg_type);
        powerfs_req_complete(req);
    }
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_dispatch_pending);

/*
 * powerfs_request_do_send - 在指定 filer 连接上发送请求并等待调度器完成响应
 *
 * v2 全异步流水线模型 (替代 v1 的 send_mutex + frame_send 直发):
 *   1. 检查 conn 状态 == CONNECTED
 *   2. 分配 seq, 将 req 挂到 conn->pending_reqs + req_tree (供 RX 匹配)
 *   3. 入 conn->tx_queue + pfs_tx_schedule (唤醒调度器发送)
 *   4. wait_for_completion_timeout(req->done) — 由调度器 RX 收到响应后 complete
 *   5. 超时: 摘除请求 → -ETIMEDOUT
 *   6. 调度器 RX 完成: req->resp_* 已由调度器填充, 返回 req->error
 *
 * 发送执行由调度器线程 (pfs_process_transmit, sk_write_space 触发) 异步完成,
 * 不在 do_send 内直接 kernel_sendmsg. 这使单连接可同时有多个 outstanding 请求
 * (流水线), do_send 不再持 send_mutex. sock 由调度器独占访问, do_send 不需
 * sock_users 引用.
 *
 * 返回: 0 成功, 负数错误码 (-errno)
 *   - -ENOTCONN: 连接断开 (调度器 send 失败或 sk_state_change 触发 disconnect),
 *                submit 应重试其他 filer
 *   - -ETIMEDOUT: 等待响应超时
 *   注: 本函数不调用 powerfs_req_complete; 由调度器/disconnect_one/FAULT 完成.
 *       send 失败和超时路径不 complete, submit 会 reinit_completion 后重试或返回.
 */
int powerfs_request_do_send(struct powerfs_request *req,
                                    struct powerfs_net_server_conn *conn)
{
    __u32 seq;
    int ret = 0;

    if (!req || !conn)
        return -EINVAL;

    /* 快速检查连接状态 */
    if (conn->state != CONN_CONNECTED)
        return -ENOTCONN;

    /* 在 state_lock 下确认状态, 防止 disconnect_one 并发关闭.
     * v2 不再获取 sock 本地引用 (sock 由调度器独占访问, do_send 不碰 sock). */
    spin_lock(&conn->state_lock);
    if (conn->state != CONN_CONNECTED || !conn->sock) {
        spin_unlock(&conn->state_lock);
        pr_debug("powerfs: do_send: filer %s:%u not connected\n",
                 conn->addr, conn->port);
        return -ENOTCONN;
    }
    spin_unlock(&conn->state_lock);

    /* 分配 seq 并将请求挂到 pending_reqs + req_tree.
     * 调度器可能在 send 完成前就收到响应 (流水线), 故先入树再入 tx_queue. */
    seq = atomic_inc_return(&conn->seq_counter);
    req->seq = seq;
    req->ts_submit = ktime_get_ns();
    req->ts_sent = 0;          /* 重置: 防止 redirect 重试读到上一轮值 */
    req->ts_recv = 0;

    spin_lock(&conn->req_lock);
    /* 若已在链表上 (异常重入/重发), 先移除 */
    if (!list_empty(&req->list_node)) {
        list_del_init(&req->list_node);
        powerfs_req_tree_remove(conn, req);
    }
    list_add_tail(&req->list_node, &conn->pending_reqs);
    powerfs_req_tree_insert(conn, req);
    spin_unlock(&conn->req_lock);

    /* v2: 入 tx_queue + 投递到调度器 (替代 v1 的 send_mutex + frame_send 直发).
     * 调度器 pfs_process_transmit 从 tx_queue 取 req, kernel_sendmsg 发送.
     * EAGAIN 时重挂回 tx_queue 等 sk_write_space 回调. */
    spin_lock(&conn->tx_lock);
    /* 若仍在 tx_queue (重发场景), 先移除再添加到尾部 */
    if (!list_empty(&req->tx_list))
        list_del_init(&req->tx_list);
    list_add_tail(&req->tx_list, &conn->tx_queue);
    spin_unlock(&conn->tx_lock);

    pfs_tx_schedule(conn);

    /* === 异步模式: callback != NULL 时不等待, 立即返回 ===
     *
     * page writeback 路径用异步提交避免 workqueue 线程阻塞在网络等待上
     * (旧同步路径 wait_for_completion 可阻塞 30s, 多个 needle 串行 →
     * workqueue lockup 598s).
     *
     * 异步请求的完成由三条路径触发, 均经 powerfs_req_complete 分发到 callback:
     *   a. 正常: 调度器 RX 收到响应 (pfs_rx_dispatch 摘除 req 后调用)
     *   b. 断连: disconnect_one / FAULT 以 -ENOTCONN 取消
     *   c. 超时: delayed_work (timeout_work) 到期以 -ETIMEDOUT 完成
     *
     * 超时定时器在 deadline 到期时触发 (process context, system_wq),
     * 与正常完成路径通过 req_lock + list_empty 互斥, 先摘除者胜.
     * 必须有超时兜底, 否则 page 永久滞留 PageWriteback → hung task. */
    if (req->callback) {
        if (req->deadline) {
            INIT_DELAYED_WORK(&req->timeout_work, powerfs_req_timeout_fn);
            req->timer_armed = true;
            queue_delayed_work(system_wq, &req->timeout_work,
                               req->deadline - jiffies);
        }
        return 0;
    }

    /* Phase 2: 等待调度器 complete (异步收发, 流水线).
     *
     * 改进 (vs Phase 1):
     *   1. wait_for_completion_killable_timeout: SIGKILL 可中断, 避免 D-state
     *   2. deadline-based 超时: 尊重调用方传入的 timeout_ms, 不再硬编码 30s
     *   3. 上限仍为 30s (RECV_TIMEOUT * 3): 防止 deadline 过大
     *
     * 完成路径:
     *   a. 正常: 调度器 RX 收到响应 → complete
     *   b. 断连: disconnect_one 以 -ENOTCONN complete, 主线程在 submit 中重试
     *   c. FAULT: set_state(FAULT) 以 -ENOTCONN complete (重连彻底失败)
     *   d. 超时: deadline 到期 → -ETIMEDOUT
     *   e. 信号: SIGKILL 中断 → -EINTR */
    {
        unsigned long do_send_deadline;
        long wr;

        if (req->deadline)
            do_send_deadline = min(req->deadline,
                jiffies + msecs_to_jiffies(POWERFS_NET_RECV_TIMEOUT * 3));
        else
            do_send_deadline = jiffies +
                msecs_to_jiffies(POWERFS_NET_RECV_TIMEOUT * 3);

        wr = wait_for_completion_killable_timeout(&req->done,
                    do_send_deadline - jiffies);

        /* 计时日志: 定位慢请求根因 (分段计时)
         *   tx_wait = ts_sent - ts_submit  (TX 队列等待 + 调度器唤醒)
         *   net     = ts_recv - ts_sent    (网络往返 + Filer 处理)
         *   rx_disp = now    - ts_recv     (RX dispatch + complete 唤醒) */
        if (req->ts_submit) {
            u64 now_ns = ktime_get_ns();
            u64 dur_us = div_u64(now_ns - req->ts_submit, 1000);
            if (dur_us > 100000) {  /* >100ms 的请求打 info 日志 */
                u64 tx_wait_us = req->ts_sent ? div_u64(req->ts_sent - req->ts_submit, 1000) : 0;
                u64 net_us = (req->ts_sent && req->ts_recv) ? div_u64(req->ts_recv - req->ts_sent, 1000) : 0;
                u64 rx_disp_us = req->ts_recv ? div_u64(now_ns - req->ts_recv, 1000) : 0;
                pr_info("powerfs: SLOW_REQ seq=%u msg=0x%04x dur=%llums tx_wait=%llums net=%llums rx_disp=%llums conn=%s:%u\n",
                        seq, req->msg_type, div_u64(dur_us, 1000),
                        div_u64(tx_wait_us, 1000), div_u64(net_us, 1000),
                        div_u64(rx_disp_us, 1000), conn->addr, conn->port);
            }
        }

        if (wr < 0) {
            /* 被信号中断 (SIGKILL): 从队列摘除, 返回 -EINTR.
             * 与超时同样的清理逻辑, 防止迟到响应误匹配. */
            pr_warn("powerfs: req seq=%u msg_type=%u interrupted by signal on filer %s:%u\n",
                    seq, req->msg_type, conn->addr, conn->port);
            spin_lock(&conn->req_lock);
            if (!list_empty(&req->list_node)) {
                list_del_init(&req->list_node);
                powerfs_req_tree_remove(conn, req);
                req->error = -EINTR;
            }
            spin_unlock(&conn->req_lock);
            spin_lock(&conn->tx_lock);
            if (!list_empty(&req->tx_list))
                list_del_init(&req->tx_list);
            spin_unlock(&conn->tx_lock);
            return req->error;
        }

        if (wr == 0) {
            /* 超时: 请求可能仍在 pending_reqs/tx_queue (无人 complete).
             * 摘除防止迟到响应误匹配. 若已被 complete (竞态: 刚超时就被
             * 调度器/disconnect 完成), list 为空, 返回完成者设置的 error. */
            pr_warn("powerfs: req seq=%u msg_type=%u timed out on filer %s:%u\n",
                    seq, req->msg_type, conn->addr, conn->port);
            spin_lock(&conn->req_lock);
            if (!list_empty(&req->list_node)) {
                list_del_init(&req->list_node);
                powerfs_req_tree_remove(conn, req);
                req->error = -ETIMEDOUT;
            }
            spin_unlock(&conn->req_lock);
            /* 从 tx_queue 摘除 (若仍在) */
            spin_lock(&conn->tx_lock);
            if (!list_empty(&req->tx_list))
                list_del_init(&req->tx_list);
            spin_unlock(&conn->tx_lock);

            /* 半开连接检测: 连续超时达阈值 → 强制断连重连.
             * 问题: TCP 半开时 (filer 侧关闭但 FIN 未达内核, 或网络静默丢包),
             * sk_state_change 不触发, 连接不会被重置, 请求持续超时.
             * 方案: 连续 N 次超时后调用 disconnect_one, 触发 reconnect_work.
             * disconnect_one 会以 -ENOTCONN complete 所有 pending 请求,
             * 上层 submit 重试时连接已重建. */
            if (atomic_inc_return(&conn->consecutive_timeouts) >=
                    POWERFS_NET_TIMEOUT_RECONNECT_THRESHOLD) {
                pr_warn("powerfs: conn %s:%u half-open detected (%d consecutive timeouts), forcing reconnect\n",
                        conn->addr, conn->port,
                        atomic_read(&conn->consecutive_timeouts));
                /* 避免重复触发: disconnect_one 内部会检查 state==CONN_CONNECTED */
                if (conn->state == CONN_CONNECTED)
                    queue_work(g_pool.reconn_wq, &conn->disconnect_work);
            }
            return req->error;
        }
    }

    /* 调度器已 complete (或 disconnect_one/FAULT 以 -ENOTCONN complete).
     * req 已被完成者从 list/tree 摘除, resp_* 已由调度器 RX 填充.
     * 再次检查摘除 (防御: 若完成者未摘除, 此处补摘). */
    spin_lock(&conn->req_lock);
    if (!list_empty(&req->list_node)) {
        list_del_init(&req->list_node);
        powerfs_req_tree_remove(conn, req);
    }
    spin_unlock(&conn->req_lock);
    /* tx_queue 侧也防御性摘除 (发送成功后 req 已从 tx_queue 摘除, 但 EAGAIN
     * 重挂场景可能仍在). 若已摘除, list_empty 为 true, no-op. */
    spin_lock(&conn->tx_lock);
    if (!list_empty(&req->tx_list))
        list_del_init(&req->tx_list);
    spin_unlock(&conn->tx_lock);

    /* 请求成功完成 → 重置半开连接检测计数器 */
    if (!req->error)
        atomic_set(&conn->consecutive_timeouts, 0);

    pr_debug("powerfs: request completed seq=%u msg_type=%u status=%u body=%zu data=%zu\n",
             seq, req->msg_type, req->resp_status,
             req->resp_body_len, req->resp_data_len);

    return ret ? ret : req->error;
}

/*
 * powerfs_request_submit - 提交请求并通过连接池发送 (主入口)
 *
 * 流程:
 *   1. 根据 shard_id 查路由状态
 *   2. VALID: 直接用 leader filer 连接
 *   3. CHECKING: 尝试其他 filer, 或将请求挂到 shard pending 队列
 *   4. UNKNOWN: 挂到 shard pending 队列等待
 *   5. 发送+接收, 处理 REDIRECT (更新路由表, 重试)
 *   6. 断连: 请求已在 filer->pending_reqs, 自动被取消
 *
 * 返回 req->error (0=成功, <0=错误)
 */
int powerfs_request_submit(struct powerfs_request *req)
{
    enum powerfs_shard_route_state route_state;
    struct powerfs_net_server_conn *conn;
    struct powerfs_net_server_conn *last_tried_conn = NULL;

    /* FIX (align with FUSE send_coherence_msg):
     *
     * PROBLEM with only last_tried_conn:
     *   After a self-redirect on node S3 (S3 says leader is S3 → fail),
     *   we rotate to S2 (Follower). S2 returns REDIRECT "leader is S3".
     *   At that point last_tried_conn == S2 (the current node, just set on
     *   entry to do_send). The cross-filer loop check
     *     "leader_addr == last_tried_conn" fails because leader=S3 != S2.
     *   So we happily update shard_router to VALID→S3, next iteration
     *   immediately returns to S3, self-redirect again → ping-pong
     *   S3→S2→S3→S2→deadline exceeded.
     *
     * SOLUTION (FUSE MetaShardClient model, ported to C):
     *   Maintain a per-request blacklist of nodes that have *already*
     *   responded with self-redirect during this request. Two new fields:
     *     bad_self_addr / bad_self_port
     *   Then three rules:
     *     (A) ROUTE_CHECKING picker skips BOTH last_tried_conn AND
     *         any filer matching (bad_self_addr, bad_self_port) so we
     *         never waste a round-trip probing a node proven stale.
     *     (B) When a non-self REDIRECT arrives whose target == the
     *         blacklisted self-redirect node, treat it as a "stale
     *         redirect" — do NOT poke shard_router at that node, just
     *         apply x2 backoff and keep rotating. In FUSE this is the
     *         `if new_addr == target_addr { rotate, don't update router }`
     *         branch, generalised to catch redirects THROUGH a follower.
     *     (C) Cross-filer A↔B loop detection still applies when neither
     *         endpoint is a known self-redirect target.
     */
    /* ================================================================
     * FIX V6: Multi-node self-redirect BLACKLIST ARRAY (was single node).
     *
     * Previous single-node bug proven by dmesg:
     *   Attempt 0: S32 self-redirect  →  bad_self = S32  (covers old)
     *   Attempt 1: S31 self-redirect  →  bad_self = S31  (OVERWRITES S32!)
     *   Attempt 2: picker skips S31,  picks S32 again  →  self-redirect
     *               →  bad_self = S32  (overwrites S31 again)
     *   Result: S32 ↔ S31 infinite ping-pong, S33 (real leader) never
     *           probed, 15s deadline exceeded despite live leader present.
     *
     * New design: fixed array of up to POWERFS_MAX_FILERS distinct
     * blacklisted self-redirect targets.  Duplicate add is no-op.  The
     * ROUTE_CHECKING picker walks the entire list for every candidate,
     * so once a filer self-redirects it is skipped for the rest of the
     * request until the global TTL expiry below clears all entries.
     * With 3 filers this guarantees we rotate through ALL of them —
     * worst case 3 attempts, we WILL reach the real leader.  */
    #define POWERFS_MAX_BLACKLISTED_SELF  8
    char  bad_self_addrs[POWERFS_MAX_BLACKLISTED_SELF][64];
    __u16 bad_self_ports[POWERFS_MAX_BLACKLISTED_SELF];
    int   bad_self_cnt = 0;

    /* FIX V3 (kept, applied to the whole blacklist array):
     * TTL counter: N subsequent STALE-redirects pointing at any
     * blacklisted node → clear the whole blacklist.  Self-redirect
     * state is TRANSIENT (lasts ~100 ms during Raft election settle),
     * so after enough follower-consensus STALE hits we must retry all
     * nodes — otherwise we permanently blacklist the real leader.
     *
     * Threshold = 2: in a 3-Filer cluster, 2 independent DIFFERENT
     * follower confirmations that X is leader is sufficient proof
     * (Raft quorum = 2).  Previously 4 was overly conservative and
     * caused deadline exceed during otherwise-normal redirect chains.
     */
    int   bad_self_stale_count = 0;
    #define BAD_SELF_MAX_STALE_ROUNDS  2

    int filer_idx;
    int attempt;
    int ret;
    unsigned long deadline;
    long wait_ret;
    int eagain_retries = 0;
#define POWERFS_REQ_MAX_EAGAIN_RETRIES  8

    if (!req)
        return -EINVAL;

    /* 设置超时 deadline */
    if (req->deadline)
        deadline = req->deadline;
    else
        deadline = jiffies + msecs_to_jiffies(POWERFS_NET_RECONNECT_WAIT_TIMEOUT_MS);

    /*
     * 重试循环: 基于 deadline 控制总时长, 不再固定 2 次.
     * 每轮:
     *   1. 查路由状态, 选 filer
     *   2. 无 filer -> 挂 pending 队列等待派发
     *      - 派发回调设置 -EAGAIN (简化实现), 收到后自动重试 (不退出循环)
     *      - 超时未派发 -> -ETIMEDOUT
     *   3. 有 filer -> do_send
     *      - -ENOTCONN: 等重连后重试
     *      - REDIRECT:
     *        a. 目标 != 当前 filer: 更新路由, 重试 (走新 leader)
     *        b. 目标 == 当前 filer (self-redirect, 选举中):
     *           切换 shard 到 ROUTE_CHECKING, 尝试其他 filer (避免循环)
     *      - 其他错误: 返回
     *   4. 成功: 返回
     */
    for (attempt = 0; ; attempt++) {
        /* 检查 deadline */
        if (time_after(jiffies, deadline)) {
            pr_warn("powerfs: req msg_type=%u shard=%llu deadline exceeded\n",
                    req->msg_type, (unsigned long long)req->shard_id);
            req->error = -ETIMEDOUT;
            powerfs_req_complete(req);
            return -ETIMEDOUT;
        }

        route_state = powerfs_shard_route_get_state(req->shard_id);
        conn = NULL;

        switch (route_state) {
        case ROUTE_VALID:
            /* leader 已知: 获取 filer 连接 */
            conn = powerfs_conn_get_filer_for_shard(req->shard_id);
            if (conn && conn->state != CONN_CONNECTED) {
                /* leader filer 断连, 触发路由检查 */
                filer_idx = powerfs_conn_get_filer_idx(conn);
                if (filer_idx >= 0)
                    powerfs_shard_route_on_filer_disconnect(filer_idx);
                conn = NULL;
            }
            /* FIX V2+V6 extension: ROUTE_VALID must ALSO honour
             * the per-request multi-node bad_self blacklist ARRAY.
             * Otherwise:
             *   S3 self-redirect → blacklist S3, state=CHECKING
             *   S2 REDIRECT → powerfs_shard_route_update sets VALID→S3
             *   Next iteration ROUTE_VALID returns S3 again, bypassing
             *   the CHECKING picker's blacklist filter → ping-pong loop.
             *
             * V6: iterate the full blacklist array, not single node. */
            if (conn && bad_self_cnt > 0) {
                int k;
                bool bl_hit = false;
                for (k = 0; k < bad_self_cnt; k++) {
                    if (strcmp(conn->addr, bad_self_addrs[k]) == 0 &&
                        conn->port == bad_self_ports[k]) {
                        bl_hit = true;
                        break;
                    }
                }
                if (bl_hit) {
                    pr_warn("powerfs: ROUTE_VALID conn %s:%u is in "
                            "bad_self blacklist (cnt=%d); downgrading to "
                            "CHECKING (attempt %d, shard=%llu)\n",
                            conn->addr, conn->port, bad_self_cnt,
                            attempt,
                            (unsigned long long)req->shard_id);
                    if (req->shard_id < POWERFS_MAX_SHARDS) {
                        struct powerfs_shard_route_entry *entry;
                        entry = &g_pool.shard_route.entries[req->shard_id];
                        spin_lock(&g_pool.shard_route.lock);
                        if (entry->state == ROUTE_VALID)
                            entry->state = ROUTE_CHECKING;
                        spin_unlock(&g_pool.shard_route.lock);
                    }
                    conn = NULL;
                }
            }
            break;

        case ROUTE_CHECKING:
        case ROUTE_UNKNOWN:
            /*
             * 寻找可用 filer, 优先跳过 last_tried_conn 和 self-redirect 黑名单数组.
             *
             * ROUTE_CHECKING: leader 可能已变, round-robin 尝试.
             * ROUTE_UNKNOWN: 无 leader 信息, 尝试任意已连接 filer.
             *
             * 两者的 filer 选择策略相同:
             *   第一轮: 跳过 last_tried_conn + 全量黑名单数组 (V6 multi-node)
             *           避免在 proven-stale 节点上浪费 RTT.
             *   第二轮: 若无其他选择, 允许重试同一个 (避免 livelock 当
             *           3 个 filer 中有 3 个已列入黑名单 → TTL will clear).
             */
            if (g_pool.filer_count > 0) {
                int i;
                /* 第一轮: 跳过 last_tried_conn + 多节点黑名单数组 */
                for (i = 0; i < g_pool.filer_count; i++) {
                    bool skip = false;
                    int k;
                    if (!g_pool.filers[i].in_use ||
                        g_pool.filers[i].state != CONN_CONNECTED)
                        continue;
                    if (&g_pool.filers[i] == last_tried_conn)
                        skip = true;
                    /* V6: linear scan of blacklist array */
                    for (k = 0; k < bad_self_cnt && !skip; k++) {
                        if (strcmp(g_pool.filers[i].addr,
                                   bad_self_addrs[k]) == 0 &&
                            g_pool.filers[i].port == bad_self_ports[k])
                            skip = true;
                    }
                    if (!skip) {
                        conn = &g_pool.filers[i];
                        break;
                    }
                }
                /* 第二轮: 若无其他选择, 允许任何已连接 filer */
                if (!conn) {
                    for (i = 0; i < g_pool.filer_count; i++) {
                        if (g_pool.filers[i].in_use &&
                            g_pool.filers[i].state == CONN_CONNECTED) {
                            conn = &g_pool.filers[i];
                            break;
                        }
                    }
                }
            }
            break;
        }

        if (!conn) {
            /* 无可用 filer: 将请求挂到 shard pending 队列等待派发 */
            struct powerfs_shard_route_entry *entry;

            if (req->shard_id >= POWERFS_MAX_SHARDS) {
                req->error = -ENOTCONN;
                powerfs_req_complete(req);
                return -ENOTCONN;
            }

            entry = &g_pool.shard_route.entries[req->shard_id];
            spin_lock(&entry->req_lock);
            list_add_tail(&req->list_node, &entry->pending_reqs);
            spin_unlock(&entry->req_lock);

            pr_debug("powerfs: no filer available for shard %llu, queueing req msg_type=%u\n",
                    (unsigned long long)req->shard_id, req->msg_type);

            /* Phase 2: 等待派发 (route_update -> dispatch_pending) 或超时.
             * 使用 killable wait: SIGKILL 可中断, 避免 D-state. */
            reinit_completion(&req->done);
            wait_ret = wait_for_completion_killable_timeout(&req->done,
                deadline - jiffies);

            /* 从 pending 队列移除 (如果还在) */
            spin_lock(&entry->req_lock);
            if (!list_empty(&req->list_node))
                list_del_init(&req->list_node);
            spin_unlock(&entry->req_lock);

            if (wait_ret < 0) {
                /* 被信号中断: 返回 -EINTR, 不再重试 */
                pr_warn("powerfs: req msg_type=%u shard=%llu interrupted by signal\n",
                        req->msg_type, (unsigned long long)req->shard_id);
                req->error = -EINTR;
                return -EINTR;
            }

            if (wait_ret == 0 && req->error == 0) {
                /* 超时且未被派发/取消 */
                pr_warn("powerfs: req msg_type=%u shard=%llu timed out waiting for filer\n",
                        req->msg_type, (unsigned long long)req->shard_id);
                req->error = -ETIMEDOUT;
                return -ETIMEDOUT;
            }

            /*
             * dispatch_pending 的简化实现设置 -EAGAIN 让调用方重试.
             * 此处自动重试 (不退出循环), 限制重试次数避免无限循环.
             * 正常情况下, route_update 已将路由设为新 leader,
             * 重试时 route_state == ROUTE_VALID, 走 do_send 到新 filer.
             */
            if (req->error == -EAGAIN) {
                eagain_retries++;
                if (eagain_retries > POWERFS_REQ_MAX_EAGAIN_RETRIES) {
                    pr_warn("powerfs: req msg_type=%u shard=%llu exhausted %d EAGAIN retries\n",
                            req->msg_type, (unsigned long long)req->shard_id,
                            POWERFS_REQ_MAX_EAGAIN_RETRIES);
                    return -EAGAIN;
                }
                pr_debug("powerfs: req msg_type=%u retry after EAGAIN (%d/%d)\n",
                         req->msg_type, eagain_retries, POWERFS_REQ_MAX_EAGAIN_RETRIES);
                req->error = 0;
                continue;
            }

            return req->error;
        }

        /* 有 filer: 同步 send+recv */
        last_tried_conn = conn;
        req->filer = conn;

        /* Phase 2: 流控 record_start (发送前递增 active 计数) */
        {
            int flow_idx = pfs_conn_flow_idx(conn);
            powerfs_flow_record_start(flow_idx,
                                      req->req_body_len + req->req_data_len);
        }

        ret = powerfs_request_do_send(req, conn);

        /* Phase 2: do_send 失败 (未走 RX 路径) 时补 record_complete,
         * 避免 active_reqs 计数泄漏. 成功时 pfs_rx_dispatch 已调 record_complete. */
        if (ret != 0) {
            powerfs_flow_record_complete(pfs_conn_flow_idx(conn),
                                         0, 0, true);
        }

        if (ret == -ENOTCONN) {
            /* 连接断开: route 已降级 (disconnect_one 设了 RECONNECTING,
             * 触发 shard_route_on_filer_disconnect → CHECKING).
             * 主线程自己重试: 重新查路由, 选其他 filer 或等重连.
             * 不依赖重连线程操作 request, 避免竞态. */
            if (atomic_read(&g_pool.stopping)) {
                req->error = -ENOTCONN;
                return -ENOTCONN;
            }
            if (time_after(jiffies, deadline)) {
                pr_warn("powerfs: req msg_type=%u timed out after %d retries (deadline)\n",
                        req->msg_type, attempt);
                req->error = -ETIMEDOUT;
                return -ETIMEDOUT;
            }
            /* brief backoff: 等 failover/reconnect, 避免忙循环 */
            msleep(100);
            reinit_completion(&req->done);
            req->error = 0;
            continue;  /* 重新 route_select, 选其他 filer */
        }

        if (ret < 0) {
            /* 其他错误 (如 -ETIMEDOUT): do_send 已设置 req->error, 直接返回.
             * 注: 新设计中 do_send 不调用 complete (由 RX/disconnect 完成),
             * 此处不等待 req->done, 直接返回 req->error. */
            return req->error;
        }

        /* 处理 REDIRECT: 解析 leader 地址, 更新路由, 重试 */
        if (req->resp_status == POWERFS_NET_STATUS_ERR_REDIRECT) {
            char leader_addr[64];
            __u16 leader_port;
            struct powerfs_net_server_conn *leader_conn;
            bool self_redirect = false;

            if (powerfs_net_parse_redirect(req->resp_body, req->resp_body_len,
                                            leader_addr, sizeof(leader_addr),
                                            &leader_port) == 0) {
                /* FIX (T1.6 alignment): 检测 self-redirect 必须 FIRST.
                 *
                 * 之前的代码先做 cross-filer loop 检测, 后做 self-redirect
                 * 检测. 当 last_tried_conn == conn == leader 三者同为 S1
                 * 时 (例如 S1 选举进行中返回 self-redirect), 错误地先命中
                 * "last_tried_conn == leader" 条件, 误报 "redirect loop"
                 * 并立即 EAGAIN, 而不走 self-redirect 的指数退避重试路径.
                 *
                 * Self-redirect (leader == conn) 是正常的选举中间态,
                 * 必须用退避重试处理; 只有 leader != conn 但又绕回
                 * last_tried_conn 时才是真正的 A↔B cross-filer loop. */
                if (strcmp(leader_addr, conn->addr) == 0 &&
                    leader_port == conn->port) {
                    self_redirect = true;
                    pr_debug("powerfs: self-redirect detected on filer %s:%u (election in progress), attempt=%d\n",
                            conn->addr, conn->port, attempt);
                }

                if (self_redirect) {
                    unsigned long backoff;
                    int dup_idx;
                    bool already_blacklisted = false;
                    /* FIX V7: After Filer FINAL FIX V2, *every* non-leader
                     * returns SELF-redirect (never cross-filer redirect
                     * to another node).  This means Rule B (stale-redirect
                     * hit → TTL clear) is NEVER reached because the else
                     * branch below (strcmp leader != conn) requires a
                     * CROSS-filer redirect.  Result: once all 3 filers are
                     * blacklisted (attempt 2), the picker's 2nd fallthrough
                     * round keeps picking the same filer forever, and
                     * bad_self_stale_count never increments → deadline.
                     *
                     * V7 fix: track duplicate self-redirect rounds via the
                     * SAME bad_self_stale_count counter.  After N dup
                     * rounds → TTL-clear entire blacklist.  Same quorum
                     * logic as Rule B (2 rounds = election settled). */
                    bool v7_ttl_cleared = false;

                    /* ========= Rule (3) V6: multi-node blacklist ADD (dedup) ====
                     * Record (conn->addr, port) into per-request BAD_SELF
                     * ARRAY if not already present.  Unlike the old single-
                     * node code we NEVER overwrite entries — two followers
                     * self-redirecting in sequence must BOTH stay skipped
                     * so the picker can reach filer #3 (real leader). */
                    for (dup_idx = 0; dup_idx < bad_self_cnt; dup_idx++) {
                        if (strcmp(bad_self_addrs[dup_idx],
                                   conn->addr) == 0 &&
                            bad_self_ports[dup_idx] == conn->port) {
                            already_blacklisted = true;
                            break;
                        }
                    }
                    if (!already_blacklisted &&
                        bad_self_cnt < POWERFS_MAX_BLACKLISTED_SELF) {
                        strncpy(bad_self_addrs[bad_self_cnt], conn->addr,
                                sizeof(bad_self_addrs[0]) - 1);
                        bad_self_addrs[bad_self_cnt]
                                     [sizeof(bad_self_addrs[0]) - 1] = '\0';
                        bad_self_ports[bad_self_cnt] = conn->port;
                        bad_self_cnt++;
                    }

                    /* ==== V7: dup self-redirect triggers TTL (same
                     * threshold as Rule B, 2 rounds).  This replaces
                     * Rule B's trigger which is now unreachable under
                     * Filer FINAL FIX V2 (all-SELF redirect model). */
                    if (already_blacklisted) {
                        bad_self_stale_count++;
                        if (bad_self_stale_count >= BAD_SELF_MAX_STALE_ROUNDS) {
                            pr_warn("powerfs: V7 SELF-redirect TTL after "
                                    "%d dup-rounds: all filers returned "
                                    "self-redirect (cnt=%d); CLEARING "
                                    "entire blacklist to retry leader "
                                    "(attempt %d, shard=%llu)\n",
                                    bad_self_stale_count,
                                    bad_self_cnt,
                                    attempt,
                                    (unsigned long long)req->shard_id);
                            bad_self_cnt = 0;
                            bad_self_stale_count = 0;
                            v7_ttl_cleared = true;
                        }
                    }

                    /* Force CHECKING state so picker rotates *past*
                     * this filer (without disconnecting it). */
                    if (req->shard_id < POWERFS_MAX_SHARDS) {
                        struct powerfs_shard_route_entry *entry;
                        entry = &g_pool.shard_route.entries[req->shard_id];
                        spin_lock(&g_pool.shard_route.lock);
                        if (entry->state == ROUTE_VALID)
                            entry->state = ROUTE_CHECKING;
                        spin_unlock(&g_pool.shard_route.lock);
                    }

                    /* PERF OPT: After Filer FINAL FIX V2, followers
                     * *stably* return self-redirect (not transient election
                     * state).  Old 400/800ms backoff was for Raft election
                     * settle (~100-500 ms).  Current case: 2 followers
                     * always return self, 1 leader always accepts.  So
                     * minimise wait: 20/40ms, cap 50ms.  Worst-case 3-node
                     * probe = 70 ms TOTAL overhead per fresh request, vs
                     * 1200 ms before. */
                    if (v7_ttl_cleared) {
                        backoff = 0;
                    } else {
                        backoff = min(20ul * (1UL << min(attempt, 1)),
                                      50ul);
                    }

                    pr_warn("powerfs: self-redirect on filer %s:%u, "
                            "x2 backoff=%lu ms, +blacklist (cnt=%d%s), "
                            "rotating next (attempt %d, shard=%llu)\n",
                            conn->addr, conn->port, backoff, bad_self_cnt,
                            already_blacklisted ? " (dup)" : "",
                            attempt,
                            (unsigned long long)req->shard_id);

                    reinit_completion(&req->done);
                    req->error = 0;
                    if (backoff > 0)
                        msleep(backoff);
                    continue;
                }

                /* ======= Stale-redirect guard (Rule B V6: multi-node array) =====
                 * Follower redirects us to a node we ALREADY know is a stale
                 * self-redirect target (present in blacklist array). Don't
                 * update shard_router; keep rotating with x2 backoff.
                 *
                 * FIX V3: TTL the whole blacklist.  Self-redirect is a
                 * TRANSIENT election-in-progress state (~100 ms).  After N
                 * stale-rounds where followers independently confirm X is
                 * leader, the election must have settled — clear ALL nodes
                 * and retry, otherwise we permanently blacklist the real
                 * leader and spin through followers until deadline. */
                {
                    int bl_hit_idx = -1;
                    int k;
                    for (k = 0; k < bad_self_cnt; k++) {
                        if (strcmp(leader_addr, bad_self_addrs[k]) == 0 &&
                            leader_port == bad_self_ports[k]) {
                            bl_hit_idx = k;
                            break;
                        }
                    }
                    if (bl_hit_idx >= 0) {
                        unsigned long backoff;
                        bool skip_sleep_v4 = false;

                        bad_self_stale_count++;
                        if (bad_self_stale_count >= BAD_SELF_MAX_STALE_ROUNDS) {
                            pr_warn("powerfs: BAD_SELF TTL expired after %d "
                                    "stale-rounds: all followers point to "
                                    "%s:%u, CLEARING ENTIRE blacklist array "
                                    "(was %d nodes) to retry proven leader "
                                    "(attempt %d, shard=%llu)\n",
                                    bad_self_stale_count,
                                    leader_addr, leader_port,
                                    bad_self_cnt,
                                    attempt,
                                    (unsigned long long)req->shard_id);
                            bad_self_cnt = 0;       /* V6: clear entire array */
                            bad_self_stale_count = 0;
                            /* FIX V4: Skip the ~1.6s stall after TTL since
                             * follower consensus already means election is
                             * settled.  Also force VALID route to the
                             * consensus leader — otherwise picker still
                             * round-robins through followers. */
                            skip_sleep_v4 = true;
                            {
                                struct powerfs_net_server_conn *consensus_ldr;
                                consensus_ldr = powerfs_conn_find_filer(
                                                    leader_addr, leader_port);
                                if (consensus_ldr) {
                                    int cidx = powerfs_conn_get_filer_idx(
                                                          consensus_ldr);
                                    if (cidx >= 0 &&
                                        req->shard_id < POWERFS_MAX_SHARDS) {
                                        powerfs_shard_route_update(
                                                           req->shard_id, cidx);
                                        pr_warn("powerfs: post-TTL force "
                                                "route VALID to consensus "
                                                "leader %s:%u (idx=%d)\n",
                                                consensus_ldr->addr,
                                                consensus_ldr->port, cidx);
                                    }
                                }
                            }
                        }

                    if (req->shard_id < POWERFS_MAX_SHARDS && !skip_sleep_v4) {
                        struct powerfs_shard_route_entry *entry;
                        entry = &g_pool.shard_route.entries[req->shard_id];
                        spin_lock(&g_pool.shard_route.lock);
                        if (entry->state == ROUTE_VALID)
                            entry->state = ROUTE_CHECKING;
                        spin_unlock(&g_pool.shard_route.lock);
                    }

                    /* PERF OPT: same rationale as self-redirect branch —
                     * stable redirect pattern, not transient election.
                     * 20/40ms cap 50ms instead of 400/800/1600ms. */
                    backoff = min(20ul * (1UL << min(attempt, 1)),
                                  50ul);

                    if (!skip_sleep_v4) {
                        pr_warn("powerfs: STALE-redirect: follower %s:%u "
                                "-> blacklisted self-target %s:%u; keeping "
                                "CHECKING, x2 backoff=%lu ms, rotate next "
                                "(attempt %d, shard=%llu, stale_cnt=%d/%d)\n",
                                conn->addr, conn->port,
                                leader_addr, leader_port,
                                backoff, attempt,
                                (unsigned long long)req->shard_id,
                                bad_self_stale_count,
                                BAD_SELF_MAX_STALE_ROUNDS);
                    } else {
                        pr_warn("powerfs: STALE-redirect (TTL-cleared): "
                                "SKIP backoff=%lu ms, go retry "
                                "consensus-leader immediately "
                                "(attempt %d, shard=%llu)\n",
                                backoff, attempt,
                                (unsigned long long)req->shard_id);
                    }

                    reinit_completion(&req->done);
                    req->error = 0;
                    if (!skip_sleep_v4)
                        msleep(backoff);
                    continue;
                    }
                }

                /* FIX V2-V6: Diagnostics when bad_self array non-empty.
                 * Log all blacklisted nodes to help diagnose why Rule B
                 * strcmp didn't match any of them for the target leader. */
                if (bad_self_cnt > 0) {
                    int dk;
                    for (dk = 0; dk < bad_self_cnt; dk++) {
                        pr_warn("powerfs: REDIRECT diag[%d/%d]: "
                                "leader='%s':%u blacklist='%s':%u "
                                "strcmp_addr=%d port_eq=%d "
                                "(attempt %d, shard=%llu)\n",
                                dk, bad_self_cnt,
                                leader_addr, leader_port,
                                bad_self_addrs[dk], bad_self_ports[dk],
                                strcmp(leader_addr, bad_self_addrs[dk]),
                                leader_port == bad_self_ports[dk] ? 1 : 0,
                                attempt,
                                (unsigned long long)req->shard_id);
                    }
                }

                /* Cross-filer A<->B loop detection. */
                if (last_tried_conn &&
                    strcmp(leader_addr, last_tried_conn->addr) == 0 &&
                    leader_port == last_tried_conn->port) {
                    pr_warn("powerfs: redirect loop detected: %s:%u -> %s:%u -> %s:%u, breaking with EAGAIN\n",
                            last_tried_conn->addr, last_tried_conn->port,
                            conn->addr, conn->port,
                            leader_addr, leader_port);
                    filer_idx = powerfs_conn_get_filer_idx(conn);
                    if (filer_idx >= 0)
                        powerfs_shard_route_on_filer_disconnect(filer_idx);
                    req->error = -EAGAIN;
                    powerfs_req_complete(req);
                    return -EAGAIN;
                }
                pr_warn("powerfs: redirect to leader %s:%u (from %s:%u, filer_count=%d)\n",
                        leader_addr, leader_port, conn->addr, conn->port,
                        g_pool.filer_count);

                /* 查找或更新 filer 路由 */
                leader_conn = powerfs_conn_find_filer(leader_addr, leader_port);
                if (leader_conn) {
                    int new_idx = powerfs_conn_get_filer_idx(leader_conn);
                    if (new_idx >= 0) {
                        /* FIX V2+V6: Defence-in-depth before route update.
                         * Even if Rule B stale-redirect guard somehow missed
                         * (e.g. formatting edge case vs leader_addr string),
                         * check the RESOLVED leader_conn pointer against the
                         * ENTIRE bad_self blacklist array.  On any match:
                         * treat as stale-redirect — don't VALID→blacklisted,
                         * stay CHECKING, backoff + rotate. */
                        {
                            int v6k;
                            bool v6_hit = false;
                            for (v6k = 0; v6k < bad_self_cnt; v6k++) {
                                if (strcmp(leader_conn->addr,
                                           bad_self_addrs[v6k]) == 0 &&
                                    leader_conn->port ==
                                        bad_self_ports[v6k]) {
                                    v6_hit = true;
                                    break;
                                }
                            }
                            if (v6_hit) {
                                unsigned long backoff_v2;
                                pr_warn("powerfs: stale-redirect (defence): "
                                        "resolved leader %s:%u matches "
                                        "blacklist[%d] (cnt=%d); SKIP route "
                                        "update, stay CHECKING + rotate "
                                        "(attempt %d, shard=%llu)\n",
                                        leader_conn->addr, leader_conn->port,
                                        v6k, bad_self_cnt,
                                        attempt,
                                        (unsigned long long)req->shard_id);
                                if (req->shard_id < POWERFS_MAX_SHARDS) {
                                    struct powerfs_shard_route_entry *entry_v2;
                                    entry_v2 = &g_pool.shard_route.entries[req->shard_id];
                                    spin_lock(&g_pool.shard_route.lock);
                                    if (entry_v2->state == ROUTE_VALID)
                                        entry_v2->state = ROUTE_CHECKING;
                                    spin_unlock(&g_pool.shard_route.lock);
                                }
                                last_tried_conn = conn;
                                /* PERF OPT: stable redirect pattern. */
                                backoff_v2 = min(20ul * (1UL << min(attempt, 1)),
                                                 50ul);
                                reinit_completion(&req->done);
                                req->error = 0;
                                msleep(backoff_v2);
                                continue;
                            }
                        }
                        /* 记录当前 filer 为上次尝试的 filer,
                         * 用于下一轮的 cross-filer redirect loop 检测 */
                        last_tried_conn = conn;
                        /* 更新 shard 路由到新 leader */
                        powerfs_shard_route_update(req->shard_id, new_idx);
                        /* 重试请求 (走新 leader) */
                        reinit_completion(&req->done);
                        req->error = 0;
                        continue;
                    }
                }
            }
            /* redirect 解析失败或 filer 未找到 */
            {
                int fi;
                pr_warn("powerfs: redirect failed, no leader filer found (body_len=%zu, filer_count=%d)\n",
                        req->resp_body_len, g_pool.filer_count);
                for (fi = 0; fi < g_pool.filer_count && fi < 8; fi++) {
                    pr_warn("powerfs:   filer[%d]: addr=%s port=%u in_use=%d\n",
                            fi, g_pool.filers[fi].addr, g_pool.filers[fi].port,
                            g_pool.filers[fi].in_use);
                }
            }
            req->error = -EAGAIN;
            powerfs_req_complete(req);
            return -EAGAIN;
        }

        /* 非 REDIRECT: 正常完成 (do_send 已 complete) */

        /* ==== PERF OPT v1: ROLLOUT SAFELY ====
         * The leader-caching optimization (setting VALID route on success)
         * is TEMPORARILY DISABLED because it correlates with soft lockup /
         * invalid opcode panics.  Investigate separately.  For now we still
         * get the 20ms backoff improvement (was 400-800ms), which reduces
         * per-request overhead from 1.2s → ~70ms. */
#if 0
        if (req->error == 0 &&
            req->resp_status != POWERFS_NET_STATUS_ERR_REDIRECT &&
            conn != NULL &&
            req->shard_id < POWERFS_MAX_SHARDS) {
            int ok_idx = powerfs_conn_get_filer_idx(conn);
            if (ok_idx >= 0) {
                struct powerfs_shard_route_entry *ok_entry;
                ok_entry = &g_pool.shard_route.entries[req->shard_id];
                spin_lock(&g_pool.shard_route.lock);
                ok_entry->leader_filer_idx = ok_idx;
                ok_entry->state = ROUTE_VALID;
                spin_unlock(&g_pool.shard_route.lock);
            }
        }
#endif

        return req->error;
    }

    /* 不会到达 */
    req->error = -EAGAIN;
    powerfs_req_complete(req);
    return -EAGAIN;
}
EXPORT_SYMBOL_GPL(powerfs_request_submit);

/* ========== 请求/响应核心 ========== */

/**
 * powerfs_net_parse_redirect - 从 REDIRECT 响应 body 解析 leader net 地址
 *
 * Filer 的 check_leader 在非 leader 时返回 STATUS_ERR_REDIRECT,
 * 响应 body 包含 Owner 字段 (string, 格式 "ip:port").
 * 内核需解析出地址, 切换连接到 leader 后重试请求.
 *
 * 返回: 0 成功, 负数失败
 */
int powerfs_net_parse_redirect(const __u8 *body, size_t body_len,
                                       char *addr, size_t addr_cap,
                                       __u16 *port)
{
    struct powerfs_tlv_dec dec;
    char owner[80];
    char *colon;
    unsigned long p;

    powerfs_tlv_dec_init(&dec, body, body_len);
    if (powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_OWNER,
                                owner, sizeof(owner) - 1) < 0)
        return -EINVAL;

    /* 解析 "ip:port" (用 strrchr 兼容 IPv6) */
    colon = strrchr(owner, ':');
    if (!colon)
        return -EINVAL;

    *colon = '\0';
    if (kstrtoul(colon + 1, 10, &p) < 0 || p == 0 || p > 0xFFFF)
        return -EINVAL;
    *port = (__u16)p;

    strncpy(addr, owner, addr_cap - 1);
    addr[addr_cap - 1] = '\0';
    return 0;
}

/**
 * powerfs_net_send_request - 发送同步请求并等待响应
 *
 * 通过新架构连接池 (per-filer 连接 + shard 路由 + 调度器) 发送请求:
 *   1. 分配 powerfs_request 并绑定 body/data
 *   2. powerfs_request_submit 走状态驱动路由 (VALID/CHECKING/UNKNOWN)
 *   3. REDIRECT 自动更新路由表并重试 (在 submit 内部完成)
 *   4. 断连返回 -ENOTCONN, 主线程在 submit 中 deadline-based 重试
 *
 * @resp_body_len_out: 输出: 实际接收到的 body 长度 (可为 NULL)
 * @resp_data_len_out: 输出: 实际接收到的 data 长度 (可为 NULL)
 *
 * 返回值:
 *   >= 0: 成功 (0 = OK, >0 = powerfs-net 状态码)
 *   < 0: 错误 (-errno)
 */
int powerfs_net_send_request(__u16 msg_type, u64 route_inode,
                              const __u8 *body, size_t body_len,
                              const __u8 *data, size_t data_len,
                              __u8 *resp_body, size_t resp_body_cap,
                              __u8 *resp_data, size_t resp_data_cap,
                              int timeout_ms,
                              size_t *resp_body_len_out,
                              size_t *resp_data_len_out)
{
    struct powerfs_request *req;
    int ret;

    /* 新架构: 连接池必须已初始化且有 filer 连接, 否则直接返回 -ENOTCONN.
     * 旧 g_conn 单连接 fallback 已移除. */
    if (g_pool.filer_count == 0 || atomic_read(&g_pool.stopping))
        return -ENOTCONN;

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
    req->shard_id = route_inode ? powerfs_calc_shard_id(route_inode) : 0;
    if (timeout_ms > 0)
        req->deadline = jiffies + msecs_to_jiffies(timeout_ms);

    ret = powerfs_request_submit(req);

    /* 输出实际响应长度 */
    if (resp_body_len_out)
        *resp_body_len_out = req->resp_body_len;
    if (resp_data_len_out)
        *resp_data_len_out = req->resp_data_len;

    /* 返回: 0=成功, >0=状态码, <0=错误 */
    if (ret < 0) {
        powerfs_request_free(req);
        return ret;
    }

    ret = req->resp_status;
    powerfs_request_free(req);
    return ret;
}

/**
 * powerfs_net_send_request_shard - 发送请求, 直接路由到 explicit_shard_id.
 *
 * 与 powerfs_net_send_request 的唯一区别:
 *   req->shard_id = explicit_shard_id (绕过 calc_shard_id).
 *
 * 用于 AllocInodeBatch / MkdirPhaseA(target_shard) 等场景: 请求需要
 * 发往指定 shard (非 inode-derived). 在 powerfs_request_submit 中
 * ROUTE_VALID/CHECKING/UNKNOWN 全部使用 req->shard_id, 所以只需要在
 * 分配 req 时替换 shard_id 赋值即可.
 */
int powerfs_net_send_request_shard(__u16 msg_type, __u64 explicit_shard_id,
                                   const __u8 *body, size_t body_len,
                                   const __u8 *data, size_t data_len,
                                   __u8 *resp_body, size_t resp_body_cap,
                                   __u8 *resp_data, size_t resp_data_cap,
                                   int timeout_ms,
                                   size_t *resp_body_len_out,
                                   size_t *resp_data_len_out)
{
    struct powerfs_request *req;
    int ret;

    if (g_pool.filer_count == 0 || atomic_read(&g_pool.stopping))
        return -ENOTCONN;

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
    req->shard_id = explicit_shard_id;          /* direct shard routing */
    if (timeout_ms > 0)
        req->deadline = jiffies + msecs_to_jiffies(timeout_ms);

    ret = powerfs_request_submit(req);

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
EXPORT_SYMBOL_GPL(powerfs_net_send_request_shard);

/* ========== 状态码转换 ========== */

/**
 * net_status_to_errno - 将 powerfs-net 状态码转换为 Linux errno
 */
int net_status_to_errno(__u16 status)
{
    switch (status) {
    case POWERFS_NET_STATUS_OK:              return 0;
    case POWERFS_NET_STATUS_ERR_NOT_FOUND:   return -ENOENT;
    case POWERFS_NET_STATUS_ERR_ALREADY_EXISTS: return -EEXIST;
    case POWERFS_NET_STATUS_ERR_PERMISSION:  return -EPERM;
    case POWERFS_NET_STATUS_ERR_IO:          return -EIO;
    case POWERFS_NET_STATUS_ERR_INVALID_ARG: return -EINVAL;
    case POWERFS_NET_STATUS_ERR_NOT_DIR:     return -ENOTDIR;
    case POWERFS_NET_STATUS_ERR_IS_DIR:      return -EISDIR;
    case POWERFS_NET_STATUS_ERR_NO_SPACE:    return -ENOSPC;
    case POWERFS_NET_STATUS_ERR_BAD_FD:      return -EBADF;
    case POWERFS_NET_STATUS_ERR_SERVER:      return -EREMOTEIO;
    /* REDIRECT 正常在 send_request 内部处理; 若漏到此处说明重试耗尽, 返回 -EAGAIN */
    case POWERFS_NET_STATUS_ERR_REDIRECT:    return -EAGAIN;
    default:                                 return -EIO;
    }
}

