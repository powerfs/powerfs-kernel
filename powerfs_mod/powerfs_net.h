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
#define POWERFS_NET_MAX_FRAME   (1024 * 1024)  /* 1MB - 适合内核 */
#define POWERFS_NET_MAX_TLV     65535  /* 64KB - 1 */
#define POWERFS_NET_MAX_BODY    (64 * 1024)  /* 64KB body */
#define POWERFS_NET_MAX_DATA    (256 * 1024)  /* 256KB data */

/* 连接超时 (ms) */
#define POWERFS_NET_CONNECT_TIMEOUT  5000
#define POWERFS_NET_SEND_TIMEOUT     10000
#define POWERFS_NET_RECV_TIMEOUT     10000
#define POWERFS_NET_RECONNECT_DELAY  2000

/* 最大重连次数 */
#define POWERFS_NET_MAX_RECONNECT    10

/* ========== 帧标志 ========== */

#define POWERFS_NET_FLAG_REQUEST   0x01
#define POWERFS_NET_FLAG_RESPONSE  0x02
#define POWERFS_NET_FLAG_NOTIFY    0x04
#define POWERFS_NET_FLAG_BATCH     0x08
#define POWERFS_NET_FLAG_ACK       0x10

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
    POWERFS_NET_MSG_PUSH_DELTA = 0x0030,
    POWERFS_NET_MSG_PULL_DELTA = 0x0031,
    POWERFS_NET_MSG_INVALIDATE = 0x0032,

    /* 状态 */
    POWERFS_NET_MSG_STATFS = 0x0040,

    /* Master 操作 */
    POWERFS_NET_MSG_ASSIGN = 0x0050,
    POWERFS_NET_MSG_LOOKUP_VOLUME = 0x0051,
    POWERFS_NET_MSG_HEARTBEAT = 0x0052,
    POWERFS_NET_MSG_KEEP_CONNECTED = 0x0053,
    POWERFS_NET_MSG_VOLUME_LIST = 0x0054,

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
    POWERFS_NET_FLD_GENERATION = 0x15,
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

    /* Delta 同步字段 */
    POWERFS_NET_FLD_CLIENT_ID = 0x30,
    POWERFS_NET_FLD_SEQ = 0x31,
    POWERFS_NET_FLD_VCLOCK_ENTRIES = 0x32,
    POWERFS_NET_FLD_DELTA_OPS = 0x33,

    /* Lease 字段 */
    POWERFS_NET_FLD_LEASE_ID = 0x40,
    POWERFS_NET_FLD_LEASE_DURATION = 0x41,
    POWERFS_NET_FLD_LEASE_EPOCH = 0x42,

    /* Rename 字段 */
    POWERFS_NET_FLD_NEW_PARENT_INO = 0x50,
    POWERFS_NET_FLD_NEW_NAME = 0x51,
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
    __u8 reserved[6];   /* 保留 */
    __le32 header_crc;  /* CRC32C (little-endian) */
} __attribute__((packed));

/* 编译时断言帧头大小 */
#define POWERFS_NET_FRAME_HDR_SIZE 28

/* ========== 握手结构 ========== */

/* 握手请求 (18 字节) */
struct powerfs_net_handshake_req {
    __u8 magic[4];       /* "PFSN" */
    __u8 version;        /* 0x01 */
    __u8 client_type;    /* 客户端类型 */
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

/* ========== powerfs-net 连接管理 ========== */

/* 连接状态 */
enum powerfs_net_state {
    POWERFS_NET_STATE_DISCONNECTED = 0,
    POWERFS_NET_STATE_CONNECTING,
    POWERFS_NET_STATE_CONNECTED,
    POWERFS_NET_STATE_HANDSHAKING,
    POWERFS_NET_STATE_ERROR,
    POWERFS_NET_STATE_RECONNECTING,
};

/* 请求上下文 (用于异步请求/响应匹配) */
struct powerfs_net_request {
    struct hlist_node node;     /* 哈希表节点 */
    struct completion done;     /* 请求完成事件 */
    __u32 seq;                  /* 序列号 (作为哈希键) */
    __u16 msg_type;             /* 消息类型 */
    int status;                 /* 结果状态 (0=成功, 负值=错误) */
    __u8 *resp_body;            /* 响应 body 数据 */
    size_t resp_body_len;       /* 响应 body 长度 */
    __u8 *resp_data;            /* 响应 data 数据 */
    size_t resp_data_len;       /* 响应 data 长度 */
};

/* 连接上下文 */
struct powerfs_net_conn {
    /* 网络连接 */
    struct socket *sock;
    /*
     * sock 引用计数: send_request 使用 sock 期间 inc, disconnect 置
     * g_conn.sock=NULL 后 wait_event 等 sock_users==0 再 close, 防止
     * 并发 recv 使用已释放 socket 的 use-after-free (QEMU 回归中
     * ls/readdir 触发的 _raw_spin_lock_irqsave NULL deref 根因).
     */
    atomic_t sock_users;
    wait_queue_head_t sock_user_wq;
    struct sockaddr_storage peer_addr;
    int peer_len;

    /* 当前连接的目标地址 (用于 find_leader 判断是否需要重连,
     * 避免 disconnect 与并发请求 recv 竞态导致 NULL deref) */
    char cur_addr[64];
    __u16 cur_port;

    /* 状态管理 */
    enum powerfs_net_state state;
    atomic_t seq_counter;
    atomic_t pending_count;

    /* 锁 */
    struct mutex conn_lock;     /* 连接操作锁 */
    spinlock_t req_lock;        /* 请求表锁 */

    /* 待处理请求表 (seq -> request 映射) */
    DECLARE_HASHTABLE(pending_reqs, 8);

    /* 接收缓冲区 */
    __u8 *recv_buf;
    size_t recv_buf_len;

    /* 发送缓冲区 */
    __u8 *send_buf;
    size_t send_buf_len;

    /* 重连工作 */
    struct work_struct reconnect_work;
    int reconnect_count;
    atomic_t stopping;          /* 模块退出时置 1，让 reconnect_work 提前退出（atomic_t 保证跨 CPU 可见性） */

    /* 服务端信息 (握手后) */
    __u64 server_id;
    __u32 server_features;
};

/* ========== 多连接池配置 ========== */

#define POWERFS_NET_MAX_SERVERS    8    /* 最大服务器数量 (Filer + Master) */
#define POWERFS_NET_MAX_PATHS      256   /* Delta Sync 最大路径缓存数 */
#define POWERFS_NET_MONITOR_INTERVAL 5000  /* 健康检查间隔 (ms) */
#define POWERFS_NET_LEADER_CHECK_INTERVAL 2000  /* Leader 检查间隔 (ms) */

/* 服务器类型 */
enum powerfs_net_server_type {
    POWERFS_NET_SERVER_FILER = 0,
    POWERFS_NET_SERVER_MASTER = 1,
    POWERFS_NET_SERVER_VOLUME = 2,
};

/* 服务器条目 */
struct powerfs_net_server_entry {
    char addr[64];              /* 服务器地址 */
    __u16 port;                 /* 端口 */
    enum powerfs_net_server_type type;  /* 服务器类型 */
    bool is_leader;             /* 是否为 leader */
    int last_ping_ms;           /* 最后一次 ping 延迟 */
    __u64 last_check_time;      /* 最后检查时间 (jiffies) */
};

/* 连接池结构 */
struct powerfs_net_pool {
    /* 服务器配置 */
    struct powerfs_net_server_entry servers[POWERFS_NET_MAX_SERVERS];
    int server_count;
    int filer_count;
    int master_count;
    int volume_count;
    
    /* 当前活跃索引 */
    atomic_t active_filer_idx;
    atomic_t active_master_idx;
    atomic_t active_volume_idx;
    
    /* leader 相关 */
    atomic_t leader_idx;        /* 当前 leader 在 filers 中的索引 */
    atomic_t leader_known;      /* leader 已知标志 */
    
    /* 故障转移统计 */
    atomic_t failover_count;
    atomic_t last_failover_time;
    
    /* 锁 */
    struct mutex pool_lock;
    
    /* 监控线程 */
    struct delayed_work monitor_work;
    struct delayed_work leader_check_work;
    bool monitoring;
};

/* Delta Sync 路径条目 */
struct powerfs_net_path_entry {
    char path[256];            /* 路径 */
    __u64 generation;          /* 当前 generation */
    __u64 last_update;         /* 最后更新时间 (jiffies) */
    bool valid;                /* 是否有效 */
};

/* Delta Sync 状态 */
struct powerfs_net_delta_state {
    struct powerfs_net_path_entry paths[POWERFS_NET_MAX_PATHS];
    atomic_t path_count;
    spinlock_t gen_lock;
    
    /* 全局 generation 追踪 */
    atomic_t global_generation;
    
    /* 最后一次全量同步时间 */
    __u64 last_full_sync;
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

/* 设置主 Filer 地址 (兼容旧接口) */
int powerfs_net_set_primary(const char *addr, __u16 port);

/* 设置多个 Filer 地址 (逗号分隔) */
int powerfs_net_set_filers(const char *addrs, const char *ports);

/* 设置 Master 地址 */
int powerfs_net_set_master(const char *addr, __u16 port);

/* 设置 Volume 地址 */
int powerfs_net_set_volume(const char *addr, __u16 port);

/* 连接管理 */
int  powerfs_net_connect(const char *addr, __u16 port);
void powerfs_net_disconnect(void);
bool powerfs_net_is_connected(void);

/* Leader 管理 */
int powerfs_net_switch_leader(int new_idx);
int powerfs_net_find_leader(void);
int powerfs_net_leader_ping(void);
bool powerfs_net_has_leader(void);
int powerfs_net_get_leader_idx(void);

/* 故障转移 */
int powerfs_net_failover(void);
void powerfs_net_start_monitor(void);
void powerfs_net_stop_monitor(void);

/* ========== Delta Sync API ========== */

/* 路径 generation 管理 */
int powerfs_net_set_path_generation(const char *path, __u64 generation);
__u64 powerfs_net_get_path_generation(const char *path);
bool powerfs_net_path_stale(const char *path, __u64 cached_generation);
void powerfs_net_invalidate_path(const char *path);
void powerfs_net_invalidate_dir(__u64 dir_ino);
void powerfs_net_clear_all_generations(void);

/* 增量同步 */
int powerfs_net_pull_delta(const char *path, __u64 *new_generation);
int powerfs_net_push_delta(const char *path, __u64 generation);
int powerfs_net_full_sync(void);

/* 全局 generation */
__u64 powerfs_net_get_global_generation(void);
void powerfs_net_inc_global_generation(void);

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
int powerfs_net_send_request(__u16 msg_type,
                             const __u8 *body, size_t body_len,
                             const __u8 *data, size_t data_len,
                             __u8 *resp_body, size_t resp_body_cap,
                             __u8 *resp_data, size_t resp_data_cap,
                             int timeout_ms,
                             size_t *resp_body_len_out,
                             size_t *resp_data_len_out);

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

/* LOOKUP (返回完整属性含时间戳) */
int powerfs_net_lookup(__u64 dir_ino, const char *name, size_t name_len,
                       __u64 *ino, __u32 *mode, __u32 *uid, __u32 *gid,
                       __u64 *size, __u32 *nlink,
                       __u64 *mtime, __u64 *atime, __u64 *ctime);

/* GETATTR (返回完整属性含时间戳) */
int powerfs_net_getattr(__u64 ino, __u32 *mode, __u32 *uid, __u32 *gid,
                         __u64 *size, __u32 *nlink,
                         __u64 *mtime, __u64 *atime, __u64 *ctime);

/* SETATTR */
int powerfs_net_setattr(__u64 ino, __u32 mode_valid, __u32 mode,
                         __u32 uid, __u32 gid, __u64 size);

/* CREATE / MKDIR */
int powerfs_net_create(__u64 dir_ino, const char *name, size_t name_len,
                        __u32 mode, __u32 uid, __u32 gid, bool is_dir,
                        __u64 *ino_ret);

/* UNLINK / RMDIR */
int powerfs_net_unlink(__u64 dir_ino, const char *name, size_t name_len,
                       bool is_dir);

/* RENAME */
int powerfs_net_rename(__u64 old_dir_ino, const char *old_name, size_t old_name_len,
                       __u64 new_dir_ino, const char *new_name, size_t new_name_len);

/* READDIR (匹配 Filer 协议: ParentIno + Limit + LastName 分页) */
int powerfs_net_readdir(__u64 dir_ino, const char *last_name, __u64 limit,
                        struct powerfs_net_dir_entry *entries, __u32 max_entries,
                        __u32 *actual_count, bool *has_more);

/* READ */
int powerfs_net_read(__u64 ino, __u64 offset, __u32 length,
                     __u8 *buf, size_t buf_cap, __u32 *read_len);

/* WRITE */
int powerfs_net_write(__u64 ino, __u64 offset, const __u8 *data, size_t data_len,
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

/* ========== 初始化/清理 ========== */

int  powerfs_net_init(void);
void powerfs_net_exit(void);

/* 暴露模块参数 (供 fill_super fallback 使用) */
const char *powerfs_net_get_server_addr(void);
__u16       powerfs_net_get_server_port(void);

/* ========== 内部工具 ========== */

/* CRC32C 计算 */
__u32 powerfs_crc32c(const __u8 *data, size_t len);

/* 帧头编解码 */
void powerfs_net_frame_hdr_encode(struct powerfs_net_frame_hdr *hdr,
                                   __u16 msg_type, __u8 flags,
                                   __u32 seq, __u16 status, __u32 data_len);
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
