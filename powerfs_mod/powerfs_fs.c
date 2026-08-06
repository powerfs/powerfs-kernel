/*
 * PowerFS 内核文件系统 - VFS 文件系统操作实现
 *
 * 参考:
 *   - cephfs (fs/ceph/dir.c, fs/ceph/inode.c) - 网络文件系统架构
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
#include <linux/pagevec.h>       /* pagevec_lookup_range_tag (writepages 批量遍历) */

#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_net.h"

/* ========== netfs 请求操作 (Stage C: 对接 netfs 子系统) ==========
 *
 * 参照 fs/ceph/addr.c 的 ceph_netfs_issue_read 实现.
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
    __u64 volume_id, file_key;
    int err;

    pr_debug("powerfs: netfs_issue_read ino=%lu start=%llu len=%zu i_size=%llu\n",
            inode->i_ino, (unsigned long long)start, len,
            (unsigned long long)rreq->i_size);

    /* 超出文件大小的部分: 由 netfs 处理 (设置 NETFS_SREQ_CLEAR_TAIL),
     * issue_read 只读取有效数据部分 */
    if (start >= rreq->i_size) {
        pr_debug("powerfs: issue_read start >= i_size, skip (start=%llu i_size=%llu)\n",
                (unsigned long long)start, (unsigned long long)rreq->i_size);
        netfs_subreq_terminated(subreq, 0, false);
        return;
    }
    if (start + len > rreq->i_size)
        len = rreq->i_size - start;

    /* 数据直连: 从 inode 获取 volume_id/file_key */
    spin_lock(&pi->i_lock);
    volume_id = pi->volume_id;
    file_key = pi->file_key;
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: issue_read ino=%lu vid=%llu fkey=%llu start=%llu len=%zu\n",
            inode->i_ino, (unsigned long long)volume_id,
            (unsigned long long)file_key, (unsigned long long)start, len);

    /* 基本功能阶段: 同步读取到临时 buffer, 再拷贝到 xarray 中的 folio.
     * 后续优化: 直接从 xarray 映射 folio, 避免额外拷贝 (参照 ceph). */
    buf = kvmalloc(len, GFP_KERNEL);
    if (!buf) {
        netfs_subreq_terminated(subreq, -ENOMEM, false);
        return;
    }

    err = powerfs_net_read(inode->i_ino, volume_id, file_key,
                           start, len, buf, len, &read_len);
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
        netfs_subreq_terminated(subreq, err, false);
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
    netfs_subreq_terminated(subreq, read_len, false);
}

static const struct netfs_request_ops powerfs_netfs_ops = {
    .issue_read = powerfs_netfs_issue_read,
};

/* ========== 全局 slab 缓存 (参考 ceph 全局 cache) ========== */

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
};
static struct workqueue_struct *powerfs_refresh_wq;

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

/* 目录项管理函数 (前向声明) */
static int powerfs_add_dir_entry(struct inode *dir, u64 ino,
                                  unsigned int type, const char *name);
static int powerfs_remove_dir_entry(struct inode *dir, const char *name);
static void powerfs_clear_dir_entries(struct inode *dir);

/* lease 续约 work 函数 (前向声明，Step 2 实现真实续约) */
static void powerfs_lease_renew_work_func(struct work_struct *work);

/* ========== Dentry operations 实现 ========== */

/*
 * d_init - 新 dentry 创建时分配私有数据
 *
 * 参考 ceph_d_init (fs/ceph/dir.c)
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
    di->lease_expire = jiffies + POWERFS_DENTRY_LEASE_TTL;
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
 * 参考 ceph_d_release (fs/ceph/dir.c)
 *
 * 重要: d_fsdata 必须用 call_rcu 延迟释放, 不能裸 kmem_cache_free.
 * 原因: RCU path walk (__d_lookup_rcu + d_revalidate) 可能并发读 d_fsdata.
 */
void powerfs_d_release(struct dentry *dentry)
{
    struct powerfs_dentry_info *di = dentry->d_fsdata;

    if (!di)
        return;

    pr_debug("powerfs: d_release '%pd' (di=%p)\n", dentry, di);

    /* 从 lease 链表移除 (如果在链表中) */
    if (!list_empty(&di->lease_list)) {
        struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dentry->d_sb);
        if (sbi && sbi->client) {
            spin_lock(&sbi->client->dentry_lease_lock);
            list_del_init(&di->lease_list);
            spin_unlock(&sbi->client->dentry_lease_lock);
        }
    }

    /* 设置 d_fsdata = NULL 并通过 RCU 延迟释放 di.
     * RCU reader 在 d_revalidate 中读 d_fsdata 可能看到 NULL (安全:
     * d_revalidate 检查 !di 返回 1) 或旧指针 (仍有效, 因为 di 还没被 free).
     * grace period 后才真正 free, 保证无 UAF. */
    dentry->d_fsdata = NULL;
    call_rcu(&di->rcu, powerfs_di_free_rcu);
}

/*
 * d_revalidate - 验证 dentry 缓存是否仍然有效
 *
 * 参考 ceph_d_revalidate (fs/ceph/dir.c)
 *
 * 这是网络文件系统的核心回调：
 *   - 返回 1: dentry 仍然有效，使用缓存
 *   - 返回 0: dentry 已失效，丢弃缓存重新 lookup
 *   - 返回负值: 错误
 */

/* Phase 1 前置声明: 目录 lease 失效 (定义在 readdir 区段, 但 mknod 等更早使用). */
static void powerfs_invalidate_dir_lease(struct inode *dir);

int powerfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
    struct powerfs_dentry_info *di;
    struct inode *inode;
    struct powerfs_inode_info *pi;

    /* RCU 模式 fast-path: 检查 dentry lease 有效性.
     *
     * 之前始终返回 -ECHILD 退出 RCU 模式, 导致 VFS 频繁切换到非 RCU 模式
     * (d_lookup), 在高并发下 d_lock 竞争加剧, 触发 RCU stall in __d_lookup.
     *
     * 优化: lease 有效时返回 1, VFS 继续在 RCU 模式查找 (__d_lookup_rcu),
     * 无需 d_lock. lease 无效时返回 -ECHILD, 退出 RCU 模式重新验证.
     *
     * 安全性: d_release 使用 call_rcu 延迟释放 di, RCU reader 访问
     * dentry->d_fsdata 和 di->lease_expire 安全 (grace period 内 di 有效).
     * dentry 本身在 RCU path walk 期间不会被释放 (VFS 持有 rcu_read_lock). */
    if (flags & LOOKUP_RCU) {
        di = dentry->d_fsdata;
        if (di && time_before(jiffies, READ_ONCE(di->lease_expire))) {
            /* dentry lease 有效, 但还需检查 inode cache_valid.
             * 跨客户端修改 (NOTIFY) 会清 cache_valid, 即使 dentry
             * lease 未过期也必须重新 lookup 获取最新元数据.
             * 用 READ_ONCE 无锁读 cache_valid (bool, 原子读取安全),
             * 避免在 RCU 临界区取 spinlock. */
            inode = d_inode(dentry);
            if (inode) {
                pi = POWERFS_I(inode);
                if (!READ_ONCE(pi->cache_valid))
                    return -ECHILD;  /* cache 无效, 退出 RCU 重新 lookup */
            }
            return 1;  /* lease 有效且 cache 有效, RCU 模式安全 */
        }
        return -ECHILD;  /* lease 无效或 di 为 NULL, 退出 RCU 模式 */
    }

    /* 获取 dentry 私有数据 */
    di = dentry->d_fsdata;
    if (!di)
        return 1;

    /* === Phase 1: Dentry lease fast-path ===
     * dentry lease 未过期: 负 dentry 直接返回 1 (5s 内不再 lookup),
     * 正 dentry 还需检查 inode cache_valid (本地 mutation 可能已失效).
     * dentry lease 过期: 让 VFS 调 lookup 重新验证 (返回 0). */
    if (time_after(jiffies, di->lease_expire)) {
        /* lease 过期: 让 VFS 重新 lookup. 正 dentry 同时清 cache_valid,
         * 让后续 getattr 拉取最新属性. 负 dentry 直接 return 0. */
        inode = d_inode(dentry);
        if (inode) {
            pi = POWERFS_I(inode);
            spin_lock(&pi->i_lock);
            pi->cache_valid = false;
            spin_unlock(&pi->i_lock);
        }
        pr_debug("powerfs: d_revalidate '%pd' lease expired\n", dentry);
        return 0;
    }

    /* lease 未过期 */
    inode = d_inode(dentry);
    if (!inode)
        return 1;   /* 负 dentry, lease 仍有效 */

    /* 正 dentry: 检查 inode 级 cache_valid.
     * 本地 mutation (mkdir/unlink 等) 会清 cache_valid, 即使 dentry lease
     * 未过期也要重新 lookup (mutation 后属性/存在性可能变). */
    pi = POWERFS_I(inode);
    spin_lock(&pi->i_lock);
    if (!pi->cache_valid) {
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: d_revalidate '%pd' inode cache invalid\n", dentry);
        return 0;
    }
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: d_revalidate '%pd' valid\n", dentry);
    return 1;
}

/*
 * d_prune - dentry 被 shrinker 回收前的通知
 *
 * 参考 ceph_d_prune (fs/ceph/dir.c)
 *
 * 用于清除父目录的 complete 标志，因为目录内容缓存不再完整
 */
void powerfs_d_prune(struct dentry *dentry)
{
    struct dentry *parent = dentry->d_parent;
    struct powerfs_inode_info *ppi;
    struct inode *dir;

    /* 根 dentry 不 prune (参考 ceph_d_prune) */
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
     * 参照 ceph __ceph_dir_clear_complete (atomic64_inc, 无锁).
     * dir_complete 是 bool, WRITE_ONCE 保证原子写入, 读取侧用 READ_ONCE. */
    WRITE_ONCE(ppi->dir_complete, false);
}

/* Dentry operations 表
 *
 * 启用 Delta Sync 所需的 d_revalidate 和 d_init/d_release
 * 参考 ceph dentry_operations
 */
static const struct dentry_operations powerfs_dentry_operations = {
    .d_revalidate   = powerfs_d_revalidate,
    .d_init         = powerfs_d_init,
    .d_release      = powerfs_d_release,
    .d_prune        = powerfs_d_prune,
};

/* ========== 辅助函数 ========== */

/*
 * powerfs_ino_compare - inode 比较函数 (用于 iget5_locked/ilookup5)
 *
 * 参考 ceph_ino_compare (fs/ceph/super.h)
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
 * 参考 ceph_set_ino_cb (fs/ceph/inode.c)
 */
static int powerfs_set_ino_cb(struct inode *inode, void *data)
{
    u64 *pino = (u64 *)data;
    inode->i_ino = *pino;
    return 0;
}

/*
 * powerfs_iget - 获取或创建 inode (参考 ceph_get_inode)
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
 * powerfs_find_inode - 查找已存在的 inode (参考 ceph_find_inode)
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

    if (!inode)
        goto out_free;

    pi = POWERFS_I(inode);

    /* 1. 发 getattr 获取最新元数据 */
    ret = powerfs_net_getattr(inode->i_ino, &mode, &uid, &gid,
                              &size, &nlink,
                              &mtime, &atime, &ctime,
                              &volume_id, &file_key);
    if (ret) {
        pr_warn("powerfs: refresh_work ino=%llu getattr failed: %d\n",
                rw->ino, ret);
        /* getattr 失败: 清 cache_valid, 让下次访问触发 re-lookup */
        spin_lock(&pi->i_lock);
        pi->cache_valid = false;
        pi->need_refresh = false;
        spin_unlock(&pi->i_lock);
        goto out_iput;
    }

    /* 2. 更新 inode 属性 */
    spin_lock(&inode->i_lock);
    /* 如果本地有脏页 (未刷盘的写入), 不用 filer 端的 size 覆盖本地 i_size.
     * 因为 write_end 已设置 i_size, 但 writeback 尚未通过 setattr 同步到 filer,
     * filer 端的 size 可能是旧值 (0).
     * 参考 ceph: 有 i_dirty_caps 时不覆盖 i_size. */
    if (mapping_tagged(inode->i_mapping, PAGECACHE_TAG_DIRTY) ||
        mapping_tagged(inode->i_mapping, PAGECACHE_TAG_WRITEBACK)) {
        pr_debug("powerfs: refresh_work ino=%llu has dirty/writeback pages, skip size update (local=%lld filer=%llu)\n",
                rw->ino, i_size_read(inode), (unsigned long long)size);
    } else {
        if (i_size_read(inode) != size) {
            i_size_write(inode, size);
        }
    }
    set_nlink(inode, nlink);
    inode->i_mtime.tv_sec = mtime;
    inode->i_mtime.tv_nsec = 0;
    inode->i_atime.tv_sec = atime;
    inode->i_atime.tv_nsec = 0;
    inode->i_ctime.tv_sec = ctime;
    inode->i_ctime.tv_nsec = 0;
    spin_unlock(&inode->i_lock);

    spin_lock(&pi->i_lock);
    pi->volume_id = volume_id;
    pi->file_key = file_key;
    pi->cache_valid = true;
    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
    pi->need_refresh = false;
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: refresh_work ino=%llu size=%llu vid=%llu fkey=%llu\n",
            rw->ino, (unsigned long long)size,
            (unsigned long long)volume_id,
            (unsigned long long)file_key);

    /* 3. 失效 page cache (clean pages), 使下次读从 volume 重新拉取 */
    invalidate_inode_pages2(inode->i_mapping);

out_iput:
    iput(inode);
out_free:
    kfree(rw);
}

/*
 * powerfs_invalidate_one - Invalidate one inode's caches
 *
 * Called from the powerfs-net RX path when a NOTIFY frame arrives
 * from the Filer (triggered by another client's metadata mutation).
 *
 * Actions:
 *   1. Look up the inode in the VFS inode hash (ilookup5). If not
 *      cached locally, nothing to invalidate — return 0.
 *   2. invalidate_inode_pages2() to drop clean pages and force
 *      re-read from the Filer/Volume on next access.
 *   3. For directories, call powerfs_invalidate_dir_lease() so the
 *      next readdir re-fetches entries.
 *
 * We intentionally do NOT d_drop() here: the Fuser-side Invalidate
 * only carries (inode, version), so we don't know if the inode was
 * deleted or merely modified.  The next lookup/getattr will fetch
 * fresh metadata; if the inode no longer exists on the Filer, the
 * lookup returns negative and VFS evicts the dentry naturally.
 *
 * Must be called in process context (workqueue / tasklet work),
 * not in softirq — invalidate_inode_pages2 may sleep.
 */
int powerfs_invalidate_one(u64 ino)
{
    struct super_block *sb = powerfs_get_sb();
    struct inode *inode;

    if (!sb)
        return -ENODEV;

    inode = powerfs_find_inode(sb, ino);
    if (!inode)
        return 0;  /* not cached, nothing to invalidate */

    /* 异步刷新 inode 元数据: 使用独立工作队列, 避免在调度器线程中
     * 同步等待 getattr 响应 (self-deadlock: 调度器等待自己处理的响应).
     *
     * 不再清除 cache_valid + 触发 d_invalidate + lookup_slow, 因为
     * 该路径在并发场景下会触发 d_lock 死锁 (__d_lookup 自旋等待
     * 被 d_invalidate 持有的 d_lock).
     *
     * 新方案: 保持 cache_valid=true, 通过异步 getattr 刷新 size/
     * volume_id/file_key, 同时失效 page cache 使下次读重新拉取数据. */
    {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        spin_lock(&pi->i_lock);
        pi->need_refresh = true;
        spin_unlock(&pi->i_lock);
    }

    /* 失效 page cache (clean pages), 使下次读从 volume 重新拉取.
     * 放到独立工作队列处理, 避免在调度器线程中阻塞. */
    {
        struct powerfs_refresh_work *rw;
        rw = kmalloc(sizeof(*rw), GFP_ATOMIC);
        if (rw) {
            INIT_WORK(&rw->work, powerfs_refresh_inode_work);
            rw->ino = ino;
            igrab(inode);
            rw->inode = inode;
            queue_work(powerfs_refresh_wq, &rw->work);
        } else {
            /* fallback: 清 cache_valid, 让下次访问触发 re-lookup */
            struct powerfs_inode_info *pi = POWERFS_I(inode);
            spin_lock(&pi->i_lock);
            pi->cache_valid = false;
            spin_unlock(&pi->i_lock);
        }
    }

    /* For directories, expire the readdir lease so next readdir
     * re-fetches entries from the Filer. */
    if (S_ISDIR(inode->i_mode))
        powerfs_invalidate_dir_lease(inode);

    pr_debug("powerfs: invalidate_one ino=%llu queued refresh\n", ino);
    iput(inode);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_invalidate_one);

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
    inode_init_owner(&init_user_ns, inode, NULL, mode);

    /* 设置 page cache 操作 */
    inode->i_mapping->a_ops = &powerfs_aops;
    mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
    mapping_set_unevictable(inode->i_mapping);

    /* 设置时间戳 */
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

    /* 初始化私有字段 */
    pi->parent_ino = parent_ino;
    strncpy(pi->name, name ? name : "", POWERFS_MAX_NAME_LEN - 1);
    pi->cache_valid = true;
    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
    pi->dir_complete = true;  /* 本地缓存模式: 目录始终完整 */
    /* Phase 1: 目录 lease 初始化.
     * 新建目录: 设 lease 未过期 (空目录, 无需拉取).
     * 新建普通文件: dir_lease_* 字段无意义, 但统一初始化. */
    pi->dir_lease_expire = jiffies + POWERFS_DIR_LEASE_TTL;
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
        pi->dir_complete = true;  /* 新建目录为空，认为 complete */
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

/*
 * powerfs_set_dentry_lease - 设置 dentry 租约
 */
void powerfs_set_dentry_lease(struct dentry *dentry, unsigned long ttl)
{
    struct powerfs_dentry_info *di = dentry->d_fsdata;

    if (!di)
        return;

    di->lease_expire = jiffies + ttl;
    di->time = jiffies;
}

/*
 * powerfs_dentry_lease_valid - 检查 dentry 租约是否有效
 */
bool powerfs_dentry_lease_valid(struct dentry *dentry)
{
    struct powerfs_dentry_info *di = dentry->d_fsdata;

    if (!di)
        return false;

    return time_before(jiffies, di->lease_expire);
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
 * 参考 ceph_alloc_inode (fs/ceph/inode.c)
 * 使用 alloc_inode_sb 辅助函数
 */
struct inode *powerfs_alloc_inode(struct super_block *sb)
{
    struct powerfs_inode_info *pi;

    pi = alloc_inode_sb(sb, powerfs_inode_cachep, GFP_NOFS);
    if (!pi)
        return NULL;

    /* netfs 初始化 (参考 ceph_alloc_inode) */
    netfs_inode_init(&pi->netfs, &powerfs_netfs_ops);

    /* 初始化 Lease 相关字段 */
    pi->lease_tree = RB_ROOT;
    spin_lock_init(&pi->lease_lock);
    INIT_DELAYED_WORK(&pi->lease_renew_work, powerfs_lease_renew_work_func);
    pi->chunks = NULL;
    pi->chunk_count = 0;
    pi->content_size = 0;
    pi->volume_id = 0;
    pi->file_key = 0;
    pi->shutdown = false;

    /* 初始化目录缓存字段 */
    INIT_LIST_HEAD(&pi->dir_entries);
    mutex_init(&pi->dir_mutex);

    pr_debug("powerfs: alloc_inode (pi=%p, inode=%p)\n", pi, &pi->netfs.inode);

    return &pi->netfs.inode;
}

/*
 * powerfs_free_inode - 释放 inode (super_operations)
 *
 * 参考 ceph_free_inode (fs/ceph/inode.c)
 * 只释放私有数据，inode 核心由 VFS 管理
 */
void powerfs_free_inode(struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: free_inode ino=%lu (pi=%p)\n", inode->i_ino, pi);

    /* 释放 chunk 映射 */
    kfree(pi->chunks);
    pi->chunks = NULL;

    kmem_cache_free(powerfs_inode_cachep, pi);
}

/*
 * powerfs_evict_inode - 驱逐 inode (super_operations)
 *
 * 参考 ceph_evict (fs/ceph/inode.c)
 *
 * 标准流程:
 *   1. truncate_inode_pages_final - 清理页面缓存
 *   2. clear_inode - 清除 inode 核心
 *   3. 清理文件系统私有资源
 */
void powerfs_evict_inode(struct inode *inode)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: evict_inode ino=%lu\n", inode->i_ino);

    /* 1. 先截断 page cache (参考 ceph_evict_inode) */
    truncate_inode_pages_final(&inode->i_data);

    /* 2. 取消后台 lease 续约 work（必须在 clear_inode 之前，因为 work 可能引用 inode）
     *    参考 ceph_evict_inode: cancel_writeback 是在 clear_inode 之前 */
    cancel_delayed_work_sync(&pi->lease_renew_work);

    /* 3. VFS 清理 */
    clear_inode(inode);

    /* 4. 释放所有 lease (rbtree 遍历) — Step 1 实现，Step 0 先清空 */
    while (!RB_EMPTY_ROOT(&pi->lease_tree)) {
        struct rb_node *n = rb_first(&pi->lease_tree);
        rb_erase(n, &pi->lease_tree);
        kfree(rb_entry(n, struct powerfs_lease, node));
    }

    /* 5. 释放 chunk 映射 */
    kfree(pi->chunks);
    pi->chunks = NULL;
    pi->chunk_count = 0;

    /* 6. 清理目录缓存链表 */
    powerfs_clear_dir_entries(inode);

    /* 7. 清理状态 */
    spin_lock(&pi->i_lock);
    pi->cache_valid = false;
    pi->dir_complete = false;
    /* Phase 1: 清目录 lease, 防止 inode 复用 (slab 重分配) 后误命中旧 lease. */
    pi->dir_lease_expire = 0;
    pi->dir_lease_epoch = 0;
    pi->shutdown = true;
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
 * 参考: ceph_write_inode (fs/ceph/inode.c) 同步 caps 模式.
 */
static int powerfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t i_size;
    u64 last_synced;
    int ret;

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

    /* 等待所有 pending 的异步 writeback 完成, 确保数据已写入 Filer 后
     * 再发送 setattr(SIZE). 否则 Filer 可能先收到 setattr 将文件扩展
     * (零填充), 然后异步写到达覆盖数据 -- 但若 Filer 的 setattr 截断
     * 已有数据, 则先到的写会被丢失.
     *
     * VFS writeback 顺序: do_writepages → write_inode → filemap_fdatawait
     * do_writepages 提交异步 work 后立即返回, write_inode 此刻发 setattr
     * 时数据可能尚未到达 Filer. 显式等待确保顺序正确. */
    filemap_fdatawait_range(inode->i_mapping, 0, LLONG_MAX);

    /* 先请求 Filer 同步 size, 成功后才更新本地 content_size.
     * 断连时 powerfs_net_setattr 返回 -ENOTCONN, writeback 会重试.
     * 之前断连时 return 0 (假装成功) 导致 size 未同步, remount 后不一致. */
    ret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                               0, 0, 0, (__u64)i_size);
    if (ret < 0) {
        pr_warn("powerfs: write_inode setattr ino=%lu size=%llu failed: %d\n",
                inode->i_ino, (u64)i_size, ret);
        return ret;  /* 传播错误, writeback 会 redirty 重试 */
    }

    /* 请求成功后才更新本地记录 */
    spin_lock(&pi->i_lock);
    pi->content_size = (u64)i_size;
    spin_unlock(&pi->i_lock);

    return 0;
}

/* Step 0 stub: lease 续约 work 函数（Step 2 实现真实续约逻辑） */
static void powerfs_lease_renew_work_func(struct work_struct *work)
{
    /* Step 0: 无操作，Step 2 实现 lease 续约 */
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

    (void)flags;

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
        int timeout_ms;

        pr_debug("powerfs: lookup '%pd' via powerfs_net\n", dentry);

        /* Phase 1: 超时策略.
         * lookup 是 open/create/stat/unlink 等关键路径的必经之路.
         * 断连期间请求入队列等待 filer 重连, 不立即返回错误.
         * 超时设为 60s 覆盖: Docker 容器重启 (~20s) + Raft 选主 (~5s)
         * + 内核重连 (~5s) + FAULT 重试 (5s) = ~35s, 留足余量.
         * hung_task_timeout=120s, 60s 不会触发 hung task. */
        timeout_ms = 60000;  /* 60s: 覆盖 Docker 容器重启 + Raft 选主 */

        /* 通过 powerfs_net 直接查询 (含时间戳 + volume_id/file_key) */
        err = powerfs_net_lookup_timeout(dir->i_ino, dentry->d_name.name,
                                          strlen(dentry->d_name.name),
                                          &ino, &mode, &uid, &gid,
                                          &size, &nlink,
                                          &mtime, &atime, &ctime,
                                          &volume_id, &file_key,
                                          timeout_ms);

        /* 断连/重连期间超时或网络不可达, 返回 -EAGAIN 让 VFS/应用层重试.
         * - ETIMEDOUT: 60s 内未收到响应 (filer 长时间不可用)
         * - ENOTCONN: disconnect_one 已 complete(-ENOTCONN) (在途请求被取消)
         * - ESHUTDOWN: pool 正在 stopping
         * 注意: 60s 超时已覆盖 Docker 容器重启场景, 正常情况下 filer 会在
         *       ~30s 内重连, 请求在队列中被 dispatch_pending 唤醒并完成. */
        if (err == -ETIMEDOUT || err == -ENOTCONN || err == -ESHUTDOWN) {
            pr_warn("powerfs: lookup '%pd' transient error %d (timeout_ms=%d), "
                    "return -EAGAIN for retry\n",
                    dentry, err, timeout_ms);
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
                inode->i_mtime.tv_sec = mtime;
                inode->i_mtime.tv_nsec = 0;
                inode->i_atime.tv_sec = atime;
                inode->i_atime.tv_nsec = 0;
                inode->i_ctime.tv_sec = ctime;
                inode->i_ctime.tv_nsec = 0;

                {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->cache_valid = true;
                    /* 数据直连: 存储 volume_id/file_key 用于 ReadNeedle/WriteNeedle.
                     * 目录的 volume_id/file_key 为 0 (无数据). */
                    pi->volume_id = volume_id;
                    pi->file_key = file_key;
                    spin_unlock(&pi->i_lock);
                }

                unlock_new_inode(inode);
            } else {
                /* 已有 inode (非 I_NEW): d_revalidate 返回 0 触发 re-lookup.
                 * 用 Filer 返回的权威属性更新现有 inode, 否则跨客户端
                 * 修改后内核仍用旧的 i_size/volume_id/file_key, 导致
                 * 读取空内容 (size=0 跳过 read) 或 needle not found. */
                spin_lock(&inode->i_lock);
                inode->i_mode = mode;
                inode->i_uid = make_kuid(&init_user_ns, uid);
                inode->i_gid = make_kgid(&init_user_ns, gid);
                if (i_size_read(inode) != size) {
                    i_size_write(inode, size);
                }
                set_nlink(inode, nlink);
                inode->i_mtime.tv_sec = mtime;
                inode->i_mtime.tv_nsec = 0;
                inode->i_atime.tv_sec = atime;
                inode->i_atime.tv_nsec = 0;
                inode->i_ctime.tv_sec = ctime;
                inode->i_ctime.tv_nsec = 0;
                spin_unlock(&inode->i_lock);

                {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->cache_valid = true;
                    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
                    /* 更新 volume_id/file_key (可能因 FUSE 端写入而变化) */
                    pi->volume_id = volume_id;
                    pi->file_key = file_key;
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
             * 中实例化 dentry 的正确接口 (参考 ceph_lookup -> d_splice_alias
             * -> __d_add)。
             */
            d_add(dentry, inode);

            /* 更新 dentry 租约时间 */
            {
                struct powerfs_dentry_info *di = dentry->d_fsdata;
                if (di) {
                    di->lease_expire = jiffies + POWERFS_DENTRY_LEASE_TTL;
                    di->time = jiffies;
                }
            }

            pr_debug("powerfs: lookup '%pd' completed\n", dentry);
            return NULL;
        }

        if (err == -ENOENT) {
            /* 文件不存在: 添加负 dentry + 设短 lease (5s 内不再重复 lookup).
             * Phase 1: 负 dentry lease 减少对不存在文件的重复网络请求. */
            struct powerfs_dentry_info *di;
            pr_debug("powerfs: lookup '%pd' not found (powerfs_net)\n", dentry);
            d_add(dentry, NULL);
            di = dentry->d_fsdata;
            if (di)
                di->lease_expire = jiffies + POWERFS_DENTRY_LEASE_TTL;
            return NULL;
        }

        /* 其他错误: 返回错误给 VFS, 不缓存负 dentry (避免误缓存 "不存在").
         * Phase 1: 之前是 "记录 + 添加负 dentry", 这会让网络瞬态错误被
         * 当成 "文件不存在" 缓存, 后续访问该文件继续返回 ENOENT.
         * 现在返回错误, 让 VFS/userspace 看到真实错误码并重试. */
        pr_warn("powerfs: lookup '%pd' powerfs_net error: %d\n", dentry, err);
        return ERR_PTR(err);
    }

    /* === 纯本地模式: 添加负 dentry === */
    pr_debug("powerfs: lookup '%pd' not found (local mode)\n", dentry);
    d_add(dentry, NULL);
    return NULL;
}

/* ========== 目录操作: mknod (通用创建函数) ========== */

/*
 * powerfs_mknod - 创建文件/目录/设备节点 (内部通用函数)
 *
 * 参考 ramfs_mknod (fs/ramfs/inode.c)
 */
static int powerfs_mknod(struct user_namespace *idmap, struct inode *dir,
                          struct dentry *dentry, umode_t mode, dev_t dev)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct inode *inode;
    u64 new_ino;
    u64 mknod_volume_id = 0, mknod_file_key = 0;  /* P3.4: Filer 自分配, 存入 inode */
    int type;

    (void)idmap;

    pr_debug("powerfs: mknod '%pd' mode=%o in dir=%lu\n",
             dentry, mode, dir->i_ino);

    /*
     * 向 filer 发 CREATE/MKDIR 请求获取权威 ino, 使文件元数据持久化到
     * filer (Raft 强一致); remount 后 lookup 能找回.
     * 断连时 powerfs_net_create 返回 -ENOTCONN, 操作失败 (不再本地分配).
     * symlink/特殊文件暂走本地 (有独立 powerfs_net_symlink 接口).
     */
    if (S_ISREG(mode) || S_ISDIR(mode)) {
        u64 remote_ino = 0;
        u64 volume_id = 0, file_key = 0;
        int rerr = powerfs_net_create(dir->i_ino, dentry->d_name.name,
                                       dentry->d_name.len, mode,
                                       from_kuid(&init_user_ns, current_fsuid()),
                                       from_kgid(&init_user_ns, current_fsgid()),
                                       S_ISDIR(mode), &remote_ino,
                                       &volume_id, &file_key);
        if (rerr) {
            pr_warn("powerfs: net_create '%pd' failed: %d\n", dentry, rerr);
            return rerr;
        }
        new_ino = remote_ino ? remote_ino
                             : (u64)atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
        /*
         * P3.4: 保存 Filer 自分配的 volume_id + needle_id 到 mknod 局部变量,
         * inode 创建后写入 powerfs_inode_info, 供后续直连 volume 读写.
         * 目录无数据, volume_id/file_key 为 0 (Filer handle_mkdir 不分配).
         */
        mknod_volume_id = volume_id;
        mknod_file_key = file_key;
    } else {
        new_ino = atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
    }

    /* 创建 inode */
    inode = powerfs_new_inode(dir->i_sb, mode, new_ino,
                               dir->i_ino, dentry->d_name.name);
    if (!inode)
        return -ENOSPC;

    /* P3.4: 将 Filer 分配的 volume_id + needle_id 存入 inode 私有数据 */
    if (S_ISREG(mode) && mknod_volume_id != 0) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        spin_lock(&pi->i_lock);
        pi->volume_id = mknod_volume_id;
        pi->file_key = mknod_file_key;
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: create '%pd' ino=%lu volume_id=%llu file_key=%llu\n",
                 dentry, inode->i_ino, mknod_volume_id, mknod_file_key);
    }

    /* 关联 dentry 和 inode */
    d_add(dentry, inode);

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

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

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: mknod '%pd' success, ino=%llu\n",
             dentry, new_ino);

    return 0;
}

/* ========== 目录操作: mkdir ========== */

static int powerfs_mkdir(struct user_namespace *idmap, struct inode *dir,
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
        return err;
    }

    /* 增加父目录 nlink (".." 指向父目录) */
    inc_nlink(dir);

    /* 获取新创建的 inode 用于远程同步 */
    inode = d_inode(dentry);

    pr_debug("powerfs: mkdir '%pd' success, ino=%lu\n",
            dentry, inode ? inode->i_ino : 0);

    return 0;
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
    dir->i_mtime = dir->i_ctime = current_time(dir);

    /* 减少被删除目录的链接数 (参考 simple_rmdir, 不 ihold/iput) */
    drop_nlink(inode);

    /* 清空子目录的目录项链表 */
    powerfs_clear_dir_entries(inode);

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

static int powerfs_create(struct user_namespace *idmap, struct inode *dir,
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
    dir->i_mtime = dir->i_ctime = current_time(dir);

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

static int powerfs_symlink(struct user_namespace *idmap, struct inode *dir,
                            struct dentry *dentry, const char *symname)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct inode *inode;
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

    /* 使用 page cache 存储符号链接目标 */
    err = page_symlink(inode, symname, strlen(symname) + 1);
    if (err) {
        iput(inode);
        return err;
    }

    /* 关联 dentry 和 inode */
    d_add(dentry, inode);

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

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
 * 参考 ceph_readlink (fs/ceph/symlink.c)
 *
 * 策略:
 *   - 通信层可用时: 优先从服务端获取最新目标路径
 *   - 纯内存模式: 直接从 page cache 读取
 */
int powerfs_readlink(struct dentry *dentry, char *buffer, int buflen)
{
    struct inode *inode = d_inode(dentry);
    int ret;

    pr_debug("powerfs: readlink '%pd'\n", dentry);

    if (!inode)
        return -ENOENT;

    if (!S_ISLNK(inode->i_mode))
        return -EINVAL;

    /*
     * 本地缓存模式: 直接从 page cache 读取
     *
     * 不再从代理读取 (避免同步通信导致的 RCU stall)
     * 符号链接目标存储在 page cache 中 (由 page_symlink 写入)
     */

    /*
     * 从 page cache 读取 (本地缓存/纯内存模式)
     */
    {
        struct page *page;
        void *page_addr;
        u32 len;

        if (inode->i_size == 0) {
            pr_warn("powerfs: readlink '%pd' empty target\n", dentry);
            buffer[0] = '\0';
            return 0;
        }

        len = min_t(u32, (u64)inode->i_size, buflen - 1);

        page = find_get_page(inode->i_mapping, 0);
        if (!page) {
            pr_warn("powerfs: readlink '%pd' no page cache\n", dentry);
            buffer[0] = '\0';
            return 0;
        }

        page_addr = kmap(page);
        memcpy(buffer, page_addr, len);
        buffer[len] = '\0';
        kunmap(page);
        put_page(page);

        pr_debug("powerfs: readlink '%pd' from cache: '%s'\n",
                 dentry, buffer);
        return 0;
    }
}

/* ========== 目录操作: link (硬链接) ========== */

/*
 * powerfs_link - 创建硬链接
 *
 * 参考 ceph_link (fs/ceph/dir.c)
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
     * d_add 不递增 i_count! 每个 dentry 必须持有独立的 i_count
     * 参考 simple_link / ramfs_link 均在 d_add 前调用 ihold.
     */
    inc_nlink(inode);
    ihold(inode);
    d_add(new_dentry, inode);

    dir->i_mtime = dir->i_ctime = current_time(dir);

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
 * 参考 ceph_getattr (fs/ceph/inode.c)
 *
 * 策略:
 *   - 通信层可用时: 优先从服务端获取最新属性
 *   - 纯内存模式: 直接使用本地 inode 缓存
 */
static int powerfs_getattr(struct user_namespace *idmap, const struct path *path,
                            struct kstat *stat, u32 request_mask,
                            unsigned int query_flags)
{
    struct inode *inode = d_inode(path->dentry);
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    (void)idmap;
    (void)request_mask;
    (void)query_flags;

    pr_debug("powerfs: getattr '%pd'\n", path->dentry);

    /*
     * 本地缓存模式: 直接使用本地 inode 属性
     *
     * 不再从代理获取属性 (避免同步通信导致的 RCU stall)
     * 如果需要从服务端获取属性，可在后续阶段添加异步机制
     */

    /* 使用 VFS 通用属性获取 */
    generic_fillattr(&init_user_ns, inode, stat);

    /* block 数 (512 字节为单位) */
    stat->blocks = (i_size_read(inode) + 511) / 512;
    stat->blksize = 4096;

    return 0;
}

/* ========== setattr ========== */

/*
 * powerfs_setattr - 设置文件属性
 *
 * 参考 ceph_setattr (__ceph_setattr) (fs/ceph/inode.c)
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
int powerfs_setattr(struct user_namespace *idmap, struct dentry *dentry,
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
    err = setattr_prepare(&init_user_ns, dentry, attr);
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
        truncate_setsize(inode, attr->ia_size);
        /* 同步 pi->content_size, 否则 write_end 的
         * (new_size != pi->content_size) 检查会误判为 "未变化"
         * 而跳过 net_setattr, 导致 O_TRUNC 后的写入不持久化 size.
         * 例: 文件原 size=4, O_TRUNC 设 size=0, 再写 4 字节:
         *   new_size=4 == pi->content_size=4 → 跳过 setattr
         *   Filer 端 size 仍为 0 → remount 后文件为空 */
        pi->content_size = attr->ia_size;
    }

    /* 第二步: 在本地修改 inode 属性 */
    setattr_copy(&init_user_ns, inode, attr);
    mark_inode_dirty(inode);

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
        __u64 sz = 0;

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

        if (valid) {
            int sret = powerfs_net_setattr(inode->i_ino, valid,
                                            m, u, g, sz);
            pr_debug("powerfs: SETATTR net ino=%lu valid=0x%x sz=%llu sret=%d\n",
                    inode->i_ino, valid, sz, sret);
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
 * 参考 ceph_rename (fs/ceph/dir.c)
 *
 * 策略:
 *   - 纯本地操作: 在 VFS dcache 中直接进行重命名
 *   - 异步通知: 操作完成后异步通知代理更新后端
 *
 * 处理情况:
 *   - 同一目录内重命名
 *   - 跨目录移动
 *   - 目标已存在 (覆盖)
 */
int powerfs_rename(struct user_namespace *idmap,
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

        strncpy(old_name_buf, old_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        old_name_buf[POWERFS_MAX_NAME_LEN] = '\0';
        strncpy(new_name_buf, new_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        new_name_buf[POWERFS_MAX_NAME_LEN] = '\0';

        /* 检查目标已存在的情况 (但不修改 nlink, 等 net_rename 成功后再改) */
        if (d_really_is_positive(new_dentry)) {
            struct inode *target = d_inode(new_dentry);

            /* 不支持 RENAME_EXCHANGE */
            if (flags & RENAME_EXCHANGE)
                return -EINVAL;

            if (!target)
                return -ENOENT;

            /* 检查是否可以删除目标 */
            if (S_ISDIR(target->i_mode)) {
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

        /*
         * 目标已存在: 减少目标 inode 的 nlink
         * 参考 ramfs_rename (fs/ramfs/inode.c)
         */
        if (d_really_is_positive(new_dentry)) {
            struct inode *target = d_inode(new_dentry);
            if (S_ISDIR(target->i_mode))
                drop_nlink(new_dir);
            drop_nlink(target);
        }

        /*
         * 目录跨移动: drop_nlink(old_dir) + inc_nlink(new_dir)
         * 参考 simple_rename_exchange
         */
        if (S_ISDIR(inode->i_mode) && old_dir != new_dir) {
            drop_nlink(old_dir);
            inc_nlink(new_dir);
        }

        /* 更新时间戳 */
        old_dir->i_mtime = old_dir->i_ctime = current_time(old_dir);
        if (old_dir != new_dir)
            new_dir->i_mtime = new_dir->i_ctime = current_time(new_dir);

        mark_inode_dirty(old_dir);
        if (old_dir != new_dir)
            mark_inode_dirty(new_dir);

        /* 更新本地目录项链表 */
        /* 从旧目录移除旧名称 */
        powerfs_remove_dir_entry(old_dir, old_name_buf);

        /* 在新目录添加新名称 (或在同一目录更新) */
        {
            unsigned int entry_type = inode->i_mode & S_IFMT;
            powerfs_add_dir_entry(new_dir, inode->i_ino,
                                  entry_type, new_name_buf);
        }

        /* 如果目标已存在，移除目标目录项 */
        if (d_really_is_positive(new_dentry)) {
            struct inode *target = d_inode(new_dentry);
            if (target) {
                powerfs_remove_dir_entry(new_dir, new_name_buf);
            }
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
 * 参考 ceph_dir_open (fs/ceph/dir.c)
 * 分配目录文件私有数据，初始化readdir状态
 */
int powerfs_dir_open(struct inode *inode, struct file *file)
{
    struct powerfs_dir_file_info *dfi;

    pr_debug("powerfs: dir_open ino=%lu\n", inode->i_ino);

    dfi = kzalloc(sizeof(*dfi), GFP_KERNEL);
    if (!dfi)
        return -ENOMEM;

    dfi->file = file;
    dfi->dir = POWERFS_I(inode);
    dfi->last_ino = 0;
    dfi->last_name = NULL;
    dfi->next_offset = 0;
    dfi->cached_entries = NULL;
    dfi->cached_count = 0;
    dfi->cached_index = 0;
    mutex_init(&dfi->lock);

    file->private_data = dfi;

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

    pr_debug("powerfs: dir_release ino=%lu\n", inode->i_ino);

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
        /* 没有更多条目，标记目录完整 */
        spin_lock(&dpi->i_lock);
        dpi->dir_complete = true;
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

    /* 获取成功，标记目录内容已完整缓存 */
    spin_lock(&dpi->i_lock);
    dpi->dir_complete = true;
    spin_unlock(&dpi->i_lock);

    pr_debug("powerfs: fill_readdir_cache got %u entries (dir_complete=true)\n",
             dfi->cached_count);
    return 0;
}

/* ========== 目录项管理 (本地 readdir) ========== */

/**
 * powerfs_add_dir_entry - 添加目录项到链表
 *
 * 使用 dir_mutex 保护
 */
static int powerfs_add_dir_entry(struct inode *dir, u64 ino,
                                  unsigned int type, const char *name)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry;

    if (!S_ISDIR(dir->i_mode))
        return 0;

    entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;

    entry->ino = ino;
    entry->type = type;
    strncpy(entry->name, name, POWERFS_MAX_NAME_LEN - 1);
    entry->name[POWERFS_MAX_NAME_LEN - 1] = '\0';

    mutex_lock(&dpi->dir_mutex);
    list_add_tail(&entry->list, &dpi->dir_entries);
    mutex_unlock(&dpi->dir_mutex);

    return 0;
}

/**
 * powerfs_remove_dir_entry - 从链表中移除目录项
 *
 * 使用 dir_mutex 保护
 */
static int powerfs_remove_dir_entry(struct inode *dir, const char *name)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry, *tmp;

    if (!S_ISDIR(dir->i_mode))
        return 0;

    mutex_lock(&dpi->dir_mutex);
    list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
        if (strcmp(entry->name, name) == 0) {
            list_del_init(&entry->list);
            mutex_unlock(&dpi->dir_mutex);
            kfree(entry);
            return 0;
        }
    }
    mutex_unlock(&dpi->dir_mutex);

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

/* Phase 1: 本地 mutation 后清父目录 lease + bump epoch.
 * 调用时机: mkdir/rmdir/create/unlink/symlink/link/rename 网络请求成功后,
 *           在修改本地数据结构的同时清目录 lease.
 * 效果: 下次 readdir 看到 lease 过期会重新拉取, 看到自己刚加/删的项.
 *       epoch++ 留给 Phase 3 callback 比对 (callback 携带 epoch,
 *       不匹配说明有过本地修改, 需重新拉取).
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

        /* lease 过期: 清空旧缓存, 重新拉取 (本地 mutation 后). */
        if (READ_ONCE(dpi->dir_complete) &&
            time_after(jiffies, READ_ONCE(dpi->dir_lease_expire))) {
            powerfs_clear_dir_entries(dir);
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

                /* 检查是否已存在 (避免重复) */
                bool found = false;
                list_for_each_entry(de, &dpi->dir_entries, list) {
                    if (de->ino == ne->ino) {
                        found = true;
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
                strncpy(de->name, ne->name, POWERFS_MAX_NAME_LEN - 1);
                de->name[POWERFS_MAX_NAME_LEN - 1] = '\0';
                list_add_tail(&de->list, &dpi->dir_entries);

                /* 更新 last_name 用于分页 */
                strncpy(last_name, ne->name, sizeof(last_name) - 1);
                last_name[sizeof(last_name) - 1] = '\0';
            }
            mutex_unlock(&dpi->dir_mutex);
        } while (has_more && net_count > 0);

        kfree(net_entries);

        /* 拉取成功 (或部分成功): 设置 dir_complete + dir_lease_expire.
         * 部分成功时也设 dir_complete (避免反复部分拉取), 下次 lease 过期再补. */
        WRITE_ONCE(dpi->dir_complete, true);
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
            mutex_lock(&dpi->dir_mutex);
            list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
                unsigned char d_type;
                if (pos > 0) { pos--; continue; }
                switch (entry->type) {
                case S_IFREG:  d_type = DT_REG; break;
                case S_IFDIR:  d_type = DT_DIR; break;
                case S_IFLNK:  d_type = DT_LNK; break;
                default:       d_type = DT_UNKNOWN; break;
                }
                if (!dir_emit(ctx, entry->name, strlen(entry->name),
                              entry->ino, d_type)) {
                    mutex_unlock(&dpi->dir_mutex);
                    return 0;
                }
                ctx->pos++;
            }
            mutex_unlock(&dpi->dir_mutex);
            return 0;
        }

        mutex_lock(&dpi->dir_mutex);

        list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
            if (count >= max)
                break;
            if (pos > 0) {
                pos--;
                continue;
            }
            buf[count].ino = entry->ino;
            buf[count].type = entry->type;
            buf[count].namelen = strlen(entry->name);
            memcpy(buf[count].name, entry->name, buf[count].namelen + 1);
            count++;
        }

        mutex_unlock(&dpi->dir_mutex);

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
                kfree(buf);
                return 0;
            }

            ctx->pos++;
        }

        kfree(buf);
    }

    return 0;
}

/* 目录文件操作表 - 使用本地链表实现 readdir */
static const struct file_operations powerfs_dir_operations = {
    .open           = powerfs_dir_open,
    .release        = powerfs_dir_release,
    .iterate_shared = powerfs_readdir,
    .llseek         = generic_file_llseek,
};

/* ========== 地址空间操作 (page cache) ========== */

/* powerfs_read_folio 已移除 (Stage C): read 路径由 netfs_read_folio +
 * powerfs_netfs_issue_read 接管, 参照 ceph_aops.read_folio = netfs_read_folio. */

/* ========== Stage C: writepages 批量异步写入 ==========
 *
 * 参照 ceph_writepages_start 模式, 批量收集脏页减少 work item 数量:
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
 * powerfs_writepage_work_fn - 批量异步写 workqueue 函数
 *
 * 直连 Volume Server (WriteNeedle), 按 needle (chunk) 分组批量写:
 *   1. 遍历 batch 内页面 (按 offset 升序), 按 needle_id 分组
 *   2. 每个 needle: read-modify-write (读现有 needle → 合并页面 → 整体写回)
 *   3. 同一 needle 的页面只做一次 read + 一次 write
 *
 * needle 模型: write_needle 整体替换 needle 内容, 不支持 partial write.
 * 因此需 read-modify-write: 读现有 needle (若存在), 拷贝脏页到对应位置, 写回.
 *
 * 性能: 1MB 文件 (全在 1 个 needle) → 1 次 read + 1 次 write (vs 逐页 256 次).
 */
static void powerfs_writepage_work_fn(struct work_struct *work)
{
    struct powerfs_writepage_work *wpw =
        container_of(work, struct powerfs_writepage_work, work);
    struct inode *inode = wpw->inode;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    __u64 volume_id, file_key;
    __u8 *needle_buf = NULL;
    __u64 current_needle_id = 0;
    bool needle_loaded = false;   /* needle_buf 是否已为 current_needle_id 加载 */
    __u32 needle_len = 0;         /* current_needle_id 的内容长度 */
    int needle_start_idx = 0;     /* 当前 needle 组的首页索引 */
    int i;

    /* 数据直连: 从 inode 获取 volume_id/file_key */
    spin_lock(&pi->i_lock);
    volume_id = pi->volume_id;
    file_key = pi->file_key;
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: writepage_work_fn ino=%lu num_pages=%d vid=%llu fkey=%llu\n",
            inode->i_ino, wpw->num_pages,
            (unsigned long long)volume_id, (unsigned long long)file_key);

    if (!volume_id || !file_key) {
        pr_warn("powerfs: writepage_work ino=%lu no volume mapping\n",
                inode->i_ino);
        goto fail_all;
    }

    if (powerfs_net_is_stopping())
        goto fail_all;

    /* needle_buf: 2MB chunk buffer, 用于 read-modify-write.
     * 一个 batch 内复用, 避免逐页分配. */
    needle_buf = kvmalloc(POWERFS_CHUNK_SIZE, GFP_KERNEL);
    if (!needle_buf)
        goto fail_all;

    for (i = 0; i < wpw->num_pages; i++) {
        struct page *page = wpw->pages[i];
        loff_t offset = wpw->offsets[i];
        size_t count = wpw->counts[i];
        __u64 needle_id;
        size_t offset_in_needle;

        if (count == 0) {
            end_page_writeback(page);
            put_page(page);
            /* 若中间有空页, 不影响 needle 分组逻辑 */
            continue;
        }

        needle_id = file_key + offset / POWERFS_CHUNK_SIZE;
        offset_in_needle = offset % POWERFS_CHUNK_SIZE;

        /* needle 边界切换: 写回前一个 needle, 加载新 needle */
        if (!needle_loaded) {
            current_needle_id = needle_id;
            needle_start_idx = i;
            memset(needle_buf, 0, POWERFS_CHUNK_SIZE);
            needle_len = 0;

            /* read-modify-write: 先读现有 needle (不存在则全零) */
            {
                __u32 existing_len = 0;
                int rerr = powerfs_net_read_needle(volume_id, needle_id,
                                                    needle_buf,
                                                    POWERFS_CHUNK_SIZE,
                                                    &existing_len);
                if (rerr < 0 && rerr != -ENOENT) {
                    pr_warn("powerfs: writepage rmw read_needle vid=%llu nid=%llu err=%d, continue with zero\n",
                            (unsigned long long)volume_id,
                            (unsigned long long)needle_id, rerr);
                } else if (rerr == 0) {
                    needle_len = existing_len;
                }
            }
            needle_loaded = true;
        } else if (needle_id != current_needle_id) {
            /* needle 切换: 写回旧 needle, 完成其页面, 加载新 needle */
            int err = powerfs_net_write_needle(volume_id, current_needle_id,
                                                inode->i_ino,
                                                needle_buf, needle_len);
            {
                int j;
                for (j = needle_start_idx; j < i; j++) {
                    struct page *p = wpw->pages[j];
                    if (wpw->counts[j] == 0) continue;
                    if (err < 0) {
                        SetPageError(p);
                        mapping_set_error(p->mapping, err);
                    }
                    end_page_writeback(p);
                    put_page(p);
                }
            }
            if (err < 0)
                pr_warn("powerfs: writepage write_needle vid=%llu nid=%llu err=%d\n",
                        (unsigned long long)volume_id,
                        (unsigned long long)current_needle_id, err);

            /* 加载新 needle */
            current_needle_id = needle_id;
            needle_start_idx = i;
            memset(needle_buf, 0, POWERFS_CHUNK_SIZE);
            needle_len = 0;
            {
                __u32 existing_len = 0;
                int rerr = powerfs_net_read_needle(volume_id, needle_id,
                                                    needle_buf,
                                                    POWERFS_CHUNK_SIZE,
                                                    &existing_len);
                if (rerr < 0 && rerr != -ENOENT) {
                    pr_warn("powerfs: writepage rmw read_needle vid=%llu nid=%llu err=%d, continue with zero\n",
                            (unsigned long long)volume_id,
                            (unsigned long long)needle_id, rerr);
                } else if (rerr == 0) {
                    needle_len = existing_len;
                }
            }
        }

        /* 拷贝页面数据到 needle_buf 对应位置 */
        {
            char *kaddr = kmap_local_page(page);
            memcpy(needle_buf + offset_in_needle, kaddr, count);
            kunmap_local(kaddr);
            pr_debug("powerfs: writepage memcpy page->index=%lu offset=%lld count=%zu needle_buf[0..7]=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    page->index, offset, count,
                    needle_buf[0], needle_buf[1], needle_buf[2], needle_buf[3],
                    needle_buf[4], needle_buf[5], needle_buf[6], needle_buf[7]);
        }

        /* 扩展 needle 长度 (若写入超出原有内容) */
        if (offset_in_needle + count > needle_len)
            needle_len = offset_in_needle + count;
    }

    /* 写回最后一个 needle 并完成其页面 */
    if (needle_loaded) {
        int err = powerfs_net_write_needle(volume_id, current_needle_id,
                                            inode->i_ino,
                                            needle_buf, needle_len);
        int j;
        pr_debug("powerfs: writepage_work_fn write_needle vid=%llu nid=%llu len=%u err=%d\n",
                (unsigned long long)volume_id,
                (unsigned long long)current_needle_id, needle_len, err);
        for (j = needle_start_idx; j < wpw->num_pages; j++) {
            struct page *p = wpw->pages[j];
            if (wpw->counts[j] == 0) {
                /* 已完成的空页跳过 */
                continue;
            }
            if (err < 0) {
                SetPageError(p);
                mapping_set_error(p->mapping, err);
            }
            end_page_writeback(p);
            put_page(p);
        }
        if (err < 0)
            pr_warn("powerfs: writepage final write_needle vid=%llu nid=%llu err=%d\n",
                    (unsigned long long)volume_id,
                    (unsigned long long)current_needle_id, err);
    }

    kvfree(needle_buf);
    iput(inode);
    kvfree(wpw);
    return;

fail_all:
    for (i = 0; i < wpw->num_pages; i++) {
        SetPageError(wpw->pages[i]);
        mapping_set_error(wpw->pages[i]->mapping, -EIO);
        end_page_writeback(wpw->pages[i]);
        put_page(wpw->pages[i]);
    }
    if (needle_buf)
        kvfree(needle_buf);
    iput(inode);
    kvfree(wpw);
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
    struct pagevec pvec;
    pgoff_t index = wbc->range_start >> PAGE_SHIFT;
    pgoff_t end = wbc->range_end >> PAGE_SHIFT;
    struct powerfs_writepage_work *batch = NULL;
    int batch_pages = sbi->write_batch_pages;
    int ret = 0;

    pr_debug("powerfs: writepages ino=%lu range=%llu-%llu nr_to_write=%ld\n",
            inode->i_ino, wbc->range_start, wbc->range_end, wbc->nr_to_write);

    if (powerfs_net_is_stopping())
        return 0;

    pagevec_init(&pvec);

    while (index <= end) {
        int nr_pages, i;

        nr_pages = pagevec_lookup_range_tag(&pvec, mapping, &index,
                                             end, PAGECACHE_TAG_DIRTY);
        if (!nr_pages) {
            pr_debug("powerfs: writepages no dirty pages found, index=%lu end=%lu\n",
                    index, end);
            break;
        }
        pr_debug("powerfs: writepages found %d dirty pages, index=%lu\n",
                nr_pages, index);

        for (i = 0; i < nr_pages; i++) {
            struct page *page = pvec.pages[i];
            loff_t offset;
            size_t count = PAGE_SIZE;

            lock_page(page);
            if (!PageDirty(page)) {
                unlock_page(page);
                continue;
            }

            clear_page_dirty_for_io(page);

            offset = page_offset(page);
            if (offset >= i_size_read(inode)) {
                unlock_page(page);
                continue;
            }
            if (offset + count > i_size_read(inode))
                count = i_size_read(inode) - offset;

            /* 分配 batch (如果当前没有 pending) */
            if (!batch) {
                batch = powerfs_alloc_write_batch(batch_pages, GFP_NOFS);
                if (!batch) {
                    redirty_page_for_writepage(wbc, page);
                    unlock_page(page);
                    continue;
                }
                INIT_WORK(&batch->work, powerfs_writepage_work_fn);
                batch->inode = igrab(inode);
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
                queue_work(sbi->writeback_wq, &batch->work);
                batch = NULL;
            }

            wbc->nr_to_write--;
            if (wbc->nr_to_write <= 0)
                goto done;
        }
        pagevec_release(&pvec);
        cond_resched();
    }

done:
    pagevec_release(&pvec);

    /* 提交剩余的 batch */
    if (batch) {
        if (batch->num_pages > 0)
            queue_work(sbi->writeback_wq, &batch->work);
        else {
            iput(batch->inode);
            kvfree(batch);
        }
    }

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

    wpw = powerfs_alloc_write_batch(1, GFP_NOFS);
    if (!wpw) {
        redirty_page_for_writepage(wbc, page);
        unlock_page(page);
        return 0;
    }

    INIT_WORK(&wpw->work, powerfs_writepage_work_fn);
    wpw->inode = igrab(inode);
    get_page(page);
    wpw->pages[0] = page;
    wpw->offsets[0] = offset;
    wpw->counts[0] = count;
    wpw->num_pages = 1;

    set_page_writeback(page);
    unlock_page(page);
    queue_work(sbi->writeback_wq, &wpw->work);

    return 0;
}

/*
 * powerfs_write_begin - 写开始 (Stage C: 对接 netfs 子系统)
 *
 * 参照 ceph_write_begin (fs/ceph/addr.c):
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
int powerfs_write_begin(struct file *file, struct address_space *mapping,
                         loff_t pos, unsigned int len, struct page **pagep,
                         void **fsdata)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct folio *folio = NULL;
    int ret;

    pr_debug("powerfs: write_begin ino=%lu pos=%lld len=%u i_size=%lld\n",
            inode->i_ino, pos, len, i_size_read(inode));

    ret = netfs_write_begin(&pi->netfs, file, inode->i_mapping,
                            pos, len, &folio, NULL);
    if (ret < 0) {
        pr_warn("powerfs: write_begin netfs_write_begin failed: %d\n", ret);
        return ret;
    }

    WARN_ON_ONCE(!folio_test_locked(folio));
    *pagep = &folio->page;
    *fsdata = NULL;
    return 0;
}

/*
 * powerfs_write_end - 写结束 (Stage C: 纯 page cache 更新, 无网络 IO)
 *
 * 参照 ceph_write_end (fs/ceph/addr.c):
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
int powerfs_write_end(struct file *file, struct address_space *mapping,
                       loff_t pos, unsigned int len, unsigned int copied,
                       struct page *page, void *fsdata)
{
    struct inode *inode = mapping->host;
    struct folio *folio = page_folio(page);
    loff_t end_pos = pos + copied;

    pr_debug("powerfs: write_end ino=%lu pos=%lld copied=%u len=%u i_size=%lld (async)\n",
            inode->i_ino, pos, copied, len, i_size_read(inode));

    if (copied > 0) {
        /* folio 必须 uptodate 才能 mark_dirty (参考 ceph_write_end).
         *
         * netfs_write_begin 对新文件 (pos >= i_size) 会跳过 issue_read,
         * folio 不是 uptodate (netfs_skip_folio_read 返回 true 但只清零
         * 不标记 uptodate). 此时若 copied == len (整 folio 写入), 标记
         * uptodate 继续; 若 copied < len (partial write), 返回 0 让 VFS
         * 重试 (需要先读取现有数据). */
        if (!folio_test_uptodate(folio)) {
            if (copied < len) {
                pr_debug("powerfs: write_end partial write, return 0 for retry\n");
                copied = 0;
                goto out;
            }
            folio_mark_uptodate(folio);
        }
        /* 更新本地 i_size. Filer 端 i_size 由 write_inode 在 writeback
         * 时通过 setattr 同步 (pi->content_size 跟踪上次同步值). */
        if (end_pos > i_size_read(inode)) {
            i_size_write(inode, end_pos);
            mark_inode_dirty(inode);
            pr_debug("powerfs: write_end i_size updated to %lld\n", end_pos);
        }
        /* 标记 folio 脏, 由 writeback 子系统触发 writepages 异步刷盘 */
        folio_mark_dirty(folio);
        pr_debug("powerfs: write_end folio marked dirty, done\n");
    }

out:
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

/* 地址空间操作表 - 内核 6.2 netfs 风格
 *
 * .dirty_folio 必须设置: folio_mark_dirty() 内部通过
 * mapping->a_ops->dirty_folio() 间接调用 (mm/page-writeback.c),
 * 若未设置则为 NULL, write_end 标脏页时触发 NULL instruction fetch oops.
 * 参考 fs/nfs/file.c, fs/btrfs/inode.c, fs/ceph/addr.c (均设置 dirty_folio).
 * 我们不使用 buffer_heads, 故用 filemap_dirty_folio (与 nfs/btrfs/zonefs 一致).
 */
static const struct address_space_operations powerfs_aops = {
    .read_folio    = netfs_read_folio,   /* Stage C: netfs 子系统管理 folio 生命周期 */
    .readahead     = netfs_readahead,    /* Stage C: 批量预读 */
    .writepages    = powerfs_writepages,  /* 批量 writeback (优先于 writepage) */
    .writepage     = powerfs_writepage,   /* fallback: migrate_pages 等内部路径 */
    .write_begin   = powerfs_write_begin,
    .write_end     = powerfs_write_end,
    .dirty_folio   = filemap_dirty_folio,
    .bmap          = powerfs_bmap,
};

/* ========== statfs ========== */

int powerfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    pr_debug("powerfs: statfs\n");

    buf->f_type = POWERFS_SUPER_MAGIC;
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_blocks = 100000000;   /* 100TB */
    buf->f_bfree = 50000000;     /* 50TB free */
    buf->f_bavail = 50000000;
    buf->f_files = 10000000;
    buf->f_ffree = 5000000;
    buf->f_namelen = POWERFS_MAX_NAME_LEN;

    return 0;
}

/* ========== 文件操作 ========== */

/*
 * powerfs_fsync - 数据同步操作
 *
 * file_write_and_wait_range 触发 writepage→powerfs_net_write 将脏页刷到 filer,
 * 然后调用 powerfs_net_setattr 同步 i_size.
 * 断连时 writepage 和 setattr 返回 -ENOTCONN, fsync 传播错误.
 */
static int powerfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    int ret;
    loff_t i_size;

    pr_debug("powerfs: fsync ino=%lu start=%llu end=%llu datasync=%d i_size=%lld\n",
            inode->i_ino, start, end, datasync, i_size_read(inode));

    /* 触发脏页写回 (writepage→powerfs_net_write) */
    ret = file_write_and_wait_range(file, start, end);
    if (ret < 0) {
        pr_warn("powerfs: fsync write_and_wait error: %d\n", ret);
        return ret;
    }

    /* 同步 i_size 到 Filer */
    i_size = i_size_read(inode);
    pr_debug("powerfs: fsync after writeback i_size=%lld\n", i_size);
    if (i_size > 0) {
        int sret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                                        0, 0, 0, (__u64)i_size);
        if (sret < 0) {
            pr_warn("powerfs: fsync setattr size=%llu ino=%lu failed: %d\n",
                    (u64)i_size, inode->i_ino, sret);
            return sret;
        }
    }

    return 0;
}

/*
 * 文件操作表 - 尽可能复用 VFS 通用实现
 *
 * 参考 ramfs_file_operations (fs/ramfs/file-mmu.c)
 */
static const struct file_operations powerfs_file_operations = {
    .read_iter    = generic_file_read_iter,
    .write_iter   = generic_file_write_iter,
    .mmap         = generic_file_mmap,
    .fsync        = powerfs_fsync,
    .splice_read  = generic_file_splice_read,
    .splice_write = iter_file_splice_write,
    .llseek       = generic_file_llseek,
};

/* ========== Inode operations 表 ========== */

/* 目录 inode 操作 */
static const struct inode_operations powerfs_dir_inode_operations = {
    .create     = powerfs_create,
    .lookup     = powerfs_lookup,
    .link       = powerfs_link,
    .unlink     = powerfs_unlink,
    .symlink    = powerfs_symlink,
    .readlink   = powerfs_readlink,
    .mkdir      = powerfs_mkdir,
    .rmdir      = powerfs_rmdir,
    .mknod      = powerfs_mknod,
    .rename     = powerfs_rename,
    .getattr    = powerfs_getattr,
    .setattr    = powerfs_setattr,
};

/* 普通文件 inode 操作 */
static const struct inode_operations powerfs_file_inode_operations = {
    .getattr    = powerfs_getattr,
    .setattr    = powerfs_setattr,
};

/*
 * powerfs_show_options - 显示挂载选项
 */
static int powerfs_show_options(struct seq_file *m, struct dentry *root)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(root->d_sb);

    seq_printf(m, ",master_addr=%s", sbi->master_addr);
    seq_printf(m, ",master_port=%u", sbi->master_port);
    seq_printf(m, ",volume_addr=%s", sbi->volume_addr);
    seq_printf(m, ",volume_port=%u", sbi->volume_port);
    seq_printf(m, ",filer_addr=%s", sbi->filer_addr);
    seq_printf(m, ",filer_port=%u", sbi->filer_port);

    return 0;
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
};

/* ========== 全局 slab 缓存初始化/销毁 ========== */

int powerfs_init_inode_cache(void)
{
    /* inode slab 缓存 */
    powerfs_inode_cachep = kmem_cache_create(
        "powerfs_inode_cache",
        sizeof(struct powerfs_inode_info),
        __alignof__(struct powerfs_inode_info),
        SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
        powerfs_inode_init_once);
    if (!powerfs_inode_cachep)
        return -ENOMEM;

    /* dentry_info slab 缓存 */
    powerfs_dentry_cachep = kmem_cache_create(
        "powerfs_dentry_cache",
        sizeof(struct powerfs_dentry_info),
        __alignof__(struct powerfs_dentry_info),
        SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD,
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
 * 参考 cephfs 设置 dentry operations 的方式
 * 通过 sb->s_d_op 设置所有 dentry 的默认操作
 */
void powerfs_set_sb_dentry_ops(struct super_block *sb)
{
    sb->s_d_op = &powerfs_dentry_operations;
}

/* ========== fill_super: 填充超级块 (fs_context 风格) ========== */

int powerfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
    struct powerfs_ctx_simple {
        char master_addr[64];
        u16  master_port;
        char volume_addr[64];
        u16  volume_port;
        char filer_addr[64];
        u16  filer_port;
        u32  write_batch_kb;
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

    pr_debug("powerfs: fill_super (sb->s_fs_info=%px, master_addr='%s')\n",
            ctx, ctx ? ctx->master_addr : "(null)");

    /* 创建超级块私有信息 */
    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sbi->sb = sb;

    /* 保存挂载参数 (ctx 来自 init_fs_context 通过 sget_fc 转移到 sb->s_fs_info) */
    if (ctx) {
        strncpy(sbi->master_addr, ctx->master_addr, sizeof(sbi->master_addr) - 1);
        sbi->master_port = ctx->master_port;
        strncpy(sbi->volume_addr, ctx->volume_addr, sizeof(sbi->volume_addr) - 1);
        sbi->volume_port = ctx->volume_port;
        strncpy(sbi->filer_addr, ctx->filer_addr, sizeof(sbi->filer_addr) - 1);
        sbi->filer_port = ctx->filer_port;
        batch_kb = ctx->write_batch_kb;
        /* 释放 init_fs_context 分配的 ctx, 释放后 sb->s_fs_info 仍指向已释放内存,
         * 必须在下方设置 sb->s_fs_info = sbi 之前完成 */
        kfree(ctx);
        ctx = NULL;
    }

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
    pr_debug("powerfs: write_batch_kb=%u → write_batch_pages=%d\n",
            batch_kb, sbi->write_batch_pages);

    /* 初始化 inode 号分配器 (从 100 开始，1 是 root) */
    atomic_set(&sbi->next_ino, 100);

    /* 设置超级块 (覆盖 sb->s_fs_info, 之前指向已释放的 ctx) */
    sb->s_fs_info = sbi;
    sb->s_op = &powerfs_super_ops;
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
     * 参考: ceph_fill_super (fs/ceph/super.c) 调用 super_setup_bdi.
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

    /* 创建异步 inode 刷新工作队列 (NOTIFY → getattr 刷新元数据) */
    powerfs_refresh_wq = alloc_workqueue("powerfs_refresh",
                                          WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
    if (!powerfs_refresh_wq) {
        pr_err("powerfs: failed to create refresh workqueue\n");
        return -ENOMEM;
    }

    /* === 初始化 powerfs_net 连接池 (多节点 Delta Sync) === */
    powerfs_net_pool_init();

    /* 配置 Filer 节点 (Delta Sync 的主节点).
     * Filer 列表获取策略 (优先级递减):
     *   1. 若配置了 master_addr, 调用 powerfs_net_discover_filers 从
     *      Master 动态发现 filer 列表 (支持几百个 filer 的扩展场景).
     *   2. 发现失败时, 回退到 sbi->filer_addr / g_server_addr 手动解析
     *      (逗号分隔多地址), 保证向后兼容.
     *
     * 注意: ctx 已在上方 kfree, 这里使用 sbi 中保存的挂载参数.
     * Master 节点也加入连接池 (供后续 Master 交互使用, 如 GetTopology). */
    {
        const char *maddr = (sbi->master_addr[0]) ? sbi->master_addr : NULL;
        __u16 mport = sbi->master_port ? sbi->master_port : 9334;
        int discovered = 0;

        /* 先添加 Master 到连接池 (discover_filers 需要) */
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
        }

        /* 尝试从 Master 发现 filer 列表 */
        if (maddr) {
            discovered = powerfs_net_discover_filers(maddr, mport);
            if (discovered > 0) {
                pr_debug("powerfs: discovered %d filers via Master\n",
                        discovered);
            } else {
                pr_warn("powerfs: Master filer discovery failed (%d), "
                        "falling back to manual filer_addr\n", discovered);
            }
        }

        /* 回退: 手动解析 filer_addr (模块参数 filer_addr=...) */
        if (discovered <= 0) {
            const char *faddr = sbi->filer_addr;
            __u16 fport = sbi->filer_port;
            if (faddr && faddr[0]) {
                char addr_buf[256];
                char *p, *tok;
                bool first = true;

                strncpy(addr_buf, faddr, sizeof(addr_buf) - 1);
                addr_buf[sizeof(addr_buf) - 1] = '\0';

                p = addr_buf;
                while ((tok = strsep(&p, ",")) != NULL) {
                    if (tok[0] == '\0')
                        continue;
                    while (*tok == ' ')
                        tok++;
                    if (tok[0] == '\0')
                        continue;
                    powerfs_net_add_server(tok, fport,
                                          POWERFS_NET_SERVER_FILER);
                    if (first) {
                        powerfs_net_set_primary(tok, fport);
                        first = false;
                    }
                    pr_debug("powerfs: added filer %s:%u (manual)\n",
                            tok, fport);
                }
            }
        }
    }

    /* 配置 Volume 节点 (支持逗号分隔多地址) */
    if (sbi->volume_addr[0]) {
        char vbuf[256];
        char *vp, *vtok;
        strncpy(vbuf, sbi->volume_addr, sizeof(vbuf) - 1);
        vbuf[sizeof(vbuf) - 1] = '\0';
        vp = vbuf;
        while ((vtok = strsep(&vp, ",")) != NULL) {
            while (*vtok == ' ') vtok++;
            if (vtok[0] == '\0') continue;
            powerfs_net_add_server(vtok, sbi->volume_port,
                                  POWERFS_NET_SERVER_VOLUME);
            pr_debug("powerfs: added volume %s:%u\n", vtok,
                    sbi->volume_port);
        }
    }

    /* 初始化新连接池.
     *
     * 新架构 (per-conn 状态机 + shard 路由 + 事件驱动) 为唯一路径.
     * 旧 g_conn 单连接 fallback 已移除, pool_init 失败直接返回错误.
     * 断连检测由 sk_state_change 回调 + TCP keepalive 取代, 无需健康监控线程.
     *
     * P3.2: 传入 master_addr, 让 g_pool.master_addr 被设置, 供后续
     * discover_volumes 及其他需要 Master 交互的场景使用. */
    {
        const char *maddr = sbi->master_addr[0] ? sbi->master_addr : NULL;
        __u16 mport = sbi->master_port ? sbi->master_port : 9334;
        int pool_ret = powerfs_conn_pool_init(maddr, mport);
        if (pool_ret != 0) {
            pr_err("powerfs: connection pool init failed (%d)\n", pool_ret);
            return pool_ret;
        }
        pr_debug("powerfs: new connection pool initialized (sk callback + keepalive)\n");

        /* P3.3: 从 Master GetTopology 发现 volume 路由表 (volume_id → conn_idx).
         * 前提: volume 连接已由 conn_pool_init 建立 (g_pool.volumes[] 已填充).
         * discover_volumes 按 addr 匹配已建立连接, 建立路由映射.
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

    /* 2. 销毁 writeback workqueue (Stage C).
     *    sync_filesystem 已触发 writeback 并等待 PageWriteback 清除,
     *    此时 workqueue 中所有 work 应已完成. destroy_workqueue 会
     *    drain 剩余 work (若有), 然后销毁. 必须在关闭网络前销毁,
     *    否则 work_fn 中的 powerfs_net_write 访问已关闭的网络. */
    if (sbi && sbi->writeback_wq) {
        destroy_workqueue(sbi->writeback_wq);
        sbi->writeback_wq = NULL;
    }

    /* 2b. 销毁 inode 刷新工作队列 (NOTIFY → getattr) */
    if (powerfs_refresh_wq) {
        destroy_workqueue(powerfs_refresh_wq);
        powerfs_refresh_wq = NULL;
    }

    /* 3. 设置 stopping 标志: 让 send_request 立即返回 -ENOTCONN,
     *    阻止 reconnect_work 在 g_pool 清零后访问野指针. */
    powerfs_net_set_stopping();

    /* 4. 关闭所有连接 (g_pool 会被清零) */
    powerfs_net_pool_cleanup();

    if (sbi) {
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

    /* 根目录的父目录是自己 */
    pi->parent_ino = POWERFS_ROOT_INO;
    pi->dir_complete = true;  /* 根目录初始为空，认为 complete */

    /* 根目录设置 uid/gid 为 0 */
    root->i_uid = GLOBAL_ROOT_UID;
    root->i_gid = GLOBAL_ROOT_GID;

    pr_debug("powerfs: root inode created, ino=%lu\n", root->i_ino);
    return root;
}
