/*
 * powerfs_dentry.c - split from powerfs_fs.c
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

#include "powerfs_vfs.h"

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
void powerfs_invalidate_dir_lease(struct inode *dir);

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
void powerfs_fill_dentry_lease(struct dentry *dentry,
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
    struct inode *dino;
    bool is_negative;

    di = dentry->d_fsdata;
    dino = d_inode(dentry);
    is_negative = !dino || (di && (di->flags & POWERFS_DN_NEGATIVE));

    /* ====== 负 dentry 统一策略: 不缓存, 强制 re-lookup ======
     * 远程写入会创建文件, 但本客户端的父目录 i_shared_gen 不会
     * 实时更新 (没有 server push invalidation). 因此:
     *   Layer 1 (per-dentry lease): 负 dentry 跳过 (已做到)
     *   Layer 2 (dir_shared_gen + I_COMPLETE): 负 dentry 也跳过,
     *       否则 "父目录 shared_gen 永不变 → Layer 2 永远命中 → 永远信任 negative → ENOENT"
     *   Layer 3 miss:
     *       RCU: -ECHILD 降级 REF
     *       REF:  return 0 → VFS d_drop/d_invalidate + 真正 lookup RPC
     * 对齐 FUSE (negative_dentry_timeout=0) + NFS (no cache for
     * negative 除非有 dir cache coherency). */
    if (is_negative) {
        if (flags & LOOKUP_RCU)
            return -ECHILD;
        /* REF: 丢 dentry + 重跑 lookup */
        {
            struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir ? dir->i_sb : NULL);
            if (sbi && sbi->client)
                powerfs_metric_dentry_mis(&sbi->client->metrics);
            return 0;
        }
    }

    /* ====== 正 dentry: Layer 1 + Layer 2 (常规三层策略) ====== */
    if (flags & LOOKUP_RCU) {
        /* === RCU 路径 === */
        if (!dir)
            return -ECHILD;
        parent_pi = POWERFS_I(dir);
        if (!di)
            return -ECHILD;

        /* Layer 1: per-dentry lease */
        lease_expire = READ_ONCE(di->lease_expire);
        if (lease_expire && time_before(now, lease_expire))
            return 1;

        /* Layer 2: dir_shared_gen + I_COMPLETE */
        parent_shared_gen = (u64)atomic_read(&parent_pi->i_shared_gen);
        if (di->dir_shared_gen == parent_shared_gen &&
            (READ_ONCE(parent_pi->i_flags) & POWERFS_I_COMPLETE))
            return 1;

        /* Layer 3 miss → 降级 REF */
        return -ECHILD;
    }

    /* === REF 路径: 正 dentry 三层校验 === */
    if (!dir)
        return 1;
    parent_pi = POWERFS_I(dir);

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

    /* Layer 3: 正 dentry trust + 在后续 getattr/read 刷新 */
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
const struct dentry_operations powerfs_dentry_operations = {
    .d_revalidate   = powerfs_d_revalidate,
    .d_init         = powerfs_d_init,
    .d_release      = powerfs_d_release,
    .d_prune        = powerfs_d_prune,
    .d_delete       = powerfs_d_delete,    /* P2-3: lease 有效时保留 dentry */
};
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

    /* Guard against NULL workqueue: same rationale as powerfs_invalidate_one
     * in powerfs_inode.c — RX NOTIFY may arrive before fill_super created
     * powerfs_refresh_wq (register_client mount failed race) or after kill_sb
     * already destroyed it. Without this, queue_work(NULL, work) dereferences
     * offset 0x100 into a NULL pointer (__queue_work flags field) and Oopses. */
    if (!powerfs_refresh_wq) {
        pr_warn_ratelimited("powerfs: invalidate_dentry parent=%llu name=%.*s skipped (refresh_wq NULL)\n",
                            parent_ino, (int)name_len, name);
        kfree(w);
        return -ENODEV;
    }

    queue_work(powerfs_refresh_wq, &w->work);
    pr_debug("powerfs: invalidate_dentry parent=%llu name=%.*s ver=%llu queued\n",
            parent_ino, (int)name_len, name, version);
    return 0;
}
EXPORT_SYMBOL_GPL(powerfs_invalidate_dentry);
