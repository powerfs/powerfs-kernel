/*
 * powerfs_lease.c - split from powerfs_fs.c
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
int ensure_lease(struct inode *inode, loff_t offset)
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
int powerfs_get_lease_token(struct inode *inode, loff_t offset,
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
void release_all_leases(struct inode *inode)
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

void powerfs_lease_renew_work_func(struct work_struct *work)
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
