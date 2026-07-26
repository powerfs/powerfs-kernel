# PowerFS 轻量网络通信层设计 (powerfs-net)

## 一、背景与动机

### 1.1 问题分析

当前 PowerFS 的通信架构存在以下问题：

| 组件 | 问题 |
|------|------|
| **gRPC 协议** | 基于 HTTP/2 + Protobuf，内核态不可用，序列化开销大 |
| **内核代理模式** | 进程间通信复杂，RCU stall 频发，死锁风险高 |
| **io_uring 代理** | 增加不必要的用户态跳转，延迟翻倍 |

### 1.2 设计目标

设计一套 **轻量级二进制网络协议**，满足：

- ✅ **内核态可用**：使用标准 Linux 内核 socket API
- ✅ **用户态可用**：Rust 实现，复用现有 FUSE 客户端代码
- ✅ **高性能**：零拷贝、批量处理、连接复用
- ✅ **双端统一**：协议格式完全一致，仅传输层实现不同
- ✅ **渐进增强**：支持从 TCP 到 RDMA 的平滑升级

---

## 二、参考架构

### 2.1 Ceph msgr2 协议借鉴

Ceph 的 msgr2 协议是成熟的分布式存储通信协议：

```
连接阶段:
1. Banner 协商 (feature bits)
2. Authentication 交换
3. Message Flow Handshake
4. 消息帧交换

帧格式 (32-byte preamble):
┌──────────────────────────────────────────────────────┐
│ tag (1B) │ segments (1B) │ segment_length[4]*4 │ ... │
│ reserved (2B) │ preamble_crc (4B)                     │
├──────────────────────────────────────────────────────┤
│ segment payload (变长)                                  │
├──────────────────────────────────────────────────────┤
│ epilogue (17B): late_flags + segment_crc[4]*4          │
└──────────────────────────────────────────────────────┘
```

**核心设计要点**：
- 4-segment 支持分离 header/middle/data/footer
- CRC32-C 校验保证数据完整性
- `late_flags` 支持帧取消 (abort)
- reconnect + keepalive 机制保证连接可靠性

### 2.2 BeeGFS 通信层借鉴

BeeGFS 的内核客户端直接连元数据/存储服务器：

```
客户端内核模块 ──TCP──► 元数据服务器 (MDS)
              └──TCP──► 存储服务器 (SS)
              
特点：
- 无代理层，直连模式
- 二进制协议，高效序列化
- RDMA 可选支持
- 内核 socket API 实现
```

---

## 三、powerfs-net 协议设计

### 3.1 总体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        应用层                                    │
│  FUSE 客户端 (Rust)          内核文件系统 (C)                    │
│  ┌─────────────┐             ┌─────────────┐                    │
│  │ MetadataOps │             │ VFS Ops     │                    │
│  │ DataOps     │             │ Page Cache  │                    │
│  └──────┬──────┘             └──────┬──────┘                    │
├─────────┼───────────────────────────┼───────────────────────────┤
│         ▼                           ▼                             │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │                    powerfs-net 统一接口                     │ │
│  │  ┌─────────────────────────────────────────────────────────┐ │ │
│  │  │  Message API: send_request / send_notify / recv_response │ │ │
│  │  └─────────────────────────────────────────────────────────┘ │ │
│  └─────────────────────────────────────────────────────────────┘ │
│         ▼                           ▼                             │
│  ┌───────────────────────┐   ┌───────────────────────┐           │
│  │  Rust Transport       │   │  Kernel Transport    │           │
│  │  (tokio TCP/RDMA)    │   │  (sock_create_kernel) │           │
│  └──────────┬────────────┘   └──────────┬────────────┘           │
├──────────────┼───────────────────────────┼────────────────────────┤
│              ▼                           ▼                        │
│     TCP/RDMA 网络                TCP 网络                          │
└─────────────────────────────────────────────────────────────────┘
                    ↕ TCP/RDMA
┌─────────────────────────────────────────────────────────────────┐
│                    PowerFS 服务端                                 │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │              powerfs-net Server Listener                    │ │
│  │  ┌───────────────┐  ┌───────────────┐  ┌───────────────┐   │ │
│  │  │ Master Handler│  │ Filer Handler │  │ Volume Handler│   │ │
│  │  └───────────────┘  └───────────────┘  └───────────────┘   │ │
│  └─────────────────────────────────────────────────────────────┘ │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │              现有 gRPC 服务 (保留，用于管理面)               │ │
│  └─────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 Wire Protocol 格式

#### 3.2.1 连接握手

```
客户端                                    服务端
  │                                        │
  │──── Banner (16B) ──────────────────►│
  │   "PFSNv1\n" + features(8B)          │
  │                                        │
  │◄─────────── Banner (16B) ───────────│
  │   "PFSNv1\n" + features(8B)          │
  │                                        │
  │──── Connect Request ────────────────►│
  │   (client_id, client_type, version)   │
  │                                        │
  │◄─────────── Connect Response ───────│
  │   (success, server_id, epoch)        │
  │                                        │
  │════════ 消息交换阶段 ════════════════│
```

**Banner 格式** (16 字节)：
```c
#define PFSN_BANNER_LEN 16
#define PFSN_BANNER_MAGIC "PFSNv1\n"  // 8 bytes

struct pfsn_banner {
    char magic[8];           // "PFSNv1\n"
    uint64_t features;       // feature bits
};

// Feature bits
#define PFSN_FEATURE_TCP       (1ULL << 0)  // TCP 支持
#define PFSN_FEATURE_RDMA      (1ULL << 1)  // RDMA 支持
#define PFSN_FEATURE_CRC       (1ULL << 2)  // CRC32C 校验
#define PFSN_FEATURE_ASYNC     (1ULL << 3)  // 异步通知
#define PFSN_FEATURE_BATCH     (1ULL << 4)  // 批量消息
```

#### 3.2.2 消息帧格式

```
┌─────────────────────────────────────────────────────────┐
│                    Frame Header (32B)                    │
│  ┌─────────────────────────────────────────────────┐    │
│  │ tag (1B)        - 帧类型标识                      │    │
│  │ flags (1B)      - 控制标志                      │    │
│  │ num_segments (2B) - 段数量 (1-4)                 │    │
│  │ seq (4B)        - 序列号                        │    │
│  │ ack_seq (4B)    - 确认号                        │    │
│  │ msg_type (4B)   - 业务消息类型                   │    │
│  │ data_len (4B)   - 数据总长度                     │    │
│  │ segment_len[4] (16B) - 各段长度                 │    │
│  │ header_crc (4B) - Header CRC32C                 │    │
│  └─────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────┤
│ Segment 0: Header Payload (变长)                         │
│   请求/响应元数据 (序列化的 protobuf 或自定义)           │
├─────────────────────────────────────────────────────────┤
│ Segment 1: Middle Payload (变长, 可选)                   │
│   扩展数据 (如文件属性、目录列表)                        │
├─────────────────────────────────────────────────────────┤
│ Segment 2: Data Payload (变长, 可选)                     │
│   大块数据 (如文件内容)                                  │
├─────────────────────────────────────────────────────────┤
│ Segment 3: Footer Payload (变长, 可选)                   │
│   校验和、签名等                                        │
├─────────────────────────────────────────────────────────┤
│                    Frame Epilogue (4B)                   │
│  data_crc (4B) - 数据 CRC32C (可选)                     │
└─────────────────────────────────────────────────────────┘
```

**帧类型 (tag)**：
```c
// 控制帧
#define PFSN_TAG_CONNECT     1   // 连接请求
#define PFSN_TAG_CONNECT_ACK 2   // 连接确认
#define PFSN_TAG_MSG         3   // 业务消息
#define PFSN_TAG_ACK         4   // 消息确认
#define PFSN_TAG_KEEPALIVE   5   // 心跳
#define PFSN_TAG_KEEPALIVE_ACK 6 // 心跳确认
#define PFSN_TAG_CLOSE       7   // 关闭连接

// 消息方向
#define PFSN_FLAG_REQUEST    0x01  // 请求 (client->server)
#define PFSN_FLAG_RESPONSE   0x02  // 响应 (server->client)
#define PFSN_FLAG_NOTIFY     0x04  // 异步通知 (server->client)
#define PFSN_FLAG_BATCH      0x08  // 批量消息
```

### 3.3 消息类型映射

映射现有 master.proto 的 RPC 到 powerfs-net 消息类型：

| powerfs-net 消息类型 | master.proto RPC | 方向 | 说明 |
|---------------------|------------------|------|------|
| `MSG_LOOKUP` | `LookupDirectoryEntry` | req/resp | 目录查找 |
| `MSG_GETATTR` | `GetEntryByInode` | req/resp | 获取属性 |
| `MSG_SETATTR` | `UpdateEntry` | req/resp | 设置属性 |
| `MSG_MKDIR` | `CreateEntry` | req/resp | 创建目录 |
| `MSG_CREATE` | `CreateEntry` | req/resp | 创建文件 |
| `MSG_UNLINK` | `DeleteEntry` | req/resp | 删除文件 |
| `MSG_RMDIR` | `DeleteEntry` | req/resp | 删除目录 |
| `MSG_RENAME` | `RenameEntry` | req/resp | 重命名 |
| `MSG_READDIR` | `ListEntries` | req/resp | 读取目录 |
| `MSG_SYMLINK` | `CreateEntry` | req/resp | 创建符号链接 |
| `MSG_READLINK` | `GetEntryByInode` | req/resp | 读取符号链接 |
| `MSG_LINK` | `UpdateEntry` | req/resp | 创建硬链接 |
| `MSG_READ` | (Volume 直接读) | req/resp | 读数据 |
| `MSG_WRITE` | (Volume 直接写) | req/resp | 写数据 |
| `MSG_FSYNC` | `UpdateEntry` | req/resp | 同步 |
| `MSG_STATFS` | `GetStatistics` | req/resp | 文件系统统计 |
| `MSG_INVALIDATE` | `MetadataNotification` | notify | 缓存失效通知 |
| `MSG_PING` | `Ping` | req/resp | 连通性检测 |

### 3.4 序列化格式

#### 3.4.1 设计原则

- **紧凑**：避免 Protobuf 的 tag-number 开销
- **可扩展**：支持字段新增
- **双端一致**：Rust 和 C 使用相同的二进制格式
- **无需依赖**：内核态不需要 protobuf 库

#### 3.4.2 TLV 编码 (Type-Length-Value)

```
每个字段:
┌──────────────┬──────────────┬────────────────────┐
│ field_id (2B) │ length (2B) │ value (变长)        │
└──────────────┴──────────────┴────────────────────┘

field_id:
  - 0x0001: parent_ino (uint64)
  - 0x0002: name (string)
  - 0x0003: mode (uint32)
  - 0x0004: uid (uint32)
  - 0x0005: gid (uint32)
  - 0x0006: size (uint64)
  - 0x0007: ino (uint64)
  - 0x0008: nlink (uint32)
  - ...
  - 0x0010: symlink_target (string)
  - 0x0011: hard_link_id (string)
  - 0x0012: chunks (repeated)
  - 0x0013: generation (uint64)
```

#### 3.4.3 示例：Lookup 请求

```c
// Rust 编码
fn encode_lookup_req(parent_ino: u64, name: &str) -> Vec<u8> {
    let mut buf = Vec::new();
    // field 0x0001: parent_ino
    buf.extend_from_slice(&0x0001u16.to_le_bytes());
    buf.extend_from_slice(&8u16.to_le_bytes());
    buf.extend_from_slice(&parent_ino.to_le_bytes());
    // field 0x0002: name
    buf.extend_from_slice(&0x0002u16.to_le_bytes());
    buf.extend_from_slice(&(name.len() as u16).to_le_bytes());
    buf.extend_from_slice(name.as_bytes());
    buf
}

// C 编码
int encode_lookup_req(uint8_t *buf, uint64_t parent_ino, const char *name) {
    uint8_t *p = buf;
    // field 0x0001: parent_ino
    put_le16(p, 0x0001); p += 2;
    put_le16(p, 8); p += 2;
    put_le64(p, parent_ino); p += 8;
    // field 0x0002: name
    uint16_t name_len = strlen(name);
    put_le16(p, 0x0002); p += 2;
    put_le16(p, name_len); p += 2;
    memcpy(p, name, name_len); p += name_len;
    return p - buf;
}
```

---

## 四、Rust 实现设计

### 4.1 Crate 结构

```
powerfs-net/
├── Cargo.toml
├── src/
│   ├── lib.rs              // 公共接口
│   ├── protocol.rs         // 协议定义 (帧格式、消息类型)
│   ├── serialize.rs        // 序列化/反序列化
│   ├── client.rs           // 客户端实现
│   ├── server.rs           // 服务端 trait
│   ├── connection.rs       // 连接管理
│   └── errors.rs           // 错误类型
```

### 4.2 Client 接口

```rust
pub trait PowerFsNetClient {
    /// 发送同步请求，等待响应
    async fn send_request(&self, req: Request) -> Result<Response>;
    
    /// 发送异步通知，不等待响应
    async fn send_notify(&self, notify: Notification) -> Result<()>;
    
    /// 接收通知/响应
    async fn recv(&self) -> Result<Message>;
    
    /// 关闭连接
    async fn close(&self) -> Result<()>;
}

pub struct PowerFsNetTcpClient {
    // TCP 连接
    stream: TcpStream,
    // 序列号
    seq: AtomicU32,
    // 待处理请求
    pending: Arc<Mutex<HashMap<u32, oneshot::Sender<Response>>>>,
    // 通知接收通道
    notify_rx: broadcast::Receiver<Notification>,
}

impl PowerFsNetTcpClient {
    pub async fn connect(addr: &str, client_id: &str) -> Result<Self>;
    pub async fn reconnect(&self) -> Result<()>;  // 自动重连
}
```

### 4.3 Server 接口

```rust
pub trait PowerFsNetHandler: Send + Sync {
    async fn handle_request(&self, req: Request) -> Result<Response>;
    async fn on_notify(&self, notify: Notification);
}

pub struct PowerFsNetServer {
    listener: TcpListener,
    handler: Arc<dyn PowerFsNetHandler>,
}

impl PowerFsNetServer {
    pub async fn bind(addr: &str, handler: Arc<dyn PowerFsNetHandler>) -> Result<Self>;
    pub async fn serve(&self) -> Result<()>;
}
```

### 4.4 与现有代码集成

#### 4.4.1 FUSE 客户端改造

```rust
// powerfs-fuse-core/src/client.rs

pub struct PowerFuseClient {
    // ... 现有字段 ...
    net_client: Option<Arc<powerfs_net::PowerFsNetTcpClient>>,  // 新增
}

impl PowerFuseClient {
    pub async fn new_with_net(
        master_addrs: &[&str],
        filer_addrs: &[&str],
        runtime_handle: Handle,
        collection: &str,
    ) -> Arc<Self> {
        // 创建 powerfs-net 客户端
        let net_client = powerfs_net::PowerFsNetTcpClient::connect(
            &master_addrs[0],
            &client_id,
        ).await?;
        
        Arc::new(PowerFuseClient {
            net_client: Some(Arc::new(net_client)),
            // ... 其他字段 ...
        })
    }
    
    // 用 net_client 替换 gRPC 调用
    pub async fn lookup_directory_entry(
        &self, parent_ino: u64, name: &str
    ) -> Result<Option<Entry>> {
        if let Some(net) = &self.net_client {
            let req = Request::Lookup(LookupReq { parent_ino, name: name.to_string() });
            let resp = net.send_request(req).await?;
            // 解析响应...
        } else {
            // 回退到 gRPC
        }
    }
}
```

---

## 五、内核实现设计

### 5.1 Kernel Client 结构

```c
// include/linux/powerfs/net_client.h

#ifndef _POWERFS_NET_CLIENT_H
#define _POWERFS_NET_CLIENT_H

#include <linux/socket.h>
#include <linux/net.h>

// 连接状态
enum pfsn_conn_state {
    PFSN_CONN_DISCONNECTED = 0,
    PFSN_CONN_CONNECTING,
    PFSN_CONN_CONNECTED,
    PFSN_CONN_ERROR,
};

// 连接结构
struct pfsn_connection {
    struct socket *sock;
    enum pfsn_conn_state state;
    struct mutex send_mutex;      // 发送锁
    struct mutex recv_mutex;      // 接收锁
    wait_queue_head_t waitq;      // 等待队列
    atomic_t seq;                 // 序列号
    unsigned long last_keepalive; // 上次心跳时间
    char server_addr[256];
    uint16_t server_port;
};

// 请求上下文
struct pfsn_request {
    u32 seq;
    u32 msg_type;
    void *req_data;
    u32 req_len;
    void *resp_data;
    u32 resp_len;
    int status;
    bool done;
    wait_queue_head_t waitq;
};

// 客户端 API
struct pfsn_connection *pfsn_connect(const char *addr, uint16_t port);
void pfsn_disconnect(struct pfsn_connection *con);

int pfsn_send_request(struct pfsn_connection *con, 
                     struct pfsn_request *req);
int pfsn_recv_response(struct pfsn_connection *con, 
                      struct pfsn_request *req, int timeout_ms);

// 便捷 API（封装请求-响应）
int pfsn_lookup(struct pfsn_connection *con,
                uint64_t parent_ino, const char *name,
                struct pfsn_entry *result);
                
int pfsn_create(struct pfsn_connection *con,
                uint64_t parent_ino, const char *name,
                uint32_t mode, uint64_t *new_ino);

// ... 其他 API ...

#endif
```

### 5.2 内核实现要点

```c
// net/net/powerfs/net_client.c

#include <linux/module.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <linux/sched.h>

// 连接建立
struct pfsn_connection *pfsn_connect(const char *addr, uint16_t port) {
    struct pfsn_connection *con;
    struct sockaddr_in sin;
    int ret;
    
    con = kzalloc(sizeof(*con), GFP_KERNEL);
    if (!con) return ERR_PTR(-ENOMEM);
    
    // 创建内核 socket
    con->sock = sock_create_kern(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (IS_ERR(con->sock)) {
        kfree(con);
        return ERR_CAST(con->sock);
    }
    
    // 设置超时
    con->sock->sk->sk_sndtimeo = msecs_to_jiffies(5000);
    con->sock->sk->sk_rcvtimeo = msecs_to_jiffies(5000);
    
    // 连接
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = in_aton(addr);
    
    ret = kernel_connect(con->sock, (struct sockaddr *)&sin, sizeof(sin), 0);
    if (ret < 0) {
        sock_release(con->sock);
        kfree(con);
        return ERR_PTR(ret);
    }
    
    // 发送 Banner
    ret = pfsn_send_banner(con);
    if (ret < 0) {
        kernel_sock_shutdown(con->sock, SHUT_RDWR);
        sock_release(con->sock);
        kfree(con);
        return ERR_PTR(ret);
    }
    
    // 接收 Banner
    ret = pfsn_recv_banner(con);
    if (ret < 0) {
        // 回退处理
    }
    
    con->state = PFSN_CONN_CONNECTED;
    mutex_init(&con->send_mutex);
    mutex_init(&con->recv_mutex);
    init_waitqueue_head(&con->waitq);
    atomic_set(&con->seq, 0);
    
    return con;
}

// 发送请求
int pfsn_send_request(struct pfsn_connection *con, struct pfsn_request *req) {
    struct pfsn_frame_header hdr;
    struct iov_iter iter;
    struct iovec iov[2];
    void *frame_buf;
    u32 frame_len;
    int ret;
    
    // 序列化请求
    frame_buf = kzalloc(PFSN_MAX_FRAME_SIZE, GFP_KERNEL);
    if (!frame_buf) return -ENOMEM;
    
    frame_len = pfsn_encode_request(frame_buf, req);
    
    // 填充 header
    memset(&hdr, 0, sizeof(hdr));
    hdr.tag = PFSN_TAG_MSG;
    hdr.flags = PFSN_FLAG_REQUEST;
    hdr.seq = atomic_inc_return(&con->seq);
    hdr.msg_type = req->msg_type;
    hdr.data_len = frame_len;
    pfsn_calc_header_crc(&hdr);
    
    // 发送
    mutex_lock(&con->send_mutex);
    
    iov[0].iov_base = &hdr;
    iov[0].iov_len = sizeof(hdr);
    iov[1].iov_base = frame_buf;
    iov[1].iov_len = frame_len;
    
    iter = ITER_IOVEC;
    iov_iter_init(&iter, WRITE, iov, 2, sizeof(hdr) + frame_len);
    
    ret = sock_sendmsg(con->sock, &iter);
    
    mutex_unlock(&con->send_mutex);
    kfree(frame_buf);
    
    return ret;
}

// 接收响应
int pfsn_recv_response(struct pfsn_connection *con, struct pfsn_request *req, int timeout_ms) {
    struct pfsn_frame_header hdr;
    struct iov_iter iter;
    struct iovec iov;
    void *resp_buf;
    int ret;
    
    // 设置接收超时
    if (timeout_ms > 0) {
        con->sock->sk->sk_rcvtimeo = msecs_to_jiffies(timeout_ms);
    }
    
    // 接收 Header
    mutex_lock(&con->recv_mutex);
    
    iov.iov_base = &hdr;
    iov.iov_len = sizeof(hdr);
    iter = ITER_IOVEC;
    iov_iter_init(&iter, READ, &iov, 1, sizeof(hdr));
    
    ret = sock_recvmsg(con->sock, &iter, MSG_WAITALL);
    if (ret < 0) {
        mutex_unlock(&con->recv_mutex);
        return ret;
    }
    
    // 校验 CRC
    if (!pfsn_verify_header_crc(&hdr)) {
        mutex_unlock(&con->recv_mutex);
        return -EINVAL;
    }
    
    // 接收数据
    if (hdr.data_len > 0) {
        resp_buf = kzalloc(hdr.data_len, GFP_KERNEL);
        if (!resp_buf) {
            mutex_unlock(&con->recv_mutex);
            return -ENOMEM;
        }
        
        iov.iov_base = resp_buf;
        iov.iov_len = hdr.data_len;
        iter = ITER_IOVEC;
        iov_iter_init(&iter, READ, &iov, 1, hdr.data_len);
        
        ret = sock_recvmsg(con->sock, &iter, MSG_WAITALL);
        if (ret < 0) {
            kfree(resp_buf);
            mutex_unlock(&con->recv_mutex);
            return ret;
        }
        
        // 反序列化响应
        pfsn_decode_response(resp_buf, hdr.data_len, req);
        kfree(resp_buf);
    }
    
    mutex_unlock(&con->recv_mutex);
    
    return hdr.status;
}
```

### 5.3 与内核文件系统集成

```c
// powerfs_fs.c 改造

struct powerfs_sb_info {
    // ... 现有字段 ...
    struct pfsn_connection *net_con;  // 新增：网络连接
};

static int powerfs_mount(struct file_system_type *fs_type,
                        int flags, const char *dev_name,
                        void *data) {
    // ... 现有初始化 ...
    
    // 建立网络连接
    sbi->net_con = pfsn_connect("192.168.1.100", 9333);
    if (IS_ERR(sbi->net_con)) {
        pr_err("powerfs: failed to connect to server\n");
        return PTR_ERR(sbi->net_con);
    }
    
    // ... 其他初始化 ...
}

static void powerfs_kill_sb(struct super_block *sb) {
    struct powerfs_sb_info *sbi = POWERFS_SB(sb);
    
    // 断开网络连接
    if (sbi->net_con) {
        pfsn_disconnect(sbi->net_con);
    }
    
    // ... 现有清理 ...
}

static int powerfs_lookup(struct inode *dir, struct dentry *dentry,
                         unsigned int flags) {
    struct powerfs_sb_info *sbi = POWERFS_SB(dir->i_sb);
    struct pfsn_entry entry;
    int ret;
    
    // 通过网络请求获取元数据
    ret = pfsn_lookup(sbi->net_con, dir->i_ino, dentry->d_name.name, &entry);
    if (ret < 0) {
        return ret;
    }
    
    // 创建 inode
    // ... 现有逻辑 ...
}
```

---

## 六、服务端改造

### 6.1 新增轻量协议 Listener

在现有 Master/Filer 服务旁新增 powerfs-net listener：

```rust
// powerfs-master/src/net_listener.rs

pub struct PowerFsNetListener {
    listener: TcpListener,
    master_handler: Arc<MasterHandler>,
}

impl PowerFsNetListener {
    pub async fn start(
        addr: &str,
        master_handler: Arc<MasterHandler>,
    ) -> Result<()> {
        let listener = TcpListener::bind(addr).await?;
        
        loop {
            let (stream, _) = listener.accept().await?;
            let handler = master_handler.clone();
            
            tokio::spawn(async move {
                if let Err(e) = Self::handle_connection(stream, handler).await {
                    error!("Connection error: {}", e);
                }
            });
        }
    }
    
    async fn handle_connection(
        stream: TcpStream,
        handler: Arc<MasterHandler>,
    ) -> Result<()> {
        let (mut reader, mut writer) = stream.split();
        
        // 1. Banner 握手
        let banner = PowerFsNetProtocol::read_banner(&mut reader).await?;
        PowerFsNetProtocol::write_banner(&mut writer).await?;
        
        // 2. Connect 请求
        let connect_req = PowerFsNetProtocol::read_connect(&mut reader).await?;
        let connect_resp = handler.handle_connect(&connect_req).await;
        PowerFsNetProtocol::write_response(&mut writer, &connect_resp).await?;
        
        // 3. 消息循环
        loop {
            let frame = PowerFsNetProtocol::read_frame(&mut reader).await?;
            
            match frame {
                Frame::Request(req) => {
                    let resp = handler.handle_request(req).await;
                    PowerFsNetProtocol::write_response(&mut writer, &resp).await?;
                }
                Frame::Close => break,
                _ => {}
            }
        }
        
        Ok(())
    }
}
```

### 6.2 Handler 实现

```rust
pub struct MasterHandler {
    // 复用现有 Master 服务逻辑
    master_service: Arc<MasterService>,
}

#[async_trait]
impl PowerFsNetHandler for MasterHandler {
    async fn handle_request(&self, req: Request) -> Result<Response> {
        match req {
            Request::Lookup { parent_ino, name } => {
                let entry = self.master_service.lookup_entry(parent_ino, &name).await?;
                Ok(Response::Lookup(LookupResp { entry }))
            }
            Request::Create { parent_ino, name, mode, uid, gid } => {
                let entry = self.master_service.create_entry(parent_ino, &name, mode, uid, gid).await?;
                Ok(Response::Create(CreateResp { entry }))
            }
            Request::Delete { ino, is_dir } => {
                self.master_service.delete_entry(ino, is_dir).await?;
                Ok(Response::Generic(GenericResp { success: true }))
            }
            // ... 其他请求类型 ...
        }
    }
}
```

---

## 七、实施路线图

### Phase 1: powerfs-net 协议层 (2周)

| 任务 | 交付物 |
|------|--------|
| 定义 wire protocol | `powerfs-net/src/protocol.rs` |
| 实现序列化/反序列化 | `powerfs-net/src/serialize.rs` |
| Rust client/server 框架 | `powerfs-net/src/client.rs`, `server.rs` |
| 单元测试 | 协议正确性验证 |

### Phase 2: FUSE 客户端改造 (2周)

| 任务 | 交付物 |
|------|--------|
| 实现 Master handler | `powerfs-master/src/net_handler.rs` |
| 改造 powerfs-fuse-core | `powerfs-fuse-core/src/client.rs` |
| 端到端测试 | FUSE → powerfs-net → Master |
| 性能对比 | vs 原有 gRPC 延迟/吞吐 |

### Phase 3: 内核客户端实现 (3周)

| 任务 | 交付物 |
|------|--------|
| 内核 net_client | `kernel/powerfs_net/` |
| 内核序列化 | `kernel/powerfs_net/serialize.c` |
| 集成到 powerfs.ko | 修改 `powerfs_fs.c` |
| QEMU 测试 | VM → Docker 集群 |

### Phase 4: 优化与增强 (2周)

| 任务 | 交付物 |
|------|--------|
| 批量消息支持 | BATCH 消息类型 |
| 零拷贝优化 | sendfile/splice 系统调用 |
| RDMA 支持 | verbs API 封装 |
| 压力测试 | 1000+ 并发连接 |

---

## 八、性能预期

### 8.1 延迟对比

| 操作 | gRPC (当前) | powerfs-net (预期) | 提升 |
|------|------------|-------------------|------|
| lookup | ~500μs | ~50μs | 10x |
| create | ~800μs | ~80μs | 10x |
| read (4KB) | ~300μs | ~60μs | 5x |
| write (4KB) | ~400μs | ~70μs | 5.7x |

### 8.2 带宽对比

| 指标 | gRPC (当前) | powerfs-net (预期) | 提升 |
|------|------------|-------------------|------|
| 单连接吞吐 | ~10K ops/s | ~50K ops/s | 5x |
| 批量吞吐 | ~50K ops/s | ~500K ops/s | 10x |
| 内存拷贝次数 | 3-4次 | 0-1次 | 3-4x |

---

## 九、风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 内核 socket API 限制 | 无法使用 epoll | 自定义 waitqueue + 轮询 |
| 协议版本兼容 | 新旧客户端不兼容 | 版本协商 + 降级处理 |
| 序列化性能 | TLV 比 protobuf 慢 | 字段复用 + 定长编码优化 |
| 内存泄漏 | 内核态无法 GC | 严格的生命周期管理 + kmemleak 检查 |

---

## 十、总结

powerfs-net 的核心设计理念：

1. **简单即强大**：二进制协议 + TLV 序列化，避免 Protobuf 复杂性
2. **双端统一**：Rust 和 C 使用相同的 wire format，减少调试成本
3. **渐进增强**：TCP 起步，RDMA 可选，不阻塞核心功能
4. **参考成熟**：借鉴 Ceph msgr2 的帧格式和 BeeGFS 的直连模式

通过实施此方案，PowerFS 将获得：
- 亚毫秒级元数据延迟
- 内核态直连集群服务
- 与 FUSE 客户端完全一致的语义
- 为 RDMA 优化奠定基础