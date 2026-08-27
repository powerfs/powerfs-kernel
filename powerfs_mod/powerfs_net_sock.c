/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_sock.c - split from powerfs_net.c (mechanical refactor) */

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

/* ========== Socket 辅助函数 ========== */

/**
 * 创建 TCP 内核 socket
 */
struct socket *powerfs_net_create_tcp_socket(void)
{
    struct socket *sock;
    int ret;

    ret = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
    if (ret < 0) {
        pr_err("powerfs: failed to create TCP socket: %d\n", ret);
        return NULL;
    }

    /* 设置 socket 选项.
     * sk_sndtimeo 初始用 CONNECT_TIMEOUT (3s): kernel_connect 用此超时,
     * filer 不可达时快速失败, 避免多 filer 串行 connect 累积阻塞.
     * connect_one 成功后改回 SEND_TIMEOUT (10s) 用于后续 send. */
    sock->sk->sk_rcvtimeo = msecs_to_jiffies(POWERFS_NET_RECV_TIMEOUT);
    sock->sk->sk_sndtimeo = msecs_to_jiffies(POWERFS_NET_CONNECT_TIMEOUT);

    /* 设置大缓冲区: 默认 sk_sndbuf (~208KB) 对 1MB write_needle 帧太小,
     * 导致 kernel_sendmsg 频繁阻塞等待 TCP 窗口推进.
     * 设为 2MB 可容纳完整 CHUNK_SIZE (2MB) 数据帧, 配合 MSG_DONTWAIT 实现零阻塞发送.
     * sk_rcvbuf 同步增大, 确保高并发响应不丢包. */
    sock->sk->sk_sndbuf = max_t(int, sock->sk->sk_sndbuf, POWERFS_CHUNK_SIZE);
    sock->sk->sk_rcvbuf = max_t(int, sock->sk->sk_rcvbuf, POWERFS_CHUNK_SIZE);

    /* 启用 TCP_NODELAY 减少延迟 */
    tcp_sock_set_nodelay(sock->sk);

    /* 启用 TCP keepalive: 检测静默死亡 (网络分区/peer panic 不发 FIN).
     * RX 线程能感知 peer 主动关闭 (FIN/RST), 但静默分区下 recv 不返回,
     * keepalive 失败后 sk_err 置位 → RX recv 返回错误 → 触发断连清理.
     * 检测时延 ≈ keepidle + keepcnt * keepintvl = 5 + 3*2 = 11s. */
    sock_set_keepalive(sock->sk);
    if (tcp_sock_set_keepidle(sock->sk, 5) < 0)
        pr_warn("powerfs: tcp_sock_set_keepidle failed\n");
    if (tcp_sock_set_keepintvl(sock->sk, 2) < 0)
        pr_warn("powerfs: tcp_sock_set_keepintvl failed\n");
    if (tcp_sock_set_keepcnt(sock->sk, 3) < 0)
        pr_warn("powerfs: tcp_sock_set_keepcnt failed\n");

    return sock;
}

/**
 * 建立 TCP 连接
 */
int powerfs_net_tcp_connect(struct socket *sock, const char *addr,
                                    __u16 port)
{
    struct sockaddr_in sin;
    int ret;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = cpu_to_be16(port);

    /* 解析 IP 地址 (使用 in4_pton 兼容内核 6.2+).
     * 注意: in4_pton 返回 1=成功, 0=失败 (不是 <0).
     *       必须用 !ret 判断, 否则非法 IP 静默通过 → sin_addr=0.0.0.0
     *       → connect 无意义, 且 register_client 走 -ENOLINK/-67. */
    ret = in4_pton(addr, -1, (void *)&sin.sin_addr, '\0', NULL);
    if (!ret) {
        pr_err("powerfs: invalid server address: %s (in4_pton returned 0)\n", addr);
        return -EINVAL;
    }

    /* 内核态 connect */
    ret = kernel_connect(sock, (struct sockaddr *)&sin, sizeof(sin), 0);
    if (ret < 0) {
        pr_err("powerfs: connect to %s:%u failed: %d\n", addr, port, ret);
        return ret;
    }

    pr_debug("powerfs: connected to %s:%u\n", addr, port);
    return 0;
}

/**
 * 关闭 socket
 */
void powerfs_net_close_socket(struct socket *sock)
{
    if (sock) {
        kernel_sock_shutdown(sock, SHUT_RDWR);
        sock_release(sock);
    }
}

/* ========== 帧发送/接收 ========== */

/**
 * powerfs_net_frame_send - 发送一个完整帧 (header + body + data)
 */
/**
 * powerfs_net_frame_send - 发送一个完整帧
 *
 * 使用 kernel_sendmsg 原子发送: 帧头 + body + data
 */
int powerfs_net_frame_send(struct socket *sock,
                            struct powerfs_net_frame_hdr *hdr,
                            const __u8 *body, size_t body_len,
                            const __u8 *data, size_t data_len)
{
    struct kvec vec[3];
    int vec_count = 0;
    size_t total_len, sent_total = 0;
    ssize_t sent;

    if (!sock)
        return -ENOTCONN;

    /* 准备 kvec 数组 */
    vec[vec_count].iov_base = hdr;
    vec[vec_count].iov_len = POWERFS_NET_FRAME_HDR_SIZE;
    vec_count++;

    if (body_len > 0 && body) {
        vec[vec_count].iov_base = (void *)body;
        vec[vec_count].iov_len = body_len;
        vec_count++;
    }

    if (data_len > 0 && data) {
        vec[vec_count].iov_base = (void *)data;
        vec[vec_count].iov_len = data_len;
        vec_count++;
    }

    /* 计算总长度 */
    total_len = vec[0].iov_len;
    if (vec_count > 1) total_len += vec[1].iov_len;
    if (vec_count > 2) total_len += vec[2].iov_len;

    /* 防御: 帧大小不超过 MAX_FRAME (与 Rust MAX_FRAME_SIZE 一致) */
    if (total_len > POWERFS_NET_MAX_FRAME) {
        pr_err("powerfs: frame too large: %zu > %d (body=%zu data=%zu)\n",
               total_len, POWERFS_NET_MAX_FRAME, body_len, data_len);
        return -EMSGSIZE;
    }

    /* 循环发送直到所有字节发完.
     * kernel_sendmsg 对大帧 (如 write_needle 携带 1MB data) 可能只发送
     * 部分数据 (TCP 发送缓冲区满时返回已接受的字节数 < total_len).
     * 若不循环发送剩余部分, 接收端 Rust read_exact 会读到错位字节流,
     * 导致下一帧的 28 字节帧头落在上一帧残留 data 上 -> magic/CRC
     * 校验失败 -> "invalid frame header" -> 连接断开 (ENOTCONN).
     *
     * 此函数用于 discover 路径 (原始 socket, sk_sndtimeo=10s 阻塞模式).
     * TX 调度器路径使用 pfs_frame_send_nonblock (MSG_DONTWAIT + send_offset). */
    while (sent_total < total_len) {
        struct msghdr msg = {};

        sent = kernel_sendmsg(sock, &msg, vec, vec_count,
                              total_len - sent_total);
        if (sent < 0) {
            if (sent == -EINTR)
                continue;
            if (sent == -EAGAIN) {
                pr_warn("powerfs: send EAGAIN (sk_sndtimeo expired, "
                        "sent=%zu/%zu) -> ENOTCONN to trigger disconnect\n",
                        sent_total, total_len);
                return -ENOTCONN;
            }
            pr_err("powerfs: send failed: %zd (sent=%zu/%zu)\n",
                   sent, sent_total, total_len);
            return sent;
        }
        if (sent == 0)
            return -ECONNRESET;

        sent_total += (size_t)sent;

        /* 调整 kvec: 跳过已发送的字节, 下一轮从剩余部分继续 */
        if (sent_total < total_len) {
            size_t to_skip = (size_t)sent;
            int i;
            for (i = 0; i < vec_count && to_skip > 0; i++) {
                if (vec[i].iov_len <= to_skip) {
                    to_skip -= vec[i].iov_len;
                    vec[i].iov_len = 0;
                    vec[i].iov_base = NULL;
                } else {
                    vec[i].iov_base = (char *)vec[i].iov_base + to_skip;
                    vec[i].iov_len -= to_skip;
                    to_skip = 0;
                }
            }
        }
    }

    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_frame_send);

/**
 * pfs_frame_send_nonblock - 非阻塞发送帧, 支持 partial send 续传.
 *
 * 使用 MSG_DONTWAIT: TCP 缓冲区满时立即返回 -EAGAIN, 不阻塞 TX 线程.
 * 通过 req->send_offset 跨调用追踪已发送字节, EAGAIN 后重新入队从 offset 继续.
 *
 * 返回值:
 *   0       = 全部发送完成 (req->send_offset 已重置为 0)
 *   -EAGAIN = 部分发送, 需重新入 tx_queue 等 sk_write_space 回调
 *   <0      = 不可恢复错误 (触发断连)
 */
int pfs_frame_send_nonblock(struct socket *sock,
                                   struct powerfs_net_frame_hdr *hdr,
                                   const __u8 *body, size_t body_len,
                                   const __u8 *data, size_t data_len,
                                   struct powerfs_request *req)
{
    struct kvec vec[3];
    int vec_count = 0;
    int i;
    size_t total_len, sent_total;
    ssize_t sent;
    size_t skip;

    if (!sock)
        return -ENOTCONN;

    /* 准备 kvec: header + body + data */
    vec[vec_count].iov_base = hdr;
    vec[vec_count].iov_len = POWERFS_NET_FRAME_HDR_SIZE;
    vec_count++;
    if (body_len > 0 && body) {
        vec[vec_count].iov_base = (void *)body;
        vec[vec_count].iov_len = body_len;
        vec_count++;
    }
    if (data_len > 0 && data) {
        vec[vec_count].iov_base = (void *)data;
        vec[vec_count].iov_len = data_len;
        vec_count++;
    }

    total_len = vec[0].iov_len;
    if (vec_count > 1) total_len += vec[1].iov_len;
    if (vec_count > 2) total_len += vec[2].iov_len;

    /* 从上次 partial send 的 offset 继续 */
    sent_total = req->send_offset;
    if (sent_total >= total_len)
        return 0;

    /* 跳过已发送的字节, 调整 kvec 起点 */
    skip = sent_total;
    for (i = 0; i < vec_count && skip > 0; i++) {
        if (vec[i].iov_len <= skip) {
            skip -= vec[i].iov_len;
            vec[i].iov_len = 0;
            vec[i].iov_base = NULL;
        } else {
            vec[i].iov_base = (char *)vec[i].iov_base + skip;
            vec[i].iov_len -= skip;
            skip = 0;
        }
    }

    while (sent_total < total_len) {
        struct msghdr msg = {
            .msg_flags = MSG_DONTWAIT,
        };

        sent = kernel_sendmsg(sock, &msg, vec, vec_count,
                              total_len - sent_total);
        if (sent < 0) {
            if (sent == -EINTR)
                continue;
            if (sent == -EAGAIN) {
                /* TCP 缓冲区满: 保存进度, 返回 EAGAIN 让 TX 线程
                 * 处理其他连接, 等 sk_write_space 回调重新入队 */
                req->send_offset = sent_total;
                return -EAGAIN;
            }
            pr_err("powerfs: nonblock send failed: %zd (sent=%zu/%zu)\n",
                   sent, sent_total, total_len);
            return sent;
        }
        if (sent == 0)
            return -ECONNRESET;

        sent_total += (size_t)sent;

        /* 调整 kvec: 跳过本轮已发送的字节 */
        if (sent_total < total_len) {
            size_t to_skip = (size_t)sent;
            for (i = 0; i < vec_count && to_skip > 0; i++) {
                if (vec[i].iov_len <= to_skip) {
                    to_skip -= vec[i].iov_len;
                    vec[i].iov_len = 0;
                    vec[i].iov_base = NULL;
                } else {
                    vec[i].iov_base = (char *)vec[i].iov_base + to_skip;
                    vec[i].iov_len -= to_skip;
                    to_skip = 0;
                }
            }
        }
    }

    /* 全部发送完成, 重置 offset */
    req->send_offset = 0;
    return 0;
}

/**
 * powerfs_net_frame_recv - 接收一个完整帧
 *
 * 接收流程:
 *   1. 先接收 28 字节帧头
 *   2. 解析 data_len (总长度) 和 body_len (body 段长度)
 *   3. 接收 data_len 字节, 按 body_len 切分到 body_buf 和 data_buf
 *
 * 注意: body/data 边界由帧头 body_len 字段决定 (不再依赖 body_cap hack).
 */
int powerfs_net_frame_recv(struct socket *sock,
                            struct powerfs_net_frame_hdr *hdr,
                            __u8 *body_buf, size_t body_cap, size_t *body_len,
                            __u8 *data_buf, size_t data_cap, size_t *data_len,
                            int timeout_ms)
{
    __u8 hdr_buf[POWERFS_NET_FRAME_HDR_SIZE];
    struct kvec vec;
    struct msghdr msg = {};
    ssize_t received;
    size_t total_data, body_sz, data_sz;

    if (!sock)
        return -ENOTCONN;

    /* 设置接收超时 */
    if (timeout_ms > 0)
        sock->sk->sk_rcvtimeo = msecs_to_jiffies(timeout_ms);

    /* 1. 接收帧头 (28 字节).
     * MSG_WAITALL: 阻塞 socket 下等待直到收到请求数量, 避免 TCP 短读
     * 导致数据流错位 (短读后下次 recv 读到 body 而非 header -> invalid frame header). */
    vec.iov_base = hdr_buf;
    vec.iov_len = POWERFS_NET_FRAME_HDR_SIZE;

    received = kernel_recvmsg(sock, &msg, &vec, 1, POWERFS_NET_FRAME_HDR_SIZE,
                              MSG_WAITALL);
    if (received < 0) {
        pr_err("powerfs: recv header failed: %zd\n", received);
        return received;
    }

    if (received < POWERFS_NET_FRAME_HDR_SIZE) {
        /* received == 0: 对端关闭连接 (EOF)
         * 0 < received < HDR_SIZE: 连接异常, 部分数据后断开
         * 两种情况都视为连接断开, 返回 -ECONNRESET 触发重连 */
        pr_err("powerfs: recv header truncated: %zd < %d (peer closed)\n",
               received, POWERFS_NET_FRAME_HDR_SIZE);
        return -ECONNRESET;
    }

    /* 2. 解码帧头 */
    if (!powerfs_net_frame_hdr_decode(hdr_buf, POWERFS_NET_FRAME_HDR_SIZE, hdr)) {
        pr_err("powerfs: invalid frame header\n");
        return -EINVAL;
    }

    /* 3. 按 body_len 切分 body 和 data */
    total_data = hdr->data_len;
    body_sz = hdr->body_len;
    if (body_sz > total_data)
        body_sz = total_data;  /* 防御: body_len 异常时钳制 */
    data_sz = total_data - body_sz;

    if (body_sz > body_cap) {
        pr_err("powerfs: body section too large: %zu > %zu\n",
               body_sz, body_cap);
        return -E2BIG;
    }
    if (data_sz > data_cap) {
        pr_err("powerfs: data section too large: %zu > %zu\n",
               data_sz, data_cap);
        return -E2BIG;
    }

    /* 接收 body 段 */
    if (body_sz > 0) {
        vec.iov_base = body_buf;
        vec.iov_len = body_sz;
        received = kernel_recvmsg(sock, &msg, &vec, 1, body_sz, MSG_WAITALL);
        if (received < 0)
            return received;
        if (body_len)
            *body_len = received;
    } else {
        if (body_len)
            *body_len = 0;
    }

    /* 接收 data 段 */
    if (data_sz > 0) {
        vec.iov_base = data_buf;
        vec.iov_len = data_sz;
        received = kernel_recvmsg(sock, &msg, &vec, 1, data_sz, MSG_WAITALL);
        if (received < 0)
            return received;
        if (data_len)
            *data_len = received;
    } else {
        if (data_len)
            *data_len = 0;
    }

    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_frame_recv);

/* ========== 握手 (用于 master 发现阶段的裸 socket 握手) ========== */

/**
 * powerfs_net_do_handshake - 与 Filer/Master 握手 (裸 18 字节协议，不含帧头)
 *
 * 仅用于 powerfs_net_discover_filers 中的临时 socket 握手, 不存储
 * server_id/features (发现阶段不需要).
 */
int powerfs_net_do_handshake(struct socket *sock)
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
    req.channel = POWERFS_NET_CHANNEL_DATA;
    req.reserved = 0;
    client_id = atomic_inc_return(&g_discover_seq) + 1000000;
    req.client_id = cpu_to_le64(client_id);
    req.features = 0;

    /* 发送裸握手请求 (20 字节) */
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)&req;
    iov.iov_len = sizeof(req);
    ret = kernel_sendmsg(sock, &msg, &iov, 1, sizeof(req));
    if (ret < 0) {
        pr_err("powerfs: handshake send failed: %d\n", ret);
        return ret;
    }

    /* 接收裸握手响应 (18 字节) */
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)&resp;
    iov.iov_len = sizeof(resp);
    ret = kernel_recvmsg(sock, &msg, &iov, 1, sizeof(resp), 0);
    if (ret < 0) {
        pr_err("powerfs: handshake recv failed: %d\n", ret);
        return ret;
    }

    /* 验证响应 */
    if (memcmp(resp.magic, "PFSN", 4) != 0) {
        pr_err("powerfs: handshake response bad magic\n");
        return -EINVAL;
    }

    if (resp.status != 0) {
        pr_err("powerfs: handshake rejected, status=%u\n", resp.status);
        return -EPERM;
    }

    pr_debug("powerfs: handshake OK, server_id=%llu\n",
            (unsigned long long)le64_to_cpu(resp.server_id));

    return 0;
}

