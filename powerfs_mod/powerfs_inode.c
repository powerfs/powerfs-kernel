/*
 * powerfs_inode.c - split from powerfs_fs.c
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

/* Forward declarations: static inode_operations structs are defined near the
 * bottom of this file but referenced earlier in powerfs_init_inode(). Keep
 * them static (single-TU use) — only the forward symbol declaration is needed. */
static const struct inode_operations powerfs_dir_inode_operations;
static const struct inode_operations powerfs_file_inode_operations;

/* ========== 异步 inode 刷新工作队列 ==========
 * 用于 powerfs_invalidate_one: 从 NOTIFY 回调中异步刷新 inode 元数据.
 * 独立于通信层调度器线程, 避免 self-deadlock (调度器等待自己处理的响应). */
struct powerfs_refresh_work {
    struct work_struct work;
    u64 ino;
    struct inode *inode;  /* igrab 引用, work 完成后 iput */
    struct rcu_head rcu;  /* RCU 延迟释放 (workqueue 在 work_fn 返回后仍引用 work_struct) */
};
struct workqueue_struct *powerfs_refresh_wq;
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
    strncpy(pi->name, name ? name : "", POWERFS_MAX_NAME_LEN);
    pi->name[POWERFS_MAX_NAME_LEN] = '\0';
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
void powerfs_inode_init_once(void *foo)
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
        if (sbi && sbi->cap_cachep)
            kmem_cache_free(sbi->cap_cachep, cap);
        else
            kfree(cap);
    }
    pi->i_auth_cap = NULL;

    /* 5c. 清理 cap_flush_list (不应有残留, 但安全起见).
     *    cap_flush 由 kmem_cache_alloc(cap_flush_cachep) 分配, 必须用
     *    kmem_cache_free 释放 (不能用 kfree, 否则 slab 计账泄漏). */
    while (!list_empty(&pi->i_cap_flush_list)) {
        struct powerfs_cap_flush *cf;
        cf = list_first_entry(&pi->i_cap_flush_list,
                              struct powerfs_cap_flush, i_list);
        list_del(&cf->i_list);
        list_del(&cf->g_list);
        if (sbi && sbi->cap_flush_cachep)
            kmem_cache_free(sbi->cap_flush_cachep, cf);
        else
            kfree(cf);
    }

    /* 5d. 清理 cap_snaps (对齐 xxx: put all cap_snaps).
     *    cap_snap 由 kmem_cache_alloc(cap_snap_cachep) 分配, 必须用
     *    kmem_cache_free 释放 (不能用 kfree, 否则 slab 计账泄漏). */
    while (!list_empty(&pi->i_cap_snaps)) {
        struct powerfs_cap_snap *cs;
        cs = list_first_entry(&pi->i_cap_snaps,
                              struct powerfs_cap_snap, ci_item);
        list_del(&cs->ci_item);
        if (sbi && sbi->cap_snap_cachep)
            kmem_cache_free(sbi->cap_snap_cachep, cs);
        else
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
/*
 * powerfs_setattr_work_fn - 异步 setattr work 函数 (WQ_UNBOUND 上下文)
 *
 * 在 writeback_wq (WQ_UNBOUND) 中执行, 不阻塞 per-CPU writeback workqueue.
 * 读取最新 i_size (可能已有多次 writeback 累积), 调用 powerfs_net_setattr
 * 同步到 Filer, 成功后更新 pi->content_size.
 */
void powerfs_setattr_work_fn(struct work_struct *work)
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
int powerfs_write_inode(struct inode *inode, struct writeback_control *wbc)
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
    /* RCU walk 模式 (inode 不可上锁, 不允许阻塞) — 强制回退到 ref-walk */
    if (mask & MAY_NOT_BLOCK)
        return -ECHILD;

    /* 标准 VFS 位校验 (含 ACL/ capability 叠加).
     *
     * 不调 powerfs_get_caps(AUTH_SHARED): permission 是 path walk 的一部分,
     * 每次 ls/stat/access 都会走, 同步等 cap grant 会阻塞 (实测 4 个 ls 卡
     * D 状态 14+ 分钟, 堆栈在 powerfs_get_caps). inode 的 mode/uid/gid 已被
     * refresh_work 周期同步 (POWERFS_INODE_CACHE_TTL) + 每次 lookup 返回最新
     * attrs, 足够 permission 校验使用. 对齐 ceph_permission: 纯本地
     * generic_permission, 不在 permission 路径发 RPC. */
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
         *   Filer 端 size 仍为 0 → remount 后文件为空
         *
         * 工程规范: 修改 content_size (powerfs private 字段) 持 pi->i_lock,
         * 与其他 pi->i_lock 读路径构成 happens-before。
         * truncate_setsize 内部持 inode->i_lock 更新 i_size，故本处安全。 */
        spin_lock(&pi->i_lock);
        pi->content_size = (__u64)attr->ia_size;
        spin_unlock(&pi->i_lock);

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

        /* P0-3 fix: ATTR_SIZE=0 (O_TRUNC / truncate-to-zero) 必须显式通知
         * Filer 清空 chunks[] 列表。单靠 powerfs_net_setattr(SIZE=0)
         * 只更新 inode.size，Filer 侧 chunks[] 数组仍保留旧 needle 条目 →
         * 另一客户端后续 lookup→read→locate 返回旧 chunk needle，
         * 若 Volume Server needle 仍保留 4KB 未 GC，则 read 读回非零旧数据，
         * 破坏"空文件读 0"语义。
         * 用 UPDATE_INODE size=0 chunks=NULL 的强一致 RPC (对齐 FUSE
         * sync_size_chunks_on_close ino size=0 路径)。 */
        if ((ia_valid & ATTR_SIZE) && attr->ia_size == 0 &&
            powerfs_net_is_connected()) {
            u64 shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : inode->i_ino);
            int uret;
            uret = powerfs_net_update_inode_size_chunks(shard_id, inode->i_ino,
                                                         0ULL, /* size=0 */
                                                         "kernel-setattr-trunc0",
                                                         NULL, 0, /* chunks=empty */
                                                         NULL, 0); /* inline_data=none */
            if (uret < 0)
                pr_warn("powerfs: setattr trunc0 update_inode_size_chunks "
                        "ino=%lu failed: %d (next flush/release will retry)\n",
                        inode->i_ino, uret);
            else
                pr_debug("powerfs: setattr trunc0 ino=%lu Filer chunks cleared\n",
                         inode->i_ino);
        }
    }

    pr_debug("powerfs: setattr '%pd' success (async)\n", dentry);
    return 0;
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
