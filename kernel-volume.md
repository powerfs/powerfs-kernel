# 内核直连 Volume Server 方案

## 1. 背景与目标

### 问题
当前内核 `powerfs_net_write` / `powerfs_net_read` 通过 Filer 转发数据，违反架构设计：
- Filer 只应管元数据，不应转发数据
- 将来 Filer 和 Volume Server 部署在不同节点，数据经 Filer 转发增加延迟和带宽消耗
- Filer 成为数据吞吐瓶颈

### 目标
- 内核客户端直连 Volume Server 读写数据（WriteNeedle / ReadNeedle）
- Filer 只负责元数据（Lookup / GetAttr / Create / SetAttr）和 chunk 映射管理
- Master 负责全局拓扑、**Zone 管理**、volume 状态汇总
- **Filer 不直接跟 Volume Server 通信**（Volume 宕机不影响 Filer）
- **Volume Server 只管数据读写**（兼容将来 NVMe-oF target 设备）
- 数据请求处理与元数据请求处理分离（独立调度器线程池）

## 2. 架构设计

### 2.1 组件职责

| 组件 | 职责 | 不参与 |
|------|------|--------|
| Master | 全局拓扑、**Zone 分配与管理**、volume 状态汇总、**Zone→物理 volume 映射表** | 文件元数据、数据路径、needle_id 分配 |
| Filer | 文件元数据（inode/dentry）、**chunk 映射管理**、**needle_id 分配**（Zone 内自管理）、Zone 注册 | 数据转发、与 Volume Server 通信 |
| Volume Server | 数据存储（needle）、只管 WriteNeedle/ReadNeedle | 元数据管理、needle_id 分配、Zone 概念（兼容 NVMe-oF target） |
| 内核客户端 | 直连 Volume Server 读写数据、直连 Filer 获取元数据、直连 Master 获取拓扑 | — |

### 2.2 Zone 概念（核心设计）

**Zone 是 Filer 的"自治领地"**：每个 Filer 拥有一个或多个 Zone，在 Zone 内完全自治地分配 needle_id，不需要跟 Master 频繁通信。

```
Zone (虚拟 volume / 逻辑分区):
  - zone_id: 全局唯一 (Master 分配, 24 bits)
  - owner_filer: 归属 Filer 节点
  - physical_volumes: 映射到的物理 volume 列表 [(volume_id, vol_addr, size, used)]
  - needle_id 空间: [zone_id << 40, (zone_id+1) << 40)

Physical Volume:
  - volume_id, vol_addr, size, used (Volume Server 上的实际存储)
  - 可被多个 Zone 共享 (通过 needle_id 编码区分)

Master 映射表:
  zone_id → { owner_filer, [physical_volume_ids] }
```

### 2.3 needle_id 编码（核心设计）

```
needle_id = (zone_id << 40) | counter
           ├─ 24 bits ─┤├─ 40 bits ─┤
           zone_id        counter (zone 内自增)
```

- **zone_id**: 24 bits，最多 1677 万个 Zone
- **counter**: 40 bits，每个 Zone 最多 1 万亿 needle
- 不同 Zone 的 needle_id **天然不冲突**
- 一个物理 volume 可被多个 Zone 共享（needle_id 编码区分）
- needle_id 编码对 Volume Server 完全透明（只是一个 u64 key）

### 2.4 needle_id 分配机制

**由 Filer 分配**（Zone 内完全自治）：

**分配方式**：
1. Filer 启动时向 Master 注册，获得 `zone_id` + 映射的物理 volume 列表
2. Filer Raft leader 维护内存计数器 `counter`（初始化为 0）
3. 创建文件时：`needle_id = (zone_id << 40) | counter++`
4. 通过 Raft 日志持久化 chunk 映射（ino → volume_id, needle_id）
5. Filer 重启时，从 chunk 映射恢复 `counter = max(counter in zone) + 1`

**唯一性保证**：
- 不同 Zone 的 needle_id 通过编码天然不冲突
- 同一 Zone 内由 Filer Raft leader 单点分配，不会重复
- 不需要 Master 协调 needle_id 分配

### 2.5 Zone 注册与映射

```
Filer 启动:
  Filer → Master: RegisterFiler(filer_id, filer_addr)
  Master:
    1. 查找该 filer_id 的所有已有 Zone
    2. 若无, 创建新 Zone: 分配 zone_id + 选 N 个物理 volume
    3. 若有, 返回所有已有 Zone (不自动创建新 Zone)
    4. 持久化到 Master Raft (P1.3 待实现)
  Master → Filer: Vec<ZoneInfo> [(zone_id, [(volume_id, vol_addr, size, used), ...]), ...]
  Filer 缓存所有 Zone + 为每个 Zone 初始化/恢复 counter

创建文件 (多 Zone round-robin):
  Filer (完全自治, 不跟 Master/Volume Server 通信):
    1. round-robin 选一个 Zone: zone_rr.fetch_add(1) % zones.len()
    2. needle_id = (zone_id << 40) | counter++  (选中 Zone 的 counter)
    3. 从该 Zone 映射的物理 volume 中选空闲比例最大的
    4. Raft 持久化 chunk 映射 (ino → volume_id, needle_id)
    5. 返回 (volume_id, needle_id) 给客户端
  客户端 → Volume Server: WriteNeedle(volume_id, needle_id, data) [直连]

Filer 重启:
  1. 向 Master 重新注册 (获取所有已有 Zone 映射)
  2. 对每个 Zone, 从 chunk 映射恢复 counter = max(counter in zone) + 1
  3. 继续分配 (round-robin 跨 Zone)
```

### 2.6 负载均衡与迁移

| 场景 | 处理 |
|------|------|
| 新 Filer 加入 | Master 分配新 zone_id + 映射到负载低的物理 volume |
| 物理 volume 满 | Master 更新 zone 映射，指向新 volume；通知 Filer 刷新 |
| Filer 宕机 | Master 将 zone 迁移到其他 Filer（needle_id 编码不变，数据可访问） |
| 再均衡 | Master 调整 zone → physical volume 映射，通知相关 Filer |

### 2.7 数据流

#### 创建文件

```
1. 客户端 → Filer: Create(parent_ino, name, mode)
2. Filer (Raft leader, 完全自治):
   a. round-robin 选 Zone: zone_rr++ % zones.len()
   b. needle_id = (zone_id << 40) | counter++  (选中 Zone 的 counter)
   c. 从该 Zone 映射的物理 volume 中选空闲比例最大的
   d. 创建 inode + 存储 chunk 映射 (volume_id, needle_id) [Raft 持久化]
   e. 返回 (ino, volume_id, needle_id)
3. 客户端 → Volume Server: WriteNeedle(volume_id, needle_id, data) [直连]
```

#### 读取文件

```
1. 客户端 → Filer: GetAttr(ino) → 得到 (volume_id, needle_id, chunks列表)
2. 客户端 → Volume Server: ReadNeedle(volume_id, needle_id) [直连]
```

#### 写入已有文件

```
1. 客户端 → Filer: GetAttr(ino) → 得到 (volume_id, needle_id)
2. 客户端 → Volume Server: WriteNeedle(volume_id, needle_id, data) [直连]
   (read-modify-write: 先 ReadNeedle 读旧数据, 合并后 WriteNeedle)
```

### 2.8 连接池设计（内核侧）

```
g_pool
├── schedulers[]          ← filer 连接专用 (per-CPU, 处理元数据收发)
├── vol_schedulers[]      ← volume 连接专用 (per-CPU, 处理数据收发)
├── filers[]              ← filer 连接 → pfs_pick_sched → schedulers[]
├── volumes[]             ← volume 连接 → pfs_pick_vol_sched → vol_schedulers[]
├── reconn_wq             ← 共享 (重连稀有，无需分离)
├── shard_routes[]        ← filer shard 路由表 (shard_id → leader)
└── vol_routes[]          ← volume 路由表 (volume_id → conn_idx)
```

**分离原则**：
- 协议层共享（TLV 编解码、frame 收发、`struct powerfs_net_server_conn`）
- 调度器线程池分离（数据 I/O 不饿死元数据请求）
- 重连 workqueue 共享（重连是稀有事件，无需分离）

### 2.9 孤儿数据处理

- 创建文件时 Filer 分配 needle_id 后，如果 Raft 持久化失败，Volume Server 上不会有孤儿（因为数据还没写入）
- 如果客户端拿到 (volume_id, needle_id) 后 crash，未写入数据 → 无孤儿（needle_id 未使用）
- 如果客户端写入数据后 crash，未 close → 有数据但 Filer 已有 chunk 映射 → 不是孤儿
- **不存在 Filer → Volume Server 通信失败的孤儿问题**（因为 Filer 不跟 Volume Server 通信）

### 2.10 NVMe-oF target 兼容性

将来 Volume Server 演进为 NVMe-oF target 设备时：
- 只需支持 WriteNeedle/ReadNeedle（或等价的块设备读写）
- needle_id 映射到 NVMe-oF 的 LBA（`lba = needle_id * chunk_size / sector_size`）
- Zone 概念对 Volume Server 完全透明
- 不需要 AssignNeedle 等复杂逻辑
- Filer 的 zone 注册改为向 Master 申请 LBA 范围

## 3. 实施步骤

> **当前进度**: Phase 1/2/4 的核心改动已完成（约 70%），剩余 Phase 2 收尾 + Phase 3 内核侧改造 + 编译/测试。
> 详见 §3.6 进度追踪表。

### Phase 1: Master 侧改动

| 步骤 | 说明 | 状态 |
|------|------|------|
| 1.1 Zone 数据结构 | `powerfs-common/src/types.rs` 新增 ZoneInfo/ZoneVolume + needle_id 编码函数 | ✅ 已完成 |
| 1.2 RegisterFiler 接口 | `powerfs-master/src/master.rs::register_filer_zone` + `net_handler.rs::handle_register_filer` | ✅ 已完成 |
| 1.3 Zone 映射持久化 | Zone → physical volume 映射通过 Master Raft 持久化 | ✅ 已完成（QEMU 验证通过） |
| 1.4 GetTopology 增强 | GetTopology 响应增加 used/file_count 字段 | ✅ 已完成 |
| 1.5 心跳状态存储 | Master 心跳解析 used/file_count | ✅ 已完成 |
| 1.6 删除旧 Assign 接口 | 旧的 Assign（客户端调用）不再需要 | ⏳ 待办（L） |

### Phase 2: Filer 侧改动

| 步骤 | 说明 | 状态 |
|------|------|------|
| 2.1 Zone 注册客户端 | `powerfs-filer/src/zone_client.rs` 已创建（register_filer / recover_counter / alloc_needle_id / select_volume） | ✅ 已完成 |
| 2.2 needle_id 计数器结构 | `FilerNetHandler` 多 Zone 状态: `zones: RwLock<Vec<ZoneState>>` + `zone_rr: AtomicU32` + `set_zones` + `set_zone_counter` + `alloc_for_new_file` (round-robin) | ✅ 已完成 |
| 2.3 main.rs 集成 Zone 注册 | Filer 启动时调用 `zone_client::register_filer` 并通过 `set_zones` 注入 FilerNetHandler (多 Zone) | ✅ 已完成 |
| 2.4 handle_create 改造 | 改用 `alloc_for_new_file` 自分配 needle_id + volume_id (多 Zone round-robin)，并通过 set_chunks 持久化 chunk 映射 | ✅ 已完成 |
| 2.5 counter 恢复 | Filer 重启时从 chunk 映射恢复 counter = max(counter in zone) + 1 | ⏳ 待办（M） |
| 2.6 volume 选择 | 从 zone 映射的物理 volume 中选空闲比例最大的（`zone_client::select_volume`） | ✅ 已完成 |
| 2.7 删除 volume_client.rs | 不再需要 Filer → Volume Server 通信 | ✅ 已完成 |
| 2.8 删除 handle_write/handle_read | Filer 不再转发数据 | ✅ 已完成 |

### Phase 3: 内核侧改动（核心）

| 步骤 | 说明 | 状态 |
|------|------|------|
| 3.1 分离 volume 调度器池 | 新增 `vol_schedulers[]`，volume 连接指向独立调度器（避免数据 I/O 饿死元数据） | ⏳ 待办（H） |
| 3.2 conn_pool_init 传入 master_addr | 让 `g_pool.master_addr` 可用（discover_volumes 依赖） | ⏳ 待办（H） |
| 3.3 discover_volumes | fill_super 中调用，填充 `vol_routes[]` 路由表 | ✅ 已修复（P3.3a 自动建连） |
| 3.3a Volume 动态发现修复 | discover_volumes 中自动建立新连接，volume_addr 降级为 fallback | ✅ 已完成（QEMU 验证通过） |
| 3.3b Lease TLV 字段顺序 + 续约修复 | acquire/renew/release/write_needle 字段顺序对齐 + renew 失败 re-acquire | ✅ 已完成（QEMU 验证通过） |
| 3.4 powerfs_net_create 解析 volume_id/needle_id | 解析 Filer 响应中的 volume_id/needle_id，存入 inode 私有数据 | ✅ 已完成 |
| 3.5 powerfs_net_write | 直连 volume（WriteNeedle + read-modify-write） | ✅ 已实现 |
| 3.6 powerfs_net_read | 直连 volume（ReadNeedle） | ✅ 已实现 |
| 3.7 powerfs_net_getattr | 解析 volume_id/file_key | ✅ 已实现 |
| 3.8 删除 powerfs_net_assign | Master assign 不再需要（Filer 自管理 needle_id） | ✅ 已完成（函数已不存在，清理协议常量+注释） |
| 3.9 writepage_work_fn | 按 needle 分组批量写 | ✅ 已实现 |

### Phase 4: Volume Server 侧改动

| 步骤 | 说明 | 状态 |
|------|------|------|
| 4.1 删除 AssignNeedle | Volume Server 不需要 AssignNeedle（Filer 自分配 needle_id） | ✅ 已完成 |
| 4.2 WriteNeedle 保持简单 | 只管写入数据，不做分配逻辑 | ✅ 已有 |
| 4.3 心跳上报状态 | 上报 used/needle_count 到 Master | ✅ 已完成 |

### Phase 5: FUSE 侧适配（优先级靠后）

| 步骤 | 说明 | 状态 |
|------|------|------|
| 5.1 FUSE create 适配 | 解析 Filer 响应中的 volume_id/needle_id | ⏳ 待办（L） |
| 5.2 FUSE 直连 volume | 数据读写直连 Volume Server | ⏳ 待办（L） |
| 5.3 FUSE 连接池分离 | 数据/元数据调度器分离 | ⏳ 待办（L） |

### 3.1 Phase 2 收尾：Filer 侧具体实施

#### P2.3 main.rs 集成 Zone 注册

**目标**：Filer 启动时调用 `zone_client::register_filer`，获取多 Zone 后注入 `FilerNetHandler`。

**位置**：`powerfs-filer/src/main.rs` 第 356 行附近（`FilerNetHandler::with_notifier` 创建之后）。

**改动要点**：
```rust
// 1. 在 FilerNetHandler 创建后，克隆 Arc 引用
let net_handler_for_zone = net_handler.clone();
let master_addrs_for_zone = master_addresses.clone();

// 2. 异步发起 Zone 注册（不阻塞主流程）
tokio::spawn(async move {
    let filer_id = format!("filer-{}", filer_cfg.raft_id);

    for master_addr in &master_addrs_for_zone {
        // register_filer 返回 Vec<ZoneInfo> (多 Zone)
        match powerfs_filer::zone_client::register_filer(master_addr, &filer_id).await {
            Ok(zones) => {
                info!(
                    "FILER_ZONE: registered zones={}, total_volumes={}",
                    zones.len(),
                    zones.iter().map(|z| z.physical_volumes.len()).sum::<usize>()
                );
                // 注入多 Zone 到 FilerNetHandler
                net_handler_for_zone.set_zones(zones);
                return; // 注册成功即退出
            }
            Err(e) => {
                warn!("FILER_ZONE: register_filer failed on {}: {}", master_addr, e);
            }
        }
    }
    warn!("FILER_ZONE: all master attempts failed, zone not registered");
});
```

**注意事项**：
- Master 的 `register_filer_zone` 内部已支持 REDIRECT 到 leader，所以可以连接任意 Master 节点。
- `register_filer` 返回 `Vec<ZoneInfo>`：首次注册返回 1 个，重启返回所有已有 Zone。
- `set_zones` 会保留已有 Zone 的 counter（若 zone_id 匹配），避免重置。
- 注册失败不阻塞 Filer 启动，但 `handle_create` 在 zones 为空时会返回 SERVER_ERROR。

#### P2.4 handle_create 改造

**目标**：`handle_create` 不再依赖客户端传入 `fid`/`cookie`/`offset`，改由 Filer 自分配。

**位置**：`powerfs-filer/src/net_handler.rs` 第 591 行 `handle_create` 函数。

**改动要点**：
```rust
async fn handle_create(&self, msg: &NetMessage) -> NetResult<NetMessage> {
    let mut dec = TlvDecoder::new(&msg.body);
    let parent_ino = dec.next_u64(FieldId::ParentIno).unwrap_or(0);
    let name = dec.next_string(FieldId::Name).unwrap_or_default();
    let mode = dec.next_u32(FieldId::Mode).unwrap_or(0o644) as u64;
    let uid = dec.next_u32(FieldId::Uid).unwrap_or(0) as u64;
    let gid = dec.next_u32(FieldId::Gid).unwrap_or(0) as u64;

    // 删除: 客户端不再传 fid/cookie/offset，由 Filer 自分配
    // let fid = dec.next_string(FieldId::Fid).ok();
    // let cookie = dec.next_u64(FieldId::Cookie).ok();
    // let offset = dec.next_u64(FieldId::FileKey).ok();
    // let chunk_size = dec.next_u64(FieldId::Size).ok();

    let shard_id = self.shard_strategy.calculate_shard(parent_ino);
    if let Err(redirect) = self.check_leader(msg, shard_id).await {
        return Ok(redirect);
    }

    // === Zone 自分配 needle_id + volume_id ===
    let (volume_id, needle_id) = match self.alloc_for_new_file() {
        Some(v) => v,
        None => {
            warn!("FILER_NET_CREATE: zone not registered, cannot allocate");
            return Ok(Self::build_response(
                msg,
                STATUS_ERR_SERVER_ERROR,
                Vec::new(),
            ));
        }
    };

    match self
        .meta_shard_manager
        .create_file_with_shard(parent_ino, &name, shard_id)
        .await
    {
        Ok(ino) => {
            let _ = self
                .meta_shard_manager
                .setattr(ino, shard_id, None, Some(mode), Some(uid), Some(gid))
                .await;

            // === 持久化 chunk 映射 (volume_id, needle_id) ===
            // fid 格式: "volume_id,needle_id"（兼容现有 set_chunks 解析）
            let fid_str = format!("{},{}", volume_id, needle_id);
            let _ = self
                .meta_shard_manager
                .set_chunks(ino, shard_id, &fid_str, volume_id, 0, needle_id, 0)
                .await;

            let now = crate::shard_store::ShardStore::current_time();
            self.notify_inode_change(parent_ino, now);
            self.notify_inode_change(ino, now);

            let mut enc = TlvEncoder::new();
            enc.add_u64(FieldId::Ino, ino);
            enc.add_u32(FieldId::Mode, mode as u32);
            enc.add_string(FieldId::Name, &name)?;
            // === 返回 volume_id + needle_id 给客户端 ===
            enc.add_u64(FieldId::VolumeId, volume_id);
            enc.add_u64(FieldId::FileKey, needle_id);
            Ok(Self::build_response(msg, STATUS_OK, enc.into_bytes()))
        }
        Err(e) => {
            warn!("FILER_NET_CREATE failed: {}", e);
            Ok(Self::build_response(msg, STATUS_ERR_SERVER_ERROR, Vec::new()))
        }
    }
}
```

**关键变化**：
1. 删除客户端传入的 `fid`/`cookie`/`offset`/`chunk_size` 解析
2. 调用 `alloc_for_new_file()` 自分配 `(volume_id, needle_id)`
3. 通过 `set_chunks` 持久化 chunk 映射（Raft 强一致）
4. 响应中通过 `FieldId::VolumeId` + `FieldId::FileKey` 返回给客户端

#### P2.5 counter 恢复 ✅ 已完成

**目标**：Filer 重启时从 chunk 映射恢复每个 Zone 的 counter，避免 needle_id 重复。

**状态**：已完成并编译验证通过。
- `ShardStore::list_all_chunks()` — 遍历单个 shard 的所有 inode，收集 (needle_id, volume_id) 映射
- `MetaShardManager::list_all_chunks()` — 遍历所有 shard，合并 chunk 映射
- `FilerNetHandler::get_zones()` — 返回所有 zone_id 列表
- `main.rs` Zone 注册成功后调用 `recover_counter` 恢复每个 Zone 的 counter

**位置**：`powerfs-filer/src/main.rs` Zone 注册成功之后。

**改动要点**：
```rust
// 在 set_zones 之后，对每个 Zone 恢复 counter
let chunks: Vec<(u64, u64)> = meta_shard_manager
    .list_all_chunks()  // 需要在 MetaShardManager 中新增此方法
    .await
    .into_iter()
    .map(|c| (c.volume_id, c.needle_id))
    .collect();

// 对每个 Zone 独立恢复 counter
let zones = net_handler_for_zone.get_zones();  // 需新增此方法
for zone_id in zones {
    let recovered = zone_client::recover_counter(zone_id, &chunks);
    net_handler_for_zone.set_zone_counter(zone_id, recovered);
    info!("FILER_ZONE: recovered zone_id={} counter={}", zone_id, recovered);
}
```

**待新增**：
- `MetaShardManager::list_all_chunks()` 方法，遍历所有 shard 收集 chunk 映射
- `FilerNetHandler::get_zones()` 方法，返回所有 zone_id 列表

### 3.2 Phase 3 内核侧具体实施

#### P3.1 分离 volume 调度器池 ✅ 已完成

**目标**：新增 `vol_schedulers[]`，volume 连接走独立调度器线程池。

**状态**：已完成并编译验证通过（powerfs.ko 生成无警告）。
- `powerfs_net.h`: `struct powerfs_net_pool` 新增 `vol_schedulers` + `num_vol_sched` 字段
- `powerfs_net.c`: 新增 `pfs_pick_vol_sched()`，volume 连接走独立调度器池
- `powerfs_sched_init()` / `powerfs_sched_exit()`: 同时管理 filer + volume 两个调度器池
- 线程命名: `pfs_sched/%d` (filer) + `pfs_vsched/%d` (volume)

**位置**：`kernel/powerfs_mod/powerfs_net.h` + `powerfs_net.c`。

**改动要点**：
```c
/* powerfs_net.h: struct powerfs_net_pool 新增字段 */
struct powerfs_net_pool {
    /* ... 现有字段 ... */
    struct powerfs_sched schedulers[POWERFS_NET_MAX_SCHED];     /* filer 专用 */
    struct powerfs_sched vol_schedulers[POWERFS_NET_MAX_SCHED]; /* volume 专用 (新增) */
    /* ... */
};

/* powerfs_net.c: pool_init 中初始化 vol_schedulers */
for (i = 0; i < num_sched; i++) {
    /* schedulers[i] 已有初始化 */
    /* 新增: vol_schedulers[i] 同样初始化 */
    ret = powerfs_sched_init(&g_pool.vol_schedulers[i], ...);
    if (ret) goto err;
}

/* 新增: pfs_pick_vol_sched 函数 (类似 pfs_pick_sched, 但用 vol_schedulers) */
static struct powerfs_sched *pfs_pick_vol_sched(struct powerfs_net_server_conn *conn)
{
    /* 基于 conn->addr hash % num_sched */
    u32 idx = powerfs_addr_hash(conn->addr) % g_pool.num_sched;
    return &g_pool.vol_schedulers[idx];
}

/* volume 连接建立时, conn->sched 指向 vol_schedulers[idx] */
```

**注意**：
- `vol_schedulers[]` 与 `schedulers[]` 线程数相同（per-CPU），但独立调度
- 重连 workqueue `reconn_wq` 共享（重连是稀有事件）
- 现有 `pfs_pick_sched` 保持不变，只服务 filer 连接

#### P3.2 conn_pool_init 传入 master_addr ✅ 已完成

**目标**：`g_pool.master_addr` 可用，供 `discover_volumes` 使用。

**状态**：已完成。`fill_super` 中 `powerfs_conn_pool_init` 改为传入 `sbi->master_addr` + `sbi->master_port`，`g_pool.master_addr` 被正确设置。

**位置**：`kernel/powerfs_mod/powerfs_net.c::powerfs_net_pool_init` + `powerfs_fs.c::powerfs_fill_super`。

**改动要点**：
```c
/* powerfs_net.h: struct powerfs_net_pool 新增字段 */
char master_addr[POWERFS_ADDR_LEN];   /* "ip:port" 或 "ip1:port1,ip2:port2" */
__u16 master_net_port;

/* powerfs_net.c: powerfs_net_pool_init 新增参数 */
int powerfs_net_pool_init(const char *master_addrs, __u16 master_net_port, ...)
{
    /* ... 现有初始化 ... */
    strncpy(g_pool.master_addr, master_addrs, sizeof(g_pool.master_addr) - 1);
    g_pool.master_net_port = master_net_port;
    /* ... */
}

/* powerfs_fs.c: powerfs_fill_super 中调用 */
powerfs_net_pool_init(config->master_addrs, config->master_net_port, ...);
```

#### P3.3 discover_volumes 集成到 fill_super ⚠️ 需修复

**目标**：mount 时自动发现 volume 路由表。

**状态**：已实现但存在设计缺陷。`fill_super` 中 `conn_pool_init` 成功后调用 `powerfs_net_discover_volumes(maddr, mport)`，从 Master GetTopology 获取 `volume_id → conn_idx` 路由表。失败不阻断挂载（filer 元数据仍可用）。

**缺陷描述**：discover_volumes 只建立 `volume_id → conn_idx` 映射，**不建立新连接**。volume 连接在 `fill_super` 中用 `volume_addr` 模块参数提前建立（powerfs_fs.c:3624-3639）。如果 `volume_addr` 未配置或配置不全，即使 master 返回了完整拓扑，所有 route 也无法匹配已有连接，打印 `no matching conn` 警告，数据 IO 全部失败。这违背了"内核只需要 master 信息"的设计目标（filer 的发现逻辑是正确的，volume 没跟上）。详见 P3.3a 修复方案。

**位置**：`kernel/powerfs_mod/powerfs_fs.c::powerfs_fill_super` + `powerfs_net.c::powerfs_net_discover_volumes`。

**当前实现**：
```c
/* powerfs_fill_super 中, pool_init 之后, 根 dentry 创建之前 */
ret = powerfs_net_discover_volumes(g_pool.master_addr, g_pool.master_net_port);
if (ret < 0) {
    pr_warn("powerfs: discover_volumes failed: %d (volume reads will fail)\n", ret);
    /* 不挂载失败: filer 元数据仍可用, 数据读写等 volume 上线后恢复 */
}
```

#### P3.3a Volume 动态发现修复方案 ✅ 已完成（QEMU 验证通过）

**目标**：discover_volumes 从 master 拿到 volume 地址后自动建立连接，`volume_addr` 模块参数降级为 fallback（仅当 master 不可用时使用）。

**根因分析**：

当前 `fill_super` 的 volume 配置流程：

| 步骤 | 行号 | 操作 | 地址来源 |
|------|------|------|----------|
| 1 | 3564-3577 | 添加 master 到连接池 | `sbi->master_addr` 模块参数 ✅ |
| 2 | 3580-3589 | 从 master 发现 filer | master 动态发现 ✅ |
| 3 | 3592-3621 | filer fallback | `sbi->filer_addr` 模块参数 |
| 4 | **3624-3639** | **添加 volume 到连接池** | **`sbi->volume_addr` 模块参数** ❌ |
| 5 | 3663-3672 | discover_volumes 建立 volume_id→conn_idx 映射 | master GetTopology ✅ |

filer 的发现是正确的（步骤 2 从 master 发现），但 volume 没跟上（步骤 4 用模块参数而非 master 发现）。

**修复设计**：

1. **discover_volumes 增加自动建连逻辑**：

```c
/* powerfs_net.c: powerfs_net_discover_volumes 修复后的核心逻辑 */

/* 对 master 返回的每个 volume route */
for (i = 0; i < route_count; i++) {
    char *vaddr = routes[i].addr;  /* "ip:net_port", 由 master 提供 */
    u64 volume_id = routes[i].volume_id;

    /* 1. 先在 g_pool.volumes[] 中查找匹配的已有连接 */
    conn_idx = find_volume_conn_by_addr(vaddr);

    if (conn_idx < 0) {
        /* 2. 未匹配 → 自动建立新连接 (核心修复) */
        char v_ip[64];
        u16 v_port;
        parse_addr_port(vaddr, v_ip, &v_port);

        /* 调用 add_server 建立 volume 连接 (复用现有函数) */
        ret = powerfs_net_add_server(v_ip, v_port,
                                      POWERFS_NET_SERVER_VOLUME);
        if (ret < 0) {
            pr_warn("vol_route: volume_id=%llu addr=%s add_server failed: %d\n",
                    volume_id, vaddr, ret);
            continue;  /* 跳过此 route, 不影响其他 */
        }
        conn_idx = g_pool.volume_count - 1;  /* 新连接的索引 */
        pr_info("vol_route: auto-connected volume_id=%llu addr=%s\n",
                volume_id, vaddr);
    }

    /* 3. 建立 volume_id → conn_idx 映射 */
    g_pool.vol_routes[nr].volume_id = volume_id;
    g_pool.vol_routes[nr].conn_idx = conn_idx;
    nr++;
}
```

2. **fill_super 中 volume_addr 降级为 fallback**：

```c
/* powerfs_fs.c: powerfs_fill_super 修复后 */

/* 先尝试从 master 动态发现 volume (主路径) */
ret = powerfs_net_discover_volumes(g_pool.master_addr, g_pool.master_net_port);
if (ret < 0 || g_pool.volume_count == 0) {
    /* master 发现失败 → fallback 到 volume_addr 模块参数 */
    pr_warn("powerfs: discover_volumes failed(%d), fallback to volume_addr\n", ret);
    if (sbi->volume_addr[0]) {
        /* 原有的 volume_addr 解析 + add_server 逻辑 (作为 fallback) */
        powerfs_net_add_server_fallback(sbi->volume_addr,
                                         sbi->volume_port,
                                         POWERFS_NET_SERVER_VOLUME);
    } else {
        pr_warn("powerfs: no volume_addr fallback, volume IO will fail\n");
    }
}
```

3. **addr 匹配函数精确化**（修复现有前缀匹配隐患）：

```c
/* 现有匹配逻辑有前缀匹配隐患:
 * strncmp("172.30.0.21", "172.30.0.21:8901", 12) 会匹配
 * 但 "172.30.0.2" 也会匹配 "172.30.0.21:8901"
 *
 * 修复: 精确比较 "ip:port" 完整字符串
 */
static int find_volume_conn_by_addr(const char *vaddr)
{
    int j;
    for (j = 0; j < g_pool.volume_count; j++) {
        char conn_addr[80];
        snprintf(conn_addr, sizeof(conn_addr), "%s:%u",
                 g_pool.volumes[j].addr, g_pool.volumes[j].port);
        if (strcmp(conn_addr, vaddr) == 0)  /* 精确匹配 */
            return j;
    }
    return -1;
}
```

**修复后的数据流**：

```
fill_super:
  1. 添加 master 到连接池 (master_addr 模块参数)
  2. 从 master 发现 filer (动态发现)
  3. discover_volumes:
     a. 从 master GetTopology 获取 volume routes
     b. 对每个 route 的 "ip:net_port":
        - 查找 g_pool.volumes[] 是否已有匹配连接
        - 未匹配 → 自动 add_server 建立新连接
        - 建立 volume_id → conn_idx 映射
  4. fallback: 如果 discover_volumes 失败,用 volume_addr 建立连接

模块参数变化:
  master_addr  → 必须配置 (唯一入口)
  filer_addr   → fallback (master 发现失败时使用)
  volume_addr  → fallback (master 发现失败时使用, 正常情况不需要)
```

**服务端确认**（无需修改）：

- Master `GetTopology` 返回的 `VolumeRoute.addr` 用 **net_port**（[master.rs:1102](file:///home/portion/powerfs/powerfs-master/src/master.rs#L1102) `format!("{}:{}", ip, net_port)`）
- Volume heartbeat 携带 `net_port`，master 更新 route table
- 预注册的 DataNodeInfo 用 gRPC 地址（仅预注册，heartbeat 修正）

**测试验证**：

```bash
# 1. 不配置 volume_addr, 仅配置 master_addr, 验证 volume IO 正常
insmod powerfs.ko master_addr="172.30.0.11,172.30.0.12,172.30.0.13" master_port=9334
mount -t powerfs none /mnt/pfs
echo test > /mnt/pfs/file.txt  # 写入触发 volume 连接
cat /mnt/pfs/file.txt          # 读取验证
dmesg | grep "auto-connected"  # 确认自动建连日志

# 2. 配置错误的 volume_addr, 验证 master 发现优先
insmod powerfs.ko master_addr="172.30.0.11" master_port=9334 \
    volume_addr="10.0.0.99" volume_port=9999
mount -t powerfs none /mnt/pfs
dmesg | grep "auto-connected"  # 确认用 master 发现的地址, 忽略错误 volume_addr

# 3. master 不可达, 验证 volume_addr fallback
insmod powerfs.ko master_addr="10.0.0.99" master_port=9334 \
    volume_addr="172.30.0.21" volume_port=8901
mount -t powerfs none /mnt/pfs
dmesg | grep "fallback"  # 确认 fallback 日志
```

#### P3.3b Lease TLV 字段顺序 + 续约机制修复 ✅ 已完成（QEMU 验证通过）

**目标**：修复内核 lease 请求的 TLV 字段顺序与 volume server 不匹配导致 "Lease holder mismatch" 和 renew 持续失败 -121 的问题。

**根因**：TlvDecoder 按顺序读取字段，内核多发 `Ino(volume_id)+InodeV2(ino)` 两个字段导致后续全部错位；write_needle 缺少 ClientId 导致 holder 不匹配；连接断开后 lease 被释放但内核不知道。

**修复内容**（详见 [kernel-lease-plan.md](kernel-lease-plan.md) Step 3）：

1. **acquire_lease**: `Ino(inode)+Offset+Limit+ClientId+Mode+Duration`（删除 InodeV2，Ino 发送 inode 而非 volume_id）
2. **renew_lease**: `LeaseToken+ClientId+Duration`（删除 Ino/InodeV2）
3. **release_lease**: `LeaseToken+ClientId`（删除 Ino/InodeV2）
4. **write_needle**: 添加 `ClientId="kernel-client"` 字段
5. **lease_renew_work_func**: renew 失败时自动 re-acquire lease

**验证结果**：acquire/write/renew 全链路正常，无 "Lease holder mismatch"，续约间隔 20s，5MB 文件 md5sum 一致。

#### P3.4 powerfs_net_create 解析 volume_id/needle_id

**目标**：内核解析 Filer create 响应中的 `VolumeId` + `FileKey`，存入 inode。

**位置**：`kernel/powerfs_mod/powerfs_net.c::powerfs_net_create` + `powerfs.h::struct powerfs_inode`。

**改动要点**：
```c
/* powerfs.h: struct powerfs_inode 新增字段 */
struct powerfs_inode {
    /* ... 现有字段 ... */
    __u64 volume_id;    /* 新增: 文件所属 volume_id */
    __u64 needle_id;    /* 新增: 文件的 needle_id (zone_id<<40 | counter) */
};

/* powerfs_net.c: powerfs_net_create 解析响应 */
int powerfs_net_create(__u64 dir_ino, const char *name, size_t name_len,
                       __u32 mode, __u32 uid, __u32 gid,
                       struct powerfs_create_result *result)
{
    /* ... 发送请求 + 接收响应 ... */
    /* 解析 TLV 响应 */
    while (tlv_has_more(&dec)) {
        field = tlv_next_field(&dec);
        switch (field.id) {
        case FieldId_Ino:
            result->ino = tlv_read_u64(field);
            break;
        case FieldId_Mode:
            result->mode = tlv_read_u32(field);
            break;
        case FieldId_VolumeId:    /* 新增 */
            result->volume_id = tlv_read_u64(field);
            break;
        case FieldId_FileKey:     /* 新增 */
            result->needle_id = tlv_read_u64(field);
            break;
        }
    }
    return 0;
}

/* powerfs_fs.c: powerfs_create/mknod 中存入 inode */
struct powerfs_create_result result;
ret = powerfs_net_create(...);
if (ret == 0) {
    inode = powerfs_iget(sb, result.ino);
    powerfs_i(inode)->volume_id = result.volume_id;
    powerfs_i(inode)->needle_id = result.needle_id;
}
```

### 3.3 编译验证与测试

#### 3.3.1 编译验证顺序

1. **Rust 侧**（按依赖顺序）：
   ```bash
   cd /home/portion/powerfs
   cargo check -p powerfs-common        # Zone 类型
   cargo check -p powerfs-net           # RegisterFiler 协议
   cargo check -p powerfs-master        # register_filer_zone
   cargo check -p powerfs-volume        # 已删除 AssignNeedle
   cargo check -p powerfs-filer         # zone_client + handle_create 改造
   cargo build --release -p powerfs-master -p powerfs-volume -p powerfs-filer
   ```

2. **内核侧**（QEMU 内编译）：
   ```bash
   # 在 QEMU 内核源码树中
   make M=fs/powerfs modules
   ```

#### 3.3.2 测试计划

**单元测试**（Rust）：
- Master `register_filer_zone` 正确性（zone_id 递增、复用、映射持久化）
- Filer `alloc_needle_id` 编码正确性（zone_id<<40 | counter）
- Filer `recover_counter` 从 chunk 列表恢复
- 多 Filer 共享物理 volume，needle_id 不冲突

**集成测试**（容器环境，按用户要求**必须**在容器内测试）：
1. 启动 Master + 3 个 Volume Server + 1 个 Filer
2. 验证 Filer 启动日志：`FILER_ZONE: registered zone_id=1, volumes=3`
3. 创建文件 → 验证 inode 中 volume_id/needle_id 正确
4. fio 顺序写 → 读 → 校验数据一致
5. 跨客户端读：客户端 A 写 → 客户端 B 读
6. 多 volume 路由：验证请求路由到正确的 volume
7. Volume 宕机恢复：停一个 volume server，Filer 创建文件不受影响

**QEMU 内核测试**（按用户要求**必须**在 QEMU 中测试）：
1. 加载 powerfs.ko 模块
2. mount powerfs → 验证 `discover_volumes complete, N routes added`
3. 创建文件 → 写数据 → 读数据 → 校验
4. fio 测试（顺序/随机 读写）
5. 高并发数据 I/O 下验证元数据延迟不退化（调度器隔离验证）
6. KASAN + CONFIG_DEBUG_DCACHE 验证无内存错误

**性能测试**：
- fio 顺序写 / 随机写 / 顺序读 / 随机读
- 对比：直连 volume vs 经 Filer 转发（已有基线）
- 元数据延迟：高并发数据 I/O 下的 lookup/getattr 延迟
- IO500 测试

### 3.4 实施顺序（推荐）

按依赖关系和风险等级排序：

```
Step 1: P2.4 handle_create 改造 (Filer 自分配 needle_id)
        └─ 依赖: P2.2 已完成 ✅
        └─ 风险: 中 (改动核心元数据路径)
        └─ 验证: cargo check -p powerfs-filer

Step 2: P2.3 main.rs 集成 Zone 注册
        └─ 依赖: P2.1 已完成 ✅, P2.4 (handle_create 需要 zone_id)
        └─ 风险: 低 (异步注册, 失败不阻塞)
        └─ 验证: 启动 Filer, 观察日志

Step 3: Rust 侧编译 + 容器集成测试
        └─ 验证 Filer 创建文件返回正确 volume_id/needle_id
        └─ 验证 Filer 重启后 counter 恢复 (P2.5)

Step 4: P3.4 powerfs_net_create 解析 volume_id/needle_id
        └─ 依赖: P2.4 (Filer 响应格式已确定)
        └─ 风险: 低 (纯解析, 不影响现有逻辑)
        └─ 验证: QEMU 内创建文件, 打印 inode 字段

Step 5: P3.1 分离 volume 调度器池
        └─ 依赖: 无 (独立改动)
        └─ 风险: 中 (影响并发行为)
        └─ 验证: QEMU 内 fio 高并发测试

Step 6: P3.2 + P3.3 conn_pool_init + discover_volumes 集成
        └─ 依赖: P3.1 (调度器池就绪)
        └─ 风险: 低 (补充字段)
        └─ 验证: QEMU 内 mount, 观察 discover_volumes 日志

Step 7: QEMU 内核完整测试 (fio + KASAN)
        └─ 按用户要求: 从简单到复杂, 多测试验证

Step 8: P1.3 Zone 映射 Raft 持久化 + P2.5 counter 恢复
        └─ 依赖: 整体功能可用后
        └─ 风险: 中 (Raft 协议改动)
        └─ 验证: Master 重启后 Zone 映射不丢失

Step 9: 清理 (P1.6 删除旧 Assign, P3.8 删除 powerfs_net_assign)
        └─ 依赖: 所有功能验证通过
        └─ 风险: 低 (删除死代码)

Step 10: IO500 测试 + 性能评估文档
```

### 3.5 风险与注意事项

1. **Master net_port 确认**：`master_addresses` 中的端口需确认是 Master 的 net_port（不是 raft 端口）。如不一致，需在 Filer 配置中新增 `master.net_port` 字段。
2. **zone_id=0 处理**：Zone 注册失败时 `zone_id=0`，`handle_create` 应返回错误而非分配非法 needle_id。
3. **counter 持久化**：counter 是内存态，Filer Raft leader 切换后新 leader 的 counter 可能落后。需要通过 Raft 日志持久化 counter 批量预分配（类似 Master 的 `FILE_KEY_BATCH_SIZE` 机制）。
4. **vol_routes 动态刷新**：当前 `vol_routes[]` 在 mount 时填充，volume 新增/删除需要重新 mount。P3.3a 修复后 mount 时自动从 master 发现并建立连接，后续可实现运行时动态刷新（Master NOTIFY 机制）。
5. **chunk 映射格式**：`set_chunks` 的 `fid_str` 格式 `"volume_id,needle_id"` 需与现有解析逻辑兼容，需确认 `meta_shard_manager.set_chunks` 的实现。
6. **内核调试**：按用户要求，内核修改必须在 QEMU 中测试，不能直接在 host 上测试，避免系统崩溃。使用 CONFIG_DEBUG_DCACHE + CONFIG_KASAN 辅助定位问题。

### 3.6 进度追踪

| 阶段 | 总步骤 | 已完成 | 进行中 | 待办 |
|------|--------|--------|--------|------|
| Phase 1: Master | 6 | 5 | 0 | 1 (P1.6) |
| Phase 2: Filer | 8 | 8 | 0 | 0 |
| Phase 3: 内核 | 10 | 10 | 0 | 0 |
| Phase 4: Volume | 3 | 3 | 0 | 0 |
| Phase 5: FUSE | 3 | 0 | 0 | 3 (低优先级) |
| **总计** | **30** | **26** | **0** | **4** |

**下一步行动**（按优先级排序）：
1. ~~**容器集成测试**~~ ✅ 已完成（QEMU 验证通过：基本I/O + 5MB大文件 + MD5一致性 + 目录操作 + 5并发写入，全链路 PASS）
2. ~~**P1.3 Zone 映射 Raft 持久化**~~ ✅ 已完成（Master 重启后 zone_registry 从 Raft 日志重放恢复，3 zones + next_zone_id=4 验证通过）
3. ~~**P3.8 删除 powerfs_net_assign**~~ ✅ 已完成（函数已不存在，清理 POWERFS_NET_MSG_ASSIGN 协议常量 + 注释）
4. **P1.6 删除旧 Assign 接口**（Master assign 仍被 S3/gRPC API 使用，需先迁移到 Zone 模式，暂跳过）

## 4. 各组件改动清单

### Master (Rust)
- `src/zone_manager.rs`（新建）：ZoneInfo 结构体、Zone 分配、Zone→物理 volume 映射管理
- `src/net_handler.rs`：
  - 新增 RegisterFiler 处理（分配 zone_id + 选物理 volume + 返回映射）
  - 已有：GetTopology 含 used/file_count
- `src/master.rs`：zone_registry 字段、register_filer 方法
- `src/raft_storage.rs`：Zone 映射 Raft 持久化

### Filer (Rust)
- `src/zone_client.rs`（新建）：Zone 注册客户端（向 Master 发送 RegisterFiler）
- `src/net_handler.rs`：
  - FilerNetHandler 增加 zone_id + volume_cache 字段
  - handle_create 选 volume + 分配 needle_id + 存储 chunk 映射
- `src/meta_shard_manager.rs`：
  - 新增 recover_needle_counter（从 chunk 映射恢复 counter）
  - needle_id = (zone_id << 40) | counter++
- 删除 `src/volume_client.rs`（不再需要 Filer → Volume Server 通信）

### 内核 (C)
- `powerfs_net.h`：新增 vol_schedulers、更新函数声明
- `powerfs_net.c`：
  - 分离 volume 调度器池
  - conn_pool_init 传入 master_addr
  - discover_volumes 调用
  - powerfs_net_create 解析 volume_id/needle_id
  - 删除 powerfs_net_assign
- `powerfs_fs.c`：
  - mknod 调用 create 后存 volume_id/needle_id 到 inode
  - fill_super 中 discover_volumes

### Volume Server (Rust)
- 删除 AssignNeedle 处理（`handle_assign_needle` 函数和分发入口）
- 删除 `alloc_needle_id()` / `next_needle_id` 字段（不需要了）
- 保留：WriteNeedle/ReadNeedle、心跳上报、`get_stats()`

## 5. 测试计划

### 5.1 单元测试
- Master Zone 分配正确性（zone_id 递增、映射持久化）
- Filer needle_id 分配正确性（编码格式、递增、不重复）
- Filer 重启后 counter 恢复
- 多 Filer 共享物理 volume，needle_id 不冲突
- 内核 powerfs_net_create 解析 volume_id/needle_id

### 5.2 集成测试（容器环境）
- 基本功能：创建文件 → 写数据 → 读数据 → 验证一致
- 跨客户端读：客户端 A 写 → 客户端 B 读
- 多 volume 路由：3 个 volume server，验证请求路由到正确的 volume
- 调度器隔离：高并发数据 I/O 下元数据延迟不退化
- Volume 宕机恢复：停止一个 volume server，Filer 不受影响
- 多 Filer 共享 volume：两个 Filer 写同一个 volume，needle_id 不冲突

### 5.3 性能测试
- fio 顺序写 / 随机写 / 顺序读 / 随机读
- 对比：直连 volume vs 经 Filer 转发（已有基线）
- 元数据延迟：高并发数据 I/O 下的 lookup/getattr 延迟
- IO500 测试

## 6. 注意事项

- Filer 不直接跟 Volume Server 通信，Volume 宕机不影响 Filer 的文件创建
- Volume Server 只管数据读写，兼容 NVMe-oF target 演进
- needle_id 由 Filer Zone 内自管理，编码格式 `(zone_id << 40) | counter`
- Filer 重启时从 chunk 映射恢复 counter（max + 1）
- Zone 是持久化的逻辑概念，Filer 宕机后可迁移到其他 Filer
- 一个物理 volume 可被多个 Zone 共享（needle_id 编码区分）
- 内核 vol_routes[] 路由表在 mount 时填充，volume 新增/删除需要重新 mount 或实现动态刷新

## 7. Zone 编码常量

```rust
// powerfs-common/src/types.rs 或 powerfs-net/src/protocol.rs
pub const ZONE_ID_BITS: u32 = 24;
pub const COUNTER_BITS: u32 = 40;
pub const ZONE_ID_SHIFT: u32 = COUNTER_BITS;  // 40
pub const ZONE_ID_MASK: u64 = (1u64 << ZONE_ID_BITS) - 1;  // 0xFFFFFF
pub const COUNTER_MASK: u64 = (1u64 << COUNTER_BITS) - 1;  // 0xFFFFFFFFFF

/// 从 needle_id 提取 zone_id
pub fn needle_zone_id(needle_id: u64) -> u32 {
    ((needle_id >> ZONE_ID_SHIFT) & ZONE_ID_MASK) as u32
}

/// 从 needle_id 提取 counter
pub fn needle_counter(needle_id: u64) -> u64 {
    needle_id & COUNTER_MASK
}

/// 构造 needle_id
pub fn make_needle_id(zone_id: u32, counter: u64) -> u64 {
    ((zone_id as u64) << ZONE_ID_SHIFT) | (counter & COUNTER_MASK)
}
```
