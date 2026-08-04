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

#include <net/sock.h>
#include <net/tcp.h>
#include <net/net_namespace.h>
#include <net/inet_sock.h>

#include "powerfs_net.h"
#include "powerfs_comm.h"

/* ========== 全局连接上下文 ========== */

static bool g_initialized = false;

/* 前向声明: g_pool 定义在后面 (多连接池实现段) */
static struct powerfs_net_pool g_pool;

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
static int  pfs_scheduler_thread(void *arg);
static int  pfs_sched_cansleep(struct powerfs_net_sched *sched);
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
static struct powerfs_net_sched *pfs_pick_sched(const char *addr);
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

/**
 * powerfs_net_frame_hdr_encode - 编码帧头 (28 字节)
 */
void powerfs_net_frame_hdr_encode(struct powerfs_net_frame_hdr *hdr,
                                   __u16 msg_type, __u8 flags,
                                   __u32 seq, __u16 status, __u32 data_len)
{
    __u32 crc_buf[7];  /* 前 24 字节 (6 u32) 的 CRC 计算缓冲 */
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

    /* data_len: little-endian */
    hdr->data_len = cpu_to_le32(data_len);

    /* reserved */
    memset(hdr->reserved, 0, sizeof(hdr->reserved));

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

    /* 设置 socket 选项 */
    sock->sk->sk_rcvtimeo = msecs_to_jiffies(POWERFS_NET_RECV_TIMEOUT);
    sock->sk->sk_sndtimeo = msecs_to_jiffies(POWERFS_NET_SEND_TIMEOUT);

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

    pr_info("powerfs: connected to %s:%u\n", addr, port);
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
    struct msghdr msg = {};
    size_t total_len;
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

    /* 发送 */
    sent = kernel_sendmsg(sock, &msg, vec, vec_count, total_len);
    if (sent < 0) {
        pr_err("powerfs: send failed: %zd\n", sent);
        return sent;
    }

    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_net_frame_send);

/**
 * powerfs_net_frame_recv - 接收一个完整帧
 *
 * 接收流程:
 *   1. 先接收 28 字节帧头
 *   2. 解析 data_len
 *   3. 接收 body (data_len 字节)
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
    size_t total_data;

    if (!sock)
        return -ENOTCONN;

    /* 设置接收超时 */
    if (timeout_ms > 0)
        sock->sk->sk_rcvtimeo = msecs_to_jiffies(timeout_ms);

    /* 1. 接收帧头 (28 字节) */
    vec.iov_base = hdr_buf;
    vec.iov_len = POWERFS_NET_FRAME_HDR_SIZE;

    received = kernel_recvmsg(sock, &msg, &vec, 1, POWERFS_NET_FRAME_HDR_SIZE, 0);
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

    /* 3. 接收 body + data */
    total_data = hdr->data_len;
    if (total_data == 0) {
        if (body_len) *body_len = 0;
        if (data_len) *data_len = 0;
        return 0;
    }

    if (total_data > body_cap + data_cap) {
        pr_err("powerfs: frame data too large: %zu > %zu\n",
               total_data, body_cap + data_cap);
        return -E2BIG;
    }

    /* 接收数据 */
    if (total_data <= body_cap) {
        /* 全部放入 body */
        vec.iov_base = body_buf;
        vec.iov_len = total_data;

        received = kernel_recvmsg(sock, &msg, &vec, 1, total_data, 0);
        if (received < 0)
            return received;
        if (body_len) *body_len = received;
        if (data_len) *data_len = 0;
    } else {
        /* body 填满，剩余放 data */
        size_t remaining;

        vec.iov_base = body_buf;
        vec.iov_len = body_cap;

        received = kernel_recvmsg(sock, &msg, &vec, 1, body_cap, 0);
        if (received < 0)
            return received;
        if (body_len) *body_len = received;

        remaining = total_data - body_cap;
        if (remaining > data_cap) {
            pr_err("powerfs: data section too large: %zu > %zu\n",
                   remaining, data_cap);
            return -E2BIG;
        }

        if (remaining > 0) {
            vec.iov_base = data_buf;
            vec.iov_len = remaining;

            received = kernel_recvmsg(sock, &msg, &vec, 1, remaining, 0);
            if (received < 0)
                return received;
        }
        if (data_len) *data_len = remaining;
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

    /* 构造握手请求 (18 字节，裸协议) */
    memcpy(req.magic, "PFSN", 4);
    req.version = POWERFS_NET_VERSION;
    req.client_type = POWERFS_NET_CLIENT_KERNEL;
    client_id = atomic_inc_return(&g_discover_seq) + 1000000;
    req.client_id = cpu_to_le64(client_id);
    req.features = 0;

    /* 发送裸握手请求 (18 字节) */
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

    pr_info("powerfs: handshake OK, server_id=%llu\n",
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

    /* 构造握手请求 (18 字节，裸协议) */
    memcpy(req.magic, "PFSN", 4);
    req.version = POWERFS_NET_VERSION;
    req.client_type = POWERFS_NET_CLIENT_KERNEL;
    client_id = atomic_read(&conn->seq_counter) + 1000000;
    req.client_id = cpu_to_le64(client_id);
    req.features = 0;

    /* 发送裸握手请求 (18 字节) */
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

    pr_info("powerfs: conn handshake OK, server_id=%llu\n",
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
            pr_info("powerfs: shard %d route -> CHECKING (filer %d down)\n",
                    i, filer_idx);
        }
    }
    spin_unlock(&g_pool.shard_route.lock);
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_on_filer_disconnect);

void powerfs_shard_route_on_filer_reconnect(int filer_idx)
{
    int i;

    if (filer_idx < 0 || filer_idx >= g_pool.filer_count)
        return;

    spin_lock(&g_pool.shard_route.lock);
    for (i = 0; i < POWERFS_MAX_SHARDS; i++) {
        if (g_pool.shard_route.entries[i].leader_filer_idx == filer_idx &&
            g_pool.shard_route.entries[i].state == ROUTE_UNKNOWN) {
            g_pool.shard_route.entries[i].state = ROUTE_CHECKING;
            pr_info("powerfs: shard %d route -> CHECKING (filer %d up)\n",
                    i, filer_idx);
        }
    }
    spin_unlock(&g_pool.shard_route.lock);
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

    pr_info("powerfs: shard %llu route -> VALID (filer %d)\n",
            (unsigned long long)shard_id, filer_idx);

    /* 若从不为 VALID 的状态切换到 VALID, 派发等待中的请求到新 leader */
    if (old_state != ROUTE_VALID)
        powerfs_shard_route_dispatch_pending(shard_id);
}
EXPORT_SYMBOL_GPL(powerfs_shard_route_update);

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
        pr_info("powerfs: filer %s:%u state %s -> %s\n",
                conn->addr, conn->port,
                powerfs_conn_state_str(old_state),
                powerfs_conn_state_str(new_state));

    filer_idx = powerfs_conn_get_filer_idx(conn);

    /* 事件传播 */
    if (new_state == CONN_RECONNECTING || new_state == CONN_FAULT) {
        if (filer_idx >= 0)
            powerfs_shard_route_on_filer_disconnect(filer_idx);
    } else if (new_state == CONN_CONNECTED && old_state == CONN_RECONNECTING) {
        if (filer_idx >= 0)
            powerfs_shard_route_on_filer_reconnect(filer_idx);
        /* 重连成功: 重置退避, 重发待重发请求 (参照 Ceph con_fault_finish) */
        conn->reconnect_delay = 0;
        powerfs_request_resend_pending(conn);
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
            complete(&req->done);
        }

        if (cancelled)
            pr_info("powerfs: cancelled pending requests on filer %s:%u FAULT\n",
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
 *   - sched->lock (spinlock_t) 保护 rx_conns/tx_conns: 回调投递和调度器消费
 *     都 spin_lock_bh
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

/* 按 addr hash 选调度器 (conn->sched = schedulers[hash % num_sched]) */
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

/* 初始化调度器数组: 按 num_online_cpus() 分配 + 启动每 CPU 一个调度器线程 */
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

        spin_lock_init(&sched->lock);
        INIT_LIST_HEAD(&sched->rx_conns);
        INIT_LIST_HEAD(&sched->tx_conns);
        init_waitqueue_head(&sched->waitq);
        sched->cpt = i;
        sched->task = kthread_run(pfs_scheduler_thread, sched,
                                  "pfs_sched/%d", i);
        if (IS_ERR(sched->task)) {
            ret = PTR_ERR(sched->task);
            pr_err("powerfs: failed to start scheduler %d: %d\n", i, ret);
            sched->task = NULL;
            /* 回滚已启动的线程 */
            atomic_set(&g_pool.stopping, 1);
            while (--i >= 0) {
                wake_up_all(&g_pool.schedulers[i].waitq);
                if (g_pool.schedulers[i].task) {
                    kthread_stop(g_pool.schedulers[i].task);
                    g_pool.schedulers[i].task = NULL;
                }
            }
            kfree(g_pool.schedulers);
            g_pool.schedulers = NULL;
            g_pool.num_sched = 0;
            atomic_set(&g_pool.stopping, 0);
            return ret;
        }
    }

    pr_info("powerfs: started %d scheduler threads (num_online_cpus=%d)\n",
            g_pool.num_sched, num_online_cpus());
    return 0;
}

/* 停止所有调度器线程 + 释放数组 (幂等, 重复调用安全) */
static void powerfs_sched_exit(void)
{
    int i;

    if (!g_pool.schedulers)
        return;

    /* 确保调度器线程看到 stopping 并退出 (kthread_stop 也会唤醒) */
    atomic_set(&g_pool.stopping, 1);
    for (i = 0; i < g_pool.num_sched; i++)
        wake_up_all(&g_pool.schedulers[i].waitq);

    for (i = 0; i < g_pool.num_sched; i++) {
        if (g_pool.schedulers[i].task) {
            kthread_stop(g_pool.schedulers[i].task);
            g_pool.schedulers[i].task = NULL;
        }
    }

    kfree(g_pool.schedulers);
    g_pool.schedulers = NULL;
    g_pool.num_sched = 0;
    pr_info("powerfs: scheduler threads stopped\n");
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
        if (sk->sk_state == TCP_CLOSE_WAIT || sk->sk_state == TCP_CLOSE)
            schedule_work(&conn->disconnect_work);   /* process context 清理 */
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
        schedule_work(&conn->disconnect_work);
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

    spin_lock_bh(&sched->lock);
    conn->rx_ready = 1;
    if (!conn->rx_scheduled) {
        list_add_tail(&conn->rx_list, &sched->rx_conns);
        conn->rx_scheduled = 1;
        powerfs_conn_get(conn);            /* 调度器持引用 (防收发中拆除) */
        wake_up(&sched->waitq);
    }
    spin_unlock_bh(&sched->lock);
}

/* TX 回调: 标记 tx_ready + 投递到 sched->tx_conns + 唤醒调度器 */
static void pfs_tx_callback(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
    if (!sched)
        return;

    spin_lock_bh(&sched->lock);
    conn->tx_ready = 1;
    if (!conn->tx_scheduled) {
        list_add_tail(&conn->tx_list, &sched->tx_conns);
        conn->tx_scheduled = 1;
        powerfs_conn_get(conn);
        wake_up(&sched->waitq);
    }
    spin_unlock_bh(&sched->lock);
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

static int pfs_sched_cansleep(struct powerfs_net_sched *sched)
{
    return list_empty(&sched->rx_conns) && list_empty(&sched->tx_conns);
}

static int pfs_scheduler_thread(void *arg)
{
    struct powerfs_net_sched *sched = arg;
    struct powerfs_net_server_conn *conn;

    spin_lock_bh(&sched->lock);

    while (!atomic_read(&g_pool.stopping) && !kthread_should_stop()) {
        bool did = false;

        /* 1. 收: 取 rx 就绪连接 */
        conn = list_first_entry_or_null(&sched->rx_conns,
                                        struct powerfs_net_server_conn,
                                        rx_list);
        if (conn) {
            list_del_init(&conn->rx_list);
            /* 清 rx_ready: 回调可在释放锁后随时再置位 */
            conn->rx_ready = 0;
            spin_unlock_bh(&sched->lock);

            pfs_process_receive(conn);   /* sock_recvmsg → 解帧 → seq 分发 → complete */

            spin_lock_bh(&sched->lock);
            /* 若收的过程中又有数据/缓冲区仍有数据, 重新挂回 */
            if (conn->rx_ready)
                list_add_tail(&conn->rx_list, &sched->rx_conns);
            else {
                conn->rx_scheduled = 0;
                powerfs_conn_put(conn);  /* 释放调度器引用 */
            }
            did = true;
        }

        /* 2. 发: 取 tx 就绪连接 */
        conn = list_first_entry_or_null(&sched->tx_conns,
                                        struct powerfs_net_server_conn,
                                        tx_list);
        if (conn) {
            list_del_init(&conn->tx_list);
            conn->tx_ready = 0;
            spin_unlock_bh(&sched->lock);

            pfs_process_transmit(conn);  /* 取 tx_queue → kernel_sendmsg */

            spin_lock_bh(&sched->lock);
            /* process_transmit 内部处理 EAGAIN: 若仍需发送且可写, 重新挂回 */
            if (conn->tx_ready)
                list_add_tail(&conn->tx_list, &sched->tx_conns);
            else {
                conn->tx_scheduled = 0;
                powerfs_conn_put(conn);
            }
            did = true;
        }

        /* 3. 无事可做 → 等待; 或 hogging CPU → cond_resched */
        if (!did) {
            spin_unlock_bh(&sched->lock);
            wait_event_interruptible(sched->waitq,
                !pfs_sched_cansleep(sched) ||
                atomic_read(&g_pool.stopping) ||
                kthread_should_stop());
            spin_lock_bh(&sched->lock);
        } else if (need_resched()) {
            spin_unlock_bh(&sched->lock);
            cond_resched();
            spin_lock_bh(&sched->lock);
        }
    }

    spin_unlock_bh(&sched->lock);
    return 0;
}

/* === 收: 单帧处理 (从 v1 rx_thread_fn 提取, 调度器调用) ===
 *
 * 调度器是 process context, 可 kmalloc(GFP_KERNEL). 每帧 kmalloc+free
 * (简单, 避免线程级缓存的生命周期管理). recv 用 sk_rcvtimeo (10s) 非阻塞.
 * recv 返回 <=0 (非 EAGAIN/EINTR) 时 schedule_work(disconnect_work) 并 return. */
static void pfs_process_receive(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_frame_hdr hdr;
    void *body = NULL;
    void *data_buf = NULL;
    size_t body_len = 0, data_len = 0;
    struct powerfs_request *req = NULL;
    int ret;

    body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    data_buf = kmalloc(POWERFS_NET_MAX_DATA, GFP_KERNEL);
    if (!body || !data_buf) {
        pr_err("powerfs: scheduler RX %s:%u OOM\n", conn->addr, conn->port);
        kfree(body);
        kfree(data_buf);
        schedule_work(&conn->disconnect_work);
        return;
    }

    /* sk_rcvtimeo=10s: 数据到达才被调度器调用, 正常立即返回.
     * EOF→-ECONNRESET, 超时→-EAGAIN, 错误→负值. */
    ret = powerfs_net_frame_recv(conn->sock, &hdr,
                                  body, POWERFS_NET_MAX_BODY, &body_len,
                                  data_buf, POWERFS_NET_MAX_DATA, &data_len,
                                  POWERFS_NET_RECV_TIMEOUT);
    if (ret == -EAGAIN || ret == -EINTR) {
        /* 超时/中断: 非断连. 检查 socket 缓冲区是否仍有数据 (回调可能已触发). */
        kfree(body);
        kfree(data_buf);
        if (conn->sock && conn->sock->sk &&
            !skb_queue_empty(&conn->sock->sk->sk_receive_queue))
            conn->rx_ready = 1;     /* 缓冲区仍有数据, 让调度器重新投递 */
        return;
    }
    if (ret < 0) {
        /* EOF/RST/keepalive失败/错误 → 断连 */
        pr_info("powerfs: scheduler RX %s:%u recv error %d, scheduling disconnect\n",
                conn->addr, conn->port, ret);
        kfree(body);
        kfree(data_buf);
        schedule_work(&conn->disconnect_work);
        return;
    }

    /* 异步通知帧 (seq=0 或 NOTIFY flag): invalidate 等主动推送.
     * TODO: 接入 invalidate 分发; 当前仅丢弃. */
    if ((hdr.flags & POWERFS_NET_FLAG_NOTIFY) || hdr.seq == 0) {
        pr_debug("powerfs: RX %s:%u: async notify seq=%u flags=0x%02x\n",
                 conn->addr, conn->port, hdr.seq, hdr.flags);
        kfree(body);
        kfree(data_buf);
        /* 通知帧处理后, 若缓冲区仍有数据, 标记 rx_ready 让调度器继续收 */
        if (conn->sock && conn->sock->sk &&
            !skb_queue_empty(&conn->sock->sk->sk_receive_queue))
            conn->rx_ready = 1;
        return;
    }

    /* 按 seq 查找请求并 complete (锁外 complete 避免锁序问题) */
    spin_lock(&conn->req_lock);
    req = powerfs_req_tree_lookup(conn, hdr.seq);
    if (req) {
        if (!list_empty(&req->list_node))
            list_del_init(&req->list_node);
        powerfs_req_tree_remove(conn, req);

        req->resp_status = hdr.status;
        req->error = 0;
        req->resp_body_len = 0;
        if (req->resp_body && body_len > 0) {
            size_t c = min(body_len, req->resp_body_cap);
            memcpy(req->resp_body, body, c);
            req->resp_body_len = c;
        }
        req->resp_data_len = 0;
        if (req->resp_data && data_len > 0) {
            size_t c = min(data_len, req->resp_data_cap);
            memcpy(req->resp_data, data_buf, c);
            req->resp_data_len = c;
        }
    }
    spin_unlock(&conn->req_lock);

    if (req) {
        complete(&req->done);
    } else {
        pr_debug("powerfs: RX %s:%u: no pending req for seq=%u\n",
                 conn->addr, conn->port, hdr.seq);
    }

    /* 收完一帧后, 若缓冲区仍有数据, 标记 rx_ready 让调度器继续收 */
    if (conn->sock && conn->sock->sk &&
        !skb_queue_empty(&conn->sock->sk->sk_receive_queue))
        conn->rx_ready = 1;

    kfree(body);
    kfree(data_buf);
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
        schedule_work(&conn->disconnect_work);
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
                                  req->req_body_len + req->req_data_len);

    ret = powerfs_net_frame_send(sock, &hdr,
                                  req->req_body, req->req_body_len,
                                  req->req_data, req->req_data_len);

    if (ret == -EAGAIN || ret == -ENOMEM) {
        /* 可写空间不足: 重挂回 tx_queue head, 等 write_space 回调.
         * 不设置 tx_ready (由 write_space 回调设置), 调度器会清 tx_scheduled. */
        spin_lock(&conn->tx_lock);
        list_add(&req->tx_list, &conn->tx_queue);
        spin_unlock(&conn->tx_lock);
        return;
    }
    if (ret < 0) {
        /* 发送失败: 摘除请求, complete -ENOTCONN, 触发断连 */
        pr_debug("powerfs: tx failed seq=%u msg_type=%u: %d\n",
                 req->seq, req->msg_type, ret);
        spin_lock(&conn->req_lock);
        if (!list_empty(&req->list_node)) {
            list_del_init(&req->list_node);
            powerfs_req_tree_remove(conn, req);
        }
        spin_unlock(&conn->req_lock);
        req->error = -ENOTCONN;
        complete(&req->done);
        schedule_work(&conn->disconnect_work);
        return;
    }

    /* 发送成功: req 留在 pending_reqs/req_tree 等响应.
     * 若 tx_queue 还有积压, 设置 tx_ready 让调度器重新投递. */
    spin_lock(&conn->tx_lock);
    if (!list_empty(&conn->tx_queue))
        conn->tx_ready = 1;
    spin_unlock(&conn->tx_lock);
}

/* do_send / 重发路径调用: 标记 tx_ready + 投递到 tx_conns + 唤醒调度器.
 * 与 pfs_tx_callback 的核心逻辑相同, 但可从 process context 调用. */
static void pfs_tx_schedule(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
    if (!sched)
        return;

    spin_lock_bh(&sched->lock);
    conn->tx_ready = 1;
    if (!conn->tx_scheduled) {
        list_add_tail(&conn->tx_list, &sched->tx_conns);
        conn->tx_scheduled = 1;
        powerfs_conn_get(conn);
        wake_up(&sched->waitq);
    }
    spin_unlock_bh(&sched->lock);
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

    spin_lock_bh(&sched->lock);
    if (conn->rx_scheduled && !list_empty(&conn->rx_list)) {
        list_del_init(&conn->rx_list);
        conn->rx_scheduled = 0;
        conn->rx_ready = 0;
        put_rx = true;
    }
    if (conn->tx_scheduled && !list_empty(&conn->tx_list)) {
        list_del_init(&conn->tx_list);
        conn->tx_scheduled = 0;
        conn->tx_ready = 0;
        put_tx = true;
    }
    spin_unlock_bh(&sched->lock);

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

    conn->sock = sock;
    powerfs_conn_set_state(conn, CONN_CONNECTED);
    conn->reconnect_count = 0;
    conn->reconnect_delay = 0;  /* 成功连接: 重置指数退避 */

    /* v2: 安装 sk 回调 (替换 v1 的 kthread_run(rx_thread)).
     * 必须在状态转为 CONNECTED 后安装, 回调依赖 conn->sock 稳定.
     * set_state(CONN_CONNECTED) 已触发 route 恢复 + resend_pending.
     * resend_pending 会把 needs_resend 请求重新入 tx_queue + pfs_tx_schedule,
     * 触发调度器首次发送; 同时 sk_data_ready 回调驱动接收. */
    pfs_conn_set_callbacks(conn);

    pr_info("powerfs: filer %s:%u connected (v2 scheduler)\n", conn->addr, conn->port);
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

    pr_info("powerfs: filer %s:%u state CONNECTED -> RECONNECTING (disconnect)\n",
            conn->addr, conn->port);

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

    /* 4. 标记在途请求重发 (置 needs_resend, 供重连后 resend_pending 使用).
     *    冗余于步骤5 (请求会被摘除), 但保持与 set_state(FAULT) 路径一致. */
    powerfs_request_mark_resend_on_conn(conn);

    /* 5. 唤醒在途请求的 submit: 以 -ENOTCONN complete, 让其立即重试其他 filer
     *    (不等本 conn 重连, 避免旧设计 30s 超时). 锁外 complete 避免锁序问题. */
    {
        struct powerfs_request *req, *tmp;
        LIST_HEAD(wake_list);
        spin_lock(&conn->req_lock);
        list_for_each_entry_safe(req, tmp, &conn->pending_reqs, list_node) {
            list_del_init(&req->list_node);
            powerfs_req_tree_remove(conn, req);
            req->error = -ENOTCONN;
            list_add_tail(&req->list_node, &wake_list);
        }
        spin_unlock(&conn->req_lock);
        list_for_each_entry_safe(req, tmp, &wake_list, list_node) {
            list_del_init(&req->list_node);
            complete(&req->done);
        }
    }

    /* 6. v2: 排空 tx_queue (调度器可能已取走部分, 剩余的以 -ENOTCONN complete).
     *    do_send 入 tx_queue 后若 disconnect, 请求可能仍在 tx_queue 未发送. */
    {
        struct powerfs_request *req, *tmp;
        LIST_HEAD(tx_drain);
        spin_lock(&conn->tx_lock);
        list_splice_init(&conn->tx_queue, &tx_drain);
        spin_unlock(&conn->tx_lock);
        list_for_each_entry_safe(req, tmp, &tx_drain, tx_list) {
            list_del_init(&req->tx_list);
            /* 已在步骤5 以 -ENOTCONN complete 的请求 (从 pending_reqs 摘除),
             * 不重复 complete; 仍在 pending_reqs 的请求此处也不重复.
             * 仅从 tx_queue 摘除即可, pending 侧已统一处理. */
        }
    }

    /* 7. v2: 退役 sock 前, 等待调度器放下 conn (kref refcount==1, 即只剩 owner 引用).
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

    /* 唤醒可能在等待重连完成的请求 (兼容旧 submit 路径, 新设计不再等待) */
    wake_up(&conn->reconnect_wq);

    pr_info("powerfs: filer %s:%u disconnected (v2 scheduler)\n", conn->addr, conn->port);

    /* 8. 调度重连 (stopping 时不调度, 由 pool_exit 主导清理) */
    if (!atomic_read(&g_pool.stopping))
        schedule_delayed_work(&conn->reconnect_work,
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
        pr_err("powerfs: filer %s:%u reconnect failed %d times, FAULT, will retry in 30s\n",
               conn->addr, conn->port, conn->reconnect_count);
        powerfs_conn_set_state(conn, CONN_FAULT);
        /* 30 秒后自动重试, 避免永久 FAULT.
         * 重置计数, 下次从 attempt 1 开始. */
        conn->reconnect_count = 0;
        conn->reconnect_delay = msecs_to_jiffies(30000);
        if (!atomic_read(&g_pool.stopping))
            schedule_delayed_work(&conn->reconnect_work,
                conn->reconnect_delay);
        return;
    }

    conn->reconnect_count++;
    pr_info("powerfs: reconnecting filer %s:%u (attempt %d/%d)\n",
            conn->addr, conn->port, conn->reconnect_count,
            POWERFS_NET_MAX_RECONNECT);

    ret = powerfs_conn_connect_one(conn);
    if (ret == 0) {
        pr_info("powerfs: filer %s:%u reconnected\n", conn->addr, conn->port);
    } else {
        /* 指数退避: BASE_DELAY * 2^(attempt-1), 上限 MAX_DELAY
         * (参照 Ceph con->delay). 成功连接时在 connect_one 中归零. */
        if (conn->reconnect_delay == 0)
            conn->reconnect_delay = msecs_to_jiffies(POWERFS_NET_BASE_DELAY);
        else
            conn->reconnect_delay = min(conn->reconnect_delay * 2,
                                         msecs_to_jiffies(POWERFS_NET_MAX_DELAY));

        if (!atomic_read(&g_pool.stopping))
            schedule_delayed_work(&conn->reconnect_work,
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
    g_pool.shard_route.shard_count = 1;  /* 默认, 从 Filer 获取后更新 */

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

        g_pool.filer_count++;
    }

    pr_info("powerfs: connection pool: %d filers\n", g_pool.filer_count);

    /* 4. 并行连接所有 filer */
    for (i = 0; i < g_pool.filer_count; i++) {
        schedule_delayed_work(&g_pool.filers[i].reconnect_work, 0);
    }

    /* 5. 等待至少一个 filer 连接成功 (30s 超时) */
    {
        int max_wait = 300;  /* 30s, 每 100ms 检查一次 */
        while (max_wait-- > 0) {
            for (i = 0; i < g_pool.filer_count; i++) {
                if (g_pool.filers[i].state == CONN_CONNECTED) {
                    pr_info("powerfs: at least one filer connected\n");
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

    /* 断开 volume 连接 (volume conn 未初始化为完整连接, in_use 通常为 false) */
    for (i = 0; i < g_pool.volume_count; i++) {
        struct powerfs_net_server_conn *conn = &g_pool.volumes[i];
        if (!conn->in_use)
            continue;

        cancel_delayed_work_sync(&conn->reconnect_work);
        powerfs_conn_disconnect_one(conn);
        cancel_work_sync(&conn->disconnect_work);
        conn->in_use = false;
    }

    g_pool.filer_count = 0;
    g_pool.volume_count = 0;

    /* v2: 所有连接已断开 (调度器不再访问 conn), 停止调度器线程.
     * 放在此处而非 pool_exit: 确保 disconnect_one 的 kref wait 不会因
     * 调度器线程已退出而死等 (调度器线程退出前会 put 完所有在飞引用). */
    powerfs_sched_exit();
}
EXPORT_SYMBOL_GPL(powerfs_conn_pool_exit);

/* ========== 请求对象生命周期 (Phase 1: 新架构) ========== */

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

/*
 * 断连时标记重发 (参照 Ceph con_fault: 不取消, 标记 r_needs_resend)
 *
 * 关键设计变更 (cancel → resend):
 *   OLD: disconnect → error=-ENOTCONN → complete → caller retries
 *   NEW: disconnect → mark needs_resend → reconnect → resend_pending → caller gets result
 *
 * 等待 completion 的调用者不会被唤醒, 继续等待直到:
 *   1. 重发成功 → complete 成功
 *   2. 重发超过 MAX_ATTEMPTS → complete -ETIMEDOUT
 *   3. Filer 进入 FAULT → complete -ENOTCONN
 *   4. deadline 到期 → complete -ETIMEDOUT
 */
void powerfs_request_mark_resend_on_conn(struct powerfs_net_server_conn *conn)
{
    struct powerfs_request *req;

    if (!conn || !conn->in_use)
        return;

    spin_lock(&conn->req_lock);
    list_for_each_entry(req, &conn->pending_reqs, list_node) {
        req->needs_resend = true;
        pr_debug("powerfs: marking req seq=%u msg_type=%u for resend on filer %s:%u\n",
                 req->seq, req->msg_type, conn->addr, conn->port);
    }
    spin_unlock(&conn->req_lock);

    pr_info("powerfs: marked pending requests for resend on filer %s:%u\n",
            conn->addr, conn->port);
}
EXPORT_SYMBOL_GPL(powerfs_request_mark_resend_on_conn);

/*
 * 重连成功后重发待重发请求 (参照 Ceph con_fault_finish)
 *
 * v2: 遍历 pending_reqs, 对 needs_resend=true 的请求重新分配 seq,
 * 入 tx_queue + pfs_tx_schedule (调度器异步发送).
 * 超过 MAX_ATTEMPTS 的请求标记 -ETIMEDOUT 并完成.
 */
void powerfs_request_resend_pending(struct powerfs_net_server_conn *conn)
{
    struct powerfs_request *req, *tmp;
    LIST_HEAD(resend_list);
    bool has_resend = false;

    if (!conn || !conn->in_use)
        return;
    if (conn->state != CONN_CONNECTED)
        return;

    /* 第一遍: 在 req_lock 下处理 needs_resend 标记 + 超过 MAX_ATTEMPTS 的请求.
     * 需重发的请求先收集到 resend_list (锁外再入 tx_queue, 避免持 req_lock 时
     * 取 tx_lock 导致锁序问题). */
    spin_lock(&conn->req_lock);
    list_for_each_entry_safe(req, tmp, &conn->pending_reqs, list_node) {
        if (!req->needs_resend)
            continue;
        req->needs_resend = false;
        req->attempts++;
        if (req->attempts > POWERFS_REQ_MAX_ATTEMPTS) {
            /* 超过最大重试次数: 放弃 */
            pr_warn("powerfs: req seq=%u msg_type=%u exceeded max attempts (%d)\n",
                    req->seq, req->msg_type, POWERFS_REQ_MAX_ATTEMPTS);
            list_del_init(&req->list_node);
            powerfs_req_tree_remove(conn, req);
            req->error = -ETIMEDOUT;
            /* 在锁外 complete 避免锁序问题: 先移到临时变量, 循环外 complete */
            spin_unlock(&conn->req_lock);
            complete(&req->done);
            spin_lock(&conn->req_lock);
        } else {
            /* v2: 重新分配 seq (旧 seq 已作废), 保留在 pending_reqs/req_tree.
             * 先从 req_tree 摘除 (旧 seq), 入 resend_list 待锁外重新入树 + tx_queue. */
            powerfs_req_tree_remove(conn, req);
            list_del_init(&req->list_node);   /* 临时摘除, 锁外重新挂回 */
            list_add_tail(&req->list_node, &resend_list);
            has_resend = true;
            pr_debug("powerfs: resend req msg_type=%u attempt=%d on filer %s:%u\n",
                     req->msg_type, req->attempts, conn->addr, conn->port);
        }
    }
    spin_unlock(&conn->req_lock);

    /* 第二遍: 锁外为每个重发请求分配新 seq, 重新入 pending_reqs/req_tree/tx_queue.
     * reinit_completion: 请求之前可能被 disconnect 以 -ENOTCONN complete 过,
     * 重发前需重置 completion 状态 (submit 在 wait_for_completion 上等待). */
    list_for_each_entry_safe(req, tmp, &resend_list, list_node) {
        __u32 new_seq;

        list_del_init(&req->list_node);
        reinit_completion(&req->done);
        req->error = 0;
        req->resp_status = 0;
        req->resp_body_len = 0;
        req->resp_data_len = 0;

        new_seq = atomic_inc_return(&conn->seq_counter);
        req->seq = new_seq;

        spin_lock(&conn->req_lock);
        list_add_tail(&req->list_node, &conn->pending_reqs);
        powerfs_req_tree_insert(conn, req);
        spin_unlock(&conn->req_lock);

        spin_lock(&conn->tx_lock);
        list_add_tail(&req->tx_list, &conn->tx_queue);
        spin_unlock(&conn->tx_lock);
    }

    /* 唤醒调度器发送重发请求 (一次性投递, 调度器会批量处理 tx_queue) */
    if (has_resend)
        pfs_tx_schedule(conn);

    pr_info("powerfs: resend pending requests on filer %s:%u\n",
            conn->addr, conn->port);
}
EXPORT_SYMBOL_GPL(powerfs_request_resend_pending);

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

    pr_info("powerfs: dispatching %d pending requests for shard %llu to filer %s:%u\n",
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
        complete(&req->done);
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
 *   注: 本函数不调用 complete(&req->done); 由调度器/disconnect_one/FAULT 完成.
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

    /* 等待调度器 complete (异步收发, 流水线).
     * 超时 3x RECV_TIMEOUT (30s): 兜底防止调度器异常未 complete.
     * 正常情况下调度器 RX 收到响应即 complete, 或 disconnect_one/FAULT 以 -ENOTCONN complete. */
    {
        long wr = wait_for_completion_timeout(&req->done,
                    msecs_to_jiffies(POWERFS_NET_RECV_TIMEOUT * 3));

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
            complete(&req->done);
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
                complete(&req->done);
                return -ENOTCONN;
            }

            entry = &g_pool.shard_route.entries[req->shard_id];
            spin_lock(&entry->req_lock);
            list_add_tail(&req->list_node, &entry->pending_reqs);
            spin_unlock(&entry->req_lock);

            pr_info("powerfs: no filer available for shard %llu, queueing req msg_type=%u\n",
                    (unsigned long long)req->shard_id, req->msg_type);

            /* 等待派发 (route_update -> dispatch_pending) 或超时 */
            reinit_completion(&req->done);
            wait_ret = wait_for_completion_timeout(&req->done,
                deadline - jiffies);

            /* 从 pending 队列移除 (如果还在) */
            spin_lock(&entry->req_lock);
            if (!list_empty(&req->list_node))
                list_del_init(&req->list_node);
            spin_unlock(&entry->req_lock);

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
        ret = powerfs_request_do_send(req, conn);

        if (ret == -ENOTCONN) {
            /* 连接断开: route 已降级 (disconnect_one 设了 RECONNECTING,
             * 触发 shard_route_on_filer_disconnect → CHECKING).
             * 立即回到循环开头重新查路由, 选其他 filer, 不等本 conn 重连
             * (避免旧设计 30s 超时). 加重试上限避免死循环. */
            if (attempt >= POWERFS_REQ_MAX_ATTEMPTS) {
                pr_warn("powerfs: req msg_type=%u exhausted %d attempts after -ENOTCONN\n",
                        req->msg_type, POWERFS_REQ_MAX_ATTEMPTS);
                req->error = -ENOTCONN;
                return -ENOTCONN;
            }
            if (atomic_read(&g_pool.stopping)) {
                req->error = -ENOTCONN;
                return -ENOTCONN;
            }
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
                pr_info("powerfs: redirect to leader %s:%u\n",
                        leader_addr, leader_port);

                /* 检测 self-redirect (filer 选举中, 返回自身地址) */
                if (strcmp(leader_addr, conn->addr) == 0 &&
                    leader_port == conn->port) {
                    self_redirect = true;
                    pr_info("powerfs: self-redirect detected on filer %s:%u (election in progress), switching to ROUTE_CHECKING\n",
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
                    pr_info("powerfs: self-redirect on filer %s:%u, backing off %lu ms (attempt %d)\n",
                            conn->addr, conn->port, backoff, attempt);

                    reinit_completion(&req->done);
                    req->error = 0;
                    msleep(backoff);
                    continue;
                }
            }
            /* redirect 解析失败或 filer 未找到 */
            pr_warn("powerfs: redirect failed, no leader filer found\n");
            req->error = -EAGAIN;
            complete(&req->done);
            return -EAGAIN;
        }

        /* 非 REDIRECT: 正常完成 (do_send 已 complete) */
        return req->error;
    }

    /* 不会到达 */
    req->error = -EAGAIN;
    complete(&req->done);
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
 *   4. 断连自动标记 needs_resend, 重连后由调度器重发
 *
 * @resp_body_len_out: 输出: 实际接收到的 body 长度 (可为 NULL)
 * @resp_data_len_out: 输出: 实际接收到的 data 长度 (可为 NULL)
 *
 * 返回值:
 *   >= 0: 成功 (0 = OK, >0 = powerfs-net 状态码)
 *   < 0: 错误 (-errno)
 */
int powerfs_net_send_request(__u16 msg_type,
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
    req->shard_id = 0;  /* 默认 shard, 后续按 inode 计算 */
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
static int net_status_to_errno(__u16 status)
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
 * powerfs_net_lookup - 查找文件 (返回完整属性含时间戳)
 *
 * Filer 响应 TLV 字段: Ino, Mode, Uid, Gid, Size, Nlink, Mtime, Atime, Ctime, Name
 */
int powerfs_net_lookup(__u64 dir_ino, const char *name, size_t name_len,
                       __u64 *ino, __u32 *mode, __u32 *uid, __u32 *gid,
                       __u64 *size, __u32 *nlink,
                       __u64 *mtime, __u64 *atime, __u64 *ctime)
{
    __u8 body[256];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[512];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    /* 编码请求: ParentIno + Name */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_LOOKUP,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 10000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

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
    }

    return 0;
}

/**
 * powerfs_net_getattr - 获取文件属性 (返回完整属性含时间戳)
 *
 * Filer 响应 TLV 字段: Ino, Mode, Uid, Gid, Size, Nlink, Mtime, Atime, Ctime
 */
int powerfs_net_getattr(__u64 ino, __u32 *mode, __u32 *uid, __u32 *gid,
                         __u64 *size, __u32 *nlink,
                         __u64 *mtime, __u64 *atime, __u64 *ctime)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[256];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_GETATTR,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 10000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_MODE, mode);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_UID, uid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_GID, gid);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, size);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_NLINK, nlink);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_MTIME, mtime);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_ATIME, atime);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_CTIME, ctime);
    }

    return 0;
}

/**
 * powerfs_net_create - 创建文件或目录
 */
int powerfs_net_create(__u64 dir_ino, const char *name, size_t name_len,
                        __u32 mode, __u32 uid, __u32 gid, bool is_dir,
                        __u64 *ino_ret)
{
    __u8 body[256];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[128];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    __u16 msg_type;
    int ret;

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

    ret = powerfs_net_send_request(msg_type,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 10000,
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
 * powerfs_net_unlink - 删除文件或目录
 */
int powerfs_net_unlink(__u64 dir_ino, const char *name, size_t name_len,
                       bool is_dir)
{
    __u8 body[256];
    struct powerfs_tlv_enc enc;
    __u16 msg_type;
    int ret;

    msg_type = is_dir ? POWERFS_NET_MSG_RMDIR : POWERFS_NET_MSG_UNLINK;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    ret = powerfs_net_send_request(msg_type,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0, 10000,
                                    NULL, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

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

    ret = powerfs_net_send_request(POWERFS_NET_MSG_RENAME,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0, 5000,
                                    NULL, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    return 0;
}

/**
 * powerfs_net_readdir - 读取目录项 (匹配 Filer 协议)
 *
 * 请求 TLV 字段: ParentIno, Limit, LastName (分页游标)
 * 响应 TLV 字段: Count, HasMore, Entry[] (每个 Entry 是嵌套 TLV)
 *
 * 每个 Entry 嵌套字段: Ino, Name, Mode, Uid, Gid, Size, Atime, Mtime, Ctime, Nlink
 */
int powerfs_net_readdir(__u64 dir_ino, const char *last_name, __u64 limit,
                        struct powerfs_net_dir_entry *entries, __u32 max_entries,
                        __u32 *actual_count, bool *has_more)
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

    *actual_count = 0;
    *has_more = false;

    /* 动态分配响应缓冲 (避免栈溢出) */
    resp_body = kmalloc(16384, GFP_KERNEL);
    if (!resp_body)
        return -ENOMEM;

    /* 编码请求: ParentIno + Limit + LastName */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_LIMIT, limit);
    if (last_name && last_name[0])
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_LAST_NAME,
                               last_name, strlen(last_name));

    ret = powerfs_net_send_request(POWERFS_NET_MSG_READDIR,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, 16384,
                                    NULL, 0, 5000,
                                    &resp_body_len, NULL);
    if (ret < 0) {
        kfree(resp_body);
        return ret;
    }
    if (ret > 0) {
        kfree(resp_body);
        return net_status_to_errno((__u16)ret);
    }

    if (resp_body_len == 0) {
        kfree(resp_body);
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

    kfree(resp_body);
    return 0;
}

/**
 * powerfs_net_read - 读数据
 *
 * Filer 的 Read 响应将文件内容作为裸 payload 发送 (TLV body 为空,
 * data 段为文件字节). 帧头 data_len = body.len() + data.len() = 文件字节数.
 *
 * 但内核 powerfs_net_frame_recv 按 body_cap(64KB) 机械切分 payload:
 *   - payload <= 64KB: 全部放入 tmp_body, tmp_data_len=0
 *   - payload >  64KB: 前 64KB 入 tmp_body, 余下入 tmp_data
 *
 * 当前 read 调用方 (powerfs_read_folio) 每次读 folio_size (4KB),
 * payload 必然 <= 64KB, 故全部数据落在 resp_body. 之前代码误将 buf
 * 作为 resp_data 传入, 导致 tmp_body 的数据被丢弃, read_len 恒为 0,
 * folio 被零填充, 表现为 "remount 后 cat 文件内容为空".
 *
 * 修复: 将 buf 作为 resp_body 传入, resp_data 留空 (folio 读不会溢出).
 */
int powerfs_net_read(__u64 ino, __u64 offset, __u32 length,
                     __u8 *buf, size_t buf_cap, __u32 *read_len)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    size_t resp_body_len = 0;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, offset);
    powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_DATA_LEN, length);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_READ,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    buf, buf_cap,
                                    NULL, 0, 10000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    if (read_len)
        *read_len = (__u32)resp_body_len;

    return 0;
}

/**
 * powerfs_net_write - 写数据
 */
int powerfs_net_write(__u64 ino, __u64 offset, const __u8 *data, size_t data_len,
                      __u32 *written)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[32];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, offset);
    powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_DATA_LEN, data_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_WRITE,
                                    body, powerfs_tlv_enc_len(&enc),
                                    data, data_len,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 10000,
                                    &resp_body_len, NULL);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    /* 解析响应获取已写字节数 */
    if (written && resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_DATA_LEN, written);
    }

    return 0;
}

/**
 * powerfs_net_setattr - 设置文件属性
 */
int powerfs_net_setattr(__u64 ino, __u32 mode_valid, __u32 mode,
                        __u32 uid, __u32 gid, __u64 size)
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

    ret = powerfs_net_send_request(POWERFS_NET_MSG_SETATTR,
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

    ret = powerfs_net_send_request(POWERFS_NET_MSG_STATFS,
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

    ret = powerfs_net_send_request(POWERFS_NET_MSG_SYMLINK,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 10000,
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

    ret = powerfs_net_send_request(POWERFS_NET_MSG_READLINK,
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
    __u8 body[256];
    struct powerfs_tlv_enc enc;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_LINK,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0, 10000,
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

    ret = powerfs_net_send_request(POWERFS_NET_MSG_PING,
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

    pr_info("powerfs: net subsystem initialized\n");
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

    pr_info("powerfs: net subsystem exited\n");
}

/* ========== 导出符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_net_is_connected);
EXPORT_SYMBOL_GPL(powerfs_net_send_request);
EXPORT_SYMBOL_GPL(powerfs_net_lookup);
EXPORT_SYMBOL_GPL(powerfs_net_getattr);
EXPORT_SYMBOL_GPL(powerfs_net_setattr);
EXPORT_SYMBOL_GPL(powerfs_net_create);
EXPORT_SYMBOL_GPL(powerfs_net_unlink);
EXPORT_SYMBOL_GPL(powerfs_net_rename);
EXPORT_SYMBOL_GPL(powerfs_net_readdir);
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

    pr_info("powerfs: connection pool initialized\n");
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

    pr_info("powerfs: connection pool exited\n");
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

    pr_info("powerfs: connection pool cleaned up\n");
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
        g_pool.volume_count++;
        break;
    }

    mutex_unlock(&g_pool.pool_lock);

    pr_info("powerfs: added server %s:%u (type=%d)\n", addr, port, type);
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
        pr_info("powerfs: removed server %s:%u\n", addr, port);
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

    pr_info("powerfs: set %d filer addresses\n", count);
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

        pr_info("powerfs: trying master %s:%u for filer discovery\n",
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
                                      seq, 0, 0);

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
                pr_info("powerfs: master redirect to %s\n",
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
                                              seq, 0, 0);

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

            pr_info("powerfs: master returned %llu filers\n",
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

                if (!healthy) {
                    pr_info("powerfs: skipping unhealthy filer %s:%llu\n",
                            faddr, (u64)fport);
                    continue;
                }

                powerfs_net_add_server(faddr, (__u16)fport,
                                       POWERFS_NET_SERVER_FILER);
                pr_info("powerfs: discovered filer %s:%llu\n",
                        faddr, (u64)fport);
                filers_added++;
            }
        }

        /* Success - don't try other masters */
        break;
    }

    kfree(resp_body);

    pr_info("powerfs: filer discovery complete, %d filers added\n",
            filers_added);
    return filers_added > 0 ? filers_added : -ENOTCONN;
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
