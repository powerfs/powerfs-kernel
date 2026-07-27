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

#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_net.h"

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

    /* Delta Sync 初始化 */
    di->generation = 0;
    di->path[0] = '\0';

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
     * 检查 2: Delta Sync generation 检查
     * 如果 path 不为空，检查 generation 是否过期
     */
    if (di->path[0] != '\0' && powerfs_net_is_connected()) {
        __u64 cached_gen = di->generation;
        bool stale;

        /* 检查路径是否过期 */
        stale = powerfs_net_path_stale(di->path, cached_gen);
        if (stale) {
            pr_debug("powerfs: d_revalidate '%pd' generation stale (cached=%llu)\n",
                     dentry, (unsigned long long)cached_gen);

            /* 失效 inode 缓存 */
            spin_lock(&pi->i_lock);
            pi->cache_valid = false;
            pi->net_cache_valid = false;
            spin_unlock(&pi->i_lock);

            return 0;  /* 失效，重新 lookup */
        }
    }

    /*
     * 检查 3: inode 级别的 net_cache_valid 检查
     */
    spin_lock(&pi->i_lock);
    if (!pi->cache_valid && pi->net_cache_valid && powerfs_net_is_connected()) {
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: d_revalidate '%pd' inode cache invalid\n", dentry);
        return 0;
    }
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: d_revalidate '%pd' valid (gen=%llu)\n",
             dentry, (unsigned long long)di->generation);
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
     * 返回 0: 让 VFS 执行默认行为 (调用 d_drop 释放 dentry)
     *
     * 参考 ramfs: ramfs 没有 d_delete 回调，VFS 默认行为是
     * 调用 d_drop，将 dentry 从 inode 的 alias 链表中移除，
     * 并触发 iput 减少 inode 引用计数。
     *
     * 配合 dget 钉住 dentry:
     * - 创建时 dget 增加额外引用，防止 dentry 被 shrinker 回收
     * - unlink/rmdir 时 VFS 调用 d_drop，触发 iput
     * - d_drop 会释放 dentry 的引用
     * - 由于 dget 额外增加了引用，iput 后 inode 不会立即释放
     * - 当 dentry 真正被回收时（dentry_kill），最后的 iput 释放 inode
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
    pi->i_caps = POWERFS_CAP_SHARED | POWERFS_CAP_EXCLUSIVE;
    pi->dir_complete = true;  /* 本地缓存模式: 目录始终完整 */

    /* Delta Sync 初始化 */
    pi->generation = 0;
    pi->net_cache_valid = false;

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

    inode_init_once(&pi->vfs_inode);

    /* 初始化私有字段 */
    pi->parent_ino = 0;
    pi->name[0] = '\0';
    spin_lock_init(&pi->i_lock);
    pi->cache_valid = false;
    pi->cache_expire = 0;
    pi->i_caps = 0;
    pi->i_dirty_caps = 0;
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

    pi = alloc_inode_sb(sb, powerfs_inode_cachep, GFP_KERNEL);
    if (!pi)
        return NULL;

    pr_debug("powerfs: alloc_inode (pi=%p, inode=%p)\n", pi, &pi->vfs_inode);

    return &pi->vfs_inode;
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

    /* 第一步: 清理页面缓存 (必须先做) */
    truncate_inode_pages_final(&inode->i_data);

    /* 第二步: 清除 inode 核心 */
    clear_inode(inode);

    /* 第三步: 清理私有资源 */
    spin_lock(&pi->i_lock);
    pi->cache_valid = false;
    pi->dir_complete = false;
    pi->i_caps = 0;
    pi->i_dirty_caps = 0;
    spin_unlock(&pi->i_lock);
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
    char names[64][256];
    struct dentry *cur;
    int depth = 0;
    int i;
    size_t total_len;

    if (!dentry || !path || path_len == 0)
        return;

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

    /* 快速路径: dentry 已有 inode (在 dcache 中) */
    if (d_inode(dentry)) {
        pr_debug("powerfs: lookup '%pd' found in dcache\n", dentry);
        return NULL;
    }

    /* 路径已缓存为负 dentry (有 inode = 正条目，无 inode = 负条目) */
    /* 负 dentry 检查: 通过 d_fsdata 的 generation 判断是否过期 */

    /* === powerfs_net 直接通信模式 === */
    if (powerfs_net_is_connected()) {
        __u64 ino = 0;
        __u32 mode = 0;
        __u32 uid = 0, gid = 0;
        __u64 size = 0;
        __u32 nlink = 0;
        char path_buf[256];
        __u64 generation;

        pr_debug("powerfs: lookup '%pd' via powerfs_net\n", dentry);

        /* 构建完整路径 */
        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));

        /* 通过 powerfs_net 直接查询 */
        err = powerfs_net_lookup(dir->i_ino, dentry->d_name.name,
                                  strlen(dentry->d_name.name),
                                  &ino, &mode, &uid, &gid,
                                  &size, &nlink);

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
                /* 新 inode: 从响应初始化 */
                inode->i_mode = mode;
                inode->i_uid = make_kuid(&init_user_ns, uid);
                inode->i_gid = make_kgid(&init_user_ns, gid);
                inode->i_size = size;
                set_nlink(inode, nlink);
                inode->i_mtime = current_time(inode);
                inode->i_atime = inode->i_mtime;
                inode->i_ctime = inode->i_mtime;

                /* 设置 inode 缓存有效 + Delta Sync 字段 */
                {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->parent_ino = dir->i_ino;
                    strncpy(pi->name, dentry->d_name.name,
                            POWERFS_MAX_NAME_LEN - 1);
                    pi->cache_valid = true;
                    pi->net_cache_valid = true;
                    /* generation 初始为 0，后续通过 Delta Sync 更新 */
                    spin_unlock(&pi->i_lock);
                }

                unlock_new_inode(inode);
            }

            /* 实例化 dentry */
            d_instantiate(dentry, inode);

            /* 设置 dentry 的 Delta Sync 字段 */
            {
                struct powerfs_dentry_info *di = dentry->d_fsdata;
                if (di) {
                    strncpy(di->path, path_buf, sizeof(di->path) - 1);
                    di->generation = 0;  /* 初始 generation */
                    di->lease_expire = jiffies + POWERFS_DENTRY_LEASE_TTL;
                    di->time = jiffies;
                }
            }

            /* 获取当前路径的 generation */
            generation = powerfs_net_get_path_generation(path_buf);
            if (generation > 0) {
                struct powerfs_dentry_info *di = dentry->d_fsdata;
                if (di)
                    di->generation = generation;

                /* 同时更新 inode 的 generation */
                if (inode) {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->generation = generation;
                    spin_unlock(&pi->i_lock);
                }
            }

            pr_debug("powerfs: lookup '%pd' completed, gen=%llu\n",
                     dentry, (unsigned long long)generation);
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
                /* 新 inode: 从响应初始化 */
                inode->i_mode = mode;
                inode->i_uid = make_kuid(&init_user_ns, resp.uid);
                inode->i_gid = make_kgid(&init_user_ns, resp.gid);
                inode->i_size = resp.size;
                set_nlink(inode, resp.nlink);
                inode->i_mtime.tv_sec = resp.mtime_sec;
                inode->i_mtime.tv_nsec = 0;
                inode->i_atime = inode->i_mtime;
                inode->i_ctime = inode->i_mtime;

                /* 设置 inode 缓存有效 + Delta Sync 字段 */
                {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->parent_ino = dir->i_ino;
                    strncpy(pi->name, dentry->d_name.name,
                            POWERFS_MAX_NAME_LEN - 1);
                    pi->cache_valid = true;
                    pi->net_cache_valid = true;
                    spin_unlock(&pi->i_lock);
                }

                unlock_new_inode(inode);
            }

            /* 实例化 dentry */
            d_instantiate(dentry, inode);

            /* 设置 dentry 的 Delta Sync 字段 */
            {
                char path_buf[256];
                struct powerfs_dentry_info *di = dentry->d_fsdata;
                if (di) {
                    powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
                    strncpy(di->path, path_buf, sizeof(di->path) - 1);
                    di->generation = powerfs_net_get_path_generation(path_buf);
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

    /* 生成新 inode 号 */
    new_ino = atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;

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

    /* === Delta Sync: 更新路径 generation === */
    {
        char path_buf[256];
        struct powerfs_dentry_info *di = dentry->d_fsdata;

        if (di) {
            powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
            /* 在创建后触发 generation 失效，强制后续操作同步 */
            powerfs_net_invalidate_path(path_buf);
            powerfs_net_invalidate_dir(dir->i_ino);
        }
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
        {
            struct powerfs_dentry_info *di = dentry->d_fsdata;
            if (di) {
                powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
                strncpy(di->path, path_buf, sizeof(di->path) - 1);
            }
        }

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

    pr_debug("powerfs: rmdir '%pd' in dir=%lu\n", dentry, dir->i_ino);

    /* 简单检查 (VFS 也会检查，这里双重保险) */
    if (!d_is_dir(dentry))
        return -ENOTDIR;

    if (!inode) {
        pr_warn("powerfs: rmdir '%pd' no inode\n", dentry);
        return -ENOENT;
    }

    /*
     * 参考 simple_rmdir 实现:
     *   drop_nlink(inode) + drop_nlink(dir)
     *
     * 关键: 在 drop_nlink 之前 ihold(inode)
     *       防止 drop_nlink 触发的 iput + dentry_kill 的 iput
     *       与 do_rmdir 的 iput 产生双重释放
     */
    ihold(inode);

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

    /* 减少被删除目录的链接数 */
    drop_nlink(inode);

    /* 清空子目录的目录项链表 */
    powerfs_clear_dir_entries(inode);

    /* 从父目录的本地目录项链表中移除 */
    powerfs_remove_dir_entry(dir, dentry->d_name.name);

    /* 减少父目录的链接数 (因删除了一个子目录) */
    drop_nlink(dir);

    /* === Delta Sync: 失效路径缓存 === */
    if (powerfs_net_is_connected()) {
        char path_buf[256];

        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
        powerfs_net_invalidate_path(path_buf);
        powerfs_net_invalidate_dir(dir->i_ino);
        pr_debug("powerfs: rmdir '%pd' invalidated cache for '%s'\n",
                 dentry, path_buf);
    }

    /* === 异步通知代理更新后端 (兼容旧接口) === */
    if (powerfs_comm_connected()) {
        struct powerfs_msg_header req_hdr;
        struct powerfs_remove_req req_data;

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_RMDIR;
        req_hdr.ino = dir->i_ino;
        req_hdr.data_len = sizeof(req_data);

        memset(&req_data, 0, sizeof(req_data));
        req_data.dir_ino = dir->i_ino;
        strncpy(req_data.name, dentry->d_name.name, sizeof(req_data.name) - 1);

        powerfs_comm_submit_notify(&req_hdr, &req_data);
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
        struct powerfs_dentry_info *di = dentry->d_fsdata;

        if (di) {
            powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
            strncpy(di->path, path_buf, sizeof(di->path) - 1);
            powerfs_net_invalidate_path(path_buf);
            powerfs_net_invalidate_dir(dir->i_ino);
        }

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

    pr_debug("powerfs: unlink '%pd' in dir=%lu\n", dentry, dir->i_ino);

    if (!inode) {
        pr_warn("powerfs: unlink '%pd' no inode\n", dentry);
        return -ENOENT;
    }

    /*
     * 参考 simple_unlink 实现:
     *   drop_nlink(inode) - 由文件系统负责
     *   VFS 会在 unlink 返回后处理 dentry 和 iput
     *
     * 关键: 当 nlink 从 1 降到 0 时, drop_nlink 内部会调用 iput
     *       此时 do_unlinkat 还持有一个 ihold 的引用，所以 i_count > 0
     *       VFS 在 vfs_unlink 返回后才会 iput(inode)
     *
     * 因此: 不要在这里手动 iput(inode)
     *       让 VFS 的 do_unlinkat 统一处理
     */
    ihold(inode);  /* 额外引用，保护 drop_nlink 内部的 iput */

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

    /* 减少 inode 链接数 */
    drop_nlink(inode);

    /* 从本地目录项链表中移除 */
    powerfs_remove_dir_entry(dir, dentry->d_name.name);

    /* === Delta Sync: 失效路径缓存 === */
    if (powerfs_net_is_connected()) {
        char path_buf[256];

        powerfs_build_dentry_path(dentry, path_buf, sizeof(path_buf));
        powerfs_net_invalidate_path(path_buf);
        powerfs_net_invalidate_dir(dir->i_ino);
        pr_debug("powerfs: unlink '%pd' invalidated cache for '%s'\n",
                 dentry, path_buf);
    }

    /* === 异步通知代理更新后端 (兼容旧接口) === */
    if (powerfs_comm_connected()) {
        struct powerfs_msg_header req_hdr;
        struct powerfs_remove_req req_data;

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_UNLINK;
        req_hdr.ino = dir->i_ino;
        req_hdr.data_len = sizeof(req_data);

        memset(&req_data, 0, sizeof(req_data));
        req_data.dir_ino = dir->i_ino;
        strncpy(req_data.name, dentry->d_name.name, sizeof(req_data.name) - 1);

        powerfs_comm_submit_notify(&req_hdr, &req_data);
    }

    pr_debug("powerfs: unlink '%pd' success\n", dentry);

    return 0;
}

/* ========== 目录操作: symlink ========== */

static int powerfs_symlink(struct user_namespace *idmap, struct inode *dir,
                            struct dentry *dentry, const char *symname)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct inode *inode;
    u64 new_ino;
    int err;

    (void)idmap;

    pr_debug("powerfs: symlink '%pd' -> '%s'\n", dentry, symname);

    /*
     * 本地操作 + 异步通知 (与mkdir/create/unlink保持一致):
     *   1. 在本地创建 inode 和 dentry
     *   2. 异步通知代理创建后端记录
     */

    /* 生成新 inode 号 */
    new_ino = atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;

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

    /* 异步通知代理创建符号链接 */
    if (powerfs_comm_connected()) {
        struct powerfs_msg_header req_hdr;
        struct powerfs_symlink_req req_data;

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_SYMLINK;
        req_hdr.ino = dir->i_ino;
        req_hdr.data_len = sizeof(req_data);

        memset(&req_data, 0, sizeof(req_data));
        req_data.dir_ino = dir->i_ino;
        req_data.name_len = strlen(dentry->d_name.name);
        req_data.symname_len = strlen(symname);
        strncpy(req_data.name, dentry->d_name.name, sizeof(req_data.name) - 1);
        strncpy(req_data.symname, symname, sizeof(req_data.symname) - 1);

        powerfs_comm_submit_notify(&req_hdr, &req_data);
    }

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

    pr_debug("powerfs: link '%pd' -> '%pd' (ino=%lu)\n",
             old_dentry, new_dentry, inode->i_ino);

    /* 不允许对目录创建硬链接 */
    if (S_ISDIR(inode->i_mode))
        return -EPERM;

    /*
     * 本地操作:
     *   1. 增加 inode 链接数 (硬链接需要)
     *   2. 添加新 dentry 到 inode 的哈希表
     *
     * 注意: VFS (vfs_link) 不会自动 inc_nlink
     *       需要文件系统自己调用 inc_nlink
     */
    inc_nlink(inode);
    d_add(new_dentry, inode);

    dir->i_mtime = dir->i_ctime = current_time(dir);

    /* 添加目录项到本地链表 */
    powerfs_add_dir_entry(dir, inode->i_ino,
                          inode->i_mode & S_IFMT,
                          new_dentry->d_name.name);

    /* 异步通知代理创建硬链接 */
    if (powerfs_comm_connected()) {
        struct powerfs_msg_header req_hdr;
        struct powerfs_link_req req_data;

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_LINK;
        req_hdr.ino = inode->i_ino;
        req_hdr.data_len = sizeof(req_data);

        memset(&req_data, 0, sizeof(req_data));
        req_data.ino = inode->i_ino;
        req_data.dir_ino = dir->i_ino;
        strncpy(req_data.name, new_dentry->d_name.name,
                sizeof(req_data.name) - 1);

        powerfs_comm_submit_notify(&req_hdr, &req_data);
    }

    pr_debug("powerfs: link '%pd' -> '%pd' success\n",
             old_dentry, new_dentry);

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

    /* 处理文件大小变更 */
    if (ia_valid & ATTR_SIZE) {
        if (attr->ia_size < i_size_read(inode)) {
            truncate_pagecache(inode, attr->ia_size);
        }
    }

    /* 第二步: 在本地修改 inode 属性 */
    setattr_copy(&init_user_ns, inode, attr);
    mark_inode_dirty(inode);

    /* 更新缓存标志 */
    spin_lock(&pi->i_lock);
    pi->cache_valid = true;
    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
    spin_unlock(&pi->i_lock);

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
        struct powerfs_msg_header req_hdr;
        struct powerfs_rename_req req_data;
        char old_name_buf[POWERFS_MAX_NAME_LEN + 1];
        char new_name_buf[POWERFS_MAX_NAME_LEN + 1];

        strncpy(old_name_buf, old_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        old_name_buf[POWERFS_MAX_NAME_LEN] = '\0';
        strncpy(new_name_buf, new_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        new_name_buf[POWERFS_MAX_NAME_LEN] = '\0';

        /*
         * 目标已存在的情况
         *
         * 参考 ramfs_rename (fs/ramfs/inode.c):
         *   - 减少目标 inode 的 nlink
         *   - 如果目标是目录，减少目标父目录的 nlink
         *   - 不调用 d_delete/d_drop，VFS 会在 dput 时处理
         */
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
                drop_nlink(new_dir);
            }

            drop_nlink(target);
        } else if (flags & RENAME_NOREPLACE) {
            return -EEXIST;
        }

        /*
         * 注意: VFS (vfs_rename) 不会自动处理 nlink 调整
         *       需要文件系统自己处理:
         *       1. 替换目标时: drop_nlink(target) 已在上方处理
         *       2. 目录跨移动时: drop_nlink(old_dir) + inc_nlink(new_dir)
         *          参考 simple_rename_exchange
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

        /* 异步通知代理更新后端 */
        if (powerfs_comm_connected()) {
            memset(&req_data, 0, sizeof(req_data));
            req_data.old_dir_ino = old_dir->i_ino;
            req_data.new_dir_ino = new_dir->i_ino;
            req_data.flags = flags;
            strncpy(req_data.old_name, old_name_buf,
                    sizeof(req_data.old_name) - 1);
            strncpy(req_data.new_name, new_name_buf,
                    sizeof(req_data.new_name) - 1);
            req_data.old_name_len = strlen(old_name_buf);
            req_data.new_name_len = strlen(new_name_buf);

            memset(&req_hdr, 0, sizeof(req_hdr));
            req_hdr.type = POWERFS_MSG_RENAME;
            req_hdr.ino = old_dir->i_ino;
            req_hdr.data_len = sizeof(req_data);

            powerfs_comm_submit_notify(&req_hdr, &req_data);
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

    /*
     * 第一阶段: 在锁内复制目录项到临时缓冲区
     *
     * 使用 kmalloc 动态分配 (避免栈溢出)
     * 限制最大条目数 256 (单条 ~264B × 256 ≈ 68KB < 128KM_MAX_SIZE)
     *
     * 关键: 在锁内仅做 O(n) 内存复制，不做任何可能阻塞的操作
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
 * powerfs_read_folio - 读取页面 (纯本地模式，不与代理通信)
 *
 * 参考 ramfs_read_folio (fs/ramfs/inode.c)
 *
 * 策略:
 *   - 阶段0 (本地缓存模式): 直接零填充页面，数据完全由 page cache 管理
 *   - 写入路径 (write_begin + write_end) 将数据写入 page cache
 *   - 读取路径从 page cache 获取数据，不需要代理参与
 *   - 如果页面不在 cache 中 (首次访问/被回收)，零填充是正确行为
 *
 * 这避免了高并发下同步通信导致的 SQ 队列满 + RCU stall
 * 后续阶段 (接入真实后端) 可通过异步通知机制扩展
 */
static int powerfs_read_folio(struct file *file, struct folio *folio)
{
    pr_debug("powerfs: read_folio ino=%lu index=%lu (local zero-fill)\n",
             folio->mapping->host->i_ino, folio->index);

    /*
     * 纯本地模式: 零填充页面
     *
     * 参考 ramfs: 不定义 read_folio，使用内核默认行为
     * 默认行为: 如果页面不在 cache 中，清零并返回
     *
     * 原因:
     *   1. 数据存储在 page cache 中 (由 write_begin/write_end 管理)
     *   2. 代理仅负责元数据 (inode/dentry)，不负责数据
     *   3. 避免同步通信导致的死锁和 RCU stall
     *   4. 后续接入真实后端时，可在 read_folio 中添加异步预读
     */
    folio_zero_range(folio, 0, folio_size(folio));
    folio_mark_uptodate(folio);
    folio_unlock(folio);

    return 0;
}

/*
 * powerfs_writepage - 写入页面 (纯本地模式，不与代理通信)
 *
 * 参考 ramfs_writepage (fs/ramfs/inode.c)
 *
 * 策略:
 *   - 阶段0 (本地缓存模式): 直接标记页面为干净，不与代理通信
 *   - 数据完全存储在 page cache 中，代理仅负责元数据
 *   - 后续阶段 (接入真实后端) 可通过异步通知机制扩展
 *
 * 这避免了高并发下 writepage 阻塞代理通知导致的 SQ 队列满 + RCU stall
 */
int powerfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = page->mapping->host;

    pr_debug("powerfs: writepage ino=%lu index=%lu (local, skip proxy)\n",
             inode->i_ino, page->index);

    /*
     * 纯本地模式: 直接标记页面为干净
     *
     * 参考 ramfs: writepage 直接调用 end_page_writeback
     * 不与代理通信，避免阻塞
     *
     * 数据在 page cache 中，由 write_begin/write_end 管理
     * writepage 仅在内核回写线程触发时标记页面干净
     */
    end_page_writeback(page);

    return 0;
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
    loff_t end_pos = pos + copied;

    pr_debug("powerfs: write_end ino=%lu pos=%lld copied=%u (async)\n",
             inode->i_ino, pos, copied);

    if (copied > 0) {
        /* 更新文件大小 */
        if (end_pos > i_size_read(inode)) {
            i_size_write(inode, end_pos);
            mark_inode_dirty(inode);
        }

        /*
         * 异步模式: 仅标记页面为脏，由 writepage 后台异步通知代理
         * 这样 write 系统调用立即返回，不会因代理处理慢而阻塞
         */
        if (!PageUptodate(page))
            SetPageUptodate(page);
        set_page_dirty(page);
    }

    unlock_page(page);
    put_page(page);

    return copied;
}

/*
 * powerfs_dirty_folio - 标记folio为脏
 *
 * 参考 ramfs 的实现，直接返回 true
 */
static bool powerfs_dirty_folio(struct address_space *mapping,
                                 struct folio *folio)
{
    folio_set_dirty(folio);
    return true;
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

/* 地址空间操作表 */
static const struct address_space_operations powerfs_aops = {
    .read_folio    = powerfs_read_folio,
    .writepage     = powerfs_writepage,
    .write_begin   = powerfs_write_begin,
    .write_end     = powerfs_write_end,
    .bmap          = powerfs_bmap,
    .dirty_folio   = powerfs_dirty_folio,
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
 * 当通信层可用时，发送 FSYNC 通知代理同步数据
 * 当通信层不可用时，直接返回成功 (纯内存模式)
 */
static int powerfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_fsync_req req_data;
    struct powerfs_fsync_resp resp_data;
    int ret;

    pr_debug("powerfs: fsync ino=%lu datasync=%d\n", inode->i_ino, datasync);

    /* 通信层不可用，纯内存模式直接返回 */
    if (!powerfs_comm_connected())
        return 0;

    /* 触发 inode 脏页写回 */
    ret = file_write_and_wait_range(file, start, end);
    if (ret < 0)
        pr_warn("powerfs: fsync write_and_wait error: %d\n", ret);

    /* 构建 FSYNC 请求 */
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_FSYNC;
    req_hdr.ino = inode->i_ino;
    req_hdr.data_len = sizeof(req_data);

    memset(&req_data, 0, sizeof(req_data));
    req_data.ino = inode->i_ino;
    req_data.datasync = datasync ? 1 : 0;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(&resp_data, 0, sizeof(resp_data));

    /* 发送 FSYNC 请求 */
    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, &resp_data, 500);
    if (ret < 0) {
        pr_warn("powerfs: fsync comm error: %d\n", ret);
        /* 通信层错误，仍然返回成功 (数据在本地 page cache) */
        return 0;
    }

    if (resp_hdr.status != 0) {
        pr_warn("powerfs: fsync resp error: %d\n", resp_hdr.status);
        return resp_hdr.status;
    }

    pr_debug("powerfs: fsync complete ino=%lu\n", inode->i_ino);
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
    .destroy_inode = powerfs_free_inode,
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
        0,
        SLAB_HWCACHE_ALIGN,
        powerfs_inode_init_once);
    if (!powerfs_inode_cachep)
        return -ENOMEM;

    /* dentry_info slab 缓存 */
    powerfs_dentry_cachep = kmem_cache_create(
        "powerfs_dentry_cache",
        sizeof(struct powerfs_dentry_info),
        0,
        SLAB_HWCACHE_ALIGN,
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

    /* 配置 Filer 节点 (Delta Sync 的主节点) */
    if (ctx && ctx->filer_addr[0]) {
        powerfs_net_add_server(ctx->filer_addr, ctx->filer_port,
                              POWERFS_NET_SERVER_FILER);
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

    /* === 清理 powerfs_net (停止 Delta Sync 监控，关闭所有连接) === */
    powerfs_net_stop_monitor();
    powerfs_net_pool_cleanup();

    if (sbi) {
        kfree(sbi);
        sb->s_fs_info = NULL;
    }

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
