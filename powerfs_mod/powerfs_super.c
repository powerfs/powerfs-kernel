/*
 * powerfs_super.c - split from powerfs_fs.c
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

/* Forward declarations: static helpers defined later in this file but used
 * earlier (put_super calls the cleanup routines before their definition). */
static void powerfs_debugfs_init(struct super_block *sb);
static void powerfs_debugfs_cleanup(struct super_block *sb);
static void powerfs_proc_init(struct super_block *sb);
static void powerfs_proc_cleanup(struct super_block *sb);

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

const struct netfs_request_ops powerfs_netfs_ops = {
    .issue_read = powerfs_netfs_issue_read,
};
/* ========== 全局 slab 缓存 (参考 xxx 全局 cache) ========== */

struct kmem_cache *powerfs_inode_cachep;
struct kmem_cache *powerfs_dentry_cachep;

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
int powerfs_quota_check_max_files(struct inode *dir)
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
int powerfs_quota_check_max_bytes(struct inode *inode, loff_t newlen)
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
/*
 * powerfs_show_options - 显示挂载选项
 */
static int powerfs_show_options(struct seq_file *m, struct dentry *root)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(root->d_sb);

    seq_printf(m, ",master_addr=%s", sbi->master_addr);
    seq_printf(m, ",master_port=%u", sbi->master_port);
    seq_printf(m, ",transport=%s",
               sbi->transport_type == POWERFS_TRANSPORT_RDMA ? "rdma" : "tcp");

    return 0;
}

/*
 * powerfs_put_super - P1-1: VFS 在卸载前调用此回调 (deactivate_locked_super → put_super).
 *
 * 职责 (对齐  xxx_put_super → destroy_mdsc 全量资源回卷):
 *   1. 注销 Cap NOTIFY 回调 (防止 RX dispatcher 在销毁后仍派发到 fs 层)
 *
 * 注意: cap slab caches (cap_cachep / cap_flush_cachep / cap_snap_cachep)
 *   不在此销毁 — 必须在 kill_sb_super (evict_inodes 之后) 销毁, 否则
 *   kmem_cache_destroy 报 "Objects remaining" BUG.
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

    /* 2. per-sb cap slab caches 不在此销毁！
     *
     * VFS 卸载顺序: generic_shutdown_super → put_super → evict_inodes → kill_sb.
     * put_super 在 evict_inodes 之前执行, 此时 inode 仍持有 cap/cap_flush/cap_snap
     * 对象. 若在此销毁 slab, kmem_cache_destroy 会发现 "Objects remaining" BUG,
     * 且 evict_inode 后续发现 sbi->cap_cachep==NULL 时错误回退到 kfree, 破坏
     * slab 计账. 正确位置是 kill_sb_super (evict_inodes 之后). */
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

    /* Step 5: sync_inodes_sb 会触发新的 writepage work (powerfs_writepages
     *         通过 igrab(inode) 增加引用计数, 提交到 writeback_wq 异步执行).
     *         Step 2 的 flush 在 sync_inodes_sb 之前, 无法等待这些新 work.
     *         若不再次 flush, umount_begin → sync_fs 返回后 writepage work
     *         仍持有 igrab 引用 → mnt_count > 1 → umount 返回 EBUSY
     *         ("target is busy"). T6 60s 并发读写后尤其明显 (大量 dirty pages
     *         → sync_inodes_sb 创建多个 writepage batch work).
     *         修复: 在 sync_inodes_sb 之后再 flush 一次, 等 igrab 引用全部释放. */
    if (sbi && sbi->writeback_wq)
        flush_workqueue(sbi->writeback_wq);

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
    /* rcu_barrier 等待所有 pending RCU 回调完成:
     *   - d_release 用 call_rcu 延迟释放 dentry_info (powerfs_di_free_rcu)
     *   - VFS free_inode 路径也可能经 RCU 延迟释放 inode_info
     * 不等 grace period 直接 kmem_cache_destroy 会发现 "Objects remaining" BUG
     * (对象已"逻辑释放"但 kmem_cache_free 还没执行). */
    rcu_barrier();

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
        char transport[8];   /* "tcp" (默认) 或 "rdma" */
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

        /* transport: 解析 "tcp"/"rdma" → sbi->transport_type.
         * parse_param 已校验只接受 tcp/rdma, 这里防御性二次校验. */
        if (strcmp(ctx->transport, "rdma") == 0) {
#ifndef CONFIG_INFINIBAND
            pr_err("powerfs: transport=rdma requires CONFIG_INFINIBAND=y\n");
            kfree(sbi);
            kfree(ctx);
            sb->s_fs_info = NULL;
            return -EINVAL;
#else
            sbi->transport_type = POWERFS_TRANSPORT_RDMA;
#endif
        } else {
            /* 空字符串或 "tcp" 都默认 TCP (兼容旧 mount 命令不传 transport). */
            sbi->transport_type = POWERFS_TRANSPORT_TCP;
        }
        /* 释放 init_fs_context 分配的 ctx, 释放后 sb->s_fs_info 仍指向已释放内存,
         * 必须清除以避免 kill_sb 访问悬空指针 (fill_super 早期失败时 VFS 仍会
         * 调用 kill_sb). 下方 sb->s_fs_info = sbi 会重新设置. */
        kfree(ctx);
        ctx = NULL;
        sb->s_fs_info = NULL;
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
        sb->s_fs_info = NULL;
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
        int pool_ret = powerfs_conn_pool_init(maddr, mport, scount,
                                                sbi->transport_type);
        if (pool_ret != 0) {
            pr_err("powerfs: connection pool init failed (%d)\n", pool_ret);
            return pool_ret;
        }
        sbi->pool_initialized = true;
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

    /* FIX: fill_super 早期失败 (如 missing master_addr) 时 sbi 为 NULL 或
     * 未完全初始化. VFS 仍会调用 kill_sb, 此时不能执行 sync_filesystem
     * (sb->s_op 可能未设置)、stop_heartbeat、destroy_workqueue 等操作.
     * 直接 kill_anon_super 释放超级块并返回. */
    if (!sbi || !sbi->initialized) {
        pr_warn("powerfs: kill_sb early return (sbi=%p initialized=%d)\n",
                sbi, sbi ? sbi->initialized : -1);
        if (g_powerfs_sb == sb)
            g_powerfs_sb = NULL;
        kill_anon_super(sb);
        return;
    }

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
     *    阻止 reconnect_work 在 g_pool 清零后访问野指针.
     *
     * FIX: 仅当 conn_pool_init 曾成功完成 (sbi->pool_initialized) 时才执行
     * 全局清理. fill_super 早期失败 (如 missing master_addr) 触发的 kill_sb
     * 此时 sbi 可能已 kfree 或 pool 未初始化, 无条件清理会破坏其他活跃 mount
     * 的 g_pool (stopping=1, filer_count=0), 导致 "Transport endpoint is not
     * connected" 且不可恢复. */
    if (sbi && sbi->pool_initialized) {
        powerfs_net_set_stopping();

        /* 4. 关闭所有连接 (g_pool 会被清零) */
        powerfs_net_pool_cleanup();
    } else if (sbi) {
        pr_warn("powerfs: kill_sb skip global pool cleanup (pool_initialized=false)\n");
    }

    /* 5. kill_anon_super → generic_shutdown_super:
     *    a) shrink_dcache_for_umount — 释放所有 dentry (d_release → call_rcu)
     *    b) sync_filesystem — no-op (已在 step 1 sync 过)
     *    c) put_super — powerfs_put_super: 注销 notify 回调 + debugfs/proc 清理
     *    d) evict_inodes — 驱逐所有 i_count==0 inode (evict_inode 释放 cap/lease)
     *    e) dput(sb->s_root) — 驱逐 root inode (最后一个, 持有 root 的 cap)
     *
     * 必须在 cap_cachep/sbi 销毁之前调用! evict_inode 需要 sbi->cap_cachep
     * 来 kmem_cache_free cap 对象. 若先销毁 cap_cachep, evict_inode 回退到
     * kfree, 破坏 slab 计账 → "Objects remaining" BUG.
     * 网络 RPC (release_all_leases) 在 net_set_stopping 后立即返回 -ENOTCONN,
     * 不阻塞; 本地 cap/lease 清理在 evict_inode 中继续. */
    kill_anon_super(sb);

    /* 5b. 等待 kill_anon_super 期间排队的 RCU 回调完成.
     *
     * kill_anon_super → evict_inodes → d_release → call_rcu(powerfs_di_free_rcu)
     * 这些 call_rcu 排队在 step 7 的 rcu_barrier 之后, 不会被它等待.
     * 如果不在此处 rcu_barrier, 后续 kmem_cache_destroy(cap_cachep) 和
     * module_exit 中的 kmem_cache_destroy(dentry_cachep) 可能在 RCU 回调
     * 执行前完成, 导致 kmem_cache_free 到已销毁的缓存 → SLUB 元数据损坏
     * → 随机内存腐败 (如 bpf_prog_aux 被 SSH 字符串覆盖). */
    rcu_barrier();

    /* 6. 所有 inode 已被 evict_inode 驱逐, cap/cap_flush/cap_snap 对象已
     *    kmem_cache_free 回 slab. 现在安全销毁 per-sb cap slab caches. */
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
