// SPDX-License-Identifier: GPL-2.0
/*
 * powerfs_net_tcp_ops.c - TCP 传输 ops 实现 (Phase 1: transport abstraction)
 *
 * 把现有 TCP 路径 (powerfs_net_sock.c / powerfs_net_conn.c) 包装成
 * struct powerfs_transport_ops. 仅做转发, 不改变既有逻辑; 现有调用点
 * (powerfs_conn_connect_one / pfs_process_transmit / pfs_process_receive)
 * 仍直接调底层函数, 本 ops 供后续 Phase 切换为 conn->transport->ops
 * 调用时使用, 也可供 RDMA 实现参照签名.
 *
 * 包装映射:
 *   tcp_connect     -> powerfs_net_create_tcp_socket + powerfs_net_tcp_connect
 *                      + powerfs_conn_do_handshake (+ 超时设置 + conn->sock 赋值)
 *   tcp_disconnect  -> powerfs_net_close_socket(conn->sock) + sock=NULL
 *   tcp_is_connected -> conn->sock != NULL && state == CONN_CONNECTED
 *   tcp_send_frame  -> pfs_frame_send_nonblock(conn->sock, ...)
 *   tcp_has_rx_data -> !skb_queue_empty(&sock->sk->sk_receive_queue)
 *   tcp_recv_frame  -> pfs_rx_step(conn)
 *   (其余 no-op: TCP sk 回调始终激活, 无需显式 arm)
 */

#include <linux/module.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <net/sock.h>

#include "powerfs_net.h"
#include "powerfs_net_internal.h"
#include "powerfs_net_transport.h"

/* ========== 连接生命周期 ========== */

/* 建链 + 握手. 与 powerfs_conn_connect_one 的传输部分对齐:
 * 创建 socket -> connect -> handshake -> 设超时 -> 赋值 conn->sock.
 * 不碰状态机/回调安装 (那些是连接池职责, 由 connect_one 自己做). */
static int tcp_connect(struct powerfs_net_server_conn *conn)
{
    struct socket *sock;
    int ret;

    sock = powerfs_net_create_tcp_socket();
    if (!sock)
        return -ENOMEM;

    ret = powerfs_net_tcp_connect(sock, conn->addr, conn->port);
    if (ret < 0) {
        powerfs_net_close_socket(sock);
        return ret;
    }

    ret = powerfs_conn_do_handshake(sock, conn);
    if (ret < 0) {
        powerfs_net_close_socket(sock);
        return ret;
    }

    /* connect 阶段用 CONNECT_TIMEOUT, 连接成功后改回 SEND/RECV_TIMEOUT
     * (与 powerfs_conn_connect_one 一致, 供后续 kernel_sendmsg/recvmsg). */
    sock->sk->sk_rcvtimeo = msecs_to_jiffies(POWERFS_NET_RECV_TIMEOUT);
    sock->sk->sk_sndtimeo = msecs_to_jiffies(POWERFS_NET_SEND_TIMEOUT);

    conn->sock = sock;
    return 0;
}

/* 关闭传输: shutdown + release socket, 置 conn->sock=NULL.
 * 不负责状态机/回调清理 (由 powerfs_conn_disconnect_one 处理). */
static void tcp_disconnect(struct powerfs_net_server_conn *conn)
{
    struct socket *sock = conn->sock;

    if (sock) {
        powerfs_net_close_socket(sock);
        conn->sock = NULL;
    }
}

/* 传输是否就绪可收发. */
static bool tcp_is_connected(struct powerfs_net_server_conn *conn)
{
    return conn->sock != NULL && conn->state == CONN_CONNECTED;
}

/* ========== 帧收发 ========== */

/* 非阻塞发送一帧. 直接转发 pfs_frame_send_nonblock, 传 conn->sock.
 * 返回值约定: 0=完成, -EAGAIN=部分发送待续, <0=不可恢复错误. */
static int tcp_send_frame(struct powerfs_net_server_conn *conn,
                          struct powerfs_net_frame_hdr *hdr,
                          const __u8 *body, size_t body_len,
                          const __u8 *data, size_t data_len,
                          struct powerfs_request *req)
{
    if (!conn->sock)
        return -ENOTCONN;
    return pfs_frame_send_nonblock(conn->sock, hdr,
                                   body, body_len,
                                   data, data_len, req);
}

/* 是否有可收数据. 仅以 socket 接收队列为准.
 *
 * 绝不能兜底 conn->rx_ready: pfs_process_receive 执行期间 conn 已移出
 * rx_list 但 rx_ready 仍为 1 (要等 process_receive 返回后由
 * pfs_rx_thread_fn 在 rx_lock 下清零/重判). 若这里在队列空时返回
 * rx_ready(=1), 内层 "recv EAGAIN → has_rx_data=true → continue" 循环
 * 永不退出, rx kthread 100% CPU 忙转 (实测 1200 万次/秒), 并饿死同调度池
 * 其他 conn (响应滞留 sk_receive_queue 无人读 → 30s 超时, Bug B).
 * 最终 rx_ready 裁决在 pfs_rx_thread_fn 的 rx_lock 临界区完成,
 * 与 pfs_data_ready 回调互斥, 不会丢唤醒. */
static bool tcp_has_rx_data(struct powerfs_net_server_conn *conn)
{
    return conn->sock && conn->sock->sk &&
           !skb_queue_empty(&conn->sock->sk->sk_receive_queue);
}

/* 非阻塞推进收帧状态机. 直接转发 pfs_rx_step:
 *   0       = 一帧完整 (conn->rx_cur_hdr / rx_body_buf / rx_data_buf 已填)
 *   -EAGAIN = 当前无数据, 保留 partial 状态
 *   <0      = EOF/RST/错误, 调用者触发断连 */
static int tcp_recv_frame(struct powerfs_net_server_conn *conn)
{
    return pfs_rx_step(conn);
}

/* ========== 通知控制 (TCP: no-op) ========== */

/* TCP 的 sk_data_ready / sk_write_space 回调由 socket stack 始终激活,
 * 无需显式 arm. RDMA 才需要 post_recv / arm completion. */
static void tcp_enable_rx_notify(struct powerfs_net_server_conn *conn)
{
    /* no-op: TCP sk callbacks always active */
}

static void tcp_enable_tx_notify(struct powerfs_net_server_conn *conn)
{
    /* no-op: TCP sk callbacks always active */
}

/* ========== per-conn init/fini (TCP: no-op) ========== */

/* TCP 无 per-conn 传输私有预分配 (RX buffer 由 conn init 直接调
 * pfs_conn_alloc_rxbuffers). RDMA 在此分配 QP / 注册 MR. */
static int tcp_init_conn(struct powerfs_net_server_conn *conn)
{
    return 0;
}

static void tcp_fini_conn(struct powerfs_net_server_conn *conn)
{
    /* no-op: TCP cleanup happens in tcp_disconnect */
}

/* ========== 全局 init/fini (TCP: no-op) ========== */

static int tcp_global_init(void)
{
    return 0;
}

static void tcp_global_exit(void)
{
    /* no-op */
}

/* ========== ops 表 ========== */

const struct powerfs_transport_ops powerfs_tcp_ops = {
    .name             = "tcp",
    .type             = POWERFS_TRANSPORT_TCP,

    .connect          = tcp_connect,
    .disconnect       = tcp_disconnect,
    .is_connected     = tcp_is_connected,

    .send_frame       = tcp_send_frame,
    .has_rx_data      = tcp_has_rx_data,
    .recv_frame       = tcp_recv_frame,

    .enable_rx_notify = tcp_enable_rx_notify,
    .enable_tx_notify = tcp_enable_tx_notify,

    .init_conn        = tcp_init_conn,
    .fini_conn        = tcp_fini_conn,

    .global_init      = tcp_global_init,
    .global_exit      = tcp_global_exit,
};
