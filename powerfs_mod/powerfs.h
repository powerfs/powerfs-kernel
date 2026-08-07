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

/* ========== 常量定义 ========== */

#define POWERFS_SUPER_MAGIC     0x50574552  /* "POWE" */
#define POWERFS_ROOT_INO        1
#define POWERFS_INO_START       2
#define POWERFS_MAX_NAME_LEN    255
#define POWERFS_VERSION         "2.0.0-ceph-style"

/* Dentry lease 超时 (jiffies, 默认 5 秒) */
#define POWERFS_DENTRY_LEASE_TTL    (5 * HZ)

/* Inode cache 超时 (jiffies, 默认 10 秒) */
#define POWERFS_INODE_CACHE_TTL     (10 * HZ)

/* 目录 lease 超时 (jiffies, 默认 30 秒).
 * readdir 成功后 30s 内重复 readdir 走本地缓存, 不发网络.
 * 本地 mutation (mkdir/rmdir/create/unlink/rename 等) 主动清零. */
#define POWERFS_DIR_LEASE_TTL       (30 * HZ)

/* Chunk 和 Stripe 大小 */
#define POWERFS_CHUNK_SIZE      (2 * 1024 * 1024)    /* 2MB */
#define POWERFS_STRIPE_SIZE     (64 * 1024 * 1024)   /* 64MB */
#define POWERFS_LEASE_DURATION  (30 * HZ)
/* Phase 3: lease 续约阈值. 当 lease 剩余有效期 < 阈值时触发续约.
 * 设为 DURATION/3 (10s): 在过期前 10s 续约, 留足网络往返 + 重试时间.
 * 参考 Ceph cap renew 在 expiry 前主动续约. */
#define POWERFS_LEASE_RENEW_THRESHOLD (POWERFS_LEASE_DURATION / 3)

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

/* ========== Dentry 私有数据 (参考 ceph_dentry_info) ========== */

struct powerfs_dentry_info {
    struct dentry *dentry;            /* 所属 dentry */
    struct list_head lease_list;      /* 租约链表节点 */
    unsigned long lease_expire;       /* 租约过期时间 (jiffies) */
    unsigned long time;               /* 最近更新时间 */
    u64 offset;                       /* readdir 偏移 */
    unsigned long flags;              /* 标志位 */
    struct rcu_head rcu;              /* RCU 延迟释放 (d_fsdata 不能裸 kmem_cache_free) */
};

/* powerfs_dentry_info 标志位 */
#define POWERFS_DENTRY_COMPLETE   0  /* 目录缓存完整 */

/* 获取 dentry 私有数据 */
static inline struct powerfs_dentry_info *POWERFS_D(struct dentry *dentry)
{
    return dentry->d_fsdata;
}

/* ========== 目录文件私有数据 (参考 ceph_dir_file_info) ========== */

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
    char name[POWERFS_MAX_NAME_LEN]; /* 文件名 */
};

/* ========== Inode 扩展结构 (参考 ceph_inode_info) ========== */

/* Chunk 映射条目 */
struct powerfs_chunk_map {
    u32 chunk_idx;
    u64 needle_id;
    u64 volume_id;
    u32 crc32;
};

/* Per-stripe Lease (参考 ceph_cap) */
struct powerfs_lease {
    struct rb_node node;
    u64 stripe_start;
    u64 stripe_count;
    char token[64];
    u64 epoch;
    unsigned long expire_jiffies;
    bool exclusive;
    u64 content_size;
};

struct powerfs_inode_info {
    struct netfs_inode netfs;          /* 内含 struct inode，必须第一个字段 */

    u64 parent_ino;
    char name[POWERFS_MAX_NAME_LEN];

    spinlock_t i_lock;

    /* Lease 强一致 (参考 ceph i_caps rbtree) */
    struct rb_root lease_tree;         /* per-stripe lease，按 stripe_start 排序 */
    spinlock_t lease_lock;             /* 保护 lease_tree */
    struct delayed_work lease_renew_work;

    /* Chunk 映射 (open/getattr 时从 Filer 获取) */
    struct powerfs_chunk_map *chunks;
    u32 chunk_count;
    u64 content_size;
    u64 volume_id;
    u64 file_key;   /* needle_id (base, chunk N = file_key + N), from Filer Create or GetAttr */

    /* 缓存有效性 */
    bool cache_valid;
    unsigned long cache_expire;
    bool need_refresh;  /* NOTIFY 置位: 需异步 getattr 刷新元数据 */

    /* 目录缓存 */
    bool dir_complete;
    struct list_head dir_entries;
    struct mutex dir_mutex;

    /* === 目录 lease (Phase 1: client-side TTL) ===
     * dir_lease_expire: 目录 lease 过期时间 (jiffies)
     *   - readdir 成功后设为 now + POWERFS_DIR_LEASE_TTL (30s)
     *   - 本目录发生 mkdir/rmdir/create/unlink/rename 时清零
     *   - 过期后 readdir 必须重新拉取
     * dir_lease_epoch: 单调递增, 本地 mutation 时自增, 留给 Phase 3
     *   callback 比对 (callback 携带 epoch, 不匹配说明有过本地修改) */
    unsigned long dir_lease_expire;
    u64 dir_lease_epoch;

    /* shutdown 标志 (参考 ceph_inode_is_shutdown) */
    bool shutdown;
};

/* 获取 inode 扩展结构 */
static inline struct powerfs_inode_info *POWERFS_I(struct inode *inode)
{
    return container_of(inode, struct powerfs_inode_info, netfs.inode);
}

/* ========== 客户端结构 (参考 ceph_fs_client) ========== */

struct powerfs_client {
    struct super_block *sb;
    
    /* Master 服务地址 */
    char master_addr[64];
    u16 master_port;
    
    /* Volume 服务地址 */
    char volume_addr[64];
    u16 volume_port;
    
    /* Filer 服务地址 */
    char filer_addr[64];
    u16 filer_port;
    
    /* 通信层 */
    struct powerfs_comm *comm;
    
    /* 工作队列 */
    struct workqueue_struct *inode_wq;
    
    /* dentry lease 链表 */
    struct list_head dentry_lease_list;
    spinlock_t dentry_lease_lock;
    
    /* 全局锁 */
    struct mutex mount_mutex;
};

/* ========== 超级块私有信息 (参考 ceph_fs_client) ========== */

struct powerfs_sb_info {
    struct super_block *sb;
    struct powerfs_client *client;

    /* Master 服务地址 */
    char master_addr[64];
    u16  master_port;

    /* Volume 服务地址 */
    char volume_addr[64];
    u16  volume_port;

    /* Filer 服务地址 */
    char filer_addr[64];
    u16  filer_port;

    /* inode slab cache */
    struct kmem_cache *inode_cache;

    /* dentry_info slab cache */
    struct kmem_cache *dentry_cachep;

    /* inode 号分配器 */
    atomic_t next_ino;

    /* 是否初始化完成 */
    bool initialized;

    /* Phase 3: 卸载标志. kill_sb_super 设置为 true, lease_renew_work_func
     * 检查此标志避免在 destroy_workqueue 期间重新排队导致 flush 循环. */
    bool shutting_down;

    /* Stage C: writeback 异步 workqueue.
     * writepage 提交异步写请求到此 workqueue, 避免在 writeback
     * 上下文同步等待网络. fill_super 创建, kill_sb 销毁. */
    struct workqueue_struct *writeback_wq;

    /* Phase 3: lease 续约 workqueue (delayed_work 调度).
     * WQ_HIGHPRI: lease 续约延迟敏感 (过期即丢锁导致写失败), 参考 GFS2
     *   glock_workqueue (fs/gfs2/glock.c:2462).
     * WQ_MEM_RECLAIM: writeback 路径内存回收安全.
     * WQ_UNBOUND: 不绑 CPU, 提升扩展性.
     * 不使用 max_active=1 串行: lease 按 stripe 独立, 无全局顺序依赖,
     *   不同 inode 的续约可并发 (参考 GFS2 glock max_active=0). */
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
void powerfs_set_dentry_lease(struct dentry *dentry, unsigned long ttl);
bool powerfs_dentry_lease_valid(struct dentry *dentry);

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

/* inode 管理 (参考 ceph iget5_locked/ilookup5 机制) */
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

/* powerfs-net 初始化/清理 (powerfs_net.c) */
int  powerfs_net_init(void);
void powerfs_net_exit(void);

#endif /* _POWERFS_H */
