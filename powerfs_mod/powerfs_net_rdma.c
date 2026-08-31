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

/* [RC16b DIAGS] 每 conn 前 N 个 send/recv frame 打 pr_info, 用于定位 "deadline exceeded"
 * 断点在 5 步: send_enter/post_send/SEND_WC/RECV_WC/recv_consume.
 * 1600 足够覆盖 7 连接 * 200 帧 (先前 48 太小被 2 个历史连接耗尽). */
#define PFS_RDMA_DIAG_N          1600
static atomic_t g_rdma_diag_cnt = ATOMIC_INIT(0); /* 全局诊断计数器, 超过 PFS_RDMA_DIAG_N 停止打 pr_info */
static inline bool pfs_rdma_diag_ok(void) {
    int v = atomic_inc_return(&g_rdma_diag_cnt);
    return v <= PFS_RDMA_DIAG_N;
}
#define PFS_RDMA_DIAG(fmt, args...) do { if (pfs_rdma_diag_ok()) pr_info("pfs_rdma_diag " fmt, ##args); } while (0)

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
 * powerfs_rdma_mr_pool_init - 分配缓冲池并设置每个 entry 的 DMA 映射与 lkey
 *
 * 历史问题 (均已修复, 记录以防回归):
 *   V1: ib_alloc_mr(IB_MR_TYPE_MEM_REG) + ib_map_mr_sg — mlx5 内部创建
 *       UMR "free" mkey (access=RELAXED_ORDERING only, free=1).
 *       ib_map_mr_sg 仅写 MTT 页表, 不 UMR 清 free=0 / 写 access.
 *       RC 首次 RECV (HCA LOCAL_WRITE) → IB_WC_LOC_PROT_ERR (status=4).
 *       dmesg: "handshake resp recv WC error" + dump_cqe LOC_PROT.
 *   V2: dev->ops.get_dma_mr per entry *手动设置 access* — 确实 bypass
 *       free mkey, 走 PA-mode mkey. 但 mlx5_ib_get_dma_mr **未初始化
 *       mr->ibmr.device/pd** (仅 ib_alloc_pd 内部调用时 core/verbs.c:306-310
 *       才赋值). mr_pool_free → ib_dereg_mr → mr->device->ops.dereg_mr
 *       → NULL deref. Oops RIP ib_dereg_mr_user+0x39 (0x1e8 deref).
 *
 *  V3 (=current): 复用 PD 的 local_dma_lkey. ib_alloc_pd() 内部已通过
 *      ops.get_dma_mr 分配了 __internal_mr (access=LOCAL_WRITE /
 *      REMOTE_READ/WRITE / RELAXED_ORDERING), 并完整初始化
 *      mr->{device,pd,type,uobject,need_inval}. lkey=__internal_mr->lkey
 *      = pd->local_dma_lkey. 此 lkey 为 PA-mode 全地址, 可直接与
 *      ib_dma_map_single 得到的 DMA 地址配对使用 SGE.
 *
 * 好处:
 *   - 不额外占用每个 entry 一个 mkey (节省 QP MPT cache / CMDIF 资源)
 *   - 无 get_dma_mr 返回 mr 未 init 的风险
 *   - 与 handshake send buffer 路径 (已经用 pd->local_dma_lkey) 统一
 */
int powerfs_rdma_mr_pool_init(struct ib_pd *pd,
                              struct powerfs_rdma_mr_pool *pool,
                              int num_entries, size_t buf_size)
{
    struct ib_device *dev = pd->device;
    int i, ret;

    pool->entries = kcalloc(num_entries, sizeof(*pool->entries), GFP_KERNEL);
    if (!pool->entries)
        return -ENOMEM;

    INIT_LIST_HEAD(&pool->free_list);
    spin_lock_init(&pool->lock);
    pool->total = num_entries;
    atomic_set(&pool->free, num_entries);
    pool->buf_size = buf_size;
    pool->dev = dev;   /* 为 mr_pool_free 保存 dev 指针 */

    /* 安全网: local_dma_lkey 必须有效. 通常 mlx5 IBK_LOCAL_DMA_LKEY=0
     * (不走 dev->local_dma_lkey), 此时 ib_alloc_pd 会 fallback 到
     * internal DMA MR + pd->local_dma_lkey = internal_mr->lkey.
     * 两条路径 local_dma_lkey 都非 0. 0xfffffffe/-2 保留 "invalid lkey". */
    if (unlikely(pd->local_dma_lkey == 0 ||
                 pd->local_dma_lkey == 0xfffffffeU)) {
        pr_err("powerfs_rdma: pd->local_dma_lkey invalid (0x%08x)\n",
               pd->local_dma_lkey);
        kfree(pool->entries);
        pool->entries = NULL;
        return -EIO;
    }

    for (i = 0; i < num_entries; i++) {
        struct powerfs_rdma_mr_entry *e = &pool->entries[i];
        u64 dma;

        e->pool_idx = (buf_size >= PFS_RDMA_DATA_BUF_SIZE) ? 1 : 0;
        e->entry_id = i;  /* [RC18d] 条目唯一编号, 用于诊断 */
        /* [RC15 FIX] 每个 MR entry 内嵌 ib_cqe.done 回调 = 统一 cqe_done.
         * 未来每个 WR 挂 &e->cqe (RECV/SEND), WC 完成时内核通过
         * wc->wr_cqe->done 路由回 powerfs_rdma_cqe_done. */
        e->cqe.done = powerfs_rdma_cqe_done;

        /* 页对齐缓冲 */
        e->buf = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO | __GFP_COMP,
                                          get_order(buf_size));
        if (!e->buf) {
            ret = -ENOMEM;
            goto err;
        }
        e->size = buf_size;

        dma = ib_dma_map_single(dev, e->buf, buf_size, DMA_BIDIRECTIONAL);
        if (ib_dma_mapping_error(dev, dma)) {
            pr_err("powerfs_rdma: dma_map_single failed\n");
            ret = -EIO;
            goto err;
        }
        e->dma = dma;

        /* 共用 pd->local_dma_lkey; entry 仍持 mr 指针用于兼容 release/acquire
         * 引用 (虽然实际解引用仅限 mr_pool_free; 此处置 NULL 让 free 跳过). */
        e->mr = NULL;

        list_add_tail(&e->list, &pool->free_list);
    }

    pr_info("powerfs_rdma: MR pool %d*%zu bytes using pd->local_dma_lkey=0x%x\n",
            num_entries, buf_size, pd->local_dma_lkey);
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
 * V3: e->mr 恒 NULL (lkey 共用 pd->local_dma_lkey). 故不需要 dereg_mr.
 * 仍需 ib_dma_unmap_single 解映射 → free_pages. dev 指针由 *pool->dev
 * 获取 (pool_init 存在但结构没这字段, 所以从 ctrl_pool/data_pool 外部
 * rdma->device 传入不便; 改为从首个 entry 的映射推断 dev — 不可能, 需
 * dev 做 unmap 但此时外部已 destroy qp, rdma 字段还在; 因此在 pool_free
 * 接口扩展 1 参? 简单起见: 复用 ctrl/data pool 所在 rdma 结构的 dev.
 * 为避免改动函数签名, 在 entry 新增 owner 指针太复杂. 我们用: 记录
 * "pool 初始化时 dev 指针" 在 pool 结构新增字段.
 *
 * 实际: 保持兼容性, 但 e->mr == NULL 时, 不能再依赖 e->mr->device.
 * 故在 struct powerfs_rdma_mr_pool 新增 dev 字段并在 mr_pool_init 中存.
 * 此处读取该字段做 dma_unmap.
 */
void powerfs_rdma_mr_pool_free(struct powerfs_rdma_mr_pool *pool)
{
    struct ib_device *dev = pool->dev;
    int i;

    if (!pool->entries)
        return;

    /* e->mr 恒 NULL (V3); 但保留对未来 MR-back 的兼容性: 仍
     * deregister 非空 entries (V1/V2 热补丁/回滚时会用到). */
    for (i = 0; i < pool->total; i++) {
        struct powerfs_rdma_mr_entry *e = &pool->entries[i];

        if (e->mr) {
            if (e->mr->device)
                ib_dereg_mr(e->mr);
            e->mr = NULL;
        }
    }
    for (i = 0; i < pool->total; i++) {
        struct powerfs_rdma_mr_entry *e = &pool->entries[i];

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
    pool->dev = NULL;
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

/* ============= CQ 完成回调 (RC15: 标准 ib_cqe->done 模式) ============= */

/* [前向声明] powerfs_rdma_process_wc 在本文件后面 L400+ 定义;
 * pfs_unpack_cqe_wc / powerfs_rdma_cqe_done 需要在前面调用它. */
static void powerfs_rdma_process_wc(struct powerfs_rdma_conn *rdma,
                                    struct ib_wc *wc);

/* 验证 wc->wr_cqe 是否属于当前 rdma 连接、回调签名正确.
 * [RC15a CRITICAL] 绝不读写 wc->wr_id 字段 (与 wc->wr_cqe 是 union, 共享存储;
 * 写 wr_id 会覆盖 wr_cqe 指针 → 后续 deref #PF Oops).
 * 返回: 0=正常, <0=非法 (已 pr_warn_ratelimited). */
static inline int
pfs_validate_cqe(struct powerfs_rdma_conn *rdma, struct ib_wc *wc)
{
    struct ib_cqe *cqe = wc->wr_cqe;

    if (unlikely(!cqe)) {
        pr_warn_ratelimited("powerfs_rdma: WC with NULL wr_cqe (status=%d op=%d)\n",
                            wc->status, wc->opcode);
        return -EINVAL;
    }

    if (unlikely(cqe->done != powerfs_rdma_cqe_done)) {
        pr_warn_ratelimited("powerfs_rdma: WC wr_cqe->done mismatch "
                            "(got %px expected %px status=%d op=%d)\n",
                            cqe->done, powerfs_rdma_cqe_done,
                            wc->status, wc->opcode);
        return -EPROTO;
    }

    /* cqe 要么是内嵌 rdma->hs_send_cqe, 要么是 mr_entry->cqe.
     * 两种情况 container_of 都能得到合法结构对象, 进一步校验
     * done 指针一致即视为有效 (由外层 process_wc 拆分 hs/entry). */
    return 0;
}

/* [ROOT CAUSE 15 FIX] 标准 ib_cqe->done 回调.
 * 内核 __ib_process_cq(cq.c L96): wc->wr_cqe->done(cq, wc).
 * 不直接覆盖 cq->comp_handler, 避免 WORKQUEUE cancel 时 comp_handler 被
 * 置 NULL 导致后续 ib_process_cq_direct 里 call *NULL → RIP=0x0 Oops.
 * [RC15a] 入口仅验证 cqe, 随后 process_wc 内部基于 wc->wr_cqe 反推.
 * 绝不触碰 wc->wr_id 字段 (union 重叠). */
void powerfs_rdma_cqe_done(struct ib_cq *cq, struct ib_wc *wc)
{
    struct powerfs_rdma_conn *rdma = cq ? cq->cq_context : NULL;

    if (unlikely(!rdma))
        return;

    if (pfs_validate_cqe(rdma, wc) != 0)
        return;
    powerfs_rdma_process_wc(rdma, wc);
}

/* 处理单个 WC. IRQ / worker 上下文, 不可睡眠.
 * [RC15a FIX union 重叠 bug] 所有 ib_send_wr/ib_recv_wr 的 wr_id/wr_cqe
 * 是 union, 写一个覆盖另一个. RC15 旧代码先写 wr_cqe 再写 wr_id 导致
 * HCA 把 wr_id 当成 wr_cqe 指针写回 wc → __ib_process_cq deref magic
 * 值 "HSWR"(0x48535752) → CR2=0x48535752 #PF Oops.
 * 新规则: WR 只设置 wr_cqe, 永不写 wr_id. 本函数从 wc->wr_cqe 反推:
 *   - cqe == &rdma->hs_send_cqe → 握手 SEND (is_hs_send_wr=true, e=NULL)
 *   - 否则 → container_of(cqe, mr_entry, cqe) → e. */
static void powerfs_rdma_process_wc(struct powerfs_rdma_conn *rdma,
                                   struct ib_wc *wc)
{
    struct powerfs_rdma_mr_entry *e = NULL;
    struct powerfs_net_server_conn *conn = rdma->owner;
    bool is_hs_send_wr;
    struct ib_cqe *cqe = wc->wr_cqe;

    /* 入口安全校验: 每个 WR 必须设置 wr_cqe (done = powerfs_rdma_cqe_done) */
    if (unlikely(!cqe || cqe->done != powerfs_rdma_cqe_done)) {
        pr_warn_ratelimited("powerfs_rdma: WC bad wr_cqe=%px done=%px expect=%px (status=%d op=%d)\n",
                            cqe, cqe ? cqe->done : NULL, powerfs_rdma_cqe_done,
                            wc->status, wc->opcode);
        return;
    }

    is_hs_send_wr = (cqe == &rdma->hs_send_cqe);
    if (!is_hs_send_wr)
        e = container_of(cqe, struct powerfs_rdma_mr_entry, cqe);
    if (!e && !is_hs_send_wr) {
        pr_warn_ratelimited("powerfs_rdma: WC not hs_send and NULL container e (status=%d op=%d)\n",
                            wc->status, wc->opcode);
        return;
    }

    /* PFSN 握手阶段拦截: 首个 SEND(hs_send_cqe)/RECV(mr_entry cqe) WC 走握手专用完成路径,
     * 不进入通用 rx_done_list / tx_callback. 失败立刻标记错误并唤醒两端. */
    if (unlikely(rdma->handshake_in_progress)) {
        if (wc->status != IB_WC_SUCCESS) {
            rdma->errored = true;
            if (e && (e->pool_idx == 0 || e->pool_idx == 1)) {
                struct powerfs_rdma_mr_pool *pool = pfs_mr_entry_pool(rdma, e);
                powerfs_rdma_mr_pool_release(pool, e);
            }
            /* 双端都唤醒避免 connect 代码永久睡眠 */
            complete(&rdma->hs_send_done);
            complete(&rdma->hs_recv_done);
            return;
        }

        if (wc->opcode & IB_WC_RECV) {
            /* [RC17 FIX 去重修正] 握手响应 RECV: 只有 !dup 的首个 WC
             * 是 HCA 真正消费了 1 个 pre-posted RECV WR. 后续伪重复
             * WC 是 RDMA 重试/中间件伪完成, 不真实消耗新 RQ entry;
             * dup=true 时释放 entry + repost_recv 会导致 recv_posted
             * 净膨胀 N (观察值 36/38/34 vs 标称 32), 最终 MR 池耗尽.
             * 正确语义: 仅 !dup 路径释放 MR + repost 1 次; dup 路径
             * 完全不触碰池状态. */
            size_t n = min_t(size_t, wc->byte_len, sizeof(rdma->hs_resp));
            bool dup = rdma->hs_recv_handled;
            PFS_RDMA_DIAG("WC_HS_RECV byte_len=%u qp_num=%u dup=%d\n",
                          wc->byte_len,
                          rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu,
                          dup ? 1 : 0);
            if (!dup) {
                if (n > 0 && e)
                    memcpy(rdma->hs_resp, e->buf, n);
                rdma->hs_resp_len = n;
                rdma->hs_recv_handled = true;
                complete(&rdma->hs_recv_done);
                /* 唯一一次真实消费: 释放 MR entry + 重 post 1 个 RECV */
                if (e) {
                    struct powerfs_rdma_mr_pool *pool = pfs_mr_entry_pool(rdma, e);
                    powerfs_rdma_mr_pool_release(pool, e);
                }
                if (likely(rdma->qp && !rdma->errored)) {
                    int repost_rc = powerfs_rdma_post_recv(rdma);
                    if (repost_rc)
                        pr_warn_ratelimited("powerfs_rdma: handshake repost_recv failed %d\n", repost_rc);
                }
            } else {
                /* [RC17] 伪重复完成: 不触碰池状态 (避免计数器膨胀 /
                 * 状态机错乱). 同一批 18B 数据被伪完成 N-1 次. */
                PFS_RDMA_DIAG("WC_HS_RECV DUP skip_release qp_num=%u\n",
                              rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu);
            }
            return;
        } else if (is_hs_send_wr) {
            /* [RC17 FIX 去重修正] 握手请求 SEND: 仅 !dup 的首个 WC
             * 表示真正消耗了 send_credit. dup 是 RC 重试伪完成, 重复
             * 增 credit 会让 credits 超过 max_inline → WR 调度错误. */
            bool dup = rdma->hs_send_handled;
            if (!dup) {
                atomic_inc(&rdma->send_credits);
                rdma->hs_send_handled = true;
                complete(&rdma->hs_send_done);
                PFS_RDMA_DIAG("WC_HS_SEND byte_len=%u qp_num=%u credits=%d dup=0\n",
                              wc->byte_len,
                              rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu,
                              atomic_read(&rdma->send_credits));
            } else {
                PFS_RDMA_DIAG("WC_HS_SEND DUP skip_credit qp_num=%u dup=1\n",
                              rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu);
            }
            return;
        } else {
            /* 兜底 fallback: 同 handshake SEND 语义, 去重 credit + complete */
            bool dup = rdma->hs_send_handled;
            if (!dup) {
                if (e) {
                    struct powerfs_rdma_mr_pool *pool = pfs_mr_entry_pool(rdma, e);
                    powerfs_rdma_mr_pool_release(pool, e);
                }
                atomic_inc(&rdma->send_credits);
                rdma->hs_send_handled = true;
                complete(&rdma->hs_send_done);
            }
            return;
        }
    }

    if (wc->status != IB_WC_SUCCESS) {
        /* 错误完成: QP 进入 ERROR, 后续 WR 也会以错误完成. 释放资源,
         * 标记连接错误, 触发断连 (由 RX/TX 调度器或 disconnect_work 处理). */
        rdma->errored = true;
        if (e && (e->pool_idx == 0 || e->pool_idx == 1)) {
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
        /* [RC17 ROOT CAUSE 18 FIX] 合法 post-handshake 帧最小 = HDR(28B).
         * 任何 wc->byte_len < POWERFS_NET_FRAME_HDR_SIZE 的 RECV 都是:
         *   (1) 延迟到达的 handshake 响应伪重复 (HCA 重试伪完成 18B)
         *       此时 handshake_in_progress 已 false 被通用路径捕获;
         *   (2) 其他损坏/截断帧.
         * 不加入 rx_done_list (否则 rdma_recv_frame hdr_decode 必然失败
         * → return -EINVAL → pfs_process_receive L1707 触发 disconnect_work,
         * 造成 1.1s 的 connect→handshake→disconnect→reconnect 死循环).
         * 正确处理: 直接释放 MR 回池 + repost_recv, 打印一条 diag 跳过. */
        if (unlikely(wc->byte_len < POWERFS_NET_FRAME_HDR_SIZE)) {
            /* [RC18b FIX F ROOT CAUSE 20 — IB verbs 契约正确语义: RECYCLE]
             *
             * 背景: 每个 RECV WC 完成 (即使数据是伪重复的 18B PFSN handshake 响应
             * 或损坏帧) 都对应 HCA 从 RQ 上真实消费了一个 posted WR. 条目被
             * HCA 硬件移出 RQ, DMA 写了数据后触发 CQ event → 不管 payload 是否
             * 有语义价值, 这个 RQ slot 已空.
             *
             * RC18 Fix E 错误做法: return 零池操作 → 每个伪 dup 泄漏 1 个 ctrl
             *   MR entry. 典型 dup 数: 31/33 per qp > 初始 pre-post 32. 净效果:
             *   pool.free = 0 → 后续 powerfs_rdma_send_frame 中 pool_acquire()
             *   取 ctrl MR NULL → POST_SEND_EAGAIN pool_empty → ls/write/cat
             *   全 EAGAIN Resource temporarily unavailable.
             *
             * Fix F (正确语义): 必须 ①把 MR entry 释放回 free pool 维持可用
             *   count; ②调用 post_recv() 再投递一个新的 RECV WR 到 HCA RQ
             *   (否则 RQ 深度逐步下降 → 真发数据时触发 RNR RETRY_EXC).
             *
             * 关于 RC17 观察到的 90+ DISCARD: 那是硬件/对端 RC retry_count=3
             *   rnr_retry_count=7 产生的真实伪完成数量, 不是软件 release+repost
             *   的循环. dup 总数是硬件重试 bound (≤ retry × N hop = ≤ 60 量级),
             *   不是无限. pool 是 FIFO (非 LIFO stack), release→acquire 不
             *   会无限拿到同一个 entry, buffer 每次 repost 的内容不会是相同
             *   旧 PFSN 18B.
             *
             * 注意: 绝对不加入 rx_done_list (hdr_decode 必失败 → 触发错误断
             * 连). 绝对不通知 RX callback (无真实帧). 这两点与 Fix E 一致.
             *
             * [RC18d Fix J 增强] 打印放在 recycle ops 之后, 带 repost_rc /
             *   recv_posted / pool.free before-after 值方便排错. pool_idx
             *   是 TYPE tag (0=ctrl 1=data) 改为 entry_id (0..N-1 唯一条目ID). */
            static atomic_t pfs_short_recv_total_discarded;
            const int tot = atomic_inc_return(&pfs_short_recv_total_discarded);
            const unsigned int blen = wc->byte_len;
            const u32 qpn = rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu;
            const int eid = e ? e->entry_id : -1;
            struct powerfs_rdma_mr_pool *pool = e ? pfs_mr_entry_pool(rdma, e) : NULL;
            const int free_before = pool ? atomic_read(&pool->free) : -1;
            int repost_rc = 0;

            if (tot == 16 || tot == 32 || tot == 64 || tot == 128)
                pr_warn_ratelimited("powerfs_rdma: %d post-handshake short_recv dups "
                                    "discarded+recycled (qp=%d, pool release+repost)\n",
                                    tot, qpn);
            /* [Fix F] ① 归还 ctrl MR entry 到 pool (SEND 需要它!); */
            if (e)
                powerfs_rdma_mr_pool_release(pool, e);
            /* [Fix F] ② 重投一个新 RECV WR 给 HCA RQ 维持深度. */
            if (likely(rdma->qp && !rdma->errored)) {
                repost_rc = powerfs_rdma_post_recv(rdma);
                if (repost_rc)
                    pr_warn_ratelimited("powerfs_rdma: short_recv repost_recv failed %d\n",
                                        repost_rc);
            }
            /* [Fix J] 完成 recycle 后打印最新状态. */
            {
                const int rp = atomic_read(&rdma->recv_posted);
                const int free_after = pool ? atomic_read(&pool->free) : -1;
                PFS_RDMA_DIAG("WC_RECV SHORT (<HDR) DISCARD_RECYCLE byte_len=%u qp_num=%u entry_id=%d tot=%d repost_rc=%d recv_posted=%d pool_free_before=%d after=%d\n",
                              blen, qpn, eid, tot, repost_rc, rp, free_before, free_after);
            }
            return;
        }
        /* RECV 完成: entry 加入 rx_done_list, 由 recv_frame 消费 */
        unsigned long flags;
        PFS_RDMA_DIAG("WC_RECV byte_len=%u qp_num=%u recv_posted=%d entry_id=%d\n",
                      wc->byte_len,
                      rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu,
                      atomic_read(&rdma->recv_posted),
                      e ? e->entry_id : -1);
        spin_lock_irqsave(&rdma->rx_done_lock, flags);
        list_add_tail(&e->list, &rdma->rx_done_list);
        spin_unlock_irqrestore(&rdma->rx_done_lock, flags);
        /* 唤醒 RX 调度器 (pfs_rx_callback 内部 spin_lock_bh 安全) */
        pfs_rx_callback(conn);
    } else {
        /* SEND 完成: 释放 MR 回池, 增 credit, 通知 TX.
         * (正常数据阶段, wr_id 必是 MR 指针; 非 MR 情形则只增 credit). */
        PFS_RDMA_DIAG("WC_SEND byte_len=%u qp_num=%u credits_old=%d entry_id=%d\n",
                      wc->byte_len,
                      rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu,
                      atomic_read(&rdma->send_credits),
                      e ? e->entry_id : -1);
        if (e) {
            struct powerfs_rdma_mr_pool *pool = pfs_mr_entry_pool(rdma, e);
            powerfs_rdma_mr_pool_release(pool, e);
        }
        atomic_inc(&rdma->send_credits);
        pfs_tx_callback(conn);
    }
}

/* [ROOT CAUSE 15 FIX] 不再使用自定义 comp_handler.
 * 旧做法: 直接覆盖 cq->comp_handler = powerfs_rdma_cq_comp_handler.
 * 违反内核契约: cq.c L248-258 根据 poll_ctx (WORKQUEUE/SOFTIRQ/DIRECT)
 * 设置 comp_handler 为内部 dispatch 函数; cancel_work_sync/cq_cleanup 会
 * 把 comp_handler 置 NULL. 随后 drain 阶段 ib_process_cq_direct →
 * __ib_process_cq L109: 若 wc->wr_cqe=NULL (老 wr_id 模式) 走 else
 * WARN branch, 最后调 cq->comp_handler 会 call *NULL → RIP=0x0 #PF Oops.
 *
 * 新标准 (RC15): 每个 WR 挂 ib_cqe* → wc->wr_cqe != NULL,
 * __ib_process_cq L96 直接执行 wc->wr_cqe->done(cq, wc) (见 powerfs_rdma_cqe_done),
 * 完全不依赖 cq->comp_handler 字段, 也不存在 cancel 后 NULL 调用问题. */

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
        /* 路由就绪或连接已建立: 通知连接线程.
         * 注意: 防止后续 REJECTED/DISCONNECTED 在握手过程中过早覆盖
         * ESTABLISHED → Step 9 已进入但 wait 尚未返回的 race.
         * cm_event 只从 INIT/ADDR/ROUTE_RESOLVED 状态升级为 ESTABLISHED,
         * 一旦 ESTABLISHED 已记录, 不被错误事件覆盖 (握手阶段 errored 单独标记). */
        pr_info("powerfs_rdma: cm event %s (old=%s) qp_num=%u jiffies=%lu\n",
                rdma_event_msg(event->event),
                rdma_event_msg(rdma->cm_event),
                rdma->qp ? rdma->qp->qp_num : 0, jiffies);
        if (rdma->cm_event != RDMA_CM_EVENT_ESTABLISHED)
            rdma->cm_event = event->event;
        rdma->cm_status = 0;
        complete(&rdma->cm_done);
        break;

    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
        pr_warn("powerfs_rdma: cm event %s (status=%d, old_event=%s, errored=%d) qp_num=%u jiffies=%lu\n",
                rdma_event_msg(event->event), event->status,
                rdma_event_msg(rdma->cm_event), rdma->errored,
                rdma->qp ? rdma->qp->qp_num : 0, jiffies);
        /* 关键: 如果已经是 ESTABLISHED, 说明握手阶段发生了异步断连.
         * 保留 ESTABLISHED 让 connect 流程进入 Step 9 (或处理握手 WC),
         * 只单独标记 errored=true 让握手路径检查失败. */
        if (rdma->cm_event != RDMA_CM_EVENT_ESTABLISHED) {
            rdma->cm_event = event->event;
            rdma->cm_status = (event->status < 0) ? event->status : -ECONNREFUSED;
            complete(&rdma->cm_done);
        }
        rdma->errored = true;
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

    /* 单条 SGE 覆盖整个 ctrl 池缓冲, 对端可发不超过该长度的帧.
     * lkey = pd->local_dma_lkey (PA-mode full access 共用),
     * e->mr 已废弃为 NULL. */
    sge.addr   = e->dma;
    sge.length = e->size;
    sge.lkey   = rdma->pd->local_dma_lkey;

    /* [RC15a FIX union 重叠] RECV WR 的 wr_id/wr_cqe 是 union → 只写 wr_cqe,
     * 永不写 wr_id (否则覆盖 wr_cqe → __ib_process_cq 调 *wr_id #PF).
     * WC 完成后 wc->wr_cqe = &e->cqe, process_wc 用 container_of 反推 e. */
    wr.wr_cqe  = &e->cqe;
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
    init_completion(&rdma->hs_send_done);
    init_completion(&rdma->hs_recv_done);
    spin_lock_init(&rdma->recv_lock);
    spin_lock_init(&rdma->rx_done_lock);
    INIT_LIST_HEAD(&rdma->rx_done_list);
    atomic_set(&rdma->recv_posted, 0);
    atomic_set(&rdma->send_credits, 0);
    init_waitqueue_head(&rdma->send_waitq);
    rdma->connected = false;
    rdma->errored   = false;
    rdma->handshake_in_progress = false;
    rdma->hs_send_handled = false;
    rdma->hs_recv_handled = false;
    rdma->hs_resp_len = 0;
    /* [RC15 FIX] 握手 SEND WR 专用 cqe.done = 标准回调入口.
     * hs_send_cqe 内嵌在 rdma 结构中, WC 完成时 cq->cq_context = rdma 已
     * 足以让 powerfs_rdma_cqe_done 做指针比对判断 handshake. */
    rdma->hs_send_cqe.done = powerfs_rdma_cqe_done;

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

    /* 清掉 errored 标志 (重连); 初始化握手相关字段. */
    rdma->errored = false;
    rdma->handshake_in_progress = false;
    rdma->hs_resp_len = 0;
    init_completion(&rdma->hs_send_done);
    init_completion(&rdma->hs_recv_done);
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
    pr_info("powerfs_rdma: ib_alloc_pd OK local_dma_lkey=0x%x unsafe_global_rkey=0x%x dev=%s\n",
            rdma->pd->local_dma_lkey, rdma->pd->unsafe_global_rkey,
            rdma->cm_id->device ? rdma->cm_id->device->name : "null");

    /* [ROOT CAUSE 13 FIX v2] 4. CQ × 2. 使用 IB_POLL_WORKQUEUE 代替 IB_POLL_SOFTIRQ.
     *
     * RC13 v1 误用 IB_POLL_DIRECT: 其注释 "caller context, no hw completions"
     *   → DIRECT 模式下 HCA CQE 到了不会自动调 comp_handler, 需调用者
     *   显式 ib_process_cq_direct(cq). 未手动 poll → HS SEND/RECV WC
     *   永不回调 → wait_for_completion 5s timeout -ETIMEDOUT → handshake 全挂.
     *   现象: [HS-REQ-SEND] pr_info 有但无 WC success, filer 端收到 req 后
     *   发 resp 触发 RNR (客户端 RECV 完成 handler 没跑, 未补 repost_recv
     *   ? 不, 是 handshake SEND WC 没跑 → hs_send_done 没 complete → Step
     *   9g wait 5s 超时直接 errored clean 断 → filer 方 resp SEND 时客户端
     *   QP 已进入错误/断连 → RETRY_EXC_ERR status=12).
     *
     * IB_POLL_WORKQUEUE: HCA CQE → IRQ → 内核 cq_poll_wq worker 线程
     *   (进程上下文) 调用 comp_handler.
     *   - 不占 softirq, 不会阻塞 RCU grace period (原 SOFTIRQ stall 根因).
     *   - 可睡眠 (我们 handler 不睡眠).
     *   - 与 ksoftirqd 解耦: WR_FLUSH_ERR 风暴不阻塞 RCU. */
    pr_info("powerfs_rdma: alloc CQ (WORKQUEUE mode) send_cqe=%u recv_cqe=%u\n",
            PFS_RDMA_MAX_SEND_WR * 2, PFS_RDMA_MAX_RECV_WR * 2);
    rdma->send_cq = ib_alloc_cq(rdma->cm_id->device, rdma,
                                 PFS_RDMA_MAX_SEND_WR * 2, 0, IB_POLL_WORKQUEUE);
    if (IS_ERR(rdma->send_cq)) {
        ret = PTR_ERR(rdma->send_cq);
        rdma->send_cq = NULL;
        pr_err("powerfs_rdma: ib_alloc_cq(send, WORKQUEUE) failed: %d cqe_req=%u\n",
               ret, PFS_RDMA_MAX_SEND_WR * 2);
        goto err_pd;
    }
    /* [RC15 FIX] 不再覆盖 comp_handler. WORKQUEUE 模式下 cq->comp_handler
     * = cq.c 的 ib_cq_completion_workqueue (内部 cancel 安全). 完成路由:
     * HCA IRQ → cq_poll_wq worker → ib_process_cq_direct → wc->wr_cqe->done
     * = powerfs_rdma_cqe_done (我们在每个 WR 上挂 wr_cqe 入口). */

    rdma->recv_cq = ib_alloc_cq(rdma->cm_id->device, rdma,
                                 PFS_RDMA_MAX_RECV_WR * 2, 0, IB_POLL_WORKQUEUE);
    if (IS_ERR(rdma->recv_cq)) {
        ret = PTR_ERR(rdma->recv_cq);
        rdma->recv_cq = NULL;
        pr_err("powerfs_rdma: ib_alloc_cq(recv, WORKQUEUE) failed: %d cqe_req=%u\n",
               ret, PFS_RDMA_MAX_RECV_WR * 2);
        goto err_send_cq;
    }
    /* [RC15 FIX] 同上: 不覆盖 recv_cq->comp_handler, 使用 wr_cqe->done 标准契约. */

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

    /* [RC12/14 FIX + RC18b FIX G] 7. Pre-post RECV = PFS_RDMA_MAX_RECV_WR (32) 全量,
     * 不再预留 2 条. 原先 -2 仅 30 预挂, filer 端 18B handshake resp 来时
     * 若 1-2 条 RECV 因 post_recv 慢 1 拍导致 RNR → filer 端 RDMA send
     * completion status=12 RETRY_EXC_ERR (RNR 7 次后报告重试耗尽).
     * [RC18b ROOT21] ctrl_pool 大小必须 > MAX_RECV_WR (见 .h: 128 >> 32),
     * 这样 post 完 32 条挂 HCA RQ 后, pool 仍有 128-32=96 空闲条给 meta
     * RPC SEND 路径 acquire (SEND 也占同 ctrl_pool entry!). 老设计 32==32
     * 导致 pre-post 后 pool.free=0 → send_frame EAGAIN 永远. */
    pr_info("powerfs_rdma: pre-post %d RECV (ctrl_pool entries=%d) to qp_num=%u\n",
            PFS_RDMA_MAX_RECV_WR, PFS_RDMA_CTRL_BUF_NUM,
            rdma->qp ? rdma->qp->qp_num : 0);
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
    conn_param.responder_resources = 7;  /* 匹配 mlx5 VF max 深度 (原 1 太小) */
    conn_param.initiator_depth     = 7;
    conn_param.retry_count         = 3;  /* 降低重试避免快速 7 次重传 */
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
    pr_info("powerfs_rdma: cm_done wakeup event=%s (%d) status=%d errored=%d qp_num=%u\n",
            rdma_event_msg(rdma->cm_event), rdma->cm_event,
            rdma->cm_status, rdma->errored,
            rdma->qp ? rdma->qp->qp_num : 0);
    if (rdma->cm_event != RDMA_CM_EVENT_ESTABLISHED) {
        pr_err("powerfs_rdma: connect rejected/failed: %s (status=%d)\n",
               rdma_event_msg(rdma->cm_event), rdma->cm_status);
        ret = rdma->cm_status ? rdma->cm_status : -ECONNREFUSED;
        goto err_data_pool;
    }

    /* 9. ESTABLISHED → PFSN 握手 (20 字节 req → 18 字节 resp, 裸协议)
     *   必须在 connected=true 之前完成. 否则 Rust 服务端未登记 client_id,
     *   会丢弃后续所有 TLV 帧 → RPC deadline exceeded.
     *
     *   发送缓冲: 专用于握手的 1 页 + ib_dma_map_single + pd->local_dma_lkey.
     *     不使用 ctrl_pool: (a) 控制池 lkey 在 mlx5 RC SEND 方向遇到
     *     IB_WC_LOC_PROT_ERR (RECV 方向 OK, 经验现象), (b) 避免与 RECV
     *     争抢有限的 ctrl MR 条目.
     *   接收缓冲: 仍由 pre-posted ctrl_pool RECV WQEs 提供, CQ handler 通过
     *     handshake_in_progress=true 分支将第 1 个 RECV WC 路由到这里. */
    {
        struct powerfs_net_handshake_req  hs_req;
        struct powerfs_net_handshake_resp hs_resp;
        struct ib_send_wr   wr;
        struct ib_sge       sge;
        const struct ib_send_wr *bad_wr = NULL;
        struct page        *pg;
        void               *page_vaddr;
        u64                 client_id;

        /* 9a. 构造握手请求 (与 TCP 路径 powerfs_conn_do_handshake 一致) */
        memcpy(hs_req.magic, "PFSN", 4);
        hs_req.version     = POWERFS_NET_VERSION;
        hs_req.client_type = POWERFS_NET_CLIENT_KERNEL;
        hs_req.channel = (conn->type == POWERFS_NET_SERVER_VOLUME_META)
                         ? POWERFS_NET_CHANNEL_META : POWERFS_NET_CHANNEL_DATA;
        hs_req.reserved    = 0;
        client_id          = atomic_read(&conn->seq_counter) + 1000000;
        hs_req.client_id   = cpu_to_le64(client_id);
        hs_req.features    = 0;

        /* 9b. 写入 conn (与 TCP 路径一致, 供后续帧头 route_hash 计算用) */
        conn->client_id = client_id;
        conn->channel   = hs_req.channel;

        /* 9c. 分配 1 页临时发送缓冲, DMA map, 写 20 字节请求 */
        pg = alloc_page(GFP_KERNEL | __GFP_ZERO);
        if (!pg) {
            pr_err("powerfs_rdma: handshake page alloc failed\n");
            ret = -ENOMEM;
            goto err_data_pool;
        }
        page_vaddr = page_address(pg);
        rdma->hs_send_page = page_vaddr;
        rdma->hs_send_dma  = ib_dma_map_single(rdma->cm_id->device, page_vaddr,
                                               PAGE_SIZE, DMA_TO_DEVICE);
        if (ib_dma_mapping_error(rdma->cm_id->device, rdma->hs_send_dma)) {
            pr_err("powerfs_rdma: handshake dma_map failed\n");
            ret = -EIO;
            __free_page(pg);
            rdma->hs_send_page = NULL;
            goto err_data_pool;
        }
        memcpy(page_vaddr, &hs_req, sizeof(hs_req));

        pr_info("powerfs_rdma: handshake send page_vaddr=%px dma=%llx "
                "size=%u lkey=0x%x (pd->local_dma_lkey) qp_num=%u\n",
                page_vaddr, (unsigned long long)rdma->hs_send_dma,
                (unsigned)sizeof(hs_req),
                rdma->pd->local_dma_lkey, rdma->qp->qp_num);

        /* 9d. 进入握手阶段 (必须在 ib_post_send 之前, 因为 comp_handler
         *     可能在 ib_post_send 返回前已在另一个 CPU 执行).
         * [RC16b] 重置去重标志, 防止同 struct 重连时旧 true 值影响新握手. */
        rdma->handshake_in_progress = true;
        rdma->hs_send_handled = false;
        rdma->hs_recv_handled = false;

        /* 9e. Build SGE + WR. lkey = pd->local_dma_lkey, wr_id = HSWR magic. */
        memset(&sge, 0, sizeof(sge));
        sge.addr   = rdma->hs_send_dma;
        sge.length = sizeof(hs_req);
        sge.lkey   = rdma->pd->local_dma_lkey;

        memset(&wr, 0, sizeof(wr));
        /* [RC15a FIX union 重叠] wr_cqe/wr_id 是 union, 只写 wr_cqe =
         * &rdma->hs_send_cqe, 不写 wr_id (否则覆盖 wr_cqe 成 magic 0x48535752
         * → __ib_process_cq deref CR2=0x48535752 #PF Oops, qemu.log L123-L170).
         * process_wc 用 cqe == &rdma->hs_send_cqe 指针直接比对识别握手 SEND. */
        wr.wr_cqe     = &rdma->hs_send_cqe;
        wr.opcode     = IB_WR_SEND;
        wr.sg_list    = &sge;
        wr.num_sge    = 1;
        wr.send_flags = IB_SEND_SIGNALED;

        /* 9e-bis: 握手 SEND 也占用一个 send credit. 若不先 dec, CQ handler
         * 完成时会 inc 一次, credit 总数从 MAX 变成 MAX+1 (泄漏 1).
         * 连接生命周期内 reconnect 会重设 credit, 故影响很小, 但还是修. */
        if (atomic_dec_if_positive(&rdma->send_credits) < 0) {
            pr_err("powerfs_rdma: no send credit available for handshake (BUG)\n");
            rdma->handshake_in_progress = false;
            ib_dma_unmap_single(rdma->cm_id->device, rdma->hs_send_dma,
                                PAGE_SIZE, DMA_TO_DEVICE);
            __free_page(pg);
            rdma->hs_send_page = NULL;
            ret = -ENOMEM;
            goto err_data_pool;
        }

        /* [RC12 诊断] handshake req 即将 ib_post_send: 打印 magic/version/
         * client_type/channel/client_id + qp_num/send_credits/prepost_recv
         * 确认 handshake 包参数与服务端接受一致, 包已真正进入 QP. */
        pr_info("powerfs_rdma: [HS-REQ-SEND] magic=PFSN ver=0x%02x "
                "client_type=0x%02x channel=%u client_id=%llu (qp_num=%u "
                "pd_lkey=0x%x sge_dma=%llx len=%zu)\n",
                hs_req.version, hs_req.client_type, hs_req.channel,
                (unsigned long long)le64_to_cpu(hs_req.client_id),
                rdma->qp ? rdma->qp->qp_num : 0,
                rdma->pd->local_dma_lkey,
                (unsigned long long)rdma->hs_send_dma,
                sizeof(hs_req));

        ret = ib_post_send(rdma->qp, &wr, &bad_wr);
        if (ret) {
            pr_err("powerfs_rdma: handshake ib_post_send failed: %d\n", ret);
            rdma->handshake_in_progress = false;
            atomic_inc(&rdma->send_credits);  /* restore unused dec */
            ib_dma_unmap_single(rdma->cm_id->device, rdma->hs_send_dma,
                                PAGE_SIZE, DMA_TO_DEVICE);
            __free_page(pg);
            rdma->hs_send_page = NULL;
            goto err_data_pool;
        }
        /* 注意: 1 个 SEND in-flight, send_credits 已在 pool_init 后置
         *       PFS_RDMA_MAX_SEND_WR (64), 足够. 完成时 CQ handler 增 credit. */

        /* 9f. 等待握手请求 send 完成 (5s 超时). */
        ret = wait_for_completion_interruptible_timeout(
                  &rdma->hs_send_done, msecs_to_jiffies(5000));
        if (ret <= 0) {
            pr_err("powerfs_rdma: handshake req send timeout/intr (%s:%u ret=%d)\n",
                   conn->addr, conn->port, ret);
            ret = (ret == 0) ? -ETIMEDOUT : ret;
            rdma->handshake_in_progress = false;
            ib_dma_unmap_single(rdma->cm_id->device, rdma->hs_send_dma,
                                PAGE_SIZE, DMA_TO_DEVICE);
            __free_page(pg);
            rdma->hs_send_page = NULL;
            goto err_data_pool;
        }
        if (rdma->errored) {
            pr_err("powerfs_rdma: handshake req send WC error (LOC_PROT?)\n");
            ret = -EIO;
            rdma->handshake_in_progress = false;
            ib_dma_unmap_single(rdma->cm_id->device, rdma->hs_send_dma,
                                PAGE_SIZE, DMA_TO_DEVICE);
            __free_page(pg);
            rdma->hs_send_page = NULL;
            goto err_data_pool;
        }
        /* SEND 完成立即释放 DMA/page: SEND WC 已确认 HCA 已读取缓冲. */
        ib_dma_unmap_single(rdma->cm_id->device, rdma->hs_send_dma,
                            PAGE_SIZE, DMA_TO_DEVICE);
        __free_page(pg);
        rdma->hs_send_page = NULL;
        pr_info("powerfs_rdma: [HS-REQ-SEND] WC success -> waiting resp (qp_num=%u)\n",
                rdma->qp ? rdma->qp->qp_num : 0);

        /* 9g. 等待握手响应 RECV 完成 (5s 超时).
         *     响应来自 pre-posted RECV 之一, 无需额外 post_recv. */
        ret = wait_for_completion_interruptible_timeout(
                  &rdma->hs_recv_done, msecs_to_jiffies(5000));
        if (ret <= 0) {
            pr_err("powerfs_rdma: [HS-RESP-RECV] timeout/intr %d "
                   "(qp_num=%u errored=%d credits_send=%d prepost_ctrl=%d/%d)\n",
                   ret, rdma->qp ? rdma->qp->qp_num : 0, rdma->errored,
                   atomic_read(&rdma->send_credits),
                   rdma->ctrl_pool.free, PFS_RDMA_CTRL_BUF_NUM);
            ret = (ret == 0) ? -ETIMEDOUT : ret;
            rdma->handshake_in_progress = false;
            goto err_data_pool;
        }
        if (rdma->errored) {
            pr_err("powerfs_rdma: [HS-RESP-RECV] WC errored, expected success "
                   "(qp_num=%u). 对应 filer 日志若为 status=12 RETRY_EXC_ERR: "
                   "客户端 RNR (接收 buffer 全空) 或本地 QP 错.\n",
                   rdma->qp ? rdma->qp->qp_num : 0);
            ret = -EIO;
            rdma->handshake_in_progress = false;
            goto err_data_pool;
        }
        if (rdma->hs_resp_len < sizeof(hs_resp)) {
            pr_err("powerfs_rdma: handshake resp too short: %zu < %zu\n",
                   rdma->hs_resp_len, sizeof(hs_resp));
            rdma->handshake_in_progress = false;
            ret = -EINVAL;
            goto err_data_pool;
        }
        memcpy(&hs_resp, rdma->hs_resp, sizeof(hs_resp));

        /* 9h. 校验 magic 与 status, 写入 conn->server_id / server_features. */
        if (memcmp(hs_resp.magic, "PFSN", 4) != 0) {
            pr_err("powerfs_rdma: handshake resp bad magic\n");
            rdma->handshake_in_progress = false;
            ret = -EINVAL;
            goto err_data_pool;
        }
        if (hs_resp.status != 0) {
            pr_err("powerfs_rdma: handshake rejected status=%u\n", hs_resp.status);
            rdma->handshake_in_progress = false;
            ret = -EPERM;
            goto err_data_pool;
        }
        conn->server_id       = le64_to_cpu(hs_resp.server_id);
        conn->server_features = le32_to_cpu(hs_resp.features);

        /* 9i. 握手成功, 退出握手阶段. 后续 WC 进入通用收发路径. */
        rdma->handshake_in_progress = false;
        pr_info("powerfs_rdma: filer handshake OK server_id=%llu qp_num=%u\n",
                (unsigned long long)conn->server_id, rdma->qp->qp_num);
    }

    /* 10. RDMA 建链 + PFSN 握手均完成. */
    rdma->connected = true;
    pr_info("powerfs_rdma: connected to %s:%u (qp_num=%u)\n",
            conn->addr, conn->port, rdma->qp->qp_num);
    return 0;

err_data_pool:
    /* [ROOT CAUSE 11 FIX] connect 失败清理顺序必须与正常 disconnect 对齐:
     *   1. errored 置位 (阻断 comp_handler 对 pools 的进一步写入)
     *   2. destroy qp → flush 所有在飞 SEND/RECV WR → 生成 WC
     *   3. drain 两个 CQ (poll 所有 flush WC, 让 pool entries 被释放回 free_list)
     *   4. free cq (此时无在飞回调, 不会触发 ib_free_cq WARNING)
     *   5. pool_free (此时所有 entry 都已归还, 不会有 comp_handler 继续
     *      访问已 free 内存导致 UAF/entry->buf=NULL Oops)
     *   6. dealloc_pd + destroy_cm_id
     * 旧顺序 (pool_free → destroy_qp → free_cq) 的崩溃链路:
     *   err_data_pool → pool_free 先 kfree(entries) + e->buf=NULL
     *   → 然后 destroy_qp flush 30 RECV WR → WR_FLUSH_ERR WC
     *   → softirq comp_handler 被调度 poll CQ → process_wc L353 调用
     *     pool_release 写已被 kfree 的 entries 内存 (UAF)
     *   → 同时 pfs_rx_callback(conn) 置 rx_ready=1
     *   → pfs_rx/3 kthread 调 recv_frame → list_empty 判断错 (UAF 写脏)
     *     → 取到野 entry (buf=NULL) → hdr_decode+0x12 读 NULL → panic #PF
     * 参考 qemu.log L104-156: RDI=0x0 hdr_decode NULL deref,
     *   call trace pfs_rx_thread_fn → pfs_process_receive → rdma_recv_frame
     *   → powerfs_rdma_recv_frame. */
    rdma->handshake_in_progress = false;
    rdma->connected = false;
    rdma->errored   = true;
    complete_all(&rdma->cm_done);
    complete_all(&rdma->hs_send_done);
    complete_all(&rdma->hs_recv_done);
    wake_up(&rdma->send_waitq);

    /* 2. destroy qp (如果存在) 引发所有在飞 WR flush */
    if (rdma->cm_id && rdma->qp) {
        rdma_destroy_qp(rdma->cm_id);
        rdma->qp = NULL;
    }

    /* 3. drain 两个 CQ (最多 200*1ms = 200ms) */
    {
        struct ib_wc wcs[PFS_RDMA_WC_BATCH];
        int i, n;
        for (i = 0; i < 200; i++) {
            int got = 0;
            while (rdma->recv_cq && (n = ib_poll_cq(rdma->recv_cq, PFS_RDMA_WC_BATCH, wcs)) > 0) {
                int j;
                for (j = 0; j < n; j++)
                    powerfs_rdma_process_wc(rdma, &wcs[j]);
                got = 1;
            }
            while (rdma->send_cq && (n = ib_poll_cq(rdma->send_cq, PFS_RDMA_WC_BATCH, wcs)) > 0) {
                int j;
                for (j = 0; j < n; j++)
                    powerfs_rdma_process_wc(rdma, &wcs[j]);
                got = 1;
            }
            if (!got) break;
            usleep_range(1000, 2000);
        }
    }

    /* 4. free CQs (drain 后无未消费 CQE, 不会 WARNING cq.c:322).
     * [RC13 加强] ib_poll_cq drain 结束后, 再 ib_process_cq_direct 400 次
     *   (每次 budget=-1 处理所有就绪 CQE) 配合 1ms sleep: 确保 worker
     *   线程仍有 on-stack 的 CQE 被彻底清掉, 否则 ib_free_cq 发现
     *   cq->wcq != NULL 触发 WARNING (cq.c:322 "still have work on CQ").
     * 每次 drain 循环至少 1 次 ib_process_cq_direct, 确保即使 poll
     *   返回 0 (worker 延迟) 时也强制推. */
    {
        int i;
        for (i = 0; i < 400; i++) {
            int any = 0;
            if (rdma->recv_cq) {
                int n = ib_process_cq_direct(rdma->recv_cq, -1);
                if (n > 0) any = 1;
            }
            if (rdma->send_cq) {
                int n = ib_process_cq_direct(rdma->send_cq, -1);
                if (n > 0) any = 1;
            }
            if (!any) break;
            usleep_range(1000, 2000);
        }
    }
    if (rdma->recv_cq) {
        ib_free_cq(rdma->recv_cq);
        rdma->recv_cq = NULL;
    }
    if (rdma->send_cq) {
        ib_free_cq(rdma->send_cq);
        rdma->send_cq = NULL;
    }

    /* 5. pool_free (此时 comp_handler 不可能再访问 entries) */
    powerfs_rdma_mr_pool_free(&rdma->data_pool);
    powerfs_rdma_mr_pool_free(&rdma->ctrl_pool);

    /* 6. pd + cm_id 释放 */
    if (rdma->pd) {
        ib_dealloc_pd(rdma->pd);
        rdma->pd = NULL;
    }
    if (rdma->cm_id) {
        rdma_destroy_id(rdma->cm_id);
        rdma->cm_id = NULL;
    }
    rdma->errored = true;
    return ret;

    /* 早期阶段 (pool/qp 未初始化) 的失败标签: 统一跳转到新清理.
     * 清理内部所有对象均带 NULL 指针检查 (无 qp → skip destroy,
     * 无 CQ → skip free, 无 pool entries → pool_free 立即返回).
     * 因此无需区分 early vs late cleanup. */
err_ctrl_pool: goto err_data_pool;
err_qp:        goto err_data_pool;
err_recv_cq:   goto err_data_pool;
err_send_cq:   goto err_data_pool;
err_pd:        goto err_data_pool;
err_cm_id:     goto err_data_pool;
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

    /* 2.5 [RC13 加强] destroy qp/cm_id 后, 二次 drain 所有 CQE:
     *   先 400× ib_process_cq_direct (每个 budget=-1), 再 ib_poll_cq 兜底.
     *   ib_free_cq(cq.c:322) WARNING 的根因是: WORKQUEUE poll 模式下,
     *   cq_poll_wq worker 仍持有 pending CQE 没跑完, destroy_id 之后才回
     *   写 CQ → ib_free_cq 时 wcqe_head != NULL → WARNING. 强制 direct
     *   drain 同步等待所有 CQE 被 process 就不会触发. */
    for (i = 0; i < 400; i++) {
        int any = 0;
        if (rdma->recv_cq) {
            int k = ib_process_cq_direct(rdma->recv_cq, -1);
            if (k > 0) any = 1;
        }
        if (rdma->send_cq) {
            int k = ib_process_cq_direct(rdma->send_cq, -1);
            if (k > 0) any = 1;
        }
        while (rdma->recv_cq && (n = ib_poll_cq(rdma->recv_cq, PFS_RDMA_WC_BATCH, wcs)) > 0) {
            int j;
            for (j = 0; j < n; j++)
                powerfs_rdma_process_wc(rdma, &wcs[j]);
            any = 1;
        }
        while (rdma->send_cq && (n = ib_poll_cq(rdma->send_cq, PFS_RDMA_WC_BATCH, wcs)) > 0) {
            int j;
            for (j = 0; j < n; j++)
                powerfs_rdma_process_wc(rdma, &wcs[j]);
            any = 1;
        }
        if (!any) break;
        usleep_range(1000, 2000);
    }

    /* 3. 释放 CQ. ib_free_cq 内部 cancel_work_sync 同步 workqueue. */
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
    if (atomic_dec_if_positive(&rdma->send_credits) < 0) {
        PFS_RDMA_DIAG("POST_SEND_EAGAIN no_credit msg_type=%u seq=%u total=%zu recv_posted=%d pool.free=%d/%d\n",
                      hdr ? hdr->msg_type : 0xFFFF,
                      hdr ? hdr->seq : 0xFFFFFFFFu,
                      total,
                      atomic_read(&rdma->recv_posted),
                      rdma->ctrl_pool.free, PFS_RDMA_CTRL_BUF_NUM);
        return -EAGAIN;
    }

    e = powerfs_rdma_mr_pool_acquire(pool, GFP_ATOMIC);
    if (!e) {
        PFS_RDMA_DIAG("POST_SEND_EAGAIN pool_empty msg_type=%u seq=%u total=%zu recv_posted=%d pool.free=%d/%d\n",
                      hdr ? hdr->msg_type : 0xFFFF,
                      hdr ? hdr->seq : 0xFFFFFFFFu,
                      total,
                      atomic_read(&rdma->recv_posted),
                      pool->free, PFS_RDMA_CTRL_BUF_NUM);
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

    /* 单 SGE 覆盖实际使用的字节 (lkey = pd->local_dma_lkey PA-mode 共用,
     * 对整个 buf 空间有效, length 可小于) */
    sge.addr   = e->dma;
    sge.length = off;
    sge.lkey   = rdma->pd->local_dma_lkey;

    memset(&wr, 0, sizeof(wr));
    /* [RC15a FIX union 重叠] SEND WR wr_cqe/wr_id 是 union → 只写 wr_cqe. */
    wr.wr_cqe    = &e->cqe;
    wr.next      = NULL;
    wr.sg_list   = &sge;
    wr.num_sge   = 1;
    wr.opcode    = IB_WR_SEND;
    wr.send_flags = IB_SEND_SIGNALED;

    ret = ib_post_send(rdma->qp, &wr, &bad);
    if (ret) {
        pr_warn_ratelimited("powerfs_rdma: ib_post_send failed: %d\n", ret);
        PFS_RDMA_DIAG("POST_SEND_FAIL ret=%d total=%zu qp_num=%u msg_type=%u\n",
                      ret, off,
                      rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu,
                      hdr ? hdr->msg_type : 0xFFFFFFFFu);
        powerfs_rdma_mr_pool_release(pool, e);
        atomic_inc(&rdma->send_credits);
        rdma->errored = true;
        return ret;
    }
    PFS_RDMA_DIAG("POST_SEND_OK total=%zu qp_num=%u msg_type=%u seq=%u\n",
                  off,
                  rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu,
                  hdr ? hdr->msg_type : 0xFFFFFFFFu,
                  hdr ? hdr->seq      : 0xFFFFFFFFu);

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

    /* [防御性] errored=1 说明 QP 已错/destroy 中, 不应再读 rx_done_list.
     * has_rx_data 可能返回 false, 但我们在此显式拦截, 避免 clean-up 阶段
     * pool entries 被释放后 rx_done_list 被 UAF 写坏导致取到野指针. */
    if (rdma->errored)
        return -ENOTCONN;

    if (!powerfs_rdma_has_rx_data(conn)) {
        PFS_RDMA_DIAG("RECV_EAGAIN(has_rx) qp_num=%u\n",
                      rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu);
        return -EAGAIN;
    }

    /* 1. 摘一个已完成 RECV */
    spin_lock_irqsave(&rdma->rx_done_lock, flags);
    if (list_empty(&rdma->rx_done_list)) {
        PFS_RDMA_DIAG("RECV_EAGAIN(list) qp_num=%u\n",
                      rdma->qp ? rdma->qp->qp_num : 0xFFFFFFFFu);
        spin_unlock_irqrestore(&rdma->rx_done_lock, flags);
        return -EAGAIN;
    }
    e = list_first_entry(&rdma->rx_done_list,
                         struct powerfs_rdma_mr_entry, list);
    list_del_init(&e->list);
    spin_unlock_irqrestore(&rdma->rx_done_lock, flags);
    PFS_RDMA_DIAG("RECV_PICK_ENTRY e=%px size=%zu entry_id=%d\n",
                  e, e ? e->size : 0, e ? e->entry_id : -1);

    /* 防御: entry->buf 必须有效 (非 NULL, 非 poison). 若 entry 来自已释放
     * pool, 会在 hdr_decode 内 Oops. errored 检查之上再加一道. */
    if (unlikely(!e || !e->buf || !e->dma || e->size == 0)) {
        pr_warn_ratelimited("powerfs_rdma: recv_frame invalid entry e=%px buf=%px dma=%llx size=%zu\n",
                            e, e ? e->buf : NULL,
                            e ? (unsigned long long)e->dma : 0ULL,
                            e ? e->size : 0);
        return -EPROTO;
    }

    /* 2. 解码帧头 + CRC 校验 */
    if (!powerfs_net_frame_hdr_decode(e->buf, POWERFS_NET_FRAME_HDR_SIZE, &hdr)) {
        pr_warn_ratelimited("powerfs_rdma: invalid frame header (CRC/magic)\n");
        ret = -EPROTO;
        goto repost;
    }
    PFS_RDMA_DIAG("RECV_OK_DECODE msg_type=%u seq=%u body_len=%u data_total=%u\n",
                  hdr.msg_type, hdr.seq, hdr.body_len, hdr.data_len);

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
        sge.lkey   = rdma->pd->local_dma_lkey;
        /* [RC15a FIX union 重叠] repost RECV: 只写 wr_cqe, 不写 wr_id. */
        wr.wr_cqe  = &e->cqe;
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
    int ret;
    /* req unused: RDMA 单 WR 即发完整帧, 不需要 partial-send 续传 */
    (void)req;
    PFS_RDMA_DIAG("SEND_ENTER msg_type=%u seq=%u body_len=%zu data_len=%zu\n",
                  hdr ? hdr->msg_type : 0xFFFFFFFFu,
                  hdr ? hdr->seq      : 0xFFFFFFFFu,
                  body_len, data_len);
    ret = powerfs_rdma_send_frame(conn, hdr, body, body_len, data, data_len);
    PFS_RDMA_DIAG("SEND_RET ret=%d msg_type=%u seq=%u\n",
                  ret,
                  hdr ? hdr->msg_type : 0xFFFFFFFFu,
                  hdr ? hdr->seq      : 0xFFFFFFFFu);
    return ret;
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
