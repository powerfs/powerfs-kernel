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
/* [ROOT CAUSE 21 FIX G] RECV 与 SEND 共享同一个 ctrl MR pool.
 * 规则: CTRL_BUF_NUM 必须 >= MAX_RECV_WR(always-posted on HCA RQ)
 *   + MAX_SEND_WR (concurrent in-flight SEND WRs), 否则 pre-post 32
 *   占满全部 32 pool 条 → SEND acquire 永远 NULL → POST_SEND_EAGAIN
 *   pool_empty. 旧值 32 = MAX_RECV_WR, 无任何余量给 SEND.
 * 新值 128 = 32 RECV + 64 SEND max + 32 冗余 (short recv recycle 波动). */
#define PFS_RDMA_MAX_RECV_WR    32
#define PFS_RDMA_MAX_SGE        3       /* hdr + body + data */
#define PFS_RDMA_MAX_INLINE     64      /* 28B header 可 inline */

/* MR 池: 控制帧缓冲 (64KB, 足够覆盖 256KB body 的多数情况) */
#define PFS_RDMA_CTRL_BUF_SIZE  65536   /* 64KB */
#define PFS_RDMA_CTRL_BUF_NUM   128     /* 128 个: 32 RECV posted + 64 SEND inflight + 32 slack */

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
    int              pool_idx; /* 所属池: 0=ctrl, 1=data (功能性勿改) */
    /* [RC18d DIAG FIX] 条目唯一索引 0..total-1. pool_idx 是 TYPE 标记
     * (ctrl=0/data=1) 不是编号, 过去诊断误用它导致全显示 idx=0. */
    int              entry_id;
    /* [ROOT CAUSE 15 FIX] 每个 MR entry 内嵌 ib_cqe.
     * 现代内核 IB_POLL_WORKQUEUE/SOFTIRQ/DIRECT 的 __ib_process_cq(cq.c L109)
     * 标准契约是: HCA 写 CQE → 内核 poll → wc->wr_cqe != NULL →
     *   `wc->wr_cqe->done(cq, wc)` 回调.
     * 绝不允许直接覆盖 cq->comp_handler (原做法):
     *   内核 cq.c L248-258 设 comp_handler 为 direct/softirq/wq 内部函数,
     *   cancel_work_sync/cq_cleanup 路径会把 comp_handler 置 NULL →
     *   我们随后的 ib_process_cq_direct → __ib_process_cq 走 wc->wr_cqe NULL
     *   分支, 但我们老的 comp_handler 模式设 wr_id 指针, wr_cqe=NULL →
     *   进 else WARN (软) 然后 comp_handler = NULL 被 call *NULL →
     *   RIP=0x0 #PF Oops 0010 (qemu.log L128-L153). */
    struct ib_cqe    cqe;
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
    /* V3: dma_unmap_single 需要 dev; 存在此处 (不依赖 e->mr 存 dev).
     * V1/V2 回滚兼容: 若为 NULL, mr_pool_free 用 e->mr->device. */
    struct ib_device            *dev;
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

    /* PFSN 握手阶段 (建链后、connected=true 前).
     * CQ handler 通过此标志拦截首个 SEND/RECV WC, 分别完成量唤醒. */
    bool                      handshake_in_progress;
    bool                      hs_send_handled;  /* [RC16b] 握手 SEND WC 已处理 (防重复WC重复complete/repost) */
    bool                      hs_recv_handled;  /* [RC16b] 握手 RECV WC 已处理 */
    struct completion         hs_send_done;  /* 握手请求 SEND 完成 */
    struct completion         hs_recv_done;  /* 握手响应 RECV 完成 */
    u8                        hs_resp[24];   /* 握手响应缓冲 (≥ 18B) */
    size_t                    hs_resp_len;   /* 握手响应实际字节数 */
    /* 握手专用 1 页缓冲 + DMA map. [RC15] SEND WR 不再用 magic wr_id,
     * 改挂 hs_send_cqe (ib_cqe->done 标准回调). PFS_RDMA_HANDSHAKE_SEND_CQE
     * 作辅助识别: compare &rdma->hs_send_cqe == wc->wr_cqe 直接判定. */
    void                     *hs_send_page;
    dma_addr_t                hs_send_dma;
    struct ib_cqe             hs_send_cqe;   /* 握手 SEND 专用 CQE */
#define PFS_RDMA_HANDSHAKE_SEND_WR_ID   0x48535752U   /* "HSWR" 旧 magic, 仅兼容保留 */

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

/* CQ completion: 现代内核 wc->wr_cqe->done 标准回调.
 * 不再直接覆盖 cq->comp_handler (会在 WORKQUEUE cancel 时被置 NULL 导致 Oops). */
void powerfs_rdma_cqe_done(struct ib_cq *cq, struct ib_wc *wc);

/* rdma_cm event handler */
int powerfs_rdma_cm_event_handler(struct rdma_cm_id *cm_id,
                                  struct rdma_cm_event *event);

/* Pre-post RECV */
int powerfs_rdma_post_recv(struct powerfs_rdma_conn *rdma);

#endif /* CONFIG_INFINIBAND */
#endif /* POWERFS_NET_RDMA_H */
