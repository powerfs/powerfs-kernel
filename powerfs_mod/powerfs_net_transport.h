/* SPDX-License-Identifier: GPL-2.0 */
/*
 * powerfs_net_transport.h - 传输层抽象 (Phase 1: TCP/IB transport abstraction)
 *
 * 把底层传输 (TCP socket / RDMA QP) 从连接池状态机中剥离出来.
 * 连接池 (powerfs_net_conn.c) 通过 conn->transport->ops 调用具体传输,
 * 不再直接触碰 conn->sock. Phase 1 仅包装现有 TCP 路径, 后续 Phase
 * 接入 RDMA 时只需实现一套 ops 并在 conn init 时切换指针.
 *
 * 设计原则:
 *   - ops 函数只做传输相关 I/O (建链/断链/收发帧), 不碰状态机/调度器
 *   - TCP 实现复用现有 powerfs_net_sock.c / powerfs_net_conn.c 函数,
 *     不改变既有行为 (仅 wrap, 不 reimplement)
 *   - 本头文件被 powerfs_net.h 在顶部 include, 故仅使用前向声明,
 *     不依赖 struct powerfs_net_server_conn 的完整定义
 */
#ifndef POWERFS_NET_TRANSPORT_H
#define POWERFS_NET_TRANSPORT_H

#include <linux/types.h>

/* 前向声明: 完整定义在 powerfs_net.h (本头文件被其提前 include) */
struct powerfs_net_server_conn;
struct powerfs_net_frame_hdr;
struct powerfs_request;

/* 传输类型 */
enum powerfs_transport_type {
    POWERFS_TRANSPORT_TCP  = 0,   /* 内核 TCP socket (现有路径) */
    POWERFS_TRANSPORT_RDMA = 1,   /* RDMA RC QP (后续 Phase 接入) */
};

/**
 * struct powerfs_transport_ops - 传输层操作集
 *
 * 所有函数均以 conn 为第一参数, 传输私有状态挂在 conn 上
 * (TCP 用 conn->sock, RDMA 后续用 conn->rdma_* 字段).
 *
 * 返回值约定与现有 TCP 路径一致:
 *   send_frame: 0=完成, -EAGAIN=部分发送待续, <0=不可恢复错误
 *   recv_frame: 0=一帧完整, -EAGAIN=无数据待续, <0=EOF/错误触发断连
 */
struct powerfs_transport_ops {
    const char *name;
    enum powerfs_transport_type type;

    /* === 连接生命周期 === */
    /* 建链 + 握手, 成功后传输私有句柄已就绪 (TCP: conn->sock 已赋值) */
    int  (*connect)(struct powerfs_net_server_conn *conn);
    /* 关闭传输 (TCP: close_socket + sock=NULL). 不负责状态机/回调清理 */
    void (*disconnect)(struct powerfs_net_server_conn *conn);
    /* 传输是否就绪可收发 (TCP: sock!=NULL && state==CONNECTED) */
    bool (*is_connected)(struct powerfs_net_server_conn *conn);

    /* === 帧收发 === */
    /* 非阻塞发送一帧 (wrap pfs_frame_send_nonblock, 传 conn->sock) */
    int  (*send_frame)(struct powerfs_net_server_conn *conn,
                       struct powerfs_net_frame_hdr *hdr,
                       const __u8 *body, size_t body_len,
                       const __u8 *data, size_t data_len,
                       struct powerfs_request *req);
    /* 是否有可收数据 (TCP: skb_queue 非空 或 rx_ready) */
    bool (*has_rx_data)(struct powerfs_net_server_conn *conn);
    /* 非阻塞推进收帧状态机 (wrap pfs_rx_step, 收到 conn->rx_*_buf) */
    int  (*recv_frame)(struct powerfs_net_server_conn *conn);

    /* === 通知控制 ===
     * TCP 的 sk 回调始终激活, 故为 no-op.
     * RDMA 需要显式 post_recv / arm completion, 留给后续 Phase. */
    void (*enable_rx_notify)(struct powerfs_net_server_conn *conn);
    void (*enable_tx_notify)(struct powerfs_net_server_conn *conn);

    /* === per-conn init/fini ===
     * TCP: no-op (buffer 分配由 conn init 直接调 pfs_conn_alloc_rxbuffers).
     * RDMA: 分配 QP / 注册 MR 等. */
    int  (*init_conn)(struct powerfs_net_server_conn *conn);
    void (*fini_conn)(struct powerfs_net_server_conn *conn);

    /* === 全局 init/fini ===
     * TCP: no-op.
     * RDMA: ib_register_client / alloc PD 等. */
    int  (*global_init)(void);
    void (*global_exit)(void);
};

/* TCP 传输 ops (定义在 powerfs_net_tcp_ops.c) */
extern const struct powerfs_transport_ops powerfs_tcp_ops;

/* RDMA 传输 ops (定义在 powerfs_net_rdma.c, 仅 CONFIG_INFINIBAND=y 时存在).
 * powerfs_net_conn.c 按 g_pool.transport_type 选 tcp/rdma ops. */
#ifdef CONFIG_INFINIBAND
extern const struct powerfs_transport_ops powerfs_rdma_ops;
#endif

/* 按 transport_type 选择 ops. 未启用 INFINIBAND 时 rdma 退化为 tcp (防御). */
static inline const struct powerfs_transport_ops *
powerfs_transport_pick_ops(enum powerfs_transport_type type)
{
#ifdef CONFIG_INFINIBAND
    if (type == POWERFS_TRANSPORT_RDMA)
        return &powerfs_rdma_ops;
#endif
    return &powerfs_tcp_ops;
}

#endif /* POWERFS_NET_TRANSPORT_H */
