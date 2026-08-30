/* SPDX-License-Identifier: GPL-2.0 */
/*
 * powerfs_net_rdma.h - RDMA 传输层内部结构
 *
 * 定义 RDMA 连接对象、MR 池和函数声明.
 * 仅在 CONFIG_INFINIBAND 启用时编译.
 */
#ifndef POWERFS_NET_RDMA_H
#define POWERFS_NET_RDMA_H

#ifdef CONFIG_INFINIBAND

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

/* ========== RDMA 配置 ========== */

#define PFS_RDMA_MAX_SEND_WR    64
/* RECV WR 数量必须 <= PFS_RDMA_CTRL_BUF_NUM, 每个 RECV WR 占一个 ctrl 池条目.
 * 旧值 64 > 池 32 致 post_recv 32/64 后 -ENOMEM(-12). */
#define PFS_RDMA_MAX_RECV_WR    32
#define PFS_RDMA_MAX_SGE        3       /* hdr + body + data */
#define PFS_RDMA_MAX_INLINE     64      /* 28B header 可 inline */

/* MR 池: 控制帧缓冲 (64KB, 足够覆盖 256KB body 的多数情况) */
#define PFS_RDMA_CTRL_BUF_SIZE  65536   /* 64KB */
#define PFS_RDMA_CTRL_BUF_NUM   32      /* 32 个控制缓冲 */

/* MR 池: 大数据帧缓冲 (2MB, 对齐 POWERFS_NET_MAX_DATA) */
#define PFS_RDMA_DATA_BUF_SIZE  (2 * 1024 * 1024)  /* 2MB */
#define PFS_RDMA_DATA_BUF_NUM   4                  /* 4 个数据缓冲 */

/* 连接超时 */
#define PFS_RDMA_ADDR_TIMEOUT   5000    /* ms */
#define PFS_RDMA_ROUTE_TIMEOUT  5000    /* ms */

/* ========== MR 池 ========== */

/**
 * struct powerfs_rdma_mr_entry - MR 池中的单个条目
 * @mr: 已注册的 memory region
 * @buf: 缓冲区虚拟地址
 * @dma: DMA 地址
 * @size: 缓冲区大小
 * @list: 挂到 free_list
 */
struct powerfs_rdma_mr_entry {
    struct ib_mr   *mr;
    void           *buf;
    dma_addr_t      dma;
    size_t          size;
    struct list_head list;   /* 挂到 pool->free_list */
    int              pool_idx; /* 所属池: 0=ctrl, 1=data */
};

/**
 * struct powerfs_rdma_mr_pool - MR 缓冲池
 * @entries: MR 条目数组
 * @free_list: 可用条目链表
 * @lock: 保护 free_list
 * @total: 总条目数
 * @free: 可用条目数
 * @buf_size: 每个缓冲的大小
 */
struct powerfs_rdma_mr_pool {
    struct powerfs_rdma_mr_entry *entries;
    struct list_head             free_list;
    spinlock_t                   lock;
    int                          total;
    atomic_t                     free;
    size_t                       buf_size;
};

/* ========== RDMA 连接 ========== */

/**
 * struct powerfs_rdma_conn - RDMA 连接私有数据
 *
 * 挂在 conn->rdma 指针上, 包含 QP/CQ/MR 等 RDMA 资源.
 *
 * 生命周期:
 *   rdma_init_conn() → connect() → send/recv → disconnect() → fini_conn()
 */
struct powerfs_rdma_conn {
    /* rdma_cm 连接管理 */
    struct rdma_cm_id         *cm_id;
    struct rdma_event_channel *cm_channel;
    struct completion          cm_done;     /* 事件完成信号 */
    enum rdma_cm_event_type   cm_event;    /* 收到的事件 */
    int                       cm_status;   /* 事件状态 */

    /* IB 资源 */
    struct ib_pd             *pd;
    struct ib_cq             *send_cq;
    struct ib_cq             *recv_cq;
    struct ib_qp            *qp;           /* == cm_id->qp, 保存便于直接访问 */

    /* MR 池 */
    struct powerfs_rdma_mr_pool ctrl_pool;  /* 控制帧缓冲 (64KB x 32) */
    struct powerfs_rdma_mr_pool data_pool;  /* 数据帧缓冲 (2MB x 4) */

    /* RX: pre-posted RECV 管理 */
    atomic_t                  recv_posted;  /* 已 post 的 RECV 数 */
    spinlock_t                recv_lock;     /* 保护 recv buffer 管理 */

    /* RX: 已接收但未处理的 completion 队列 */
    struct list_head          rx_done_list;   /* 已完成的 RECV WR */
    spinlock_t                rx_done_lock;

    /* TX: send credit 管理 */
    atomic_t                  send_credits;  /* 可用发送槽位 (max_send_wr - in_flight) */
    wait_queue_head_t         send_waitq;    /* 等待 credit 释放 */

    /* 连接状态 */
    bool                      connected;
    bool                      errored;

    /* 所属 conn (回指针) */
    struct powerfs_net_server_conn *owner;
};

/* ========== 函数声明 ========== */

/* MR 池 */
int  powerfs_rdma_mr_pool_init(struct ib_pd *pd,
                               struct powerfs_rdma_mr_pool *pool,
                               int num_entries, size_t buf_size);
void powerfs_rdma_mr_pool_free(struct powerfs_rdma_mr_pool *pool);
struct powerfs_rdma_mr_entry *
powerfs_rdma_mr_pool_acquire(struct powerfs_rdma_mr_pool *pool,
                             gfp_t gfp);
void powerfs_rdma_mr_pool_release(struct powerfs_rdma_mr_pool *pool,
                                  struct powerfs_rdma_mr_entry *entry);

/* RDMA ops (供 powerfs_net_rdma.c 定义 powerfs_rdma_ops) */
int  powerfs_rdma_init_conn(struct powerfs_net_server_conn *conn);
void powerfs_rdma_fini_conn(struct powerfs_net_server_conn *conn);
int  powerfs_rdma_connect(struct powerfs_net_server_conn *conn);
void powerfs_rdma_disconnect(struct powerfs_net_server_conn *conn);
bool powerfs_rdma_is_connected(struct powerfs_net_server_conn *conn);
int  powerfs_rdma_send_frame(struct powerfs_net_server_conn *conn,
                             struct powerfs_net_frame_hdr *hdr,
                             const void *body, size_t body_len,
                             const void *data, size_t data_len);
bool powerfs_rdma_has_rx_data(struct powerfs_net_server_conn *conn);
int  powerfs_rdma_recv_frame(struct powerfs_net_server_conn *conn,
                             void *body_buf, size_t body_cap,
                             void *data_buf, size_t data_cap,
                             struct powerfs_net_frame_hdr *hdr_out,
                             size_t *body_len_out,
                             size_t *data_len_out);
void powerfs_rdma_enable_rx_notify(struct powerfs_net_server_conn *conn);
void powerfs_rdma_enable_tx_notify(struct powerfs_net_server_conn *conn);
int  powerfs_rdma_global_init(void);
void powerfs_rdma_global_exit(void);

/* CQ completion handler (softirq 上下文) */
void powerfs_rdma_cq_comp_handler(struct ib_cq *cq, void *ctx);

/* rdma_cm event handler */
int powerfs_rdma_cm_event_handler(struct rdma_cm_id *cm_id,
                                  struct rdma_cm_event *event);

/* Pre-post RECV */
int powerfs_rdma_post_recv(struct powerfs_rdma_conn *rdma);

#endif /* CONFIG_INFINIBAND */
#endif /* POWERFS_NET_RDMA_H */
