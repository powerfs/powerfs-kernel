# PowerFS 流量控制与连接管理方案设计

> 状态: 设计中
> 创建: 2026-08-07
> 关联: network-architecture.md, lease-design.md
> 协议基线: v3.2-protocol-v2

## 1. 背景与目标

### 1.1 问题

当前 PowerFS 在高负载下存在以下问题:

- **无入口限流**: read/write 直接进 VFS 回调, 并发无限制. 负载超过网络/服务能力时,
  请求堆积 → 调度器队列爆满 → TX/RX 互相阻塞 → 雪崩
- **无系统化统计**: 仅有分散的 `SLOW_RX` / `SLOW_REQ` 日志, 无 per-conn 流量/延迟统计,
  无全局汇总, 排查问题靠 grep dmesg
- **无慢连接标记**: 异常慢的连接无法自动识别和标记, 混在正常连接中, 难以定位
- **服务端无统一流控视图**: 已有 `RateLimiter`(per-conn) / `CircuitBreaker`(FUSE 客户端) /
  `RequestPipeline`(middleware), 但分散在多个文件, 无统一汇总, 无图形化管理接口

### 1.2 目标

1. **流量与时间统计**: per-conn + 全局的收发字节/请求数/延迟分布/错误数
2. **入口限流**: 客户端 read/write/lookup 在 VFS 回调入口排队 (page lock 之前),
   有序流控而非压垮
3. **慢连接标记**: 自动识别延迟异常的连接, 标记 + 告警 + 可选降级
4. **服务端为中心**: 服务端汇总所有连接状态, 暴露 HTTP API + Prometheus metrics,
   支持图形化管理
5. **独立模块**: 流控逻辑集中, 操作集 + 数据结构抽象, 策略可插拔, 代码不分散
6. **客户端/服务器联动** (Phase 2): 服务端反馈 `load_factor`, 客户端据此自适应调整

### 1.3 设计约束

- kernel 侧网络 I/O 必须在 VFS 锁之外完成 (project_memory 约束)
- 排队必须在 `inode_lock()` / `folio_lock()` **之前**, 不持任何 VFS 锁等待
- 协议变更需谨慎 (刚打 `v3.2-protocol-v2` tag), Phase 1 不动协议
- kernel 修改需在 QEMU 内测试, 从简单到复杂验证
- 服务端测试在容器内, 用 fio/io500 标准测试

---

## 2. 现状分析

### 2.1 客户端 (kernel) 侧

| 已有机制 | 位置 | 说明 |
|---------|------|------|
| writeback `max_active=4` | `powerfs_fs.c:3849` | workqueue 级并发限制, 仅 writeback 路径 |
| `SLOW_RX` 日志 | `powerfs_net.c:2029` | 响应延迟 >100ms 打 info, 分散, 无统计 |
| `SLOW_REQ` 日志 | `powerfs_net.c:2998` | 请求超时打 info, 分散, 无统计 |
| TX 非阻塞 send | `powerfs_net.c` `pfs_frame_send_nonblock` | MSG_DONTWAIT + send_offset, 防阻塞但不限流 |
| RX 非阻塞状态机 | `powerfs_net.c` `pfs_rx_step` | per-conn partial 续收, 防阻塞但不限流 |

**缺失**: 系统化统计、入口 admission、debugfs 接口、慢连接标记、背压传递

### 2.2 服务端 (Rust) 侧

| 已有机制 | 位置 | 说明 |
|---------|------|------|
| `CircuitBreaker` | `powerfs-fuse-core/src/circuit_breaker.rs` | 熔断器 (Closed/Open/HalfOpen), **FUSE 客户端用**, 非服务端 |
| `RateLimiter` | `powerfs-net/src/client_conn.rs:205` | per-conn 令牌桶限流, `check_rate_limit()` |
| `RequestPipeline` | `powerfs-net/src/middleware.rs` | 中间件链, `with_concurrent_rate_limit()` |
| metrics 端点 | `powerfs-{master,volume}/src/metrics.rs` | Prometheus 风格, 但无流控指标 |
| dynamic_log | `powerfs-common/src/dynamic_log.rs` | 运行时日志级别, 可复用模式 |

**缺失**: per-conn 流量/延迟统计、全局汇总、慢连接标记、流控 HTTP API、图形化接口

### 2.3 集成点

**客户端侧集成点**:
- VFS 回调入口: `powerfs_read` / `powerfs_write_iter` / `powerfs_lookup` / `powerfs_readdir`
  → admission check (page lock / inode_lock 之前)
- 网络层: `powerfs_request_do_send` 入口 / `pfs_rx_dispatch` 完成 → record 统计
- writeback: `powerfs_writepage_work_fn` → 整合 max_active 进 flow controller

**服务端侧集成点**:
- IoLoop 入口: `io_loop.rs` 收到请求后 → admit check
- Worker 处理完成: `worker.rs` → record 统计
- 响应发送: 帧构建时附带 load_factor (Phase 2)

---

## 3. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│  服务端 (Rust) —— 流控中心 + 汇总 + 图形化管理               │
│  ┌────────────────────────────────────────────────────────┐ │
│  │ powerfs-net/src/flow_control.rs (独立模块)             │ │
│  │  ├── ConnStats        per-conn 统计 (bytes/reqs/lat)   │ │
│  │  ├── GlobalStats      全局汇总                         │ │
│  │  ├── SlowConnTracker   EWMA 延迟跟踪 + 慢连接标记       │ │
│  │  ├── FlowPolicy       可插拔策略 (trait)                │ │
│  │  └── FlowController   统一入口: admit/record/report    │ │
│  └────────────────────────────────────────────────────────┘ │
│  IoLoop/Worker → FlowController.admit() / record()          │
│  HTTP API: /admin/flow/connections, /admin/flow/stats       │
│  Prometheus: powerfs_flow_*                                 │
└──────────────────┬──────────────────────────────────────────┘
                   │ load_factor (Phase 2, 响应帧)
                   ▼
┌─────────────────────────────────────────────────────────────┐
│  客户端 (kernel) —— 入口流控 + 自适应                        │
│  ┌────────────────────────────────────────────────────────┐ │
│  │ powerfs_flow.c + powerfs_flow.h (独立模块)             │ │
│  │  ├── powerfs_flow_admit()    VFS 入口排队              │ │
│  │  ├── powerfs_flow_record()   网络层统计更新            │ │
│  │  ├── powerfs_flow_load()     读服务端 load_factor (P2) │ │
│  │  └── debugfs 接口            本地状态查看              │ │
│  └────────────────────────────────────────────────────────┘ │
│  read/write/lookup → admit() → 排队(不持锁) → 放行          │
│  网络收发 → record() 更新统计                               │
│  load_factor 高 → admission 阈值收紧 (Phase 2)              │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. 服务端流控中心设计 (Phase 1)

### 4.1 模块结构

```
powerfs-net/src/flow_control.rs   — 核心模块 (所有流控逻辑集中)
powerfs-net/src/flow_policy.rs    — 策略实现 (可插拔)
powerfs-volume/src/metrics.rs     — HTTP API + Prometheus 集成
powerfs-master/src/metrics.rs     — HTTP API + Prometheus 集成
```

### 4.2 核心数据结构

```rust
// flow_control.rs

/// 单连接统计 (原子操作, 无锁读)
pub struct ConnStats {
    pub conn_id: u64,
    pub peer_addr: String,
    pub channel: u8,  // CHANNEL_DATA / CHANNEL_META

    // 流量计数
    pub bytes_sent: AtomicU64,
    pub bytes_recv: AtomicU64,
    pub reqs_sent: AtomicU64,   // 响应数
    pub reqs_recv: AtomicU64,   // 请求数
    pub reqs_err: AtomicU64,

    // 延迟统计 (EWMA, 无需历史窗口)
    pub lat_ewma_us: AtomicU64,      // 指数加权移动平均 (微秒)
    pub lat_max_us: AtomicU64,       // 近期最大延迟
    pub slow_count: AtomicU64,       // 慢请求计数 (> slow_threshold)

    // 慢连接标记
    pub slow: AtomicBool,            // 是否标记为慢连接
    pub slow_since: AtomicU64,       // 标记为慢的时间戳 (ns)

    // 在途请求
    pub active_reqs: AtomicU32,      // 当前在途请求数
}

/// 全局汇总
pub struct GlobalStats {
    pub total_bytes_sent: AtomicU64,
    pub total_bytes_recv: AtomicU64,
    pub total_reqs: AtomicU64,
    pub total_errs: AtomicU64,
    pub active_reqs: AtomicU32,      // 全局在途请求数
    pub active_conns: AtomicU32,     // 活跃连接数
    pub slow_conns: AtomicU32,       // 慢连接数

    // 延迟分位数 (粗粒度直方图, 适合 Prometheus)
    pub lat_buckets: [AtomicU64; 6], // <=1ms, <=10ms, <=100ms, <=1s, <=10s, >10s
}

/// 慢连接跟踪器
pub struct SlowConnTracker {
    slow_threshold_us: u64,      // 慢阈值 (默认 100ms)
    ewma_alpha: u8,              // EWMA 平滑系数 (0-100, 默认 20 = 0.2)
    recovery_threshold_us: u64,  // 恢复阈值 (默认 10ms, 低于此值解除慢标记)
    recovery_count: u32,         // 连续 N 次低于恢复阈值才解除 (默认 10)
}

/// 流控策略 trait (可插拔)
pub trait FlowPolicy: Send + Sync {
    /// 准入决策: 是否允许新请求
    fn admit(&self, ctx: &FlowCtx) -> AdmissionDecision;

    /// 记录请求完成
    fn record(&self, ctx: &FlowCtx, latency_us: u64, bytes: u64, err: bool);

    /// 当前负载因子 (0-255, Phase 2 用)
    fn load_factor(&self) -> u8;

    /// 策略名称
    fn name(&self) -> &'static str;
}

pub enum AdmissionDecision {
    Admit,
    Reject { reason: RejectReason },
}

pub enum RejectReason {
    ConnFull,        // per-conn 并发上限
    GlobalFull,      // 全局并发上限
    SlowConn,        // 慢连接限流
    RateLimited,     // 令牌桶耗尽
}

pub struct FlowCtx<'a> {
    pub conn_id: u64,
    pub msg_type: u16,
    pub est_bytes: usize,   // 预估请求字节数
    pub stats: &'a ConnStats,
}

/// 流控控制器 (统一入口)
pub struct FlowController {
    global: GlobalStats,
    conns: DashMap<u64, ConnStats>,   // conn_id → stats
    slow_tracker: SlowConnTracker,
    policy: Arc<dyn FlowPolicy>,
}
```

### 4.3 默认策略: 自适应并发上限

```rust
// flow_policy.rs

pub struct AdaptiveConcurrencyPolicy {
    max_active_global: AtomicU32,   // 全局最大并发 (默认 256)
    max_active_per_conn: AtomicU32, // per-conn 最大并发 (默认 64)
    // Phase 2: 根据 load_factor 动态调整上述上限
}

impl FlowPolicy for AdaptiveConcurrencyPolicy {
    fn admit(&self, ctx: &FlowCtx) -> AdmissionDecision {
        // 1. 慢连接限流: 慢连接的并发减半
        if ctx.stats.slow.load(Relaxed) {
            let limit = self.max_active_per_conn.load(Relaxed) / 2;
            if ctx.stats.active_reqs.load(Relaxed) >= limit {
                return Reject { reason: SlowConn };
            }
        }
        // 2. per-conn 并发上限
        if ctx.stats.active_reqs.load(Relaxed) >= self.max_active_per_conn.load(Relaxed) {
            return Reject { reason: ConnFull };
        }
        // 3. 全局并发上限
        if ctx.global_active.load(Relaxed) >= self.max_active_global.load(Relaxed) {
            return Reject { reason: GlobalFull };
        }
        Admit
    }

    fn record(&self, ctx: &FlowCtx, latency_us: u64, bytes: u64, err: bool) {
        // 更新 ConnStats
        ctx.stats.active_reqs.fetch_sub(1, Relaxed);
        if err { ctx.stats.reqs_err.fetch_add(1, Relaxed); }
        // EWMA 延迟更新
        update_ewma(&ctx.stats.lat_ewma_us, latency_us, alpha);
        // 慢连接标记/解除
        slow_tracker.evaluate(&ctx.stats, latency_us);
        // 全局统计
        global.lat_buckets[bucket_index(latency_us)].fetch_add(1, Relaxed);
    }
}
```

### 4.4 EWMA 慢连接标记算法

```rust
impl SlowConnTracker {
    pub fn evaluate(&self, stats: &ConnStats, latency_us: u64) {
        let ewma = stats.lat_ewma_us.load(Relaxed);
        let was_slow = stats.slow.load(Relaxed);

        if !was_slow && ewma > self.slow_threshold_us {
            // 标记为慢
            stats.slow.store(true, Relaxed);
            stats.slow_since.store(ktime_get_ns(), Relaxed);
            stats.slow_count.fetch_add(1, Relaxed);
            log::warn!(
                "FlowControl: conn {} marked SLOW (ewma={}us > {}us)",
                stats.conn_id, ewma, self.slow_threshold_us
            );
        } else if was_slow && ewma < self.recovery_threshold_us {
            // 连续 N 次低于恢复阈值才解除
            let n = stats.recovery_counter.fetch_add(1, Relaxed) + 1;
            if n >= self.recovery_count {
                stats.slow.store(false, Relaxed);
                stats.recovery_counter.store(0, Relaxed);
                log::info!("FlowControl: conn {} recovered from SLOW", stats.conn_id);
            }
        } else if was_slow {
            // 仍慢, 重置恢复计数
            stats.recovery_counter.store(0, Relaxed);
        }
    }
}
```

### 4.5 HTTP API + Prometheus

```
GET  /admin/flow/connections
     → JSON: [{ conn_id, peer_addr, channel, active_reqs, lat_ewma_us, slow, bytes_sent, bytes_recv, ... }]

GET  /admin/flow/stats
     → JSON: { total_bytes_sent, total_bytes_recv, active_reqs, active_conns, slow_conns, lat_buckets }

GET  /admin/flow/policy
     → JSON: { name, max_active_global, max_active_per_conn, slow_threshold_us }

PUT  /admin/flow/policy?max_active_global=512&slow_threshold_us=50000
     → 运行时调整阈值 (类似 dynamic_log 模式)

GET  /metrics (Prometheus)
     → powerfs_flow_conn_bytes_total{conn_id,dir="send|recv"}
     → powerfs_flow_conn_latency_ewma_us{conn_id}
     → powerfs_flow_conn_active_reqs{conn_id}
     → powerfs_flow_conn_slow{conn_id}
     → powerfs_flow_global_active_reqs
     → powerfs_flow_global_slow_conns
     → powerfs_flow_latency_bucket{le="0.001|0.01|0.1|1|10|+Inf"}
```

---

## 5. 客户端入口流控设计 (Phase 1)

### 5.1 模块结构

```
kernel/powerfs_mod/powerfs_flow.h   — 数据结构 + 接口声明
kernel/powerfs_mod/powerfs_flow.c   — 核心实现
kernel/powerfs_mod/powerfs_fs.c     — VFS 回调集成 (调用 admit/record)
kernel/powerfs_mod/powerfs_net.c    — 网络层集成 (调用 record)
```

### 5.2 核心数据结构

```c
// powerfs_flow.h

enum powerfs_flow_op {
    FLOW_OP_READ,
    FLOW_OP_WRITE,
    FLOW_OP_LOOKUP,
    FLOW_OP_READDIR,
    FLOW_OP_WRITEBACK,
    FLOW_OP_LEASE,
    FLOW_OP_NUM,
};

enum powerfs_flow_decision {
    FLOW_ADMIT,     /* 立即放行 */
    FLOW_QUEUE,     /* 排队等待 */
    FLOW_REJECT,    /* 拒绝 (-EAGAIN) */
};

/* per-conn 统计 (kernel 侧, 对应服务端 ConnStats) */
struct powerfs_flow_conn_stats {
    atomic64_t bytes_sent;
    atomic64_t bytes_recv;
    atomic64_t reqs_sent;
    atomic64_t reqs_err;
    atomic64_t lat_ewma_ns;     /* EWMA 延迟 (纳秒) */
    atomic64_t lat_max_ns;
    atomic_t   active_reqs;     /* 在途请求数 */
    bool       slow;            /* 慢连接标记 */
    u64        slow_since;
};

/* 全局统计 */
struct powerfs_flow_global {
    atomic64_t total_bytes;
    atomic64_t total_reqs;
    atomic_t   active_reqs;     /* 全局在途 */
    atomic_t   queued_reqs;     /* 排队等待 */
    /* 延迟直方图 */
    atomic64_t lat_buckets[6];  /* <=1ms, <=10ms, <=100ms, <=1s, <=10s, >10s */
};

/* 流控控制器 */
struct powerfs_flow_controller {
    struct powerfs_flow_global global;
    struct powerfs_flow_conn_stats conn_stats[MAX_CONNS]; /* 索引 = conn 数组下标 */

    /* 准入配置 (可运行时调) */
    atomic_t max_active_global;     /* 默认 256 */
    atomic_t max_active_per_conn;   /* 默认 64 */
    atomic_t queue_limit;           /* 排队上限, 默认 512 */
    u32      slow_threshold_us;     /* 默认 100ms */
    u32      queue_timeout_ms;      /* 排队超时, 默认 5000ms */

    /* 排队 waitq */
    wait_queue_head_t admit_wq;

    /* Phase 2: 服务端负载因子 */
    atomic_t load_factor;   /* 0-255, 0=空闲, 255=满载 */
};

/* 策略操作集 (可插拔) */
struct powerfs_flow_policy {
    enum powerfs_flow_decision (*admit)(struct powerfs_flow_controller *fc,
                                        enum powerfs_flow_op op,
                                        int conn_idx, size_t est_bytes);
    void (*record)(struct powerfs_flow_controller *fc,
                   enum powerfs_flow_op op,
                   int conn_idx, u64 lat_ns, size_t bytes, int err);
    void (*on_load_factor)(struct powerfs_flow_controller *fc,
                           u8 load_factor);  /* Phase 2 */
    const char *name;
};
```

### 5.3 入口排队 (VFS 回调集成)

```c
// powerfs_fs.c - read 路径集成示例

static ssize_t powerfs_read(struct file *file, char __user *buf,
                            size_t count, loff_t *ppos)
{
    int conn_idx = powerfs_get_conn_idx(inode);  /* 目标连接索引 */
    enum powerfs_flow_decision d;
    ssize_t ret;
    u64 ts;

    /* === admission check: 在 inode_lock / folio_lock 之前 === */
    d = powerfs_flow_admit(g_flow, FLOW_OP_READ, conn_idx, count);
    if (d == FLOW_REJECT)
        return -EAGAIN;
    if (d == FLOW_QUEUE) {
        /* 排队等待, 不持任何 VFS 锁 */
        ret = wait_event_interruptible_timeout(g_flow->admit_wq,
            powerfs_flow_admit(g_flow, FLOW_OP_READ, conn_idx, count) == FLOW_ADMIT,
            msecs_to_jiffies(g_flow->queue_timeout_ms));
        if (ret <= 0)
            return ret ? ret : -EAGAIN;
    }

    ts = ktime_get_ns();

    /* === 正常 read 路径 (持锁 + 网络 I/O) === */
    ret = powerfs_do_read(file, buf, count, ppos);

    /* === 记录统计 (锁外) === */
    powerfs_flow_record(g_flow, FLOW_OP_READ, conn_idx,
                        ktime_get_ns() - ts, count, ret < 0);

    /* 唤醒排队者 */
    wake_up(&g_flow->admit_wq);

    return ret;
}
```

**关键点**:
- `powerfs_flow_admit()` 返回 QUEUE 时, 在 `admit_wq` 上 `wait_event_interruptible_timeout`
- 等待期间**不持任何 VFS 锁** (inode_lock / folio_lock 都还没获取)
- 放行后才进入正常路径获取锁 + 网络 I/O
- 完成后 `powerfs_flow_record()` 更新统计 + `wake_up` 唤醒下一个排队者
- writeback 路径整合: `max_active=4` 由 flow controller 统一管理, 不再单独 workqueue 级限制

### 5.4 默认策略: 自适应并发上限

```c
// powerfs_flow.c

static enum powerfs_flow_decision
default_admit(struct powerfs_flow_controller *fc,
              enum powerfs_flow_op op, int conn_idx, size_t est_bytes)
{
    struct powerfs_flow_conn_stats *cs = &fc->conn_stats[conn_idx];
    int active, max_per_conn, max_global, queued;

    /* Phase 2: 根据 load_factor 动态收紧上限 */
    max_per_conn = atomic_read(&fc->max_active_per_conn);
    max_global = atomic_read(&fc->max_active_global);

    /* load_factor 高时收紧 (Phase 2) */
    /* u8 lf = atomic_read(&fc->load_factor);
     * max_per_conn = max_per_conn * (255 - lf) / 255; */

    /* 1. 慢连接: 并发减半 */
    if (cs->slow)
        max_per_conn /= 2;

    /* 2. per-conn 并发上限 */
    active = atomic_read(&cs->active_reqs);
    if (active >= max_per_conn)
        goto queue;

    /* 3. 全局并发上限 */
    active = atomic_read(&fc->global.active_reqs);
    if (active >= max_global)
        goto queue;

    /* 放行: 递增计数 */
    atomic_inc(&cs->active_reqs);
    atomic_inc(&fc->global.active_reqs);
    return FLOW_ADMIT;

queue:
    queued = atomic_read(&fc->global.queued_reqs);
    if (queued >= atomic_read(&fc->queue_limit))
        return FLOW_REJECT;  /* 排队也满了, 直接拒绝 */
    atomic_inc(&fc->global.queued_reqs);
    return FLOW_QUEUE;
}

static void
default_record(struct powerfs_flow_controller *fc,
               enum powerfs_flow_op op, int conn_idx,
               u64 lat_ns, size_t bytes, int err)
{
    struct powerfs_flow_conn_stats *cs = &fc->conn_stats[conn_idx];

    /* 递减在途计数 */
    atomic_dec(&cs->active_reqs);
    atomic_dec(&fc->global.active_reqs);
    atomic_dec(&fc->global.queued_reqs);  /* 如果是排队放行的 */

    /* 流量统计 */
    atomic64_add(bytes, &cs->bytes_sent);
    atomic64_add(bytes, &fc->global.total_bytes);
    if (err) atomic64_inc(&cs->reqs_err);
    atomic64_inc(&fc->global.total_reqs);

    /* EWMA 延迟 + 慢连接标记 */
    update_ewma(&cs->lat_ewma_ns, lat_ns);
    evaluate_slow(cs, fc->slow_threshold_us, lat_ns);

    /* 全局直方图 */
    atomic64_inc(&fc->global.lat_buckets[lat_bucket_index(lat_ns)]);
}
```

### 5.5 debugfs 接口

```
/sys/kernel/debug/powerfs/flow/
├── global          # 全局统计 (active_reqs, queued, total_bytes, lat_buckets)
├── connections     # 所有连接状态 (conn_idx, addr, active, ewma, slow, bytes)
├── policy          # 当前策略和阈值
├── load_factor     # 服务端负载因子 (Phase 2)
└── slow_log        # 慢连接事件日志 (最近 N 条)
```

运行时调阈值:
```bash
echo 512 > /sys/kernel/debug/powerfs/flow/max_active_global
echo 50000 > /sys/kernel/debug/powerfs/flow/slow_threshold_us  # 50ms
```

---

## 6. Phase 2: load_factor 联动

### 6.1 协议扩展 (向后兼容)

在现有帧头 `flags` 字段中复用 2 bit 编码 `load_factor` 粗粒度等级:

| flags bit | 含义 | load_factor 范围 |
|-----------|------|-----------------|
| bit 6-7 = 00 | 空闲 | 0.0 - 0.25 |
| bit 6-7 = 01 | 正常 | 0.25 - 0.50 |
| bit 6-7 = 10 | 较忙 | 0.50 - 0.75 |
| bit 6-7 = 11 | 满载 | 0.75 - 1.0 |

**优点**: 不增加帧长, 向后兼容 (旧客户端忽略这 2 bit, 旧服务端填 0).

### 6.2 客户端自适应

```c
// Phase 2: 收到响应时更新 load_factor
void powerfs_flow_on_load_factor(struct powerfs_flow_controller *fc,
                                 int conn_idx, u8 lf_level)
{
    /* lf_level: 0-3 (对应 flags bit 6-7) */
    atomic_set(&fc->conn_stats[conn_idx].load_factor, lf_level);

    /* 自适应调整: lf 高时收紧并发上限 */
    /* 全局 load_factor = max(所有 conn 的 load_factor) */
    u8 global_lf = powerfs_flow_max_load_factor(fc);
    int base = FLOW_DEFAULT_MAX_PER_CONN;
    int adjusted = base * (4 - global_lf) / 4;  /* lf=0→base, lf=3→base/4 */
    atomic_set(&fc->max_active_per_conn, adjusted);

    if (global_lf >= 3)
        log_info("FlowControl: server overload (lf=3), throttling to %d", adjusted);
}
```

### 6.3 服务端 load_factor 计算

```rust
fn compute_load_factor(&self) -> u8 {
    let active = self.global.active_reqs.load(Relaxed);
    let max = self.policy.max_active_global.load(Relaxed);
    let ratio = active as f64 / max as f64;
    // 0.0-1.0 → 0-3 等级
    match ratio {
        r if r < 0.25 => 0,
        r if r < 0.50 => 1,
        r if r < 0.75 => 2,
        _ => 3,
    }
}
```

---

## 7. 实施阶段

### Phase 1: 各自单边实现 (无协议变更)

#### 7.1.1 服务端 (Rust)

| 步骤 | 内容 | 验证 |
|------|------|------|
| S1 | `flow_control.rs` + `flow_policy.rs`: ConnStats / GlobalStats / SlowConnTracker / FlowController | 单元测试 |
| S2 | AdaptiveConcurrencyPolicy 实现 | 单元测试 |
| S3 | IoLoop/Worker 集成 admit/record | 容器内功能测试 |
| S4 | HTTP API: /admin/flow/* 端点 | curl 验证 |
| S5 | Prometheus metrics: powerfs_flow_* | curl /metrics 验证 |
| S6 | fio 压测 + 慢连接标记验证 | fio + 日志检查 |

#### 7.1.2 客户端 (kernel)

| 步骤 | 内容 | 验证 |
|------|------|------|
| K1 | `powerfs_flow.h` + `powerfs_flow.c`: 数据结构 + admit/record | 编译通过 |
| K2 | 默认策略 (自适应并发上限) + EWMA 慢连接标记 | 单元逻辑验证 |
| K3 | VFS 集成: read/write/lookup/readir 入口 admit | QEMU 简单读写 |
| K4 | writeback 整合 max_active 进 flow controller | QEMU 1MB/10MB 写 |
| K5 | debugfs 接口 | cat debugfs 验证 |
| K6 | 高并发压测 (fio in QEMU) | fio + debugfs 统计对照 |

#### 7.1.3 集成测试

| 步骤 | 内容 | 验证 |
|------|------|------|
| I1 | 客户端 + 服务端联调 (各自单边流控) | 容器服务 + QEMU 客户端 |
| I2 | fio 高并发压测 (随机读写 4K-1M) | 吞吐 + 延迟 + 无雪崩 |
| I3 | 慢连接模拟 (tc netem 延迟注入) | 慢连接标记 + 告警 |
| I4 | io500 回归 | 分数不退化 |

### Phase 2: load_factor 联动

| 步骤 | 内容 | 验证 |
|------|------|------|
| P2.1 | 服务端 load_factor 计算 + flags 编码 | 单元测试 |
| P2.2 | 客户端 load_factor 解析 + 自适应调整 | QEMU 测试 |
| P2.3 | 服务端过载模拟 (fio 压垮) → 客户端自动降速 | 延迟 + 吞吐监控 |
| P2.4 | fio + io500 回归 | 无退化 |

### Phase 3: 自适应策略 + dashboard (未来)

| 步骤 | 内容 |
|------|------|
| P3.1 | AIMD / BBR 风格自适应策略 (替代固定阈值) |
| P3.2 | Grafana dashboard (基于 Prometheus metrics) |
| P3.3 | 多策略支持 (按 msg_type / volume 分策略) |

---

## 8. 测试计划

### 8.1 服务端测试 (容器内)

```bash
# 1. 功能测试: HTTP API
curl http://volume-1:8080/admin/flow/connections | jq
curl http://volume-1:8080/admin/flow/stats | jq

# 2. 限流测试: 压到上限后观察 Reject
fio --name=flow --filename=/mnt/pfs/test --bs=1M --size=1G \
    --ioengine=libaio --iodepth=256 --numjobs=16 --rw=randwrite

# 3. 慢连接模拟: tc 注入延迟
tc qdisc add dev eth0 root netem delay 500ms
# 观察 /admin/flow/connections 中 slow=true

# 4. Prometheus metrics
curl http://volume-1:8080/metrics | grep powerfs_flow
```

### 8.2 客户端测试 (QEMU 内)

```bash
# 1. debugfs 查看
cat /sys/kernel/debug/powerfs/flow/global
cat /sys/kernel/debug/powerfs/flow/connections

# 2. 高并发读写 (验证排队 + 不雪崩)
fio --name=flow --filename=/mnt/pfs/test --bs=4k --size=100M \
    --ioengine=libaio --iodepth=128 --numjobs=8 --rw=randrw

# 3. 慢连接模拟 (服务端 tc 延迟)
# 观察 debugfs connections 中 slow=Y

# 4. io500 回归
cd /io500 && ./io500.sh config.ini
```

### 8.3 验证标准

- **不雪崩**: 高并发下系统不崩溃, 延迟可控 (p99 < 10s)
- **限流生效**: active_reqs 到达上限后 queued_reqs 增长, 不无限堆积
- **慢连接标记**: 延迟注入后 5s 内标记 slow=true, 恢复后 30s 内解除
- **可观测性**: HTTP API + debugfs + Prometheus 数据一致
- **无退化**: io500 分数不低于 v3.2-protocol-v2 基线

---

## 9. 代码组织原则

1. **集中**: 所有流控逻辑在 `flow_control.rs` (服务端) / `powerfs_flow.c` (客户端), 不散落到其他文件
2. **薄集成层**: 调用方 (VFS 回调 / IoLoop / Worker) 只调 2 个入口:
   - `admit()` — 准入决策
   - `record()` — 记录统计
3. **策略可插拔**: 通过 trait (Rust) / ops struct (kernel) 切换策略, 默认 AdaptiveConcurrency
4. **调试友好**: debugfs (客户端) + HTTP API (服务端) 暴露完整状态, 无需 grep 日志
5. **无锁优先**: 统计用 atomic, 策略决策用 RCU/atomic read, 避免热路径加锁

---

## 附录 A: 拒绝处理

客户端 `admit()` 返回 `FLOW_REJECT` 时:
- read/write: 返回 `-EAGAIN` (应用可重试)
- lookup/readdir: 返回 `-EAGAIN` (VFS 上层重试)
- writeback: 延迟重试 (redirty page)

服务端 `admit()` 返回 `Reject` 时:
- 返回 `BUSY` 状态码 (Phase 1: 复用现有错误码; Phase 2: 专用状态码)
- 客户端收到 BUSY 后退避重试 (指数退避, 上限 1s)

## 附录 B: 配置默认值

| 参数 | 默认值 | 说明 |
|------|--------|------|
| max_active_global | 256 | 全局最大并发请求数 |
| max_active_per_conn | 64 | per-conn 最大并发 |
| queue_limit | 512 | 全局排队上限 |
| queue_timeout_ms | 5000 | 排队超时 |
| slow_threshold_us | 100000 (100ms) | EWMA 超此值标记慢 |
| recovery_threshold_us | 10000 (10ms) | EWMA 低于此值开始恢复计数 |
| recovery_count | 10 | 连续 N 次低于恢复阈值才解除慢标记 |
| ewma_alpha | 20 (0.2) | EWMA 平滑系数 |

所有参数运行时可通过 HTTP API (服务端) / debugfs (客户端) 调整.
