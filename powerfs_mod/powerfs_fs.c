/*
 * PowerFS 内核文件系统 - VFS 文件系统操作实现
 *
 * 参考:
 *   - ramfs (fs/ramfs/inode.c) - 纯内存 VFS 缓存使用范例
 *
 * 设计原则:
 *   - 完全依赖 VFS dcache 和 page cache，不重复造轮子
 *   - inode 生命周期由 VFS 管理（引用计数 + LRU + shrinker）
 *   - dentry 通过 d_revalidate 做缓存有效性验证
 *   - 数据存储在 page cache 中，使用 ram_aops
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/dcache.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/time.h>
#include <linux/atomic.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/list_lru.h>
#include <linux/backing-dev.h>
#include <linux/ramfs.h>
#include <linux/writeback.h>     /* folio_mark_dirty, writeback_control */
#include <linux/mnt_idmapping.h> /* mnt_idmap, nop_mnt_idmap (6.5+) */
#include <linux/pagevec.h>       /* pagevec_lookup_range_tag (writepages 批量遍历) */
#include <linux/delay.h>        /* msleep (release 重试退避) */
#include <linux/xattr.h>        /* simple_xattr API for xattr support */
#include <linux/falloc.h>       /* FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE */
#include <linux/iversion.h>     /* inode_inc_iversion_raw (page_mkwrite) */
#include <linux/filelock.h>     /* posix_lock_file / locks_lock_inode_wait / FL_FLOCK / FL_POSIX */
#include <linux/posix_acl_xattr.h>  /* posix_acl_from_xattr / posix_acl_to_xattr (序列化 xattr<->acl) */
#include <linux/migrate.h>      /* filemap_migrate_folio (页迁移, P1-4) */
#include <linux/fs.h>           /* FS_IOC_* ioctl 编号 (P1-3) */
#include <linux/fileattr.h>     /* ioctl_getflags/ioctl_setflags/ioctl_fsgetxattr/ioctl_fssetxattr (P1-3, Linux 6.17+) */
#include <linux/exportfs.h>     /* export_operations (P2-8 NFS export) */
#include <linux/splice.h>       /* splice_copy_file_range (P2-5) */
#include <linux/utsname.h>      /* init_utsname() 主机名 */
#include <linux/jiffies.h>      /* jiffies_64 时间戳 */

#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_net.h"
#include "powerfs_flow.h"

/* ========== netfs 请求操作 (Stage C: 对接 netfs 子系统) ==========
 *
 * 参照 fs/xxx/addr.c 的 xxx_netfs_issue_read 实现.
 *
 * read 路径改造:
 *   - powerfs_aops.read_folio = netfs_read_folio (netfs 提供)
 *   - powerfs_aops.readahead  = netfs_readahead  (netfs 提供)
 *   - powerfs_netfs_ops.issue_read = powerfs_netfs_issue_read (实际网络读取)
 *
 * netfs 子系统管理 folio 生命周期 (锁定/解锁/uptodate), filesystem 只需
 * 在 issue_read 中填充数据并调用 netfs_subreq_terminated 完成.
 *
 * 基本功能阶段: issue_read 中同步调用 powerfs_net_read, 简单但会阻塞
 * 当前进程. 后续可改为异步 (提交网络请求 -> 回调 netfs_subreq_terminated).
 */

static void powerfs_netfs_issue_read(struct netfs_io_subrequest *subreq)
{
    struct netfs_io_request *rreq = subreq->rreq;
    struct inode *inode = rreq->inode;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    size_t len = subreq->len;
    loff_t start = subreq->start;
    struct iov_iter iter;
    void *buf;
    __u32 read_len = 0;
    int err;

    pr_debug("powerfs: netfs_issue_read ino=%lu start=%llu len=%zu i_size=%llu\n",
            inode->i_ino, (unsigned long long)start, len,
            (unsigned long long)rreq->i_size);

    /* 超出文件大小的部分: 由 netfs 处理 (设置 NETFS_SREQ_CLEAR_TAIL),
     * issue_read 只读取有效数据部分 */
    if (start >= rreq->i_size) {
        pr_debug("powerfs: issue_read start >= i_size, skip (start=%llu i_size=%llu)\n",
                (unsigned long long)start, (unsigned long long)rreq->i_size);
        netfs_read_subreq_terminated(subreq);
        return;
    }
    if (start + len > rreq->i_size)
        len = rreq->i_size - start;

    /* K2: Inline 模式 — 直接从 inline_data 读取, 不走 Volume Server RPC.
     * inline_data 由 i_lock 保护, 持锁期间直接通过 iov_iter 拷贝到 page cache.
     *
     * 注意: netfs_issue_read 在 readahead 路径中调用, preempt 可能被禁用
     * (read_pages → page_cache_ra_unbounded 持 xa_lock). 不能使用 GFP_KERNEL
     * 分配临时缓冲. 直接用 copy_to_iter 从 inline_data 拷贝到 xarray folio.
     * x86-64 上 kmap_local_folio 是 page_address (不睡眠), 持 spinlock 安全. */
    if (pi->placement == POWERFS_PLACEMENT_INLINE) {
        u8 *src;
        u32 src_len;
        size_t copy_len;

        spin_lock(&pi->i_lock);
        src = pi->inline_data;
        src_len = pi->inline_len;
        if (!src || src_len == 0) {
            /* Inline 文件无 inline_data: 新建文件尚未写入, 或 GETATTR 未携带.
             * 不回退到 Volume 路径 (Inline 文件无 needle, locate_chunk 返回 -EINVAL).
             * 返回 0 字节 + HIT_EOF: netfs 将 folio 填零并标记 uptodate,
             * write_begin 正常进行. 对齐 FUSE: read-before-write 对空文件返回 0.
             * HIT_EOF 必须设置, 否则 netfs_read_collect 将 short read 转为 -ENODATA. */
            spin_unlock(&pi->i_lock);
            pr_debug("powerfs: issue_read INLINE ino=%lu no inline_data, return 0 (HIT_EOF)\n",
                    inode->i_ino);
            subreq->transferred = 0;
            __set_bit(NETFS_SREQ_HIT_EOF, &subreq->flags);
            netfs_read_subreq_terminated(subreq);
            return;
        }

        /* 计算可拷贝长度: 从 start 开始, 不超过 inline_len, 不超过请求 len */
        if (start >= src_len) {
            /* 请求超出 inline 数据范围, 返回 0 字节 (EOF) */
            spin_unlock(&pi->i_lock);
            pr_debug("powerfs: issue_read INLINE ino=%lu start=%llu >= inline_len=%u, EOF\n",
                    inode->i_ino, (unsigned long long)start, src_len);
            subreq->transferred = 0;
            __set_bit(NETFS_SREQ_HIT_EOF, &subreq->flags);
            netfs_read_subreq_terminated(subreq);
            return;
        }
        copy_len = min_t(size_t, len, src_len - start);

        /* 直接从 inline_data 拷贝到 page cache (xarray folio).
         * 持 i_lock 防止 inline_data 被并发释放/修改.
         * copy_to_iter 内部 kmap_local_folio 在 x86-64 上不睡眠. */
        if (copy_len > 0) {
            iov_iter_xarray(&iter, ITER_DEST, &rreq->mapping->i_pages,
                            start, copy_len);
            copy_to_iter(src + start, copy_len, &iter);
        }

        /* K2: 调试 — 在锁内计算 checksum 和前 8 字节, 避免锁外
         * src 指针被 GETATTR 并发释放导致 use-after-free. */
        {
            __u32 i, csum = 0;
            __u8 b[8] = {0};
            __u8 *p = src + start;
            for (i = 0; i < copy_len && i < src_len - start; i++) {
                csum += p[i];
                if (i < 8)
                    b[i] = p[i];
            }
            spin_unlock(&pi->i_lock);

            subreq->transferred = copy_len;
            /* 部分读取 (copy_len < len): inline 数据不足请求长度, 即 EOF.
             * 必须设置 HIT_EOF, 否则 netfs_read_collect 将 short read 转为 -ENODATA. */
            if (copy_len < len)
                __set_bit(NETFS_SREQ_HIT_EOF, &subreq->flags);
            pr_info("powerfs: issue_read INLINE ino=%lu start=%llu copy=%zu len=%u csum=%u first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    inode->i_ino, (unsigned long long)start, copy_len, src_len, csum,
                    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        }
        netfs_read_subreq_terminated(subreq);
        return;
    }

    /* K3: powerfs_net_read 内部按 offset 调用 powerfs_locate_chunk 定位
     * (volume_id, needle_id), 统一支持 Flat/Stripe/WideStripe. */

    /* 新建 Flat 文件尚无 chunks (无 volume_id, file_key, chunks 数组):
     * GETATTR 可能在 create 后返回 placement=Flat 但尚未分配 chunks.
     * 此时不走 powerfs_net_read (locate_chunk 会返回 -EINVAL),
     * 返回 0 字节 + HIT_EOF, 让 write_begin 正常进行 (read-before-write 无数据可读). */
    if (pi->placement != POWERFS_PLACEMENT_INLINE &&
        !pi->volume_id && !pi->file_key &&
        !pi->chunks && !pi->volume_ids) {
        pr_debug("powerfs: issue_read FLAT ino=%lu no chunks yet, return 0 (HIT_EOF)\n",
                inode->i_ino);
        subreq->transferred = 0;
        __set_bit(NETFS_SREQ_HIT_EOF, &subreq->flags);
        netfs_read_subreq_terminated(subreq);
        return;
    }

    /* 基本功能阶段: 同步读取到临时 buffer, 再拷贝到 xarray 中的 folio.
     * 后续优化: 直接从 xarray 映射 folio, 避免额外拷贝 (参照 xxx).
     * GFP_NOFS: 避免 FS 回调递归 (netfs readahead 上下文). */
    buf = kvmalloc(len, GFP_NOFS);
    if (!buf) {
        subreq->error = -ENOMEM;
        netfs_read_subreq_terminated(subreq);
        return;
    }

    err = powerfs_net_read(pi, inode->i_ino, start, len, buf, len, &read_len);
    {
        __u8 *b = (__u8 *)buf;
        pr_debug("powerfs: issue_read powerfs_net_read ret=%d read_len=%u buf[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                err, read_len,
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    if (err) {
        pr_warn("powerfs: netfs_issue_read ino=%lu start=%llu len=%zu failed: %d\n",
                inode->i_ino, (unsigned long long)start, len, err);
        kvfree(buf);
        subreq->error = err;
        netfs_read_subreq_terminated(subreq);
        return;
    }

    /* 拷贝到 xarray 中的 folio (netfs 已预先分配并锁定 folio) */
    if (read_len > 0) {
        iov_iter_xarray(&iter, ITER_DEST, &rreq->mapping->i_pages,
                        start, read_len);
        copy_to_iter(buf, read_len, &iter);
        pr_debug("powerfs: issue_read copied %u bytes to folio\n", read_len);
    }

    kvfree(buf);
    subreq->transferred = read_len;
    /* 部分读取 (read_len < len): volume 数据不足请求长度, 即 EOF.
     * 必须设置 HIT_EOF, 否则 netfs_read_collect 将 short read 转为 -ENODATA. */
    if (read_len < len)
        __set_bit(NETFS_SREQ_HIT_EOF, &subreq->flags);
    netfs_read_subreq_terminated(subreq);
}

static const struct netfs_request_ops powerfs_netfs_ops = {
    .issue_read = powerfs_netfs_issue_read,
};

/* ========== 全局 slab 缓存 (参考 xxx 全局 cache) ========== */

static struct kmem_cache *powerfs_inode_cachep;
static struct kmem_cache *powerfs_dentry_cachep;

/* 全局超级块指针 (用于跨模块访问) */
static struct super_block *g_powerfs_sb;

/* ========== 异步 inode 刷新工作队列 ==========
 * 用于 powerfs_invalidate_one: 从 NOTIFY 回调中异步刷新 inode 元数据.
 * 独立于通信层调度器线程, 避免 self-deadlock (调度器等待自己处理的响应). */
struct powerfs_refresh_work {
    struct work_struct work;
    u64 ino;
    struct inode *inode;  /* igrab 引用, work 完成后 iput */
    struct rcu_head rcu;  /* RCU 延迟释放 (workqueue 在 work_fn 返回后仍引用 work_struct) */
};
static struct workqueue_struct *powerfs_refresh_wq;

/* ========== 异步 Cap Notify 处理 (服务端→客户端推送) ==========
 * CapRecallNotify / CapUpgradeNotify 由 RX dispatcher 收到后,
 * 异步排队到 powerfs_refresh_wq 处理, 避免阻塞 RX 调度线程:
 *   - ilookup5 查找 inode 可能阻塞 (等待 I_FREEING 状态)
 *   - CapRecall → powerfs_cap_revoke → cap_flush + recall_ack 需同步网络 I/O
 *
 * 设计: 用单一 cap_notify_work 结构 + kind 标签区分 recall/upgrade,
 * body 通过匿名 union 内嵌对应字段 (token/recall_mask/retain_mask/epoch/sn). */
enum powerfs_cap_notify_kind {
    CAP_NOTIFY_RECALL = 1,
    CAP_NOTIFY_UPGRADE = 2,
};
struct powerfs_cap_notify_work {
    struct work_struct work;
    struct rcu_head rcu;
    enum powerfs_cap_notify_kind kind;
    u64 ino;
    char lease_token[64];
    size_t token_len;
    union {
        struct {
            __u8 recall_mask;
            __u8 retain_mask;
            __u64 epoch;
        } recall;
        struct {
            __u8 new_granted;
            __u64 epoch;
            __u64 sn;
        } upgrade;
    } body;
};

/* RCU 延迟释放 cap_notify_work (work_struct 被 workqueue core 引用到返回后). */
static void powerfs_cap_notify_work_free_rcu(struct rcu_head *head)
{
    struct powerfs_cap_notify_work *w =
        container_of(head, struct powerfs_cap_notify_work, rcu);
    kfree(w);
}

/* 在 pi->i_caps rbtree 中按 lease_token 查找匹配 cap (服务端 recall/upgrade
 * 推送总是携带最初 grant 返回的 token, 多 issuer 场景据此区分不同 cap).
 * 调用方持 pi->i_lock. 返回匹配的 cap 或 NULL (找不到时退回 i_auth_cap). */
static struct powerfs_cap *
find_cap_by_token_locked(struct powerfs_inode_info *pi,
                         const char *token, size_t token_len)
{
    struct powerfs_cap *cap;
    struct rb_node *node;

    if (!token || token_len == 0)
        return pi->i_auth_cap;

    for (node = rb_first(&pi->i_caps); node; node = rb_next(node)) {
        cap = rb_entry(node, struct powerfs_cap, ci_node);
        if (strlen(cap->token) == token_len &&
            memcmp(cap->token, token, token_len) == 0)
            return cap;
    }
    /* token 不匹配 (例如降级态未写 token, 或老版本 client): 退回 auth_cap */
    return pi->i_auth_cap;
}

/* wire → kernel cap bits 映射前向声明 (定义在 §13.3 初始化/grant 代码段).
 * powerfs_cap_notify_work_func (定义于下方) 需要提前引用, 此处 forward. */
static unsigned int wire_capset_to_kernel_bits(__u8 wire_caps);

/*
 * powerfs_get_sb - 获取全局超级块指针
 *
 * 用于通信层等模块访问文件系统的 inode 哈希表
 * 返回的引用需要在使用时持有 (不会增加引用计数)
 */
struct super_block *powerfs_get_sb(void)
{
    return g_powerfs_sb;
}

/* ========== 前向声明 ========== */

static const struct super_operations powerfs_super_ops;
static const struct inode_operations powerfs_dir_inode_operations;
static const struct inode_operations powerfs_file_inode_operations;
static const struct file_operations powerfs_file_operations;
static const struct file_operations powerfs_dir_operations;
static const struct dentry_operations powerfs_dentry_operations;
static const struct address_space_operations powerfs_aops;

/* P0 lock/fsync/ioctl 前向声明 (定义在 8126+) */
static int powerfs_lock(struct file *filp, int cmd, struct file_lock *fl);
static int powerfs_flock(struct file *filp, int cmd, struct file_lock *fl);
static int powerfs_dir_fsync(struct file *file, loff_t start, loff_t end, int datasync);
static long powerfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
/* P1-3 fileattr 回调前向声明 (Linux 6.17 inode_operations 新成员) */
static int powerfs_fileattr_get(struct dentry *dentry, struct file_kattr *fa);
static int powerfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry,
                                 struct file_kattr *fa);
/* P1-2 atomic_open 辅助 (mknod 逻辑共享，避免 TOCTOU) */
static struct inode *__powerfs_do_create_core(struct mnt_idmap *idmap,
                                               struct inode *dir,
                                               struct dentry *dentry,
                                               umode_t mode, dev_t dev);
/* 6.17 atomic_open 签名无 mnt_idmap 参数 (VFS 要求签名 5-param),
 * 内部用 file_mnt_idmap(file) 取 idmap, 对齐  xxx_atomic_open. */
static int powerfs_atomic_open(struct inode *dir, struct dentry *dentry,
                                struct file *file, unsigned open_flag,
                                umode_t create_mode);
/* 给 atomic_open 的 finish_open() 用: powerfs_file_open 定义在 8188 行 */
static int powerfs_file_open(struct inode *inode, struct file *file);

/* P2-7: Quota enforcement (定义在 statfs 之前, 此处前向声明供 mknod/write_begin 用) */
static int powerfs_quota_check_max_files(struct inode *dir);
static int powerfs_quota_check_max_bytes(struct inode *inode, loff_t newlen);

/* P3-4: Debugfs (定义在 NFS export ops 之前, 此处前向声明供 put_super/fill_super 用) */
static void powerfs_debugfs_init(struct super_block *sb);
static void powerfs_debugfs_cleanup(struct super_block *sb);

/* P3-5: /proc metrics (定义在 P3-4 debugfs 之前, 此处前向声明供 put_super/fill_super 用) */
static void powerfs_proc_init(struct super_block *sb);
static void powerfs_proc_cleanup(struct super_block *sb);

/* 目录项管理函数 (前向声明) */
static int powerfs_add_dir_entry(struct inode *dir, u64 ino,
                                  unsigned int type, const char *name);
static int powerfs_remove_dir_entry(struct inode *dir, const char *name);
static void powerfs_clear_dir_entries(struct inode *dir);

/* lease 续约 work 函数 (前向声明，Step 2 实现真实续约) */
static void powerfs_lease_renew_work_func(struct work_struct *work);
/* 异步 setattr work 函数 (writeback offload, 定义在 write_inode 之前) */
static void powerfs_setattr_work_fn(struct work_struct *work);
/* lease 释放 (evict_inode 调用, 定义在 lease 管理段) */
static void release_all_leases(struct inode *inode);
/* lease 获取 (写路径调用, 定义在 lease 管理段) */
static int ensure_lease(struct inode *inode, loff_t offset);

/* ========== Dentry operations 实现 ========== */

/*
 * d_init - 新 dentry 创建时分配私有数据
 *
 * 目录级 lease 方案: dentry_info 不再维护独立 lease, 仅保留 RCU 释放和
 * readdir 偏移. dentry 有效性完全依赖父目录 inode 的 dir_lease_expire.
 *
 * 参考 xxx_d_init (fs/xxx/dir.c)
 *
 * 返回 0 表示成功，负值表示失败 (d_fsdata 将为 NULL)
 */
int powerfs_d_init(struct dentry *dentry)
{
    struct powerfs_dentry_info *di;

    di = kmem_cache_zalloc(powerfs_dentry_cachep, GFP_KERNEL);
    if (!di)
        return -ENOMEM;

    di->dentry = dentry;
    di->time = jiffies;

    /* === Per-dentry lease 初始化 (对齐 xxx_d_init) ===
     * 新 dentry 无 lease, 需 lookup/readdir 从 Filer 获取后填充.
     * lease_expire=0 表示无 lease, d_revalidate Layer 1 直接 miss. */
    di->flags = 0;
    di->lease_issuer_id = 0;
    di->lease_renew_after = 0;
    di->lease_renew_from = 0;
    di->lease_expire = 0;
    di->lease_duration_ms = 0;
    di->lease_gen = 0;
    di->lease_seq = 0;
    di->dir_shared_gen = 0;
    INIT_LIST_HEAD(&di->lease_list);

    dentry->d_fsdata = di;

    pr_debug("powerfs: d_init '%pd' (di=%p)\n", dentry, di);
    return 0;
}

/*
 * powerfs_di_free_rcu - RCU 回调, 延迟释放 dentry 私有数据
 *
 * 为什么需要 RCU 延迟释放:
 *   __d_lookup_rcu 在 RCU 临界区中返回 dentry, 调用者 lookup_fast 随后调用
 *   d_revalidate, 后者读 dentry->d_fsdata. 如果 d_release 直接 kmem_cache_free
 *   释放 di, RCU reader 仍可能解引用已释放的指针 -> UAF in __d_lookup_rcu.
 *
 *   d_release 只是设置 d_fsdata = NULL 并 call_rcu 排队, 真正的 kmem_cache_free
 *   在 RCU grace period 之后执行, 此时所有 RCU reader 已退出临界区.
 */
static void powerfs_di_free_rcu(struct rcu_head *head)
{
    struct powerfs_dentry_info *di =
        container_of(head, struct powerfs_dentry_info, rcu);

    kmem_cache_free(powerfs_dentry_cachep, di);
}

/*
 * d_release - dentry 销毁前释放私有数据
 *
 * Per-dentry lease 方案: 从全局 dentry_lease_list 摘除 (如果在上面).
 *
 * 重要: d_fsdata 必须用 call_rcu 延迟释放, 不能裸 kmem_cache_free.
 * 原因: RCU path walk (__d_lookup_rcu + d_revalidate) 可能并发读 d_fsdata.
 *
 * 参考 xxx_d_release (fs/xxx/dir.c)
 */
void powerfs_d_release(struct dentry *dentry)
{
    struct powerfs_dentry_info *di = dentry->d_fsdata;
    struct powerfs_sb_info *sbi;

    if (!di)
        return;

    pr_debug("powerfs: d_release '%pd' (di=%p)\n", dentry, di);

    /* 从全局 dentry_lease_list 摘除 (如果已挂载且 di 在链表上).
     * 对齐 xxx_d_release: __dentry_unhash_lru. */
    sbi = POWERFS_SB_INFO(dentry->d_sb);
    if (sbi && sbi->client && (di->flags & POWERFS_DN_LEASE_LIST)) {
        spin_lock(&sbi->client->dentry_lease_lock);
        list_del_init(&di->lease_list);
        spin_unlock(&sbi->client->dentry_lease_lock);
        di->flags &= ~POWERFS_DN_LEASE_LIST;
    }

    /* 设置 d_fsdata = NULL 并通过 RCU 延迟释放 di. */
    dentry->d_fsdata = NULL;
    call_rcu(&di->rcu, powerfs_di_free_rcu);
}

/* Phase 1 前置声明: 目录 lease 失效 (定义在 readdir 区段, 但 mknod 等更早使用). */
static void powerfs_invalidate_dir_lease(struct inode *dir);

/*
 * powerfs_fill_dentry_lease — 在 lookup/readdir 成功后填充 per-dentry lease
 *
 * 对齐  __update_dentry_lease (fs/xxx/inode.c L1388-1453) +
 *      Rust cache.rs DentryLease (powerfs-fuse/src/cache.rs L153-160)
 *
 * 填充内容:
 *   - lease_expire: now + TTL (对齐 Rust DentryLease::expire_at)
 *   - dir_shared_gen: 父目录当前 i_shared_gen (对齐 Rust dir_shared_gen)
 *   - lease_gen / lease_seq: 预留 (Filer 后续在响应中携带)
 *
 * 调用上下文: lookup/readdir 成功后, 持有 dentry 引用
 * 注意: 不需要持锁 (单线程 VFS lookup 路径, dentry 尚未被并发访问)
 */
static void powerfs_fill_dentry_lease(struct dentry *dentry,
                                       struct inode *dir,
                                       u64 lease_ttl_ms)
{
    struct powerfs_dentry_info *di;
    struct powerfs_inode_info *parent_pi;
    struct powerfs_sb_info *sbi;

    if (!dentry || !dir)
        return;

    di = POWERFS_D(dentry);
    if (!di)
        return;

    parent_pi = POWERFS_I(dir);
    sbi = POWERFS_SB_INFO(dir->i_sb);

    /* 填充 lease TTL (Layer 1) */
    if (lease_ttl_ms > 0) {
        di->lease_duration_ms = lease_ttl_ms;
        di->lease_expire = jiffies + msecs_to_jiffies(lease_ttl_ms);
        di->lease_renew_after = jiffies +
            msecs_to_jiffies(lease_ttl_ms / 3);  /* renew at 2/3 TTL */
        di->lease_renew_from = 0;
    } else {
        /* Filer 未发放 lease, 使用默认目录 TTL */
        di->lease_duration_ms = jiffies_to_msecs(POWERFS_DIR_LEASE_TTL);
        di->lease_expire = jiffies + POWERFS_DIR_LEASE_TTL;
        di->lease_renew_after = jiffies + POWERFS_DIR_LEASE_TTL * 2 / 3;
    }

    /* 填充 dir_shared_gen (Layer 2) — 对齐 Rust cache.rs dir_shared_gen */
    di->dir_shared_gen = (u64)atomic_read(&parent_pi->i_shared_gen);

    /* 挂到全局 dentry_lease_list (用于 shrinker/LRU 管理) */
    if (sbi && sbi->client && !(di->flags & POWERFS_DN_LEASE_LIST)) {
        spin_lock(&sbi->client->dentry_lease_lock);
        list_add_tail(&di->lease_list, &sbi->client->dentry_lease_list);
        di->flags |= POWERFS_DN_LEASE_LIST;
        spin_unlock(&sbi->client->dentry_lease_lock);
    }

    pr_debug("powerfs: fill_dentry_lease '%pd' expire=%lu gen=%llu\n",
             dentry, di->lease_expire, di->dir_shared_gen);
}

/*
 * d_revalidate — 三层校验 (对齐  + Rust DentryLeaseStatus)
 *
 * Layer 1: per-dentry lease (di->lease_expire > now → valid)
 * Layer 2: dir_shared_gen matches parent's i_shared_gen && parent has I_COMPLETE
 * Layer 3: RPC (返回 1 让 VFS 重试 lookup, lookup 会向 Filer 发 RPC)
 *
 * RCU 路径: 无锁快速检查 Layer 1 + Layer 2, miss 则 -ECHILD 降级
 * REF 路径: 同样检查, miss 则返回 1 (VFS 会触发 re-lookup)
 *
 * 返回值:
 *   1: dentry 有效 (正/负), 使用缓存
 *   -ECHILD: 退出 RCU (仅 RCU 模式)
 *
 * 参考: xxx_d_revalidate (fs/xxx/dir.c L1280-1370)
 *       Rust DentryLeaseStatus (powerfs-fuse/src/cache.rs L167-179)
 */
int powerfs_d_revalidate(struct inode *dir, const struct qstr *name,
                         struct dentry *dentry, unsigned int flags)
{
    struct powerfs_inode_info *parent_pi;
    struct powerfs_dentry_info *di;
    unsigned long now = jiffies;
    unsigned long lease_expire;
    u64 parent_shared_gen;

    /* === RCU 路径: 无锁检查 === */
    if (flags & LOOKUP_RCU) {
        if (!dir)
            return -ECHILD;
        parent_pi = POWERFS_I(dir);
        di = dentry->d_fsdata;
        if (!di)
            return -ECHILD;

        /* Layer 1: per-dentry lease (无锁读, 可能略旧) */
        lease_expire = READ_ONCE(di->lease_expire);
        if (lease_expire && time_before(now, lease_expire))
            return 1;       /* dentry lease 有效: 正/负 dentry 全部放行 */

        /* Layer 2: dir_shared_gen + parent I_COMPLETE (无锁读) */
        parent_shared_gen = (u64)atomic_read(&parent_pi->i_shared_gen);
        if (di->dir_shared_gen == parent_shared_gen &&
            (READ_ONCE(parent_pi->i_flags) & POWERFS_I_COMPLETE))
            return 1;       /* shared_gen 匹配 + 目录完整 → 信任缓存 */

        /* Layer 3: miss → 降级 REF 路径 (REF 路径可做 RPC) */
        return -ECHILD;
    }

    /* === REF 路径: 带锁三层校验 === */
    if (!dir)
        return 1;
    parent_pi = POWERFS_I(dir);
    di = dentry->d_fsdata;
    if (!di)
        return 1;

    /* Layer 1: per-dentry lease */
    lease_expire = READ_ONCE(di->lease_expire);
    if (lease_expire && time_before(now, lease_expire)) {
        struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
        if (sbi && sbi->client)
            powerfs_metric_dentry_hit(&sbi->client->metrics);
        return 1;
    }

    /* Layer 2: dir_shared_gen + I_COMPLETE */
    parent_shared_gen = (u64)atomic_read(&parent_pi->i_shared_gen);
    if (di->dir_shared_gen == parent_shared_gen &&
        (READ_ONCE(parent_pi->i_flags) & POWERFS_I_COMPLETE)) {
        struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
        if (sbi && sbi->client)
            powerfs_metric_dentry_hit(&sbi->client->metrics);
        return 1;
    }

    /* Layer 3: lease miss → 返回 1 让 VFS 使用缓存.
     * 不在此处做 RPC (d_revalidate 在路径遍历中频繁调用).
     * VFS 会在需要时重新 lookup (向 Filer 发 RPC), 届时填充新 lease.
     * stale dentry 由 readdir 刷新 / shrinker 回收 / 本地 mutation 失效.
     *
     * Note: 返回 0 (invalid) 会触发 d_invalidate→d_drop→re-lookup 循环,
     * 与 __d_lookup_rcu 竞态可能导致 RCU stall. 统一返回 1 更安全.
     * 正/负 dentry 无差别: 负 dentry 的 inode==NULL, Layer 1/2 仍可信任. */
    {
        struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
        if (sbi && sbi->client)
            powerfs_metric_dentry_mis(&sbi->client->metrics);
    }
    return 1;
}

/*
 * d_prune - dentry 被 shrinker 回收前的通知
 *
 * 参考 xxx_d_prune (fs/xxx/dir.c)
 *
 * 用于清除父目录的 complete 标志，因为目录内容缓存不再完整
 */
void powerfs_d_prune(struct dentry *dentry)
{
    struct dentry *parent = dentry->d_parent;
    struct powerfs_inode_info *ppi;
    struct inode *dir;

    /* 根 dentry 不 prune (参考 xxx_d_prune) */
    if (IS_ROOT(dentry))
        return;

    /* d_prune 在 dentry->d_lock 被持有时调用 (见 __dentry_kill).
     * 只能用 d_parent (d_lock 保护) 和无锁操作, 不能获取任何 spinlock
     * (如 i_lock), 否则会与持 i_lock 后尝试 d_lock 的路径死锁. */
    dir = d_inode(parent);
    if (!dir || !S_ISDIR(dir->i_mode))
        return;

    ppi = POWERFS_I(dir);

    pr_debug("powerfs: d_prune '%pd' (parent=%pd, clearing dir_complete)\n",
             dentry, parent);

    /* 清除父目录的 complete 标志.
     * 参照 xxx __xxx_dir_clear_complete (atomic64_inc, 无锁).
     * dir_complete 是 bool, WRITE_ONCE 保证原子写入, 读取侧用 READ_ONCE. */
    WRITE_ONCE(ppi->dir_complete, false);
}

/*
 * powerfs_d_delete - P2-3: dentry 删除前回调，lease 有效时保留 dentry.
 *
 * 对齐  xxx_d_delete (dir.c L2046):
 *   - 负 dentry (d_really_is_negative): 返回 0 (保留，VFS 不主动删)
 *   - 正 dentry + dentry lease 有效 (Layer 1 TTL): 返回 0 (保留)
 *   - 正 dentry + dir lease 有效 (Layer 2 shared_gen + I_COMPLETE): 返回 0
 *   - 无 lease: 返回 1 (允许 VFS 回收)
 *
 * 返回 0 = 保留 dentry (缓存命中率高，减少 lookup RPC)
 * 返回 1 = 允许 VFS 在 d_count==0 时回收 dentry
 *
 * 注意: 此处在 dentry 即将被回收时调用，不能阻塞 (不能做 RPC)。
 *       仅做本地 lease 过期检查，与 d_revalidate 的 Layer 1/2 逻辑一致。
 */
static int powerfs_d_delete(const struct dentry *dentry)
{
    struct powerfs_dentry_info *di;
    unsigned long now = jiffies;

    /* 负 dentry: 保留 (d_revalidate 会后续校验) */
    if (d_really_is_negative(dentry))
        return 0;

    di = dentry->d_fsdata;
    if (!di)
        return 1;

    /* Layer 1: per-dentry lease 未过期 → 保留 */
    if (di->lease_expire && time_before(now, di->lease_expire))
        return 0;

    /* Layer 2: dir_shared_gen 匹配 + 父目录 I_COMPLETE → 保留 */
    {
        struct dentry *parent = dentry->d_parent;
        struct inode *dir;

        if (!IS_ROOT(dentry) && parent) {
            dir = d_inode(parent);
            if (dir) {
                struct powerfs_inode_info *ppi = POWERFS_I(dir);
                u64 parent_shared_gen = (u64)atomic_read(&ppi->i_shared_gen);

                if (di->dir_shared_gen == parent_shared_gen &&
                    (READ_ONCE(ppi->i_flags) & POWERFS_I_COMPLETE))
                    return 0;
            }
        }
    }

    return 1;
}

/* Dentry operations 表
 *
 * 目录级 lease 方案:
 *   - d_revalidate: 检查父目录 lease, 永不返回 0 (避免 RCU stall)
 *   - d_init: 分配 dentry_info (RCU 释放用)
 *   - d_release: RCU 延迟释放 dentry_info (防止内存泄漏 + UAF)
 *   - d_prune: 清父目录 dir_complete (shrinker 回收时目录缓存不再完整)
 *   - d_delete: lease 有效时保留 dentry (减少 lookup RPC)
 *
 * 参考 dentry_operations
 */
static const struct dentry_operations powerfs_dentry_operations = {
    .d_revalidate   = powerfs_d_revalidate,
    .d_init         = powerfs_d_init,
    .d_release      = powerfs_d_release,
    .d_prune        = powerfs_d_prune,
    .d_delete       = powerfs_d_delete,    /* P2-3: lease 有效时保留 dentry */
};

/* ========== 辅助函数 ========== */

/*
 * powerfs_ino_compare - inode 比较函数 (用于 iget5_locked/ilookup5)
 *
 * 参考 xxx_ino_compare (fs/xxx/super.h)
 *
 * 比较 inode 的 ino 是否匹配。
 * 目前只比较 ino，后续如果支持快照等可以扩展。
 */
static int powerfs_ino_compare(struct inode *inode, void *data)
{
    u64 *pino = (u64 *)data;
    return inode->i_ino == *pino;
}

/*
 * powerfs_set_ino_cb - 设置新 inode 的 ino (用于 iget5_locked)
 *
 * 参考 xxx_set_ino_cb (fs/xxx/inode.c)
 */
static int powerfs_set_ino_cb(struct inode *inode, void *data)
{
    u64 *pino = (u64 *)data;
    inode->i_ino = *pino;
    return 0;
}

/*
 * powerfs_iget - 获取或创建 inode (参考 xxx_get_inode)
 *
 * 使用 iget5_locked 在内核 inode 哈希表中查找：
 *   - 如果找到：增加引用计数并返回
 *   - 如果没找到：分配新 inode，设置 I_NEW 状态并返回
 *
 * 调用者需要检查 inode->i_state & I_NEW 来判断是否是新创建的，
 * 如果是新创建的，需要调用 powerfs_init_inode 初始化，
 * 然后调用 unlock_new_inode 解锁。
 */
struct inode *powerfs_iget(struct super_block *sb, u64 ino)
{
    struct inode *inode;
    u64 ino_val = ino;

    inode = iget5_locked(sb, (unsigned long)ino,
                         powerfs_ino_compare,
                         powerfs_set_ino_cb,
                         &ino_val);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    pr_debug("powerfs: iget ino=%llu inode=%p new=%d\n",
             ino, inode, !!(inode->i_state & I_NEW));

    return inode;
}

/*
 * powerfs_find_inode - 查找已存在的 inode (参考 xxx_find_inode)
 *
 * 使用 ilookup5 在内核 inode 哈希表中查找：
 *   - 如果找到：增加引用计数并返回
 *   - 如果没找到：返回 NULL
 *
 * 注意: 不会创建新 inode，只查找已存在的。
 */
struct inode *powerfs_find_inode(struct super_block *sb, u64 ino)
{
    u64 ino_val = ino;
    return ilookup5(sb, (unsigned long)ino,
                    powerfs_ino_compare, &ino_val);
}

/*
 * powerfs_locate_chunk - K1-5 统一 chunk 定位 (Flat/Stripe 多卷入口)
 *
 * 对齐 FUSE powerfs-fuse/src/fuse.rs resolve_stripe_chunk() (L462) 逻辑:
 *   - Flat 模型: needle_id = file_key + offset / chunk_size,
 *     volume_id = inode->volume_id (单卷)
 *   - Stripe 模型 (K3):
 *       stripe_unit_idx = offset / stripe_size
 *       chunk_idx_in_unit = (offset % stripe_size) / chunk_size
 *       volume_id = volume_ids[stripe_unit_idx]
 *       needle_id = file_key + chunk_idx_in_unit
 *   - Inline 模型 (K2): 返回 -EINVAL, inline 不走 volume 路径
 *
 * 注意: 调用方应持 pi->i_lock 或确保 volume_ids/chunks 不被并发释放.
 *       当前 read/write 路径在持锁快照后调用, 满足约束.
 */
int powerfs_locate_chunk(struct powerfs_inode_info *pi, loff_t offset,
                         u64 *volume_id_out, u64 *needle_id_out)
{
    u32 chunk_size;
    u64 chunk_idx;

    if (!pi || !volume_id_out || !needle_id_out)
        return -EINVAL;

    /* Inline 文件不走 volume 路径 (K2) */
    if (pi->placement == POWERFS_PLACEMENT_INLINE)
        return -EINVAL;

    /* chunk_size: 优先用 layout 解析值, 兜底 POWERFS_CHUNK_SIZE */
    chunk_size = pi->layout_chunk_size ? pi->layout_chunk_size : POWERFS_CHUNK_SIZE;
    if (chunk_size == 0)
        return -EINVAL;

    chunk_idx = (u64)(offset / chunk_size);

    /* K3 多卷路径: chunks 数组存在且 chunk_idx 命中.
     * 用于 Flat 模式下 GETATTR 返回的显式 chunks 列表.
     * 对齐 FUSE chunk_map: 使用显式 per-chunk needle_id, 而非 file_key + chunk_idx. */
    if (pi->chunks && chunk_idx < pi->chunk_count) {
        struct powerfs_chunk_map *cm = &pi->chunks[chunk_idx];
        if (cm->volume_id != 0 && cm->needle_id != 0) {
            *volume_id_out = cm->volume_id;
            *needle_id_out = cm->needle_id;
            pr_debug("powerfs: locate CHUNKS ino=%lu offset=%lld chunk_idx=%llu -> vid=%llu nid=%llu\n",
                    pi->netfs.inode.i_ino, offset, chunk_idx,
                    (unsigned long long)cm->volume_id,
                    (unsigned long long)cm->needle_id);
            return 0;
        }
    }

    /* K3 Stripe 多卷路径: volume_ids 数组 + file_key base needle.
     * 对齐 FUSE resolve_stripe_chunk (fuse.rs L462).
     * stripe_unit_idx 索引 volume_ids[], chunk_idx_in_unit 偏移 needle_id. */
    if ((pi->placement == POWERFS_PLACEMENT_STRIPE ||
         pi->placement == POWERFS_PLACEMENT_WIDESTRIPE) &&
        pi->volume_ids && pi->volume_ids_count > 0) {
        u64 stripe_size = pi->stripe_size ? pi->stripe_size : chunk_size;
        u64 stripe_unit_idx = (u64)(offset / stripe_size);
        u64 chunk_idx_in_unit;

        if (stripe_unit_idx >= pi->volume_ids_count) {
            pr_debug("powerfs: locate stripe_unit_idx=%llu >= count=%u (offset=%lld)\n",
                     stripe_unit_idx, pi->volume_ids_count, offset);
            return -EINVAL;
        }

        chunk_idx_in_unit = (u64)((offset % stripe_size) / chunk_size);
        *volume_id_out = pi->volume_ids[stripe_unit_idx];
        *needle_id_out = pi->file_key + chunk_idx_in_unit;
        return 0;
    }

    /* Flat 模型: file_key + chunk_idx, 单卷.
     * 仅当 pi->chunks 未填充 (无 PER_CHUNK 数据) 时使用此回退路径. */
    if (!pi->volume_id || !pi->file_key)
        return -EINVAL;

    *volume_id_out = pi->volume_id;
    *needle_id_out = pi->file_key + chunk_idx;
    pr_debug("powerfs: locate FALLBACK ino=%lu offset=%lld chunk_idx=%llu -> vid=%llu nid=%llu (fkey=%llu+%llu)\n",
            pi->netfs.inode.i_ino, offset, chunk_idx,
            (unsigned long long)pi->volume_id,
            (unsigned long long)*needle_id_out,
            (unsigned long long)pi->file_key,
            chunk_idx);
    return 0;
}

/*
 * powerfs_apply_layout_to_inode - K3-1 将 FileLayout 解析结果应用到 inode
 *
 * 在持 pi->i_lock 的情况下调用. volume_ids/inline_data 所有权从 layout 转移到 inode.
 * 若 inode 已有 volume_ids/inline_data, 先 kfree 旧的再替换 (避免泄漏).
 */
void powerfs_apply_layout_to_inode(struct powerfs_inode_info *pi,
                                   struct powerfs_file_layout *layout)
{
    u64 *old_vids;
    u8 *old_inline;

    if (!pi || !layout)
        return;

    /* K2: 保护有未提交 inline_data 的 inode — refresh_work 的 getattr 可能在
     * 文件写入过程中触发, 此时 Filer 还没收到 inline_data (close 时才提交),
     * getattr 响应中 placement=Flat (info.inline_data=None).
     * 若直接覆盖 placement=Flat, 后续 write_end 不再走 Inline 分支,
     * 导致 inline_data 不一致 + 数据丢失.
     * 修复: inode 有 inline_dirty 时, 不从 getattr 覆盖 placement.
     *
     * K2-8: Also protect against Flat→INLINE regression after migration.
     * After powerfs_migrate_inline_to_flat sets placement=Flat + volume_id +
     * file_key, the Filer may still report placement=INLINE (close hasn't
     * synced the new layout yet). Allowing placement to revert to INLINE
     * causes writeback to use the INLINE path (inline_data=NULL → data loss)
     * and reads to fail (no needle lookup). Guard: if inode has volume_id
     * or file_key (migrated to Flat), don't revert to INLINE. */
    if (layout->has_placement) {
        if (pi->placement == POWERFS_PLACEMENT_INLINE && pi->inline_dirty &&
            layout->placement != POWERFS_PLACEMENT_INLINE) {
            pr_info("powerfs: apply_layout skip placement=%u→%u, inline_dirty ino=%lu\n",
                    pi->placement, layout->placement, pi->netfs.inode.i_ino);
        } else if (pi->placement != POWERFS_PLACEMENT_INLINE &&
                   (pi->volume_id || pi->file_key) &&
                   layout->placement == POWERFS_PLACEMENT_INLINE) {
            pr_info("powerfs: apply_layout skip placement=%u→%u (Flat→INLINE regression, has volume_id/file_key) ino=%lu\n",
                    pi->placement, layout->placement, pi->netfs.inode.i_ino);
        } else {
            pi->placement = layout->placement;
        }
    }
    if (layout->has_reliability)
        pi->reliability = layout->reliability;
    pi->reliability_state = layout->reliability_state;
    if (layout->chunk_size > 0)
        pi->layout_chunk_size = layout->chunk_size;

    /* K2: InlineMaxSize — 从 layout 同步到 inode */
    if (layout->inline_max_size > 0)
        pi->inline_max_size = layout->inline_max_size;
    else if (pi->inline_max_size == 0)
        pi->inline_max_size = POWERFS_INLINE_MAX_SIZE;

    /* K2: InlineData — 仅在 placement==INLINE 时应用.
     * 其他模式不应携带 inline_data, 但若误传则释放避免泄漏. */
    if (pi->placement == POWERFS_PLACEMENT_INLINE && layout->has_inline_data) {
        /* inline_data 所有权转移: 先释放旧 buffer, 再挂载新 buffer */
        old_inline = pi->inline_data;
        pi->inline_data = layout->inline_data;
        pi->inline_len = layout->inline_len;
        layout->inline_data = NULL;       /* 所有权转移, 防止 double-free */
        layout->inline_len = 0;
        kfree(old_inline);
    } else {
        /* Flat/Stripe: 不应持有 inline_data, 释放误传的 buffer */
        kfree(layout->inline_data);
        layout->inline_data = NULL;
        layout->inline_len = 0;
        /* 若 placement 从 Inline 切换到 Flat (迁移后), 清除 inode 的 inline_data */
        if (pi->placement != POWERFS_PLACEMENT_INLINE && pi->inline_data) {
            kfree(pi->inline_data);
            pi->inline_data = NULL;
            pi->inline_len = 0;
            pi->inline_dirty = false;
        }
    }

    /* K3: Stripe 元数据. 仅在 placement 为 Stripe/WideStripe 时应用.
     * Flat/Inline 模式不应携带 volume_ids, 但若误传则释放避免泄漏. */
    if (pi->placement == POWERFS_PLACEMENT_STRIPE ||
        pi->placement == POWERFS_PLACEMENT_WIDESTRIPE) {
        pi->stripe_size = layout->stripe_size;
        pi->stripe_count = layout->stripe_count;
        pi->start_volume_idx = layout->start_volume_idx;

        /* volume_ids 所有权转移: 先释放旧数组, 再挂载新数组 */
        old_vids = pi->volume_ids;
        pi->volume_ids = layout->volume_ids;
        pi->volume_ids_count = layout->volume_ids_count;
        layout->volume_ids = NULL;       /* 所有权转移, 防止 double-free */
        layout->volume_ids_count = 0;
        kfree(old_vids);
    } else {
        /* Flat/Inline: 不应持有 volume_ids, 释放误传的数组 */
        kfree(layout->volume_ids);
        layout->volume_ids = NULL;
        layout->volume_ids_count = 0;
        /* 清零 inode 上可能残留的 Stripe 字段 (placement 切换场景) */
        if (pi->volume_ids) {
            kfree(pi->volume_ids);
            pi->volume_ids = NULL;
            pi->volume_ids_count = 0;
        }
        pi->stripe_size = 0;
        pi->stripe_count = 0;
    }

    /* K4-8: EC 元数据 — 从 Reliability EC 分支解析.
     * 仅在 reliability==EC 时应用, 非 EC 清零. */
    if (pi->reliability == POWERFS_RELIABILITY_EC) {
        pi->ec_data_shards = layout->ec_data_shards;
        pi->ec_parity_shards = layout->ec_parity_shards;
    } else {
        pi->ec_data_shards = 0;
        pi->ec_parity_shards = 0;
    }

    /* K4-2: ReplicaChunks — 读 failover 使用.
     * 所有权转移: 先释放旧数组, 再挂载新数组.
     * 仅在 reliability==REPLICATED 时有意义, 但解析不区分 (apply 时决定). */
    if (layout->has_replica_chunks) {
        struct powerfs_chunk_map *old_rep = pi->replica_chunks;
        pi->replica_chunks = layout->replica_chunks;
        pi->replica_count = layout->replica_count;
        layout->replica_chunks = NULL;     /* 所有权转移, 防止 double-free */
        layout->replica_count = 0;
        kfree(old_rep);
    }

    /* ChunkLayout PER_CHUNK: Filer 对 Flat/EC 文件均使用 tag=0x01 编码 chunks 列表.
     * - EC 文件: shards 列表, 存入 pi->ec_chunks (EC 降级读取路径使用)
     * - Flat 文件: 主 chunks 列表, 存入 pi->chunks (locate_chunk 使用显式 needle_id)
     * - Stripe 文件: 释放 (locate_chunk 使用 volume_ids 路径)
     *
     * 修复 remount 读路径 bug: 之前 Flat 文件的 PER_CHUNK 数据被误存入 pi->ec_chunks
     * (仅 EC 读取路径使用), 而 pi->chunks 始终为 NULL, 导致 locate_chunk 回退到
     * file_key + chunk_idx 计算. FUSE 客户端使用显式 per-chunk needle_id (chunk_map),
     * 两者在 needle_id 非连续时不一致. 现在对 Flat 文件将 PER_CHUNK 数据存入
     * pi->chunks, 使 locate_chunk 使用与 FUSE 客户端相同的显式查找. */
    if (layout->has_ec_chunks) {
        if (pi->reliability == POWERFS_RELIABILITY_EC) {
            /* EC: shards 列表存入 ec_chunks */
            struct powerfs_chunk_map *old_ec = pi->ec_chunks;
            pi->ec_chunks = layout->ec_chunks;
            pi->ec_chunk_count = layout->ec_chunk_count;
            layout->ec_chunks = NULL;
            layout->ec_chunk_count = 0;
            kfree(old_ec);
        } else if (pi->placement == POWERFS_PLACEMENT_FLAT) {
            /* Flat: PER_CHUNK 是主 chunks 列表, 存入 pi->chunks.
             * locate_chunk 优先使用 pi->chunks[chunk_idx] 的显式 needle_id,
             * 而非 file_key + chunk_idx 计算 (对齐 FUSE chunk_map). */
            struct powerfs_chunk_map *old = pi->chunks;
            pi->chunks = layout->ec_chunks;
            pi->chunk_count = layout->ec_chunk_count;
            layout->ec_chunks = NULL;
            layout->ec_chunk_count = 0;
            kfree(old);
            pr_debug("powerfs: apply_layout FLAT chunks count=%u (vid=%llu fkey=%llu)\n",
                    pi->chunk_count,
                    (unsigned long long)pi->volume_id,
                    (unsigned long long)pi->file_key);
        } else {
            /* Stripe: 释放, locate_chunk 使用 volume_ids 路径 */
            kfree(layout->ec_chunks);
            layout->ec_chunks = NULL;
            layout->ec_chunk_count = 0;
        }
    }
}

/*
 * powerfs_refresh_inode_work - 异步刷新 inode 元数据 (workqueue 回调)
 *
 * 在独立工作队列中执行, 避免 self-deadlock:
 *   - 调度器线程收到 NOTIFY → powerfs_invalidate_one → queue_work
 *   - 本函数在独立线程中执行 powerfs_net_getattr
 *   - getattr 响应由调度器线程处理 (不阻塞本线程)
 *
 * 步骤:
 *   1. 发 getattr 获取最新 size/volume_id/file_key
 *   2. 更新 inode 属性 (i_size, volume_id, file_key)
 *   3. 失效 page cache (clean pages)
 *   4. 清 need_refresh
 */
/* RCU 回调: 延迟释放 refresh_work (workqueue 在 work_fn 返回后仍引用 work_struct) */
static void powerfs_refresh_work_free_rcu(struct rcu_head *head)
{
    struct powerfs_refresh_work *rw =
        container_of(head, struct powerfs_refresh_work, rcu);
    kfree(rw);
}

static void powerfs_refresh_inode_work(struct work_struct *work)
{
    struct powerfs_refresh_work *rw =
        container_of(work, struct powerfs_refresh_work, work);
    struct inode *inode = rw->inode;
    struct powerfs_inode_info *pi;
    __u32 mode = 0, uid = 0, gid = 0, nlink = 0;
    __u64 size = 0, mtime = 0, atime = 0, ctime = 0;
    __u64 volume_id = 0, file_key = 0;
    int ret;

    /* If inode is NULL, do the lookup here (in workqueue context, not RX thread).
     * This avoids blocking the RX dispatcher in ilookup5 → __wait_on_freeing_inode
     * when the inode is being freed. The lookup may block, but only this workqueue
     * thread is affected, not the RX thread that handles all network responses. */
    if (!inode) {
        struct super_block *sb = powerfs_get_sb();
        if (!sb)
            goto out_free;
        inode = powerfs_find_inode(sb, rw->ino);
        if (!inode)
            goto out_free;
        /* ilookup5 returned a referenced inode; iput at out_iput. */
    }

    /* 防御性检查: inode 是否已被 evict (I_FREEING/I_CLEAR).
     * 虽然修复了 igrab 检查后不应出现此情况, 但 getattr 是长时间
     * 阻塞网络调用, 返回后 inode 状态可能已变. 持有 igrab 引用
     * 保证 inode 不会被释放, 但 evict_inode 可能已标记 shutdown.
     * 此时不应再修改 inode 字段 (可能与 evict 并发). */
    if (inode->i_state & (I_FREEING | I_CLEAR | I_WILL_FREE)) {
        pr_debug("powerfs: refresh_work ino=%llu inode being evicted, skip\n",
                 rw->ino);
        goto out_iput;
    }

    pi = POWERFS_I(inode);

    /* evict_inode 已设置 shutdown: inode 私有数据正在被释放,
     * 不应再修改 pi 字段. */
    if (pi->shutdown) {
        pr_debug("powerfs: refresh_work ino=%llu inode shutdown, skip\n",
                 rw->ino);
        goto out_iput;
    }

    /* Mark need_refresh: signals concurrent readers that a refresh is in flight.
     * (Previously set in powerfs_invalidate_one before scheduling, but now
     * the inode lookup is deferred to this work function.) */
    spin_lock(&pi->i_lock);
    pi->need_refresh = true;
    spin_unlock(&pi->i_lock);

    /* 1. 发 getattr 获取最新元数据 */
    {
        struct powerfs_file_layout layout = {0};
        __u64 rbytes = 0, rfiles = 0, rsubdirs = 0, rctime_sec = 0;
        __u32 rctime_nsec = 0;
        ret = powerfs_net_getattr(inode->i_ino, &mode, &uid, &gid,
                                  &size, &nlink,
                                  &mtime, &atime, &ctime,
                                  &volume_id, &file_key, &layout,
                                  /* P1-5: 回填 rstat 到 pi，getattr 展示给用户 (du, ls -l) */
                                  &rbytes, &rfiles, &rsubdirs,
                                  &rctime_sec, &rctime_nsec);
        if (ret == 0) {
            spin_lock(&pi->i_lock);
            powerfs_apply_layout_to_inode(pi, &layout);
            /* P1-5: 仅对目录 inode 回填 rstat。
             * Filer 对文件 inode 不编码 rstat 字段，解析得到的都是 0。
             * S_ISDIR 判断可防误覆盖。 */
            if (S_ISDIR(inode->i_mode)) {
                pi->i_rbytes = rbytes;
                pi->i_rfiles = rfiles;
                pi->i_rsubdirs = rsubdirs;
                pi->i_rctime.tv_sec = (time64_t)rctime_sec;
                pi->i_rctime.tv_nsec = (long)rctime_nsec;
            }
            spin_unlock(&pi->i_lock);
            /* apply 后 layout.volume_ids/inline_data/replica_chunks 已转移到 inode (或已释放).
             * 防御性: 若 apply 异常未消费, 这里释放. */
            kfree(layout.volume_ids);
            kfree(layout.inline_data);
            kfree(layout.replica_chunks);
            kfree(layout.ec_chunks);
        } else {
            /* getattr 失败: 释放 parse 可能分配的 volume_ids/inline_data/replica_chunks/ec_chunks */
            kfree(layout.volume_ids);
            kfree(layout.inline_data);
            kfree(layout.replica_chunks);
            kfree(layout.ec_chunks);
        }
    }
    if (ret) {
        pr_warn("powerfs: refresh_work ino=%llu getattr failed: %d\n",
                rw->ino, ret);
        /* getattr 失败: 清 cache_valid, 让下次访问触发 re-lookup.
         * 但若 inode 已 shutdown, 不修改 pi 字段 (evict 进行中). */
        if (!pi->shutdown) {
            spin_lock(&pi->i_lock);
            pi->cache_valid = false;
            pi->need_refresh = false;
            spin_unlock(&pi->i_lock);
        }
        goto out_iput;
    }

    /* getattr 返回后再次检查: 长时间网络阻塞期间 inode 可能被 evict. */
    if (pi->shutdown || (inode->i_state & (I_FREEING | I_CLEAR))) {
        pr_debug("powerfs: refresh_work ino=%llu inode evicted during getattr, skip update\n",
                 rw->ino);
        goto out_iput;
    }

    /* 2. 更新 inode 属性 */
    /* Read pi->content_size and inline_dirty BEFORE inode->i_lock.
     * Lock ordering: pi->i_lock → inode->i_lock (powerfs_setattr acquires
     * pi->i_lock first, then calls setattr_copy/mark_inode_dirty which use
     * inode->i_lock). Acquiring pi->i_lock inside inode->i_lock would invert
     * this order and risk deadlock. */
    u64 local_content_size;
    bool local_inline_dirty;
    spin_lock(&pi->i_lock);
    local_content_size = pi->content_size;
    local_inline_dirty = pi->inline_dirty;
    spin_unlock(&pi->i_lock);

    /* Determine if local client has pending (uncommitted) modifications.
     * If so, skip size/attribute updates — the GETATTR response may be stale
     * (self-NOTIFY: Filer notifies the same client that made the change,
     *  before the server has processed the local SETATTR/writeback).
     *
     * Multi-client fix (MC-302): Previously, the check also included
     * `local_content_size != size`, which incorrectly fired when a REMOTE
     * client modified the file (local size != new server size). This caused
     * size, mode, and pagecache updates to be skipped on remote changes,
     * leading to stale reads in multi-client scenarios.
     *
     * The fix: rely ONLY on dirty/writeback page tags and inline_dirty to
     * detect pending LOCAL modifications. If no local writes are pending,
     * always accept the server's attributes (the change came from another
     * client or the local writeback has completed). */
    bool local_pending = mapping_tagged(inode->i_mapping, PAGECACHE_TAG_DIRTY) ||
                         mapping_tagged(inode->i_mapping, PAGECACHE_TAG_WRITEBACK) ||
                         local_inline_dirty;

    spin_lock(&inode->i_lock);
    if (local_pending) {
        pr_debug("powerfs: refresh_work ino=%llu skip attr update (local pending: dirty/wb/inline)\n",
                rw->ino);
    } else {
        /* Accept server's size — no pending local modifications */
        if (i_size_read(inode) != size) {
            i_size_write(inode, size);
        }
        /* Update permission bits and ownership from GETATTR response.
         * Previously, mode/uid/gid were fetched but never applied, causing
         * chmod/chown changes by other clients to be invisible (MC-304). */
        inode->i_mode = mode;
        i_uid_write(inode, uid);
        i_gid_write(inode, gid);
    }
    /* nlink, mtime, atime, ctime are always safe to update (metadata-only,
     * no data consistency implications) */
    set_nlink(inode, nlink);
    inode_set_mtime(inode, mtime, 0);
    inode_set_atime(inode, atime, 0);
    inode_set_ctime(inode, ctime, 0);
    spin_unlock(&inode->i_lock);

    spin_lock(&pi->i_lock);
    /* Sync content_size only when we accepted the server's size (no pending
     * local modifications). */
    if (!local_pending)
        pi->content_size = size;
    /* 仅在 Filer 返回非零值时更新 volume_id/file_key.
     * close 前的 getattr 可能返回 0 (Filer 端 chunks 在 close 时才同步),
     * 若用 0 覆盖迁移后已设置的值, 会导致 writeback locate 失败 (-EINVAL). */
    if (volume_id != 0)
        pi->volume_id = volume_id;
    if (file_key != 0)
        pi->file_key = file_key;
    pi->cache_valid = true;
    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
    pi->need_refresh = false;
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: refresh_work ino=%llu size=%llu vid=%llu fkey=%llu mode=%o local_pending=%d\n",
            rw->ino, (unsigned long long)size,
            (unsigned long long)volume_id,
            (unsigned long long)file_key, mode, local_pending);

    /* 3. 失效 page cache (clean unlocked pages), 使下次读从 volume 重新拉取.
     * 使用非阻塞版本 invalidate_mapping_pages: 跳过已锁定/脏页, 不阻塞等待.
     *
     * Multi-client fix (MC-302/O-04): Previously, pagecache invalidation was
     * skipped for ALL FLAT files when local == server (K2-14), and for ALL
     * files when local != server (K2-8/K2-11). Both conditions are wrong for
     * multi-client: they prevent Client B from seeing Client A's writes.
     *
     * New behavior: only skip invalidation when the LOCAL client has pending
     * writes (dirty/writeback pages or inline_dirty). If no local writes are
     * pending, always invalidate — the data on the server is at least as new
     * as the local pagecache (either a remote client wrote, or local
     * writeback has completed and the server has the latest data).
     *
     * For FLAT files specifically: the K2-14 concern (server needle has stale
     * zeros at i_size boundary) is no longer relevant because O-03/O-10 fixes
     * now ensure the server needle is properly zeroed on extend/punch_hole.
     * The truncate-extend fix (setattr) also triggers synchronous writeback,
     * so by the time refresh_work runs, the server needle is up-to-date. */
    if (local_pending) {
        pr_debug("powerfs: refresh_work ino=%llu skip pagecache invalidate (local pending)\n",
                rw->ino);
    } else {
        invalidate_mapping_pages(inode->i_mapping, 0, (pgoff_t)-1);
    }

    /* 4. For directories, expire the readdir lease so next readdir
     * re-fetches entries from the Filer. (Moved from powerfs_invalidate_one
     * since the inode lookup is now deferred to this work function.)
     *
     * Multi-client fix (MC-201/MC-203): When a remote client unlinks or
     * rmdir's a child, the local dentry cache retains the stale dentry.
     * Even though dir_lease expiry forces readdir re-fetch, individual
     * dentry lookups (e.g. `ls <file>`) still hit the cached dentry and
     * never re-query the Filer. shrink_dcache_parent evicts unreferenced
     * child dentries so the next lookup goes to the Filer and returns
     * -ENOENT for deleted entries. Referenced dentries (open files) are
     * kept until release, which is the desired behavior. */
    if (S_ISDIR(inode->i_mode)) {
        struct dentry *dir_dentry;
        powerfs_invalidate_dir_lease(inode);
        /* Evict unreferenced child dentries so the next lookup goes to
         * the Filer instead of returning a stale cached dentry. */
        dir_dentry = d_find_any_alias(inode);
        if (dir_dentry) {
            shrink_dcache_parent(dir_dentry);
            dput(dir_dentry);
        }
    }

out_iput:
    iput(inode);
out_free:
    /* 使用 call_rcu 延迟释放: workqueue 的 worker_thread 在 work_fn 返回后
     * 仍需引用 work_struct (assign_work 等), 直接 kfree 会导致 use-after-free.
     * 与 writeback 路径 (powerfs_wb_final_cleanup) 使用相同的 call_rcu 模式. */
    call_rcu(&rw->rcu, powerfs_refresh_work_free_rcu);
}

/*
 * powerfs_invalidate_one - Invalidate one inode's caches
 *
 * Called from the powerfs-net RX path when a NOTIFY frame arrives
 * from the Filer (triggered by another client's metadata mutation).
 *
 * This function is NON-BLOCKING and safe to call from the RX dispatcher.
 * It defers ALL work (inode lookup, getattr, page cache invalidation,
 * dir lease expiry) to powerfs_refresh_wq via powerfs_refresh_inode_work.
 *
 * Why deferral is required:
 *   ilookup5 → find_inode → __wait_on_freeing_inode blocks if the inode
 *   is being freed. In the RX thread, this blocks ALL response processing
 *   (writeback completions, read responses, etc.), causing hung_task panic
 *   after 60s. By deferring to a workqueue, only the workqueue thread
 *   blocks, not the RX thread.
 *
 * NOTE: If the NOTIFY also carries ParentIno + Name fields, we prefer the
 * dentry-level path powerfs_invalidate_dentry() which knows (parent,name)
 * and can explicitly d_drop() the stale alias plus remove it from the
 * parent's dir_entries list.
 */
int powerfs_invalidate_one(u64 ino)
{
    struct powerfs_refresh_work *rw;

    /* Schedule async refresh work with inode=NULL: the work function will
     * do the ilookup5 in workqueue context, NOT in the RX dispatcher thread.
     *
     * Why: ilookup5 → find_inode → __wait_on_freeing_inode blocks if the
     * inode is being freed. In the RX thread, this blocks ALL response
     * processing (including writeback completions), causing hung_task panic
     * after 60s. Moving the lookup to a workqueue isolates the blocking. */
    rw = kmalloc(sizeof(*rw), GFP_ATOMIC);
    if (!rw) {
        pr_warn("powerfs: invalidate_one ino=%llu kmalloc failed, skipped\n", ino);
        return -ENOMEM;
    }

    INIT_WORK(&rw->work, powerfs_refresh_inode_work);
    rw->ino = ino;
    rw->inode = NULL;  /* NULL → work function will do ilookup5 */
    queue_work(powerfs_refresh_wq, &rw->work);

    pr_debug("powerfs: invalidate_one ino=%llu queued (lookup deferred to workqueue)\n", ino);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_invalidate_one);

/* ── §8b Dentry-level (parent,name,version) invalidate dispatch ──
 *
 * MC-401: 对齐 FUSE 侧 Invalidate Handler 的 dentry-level 失效能力.
 *
 * 之前 powerfs_invalidate_one 只做 inode-level (ino,version), 缺失
 * VFS dcache 的子 dentry 哈希链清理 —— 即使 inode 在 Filer 上已被
 * unlink/rename 覆盖, VFS __d_lookup 仍可能找到已缓存的旧 dentry
 * alias, 指向 stale inode 映射 (ar rcs 的 8 字节 magic-only 文件就是
 * 这种场景: 旧 libtest.a 被 rename 覆盖后, dir_entries 中还挂着
 * active entry, VFS dcache 也可能存着指向 old inode 的 hashed dentry).
 *
 * 这里新增 powerfs_invalidate_dentry(parent_ino, name, name_len, version):
 *   1. 先做 (parent,name,version) dedup: Filer 可能因网络重传 / 多副本
 *      广播同一个 notify N 次, 或 Filer 因 exclude_client_id + fallback
 *      dedup 推送重复. 仅当 version > last_seen_version 时才处理.
 *   2. 所有阻塞操作 (ilookup5 / d_find_any_alias / d_invalidate) 都放到
 *      workqueue 上 —— 与 inode-level invalidate 共享 powerfs_refresh_wq.
 *   3. workqueue 内:
 *        a) ilookup5 parent inode (可阻塞)
 *        b) 从 parent dir_entries 移除该 name 的 active entry (soft-delete
 *           标记 deleted=true, 对齐 powerfs_remove_dir_entry 语义).
 *        c) d_find_any_alias(parent) 找到父目录 dentry,
 *           从 d_subdirs/d_children 中找匹配 name 的子 dentry, 对它做
 *           d_invalidate(截断 alias inode page cache + unhash) + d_drop.
 *        d) 若找到子 inode, 异步 schedule 一次 refresh_work (通过
 *           powerfs_invalidate_one), 让其 getattr + pagecache inval.
 *   4. 失败都为 best-effort: 下次 dir lease expire / d_revalidate
 *      会再触发 Filer refetch.
 */

/* Recent processed dentry notify ring-buffer.
 * 环形哈希去重: 最多 POWERFS_DENTRY_DEDUP_ENTRIES 条,
 * 新来的 notify 若 (parent,name) 命中且 version <= recorded, 直接 drop. */
#define POWERFS_DENTRY_DEDUP_ENTRIES 256

struct powerfs_dentry_dedup_entry {
    u64 parent_ino;
    u64 version;
    size_t name_hash;     /* full_name_len 的 fast hash (pre-compute) */
    u32 name_len;
    char *name;           /* kstrdup'd name, GFP_NOIO/KERNEL */
    unsigned long jiffies_last;  /* 超长时间未命中可回收 (LRU-ish) */
};

static DEFINE_SPINLOCK(g_dentry_dedup_lock);
static struct powerfs_dentry_dedup_entry g_dentry_dedup[POWERFS_DENTRY_DEDUP_ENTRIES];
static u32 g_dentry_dedup_cursor; /* 环形 buffer cursor, 超过时覆盖最旧 */

/* Simple full_name hash (same as FUSE's processed_dentry_versions uses
 * (parent,name) key).  Use jhash: 但 Linux kernel 里 jhash 需
 * linux/jhash.h, 这里用简单的 FNV-1a 64-bit, 无依赖. */
static u64 fnv1a_hash(const u8 *buf, size_t len)
{
    u64 hash = 1469598103934665603ULL; /* FNV offset basis 64-bit */
    size_t i;
    for (i = 0; i < len; ++i) {
        hash ^= (u64)buf[i];
        hash *= 1099511628211ULL; /* FNV prime 64-bit */
    }
    return hash;
}

/*
 * dentry_dedup_check_and_record - 返回 true 表示去重命中 (应跳过)
 *
 * Caller MUST hold g_dentry_dedup_lock. 此函数可能进行 GFP_ATOMIC
 * 名称分配 (失败则放弃 dedup 直接放行, 是 best-effort). */
static bool dentry_dedup_check_and_record_locked(u64 parent_ino,
                                                  const char *name, u32 name_len,
                                                  u64 version)
{
    u64 nhash = fnv1a_hash((const u8 *)name, name_len);
    struct powerfs_dentry_dedup_entry *e;
    u32 i, free_slot = POWERFS_DENTRY_DEDUP_ENTRIES;
    unsigned long now = jiffies;

    for (i = 0; i < POWERFS_DENTRY_DEDUP_ENTRIES; ++i) {
        e = &g_dentry_dedup[i];
        if (!e->name) {
            if (free_slot == POWERFS_DENTRY_DEDUP_ENTRIES)
                free_slot = i;
            continue;
        }
        if (e->parent_ino == parent_ino &&
            e->name_len == name_len &&
            e->name_hash == nhash &&
            memcmp(e->name, name, name_len) == 0) {
            /* 命中: version <= 已记录 → 去重 */
            if (version <= e->version) {
                pr_debug("powerfs: dedup dentry_notify parent=%llu name=%.*s ver=%llu <= rec=%llu, skip\n",
                        parent_ino, name_len, name, version, e->version);
                return true;
            }
            /* version 更新: 覆盖记录, 不释放 name (相同) */
            e->version = version;
            e->jiffies_last = now;
            pr_debug("powerfs: accept newer dentry_notify parent=%llu name=%.*s ver=%llu (prev=%llu)\n",
                    parent_ino, name_len, name, version, version);
            return false;
        }
    }

    /* 未命中: 插入新条目 */
    if (free_slot == POWERFS_DENTRY_DEDUP_ENTRIES) {
        free_slot = g_dentry_dedup_cursor;
        g_dentry_dedup_cursor = (g_dentry_dedup_cursor + 1) % POWERFS_DENTRY_DEDUP_ENTRIES;
    }
    e = &g_dentry_dedup[free_slot];
    kfree(e->name);
    e->name = kstrndup(name, name_len, GFP_ATOMIC);
    if (!e->name) {
        /* 分配失败: 放弃 dedup, 放行 notify. */
        pr_warn_ratelimited("powerfs: dentry_dedup kstrndup failed (%u bytes), dedup skipped\n",
                            name_len);
        e->name_len = 0;
        e->parent_ino = 0;
        e->version = 0;
        e->name_hash = 0;
        return false;
    }
    e->parent_ino = parent_ino;
    e->name_len = name_len;
    e->name_hash = nhash;
    e->version = version;
    e->jiffies_last = now;
    return false;
}

void powerfs_dentry_dedup_destroy_all(void)
{
    u32 i;
    spin_lock(&g_dentry_dedup_lock);
    for (i = 0; i < POWERFS_DENTRY_DEDUP_ENTRIES; ++i) {
        kfree(g_dentry_dedup[i].name);
        g_dentry_dedup[i].name = NULL;
    }
    spin_unlock(&g_dentry_dedup_lock);
}

/* Work payload for async dentry-level invalidate.
 *
 * RX thread 中可以 GFP_ATOMIC 分配, 但不能阻塞等 inode.
 * workqueue 中可阻塞, 所以把 parent_ino/name/version 全带过去. */
struct powerfs_dentry_inval_work {
    struct work_struct work;
    struct rcu_head rcu;
    u64 parent_ino;
    u64 version;
    u32 name_len;
    char name[];   /* flexible array */
};

static void dentry_inval_work_free_rcu(struct rcu_head *head)
{
    struct powerfs_dentry_inval_work *w =
        container_of(head, struct powerfs_dentry_inval_work, rcu);
    kfree(w);
}

static void powerfs_dentry_inval_work_fn(struct work_struct *work)
{
    struct powerfs_dentry_inval_work *w =
        container_of(work, struct powerfs_dentry_inval_work, work);
    struct super_block *sb;
    struct inode *parent_inode;
    struct dentry *parent_dentry = NULL;

    sb = powerfs_get_sb();
    if (!sb)
        goto out_free;

    /* Workqueue 上下文, 可阻塞: ilookup5 parent */
    parent_inode = powerfs_find_inode(sb, w->parent_ino);
    if (!parent_inode) {
        pr_debug("powerfs: dentry_inval parent=%llu not in icache, skip\n",
                w->parent_ino);
        goto out_free;
    }
    if (parent_inode->i_state & (I_FREEING | I_CLEAR | I_WILL_FREE)) {
        pr_debug("powerfs: dentry_inval parent=%llu inode evicting, skip\n",
                w->parent_ino);
        goto out_iput_parent;
    }

    /* ── P1: 父目录 dir_entries 中清掉该 name (soft-delete) ──
     *
     * 与 rename 修复保持一致: 即使 VFS dcache 中没有对应子 dentry,
     * 我们自己的 dir_entries 链表也可能保留 active entry,
     * readdir 返回 stale 条目, lookup 先命中 dir_entries
     * 返回旧 inode. */
    {
        /* powerfs_remove_dir_entry 按 strcmp + !deleted 查找,
         * 无论 dcache 状态都能找到 active entry. */
        char name_nul[POWERFS_MAX_NAME_LEN + 1];
        u32 copy_len = w->name_len;
        if (copy_len > POWERFS_MAX_NAME_LEN)
            copy_len = POWERFS_MAX_NAME_LEN;
        memcpy(name_nul, w->name, copy_len);
        name_nul[copy_len] = '\0';
        pr_debug("powerfs: dentry_inval remove_dir_entry parent=%llu name=%s\n",
                w->parent_ino, name_nul);
        powerfs_remove_dir_entry(parent_inode, name_nul);
    }

    /* 父目录 dentry lease expire —— 下次 readdir 会 re-fetch */
    powerfs_invalidate_dir_lease(parent_inode);

    /* ── P2: 在 VFS dcache 中找子 dentry → d_invalidate + d_drop ──
     *
     * 需要先通过 d_find_any_alias 拿到父目录 dentry. 多 alias
     * (同一 dir inode 被多个 mount 挂载) 情形下, 我们遍历所有 alias
     * dentry, 在各 alias 的 d_children (hlist) 中查找匹配 name 的
     * child dentry.
     *
     * Linux 6.17 dcache: parent->d_children 是 hlist_head,
     * child->d_sib 是 hlist_node. 用 d_first_child/d_next_sibling
     * 遍历 (见 linux/dcache.h). */
    parent_dentry = d_find_any_alias(parent_inode);
    if (parent_dentry) {
        struct dentry *child;
        /* Loop over all child dentries using d_first_child/d_next_sibling.
         *
         * Locking: walk 需持 parent->d_lock (RCU-free version), 但
         * d_invalidate/drop 需 release lock. 所以:
         *   - 持 d_lock 读 name + dget(child);
         *   - unlock 后操作;
         *   - re-lock 再 break (单次). */
        child = d_first_child(parent_dentry);
        while (child) {
            if (child->d_name.len == w->name_len &&
                memcmp(child->d_name.name, w->name, w->name_len) == 0) {
                struct dentry *child_ref;
                struct inode *child_inode;

                child_ref = dget(child);
                child_inode = d_inode(child_ref);
                pr_debug("powerfs: dentry_inval: found child=%pd d_inode=%p ino=%lu\n",
                        child_ref, child_inode,
                        child_inode ? child_inode->i_ino : 0LU);

                /* d_invalidate: 若 child 是 positive, 这会 truncate
                 * alias inode page cache 并 unhash; 否则只 unhash. */
                d_invalidate(child_ref);
                d_drop(child_ref);
                dput(child_ref);
                break;  /* name 唯一 */
            }
            child = d_next_sibling(child);
        }
        dput(parent_dentry);
    }

    /* ── P3: (best-effort) 如果目标 inode 存在, 异步 refresh its meta
     *       + pagecache inval. 若目标 inode 不存在 (unlink/rename-over case),
     *       没什么可做的: d_drop 已发生. */
    {
        /* 没法用 inode-level notify 信息直接关联 (INO 字段可能带也可能
         * 不带). 退而求其次: 用 ilookup5 查 parent dir_entries 里被
         * soft-deleted 的 entry 关联的 ino 并 inval. 但那太绕;
         * 多数情况下 inode-level notify 也会一起推 (Filer 同时发).
         * 所以这里只打印 debug, 不强做. */
    }

out_iput_parent:
    iput(parent_inode);
out_free:
    call_rcu(&w->rcu, dentry_inval_work_free_rcu);
}

/*
 * powerfs_invalidate_dentry - 入口, RX 线程调用
 *
 * 1) 在 RX-thread 上下文做 (parent,name,version) dedup (spin_lock, GFP_ATOMIC).
 * 2) 如 dedup 未命中, 分配 + 入队 dentry_inval_work_fn.
 */
int powerfs_invalidate_dentry(u64 parent_ino, const char *name, size_t name_len, u64 version)
{
    struct powerfs_dentry_inval_work *w;
    bool skip;

    if (!name || name_len == 0 || name_len > POWERFS_MAX_NAME_LEN)
        return -EINVAL;

    /* Step 1: dedup */
    spin_lock(&g_dentry_dedup_lock);
    skip = dentry_dedup_check_and_record_locked(parent_ino, name, (u32)name_len, version);
    spin_unlock(&g_dentry_dedup_lock);
    if (skip)
        return 0;

    /* Step 2: async workqueue dispatch */
    w = kmalloc(sizeof(*w) + name_len + 1, GFP_ATOMIC);
    if (!w) {
        pr_warn_ratelimited("powerfs: invalidate_dentry kmalloc %zu failed, skip\n",
                            sizeof(*w) + name_len + 1);
        return -ENOMEM;
    }
    INIT_WORK(&w->work, powerfs_dentry_inval_work_fn);
    w->parent_ino = parent_ino;
    w->version = version;
    w->name_len = (u32)name_len;
    memcpy(w->name, name, name_len);
    w->name[name_len] = '\0';

    queue_work(powerfs_refresh_wq, &w->work);
    pr_debug("powerfs: invalidate_dentry parent=%llu name=%.*s ver=%llu queued\n",
            parent_ino, (int)name_len, name, version);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_invalidate_dentry);

/* ========== §13 Cap NOTIFY async work (Filer→Client push) ==========
 *
 * powerfs_cap_notify_work_func — 异步处理 CapRecallNotify / CapUpgradeNotify:
 *   1. ilookup5 inode (workqueue 上下文, 可阻塞)
 *   2. 按 lease_token 找到 cap
 *   3. RECALL:  更新 epoch → wire_mask→kernel_bits → powerfs_cap_revoke
 *      UPGRADE: 更新 epoch/sn → wire_mask→kernel_bits → powerfs_cap_issue
 *   4. iput + call_rcu free
 *
 * 注意: powerfs_cap_revoke 内部会临时释放 i_lock 做 flush + recall_ack
 * (同步网络 RPC), 这是正确的 (workqueue 上下文允许多次阻塞). */
static void powerfs_cap_notify_work_func(struct work_struct *work)
{
    struct powerfs_cap_notify_work *w =
        container_of(work, struct powerfs_cap_notify_work, work);
    struct super_block *sb;
    struct inode *inode;
    struct powerfs_inode_info *pi;
    struct powerfs_cap *cap;

    sb = powerfs_get_sb();
    if (!sb)
        goto out_free;

    inode = powerfs_find_inode(sb, w->ino);
    if (!inode) {
        pr_debug_ratelimited("powerfs: cap_notify kind=%d ino=%llu: no inode in cache, skip\n",
                             w->kind, w->ino);
        goto out_free;
    }

    if (inode->i_state & (I_FREEING | I_CLEAR | I_WILL_FREE)) {
        pr_debug("powerfs: cap_notify ino=%llu inode evicting, skip\n", w->ino);
        goto out_iput;
    }
    pi = POWERFS_I(inode);
    if (pi->shutdown) {
        pr_debug("powerfs: cap_notify ino=%llu pi shutdown, skip\n", w->ino);
        goto out_iput;
    }

    spin_lock(&pi->i_lock);
    cap = find_cap_by_token_locked(pi, w->lease_token, w->token_len);
    if (!cap) {
        /* i_caps 为空 (从未 open_grant), 本客户端没有持有 cap,
         * recall/upgrade 都是空操作 — 直接退出不发 ACK (服务端
         * 没有针对不存在持有者的状态跟踪, ACK 只针对有效 grant). */
        spin_unlock(&pi->i_lock);
        pr_debug_ratelimited("powerfs: cap_notify kind=%d ino=%llu: no cap found, skip\n",
                             w->kind, w->ino);
        goto out_iput;
    }

    if (w->kind == CAP_NOTIFY_RECALL) {
        unsigned int k_recall, k_retain;

        /* recall_mask = 要撤销的 wire bits; retain_mask = 撤销后仍有效的 bits.
         * k_recall = ~retain_mask 的 kernel 形式 (和 issued & ~k_recall 后得到新 issued). */
        k_retain = wire_capset_to_kernel_bits(w->body.recall.retain_mask);
        k_recall = cap->issued & ~k_retain;

        /* 更新 epoch (服务端 recall 总会递增 epoch, fencing 拦截旧 IO). */
        cap->epoch = w->body.recall.epoch;

        pr_debug("powerfs: CapRecallNotify ino=%llu wire_recall=0x%02x wire_retain=0x%02x "
                 "k_recall=0x%x epoch=%llu\n",
                 w->ino, w->body.recall.recall_mask, w->body.recall.retain_mask,
                 k_recall, (unsigned long long)w->body.recall.epoch);

        if (k_recall != 0) {
            /* powerfs_cap_revoke 内部会释放 i_lock → flush + recall_ack →
             * 重新获取 i_lock, 最后唤醒 i_cap_wq. 入参 cap 仍有效 (revoke
             * 不释放 cap, 只降级 issued). */
            powerfs_cap_revoke(pi, cap, k_recall);
        } else {
            /* 无位可撤: 不调 revoke, 但仍需唤醒等待者 (例如 check_caps
             * 在等待 cap 状态变化, 虽然没撤到位, 但 epoch 已更新). */
            wake_up_all(&pi->i_cap_wq);
        }
        spin_unlock(&pi->i_lock);
    } else if (w->kind == CAP_NOTIFY_UPGRADE) {
        unsigned int k_issued;

        k_issued = wire_capset_to_kernel_bits(w->body.upgrade.new_granted);
        cap->epoch = w->body.upgrade.epoch;
        cap->seq   = w->body.upgrade.sn;
        cap->issue_seq = w->body.upgrade.sn;

        pr_debug("powerfs: CapUpgradeNotify ino=%llu wire=0x%02x kernel=0x%x "
                 "epoch=%llu sn=%llu\n",
                 w->ino, w->body.upgrade.new_granted, k_issued,
                 (unsigned long long)w->body.upgrade.epoch,
                 (unsigned long long)w->body.upgrade.sn);

        powerfs_cap_issue(pi, cap, k_issued);
        spin_unlock(&pi->i_lock);
        wake_up_all(&pi->i_cap_wq);
    }

out_iput:
    iput(inode);
out_free:
    call_rcu(&w->rcu, powerfs_cap_notify_work_free_rcu);
}

/* --- powerfs_net layer NOTIFY 入口: 由 RX dispatcher 同步调用,
 *     只分配 work 并排队, 永不阻塞 (GFP_ATOMIC 下仍可失败降级). --- */

/* CapRecallNotify handler: 在独立线程异步 flush + revoke + ACK.
 * 对齐 Rust 服务端 cap_manager.rs recall 推送契约. */
static void powerfs_cap_recall_notify_handler(u64 ino,
            const char *lease_token, size_t token_len,
            __u8 recall_mask, __u8 retain_mask, __u64 epoch)
{
    struct powerfs_cap_notify_work *w;

    w = kmalloc(sizeof(*w), GFP_ATOMIC);
    if (!w) {
        pr_warn_ratelimited("powerfs: CapRecallNotify ino=%llu kmalloc failed, "
                            "cap will expire naturally by TTL\n", ino);
        return;
    }
    INIT_WORK(&w->work, powerfs_cap_notify_work_func);
    w->kind = CAP_NOTIFY_RECALL;
    w->ino = ino;
    if (token_len > 0 && token_len < sizeof(w->lease_token)) {
        memcpy(w->lease_token, lease_token, token_len);
        w->lease_token[token_len] = '\0';
        w->token_len = token_len;
    } else {
        w->lease_token[0] = '\0';
        w->token_len = 0;
    }
    w->body.recall.recall_mask = recall_mask;
    w->body.recall.retain_mask = retain_mask;
    w->body.recall.epoch = epoch;

    if (!powerfs_refresh_wq) {
        pr_warn("powerfs: CapRecallNotify refresh_wq not ready, free\n");
        kfree(w);
        return;
    }
    queue_work(powerfs_refresh_wq, &w->work);
}

/* CapUpgradeNotify handler: 存活 writer 被升级到 EXCLUSIVE_WRITE,
 * 异步调 cap_issue 更新 issued 位 (后续 write 可走本地缓存路径). */
static void powerfs_cap_upgrade_notify_handler(u64 ino,
            const char *lease_token, size_t token_len,
            __u8 new_granted, __u64 epoch, __u64 sn)
{
    struct powerfs_cap_notify_work *w;

    w = kmalloc(sizeof(*w), GFP_ATOMIC);
    if (!w) {
        pr_warn_ratelimited("powerfs: CapUpgradeNotify ino=%llu kmalloc failed, "
                            "degrade to SHARED_WRITE\n", ino);
        return;
    }
    INIT_WORK(&w->work, powerfs_cap_notify_work_func);
    w->kind = CAP_NOTIFY_UPGRADE;
    w->ino = ino;
    if (token_len > 0 && token_len < sizeof(w->lease_token)) {
        memcpy(w->lease_token, lease_token, token_len);
        w->lease_token[token_len] = '\0';
        w->token_len = token_len;
    } else {
        w->lease_token[0] = '\0';
        w->token_len = 0;
    }
    w->body.upgrade.new_granted = new_granted;
    w->body.upgrade.epoch = epoch;
    w->body.upgrade.sn = sn;

    if (!powerfs_refresh_wq) {
        pr_warn("powerfs: CapUpgradeNotify refresh_wq not ready, free\n");
        kfree(w);
        return;
    }
    queue_work(powerfs_refresh_wq, &w->work);
}

/*
 * powerfs_init_inode - 初始化新 inode 的字段
 *
 * 当 powerfs_iget 返回 I_NEW 状态的 inode 时，
 * 调用此函数初始化 inode 的各个字段。
 *
 * 参考 ramfs_get_inode + powerfs_new_inode 的逻辑。
 */
int powerfs_init_inode(struct inode *inode, umode_t mode,
                       u64 parent_ino, const char *name)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: init_inode ino=%lu mode=%o, S_IFDIR=%d\n",
            inode->i_ino, mode, S_ISDIR(mode));

    /* 初始化所有者和权限 */
    inode_init_owner(&nop_mnt_idmap, inode, NULL, mode);

    /* 设置 page cache 操作 */
    inode->i_mapping->a_ops = &powerfs_aops;
    mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
    mapping_set_unevictable(inode->i_mapping);

    /* 设置时间戳 */
    {
        struct timespec64 now = current_time(inode);
        inode_set_atime(inode, now.tv_sec, now.tv_nsec);
        inode_set_mtime(inode, now.tv_sec, now.tv_nsec);
        inode_set_ctime(inode, now.tv_sec, now.tv_nsec);
    }

    /* 初始化私有字段 */
    pi->parent_ino = parent_ino;
    strncpy(pi->name, name ? name : "", POWERFS_MAX_NAME_LEN - 1);
    pi->cache_valid = true;
    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
    /* dir_complete = false: looked-up directories must fetch entries from
     * Filer on first readdir. Only mkdir (new empty directory) sets this
     * to true after calling powerfs_init_inode. Setting true here causes
     * readdir to take the fast-path with an empty dir_entries list,
     * making directories appear empty after remount. */
    pi->dir_complete = false;
    pi->dir_lease_expire = 0;
    pi->dir_lease_epoch = 0;

    /* 初始化目录项链表 */
    INIT_LIST_HEAD(&pi->dir_entries);
    mutex_init(&pi->dir_mutex);

    /* 根据文件类型设置操作表 */
    switch (mode & S_IFMT) {
    case S_IFREG:
        inode->i_op = &powerfs_file_inode_operations;
        inode->i_fop = &powerfs_file_operations;
        set_nlink(inode, 1);
        pr_debug("powerfs: init_inode REG, i_fop=%p\n", inode->i_fop);
        break;

    case S_IFDIR:
        inode->i_op = &powerfs_dir_inode_operations;
        inode->i_fop = &powerfs_dir_operations;
        set_nlink(inode, 2);  /* "." + ".." */
        /* dir_complete stays false (set above). mkdir path sets it to
         * true after powerfs_init_inode returns (new empty directory). */
        pr_debug("powerfs: init_inode DIR, i_fop=%p\n", inode->i_fop);
        break;

    case S_IFLNK:
        inode->i_op = &page_symlink_inode_operations;
        inode_nohighmem(inode);
        set_nlink(inode, 1);
        break;

    default:
        init_special_inode(inode, mode, 0);
        set_nlink(inode, 1);
        break;
    }

    return 0;
}

/* ========== Inode 生命周期管理 ========== */

/*
 * inode 初始化 (slab 构造函数)
 *
 * 只初始化 powerfs 私有字段，不触碰 inode 结构本身
 * VFS 的 inode_init_once 会处理 inode 核心字段
 */
static void powerfs_inode_init_once(void *foo)
{
    struct powerfs_inode_info *pi = foo;

    inode_init_once(&pi->netfs.inode);

    /* 初始化私有字段 */
    pi->parent_ino = 0;
    pi->name[0] = '\0';
    spin_lock_init(&pi->i_lock);
    pi->cache_valid = false;
    pi->cache_expire = 0;
    pi->dir_complete = false;
}

/*
 * powerfs_alloc_inode - 分配 inode (super_operations)
 *
 * 参考 xxx_alloc_inode (fs/xxx/inode.c)
 * 使用 alloc_inode_sb 辅助函数
 */
struct inode *powerfs_alloc_inode(struct super_block *sb)
{
    struct powerfs_inode_info *pi;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    pi = alloc_inode_sb(sb, powerfs_inode_cachep, GFP_NOFS);
    if (!pi)
        return NULL;

    /* P3-5: 统计 opened_inodes + total_inodes */
    if (sbi && sbi->client) {
        percpu_counter_inc(&sbi->client->metrics.opened_inodes);
        percpu_counter_inc(&sbi->client->metrics.total_inodes);
    }

    /* netfs 初始化 (参考 xxx_alloc_inode) */
    netfs_inode_init(&pi->netfs, &powerfs_netfs_ops, false);

    /* 初始化 Lease 相关字段 */
    pi->lease_tree = RB_ROOT;
    spin_lock_init(&pi->lease_lock);
    INIT_DELAYED_WORK(&pi->lease_renew_work, powerfs_lease_renew_work_func);

    /* === Cap 管理层初始化 (对齐 xxx_alloc_inode) === */
    pi->i_caps = RB_ROOT;
    pi->i_auth_cap = NULL;
    pi->i_dirty_caps = 0;
    pi->i_flushing_caps = 0;
    INIT_LIST_HEAD(&pi->i_dirty_item);
    INIT_LIST_HEAD(&pi->i_flushing_item);
    INIT_LIST_HEAD(&pi->i_cap_delay_list);
    INIT_LIST_HEAD(&pi->i_cap_flush_list);
    pi->i_prealloc_cap_flush = NULL;
    init_waitqueue_head(&pi->i_cap_wq);
    INIT_LIST_HEAD(&pi->i_cap_snaps);
    pi->i_snap_caps = 0;
    pi->i_head_snapc_epoch = 0;

    /* Cap 引用计数清零 */
    pi->i_pin_ref = 0;
    pi->i_rd_ref = 0;
    pi->i_rdcache_ref = 0;
    pi->i_wr_ref = 0;
    pi->i_wb_ref = 0;
    pi->i_fx_ref = 0;
    pi->i_wrbuffer_ref = 0;
    pi->i_wrbuffer_ref_head = 0;
    atomic_set(&pi->i_filelock_ref, 0);

    /* shared_gen / cache_gen 初始化 (对齐 xxx i_shared_gen=0, i_rdcache_gen=0) */
    atomic_set(&pi->i_shared_gen, 0);
    pi->i_rdcache_gen = 0;
    pi->i_rdcache_revoking = 0;
    memset(pi->i_nr_by_mode, 0, sizeof(pi->i_nr_by_mode));
    pi->i_last_rd = 0;
    pi->i_last_wr = 0;

    /* Inode 版本与 flag 层 */
    pi->i_version = 0;
    pi->i_time_warp_seq = 0;
    pi->i_flags = 0;
    atomic64_set(&pi->i_release_count, 0);
    atomic64_set(&pi->i_ordered_count, 0);

    /* size/truncate 同步 (对齐  四方 size) */
    pi->i_max_size = 0;
    pi->i_reported_size = 0;
    pi->i_wanted_max_size = 0;
    pi->i_requested_max_size = 0;
    mutex_init(&pi->i_truncate_mutex);
    pi->i_truncate_seq = 0;
    pi->i_truncate_size_visible = 0;
    pi->i_xattr_version = 0;

    /* 目录递归统计 + quota + btime */
    pi->i_rbytes = 0;
    pi->i_rfiles = 0;
    pi->i_rsubdirs = 0;
    pi->i_rsnaps = 0;
    pi->i_files = 0;
    pi->i_subdirs = 0;
    pi->i_max_bytes = 0;
    pi->i_max_files = 0;
    pi->i_fragtree = RB_ROOT;
    pi->i_fragtree_nsplits = 0;
    mutex_init(&pi->i_fragtree_mutex);

    /* unsafe ops 链表 */
    INIT_LIST_HEAD(&pi->i_unsafe_dirops);
    INIT_LIST_HEAD(&pi->i_unsafe_iops);
    spin_lock_init(&pi->i_unsafe_lock);

    /* inode work (多工作项位图, 对齐  i_work/i_work_mask) */
    INIT_WORK(&pi->i_work, NULL);  /* 后续 powerfs_inode_work_fn */
    pi->i_work_mask = 0;

    pi->chunks = NULL;
    pi->chunk_count = 0;
    pi->content_size = 0;
    pi->volume_id = 0;
    pi->file_key = 0;
    pi->shutdown = false;

    /* K3: FileLayout 默认值 (placement 枚举 0=INLINE, 必须显式设 FLAT).
     * layout_chunk_size 默认 POWERFS_CHUNK_SIZE, 后续 GETATTR 可覆盖. */
    pi->placement = POWERFS_PLACEMENT_FLAT;
    pi->reliability = POWERFS_RELIABILITY_SINGLE;
    pi->reliability_state = POWERFS_RSTATE_PENDING;
    pi->layout_chunk_size = POWERFS_CHUNK_SIZE;
    pi->stripe_size = 0;
    pi->stripe_count = 0;
    pi->start_volume_idx = 0;
    pi->volume_ids = NULL;
    pi->volume_ids_count = 0;
    pi->replica_chunks = NULL;
    pi->replica_count = 0;

    /* K4-5: EC shards 列表 — 必须初始化为 NULL!
     * 修复 crash: slab 重用旧 inode 内存时, ec_chunks 残留悬空指针,
     * apply_layout_to_inode 的 kfree(old_ec) 对垃圾指针 kfree → GPF.
     * 同理 ec_data_shards/ec_parity_shards 也需初始化. */
    pi->ec_chunks = NULL;
    pi->ec_chunk_count = 0;
    pi->ec_data_shards = 0;
    pi->ec_parity_shards = 0;

    /* K2: Inline 数据缓冲初始化 (默认无 inline_data, GETATTR 时按需分配) */
    pi->inline_data = NULL;
    pi->inline_len = 0;
    pi->inline_max_size = 0;  /* GETATTR 响应覆盖, 或 apply_layout 时设默认值 */
    pi->inline_dirty = false;

    /* 异步 setattr work (writeback offload) */
    INIT_WORK(&pi->setattr_work, powerfs_setattr_work_fn);
    pi->setattr_pending = false;

    /* writeback 互斥: 防止并发 RMW 数据覆盖 */
    mutex_init(&pi->wb_mutex);
    atomic_set(&pi->wb_batch_count, 0);

    /* 初始化目录缓存字段.
     * MUST reset dir_complete/dir_lease_expire/dir_lease_epoch on every
     * alloc_inode: the slab constructor (powerfs_inode_init_once) runs only
     * on first slab page creation, NOT on object reuse. Without this reset,
     * a reused inode inherits stale dir_complete=true + future
     * dir_lease_expire from the previous inode, causing readdir to skip the
     * Filer fetch and emit an empty dir_entries list (T3b/T3c/T9a/T9b
     * "Directory not empty" failures after remount). */
    INIT_LIST_HEAD(&pi->dir_entries);
    mutex_init(&pi->dir_mutex);
    WRITE_ONCE(pi->dir_complete, false);
    WRITE_ONCE(pi->dir_lease_expire, 0);
    pi->dir_lease_epoch = 0;

    /* xattr 存储 (simple_xattr in-memory) */
    simple_xattrs_init(&pi->xattrs);

#if 0 /* DEAD_CODE — powerfs_init_mdlocks.
     * The per-inode MDLock subsystem (8 independent lock objects per inode,
     * MDLock state machine (AVAILABLE/SHARED/LONER/GATHER/EXCL), holder list,
     * GATHER list, mdlock_eval) was NEVER called from any VFS entry. Grep
     * confirms zero invocations of powerfs_mdlock_rdlock/wrlock/xlock/unlock
     * outside of the dead function bodies below. See dead-code block at
     * powerfs_fs.c ~L3341 for the full removed function definitions and
     * architecture-alignment comments (lock arbitration lives on the
     * Filer leader in lock_arbiter.rs, not on the client). */
    /* Phase 1: MDLock 独立锁对象初始化
     * 每个 inode 持有 8 把独立锁 (AUTH/LINK/XATTR/DN/SNAP/FILE/DFT/NEST)
     * 初始状态均为 AVAILABLE, 无持有者. */
    powerfs_init_mdlocks(pi);
#endif /* DEAD_CODE */

    pr_debug("powerfs: alloc_inode (pi=%p, inode=%p)\n", pi, &pi->netfs.inode);

    return &pi->netfs.inode;
}

/*
 * powerfs_free_inode - 释放 inode (super_operations)
 *
 * 参考 xxx_free_inode (fs/xxx/inode.c)
 * 只释放私有数据，inode 核心由 VFS 管理
 */
void powerfs_free_inode(struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: free_inode ino=%lu (pi=%p)\n", inode->i_ino, pi);

    /* 释放 chunk 映射 */
    kfree(pi->chunks);
    pi->chunks = NULL;

    /* K2: 释放 Inline 数据缓冲 */
    kfree(pi->inline_data);
    pi->inline_data = NULL;
    pi->inline_len = 0;
    pi->inline_dirty = false;

    /* K3-1: 释放 Stripe volume_ids 数组 */
    kfree(pi->volume_ids);
    pi->volume_ids = NULL;
    pi->volume_ids_count = 0;

    /* K4: 释放副本 chunk 列表 */
    kfree(pi->replica_chunks);
    pi->replica_chunks = NULL;
    pi->replica_count = 0;

    /* K4-5: 释放 EC shards 列表 */
    kfree(pi->ec_chunks);
    pi->ec_chunks = NULL;
    pi->ec_chunk_count = 0;

    /* xattr 存储: 释放所有 xattr 条目 */
    simple_xattrs_free(&pi->xattrs, NULL);

    kmem_cache_free(powerfs_inode_cachep, pi);
}

/*
 * powerfs_evict_inode - 驱逐 inode (super_operations)
 *
 * 参考 xxx_evict (fs/xxx/inode.c)
 *
 * 标准流程:
 *   1. truncate_inode_pages_final - 清理页面缓存
 *   2. clear_inode - 清除 inode 核心
 *   3. 清理文件系统私有资源
 */
void powerfs_evict_inode(struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);

    pr_debug("powerfs: evict_inode ino=%lu\n", inode->i_ino);

    /* P3-5: 统计 opened_inodes (与 alloc_inode 对称, total_inodes 不递减) */
    if (sbi && sbi->client)
        percpu_counter_dec(&sbi->client->metrics.opened_inodes);

    /* 1. 先截断 page cache (参考 xxx_evict_inode) */
    truncate_inode_pages_final(&inode->i_data);

    /* 2. 取消后台 lease 续约 work 和异步 setattr work
     *    （必须在 clear_inode 之前，因为 work 可能引用 inode）
     *    参考 xxx_evict_inode: cancel_writeback 是在 clear_inode 之前 */
    cancel_delayed_work_sync(&pi->lease_renew_work);
    cancel_work_sync(&pi->setattr_work);

    /* 3. 释放所有 lease (通知 volume server, 必须在 clear_inode 之前) */
    release_all_leases(inode);

    /* 4. VFS 清理 */
    clear_inode(inode);

    /* 5. 清理残留 lease (release_all_leases 批量限制, 可能有剩余) */
    while (!RB_EMPTY_ROOT(&pi->lease_tree)) {
        struct rb_node *n = rb_first(&pi->lease_tree);
        rb_erase(n, &pi->lease_tree);
        kfree(rb_entry(n, struct powerfs_lease, node));
    }

    /* 5b. 清理 i_caps rbtree (对齐 xxx_evict_inode: remove caps)
     *    evict 时所有 cap 应已被 revoke/release, 但安全起见遍历释放.
     *    必须先从 cap_lru_list 移除再释放, 否则 slab 复用内存后
     *    INIT_LIST_HEAD 会造成 cap_lru_list 链表腐败 (list_add corruption).
     *    同时修正分配器: cap 由 kmem_cache_zalloc(cap_cachep) 分配,
     *    必须用 kmem_cache_free 释放 (不能用 kfree). */
    while (!RB_EMPTY_ROOT(&pi->i_caps)) {
        struct rb_node *n = rb_first(&pi->i_caps);
        struct powerfs_cap *cap = rb_entry(n, struct powerfs_cap, ci_node);
        rb_erase(n, &pi->i_caps);
        /* 从全局 cap_lru_list 移除 (与 add_cap_for_inode_locked 对称) */
        if (sbi && sbi->client) {
            spin_lock(&sbi->client->cap_lru_lock);
            list_del_init(&cap->lru_item);
            spin_unlock(&sbi->client->cap_lru_lock);
            /* P3-5: 统计 total_caps (与 add_cap_for_inode_locked 对称) */
            atomic64_dec(&sbi->client->metrics.total_caps);
        }
        kmem_cache_free(sbi->cap_cachep, cap);
    }
    pi->i_auth_cap = NULL;

    /* 5c. 清理 cap_flush_list (不应有残留, 但安全起见) */
    while (!list_empty(&pi->i_cap_flush_list)) {
        struct powerfs_cap_flush *cf;
        cf = list_first_entry(&pi->i_cap_flush_list,
                              struct powerfs_cap_flush, i_list);
        list_del(&cf->i_list);
        list_del(&cf->g_list);
        kfree(cf);
    }

    /* 5d. 清理 cap_snaps (对齐 xxx: put all cap_snaps) */
    while (!list_empty(&pi->i_cap_snaps)) {
        struct powerfs_cap_snap *cs;
        cs = list_first_entry(&pi->i_cap_snaps,
                              struct powerfs_cap_snap, ci_item);
        list_del(&cs->ci_item);
        kfree(cs);
    }

    /* 5e. 唤醒所有等待 i_cap_wq 的线程 (对齐 xxx: wake up cap waiters) */
    wake_up_all(&pi->i_cap_wq);

#if 0 /* DEAD_CODE — powerfs_destroy_mdlocks + i_mdlock_wq.
     * See DEAD_CODE blocks in powerfs.h (i_locks[]/i_mdlock_wq) and
     * powerfs_lock.h (powerfs_mdlock struct + API decl). The function
     * body was part of the ~1400-line MDLock subsystem and is also
     * compiled out under the #if 0 block starting at powerfs_fs.c L3339.
     * The struct fields were removed from powerfs_inode_info, so these
     * identifiers no longer resolve. */
    /* 5f. 清理 MDLock 独立锁对象 (holders/gather/waiters)
     *    必须在 i_cap_wq 唤醒后, 确保等待者不会访问已释放的锁. */
    powerfs_destroy_mdlocks(pi);
    wake_up_all(&pi->i_mdlock_wq);
#endif /* DEAD_CODE */

    /* 释放动态分配的布局数据. 持锁防止与 refresh_work/apply_layout 竞争.
     * 修复 crash: 连接断开时 refresh_work getattr 失败 → iput → evict_inode,
     * 若不持锁, apply_layout 可能并发释放/替换指针, 导致 kfree 无效地址. */
    spin_lock(&pi->i_lock);

    /* 释放 chunk 映射 */
    if (pi->chunks && virt_addr_valid(pi->chunks))
        kfree(pi->chunks);
    pi->chunks = NULL;
    pi->chunk_count = 0;

    /* K3-1: 释放 Stripe volume_ids 数组 (evict 时释放, 避免 slab 重分配后悬挂) */
    if (pi->volume_ids && virt_addr_valid(pi->volume_ids))
        kfree(pi->volume_ids);
    pi->volume_ids = NULL;
    pi->volume_ids_count = 0;
    pi->stripe_size = 0;
    pi->stripe_count = 0;

    /* K4: 释放副本 chunk 列表 */
    if (pi->replica_chunks && virt_addr_valid(pi->replica_chunks))
        kfree(pi->replica_chunks);
    pi->replica_chunks = NULL;
    pi->replica_count = 0;

    /* K4-5: 释放 EC shards 列表 */
    if (pi->ec_chunks && virt_addr_valid(pi->ec_chunks))
        kfree(pi->ec_chunks);
    pi->ec_chunks = NULL;
    pi->ec_chunk_count = 0;

    /* K2: 释放 inline_data (之前遗漏, 导致内存泄漏) */
    if (pi->inline_data && virt_addr_valid(pi->inline_data))
        kfree(pi->inline_data);
    pi->inline_data = NULL;
    pi->inline_len = 0;
    pi->inline_dirty = false;

    /* 7. 清理状态 (仍在 i_lock 下) */
    pi->cache_valid = false;
    pi->dir_complete = false;
    /* Phase 1: 清目录 lease, 防止 inode 复用 (slab 重分配) 后误命中旧 lease. */
    pi->dir_lease_expire = 0;
    pi->dir_lease_epoch = 0;
    pi->shutdown = true;
    spin_unlock(&pi->i_lock);

    /* 6. 清理目录缓存链表 (使用 dir_mutex, 不在 i_lock 下) */
    powerfs_clear_dir_entries(inode);
}

/* ================================================================== *
 * Capability 管理层 — 对齐  caps.c 客户端 cap 生命周期
 *
 * 核心数据流:
 *   open()    → powerfs_cap_get_refs(RD|WR|CACHE)  → refcount++
 *   read()    → powerfs_caps_issued_mask(RDCACHE)   → 命中则走缓存
 *   write()   → mark dirty_caps |= FILE_WR          → 后台 flush
 *   release() → powerfs_cap_put_refs(had)           → 最后 ref 触发 check_caps
 *   grant     → powerfs_cap_issue(cap, issued)      → 更新 issued/implemented
 *   revoke    → powerfs_cap_revoke(cap, revoking)   → flush dirty → ack → 降级
 *   flush     → powerfs_cap_flush(mask)             → 写回 + 等 i_cap_wq
 *
 * 锁约定: cap 字段访问持 pi->i_lock (对齐  i_xxx_lock).
 *         flush/check_caps 需要发 RPC 时临时释放 i_lock.
 * ================================================================== */

/* ==================================================================
 * §13 Cap wire-format ↔ 内核 cap bits 映射
 * ==================================================================
 *
 * Filer 端 3-bit CapSet (u8) 对齐 Rust cap_manager.rs CapSet:
 *   POWERFS_NET_CAP_R (0b001) = 读缓存许可  → 内核: FILE_SHARED | FILE_CACHE | AUTH_SHARED
 *   POWERFS_NET_CAP_W (0b010) = 写缓存许可  → 内核: FILE_WR
 *   POWERFS_NET_CAP_X (0b100) = 元数据独占写 → 内核: FILE_EXCL | AUTH_EXCL | XATTR_EXCL
 *
 * 空集 (SHARED_WRITE 参与者) → 内核仅保留 PIN 位 (无本地缓存权限, IO 走同步路径).
 *
 * 设计原则: wire-format 3-bit 是最小共识, 内核扩展位 (RD/WR 独立引用计数位,
 * xattr/link 位) 可以从这 3-bit 派生出超集, 保证跨端共识 + 内核内部足够细粒度. */
static unsigned int wire_capset_to_kernel_bits(__u8 wire_caps)
{
    unsigned int k = POWERFS_CAP_PIN;  /* issued cap 总带基础引用 (和 xxx PIN 语义一致) */

    if (wire_caps & POWERFS_NET_CAP_R) {
        k |= POWERFS_CAP_AUTH_SHARED;
        k |= POWERFS_CAP_FILE_SHARED;
        k |= POWERFS_CAP_FILE_CACHE;
        k |= POWERFS_CAP_XATTR_SHARED;
        k |= POWERFS_CAP_LINK_SHARED;
    }
    if (wire_caps & POWERFS_NET_CAP_W) {
        /* CAP_W = 可本地写缓存 (write 不必 RPC), 对应内核 FILE_WR */
        k |= POWERFS_CAP_FILE_WR;
    }
    if (wire_caps & POWERFS_NET_CAP_X) {
        /* CAP_X = 可本地修改元数据 (setattr/truncate),
         * 对应 FILE_EXCL (独占写, 可追加/truncate) + AUTH_EXCL + XATTR_EXCL */
        k |= POWERFS_CAP_FILE_EXCL;
        k |= POWERFS_CAP_AUTH_EXCL;
        k |= POWERFS_CAP_XATTR_EXCL;
    }
    return k;
}

/* 反向: 内核 wanted/issued bits → 最小必要 wire_capset (用于 AcquireCap RPC,
 * 对应  CEPH_CAP_* 位 → Filer CapSet; 当前阶段 open_grant 不用此函数,
 * 后续 AcquireCap 增量请求需用到, 提前提供). */
static __u8 kernel_bits_to_wire_capset(unsigned int k_bits)
{
    __u8 w = 0;
    if (k_bits & (POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE | POWERFS_CAP_AUTH_SHARED))
        w |= POWERFS_NET_CAP_R;
    if (k_bits & POWERFS_CAP_FILE_WR)
        w |= POWERFS_NET_CAP_W;
    if (k_bits & (POWERFS_CAP_FILE_EXCL | POWERFS_CAP_AUTH_EXCL | POWERFS_CAP_XATTR_EXCL))
        w |= POWERFS_NET_CAP_X;
    return w;
}

/* 取出当前 mount 的 client_id 字符串 (长度).
 * sbi->client 若已赋 client_id, 用它; 否则退回 "powerfs-kernel-0".
 * 调用方确保 out 至少 64B 空间 (对齐 ClientId TLV size_t 最大值 255B,
 * 但实际上 client_id 远小于 64). */
static size_t get_mount_client_id(struct super_block *sb, char *out, size_t out_cap)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    const char *fallback = "powerfs-kernel-0";
    size_t flen = strlen(fallback);

    if (!out || out_cap == 0)
        return 0;
    if (sbi && sbi->client && sbi->client->client_id_len > 0 &&
        sbi->client->client_id_len < out_cap) {
        memcpy(out, sbi->client->client_id, sbi->client->client_id_len);
        out[sbi->client->client_id_len] = '\0';
        return sbi->client->client_id_len;
    }
    memcpy(out, fallback, flen);
    out[flen] = '\0';
    return flen;
}

/* 为 inode 创建并挂载一个新 cap (对齐 xxx_add_cap + xxx_get_cap_session).
 * issuer_id = 0 (单 Filer 场景, 多 Filer authority migration 场景后续扩展).
 * 调用方必须持 pi->i_lock. 返回新 cap (引用已挂到 inode 的 rbtree). */
static struct powerfs_cap *
add_cap_for_inode_locked(struct powerfs_inode_info *pi, u64 issuer_id)
{
    struct powerfs_cap *cap;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(pi->netfs.inode.i_sb);
    struct rb_node **p, *parent;

    cap = kmem_cache_zalloc(sbi->cap_cachep, GFP_ATOMIC);
    if (!cap)
        return NULL;

    cap->ci = pi;
    cap->issuer_id = issuer_id;
    cap->cap_gen = 1;  /* 会话代次, 非零 = 有效 */
    INIT_LIST_HEAD(&cap->session_caps);
    INIT_LIST_HEAD(&cap->lru_item);
    RB_CLEAR_NODE(&cap->ci_node);
    RB_CLEAR_NODE(&cap->node);

    /* 挂入 inode->i_caps (by issuer_id). PowerFS 单 filer 场景只有一个 cap,
     * 这里按  xxx_add_cap 标准形式维护 rbtree, 为 authority migration 预留. */
    p = &pi->i_caps.rb_node;
    parent = NULL;
    while (*p) {
        struct powerfs_cap *c = rb_entry(*p, struct powerfs_cap, ci_node);
        parent = *p;
        if (issuer_id < c->issuer_id)
            p = &(*p)->rb_left;
        else if (issuer_id > c->issuer_id)
            p = &(*p)->rb_right;
        else {
            /* issuer 已存在: 释放新分配, 返回已有 */
            kmem_cache_free(sbi->cap_cachep, cap);
            return c;
        }
    }
    rb_link_node(&cap->ci_node, parent, p);
    rb_insert_color(&cap->ci_node, &pi->i_caps);

    /* 首个 cap → auth_cap */
    if (!pi->i_auth_cap)
        pi->i_auth_cap = cap;

    /* 加入 client 的 cap_lru_list (shrinker 可回收) */
    if (sbi->client) {
        spin_lock(&sbi->client->cap_lru_lock);
        list_add_tail(&cap->lru_item, &sbi->client->cap_lru_list);
        spin_unlock(&sbi->client->cap_lru_lock);
        /* P3-5: 统计 total_caps */
        atomic64_inc(&sbi->client->metrics.total_caps);
    }

    return cap;
}

/* §13.3: 同步发起 CapOpenGrant RPC → 根据响应调用 cap_issue 更新 issued 位.
 *
 * 调用方**不持 pi->i_lock** (RPC 不可在 spin_lock 下做). 本函数内部
 * 短暂加锁挂载 cap + 调 cap_issue, 释放锁再发 RPC, 保持锁粒度合理.
 *
 * is_write_open = (f_mode & FMODE_WRITE) != 0
 *
 * 返回 0 成功, <0 错误 (网络错误时内核降级到无 cap: SHARED_WRITE, 不阻止 open). */
static int cap_open_grant_and_issue(struct powerfs_inode_info *pi, bool is_write_open)
{
    struct super_block *sb = pi->netfs.inode.i_sb;
    char cid[64];
    size_t cid_len;
    char token[64];
    size_t token_len = sizeof(token);
    __u8 wire_caps = 0;
    __u64 epoch = 0, sn = 0, dur_ms = 0;
    struct powerfs_cap *cap;
    unsigned int k_issued;
    int ret;

    cid_len = get_mount_client_id(sb, cid, sizeof(cid));

    /* 先发起网络 RPC (无锁环境, 可能睡眠).
     * 失败: 降级行为, open 不应因为网络不好失败, 只是本地缓存不可用. */
    ret = powerfs_net_cap_open_grant(pi->netfs.inode.i_ino,
                                     cid, is_write_open,
                                     token, &token_len,
                                     &wire_caps, &epoch, &sn, &dur_ms);
    if (ret < 0) {
        pr_warn_ratelimited("powerfs: cap_open_grant ino=%lu write=%d ret=%d: "
                            "degrade to SHARED_WRITE\n",
                            pi->netfs.inode.i_ino, (int)is_write_open, ret);
        /* 降级: wire_caps = 0 (SHARED_WRITE), 继续走 cap_issue(0) 流程以便
         * cap 对象存在 (后续 recall/renew 仍能按 inode 定位). */
        wire_caps = 0;
        token_len = 0;
        epoch = 0;
        sn = 0;
        dur_ms = 0;
        token[0] = '\0';
        ret = 0;  /* 网络失败不阻止 open 成功 */
    }

    /* 将 grant 结果灌入 inode cap. */
    spin_lock(&pi->i_lock);

    /* auth_cap 不存在 → 新建. */
    cap = pi->i_auth_cap;
    if (!cap) {
        cap = add_cap_for_inode_locked(pi, 0 /* issuer_id */);
        if (!cap) {
            spin_unlock(&pi->i_lock);
            pr_warn("powerfs: cap_open_grant ino=%lu alloc cap failed\n",
                    pi->netfs.inode.i_ino);
            return -ENOMEM;
        }
    }

    /* 记录服务端同步返回的 token / epoch / sn 信息.
     * token 用于后续 CapRecallAck / CapRelease 对服务端证明持有者身份. */
    if (token_len > 0) {
        size_t cp = min(token_len, sizeof(cap->token) - 1);
        memcpy(cap->token, token, cp);
        cap->token[cp] = '\0';
    }
    cap->epoch = epoch;
    cap->seq = sn;
    cap->issue_seq = sn;
    if (dur_ms > 0)
        cap->expire_jiffies = jiffies + msecs_to_jiffies(dur_ms);
    else
        cap->expire_jiffies = jiffies + POWERFS_LEASE_DURATION;

    cap->content_size = (u64)i_size_read(&pi->netfs.inode);

    /* 映射并调用 cap_issue: 内核 issued 只增不减, revoke 才降 */
    k_issued = wire_capset_to_kernel_bits(wire_caps);
    powerfs_cap_issue(pi, cap, k_issued);

    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: cap_open_grant ino=%lu write=%d wire=0x%02x kernel=0x%x "
             "epoch=%llu sn=%llu dur_ms=%llu\n",
             pi->netfs.inode.i_ino, (int)is_write_open, wire_caps, k_issued,
             (unsigned long long)epoch, (unsigned long long)sn, (unsigned long long)dur_ms);

    return ret;
}

/* §13.4.2 CapRecallAck 包装: 把 cap 的 token + inode + client_id 组装后 ACK 到 Filer.
 * 由 powerfs_cap_revoke() 在 flush 完后调用 (revoke 期间需 ACK).
 * 调用方**不持 pi->i_lock** (RPC 可能阻塞). */
static int cap_send_recall_ack(struct powerfs_inode_info *pi, struct powerfs_cap *cap)
{
    struct super_block *sb = pi->netfs.inode.i_sb;
    char cid[64];
    size_t cid_len;
    size_t token_len;
    int ret;

    if (!cap)
        return -EINVAL;

    token_len = strlen(cap->token);
    if (token_len == 0) {
        /* 从未成功 open_grant (例如降级态或刚 open 失败): 无需 ACK, 直接成功. */
        return 0;
    }

    cid_len = get_mount_client_id(sb, cid, sizeof(cid));

    ret = powerfs_net_cap_recall_ack(pi->netfs.inode.i_ino, cid,
                                     cap->token, token_len);
    if (ret < 0) {
        pr_warn_ratelimited("powerfs: cap_recall_ack ino=%lu ret=%d (服务端可能已完成 recall)\n",
                            pi->netfs.inode.i_ino, ret);
    }
    return ret;
}

/* §13.4 场景 3: 主动 CapRelease (close 时). 返回 HasUpgrade 结果,
 * 若 HasUpgrade=1 且 survivor 是自己 (cap 就是此 inode 的 auth_cap), 则
 * 在内部同时调 cap_issue 更新 issued 位 (从 SHARED_WRITE 升级到 EXCLUSIVE).
 * 调用方不持锁. */
static int cap_send_release(struct powerfs_inode_info *pi, struct powerfs_cap *cap)
{
    struct super_block *sb = pi->netfs.inode.i_sb;
    char cid[64];
    size_t cid_len;
    size_t token_len;
    __u8 has_upg = 0;
    char upg_token[64];
    size_t upg_toklen = sizeof(upg_token);
    __u8 upg_wire = 0;
    __u64 upg_epoch = 0, upg_sn = 0;
    int ret;

    if (!cap)
        return -EINVAL;

    token_len = strlen(cap->token);
    if (token_len == 0) {
        /* 从未成功 open_grant → 无需发 release RPC, 直接成功 (降级态). */
        return 0;
    }

    cid_len = get_mount_client_id(sb, cid, sizeof(cid));

    ret = powerfs_net_cap_release(pi->netfs.inode.i_ino, cid,
                                  cap->token, token_len,
                                  &has_upg,
                                  upg_token, &upg_toklen,
                                  &upg_wire, &upg_epoch, &upg_sn);
    if (ret < 0) {
        pr_warn_ratelimited("powerfs: cap_release ino=%lu ret=%d (服务端可能已 GC)\n",
                            pi->netfs.inode.i_ino, ret);
        return ret;
    }

    pr_debug("powerfs: cap_release ino=%lu has_upg=%d upg_wire=0x%02x\n",
             pi->netfs.inode.i_ino, (int)has_upg, upg_wire);

    /* HasUpgrade=1 说明有 survivor 升级到了 EXCLUSIVE_WRITE. 通常 survivor
     * 是"其他"客户端, NOTIFY 通道异步推送 CapUpgradeNotify 给它;
     * 若 survivor 就是自己 (最后一个 SHARED_WRITE 关闭了其他 writer),
     * release 响应体内嵌升级信息, 我们直接在本端 cap_issue 升级 issued 位,
     * 这样下一次 write_begin 走本地 FILE_WR 无需再 RPC. */
    if (has_upg) {
        unsigned int k_issued;
        spin_lock(&pi->i_lock);
        /* 升级 token (survivor 自己的新 token) */
        if (upg_toklen > 0 && upg_toklen < sizeof(cap->token)) {
            memcpy(cap->token, upg_token, upg_toklen);
            cap->token[upg_toklen] = '\0';
        }
        cap->epoch = upg_epoch;
        cap->seq = upg_sn;
        cap->issue_seq = upg_sn;
        k_issued = wire_capset_to_kernel_bits(upg_wire);
        powerfs_cap_issue(pi, cap, k_issued);
        spin_unlock(&pi->i_lock);
        wake_up_all(&pi->i_cap_wq);
    }

    return 0;
}

/* cap 有效性检查 — cap_gen 匹配且未过期.
 * 对齐  __cap_is_valid (caps.c L787).
 * 调用方持 pi->i_lock. */
bool powerfs_cap_is_valid(struct powerfs_cap *cap)
{
    if (!cap || !cap->ci)
        return false;

    /* cap_gen 不匹配 = 会话重建后旧 cap 失效 */
    if (cap->cap_gen == 0)
        return false;

    /* expire_jiffies 为 0 表示未设置过期 (永不过期), 仅靠 cap_gen 控制 */
    if (cap->expire_jiffies && time_after_eq(jiffies, cap->expire_jiffies))
        return false;

    return true;
}

/* 遍历 i_caps rbtree, 返回有效 cap 的 issued 并集.
 * 对齐  __xxx_caps_issued (caps.c L812).
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_issued(struct powerfs_inode_info *pi,
                                 unsigned int *implemented)
{
    struct powerfs_cap *cap;
    struct rb_node *p;
    unsigned int have = pi->i_snap_caps;

    if (implemented)
        *implemented = 0;

    for (p = rb_first(&pi->i_caps); p; p = rb_next(p)) {
        cap = rb_entry(p, struct powerfs_cap, ci_node);
        if (!powerfs_cap_is_valid(cap))
            continue;
        have |= cap->issued;
        if (implemented)
            *implemented |= cap->implemented;
    }

    /* 排除 auth_cap 正在 revoke 的位 (implemented & ~issued)
     * 对齐 : have &= ~cap->implemented | cap->issued */
    if (pi->i_auth_cap) {
        cap = pi->i_auth_cap;
        have &= ~cap->implemented | cap->issued;
    }

    return have;
}

/* 检查 mask 是否被 issued 完全覆盖.
 * 对齐  __xxx_caps_issued_mask (caps.c L891).
 * @touch=true 时把命中的 cap 移到 LRU 尾部 (保持 cap 热度).
 * 调用方持 pi->i_lock. */
int powerfs_caps_issued_mask(struct powerfs_inode_info *pi,
                             unsigned int mask, int touch)
{
    struct powerfs_cap *cap;
    struct rb_node *p;
    unsigned int have = pi->i_snap_caps;

    /* snap_caps 已满足 */
    if ((have & mask) == mask)
        return 1;

    for (p = rb_first(&pi->i_caps); p; p = rb_next(p)) {
        cap = rb_entry(p, struct powerfs_cap, ci_node);
        if (!powerfs_cap_is_valid(cap))
            continue;

        /* 单个 cap 满足 */
        if ((cap->issued & mask) == mask) {
            if (touch)
                cap->last_used = jiffies;
            return 1;
        }

        /* 组合满足 */
        have |= cap->issued;
        if ((have & mask) == mask) {
            if (touch) {
                struct rb_node *q;
                cap->last_used = jiffies;
                for (q = rb_first(&pi->i_caps); q != p; q = rb_next(q)) {
                    cap = rb_entry(q, struct powerfs_cap, ci_node);
                    if (!powerfs_cap_is_valid(cap))
                        continue;
                    if (cap->issued & mask)
                        cap->last_used = jiffies;
                }
            }
            return 1;
        }
    }

    return 0;
}

/* 从 refcount 派生 used caps.
 * 对齐  __xxx_caps_used (caps.c L981).
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_used(struct powerfs_inode_info *pi)
{
    struct inode *inode = &pi->netfs.inode;
    unsigned int used = 0;

    if (pi->i_pin_ref)
        used |= POWERFS_CAP_PIN;
    if (pi->i_rd_ref)
        used |= POWERFS_CAP_FILE_SHARED;
    if (pi->i_rdcache_ref ||
        (S_ISREG(inode->i_mode) && inode->i_data.nrpages))
        used |= POWERFS_CAP_FILE_CACHE;
    if (pi->i_wr_ref)
        used |= POWERFS_CAP_FILE_WR;
    if (pi->i_wb_ref || pi->i_wrbuffer_ref)
        used |= POWERFS_CAP_FILE_WR;
    if (pi->i_fx_ref)
        used |= POWERFS_CAP_FILE_EXCL;

    return used;
}

/* 从 open 模式 + 时间窗口派生 file_wanted caps.
 * 对齐  __xxx_caps_file_wanted (caps.c L1006).
 *
 * PowerFS 简化: 不区分 caps_wanted_delay_min/max (用单一 TTL),
 * 不支持 LAZY 模式. 目录/文件分别处理.
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_file_wanted(struct powerfs_inode_info *pi)
{
    struct inode *inode = &pi->netfs.inode;
    unsigned long used_cutoff = jiffies - POWERFS_DIR_LEASE_TTL;
    unsigned long idle_cutoff = jiffies - POWERFS_INODE_CACHE_TTL;

    if (S_ISDIR(inode->i_mode)) {
        unsigned int want = 0;

        /* 目录有读打开 or 最近读过 → 要 SHARED */
        if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] > 0 ||
            time_after(pi->i_last_rd, used_cutoff))
            want |= POWERFS_CAP_ANY_RD;

        /* 目录有写打开 or 最近写过 → 要 SHARED + EXCL */
        if (pi->i_nr_by_mode[POWERFS_FILE_MODE_WR] > 0 ||
            time_after(pi->i_last_wr, used_cutoff)) {
            want |= POWERFS_CAP_ANY_RD | POWERFS_CAP_FILE_EXCL;
        }

        if (want || pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] > 0)
            want |= POWERFS_CAP_PIN;

        return want;
    } else {
        unsigned int want = 0;

        /* 文件读打开 or 空闲期内读过 → 要 RD */
        if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] > 0) {
            want |= POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE;
        } else if (time_after(pi->i_last_rd, idle_cutoff)) {
            want |= POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE;
        }

        /* 文件写打开 → 要 WR + EXCL */
        if (pi->i_nr_by_mode[POWERFS_FILE_MODE_WR] > 0 ||
            time_after(pi->i_last_wr, used_cutoff)) {
            want |= POWERFS_CAP_FILE_WR | POWERFS_CAP_FILE_EXCL;
        }

        if (want)
            want |= POWERFS_CAP_PIN;

        return want;
    }
}

/* wanted = file_wanted | used, 脏数据时追加 EXCL.
 * 对齐  __xxx_caps_wanted (caps.c L1067).
 * 调用方持 pi->i_lock. */
unsigned int powerfs_caps_wanted(struct powerfs_inode_info *pi)
{
    unsigned int w = powerfs_caps_file_wanted(pi) | powerfs_caps_used(pi);

    if (S_ISDIR(pi->netfs.inode.i_mode)) {
        /* 目录有写操作 wanted → 要 EXCL (原子目录修改) */
        if (w & POWERFS_CAP_FILE_EXCL)
            w |= POWERFS_CAP_FILE_EXCL;
    } else {
        /* 文件有脏数据 → 要 EXCL (可追加/truncate) */
        if (pi->i_dirty_caps & POWERFS_CAP_ANY_DIRTY)
            w |= POWERFS_CAP_FILE_EXCL;
    }

    return w;
}

/* 内部: 获取 cap 引用计数 (调用方持 i_lock).
 * 对齐  xxx_take_cap_refs (caps.c L2761). */
static void powerfs_cap_take_refs(struct powerfs_inode_info *pi,
                                  unsigned int got)
{
    struct inode *inode = &pi->netfs.inode;

    if (got & POWERFS_CAP_PIN)
        pi->i_pin_ref++;
    if (got & POWERFS_CAP_FILE_SHARED)
        pi->i_rd_ref++;
    if (got & POWERFS_CAP_FILE_CACHE)
        pi->i_rdcache_ref++;
    if (got & POWERFS_CAP_FILE_EXCL)
        pi->i_fx_ref++;
    if (got & POWERFS_CAP_FILE_WR) {
        if (pi->i_wr_ref == 0)
            ihold(inode);
        pi->i_wr_ref++;
    }
}

/* 公共: 获取 cap 引用计数.
 * 对齐  xxx_get_cap_refs (caps.c L3185). */
void powerfs_cap_get_refs(struct powerfs_inode_info *pi, unsigned int got)
{
    spin_lock(&pi->i_lock);
    powerfs_cap_take_refs(pi, got);
    spin_unlock(&pi->i_lock);
}

/* 公共: 释放 cap 引用计数.
 * 对齐  __xxx_put_cap_refs (caps.c L3232).
 * 最后一个引用释放时触发 check_caps. */
void powerfs_cap_put_refs(struct powerfs_inode_info *pi, unsigned int had)
{
    struct inode *inode = &pi->netfs.inode;
    int last = 0;
    int put_inode = 0;

    spin_lock(&pi->i_lock);

    if (had & POWERFS_CAP_PIN)
        pi->i_pin_ref--;
    if (had & POWERFS_CAP_FILE_SHARED) {
        if (--pi->i_rd_ref == 0)
            last++;
    }
    if (had & POWERFS_CAP_FILE_CACHE) {
        if (--pi->i_rdcache_ref == 0)
            last++;
    }
    if (had & POWERFS_CAP_FILE_EXCL) {
        if (--pi->i_fx_ref == 0)
            last++;
    }
    if (had & POWERFS_CAP_FILE_WR) {
        if (--pi->i_wr_ref == 0) {
            last++;
            /* wr_ref 归零, 释放 take_refs 时持有的 inode 引用 */
            if (pi->i_wb_ref == 0)
                put_inode = 1;
        }
    }

    spin_unlock(&pi->i_lock);

    /* 最后一个引用释放 → 评估是否可归还 cap */
    if (last)
        powerfs_check_caps(pi, 0);

    /* 释放 ihold 引用 (在锁外做, 避免 AB-BA) */
    while (put_inode-- > 0)
        iput(inode);
}

/* Filer 授予 cap (grant / issue 消息处理).
 * 对齐  __check_cap_issue + xxx_add_cap 的 issue 部分.
 *
 * 更新 issued/implemented; FILE_SHARED 变化时递增 i_shared_gen
 * 并清除 I_COMPLETE (目录缓存失效, 需要重新 readdir).
 * 调用方持 pi->i_lock. */
void powerfs_cap_issue(struct powerfs_inode_info *pi, struct powerfs_cap *cap,
                       unsigned int issued)
{
    struct inode *inode = &pi->netfs.inode;
    unsigned int had;

    had = powerfs_caps_issued(pi, NULL);

    /* 更新授权位 (单调: issued 只增不减, revoke 时才降) */
    cap->issued = issued;
    /* implemented 取 issued 的超集 (保留本地仍用的位) */
    cap->implemented |= issued;

    /* 刷新过期时间 (grant 意味着 cap 有效) */
    cap->cap_gen = 1;
    cap->expire_jiffies = jiffies + POWERFS_LEASE_DURATION;

    /*
     * FILE_SHARED 新发授 → 目录缓存可能 stale, 递增 shared_gen
     * 对齐  __check_cap_issue L603-610
     */
    if (S_ISDIR(inode->i_mode) &&
        (issued & POWERFS_CAP_FILE_SHARED) &&
        !(had & POWERFS_CAP_FILE_SHARED)) {
        atomic_inc(&pi->i_shared_gen);
        pi->i_flags &= ~POWERFS_I_COMPLETE;
        pi->dir_complete = false;
    }

    /*
     * FILE_CACHE 新发授 → 递增 rdcache_gen (缓存代次)
     * 对齐  __check_cap_issue L591-595
     */
    if (S_ISREG(inode->i_mode) &&
        (issued & POWERFS_CAP_FILE_CACHE) &&
        !(had & POWERFS_CAP_FILE_CACHE)) {
        pi->i_rdcache_gen++;
    }

    /* 设置 auth_cap (首个 cap 或 issuer 匹配) */
    if (!pi->i_auth_cap)
        pi->i_auth_cap = cap;

    pr_debug("powerfs: cap_issue ino=%lu issued=0x%x had=0x%x shared_gen=%d\n",
             inode->i_ino, issued, had, atomic_read(&pi->i_shared_gen));
}

/* 服务端撤回 cap (revoke 消息处理).
 * 对齐  handle_cap_revoke 的客户端降级逻辑.
 *
 * 流程:
 *   1. issued &= ~revoking (降级授权)
 *   2. 若 dirty_caps & revoking != 0 → 需要 flush 脏数据
 *   3. flush 完成后 implemented = issued (降级生效)
 *   4. 唤醒 i_cap_wq 等待者
 *
 * 调用方持 pi->i_lock (内部临时释放以发 flush RPC). */
void powerfs_cap_revoke(struct powerfs_inode_info *pi, struct powerfs_cap *cap,
                        unsigned int revoking)
{
    struct inode *inode = &pi->netfs.inode;
    unsigned int dirty_to_flush;
    bool need_flush = false;

    /* 1. 降级 issued */
    cap->issued &= ~revoking;

    /* 2. 检查是否有脏数据需要 flush */
    dirty_to_flush = pi->i_dirty_caps & revoking;
    if (dirty_to_flush) {
        /* 将 dirty 位移到 flushing 位, 清除 dirty */
        pi->i_flushing_caps |= dirty_to_flush;
        pi->i_dirty_caps &= ~dirty_to_flush;
        need_flush = true;
    }

    pr_debug("powerfs: cap_revoke ino=%lu revoking=0x%x dirty_flush=0x%x need_flush=%d\n",
             inode->i_ino, revoking, dirty_to_flush, need_flush);

    if (need_flush) {
        /* 临时释放锁发 flush RPC (flush 内部自行加锁) */
        spin_unlock(&pi->i_lock);
        powerfs_cap_flush(pi, dirty_to_flush);
        spin_lock(&pi->i_lock);
    }

    /* 3. flush 完成后 implemented = issued (降级生效) */
    cap->implemented = cap->issued;

    /* 4. 如果 FILE_SHARED 被撤, 目录缓存失效 */
    if (revoking & POWERFS_CAP_FILE_SHARED) {
        atomic_inc(&pi->i_shared_gen);
        pi->i_flags &= ~POWERFS_I_COMPLETE;
        pi->dir_complete = false;
    }

    /* 5. §13.4.2: 发 CapRecallAck 到 Filer, 证明 flush 完成 + issued 已降级.
     *    服务端收到 ACK 才会完成 recall 流程, 将 EXCLUSIVE 权限授予新申请者.
     *    注意: RPC 不能在 spinlock 下执行, 临时释放 i_lock (此时已无 shared
     *    state 与其他路径竞态, issued/implemented 已落盘). */
    if (1) {  /* recall 总是需要 ACK, 不管有没有 flush (服务端统一状态机) */
        spin_unlock(&pi->i_lock);
        cap_send_recall_ack(pi, cap);
        spin_lock(&pi->i_lock);
    }

    /* 6. 唤醒等待者 */
    wake_up_all(&pi->i_cap_wq);
}

/* 写回 dirty_caps 并等待 ACK.
 * 对齐  xxx_flush_dirty_caps + __send_cap.
 *
 * 流程:
 *   1. 分配 powerfs_cap_flush 记录, 挂到 i_cap_flush_list + 全局 cap_flush_list
 *   2. 将 dirty_caps 移到 flushing_caps
 *   3. 发送 CapFlush RPC 到 Filer (TODO: 接入 powerfs_net 层)
 *   4. 等待 i_cap_wq 唤醒 (ACK 回调唤醒)
 *
 * 调用方不持锁. */
int powerfs_cap_flush(struct powerfs_inode_info *pi, unsigned int mask)
{
    struct inode *inode = &pi->netfs.inode;
    struct powerfs_cap_flush *cf;
    struct powerfs_sb_info *sbi;
    unsigned int flushing;
    unsigned int need_data_flush = 0;
    unsigned int need_attr_flush = 0;
    unsigned int need_xattr_flush = 0;
    unsigned int need_inline_flush = 0;
    int ret = 0;

    sbi = POWERFS_SB_INFO(inode->i_sb);

    spin_lock(&pi->i_lock);

    /* 取 dirty_caps 与 mask 的交集 (需要 flush 的位) */
    flushing = pi->i_dirty_caps & mask;
    if (!flushing) {
        spin_unlock(&pi->i_lock);
        return 0;
    }

    /* 分类需要实际推到后端的脏类型:
     *   WR_DATA → VFS page cache writeback
     *   AUTH_EXCL → setattr (uid/gid/mode/time/size)
     *   XATTR_EXCL → 扩展属性 setxattr/removexattr 全量推送
     *   inline_dirty → INLINE placement inline_data payload 同步
     *
     * 拷贝 flushing 分类后释放 i_lock (I/O 期间不能持自旋锁),
     * 用 cf 作为 flushing record, 结束后回到 i_lock 清理状态. */
    if (flushing & POWERFS_CAP_WR_DATA) {
        need_data_flush = flushing & POWERFS_CAP_WR_DATA;
        /* INLINE placement: 除了 page cache, 还需同步 inline_data payload.
         * 若 inline_dirty, FLAT placement 用不到该标志位. */
        if (pi->placement == POWERFS_PLACEMENT_INLINE && pi->inline_dirty)
            need_inline_flush = 1;
    }
    if (flushing & POWERFS_CAP_AUTH_EXCL)
        need_attr_flush = flushing & POWERFS_CAP_AUTH_EXCL;
    if (flushing & POWERFS_CAP_XATTR_EXCL)
        need_xattr_flush = POWERFS_CAP_XATTR_EXCL;

    /* 移到 flushing_caps */
    pi->i_dirty_caps &= ~flushing;
    pi->i_flushing_caps |= flushing;

    /* 分配 flush 记录 (优先用预分配槽) */
    if (pi->i_prealloc_cap_flush) {
        cf = pi->i_prealloc_cap_flush;
        pi->i_prealloc_cap_flush = NULL;
    } else {
        cf = kmem_cache_alloc(sbi->cap_flush_cachep, GFP_NOFS);
        if (!cf) {
            /* 内存不足: 回滚, dirty 保留, 下次再推 (宁可推两遍也不丢脏). */
            pi->i_dirty_caps |= flushing;
            pi->i_flushing_caps &= ~flushing;
            spin_unlock(&pi->i_lock);
            return -ENOMEM;
        }
    }

    cf->tid = atomic64_inc_return(&pi->i_release_count);
    cf->caps = flushing;
    cf->wake = true;
    cf->is_capsnap = false;
    INIT_LIST_HEAD(&cf->g_list);
    INIT_LIST_HEAD(&cf->i_list);
    list_add_tail(&cf->i_list, &pi->i_cap_flush_list);

    spin_unlock(&pi->i_lock);

    /* =====================================================================
     * 以下流程不持 pi->i_lock, 允许阻塞 I/O + 网络 RPC.
     * 若任何步骤失败: 失败的脏位放回 pi->i_dirty_caps, 下次重试.
     * 成功的位: 留在 flushing → cap_revoke 已发送 ACK 前不会重复.
     * ===================================================================== */

    /* --- Step 1: WR_DATA → 同步写回 page cache 脏页到 Volume/Filer ---
     *   filemap_write_and_wait_range 触发:
     *     netfs_writepages → netfs_write_block → powerfs_net_write(Volume)
     *   成功后再在 release/fsync 路径 UPDATE_INODE_SIZE_CHUNKS 原子提交. */
    if (need_data_flush) {
        loff_t size = i_size_read(inode);
        int err;

        pr_debug("powerfs: cap_flush WR_DATA ino=%lu size=%lld caps=0x%x\n",
                 inode->i_ino, size, need_data_flush);
        err = filemap_write_and_wait_range(inode->i_mapping, 0,
                                           size > 0 ? size - 1 : 0);
        if (err < 0) {
            pr_warn_ratelimited("powerfs: cap_flush WR_DATA ino=%lu failed: %d, will retry\n",
                                inode->i_ino, err);
            /* 失败: 把 WR_DATA 脏位放回 i_dirty_caps, 下次 check_caps/recall 再推 */
            spin_lock(&pi->i_lock);
            pi->i_dirty_caps    |= need_data_flush;
            pi->i_flushing_caps &= ~need_data_flush;
            spin_unlock(&pi->i_lock);
            ret = err;
        }
    }

    /* --- Step 2: AUTH_EXCL → setattr 同步 inode 元数据到 Filer ---
     *   Filer 的 setattr RPC (0x0030 或 UPDATE_INODE_SIZE_CHUNKS) 已在
     *   powerfs_net_setattr 中实现. 这里收集当前 inode 的最新值:
     *   mode/uid/gid/size/mtime/atime → 组装 mode_valid bit → RPC. */
    if (need_attr_flush) {
        __u32 valid = 0;
        __u32 mode = 0, uid = 0, gid = 0;
        __u64 size = 0;
        __u64 mtime = 0, atime = 0;
        int err;

        /* 读 inode 当前属性 (持 inode->i_lock? 我们不需要锁, 快照即可;
         * setattr_prepare 已在 VFS setattr 回调中持有, 此处只是采样同步) */
        mode = inode->i_mode;
        uid  = i_uid_read(inode);
        gid  = i_gid_read(inode);
        size = i_size_read(inode);
        {
            struct timespec64 ts = inode_get_mtime(inode);
            mtime = (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
        }
        {
            struct timespec64 ts = inode_get_atime(inode);
            atime = (__u64)ts.tv_sec * 1000000000ULL + (__u64)ts.tv_nsec;
        }
        /* valid 表示哪些字段要推 (POWERFS_ATTR_* 协议 bit 枚举, 见 powerfs_net.h):
         *   AUTH_EXCL 意味着属性可能变更, 为保守全量推 mode/uid/gid/size/mtime.
         *   atime 只有在显式修改时才脏, 但我们不细区分, 一起推 (对 RPC 性能影响小) */
        valid = POWERFS_ATTR_MODE | POWERFS_ATTR_UID | POWERFS_ATTR_GID |
                POWERFS_ATTR_SIZE | POWERFS_ATTR_MTIME | POWERFS_ATTR_ATIME;

        pr_debug("powerfs: cap_flush AUTH_EXCL ino=%lu valid=0x%x mode=0%o size=%llu\n",
                 inode->i_ino, valid, mode & 07777, (unsigned long long)size);
        err = powerfs_net_setattr(inode->i_ino, valid, mode, uid, gid,
                                  size, mtime, atime);
        if (err < 0) {
            pr_warn_ratelimited("powerfs: cap_flush AUTH_EXCL ino=%lu failed: %d, will retry\n",
                                inode->i_ino, err);
            spin_lock(&pi->i_lock);
            pi->i_dirty_caps    |= need_attr_flush;
            pi->i_flushing_caps &= ~need_attr_flush;
            spin_unlock(&pi->i_lock);
            ret = ret ?: err;
        }
    }

    /* --- Step 3: XATTR_EXCL → xattr 脏数据推 Filer (兜底) ---
     *   正常路径: xattr_handler_set/remove 已同步 net RPC, XATTR_EXCL
     *   标记为脏仅是为了在 recall 时让 CapRevoke 等待该版本号.
     *   兜底场景: 若未来优化为"持 XATTR_EXCL 时先只写 L1 cache, recall 时
     *   cap_flush 统一推送", 则本步骤会真正用到.
     *
     *   实现策略: 遍历 L1 simple_xattr 缓存中的所有键值对, 逐条
     *   net setxattr 重推. simple_xattr 用 rb_root, 无公开遍历 API,
     *   所以先用 simple_xattr_list(NULL probe → buffer) 枚举 keys,
     *   再逐个 simple_xattr_get 拿 value → net setxattr.
     *
     *   任何单条失败 → 整体 XATTR_EXCL 放回脏位, 下次重试. */
    if (need_xattr_flush) {
        __u64 shard_id = powerfs_calc_shard_id(inode->i_ino);
        ssize_t list_sz;
        char *list_buf = NULL;
        int step_err = 0;

        /* Step 3.1: probe 需要的 list buffer 尺寸 */
        list_sz = simple_xattr_list(inode, &pi->xattrs, NULL, 0);
        if (list_sz < 0) {
            step_err = (int)list_sz;
            goto xattr_flush_fail;
        }
        if (list_sz == 0) {
            /* 无 xattr → 视为成功 (没有脏数据需要推送, 说明 XATTR_EXCL 脏位
             * 对应的是刚刚在 xattr_handler_set 里同步 RPC 过且 simple_xattr
             * 清掉了条目 (remove) 的场景. */
            goto xattr_flush_ok;
        }

        list_buf = kmalloc((size_t)list_sz, GFP_NOFS);
        if (!list_buf) {
            step_err = -ENOMEM;
            goto xattr_flush_fail;
        }
        {
            ssize_t got = simple_xattr_list(inode, &pi->xattrs, list_buf, (size_t)list_sz);
            if (got != list_sz) {
                step_err = (got < 0) ? (int)got : -EIO;
                goto xattr_flush_fail_cleanup;
            }
        }

        /* Step 3.2: 遍历 NUL-separated keys, 逐条 push */
        {
            size_t pos = 0;
            while (pos < (size_t)list_sz) {
                const char *key = list_buf + pos;
                size_t klen = strlen(key);
                ssize_t vsize;
                __u8 *vbuf;

                /* probe value 尺寸 */
                vsize = simple_xattr_get(&pi->xattrs, key, NULL, 0);
                if (vsize < 0) {
                    /* 某条拿不到 → 不视为失败 (可能并发删除), 跳过 */
                    pos += klen + 1;
                    continue;
                }
                vbuf = kmalloc((size_t)(vsize > 0 ? vsize : 1), GFP_NOFS);
                if (!vbuf) {
                    step_err = -ENOMEM;
                    goto xattr_flush_fail_cleanup;
                }
                if (vsize > 0) {
                    ssize_t gotv = simple_xattr_get(&pi->xattrs, key, vbuf, (size_t)vsize);
                    if (gotv != vsize) {
                        kfree(vbuf);
                        step_err = (gotv < 0) ? (int)gotv : -EIO;
                        goto xattr_flush_fail_cleanup;
                    }
                }
                {
                    int r = powerfs_net_setxattr(shard_id, inode->i_ino,
                                                 key, klen,
                                                 vbuf, (size_t)(vsize > 0 ? vsize : 0));
                    kfree(vbuf);
                    if (r < 0) {
                        step_err = r;
                        goto xattr_flush_fail_cleanup;
                    }
                }
                pos += klen + 1;
            }
        }

xattr_flush_ok:
        kfree(list_buf);
        pr_debug("powerfs: cap_flush XATTR_EXCL ino=%lu OK\n", inode->i_ino);
        goto xattr_flush_done;

xattr_flush_fail_cleanup:
        kfree(list_buf);
xattr_flush_fail:
        pr_warn_ratelimited("powerfs: cap_flush XATTR_EXCL ino=%lu failed: %d, rollback dirty\n",
                            inode->i_ino, step_err);
        spin_lock(&pi->i_lock);
        pi->i_dirty_caps    |= need_xattr_flush;
        pi->i_flushing_caps &= ~need_xattr_flush;
        spin_unlock(&pi->i_lock);
        ret = ret ?: step_err;
xattr_flush_done:
        (void)shard_id;  /* shard_id 已在循环内通过 calc 使用, 这里抑制 "set but not used" */
    }

    /* --- Step 4: INLINE placement inline_data dirty flush ---
     *   inline_data is set during CreateInode (Phase A) and updated by
     *   write_begin/write_end. There is no standalone UpdateInline RPC;
     *   we reuse powerfs_net_update_inode_size_chunks with inline_data
     *   payload — same approach as the release() path (see L9784+).
     *
     *   Without this, a CapRecallNotify on an inline file flushes page
     *   cache (Step 1) and sends ACK (cap_send_recall_ack), but the
     *   inline_data payload stays stale on the Filer. The server then
     *   promotes a waiting client that reads the stale inline_data,
     *   causing silent data loss — identical to the FUSE L4.21 bug. */
    if (need_inline_flush) {
        __u8 *snap_data = NULL;
        __u32 snap_len;
        __u64 shard_id;
        int attempt;
        bool inline_synced = false;

        /* Snapshot inline_data under lock (network I/O cannot hold spinlock) */
        spin_lock(&pi->i_lock);
        if (!pi->inline_data || pi->inline_len == 0) {
            pi->inline_dirty = false;
            spin_unlock(&pi->i_lock);
            pr_warn_ratelimited("powerfs: cap_flush INLINE ino=%lu dirty but no data\n",
                                inode->i_ino);
            goto inline_flush_done;
        }
        snap_len = pi->inline_len;
        spin_unlock(&pi->i_lock);

        snap_data = kmalloc(snap_len, GFP_NOFS);
        if (!snap_data) {
            pr_warn_ratelimited("powerfs: cap_flush INLINE ino=%lu kmalloc %u failed\n",
                                inode->i_ino, snap_len);
            /* Keep inline_dirty; next recall/fsync will retry */
            spin_lock(&pi->i_lock);
            pi->inline_dirty = true;
            spin_unlock(&pi->i_lock);
            ret = ret ?: -ENOMEM;
            goto inline_flush_done;
        }

        spin_lock(&pi->i_lock);
        if (pi->inline_data && pi->inline_len == snap_len) {
            memcpy(snap_data, pi->inline_data, snap_len);
        } else {
            /* Concurrent modification — abandon this snapshot */
            spin_unlock(&pi->i_lock);
            pr_warn_ratelimited("powerfs: cap_flush INLINE ino=%lu data changed during snapshot\n",
                                inode->i_ino);
            kfree(snap_data);
            spin_lock(&pi->i_lock);
            pi->inline_dirty = true;
            spin_unlock(&pi->i_lock);
            ret = ret ?: -EAGAIN;
            goto inline_flush_done;
        }
        spin_unlock(&pi->i_lock);

        shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : inode->i_ino);

        /* Retry loop: 5 attempts, 500ms×attempt backoff (covers Raft election) */
        for (attempt = 1; attempt <= 5; attempt++) {
            int r = powerfs_net_update_inode_size_chunks(shard_id, inode->i_ino,
                                                         (__u64)snap_len,
                                                         "kernel",
                                                         NULL, 0,
                                                         snap_data, snap_len);
            if (r == 0) {
                inline_synced = true;
                pr_debug("powerfs: cap_flush INLINE ino=%lu synced size=%u (attempt %d)\n",
                        inode->i_ino, snap_len, attempt);
                break;
            }
            pr_warn_ratelimited("powerfs: cap_flush INLINE ino=%lu attempt %d failed: %d\n",
                                inode->i_ino, attempt, r);
            if (attempt < 5)
                msleep(500 * attempt);
        }

        kfree(snap_data);

        if (inline_synced) {
            spin_lock(&pi->i_lock);
            pi->inline_dirty = false;
            spin_unlock(&pi->i_lock);
        } else {
            /* Keep inline_dirty for next recall/fsync/close retry */
            spin_lock(&pi->i_lock);
            pi->inline_dirty = true;
            spin_unlock(&pi->i_lock);
            ret = ret ?: -EIO;
        }
    }
inline_flush_done:

    /* =====================================================================
     * 清理: 回到 i_lock, 把成功的 flushing 位移出 i_flushing_caps.
     * 如果所有步骤 ret == 0 (全部成功): 正常清 flushing + free cf.
     * 如果有失败: 失败脏位已在各 Step 中放回 i_dirty_caps.
     *   仍需把剩下的成功位对应的 flushing 清除. */
    spin_lock(&pi->i_lock);
    {
        unsigned int remain = pi->i_flushing_caps;
        pi->i_flushing_caps = 0; /* flushing window 已结束 */
        /* 注意: 若任何子步骤失败, 对应 dirty 位已在该 Step 内放回 i_dirty_caps.
         * 我们只需清 flushing, 那些脏位会在后续 check_caps/recall 中再次 flush.
         * (若 remain != 0 说明有的脏位没回滚但 RPC 失败: 安全起见不要清 dirty,
         *  但我们已经在每个分支都回滚了, remain 理论上 == 0) */
        (void)remain;
    }
    list_del(&cf->i_list);

    /* 回收 cf: 挂预分配槽 or kmem_cache_free */
    if (!pi->i_prealloc_cap_flush)
        pi->i_prealloc_cap_flush = cf;
    else
        kmem_cache_free(sbi->cap_flush_cachep, cf);

    spin_unlock(&pi->i_lock);

    /* 唤醒等待 cap 刷新的 get_caps / revoke 线程 */
    wake_up_all(&pi->i_cap_wq);

    if (ret == 0)
        pr_debug("powerfs: cap_flush success ino=%lu caps=0x%x tid=%llu\n",
                 inode->i_ino, flushing, cf->tid);
    return ret;
}

/* 评估并可能发送 cap 状态更新.
 * 对齐  xxx_check_caps (caps.c L2018).
 *
 * 比较 wanted vs issued:
 *   - wanted > issued → 发 AcquireCap (请求更多权限)
 *   - wanted < issued → 发 ReleaseCap (归还多余权限)
 *   - dirty_caps 非空 → 触发 flush
 *
 * 当前阶段: 仅记录日志, 实际 RPC 发送待接入 net 层.
 * 调用方不持锁. */
void powerfs_check_caps(struct powerfs_inode_info *pi, int flags)
{
    struct inode *inode = &pi->netfs.inode;
    unsigned int issued, implemented, wanted, used, file_wanted;
    unsigned int revoking;

    spin_lock(&pi->i_lock);

    issued = powerfs_caps_issued(pi, &implemented);
    revoking = implemented & ~issued;
    used = powerfs_caps_used(pi);
    file_wanted = powerfs_caps_file_wanted(pi);
    wanted = file_wanted | used;

    pr_debug("powerfs: check_caps ino=%lu want=0x%x used=0x%x issued=0x%x "
             "impl=0x%x revoke=0x%x dirty=0x%x flush=0x%x\n",
             inode->i_ino, wanted, used, issued, implemented, revoking,
             pi->i_dirty_caps, pi->i_flushing_caps);

    /* FLUSH 标志: 有脏数据时立即 flush */
    if ((flags & POWERFS_CHECK_CAPS_FLUSH) && pi->i_dirty_caps) {
        spin_unlock(&pi->i_lock);
        powerfs_cap_flush(pi, pi->i_dirty_caps);
        return;
    }

    /* TODO: 接入 net 层后, 根据 wanted vs issued 决定:
     *   - wanted & ~issued → 发送 AcquireCap RPC
     *   - issued & ~wanted & ~used → 发送 ReleaseCap RPC
     *   - revoking & ~dirty → 发送 CapAck 确认降级
     * 当前阶段仅日志, 不实际发送. */

    spin_unlock(&pi->i_lock);
}

/* 标记 cap dirty 位 (write/setattr 路径调用).
 * 对齐  __xxx_mark_caps_dirty.
 * @caps: 要标记的 dirty 位掩码 (POWERFS_CAP_FILE_WR / POWERFS_CAP_AUTH_EXCL 等).
 * 调用方不持锁. */
void powerfs_cap_mark_dirty(struct powerfs_inode_info *pi, unsigned int caps)
{
    spin_lock(&pi->i_lock);
    pi->i_dirty_caps |= caps;
    spin_unlock(&pi->i_lock);
}

/*
 * 本地 try_get_cap_refs 辅助: 若 issued 覆盖 need 且想拿的 want 也在 issued/used
 * 允许范围内, 调 cap_get_refs 递增引用计数, 返回 true(成功).
 * 调用方必须持有 pi->i_lock. 对 FILE 路径, used_mode = (need|want) &
 * (CAP_PIN | CAP_FILE_SHARED | CAP_FILE_CACHE | CAP_FILE_WR | CAP_FILE_EXCL).
 */
static bool try_get_cap_refs_locked(struct powerfs_inode_info *pi,
                                    unsigned int need, unsigned int want,
                                    unsigned int *got)
{
    unsigned int issued, impl;
    unsigned int refs = 0;
    bool have_need, have_want;

    issued = powerfs_caps_issued(pi, &impl);

    have_need = (need & ~issued) == 0;
    have_want = ((want & (POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE |
                          POWERFS_CAP_FILE_WR | POWERFS_CAP_FILE_EXCL)) & ~issued) == 0;

    /* 只有至少满足 need 时才增加引用计数, 否则 *got=0 不占引用.
     * 防止 get_caps 阻塞循环里反复加引用造成泄漏. */
    if (have_need) {
        refs |= POWERFS_CAP_PIN;
        if (issued & POWERFS_CAP_FILE_SHARED)
            refs |= POWERFS_CAP_FILE_SHARED;
        if (issued & POWERFS_CAP_FILE_CACHE)
            refs |= POWERFS_CAP_FILE_CACHE;
        if (issued & POWERFS_CAP_FILE_WR)
            refs |= POWERFS_CAP_FILE_WR;
        if (issued & POWERFS_CAP_FILE_EXCL)
            refs |= POWERFS_CAP_FILE_EXCL;
        /* FIX SPINLOCK RE-ENTRY DEADLOCK: callers of
         * try_get_cap_refs_locked already hold pi->i_lock (see
         * powerfs_try_get_caps / powerfs_get_caps call sites).  The
         * public powerfs_cap_get_refs() does spin_lock(&pi->i_lock)
         * itself which would deadlock on the same owner.  Use the
         * internal _locked variant powerfs_cap_take_refs() instead. */
        powerfs_cap_take_refs(pi, refs);
    }

    if (got)
        *got = refs;

    return have_need && have_want;
}

/*
 * powerfs_try_get_caps - 非阻塞获取 cap 引用 (对齐 xxx_try_get_caps).
 *
 * 若已持有 >= need 的 issued 位, cap_get_refs 增加引用并返回 true;
 * 否则立即触发 powerfs_check_caps(0) 向服务端申请, 返回 false.
 * 调用方可以基于返回值选择:
 *   - true:  完整拿到 need+want, 可安全走本地缓存 IO.
 *   - false: need 不满足 或 want 未完全覆盖, 降级走直读直写 (带 PIN 引用, 不阻塞).
 */
bool powerfs_try_get_caps(struct powerfs_inode_info *pi,
                          unsigned int need, unsigned int want,
                          bool nonblock, unsigned int *got)
{
    bool ok;
    unsigned int refs = 0;

    spin_lock(&pi->i_lock);
    ok = try_get_cap_refs_locked(pi, need, want, &refs);
    spin_unlock(&pi->i_lock);

    if (!ok) {
        /* 触发向服务端 AcquireCap RPC (当前 check_caps 仅日志; 接入 net 后自动生效). */
        powerfs_check_caps(pi, 0);
    }

    if (got)
        *got = refs;
    return ok;
}

/*
 * powerfs_get_caps - 阻塞获取 cap 引用 (对齐 xxx_get_caps / __xxx_get_caps).
 *
 * 循环:
 *   1. try_get_cap_refs_locked → 成功直接返回.
 *   2. 否则 DEFINE_WAIT + add_wait_queue(i_cap_wq) → wait_woken 最多 50ms.
 *   3. 期间若 pending 信号, 返回 -ERESTARTSYS.
 *   4. 连续等待超过 30 轮 (≈1.5s) 仍未到位, 降级宽松模式: 只要拿到 need 就
 *      返回 (避免 cap 协议未就绪期间卡死 IO).
 *
 * @inode: 目标 inode.
 * @filp:  可选文件描述符 (当前仅用于将来 mode 追踪).
 * @need:  必须位 (如读 = FILE_SHARED, 写 = FILE_SHARED | FILE_WR).
 * @want:  期望位 (如 FILE_CACHE | FILE_EXCL).
 * @endoff:写路径的写入偏移 (当前未用, 预留做 size 限制校验).
 * @got:   输出实际拿到的引用位 (后续 put_refs 对称释放用).
 * 返回 0 成功, <0 错误码.
 */
int powerfs_get_caps(struct inode *inode, struct file *filp,
                     unsigned int need, unsigned int want,
                     loff_t endoff, unsigned int *got)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);
    const int max_rounds = 30;   /* 最大轮次 ≈ 50ms * 30 = 1.5s */
    int round = 0;
    unsigned int refs = 0;
    bool ok;

    for (;;) {
        DEFINE_WAIT_FUNC(wait, woken_wake_function);

        spin_lock(&pi->i_lock);
        ok = try_get_cap_refs_locked(pi, need, want, &refs);
        spin_unlock(&pi->i_lock);

        if (ok) {
            if (got) *got = refs;
            if (sbi && sbi->client) {
                if (round == 0)
                    powerfs_metric_cap_hit(&sbi->client->metrics);
                else
                    powerfs_metric_cap_mis(&sbi->client->metrics);
            }
            return 0;
        }
        if (round++ >= max_rounds) {
            /* 超时降级: 如果至少拿到了 need 位, 就放行 (refs 在上面已递增).
             * 如果连 need 都没拿到, 那也放行但 refs 可能只有 PIN,
             * 交给上层 IO 路径自己处理 (直读直写 / 返回错误). */
            if (got) *got = refs;
            if (sbi && sbi->client)
                powerfs_metric_cap_mis(&sbi->client->metrics);
            return 0;
        }

        /* 先 check_caps 催一下 AcquireCap (幂等). */
        powerfs_check_caps(pi, 0);

        if (signal_pending(current))
            return -ERESTARTSYS;

        add_wait_queue(&pi->i_cap_wq, &wait);
        set_current_state(TASK_INTERRUPTIBLE);

        /* 再做一次 double-check, 防止 grant 发生在 add_wait_queue 前被漏唤醒. */
        spin_lock(&pi->i_lock);
        ok = try_get_cap_refs_locked(pi, need, want, &refs);
        spin_unlock(&pi->i_lock);
        if (ok) {
            __set_current_state(TASK_RUNNING);
            remove_wait_queue(&pi->i_cap_wq, &wait);
            if (got) *got = refs;
            return 0;
        }

        if (signal_pending(current)) {
            __set_current_state(TASK_RUNNING);
            remove_wait_queue(&pi->i_cap_wq, &wait);
            return -ERESTARTSYS;
        }

        /* 50ms 超时: 配合 30 轮, 最多等 1.5s. */
        schedule_timeout(msecs_to_jiffies(50));
        remove_wait_queue(&pi->i_cap_wq, &wait);
    }
}

#if 0 /* DEAD_CODE — MDLock subsystem (Ceph MDS Locker on client).
     *
     * = ARCHITECTURE MISPLACEMENT =
     * The ~1400 lines below implement a complete Ceph MDS Locker state
     * machine (SimpleLock + ScatterLock + FileLock + LocalLock classes;
     * states AVAILABLE/SHARED/LONER/EXCL/GATHER/REVOKING; per-lock
     * holder list + gather list; mdlock_eval() driving Loner promotion
     * and GATHER barriers; per-type → cap-bit maps). This is the KIND
     * of logic that lives on the Filer leader, in lock_arbiter.rs — the
     * server-side single authority that grants/revokes caps. It was
     * WRONGLY placed inside the kernel client.
     *
     * Why it compiled but never ran:
     *  • The entry points (powerfs_mdlock_rdlock/wrlock/xlock/unlock)
     *    are defined here but grep found ZERO invocations from any VFS
     *    path (lookup/open/create/unlink/setattr/setxattr/readdir/…).
     *    The actual per-file cap negotiation goes through the separate
     *    powerfs_net_cap_* path (MsgType 0x91~0x95 on powerfs-net) that
     *    talks to the REAL lock_arbiter.rs on the Filer leader.
     *  • The inode field `i_locks[]` that these functions operated on
     *    is also being disabled in struct powerfs_inode_info in
     *    powerfs.h (DEAD_CODE block there). The two sides were an
     *    island: initialized on every inode allocation but never read
     *    or written by FS logic.
     *
     * = Why we are not deleting the source text =
     * Locking is subtle and the state machine below is a complete,
     * self-consistent translation of the Ceph SimpleLock semantics
     * (AVAILABLE → MIX via rdlock/wrlock, GATHER barrier on downgrade,
     * Loner promotion when holder_count==1, ScatterLock DSCATTER/INACTIVE
     * transitions, revoke-ack handling, mdlock_gather_timeout, etc.).
     * If future work ever needs to add CLIENT-SIDE caching of the
     * per-cap issued/wanted/epoch/sn state (the actual client role),
     * the per-lock bit-mask tables here can be used as reference for
     * the translation table between lock types and the Ceph cap bit
     * layout. Keeping the code text (with a clear #if 0 guard and this
     * 16-line explanation block) prevents anyone from re-accidentally
     * re-adding a second lock arbitration layer on the client.
     *
     * Lock arbitration = SERVER ONLY (lock_arbiter.rs in
     * powerfs-filer/src). Client side cap cache is modeled by
     * powerfs-fuse/src/client_cap.rs ClientCap
     * {issued_mask, wanted_mask, epoch, token, sn}. Please mirror that
     * lightweight struct here when adding client cap caching. */

/* ==============================================================
 * MDLock — 独立锁对象实现 (对齐 Ceph MDS Locker SimpleLock)
 *
 * 设计文档: docs/mdlock-design.md
 * 参考: src/mds/SimpleLock.h, src/mds/locks.cc
 *
 * 每个 inode 持有 8 把独立锁 (AUTH/LINK/XATTR/DN/SNAP/FILE/DFT/NEST),
 * 每把锁有独立状态机: AVAILABLE/SHARED/LONER/EXCL/GATHER/REVOKING.
 * chmod 只操作 i_locks[AUTH], 完全不碰 i_locks[FILE].
 * ============================================================== */

/* 锁类型 → cap 位掩码映射 (eval 使用) */
const unsigned int powerfs_lock_type_cap_bits[POWERFS_NUM_LOCK_TYPES] = {
    [POWERFS_LOCK_AUTH]  = POWERFS_CAP_AUTH_SHARED | POWERFS_CAP_AUTH_EXCL,
    [POWERFS_LOCK_LINK]  = POWERFS_CAP_LINK_SHARED,
    [POWERFS_LOCK_XATTR] = POWERFS_CAP_XATTR_SHARED | POWERFS_CAP_XATTR_EXCL,
    [POWERFS_LOCK_DN]    = 0,  /* DN 锁输出 lease, 不输出 cap */
    [POWERFS_LOCK_SNAP]  = 0,  /* LocalLock, 不输出 cap */
    [POWERFS_LOCK_FILE]  = POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE |
                            POWERFS_CAP_FILE_WR | POWERFS_CAP_FILE_EXCL,
    [POWERFS_LOCK_DFT]   = 0,  /* ScatterLock, 不输出客户端 cap */
    [POWERFS_LOCK_NEST]  = 0,  /* ScatterLock, 不输出客户端 cap */
};

/* 锁类型 → 状态机类别映射
 * 对齐 Ceph: IAUTH/ILINK/IXATTR/DN=SimpleLock, ISNAP=LocalLock,
 * IFILE=FileLock, IDFT/INEST=ScatterLock */
const enum powerfs_lock_class powerfs_lock_type_class[POWERFS_NUM_LOCK_TYPES] = {
    [POWERFS_LOCK_AUTH]  = LOCK_CLASS_SIMPLE,
    [POWERFS_LOCK_LINK]  = LOCK_CLASS_SIMPLE,
    [POWERFS_LOCK_XATTR] = LOCK_CLASS_SIMPLE,
    [POWERFS_LOCK_DN]    = LOCK_CLASS_SIMPLE,   /* DN: SimpleLock + Lease */
    [POWERFS_LOCK_SNAP]  = LOCK_CLASS_LOCAL,    /* ISNAP: LocalLock */
    [POWERFS_LOCK_FILE]  = LOCK_CLASS_FILE,     /* IFILE: FileLock */
    [POWERFS_LOCK_DFT]   = LOCK_CLASS_SCATTER,  /* IDFT: ScatterLock */
    [POWERFS_LOCK_NEST]  = LOCK_CLASS_SCATTER,  /* INEST: ScatterLock */
};

/* 前向声明: recall 发送回调 (定义在补全实现段) */
static int mdlock_send_recall(struct powerfs_inode_info *pi,
                              struct powerfs_mdlock *lock,
                              struct powerfs_lock_holder *h);

/* 初始化 inode 的所有 mdlock 实例
 * 在 powerfs_alloc_inode 中调用 */
void powerfs_init_mdlocks(struct powerfs_inode_info *pi)
{
    int i;

    for (i = 0; i < POWERFS_NUM_LOCK_TYPES; i++) {
        struct powerfs_mdlock *lock = &pi->i_locks[i];

        lock->type = (enum powerfs_lock_type)i;
        lock->cls = powerfs_lock_type_class[i];
        lock->state = LOCK_ST_AVAILABLE;
        INIT_LIST_HEAD(&lock->holders);
        INIT_LIST_HEAD(&lock->gather_list);
        INIT_LIST_HEAD(&lock->waiting);
        lock->holder_count = 0;
        lock->gather_remaining = 0;
        lock->gather_target = GATHER_TO_AVAIL;
        lock->eval_issued = 0;
        lock->eval_wanted = 0;
        lock->pi = pi;
    }
    init_waitqueue_head(&pi->i_mdlock_wq);
}

/* ===== 内部辅助函数 ===== */

/* 获取第一个 holder (假设 holders 非空) */
static struct powerfs_lock_holder *mdlock_first_holder(struct powerfs_mdlock *lock)
{
    if (list_empty(&lock->holders))
        return NULL;
    return list_first_entry(&lock->holders, struct powerfs_lock_holder, list);
}

/* 查找指定 client 的 holder */
static struct powerfs_lock_holder *mdlock_find_holder(struct powerfs_mdlock *lock,
                                                       __u64 client_id)
{
    struct powerfs_lock_holder *h;
    list_for_each_entry(h, &lock->holders, list) {
        if (h->client_id == client_id)
            return h;
    }
    return NULL;
}

/* 检查 holder 是否过期 */
static bool mdlock_holder_expired(struct powerfs_lock_holder *h)
{
    return time_after(jiffies, h->expire_jiffies);
}

/* 清理过期 holder (在锁操作前调用)
 * 返回 true 如果清理了任何 holder */
static bool mdlock_garbage_collect(struct powerfs_mdlock *lock)
{
    struct powerfs_lock_holder *h, *tmp;
    bool cleaned = false;

    list_for_each_entry_safe(h, tmp, &lock->holders, list) {
        if (mdlock_holder_expired(h)) {
            pr_debug("powerfs: mdlock GC expired holder type=%d client=%llu\n",
                     lock->type, h->client_id);
            list_del(&h->list);
            lock->holder_count--;
            kfree(h);
            cleaned = true;
        }
    }
    return cleaned;
}

/* ===== eval: 锁状态 → cap 掩码 (对齐 Ceph SimpleLock::eval) ===== */

/* eval 内部: 按 lock->cls 分发到对应状态机的 eval 逻辑 */

/* LocalLock eval: 不输出客户端 cap, 仅 MDS 本地 */
static unsigned int mdlock_eval_caps_local(struct powerfs_mdlock *lock,
                                           struct powerfs_lock_holder *h)
{
    /* LocalLock 不涉及客户端 cap, 始终返回 0 */
    return 0;
}

/* SimpleLock eval: 排他写+共享读, 支持 Loner
 * 用于 AUTH/LINK/XATTR/DN */
static unsigned int mdlock_eval_caps_simple(struct powerfs_mdlock *lock,
                                            struct powerfs_lock_holder *h)
{
    switch (lock->state) {
    case LOCK_ST_LONER:
        /* LONER: 单 client, 下发全套 exclusive cap */
        return powerfs_lock_type_cap_bits[lock->type];

    case LOCK_ST_SHARED:
        /* SHARED: 只下发 shared (只读) cap */
        if (lock->type == POWERFS_LOCK_AUTH)
            return POWERFS_CAP_AUTH_SHARED;
        if (lock->type == POWERFS_LOCK_XATTR)
            return POWERFS_CAP_XATTR_SHARED;
        if (lock->type == POWERFS_LOCK_LINK)
            return POWERFS_CAP_LINK_SHARED;
        return 0;

    case LOCK_ST_EXCL:
        /* EXCL: xlock holder 独占, 其他人无 cap */
        if (lock->holder_count == 1 && h == mdlock_first_holder(lock))
            return powerfs_lock_type_cap_bits[lock->type];
        return 0;

    case LOCK_ST_GATHER:
        return h->granted_caps;

    case LOCK_ST_REVOKING:
        return h->retain_caps;

    default:
        return 0;
    }
}

/* ScatterLock eval: 多方共享写, MDS 间合并
 * 不输出客户端 cap (客户端永远拿不到写 cap)
 * 用于 DFT/NEST */
static unsigned int mdlock_eval_caps_scatter(struct powerfs_mdlock *lock,
                                              struct powerfs_lock_holder *h)
{
    /* ScatterLock 不输出客户端 cap, 始终返回 0
     * 变更在 MDS 间合并, 客户端永远拿不到写 cap */
    return 0;
}

/* FileLock eval: 扩展 SimpleLock + 完整 FILE cap 语义 + SYNC
 * 用于 IFILE */
static unsigned int mdlock_eval_caps_file(struct powerfs_mdlock *lock,
                                          struct powerfs_lock_holder *h)
{
    switch (lock->state) {
    case LOCK_ST_LONER:
        /* FileLock LONER: 单 client 写, 下发全套 FILE cap
         * 允许本地 dirty (FILE_WR + FILE_EXCL), 大幅减 RPC */
        return POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE |
               POWERFS_CAP_FILE_WR | POWERFS_CAP_FILE_EXCL;

    case LOCK_ST_SHARED:
        /* FileLock SHARED: 多 client, 只读 cap, 不能 dirty */
        return POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE;

    case LOCK_ST_SYNC:
        /* FileLock SYNC: 只读, cap 已写回, 类似 SHARED 但无 dirty 可能
         * 对齐 Ceph: SYNC 把 all FILE caps 收回, 仅保留 FILE_SHARED */
        return POWERFS_CAP_FILE_SHARED;

    case LOCK_ST_EXCL:
        /* FileLock EXCL: xlock holder 独占, 其他人无 cap */
        if (lock->holder_count == 1 && h == mdlock_first_holder(lock))
            return POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE |
                   POWERFS_CAP_FILE_WR | POWERFS_CAP_FILE_EXCL;
        return 0;

    case LOCK_ST_GATHER:
        return h->granted_caps;

    case LOCK_ST_REVOKING:
        return h->retain_caps;

    default:
        return 0;
    }
}

/* eval 分发: 根据 lock->cls 调用对应状态机的 eval */
static unsigned int mdlock_eval_caps(struct powerfs_mdlock *lock,
                                     struct powerfs_lock_holder *h)
{
    switch (lock->cls) {
    case LOCK_CLASS_LOCAL:
        return mdlock_eval_caps_local(lock, h);
    case LOCK_CLASS_SIMPLE:
        return mdlock_eval_caps_simple(lock, h);
    case LOCK_CLASS_SCATTER:
        return mdlock_eval_caps_scatter(lock, h);
    case LOCK_CLASS_FILE:
        return mdlock_eval_caps_file(lock, h);
    default:
        return 0;
    }
}

/* eval: 锁状态 → cap 掩码 + lease 令牌
 * 对齐 Ceph: SimpleLock::eval()
 * 更新每个 holder 的 granted_caps + 聚合 eval_issued */
void powerfs_mdlock_eval(struct powerfs_mdlock *lock)
{
    struct powerfs_lock_holder *h;
    unsigned int issued_union = 0;

    lock->eval_issued = 0;
    lock->eval_wanted = 0;

    if (list_empty(&lock->holders)) {
        return;
    }

    list_for_each_entry(h, &lock->holders, list) {
        h->granted_caps = mdlock_eval_caps(lock, h);
        issued_union |= h->granted_caps;
        lock->eval_wanted |= h->dirty_caps;
    }

    lock->eval_issued = issued_union;
}

/* ===== GATHER 同步屏障 ===== */

/* GATHER 完成: ACK 收齐, 跃迁到目标状态 */
static void mdlock_gather_complete(struct powerfs_mdlock *lock)
{
    struct powerfs_lock_gather *g, *tmp;

    /* 清理 gather_list */
    list_for_each_entry_safe(g, tmp, &lock->gather_list, list) {
        list_del(&g->list);
        kfree(g);
    }
    lock->gather_remaining = 0;

    /* 跃迁到目标状态 */
    switch (lock->gather_target) {
    case GATHER_TO_EXCL:
        lock->state = LOCK_ST_EXCL;
        break;
    case GATHER_TO_SHARED:
        lock->state = LOCK_ST_SHARED;
        break;
    case GATHER_TO_LONER:
        lock->state = LOCK_ST_LONER;
        break;
    case GATHER_TO_AVAIL:
        lock->state = LOCK_ST_AVAILABLE;
        break;
    }

    /* eval 重新下发 cap */
    powerfs_mdlock_eval(lock);

    /* 唤醒等待者 */
    wake_up_all(&lock->pi->i_mdlock_wq);
}

/* recall 超时: force-reclaim (对齐 Ceph session timeout) */
static void mdlock_gather_timeout(struct powerfs_mdlock *lock)
{
    struct powerfs_lock_gather *g, *tmp;
    bool any_timeout = false;

    list_for_each_entry_safe(g, tmp, &lock->gather_list, list) {
        if (time_after(jiffies, g->sent_jiffies +
                       msecs_to_jiffies(MDLOCK_RECALL_TIMEOUT_MS))) {
            pr_warn("powerfs: mdlock recall timeout type=%d client=%llu, "
                    "force-reclaim\n", lock->type, g->client_id);
            list_del(&g->list);
            kfree(g);
            lock->gather_remaining--;
            any_timeout = true;
        }
    }
    if (any_timeout && lock->gather_remaining == 0)
        mdlock_gather_complete(lock);
}

/* ===== 锁原语实现 ===== */

/* rdlock: 获取共享读锁
 * 对齐 Ceph: SimpleLock::rdlock()
 * 等待机制: prepare_to_wait_exclusive + schedule_timeout(GATHER_TIMEOUT)
 * 避免忙等: 不用 50ms 轮询, 用 GATHER 超时作为最大等待 */
int powerfs_mdlock_rdlock(struct powerfs_inode_info *pi,
                          enum powerfs_lock_type type,
                          __u64 client_id,
                          struct powerfs_lock_holder **holder_out)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;
    DEFINE_WAIT(wait);

    might_sleep();

    spin_lock(&pi->i_lock);

    /* 清理过期 holder */
    if (mdlock_garbage_collect(lock))
        powerfs_mdlock_eval(lock);

    /* 等待直到状态允许 rdlock (非 EXCL/GATHER/REVOKING/LOCK) */
    while (lock->state == LOCK_ST_EXCL || lock->state == LOCK_ST_GATHER ||
           lock->state == LOCK_ST_REVOKING || lock->state == LOCK_ST_LOCK) {

        /* GATHER 超时检查 (持锁状态) */
        if (lock->state == LOCK_ST_GATHER)
            mdlock_gather_timeout(lock);

        /* 超时后状态可能已变, 重新检查 */
        if (lock->state != LOCK_ST_EXCL && lock->state != LOCK_ST_GATHER &&
            lock->state != LOCK_ST_REVOKING && lock->state != LOCK_ST_LOCK)
            break;

        /* 准备睡眠: exclusive 避免惊群 */
        prepare_to_wait_exclusive(&pi->i_mdlock_wq, &wait,
                                  TASK_INTERRUPTIBLE);
        spin_unlock(&pi->i_lock);

        if (signal_pending(current)) {
            finish_wait(&pi->i_mdlock_wq, &wait);
            return -ERESTARTSYS;
        }

        /* 睡眠直到 wake_up 或 GATHER 超时 (不用 50ms 轮询) */
        schedule_timeout(msecs_to_jiffies(MDLOCK_RECALL_TIMEOUT_MS));
        spin_lock(&pi->i_lock);
    }
    finish_wait(&pi->i_mdlock_wq, &wait);

    /* 检查是否已持有 (同 client 重入) */
    h = mdlock_find_holder(lock, client_id);
    if (h) {
        if (holder_out)
            *holder_out = h;
        spin_unlock(&pi->i_lock);
        return 0;
    }

    /* 分配新 holder */
    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h) {
        spin_unlock(&pi->i_lock);
        return -ENOMEM;
    }

    h->client_id = client_id;
    h->sn = ++pi->i_version;
    h->epoch = 0;
    h->duration_ms = POWERFS_LEASE_DURATION_MS;
    h->expire_jiffies = jiffies + msecs_to_jiffies(h->duration_ms);
    h->granted_caps = 0;
    h->dirty_caps = 0;
    h->recall_in_flight = false;
    h->lock = lock;

    list_add_tail(&h->list, &lock->holders);
    lock->holder_count++;

    /* 状态转移 */
    if (lock->state == LOCK_ST_AVAILABLE)
        lock->state = LOCK_ST_SHARED;
    else if (lock->state == LOCK_ST_LONER) {
        /* LONER 被新 reader 打破 → SHARED */
        lock->state = LOCK_ST_SHARED;
    }

    powerfs_mdlock_eval(lock);

    if (holder_out)
        *holder_out = h;

    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: mdlock_rdlock type=%d client=%llu state=%d\n",
             type, client_id, lock->state);
    return 0;
}

/* wrlock: 获取排他写锁
 * 对齐 Ceph: SimpleLock::wrlock() + FileLock loner
 * 等待机制: prepare_to_wait_exclusive + schedule_timeout(GATHER_TIMEOUT) */
int powerfs_mdlock_wrlock(struct powerfs_inode_info *pi,
                          enum powerfs_lock_type type,
                          __u64 client_id,
                          struct powerfs_lock_holder **holder_out)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;
    DEFINE_WAIT(wait);

    might_sleep();

    spin_lock(&pi->i_lock);

    /* 清理过期 holder */
    if (mdlock_garbage_collect(lock))
        powerfs_mdlock_eval(lock);

    /* LONER fast path: 同 client 复用 */
    if (lock->state == LOCK_ST_LONER) {
        h = mdlock_first_holder(lock);
        if (h && h->client_id == client_id) {
            if (holder_out)
                *holder_out = h;
            spin_unlock(&pi->i_lock);
            return 0;
        }
    }

    /* 需要排他: 如果有其他 holder, 进入 GATHER */
    while (lock->holder_count > 0 &&
           !(lock->state == LOCK_ST_LONER &&
             mdlock_first_holder(lock) &&
             mdlock_first_holder(lock)->client_id == client_id)) {

        /* 进入 GATHER: recall 其他 holder */
        if (lock->state != LOCK_ST_GATHER) {
            struct powerfs_lock_holder *other;
            lock->state = LOCK_ST_GATHER;
            lock->gather_target = (lock->holder_count == 0) ?
                GATHER_TO_LONER : GATHER_TO_SHARED;

            list_for_each_entry(other, &lock->holders, list) {
                if (other->client_id != client_id) {
                    struct powerfs_lock_gather *g;
                    g = kzalloc(sizeof(*g), GFP_ATOMIC);
                    if (!g)
                        continue;
                    g->client_id = other->client_id;
                    g->sn = other->sn;
                    g->sent_jiffies = jiffies;
                    g->acked = false;
                    list_add_tail(&g->list, &lock->gather_list);
                    lock->gather_remaining++;
                    other->recall_in_flight = true;
                    other->recall_caps = other->granted_caps;
                    other->retain_caps = other->granted_caps &
                        ~powerfs_lock_type_cap_bits[lock->type];
                    mdlock_send_recall(pi, lock, other);
                }
            }
        }

        /* GATHER 超时检查 */
        mdlock_gather_timeout(lock);

        if (lock->gather_remaining == 0)
            break;

        /* 准备睡眠: exclusive 避免惊群 */
        prepare_to_wait_exclusive(&pi->i_mdlock_wq, &wait,
                                  TASK_INTERRUPTIBLE);
        spin_unlock(&pi->i_lock);

        if (signal_pending(current)) {
            finish_wait(&pi->i_mdlock_wq, &wait);
            return -ERESTARTSYS;
        }

        /* 睡眠直到 wake_up 或 GATHER 超时 */
        schedule_timeout(msecs_to_jiffies(MDLOCK_RECALL_TIMEOUT_MS));
        spin_lock(&pi->i_lock);
    }
    finish_wait(&pi->i_mdlock_wq, &wait);

    /* GATHER 完成后授予 */
    h = mdlock_find_holder(lock, client_id);
    if (!h) {
        h = kzalloc(sizeof(*h), GFP_KERNEL);
        if (!h) {
            spin_unlock(&pi->i_lock);
            return -ENOMEM;
        }
        h->client_id = client_id;
        h->sn = ++pi->i_version;
        h->epoch = 0;
        h->duration_ms = POWERFS_LEASE_DURATION_MS;
        h->expire_jiffies = jiffies + msecs_to_jiffies(h->duration_ms);
        h->lock = lock;
        list_add_tail(&h->list, &lock->holders);
        lock->holder_count++;
    }

    if (lock->holder_count == 1)
        lock->state = LOCK_ST_LONER;
    else
        lock->state = LOCK_ST_SHARED;

    powerfs_mdlock_eval(lock);

    if (holder_out)
        *holder_out = h;

    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: mdlock_wrlock type=%d client=%llu state=%d\n",
             type, client_id, lock->state);
    return 0;
}

/* xlock: 获取完全独占锁
 * 对齐 Ceph: SimpleLock::xlock()
 * 等待机制: prepare_to_wait_exclusive + schedule_timeout(GATHER_TIMEOUT) */
int powerfs_mdlock_xlock(struct powerfs_inode_info *pi,
                         enum powerfs_lock_type type,
                         __u64 client_id,
                         struct powerfs_lock_holder **holder_out)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;
    DEFINE_WAIT(wait);

    might_sleep();

    spin_lock(&pi->i_lock);

    /* 清理过期 holder */
    if (mdlock_garbage_collect(lock))
        powerfs_mdlock_eval(lock);

    /* 如果有其他 holder, 进入 GATHER (收全部) */
    while (lock->holder_count > 0) {
        struct powerfs_lock_holder *first = mdlock_first_holder(lock);

        /* 如果是同 client 已持有, 直接升级 */
        if (first && first->client_id == client_id &&
            lock->holder_count == 1) {
            lock->state = LOCK_ST_EXCL;
            powerfs_mdlock_eval(lock);
            if (holder_out)
                *holder_out = first;
            spin_unlock(&pi->i_lock);
            return 0;
        }

        /* 进入 GATHER: recall 所有 holder 的 cap */
        if (lock->state != LOCK_ST_GATHER) {
            struct powerfs_lock_holder *other;
            lock->state = LOCK_ST_GATHER;
            lock->gather_target = GATHER_TO_EXCL;

            list_for_each_entry(other, &lock->holders, list) {
                if (other->client_id != client_id) {
                    struct powerfs_lock_gather *g;
                    g = kzalloc(sizeof(*g), GFP_ATOMIC);
                    if (!g)
                        continue;
                    g->client_id = other->client_id;
                    g->sn = other->sn;
                    g->sent_jiffies = jiffies;
                    g->acked = false;
                    list_add_tail(&g->list, &lock->gather_list);
                    lock->gather_remaining++;
                    other->recall_in_flight = true;
                    other->recall_caps = other->granted_caps;
                    other->retain_caps = 0;
                    mdlock_send_recall(pi, lock, other);
                }
            }
        }

        mdlock_gather_timeout(lock);

        if (lock->gather_remaining == 0)
            break;

        /* 准备睡眠: exclusive 避免惊群 */
        prepare_to_wait_exclusive(&pi->i_mdlock_wq, &wait,
                                  TASK_INTERRUPTIBLE);
        spin_unlock(&pi->i_lock);

        if (signal_pending(current)) {
            finish_wait(&pi->i_mdlock_wq, &wait);
            return -ERESTARTSYS;
        }

        /* 睡眠直到 wake_up 或 GATHER 超时 */
        schedule_timeout(msecs_to_jiffies(MDLOCK_RECALL_TIMEOUT_MS));
        spin_lock(&pi->i_lock);
    }
    finish_wait(&pi->i_mdlock_wq, &wait);

    /* 授予 xlock: 分配 holder, 状态 → EXCL */
    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h) {
        spin_unlock(&pi->i_lock);
        return -ENOMEM;
    }
    h->client_id = client_id;
    h->sn = ++pi->i_version;
    h->epoch = 0;
    h->duration_ms = POWERFS_LEASE_DURATION_MS;
    h->expire_jiffies = jiffies + msecs_to_jiffies(h->duration_ms);
    h->lock = lock;
    list_add_tail(&h->list, &lock->holders);
    lock->holder_count++;

    lock->state = LOCK_ST_EXCL;
    powerfs_mdlock_eval(lock);

    if (holder_out)
        *holder_out = h;

    spin_unlock(&pi->i_lock);
    finish_wait(&pi->i_mdlock_wq, &wait);

    pr_debug("powerfs: mdlock_xlock type=%d client=%llu state=%d\n",
             type, client_id, lock->state);
    return 0;
}

/* unlock: 释放锁 */
int powerfs_mdlock_unlock(struct powerfs_inode_info *pi,
                          enum powerfs_lock_type type,
                          __u64 sn)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h, *tmp;

    spin_lock(&pi->i_lock);

    /* 找到 sn 对应的 holder 并移除 */
    list_for_each_entry_safe(h, tmp, &lock->holders, list) {
        if (h->sn == sn) {
            list_del(&h->list);
            lock->holder_count--;
            kfree(h);
            break;
        }
    }

    /* 状态转移 */
    if (lock->holder_count == 0) {
        if (lock->state == LOCK_ST_EXCL) {
            /* EXCL 释放: 进入 GATHER 等待 flush 完成 */
            lock->state = LOCK_ST_GATHER;
            lock->gather_target = GATHER_TO_AVAIL;
            lock->gather_remaining = 0;  /* 无其他 holder, 立即完成 */
            mdlock_gather_complete(lock);
        } else {
            lock->state = LOCK_ST_AVAILABLE;
            powerfs_mdlock_eval(lock);
        }
    } else if (lock->holder_count == 1) {
        /* 只剩 1 个 holder → 升级 LONER (如果是 SimpleLock/FileLock) */
        if (lock->state == LOCK_ST_SHARED) {
            lock->state = LOCK_ST_LONER;
            powerfs_mdlock_eval(lock);
        }
    } else {
        powerfs_mdlock_eval(lock);
    }

    wake_up_all(&pi->i_mdlock_wq);
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: mdlock_unlock type=%d sn=%llu state=%d holders=%d\n",
             type, sn, lock->state, lock->holder_count);
    return 0;
}

/* recall_ack: 客户端 ACK recall, GATHER 计数减一 */
int powerfs_mdlock_recall_ack(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 client_id, __u64 sn)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_gather *g, *tmp;

    spin_lock(&pi->i_lock);

    /* 找到对应的 gather 项 */
    list_for_each_entry_safe(g, tmp, &lock->gather_list, list) {
        if (g->client_id == client_id && g->sn == sn) {
            g->acked = true;
            lock->gather_remaining--;
            list_del(&g->list);
            kfree(g);

            /* 更新对应 holder: recall_in_flight 清除 */
            struct powerfs_lock_holder *h = mdlock_find_holder(lock, client_id);
            if (h) {
                h->recall_in_flight = false;
                h->granted_caps = h->retain_caps;
            }

            /* GATHER 完成? */
            if (lock->gather_remaining == 0)
                mdlock_gather_complete(lock);

            spin_unlock(&pi->i_lock);
            pr_debug("powerfs: mdlock_recall_ack type=%d client=%llu sn=%llu "
                     "remaining=%d\n", type, client_id, sn, lock->gather_remaining);
            return 0;
        }
    }

    spin_unlock(&pi->i_lock);
    pr_debug("powerfs: mdlock_recall_ack: no matching gather type=%d "
             "client=%llu sn=%llu\n", type, client_id, sn);
    return -ENOENT;
}

/* ===== mdlock ↔ cap 转换函数 ===== */

/* mdlock → cap: 聚合所有锁的 eval_issued 到 cap.issued */
unsigned int powerfs_mdlocks_to_cap_issued(struct powerfs_inode_info *pi)
{
    unsigned int mask = 0;
    int i;

    spin_lock(&pi->i_lock);
    for (i = 0; i < POWERFS_NUM_LOCK_TYPES; i++)
        mask |= pi->i_locks[i].eval_issued;
    spin_unlock(&pi->i_lock);

    return mask;
}

/* cap → mdlock: 从收到的 CapRecall 消息拆解到各锁 */
void powerfs_cap_revoke_to_mdlocks(struct powerfs_inode_info *pi,
                                   unsigned int revoking)
{
    int i;

    spin_lock(&pi->i_lock);

    for (i = 0; i < POWERFS_NUM_LOCK_TYPES; i++) {
        unsigned int bits = revoking & powerfs_lock_type_cap_bits[i];
        if (bits) {
            struct powerfs_mdlock *lock = &pi->i_locks[i];

            /* 该锁类型有位被 recall → 进入 REVOKING */
            if (lock->state == LOCK_ST_LONER || lock->state == LOCK_ST_SHARED) {
                lock->state = LOCK_ST_REVOKING;

                /* 更新各 holder 的 recall_caps/retain_caps */
                struct powerfs_lock_holder *h;
                list_for_each_entry(h, &lock->holders, list) {
                    h->recall_caps = bits & h->granted_caps;
                    h->retain_caps = h->granted_caps & ~bits;
                    h->recall_in_flight = true;
                }

                powerfs_mdlock_eval(lock);
            }
        }
    }

    spin_unlock(&pi->i_lock);
}

/* ==============================================================
 * MDLock 补全实现 — trylock / LocalLock / ScatterLock / FileLock SYNC
 *                  / fencing / recall回调 / destroy / tick / dump
 *
 * 对齐 Ceph: src/mds/locks.cc, SimpleLock.h, ScatterLock.h,
 *            FileLock.h, LocalLock.h
 * ============================================================== */

/* ===== recall 发送回调 (解耦传输层) ===== */

static const struct powerfs_mdlock_recall_ops *mdlock_recall_ops;

void powerfs_mdlock_set_recall_ops(const struct powerfs_mdlock_recall_ops *ops)
{
    mdlock_recall_ops = ops;
}

/* 发送 recall 到客户端 (通过回调) */
static int mdlock_send_recall(struct powerfs_inode_info *pi,
                              struct powerfs_mdlock *lock,
                              struct powerfs_lock_holder *h)
{
    if (!mdlock_recall_ops || !mdlock_recall_ops->send_recall)
        return -ENOSYS;

    return mdlock_recall_ops->send_recall(
        &pi->netfs.inode, lock->type, h->client_id, h->sn,
        h->recall_caps, h->retain_caps);
}

/* ===== sn fencing 机制 ===== */

bool powerfs_mdlock_sn_valid(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 sn)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;
    bool valid = false;

    spin_lock(&pi->i_lock);
    list_for_each_entry(h, &lock->holders, list) {
        if (h->sn == sn) {
            valid = true;
            break;
        }
    }
    spin_unlock(&pi->i_lock);
    return valid;
}

void powerfs_mdlock_fence_epoch(struct powerfs_inode_info *pi,
                                 enum powerfs_lock_type type)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h, *tmp;

    spin_lock(&pi->i_lock);

    pr_warn("powerfs: mdlock fence_epoch type=%d, force-reclaiming all holders\n",
             type);

    list_for_each_entry_safe(h, tmp, &lock->holders, list) {
        list_del(&h->list);
        lock->holder_count--;
        kfree(h);
    }

    /* 清理 gather_list */
    {
        struct powerfs_lock_gather *g, *gtmp;
        list_for_each_entry_safe(g, gtmp, &lock->gather_list, list) {
            list_del(&g->list);
            kfree(g);
        }
        lock->gather_remaining = 0;
    }

    lock->state = LOCK_ST_AVAILABLE;
    lock->eval_issued = 0;
    lock->eval_wanted = 0;
    powerfs_mdlock_eval(lock);

    spin_unlock(&pi->i_lock);
    wake_up_all(&pi->i_mdlock_wq);
}

/* ===== trylock 非阻塞版本 ===== */

int powerfs_mdlock_try_rdlock(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 client_id,
                              struct powerfs_lock_holder **holder_out)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;

    spin_lock(&pi->i_lock);

    mdlock_garbage_collect(lock);

    /* 非阻塞: EXCL/GATHER/REVOKING 时不可获取 */
    if (lock->state == LOCK_ST_EXCL || lock->state == LOCK_ST_GATHER ||
        lock->state == LOCK_ST_REVOKING || lock->state == LOCK_ST_LOCK) {
        spin_unlock(&pi->i_lock);
        return -EAGAIN;
    }

    h = mdlock_find_holder(lock, client_id);
    if (h) {
        if (holder_out)
            *holder_out = h;
        spin_unlock(&pi->i_lock);
        return 0;
    }

    h = kzalloc(sizeof(*h), GFP_ATOMIC);
    if (!h) {
        spin_unlock(&pi->i_lock);
        return -ENOMEM;
    }

    h->client_id = client_id;
    h->sn = ++pi->i_version;
    h->duration_ms = POWERFS_LEASE_DURATION_MS;
    h->expire_jiffies = jiffies + msecs_to_jiffies(h->duration_ms);
    h->lock = lock;
    list_add_tail(&h->list, &lock->holders);
    lock->holder_count++;

    if (lock->state == LOCK_ST_AVAILABLE)
        lock->state = LOCK_ST_SHARED;
    else if (lock->state == LOCK_ST_LONER)
        lock->state = LOCK_ST_SHARED;

    powerfs_mdlock_eval(lock);
    if (holder_out)
        *holder_out = h;
    spin_unlock(&pi->i_lock);
    return 0;
}

int powerfs_mdlock_try_wrlock(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 client_id,
                              struct powerfs_lock_holder **holder_out)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;

    spin_lock(&pi->i_lock);

    mdlock_garbage_collect(lock);

    /* LONER fast path: 同 client 复用 */
    if (lock->state == LOCK_ST_LONER) {
        h = mdlock_first_holder(lock);
        if (h && h->client_id == client_id) {
            if (holder_out)
                *holder_out = h;
            spin_unlock(&pi->i_lock);
            return 0;
        }
    }

    /* 非阻塞: 有其他 holder 时不等待 */
    if (lock->holder_count > 0) {
        h = mdlock_first_holder(lock);
        if (!h || h->client_id != client_id) {
            spin_unlock(&pi->i_lock);
            return -EAGAIN;
        }
    }

    /* 可获取: 无竞争或同 client */
    h = mdlock_find_holder(lock, client_id);
    if (!h) {
        h = kzalloc(sizeof(*h), GFP_ATOMIC);
        if (!h) {
            spin_unlock(&pi->i_lock);
            return -ENOMEM;
        }
        h->client_id = client_id;
        h->sn = ++pi->i_version;
        h->duration_ms = POWERFS_LEASE_DURATION_MS;
        h->expire_jiffies = jiffies + msecs_to_jiffies(h->duration_ms);
        h->lock = lock;
        list_add_tail(&h->list, &lock->holders);
        lock->holder_count++;
    }

    if (lock->holder_count == 1)
        lock->state = LOCK_ST_LONER;
    else
        lock->state = LOCK_ST_SHARED;

    powerfs_mdlock_eval(lock);
    if (holder_out)
        *holder_out = h;
    spin_unlock(&pi->i_lock);
    return 0;
}

int powerfs_mdlock_try_xlock(struct powerfs_inode_info *pi,
                             enum powerfs_lock_type type,
                             __u64 client_id,
                             struct powerfs_lock_holder **holder_out)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;

    spin_lock(&pi->i_lock);

    mdlock_garbage_collect(lock);

    /* 非阻塞: 有其他 client 的 holder 时不等待 */
    if (lock->holder_count > 0) {
        h = mdlock_first_holder(lock);
        if (!h || h->client_id != client_id || lock->holder_count > 1) {
            spin_unlock(&pi->i_lock);
            return -EAGAIN;
        }
        /* 同 client 唯一 holder: 直接升级 */
        lock->state = LOCK_ST_EXCL;
        powerfs_mdlock_eval(lock);
        if (holder_out)
            *holder_out = h;
        spin_unlock(&pi->i_lock);
        return 0;
    }

    /* 无 holder: 直接获取 */
    h = kzalloc(sizeof(*h), GFP_ATOMIC);
    if (!h) {
        spin_unlock(&pi->i_lock);
        return -ENOMEM;
    }
    h->client_id = client_id;
    h->sn = ++pi->i_version;
    h->duration_ms = POWERFS_LEASE_DURATION_MS;
    h->expire_jiffies = jiffies + msecs_to_jiffies(h->duration_ms);
    h->lock = lock;
    list_add_tail(&h->list, &lock->holders);
    lock->holder_count++;

    lock->state = LOCK_ST_EXCL;
    powerfs_mdlock_eval(lock);
    if (holder_out)
        *holder_out = h;
    spin_unlock(&pi->i_lock);
    return 0;
}

/* ===== LocalLock 专用原语 ===== */

int powerfs_mdlock_local_lock(struct powerfs_inode_info *pi,
                               enum powerfs_lock_type type)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];

    spin_lock(&pi->i_lock);

    if (lock->state != LOCK_ST_AVAILABLE) {
        spin_unlock(&pi->i_lock);
        return -EAGAIN;  /* LocalLock 不阻塞等待 */
    }

    lock->state = LOCK_ST_LOCK;
    /* LocalLock 无客户端 cap, eval 无效但保持一致 */
    powerfs_mdlock_eval(lock);

    spin_unlock(&pi->i_lock);
    pr_debug("powerfs: mdlock_local_lock type=%d state=LOCK\n", type);
    return 0;
}

int powerfs_mdlock_local_unlock(struct powerfs_inode_info *pi,
                                 enum powerfs_lock_type type)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];

    spin_lock(&pi->i_lock);

    lock->state = LOCK_ST_AVAILABLE;
    powerfs_mdlock_eval(lock);

    spin_unlock(&pi->i_lock);
    wake_up_all(&pi->i_mdlock_wq);

    pr_debug("powerfs: mdlock_local_unlock type=%d state=AVAILABLE\n", type);
    return 0;
}

/* ===== ScatterLock 专用原语 ===== */

int powerfs_mdlock_scatter_wrlock(struct powerfs_inode_info *pi,
                                   enum powerfs_lock_type type,
                                   __u64 client_id,
                                   struct powerfs_lock_holder **holder_out)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h;

    spin_lock(&pi->i_lock);

    mdlock_garbage_collect(lock);

    /* ScatterLock: INACTIVE → 重新 DSCATTER */
    if (lock->state == LOCK_ST_INACTIVE)
        lock->state = LOCK_ST_DSCATTER;

    /* DSCATTER 允许多方共享写 */
    if (lock->state != LOCK_ST_AVAILABLE &&
        lock->state != LOCK_ST_DSCATTER &&
        lock->state != LOCK_ST_SYNC) {
        /* EXCL/GATHER 时阻塞 */
        spin_unlock(&pi->i_lock);
        return -EAGAIN;  /* 简化: 不阻塞, 调用方重试 */
    }

    h = mdlock_find_holder(lock, client_id);
    if (!h) {
        h = kzalloc(sizeof(*h), GFP_ATOMIC);
        if (!h) {
            spin_unlock(&pi->i_lock);
            return -ENOMEM;
        }
        h->client_id = client_id;
        h->sn = ++pi->i_version;
        h->duration_ms = POWERFS_LEASE_DURATION_MS;
        h->expire_jiffies = jiffies + msecs_to_jiffies(h->duration_ms);
        h->lock = lock;
        list_add_tail(&h->list, &lock->holders);
        lock->holder_count++;
    }

    /* 状态转移 */
    if (lock->state == LOCK_ST_AVAILABLE || lock->state == LOCK_ST_SYNC)
        lock->state = LOCK_ST_DSCATTER;

    powerfs_mdlock_eval(lock);
    if (holder_out)
        *holder_out = h;
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: mdlock_scatter_wrlock type=%d client=%llu state=%d\n",
             type, client_id, lock->state);
    return 0;
}

int powerfs_mdlock_scatter_unlock(struct powerfs_inode_info *pi,
                                  enum powerfs_lock_type type,
                                  __u64 sn)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];
    struct powerfs_lock_holder *h, *tmp;

    spin_lock(&pi->i_lock);

    list_for_each_entry_safe(h, tmp, &lock->holders, list) {
        if (h->sn == sn) {
            list_del(&h->list);
            lock->holder_count--;
            kfree(h);
            break;
        }
    }

    if (lock->holder_count == 0) {
        /* 无活跃 holder → INACTIVE (保持 scatter 状态便于快速重入)
         * 后续可由 tick 清理到 AVAILABLE */
        if (lock->state == LOCK_ST_DSCATTER)
            lock->state = LOCK_ST_INACTIVE;
        else
            lock->state = LOCK_ST_AVAILABLE;
    }
    powerfs_mdlock_eval(lock);

    spin_unlock(&pi->i_lock);
    wake_up_all(&pi->i_mdlock_wq);

    pr_debug("powerfs: mdlock_scatter_unlock type=%d sn=%llu state=%d holders=%d\n",
             type, sn, lock->state, lock->holder_count);
    return 0;
}

/* ===== FileLock SYNC 状态转移 ===== */

int powerfs_mdlock_file_flush_to_sync(struct powerfs_inode_info *pi,
                                       enum powerfs_lock_type type,
                                       __u64 client_id)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];

    /* 仅 FileLock 使用 SYNC 状态 */
    if (lock->cls != LOCK_CLASS_FILE)
        return -EINVAL;

    spin_lock(&pi->i_lock);

    if (lock->state != LOCK_ST_SHARED && lock->state != LOCK_ST_LONER) {
        spin_unlock(&pi->i_lock);
        return -EINVAL;
    }

    /* SHARED/LONER → SYNC: cap 已写回, 降级为只读 */
    lock->state = LOCK_ST_SYNC;
    powerfs_mdlock_eval(lock);

    spin_unlock(&pi->i_lock);
    wake_up_all(&pi->i_mdlock_wq);

    pr_debug("powerfs: mdlock_file_flush_to_sync type=%d client=%llu\n",
             type, client_id);
    return 0;
}

int powerfs_mdlock_file_sync_to_shared(struct powerfs_inode_info *pi,
                                        enum powerfs_lock_type type,
                                        __u64 client_id)
{
    struct powerfs_mdlock *lock = &pi->i_locks[type];

    if (lock->cls != LOCK_CLASS_FILE)
        return -EINVAL;

    spin_lock(&pi->i_lock);

    if (lock->state != LOCK_ST_SYNC) {
        spin_unlock(&pi->i_lock);
        return -EINVAL;
    }

    /* SYNC → SHARED: 新写请求, 升级回 SHARED */
    lock->state = LOCK_ST_SHARED;
    powerfs_mdlock_eval(lock);

    spin_unlock(&pi->i_lock);
    wake_up_all(&pi->i_mdlock_wq);

    pr_debug("powerfs: mdlock_file_sync_to_shared type=%d client=%llu\n",
             type, client_id);
    return 0;
}

/* ===== inode 销毁清理 ===== */

void powerfs_destroy_mdlocks(struct powerfs_inode_info *pi)
{
    int i;

    for (i = 0; i < POWERFS_NUM_LOCK_TYPES; i++) {
        struct powerfs_mdlock *lock = &pi->i_locks[i];
        struct powerfs_lock_holder *h, *htmp;
        struct powerfs_lock_gather *g, *gtmp;
        struct powerfs_lock_waiter *w, *wtmp;

        /* 释放 holders */
        list_for_each_entry_safe(h, htmp, &lock->holders, list) {
            list_del(&h->list);
            kfree(h);
        }

        /* 释放 gather_list */
        list_for_each_entry_safe(g, gtmp, &lock->gather_list, list) {
            list_del(&g->list);
            kfree(g);
        }

        /* 释放 waiting */
        list_for_each_entry_safe(w, wtmp, &lock->waiting, list) {
            list_del(&w->list);
            kfree(w);
        }

        lock->holder_count = 0;
        lock->gather_remaining = 0;
        lock->state = LOCK_ST_AVAILABLE;
    }
}

/* ===== tick: 过期 holder 清理 + GATHER 超时 ===== */

void powerfs_mdlock_tick(struct powerfs_inode_info *pi)
{
    int i;
    bool any_change = false;

    if (!pi)
        return;

    /* 避免 deadlock: 不持锁检查, 有过期才加锁清理 */
    spin_lock(&pi->i_lock);

    for (i = 0; i < POWERFS_NUM_LOCK_TYPES; i++) {
        struct powerfs_mdlock *lock = &pi->i_locks[i];

        /* 清理过期 holder */
        if (mdlock_garbage_collect(lock)) {
            any_change = true;
            /* holder 数为 0 → AVAILABLE */
            if (lock->holder_count == 0 &&
                lock->state != LOCK_ST_GATHER) {
                lock->state = LOCK_ST_AVAILABLE;
            } else if (lock->holder_count == 1 &&
                       lock->state == LOCK_ST_SHARED) {
                /* 只剩 1 个 holder → LONER (SimpleLock/FileLock) */
                if (lock->cls == LOCK_CLASS_SIMPLE ||
                    lock->cls == LOCK_CLASS_FILE)
                    lock->state = LOCK_ST_LONER;
            }
            powerfs_mdlock_eval(lock);
        }

        /* GATHER 超时检查 */
        if (lock->state == LOCK_ST_GATHER && lock->gather_remaining > 0) {
            mdlock_gather_timeout(lock);
            any_change = true;
        }

        /* ScatterLock INACTIVE → AVAILABLE (idle 回收) */
        if (lock->cls == LOCK_CLASS_SCATTER &&
            lock->state == LOCK_ST_INACTIVE &&
            lock->holder_count == 0) {
            lock->state = LOCK_ST_AVAILABLE;
            any_change = true;
        }
    }

    spin_unlock(&pi->i_lock);

    if (any_change)
        wake_up_all(&pi->i_mdlock_wq);
}

/* ===== 调试: lock status dump ===== */

static const char *mdlock_state_name(enum powerfs_lock_state s)
{
    switch (s) {
    case LOCK_ST_AVAILABLE: return "AVAILABLE";
    case LOCK_ST_SHARED:    return "SHARED";
    case LOCK_ST_LONER:     return "LONER";
    case LOCK_ST_EXCL:      return "EXCL";
    case LOCK_ST_GATHER:    return "GATHER";
    case LOCK_ST_REVOKING:  return "REVOKING";
    case LOCK_ST_LOCK:      return "LOCK";
    case LOCK_ST_DSCATTER:  return "DSCATTER";
    case LOCK_ST_INACTIVE:  return "INACTIVE";
    case LOCK_ST_SYNC:      return "SYNC";
    default:                return "UNKNOWN";
    }
}

static const char *mdlock_class_name(enum powerfs_lock_class c)
{
    switch (c) {
    case LOCK_CLASS_LOCAL:   return "Local";
    case LOCK_CLASS_SIMPLE:  return "Simple";
    case LOCK_CLASS_SCATTER: return "Scatter";
    case LOCK_CLASS_FILE:    return "File";
    default:                  return "?";
    }
}

static const char *mdlock_type_name(enum powerfs_lock_type t)
{
    static const char *names[POWERFS_NUM_LOCK_TYPES] = {
        "AUTH", "LINK", "XATTR", "DN", "SNAP", "FILE", "DFT", "NEST"
    };
    if (t < POWERFS_NUM_LOCK_TYPES)
        return names[t];
    return "?";
}

int powerfs_mdlock_dump_one(struct powerfs_mdlock *lock, char *buf, int buflen)
{
    int n = 0;

    n += scnprintf(buf + n, buflen - n,
                   "  %s[%s] state=%s holders=%d gather=%d/%d "
                   "eval_issued=0x%x eval_wanted=0x%x\n",
                   mdlock_type_name(lock->type),
                   mdlock_class_name(lock->cls),
                   mdlock_state_name(lock->state),
                   lock->holder_count,
                   lock->gather_remaining,
                   lock->gather_target,
                   lock->eval_issued,
                   lock->eval_wanted);

    /* 列出每个 holder */
    {
        struct powerfs_lock_holder *h;
        int idx = 0;
        list_for_each_entry(h, &lock->holders, list) {
            n += scnprintf(buf + n, buflen - n,
                           "    [%d] client=%llu sn=%llu granted=0x%x "
                           "dirty=0x%x recall=%s\n",
                           idx++, h->client_id, h->sn,
                           h->granted_caps, h->dirty_caps,
                           h->recall_in_flight ? "Y" : "N");
            if (n >= buflen - 80)
                break;
        }
    }

    return n;
}

int powerfs_mdlock_dump(struct powerfs_inode_info *pi, char *buf, int buflen)
{
    int i, n = 0;

    n += scnprintf(buf + n, buflen - n,
                   "MDLock dump for inode %lu:\n",
                   pi->netfs.inode.i_ino);

    for (i = 0; i < POWERFS_NUM_LOCK_TYPES; i++) {
        struct powerfs_mdlock *lock = &pi->i_locks[i];

        /* 只 dump 非空闲锁 */
        if (lock->state == LOCK_ST_AVAILABLE && lock->holder_count == 0)
            continue;

        n += powerfs_mdlock_dump_one(lock, buf + n, buflen - n);
        if (n >= buflen - 80)
            break;
    }

    return n;
}

#endif /* DEAD_CODE — MDLock subsystem (~1400 lines, Ceph MDS Locker-on-client; see block comment at L3339).
        * Real lock arbitration lives on the Filer leader in lock_arbiter.rs.
        * Client side only mirrors the lightweight ClientCap cap-cache
        * {issued, wanted, epoch, sn, token} per inode. */

/*
 * powerfs_setattr_work_fn - 异步 setattr work 函数 (WQ_UNBOUND 上下文)
 *
 * 在 writeback_wq (WQ_UNBOUND) 中执行, 不阻塞 per-CPU writeback workqueue.
 * 读取最新 i_size (可能已有多次 writeback 累积), 调用 powerfs_net_setattr
 * 同步到 Filer, 成功后更新 pi->content_size.
 */
static void powerfs_setattr_work_fn(struct work_struct *work)
{
    struct powerfs_inode_info *pi = container_of(work,
                                                   struct powerfs_inode_info,
                                                   setattr_work);
    struct inode *inode = &pi->netfs.inode;
    u64 i_size;
    int ret;

    if (powerfs_net_is_stopping())
        goto out;

    i_size = i_size_read(inode);
    if (i_size == 0)
        goto out;

    ret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                               0, 0, 0, i_size, 0, 0);
    if (ret < 0) {
        pr_warn("powerfs: async setattr ino=%lu size=%llu failed: %d\n",
                inode->i_ino, i_size, ret);
        goto out;  /* content_size 不更新, 下次 writeback 重试 */
    }

    spin_lock(&pi->i_lock);
    pi->content_size = i_size;
    spin_unlock(&pi->i_lock);

out:
    spin_lock(&pi->i_lock);
    pi->setattr_pending = false;
    spin_unlock(&pi->i_lock);
}

/*
 * powerfs_write_inode - VFS writeback 时同步 inode 元数据到 Filer
 *
 * 作用:
 *   write_end 更新内存 inode 的 i_size, writepage 只刷数据页, 二者都
 *   不会把 i_size 元数据同步到 Filer. 若不同步, sync/writeback 后 Filer
 *   端 i_size 仍为 0, remount 后 lookup 返回 size=0, 文件内容丢失.
 *
 * 触发时机:
 *   - sync(2) / sync 命令 (sync_filesystem -> writeback_inodes_sb)
 *   - fsync(2) (file_write_and_wait_range -> __writeback_single_inode)
 *   - 内存压力下的 writeback
 *
 * 优化:
 *   用 pi->content_size 跟踪上次成功同步的 size, 仅在 i_size 变化时
 *   发送 setattr, 避免每次 writeback 都网络往返.
 *
 * WB_SYNC_ALL (fsync/sync): 调用方进程上下文, 可安全同步等待网络.
 * WB_SYNC_NONE (背景 writeback): 内核 per-CPU writeback workqueue,
 *   必须 offload 到 WQ_UNBOUND, 否则同步网络调用阻塞 per-CPU worker
 *   导致 workqueue lockup (实测 598s).
 *
 * 参考: xxx_write_inode (fs/xxx/inode.c) 同步 caps 模式.
 */
static int powerfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);
    loff_t i_size;
    u64 last_synced;

    if (!S_ISREG(inode->i_mode))
        return 0;

    /* umount 期间 (stopping=1): 跳过网络同步, 返回 0 允许 inode 驱逐.
     * 否则 writeback 会 redirty, evict_inodes 无法驱逐, umount 挂起. */
    if (powerfs_net_is_stopping())
        return 0;

    i_size = i_size_read(inode);
    if (i_size == 0)
        return 0;

    /* 仅在 i_size 变化时同步 */
    spin_lock(&pi->i_lock);
    last_synced = pi->content_size;
    spin_unlock(&pi->i_lock);

    if ((u64)i_size == last_synced)
        return 0;

    if (wbc->sync_mode == WB_SYNC_ALL && !(current->flags & PF_WQ_WORKER)) {
        /* fsync: 调用方进程上下文 (非 writeback workqueue), 可安全同步等待网络.
         * 注意: sync(2) 会在 per-CPU writeback 线程中触发 WB_SYNC_ALL,
         * 此时不能阻塞 (PF_WQ_WORKER), 走 offload 路径. */
        int ret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                                       0, 0, 0, (__u64)i_size, 0, 0);
        if (ret < 0) {
            pr_warn("powerfs: write_inode sync setattr ino=%lu size=%llu failed: %d\n",
                    inode->i_ino, (u64)i_size, ret);
            return ret;
        }
        spin_lock(&pi->i_lock);
        pi->content_size = (u64)i_size;
        spin_unlock(&pi->i_lock);
    } else {
        /* WB_SYNC_NONE (背景 writeback): 内核 per-CPU writeback workqueue,
         * 必须不能阻塞 — offload 到 WQ_UNBOUND (sbi->writeback_wq).
         * 若已有 pending work, 跳过 (work 函数会读取最新 i_size).
         * 返回 0 (不 redirty), setattr 失败时 content_size 不更新, 下次重试. */
        bool already_pending;

        spin_lock(&pi->i_lock);
        already_pending = pi->setattr_pending;
        if (!already_pending)
            pi->setattr_pending = true;
        spin_unlock(&pi->i_lock);

        if (!already_pending)
            queue_work(sbi->writeback_wq, &pi->setattr_work);
    }

    return 0;
}

/* Phase 3: lease 续约 work 函数.
 *
 * 每次触发时扫描 inode 的 lease_tree, 对即将过期的 lease (剩余有效期 <
 * POWERFS_LEASE_RENEW_THRESHOLD) 发送续约请求.
 *
 * 设计要点 (参考  cap renew + GFS2 glock queue_delayed_work):
 *   - delayed_work 按 inode 独立调度, 不同 inode 的续约互不阻塞
 *   - 两阶段: 锁内收集待续约 lease → 解锁逐个续约 → 重新加锁更新
 *   - 续约失败不删除 lease: lease 自然过期后由 acquire 路径重新获取
 *   - shutting_down 时停止重调度, 避免 destroy_workqueue flush 循环
 *
 * 调度策略:
 *   - 下次触发 = earliest_expiry - RENEW_THRESHOLD
 *   - 最小间隔 1s, 避免过于频繁的重调度
 *
 * 批量大小: POWERFS_LEASE_RENEW_BATCH 限制单次收集的 lease 数.
 *   一个 inode 的 lease 数 = file_size / STRIPE_SIZE, 多数文件 1 个,
 *   1GB 文件 16 个. 批量不够时剩余 lease 下次 work 触发时续约. */
#define POWERFS_LEASE_RENEW_BATCH  16

/*
 * Lease 管理函数 (Phase 3: 强一致性写)
 *
 * ensure_lease: 写路径中获取 per-stripe lease (Follower→Leader)
 * release_lease: close 时释放单个 stripe lease
 * release_all_leases: inode 销毁时释放所有 lease
 */

/* 在 lease_tree 中查找 stripe_start 对应的 lease (调用方持有 lease_lock) */
static struct powerfs_lease *powerfs_lease_find(struct powerfs_inode_info *pi,
                                                 u64 stripe_start)
{
    struct rb_node *n = pi->lease_tree.rb_node;
    while (n) {
        struct powerfs_lease *l = rb_entry(n, struct powerfs_lease, node);
        if (stripe_start < l->stripe_start)
            n = n->rb_left;
        else if (stripe_start > l->stripe_start)
            n = n->rb_right;
        else
            return l;
    }
    return NULL;
}

/* 将 lease 插入 lease_tree (调用方持有 lease_lock) */
static void powerfs_lease_insert(struct powerfs_inode_info *pi,
                                  struct powerfs_lease *lease)
{
    struct rb_node **p = &pi->lease_tree.rb_node;
    struct rb_node *parent = NULL;
    while (*p) {
        struct powerfs_lease *l = rb_entry(*p, struct powerfs_lease, node);
        parent = *p;
        if (lease->stripe_start < l->stripe_start)
            p = &(*p)->rb_left;
        else if (lease->stripe_start > l->stripe_start)
            p = &(*p)->rb_right;
        else
            return; /* 已存在, 不插入 */
    }
    rb_link_node(&lease->node, parent, p);
    rb_insert_color(&lease->node, &pi->lease_tree);
}

/*
 * ensure_lease - 确保持有指定 offset 所在 stripe 的 lease
 *
 * 写路径调用: 在写入数据前获取 lease, 保证强一致性.
 * stripe_start = (offset / STRIPE_SIZE) * STRIPE_SIZE
 * 如果 lease 已存在且未过期, 直接返回 (快速路径).
 * 如果 lease 已过期或不存在, 向 volume server 请求获取.
 *
 * 返回 0 成功, 负值错误.
 */
static int ensure_lease(struct inode *inode, loff_t offset)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = sb ? POWERFS_SB_INFO(sb) : NULL;
    u64 stripe_start = (offset / POWERFS_STRIPE_SIZE) * POWERFS_STRIPE_SIZE;
    struct powerfs_lease *lease, *old = NULL;
    unsigned long now = jiffies;
    size_t token_len;
    int ret;

    /* 快速路径: 持有有效 lease */
    spin_lock(&pi->lease_lock);
    lease = powerfs_lease_find(pi, stripe_start);
    if (lease && time_after(lease->expire_jiffies, now)) {
        spin_unlock(&pi->lease_lock);
        return 0;
    }
    /* 过期 lease: 移除并稍后释放 */
    if (lease) {
        rb_erase(&lease->node, &pi->lease_tree);
        old = lease;
    }
    spin_unlock(&pi->lease_lock);

    /* 锁外释放过期 lease (网络操作) */
    if (old) {
        powerfs_net_release_lease(pi->volume_id, inode->i_ino,
                                   old->token, strlen(old->token), NULL);
        kfree(old);
    }

    /* 分配新 lease */
    lease = kzalloc(sizeof(*lease), GFP_NOFS);
    if (!lease)
        return -ENOMEM;

    lease->stripe_start = stripe_start;
    lease->stripe_count = 1;
    lease->exclusive = true;
    token_len = sizeof(lease->token);

    /* 向 volume server 请求 lease (网络操作, 锁外) */
    ret = powerfs_net_acquire_lease(pi->volume_id, inode->i_ino,
                                     stripe_start, 1, NULL,
                                     lease->token, &token_len,
                                     &lease->epoch, &lease->content_size,
                                     &lease->expire_jiffies);
    if (ret) {
        pr_warn("powerfs: ensure_lease failed ino=%lu stripe=%llu: %d\n",
                inode->i_ino, (unsigned long long)stripe_start, ret);
        kfree(lease);
        return ret;
    }

    /* 插入 lease_tree */
    spin_lock(&pi->lease_lock);
    /* 如果在此期间有人插入了相同 stripe 的 lease, 保留旧的 */
    if (!powerfs_lease_find(pi, stripe_start)) {
        powerfs_lease_insert(pi, lease);
        lease = NULL; /* 被树接管 */
    }
    spin_unlock(&pi->lease_lock);

    if (lease) {
        /* 竞争失败, 释放多余的 lease */
        powerfs_net_release_lease(pi->volume_id, inode->i_ino,
                                   lease->token, strlen(lease->token), NULL);
        kfree(lease);
        lease = NULL;  /* 置 NULL, 避免悬空指针检查 */
    }

    /* 调度续约工作 (仅在新 lease 成功插入时启动).
     * delay = LEASE_DURATION - RENEW_THRESHOLD (20s), 在过期前 10s 续约.
     * queue_delayed_work 不会重复入队: 若 work 已 pending, 返回 false. */
    if (sbi && sbi->lease_wq && !sbi->shutting_down) {
        unsigned long delay = POWERFS_LEASE_DURATION - POWERFS_LEASE_RENEW_THRESHOLD;
        queue_delayed_work(sbi->lease_wq, &pi->lease_renew_work, delay);
    }

    return 0;
}

/*
 * powerfs_get_lease_token - 获取指定 offset 所在 stripe 的 lease token
 *
 * writeback 路径辅助函数: 从 lease_lock 下读取已持有的 token.
 *
 * 重要修复: 不再调用 ensure_lease (同步网络调用 powerfs_net_acquire_lease).
 * writeback 工作队列 (powerfs_wb) 禁止阻塞在网络 I/O 上, 否则会导致:
 *   - workqueue lockup (实测 679s+)
 *   - wb_workfn 在 powerfs_write_inode spin_lock 上死等
 *   - sync(2) 进入 D 状态永久卡死
 *   - RCU stall
 *
 * lease 获取由 open/write 路径预先完成, writeback 时 lease 应已持有.
 * 若 lease 未持有或已过期, 返回 -ENOENT, writeback 继续无 lease 写入
 * (Volume Server 容许无 lease 写入, lease_token 为可选字段).
 *
 * 返回 0 成功 (token/token_len 已填充), 负值错误 (-ENOENT=无 lease).
 */
static int powerfs_get_lease_token(struct inode *inode, loff_t offset,
                                    char *token, size_t *token_len)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    u64 stripe_start = (offset / POWERFS_STRIPE_SIZE) * POWERFS_STRIPE_SIZE;
    struct powerfs_lease *l;

    spin_lock(&pi->lease_lock);
    l = powerfs_lease_find(pi, stripe_start);
    if (l && time_after(l->expire_jiffies, jiffies)) {
        strncpy(token, l->token, 63);
        token[63] = '\0';
        *token_len = strlen(token);
        spin_unlock(&pi->lease_lock);
        return 0;
    }
    spin_unlock(&pi->lease_lock);
    *token_len = 0;
    return -ENOENT;
}

/*
 * release_all_leases - 释放 inode 的所有 lease (close/evict 时调用)
 *
 * 锁内收集 lease 列表, 锁外逐个释放 (网络操作).
 */
static void release_all_leases(struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_lease *leases[POWERFS_LEASE_RENEW_BATCH];
    int count = 0, i;
    struct rb_node *n;

    /* 收集所有 lease (锁内) */
    spin_lock(&pi->lease_lock);
    while (!RB_EMPTY_ROOT(&pi->lease_tree) && count < POWERFS_LEASE_RENEW_BATCH) {
        n = rb_first(&pi->lease_tree);
        rb_erase(n, &pi->lease_tree);
        leases[count++] = rb_entry(n, struct powerfs_lease, node);
    }
    spin_unlock(&pi->lease_lock);

    /* 释放 lease (锁外, 网络操作) */
    for (i = 0; i < count; i++) {
        powerfs_net_release_lease(pi->volume_id, inode->i_ino,
                                   leases[i]->token,
                                   strlen(leases[i]->token), NULL);
        kfree(leases[i]);
    }
}

static void powerfs_lease_renew_work_func(struct work_struct *work)
{
    struct delayed_work *dw = to_delayed_work(work);
    struct powerfs_inode_info *pi =
        container_of(dw, struct powerfs_inode_info, lease_renew_work);
    struct inode *inode = &pi->netfs.inode;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = sb ? POWERFS_SB_INFO(sb) : NULL;
    struct rb_node *n;
    unsigned long now = jiffies;
    unsigned long earliest_expiry = 0;
    bool has_lease = false;

    /* 待续约 lease 信息 (锁内收集, 锁外续约) */
    struct {
        u64 stripe_start;
        char token[64];
        size_t token_len;
    } renew_list[POWERFS_LEASE_RENEW_BATCH];
    int renew_count = 0;
    u64 volume_id;
    int i;

    /* 卸载中或 workqueue 已销毁: 停止续约, 不重调度 */
    if (!sbi || !sbi->lease_wq || sbi->shutting_down || pi->shutdown)
        return;

    /* Phase 1: 锁内扫描, 收集需要续约的 lease 信息 + 跟踪最早过期时间.
     * 不在锁内执行网络 I/O: spinlock 不可睡眠, 网络 RPC 可阻塞 5s. */
    spin_lock(&pi->lease_lock);
    volume_id = pi->volume_id;
    for (n = rb_first(&pi->lease_tree); n; n = rb_next(n)) {
        struct powerfs_lease *lease =
            rb_entry(n, struct powerfs_lease, node);

        has_lease = true;

        /* 跟踪最早过期时间 (用于重调度) */
        if (earliest_expiry == 0 ||
            time_before(lease->expire_jiffies, earliest_expiry))
            earliest_expiry = lease->expire_jiffies;

        /* 检查是否需要续约: 剩余有效期 < RENEW_THRESHOLD */
        if (time_after(now + POWERFS_LEASE_RENEW_THRESHOLD,
                       lease->expire_jiffies)) {
            if (renew_count < POWERFS_LEASE_RENEW_BATCH) {
                renew_list[renew_count].stripe_start = lease->stripe_start;
                memcpy(renew_list[renew_count].token, lease->token, 64);
                renew_list[renew_count].token_len =
                    strnlen(renew_list[renew_count].token, 64);
                renew_count++;
            }
            /* 批量满时剩余 lease 下次 work 处理 */
        }
    }
    spin_unlock(&pi->lease_lock);

    /* Phase 2: 锁外逐个发送续约请求 (网络 I/O 可睡眠) */
    for (i = 0; i < renew_count; i++) {
        unsigned long new_expire = 0;
        int ret;

        ret = powerfs_net_renew_lease(volume_id, inode->i_ino,
                                      renew_list[i].token,
                                      renew_list[i].token_len,
                                      &new_expire);
        if (ret == 0) {
            /* Phase 3: 重新加锁, 按 stripe_start 查找并更新 */
            spin_lock(&pi->lease_lock);
            for (n = rb_first(&pi->lease_tree); n; n = rb_next(n)) {
                struct powerfs_lease *l =
                    rb_entry(n, struct powerfs_lease, node);
                if (l->stripe_start == renew_list[i].stripe_start) {
                    l->expire_jiffies = new_expire;
                    pr_debug("powerfs: lease renewed ino=%lu stripe=%llu\n",
                             inode->i_ino,
                             (unsigned long long)l->stripe_start);
                    break;
                }
            }
            spin_unlock(&pi->lease_lock);
        } else {
            /* 续约失败: 可能是连接断开导致 volume server 释放了 lease
             * (on_disconnect → disconnect_holder), 或者 lease 已过期被
             * cleanup_expired 清理. 重新获取 lease (acquire_lease). */
            char new_token[64];
            size_t new_token_len = sizeof(new_token);
            u64 new_epoch = 0;
            u64 new_content_size = 0;
            unsigned long new_expire = 0;
            int acq_ret;

            pr_warn("powerfs: lease renew failed ino=%lu stripe=%llu "
                    "err=%d, re-acquiring...\n",
                    inode->i_ino,
                    (unsigned long long)renew_list[i].stripe_start,
                    ret);

            acq_ret = powerfs_net_acquire_lease(volume_id, inode->i_ino,
                                                renew_list[i].stripe_start,
                                                1, NULL,
                                                new_token, &new_token_len,
                                                &new_epoch, &new_content_size,
                                                &new_expire);
            if (acq_ret == 0) {
                /* 重新获取成功: 更新 lease token 和 expire */
                spin_lock(&pi->lease_lock);
                for (n = rb_first(&pi->lease_tree); n; n = rb_next(n)) {
                    struct powerfs_lease *l =
                        rb_entry(n, struct powerfs_lease, node);
                    if (l->stripe_start == renew_list[i].stripe_start) {
                        size_t copy_len = min(new_token_len,
                                              sizeof(l->token) - 1);
                        memcpy(l->token, new_token, copy_len);
                        l->token[copy_len] = '\0';
                        l->epoch = new_epoch;
                        l->content_size = new_content_size;
                        l->expire_jiffies = new_expire;
                        pr_info("powerfs: lease re-acquired ino=%lu "
                                "stripe=%llu\n",
                                inode->i_ino,
                                (unsigned long long)l->stripe_start);
                        break;
                    }
                }
                spin_unlock(&pi->lease_lock);
            } else {
                pr_warn("powerfs: lease re-acquire failed ino=%lu "
                        "stripe=%llu err=%d\n",
                        inode->i_ino,
                        (unsigned long long)renew_list[i].stripe_start,
                        acq_ret);
            }
        }
    }

    /* 重调度: 在最早过期时间前 RENEW_THRESHOLD 触发下次续约.
     * 若没有 lease, 不重调度 (由 acquire 路径重新启动). */
    if (has_lease && !sbi->shutting_down && sbi->lease_wq) {
        unsigned long delay;

        if (time_before(now + POWERFS_LEASE_RENEW_THRESHOLD,
                        earliest_expiry)) {
            /* lease 还远未到续约时间 */
            delay = earliest_expiry - POWERFS_LEASE_RENEW_THRESHOLD - now;
        } else {
            /* lease 已在续约窗口内或已过期, 短延迟后重试 */
            delay = HZ;  /* 1s */
        }

        queue_delayed_work(sbi->lease_wq, &pi->lease_renew_work, delay);
    }
}

/* ========== Inode 创建辅助函数 ========== */

/*
 * powerfs_new_inode - 创建新的 inode
 *
 * 内部改用 powerfs_iget (基于 iget5_locked)
 * 这样 inode 会被加入内核 inode 哈希表，可以通过 ino 查找
 *
 * 注意: 调用者必须确保 ino 未被使用
 */
struct inode *powerfs_new_inode(struct super_block *sb, umode_t mode,
                                 u64 ino, u64 parent_ino, const char *name)
{
    struct inode *inode;
    int err;

    inode = powerfs_iget(sb, ino);
    if (IS_ERR(inode))
        return NULL;

    /* 如果是新创建的 inode，初始化它 */
    if (inode->i_state & I_NEW) {
        err = powerfs_init_inode(inode, mode, parent_ino, name);
        if (err) {
            iput(inode);
            return NULL;
        }
        unlock_new_inode(inode);
    } else {
        /* inode 已存在 - 这不应该发生在创建场景 */
        pr_warn("powerfs: new_inode called for existing inode %llu\n", ino);
        iput(inode);
        return NULL;
    }

    pr_debug("powerfs: new_inode ino=%llu mode=%o (%s)\n",
             ino, mode, name ? name : "");

    return inode;
}

/* ========== 目录操作: lookup ========== */

/*
 * powerfs_lookup - 查找文件/目录
 *
 * 策略:
 *   1. 快速路径: dentry 已有 inode (在 dcache 中)，直接返回
 *   2. 通信层可用: 通过代理查询后端 Filer
 *      - 找到: 创建 inode 并实例化 dentry
 *      - 未找到: 添加负 dentry
 *   3. 通信层不可用: 纯本地模式，添加负 dentry
 *
 * 注意: 不在 RCU read-side critical section 中做任何阻塞操作
 */
/*
 * powerfs_lookup - 查找文件/目录
 *
 * 策略 (更新版 - Delta Sync + powerfs_net):
 *   1. 快速路径: dentry 已有 inode (在 dcache 中)
 *   2. powerfs_net 连接可用: 直接通过 powerfs_net_lookup 查询
 *      - 找到: 创建 inode，设置 generation，记录路径
 *      - 未找到: 添加负 dentry
 *   3. powerfs_comm 连接可用 (兼容旧接口): 使用旧代理查询
 *   4. 纯本地模式: 添加负 dentry
 */
struct dentry *powerfs_lookup(struct inode *dir, struct dentry *dentry,
                               unsigned int flags)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct inode *inode;
    int err;
    u64 ts_entry, ts_net, net_dur_us, total_us;

    (void)flags;

    ts_entry = ktime_get_ns();

    pr_debug("powerfs: lookup '%pd' in dir=%lu\n", dentry, dir->i_ino);

    /*
     * VFS 契约: ->lookup 仅在 dentry 处于 DCACHE_PAR_LOOKUP (新鲜、
     * d_inode==NULL、d_alias/d_in_lookup_hash 共享 union 且当前由
     * in_lookup_hash 占用) 时调用 (见 namei.c __lookup_slow/__lookup_hash)。
     * 因此这里不需要、也不应该检查 d_inode(dentry) —— 如果它非空，
     * 说明 VFS 已被破坏，提前返回 NULL 只会把 dentry 留在 PAR_LOOKUP
     * 悬挂状态，进一步污染 dcache。交由 d_add 走正常实例化路径。
     */

    /* === powerfs_net 直接通信模式 === */
    if (powerfs_net_is_connected()) {
        __u64 ino = 0;
        __u32 mode = 0;
        __u32 uid = 0, gid = 0;
        __u64 size = 0;
        __u32 nlink = 0;
        __u64 mtime = 0, atime = 0, ctime = 0;
        __u64 volume_id = 0, file_key = 0;
        struct powerfs_file_layout lookup_layout;
        int timeout_ms;

        /* K3: 零初始化, 确保 err/ENOENT 路径 kfree(volume_ids=NULL) 安全 */
        memset(&lookup_layout, 0, sizeof(lookup_layout));

        pr_debug("powerfs: lookup '%pd' via powerfs_net\n", dentry);

        /* Phase 1: 超时策略.
         *
         * lookup 在 VFS 持有 dir->i_rwsem 读锁时调用, 长超时会阻塞
         * 整个目录的所有操作. 使用固定 2s 短超时:
         *   - 正常 RPC 在 LAN 内 <10ms, 2s 足够
         *   - 断连/filer 重启时快速返回 -EAGAIN, VFS 上层自动重试
         *   - 不使用 powerfs_net_pick_timeout (自适应 10s), 因为
         *     lookup 在 i_rwsem 内, 必须快速释放锁
         *
         * 重试由 VFS 上层处理 (namei.c __lookup_slow 循环),
         * 每次重试间隔由 VFS 调度, 提供自然退避. */
        timeout_ms = POWERFS_LOOKUP_TIMEOUT_MS;  /* 2s: short timeout under i_rwsem */

        /* 通过 powerfs_net 直接查询 (含时间戳 + volume_id/file_key + layout) */
        ts_net = ktime_get_ns();
        err = powerfs_net_lookup_timeout(dir->i_ino, dentry->d_name.name,
                                          strlen(dentry->d_name.name),
                                          &ino, &mode, &uid, &gid,
                                          &size, &nlink,
                                          &mtime, &atime, &ctime,
                                          &volume_id, &file_key,
                                          &lookup_layout,
                                          timeout_ms);
        net_dur_us = div_u64(ktime_get_ns() - ts_net, 1000);

        /* 断连/重连期间超时或网络不可达, 返回 -EAGAIN 让 VFS/应用层重试.
         * - ETIMEDOUT: 2s 内未收到响应 (filer 不可达或繁忙)
         * - ENOTCONN: disconnect_one 已 complete(-ENOTCONN) (在途请求被取消)
         * - ESHUTDOWN: pool 正在 stopping
         *
         * 2s 短超时策略: 不等待 filer 重启完成, 快速返回 -EAGAIN 让 VFS 重试.
         * VFS 的 __lookup_slow 循环会自动重试, 每次重试间隔由调度器决定,
         * filer 恢复后下一次 lookup 即可成功. 避免在 i_rwsem 读锁内长时间阻塞. */
        if (err == -ETIMEDOUT || err == -ENOTCONN || err == -ESHUTDOWN) {
            total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
            pr_warn("powerfs: LOOKUP '%pd' EAGAIN err=%d net=%lluus total=%lluus\n",
                    dentry, err, net_dur_us, total_us);
            return ERR_PTR(-EAGAIN);
        }

        if (err == 0 && ino != 0) {
            /* 找到文件: 创建 inode */
            pr_debug("powerfs: lookup '%pd' found ino=%llu mode=%o\n",
                     dentry, (unsigned long long)ino, mode);

            inode = powerfs_iget(dir->i_sb, ino);
            if (IS_ERR(inode)) {
                pr_warn("powerfs: lookup '%pd' iget failed: %ld\n",
                        dentry, PTR_ERR(inode));
                /* K3: iget 失败, 释放 parse 分配的 volume_ids/inline_data/replica_chunks */
                kfree(lookup_layout.volume_ids);
                kfree(lookup_layout.inline_data);
                kfree(lookup_layout.replica_chunks);
                kfree(lookup_layout.ec_chunks);
                d_add(dentry, NULL);
                return NULL;
            }

            if (inode->i_state & I_NEW) {
                /*
                 * 新 inode: 先用 powerfs_init_inode 设置 i_op/i_fop/a_ops
                 * 和默认属性。缺少这一步会导致 looked-up inode 的操作表为
                 * NULL，后续 open/read 等触发空指针解引用。
                 */
                powerfs_init_inode(inode, mode, dir->i_ino,
                                   dentry->d_name.name);
                /* 用 Filer 返回的权威属性覆盖默认值 */
                inode->i_mode = mode;
                inode->i_uid = make_kuid(&init_user_ns, uid);
                inode->i_gid = make_kgid(&init_user_ns, gid);
                inode->i_size = size;
                set_nlink(inode, nlink);
                inode_set_mtime(inode, mtime, 0);
                inode_set_atime(inode, atime, 0);
                inode_set_ctime(inode, ctime, 0);

                {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->cache_valid = true;
                    /* K2-11: Initialize content_size to the server's size so
                     * that refresh_work's != check works correctly. Without
                     * this, content_size stays at 0 (from alloc_inode) and
                     * refresh_work would always skip size updates. */
                    pi->content_size = size;
                    /* 数据直连: 存储 volume_id/file_key 用于 ReadNeedle/WriteNeedle.
                     * 目录的 volume_id/file_key 为 0 (无数据). */
                    pi->volume_id = volume_id;
                    pi->file_key = file_key;
                    /* K3: 应用 FileLayout (placement/volume_ids 等).
                     * Stripe 文件在 lookup 时即获取 volume_ids, 无需等待 getattr. */
                    powerfs_apply_layout_to_inode(pi, &lookup_layout);
                    spin_unlock(&pi->i_lock);
                }

                unlock_new_inode(inode);
            } else {
                struct powerfs_inode_info *pi = POWERFS_I(inode);
                u64 local_content_size;
                /* 已有 inode (非 I_NEW): d_revalidate 返回 0 触发 re-lookup.
                 * 用 Filer 返回的权威属性更新现有 inode, 否则跨客户端
                 * 修改后内核仍用旧的 i_size/volume_id/file_key, 导致
                 * 读取空内容 (size=0 跳过 read) 或 needle not found. */
                /* Read content_size before inode->i_lock (lock ordering). */
                spin_lock(&pi->i_lock);
                local_content_size = pi->content_size;
                spin_unlock(&pi->i_lock);

                spin_lock(&inode->i_lock);
                inode->i_mode = mode;
                inode->i_uid = make_kuid(&init_user_ns, uid);
                inode->i_gid = make_kgid(&init_user_ns, gid);
                /* Don't overwrite i_size if local content_size != server size:
                 * local truncate/write set a size that the server hasn't
                 * reflected yet (stale GETATTR). See refresh_work.
                 * K2-11: Changed from <= to == to handle both extend and
                 * truncate cases (local can be < or > server size). */
                if (local_content_size == size &&
                    i_size_read(inode) != size) {
                    i_size_write(inode, size);
                }
                set_nlink(inode, nlink);
                inode_set_mtime(inode, mtime, 0);
                inode_set_atime(inode, atime, 0);
                inode_set_ctime(inode, ctime, 0);
                spin_unlock(&inode->i_lock);

                {
                    spin_lock(&pi->i_lock);
                    /* K2-11: Only sync content_size when local matches server
                     * (no pending local truncate/write). */
                    if (local_content_size == size)
                        pi->content_size = size;
                    pi->cache_valid = true;
                    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
                    /* 仅在 Filer 返回非零值时更新 volume_id/file_key.
                     * close 前的 lookup 可能返回 0 (Filer 端 chunks 在 close 时才同步),
                     * 若用 0 覆盖已有值, 会导致 writeback/read locate 失败. */
                    if (volume_id != 0)
                        pi->volume_id = volume_id;
                    if (file_key != 0)
                        pi->file_key = file_key;
                    /* K3: 更新 FileLayout (placement 可能从 Flat 切换到 Stripe) */
                    powerfs_apply_layout_to_inode(pi, &lookup_layout);
                    spin_unlock(&pi->i_lock);
                }
                pr_debug("powerfs: lookup '%pd' updated existing inode ino=%llu size=%llu vid=%llu fkey=%llu\n",
                        dentry, (unsigned long long)ino,
                        (unsigned long long)size,
                        (unsigned long long)volume_id,
                        (unsigned long long)file_key);
            }

            /*
             * 实例化 dentry —— 必须使用 d_add，不能用 d_instantiate。
             *
             * 根因: ->lookup 期间 dentry 仍处于 DCACHE_PAR_LOOKUP，
             * d_alias 与 d_in_lookup_hash 共享 union (dcache.h:109-112)，
             * 此时 in_lookup_hash 已链入 in_lookup 哈希表，故 d_alias 也
             * 表现为 "hashed"。d_instantiate 直接 BUG_ON(!hlist_unhashed
             * (&d_alias)) 即触发 kernel BUG (dcache.c:2032)。
             *
             * d_add -> __d_add (dcache.c:2775) 先 d_in_lookup 检测并
             * __d_lookup_unhash 清理 in_lookup_hash，再 hlist_add_head
             * (&d_alias)，最后 __d_rehash 加入主 dcache 哈希。这是 ->lookup
             * 中实例化 dentry 的正确接口 (参考 xxx_lookup -> d_splice_alias
             * -> __d_add)。
             */
            d_add(dentry, inode);

            /* Per-dentry lease: lookup 成功后填充 (对齐  __update_dentry_lease).
             * 正 dentry: lease 有效期间信任缓存, d_revalidate Layer 1 命中. */
            powerfs_fill_dentry_lease(dentry, dir, 0);

            /* 目录级 lease: lookup 成功后续约父目录 lease.
             * 一次 RPC 同时完成查询+续约, 后续同目录的 d_revalidate
             * 全部 RCU 命中, 无网络交互. */
            WRITE_ONCE(POWERFS_I(dir)->dir_lease_expire,
                       jiffies + POWERFS_DIR_LEASE_TTL);

            pr_debug("powerfs: lookup '%pd' completed\n", dentry);
            total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
            pr_info_ratelimited("powerfs: LOOKUP '%pd' found ino=%llu net=%lluus total=%lluus\n",
                    dentry, (unsigned long long)ino, net_dur_us, total_us);
            if (sbi->client)
                powerfs_update_metadata_metrics(&sbi->client->metrics,
                                                ns_to_ktime(ts_entry), ktime_get(), 0);
            return NULL;
        }

        if (err == -ENOENT) {
            /* 文件不存在: 添加负 dentry.
             * 目录级 lease: 负 dentry 不再维护独立 TTL, 有效性依赖父目录 lease.
             * lookup 成功 (即使 ENOENT) 也续约父目录 lease. */
            pr_debug("powerfs: lookup '%pd' not found (powerfs_net)\n", dentry);
            /* K3: ENOENT 时 lookup_layout 已被 parse_file_layout 零初始化,
             * volume_ids/inline_data/replica_chunks 为 NULL (Filer 不对不存在的文件编码 layout), 安全. */
            kfree(lookup_layout.volume_ids);
            kfree(lookup_layout.inline_data);
            kfree(lookup_layout.replica_chunks);
            kfree(lookup_layout.ec_chunks);
            d_add(dentry, NULL);

            /* Per-dentry lease: 负 dentry 也填充 lease (对齐  负 dentry 信任).
             * 负 dentry lease 有效期间, d_revalidate Layer 1 命中 → 直接返回 ENOENT,
             * 无需 RPC. 对齐 Rust DentryLeaseStatus::LeaseValid (negative). */
            powerfs_fill_dentry_lease(dentry, dir, 0);
            /* 标记为负 dentry (用于 Layer 2 的 I_COMPLETE + ENOENT 信任) */
            {
                struct powerfs_dentry_info *di = POWERFS_D(dentry);
                if (di)
                    di->flags |= POWERFS_DN_NEGATIVE;
            }

            WRITE_ONCE(POWERFS_I(dir)->dir_lease_expire,
                       jiffies + POWERFS_DIR_LEASE_TTL);
            total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
            pr_info_ratelimited("powerfs: LOOKUP '%pd' enoent net=%lluus total=%lluus\n",
                    dentry, net_dur_us, total_us);
            if (sbi->client)
                powerfs_update_metadata_metrics(&sbi->client->metrics,
                                                ns_to_ktime(ts_entry), ktime_get(), 0);
            return NULL;
        }

        /* 其他错误: 返回错误给 VFS, 不缓存负 dentry (避免误缓存 "不存在").
         * Phase 1: 之前是 "记录 + 添加负 dentry", 这会让网络瞬态错误被
         * 当成 "文件不存在" 缓存, 后续访问该文件继续返回 ENOENT.
         * 现在返回错误, 让 VFS/userspace 看到真实错误码并重试. */
        total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
        pr_warn("powerfs: LOOKUP '%pd' error=%d net=%lluus total=%lluus\n",
                dentry, err, net_dur_us, total_us);
        /* K3: err 路径 lookup_layout 已零初始化, kfree(NULL) 安全 */
        kfree(lookup_layout.volume_ids);
        kfree(lookup_layout.inline_data);
        kfree(lookup_layout.replica_chunks);
        kfree(lookup_layout.ec_chunks);
        return ERR_PTR(err);
    }

    /* === 纯本地模式: 添加负 dentry === */
    pr_debug("powerfs: lookup '%pd' not found (local mode)\n", dentry);
    d_add(dentry, NULL);
    if (sbi->client)
        powerfs_update_metadata_metrics(&sbi->client->metrics,
                                        ns_to_ktime(ts_entry), ktime_get(), 0);
    return NULL;
}

/* ========== P1-2: create 核心 helper (供 mknod 和 atomic_open 共享) ========== */

/*
 * __powerfs_do_create_core - 原子创建 inode (Filer RPC + 本地 inode 构造)
 *
 * 与 powerfs_mknod 的区别：
 *   - 不 d_instantiate()、不加 dir_entry、不更新父目录时间戳
 *   - 只负责 net_create → new_inode → 应用 layout 的核心路径
 *   - 返回新创建的 inode (带引用计数，调用者负责 iput/d_instantiate)
 *
 * 用于 atomic_open: 把 "lookup→create→open" 合成一个回调，避免 VFS
 * 在 lookup 和 create 之间的窗口 (TOCTOU) 被其他客户端抢先。
 */
static struct inode *__powerfs_do_create_core(struct mnt_idmap *idmap,
                                               struct inode *dir,
                                               struct dentry *dentry,
                                               umode_t mode, dev_t dev)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct inode *inode;
    u64 new_ino;
    u64 mknod_volume_id = 0, mknod_file_key = 0;
    struct powerfs_file_layout mknod_layout;
    bool mknod_has_layout = false;

    (void)idmap;
    (void)dev;

    if (S_ISREG(mode) || S_ISDIR(mode) || S_ISFIFO(mode) ||
        S_ISBLK(mode) || S_ISCHR(mode) || S_ISSOCK(mode)) {
        u64 remote_ino = 0;
        u64 volume_id = 0, file_key = 0;
        struct powerfs_file_layout layout = {0};
        int rerr = powerfs_net_create(dir->i_ino, dentry->d_name.name,
                                       dentry->d_name.len, mode,
                                       from_kuid(&init_user_ns, current_fsuid()),
                                       from_kgid(&init_user_ns, current_fsgid()),
                                       S_ISDIR(mode), &remote_ino,
                                       &volume_id, &file_key, &layout);
        if (rerr) {
            pr_warn("powerfs: net_create '%pd' failed: %d\n", dentry, rerr);
            return ERR_PTR(rerr);
        }
        new_ino = remote_ino ? remote_ino
                             : (u64)atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
        mknod_volume_id = volume_id;
        mknod_file_key = file_key;
        mknod_layout = layout;
        mknod_has_layout = true;
    } else {
        new_ino = atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
    }

    inode = powerfs_new_inode(dir->i_sb, mode, new_ino,
                               dir->i_ino, dentry->d_name.name);
    if (!inode)
        return ERR_PTR(-ENOSPC);

    if (S_ISDIR(mode)) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        WRITE_ONCE(pi->dir_complete, true);
        pi->i_flags |= POWERFS_I_COMPLETE;
        WRITE_ONCE(pi->dir_lease_expire, jiffies + POWERFS_DIR_LEASE_TTL);
    }

    if (S_ISREG(mode) && mknod_volume_id != 0) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        spin_lock(&pi->i_lock);
        pi->volume_id = mknod_volume_id;
        pi->file_key = mknod_file_key;
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: do_create_core '%pd' ino=%lu volume_id=%llu file_key=%llu\n",
                 dentry, inode->i_ino, mknod_volume_id, mknod_file_key);
    }

    if (S_ISREG(mode) && mknod_has_layout) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        spin_lock(&pi->i_lock);
        powerfs_apply_layout_to_inode(pi, &mknod_layout);
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: do_create_core '%pd' ino=%lu placement=%u reliability=%u\n",
                 dentry, inode->i_ino, pi->placement, pi->reliability);
    } else if (mknod_has_layout && (mknod_layout.volume_ids || mknod_layout.inline_data ||
                                    mknod_layout.replica_chunks || mknod_layout.ec_chunks)) {
        kfree(mknod_layout.volume_ids);
        kfree(mknod_layout.inline_data);
        kfree(mknod_layout.replica_chunks);
        kfree(mknod_layout.ec_chunks);
    }

    return inode;
}

/* ========== 目录操作: mknod (通用创建函数) ========== */

/*
 * powerfs_mknod - 创建文件/目录/设备节点 (内部通用函数)
 *
 * 参考 ramfs_mknod (fs/ramfs/inode.c)
 */
static int powerfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
                          struct dentry *dentry, umode_t mode, dev_t dev)
{
    struct inode *inode;
    u64 new_ino;
    int type;
    int ret;

    pr_debug("powerfs: mknod '%pd' mode=%o in dir=%lu\n",
             dentry, mode, dir->i_ino);

    /* P2-7: Quota check — 文件数配额 */
    ret = powerfs_quota_check_max_files(dir);
    if (ret)
        return ret;

    /* 核心 create 逻辑 (P1-2 共享 helper):
     *   net_create (Filer Raft) → new_inode (本地) → 应用 layout.
     * 失败直接返回 errno; 成功返回带引用的 inode. */
    inode = __powerfs_do_create_core(idmap, dir, dentry, mode, dev);
    if (IS_ERR(inode)) {
        int rerr = PTR_ERR(inode);
        pr_warn("powerfs: mknod '%pd' do_create_core failed: %d\n", dentry, rerr);
        return rerr;
    }
    new_ino = inode->i_ino;

    /* 关联 dentry 和 inode (d_instantiate，不重复 hash) */
    d_instantiate(dentry, inode);

    /* 更新父目录时间戳 */
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 添加目录项到本地链表 */
    if (S_ISREG(mode))
        type = S_IFREG;
    else if (S_ISDIR(mode))
        type = S_IFDIR;
    else if (S_ISLNK(mode))
        type = S_IFLNK;
    else
        type = mode & S_IFMT;

    powerfs_add_dir_entry(dir, new_ino, type, dentry->d_name.name);

    /* 本地 mutation 清父目录 lease */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: mknod '%pd' success, ino=%llu\n",
             dentry, new_ino);

    return 0;
}

/* ========== 目录操作: mkdir ========== */

static struct dentry *powerfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                          struct dentry *dentry, umode_t mode)
{
    struct inode *inode;
    int err;

    pr_debug("powerfs: mkdir '%pd' in dir=%lu mode=%o\n",
            dentry, dir->i_ino, mode);

    /*
     * 本地操作 + 远程同步通知:
     *   1. 在本地创建 inode 和 dentry
     *   2. 通过 powerfs_net 同步到远端（Delta Sync）
     *   3. 通过 powerfs_comm 异步通知代理（兼容旧接口）
     */
    err = powerfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);
    if (err) {
        pr_warn("powerfs: mkdir '%pd' mknod failed: %d\n", dentry, err);
        return ERR_PTR(err);
    }

    /* 增加父目录 nlink (".." 指向父目录) */
    inc_nlink(dir);

    /* 获取新创建的 inode 用于远程同步 */
    inode = d_inode(dentry);

    pr_debug("powerfs: mkdir '%pd' success, ino=%lu\n",
            dentry, inode ? inode->i_ino : 0);

    return NULL;
}

/* ========== 目录操作: rmdir ========== */

static int powerfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    int rerr;

    pr_debug("powerfs: rmdir '%pd' in dir=%lu\n", dentry, dir->i_ino);

    /* 简单检查 (VFS 也会检查，这里双重保险) */
    if (!d_is_dir(dentry))
        return -ENOTDIR;

    if (!inode) {
        pr_warn("powerfs: rmdir '%pd' no inode\n", dentry);
        return -ENOENT;
    }

    /*
     * 先向 filer 发 RMDIR 持久化删除, 成功后才改本地数据结构.
     * 断连时 powerfs_net_unlink 返回 -ENOTCONN, 操作失败, 本地不变.
     * -ENOENT 视为幂等成功 (目录已在 filer 端删除).
     */
    rerr = powerfs_net_unlink(dir->i_ino, dentry->d_name.name,
                               dentry->d_name.len, true);
    if (rerr && rerr != -ENOENT) {
        pr_warn("powerfs: net_rmdir '%pd' failed: %d\n", dentry, rerr);
        return rerr;
    }

    /* === 以下操作仅在 filer 删除成功后执行 === */

    /* 更新父目录时间戳 */
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 减少被删除目录的链接数 (参考 simple_rmdir, 不 ihold/iput) */
    drop_nlink(inode);

    /* 清空子目录的目录项链表 */
    powerfs_clear_dir_entries(inode);

    /* Invalidate the removed directory's own lease. Without this, the inode
     * may stay in the inode cache (dentry references) with stale
     * dir_complete=true + future dir_lease_expire. When the Filer reuses
     * this inode number for a new directory, iget5_locked returns the cached
     * inode, and readdir takes the fast-path with an empty dir_entries list,
     * skipping the Filer fetch (T3b/T3c/T9a/T9b "Directory not empty"). */
    powerfs_invalidate_dir_lease(inode);

    /* 从父目录的本地目录项链表中移除 */
    powerfs_remove_dir_entry(dir, dentry->d_name.name);

    /* 减少父目录的链接数 (因删除了一个子目录) */
    drop_nlink(dir);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: rmdir '%pd' success\n", dentry);

    /*
     * 注意: 不要在这里 iput(inode)
     *       让 do_rmdir 的 iput 统一处理
     */
    return 0;
}

/* ========== 目录操作: create ========== */

static int powerfs_create(struct mnt_idmap *idmap, struct inode *dir,
                           struct dentry *dentry, umode_t mode, bool excl)
{
    struct inode *inode;
    int err;

    (void)excl;

    pr_debug("powerfs: create '%pd' in dir=%lu mode=%o\n",
             dentry, dir->i_ino, mode);

    /*
     * 本地操作 + 远程同步通知:
     *   1. 在本地创建 inode 和 dentry
     *   2. 通过 powerfs_net 触发 Delta Sync 失效
     *   3. 通过 powerfs_comm 异步通知代理（兼容旧接口）
     *
     * dget 锁定 dentry 后，stat() 直接从 dcache 查找，不触发 lookup，
     * 避免 lookup 时代理还没处理完 CREATE 导致 -ENOENT 的问题
     */
    err = powerfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
    if (err) {
        pr_warn("powerfs: create '%pd' mknod failed: %d\n", dentry, err);
        return err;
    }

    /* 获取新创建的 inode 用于通知 */
    inode = d_inode(dentry);

    pr_debug("powerfs: create '%pd' success, ino=%lu\n",
             dentry, inode ? inode->i_ino : 0);

    return 0;
}

/* ========== P1-2: atomic_open (原子创建+打开，消除 TOCTOU) ========== */

/*
 * powerfs_atomic_open - 把 VFS "lookup → create → open" 三步合成一次回调
 *
 * 解决的核心问题 (对齐  xxx_atomic_open):
 *   - 原先 VFS: lookup(负 dentry ENOTDIR) → schedule → 另一客户端同文件名 CREATE
 *     → 回到本客户端 → .create 收到 -EEXIST，但 VFS 期望 O_CREAT|O_EXCL 时
 *     才返回 EEXIST，普通 O_CREAT 应当静默返回已存在文件; 反之亦然
 *   - 竞态窗口 (TOCTOU): tar/gcc 编译临时文件、flock(O_CREAT|O_EXCL) 互斥
 *     NFSv4 export 原子 open CREATE 全部因此失败
 *
 * 实现要点:
 *   1. flags &= ~O_TRUNC: atomic_open 先不 truncate，VFS 在后续权限检查后
 *      用 notify_change 单独 truncate，避免越权
 *   2. O_CREAT 分支: 调用共享的 __powerfs_do_create_core (Filer 原子 RPC)，
 *      用 d_splice_alias 把 inode 拼到 dentry，d_splice_alias 内部已处理
 *      in-lookup/hashed 两种 dentry 状态
 *   3. 最后调用 finish_open(): 一次性把 file->private_data 绑定 + 调
 *      powerfs_file_open 拿 cap 引用，VFS 不再进入后续 ->create 回调
 *   4. 若 !O_CREAT 且 dentry 不在 lookup 态 (已 hashed 的负 dentry)，直接
 *      -ENOENT；若 d_in_lookup 则 d_add(NULL) 完成 lookup，VFS 走后续
 *      普通路径
 */
static int powerfs_atomic_open(struct inode *dir, struct dentry *dentry,
                                struct file *file, unsigned open_flag,
                                umode_t create_mode)
{
    /* 6.17 atomic_open 不提供 mnt_idmap 形参, 用 file_mnt_idmap() 自取.
     * 对齐  xxx_atomic_open file.c L779: idmap = file_mnt_idmap(file). */
    struct mnt_idmap *idmap = file_mnt_idmap(file);
    struct inode *inode = NULL;
    int err;
    unsigned flags = open_flag;

    if (dentry->d_name.len > NAME_MAX)
        return -ENAMETOOLONG;

    /* O_TRUNC 在权限检查之后由 VFS 做，这里先清除 */
    flags &= ~O_TRUNC;

    pr_debug("powerfs: atomic_open '%pd' in dir=%lu flags=0x%x mode=0%o d_in_lookup=%d\n",
             dentry, dir->i_ino, flags, create_mode, d_in_lookup(dentry));

    if (flags & O_CREAT) {
        /* —— 原子创建分支: 复用 __powerfs_do_create_core —— */
        int type;
        struct dentry *dn;

        /* FIX BUG at dcache.c:2993: d_splice_alias() requires d_unhashed(dentry).
         * When VFS calls atomic_open after a prior LOOKUP that instantiated a
         * hashed NEGATIVE dentry (via d_add(NULL)), dentry is already hashed and
         * BUG_ON(!d_unhashed(dentry)) fires.  d_drop removes the stale neg
         * dentry from dcache hash, restoring unhashed state required by
         * d_splice_alias (matches NFS/overlayfs atomic_open idiom). */
        if (!d_unhashed(dentry))
            d_drop(dentry);

        inode = __powerfs_do_create_core(idmap, dir, dentry,
                                          create_mode | S_IFREG, 0);
        if (IS_ERR(inode)) {
            int rerr = PTR_ERR(inode);
            inode = NULL;
            /* -EEXIST 视为文件已存在 (O_EXCL 时向上抛，否则走已存在路径) */
            if (rerr == -EEXIST && !(flags & O_EXCL)) {
                pr_debug("powerfs: atomic_open '%pd' EEXIST,!O_EXCL → treat as existing\n",
                         dentry);
                goto existing_lookup;
            }
            pr_warn("powerfs: atomic_open '%pd' do_create_core failed: %d\n",
                    dentry, rerr);
            return rerr;
        }

        /* d_splice_alias: 把新 inode 拼到 dentry，兼容 d_in_lookup / hashed
         * 两种态。若返回 alias dentry，也能用（ 做法 WARN_ON != dentry） */
        dn = d_splice_alias(inode, dentry);
        WARN_ON_ONCE(dn && dn != dentry);
        inode = NULL; /* d_splice_alias 已转移引用 */

        /* 尾部工作 (与 mknod 对称): 更新父目录时间戳 + dir_entry + 清 lease */
        {
            struct timespec64 now = current_time(dir);
            inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
            inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
        }
        type = S_IFREG;
        powerfs_add_dir_entry(dir, d_inode(dentry)->i_ino, type, dentry->d_name.name);
        powerfs_invalidate_dir_lease(dir);

        /* 标记 file: 通知上层这个 file 是这次刚创建的 */
        file->f_mode |= FMODE_CREATED;

        /* finish_open: 调 powerfs_file_open 拿 cap 引用 + 绑定 file 私有结构
         *  file.c L982: err = finish_open(file, dentry, xxx_open) */
        err = finish_open(file, dentry, powerfs_file_open);
        pr_debug("powerfs: atomic_open '%pd' CREATE+OPEN finish_open err=%d f_mode_created=%d\n",
                 dentry, err, !!(file->f_mode & FMODE_CREATED));
        return err;
    }

existing_lookup:
    /* —— 非创建分支: LOOKUP + OPEN (或 EEXIST fallthrough) —— */
    if (!d_in_lookup(dentry)) {
        /* VFS 传入的是已 hashed 的负 dentry，且没 O_CREAT → 文件不存在 */
        return -ENOENT;
    }

    /* d_in_lookup: 通过 d_add(NULL) 把它实例化为负 dentry，
     * 然后 finish_no_open(file, NULL) 通知 VFS：atomic_open 没把 open
     * 做完，请走普通 lookup→open 路径 ( file.c L964) */
    d_add(dentry, NULL);
    return finish_no_open(file, NULL);
}

/* ========== 目录操作: unlink ========== */

static int powerfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    int rerr;

    pr_debug("powerfs: unlink '%pd' in dir=%lu\n", dentry, dir->i_ino);

    if (!inode) {
        pr_warn("powerfs: unlink '%pd' no inode\n", dentry);
        return -ENOENT;
    }

    /*
     * 先向 filer 发 UNLINK 持久化删除, 成功后才改本地数据结构.
     * 断连时 powerfs_net_unlink 返回 -ENOTCONN, 操作失败, 本地不变.
     * -ENOENT 视为幂等成功 (文件已在 filer 端删除).
     * 之前是"先改本地再发网络", 断连时本地已改但 filer 未删 → 不一致.
     */
    rerr = powerfs_net_unlink(dir->i_ino, dentry->d_name.name,
                               dentry->d_name.len, false);
    if (rerr && rerr != -ENOENT) {
        pr_warn("powerfs: net_unlink '%pd' failed: %d\n", dentry, rerr);
        return rerr;
    }

    /* === 以下操作仅在 filer 删除成功后执行 === */

    /* 更新父目录时间戳 */
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 减少 inode 链接数 (参考 simple_unlink, 不 ihold/iput, 由 VFS dentry 管理) */
    drop_nlink(inode);

    /* 从本地目录项链表中移除 */
    powerfs_remove_dir_entry(dir, dentry->d_name.name);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: unlink '%pd' success\n", dentry);

    return 0;
}

/* ========== 目录操作: symlink ========== */

static int powerfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
                            struct dentry *dentry, const char *symname)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct inode *inode;
    struct powerfs_inode_info *pi;
    u64 new_ino;
    int err;

    (void)idmap;

    pr_debug("powerfs: symlink '%pd' -> '%s'\n", dentry, symname);

    /*
     * 先向 filer 发 SYMLINK 持久化, 成功后才创建本地 inode.
     * 断连时 powerfs_net_symlink 返回 -ENOTCONN, 操作失败.
     */
    {
        __u64 sym_ino = 0;
        int nret = powerfs_net_symlink(dir->i_ino,
                                        dentry->d_name.name,
                                        dentry->d_name.len,
                                        symname, strlen(symname),
                                        &sym_ino);
        if (nret < 0) {
            pr_warn("powerfs: symlink net_sync name=%s failed: %d\n",
                    dentry->d_name.name, nret);
            return nret;
        }
        new_ino = sym_ino ? sym_ino
                          : (u64)atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
    }

    /* === 以下操作仅在 filer 创建成功后执行 === */

    /* 创建符号链接 inode */
    inode = powerfs_new_inode(dir->i_sb, S_IFLNK | 0777, new_ino,
                               dir->i_ino, dentry->d_name.name);
    if (!inode)
        return -ENOSPC;

    pi = POWERFS_I(inode);

    /* 使用 page cache 存储符号链接目标 */
    err = page_symlink(inode, symname, strlen(symname) + 1);
    if (err) {
        iput(inode);
        return err;
    }

    /* Filer 会将 symlink target 存储为 InlineData (placement=Inline).
     * 本地也存储一份到 inline_data, 以便 page cache 被 evict 后
     * netfs_issue_read 能从 inline_data 恢复 (而非回退到 volume_read
     * 导致 -EINVAL, 因为 Inline 文件无 needle). */
    {
        size_t target_len = strlen(symname) + 1;
        u8 *buf = kvmalloc(target_len, GFP_KERNEL);
        if (buf) {
            memcpy(buf, symname, target_len);
            spin_lock(&pi->i_lock);
            kfree(pi->inline_data);
            pi->inline_data = buf;
            pi->inline_len = target_len;
            pi->placement = POWERFS_PLACEMENT_INLINE;
            pi->inline_dirty = false;  /* Filer 已知 target */
            spin_unlock(&pi->i_lock);
        }
    }

    /* 关联 dentry 和 inode (用 d_instantiate, 不能用 d_add — 见 powerfs_mknod 注释) */
    d_instantiate(dentry, inode);

    /* 更新父目录时间戳 */
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 添加目录项到本地链表 */
    powerfs_add_dir_entry(dir, new_ino, S_IFLNK, dentry->d_name.name);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: symlink '%pd' success, ino=%llu\n",
             dentry, (unsigned long long)new_ino);

    return 0;
}

/* ========== 目录操作: readlink ========== */

/*
 * powerfs_readlink - 读取符号链接目标
 *
 * 参考 xxx_readlink (fs/xxx/symlink.c)
 *
 * 策略:
 *   - 通信层可用时: 优先从服务端获取最新目标路径
 *   - 纯内存模式: 直接从 page cache 读取
 */
int powerfs_readlink(struct dentry *dentry, char *buffer, int buflen)
{
    struct inode *inode = d_inode(dentry);
    struct powerfs_inode_info *pi;
    struct page *page;
    void *page_addr;
    u32 len;

    pr_debug("powerfs: readlink '%pd'\n", dentry);

    if (!inode)
        return -ENOENT;

    if (!S_ISLNK(inode->i_mode))
        return -EINVAL;

    if (inode->i_size == 0) {
        /* Symlink size may be 0 if created by an older Filer that didn't
         * set size=target.len(). Fall through to inline_data / GETATTR
         * instead of returning empty string. */
        pr_debug("powerfs: readlink '%pd' size=0, trying inline/getattr\n", dentry);
    }

    len = min_t(u32, (u64)inode->i_size, buflen - 1);
    if (len == 0)
        len = buflen - 1;  /* fallback: allow GETATTR to fill the target */

    /* 1. Try page cache first (fast path, works before remount). */
    page = find_get_page(inode->i_mapping, 0);
    if (!page) {
        pi = POWERFS_I(inode);

        /* 2. Check inline_data (set by powerfs_symlink or GETATTR response). */
        spin_lock(&pi->i_lock);
        if (pi->inline_data && pi->inline_len > 0) {
            u32 copy_len = min_t(u32, pi->inline_len, len);
            memcpy(buffer, pi->inline_data, copy_len);
            buffer[copy_len] = '\0';
            spin_unlock(&pi->i_lock);
            pr_debug("powerfs: readlink '%pd' from inline_data: '%s'\n",
                     dentry, buffer);
            return 0;
        }
        spin_unlock(&pi->i_lock);

        /* 3. No inline_data: fetch via GETATTR (symlink target is stored as
         * inline_data on the Filer, but LOOKUP response may not include it).
         * After GETATTR, inline_data is populated and we can read from it. */
        {
            struct powerfs_file_layout layout = {0};
            __u32 mode = 0, uid = 0, gid = 0, nlink = 0;
            __u64 size = 0, mtime = 0, atime = 0, ctime = 0;
            __u64 volume_id = 0, file_key = 0;
            int ret;

            ret = powerfs_net_getattr(inode->i_ino, &mode, &uid, &gid,
                                      &size, &nlink, &mtime, &atime, &ctime,
                                      &volume_id, &file_key, &layout,
                                      NULL, NULL, NULL, NULL, NULL);  /* P1-5: readlink 不需要 rstat */
            if (ret == 0) {
                spin_lock(&pi->i_lock);
                powerfs_apply_layout_to_inode(pi, &layout);
                if (pi->inline_data && pi->inline_len > 0) {
                    u32 copy_len = min_t(u32, pi->inline_len, len);
                    memcpy(buffer, pi->inline_data, copy_len);
                    buffer[copy_len] = '\0';
                    spin_unlock(&pi->i_lock);
                    pr_debug("powerfs: readlink '%pd' from GETATTR inline: '%s'\n",
                             dentry, buffer);
                    kfree(layout.volume_ids);
                    kfree(layout.replica_chunks);
                    kfree(layout.ec_chunks);
                    return 0;
                }
                spin_unlock(&pi->i_lock);
            }
            kfree(layout.volume_ids);
            kfree(layout.inline_data);
            kfree(layout.replica_chunks);
            kfree(layout.ec_chunks);
        }

        /* 4. GETATTR didn't return inline_data: try read_cache_page as
         * last resort (triggers netfs_read_folio for Flat files). */
        page = read_cache_page(inode->i_mapping, 0, NULL, NULL);
        if (IS_ERR(page)) {
            pr_warn("powerfs: readlink '%pd' read_cache_page failed: %ld\n",
                    dentry, PTR_ERR(page));
            buffer[0] = '\0';
            return 0;
        }
    }

    page_addr = kmap(page);
    memcpy(buffer, page_addr, len);
    buffer[len] = '\0';
    kunmap(page);
    put_page(page);

    pr_debug("powerfs: readlink '%pd' from cache: '%s'\n", dentry, buffer);
    return 0;
}

/* ========== 目录操作: link (硬链接) ========== */

/*
 * powerfs_link - 创建硬链接
 *
 * 参考 xxx_link (fs/xxx/dir.c)
 *
 * 策略 (本地操作 + 异步通知，与mkdir/create/unlink保持一致):
 *   1. 本地增加 nlink 和 dentry
 *   2. 异步通知代理增加后端硬链接计数
 */
static int powerfs_link(struct dentry *old_dentry, struct inode *dir,
                        struct dentry *new_dentry)
{
    struct inode *inode = d_inode(old_dentry);
    int nret;

    pr_debug("powerfs: link '%pd' -> '%pd' (ino=%lu)\n",
             old_dentry, new_dentry, inode->i_ino);

    /* 不允许对目录创建硬链接 */
    if (S_ISDIR(inode->i_mode))
        return -EPERM;

    /*
     * 先向 filer 发 LINK 持久化, 成功后才改本地数据结构.
     * 断连时 powerfs_net_link 返回 -ENOTCONN, 操作失败, 本地不变.
     */
    nret = powerfs_net_link(inode->i_ino, dir->i_ino,
                             new_dentry->d_name.name,
                             new_dentry->d_name.len);
    if (nret < 0) {
        pr_warn("powerfs: link net_sync name=%s failed: %d\n",
                new_dentry->d_name.name, nret);
        return nret;
    }

    /* === 以下操作仅在 filer link 成功后执行 === */

    /*
     * VFS (vfs_link) 不会自动 inc_nlink, 需要文件系统自己调用.
     * d_instantiate 不递增 i_count! 每个 dentry 必须持有独立的 i_count,
     * 参考 simple_link / ramfs_link 均在 d_instantiate 前调用 ihold.
     * 用 d_instantiate (不能用 d_add): new_dentry 已被 lookup 路径加入 hash 链.
     */
    inc_nlink(inode);
    ihold(inode);
    d_instantiate(new_dentry, inode);

    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 添加目录项到本地链表 */
    powerfs_add_dir_entry(dir, inode->i_ino,
                          inode->i_mode & S_IFMT,
                          new_dentry->d_name.name);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    return 0;
}

/* ========== getattr ========== */

/*
 * powerfs_getattr - 获取文件属性
 *
 * 参考 xxx_getattr (fs/xxx/inode.c)
 *
 * 策略:
 *   - 通信层可用时: 优先从服务端获取最新属性
 *   - 纯内存模式: 直接使用本地 inode 缓存
 */
static int powerfs_getattr(struct mnt_idmap *idmap, const struct path *path,
                            struct kstat *stat, u32 request_mask,
                            unsigned int query_flags)
{
    struct inode *inode = d_inode(path->dentry);
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    (void)idmap;
    (void)query_flags;

    pr_debug("powerfs: getattr '%pd'\n", path->dentry);

    /* 使用 VFS 通用属性获取 (6.17: 增加 request_mask 参数) */
    generic_fillattr(idmap, request_mask, inode, stat);

    /* P1-5: 目录 inode → 用递归聚合 rstat 覆盖 blocks/size。
     *   blocks = (rbytes + 511)/512     (du -s 核心依据)
     *   size   = rbytes                 (ls -l 目录显示的"大小"近似 du)
     * 若 Filer 未编码 rstat (pi->i_rbytes == 0)，退化为使用本地 i_size
     * 计算 blocks，与原行为一致，保证"结构打通前后数字不会变差"。 */
    if (S_ISDIR(inode->i_mode)) {
        __u64 rbytes;
        spin_lock(&pi->i_lock);
        rbytes = pi->i_rbytes;
        spin_unlock(&pi->i_lock);
        if (rbytes > 0) {
            stat->size = rbytes;
            stat->blocks = (rbytes + 511) / 512;
        } else {
            stat->blocks = (i_size_read(inode) + 511) / 512;
        }
    } else {
        /* 文件 inode: blocks 按 512-byte sectors 计算 (与 POSIX stat 定义一致) */
        stat->blocks = (i_size_read(inode) + 511) / 512;
    }
    stat->blksize = 4096;

    return 0;
}

/* ========== permission ========== */

/*
 * powerfs_permission - inode 权限校验
 *
 * 对齐  xxx_permission (inode.c L3063):
 *   1. MAY_NOT_BLOCK: 直接返回 -ECHILD (rcu 模式下不做网络调用)
 *   2. 阻塞获取 AUTH_SHARED cap — 保证 mode/uid/gid 等授权属性最新
 *   3. 调 generic_permission 按 VFS 通用规则做位校验
 *
 * 注意: 当前 AUTH_SHARED 为内存态 (不强制网络往返),
 *       cap grant 接入 powerfs-net 后会自动生效 (服务端 push attrs).
 */
int powerfs_permission(struct mnt_idmap *idmap, struct inode *inode, int mask)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = 0;
    int err;

    /* RCU walk 模式 (inode 不可上锁, 不允许阻塞) — 强制回退到 ref-walk */
    if (mask & MAY_NOT_BLOCK)
        return -ECHILD;

    /* 拿 AUTH_SHARED: 本地缓存或服务端 attrs 保证授权信息是最新的.
     * get_caps 失败 (如 -ENOTCONN) 不立刻拒绝, 仍做本地位校验. */
    err = powerfs_get_caps(inode, NULL, POWERFS_CAP_AUTH_SHARED,
                           POWERFS_CAP_AUTH_SHARED, 0, &got);
    if (err == 0) {
        /* get_caps 内部已调 cap_get_refs, 对称释放 */
        powerfs_cap_put_refs(pi, got);
    }

    /* 标准 VFS 位校验 (含 ACL/ capability 叠加) */
    return generic_permission(idmap, inode, mask);
}

/* ========== setattr ========== */

/*
 * powerfs_setattr - 设置文件属性
 *
 * 参考 xxx_setattr (__xxx_setattr) (fs/xxx/inode.c)
 *
 * 策略:
 *   - 通信层可用时: 先向服务端发送 SETATTR 请求，成功后更新本地缓存
 *   - 纯内存模式: 直接修改本地 inode 属性
 *
 * 支持的属性:
 *   - ATTR_MODE: 文件权限
 *   - ATTR_UID/ATTR_GID: 所有者
 *   - ATTR_SIZE: 文件大小 (truncate)
 *   - ATTR_ATIME/ATTR_MTIME: 时间戳
 */
int powerfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                    struct iattr *attr)
{
    struct inode *inode = d_inode(dentry);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int err;
    unsigned int ia_valid;

    (void)idmap;

    pr_debug("powerfs: setattr '%pd' ia_valid=0x%x\n", dentry, attr->ia_valid);
    pr_debug("powerfs: SETATTR ino=%lu ia_valid=0x%x ia_size=%lld cur_size=%lld\n",
            inode->i_ino, attr->ia_valid,
            (attr->ia_valid & ATTR_SIZE) ? attr->ia_size : -1,
            i_size_read(inode));

    /*
     * 纯本地操作 + 异步通知:
     *   1. 先在本地修改 inode 属性
     *   2. 异步通知代理更新后端记录
     *
     * 同步通信会导致高并发下超时和卡顿，改为异步通知模式
     */
    ia_valid = attr->ia_valid;

    /* 第一步: 执行 VFS 通用 setattr 处理 (权限检查等) */
    err = setattr_prepare(idmap, dentry, attr);
    if (err)
        return err;

    /* 处理文件大小变更
     *
     * 注意: setattr_copy 不会设置 i_size (它只处理 uid/gid/mode/time).
     * 必须调用 truncate_setsize 来同时截断 page cache 并更新 i_size.
     * 之前只调 truncate_pagecache 导致 i_size 未更新, O_TRUNC 后
     * i_size 仍为旧值, O_APPEND 写到了错误的 offset.
     */
    if (ia_valid & ATTR_SIZE) {
        loff_t old_size = i_size_read(inode);

        truncate_setsize(inode, attr->ia_size);
        /* 同步 pi->content_size, 否则 write_end 的
         * (new_size != pi->content_size) 检查会误判为 "未变化"
         * 而跳过 net_setattr, 导致 O_TRUNC 后的写入不持久化 size.
         * 例: 文件原 size=4, O_TRUNC 设 size=0, 再写 4 字节:
         *   new_size=4 == pi->content_size=4 → 跳过 setattr
         *   Filer 端 size 仍为 0 → remount 后文件为空 */
        pi->content_size = attr->ia_size;

        /* K2: Inline 文件 truncate 时必须同步 inline_data.
         * O_TRUNC (size=0): 释放 inline_data, 清除 dirty 标记.
         *   否则旧 inline_data 残留, dd '>' 覆盖旧文件时:
         *   - inline_len 仍为旧值 (如 4096)
         *   - write_end 中 inline_len >= need_len, 不重新分配
         *   - 旧 inline_data buffer 被部分覆盖, 数据不一致
         * truncate 到非 0: 截断 inline_data 到新大小 */
        if (pi->placement == POWERFS_PLACEMENT_INLINE) {
            spin_lock(&pi->i_lock);
            if (attr->ia_size == 0) {
                kfree(pi->inline_data);
                pi->inline_data = NULL;
                pi->inline_len = 0;
                pi->inline_dirty = false;
                pr_debug("powerfs: SETATTR truncate INLINE ino=%lu size=0, cleared inline_data\n",
                        inode->i_ino);
            } else if (pi->inline_data && attr->ia_size < pi->inline_len) {
                pi->inline_len = attr->ia_size;
                pi->inline_dirty = true;
                pr_debug("powerfs: SETATTR truncate INLINE ino=%lu size=%llu, inline_len=%u\n",
                        inode->i_ino, attr->ia_size, pi->inline_len);
            }
            spin_unlock(&pi->i_lock);
        }

        /* K2-10: When a FLAT file is truncated (i_size decreases), the
         * server needle still has stale data beyond the new i_size.
         * Subsequent reads of pages beyond the old truncate point (but
         * within the new i_size after re-extension) would fetch this
         * stale data from the server.
         *
         * Fix: mark all pages within the new i_size as dirty and trigger
         * synchronous writeback. The writeback will RMW the needle and
         * truncate it to i_size (via K2-10 fix in powerfs_wb_read_cb). */
        if (pi->placement == POWERFS_PLACEMENT_FLAT &&
            pi->volume_id && pi->file_key &&
            attr->ia_size < old_size) {
            struct folio_batch fbatch;
            pgoff_t idx = 0;
            pgoff_t end = attr->ia_size >> PAGE_SHIFT;
            int dirty_count = 0;

            folio_batch_init(&fbatch);
            while (filemap_get_folios(inode->i_mapping, &idx,
                                      end, &fbatch) > 0) {
                int i;
                for (i = 0; i < folio_batch_count(&fbatch); i++) {
                    struct folio *f = fbatch.folios[i];
                    if (!folio_test_dirty(f)) {
                        if (folio_trylock(f)) {
                            if (!folio_test_dirty(f))
                                folio_mark_dirty(f);
                            folio_unlock(f);
                            dirty_count++;
                        }
                    }
                    folio_put(f);
                }
                folio_batch_init(&fbatch);
            }
            pr_debug("powerfs: SETATTR truncate FLAT ino=%lu old=%llu new=%llu, re-dirtied %d pages for needle truncation\n",
                    inode->i_ino, (unsigned long long)old_size,
                    (unsigned long long)attr->ia_size, dirty_count);

            /* If no pages were found in the page cache (e.g., all pages
             * were invalidated by a previous truncate), we need to
             * populate at least one page to trigger writeback. Read the
             * last page within the new size — this fetches valid data
             * from the server and creates a page cache entry. Then mark
             * it dirty and trigger writeback to truncate the needle. */
            if (dirty_count == 0 && attr->ia_size > 0) {
                pgoff_t last_pg = (attr->ia_size - 1) >> PAGE_SHIFT;
                struct page *page = read_mapping_page(inode->i_mapping,
                                                       last_pg, NULL);
                if (!IS_ERR(page)) {
                    lock_page(page);
                    set_page_dirty(page);
                    unlock_page(page);
                    put_page(page);
                    dirty_count = 1;
                    pr_debug("powerfs: SETATTR truncate FLAT ino=%lu read page %lu to trigger writeback\n",
                            inode->i_ino, (unsigned long)last_pg);
                }
            }

            /* Trigger synchronous writeback to update server needle */
            filemap_write_and_wait(inode->i_mapping);

            /* K2-13: truncate_setsize() above called truncate_pagecache()
             * which skips pages under writeback. Those pages retain stale
             * data beyond i_size in the pagecache. After filemap_write_and_wait
             * completes (all writeback done), call truncate_pagecache() again
             * to zero the tails of partially-truncated pages. Without this,
             * a subsequent extend + writeback would read stale data from the
             * pagecache and send it to the server. */
            pr_debug("powerfs: SETATTR truncate FLAT ino=%lu second truncate_pagecache (size=%llu)\n",
                    inode->i_ino, (unsigned long long)attr->ia_size);
            truncate_pagecache(inode, attr->ia_size);
            pr_debug("powerfs: SETATTR truncate FLAT ino=%lu second truncate_pagecache done\n",
                    inode->i_ino);
        }

        /* K2-14: When extending a FLAT file via truncate (ftruncate to a
         * larger size), the extended region (old_size..new_size) must be
         * zeros per POSIX. However, the server needle may still have stale
         * data from before a previous truncate-down. Even though the
         * truncate-down writeback tries to zero data beyond i_size, the
         * server might not store those zeros reliably (or refresh_work may
         * invalidate the pagecache, forcing a re-read of stale server data).
         *
         * Fix: explicitly zero the pages in the extended region and trigger
         * synchronous writeback. This ensures the server's needle has zeros
         * in the extended region, so subsequent reads return zeros. */
        if (pi->placement == POWERFS_PLACEMENT_FLAT &&
            pi->volume_id && pi->file_key &&
            attr->ia_size > old_size) {
            pgoff_t start_pg = old_size >> PAGE_SHIFT;
            pgoff_t end_pg = (attr->ia_size - 1) >> PAGE_SHIFT;
            pgoff_t pg;
            int dirty_count = 0;

            for (pg = start_pg; pg <= end_pg; pg++) {
                struct page *page;
                size_t off = 0;
                bool need_read = false;

                if (pg == start_pg) {
                    off = old_size & (PAGE_SIZE - 1);
                    /* If old_size is page-aligned, the entire page
                     * is in the extended region — no need to read.
                     * Otherwise, we need valid data before old_size. */
                    need_read = (off > 0);
                }

                if (need_read) {
                    /* Read existing page to preserve data before old_size.
                     * read_mapping_page handles pagecache lookup and
                     * server fetch. Returns unlocked, uptodate page. */
                    page = read_mapping_page(inode->i_mapping, pg, NULL);
                    if (IS_ERR(page)) {
                        /* Server read failed — create zero page */
                        page = find_or_create_page(
                            inode->i_mapping, pg, GFP_NOFS);
                        if (!page)
                            continue;
                        zero_user_segment(page, 0, PAGE_SIZE);
                        SetPageUptodate(page);
                        off = 0;
                    } else {
                        lock_page(page);
                        zero_user_segment(page, off, PAGE_SIZE);
                    }
                } else {
                    /* Page is entirely in the extended region (or
                     * old_size is page-aligned). Create zero page
                     * without reading from server (avoids fetching
                     * stale data). */
                    page = find_or_create_page(
                        inode->i_mapping, pg, GFP_NOFS);
                    if (!page)
                        continue;
                    zero_user_segment(page, 0, PAGE_SIZE);
                    SetPageUptodate(page);
                }

                set_page_dirty(page);
                unlock_page(page);
                put_page(page);
                dirty_count++;
            }

            pr_debug("powerfs: SETATTR extend FLAT ino=%lu old=%llu new=%llu, zeroed %d pages for sparse region\n",
                    inode->i_ino, (unsigned long long)old_size,
                    (unsigned long long)attr->ia_size, dirty_count);

            /* Trigger synchronous writeback to update server needle.
             * Note: we do NOT re-dirty the pages after writeback. Instead,
             * refresh_work is modified to skip pagecache invalidation for
             * FLAT files (see refresh_work). This prevents the zeroed pages
             * from being removed from the pagecache, which would cause
             * subsequent writeback RMW to retain stale server data at the
             * i_size boundary. */
            filemap_write_and_wait(inode->i_mapping);
        }
    }
    setattr_copy(idmap, inode, attr);
    mark_inode_dirty(inode);

    /* 标记 AUTH_EXCL cap dirty — setattr 修改了 inode 元数据 (size/mode/uid/...).
     * 对齐  xxx_setattr → __xxx_mark_caps_dirty(CEPH_CAP_AUTH_EXCL).
     * revoke 时会 flush 这些属性到 Filer. */
    powerfs_cap_mark_dirty(pi, POWERFS_CAP_AUTH_EXCL);

    /* 更新缓存标志 */
    spin_lock(&pi->i_lock);
    pi->cache_valid = true;
    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
    spin_unlock(&pi->i_lock);

    /*
     * powerfs_net 同步: truncate/fchmod/chown 等必须持久化到 Filer,
     * 否则 remount 后属性丢失. fsync 路径已单独同步 size, 这里覆盖
     * setattr 回调路径 (truncate/ftruncate/chmod/chown/utimes).
     */
    if (powerfs_net_is_connected()) {
        __u32 valid = 0;
        __u32 m = 0, u = 0, g = 0;
        __u64 sz = 0, mt = 0, at = 0;

        if (ia_valid & ATTR_MODE) {
            valid |= POWERFS_ATTR_MODE;
            m = inode->i_mode;
        }
        if (ia_valid & ATTR_UID) {
            valid |= POWERFS_ATTR_UID;
            u = from_kuid(&init_user_ns, inode->i_uid);
        }
        if (ia_valid & ATTR_GID) {
            valid |= POWERFS_ATTR_GID;
            g = from_kgid(&init_user_ns, inode->i_gid);
        }
        if (ia_valid & ATTR_SIZE) {
            valid |= POWERFS_ATTR_SIZE;
            sz = i_size_read(inode);
        }
        /* ATTR_MTIME/ATTR_ATIME: persist timestamps so utimes(2)/touch
         * survive remount. Read inode mtime/atime (already updated by
         * setattr_copy above) via 6.17 accessors, convert to unix seconds.
         * ATTR_CTIME is not forwarded (Filer sets ctime implicitly on
         * size change). */
        if (ia_valid & ATTR_MTIME) {
            valid |= POWERFS_ATTR_MTIME;
            mt = inode_get_mtime(inode).tv_sec;
        }
        if (ia_valid & ATTR_ATIME) {
            valid |= POWERFS_ATTR_ATIME;
            at = inode_get_atime(inode).tv_sec;
        }

        if (valid) {
            int sret = powerfs_net_setattr(inode->i_ino, valid,
                                            m, u, g, sz, mt, at);
            pr_debug("powerfs: SETATTR net ino=%lu valid=0x%x sz=%llu mt=%llu at=%llu sret=%d\n",
                    inode->i_ino, valid, sz, mt, at, sret);
            if (sret < 0)
                pr_warn("powerfs: setattr net sync ino=%lu failed: %d\n",
                        inode->i_ino, sret);
        }
    }

    pr_debug("powerfs: setattr '%pd' success (async)\n", dentry);
    return 0;
}

/* ========== rename ========== */

/*
 * powerfs_rename - 重命名/移动文件
 *
 * 参考 xxx_rename (fs/xxx/dir.c)
 *
 * 策略 (与 FUSE 侧对齐, POSIX-safe 同步模型):
 *   - 先通过 powerfs_net_rename 同步等 Filer Raft 提交 (POSIX 要求 rename
 *     成功返回后其他客户端立即可见, 所以必须等持久化).
 *   - Filer rename 成功后:
 *       1. 无条件从本地 dir_entries 移除 old_name 和 new_name
 *          (即使 new_dentry 不是 really positive, Filer 已 commit rename-over-replace,
 *           旧 target entry 必须被清掉; 否则后续 lookup/readdir 可能返回
 *           stale 8 字节 inode, 直到 dir_lease 过期).
 *       2. 对 target inode 做 pagecache invalidate + d_drop(new_dentry).
 *       3. 对 source inode 做 pagecache invalidate + d_drop(old_dentry).
 *          (VFS rename 成功返回后会调用 d_move, 但我们本地 dir_entries
 *           自己管理, 所以还要先 drop 掉).
 *
 * 处理情况:
 *   - 同一目录内重命名
 *   - 跨目录移动
 *   - 目标已存在 (覆盖)
 */
int powerfs_rename(struct mnt_idmap *idmap,
                   struct inode *old_dir, struct dentry *old_dentry,
                   struct inode *new_dir, struct dentry *new_dentry,
                   unsigned int flags)
{
    struct powerfs_inode_info *old_dpi, *new_dpi;
    struct inode *inode = d_inode(old_dentry);

    (void)idmap;

    pr_debug("powerfs: rename '%pd' -> '%pd' (flags=%u)\n",
             old_dentry, new_dentry, flags);

    /* 不支持的标志 */
    if (flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -EINVAL;

    if (!inode) {
        pr_warn("powerfs: rename '%pd' no inode\n", old_dentry);
        return -ENOENT;
    }

    old_dpi = POWERFS_I(old_dir);
    new_dpi = POWERFS_I(new_dir);

    /*
     * 保存旧/新名称 (VFS 会在 rename 返回后调用 d_move 改变 dentry 名称)
     */
    {
        char old_name_buf[POWERFS_MAX_NAME_LEN + 1];
        char new_name_buf[POWERFS_MAX_NAME_LEN + 1];
        int nret;
        struct inode *target_inode = NULL;

        strncpy(old_name_buf, old_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        old_name_buf[POWERFS_MAX_NAME_LEN] = '\0';
        strncpy(new_name_buf, new_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        new_name_buf[POWERFS_MAX_NAME_LEN] = '\0';

        /* 保存替换目标 inode (用于后续 pagecache invalidate),
         * 需要在 d_move/d_drop 之前从 new_dentry 读出. */
        if (d_really_is_positive(new_dentry)) {
            target_inode = d_inode(new_dentry);

            /* 不支持 RENAME_EXCHANGE */
            if (flags & RENAME_EXCHANGE)
                return -EINVAL;

            if (!target_inode)
                return -ENOENT;

            /* 检查是否可以删除目标 */
            if (S_ISDIR(target_inode->i_mode)) {
                /* 目录必须为空 */
                if (!simple_empty(new_dentry))
                    return -ENOTEMPTY;
            }
        } else if (flags & RENAME_NOREPLACE) {
            return -EEXIST;
        }

        /*
         * 先向 filer 发 RENAME 持久化, 成功后才改本地数据结构.
         * 断连时 powerfs_net_rename 返回 -ENOTCONN, 操作失败, 本地不变.
         */
        nret = powerfs_net_rename(old_dir->i_ino,
                                   old_name_buf, strlen(old_name_buf),
                                   new_dir->i_ino,
                                   new_name_buf, strlen(new_name_buf));
        if (nret < 0) {
            pr_warn("powerfs: rename net_sync old=%s new=%s failed: %d\n",
                    old_name_buf, new_name_buf, nret);
            return nret;
        }

        /* === 以下操作仅在 filer rename 成功后执行 === */

        /* ── Phase A: 本地 dir_entries 清理 (对齐 fuse 侧 cache.rename 修复) ──
         *
         * 关键: 无条件从本地 dir_entries 移除 new_name.
         * d_really_is_positive(new_dentry) 仅反映 VFS dcache, 不反映
         * 我们自己的 dir_entries. Filer 已经 commit rename-over-replace,
         * 旧 target entry 必须被清掉, 否则:
         *   - readdir 通过 dir_entries 返回旧 8 字节条目 (例如 ar 的
         *     !<arch>\n magic-only libtest.a).
         *   - lookup 先查 dir_entries -> 返回旧 inode.
         *
         * 必须在 add_dir_entry 之前调用 remove_dir_entry, 否则
         * add_dir_entry 中的同名复用会保留旧 target inode. */
        powerfs_remove_dir_entry(new_dir, new_name_buf);
        powerfs_remove_dir_entry(old_dir, old_name_buf);

        /* ── Phase B: VFS dcache + pagecache 失效 (对齐 FUSE 侧的
         * FUSE_NOTIFY_INVAL_ENTRY + FUSE_NOTIFY_INVAL_INODE) ──
         *
         * Target inode (被替换的旧文件):
         *   - drop_nlink 已做在下方, 但还需清理 pagecache:
         *     内核可能还缓存着旧文件内容 (8 字节 magic),
         *     不清掉的话, 后续 read 可能通过 old alias 访问到 stale 数据.
         *   - d_drop(new_dentry) 强制下次 lookup 重新进入 powerfs_lookup.
         *
         * Source inode (被移动的文件):
         *   - 其在旧路径下的任何 dcache alias 都已无效.
         *   - d_drop(old_dentry) 后 VFS 会 d_move 它 (重命名回正常 dentry
         *     位置), 但 d_drop 保证旧 alias 不再被 DCACHE 哈希链命中. */
        if (target_inode) {
            pr_debug("powerfs: rename-over-replace: inval pages target ino=%lu\n",
                     target_inode->i_ino);
            /* 非阻塞丢弃 clean page, 跳过 dirty/locked page.
             * (rename 的目标通常是已 close 过的文件, page 应为 clean.) */
            invalidate_mapping_pages(target_inode->i_mapping, 0, (pgoff_t)-1);
            /* 从 DCACHE 哈希链摘掉 new_dentry (如果 still hashed),
             * 避免后续 path walk 命中 stale alias. */
            d_drop(new_dentry);
        }
        /* Source inode pagecache invalidate: 它被移到了新路径,
         * 旧路径下的任何 read/write/mmap alias 都应失效.
         * 内核 rename_succeeded 会处理 inode alias list, 但我们主动做
         * invalidate_mapping_pages 以防 VFS 跳过某些情况下 (例如 noopen). */
        invalidate_mapping_pages(inode->i_mapping, 0, (pgoff_t)-1);
        d_drop(old_dentry);

        /* ── Phase C: nlink / 时间戳 / dir_entries 更新 ── */

        /*
         * 目标已存在: 减少目标 inode 的 nlink
         * 参考 ramfs_rename (fs/ramfs/inode.c)
         */
        if (target_inode) {
            if (S_ISDIR(target_inode->i_mode))
                drop_nlink(new_dir);
            drop_nlink(target_inode);
        }

        /*
         * 目录跨移动: drop_nlink(old_dir) + inc_nlink(new_dir)
         * 参考 simple_rename_exchange
         */
        if (S_ISDIR(inode->i_mode) && old_dir != new_dir) {
            drop_nlink(old_dir);
            inc_nlink(new_dir);
        }

        /* 更新时间戳 (6.17: 使用 inode_set_mtime_to_ts/inode_set_ctime_to_ts) */
        {
            struct timespec64 now = current_time(old_dir);
            inode_set_mtime_to_ts(old_dir, now);
            inode_set_ctime_to_ts(old_dir, now);
            if (old_dir != new_dir) {
                now = current_time(new_dir);
                inode_set_mtime_to_ts(new_dir, now);
                inode_set_ctime_to_ts(new_dir, now);
            }
        }

        mark_inode_dirty(old_dir);
        if (old_dir != new_dir)
            mark_inode_dirty(new_dir);

        /* 在新目录添加新名称 (或在同一目录更新) */
        {
            unsigned int entry_type = inode->i_mode & S_IFMT;
            powerfs_add_dir_entry(new_dir, inode->i_ino,
                                  entry_type, new_name_buf);
        }

        /* Phase 1: 本地 mutation 清两个父目录的 lease + epoch++.
         * rename 涉及 old_dir 和 new_dir (可能相同), 都要清,
         * 下次 readdir 重新拉取, 看到重命名后的结果. */
        powerfs_invalidate_dir_lease(old_dir);
        if (old_dir != new_dir)
            powerfs_invalidate_dir_lease(new_dir);
    }

    pr_debug("powerfs: rename '%pd' -> '%pd' success\n",
             old_dentry, new_dentry);

    return 0;
}

/* ========== readdir: 目录读取操作 ========== */

/*
 * powerfs_dir_open - 打开目录文件
 *
 * 参考 xxx_dir_open (fs/xxx/dir.c)
 * 分配目录文件私有数据，初始化readdir状态
 */
int powerfs_dir_open(struct inode *inode, struct file *file)
{
    struct powerfs_dir_file_info *dfi;
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: dir_open ino=%lu\n", inode->i_ino);

    dfi = kzalloc(sizeof(*dfi), GFP_KERNEL);
    if (!dfi)
        return -ENOMEM;

    dfi->file = file;
    dfi->dir = pi;
    dfi->last_ino = 0;
    dfi->last_name = NULL;
    dfi->next_offset = 0;
    dfi->cached_entries = NULL;
    dfi->cached_count = 0;
    dfi->cached_index = 0;
    mutex_init(&dfi->lock);

    file->private_data = dfi;

    /* Cap 引用: 目录打开 → PIN + FILE_SHARED (对齐  xxx_init_file dir 路径) */
    spin_lock(&pi->i_lock);
    pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]++;
    pi->i_last_rd = jiffies;
    spin_unlock(&pi->i_lock);
    powerfs_cap_get_refs(pi, POWERFS_CAP_PIN | POWERFS_CAP_FILE_SHARED);

    pr_debug("powerfs: dir_open success, fop=%p\n", file->f_op);

    return 0;
}

/*
 * powerfs_dir_release - 关闭目录文件
 *
 * 释放目录文件私有数据和缓存
 */
int powerfs_dir_release(struct inode *inode, struct file *file)
{
    struct powerfs_dir_file_info *dfi = file->private_data;
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: dir_release ino=%lu\n", inode->i_ino);

    /* Cap 引用释放 (与 dir_open 对称) */
    spin_lock(&pi->i_lock);
    if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] > 0)
        pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]--;
    spin_unlock(&pi->i_lock);
    powerfs_cap_put_refs(pi, POWERFS_CAP_PIN | POWERFS_CAP_FILE_SHARED);

    if (!dfi)
        return 0;

    if (dfi->last_name)
        kfree(dfi->last_name);
    if (dfi->cached_entries)
        kfree(dfi->cached_entries);
    kfree(dfi);

    file->private_data = NULL;
    return 0;
}

/*
 * powerfs_fill_readdir_cache - 填充目录项缓存
 *
 * 从通信层批量获取目录项，缓存到内存中供readdir使用
 * 返回 0 表示成功，负值表示错误
 *
 * 获取成功后设置 dir_complete = true，表示目录内容已完整缓存
 */
static int powerfs_fill_readdir_cache(struct powerfs_dir_file_info *dfi,
                                       struct inode *dir)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_readdir_req req_data;
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    void *resp_data = NULL;
    int ret;
    u32 max_entries = 64;
    u32 resp_size;

    pr_debug("powerfs: fill_readdir_cache dir_ino=%lu offset=%u\n",
             dir->i_ino, dfi->next_offset);

    /* 如果通信层不可用，返回 ENOTCONN，调用方会用内存模式 */
    if (!powerfs_net_is_connected())
        return -ENOTCONN;

    resp_size = max_entries * sizeof(struct powerfs_dirent);
    resp_data = kmalloc(resp_size, GFP_KERNEL);
    if (!resp_data)
        return -ENOMEM;

    /* 构建请求 */
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_READDIR;
    req_hdr.ino = dir->i_ino;
    req_hdr.data_len = sizeof(req_data);

    memset(&req_data, 0, sizeof(req_data));
    req_data.dir_ino = dir->i_ino;
    req_data.offset = dfi->next_offset;
    req_data.max_entries = max_entries;

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    /* 发送请求 */
    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, resp_data, 500);
    if (ret < 0) {
        kfree(resp_data);
        return ret;
    }

    if (resp_hdr.status != 0) {
        kfree(resp_data);
        return resp_hdr.status;
    }

    /* 解析响应 */
    if (dfi->cached_entries)
        kfree(dfi->cached_entries);
    dfi->cached_entries = NULL;
    dfi->cached_count = 0;
    dfi->cached_index = 0;

    if (resp_hdr.data_len == 0) {
        kfree(resp_data);
        /* 没有更多条目，标记目录完整 (I_COMPLETE)
         * 对齐 : __xxx_dir_set_complete(ci) + i_flags |= I_COMPLETE. */
        spin_lock(&dpi->i_lock);
        dpi->dir_complete = true;
        dpi->i_flags |= POWERFS_I_COMPLETE;
        spin_unlock(&dpi->i_lock);
        return 0;
    }

    /* 计算有多少个目录项 */
    dfi->cached_count = resp_hdr.data_len / sizeof(struct powerfs_dirent);
    if (dfi->cached_count == 0) {
        kfree(resp_data);
        return 0;
    }

    /* 分配并拷贝目录项 */
    dfi->cached_entries = kmemdup(resp_data,
                                   dfi->cached_count * sizeof(struct powerfs_dirent),
                                   GFP_KERNEL);
    if (!dfi->cached_entries) {
        dfi->cached_count = 0;
        kfree(resp_data);
        return -ENOMEM;
    }

    kfree(resp_data);

    /* 获取成功，标记目录内容已完整缓存 (I_COMPLETE)
     * 对齐 : xxx_readdir_prepopulate → __xxx_dir_set_complete. */
    spin_lock(&dpi->i_lock);
    dpi->dir_complete = true;
    dpi->i_flags |= POWERFS_I_COMPLETE;
    spin_unlock(&dpi->i_lock);

    pr_debug("powerfs: fill_readdir_cache got %u entries (dir_complete=true)\n",
             dfi->cached_count);
    return 0;
}

/* ========== 目录项管理 (本地 readdir) ========== */

/**
 * powerfs_add_dir_entry - 添加目录项到链表
 *
 * 使用 dir_mutex 保护.
 * 如果同名条目已存在且标记为 deleted, 复用该条目 (un-delete + 更新元数据),
 * 避免同名重复条目堆积.
 */
static int powerfs_add_dir_entry(struct inode *dir, u64 ino,
                                  unsigned int type, const char *name)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry, *existing = NULL;

    if (!S_ISDIR(dir->i_mode))
        return 0;

    mutex_lock(&dpi->dir_mutex);

    /* 检查是否已有同名条目 (可能是 deleted 状态) */
    list_for_each_entry(entry, &dpi->dir_entries, list) {
        if (strcmp(entry->name, name) == 0) {
            existing = entry;
            break;
        }
    }

    if (existing) {
        /* 复用已有条目: un-delete + 更新元数据 */
        if (existing->deleted) {
            pr_debug("powerfs: add_dir_entry UN_DELETE dir_ino=%lu name='%s' "
                    "old_ino=%llu new_ino=%llu (was deleted, now reactivated)\n",
                    dir->i_ino, name, existing->ino, ino);
        }
        existing->deleted = false;
        existing->ino = ino;
        existing->type = type;
        mutex_unlock(&dpi->dir_mutex);
        return 0;
    }

    mutex_unlock(&dpi->dir_mutex);

    /* 没有同名条目, 分配新条目 */
    entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;

    entry->ino = ino;
    entry->type = type;
    entry->deleted = false;
    strncpy(entry->name, name, POWERFS_MAX_NAME_LEN - 1);
    entry->name[POWERFS_MAX_NAME_LEN - 1] = '\0';

    mutex_lock(&dpi->dir_mutex);
    list_add_tail(&entry->list, &dpi->dir_entries);
    mutex_unlock(&dpi->dir_mutex);

    return 0;
}

/**
 * powerfs_remove_dir_entry - 标记目录项为已删除
 *
 * 使用 dir_mutex 保护.
 *
 * 重要: 不物理删除链表节点, 只标记 deleted=true.
 * 原因: powerfs_readdir 使用 ctx->pos 作为链表索引, 物理删除会导致
 *       后续 getdents 调用的索引偏移, 漏掉未发射的条目 (C9f bug 根因).
 *       标记删除保持链表结构稳定, readdir 跳过 deleted 条目即可.
 *       deleted 条目在以下场景被清理:
 *       - powerfs_clear_dir_entries (rmdir/evict)
 *       - powerfs_add_dir_entry 同名复用
 *       - powerfs_readdir Filer 重新拉取时 un-delete (文件被重建)
 *       - powerfs_compact_dir_entries (O-05: readdir ctx->pos==0 时阈值触发)
 */
static int powerfs_remove_dir_entry(struct inode *dir, const char *name)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry;
    int total = 0, deleted_count = 0;

    if (!S_ISDIR(dir->i_mode))
        return 0;

    mutex_lock(&dpi->dir_mutex);
    list_for_each_entry(entry, &dpi->dir_entries, list) {
        total++;
        if (entry->deleted)
            deleted_count++;
        if (strcmp(entry->name, name) == 0 && !entry->deleted) {
            entry->deleted = true;
            deleted_count++;
            pr_debug("powerfs: remove_dir_entry MARK_DELETED dir_ino=%lu "
                    "name='%s' entry_ino=%llu (total=%d deleted=%d active=%d)\n",
                    dir->i_ino, name, entry->ino, total, deleted_count,
                    total - deleted_count);
            mutex_unlock(&dpi->dir_mutex);
            return 0;
        }
    }
    mutex_unlock(&dpi->dir_mutex);

    pr_warn("powerfs: remove_dir_entry NOT_FOUND dir_ino=%lu name='%s' "
            "(total=%d deleted=%d active=%d)\n",
            dir->i_ino, name, total, deleted_count, total - deleted_count);

    return -ENOENT;
}

/**
 * powerfs_clear_dir_entries - 清空目录项链表
 *
 * 使用 dir_mutex 保护
 */
static void powerfs_clear_dir_entries(struct inode *dir)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry, *tmp;

    if (!S_ISDIR(dir->i_mode))
        return;

    mutex_lock(&dpi->dir_mutex);
    list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
        list_del_init(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&dpi->dir_mutex);
}

/**
 * powerfs_compact_dir_entries - 物理清理已标记 deleted 的目录条目
 *
 * O-05: 防止长生命周期目录 (如 /tmp) 的 deleted 条目无限堆积导致内存泄漏.
 *
 * 清理条件: deleted 条目数 >= 64 且占总数 50% 以上
 * 调用时机: powerfs_readdir 入口且 ctx->pos == 0 (新读或 rewind)
 *           此时无 stale ctx->pos 需要保持, 物理删除安全
 * 锁: 调用方须持 dir_mutex
 *
 * 返回: 清理的条目数
 */
static int powerfs_compact_dir_entries(struct inode *dir)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry, *tmp;
    int total = 0, deleted = 0, removed = 0;

    /* 统计总数和已删除数 */
    list_for_each_entry(entry, &dpi->dir_entries, list) {
        total++;
        if (entry->deleted)
            deleted++;
    }

    /* 未达阈值, 不清理 */
    if (deleted < 64 || deleted * 2 < total)
        return 0;

    /* 物理删除所有 deleted 条目 */
    list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
        if (entry->deleted) {
            list_del_init(&entry->list);
            kfree(entry);
            removed++;
        }
    }

    pr_info("powerfs: compact_dir_entries dir_ino=%lu compacted %d/%d entries "
            "(remaining=%d)\n",
            dir->i_ino, removed, total, total - removed);

    return removed;
}

/* Phase 1: 本地 mutation 后清父目录 lease + bump shared_gen.
 * 调用时机: mkdir/rmdir/create/unlink/symlink/link/rename 网络请求成功后,
 *           在修改本地数据结构的同时清目录 lease.
 * 效果: 下次 readdir 看到 lease 过期会重新拉取, 看到自己刚加/删的项.
 *       i_shared_gen++ 使所有子 dentry 的 dir_shared_gen 失效,
 *       d_revalidate Layer 2 不再命中 → 触发 re-lookup.
 *       清 I_COMPLETE 使负 dentry 不再被信任 (目录内容已变).
 * 注意: 调用方已持 dir->i_rwsem 写锁 (VFS 保证), 此处无需额外锁. */
static void powerfs_invalidate_dir_lease(struct inode *dir)
{
    struct powerfs_inode_info *dpi;

    if (!dir || !S_ISDIR(dir->i_mode))
        return;
    dpi = POWERFS_I(dir);
    /* dir_mutex 保护 dir_entries 链表; dir_lease_expire/epoch 是 atomic-like
     * 字段, 用 WRITE_ONCE 配合 readdir 的 READ_ONCE. */
    mutex_lock(&dpi->dir_mutex);
    WRITE_ONCE(dpi->dir_lease_expire, 0);
    dpi->dir_lease_epoch++;
    /* Bump i_shared_gen: 使所有子 dentry 的 dir_shared_gen 不再匹配.
     * 对齐  atomic_inc(&ci->i_shared_gen) + Rust cache.rs bump_dir_version.
     * 对齐 Rust: "Bump dir_version so stale dentries with dir_shared_gen
     * mismatch are detected." */
    atomic_inc(&dpi->i_shared_gen);
    /* 清 I_COMPLETE: 目录内容已变, 不再信任负 dentry.
     * 对齐 : __xxx_dir_clear_complete(ci). */
    dpi->i_flags &= ~POWERFS_I_COMPLETE;
    dpi->dir_complete = false;
    mutex_unlock(&dpi->dir_mutex);
}

/*
 * powerfs_readdir - 读取目录内容 (使用本地链表)
 *
 * 关键设计:
 *   1. 在 dir_mutex 保护下复制目录项到临时数组
 *   2. 释放 dir_mutex 后再调用 dir_emit
 *
 * 为什么不能在 dir_mutex 内调用 dir_emit:
 *   - dir_emit 可能触发 d_revalidate (路径遍历)
 *   - create/unlink 也需要 dir_mutex
 *   - 如果在 dir_mutex 内调用 dir_emit，而 dir_emit 内部
 *     触发 d_revalidate 需要 d_lock，同时 create 持有
 *     dir_mutex 尝试获取 d_lock → ABBA 死锁
 *
 * 解决方案:
 *   - 在锁内快速复制所有条目 (O(n) 一次性)
 *   - 释放锁后逐条 emit，允许并发修改
 */
int powerfs_readdir(struct file *file, struct dir_context *ctx)
{
    struct inode *dir = file_inode(file);
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry, *tmp;
    loff_t pos = 0;

    pr_debug("powerfs: readdir ENTER dir_ino=%lu ctx_pos=%lld "
             "dir_complete=%d lease_expire=%ld lease_epoch=%u\n",
             dir->i_ino, (s64)ctx->pos,
             READ_ONCE(dpi->dir_complete),
             READ_ONCE(dpi->dir_lease_expire),
             dpi->dir_lease_epoch);

    /* O-05: 清理 deleted 条目, 防止内存泄漏.
     * 仅在 ctx->pos == 0 (新读/rewind) 时清理, 此时无 stale 位置需保持. */
    if (ctx->pos == 0) {
        mutex_lock(&dpi->dir_mutex);
        powerfs_compact_dir_entries(dir);
        mutex_unlock(&dpi->dir_mutex);
    }

    /* 处理 "." 和 ".." */
    if (ctx->pos == 0) {
        if (!dir_emit_dots(file, ctx))
            return 0;
    }

    pos = ctx->pos - 2;

    /* === Phase 1: 目录 lease fast-path ===
     * dir_complete && dir_lease_expire 未过期: 直接用本地缓存, 不发网络.
     * 30s 内重复 readdir 走纯内存 (POWERFS_DIR_LEASE_TTL).
     * lease 过期或 dir_complete=false: 清缓存重新拉取 (本地 mutation 后). */
    if (READ_ONCE(dpi->dir_complete) &&
        time_before(jiffies, READ_ONCE(dpi->dir_lease_expire))) {
        pr_debug("powerfs: readdir ino=%lu lease fast-path (cached)\n",
                 dir->i_ino);
        goto emit_cached;
    }

    /* 如果目录 lease 过期或缓存为空, 且网络可用, 从 Filer 获取目录列表.
     * lease 过期但缓存非空: 先清缓存 (避免新旧条目混合). */
    if (powerfs_net_is_connected()) {
        struct powerfs_net_dir_entry *net_entries;
        __u32 net_count = 0;
        bool has_more = false;
        char last_name[256] = "";
        int ret;
        int timeout_ms;
        bool pulled_any = false;  /* 是否已拉取到至少一页 (超时容错) */

        /* lease 过期: 标记需要重新拉取, 但不清空 dir_entries.
         * 原因: 本地 mutation (create/mkdir) 通过 powerfs_add_dir_entry 添加的条目
         * 可能尚未出现在 Filer 的 readdir 响应中, 清空会导致这些条目丢失.
         * 网络拉取时通过 ino 去重, 旧条目保留, 新条目追加. */
        if (READ_ONCE(dpi->dir_complete) &&
            time_after(jiffies, READ_ONCE(dpi->dir_lease_expire))) {
            int total = 0, del = 0;
            struct powerfs_dir_entry *e;
            list_for_each_entry(e, &dpi->dir_entries, list) {
                total++;
                if (e->deleted) del++;
            }
            pr_debug("powerfs: readdir REFETCH dir_ino=%lu ctx_pos=%lld "
                    "lease_expired (entries: total=%d deleted=%d active=%d, "
                    "epoch=%u)\n",
                    dir->i_ino, (s64)ctx->pos, total, del,
                    total - del, dpi->dir_lease_epoch);
            WRITE_ONCE(dpi->dir_complete, false);
            WRITE_ONCE(dpi->dir_lease_expire, 0);
        }

        net_entries = kmalloc_array(256, sizeof(*net_entries), GFP_KERNEL);
        if (!net_entries)
            return -ENOMEM;

        /* Phase 1: 短超时策略 (同 lookup). */
        timeout_ms = powerfs_net_pick_timeout(POWERFS_READDIR_TIMEOUT_MS);

        /* 从 Filer 获取目录条目 (分页循环直到获取全部) */
        do {
            ret = powerfs_net_readdir_timeout(dir->i_ino, last_name, 256,
                                              net_entries, 256,
                                              &net_count, &has_more,
                                              timeout_ms);
            if (ret < 0) {
                /* Phase 1: 瞬态错误处理.
                 * - 已拉取部分页: 用已拉取的, 不返回错误 (best-effort).
                 * - 一页都没拉到:
                 *   * 缓存有内容 (lease 过期但未清): 用旧缓存
                 *   * 缓存为空: 返回 -EAGAIN 让 VFS/应用层重试 */
                if (pulled_any) {
                    pr_warn("powerfs: readdir ino=%lu partial fetch err=%d, "
                            "using entries pulled so far\n",
                            dir->i_ino, ret);
                    break;
                }
                if (!list_empty(&dpi->dir_entries)) {
                    pr_warn("powerfs: readdir ino=%lu net err=%d, "
                            "using stale cache\n", dir->i_ino, ret);
                    break;
                }
                if (ret == -ETIMEDOUT || ret == -ENOTCONN ||
                    ret == -ESHUTDOWN) {
                    pr_warn("powerfs: readdir ino=%lu transient err=%d "
                            "(timeout_ms=%d), return -EAGAIN\n",
                            dir->i_ino, ret, timeout_ms);
                    kfree(net_entries);
                    return -EAGAIN;
                }
                pr_warn("powerfs: readdir ino=%lu net error: %d\n",
                        dir->i_ino, ret);
                kfree(net_entries);
                return ret;
            }
            pulled_any = true;

            /* 将网络条目添加到本地缓存 */
            mutex_lock(&dpi->dir_mutex);
            for (__u32 i = 0; i < net_count; i++) {
                struct powerfs_dir_entry *de;
                struct powerfs_net_dir_entry *ne = &net_entries[i];

                /* 更新 last_name 用于分页 — 必须在每个条目上更新,
                 * 不能只在非重复条目上更新. 否则当所有条目都是缓存中
                 * 已有的重复条目时 (如本地 create 已添加), last_name
                 * 不变, 导致分页循环重复请求同一页 (infinite loop). */
                strncpy(last_name, ne->name, sizeof(last_name) - 1);
                last_name[sizeof(last_name) - 1] = '\0';

                /* 检查是否已存在 (按名称去重, 不能用 ino — hardlink
                 * 的多个目录项共享同一 inode 但名称不同) */
                bool found = false;
                list_for_each_entry(de, &dpi->dir_entries, list) {
                    if (strcmp(de->name, ne->name) == 0) {
                        found = true;
                        /* 如果本地标记为 deleted 但 Filer 仍返回该条目,
                         * 说明文件被重建 (同名新 inode), un-delete 并更新元数据. */
                        if (de->deleted) {
                            pr_debug("powerfs: readdir REFETCH_UN_DELETE "
                                    "dir_ino=%lu name='%s' old_ino=%llu "
                                    "new_ino=%llu (Filer still has it, "
                                    "file was re-created)\n",
                                    dir->i_ino, ne->name, de->ino,
                                    ne->ino);
                            de->deleted = false;
                            de->ino = ne->ino;
                            de->type = ne->mode & S_IFMT;
                        }
                        break;
                    }
                }
                if (found)
                    continue;

                de = kmalloc(sizeof(*de), GFP_KERNEL);
                if (!de) {
                    mutex_unlock(&dpi->dir_mutex);
                    kfree(net_entries);
                    return -ENOMEM;
                }

                de->ino = ne->ino;
                de->type = ne->mode & S_IFMT;
                de->deleted = false;
                strncpy(de->name, ne->name, POWERFS_MAX_NAME_LEN - 1);
                de->name[POWERFS_MAX_NAME_LEN - 1] = '\0';
                list_add_tail(&de->list, &dpi->dir_entries);
            }
            mutex_unlock(&dpi->dir_mutex);
        } while (has_more && net_count > 0);

        kfree(net_entries);

        /* 拉取成功 (或部分成功): 设置 dir_complete + I_COMPLETE + dir_lease_expire.
         * 部分成功时也设 dir_complete (避免反复部分拉取), 下次 lease 过期再补.
         * 对齐 : __xxx_dir_set_complete + I_COMPLETE. */
        WRITE_ONCE(dpi->dir_complete, true);
        dpi->i_flags |= POWERFS_I_COMPLETE;
        WRITE_ONCE(dpi->dir_lease_expire, jiffies + POWERFS_DIR_LEASE_TTL);
    }

emit_cached:

    /*
     * 第一阶段: 在锁内复制目录项到临时缓冲区
     */
    {
        struct dentry_emit_entry {
            u64 ino;
            unsigned int type;
            unsigned short namelen;
            char name[POWERFS_MAX_NAME_LEN];
        } *buf;
        int count = 0;
        int max = 256;
        int i;

        buf = kmalloc_array(max, sizeof(*buf), GFP_KERNEL);
        if (!buf) {
            /* 分配失败，直接在锁内 emit (回退方案) */
            int fb_skipped_deleted = 0, fb_emitted = 0;
            mutex_lock(&dpi->dir_mutex);
            list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
                unsigned char d_type;
                if (pos > 0) { pos--; continue; }
                /* 跳过已标记删除的条目, 但仍消耗位置槽以保持 ctx->pos 索引稳定 */
                if (entry->deleted) { ctx->pos++; fb_skipped_deleted++; continue; }
                switch (entry->type) {
                case S_IFREG:  d_type = DT_REG; break;
                case S_IFDIR:  d_type = DT_DIR; break;
                case S_IFLNK:  d_type = DT_LNK; break;
                default:       d_type = DT_UNKNOWN; break;
                }
                if (!dir_emit(ctx, entry->name, strlen(entry->name),
                              entry->ino, d_type)) {
                    pr_debug("powerfs: readdir EMIT_FALLBACK dir_ino=%lu "
                             "ctx_pos=%lld emitted=%d skipped_deleted=%d "
                             "(buf full, returning early)\n",
                             dir->i_ino, (s64)ctx->pos, fb_emitted,
                             fb_skipped_deleted);
                    mutex_unlock(&dpi->dir_mutex);
                    return 0;
                }
                ctx->pos++;
                fb_emitted++;
            }
            pr_debug("powerfs: readdir EMIT_FALLBACK_DONE dir_ino=%lu "
                     "ctx_pos=%lld emitted=%d skipped_deleted=%d (EOF)\n",
                     dir->i_ino, (s64)ctx->pos, fb_emitted,
                     fb_skipped_deleted);
            mutex_unlock(&dpi->dir_mutex);
            return 0;
        }

        {
            int skipped_deleted = 0;
            loff_t pos_start = ctx->pos;

            mutex_lock(&dpi->dir_mutex);

            list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
                if (count >= max)
                    break;
                if (pos > 0) {
                    pos--;
                    continue;
                }
                /* 跳过已标记删除的条目, 但仍消耗位置槽以保持 ctx->pos 索引稳定.
                 * 这确保 rm -rf 在 unlink 子条目后, 后续 getdents 不会因
                 * 链表节点移除而导致 ctx->pos 偏移、漏掉未发射的条目 (C9f bug). */
                if (entry->deleted) {
                    ctx->pos++;
                    skipped_deleted++;
                    continue;
                }
                buf[count].ino = entry->ino;
                buf[count].type = entry->type;
                buf[count].namelen = strlen(entry->name);
                memcpy(buf[count].name, entry->name, buf[count].namelen + 1);
                count++;
            }

            mutex_unlock(&dpi->dir_mutex);

            if (skipped_deleted > 0)
                pr_debug("powerfs: readdir EMIT_SKIP dir_ino=%lu "
                        "pos_start=%lld pos_end=%lld copied=%d "
                        "skipped_deleted=%d (buf_max=%d)\n",
                        dir->i_ino, (s64)pos_start, (s64)ctx->pos,
                        count, skipped_deleted, max);
        }

        /* 第二阶段: 释放锁后逐条 emit */
        for (i = 0; i < count; i++) {
            unsigned char d_type;

            switch (buf[i].type) {
            case S_IFREG:  d_type = DT_REG; break;
            case S_IFDIR:  d_type = DT_DIR; break;
            case S_IFLNK:  d_type = DT_LNK; break;
            default:       d_type = DT_UNKNOWN; break;
            }

            if (!dir_emit(ctx, buf[i].name, buf[i].namelen,
                          buf[i].ino, d_type)) {
                pr_debug("powerfs: readdir EMIT_PARTIAL dir_ino=%lu "
                         "ctx_pos=%lld emitted=%d/%d (userspace buf full)\n",
                         dir->i_ino, (s64)ctx->pos, i, count);
                kfree(buf);
                return 0;
            }

            ctx->pos++;
        }

        if (count > 0)
            pr_debug("powerfs: readdir EMIT_DONE dir_ino=%lu "
                     "ctx_pos=%lld emitted=%d (EOF or buf exhausted)\n",
                     dir->i_ino, (s64)ctx->pos, count);

        kfree(buf);
    }

    return 0;
}

/* 目录文件操作表 - 使用本地链表实现 readdir
 *
 * P0-4 fix: 补齐 fsync/lock/flock/ioctl, 与文件 fops 对应函数共用实现.
 *   - lock/flock: 目录级文件锁 (NFS 导出、Maildir、git index.lock 等会用)
 *   - fsync: 事务场景 fsync(dir_fd) 确保 create/rename 持久化
 *   - ioctl: 预留扩展 (ACL ioctl 等, 但 ACL 通 xattr 路径完成) */
static const struct file_operations powerfs_dir_operations = {
    .open           = powerfs_dir_open,
    .release        = powerfs_dir_release,
    .iterate_shared = powerfs_readdir,
    .llseek         = generic_file_llseek,
    .read           = generic_read_dir,   /* read() on dir returns -EISDIR (POSIX) */
    .fsync          = powerfs_dir_fsync,  /* P0-4 fix: fsync(dir_fd) 同步元数据+dirty xattr */
    .lock           = powerfs_lock,       /* P0-4 fix: 目录 POSIX 记录锁 */
    .flock          = powerfs_flock,      /* P0-4 fix: 目录 BSD flock */
    .unlocked_ioctl = powerfs_ioctl,      /* P1-3 扩展实现 */
    .compat_ioctl   = compat_ptr_ioctl,   /* P1-3 32-bit compat */
    .setlease       = simple_nosetlease,  /* P2-4: 明确拒绝 F_SETLEASE delegations，
                                           * 防止 silent data stale (远端写不 break local lease) */
};

/* ========== 地址空间操作 (page cache) ========== */

/* powerfs_read_folio 已移除 (Stage C): read 路径由 netfs_read_folio +
 * powerfs_netfs_issue_read 接管, 参照 xxx_aops.read_folio = netfs_read_folio. */

/* ========== Stage C: writepages 批量异步写入 ==========
 *
 * 参照 xxx_writepages_start 模式, 批量收集脏页减少 work item 数量:
 *   1. writepages 遍历脏页, 收集到 powerfs_writepage_work 批量结构
 *   2. batch 满 (sbi->write_batch_pages 页) 或遍历完成时提交 work item
 *   3. workqueue 线程串行发送 batch 内所有页面
 *   4. max_active=4 限制全局并发 worker 数
 *
 * 批量大小可配 (mount option write_batch_kb):
 *   - 默认 64KB (16 pages): TCP 网络
 *   - ROCE 推荐 1MB (256 pages): 高吞吐
 *   - 最大 stripe size 64MB (16384 pages): 单 stripe 一次提交
 *
 * 性能: 1MB 文件, batch=1MB → 1 个 work item (vs 旧方案 256 个)
 *
 * work 结构体使用 kvmalloc 单块分配 (header + 3 个动态数组), 因为
 * max 可达 16384 页 (数组 ~384KB, 超过 kmalloc 上限). */

struct powerfs_writepage_work {
    struct work_struct work;
    struct inode *inode;
    int num_pages;
    int max_pages;
    struct page **pages;     /* max_pages 项, 紧随结构体后 */
    loff_t *offsets;         /* max_pages 项, 紧随 pages 后 */
    size_t *counts;          /* max_pages 项, 紧随 offsets 后 */
    /* 异步提交计数: 跟踪尚未完成的 needle 写入. work_fn 持有 +1 ref,
     * 每个 needle ctx 完成时 dec. 归零时执行 final cleanup (iput/dec/kvfree).
     * 防止提前归零: work_fn 在所有 ctx 提交后才 dec 自己的 ref. */
    atomic_t pending_needles;
    /* RCU 延迟释放头: work_struct 在 wpw 中, work_fn 返回后 workqueue 仍
     * 访问 work->data. 用 call_rcu 延迟 kvfree 到 RCU 宽限期后, 确保
     * workqueue 对 work_struct 的访问已完成. */
    struct rcu_head rcu;
};

/*
 * powerfs_alloc_write_batch - 分配批量写 work 结构 (含动态数组)
 *
 * 单块 kvmalloc: [header][pages[]][offsets[]][counts[]]
 * 大批量 (如 64MB=16384 页) 时数组 ~384KB, 需 kvmalloc 自动回退 vmalloc.
 */
static struct powerfs_writepage_work *
powerfs_alloc_write_batch(int max_pages, gfp_t gfp)
{
    struct powerfs_writepage_work *batch;
    size_t arr_sz = (size_t)max_pages *
                    (sizeof(struct page *) + sizeof(loff_t) + sizeof(size_t));
    size_t sz = sizeof(*batch) + arr_sz;

    batch = kvmalloc(sz, gfp | __GFP_ZERO);
    if (!batch)
        return NULL;

    batch->max_pages = max_pages;
    batch->num_pages = 0;
    batch->pages = (struct page **)(batch + 1);
    batch->offsets = (loff_t *)(batch->pages + max_pages);
    batch->counts = (size_t *)(batch->offsets + max_pages);
    return batch;
}

/*
 * ========== Stage D: page writeback 异步提交模式 ==========
 *
 * 旧同步实现 (powerfs_net_write_needle/read_needle) 在 workqueue 线程内
 * wait_for_completion 等待响应 (最长 30s/请求), 多个 needle 串行 →
 * workqueue lockup 598s. 新实现将 read_needle + write_needle 改为异步提交:
 *
 *   writepage_work_fn (workqueue 线程, 快速返回):
 *     1. 按 needle_id 分组 batch 内页面
 *     2. 每个 needle 组: alloc ctx (含 2MB needle_buf), 获取 lease (同步, 快速),
 *        submit read_needle_async (非阻塞入队)
 *     3. work_fn 返回, workqueue 线程立即释放
 *
 *   read_cb (调度器 RX 上下文, 响应到达时):
 *     1. 读取结果填入 ctx->needle_buf (scheduler 已完成 memcpy)
 *     2. 合并脏页数据到 needle_buf (read-modify-write 的 modify)
 *     3. submit write_needle_async (非阻塞入队)
 *
 *   write_cb (调度器 RX 上下文, 写响应到达时):
 *     1. end_page_writeback + put_page (清除该 needle 的所有页)
 *     2. free ctx (needle_buf + ctx)
 *     3. atomic_dec pending_needles; 归零则 final_cleanup (iput/dec/kvfree wpw)
 *
 * 引用计数 (pending_needles):
 *   - 初始 = num_needle_groups + 1 (work_fn 持有 +1 ref)
 *   - 每个 ctx 完成 (成功或失败) dec 1
 *   - work_fn 在所有 ctx 提交后 dec 自己的 +1
 *   - 归零时 final_cleanup, 防止提前归零 (work_fn 仍在访问 wpw)
 *
 * 超时兜底: 异步请求用 delayed_work (system_wq) 在 deadline 到期时以
 *   -ETIMEDOUT 完成, 防止 page 永久滞留 PageWriteback → hung task.
 */

/* Per-needle 异步上下文 (一个 needle 组对应一个 ctx) */
struct powerfs_wb_ctx {
    struct powerfs_writepage_work *wpw;   /* 所属 batch */
    int needle_start_idx;                  /* wpw->pages 中的起始索引 */
    int needle_end_idx;                    /* 结束索引 (exclusive) */
    __u64 volume_id;
    __u64 needle_id;
    __u8 *needle_buf;                      /* 2MB, per-ctx (read 写入, write 读出) */
    __u32 needle_len;                      /* needle 有效数据长度 */
    char lease_token[64];
    size_t lease_token_len;
    /* 持久缓冲区: 异步请求的 req_body / resp_body, 存活到 callback 触发 */
    __u8 req_body[256];
    __u8 resp_body[64];
};

/* 前向声明 */
static void powerfs_wb_final_cleanup(struct powerfs_writepage_work *wpw);
static int powerfs_wb_read_cb(struct powerfs_request *req);
static int powerfs_wb_write_cb(struct powerfs_request *req);

/* powerfs_wb_fail_pages - 失败一个 ctx 范围内的所有页面
 *
 * 用于 ctx 分配失败或异步提交失败时, 同步清理页面 (在 work_fn 上下文).
 * 调用方负责 dec pending_needles + free ctx. */
static void powerfs_wb_fail_pages(struct powerfs_wb_ctx *ctx, int err)
{
    struct powerfs_writepage_work *wpw = ctx->wpw;
    int j;

    for (j = ctx->needle_start_idx; j < ctx->needle_end_idx; j++) {
        struct page *p = wpw->pages[j];
        if (wpw->counts[j] == 0)
            continue;
        if (err < 0)
            mapping_set_error(p->mapping, err);
        end_page_writeback(p);
        put_page(p);
    }
}

/* powerfs_wb_free_rcu - RCU 回调: 延迟释放 wpw 内存
 *
 * work_struct 在 wpw 中, work_fn 返回后 workqueue 子系统仍访问 work->data
 * (清除 WORK_BUSY_PENDING 等标志). 直接 kvfree 会导致 use-after-free
 * (assign_work NULL deref oops). 用 call_rcu 延迟到 RCU 宽限期后释放,
 * 确保 workqueue 对 work_struct 的访问已完成. */
static void powerfs_wb_free_rcu(struct rcu_head *rcu)
{
    struct powerfs_writepage_work *wpw =
        container_of(rcu, struct powerfs_writepage_work, rcu);
    kvfree(wpw);
}

/* powerfs_wb_final_cleanup - 所有 needle 完成后的最终清理
 *
 * 释放 batch 级资源: inode 引用, wb_in_flight 计数.
 * wpw 内存通过 call_rcu 延迟释放 (work_struct 生命周期约束).
 * 由最后一个完成的 ctx (或 work_fn 的自身 ref) 触发. */
static void powerfs_wb_final_cleanup(struct powerfs_writepage_work *wpw)
{
    struct inode *inode = wpw->inode;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: WB_FINAL_CLEANUP ino=%lu wb_in_flight=%d\n",
            inode->i_ino, atomic_read(&sbi->wb_in_flight));
    atomic_dec(&sbi->wb_in_flight);

    /* writeback 互斥: 最后一个 batch 完成时释放 wb_mutex,
     * 允许下一次 powerfs_writepages 执行. */
    if (atomic_dec_and_test(&pi->wb_batch_count))
        mutex_unlock(&pi->wb_mutex);

    iput(inode);
    /* 延迟释放 wpw: work_struct 在 wpw 中, workqueue 在 work_fn 返回后
     * 仍访问 work->data. call_rcu 等待 RCU 宽限期后释放. */
    call_rcu(&wpw->rcu, powerfs_wb_free_rcu);
}

/* powerfs_wb_read_cb - read_needle 异步完成回调
 *
 * 由 powerfs_req_complete 在调度器 RX 上下文 (process context) 调用.
 * 1. 处理读取结果 (NOT_FOUND=新 needle, 其他错误=失败页面)
 * 2. 合并脏页数据到 needle_buf (read-modify-write 的 modify)
 * 3. 提交 write_needle_async 进入第二阶段
 *
 * 注意: 此函数在调度器线程上下文执行, 应避免长时间阻塞. 合并操作是
 * N×4KB memcpy (16页=64KB ≈ 32µs), 可接受. write_needle_async 仅入队, 不阻塞. */
static int powerfs_wb_read_cb(struct powerfs_request *req)
{
    struct powerfs_wb_ctx *ctx = req->priv;
    struct powerfs_writepage_work *wpw = ctx->wpw;
    struct inode *inode = wpw->inode;
    int ret;

    pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu err=%d status=%u pages=[%d,%d) needle_len=%u\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            req->error, req->resp_status,
            ctx->needle_start_idx, ctx->needle_end_idx, ctx->needle_len);

    /* 解析读取结果 */
    if (req->error < 0) {
        /* 网络错误 (断连/超时/截断): 失败页面 */
        pr_warn("powerfs: wb read_cb ino=%lu nid=%llu net_err=%d\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                req->error);
        powerfs_wb_fail_pages(ctx, req->error);
        powerfs_request_free(req);
        kvfree(ctx->needle_buf);
        kfree(ctx);
        if (atomic_dec_and_test(&wpw->pending_needles))
            powerfs_wb_final_cleanup(wpw);
        return 0;
    }

    if (req->resp_status == POWERFS_NET_STATUS_ERR_NOT_FOUND) {
        /* needle 不存在 (新文件首次写): 全零 needle, 正常继续 */
        ctx->needle_len = 0;
    } else if (req->resp_status != POWERFS_NET_STATUS_OK) {
        /* 其他服务端错误: 失败页面 */
        int err = net_status_to_errno(req->resp_status);
        pr_warn("powerfs: wb read_cb ino=%lu nid=%llu status=%u err=%d\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                req->resp_status, err);
        powerfs_wb_fail_pages(ctx, err);
        powerfs_request_free(req);
        kvfree(ctx->needle_buf);
        kfree(ctx);
        if (atomic_dec_and_test(&wpw->pending_needles))
            powerfs_wb_final_cleanup(wpw);
        return 0;
    } else {
        /* 读取成功: scheduler 已将 needle 内容写入 ctx->needle_buf */
        ctx->needle_len = (__u32)req->resp_data_len;
    }

    pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu after_resp status=%u resp_data_len=%llu needle_len=%u\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            req->resp_status, (unsigned long long)req->resp_data_len,
            ctx->needle_len);

    powerfs_request_free(req);  /* 释放 read 请求 */

    /* K2-8: The Volume Server may return empty or partial needle data,
     * especially right after inline→Flat migration when the server hasn't
     * committed the data yet. If we proceed with RMW using only the server
     * data, any gaps between dirty pages are zeroed → data corruption.
     *
     * Optimization: fill from page cache ONLY when the server returned
     * empty or partial data (needle_len < max_needle_len). When the server
     * has complete data, dirty pages will be overlaid in the modify step
     * below, and clean pages are already consistent with the server — no
     * need to traverse the pagecache. This avoids 256 find_get_page+kmap
     * per needle in the common writeback path.
     *
     * Pages in the page cache are at least as new as the server data (they
     * were either read from the server or written by this client), so
     * overwriting server data with page cache data is safe.
     *
     * We fill from needle_start to min(i_size, needle_start + CHUNK_SIZE),
     * covering the full needle range (not just the dirty page range). */
    {
        loff_t first_offset = wpw->offsets[ctx->needle_start_idx];
        loff_t needle_start = first_offset - (first_offset % POWERFS_CHUNK_SIZE);
        loff_t isize = i_size_read(inode);
        __u32 max_needle_len = (__u32)min_t(loff_t, isize - needle_start,
                                            POWERFS_CHUNK_SIZE);
        loff_t fill_end = min_t(loff_t, isize,
                                needle_start + POWERFS_CHUNK_SIZE);
        pgoff_t start_pg = needle_start >> PAGE_SHIFT;
        pgoff_t end_pg = (fill_end + PAGE_SIZE - 1) >> PAGE_SHIFT;
        pgoff_t pg;
        int pages_found = 0;

        pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu filling range %llu-%llu (i_size=%llu) from pagecache, server_needle_len=%u\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                (unsigned long long)needle_start,
                (unsigned long long)fill_end,
                (unsigned long long)isize, ctx->needle_len);

        /* Skip pagecache fill when server already has complete data.
         * Dirty pages will be overlaid in the modify step below. */
        if (ctx->needle_len >= max_needle_len) {
            pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu skip pagecache fill (server_len=%u >= max=%u)\n",
                    inode->i_ino, (unsigned long long)ctx->needle_id,
                    ctx->needle_len, max_needle_len);
            goto skip_pagecache_fill;
        }

        for (pg = start_pg; pg < end_pg; pg++) {
            struct page *pgc = find_get_page(inode->i_mapping, pg);
            if (pgc) {
                size_t off_in_needle = (pg << PAGE_SHIFT) - needle_start;
                size_t copy_len = min_t(size_t, PAGE_SIZE,
                                        fill_end - (pg << PAGE_SHIFT));
                char *kaddr = kmap_local_page(pgc);
                memcpy(ctx->needle_buf + off_in_needle, kaddr, copy_len);
                kunmap_local(kaddr);
                put_page(pgc);
                pages_found++;
                if (off_in_needle + copy_len > ctx->needle_len)
                    ctx->needle_len = off_in_needle + copy_len;
            }
        }
        pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu pagecache fill done: found %d pages, needle_len=%u\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                pages_found, ctx->needle_len);
    skip_pagecache_fill:
        ; /* label target */
    }

    /* K2-10: After truncate, the server needle may still have stale data
     * beyond i_size. When the file is extended again, pages beyond the
     * old truncate point may not be in the page cache. If we write the
     * needle with the stale server data, subsequent reads will return
     * the old data instead of zeros.
     *
     * Fix: truncate needle_len to i_size and zero out data beyond i_size.
     * This ensures the server needle doesn't retain stale data from
     * before the truncate. The buffer was zeroed initially, so we just
     * need to adjust needle_len. */
    {
        loff_t isize = i_size_read(inode);
        loff_t first_offset = wpw->offsets[ctx->needle_start_idx];
        loff_t needle_start = first_offset - (first_offset % POWERFS_CHUNK_SIZE);
        __u32 max_needle_len = (__u32)min_t(loff_t, isize - needle_start,
                                             POWERFS_CHUNK_SIZE);

        if (ctx->needle_len > max_needle_len) {
            /* K2-10/K2-12: After truncate, the server needle still has stale
             * data beyond i_size. The Volume Server does NOT truncate the
             * needle when a shorter write is submitted — it only overwrites
             * the first N bytes. So we must submit the FULL original server
             * needle length (with zeros beyond i_size) to overwrite the
             * stale data. The memset below zeros out the stale region. */
            pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu zeroing stale data beyond i_size (needle_len=%u, i_size=%llu, needle_start=%llu) — keeping full needle_len to overwrite server\n",
                    inode->i_ino, (unsigned long long)ctx->needle_id,
                    ctx->needle_len,
                    (unsigned long long)isize,
                    (unsigned long long)needle_start);
            /* Zero out data beyond i_size to prevent stale data */
            if (max_needle_len < POWERFS_CHUNK_SIZE)
                memset(ctx->needle_buf + max_needle_len, 0,
                       ctx->needle_len - max_needle_len);
            /* Do NOT reduce needle_len: submit the full server needle length
             * so the server overwrites stale data with zeros. */
        } else if (ctx->needle_len < max_needle_len) {
            /* K2-11: File may have been extended via ftruncate() to a sparse
             * region beyond the current server needle length. The new region
             * has no pages in the page cache (no data was written there), so
             * the pagecache fill loop above did not extend needle_len.
             *
             * The needle_buf was zero-initialized (kvzalloc), so the gap is
             * already zeros - which is the correct content for the sparse
             * region. Extend needle_len to cover the full range up to i_size
             * so the server needle size matches i_size and subsequent reads
             * of the extended region return zeros instead of short reads. */
            pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu extending needle from %u to %u (i_size=%llu, needle_start=%llu, sparse region zero-filled)\n",
                    inode->i_ino, (unsigned long long)ctx->needle_id,
                    ctx->needle_len, max_needle_len,
                    (unsigned long long)isize,
                    (unsigned long long)needle_start);
            ctx->needle_len = max_needle_len;
        }
    }

    /* 合并脏页数据到 needle_buf (read-modify-write 的 modify) */
    {
        int j;
        for (j = ctx->needle_start_idx; j < ctx->needle_end_idx; j++) {
            struct page *page = wpw->pages[j];
            loff_t offset = wpw->offsets[j];
            size_t count = wpw->counts[j];
            size_t offset_in_needle;

            if (count == 0)
                continue;

            offset_in_needle = offset % POWERFS_CHUNK_SIZE;
            {
                char *kaddr = kmap_local_page(page);
                memcpy(ctx->needle_buf + offset_in_needle, kaddr, count);
                kunmap_local(kaddr);
            }
            if (offset_in_needle + count > ctx->needle_len)
                ctx->needle_len = offset_in_needle + count;
        }
    }

    /* 提交 write_needle_async (第二阶段) */
    pr_debug("powerfs: WB_WRITE_SUBMIT ino=%lu nid=%llu len=%u\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            ctx->needle_len);
    ret = powerfs_net_write_needle_async(
        ctx->volume_id, ctx->needle_id, inode->i_ino,
        ctx->needle_buf, ctx->needle_len,
        ctx->lease_token_len > 0 ? ctx->lease_token : NULL,
        ctx->lease_token_len,
        ctx->req_body, sizeof(ctx->req_body),
        ctx->resp_body, sizeof(ctx->resp_body),
        30000, powerfs_wb_write_cb, ctx);

    if (ret) {
        /* 提交失败: callback 不会触发, 手动清理 */
        pr_warn("powerfs: wb read_cb ino=%lu nid=%llu write submit failed: %d\n",
                inode->i_ino, (unsigned long long)ctx->needle_id, ret);
        powerfs_wb_fail_pages(ctx, ret);
        kvfree(ctx->needle_buf);
        kfree(ctx);
        if (atomic_dec_and_test(&wpw->pending_needles))
            powerfs_wb_final_cleanup(wpw);
    }

    return 0;
}

/* powerfs_wb_write_cb - write_needle 异步完成回调
 *
 * 写响应到达时调用, 清除该 needle 所有页的 PageWriteback, 释放 ctx.
 * 最后一个 needle 完成时触发 final_cleanup. */
static int powerfs_wb_write_cb(struct powerfs_request *req)
{
    struct powerfs_wb_ctx *ctx = req->priv;
    struct powerfs_writepage_work *wpw = ctx->wpw;
    struct inode *inode = wpw->inode;
    int err = 0;

    pr_info("powerfs: WB_WRITE_CB ino=%lu nid=%llu err=%d status=%u pages=[%d,%d)\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            req->error, req->resp_status,
            ctx->needle_start_idx, ctx->needle_end_idx);

    if (req->error < 0) {
        err = req->error;
    } else if (req->resp_status != POWERFS_NET_STATUS_OK) {
        err = net_status_to_errno(req->resp_status);
    }

    if (err)
        pr_warn("powerfs: wb write_cb ino=%lu nid=%llu err=%d status=%u\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                err, req->resp_status);

    /* 完成该 needle 的所有页面 */
    {
        int j;
        for (j = ctx->needle_start_idx; j < ctx->needle_end_idx; j++) {
            struct page *p = wpw->pages[j];
            if (wpw->counts[j] == 0)
                continue;
            if (err)
                mapping_set_error(p->mapping, err);
            end_page_writeback(p);
            put_page(p);
        }
    }

    powerfs_request_free(req);
    kvfree(ctx->needle_buf);
    kfree(ctx);

    if (atomic_dec_and_test(&wpw->pending_needles))
        powerfs_wb_final_cleanup(wpw);

    return 0;
}

/*
 * powerfs_writepage_work_fn - 批量异步写 workqueue 函数 (异步提交模式)
 *
 * 按 needle_id 分组, 每组创建 ctx 并异步提交 read_needle. work_fn 在所有
 * ctx 提交后立即返回 (不阻塞等待网络响应), 由 read_cb → write_cb 两阶段
 * 回调完成实际写入并清除 PageWriteback.
 *
 * needle 模型: write_needle 整体替换 needle 内容, 不支持 partial write.
 * 需 read-modify-write: 读现有 needle (异步) → 合并脏页 (read_cb) → 写回 (异步).
 */
static void powerfs_writepage_work_fn(struct work_struct *work)
{
    struct powerfs_writepage_work *wpw =
        container_of(work, struct powerfs_writepage_work, work);
    struct inode *inode = wpw->inode;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int i;
    int num_groups = 0;
    int group_start = -1;
    __u64 group_needle_id = 0;
    __u64 group_volume_id = 0;

    pr_info("powerfs: WP_START ino=%lu npages=%d\n",
            inode->i_ino, wpw->num_pages);

    if (powerfs_net_is_stopping())
        goto fail_all;

    /* 第一遍: 完成 count==0 的空页, 统计 needle 组数.
     * K3: 按 offset 调用 powerfs_locate_chunk 获取 (volume_id, needle_id),
     *     分组判断改为 (needle_id, volume_id) 元组, 确保 Stripe 模式下
     *     不同 volume 的 chunk 分到不同组. */
    for (i = 0; i < wpw->num_pages; i++) {
        struct page *page = wpw->pages[i];
        loff_t offset = wpw->offsets[i];
        size_t count = wpw->counts[i];
        __u64 needle_id, volume_id;
        int loc_ret;

        cond_resched();

        if (count == 0) {
            /* 空页: 直接完成, 不参与 needle 分组 */
            end_page_writeback(page);
            put_page(page);
            continue;
        }

        spin_lock(&pi->i_lock);
        loc_ret = powerfs_locate_chunk(pi, offset, &volume_id, &needle_id);
        spin_unlock(&pi->i_lock);
        if (loc_ret) {
            pr_warn("powerfs: writepage locate ino=%lu offset=%lld err=%d\n",
                    inode->i_ino, offset, loc_ret);
            mapping_set_error(page->mapping, loc_ret);
            end_page_writeback(page);
            put_page(page);
            continue;
        }

        if (group_start < 0) {
            /* 开始新组 */
            group_start = i;
            group_needle_id = needle_id;
            group_volume_id = volume_id;
            num_groups++;
        } else if (needle_id != group_needle_id ||
                   volume_id != group_volume_id) {
            /* needle/volume 切换: 当前组结束, 开始新组 */
            group_start = i;
            group_needle_id = needle_id;
            group_volume_id = volume_id;
            num_groups++;
        }
    }

    /* 若无有效页面 (全为 count==0 或 locate 失败), 直接清理 */
    if (num_groups == 0) {
        atomic_dec(&sbi->wb_in_flight);
        /* writeback 互斥: 最后一个 batch 完成时释放 wb_mutex */
        if (atomic_dec_and_test(&pi->wb_batch_count))
            mutex_unlock(&pi->wb_mutex);
        iput(inode);
        /* 延迟释放: 同 final_cleanup, work_fn 上下文不能直接 kvfree wpw. */
        call_rcu(&wpw->rcu, powerfs_wb_free_rcu);
        return;
    }

    /* 设置计数: num_groups + 1 (work_fn 持有 +1 ref, 防止提前归零) */
    atomic_set(&wpw->pending_needles, num_groups + 1);

    /* 第二遍: 为每个 needle 组创建 ctx 并异步提交 read_needle.
     * K3: 每个页面持锁 locate 获取 (volume_id, needle_id), 组切换时用
     *     cur_volume_id/cur_needle_id 设置 ctx (Stripe 模式下每组 volume 不同). */
    {
        int cur_start = -1;
        __u64 cur_needle_id = 0;
        __u64 cur_volume_id = 0;

        for (i = 0; i < wpw->num_pages; i++) {
            loff_t offset = wpw->offsets[i];
            size_t count = wpw->counts[i];
            __u64 needle_id, volume_id;
            int loc_ret;

            if (count == 0)
                continue;

            spin_lock(&pi->i_lock);
            loc_ret = powerfs_locate_chunk(pi, offset, &volume_id, &needle_id);
            spin_unlock(&pi->i_lock);
            if (loc_ret) {
                /* locate 失败: 跳过该页 (第一遍已 end_page_writeback) */
                continue;
            }

            if (cur_start < 0) {
                cur_start = i;
                cur_needle_id = needle_id;
                cur_volume_id = volume_id;
            } else if (needle_id != cur_needle_id ||
                       volume_id != cur_volume_id) {
                /* 提交 [cur_start, i) 这一组 */
                struct powerfs_wb_ctx *ctx;
                loff_t group_offset;
                size_t token_len = 0;
                int ret;

                ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
                if (!ctx) {
                    /* 分配失败: 失败该组页面, dec 计数 (此 ctx 不会回调) */
                    struct powerfs_wb_ctx fail_ctx = {
                        .wpw = wpw,
                        .needle_start_idx = cur_start,
                        .needle_end_idx = i,
                    };
                    powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                    atomic_dec(&wpw->pending_needles);
                    goto next_group;
                }

                ctx->needle_buf = kvzalloc(POWERFS_CHUNK_SIZE, GFP_NOFS);
                if (!ctx->needle_buf) {
                    struct powerfs_wb_ctx fail_ctx = {
                        .wpw = wpw,
                        .needle_start_idx = cur_start,
                        .needle_end_idx = i,
                    };
                    kfree(ctx);
                    powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                    atomic_dec(&wpw->pending_needles);
                    goto next_group;
                }
                memset(ctx->needle_buf, 0, POWERFS_CHUNK_SIZE);

                ctx->wpw = wpw;
                ctx->needle_start_idx = cur_start;
                ctx->needle_end_idx = i;
                ctx->volume_id = cur_volume_id;
                ctx->needle_id = cur_needle_id;
                ctx->needle_len = 0;

                /* 获取 lease (同步, 快速: ensure_lease 有快速路径) */
                group_offset = wpw->offsets[cur_start];
                if (powerfs_get_lease_token(inode, group_offset,
                                            ctx->lease_token,
                                            &token_len)) {
                    pr_warn("powerfs: writepage lease ino=%lu stripe=%llu, continuing without lease\n",
                            inode->i_ino,
                            (unsigned long long)(group_offset / POWERFS_STRIPE_SIZE
                                                 * POWERFS_STRIPE_SIZE));
                }
                ctx->lease_token_len = token_len;

                /* 异步提交 read_needle (非阻塞) */
                pr_debug("powerfs: WB_READ_SUBMIT ino=%lu vid=%llu nid=%llu pages=[%d,%d)\n",
                        inode->i_ino, (unsigned long long)cur_volume_id,
                        (unsigned long long)cur_needle_id, cur_start, i);
                ret = powerfs_net_read_needle_async(
                    cur_volume_id, cur_needle_id,
                    ctx->needle_buf, POWERFS_CHUNK_SIZE,
                    ctx->req_body, sizeof(ctx->req_body),
                    30000, powerfs_wb_read_cb, ctx);

                if (ret) {
                    /* 提交失败: callback 不会触发, 手动清理 */
                    pr_warn("powerfs: writepage read submit ino=%lu nid=%llu err=%d\n",
                            inode->i_ino, (unsigned long long)cur_needle_id,
                            ret);
                    powerfs_wb_fail_pages(ctx, ret);
                    kvfree(ctx->needle_buf);
                    kfree(ctx);
                    /* dec 计数: 此 ctx 不会回调 */
                    if (atomic_dec_and_test(&wpw->pending_needles))
                        powerfs_wb_final_cleanup(wpw);
                }

next_group:
                cur_start = i;
                cur_needle_id = needle_id;
                cur_volume_id = volume_id;
            }
        }

        /* 提交最后一组 [cur_start, num_pages) */
        if (cur_start >= 0) {
            struct powerfs_wb_ctx *ctx;
            loff_t group_offset;
            size_t token_len = 0;
            int ret;

            ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
            if (!ctx) {
                struct powerfs_wb_ctx fail_ctx = {
                    .wpw = wpw,
                    .needle_start_idx = cur_start,
                    .needle_end_idx = wpw->num_pages,
                };
                powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                atomic_dec(&wpw->pending_needles);
                goto last_group_done;
            }

            ctx->needle_buf = kvzalloc(POWERFS_CHUNK_SIZE, GFP_NOFS);
            if (!ctx->needle_buf) {
                struct powerfs_wb_ctx fail_ctx = {
                    .wpw = wpw,
                    .needle_start_idx = cur_start,
                    .needle_end_idx = wpw->num_pages,
                };
                kfree(ctx);
                powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                atomic_dec(&wpw->pending_needles);
                goto last_group_done;
            }
            memset(ctx->needle_buf, 0, POWERFS_CHUNK_SIZE);

            ctx->wpw = wpw;
            ctx->needle_start_idx = cur_start;
            ctx->needle_end_idx = wpw->num_pages;
            ctx->volume_id = cur_volume_id;
            ctx->needle_id = cur_needle_id;
            ctx->needle_len = 0;

            group_offset = wpw->offsets[cur_start];
            if (powerfs_get_lease_token(inode, group_offset,
                                        ctx->lease_token,
                                        &token_len)) {
                pr_warn("powerfs: writepage final lease ino=%lu stripe=%llu, continuing without lease\n",
                        inode->i_ino,
                        (unsigned long long)(group_offset / POWERFS_STRIPE_SIZE
                                             * POWERFS_STRIPE_SIZE));
            }
            ctx->lease_token_len = token_len;

            ret = powerfs_net_read_needle_async(
                cur_volume_id, cur_needle_id,
                ctx->needle_buf, POWERFS_CHUNK_SIZE,
                ctx->req_body, sizeof(ctx->req_body),
                30000, powerfs_wb_read_cb, ctx);

            if (ret) {
                pr_warn("powerfs: writepage read submit ino=%lu nid=%llu err=%d\n",
                        inode->i_ino, (unsigned long long)cur_needle_id,
                        ret);
                powerfs_wb_fail_pages(ctx, ret);
                kvfree(ctx->needle_buf);
                kfree(ctx);
                if (atomic_dec_and_test(&wpw->pending_needles))
                    powerfs_wb_final_cleanup(wpw);
            }
        }
    }

last_group_done:
    /* work_fn 自身 ref: 所有 ctx 已提交 (或失败已处理), dec 自身引用.
     * 若所有 ctx 已完成 (极端竞态: 提交后立即回调), 此处归零触发 final_cleanup. */
    if (atomic_dec_and_test(&wpw->pending_needles))
        powerfs_wb_final_cleanup(wpw);

    pr_debug("powerfs: WP_SUBMIT_DONE ino=%lu groups=%d\n",
            inode->i_ino, num_groups);
    return;  /* workqueue 线程立即释放 */

fail_all:
    for (i = 0; i < wpw->num_pages; i++) {
        mapping_set_error(wpw->pages[i]->mapping, -EIO);
        end_page_writeback(wpw->pages[i]);
        put_page(wpw->pages[i]);
    }
    atomic_dec(&sbi->wb_in_flight);
    /* writeback 互斥: 最后一个 batch 完成时释放 wb_mutex */
    if (atomic_dec_and_test(&pi->wb_batch_count))
        mutex_unlock(&pi->wb_mutex);
    iput(inode);
    /* 延迟释放: 同 final_cleanup, work_fn 上下文不能直接 kvfree wpw
     * (work_struct 生命周期约束). */
    call_rcu(&wpw->rcu, powerfs_wb_free_rcu);
}

/*
 * powerfs_writepages - 批量 writeback 入口
 *
 * 自己遍历脏页 (pagevec_lookup_range_tag), 批量收集到 work item.
 * 替代 VFS 默认的 write_cache_pages + writepage 逐页模式.
 *
 * 并发控制:
 *   - max_active=4 限制全局并发 worker (workqueue 级)
 *   - batch 内串行发送 (单 work item 内页面顺序写)
 *   - writepages 由 writeback 子系统串行调用 (per-inode 不会并发)
 */
int powerfs_writepages(struct address_space *mapping,
                               struct writeback_control *wbc)
{
    struct inode *inode = mapping->host;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct folio_batch fbatch;
    pgoff_t index = wbc->range_start >> PAGE_SHIFT;
    pgoff_t end = wbc->range_end >> PAGE_SHIFT;
    struct powerfs_writepage_work *batch = NULL;
    int batch_pages = sbi->write_batch_pages;
    int ret = 0;

    pr_info("powerfs: WPAGES ino=%lu range=%llu-%llu nr_to_write=%ld sync_mode=%d placement=%u\n",
            inode->i_ino, wbc->range_start, wbc->range_end, wbc->nr_to_write,
            wbc->sync_mode, pi->placement);

    /* 目录无数据页: 跳过 writeback 避免无意义的网络 I/O.
     * 根目录 (ino=1) 的元数据修改 (nlink/mtime) 由 write_inode 同步,
     * 不走 page cache writeback 路径. */
    if (S_ISDIR(inode->i_mode)) {
        pr_debug("powerfs: WPAGES skip directory ino=%lu\n", inode->i_ino);
        return 0;
    }

    /* K2: Inline 文件不走 Volume Server writeback.
     * 数据已在 write_end 中同步到 inline_data 缓冲,
     * close 时通过 UPDATE_INODE 提交到 Filer.
     * 这里清理脏页标记, 让 writeback 认为已完成. */
    if (POWERFS_I(inode)->placement == POWERFS_PLACEMENT_INLINE) {
        struct folio_batch fbatch2;
        pgoff_t idx2 = wbc->range_start >> PAGE_SHIFT;
        pgoff_t end2 = wbc->range_end >> PAGE_SHIFT;

        pr_debug("powerfs: WPAGES INLINE ino=%lu, clean dirty pages\n", inode->i_ino);
        folio_batch_init(&fbatch2);
        while (filemap_get_folios_tag(mapping, &idx2, end2,
                                       PAGECACHE_TAG_DIRTY, &fbatch2) > 0) {
            int i;
            for (i = 0; i < folio_batch_count(&fbatch2); i++) {
                struct folio *f = fbatch2.folios[i];
                folio_lock(f);
                folio_clear_dirty(f);
                folio_unlock(f);
                folio_put(f);
            }
            folio_batch_init(&fbatch2);
            if (idx2 > end2)
                break;
        }
        return 0;
    }

    if (powerfs_net_is_stopping()) {
        pr_debug("powerfs: WPAGES net stopping, skip ino=%lu\n", inode->i_ino);
        return 0;
    }

    /* 并发限制: 防止过多 work item 同时阻塞在网络 I/O 导致 workqueue lockup.
     * 如果已有太多 in-flight work item, 让 VFS 稍后重试. */
    if (atomic_read(&sbi->wb_in_flight) >= POWERFS_WB_MAX_IN_FLIGHT) {
        pr_debug("powerfs: WPAGES throttled ino=%lu in_flight=%d\n",
                inode->i_ino, atomic_read(&sbi->wb_in_flight));
        wbc->pages_skipped += wbc->nr_to_write;
        return 0;
    }

    /* writeback 互斥: 确保同一 inode 的 writeback 串行执行.
     * powerfs_writepages 是异步的 (queue_work), 返回后异步 work 可能仍在执行.
     * 若 writeback 线程再次调用 powerfs_writepages 处理同一 needle 的不同
     * 页面, 两个 RMW 并发会导致后写入的覆盖先写入的数据 (data corruption).
     * mutex_lock 等待上一次 writeback 的所有 batch 完成.
     *
     * 自引用 (+1): writepages 在提交 batch 期间持有一份 wb_batch_count 引用,
     * 防止已提交的 batch 快速完成后将 count 归零并释放 wb_mutex, 导致后续
     * batch 提交期间另一次 writepages 并发执行. writepages 结束时释放此引用. */
    mutex_lock(&pi->wb_mutex);
    atomic_inc(&pi->wb_batch_count);

    folio_batch_init(&fbatch);

    while (index <= end) {
        int nr_pages, i;

        nr_pages = filemap_get_folios_tag(mapping, &index, end,
                                           PAGECACHE_TAG_DIRTY, &fbatch);
        if (!nr_pages) {
            pr_debug("powerfs: WPAGES no dirty pages, index=%lu end=%lu\n",
                    index, end);
            break;
        }
        pr_debug("powerfs: WPAGES found %d dirty pages, index=%lu\n",
                nr_pages, index);

        for (i = 0; i < nr_pages; i++) {
            struct page *page = folio_page(fbatch.folios[i], 0);
            loff_t offset;
            size_t count = PAGE_SIZE;
            u64 cur_needle_idx;

            lock_page(page);
            if (!PageDirty(page)) {
                pr_debug("powerfs: WPAGES page not dirty, skip\n");
                unlock_page(page);
                continue;
            }

            clear_page_dirty_for_io(page);

            offset = page_offset(page);
            /* 按 needle (chunk) 边界分组: 当页面的 chunk_idx 变化时,
             * 提交当前 batch, 确保同一 needle 的所有页面在同一个 batch
             * 中完成 read-modify-write. 避免多个 batch 并发对同一 needle
             * 做 RMW 导致数据覆盖 (1MB 文件 16 个 batch 并发覆盖问题). */
            cur_needle_idx = offset / POWERFS_CHUNK_SIZE;
            if (batch && batch->num_pages > 0) {
                u64 prev_needle_idx = batch->offsets[batch->num_pages - 1]
                                      / POWERFS_CHUNK_SIZE;
                if (cur_needle_idx != prev_needle_idx) {
                    /* needle 边界变化: 提交当前 batch */
                    pr_info("powerfs: WPAGES SUBMIT ino=%lu batch npages=%d needle_idx=%llu offset=%lld-%lld\n",
                            inode->i_ino, batch->num_pages, prev_needle_idx,
                            batch->offsets[0],
                            batch->offsets[batch->num_pages - 1] + batch->counts[batch->num_pages - 1]);
                    atomic_inc(&pi->wb_batch_count);
                    atomic_inc(&sbi->wb_in_flight);
                    queue_work(sbi->writeback_wq, &batch->work);
                    batch = NULL;
                }
            }

            pr_debug("powerfs: WPAGES page ino=%lu offset=%lld i_size=%lld\n",
                    inode->i_ino, offset, i_size_read(inode));
            if (offset >= i_size_read(inode)) {
                pr_debug("powerfs: WPAGES offset >= i_size, SKIP page\n");
                unlock_page(page);
                continue;
            }
            if (offset + count > i_size_read(inode))
                count = i_size_read(inode) - offset;

            /* 分配 batch (如果当前没有 pending) */
            if (!batch) {
                struct inode *grabbed;
                batch = powerfs_alloc_write_batch(batch_pages, GFP_NOFS);
                if (!batch) {
                    redirty_page_for_writepage(wbc, page);
                    unlock_page(page);
                    continue;
                }
                grabbed = igrab(inode);
                if (!grabbed) {
                    /* inode 正在被 evict (I_FREEING), 无法提交 writeback.
                     * 释放 batch, redirty 页面. */
                    kvfree(batch);
                    batch = NULL;
                    redirty_page_for_writepage(wbc, page);
                    unlock_page(page);
                    continue;
                }
                INIT_WORK(&batch->work, powerfs_writepage_work_fn);
                batch->inode = grabbed;
            }

            /* 添加页面到 batch */
            get_page(page);
            batch->pages[batch->num_pages] = page;
            batch->offsets[batch->num_pages] = offset;
            batch->counts[batch->num_pages] = count;
            batch->num_pages++;

            set_page_writeback(page);
            unlock_page(page);

            /* batch 满了，提交到 workqueue */
            if (batch->num_pages >= batch_pages) {
                pr_info("powerfs: WPAGES FULL ino=%lu batch npages=%d offset=%lld-%lld\n",
                        inode->i_ino, batch->num_pages,
                        batch->offsets[0],
                        batch->offsets[batch->num_pages - 1] + batch->counts[batch->num_pages - 1]);
                atomic_inc(&pi->wb_batch_count);
                atomic_inc(&sbi->wb_in_flight);
                queue_work(sbi->writeback_wq, &batch->work);
                batch = NULL;
            }

            wbc->nr_to_write--;
            if (wbc->nr_to_write <= 0)
                goto done;
        }
        folio_batch_release(&fbatch);
        cond_resched();
    }

done:
    folio_batch_release(&fbatch);

    /* 提交剩余的 batch */
    if (batch) {
        if (batch->num_pages > 0) {
            atomic_inc(&pi->wb_batch_count);
            atomic_inc(&sbi->wb_in_flight);
            queue_work(sbi->writeback_wq, &batch->work);
        } else {
            iput(batch->inode);
            kvfree(batch);
        }
    }

    /* writeback 互斥: 释放 writepages 自身引用.
     * 若所有 batch 已完成 (或无 batch 提交), 此处归零并释放 wb_mutex.
     * 这防止了 batch 完成后提前释放 wb_mutex, 导致后续 batch 提交期间
     * 另一次 writepages 并发执行造成同一 needle 的 RMW 数据覆盖. */
    if (atomic_dec_and_test(&pi->wb_batch_count))
        mutex_unlock(&pi->wb_mutex);

    return ret;
}

/*
 * powerfs_writepage - 单页 writeback (fallback, VFS 内部路径使用)
 *
 * writepages 注册后, VFS 优先调用 writepages. writepage 仅作为 fallback:
 *   - migrate_pages 等内核内部路径可能直接调用 writepage
 *   - 保持与旧接口兼容
 */
int powerfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = page->mapping->host;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_writepage_work *wpw;
    loff_t offset = page_offset(page);
    size_t count = PAGE_SIZE;

    if (powerfs_net_is_stopping()) {
        redirty_page_for_writepage(wbc, page);
        unlock_page(page);
        return 0;
    }

    if (offset >= i_size_read(inode)) {
        unlock_page(page);
        return 0;
    }
    if (offset + count > i_size_read(inode))
        count = i_size_read(inode) - offset;

    /* writeback 互斥: 与 powerfs_writepages 共用 wb_mutex, 防止
     * 单页 fallback 与批量 writeback 并发对同一 needle 做 RMW. */
    mutex_lock(&pi->wb_mutex);
    /* 自引用 + batch 引用: work_fn 完成时 dec batch 引用,
     * writepage 结束时 dec 自引用, 归零者释放 wb_mutex. */
    atomic_inc(&pi->wb_batch_count);

    wpw = powerfs_alloc_write_batch(1, GFP_NOFS);
    if (!wpw) {
        redirty_page_for_writepage(wbc, page);
        unlock_page(page);
        if (atomic_dec_and_test(&pi->wb_batch_count))
            mutex_unlock(&pi->wb_mutex);
        return 0;
    }

    INIT_WORK(&wpw->work, powerfs_writepage_work_fn);
    wpw->inode = igrab(inode);
    if (!wpw->inode) {
        /* inode 正在被 evict (I_FREEING), 无法提交 writeback.
         * 释放 wpw, redirty 页面. */
        kvfree(wpw);
        redirty_page_for_writepage(wbc, page);
        unlock_page(page);
        if (atomic_dec_and_test(&pi->wb_batch_count))
            mutex_unlock(&pi->wb_mutex);
        return 0;
    }
    get_page(page);
    wpw->pages[0] = page;
    wpw->offsets[0] = offset;
    wpw->counts[0] = count;
    wpw->num_pages = 1;

    set_page_writeback(page);
    unlock_page(page);
    atomic_inc(&pi->wb_batch_count);   /* batch 引用 */
    atomic_inc(&sbi->wb_in_flight);
    queue_work(sbi->writeback_wq, &wpw->work);

    /* 释放自引用; 若 batch 已完成, 此处归零并释放 wb_mutex */
    if (atomic_dec_and_test(&pi->wb_batch_count))
        mutex_unlock(&pi->wb_mutex);

    return 0;
}

/*
 * powerfs_write_begin - 写开始 (Stage C: 对接 netfs 子系统)
 *
 * 参照 xxx_write_begin (fs/xxx/addr.c):
 *   调用 netfs_write_begin 让 netfs 管理页面准备, 包括:
 *   - 分配并锁定 folio
 *   - 若 folio 不在 page cache 或非 uptodate, 通过 issue_read
 *     从 Filer 拉取现有数据 (处理 partial write 的 read-modify-write)
 *   - folio 返回时已锁定, 调用者写入后通过 write_end 解锁
 *
 * 替代旧的 grab_cache_page_write_begin + zero_user 方案:
 *   旧方案对 partial write (offset/len 未覆盖整页) 会丢失未写入部分
 *   的现有数据, 因为 zero_user 把整页清零. netfs_write_begin 会先读取
 *   现有数据, write_end 只更新写入部分, 保证 read-modify-write 正确性.
 */
int powerfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
                         loff_t pos, unsigned int len, struct folio **foliop,
                         void **fsdata)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct folio *folio = NULL;
    unsigned int got_caps = 0;
    int ret;

    pr_debug("powerfs: write_begin ino=%lu pos=%lld len=%u i_size=%lld\n",
            inode->i_ino, pos, len, i_size_read(inode));

    /* P2-7: Quota check — 字节配额 (写入后 i_size 不能超过父目录 max_bytes) */
    {
        loff_t newlen = pos + len;
        if (newlen > i_size_read(inode)) {
            ret = powerfs_quota_check_max_bytes(inode, newlen);
            if (ret)
                return ret;
        }
    }

    /* ========== Cap 仲裁 (对齐  xxx_write_begin → try_get_caps) ==========
     *
     * 写页面必须至少拿到 FILE_SHARED (读) + FILE_WR (写) 位.
     *   - FULL (同时拿到 FILE_CACHE + FILE_EXCL): 可完全信任本地缓存, write_end
     *     直接写 pagecache 并标记 dirty, revoke 时 flush 保障一致性.
     *   - PARTIAL (只拿到 FILE_SHARED|WR): 本地能写但没独占权, 也放行.
     *     netfs_write_begin 遇到 partial page 会从服务端回源拉现有数据,
     *     不会产生 stale 数据.
     *   - NONE (连 FILE_SHARED 都没): 仍然放行 (降级模式, 等 cap 协议对接
     *     服务端 grant 到位后严格化). try_get_caps 会触发 check_caps 催
     *     服务端 AcquireCap, 下次 IO 再命中.
     *
     * 使用 nonblock=true: write_begin 是 VFS 页缓存准备路径, 不可长时间阻塞.
     * got_caps 通过 *fsdata 带到 write_end 对称 cap_put_refs. */
    if (iocb) {
        powerfs_try_get_caps(pi,
                             POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_WR,
                             POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE |
                             POWERFS_CAP_FILE_WR     | POWERFS_CAP_FILE_EXCL,
                             true, &got_caps);
    }
    *fsdata = (void *)(uintptr_t)got_caps;

    /* page_symlink() 调用时 iocb==NULL (无 file 描述符),
     * 不能走 netfs_write_begin (需要 file 参数).
     * 改用 __filemap_get_folio 直接获取 page cache folio. */
    if (!iocb) {
        folio = __filemap_get_folio(mapping, pos / PAGE_SIZE,
                                    FGP_WRITEBEGIN | FGP_LOCK | FGP_CREAT,
                                    mapping_gfp_mask(mapping));
        if (!folio)
            return -ENOMEM;
        *foliop = folio;
        return 0;
    }

    ret = netfs_write_begin(&pi->netfs, iocb->ki_filp, inode->i_mapping,
                            pos, len, &folio, NULL);
    if (ret < 0) {
        pr_warn("powerfs: write_begin netfs_write_begin failed: %d\n", ret);
        /* netfs 失败时对称释放 try_get_caps 占的引用, 避免泄漏. */
        if (got_caps)
            powerfs_cap_put_refs(pi, got_caps);
        return ret;
    }

    pr_debug("powerfs: WB_BEGIN ok ino=%lu folio=%px order=%d locked=%d uptodate=%d index=%lu\n",
            inode->i_ino, folio, folio_order(folio),
            folio_test_locked(folio), folio_test_uptodate(folio),
            folio->index);

    WARN_ON_ONCE(!folio_test_locked(folio));
    *foliop = folio;
    /* 注意: *fsdata 已在函数开头写入 got_caps, 这里不要覆盖,
     * 这样 write_end 能取出它做对称 cap_put_refs. */
    return 0;
}

/*
 * powerfs_migrate_inline_to_flat - K2-7 Inline → Flat 自动迁移
 *
 * 对齐 FUSE write inline migrate (powerfs-fuse/src/fuse.rs L3446):
 *   1. 快照 inline_data (持 i_lock)
 *   2. 调 Filer MIGRATE_INLINE_ALLOC 分配 (volume_id, needle_id)
 *   3. 同步写 merged_data 到 Volume Server (WriteNeedle, lease_token=NULL)
 *   4. 持锁切换 inode: placement=Flat + volume_id + file_key + 清 inline_data
 *
 * crash safety (对齐 FUSE):
 *   - Filer 分配后不修改 inode, 保留 inline_data
 *   - 客户端崩溃后 Filer 仍有 inline_data, 文件仍可作 Inline 读
 *   - 分配的 needle_id 泄漏 (可接受, 同 CREATE 失败)
 *   - 客户端写 Volume Server 成功后, close 时 UPDATE_INODE_SIZE_CHUNKS
 *     原子完成切换 (清除 inline_data + 设 Flat chunks)
 *
 * 注意: 本函数在 write_end 中调用, 持有 folio lock. 网络 I/O 期间
 *       folio lock 被持有, 但 inline 路径已在 write_end 中做 kvmalloc
 *       (可睡眠), 故阻塞操作可接受. 迁移数据最大 8KB, 网络往返 <100ms.
 *
 * 返回 0 成功, 负数错误码 (网络错误透传, inline_data 保持原状).
 */
static int powerfs_migrate_inline_to_flat(struct inode *inode,
                                          struct powerfs_inode_info *pi)
{
    u8 *snap_data = NULL;
    u32 snap_len = 0;
    u64 shard_id, ino = inode->i_ino;
    u64 volume_id = 0, file_key = 0;
    int ret;

    /* 1. 持锁快照 inline_data (网络 I/O 不能持 spinlock) */
    spin_lock(&pi->i_lock);
    if (!pi->inline_data || pi->inline_len == 0) {
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: MIGRATE ino=%lu no inline_data to migrate\n", ino);
        return -EINVAL;
    }
    snap_len = pi->inline_len;
    spin_unlock(&pi->i_lock);

    snap_data = kmalloc(snap_len, GFP_KERNEL);
    if (!snap_data) {
        pr_warn("powerfs: MIGRATE ino=%lu kmalloc %u failed\n", ino, snap_len);
        return -ENOMEM;
    }
    spin_lock(&pi->i_lock);
    if (pi->inline_data && pi->inline_len == snap_len) {
        memcpy(snap_data, pi->inline_data, snap_len);
    } else {
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: MIGRATE ino=%lu inline_data changed during snapshot\n", ino);
        kfree(snap_data);
        return -EAGAIN;
    }
    spin_unlock(&pi->i_lock);

    pr_info("powerfs: MIGRATE ino=%lu inline_len=%u → triggering Flat migration\n",
            ino, snap_len);

    /* 2. 调 Filer MIGRATE_INLINE_ALLOC 分配 (volume_id, needle_id).
     * shard_id = shard_map_route(parent_ino) — 区间路由, 对齐 FUSE ShardMap. */
    shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);
    ret = powerfs_net_migrate_inline_alloc(shard_id, ino, &volume_id, &file_key);
    if (ret < 0) {
        pr_warn("powerfs: MIGRATE ino=%lu alloc failed: %d, inline buffer unmodified\n",
                ino, ret);
        kfree(snap_data);
        return ret;  /* 透传网络错误 (-ENOTCONN/-ETIMEDOUT 等), 非 EFBIG */
    }

    /* 3. 同步写 snap_data 到 Volume Server (WriteNeedle).
     * lease_token=NULL: Volume Server 不校验 lease (net_handler.rs L92).
     * ClientId="kernel-client" 必须发送 (write_needle L5934 注释). */
    ret = powerfs_net_write_needle(volume_id, file_key, ino,
                                    snap_data, snap_len,
                                    NULL, 0);
    if (ret < 0) {
        pr_warn("powerfs: MIGRATE ino=%lu write_needle failed: %d, needle_id=%#llx leaked\n",
                ino, ret, (unsigned long long)file_key);
        kfree(snap_data);
        return ret;  /* 透传网络错误, 非 EFBIG */
    }

    pr_info("powerfs: MIGRATE ino=%lu write_needle OK volume_id=%llu needle_id=%#llx size=%u\n",
            ino, (unsigned long long)volume_id,
            (unsigned long long)file_key, snap_len);

    /* 4. 持锁切换 inode 到 Flat: 释放 inline_data, 更新布局元数据.
     * 后续 write 走 Flat writeback 路径, close 时 UPDATE_INODE_SIZE_CHUNKS
     * 同步 size+chunks 到 Filer (原子清除 inline_data + 设 Flat chunks). */
    spin_lock(&pi->i_lock);
    pi->placement = POWERFS_PLACEMENT_FLAT;
    pi->volume_id = volume_id;
    pi->file_key = file_key;
    pi->layout_chunk_size = POWERFS_CHUNK_SIZE;
    kfree(pi->inline_data);
    pi->inline_data = NULL;
    pi->inline_len = 0;
    pi->inline_dirty = false;
    spin_unlock(&pi->i_lock);

    kfree(snap_data);

    /* K2-9: After migration, the Volume Server only has inline_data (≤8KB).
     * Pages beyond inline_data's range were written via write_end (which
     * marks them dirty) but INLINE writeback cleared their dirty flags
     * without writing to the server. These clean pages are only in the
     * page cache — if they get evicted (memory pressure, refresh_work),
     * their data is lost forever.
     *
     * Fix: mark ALL pages in the page cache as dirty. This ensures the
     * next FLAT writeback will RMW all pages (not just the currently
     * dirty ones) and produce a complete needle on the Volume Server.
     * The overhead is one writeback of the full file, which is acceptable
     * since migration is a one-time event.
     *
     * Note: migration is called from write_end which holds the current
     * folio's lock. Use folio_trylock to avoid deadlock — if a folio
     * is already locked (the current write_end folio), it's already
     * dirty (write_end marks it dirty before calling migrate). */
    {
        struct folio_batch fbatch;
        pgoff_t idx = 0;
        int re_dirty_count = 0;

        folio_batch_init(&fbatch);
        while (filemap_get_folios(inode->i_mapping, &idx,
                                  (pgoff_t)-1, &fbatch) > 0) {
            int i;
            for (i = 0; i < folio_batch_count(&fbatch); i++) {
                struct folio *f = fbatch.folios[i];
                if (!folio_test_dirty(f)) {
                    if (folio_trylock(f)) {
                        if (!folio_test_dirty(f))
                            folio_mark_dirty(f);
                        folio_unlock(f);
                        re_dirty_count++;
                    }
                    /* If trylock fails, folio is locked by current
                     * write_end — it's already dirty, skip. */
                }
                folio_put(f);
            }
            folio_batch_init(&fbatch);
        }
        if (re_dirty_count > 0) {
            pr_info("powerfs: MIGRATE ino=%lu re-dirtied %d clean pages (data beyond inline_data not on server)\n",
                    ino, re_dirty_count);
        }
    }

    pr_info("powerfs: MIGRATE ino=%lu → Flat done, subsequent writes → Volume Server\n", ino);
    return 0;
}

/*
 * powerfs_write_end - 写结束 (Stage C: 纯 page cache 更新, 无网络 IO)
 *
 * 参照 xxx_write_end (fs/xxx/addr.c):
 *   - 标记 folio uptodate
 *   - 更新本地 i_size (i_size_write + mark_inode_dirty)
 *   - folio_mark_dirty 让 writeback 子系统负责持久化
 *   - 不做任何网络 IO, 数据持久化由 writepage 异步完成,
 *     i_size 同步由 write_inode (writeback 时) 完成
 *
 * 替代旧的同步写方案:
 *   旧方案在 write_end 中同步调用 powerfs_net_write + powerfs_net_setattr,
 *   导致 write() 系统调用阻塞在网络往返上, 高并发下性能差.
 *   Stage C 改为纯 page cache 更新, write() 立即返回,
 *   持久化推迟到 writeback (writepage + write_inode).
 */
int powerfs_write_end(const struct kiocb *iocb, struct address_space *mapping,
                       loff_t pos, unsigned int len, unsigned int copied,
                       struct folio *folio, void *fsdata)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t end_pos = pos + copied;
    unsigned int got_caps = (unsigned int)(uintptr_t)fsdata;

    pr_debug("powerfs: WB_END ino=%lu pos=%lld copied=%u len=%u i_size=%lld uptodate=%d\n",
            inode->i_ino, pos, copied, len, i_size_read(inode),
            folio_test_uptodate(folio));

    if (copied > 0) {
        if (!folio_test_uptodate(folio)) {
            if (copied < len) {
                pr_debug("powerfs: WB_END partial write, return 0 for retry\n");
                copied = 0;
                goto out;
            }
            folio_mark_uptodate(folio);
        }
        if (end_pos > i_size_read(inode)) {
            i_size_write(inode, end_pos);
            mark_inode_dirty(inode);
        }
        folio_mark_dirty(folio);

        /* 标记 FILE_WR cap dirty — writeback/revoke 时 flush 到 Filer.
         * 对齐  xxx_write_end → __xxx_mark_caps_dirty(CEPH_CAP_FILE_WR). */
        powerfs_cap_mark_dirty(pi, POWERFS_CAP_FILE_WR);

        /* K2: Inline 模式 — 同步写入数据到 inline_data 缓冲.
         * writeback 不会将 Inline 文件的数据发送到 Volume Server,
         * 而是在 close 时通过 UPDATE_INODE 提交 inline_data 到 Filer.
         *
         * 这里在 write_end 中直接拷贝 folio 数据到 inline_data,
         * 确保 inline_data 始终与 page cache 同步.
         *
         * 并发保护: 持 i_lock 修改 inline_data.
         * 注意: kvmalloc 在持 spinlock 时可能睡眠 (GFP_KERNEL),
         * 因此先在锁外分配, 再持锁替换. */
        if (pi->placement == POWERFS_PLACEMENT_INLINE) {
            size_t folio_off = offset_in_folio(folio, pos);
            size_t need_len = end_pos;
            u8 *new_buf = NULL;

            /* K2: 仅首次写入(pos==0)输出日志, 避免 bs=1 时 8192 条日志淹没 serial */
            if (pos == 0)
                pr_info("powerfs: WB_END INLINE first write ino=%lu copied=%u end=%zu\n",
                        inode->i_ino, copied, end_pos);

            /* K2-7: 检查是否超出 inline 硬上限 (8KB).
             * 超出时截断到 INLINE_MAX_SIZE, 写入后触发迁移到 Flat.
             * 迁移阈值 = min(inline_max_size × 1.5, INLINE_MAX_SIZE),
             * 对齐 FUSE fuse.rs L3444. */
            if (need_len > POWERFS_INLINE_MAX_SIZE) {
                pr_warn("powerfs: WB_END INLINE ino=%lu write end=%zu > INLINE_MAX=%d, "
                        "will migrate after write\n",
                        inode->i_ino, need_len, POWERFS_INLINE_MAX_SIZE);
                need_len = POWERFS_INLINE_MAX_SIZE;
                copied = min_t(unsigned int, copied,
                               POWERFS_INLINE_MAX_SIZE - pos);
                if (copied == 0) {
                    /* pos 已超 8KB, 无法写入 inline_data.
                     * 返回 -EFBIG 触发上层重试 (此时若已迁移则走 Flat). */
                    folio_unlock(folio);
                    folio_put(folio);
                    return -EFBIG;
                }
                end_pos = pos + copied;
            }

            /* 若 inline_data 未分配或不够大, 在锁外分配新 buffer */
            spin_lock(&pi->i_lock);
            if (!pi->inline_data || pi->inline_len < need_len) {
                u8 *old = pi->inline_data;
                u32 old_len = pi->inline_len;
                spin_unlock(&pi->i_lock);

                /* 锁外分配 (GFP_KERNEL 可睡眠) */
                new_buf = kvmalloc(need_len, GFP_KERNEL);
                if (!new_buf) {
                    pr_warn("powerfs: WB_END INLINE ino=%lu kvmalloc %zu failed\n",
                            inode->i_ino, need_len);
                    /* 分配失败不影响 page cache, writeback 仍会标脏页.
                     * inline_data 不更新, close 时可能丢失数据.
                     * 后续可回退到 Flat 模式. */
                    goto inline_done;
                }
                /* 拷贝旧数据到新 buffer */
                if (old && old_len > 0)
                    memcpy(new_buf, old, old_len);
                /* 新区域清零 */
                if (need_len > old_len)
                    memset(new_buf + old_len, 0, need_len - old_len);

                /* 持锁替换 */
                spin_lock(&pi->i_lock);
                kfree(pi->inline_data);
                pi->inline_data = new_buf;
                pi->inline_len = need_len;
                /* new_buf 所有权已转移, 防止下方 kfree */
                new_buf = NULL;
            }

            /* 从 folio 拷贝写入的数据到 inline_data */
            if (pi->inline_data && pos + copied <= pi->inline_len) {
                void *kaddr = kmap_local_folio(folio, folio_off);
                memcpy(pi->inline_data + pos, kaddr, copied);
                kunmap_local(kaddr);
                pi->inline_dirty = true;
                pr_debug("powerfs: WB_END INLINE ino=%lu pos=%lld copied=%u inline_len=%u\n",
                        inode->i_ino, pos, copied, pi->inline_len);
            }
            spin_unlock(&pi->i_lock);

            /* 分配了但未使用 (被并发覆盖) 的 buffer 释放 */
            kfree(new_buf);

            /* K2-7: 检查是否需要迁移到 Flat.
             * 迁移阈值 = min(inline_max_size × 1.5, POWERFS_INLINE_MAX_SIZE).
             * 对齐 FUSE fuse.rs L3444: 滞后窗口避免边界抖动.
             * 迁移成功: inode 切换到 Flat, 后续 write 走 Flat writeback.
             * 迁移失败: 透传网络错误码, inline_data 保持原状 (close 时同步到 Filer). */
            {
                u32 migrate_threshold = min(pi->inline_max_size * 3 / 2,
                                            (u32)POWERFS_INLINE_MAX_SIZE);
                /* 注意: 用 >= 而非 >, 否则当 inline_max_size == POWERFS_INLINE_MAX_SIZE
                 * (如 8192) 时, migrate_threshold=8192, inline_len 最大也是 8192,
                 * 条件 inline_len > 8192 永远为 false, 迁移不会触发. */
                if (pi->inline_len >= migrate_threshold) {
                    int mig_ret = powerfs_migrate_inline_to_flat(inode, pi);
                    if (mig_ret < 0) {
                        pr_warn("powerfs: WB_END INLINE ino=%lu migrate failed: %d\n",
                                inode->i_ino, mig_ret);
                        folio_unlock(folio);
                        folio_put(folio);
                        if (got_caps)
                            powerfs_cap_put_refs(pi, got_caps);
                        return mig_ret;
                    }
                    /* 迁移成功: inode 已切换到 Flat.
                     * folio 仍标记 dirty, writeback 会走 Flat 路径写 Volume Server.
                     * close 时 UPDATE_INODE_SIZE_CHUNKS 同步 size+chunks 到 Filer. */
                }
            }
        }
    }

inline_done:
out:
    /* 与 write_begin 中 try_get_caps 对称, 释放 cap 引用.
     * got_caps 为 0 时直接跳过 (write_begin 中 !iocb 或 need 不满足时). */
    if (got_caps)
        powerfs_cap_put_refs(pi, got_caps);
    folio_unlock(folio);
    folio_put(folio);

    return copied;
}

/*
 * powerfs_bmap - 逻辑块到物理块映射
 *
 * 对于网络文件系统，通常不需要实现或返回 0
 */
sector_t powerfs_bmap(struct address_space *mapping, sector_t block)
{
    return 0;
}

/*
 * powerfs_direct_IO - O_DIRECT 回调 (fallback to buffered I/O)
 *
 * PowerFS 是基于 netfs/page cache 的网络文件系统, 不支持真正的 direct I/O
 * (DMA 直传用户缓冲区). 但很多应用 (如 LTP read02 测试) 会用 O_DIRECT 打开
 * 文件. 在内核 6.17 中, do_dentry_open() 检查:
 *   if ((f->f_flags & O_DIRECT) && !(f->f_mode & FMODE_CAN_ODIRECT))
 *       return -EINVAL;
 * FMODE_CAN_ODIRECT 仅在 a_ops->direct_IO 非 NULL 时设置 (fs/open.c:978).
 *
 * 若不提供 direct_IO 回调, open(O_DIRECT) 会返回 EINVAL, 导致依赖 O_DIRECT
 * 的测试和应用无法运行.
 *
 * 返回 0 的语义: generic_file_read_iter() 中, direct_IO 返回 0 表示
 * "0 bytes transferred", 随后 fallthrough 到 filemap_read() (buffered I/O):
 *   retval = mapping->a_ops->direct_IO(iocb, iter);  // = 0
 *   if (retval >= 0) { iocb->ki_pos += retval; count -= retval; }  // no-op
 *   if (retval < 0 || !count || IS_DAX(inode)) return retval;     // false
 *   if (iocb->ki_pos >= i_size_read(inode)) return retval;        // false
 *   return filemap_read(iocb, iter, retval);  // ← buffered read
 *
 * 这与 ramfs/tmpfs 不同 (它们不支持 O_DIRECT open), 但与 btrfs 压缩文件
 * 短读 fallback 行为一致 (mm/filemap.c:2898 注释).
 */
static ssize_t powerfs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
    return 0;
}

/* 地址空间操作表 - 内核 6.2 netfs 风格
 *
 * .dirty_folio 必须设置: folio_mark_dirty() 内部通过
 * mapping->a_ops->dirty_folio() 间接调用 (mm/page-writeback.c),
 * 若未设置则为 NULL, write_end 标脏页时触发 NULL instruction fetch oops.
 * 参考 fs/nfs/file.c, fs/btrfs/inode.c, fs/xxx/addr.c (均设置 dirty_folio).
 * 我们不使用 buffer_heads, 故用 filemap_dirty_folio (与 nfs/btrfs/zonefs 一致)
 *   外加 PowerFS cap 封装: 脏页同步标 i_dirty_caps 的 CAP_WR_DATA.
 *
 * .invalidate_folio / .release_folio 对接 netfs 子系统通用实现:
 *   truncate/invalidate/shrinker 触发时需清理 netfs 私有资源 (struct netfs_folio,
 *   缓存列表, subrequest). 若缺失, 内存会泄漏, 且截断后读回 stale 数据.
 */

/*
 * PowerFS custom dirty_folio: 页面被标记 dirty 时, 同步标记 pi->i_dirty_caps 的
 * CAP_WR_DATA. 如果 PG_dirty 被设置但 i_dirty_caps 没有 CAP_WR_DATA,
 * 后续 cap_recall → WR_DATA 不在 flushing mask → 跳过写回 → recall_ack 提前
 * 返回, 服务端授 EXCLUSIVE 给新 writer 后其 read_folio 拉到 Filer 旧版 0 填充页.
 *
 * 对齐  xxx_dirty_folio (addr.c L80-L133): 先 VFS 标准 PG_dirty +
 * accounting, 再追加 FS 侧 cap/wrbuffer 跟踪. PowerFS 没有 snap context,
 * 只需追加 dirty_caps 标记.
 */
static bool powerfs_dirty_folio(struct address_space *mapping, struct folio *folio)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi;
    bool ret;

    /* Step 1: VFS 标准 dirty 路径.
     *   __folio_mark_dirty → 增加 NR_FILE_DIRTY accounting, 挂 wb list, memcg 记账 */
    ret = filemap_dirty_folio(mapping, folio);

    /* Step 2: 追加 PowerFS cap dirty_caps 跟踪.
     * 跳过条件: inode NULL (匿名 mapping), folio 最终未脏, inode shutdown. */
    if (unlikely(!inode || !folio_test_dirty(folio)))
        return ret;
    pi = POWERFS_I(inode);
    if (unlikely(!pi || pi->shutdown))
        return ret;

    /* 标 CAP_WR_DATA: 只要有一页被标 dirty, recall 时就要走 filemap writeback
     * 把整个 inode 的脏页写回. 粒度粗但安全; write_end 里也会重复标位
     * (idempotent), 不会有副作用. */
    spin_lock(&pi->i_lock);
    pi->i_dirty_caps |= POWERFS_CAP_WR_DATA;
    spin_unlock(&pi->i_lock);

    return ret;
}

static const struct address_space_operations powerfs_aops = {
    .read_folio       = netfs_read_folio,     /* Stage C: netfs 子系统管理 folio 生命周期 */
    .readahead        = netfs_readahead,      /* Stage C: 批量预读 */
    .writepages       = powerfs_writepages,   /* 批量 writeback (6.17: 无 writepage 回调) */
    .write_begin      = powerfs_write_begin,
    .write_end        = powerfs_write_end,
    .dirty_folio      = powerfs_dirty_folio,  /* P0-7 fix: 脏页同步标 CAP_WR_DATA dirty */
    .invalidate_folio = netfs_invalidate_folio, /* P0-6 fix: truncate/invalidate 清理 netfs folio 资源 */
    .release_folio    = netfs_release_folio,    /* P0-6 fix: shrinker 回收前释放 netfs 资源 */
    .direct_IO        = powerfs_direct_IO,    /* O_DIRECT fallback to buffered I/O */
    .bmap             = powerfs_bmap,
    .migrate_folio    = filemap_migrate_folio, /* P1-4: 支持 NUMA 页迁移 / memory hotplug / CRIU */
};

/* ========== P2-7: Quota enforcement ========== */

/*
 * powerfs_quota_check_max_files - 检查文件数配额.
 *
 * 对齐  xxx_quota_is_max_files_exceeded (quota.c):
 *   向上遍历父目录链, 任一祖先有 i_max_files 且 rfiles >= i_max_files → 超限.
 *
 * PowerFS 简化: 只检查直接父目录的 i_max_files + i_rfiles (递归统计).
 * 完整实现需要 Filer 侧 UpdateChildSummary 增量聚合到祖先链,
 * 当前 rfiles 在 getattr 时从 Filer 拉取 (rstat 字段 0xCE).
 *
 * 返回 -EDQUOT 超限, 0 允许.
 */
static int powerfs_quota_check_max_files(struct inode *dir)
{
    struct powerfs_inode_info *pi;

    if (!dir || !S_ISDIR(dir->i_mode))
        return 0;

    pi = POWERFS_I(dir);
    if (pi->i_max_files == 0)
        return 0;

    if (pi->i_rfiles >= pi->i_max_files) {
        pr_debug("powerfs: quota files exceeded ino=%lu rfiles=%llu max=%llu\n",
                 dir->i_ino, pi->i_rfiles, pi->i_max_files);
        return -EDQUOT;
    }
    return 0;
}

/*
 * powerfs_quota_check_max_bytes - 检查字节配额.
 *
 * 对齐  xxx_quota_is_max_bytes_exceeded (quota.c):
 *   newlen (写入后 i_size) 超过 i_max_bytes → 超限.
 *
 * 返回 -EDQUOT 超限, 0 允许.
 */
static int powerfs_quota_check_max_bytes(struct inode *inode, loff_t newlen)
{
    struct powerfs_inode_info *pi;

    if (!inode)
        return 0;

    /* 只检查目录的 quota (文件继承父目录配额) */
    if (!S_ISDIR(inode->i_mode)) {
        /* 对文件: 检查父目录的 byte quota */
        struct dentry *de = d_find_alias(inode);
        if (de && de->d_parent) {
            struct inode *parent = d_inode(de->d_parent);
            if (parent) {
                int ret = powerfs_quota_check_max_bytes(parent, newlen);
                dput(de);
                return ret;
            }
        }
        if (de)
            dput(de);
        return 0;
    }

    pi = POWERFS_I(inode);
    if (pi->i_max_bytes == 0)
        return 0;

    if (newlen > (loff_t)pi->i_max_bytes) {
        pr_debug("powerfs: quota bytes exceeded ino=%lu newlen=%lld max=%llu\n",
                 inode->i_ino, newlen, pi->i_max_bytes);
        return -EDQUOT;
    }
    return 0;
}

/* ========== statfs ========== */

int powerfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    pr_debug("powerfs: statfs\n");
    return powerfs_net_statfs(buf);
}

/* ========== 文件操作 ========== */

/*
 * powerfs_fsync - 数据同步操作
 *
 * Flat/Stripe: file_write_and_wait_range 触发 writepage→powerfs_net_write 将脏页刷到
 *   volume server, 然后调用 powerfs_net_setattr 同步 i_size.
 * Inline (K2-5): 脏页已在 write_end 同步到 inline_data, fsync 时若 inline_dirty
 *   则通过 UPDATE_INODE 将 inline_data 提交到 Filer (数据+元数据 Raft 强一致).
 *
 * 断连时 writepage 和 setattr/update 返回 -ENOTCONN, fsync 传播错误.
 */
static int powerfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = 0;
    int ret, cap_err;
    loff_t i_size;

    pr_debug("powerfs: fsync ino=%lu start=%llu end=%llu datasync=%d i_size=%lld\n",
            inode->i_ino, start, end, datasync, i_size_read(inode));

    /* 对齐  fsync 前置: 拿 FILE_WR + AUTH_EXCL 引用, flush dirty_caps.
     * 写回脏页前先确保服务端知道我们有脏态 (revoke 阻塞到 flush ACK). */
    cap_err = powerfs_get_caps(inode, file,
                               POWERFS_CAP_FILE_WR,
                               POWERFS_CAP_WR_DATA | POWERFS_CAP_AUTH_EXCL,
                               end, &got);
    if (cap_err < 0) {
        pr_debug("powerfs: fsync get_caps WR ino=%lu ret=%d, sync anyway\n",
                 inode->i_ino, cap_err);
        got = 0;
    }
    /* 有 dirty_caps 时先触发 cap_flush — 把 dirty→flushing 状态机推前,
     * 服务端 revoke 可感知到 flush-on-progress 避免超时硬踢. */
    if (got & POWERFS_CAP_WR_DATA)
        (void)powerfs_cap_flush(pi, POWERFS_CAP_ANY_DIRTY);

    /* 触发脏页写回 (Flat: writepage→powerfs_net_write; Inline: 仅清脏标) */
    ret = file_write_and_wait_range(file, start, end);
    if (ret < 0) {
        pr_warn("powerfs: fsync write_and_wait error: %d\n", ret);
        goto out_put;
    }

    /* K2-5: Inline 模式 — 通过 UPDATE_INODE 同步 inline_data 到 Filer.
     * 复用 release 路径逻辑: 快照 inline_data → 锁外网络 I/O → 清 dirty.
     * fsync 不做重试 (由调用方决定是否重试), 单次同步失败返回错误. */
    if (pi->placement == POWERFS_PLACEMENT_INLINE && pi->inline_dirty) {
        u8 *snap_data;
        u32 snap_len;
        u64 shard_id;

        spin_lock(&pi->i_lock);
        if (!pi->inline_data || pi->inline_len == 0) {
            pi->inline_dirty = false;
            spin_unlock(&pi->i_lock);
            goto out_put;
        }
        snap_len = pi->inline_len;
        spin_unlock(&pi->i_lock);

        snap_data = kmalloc(snap_len, GFP_KERNEL);
        if (!snap_data) {
            ret = -ENOMEM;
            goto out_put;
        }

        spin_lock(&pi->i_lock);
        if (pi->inline_data && pi->inline_len == snap_len) {
            memcpy(snap_data, pi->inline_data, snap_len);
        } else {
            spin_unlock(&pi->i_lock);
            kfree(snap_data);
            goto out_put;
        }
        spin_unlock(&pi->i_lock);

        shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : inode->i_ino);
        ret = powerfs_net_update_inode_size_chunks(shard_id, inode->i_ino,
                                                    (__u64)snap_len,
                                                    "kernel",
                                                    NULL, 0,
                                                    snap_data, snap_len);
        kfree(snap_data);
        if (ret < 0) {
            pr_warn("powerfs: fsync INLINE ino=%lu update failed: %d\n",
                    inode->i_ino, ret);
            goto out_put;
        }
        /* 成功则清 dirty */
        spin_lock(&pi->i_lock);
        pi->inline_dirty = false;
        spin_unlock(&pi->i_lock);
        goto out_put;
    }

    /* Flat/Stripe: 同步 i_size 到 Filer */
    i_size = i_size_read(inode);
    pr_debug("powerfs: fsync after writeback i_size=%lld\n", i_size);
    if (i_size > 0) {
        int sret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                                        0, 0, 0, (__u64)i_size, 0, 0);
        if (sret < 0) {
            pr_warn("powerfs: fsync setattr size=%llu ino=%lu failed: %d\n",
                    (u64)i_size, inode->i_ino, sret);
            ret = sret;
            goto out_put;
        }
    }

    ret = 0;
out_put:
    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}

/*
 * Phase 2: 流控准入 wrapper — 在 file_operations 层排队等待.
 *
 * 不能在 address_space_operations (write_begin/read_folio/writepages) 里 wait:
 *   - write_begin 持有 inode->i_rwsem 写锁
 *   - read_folio 持有 folio lock
 *   - writepages 持有 folio lock (writeback 线程内)
 *
 * file_operations 层 (read_iter/write_iter) 不持有 page lock,
 * generic_file_write_iter 内部才获取 i_rwsem, 此时 admit_wait 已返回.
 */
static ssize_t powerfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file->f_inode;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = 0;
    int err;
    ssize_t ret;
    ktime_t __metric_start = ktime_get();

    powerfs_flow_admit_wait(POWERFS_FLOW_OP_READ, 2000);

    /* 对齐  filemap_fault (addr.c L1982):
     *   need = FILE_SHARED (读必须位), want = FILE_SHARED|FILE_CACHE
     * 拿到 cap 保证 page cache 一致性视图, 服务端可 recall 撤销. */
    err = powerfs_get_caps(inode, file,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_RDCACHE,
                           -1, &got);
    if (err < 0) {
        /* 降级: 无 cap 仍允许读 (与 write_iter lease 策略一致),
         * 服务端 recall 时通过 page_mkwrite 路径再校验. */
        pr_debug("powerfs: read_iter get_caps RD ino=%lu ret=%d, downgrade\n",
                 inode->i_ino, err);
        got = 0;
    }

    ret = generic_file_read_iter(iocb, to);

    if (got)
        powerfs_cap_put_refs(pi, got);

    if (POWERFS_SB_INFO(inode->i_sb)->client)
        powerfs_update_read_metrics(&POWERFS_SB_INFO(inode->i_sb)->client->metrics,
                                    __metric_start, ktime_get(),
                                    ret > 0 ? (unsigned int)ret : 0, (int)ret);
    return ret;
}

/*
 * powerfs_splice_read - splice_read 带 cap ref 管理.
 *
 * 对齐  xxx_splice_read (file.c L2259):
 *   1. 拿 FILE_SHARED (need) + FILE_CACHE (want) cap refs
 *   2. 有 CACHE cap → filemap_splice_read (零拷贝 page cache → pipe)
 *   3. 无 CACHE cap → copy_splice_read (降级: 用户态拷贝)
 *   4. 放 cap refs
 *
 * 之前直接注册 filemap_splice_read 不持 cap ref，splice 期间 cap recall
 * 可能回收 page cache 导致数据不一致。
 */
static ssize_t powerfs_splice_read(struct file *in, loff_t *ppos,
                                    struct pipe_inode_info *pipe,
                                    size_t len, unsigned int flags)
{
    struct inode *inode = file_inode(in);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = 0;
    int err;
    ssize_t ret;

    /* umount 期间跳过网络同步 */
    if (powerfs_net_is_stopping())
        return -ENOTCONN;

    /* 拿 cap refs: need=FILE_SHARED (读必须), want=FILE_CACHE (page cache 信任) */
    err = powerfs_get_caps(inode, in,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_FILE_CACHE,
                           -1, &got);
    if (err < 0) {
        pr_debug("powerfs: splice_read get_caps ino=%lu ret=%d, fallback copy\n",
                 inode->i_ino, err);
        got = 0;
    }

    /* 有 CACHE cap → filemap_splice_read (零拷贝);
     * 无 CACHE cap → copy_splice_read (用户态拷贝降级) */
    if (got & POWERFS_CAP_FILE_CACHE)
        ret = filemap_splice_read(in, ppos, pipe, len, flags);
    else
        ret = copy_splice_read(in, ppos, pipe, len, flags);

    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}

static ssize_t powerfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file->f_inode;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t offset = iocb->ki_pos;
    size_t count = iov_iter_count(from);
    unsigned int got = 0;
    int err;
    ssize_t ret;
    ktime_t __metric_start = ktime_get();

    powerfs_flow_admit_wait(POWERFS_FLOW_OP_WRITE, 2000);

    /* 对齐  page_mkwrite (addr.c L2087):
     *   need = FILE_WR, want = FILE_WR|FILE_EXCL (BUFFER).
     * write_begin 内部还会 try_get_caps 作为快速路径, 这里在 i_rwsem 外
     * 提前阻塞获取, 避免 write_begin 内循环 EAGAIN 重试开销. */
    if (S_ISREG(inode->i_mode) && count > 0) {
        err = powerfs_get_caps(inode, file,
                               POWERFS_CAP_FILE_WR,
                               POWERFS_CAP_WR_DATA,
                               offset + (loff_t)count, &got);
        if (err < 0) {
            pr_debug("powerfs: write_iter get_caps WR ino=%lu ret=%d, write_begin will retry\n",
                     inode->i_ino, err);
            got = 0;
        }
    }

    /* 预先获取 lease (同步网络调用, 用户进程上下文可阻塞). */
    if (pi->placement != POWERFS_PLACEMENT_INLINE &&
        S_ISREG(inode->i_mode) && count > 0) {
        int lease_ret = ensure_lease(inode, offset);
        if (lease_ret && lease_ret != -ENOMEM)
            pr_debug("powerfs: write_iter ensure_lease ino=%lu off=%lld ret=%d, continuing without lease\n",
                     inode->i_ino, offset, lease_ret);
    }

    ret = generic_file_write_iter(iocb, from);

    /* 写入成功后标记 cap WR dirty (对齐 __xxx_mark_dirty_caps 在 write_end),
     * 供 revoke/flush 时感知有脏数据需要同步回服务端. */
    if (ret > 0)
        powerfs_cap_mark_dirty(pi, POWERFS_CAP_WR_DATA);

    if (got)
        powerfs_cap_put_refs(pi, got);

    if (POWERFS_SB_INFO(inode->i_sb)->client)
        powerfs_update_write_metrics(&POWERFS_SB_INFO(inode->i_sb)->client->metrics,
                                     __metric_start, ktime_get(),
                                     ret > 0 ? (unsigned int)ret : 0, (int)ret);
    return ret;
}

/*
 * powerfs_file_release - 文件关闭 (最后一个 fd 释放时调用)
 *
 * K2-5: Inline 模式下, 若 inline_dirty 为 true, 通过 UPDATE_INODE 将
 * inline_data 提交到 Filer (单次 Raft 提交 = 数据 + 元数据).
 *
 * 对齐 FUSE release inline 路径 (fuse.rs L3988):
 *   - shard_id = shard_map_route(parent_ino) (Filer 路由, 对齐 FUSE ShardMap)
 *   - chunks 为空 (Inline 不走 Volume Server)
 *   - size = inline_len
 *   - 5 次重试, 500ms×attempt 退避 (覆盖 Raft 选举窗口)
 *
 * 并发保护: 持 i_lock 快照 inline_data 指针/长度, 锁外做网络 I/O.
 * 网络成功后持锁清 inline_dirty.
 *
 * 注意: VFS 忽略 release 返回值, 故即使同步失败也返回 0 (仅告警).
 *       数据丢失风险由 fsck 兜底 (与 FUSE 一致).
 *
 * 局限: mmap 写入的脏页未同步到 inline_data (writepages 对 Inline 仅清脏标),
 *       当前仅支持 write_iter 路径的 Inline 持久化.
 */
static int powerfs_file_release(struct inode *inode, struct file *file)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);
    u8 *snap_data = NULL;
    u32 snap_len = 0;
    u64 shard_id;
    u64 ino = inode->i_ino;
    int attempt;
    int ret = 0;
    bool synced = false;

    /* P3-5: 统计 opened_files (与 file_open 对称) */
    if (sbi && sbi->client)
        atomic64_dec(&sbi->client->metrics.opened_files);

    /* Cap 引用释放 (与 powerfs_file_open 对称).
     * 对齐  __xxx_put_cap_refs: 递减 i_nr_by_mode + cap_put_refs.
     * 必须在 release 逻辑前执行, 避免 flush 脏数据时 cap 已失效. */
    {
        unsigned int had = POWERFS_CAP_PIN;

        spin_lock(&pi->i_lock);
        if (file->f_mode & FMODE_READ) {
            if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] > 0)
                pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]--;
            had |= POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE;
        }
        if (file->f_mode & FMODE_WRITE) {
            if (pi->i_nr_by_mode[POWERFS_FILE_MODE_WR] > 0)
                pi->i_nr_by_mode[POWERFS_FILE_MODE_WR]--;
            had |= POWERFS_CAP_FILE_WR;
        }
        spin_unlock(&pi->i_lock);

        powerfs_cap_put_refs(pi, had);
    }

    /* §13.4 场景 3: 主动 release cap → 发送 CapRelease RPC.
     * 必须在 cap_put_refs 之后执行 (确保本 filp 的 refcount 已释放),
     * 但又要早于 inline/chunks sync (因为 Filer 端 release 会释放对该
     * 客户端 cap 的持有, inline sync 仍是普通 update_inode RPC, 不依赖 cap).
     * 若 HasUpgrade=1 (自己是 survivor), cap_send_release 内部会调
     * cap_issue 升级 issued 位 (从 SHARED_WRITE 恢复到 EXCLUSIVE_WRITE). */
    {
        struct powerfs_cap *auth_cap;
        spin_lock(&pi->i_lock);
        auth_cap = pi->i_auth_cap;
        spin_unlock(&pi->i_lock);
        if (auth_cap) {
            /* last release (所有 RD/WR refcount 归零) 时才发 release RPC.
             * 避免多 fd 并发 open/close 时每次 close 都触发一次 RPC 风暴.
             * i_nr_by_mode[RD]+i_nr_by_mode[WR] == 0 → 这是最后一个 fd. */
            spin_lock(&pi->i_lock);
            if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] == 0 &&
                pi->i_nr_by_mode[POWERFS_FILE_MODE_WR] == 0) {
                spin_unlock(&pi->i_lock);
                cap_send_release(pi, auth_cap);
            } else {
                spin_unlock(&pi->i_lock);
            }
        }
    }

    /* Inline 模式 + dirty: 同步 inline_data 到 Filer (K2-5).
     * Flat 模式: 同步 size+chunks 到 Filer (对齐 FUSE sync_size_chunks_on_close).
     *   不同步的话 Filer 端 chunks 为空, remount/lookup 后 read locate 失败 (-EINVAL). */
    if (pi->placement == POWERFS_PLACEMENT_INLINE && pi->inline_dirty) {
    pr_info("powerfs: RELEASE INLINE ino=%lu dirty=%d inline_len=%u\n",
            ino, pi->inline_dirty ? 1 : 0, pi->inline_len);

    /* 持锁快照 inline_data (网络 I/O 不能持 spinlock) */
    spin_lock(&pi->i_lock);
    if (!pi->inline_data || pi->inline_len == 0) {
        /* dirty 标记但无数据 — 异常状态, 清标志避免反复重试 */
        pi->inline_dirty = false;
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: RELEASE INLINE ino=%lu dirty but no inline_data\n", ino);
        return 0;
    }
    snap_len = pi->inline_len;
    spin_unlock(&pi->i_lock);

    /* 锁外分配快照缓冲并拷贝 */
    snap_data = kmalloc(snap_len, GFP_KERNEL);
    if (!snap_data) {
        pr_warn("powerfs: RELEASE INLINE ino=%lu kmalloc %u failed, data may be lost\n",
                ino, snap_len);
        return 0;
    }
    spin_lock(&pi->i_lock);
    if (pi->inline_data && pi->inline_len == snap_len) {
        memcpy(snap_data, pi->inline_data, snap_len);
    } else {
        /* 并发修改了 inline_data, 放弃本次同步 */
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: RELEASE INLINE ino=%lu inline_data changed during snapshot\n", ino);
        kfree(snap_data);
        return 0;
    }
    spin_unlock(&pi->i_lock);

    /* K2: 调试 — 输出 snap_data 的 checksum, 与 issue_read 的 csum 比较 */
    {
        __u32 i, csum = 0;
        for (i = 0; i < snap_len; i++)
            csum += snap_data[i];
        pr_info("powerfs: RELEASE INLINE ino=%lu snap_len=%u csum=%u first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                ino, snap_len, csum,
                snap_len > 0 ? snap_data[0] : 0, snap_len > 1 ? snap_data[1] : 0,
                snap_len > 2 ? snap_data[2] : 0, snap_len > 3 ? snap_data[3] : 0,
                snap_len > 4 ? snap_data[4] : 0, snap_len > 5 ? snap_data[5] : 0,
                snap_len > 6 ? snap_data[6] : 0, snap_len > 7 ? snap_data[7] : 0);
    }

    shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);

    /* 5 次重试, 500ms×attempt 退避 (对齐 FUSE, 覆盖 Raft 选举窗口 ~3s) */
    for (attempt = 1; attempt <= 5; attempt++) {
        ret = powerfs_net_update_inode_size_chunks(shard_id, ino,
                                                    (__u64)snap_len,
                                                    "kernel",
                                                    NULL, 0,
                                                    snap_data, snap_len);
        if (ret == 0) {
            synced = true;
            pr_info("powerfs: RELEASE INLINE ino=%lu synced size=%u (attempt %d)\n",
                     ino, snap_len, attempt);
            break;
        }
        pr_warn("powerfs: RELEASE INLINE ino=%lu attempt %d failed: %d\n",
                ino, attempt, ret);
        if (attempt < 5)
            msleep(500 * attempt);
    }

    /* 成功则清 dirty; 失败保留 dirty (evict_inode 时 kfree, 数据丢失) */
    if (synced) {
        spin_lock(&pi->i_lock);
        pi->inline_dirty = false;
        spin_unlock(&pi->i_lock);
    } else {
        pr_err("powerfs: RELEASE INLINE ino=%lu FAILED after 5 attempts: %d — data may be lost\n",
               ino, ret);
    }

    kfree(snap_data);
    return 0;
    } /* end Inline release */

    /* Flat 模式: close 时同步 size+chunks 到 Filer.
     * 对齐 FUSE sync_size_chunks_on_close (fuse.rs L990).
     * writeback 只刷数据到 Volume Server, 不同步 chunks 到 Filer.
     * 若不同步, Filer 端 chunks 为空, remount/lookup 后 read locate 失败.
     *
     * 性能修复: close 时先 flush 脏页到 Volume Server (filemap_write_and_wait_range),
     * 确保数据在 lease 有效期内写入. 否则 lease 过期后 writeback 无 lease 写入,
     * 每个 writepage 需 5s (实测 780MB → 65min), 严重影响 sync 性能.
     * file_release 在用户进程上下文调用, 可安全阻塞. */
    if (pi->placement == POWERFS_PLACEMENT_FLAT && pi->volume_id && pi->file_key) {
        loff_t i_size = i_size_read(inode);
        u64 shard_id;
        u32 chunk_size = pi->layout_chunk_size ? pi->layout_chunk_size : POWERFS_CHUNK_SIZE;
        u32 chunk_count;
        struct powerfs_chunk_map *chunks = NULL;
        u32 i;
        int flush_ret;

        if (i_size == 0) {
            pr_debug("powerfs: RELEASE FLAT ino=%lu size=0, skip\n", ino);
            return 0;
        }

        /* Flush 脏页到 Volume Server (在 lease 过期前完成数据写入).
         * ensure_lease 刷新 stripe 0 的 lease (快速路径: 已持有则立即返回).
         * filemap_write_and_wait_range 触发 writeback, 此时 lease 仍有效,
         * powerfs_get_lease_token 能找到 lease token, 写入快速完成. */
        ensure_lease(inode, 0);
        pr_info("powerfs: RELEASE FLAT ino=%lu pre-flush i_size=%llu\n", ino, (u64)i_size);
        flush_ret = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
        if (flush_ret)
            pr_warn("powerfs: RELEASE FLAT ino=%lu filemap_write_and_wait_range: %d\n",
                    ino, flush_ret);

        /* 仅在 i_size 变化时同步 (用 content_size 跟踪上次同步值) */
        spin_lock(&pi->i_lock);
        if ((u64)i_size == pi->content_size) {
            spin_unlock(&pi->i_lock);
            pr_debug("powerfs: RELEASE FLAT ino=%lu size=%llu unchanged, skip\n",
                     ino, (u64)i_size);
            return 0;
        }
        spin_unlock(&pi->i_lock);

        chunk_count = (u32)div_u64(i_size + chunk_size - 1, chunk_size);
        /* 限制最大 chunk_count 避免过大分配 (4096 chunks = 4GB @ 1MB chunk) */
        if (chunk_count > 4096) {
            pr_warn("powerfs: RELEASE FLAT ino=%lu chunk_count=%u > 4096, truncating\n",
                    ino, chunk_count);
            chunk_count = 4096;
        }

        chunks = kmalloc_array(chunk_count, sizeof(*chunks), GFP_KERNEL);
        if (!chunks) {
            pr_warn("powerfs: RELEASE FLAT ino=%lu kmalloc %u chunks failed, metadata not synced\n",
                    ino, chunk_count);
            return 0;
        }

        for (i = 0; i < chunk_count; i++) {
            chunks[i].chunk_idx = i;
            chunks[i].needle_id = pi->file_key + i;
            chunks[i].volume_id = pi->volume_id;
            chunks[i].crc32 = 0;
        }

        shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);

        pr_info("powerfs: RELEASE FLAT ino=%lu size=%llu chunks=%u vid=%llu fkey=%llu\n",
                ino, (u64)i_size, chunk_count,
                (unsigned long long)pi->volume_id,
                (unsigned long long)pi->file_key);

        for (attempt = 1; attempt <= 5; attempt++) {
            ret = powerfs_net_update_inode_size_chunks(shard_id, ino,
                                                        (__u64)i_size,
                                                        "kernel",
                                                        chunks, chunk_count,
                                                        NULL, 0);
            if (ret == 0) {
                spin_lock(&pi->i_lock);
                pi->content_size = (u64)i_size;
                spin_unlock(&pi->i_lock);
                pr_info("powerfs: RELEASE FLAT ino=%lu synced size=%llu chunks=%u (attempt %d)\n",
                        ino, (u64)i_size, chunk_count, attempt);
                break;
            }
            pr_warn("powerfs: RELEASE FLAT ino=%lu attempt %d failed: %d\n",
                    ino, attempt, ret);
            if (attempt < 5)
                msleep(500 * attempt);
        }

        kfree(chunks);
        return 0;
    }

    pr_debug("powerfs: RELEASE skip ino=%lu placement=%u dirty=%d\n",
             ino, pi->placement, pi->inline_dirty ? 1 : 0);
    return 0;
}

/* Forward declaration — defined below */
static long powerfs_fallocate(struct file *file, int mode,
                              loff_t offset, loff_t len);

/*
 * powerfs_file_open - 文件打开 cap 接入
 *
 * 对齐  xxx_init_file + __xxx_touch_fmode:
 *   1. 根据 f_mode 记录 i_nr_by_mode[] (RD/WR 计数)
 *   2. 更新 i_last_rd / i_last_wr 时间戳
 *   3. 获取 cap 引用: RD → FILE_SHARED|FILE_CACHE, WR → FILE_WR
 *
 * cap 引用在 release 时通过 powerfs_cap_put_refs 对称释放.
 * 若 Filer 尚未授予足够 cap, powerfs_check_caps 会在后台请求.
 */
static int powerfs_file_open(struct inode *inode, struct file *file)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = POWERFS_CAP_PIN;
    bool is_write_open = !!(file->f_mode & FMODE_WRITE);

    /* §13.3: 先从 Filer 获取授权 CapOpenGrant (永不阻塞),
     * 根据响应挂载 cap + 更新 issued 位. 网络失败自动降级 (不阻断 open).
     * 注意: __block_on 在此处调, 早于 cap_get_refs 拿 refcount, 避免
     * refcount 先占有后授权位不足导致 try_get_caps 循环等. */
    cap_open_grant_and_issue(pi, is_write_open);

    spin_lock(&pi->i_lock);

    if (file->f_mode & FMODE_READ) {
        pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]++;
        pi->i_last_rd = jiffies;
        got |= POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE;
    }
    if (file->f_mode & FMODE_WRITE) {
        pi->i_nr_by_mode[POWERFS_FILE_MODE_WR]++;
        pi->i_last_wr = jiffies;
        got |= POWERFS_CAP_FILE_WR;
    }

    spin_unlock(&pi->i_lock);

    /* 获取 cap 引用 (内部自行加锁) */
    powerfs_cap_get_refs(pi, got);

    /* P3-5: 统计 opened_files */
    {
        struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);
        if (sbi && sbi->client)
            atomic64_inc(&sbi->client->metrics.opened_files);
    }

    pr_debug("powerfs: file_open ino=%lu mode=0x%x got=0x%x write=%d\n",
             inode->i_ino, file->f_mode, got, (int)is_write_open);

    return 0;
}

/* ========== vm_operations (mmap fault/page_mkwrite cap 接入) ==========
 *
 * 对齐  xxx_vmops (addr.c L2333):
 *   .fault        = xxx_filemap_fault   — 读缺页: 拿 FILE_SHARED|FILE_CACHE
 *   .page_mkwrite = xxx_page_mkwrite    — 写缺页: 拿 FILE_WR|FILE_EXCL, mark dirty
 *
 * generic_file_mmap 默认 vm_ops 不走 cap, 导致 mmap 写可以绕过
 * write_begin/page_mkwrite 的校验 (服务端无法 recall 已 mmap 的页).
 */

static vm_fault_t powerfs_filemap_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct file *file = vma->vm_file;
    struct inode *inode = file_inode(file);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t off = (loff_t)vmf->pgoff << PAGE_SHIFT;
    unsigned int got = 0;
    int err;
    vm_fault_t ret;

    /* 对齐  filemap_fault (addr.c L1982):
     *   need = FILE_SHARED (读必须位), want = FILE_SHARED|FILE_CACHE.
     * endoff = -1 表示不限制范围 (整个文件读). */
    err = powerfs_get_caps(inode, file,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_RDCACHE,
                           -1, &got);
    if (err < 0) {
        pr_debug("powerfs: vm_fault get_caps RD ino=%lu off=%lld ret=%d\n",
                 inode->i_ino, off, err);
        got = 0;
    }

    ret = filemap_fault(vmf);

    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}

static vm_fault_t powerfs_page_mkwrite(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct file *file = vma->vm_file;
    struct inode *inode = file_inode(file);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct folio *folio = page_folio(vmf->page);
    loff_t off = folio_pos(folio);
    loff_t size = i_size_read(inode);
    loff_t endoff;
    size_t len;
    unsigned int got = 0;
    int err;
    vm_fault_t ret = VM_FAULT_SIGBUS;

    if (off + (loff_t)folio_size(folio) <= size)
        len = folio_size(folio);
    else
        len = (size_t)((long long)size - (long long)off);
    endoff = off + (loff_t)len;

    /* 对齐  page_mkwrite (addr.c L2087):
     *   need = FILE_WR, want = FILE_WR|FILE_EXCL.
     * page_mkwrite 持有 folio lock, 但 get_caps 不依赖 page lock,
     * 阻塞在 i_cap_wq (服务端 grant). */
    err = powerfs_get_caps(inode, file,
                           POWERFS_CAP_FILE_WR,
                           POWERFS_CAP_WR_DATA,
                           endoff, &got);
    if (err < 0) {
        pr_debug("powerfs: page_mkwrite get_caps WR ino=%lu off=%lld ret=%d\n",
                 inode->i_ino, off, err);
        goto out_nocaps;
    }

    /* 更新 mtime/iversion — 与 generic_permission / write_end 时间语义对齐 */
    file_update_time(file);
    inode_inc_iversion_raw(inode);

    /*  这里会 wait_on_page_writeback, 通用 filemap_fault 已处理.
     * lock_page 由上层 do_page_mkwrite 持有 (进入回调时 page 已 locked). */

    ret = VM_FAULT_LOCKED;   /* 保持 folio locked */

    /* 成功写缺页: 标记 WR_DATA dirty (写回需要同步).
     * mark_inode_dirty 由 set_page_dirty 在 writepage 前置位触发. */
    powerfs_cap_mark_dirty(pi, POWERFS_CAP_WR_DATA);

    powerfs_cap_put_refs(pi, got);
    return ret;

out_nocaps:
    if (got)
        powerfs_cap_put_refs(pi, got);
    return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct powerfs_file_vmops = {
    .fault         = powerfs_filemap_fault,
    .page_mkwrite  = powerfs_page_mkwrite,
};

/*
 * powerfs_mmap - 自定义 mmap, 注入 vm_ops (cap 一致性)
 *
 * 保留 .mmap 路径用于内核 < 6.17 兼容 (若 mmap_prepare 未注册则 VFS
 * fallback 到 .mmap). 6.17 优先走 .mmap_prepare.
 */
static int powerfs_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct address_space *mapping = file->f_mapping;
    int ret;

    if (!mapping->a_ops->read_folio)
        return -ENOEXEC;

    ret = generic_file_mmap(file, vma);
    if (ret == 0)
        vma->vm_ops = &powerfs_file_vmops;
    return ret;
}

/*
 * powerfs_mmap_prepare - P2-6: 6.17 新接口, 在 sys_mmap 时提前设置 vm_ops.
 *
 * 对齐  xxx_mmap_prepare (addr.c L2338):
 *   1. 检查 address_space 有 read_folio (正则文件)
 *   2. 设置 desc->vm_ops = &powerfs_file_vmops (fault/page_mkwrite 带 cap)
 *
 * 相比旧 .mmap 路径: mmap_prepare 在 VFS 分配 vma 前调用,
 * 可以提前拿到 cap, 避免 page fault 时才发现 cap miss 导致竞态.
 * msync(MS_INVALIDATE) 时 page_mkwrite 持有 cap ref, 不会被 cap_recall
 * 中途回收 page cache, 修复 mmap + msync 写数据丢失竞态.
 */
static int powerfs_mmap_prepare(struct vm_area_desc *desc)
{
    struct address_space *mapping = desc->file->f_mapping;

    if (!mapping->a_ops->read_folio)
        return -ENOEXEC;

    desc->vm_ops = &powerfs_file_vmops;
    return 0;
}

/*
 * powerfs_copy_file_range - P2-5: 文件间复制 (sendfile/copy_file_range syscall).
 *
 * 对齐  xxx_copy_file_range (file.c L3148):
 *   1. 同 fs 内: 尝试 splice_copy_file_range (page cache splice, 零用户态拷贝)
 *   2. 跨 fs 或不支持: fallback generic_copy_file_range (用户态 read/write)
 *
 * PowerFS 当前没有服务端 OSD offload ( RADOS copy-from)，但 splice
 * 路径已能在 page cache 层做高效复制，避免用户态往返拷贝。
 *
 * cap 管理: src 端拿 FILE_SHARED (读), dst 端拿 FILE_WR (写),
 * 复制完成后释放 refs。
 */
static ssize_t powerfs_copy_file_range(struct file *src_file, loff_t src_off,
                                        struct file *dst_file, loff_t dst_off,
                                        size_t len, unsigned int flags)
{
    struct inode *src_inode = file_inode(src_file);
    struct inode *dst_inode = file_inode(dst_file);
    struct powerfs_inode_info *src_pi = POWERFS_I(src_inode);
    struct powerfs_inode_info *dst_pi = POWERFS_I(dst_inode);
    unsigned int src_got = 0, dst_got = 0;
    ssize_t ret;

    /* flags must be 0 (no SPLICE_F_MOVE etc) */
    if (flags != 0)
        return -EINVAL;

    /* src 端: 拿读 cap */
    ret = powerfs_get_caps(src_inode, src_file,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_FILE_CACHE,
                           -1, &src_got);
    if (ret < 0)
        src_got = 0;

    /* dst 端: 拿写 cap */
    ret = powerfs_get_caps(dst_inode, dst_file,
                           POWERFS_CAP_FILE_WR,
                           POWERFS_CAP_FILE_WR,
                           -1, &dst_got);
    if (ret < 0)
        dst_got = 0;

    /* 用 splice_copy_file_range (page cache → pipe → page cache).
     * 跨 fs 场景 splice 也能处理 (VFS splice_file_range 内部会做检查). */
    ret = splice_copy_file_range(src_file, src_off, dst_file, dst_off, len);

    if (src_got)
        powerfs_cap_put_refs(src_pi, src_got);
    if (dst_got)
        powerfs_cap_put_refs(dst_pi, dst_got);

    return ret;
}

/* ========== File Locking (POSIX record locks + BSD flock) ==========
 * 设计决策 (同  xxx_lock.c 的初版架构):
 *   先做**单机一致性锁**: 委托 VFS 通用框架 (posix_lock_file / locks_lock_inode_wait)
 *   在内存 inode 内维护锁冲突链表和阻塞队列. 多客户端场景下, 跨节点锁仲裁
 *   需 Filer 侧新增 LOCK/UNLOCK 协议帧, 路线图:
 *     Phase 1 (本地, 当前): 单机多进程/多线程正确互斥. 覆盖 90% 单机使用场景.
 *     Phase 2 (分布式): wrapper 内先本地冲突检测, 再发 Filer LOCK RPC 仲裁.
 *
 * 为什么不返回 -ENOSYS (P0 修复前的行为):
 *   大量应用 (SQLite, PostgreSQL, Maildir, Python fcntl.flock, git) 依赖
 *   fcntl(F_SETLK) / flock() == 0 的成功路径; -ENOSYS 会直接 panic 应用.
 */

/*
 * powerfs_lock — POSIX 记录锁 (fcntl(2) F_SETLK/F_SETLKW/F_GETLK/F_OFD_*).
 *
 * VFS 层传 cmd: F_SETLK (非阻塞), F_SETLKW (阻塞), F_GETLK (查询冲突锁),
 * 以及 OFD 版本 (F_OFD_SETLK etc). struct file_lock *fl 含锁类型/范围/owner.
 * 直接用 VFS 通用 posix_lock_file 实现, 内核已维护冲突检测 + 文件锁阻塞队列
 * (fl->fl_blocked 链表 + wake_up). 此函数 EXPORT_SYMBOL.
 */
static int powerfs_lock(struct file *filp, int cmd, struct file_lock *fl)
{
    struct inode *inode = file_inode(filp);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int err;

    if (unlikely(!pi || pi->shutdown))
        return -ENOTCONN;

    pr_debug("powerfs: lock ino=%lu cmd=%d type=%d pid=%d\n",
             inode->i_ino, cmd, fl->c.flc_type, current->pid);

    /* P0 单机 VFS 通用锁表:
     *   posix_lock_file(file, fl, conflock) ——
     *     [in] file: 目标文件
     *     [in] fl:   新锁请求 (类型/范围/owner)
     *     [out] conflock: GETLK 场景返回首个冲突锁; SET/UNLOCK 传 NULL
     *   内核内部已实现冲突检测、阻塞等待 (SETLKW)、信号唤醒.
     *   此函数 EXPORT_SYMBOL (fs/locks.c:1404). */
    err = posix_lock_file(filp, fl, NULL);

    if (err < 0)
        pr_debug_ratelimited("powerfs: lock ino=%lu failed: %d\n",
                             inode->i_ino, err);
    return err;
}

/*
 * powerfs_flock — BSD 风格 flock(2).
 *
 * VFS 把 LOCK_SH/LOCK_EX/LOCK_UN (+ LOCK_NB) 转换成 file_lock 结构 (fl_type
 * F_RDLCK/F_WRLCK/F_UNLCK, fl_flags |= FL_FLOCK 标记) 后传入.
 * 通用入口 locks_lock_inode_wait 内部按 FL_FLOCK 分派到 flock_lock_inode_wait
 * 处理冲突检测 + 等待. 此函数 EXPORT_SYMBOL.
 */
static int powerfs_flock(struct file *filp, int cmd, struct file_lock *fl)
{
    struct inode *inode = file_inode(filp);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int err;

    if (unlikely(!pi || pi->shutdown))
        return -ENOTCONN;

    /* flock 锁的 fl_flags 需要 FL_FLOCK: VFS 传进来时通常已设置, 但为了
     * 兜底 (lockf 共享路径) 我们显式标记. */
    fl->c.flc_flags |= FL_FLOCK;

    pr_debug("powerfs: flock ino=%lu cmd=%d type=%d pid=%d\n",
             inode->i_ino, cmd, fl->c.flc_type, current->pid);

    err = locks_lock_inode_wait(inode, fl);
    if (err < 0)
        pr_debug_ratelimited("powerfs: flock ino=%lu failed: %d\n",
                             inode->i_ino, err);
    return err;
}

/*
 * powerfs_dir_fsync — 目录 fsync(2).
 *
 * 目录没有数据页, 只需同步元数据 (mode/uid/gid/mtime/ctime/size 等).
 * - write_inode_now: 调 super_ops->write_inode (powerfs_write_inode),
 *   后者最终发 powerfs_net_setattr 推送属性.
 * - cap_flush: 若目录级 XATTR/权限 dirty, 也在此路径推 Filer.
 *
 * 数据库场景 (e.g. SQLite commit) 在事务后通常 fsync(data file) + fsync(dir fd)
 * 确保 rename/create 落盘. 若目录 fsync 为空, crash+恢复后目录条目丢失.
 */
static int powerfs_dir_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int err, ret;

    (void)start; (void)end; (void)datasync;
    if (unlikely(!pi || pi->shutdown))
        return -ENOTCONN;

    pr_debug("powerfs: dir_fsync ino=%lu\n", inode->i_ino);

    /* Step 1: 同步 inode 属性 (mode/mtime etc.) 到后端 (super write_inode).
     * 第二参数 wait=1: 阻塞直到 write_inode 完成 (真正发 net_setattr). */
    err = write_inode_now(inode, 1);

    /* Step 2: 若 i_dirty_caps 有位 (XATTR/AUTH 脏), 同步 cap_flush.
     * 目录一般没 WR_DATA, 但 setfacl/chmod/chown 后 AUTH_EXCL/XATTR_EXCL 会脏. */
    ret = 0;
    if (pi->i_dirty_caps)
        ret = powerfs_cap_flush(pi, pi->i_dirty_caps);

    return err ?: ret;
}

/*
 * P1-3: Linux 6.17 使用 fileattr 子系统替代传统 ATTR_FLAGS/ia_flags.
 *       通过 inode_operations.fileattr_get/fileattr_set 回调 +
 *       vfs_fileattr_get/set 通用实现完成权限/capability 检查和映射.
 *       内部辅助: S_* 内核 i_flags ↔ FS_*_FL 用户态 flags 双向转换.
 *
 * 注意: Linux 6.17 inode->i_flags 不再有 S_NODUMP (nodump 语义改用 FS_NODUMP_FL
 * 通过 fileattr.flags 传递，文件系统自行存储; PowerFS 当前不支持 NODUMP
 * 持久化，GET 返回 0，SET 忽略 -EOPNOTSUPP (拒绝而非静默丢失).
 */

/* 受支持的 FS_*_FL 用户态标志 (子集，对应 S_SYNC/S_IMMUTABLE/S_APPEND/S_NOATIME/S_DIRSYNC) */
#define POWERFS_FS_FL_SUPPORTED_MASK \
    ((unsigned int)(FS_SYNC_FL | FS_IMMUTABLE_FL | FS_APPEND_FL | \
                    FS_NOATIME_FL | FS_DIRSYNC_FL))

/* 内核 i_flags → 用户态 FS_*_FL (单向转换，用于 fileattr_fill_flags) */
static unsigned int powerfs_i_flags_to_fs_fl(unsigned int i_flags)
{
    unsigned int fs_fl = 0;
    if (i_flags & S_SYNC)       fs_fl |= FS_SYNC_FL;
    if (i_flags & S_IMMUTABLE)  fs_fl |= FS_IMMUTABLE_FL;
    if (i_flags & S_APPEND)     fs_fl |= FS_APPEND_FL;
    if (i_flags & S_NOATIME)    fs_fl |= FS_NOATIME_FL;
    if (i_flags & S_DIRSYNC)    fs_fl |= FS_DIRSYNC_FL;
    return fs_fl;
}

/*
 * powerfs_fileattr_get — P1-3 inode_operations.fileattr_get 回调.
 *
 * 由 vfs_fileattr_get (→ ioctl_getflags / ioctl_fsgetxattr) 调用.
 * 只需填充 fa.flags / fa.fsx_* 字段，权限检查由 VFS 侧完成.
 */
static int powerfs_fileattr_get(struct dentry *dentry, struct file_kattr *fa)
{
    struct inode *inode = d_inode(dentry);
    unsigned int i_flags, fs_fl;

    pr_debug("powerfs: fileattr_get ino=%lu\n", inode->i_ino);

    spin_lock(&inode->i_lock);
    i_flags = inode->i_flags;
    spin_unlock(&inode->i_lock);

    fs_fl = powerfs_i_flags_to_fs_fl(i_flags);
    fileattr_fill_flags(fa, fs_fl);
    /* 当前不支持 fsx_* (cowextsize/extsize/projid/xflags)，
     * fileattr_fill_flags 已将 fa.fsx_valid 置 false，ioctl_fsgetxattr 返回全零. */
    return 0;
}

/*
 * powerfs_fileattr_set — P1-3 inode_operations.fileattr_set 回调.
 *
 * 由 vfs_fileattr_set (→ ioctl_setflags / ioctl_fssetxattr) 调用.
 * VFS 已完成:
 *   - inode_lock(inode) 持有
 *   - owner or capable 检查
 *   - immutable/append → CAP_LINUX_IMMUTABLE capable 检查
 *   - fileattr_set_prepare: 校验不支持位并返回 -EOPNOTSUPP
 *   - security_inode_file_setattr lsm hook
 *
 * 文件系统侧职责: 把 fa.flags → S_* 位设置到 inode->i_flags，
 * 标记 inode dirty + 触发 write_inode_now 同步到 Filer Raft.
 */
static int powerfs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry,
                                 struct file_kattr *fa)
{
    struct inode *inode = d_inode(dentry);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int new_fs_fl = 0;
    unsigned int old_i_flags, new_i_flags;
    int ret;

    (void)idmap;
    pr_debug("powerfs: fileattr_set ino=%lu flags_valid=%d fsx_valid=%d\n",
             inode->i_ino, fa->flags_valid, fa->fsx_valid);

    /* --- Step 1: fsx 属性 (cowextsize/extsize/projid/xflags) 当前不支持 ---
     * VFS 已在 fileattr_set_prepare 中检查 FS_XFLAG_COMMON 外的位并返回 -EOPNOTSUPP.
     * 我们再拒绝任何 fsx_valid 请求，避免用户以为设置成功但未持久化. */
    if (fa->fsx_valid && (fa->fsx_xflags || fa->fsx_extsize ||
                          fa->fsx_projid || fa->fsx_cowextsize))
        return -EOPNOTSUPP;

    /* --- Step 2: flags 处理 (chattr +i/+a/+A/+s/+d 等) --- */
    if (fa->flags_valid) {
        /* 超出支持范围的位 (如 NODUMP_FL/COMPR_FL/ENCRYPT_FL 等高级位):
         * 拒绝整个请求，对齐 ext4 语义 — chattr +d 会得 Operation not supported
         * 而非静默失败. */
        if (fa->flags & ~POWERFS_FS_FL_SUPPORTED_MASK) {
            pr_debug("powerfs: fileattr_set unsupported flags: 0x%x (mask 0x%x)\n",
                    fa->flags & ~POWERFS_FS_FL_SUPPORTED_MASK,
                    POWERFS_FS_FL_SUPPORTED_MASK);
            return -EOPNOTSUPP;
        }
        new_fs_fl = fa->flags;
    }

    /* --- Step 3: 转换并更新 inode->i_flags --- */
    spin_lock(&inode->i_lock);
    old_i_flags = inode->i_flags;
    /* 先清除旧的 S_* 可设置位，再按 new_fs_fl 设置 */
    new_i_flags = old_i_flags & ~(S_SYNC | S_IMMUTABLE | S_APPEND | S_NOATIME | S_DIRSYNC);
    if (new_fs_fl & FS_SYNC_FL)       new_i_flags |= S_SYNC;
    if (new_fs_fl & FS_IMMUTABLE_FL)  new_i_flags |= S_IMMUTABLE;
    if (new_fs_fl & FS_APPEND_FL)     new_i_flags |= S_APPEND;
    if (new_fs_fl & FS_NOATIME_FL)    new_i_flags |= S_NOATIME;
    if (new_fs_fl & FS_DIRSYNC_FL)    new_i_flags |= S_DIRSYNC;
    inode->i_flags = new_i_flags;
    spin_unlock(&inode->i_lock);

    if (new_i_flags == old_i_flags) {
        pr_debug("powerfs: fileattr_set no change, skip sync\n");
        return 0;
    }

    /* --- Step 4: 标记 inode 脏态 + AUTH_EXCL cap dirty --- */
    mark_inode_dirty(inode);
    powerfs_cap_mark_dirty(pi, POWERFS_CAP_AUTH_EXCL);

    /* --- Step 5: 同步到 Filer Raft (notify_change → setattr → net_setattr 链条),
     * 但 inode_operations.setattr 路径不会处理 i_flags 修改 (它只处理 ATTR_*),
     * 因此这里直接调 write_inode_now (真正的 write_inode 回调会 Raft flush). */
    ret = write_inode_now(inode, 1);
    if (ret < 0) {
        pr_warn("powerfs: fileattr_set write_inode_now ino=%lu failed: %d\n",
                inode->i_ino, ret);
        return ret;
    }

    pr_debug("powerfs: fileattr_set ino=%lu ok, old_i=0x%x new_i=0x%x fs_fl=0x%x\n",
             inode->i_ino, old_i_flags, new_i_flags, new_fs_fl);
    return 0;
}

/*
 * powerfs_ioctl — P1-3: 基础 ioctl 实现 (替代旧占位).
 *
 * 策略 (Linux 6.17 new fileattr 子系统):
 *   FS_IOC_GETFLAGS/SETFLAGS/FSGETXATTR/FSSETXATTR 直接委托内核通用函数
 *   ioctl_getflags / ioctl_setflags / ioctl_fsgetxattr / ioctl_fssetxattr,
 *   它们会 vfs_fileattr_get/set → 我们的 fileattr_get/set 回调.
 *   FITRIM 仍手动处理 (PowerFS 无块设备 discard).
 *
 * 通用函数返回 -ENOIOCTLCMD 时降级为 -ENOTTY (用户 ENOTTY).
 */
static long powerfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct inode *inode = file_inode(file);
    void __user *argp = (void __user *)arg;
    long ret = -ENOTTY;

    pr_debug("powerfs: ioctl cmd=0x%x ino=%lu\n", cmd, inode->i_ino);

    switch (cmd) {
    /* ================================================================
     * 1. FS_IOC_GETFLAGS / SETFLAGS — chattr(1) / lsattr(1) 走 fileattr
     * ================================================================ */
    case FS_IOC_GETFLAGS:
        ret = ioctl_getflags(file, (unsigned int __user *)argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    case FS_IOC_SETFLAGS:
        ret = ioctl_setflags(file, (unsigned int __user *)argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    /* ================================================================
     * 2. FS_IOC_FSGETXATTR / FSSETXATTR — chattr 2.0 扩展属性
     * ================================================================ */
    case FS_IOC_FSGETXATTR:
        ret = ioctl_fsgetxattr(file, argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    case FS_IOC_FSSETXATTR:
        ret = ioctl_fssetxattr(file, argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    /* ================================================================
     * 3. FITRIM — fstrim -av 空间回收
     *
     * PowerFS 是对象存储架构 (Volume → Needles)，无底层块设备 discard，
     * 返回 -EOPNOTSUPP 让 fstrim 跳过而非报错. 若未来实现 Needle GC，
     * 可在此触发并返回已回收字节数.
     * ================================================================ */
    case FITRIM: {
        struct super_block *sb = inode->i_sb;
        struct fstrim_range range;

        if (!capable(CAP_SYS_ADMIN))
            return -EPERM;
        if (copy_from_user(&range, argp, sizeof(range)))
            return -EFAULT;
        if (range.len < sb->s_blocksize)
            return -EINVAL;
        pr_info_once("powerfs: FITRIM not supported (object storage, no block discard)\n");
        return -EOPNOTSUPP;
    }

    default:
        ret = -ENOTTY;
        break;
    }

    return ret;
}

/*
 * 文件操作表 - 尽可能复用 VFS 通用实现
 *
 * 参考 ramfs_file_operations (fs/ramfs/file-mmu.c)
 */
static const struct file_operations powerfs_file_operations = {
    .open           = powerfs_file_open,
    .read_iter      = powerfs_file_read_iter,
    .write_iter     = powerfs_file_write_iter,
    .mmap           = powerfs_mmap,          /* < 6.17 兼容路径 */
    .mmap_prepare   = powerfs_mmap_prepare,  /* P2-6: 6.17 提前设 vm_ops, 修复 msync 竞态 */
    .release        = powerfs_file_release,
    .fsync          = powerfs_fsync,
    .fallocate      = powerfs_fallocate,
    .lock           = powerfs_lock,       /* P0-1 fix: POSIX record locks (fcntl) */
    .flock          = powerfs_flock,      /* P0-2 fix: BSD flock(2) */
    .unlocked_ioctl = powerfs_ioctl,      /* P1-3 扩展实现 */
    .compat_ioctl   = compat_ptr_ioctl,   /* P1-3 32-bit user on 64-bit kernel */
    .splice_read    = powerfs_splice_read,  /* cap ref 管理 + CACHE/copy 降级 */
    .splice_write   = iter_file_splice_write,
    .llseek         = generic_file_llseek,
    .setlease       = simple_nosetlease,  /* P2-4: 明确拒绝 F_SETLEASE delegations，
                                           * 防止远端写时不触发本地 break，导致 rsync/WAL silent stale */
    .copy_file_range = powerfs_copy_file_range,  /* P2-5: 同 fs splice + 跨 fs generic fallback */
};

/* ========== fallocate 回调 (空间预分配) ==========
 *
 * PowerFS 不预分配物理块 (空间按需分配), fallocate 仅更新 i_size:
 * - mode 0 (默认): 扩展 i_size 到 offset+len, 页面全零
 * - FALLOC_FL_KEEP_SIZE: 不改变 i_size (no-op)
 * - FALLOC_FL_PUNCH_HOLE: 释放页面缓存 (truncate_pagecache_range)
 *
 * 参考: brd_fallocate (ramfs), shmem_fallocate (tmpfs)
 */
static long powerfs_fallocate(struct file *file, int mode,
                              loff_t offset, loff_t len)
{
    struct inode *inode = file_inode(file);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t new_size = offset + len;
    unsigned int got = 0;
    int cap_err, ret;

    /* Only support default, KEEP_SIZE, and PUNCH_HOLE modes */
    if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
        return -EOPNOTSUPP;

    /* PUNCH_HOLE requires KEEP_SIZE */
    if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
        return -EOPNOTSUPP;

    if (offset < 0 || len <= 0)
        return -EINVAL;

    /* 对齐  fallocate: 需持有 FILE_EXCL (改 size/hole) + AUTH_EXCL (改 attrs).
     * i_rwsem 会在 inode_lock 获取, 此处先在锁外阻塞拿 cap, 避免持有锁时网络阻塞. */
    cap_err = powerfs_get_caps(inode, file,
                               POWERFS_CAP_FILE_EXCL,
                               POWERFS_CAP_FILE_EXCL | POWERFS_CAP_AUTH_EXCL,
                               new_size, &got);
    if (cap_err < 0) {
        pr_debug("powerfs: fallocate get_caps EXCL ino=%lu ret=%d, continue\n",
                 inode->i_ino, cap_err);
        got = 0;
    }

    inode_lock(inode);

    if (mode & FALLOC_FL_PUNCH_HOLE) {
        /* PUNCH_HOLE: deallocate data in [offset, new_size), reads as zeros.
         * File size is unchanged (KEEP_SIZE is required for PUNCH_HOLE).
         *
         * O-10: For FLAT files, truncate_pagecache_range alone is
         * insufficient — it only zeroes/removes pagecache pages but
         * does not update the server needle. After pagecache eviction
         * (memory pressure, posix_fadvise, refresh_work invalidation),
         * reads re-fetch the original (non-zero) data from the server.
         *
         * Fix: zero the punched region in pagecache, mark pages dirty,
         * and trigger synchronous writeback so the server needle is
         * updated with zeros. This mirrors the O-03 extend path.
         *
         * For non-FLAT files (INLINE), data lives in inline_data (not
         * the server needle), so truncate_pagecache_range is sufficient.
         */
        if (pi->placement == POWERFS_PLACEMENT_FLAT &&
            pi->volume_id && pi->file_key) {
            loff_t file_size = i_size_read(inode);
            loff_t punch_end = min(new_size, file_size);
            pgoff_t start_pg, end_pg, pg;
            int dirty_count = 0;

            if (offset >= file_size) {
                /* Hole entirely beyond file size — no-op */
                ret = 0;
                goto falloc_done;
            }

            start_pg = offset >> PAGE_SHIFT;
            end_pg = (punch_end - 1) >> PAGE_SHIFT;

            for (pg = start_pg; pg <= end_pg; pg++) {
                struct page *page;
                size_t pg_off = (size_t)pg << PAGE_SHIFT;
                size_t z_start, z_end;
                bool need_read = false;

                /* Calculate zero range within this page.
                 * z_start/z_end are offsets within the page (0..PAGE_SIZE).
                 */
                z_start = (pg_off < (size_t)offset)
                          ? (size_t)(offset - pg_off) : 0;
                z_end = (pg_off + PAGE_SIZE > (size_t)punch_end)
                        ? (size_t)(punch_end - pg_off) : PAGE_SIZE;

                /* If the page is partially outside the hole (first or
                 * last page with non-page-aligned boundaries), we need
                 * to read existing data to preserve the non-punched
                 * portion. Pages fully within the hole can be zeroed
                 * without reading from the server.
                 */
                if (z_start > 0 || z_end < PAGE_SIZE)
                    need_read = true;

                if (need_read) {
                    page = read_mapping_page(inode->i_mapping, pg, NULL);
                    if (IS_ERR(page)) {
                        /* Server read failed — create zero page */
                        page = find_or_create_page(
                            inode->i_mapping, pg, GFP_NOFS);
                        if (!page)
                            continue;
                        zero_user_segment(page, 0, PAGE_SIZE);
                        SetPageUptodate(page);
                    } else {
                        lock_page(page);
                    }
                } else {
                    /* Page is entirely within the hole — create/reuse
                     * zero page without reading from server.
                     */
                    page = find_or_create_page(
                        inode->i_mapping, pg, GFP_NOFS);
                    if (!page)
                        continue;
                    zero_user_segment(page, 0, PAGE_SIZE);
                    SetPageUptodate(page);
                }

                /* Zero the punched portion (for partial pages, this
                 * preserves data outside the hole). For full pages,
                 * z_start=0, z_end=PAGE_SIZE (already zeroed above).
                 */
                zero_user_segment(page, z_start, z_end);
                set_page_dirty(page);
                unlock_page(page);
                put_page(page);
                dirty_count++;
            }

            pr_debug("powerfs: FALLOCATE punch_hole FLAT ino=%lu offset=%llu len=%llu, zeroed %d pages\n",
                    inode->i_ino, (unsigned long long)offset,
                    (unsigned long long)len, dirty_count);

            /* Synchronous writeback to update server needle with zeros
             * in the punched region.
             */
            filemap_write_and_wait(inode->i_mapping);
        } else {
            /* Non-FLAT (INLINE): just release page cache (original
             * behavior). INLINE data is in inline_data, not the server
             * needle, so truncate_pagecache_range is sufficient.
             */
            truncate_pagecache_range(inode, offset, new_size - 1);
            if (pi->placement == POWERFS_PLACEMENT_INLINE)
                pi->inline_dirty = true;
        }
        ret = 0;
    } else if (!(mode & FALLOC_FL_KEEP_SIZE)) {
        /* Default mode: extend file size */
        loff_t old_size = i_size_read(inode);

        if (new_size > old_size) {
            i_size_write(inode, new_size);
            mark_inode_dirty(inode);

            /* O-03: For FLAT files, explicitly zero the extended region
             * [old_size, new_size) and trigger synchronous writeback so
             * the server needle is extended with zeros. Without this:
             *   - The extended region has no pagecache pages; a subsequent
             *     writeback RMW gap-fill would find no pages and produce
             *     uninitialized gap data.
             *   - The server needle retains its old size; if refresh_work
             *     later invalidates the pagecache (multi-client NOTIFY),
             *     reads of the extended region would fetch stale/short
             *     data from the server.
             * This mirrors the powerfs_setattr extend path (K2-14).
             */
            if (pi->placement == POWERFS_PLACEMENT_FLAT &&
                pi->volume_id && pi->file_key) {
                pgoff_t start_pg = old_size >> PAGE_SHIFT;
                pgoff_t end_pg = (new_size - 1) >> PAGE_SHIFT;
                pgoff_t pg;
                int dirty_count = 0;

                for (pg = start_pg; pg <= end_pg; pg++) {
                    struct page *page;
                    size_t off = 0;
                    bool need_read = false;

                    if (pg == start_pg) {
                        off = old_size & (PAGE_SIZE - 1);
                        /* If old_size is page-aligned, the entire page
                         * is in the extended region — no need to read.
                         * Otherwise, we need valid data before old_size.
                         */
                        need_read = (off > 0);
                    }

                    if (need_read) {
                        /* Read existing page to preserve data before
                         * old_size. read_mapping_page handles pagecache
                         * lookup and server fetch.
                         */
                        page = read_mapping_page(inode->i_mapping,
                                                  pg, NULL);
                        if (IS_ERR(page)) {
                            /* Server read failed — create zero page */
                            page = find_or_create_page(
                                inode->i_mapping, pg, GFP_NOFS);
                            if (!page)
                                continue;
                            zero_user_segment(page, 0, PAGE_SIZE);
                            SetPageUptodate(page);
                            off = 0;
                        } else {
                            lock_page(page);
                            zero_user_segment(page, off, PAGE_SIZE);
                        }
                    } else {
                        /* Page is entirely in the extended region (or
                         * old_size is page-aligned). Create zero page
                         * without reading from server (avoids fetching
                         * stale data).
                         */
                        page = find_or_create_page(
                            inode->i_mapping, pg, GFP_NOFS);
                        if (!page)
                            continue;
                        zero_user_segment(page, 0, PAGE_SIZE);
                        SetPageUptodate(page);
                    }

                    set_page_dirty(page);
                    unlock_page(page);
                    put_page(page);
                    dirty_count++;
                }

                pr_debug("powerfs: FALLOCATE extend FLAT ino=%lu old=%llu new=%llu, zeroed %d pages\n",
                        inode->i_ino, (unsigned long long)old_size,
                        (unsigned long long)new_size, dirty_count);

                /* Synchronous writeback to update server needle with
                 * zeros in the extended region. Do NOT re-dirty after
                 * writeback; rely on refresh_work's K2-14 skip to keep
                 * the zeroed pages resident in pagecache.
                 */
                filemap_write_and_wait(inode->i_mapping);
            } else if (pi->placement == POWERFS_PLACEMENT_INLINE) {
                pi->inline_dirty = true;
            }
        }
        ret = 0;
    } else {
        /* KEEP_SIZE: no-op (no physical pre-allocation) */
        ret = 0;
    }

falloc_done:
    inode_unlock(inode);

    /* fallocate 成功 (punch/extend) 后, 标记 EXCL/AUTH dirty.
     * INLINE: inline_dirty 已在分支内置位, 这里额外 mark cap dirty 供 revoke 感知. */
    if (ret == 0 && !(mode & FALLOC_FL_KEEP_SIZE))
        powerfs_cap_mark_dirty(pi, POWERFS_CAP_AUTH_EXCL | POWERFS_CAP_FILE_EXCL);

    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}

/* ========== xattr 回调 (simple_xattr L1 cache + Filer Raft L2) ==========
 *
 * xattr 持久化架构:
 *   - L1 缓存: 内存 simple_xattr (pi->xattrs), 用于加速 GET/LIST
 *   - L2 持久化: Filer Raft (通过 powerfs-net TLV: SET_XATTR=0x38/GET_XATTR=0x39/
 *                 REMOVE_XATTR=0x3a/LIST_XATTR=0x3b)
 *
 * 一致性策略:
 *   - GET: 先 L1 cache, miss → net getxattr → 成功则回填 L1
 *   - SET: 同步 net setxattr → Filer Raft 确认 → 更新 L1 cache
 *   - REMOVE: 同步 net removexattr → 成功 → 从 L1 cache 清除
 *   - LIST: 先 L1 cache 非空直接用; 为空 → net listxattr 回填 L1
 *   - CapFlush XATTR_EXCL: L1 条目逐条重推 (recall/fsync 场景兜底)
 *
 * Kernel 6.17 移除了 inode_operations.setxattr/getxattr/removexattr,
 * 改用 xattr_handler 注册到 super_block.s_xattr.
 * 支持 user.* / trusted.* / security.* / system.posix_acl_* 前缀.
 */

static int powerfs_xattr_handler_get(const struct xattr_handler *handler,
                                     struct dentry *unused, struct inode *inode,
                                     const char *name, void *buffer, size_t size)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    __u64 shard_id;
    const char *full_name;
    size_t name_len;
    int ret;

    full_name = xattr_full_name(handler, name);

    /* 快速路径: L1 simple_xattr cache hit */
    ret = simple_xattr_get(&pi->xattrs, full_name, buffer, size);
    if (ret != -ENODATA)
        return ret;  /* hit (>=0) 或 -ERANGE 等直接返回 */

    /* 慢速路径: cache miss → net getxattr 查询 Filer */
    shard_id = powerfs_calc_shard_id(inode->i_ino);
    name_len = strlen(full_name);

    if (buffer && size > 0) {
        size_t got_len = 0;
        ret = powerfs_net_getxattr(shard_id, inode->i_ino,
                                   full_name, name_len,
                                   (__u8 *)buffer, size, &got_len);
        if (ret == 0) {
            struct simple_xattr *old;
            old = simple_xattr_set(&pi->xattrs, full_name, buffer, got_len, 0);
            if (!IS_ERR(old))
                simple_xattr_free(old);
            return (int)got_len;
        }
        return ret;
    } else {
        /* buffer=NULL/size=0: VFS probe 语义, 返回 value 长度 */
        size_t got_len = 0;
        __u8 stackbuf[256];
        __u8 *tmpbuf = stackbuf;
        size_t tmpcap = sizeof(stackbuf);

        ret = powerfs_net_getxattr(shard_id, inode->i_ino,
                                   full_name, name_len,
                                   tmpbuf, tmpcap, &got_len);
        if (ret == -ERANGE)
            return (int)got_len;
        if (ret == 0) {
            struct simple_xattr *old;
            old = simple_xattr_set(&pi->xattrs, full_name, tmpbuf, got_len, 0);
            if (!IS_ERR(old))
                simple_xattr_free(old);
            return (int)got_len;
        }
        return ret;
    }
}

static int powerfs_xattr_handler_set(const struct xattr_handler *handler,
                                     struct mnt_idmap *idmap,
                                     struct dentry *unused, struct inode *inode,
                                     const char *name, const void *value,
                                     size_t size, int flags)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    __u64 shard_id;
    const char *full_name;
    size_t name_len;
    struct simple_xattr *old_xattr;
    int ret;

    (void)idmap;

    if (size > XATTR_SIZE_MAX)
        return -E2BIG;

    full_name = xattr_full_name(handler, name);
    name_len = strlen(full_name);
    shard_id = powerfs_calc_shard_id(inode->i_ino);

    /* value==NULL && size==0: VFS removexattr 语义 */
    if (!value && size == 0) {
        ret = powerfs_net_removexattr(shard_id, inode->i_ino,
                                      full_name, name_len);
        if (ret < 0)
            return ret;

        old_xattr = simple_xattr_set(&pi->xattrs, full_name, NULL, 0, 0);
        if (!IS_ERR(old_xattr))
            simple_xattr_free(old_xattr);

        powerfs_cap_mark_dirty(pi, POWERFS_CAP_XATTR_EXCL);
        pi->i_xattr_version++;
        return 0;
    }

    /* SETXATTR: 先 Filer 持久化 (Raft), 成功后才更新 L1 cache */
    ret = powerfs_net_setxattr(shard_id, inode->i_ino,
                               full_name, name_len,
                               (const __u8 *)value, size);
    if (ret < 0)
        return ret;

    old_xattr = simple_xattr_set(&pi->xattrs, full_name, value, size, flags);
    if (IS_ERR(old_xattr)) {
        pr_warn_ratelimited("powerfs: setxattr ino=%lu name=%s "
                            "simple_xattr_set cache fill failed: %ld (ignored)\n",
                            inode->i_ino, full_name, PTR_ERR(old_xattr));
    } else {
        simple_xattr_free(old_xattr);
    }

    powerfs_cap_mark_dirty(pi, POWERFS_CAP_XATTR_EXCL);
    pi->i_xattr_version++;
    return 0;
}

static ssize_t powerfs_listxattr(struct dentry *dentry, char *buffer,
                                 size_t size)
{
    struct inode *inode = d_inode(dentry);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    ssize_t l1_ret;

    /* 快速路径: L1 cache 非空时直接返回 */
    l1_ret = simple_xattr_list(inode, &pi->xattrs, buffer, size);
    if (l1_ret > 0 || (l1_ret == 0 && size == 0))
        return l1_ret;

    /* 慢速路径: L1 cache 空 → net listxattr 拉取 + 回填 L1 */
    {
        __u64 shard_id = powerfs_calc_shard_id(inode->i_ino);
        char *list_buf;
        size_t list_cap = (size > 0) ? size : 4096;
        size_t list_len = 0;
        int ret;

        if (buffer && size > 0) {
            list_buf = buffer;
        } else {
            list_buf = kmalloc(list_cap, GFP_KERNEL);
            if (!list_buf)
                return -ENOMEM;
        }

        ret = powerfs_net_listxattr(shard_id, inode->i_ino,
                                    list_buf, list_cap, &list_len);
        if (ret == -ERANGE && !(buffer && size > 0)) {
            kfree(list_buf);
            list_cap = list_len;
            list_buf = kmalloc(list_cap, GFP_KERNEL);
            if (!list_buf)
                return -ENOMEM;
            list_len = 0;
            ret = powerfs_net_listxattr(shard_id, inode->i_ino,
                                        list_buf, list_cap, &list_len);
        }
        if (ret < 0) {
            if (list_buf != buffer)
                kfree(list_buf);
            return ret;
        }

        /* 回填 L1: 遍历 NUL-separated keys, 逐个 getxattr + simple_xattr_set */
        {
            size_t pos = 0;
            while (pos < list_len) {
                const char *key = list_buf + pos;
                size_t klen = strlen(key);
                size_t vcap = XATTR_SIZE_MAX;
                __u8 *vbuf;
                size_t vlen = 0;

                vbuf = kmalloc(vcap, GFP_KERNEL);
                if (vbuf) {
                    if (powerfs_net_getxattr(shard_id, inode->i_ino,
                                             key, klen,
                                             vbuf, vcap, &vlen) == 0) {
                        struct simple_xattr *old;
                        old = simple_xattr_set(&pi->xattrs, key, vbuf, vlen, 0);
                        if (!IS_ERR(old))
                            simple_xattr_free(old);
                    }
                    kfree(vbuf);
                }
                pos += klen + 1;
            }
        }

        if (!buffer && size == 0) {
            kfree(list_buf);
            return simple_xattr_list(inode, &pi->xattrs, NULL, 0);
        }
        if (list_buf == buffer)
            return (ssize_t)list_len;

        kfree(list_buf);
        return simple_xattr_list(inode, &pi->xattrs, NULL, 0);
    }
}

static const struct xattr_handler powerfs_security_xattr_handler = {
    .prefix = XATTR_SECURITY_PREFIX,
    .get = powerfs_xattr_handler_get,
    .set = powerfs_xattr_handler_set,
};

static const struct xattr_handler powerfs_trusted_xattr_handler = {
    .prefix = XATTR_TRUSTED_PREFIX,
    .get = powerfs_xattr_handler_get,
    .set = powerfs_xattr_handler_set,
};

static const struct xattr_handler powerfs_user_xattr_handler = {
    .prefix = XATTR_USER_PREFIX,
    .get = powerfs_xattr_handler_get,
    .set = powerfs_xattr_handler_set,
};

static const struct xattr_handler * const powerfs_xattr_handlers[] = {
    &powerfs_security_xattr_handler,
    &powerfs_trusted_xattr_handler,
    &powerfs_user_xattr_handler,
    NULL
};

/* ========== POSIX ACL (.get_inode_acl / .set_acl) ==========
 * P0-3 fix: 对齐  xxx_get_acl / xxx_set_acl.
 *
 * 设计:
 *   - ACL 以 xattr 形式存储在 pi->xattrs (simple_xattr),
 *     name = system.posix_acl_access / system.posix_acl_default.
 *   - get: simple_xattr 查内存 → posix_acl_from_xattr 反序列化成 struct posix_acl*
 *         → set_cached_acl 挂 inode i_acl / i_default_acl (RCU 指针)
 *         → 下次快速命中 cached_acl, 不重走 xattr 反序列化
 *   - set: posix_acl_to_xattr → simple_xattr_set 写内存
 *         → set_cached_acl 更新缓存
 *         → mark dirty_caps XATTR_EXCL + AUTH_EXCL + inode dirty
 *         → 后续 cap_flush/setattr_sync 把 xattr+inode 属性推到 Filer
 *
 * 注意:
 *   - 目前 xattr 内存态后端 (Rust Filer 端 setxattr/removexattr net 协议未实现),
 *     先 warn + 放回 dirty, 下次 recall/fsync 再推进 (同 cap_flush XATTR_EXCL
 *     WARN_ONCE 策略, 避免静默丢脏 ACL).
 *   - 单机场景 ACL 立即可用 (getfacl/setfacl 正常返回 + VFS 权限叠加
 *     __check_acl 路径在 powerfs_permission 内已由 generic_permission 覆盖).
 *   - DEFAULT ACL 只有目录有 (S_ISDIR 判断).
 */

#include <linux/posix_acl.h>  /* posix_acl_from_xattr / set_cached_acl */

static struct posix_acl *
powerfs_get_acl(struct inode *inode, int type, bool rcu)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct posix_acl *acl;
    const char *xattr_name;
    struct simple_xattr *xattr;
    void *value = NULL;
    ssize_t size;

    /* RCU walk: inode 不可锁 + 不允许阻塞. simple_xattr 内部持 spin_lock
     * (非睡眠) + get_cached_acl 只是 RCU deref, 理论可支持 RCU 模式, 但为
     * 避免遗漏 edge case (xattr value kmalloc 未初始化?), 强制 ref-walk. */
    if (rcu)
        return ERR_PTR(-ECHILD);

    switch (type) {
    case ACL_TYPE_ACCESS:
        xattr_name = XATTR_NAME_POSIX_ACL_ACCESS;
        break;
    case ACL_TYPE_DEFAULT:
        if (!S_ISDIR(inode->i_mode))
            return NULL;  /* 非目录无 default ACL (VFS 已检查, 再兜底) */
        xattr_name = XATTR_NAME_POSIX_ACL_DEFAULT;
        break;
    default:
        return ERR_PTR(-EINVAL);
    }

    /* Fast path: 缓存命中 (ACL_NOT_CACHED = 哨兵 NULL 指针, 需实际查). */
    acl = get_cached_acl(inode, type);
    if (acl != ACL_NOT_CACHED)
        return acl;

    /* Slow path: simple_xattr_get (返回 size 或写入 buffer).
     * 先查 size (buffer=NULL), 再分配 buffer, 再读实际 value. */
    size = simple_xattr_get(&pi->xattrs, xattr_name, NULL, 0);
    if (size <= 0) {
        /* size < 0: 未设置 → acl = NULL; size == 0: 空值等价于未设 */
        acl = NULL;
        goto out_cache;
    }
    value = kmalloc(size, GFP_KERNEL);
    if (!value)
        return ERR_PTR(-ENOMEM);
    xattr = NULL; /* suppress unused (simple_xattr_get 有 size-only 模式) */
    size = simple_xattr_get(&pi->xattrs, xattr_name, value, size);
    if (size < 0) {
        kfree(value);
        return ERR_PTR((int)size);
    }
    acl = posix_acl_from_xattr(&init_user_ns, value, size);
    kfree(value);
    if (IS_ERR(acl))
        return acl;

out_cache:
    set_cached_acl(inode, type, acl); /* 挂 RCU 指针缓存, 下次快路径命中 */
    return acl;
}

/*
 * powerfs_set_acl — 设置/删除 POSIX ACL.
 *
 * 调用链: setfacl(1) → sys_acl_set_file → set_posix_acl → iop->set_acl.
 * set_posix_acl 外层已做权限检查 + 应用 ACL mask 到 inode->i_mode 后
 * 调用 set_acl; 我们只需: 1) 写内存 xattr  2) 更新缓存  3) 打 dirty_caps
 * set_posix_acl 外层会处理 mark_inode_dirty(inode) 及 setattr_copy →
 * i_mode 已更新 → AUTH_EXCL 需同步 (chmod 叠加位).
 */
static int
powerfs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
                struct posix_acl *acl, int type)
{
    struct inode *inode = d_inode(dentry);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    const char *xattr_name;
    struct simple_xattr *old_xattr;
    void *value = NULL;
    int size;
    int ret = 0;

    (void)idmap;

    switch (type) {
    case ACL_TYPE_ACCESS:
        xattr_name = XATTR_NAME_POSIX_ACL_ACCESS;
        break;
    case ACL_TYPE_DEFAULT:
        if (!S_ISDIR(inode->i_mode))
            return acl ? -EACCES : 0; /* 非目录, 删 default ACL = NOP, 设 = EACCES */
        xattr_name = XATTR_NAME_POSIX_ACL_DEFAULT;
        break;
    default:
        return -EINVAL;
    }

    if (acl) {
        /* Step 1: ACL → xattr value 序列化 (先 probe size, 再写入). */
        size = posix_acl_to_xattr(&init_user_ns, acl, NULL, 0);
        if (size < 0)
            return size;
        value = kmalloc(size, GFP_NOFS);
        if (!value)
            return -ENOMEM;
        ret = posix_acl_to_xattr(&init_user_ns, acl, value, size);
        if (ret < 0) {
            kfree(value);
            return ret;
        }
        /* Step 2: 写 simple_xattr (创建或替换原值, flags=0 无特殊语义). */
        old_xattr = simple_xattr_set(&pi->xattrs, xattr_name, value, size, 0);
        kfree(value);
        if (IS_ERR(old_xattr))
            return PTR_ERR(old_xattr);
        simple_xattr_free(old_xattr);
    } else {
        /* acl == NULL: 删除 ACL xattr. 若当前未设置, XATTR_REPLACE 返回 -ENODATA
         * 但我们接受"删除不存在项 = NOP", 忽略 -ENODATA. */
        old_xattr = simple_xattr_set(&pi->xattrs, xattr_name, NULL, 0, XATTR_REPLACE);
        if (!IS_ERR(old_xattr))
            simple_xattr_free(old_xattr);
        else if (PTR_ERR(old_xattr) != -ENODATA)
            return PTR_ERR(old_xattr);
    }

    /* Step 3: 更新缓存. */
    set_cached_acl(inode, type, acl);

    /* Step 4: 打 dirty_caps + xattr version bump.
     *   XATTR_EXCL: 需要推 xattr 到 Filer (协议未实现, 当前 WARN+回滚)
     *   AUTH_EXCL: inode i_mode 被 set_posix_acl 外层改了 (ACL mask 叠加),
     *              需同步 setattr 到 Filer */
    spin_lock(&pi->i_lock);
    pi->i_dirty_caps |= POWERFS_CAP_XATTR_EXCL | POWERFS_CAP_AUTH_EXCL;
    spin_unlock(&pi->i_lock);
    pi->i_xattr_version++;  /* xattr 版本号, Filer 端用于 fencing 旧请求 */
    inode_inc_iversion_raw(inode);

    return 0;
}

/* ========== Inode operations 表 ========== */

/* 目录 inode 操作 */
static const struct inode_operations powerfs_dir_inode_operations = {
    .create         = powerfs_create,
    .lookup         = powerfs_lookup,
    .link           = powerfs_link,
    .unlink         = powerfs_unlink,
    .symlink        = powerfs_symlink,
    .readlink       = powerfs_readlink,
    .mkdir          = powerfs_mkdir,
    .rmdir          = powerfs_rmdir,
    .mknod          = powerfs_mknod,
    .rename         = powerfs_rename,
    .atomic_open    = powerfs_atomic_open,  /* P1-2: lookup→create→open 原子路径，防 TOCTOU */
    .permission     = powerfs_permission,
    .getattr        = powerfs_getattr,
    .setattr        = powerfs_setattr,
    .listxattr      = powerfs_listxattr,
    .get_inode_acl  = powerfs_get_acl,  /* P0-3 fix: POSIX ACL get (getfacl/permission 叠加) */
    .set_acl        = powerfs_set_acl,  /* P0-3 fix: POSIX ACL set (setfacl) */
    .fileattr_get   = powerfs_fileattr_get,  /* P1-3: chattr/lsattr GETFLAGS/FSGETXATTR 回调 */
    .fileattr_set   = powerfs_fileattr_set,  /* P1-3: chattr SETFLAGS/FSSETXATTR 回调 */
};

/* 普通文件 inode 操作 */
static const struct inode_operations powerfs_file_inode_operations = {
    .permission     = powerfs_permission,
    .getattr        = powerfs_getattr,
    .setattr        = powerfs_setattr,
    .listxattr      = powerfs_listxattr,
    .get_inode_acl  = powerfs_get_acl,  /* P0-3 fix: POSIX ACL get (文件 ACCESS ACL) */
    .set_acl        = powerfs_set_acl,  /* P0-3 fix: POSIX ACL set (setfacl 修改文件 ACL) */
    .fileattr_get   = powerfs_fileattr_get,  /* P1-3: chattr/lsattr GETFLAGS/FSGETXATTR 回调 */
    .fileattr_set   = powerfs_fileattr_set,  /* P1-3: chattr SETFLAGS/FSSETXATTR 回调 */
};

/*
 * powerfs_show_options - 显示挂载选项
 */
static int powerfs_show_options(struct seq_file *m, struct dentry *root)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(root->d_sb);

    seq_printf(m, ",master_addr=%s", sbi->master_addr);
    seq_printf(m, ",master_port=%u", sbi->master_port);

    return 0;
}

/*
 * powerfs_put_super - P1-1: VFS 在卸载前调用此回调 (deactivate_locked_super → put_super).
 *
 * 职责 (对齐  xxx_put_super → destroy_mdsc 全量资源回卷):
 *   1. 注销 Cap NOTIFY 回调 (防止 RX dispatcher 在销毁后仍派发到 fs 层)
 *   2. 销毁 per-sb slab caches (cap_cachep / cap_snap_cachep / cap_flush_cachep)
 *
 * 注意: 重型清理 (workqueue/网络/sync) 放在 kill_sb_super，因为 put_super 执行时
 * VFS 仍持有 s_umount sem，同步写回/销毁 workqueue 可能阻塞; kill_sb 顺序是
 *   umount_begin → put_super → deactivate_locked_super → kill_sb
 * 所以 put_super 做"断开回调 + 轻量 slab 销毁"，kill_sb 做重型清理。
 */
static void powerfs_put_super(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    pr_debug("powerfs: put_super\n");

    if (!sbi)
        return;

    /* P3-4: 清理 debugfs 入口 (在 slab 销毁前, 避免遍历已释放的 inode) */
    powerfs_debugfs_cleanup(sb);

    /* P3-5: 清理 /proc 入口 */
    powerfs_proc_cleanup(sb);

    /* 1. 注销 Cap NOTIFY 回调 (防止 RX dispatcher 再派发 recall/upgrade 到本 sb).
     *    注意: 全局 refresh_wq 还没销毁，回调内部 queue_work 不会用 NULL wq，
     *    但 powerfs_cap_recall_notify_handler 开头会检查 g_powerfs_sb != sb 直接返回，
     *    安全. */
    powerfs_net_reg_cap_notify_handlers(NULL, NULL);

    /* 2. 销毁 per-sb slab caches.
     *    顺序: cap_flush (最短期) → cap_snap (snap 单元) → cap (长期).
     *    kmem_cache_destroy 内部会等所有已分配对象 free 完再销毁 slab，
     *    若仍有对象泄漏会在 dmesg 打印 "kmem_cache_destroy X remaining"，
     *    便于排查 cap 泄漏. */
    if (sbi->cap_flush_cachep) {
        kmem_cache_destroy(sbi->cap_flush_cachep);
        sbi->cap_flush_cachep = NULL;
    }
    if (sbi->cap_snap_cachep) {
        kmem_cache_destroy(sbi->cap_snap_cachep);
        sbi->cap_snap_cachep = NULL;
    }
    if (sbi->cap_cachep) {
        kmem_cache_destroy(sbi->cap_cachep);
        sbi->cap_cachep = NULL;
    }
}

/*
 * powerfs_sync_fs - P2-1: VFS 在 sync(2)/syncfs(2) 时调用.
 *
 * 对齐  xxx_sync_fs: 分 wait=0 (非阻塞) 和 wait=1 (阻塞) 两档.
 *
 * 非阻塞 (!wait):
 *   - 遍历所有 inode, 对有 dirty_caps 的 inode 触发 cap_flush (非阻塞入队).
 *     用户态 sync(2) 先发 non-blocking 批次做"尽力推", 然后再发 blocking
 *     批次等 ACK. 这档不能阻塞 (调用方不持页锁, 但在 global sync 上下文中).
 *
 * 阻塞 (wait):
 *   - flush_workqueue(writeback_wq): 等 Stage C writeback_work 全部执行完
 *     (这些 work 发 powerfs_net_write 落盘).
 *   - flush_workqueue(refresh_wq): 等 cap recall/upgrade NOTIFY 的异步处理完.
 *   - sync_filesystem(sb): 用 VFS 内置 writeback 循环触发 write_inode +
 *     writepage, WB_SYNC_ALL 模式, 等所有 I_DIRTY_PAGES inode 落盘.
 *
 * 注意: write_inode 已在 umount (stopping=1) 时短路跳过网络同步, 但 sync_fs
 * 调用时网络还活着, 所以 write_inode 会真的推 SetAttr 到 Filer, 与 kill_sb
 * 的 sync_filesystem 用途不同.
 */
static int powerfs_sync_fs(struct super_block *sb, int wait)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    pr_debug("powerfs: sync_fs wait=%d\n", wait);

    /* 非阻塞档: 遍历 inode 触发 dirty caps 入队 (cap_flush 内部不阻塞等 ACK,
     * 仅分配 flush record + 切 flushing_caps + 发 TX, TX 本身是异步 sendmsg).
     * 若某些 inode 此时无法 flush (内存不足), 留给下次 sync. */
    if (!wait) {
        /* 用 VFS iterate_supers 风格的 writeback_inodes_sb_nr 间接触发.
         * Stage C writeback 工作线程会再处理 cap 脏数据, 这里不强推,
         * 对齐  xxx_flush_dirty_caps 只 flush 已 pending 的 cap.
         *
         * 我们目前 cap_flush 已经在 recall/fsync 路径上触发, 非阻塞档仅
         * kick writeback_wq 让已入队的 work 尽早执行即可. */
        if (sbi && sbi->writeback_wq)
            writeback_inodes_sb(sb, WB_REASON_SYNC);
        pr_debug("powerfs: sync_fs non-blocking done\n");
        return 0;
    }

    /* 阻塞档: 严格等所有层落盘. */

    /* Step 1: kick 一次 writeback_inodes 让 write_inode 把 SetAttr 推出去.
     *         WB_SYNC_ALL 由 sync_filesystem 内部统一执行, 这里先 kick. */
    writeback_inodes_sb(sb, WB_REASON_SYNC);

    /* Step 2: 等 writeback_wq 所有 work 执行完 (powerfs_net_write 异步发送).
     *         destroy_workqueue 会 drain, 但我们不想 destroy, flush 就够了. */
    if (sbi && sbi->writeback_wq)
        flush_workqueue(sbi->writeback_wq);

    /* Step 3: 等 refresh_wq (cap recall/upgrade 的异步 work) 完成,
     *         防止 recall 半状态 inode 在 sync 完成后仍有脏. */
    if (powerfs_refresh_wq)
        flush_workqueue(powerfs_refresh_wq);

    /* Step 4: inode/page cache 阻塞等待 writeback 完成.
     *
     * 注意: 严禁调用 sync_filesystem(sb). Linux 6.17 sync_filesystem()
     * 内部 fs/sync.c L66 会回调 ->sync_fs(sb, wait=1), 而我们正是在
     * sync_fs(wait=1) 上下文中执行, 会形成 sync_filesystem ↔ powerfs_sync_fs
     * 无限递归 → 内核栈溢出 → TASK stack guard page was hit → panic.
     *
     * PowerFS 是 nodev fs (sb->s_bdev == NULL), 块设备路径完全不生效;
     * 我们也没有 export sync_blockdev_nowait/sync_blockdev. 直接使用
     * sync_inodes_sb(sb): 内部 writeback_inodes_sb 触发 writepage/write_inode,
     * 然后等待 I_DIRTY_PAGES 全部落盘 — 已经覆盖了 sync_filesystem 对
     * nodev fs 的实际有用的路径, 并且不会再次回调 ->sync_fs. */
    sync_inodes_sb(sb);

    pr_debug("powerfs: sync_fs blocking done\n");
    return 0;
}

/*
 * powerfs_umount_begin - P2-2: 处理 lazy umount (MNT_DETACH) 的准备阶段.
 *
 * 对齐  xxx_umount_begin: VFS 在 umount_begin 时调用 (发生在
 * put_super / kill_sb 之前, 用户态刚执行 umount 时), 用于:
 *   1. 标记 sb 进入卸载态 (shutting_down), 阻止后续网络请求入队;
 *   2. 尽力 sync 一次脏数据 (sync_fs wait=1, 非强制, 失败不回滚);
 *   3. 准备把 inode 上的 cap 标记为 "可 revoke", 后续 recall 直接放行.
 *
 * 注意: 对 normal umount 也会触发 (不只是 MNT_DETACH). 后续 kill_sb_super
 * 会再执行更彻底的清理 (destroy_workqueue / net cleanup / slab destroy).
 *
 * 设计选择 — 不直接 abort 网络请求 (像  xxx_osdc_abort_requests):
 *   我们 powerfs-net 层没有 per-request abort 句柄, 改用全局 stopping 标志
 *   让后续 send_request 立即返回 -ENOTCONN, in-flight 请求由超时机制回收.
 *   这样不会在 umount_begin 时把正常 close() 的 cap_release 打断.
 */
static void powerfs_umount_begin(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    pr_debug("powerfs: umount_begin\n");

    if (!sbi)
        return;

    /* 1. 标记 sb shutting_down: 让 lease_renew_work_func 不再重新排队,
     *    防止 lease_wq flush 循环. */
    sbi->shutting_down = true;

    /* 2. 尽力阻塞 sync 一次 (不强制, 失败也不阻止卸载继续).
     *    sync_fs 内部已经处理 writeback_wq / refresh_wq flush. */
    (void)powerfs_sync_fs(sb, 1);

    pr_debug("powerfs: umount_begin done\n");
}

/* ========== Super operations 表 ========== */

static const struct super_operations powerfs_super_ops = {
    .alloc_inode   = powerfs_alloc_inode,
    .free_inode    = powerfs_free_inode,
    .evict_inode   = powerfs_evict_inode,
    .write_inode   = powerfs_write_inode,  /* Stage C: writeback 时同步 i_size 到 Filer */
    .statfs        = powerfs_statfs,
    .drop_inode    = generic_delete_inode,
    .show_options  = powerfs_show_options,
    .put_super     = powerfs_put_super,    /* P1-1: 挂载清理 — 注销回调 + 销毁 per-sb slab */
    .sync_fs       = powerfs_sync_fs,      /* P2-1: sync(2)/syncfs(2) → flush wq + writeback */
    .umount_begin  = powerfs_umount_begin, /* P2-2: lazy umount → shutting_down + best-effort sync */
};

/* ========== P3-5: 全局性能计数 + /proc/metrics ========== */

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/percpu_counter.h>

/*
 * powerfs_metrics_init - 初始化全局性能计数器.
 *
 * 对齐  xxx_metric_init (metric.c):
 *   percpu_counter 用 batch=0 确保精确读取.
 */
int powerfs_metrics_init(struct powerfs_metrics *m)
{
    int ret, i;

    for (i = 0; i < POWERFS_METRIC_MAX; i++) {
        spin_lock_init(&m->metric[i].lock);
        m->metric[i].total = 0;
        m->metric[i].size_sum = 0;
        m->metric[i].size_min = U64_MAX;
        m->metric[i].size_max = 0;
        m->metric[i].latency_sum = 0;
        m->metric[i].latency_min = U64_MAX;
        m->metric[i].latency_max = 0;
    }

    ret = percpu_counter_init(&m->d_lease_hit, 0, GFP_KERNEL);
    if (ret)
        return ret;
    ret = percpu_counter_init(&m->d_lease_mis, 0, GFP_KERNEL);
    if (ret)
        goto err_d_lease_mis;
    ret = percpu_counter_init(&m->i_caps_hit, 0, GFP_KERNEL);
    if (ret)
        goto err_i_caps_hit;
    ret = percpu_counter_init(&m->i_caps_mis, 0, GFP_KERNEL);
    if (ret)
        goto err_i_caps_mis;
    ret = percpu_counter_init(&m->opened_inodes, 0, GFP_KERNEL);
    if (ret)
        goto err_opened_inodes;
    ret = percpu_counter_init(&m->total_inodes, 0, GFP_KERNEL);
    if (ret)
        goto err_total_inodes;

    atomic64_set(&m->opened_files, 0);
    atomic64_set(&m->total_caps, 0);
    return 0;

err_total_inodes:
    percpu_counter_destroy(&m->opened_inodes);
err_opened_inodes:
    percpu_counter_destroy(&m->i_caps_mis);
err_i_caps_mis:
    percpu_counter_destroy(&m->i_caps_hit);
err_i_caps_hit:
    percpu_counter_destroy(&m->d_lease_mis);
err_d_lease_mis:
    percpu_counter_destroy(&m->d_lease_hit);
    return ret;
}

void powerfs_metrics_destroy(struct powerfs_metrics *m)
{
    percpu_counter_destroy(&m->d_lease_hit);
    percpu_counter_destroy(&m->d_lease_mis);
    percpu_counter_destroy(&m->i_caps_hit);
    percpu_counter_destroy(&m->i_caps_mis);
    percpu_counter_destroy(&m->opened_inodes);
    percpu_counter_destroy(&m->total_inodes);
}

/*
 * powerfs_update_metric - 更新单个操作类型的延迟/吞吐统计.
 *
 * 对齐  xxx_update_metrics (metric.c L343):
 *   只在 rc >= 0 (成功) 时统计, 失败不计数.
 *   延迟用 ktime_to_ns, 跟踪 sum/min/max.
 */
void powerfs_update_metric(struct powerfs_metric *m, ktime_t start, ktime_t end,
                           unsigned int size, int rc)
{
    u64 lat;

    if (rc < 0)
        return;

    lat = ktime_to_ns(ktime_sub(end, start));

    spin_lock(&m->lock);
    m->total++;
    m->size_sum += size;
    if (size < m->size_min)
        m->size_min = size;
    if (size > m->size_max)
        m->size_max = size;
    m->latency_sum += lat;
    if (lat < m->latency_min)
        m->latency_min = lat;
    if (lat > m->latency_max)
        m->latency_max = lat;
    spin_unlock(&m->lock);
}

/* ----- /proc 展示函数 ----- */

static const char * const powerfs_metric_names[] = {
    "read",
    "write",
    "metadata",
};

/*
 * metrics_latency_show - IO 延迟统计.
 *
 * 对齐  metrics_latency_show (debugfs.c L171):
 *   total, avg_lat(us), min_lat(us), max_lat(us)
 */
static int powerfs_proc_metrics_latency_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_metrics *m;
    int i;

    if (!sbi || !sbi->client) {
        seq_puts(s, "client not initialized\n");
        return 0;
    }
    m = &sbi->client->metrics;

    seq_printf(s, "item          total       avg_lat(us)     min_lat(us)     max_lat(us)\n");
    seq_printf(s, "-------------------------------------------------------------------------\n");

    for (i = 0; i < POWERFS_METRIC_MAX; i++) {
        u64 total, lat_sum, lat_min, lat_max, avg_ns;

        spin_lock(&m->metric[i].lock);
        total = m->metric[i].total;
        lat_sum = m->metric[i].latency_sum;
        lat_min = m->metric[i].latency_min;
        lat_max = m->metric[i].latency_max;
        spin_unlock(&m->metric[i].lock);

        avg_ns = total > 0 ? div64_u64(lat_sum, total) : 0;

        seq_printf(s, "%-14s%-12llu%-16llu%-16llu%llu\n",
                   powerfs_metric_names[i],
                   total,
                   div64_u64(avg_ns, 1000),
                   total > 0 ? div64_u64(lat_min, 1000) : 0,
                   div64_u64(lat_max, 1000));
    }

    return 0;
}

/*
 * metrics_size_show - IO 吞吐量统计.
 *
 * 对齐  metrics_size_show (debugfs.c L197):
 *   total, avg_sz(bytes), min_sz, max_sz, total_sz
 */
static int powerfs_proc_metrics_size_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_metrics *m;
    int i;

    if (!sbi || !sbi->client) {
        seq_puts(s, "client not initialized\n");
        return 0;
    }
    m = &sbi->client->metrics;

    seq_printf(s, "item          total       avg_sz(bytes)   min_sz(bytes)   max_sz(bytes)  total_sz(bytes)\n");
    seq_printf(s, "----------------------------------------------------------------------------------------\n");

    for (i = 0; i < POWERFS_METRIC_MAX; i++) {
        u64 total, sum, avg, min_sz, max_sz;

        /* metadata 无 size, 跳过 */
        if (i == POWERFS_METRIC_METADATA)
            continue;

        spin_lock(&m->metric[i].lock);
        total = m->metric[i].total;
        sum = m->metric[i].size_sum;
        min_sz = m->metric[i].size_min;
        max_sz = m->metric[i].size_max;
        spin_unlock(&m->metric[i].lock);

        avg = total > 0 ? div64_u64(sum, total) : 0;
        min_sz = (total > 0 && min_sz == U64_MAX) ? 0 : min_sz;

        seq_printf(s, "%-14s%-12llu%-16llu%-16llu%-15llu%llu\n",
                   powerfs_metric_names[i], total, avg, min_sz, max_sz, sum);
    }

    return 0;
}

/*
 * metrics_caps_show - Cap/Dentry lease 命中率 + 文件/inode 计数.
 *
 * 对齐  metrics_caps_show (debugfs.c L227) + metrics_file_show (L148):
 */
static int powerfs_proc_metrics_caps_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_metrics *m;
    s64 d_hit, d_mis, c_hit, c_mis, open_inodes, total_inodes;

    if (!sbi || !sbi->client) {
        seq_puts(s, "client not initialized\n");
        return 0;
    }
    m = &sbi->client->metrics;

    d_hit = percpu_counter_sum(&m->d_lease_hit);
    d_mis = percpu_counter_sum(&m->d_lease_mis);
    c_hit = percpu_counter_sum(&m->i_caps_hit);
    c_mis = percpu_counter_sum(&m->i_caps_mis);
    open_inodes = percpu_counter_sum(&m->opened_inodes);
    total_inodes = percpu_counter_sum(&m->total_inodes);

    seq_printf(s, "item                               total\n");
    seq_printf(s, "------------------------------------------\n");
    seq_printf(s, "%-35s%lld\n", "total inodes", total_inodes);
    seq_printf(s, "%-35s%lld\n", "opened files", atomic64_read(&m->opened_files));
    seq_printf(s, "%-35s%lld\n", "pinned i_caps", atomic64_read(&m->total_caps));
    seq_printf(s, "%-35s%lld\n", "opened inodes", open_inodes);
    seq_printf(s, "\n");
    seq_printf(s, "item                               hit         miss        hit_rate\n");
    seq_printf(s, "------------------------------------------------------------\n");
    seq_printf(s, "%-35s%-12lld%-12lld", "dentry lease", d_hit, d_mis);
    if (d_hit + d_mis > 0)
        seq_printf(s, "%llu%%\n", div64_u64(d_hit * 100, d_hit + d_mis));
    else
        seq_puts(s, "N/A\n");
    seq_printf(s, "%-35s%-12lld%-12lld", "inode caps", c_hit, c_mis);
    if (c_hit + c_mis > 0)
        seq_printf(s, "%llu%%\n", div64_u64(c_hit * 100, c_hit + c_mis));
    else
        seq_puts(s, "N/A\n");

    return 0;
}

/* P3-5: /proc entry wrappers — procfs uses proc_ops (not file_operations).
 * pde_data(inode) retrieves the sb pointer passed via proc_create_data. */
static int powerfs_proc_metrics_latency_open(struct inode *inode, struct file *file)
{
    return single_open(file, powerfs_proc_metrics_latency_show, pde_data(inode));
}
static const struct proc_ops powerfs_proc_metrics_latency_pops = {
    .proc_open    = powerfs_proc_metrics_latency_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int powerfs_proc_metrics_size_open(struct inode *inode, struct file *file)
{
    return single_open(file, powerfs_proc_metrics_size_show, pde_data(inode));
}
static const struct proc_ops powerfs_proc_metrics_size_pops = {
    .proc_open    = powerfs_proc_metrics_size_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

static int powerfs_proc_metrics_caps_open(struct inode *inode, struct file *file)
{
    return single_open(file, powerfs_proc_metrics_caps_show, pde_data(inode));
}
static const struct proc_ops powerfs_proc_metrics_caps_pops = {
    .proc_open    = powerfs_proc_metrics_caps_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/*
 * powerfs_proc_init - 在 fill_super 中调用, 创建 /proc/powerfs/<sb_id>/ 入口.
 *
 * 目录结构: /proc/powerfs/sb-<addr>/
 *   latency  - IO 延迟统计 (read/write/metadata: total, avg/min/max lat)
 *   size     - IO 吞吐量统计 (read/write: total, avg/min/max size, total_bytes)
 *   caps     - Cap/Dentry lease 命中率 + opened_files/inodes 计数
 */
static void powerfs_proc_init(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct proc_dir_entry *root;
    char name[32];

    snprintf(name, sizeof(name), "sb-%p", sb);

    /* 创建 /proc/powerfs/ 父目录 (已存在则返回旧入口) */
    root = proc_mkdir("powerfs", NULL);
    if (!root) {
        pr_warn("powerfs: failed to create /proc/powerfs\n");
        return;
    }

    sbi->proc_dir = proc_mkdir(name, root);
    if (!sbi->proc_dir) {
        pr_warn("powerfs: failed to create /proc/powerfs/%s\n", name);
        return;
    }

    proc_create_data("latency", 0444, sbi->proc_dir, &powerfs_proc_metrics_latency_pops, sb);
    proc_create_data("size",    0444, sbi->proc_dir, &powerfs_proc_metrics_size_pops, sb);
    proc_create_data("caps",    0444, sbi->proc_dir, &powerfs_proc_metrics_caps_pops, sb);

    pr_info("powerfs: /proc/powerfs/%s/ entries created\n", name);
}

/*
 * powerfs_proc_cleanup - 在 put_super 中调用, 清理 /proc 入口.
 */
static void powerfs_proc_cleanup(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    if (sbi->proc_dir) {
        proc_remove(sbi->proc_dir);
        sbi->proc_dir = NULL;
    }
}

/* ========== P3-4: Debugfs 内部状态导出 ========== */

#ifdef CONFIG_DEBUG_FS

#include <linux/debugfs.h>
#include <linux/seq_file.h>

/*
 * status_show - 挂载状态总览.
 *
 * 对齐  status_show (debugfs.c L350):
 *   mount_state, master 地址, client_id, blocklisted, writeback 统计.
 */
static int powerfs_debugfs_status_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_client *client = sbi->client;

    if (!client) {
        seq_puts(s, "client not initialized\n");
        return 0;
    }

    seq_printf(s, "mount_state:    %d\n", client->mount_state);
    seq_printf(s, "master:         %s:%u\n", sbi->master_addr, sbi->master_port);
    seq_printf(s, "client_id:      %s\n", client->client_id);
    seq_printf(s, "blocklisted:    %s\n", str_true_false(client->blocklisted));
    seq_printf(s, "shutting_down:  %s\n", str_true_false(sbi->shutting_down));
    seq_printf(s, "max_file_size:  %lld\n", client->max_file_size);
    seq_printf(s, "writeback:      %ld (congested=%s)\n",
               atomic_long_read(&client->writeback_count),
               str_true_false(client->write_congested));
    seq_printf(s, "wb_in_flight:   %d (max=%d)\n",
               atomic_read(&sbi->wb_in_flight), POWERFS_WB_MAX_IN_FLIGHT);
    seq_printf(s, "next_ino:       %d\n", atomic_read(&sbi->next_ino));

    return 0;
}

/*
 * caps_show - 遍历所有 inode, 导出 cap 状态.
 *
 * 对齐  caps_show (debugfs.c L266):
 *   遍历 super_block->s_inodes 列表, 每个 inode 打印:
 *   ino, issued, implemented, dirty_caps, flushing_caps, refs.
 */
static int powerfs_debugfs_caps_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct powerfs_client *client = POWERFS_SB_INFO(sb)->client;
    struct inode *inode;

    seq_printf(s, "ino              issued           implemented      dirty           flushing        refs\n");
    seq_printf(s, "------------------------------------------------------------------------------------------\n");

    if (!client) {
        seq_puts(s, "(client not initialized)\n");
        return 0;
    }

    spin_lock(&sb->s_inode_list_lock);
    list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);

        if (!pi)
            continue;

        /* igrab 安全: 持有 s_inode_list_lock 时 inode 不会被释放 */
        if (!igrab(inode))
            continue;
        spin_unlock(&sb->s_inode_list_lock);

        spin_lock(&pi->i_lock);
        if (pi->i_auth_cap || pi->i_dirty_caps || pi->i_flushing_caps) {
            seq_printf(s, "%-16lu%-17x%-17x%-17x%-17x%-4d\n",
                       inode->i_ino,
                       pi->i_auth_cap ? pi->i_auth_cap->issued : 0,
                       pi->i_auth_cap ? pi->i_auth_cap->implemented : 0,
                       pi->i_dirty_caps,
                       pi->i_flushing_caps,
                       pi->i_pin_ref);
        }
        spin_unlock(&pi->i_lock);

        iput(inode);
        spin_lock(&sb->s_inode_list_lock);
    }
    spin_unlock(&sb->s_inode_list_lock);

    /* 全局 cap LRU 统计 */
    {
        int cap_lru_count = 0;
        struct powerfs_cap *entry;

        spin_lock(&client->cap_lru_lock);
        list_for_each_entry(entry, &client->cap_lru_list, lru_item)
            cap_lru_count++;
        spin_unlock(&client->cap_lru_lock);

        seq_printf(s, "\ncap_lru_count:  %d\n", cap_lru_count);
    }

    /* 全局 dirty/flushing 列表统计 */
    {
        int flush_count = 0;
        struct powerfs_cap_flush *cf;

        spin_lock(&client->cap_flush_lock);
        list_for_each_entry(cf, &client->cap_flush_list, g_list)
            flush_count++;
        spin_unlock(&client->cap_flush_lock);

        seq_printf(s, "cap_flush_list: %d\n", flush_count);
    }

    return 0;
}

/*
 * inodes_show - 遍历所有 inode, 导出关键属性.
 *
 * 对齐  mdsc_show (debugfs.c L52) 的 inode 部分:
 *   ino, mode, size, nlink, placement, dirty, cache_valid.
 */
static int powerfs_debugfs_inodes_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct inode *inode;

    seq_printf(s, "ino              mode      size             nlink  placement  dirty  cache_valid\n");
    seq_printf(s, "----------------------------------------------------------------------------------------\n");

    spin_lock(&sb->s_inode_list_lock);
    list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);

        if (!pi)
            continue;

        if (!igrab(inode))
            continue;
        spin_unlock(&sb->s_inode_list_lock);

        spin_lock(&pi->i_lock);
        seq_printf(s, "%-16lu%-10o%-16lld%-7d%-10d%-7x%-7d\n",
                   inode->i_ino,
                   inode->i_mode,
                   i_size_read(inode),
                   inode->i_nlink,
                   pi->placement,
                   pi->i_dirty_caps,
                   pi->cache_valid ? 1 : 0);
        spin_unlock(&pi->i_lock);

        iput(inode);
        spin_lock(&sb->s_inode_list_lock);
    }
    spin_unlock(&sb->s_inode_list_lock);

    return 0;
}

/*
 * dentries_show - 遍历 dentry lease 列表, 导出 lease 状态.
 *
 * 对齐  caps_show_cb + dentry lease 部分:
 *   dentry name, lease_expire, lease_gen, dir_shared_gen, flags.
 */
static int powerfs_debugfs_dentries_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct powerfs_client *client = POWERFS_SB_INFO(sb)->client;
    struct powerfs_dentry_info *di;
    int count = 0;

    if (!client) {
        seq_puts(s, "(client not initialized)\n");
        return 0;
    }

    seq_printf(s, "dentry            lease_expire  lease_gen  dir_shared_gen  flags\n");
    seq_printf(s, "------------------------------------------------------------------------\n");

    spin_lock(&client->dentry_lease_lock);
    list_for_each_entry(di, &client->dentry_lease_list, lease_list) {
        struct dentry *dentry = di->dentry;
        unsigned long now = jiffies;
        bool expired = di->lease_expire && time_after_eq(now, di->lease_expire);

        if (dentry && dentry->d_name.name) {
            seq_printf(s, "%-18s%-14lu%-11u%-16llu%-5lx%s\n",
                       dentry->d_name.name,
                       di->lease_expire,
                       di->lease_gen,
                       di->dir_shared_gen,
                       di->flags,
                       expired ? " (EXPIRED)" : "");
        }
        count++;
    }
    spin_unlock(&client->dentry_lease_lock);

    seq_printf(s, "\ntotal dentry_leases: %d\n", count);
    return 0;
}

/*
 * leases_show - 目录 lease 状态 (shared_gen + I_COMPLETE).
 */
static int powerfs_debugfs_leases_show(struct seq_file *s, void *p)
{
    struct super_block *sb = s->private;
    struct inode *inode;

    seq_printf(s, "ino              shared_gen  complete  rdcache_gen  rfiles  rbytes\n");
    seq_printf(s, "------------------------------------------------------------------------\n");

    spin_lock(&sb->s_inode_list_lock);
    list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);

        if (!pi || !S_ISDIR(inode->i_mode))
            continue;

        if (!igrab(inode))
            continue;
        spin_unlock(&sb->s_inode_list_lock);

        spin_lock(&pi->i_lock);
        seq_printf(s, "%-16lu%-12d%-10s%-13u%-8llu%-12llu\n",
                   inode->i_ino,
                   atomic_read(&pi->i_shared_gen),
                   (pi->i_flags & POWERFS_I_COMPLETE) ? "yes" : "no",
                   pi->i_rdcache_gen,
                   pi->i_rfiles,
                   pi->i_rbytes);
        spin_unlock(&pi->i_lock);

        iput(inode);
        spin_lock(&sb->s_inode_list_lock);
    }
    spin_unlock(&sb->s_inode_list_lock);

    return 0;
}

DEFINE_SHOW_ATTRIBUTE(powerfs_debugfs_status);
DEFINE_SHOW_ATTRIBUTE(powerfs_debugfs_caps);
DEFINE_SHOW_ATTRIBUTE(powerfs_debugfs_inodes);
DEFINE_SHOW_ATTRIBUTE(powerfs_debugfs_dentries);
DEFINE_SHOW_ATTRIBUTE(powerfs_debugfs_leases);

/*
 * powerfs_debugfs_init - 在 fill_super 中调用, 创建 debugfs 目录和文件.
 *
 * 目录结构: /sys/kernel/debug/powerfs/<sb_id>/
 *   status    - 挂载状态总览
 *   caps      - 所有 inode cap 状态
 *   inodes    - 所有 inode 关键属性
 *   dentries  - dentry lease 列表
 *   leases    - 目录 lease 状态
 */
static void powerfs_debugfs_init(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    char name[32];

    /* 用超级块地址作为唯一标识 (同  用 client->debugfs_dir) */
    snprintf(name, sizeof(name), "sb-%p", sb);

    sbi->debugfs_dir = debugfs_create_dir(name, NULL);
    if (IS_ERR_OR_NULL(sbi->debugfs_dir)) {
        pr_warn("powerfs: failed to create debugfs dir\n");
        sbi->debugfs_dir = NULL;
        return;
    }

    debugfs_create_file("status",   0400, sbi->debugfs_dir, sb, &powerfs_debugfs_status_fops);
    debugfs_create_file("caps",     0400, sbi->debugfs_dir, sb, &powerfs_debugfs_caps_fops);
    debugfs_create_file("inodes",   0400, sbi->debugfs_dir, sb, &powerfs_debugfs_inodes_fops);
    debugfs_create_file("dentries", 0400, sbi->debugfs_dir, sb, &powerfs_debugfs_dentries_fops);
    debugfs_create_file("leases",   0400, sbi->debugfs_dir, sb, &powerfs_debugfs_leases_fops);

    pr_info("powerfs: debugfs entries at /sys/kernel/debug/%s/\n", name);
}

/*
 * powerfs_debugfs_cleanup - 在 put_super 中调用, 清理 debugfs 目录.
 */
static void powerfs_debugfs_cleanup(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    if (sbi->debugfs_dir) {
        debugfs_remove_recursive(sbi->debugfs_dir);
        sbi->debugfs_dir = NULL;
    }
}

#else /* !CONFIG_DEBUG_FS */

/* CONFIG_DEBUG_FS 未启用时, init/cleanup 为空操作.
 * 前向声明已在文件头部 (非 static inline, 而是 static),
 * 此处提供空函数体. */

static void powerfs_debugfs_init(struct super_block *sb) {}
static void powerfs_debugfs_cleanup(struct super_block *sb) {}

#endif /* CONFIG_DEBUG_FS */

/* ========== P2-8: NFS Export Operations ========== */

/*
 * powerfs_encode_fh - 将 inode 编码为 NFS file handle.
 *
 * 对齐  xxx_encode_fh (export.c L94):
 *   - 无 parent: fh = {ino} (FILEID_INO32_GEN)
 *   - 有 parent: fh = {ino, parent_ino} (FILEID_INO32_GEN_PARENT)
 *
 * PowerFS file handle 只用 inode number (u64), 简洁可靠。
 */
static int powerfs_encode_fh(struct inode *inode, __u32 *fh, int *max_len,
                              struct inode *parent)
{
    int type;

    if (parent && (*max_len < 4)) {
        *max_len = 4;
        return FILEID_INVALID;
    } else if (*max_len < 2) {
        *max_len = 2;
        return FILEID_INVALID;
    }

    if (parent) {
        u64 ino = inode->i_ino;
        u64 pino = parent->i_ino;
        fh[0] = (__u32)(ino & 0xFFFFFFFF);
        fh[1] = (__u32)(ino >> 32);
        fh[2] = (__u32)(pino & 0xFFFFFFFF);
        fh[3] = (__u32)(pino >> 32);
        *max_len = 4;
        type = FILEID_INO32_GEN_PARENT;
    } else {
        u64 ino = inode->i_ino;
        fh[0] = (__u32)(ino & 0xFFFFFFFF);
        fh[1] = (__u32)(ino >> 32);
        *max_len = 2;
        type = FILEID_INO32_GEN;
    }
    return type;
}

/*
 * powerfs_fh_to_dentry - 从 NFS file handle 恢复 dentry.
 *
 * 对齐  __fh_to_dentry (export.c L189):
 *   1. 从 fid 提取 ino
 *   2. powerfs_iget 查找/创建 inode (若不在 icache 则需网络 getattr)
 *   3. d_obtain_alias 关联 dentry
 */
static struct dentry *powerfs_fh_to_dentry(struct super_block *sb,
                                            struct fid *fid,
                                            int fh_len, int fh_type)
{
    struct inode *inode;
    u64 ino;

    if (fh_type != FILEID_INO32_GEN &&
        fh_type != FILEID_INO32_GEN_PARENT)
        return NULL;

    if (fh_len < 2)
        return NULL;

    ino = (u64)fid->raw[0] | ((u64)fid->raw[1] << 32);

    /* iget5_locked 先查 icache; 若 I_NEW 则需 fill_super 的 read_inode 路径
     * 填充. NFS export 场景 inode 通常已在 icache (之前被访问过). */
    inode = powerfs_iget(sb, ino);
    if (IS_ERR(inode))
        return ERR_CAST(inode);

    if (is_bad_inode(inode)) {
        iput(inode);
        return ERR_PTR(-ESTALE);
    }

    return d_obtain_alias(inode);
}

/*
 * powerfs_fh_to_parent - 从 NFS file handle 恢复父目录 dentry.
 */
static struct dentry *powerfs_fh_to_parent(struct super_block *sb,
                                            struct fid *fid,
                                            int fh_len, int fh_type)
{
    struct inode *inode;
    u64 pino;

    if (fh_type != FILEID_INO32_GEN_PARENT)
        return NULL;

    if (fh_len < 4)
        return NULL;

    pino = (u64)fid->raw[2] | ((u64)fid->raw[3] << 32);

    inode = powerfs_iget(sb, pino);
    if (IS_ERR(inode))
        return ERR_CAST(inode);

    if (is_bad_inode(inode)) {
        iput(inode);
        return ERR_PTR(-ESTALE);
    }

    return d_obtain_alias(inode);
}

/*
 * powerfs_get_parent - 获取 dentry 的父目录 (NFS exportfs 用).
 *
 * 对齐  xxx_get_parent (export.c L369):
 *   PowerFS 简化: 用 dentry->d_parent 直接获取 (不做 RPC).
 *   适用于本地 dcache 有父 dentry 的场景. 若 d_parent == root 则返回 -ESTALE.
 */
static struct dentry *powerfs_get_parent(struct dentry *child)
{
    struct dentry *parent;

    if (IS_ROOT(child))
        return ERR_PTR(-ESTALE);

    parent = dget_parent(child);
    return parent;
}

static const struct export_operations powerfs_export_ops = {
    .encode_fh     = powerfs_encode_fh,
    .fh_to_dentry  = powerfs_fh_to_dentry,
    .fh_to_parent  = powerfs_fh_to_parent,
    .get_parent    = powerfs_get_parent,
};

/* ========== 全局 slab 缓存初始化/销毁 ========== */

int powerfs_init_inode_cache(void)
{
    /* inode slab 缓存 */
    powerfs_inode_cachep = kmem_cache_create(
        "powerfs_inode_cache",
        sizeof(struct powerfs_inode_info),
        __alignof__(struct powerfs_inode_info),
        SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
        powerfs_inode_init_once);
    if (!powerfs_inode_cachep)
        return -ENOMEM;

    /* dentry_info slab 缓存 */
    powerfs_dentry_cachep = kmem_cache_create(
        "powerfs_dentry_cache",
        sizeof(struct powerfs_dentry_info),
        __alignof__(struct powerfs_dentry_info),
        SLAB_RECLAIM_ACCOUNT,
        NULL);
    if (!powerfs_dentry_cachep) {
        kmem_cache_destroy(powerfs_inode_cachep);
        powerfs_inode_cachep = NULL;
        return -ENOMEM;
    }

    pr_debug("powerfs: slab caches created\n");
    return 0;
}

void powerfs_destroy_inode_cache(void)
{
    if (powerfs_dentry_cachep) {
        kmem_cache_destroy(powerfs_dentry_cachep);
        powerfs_dentry_cachep = NULL;
    }
    if (powerfs_inode_cachep) {
        kmem_cache_destroy(powerfs_inode_cachep);
        powerfs_inode_cachep = NULL;
    }
    pr_debug("powerfs: slab caches destroyed\n");
}

/*
 * powerfs_set_sb_dentry_ops - 设置超级块的默认 dentry 操作
 *
 * 6.17 内核使用 set_default_d_op() 替代直接赋值 sb->s_d_op
 */
void powerfs_set_sb_dentry_ops(struct super_block *sb)
{
    set_default_d_op(sb, &powerfs_dentry_operations);
}

/* ========== fill_super: 填充超级块 (fs_context 风格) ========== */

int powerfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct powerfs_ctx_simple {
        char master_addr[64];
        u16  master_port;
        u16  shard_count;
        u32  write_batch_kb;
        char ca_crt[512];
        char client_crt[512];
        char client_key[512];
    };
    /* 注意: sget_fc() 会将 fc->s_fs_info 转移到 sb->s_fs_info, 然后将
     * fc->s_fs_info 置 NULL. 因此必须从 sb->s_fs_info 获取 ctx, 而不是
     * 从 fc->s_fs_info 获取 (后者已经是 NULL).
     * 参考: fs/super.c sget_fc() 第 566-574 行 */
    struct powerfs_ctx_simple *ctx = sb->s_fs_info;
    struct powerfs_sb_info *sbi;
    struct inode *root;
    u32 batch_kb = POWERFS_WRITE_BATCH_DEFAULT_KB;
    int ret;

    /* 创建超级块私有信息 */
    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sbi->sb = sb;

    /* 保存挂载参数 (ctx 来自 init_fs_context 通过 sget_fc 转移到 sb->s_fs_info) */
    if (ctx) {
        strncpy(sbi->master_addr, ctx->master_addr, sizeof(sbi->master_addr) - 1);
        sbi->master_port = ctx->master_port;
        sbi->shard_count  = ctx->shard_count;
        batch_kb = ctx->write_batch_kb;
        /* 证书路径先保存到 sbi, 下方 powerfs_client 初始化时再从 sbi
         * 拷贝到 client->ca_crt/client_crt/client_key. 必须在 kfree(ctx)
         * 之前完成拷贝, 否则路径字符串丢失. */
        strncpy(sbi->ca_crt,     ctx->ca_crt,     sizeof(sbi->ca_crt) - 1);
        strncpy(sbi->client_crt, ctx->client_crt, sizeof(sbi->client_crt) - 1);
        strncpy(sbi->client_key, ctx->client_key, sizeof(sbi->client_key) - 1);
        sbi->ca_crt[sizeof(sbi->ca_crt) - 1]             = '\0';
        sbi->client_crt[sizeof(sbi->client_crt) - 1]     = '\0';
        sbi->client_key[sizeof(sbi->client_key) - 1]     = '\0';
        /* 释放 init_fs_context 分配的 ctx, 释放后 sb->s_fs_info 仍指向已释放内存,
         * 必须在下方设置 sb->s_fs_info = sbi 之前完成 */
        kfree(ctx);
        ctx = NULL;
    }

    /* 挂载时必须传 master_addr. 不能再从 module_param 全局默认值回退:
     * 多个 mount point 会冲突, 也不允许静默连到 "默认集群" 造成跨租户事故. */
    if (sbi->master_addr[0] == '\0') {
        pr_err("powerfs: missing required mount option 'master_addr='. "
               "Usage: mount -t powerfs none /mnt -o master_addr=h1,h2,h3[,master_port=P,shard_count=N]\n");
        kfree(sbi);
        return -EINVAL;
    }
    if (sbi->shard_count == 0) {
        pr_err("powerfs: shard_count=0 invalid, must be >=1\n");
        kfree(sbi);
        return -EINVAL;
    }
    if (sbi->master_port == 0)
        sbi->master_port = 9334;

    pr_debug("powerfs: fill_super master_addr='%s' master_port=%u shard_count=%u\n",
             sbi->master_addr, (unsigned)sbi->master_port,
             (unsigned)sbi->shard_count);

    /* 验证并转换 write_batch_kb → write_batch_pages.
     * 范围: 4KB (1 page) ~ 64MB (stripe size, 16384 pages).
     * 越界值 clamp 到合法范围并告警, 不拒绝挂载 (避免配置笔误导致不可用). */
    if (batch_kb < POWERFS_WRITE_BATCH_MIN_KB) {
        pr_warn("powerfs: write_batch_kb=%u too small, clamped to %d\n",
                batch_kb, POWERFS_WRITE_BATCH_MIN_KB);
        batch_kb = POWERFS_WRITE_BATCH_MIN_KB;
    } else if (batch_kb > POWERFS_WRITE_BATCH_MAX_KB) {
        pr_warn("powerfs: write_batch_kb=%u too large, clamped to %d\n",
                batch_kb, POWERFS_WRITE_BATCH_MAX_KB);
        batch_kb = POWERFS_WRITE_BATCH_MAX_KB;
    }
    /* 转换为页数: kb * 1024 / PAGE_SIZE = kb / (PAGE_SIZE/1024) = kb / 4 */
    sbi->write_batch_pages = (int)(batch_kb * 1024 / PAGE_SIZE);
    /* K1-5 fix: 确保 batch 至少能容纳一个 chunk 的所有页面,
     * 避免 same-needle 的 RMW 并发覆盖. chunk_size=1MB → 256 页. */
    {
        int min_pages = (int)(POWERFS_CHUNK_SIZE / PAGE_SIZE);
        if (sbi->write_batch_pages < min_pages)
            sbi->write_batch_pages = min_pages;
    }
    pr_debug("powerfs: write_batch_kb=%u → write_batch_pages=%d\n",
            batch_kb, sbi->write_batch_pages);

    /* 初始化 inode 号分配器 (从 100 开始，1 是 root) */
    atomic_set(&sbi->next_ino, 100);

    /* P3-5: 分配并初始化 powerfs_client (对齐  xxx_fs_client).
     * client 承载全局性能计数、dentry lease LRU、cap LRU、client_id 等.
     * 之前 client 始终为 NULL, 导致 dentry lease 链表/cap LRU/metrics 全部失效. */
    sbi->client = kzalloc(sizeof(*sbi->client), GFP_KERNEL);
    if (!sbi->client) {
        pr_err("powerfs: failed to allocate powerfs_client\n");
        kfree(sbi);
        return -ENOMEM;
    }
    sbi->client->sb = sb;
    sbi->client->mount_state = POWERFS_MOUNT_MOUNTING;
    sbi->client->blocklisted = false;
    sbi->client->max_file_size = MAX_LFS_FILESIZE;
    atomic_long_set(&sbi->client->writeback_count, 0);
    sbi->client->write_congested = false;
    sbi->client->filp_gen = 0;
    memcpy(sbi->client->master_addr, sbi->master_addr, sizeof(sbi->master_addr));
    sbi->client->master_port = sbi->master_port;
    /* 证书路径: fill_super 阶段暂存在 sbi 上, 这里复制到 client.
     * RegisterClient/DeregisterClient 都以 client 指针为入口读取路径. */
    memcpy(sbi->client->ca_crt,     sbi->ca_crt,     sizeof(sbi->ca_crt));
    memcpy(sbi->client->client_crt, sbi->client_crt, sizeof(sbi->client_crt));
    memcpy(sbi->client->client_key, sbi->client_key, sizeof(sbi->client_key));
    /* client_id: "powerfs-kernel-<tgid>" — 唯一标识本 mount, 用于 Cap recall */
    {
        int id_len = snprintf(sbi->client->client_id, sizeof(sbi->client->client_id),
                              "powerfs-kernel-%d", task_tgid_vnr(current));
        sbi->client->client_id_len = (size_t)id_len;
    }
    INIT_LIST_HEAD(&sbi->client->dentry_lease_list);
    spin_lock_init(&sbi->client->dentry_lease_lock);
    INIT_LIST_HEAD(&sbi->client->cap_lru_list);
    spin_lock_init(&sbi->client->cap_lru_lock);
    INIT_LIST_HEAD(&sbi->client->cap_flush_list);
    spin_lock_init(&sbi->client->cap_flush_lock);
    mutex_init(&sbi->client->mount_mutex);
    ret = powerfs_metrics_init(&sbi->client->metrics);
    if (ret) {
        pr_err("powerfs: powerfs_metrics_init failed: %d\n", ret);
        kfree(sbi->client);
        sbi->client = NULL;
        kfree(sbi);
        return ret;
    }
    pr_info("powerfs: client allocated, client_id=%s\n", sbi->client->client_id);

    /* 设置超级块 (覆盖 sb->s_fs_info, 之前指向已释放的 ctx) */
    sb->s_fs_info = sbi;
    sb->s_op = &powerfs_super_ops;
    sb->s_export_op = &powerfs_export_ops;  /* P2-8: NFS export 支持 */
    sb->s_xattr = powerfs_xattr_handlers;
    sb->s_magic = POWERFS_SUPER_MAGIC;
    sb->s_blocksize = 4096;
    sb->s_blocksize_bits = 12;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_time_gran = 1;

    /* Stage C: 设置 BDI 支持 writeback.
     *
     * super_setup_bdi 创建独立的 backing_dev_info 并设置 BDI_CAP_WRITEBACK.
     * 没有 BDI_CAP_WRITEBACK, mapping_can_writeback() 返回 false,
     * folio_account_dirtied 不增加 dirty 统计, writeback 子系统不扫描
     * dirty folio, 导致 sync/fsync 不触发 writepage, 数据无法持久化.
     *
     * 参考: xxx_fill_super (fs/xxx/super.c) 调用 super_setup_bdi.
     * ramfs 不需要 (纯内存, 无 writeback). */
    ret = super_setup_bdi(sb);
    if (ret) {
        pr_err("powerfs: super_setup_bdi failed: %d\n", ret);
        kfree(sbi);
        return ret;
    }

    /* 设置默认 dentry operations (所有 dentry 共享) */
    powerfs_set_sb_dentry_ops(sb);

    /* 创建根目录 inode */
    root = powerfs_create_root(sb);
    if (!root) {
        kfree(sbi);
        sb->s_fs_info = NULL;
        return -ENOMEM;
    }

    /* 创建根 dentry */
    sb->s_root = d_make_root(root);
    if (!sb->s_root) {
        iput(root);
        kfree(sbi);
        sb->s_fs_info = NULL;
        return -ENOMEM;
    }

    sbi->initialized = true;
    sbi->shutting_down = false;
    if (sbi->client)
        sbi->client->mount_state = POWERFS_MOUNT_MOUNTED;

    /* 设置全局超级块指针 (供通信层使用) */
    g_powerfs_sb = sb;

    /* Stage C: 创建 writeback 异步 workqueue.
     * 使用 WQ_UNBOUND 提高扩展性 (work 不绑定到特定 CPU).
     * max_active=4 限制并发: powerfs_net_write 是同步网络调用, 过多并发
     * worker 会压垮单连接 (256 页 1MB 文件曾导致 132 个 worker 线程锁死). */
    sbi->writeback_wq = alloc_workqueue("powerfs_wb",
                                        WQ_UNBOUND | WQ_MEM_RECLAIM, 4);
    if (!sbi->writeback_wq) {
        pr_err("powerfs: failed to create writeback workqueue\n");
        /* 注意: 此处不清理, kill_sb 会处理. */
        return -ENOMEM;
    }
    atomic_set(&sbi->wb_in_flight, 0);

    /* 创建异步 inode 刷新工作队列 (NOTIFY → getattr 刷新元数据).
     * max_active=4: 允许多个 refresh work 并发执行, 避免单个 ilookup5 阻塞
     * (等待 I_FREEING inode) 导致所有 NOTIFY 处理积压.
     * WQ_MEM_RECLAIM: 确保 memory reclaim 路径可以提交 work. */
    powerfs_refresh_wq = alloc_workqueue("powerfs_refresh",
                                          WQ_UNBOUND | WQ_MEM_RECLAIM, 4);
    if (!powerfs_refresh_wq) {
        pr_err("powerfs: failed to create refresh workqueue\n");
        return -ENOMEM;
    }

    /* Stage C+: 注册 Cap NOTIFY 回调 (Filer→Client async push).
     * refresh_wq 必须已创建 (handlers 内部 queue_work 到它).
     * 注册后 RX dispatcher 收到 CapRecallNotify / CapUpgradeNotify 会
     * 派发到 fs 层 powerfs_cap_recall_notify_handler / upgrade handler. */
    powerfs_net_reg_cap_notify_handlers(powerfs_cap_recall_notify_handler,
                                        powerfs_cap_upgrade_notify_handler);
    pr_info("powerfs: cap notify handlers registered via powerfs-net dispatcher\n");

    /* Phase 3: lease 续约 workqueue.
     * WQ_UNBOUND: 不绑定 CPU, 允许调度器自由调度.
     * WQ_MEM_RECLAIM: 参考 nfsiod/GFS2, 内存回收路径可用.
     * 不使用 WQ_HIGHPRI: 大量 lease 续约 (50+ 文件) 会饥饿其他工作队列
     *   (writeback/refresh), 导致 workqueue lockup + RCU stall.
     *   lease 续约不是延迟敏感的 — 过期前 10s 续约即可, 延迟 1-2s 可接受.
     * max_active=4: 限制并发续约数, 防止网络 I/O 阻塞淹没 workqueue. */
    sbi->lease_wq = alloc_workqueue("powerfs_lease",
                                     WQ_UNBOUND | WQ_MEM_RECLAIM, 4);
    if (!sbi->lease_wq) {
        pr_err("powerfs: failed to create lease workqueue\n");
        return -ENOMEM;
    }

    /* P1-1: 创建 per-sb slab caches (对齐  init_caches() — cap/cap_snap/cap_flush).
     * 注意: inode_cache/dentry_cachep 是模块级全局 (powerfs_init_inode_cache 创建),
     * cap/cap_snap/cap_flush 是 per-sb (每个挂载独立, 支持多挂载互不干扰). */
    sbi->cap_cachep = kmem_cache_create(
        "powerfs_cap",
        sizeof(struct powerfs_cap),
        __alignof__(struct powerfs_cap),
        SLAB_RECLAIM_ACCOUNT,
        NULL);
    if (!sbi->cap_cachep) {
        pr_err("powerfs: failed to create cap_cachep\n");
        return -ENOMEM;
    }
    sbi->cap_snap_cachep = kmem_cache_create(
        "powerfs_cap_snap",
        sizeof(struct powerfs_cap_snap),
        __alignof__(struct powerfs_cap_snap),
        SLAB_RECLAIM_ACCOUNT,
        NULL);
    if (!sbi->cap_snap_cachep) {
        pr_err("powerfs: failed to create cap_snap_cachep\n");
        return -ENOMEM;
    }
    sbi->cap_flush_cachep = kmem_cache_create(
        "powerfs_cap_flush",
        sizeof(struct powerfs_cap_flush),
        __alignof__(struct powerfs_cap_flush),
        SLAB_RECLAIM_ACCOUNT,
        NULL);
    if (!sbi->cap_flush_cachep) {
        pr_err("powerfs: failed to create cap_flush_cachep\n");
        return -ENOMEM;
    }

    /* === 初始化 powerfs_net 连接池 (多节点 Delta Sync) === */
    powerfs_net_pool_init();

    /* 配置 Filer 节点 — 全部通过 Master 动态发现.
     *
     * 架构: 只需配置 master_addr (3 个 Raft 节点), Filer 和 Volume 地址
     * 全部通过 Master 的 GetTopology / ListFilers 动态发现.
     * 不再支持手动配置 filer_addr / volume_addr.
     *
     * 流程:
     *   1. 添加 Master 到连接池 (discover_filers/volumes 需要)
     *   2. powerfs_net_discover_filers 从 Master 获取 filer 列表
     *   3. powerfs_net_discover_volumes 从 Master 获取 volume 路由表 */
    {
        const char *maddr = (sbi->master_addr[0]) ? sbi->master_addr : NULL;
        __u16 mport = sbi->master_port ? sbi->master_port : 9334;

        /* 添加 Master 到连接池 */
        if (maddr) {
            char mbuf[256];
            char *mp, *mtok;
            strncpy(mbuf, maddr, sizeof(mbuf) - 1);
            mbuf[sizeof(mbuf) - 1] = '\0';
            mp = mbuf;
            while ((mtok = strsep(&mp, ",")) != NULL) {
                while (*mtok == ' ') mtok++;
                if (mtok[0] == '\0') continue;
                powerfs_net_add_server(mtok, mport,
                                      POWERFS_NET_SERVER_MASTER);
                pr_debug("powerfs: added master %s:%u\n", mtok, mport);
            }
        } else {
            pr_err("powerfs: master_addr not configured, cannot mount\n");
            return -EINVAL;
        }

        /* ===== RegisterClient: 向 Master 注册 client_uuid + 黑名单检查
         * 在 Master 地址建立后、filer/volume 路由使用前调用.
         * 成功 -> 保存 assigned_client_id(u64) + client_uuid 字符串;
         * 黑名单拒绝 -> 挂载失败返回 -EPERM 并打印拒绝理由. */
        {
            char reason_buf[256];
            __u64 assigned_id = 0;
            bool mount_allowed = false;
            const char *host_str = "kernel";
            char host_buf[65];
            int rc;

            /* 1. 生成 client_uuid: 单 mount 唯一, 无需 crash 持久化.
             *    snprintf 格式 "pwfs-k-<tgid>-<jiffies_64%hex>",
             *    组合 (进程 ID + 启动后 64 位时钟 tick) 实际唯一. */
            {
                int uuid_len = snprintf(sbi->client->client_uuid,
                                        sizeof(sbi->client->client_uuid),
                                        "pwfs-k-%d-%llx",
                                        task_tgid_vnr(current),
                                        (unsigned long long)get_jiffies_64());
                if (uuid_len < 0 ||
                    (size_t)uuid_len >= sizeof(sbi->client->client_uuid)) {
                    strncpy(sbi->client->client_uuid,
                            "pwfs-k-fallback",
                            sizeof(sbi->client->client_uuid) - 1);
                    sbi->client->client_uuid[sizeof(sbi->client->client_uuid) - 1] = '\0';
                }
            }

            /* 2. host: 优先 uts nodename; 失败 fallback 到 "kernel" */
            {
                struct new_utsname *uts = init_utsname();
                if (uts && uts->nodename[0]) {
                    size_t nn_len = strlen(uts->nodename);
                    if (nn_len > sizeof(host_buf) - 1)
                        nn_len = sizeof(host_buf) - 1;
                    memcpy(host_buf, uts->nodename, nn_len);
                    host_buf[nn_len] = '\0';
                    host_str = host_buf;
                }
            }

            reason_buf[0] = '\0';
            rc = powerfs_net_register_client(maddr, mport,
                                             sbi->client->client_uuid,
                                             "kernel",
                                             "/mnt/powerfs",
                                             "default",
                                             "none",
                                             host_str,
                                             (__u64)task_tgid_vnr(current),
                                             sbi->client->client_crt[0] ?
                                                 sbi->client->client_crt : NULL,
                                             &assigned_id,
                                             &mount_allowed,
                                             reason_buf,
                                             sizeof(reason_buf));
            if (rc != 0) {
                pr_err("powerfs: register_client RPC failed (%d), "
                       "abort mount\n", rc);
                return rc;
            }
            if (!mount_allowed) {
                if (reason_buf[0])
                    pr_err("powerfs: mount denied by master blacklist: %s\n",
                           reason_buf);
                else
                    pr_err("powerfs: mount denied by master blacklist\n");
                return -EPERM;
            }
            sbi->client->assigned_client_id = assigned_id;
            /* Cap RPC (open_grant / recall_ack / release) 发送 STRING
             * holder 给 filer lock_arbiter 作为 session key. 将 master
             * 分配的 numeric id 字符串化后写入 client_id[], 确保 cap
             * 操作一致使用 master-assigned id:  assigned=42 → "42".
             * get_mount_client_id() 直接读取此字符串, 无需后续改动. */
            {
                int cid_len = snprintf(sbi->client->client_id,
                                      sizeof(sbi->client->client_id),
                                      "%llu",
                                      (unsigned long long)assigned_id);
                if (cid_len > 0 &&
                    (size_t)cid_len < sizeof(sbi->client->client_id))
                    sbi->client->client_id_len = (size_t)cid_len;
                else
                    pr_warn("powerfs: failed to stringify assigned_id=%llu, "
                            "keeping legacy client_id=%s\n",
                            (unsigned long long)assigned_id,
                            sbi->client->client_id);
            }
            pr_info("powerfs: mount registered with master, "
                    "assigned_client_id=%llu (holder=%s) client_uuid=%s\n",
                    (unsigned long long)assigned_id,
                    sbi->client->client_id,
                    sbi->client->client_uuid);
        }

        /* 从 Master 发现 filer 列表 */
        {
            int discovered = powerfs_net_discover_filers(maddr, mport);
            if (discovered > 0) {
                pr_info("powerfs: discovered %d filers via Master\n",
                        discovered);
            } else {
                pr_err("powerfs: Master filer discovery failed (%d), "
                       "cannot mount without filers\n", discovered);
                return -ENOLINK;
            }
        }
    }

    /* 初始化新连接池.
     *
     * 新架构 (per-conn 状态机 + shard 路由 + 事件驱动) 为唯一路径.
     * 断连检测由 sk_state_change 回调 + TCP keepalive 取代, 无需健康监控线程.
     *
     * 参数全来自 per-mount sbi (由 mount -o 选项决定). */
    {
        const char *maddr = sbi->master_addr[0] ? sbi->master_addr : NULL;
        __u16 mport = sbi->master_port ? sbi->master_port : 9334;
        __u16 scount = sbi->shard_count ? sbi->shard_count : 3;
        int pool_ret = powerfs_conn_pool_init(maddr, mport, scount);
        if (pool_ret != 0) {
            pr_err("powerfs: connection pool init failed (%d)\n", pool_ret);
            return pool_ret;
        }
        pr_info("powerfs: connection pool init success (master=%s:%u shard_count=%u)\n",
                maddr ? maddr : "(null)", (unsigned)mport, (unsigned)scount);

        /* === B1: 启动 KeepConnected 周期心跳 (对齐 FUSE 30s) ===
         * 必须在 conn_pool_init (创建 reconn_wq) + register_client (拿到
         * assigned_client_id + client_uuid) 都完成后启动. 参数严格对齐
         * 上面 register_client 调用参数, 保证 heartbeat 和注册信息一致. */
        {
            const char *hb_host_str = "kernel";
            char hb_host_buf[65];
            struct new_utsname *uts = init_utsname();
            int hb_rc;
            if (uts && uts->nodename[0]) {
                size_t nn_len = strlen(uts->nodename);
                if (nn_len > sizeof(hb_host_buf) - 1)
                    nn_len = sizeof(hb_host_buf) - 1;
                memcpy(hb_host_buf, uts->nodename, nn_len);
                hb_host_buf[nn_len] = '\0';
                hb_host_str = hb_host_buf;
            }
            hb_rc = powerfs_net_start_heartbeat(
                        sbi->client->client_uuid,
                        "kernel",
                        "/mnt/powerfs",
                        "default",
                        "none",
                        hb_host_str,
                        (__u64)task_tgid_vnr(current),
                        sbi->client->assigned_client_id,
                        sbi->client->client_crt[0] ? sbi->client->client_crt : NULL);
            if (hb_rc < 0)
                pr_warn("powerfs: start_heartbeat failed (%d), "
                        "heartbeat skipped (may cause 55s filer EAGAIN)\n", hb_rc);
        }

        /* 从 Master GetTopology 发现 volume 路由表 (volume_id → conn_idx).
         * 失败不挂载失败: filer 元数据仍可用, 数据读写等 volume 上线后恢复. */
        if (maddr) {
            int vol_ret = powerfs_net_discover_volumes(maddr, mport);
            if (vol_ret < 0) {
                pr_warn("powerfs: discover_volumes failed: %d "
                        "(volume data IO will fail until routes established)\n",
                        vol_ret);
            } else {
                pr_debug("powerfs: volume routes discovered via Master\n");
            }
        }
    }

    pr_debug("powerfs: fill_super done, root ino=%lu\n", root->i_ino);
    pr_debug("powerfs: powerfs_net pool initialized (Delta Sync ready)\n");

    /* P3-4: 创建 debugfs 状态导出入口 */
    powerfs_debugfs_init(sb);

    /* P3-5: 创建 /proc/powerfs/<sb>/ 性能计数入口 */
    powerfs_proc_init(sb);

    return 0;
}

/* ========== kill_sb_super: 卸载清理 ========== */

void powerfs_kill_sb_super(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    pr_debug("powerfs: kill_sb_super\n");

    /* 清除全局超级块指针 */
    if (g_powerfs_sb == sb)
        g_powerfs_sb = NULL;

    /* 1. 先 sync 脏 inode (网络仍可用, write_inode 能同步 size 到 Filer,
     *    writepage 异步 work 也会执行网络写).
     *    如果先关闭网络, write_inode 的 setattr 会失败, inode 保持 dirty,
     *    evict_inodes 无法驱逐, 导致 umount 挂起或内存泄漏. */
    sync_filesystem(sb);

    /* 1a. DeregisterClient: 向 Master 优雅下线 (最佳努力, 忽略错误).
     *    在 sync 之后、关闭网络/销毁队列 之前执行, 确保网络栈正常.
     *    deregister_client 内部使用独立 raw socket 直连 Master, 不依赖
     *    g_pool 连接池, 因此在 powerfs_net_set_stopping/pool_cleanup 之前
     *    之后都能工作, 但这里网络最健康. */
    if (sbi && sbi->client && sbi->client->client_uuid[0] &&
        sbi->client->master_addr[0]) {
        int dr = powerfs_net_deregister_client(sbi->client->master_addr,
                                               sbi->client->master_port,
                                               sbi->client->client_uuid,
                                               sbi->client->assigned_client_id,
                                               sbi->client->client_crt[0] ?
                                                   sbi->client->client_crt : NULL);
        if (dr == 0)
            pr_info("powerfs: umount deregistered client "
                    "(assigned_id=%llu, uuid=%s)\n",
                    (unsigned long long)sbi->client->assigned_client_id,
                    sbi->client->client_uuid);
        else
            pr_warn("powerfs: umount deregister_client failed (%d); "
                    "master will evict entry on heartbeat timeout\n", dr);
    }

    /* 1a-2. B1: 停止 KeepConnected 周期心跳.
     *         在 deregister_client 之后、pool_exit(reconn_wq 销毁) 之前. */
    powerfs_net_stop_heartbeat();

    /* 1b. Phase 3: 设置 shutting_down 标志.
     *    lease_renew_work_func 检查此标志, 避免在 destroy_workqueue(lease_wq)
     *    期间重新排队导致 flush 循环. 必须在销毁 lease_wq 之前设置. */
    if (sbi)
        sbi->shutting_down = true;

    /* 2. 销毁 writeback workqueue (Stage C).
     *    sync_filesystem 已触发 writeback 并等待 PageWriteback 清除,
     *    此时 workqueue 中所有 work 应已完成. destroy_workqueue 会
     *    drain 剩余 work (若有), 然后销毁. 必须在关闭网络前销毁,
     *    否则 work_fn 中的 powerfs_net_write 访问已关闭的网络. */
    if (sbi && sbi->writeback_wq) {
        destroy_workqueue(sbi->writeback_wq);
        sbi->writeback_wq = NULL;
    }
    /* 等待所有 wpw 的 call_rcu 回调完成 (work_struct 延迟释放).
     * destroy_workqueue 已确保 work_fn 全部执行完, 但 wpw 内存通过
     * call_rcu 异步释放. rcu_barrier 确保卸载时无残留内存. */
    rcu_barrier();

    /* 2b. 销毁 lease 续约工作队列.
     *    必须在关闭网络前销毁 (renew_work_fn 发网络请求).
     *    destroy_workqueue 会 flush 所有 pending delayed_work,
     *    之后 evict_inode 的 cancel_delayed_work_sync 仅取消定时器, 安全. */
    if (sbi && sbi->lease_wq) {
        destroy_workqueue(sbi->lease_wq);
        sbi->lease_wq = NULL;
    }

    /* 2c. 销毁 inode 刷新工作队列 (NOTIFY → getattr) */
    if (powerfs_refresh_wq) {
        destroy_workqueue(powerfs_refresh_wq);
        powerfs_refresh_wq = NULL;
    }
    /* 等待 refresh_work 的 call_rcu 回调完成 (与 wpw 相同的延迟释放模式) */
    rcu_barrier();

    /* 3. 设置 stopping 标志: 让 send_request 立即返回 -ENOTCONN,
     *    阻止 reconnect_work 在 g_pool 清零后访问野指针. */
    powerfs_net_set_stopping();

    /* 4. 关闭所有连接 (g_pool 会被清零) */
    powerfs_net_pool_cleanup();

    /* 4b. P1-1 防御式清理: put_super 理论上先执行并销毁 cap caches，
     *     但 mount 中途失败 (fill_super 报错→kill_sb) 时 put_super 不会被调用，
     *     这里重复销毁 (kmem_cache_destroy(NULL) 安全，重复 destroy 也安全)。 */
    if (sbi) {
        if (sbi->cap_flush_cachep) {
            kmem_cache_destroy(sbi->cap_flush_cachep);
            sbi->cap_flush_cachep = NULL;
        }
        if (sbi->cap_snap_cachep) {
            kmem_cache_destroy(sbi->cap_snap_cachep);
            sbi->cap_snap_cachep = NULL;
        }
        if (sbi->cap_cachep) {
            kmem_cache_destroy(sbi->cap_cachep);
            sbi->cap_cachep = NULL;
        }
    }

    if (sbi) {
        if (sbi->client) {
            sbi->client->mount_state = POWERFS_MOUNT_UNMOUNTING;
            powerfs_metrics_destroy(&sbi->client->metrics);
            kfree(sbi->client);
            sbi->client = NULL;
        }
        kfree(sbi);
        sb->s_fs_info = NULL;
    }

    /* 5. kill_anon_super 会再次 sync (但已无脏数据) + shrink_dcache + evict_inodes */
    kill_anon_super(sb);
}

/*
 * powerfs_create_root - 创建根目录 inode
 */
struct inode *powerfs_create_root(struct super_block *sb)
{
    struct powerfs_inode_info *pi;
    struct inode *root;

    root = powerfs_new_inode(sb, S_IFDIR | 0755,
                              POWERFS_ROOT_INO, 0, "/");
    if (!root)
        return NULL;

    pi = POWERFS_I(root);

    /* 根目录的父目录是自己.
     * dir_complete=false: first readdir fetches from Filer to get any
     * pre-existing entries. The root may have files from previous mounts. */
    pi->parent_ino = POWERFS_ROOT_INO;
    WRITE_ONCE(pi->dir_complete, false);
    WRITE_ONCE(pi->dir_lease_expire, 0);

    /* 根目录设置 uid/gid 为 0 */
    root->i_uid = GLOBAL_ROOT_UID;
    root->i_gid = GLOBAL_ROOT_GID;

    pr_debug("powerfs: root inode created, ino=%lu\n", root->i_ino);
    return root;
}
