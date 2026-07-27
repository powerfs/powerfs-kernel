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
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/hashtable.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/inet.h>
#include <linux/statfs.h>

#include <net/sock.h>
#include <net/tcp.h>
#include <net/net_namespace.h>
#include <net/inet_sock.h>

#include "powerfs_net.h"
#include "powerfs_comm.h"

/* ========== 全局连接上下文 ========== */

static struct powerfs_net_conn g_conn;
static bool g_initialized = false;

/* 目标服务地址 (模块参数) */
static char *g_server_addr = "127.0.0.1";
static __u16 g_server_port = 9000;

module_param(g_server_addr, charp, 0644);
module_param(g_server_port, ushort, 0644);

/* ========== 前向声明 ========== */
static void powerfs_net_monitor_work_func(struct work_struct *work);
static void powerfs_net_leader_check_work_func(struct work_struct *work);

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

    /* 填充字段 */
    hdr->magic = (__be32)POWERFS_NET_MAGIC;
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
        pr_err("powerfs: recv header truncated: %zd < %d\n",
               received, POWERFS_NET_FRAME_HDR_SIZE);
        return -EIO;
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

/* ========== 握手 ========== */

/**
 * powerfs_net_do_handshake - 与 Filer 握手 (裸 18 字节协议，不含帧头)
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
    client_id = atomic_read(&g_conn.seq_counter) + 1000000;
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

    g_conn.server_id = le64_to_cpu(resp.server_id);
    g_conn.server_features = le32_to_cpu(resp.features);

    return 0;
}

/* ========== 连接管理 ========== */

/**
 * powerfs_net_connect - 建立到 Filer 的 TCP 连接
 */
int powerfs_net_connect(const char *addr, __u16 port)
{
    struct socket *sock;
    int ret;

    mutex_lock(&g_conn.conn_lock);

    if (g_conn.state == POWERFS_NET_STATE_CONNECTED) {
        mutex_unlock(&g_conn.conn_lock);
        return 0;
    }

    /* 清理旧连接 */
    if (g_conn.sock) {
        powerfs_net_close_socket(g_conn.sock);
        g_conn.sock = NULL;
    }

    g_conn.state = POWERFS_NET_STATE_CONNECTING;

    /* 创建 socket */
    sock = powerfs_net_create_tcp_socket();
    if (!sock) {
        g_conn.state = POWERFS_NET_STATE_ERROR;
        mutex_unlock(&g_conn.conn_lock);
        return -ENOMEM;
    }

    /* 建立 TCP 连接 */
    ret = powerfs_net_tcp_connect(sock, addr, port);
    if (ret < 0) {
        powerfs_net_close_socket(sock);
        g_conn.state = POWERFS_NET_STATE_ERROR;
        mutex_unlock(&g_conn.conn_lock);
        return ret;
    }

    g_conn.sock = sock;

    /* 执行握手 */
    g_conn.state = POWERFS_NET_STATE_HANDSHAKING;
    ret = powerfs_net_do_handshake(sock);
    if (ret < 0) {
        powerfs_net_close_socket(sock);
        g_conn.sock = NULL;
        g_conn.state = POWERFS_NET_STATE_ERROR;
        mutex_unlock(&g_conn.conn_lock);
        return ret;
    }

    g_conn.state = POWERFS_NET_STATE_CONNECTED;
    g_conn.reconnect_count = 0;

    pr_info("powerfs: net connected to %s:%u\n", addr, port);

    mutex_unlock(&g_conn.conn_lock);
    return 0;
}

/**
 * powerfs_net_disconnect - 断开连接
 */
void powerfs_net_disconnect(void)
{
    mutex_lock(&g_conn.conn_lock);

    if (g_conn.sock) {
        powerfs_net_close_socket(g_conn.sock);
        g_conn.sock = NULL;
    }

    g_conn.state = POWERFS_NET_STATE_DISCONNECTED;

    /* 完成所有待处理请求 (返回错误) */
    pr_info("powerfs: net disconnected\n");

    mutex_unlock(&g_conn.conn_lock);
}

/**
 * powerfs_net_is_connected - 检查连接状态
 */
bool powerfs_net_is_connected(void)
{
    return g_conn.state == POWERFS_NET_STATE_CONNECTED;
}

/* ========== 请求/响应核心 ========== */

/**
 * powerfs_net_send_request - 发送同步请求并等待响应
 *
 * 实现真正的同步发送-接收流程:
 *   1. 编码并发送请求帧
 *   2. 同步接收响应帧
 *   3. 拷贝响应数据到输出缓冲区
 *   4. 返回响应状态码
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
    struct powerfs_net_frame_hdr hdr;
    struct powerfs_net_frame_hdr resp_hdr;
    __u32 seq;
    __u8 *tmp_body, *tmp_data;
    size_t tmp_body_len, tmp_data_len;
    int ret;
    int net_status;

    /* 检查连接状态 */
    if (g_conn.state != POWERFS_NET_STATE_CONNECTED || !g_conn.sock) {
        pr_debug("powerfs: not connected, cannot send request\n");
        return -ENOTCONN;
    }

    /* 分配序列号 */
    seq = atomic_inc_return(&g_conn.seq_counter);

    /* 编码并发送帧 */
    powerfs_net_frame_hdr_encode(&hdr, msg_type,
                                  POWERFS_NET_FLAG_REQUEST,
                                  seq, 0, body_len + data_len);

    ret = powerfs_net_frame_send(g_conn.sock, &hdr, body, body_len,
                                  data, data_len);
    if (ret < 0) {
        pr_debug("powerfs: send failed seq=%u: %d\n", seq, ret);
        return ret;
    }

    /* 同步接收响应 */
    tmp_body = kmalloc(POWERFS_NET_MAX_BODY, GFP_KERNEL);
    tmp_data = kmalloc(POWERFS_NET_MAX_DATA, GFP_KERNEL);
    if (!tmp_body || !tmp_data) {
        kfree(tmp_body);
        kfree(tmp_data);
        return -ENOMEM;
    }

    tmp_body_len = 0;
    tmp_data_len = 0;

    ret = powerfs_net_frame_recv(g_conn.sock, &resp_hdr,
                                  tmp_body, POWERFS_NET_MAX_BODY, &tmp_body_len,
                                  tmp_data, POWERFS_NET_MAX_DATA, &tmp_data_len,
                                  timeout_ms);
    if (ret < 0) {
        pr_debug("powerfs: recv failed seq=%u: %d\n", seq, ret);

        /* 连接断开 */
        if (ret == -EPIPE || ret == -ECONNRESET || ret == -ETIMEDOUT) {
            pr_err("powerfs: connection lost during request seq=%u\n", seq);
            mutex_lock(&g_conn.conn_lock);
            if (g_conn.sock) {
                powerfs_net_close_socket(g_conn.sock);
                g_conn.sock = NULL;
            }
            g_conn.state = POWERFS_NET_STATE_DISCONNECTED;
            mutex_unlock(&g_conn.conn_lock);
        }

        kfree(tmp_body);
        kfree(tmp_data);
        return ret;
    }

    /* 检查响应序列号 */
    if (resp_hdr.seq != seq) {
        pr_warn("powerfs: seq mismatch: sent=%u received=%u\n",
                 seq, resp_hdr.seq);
        kfree(tmp_body);
        kfree(tmp_data);
        return -EIO;
    }

    /* 获取响应状态 */
    net_status = (int)resp_hdr.status;

    /* 拷贝响应 body 到输出缓冲区 */
    if (resp_body && tmp_body_len > 0) {
        size_t copy_len = min(tmp_body_len, resp_body_cap);
        memcpy(resp_body, tmp_body, copy_len);
    }

    /* 拷贝响应 data 到输出缓冲区 */
    if (resp_data && tmp_data_len > 0) {
        size_t copy_len = min(tmp_data_len, resp_data_cap);
        memcpy(resp_data, tmp_data, copy_len);
    }

    /* 输出实际长度 */
    if (resp_body_len_out)
        *resp_body_len_out = tmp_body_len;
    if (resp_data_len_out)
        *resp_data_len_out = tmp_data_len;

    pr_debug("powerfs: request completed seq=%u msg_type=%u status=%d body=%zu data=%zu\n",
             seq, msg_type, net_status, tmp_body_len, tmp_data_len);

    kfree(tmp_body);
    kfree(tmp_data);

    return net_status;
}

/*
 * 注意: 以下异步接收函数已被弃用
 * 当前使用同步请求-响应模型 (powerfs_net_send_request)
 * 这些函数保留仅为未来可能的异步扩展
 */

/* ========== 重连机制 ========== */

/**
 * powerfs_net_reconnect_work - 重连工作函数
 */
static void powerfs_net_reconnect_work(struct work_struct *work)
{
    char addr[64];
    __u16 port;
    int ret;
    int i;

    /* 读取目标地址 */
    mutex_lock(&g_conn.conn_lock);
    strncpy(addr, g_server_addr, sizeof(addr) - 1);
    addr[sizeof(addr) - 1] = '\0';
    port = g_server_port;
    mutex_unlock(&g_conn.conn_lock);

    pr_info("powerfs: trying to reconnect to %s:%u (attempt %d)\n",
            addr, port, g_conn.reconnect_count + 1);

    /* 尝试重连 */
    for (i = 0; i < POWERFS_NET_MAX_RECONNECT; i++) {
        /* 等待一段时间 */
        msleep(POWERFS_NET_RECONNECT_DELAY);

        ret = powerfs_net_connect(addr, port);
        if (ret == 0) {
            pr_info("powerfs: reconnected successfully\n");
            return;
        }

        g_conn.reconnect_count++;
    }

    pr_err("powerfs: failed to reconnect after %d attempts\n",
           POWERFS_NET_MAX_RECONNECT);
    g_conn.state = POWERFS_NET_STATE_ERROR;
}

/* ========== 便捷方法 ========== */

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
    default:                                 return -EIO;
    }
}

/* ========== 便捷方法 ========== */

/**
 * powerfs_net_lookup - 查找文件
 */
int powerfs_net_lookup(__u64 dir_ino, const char *name, size_t name_len,
                       __u64 *ino, __u32 *mode, __u32 *uid, __u32 *gid,
                       __u64 *size, __u32 *nlink)
{
    __u8 body[256];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[256];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    /* 编码请求 */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, dir_ino);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, name, name_len);

    /* 发送请求 */
    ret = powerfs_net_send_request(POWERFS_NET_MSG_LOOKUP,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 2000,
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
    }

    return 0;
}

/**
 * powerfs_net_getattr - 获取文件属性
 */
int powerfs_net_getattr(__u64 ino, __u32 *mode, __u32 *uid, __u32 *gid,
                         __u64 *size, __u32 *nlink, __u64 *mtime_sec)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[128];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_GETATTR,
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
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_MODE, mode);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_UID, uid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_GID, gid);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, size);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_NLINK, nlink);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_MTIME, mtime_sec);
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
    powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_MODE, mode);
    powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_UID, uid);
    powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_GID, gid);
    powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_IS_DIR, is_dir ? 1 : 0);

    ret = powerfs_net_send_request(msg_type,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 3000,
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
                                    NULL, 0, 3000,
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
 * powerfs_net_readdir - 读取目录项
 */
int powerfs_net_readdir(__u64 dir_ino, __u64 offset, __u32 count,
                        __u8 *entries_buf, size_t buf_cap, __u32 *entries_count)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    size_t resp_data_len = 0;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, dir_ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, offset);
    powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_COUNT, count);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_READDIR,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    entries_buf, buf_cap, 5000,
                                    NULL, &resp_data_len);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    /* 解析 readdir 响应 */
    if (entries_count && resp_data_len > 0) {
        struct powerfs_tlv_dec dec;
        __u32 cnt = 0;

        powerfs_tlv_dec_init(&dec, entries_buf, resp_data_len);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_COUNT, &cnt);
        *entries_count = cnt;
    }

    return 0;
}

/**
 * powerfs_net_read - 读数据
 */
int powerfs_net_read(__u64 ino, __u64 offset, __u32 length,
                     __u8 *buf, size_t buf_cap, __u32 *read_len)
{
    __u8 body[64];
    struct powerfs_tlv_enc enc;
    size_t resp_data_len = 0;
    int ret;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, ino);
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, offset);
    powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_DATA_LEN, length);

    ret = powerfs_net_send_request(POWERFS_NET_MSG_READ,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    buf, buf_cap, 10000,
                                    NULL, &resp_data_len);
    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    if (read_len)
        *read_len = (__u32)resp_data_len;

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
                                    NULL, 0, 3000,
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
                                    NULL, 0, 3000,
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

    if (g_conn.state != POWERFS_NET_STATE_CONNECTED)
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
 */
int powerfs_net_init(void)
{
    int ret;

    /* 初始化 CRC32C 表 */
    if (!crc32c_table_init)
        powerfs_crc32c_init_table();

    /* 初始化连接上下文 */
    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.state = POWERFS_NET_STATE_DISCONNECTED;
    atomic_set(&g_conn.seq_counter, 0);
    mutex_init(&g_conn.conn_lock);
    spin_lock_init(&g_conn.req_lock);
    hash_init(g_conn.pending_reqs);
    INIT_WORK(&g_conn.reconnect_work, powerfs_net_reconnect_work);

    /* 分配接收缓冲区 (使用 vmalloc 避免大内存分配失败) */
    g_conn.recv_buf = vmalloc(POWERFS_NET_MAX_BODY + POWERFS_NET_MAX_DATA);
    if (!g_conn.recv_buf)
        return -ENOMEM;

    g_initialized = true;

    pr_info("powerfs: net subsystem initialized\n");

    /* 尝试立即连接到 Filer 服务器 */
    pr_info("powerfs: connecting to %s:%u ...", g_server_addr, g_server_port);
    ret = powerfs_net_connect(g_server_addr, g_server_port);
    if (ret < 0) {
        pr_warn("powerfs: initial connection failed: %d, will retry later\n", ret);
        /* 安排重连工作 */
        schedule_work(&g_conn.reconnect_work);
    }

    return 0;
}

/**
 * powerfs_net_exit - 清理 powerfs-net 子系统
 */
void powerfs_net_exit(void)
{
    if (!g_initialized)
        return;

    /* 断开连接 */
    powerfs_net_disconnect();

    /* 取消重连工作 */
    cancel_work_sync(&g_conn.reconnect_work);

    /* 释放缓冲区 (对应 vmalloc 使用 vfree) */
    if (g_conn.recv_buf)
        vfree(g_conn.recv_buf);

    /*
     * 注意: 当前使用同步请求-响应模型，pending_reqs 哈希表不再使用。
     * 保留哈希表结构定义以备未来异步扩展。
     * 清理所有哈希表条目 (如果有)。
     */
    spin_lock(&g_conn.req_lock);
    {
        struct powerfs_net_request *req;
        struct hlist_node *tmp;
        int i;

        hash_for_each_possible_safe(g_conn.pending_reqs, req, tmp, node, i) {
            req->status = -ENOTCONN;
            complete(&req->done);
            hash_del(&req->node);
        }
    }
    spin_unlock(&g_conn.req_lock);

    mutex_destroy(&g_conn.conn_lock);

    g_initialized = false;

    pr_info("powerfs: net subsystem exited\n");
}

/* ========== 导出符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_net_connect);
EXPORT_SYMBOL_GPL(powerfs_net_disconnect);
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

/* 全局连接池和 Delta Sync 状态 */
static struct powerfs_net_pool g_pool;
static struct powerfs_net_delta_state g_delta;
static bool g_pool_initialized = false;

/**
 * powerfs_net_pool_init - 初始化连接池
 */
int powerfs_net_pool_init(void)
{
    int i;

    /* 初始化连接池 */
    memset(&g_pool, 0, sizeof(g_pool));
    mutex_init(&g_pool.pool_lock);
    atomic_set(&g_pool.active_filer_idx, 0);
    atomic_set(&g_pool.active_master_idx, 0);
    atomic_set(&g_pool.active_volume_idx, 0);
    atomic_set(&g_pool.leader_idx, -1);
    atomic_set(&g_pool.leader_known, 0);
    atomic_set(&g_pool.failover_count, 0);
    INIT_DELAYED_WORK(&g_pool.monitor_work, powerfs_net_monitor_work_func);
    INIT_DELAYED_WORK(&g_pool.leader_check_work, powerfs_net_leader_check_work_func);
    g_pool.monitoring = false;

    /* 初始化服务器条目 */
    for (i = 0; i < POWERFS_NET_MAX_SERVERS; i++) {
        memset(&g_pool.servers[i], 0, sizeof(g_pool.servers[i]));
        g_pool.servers[i].last_check_time = jiffies;
    }

    /* 初始化 Delta Sync 状态 */
    memset(&g_delta, 0, sizeof(g_delta));
    spin_lock_init(&g_delta.gen_lock);
    atomic_set(&g_delta.path_count, 0);
    atomic_set(&g_delta.global_generation, 0);
    g_delta.last_full_sync = jiffies;

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

    /* 停止监控 */
    powerfs_net_stop_monitor();

    mutex_destroy(&g_pool.pool_lock);
    g_pool_initialized = false;

    pr_info("powerfs: connection pool exited\n");
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

    /* 停止监控 */
    powerfs_net_stop_monitor();

    /* 关闭主连接 */
    powerfs_net_disconnect();

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

    /* 清理 delta 状态 */
    powerfs_net_clear_all_generations();

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

/**
 * powerfs_net_set_volume - 设置 Volume 地址
 */
int powerfs_net_set_volume(const char *addr, __u16 port)
{
    return powerfs_net_add_server(addr, port, POWERFS_NET_SERVER_VOLUME);
}

/* ========== Leader 管理 ========== */

/**
 * powerfs_net_switch_leader - 切换 leader
 */
int powerfs_net_switch_leader(int new_idx)
{
    int ret = -EINVAL;

    if (new_idx < 0 || new_idx >= g_pool.server_count)
        return -EINVAL;

    mutex_lock(&g_pool.pool_lock);

    if (g_pool.servers[new_idx].type != POWERFS_NET_SERVER_FILER) {
        mutex_unlock(&g_pool.pool_lock);
        return -EINVAL;
    }

    /* 清除旧 leader */
    {
        int i;
        for (i = 0; i < g_pool.server_count; i++) {
            if (g_pool.servers[i].is_leader && i != new_idx) {
                g_pool.servers[i].is_leader = false;
            }
        }
    }

    /* 设置新 leader */
    g_pool.servers[new_idx].is_leader = true;
    atomic_set(&g_pool.leader_idx, new_idx);
    atomic_set(&g_pool.leader_known, 1);

    mutex_unlock(&g_pool.pool_lock);

    pr_info("powerfs: switched leader to server %d (%s:%u)\n",
            new_idx, g_pool.servers[new_idx].addr, g_pool.servers[new_idx].port);

    /* 记录故障转移 */
    atomic_inc(&g_pool.failover_count);
    atomic_set(&g_pool.last_failover_time, jiffies);

    ret = 0;

    /* 实际连接到新 leader */
    pr_info("powerfs: connecting to new leader %s:%u\n",
            g_pool.servers[new_idx].addr, g_pool.servers[new_idx].port);
    powerfs_net_disconnect();
    powerfs_net_connect(g_pool.servers[new_idx].addr,
                        g_pool.servers[new_idx].port);

    return ret;
}

/**
 * powerfs_net_find_leader - 查找并连接到 leader
 */
int powerfs_net_find_leader(void)
{
    int i;
    int ret = -ENOENT;

    /* 首先检查已知 leader */
    if (atomic_read(&g_pool.leader_known) &&
        atomic_read(&g_pool.leader_idx) >= 0) {
        int idx = atomic_read(&g_pool.leader_idx);
        if (idx < g_pool.server_count &&
            g_pool.servers[idx].type == POWERFS_NET_SERVER_FILER) {
            /* 尝试 ping leader */
            mutex_lock(&g_pool.pool_lock);
            {
                char addr[64];
                __u16 port;
                strncpy(addr, g_pool.servers[idx].addr, sizeof(addr) - 1);
                port = g_pool.servers[idx].port;
                mutex_unlock(&g_pool.pool_lock);

                pr_info("powerfs: trying known leader %s:%u\n", addr, port);
                powerfs_net_disconnect();
                ret = powerfs_net_connect(addr, port);
                if (ret == 0) {
                    pr_info("powerfs: connected to known leader\n");
                    return idx;
                }
            }
        }
    }

    /* 遍历所有 Filer 尝试连接 */
    mutex_lock(&g_pool.pool_lock);
    for (i = 0; i < g_pool.server_count; i++) {
        if (g_pool.servers[i].type == POWERFS_NET_SERVER_FILER) {
            char addr[64];
            __u16 port;

            strncpy(addr, g_pool.servers[i].addr, sizeof(addr) - 1);
            port = g_pool.servers[i].port;
            mutex_unlock(&g_pool.pool_lock);

            pr_info("powerfs: trying filer %d (%s:%u)\n", i, addr, port);
            powerfs_net_disconnect();
            ret = powerfs_net_connect(addr, port);
            if (ret == 0) {
                /* 标记为 leader */
                mutex_lock(&g_pool.pool_lock);
                {
                    int j;
                    for (j = 0; j < g_pool.server_count; j++) {
                        g_pool.servers[j].is_leader = (j == i);
                    }
                }
                atomic_set(&g_pool.leader_idx, i);
                atomic_set(&g_pool.leader_known, 1);
                mutex_unlock(&g_pool.pool_lock);

                pr_info("powerfs: found leader at server %d\n", i);
                return i;
            }

            mutex_lock(&g_pool.pool_lock);
        }
    }
    mutex_unlock(&g_pool.pool_lock);

    pr_err("powerfs: no leader found\n");
    return -ENOENT;
}

/**
 * powerfs_net_leader_ping - Ping leader 检查健康状态
 */
int powerfs_net_leader_ping(void)
{
    int idx;
    char addr[64];
    __u16 port;

    if (!atomic_read(&g_pool.leader_known))
        return -ENOENT;

    idx = atomic_read(&g_pool.leader_idx);
    if (idx < 0 || idx >= g_pool.server_count)
        return -EINVAL;

    mutex_lock(&g_pool.pool_lock);
    strncpy(addr, g_pool.servers[idx].addr, sizeof(addr) - 1);
    port = g_pool.servers[idx].port;
    g_pool.servers[idx].last_check_time = jiffies;
    mutex_unlock(&g_pool.pool_lock);

    /* 使用现有连接 ping */
    return powerfs_net_ping();
}

/**
 * powerfs_net_has_leader - 检查是否有 leader
 */
bool powerfs_net_has_leader(void)
{
    return atomic_read(&g_pool.leader_known) != 0;
}

/**
 * powerfs_net_get_leader_idx - 获取 leader 索引
 */
int powerfs_net_get_leader_idx(void)
{
    return atomic_read(&g_pool.leader_idx);
}

/* ========== 故障转移 ========== */

/**
 * powerfs_net_failover - 执行故障转移
 */
int powerfs_net_failover(void)
{
    int old_idx;
    int ret;

    old_idx = atomic_read(&g_pool.leader_idx);

    pr_warn("powerfs: starting failover from server %d\n", old_idx);

    /* 尝试查找新 leader */
    ret = powerfs_net_find_leader();
    if (ret < 0) {
        pr_err("powerfs: failover failed, no new leader found\n");
        return ret;
    }

    /* 记录故障转移统计 */
    atomic_inc(&g_pool.failover_count);
    atomic_set(&g_pool.last_failover_time, jiffies);

    pr_info("powerfs: failover completed: server %d -> %d\n",
            old_idx, ret);

    /* Delta Sync: 失效所有缓存 */
    powerfs_net_clear_all_generations();

    return 0;
}

/**
 * powerfs_net_monitor_work - 健康监控工作
 */
static void powerfs_net_monitor_work_func(struct work_struct *work)
{
    int ret;

    if (!g_pool.monitoring)
        return;

    /* 检查 leader 健康状态 */
    ret = powerfs_net_leader_ping();
    if (ret < 0) {
        pr_warn("powerfs: leader health check failed: %d\n", ret);

        /* 尝试故障转移 */
        ret = powerfs_net_failover();
        if (ret < 0) {
            pr_err("powerfs: failover failed, will retry\n");
        }
    }

    /* 安排下次检查 */
    if (g_pool.monitoring) {
        schedule_delayed_work(&g_pool.monitor_work,
                              msecs_to_jiffies(POWERFS_NET_MONITOR_INTERVAL));
    }
}

/**
 * powerfs_net_leader_check_work_func - Leader 检查工作
 */
static void powerfs_net_leader_check_work_func(struct work_struct *work)
{
    if (!g_pool.monitoring)
        return;

    /* 定期尝试发现/切换 leader */
    powerfs_net_find_leader();

    if (g_pool.monitoring) {
        schedule_delayed_work(&g_pool.leader_check_work,
                              msecs_to_jiffies(POWERFS_NET_LEADER_CHECK_INTERVAL));
    }
}

/**
 * powerfs_net_start_monitor - 启动健康监控
 */
void powerfs_net_start_monitor(void)
{
    if (g_pool.monitoring)
        return;

    g_pool.monitoring = true;
    schedule_delayed_work(&g_pool.monitor_work, 0);
    schedule_delayed_work(&g_pool.leader_check_work, 0);

    pr_info("powerfs: health monitor started\n");
}

/**
 * powerfs_net_stop_monitor - 停止健康监控
 */
void powerfs_net_stop_monitor(void)
{
    g_pool.monitoring = false;
    cancel_delayed_work_sync(&g_pool.monitor_work);
    cancel_delayed_work_sync(&g_pool.leader_check_work);

    pr_info("powerfs: health monitor stopped\n");
}

/* ========== Delta Sync 实现 ========== */

/**
 * powerfs_net_set_path_generation - 设置路径 generation
 */
int powerfs_net_set_path_generation(const char *path, __u64 generation)
{
    int i;
    int empty_idx = -1;

    if (!path)
        return -EINVAL;

    spin_lock(&g_delta.gen_lock);

    /* 查找现有路径 */
    for (i = 0; i < POWERFS_NET_MAX_PATHS; i++) {
        if (g_delta.paths[i].valid &&
            strcmp(g_delta.paths[i].path, path) == 0) {
            g_delta.paths[i].generation = generation;
            g_delta.paths[i].last_update = jiffies;
            spin_unlock(&g_delta.gen_lock);
            return 0;
        }
        if (!g_delta.paths[i].valid && empty_idx < 0)
            empty_idx = i;
    }

    /* 找到空位，插入新路径 */
    if (empty_idx >= 0) {
        strncpy(g_delta.paths[empty_idx].path, path,
                sizeof(g_delta.paths[empty_idx].path) - 1);
        g_delta.paths[empty_idx].generation = generation;
        g_delta.paths[empty_idx].last_update = jiffies;
        g_delta.paths[empty_idx].valid = true;
        atomic_inc(&g_delta.path_count);
    } else {
        /* 没有空位，复用最旧的 */
        __u64 oldest_time = ~0ULL;
        int oldest_idx = 0;

        for (i = 0; i < POWERFS_NET_MAX_PATHS; i++) {
            if (g_delta.paths[i].last_update < oldest_time) {
                oldest_time = g_delta.paths[i].last_update;
                oldest_idx = i;
            }
        }

        strncpy(g_delta.paths[oldest_idx].path, path,
                sizeof(g_delta.paths[oldest_idx].path) - 1);
        g_delta.paths[oldest_idx].generation = generation;
        g_delta.paths[oldest_idx].last_update = jiffies;
        g_delta.paths[oldest_idx].valid = true;
    }

    spin_unlock(&g_delta.gen_lock);
    return 0;
}

/**
 * powerfs_net_get_path_generation - 获取路径 generation
 */
__u64 powerfs_net_get_path_generation(const char *path)
{
    int i;
    __u64 gen = 0;

    if (!path)
        return 0;

    spin_lock(&g_delta.gen_lock);

    for (i = 0; i < POWERFS_NET_MAX_PATHS; i++) {
        if (g_delta.paths[i].valid &&
            strcmp(g_delta.paths[i].path, path) == 0) {
            gen = g_delta.paths[i].generation;
            break;
        }
    }

    spin_unlock(&g_delta.gen_lock);
    return gen;
}

/**
 * powerfs_net_path_stale - 检查路径是否过期
 */
bool powerfs_net_path_stale(const char *path, __u64 cached_generation)
{
    __u64 current_gen;

    if (!path)
        return true;

    current_gen = powerfs_net_get_path_generation(path);

    /* 如果没有记录，视为过期 */
    if (current_gen == 0)
        return true;

    return current_gen > cached_generation;
}

/**
 * powerfs_net_invalidate_path - 失效指定路径的缓存
 */
void powerfs_net_invalidate_path(const char *path)
{
    int i;

    if (!path)
        return;

    spin_lock(&g_delta.gen_lock);

    for (i = 0; i < POWERFS_NET_MAX_PATHS; i++) {
        if (g_delta.paths[i].valid &&
            strcmp(g_delta.paths[i].path, path) == 0) {
            g_delta.paths[i].generation = 0;
            g_delta.paths[i].valid = false;
            g_delta.paths[i].last_update = 0;
            atomic_dec(&g_delta.path_count);
            break;
        }
    }

    spin_unlock(&g_delta.gen_lock);

    pr_debug("powerfs: invalidated path cache: %s\n", path);
}

/**
 * powerfs_net_invalidate_dir - 失效目录的所有子路径
 */
void powerfs_net_invalidate_dir(__u64 dir_ino)
{
    int i;
    char dir_path[64];

    snprintf(dir_path, sizeof(dir_path), "ino_%llu",
              (unsigned long long)dir_ino);

    spin_lock(&g_delta.gen_lock);

    for (i = 0; i < POWERFS_NET_MAX_PATHS; i++) {
        if (g_delta.paths[i].valid &&
            strstr(g_delta.paths[i].path, dir_path) != NULL) {
            g_delta.paths[i].generation = 0;
            g_delta.paths[i].valid = false;
            atomic_dec(&g_delta.path_count);
        }
    }

    spin_unlock(&g_delta.gen_lock);

    pr_debug("powerfs: invalidated dir cache for ino %llu\n",
             (unsigned long long)dir_ino);
}

/**
 * powerfs_net_clear_all_generations - 清除所有路径 generation
 */
void powerfs_net_clear_all_generations(void)
{
    int i;

    spin_lock(&g_delta.gen_lock);

    for (i = 0; i < POWERFS_NET_MAX_PATHS; i++) {
        g_delta.paths[i].generation = 0;
        g_delta.paths[i].valid = false;
        g_delta.paths[i].last_update = 0;
    }

    atomic_set(&g_delta.path_count, 0);
    atomic_inc(&g_delta.global_generation);
    g_delta.last_full_sync = jiffies;

    spin_unlock(&g_delta.gen_lock);

    pr_info("powerfs: all path generations cleared (full sync needed)\n");
}

/**
 * powerfs_net_pull_delta - 拉取增量同步
 */
int powerfs_net_pull_delta(const char *path, __u64 *new_generation)
{
    __u64 current_gen;
    __u8 body[512];
    struct powerfs_tlv_enc enc;
    __u8 resp_body[512];
    size_t resp_body_len = 0;
    struct powerfs_tlv_dec dec;
    int ret;

    if (!path || !new_generation)
        return -EINVAL;

    current_gen = powerfs_net_get_path_generation(path);

    /* 编码请求: 发送当前 generation 请求增量 */
    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_GENERATION, current_gen);
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, path, strlen(path));

    /* 发送 PULL_DELTA 请求 */
    ret = powerfs_net_send_request(POWERFS_NET_MSG_PULL_DELTA,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    resp_body, sizeof(resp_body),
                                    NULL, 0, 2000,
                                    &resp_body_len, NULL);

    if (ret < 0)
        return ret;
    if (ret > 0)
        return net_status_to_errno((__u16)ret);

    /* 解码新 generation */
    if (resp_body_len > 0) {
        powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);
        ret = powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_GENERATION,
                                   new_generation);
        if (ret == 0) {
            /* 更新本地 generation */
            powerfs_net_set_path_generation(path, *new_generation);
        }
    } else {
        *new_generation = current_gen;
    }

    return 0;
}

/**
 * powerfs_net_push_delta - 推送增量同步
 */
int powerfs_net_push_delta(const char *path, __u64 generation)
{
    __u8 body[256];
    struct powerfs_tlv_enc enc;
    int ret;

    if (!path)
        return -EINVAL;

    powerfs_tlv_enc_init(&enc, body, sizeof(body));
    powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME, path, strlen(path));
    powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_GENERATION, generation);

    /* 发送 PUSH_DELTA 通知 */
    ret = powerfs_net_send_request(POWERFS_NET_MSG_PUSH_DELTA,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0, 1000,
                                    NULL, NULL);

    /* 更新本地记录 */
    powerfs_net_set_path_generation(path, generation);

    return ret;
}

/**
 * powerfs_net_full_sync - 执行全量同步
 */
int powerfs_net_full_sync(void)
{
    __u8 body[64];
    int ret;

    struct powerfs_tlv_enc enc;
    powerfs_tlv_enc_init(&enc, body, sizeof(body));

    /* 发送 FSYNC 触发全量同步 */
    ret = powerfs_net_send_request(POWERFS_NET_MSG_STATFS,
                                    body, powerfs_tlv_enc_len(&enc),
                                    NULL, 0,
                                    NULL, 0,
                                    NULL, 0, 5000,
                                    NULL, NULL);

    if (ret == 0) {
        powerfs_net_clear_all_generations();
        g_delta.last_full_sync = jiffies;
        pr_info("powerfs: full sync completed\n");
    }

    return ret;
}

/**
 * powerfs_net_get_global_generation - 获取全局 generation
 */
__u64 powerfs_net_get_global_generation(void)
{
    return (__u64)atomic_read(&g_delta.global_generation);
}

/**
 * powerfs_net_inc_global_generation - 增加全局 generation
 */
void powerfs_net_inc_global_generation(void)
{
    atomic_inc(&g_delta.global_generation);
}

/* ========== 导出新符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_net_pool_init);
EXPORT_SYMBOL_GPL(powerfs_net_pool_exit);
EXPORT_SYMBOL_GPL(powerfs_net_pool_cleanup);
EXPORT_SYMBOL_GPL(powerfs_net_add_server);
EXPORT_SYMBOL_GPL(powerfs_net_remove_server);
EXPORT_SYMBOL_GPL(powerfs_net_set_primary);
EXPORT_SYMBOL_GPL(powerfs_net_set_filers);
EXPORT_SYMBOL_GPL(powerfs_net_set_master);
EXPORT_SYMBOL_GPL(powerfs_net_set_volume);
EXPORT_SYMBOL_GPL(powerfs_net_switch_leader);
EXPORT_SYMBOL_GPL(powerfs_net_find_leader);
EXPORT_SYMBOL_GPL(powerfs_net_leader_ping);
EXPORT_SYMBOL_GPL(powerfs_net_has_leader);
EXPORT_SYMBOL_GPL(powerfs_net_get_leader_idx);
EXPORT_SYMBOL_GPL(powerfs_net_failover);
EXPORT_SYMBOL_GPL(powerfs_net_start_monitor);
EXPORT_SYMBOL_GPL(powerfs_net_stop_monitor);
EXPORT_SYMBOL_GPL(powerfs_net_set_path_generation);
EXPORT_SYMBOL_GPL(powerfs_net_get_path_generation);
EXPORT_SYMBOL_GPL(powerfs_net_path_stale);
EXPORT_SYMBOL_GPL(powerfs_net_invalidate_path);
EXPORT_SYMBOL_GPL(powerfs_net_invalidate_dir);
EXPORT_SYMBOL_GPL(powerfs_net_clear_all_generations);
EXPORT_SYMBOL_GPL(powerfs_net_pull_delta);
EXPORT_SYMBOL_GPL(powerfs_net_push_delta);
EXPORT_SYMBOL_GPL(powerfs_net_full_sync);
EXPORT_SYMBOL_GPL(powerfs_net_get_global_generation);
EXPORT_SYMBOL_GPL(powerfs_net_inc_global_generation);
