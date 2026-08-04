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
#include <linux/writeback.h>     /* folio_mark_dirty (set_page_dirty 未导出到运行内核) */

#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_net.h"

/* netfs 请求操作 (Step 2 实现 issue_read，Step 0 先空声明) */
static const struct netfs_request_ops powerfs_netfs_ops;

/* ========== 全局 slab 缓存 (参考 ceph 全局 cache) ========== */

static struct kmem_cache *powerfs_inode_cachep;
static struct kmem_cache *powerfs_dentry_cachep;

/* 全局超级块指针 (用于跨模块访问) */
static struct super_block *g_powerfs_sb;

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
 * d_release - dentry 销毁前释放私有数据
 *
 * 参考 ceph_d_release (fs/ceph/dir.c)
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

    dentry->d_fsdata = NULL;
    kmem_cache_free(powerfs_dentry_cachep, di);
}

/*
 * powerfs_comm_connected - 检查通信层是否已连接
 *
 * 通过检查全局连接标志来判断
 * 用户态代理打开字符设备后标记为已连接
 */
static bool powerfs_comm_connected(void)
{
    return powerfs_comm_is_connected();
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
 *
 * Delta Sync 策略:
 *   - 检查 dentry 的 generation 是否过期
 *   - 如果 generation 过期 (powerfs_net_path_stale 返回 true)，返回 0 失效
 *   - 如果 powerfs_net 未连接 (本地模式)，始终返回 1
 *   - 对于目录 dentry，基于 TTL 进行简单过期检查
 */
int powerfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
    struct powerfs_dentry_info *di;
    struct inode *inode;
    struct powerfs_inode_info *pi;

    /* RCU 模式: 快速检查 TTL */
    if (flags & LOOKUP_RCU) {
        return 1;  /* RCU 模式下不做完整验证 */
    }

    /* 获取 dentry 私有数据 */
    di = dentry->d_fsdata;
    if (!di)
        return 1;

    /* 如果没有 inode，负 dentry 始终有效 */
    inode = d_inode(dentry);
    if (!inode)
        return 1;

    /* 获取 inode 私有数据 */
    pi = POWERFS_I(inode);

    /*
     * 检查 1: TTL 过期检查
     * 如果超过 TTL，强制失效以获取最新数据
     */
    if (time_after(jiffies, di->lease_expire)) {
        pr_debug("powerfs: d_revalidate '%pd' TTL expired\n", dentry);
        spin_lock(&pi->i_lock);
        pi->cache_valid = false;
        spin_unlock(&pi->i_lock);
        return 0;
    }

    /*
     * 检查 2: inode 级别的 cache_valid 检查
     */
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
 * d_delete - d_count 归零时决定是否立即删除
 *
 * 参考 ceph_d_delete (fs/ceph/dir.c)
 *
 * 返回 0: 保留在 dcache LRU 缓存中
 * 返回 1: 立即删除 dentry
 *
 * 策略:
 *   - 返回 0: 保留 dentry 在 dcache 中
 *     这样 stat 等操作可以复用已缓存的 dentry
 *     避免频繁的路径解析导致的性能问题
 */
int powerfs_d_delete(const struct dentry *dentry)
{
    /*
     * 返回 0: 保留 dentry 在 dcache LRU 中, 供 stat 等操作复用.
     *
     * 参考 ramfs: ramfs 没有 d_delete 回调, VFS 默认保留 dentry.
     * 参考 ceph: ceph_d_delete 返回 0 保留 dentry (供 cap 复用).
     *
     * inode 生命周期完全由 VFS dentry 引用管理:
     *   d_count 归零 -> __dentry_kill -> dentry_unlink_inode -> iput
     * 文件系统不应在 unlink/rmdir 中 ihold, 否则 i_count 泄漏.
     */
    (void)dentry;
    return 0;
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

    if (!parent)
        return;

    dir = d_inode(parent);
    if (!dir || !S_ISDIR(dir->i_mode))
        return;

    ppi = POWERFS_I(dir);

    pr_debug("powerfs: d_prune '%pd' (parent=%pd, clearing dir_complete)\n",
             dentry, parent);

    /* 清除父目录的 complete 标志 */
    spin_lock(&ppi->i_lock);
    ppi->dir_complete = false;
    spin_unlock(&ppi->i_lock);
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

    pr_info("powerfs: init_inode ino=%lu mode=%o, S_IFDIR=%d\n",
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

    /* 初始化目录项链表 */
    INIT_LIST_HEAD(&pi->dir_entries);
    mutex_init(&pi->dir_mutex);

    /* 根据文件类型设置操作表 */
    switch (mode & S_IFMT) {
    case S_IFREG:
        inode->i_op = &powerfs_file_inode_operations;
        inode->i_fop = &powerfs_file_operations;
        set_nlink(inode, 1);
        pr_info("powerfs: init_inode REG, i_fop=%p\n", inode->i_fop);
        break;

    case S_IFDIR:
        inode->i_op = &powerfs_dir_inode_operations;
        inode->i_fop = &powerfs_dir_operations;
        set_nlink(inode, 2);  /* "." + ".." */
        pi->dir_complete = true;  /* 新建目录为空，认为 complete */
        pr_info("powerfs: init_inode DIR, i_fop=%p\n", inode->i_fop);
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

/* ========== 通信层辅助函数 ========== */

/*
 * powerfs_comm_lookup - 通过通信层查找文件
 *
 * 返回 0 表示找到，resp 中有文件信息
 * 返回 -ENOENT 表示文件不存在
 * 返回负值表示错误
 */
static int powerfs_comm_lookup(struct inode *dir, const char *name,
                                struct powerfs_lookup_resp *resp)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_lookup_req *req_data;
    int ret;

    req_data = kmalloc(sizeof(*req_data), GFP_KERNEL);
    if (!req_data)
        return -ENOMEM;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_LOOKUP;
    req_hdr.ino = dir->i_ino;
    req_hdr.data_len = sizeof(*req_data);

    memset(req_data, 0, sizeof(*req_data));
    req_data->dir_ino = dir->i_ino;
    strncpy(req_data->name, name, sizeof(req_data->name) - 1);

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(resp, 0, sizeof(*resp));

    /* lookup 使用较短超时 (1秒)，避免高并发下长时间阻塞 */
    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, resp, 1000);
    kfree(req_data);
    if (ret == -ETIMEDOUT) {
        /* 超时视为文件不存在，避免 VFS 重试导致更多请求积压 */
        return -ENOENT;
    }
    if (ret < 0)
        return ret;

    /* 返回响应状态码 (0=成功, -ENOENT 等) */
    return resp_hdr.status;
}

/*
 * powerfs_comm_mkdir - 通过通信层创建目录
 *
 * 返回 0 表示成功，resp 中有新目录信息
 * 返回负值表示错误
 */
static int powerfs_comm_create(struct inode *dir, const char *name,
                                umode_t mode, u64 new_ino,
                                struct powerfs_create_resp *resp)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_create_req *req_data;
    int ret;

    req_data = kmalloc(sizeof(*req_data), GFP_KERNEL);
    if (!req_data)
        return -ENOMEM;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = S_ISDIR(mode) ? POWERFS_MSG_MKDIR : POWERFS_MSG_CREATE;
    req_hdr.ino = dir->i_ino;
    req_hdr.data_len = sizeof(*req_data);

    memset(req_data, 0, sizeof(*req_data));
    req_data->dir_ino = dir->i_ino;
    req_data->new_ino = new_ino;
    req_data->mode = mode;
    req_data->uid = from_kuid(&init_user_ns, current_fsuid());
    req_data->gid = from_kgid(&init_user_ns, current_fsgid());
    strncpy(req_data->name, name, sizeof(req_data->name) - 1);

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(resp, 0, sizeof(*resp));

    /* 使用 200ms 短超时，避免高并发下长时间阻塞 */
    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, resp, 200);
    kfree(req_data);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/*
 * powerfs_comm_unlink - 通过通信层删除文件/目录
 *
 * 返回 0 表示成功
 * 返回负值表示错误
 */
static int powerfs_comm_remove(struct inode *dir, const char *name, bool is_dir)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_remove_req *req_data;
    int ret;

    req_data = kmalloc(sizeof(*req_data), GFP_KERNEL);
    if (!req_data)
        return -ENOMEM;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = is_dir ? POWERFS_MSG_RMDIR : POWERFS_MSG_UNLINK;
    req_hdr.ino = dir->i_ino;
    req_hdr.data_len = sizeof(*req_data);

    memset(req_data, 0, sizeof(*req_data));
    req_data->dir_ino = dir->i_ino;
    strncpy(req_data->name, name, sizeof(req_data->name) - 1);

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, NULL, 500);
    kfree(req_data);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/*
 * powerfs_comm_link - 通过通信层创建硬链接
 */
static int powerfs_comm_link(struct inode *dir, uint64_t target_ino,
                             const char *name)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_link_req *req_data;
    int ret;

    req_data = kmalloc(sizeof(*req_data), GFP_KERNEL);
    if (!req_data)
        return -ENOMEM;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_LINK;
    req_hdr.ino = target_ino;
    req_hdr.data_len = sizeof(*req_data);

    memset(req_data, 0, sizeof(*req_data));
    req_data->ino = target_ino;
    req_data->dir_ino = dir->i_ino;
    strncpy(req_data->name, name, sizeof(req_data->name) - 1);

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, NULL, 500);
    kfree(req_data);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/*
 * powerfs_comm_symlink - 通过通信层创建符号链接
 */
static int powerfs_comm_symlink(struct inode *dir, const char *name,
                                const char *symname)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_symlink_req *req_data;
    int ret;

    req_data = kmalloc(sizeof(*req_data), GFP_KERNEL);
    if (!req_data)
        return -ENOMEM;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_SYMLINK;
    req_hdr.ino = dir->i_ino;
    req_hdr.data_len = sizeof(*req_data);

    memset(req_data, 0, sizeof(*req_data));
    req_data->dir_ino = dir->i_ino;
    req_data->name_len = strlen(name);
    req_data->symname_len = strlen(symname);
    strncpy(req_data->name, name, sizeof(req_data->name) - 1);
    strncpy(req_data->symname, symname, sizeof(req_data->symname) - 1);

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    pr_debug("powerfs: comm_symlink: dir_ino=%lu name=%s symname=%s\n",
             dir->i_ino, name, symname);

    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, NULL, 500);
    kfree(req_data);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/*
 * powerfs_comm_getattr - 通过通信层获取文件属性
 *
 * 返回 0 表示成功，resp 中有属性信息
 * 返回负值表示错误
 */
static int powerfs_comm_getattr(struct inode *inode,
                                 struct powerfs_getattr_resp *resp)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_getattr_req req_data;
    int ret;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_GETATTR;
    req_hdr.ino = inode->i_ino;
    req_hdr.data_len = sizeof(req_data);

    memset(&req_data, 0, sizeof(req_data));
    req_data.ino = inode->i_ino;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(resp, 0, sizeof(*resp));

    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, resp, 200);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/*
 * powerfs_comm_setattr - 通过通信层设置文件属性
 *
 * 返回 0 表示成功，resp 中有更新后的属性
 * 返回负值表示错误
 */
static int powerfs_comm_setattr(struct inode *inode, struct iattr *attr,
                                 struct powerfs_setattr_resp *resp)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_setattr_req req_data;
    int ret;
    u32 ia_valid = 0;

    memset(&req_data, 0, sizeof(req_data));
    req_data.ino = inode->i_ino;

    /* 转换属性掩码 */
    if (attr->ia_valid & ATTR_MODE) {
        ia_valid |= POWERFS_ATTR_MODE;
        req_data.mode = attr->ia_mode;
    }
    if (attr->ia_valid & ATTR_UID) {
        ia_valid |= POWERFS_ATTR_UID;
        req_data.uid = from_kuid(&init_user_ns, attr->ia_uid);
    }
    if (attr->ia_valid & ATTR_GID) {
        ia_valid |= POWERFS_ATTR_GID;
        req_data.gid = from_kgid(&init_user_ns, attr->ia_gid);
    }
    if (attr->ia_valid & ATTR_SIZE) {
        ia_valid |= POWERFS_ATTR_SIZE;
        req_data.size = attr->ia_size;
    }
    if (attr->ia_valid & ATTR_ATIME) {
        ia_valid |= POWERFS_ATTR_ATIME;
        req_data.atime_sec = attr->ia_atime.tv_sec;
        req_data.atime_nsec = attr->ia_atime.tv_nsec;
    }
    if (attr->ia_valid & ATTR_MTIME) {
        ia_valid |= POWERFS_ATTR_MTIME;
        req_data.mtime_sec = attr->ia_mtime.tv_sec;
        req_data.mtime_nsec = attr->ia_mtime.tv_nsec;
    }
    if (attr->ia_valid & ATTR_CTIME) {
        ia_valid |= POWERFS_ATTR_CTIME;
        req_data.ctime_sec = attr->ia_ctime.tv_sec;
        req_data.ctime_nsec = attr->ia_ctime.tv_nsec;
    }

    req_data.ia_valid = ia_valid;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_SETATTR;
    req_hdr.ino = inode->i_ino;
    req_hdr.data_len = sizeof(req_data);

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(resp, 0, sizeof(*resp));

    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, resp, 200);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/*
 * powerfs_comm_rename - 通过通信层重命名文件
 *
 * 返回 0 表示成功
 * 返回负值表示错误
 */
static int powerfs_comm_rename(struct inode *old_dir, const char *old_name,
                                struct inode *new_dir, const char *new_name,
                                unsigned int flags)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_rename_req req_data;
    int ret;

    memset(&req_data, 0, sizeof(req_data));
    req_data.old_dir_ino = old_dir->i_ino;
    req_data.new_dir_ino = new_dir->i_ino;
    req_data.flags = flags;
    strncpy(req_data.old_name, old_name, sizeof(req_data.old_name) - 1);
    strncpy(req_data.new_name, new_name, sizeof(req_data.new_name) - 1);
    req_data.old_name_len = strlen(old_name);
    req_data.new_name_len = strlen(new_name);

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_RENAME;
    req_hdr.ino = old_dir->i_ino;
    req_hdr.data_len = sizeof(req_data);

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, NULL, 500);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
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
/* 构建 dentry 的完整路径 (用于 Delta Sync) */
static void powerfs_build_dentry_path(struct dentry *dentry, char *path, size_t path_len)
{
    char (*names)[256];
    struct dentry *cur;
    int depth = 0;
    int i;
    size_t total_len;

    if (!dentry || !path || path_len == 0)
        return;

    names = kmalloc_array(64, sizeof(*names), GFP_KERNEL);
    if (!names) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    /* 向上遍历到根，收集各层名称 */
    cur = dentry;
    while (cur && !IS_ROOT(cur)) {
        if (depth >= 63) break;  /* 防止过深递归 */
        strncpy(names[depth], cur->d_name.name, 255);
        names[depth][255] = '\0';
        depth++;
        cur = cur->d_parent;
    }

    /* 从根向下构建路径 */
    path[0] = '/';
    path[1] = '\0';
    total_len = 1;
    for (i = depth - 1; i >= 0 && total_len < path_len - 1; i--) {
        size_t name_len = strlen(names[i]);
        size_t need = name_len + (i > 0 ? 1 : 0);

        if (total_len + need >= path_len)
            break;

        if (i < depth - 1) {
            /* 非最后一个组件，添加 "/" */
            path[total_len++] = '/';
        }
        memcpy(path + total_len, names[i], name_len);
        total_len += name_len;
    }
    path[total_len] = '\0';

    kfree(names);
}

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

        pr_debug("powerfs: lookup '%pd' via powerfs_net\n", dentry);

        /* 通过 powerfs_net 直接查询 (含时间戳) */
        err = powerfs_net_lookup(dir->i_ino, dentry->d_name.name,
                                  strlen(dentry->d_name.name),
                                  &ino, &mode, &uid, &gid,
                                  &size, &nlink,
                                  &mtime, &atime, &ctime);

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
                    spin_unlock(&pi->i_lock);
                }

                unlock_new_inode(inode);
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
            /* 文件不存在: 添加负 dentry */
            pr_debug("powerfs: lookup '%pd' not found (powerfs_net)\n", dentry);
            d_add(dentry, NULL);
            return NULL;
        }

        /* 其他错误: 记录但仍添加负 dentry */
        pr_warn("powerfs: lookup '%pd' powerfs_net error: %d\n", dentry, err);
        d_add(dentry, NULL);
        return NULL;
    }

    /* === 兼容旧 powerfs_comm 接口 === */
    if (powerfs_comm_connected()) {
        struct powerfs_lookup_resp resp;
        umode_t mode;

        pr_debug("powerfs: lookup '%pd' via powerfs_comm (legacy)\n", dentry);

        err = powerfs_comm_lookup(dir, dentry->d_name.name, &resp);
        if (err == 0) {
            /* 找到文件: 创建 inode */
            mode = resp.mode;

            inode = powerfs_iget(dir->i_sb, resp.ino);
            if (IS_ERR(inode)) {
                pr_warn("powerfs: lookup '%pd' iget failed: %ld\n",
                        dentry, PTR_ERR(inode));
                d_add(dentry, NULL);
                return NULL;
            }

            if (inode->i_state & I_NEW) {
                /*
                 * 新 inode: 先用 powerfs_init_inode 设置 i_op/i_fop/a_ops
                 * (与 net 路径同因)，再用响应数据覆盖。
                 */
                powerfs_init_inode(inode, mode, dir->i_ino,
                                   dentry->d_name.name);
                inode->i_mode = mode;
                inode->i_uid = make_kuid(&init_user_ns, resp.uid);
                inode->i_gid = make_kgid(&init_user_ns, resp.gid);
                inode->i_size = resp.size;
                set_nlink(inode, resp.nlink);
                inode->i_mtime.tv_sec = resp.mtime_sec;
                inode->i_mtime.tv_nsec = 0;
                inode->i_atime = inode->i_mtime;
                inode->i_ctime = inode->i_mtime;

                {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->cache_valid = true;
                    spin_unlock(&pi->i_lock);
                }

                unlock_new_inode(inode);
            }

            /* 实例化 dentry: 必须用 d_add (见 net 路径注释) */
            d_add(dentry, inode);

            /* 更新 dentry 租约时间 */
            {
                struct powerfs_dentry_info *di = dentry->d_fsdata;
                if (di) {
                    di->lease_expire = jiffies + POWERFS_DENTRY_LEASE_TTL;
                    di->time = jiffies;
                }
            }

            pr_debug("powerfs: lookup '%pd' found ino=%llu mode=%o (legacy)\n",
                     dentry, (unsigned long long)resp.ino, mode);
            return NULL;
        }

        if (err == -ENOENT || err == -ETIMEDOUT) {
            /* 文件不存在: 添加负 dentry */
            pr_debug("powerfs: lookup '%pd' not found (comm)\n", dentry);
            d_add(dentry, NULL);
            return NULL;
        }

        /* 其他错误 */
        pr_warn("powerfs: lookup '%pd' comm error: %d\n", dentry, err);
        d_add(dentry, NULL);
        return NULL;
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
        int rerr = powerfs_net_create(dir->i_ino, dentry->d_name.name,
                                       dentry->d_name.len, mode,
                                       from_kuid(&init_user_ns, current_fsuid()),
                                       from_kgid(&init_user_ns, current_fsgid()),
                                       S_ISDIR(mode), &remote_ino);
        if (rerr) {
            pr_warn("powerfs: net_create '%pd' failed: %d\n", dentry, rerr);
            return rerr;
        }
        new_ino = remote_ino ? remote_ino
                             : (u64)atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
    } else {
        new_ino = atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
    }

    /* 创建 inode */
    inode = powerfs_new_inode(dir->i_sb, mode, new_ino,
                               dir->i_ino, dentry->d_name.name);
    if (!inode)
        return -ENOSPC;

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

    /* === Delta Sync: 失效路径缓存 === */
    {
        char path_buf[256];

        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
        /* 在创建后触发 generation 失效，强制后续操作同步 */
        powerfs_net_invalidate_path(path_buf);
        powerfs_net_invalidate_dir(dir->i_ino);
    }

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

    /* === powerfs_net 同步: 同步创建目录到远端 === */
    if (inode && powerfs_net_is_connected()) {
        char path_buf[256];

        /* 构建路径用于 Delta Sync */
        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));

        /* 触发 Delta Sync: 失效缓存，后续操作将通过 pull_delta 获取最新状态 */
        powerfs_net_invalidate_path(path_buf);
        powerfs_net_invalidate_dir(dir->i_ino);

        pr_debug("powerfs: mkdir '%pd' synced to powerfs_net\n", dentry);
    }

    /* === 兼容旧 powerfs_comm 接口 === */
    if (inode && powerfs_comm_connected()) {
        struct powerfs_msg_header req_hdr;
        struct powerfs_create_req req_data;

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_MKDIR;
        req_hdr.ino = dir->i_ino;
        req_hdr.data_len = sizeof(req_data);

        memset(&req_data, 0, sizeof(req_data));
        req_data.dir_ino = dir->i_ino;
        req_data.mode = mode | S_IFDIR;
        req_data.uid = from_kuid(&init_user_ns, current_fsuid());
        req_data.gid = from_kgid(&init_user_ns, current_fsgid());
        req_data.new_ino = inode->i_ino;
        strncpy(req_data.name, dentry->d_name.name, sizeof(req_data.name) - 1);

        powerfs_comm_submit_notify(&req_hdr, &req_data);
    }

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

    /* 失效路径缓存 */
    {
        char path_buf[256];
        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
        powerfs_net_invalidate_path(path_buf);
        powerfs_net_invalidate_dir(dir->i_ino);
    }

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

    /* === powerfs_net: 触发 Delta Sync 失效 === */
    if (inode && powerfs_net_is_connected()) {
        char path_buf[256];

        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
        powerfs_net_invalidate_path(path_buf);
        powerfs_net_invalidate_dir(dir->i_ino);

        pr_debug("powerfs: create '%pd' delta sync invalidated\n", dentry);
    }

    /* === 兼容旧 powerfs_comm 接口 === */
    if (inode && powerfs_comm_connected()) {
        struct powerfs_msg_header req_hdr;
        struct powerfs_create_req req_data;

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_CREATE;
        req_hdr.ino = dir->i_ino;
        req_hdr.data_len = sizeof(req_data);

        memset(&req_data, 0, sizeof(req_data));
        req_data.dir_ino = dir->i_ino;
        req_data.new_ino = inode->i_ino;
        req_data.mode = mode | S_IFREG;
        req_data.uid = from_kuid(&init_user_ns, current_fsuid());
        req_data.gid = from_kgid(&init_user_ns, current_fsgid());
        strncpy(req_data.name, dentry->d_name.name, sizeof(req_data.name) - 1);

        powerfs_comm_submit_notify(&req_hdr, &req_data);
    }

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

    /* 失效路径缓存 */
    {
        char path_buf[256];
        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
        powerfs_net_invalidate_path(path_buf);
        powerfs_net_invalidate_dir(dir->i_ino);
    }

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
    pr_info("powerfs: SETATTR ino=%lu ia_valid=0x%x ia_size=%lld cur_size=%lld\n",
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
            pr_info("powerfs: SETATTR net ino=%lu valid=0x%x sz=%llu sret=%d\n",
                    inode->i_ino, valid, sz, sret);
            if (sret < 0)
                pr_warn("powerfs: setattr net sync ino=%lu failed: %d\n",
                        inode->i_ino, sret);
        }
    }

    /* 第三步: 异步通知代理更新后端记录 */
    if (powerfs_comm_connected()) {
        struct powerfs_msg_header req_hdr;
        struct powerfs_setattr_req req_data;

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_SETATTR;
        req_hdr.ino = inode->i_ino;
        req_hdr.data_len = sizeof(req_data);

        memset(&req_data, 0, sizeof(req_data));
        req_data.ino = inode->i_ino;
        req_data.ia_valid = ia_valid;
        if (ia_valid & ATTR_MODE)
            req_data.mode = inode->i_mode;
        if (ia_valid & ATTR_UID)
            req_data.uid = from_kuid(&init_user_ns, inode->i_uid);
        if (ia_valid & ATTR_GID)
            req_data.gid = from_kgid(&init_user_ns, inode->i_gid);
        if (ia_valid & ATTR_SIZE)
            req_data.size = i_size_read(inode);
        if (ia_valid & ATTR_ATIME) {
            req_data.atime_sec = inode->i_atime.tv_sec;
            req_data.atime_nsec = inode->i_atime.tv_nsec;
        }
        if (ia_valid & ATTR_MTIME) {
            req_data.mtime_sec = inode->i_mtime.tv_sec;
            req_data.mtime_nsec = inode->i_mtime.tv_nsec;
        }
        if (ia_valid & ATTR_CTIME) {
            req_data.ctime_sec = inode->i_ctime.tv_sec;
            req_data.ctime_nsec = inode->i_ctime.tv_nsec;
        }

        powerfs_comm_submit_notify(&req_hdr, &req_data);
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

    pr_info("powerfs: dir_open ino=%lu\n", inode->i_ino);

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

    pr_info("powerfs: dir_open success, fop=%p\n", file->f_op);

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
    if (!powerfs_comm_connected())
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

    /* 如果目录缓存为空且网络可用，从 Filer 获取目录列表 */
    if (list_empty(&dpi->dir_entries) && powerfs_net_is_connected()) {
        struct powerfs_net_dir_entry *net_entries;
        __u32 net_count = 0;
        bool has_more = false;
        char last_name[256] = "";
        int ret;

        net_entries = kmalloc_array(256, sizeof(*net_entries), GFP_KERNEL);
        if (!net_entries)
            return -ENOMEM;

        /* 从 Filer 获取目录条目 (分页循环直到获取全部) */
        do {
            ret = powerfs_net_readdir(dir->i_ino, last_name, 256,
                                      net_entries, 256,
                                      &net_count, &has_more);
            if (ret < 0) {
                pr_warn("powerfs: readdir ino=%lu net error: %d\n",
                        dir->i_ino, ret);
                kfree(net_entries);
                return ret;
            }

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
        dpi->dir_complete = true;
    }

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

/*
 * powerfs_read_folio - 读取页面
 *
 * 当 powerfs_net 连接时: 从 filer 读取数据填充 folio (跨 client 可见),
 *   remount 后读取持久化数据由此完成.
 * 未连接时: 本地缓存模式, 零填充.
 */
static int powerfs_read_folio(struct file *file, struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    loff_t offset = folio_pos(folio);
    size_t count = folio_size(folio);
    __u32 read_len = 0;
    char *kaddr;
    int err;

    pr_debug("powerfs: read_folio ino=%lu index=%lu\n",
             inode->i_ino, folio->index);

    /* 本地缓存模式: 零填充 */
    if (!powerfs_net_is_connected()) {
        folio_zero_range(folio, 0, count);
        folio_mark_uptodate(folio);
        folio_unlock(folio);
        return 0;
    }

    /* 超出文件大小的部分零填充 */
    if (offset >= i_size_read(inode)) {
        folio_zero_range(folio, 0, count);
        folio_mark_uptodate(folio);
        folio_unlock(folio);
        return 0;
    }

    kaddr = kmap_local_folio(folio, 0);
    err = powerfs_net_read(inode->i_ino, offset, count, kaddr, count, &read_len);
    kunmap_local(kaddr);

    if (err) {
        pr_warn("powerfs: read_folio ino=%lu offset=%llu failed: %d\n",
                inode->i_ino, offset, err);
        /* 读失败时零填充, 避免向上层传播 IO error 导致读路径死锁 */
        folio_zero_range(folio, 0, count);
    } else if (read_len < count) {
        /* 不足部分零填充 */
        folio_zero_range(folio, read_len, count - read_len);
    }

    folio_mark_uptodate(folio);
    folio_unlock(folio);
    return 0;
}

/*
 * powerfs_writepage - 写入页面
 *
 * 当 powerfs_net 连接时: 同步将页数据写到 filer (filer 转发 volume server),
 *   实现数据持久化; fsync/release 触发 writeback 时由此完成刷盘.
 * 未连接时: 本地缓存模式, 仅标记干净.
 */
int powerfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = page->mapping->host;
    loff_t offset = page_offset(page);
    size_t count = PAGE_SIZE;
    __u32 written = 0;
    char *kaddr;
    int err = 0;

    pr_debug("powerfs: writepage ino=%lu index=%lu\n",
             inode->i_ino, page->index);

    /* 本地缓存模式: 直接标记干净 */
    if (!powerfs_net_is_connected()) {
        end_page_writeback(page);
        return 0;
    }

    /* 超出文件大小的页不写 */
    if (offset >= i_size_read(inode)) {
        end_page_writeback(page);
        return 0;
    }
    /* 最后一页截断到 i_size */
    if (offset + count > i_size_read(inode))
        count = i_size_read(inode) - offset;

    kaddr = kmap_local_page(page);
    err = powerfs_net_write(inode->i_ino, offset, kaddr, count, &written);
    kunmap_local(kaddr);

    if (err) {
        pr_warn("powerfs: writepage ino=%lu offset=%llu failed: %d\n",
                inode->i_ino, offset, err);
        SetPageError(page);
        mapping_set_error(page->mapping, err);
    }

    end_page_writeback(page);
    return err;
}

/*
 * powerfs_write_begin - 写开始 (准备页面)
 *
 * 参考 ramfs_write_begin / __generic_write_begin
 *
 * 对于纯内存模式，我们可以直接使用 grab_cache_page_write_begin
 */
int powerfs_write_begin(struct file *file, struct address_space *mapping,
                         loff_t pos, unsigned int len, struct page **pagep,
                         void **fsdata)
{
    struct inode *inode = mapping->host;
    pgoff_t index = pos >> PAGE_SHIFT;
    struct page *page;
    int ret;

    pr_debug("powerfs: write_begin ino=%lu pos=%lld len=%u\n",
             inode->i_ino, pos, len);

    page = grab_cache_page_write_begin(mapping, index);
    if (!page)
        return -ENOMEM;

    *pagep = page;
    *fsdata = NULL;

    /*
     * 如果页面不是最新的，直接清零整个页面
     *
     * 参考 ramfs_write_begin: 不从后端读取，直接清零
     *
     * 原因:
     *   1. 数据存储在 page cache 中，write_end 会更新 page cache
     *   2. read_folio 会在首次读取时从代理加载（如果页面不在 page cache）
     *   3. write_begin 中调用 read_folio 会导致同步通信，
     *      在高并发下可能死锁（write_begin 持有 page 锁，
     *      read_folio 尝试释放/重新获取 page 锁，与并发 read 竞争）
     *   4. write 操作会覆盖整个页面，不需要保留旧数据
     */
    if (!PageUptodate(page)) {
        zero_user(page, 0, PAGE_SIZE);
        SetPageUptodate(page);
    }

    return 0;
}

/*
 * powerfs_write_end - 写结束 (完成写入)
 *
 * 参考 ceph_write_end / generic_write_end
 *
 * 为了确保数据一致性，write_end 直接同步数据到后端。
 * 这确保了 write() 系统调用返回后，数据已经可以被 read() 获取。
 *
 * 同步策略:
 *   - 异步模式: 仅更新 page cache，由 writepage 后台异步通知代理
 *
 * 注意: 之前使用同步通信 (powerfs_comm_send_request) 等待代理响应，
 *       但在高并发下会导致 SQ 队列满 + RCU stall in __d_lookup。
 *       现在改为纯异步：write_end 仅更新 page cache，
 *       数据持久化由 writepage 异步通知代理完成。
 */
int powerfs_write_end(struct file *file, struct address_space *mapping,
                       loff_t pos, unsigned int len, unsigned int copied,
                       struct page *page, void *fsdata)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t end_pos = pos + copied;
    loff_t new_size;

    pr_debug("powerfs: write_end ino=%lu pos=%lld copied=%u (async)\n",
             inode->i_ino, pos, copied);

    if (copied > 0) {
        /* 更新文件大小 */
        new_size = i_size_read(inode);
        if (end_pos > new_size) {
            new_size = end_pos;
            i_size_write(inode, new_size);
            mark_inode_dirty(inode);
        }

        /*
         * 异步模式: 仅标记页面为脏，由 writepage 后台异步通知代理
         * 这样 write 系统调用立即返回，不会因代理处理慢而阻塞
         *
         * 注意: folio_mark_dirty 内部通过 mapping->a_ops->dirty_folio()
         * 间接调用 (mm/page-writeback.c). 若 a_ops 未设置 .dirty_folio
         * 则 NULL 间接调用 → powerfs_write_end+0x45 NULL instruction
         * fetch oops. 根因是 powerfs_aops 接口不完整, 已在 aops 表中
         * 补 .dirty_folio = filemap_dirty_folio 修复 (参考 nfs/btrfs).
         */
        if (!PageUptodate(page))
            SetPageUptodate(page);
        folio_mark_dirty(page_folio(page));

        /*
         * 同步数据 + i_size 到 Filer:
         *
         * 基本功能阶段采用同步写: write_end 直接调用 powerfs_net_write
         * 将本页数据持久化到 Filer 的 inline data store, 保证 write()
         * 返回后数据可被跨 mount session 读取.
         *
         * 不依赖 writepage 异步刷盘的原因:
         *   - writepage 由 VM writeback 触发, 时机不可控 (内存充裕时
         *     可能永远不触发), umount/sync 路径在本地缓存模式下也可能
         *     跳过网络写 → remount 后数据丢失.
         *   - 高并发同步写的 SQ 队列满问题来自旧 comm 层
         *     (powerfs_comm_send_request), 新 net 层 (powerfs_net_write)
         *     使用独立 TCP 连接 + send_recv_mutex 串行化, 不涉及 SQ.
         *
         * 后续优化可改回异步 (writepage 刷盘 + fsync 同步), 但需确保
         * umount 路径触发并等待 writeback 完成.
         */
        if (powerfs_net_is_connected()) {
            __u32 written = 0;
            /* pos % PAGE_SIZE gives the offset within the page where the
             * new data was written. kaddr points to the page start, so we
             * must add the in-page offset to send the actual written bytes
             * (not bytes from the beginning of the page, which would be
             * stale data from prior writes in the same page). This was the
             * root cause of append-write corruption: all appends sent the
             * first chunk's bytes because kaddr[0..copied) was used
             * regardless of the write position within the page. */
            size_t off_in_page = pos & (PAGE_SIZE - 1);
            char *kaddr = kmap_local_page(page);
            int wret = powerfs_net_write(inode->i_ino, pos,
                                         kaddr + off_in_page, copied, &written);
            kunmap_local(kaddr);
            if (wret < 0) {
                pr_warn("powerfs: write_end net_write ino=%lu pos=%lld len=%u failed: %d\n",
                        inode->i_ino, pos, copied, wret);
            }
        }

        /*
         * 同步 i_size 到 Filer:
         *   writepage 只刷数据页, sync 命令只触发 writeback 不触发 fsync,
         *   若不在 write_end 同步 size, Filer 端 i_size 永远为 0, remount
         *   后 lookup 返回 size=0, read_folio zero fill → 数据丢失.
         *
         *   用 pi->content_size 跟踪上次同步值, 仅在变化时 setattr,
         *   避免每次 write_end 都网络往返. write_end 在用户进程上下文
         *   (持有 page lock 但可睡眠, GFP_KERNEL), 调用 net_setattr 安全.
         *   不用 write_inode: writeback 上下文调用网络 IO 会与 writepage
         *   串行化卡死 (send_recv_mutex).
         */
        if (powerfs_net_is_connected() && (u64)new_size != pi->content_size) {
            int sret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                                            0, 0, 0, (u64)new_size);
            if (sret < 0)
                pr_warn("powerfs: write_end setattr ino=%lu size=%llu failed: %d\n",
                        inode->i_ino, (u64)new_size, sret);
            else
                pi->content_size = (u64)new_size;
        }
    }

    unlock_page(page);
    put_page(page);

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
    .read_folio    = powerfs_read_folio,
    .writepage     = powerfs_writepage,
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

    pr_debug("powerfs: fsync ino=%lu datasync=%d\n", inode->i_ino, datasync);

    /* 触发脏页写回 (writepage→powerfs_net_write) */
    ret = file_write_and_wait_range(file, start, end);
    if (ret < 0) {
        pr_warn("powerfs: fsync write_and_wait error: %d\n", ret);
        return ret;
    }

    /* 同步 i_size 到 Filer */
    i_size = i_size_read(inode);
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

    pr_info("powerfs: slab caches created\n");
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
    pr_info("powerfs: slab caches destroyed\n");
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
    };
    struct powerfs_ctx_simple *ctx = fc->s_fs_info;
    struct powerfs_sb_info *sbi;
    struct inode *root;

    pr_info("powerfs: fill_super\n");

    /* 创建超级块私有信息 */
    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;

    sbi->sb = sb;

    /* 保存挂载参数 */
    if (ctx) {
        strncpy(sbi->master_addr, ctx->master_addr, sizeof(sbi->master_addr) - 1);
        sbi->master_port = ctx->master_port;
        strncpy(sbi->volume_addr, ctx->volume_addr, sizeof(sbi->volume_addr) - 1);
        sbi->volume_port = ctx->volume_port;
        strncpy(sbi->filer_addr, ctx->filer_addr, sizeof(sbi->filer_addr) - 1);
        sbi->filer_port = ctx->filer_port;
    }

    /* 初始化 inode 号分配器 (从 100 开始，1 是 root) */
    atomic_set(&sbi->next_ino, 100);

    /* 设置超级块 */
    sb->s_fs_info = sbi;
    sb->s_op = &powerfs_super_ops;
    sb->s_magic = POWERFS_SUPER_MAGIC;
    sb->s_blocksize = 4096;
    sb->s_blocksize_bits = 12;
    sb->s_maxbytes = MAX_LFS_FILESIZE;
    sb->s_time_gran = 1;

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

    /* === 初始化 powerfs_net 连接池 (多节点 Delta Sync) === */
    powerfs_net_pool_init();

    /* 配置 Filer 节点 (Delta Sync 的主节点).
     * mount 无参数时 ctx->filer_addr 为空, 此时 fallback 到模块参数
     * g_server_addr/g_server_port (由 insmod 传入), 保证 pool 非空,
     * 否则 leader_check_work/monitor_work 会一直报 "no leader found". */
    {
        const char *faddr = (ctx && ctx->filer_addr[0]) ?
                            ctx->filer_addr : powerfs_net_get_server_addr();
        __u16 fport = (ctx && ctx->filer_addr[0]) ?
                      ctx->filer_port : powerfs_net_get_server_port();
        if (faddr && faddr[0]) {
            powerfs_net_add_server(faddr, fport,
                                  POWERFS_NET_SERVER_FILER);
            /* 标记为 leader (单 Filer 模式) */
            powerfs_net_set_primary(faddr, fport);
        }
    }

    /* 配置 Master 节点 */
    if (ctx && ctx->master_addr[0]) {
        powerfs_net_add_server(ctx->master_addr, ctx->master_port,
                              POWERFS_NET_SERVER_MASTER);
    }

    /* 配置 Volume 节点 */
    if (ctx && ctx->volume_addr[0]) {
        powerfs_net_add_server(ctx->volume_addr, ctx->volume_port,
                              POWERFS_NET_SERVER_VOLUME);
    }

    /* 启动 Delta Sync 监控 (leader 探测 + 健康检查) */
    powerfs_net_start_monitor();

    pr_info("powerfs: fill_super done, root ino=%lu\n", root->i_ino);
    pr_info("powerfs: powerfs_net pool initialized (Delta Sync ready)\n");
    return 0;
}

/* ========== kill_sb_super: 卸载清理 ========== */

void powerfs_kill_sb_super(struct super_block *sb)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    pr_info("powerfs: kill_sb_super\n");

    /* 清除全局超级块指针 */
    if (g_powerfs_sb == sb)
        g_powerfs_sb = NULL;

    /* 1. 先 sync 脏 inode (网络仍可用, write_inode 能同步 size 到 Filer).
     *    如果先关闭网络, write_inode 的 setattr 会失败, inode 保持 dirty,
     *    evict_inodes 无法驱逐, 导致 umount 挂起或内存泄漏. */
    sync_filesystem(sb);

    /* 2. 设置 stopping 标志: 让 send_request 立即返回 -ENOTCONN,
     *    阻止 reconnect_work 在 g_pool 清零后访问野指针. */
    powerfs_net_set_stopping();

    /* 3. 停止监控并关闭所有连接 (g_pool 会被清零) */
    powerfs_net_stop_monitor();
    powerfs_net_pool_cleanup();

    if (sbi) {
        kfree(sbi);
        sb->s_fs_info = NULL;
    }

    /* 4. kill_anon_super 会再次 sync (但已无脏数据) + shrink_dcache + evict_inodes */
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

    pr_info("powerfs: root inode created, ino=%lu\n", root->i_ino);
    return root;
}
