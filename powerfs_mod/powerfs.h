#ifndef _POWERFS_H
#define _POWERFS_H

#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/fs_context.h>
#include <linux/netfs.h>
#include <linux/xattr.h>
#if 0 /* DEAD_CODE — powerfs_lock.h: entire header body inside #if 0 DEAD_CODE (audit 2026-08-23).
     * All MDLock/lock_client/tlk_codec content in that header is disabled; keeping
     * the #include here would only pull in the include-guard macro and ~60 lines of
     * detailed block comments explaining WHY it's dead — nothing functional. */
#include "powerfs_lock.h"  /* MDLock 独立锁对象 */
#endif /* DEAD_CODE */
#include "powerfs_net_transport.h"  /* enum powerfs_transport_type + transport_ops */

/* ========== 常量定义 ========== */

#define POWERFS_SUPER_MAGIC     0x50574552  /* "POWE" */
#define POWERFS_ROOT_INO        1
#define POWERFS_INO_START       2
#define POWERFS_MAX_NAME_LEN    255
#define POWERFS_VERSION         "2.0.0-powerfs-style"

/* 目录 lease 超时 (jiffies, 默认 5 秒).
 * 绑定在父目录 inode 上, 控制该目录下所有子 dentry (正/负) 的缓存有效期.
 * - readdir / lookup 成功后设为 now + TTL
 * - 本地 mutation (mkdir/rmdir/create/unlink/rename) 主动清零
 * - d_revalidate 检查此字段, 永不返回 0 (避免 RCU stall)
 * 平衡: 太短 → 频繁网络查询; 太长 → 跨客户端可见性延迟 */
#define POWERFS_DIR_LEASE_TTL       (5 * HZ)

/* Inode cache 超时 (jiffies, 默认 10 秒) */
#define POWERFS_INODE_CACHE_TTL     (10 * HZ)

/* Chunk 和 Stripe 大小
 * chunk_size 与用户态 (powerfs-layout) 统一为 1MB, 见 file-layout-design.md §9.
 * stripe_size (64MB) 是写批处理 lease 单元, 非布局 Placement::Stripe 的 stripe_size. */
#define POWERFS_CHUNK_SIZE      (1 * 1024 * 1024)    /* 1MB — 与用户态 P4 统一 */
#define POWERFS_STRIPE_SIZE     (64 * 1024 * 1024)   /* 64MB */

/* K2: Inline 小文件硬上限 (与 FUSE INLINE_HARD_LIMIT / Filer INLINE_HARD_LIMIT 一致).
 * 超过此大小的文件不走 Inline 模式, 直接走 Flat.
 * inline_data 在 inode 中用指针 (kmalloc), 仅 Inline 文件分配, 避免浪费 slab. */
#define POWERFS_INLINE_MAX_SIZE (8 * 1024)           /* 8KB */
/* K2: 网络响应缓冲区容量 — LOOKUP/GETATTR/CREATE 响应可能携带 inline_data,
 * 需容纳 8KB inline + ~512B 元数据开销. 栈上分配不安全, 用 kvmalloc. */
#define POWERFS_NET_RESP_INLINE_CAP  (POWERFS_INLINE_MAX_SIZE + 512)
#define POWERFS_LEASE_DURATION  (30 * HZ)
#define POWERFS_LEASE_DURATION_MS  30000   /* 30s in ms, for mdlock use */
/* Phase 3: lease 续约阈值. 当 lease 剩余有效期 < 阈值时触发续约.
 * 设为 DURATION/3 (10s): 在过期前 10s 续约, 留足网络往返 + 重试时间.
 * 参考  cap renew 在 expiry 前主动续约. */
#define POWERFS_LEASE_RENEW_THRESHOLD (POWERFS_LEASE_DURATION / 3)

/* ========== Capability (Lease) 位定义 — 对齐  CEPH_CAP_* (src/include/powerfsfs/types.h)
 *
 * 用于 struct powerfs_cap 的 issued / implemented / wanted / dirty_caps 字段.
 * 与  完全对齐的 8 位掩码 + PIN 高阶位, 为后续 revoke/release 三层语义铺路:
 *   - issued:       Filer/Master 授予的能力 (权威权限位, 来自 lookup/open/getattr 响应)
 *   - implemented:  本地仍持有的权限超集, 用于优雅降级 (先收到 issued<old,
 *                   写回脏数据后才能 implemented=issued, 避免 revoke 期间丢失数据)
 *   - wanted:       客户端实际需要的权限 (派生自 i_nr_by_mode[] 与 in-flight I/O),
 *                   与 issued 不匹配时主动向 Filer 请求 AcquireCap/ReleaseCap
 *   - dirty_caps:   哪些字段 (size/mtime/uid/...) 本地有脏数据需要写回,
 *                   revoke 发现 dirty_caps & 目标要撤的位 != 0 必须 FlushThenAck
 * 参考:
 *   - linux-6.17/include/linux/powerfs/powerfs_fs.h
 *   - powerfs/src/client/Inode.h Cap + Inode::dirty_caps */

#define POWERFS_CAP_PIN        (1 << 0)   /* 基础引用 (有 inode 活着就有) */
#define POWERFS_CAP_AUTH_SHARED (1 << 1)  /* AUTH_SHARED: 读权限 (attrs+data) */
#define POWERFS_CAP_AUTH_EXCL  (1 << 2)   /* AUTH_EXCL: 独占写权限 (可改 size/attrs) */
#define POWERFS_CAP_LINK_SHARED (1 << 3)  /* 可创建/查找硬链接 */
#define POWERFS_CAP_XATTR_SHARED (1 << 4) /* 可读 xattr */
#define POWERFS_CAP_XATTR_EXCL  (1 << 5)  /* 可写 xattr */
#define POWERFS_CAP_FILE_SHARED (1 << 6)  /* FILE_SHARED: 可读文件数据 (缓存) */
#define POWERFS_CAP_FILE_CACHE  (1 << 7)  /* FILE_CACHE: 可缓存读 (跨进程信任) */
#define POWERFS_CAP_FILE_WR    (1 << 8)   /* FILE_WR: 可写文件数据 (独占/主副本) */
#define POWERFS_CAP_FILE_EXCL  (1 << 9)   /* FILE_EXCL: 独占写 (可追加/truncate) */

/* 常用组合位掩码 (派生, 与  CEPH_CAP_ANY_* 对齐) */
#define POWERFS_CAP_RDCACHE     (POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE)
#define POWERFS_CAP_WR_DATA     (POWERFS_CAP_FILE_WR | POWERFS_CAP_FILE_EXCL)
#define POWERFS_CAP_ANY_RD      (POWERFS_CAP_AUTH_SHARED | POWERFS_CAP_FILE_SHARED)
#define POWERFS_CAP_ANY_WR      (POWERFS_CAP_AUTH_EXCL | POWERFS_CAP_FILE_WR)
#define POWERFS_CAP_ANY_DIRTY   (POWERFS_CAP_AUTH_EXCL | POWERFS_CAP_WR_DATA | \
                                 POWERFS_CAP_XATTR_EXCL)

/* Cap 引用模式位下标 (用于 i_nr_by_mode[] / i_pin_ref 等数组, 对齐 CEPH_FILE_MODE_BITS) */
enum {
    POWERFS_FILE_MODE_RD = 0,
    POWERFS_FILE_MODE_WR,
    POWERFS_FILE_MODE_CACHE,
    POWERFS_FILE_MODE_MAX
};
#define POWERFS_FILE_MODE_BITS  POWERFS_FILE_MODE_MAX

/* inode 级 flag (对齐  Inode.h I_COMPLETE / I_DIR_ORDERED 等)
 * 放在 struct powerfs_inode_info.i_flags, 取代原来的几个孤立 bool. */
#define POWERFS_I_COMPLETE       (1 << 0)  /* 目录缓存完整, 支持负 dentry 信任 */
#define POWERFS_I_DIR_ORDERED    (1 << 1)  /* 目录操作有序, readdir 顺序是原子快照 */
#define POWERFS_I_KICK_FLUSH     (1 << 2)  /* pending dirty 需尽快 kick 回写 */
#define POWERFS_I_CAP_DROPPED    (1 << 3)  /* cap 已被服务端 revoke, 不再信任缓存 */
#define POWERFS_I_ERROR_LOCK     (1 << 4)  /* lock 错误需向上层报告 */
#define POWERFS_I_TRUNC_PENDING  (1 << 5)  /* 有未完成的 truncate 工作项 */
#define POWERFS_I_DIRTY          (1 << 6)  /* 元数据 (attrs/xattr) 有脏位 */
#define POWERFS_I_WRITEBACK      (1 << 7)  /* 正在数据写回中 (wb_batch_count > 0) */

/* mount_state 枚举 (对齐  CEPH_MOUNT_*)
 * 取代原来 shutting_down / initialized 两个 bool, 避免中间态 race. */
enum {
    POWERFS_MOUNT_MOUNTING = 0,
    POWERFS_MOUNT_MOUNTED,
    POWERFS_MOUNT_UNMOUNTING,
    POWERFS_MOUNT_UNMOUNTED,
    POWERFS_MOUNT_SHUTDOWN,
};

/* ========== writepages 批量写配置 ==========
 *
 * 通过 mount option `write_batch_kb` 配置单次 work item 收集的脏页总量:
 *   - 默认 64KB (16 pages): 普通TCP网络安全值
 *   - ROCE 网络推荐 1MB (256 pages): 降低 work item 数量, 提升吞吐
 *   - 最大 stripe size 64MB (16384 pages): 单 stripe 一次提交, 内存压力大
 *
 * 范围: 4KB (1 page) ~ POWERFS_STRIPE_SIZE (64MB)
 * 注意: max_active=4 时, 64MB 批次最多持 65536 页 (256MB) 内存 */
#define POWERFS_WRITE_BATCH_DEFAULT_KB   64                      /* 64KB = 16 pages */
#define POWERFS_WRITE_BATCH_MIN_KB       4                       /* 4KB = 1 page */
#define POWERFS_WRITE_BATCH_MAX_KB       (POWERFS_STRIPE_SIZE / 1024)  /* 64MB = 65536 KB */

/* ========== 前向声明 ========== */

struct powerfs_sb_info;
struct powerfs_inode_info;
struct powerfs_dentry_info;
struct powerfs_client;
struct powerfs_dir_file_info;

/* 响应结构 (定义在 powerfs_comm.h) */
struct powerfs_lookup_resp;
struct powerfs_getattr_resp;
struct powerfs_create_resp;
struct powerfs_setattr_resp;
struct powerfs_write_resp;
struct powerfs_readlink_resp;
struct powerfs_dirent;

/* 请求结构 (定义在 powerfs_comm.h) */
struct powerfs_lookup_req;
struct powerfs_create_req;

/* ========== Dentry 私有数据 — 对齐 powerfs_dentry_info (linux-6.17/fs/powerfs/super.h L303-315)
 *                                        + Dentry.h L99-L110 (用户态)
 *                                        + Rust CachedEntry (powerfs-fuse/src/cache.rs L129-L144)
 * 三层语义对齐:
 *   Layer 1: per-dentry lease (dentry_lease_expire > now → DentryLeaseStatus::LeaseValid)
 *   Layer 2: dir_shared_gen matches && dir has I_COMPLETE flag   (→ SharedGenValid)
 *   Layer 3: 否则必须 revalidate RPC (→ Expired/Miss)
 *
 * Note: 保留原有 time / offset / rcu 字段避免破坏 powerfs_fs.c 现有代码. */

/* dentry_info flag 位 (对齐 CEPH_DENTRY_REFERENCED / LEASE_LIST / SHRINK_LIST 等) */
#define POWERFS_DN_REFERENCED     (1 << 0)   /* 最近被访问, 影响 shrinker 选择 */
#define POWERFS_DN_LEASE_LIST     (1 << 1)   /* 已挂到 client->dentry_lease_list */
#define POWERFS_DN_SHRINK_LIST    (1 << 2)   /* 已挂到 shrinker 候选链表 */
#define POWERFS_DN_PRIMARY_LINK   (1 << 3)   /* inode 的首个 parent dentry (硬链接场景) */
#define POWERFS_DN_ASYNC_UNLINK   (1 << 4)   /* 正在进行 async unlink */
#define POWERFS_DN_NEGATIVE       (1 << 5)   /* 负 dentry (inode==0), 用于 I_COMPLETE 下的 ENOENT 信任 */

struct powerfs_dentry_info {
    struct dentry *dentry;            /* 所属 dentry */
    unsigned long time;               /* 创建时间 (调试用, 保持) */
    u64 offset;                       /* readdir 偏移 (保持) */
    struct rcu_head rcu;              /* RCU 延迟释放 (保持) */

    /* === 对齐 powerfs_dentry_info: lease 链 + LRU + shrinker === */
    struct list_head lease_list;      /* 挂 client->dentry_lease_list */
    struct hlist_node hnode;          /* 全局/按 dir hash (可选) */
    unsigned long flags;              /* POWERFS_DN_* 位掩码 */

    /* === 对齐 powerfs_dentry_info: per-dentry lease 三层校验 (Layer 1) === */
    u64 lease_issuer_id;              /* 发放 lease 的 Filer node_id (session 关联) */
    unsigned long lease_renew_after;  /* 到达此 jiffies 开始异步续约 */
    unsigned long lease_renew_from;   /* 续约窗口起点 (可开始发 Renew 请求) */
    unsigned long lease_expire;       /* Layer 1: dentry lease 到期时间 (jiffies) */
    u64 lease_duration_ms;            /* 本次发放的 TTL (ms), 续约用 */

    /* === 序列号屏障 (对齐 powerfs lease_gen/lease_seq) === */
    u32 lease_gen;                    /* lease 代次, Filer 重发新 lease 时 +1 */
    u32 lease_seq;                    /* 序列号, revoke/invalidate 消息需要 seq > 此数才生效 */

    /* === 对齐 Rust CachedEntry::dir_shared_gen (Layer 2: shared_gen 匹配) ===
     * 本 dentry 最后一次被验证为可信时父目录的 dir_version.
     * d_revalidate 若 Layer 1 (TTL) 过期, 进入 Layer 2:
     *   if dir_version_current == dir_shared_gen && dir has I_COMPLETE → 仍然可信 */
    u64 dir_shared_gen;
};

/* 获取 dentry 私有数据 */
static inline struct powerfs_dentry_info *POWERFS_D(struct dentry *dentry)
{
    return dentry->d_fsdata;
}

/* ========== 目录文件私有数据 (参考 powerfs_dir_file_info) ========== */

struct powerfs_dir_file_info {
    struct file *file;
    struct powerfs_inode_info *dir;
    
    /* readdir 状态 */
    u64 last_ino;           /* 最后读取的 inode 号 */
    char *last_name;        /* 最后读取的文件名 (用于断点续传) */
    u32 next_offset;        /* 下一个偏移量 */
    
    /* 缓存的目录项 (从通信层获取的批量结果) */
    struct powerfs_dirent *cached_entries;
    u32 cached_count;
    u32 cached_index;
    
    /* 锁 */
    struct mutex lock;
};

/* 目录项结构 (用于本地 readdir) */
struct powerfs_dir_entry {
    struct list_head list;          /* 链表节点 */
    u64 ino;                        /* inode 号 */
    unsigned int type;              /* 文件类型 (DT_REG, DT_DIR, 等) */
    char name[POWERFS_MAX_NAME_LEN + 1]; /* 文件名 (255 chars + null) */
    bool deleted;                   /* 标记删除: 保持链表位置稳定, readdir 跳过 */
    /* fetch_epoch: 上次从 Filer 拉取到该 name 时的 dir_fetch_epoch 值.
     * readdir refetch 完成后, active 条目中 fetch_epoch != 当前 dir_fetch_epoch
     * 的 (即本次 refetch 未被 Filer 返回) 标记为 deleted, 清理 stale active 条目.
     * 解决 root cause: 之前 refetch 只追加/undelete, 从不清理本地 stale active,
     * 导致历史测试遗留的已删除文件名无限堆积 (实测 dir_ino=1 累积 1862 active
     * 而 Filer 实际仅 109 个文件). */
    u64 fetch_epoch;
};

/* ========== Inode 扩展结构 (参考 powerfs_inode_info) ========== */

/* Chunk 映射条目 */
struct powerfs_chunk_map {
    u32 chunk_idx;
    u64 needle_id;
    u64 volume_id;
    u32 crc32;
    u64 size;   /* chunk 有效数据大小 (字节), 用于 Filer 元数据同步 */
};

/* ==============================================================
 * Cap / Lease / Flush 结构层 — 对齐  powerfs_cap / powerfs_cap_flush / powerfs_cap_snap
 *
 * 说明:
 *   powerfs_cap       → powerfs_cap (per-inode-per-session 授权单元, 含双轨 issued/implemented)
 *   powerfs_cap_flush → powerfs_cap_flush (正在向 Filer 发送的 CapFlush/CapRelease 消息)
 *   powerfs_cap_snap  → powerfs_cap_snap (snapshot 发生时，冻结被 snap 出去的 size/mtime/...
 *                                      直到 flush 完才合并)
 * 为了兼容现有 powerfs_lease_* 代码，保留 typedef powerfs_lease。
 * ============================================================== */

/* 正在 flush (等待 Filer ACK) 的 CapFlush/CapRelease 记录
 * 对齐 powerfs_cap_flush (linux-6.17/fs/powerfs/super.h L208-215). */
struct powerfs_cap_flush {
    u64 tid;               /* 对应通信层 request->tid, 用于 ACK 匹配 */
    unsigned int caps;     /* 本次 flush 携带的 dirty cap bitmask */
    bool wake;             /* 收到 ACK 后唤醒 i_cap_wq 等待者? */
    bool is_capsnap;       /* 来自 cap_snap, 否则普通 dirty_caps flush */
    struct list_head g_list; /* 全局 client->cap_flush_list 链表 */
    struct list_head i_list; /* 每 inode i_cap_flush_list 链表 */
};

/* Cap Snap — 快照发生时的"冻结状态"，用于在新 epoch 写回未完成之前
 * 保证旧 epoch 状态能被正确 flush.
 * 对齐 powerfs_cap_snap (linux-6.17/fs/powerfs/super.h L222-249) */
struct powerfs_cap_snap {
    refcount_t nref;
    struct list_head ci_item;   /* 挂在 inode->i_cap_snaps */

    struct powerfs_cap_flush cap_flush;  /* 内嵌 flush 追踪 */

    u64 follows;          /* 对应 snap 生成的 epoch (cap snap 在哪个 epoch 后出现) */
    unsigned int issued;  /* snap 时刻 issued 权限 */
    unsigned int dirty;   /* snap 时刻 dirty_caps，需要 flush 的位 */

    /* snap 冻结的 inode 元数据快照 */
    umode_t mode;
    kuid_t  uid;
    kgid_t  gid;
    u64 size;
    struct timespec64 mtime, atime, ctime, btime;
    u64 truncate_size;
    u32 truncate_seq;

    int writing;           /* 同步写仍在进行 (计数) */
    int dirty_pages;       /* 仍未写回的脏页计数 */
    bool inline_data;      /* snap 中含 inline dirty data */
    bool need_flush;       /* 本 snap 需要先 flush 才能 ack revoke */
};

/* 主 Cap 结构 — 对齐 powerfs_cap + 合并原 powerfs_lease 的 per-stripe 字段
 *
 * 每个 inode 按 "session (Filer node id)" 有一个 cap (PowerFS 单 Filer 场景下通常只有
 * 一个 auth_cap)；多 Filer 分片 + authority migration 场景支持多 cap 红黑树。 */
struct powerfs_cap {
    struct powerfs_inode_info *ci;       /* back pointer to inode */

    /* 两个 rb_node, 分别挂载到两个独立的 rbtree:
     *   - ci_node   → key = issuer_id (session id), 挂 inode->i_caps
     *   - node      → key = stripe_start (字节偏移), 挂 inode->lease_tree
     *                 (兼容旧代码, lock_client 以 stripe 为单位拿锁) */
    struct rb_node ci_node;              /* per-inode cap rbtree (key = issuer_id) */
    struct rb_node node;                 /* per-stripe lease rbtree 兼容旧代码 */

    u64 cap_id;       /* Filer 分配的唯一 cap id (对 debugging/revoke 必用) */
    u64 issuer_id;    /* 发放 Filer/Master 的 node_id (session 关联键) */
    struct list_head session_caps;       /* 挂到 session cap list (未来) */
    struct list_head lru_item;           /* LRU item, 用于 cap_reclaim / shrinker */

    /* 授权 4 轨 (核心语义, 对齐 ) */
    unsigned int issued;       /* Filer 最新授予的权限位 (权威) */
    unsigned int implemented;  /* 本地仍在使用的权限超集 (≥issued, 用于优雅降级) */
    unsigned int wanted;       /* 客户端根据 open/IO 实际想要的位 */
    unsigned int mds_wanted;   /* 已经向 issuer 请求过的 wanted (防止重复请求) */

    /* 序列号屏障 (对齐 powerfs seq/issue_seq/mseq + cap_gen) */
    u64 seq;           /* 最新消息序列号, revoke/grant 消息 seq 老的丢弃 */
    u64 issue_seq;     /* 最近一次 grant 携带的 issue_seq, 区分不同 lease 代 */
    u32 mseq;          /* authority migration 序号, Filer 主从切换 +1 */
    u32 cap_gen;       /* active/stale cycle, 会话重建后旧 gen cap 批量失效 */

    /* Per-stripe lock 范围 — 保留并扩展原 powerfs_lease 字段
     * 未来当 lock_client 从 per-stripe 切到 per-cap 时这里作为主存储. */
    u64 stripe_start;
    u64 stripe_count;
    char token[64];
    u64 epoch;                 /* 分布式锁 epoch, 与 Master 心跳同步递增 */
    unsigned long expire_jiffies;  /* cap 过期时间 (jiffies), 到期需 renew/reacquire */
    unsigned long last_used;        /* LRU 最近使用 jiffies, 用于 reclaim */
    bool exclusive;          /* 是否为独占锁 (对应 CAP_FILE_EXCL) */
    u64 content_size;        /* 授权时同步返回的最新 content_size, 对齐最大写范围 */
};

/* 向后兼容 typedef — 旧代码里的 "struct powerfs_lease" 仍然可编译,
 * 待所有 lock_client.c / powerfs_fs.c 代码逐步改为 powerfs_cap. */
typedef struct powerfs_cap powerfs_lease_t;
#define powerfs_lease  powerfs_cap  /* 兼容 struct powerfs_lease * 旧指针 */

/* ========== FileLayout 枚举 (对齐 powerfs-layout crate) ========== */

/* Placement 类型 (FieldId 0xA0, u8 tag).
 * 值必须与 powerfs-layout/src/codec.rs placement_tag 一致 (wire 格式):
 *   INLINE=0, FLAT=1, STRIPE=2, WIDE_STRIPE=3
 * 注意: 默认值 0 = INLINE, 新 inode 必须显式设置 FLAT. */
enum powerfs_placement {
    POWERFS_PLACEMENT_INLINE    = 0,
    POWERFS_PLACEMENT_FLAT      = 1,
    POWERFS_PLACEMENT_STRIPE    = 2,
    POWERFS_PLACEMENT_WIDESTRIPE = 3,
};

/* Reliability 类型 (FieldId 0xA1, u8 tag) */
enum powerfs_reliability {
    POWERFS_RELIABILITY_SINGLE     = 0,
    POWERFS_RELIABILITY_REPLICATED = 1,
    POWERFS_RELIABILITY_EC         = 2,
};

/* ReliabilityState (FieldId 0xA2, u8) */
enum powerfs_reliability_state {
    POWERFS_RSTATE_PENDING     = 0,
    POWERFS_RSTATE_REPLICATED  = 1,
    POWERFS_RSTATE_EC          = 2,
    POWERFS_RSTATE_DEGRADED    = 3,
};

/* FileLayout 解析结果 (从 GETATTR/CREATE TLV 响应提取) */
struct powerfs_file_layout {
    u8 placement;           /* enum powerfs_placement */
    u8 reliability;         /* enum powerfs_reliability */
    u8 reliability_state;   /* enum powerfs_reliability_state */
    u32 chunk_size;         /* layout chunk_size, 默认 POWERFS_CHUNK_SIZE */
    u32 inline_max_size;    /* Inline 阈值 (K2) */

    /* === K2: Inline 数据 (从 0xAE InlineData 字段解析) ===
     * inline_data 在 parse 阶段 kmalloc, apply 阶段所有权转移给 inode
     * (layout->inline_data 置 NULL). 调用方负责 parse 失败时 kfree. */
    u8 *inline_data;            /* Inline 数据 (kmalloc), NULL=无 InlineData 字段 */
    u32 inline_len;             /* inline_data 实际长度 */

    /* === K3: Stripe/WideStripe 元数据 ===
     * 从 Placement(0xA0) 后续字段 + 独立 FieldId 解析.
     * volume_ids 由调用方 apply 后转入 inode (kmalloc), layout 本身
     * 仅持有指针, parse 阶段分配, apply 阶段所有权转移给 inode. */
    u64 stripe_size;            /* stripe unit 大小 (字节) */
    u32 stripe_count;           /* 条带卷数 */
    u32 start_volume_idx;       /* 起始卷索引 */
    u64 start_needle_id;        /* StripeDescriptor 首 needle_id (K3-5 预留) */
    u64 *volume_ids;            /* volume_ids 数组 (kmalloc), NULL=未解析 */
    u32 volume_ids_count;       /* volume_ids 数组长度 */

    /* === K4: Reliability EC 元数据 (从 0xA1 Reliability 解析) ===
     * EC tag=0x02 后续: data_shards u32 LE + parity_shards u32 LE.
     * 对齐 powerfs-layout codec.rs decode_reliability (L359). */
    u32 ec_data_shards;         /* EC 数据分片数, 0=非 EC */
    u32 ec_parity_shards;       /* EC 校验分片数 */

    /* === K4: 副本 chunk 列表 (从 0xB5 ReplicaChunks 解析) ===
     * 每个 ChunkRef 44 字节: offset/size/needle_id/volume_id/crc32/mtime.
     * 对齐 powerfs-layout codec.rs decode_chunk_list (L584).
     * parse 阶段 kmalloc, apply 阶段所有权转移给 inode. */
    struct powerfs_chunk_map *replica_chunks;  /* 副本 chunks (kmalloc), NULL=无 */
    u32 replica_count;                          /* replica_chunks 数组长度 */

    /* === K4-5: EC shards 列表 (从 ChunkLayout PER_CHUNK tag=0x01 解析) ===
     * EC 文件的所有 shards (data + parity), 按 group 连续排列:
     *   group 0: shards[0..total-1], group 1: shards[total..2*total-1], ...
     * 每个 ChunkRef 44 字节, 与 replica_chunks 格式相同.
     * 对齐 FUSE fuse.rs L2465-2473 ec_chunks 读取. */
    struct powerfs_chunk_map *ec_chunks;  /* EC shards (kmalloc), NULL=非EC */
    u32 ec_chunk_count;                    /* ec_chunks 数组长度 */

    bool has_placement;     /* 响应中是否包含 Placement 字段 */
    bool has_reliability;   /* 响应中是否包含 Reliability 字段 */
    bool has_inline_data;   /* 响应中是否包含 InlineData 字段 */
    bool has_replica_chunks;/* 响应中是否包含 ReplicaChunks 字段 */
    bool has_ec_chunks;     /* 响应中是否包含 EC chunks (PER_CHUNK) */
};

struct powerfs_inode_info {
    struct netfs_inode netfs;          /* 内含 struct inode，必须第一个字段 */

    u64 parent_ino;
    char name[POWERFS_MAX_NAME_LEN + 1];

    spinlock_t i_lock;

    /* ==============================================================
     * === Inode 版本与 flag 层 — 对齐 powerfs_inode_info (super.h L353-L360) ===
     * ============================================================== */
    u64 i_version;               /* Filer 侧 inode 全局版本, 每次 setattr/op 返回的最新 */
    u32 i_time_warp_seq;         /* mtime/atime timewarp 计数, 防止用户显式 utimes() 被晚到的 lease 回写覆盖 */
    unsigned long i_flags;       /* POWERFS_I_* 位图 (COMPLETE / DIR_ORDERED / TRUNC_PENDING / ...) */

    /* 目录 ops 完成计数 ( i_release_count / i_ordered_count),
     * 用于 cross-client barrier: mkdir 返回时 release_count<=ordered_count
     * 表示该目录所有历史 mutation 已对其它客户端可见. */
    atomic64_t i_release_count;
    atomic64_t i_ordered_count;

    /* ==============================================================
     * === Cap / Lease 管理层 (对齐 powerfs i_caps, dirty_caps 等) ===
     * ============================================================== */
    /* Lease 强一致 (参考 powerfs i_caps rbtree) */
    struct rb_root lease_tree;         /* per-stripe lease，按 stripe_start 排序 */
    spinlock_t lease_lock;             /* 保护 lease_tree */
    struct delayed_work lease_renew_work;

    /* === 对齐  powerfs_inode_info: 多 session cap + 脏/flush 追踪 === */
    struct rb_root i_caps;             /* per-session cap rbtree (key = issuer_id,
                                        * 含 powerfs_cap 的 issued/implemented) */
    struct powerfs_cap *i_auth_cap;    /* authoritative cap (主 Filer 的 cap, 快速路径) */
    unsigned int i_dirty_caps;         /* bitmask: 哪些 cap 位有未写回的脏数据 */
    unsigned int i_flushing_caps;      /* bitmask: 正在 CapFlush 传输中的脏位 */
    struct list_head i_dirty_item;     /* 挂到全局 dirty_cap 列表 (session / client level) */
    struct list_head i_flushing_item;  /* 挂到全局 flushing_cap 列表 */
    struct list_head i_cap_delay_list; /* 延迟释放的 cap list (close→reopen 抖动避免) */

    /* 正在等待 ACK 的 CapFlush 消息追踪 + 等待队列 (对齐 powerfs i_cap_flush_list + i_cap_wq) */
    struct powerfs_cap_flush *i_prealloc_cap_flush;  /* 预分配 1 个, 避免内存压力下 ENOMEM */
    struct list_head i_cap_flush_list; /* powerfs_cap_flush.i_list 链表头 */
    wait_queue_head_t i_cap_wq;        /* 等待 cap acquire/revoke ACK 的线程挂这里 */

    /* Cap snap 列表 (snapshot 冻结状态), 对齐 powerfs i_cap_snaps / i_head_snapc / i_snap_caps */
    struct list_head i_cap_snaps;      /* cap_snap list 头 */
    unsigned int i_snap_caps;          /* cap bits for snapped files */
    u64 i_head_snapc_epoch;            /* 当前写快照的 epoch */

    /* Cap 引用计数 (对齐 powerfs i_pin_ref / i_rd_ref / i_wr_ref 等)
     * 释放 cap 前必须 ref==0，防止写回途中 revoke 丢数据 */
    int i_pin_ref;
    int i_rd_ref, i_rdcache_ref, i_wr_ref, i_wb_ref, i_fx_ref;
    int i_wrbuffer_ref, i_wrbuffer_ref_head;
    atomic_t i_filelock_ref;           /* POSIX/FLK file lock 持有引用 */

    /* shared_gen + cache_gen (对齐  atomic_t i_shared_gen + rdcache_gen + rdcache_revoking)
     * - shared_gen: 每次该 inode 下 dentry 发生 FILE_SHARED 事件时自增
     *   子 dentry 的 dir_shared_gen 要和 父 inode 的 shared_gen 比对
     * - rdcache_gen:   每次获得 FILE_CACHE +1, 读路径上缓存的页如果 rdcache_gen 匹配则可信
     * - rdcache_revoking: 异步 RDCACHE 失效 (revoke in-flight) */
    atomic_t i_shared_gen;
    u32 i_rdcache_gen;
    u32 i_rdcache_revoking;

    /* per-mode open file count (对齐 powerfs i_nr_by_mode[])
     * mode = [RD, WR, CACHE], 用于派生 wanted caps. */
    int i_nr_by_mode[POWERFS_FILE_MODE_BITS];

    /* 读写计时 (对齐 powerfs i_last_rd / i_last_wr) */
    unsigned long i_last_rd;
    unsigned long i_last_wr;

    /* ==============================================================
     * === size/truncate 同步 (对齐  四方 size 同步) ===
     * ============================================================== */
    /* Max file size 四象限 (CTO 语义核心, 对齐 powerfs i_max_size / reported_size / ...):
     *   i_max_size           = Filer 授权可写的上限 (大于此 offset 不能写, 必须先 CapUpdate)
     *   i_reported_size      = 已向 Filer 报告 (请求) 过的 max_size
     *   i_wanted_max_size    = 实际 (从 write_iter kiocb.ki_pos 推导) 想写到的上限
     *   i_requested_max_size = 已发送请求且等待 ACK 的 wanted_max_size */
    u64 i_max_size;
    u64 i_reported_size;
    u64 i_wanted_max_size;
    u64 i_requested_max_size;

    /* Chunk 映射 (open/getattr 时从 Filer 获取) */
    struct powerfs_chunk_map *chunks;
    u32 chunk_count;
    u64 content_size;
    u64 volume_id;
    u64 file_key;   /* needle_id (base, chunk N = file_key + N), from Filer Create or GetAttr */

    /* === K1: FileLayout 元数据 (从 GETATTR/CREATE TLV 解析) === */
    u8 placement;           /* enum powerfs_placement, 默认 FLAT */
    u8 reliability;         /* enum powerfs_reliability, 默认 SINGLE */
    u8 reliability_state;   /* enum powerfs_reliability_state */
    u32 layout_chunk_size;  /* 从 GETATTR 解析的 chunk_size, 默认 POWERFS_CHUNK_SIZE */

    /* === K2: Inline 小文件数据缓冲 ===
     * placement==INLINE 时, 文件数据存储在 inline_data (kmalloc), 不走 Volume RPC.
     * - GETATTR/LOOKUP 响应含 InlineData (0xAE) 字段时, 分配并填充 inline_data
     * - write 路径: 小文件写入 inline_data, 标记 inline_dirty
     * - close/release: inline_dirty 时通过 UPDATE_INODE 提交到 Filer
     * - 累计写入 > inline_max_size×1.5 时触发迁移到 Flat (K2-7)
     * - 迁移到 Flat 后 inline_data 释放 (kfree + 置 NULL)
     * - evict_inode/free_inode 中确保 kfree(inline_data)
     *
     * 并发保护: inline_data 由 i_lock (spinlock) 保护, 写路径持有 i_lock 修改.
     * read 路径 (netfs_read_folio 回调) 持有 i_lock 读 inline_data. */
    u8 *inline_data;            /* Inline 文件数据 (kmalloc), NULL=非 Inline */
    u32 inline_len;             /* inline_data 实际长度 (字节) */
    u32 inline_max_size;        /* Inline 阈值 (从 Filer 获取, 默认 POWERFS_INLINE_MAX_SIZE) */
    bool inline_dirty;          /* inline_data 已修改, close 时需提交到 Filer */

    /* === K3: Stripe/WideStripe 多卷元数据 ===
     * 从 GETATTR/CREATE 响应解析, 由 powerfs_apply_layout_to_inode() 填充.
     * volume_ids 在 evict_inode/free_inode 中释放 (kmalloc).
     *
     * locate 算法 (对齐 FUSE resolve_stripe_chunk, fuse.rs L462):
     *   stripe_unit_idx = offset / stripe_size
     *   chunk_idx_in_unit = (offset % stripe_size) / chunk_size
     *   volume_id = volume_ids[stripe_unit_idx]
     *   needle_id = file_key + chunk_idx_in_unit
     *
     * 注意: 当前实现假设所有 stripe unit 共享同一 base needle_id (file_key).
     * FUSE 端 chunks[stripe_unit_idx].needle_id 作为 base, 这里用 file_key
     * 是因为 Filer CREATE Stripe 响应中各 chunk 的 needle_id 各不相同,
     * 但 file_key 字段未单独携带. 后续若需要 per-unit needle, 切换到
     * chunks[] 数组方式 (K3-5 LIST_CHUNKS). */
    u64 stripe_size;            /* stripe unit 大小 (字节), 默认=layout_chunk_size */
    u32 stripe_count;           /* 条带卷数 */
    u32 start_volume_idx;       /* 起始卷索引 (预留) */
    u64 *volume_ids;            /* volume_ids 数组 (kmalloc), NULL=Flat/Inline */
    u32 volume_ids_count;       /* volume_ids 数组长度 */

    /* === K4: 副本 chunk 列表 (读 failover 使用, 从 GETATTR 0xB5 解析) === */
    struct powerfs_chunk_map *replica_chunks;
    u32 replica_count;

    /* === K4: EC 元数据 (从 0xA1 Reliability 解析) ===
     * ec_data_shards=0 表示非 EC 模式. */
    u32 ec_data_shards;
    u32 ec_parity_shards;

    /* === K4-5: EC shards 列表 (从 ChunkLayout PER_CHUNK 解析) ===
     * EC 文件所有 shards (data+parity), 按 group 连续排列.
     * evict_inode/free_inode 中释放. */
    struct powerfs_chunk_map *ec_chunks;
    u32 ec_chunk_count;

    /* 缓存有效性 */
    bool cache_valid;
    unsigned long cache_expire;
    bool need_refresh;  /* NOTIFY 置位: 需异步 getattr 刷新元数据 */

    /* === 对齐 : truncate 顺序屏障 + btime + xattr version === */
    struct mutex i_truncate_mutex;        /* 与写回/setattr 互斥 */
    u32 i_truncate_seq;                   /* 最近一次截断的序号 ( 同名字段) */
    u64 i_truncate_size_visible;          /* 用户可见 truncate_size (已 vmtruncate 完成) */
    struct timespec64 i_btime;            /* 文件创建时间 (birth/creation time,
                                           * 对齐  inode i_btime / snap_btime) */
    struct timespec64 i_snap_btime;
    u64 i_xattr_version;                  /* xattr 版本号, Filer 返回的最新 */

    /* === 对齐 : 目录递归统计 + 配额 (rbytes / rfiles / rsubdirs / quota) === */
    struct timespec64 i_rctime;
    u64 i_rbytes, i_rfiles, i_rsubdirs, i_rsnaps;
    u64 i_files, i_subdirs;
    u64 i_max_bytes, i_max_files;        /* 目录级 quota (0 = 未设置) */

    /* === 对齐 : 目录 frag tree (大目录分片, 预留) === */
    struct rb_root i_fragtree;
    int i_fragtree_nsplits;
    struct mutex i_fragtree_mutex;

    /* 目录缓存 (保持原有字段 + I_COMPLETE 使用 dir_complete 时同步置 i_flags 位) */
    bool dir_complete;                   /* ← 仍保留, 新旧代码兼容; 等价于 (i_flags & POWERFS_I_COMPLETE) */
    struct list_head dir_entries;
    struct mutex dir_mutex;

    /* === 目录 lease (Phase 1: client-side TTL) ===
     * dir_lease_expire: 目录 lease 过期时间 (jiffies)
     *   - readdir 成功后设为 now + POWERFS_DIR_LEASE_TTL (30s)
     *   - 本目录发生 mkdir/rmdir/create/unlink/rename 时清零
     *   - 过期后 readdir 必须重新拉取
     * dir_lease_epoch: 单调递增, 本地 mutation 时自增, 留给 Phase 3
     *   callback 比对 (callback 携带 epoch, 不匹配说明有过本地修改)
     * Note: dir_lease_epoch 现在直接对齐父目录 inode 的 atomic i_shared_gen. */
    unsigned long dir_lease_expire;
    u64 dir_lease_epoch;
    /* dir_fetch_epoch: 单调递增, 每次 readdir refetch 开始时 ++.
     * 用于清理 stale active dir_entries: refetch 拉取过程中遇到的同名条目
     * 更新 fetch_epoch = dir_fetch_epoch; refetch 结束后所有 fetch_epoch
     * 落后于 dir_fetch_epoch 的 active 条目说明 Filer 不再返回, 标记 deleted. */
    u64 dir_fetch_epoch;

    /* === 对齐 : 未 commit 的 async dirop / iop 链表 (Async DIROPS 核心) === */
    struct list_head i_unsafe_dirops;    /* uncommitted mds dir op 链表 */
    struct list_head i_unsafe_iops;      /* uncommitted mds inode op 链表 */
    spinlock_t i_unsafe_lock;            /* 保护两个 unsafe_* 链表 */

    /* 异步 inode 工作项 (对齐  i_work + i_work_mask).
     * 取代原来单一 setattr_work, 扩展为多工作项位图:
     *   BIT(0) = INODE_SETATTR,
     *   BIT(1) = INODE_INVALIDATE_PAGES,
     *   BIT(2) = INODE_VM_TRUNCATE
     * 原有 setattr_work/setattr_pending 保留为兼容快速路径. */
    struct work_struct i_work;
    unsigned long i_work_mask;
    struct work_struct setattr_work;
    bool setattr_pending;

    /* === writeback 互斥 (防止并发 RMW 数据覆盖) ===
     * powerfs_writepages 是异步的 (queue_work), 返回后异步 work 可能仍在执行.
     * 若 writeback 线程再次调用 powerfs_writepages 处理同一 needle 的不同页面,
     * 两个 RMW 并发会导致后写入的覆盖先写入的数据 (data corruption).
     * wb_mutex 确保同一 inode 的 writeback 串行: writepages 获取, 最后一个
     * batch 的 final_cleanup 释放. wb_batch_count 跟踪待完成 batch 数. */
    struct mutex wb_mutex;
    atomic_t wb_batch_count;

    /* shutdown 标志 (参考 powerfs_inode_is_shutdown) */
    bool shutdown;

    /* === xattr 存储 (simple_xattr in-memory) ===
     * 使用内核 simple_xattr API, xattrs 存储在内存中 (不持久化到 Filer).
     * 支持 user.* / trusted.* / security.* 前缀.
     * 由 i_lock 间接保护 (simple_xattr 内部有自旋锁). */
    struct simple_xattrs xattrs;

#if 0 /* DEAD_CODE — removed per architecture alignment (2026-08-23, audit s6-audit-scope).
     *
     * Original comment: "Phase 1 inode-level per-lock-type MDLock array +
     * GATHER waitqueue, one independent lock per POWERFS_NUM_LOCK_TYPES
     * (AUTH / LINK / DIR / Xattr / FileLock / ...)."
     *
     * Why this is dead code:
     *  1. `powerfs_mdlock_rdlock/wrlock/xlock/unlock` were defined in
     *     powerfs_fs.c but NEVER called from any VFS entry (lookup / open /
     *     create / unlink / setattr / setxattr / readdir / …) — grep
     *     returned zero invocations outside of function bodies.
     *  2. The actual FILE-lock cap negotiation goes through
     *     cap_open_grant_and_issue() → powerfs_net_cap_open_grant()
     *     (MsgType 0x91) talking to the real lock_arbiter.rs that runs
     *     EXCLUSIVELY on the Filer leader — this per-inode C state machine
     *     was a duplicate attempt to replicate the MDS Locker on the
     *     client, which violates the Ceph architecture invariant: lock
     *     arbitration lives only on the server; clients only cache
     *     issued cap bits and react to CapRecallNotify.
     *  3. Leaving these fields multiplied `sizeof(struct powerfs_mdlock)`
     *     (8 locks × large struct with holders/gather/waiting lists)
     *     onto *every* inode in memory — significant wasted memory for
     *     code that never ran.
     *
     * The header `#include "powerfs_lock.h"` above is also being wrapped
     * out. If the MDLock design is ever revisited, note it must live on
     * the Filer side (lock_arbiter.rs) not the client; the client-only
     * equivalent is ClientCap (powerfs-fuse/src/client_cap.rs) which
     * simply tracks {issued_mask, wanted_mask, epoch, token, sn}. */
    struct powerfs_mdlock i_locks[POWERFS_NUM_LOCK_TYPES];
    wait_queue_head_t i_mdlock_wq;   /* GATHER 全局等待队列 (跨锁类型) */
#endif /* DEAD_CODE */
};

/* 获取 inode 扩展结构 */
static inline struct powerfs_inode_info *POWERFS_I(struct inode *inode)
{
    return container_of(inode, struct powerfs_inode_info, netfs.inode);
}

/* K1-5: powerfs_locate_chunk - 统一 chunk 定位 (Flat/Stripe 多卷入口)
 *
 * 按 offset 查找对应的 (volume_id, needle_id):
 *   - chunks 数组存在且 chunk_idx 命中: 多卷查表 (Stripe/K3 路径)
 *   - 否则: Flat 模型, needle_id = file_key + offset / chunk_size,
 *     volume_id = inode->volume_id
 *
 * 调用方持 pi->i_lock 或确保 chunks 数组不被并发释放.
 * 返回 0 成功, -EINVAL inline 文件 (不走 volume 路径) 或 chunk_idx 越界.
 */
int powerfs_locate_chunk(struct powerfs_inode_info *pi, loff_t offset,
                         u64 *volume_id_out, u64 *needle_id_out);

/* K3-1: powerfs_apply_layout_to_inode - 将 FileLayout 解析结果应用到 inode
 *
 * 在持 pi->i_lock 的情况下调用 (或确保 inode 未被并发访问).
 * volume_ids 所有权从 layout 转移到 inode (layout->volume_ids 置 NULL).
 * inline_data 所有权从 layout 转移到 inode (layout->inline_data 置 NULL).
 * 若 inode 已有 volume_ids/inline_data, 先 kfree 旧的再替换 (避免泄漏).
 * 调用方负责在 layout 解析失败时 kfree(layout.volume_ids)、kfree(layout.inline_data)
 * 和 kfree(layout.replica_chunks). */
void powerfs_apply_layout_to_inode(struct powerfs_inode_info *pi,
                                   struct powerfs_file_layout *layout);

/* ========== P3-5: 全局性能计数 (对齐  powerfs_client_metric) ========== */

/*
 * powerfs_metric - 单个操作类型的延迟/吞吐统计.
 *
 * 对齐  powerfs_metric (metric.h L154):
 *   total: 操作次数
 *   size_sum/min/max: 操作大小统计
 *   latency_sum/min/max: 延迟统计 (ns 精度)
 */
struct powerfs_metric {
    spinlock_t lock;
    u64 total;
    u64 size_sum;
    u64 size_min;
    u64 size_max;
    u64 latency_sum;      /* ns */
    u64 latency_min;      /* ns */
    u64 latency_max;      /* ns */
};

/* 操作类型枚举 */
enum powerfs_metric_type {
    POWERFS_METRIC_READ,
    POWERFS_METRIC_WRITE,
    POWERFS_METRIC_METADATA,
    POWERFS_METRIC_MAX,
};

/*
 * powerfs_metrics - 全局性能计数器集合.
 *
 * 对齐  powerfs_client_metric (metric.h L168):
 *   - percpu_counter 用于高频 hit/miss 计数 (无锁, 高性能)
 *   - spinlock 保护的 metric[] 用于延迟/吞吐统计 (低频更新)
 *   - atomic64_t 用于 opened_files/total_inodes 等简单计数
 */
struct powerfs_metrics {
    /* IO 延迟/吞吐 (read/write/metadata) */
    struct powerfs_metric metric[POWERFS_METRIC_MAX];

    /* Dentry lease 命中率 */
    struct percpu_counter d_lease_hit;
    struct percpu_counter d_lease_mis;

    /* Cap 命中率 */
    struct percpu_counter i_caps_hit;
    struct percpu_counter i_caps_mis;

    /* 文件/inode 计数 */
    atomic64_t opened_files;
    atomic64_t total_caps;
    struct percpu_counter opened_inodes;
    struct percpu_counter total_inodes;
};

/* Metrics API */
int powerfs_metrics_init(struct powerfs_metrics *m);
void powerfs_metrics_destroy(struct powerfs_metrics *m);
void powerfs_update_metric(struct powerfs_metric *m, ktime_t start, ktime_t end,
                           unsigned int size, int rc);

static inline void powerfs_update_read_metrics(struct powerfs_metrics *m,
                                                ktime_t start, ktime_t end,
                                                unsigned int size, int rc)
{
    powerfs_update_metric(&m->metric[POWERFS_METRIC_READ], start, end, size, rc);
}

static inline void powerfs_update_write_metrics(struct powerfs_metrics *m,
                                                 ktime_t start, ktime_t end,
                                                 unsigned int size, int rc)
{
    powerfs_update_metric(&m->metric[POWERFS_METRIC_WRITE], start, end, size, rc);
}

static inline void powerfs_update_metadata_metrics(struct powerfs_metrics *m,
                                                    ktime_t start, ktime_t end,
                                                    int rc)
{
    powerfs_update_metric(&m->metric[POWERFS_METRIC_METADATA], start, end, 0, rc);
}

static inline void powerfs_metric_cap_hit(struct powerfs_metrics *m)
{
    percpu_counter_inc(&m->i_caps_hit);
}

static inline void powerfs_metric_cap_mis(struct powerfs_metrics *m)
{
    percpu_counter_inc(&m->i_caps_mis);
}

static inline void powerfs_metric_dentry_hit(struct powerfs_metrics *m)
{
    percpu_counter_inc(&m->d_lease_hit);
}

static inline void powerfs_metric_dentry_mis(struct powerfs_metrics *m)
{
    percpu_counter_inc(&m->d_lease_mis);
}

/* ========== 客户端结构 (参考 powerfs_fs_client / linux-6.17/fs/powerfs/super.h L120-L164) ========== */

struct powerfs_client {
    struct super_block *sb;

    /* 对齐 : mount 状态机 (取代 initialized / shutting_down bool, 避免中间态 race) */
    int mount_state;            /* POWERFS_MOUNT_MOUNTING / MOUNTED / UNMOUNTING / ... */
    bool blocklisted;           /* 被 Master/Filer 拉黑, 需发起 clean reconnect */

    /* Master 服务地址 (唯一配置项, Filer/Volume 通过 Master 动态发现) */
    char master_addr[64];
    u16 master_port;

    /* 证书路径: 来源 mount -o ca_crt/client_crt/client_key.
     * deregister_client 在 kill_sb_super 中通过 client 指针取此路径,
     * 故保存在 client 上而非仅 sbi 上. 空字符串表示未提供, 由 Master
     * 强制模式在 net_handler 层拒绝挂载 (不强制内核层 return EINVAL,
     * 兼容 dev mode 无 CA 的开发环境). */
    char ca_crt[512];
    char client_crt[512];
    char client_key[512];

    /* §13 Cap model: 客户端唯一字符串标识 (ClientId string, TLV FieldId::ClientId = 0x30).
     * 对齐 FUSE MetaShardClient::cap_open_grant 入参 client_id, 服务端 cap_manager
     * 用此键维护 per-client 状态并在 recall 时推送 NOTIFY 到对应 net 连接.
     * 内核态填: "powerfs-kernel-<tgid>" (mount 时生成, 单 mount 唯一). */
    char client_id[64];
    size_t client_id_len;

    /* Master RegisterClient/DeregisterClient: 本 mount 持久字符串 UUID
     * (发送给 Master, 用于黑名单/心跳注册表键). 格式: "pwfs-k-<pid>-<jiffies>"
     * 或类似, 单 mount 唯一, 不需要 crash 持久化. */
    char client_uuid[64];

    /* Master RegisterClient 响应中返回的统一分配 numeric client_id (u64).
     * 0 = 尚未完成注册 (默认). 用于心跳/路由等需要 numeric id 的场景. */
    u64 assigned_client_id;

    /* 通信层 */
    struct powerfs_comm *comm;

    /* 对齐 : per-file handle generation — 每次 revoke 自增,
     * 防止 file private data 被跨 revoke 复用. */
    u32 filp_gen;

    /* 对齐 : 全局最大文件大小 (Filer/Master 告知的全局上限) */
    loff_t max_file_size;

    /* 对齐 : 全局 writeback 统计 + congestion 指示 */
    atomic_long_t writeback_count;
    bool write_congested;

    /* 工作队列 (对齐  inode_wq + cap_wq 分工) */
    struct workqueue_struct *inode_wq;   /* inode 异步工作 (setattr/invalidate_pages/truncate) */
    struct workqueue_struct *cap_wq;     /* cap acquire/release/flush/renew 串行化队列 */

    /* dentry lease 链表 ( s_dentry_lru 对应) */
    struct list_head dentry_lease_list;
    spinlock_t dentry_lease_lock;

    /* cap LRU: 所有 session 的 cap lru_item 链接, shrinker 回收入口 */
    struct list_head cap_lru_list;
    spinlock_t cap_lru_lock;

    /* P2-2: Cap shrinker — 低内存时主动释放 cap + invalidate page cache */
    struct shrinker *cap_shrinker;

    /* 全局 CapFlush 列表 (对齐  mdsc->cap_dirty_lock 级别的 g_list) */
    struct list_head cap_flush_list;
    spinlock_t cap_flush_lock;

    /* P3-5: 全局性能计数 (对齐  powerfs_client_metric) */
    struct powerfs_metrics metrics;

    /* 全局锁 */
    struct mutex mount_mutex;
};

/* P2-2: Cap shrinker init/destroy (powerfs_caps.c) */
int powerfs_cap_shrinker_init(struct powerfs_client *cli);
void powerfs_cap_shrinker_destroy(struct powerfs_client *cli);

/* P2-4: Quiesce 接口 (热迁移 / Filer balancer 调用)
 * 同步脏数据 + 释放 cap + 清除 page cache, 让客户端 "静默".
 * 返回 0 成功, *released 为释放的 inode 数; 负值表示错误. */
int powerfs_quiesce_all(struct super_block *sb, unsigned long *released);

/* ========== 超级块私有信息 (参考 powerfs_fs_client + 挂载层 slab) ========== */

struct powerfs_sb_info {
    struct super_block *sb;
    struct powerfs_client *client;

    /* Master 服务地址 (唯一配置项, Filer/Volume 通过 Master 动态发现) */
    char master_addr[64];
    u16  master_port;
    u16  shard_count;   /* Filer 总分片数, 用于元数据路由 (inode/1M) % shard_count */

    /* 传输层类型: TCP (默认) 或 RDMA (CONFIG_INFINIBAND=y 时可用).
     * 由 mount -o transport=tcp|rdma 传入, fill_super 解析后存此.
     * powerfs_conn_pool_init 读取此字段设置 g_pool.transport_type,
     * conn 初始化时按 g_pool.transport_type 选择 ops. */
    enum powerfs_transport_type transport_type;

    /* 证书路径: fill_super 阶段从 ctx 暂存到此, 后续 powerfs_client 初始化
     * 时再拷贝到 client->ca_crt/client_crt/client_key. 长度 511B + NUL. */
    char ca_crt[512];
    char client_crt[512];
    char client_key[512];

    /* 多组 kmem_cache: 对齐  init_caches() (powerfs_cap_snap_cachep / inode / dentry) */
    struct kmem_cache *inode_cache;       /* powerfs_inode_info (内含 netfs_inode) */
    struct kmem_cache *dentry_cachep;     /* powerfs_dentry_info */
    struct kmem_cache *cap_cachep;        /* powerfs_cap — 每 cap 独立分配 */
    struct kmem_cache *cap_snap_cachep;   /* powerfs_cap_snap — snapshot flush 单元 */
    struct kmem_cache *cap_flush_cachep;  /* powerfs_cap_flush — in-flight flush 记录 */

    /* inode 号分配器 */
    atomic_t next_ino;

    /* 是否初始化完成 (兼容旧代码, 新代码用 client->mount_state) */
    bool initialized;

    /* 标记 powerfs_conn_pool_init 是否成功完成.
     * kill_sb 检查此标志: 仅当 pool 已初始化时才执行
     * powerfs_net_set_stopping + powerfs_net_pool_cleanup.
     * 否则 fill_super 早期失败 (如 missing master_addr) 触发的
     * kill_sb 会无条件清理全局 g_pool, 导致其他活跃 mount 不可用. */
    bool pool_initialized;

    /* Phase 3: 卸载标志. kill_sb_super 设置为 true, lease_renew_work_func
     * 检查此标志避免在 destroy_workqueue 期间重新排队导致 flush 循环.
     * (兼容旧代码; 新代码判 client->mount_state == UNMOUNTING/SHUTDOWN) */
    bool shutting_down;

    /* Stage C: writeback 异步 workqueue.
     * writepage 提交异步写请求到此 workqueue, 避免在 writeback
     * 上下文同步等待网络. fill_super 创建, kill_sb 销毁.
     * (等价于  把 writeback 提交到 inode_wq 上下文, 但 PowerFS 数据走 Volume
     * 分离路径, 所以保留独立 writeback_wq) */
    struct workqueue_struct *writeback_wq;

    /* Phase 3: lease 续约 workqueue (delayed_work 调度).
     * NOTE: 现有 lease_wq 逻辑已被上面的 client->cap_wq 语义吸收,
     * 保留指针为了兼容旧版 delayed_work 调度代码不被 break. */
    struct workqueue_struct *lease_wq;

    /* writepages 批量大小 (页数), 由 mount option write_batch_kb 决定.
     * 单次 work item 最多收集这么多脏页, 减少网络往返和 work item 数量.
     * 默认 16 (64KB), ROCE 可设为 256 (1MB) ~ 16384 (64MB stripe). */
    int write_batch_pages;

    /* writeback 并发限制: 防止过多 work item 同时阻塞在网络 I/O,
     * 导致内存回收停滞 (mm_percpu_wq lockup).
     * 每个 work item 占用 2MB needle_buf, 限制为 2 个并发 = 最多 4MB. */
    atomic_t wb_in_flight;
#define POWERFS_WB_MAX_IN_FLIGHT  2

    /* P3-4: debugfs 根目录 (/sys/kernel/debug/powerfs/<sb_id>/) */
    struct dentry *debugfs_dir;

    /* P3-5: /proc/powerfs/<sb_id>/ 入口 */
    struct proc_dir_entry *proc_dir;
};

#define POWERFS_SB_INFO(sb) ((struct powerfs_sb_info *)(sb)->s_fs_info)

/* ========== 通信层接口 (powerfs_transport.c) ========== */

int powerfs_comm_init(void);
void powerfs_comm_exit(void);

struct powerfs_msg_header;

int powerfs_comm_send_request(struct powerfs_msg_header *req_hdr,
                               void *req_data,
                               struct powerfs_msg_header *resp_hdr,
                               void *resp_data,
                               int timeout_ms);

int powerfs_comm_submit_notify(struct powerfs_msg_header *req_hdr,
                               void *req_data);

/* ========== 文件系统操作 (powerfs_fs.c) ========== */

/* super_operations */
struct inode *powerfs_alloc_inode(struct super_block *sb);
void powerfs_free_inode(struct inode *inode);
void powerfs_evict_inode(struct inode *inode);
int  powerfs_statfs(struct dentry *dentry, struct kstatfs *buf);

/* dentry_operations */
int  powerfs_d_revalidate(struct inode *dir, const struct qstr *name,
                          struct dentry *dentry, unsigned int flags);
int  powerfs_d_init(struct dentry *dentry);
void powerfs_d_release(struct dentry *dentry);
void powerfs_d_prune(struct dentry *dentry);

/* 模块级初始化/清理 */
int  powerfs_init_inode_cache(void);
void powerfs_destroy_inode_cache(void);
int  powerfs_fill_super(struct super_block *sb, struct fs_context *fc);
void powerfs_kill_sb_super(struct super_block *sb);
void powerfs_set_sb_dentry_ops(struct super_block *sb);

/* 辅助函数 */
struct inode *powerfs_create_root(struct super_block *sb);

/* readdir (目录文件操作) */
int powerfs_dir_open(struct inode *inode, struct file *file);
int powerfs_dir_release(struct inode *inode, struct file *file);
int powerfs_readdir(struct file *file, struct dir_context *ctx);

/* 地址空间操作 (page cache) */
int powerfs_writepage(struct page *page, struct writeback_control *wbc);
int powerfs_writepages(struct address_space *mapping, struct writeback_control *wbc);
int powerfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
                        loff_t pos, unsigned int len, struct folio **foliop,
                        void **fsdata);
int powerfs_write_end(const struct kiocb *iocb, struct address_space *mapping,
                      loff_t pos, unsigned int len, unsigned int copied,
                      struct folio *folio, void *fsdata);
sector_t powerfs_bmap(struct address_space *mapping, sector_t block);

/* inode 管理 (参考 powerfs iget5_locked/ilookup5 机制) */
struct inode *powerfs_iget(struct super_block *sb, u64 ino);
struct inode *powerfs_find_inode(struct super_block *sb, u64 ino);
struct inode *powerfs_new_inode(struct super_block *sb, umode_t mode,
                                u64 ino, u64 parent_ino, const char *name);
struct dentry *powerfs_lookup(struct inode *dir, struct dentry *dentry,
                               unsigned int flags);
int powerfs_init_inode(struct inode *inode, umode_t mode,
                       u64 parent_ino, const char *name);

/* setattr/rename 操作 */
int powerfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                    struct iattr *attr);
int powerfs_rename(struct mnt_idmap *idmap,
                   struct inode *old_dir, struct dentry *old_dentry,
                   struct inode *new_dir, struct dentry *new_dentry,
                   unsigned int flags);

/* readlink 操作 */
int powerfs_readlink(struct dentry *dentry, char *buffer, int buflen);

/* 全局超级块访问 (powerfs_fs.c) */
struct super_block *powerfs_get_sb(void);

/* inode 查找 (powerfs_fs.c) */
struct inode *powerfs_find_inode(struct super_block *sb, u64 ino);

/* Invalidate one inode's page cache and attribute cache.
 * Called from powerfs_net.c when a NOTIFY frame is received from Filer.
 * Returns 0 on success, -ENOENT if inode not in cache, negative on error. */
int powerfs_invalidate_one(u64 ino);

/* Dentry-level invalidation: drop the dentry identified by (parent_ino, name)
 * from the VFS dcache and optionally refresh its inode metadata. Also updates
 * the dir_entries of the parent directory to remove stale entries.
 * Called from powerfs_net.c RX dispatcher when a NOTIFY frame carries
 * the ParentIno + Name TLV fields in addition to (or instead of) Ino.
 *
 * (parent,name,version) dedup is performed inside: replays/network retransmits
 * with the same (or older) version are ignored. */
int powerfs_invalidate_dentry(u64 parent_ino, const char *name, size_t name_len, u64 version);

/* Destroy (free) all entries in the dentry notify dedup ring. Called from
 * module_exit to leak all name strings. */
void powerfs_dentry_dedup_destroy_all(void);

/* invalidate 通知处理 (powerfs_transport.c) */
struct powerfs_invalidate_req;  /* 前向声明 */
int powerfs_handle_invalidate(struct powerfs_invalidate_req *req);

/* 通信层全局状态 */
bool powerfs_comm_is_connected(void);

/* 便捷方法 (powerfs_transport.c) - 与 powerfs_fs.c 中静态版本不冲突的接口 */
int powerfs_comm_read(struct inode *inode, loff_t offset, size_t length,
                       __u8 *buf, size_t *read_len);
int powerfs_comm_write(struct inode *inode, loff_t offset,
                        const __u8 *data, size_t length, size_t *written);
int powerfs_comm_readdir(struct inode *dir, __u64 offset, __u32 count,
                          struct powerfs_dirent *entries,
                          __u32 *actual_count);
int powerfs_comm_readlink(struct inode *inode, char *target, size_t buflen);
int powerfs_comm_statfs(struct kstatfs *stats);

/* ========== Capability 管理接口 (powerfs_fs.c) ==========
 *
 * 对齐  caps.c 客户端 cap 生命周期:
 *   - issued/implemented 双轨: grant 更新 issued, revoke 先降 implemented,
 *     dirty flush 完成后才 implemented=issued (优雅降级, 避免丢数据)
 *   - wanted 由 open 模式 + refcount 派生, 与 issued 不匹配时主动 AcquireCap
 *   - dirty_caps 标记本地脏位, revoke/flush 时写回 Filer
 *   - i_cap_wq 等待 flush ACK, 保证 revoke 期间数据一致
 *
 * 锁约定: 所有 cap rbtree / issued / implemented / refcount / dirty_caps
 * 字段的访问必须在 pi->i_lock 保护下 (对齐  i_powerfs_lock).
 */

/* cap 有效性检查 (cap_gen 匹配 + 未过期).
 * 调用方持 pi->i_lock. */
bool powerfs_cap_is_valid(struct powerfs_cap *cap);

/* 遍历 i_caps rbtree, 返回所有有效 cap 的 issued 并集.
 * @implemented 非 NULL 时同时返回 implemented 并集.
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_issued(struct powerfs_inode_info *pi,
                                 unsigned int *implemented);

/* 检查 mask 是否被 issued 完全覆盖.
 * @touch=true 时把命中的 cap 移到 LRU 尾部.
 * 返回 1 = 满足, 0 = 不满足. 调用方持 pi->i_lock. */
int powerfs_caps_issued_mask(struct powerfs_inode_info *pi,
                             unsigned int mask, int touch);

/* 从 refcount 派生 used caps (PIN/RD/CACHE/WR/EXCL).
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_used(struct powerfs_inode_info *pi);

/* 从 open 模式 + 时间窗口派生 file_wanted caps.
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_file_wanted(struct powerfs_inode_info *pi);

/* wanted = file_wanted | used, 文件脏数据时追加 EXCL.
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_wanted(struct powerfs_inode_info *pi);

/* 引用计数获取 — 调用方不持锁, 内部加 i_lock.
 * @got 指明要取哪些 cap 的引用 (PIN/RD/CACHE/WR/EXCL 位掩码). */
void powerfs_cap_get_refs(struct powerfs_inode_info *pi, unsigned int got);

/* 引用计数释放 — 调用方不持锁, 内部加 i_lock.
 * @had 指明释放哪些 cap 引用 (与 get_refs 对称).
 * 最后一个引用释放时触发 check_caps (评估是否可归还 cap). */
void powerfs_cap_put_refs(struct powerfs_inode_info *pi, unsigned int had);

/* Filer 授予 cap (grant 消息处理).
 * 更新 issued / implemented, 处理 FILE_SHARED 变化 (i_shared_gen / I_COMPLETE).
 * 调用方持 pi->i_lock. */
void powerfs_cap_issue(struct powerfs_inode_info *pi, struct powerfs_cap *cap,
                       unsigned int issued);

/* 服务端撤回 cap (revoke 消息处理).
 * 降级 issued; 若 dirty_caps 与被撤位有交集, 先 flush 再 ack;
 * flush 完成后 implemented = issued (降级生效).
 * 调用方持 pi->i_lock (内部会临时释放以发送 flush RPC). */
void powerfs_cap_revoke(struct powerfs_inode_info *pi, struct powerfs_cap *cap,
                        unsigned int revoking);

/* 写回 dirty_caps 并等待 ACK.
 * 构造 CapFlush 消息发送到 Filer, 将 dirty_caps 移到 flushing_caps,
 * 等待 i_cap_wq 唤醒.
 * 调用方不持锁. */
int powerfs_cap_flush(struct powerfs_inode_info *pi, unsigned int mask);

/* 评估并可能发送 cap 状态更新 (对齐 powerfs_check_caps).
 * 比较 wanted vs issued, 决定是否 AcquireCap / ReleaseCap.
 * @flags: POWERFS_CHECK_CAPS_* 位掩码.
 * 调用方不持锁. */
#define POWERFS_CHECK_CAPS_FLUSH    (1 << 0)
#define POWERFS_CHECK_CAPS_AUTHONLY (1 << 1)
void powerfs_check_caps(struct powerfs_inode_info *pi, int flags);

/* 标记 cap dirty 位 (write/setattr 路径调用, 对齐 __powerfs_mark_caps_dirty).
 * 调用方不持锁. */
void powerfs_cap_mark_dirty(struct powerfs_inode_info *pi, unsigned int caps);

/* 非阻塞尝试获取 cap 引用 (对齐 powerfs_try_get_caps).
 * 若已持有 >= need 的 issued 位, 调 cap_get_refs 递增引用并返回 true;
 * 否则触发 check_caps(0) 向服务端申请, 返回 false (调用方决定降级或重试).
 * need 只允许 POWERFS_CAP_FILE_SHARED (纯读必须位), want 是推荐位掩码.
 * @got: 非 NULL 时返回实际拿到的 cap 引用位 (供 put_refs 对称释放).
 * 调用方不持锁. */
bool powerfs_try_get_caps(struct powerfs_inode_info *pi,
                          unsigned int need, unsigned int want,
                          bool nonblock, unsigned int *got);

/* 阻塞等待 cap 授权 (对齐 powerfs_get_caps).
 * 循环调 try_get_cap_refs, 不满足时挂在 i_cap_wq 上等待服务端 cap_issue 唤醒.
 * @filp: 打开的文件描述符 (可 NULL, 如 write_begin 从 iocb 取不到时).
 * @endoff: 写路径的写入末尾偏移 (0 表示不限制).
 * @got: 输出实际拿到的引用位.
 * 返回 0 成功, <0 错误 (如 -ERESTARTSYS/-ENOTCONN).
 * 调用方不持锁, 可阻塞. */
int powerfs_get_caps(struct inode *inode, struct file *filp,
                     unsigned int need, unsigned int want,
                     loff_t endoff, unsigned int *got);

/* inode permission 鉴权 — 对齐 powerfs_permission.
 * 先拿 AUTH_SHARED cap 保证 inode attrs 最新, 再调 generic_permission.
 * 返回 0 允许访问, <0 错误码 (-EACCES/-ECHILD). */
int powerfs_permission(struct mnt_idmap *idmap, struct inode *inode, int mask);

/* powerfs-net 初始化/清理 (powerfs_net.c) */
int  powerfs_net_init(void);
void powerfs_net_exit(void);

#endif /* _POWERFS_H */
