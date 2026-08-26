/*
 * powerfs_locks.c - split from powerfs_fs.c
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
