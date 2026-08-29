// SPDX-License-Identifier: GPL-2.0
/*
 * powerfs_net_rdma.c - RDMA RC QP 传输 ops 实现
 *
 * 在内核态用 rdma_cm + ib_verbs 建立 RC QP, 通过 SEND/RECV 收发
 * powerfs-net 帧 (frame header + body + data). MR 池预注册控制帧
 * (64KB) 与大数据帧 (2MB) 两档缓冲, SEND/RECV WR 复用池条目.
 *
 * 设计要点:
 *   - rdma_cm 事件回调 (cm_event_handler) 与 CQ 完成回调
 *     (cq_comp_handler) 共用 struct powerfs_rdma_conn 作为上下文.
 *   - CQ 完成回调在 IRQ 上下文执行 (RXE 在 cq_lock irqsave 内调用),
 *     全程使用 irqsave 自旋锁 + 原子操作, 不睡眠.
 *   - send_frame 非阻塞: 无发送 credit 或 MR 池空时返回 -EAGAIN,
 *     调用方 (TX 调度器) 由 CQ 完成回调 pfs_tx_callback 重新投递.
 *   - recv_frame 从 rx_done_list 取已完成 RECV, 拷出帧后立刻 re-post,
 *     由 pfs_rx_callback 通知 RX 调度器续收.
 *   - 握手: 与现有 TCP 路径不同, RDMA 当前指向未来 RDMA-capable 服务端,
 *     Phase 1 在 ESTABLISHED 后直接置 connected=true, 跳过握手帧交换.
 *
 * 内存注册: 内核 kmalloc/page 分配的缓冲无法走 ib_reg_user_mr (其底层
 * 走 pin_user_pages 仅作用于用户地址), 故采用内核标准模式
 * ib_alloc_mr(IB_MR_TYPE_MEM_REG) + ib_map_mr_sg 把页注册到 MR, 用
 * mr->lkey 作为 SGE 的 lkey. 此模式对 RXE/SIW 软件卡与硬件卡都通用.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/delay.h>
#include <net/net_namespace.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "powerfs_net.h"
#include "powerfs_net_internal.h"
#include "powerfs_net_transport.h"
#include "powerfs_net_rdma.h"

/* CQ 单次 poll 的 WC 数 (栈数组, 16 * ~80B = 1.3KB, 8KB 栈足够) */
#define PFS_RDMA_WC_BATCH        16

/* CQ comp_handler 单次调用处理上限 (对齐内核 ib_poll_handler budget) */
#define PFS_RDMA_CQ_BUDGET       256

/* ============= 内部辅助 ============= */

static inline struct powerfs_rdma_conn *
pfs_conn_rdma(struct powerfs_net_server_conn *conn)
{
    return conn->rdma;
}

/* 由 MR 条目反查所属池 */
static inline struct powerfs_rdma_mr_pool *
pfs_mr_entry_pool(struct powerfs_rdma_conn *rdma,
                  struct powerfs_rdma_mr_entry *e)
{
    return (e->pool_idx == 1) ? &rdma->data_pool : &rdma->ctrl_pool;
}

/* ============= MR 池 ============= */

/**
 * powerfs_rdma_mr_pool_init - 预注册 num_entries 个 buf_size 大小的 MR
 *
 * 每个 entry:
 *   1. __get_free_pages 分配页对齐缓冲 (DMA 友好, 比 kmalloc 更易整页注册)
 *   2. ib_alloc_mr(IB_MR_TYPE_MEM_REG, 1) 分配 MR 描述符
 *   3. ib_dma_map_single 取 DMA 地址
 *   4. ib_map_mr_sg 把单条 SG (整个缓冲) 烧入 MR
 * 之后 entry->mr->lkey 可直接用于 SGE.
 *
 * 失败时回滚已分配资源.
 */
int powerfs_rdma_mr_pool_init(struct ib_pd *pd,
                              struct powerfs_rdma_mr_pool *pool,
                              int num_entries, size_t buf_size)
{
    struct ib_device *dev = pd->device;
    struct scatterlist sg;
    int i, ret;

    pool->entries = kcalloc(num_entries, sizeof(*pool->entries), GFP_KERNEL);
    if (!pool->entries)
        return -ENOMEM;

    INIT_LIST_HEAD(&pool->free_list);
    spin_lock_init(&pool->lock);
    pool->total = num_entries;
    atomic_set(&pool->free, num_entries);
    pool->buf_size = buf_size;

    for (i = 0; i < num_entries; i++) {
        struct powerfs_rdma_mr_entry *e = &pool->entries[i];
        u64 dma;
        int nents;

        e->pool_idx = (buf_size >= PFS_RDMA_DATA_BUF_SIZE) ? 1 : 0;

        /* 页对齐缓冲: order = ceil(log2(buf_size)/PAGE_SHIFT) */
        e->buf = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO | __GFP_COMP,
                                          get_order(buf_size));
        if (!e->buf) {
            ret = -ENOMEM;
            goto err;
        }
        e->size = buf_size;

        e->mr = ib_alloc_mr(pd, IB_MR_TYPE_MEM_REG, 1);
        if (IS_ERR(e->mr)) {
            ret = PTR_ERR(e->mr);
            e->mr = NULL;
            goto err;
        }

        dma = ib_dma_map_single(dev, e->buf, buf_size, DMA_BIDIRECTIONAL);
        if (ib_dma_mapping_error(dev, dma)) {
            ret = -EIO;
            goto err;
        }
        e->dma = dma;

        /* 单条 SG 覆盖整个缓冲, ib_map_mr_sg 把 DMA 地址烧入 MR */
        sg_init_table(&sg, 1);
        sg_set_buf(&sg, e->buf, buf_size);
        sg_dma_address(&sg) = dma;
        sg_dma_len(&sg) = buf_size;
        nents = ib_map_mr_sg(e->mr, &sg, 1, NULL, PAGE_SIZE);
        if (nents != 1) {
            ret = (nents < 0) ? nents : -EINVAL;
            goto err;
        }

        list_add_tail(&e->list, &pool->free_list);
    }

    return 0;

err:
    pr_err("powerfs_rdma: mr_pool_init(buf_size=%zu, n=%d) failed at %d: %d\n",
           buf_size, num_entries, i, ret);
    powerfs_rdma_mr_pool_free(pool);
    return ret;
}

/**
 * powerfs_rdma_mr_pool_free - 释放 MR 池所有资源
 *
 * 顺序: dereg_mr → dma_unmap → free_pages. 注意 ib_dereg_mr 内部释放
 * MR 描述符, 故需先从某 entry 保存 device 指针供后续 unmap 使用.
 */
void powerfs_rdma_mr_pool_free(struct powerfs_rdma_mr_pool *pool)
{
    struct ib_device *dev = NULL;
    int i;

    if (!pool->entries)
        return;

    /* 仍在 free_list 上的条目无需摘除, 整批释放 */
    for (i = 0; i < pool->total; i++) {
        struct powerfs_rdma_mr_entry *e = &pool->entries[i];
        if (e->mr && !dev)
            dev = e->mr->device;
    }

    for (i = 0; i < pool->total; i++) {
        struct powerfs_rdma_mr_entry *e = &pool->entries[i];

        if (e->mr) {
            ib_dereg_mr(e->mr);
            e->mr = NULL;
        }
        if (e->dma && dev) {
            ib_dma_unmap_single(dev, e->dma, e->size, DMA_BIDIRECTIONAL);
            e->dma = 0;
        }
        if (e->buf) {
            free_pages((unsigned long)e->buf, get_order(e->size));
            e->buf = NULL;
        }
    }

    kfree(pool->entries);
    pool->entries = NULL;
    pool->total = 0;
    atomic_set(&pool->free, 0);
    INIT_LIST_HEAD(&pool->free_list);
}

/**
 * powerfs_rdma_mr_pool_acquire - 从 free_list 摘一个 MR 条目
 *
 * 在持锁前先 atomic_dec_if_positive 试探, 避免无谓锁竞争.
 * 软中断上下文 (CQ handler 路径) 调用必须传 GFP_ATOMIC.
 */
struct powerfs_rdma_mr_entry *
powerfs_rdma_mr_pool_acquire(struct powerfs_rdma_mr_pool *pool, gfp_t gfp)
{
    struct powerfs_rdma_mr_entry *e = NULL;
    unsigned long flags;

    /* 快路径: 池空时立即返回 NULL, 不阻塞 */
    if (atomic_dec_if_positive(&pool->free) < 0)
        return NULL;

    spin_lock_irqsave(&pool->lock, flags);
    if (!list_empty(&pool->free_list)) {
        e = list_first_entry(&pool->free_list,
                             struct powerfs_rdma_mr_entry, list);
        list_del_init(&e->list);
    }
    spin_unlock_irqrestore(&pool->lock, flags);

    /* 极端情况: atomic 计数与链表短暂不一致 (并发释放/获取), 把份额还回 */
    if (!e)
        atomic_inc(&pool->free);

    return e;
}

/**
 * powerfs_rdma_mr_pool_release - 把 MR 条目还回 free_list
 *
 * 可在 IRQ 上下文调用 (CQ handler 完成时).
 */
void powerfs_rdma_mr_pool_release(struct powerfs_rdma_mr_pool *pool,
                                  struct powerfs_rdma_mr_entry *entry)
{
    unsigned long flags;

    if (!entry)
        return;

    spin_lock_irqsave(&pool->lock, flags);
    list_add_tail(&entry->list, &pool->free_list);
    spin_unlock_irqrestore(&pool->lock, flags);

    atomic_inc(&pool->free);
}

/* ============= CQ 完成回调 ============= */

/* 处理单个 WC. IRQ 上下文, 不可睡眠. */
static void powerfs_rdma_process_wc(struct powerfs_rdma_conn *rdma,
                                   struct ib_wc *wc)
{
    struct powerfs_rdma_mr_entry *e;
    struct powerfs_net_server_conn *conn = rdma->owner;

    /* wc->wr_id 与 wc->wr_cqe 是 union, 我们用 wr_id 编码 entry 指针 */
    e = (struct powerfs_rdma_mr_entry *)(uintptr_t)wc->wr_id;
    if (!e) {
        pr_warn_ratelimited("powerfs_rdma: WC with NULL wr_id (status=%d op=%d)\n",
                            wc->status, wc->opcode);
        return;
    }

    if (wc->status != IB_WC_SUCCESS) {
        /* 错误完成: QP 进入 ERROR, 后续 WR 也会以错误完成. 释放资源,
         * 标记连接错误, 触发断连 (由 RX/TX 调度器或 disconnect_work 处理). */
        rdma->errored = true;
        if (e->pool_idx == 0 || e->pool_idx == 1) {
            struct powerfs_rdma_mr_pool *pool = pfs_mr_entry_pool(rdma, e);
            powerfs_rdma_mr_pool_release(pool, e);
        }
        if (wc->opcode & IB_WC_RECV) {
            /* recv 错误也算接收事件, 让 RX 调度器看到错误后断连 */
            pfs_rx_callback(conn);
        } else {
            /* send 错误: 释放 credit + 通知 TX */
            atomic_inc(&rdma->send_credits);
            pfs_tx_callback(conn);
        }
        return;
    }

    if (wc->opcode & IB_WC_RECV) {
        /* RECV 完成: entry 加入 rx_done_list, 由 recv_frame 消费 */
        unsigned long flags;
        spin_lock_irqsave(&rdma->rx_done_lock, flags);
        list_add_tail(&e->list, &rdma->rx_done_list);
        spin_unlock_irqrestore(&rdma->rx_done_lock, flags);
        /* 唤醒 RX 调度器 (pfs_rx_callback 内部 spin_lock_bh 安全) */
        pfs_rx_callback(conn);
    } else {
        /* SEND 完成: 释放 MR 回池, 增 credit, 通知 TX */
        struct powerfs_rdma_mr_pool *pool = pfs_mr_entry_pool(rdma, e);
        powerfs_rdma_mr_pool_release(pool, e);
        atomic_inc(&rdma->send_credits);
        pfs_tx_callback(conn);
    }
}

/**
 * powerfs_rdma_cq_comp_handler - CQ 完成回调 (IRQ 上下文)
 *
 * 流程:
 *   1. 排空 CQ (ib_poll_cq 循环直至无完成)
 *   2. 重新 arm CQ (IB_CQ_NEXT_COMP), 若返回 >0 表示 arm 期间有遗漏,
 *      再 poll 一次.
 *
 * 注意: ib_alloc_cq(SOFTIRQ) 默认会把 comp_handler 设为 ib_cq_completion_softirq
 * (走 irq_poll 框架, 要求每个 WR 用 ib_cqe->done 模式). 我们覆盖 comp_handler
 * 后绕过 irq_poll, 直接用 wr_id 模式, 与现有 MR 池设计 (entry 指针当 wr_id) 对齐.
 */
void powerfs_rdma_cq_comp_handler(struct ib_cq *cq, void *ctx)
{
    struct powerfs_rdma_conn *rdma = (struct powerfs_rdma_conn *)ctx;
    struct ib_wc wcs[PFS_RDMA_WC_BATCH];
    int n, i, missed;
    int budget = PFS_RDMA_CQ_BUDGET;

    if (!rdma)
        return;

poll_again:
    /* 1. 排空当前完成 (受 budget 限制, 防止完成风暴下长时间占用 CPU) */
    while ((n = ib_poll_cq(cq, min_t(int, PFS_RDMA_WC_BATCH, budget), wcs)) > 0) {
        for (i = 0; i < n; i++)
            powerfs_rdma_process_wc(rdma, &wcs[i]);
        budget -= n;
        if (budget <= 0)
            break;
    }

    /* 2. 重新 arm CQ; 若有遗漏事件, 再 poll 一次防丢失.
     * budget 耗尽时遗漏事件会在下次 comp_handler 触发时处理. */
    if (budget > 0) {
        missed = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP |
                                  IB_CQ_REPORT_MISSED_EVENTS);
        if (missed > 0)
            goto poll_again;
    }
}

/* ============= rdma_cm 事件处理 ============= */

/**
 * powerfs_rdma_cm_event_handler - rdma_cm 事件回调
 *
 * 与连接线程通过 cm_done completion 同步:
 *   ADDR_RESOLVED    → 继续路由解析 (rdma_resolve_route)
 *   ROUTE_RESOLVED   → 通知连接线程 (线程随后建 PD/CQ/QP/MR)
 *   ESTABLISHED      → 通知连接线程成功
 *   其余错误事件      → 通知连接线程失败
 *   DISCONNECTED     → 标记 errored, 唤醒 RX/TX 调度器做断连
 *
 * rdma_cm 在持有 cm_id->mutex 的进程上下文调用本回调, 可睡眠.
 */
int powerfs_rdma_cm_event_handler(struct rdma_cm_id *cm_id,
                                  struct rdma_cm_event *event)
{
    struct powerfs_rdma_conn *rdma = (struct powerfs_rdma_conn *)cm_id->context;

    if (!rdma) {
        pr_warn_ratelimited("powerfs_rdma: cm event %d with NULL ctx\n",
                            event->event);
        return 0;
    }

    switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
        /* 地址解析完成, 继续路由解析 (异步, 完成后会再来 ROUTE_RESOLVED) */
        if (rdma_resolve_route(cm_id, PFS_RDMA_ROUTE_TIMEOUT)) {
            pr_warn("powerfs_rdma: rdma_resolve_route failed\n");
            rdma->cm_event = RDMA_CM_EVENT_ROUTE_ERROR;
            rdma->cm_status = -EFAULT;
            complete(&rdma->cm_done);
        }
        break;

    case RDMA_CM_EVENT_ROUTE_RESOLVED:
    case RDMA_CM_EVENT_ESTABLISHED:
        /* 路由就绪或连接已建立: 通知连接线程 */
        rdma->cm_event = event->event;
        rdma->cm_status = 0;
        complete(&rdma->cm_done);
        break;

    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
        pr_warn("powerfs_rdma: connect failed: %s (status=%d)\n",
                rdma_event_msg(event->event), event->status);
        rdma->cm_event = event->event;
        rdma->cm_status = (event->status < 0) ? event->status : -ECONNREFUSED;
        rdma->errored = true;
        complete(&rdma->cm_done);
        break;

    case RDMA_CM_EVENT_DISCONNECTED:
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
    case RDMA_CM_EVENT_ADDR_CHANGE:
        pr_info("powerfs_rdma: peer disconnect (%s)\n",
                rdma_event_msg(event->event));
        rdma->connected = false;
        rdma->errored = true;
        /* 唤醒 RX/TX 调度器让其感知断连 */
        if (rdma->owner) {
            pfs_rx_callback(rdma->owner);
            pfs_tx_callback(rdma->owner);
            wake_up(&rdma->send_waitq);
        }
        break;

    default:
        pr_debug("powerfs_rdma: ignored cm event %s\n",
                 rdma_event_msg(event->event));
        break;
    }

    /* 返回 0: 不让 rdma_cm 自动销毁 cm_id (由我们 disconnect 时显式销毁) */
    return 0;
}

/* ============= Pre-post RECV ============= */

/**
 * powerfs_rdma_post_recv - 从 ctrl 池取一个 MR, post 一个 RECV WR
 *
 * RECV 缓冲必须足够大以容纳最大可能的入站帧 (HDR + body + data).
 * ctrl 池 64KB 只够装小帧; 大帧 (write_needle 1MB data) 不会被 RECV,
 * 而是走单独的 RDMA READ 流程 (后续 Phase). 当前 Phase 1 仅支持控制帧.
 */
int powerfs_rdma_post_recv(struct powerfs_rdma_conn *rdma)
{
    struct powerfs_rdma_mr_entry *e;
    struct ib_sge sge;
    struct ib_recv_wr wr;
    const struct ib_recv_wr *bad;
    int ret;

    e = powerfs_rdma_mr_pool_acquire(&rdma->ctrl_pool, GFP_ATOMIC);
    if (!e)
        return -ENOMEM;

    /* 单条 SGE 覆盖整个 ctrl 池缓冲, 对端可发不超过该长度的帧 */
    sge.addr   = e->dma;
    sge.length = e->size;
    sge.lkey   = e->mr->lkey;

    wr.wr_id   = (u64)(uintptr_t)e;
    wr.next    = NULL;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ret = ib_post_recv(rdma->qp, &wr, &bad);
    if (ret) {
        pr_warn_ratelimited("powerfs_rdma: ib_post_recv failed: %d\n", ret);
        powerfs_rdma_mr_pool_release(&rdma->ctrl_pool, e);
        return ret;
    }

    atomic_inc(&rdma->recv_posted);
    return 0;
}

/* ============= 连接生命周期 ============= */

/* 构造目的 sockaddr_in (复用 TCP 路径的 in4_pton 校验) */
static int pfs_rdma_resolve_dst(struct powerfs_net_server_conn *conn,
                                struct rdma_cm_id *cm_id)
{
    struct sockaddr_in sin = {};
    int ret;

    sin.sin_family = AF_INET;
    sin.sin_port = cpu_to_be16(conn->port);
    /* in4_pton 返回 1=成功, 0=失败 */
    if (!in4_pton(conn->addr, -1, (void *)&sin.sin_addr, '\0', NULL)) {
        pr_err("powerfs_rdma: invalid address %s\n", conn->addr);
        return -EINVAL;
    }

    ret = rdma_resolve_addr(cm_id, NULL, (struct sockaddr *)&sin,
                            PFS_RDMA_ADDR_TIMEOUT);
    if (ret) {
        pr_err("powerfs_rdma: rdma_resolve_addr(%s:%u) failed: %d\n",
               conn->addr, conn->port, ret);
        return ret;
    }
    return 0;
}

/**
 * powerfs_rdma_init_conn - 为 conn 分配 rdma_conn 私有结构
 *
 * 仅分配结构, 不创建任何 RDMA 资源 (PD/CQ/QP/MR). 资源在 connect() 时
 * 才建, 因为只有 rdma_resolve_addr/route 之后才能拿到 ib_device.
 */
int powerfs_rdma_init_conn(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma;

    if (conn->rdma)
        return 0;  /* 已初始化 */

    rdma = kzalloc(sizeof(*rdma), GFP_KERNEL);
    if (!rdma)
        return -ENOMEM;

    rdma->cm_id     = NULL;
    rdma->cm_channel = NULL;  /* 内核态不用 event_channel */
    rdma->owner     = conn;
    init_completion(&rdma->cm_done);
    spin_lock_init(&rdma->recv_lock);
    spin_lock_init(&rdma->rx_done_lock);
    INIT_LIST_HEAD(&rdma->rx_done_list);
    atomic_set(&rdma->recv_posted, 0);
    atomic_set(&rdma->send_credits, 0);
    init_waitqueue_head(&rdma->send_waitq);
    rdma->connected = false;
    rdma->errored   = false;

    conn->rdma = rdma;
    return 0;
}

/**
 * powerfs_rdma_fini_conn - 释放 conn->rdma (资源已在 disconnect 时销毁)
 */
void powerfs_rdma_fini_conn(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);

    if (!rdma)
        return;

    /* 兜底: 若 disconnect 未跑完, 强制清理 (理论不应触发) */
    if (rdma->cm_id) {
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
    }
    if (rdma->send_cq) {
        ib_free_cq(rdma->send_cq);
        rdma->send_cq = NULL;
    }
    if (rdma->recv_cq) {
        ib_free_cq(rdma->recv_cq);
        rdma->recv_cq = NULL;
    }
    powerfs_rdma_mr_pool_free(&rdma->ctrl_pool);
    powerfs_rdma_mr_pool_free(&rdma->data_pool);
    if (rdma->pd) {
        ib_dealloc_pd(rdma->pd);
        rdma->pd = NULL;
    }

    kfree(rdma);
    conn->rdma = NULL;
}

/**
 * powerfs_rdma_connect - 建链
 *
 * 流程 (同步阻塞, 由连接池 connect_one 调用):
 *   1. rdma_create_id (event_handler=powerfs_rdma_cm_event_handler, ctx=rdma)
 *   2. rdma_resolve_addr → wait cm_done (ADDR_RESOLVED → 自动 ROUTE_RESOLVED)
 *   3. cm_id->device 已绑定, ib_alloc_pd
 *   4. ib_alloc_cq × 2 (send_cq + recv_cq, 均 IB_POLL_SOFTIRQ, 覆盖 comp_handler)
 *   5. rdma_create_qp (IB_QPT_RC, sq_sig_all=1)
 *   6. powerfs_rdma_mr_pool_init(ctrl_pool) + (data_pool)
 *   7. powerfs_rdma_post_recv × PFS_RDMA_MAX_RECV_WR
 *   8. rdma_connect → wait cm_done (ESTABLISHED)
 *   9. connected = true
 *
 * 任一步失败均回滚已分配资源.
 */
int powerfs_rdma_connect(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);
    struct ib_qp_init_attr qp_attr = {};
    struct rdma_conn_param conn_param = {};
    int ret, i;

    if (!rdma)
        return -ENOTCONN;
    if (rdma->connected)
        return 0;

    /* 清掉 errored 标志 (重连) */
    rdma->errored = false;
    reinit_completion(&rdma->cm_done);

    /* 1. cm_id */
    rdma->cm_id = rdma_create_id(&init_net, powerfs_rdma_cm_event_handler,
                                  rdma, RDMA_PS_TCP, IB_QPT_RC);
    if (IS_ERR(rdma->cm_id)) {
        ret = PTR_ERR(rdma->cm_id);
        rdma->cm_id = NULL;
        pr_err("powerfs_rdma: rdma_create_id failed: %d\n", ret);
        return ret;
    }

    /* 2. 地址解析 → 路由解析 (异步事件, ADDR_RESOLVED 处理时再触发 ROUTE_RESOLVED) */
    ret = pfs_rdma_resolve_dst(conn, rdma->cm_id);
    if (ret)
        goto err_cm_id;
    ret = wait_for_completion_interruptible_timeout(&rdma->cm_done,
                                                    msecs_to_jiffies(PFS_RDMA_ADDR_TIMEOUT +
                                                                     PFS_RDMA_ROUTE_TIMEOUT + 1000));
    if (ret <= 0) {
        pr_err("powerfs_rdma: addr/route resolve timeout (%s:%u)\n",
               conn->addr, conn->port);
        ret = (ret == 0) ? -ETIMEDOUT : ret;
        goto err_cm_id;
    }
    if (rdma->cm_event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        pr_err("powerfs_rdma: addr/route resolve failed: %s\n",
               rdma_event_msg(rdma->cm_event));
        ret = rdma->cm_status ? rdma->cm_status : -EHOSTUNREACH;
        goto err_cm_id;
    }
    /* 路由解析后 cm_id->device 已绑定 */

    /* 3. PD */
    rdma->pd = ib_alloc_pd(rdma->cm_id->device, 0);
    if (IS_ERR(rdma->pd)) {
        ret = PTR_ERR(rdma->pd);
        rdma->pd = NULL;
        pr_err("powerfs_rdma: ib_alloc_pd failed: %d\n", ret);
        goto err_cm_id;
    }

    /* 4. CQ × 2. 用 IB_POLL_SOFTIRQ 初始化 (会调 ib_req_notify_cq 初始 arm + irq_poll_init),
     * 然后覆盖 comp_handler 为本模块的 powerfs_rdma_cq_comp_handler,
     * 直接用 wr_id 模式而绕过 irq_poll 的 ib_cqe->done 路径. */
    rdma->send_cq = ib_alloc_cq(rdma->cm_id->device, rdma,
                                 PFS_RDMA_MAX_SEND_WR * 2, 0, IB_POLL_SOFTIRQ);
    if (IS_ERR(rdma->send_cq)) {
        ret = PTR_ERR(rdma->send_cq);
        rdma->send_cq = NULL;
        pr_err("powerfs_rdma: ib_alloc_cq(send) failed: %d\n", ret);
        goto err_pd;
    }
    rdma->send_cq->comp_handler = powerfs_rdma_cq_comp_handler;

    rdma->recv_cq = ib_alloc_cq(rdma->cm_id->device, rdma,
                                 PFS_RDMA_MAX_RECV_WR * 2, 0, IB_POLL_SOFTIRQ);
    if (IS_ERR(rdma->recv_cq)) {
        ret = PTR_ERR(rdma->recv_cq);
        rdma->recv_cq = NULL;
        pr_err("powerfs_rdma: ib_alloc_cq(recv) failed: %d\n", ret);
        goto err_send_cq;
    }
    rdma->recv_cq->comp_handler = powerfs_rdma_cq_comp_handler;

    /* 5. QP. rdma_create_qp 后 cm_id->qp 自动随 cm_id 销毁, 同时存一份便于直接访问 */
    qp_attr.send_cq       = rdma->send_cq;
    qp_attr.recv_cq       = rdma->recv_cq;
    qp_attr.cap.max_send_wr  = PFS_RDMA_MAX_SEND_WR;
    qp_attr.cap.max_recv_wr  = PFS_RDMA_MAX_RECV_WR;
    qp_attr.cap.max_send_sge = PFS_RDMA_MAX_SGE;
    qp_attr.cap.max_recv_sge = 1;  /* RECV 用单 SGE (整缓冲) */
    qp_attr.cap.max_inline_data = 0;
    qp_attr.sq_sig_type   = IB_SIGNAL_ALL_WR;  /* 所有 SEND 都产生完成 */
    qp_attr.qp_type       = IB_QPT_RC;

    ret = rdma_create_qp(rdma->cm_id, rdma->pd, &qp_attr);
    if (ret) {
        pr_err("powerfs_rdma: rdma_create_qp failed: %d\n", ret);
        goto err_recv_cq;
    }
    rdma->qp = rdma->cm_id->qp;

    /* 6. MR 池 */
    ret = powerfs_rdma_mr_pool_init(rdma->pd, &rdma->ctrl_pool,
                                     PFS_RDMA_CTRL_BUF_NUM, PFS_RDMA_CTRL_BUF_SIZE);
    if (ret)
        goto err_qp;
    ret = powerfs_rdma_mr_pool_init(rdma->pd, &rdma->data_pool,
                                     PFS_RDMA_DATA_BUF_NUM, PFS_RDMA_DATA_BUF_SIZE);
    if (ret)
        goto err_ctrl_pool;

    /* 初始发送 credit = max_send_wr (sq_sig_all, 每发必完成, 完成即归还 credit) */
    atomic_set(&rdma->send_credits, PFS_RDMA_MAX_SEND_WR);

    /* 7. Pre-post RECV */
    for (i = 0; i < PFS_RDMA_MAX_RECV_WR; i++) {
        ret = powerfs_rdma_post_recv(rdma);
        if (ret) {
            pr_err("powerfs_rdma: post_recv %d/%d failed: %d\n",
                   i, PFS_RDMA_MAX_RECV_WR, ret);
            /* 失败不致命: 后续可补 post, 这里继续 connect */
            break;
        }
    }

    /* 8. rdma_connect → ESTABLISHED */
    reinit_completion(&rdma->cm_done);
    conn_param.responder_resources = 1;
    conn_param.initiator_depth     = 1;
    conn_param.retry_count         = 7;
    conn_param.rnr_retry_count    = 7;
    /* qp_num 留 0: rdma_cm 在有 cm_id->qp 时自动用 qp->qp_num */

    ret = rdma_connect(rdma->cm_id, &conn_param);
    if (ret) {
        pr_err("powerfs_rdma: rdma_connect failed: %d\n", ret);
        goto err_data_pool;
    }

    ret = wait_for_completion_interruptible_timeout(&rdma->cm_done,
                                                    msecs_to_jiffies(PFS_RDMA_ADDR_TIMEOUT + 1000));
    if (ret <= 0) {
        pr_err("powerfs_rdma: rdma_connect timeout (%s:%u)\n",
               conn->addr, conn->port);
        ret = (ret == 0) ? -ETIMEDOUT : ret;
        goto err_data_pool;
    }
    if (rdma->cm_event != RDMA_CM_EVENT_ESTABLISHED) {
        pr_err("powerfs_rdma: connect rejected/failed: %s\n",
               rdma_event_msg(rdma->cm_event));
        ret = rdma->cm_status ? rdma->cm_status : -ECONNREFUSED;
        goto err_data_pool;
    }

    /* 9. ESTABLISHED. Phase 1 跳过握手帧交换 (RDMA 服务端尚未实现握手协议) */
    rdma->connected = true;
    pr_info("powerfs_rdma: connected to %s:%u (qp_num=%u)\n",
            conn->addr, conn->port, rdma->qp->qp_num);
    return 0;

err_data_pool:
    powerfs_rdma_mr_pool_free(&rdma->data_pool);
err_ctrl_pool:
    powerfs_rdma_mr_pool_free(&rdma->ctrl_pool);
err_qp:
    rdma_destroy_qp(rdma->cm_id);
    rdma->qp = NULL;
err_recv_cq:
    if (rdma->recv_cq) {
        ib_free_cq(rdma->recv_cq);
        rdma->recv_cq = NULL;
    }
err_send_cq:
    if (rdma->send_cq) {
        ib_free_cq(rdma->send_cq);
        rdma->send_cq = NULL;
    }
err_pd:
    if (rdma->pd) {
        ib_dealloc_pd(rdma->pd);
        rdma->pd = NULL;
    }
err_cm_id:
    if (rdma->cm_id) {
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
    }
    rdma->errored = true;
    return ret;
}

/**
 * powerfs_rdma_disconnect - 拆链 + 销毁 RDMA 资源
 *
 * 顺序:
 *   1. rdma_disconnect → QP 转 ERROR, 在飞 WR 以错误完成
 *   2. 排空 CQ (ib_poll_cq 处理错误完成, 释放 MR/credit)
 *   3. ib_free_cq × 2 (irq_poll_disable 会等在飞 poll 完成)
 *   4. rdma_destroy_id (销毁 cm_id + 内嵌 qp)
 *   5. powerfs_rdma_mr_pool_free × 2
 *   6. ib_dealloc_pd
 *
 * 不负责状态机/回调清理 (由 powerfs_conn_disconnect_one 处理).
 */
void powerfs_rdma_disconnect(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);
    struct ib_wc wcs[PFS_RDMA_WC_BATCH];
    int n, i;

    if (!rdma)
        return;

    rdma->connected = false;
    rdma->errored   = true;
    wake_up(&rdma->send_waitq);

    /* 1. 发起断连 (若 QP 还在) */
    if (rdma->cm_id && rdma->qp) {
        rdma_disconnect(rdma->cm_id);
        /* 等 QP 转 ERROR 让在飞 WR 以错误完成. 软件卡 (RXE) 同步完成;
         * 硬件卡可能需 drain. 这里轮询两个 CQ 各 200ms 兜底.
         * 此函数在 workqueue 上下文执行 (可睡眠), 用 usleep_range 替代 udelay. */
        for (i = 0; i < 200; i++) {
            int got = 0;
            while ((n = ib_poll_cq(rdma->send_cq, PFS_RDMA_WC_BATCH, wcs)) > 0) {
                int j;
                for (j = 0; j < n; j++)
                    powerfs_rdma_process_wc(rdma, &wcs[j]);
                got = 1;
            }
            while ((n = ib_poll_cq(rdma->recv_cq, PFS_RDMA_WC_BATCH, wcs)) > 0) {
                int j;
                for (j = 0; j < n; j++)
                    powerfs_rdma_process_wc(rdma, &wcs[j]);
                got = 1;
            }
            if (!got)
                break;
            usleep_range(1000, 2000);
        }
    }

    /* 2. 销毁 cm_id + 内嵌 qp (rdma_destroy_id 自带 rdma_destroy_qp) */
    if (rdma->cm_id) {
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
        rdma->qp = NULL;
    }

    /* 3. 释放 CQ. ib_free_cq 内部 irq_poll_disable/cancel_work_sync 同步
     *    在飞回调, 返回后保证不再有 comp_handler 触发, 后续释放才安全. */
    if (rdma->send_cq) {
        ib_free_cq(rdma->send_cq);
        rdma->send_cq = NULL;
    }
    if (rdma->recv_cq) {
        ib_free_cq(rdma->recv_cq);
        rdma->recv_cq = NULL;
    }

    /* 4. MR 池 */
    powerfs_rdma_mr_pool_free(&rdma->ctrl_pool);
    powerfs_rdma_mr_pool_free(&rdma->data_pool);

    /* 5. PD */
    if (rdma->pd) {
        ib_dealloc_pd(rdma->pd);
        rdma->pd = NULL;
    }

    /* 清空 rx_done_list (若有残留未消费 entry, 早已在 pool_free 时随 entries 释放) */
    INIT_LIST_HEAD(&rdma->rx_done_list);
    atomic_set(&rdma->recv_posted, 0);
    atomic_set(&rdma->send_credits, 0);
}

/**
 * powerfs_rdma_is_connected - 传输是否就绪
 */
bool powerfs_rdma_is_connected(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);

    return rdma && rdma->connected && !rdma->errored &&
           conn->state == CONN_CONNECTED;
}

/* ============= 帧收发 ============= */

/**
 * powerfs_rdma_send_frame - 非阻塞发送一帧
 *
 * 帧布局与 TCP 一致: HDR(28) + body + data, 一并 memcpy 到 MR 缓冲,
 * 单个 SGE + 单个 SEND WR 发送. SEND 完成时由 CQ handler 释放 MR/credit.
 *
 * 返回值约定 (与 TCP ops 一致):
 *   0       = 已 post, 调用方可视为发送完成
 *   -EAGAIN = 无 credit 或 MR 池空, 调用方重新入队等下次 TX 回调
 *   <0      = 不可恢复错误 (触发断连)
 */
int powerfs_rdma_send_frame(struct powerfs_net_server_conn *conn,
                             struct powerfs_net_frame_hdr *hdr,
                             const void *body, size_t body_len,
                             const void *data, size_t data_len)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);
    struct powerfs_rdma_mr_entry *e;
    struct powerfs_rdma_mr_pool *pool;
    struct ib_sge sge;
    struct ib_send_wr wr;
    const struct ib_send_wr *bad;
    size_t total, off = 0;
    int ret;

    if (!rdma || !rdma->connected || rdma->errored)
        return -ENOTCONN;

    total = POWERFS_NET_FRAME_HDR_SIZE + body_len + data_len;
    if (total > POWERFS_NET_MAX_FRAME) {
        pr_err("powerfs_rdma: frame too large %zu\n", total);
        return -EMSGSIZE;
    }

    /* Phase 1: 仅支持 <= ctrl_pool 缓冲 (64KB) 的控制帧.
     * 大数据帧 (write_needle 1MB) 需 RDMA READ 流程 (后续 Phase 实现),
     * 当前 RECV 缓冲仅 64KB, 发送 >64KB 会导致对端 IB_WC_LOC_LEN_ERR. */
    if (total > PFS_RDMA_CTRL_BUF_SIZE) {
        pr_warn_ratelimited("powerfs_rdma: frame %zu > ctrl buf %d, "
                            "large frames require RDMA READ (not yet impl)\n",
                            total, PFS_RDMA_CTRL_BUF_SIZE);
        return -EOPNOTSUPP;
    }
    pool = &rdma->ctrl_pool;

    /* 防御: 确保帧不超过所选池的缓冲大小 */
    if (total > pool->buf_size) {
        pr_err("powerfs_rdma: frame %zu > pool buf %zu\n", total, pool->buf_size);
        return -EMSGSIZE;
    }

    /* 抢 credit (atomic_dec_if_positive 不阻塞, 没有就 -EAGAIN) */
    if (atomic_dec_if_positive(&rdma->send_credits) < 0)
        return -EAGAIN;

    e = powerfs_rdma_mr_pool_acquire(pool, GFP_ATOMIC);
    if (!e) {
        atomic_inc(&rdma->send_credits);
        return -EAGAIN;
    }

    /* 拼帧: HDR + body + data */
    memcpy(e->buf + off, hdr, POWERFS_NET_FRAME_HDR_SIZE);
    off += POWERFS_NET_FRAME_HDR_SIZE;
    if (body && body_len) {
        memcpy(e->buf + off, body, body_len);
        off += body_len;
    }
    if (data && data_len) {
        memcpy(e->buf + off, data, data_len);
        off += data_len;
    }

    /* 单 SGE 覆盖实际使用的字节 (lkey 对整个 buf 有效, length 可小于) */
    sge.addr   = e->dma;
    sge.length = off;
    sge.lkey   = e->mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id     = (u64)(uintptr_t)e;
    wr.next      = NULL;
    wr.sg_list   = &sge;
    wr.num_sge   = 1;
    wr.opcode    = IB_WR_SEND;
    wr.send_flags = IB_SEND_SIGNALED;

    ret = ib_post_send(rdma->qp, &wr, &bad);
    if (ret) {
        pr_warn_ratelimited("powerfs_rdma: ib_post_send failed: %d\n", ret);
        powerfs_rdma_mr_pool_release(pool, e);
        atomic_inc(&rdma->send_credits);
        rdma->errored = true;
        return ret;
    }

    return 0;
}

/**
 * powerfs_rdma_has_rx_data - rx_done_list 是否有已完成 RECV
 */
bool powerfs_rdma_has_rx_data(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);
    unsigned long flags;
    bool has;

    if (!rdma)
        return false;

    spin_lock_irqsave(&rdma->rx_done_lock, flags);
    has = !list_empty(&rdma->rx_done_list);
    spin_unlock_irqrestore(&rdma->rx_done_lock, flags);
    return has;
}

/**
 * powerfs_rdma_recv_frame - 取一个已完成 RECV, 拷出帧, 重 post RECV
 *
 * 返回值:
 *   0       = 一帧完整, hdr_out/body_buf/data_buf 已填
 *   -EAGAIN = 无已完成 RECV
 *   <0      = 错误 (magic/CRC 校验失败, 缓冲不够, peer 断连)
 */
int powerfs_rdma_recv_frame(struct powerfs_net_server_conn *conn,
                             void *body_buf, size_t body_cap,
                             void *data_buf, size_t data_cap,
                             struct powerfs_net_frame_hdr *hdr_out,
                             size_t *body_len_out,
                             size_t *data_len_out)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);
    struct powerfs_rdma_mr_entry *e;
    struct powerfs_net_frame_hdr hdr;
    size_t body_len, data_len, frame_total;
    unsigned long flags;
    int ret;

    if (!rdma)
        return -ENOTCONN;

    if (rdma->errored && !powerfs_rdma_has_rx_data(conn))
        return -ENOTCONN;

    /* 1. 摘一个已完成 RECV */
    spin_lock_irqsave(&rdma->rx_done_lock, flags);
    if (list_empty(&rdma->rx_done_list)) {
        spin_unlock_irqrestore(&rdma->rx_done_lock, flags);
        return -EAGAIN;
    }
    e = list_first_entry(&rdma->rx_done_list,
                         struct powerfs_rdma_mr_entry, list);
    list_del_init(&e->list);
    spin_unlock_irqrestore(&rdma->rx_done_lock, flags);

    /* 2. 解码帧头 + CRC 校验 */
    if (!powerfs_net_frame_hdr_decode(e->buf, POWERFS_NET_FRAME_HDR_SIZE, &hdr)) {
        pr_warn_ratelimited("powerfs_rdma: invalid frame header (CRC/magic)\n");
        ret = -EPROTO;
        goto repost;
    }

    body_len = hdr.body_len;
    if (body_len > hdr.data_len)
        body_len = hdr.data_len;  /* 钳制异常 */
    data_len = (hdr.data_len >= body_len) ? (hdr.data_len - body_len) : 0;

    frame_total = POWERFS_NET_FRAME_HDR_SIZE + body_len + data_len;
    if (frame_total > e->size) {
        pr_warn_ratelimited("powerfs_rdma: frame %zu > recv buf %zu\n",
                            frame_total, e->size);
        ret = -EMSGSIZE;
        goto repost;
    }
    if (body_len > body_cap || data_len > data_cap) {
        pr_warn_ratelimited("powerfs_rdma: body/data cap insufficient "
                            "(body %zu/%zu data %zu/%zu)\n",
                            body_len, body_cap, data_len, data_cap);
        ret = -ENOSPC;
        goto repost;
    }

    /* 3. 拷出 body/data 到调用方缓冲 */
    if (body_len > 0)
        memcpy(body_buf, e->buf + POWERFS_NET_FRAME_HDR_SIZE, body_len);
    if (data_len > 0)
        memcpy(data_buf, e->buf + POWERFS_NET_FRAME_HDR_SIZE + body_len, data_len);

    if (hdr_out)
        *hdr_out = hdr;
    if (body_len_out)
        *body_len_out = body_len;
    if (data_len_out)
        *data_len_out = data_len;

    ret = 0;

repost:
    /* 4. 重 post RECV (用同一 MR entry, 接收下一帧) */
    {
        struct ib_sge sge;
        struct ib_recv_wr wr;
        const struct ib_recv_wr *bad;

        sge.addr   = e->dma;
        sge.length = e->size;
        sge.lkey   = e->mr->lkey;
        wr.wr_id   = (u64)(uintptr_t)e;
        wr.next    = NULL;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        if (ib_post_recv(rdma->qp, &wr, &bad)) {
            /* post 失败: entry 还回所属池, recv_posted 减 1 */
            struct powerfs_rdma_mr_pool *pool = pfs_mr_entry_pool(rdma, e);
            powerfs_rdma_mr_pool_release(pool, e);
            atomic_dec(&rdma->recv_posted);
            rdma->errored = true;
            /* 若上面解析失败, 以原错误返回; 否则覆盖为 post 失败 */
            if (ret == 0)
                ret = -EIO;
        }
        /* 成功: recv_posted 不变 (摘下又重新 post), entry 仍在 QP */
    }

    return ret;
}

/* ============= 通知控制 ============= */

/**
 * powerfs_rdma_enable_rx_notify - 显式 arm RX 通知
 *
 * CQ 在 ib_alloc_cq(SOFTIRQ) 时已 ib_req_notify_cq arm 过, 每次
 * comp_handler 内部也会 re-arm, 故此处 no-op (与 TCP 一致).
 * 若有 rx_done_list 残留未消费, 主动通知一次 RX 调度器.
 */
void powerfs_rdma_enable_rx_notify(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);
    if (rdma && rdma->owner && powerfs_rdma_has_rx_data(conn))
        pfs_rx_callback(rdma->owner);
}

/**
 * powerfs_rdma_enable_tx_notify - 显式 arm TX 通知
 *
 * SEND 完成回调自动归 credit + pfs_tx_callback, 此处主要兜底:
 * 若连接刚恢复且有 credit, 主动通知 TX 调度器继续.
 */
void powerfs_rdma_enable_tx_notify(struct powerfs_net_server_conn *conn)
{
    struct powerfs_rdma_conn *rdma = pfs_conn_rdma(conn);
    if (rdma && rdma->owner && atomic_read(&rdma->send_credits) > 0)
        pfs_tx_callback(rdma->owner);
}

/* ============= 全局 init/fini ============= */

/**
 * powerfs_rdma_global_init - 模块加载时初始化
 *
 * Phase 1 不需要全局 ib_register_client (客户端按需用 rdma_create_id
 * 即可发现设备). 留作后续 Phase (如服务端 listen / 设备热插拔) 接入.
 */
int powerfs_rdma_global_init(void)
{
    pr_info("powerfs_rdma: global init (transport=rdma, kernel RDMA ready)\n");
    return 0;
}

void powerfs_rdma_global_exit(void)
{
    pr_info("powerfs_rdma: global exit\n");
}

/* ============= transport ops 表 =============
 *
 * send_frame / recv_frame 的 transport.h 签名带 req 参数 (用于 TCP 路径
 * partial-send 续传), RDMA 每帧一个 WR 无 partial, 故 wrapper 忽略 req.
 * recv_frame 的 transport.h 签名只取 conn, 用 conn 内置 rx_body_buf /
 * rx_data_buf / rx_cur_hdr 接收, 对齐 pfs_process_receive 的预期.
 */
static int rdma_send_frame(struct powerfs_net_server_conn *conn,
                           struct powerfs_net_frame_hdr *hdr,
                           const __u8 *body, size_t body_len,
                           const __u8 *data, size_t data_len,
                           struct powerfs_request *req)
{
    /* req unused: RDMA 单 WR 即发完整帧, 不需要 partial-send 续传 */
    (void)req;
    return powerfs_rdma_send_frame(conn, hdr, body, body_len, data, data_len);
}

static int rdma_recv_frame(struct powerfs_net_server_conn *conn)
{
    size_t body_len = 0, data_len = 0;
    int ret;

    ret = powerfs_rdma_recv_frame(conn,
                                   conn->rx_body_buf, POWERFS_NET_MAX_BODY,
                                   conn->rx_data_buf, POWERFS_NET_MAX_DATA,
                                   &conn->rx_cur_hdr,
                                   &body_len, &data_len);
    if (ret)
        return ret;

    /* 与 TCP 路径 pfs_rx_step 一致: 把已收到的 body/data 长度写入 conn 字段,
     * 供 pfs_rx_dispatch 解码 TLV / 路由 reply. */
    conn->rx_body_total = body_len;
    conn->rx_data_total = data_len;
    conn->rx_body_got   = body_len;
    conn->rx_data_got   = data_len;
    conn->rx_hdr_got    = POWERFS_NET_FRAME_HDR_SIZE;
    conn->rx_phase      = 3;  /* done */
    return 0;
}

const struct powerfs_transport_ops powerfs_rdma_ops = {
    .name             = "rdma",
    .type             = POWERFS_TRANSPORT_RDMA,

    .connect          = powerfs_rdma_connect,
    .disconnect       = powerfs_rdma_disconnect,
    .is_connected     = powerfs_rdma_is_connected,

    .send_frame       = rdma_send_frame,
    .has_rx_data      = powerfs_rdma_has_rx_data,
    .recv_frame       = rdma_recv_frame,

    .enable_rx_notify = powerfs_rdma_enable_rx_notify,
    .enable_tx_notify = powerfs_rdma_enable_tx_notify,

    .init_conn        = powerfs_rdma_init_conn,
    .fini_conn        = powerfs_rdma_fini_conn,

    .global_init      = powerfs_rdma_global_init,
    .global_exit      = powerfs_rdma_global_exit,
};
