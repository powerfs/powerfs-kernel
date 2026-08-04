# PowerFS 内核文件系统 Lease 强一致方案（细化版）

## 一、总体设计原则

1. **借鉴 Ceph**：netfs 库、iget5_locked、kmem_cache、rbtree
2. **借鉴 FUSE**：Transport trait 模块化、TLV 协议
3. **崩溃防护**：GFP_NOFS、超时、shutdown flag、goto 清理、WARN_ON_ONCE 替代 BUG_ON
4. **Shrink 机制**：SLAB_RECLAIM_ACCOUNT 让内核自动回收 slab，page cache 由 LRU 管理
5. **渐进式实现**：每步可编译可测试，从最简单流程开始

## 二、Inode 详细设计

### 2.1 核心结构

```c
/* powerfs.h */

/* 常量 */
#define POWERFS_SUPER_MAGIC     0x50574552
#define POWERFS_ROOT_INO        1
#define POWERFS_CHUNK_SIZE      (2 * 1024 * 1024)    /* 2MB */
#define POWERFS_STRIPE_SIZE     (64 * 1024 * 1024)   /* 64MB */
#define POWERFS_DENTRY_LEASE_TTL  (5 * HZ)
#define POWERFS_INODE_CACHE_TTL   (10 * HZ)
#define POWERFS_LEASE_DURATION    (30 * HZ)
#define POWERFS_LEASE_RENEW_THRESHOLD (POWERFS_LEASE_DURATION / 3)

/* Chunk 映射条目 */
struct powerfs_chunk_map {
    u32 chunk_idx;
    u64 needle_id;
    u64 volume_id;
    u32 crc32;
};

/* Per-stripe Lease（参考 ceph_cap，rbtree 节点） */
struct powerfs_lease {
    struct rb_node node;              /* rbtree 节点，键为 stripe_start */
    u64 stripe_start;
    u64 stripe_count;                 /* 固定 64MB */
    char token[64];
    u64 epoch;
    unsigned long expire_jiffies;
    bool exclusive;                   /* true=写独占, false=读共享 */
    u64 content_size;                 /* lease 响应携带 */
};

/* Inode 私有数据（参考 ceph_inode_info，netfs_inode 第一个字段） */
struct powerfs_inode_info {
    struct netfs_inode netfs;         /* 内含 struct inode，必须第一个 */

    u64 parent_ino;
    char name[255];

    /* Lease：rbtree 按 stripe_start 排序 */
    struct rb_root lease_tree;
    spinlock_t lease_lock;            /* spinlock 保护 lease_tree */
    struct delayed_work lease_renew_work;

    /* Chunk 映射（open/getattr 时从 Filer 获取，lease 响应更新） */
    struct powerfs_chunk_map *chunks;
    u32 chunk_count;
    u64 content_size;
    u64 volume_id;                    /* 文件主 volume */

    /* 缓存有效性 */
    bool cache_valid;
    unsigned long cache_expire;

    /* 目录缓存（本地 readdir） */
    bool dir_complete;
    struct list_head dir_entries;     /* powerfs_dir_entry 链表 */
    struct mutex dir_mutex;           /* readdir 可睡眠，用 mutex */

    /* shutdown 标志（参考 ceph_inode_is_shutdown） */
    bool shutdown;
};
```

### 2.2 Dentry 私有数据

```c
struct powerfs_dentry_info {
    struct dentry *dentry;
    struct list_head lease_list;      /* 全局 dentry lease 链表节点 */
    unsigned long lease_expire;       /* jiffies */
    unsigned long time;               /* 最近验证时间 */
    u64 offset;                       /* readdir 偏移 */
};

struct powerfs_dir_entry {
    struct list_head list;
    u64 ino;
    unsigned int type;                /* DT_REG, DT_DIR... */
    char name[255];
};
```

### 2.3 Super Block 私有数据

```c
struct powerfs_sb_info {
    struct super_block *sb;

    /* 网络连接（模块化设计，参考 FUSE Transport） */
    struct powerfs_net *filer_net;    /* Filer 连接 */
    struct powerfs_net *volume_net;   /* Volume 连接 */

    /* slab caches */
    struct kmem_cache *inode_cachep;
    struct kmem_cache *dentry_cachep;
    struct kmem_cache *lease_cachep;

    /* 客户端标识 */
    char client_id[64];               /* holder_id，与 FUSE 一致 */

    /* 全局 dentry lease 链表 */
    struct list_head dentry_lease_list;
    spinlock_t dentry_lease_lock;

    /* mount 互斥 */
    struct mutex mount_mutex;

    /* shutdown 标志 */
    bool shutting_down;
};
```

## 三、网络层模块化设计（借鉴 FUSE Transport trait + 多队列多线程）

### 3.1 设计思路

FUSE 用 Rust trait `Transport: send_request(msg_type, body) -> Result<Vec<u8>>`。内核 C 用函数指针结构体实现等价抽象：

### 3.2 多队列多线程设计（借鉴 FUSE data/lease/mgmt 三队列）

FUSE 客户端有三条独立队列 + 独立 Notify 唤醒：

| FUSE 队列 | max_concurrent | 超时 | 用途 |
|-----------|---------------|------|------|
| data_queue | 32 | 10s | Read/Write 数据请求 |
| lease_queue | 4 | 3s | Lease acquire/renew/release |
| mgmt_queue | 4 | 5s | 管理请求 |

内核用 **workqueue** 实现等价设计（每个 workqueue 独立线程池 + 独立唤醒 + max_active 并发控制）：

```c
/* 请求类型（对应 FUSE RequestKind） */
enum powerfs_req_kind {
    POWERFS_REQ_META,     /* lookup, getattr, create, mkdir → Filer */
    POWERFS_REQ_LEASE,    /* acquire, renew, release → Volume */
    POWERFS_REQ_DATA,     /* read_blob, write_blob → Volume */
    POWERFS_REQ_NOTIFY,   /* async invalidate ← Filer push */
};

/* 请求结构 */
struct powerfs_request {
    struct work_struct work;          /* workqueue work item */
    enum powerfs_req_kind kind;
    u16 msg_type;

    /* 请求/响应数据 */
    void *req_body;
    size_t req_len;
    void *resp_buf;
    size_t resp_max;
    size_t resp_len;
    int status;

    /* 同步等待（sync=true 时 VFS 回调等待完成） */
    struct completion done;
    bool async;                       /* true = fire-and-forget */

    /* NOTIFY 回调 */
    void (*notify_cb)(struct powerfs_request *req);
};

/* 队列定义（对应 FUSE 的 data/lease/mgmt 三个队列） */
struct powerfs_req_queue {
    struct workqueue_struct *wq;
    int max_active;                   /* 对应 FUSE max_concurrent */
    atomic_t active_count;            /* 当前活跃数（监控用） */
    char name[32];
};

/* SB info 中的多队列 */
struct powerfs_sb_info {
    /* ... 其他字段 ... */
    struct powerfs_req_queue meta_queue;    /* max_active=8, 对应 FUSE mgmt */
    struct powerfs_req_queue lease_queue;   /* max_active=4, 对应 FUSE lease */
    struct powerfs_req_queue data_queue;    /* max_active=32, 对应 FUSE data */
    struct powerfs_req_queue notify_queue;  /* max_active=4, 异步通知 */
};
```

#### 3.2.1 队列初始化（参考 Ceph 双 workqueue + GFS2 glock HIGHPRI + NFS WQ_MEM_RECLAIM）

**源码研究发现**（关键修正：之前"无 FS 用 WQ_HIGHPRI"的结论是错的）：

| 文件系统 | workqueue | 标志 | max_active | 用途 | 源码位置 |
|---------|-----------|------|-----------|------|---------|
| Ceph | `ceph-inode` | `WQ_UNBOUND` | 0 | inode 异步元数据工作 | `fs/ceph/super.c:814` |
| Ceph | `ceph-cap` | `0` | **1** | cap release/reclaim（**串行**） | `fs/ceph/super.c:817` |
| NFS | `nfsiod` | `WQ_MEM_RECLAIM \| WQ_UNBOUND` | 0 | 通用 I/O | `fs/nfs/inode.c:2313` |
| **GFS2** | `glock_workqueue` | **`WQ_HIGHPRI \| WQ_MEM_RECLAIM \| WQ_FREEZABLE`** | 0 | **glock 锁管理（延迟敏感）** | `fs/gfs2/glock.c:2462` |
| GFS2 | `delete_workqueue` | `WQ_MEM_RECLAIM \| WQ_FREEZABLE` | 0 | 删除工作（非高优先） | `fs/gfs2/glock.c:2468` |
| DLM | `dlm_io` | `WQ_HIGHPRI \| WQ_MEM_RECLAIM` | — | DLM I/O（锁通信） | `fs/dlm/lowcomms.c:1674` |
| XFS | `xfs-log` | `WQ_HIGHPRI` | — | 日志写入（延迟敏感） | `fs/xfs/xfs_log.c:1682` |
| F2FS | data wq | `WQ_UNBOUND \| WQ_HIGHPRI` | — | 数据路径 | `fs/f2fs/data.c:4102` |
| EROFS | `erofs_unzip` | `WQ_UNBOUND \| WQ_HIGHPRI` | — | 解压（CPU 敏感） | `fs/erofs/zdata.c:204` |

**关键结论**：
1. **WQ_HIGHPRI 用于延迟敏感场景**：锁管理（GFS2 glock、DLM）、日志写入（XFS log）、CPU 敏理解压（EROFS）。**PowerFS lease 续约若延迟超过 expiry 会丢锁导致写失败**，与 GFS2 glock 同理。
2. **Ceph cap_wq max_active=1（串行）**：cap release/reclaim 需要顺序处理避免竞态。
3. **Ceph inode_wq WQ_UNBOUND max_active=0**：inode 异步工作可并发，不绑 CPU。
4. **GFS2 glock 用 `queue_delayed_work`**：锁工作可延迟调度（`fs/gfs2/glock.c:253`），PowerFS lease 续约也用 delayed_work。

**Ceph 队列用途验证**（实际 queue_work 调用点）：
- `fsc->inode_wq`：`fs/ceph/inode.c:1848` `queue_work(fsc->inode_wq, &ci->i_work)` — inode 元数据异步处理
- `fsc->cap_wq`：`fs/ceph/mds_client.c:2209` `queue_work(...cap_wq, &session->s_cap_release_work)` — cap 释放
- `fsc->cap_wq`：`fs/ceph/mds_client.c:2245` `queue_work(...cap_wq, &mdsc->cap_reclaim_work)` — cap 回收
- 注意：`ci->i_cap_wq`（`fs/ceph/super.h:405`）是 `wait_queue_head_t`（等待队列），**不是** workqueue，用于 `wait_event_interruptible` 阻塞等待 cap flush 完成。

PowerFS 队列设计（基于上述源码研究）：

```c
int powerfs_init_queues(struct powerfs_sb_info *sbi)
{
    /* meta_queue: 元数据请求（lookup/getattr/create），可并发，不绑 CPU
     * 参考: ceph-inode (WQ_UNBOUND, max_active=0) + nfsiod (WQ_MEM_RECLAIM) */
    sbi->meta_queue.wq = alloc_workqueue("powerfs_meta",
        WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!sbi->meta_queue.wq)
        goto out_meta;

    /* lease_queue: lease acquire/renew/release，延迟敏感（过期即丢锁）
     * 参考: GFS2 glock_workqueue (WQ_HIGHPRI | WQ_MEM_RECLAIM | WQ_FREEZABLE)
     * 续约用 delayed_work（参考 GFS2 __gfs2_glock_queue_work 用 queue_delayed_work）
     * max_active=0: acquire 可并发（不同 stripe 互不影响），续约靠 delayed_work 串行 */
    sbi->lease_queue.wq = alloc_workqueue("powerfs_lease",
        WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM, 0);
    if (!sbi->lease_queue.wq)
        goto out_lease;

    /* data_queue: read/write blob，I/O 密集，可高并发
     * 参考: nfsiod (WQ_MEM_RECLAIM | WQ_UNBOUND), ceph-inode (max_active=0)
     * 不加 WQ_HIGHPRI: 数据 I/O 不如 lease 续约紧急，避免饿死元数据 */
    sbi->data_queue.wq = alloc_workqueue("powerfs_data",
        WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!sbi->data_queue.wq)
        goto out_data;

    /* notify_queue: Filer push invalidate，异步，可并发
     * 参考: ceph-inode (WQ_UNBOUND), 不加 HIGHPRI（invalidate 非紧急） */
    sbi->notify_queue.wq = alloc_workqueue("powerfs_notify",
        WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
    if (!sbi->notify_queue.wq)
        goto out_notify;

    return 0;

out_notify:
    destroy_workqueue(sbi->data_queue.wq);
out_data:
    destroy_workqueue(sbi->lease_queue.wq);
out_lease:
    destroy_workqueue(sbi->meta_queue.wq);
out_meta:
    return -ENOMEM;
}

void powerfs_destroy_queues(struct powerfs_sb_info *sbi)
{
    /* 参考 ceph flush_fs_workqueues (fs/ceph/super.c:843): flush + destroy
     * flush 保证所有 pending work 完成，避免卸载后回调访问已释放内存 */
    if (sbi->notify_queue.wq) {
        flush_workqueue(sbi->notify_queue.wq);
        destroy_workqueue(sbi->notify_queue.wq);
    }
    if (sbi->data_queue.wq) {
        flush_workqueue(sbi->data_queue.wq);
        destroy_workqueue(sbi->data_queue.wq);
    }
    if (sbi->lease_queue.wq) {
        flush_workqueue(sbi->lease_queue.wq);
        destroy_workqueue(sbi->lease_queue.wq);
    }
    if (sbi->meta_queue.wq) {
        flush_workqueue(sbi->meta_queue.wq);
        destroy_workqueue(sbi->meta_queue.wq);
    }
}
```

**设计决策**（基于源码研究，已修正）：
- `lease_queue` 加 `WQ_HIGHPRI`：lease 续约延迟敏感（过期丢锁），参考 GFS2 `glock_workqueue`（`fs/gfs2/glock.c:2462`）。**这是对之前"不用 WQ_HIGHPRI"结论的修正**。
- 其他队列不加 `WQ_HIGHPRI`：meta/data/notify 非延迟敏感，避免高优先队列饿死其他工作。
- `max_active=0`：使用内核默认值（参考 Ceph inode_wq、NFS nfsiod），不自限制并发。
- 不用 `max_active=1` 串行：Ceph cap_wq 串行是因为 cap release/reclaim 有全局顺序依赖；PowerFS lease 按 stripe 独立，无全局顺序，可并发（参考 GFS2 glock max_active=0）。
- `WQ_MEM_RECLAIM`：所有队列都加（参考 NFS nfsiod、GFS2），writeback 路径内存回收安全。
- 不加 `WQ_FREEZABLE`：PowerFS 不需要支持系统挂起（参考 Ceph 也不加），避免挂起时 I/O 卡死。
- 续约用 `delayed_work` + `queue_delayed_work`：参考 GFS2 `__gfs2_glock_queue_work`（`fs/gfs2/glock.c:253`），定时触发续约而非忙等。
- flush + destroy：卸载时保证所有 pending work 完成（参考 Ceph `flush_fs_workqueues`）。

#### 3.2.2 请求提交与处理

```c
/* 提交请求到对应队列（对应 FUSE 的 notify_one 唤醒 processor loop） */
int powerfs_submit_request(struct powerfs_sb_info *sbi,
                           struct powerfs_request *req)
{
    struct powerfs_req_queue *q;

    switch (req->kind) {
    case POWERFS_REQ_META:   q = &sbi->meta_queue; break;
    case POWERFS_REQ_LEASE:  q = &sbi->lease_queue; break;
    case POWERFS_REQ_DATA:   q = &sbi->data_queue; break;
    case POWERFS_REQ_NOTIFY: q = &sbi->notify_queue; break;
    default: return -EINVAL;
    }

    INIT_WORK(&req->work, powerfs_process_request);
    atomic_inc(&q->active_count);
    queue_work(q->wq, &req->work);  /* 独立唤醒：只唤醒该队列的 worker 线程 */

    /* 同步请求：等待完成 */
    if (!req->async) {
        wait_for_completion_timeout(&req->done,
            msecs_to_jiffies(POWERFS_NET_TIMEOUT_MS));
        return req->status;
    }
    return 0;  /* 异步请求立即返回 */
}

/* workqueue worker 回调（对应 FUSE 的 process_data_requests 等） */
static void powerfs_process_request(struct work_struct *work)
{
    struct powerfs_request *req = container_of(work, struct powerfs_request, work);
    struct powerfs_sb_info *sbi = powerfs_get_sbi_from_req(req);
    struct powerfs_req_queue *q;

    /* 根据 kind 选择队列（用于 active_count 递减） */
    switch (req->kind) {
    case POWERFS_REQ_META:   q = &sbi->meta_queue; break;
    case POWERFS_REQ_LEASE:  q = &sbi->lease_queue; break;
    case POWERFS_REQ_DATA:   q = &sbi->data_queue; break;
    case POWERFS_REQ_NOTIFY: q = &sbi->notify_queue; break;
    }

    /* 处理请求（workqueue 自动管理并发，max_active 生效） */
    switch (req->kind) {
    case POWERFS_REQ_META:
        req->status = powerfs_send_to_filer(sbi, req);
        break;
    case POWERFS_REQ_LEASE:
        req->status = powerfs_send_to_volume(sbi, req);
        break;
    case POWERFS_REQ_DATA:
        req->status = powerfs_send_to_volume(sbi, req);
        break;
    case POWERFS_REQ_NOTIFY:
        if (req->notify_cb)
            req->notify_cb(req);  /* 回调处理 invalidate */
        kfree(req);
        atomic_dec(&q->active_count);
        return;  /* 异步，无 completion */
    }

    /* 同步请求：唤醒等待的 VFS 回调 */
    if (!req->async)
        complete(&req->done);

    atomic_dec(&q->active_count);
}
```

#### 3.2.3 接收线程 + 响应匹配

```c
/* Pending 请求表（用于 seq → request 匹配） */
struct powerfs_pending_req {
    u32 seq;
    struct powerfs_request *req;     /* 关联的请求 */
    struct list_head list;           /* pending_list */
};

/* 接收线程（单个 kthread，对应 FUSE 的 transport receive loop） */
static int powerfs_recv_thread(void *data)
{
    struct powerfs_net *net = data;

    while (!kthread_should_stop()) {
        /* 1. 读取 TLV 帧 */
        int ret = powerfs_recv_frame(net, &frame);
        if (ret == -EINTR || ret == -ESHUTDOWN)
            break;
        if (ret < 0) {
            pr_warn_ratelimited("powerfs: recv error %d\n", ret);
            msleep(100);  /* 避免忙等 */
            continue;
        }

        /* 2. 响应帧：匹配 seq → complete() */
        if (powerfs_is_response(frame.msg_type)) {
            spin_lock(&net->pending_lock);
            struct powerfs_pending_req *pr = powerfs_find_pending(net, frame.seq);
            if (pr) {
                /* 拷贝响应数据 */
                memcpy(pr->req->resp_buf, frame.body, frame.len);
                pr->req->resp_len = frame.len;
                pr->req->status = 0;
                complete(&pr->req->done);
                list_del(&pr->list);
                kfree(pr);
            }
            spin_unlock(&net->pending_lock);
        }
        /* 3. NOTIFY 帧：提交到 notify_queue（不阻塞接收线程） */
        else if (frame.msg_type == POWERFS_NET_MSG_NOTIFY) {
            struct powerfs_request *req = kzalloc(sizeof(*req), GFP_ATOMIC);
            if (req) {
                req->kind = POWERFS_REQ_NOTIFY;
                req->async = true;
                req->notify_cb = powerfs_handle_invalidate;
                /* 拷贝 NOTIFY 内容 */
                powerfs_submit_request(sbi, req);
            }
        }
    }
    return 0;
}
```

#### 3.2.4 请求流程总结

```
┌─────────────────────────────────────────────────────────────────┐
│                    VFS 回调 (进程上下文)                          │
│  powerfs_lookup / powerfs_getattr / powerfs_read_folio          │
│         │                                                       │
│         ▼                                                       │
│  powerfs_submit_request(kind=META, sync=true)                   │
│         │                                                       │
│         ├──→ queue_work(meta_wq, &req->work)  [独立唤醒]         │
│         │         │                                             │
│         │    ┌────▼──────────────────────────┐                  │
│         │    │ meta_wq worker (max_active=8) │                  │
│         │    │  powerfs_send_to_filer()      │                  │
│         │    │  → 发送 TLV → 等待响应          │                  │
│         │    └───────────────────────────────┘                  │
│         │                                                       │
│         ▼                                                       │
│  wait_for_completion_timeout(&req->done, 15s)                   │
│         │                                                       │
│         ▼ (接收线程匹配 seq → complete)                          │
│  返回响应给 VFS                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                    异步路径 (后台上下文)                          │
│  • writepages → queue_work(data_wq, writeback_work)             │
│  • lease_renew → queue_work(lease_wq, renew_work)               │
│  • Filer NOTIFY → queue_work(notify_wq, invalidate_work)        │
│                                                                 │
│  4 个独立 workqueue：互不阻塞，各自 max_active 控制并发           │
└─────────────────────────────────────────────────────────────────┘
```

#### 3.2.5 并发控制对比

| FUSE (Rust) | 内核 (C workqueue) | 说明 |
|-------------|-------------------|------|
| `max_concurrent=32` | `max_active=0`（内核默认） | workqueue 自管理并发 |
| `data_notify.notify_one()` | `queue_work(data_wq, ...)` | 独立唤醒 |
| `tokio::spawn(task)` | `queue_work` 自动派发 | workqueue 内部管理 |
| `ConcurrencyGuard` (Drop) | `atomic_dec` 在 worker 末尾 | 引用计数递减 |
| 独立 Notify (3 个) | 独立 workqueue (4 个) | 互不阻塞 |
| `process_data_requests` loop | workqueue worker pool | 内核管理线程池 |
| 无对应（tokio 无优先级） | `lease_queue` 加 `WQ_HIGHPRI` | lease 续约延迟敏感（参考 GFS2 glock） |
| 无对应 | `delayed_work` + `queue_delayed_work` | lease 定时续约（参考 GFS2 `__gfs2_glock_queue_work`） |

```c
/* powerfs_net.h */

/* 网络传输层接口（对应 FUSE Transport trait） */
struct powerfs_net_ops {
    /* 发送请求并等待响应（同步） */
    int (*send_request)(struct powerfs_net *net, u16 msg_type,
                        const void *req_body, size_t req_len,
                        void *resp_body, size_t resp_max,
                        size_t *resp_len, int timeout_ms);
    /* 异步发送（不等待响应，用于 notify） */
    int (*send_notify)(struct powerfs_net *net, u16 msg_type,
                       const void *body, size_t len);
    /* 连接管理 */
    int (*connect)(struct powerfs_net *net, const char *addr, u16 port);
    void (*disconnect)(struct powerfs_net *net);
    bool (*is_connected)(struct powerfs_net *net);
};

/* 网络连接实例（对应 FUSE TransportPool） */
struct powerfs_net {
    const struct powerfs_net_ops *ops;
    struct socket *sock;              /* TCP socket */
    char addr[64];
    u16 port;

    /* 请求-响应匹配 */
    spinlock_t lock;
    u32 next_seq;
    struct list_head pending_list;    /* 等待响应的请求 */
    wait_queue_head_t waitq;

    /* 接收线程 */
    struct task_struct *recv_thread;
    bool running;

    /* shutdown */
    bool shutdown;
};
```

### 3.2 高层 API（对应 FuseClientFacade）

```c
/* powerfs_comm.h — 高层接口，屏蔽 TLV 细节 */

/* Filer 元数据操作 */
int powerfs_filer_lookup(struct powerfs_sb_info *sbi, u64 parent_ino,
                         const char *name, struct powerfs_lookup_resp *resp);
int powerfs_filer_getattr(struct powerfs_sb_info *sbi, u64 ino,
                          struct powerfs_getattr_resp *resp);
int powerfs_filer_create(struct powerfs_sb_info *sbi, u64 parent_ino,
                         const char *name, u32 mode, u32 uid, u32 gid,
                         struct powerfs_create_resp *resp);
int powerfs_filer_mkdir(struct powerfs_sb_info *sbi, u64 parent_ino,
                        const char *name, u32 mode, u32 uid, u32 gid,
                        struct powerfs_create_resp *resp);
int powerfs_filer_readdir(struct powerfs_sb_info *sbi, u64 ino, u64 offset,
                          struct powerfs_dirent *entries, u32 *count);

/* Volume Lease + 数据操作 */
int powerfs_volume_acquire_lease(struct powerfs_sb_info *sbi, u64 volume_id,
                                 u64 ino, u64 stripe_start, u64 stripe_count,
                                 bool exclusive, struct powerfs_lease_resp *resp);
int powerfs_volume_renew_lease(struct powerfs_sb_info *sbi, const char *token);
int powerfs_volume_release_lease(struct powerfs_sb_info *sbi, const char *token);
int powerfs_volume_read_blob(struct powerfs_sb_info *sbi, u64 volume_id,
                             u64 needle_id, u64 offset, size_t len,
                             void *buf, size_t *read_len);
int powerfs_volume_write_blob(struct powerfs_sb_info *sbi, u64 volume_id,
                              u64 needle_id, const void *data, size_t len,
                              u64 *new_needle_id);
```

## 四、Operations 详细设计

### 4.1 super_operations

```c
static const struct super_operations powerfs_super_ops = {
    .alloc_inode    = powerfs_alloc_inode,   /* slab + netfs_inode_init */
    .free_inode     = powerfs_free_inode,     /* kmem_cache_free */
    .evict_inode    = powerfs_evict_inode,    /* truncate_pages → clear_inode → release leases */
    .drop_inode     = generic_delete_inode,   /* 参考 ceph */
    .statfs         = powerfs_statfs,
    .show_options   = powerfs_show_options,
};
```

### 4.2 inode_operations

```c
/* 目录 inode_operations */
static const struct inode_operations powerfs_dir_inode_ops = {
    .lookup     = powerfs_lookup,       /* iget5_locked + fill_inode + d_splice_alias */
    .create     = powerfs_create,       /* filer_create + d_instantiate */
    .mkdir      = powerfs_mkdir,        /* filer_mkdir + d_instantiate */
    .unlink     = powerfs_unlink,       /* filer_unlink + d_drop */
    .rmdir      = powerfs_rmdir,        /* filer_rmdir + d_drop */
    .rename     = powerfs_rename,       /* filer_rename + d_move */
    .getattr    = powerfs_getattr,      /* filer_getattr + fill_inode */
    .setattr    = powerfs_setattr,      /* filer_setattr */
};

/* 文件 inode_operations */
static const struct inode_operations powerfs_file_inode_ops = {
    .getattr    = powerfs_getattr,
    .setattr    = powerfs_setattr,
};
```

### 4.3 file_operations

```c
static const struct file_operations powerfs_file_ops = {
    .open       = powerfs_open,         /* getattr 获取 chunks + acquire read lease */
    .release    = powerfs_release,      /* release_all_leases */
    .read       = powerfs_read,         /* 通用 read（触发 page cache → netfs issue_read） */
    .write      = powerfs_write,        /* 通用 write（触发 write_begin → acquire write lease） */
    .llseek     = generic_file_llseek,
    .mmap       = generic_file_mmap,
    .fsync      = powerfs_fsync,        /* writepages 同步 + filer metadata sync */
    .splice_read = generic_file_splice_read,
};

static const struct file_operations powerfs_dir_ops = {
    .open       = powerfs_dir_open,
    .release    = powerfs_dir_release,
    .iterate_shared = powerfs_readdir,  /* filer_readdir + filldir */
    .llseek     = generic_file_llseek,
};
```

### 4.4 dentry_operations

```c
static const struct dentry_operations powerfs_dentry_ops = {
    .d_init       = powerfs_d_init,       /* kmem_cache_zalloc */
    .d_release    = powerfs_d_release,    /* unlist + kmem_cache_free */
    .d_revalidate = powerfs_d_revalidate, /* lease 检查 + fallback lookup */
    .d_delete     = always_delete_dentry,
};
```

### 4.5 address_space_operations

```c
static const struct address_space_operations powerfs_aops = {
    .read_folio    = netfs_read_folio,         /* netfs 库 */
    .readahead     = netfs_readahead,          /* netfs 库 */
    .writepage     = powerfs_writepage,
    .writepages    = powerfs_writepages,
    .write_begin   = powerfs_write_begin,      /* netfs_write_begin + acquire lease */
    .write_end     = powerfs_write_end,        /* folio_mark_dirty + folio_put */
    .dirty_folio   = netfs_dirty_folio,        /* netfs 库 */
    .release_folio = powerfs_release_folio,
};

/* netfs 请求操作 */
static const struct netfs_request_ops powerfs_netfs_ops = {
    .init_request      = powerfs_init_request,
    .issue_read        = powerfs_netfs_issue_read,  /* 核心：Volume Server 读数据 */
    .expand_readahead  = powerfs_expand_readahead,
    .clamp_length      = powerfs_clamp_length,      /* 限制单次读不超过 chunk 边界 */
};
```

## 五、崩溃防护策略

### 5.1 内存分配

| 场景 | GFP 标志 | 原因 |
|------|---------|------|
| VFS 回调中分配 | `GFP_NOFS` | 避免递归进入文件系统 |
| 模块初始化 | `GFP_KERNEL` | 无递归风险 |
| 原子上下文 | `GFP_ATOMIC` | spinlock 持有期间 |
| slab 创建 | `SLAB_RECLAIM_ACCOUNT \| SLAB_MEM_SPREAD \| SLAB_ACCOUNT` | 让 shrinker 可回收 |

### 5.2 错误处理模式

```c
/* goto 链式清理（参考 ceph init_caches） */
static int powerfs_do_something(struct powerfs_sb_info *sbi)
{
    int ret;
    struct powerfs_lease *lease;

    lease = kmem_cache_zalloc(sbi->lease_cachep, GFP_NOFS);
    if (!lease)
        return -ENOMEM;

    ret = powerfs_volume_acquire_lease(sbi, ...);
    if (ret)
        goto out_free_lease;

    ret = powerfs_insert_lease(pi, lease);
    if (ret)
        goto out_release_lease;

    return 0;

out_release_lease:
    powerfs_volume_release_lease(sbi, lease->token);
out_free_lease:
    kmem_cache_free(sbi->lease_cachep, lease);
    return ret;
}
```

### 5.3 Shutdown 防护（参考 ceph_inode_is_shutdown）

```c
/* 所有 VFS 回调入口检查 shutdown */
static int powerfs_read_folio(struct file *file, struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    if (unlikely(pi->shutdown || POWERFS_SB(inode->i_sb)->shutting_down)) {
        folio_zero_range(folio, 0, folio_size(folio));
        folio_mark_uptodate(folio);
        folio_unlock(folio);
        return 0;  /* 优雅降级，不崩溃 */
    }
    /* 正常处理... */
}
```

### 5.4 禁止 BUG_ON

```c
/* 错误：内核 panic */
BUG_ON(!pi->chunks);  /* ❌ */

/* 正确：警告 + 优雅返回 */
if (WARN_ON_ONCE(!pi->chunks))
    return -EIO;      /* ✅ */
```

### 5.5 网络超时

```c
/* 所有网络请求必须有超时（参考 GRPC_CALL_TIMEOUT=15s） */
#define POWERFS_NET_TIMEOUT_MS  15000

ret = powerfs_net_send_request(net, msg_type, body, len,
                                resp, resp_max, &resp_len,
                                POWERFS_NET_TIMEOUT_MS);
if (ret == -ETIMEDOUT || ret == -ECONNRESET) {
    /* 优雅降级，不崩溃 */
    pr_warn_ratelimited("powerfs: network timeout, falling back\n");
    return -EIO;
}
```

### 5.6 锁层次（避免死锁）

```
powerfs_sb_info->mount_mutex     (mutex)
  └─ powerfs_inode_info->dir_mutex (mutex)
       └─ powerfs_inode_info->lease_lock (spinlock)
            └─ folio_lock (spinlock, 内核管理)
```

**规则**：永远不反向获取锁；spinlock 持有期间不睡眠、不分配内存。

## 五.5、调试信息规划（参考 Ceph ceph_debug.h + 9p tracepoints + Ceph debugfs）

### 5.5.1 日志前缀与级别（参考 Ceph `ceph_debug.h`）

**现状问题**：现有 `powerfs_net.c` 等文件大量使用 `pr_err("powerfs: ...")` 手动加前缀，应改用 `pr_fmt` 统一处理。

**Ceph 的 `dout` 宏**（`include/linux/ceph/ceph_debug.h:18-27`，CONFIG_CEPH_LIB_PRETTYDEBUG 时）：
```c
# if defined(DEBUG) || defined(CONFIG_DYNAMIC_DEBUG)
#  define dout(fmt, ...)                                            \
    pr_debug("%.*s %12.12s:%-4d : " fmt,                            \
         8 - (int)sizeof(KBUILD_MODNAME), "    ",                   \
         kbasename(__FILE__), __LINE__, ##__VA_ARGS__)
# else
#  define dout(fmt, ...) do {                                       \
    if (0)                                                          \
        printk(KERN_DEBUG fmt, ##__VA_ARGS__);                      \
#  endif
#endif
```

PowerFS 采用 Ceph 完全相同的模式：

```c
/* powerfs.h 顶部 — 必须 #define pr_fmt 在 #include 之前 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt  /* 所有 printk 自动加 "powerfs: " 前缀 */
/* 之后所有 pr_err("foo") 输出 "powerfs: foo"，不再手写 "powerfs: " 前缀 */

/* powerfs_debug.h — 参考 include/linux/ceph/ceph_debug.h */
#ifndef _POWERFS_DEBUG_H
#define _POWERFS_DEBUG_H

#include <linux/printk.h>

/* 调试宏：参考 ceph dout，带文件名:行号前缀，集成 dynamic_debug */
#if defined(DEBUG) || defined(CONFIG_DYNAMIC_DEBUG)
#  define powerfs_dbg(fmt, ...)                                      \
    pr_debug("%.*s %12.12s:%-4d : " fmt,                            \
             8 - (int)sizeof(KBUILD_MODNAME), "    ",                \
             kbasename(__FILE__), __LINE__, ##__VA_ARGS__)
#else
/* faux printk call just to see any compiler warnings. (ceph 同款) */
#  define powerfs_dbg(fmt, ...) do {                                 \
    if (0)                                                           \
        printk(KERN_DEBUG fmt, ##__VA_ARGS__);                       \
} while (0)
#endif

#endif /* _POWERFS_DEBUG_H */
```

**日志级别使用规范**（参考 Ceph 源码实际用法）：

| 级别 | 宏 | 使用场景 | Ceph 参考位置 |
|------|-----|---------|-------------|
| ERROR | `pr_err` | 严重错误，影响数据一致性 | `caps.c` cap export/import 错误 |
| ERROR (限速) | `pr_err_ratelimited` | 重复性错误（网络超时、ACL 失败） | `quota.c`, `acl.c` |
| WARN | `pr_warn` | 异常但可恢复 | `inode.c:1868` shutdown 警告 |
| WARN (限速) | `pr_warn_ratelimited` | 重复性警告（请求丢弃、seq 溢出） | `mds_client.c:1608` |
| INFO | `pr_info` | 关键生命周期事件（mount/unmount/connect） | `super.c` |
| DEBUG | `powerfs_dbg` | 开发调试（lookup/readdir/lease 流程） | 全文 `dout()` |
| DEBUG (运行时) | `pr_debug` | 动态调试（可通过 dynamic_debug 控制） | — |

**限速规则**（参考 Ceph `pr_warn_ratelimited` 用法）：
```c
/* 网络错误：可能高频重复，必须限速 */
pr_warn_ratelimited("network timeout to %s:%u\n", addr, port);  /* pr_fmt 自动加 powerfs: 前缀 */
pr_err_ratelimited("lease acquire failed for ino %llu\n", ino);

/* 生命周期事件：低频，不需要限速 */
pr_info("mounted, filer=%s:%u\n", filer_addr, filer_port);
pr_err("mount failed, ret=%d\n", ret);
```

### 5.5.2 运行时动态调试（参考 Ceph dynamic_debug 集成）

`powerfs_dbg` 和 `pr_debug` 在 `CONFIG_DYNAMIC_DEBUG` 启用时自动集成到 dynamic_debug 框架，无需重新编译即可控制：

```bash
# 启用 powerfs 模块所有调试日志
echo 'module powerfs +p' > /sys/kernel/debug/dynamic_debug/control

# 仅启用特定文件
echo 'file powerfs_fs.c +p' > /sys/kernel/debug/dynamic_debug/control

# 仅启用特定函数
echo 'func powerfs_lookup +p' > /sys/kernel/debug/dynamic_debug/control

# 关闭
echo 'module powerfs -p' > /sys/kernel/debug/dynamic_debug/control

# 查看当前状态
cat /sys/kernel/debug/dynamic_debug/control | grep powerfs
```

### 5.5.3 Tracepoints（参考 9p `include/trace/events/9p.h`）

**9p 是网络文件系统 tracepoints 的最佳参考**（`include/trace/events/9p.h`），覆盖请求/响应/fid 引用/协议 dump。PowerFS 借鉴 9p 定义 `include/trace/events/powerfs.h`：

```c
/* include/trace/events/powerfs.h — 参考 include/trace/events/9p.h */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM powerfs

#if !defined(_TRACE_POWERFS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_POWERFS_H

#include <linux/tracepoint.h>

/* 请求类型枚举（参考 9p P9_MSG_T 的 EM/EMe 模式） */
#define POWERFS_REQ_T                                   \
    EM( POWERFS_NET_MSG_LOOKUP,     "LOOKUP"    )       \
    EM( POWERFS_NET_MSG_GETATTR,    "GETATTR"   )       \
    EM( POWERFS_NET_MSG_CREATE,     "CREATE"    )       \
    EM( POWERFS_NET_MSG_MKDIR,      "MKDIR"     )       \
    EM( POWERFS_NET_MSG_READDIR,    "READDIR"   )       \
    EM( POWERFS_NET_MSG_ACQUIRE_LEASE, "ACQ_LEASE" )    \
    EM( POWERFS_NET_MSG_RENEW_LEASE,   "REN_LEASE" )    \
    EM( POWERFS_NET_MSG_RELEASE_LEASE, "REL_LEASE" )    \
    EM( POWERFS_NET_MSG_READ_BLOB,  "READ_BLOB" )       \
    EM( POWERFS_NET_MSG_WRITE_BLOB, "WRITE_BLOB")       \
    EMe(POWERFS_NET_MSG_NOTIFY,     "NOTIFY"    )

#undef EM
#undef EMe
#define EM(a, b)  a,
#define EMe(a, b) a
enum powerfs_msg_type { POWERFS_REQ_T };
#undef EM
#undef EMe
#define EM(a, b)  { a, b },
#define EMe(a, b) { a, b }
#define show_powerfs_op(type)  __print_symbolic(type, POWERFS_REQ_T)

/* 请求 tracepoint（参考 9p_client_req, 9p.h:124） */
TRACE_EVENT(powerfs_req,
    TP_PROTO(u32 seq, u8 type, u64 ino),
    TP_ARGS(seq, type, ino),
    TP_STRUCT__entry(
        __field(u32, seq)
        __field(u8,  type)
        __field(u64, ino)
    ),
    TP_fast_assign(
        __entry->seq  = seq;
        __entry->type = type;
        __entry->ino  = ino;
    ),
    TP_printk("seq=%u %s ino=%llu", __entry->seq,
              show_powerfs_op(__entry->type), __entry->ino)
);

/* 响应 tracepoint（参考 9p_client_res, 9p.h:146） */
TRACE_EVENT(powerfs_res,
    TP_PROTO(u32 seq, u8 type, int err),
    TP_ARGS(seq, type, err),
    TP_STRUCT__entry(
        __field(u32, seq)
        __field(u8,  type)
        __field(int, err)
    ),
    TP_fast_assign(
        __entry->seq  = seq;
        __entry->type = type;
        __entry->err  = err;
    ),
    TP_printk("seq=%u %s err=%d", __entry->seq,
              show_powerfs_op(__entry->type), __entry->err)
);

/* Lease 生命周期 tracepoint（Ceph 无对应，PowerFS 自定义） */
TRACE_EVENT(powerfs_lease,
    TP_PROTO(u64 ino, u64 stripe_start, const char *action, u64 expire_jif),
    TP_ARGS(ino, stripe_start, action, expire_jif),
    TP_STRUCT__entry(
        __field(u64,        ino)
        __field(u64,        stripe_start)
        __string(action,    action)
        __field(u64,        expire_jif)
    ),
    TP_fast_assign(
        __entry->ino          = ino;
        __entry->stripe_start = stripe_start;
        __assign_str(action, action);
        __entry->expire_jif   = expire_jif;
    ),
    TP_printk("ino=%llu stripe=%llu %s expire=%llu",
              __entry->ino, __entry->stripe_start,
              __get_str(action), __entry->expire_jif)
);

#endif /* _TRACE_POWERFS_H */

/* 必须在 #include 之外，参考 9p.h 末尾模式 */
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE powerfs
#include <trace/define_trace.h>
```

**使用方式**（参考 9p）：
```bash
# 启用所有 powerfs tracepoints
echo 1 > /sys/kernel/debug/tracing/events/powerfs/enable

# 仅启用 lease tracepoint
echo 1 > /sys/kernel/debug/tracing/events/powerfs/powerfs_lease/enable

# 查看追踪结果
cat /sys/kernel/debug/tracing/trace

# 用 trace-cmd 录制（更高效）
trace-cmd record -e powerfs
trace-cmd report trace.dat
```

**Makefile 集成**（参考 fs/9p/Makefile）：
```makefile
# 必须让 tracepoint 在模块中生效
CFLAGS_powerfs_tracepoints.o = -I$(src)
powerfs-y += powerfs_tracepoints.o
```

### 5.5.4 debugfs 统计信息（参考 Ceph `debugfs.c` seq_file 模式）

**Ceph debugfs 实际暴露内容**（`fs/ceph/debugfs.c:412-465`）：
- `mdsmap` — MDS 集群拓扑（`mdsmap_show` 用 `seq_printf`）
- `mds_sessions` — MDS 会话列表
- `mdsc` — 请求树（`mdsc_show` 遍历 `mdsc->request_tree` rbtree，`debugfs.c:52`）
- `caps` — cap 统计
- `status` — 集群状态
- `metrics/file`, `metrics/latency`, `metrics/size`, `metrics/caps` — 性能指标

PowerFS 在 `/sys/kernel/debug/powerfs/` 下暴露：

```c
/* powerfs_debugfs.c — 参考 fs/ceph/debugfs.c 的 seq_file 模式 */

struct powerfs_stats {
    /* 元数据统计 */
    atomic64_t lookup_count;
    atomic64_t getattr_count;
    atomic64_t readdir_count;

    /* Lease 统计 */
    atomic64_t lease_acquire_count;
    atomic64_t lease_renew_count;
    atomic64_t lease_release_count;
    atomic64_t lease_active;

    /* 数据统计 */
    atomic64_t read_bytes;
    atomic64_t write_bytes;
    atomic64_t read_errors;
    atomic64_t write_errors;

    /* 队列统计 */
    atomic_t meta_active;
    atomic_t lease_active_workers;
    atomic_t data_active;
};

/* stats_show — 参考 ceph mdsc_show 的 seq_printf 模式 */
static int powerfs_stats_show(struct seq_file *s, void *p)
{
    struct powerfs_sb_info *sbi = s->private;

    seq_printf(s, "=== PowerFS Statistics ===\n");
    seq_printf(s, "\n[Metadata]\n");
    seq_printf(s, "  lookup:   %lld\n", atomic64_read(&sbi->stats.lookup_count));
    seq_printf(s, "  getattr:  %lld\n", atomic64_read(&sbi->stats.getattr_count));
    seq_printf(s, "  readdir:  %lld\n", atomic64_read(&sbi->stats.readdir_count));

    seq_printf(s, "\n[Lease]\n");
    seq_printf(s, "  acquire:  %lld\n", atomic64_read(&sbi->stats.lease_acquire_count));
    seq_printf(s, "  renew:    %lld\n", atomic64_read(&sbi->stats.lease_renew_count));
    seq_printf(s, "  release:  %lld\n", atomic64_read(&sbi->stats.lease_release_count));
    seq_printf(s, "  active:   %lld\n", atomic64_read(&sbi->stats.lease_active));

    seq_printf(s, "\n[Data]\n");
    seq_printf(s, "  read:     %lld bytes\n", atomic64_read(&sbi->stats.read_bytes));
    seq_printf(s, "  write:    %lld bytes\n", atomic64_read(&sbi->stats.write_bytes));
    seq_printf(s, "  rd_err:   %lld\n", atomic64_read(&sbi->stats.read_errors));
    seq_printf(s, "  wr_err:   %lld\n", atomic64_read(&sbi->stats.write_errors));

    seq_printf(s, "\n[Queues]\n");
    seq_printf(s, "  meta:     %d active\n", atomic_read(&sbi->stats.meta_active));
    seq_printf(s, "  lease:    %d active\n", atomic_read(&sbi->stats.lease_active_workers));
    seq_printf(s, "  data:     %d active\n", atomic_read(&sbi->stats.data_active));
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(powerfs_stats);  /* 内核 5.18+ 提供的简化宏 */

/* leases_show — 参考 ceph mdsc_show 遍历 rbtree 模式 */
static int powerfs_leases_show(struct seq_file *s, void *p)
{
    struct powerfs_sb_info *sbi = s->private;
    struct powerfs_inode_info *pi;
    struct rb_node *node;
    struct inode *inode;

    /* 遍历所有 inode 的 lease_tree（参考 ceph mdsc_show 遍历 request_tree） */
    seq_printf(s, "ino\t\tstripe_start\texclusive\texpire(jif)\n");
    spin_lock(&sbi->inode_list_lock);
    list_for_each_entry(pi, &sbi->inode_list, i_list) {
        spin_lock(&pi->lease_lock);
        for (node = rb_first(&pi->lease_tree); node; node = rb_next(node)) {
            struct powerfs_lease *l = rb_entry(node, struct powerfs_lease, node);
            seq_printf(s, "%llu\t%llu\t\t%d\t\t%lu\n",
                       pi->netfs.inode.i_ino, l->stripe_start,
                       l->exclusive, l->expire_jiffies);
        }
        spin_unlock(&pi->lease_lock);
    }
    spin_unlock(&sbi->inode_list_lock);
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(powerfs_leases);

/* 创建 debugfs 条目（参考 ceph_fs_debugfs_init, debugfs.c:412-465） */
void powerfs_debugfs_init(struct powerfs_sb_info *sbi)
{
    struct dentry *root = debugfs_create_dir("powerfs", NULL);
    sbi->debugfs_root = root;

    /* 统计文件（参考 ceph debugfs_create_file） */
    debugfs_create_file("stats",   0400, root, sbi, &powerfs_stats_fops);
    debugfs_create_file("leases",  0400, root, sbi, &powerfs_leases_fops);
    debugfs_create_file("queues",  0400, root, sbi, &powerfs_queues_fops);

    /* 直接暴露原子计数（参考 ceph 的 debugfs_create_u64） */
    debugfs_create_u64("read_bytes",  0400, root, &sbi->stats.read_bytes.counter);
    debugfs_create_u64("write_bytes", 0400, root, &sbi->stats.write_bytes.counter);
}

void powerfs_debugfs_exit(struct powerfs_sb_info *sbi)
{
    /* 递归删除整个目录树（参考 ceph） */
    debugfs_remove_recursive(sbi->debugfs_root);
}
```

**使用方式**：
```bash
# 查看统计
cat /sys/kernel/debug/powerfs/stats

# 查看活跃 lease（排查 lease 泄漏）
cat /sys/kernel/debug/powerfs/leases

# 查看队列状态（排查队列堆积）
cat /sys/kernel/debug/powerfs/queues

# 监控读写字节
watch -n1 cat /sys/kernel/debug/powerfs/read_bytes
```

### 5.5.5 内核 panic 防护与 crash dump（修正：Ceph 实际使用 BUG_ON）

**源码研究修正**：之前"参考 Ceph 无 BUG_ON"的结论是错的。Ceph 实际有大量 BUG_ON：
- `fs/ceph/dir.c:514` `BUG_ON(rde->offset < ctx->pos)`
- `fs/ceph/dir.c:521` `BUG_ON(!rde->inode.in)`
- `fs/ceph/xattr.c:671` `BUG_ON(!xattr)`
- `fs/ceph/file.c:213` `BUG_ON(inode->i_fop->release != ceph_release)`

**正确指导**：Ceph 的模式是——**可恢复的错误用 WARN_ON_ONCE + 返回错误码；真正"不可能发生"的不变式违反用 BUG_ON**（如内部数据结构损坏、类型混淆）。PowerFS 采用同一哲学：

```c
/* 1. 可恢复错误：WARN_ON_ONCE + 优雅返回（多数情况） */
if (WARN_ON_ONCE(!pi->chunks))
    return -EIO;

/* 2. 数据结构不变式违反：BUG_ON（参考 ceph dir.c:514, xattr.c:671）
 *    仅用于"如果发生说明内核本身已损坏，继续运行会更危险"的场景 */
BUG_ON(lease->stripe_count != POWERFS_STRIPE_SIZE);  /* 不变式，构造时保证 */

/* 3. shutdown 检查（参考 ceph_inode_is_shutdown） */
if (unlikely(pi->shutdown)) {
    pr_warn_ratelimited("inode %llu is shut down\n", inode->i_ino);
    return -EIO;
}

/* 4. KASAN/KMSAN 编译选项（Makefile 中可选启用） */
# make C=1 KASAN=1   # 启用 AddressSanitizer 检测内存越界
# make C=1 KMSAN=1   # 启用 MemorySanitizer 检测未初始化内存
```

**BUG_ON vs WARN_ON_ONCE 决策树**：
```
错误是否可恢复？
├─ 是 → WARN_ON_ONCE + 返回错误码（-EIO/-ENOMEM）
└─ 否 → 继续运行是否会导致数据损坏/内存破坏？
    ├─ 是 → BUG_ON（如：释放后使用、类型混淆、rbtree 损坏）
    └─ 否 → WARN_ON_ONCE + 尽力恢复
```

### 5.5.6 QEMU 内核调试

```bash
# 1. QEMU 启动加 -s -S 支持远程 GDB（已有 run_qemu_debug.sh）
# 2. host 上 GDB 连接
gdb /home/portion/powerfs/ubuntu-linux-git/vmlinux
(gdb) target remote localhost:1234
(gdb) break powerfs_lookup
(gdb) continue

# 3. dmesg 实时监控
# 在 QEMU 内：
dmesg -w | grep powerfs

# 4. 动态调试（无需重新编译）
echo 'module powerfs +p' > /sys/kernel/debug/dynamic_debug/control

# 5. tracepoints 实时追踪（参考 9p 用法）
echo 1 > /sys/kernel/debug/tracing/events/powerfs/enable
cat /sys/kernel/debug/tracing/trace | tail -50

# 6. crash dump 分析（如果 QEMU 内核崩溃）
# QEMU 配置内核 panic=0 避免自动重启，保留现场
# 检查 /var/log/kdump 或串口输出
```

## 六、Shrink 机制利用

### 6.1 Slab Shrinker（自动）

```c
/* SLAB_RECLAIM_ACCOUNT 让内核 shrinker 自动回收 slab 对象 */
powerfs_inode_cachep = kmem_cache_create("powerfs_inode_info",
    sizeof(struct powerfs_inode_info),
    __alignof__(struct powerfs_inode_info),
    SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,  /* ← 关键 */
    powerfs_inode_init_once);

powerfs_dentry_cachep = KMEM_CACHE(powerfs_dentry_info,
    SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD);   /* ← 关键 */

powerfs_lease_cachep = KMEM_CACHE(powerfs_lease, SLAB_MEM_SPREAD);
/* lease 不加 RECLAIM_ACCOUNT：lease 回收需要先 release 到 Volume Server */
```

**效果**：内存紧张时内核自动调用 `powerfs_free_inode` / `powerfs_d_release` 回收 slab 对象，无需自定义 shrinker。

### 6.2 Page Cache LRU（自动）

内核自动管理 page cache LRU：
- 空闲内存不足时触发 writeback → `powerfs_writepage`
- `truncate_inode_pages_final` 在 evict_inode 中清理
- **不需要自定义 shrinker**

### 6.3 Inode/ Dentry Cache（自动）

`iget5_locked` 分配的 inode 和 `d_splice_alias` 关联的 dentry 由内核 VFS 管理：
- 内核 LRU 在内存压力时自动回收
- `generic_delete_inode` → `evict_inode` → `free_inode`
- **不需要自定义 shrinker**

## 七、渐进式实现计划（每步可测试）

### Step 0: 模块骨架（最小可加载）

**目标**：insmod/rmmod 不崩溃，无功能

**内容**：
- 改造 `powerfs_inode_info`（netfs_inode + 新字段）
- 改造 `powerfs_alloc_inode` / `free_inode` / `evict_inode`
- `powerfs_fill_super`：创建根 inode，注册 super_ops
- `powerfs_init_inode_cache` / `destroy_inode_cache`
- 文件系统注册（`register_filesystem`）

**测试**：
```bash
# 在 QEMU 中
insmod powerfs.ko
dmesg | grep powerfs    # 无错误
mount -t powerfs none /mnt/powerfs
ls /mnt/powerfs         # 空目录
umount /mnt/powerfs
rmmod powerfs
dmesg | grep powerfs    # 无泄漏/错误
```

**验证清单**：
- [x] insmod 成功，dmesg 无 WARN
- [x] mount 成功
- [x] ls 显示空根目录
- [x] umount 成功
- [x] rmmod 成功，无 slab 泄漏 (`/proc/slabinfo` 查 powerfs_inode_info = 0)

### Step 1: 元数据通路（lookup + getattr）

**目标**：能 `ls` 和 `stat` 已有文件

**内容**：
- `powerfs_filer_lookup`：向 Filer 发 TLV LOOKUP 请求
- `powerfs_filer_getattr`：向 Filer 发 TLV GETATTR 请求
- `powerfs_lookup`：iget5_locked + fill_inode + d_splice_alias
- `powerfs_getattr`：filer_getattr + generic_fillattr
- `powerfs_fill_inode`：从 Filer 响应填充 inode 属性 + chunks

**测试**：
```bash
# 先用 FUSE 客户端创建测试文件
# 然后内核客户端挂载
mount -t powerfs none /mnt/powerfs
ls /mnt/powerfs/           # 能看到 FUSE 创建的文件
stat /mnt/powerfs/testfile # getattr 正确
ls -la /mnt/powerfs/       # 所有属性正确
```

**验证清单**：
- [ ] lookup 正确返回 inode
- [ ] dentry 缓存生效（第二次 ls 无网络请求）
- [ ] getattr 属性正确（mode, size, uid, gid, timestamps）
- [ ] chunk 映射正确填充到 inode_info
- [ ] d_revalidate lease 生效

### Step 2: 读路径（netfs issue_read）

**目标**：能 `cat` 已有文件

**内容**：
- `powerfs_netfs_ops.issue_read`：从 Volume Server 读 chunk 数据
- `powerfs_volume_acquire_lease`：获取读共享 lease
- `powerfs_volume_read_blob`：TLV READ_BLOB 请求
- `powerfs_clamp_length`：限制单次读不超过 chunk 边界（2MB）
- 后台 lease 续约 work

**测试**：
```bash
# FUSE 创建文件 → 内核客户端读
mount -t powerfs none /mnt/powerfs
cat /mnt/powerfs/testfile > /tmp/copy
diff /tmp/copy original_file  # 内容一致
md5sum /mnt/powerfs/testfile  # CRC32 校验
# fio 顺序读
fio --name=seqread --filename=/mnt/powerfs/testfile \
    --bs=64k --rw=read --size=64M --ioengine=psync
```

**验证清单**：
- [ ] 读取 2MB 文件正确（单 chunk）
- [ ] 读取 10MB 文件正确（多 chunk）
- [ ] 读取 100MB 文件正确（跨 stripe lease）
- [ ] CRC32 校验通过
- [ ] lease acquire/renew 正常（dmesg 无 "lease expired" 错误）
- [ ] fio 顺序读无 EIO

### Step 3: 写路径

**目标**：能创建文件并写入

**内容**：
- `powerfs_write_begin`：acquire exclusive lease + netfs_write_begin
- `powerfs_write_end`：folio_mark_dirty + folio_put
- `powerfs_writepages`：write_cache_pages + volume_write_blob
- `powerfs_fsync`：同步写回 + filer metadata sync
- `powerfs_create` / `powerfs_mkdir`：filer_create + d_instantiate

**测试**：
```bash
mount -t powerfs none /mnt/powerfs
echo "hello" > /mnt/powerfs/newfile    # 写入
cat /mnt/powerfs/newfile               # 读回验证
dd if=/dev/urandom of=/mnt/powerfs/bigfile bs=1M count=100
md5sum /mnt/powerfs/bigfile
# FUSE 客户端读取验证跨客户端一致性
```

**验证清单**：
- [ ] 小文件写入正确
- [ ] 大文件写入正确（多 chunk）
- [ ] fsync 后数据持久化
- [ ] 跨客户端一致性（内核写 → FUSE 读）
- [ ] write lease 正确释放

### Step 4: Callback Invalidation

**目标**：跨客户端缓存一致性

**内容**：
- Filer NOTIFY 帧处理
- inode/dentry invalidate
- dirty inode 跳过

**测试**：
```bash
# FUSE 客户端修改文件 → 内核客户端感知
# 内核客户端 ls → FUSE 新建文件可见
```

## 八、代码质量检查

每步实现后执行：

```bash
# 1. 编译检查（零警告）
cd /home/portion/powerfs/kernel/powerfs_mod
make C=1 EXTRA_CFLAGS="-Werror" 2>&1 | grep -v "^make" | head -20

# 2. Sparse 静态分析
make C=2 2>&1 | grep -i "warning\|error" | head -20

# 3. 模块符号检查
nm powerfs.ko | grep -i " U "   # 不应有未定义符号

# 4. QEMU 中加载测试
# (见各 Step 测试流程)
```

## 九、QEMU 环境确保

**所有内核模块测试必须在 QEMU 中进行。**

```bash
# 启动后端
cd /home/portion/powerfs/docker
docker compose -f docker-compose.kernel-test.yml up -d

# 编译内核模块
cd /home/portion/powerfs/kernel/powerfs_mod
make

# 启动 QEMU（内核 6.2.0）
cd /home/portion/powerfs/kernel/vm
./run_qemu_kernel.sh

# SSH 到 QEMU
ssh root@172.20.0.100
# 在 QEMU 内测试...
```

## 十、实现进展与问题记录

### Step 0 验证结果 (2026-08-03)

**状态：✅ 全部通过**

测试环境：QEMU (内核 6.2.0, 4CPU/4GB), 后端 Docker 服务运行中 (filer-1:8888)

测试流程：`step0_test` 脚本自动执行 insmod → mount → ls → dmesg → umount → rmmod → slab 检查

最终测试日志 (dmesg 关键时间线):
```
[ 1.641] slab caches created
[ 1.641] net subsystem initialized
[11.810] connect to 172.20.0.1:8888 failed: -115  (后端端口不匹配, 不影响 Step 0)
[11.810] trying to reconnect (attempt 1)
[12.827] fill_super → root inode created (ino=1)
[12.828] dir_open ino=1
[12.837] kill_sb_super (umount)
[12.840] module exit (rmmod)
[13.858] reconnect stopped after sleep (module exiting)  ← atomic_t 立即生效
[13.858] net subsystem exited
[13.858] slab caches destroyed
[13.858] module unloaded
```

验证清单结果:
- ✅ insmod 成功，无 WARN/oops
- ✅ mount 成功，root inode (ino=1) 创建
- ✅ ls 显示空根目录
- ✅ umount 成功，kill_sb_super 清理正常
- ✅ rmmod 成功 (耗时 1.0 秒)
- ✅ slab 无泄漏 (`/proc/slabinfo` 无 powerfs 条目)

### 遇到的问题与修复

#### 问题 1: rmmod HUNG (已修复)

**现象**: `rmmod` 超过 10 秒未完成，被 `kill -9` 强制终止

**根因**: `g_conn.stopping` 使用 `bool` 类型，在多 CPU 环境下存在内存可见性问题。
`reconnect_work` 在 `msleep` 醒来后看不到 `stopping=true`，调用了 `kernel_connect`，
而 TCP connect 超时约 10 秒，导致 `cancel_work_sync` 阻塞。

**修复**: 将 `stopping` 改为 `atomic_t`，使用 `atomic_read`/`atomic_set` 保证跨 CPU 可见性:
```c
// powerfs_net.h
struct powerfs_net_conn {
    atomic_t stopping;  // 替代 bool stopping
};

// powerfs_net.c - reconnect_work 中检查
if (atomic_read(&g_conn.stopping)) {
    pr_info("powerfs: reconnect stopped after sleep (module exiting)\n");
    return;
}

// powerfs_net.c - powerfs_net_exit 中设置
atomic_set(&g_conn.stopping, 1);
```

**效果**: rmmod 耗时从 11.2 秒降至 1.0 秒 (11x 提升)

#### 问题 2: health monitor stopped 重复打印 (已修复)

**现象**: `powerfs: health monitor stopped` 在 dmesg 中打印两次

**根因**: `kill_sb_super` 调用 `powerfs_net_stop_monitor`，随后 `powerfs_net_exit` →
`powerfs_net_pool_exit` 再次调用 `powerfs_net_stop_monitor`。

**修复**: 在 `powerfs_net_stop_monitor` 开头添加 `monitoring` 标志检查:
```c
void powerfs_net_stop_monitor(void)
{
    if (!g_pool.monitoring)  // 已停止则跳过
        return;
    g_pool.monitoring = false;
    cancel_delayed_work_sync(&g_pool.monitor_work);
    cancel_delayed_work_sync(&g_pool.leader_check_work);
    pr_info("powerfs: health monitor stopped\n");
}
```

#### 问题 3: slab 泄漏 (已修复)

**现象**: `powerfs_inode_cache` 显示 29 个 active objects 未释放

**根因**: `kill -9 rmmod` 导致模块退出不完整，inode slab 未被正确销毁。
正常运行 `rmmod` (非 kill -9) 时无此问题。

**修复**: 确保 `evict_inode` 正确清理所有资源 (lease_tree, chunks, dir_entries)，
`powerfs_free_inode` 调用 `kmem_cache_free`。非强制终止时 slab 正常释放。

#### 问题 4: evict_inode 初始化顺序 (已修复)

**现象**: `evict_inode` 中 `cancel_delayed_work_sync` 访问未初始化的 `lease_renew_work`

**修复**: 在 `powerfs_alloc_inode` 中初始化所有字段:
```c
pi->lease_tree = RB_ROOT;
spin_lock_init(&pi->lease_lock);
INIT_DELAYED_WORK(&pi->lease_renew_work, powerfs_lease_renew_work_func);
INIT_LIST_HEAD(&pi->dir_entries);
mutex_init(&pi->dir_mutex);
```
在 `evict_inode` 中按正确顺序清理:
```c
truncate_inode_pages_final(&inode->i_data);  // 1. 清理 page cache
cancel_delayed_work_sync(&pi->lease_renew_work);  // 2. 取消 work (在 clear_inode 前)
clear_inode(inode);  // 3. VFS 清理
// 4. 释放 lease_tree, chunks, dir_entries
```

### 代码变更摘要

**powerfs_net.h**:
- `bool stopping` → `atomic_t stopping` (跨 CPU 可见性)

**powerfs_net.c**:
- `powerfs_net_init`: `atomic_set(&g_conn.stopping, 0)` 初始化
- `powerfs_net_exit`: `atomic_set(&g_conn.stopping, 1)` 设置停止标志
- `powerfs_net_reconnect_work`: 3 处 `atomic_read(&g_conn.stopping)` 检查 (循环开始/msleep 后/connect 前)
- `powerfs_net_stop_monitor`: 添加 `monitoring` 标志检查，避免重复调用

**powerfs_fs.c**:
- `powerfs_alloc_inode`: 初始化 `lease_tree`, `lease_lock`, `lease_renew_work`, `dir_entries`, `dir_mutex`
- `powerfs_evict_inode`: 按 `truncate_inode_pages_final` → `cancel_delayed_work_sync` → `clear_inode` → 释放资源顺序清理

### Step 1 验证结果 (2026-08-03)

**状态：🔧 修复中（lookup 根因已定位并修复，待 QEMU 回归验证）**

测试环境：QEMU (内核 6.2.0, 4CPU/4GB), 后端 Docker filer (172.30.0.35:9334), VM 172.30.0.100/16

#### 问题 5: `d_instantiate` 触发 `BUG_ON` (根因修复，非 workaround)

**现象**: `ls -la /mnt/pfs` (lstat 遍历) 触发内核 BUG，栈：
```
kernel BUG at fs/dcache.c:2032!
RIP: 0010:d_instantiate+0x1a/0x20
Call Trace:
 powerfs_lookup.part.0+0x15d/0x4c0 [powerfs]
 __lookup_slow+0x80/0x130
 walk_component+0xe0/0x150
 filename_lookup+0xce/0x1a0    ← lstat 路径
```

**根因（核心）**：`->lookup` 在 dentry 仍处于 `DCACHE_PAR_LOOKUP` 时被调用，
而 `powerfs_lookup` 错误地使用了 `d_instantiate()` 而非 `d_add()`。

关键证据链（全部来自内核源码，非推测）：

1. `include/linux/dcache.h:100-112` —— `struct dentry` 中 `d_alias` 与
   `d_in_lookup_hash` 共享同一段 union 内存：
   ```c
   union {
       struct hlist_node d_alias;            /* inode alias list */
       struct hlist_bl_node d_in_lookup_hash; /* only for in-lookup ones */
       struct rcu_head d_rcu;
   };
   ```

2. `fs/namei.c:1670-1685` —— `__lookup_slow` 通过 `d_alloc_parallel` 分配
   PAR_LOOKUP dentry（`d_in_lookup_hash` 已链入 in_lookup 哈希表），随后才
   调用 `inode->i_op->lookup()`。此时 `d_alias`（同一段内存）也表现为
   "hashed"。

3. `fs/dcache.c:2030-2032` —— `d_instantiate` 第一行就是：
   ```c
   void d_instantiate(struct dentry *entry, struct inode *inode) {
       BUG_ON(!hlist_unhashed(&entry->d_u.d_alias));   /* ← 直接 BUG */
   ```
   由于 union 共享，`d_alias` 此刻是 hashed → 触发 `BUG_ON`。

4. `fs/dcache.c:2775-2800` —— `__d_add`（`d_add` 的实现）正确处理此场景：
   ```c
   static inline void __d_add(struct dentry *dentry, struct inode *inode) {
       spin_lock(&dentry->d_lock);
       if (unlikely(d_in_lookup(dentry))) {        /* 检测 PAR_LOOKUP */
           dir = dentry->d_parent->d_inode;
           n = start_dir_add(dir);
           d_wait = __d_lookup_unhash(dentry);     /* 先清理 in_lookup_hash */
       }
       if (inode)
           hlist_add_head(&dentry->d_u.d_alias, &inode->i_dentry);  /* 再用 d_alias */
       __d_rehash(dentry);                          /* 加入主 dcache 哈希 */
       ...
   }
   ```
   即 `d_add` 会先 `__d_lookup_unhash` 清理 in_lookup_hash，再操作 d_alias，
   最后 `__d_rehash` 加入主 dcache 哈希。这是 `->lookup` 中实例化 dentry 的
   正确接口（参考 `ceph_lookup` → `ceph_finish_lookup` → `d_splice_alias` →
   `__d_add`）。

**为什么之前的"诊断 workaround"是错的（让内核将就 bug）**：

之前的修复在 `d_instantiate` 前加了检查：
```c
if (d_inode(dentry) || !hlist_unhashed(&dentry->d_u.d_alias)) {
    pr_err(...);
    iput(inode);
    return NULL;   /* ← 错误：dentry 仍留在 PAR_LOOKUP 未解析 */
}
d_instantiate(dentry, inode);
```
这违背了用户原则"是哪里有Bug就修改哪里，而不是让内核将就bug"：
- 它检测到症状（d_alias hashed）却返回 NULL，把 dentry 留在
  `DCACHE_PAR_LOOKUP` 悬挂状态，污染 VFS dcache。
- 真正的 Bug 是用错了 API（`d_instantiate` vs `d_add`），应直接替换。

**正确修复**：将 net 路径和 legacy comm 路径的 `d_instantiate(dentry, inode)`
全部替换为 `d_add(dentry, inode)`，并删除 early-return 和诊断 workaround。

#### 问题 6: looked-up inode 操作表为 NULL（已修复）

**现象**: 通过 lookup（非本地 mknod）得到的 inode 缺少 `i_op`/`i_fop`/`a_ops`，
后续 `open`/`read` 等操作触发空指针。

**根因**: `powerfs_lookup` 的 I_NEW 分支只内联设置了 `i_mode`/`i_uid`/`i_size`
等属性，未调用 `powerfs_init_inode()` 来设置操作表（`i_op`/`i_fop`/`a_ops`
按 `S_IFREG`/`S_IFDIR`/`S_IFLNK` 分发）。而 `powerfs_new_inode`（mknod 路径）
是调用 `powerfs_init_inode` 的，两条路径不一致。

**修复**: lookup 的 I_NEW 分支先调用 `powerfs_init_inode(inode, mode, dir->i_ino,
dentry->d_name.name)`（设置操作表 + 默认属性），再用 Filer 返回的权威属性覆盖
`i_mode`/`i_uid`/`i_gid`/`i_size`/`nlink`/时间戳。

### 代码变更摘要（Step 1）

**powerfs_fs.c** (`powerfs_lookup`):
- 删除错误的 early-return `if (d_inode(dentry)) return NULL;`（VFS 契约保证
  `->lookup` 收到的是新鲜 PAR_LOOKUP dentry，此分支是死代码且返回 NULL 会
  破坏 VFS 状态）
- 删除诊断 workaround（`d_inode`/`hlist_unhashed` 检查 + `iput` + `return NULL`）
- net 路径 + legacy comm 路径：`d_instantiate(dentry, inode)` → `d_add(dentry, inode)`
- net 路径 + legacy comm 路径：I_NEW 分支增加 `powerfs_init_inode()` 调用

#### 问题 7: readdir 路径 socket use-after-free（已修复）

**现象**: `ls -la /mnt/pfs` 触发内核 oops，栈指向 `_raw_spin_lock_irqsave`
空指针解引用。

**根因**: `powerfs_net_send_request` 直接使用全局 `g_conn.sock`，无引用计数
保护。并发 `powerfs_net_disconnect` 释放 socket 期间，send_request 仍在
`kernel_recvmsg` 使用已释放 socket → use-after-free。

**定位方法**: 聚焦出错的函数与数据结构。send_request 持有裸 `g_conn.sock`
指针跨越阻塞调用（`kernel_recvmsg`），与 disconnect 的 `sock_release` 无同步，
是典型的并发释放竞态。

**修复**（参考 Ceph 内核客户端 socket 引用计数模式）:
- `struct powerfs_net_conn` 增加 `atomic_t sock_users` 引用计数和
  `wait_queue_head_t sock_user_wq` 等待队列
- `powerfs_net_send_request`: 取本地 sock 引用，`atomic_inc(&sock_users)`
  后使用本地指针，完毕 `powerfs_net_put_sock_ref()` 释放
- `powerfs_net_disconnect`: 先置 `g_conn.sock=NULL` 退役（新请求看到 NULL
  返回 -ENOTCONN），再 `wait_event(sock_user_wq, sock_users==0)` 等待所有
  in-flight send_request 释放引用，最后 `powerfs_net_close_socket(sock)`
- `powerfs_net_put_sock_ref`: `atomic_dec_and_test` 归零时 `wake_up` 唤醒
  等待的 disconnect

#### 问题 8: write_end 调用 folio_mark_dirty 触发 NULL instruction fetch（已修复）

**现象**: 文件写入触发内核 oops，`powerfs_write_end+0x45` 处 NULL instruction
fetch（调用空函数指针）。

**误诊过程**: 一开始怀疑运行内核未导出 `set_page_dirty` 符号，改用
`folio_mark_dirty`，但 oops 依旧。这是"怀疑系统符号定位"的错误方向。

**正确根因**（遵循"模块接口使用错误优先"原则）: 查阅
`mm/page-writeback.c` 的 `folio_mark_dirty` 实现:
```c
bool folio_mark_dirty(struct folio *folio)
{
    struct address_space *mapping = folio_mapping(folio);
    if (likely(mapping)) {
        ...
        return mapping->a_ops->dirty_folio(mapping, folio);  /* 间接调用 */
    }
    return noop_dirty_folio(mapping, folio);
}
```
文件页 `mapping` 非空时，通过 `a_ops->dirty_folio` 间接调用。而我们的
`powerfs_aops` **未设置 `.dirty_folio`** → NULL 间接调用 → oops。

对照 `fs/ceph/addr.c` 的 `ceph_aops` 完整设置了 `.dirty_folio = ceph_dirty_folio`
（内部最终调用 `filemap_dirty_folio`）。所有不使用 buffer_heads 的网络文件系统
（nfs/btrfs/hostfs/vboxsf/zonefs）均设置 `.dirty_folio = filemap_dirty_folio`。

**修复**: `powerfs_aops` 增加 `.dirty_folio = filemap_dirty_folio`（与 nfs/btrfs
一致，`filemap_dirty_folio` 已 `EXPORT_SYMBOL` 且声明在 `<linux/writeback.h>`）。

**教训**:
1. 系统/内核在其他模块正常，问题大概率在**本模块的接口使用不完整或数据错误**
   ——应优先核对 VFS 回调表是否补齐、参数是否合法，而非怀疑系统符号
2. 遇到 NULL 间接调用，应查清调用链（`a_ops->xxx`）所需回调是否在 ops 表中
   补齐，参考 `fs/ceph`、`fs/nfs` 等成熟实现的完整 ops 表
3. dump 数据结构（如 ops 表指针）可快速定位哪个回调为 NULL

### Step 1 QEMU 回归验证结果（两 oops 修复后）

环境: 内核 6.2.0 + powerfs.ko + QEMU(KVM) + 后端 filer-1(172.30.0.35:9334,
leader 重定向至 filer-2 172.30.0.36:9334)

| 步骤 | 操作 | 结果 |
|------|------|------|
| 1 | `mount -t powerfs none /mnt/pfs` | OK |
| 2 | `ls -la /mnt/pfs`（readdir，验证 oops1） | OK，列出 io500-data/io500-results |
| 3 | `echo "hello powerfs kernel fs" > /mnt/pfs/test.txt`（验证 oops2） | OK，无 oops |
| 4 | `cat /mnt/pfs/test.txt` | OK，回读 "hello powerfs kernel fs" |
| 5 | `dmesg \| grep -iE "oops\|bug\|panic\|null pointer\|segfault"` | 干净（仅启动 PCI 提示） |

**结论**: oops1（socket use-after-free）和 oops2（dirty_folio NULL deref）
均彻底修复，读写路径无内核崩溃。

**已知后续项**: umount+remount 后 test.txt 丢失——异步 writepath 仅 mark_dirty，
writepage→后端刷盘尚未完整实现，属 Lease 刷盘阶段范畴（非本次 oops 修复的回归）。

### 顺带修复: QEMU VM eth1 未配置导致 SSH banner exchange 超时

**现象**: `ssh -p 2223 root@localhost` 报 "Connection timed out during banner
exchange"。QEMU 双网卡模式 eth1（user-net + hostfwd）在 init 脚本中未配置 IP，
host:2223 转发的包无法到达 guest sshd。

**修复**: `build_initramfs.sh` init 脚本增加 eth1 配置
`ip addr add 10.0.2.15/24 dev eth1`（QEMU user-net 默认网段）。

