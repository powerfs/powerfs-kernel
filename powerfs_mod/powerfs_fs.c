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
 * 策略:
 *   - 通信层可用时: 使用 TTL 机制，过期则重新向代理查询
 *   - 纯内存模式: dentry 始终有效 (因为内存中文件不会被外部修改)
 *   - RCU 模式: 只做快速检查
 */
int powerfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
    struct powerfs_dentry_info *di;
    bool valid;

    /* RCU 模式: 只能做无锁快速检查 */
    if (flags & LOOKUP_RCU) {
        di = rcu_dereference(dentry->d_fsdata);
        if (!di)
            return -ECHILD;

        /* RCU 模式下先做乐观检查 */
        if (time_before(jiffies, di->lease_expire))
            return 1;

        /* TTL 可能过期，退出 RCU 模式进行完整检查 */
        return -ECHILD;
    }

    /* 正常模式 */
    di = dentry->d_fsdata;
    if (!di)
        return 0;

    pr_debug("powerfs: d_revalidate '%pd' (lease_expire=%lu, now=%lu)\n",
             dentry, di->lease_expire, jiffies);

    /*
     * 如果通信层不可用 (纯内存模式)，dentry 始终有效
     * 因为内存中的文件不会被外部修改
     */
    if (!powerfs_comm_connected()) {
        pr_debug("powerfs: d_revalidate '%pd' valid (内存模式，始终有效)\n", dentry);
        return 1;
    }

    /* 通信层可用时，检查 TTL 是否过期 */
    valid = time_before(jiffies, di->lease_expire);

    if (valid) {
        pr_debug("powerfs: d_revalidate '%pd' valid (TTL 未过期)\n", dentry);
        return 1;
    }

    /*
     * TTL 已过期，dentry 失效
     * VFS 会重新调用 lookup 来验证
     *
     * 参考 ceph: 过期的 dentry 返回 0，让 VFS 重新 lookup
     */
    pr_debug("powerfs: d_revalidate '%pd' invalid (TTL 过期，重新 lookup)\n",
             dentry);
    return 0;
}

/*
 * d_delete - d_count 归零时决定是否立即删除
 *
 * 参考 ceph_d_delete (fs/ceph/dir.c)
 *
 * 返回 1: 立即删除 dentry
 * 返回 0: 保留在 dcache LRU 缓存中
 *
 * 策略:
 *   - 通信层可用时: TTL 过期则删除，未过期则保留
 *   - 纯内存模式: 始终保留在 dcache 中 (文件不会被外部删除)
 *
 * 注意: 即使 lease 有效，内存压力大时 shrinker 仍可能回收 dentry
 *       (会触发 d_prune 回调)
 */
int powerfs_d_delete(const struct dentry *dentry)
{
    struct powerfs_dentry_info *di = dentry->d_fsdata;
    bool expired;

    if (!di)
        return 1;  /* 没有私有数据，直接删除 */

    /*
     * 如果通信层不可用 (纯内存模式)，始终保留 dentry
     * 因为内存中的文件不会被外部修改
     */
    if (!powerfs_comm_connected()) {
        pr_debug("powerfs: d_delete '%pd' 保留 (内存模式)\n", dentry);
        return 0;
    }

    /* 通信层可用时，检查 lease 是否过期 */
    expired = time_after_eq(jiffies, di->lease_expire);

    pr_debug("powerfs: d_delete '%pd' (lease_expire=%lu, now=%lu, expired=%d)\n",
             dentry, di->lease_expire, jiffies, expired);

    if (expired) {
        /* lease 已过期，立即删除 dentry */
        pr_debug("powerfs: d_delete '%pd' 删除 (TTL 过期)\n", dentry);
        return 1;
    }

    /* lease 还有效，保留在 dcache LRU 缓存中 */
    pr_debug("powerfs: d_delete '%pd' 保留 (TTL 未过期)\n", dentry);
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

/* Dentry operations 表 */
static const struct dentry_operations powerfs_dentry_operations = {
    .d_revalidate = powerfs_d_revalidate,
    .d_delete     = powerfs_d_delete,
    .d_init       = powerfs_d_init,
    .d_release    = powerfs_d_release,
    .d_prune      = powerfs_d_prune,
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

    pr_debug("powerfs: init_inode ino=%lu mode=%o\n",
             inode->i_ino, mode);

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
    pi->dir_complete = false;

    /* 根据文件类型设置操作表 */
    switch (mode & S_IFMT) {
    case S_IFREG:
        inode->i_op = &powerfs_file_inode_operations;
        inode->i_fop = &powerfs_file_operations;
        set_nlink(inode, 1);
        break;

    case S_IFDIR:
        inode->i_op = &powerfs_dir_inode_operations;
        inode->i_fop = &powerfs_dir_operations;
        set_nlink(inode, 2);  /* "." + ".." */
        pi->dir_complete = true;  /* 新建目录为空，认为 complete */
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

    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, resp, 5000);
    kfree(req_data);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/*
 * powerfs_comm_mkdir - 通过通信层创建目录
 *
 * 返回 0 表示成功，resp 中有新目录信息
 * 返回负值表示错误
 */
static int powerfs_comm_create(struct inode *dir, const char *name,
                                umode_t mode, struct powerfs_create_resp *resp)
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
    req_data->mode = mode;
    req_data->uid = from_kuid(&init_user_ns, current_fsuid());
    req_data->gid = from_kgid(&init_user_ns, current_fsgid());
    strncpy(req_data->name, name, sizeof(req_data->name) - 1);

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(resp, 0, sizeof(*resp));

    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, resp, 5000);
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
                                     &resp_hdr, NULL, 5000);
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
                                     &resp_hdr, NULL, 5000);
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
                                     &resp_hdr, NULL, 5000);
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
                                     &resp_hdr, resp, 5000);
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
                                     &resp_hdr, resp, 5000);
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
                                     &resp_hdr, NULL, 5000);
    if (ret < 0)
        return ret;

    return resp_hdr.status;
}

/* ========== 目录操作: lookup ========== */

/*
 * powerfs_lookup - 查找文件/目录
 *
 * 参考 simple_lookup (fs/libfs.c) + ceph_lookup (fs/ceph/dir.c)
 *
 * 策略:
 *   - 优先通过通信层向服务端查询
 *   - 如果通信层不可用，回退到内存模式
 *   - 返回 NULL 并创建负 dentry 表示不存在
 *   - 返回 ERR_PTR 表示错误
 */
struct dentry *powerfs_lookup(struct inode *dir, struct dentry *dentry,
                               unsigned int flags)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_lookup_resp lookup_resp;
    struct inode *inode;
    int ret;

    (void)flags;

    pr_debug("powerfs: lookup '%pd' in dir=%lu\n",
             dentry, dir->i_ino);

    /*
     * 步骤1: 尝试通过通信层查询服务端
     *
     * 参考 ceph_lookup: 网络文件系统首先向 MDS 发送 LOOKUP 请求
     */
    ret = powerfs_comm_lookup(dir, dentry->d_name.name, &lookup_resp);
    if (ret == 0) {
        /* 找到文件，从 inode 缓存中获取或创建新 inode */
        inode = powerfs_iget(dir->i_sb, lookup_resp.ino);
        if (IS_ERR(inode)) {
            pr_warn("powerfs: lookup '%pd' failed to get inode: %ld\n",
                    dentry, PTR_ERR(inode));
            return ERR_CAST(inode);
        }

        /* 如果是新 inode，初始化它 */
        if (inode->i_state & I_NEW) {
            ret = powerfs_init_inode(inode, lookup_resp.mode,
                                     dir->i_ino, dentry->d_name.name);
            if (ret) {
                iput(inode);
                return ERR_PTR(ret);
            }

            /* 设置属性 */
            i_uid_write(inode, lookup_resp.uid);
            i_gid_write(inode, lookup_resp.gid);
            i_size_write(inode, lookup_resp.size);
            set_nlink(inode, lookup_resp.nlink);

            unlock_new_inode(inode);
        }

        d_add(dentry, inode);
        powerfs_set_dentry_lease(dentry, POWERFS_DENTRY_LEASE_TTL);

        pr_debug("powerfs: lookup '%pd' found, ino=%llu\n",
                 dentry, (unsigned long long)lookup_resp.ino);
        return NULL;
    }

    if (ret != -ENOENT && ret != -ENOTCONN) {
        /* 其他错误，返回错误 */
        pr_warn("powerfs: lookup '%pd' comm error: %d\n", dentry, ret);
        /* 回退到内存模式 */
    }

    /*
     * 步骤2: 回退到内存模式
     *
     * 如果通信层不可用或返回 ENOENT，使用内存模式处理
     */
    if (dpi->dir_complete) {
        pr_debug("powerfs: lookup '%pd' not found (dir complete), adding negative dentry\n", dentry);
        d_add(dentry, NULL);
        return NULL;
    }

    pr_debug("powerfs: lookup '%pd' not found, adding negative dentry\n", dentry);
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

    /*
     * 设置 dentry lease
     *
     * 注意: 纯内存文件系统通常还会 dget(dentry) 锁定在内存中
     *       但我们模拟网络文件系统风格，使用 lease TTL 机制
     *       这样可以测试 d_revalidate/d_delete 路径
     */
    powerfs_set_dentry_lease(dentry, POWERFS_DENTRY_LEASE_TTL);

    /* 清除父目录的 complete 标志 (目录内容已改变) */
    spin_lock(&dpi->i_lock);
    dpi->dir_complete = false;
    spin_unlock(&dpi->i_lock);

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

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
     * 纯本地操作 + 异步通知:
     *   1. 在本地创建 inode 和 dentry
     *   2. 异步通知代理创建后端记录
     */
    err = powerfs_mknod(idmap, dir, dentry, mode | S_IFDIR, 0);
    if (err) {
        pr_warn("powerfs: mkdir '%pd' mknod failed: %d\n", dentry, err);
        return err;
    }

    /* 增加父目录 nlink (".." 指向父目录) */
    inc_nlink(dir);

    /* 获取新创建的 inode 用于通知 */
    inode = d_inode(dentry);

    /* 异步通知代理创建目录 */
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
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
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
     * 参考 ceph_rmdir (fs/ceph/dir.c)
     *
     * 操作顺序:
     *   1. 减少目录 inode 的 nlink
     *   2. 清除父目录 complete 标志
     *   3. 父目录的 nlink 由 VFS 在 rmdir 返回后自动处理
     *      不需要我们手动 drop_nlink(dir)，否则会导致重复减少
     *
     * 注意: 不要在这里调用 truncate_inode_pages_final 或 d_drop，
     *       VFS 会在 rmdir 返回后自动调用 d_delete_notify 来清理
     *       dentry。在这里调用会导致 iput 双重释放 BUG。
     *       页面缓存会在 evict_inode 中清理。
     */

    /* 减少链接数 */
    drop_nlink(inode);

    /* 清除父目录的 complete 标志 */
    spin_lock(&dpi->i_lock);
    dpi->dir_complete = false;
    spin_unlock(&dpi->i_lock);

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

    /* 异步通知代理更新后端 */
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
     * 纯本地操作 + 异步通知:
     *   1. 在本地创建 inode 和 dentry
     *   2. 异步通知代理创建后端记录
     */
    err = powerfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
    if (err) {
        pr_warn("powerfs: create '%pd' mknod failed: %d\n", dentry, err);
        return err;
    }

    /* 获取新创建的 inode 用于通知 */
    inode = d_inode(dentry);

    /* 异步通知代理创建文件 */
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
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct inode *inode = d_inode(dentry);

    pr_debug("powerfs: unlink '%pd' in dir=%lu\n", dentry, dir->i_ino);

    if (!inode) {
        pr_warn("powerfs: unlink '%pd' no inode\n", dentry);
        return -ENOENT;
    }

    /*
     * 参考 ceph_unlink (fs/ceph/dir.c)
     *
     * 操作顺序 (非常重要!):
     *   1. 先减少 inode 链接数 (drop_nlink)
     *   2. 如果 nlink 降为 0 (最后一个硬链接), 清理页面缓存
     *   3. 从 dcache 移除 dentry (d_drop 会触发 iput)
     *   4. 异步通知代理更新后端
     *
     * 注意:
     *   - 不要使用 d_invalidate，它会干扰引用计数管理
     *   - d_drop 会减少 dentry 的引用计数，当引用归零时触发 d_release
     *   - d_release 会调用 iput 减少 inode 的引用计数
     *   - 此时 nlink 已经被 drop_nlink 减少，所以 iput 不会触发 BUG
     *
     * 硬链接场景 (nlink > 1):
     *   - drop_nlink 将 nlink 减 1，但不会触发 inode 释放
     *   - d_drop 减少 dentry 引用，最终触发 iput 减少 refcount
     *   - 由于 nlink > 0，iput 不会触发 BUG
     *
     * 注意: 不要在这里调用 truncate_inode_pages_final 或 d_drop，
     *       VFS 会在 unlink 返回后自动调用 d_delete_notify 来清理
     *       dentry。在这里调用 d_drop 会导致 VFS 的 d_delete 中的
     *       iput 与 do_unlinkat 的 iput 产生双重释放，触发
     *       BUG_ON(inode->i_state & I_CLEAR)。
     *       页面缓存会在 evict_inode 中清理。
     */

    /* 步骤1: 先减少 inode 链接数 */
    drop_nlink(inode);

    /* 步骤2: 清除父目录的 complete 标志 */
    spin_lock(&dpi->i_lock);
    dpi->dir_complete = false;
    spin_unlock(&dpi->i_lock);

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

    /* 步骤3: 异步通知代理更新后端 */
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

    pr_debug("powerfs: symlink '%pd' -> '%s' (comm_connected=%d)\n",
             dentry, symname, powerfs_comm_connected());

    /*
     * 步骤1: 尝试通过通信层创建符号链接
     */
    if (powerfs_comm_connected()) {
        err = powerfs_comm_symlink(dir, dentry->d_name.name, symname);
        if (err == 0) {
            /* 通信层成功，在本地创建符号链接 */
            new_ino = atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;

            inode = powerfs_new_inode(dir->i_sb, S_IFLNK | 0777, new_ino,
                                       dir->i_ino, dentry->d_name.name);
            if (!inode)
                return -ENOSPC;

            err = page_symlink(inode, symname, strlen(symname) + 1);
            if (err) {
                iput(inode);
                return err;
            }

            d_add(dentry, inode);
            powerfs_set_dentry_lease(dentry, POWERFS_DENTRY_LEASE_TTL);

            spin_lock(&dpi->i_lock);
            dpi->dir_complete = false;
            spin_unlock(&dpi->i_lock);

            dir->i_mtime = dir->i_ctime = current_time(dir);

            pr_debug("powerfs: symlink '%pd' success (comm)\n", dentry);
            return 0;
        }

        pr_warn("powerfs: symlink comm error: %d, falling back\n", err);
    }

    /* 步骤2: 回退到内存模式 */
    pr_debug("powerfs: symlink '%pd' -> '%s' (memory mode)\n", dentry, symname);

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

    /* 设置 dentry lease */
    powerfs_set_dentry_lease(dentry, POWERFS_DENTRY_LEASE_TTL);

    /* 清除父目录的 complete 标志 */
    spin_lock(&dpi->i_lock);
    dpi->dir_complete = false;
    spin_unlock(&dpi->i_lock);

    /* 更新父目录时间戳 */
    dir->i_mtime = dir->i_ctime = current_time(dir);

    pr_debug("powerfs: symlink '%pd' success\n", dentry);

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
    struct powerfs_inode_info *pi;
    int ret;

    pr_debug("powerfs: readlink '%pd'\n", dentry);

    if (!inode)
        return -ENOENT;

    if (!S_ISLNK(inode->i_mode))
        return -EINVAL;

    pi = POWERFS_I(inode);

    /*
     * 如果通信层可用且缓存已过期，从服务端获取
     */
    if (powerfs_comm_connected() && !pi->cache_valid) {
        struct powerfs_msg_header req_hdr, resp_hdr;
        struct powerfs_readlink_req *req_data;
        struct powerfs_readlink_resp *resp_data;

        req_data = kmalloc(sizeof(*req_data), GFP_KERNEL);
        resp_data = kmalloc(sizeof(*resp_data), GFP_KERNEL);
        if (!req_data || !resp_data) {
            kfree(req_data);
            kfree(resp_data);
            return -ENOMEM;
        }

        memset(&req_hdr, 0, sizeof(req_hdr));
        req_hdr.type = POWERFS_MSG_READLINK;
        req_hdr.ino = inode->i_ino;
        req_hdr.data_len = sizeof(*req_data);

        memset(req_data, 0, sizeof(*req_data));
        req_data->ino = inode->i_ino;

        memset(&resp_hdr, 0, sizeof(resp_hdr));
        memset(resp_data, 0, sizeof(*resp_data));

        ret = powerfs_comm_send_request(&req_hdr, req_data,
                                         &resp_hdr, resp_data, 5000);
        kfree(req_data);
        if (ret == 0 && resp_hdr.status == 0) {
            u32 len = resp_data->len;

            if (len >= (u32)buflen)
                len = buflen - 1;

            memcpy(buffer, resp_data->target, len);
            buffer[len] = '\0';

            spin_lock(&pi->i_lock);
            pi->cache_valid = true;
            pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
            spin_unlock(&pi->i_lock);

            pr_debug("powerfs: readlink '%pd' from server: '%s'\n",
                     dentry, buffer);
            kfree(resp_data);
            return 0;
        }
        kfree(resp_data);

        if (ret != -ENOTCONN) {
            pr_warn("powerfs: readlink comm error: %d, falling back to cache\n", ret);
        }
    }

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
 * 策略:
 *   - 通信层可用时: 向代理发送 LINK 请求，增加 nlink
 *   - 纯内存模式: 直接在本地增加 nlink
 */
static int powerfs_link(struct dentry *old_dentry, struct inode *dir,
                        struct dentry *new_dentry)
{
    struct inode *inode = d_inode(old_dentry);
    int err;

    pr_debug("powerfs: link '%pd' -> '%pd' (ino=%lu)\n",
             old_dentry, new_dentry, inode->i_ino);

    /* 不允许对目录创建硬链接 */
    if (S_ISDIR(inode->i_mode))
        return -EPERM;

    /*
     * 步骤1: 尝试通过通信层创建硬链接
     */
    if (powerfs_comm_connected()) {
        err = powerfs_comm_link(dir, inode->i_ino, new_dentry->d_name.name);
        if (err == 0) {
            /* 通信层成功，在本地增加 nlink */
            inc_nlink(inode);

            /*
             * 使用 d_add 替代 d_instantiate:
             * - d_add 会将 dentry 添加到 inode 的哈希表中
             * - 它也会调用 ihold 增加 inode 的引用计数
             * - 当 dentry 被释放时，VFS 会自动调用 iput
             */
            d_add(new_dentry, inode);

            powerfs_set_dentry_lease(new_dentry, POWERFS_DENTRY_LEASE_TTL);

            /* 清除父目录的 complete 标志 */
            {
                struct powerfs_inode_info *dpi = POWERFS_I(dir);
                spin_lock(&dpi->i_lock);
                dpi->dir_complete = false;
                spin_unlock(&dpi->i_lock);
            }

            dir->i_mtime = dir->i_ctime = current_time(dir);

            pr_debug("powerfs: link '%pd' -> '%pd' success (comm)\n",
                     old_dentry, new_dentry);
            return 0;
        }

        if (err != -ENOTCONN && err != -ENOENT) {
            pr_warn("powerfs: link comm error: %d, falling back\n", err);
        }
    }

    /* 步骤2: 回退到内存模式 */
    inc_nlink(inode);
    d_add(new_dentry, inode);

    dir->i_mtime = dir->i_ctime = current_time(dir);

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
     * 如果通信层可用，且缓存已过期，从服务端获取最新属性
     *
     * 参考 ceph_getattr: 检查 cap 是否有效，无效则向 MDS 请求
     */
    if (powerfs_comm_connected() && !pi->cache_valid) {
        struct powerfs_getattr_resp getattr_resp;
        int ret;

        ret = powerfs_comm_getattr(inode, &getattr_resp);
        if (ret == 0) {
            /* 从服务端获取成功，更新本地缓存 */
            inode->i_mode = getattr_resp.mode;
            set_nlink(inode, getattr_resp.nlink);
            i_uid_write(inode, getattr_resp.uid);
            i_gid_write(inode, getattr_resp.gid);
            i_size_write(inode, getattr_resp.size);
            inode->i_atime.tv_sec = getattr_resp.atime_sec;
            inode->i_atime.tv_nsec = getattr_resp.atime_nsec;
            inode->i_mtime.tv_sec = getattr_resp.mtime_sec;
            inode->i_mtime.tv_nsec = getattr_resp.mtime_nsec;
            inode->i_ctime.tv_sec = getattr_resp.ctime_sec;
            inode->i_ctime.tv_nsec = getattr_resp.ctime_nsec;

            /* 更新缓存有效期 */
            spin_lock(&pi->i_lock);
            pi->cache_valid = true;
            pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
            spin_unlock(&pi->i_lock);

            pr_debug("powerfs: getattr '%pd' updated from server\n", path->dentry);
        } else if (ret != -ENOTCONN && ret != -ENOENT) {
            pr_warn("powerfs: getattr '%pd' comm error: %d, using cache\n",
                    path->dentry, ret);
        }
    }

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
     * 第一步: 执行 VFS 通用 setattr 处理
     * 这会处理权限检查、配额检查等
     *
     * 注意: setattr_prepare 会根据 attr 修改 inode
     *       但我们需要先向服务端请求，成功后再修改
     *       所以这里我们先做检查，真正的修改在后面
     */
    ia_valid = attr->ia_valid;

    /*
     * 第二步: 如果通信层可用，先向服务端发送 SETATTR 请求
     *
     * 参考 ceph_setattr:
     *   - 如果有独占 cap，可以直接修改本地缓存
     *   - 否则需要向 MDS 发送请求
     *
     * 我们简化处理：始终先尝试通信层
     */
    if (powerfs_comm_connected()) {
        struct powerfs_setattr_resp setattr_resp;

        err = powerfs_comm_setattr(inode, attr, &setattr_resp);
        if (err == 0) {
            /* 服务端修改成功，更新本地缓存 */
            if (ia_valid & ATTR_MODE)
                inode->i_mode = setattr_resp.mode;
            if (ia_valid & ATTR_UID)
                i_uid_write(inode, setattr_resp.uid);
            if (ia_valid & ATTR_GID)
                i_gid_write(inode, setattr_resp.gid);
            if (ia_valid & ATTR_SIZE)
                i_size_write(inode, setattr_resp.size);
            if (ia_valid & ATTR_ATIME) {
                inode->i_atime.tv_sec = setattr_resp.atime_sec;
                inode->i_atime.tv_nsec = setattr_resp.atime_nsec;
            }
            if (ia_valid & ATTR_MTIME) {
                inode->i_mtime.tv_sec = setattr_resp.mtime_sec;
                inode->i_mtime.tv_nsec = setattr_resp.mtime_nsec;
            }
            if (ia_valid & ATTR_CTIME) {
                inode->i_ctime.tv_sec = setattr_resp.ctime_sec;
                inode->i_ctime.tv_nsec = setattr_resp.ctime_nsec;
            }
            if (ia_valid & ATTR_SIZE) {
                /* 文件大小变更时，处理页面缓存 */
                if (attr->ia_size < i_size_read(inode)) {
                    truncate_pagecache(inode, attr->ia_size);
                }
            }

            /* 更新缓存有效期 */
            spin_lock(&pi->i_lock);
            pi->cache_valid = true;
            pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
            spin_unlock(&pi->i_lock);

            mark_inode_dirty(inode);

            pr_debug("powerfs: setattr '%pd' success (comm)\n", dentry);
            return 0;
        }

        if (err != -ENOTCONN && err != -ENOENT) {
            pr_warn("powerfs: setattr '%pd' comm error: %d, falling back\n",
                    dentry, err);
            /* 通信层错误，回退到内存模式 */
        }
    }

    /*
     * 第三步: 纯内存模式 - 直接修改本地 inode
     *
     * 使用 setattr_prepare + setattr_copy 来正确处理
     */
    err = setattr_prepare(&init_user_ns, dentry, attr);
    if (err)
        return err;

    /* 处理文件大小变更 */
    if (ia_valid & ATTR_SIZE) {
        if (attr->ia_size < i_size_read(inode)) {
            truncate_pagecache(inode, attr->ia_size);
        }
    }

    setattr_copy(&init_user_ns, inode, attr);
    mark_inode_dirty(inode);

    /* 更新缓存标志 */
    spin_lock(&pi->i_lock);
    pi->cache_valid = true;
    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
    spin_unlock(&pi->i_lock);

    pr_debug("powerfs: setattr '%pd' success (memory mode)\n", dentry);
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
     * 目标已存在的情况
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
        d_delete(new_dentry);
    } else if (flags & RENAME_NOREPLACE) {
        return -EEXIST;
    }

    /*
     * 纯本地操作: 使用 d_move 移动 dentry
     *
     * 注意: 必须在 d_move 之前保存 old_name，因为 d_move 会交换
     * dentry 的名称，之后 old_dentry->d_name.name 就变成了新名称。
     */
    {
        struct powerfs_msg_header req_hdr;
        struct powerfs_rename_req req_data;
        char old_name_buf[POWERFS_MAX_NAME_LEN + 1];
        char new_name_buf[POWERFS_MAX_NAME_LEN + 1];

        /* 在 d_move 之前保存名称 */
        strncpy(old_name_buf, old_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        old_name_buf[POWERFS_MAX_NAME_LEN] = '\0';
        strncpy(new_name_buf, new_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        new_name_buf[POWERFS_MAX_NAME_LEN] = '\0';

        /* 执行重命名 */
        d_move(old_dentry, new_dentry);

        /* 如果是目录且跨目录，调整父目录链接数 */
        if (S_ISDIR(inode->i_mode) && old_dir != new_dir) {
            drop_nlink(old_dir);
            inc_nlink(new_dir);
        }

        /* 按 inode 号排序获取锁，避免死锁 */
        if (old_dir != new_dir && old_dir->i_ino > new_dir->i_ino) {
            spin_lock(&new_dpi->i_lock);
            new_dpi->dir_complete = false;
            spin_unlock(&new_dpi->i_lock);

            spin_lock(&old_dpi->i_lock);
            old_dpi->dir_complete = false;
            spin_unlock(&old_dpi->i_lock);
        } else {
            spin_lock(&old_dpi->i_lock);
            old_dpi->dir_complete = false;
            spin_unlock(&old_dpi->i_lock);

            if (old_dir != new_dir) {
                spin_lock(&new_dpi->i_lock);
                new_dpi->dir_complete = false;
                spin_unlock(&new_dpi->i_lock);
            }
        }

        /* 更新时间戳 */
        old_dir->i_mtime = old_dir->i_ctime = current_time(old_dir);
        if (old_dir != new_dir)
            new_dir->i_mtime = new_dir->i_ctime = current_time(new_dir);

        mark_inode_dirty(old_dir);
        if (old_dir != new_dir)
            mark_inode_dirty(new_dir);

        /* 异步通知代理更新后端 (使用保存的旧名称) */
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
                                     &resp_hdr, resp_data, 5000);
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

/*
 * powerfs_readdir - 读取目录内容
 *
 * 参考 ceph_readdir (fs/ceph/dir.c)
 *
 * 策略:
 *   - 优先使用缓存的目录项
 *   - 缓存用完后从通信层批量获取
 *   - 如果通信层不可用，使用内存模式 (遍历dcache)
 */
int powerfs_readdir(struct file *file, struct dir_context *ctx)
{
    struct powerfs_dir_file_info *dfi = file->private_data;
    struct inode *dir = file_inode(file);
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    int ret = 0;
    bool dir_invalidated = false;
    int dotpos = 0;

    pr_debug("powerfs: readdir ino=%lu pos=%lld\n", dir->i_ino, ctx->pos);

    mutex_lock(&dfi->lock);

    /*
     * 检测目录是否被失效
     *
     * 当 INVALIDATE_NOTIFY 清除 dir_complete 标志时，
     * 说明目录内容可能已变更，需要重新从服务端获取
     */
    spin_lock(&dpi->i_lock);
    if (!dpi->dir_complete && powerfs_comm_connected()) {
        dir_invalidated = true;
        pr_debug("powerfs: readdir dir_complete=false, need refresh\n");
    }
    spin_unlock(&dpi->i_lock);

    /* 如果目录被失效，清除缓存并从头开始 */
    if (dir_invalidated) {
        if (dfi->cached_entries) {
            kfree(dfi->cached_entries);
            dfi->cached_entries = NULL;
        }
        dfi->cached_count = 0;
        dfi->cached_index = 0;
        dfi->next_offset = 0;
        ctx->pos = 0;

        /* 重新处理 "." 和 ".." (位置从 0 开始) */
        if (!dir_emit_dots(file, ctx)) {
            mutex_unlock(&dfi->lock);
            return 0;
        }
        dotpos = 2;
    } else {
        /* "." 和 ".." 由 VFS 处理，pos 从 2 开始是真实条目 */
        if (ctx->pos == 0) {
            if (!dir_emit_dots(file, ctx)) {
                mutex_unlock(&dfi->lock);
                return 0;
            }
            dotpos = 2;
        } else {
            dotpos = ctx->pos;
        }
    }

    /*
     * 步骤1: 尝试从通信层获取目录项
     */
    if (powerfs_comm_connected()) {
        /* 如果缓存已用完或被失效，尝试重新填充 */
        while (dfi->cached_index >= dfi->cached_count) {
            ret = powerfs_fill_readdir_cache(dfi, dir);
            if (ret < 0) {
                if (ret == -ENOENT) {
                    /* 没有更多条目 */
                    ret = 0;
                    goto out;
                }
                if (ret == -ENOTCONN) {
                    /* 通信层不可用，回退到内存模式 */
                    break;
                }
                goto out;
            }
            if (dfi->cached_count == 0) {
                /* 没有更多条目 */
                ret = 0;
                goto out;
            }
        }

        /* 从缓存中逐个取出 */
        while (dfi->cached_index < dfi->cached_count) {
            struct powerfs_dirent *entry = &dfi->cached_entries[dfi->cached_index];
            unsigned char d_type;

            switch (entry->type) {
            case 1: d_type = DT_REG; break;
            case 2: d_type = DT_DIR; break;
            case 3: d_type = DT_LNK; break;
            default: d_type = DT_UNKNOWN; break;
            }

            if (!dir_emit(ctx, entry->name, entry->name_len,
                          entry->ino, d_type)) {
                /* 用户缓冲区满了 */
                goto out;
            }

            dfi->cached_index++;
            dfi->next_offset++;
            ctx->pos++;
        }

        ret = 0;
        goto out;
    }

    /*
     * 步骤2: 内存模式 - 使用 dcache 遍历
     *
     * 参考 simple_readdir 的实现方式
     * 当目录的 dir_complete 为 true 时，可以直接从 dcache 读取
     */
    {
        struct dentry *dentry = file->f_path.dentry;
        struct list_head *p;
        loff_t pos = dotpos;  /* 从 "." 和 ".." 之后的位置开始 */
        bool found = false;

        spin_lock(&dentry->d_lock);

        list_for_each(p, &dentry->d_subdirs) {
            struct dentry *child = list_entry(p, struct dentry, d_child);
            struct inode *child_inode;
            bool skip = false;

            if (pos < ctx->pos) {
                pos++;
                continue;
            }

            spin_lock_nested(&child->d_lock, DENTRY_D_LOCK_NESTED);
            child_inode = d_inode(child);

            /* 检查 dentry 有效性: 如果 lease 已过期，跳过该条目 */
            {
                struct powerfs_dentry_info *di = child->d_fsdata;
                if (di && powerfs_comm_connected()) {
                    if (time_after_eq(jiffies, di->lease_expire)) {
                        skip = true;
                        pr_debug("powerfs: readdir skip invalidated '%pd'\n", child);
                    }
                }
            }

            if (child_inode && !skip) {
                unsigned char d_type = DT_UNKNOWN;

                if (S_ISDIR(child_inode->i_mode))
                    d_type = DT_DIR;
                else if (S_ISREG(child_inode->i_mode))
                    d_type = DT_REG;
                else if (S_ISLNK(child_inode->i_mode))
                    d_type = DT_LNK;

                if (!dir_emit(ctx, child->d_name.name, child->d_name.len,
                              child_inode->i_ino, d_type)) {
                    spin_unlock(&child->d_lock);
                    found = true;
                    break;
                }
                ctx->pos++;
            }
            spin_unlock(&child->d_lock);
            pos++;
        }

        spin_unlock(&dentry->d_lock);

        /* 如果 dir_complete，表示我们已经遍历了所有条目 */
        if (dpi->dir_complete && !found) {
            /* 遍历完成 */
        }
    }

out:
    mutex_unlock(&dfi->lock);
    return ret;
}

/* 目录文件操作表 */
static const struct file_operations powerfs_dir_operations = {
    .open           = powerfs_dir_open,
    .release        = powerfs_dir_release,
    .iterate_shared = powerfs_readdir,
    .llseek         = generic_file_llseek,
};

/* ========== 地址空间操作 (page cache) ========== */

/*
 * powerfs_read_folio - 读取页面 (从后端服务加载数据)
 *
 * 参考 ceph_readpage (fs/ceph/addr.c)
 *
 * 注意: Linux 5.18+ 使用 read_folio 替代 readpage
 *       参数从 struct page 变为 struct folio
 */
static int powerfs_read_folio(struct file *file, struct folio *folio)
{
    struct page *page = &folio->page;
    struct inode *inode = page->mapping->host;
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_read_req req_data;
    struct powerfs_read_resp *resp_data;
    int ret;
    void *page_addr;

    pr_debug("powerfs: read_folio ino=%lu index=%lu\n",
             inode->i_ino, folio->index);

    /*
     * 如果通信层不可用 (纯内存模式)，用零填充页面
     * 这和 ramfs 的行为一致：新创建的文件没有数据
     */
    if (!powerfs_comm_connected()) {
        folio_zero_range(folio, 0, folio_size(folio));
        folio_mark_uptodate(folio);
        folio_unlock(folio);
        return 0;
    }

    /* 分配响应缓冲区 (需要足够大容纳完整响应数据) */
    resp_data = kmalloc(POWERFS_MAX_DATA_SIZE, GFP_KERNEL);
    if (!resp_data)
        return -ENOMEM;

    /* 构建请求 */
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_READ;
    req_hdr.ino = inode->i_ino;
    req_hdr.data_len = sizeof(req_data);

    memset(&req_data, 0, sizeof(req_data));
    req_data.ino = inode->i_ino;
    req_data.offset = (u64)page->index << PAGE_SHIFT;
    req_data.length = PAGE_SIZE;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(resp_data, 0, sizeof(*resp_data));

    /* 发送请求 */
    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, resp_data, 5000);
    if (ret < 0) {
        kfree(resp_data);
        folio_set_error(folio);
        folio_unlock(folio);
        return ret;
    }

    if (resp_hdr.status != 0) {
        kfree(resp_data);
        folio_set_error(folio);
        folio_unlock(folio);
        return resp_hdr.status;
    }

    /* 将数据拷贝到页面 */
    page_addr = kmap_local_folio(folio, 0);
    if (resp_data->length > 0) {
        memcpy(page_addr, resp_data->data, resp_data->length);
        if (resp_data->length < PAGE_SIZE)
            memset(page_addr + resp_data->length, 0,
                   PAGE_SIZE - resp_data->length);
    } else {
        memset(page_addr, 0, PAGE_SIZE);
    }
    kunmap_local(page_addr);

    kfree(resp_data);

    folio_mark_uptodate(folio);
    folio_unlock(folio);

    pr_debug("powerfs: read_folio complete, read %u bytes\n",
             resp_data ? resp_data->length : 0);
    return 0;
}

/*
 * powerfs_writepage - 写入页面 (写回后端服务)
 *
 * 参考 ceph_writepage (fs/ceph/addr.c)
 *
 * 策略:
 *   - 通信层可用时: 写回后端服务
 *   - 纯内存模式: 直接标记为干净 (因为内存文件系统不需要持久化)
 */
int powerfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = page->mapping->host;
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_write_req *req_data;
    struct powerfs_write_resp resp_data;
    void *page_addr;
    int ret;
    u32 write_len;
    u64 file_size;
    void *buf;

    pr_debug("powerfs: writepage ino=%lu index=%lu\n",
             inode->i_ino, page->index);

    /*
     * 如果通信层不可用 (纯内存模式)，直接标记为干净
     * 这和 ramfs 的行为一致
     */
    if (!powerfs_comm_connected()) {
        end_page_writeback(page);
        return 0;
    }

    file_size = i_size_read(inode);
    write_len = PAGE_SIZE;

    /* 如果是文件的最后一页，可能只写部分数据 */
    if ((u64)(page->index + 1) << PAGE_SHIFT > file_size) {
        if ((u64)page->index << PAGE_SHIFT >= file_size) {
            /* 完全在文件大小之外，跳过 */
            end_page_writeback(page);
            return 0;
        }
        write_len = file_size - ((u64)page->index << PAGE_SHIFT);
    }

    /* 分配请求缓冲区 (头部 + 数据) */
    buf = kmalloc(sizeof(*req_data) + write_len, GFP_KERNEL);
    if (!buf) {
        redirty_page_for_writepage(wbc, page);
        return -ENOMEM;
    }

    req_data = buf;

    /* 构建请求头部 */
    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_WRITE;
    req_hdr.ino = inode->i_ino;
    req_hdr.data_len = sizeof(*req_data) + write_len;

    memset(req_data, 0, sizeof(*req_data));
    req_data->ino = inode->i_ino;
    req_data->offset = (u64)page->index << PAGE_SHIFT;
    req_data->length = write_len;

    /* 从页面拷贝数据 */
    page_addr = kmap(page);
    memcpy((u8 *)buf + sizeof(*req_data), page_addr, write_len);
    kunmap(page);

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(&resp_data, 0, sizeof(resp_data));

    /* 发送请求 */
    ret = powerfs_comm_send_request(&req_hdr, buf,
                                     &resp_hdr, &resp_data, 5000);
    kfree(buf);

    if (ret < 0) {
        redirty_page_for_writepage(wbc, page);
        return ret;
    }

    if (resp_hdr.status != 0) {
        redirty_page_for_writepage(wbc, page);
        return resp_hdr.status;
    }

    end_page_writeback(page);

    pr_debug("powerfs: writepage complete, wrote %u bytes\n",
             resp_data.written);
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
     * 如果页面不是最新的，需要从后端读取
     * 但以下情况跳过后端读取:
     *   1. file 为 NULL (page_symlink 调用)
     *   2. 页面超出 i_size (新页面无后端数据)
     *   3. 通信层不可用
     */
    if (!PageUptodate(page)) {
        bool need_read = false;

        if (file && powerfs_comm_connected() &&
            (pos + len) <= i_size_read(inode)) {
            need_read = true;
        }

        if (need_read) {
            struct folio *folio = page_folio(page);
            ret = powerfs_read_folio(file, folio);
            if (ret < 0) {
                unlock_page(page);
                put_page(page);
                return ret;
            }
            lock_page(page);
        } else {
            /*
             * 新页面或纯内存模式
             * 必须清零整个页面，避免残留旧数据
             * （如之前inode释放后页面被复用的情况）
             */
            zero_user(page, 0, PAGE_SIZE);
            SetPageUptodate(page);
        }
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
 *   - 通信层可用: 立即同步到用户态代理
 *   - 通信层不可用: 仅写入 page cache (纯内存模式)
 */
int powerfs_write_end(struct file *file, struct address_space *mapping,
                       loff_t pos, unsigned int len, unsigned int copied,
                       struct page *page, void *fsdata)
{
    struct inode *inode = mapping->host;
    loff_t end_pos = pos + copied;
    int ret = copied;

    pr_debug("powerfs: write_end ino=%lu pos=%lld copied=%u\n",
             inode->i_ino, pos, copied);

    if (copied > 0) {
        /* 更新文件大小 */
        if (end_pos > i_size_read(inode)) {
            i_size_write(inode, end_pos);
            mark_inode_dirty(inode);
        }

        /*
         * 如果通信层可用，立即同步数据到后端
         * 采用分块发送策略，每块数据不超过 POWERFS_MAX_DATA_SIZE
         *
         * 计算: 最大数据长度 = POWERFS_MAX_DATA_SIZE - sizeof(write_req)
         *       = 4096 - 24 = 4072 字节/块
         */
        if (powerfs_comm_connected() && file) {
            void *page_addr;
            u64 file_size;
            u32 total_len;
            u32 max_data_per_block;
            u64 page_offset;
            u32 remaining;
            bool sync_failed = false;

            file_size = i_size_read(inode);
            total_len = PAGE_SIZE;

            /* 如果是文件的最后一页，可能只写部分数据 */
            if ((u64)(page->index + 1) << PAGE_SHIFT > file_size) {
                if ((u64)page->index << PAGE_SHIFT >= file_size) {
                    /* 完全在文件大小之外，不需要同步 */
                    set_page_dirty(page);
                    unlock_page(page);
                    put_page(page);
                    return copied;
                }
                total_len = file_size - ((u64)page->index << PAGE_SHIFT);
            }

            /* 计算每块最大可携带的数据长度 */
            max_data_per_block = POWERFS_MAX_DATA_SIZE -
                                 sizeof(struct powerfs_write_req);

            page_addr = kmap(page);
            page_offset = (u64)page->index << PAGE_SHIFT;
            remaining = total_len;

            /* 分块发送数据 */
            while (remaining > 0) {
                struct powerfs_msg_header req_hdr, resp_hdr;
                struct powerfs_write_req *req_data;
                struct powerfs_write_resp resp_data;
                void *buf;
                u32 chunk_len;
                u32 offset_in_page;
                int send_ret;

                chunk_len = min(remaining, max_data_per_block);
                offset_in_page = total_len - remaining;

                /* 分配请求缓冲区 (头部 + 数据) */
                buf = kmalloc(sizeof(*req_data) + chunk_len, GFP_KERNEL);
                if (!buf) {
                    pr_warn("powerfs: write_end kmalloc failed\n");
                    sync_failed = true;
                    break;
                }

                req_data = buf;

                /* 构建请求 */
                memset(&req_hdr, 0, sizeof(req_hdr));
                req_hdr.type = POWERFS_MSG_WRITE;
                req_hdr.ino = inode->i_ino;
                req_hdr.data_len = sizeof(*req_data) + chunk_len;

                memset(req_data, 0, sizeof(*req_data));
                req_data->ino = inode->i_ino;
                req_data->offset = page_offset + offset_in_page;
                req_data->length = chunk_len;

                /* 从页面拷贝数据 */
                memcpy((u8 *)buf + sizeof(*req_data),
                       (u8 *)page_addr + offset_in_page, chunk_len);

                memset(&resp_hdr, 0, sizeof(resp_hdr));
                memset(&resp_data, 0, sizeof(resp_data));

                /* 同步发送请求 */
                send_ret = powerfs_comm_send_request(&req_hdr, buf,
                                                     &resp_hdr, &resp_data,
                                                     5000);
                kfree(buf);

                if (send_ret >= 0 && resp_hdr.status == 0) {
                    /* 此块同步成功 */
                    pr_debug("powerfs: write_end chunk sent: offset=%llu len=%u\n",
                             (unsigned long long)req_data->offset, chunk_len);
                    remaining -= chunk_len;
                } else {
                    pr_warn("powerfs: write_end chunk sync failed "
                            "(ret=%d, status=%d) at offset=%u\n",
                            send_ret, resp_hdr.status, offset_in_page);
                    sync_failed = true;
                    break;
                }
            }

            kunmap(page);

            if (sync_failed) {
                /* 同步失败，标记页面为脏以便 writepage 后台重试 */
                pr_warn("powerfs: write_end sync failed, "
                        "marking page dirty for writepage retry\n");
                set_page_dirty(page);
            } else {
                /* 所有块同步成功，标记页面为干净 */
                ClearPageDirty(page);
                set_page_writeback(page);
                end_page_writeback(page);
            }
        } else {
            /* 纯内存模式，标记页面为脏 */
            set_page_dirty(page);
        }
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
                                     &resp_hdr, &resp_data, 5000);
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

    pr_info("powerfs: fill_super done, root ino=%lu\n", root->i_ino);
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
