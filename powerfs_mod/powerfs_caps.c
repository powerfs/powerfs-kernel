/*
 * powerfs_caps.c - split from powerfs_fs.c
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
void powerfs_cap_recall_notify_handler(u64 ino,
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
void powerfs_cap_upgrade_notify_handler(u64 ino,
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
int cap_open_grant_and_issue(struct powerfs_inode_info *pi, bool is_write_open)
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
int cap_send_release(struct powerfs_inode_info *pi, struct powerfs_cap *cap)
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
 *   0. 预先在锁外分配 cf (避免持 spinlock + kmem_cache_alloc 的 GFP_* 触发睡眠
 *      → preempt_count > 0 时的 "sleeping function called from invalid context" BUG).
 *      cf_alloc == NULL 且无预分配槽时, 进入锁内用 GFP_ATOMIC 最后手段 (不睡眠).
 *   1. 取得 powerfs_cap_flush 记录, 挂到 i_cap_flush_list + 全局 cap_flush_list
 *   2. 将 dirty_caps 移到 flushing_caps
 *   3. 发送 CapFlush RPC 到 Filer (TODO: 接入 powerfs_net 层)
 *   4. 等待 i_cap_wq 唤醒 (ACK 回调唤醒)
 *
 * 调用方不持锁.
 *
 * P0-0 修复 (vm1 ×1 GFP BUG): 原实现在 L1079-L1092 持 pi->i_lock (spin_lock) 期间
 * 执行 kmem_cache_alloc(GFP_NOFS). 尽管 GFP_NOFS 比 GFP_KERNEL 更严, 但在
 * PREEMPT_DYNAMIC + 持 spin_lock (preempt_count ≥ 1) 的上下文中, slab allocator
 * 仍可能进入 direct reclaim / compaction 路径而调用 might_sleep(), 触发
 * `BUG: sleeping function called from invalid context at mm.h:321`
 * (preempt_count=1, in_atomic=1, kmem_cache_alloc 偏移 0x53b).
 * 修复方式严格遵循 "持锁不分配, 分配不持锁":
 *   ① 在 spin_lock_irqsave 前尝试 GFP_KERNEL (1st alloc, 无竞态安全可睡眠);
 *   ② 锁内先取预分配槽 i_prealloc_cap_flush;
 *   ③ 1st 分配 NULL 且无预分配 → GFP_ATOMIC (持锁内最后手段, 保证不睡眠);
 *   ④ 仍失败 → 回滚脏位 + 返回 -ENOMEM, 下次重试 (宁可重复推送不丢脏). */
int powerfs_cap_flush(struct powerfs_inode_info *pi, unsigned int mask)
{
    struct inode *inode = &pi->netfs.inode;
    struct powerfs_cap_flush *cf;
    struct powerfs_cap_flush *cf_prealloc = NULL;  /* 锁外 GFP_KERNEL 预分配 */
    struct powerfs_sb_info *sbi;
    unsigned int flushing;
    unsigned int need_data_flush = 0;
    unsigned int need_attr_flush = 0;
    unsigned int need_xattr_flush = 0;
    unsigned int need_inline_flush = 0;
    int ret = 0;

    sbi = POWERFS_SB_INFO(inode->i_sb);

    /* === Step 0: 锁外 1st 分配 (GFP_KERNEL, 允许睡眠, 不在 atomic context) ===
     * 绝大多数情况这里直接拿到对象, 后续锁内只是 "取指针 or 退栈 GFP_ATOMIC".
     * 分配失败不致命: 进入锁内优先用 inode 预分配槽, 仍空再 GFP_ATOMIC. */
    if (sbi && sbi->cap_flush_cachep) {
        cf_prealloc = kmem_cache_alloc(sbi->cap_flush_cachep, GFP_KERNEL);
        /* cf_prealloc == NULL 正常进入, 不在此做失败分支 */
    }

    spin_lock(&pi->i_lock);

    /* 取 dirty_caps 与 mask 的交集 (需要 flush 的位) */
    flushing = pi->i_dirty_caps & mask;
    if (!flushing) {
        spin_unlock(&pi->i_lock);
        /* 无 flush 工作 → 锁外预分配若成功 → 作为 inode 预分配槽缓存 (下一次命中) */
        if (cf_prealloc) {
            spin_lock(&pi->i_lock);
            if (!pi->i_prealloc_cap_flush)
                pi->i_prealloc_cap_flush = cf_prealloc;
            else
                kmem_cache_free(sbi->cap_flush_cachep, cf_prealloc);
            spin_unlock(&pi->i_lock);
        }
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

    /* --- 分配 cf: 优先级 ① inode 预分配槽 → ② 锁外 GFP_KERNEL → ③ GFP_ATOMIC 最后手段 ---
     * 所有分配都在持 spin_lock 期间不会触发任何可能睡眠的路径:
     *   - ① inode 槽: 只是取指针, 0 分配开销
     *   - ② cf_prealloc: 已在锁外完成分配, 此处只引用
     *   - ③ GFP_ATOMIC: 明确 __GFP_DIRECT_RECLAIM 清空, 保证不睡眠
     *                     (在 preempt_count ≥ 1 下安全) */
    if (pi->i_prealloc_cap_flush) {
        cf = pi->i_prealloc_cap_flush;
        pi->i_prealloc_cap_flush = NULL;
        /* 消耗了 inode 预分配槽 → 若 ② cf_prealloc 存在则把它补到预分配槽,
         * 让下一次 cap_flush 在 ① 直接命中, 省一次 slab 开销. */
        if (cf_prealloc) {
            pi->i_prealloc_cap_flush = cf_prealloc;
            cf_prealloc = NULL;
        }
    } else if (cf_prealloc) {
        /* inode 槽空, 但锁外 1st 分配已拿到 → 直接使用, 零持锁内分配. */
        cf = cf_prealloc;
        cf_prealloc = NULL;
    } else {
        /* 极端路径: ① ② 都没有 → GFP_ATOMIC 最后手段.
         * 注意 kmem_cache_alloc(GFP_ATOMIC) 在内存压力非常高时可能失败,
         * 此时我们回滚 flushing → dirty, 让下一次 recall/fsync 再试.
         * 在 preempt_count ≥ 1 下, GFP_ATOMIC 绝对不会触发 might_sleep(). */
        BUILD_BUG_ON(!(GFP_ATOMIC & __GFP_HIGH));
        cf = kmem_cache_alloc(sbi->cap_flush_cachep, GFP_ATOMIC);
        if (!cf) {
            /* 内存不足: 回滚, dirty 保留, 下次再推 (宁可推两遍也不丢脏). */
            pi->i_dirty_caps |= flushing;
            pi->i_flushing_caps &= ~flushing;
            spin_unlock(&pi->i_lock);
            return -ENOMEM;
        }
    }
    /* cf_prealloc 若到此处仍非 NULL (理论上不会, 已在优先级①中消耗补槽):
     * 安全起见释放回 slab, 不泄漏. */
    if (unlikely(cf_prealloc)) {
        kmem_cache_free(sbi->cap_flush_cachep, cf_prealloc);
        cf_prealloc = NULL;
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

    /* P0-1: wanted > issued → 发送 AcquireCap RPC 请求升级.
     * wanted & ~issued 计算出需要增量请求的内核 cap 位.
     * 转换到 wire 3-bit CapSet 后发 AcquireCap(0x96) RPC.
     * RPC 可阻塞 → 必须先释放 i_lock, RPC 完成后在 i_lock 内 cap_issue.
     *
     * 防止重复 RPC: i_flushing_caps 非空时说明上一轮 flush 正在走,
     * 等 flush 完成后再 check_caps (flush 结束 wake_up_all 会重新调 check_caps). */
    {
        unsigned int need_acquire = wanted & ~issued;
        unsigned int k_issued;
        __u8 wire_wanted, wire_granted = 0;
        __u64 new_epoch = 0, new_sn = 0, new_dur = 0;
        char grant_token[64];
        size_t grant_toklen = sizeof(grant_token);
        char cid[64];
        size_t cid_len;
        size_t cur_toklen;
        int acq_ret;
        struct powerfs_cap *cap = pi->i_auth_cap;

        if (need_acquire && cap && !pi->i_flushing_caps) {
            /* 有 cap 且非 flushing → 转换 wanted 到 wire 格式发 RPC */
            wire_wanted = kernel_bits_to_wire_capset(need_acquire);
            if (wire_wanted == 0) {
                /* 无 wire 映射的位 (如 PIN) → 不发 RPC, 直接解锁返回 */
                spin_unlock(&pi->i_lock);
                return;
            }
            cur_toklen = strlen(cap->token);
            cid_len = get_mount_client_id(inode->i_sb, cid, sizeof(cid));

            pr_debug("powerfs: check_caps ACQUIRE ino=%lu want_k=0x%x wire_w=0x%02x "
                     "issued=0x%x token_len=%zu\n",
                     inode->i_ino, need_acquire, wire_wanted,
                     issued, cur_toklen);

            /* 释放 i_lock (RPC 可阻塞) */
            spin_unlock(&pi->i_lock);

            acq_ret = powerfs_net_cap_acquire(inode->i_ino, cid,
                                               cap->token, cur_toklen,
                                               wire_wanted,
                                               grant_token, &grant_toklen,
                                               &wire_granted, &new_epoch,
                                               &new_sn, &new_dur);

            if (acq_ret == 0) {
                /* 成功: 在 i_lock 内更新 cap + cap_issue */
                spin_lock(&pi->i_lock);
                /* 升级 token (Filer 可能换了 token) */
                if (grant_toklen > 0 && grant_toklen < sizeof(cap->token)) {
                    memcpy(cap->token, grant_token, grant_toklen);
                    cap->token[grant_toklen] = '\0';
                }
                cap->epoch = new_epoch;
                cap->seq = new_sn;
                cap->issue_seq = new_sn;
                if (new_dur > 0)
                    cap->expire_jiffies = jiffies + msecs_to_jiffies(new_dur);
                else
                    cap->expire_jiffies = jiffies + POWERFS_LEASE_DURATION;

                k_issued = wire_capset_to_kernel_bits(wire_granted);
                powerfs_cap_issue(pi, cap, k_issued);
                spin_unlock(&pi->i_lock);
                wake_up_all(&pi->i_cap_wq);

                pr_debug("powerfs: check_caps ACQUIRE ok ino=%lu wire_g=0x%02x "
                         "kernel=0x%x epoch=%llu sn=%llu\n",
                         inode->i_ino, wire_granted, k_issued,
                         (unsigned long long)new_epoch,
                         (unsigned long long)new_sn);
                return;
            } else {
                pr_debug("powerfs: check_caps ACQUIRE ino=%lu ret=%d (will retry)\n",
                         inode->i_ino, acq_ret);
                return;
            }
        }
    }

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
