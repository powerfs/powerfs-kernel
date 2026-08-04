# PowerFS 内核通信层断连恢复与请求队列方案

## 一、问题分析

### 1.1 当前缺陷

| 问题 | 位置 | 后果 |
|------|------|------|
| 断连时请求静默丢失 | `write_inode` 返回 0, `mknod` 本地分配 ino | size/ino 不一致, remount 后数据丢失 |
| 无请求队列 | `reconnect_work` 只重连 TCP | 断连窗口内的写操作全部蒸发 |
| 重连计数不归零 | `reconnect_work` 成功后直接 return | 下次断连时计数从非零开始, 误判 |
| 数据结构先于请求修改 | `write_inode` 先 `i_size_write` 再 `setattr` | 请求失败时本地已改, 不可回滚 |
| 读请求无降级策略 | 断连时 lookup 走本地缓存 | 可能返回过期/已删除文件 |

### 1.2 不一致风险

内核态数据不一致极易触发 OOPS:
- 本地 ino 与 Filer ino 不一致 → remount 后 lookup 返回错误 inode → UAF
- 本地 size 与 Filer size 不一致 → read 越界 → page fault
- 本地目录项与 Filer 不一致 → readdir 缺失/多余 → ENOENT/EEXIST

## 二、设计目标

1. **写请求不丢失**: 断连期间所有写请求入队列, 重连后自动发送
2. **读请求等待重连**: 不降级到本地缓存 (page cache 命中除外, 见 §3.3)
3. **有限重试**: 重连 3 次都失败才返回 `-ENOTCONN`
4. **计数归零**: 重连成功后 `reconnect_count` 归零, 不影响下次断连
5. **先请求后改结构**: 写操作必须等请求成功回调后才修改本地数据结构

## 三、架构设计

### 3.1 分层结构

```
┌──────────────────────────────────────────────────┐
│  VFS 层 (powerfs_fs.c)                            │
│  - 写操作: 调用 net 层, 等待成功回调才改数据结构    │
│  - read: page cache 命中直接返回, miss 走 net 层   │
├──────────────────────────────────────────────────┤
│  请求队列层 (powerfs_net_queue.c)  [新增]          │
│  - 所有请求入队列                                  │
│  - 断连时排队等待, 重连后自动 flush                 │
│  - 重连 3 次失败 → 唤醒所有等待者返回 -ENOTCONN     │
├──────────────────────────────────────────────────┤
│  通信层 (powerfs_net.c)                            │
│  - send_request: 已连接直接发送, 断连返回 -ENOTCONN │
│  - reconnect_work: 重连成功后通知队列层 flush       │
│  - reconnect_count: 成功后归零                      │
└──────────────────────────────────────────────────┘
```

### 3.2 请求处理流程

```
send_request(msg_type, req, resp):
    │
    ├─ 已连接?
    │   ├─ YES → 直接发送
    │   │         ├─ 成功 → 返回 0, 填充 resp
    │   │         └─ 失败 → 触发 disconnect → 入队列
    │   │
    │   └─ NO → 入队列等待
    │
    ▼
┌─────────────────────────────┐
│  请求队列 (pending_list)     │
│  - 入队, 等待 completion     │
│  - 阻塞等待直到:              │
│    a) 重连成功 → flush 发送   │
│    b) 重连 3 次失败 → -ENOTCONN│
└─────────────────────────────┘
```

### 3.3 read 的特殊处理

read 操作分两层:
- **VFS 层 `file_read`**: 先查 page cache
  - 命中 (含脏页) → 直接返回, 不走网络 ← **允许读到未持久化数据**
  - miss → 调用 `powerfs_net_read` 从 Filer 获取
- **net 层 `powerfs_net_read`**: 断连时入队列等待重连 (不降级)

这样断连期间:
- 已写过的文件 (page cache 有脏页) 仍可读
- 未读过的文件 (page cache miss) 会等待重连

### 3.4 重连计数策略

```c
/* 重连成功后归零, 失败累加 */
reconnect_work:
    for (i = 0; i < MAX_RECONNECT_RETRIES; i++) {  /* MAX = 3 */
        msleep(RECONNECT_DELAY);
        ret = powerfs_net_connect(addr, port);
        if (ret == 0) {
            g_conn.reconnect_count = 0;           /* 归零 */
            wake_up(&queue.wq);                   /* 通知队列 flush */
            return;
        }
        g_conn.reconnect_count++;
    }
    /* 3 次都失败 */
    g_conn.state = STATE_ERROR;
    wake_up_all(&queue.wq);  /* 唤醒所有等待者, 返回 -ENOTCONN */
```

## 四、数据结构

### 4.1 请求队列

```c
/* powerfs_net_queue.h */

#define POWERFS_NET_MAX_RECONNECT_RETRIES  3
#define POWERFS_NET_RECONNECT_DELAY_MS     2000

/**
 * 单个排队请求
 */
struct powerfs_net_req {
    struct list_head    list;
    __u16               msg_type;
    const void         *req_data;      /* 请求 TLV body */
    size_t              req_len;
    void               *resp_buf;      /* 响应缓冲区 (调用者提供) */
    size_t              resp_buf_size;
    size_t             *resp_len;      /* 实际响应长度 */
    int                 status;        /* 0=pending, >0=done, <0=error */
    struct completion   done;
};

/**
 * 请求队列
 */
struct powerfs_net_queue {
    struct list_head    pending;       /* 待发送队列 */
    spinlock_t          lock;          /* 保护 pending list */
    wait_queue_head_t   sender_wq;     /* 唤醒 flush 线程 */
    bool                flushing;      /* 正在 flush */
};
```

### 4.2 连接状态扩展

```c
/* powerfs_net.c 中 g_conn 扩展 */

struct powerfs_net_conn {
    /* ... 现有字段 ... */

    /* 请求队列 */
    struct powerfs_net_queue  queue;

    /* 重连状态 */
    atomic_t   reconnect_count;        /* 当前已重连次数, 成功后归零 */
    atomic_t   reconnect_failed;       /* 3 次都失败的标志 */
};
```

## 五、关键代码路径修改

### 5.1 send_request 改造

```c
int powerfs_net_send_request(__u16 msg_type,
                              const void *req_data, size_t req_len,
                              void *resp_buf, size_t resp_buf_size,
                              size_t *resp_len)
{
    /* 1. 检查是否已达到最大重连失败 */
    if (atomic_read(&g_conn.reconnect_failed))
        return -ENOTCONN;

    /* 2. 已连接 → 直接发送 */
    if (powerfs_net_is_connected()) {
        int ret = powerfs_net_do_send(msg_type, req_data, req_len,
                                       resp_buf, resp_buf_size, resp_len);
        if (ret == 0)
            return 0;

        /* 发送失败 → 触发断连, 走队列 */
        powerfs_net_handle_disconnect();
    }

    /* 3. 未连接 → 入队列等待 */
    return powerfs_net_queue_request(msg_type, req_data, req_len,
                                      resp_buf, resp_buf_size, resp_len);
}
```

### 5.2 队列等待与唤醒

```c
int powerfs_net_queue_request(__u16 msg_type, ...)
{
    struct powerfs_net_req req;
    int ret;

    /* 初始化请求 */
    INIT_LIST_HEAD(&req.list);
    req.msg_type = msg_type;
    req.req_data = req_data;
    req.req_len = req_len;
    req.resp_buf = resp_buf;
    req.resp_buf_size = resp_buf_size;
    req.resp_len = resp_len;
    req.status = 0;
    init_completion(&req.done);

    /* 入队 */
    spin_lock(&g_conn.queue.lock);
    list_add_tail(&req.list, &g_conn.queue.pending);
    spin_unlock(&g_conn.queue.lock);

    /* 等待结果 */
    wait_for_completion(&req.done);

    /* 从队列移除 (flush 线程可能已移除, 这里安全) */
    spin_lock(&g_conn.queue.lock);
    list_del(&req.list);
    spin_unlock(&g_conn.queue.lock);

    return req.status;
}

/* 重连成功后调用 */
void powerfs_net_flush_queue(void)
{
    struct powerfs_net_req *req, *tmp;
    LIST_HEAD(to_send);

    /* 取出所有待发送请求 */
    spin_lock(&g_conn.queue.lock);
    list_splice_init(&g_conn.queue.pending, &to_send);
    spin_unlock(&g_conn.queue.lock);

    /* 逐个发送 (串行, 因为单连接) */
    list_for_each_entry_safe(req, tmp, &to_send, list) {
        int ret = powerfs_net_do_send(req->msg_type,
                                       req->req_data, req->req_len,
                                       req->resp_buf, req->resp_buf_size,
                                       req->resp_len);
        req->status = ret;
        complete(&req->done);
    }
}

/* 重连 3 次都失败后调用 */
void powerfs_net_fail_queue(int err)
{
    struct powerfs_net_req *req, *tmp;

    spin_lock(&g_conn.queue.lock);
    list_for_each_entry_safe(req, tmp, &g_conn.queue.pending, list) {
        req->status = err;
        complete(&req->done);
    }
    INIT_LIST_HEAD(&g_conn.queue.pending);  /* 清空 */
    spin_unlock(&g_conn.queue.lock);
}
```

### 5.3 reconnect_work 修改

```c
static void powerfs_net_reconnect_work(struct work_struct *work)
{
    int i;

    for (i = 0; i < POWERFS_NET_MAX_RECONNECT_RETRIES; i++) {
        if (atomic_read(&g_conn.stopping))
            return;

        msleep_interruptible(POWERFS_NET_RECONNECT_DELAY_MS);

        ret = powerfs_net_connect(addr, port);
        if (ret == 0) {
            /* 重连成功: 归零计数, flush 队列 */
            atomic_set(&g_conn.reconnect_count, 0);
            atomic_set(&g_conn.reconnect_failed, 0);
            powerfs_net_flush_queue();
            return;
        }

        atomic_inc(&g_conn.reconnect_count);
    }

    /* 3 次都失败: 唤醒所有等待者 */
    pr_err("powerfs: reconnect failed after %d attempts\n",
           POWERFS_NET_MAX_RECONNECT_RETRIES);
    atomic_set(&g_conn.reconnect_failed, 1);
    powerfs_net_fail_queue(-ENOTCONN);
}
```

### 5.4 VFS 层数据结构操作顺序修正

```c
/* powerfs_write_inode: 先请求再改本地 */
static int powerfs_write_inode(struct inode *inode,
                                struct writeback_control *wbc)
{
    loff_t i_size = i_size_read(inode);

    /* 不再检查 is_connected, 让队列处理等待 */
    ret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                               0, 0, 0, (__u64)i_size);
    if (ret < 0) {
        /* 队列等待也失败 (3 次重连失败) */
        pr_err("powerfs: write_inode setattr failed: %d\n", ret);
        return ret;  /* 传播错误, writeback 会重试 */
    }

    /* 请求成功后才更新本地记录 */
    spin_lock(&pi->i_lock);
    pi->content_size = (u64)i_size;
    spin_unlock(&pi->i_lock);

    return 0;
}

/* powerfs_mknod: 移除断连时的本地 fallback */
static int powerfs_mknod(...)
{
    /* 不再 if (powerfs_net_is_connected()), 直接走 net 层 */
    /* net 层内部处理断连等待 */
    int rerr = powerfs_net_create(dir->i_ino, ...);
    if (rerr)
        return rerr;  /* -ENOTCONN 或其他错误 */

    /* 请求成功后才创建本地 inode */
    inode = powerfs_new_inode(...);
    d_add(dentry, inode);
    ...
}
```

## 六、请求分类处理

| 请求类型 | 断连行为 | page cache | 说明 |
|----------|----------|------------|------|
| CREATE/MKDIR | 入队列等待 | N/A | 元数据必须持久化 |
| UNLINK/RMDIR | 入队列等待 | N/A | 同上 |
| RENAME | 入队列等待 | N/A | 同上 |
| WRITE (数据) | 入队列等待 | 脏页保留 | page cache 有脏页, read 可读到 |
| SETATTR | 入队列等待 | N/A | size/mode 变更必须持久化 |
| LOOKUP | 入队列等待 | N/A | 不降级到本地缓存, 避免过期 |
| READDIR | 入队列等待 | N/A | 同上 |
| GETATTR | 入队列等待 | N/A | 同上 |
| READ | 入队列等待 | 命中可直接返回 | page cache miss 时走 net 层等待 |

## 七、与 leader 切换的协调

当前阶段 leader 切换概率应很小, 发生时需要排查原因。
但通信层仍需正确处理 leader redirect:

1. `send_request` 收到 REDIRECT 响应 → 切换 leader 地址 → 重试请求 (不消耗重连次数)
2. leader 不可达 → 视为断连 → 入队列等待重连
3. 重连时尝试当前已知 leader 地址, 失败则尝试其他 raft 节点

```c
/* redirect 不算重连失败, 只是换地址重试 */
if (status == STATUS_ERR_REDIRECT) {
    /* 解析新 leader 地址, 更新 g_conn */
    powerfs_net_switch_leader(new_addr, new_port);
    /* 重试发送 (不计入 reconnect_count) */
    goto retry_send;
}
```

## 八、测试计划

### 8.1 基本功能测试

| 测试 | 预期 |
|------|------|
| 正常 write → read | 数据一致 |
| 正常 create → remount → lookup | 文件存在 |
| 断连期间 write → 重连 → read | 数据一致 |
| 断连期间 create → 重连 → remount → lookup | 文件存在 |

### 8.2 断连恢复测试

| 测试 | 预期 |
|------|------|
| kill filer → write (应阻塞) → 重启 filer → write 返回 | 数据一致 |
| kill filer → write → 3 次重连失败 → write 返回 -ENOTCONN | 上层报错 |
| kill filer → write → 重连成功 → write 返回 | reconnect_count 归零 |
| 连续断连 2 次 | 第二次重连计数从 0 开始 |

### 8.3 leader 切换测试

| 测试 | 预期 |
|------|------|
| 重启 leader filer → write | redirect 到新 leader, 数据一致 |
| 所有 filer 重启 → write → 等待选举 → write 返回 | 数据一致 |

### 8.4 一致性验证

| 测试 | 预期 |
|------|------|
| 断连期间 write page cache → read | 读到 page cache 脏页 (允许) |
| 断连期间 write → 重连 → remount → read | 数据一致 |
| 并发 write + 断连 → 重连 | 串行化, 无丢失 |

## 九、状态机设计

### 9.1 问题分析：当前实现的缺陷

当前状态管理分散在多层，用零散的标志位组合表达状态，导致逻辑难以理解和维护：

**内核侧（零散标志）：**
| 标志 | 类型 | 位置 | 含义 |
|------|------|------|------|
| `g_conn.state` | enum (6值) | powerfs_net.h:286 | TCP连接状态 |
| `g_pool.leader_known` | atomic_bool | powerfs_net.h:404 | leader是否已知 |
| `g_conn.reconnect_failed` | atomic_bool | powerfs_net.h:356 | 3次重连是否都失败 |
| `g_conn.reconnect_count` | int | powerfs_net.h:350 | 当前重连次数 |
| `g_conn.failover_count` | atomic_t | powerfs_net.h:357 | failover失败次数 |

**Filer侧（ad-hoc元组）：**
| 字段 | 类型 | 含义 |
|------|------|------|
| `leader_state` | AtomicBool | 本节点是否为leader |
| `leader_address` | RwLock\<String\> | leader的gRPC地址（空串=未知）|
| `get_shard_leader_status()` | Option\<(bool, String)\> | 组合返回，调用者需pattern match |

**条件判断式的问题：**
```rust
// Filer check_leader — 当前实现，依赖条件分支而非状态
match get_shard_leader_status(shard_id).await {
    Some((true, _)) => Ok(()),                         // 是leader
    Some((false, addr)) if !addr.is_empty() => redirect(addr), // follower，知道leader
    _ => server_error(),                               // 其他（选举中？不可用？分不清）
}
```
```c
/* 内核 monitor_work — 零散标志组合判断 */
if (!atomic_read(&g_pool.leader_known)) {
    if (atomic_read(&g_conn.reconnect_failed)) {
        /* 重置标志，重新调度 */
    }
    goto reschedule;
}
ret = powerfs_net_leader_ping();
if (ret < 0) powerfs_net_failover();
```

**核心问题：** "leader_known=false + reconnect_failed=false" 和 "leader_known=false + reconnect_failed=true" 是两种完全不同的情况，但靠两个独立标志的组合来表达，容易遗漏边界条件。

### 9.2 双层状态机设计

将状态管理拆分为两个正交层次：**Filer连接状态**（网络层）和 **MetaShard状态**（元数据可用性层）。

#### 9.2.1 Layer 1: Filer 连接状态（内核侧，网络层）

管理TCP连接生命周期。回答："能否到达某个filer？"

```
┌─────────────┐  connect()   ┌──────────────┐  success   ┌───────────┐
│ DISCONNECTED│─────────────→│ RECONNECTING │───────────→│ CONNECTED │
│             │              │ (1-3次重试)   │            │           │
└─────────────┘              └──────────────┘            └───────────┘
       ↑                            │                          │
       │                      3x fail                   network error
       │                            │                          │
       │                            ▼                          │
       │                      ┌─────────┐                     │
       │                      │  FAULT  │←────────────────────┘
       │                      │(仅umount│
       │                      │ 可恢复) │
       │                      └─────────┘
       │                            │
       └──────umount+remount────────┘
```

| 状态 | 含义 | 进入条件 | 退出条件 |
|------|------|----------|----------|
| `DISCONNECTED` | 无TCP连接，等待重连 | 初始状态 / 网络断开 | reconnect_work启动 |
| `RECONNECTING` | 正在尝试重连（最多3次） | reconnect_work开始执行 | 连接成功→CONNECTED / 3次失败→FAULT |
| `CONNECTED` | TCP连接已建立，握手完成 | connect成功 | 网络错误→DISCONNECTED / ping失败→DISCONNECTED |
| `FAULT` | 3次重连均失败，不可自动恢复 | reconnect 3次失败 | umount+remount→DISCONNECTED |

#### 9.2.2 Layer 2: MetaShard 状态（两侧，元数据可用性层）

管理元数据操作的可用性。回答："能否服务元数据请求？"

**关键洞察：TCP连接 ≠ 元数据可用。** Filer可能可达（TCP通）但无法服务请求（Raft选举中、不是leader、无多数派）。

```
                         Raft选举开始
                    ┌─────────────────────────┐
                    │                         │
                    ▼                         │
              ┌──────────┐  选举完成(本节点赢) ┌─────────────────┐
              │  NORMAL  │←───────────────────│ WAITING_ELECTION │
              │ (是leader)│                    │ (leader未知)     │
              └──────────┘  选举完成(本节点赢) └─────────────────┘
                    │                              │
                    │ leader变更                      │ 选举完成(输)
                    │ (另一节点成为leader)             │
                    ▼                              │
              ┌──────────┐                         │
              │ FOLLOWER │←────────────────────────┘
              │(知道leader│
              │  地址)    │
              └──────────┘
                    │
                    │ 无多数派 / 存储错误
                    │ / leader不可达且无其他候选
                    ▼
              ┌───────────┐
              │UNAVAILABLE │
              │(不可自动恢复)│
              └───────────┘
```

| 状态 | 含义 | 请求行为 | Filer侧进入条件 |
|------|------|----------|----------------|
| `NORMAL` | 本节点是Raft leader，可服务请求 | 直接处理 | `StateRole::Leader` |
| `FOLLOWER` | 不是leader，但知道leader地址 | 返回REDIRECT到leader | `StateRole::Follower` + `leader_id != 0` |
| `WAITING_ELECTION` | Raft选举进行中，leader未知 | 返回REDIRECT到自身(-EAGAIN) | `StateRole::Candidate` / `leader_id == 0` |
| `UNAVAILABLE` | 不可自动恢复（无多数派等） | 返回SERVER_ERROR(-EREMOTEIO) | 存储错误 / 长时间无leader |

#### 9.2.3 两层正交关系

连接状态和MetaShard状态是正交的，组合决定请求行为：

```
                    MetaShard状态
                ┌────────┬────────┬──────────┬───────────┐
                │ NORMAL │FOLLOWER│WAITING   │UNAVAILABLE│
    ┌───────────┼────────┼────────┼ELECTION  ├───────────┤
    │CONNECTED  │ 直接   │ REDIRECT│REDIRECT  │ -ENOTCONN  │
连  │           │ 发送   │ 到leader│到自身    │            │
接  ├───────────┼────────┼────────┼─────────┼───────────┤
状  │DISCONNECTED│  入队列等待重连  │  入队列  │ -ENOTCONN  │
态  │RECONNECTING│                 │  等待    │            │
    ├───────────┼──────────────────┼─────────┼───────────┤
    │FAULT      │     -ENOTCONN    │ -ENOTCONN│ -ENOTCONN  │
    └───────────┴──────────────────┴─────────┴───────────┘
```

**简化规则：** MetaShard状态仅在CONNECTED时有意义；DISCONNECTED/RECONNECTING时统一入队列等待；FAULT时统一拒绝。

### 9.3 Filer侧实现

#### 9.3.1 MetaShardState 枚举

```rust
// powerfs-filer/src/raft_group_manager.rs

/// MetaShard 元数据可用性状态
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MetaShardState {
    /// 本节点是Raft leader，可正常服务元数据请求
    Normal,
    /// 不是leader，但知道leader地址，需重定向
    Follower { leader_addr: String },
    /// Raft选举进行中，leader未知，返回REDIRECT到自身让内核重试
    WaitingElection,
    /// 不可用（无多数派/存储错误），返回永久错误
    Unavailable,
}
```

#### 9.3.2 状态推导

```rust
impl RaftGroupManager {
    /// 根据Raft状态推导MetaShardState
    pub async fn get_shard_state(&self, shard_id: ShardId) -> MetaShardState {
        let arcs = self.shard_status_arcs.read().await;
        let Some(handle) = arcs.get(&shard_id) else {
            return MetaShardState::Unavailable;
        };

        let is_leader = handle.leader_state.load(Ordering::SeqCst);
        let leader_addr = handle.leader_address.read().unwrap().clone();

        if is_leader {
            MetaShardState::Normal
        } else if !leader_addr.is_empty() {
            MetaShardState::Follower { leader_addr }
        } else {
            // 不是leader且leader地址未知 → 选举进行中
            MetaShardState::WaitingElection
        }
    }
}
```

#### 9.3.3 状态驱动的 check_leader

```rust
// powerfs-filer/src/net_handler.rs

async fn check_leader(&self, msg: &NetMessage, shard_id: ShardId) -> Result<(), NetMessage> {
    let state = self.meta_shard_manager.get_shard_state(shard_id).await;
    match state {
        MetaShardState::Normal => Ok(()),

        MetaShardState::Follower { leader_addr } => {
            let net_addr = Self::grpc_addr_to_net_addr(&leader_addr, self.net_port);
            warn!("shard {}: FOLLOWER, redirect to {}", shard_id.0, net_addr);
            Err(Self::build_redirect(msg, &net_addr))
        }

        MetaShardState::WaitingElection => {
            // REDIRECT到自身 → 内核-EAGAIN → VFS重试
            // 选举完成后下一次请求成功或REDIRECT到真正的leader
            let self_addr = Self::grpc_addr_to_net_addr(
                &self.meta_shard_manager.get_node_grpc_address(),
                self.net_port,
            );
            warn!("shard {}: WAITING_ELECTION, redirect to self {}", shard_id.0, self_addr);
            Err(Self::build_redirect(msg, &self_addr))
        }

        MetaShardState::Unavailable => {
            error!("shard {}: UNAVAILABLE, returning server error", shard_id.0);
            Err(Self::build_response(msg, STATUS_ERR_SERVER_ERROR, Vec::new()))
        }
    }
}
```

### 9.4 内核侧实现

#### 9.4.1 合并状态枚举

```c
// powerfs_net.h

/* 合并后的元数据请求状态 (连接状态 + leader状态的综合) */
enum powerfs_meta_state {
    META_STATE_NORMAL = 0,          /* 已连接 + leader已知 */
    META_STATE_RECONNECTING,        /* 断连中，请求入队列等待 */
    META_STATE_WAITING_ELECTION,    /* 已连接但leader未知，-EAGAIN重试 */
    META_STATE_UNAVAILABLE,         /* 不可恢复，-ENOTCONN */
};
```

#### 9.4.2 状态推导函数

```c
// powerfs_net.c

static enum powerfs_meta_state powerfs_net_get_meta_state(void)
{
    /* FAULT: 3次重连都失败 */
    if (atomic_read(&g_conn.reconnect_failed))
        return META_STATE_UNAVAILABLE;

    /* 未连接: 入队列等待重连 */
    if (g_conn.state != POWERFS_NET_STATE_CONNECTED)
        return META_STATE_RECONNECTING;

    /* 已连接但leader未知: 选举中 */
    if (!atomic_read(&g_pool.leader_known))
        return META_STATE_WAITING_ELECTION;

    /* 正常态 */
    return META_STATE_NORMAL;
}
```

#### 9.4.3 状态驱动的 send_request

```c
int powerfs_net_send_request(...)
{
    enum powerfs_meta_state state = powerfs_net_get_meta_state();

    switch (state) {
    case META_STATE_NORMAL:
        /* 直接发送，处理REDIRECT（FOLLOWER场景） */
        return do_send_with_redirect_retry(...);

    case META_STATE_RECONNECTING:
        /* 入队列等待重连，重连后重新检查状态 */
        return queue_and_wait_for_reconnect(...);
        /* 重连成功后可能变为 NORMAL 或 WAITING_ELECTION */

    case META_STATE_WAITING_ELECTION:
        /* 发送请求，期望收到REDIRECT到自身 → -EAGAIN
         * VFS层重试，自然backoff（disconnect+reconnect延迟）
         * 选举完成后下一次请求成功 */
        return do_send_with_eagain(...);

    case META_STATE_UNAVAILABLE:
        return -ENOTCONN;
    }
}
```

#### 9.4.4 状态驱动的 monitor_work

```c
static void powerfs_net_monitor_work_func(struct work_struct *work)
{
    enum powerfs_meta_state state = powerfs_net_get_meta_state();

    switch (state) {
    case META_STATE_NORMAL:
        /* 正常态：ping leader，失败则触发failover */
        if (powerfs_net_leader_ping() < 0)
            powerfs_net_failover();
        break;

    case META_STATE_RECONNECTING:
        /* 重连中：检查reconnect_failed，若失败则重新调度reconnect_work */
        if (atomic_read(&g_conn.reconnect_failed)) {
            atomic_set(&g_conn.reconnect_failed, 0);
            if (!atomic_read(&g_conn.stopping))
                schedule_work(&g_conn.reconnect_work);
        }
        /* reconnect_work运行中时什么都不做，等其完成 */
        break;

    case META_STATE_WAITING_ELECTION:
        /* 选举中：不ping（leader未知，ping无意义）
         * 等待Raft选举自然完成，下一个请求会感知到状态变化 */
        break;

    case META_STATE_UNAVAILABLE:
        /* 不可恢复：仅日志，等待umount */
        pr_warn_once("powerfs: meta state UNAVAILABLE, umount required\n");
        break;
    }

    /* 重新调度 */
    if (g_pool.monitoring)
        schedule_delayed_work(&g_pool.monitor_work, ...);
}
```

### 9.5 状态转换日志

每个状态转换都应记录日志，便于调试：

```
[powerfs] meta_state: NORMAL → RECONNECTING (reason: network error, sock_users=2)
[powerfs] meta_state: RECONNECTING → WAITING_ELECTION (reason: reconnect success but leader unknown)
[powerfs] meta_state: WAITING_ELECTION → NORMAL (reason: leader elected, redirect to self succeeded)
[powerfs] meta_state: NORMAL → FOLLOWER (reason: leader change detected via REDIRECT)
[powerfs] meta_state: RECONNECTING → UNAVAILABLE (reason: 3 reconnect attempts failed)
```

Filer侧（通过Raft状态变更回调）：
```
[filer] shard 0: Normal → WaitingElection (reason: raft state=Follower, leader_id=0)
[filer] shard 0: WaitingElection → Normal (reason: raft state=Leader, term=5)
[filer] shard 0: Normal → Follower { leader_addr=172.30.0.36:8889 } (reason: leader change)
```

### 9.6 与当前实现的对比

| 维度 | 当前实现 | 状态机设计 |
|------|----------|------------|
| **状态表达** | 零散标志位组合 (leader_known + reconnect_failed + state) | 枚举状态，单一变量 |
| **决策方式** | 条件分支 if/else 嵌套 | switch/case 状态分派 |
| **边界覆盖** | 容易遗漏（如 leader_known=false + reconnect_failed=true 需手动检查）| 穷举所有状态，编译器保证完整性 |
| **可读性** | 需追踪多个标志的交叉关系 | 状态转换图清晰表达 |
| **可测试性** | 难以模拟特定标志组合 | 按状态构造测试用例 |
| **日志** | 零散的 pr_warn/pr_info | 状态转换日志，完整审计链 |

### 9.7 迁移计划

迁移分两步，每步可独立验证：

**Step 1: Filer侧（已完成）**
- 添加 `MetaShardState` 枚举和 `get_shard_state()` 方法
- 将 `check_leader` 改为状态驱动（`WaitingElection` 返回 REDIRECT 到自身）
- 当前修改已实现此步的核心逻辑

**Step 2: 内核侧（待实现）**
- 添加 `enum powerfs_meta_state` 和 `powerfs_net_get_meta_state()` 函数
- 将 `send_request` 改为 switch 状态分派
- 将 `monitor_work` 改为 switch 状态分派
- 移除零散的标志检查，统一用状态枚举
- 可逐步迁移：先添加状态推导函数（不改行为），再逐步替换条件分支

### 9.8 测试矩阵

| 初始状态 | 操作 | 预期状态转换 | 预期行为 |
|----------|------|-------------|----------|
| NORMAL | write | 无 | 成功 |
| NORMAL | kill所有filer → write | →RECONNECTING | 阻塞等待 |
| RECONNECTING | 恢复多数派 | →NORMAL或→WAITING_ELECTION | write完成或-EAGAIN重试 |
| WAITING_ELECTION | 等待选举完成 | →NORMAL | 下一次请求成功 |
| NORMAL | kill leader filer | →RECONNECTING→NORMAL(redirect到新leader) | write经redirect完成 |
| RECONNECTING | 3次重连失败 | →UNAVAILABLE | 返回-ENOTCONN |
| UNAVAILABLE | umount+remount | →DISCONNECTED→...→NORMAL | 恢复 |

## 十、连接池架构重构（根本性设计修正）

### 10.1 问题根源

当前架构采用**单连接模型**：内核只维护一条到 Filer 的 TCP 连接（`g_conn.sock`），以及一条到 Volume 的连接。这导致：

1. **每次 REDIRECT 都断开重连**：Filer 返回 REDIRECT 到另一地址时，内核 disconnect 当前连接 → connect 新地址 → 重试。实际上目标 Filer 可能早已在已知列表中，完全可以复用已有连接。

2. **全局 `send_recv_mutex` 串行化**：所有请求共享一把互斥锁，一个请求阻塞（如等待重连）会阻塞所有请求，包括发往不同 Filer 的请求。

3. **一个 Filer 故障影响全局**：Filer-3 挂了，内核全局 disconnect → reconnect_work 重新遍历所有 Filer。实际上 Filer-1/Filer-2 的连接完全不受影响，不需要重连。

4. **REDIRECT-to-self hack**：为了处理选举中的重试，不得不在 `send_request` 内部加 `msleep` 循环。这是单连接模型的补丁，连接池模型下根本不需要。

### 10.2 目标架构：连接池 + MetaShard 路由

```
┌──────────────────────────────────────────────────────────┐
│                      VFS Layer                            │
│            create / write / read / unlink / readdir       │
├──────────────────────────────────────────────────────────┤
│                   MetaShard Router                        │
│                                                          │
│   shard_id = calculate_shard(ino)                        │
│   filer_idx = shard_leader_map[shard_id]                 │
│   conn = filer_pool[filer_idx]  ← 直接复用！              │
│                                                          │
│   REDIRECT → 更新 shard_leader_map → 用新 conn 重试       │
│             (不断开任何连接！)                             │
├────────────────────────┬─────────────────────────────────┤
│    Filer 连接池         │      Volume 连接池               │
│                        │                                 │
│  pool[0] → filer-1     │  pool[0] → vol-1 (172.30.0.21)  │
│   .35:9334 [CONNECTED] │   :8901  [CONNECTED]             │
│                        │                                 │
│  pool[1] → filer-2     │  pool[1] → vol-2 (172.30.0.22)  │
│   .36:9334 [CONNECTED] │   :8901  [CONNECTED]             │
│                        │                                 │
│  pool[2] → filer-3     │  pool[2] → vol-3 (172.30.0.23)  │
│   .37:9334 [RECONNECT] │   :8901  [CONNECTED]             │
│                        │                                 │
│  独立重连:              │  独立重连:                        │
│  filer-3挂 →只pool[2]  │  vol-2挂 →只pool[1]重连          │
│  重连, 其他不受影响     │  其他不受影响                     │
├────────────────────────┴─────────────────────────────────┤
│                   Master Discovery                       │
│   ListFilers → filer 列表 → 初始化 Filer 连接池           │
│   GetTopology → volume 列表 → 初始化 Volume 连接池        │
└──────────────────────────────────────────────────────────┘
```

### 10.3 核心数据结构

#### 10.3.1 连接池

```c
/* 单个连接（Filer 或 Volume 通用） */
struct powerfs_conn {
    /* 标识 */
    char addr[64];              /* IP 地址 */
    __u16 port;                 /* 端口 */
    u8 node_id;                 /* 节点编号 (filer_id 或 volume_id) */
    bool is_filer;              /* true=Filer连接, false=Volume连接 */

    /* TCP 连接 */
    struct socket *sock;        /* 当前 socket (NULL=未连接) */
    atomic_t sock_users;        /* 引用计数 (防止并发 close) */
    wait_queue_head_t sock_user_wq;  /* 等待 sock_users==0 */

    /* 状态 */
    enum {
        CONN_DISCONNECTED,      /* 未连接，等待重连 */
        CONN_CONNECTING,        /* 正在连接 */
        CONN_CONNECTED,         /* 已连接，可用 */
        CONN_RECONNECTING,      /* 重连中 (1-3次) */
        CONN_FAULT,             /* 重连失败，仅 umount 可恢复 */
    } state;
    spinlock_t state_lock;      /* 保护 state */

    /* 独立重连 */
    struct delayed_work reconnect_work;
    int reconnect_count;        /* 当前重连次数 (0-3) */
    wait_queue_head_t reconnect_wq;  /* 等待重连完成 */

    /* 独立互斥锁 (替代全局 send_recv_mutex) */
    struct mutex send_mutex;    /* 串行化同一连接的请求 */
};

/* 连接池 */
struct powerfs_conn_pool {
    struct powerfs_conn filers[MAX_FILERS];
    int filer_count;

    struct powerfs_conn volumes[MAX_VOLUMES];
    int volume_count;

    /* Filer 健康监控 */
    struct delayed_work health_monitor;
    bool monitoring;
};
```

#### 10.3.2 MetaShard 路由表

```c
/* shard → leader filer 映射 */
struct shard_route_table {
    /* shard_id → filer_pool 索引 */
    int shard_leader[MAX_SHARDS];
    spinlock_t lock;            /* 保护映射表 */

    /* 从 REDIRECT 响应学习 leader 变更 */
    /* 从 Filer Notify 接收 leader 变更通知 */
};

/* 获取 shard 对应的连接 */
static struct powerfs_conn *get_filer_conn_for_shard(
    struct shard_route_table *routes,
    struct powerfs_conn_pool *pool,
    u64 shard_id)
{
    int filer_idx;

    spin_lock(&routes->lock);
    filer_idx = routes->shard_leader[shard_id];
    spin_unlock(&routes->lock);

    if (filer_idx < 0 || filer_idx >= pool->filer_count)
        return NULL;

    return &pool->filers[filer_idx];
}
```

### 10.4 请求路由流程

```c
/**
 * send_request - 通过连接池发送请求
 *
 * 核心改变:
 * 1. 不再断开/重连任何连接
 * 2. REDIRECT 只更新路由表，用已有连接重试
 * 3. 每个 filer 有独立 mutex，不阻塞其他 filer 的请求
 */
int powerfs_net_send_request(u64 shard_id, int msg_type,
                              const void *body, size_t body_len,
                              const void *data, size_t data_len,
                              /* response params ... */)
{
    struct powerfs_conn *conn;
    int filer_idx;
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        /* 1. 从路由表获取 shard 对应的 filer 连接 */
        conn = get_filer_conn_for_shard(&routes, &pool, shard_id);
        if (!conn || conn->state != CONN_CONNECTED) {
            /* 该 filer 未连接，等待其独立重连 */
            wait_event_timeout(conn->reconnect_wq,
                conn->state == CONN_CONNECTED ||
                conn->state == CONN_FAULT,
                msecs_to_jiffies(30000));

            if (conn->state == CONN_FAULT)
                return -ENOTCONN;
            if (conn->state != CONN_CONNECTED)
                return -ENOTCONN;
        }

        /* 2. 通过该 filer 的独立 mutex 串行化发送 */
        mutex_lock(&conn->send_mutex);

        ret = do_send_recv(conn, msg_type, body, data, ...);

        mutex_unlock(&conn->send_mutex);

        /* 3. 处理 REDIRECT: 更新路由表，用新连接重试 */
        if (ret == STATUS_REDIRECT && attempt == 0) {
            parse_redirect_addr(resp, new_addr, &new_port);

            /* 找到目标 filer 在连接池中的索引 */
            new_filer_idx = find_filer_in_pool(&pool, new_addr, new_port);
            if (new_filer_idx < 0) {
                pr_warn("redirect to unknown filer %s:%u\n", new_addr, new_port);
                return -EAGAIN;
            }

            /* 更新 shard 路由表 */
            spin_lock(&routes->lock);
            routes->shard_leader[shard_id] = new_filer_idx;
            spin_unlock(&routes->lock);

            pr_info("shard %llu: leader changed to filer %d (%s:%u)\n",
                    shard_id, new_filer_idx, new_addr, new_port);

            /* 重试: 下次循环会用新 filer 的连接 (已连接！) */
            continue;
        }

        /* 4. REDIRECT 到自身 (选举中): 该 filer 仍连着，
         *    只是 leader 未知。短暂等待后重试同一连接。
         *    不需要 disconnect/reconnect！ */
        if (ret == STATUS_REDIRECT && attempt == 1) {
            /* 第二次仍然是 REDIRECT，可能是选举中。
             * 返回 -EAGAIN 让 VFS 重试，或内部等待。
             * 由于不断开连接，重试成本低。 */
            msleep(500);
            attempt--;  /* 保持 attempt=1, 再试一次 */
            /* 但需要限制重试次数避免死循环 */
            if (++self_redirect_count > 30) {
                pr_warn("election timeout after 15s\n");
                return -EAGAIN;
            }
            continue;
        }

        return ret;
    }

    return ret;
}
```

### 10.5 事件驱动状态传播

**核心原则：** 连接层状态变化通过事件通知传播到 MetaShard 路由层，路由层根据事件变更 shard 状态，请求路由根据 shard 状态决定动作。不依赖轮询，不依赖条件判断。

#### 10.5.1 双层状态机联动

```
┌─────────────────────────────────────────────────────────┐
│  连接层状态机 (per-conn)                                  │
│                                                         │
│  CONNECTED ──→ 断连 ──→ RECONNECTING ──→ 成功 ──→ CONNECTED│
│                                  ↓                      │
│                              3次失败                    │
│                                  ↓                      │
│                               FAULT                     │
│                                                         │
│  状态变化时调用:                                         │
│    powerfs_conn_set_state(conn, new_state)              │
│       ↓                                                 │
│    通知 shard 路由层 (事件传播)                           │
└──────────────────────┬──────────────────────────────────┘
                       │ 事件
                       ▼
┌─────────────────────────────────────────────────────────┐
│  Shard 路由状态机 (per-shard)                             │
│                                                         │
│  ROUTE_VALID ──→ leader filer断连 ──→ ROUTE_CHECKING     │
│       ↑                                    │             │
│       │              ┌─────────────────────┘             │
│       │              ↓                                   │
│       │     找到新leader(REDIRECT) → ROUTE_VALID         │
│       │              │                                   │
│       │     所有filer试过无leader → ROUTE_UNKNOWN        │
│       │              │                                   │
│       │     filer重连成功 → ROUTE_CHECKING (重新确认)     │
│       │                                                  │
│  请求路由时根据 shard 状态决定动作:                        │
│    VALID:    直接用 leader filer 的连接                   │
│    CHECKING: 尝试其他 CONNECTED 的 filer                  │
│    UNKNOWN:  返回 -EAGAIN (VFS 重试) 或等待               │
└─────────────────────────────────────────────────────────┘
```

#### 10.5.2 事件传播实现

```c
/**
 * powerfs_conn_set_state - 连接状态变更 (事件入口)
 *
 * 所有连接状态变更都必须经过此函数，确保事件传播到路由层。
 * 不直接写 conn->state，而是通过此函数变更。
 */
void powerfs_conn_set_state(struct powerfs_net_server_conn *conn,
                            enum powerfs_conn_state new_state)
{
    enum powerfs_conn_state old_state;
    int filer_idx = -1;

    spin_lock(&conn->state_lock);
    old_state = conn->state;
    conn->state = new_state;
    spin_unlock(&conn->state_lock);

    if (old_state == new_state)
        return;

    pr_info("powerfs: conn %s:%u state: %s → %s\n",
            conn->addr, conn->port,
            powerfs_conn_state_str(old_state),
            powerfs_conn_state_str(new_state));

    /* 查找 filer_idx (在 pool 中的索引) */
    filer_idx = powerfs_conn_get_filer_idx(conn);
    if (filer_idx < 0)
        return;

    /* 事件传播到 shard 路由层 */
    switch (new_state) {
    case CONN_RECONNECTING:
        /* 断连: 所有 leader=该filer 的 shard → CHECKING */
        powerfs_shard_route_on_filer_disconnect(filer_idx);
        break;

    case CONN_CONNECTED:
        if (old_state == CONN_RECONNECTING) {
            /* 重连成功: 相关 shard → CHECKING (重新确认 leader) */
            powerfs_shard_route_on_filer_reconnect(filer_idx);
        }
        break;

    case CONN_FAULT:
        /* 彻底故障: 相关 shard → CHECKING (尝试其他 filer) */
        powerfs_shard_route_on_filer_disconnect(filer_idx);
        break;

    default:
        break;
    }
}

/**
 * powerfs_shard_route_on_filer_disconnect - filer 断连事件
 *
 * 遍历所有 shard, 将 leader=该filer 的 shard 标记为 CHECKING。
 * CHECKING 状态下，请求会尝试其他 CONNECTED 的 filer。
 */
void powerfs_shard_route_on_filer_disconnect(int filer_idx)
{
    struct powerfs_shard_route *route = &g_pool.shard_route;
    int i;

    spin_lock(&route->lock);
    for (i = 0; i < route->shard_count && i < POWERFS_MAX_SHARDS; i++) {
        if (route->entries[i].leader_filer_idx == filer_idx &&
            route->entries[i].state == ROUTE_VALID) {
            route->entries[i].state = ROUTE_CHECKING;
            pr_info("powerfs: shard %d → CHECKING (filer %d disconnected)\n",
                    i, filer_idx);
        }
    }
    spin_unlock(&route->lock);
}

/**
 * powerfs_shard_route_on_filer_reconnect - filer 重连成功事件
 *
 * 不直接恢复 VALID，因为 leader 可能已切换到其他 filer。
 * 标记为 CHECKING，让下次请求确认 leader 是否还在该 filer。
 */
void powerfs_shard_route_on_filer_reconnect(int filer_idx)
{
    struct powerfs_shard_route *route = &g_pool.shard_route;
    int i;

    spin_lock(&route->lock);
    for (i = 0; i < route->shard_count && i < POWERFS_MAX_SHARDS; i++) {
        if (route->entries[i].leader_filer_idx == filer_idx &&
            route->entries[i].state == ROUTE_UNKNOWN) {
            route->entries[i].state = ROUTE_CHECKING;
            pr_info("powerfs: shard %d → CHECKING (filer %d reconnected)\n",
                    i, filer_idx);
        }
    }
    spin_unlock(&route->lock);
}
```

#### 10.5.3 请求路由与状态机联动

```c
/**
 * powerfs_net_send_request_v2 - 状态驱动的请求路由
 *
 * 根据 shard 路由状态决定动作:
 *   ROUTE_VALID:    直接用 leader filer 连接
 *   ROUTE_CHECKING: 尝试其他 CONNECTED 的 filer
 *   ROUTE_UNKNOWN:  返回 -EAGAIN
 */
int powerfs_net_send_request_v2(u64 shard_id, __u16 msg_type, ...)
{
    enum powerfs_shard_route_state route_state;
    struct powerfs_net_server_conn *conn;
    int filer_idx;
    int filers_tried = 0;

    for (;;) {
        /* 1. 查询 shard 路由状态 */
        route_state = powerfs_shard_route_get_state(shard_id);

        switch (route_state) {
        case ROUTE_VALID:
            /* leader 已知且连接正常, 直接使用 */
            conn = powerfs_conn_get_filer_for_shard(shard_id);
            if (!conn || conn->state != CONN_CONNECTED) {
                /* 连接实际不可用 (竞态), 触发状态更新 */
                powerfs_shard_route_on_filer_disconnect(
                    powerfs_shard_route_get_leader(shard_id));
                continue;  /* 重新评估状态 */
            }
            break;

        case ROUTE_CHECKING:
            /* leader 不确定, 尝试找一个 CONNECTED 的 filer */
            filer_idx = powerfs_shard_route_find_available_filer(shard_id);
            if (filer_idx < 0) {
                /* 没有 CONNECTED 的 filer, 进入 UNKNOWN */
                powerfs_shard_route_set_state(shard_id, ROUTE_UNKNOWN);
                continue;
            }
            conn = &g_pool.filers[filer_idx];
            break;

        case ROUTE_UNKNOWN:
            /* 所有 filer 都不可用 */
            return -EAGAIN;
        }

        /* 2. 通过该 filer 的连接发送请求 */
        mutex_lock(&conn->send_mutex);
        ret = do_send_recv(conn, msg_type, body, data, ...);
        mutex_unlock(&conn->send_mutex);

        /* 3. 根据响应更新路由状态 */
        if (ret == STATUS_REDIRECT) {
            /* 解析 REDIRECT 目标 */
            parse_redirect(resp, new_addr, &new_port);
            new_filer = powerfs_conn_find_filer(new_addr, new_port);

            if (new_filer >= 0) {
                /* 找到新 leader, 更新路由 → VALID */
                powerfs_shard_route_update(shard_id, new_filer);
                continue;  /* 用新 leader 重试 */
            }
            /* 未知 filer, 标记 UNKNOWN */
            powerfs_shard_route_set_state(shard_id, ROUTE_UNKNOWN);
            return -EAGAIN;
        }

        if (ret == 0) {
            /* 请求成功, 确认 leader → VALID */
            if (route_state == ROUTE_CHECKING) {
                powerfs_shard_route_update(shard_id,
                    powerfs_conn_get_filer_idx(conn));
            }
        }

        /* 网络错误: 触发该连接的状态变更 (→ RECONNECTING) */
        if (ret < 0) {
            powerfs_conn_set_state(conn, CONN_RECONNECTING);
            /* 状态变更会触发 shard → CHECKING */
            continue;  /* 重新评估状态, 可能尝试其他 filer */
        }

        return ret;
    }
}
```

#### 10.5.4 断连检测机制

内核中检测连接断开的三种途径（互为补充）：

| 机制 | 触发时机 | 延迟 | 实现 |
|------|----------|------|------|
| **send/recv 错误** | 请求发送或接收失败时 | 即时 | `do_send_recv` 返回负值 |
| **socket 回调** | TCP 状态变化 (RST/FIN) | 即时 | `sk_state_change` 回调 |
| **健康监控** | 定期 ping | 5s | `health_monitor_fn` |

**send/recv 错误检测（主要途径）:**
```c
/* 在 do_send_recv 中 */
ret = powerfs_net_frame_send(conn->sock, ...);
if (ret < 0) {
    /* 发送失败 = 连接断开 */
    powerfs_conn_set_state(conn, CONN_RECONNECTING);
    schedule_delayed_work(&conn->reconnect_work, 0);
    return ret;  /* 上层会重新评估路由状态 */
}
```

**socket 回调检测（即时，但可选）:**
```c
/* 在 connect 时设置 socket 回调 */
static void powerfs_sock_state_change(struct sock *sk)
{
    if (sk->sk_state != TCP_ESTABLISHED) {
        struct powerfs_net_server_conn *conn = sk->sk_user_data;
        if (conn && conn->state == CONN_CONNECTED) {
            powerfs_conn_set_state(conn, CONN_RECONNECTING);
            schedule_delayed_work(&conn->reconnect_work, 0);
        }
    }
}
```

### 10.6 关键对比

| 维度 | 单连接模型 (当前) | 连接池模型 (目标) |
|------|-------------------|-------------------|
| **连接数量** | 1 个 Filer + 1 个 Volume | 所有 Filer + 所有 Volume |
| **REDIRECT 处理** | disconnect → connect → 重试 | 更新路由表 → 复用已有连接重试 |
| **互斥锁** | 全局 `send_recv_mutex` | 每 conn 独立 `send_mutex` |
| **Filer 故障影响** | 全局断连重连 | 仅该 filer 的 conn 重连 |
| **选举中重试** | disconnect+reconnect 循环 (昂贵) | 同一连接重试 (廉价，连接不断) |
| **并发能力** | 所有请求串行 | 不同 filer 的请求并行 |
| **重连范围** | 全局 reconnect_work 遍历所有 filer | 仅故障 filer 的 reconnect_work |

### 10.6 初始化流程

```c
/**
 * powerfs_conn_pool_init - 挂载时初始化连接池
 *
 * 1. 从 Master 获取 filer 和 volume 列表
 * 2. 为每个节点创建连接
 * 3. 并行连接所有节点
 * 4. 启动健康监控
 */
int powerfs_conn_pool_init(struct powerfs_conn_pool *pool,
                            const char *master_addr, __u16 master_port)
{
    /* 1. 从 Master 获取 filer 列表 (ListFilers) */
    ret = master_list_filers(master_addr, master_port,
                              filer_list, &filer_count);

    /* 2. 从 Master 获取 volume 列表 (GetTopology) */
    ret = master_get_topology(master_addr, master_port,
                               volume_list, &volume_count);

    /* 3. 初始化 Filer 连接池 */
    for (i = 0; i < filer_count; i++) {
        conn = &pool->filers[i];
        conn->addr = filer_list[i].addr;
        conn->port = filer_list[i].net_port;
        conn->node_id = filer_list[i].id;
        conn->is_filer = true;
        mutex_init(&conn->send_mutex);
        init_waitqueue_head(&conn->reconnect_wq);
        INIT_DELAYED_WORK(&conn->reconnect_work,
                          filer_reconnect_work_fn);

        /* 并行连接 (不阻塞其他 filer) */
        schedule_work(&conn->reconnect_work.work);
    }
    pool->filer_count = filer_count;

    /* 4. 初始化 Volume 连接池 (同理) */
    for (i = 0; i < volume_count; i++) {
        /* ... 同上 ... */
    }
    pool->volume_count = volume_count;

    /* 5. 等待至少一个 Filer 连接成功 (leader 所在) */
    wait_event_timeout(pool->any_connected_wq,
        any_filer_connected(pool), msecs_to_jiffies(30000));

    /* 6. 启动健康监控 */
    schedule_delayed_work(&pool->health_monitor,
                         msecs_to_jiffies(5000));

    return 0;
}
```

### 10.7 独立重连

```c
/**
 * filer_reconnect_work_fn - 单个 Filer 的独立重连
 *
 * 只重连这一个 Filer，不影响其他连接。
 * 最多重试 3 次，失败后标记 FAULT。
 */
static void filer_reconnect_work_fn(struct work_struct *work)
{
    struct powerfs_conn *conn =
        container_of(work, struct powerfs_conn, reconnect_work.work);

    if (conn->state == CONN_CONNECTED)
        return;

    if (conn->reconnect_count >= 3) {
        pr_err("powerfs: filer %s:%u reconnect failed 3 times, FAULT\n",
               conn->addr, conn->port);
        spin_lock(&conn->state_lock);
        conn->state = CONN_FAULT;
        spin_unlock(&conn->state_lock);
        wake_up(&conn->reconnect_wq);
        return;
    }

    conn->reconnect_count++;
    pr_info("powerfs: reconnecting filer %s:%u (attempt %d/3)\n",
            conn->addr, conn->port, conn->reconnect_count);

    ret = powerfs_net_connect(conn, conn->addr, conn->port);
    if (ret == 0) {
        spin_lock(&conn->state_lock);
        conn->state = CONN_CONNECTED;
        conn->reconnect_count = 0;
        spin_unlock(&conn->state_lock);
        wake_up(&conn->reconnect_wq);
        pr_info("powerfs: filer %s:%u reconnected\n",
                conn->addr, conn->port);
    } else {
        /* 2 秒后重试 */
        schedule_delayed_work(&conn->reconnect_work,
                             msecs_to_jiffies(2000));
    }
}
```

### 10.8 健康监控

```c
/**
 * health_monitor_fn - 定期检查所有连接健康状态
 *
 * 对每个连接独立 ping，失败只触发该连接的重连。
 */
static void health_monitor_fn(struct work_struct *work)
{
    struct powerfs_conn_pool *pool = ...;
    int i;

    if (!pool->monitoring)
        return;

    /* 检查所有 Filer 连接 */
    for (i = 0; i < pool->filer_count; i++) {
        struct powerfs_conn *conn = &pool->filers[i];

        if (conn->state != CONN_CONNECTED)
            continue;  /* 已经在重连中 */

        ret = filer_ping(conn);
        if (ret < 0) {
            pr_warn("filer %d (%s:%u) health check failed: %d\n",
                    i, conn->addr, conn->port, ret);

            /* 只断开这一个连接 */
            powerfs_conn_disconnect(conn);

            /* 触发这一个 filer 的重连 */
            schedule_delayed_work(&conn->reconnect_work, 0);
        }
    }

    /* Volume 连接同理 */

    schedule_delayed_work(&pool->health_monitor,
                         msecs_to_jiffies(5000));
}
```

### 10.9 与状态机的关系

连接池模型下，状态机简化为：

**Filer 连接状态（每连接独立）：**
```
DISCONNECTED → CONNECTING → CONNECTED
                  ↓                ↓
               fail          health check fail
                  ↓                ↓
              RECONNECTING ←───────┘
                  ↓
           3x fail → FAULT
```

**MetaShard 状态（基于路由表）：**
```
NORMAL:     shard_leader[shard_id] 指向已连接的 filer
REDIRECT:   收到 REDIRECT → 更新路由表 → NORMAL
ELECTION:   收到 REDIRECT-to-self → 同一连接重试 (不断开)
UNAVAILABLE: 所有 filer 都 FAULT → -ENOTCONN
```

**关键简化：** 不再有全局的 "RECONNECTING" 状态。每个连接独立管理自己的重连，上层只关心路由表是否指向可用连接。

### 10.10 迁移计划

```
Phase 1: 连接池基础设施 (内核侧)
├── 定义 struct powerfs_conn 和 struct powerfs_conn_pool
├── 实现 per-conn connect/disconnect/reconnect
├── 实现 per-conn mutex (替代全局 send_recv_mutex)
└── 实现 health_monitor (per-conn ping)

Phase 2: MetaShard 路由表
├── 定义 shard_leader_map[shard_id] → filer_idx
├── 实现 get_filer_conn_for_shard()
├── 实现 REDIRECT → 更新路由表 (不断开连接)
└── 初始路由表从 Filer 响应中学习

Phase 3: 请求路由改造
├── send_request 改为通过连接池发送
├── REDIRECT 处理改为路由表更新 + 复用连接
├── 移除全局 send_recv_mutex
└── 移除全局 g_conn 和 reconnect_work

Phase 4: Volume 连接池
├── Volume 连接池 (同 Filer 模式)
├── 数据操作通过 Volume 连接池发送
└── per-volume 重连

Phase 5: 测试验证
├── 单 filer 断连: 只影响该 filer 的 shard
├── 多 filer 断连: 各自独立重连
├── REDIRECT: 不断开连接，路由表更新
├── 选举中: 同一连接重试 (无 disconnect 开销)
└── 全 filer 断连: 全部 FAULT → -ENOTCONN
```

### 10.11 与当前代码的关系

当前代码中需要替换的关键部分：

| 当前代码 | 连接池模型 |
|----------|------------|
| `g_conn.sock` (单连接) | `pool->filers[i].sock` (多连接) |
| `g_conn.state` (全局状态) | `pool->filers[i].state` (per-conn) |
| `send_recv_mutex` (全局锁) | `conn->send_mutex` (per-conn锁) |
| `g_conn.reconnect_work` (全局重连) | `conn->reconnect_work` (per-conn重连) |
| `g_pool.leader_known` (全局标志) | `routes->shard_leader[s]` (路由表) |
| REDIRECT: disconnect+connect | REDIRECT: 更新路由表+复用连接 |
| `g_conn.failover_count` | 移除 (per-conn 重连不需要全局 failover) |
| `g_conn.reconnect_failed` | `conn->state == CONN_FAULT` (per-conn) |

### 10.9 多 MetaShard 组设计

#### 9.9.1 当前架构 vs 目标架构

**当前：全参与模式（1个Filer集群，所有filer参与所有shard）**

```
Filer Cluster A (filer-1/2/3):
├── Shard 0 (Raft Group: filer-1, filer-2, filer-3)
└── Shard 1 (Raft Group: filer-1, filer-2, filer-3)
```

- 优点：简单，所有filer有所有数据
- 缺点：不可水平扩展（Raft节点>7时共识开销过大）

**目标：分片归属模式（多个Filer集群，各管不同shard）**

```
Filer Cluster A (filer-1/2/3):
├── Shard 0 (Raft Group: filer-1, filer-2, filer-3)
├── Shard 1 (Raft Group: filer-1, filer-2, filer-3)
└── Shard 2 (Raft Group: filer-1, filer-2, filer-3)

Filer Cluster B (filer-4/5/6):
├── Shard 3 (Raft Group: filer-4, filer-5, filer-6)
├── Shard 4 (Raft Group: filer-4, filer-5, filer-6)
└── Shard 5 (Raft Group: filer-4, filer-5, filer-6)
```

- 优点：水平扩展，独立故障域
- 需要：跨集群重定向、Master分片映射

#### 9.9.2 新增状态：NotManaged

`MetaShardState` 枚举新增 `NotManaged` 状态：

```rust
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MetaShardState {
    Normal,                                      // 是leader，可服务
    Follower { leader_addr: String },            // 不是leader，REDIRECT到leader
    WaitingElection,                             // 选举中，REDIRECT到自身
    Unavailable,                                 // 不可恢复，SERVER_ERROR
    NotManaged { correct_cluster_filer: String },// 本集群不管此shard，REDIRECT到正确集群 ★新增
}
```

**`NotManaged` vs `Unavailable` 的区别：**

| 状态 | 含义 | 行为 | 错误码 |
|------|------|------|--------|
| `Unavailable` | 本集群管理此shard，但故障（无多数派/存储错误） | SERVER_ERROR | -EREMOTEIO (永久) |
| `NotManaged` | 本集群不管此shard | REDIRECT到正确集群的filer | -EAGAIN (重试) |

#### 9.9.3 跨集群重定向流程

```
内核                    Cluster A filer              Master              Cluster B filer
  │                          │                         │                       │
  │── write(ino) ───────────→│                         │                       │
  │                          │ calculate_shard(ino)=3  │                       │
  │                          │ shard 3 不属于本集群     │                       │
  │                          │── get_cluster_for_shard(3) ──→│                │
  │                          │←── filer-4:172.30.0.38 ─────│                 │
  │←── REDIRECT(172.30.0.38) │                         │                       │
  │                          │                         │                       │
  │── write(ino) ────────────┼─────────────────────────┼──────────────────────→│
  │                          │                         │                       │
  │←── OK ───────────────────┼─────────────────────────┼──────────────────────│
```

**关键点：** 现有 REDIRECT 机制天然支持跨集群重定向，内核无需感知"集群"概念——只看到 REDIRECT 地址，连接新地址重试即可。

#### 9.9.4 Master 分片映射管理

Master 维护 shard → cluster 映射表：

```rust
// powerfs-master 新增数据结构
struct ShardClusterMapping {
    /// shard_id → 集群信息
    mappings: RwLock<HashMap<u64, ClusterInfo>>,
}

struct ClusterInfo {
    cluster_id: String,
    /// 该集群的filer列表（用于跨集群redirect）
    filers: Vec<FilerInfo>,
    /// 该集群管理的shard范围
    shard_range: (u64, u64),
}
```

**Filer注册时声明shard范围：**
```rust
// Filer启动时向Master注册
RegisterFilerRequest {
    raft_id: 1,
    grpc_addr: "172.30.0.38:8889",
    net_addr: "172.30.0.38:9334",
    cluster_id: "cluster-b",        // ★新增
    managed_shards: [3, 4, 5],      // ★新增：本集群管理的shard列表
}
```

**Filer缓存映射表：**
- Filer启动时从Master拉取全量映射
- 每60秒刷新一次（或通过Redis事件订阅增量更新）
- 收到非本集群shard请求时，查缓存返回REDIRECT

#### 9.9.5 check_leader 更新

```rust
async fn check_leader(&self, msg: &NetMessage, shard_id: ShardId) -> Result<(), NetMessage> {
    // 1. 先检查本集群是否管理此shard
    if !self.meta_shard_manager.manages_shard(shard_id) {
        // 查缓存找正确集群的filer
        let correct_filer = self.shard_mapping
            .get_filer_for_shard(shard_id)
            .await
            .ok_or_else(|| {
                // Master也不知道 → 不可用
                error!("shard {}: NotManaged but no cluster mapping found", shard_id.0);
                Self::build_response(msg, STATUS_ERR_SERVER_ERROR, Vec::new())
            })?;

        warn!("shard {}: NotManaged, redirect to cluster B filer {}",
              shard_id.0, correct_filer);
        let mut enc = TlvEncoder::new();
        let _ = enc.add_string(FieldId::Owner, &correct_filer);
        return Err(Self::build_response(msg, STATUS_ERR_REDIRECT, enc.into_bytes()));
    }

    // 2. 本集群管理此shard，检查MetaShard状态
    let state = self.meta_shard_manager.get_shard_state(shard_id).await;
    match state {
        MetaShardState::Normal => Ok(()),
        MetaShardState::Follower { leader_addr } => { /* REDIRECT到leader */ }
        MetaShardState::WaitingElection => { /* REDIRECT到自身 */ }
        MetaShardState::Unavailable => { /* SERVER_ERROR */ }
        MetaShardState::NotManaged { .. } => {
            // 不会到达此处（上面已检查manages_shard）
            unreachable!("NotManaged should be handled above")
        }
    }
}
```

#### 9.9.6 状态机更新（多集群场景）

每个集群的每个shard有独立状态，互不影响：

```
Cluster A:
├── Shard 0: NORMAL ✓
├── Shard 1: WAITING_ELECTION (选举中)
└── Shard 2: NORMAL ✓

Cluster B:
├── Shard 3: NORMAL ✓
├── Shard 4: NORMAL ✓
└── Shard 5: UNAVAILABLE (多数派故障)
```

**故障隔离验证：**
- Cluster B 的 Shard 5 UNAVAILABLE 不影响 Cluster A 的任何 shard
- 内核访问 Shard 5 时收到 SERVER_ERROR，访问其他 shard 正常

#### 9.9.7 测试环境建议

**阶段2断连测试（当前）：** 保持3 filer + shard_count=2

- 已有2个独立Raft组，足以验证per-shard状态机
- 断连/重连/选举场景在单集群内可充分测试
- 增加filer不改变断连测试的本质

**阶段3多集群验收（后续）：** 增加 filer-4/5/6

```
Cluster A: filer-1/2/3 (172.30.0.35-37) → shard 0, 1
Cluster B: filer-4/5/6 (172.30.0.38-40) → shard 2, 3
```

需完成的准备工作：
1. 创建 filer-4/5/6 配置文件（独立raft_peers）
2. docker-compose.yml 添加3个filer服务
3. Master 实现 shard→cluster 映射管理
4. Filer 实现 `NotManaged` 状态和跨集群redirect
5. Filer 启动时声明 `managed_shards` 和 `cluster_id`

**多集群测试矩阵：**

| 测试场景 | 操作 | 预期 |
|----------|------|------|
| 跨集群redirect | 内核连接Cluster A，写shard 3的数据 | REDIRECT到Cluster B，写入成功 |
| Cluster A全挂，Cluster B正常 | kill filer-1/2/3，写shard 3 | 成功（故障隔离） |
| Cluster A全挂，Cluster B正常 | kill filer-1/2/3，写shard 0 | -ENOTCONN（本集群不可用） |
| Cluster A恢复 | 重启filer-1/2/3，写shard 0 | 选举完成后成功 |
| 两集群同时选举 | 重启所有filer，写shard 0和shard 3 | 各自独立选举，互不影响 |

#### 9.9.8 迁移路径

```
当前 ──────────────────────────────────────────────────── 目标
│                                                          │
├─ Step 1: Filer侧状态机 (已完成)                          │
│  └─ MetaShardState + check_leader状态驱动                │
│                                                          │
├─ Step 2: 内核侧状态机 (待实现)                            │
│  └─ enum powerfs_meta_state + send_request状态分派       │
│                                                          │
├─ Step 3: 阶段2断连测试 (当前)                             │
│  └─ 3 filer + 2 shard, 验证断连/重连/选举                │
│                                                          │
├─ Step 4: Master分片映射 (后续)                            │
│  └─ RegisterFilerRequest增加cluster_id + managed_shards  │
│  └─ Master维护shard→cluster映射表                        │
│  └─ Filer缓存映射，支持NotManaged状态                     │
│                                                          │
├─ Step 5: 多集群部署 (后续)                                │
│  └─ 添加filer-4/5/6配置和docker-compose                  │
│  └─ 两个集群独立运行，跨集群redirect                      │
│                                                          │
└─ Step 6: 多集群验收测试 (后续)                            │
   └─ 故障隔离、跨集群redirect、独立选举                    │
```

## 十一、异步收发架构重构（真并行，面向大规模节点）

### 11.1 问题根源：同步回合制 = 假并行

当前 `powerfs_request_do_send`（powerfs_net.c:1588-1637）的关键缺陷：

```c
mutex_lock(&conn->send_mutex);      // 抢连接锁
frame_send(sock, req);              // 发请求
frame_recv(sock, &resp);            // 阻塞收响应 —— 持锁等待!
mutex_unlock(&conn->send_mutex);
```

`send_mutex` 在 send + recv 期间一直持有。后果：

1. **单连接串行**：同一 filer 连接同时只有一个在途请求（outstanding=1）。10 个线程写同一 leader → 排成一字长蛇阵。
2. **跨 filer 才并行**：不同 conn 不同 mutex，但同一 shard 的 leader 唯一，该 shard 的全部 I/O 实质串行。
3. **断连感知滞后**：无独立接收路径常驻 recv，空闲时无人感知 peer FIN/RST，只能靠 send 探测（且 send 对优雅关闭的 socket 会缓冲数据、不立即报错）→ leader 死亡后写请求卡满 RECV_TIMEOUT 才超时（实测 30s）。
4. **健康监控抢锁**：send 探测也要抢 send_mutex，和正常 I/O 互相阻塞。

面向多节点（N filer × M shard），回合制是吞吐与扩展性的根本瓶颈，必须重构为异步流水线。

### 11.2 目标架构：per-conn RX 线程 + 流水线发送

内核态对应 epoll 的机制是 **per-connection 专用接收 kthread**：常驻 `kernel_recvmsg`，任何帧到达即返回并按 seq 分发。

```
┌──────────────────────────── per filer conn ─────────────────────────────┐
│                                                                         │
│   发送侧 (多线程并发)              接收侧 (单 RX kthread)                │
│   ───────────────────              ──────────────────────                │
│   req1: insert tree ──┐            while (!should_stop) {               │
│   req2: insert tree ──┤              n = kernel_recvmsg(sock)  ← 阻塞   │
│        frame_send ←─── send_mutex     if (n<=0) → 断连, exit            │
│        (只锁发送瞬间)                 req = tree_lookup(seq)            │
│        wait_for_completion            complete(&req->done)              │
│   req1: wait done ←─────────────────  }                                  │
│   req2: wait done ←─────────────────                                     │
│        (outstanding = N, 流水线)                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

**核心特征：**

- 发送只锁 `send_mutex` 的 `frame_send` 瞬间，**不持锁等响应**。
- 多个请求可同时 outstanding（pipeline 深度受窗口控制），RX 线程按 seq 异步分发。
- RX 线程常驻 recv：peer 关闭（FIN/RST）立即返回 0/-ECONNRESET → **即时断连感知，无需 send 探测**。

### 11.3 数据结构扩展

```c
struct powerfs_net_server_conn {
    /* ... 既有字段 ... */
    struct socket *sock;
    struct mutex send_mutex;          /* 仅保护 frame_send, 不再覆盖 recv */

    /* === 异步收发 === */
    struct task_struct *rx_task;      /* per-conn 接收线程 */
    bool rx_running;                  /* RX 线程是否在运行 */
    atomic_t next_seq;                /* 请求序号 (原子递增) */
    struct work_struct disconnect_work; /* RX 检测断连后触发的清理 work */

    /* 请求管理 (已存在, 复用) */
    struct rb_root req_tree;          /* seq → request 红黑树 */
    struct list_head pending_reqs;    /* 待答请求链表 */
    spinlock_t req_lock;
};
```

### 11.4 请求生命周期

```c
/* 发送路径: 只发不等收 */
int powerfs_request_submit(req) {
    conn = route_select(shard);           /* 选 filer */
    seq = atomic_inc_return(&conn->next_seq);
    req->seq = seq;

    /* 1. 先入待答树 (RX 可能在 send 完成前就收到响应) */
    spin_lock(&conn->req_lock);
    powerfs_req_tree_insert(conn, req);
    list_add_tail(&req->list_node, &conn->pending_reqs);
    spin_unlock(&conn->req_lock);

    /* 2. 发送 (只锁 send 瞬间) */
    mutex_lock(&conn->send_mutex);
    ret = frame_send(conn->sock, req, seq);
    mutex_unlock(&conn->send_mutex);

    if (ret < 0) {
        /* 发送失败: 摘除请求, 触发断连清理 */
        __req_remove(conn, req);
        powerfs_conn_disconnect(conn);    /* shutdown socket → 唤醒 RX */
        return -ENOTCONN;
    }

    /* 3. 等待 RX 线程 complete (可超时) */
    ret = wait_for_completion_timeout(&req->done, RECV_TIMEOUT);
    if (ret == 0) {
        __req_remove(conn, req);          /* 超时摘除 */
        return -ETIMEDOUT;
    }
    return req->error;                    /* RX 已填充 resp */
}

/* RX 线程: 常驻接收, 按 seq 分发 */
int rx_thread_fn(conn) {
    while (!kthread_should_stop()) {
        n = frame_recv(conn->sock, &hdr, body, data);   /* 阻塞 */
        if (n <= 0) {
            /* EOF / RST / 错误 → 调度清理 work, 退出 */
            schedule_work(&conn->disconnect_work);
            return 0;
        }
        if (hdr.flags & NOTIFY) {
            async_notify_dispatch(conn, &hdr, body, data);
            continue;
        }
        req = req_tree_lookup(conn, hdr.seq);
        if (req) {
            req_fill_response(req, &hdr, body, data);
            __req_remove(conn, req);
            complete(&req->done);
        }
    }
    return 0;
}
```

### 11.5 断连检测与清理

**唯一清理入口 `powerfs_conn_disconnect(conn)`**（幂等，可从多处调用）：

```c
void powerfs_conn_disconnect(conn) {
    if (conn->state != CONN_CONNECTED) return;   /* 幂等 */
    conn_set_state(conn, CONN_RECONNECTING);

    /* 1. shutdown socket → 唤醒阻塞在 recv 的 RX 线程 */
    if (conn->sock) kernel_sock_shutdown(conn->sock, SHUT_RDWR);

    /* 2. 停止并回收 RX 线程 (kthread_stop 对已退出线程也安全) */
    if (conn->rx_task) {
        kthread_stop(conn->rx_task);
        conn->rx_task = NULL;
    }

    /* 3. 唤醒在途请求的 submit (标记重发, 让其重试其他 filer) */
    spin_lock(&conn->req_lock);
    list_for_each_entry(req, &conn->pending_reqs, list_node) {
        req->needs_resend = true;
        req->error = -ENOTCONN;
        complete(&req->done);
    }
    /* 清空 tree/list */
    spin_unlock(&conn->req_lock);

    /* 4. 路由降级 + 关闭 sock */
    shard_route_on_filer_disconnect(conn);
    sock_release(conn->sock); conn->sock = NULL;

    /* 5. 调度重连 */
    schedule_delayed_work(&conn->reconnect_work, RECONNECT_DELAY);
}
```

**调用来源：**

- RX 线程检测到 recv 错误 → `schedule_work(&conn->disconnect_work)` → disconnect_work 调 `powerfs_conn_disconnect`（process context，kthread_stop 安全）。
- 发送路径 send 失败 → 直接调 `powerfs_conn_disconnect`。
- 卸载/停止 → 对每个 conn 调 `powerfs_conn_disconnect`。

**kthread_stop 安全性**：RX 线程自行退出（error 路径）后 task_struct 成为 zombie，`kthread_stop` 会 reap 并立即返回（`exited` completion 已 done）。线程仍运行时先 `kernel_sock_shutdown` 唤醒其阻塞 recv，线程见 `kthread_should_stop` 退出，`kthread_stop` 回收。不会自死锁（RX 线程不直接调 disconnect，而是调度 disconnect_work）。

### 11.6 静默死亡兜底：TCP keepalive

RX 线程能感知 peer **主动关闭**（FIN/RST，如 docker stop / 进程退出）。但**网络静默分区**（拔网线、peer panic 不发 FIN）下 recv 不返回。兜底：`connect_one` 对 socket 启用 keepalive：

```c
tcp_sock_set_keepalive(sock->sk, 1);
tcp_sock_set_keepidle(sock->sk, 5);    /* 空闲 5s 开始探测 */
tcp_sock_set_keepintvl(sock->sk, 2);   /* 每 2s 探测 */
tcp_sock_set_keepcnt(sock->sk, 3);     /* 3 次失败 → 设 sk_err */
```

keepalive 失败 → `sk_err` 置位 → RX 线程 recv 返回 -ETIMEDOUT → 触发断连清理。检测时延 ≈ 5 + 3×2 = 11s。

**健康监控 `health_monitor_fn` 移除**：send 探测职责完全被 RX 线程 + keepalive 取代。

### 11.7 重连成功后启动 RX 线程

```c
void powerfs_conn_reconnect_work(conn) {
    if (stopping) return;
    ret = powerfs_conn_connect_one(conn);   /* 建 TCP */
    if (ret == 0) {
        conn_set_state(conn, CONN_CONNECTED);
        shard_route_on_filer_reconnect(conn);
        conn->rx_task = kthread_run(rx_thread_fn, conn, "pfs_rx_%s", conn->addr);
        powerfs_request_resend_pending(conn);   /* 重发在途请求 */
    } else {
        schedule_delayed_work(&conn->reconnect_work, backoff);
    }
}
```

### 11.8 Filer 侧前提

流水线要求 filer（Rust tokio）支持**并发处理多请求并按 seq 响应**。tokio async handler 天然支持（每请求独立 task，按 seq 回复）。验收点：单连接多 outstanding 请求时，filer 响应顺序与到达顺序解耦，按 seq 正确回复，不串行化同一连接的请求。

### 11.9 与旧设计对比

| 维度 | 旧（同步回合制） | 新（异步 RX 线程） |
|------|------------------|---------------------|
| 单连接 outstanding | 1（串行） | N（流水线，窗口控制） |
| send_mutex 覆盖 | send + recv | 仅 frame_send |
| 断连感知 | send 探测 / 请求超时 (30s) | RX recv 即时 (FIN/RST) + keepalive (11s 静默) |
| 健康监控 | send ping（抢锁） | 移除（RX 线程 + keepalive 取代） |
| 跨 filer 并行 | 是 | 是 |
| 同 shard 并行 | 否（leader 唯一 + 串行） | 是（流水线） |
| 扩展性 | 节点多时瓶颈在串行 | 线性扩展 |

### 11.10 迁移步骤

```
Phase 1: RX 线程基础设施
├── conn 新增 rx_task / disconnect_work / next_seq 字段
├── 实现 rx_thread_fn (recv → 按 seq 分发 → complete)
├── 实现 disconnect_work → powerfs_conn_disconnect (幂等清理)
└── connect 成功后 kthread_run 启动 RX

Phase 2: 发送路径解耦
├── request_do_send 拆分: insert tree → frame_send(仅锁send) → wait_for_completion
├── send 失败 → powerfs_conn_disconnect → 返回 -ENOTCONN
├── 超时 → 摘除请求 → -ETIMEDOUT
└── 移除 do_send 内的 frame_recv (改由 RX 线程接收)

Phase 3: 断连与重连
├── RX 线程 error 路径 → schedule_work(disconnect_work) → 退出
├── powerfs_conn_disconnect: shutdown + kthread_stop + 标记重发 + route 降级
├── reconnect_work 成功后启动新 RX 线程
└── enable TCP keepalive (keepidle=5, intvl=2, cnt=3)

Phase 4: 移除旧机制
├── 移除 health_monitor_fn (send 探测)
├── 移除 conn_health_monitor 调度
└── 移除 do_send 内同步 recv 残留

Phase 5: 验证 (QEMU)
├── 基准: 单连接多线程写, 确认 outstanding>1 (fio QD 或 dd 并发)
├── leader 断连: docker stop leader → 写 failover ≤ 2s (RX 即时感知)
├── 静默死亡: 网络分区模拟 → keepalive ~11s 检测 → failover
├── 重连恢复: 重启 filer → RX 线程重启 → 请求重发
└── 压测: fio QD32 对比旧设计吞吐
```

### 11.11 验收指标

- leader 断连后写请求 failover 完成 ≤ 2s（旧：30s 超时）
- 单连接流水线深度 ≥ 8（fio QD8+ 吞吐显著高于 QD1）
- 无 send 探测抢锁导致的延迟尖峰
- 卸载时所有 RX 线程干净退出（无 leak / zombie）

---

## 十二、通信架构 v2：sk 回调 + per-CPT 调度器（面向上千节点，参照 Lustre/BeeGFS）

### 12.0 背景与定位

第十一章的 per-conn kthread 已验证流水线 + failover（leader 断连 104ms），但它是"每连接一线程"模型：N 个 filer = N 个 kthread。面向上千客户端/服务节点时，线程数线性膨胀（1000 连接 ≈ 1000 线程 × 8KB 栈 ≈ 8MB + 调度开销），不够扩展。

**v2 目标**：参照 Lustre socklnd（`ksocknal_scheduler`）和 BeeGFS 内核客户端（`StandardSocket`），改用 **socket 回调 + 固定 M 个调度器线程**，M 个线程服务 N 个连接（M = CPU 分区数，与连接数无关）。这是内核态 epoll 的真正等价物，且是两个成熟分布式文件系统的实战架构。

### 12.1 参照实现要点

**BeeGFS**（`client_module/source/common/net/sock/StandardSocket.c:210-213`）：
```c
sk->sk_data_ready   = sock_readable;      // 数据到达 → wake_up(sk_sleep, POLLIN)
sk->sk_write_space  = sock_write_space;    // 可写 → wake_up(sk_sleep, POLLOUT)
sk->sk_state_change = sock_wakeup;         // 状态变(断连) → wake_up_all
sk->sk_error_report = sock_error_report;   // 错误 → wake_up(POLLERR)
```
回调在 softirq 上下文，仅 `read_lock(sk_callback_lock)` + `__wake_up_sync_key`，唤醒在 socket 等待队列上的 worker。

**Lustre socklnd**（`socklnd_cb.c:1347` + `socklnd_lib.c:448`）：
```c
// 回调 (softirq)
sk->sk_data_ready  = ksocknal_data_ready;   // → conn=sk_user_data; ksocknal_read_callback(conn)
sk->sk_write_space = ksocknal_write_space;  // → ksocknal_write_callback(conn)

// read_callback (softirq, 仅标记+投递+唤醒)
ksocknal_read_callback(conn):
    spin_lock_bh(&sched->kss_lock);
    conn->ksnc_rx_ready = 1;
    if (!conn->ksnc_rx_scheduled) {
        list_add_tail(&conn->ksnc_rx_list, &sched->kss_rx_conns);
        conn->ksnc_rx_scheduled = 1;
        ksocknal_conn_addref(conn);     // 调度器持引用
        wake_up(&sched->kss_waitq);
    }
    spin_unlock_bh(&sched->kss_lock);

// scheduler (process context, per-CPT)
ksocknal_scheduler:
    while (!shuttingdown) {
        conn = list_first_entry_or_null(&sched->kss_rx_conns, ...);
        if (conn) { ... ksocknal_process_receive(conn); ... }   // sock_recvmsg + 分发
        if (tx_conns) { ... ksocknal_process_transmit(conn); ... }
        if (!did_something) wait_event_interruptible_exclusive(sched->kss_waitq, ...);
    }

// 竞态处理 (socklnd_lib.c:511-539)
ksocknal_lib_save_callback:  conn->saved_data_ready = sk->sk_data_ready;  // 保存原始
ksocknal_lib_set_callback:   sk->sk_user_data = conn; sk->sk_data_ready = ksocknal_data_ready;
ksocknal_lib_reset_callback: sk->sk_data_ready = saved; // 恢复; 回调持 read_lock(ksnd_global_lock),
                             // 见 sk_user_data==NULL 则 NOOP (调原始回调), 与 reset 的 write_lock 互斥
```

### 12.2 核心机制：回调驱动 + 调度器消费

```
                    softirq 上下文 (TCP 栈触发)          process 上下文 (调度器线程)
                    ──────────────────────────           ──────────────────────────
  数据到达  ──→  sk_data_ready(conn)            ┌──→  scheduler (per-CPT, M 个)
                   ├ rx_ready = 1                │       wait_event(kss_waitq, rx_conns|tx_conns)
                   ├ list_add(rx_conns)          │       ├ 取 conn from rx_conns
                   └ wake_up(kss_waitq) ─────────┘       ├ sock_recvmsg → 解帧
                                                          ├ req_tree_lookup(seq) → complete(&req->done)
  可写空间  ──→  sk_write_space(conn)                   │ (tx) 取 conn from tx_conns → sock_sendmsg
                   ├ tx_ready = 1                        └ 重新挂回队列 if 还有数据 / 清 scheduled
                   └ wake_up(kss_waitq)
  状态变化  ──→  sk_state_change(conn)    ← 即时断连感知
                   ├ state=CLOSE_WAIT/CLOSE → schedule disconnect_work
  错误      ──→  sk_error_report(conn)
                   └ schedule disconnect_work
```

**关键特征：**
- 回调在 softirq 上下文，**只做 `set flag + list_add + wake_up`**，不碰 mutex/不睡眠/不分配大内存。
- 调度器线程数 = `num_online_cpus()`（每 CPU 一个，**与连接数无关**）。M 个线程服务 N 个连接。
- 断连感知由 `sk_state_change` 回调触发，比 kthread 的 recv 返回更早（状态变化先于数据读取）。
- 空闲时调度器 `wait_event` 睡眠，零 CPU。

### 12.3 数据结构

**决策记录（已锁定）：**
- 调度器线程数 = `num_online_cpus()`（完全跟随 Lustre，每 CPU 一个调度器，最大化并行）。
- TX 路径 = 全异步（tx_queue + `sk_write_space` 触发调度器发送，Lustre 式），与 RX 对称。发送线程只入队 + 投递 + 等待 completion，不直接 `kernel_sendmsg`。

```c
/* per-CPT 调度器 (参照 Lustre ksock_sched) */
struct powerfs_net_sched {
    spinlock_t          lock;           /* 保护 rx_conns/tx_conns (spin_lock_bh) */
    struct list_head    rx_conns;       /* 数据就绪待收的连接 */
    struct list_head    tx_conns;       /* 可写待发的连接 */
    wait_queue_head_t   waitq;          /* 调度器线程等待队列 */
    int                 cpt;            /* 所属 CPU 分区 */
    struct task_struct *task;           /* 调度器线程 */
};

/* 全局 (参照 Lustre ksocknal_data) */
struct powerfs_net_pool {
    struct powerfs_net_sched  *schedulers;   /* [num_cpts] */
    int                        num_cpts;
    rwlock_t                   global_lock;  /* 保护 sk_user_data 解引用 (回调 vs 拆除) */
    /* ... conn 数组、stopping 等 ... */
};

/* conn 新增字段 (替换第十一章的 rx_task) */
struct powerfs_net_server_conn {
    /* ... 既有: sock, send_mutex, req_tree, pending_reqs, req_lock, state ... */

    /* === v2 回调驱动 === */
    struct powerfs_net_sched *sched;          /* 归属的调度器 (按 addr hash 到 CPT) */
    struct list_head          rx_list;        /* 挂到 sched->rx_conns */
    struct list_head          tx_list;        /* 挂到 sched->tx_conns */
    bool                      rx_ready;       /* 回调置位: 有数据可收 */
    bool                      rx_scheduled;   /* 已在 rx_conns 中 (防重复投递) */
    bool                      tx_ready;       /* 回调置位: 有空间可发 */
    bool                      tx_scheduled;
    struct list_head          tx_queue;       /* 待发送请求队列 (流水线) */
    spinlock_t                tx_lock;        /* 保护 tx_queue */

    /* === 回调保存 (竞态处理, 参照 Lustre) === */
    void (*saved_data_ready)(struct sock *);
    void (*saved_write_space)(struct sock *);
    void (*saved_state_change)(struct sock *);
    void (*saved_error_report)(struct sock *);
};
```

### 12.4 回调实现（softirq 安全）

```c
/* 数据到达: softirq 上下文, 仅标记+投递+唤醒 */
static void pfs_data_ready(struct sock *sk)
{
    struct powerfs_net_server_conn *conn;

    read_lock_bh(&g_pool.global_lock);     /* 与 reset_callback 的 write_lock 互斥 */
    conn = sk->sk_user_data;
    if (conn)
        pfs_rx_callback(conn);             /* → rx_ready=1 + list_add + wake_up */
    else
        sk->sk_data_ready(sk);   /* NULL: 拆除中. reset_callback 已在 write_lock 下
                                  * 把 sk_data_ready 恢复为原始回调, 此处调原始回调
                                  * (注意: 此时 sk->sk_data_ready 已非 pfs_data_ready,
                                  *  不会递归. 参照 Lustre socklnd_lib.c:458-461) */
    read_unlock_bh(&g_pool.global_lock);
}

static void pfs_rx_callback(struct powerfs_net_server_conn *conn)
{
    struct powerfs_net_sched *sched = conn->sched;
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

/* 状态变化: 即时断连感知 (TCP_CLOSE_WAIT=peer FIN, TCP_CLOSE=RST) */
static void pfs_state_change(struct sock *sk)
{
    struct powerfs_net_server_conn *conn;
    read_lock_bh(&g_pool.global_lock);
    conn = sk->sk_user_data;
    if (conn) {
        if (sk->sk_state == TCP_CLOSE_WAIT || sk->sk_state == TCP_CLOSE)
            schedule_work(&conn->disconnect_work);   /* process context 清理 */
    }
    read_unlock_bh(&g_pool.global_lock);
}
/* pfs_write_space / pfs_error_report 同理: 标记 tx_ready / 调度 disconnect_work.
 *
 * 锁类型选择 (对照 Lustre socklnd_lib.c):
 * - Lustre ksocknal_data_ready 用 read_lock_bh (softirq 上下文)
 * - Lustre ksocknal_write_space 用 read_lock (process 上下文, 不关软中断)
 * - PowerFS 统一用 read_lock_bh: 更保守但更安全.
 *   原因: sk_write_space 的调用上下文取决于 TCP 栈实现, 在不同内核版本/
 *   场景下可能从 softirq (如 tcp_tasklet) 或 process context 调用.
 *   统一 read_lock_bh 避免对调用上下文做假设, 代价是 write_space 路径
 *   多关一次软中断 (微秒级, 可忽略). 参照 BeeGFS StandardSocket 的做法
 *   (BeeGFS 回调内不区分上下文, 统一处理). */
```

### 12.5 调度器线程（process context，per-CPT）

```c
static int pfs_scheduler_thread(void *arg)
{
    struct powerfs_net_sched *sched = arg;

    spin_lock_bh(&sched->lock);
    while (!atomic_read(&g_pool.stopping)) {
        bool did = false;

        /* 1. 收: 取 rx 就绪连接 */
        struct powerfs_net_server_conn *conn =
            list_first_entry_or_null(&sched->rx_conns, typeof(*conn), rx_list);
        if (conn) {
            list_del_init(&conn->rx_list);
            conn->rx_ready = 0;             /* 清除, 回调可再次置位 */
            spin_unlock_bh(&sched->lock);

            pfs_process_receive(conn);      /* sock_recvmsg → 解帧 → seq 分发 → complete */

            spin_lock_bh(&sched->lock);
            if (conn->rx_ready)             /* 收的过程中又有数据 → 重新挂回 */
                list_add_tail(&conn->rx_list, &sched->rx_conns);
            else {
                conn->rx_scheduled = 0;
                powerfs_conn_put(conn);     /* 释放调度器引用 */
            }
            did = true;
        }

        /* 2. 发: 取 tx 就绪连接 (有空间可写且有积压) */
        conn = list_first_entry_or_null(&sched->tx_conns, typeof(*conn), tx_list);
        if (conn) { /* 同上: pfs_process_transmit(conn) → sock_sendmsg */ did = true; }

        /* 3. 无事可做 → 等待 */
        if (!did) {
            spin_unlock_bh(&sched->lock);
            wait_event_interruptible(sched->waitq, !pfs_sched_cansleep(sched));
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
```

### 12.6 请求生命周期（流水线 + 事件驱动）

```
发送 (VFS 线程, 多个并发):
  req->seq = atomic_inc(&conn->seq_counter);
  req_tree_insert(conn, req);                  /* 入待答树 */
  spin_lock(tx_lock); list_add_tail(&req->tx_list, &conn->tx_queue); spin_unlock(tx_lock);
  pfs_tx_schedule(conn);                        /* 标记 tx_ready + 投递 tx_conns + wake_up */
  wait_for_completion_timeout(&req->done, 30s); /* 等调度器收响应后 complete */

接收 (调度器线程, sk_data_ready 触发):
  pfs_process_receive(conn):
    n = sock_recvmsg(conn->sock, ...);          /* 非阻塞读一帧 */
    if (n <= 0) { schedule_work(&conn->disconnect_work); return; }
    req = req_tree_lookup(conn, hdr.seq);
    req_fill_response(req, &hdr, body, data);
    req_tree_remove(conn, req);
    complete(&req->done);                        /* 唤醒发送线程 */

发送执行 (调度器线程, sk_write_space 触发):
  pfs_process_transmit(conn):
    spin_lock(tx_lock);
    req = list_first_entry(&conn->tx_queue, ...);
    list_del(&req->tx_list);
    spin_unlock(tx_lock);
    ret = kernel_sendmsg(conn->sock, ...);       /* 发一帧 */
    if (ret == -EAGAIN) { 重新挂回 tx_queue; pfs_tx_schedule(conn); }  /* 等可写 */
```

**流水线**：发送线程只入队 + 投递 + 等待，不持锁等响应；调度器线程串行收/发同一连接的帧但跨连接并行。同一连接可有多个 outstanding 请求（tx_queue 积压），调度器批量推进。

### 12.7 断连感知（即时，三重保障）

| 触发 | 机制 | 时延 |
|------|------|------|
| peer 主动关闭 (docker stop/进程退出) | `sk_state_change` → TCP_CLOSE_WAIT | **毫秒级** |
| RST (端口不可达/重置) | `sk_state_change` → TCP_CLOSE | 毫秒级 |
| 静默死亡 (网络分区/panic) | TCP keepalive → `sk_error_report` | ~11s (keepidle=5 + 3×keepintvl=2) |
| 收发中错误 | `sock_recvmsg`/`sendmsg` 返回错误 | 即时 |

`sk_state_change` 回调直接 `schedule_work(&conn->disconnect_work)`，比 v1 的"RX kthread recv 返回错误"更早（状态变化先于数据读取）。

### 12.8 生命周期与竞态处理（参照 Lustre，最关键部分）

回调在 softirq 上下文，conn 拆除在 process 上下文，必须严格处理竞态：

```c
/* 建连: 保存原始回调 + 安装自定义回调 */
void pfs_conn_set_callbacks(struct powerfs_net_server_conn *conn)
{
    struct sock *sk = conn->sock->sk;
    write_lock_bh(&g_pool.global_lock);
    conn->saved_data_ready   = sk->sk_data_ready;
    conn->saved_write_space  = sk->sk_write_space;
    conn->saved_state_change = sk->sk_state_change;
    conn->saved_error_report = sk->sk_error_report;
    sk->sk_user_data = conn;
    sk->sk_data_ready   = pfs_data_ready;
    sk->sk_write_space  = pfs_write_space;
    sk->sk_state_change = pfs_state_change;
    sk->sk_error_report = pfs_error_report;
    write_unlock_bh(&g_pool.global_lock);
}

/* 拆除: 恢复原始回调 + 清 sk_user_data + 等待在飞回调退出 */
void pfs_conn_reset_callbacks(struct powerfs_net_server_conn *conn)
{
    struct sock *sk = conn->sock->sk;
    write_lock_bh(&g_pool.global_lock);
    sk->sk_data_ready   = conn->saved_data_ready;
    sk->sk_write_space  = conn->saved_write_space;
    sk->sk_state_change = conn->saved_state_change;
    sk->sk_error_report = conn->saved_error_report;
    sk->sk_user_data = NULL;
    write_unlock_bh(&g_pool.global_lock);
    /* global_lock 的 write_lock 与回调的 read_lock 互斥,
     * 释放 write_lock 后, 新回调读到 sk_user_data==NULL 走 NOOP (调原始回调).
     * 但已进入回调(持 read_lock)的会安全完成. write_unlock_bh 关软中断,
     * 保证此 CPU 上的回调不会在释放锁后重入. */
}
```

**断连清理流程**（`disconnect_work`，process context）：
1. `pfs_conn_reset_callbacks(conn)` — 恢复回调 + 清 sk_user_data（此后回调 NOOP）。
2. `kernel_sock_shutdown(sock, SHUT_RDWR)` — 唤醒可能在 wait_event 的调度器线程。
3. 从 `sched->rx_conns/tx_conns` 摘除 conn（持 `sched->lock`），清 `rx_scheduled/tx_scheduled`。
4. `set_state(RECONNECTING)` — 触发 route 降级。
5. 唤醒在途请求（`complete` with -ENOTCONN）让其重试其他 filer。
6. `sock_release` + `schedule_delayed_work(reconnect_work)`。
7. 重连成功后 `pfs_conn_set_callbacks` + 投递首个请求触发收发。

**引用计数**：`powerfs_conn_get/put`——回调投递到 `rx_conns` 时 get，调度器处理完 put。拆除时等待引用归零（或调度器已 put）才 `sock_release`。

### 12.9 与第十一章（kthread v1）的关系

| 组件 | v1 (kthread) | v2 (callback+scheduler) | 复用？ |
|------|--------------|--------------------------|--------|
| req_tree / seq / pending_reqs | ✓ | ✓ | **完全复用** |
| wait_for_completion + 超时摘除 | ✓ | ✓ | **完全复用** |
| disconnect_work 幂等清理 | ✓ | ✓ | **复用, 增加回调重置** |
| 接收触发 | per-conn kthread 阻塞 recv | sk_data_ready 回调 + 调度器 | **替换** |
| 断连触发 | recv 返回错误 | sk_state_change 回调 | **替换, 更早** |
| 健康监控 | 已移除 | 已移除 | — |
| keepalive | ✓ | ✓ | **复用** |

**结论**：v2 替换的是"接收触发机制"和"断连触发机制"，请求生命周期/dispatch/重试逻辑完全复用 v1。v1 是 v2 的可行中间态，迁移风险可控。

### 12.10 迁移步骤（v1 → v2）

```
Phase A: 调度器基础设施
├── 新增 powerfs_net_sched (lock/rx_conns/tx_conns/waitq/task)
├── pool_init: 按 num_online_cpus() 创建 schedulers[], kthread_run 启动调度器线程
├── conn 新增 sched/rx_list/tx_list/rx_ready/rx_scheduled/tx_*/saved_* 字段
└── conn 按 addr hash 分配到 sched[cpu % num_online_cpus]

Phase B: 回调安装
├── 实现 pfs_data_ready/pfs_write_space/pfs_state_change/pfs_error_report (softirq 安全)
├── pfs_rx_callback/pfs_tx_callback: 标记+投递+wake_up
├── pfs_conn_set_callbacks / pfs_conn_reset_callbacks (global_lock 互斥)
└── connect_one 成功后 set_callbacks (替换 v1 的 kthread_run)

Phase C: 调度器收发
├── pfs_process_receive: sock_recvmsg → seq 分发 → complete (复用 v1 dispatch)
├── pfs_process_transmit: 取 tx_queue → kernel_sendmsg → EAGAIN 重挂
├── 发送路径: 入 tx_queue + pfs_tx_schedule + wait_for_completion (替换 v1 的 frame_send 直发)
└── 调度器线程主循环 (wait_event + rx/tx 消费)

Phase D: 断连与清理
├── pfs_state_change → schedule_work(disconnect_work) (替换 v1 的 RX recv 错误)
├── disconnect_work: reset_callbacks + 摘 rx/tx_conns + shutdown + 标记重发 + reconnect
└── 引用计数: 投递时 get, 消费完 put, 拆除等归零

Phase E: 移除 v1
├── 移除 rx_thread_fn / kthread_run (per-conn RX)
├── 移除 conn->rx_task 字段
└── disconnect_one 改用 reset_callbacks 路径

Phase F: 验证 (QEMU)
├── 基准: fio QD32 对比 v1, 确认吞吐不降且流水线工作
├── leader 断连: docker stop → sk_state_change 即时 → failover ≤ 500ms (目标优于 v1 的 104ms)
├── 静默死亡: iptables drop → keepalive ~11s → failover
├── 回调竞态: 高频断连+重连循环 100 次, 无 crash/UAF (valgrind/kasan/kmemleak)
├── 重连恢复: 重启 filer → set_callbacks → 收发恢复
└── 多连接压测: 模拟 50+ filer 连接, 确认调度器线程数固定不变
```

### 12.11 验收指标（v2）

- 调度器线程数 = num_cpts（固定，与 filer 数无关）—— 50 连接时仍只有 M 个线程
- leader 断连 failover ≤ 500ms（`sk_state_change` 即时，优于 v1 的 104ms）
- 高频断连重连 100 次无 crash / UAF / kmemleak 告警
- fio QD32 吞吐 ≥ v1（流水线保持）
- 卸载时所有调度器线程干净退出，无回调重入

### 12.12 风险与缓解

| 风险 | 缓解 |
|------|------|
| 回调在 softirq 触发时 conn 正被拆除 | global_lock write/read 互斥 + sk_user_data=NULL NOOP（参照 Lustre） |
| 回调重入（同一 socket 多次 data_ready） | `rx_scheduled` 标志防重复投递（参照 Lustre `ksnc_rx_scheduled`） |
| 调度器持 conn 引用时拆除 | `powerfs_conn_get/put` 引用计数，拆除等归零 |
| socket 卸载后回调仍触发（模块卸载） | reset_callbacks 恢复原始回调（socket 可能比模块长寿，参照 Lustre 注释 530-532） |
| softirq 回调不能睡眠/分配大内存 | 回调仅 set+list_add+wake_up，重活全在调度器 process context |

### 12.13 架构 Review：对照 Lustre/BeeGFS 验证结论

**Review 日期**: 2026-08-04
**对照源码**:
- Lustre: `lustre-release/lnet/klnds/socklnd/socklnd_cb.c` (scheduler), `socklnd_lib.c` (callback 管理)
- BeeGFS: `beegfs/client_module/source/common/net/sock/StandardSocket.c` (回调安装)

#### 12.13.1 对照验证矩阵

| 设计点 | PowerFS v2 | Lustre socklnd | BeeGFS StandardSocket | 结论 |
|--------|-----------|----------------|----------------------|------|
| 回调注册数量 | 4 个 (data_ready/write_space/state_change/error_report) | 2 个 (data_ready/write_space) | 4 个 (同 PowerFS) | **与 BeeGFS 一致, 优于 Lustre** |
| 回调上下文 | softirq, 仅 set+list_add+wake_up | 同 | 同 (wake_up_sync_key) | **一致** |
| `sk_user_data` 解引用保护 | `read_lock_bh(global_lock)` | `read_lock_bh(ksnd_global_lock)` | `read_lock(sk_callback_lock)` | **与 Lustre 一致** |
| `conn==NULL` 时回调行为 | 调 `sk->sk_data_ready(sk)` (原始回调) | 同 (socklnd_lib.c:458-461) | N/A (BeeGFS 不清 sk_user_data) | **与 Lustre 一致** |
| 回调保存/恢复 | 保存 4 个 saved_* | 保存 2 个 (ksnc_saved_*) | 不保存 (直接覆盖) | **比 Lustre 更完整** |
| 防重复投递 | `rx_scheduled`/`tx_scheduled` 标志 | `ksnc_rx_scheduled`/`ksnc_tx_scheduled` | N/A (BeeGFS 用 wait_event) | **与 Lustre 一致** |
| 调度器线程数 | `num_online_cpus()` | `num_cpts` (CPU 分区数) | N/A (BeeGFS 用 per-socket 线程) | **与 Lustre 一致** |
| 调度器主循环 | wait_event + rx/tx 消费 + cond_resched | 同 (socklnd_cb.c:1347-1508) | N/A | **与 Lustre 一致** |
| 调度器持引用 | `powerfs_conn_get/put` | `ksocknal_conn_addref/decref` | N/A | **与 Lustre 一致** |
| 部分接收处理 | 检查 `sk_receive_queue` 是否空 → 设 `rx_ready` | `process_receive` 返回 `rc==0` → 设 `rx_ready=1` | N/A | **等价实现** |
| TX EAGAIN 处理 | 重挂 tx_queue head + 等 `write_space` | 同 (socklnd_cb.c:1455-1461) | N/A | **与 Lustre 一致** |
| 断连感知 | `sk_state_change` + `sk_error_report` + recv 错误 | 仅 recv 错误 (无 state_change 回调) | `sk_state_change` (sock_wakeup) | **与 BeeGFS 一致, 优于 Lustre** |
| keepalive | keepidle=5, intvl=2, cnt=3 | 可配置 tunable | `SO_KEEPALIVE` (StandardSocket_setSoKeepAlive) | **一致** |
| CPU 亲和性绑定 | 暂未实现 | `cfs_cpt_bind` (socklnd_cb.c:1357) | N/A | **后续优化点** |

#### 12.13.2 关键设计决策确认

**1. 4 回调全覆盖 (BeeGFS 式, 优于 Lustre)**

Lustre 只覆盖 `data_ready`/`write_space`, 通过 recv 返回错误检测断连。PowerFS 额外覆盖 `state_change`/`error_report`:
- **优势**: `sk_state_change` 在 TCP 状态变化 (CLOSE_WAIT/CLOSE) 时即时触发, 比 recv 返回错误更早 (状态变化先于数据读取)。leader 断连感知从 v1 的 104ms 可降至毫秒级。
- **代价**: 需处理更多回调竞态, 但已有 `global_lock` + `sk_user_data=NULL NOOP` 机制覆盖。
- **结论**: 设计合理, 与 BeeGFS 一致。

**2. `pfs_write_space` 锁类型 (统一 `read_lock_bh`)**

Lustre 区分: `data_ready` 用 `read_lock_bh` (softirq), `write_space` 用 `read_lock` (process)。
PowerFS 统一用 `read_lock_bh`:
- **原因**: `sk_write_space` 的调用上下文取决于 TCP 栈实现, 在不同内核版本可能从 softirq 或 process context 调用。统一 `read_lock_bh` 避免对调用上下文做假设。
- **代价**: `write_space` 路径多关一次软中断 (微秒级, 可忽略)。
- **结论**: 保守但安全, 参照 BeeGFS 做法。

**3. 部分接收处理 (检查 `sk_receive_queue`)**

Lustre: `process_receive` 返回 `rc==0` → 调度器设 `rx_ready=1` 重新调度。
PowerFS: `pfs_process_receive` 内部检查 `skb_queue_empty(&sk->sk_receive_queue)`, 非空则设 `rx_ready=1`。
- **等价性**: 两者都达到"收完一帧后若缓冲区仍有数据则继续调度"的效果。
- **PowerFS 优势**: 更直接 (直接检查 socket 缓冲区), 不依赖 `process_receive` 返回值语义。
- **结论**: 等价实现, 无问题。

**4. `state_change`/`error_report` 直接 `schedule_work`**

回调在 softirq 上下文, 只调 `schedule_work(&conn->disconnect_work)` (非阻塞, 原子操作)。
- `disconnect_work` 在 process context 执行 `reset_callbacks` + `shutdown` + `kthread_stop` + 唤醒在途请求。
- `schedule_work` 是原子的, 多次调用同一 work_struct 只会排队一次 (幂等)。
- **结论**: 安全, 无竞态。

#### 12.13.3 后续优化点 (非阻塞, 可后续迭代)

| 优化点 | 参照 | 收益 | 优先级 |
|--------|------|------|--------|
| CPU 亲和性绑定 (`cfs_cpt_bind`/`kthread_bind`) | Lustre socklnd_cb.c:1357 | 提高缓存亲和性, 减少 cache miss | 中 |
| TX 批量发送 (多个 req 合并一次 sendmsg) | Lustre `ksocknal_next_tx_carrier` | 减少 syscall 次数, 提高吞吐 | 中 |
| RX 线程级缓冲池 (per-slab, 避免 kmalloc/free) | Lustre `ksocknal_alloc` | 减少分配开销, 适合高吞吐场景 | 低 |
| `write_space` 锁优化 (区分上下文用 `read_lock`) | Lustre socklnd_lib.c:476 | 微秒级优化 | 低 |

#### 12.13.4 总体结论

**架构已确定, 无阻塞问题。** PowerFS v2 通信架构在核心机制上与 Lustre socklnd 完全一致 (回调驱动 + per-CPT 调度器 + 引用计数 + 竞态处理), 在断连感知上借鉴 BeeGFS 做了增强 (4 回调全覆盖)。设计文档与实现代码已对齐, 可推进 QEMU 验证 (Phase F)。

**关键确认:**
- 回调竞态处理: ✓ (global_lock + sk_user_data NOOP, 参照 Lustre)
- 调度器线程模型: ✓ (per-CPT, M 个线程服务 N 连接, 参照 Lustre)
- 请求生命周期: ✓ (seq + req_tree + completion, 参照 v1)
- 断连感知: ✓ (state_change + error_report + keepalive, 参照 BeeGFS)
- 引用计数: ✓ (conn_get/put, 调度器持引用, 参照 Lustre)

---

## 13. 死代码清理计划 (待 UAF 修复后统一处理)

**触发条件**: dentry UAF 问题修复完成, 测试通过后, 在最终代码清理阶段统一执行.

**原则**: 每一项删除前需再次 grep 确认无新引用, 删除后编译通过 + 跑一遍 drop_caches 测试.

### 13.1 Lease 链表机制 (整个机制未启用)

**根因**: `struct powerfs_client` 从未被分配 (`sbi->client` 永远为 NULL), 整个 lease 链表机制是死代码.

| 位置 | 内容 | 状态 |
|------|------|------|
| `powerfs.h:167` `struct powerfs_client` | 整个结构体定义 | 未使用 (从未 kzalloc) |
| `powerfs.h:190` `sbi->client` 字段 | `struct powerfs_client *client` | 未使用 (从未赋值) |
| `powerfs.h:74` `di->lease_list` | `struct list_head lease_list` | 未使用 (从未 list_add) |
| `powerfs.h:77` `di->time` | `unsigned long time` (lease 续约用) | 未使用 |
| `powerfs.h:79` `di->lease_expire` | `unsigned long lease_expire` | 仅在 `d_init` 赋值, `d_revalidate` 读取 (待确认是否保留) |
| `powerfs_fs.c:100` `INIT_LIST_HEAD(&di->lease_list)` | d_init 中初始化 | 死代码 |
| `powerfs_fs.c:123-130` `d_release` lease 移除块 | `if (!list_empty(...))` 整个块 | 死代码 (条件永远 false) |
| `powerfs_fs.c:78` `powerfs_lease_renew_work_func` 前向声明 | lease 续约 work | 死代码 |
| `powerfs_fs.c:?` `powerfs_lease_renew_work` 实现 | 续约逻辑 | 死代码 (从未 schedule) |

**清理动作**:
1. 删除 `struct powerfs_client` 整个结构体
2. 删除 `sbi->client` 字段
3. 删除 `di->lease_list` 字段和 `INIT_LIST_HEAD`
4. 删除 `d_release` 中的 lease 移除块
5. 删除 `powerfs_lease_renew_work_func` 及其前向声明
6. **保留** `di->lease_expire` 和 `di->time` (d_revalidate TTL 检查仍用, 待确认)

### 13.2 `dfi->last_name` (从未分配)

**根因**: `last_name` 字段只在 open 时赋值为 NULL, 从未 kmalloc/kstrdup.

| 位置 | 内容 | 状态 |
|------|------|------|
| `powerfs.h:85` `dfi->last_name` 字段 | `char *last_name` | 未使用 |
| `powerfs_fs.c:1567` `dfi->last_name = NULL` | open 时初始化 | 死代码 |
| `powerfs_fs.c:1596` `kfree(dfi->last_name)` | release 时释放 | `kfree(NULL)` no-op |

**清理动作**: 删除字段声明、初始化、释放.

### 13.3 已删除的 `d_delete` 回调 (已完成)

**说明**: `powerfs_d_delete` 无条件返回 0, 等价于 VFS 默认行为, 已于本次清理删除. 记录在此供回溯.

### 13.4 执行顺序

1. 先完成 dentry UAF 修复 + drop_caches 测试通过
2. 按 13.1 → 13.2 顺序删除
3. 每删一项编译一次, 确认无新引用
4. 全部删除后, 跑一遍 `test_stage2_disconnect.sh` + drop_caches 回归
5. 英文 commit: "kernel: remove dead lease-list and last_name code"

### 13.5 风险评估

| 风险 | 评估 | 缓解 |
|------|------|------|
| 误删仍在用的字段 | 低 (已 grep 确认) | 删除前再次 grep, 编译验证 |
| `lease_expire` 误删 | 中 (d_revalidate 在用) | 单独保留, 不随 lease_list 一起删 |
| 行为变化 | 无 (都是 no-op 代码) | drop_caches 回归测试确认 |
