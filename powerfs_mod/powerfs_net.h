/*
 * PowerFS 内核态 powerfs-net 协议实现
 *
 * 直接在内核中实现 powerfs-net 二进制协议，通过 TCP 连接与 Filer 通信。
 * 抛弃用户态代理方式，简化架构。
 *
 * 架构:
 *   内核 VFS -> powerfs_net (TLV + 帧) -> TCP socket -> Filer
 *
 * 协议格式:
 *   Frame Header (28B) + TLV Body [+ Data]
 *
 * 帧头布局 (28 字节):
 *   magic: 4B   - "PFSN" (0x5046534E)
 *   version: 1B - 协议版本 (0x01)
 *   flags: 1B   - 帧标志 (REQUEST/RESPONSE)
 *   seq: 4B     - 序列号
 *   msg_type: 2B - 消息类型
 *   status: 2B  - 响应状态码
 *   data_len: 4B - 数据长度 (body + data)
 *   reserved: 6B - 保留
 *   header_crc: 4B - CRC32C 校验
 */

#ifndef _POWERFS_NET_H
#define _POWERFS_NET_H

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

/* ========== 协议常量 ========== */

#define POWERFS_NET_MAGIC       0x5046534E  /* "PFSN" */
#define POWERFS_NET_VERSION     0x01
#define POWERFS_NET_HEADER_SIZE 28
#define POWERFS_NET_MAX_FRAME   (4 * 1024 * 1024)  /* 4MB - 与 Rust MAX_FRAME_SIZE 一致 */
#define POWERFS_NET_MAX_TLV     65535  /* 64KB - 1 */
#define POWERFS_NET_MAX_BODY    (256 * 1024)  /* 256KB body (容 netfs 128KB 预读响应) */
#define POWERFS_NET_MAX_DATA    (2 * 1024 * 1024)  /* 2MB data (容 POWERFS_CHUNK_SIZE read_needle 响应) */

/* 连接超时 (ms) */
#define POWERFS_NET_CONNECT_TIMEOUT  5000   /* connect: 5s (容器重启后网络需时间稳定) */
#define POWERFS_NET_SEND_TIMEOUT     10000  /* post-connect send timeout: 10s */
#define POWERFS_NET_RECV_TIMEOUT     10000

/* Phase 1: Lookup / Readdir 短超时 (ms).
 * 仅在最近发生过断连的窗口内使用 (见 powerfs_net_recently_disconnected),
 * 用于快速失败让 VFS / 应用层重试, 避免 hung task.
 * 正常连接时仍用 POWERFS_NET_RECV_TIMEOUT (10s). */
#define POWERFS_LOOKUP_TIMEOUT_MS       2000    /* lookup: 2s */
#define POWERFS_READDIR_TIMEOUT_MS      5000    /* readdir: 5s */

/* Phase 2: 元数据操作短超时 (ms).
 * create/unlink/rename/symlink/link 等在 VFS 持有 i_rwsem 写锁时调用,
 * 必须快速完成或失败, 避免长时间阻塞目录操作.
 * 5s 足够覆盖正常 LAN RPC (<10ms) + 重连 (3s) + 重试. */
#define POWERFS_META_TIMEOUT_MS         5000    /* metadata ops: 5s */

/* 断连窗口: 断连后 5s 内视为 "最近断连", lookup/readdir 用短超时 */
#define POWERFS_RECENT_DISCONNECT_MS    5000

/* 最大重连次数 (per-conn 状态机使用) */
#define POWERFS_NET_MAX_RECONNECT    3

/* send_request 等待重连的最大时间 (ms).
 * 作为安全兜底防止 wait_event 挂死. */
#define POWERFS_NET_RECONNECT_WAIT_TIMEOUT_MS  30000

/* ========== 帧标志 ========== */

#define POWERFS_NET_FLAG_REQUEST   0x01
#define POWERFS_NET_FLAG_RESPONSE  0x02
#define POWERFS_NET_FLAG_NOTIFY    0x04
#define POWERFS_NET_FLAG_BATCH     0x08
#define POWERFS_NET_FLAG_ACK       0x10

/* Phase 2: 服务端负载因子编码在 flags bits 6-7 (向后兼容)
 *   00=空闲(0-25%), 01=正常(25-50%), 10=较忙(50-75%), 11=满载(75%+)
 * 旧客户端忽略这些位, 旧服务端填 0 (空闲). */
#define POWERFS_NET_FLAG_LOAD_FACTOR_SHIFT  6
#define POWERFS_NET_FLAG_LOAD_FACTOR_MASK   0xC0
#define POWERFS_NET_FLAG_LOAD_FACTOR_GET(f) \
    (((f) & POWERFS_NET_FLAG_LOAD_FACTOR_MASK) >> POWERFS_NET_FLAG_LOAD_FACTOR_SHIFT)

/* ========== 客户端类型 (握手) ========== */

#define POWERFS_NET_CLIENT_FUSE    0x01
#define POWERFS_NET_CLIENT_KERNEL  0x02
#define POWERFS_NET_CLIENT_ADMIN   0x03

/* ========== 消息类型 (与 powerfs-net MsgType 对应) ========== */

enum powerfs_net_msg_type {
    /* 控制消息 */
    POWERFS_NET_MSG_PING = 0x0001,
    POWERFS_NET_MSG_HANDSHAKE = 0x0002,

    /* 元数据操作 */
    POWERFS_NET_MSG_LOOKUP = 0x0010,
    POWERFS_NET_MSG_GETATTR = 0x0011,
    POWERFS_NET_MSG_SETATTR = 0x0012,
    POWERFS_NET_MSG_CREATE = 0x0013,
    POWERFS_NET_MSG_MKDIR = 0x0014,
    POWERFS_NET_MSG_UNLINK = 0x0015,
    POWERFS_NET_MSG_RMDIR = 0x0016,
    POWERFS_NET_MSG_RENAME = 0x0017,
    POWERFS_NET_MSG_READDIR = 0x0018,
    POWERFS_NET_MSG_SYMLINK = 0x0019,
    POWERFS_NET_MSG_READLINK = 0x001A,
    POWERFS_NET_MSG_LINK = 0x001B,

    /* 数据操作 */
    POWERFS_NET_MSG_READ = 0x0020,
    POWERFS_NET_MSG_WRITE = 0x0021,

    /* 一致性操作 */
    POWERFS_NET_MSG_INVALIDATE = 0x0032,
    /* K1-6: close/fsync 时强一致同步 size+chunks 到 Filer (Raft).
     * 对齐 powerfs-net MsgType::UpdateInodeSizeChunks (0x0034).
     * K1 阶段内核不维护 chunks 列表, fsync 仍用 SETATTR;
     * K2 (Inline close) / K3 (Stripe close) 切换到此消息. */
    POWERFS_NET_MSG_UPDATE_INODE_SIZE_CHUNKS = 0x0034,

    /* 状态 */
    POWERFS_NET_MSG_STATFS = 0x0040,

    /* Master 操作 */
    POWERFS_NET_MSG_LOOKUP_VOLUME = 0x0051,
    POWERFS_NET_MSG_HEARTBEAT = 0x0052,
    POWERFS_NET_MSG_KEEP_CONNECTED = 0x0053,
    POWERFS_NET_MSG_VOLUME_LIST = 0x0054,

    /* Master topology & discovery */
    POWERFS_NET_MSG_GET_TOPOLOGY = 0x0070,
    POWERFS_NET_MSG_LIST_FILERS = 0x0074,

    /* Volume 操作 */
    POWERFS_NET_MSG_CREATE_VOLUME = 0x0060,
    POWERFS_NET_MSG_DELETE_VOLUME = 0x0061,
    POWERFS_NET_MSG_WRITE_NEEDLE = 0x0062,
    POWERFS_NET_MSG_READ_NEEDLE = 0x0063,
    POWERFS_NET_MSG_DELETE_NEEDLE = 0x0064,
    POWERFS_NET_MSG_BATCH_WRITE_NEEDLE = 0x0065,
    POWERFS_NET_MSG_READ_NEEDLE_BLOB = 0x0066,
    POWERFS_NET_MSG_RANGE_LEASE = 0x0067,
    POWERFS_NET_MSG_VOLUME_STATUS = 0x0068,
    /* Lease 消息值必须与 powerfs-net/src/protocol.rs MsgType 枚举一致:
     *   0x0069 在 Rust 端是 AssignNeedle, 不是 AcquireLease.
     *   AcquireLease=0x0080, ReleaseLease=0x0081, RenewLease=0x0082.
     * 之前内核误用 0x0069 导致 volume server 把 ACQUIRE_LEASE 当作
     * AssignNeedle 处理 (unsupported), 继而触发 invalid frame header
     * 关闭连接, 所有后续 WriteNeedle/ReadNeedle 超时 (-ENOTCONN). */
    POWERFS_NET_MSG_ACQUIRE_LEASE = 0x0080,
    POWERFS_NET_MSG_RELEASE_LEASE = 0x0081,
    POWERFS_NET_MSG_RENEW_LEASE = 0x0082,
};

/* ========== 响应状态码 ========== */

#define POWERFS_NET_STATUS_OK              0
#define POWERFS_NET_STATUS_ERR_NOT_FOUND   1
#define POWERFS_NET_STATUS_ERR_ALREADY_EXISTS 2
#define POWERFS_NET_STATUS_ERR_PERMISSION  3
#define POWERFS_NET_STATUS_ERR_IO          4
#define POWERFS_NET_STATUS_ERR_INVALID_ARG 5
#define POWERFS_NET_STATUS_ERR_NOT_DIR     6
#define POWERFS_NET_STATUS_ERR_IS_DIR      7
#define POWERFS_NET_STATUS_ERR_NO_SPACE    8
#define POWERFS_NET_STATUS_ERR_BAD_FD      9
#define POWERFS_NET_STATUS_ERR_SERVER      10
#define POWERFS_NET_STATUS_ERR_REDIRECT    11  /* 响应体 Owner 字段携带 leader net 地址 "ip:port", 客户端需切换连接重试 */

/* ========== TLV 字段 ID ========== */

enum powerfs_net_field_id {
    /* 通用字段 */
    POWERFS_NET_FLD_PARENT_INO = 0x01,
    POWERFS_NET_FLD_NAME = 0x02,
    POWERFS_NET_FLD_MODE = 0x03,
    POWERFS_NET_FLD_UID = 0x04,
    POWERFS_NET_FLD_GID = 0x05,
    POWERFS_NET_FLD_SIZE = 0x06,
    POWERFS_NET_FLD_INO = 0x07,
    POWERFS_NET_FLD_NLINK = 0x08,
    POWERFS_NET_FLD_MTIME = 0x09,
    POWERFS_NET_FLD_ATIME = 0x0A,
    POWERFS_NET_FLD_CTIME = 0x0B,
    POWERFS_NET_FLD_SYMLINK_TARGET = 0x0C,
    POWERFS_NET_FLD_IS_DIR = 0x0D,
    POWERFS_NET_FLD_OFFSET = 0x0E,
    POWERFS_NET_FLD_DATA_LEN = 0x0F,

    /* 扩展字段 */
    POWERFS_NET_FLD_RDEV = 0x10,
    POWERFS_NET_FLD_BLKSIZE = 0x11,
    POWERFS_NET_FLD_BLOCKS = 0x12,
    POWERFS_NET_FLD_CONTENT_SIZE = 0x13,
    POWERFS_NET_FLD_DISK_SIZE = 0x14,
    POWERFS_NET_FLD_HARD_LINK_ID = 0x16,
    POWERFS_NET_FLD_OWNER = 0x17,
    POWERFS_NET_FLD_BACKEND = 0x18,

    /* 列表字段 */
    POWERFS_NET_FLD_LIMIT = 0x20,
    POWERFS_NET_FLD_LAST_NAME = 0x21,
    POWERFS_NET_FLD_HAS_MORE = 0x22,
    POWERFS_NET_FLD_ENTRIES = 0x23,
    POWERFS_NET_FLD_COUNT = 0x24,
    POWERFS_NET_FLD_ENTRY = 0x25,
    POWERFS_NET_FLD_VERSION = 0x19,  /* matches Rust FieldId::Version */

    /* Delta 同步字段 */
    POWERFS_NET_FLD_CLIENT_ID = 0x30,
    POWERFS_NET_FLD_SEQ = 0x31,
    POWERFS_NET_FLD_VCLOCK_ENTRIES = 0x32,
    POWERFS_NET_FLD_DELTA_OPS = 0x33,

    /* K1-6: ShardId (u64), 对齐 powerfs-net FieldId::ShardId (0x70).
     * 用于 UpdateInodeSizeChunks 等需 shard 路由的消息. */
    POWERFS_NET_FLD_SHARD_ID = 0x70,

    /* Lease 字段 */
    POWERFS_NET_FLD_LEASE_ID = 0x40,
    POWERFS_NET_FLD_LEASE_DURATION = 0x41,
    POWERFS_NET_FLD_LEASE_EPOCH = 0x42,

    /* Rename 字段 */
    POWERFS_NET_FLD_NEW_PARENT_INO = 0x50,
    POWERFS_NET_FLD_NEW_NAME = 0x51,

    /* Lease Token (Volume Server lease validation, matches Rust FieldId::LeaseToken) */
    POWERFS_NET_FLD_LEASE_TOKEN = 0x80,

    /* Volume / Needle 字段 (数据直连 Volume Server) */
    POWERFS_NET_FLD_VOLUME_ID = 0x92,   /* volume_id (u64), needle 操作中复用 Ino 字段 */
    POWERFS_NET_FLD_FILE_KEY = 0x94,    /* needle_id / file_key (u64) */
    POWERFS_NET_FLD_CHUNKS = 0x96,      /* chunks 列表 (JSON, GetAttr 响应) */
    POWERFS_NET_FLD_INODE_V2 = 0x97,    /* inode (u64), needle 操作中与 FileKey 并存 */
    POWERFS_NET_FLD_USED_SPACE = 0x98,  /* used space (u64), matches Rust FieldId::UsedSpace */
    POWERFS_NET_FLD_FILE_COUNT = 0x99,  /* file count (u64), matches Rust FieldId::FileCount */

    /* ===== FileLayout fields (0xA0-0xAF) — powerfs-layout crate ===== */
    POWERFS_NET_FLD_PLACEMENT = 0xA0,       /* Placement 编码 (u8 tag + 后续) */
    POWERFS_NET_FLD_RELIABILITY = 0xA1,     /* Reliability 编码 (u8 tag + count) */
    POWERFS_NET_FLD_RELIABILITY_STATE = 0xA2, /* ReliabilityState (u8) */
    POWERFS_NET_FLD_COMPRESSION = 0xA3,     /* CompressionState (u8), 预留 */
    POWERFS_NET_FLD_CHUNK_LAYOUT = 0xA4,    /* 二进制 ChunkEncoding (替代 JSON Chunks) */
    POWERFS_NET_FLD_HAS_MORE_CHUNKS = 0xA5, /* Paginated 标志 (u8) */
    POWERFS_NET_FLD_NEXT_OFFSET = 0xA6,     /* LIST_CHUNKS 起始 (u64) */
    POWERFS_NET_FLD_TOTAL_COUNT = 0xA7,     /* 总 chunk 数 (u32) */
    POWERFS_NET_FLD_STRIPE_SIZE = 0xA8,     /* Stripe/WideStripe stripe_size (u64) */
    POWERFS_NET_FLD_STRIPE_COUNT = 0xA9,    /* Stripe/WideStripe stripe_count (u32) */
    POWERFS_NET_FLD_START_VOLUME_IDX = 0xAA, /* 起始 volume 索引 (u32) */
    POWERFS_NET_FLD_VOLUME_IDS = 0xAB,      /* volume_ids 列表 (u64 LE 数组) */
    POWERFS_NET_FLD_START_NEEDLE_ID = 0xAC, /* 首 needle_id (u64) */
    POWERFS_NET_FLD_CHUNK_SIZE = 0xAD,      /* 单 chunk 大小 (u32), 统一 1MB */
    POWERFS_NET_FLD_INLINE_DATA = 0xAE,     /* Inline 数据 (bytes, <=8KB) */
    POWERFS_NET_FLD_INLINE_MAX_SIZE = 0xAF, /* Inline 阈值 (u32) */

    /* ===== Coherence protocol fields (0xB0-0xB2), 预留 ===== */
    POWERFS_NET_FLD_START_INODE = 0xB0,     /* AllocInodeBatch 起始 (u64), 预留 */
    POWERFS_NET_FLD_END_INODE = 0xB1,       /* AllocInodeBatch 结束 (u64), 预留 */
    POWERFS_NET_FLD_OPEN_COUNT = 0xB2,      /* open_count (u32), 预留 */

    /* ===== Xattr fields (0xB3-0xB4), 预留 ===== */
    POWERFS_NET_FLD_XATTR_KEY = 0xB3,       /* xattr 键 (string), 预留 */
    POWERFS_NET_FLD_XATTR_VALUE = 0xB4,     /* xattr 值 (bytes), 预留 */

    /* ===== Replica fields (0xB5) ===== */
    POWERFS_NET_FLD_REPLICA_CHUNKS = 0xB5,  /* 副本 chunk 列表 (ChunkRef 二进制数组) */

    /* ===== WideStripe range compression (0xB6) ===== */
    POWERFS_NET_FLD_VOLUME_IDS_RANGE = 0xB6, /* 范围压缩: start_u64 + count_u32 = 12B */
};

/* ========== 帧头结构 (28 字节, packed) ========== */

struct powerfs_net_frame_hdr {
    __be32 magic;        /* 0x5046534E "PFSN" */
    __u8 version;        /* 0x01 */
    __u8 flags;         /* 帧标志 */
    __u32 seq;          /* 序列号 (little-endian) */
    __u16 msg_type;     /* 消息类型 (little-endian) */
    __u16 status;       /* 状态码 (little-endian) */
    __u32 data_len;     /* body + data 总长度 (little-endian) */
    __u32 body_len;     /* body 段长度 (data 段 = data_len - body_len) */
    __u8 route_hash;    /* 高7位=目的地hash(client_id), 低1位=channel(0=data,1=meta) */
    __u8 protocol_ver;  /* 协议版本 (版本升级一致性检查) */
    __le32 header_crc;  /* CRC32C (little-endian) */
} __attribute__((packed));

/* 编译时断言帧头大小 */
#define POWERFS_NET_FRAME_HDR_SIZE 28

/* ========== 通路类型 (握手 + 帧头防错乱校验) ========== */
/* channel 标识连接通路: data (大帧读写) vs meta (lease 小请求).
 * 握手时填入 HandshakeRequest.channel, 帧头 route_hash 低 1 位也携带.
 * 服务端收帧时三重校验 (channel/hash/protocol_ver), 不匹配断连. */
#define POWERFS_NET_CHANNEL_DATA    0   /* data 通路: write_needle/read_needle */
#define POWERFS_NET_CHANNEL_META    1   /* meta 通路: lease acquire/renew/release */
#define POWERFS_NET_PROTOCOL_VER    0x01 /* 帧头 protocol_ver, 版本升级一致性检查 */

/* ========== 握手结构 ========== */

/* 握手请求 (20 字节) */
struct powerfs_net_handshake_req {
    __u8 magic[4];       /* "PFSN" */
    __u8 version;        /* 0x01 */
    __u8 client_type;    /* 客户端类型 */
    __u8 channel;        /* 通路类型: 0=data, 1=meta */
    __u8 reserved;       /* 对齐 */
    __u64 client_id;     /* 客户端 ID (little-endian) */
    __u32 features;      /* 特性标志 (little-endian) */
} __attribute__((packed));

/* 握手响应 (18 字节) */
struct powerfs_net_handshake_resp {
    __u8 magic[4];       /* "PFSN" */
    __u8 version;        /* 0x01 */
    __u8 status;         /* 0=OK, 1=REJECT */
    __u64 server_id;     /* 服务端 ID */
    __u32 features;      /* 特性标志 */
} __attribute__((packed));

/* ========== TLV 编解码接口 ========== */

/* TLV 编码器 */
struct powerfs_tlv_enc {
    __u8 *buf;
    size_t len;
    size_t cap;
};

/* TLV 解码器 */
struct powerfs_tlv_dec {
    const __u8 *buf;
    size_t len;
    size_t pos;
};

/* TLV 编码器操作 */
void powerfs_tlv_enc_init(struct powerfs_tlv_enc *enc, __u8 *buf, size_t cap);
int  powerfs_tlv_enc_u8(struct powerfs_tlv_enc *enc, __u8 field, __u8 val);
int  powerfs_tlv_enc_u16(struct powerfs_tlv_enc *enc, __u8 field, __u16 val);
int  powerfs_tlv_enc_u32(struct powerfs_tlv_enc *enc, __u8 field, __u32 val);
int  powerfs_tlv_enc_u64(struct powerfs_tlv_enc *enc, __u8 field, __u64 val);
int  powerfs_tlv_enc_string(struct powerfs_tlv_enc *enc, __u8 field,
                            const char *str, size_t len);
int  powerfs_tlv_enc_bytes(struct powerfs_tlv_enc *enc, __u8 field,
                           const __u8 *data, size_t len);
int  powerfs_tlv_enc_nested(struct powerfs_tlv_enc *enc, __u8 field,
                            const __u8 *data, size_t len);
size_t powerfs_tlv_enc_len(const struct powerfs_tlv_enc *enc);

/* TLV 解码器操作 */
void powerfs_tlv_dec_init(struct powerfs_tlv_dec *dec, const __u8 *buf, size_t len);
int  powerfs_tlv_dec_next(struct powerfs_tlv_dec *dec, __u8 *field, size_t *length);
int  powerfs_tlv_dec_u8(struct powerfs_tlv_dec *dec, __u8 field, __u8 *val);
int  powerfs_tlv_dec_u16(struct powerfs_tlv_dec *dec, __u8 field, __u16 *val);
int  powerfs_tlv_dec_u32(struct powerfs_tlv_dec *dec, __u8 field, __u32 *val);
int  powerfs_tlv_dec_u64(struct powerfs_tlv_dec *dec, __u8 field, __u64 *val);
int  powerfs_tlv_dec_string(struct powerfs_tlv_dec *dec, __u8 field,
                            char *str, size_t max_len);
int  powerfs_tlv_dec_skip(struct powerfs_tlv_dec *dec, size_t length);
bool powerfs_tlv_dec_is_empty(const struct powerfs_tlv_dec *dec);
int  powerfs_tlv_dec_find_u64(struct powerfs_tlv_dec *dec, __u8 field, __u64 *val);
int  powerfs_tlv_dec_find_u32(struct powerfs_tlv_dec *dec, __u8 field, __u32 *val);
int  powerfs_tlv_dec_find_u8(struct powerfs_tlv_dec *dec, __u8 field, __u8 *val);
int  powerfs_tlv_dec_find_raw(struct powerfs_tlv_dec *dec, __u8 field,
                              const __u8 **val, size_t *len);

/* ========== powerfs-net 连接管理 (v2: sk 回调 + per-CPU 调度器) ========== */

/* 服务器类型 (前置声明: 新架构 struct powerfs_net_server_conn 需要) */
enum powerfs_net_server_type {
    POWERFS_NET_SERVER_FILER = 0,
    POWERFS_NET_SERVER_MASTER = 1,
    POWERFS_NET_SERVER_VOLUME = 2,       /* data 通路: write/read/delete needle (大帧) */
    POWERFS_NET_SERVER_VOLUME_META = 3,  /* meta 通路: lease acquire/renew/release (小请求) */
};

/* === v2: per-CPU 调度器 (参照 Lustre ksock_sched) ===
 *
 * 每个 CPU 一个调度器线程, 服务按 addr hash 分配到本调度器的所有连接.
 * 线程数固定 = num_online_cpus(), 与连接数无关 (解决 v1 的 N 连接 N 线程问题).
 *
 * 调度器消费两个队列 (各自独立锁, 互不阻塞):
 *   rx_conns: sk_data_ready 回调投递的"有数据可收"连接 (rx_lock 保护)
 *   tx_conns: sk_write_space 回调/do_send 投递的"可写待发"连接 (tx_lock 保护)
 * 空闲时 wait_event 睡眠, 回调 wake_up 唤醒.
 *
 * 锁拆分原因: RX/TX 线程独立运行, 共用一把锁会让 RX 回调与 TX 回调
 * 互斥; 拆为两把锁后, 收发路径完全解耦, 无嵌套获取 → 无死锁风险. */
struct powerfs_net_sched {
    spinlock_t          rx_lock;    /* 保护 rx_conns (spin_lock_bh) */
    spinlock_t          tx_lock;    /* 保护 tx_conns (spin_lock_bh) */
    struct list_head    rx_conns;   /* 数据就绪待收的连接 (RX 线程消费) */
    struct list_head    tx_conns;   /* 可写待发的连接 (TX 线程消费) */
    wait_queue_head_t   rx_waitq;   /* RX 线程等待队列 (sk_data_ready 唤醒) */
    wait_queue_head_t   tx_waitq;   /* TX 线程等待队列 (sk_write_space/pfs_tx_schedule 唤醒) */
    int                 cpt;        /* 所属 CPU 编号 (0 .. num_online_cpus-1) */
    struct task_struct *rx_task;    /* RX 线程 (pfs_rx_thread_fn) */
    struct task_struct *tx_task;    /* TX 线程 (pfs_tx_thread_fn) */

    /* Per-sched 复用收帧缓冲 (避免每帧 kvmalloc 2MB).
     * RX 线程串行处理各连接, 无并发访问.
     * lease/getattr 响应只有几百字节, 之前每帧 kvmalloc(2MB) 在
     * writeback 内存紧张时极慢 (vmalloc 回退建页表) -> 10s+ 延迟. */
    void               *rx_body_buf;  /* POWERFS_NET_MAX_BODY (256KB) */
    void               *rx_data_buf;  /* POWERFS_NET_MAX_DATA (2MB) */
};

/* ========== 连接池状态机 (Phase 1: 新架构) ========== */

/*
 * Per-conn 状态机:
 *
 *   DISCONNECTED → CONNECTING → CONNECTED
 *                      ↓              ↓
 *                   fail         send error / health fail
 *                      ↓              ↓
 *                   RECONNECTING ←────┘
 *                      ↓
 *                retry (count++)
 *                      ↓
 *              count >= MAX → FAULT
 *              success → CONNECTED (count=0)
 *
 * 每个连接独立维护状态，互不影响。
 * 上层通过 shard_leader_map 路由到对应 filer 的连接。
 */
enum powerfs_conn_state {
    CONN_INIT = 0,          /* 初始化，尚未连接 */
    CONN_CONNECTING,        /* TCP 连接中 */
    CONN_CONNECTED,         /* 已连接，可用 */
    CONN_RECONNECTING,      /* 重连中 (1-3次) */
    CONN_FAULT,             /* 重连失败，仅 umount 可恢复 */
};

/*
 * 请求对象 (参照 Ceph ceph_osd_request / ceph_mds_request 设计)
 *
 * 关键设计 (学习 Ceph):
 *   1. kref 引用计数: 多处持有请求时自动管理生命周期
 *   2. rb_node 红黑树: by seq 快速查找 (O(log n)), 用于 reply 匹配
 *   3. 断连重发: 请求不取消, 而是标记 r_needs_resend, 重连后自动重发
 *   4. 异步回调: 支持 callback + completion 两种完成方式
 *   5. 尝试计数: r_attempts, 超过阈值放弃
 *
 * 生命周期:
 *   1. VFS 回调创建 request (powerfs_request_alloc), kref=1
 *   2. submit: 分配 seq, 挂到 filer->pending_reqs + filer->req_tree
 *   3. 发送 + 接收响应
 *   4a. 成功: 从 pending 摘除, complete/callback, kref_put
 *   4b. 断连: 标记 needs_resend, 不取消, 重连后自动重发
 *   5. 超时/超过重试: error=-ETIMEDOUT, complete, kref_put
 *   6. VFS 回调读取 resp_*, powerfs_request_free (kref_put → kfree)
 */
struct powerfs_request {
    /* === 红黑树节点 (参照 Ceph r_node) === */
    /* 挂到 filer->req_tree, by seq 快速查找, 用于 reply 匹配 */
    struct rb_node rb_node;

    /* === 链表节点 === */
    /* 挂到 filer->pending_reqs (发送顺序) 或 shard pending 队列 */
    struct list_head list_node;
    /* v2: 挂到 conn->tx_queue (待发送队列, 调度器消费).
     * 与 list_node 独立: 请求可同时在 pending_reqs (等响应) 和 tx_queue (待发送) */
    struct list_head tx_list;

    /* === 请求标识 === */
    __u32 seq;                   /* 序列号 (per-conn, 由 submit 分配) */
    __u16 msg_type;             /* 消息类型 (POWERFS_NET_MSG_*) */
    __u64 shard_id;             /* 所属 shard (用于路由, 0=不关心) */

    /* === 请求数据 (发送方提供, submit 不修改) === */
    const __u8 *req_body;       /* TLV body */
    size_t req_body_len;
    const __u8 *req_data;       /* 附加数据 (如写数据) */
    size_t req_data_len;

    /* === 响应空间 (submit 填入, 调用方读取) === */
    __u16 resp_status;          /* 响应状态码 (0=OK, 其他=错误) */
    __u8 *resp_body;            /* 响应 body 缓冲区 (调用方分配) */
    size_t resp_body_cap;       /* 缓冲区容量 */
    size_t resp_body_len;       /* 实际响应 body 长度 */
    __u8 *resp_data;            /* 响应 data 缓冲区 (调用方分配) */
    size_t resp_data_cap;       /* 缓冲区容量 */
    size_t resp_data_len;       /* 实际响应 data 长度 */

    /* === 完成与超时 === */
    struct completion done;     /* 同步等待 */
    int (*callback)(struct powerfs_request *);  /* 异步回调 (可选, NULL=同步) */
    void *priv;                 /* 异步回调上下文 (调用方设置, callback 内访问) */
    int error;                  /* 最终错误码 (0=成功, <0=错误) */
    unsigned long deadline;     /* 超时 jiffies (0=使用默认) */
    u64 ts_submit;              /* 调试: do_send 入队时间 (ktime_get_ns, 0=未启用) */

    /* === 异步超时 (仅 callback != NULL 时启用) ===
     * 同步请求用 do_send 内 wait_for_completion_killable_timeout 兜底;
     * 异步请求无等待线程, 用 delayed_work 在 deadline 到期时触发完成,
     * 防止请求挂死 (page 永远留在 PageWriteback → hung task).
     * 在 process context (system_wq) 执行, callback 可睡眠安全. */
    struct delayed_work timeout_work;
    bool timer_armed;

    /* === 重发控制 (参照 Ceph r_attempts) === */
    int attempts;               /* 发送尝试次数 */
    bool needs_resend;          /* 断连后标记, 重连后自动重发 */
#define POWERFS_REQ_MAX_ATTEMPTS  5

    /* === Partial send 追踪 (大帧非阻塞发送) ===
     * MSG_DONTWAIT 下 kernel_sendmsg 可能只发送部分帧.
     * send_offset 记录已发送字节数 (跨 header+body+data),
     * EAGAIN 时保存进度, 重新入 tx_queue 等 sk_write_space 回调.
     * 下次 pfs_process_transmit 从 offset 继续, 避免重发 header 导致错位. */
    size_t send_offset;

    /* === 关联 === */
    struct powerfs_net_server_conn *filer;  /* 发往哪个 filer (NULL=未绑定) */
    struct kref kref;                      /* 引用计数 (参照 Ceph r_kref) */
};

/* 请求分配/释放 */
struct powerfs_request *powerfs_request_alloc(__u16 msg_type, gfp_t gfp);
void powerfs_request_free(struct powerfs_request *req);

/* Per-server 连接 (替代全局 g_conn) */
struct powerfs_net_server_conn {
    /* 标识 */
    char addr[64];              /* IP 地址 */
    __u16 port;                 /* 端口 */
    enum powerfs_net_server_type type;
    bool in_use;                /* 该槽位是否已使用 */

    /* TCP 连接 */
    struct socket *sock;        /* 当前 socket (NULL=未连接) */
    wait_queue_head_t sock_user_wq;

    /* 状态机 */
    enum powerfs_conn_state state;
    spinlock_t state_lock;      /* 保护 state */

    /* Per-conn 重连 */
    struct delayed_work reconnect_work;
    int reconnect_count;        /* 当前重连次数 (0-3) */
    wait_queue_head_t reconnect_wq;  /* 等待重连完成 */

    /* Per-conn 序列号 */
    atomic_t seq_counter;

    /* 服务端信息 (握手后) */
    __u64 server_id;
    __u32 server_features;
    __u64 client_id;            /* 本连接的 client_id (握手时分配, 帧头 route_hash 用) */
    __u8 channel;               /* 通路类型: 0=data, 1=meta (握手时设置) */

    /* === 请求追踪 (参照 Ceph out_queue + out_sent) === */
    /* pending_reqs: 已发送等待响应的请求 (链表, 按发送顺序) */
    struct list_head pending_reqs;
    /* req_tree: 按 seq 查找请求 (红黑树, O(log n), 用于 reply 匹配) */
    struct rb_root req_tree;
    spinlock_t req_lock;        /* 保护 pending_reqs + req_tree */

    /* === v2 回调驱动 (替换 v1 的 per-conn RX 线程) === */
    struct work_struct disconnect_work;  /* sk_state_change/error_report 或收发错误触发的清理 work */
    struct powerfs_net_sched *sched;     /* 归属的调度器 (按 addr hash 到 CPU) */
    struct list_head          rx_list;   /* 挂到 sched->rx_conns */
    struct list_head          tx_list;   /* 挂到 sched->tx_conns */
    bool                      rx_ready;  /* 回调置位: 有数据可收 */
    bool                      rx_scheduled; /* 已在 rx_conns 中 (防重复投递) */
    bool                      tx_ready;  /* 回调置位: 有空间可发 */
    bool                      tx_scheduled; /* 已在 tx_conns 中 */
    struct list_head          tx_queue;  /* 待发送请求队列 (do_send 入队, 调度器消费) */
    spinlock_t                tx_lock;   /* 保护 tx_queue */

    /* === v2 RX 非阻塞状态机 (对齐 TX send_offset 设计) ===
     * 问题: 大响应 (1MB needle) 分多 TCP 段到达, MSG_WAITALL+sk_rcvtimeo=10s
     *       会阻塞 sched 上其他 conn 的响应接收 (1MB 文件 10s 延迟根因).
     * 方案: MSG_DONTWAIT + 断点续收, 每帧分 3 段 (hdr/body/data),
     *       EAGAIN 时保留 partial 状态退出, 等 sk_data_ready 下次回调续收.
     * buffer 必须在 conn 上 (sched 共享 buffer 切 conn 时 partial 会丢失). */
    u8      rx_hdr_buf[POWERFS_NET_FRAME_HDR_SIZE]; /* 28 字节帧头 */
    size_t  rx_hdr_got;       /* 已收帧头字节 */
    size_t  rx_body_got;      /* 已收 body 字节 */
    size_t  rx_data_got;      /* 已收 data 字节 */
    size_t  rx_body_total;    /* 帧头解析后填入: 本帧 body 段长度 */
    size_t  rx_data_total;    /* 帧头解析后填入: 本帧 data 段长度 */
    u8      rx_phase;         /* 0=收hdr, 1=收body, 2=收data, 3=done */
    struct powerfs_net_frame_hdr rx_cur_hdr;  /* 当前帧头 (解析后) */
    void   *rx_body_buf;      /* per-conn body buffer (POWERFS_NET_MAX_BODY 256KB) */
    void   *rx_data_buf;      /* per-conn data buffer (POWERFS_NET_MAX_DATA 2MB) */

    /* === v2 sk 回调保存 (竞态处理, 参照 Lustre socklnd_lib.c) ===
     * disconnect 时恢复原始回调, 防止 socket 比模块长寿导致 UAF */
    void (*saved_data_ready)(struct sock *);
    void (*saved_write_space)(struct sock *);
    void (*saved_state_change)(struct sock *);
    void (*saved_error_report)(struct sock *);

    /* === v2 调度器引用计数 ===
     * kref 用于调度器持引用: 回调投递到 rx_conns/tx_conns 时 get,
     * 调度器处理完 put. disconnect 等 kref refcount==1 (只剩 owner 引用)
     * 才 sock_release, 防止调度器在飞时 UAF. */
    struct kref kref;

    /* === 指数退避 (参照 Ceph con->delay) === */
    unsigned long reconnect_delay;  /* 当前退避间隔 (jiffies) */
#define POWERFS_NET_BASE_DELAY    1000    /* 初始 1s */
#define POWERFS_NET_MAX_DELAY     30000   /* 最大 30s */
};

/* Shard 路由表: shard_id → filer_idx */
#define POWERFS_MAX_SHARDS  64

/*
 * Per-shard 路由状态机:
 *
 *   ROUTE_VALID ←───────────────────────────────┐
 *       │                                       │
 *   leader filer 断连 (事件)                      │
 *       ↓                                       │
 *   ROUTE_CHECKING ──→ 找到新 leader ────────────┘
 *       │                  (REDIRECT)
 *       │
 *   所有 filer 都试过, 无 leader
 *       ↓
 *   ROUTE_UNKNOWN ──→ filer 重连成功 → ROUTE_CHECKING
 *
 * 事件来源:
 *   1. 连接断开: conn_pool 通知 route_table, 相关 shard → CHECKING
 *   2. REDIRECT 响应: 更新 leader, shard → VALID
 *   3. filer 重连成功: conn_pool 通知, 相关 shard → CHECKING (重试)
 */
enum powerfs_shard_route_state {
    ROUTE_VALID = 0,       /* leader 已知且连接正常 */
    ROUTE_CHECKING,        /* leader filer 断连, 正在寻找新 leader */
    ROUTE_UNKNOWN,         /* 所有 filer 都试过, 暂无 leader */
};

struct powerfs_shard_route_entry {
    int leader_filer_idx;               /* -1=未知 */
    enum powerfs_shard_route_state state;

    /* CHECKING/UNKNOWN 状态下的待处理请求队列.
     * 请求在此等待, 直到:
     *   - REDIRECT 找到新 leader → VALID, 队列中的请求发往新 leader
     *   - 超时 → 请求返回 -ETIMEDOUT
     * VALID 状态下此队列为空 (请求直接走 filer->pending_reqs). */
    struct list_head pending_reqs;
    spinlock_t req_lock;
};

struct powerfs_shard_route {
    struct powerfs_shard_route_entry entries[POWERFS_MAX_SHARDS];
    spinlock_t lock;
    __u64 shard_count;          /* 从 Filer 获取 */
};

/* 连接状态查询辅助函数 */
static inline const char *powerfs_conn_state_str(enum powerfs_conn_state s)
{
    switch (s) {
    case CONN_INIT:        return "INIT";
    case CONN_CONNECTING:  return "CONNECTING";
    case CONN_CONNECTED:   return "CONNECTED";
    case CONN_RECONNECTING:return "RECONNECTING";
    case CONN_FAULT:       return "FAULT";
    default:               return "UNKNOWN";
    }
}

/* ========== 连接池 API (新架构) ========== */

/* 初始化连接池 (从 Master 发现 filer/volume 列表) */
int powerfs_conn_pool_init(const char *master_addr, __u16 master_port);

/* 连接池清理 */
void powerfs_conn_pool_exit(void);

/* 获取 shard 对应的 filer 连接 (路由表查找) */
struct powerfs_net_server_conn *
powerfs_conn_get_filer_for_shard(u64 shard_id);

/* 通过地址查找 filer 连接 */
struct powerfs_net_server_conn *
powerfs_conn_find_filer(const char *addr, __u16 port);

/* 更新 shard 路由 (REDIRECT 时调用, shard → VALID) */
void powerfs_shard_route_update(u64 shard_id, int filer_idx);

/* === 事件驱动: 连接状态变化通知 shard 路由 === */

/*
 * 连接断开时调用: 所有 leader=该filer 的 shard → ROUTE_CHECKING
 * 场景: TCP 断连、send/recv 错误、健康检查失败
 */
void powerfs_shard_route_on_filer_disconnect(int filer_idx);

/*
 * 连接重连成功时调用: 所有 leader=该filer 的 shard → ROUTE_CHECKING
 * (不是直接 VALID, 因为 leader 可能已切换到其他 filer, 需要重新确认)
 */
void powerfs_shard_route_on_filer_reconnect(int filer_idx);

/*
 * 获取 shard 路由状态
 */
enum powerfs_shard_route_state
powerfs_shard_route_get_state(u64 shard_id);

/*
 * 在 ROUTE_CHECKING 状态下, 获取下一个可用的 filer
 * 跳过 FAULT 和 DISCONNECTED 的连接
 * 返回 filer_idx, 或 -1 如果没有可用 filer
 */
int powerfs_shard_route_find_available_filer(u64 shard_id);

/* 单个连接的状态操作 */
int  powerfs_conn_connect_one(struct powerfs_net_server_conn *conn);
void powerfs_conn_disconnect_one(struct powerfs_net_server_conn *conn);

/* 连接状态变更 (内部调用, 触发路由表更新) */
void powerfs_conn_set_state(struct powerfs_net_server_conn *conn,
                            enum powerfs_conn_state new_state);

/* === 请求生命周期 (参照 Ceph osd_request 设计) === */

/*
 * 提交请求并通过连接池发送 (主入口, 替代 powerfs_net_send_request)
 *
 * 流程:
 *   1. 根据 shard_id 查路由状态
 *   2. VALID: 直接用 leader filer 连接
 *   3. CHECKING: 尝试其他 filer, 或将请求挂到 shard pending 队列
 *   4. UNKNOWN: 挂到 shard pending 队列等待, 或返回 -EAGAIN
 *   5. 发送+接收, 处理 REDIRECT (更新路由表, 重试)
 *   6. 断连: 标记 needs_resend, 重连后自动重发 (参照 Ceph con_fault)
 *
 * 返回 req->error (0=成功, <0=错误)
 */
int powerfs_request_submit(struct powerfs_request *req);

/*
 * 派发 shard pending 队列中的请求 (找到新 leader 后调用)
 */
void powerfs_shard_route_dispatch_pending(u64 shard_id);

/* kref 释放 (内部, powerfs_request_free 调用) */
void powerfs_request_release(struct kref *kref);

/* ========== 多连接池配置 ========== */

#define POWERFS_NET_MAX_SERVERS    32   /* 最大服务器数量 (Filer + Master + Volume, 支持多节点扩展) */
#define POWERFS_NET_MAX_FILERS     16   /* 最大 Filer 数量 */
#define POWERFS_NET_MAX_VOLUMES    32   /* 最大 Volume 数量 */
#define POWERFS_NET_MONITOR_INTERVAL 5000  /* 健康检查间隔 (ms) */
#define POWERFS_NET_LEADER_CHECK_INTERVAL 2000  /* Leader 检查间隔 (ms) */

/* 服务器条目 (元数据, 兼容旧代码) */
struct powerfs_net_server_entry {
    char addr[64];              /* 服务器地址 */
    __u16 port;                 /* 端口 */
    enum powerfs_net_server_type type;  /* 服务器类型 */
    bool is_leader;             /* 是否为 leader */
    int last_ping_ms;           /* 最后一次 ping 延迟 */
    __u64 last_check_time;      /* 最后检查时间 (jiffies) */
};

/* 连接池结构 (新架构: per-conn 连接 + shard 路由) */
struct powerfs_net_pool {
    /* === 连接池 === */
    /* Filer 连接池: 每个连接独立 state/mutex/reconnect */
    struct powerfs_net_server_conn filers[POWERFS_NET_MAX_FILERS];
    int filer_count;

    /* Volume 连接池 */
    struct powerfs_net_server_conn volumes[POWERFS_NET_MAX_VOLUMES];
    int volume_count;

    /* === Volume 路由表: volume_id → (data conn idx, meta conn idx) ===
     * 从 Master GetTopology 获取, 用于按 volume_id 找到正确的 Volume Server 连接.
     * 每个 volume server 建两条独立 TCP 连接:
     *   conn_idx       — data 通路 (write/read needle 大帧, 2MB)
     *   meta_conn_idx  — meta 通路 (lease acquire/renew/release 小请求)
     * 物理分离避免 write_needle 大帧阻塞 lease 小请求导致 -110 超时. */
    struct {
        __u64 volume_id;
        int conn_idx;        /* data conn (write/read/delete needle) */
        int meta_conn_idx;   /* meta conn (lease acquire/renew/release) */
    } vol_routes[POWERFS_NET_MAX_VOLUMES];
    int vol_route_count;
    spinlock_t vol_route_lock;

    /* === Shard 路由表 === */
    struct powerfs_shard_route shard_route;

    /* === Master 地址 (用于 discover) === */
    char master_addr[64];
    __u16 master_port;
    bool master_set;

    atomic_t stopping;

    /* === Phase 1: 最近断连时间戳 (atomic_long_t, jiffies) ===
     * powerfs_conn_disconnect_one 内 WRITE_ONCE 更新.
     * powerfs_net_recently_disconnected 读取, 判断是否在断连窗口内
     * (窗口内 lookup/readdir 用短超时, 见 powerfs.h). */
    atomic_long_t last_disconnect_jiffies;

    /* === v2: 独立重连/断连 workqueue ===
     * 重连 work 中 kernel_connect 可能阻塞 3s, 多 filer 同时重连时
     * 若共享 system_wq 会串行执行, 累积超过 workqueue lockup 阈值.
     * 用 WQ_UNBOUND 让 work 并行在不同 CPU 上跑, 互不阻塞.
     * disconnect_work 也放这里 (sk 回调调度, 同样不应阻塞 system_wq). */
    struct workqueue_struct *reconn_wq;

    /* === v2: per-CPU 调度器数组 (参照 Lustre ksocknal_data.ksnd_schedulers) ===
     * schedulers[i] 服务 addr hash % num_sched == i 的所有 filer 连接.
     * vol_schedulers[j] 服务 addr hash % num_vol_sched == j 的所有 volume 连接.
     * 分离设计: 避免数据 I/O (volume) 饿死元数据 (filer) 操作.
     * global_lock 保护 sk_user_data 解引用 (回调 read_lock_bh vs set/reset write_lock_bh) */
    struct powerfs_net_sched *schedulers;       /* filer (元数据) 调度器池 */
    int                        num_sched;
    struct powerfs_net_sched *vol_schedulers;   /* volume (数据) 调度器池 (P3.1) */
    int                        num_vol_sched;
    rwlock_t                   global_lock;

    /* === 旧字段 (兼容期, 逐步移除) === */
    struct powerfs_net_server_entry servers[POWERFS_NET_MAX_SERVERS];
    int server_count;
    int master_count;
    atomic_t active_filer_idx;
    atomic_t active_master_idx;
    atomic_t active_volume_idx;
    atomic_t leader_idx;
    atomic_t leader_known;
    atomic_t failover_count;
    atomic_t last_failover_time;
    struct mutex pool_lock;
    struct delayed_work leader_check_work;
};

/* ========== 连接管理 API (增强版) ========== */

/* 初始化连接池 */
int powerfs_net_pool_init(void);
void powerfs_net_pool_exit(void);

/* 添加服务器到池 */
int powerfs_net_add_server(const char *addr, __u16 port, 
                           enum powerfs_net_server_type type);

/* 从池移除服务器 */
int powerfs_net_remove_server(const char *addr, __u16 port);

/* 清理连接池 (关闭所有连接，释放资源) */
void powerfs_net_pool_cleanup(void);

/* 设置 stopping 标志, 让所有等待的 send_request 返回 -ENOTCONN */
void powerfs_net_set_stopping(void);

/* 检查是否正在停止 (umount 中) */
bool powerfs_net_is_stopping(void);

/* 设置主 Filer 地址 (兼容旧接口) */
int powerfs_net_set_primary(const char *addr, __u16 port);

/* 设置多个 Filer 地址 (逗号分隔) */
int powerfs_net_set_filers(const char *addrs, const char *ports);

/* 设置 Master 地址 */
int powerfs_net_set_master(const char *addr, __u16 port);

/* 从 Master 查询 filer 列表并添加到连接池.
 * 遍历 master_addrs (逗号分隔), 连接第一个可达的 Master leader,
 * 发送 LIST_FILERS 请求, 解析响应并 add_server 每个返回的 filer.
 * 成功返回 filer 数量 (>0), 失败返回负值.
 * master_port 为 Master 的 powerfs-net 端口 (通常 9334). */
int powerfs_net_discover_filers(const char *master_addrs, __u16 master_port);

/* 设置 Volume 地址 */
int powerfs_net_set_volume(const char *addr, __u16 port);

/* 连接状态检查 (新架构: 检查 g_pool 是否有可用 filer 连接) */
bool powerfs_net_is_connected(void);

/* 检查最近是否发生过断连 (窗口内 lookup/readdir 用短超时, 见 powerfs.h).
 * window_ms: 窗口大小 (ms), 返回 true 表示窗口内有过断连. */
bool powerfs_net_recently_disconnected(unsigned int window_ms);

/* 根据 当前连接状态 + 最近断连情况 选择 lookup/readdir 超时.
 * 正常连接 + 非最近断连: POWERFS_NET_RECV_TIMEOUT (10s)
 * 否则 (断连中 / 最近断连窗口内): 用 short_timeout_ms */
int powerfs_net_pick_timeout(int short_timeout_ms);

/* ========== 请求/响应 API ========== */

/**
 * powerfs_net_send_request - 发送同步请求并等待响应
 *
 * @msg_type: 消息类型 (POWERFS_NET_MSG_*)
 * @body: TLV 编码的 body 数据
 * @body_len: body 长度
 * @data: 附加数据 (如写数据)
 * @data_len: 数据长度
 * @resp_body: 输出响应 body 缓冲区 (可 NULL)
 * @resp_body_cap: 响应 body 缓冲区容量
 * @resp_data: 输出响应 data 缓冲区 (可 NULL)
 * @resp_data_cap: 响应 data 缓冲区容量
 * @timeout_ms: 超时 (毫秒)
 * @resp_body_len_out: 输出: 实际 body 长度 (可 NULL)
 * @resp_data_len_out: 输出: 实际 data 长度 (可 NULL)
 *
 * 返回: >=0 成功 (0=OK, >0=协议状态码), <0 错误 (-errno)
 */
int powerfs_net_send_request(__u16 msg_type, u64 route_inode,
                             const __u8 *body, size_t body_len,
                             const __u8 *data, size_t data_len,
                             __u8 *resp_body, size_t resp_body_cap,
                             __u8 *resp_data, size_t resp_data_cap,
                             int timeout_ms,
                             size_t *resp_body_len_out,
                             size_t *resp_data_len_out);

u64 powerfs_calc_shard_id(u64 inode);

/* ========== 便捷方法 ========== */

/* 目录条目 (readdir 返回) */
struct powerfs_net_dir_entry {
    __u64 ino;
    char name[256];
    __u32 mode;
    __u32 uid;
    __u32 gid;
    __u64 size;
    __u64 mtime;
    __u64 atime;
    __u64 ctime;
    __u32 nlink;
};

/* GETATTR (返回完整属性含时间戳 + volume_id/file_key 用于数据直连) */
struct powerfs_file_layout;  /* 定义在 powerfs.h */
struct powerfs_chunk_map;   /* 定义在 powerfs.h */
struct powerfs_inode_info;  /* 定义在 powerfs.h */

/* LOOKUP (返回完整属性含时间戳 + volume_id/file_key 用于数据直连).
 * powerfs_net_lookup 用默认 10s 超时 (兼容旧调用方).
 * powerfs_net_lookup_timeout 由调用方指定超时 (Phase 1: lookup 在断连窗口内
 * 用 2s 短超时, 见 powerfs_net_pick_timeout).
 * volume_id/file_key: 输出, 用于直连 Volume Server 读写数据 (目录为 0).
 * layout: 输出, K3 解析 FileLayout (placement/volume_ids 等), 可为 NULL. */
int powerfs_net_lookup(__u64 dir_ino, const char *name, size_t name_len,
                       __u64 *ino, __u32 *mode, __u32 *uid, __u32 *gid,
                       __u64 *size, __u32 *nlink,
                       __u64 *mtime, __u64 *atime, __u64 *ctime,
                       __u64 *volume_id, __u64 *file_key,
                       struct powerfs_file_layout *layout);
int powerfs_net_lookup_timeout(__u64 dir_ino, const char *name, size_t name_len,
                               __u64 *ino, __u32 *mode, __u32 *uid, __u32 *gid,
                               __u64 *size, __u32 *nlink,
                               __u64 *mtime, __u64 *atime, __u64 *ctime,
                               __u64 *volume_id, __u64 *file_key,
                               struct powerfs_file_layout *layout,
                               int timeout_ms);
int powerfs_net_getattr(__u64 ino, __u32 *mode, __u32 *uid, __u32 *gid,
                         __u64 *size, __u32 *nlink,
                         __u64 *mtime, __u64 *atime, __u64 *ctime,
                         __u64 *volume_id, __u64 *file_key,
                         struct powerfs_file_layout *layout);

/* SETATTR */
int powerfs_net_setattr(__u64 ino, __u32 mode_valid, __u32 mode,
                         __u32 uid, __u32 gid, __u64 size);

/* CREATE / MKDIR
 * layout: 输出参数, 解析 CREATE 响应中的 FileLayout (placement/reliability/
 *         chunk_size). Flat 模式 Filer 不 encode layout, 此时 layout->has_placement
 *         为 false, 调用方按默认 Flat 处理. 可为 NULL. */
int powerfs_net_create(__u64 dir_ino, const char *name, size_t name_len,
                        __u32 mode, __u32 uid, __u32 gid, bool is_dir,
                        __u64 *ino_ret,
                        __u64 *volume_id_ret, __u64 *file_key_ret,
                        struct powerfs_file_layout *layout);

/* UNLINK / RMDIR */
int powerfs_net_unlink(__u64 dir_ino, const char *name, size_t name_len,
                       bool is_dir);

/* RENAME */
int powerfs_net_rename(__u64 old_dir_ino, const char *old_name, size_t old_name_len,
                       __u64 new_dir_ino, const char *new_name, size_t new_name_len);

/* K1-6: UPDATE_INODE_SIZE_CHUNKS — close/fsync 时强一致同步 size+chunks 到 Filer.
 *
 * 对齐 FUSE sync_size_chunks_on_close (powerfs-fuse/src/fuse.rs L990):
 *   - shard_id: 父目录 ino (Filer 用 calculate_shard 路由)
 *   - ino: 文件 inode
 *   - size: content_size
 *   - client_id: 客户端标识 (用于去重/调试)
 *   - chunks/chunk_count: chunk 映射数组, 可为 NULL (K1 阶段不传 chunks)
 *   - inline_data/inline_len: K2 Inline 文件数据, 非 NULL 时 Placement=INLINE
 *
 * 注意: Filer 端会用传入的 chunks 覆盖现有 chunks (update_inode_size_chunks_atomic).
 *       K1 阶段内核不维护 chunks 列表, 不应调用此函数 (用 setattr 替代).
 *       K2/K3 阶段内核维护 chunks 后, close/fsync 切换到此函数.
 *
 * 返回 0 成功, 负数错误码. */
int powerfs_net_update_inode_size_chunks(__u64 shard_id, __u64 ino, __u64 size,
                                         const char *client_id,
                                         const struct powerfs_chunk_map *chunks,
                                         __u32 chunk_count,
                                         const __u8 *inline_data,
                                         __u32 inline_len);

/* READDIR (匹配 Filer 协议: ParentIno + Limit + LastName 分页).
 * powerfs_net_readdir 用默认 5s 超时 (兼容旧调用方).
 * powerfs_net_readdir_timeout 由调用方指定超时 (Phase 1: 断连窗口内用 5s). */
int powerfs_net_readdir(__u64 dir_ino, const char *last_name, __u64 limit,
                        struct powerfs_net_dir_entry *entries, __u32 max_entries,
                        __u32 *actual_count, bool *has_more);
int powerfs_net_readdir_timeout(__u64 dir_ino, const char *last_name, __u64 limit,
                                struct powerfs_net_dir_entry *entries,
                                __u32 max_entries, __u32 *actual_count,
                                bool *has_more, int timeout_ms);

/* READ (直连 Volume Server, 不经过 Filer).
 * K3: 改为接受 powerfs_inode_info, 内部用 powerfs_locate_chunk 按 offset 定位
 * (volume_id, needle_id), 统一支持 Flat/Stripe/WideStripe 多卷布局.
 * 调用方无需预先获取 volume_id/file_key.
 * needle 模型: 整存整取, 内部按 offset/length 截取. */
int powerfs_net_read(struct powerfs_inode_info *pi, __u64 ino,
                     __u64 offset, __u32 length,
                     __u8 *buf, size_t buf_cap, __u32 *read_len);

/* WRITE (直连 Volume Server, 不经过 Filer).
 * volume_id/file_key: 从 lookup/getattr 获取的数据直连标识.
 * needle 模型: write_needle 整体替换, 内部 read-modify-write 处理 partial write. */
int powerfs_net_write(__u64 ino, __u64 volume_id, __u64 file_key,
                      __u64 offset, const __u8 *data, size_t data_len,
                      __u32 *written);

/* STATFS */
int powerfs_net_statfs(struct kstatfs *stats);

/* SYMLINK */
int powerfs_net_symlink(__u64 dir_ino, const char *name, size_t name_len,
                        const char *target, size_t target_len, __u64 *ino_ret);

/* READLINK */
int powerfs_net_readlink(__u64 ino, char *target, size_t target_cap);

/* LINK (硬链接) */
int powerfs_net_link(__u64 ino, __u64 dir_ino, const char *name, size_t name_len);

/* PING (连接健康检查) */
int powerfs_net_ping(void);

/* ========== Volume 直连 API (数据读写不经过 Filer) ==========
 *
 * 架构: 内核客户端 → powerfs-net → Volume Server (WriteNeedle/ReadNeedle)
 * Filer 只负责元数据 (Lookup/GetAttr/Create 等) + chunk 映射管理.
 * 客户端从 GetAttr 响应的 Chunks 字段获取 (volume_id, needle_id) 后直连 volume.
 */

/* 获取 volume 连接数量 */
int powerfs_net_get_volume_count(void);

/* 获取 volume 连接 (by index, 0..volume_count-1) */
struct powerfs_net_server_conn *powerfs_net_get_volume_conn(int idx);

/* 按 volume_id 查找 volume 连接 (通过 vol_routes 路由表).
 * is_meta=true 返回 meta conn (lease), false 返回 data conn (needle). */
struct powerfs_net_server_conn *powerfs_net_find_volume_conn(__u64 volume_id, bool is_meta);

/* 发送请求直连到 volume server (bypass shard routing).
 * vol_idx: volume 连接索引 (-1 = 按 volume_id 自动查找)
 * volume_id: 用于路由查找 (vol_idx < 0 时使用)
 * 其他参数同 powerfs_net_send_request */
int powerfs_net_send_to_volume(int vol_idx, __u64 volume_id,
                                __u16 msg_type,
                                const __u8 *body, size_t body_len,
                                const __u8 *data, size_t data_len,
                                __u8 *resp_body, size_t resp_body_cap,
                                __u8 *resp_data, size_t resp_data_cap,
                                int timeout_ms,
                                size_t *resp_body_len_out,
                                size_t *resp_data_len_out);

/* WriteNeedle: 直连 volume 写数据.
 * volume_id: 目标 volume
 * file_key: needle ID
 * inode: 用于 lease 校验 (lease 按 inode 注册, 非 needle)
 * data/data_len: 写入数据
 * 返回 0 成功, <0 错误 */
int powerfs_net_write_needle(__u64 volume_id, __u64 file_key, __u64 inode,
                             const __u8 *data, size_t data_len,
                             const char *lease_token, size_t token_len);

/* ReadNeedle: 直连 volume 读数据.
 * volume_id: 目标 volume
 * file_key: needle ID
 * buf/buf_cap: 读取缓冲区
 * read_len: 输出, 实际读取长度
 * 返回 0 成功, <0 错误 */
int powerfs_net_read_needle(__u64 volume_id, __u64 file_key,
                            __u8 *buf, size_t buf_cap, __u32 *read_len);

/* === 异步 WriteNeedle / ReadNeedle (page writeback 异步提交模式) ===
 *
 * 与同步版本区别: 入队后立即返回, 不等待响应. 响应到达时由调度器调用
 * req->callback(req). 调用方通过 req->priv 携带完成上下文.
 *
 * 生命周期约定:
 *   - 调用方提供的缓冲区 (req_body/resp_body/resp_data/data) 必须持久存活,
 *     直到 callback 被调用后才可释放 (通常放在 callback 所属的 ctx 中).
 *   - 返回 0: 提交成功, callback 将被调用 (成功或超时/断连), 调用方不得
 *     再访问传入的 req (由网络层在 callback 触发前管理).
 *   - 返回 <0: 提交失败 (如无可用连接), callback 不会被调用, 调用方自行
 *     清理缓冲区与上下文.
 *   - callback 内必须调用 powerfs_request_free(req) 释放请求对象.
 *
 * 超时: 异步请求用 delayed_work 在 deadline 到期时以 -ETIMEDOUT 完成,
 * 防止 page 永久滞留 PageWriteback 状态. timeout_ms 建议 30000 (30s). */

/* 异步 WriteNeedle.
 * req_body/req_body_cap: 调用方提供的 TLV body 缓冲区 (函数内编码, 持久存活)
 * resp_body/resp_body_cap: 调用方提供的响应 body 缓冲区 (持久存活)
 * data/data_len: 写入数据 (持久存活到 callback)
 * callback/priv: 完成回调与上下文 */
int powerfs_net_write_needle_async(__u64 volume_id, __u64 file_key, __u64 inode,
                                   const __u8 *data, size_t data_len,
                                   const char *lease_token, size_t token_len,
                                   __u8 *req_body, size_t req_body_cap,
                                   __u8 *resp_body, size_t resp_body_cap,
                                   int timeout_ms,
                                   int (*callback)(struct powerfs_request *),
                                   void *priv);

/* 异步 ReadNeedle (writeback RMW 读现有 needle).
 * req_body/req_body_cap: TLV body 缓冲区 (函数内编码, 持久存活)
 * resp_data/resp_data_cap: 响应数据缓冲区 (持久存活, scheduler 写入 needle 内容)
 * callback/priv: 完成回调与上下文 */
int powerfs_net_read_needle_async(__u64 volume_id, __u64 file_key,
                                  __u8 *resp_data, size_t resp_data_cap,
                                  __u8 *req_body, size_t req_body_cap,
                                  int timeout_ms,
                                  int (*callback)(struct powerfs_request *),
                                  void *priv);

/* 统一完成入口: 供内部 RX/disconnect/timeout 路径调用.
 * 有 callback → 调 callback (异步); 无 callback → complete(&done) (同步).
 * 同时取消异步超时定时器 (若已武装). */
void powerfs_req_complete(struct powerfs_request *req);

/* 将 powerfs-net 响应状态码转为 Linux errno (供异步 callback 使用) */
int net_status_to_errno(__u16 status);

/* 从 Master GetTopology 获取 volume 路由表 (volume_id → addr).
 * 在 pool_init 后调用, 填充 vol_routes[] 用于 ReadNeedle 路由. */
int powerfs_net_discover_volumes(const char *master_addrs, __u16 master_port);

/* Phase 3: RangeLease 续约 (直连 volume server).
 * volume_id: 目标 volume (路由查找)
 * ino: inode 号 (lease 按 inode + stripe 注册)
 * token: lease token (acquire 时返回)
 * token_len: token 实际长度
 * new_expire_jiffies: 输出, 续约成功后的新过期时间 (jiffies)
 * 返回 0 成功, <0 错误 (网络不可达/token 失效等) */
int powerfs_net_renew_lease(__u64 volume_id, __u64 ino,
                            const char *token, size_t token_len,
                            unsigned long *new_expire_jiffies);

/* Phase 3: RangeLease 获取 (直连 volume server).
 * volume_id: 目标 volume (路由查找)
 * ino: inode 号 (lease 按 inode + stripe 注册)
 * stripe_start: stripe 起始偏移 (bytes, 64MB 对齐)
 * stripe_count: stripe 数量 (通常为 1)
 * client_id: 持有者标识 (用于冲突检测)
 * token_out: 输出, lease token (调用方提供 >=64 字节缓冲区)
 * token_len_out: 输出, token 实际长度
 * epoch_out: 输出, lease epoch
 * content_size_out: 输出, 当前 content_size (避免额外 getattr)
 * expire_jiffies_out: 输出, 过期时间 (jiffies)
 * 返回 0 成功, <0 错误 */
int powerfs_net_acquire_lease(__u64 volume_id, __u64 ino,
                              __u64 stripe_start, __u64 stripe_count,
                              const char *client_id,
                              char *token_out, size_t *token_len_out,
                              __u64 *epoch_out, __u64 *content_size_out,
                              unsigned long *expire_jiffies_out);

/* Phase 3: RangeLease 释放 (直连 volume server).
 * volume_id: 目标 volume
 * ino: inode 号
 * token: lease token (acquire 时返回)
 * token_len: token 实际长度
 * client_id: 持有者标识
 * 返回 0 成功, <0 错误 */
int powerfs_net_release_lease(__u64 volume_id, __u64 ino,
                              const char *token, size_t token_len,
                              const char *client_id);

/* ========== 初始化/清理 ========== */

int  powerfs_net_init(void);
void powerfs_net_exit(void);

/* ========== 内部工具 ========== */

/* CRC32C 计算 */
__u32 powerfs_crc32c(const __u8 *data, size_t len);

/* 帧头编解码 */
void powerfs_net_frame_hdr_encode(struct powerfs_net_frame_hdr *hdr,
                                   __u16 msg_type, __u8 flags,
                                   __u32 seq, __u16 status,
                                   __u32 body_len, __u32 data_len,
                                   __u8 route_hash);
bool powerfs_net_frame_hdr_decode(const __u8 *buf, size_t len,
                                   struct powerfs_net_frame_hdr *hdr);

/* 帧发送/接收 (内部) */
int  powerfs_net_frame_send(struct socket *sock,
                             struct powerfs_net_frame_hdr *hdr,
                             const __u8 *body, size_t body_len,
                             const __u8 *data, size_t data_len);
int  powerfs_net_frame_recv(struct socket *sock,
                             struct powerfs_net_frame_hdr *hdr,
                             __u8 *body_buf, size_t body_cap, size_t *body_len,
                             __u8 *data_buf, size_t data_cap, size_t *data_len,
                             int timeout_ms);

#endif /* _POWERFS_NET_H */
