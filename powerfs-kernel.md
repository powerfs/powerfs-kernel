# PowerFS 内核文件系统开发方案

## 一、背景与目标

### 1.1 项目背景
PowerFS 是一个分布式并行文件系统，当前通过 FUSE（用户态）实现客户端。为了提升性能、降低延迟并充分利用 RDMA 网络硬件，需要开发一套纯内核态的文件系统客户端。

### 1.2 核心目标
- **极致性能**：实现亚毫秒级元数据延迟，高吞吐数据传输
- **RDMA 原生支持**：利用 RDMA 网卡实现零拷贝、低延迟网络通信
- **与现有服务端无缝兼容**：复用现有 PowerFS gRPC/RDMA 服务端，无需修改远端
- **稳定可靠**：生产级稳定性，支持故障恢复和缓存一致性

---

## 二、核心架构设计

### 2.1 总体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                            用户态                                │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  PowerFS 用户态代理 (powerfs-proxy)                         │ │
│  │                                                             │ │
│  │  ┌─ io_uring 管理层 ──────────────────────────────────────┐ │ │
│  │  │  • 批量轮询 SQ/CQ                                      │ │ │
│  │  │  • 注册内存池管理                                       │ │ │
│  │  │  • 私有命令处理 (URING_CMD)                             │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  │                                                             │ │
│  │  ┌─ RPC 桥接层 ──────────────────────────────────────────┐ │ │
│  │  │  • nanopb 裸 Protobuf ↔ gRPC HTTP/2 封装/解封装        │ │ │
│  │  │  • gRPC 请求批量合并                                    │ │ │
│  │  │  • 响应按序回填 CQ                                      │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  │                                                             │ │
│  │  ┌─ RDMA 传输层 ─────────────────────────────────────────┐ │ │
│  │  │  • grpc-rdma 客户端                                    │ │ │
│  │  │  • RDMA MR 内存注册 (与 io_uring 内存池复用)            │ │ │
│  │  │  • 连接管理、故障恢复                                  │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                    ↕ io_uring SQ/CQ (零拷贝共享内存)              │
├─────────────────────────────────────────────────────────────────┤
│                            内核态                                │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  PowerFS 内核模块 (powerfs.ko)                              │ │
│  │                                                             │ │
│  │  ┌─ VFS 操作层 ───────────────────────────────────────────┐ │ │
│  │  │  • super_operations: mount/umount                      │ │ │
│  │  │  • inode_operations:                                   │ │ │
│  │  │    - lookup, mkdir, rmdir                              │ │ │
│  │  │    - create, unlink, rename                            │ │ │
│  │  │    - symlink, readlink, link                           │ │ │
│  │  │    - readdir, getattr, setattr                         │ │ │
│  │  │  • file_operations:                                    │ │ │
│  │  │    - open, release, fsync                               │ │ │
│  │  │    - read, write, mmap                                 │ │ │
│  │  │  • address_space_operations:                           │ │ │
│  │  │    - readpage, writepage                               │ │ │
│  │  │    - readpages, writepages                              │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  │                                                             │ │
│  │  ┌─ 缓存管理层 ──────────────────────────────────────────┐ │ │
│  │  │  • 元数据缓存 (简化版 OR-Set)                           │ │ │
│  │  │  • 页缓存集成 (page cache)                              │ │ │
│  │  │  • 主动失效通知处理                                     │ │ │
│  │  │  • read-ahead 预读优化                                  │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  │                                                             │ │
│  │  ┌─ io_uring 通信层 ──────────────────────────────────────┐ │ │
│  │  │  • SQ/CQ 共享环形缓冲区                                │ │ │
│  │  │  • nanopb 序列化/反序列化                               │ │ │
│  │  │  • 私有 URING_CMD 封装                                  │ │ │
│  │  │  • 同步/异步 RPC 接口                                   │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                    ↕ RDMA (RoCEv2/InfiniBand)
┌─────────────────────────────────────────────────────────────────┐
│                    PowerFS 集群 (远端)                           │
│  ├─ Master: 元数据服务 (gRPC)                                   │
│  ├─ Filer: 路由查询服务                                         │
│  └─ Volume Server: 数据存储服务 (gRPC + RDMA)                   │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 核心设计理念

#### 2.2.1 io_uring 本地通信优势

| 特性 | Netlink | 传统字符设备 | **io_uring (URING_CMD)** |
|------|---------|------------|--------------------------|
| **批量消息** | ❌ 差，单消息 4KB 上限 | ❌ 不支持 | ✅ 极强，单轮数百条 |
| **零拷贝** | ❌ 否 | ⚠️ mmap 可零拷贝 | ✅ 预注册 buffer 纯零拷贝 |
| **系统调用开销** | ❌ 极高 | ⚠️ 高 | ✅ 极低，SQPOLL 近乎无 syscall |
| **RDMA 内存复用** | ❌ 无法复用 | ⚠️ 可复用但无批量 | ✅ 完美，同内存同时注册 RDMA MR |
| **同步 RPC 友好度** | ⚠️ 复杂 | ✅ 高 | ✅ 高，批量同步+异步双模式 |

#### 2.2.2 内存复用策略

用户态代理分配的内存池同时注册到：
1. **io_uring 预注册缓冲区**：内核 fs 零拷贝写入 RPC 消息
2. **RDMA MR**：直接通过 RDMA 发送裸 Protobuf

**优势**：
- 消除双重内存分配
- 减少页锁定（pin/register）开销
- 端到端零拷贝路径

---

## 三、详细技术设计

### 3.1 io_uring 配置与初始化

```c
// 内核侧 io_uring 初始化
struct powerfs_uring_ctx {
    struct io_ring_ctx *ring_ctx;
    
    // 共享内存区域
    void *sq_base;          // SQ 基地址
    void *cq_base;          // CQ 基地址
    void *buf_base;         // 预注册缓冲区基地址
    size_t buf_size;        // 缓冲区大小
    
    // 元数据
    u32 sq_size;            // SQ 深度 (2048~8192)
    u32 cq_size;            // CQ 深度 (sq_size * 2)
    u32 buf_nbufs;          // 预注册缓冲区数量
    
    // 私有命令
    u8 uring_cmd_opcode;    // 私有命令号
};

// 初始化流程
int powerfs_uring_init(struct powerfs_uring_ctx *ctx) {
    // 1. 创建 io_uring
    ctx->ring_ctx = io_uring_ctx_alloc(
        IORING_SETUP_SQPOLL |      // SQPOLL 模式
        IORING_SETUP_CQSIZE,      // 自定义 CQ 大小
        ctx->sq_size,
        ctx->cq_size,
        GFP_KERNEL
    );
    
    // 2. 预注册缓冲区
    struct io_uring_params params = {};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_entries = ctx->sq_size;
    params.cq_entries = ctx->cq_size;
    
    // 注册大内存池 (可被 RDMA 复用)
    io_uring_register_buffers(ctx->ring_ctx, ctx->buf_base, ctx->buf_size, ...);
    
    // 3. 注册私有命令处理器
    io_uring_register_cmd_handler(ctx->ring_ctx, POWERFS_URING_CMD, powerfs_cmd_handler);
    
    return 0;
}
```

### 3.2 消息协议设计

#### 3.2.1 nanopb 轻量化序列化

```c
// nanopb 配置 (极小体积，适合内核态)
// 内核侧只保存 Protobuf 消息的裸字节
// 不使用动态分配，全部栈分配

// 消息类型枚举
typedef enum {
    // 元数据请求
    POWERFS_MSG_LOOKUP_REQ = 1,
    POWERFS_MSG_CREATE_REQ,
    POWERFS_MSG_MKDIR_REQ,
    POWERFS_MSG_UNLINK_REQ,
    POWERFS_MSG_RMDIR_REQ,
    POWERFS_MSG_RENAME_REQ,
    POWERFS_MSG_SYMLINK_REQ,
    POWERFS_MSG_READLINK_REQ,
    POWERFS_MSG_LINK_REQ,
    POWERFS_MSG_READDIR_REQ,
    POWERFS_MSG_SETATTR_REQ,
    POWERFS_MSG_GETATTR_REQ,
    
    // 数据请求
    POWERFS_MSG_READ_REQ,
    POWERFS_MSG_WRITE_REQ,
    POWERFS_MSG_FSYNC_REQ,
    POWERFS_MSG_TRUNCATE_REQ,
    
    // 统计请求
    POWERFS_MSG_STATFS_REQ,
    
    // 响应
    POWERFS_MSG_RESPONSE_BASE = 1000,
    POWERFS_MSG_LOOKUP_RESP,
    POWERFS_MSG_CREATE_RESP,
    // ... 对应每种请求的响应
    
    // 异步通知 (用户态→内核)
    POWERFS_MSG_INVALIDATE_NOTIFY = 2000,
} powerfs_msg_type_t;

// 内核侧发送的 RPC 请求结构
struct powerfs_rpc_request {
    u64 seq;                // 序列号 (用于匹配响应)
    u32 msg_type;           // 消息类型
    u32 payload_len;        // Protobuf 负载长度
    u8  payload[POWERFS_MAX_PAYLOAD];  // nanopb 序列化后的 Protobuf 数据
};

// 用户态代理回填的 RPC 响应结构
struct powerfs_rpc_response {
    u64 seq;                // 序列号 (对应请求)
    u32 result;             // 结果码 (0=成功, 负=错误)
    u32 msg_type;           // 消息类型
    u32 payload_len;        // Protobuf 负载长度
    u8  payload[POWERFS_MAX_PAYLOAD];  // nanopb 反序列化前的 Protobuf 数据
};
```

#### 3.2.2 nanopb 序列化示例

```c
// Lookup 请求内核打包 (nanopb 栈分配)
int powerfs_build_lookup_req(
    struct powerfs_rpc_request *req,
    u64 parent_ino,
    const char *name
) {
    // nanopb 栈分配
    LookupRequest lookup_req = {};
    lookup_req.parent_ino = parent_ino;
    strncpy(lookup_req.name, name, sizeof(lookup_req.name) - 1);
    
    // 序列化到 payload
    pb_ostream_t stream = pb_ostream_from_buffer(req->payload, POWERFS_MAX_PAYLOAD);
    bool status = pb_encode(&stream, LookupRequest_fields, &lookup_req);
    
    req->payload_len = stream.bytes_written;
    req->msg_type = POWERFS_MSG_LOOKUP_REQ;
    
    return status ? 0 : -EINVAL;
}

// 用户态代理反序列化 (nanopb)
int powerfs_parse_lookup_resp(
    struct powerfs_rpc_response *resp,
    struct LookupResponse *result
) {
    pb_istream_t stream = pb_istream_from_buffer(resp->payload, resp->payload_len);
    return pb_decode(&stream, LookupResponse_fields, result);
}
```

### 3.3 核心流程实现

#### 3.3.1 元数据同步流程 (mkdir 示例)

```
用户态 mkdir /powerfs/test
         │
         ▼
    Linux VFS
         │
         ▼
    powerfs_mkdir()  ← 内核模块
         │
         ├─ 1. 本地快速检查
         │     if (parent->dentry_cache contains "test")
         │         return -EEXIST;
         │
         ├─ 2. 构造 mkdir 请求
         │     pb_ostream_t stream;
         │     stream = pb_ostream_from_buffer(req->payload, POWERFS_MAX_PAYLOAD);
         │     pb_encode(&stream, MkDirRequest_fields, &mkdir_req);
         │
         ├─ 3. 提交 io_uring 请求
         │     struct io_uring_sqe *sqe = io_uring_get_sqe(ring_ctx);
         │     io_uring_prep_uring_cmd(sqe, POWERFS_URING_CMD_SYNC, 
         │                             req, sizeof(*req));
         │     io_uring_submit(ring_ctx);
         │
         ├─ 4. 等待响应 (同步模式)
         │     // 内核在此阻塞，直到用户态处理完成
         │     wait_for_completion_timeout(&ctx->done, timeout);
         │
         ├─ 5. 用户态代理处理 (异步)
         │     powerfs-proxy:
         │       poll() ← 等待 SQPOLL 唤醒
         │       // 从 SQ 获取请求
         │       io_uring_wait_cqe(ring_ctx, &cqe);
         │       // 反序列化 + gRPC 调用
         │       pb_istream_from_buffer(cqe->buf, ...)
         │       → grpc_client->MkDir(mkdir_req, &mkdir_resp)
         │       // 序列化响应 + 回填 CQ
         │       pb_ostream_from_buffer(uring_buf, ...)
         │       pb_encode(&stream, MkDirResponse_fields, &mkdir_resp);
         │       io_uring_prep_uring_cmd(sqe, POWERFS_URING_CMD_COMPLETE, 
         │                                 resp, sizeof(*resp));
         │       io_uring_submit(ring_ctx);
         │
         ├─ 6. 内核恢复执行
         │     // 从 CQ 获取响应
         │     io_uring_peek_cqe(ring_ctx, &cqe);
         │     parse_response(cqe, &mkdir_resp);
         │     if (mkdir_resp.error != 0) return mkdir_resp.error;
         │
         └─ 7. 更新本地缓存
               d_instantiate(dentry, inode);
               add_to_metadata_cache(parent, dentry);
               return 0;
```

#### 3.3.2 数据写入流程 (write 示例)

```
用户态 write(fd, buf, len)
         │
         ▼
    Linux VFS + 页缓存
         │
         ▼
    powerfs_write_begin() / powerfs_write_end()
         │
         ├─ 1. 写入页缓存 (立即返回)
         │     page_fault → write_begin
         │     memcpy(page, buf, len)
         │     write_end
         │     mark_page_dirty(page)
         │     return len;
         │
         ├─ 2. 后台 writeback
         │     powerfs_writepage()
         │     │
         │     ├─ 2.1 分配预注册缓冲区
         │     │     buf_idx = io_uring_get_buf(ring_ctx);
         │     │     // 该缓冲区同时注册在 RDMA MR 中
         │     │
         │     ├─ 2.2 拷贝数据到共享内存
         │     │     memcpy(ring_buf, page->data, PAGE_SIZE);
         │     │
         │     ├─ 2.3 异步提交写请求
         │     │     struct io_uring_sqe *sqe;
         │     │     sqe = io_uring_get_sqe(ring_ctx);
         │     │     io_uring_prep_uring_cmd(sqe, POWERFS_URING_CMD_WRITE, 
         │     │                             write_req, sizeof(*write_req));
         │     │     // 标记使用预注册缓冲区
         │     │     io_uring_sqe_set_buf(sqe, buf_idx, 0, PAGE_SIZE);
         │     │     io_uring_submit(ring_ctx);
         │     │     // 不阻塞 writeback 线程
         │     │     return 0;
         │     │
         │     └─ 2.4 标记回写状态
         │           SetPageWriteback(page);
         │
         └─ 3. 用户态代理处理 (异步)
               powerfs-proxy:
                 // 批量获取写请求
                 while ((cqe = io_uring_wait_cqe(ring_ctx))) {
                     // 直接使用预注册缓冲区 (零拷贝)
                     void *rdma_buf = ring_bufs[cqe->buf_idx];
                     // RDMA 发送
                     rdma_client->Write(rdma_buf, cqe->len);
                     // 回填确认
                     io_uring_prep_uring_cmd(sqe, POWERFS_URING_CMD_WRITE_ACK, 
                                                                    ack, sizeof(*ack));
                     io_uring_submit(ring_ctx);
                 }
                 
         └─ 4. 内核完成回写
               powerfs_complete_writeback()
               ClearPageWriteback(page);
               unlock_page(page);
```

#### 3.3.3 数据读取流程 (read 示例)

```
用户态 read(fd, buf, len)
         │
         ▼
    Linux VFS + 页缓存
         │
         ▼
    powerfs_readpage()
         │
         ├─ 1. 检查页缓存
         │     if (page_in_cache(inode, offset))
         │         // 缓存命中，直接返回
         │         memcpy(buf, page->data, len);
         │         return len;
         │
         ├─ 2. 缓存未命中 → 同步读取
         │     struct page *page = alloc_page();
         │     lock_page(page);
         │
         ├─ 3. 分配预注册缓冲区
         │     buf_idx = io_uring_get_buf(ring_ctx);
         │
         ├─ 4. 提交同步读请求
         │     struct io_uring_sqe *sqe;
         │     sqe = io_uring_get_sqe(ring_ctx);
         │     io_uring_prep_uring_cmd(sqe, POWERFS_URING_CMD_READ, 
         │                             read_req, sizeof(*read_req));
         │     // 指定接收数据的缓冲区
         │     io_uring_sqe_set_buf(sqe, buf_idx, 0, PAGE_SIZE);
         │     io_uring_submit(ring_ctx);
         │
         ├─ 5. 等待响应 (同步模式)
         │     wait_for_completion_timeout(&ctx->done, timeout);
         │
         ├─ 6. 用户态代理处理
         │     powerfs-proxy:
         │       // 获取读请求
         │       io_uring_wait_cqe(ring_ctx, &cqe);
         │       // 从预注册缓冲区直接 RDMA 发送读请求
         │       // RDMA 读取数据后直接写入预注册缓冲区
         │       rdma_client->Read(cqe->buf_idx, len);
         │       // 回填完成状态
         │       io_uring_prep_uring_cmd(sqe, POWERFS_URING_CMD_READ_COMPLETE, 
         │                                 complete_req, sizeof(*complete_req));
         │       io_uring_submit(ring_ctx);
         │
         ├─ 7. 内核恢复执行
         │     // 直接从预注册缓冲区拷贝到页缓存
         │     memcpy(page->data, ring_bufs[buf_idx], len);
         │     SetPageUptodate(page);
         │
         └─ 8. 返回数据
               unlock_page(page);
               // 预注册缓冲区归还
               io_uring_put_buf(ring_ctx, buf_idx);
               return len;
```

---

## 四、实施计划

### 阶段 1：MVP 元数据验证 (4-6 周)

**目标**：验证 io_uring 通信机制，实现元数据操作（仅元数据）

**决策依据**：先验证架构可行性，MVP 阶段聚焦元数据路径

**任务清单**：

#### 4.1.1 内核模块基础
- [x] QEMU 虚拟化开发环境搭建
- [x] 切换到 Linux 6.2 内核分支
- [ ] powerfs.ko 基础框架：
  - [ ] super_operations 实现（mount/umount）
  - [ ] 基本 inode 生命周期管理
  - [ ] 简单的 dentry 哈希缓存
- [ ] io_uring 集成（6.2 内核）：
  - [ ] 初始化 io_uring 上下文（SQPOLL 模式）
  - [ ] 预注册缓冲区池（256 个缓冲区，每个 64KB）
  - [ ] 私有 URING_CMD 处理函数

#### 4.1.2 用户态代理基础（混合架构）
- [ ] C/C++ 核心层：
  - [ ] io_uring 管理（liburing）
  - [ ] SQPOLL 轮询机制
  - [ ] 预注册缓冲区管理
  - [ ] nanopb 序列化/反序列化
  - [ ] FFI 接口定义（供 Rust 调用）
- [ ] Rust 外围层：
  - [ ] gRPC 客户端（tonic）
  - [ ] 复用现有 PowerFS FUSE 代码
  - [ ] 桥接 C/C++ 核心层

#### 4.1.3 元数据操作实现（MVP 范围）
- [ ] 实现核心元数据操作：
  - [ ] **lookup**（查找目录项）
  - [ ] **mkdir / rmdir**（创建/删除目录）
  - [ ] **create / unlink**（创建/删除文件）
  - [ ] **symlink / readlink**（符号链接）
  - [ ] **getattr**（基础属性获取）
- [ ] 实现失效通知机制：
  - [ ] 用户态接收 Master 失效通知（gRPC）
  - [ ] 通过 io_uring 回填内核
  - [ ] 内核执行 d_invalidate / invalidate_inode_pages

#### 4.1.4 测试环境搭建
- [ ] Docker 容器组启动：
  - [ ] Master 服务
  - [ ] Filer 服务
  - [ ] 容器组网桥配置
- [ ] QEMU 虚拟机网络配置：
  - [ ] 桥接网卡连接容器网络
  - [ ] 验证网络连通性
- [ ] 验证测试：
  - [ ] mount -t powerfs 成功
  - [ ] ls, mkdir, touch, rm, ln 等命令正常
  - [ ] 单并发 lookup 延迟测试（目标 < 1ms 缓存命中）

### 阶段 2：功能完善 (6-8 周)

**目标**：实现完整文件系统功能，包括数据路径

**任务清单**：

#### 4.2.1 元数据完善
- [ ] 实现所有元数据操作：
  - [ ] rename, link
  - [ ] readdir (迭代目录)
  - [ ] getattr, setattr
  - [ ] statfs
- [ ] 优化元数据缓存：
  - [ ] TTL 失效机制
  - [ ] 主动失效
  - [ ] 并发安全

#### 4.2.2 数据路径实现
- [ ] address_space_operations:
  - [ ] readpage, writepage
  - [ ] readpages (批量读)
  - [ ] writepages (批量写)
- [ ] file_operations:
  - [ ] open, release
  - [ ] fsync, flush
  - [ ] mmap 支持
  - [ ] truncate
- [ ] read-ahead 预读优化

#### 4.2.3 RPC 批量优化
- [ ] 内核侧请求合并：
  - [ ] 多个小请求合并为一次 io_uring submit
  - [ ] 流水线并发 RPC
- [ ] 用户态批量处理：
  - [ ] 批量收割 CQ
  - [ ] gRPC 批量请求 (MkFileBatch 等)

#### 4.2.4 稳定性测试
- [ ] 用户态代理崩溃恢复
- [ ] 网络断线重连
- [ ] 数据一致性校验
- [ ] 并发压力测试

### 阶段 3：性能优化 (4-6 周)

**目标**：达到或超越 FUSE 版本性能

**任务清单**：

#### 4.3.1 RDMA 集成
- [ ] 引入 grpc-rdma：
  - [ ] RDMA 客户端初始化
  - [ ] 与 io_uring 内存池共享
  - [ ] 连接管理和故障转移
- [ ] RDMA 读优化：
  - [ ] Zero-copy RDMA read
  - [ ] 直接写入 io_uring 缓冲区
  - [ ] 直接写入页缓存

#### 4.3.2 性能调优
- [ ] io_uring 参数调优：
  - [ ] SQ/CQ 深度
  - [ ] 预注册缓冲区大小
  - [ ] SQPOLL 配置
- [ ] 缓存策略优化：
  - [ ] 元数据缓存 TTL
  - [ ] 预读窗口大小
  - [ ] 写回频率
- [ ] 内存管理优化：
  - [ ] 减少内核内存分配
  - [ ] 大页支持 (HugeTLB)

#### 4.3.3 基准测试
- [ ] 元数据基准：
  - [ ] mdtest (小文件元数据)
  - [ ] 单并发/多并发 lookup
- [ ] 数据基准：
  - [ ] fio (随机读写)
  - [ ] IO500 (综合)
  - [ ] 大文件吞吐
- [ ] 对比测试：
  - [ ] vs FUSE 版本性能
  - [ ] vs 其他文件系统 (NFS, CephFS)

---

## 五、技术栈选型

### 5.1 内核侧
| 组件 | 技术 | 说明 |
|------|------|------|
| 编程语言 | C | 内核模块标准语言 |
| io_uring | Linux 5.19+ | 必须支持 URING_CMD |
| Protobuf | nanopb | 极小体积，栈分配 |
| 页缓存 | 内核 page cache | 利用 Linux 原生缓存机制 |

### 5.2 用户态（混合架构）
| 组件 | 技术 | 说明 |
|------|------|------|
| **核心层 (C/C++)** | | |
| 编程语言 | C (C11) | io_uring 管理、内存操作 |
| io_uring 库 | liburing | io_uring 用户态接口 |
| Protobuf | nanopb (C) | 内核侧序列化 |
| 共享内存操作 | POSIX shm | 零拷贝数据传输 |
| **外围层 (Rust)** | | |
| 编程语言 | Rust | gRPC 客户端、业务逻辑 |
| gRPC | tonic | Rust gRPC 标准库 |
| Protobuf | prost | Rust Protobuf 支持 |
| 运行时 | Tokio | 异步运行时 |
| **桥接层** | | |
| FFI | bindgen | C/C++ ↔ Rust 接口绑定 |
| 共享内存 | mmap | 数据传输通道 |

### 5.3 内核版本要求
- **目标版本**：Linux 6.2+（`ubuntu-linux-git` 已切换到 v6.2 分支）
- **关键特性**：
  - URING_CMD（私有 io_uring 命令）
  - SQPOLL（内核轮询提交队列）
  - Big SQE（128 字节大消息）
- **测试环境**：QEMU 虚拟机 + 6.2 内核

---

## 六、风险与缓解措施

### 6.1 技术风险

| 风险 | 影响 | 概率 | 缓解措施 |
|------|------|------|----------|
| io_uring 版本兼容性 | 中 | 中 | 重点支持 6.2+，条件编译处理 |
| nanopb 功能限制 | 低 | 低 | 仅使用基础类型，复杂结构预编码 |
| RDMA 驱动兼容性 | 中 | 中 | 支持主流 OFED 版本，做好降级处理 |
| 内核锁竞争 | 高 | 中 | 仔细分析锁依赖，避免死锁 |

### 6.2 性能风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 小 RPC 批量效果差 | 低延迟场景退化 | 动态批量策略，小请求立即提交 |
| 缓存命中率低 | 元数据路径性能下降 | 自适应缓存策略 + 主动失效 |
| RDMA 初始化开销 | 冷启动慢 | 延迟初始化 + 预热机制 |

### 6.3 稳定性风险

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 用户态代理崩溃 | 文件系统不可用 | 内核检测 + 自动重连 |
| RDMA 网卡故障 | 数据路径中断 | TCP fallback + 故障转移 |
| 内存泄漏 | 系统不稳定 | 严格内存管理 + 诊断工具 |

---

## 七、已确认决策

### ✅ D1：内核版本支持范围
**决策**：仅支持 Linux 6.2+
- **原因**：io_uring URING_CMD、SQPOLL 等关键特性在 6.2+ 版本中稳定
- **内核源码**：`ubuntu-linux-git` 已切换到 v6.2 分支
- **测试环境**：QEMU 虚拟机使用 6.2 内核

### ✅ D2：用户态代理编程语言
**决策**：混合方案（核心 C/C++，外围 Rust）
- **C/C++ 核心**：
  - io_uring 管理（liburing C API）
  - 共享内存操作
  - nanopb 序列化（C 实现）
  - 高性能数据路径
- **Rust 外围**：
  - gRPC 客户端（复用现有 FUSE 代码）
  - 业务逻辑封装
  - 错误处理和恢复
- **通信方式**：通过 FFI 或共享内存桥接 C/C++ 和 Rust 部分

### ✅ D3：RDMA 支持优先级
**决策**：先 TCP，后续加 RDMA
- **阶段 1-2**：使用 TCP 进行基础验证
- **阶段 3**：引入 RDMA 优化
- **优势**：降低初期复杂度，快速验证架构可行性

### ✅ D4：测试环境与容器支持
**决策**：暂不需要容器，VM 通过容器组网桥连接
- **架构**：
  ```
  ┌─────────────────────────────────────┐
  │  主机 (物理机)                       │
  │  ┌───────────────────────────────┐  │
  │  │  Docker 容器组                 │  │
  │  │  ├─ Master (gRPC)              │  │
  │  │  ├─ Filer                      │  │
  │  │  └─ Volume Server (gRPC+RDMA)  │  │
  │  └───────────────┬───────────────┘  │
  │                  │ 容器组网桥          │
  │  ┌───────────────┴───────────────┐  │
  │  │  QEMU 虚拟机 (VM)              │  │
  │  │  ├─ PowerFS 内核模块           │  │
  │  │  └─ powerfs-proxy (用户态)     │  │
  │  └───────────────────────────────┘  │
  └─────────────────────────────────────┘
  ```
- **网络配置**：VM 通过桥接网卡连接容器网络
- **后续扩展**：生产环境通过原生 mount 支持容器

### ✅ D5：缓存一致性模型
**决策**：混合模式（元数据弱一致，数据强一致）
- **元数据操作**（lookup, mkdir, create 等）：
  - 弱一致：本地缓存优先，TTL 过期或主动失效时刷新
  - 原因：元数据操作频繁，强一致会严重影响性能
- **数据操作**（read, write）：
  - 强一致：写入确认后返回，读取从主副本获取
  - 原因：数据一致性要求更高
- **失效机制**：
  - Master 主动推送失效通知
  - 缓存 TTL 自动过期
  - 用户态代理监听并通知内核

### ✅ D6：MVP 阶段范围
**决策**：仅实现元数据操作
- **MVP 功能清单**：
  - lookup（查找目录项）
  - mkdir / rmdir（创建/删除目录）
  - create / unlink（创建/删除文件）
  - symlink / readlink（符号链接）
  - 基础属性获取（getattr）
- **暂不实现**：
  - 数据读写（read/write）
  - 目录迭代（readdir）
  - 重命名（rename）
  - RDMA 支持

---

## 八、参考资料

### 8.1 内核文档
- io_uring: [Documentation/core-api/io_uring.rst](https://docs.kernel.org/core-api/io_uring.html)
- VFS: [Documentation/filesystems/vfs.rst](https://docs.kernel.org/filesystems/vfs.rst)
- Page Cache: [Documentation/core-api/cache.rst](https://docs.kernel.org/core-api/cache.rst)

### 8.2 相关实现
- CephFS: `ubuntu-linux-git/fs/ceph/`
- BeeGFS: `beegfs/client_module/`
- FUSE: `ubuntu-linux-git/fs/fuse/`

### 8.3 用户态库
- liburing: https://github.com/axboe/liburing
- nanopb: https://github.com/nanopb/nanopb
- tonic (Rust gRPC): https://github.com/hyperium/tonic
- grpc-rdma: https://github.com/grpc-ecosystem/grpc-rdma

---

**文档版本**: v1.1  
**最后更新**: 2026-07-24  
**作者**: PowerFS 开发团队

---

## 附录：混合编程架构详细设计

---

## 九、阶段0：MVP过渡方案（已确认）

> **更新时间**: 2026-07-25  
> **背景**: 为快速验证内核文件系统架构可行性，采用简化实现作为过渡方案

### 9.1 过渡方案核心决策

| 组件 | 目标架构（阶段1+） | 过渡方案（阶段0） | 迁移计划 |
|------|-------------------|------------------|----------|
| **通信层** | io_uring URING_CMD + SQPOLL | 自定义字符设备 `/dev/powerfs_comm` + ioctl + mmap | 保留 SQ/CQ 队列抽象，后续替换底层传输为 io_uring |
| **序列化** | nanopb 裸 Protobuf | 硬编码 C 结构体直接传递 | 引入序列化抽象层，当前用 memcpy 实现，后续替换为 nanopb |
| **用户态代理** | C/C++ 核心 + Rust 外围（混合架构） | 纯 C 实现（单进程） | 先实现 C 核心层接口，后续叠加 Rust gRPC 外围 |
| **后端服务** | 真实 PowerFS gRPC 服务 | 模拟响应（内存存储） | 保持接口兼容，后续替换为真实 gRPC 调用 |
| **网络配置** | Docker 容器组网桥 + QEMU 桥接 | QEMU user-mode + 端口转发 | 切换为桥接网卡时无需修改代码 |

### 9.2 过渡方案架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                    用户态 (VM 内部)                               │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  PowerFS 过渡代理 (powerfs-proxy)                           │ │
│  │                                                             │ │
│  │  ┌─ 消息处理层 ──────────────────────────────────────────┐ │ │
│  │  │  • 从 SQ 获取请求 (ioctl/mmap)                        │ │ │
│  │  │  • 解析请求类型 (switch-case)                          │ │ │
│  │  │  • 模拟后端响应 (内存存储)                             │ │ │
│  │  │  • 回填响应到 CQ                                       │ │ │
│  │  │  • 支持 INVALIDATE_NOTIFY 主动失效                     │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  │                                                             │ │
│  │  ┌─ 数据存储层 ──────────────────────────────────────────┐ │ │
│  │  │  • 模拟 inode/dentry 存储 (哈希表)                    │ │ │
│  │  │  • 模拟文件数据存储 (内存 buffer)                      │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────────────┘ │
│                    ↕ 字符设备 + mmap (零拷贝共享内存)             │
├─────────────────────────────────────────────────────────────────┤
│                    内核态 (powerfs.ko)                           │
│                                                                  │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │  PowerFS 内核文件系统                                       │ │
│  │                                                             │ │
│  │  ┌─ VFS 操作层 ───────────────────────────────────────────┐ │ │
│  │  │  • super_operations: mount/umount                      │ │ │
│  │  │  • inode_operations:                                   │ │ │
│  │  │    - lookup, mkdir, rmdir, create, unlink              │ │ │
│  │  │    - symlink, readlink, rename                         │ │ │
│  │  │    - getattr, setattr                                  │ │ │
│  │  │  • file_operations: read, write, mmap                  │ │ │
│  │  │  • address_space_operations:                          │ │ │
│  │  │    - read_folio, writepage, write_begin/end            │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  │                                                             │ │
│  │  ┌─ 缓存管理层 ──────────────────────────────────────────┐ │ │
│  │  │  • dentry TTL 缓存有效性                              │ │ │
│  │  │  • inode 缓存有效期                                   │ │ │
│  │  │  • page cache 集成                                    │ │ │
│  │  │  • 主动失效通知处理 (INVALIDATE_NOTIFY)                │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  │                                                             │ │
│  │  ┌─ 通信层 (powerfs_transport.c) ────────────────────────┐ │ │
│  │  │  • 字符设备注册 (/dev/powerfs_comm)                    │ │ │
│  │  │  • mmap 共享内存分配 (SQ/CQ + 数据区)                  │ │ │
│  │  │  • ioctl 命令处理 (GET_REQ/SUBMIT_RESP/PING)          │ │ │
│  │  │  • poll 等待队列 (用户态代理等待新请求)                │ │ │
│  │  │  • 序列化抽象层 (预留 nanopb 替换点)                   │ │ │
│  │  └────────────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 9.3 过渡方案消息类型定义

```c
/* ========== 过渡方案消息类型 (powerfs_comm.h) ========== */

/* 基础通信 */
POWERFS_MSG_NONE = 0,
POWERFS_MSG_PING = 1,          /* 测试连接 */

/* 元数据请求 (MVP) */
POWERFS_MSG_LOOKUP = 10,       /* 查找目录项 */
POWERFS_MSG_GETATTR = 11,      /* 获取属性 */
POWERFS_MSG_SETATTR = 12,      /* 设置属性 */
POWERFS_MSG_MKDIR = 13,        /* 创建目录 */
POWERFS_MSG_CREATE = 14,       /* 创建文件 */
POWERFS_MSG_UNLINK = 15,       /* 删除文件 */
POWERFS_MSG_RMDIR = 16,        /* 删除目录 */
POWERFS_MSG_RENAME = 17,       /* 重命名 */
POWERFS_MSG_READDIR = 18,      /* 读取目录 */
POWERFS_MSG_SYMLINK = 19,      /* 创建符号链接 */
POWERFS_MSG_READLINK = 20,     /* 读取符号链接目标 */
POWERFS_MSG_LINK = 21,         /* 创建硬链接 */
POWERFS_MSG_MKNOD = 22,        /* 创建设备节点 */

/* 数据路径 (超前实现) */
POWERFS_MSG_READ = 30,         /* 读取数据 */
POWERFS_MSG_WRITE = 31,        /* 写入数据 */
POWERFS_MSG_FSYNC = 32,        /* 数据同步 */
POWERFS_MSG_TRUNCATE = 33,     /* 文件截断 */

/* 文件系统统计 */
POWERFS_MSG_STATFS = 40,       /* 获取文件系统统计 */

/* 主动失效通知 (用户态→内核) */
POWERFS_MSG_INVALIDATE_NOTIFY = 50,  /* dentry/inode 失效 */

/* 失效类型 */
POWERFS_INVALIDATE_DENTRY = (1 << 0),   /* dentry 失效 */
POWERFS_INVALIDATE_INODE = (1 << 1),    /* inode 失效 */
POWERFS_INVALIDATE_ALL = (1 << 2),      /* 全部失效 */
```

### 9.4 序列化抽象层设计

```c
/* ========== 序列化抽象层 ========== */

/* 序列化操作接口 */
struct powerfs_serializer {
    /* 将请求结构体序列化为字节流 */
    int (*serialize_request)(uint32_t msg_type, void *req_struct, 
                            uint32_t *out_len, uint8_t *out_buf);
    
    /* 从字节流反序列化为响应结构体 */
    int (*deserialize_response)(uint32_t msg_type, uint8_t *in_buf,
                                uint32_t in_len, void *resp_struct);
    
    /* 初始化序列化器 */
    int (*init)(void);
    
    /* 销毁序列化器 */
    void (*destroy)(void);
};

/* 切换序列化器 (运行时可替换) */
int powerfs_set_serializer(struct powerfs_serializer *serializer);

/* 当前默认实现: 直接 memcpy (过渡方案) */
int powerfs_serialize_direct(uint32_t msg_type, void *req_struct,
                            uint32_t *out_len, uint8_t *out_buf);
int powerfs_deserialize_direct(uint32_t msg_type, uint8_t *in_buf,
                               uint32_t in_len, void *resp_struct);

/* 未来实现: nanopb (阶段1+) */
int powerfs_serialize_nanopb(uint32_t msg_type, void *req_struct,
                            uint32_t *out_len, uint8_t *out_buf);
int powerfs_deserialize_nanopb(uint32_t msg_type, uint8_t *in_buf,
                               uint32_t in_len, void *resp_struct);
```

### 9.5 INVALIDATE_NOTIFY 通道设计

```c
/* ========== 主动失效通知机制 ========== */

/* 失效通知请求结构 */
struct powerfs_invalidate_req {
    __u32 flags;                /* 失效类型 (POWERFS_INVALIDATE_*) */
    __u32 count;                /* 失效条目数量 */
    __u64 ino[];                /* 要失效的 inode 号数组 */
};

/* 内核侧失效处理 */
int powerfs_handle_invalidate(struct powerfs_invalidate_req *req);
/* 处理流程:
 * 1. 根据 flags 确定失效范围
 * 2. 遍历 ino 数组
 * 3. 从 inode 哈希表中查找对应 inode
 * 4. 调用 invalidate_inode_pages2(inode) 失效页缓存
 * 5. 标记 dentry 为无效 (d_invalidate)
 * 6. 重置 inode 缓存有效期
 */

/* 用户态代理发送失效通知 */
int powerfs_send_invalidate(struct powerfs_invalidate_req *req);
/* 通过 char device 的 write/ioctl 通道发送 */

/* ioctl 命令 */
#define POWERFS_IOCTL_INVALIDATE \
    _IOW(POWERFS_IOCTL_MAGIC, 20, struct powerfs_invalidate_req)
```

### 9.6 过渡方案验证测试清单

#### 已完成 ✅
- [x] QEMU 虚拟化开发环境搭建
- [x] Linux 6.2 内核编译与启动
- [x] PowerFS 内核模块基础框架
- [x] 字符设备通信层实现
- [x] mmap 共享内存 (260KB, 64 请求)
- [x] MVP 元数据操作：
  - [x] lookup (查找目录项)
  - [x] mkdir / rmdir (创建/删除目录)
  - [x] create / unlink (创建/删除文件)
  - [x] symlink / readlink (符号链接)
  - [x] link (硬链接)
  - [x] getattr / setattr (属性获取/设置)
  - [x] rename (重命名)
  - [x] readdir (目录迭代)
- [x] 数据路径超前实现：
  - [x] read (读取数据)
  - [x] write (写入数据)
  - [x] fsync (数据同步)
  - [x] truncate (文件截断)
- [x] 用户态代理基础实现
- [x] 模拟后端响应 (内存存储)
- [x] 序列化抽象层实现 (direct memcpy 模式)
- [x] INVALIDATE_NOTIFY 通道实现
- [x] 持久化存储功能
- [x] 基本功能测试通过 (创建/读写/删除/符号链接/硬链接)
- [x] 持久化验证测试通过 (重新挂载后数据恢复)

#### 进行中 ⏳
- [ ] nanopb 集成 (阶段1+)
- [ ] 并发压力测试

#### 待实现 ⏸️
- [ ] 预注册缓冲区池 (256×64KB)
- [ ] 多请求批量处理优化
- [ ] 真实 gRPC 后端对接
- [ ] OR-Set CRDT 弱一致模型
- [ ] Docker 容器组网桥配置
- [ ] RDMA 传输层集成
- [ ] 性能基准测试

### 9.7 迁移到目标架构的路径

```
阶段0 (当前) ──────────────────────────────────────────────────┐
  │ 字符设备 + ioctl                                              │
  │ C 结构体直接传递                                               │
  │ 纯 C 代理 + 模拟响应                                          │
  │ QEMU user-mode 网络                                            │
  │                                                                │
  ▼ 迁移步骤                                                      │
  1. 引入序列化抽象层（当前 memcpy → 可替换为 nanopb）             │
  2. 扩展 SQ/CQ 队列深度（64 → 2048+）                            │
  3. 实现批量请求处理                                              │
  4. 对接真实 gRPC 后端（保持接口兼容）                            │
  5. 切换网络配置为 Docker 容器组网桥                              │
  │                                                                │
阶段1+ (目标) ────────────────────────────────────────────────── ──┘
  │ io_uring URING_CMD + SQPOLL
  │ nanopb Protobuf 序列化
  │ C/C++ 核心 + Rust 外围 (混合架构)
  │ RDMA 传输层
  │ Docker 容器组网桥
```

### 9.8 风险与缓解

| 风险 | 影响 | 概率 | 缓解措施 |
|------|------|------|----------|
| 过渡代码与目标架构差异过大 | 迁移困难 | 中 | 在抽象层设计时预留清晰接口 |
| 性能无法达到目标要求 | 需要大幅重构 | 中 | 阶段0只验证功能正确性，性能留到阶段3优化 |
| 序列化抽象层设计不当 | 后续需要重构 | 低 | 参考成熟方案（cephfs, FUSE）的接口设计 |
| 用户态代理与后端接口不兼容 | 对接困难 | 中 | 保持 protobuf 消息格式兼容 |

---

**文档版本**: v1.3 (更新实施进度)  
**最后更新**: 2026-07-25  
**作者**: PowerFS 开发团队

---

## 附录：混合编程架构详细设计

### A1：C/C++ 核心层接口定义

```c
// powerfs_proxy.h - C/C++ 核心层 API

// io_uring 上下文
struct powerfs_uring_ctx;

// 初始化/销毁
int powerfs_proxy_init(const char *shmem_path, size_t shmem_size);
void powerfs_proxy_destroy(void);

// 同步请求 (阻塞等待响应)
int powerfs_sync_request(uint32_t msg_type, const void *req_data, 
                         uint32_t req_len, void *resp_data, 
                         uint32_t *resp_len, uint32_t timeout_ms);

// 异步请求 (立即返回)
int powerfs_async_request(uint32_t msg_type, const void *req_data,
                          uint32_t req_len, uint64_t *seq);

// 批量请求 (优化用)
int powerfs_batch_submit(struct powerfs_request *requests, 
                         uint32_t count);

// 缓冲区管理 (供 Rust 层直接操作)
void* powerfs_get_buffer(uint32_t *buf_idx);
void powerfs_put_buffer(uint32_t buf_idx);
void* powerfs_buffer_to_addr(uint32_t buf_idx);

// nanopb 序列化/反序列化
int powerfs_serialize_proto(uint32_t msg_type, const void *proto_struct,
                            void *buf, uint32_t buf_size, uint32_t *out_len);
int powerfs_deserialize_proto(uint32_t msg_type, const void *buf,
                              uint32_t buf_len, void *proto_struct);

// 事件通知 (内核→用户态)
int powerfs_wait_event(uint32_t timeout_ms);  // 等待内核新请求
int powerfs_get_pending_count(void);  // 获取待处理请求数
```

### A2：Rust 外围层接口定义

```rust
// powerfs-proxy/src/lib.rs - Rust 外围层

// 通过 FFI 调用 C/C++ 核心层
#[link(name = "powerfs_proxy")]
extern "C" {
    fn powerfs_proxy_init(shmem_path: *const c_char, shmem_size: u64) -> i32;
    fn powerfs_proxy_destroy();
    fn powerfs_sync_request(msg_type: u32, req_data: *const u8,
                            req_len: u32, resp_data: *mut u8,
                            resp_len: *mut u32, timeout_ms: u32) -> i32;
    fn powerfs_wait_event(timeout_ms: u32) -> i32;
}

// gRPC 服务封装
impl PowerFSProxy {
    // 处理内核请求
    async fn handle_kernel_request(&self, req: &[u8]) -> Result<Vec<u8>>;
    
    // 连接 Master
    async fn connect_master(&self, addr: &str) -> Result<()>;
    
    // 监听失效通知
    async fn listen_invalidation(&self);
    
    // 启动主循环
    async fn run(&self) -> Result<()>;
}

// 主循环伪代码
async fn main_loop() {
    loop {
        // 1. 等待内核新请求
        let event = powerfs_wait_event(100);  // 100ms 超时
        
        // 2. 获取待处理请求
        let count = powerfs_get_pending_count();
        
        // 3. 批量处理
        for _ in 0..count {
            // 3.1 从共享内存读取请求
            // 3.2 调用 gRPC 客户端
            // 3.3 写回响应到共享内存
        }
        
        // 4. 检查失效通知
        // (通过 gRPC streaming 接收)
    }
}
```

### A3：容器组网桥配置示例

```bash
#!/bin/bash
# setup_bridge.sh - 配置容器组网桥

# 创建网桥
docker network create \
  --driver bridge \
  --subnet 172.20.0.0/24 \
  --gateway 172.20.0.1 \
  powerfs-net

# 启动 Master
docker run -d \
  --name powerfs-master \
  --network powerfs-net \
  --ip 172.20.0.10 \
  -p 9334:9334 \
  powerfs/master:latest

# 启动 Filer
docker run -d \
  --name powerfs-filer \
  --network powerfs-net \
  --ip 172.20.0.11 \
  -p 9335:9335 \
  powerfs/filer:latest

# 启动 Volume Server
docker run -d \
  --name powerfs-volume \
  --network powerfs-net \
  --ip 172.20.0.12 \
  -p 9336:9336 \
  powerfs/volume:latest
```

### A4：QEMU 网络配置示例

```bash
#!/bin/bash
# run_qemu.sh - 启动 QEMU 虚拟机

# 创建 TAP 设备
tunctl -t tap0
ip link set tap0 up
ip addr add 172.20.0.100/24 dev tap0

# 启动 QEMU
qemu-system-x86_64 \
  -m 4G \
  -smp 4 \
  -kernel bzImage \
  -initrd initramfs.cpio.gz \
  -append "console=ttyS0 root=/dev/ram0 rw" \
  -netdev tap,id=net0,ifname=tap0,script=no \
  -device virtio-net-pci,netdev=net0 \
  -enable-kvm
```
