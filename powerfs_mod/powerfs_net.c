/*
 * PowerFS 内核态 powerfs-net 协议实现
 *
 * 直接在内核中实现 powerfs-net 二进制协议，通过 TCP 连接与 Filer 通信。
 * 参考 Ceph 内核客户端的 socket 使用模式。
 *
 * 通信流程:
 *   1. 建立 TCP 连接 (sock_create_kern + kernel_connect)
 *   2. 发送握手帧 (客户端类型 = Kernel)
 *   3. 发送请求帧 + 等待响应帧
 *   4. 断线自动重连 (workqueue)
 *
 * 线程安全:
 *   - conn_lock 保护连接状态和 socket 操作
 *   - req_lock + pending_reqs 保护序列号到请求的映射
 *   - 每个请求使用 completion 等待响应
 */

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
#include "powerfs_ec.h"

#include <net/sock.h>
#include <net/tcp.h>
#include <net/net_namespace.h>
#include <net/inet_sock.h>

#include "powerfs_net.h"
#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_flow.h"

/* ========== 全局连接上下文 ========== */

static bool g_initialized = false;

/* 前向声明: g_pool 定义在后面 (多连接池实现段) */
static struct powerfs_net_pool g_pool;

/* 从模块参数获取 (定义在 powerfs_mod.c) */
extern ushort shard_count;

/* discover_filers 用的序列号计数器 (旧 g_conn.seq_counter 的替代,
 * 仅用于 master 发现阶段的裸 socket 请求, 不涉及 per-filer 连接) */
static atomic_t g_discover_seq = ATOMIC_INIT(0);

/* ========== 前向声明 ========== */
static int powerfs_net_parse_redirect(const __u8 *body, size_t body_len,
                                       char *addr, size_t addr_cap,
                                       __u16 *port);
static int powerfs_conn_get_filer_idx(struct powerfs_net_server_conn *conn);

/* 红黑树辅助函数 (按 seq 组织请求, 用于 reply 匹配) */
static void powerfs_req_tree_insert(struct powerfs_net_server_conn *conn,
                                     struct powerfs_request *req);
static struct powerfs_request *
powerfs_req_tree_lookup(struct powerfs_net_server_conn *conn, __u32 seq)
    __maybe_unused;
static void powerfs_req_tree_remove(struct powerfs_net_server_conn *conn,
                                     struct powerfs_request *req);

/* === v2: 调度器 + sk 回调 (前向声明) === */
static int  pfs_rx_thread_fn(void *arg);
static int  pfs_tx_thread_fn(void *arg);
static void pfs_data_ready(struct sock *sk);
static void pfs_write_space(struct sock *sk);
static void pfs_state_change(struct sock *sk);
static void pfs_error_report(struct sock *sk);
static void pfs_rx_callback(struct powerfs_net_server_conn *conn);
static void pfs_tx_callback(struct powerfs_net_server_conn *conn);
static void pfs_conn_set_callbacks(struct powerfs_net_server_conn *conn);
static void pfs_conn_reset_callbacks(struct powerfs_net_server_conn *conn);
static void pfs_process_receive(struct powerfs_net_server_conn *conn);
static void pfs_process_transmit(struct powerfs_net_server_conn *conn);
static void pfs_tx_schedule(struct powerfs_net_server_conn *conn);
static void pfs_conn_remove_from_sched(struct powerfs_net_server_conn *conn);
/* v2 RX 非阻塞状态机 helpers */
static void pfs_rx_reset_partial(struct powerfs_net_server_conn *conn);
static int  pfs_conn_alloc_rxbuffers(struct powerfs_net_server_conn *conn);
static void pfs_conn_free_rxbuffers(struct powerfs_net_server_conn *conn);
static struct powerfs_net_sched *pfs_pick_sched(const char *addr);
static struct powerfs_net_sched *pfs_pick_vol_sched(const char *addr,
                                                    enum powerfs_net_server_type type);
static int  powerfs_sched_init(void);
static void powerfs_sched_exit(void);

/* disconnect_work: sk 回调检测断连后调度, 在 process context 执行清理 */
static void powerfs_conn_disconnect_work_fn(struct work_struct *work);

/* ========== CRC32C 实现 (软件) ========== */

/* CRC32C 查表 (Castagnoli 多项式) */
static __u32 crc32c_table[256];
static bool crc32c_table_init = false;

static void powerfs_crc32c_init_table(void)
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
 * powerfs_crc32c - 计算 CRC32C 校验值
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

/* ========== 帧头编解码 ========== */

/* 计算 route_hash: 高7位=client_id hash, 低1位=channel (防错乱校验).
 * 服务端收帧时校验 route_hash 是否匹配本连接, 防止帧串到错误连接. */
static inline __u8 pfs_route_hash(__u64 client_id, __u8 channel)
{
    __u64 h = client_id;
    h ^= h >> 32;
    h *= 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    return ((__u8)(h >> 25) << 1) | (channel & 0x01);
}

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

    /* 计算 header_crc (前 24 字节的 CRC32C) */
    crc = powerfs_crc32c((const __u8 *)hdr, 24);
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

    /* 验证 CRC */
    calc_crc = powerfs_crc32c(buf, 24);
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

/* ========== Socket 辅助函数 ========== */

/**
 * 创建 TCP 内核 socket
 */
static struct socket *powerfs_net_create_tcp_socket(void)
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
static int powerfs_net_tcp_connect(struct socket *sock, const char *addr,
                                    __u16 port)
{
    struct sockaddr_in sin;
    int ret;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = cpu_to_be16(port);

    /* 解析 IP 地址 (使用 in4_pton 兼容内核 6.2+) */
    ret = in4_pton(addr, -1, (void *)&sin.sin_addr, '\0', NULL);
    if (ret < 0) {
        pr_err("powerfs: invalid server address: %s\n", addr);
        return ret;
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
static void powerfs_net_close_socket(struct socket *sock)
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
static int pfs_frame_send_nonblock(struct socket *sock,
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
static int powerfs_net_do_handshake(struct socket *sock)
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
static int powerfs_conn_get_filer_idx(struct powerfs_net_server_conn *conn)
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
static int powerfs_conn_do_handshake(struct socket *sock,
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
static void powerfs_conn_release(struct kref *kref)
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
static struct powerfs_net_sched *pfs_pick_sched(const char *addr)
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
static struct powerfs_net_sched *pfs_pick_vol_sched(const char *addr,
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
static int powerfs_sched_init(void)
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
static void powerfs_sched_exit(void)
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
static void pfs_data_ready(struct sock *sk)
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
static void pfs_write_space(struct sock *sk)
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
static void pfs_state_change(struct sock *sk)
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
static void pfs_error_report(struct sock *sk)
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

/* RX 回调: 标记 rx_ready + 投递到 sched->rx_conns + 唤醒调度器 */
static void pfs_rx_callback(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
    if (!sched)
        return;

    spin_lock_bh(&sched->rx_lock);
    conn->rx_ready = 1;
    if (!conn->rx_scheduled) {
        list_add_tail(&conn->rx_list, &sched->rx_conns);
        conn->rx_scheduled = 1;
        powerfs_conn_get(conn);            /* 调度器持引用 (防收发中拆除) */
        wake_up(&sched->rx_waitq);
    }
    spin_unlock_bh(&sched->rx_lock);
}

/* TX 回调: 标记 tx_ready + 投递到 sched->tx_conns + 唤醒 TX 线程 */
static void pfs_tx_callback(struct powerfs_net_server_conn *conn)
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

/* 建连: 保存原始回调 + 安装自定义回调 (参照 Lustre ksocknal_lib_save_callback +
 * ksocknal_lib_set_callback, socklnd_lib.c:511-524) */
static void pfs_conn_set_callbacks(struct powerfs_net_server_conn *conn)
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
static void pfs_conn_reset_callbacks(struct powerfs_net_server_conn *conn)
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
static int pfs_rx_thread_fn(void *arg)
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
static int pfs_tx_thread_fn(void *arg)
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
static void pfs_rx_reset_partial(struct powerfs_net_server_conn *conn)
{
    conn->rx_hdr_got = 0;
    conn->rx_body_got = 0;
    conn->rx_data_got = 0;
    conn->rx_body_total = 0;
    conn->rx_data_total = 0;
    conn->rx_phase = 0;
}

/* 分配 per-conn RX buffer (filer/volume conn init 调用) */
static int pfs_conn_alloc_rxbuffers(struct powerfs_net_server_conn *conn)
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
static void pfs_conn_free_rxbuffers(struct powerfs_net_server_conn *conn)
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
static int pfs_rx_step(struct powerfs_net_server_conn *conn)
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
static int pfs_conn_flow_idx(struct powerfs_net_server_conn *conn)
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

/* 一帧完整后处理: NOTIFY 异步帧 或 按 seq 匹配 pending req + complete.
 * 锁外 memcpy 大块 READ 响应 (持 req_lock 会阻塞 do_send 入队). */
static void pfs_rx_dispatch(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_frame_hdr *hdr = &conn->rx_cur_hdr;
    void *body = conn->rx_body_buf;
    void *data = conn->rx_data_buf;
    size_t body_len = conn->rx_body_got;
    size_t data_len = conn->rx_data_got;
    struct powerfs_request *req = NULL;

    /* 异步通知帧 (seq=0 或 NOTIFY flag): invalidate 主动推送.
     * Filer 元数据变更后推 Invalidate(inode, version) 到所有订阅客户端. */
    if ((hdr->flags & POWERFS_NET_FLAG_NOTIFY) || hdr->seq == 0) {
        __u64 ino = 0;
        __u64 version = 0;

        pr_debug("powerfs: RX %s:%u: async notify seq=%u flags=0x%02x\n",
                 conn->addr, conn->port, hdr->seq, hdr->flags);

        if (body && body_len > 0) {
            struct powerfs_tlv_dec dec;
            powerfs_tlv_dec_init(&dec, body, body_len);
            powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_INO, &ino);
            powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_VERSION, &version);
        }

        if (ino != 0) {
            pr_debug("powerfs: invalidate ino=%llu version=%llu\n",
                    ino, version);
            /* powerfs_invalidate_one() now defers all work (inode lookup +
             * getattr + page cache invalidation) to powerfs_refresh_wq.
             * This is non-blocking and safe to call from the RX dispatcher. */
            powerfs_invalidate_one(ino);
        } else {
            pr_warn("powerfs: notify frame missing Ino field\n");
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
        u32 seq = req->seq;
        u16 msg_type = req->msg_type;
        u64 now_ns = ktime_get_ns();

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
         * 用本地变量, req 可能已被等待线程释放. */
        if (ts_submit) {
            u64 dur_us = div_u64(now_ns - ts_submit, 1000);
            if (dur_us > 100000)
                pr_info("powerfs: SLOW_RX seq=%u msg=0x%04x dur=%llums\n",
                        seq, msg_type, div_u64(dur_us, 1000));
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
static void pfs_process_receive(struct powerfs_net_server_conn *conn)
{
    int ret;
    int frames = 0;

    while (1) {
        ret = pfs_rx_step(conn);
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
            /* 当前没数据. 检查 skb_queue:
             *   非空 → 继续 (数据已在 queue, 重试 recv)
             *   空  → break, rx_ready 的最终决定交给 pfs_rx_thread_fn
             *         在 rx_lock 下做 (避免与 sk_data_ready 回调 race:
             *         无锁清 rx_ready=0 会 clobber 回调刚设的 rx_ready=1) */
            if (conn->sock && conn->sock->sk &&
                !skb_queue_empty(&conn->sock->sk->sk_receive_queue))
                continue;
            break;
        }
        /* EOF/RST/错误 → 断连, reset partial 状态 */
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
static void pfs_process_transmit(struct powerfs_net_server_conn *conn)
{
    struct powerfs_request *req = NULL;
    struct powerfs_net_frame_hdr hdr;
    struct socket *sock;
    int ret;

    sock = conn->sock;
    if (!sock) {
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

    ret = pfs_frame_send_nonblock(sock, &hdr,
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
    if (req->ts_submit) {
        u64 dur_us = div_u64(ktime_get_ns() - req->ts_submit, 1000);
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
static void pfs_tx_schedule(struct powerfs_net_server_conn *conn)
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
static void pfs_conn_remove_from_sched(struct powerfs_net_server_conn *conn)
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
static void powerfs_conn_disconnect_work_fn(struct work_struct *work)
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

static void powerfs_conn_reconnect_work_fn(struct work_struct *work)
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
         * (参照 Ceph con->delay). 成功连接时在 connect_one 中归零. */
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

/* === 6. 连接池 init/exit === */

int powerfs_conn_pool_init(const char *master_addr, __u16 master_port)
{
    int i, ret;

    /* 1. 存储 master 地址 (可选, 用于后续动态发现) */
    if (master_addr) {
        strncpy(g_pool.master_addr, master_addr, sizeof(g_pool.master_addr) - 1);
        g_pool.master_port = master_port;
        g_pool.master_set = true;
    }
    atomic_set(&g_pool.stopping, 0);

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

    /* 2. 初始化 shard 路由表 */
    spin_lock_init(&g_pool.shard_route.lock);
    for (i = 0; i < POWERFS_MAX_SHARDS; i++) {
        g_pool.shard_route.entries[i].leader_filer_idx = -1;
        g_pool.shard_route.entries[i].state = ROUTE_UNKNOWN;
        INIT_LIST_HEAD(&g_pool.shard_route.entries[i].pending_reqs);
        spin_lock_init(&g_pool.shard_route.entries[i].req_lock);
    }
    g_pool.shard_route.shard_count = shard_count;  /* 从模块参数获取 */

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
static void powerfs_req_timeout_fn(struct work_struct *work)
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

/* kref 释放回调: 引用计数归零时释放请求 (参照 Ceph ceph_osdc_release_request) */
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

/* === 红黑树辅助函数 (参照 Ceph: 按 seq 组织请求, O(log n) 查找) ===
 *
 * req_tree 按 seq 排序, 用于 reply 匹配: 收到响应时按 seq 快速定位请求.
 * 与 pending_reqs (链表, 按发送顺序) 互补.
 */

/* 插入请求到 conn->req_tree (按 seq) */
static void powerfs_req_tree_insert(struct powerfs_net_server_conn *conn,
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
static struct powerfs_request * __maybe_unused
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
static void powerfs_req_tree_remove(struct powerfs_net_server_conn *conn,
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
static int powerfs_request_do_send(struct powerfs_request *req,
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

        /* 计时日志: 定位 1MB writeback 慢的根因 */
        if (req->ts_submit) {
            u64 dur_us = div_u64(ktime_get_ns() - req->ts_submit, 1000);
            if (dur_us > 100000)  /* >100ms 的请求打 info 日志 */
                pr_info("powerfs: SLOW_REQ seq=%u msg=0x%04x dur=%llums conn=%s:%u\n",
                        seq, req->msg_type, div_u64(dur_us, 1000),
                        conn->addr, conn->port);
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
            break;

        case ROUTE_CHECKING:
        case ROUTE_UNKNOWN:
            /*
             * 寻找可用 filer, 优先跳过上次尝试的 (避免 self-redirect 循环).
             *
             * ROUTE_CHECKING: leader 可能已变, round-robin 尝试.
             * ROUTE_UNKNOWN: 无 leader 信息, 尝试任意已连接 filer.
             *
             * 两者的 filer 选择策略相同: 先跳过 last_tried_conn,
             * 若无其他选择则允许重试同一个.
             */
            if (g_pool.filer_count > 0) {
                int i;
                /* 第一轮: 跳过 last_tried_conn (避免 self-redirect 循环) */
                for (i = 0; i < g_pool.filer_count; i++) {
                    if (g_pool.filers[i].in_use &&
                        g_pool.filers[i].state == CONN_CONNECTED &&
                        &g_pool.filers[i] != last_tried_conn) {
                        conn = &g_pool.filers[i];
                        break;
                    }
                }
                /* 第二轮: 若无其他选择, 允许重试同一个 */
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
                pr_warn("powerfs: redirect to leader %s:%u (from %s:%u, filer_count=%d)\n",
                        leader_addr, leader_port, conn->addr, conn->port,
                        g_pool.filer_count);

                /* 检测 self-redirect (filer 选举中, 返回自身地址) */
                if (strcmp(leader_addr, conn->addr) == 0 &&
                    leader_port == conn->port) {
                    self_redirect = true;
                    pr_debug("powerfs: self-redirect detected on filer %s:%u (election in progress), switching to ROUTE_CHECKING\n",
                            conn->addr, conn->port);
                }

                if (!self_redirect) {
                    /* 查找或更新 filer 路由 */
                    leader_conn = powerfs_conn_find_filer(leader_addr, leader_port);
                    if (leader_conn) {
                        int new_idx = powerfs_conn_get_filer_idx(leader_conn);
                        if (new_idx >= 0) {
                            /* 更新 shard 路由到新 leader */
                            powerfs_shard_route_update(req->shard_id, new_idx);
                            /* 重试请求 (走新 leader) */
                            reinit_completion(&req->done);
                            req->error = 0;
                            continue;
                        }
                    }
                } else {
                    /*
                     * Self-redirect: filer 选举中, 不知道 leader.
                     * 使用指数退避重试, 避免快速循环 (22ms 一次会消耗大量 CPU).
                     * 退避时间: 200ms, 400ms, 800ms, 1600ms, 上限 2000ms.
                     * 每次重试会重新查路由, 若选举已完成则请求成功或收到
                     * 指向真正 leader 的 REDIRECT.
                     */
                    unsigned long backoff;

                    filer_idx = powerfs_conn_get_filer_idx(conn);
                    if (filer_idx >= 0)
                        powerfs_shard_route_on_filer_disconnect(filer_idx);

                    /* 指数退避: 200ms * 2^min(attempt, 4), 上限 2000ms */
                    backoff = min(200ul * (1UL << min(attempt, 4)),
                                  2000ul);
                    pr_debug("powerfs: self-redirect on filer %s:%u, backing off %lu ms (attempt %d)\n",
                            conn->addr, conn->port, backoff, attempt);

                    reinit_completion(&req->done);
                    req->error = 0;
                    msleep(backoff);
                    continue;
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
static int powerfs_net_parse_redirect(const __u8 *body, size_t body_len,
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

/* K3: 前向声明 — parse_file_layout 定义在 getattr 之后, 但 lookup 需先调用 */
static void parse_file_layout(struct powerfs_tlv_dec *dec,
                              struct powerfs_file_layout *layout);

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
static int parse_placement_field(const __u8 *val, size_t len,
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
static int parse_reliability_field(const __u8 *val, size_t len,
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
static int parse_replica_chunks_field(const __u8 *val, size_t len,
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
static void parse_file_layout(struct powerfs_tlv_dec *dec,
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

    /* K3-DEBUG: log parsed layout for diagnostics */
    pr_info("powerfs: parse_file_layout RESULT placement=%u reliability=%u chunk_size=%u "
            "has_placement=%d has_reliability=%d stripe_size=%llu stripe_count=%u "
            "volume_ids_count=%u inline_len=%u ec_data=%u ec_parity=%u replica_count=%u\n",
            layout->placement, layout->reliability, layout->chunk_size,
            layout->has_placement ? 1 : 0, layout->has_reliability ? 1 : 0,
            (unsigned long long)layout->stripe_size, layout->stripe_count,
            layout->volume_ids_count, layout->inline_len,
            layout->ec_data_shards, layout->ec_parity_shards, layout->replica_count);
}

int powerfs_net_getattr(__u64 ino, __u32 *mode, __u32 *uid, __u32 *gid,
                         __u64 *size, __u32 *nlink,
                         __u64 *mtime, __u64 *atime, __u64 *ctime,
                         __u64 *volume_id, __u64 *file_key,
                         struct powerfs_file_layout *layout)
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

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, old_dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, old_name, old_name_len);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_NEW_PARENT_INO, new_dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NEW_NAME, new_name, new_name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_RENAME, old_dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    NULL, NULL);
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
            put_unaligned_le64(0, p + 8);                            /* size */
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
static int powerfs_net_read_ec(struct powerfs_inode_info *pi, __u64 ino,
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
 * powerfs_net_statfs - 获取文件系统统计信息
 */
int powerfs_net_statfs(struct kstatfs *stats)
{
    size_t resp_body_len = 0;
    int ret;

    ret = powerfs_net_send_request(POWERFS_NET_MSG_STATFS, 0,
                                    NULL, 0,
                                    NULL, 0,
                                    (__u8 *)stats, sizeof(*stats),
                                    NULL, 0, 2000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

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

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_LINK, dir_ino,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0, POWERFS_META_TIMEOUT_MS,
                                    NULL, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}

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

/* ========== 初始化/清理 ========== */

/**
 * powerfs_net_init - 初始化 powerfs-net 子系统
 *
 * 仅初始化 CRC32C 表和 discover 序列号. 实际的 per-filer 连接池由
 * powerfs_conn_pool_init (在 fill_super 中) 创建, 由 powerfs_net_pool_init
 * 初始化 g_pool 基础字段 (servers[], pool_lock 等).
 */
int powerfs_net_init(void)
{
    /* 初始化 CRC32C 表 */
    if (!crc32c_table_init)
        powerfs_crc32c_init_table();

    atomic_set(&g_discover_seq, 0);

    g_initialized = true;

    pr_debug("powerfs: net subsystem initialized\n");
    return 0;
}

/**
 * powerfs_net_exit - 清理 powerfs-net 子系统
 *
 * 兜底清理: 若 umount 路径未执行 (直接 rmmod), 这里调用 pool_exit
 * 停止新架构连接池 (per-filer 连接 + 调度器线程).
 */
void powerfs_net_exit(void)
{
    if (!g_initialized)
        return;

    /* 清理连接池 (销毁 pool mutex)
     * 注意: kill_sb 已调用 pool_cleanup，但 pool_exit 还没调用；
     *       如果 umount 没执行（直接 rmmod），这里兜底 */
    powerfs_net_pool_exit();

    g_initialized = false;

    pr_debug("powerfs: net subsystem exited\n");
}

/* ========== 导出符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_net_is_connected);
EXPORT_SYMBOL_GPL(powerfs_net_recently_disconnected);
EXPORT_SYMBOL_GPL(powerfs_net_pick_timeout);
EXPORT_SYMBOL_GPL(powerfs_net_send_request);
EXPORT_SYMBOL_GPL(powerfs_net_lookup);
EXPORT_SYMBOL_GPL(powerfs_net_lookup_timeout);
EXPORT_SYMBOL_GPL(powerfs_net_getattr);
EXPORT_SYMBOL_GPL(powerfs_net_setattr);
EXPORT_SYMBOL_GPL(powerfs_net_create);
EXPORT_SYMBOL_GPL(powerfs_net_unlink);
EXPORT_SYMBOL_GPL(powerfs_net_rename);
EXPORT_SYMBOL_GPL(powerfs_net_readdir);
EXPORT_SYMBOL_GPL(powerfs_net_readdir_timeout);
EXPORT_SYMBOL_GPL(powerfs_net_read);
EXPORT_SYMBOL_GPL(powerfs_net_write);
EXPORT_SYMBOL_GPL(powerfs_net_statfs);
EXPORT_SYMBOL_GPL(powerfs_net_symlink);
EXPORT_SYMBOL_GPL(powerfs_net_readlink);
EXPORT_SYMBOL_GPL(powerfs_net_link);
EXPORT_SYMBOL_GPL(powerfs_net_ping);
EXPORT_SYMBOL_GPL(powerfs_net_init);
EXPORT_SYMBOL_GPL(powerfs_net_exit);

/* ========== 多连接池实现 ========== */

/* 全局连接池状态
 * (g_pool 已在文件顶部声明, 这里仅声明 g_pool_initialized) */
static bool g_pool_initialized = false;

/**
 * powerfs_net_pool_init - 初始化连接池
 */
int powerfs_net_pool_init(void)
{
    int i;

    /* v2: 防御性释放可能残留的调度器 (remount 场景: 若上次的 conn_pool_exit
     * 未执行, schedulers 可能仍分配). memset 会清零指针导致泄漏, 故先释放.
     * sched_exit 是幂等的 (schedulers==NULL 时 no-op). */
    powerfs_sched_exit();

    /* 初始化连接池 */
    memset(&g_pool, 0, sizeof(g_pool));
    mutex_init(&g_pool.pool_lock);
    atomic_set(&g_pool.active_filer_idx, 0);
    atomic_set(&g_pool.active_master_idx, 0);
    atomic_set(&g_pool.active_volume_idx, 0);
    atomic_set(&g_pool.leader_idx, -1);
    atomic_set(&g_pool.leader_known, 0);
    atomic_set(&g_pool.failover_count, 0);

    /* 初始化服务器条目 */
    for (i = 0; i < POWERFS_NET_MAX_SERVERS; i++) {
        memset(&g_pool.servers[i], 0, sizeof(g_pool.servers[i]));
        g_pool.servers[i].last_check_time = jiffies;
    }

    g_pool_initialized = true;

    pr_debug("powerfs: connection pool initialized\n");
    return 0;
}

/**
 * powerfs_net_pool_exit - 清理连接池
 */
void powerfs_net_pool_exit(void)
{
    if (!g_pool_initialized)
        return;

    mutex_destroy(&g_pool.pool_lock);
    g_pool_initialized = false;

    pr_debug("powerfs: connection pool exited\n");
}

/**
 * powerfs_net_set_stopping - 设置 stopping 标志, 让所有等待的 send_request
 * 立即返回 -ENOTCONN. 在 kill_sb 中网络清零前调用, 防止 reconnect_work
 * 访问已清零的 g_pool.
 */
void powerfs_net_set_stopping(void)
{
    /* 新架构: g_pool.stopping 让 powerfs_request_submit / per-conn
     * reconnect_work 立即停止.
     * kill_sb 在 sync_filesystem 之后调用本函数, 必须让新架构立即停止
     * 接收请求与重连, 否则在 conn_pool_exit 真正设置 g_pool.stopping
     * 之前的窗口内, reconnect_work 仍会尝试连接即将被清理的 filer. */
    atomic_set(&g_pool.stopping, 1);
}

bool powerfs_net_is_stopping(void)
{
    return atomic_read(&g_pool.stopping) != 0;
}

/**
 * powerfs_net_pool_cleanup - 清理连接池上所有资源
 *
 * 关闭所有活动连接，清理 delta 状态，重置服务器列表
 * 用于文件系统卸载时清理
 */
void powerfs_net_pool_cleanup(void)
{
    int i;

    if (!g_pool_initialized)
        return;

    /* 安全措施: 确保 stopping 已设置 (防止 reconnect_work 在清零后访问 g_pool) */
    atomic_set(&g_pool.stopping, 1);

    /* 清理新连接池 (per-filer 连接, 待处理请求) */
    powerfs_conn_pool_exit();

    mutex_lock(&g_pool.pool_lock);

    /* 重置所有服务器条目 */
    for (i = 0; i < POWERFS_NET_MAX_SERVERS; i++) {
        memset(&g_pool.servers[i], 0, sizeof(g_pool.servers[i]));
    }

    /* 重置计数 */
    g_pool.server_count = 0;
    g_pool.filer_count = 0;
    g_pool.master_count = 0;
    g_pool.volume_count = 0;

    atomic_set(&g_pool.active_filer_idx, 0);
    atomic_set(&g_pool.active_master_idx, 0);
    atomic_set(&g_pool.active_volume_idx, 0);
    atomic_set(&g_pool.leader_idx, -1);
    atomic_set(&g_pool.leader_known, 0);

    mutex_unlock(&g_pool.pool_lock);

    pr_debug("powerfs: connection pool cleaned up\n");
}

/**
 * powerfs_net_add_server - 添加服务器到池
 */
int powerfs_net_add_server(const char *addr, __u16 port,
                           enum powerfs_net_server_type type)
{
    int idx;

    if (!addr)
        return -EINVAL;

    mutex_lock(&g_pool.pool_lock);

    if (g_pool.server_count >= POWERFS_NET_MAX_SERVERS) {
        mutex_unlock(&g_pool.pool_lock);
        pr_err("powerfs: server pool full\n");
        return -ENOSPC;
    }

    idx = g_pool.server_count;
    strncpy(g_pool.servers[idx].addr, addr, sizeof(g_pool.servers[idx].addr) - 1);
    g_pool.servers[idx].port = port;
    g_pool.servers[idx].type = type;
    g_pool.servers[idx].is_leader = false;
    g_pool.servers[idx].last_check_time = jiffies;
    g_pool.server_count++;

    /* 更新类型计数 */
    switch (type) {
    case POWERFS_NET_SERVER_FILER:
        g_pool.filer_count++;
        break;
    case POWERFS_NET_SERVER_MASTER:
        g_pool.master_count++;
        break;
    case POWERFS_NET_SERVER_VOLUME:
    case POWERFS_NET_SERVER_VOLUME_META:
        g_pool.volume_count++;
        break;
    }

    mutex_unlock(&g_pool.pool_lock);

    pr_debug("powerfs: added server %s:%u (type=%d)\n", addr, port, type);
    return idx;
}

/**
 * powerfs_net_remove_server - 从池移除服务器
 */
int powerfs_net_remove_server(const char *addr, __u16 port)
{
    int i;
    int removed = -1;

    mutex_lock(&g_pool.pool_lock);

    for (i = 0; i < g_pool.server_count; i++) {
        if (strcmp(g_pool.servers[i].addr, addr) == 0 &&
            g_pool.servers[i].port == port) {
            /* 更新类型计数 */
            switch (g_pool.servers[i].type) {
            case POWERFS_NET_SERVER_FILER:
                g_pool.filer_count--;
                break;
            case POWERFS_NET_SERVER_MASTER:
                g_pool.master_count--;
                break;
            case POWERFS_NET_SERVER_VOLUME:
            case POWERFS_NET_SERVER_VOLUME_META:
                g_pool.volume_count--;
                break;
            }

            /* 移除服务器 */
            if (i < g_pool.server_count - 1) {
                memmove(&g_pool.servers[i], &g_pool.servers[i + 1],
                        (g_pool.server_count - i - 1) * sizeof(struct powerfs_net_server_entry));
            }
            g_pool.server_count--;
            removed = i;
            break;
        }
    }

    mutex_unlock(&g_pool.pool_lock);

    if (removed >= 0) {
        pr_debug("powerfs: removed server %s:%u\n", addr, port);
        return 0;
    }
    return -ENOENT;
}

/**
 * powerfs_net_set_primary - 设置主 Filer 地址 (兼容旧接口)
 */
int powerfs_net_set_primary(const char *addr, __u16 port)
{
    int ret;

    /* 如果已有服务器，移除第一个 Filer */
    mutex_lock(&g_pool.pool_lock);
    {
        int i;
        for (i = 0; i < g_pool.server_count; i++) {
            if (g_pool.servers[i].type == POWERFS_NET_SERVER_FILER) {
                /* 找到第一个 Filer，标记为 leader */
                g_pool.servers[i].is_leader = true;
                atomic_set(&g_pool.leader_idx, i);
                atomic_set(&g_pool.leader_known, 1);
                mutex_unlock(&g_pool.pool_lock);
                return 0;
            }
        }
    }
    mutex_unlock(&g_pool.pool_lock);

    /* 添加新 Filer */
    ret = powerfs_net_add_server(addr, port, POWERFS_NET_SERVER_FILER);
    if (ret >= 0) {
        /* 标记为 leader */
        mutex_lock(&g_pool.pool_lock);
        g_pool.servers[ret].is_leader = true;
        atomic_set(&g_pool.leader_idx, ret);
        atomic_set(&g_pool.leader_known, 1);
        mutex_unlock(&g_pool.pool_lock);
    }

    return ret >= 0 ? 0 : ret;
}

/**
 * powerfs_net_set_filers - 设置多个 Filer 地址
 */
int powerfs_net_set_filers(const char *addrs, const char *ports)
{
    char addr_buf[1024];
    char port_buf[512];
    char *addr_token, *port_token;
    int ret;
    int count = 0;

    if (!addrs || !ports)
        return -EINVAL;

    strncpy(addr_buf, addrs, sizeof(addr_buf) - 1);
    strncpy(port_buf, ports, sizeof(port_buf) - 1);
    addr_buf[sizeof(addr_buf) - 1] = '\0';
    port_buf[sizeof(port_buf) - 1] = '\0';

    {
        char *addr_save = addr_buf;
        char *port_save = port_buf;

        addr_token = strsep(&addr_save, ",");
        port_token = strsep(&port_save, ",");

        while (addr_token && port_token) {
            __u16 port = simple_strtoul(port_token, NULL, 10);

            /* 去除可能的空格 */
            while (*addr_token == ' ') addr_token++;
            while (*port_token == ' ') port_token++;

            if (*addr_token == '\0' || *port_token == '\0')
                break;

            ret = powerfs_net_add_server(addr_token, port, POWERFS_NET_SERVER_FILER);
            if (ret >= 0) {
                if (count == 0) {
                    /* 第一个 Filer 标记为 leader */
                    mutex_lock(&g_pool.pool_lock);
                    g_pool.servers[ret].is_leader = true;
                    atomic_set(&g_pool.leader_idx, ret);
                    atomic_set(&g_pool.leader_known, 1);
                    mutex_unlock(&g_pool.pool_lock);
                }
                count++;
            }

            addr_token = strsep(&addr_save, ",");
            port_token = strsep(&port_save, ",");
        }
    }

    pr_debug("powerfs: set %d filer addresses\n", count);
    return count > 0 ? 0 : -ENOENT;
}

/**
 * powerfs_net_set_master - 设置 Master 地址
 */
int powerfs_net_set_master(const char *addr, __u16 port)
{
    return powerfs_net_add_server(addr, port, POWERFS_NET_SERVER_MASTER);
}

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
static bool pfs_is_meta_msg(__u16 msg_type)
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
static int pfs_find_vol_conn_by_addr(const char *ip, __u16 port,
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
static int pfs_ensure_volume_conn(const char *ip, __u16 port,
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

/* ========== 导出新符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_net_pool_init);
EXPORT_SYMBOL_GPL(powerfs_net_pool_exit);
EXPORT_SYMBOL_GPL(powerfs_net_pool_cleanup);
EXPORT_SYMBOL_GPL(powerfs_net_add_server);
EXPORT_SYMBOL_GPL(powerfs_net_remove_server);
EXPORT_SYMBOL_GPL(powerfs_net_set_primary);
EXPORT_SYMBOL_GPL(powerfs_net_set_filers);
EXPORT_SYMBOL_GPL(powerfs_net_discover_filers);
EXPORT_SYMBOL_GPL(powerfs_net_set_master);
EXPORT_SYMBOL_GPL(powerfs_net_set_volume);
EXPORT_SYMBOL_GPL(powerfs_net_get_volume_count);
EXPORT_SYMBOL_GPL(powerfs_net_get_volume_conn);
EXPORT_SYMBOL_GPL(powerfs_net_find_volume_conn);
EXPORT_SYMBOL_GPL(powerfs_net_send_to_volume);
EXPORT_SYMBOL_GPL(powerfs_net_write_needle);
EXPORT_SYMBOL_GPL(powerfs_net_read_needle);
EXPORT_SYMBOL_GPL(powerfs_net_discover_volumes);
EXPORT_SYMBOL_GPL(powerfs_net_renew_lease);
