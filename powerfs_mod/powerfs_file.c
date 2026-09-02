/*
 * powerfs_file.c - split from powerfs_fs.c
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

/* ========== 文件操作 ========== */

/*
 * powerfs_flush - flush 文件 (close(2) 前 VFS 针对每个 close-on-exec / dup 关闭调用)
 *
 * 语义对齐 FUSE `fn flush` fuse.rs L9006:
 *   - flush 是 close(2) 前 VFS 每调用一次 close() 都触发（不等最后一个 fd release），
 *     这让管道 / dup / close-on-exec 场景下（如 `tee FILE`、`dd of=FILE conv=fdatasync`）
 *     在应用 close 返回前就能把 data + metadata 提交到远端。
 *   - Guard had_dirty：仅在该 inode 有 dirty chunk/inline_dirty 时才真正 flush + sync size，
 *     避免读-only open close 时用 stale local i_size 覆盖 Filer 端已被另一 client
 *     写入的更大 size（对齐 FUSE A7 fix 2026-08-22 comment）。
 *
 * VFS 忽略 flush 返回值（失败仅记录到 filp f_wb_err），因此我们尽力同步并打印告警。
 */
static int powerfs_flush(struct file *file, fl_owner_t id)
{
    struct inode *inode = file_inode(file);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    u64 ino = inode->i_ino;
    bool had_dirty;
    int flush_ret = 0;
    int sync_ret = 0;

    pr_debug("powerfs: FLUSH ino=%lu f_mode=%x\n", ino, file->f_mode);

    /* § Snap had_dirty BEFORE flush (same guard as FUSE flush L9044).
     * 若本 open 是 read-only 或上次 flush/release 已全提交，had_dirty=false，
     * flush 仍执行以清空 page cache 中残留脏页（安全路径），但禁止 sync metadata。
     *
     * had_dirty 双判据 (Linux 6.17 基线):
     *   1. INLINE 模式: inline_dirty == true (K2-5 写入产生的未提交 inline_data)
     *   2. FLAT 模式  : inode->i_state & I_DIRTY_PAGES  (VFS 脏页标记)
     *                  || mapping_writably_mapped(mapping) (mmap 共享脏页窗口)
     * 避免 I_DIRTY_PAGES 只在 mark_inode_dirty 时设置导致 flush 后漏判。 */
    {
        bool inline_dirty_snap;
        bool pages_dirty_snap;
        spin_lock(&pi->i_lock);
        inline_dirty_snap = (pi->placement == POWERFS_PLACEMENT_INLINE &&
                             pi->inline_dirty);
        spin_unlock(&pi->i_lock);
        pages_dirty_snap = (inode->i_state & I_DIRTY_PAGES) != 0;
        had_dirty = inline_dirty_snap || pages_dirty_snap;
    }

    /* 写回所有脏页 (Flat: → volume server; Inline: 只清 pagecache 脏标,
     * inline_data 提交留在下方 sync step 避免与 inline_dirty 并发竞争) */
    flush_ret = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
    if (flush_ret)
        pr_warn("powerfs: FLUSH ino=%lu filemap_write_and_wait_range err=%d\n",
                ino, flush_ret);

    /* Only push metadata when BOTH conditions hold (对齐 FUSE L9055):
     *   1. had_dirty was true  before flush — 本 fd 生命周期内产生过脏态
     *   2. dirty is fully cleared after flush — 本次 flush 全完成
     * 否则跳过 sync，避免本地 stale size 覆盖另一客户端新提交的更大 size。
     *
     * dirty_after 判据与 had_dirty 对称 (Linux 6.17 基线)。 */
    if (had_dirty) {
        bool inline_dirty_after;
        bool pages_dirty_after;
        spin_lock(&pi->i_lock);
        inline_dirty_after = (pi->placement == POWERFS_PLACEMENT_INLINE &&
                              pi->inline_dirty);
        spin_unlock(&pi->i_lock);
        pages_dirty_after = (inode->i_state & I_DIRTY_PAGES) != 0;

        if (!(inline_dirty_after || pages_dirty_after)) {
            if (pi->placement == POWERFS_PLACEMENT_INLINE && pi->inline_dirty) {
                /* INLINE 路径 — 复用 release 骨架的快照+网络提交。
                 * flush 只做 1 次尝试；失败下次 release 重试兜底。 */
                u8 *snap_data = NULL;
                u32 snap_len = 0;
                u64 shard_id;

                spin_lock(&pi->i_lock);
                if (pi->inline_data && pi->inline_len > 0)
                    snap_len = pi->inline_len;
                spin_unlock(&pi->i_lock);

                if (snap_len > 0) {
                    snap_data = kmalloc(snap_len, GFP_KERNEL);
                    if (!snap_data) {
                        sync_ret = -ENOMEM;
                        goto flush_out;
                    }
                    spin_lock(&pi->i_lock);
                    if (pi->inline_data && pi->inline_len == snap_len) {
                        memcpy(snap_data, pi->inline_data, snap_len);
                    } else {
                        spin_unlock(&pi->i_lock);
                        kfree(snap_data);
                        goto flush_out; /* concurrent inode changed */
                    }
                    spin_unlock(&pi->i_lock);

                    shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);
                    sync_ret = powerfs_net_update_inode_size_chunks(shard_id, ino,
                                                                    (u64)snap_len,
                                                                    "kernel-flush",
                                                                    NULL, 0,
                                                                    snap_data, snap_len);
                    kfree(snap_data);
                    if (sync_ret == 0) {
                        /* 不清 inline_dirty：release 会重新查 dirty，安全重复提交
                         * (相同 size+data 幂等)；清掉反而可能让 release 跳过。 */
                        pr_debug("powerfs: FLUSH INLINE ino=%lu synced ok\n", ino);
                    } else {
                        pr_warn("powerfs: FLUSH INLINE ino=%lu update err=%d "
                                "(release path will retry)\n", ino, sync_ret);
                    }
                }
            } else if (pi->placement == POWERFS_PLACEMENT_FLAT && pi->volume_id && pi->file_key) {
                /* FLAT/Stripe 路径 — 复用 RELEASE FLAT 骨架：
                 * 构造 chunk_map 数组 + powerfs_net_update_inode_size_chunks。 */
                loff_t i_size = i_size_read(inode);
                u64 shard_id;
                u32 chunk_size = pi->layout_chunk_size ? pi->layout_chunk_size : POWERFS_CHUNK_SIZE;
                u32 chunk_count, i;
                struct powerfs_chunk_map *chunks = NULL;

                /* content_size 未变 → 上次已提交过，跳过避免 clobber */
                spin_lock(&pi->i_lock);
                if ((u64)i_size == pi->content_size) {
                    spin_unlock(&pi->i_lock);
                    goto flush_out;
                }
                spin_unlock(&pi->i_lock);

                if (i_size <= 0)
                    goto flush_out;

                chunk_count = (u32)div_u64(i_size + chunk_size - 1, chunk_size);
                if (chunk_count > 4096)
                    chunk_count = 4096;
                chunks = kmalloc_array(chunk_count, sizeof(*chunks), GFP_KERNEL);
                if (!chunks) {
                    sync_ret = -ENOMEM;
                    goto flush_out;
                }
                for (i = 0; i < chunk_count; i++) {
                    u64 chunk_off = (u64)i * chunk_size;
                    chunks[i].chunk_idx = i;
                    chunks[i].needle_id = pi->file_key + i;
                    chunks[i].volume_id  = pi->volume_id;
                    chunks[i].crc32 = 0;
                    chunks[i].size = (i == chunk_count - 1)
                                     ? (u64)i_size - chunk_off
                                     : (u64)chunk_size;
                }
                shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);
                sync_ret = powerfs_net_update_inode_size_chunks(shard_id, ino,
                                                                (u64)i_size,
                                                                "kernel-flush",
                                                                chunks, chunk_count,
                                                                NULL, 0);
                kfree(chunks);
                if (sync_ret == 0) {
                    spin_lock(&pi->i_lock);
                    pi->content_size = (u64)i_size;
                    spin_unlock(&pi->i_lock);
                    pr_debug("powerfs: FLUSH FLAT ino=%lu size=%lld chunks=%u ok\n",
                             ino, i_size, chunk_count);
                } else {
                    pr_warn("powerfs: FLUSH FLAT ino=%lu update err=%d "
                            "(release path will retry)\n", ino, sync_ret);
                }
            }
        } else {
            pr_debug("powerfs: FLUSH ino=%lu skip metadata sync (dirty remains after flush)\n",
                     ino);
        }
    } else {
        pr_debug("powerfs: FLUSH ino=%lu skip (had_dirty=false, read-only or already synced)\n",
                 ino);
    }

flush_out:
    return (flush_ret < 0) ? flush_ret : sync_ret;
}

/*
 * powerfs_llseek - 自定义 llseek (替换 generic_file_llseek)
 *
 * 对齐 Ceph `ceph_llseek` file.c L2515:
 *   - SEEK_END / SEEK_DATA / SEEK_HOLE 依赖 Filer 权威 inode size，
 *     不能直接读本地 inode->i_size（另一个客户端写完 Filer 已更新，
 *     本端缓存可能 stale，generic_file_llseek 直接读到旧值 → SEEK_END+write
 *     会写错 offset 或与 Filer size 冲突）。
 *   - 其他 whence (SEEK_SET / SEEK_CUR / SEEK_{MAX}) 走 generic 快速路径。
 *
 * RPC 失败则透传 errno（可能 -ENOTCONN / -ETIMEDOUT）。
 */
static loff_t powerfs_llseek(struct file *file, loff_t offset, int whence)
{
    struct inode *inode = file_inode(file);

    if (whence == SEEK_END || whence == SEEK_DATA || whence == SEEK_HOLE) {
        __u64 remote_size = 0;
        int ret;

        pr_debug("powerfs: llseek ino=%lu whence=%d NEED_AUTHORITATIVE_SIZE\n",
                 inode->i_ino, whence);
        /* 通过 powerfs_net_getattr 从 Filer 拉权威 size；其他输出参数全传 NULL。
         * timeout_ms 默认传 -1 (infinite)，内部 RPC 有 timeout。 */
        ret = powerfs_net_getattr(inode->i_ino,
                                  NULL, NULL, NULL,   /* mode, uid, gid */
                                  &remote_size,       /* size ← authoritative */
                                  NULL,               /* nlink */
                                  NULL, NULL, NULL,   /* mtime, atime, ctime */
                                  NULL, NULL, NULL,   /* volume_id, file_key, layout */
                                  NULL, NULL, NULL,   /* rbytes, rfiles, rsubdirs */
                                  NULL, NULL);        /* rctime */
        if (ret < 0) {
            pr_warn("powerfs: llseek ino=%lu getattr err=%d, fallback to generic\n",
                    inode->i_ino, ret);
            /* 远端不通：回退到 generic，避免应用死锁。若 local 也 stale 则
             * 至少应用看到 ENXIO，不会在错误 offset 上写。 */
        } else {
            /* 用权威 size 原子更新本地 i_size（对齐 powerfs_inode_set_size，
             * 但这里只有 size 更新，直接拿 inode->i_lock spinlock）。 */
            spin_lock(&inode->i_lock);
            if ((loff_t)remote_size != i_size_read(inode)) {
                i_size_write(inode, (loff_t)remote_size);
                pr_debug("powerfs: llseek ino=%lu i_size updated local→remote %lld→%llu\n",
                         inode->i_ino, i_size_read(inode), remote_size);
            }
            spin_unlock(&inode->i_lock);
        }
    }
    return generic_file_llseek(file, offset, whence);
}

/*
 * powerfs_fsync - 数据同步操作
 *
 * Flat/Stripe: file_write_and_wait_range 触发 writepage→powerfs_net_write 将脏页刷到
 *   volume server, 然后调用 powerfs_net_setattr 同步 i_size.
 * Inline (K2-5): 脏页已在 write_end 同步到 inline_data, fsync 时若 inline_dirty
 *   则通过 UPDATE_INODE 将 inline_data 提交到 Filer (数据+元数据 Raft 强一致).
 *
 * 断连时 writepage 和 setattr/update 返回 -ENOTCONN, fsync 传播错误.
 */
static int powerfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = 0;
    int ret, cap_err;
    loff_t i_size;

    pr_debug("powerfs: fsync ino=%lu start=%llu end=%llu datasync=%d i_size=%lld\n",
            inode->i_ino, start, end, datasync, i_size_read(inode));

    /* 对齐  fsync 前置: 拿 FILE_WR + AUTH_EXCL 引用, flush dirty_caps.
     * 写回脏页前先确保服务端知道我们有脏态 (revoke 阻塞到 flush ACK). */
    cap_err = powerfs_get_caps(inode, file,
                               POWERFS_CAP_FILE_WR,
                               POWERFS_CAP_WR_DATA | POWERFS_CAP_AUTH_EXCL,
                               end, &got);
    if (cap_err < 0) {
        pr_debug("powerfs: fsync get_caps WR ino=%lu ret=%d, sync anyway\n",
                 inode->i_ino, cap_err);
        got = 0;
    }
    /* 有 dirty_caps 时先触发 cap_flush — 把 dirty→flushing 状态机推前,
     * 服务端 revoke 可感知到 flush-on-progress 避免超时硬踢. */
    if (got & POWERFS_CAP_WR_DATA)
        (void)powerfs_cap_flush(pi, POWERFS_CAP_ANY_DIRTY);

    /* 触发脏页写回 (Flat: writepage→powerfs_net_write; Inline: 仅清脏标) */
    ret = file_write_and_wait_range(file, start, end);
    if (ret < 0) {
        pr_warn("powerfs: fsync write_and_wait error: %d\n", ret);
        goto out_put;
    }

    /* K2-5: Inline 模式 — 通过 UPDATE_INODE 同步 inline_data 到 Filer.
     * 复用 release 路径逻辑: 快照 inline_data → 锁外网络 I/O → 清 dirty.
     * fsync 不做重试 (由调用方决定是否重试), 单次同步失败返回错误. */
    if (pi->placement == POWERFS_PLACEMENT_INLINE && pi->inline_dirty) {
        u8 *snap_data;
        u32 snap_len;
        u64 shard_id;

        spin_lock(&pi->i_lock);
        if (!pi->inline_data || pi->inline_len == 0) {
            pi->inline_dirty = false;
            spin_unlock(&pi->i_lock);
            goto out_put;
        }
        snap_len = pi->inline_len;
        spin_unlock(&pi->i_lock);

        snap_data = kmalloc(snap_len, GFP_KERNEL);
        if (!snap_data) {
            ret = -ENOMEM;
            goto out_put;
        }

        spin_lock(&pi->i_lock);
        if (pi->inline_data && pi->inline_len == snap_len) {
            memcpy(snap_data, pi->inline_data, snap_len);
        } else {
            spin_unlock(&pi->i_lock);
            kfree(snap_data);
            goto out_put;
        }
        spin_unlock(&pi->i_lock);

        shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : inode->i_ino);
        ret = powerfs_net_update_inode_size_chunks(shard_id, inode->i_ino,
                                                    (__u64)snap_len,
                                                    "kernel",
                                                    NULL, 0,
                                                    snap_data, snap_len);
        kfree(snap_data);
        if (ret < 0) {
            pr_warn("powerfs: fsync INLINE ino=%lu update failed: %d\n",
                    inode->i_ino, ret);
            goto out_put;
        }
        /* 成功则清 dirty */
        spin_lock(&pi->i_lock);
        pi->inline_dirty = false;
        spin_unlock(&pi->i_lock);
        goto out_put;
    }

    /* Flat/Stripe: 同步 i_size 到 Filer */
    i_size = i_size_read(inode);
    pr_debug("powerfs: fsync after writeback i_size=%lld\n", i_size);
    if (i_size > 0) {
        int sret = powerfs_net_setattr(inode->i_ino, POWERFS_ATTR_SIZE,
                                        0, 0, 0, (__u64)i_size, 0, 0);
        if (sret < 0) {
            pr_warn("powerfs: fsync setattr size=%llu ino=%lu failed: %d\n",
                    (u64)i_size, inode->i_ino, sret);
            ret = sret;
            goto out_put;
        }
    }

    ret = 0;
out_put:
    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}

/*
 * Phase 2: 流控准入 wrapper — 在 file_operations 层排队等待.
 *
 * 不能在 address_space_operations (write_begin/read_folio/writepages) 里 wait:
 *   - write_begin 持有 inode->i_rwsem 写锁
 *   - read_folio 持有 folio lock
 *   - writepages 持有 folio lock (writeback 线程内)
 *
 * file_operations 层 (read_iter/write_iter) 不持有 page lock,
 * generic_file_write_iter 内部才获取 i_rwsem, 此时 admit_wait 已返回.
 */
static ssize_t powerfs_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file->f_inode;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = 0;
    int err;
    ssize_t ret;
    ktime_t __metric_start = ktime_get();

    powerfs_flow_admit_wait(POWERFS_FLOW_OP_READ, 2000);

    /* 对齐  filemap_fault (addr.c L1982):
     *   need = FILE_SHARED (读必须位), want = FILE_SHARED|FILE_CACHE
     * 拿到 cap 保证 page cache 一致性视图, 服务端可 recall 撤销. */
    err = powerfs_get_caps(inode, file,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_RDCACHE,
                           -1, &got);
    if (err < 0) {
        /* 降级: 无 cap 仍允许读 (与 write_iter lease 策略一致),
         * 服务端 recall 时通过 page_mkwrite 路径再校验. */
        pr_debug("powerfs: read_iter get_caps RD ino=%lu ret=%d, downgrade\n",
                 inode->i_ino, err);
        got = 0;
    }

    ret = generic_file_read_iter(iocb, to);

    if (got)
        powerfs_cap_put_refs(pi, got);

    if (POWERFS_SB_INFO(inode->i_sb)->client)
        powerfs_update_read_metrics(&POWERFS_SB_INFO(inode->i_sb)->client->metrics,
                                    __metric_start, ktime_get(),
                                    ret > 0 ? (unsigned int)ret : 0, (int)ret);
    return ret;
}

/*
 * powerfs_splice_read - splice_read 带 cap ref 管理.
 *
 * 对齐  xxx_splice_read (file.c L2259):
 *   1. 拿 FILE_SHARED (need) + FILE_CACHE (want) cap refs
 *   2. 有 CACHE cap → filemap_splice_read (零拷贝 page cache → pipe)
 *   3. 无 CACHE cap → copy_splice_read (降级: 用户态拷贝)
 *   4. 放 cap refs
 *
 * 之前直接注册 filemap_splice_read 不持 cap ref，splice 期间 cap recall
 * 可能回收 page cache 导致数据不一致。
 */
static ssize_t powerfs_splice_read(struct file *in, loff_t *ppos,
                                    struct pipe_inode_info *pipe,
                                    size_t len, unsigned int flags)
{
    struct inode *inode = file_inode(in);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = 0;
    int err;
    ssize_t ret;

    /* umount 期间跳过网络同步 */
    if (powerfs_net_is_stopping())
        return -ENOTCONN;

    /* 拿 cap refs: need=FILE_SHARED (读必须), want=FILE_CACHE (page cache 信任) */
    err = powerfs_get_caps(inode, in,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_FILE_CACHE,
                           -1, &got);
    if (err < 0) {
        pr_debug("powerfs: splice_read get_caps ino=%lu ret=%d, fallback copy\n",
                 inode->i_ino, err);
        got = 0;
    }

    /* 有 CACHE cap → filemap_splice_read (零拷贝);
     * 无 CACHE cap → copy_splice_read (用户态拷贝降级) */
    if (got & POWERFS_CAP_FILE_CACHE)
        ret = filemap_splice_read(in, ppos, pipe, len, flags);
    else
        ret = copy_splice_read(in, ppos, pipe, len, flags);

    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}

static ssize_t powerfs_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct inode *inode = file->f_inode;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t offset;
    size_t count = iov_iter_count(from);
    unsigned int got = 0;
    int err;
    ssize_t ret;
    ktime_t __metric_start = ktime_get();

    powerfs_flow_admit_wait(POWERFS_FLOW_OP_WRITE, 2000);

    /* § O_APPEND 语义: 每次 write syscall 写入 offset 强制 = i_size.
     * VFS generic_file_write_iter 不主动处理 O_APPEND, 必须由 FS
     * 在 ->write_iter 入口把 iocb->ki_pos 重置到 i_size (对齐
     * ext4_file_write_iter / NFS write_iter 开头).
     * Note: 即使 ->open 中已经设置了 f_pos=i_size, 多进程并行
     * 写时别的 writer 先 extend 了 i_size, 本进程必须再次
     * "read i_size then write", 否则覆盖后写入的数据. */
    if (unlikely(file->f_flags & O_APPEND)) {
        loff_t isz;
        spin_lock(&pi->i_lock);
        isz = i_size_read(inode);
        spin_unlock(&pi->i_lock);
        iocb->ki_pos = isz;
        file->f_pos = isz;
        pr_debug("powerfs: write_iter ino=%lu O_APPEND -> ki_pos=f_pos=%lld\n",
                 inode->i_ino, (long long)isz);
    }
    offset = iocb->ki_pos;

    /* 对齐  page_mkwrite (addr.c L2087):
     *   need = FILE_WR, want = FILE_WR|FILE_EXCL (BUFFER).
     * write_begin 内部还会 try_get_caps 作为快速路径, 这里在 i_rwsem 外
     * 提前阻塞获取, 避免 write_begin 内循环 EAGAIN 重试开销. */
    if (S_ISREG(inode->i_mode) && count > 0) {
        err = powerfs_get_caps(inode, file,
                               POWERFS_CAP_FILE_WR,
                               POWERFS_CAP_WR_DATA,
                               offset + (loff_t)count, &got);
        if (err < 0) {
            pr_debug("powerfs: write_iter get_caps WR ino=%lu ret=%d, write_begin will retry\n",
                     inode->i_ino, err);
            got = 0;
        }
    }

    /* 预先获取 lease (同步网络调用, 用户进程上下文可阻塞). */
    if (pi->placement != POWERFS_PLACEMENT_INLINE &&
        S_ISREG(inode->i_mode) && count > 0) {
        int lease_ret = ensure_lease(inode, offset);
        if (lease_ret && lease_ret != -ENOMEM)
            pr_debug("powerfs: write_iter ensure_lease ino=%lu off=%lld ret=%d, continuing without lease\n",
                     inode->i_ino, offset, lease_ret);
    }
    ret = generic_file_write_iter(iocb, from);

    /* 写入成功后标记 cap WR dirty (对齐 __xxx_mark_dirty_caps 在 write_end),
     * 供 revoke/flush 时感知有脏数据需要同步回服务端. */
    if (ret > 0)
        powerfs_cap_mark_dirty(pi, POWERFS_CAP_WR_DATA);

    if (got)
        powerfs_cap_put_refs(pi, got);

    if (POWERFS_SB_INFO(inode->i_sb)->client)
        powerfs_update_write_metrics(&POWERFS_SB_INFO(inode->i_sb)->client->metrics,
                                     __metric_start, ktime_get(),
                                     ret > 0 ? (unsigned int)ret : 0, (int)ret);
    return ret;
}

/*
 * powerfs_file_release - 文件关闭 (最后一个 fd 释放时调用)
 *
 * K2-5: Inline 模式下, 若 inline_dirty 为 true, 通过 UPDATE_INODE 将
 * inline_data 提交到 Filer (单次 Raft 提交 = 数据 + 元数据).
 *
 * 对齐 FUSE release inline 路径 (fuse.rs L3988):
 *   - shard_id = shard_map_route(parent_ino) (Filer 路由, 对齐 FUSE ShardMap)
 *   - chunks 为空 (Inline 不走 Volume Server)
 *   - size = inline_len
 *   - 5 次重试, 500ms×attempt 退避 (覆盖 Raft 选举窗口)
 *
 * 并发保护: 持 i_lock 快照 inline_data 指针/长度, 锁外做网络 I/O.
 * 网络成功后持锁清 inline_dirty.
 *
 * 注意: VFS 忽略 release 返回值, 故即使同步失败也返回 0 (仅告警).
 *       数据丢失风险由 fsck 兜底 (与 FUSE 一致).
 *
 * 局限: mmap 写入的脏页未同步到 inline_data (writepages 对 Inline 仅清脏标),
 *       当前仅支持 write_iter 路径的 Inline 持久化.
 */
static int powerfs_file_release(struct inode *inode, struct file *file)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);
    u8 *snap_data = NULL;
    u32 snap_len = 0;
    u64 shard_id;
    u64 ino = inode->i_ino;
    int attempt;
    int ret = 0;
    bool synced = false;

    /* P3-5: 统计 opened_files (与 file_open 对称) */
    if (sbi && sbi->client)
        atomic64_dec(&sbi->client->metrics.opened_files);

    /* Cap 引用释放 (与 powerfs_file_open 对称).
     * 对齐  __xxx_put_cap_refs: 递减 i_nr_by_mode + cap_put_refs.
     * 必须在 release 逻辑前执行, 避免 flush 脏数据时 cap 已失效. */
    {
        unsigned int had = POWERFS_CAP_PIN;

        spin_lock(&pi->i_lock);
        if (file->f_mode & FMODE_READ) {
            if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] > 0)
                pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]--;
            had |= POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE;
        }
        if (file->f_mode & FMODE_WRITE) {
            if (pi->i_nr_by_mode[POWERFS_FILE_MODE_WR] > 0)
                pi->i_nr_by_mode[POWERFS_FILE_MODE_WR]--;
            had |= POWERFS_CAP_FILE_WR;
        }
        spin_unlock(&pi->i_lock);

        powerfs_cap_put_refs(pi, had);
    }

    /* §13.4 场景 3: 主动 release cap → 发送 CapRelease RPC.
     * 必须在 cap_put_refs 之后执行 (确保本 filp 的 refcount 已释放),
     * 但又要早于 inline/chunks sync (因为 Filer 端 release 会释放对该
     * 客户端 cap 的持有, inline sync 仍是普通 update_inode RPC, 不依赖 cap).
     * 若 HasUpgrade=1 (自己是 survivor), cap_send_release 内部会调
     * cap_issue 升级 issued 位 (从 SHARED_WRITE 恢复到 EXCLUSIVE_WRITE). */
    {
        struct powerfs_cap *auth_cap;
        spin_lock(&pi->i_lock);
        auth_cap = pi->i_auth_cap;
        spin_unlock(&pi->i_lock);
        if (auth_cap) {
            /* last release (所有 RD/WR refcount 归零) 时才发 release RPC.
             * 避免多 fd 并发 open/close 时每次 close 都触发一次 RPC 风暴.
             * i_nr_by_mode[RD]+i_nr_by_mode[WR] == 0 → 这是最后一个 fd. */
            spin_lock(&pi->i_lock);
            if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] == 0 &&
                pi->i_nr_by_mode[POWERFS_FILE_MODE_WR] == 0) {
                spin_unlock(&pi->i_lock);
                cap_send_release(pi, auth_cap);
            } else {
                spin_unlock(&pi->i_lock);
            }
        }
    }

    /* Inline 模式 + dirty: 同步 inline_data 到 Filer (K2-5).
     * Flat 模式: 同步 size+chunks 到 Filer (对齐 FUSE sync_size_chunks_on_close).
     *   不同步的话 Filer 端 chunks 为空, remount/lookup 后 read locate 失败 (-EINVAL). */
    if (pi->placement == POWERFS_PLACEMENT_INLINE && pi->inline_dirty) {
    pr_info("powerfs: RELEASE INLINE ino=%lu dirty=%d inline_len=%u\n",
            ino, pi->inline_dirty ? 1 : 0, pi->inline_len);

    /* 持锁快照 inline_data (网络 I/O 不能持 spinlock) */
    spin_lock(&pi->i_lock);
    if (!pi->inline_data || pi->inline_len == 0) {
        /* dirty 标记但无数据 — 异常状态, 清标志避免反复重试 */
        pi->inline_dirty = false;
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: RELEASE INLINE ino=%lu dirty but no inline_data\n", ino);
        return 0;
    }
    snap_len = pi->inline_len;
    spin_unlock(&pi->i_lock);

    /* 锁外分配快照缓冲并拷贝 */
    snap_data = kmalloc(snap_len, GFP_KERNEL);
    if (!snap_data) {
        pr_warn("powerfs: RELEASE INLINE ino=%lu kmalloc %u failed, data may be lost\n",
                ino, snap_len);
        return 0;
    }
    spin_lock(&pi->i_lock);
    if (pi->inline_data && pi->inline_len == snap_len) {
        memcpy(snap_data, pi->inline_data, snap_len);
    } else {
        /* 并发修改了 inline_data, 放弃本次同步 */
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: RELEASE INLINE ino=%lu inline_data changed during snapshot\n", ino);
        kfree(snap_data);
        return 0;
    }
    spin_unlock(&pi->i_lock);

    /* K2: 调试 — 输出 snap_data 的 checksum, 与 issue_read 的 csum 比较 */
    {
        __u32 i, csum = 0;
        for (i = 0; i < snap_len; i++)
            csum += snap_data[i];
        pr_info("powerfs: RELEASE INLINE ino=%lu snap_len=%u csum=%u first8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                ino, snap_len, csum,
                snap_len > 0 ? snap_data[0] : 0, snap_len > 1 ? snap_data[1] : 0,
                snap_len > 2 ? snap_data[2] : 0, snap_len > 3 ? snap_data[3] : 0,
                snap_len > 4 ? snap_data[4] : 0, snap_len > 5 ? snap_data[5] : 0,
                snap_len > 6 ? snap_data[6] : 0, snap_len > 7 ? snap_data[7] : 0);
    }

    shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);

    /* 5 次重试, 500ms×attempt 退避 (对齐 FUSE, 覆盖 Raft 选举窗口 ~3s) */
    for (attempt = 1; attempt <= 5; attempt++) {
        ret = powerfs_net_update_inode_size_chunks(shard_id, ino,
                                                    (__u64)snap_len,
                                                    "kernel",
                                                    NULL, 0,
                                                    snap_data, snap_len);
        if (ret == 0) {
            synced = true;
            pr_info("powerfs: RELEASE INLINE ino=%lu synced size=%u (attempt %d)\n",
                     ino, snap_len, attempt);
            break;
        }
        pr_warn("powerfs: RELEASE INLINE ino=%lu attempt %d failed: %d\n",
                ino, attempt, ret);
        if (attempt < 5)
            msleep(500 * attempt);
    }

    /* 成功则清 dirty; 失败保留 dirty (evict_inode 时 kfree, 数据丢失) */
    if (synced) {
        spin_lock(&pi->i_lock);
        pi->inline_dirty = false;
        spin_unlock(&pi->i_lock);
    } else {
        pr_err("powerfs: RELEASE INLINE ino=%lu FAILED after 5 attempts: %d — data may be lost\n",
               ino, ret);
    }

    kfree(snap_data);
    return 0;
    } /* end Inline release */

    /* Flat 模式: close 时同步 size+chunks 到 Filer.
     * 对齐 FUSE sync_size_chunks_on_close (fuse.rs L990).
     * writeback 只刷数据到 Volume Server, 不同步 chunks 到 Filer.
     * 若不同步, Filer 端 chunks 为空, remount/lookup 后 read locate 失败.
     *
     * 性能修复: close 时先 flush 脏页到 Volume Server (filemap_write_and_wait_range),
     * 确保数据在 lease 有效期内写入. 否则 lease 过期后 writeback 无 lease 写入,
     * 每个 writepage 需 5s (实测 780MB → 65min), 严重影响 sync 性能.
     * file_release 在用户进程上下文调用, 可安全阻塞. */
    if (pi->placement == POWERFS_PLACEMENT_FLAT && pi->volume_id && pi->file_key) {
        loff_t i_size = i_size_read(inode);
        u64 shard_id;
        u32 chunk_size = pi->layout_chunk_size ? pi->layout_chunk_size : POWERFS_CHUNK_SIZE;
        u32 chunk_count;
        struct powerfs_chunk_map *chunks = NULL;
        u32 i;
        int flush_ret;

        if (i_size == 0) {
            pr_debug("powerfs: RELEASE FLAT ino=%lu size=0, skip\n", ino);
            return 0;
        }

        /* Flush 脏页到 Volume Server (在 lease 过期前完成数据写入).
         * ensure_lease 刷新 stripe 0 的 lease (快速路径: 已持有则立即返回).
         * filemap_write_and_wait_range 触发 writeback, 此时 lease 仍有效,
         * powerfs_get_lease_token 能找到 lease token, 写入快速完成. */
        ensure_lease(inode, 0);
        pr_info("powerfs: RELEASE FLAT ino=%lu pre-flush i_size=%llu\n", ino, (u64)i_size);
        flush_ret = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
        if (flush_ret)
            pr_warn("powerfs: RELEASE FLAT ino=%lu filemap_write_and_wait_range: %d\n",
                    ino, flush_ret);

        /* 仅在 i_size 变化时同步 (用 content_size 跟踪上次同步值) */
        spin_lock(&pi->i_lock);
        if ((u64)i_size == pi->content_size) {
            spin_unlock(&pi->i_lock);
            pr_debug("powerfs: RELEASE FLAT ino=%lu size=%llu unchanged, skip\n",
                     ino, (u64)i_size);
            return 0;
        }
        spin_unlock(&pi->i_lock);

        chunk_count = (u32)div_u64(i_size + chunk_size - 1, chunk_size);
        /* 限制最大 chunk_count 避免过大分配 (4096 chunks = 4GB @ 1MB chunk) */
        if (chunk_count > 4096) {
            pr_warn("powerfs: RELEASE FLAT ino=%lu chunk_count=%u > 4096, truncating\n",
                    ino, chunk_count);
            chunk_count = 4096;
        }

        chunks = kmalloc_array(chunk_count, sizeof(*chunks), GFP_KERNEL);
        if (!chunks) {
            pr_warn("powerfs: RELEASE FLAT ino=%lu kmalloc %u chunks failed, metadata not synced\n",
                    ino, chunk_count);
            return 0;
        }

        for (i = 0; i < chunk_count; i++) {
            u64 chunk_off = (u64)i * chunk_size;
            chunks[i].chunk_idx = i;
            chunks[i].needle_id = pi->file_key + i;
            chunks[i].volume_id = pi->volume_id;
            chunks[i].crc32 = 0;
            /* 最后一个 chunk 可能是非整数块大小; 其余为满块.
             * FUSE 端用 chunk.size 判断有效数据长度, size=0 会被视为 hole
             * 并填充 0 (参见 fuse.rs chunk_size_map 逻辑). */
            if (i == chunk_count - 1) {
                chunks[i].size = (u64)i_size - chunk_off;
            } else {
                chunks[i].size = chunk_size;
            }
        }

        shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);

        pr_info("powerfs: RELEASE FLAT ino=%lu size=%llu chunks=%u vid=%llu fkey=%llu\n",
                ino, (u64)i_size, chunk_count,
                (unsigned long long)pi->volume_id,
                (unsigned long long)pi->file_key);

        for (attempt = 1; attempt <= 5; attempt++) {
            ret = powerfs_net_update_inode_size_chunks(shard_id, ino,
                                                        (__u64)i_size,
                                                        "kernel",
                                                        chunks, chunk_count,
                                                        NULL, 0);
            if (ret == 0) {
                spin_lock(&pi->i_lock);
                pi->content_size = (u64)i_size;
                spin_unlock(&pi->i_lock);
                pr_info("powerfs: RELEASE FLAT ino=%lu synced size=%llu chunks=%u (attempt %d)\n",
                        ino, (u64)i_size, chunk_count, attempt);
                break;
            }
            pr_warn("powerfs: RELEASE FLAT ino=%lu attempt %d failed: %d\n",
                    ino, attempt, ret);
            if (attempt < 5)
                msleep(500 * attempt);
        }

        kfree(chunks);
        return 0;
    }

    pr_debug("powerfs: RELEASE skip ino=%lu placement=%u dirty=%d\n",
             ino, pi->placement, pi->inline_dirty ? 1 : 0);
    return 0;
}

/* Forward declaration — defined below */
static long powerfs_fallocate(struct file *file, int mode,
                              loff_t offset, loff_t len);

/*
 * powerfs_file_open - 文件打开 cap 接入
 *
 * 对齐  xxx_init_file + __xxx_touch_fmode:
 *   1. 根据 f_mode 记录 i_nr_by_mode[] (RD/WR 计数)
 *   2. 更新 i_last_rd / i_last_wr 时间戳
 *   3. 获取 cap 引用: RD → FILE_SHARED|FILE_CACHE, WR → FILE_WR
 *
 * cap 引用在 release 时通过 powerfs_cap_put_refs 对称释放.
 * 若 Filer 尚未授予足够 cap, powerfs_check_caps 会在后台请求.
 */
int powerfs_file_open(struct inode *inode, struct file *file)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    unsigned int got = POWERFS_CAP_PIN;
    bool is_write_open = !!(file->f_mode & FMODE_WRITE);

    /* §13.3: 先从 Filer 获取授权 CapOpenGrant (永不阻塞),
     * 根据响应挂载 cap + 更新 issued 位. 网络失败自动降级 (不阻断 open).
     * 注意: __block_on 在此处调, 早于 cap_get_refs 拿 refcount, 避免
     * refcount 先占有后授权位不足导致 try_get_caps 循环等. */
    cap_open_grant_and_issue(pi, is_write_open);

    spin_lock(&pi->i_lock);

    if (file->f_mode & FMODE_READ) {
        pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]++;
        pi->i_last_rd = jiffies;
        got |= POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE;
    }
    if (file->f_mode & FMODE_WRITE) {
        pi->i_nr_by_mode[POWERFS_FILE_MODE_WR]++;
        pi->i_last_wr = jiffies;
        got |= POWERFS_CAP_FILE_WR;
    }

    spin_unlock(&pi->i_lock);

    /* 获取 cap 引用 (内部自行加锁) */
    powerfs_cap_get_refs(pi, got);

    /* P3-5: 统计 opened_files */
    {
        struct powerfs_sb_info *sbi = POWERFS_SB_INFO(inode->i_sb);
        if (sbi && sbi->client)
            atomic64_inc(&sbi->client->metrics.opened_files);
    }

    pr_debug("powerfs: file_open ino=%lu mode=0x%x got=0x%x write=%d\n",
             inode->i_ino, file->f_mode, got, (int)is_write_open);

    /* § O_APPEND 语义: 每次 write 先 lseek 到 i_size. VFS 不自动
     * 设置 f_pos, 必须由具体 FS 在 ->open 中手动把 file->f_pos
     * 更新到 i_size, 否则后续 write_iter 由 caller 传的 offset
     * (通常 file->f_pos=0 或旧 pos) 覆盖之前的内容, 造成
     * "append = overwrite" 假象.
     * 对齐 ext4_file_open() 末尾 + NFS file_open() 对
     * O_APPEND 处理: 打开成功后 f_pos = i_size. */
    if (unlikely((file->f_flags & O_APPEND) && (file->f_mode & FMODE_WRITE))) {
        spin_lock(&pi->i_lock);
        file->f_pos = i_size_read(inode);
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: file_open ino=%lu O_APPEND -> f_pos=%lld (i_size)\n",
                 inode->i_ino, (long long)file->f_pos);
    }

    return 0;
}

/* ========== vm_operations (mmap fault/page_mkwrite cap 接入) ==========
 *
 * 对齐  xxx_vmops (addr.c L2333):
 *   .fault        = xxx_filemap_fault   — 读缺页: 拿 FILE_SHARED|FILE_CACHE
 *   .page_mkwrite = xxx_page_mkwrite    — 写缺页: 拿 FILE_WR|FILE_EXCL, mark dirty
 *
 * generic_file_mmap 默认 vm_ops 不走 cap, 导致 mmap 写可以绕过
 * write_begin/page_mkwrite 的校验 (服务端无法 recall 已 mmap 的页).
 */

static vm_fault_t powerfs_filemap_fault(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct file *file = vma->vm_file;
    struct inode *inode = file_inode(file);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t off = (loff_t)vmf->pgoff << PAGE_SHIFT;
    unsigned int got = 0;
    int err;
    vm_fault_t ret;

    /* 对齐  filemap_fault (addr.c L1982):
     *   need = FILE_SHARED (读必须位), want = FILE_SHARED|FILE_CACHE.
     * endoff = -1 表示不限制范围 (整个文件读). */
    err = powerfs_get_caps(inode, file,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_RDCACHE,
                           -1, &got);
    if (err < 0) {
        pr_debug("powerfs: vm_fault get_caps RD ino=%lu off=%lld ret=%d\n",
                 inode->i_ino, off, err);
        got = 0;
    }

    ret = filemap_fault(vmf);

    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}

static vm_fault_t powerfs_page_mkwrite(struct vm_fault *vmf)
{
    struct vm_area_struct *vma = vmf->vma;
    struct file *file = vma->vm_file;
    struct inode *inode = file_inode(file);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct folio *folio = page_folio(vmf->page);
    loff_t off = folio_pos(folio);
    loff_t size = i_size_read(inode);
    loff_t endoff;
    size_t len;
    unsigned int got = 0;
    int err;
    vm_fault_t ret = VM_FAULT_SIGBUS;

    if (off + (loff_t)folio_size(folio) <= size)
        len = folio_size(folio);
    else
        len = (size_t)((long long)size - (long long)off);
    endoff = off + (loff_t)len;

    /* 对齐  page_mkwrite (addr.c L2087):
     *   need = FILE_WR, want = FILE_WR|FILE_EXCL.
     * page_mkwrite 持有 folio lock, 但 get_caps 不依赖 page lock,
     * 阻塞在 i_cap_wq (服务端 grant). */
    err = powerfs_get_caps(inode, file,
                           POWERFS_CAP_FILE_WR,
                           POWERFS_CAP_WR_DATA,
                           endoff, &got);
    if (err < 0) {
        pr_debug("powerfs: page_mkwrite get_caps WR ino=%lu off=%lld ret=%d\n",
                 inode->i_ino, off, err);
        goto out_nocaps;
    }

    /* 更新 mtime/iversion — 与 generic_permission / write_end 时间语义对齐 */
    file_update_time(file);
    inode_inc_iversion_raw(inode);

    /*  这里会 wait_on_page_writeback, 通用 filemap_fault 已处理.
     * lock_page 由上层 do_page_mkwrite 持有 (进入回调时 page 已 locked). */

    ret = VM_FAULT_LOCKED;   /* 保持 folio locked */

    /* 成功写缺页: 标记 WR_DATA dirty (写回需要同步).
     * mark_inode_dirty 由 set_page_dirty 在 writepage 前置位触发. */
    powerfs_cap_mark_dirty(pi, POWERFS_CAP_WR_DATA);

    powerfs_cap_put_refs(pi, got);
    return ret;

out_nocaps:
    if (got)
        powerfs_cap_put_refs(pi, got);
    return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct powerfs_file_vmops = {
    .fault         = powerfs_filemap_fault,
    .page_mkwrite  = powerfs_page_mkwrite,
};

/*
 * powerfs_mmap - 自定义 mmap, 注入 vm_ops (cap 一致性)
 *
 * 保留 .mmap 路径用于内核 < 6.17 兼容 (若 mmap_prepare 未注册则 VFS
 * fallback 到 .mmap). 6.17 优先走 .mmap_prepare.
 */
static int powerfs_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct address_space *mapping = file->f_mapping;
    int ret;

    if (!mapping->a_ops->read_folio)
        return -ENOEXEC;

    ret = generic_file_mmap(file, vma);
    if (ret == 0)
        vma->vm_ops = &powerfs_file_vmops;
    return ret;
}

/*
 * powerfs_mmap_prepare - P2-6: 6.17 新接口, 在 sys_mmap 时提前设置 vm_ops.
 *
 * 对齐  xxx_mmap_prepare (addr.c L2338):
 *   1. 检查 address_space 有 read_folio (正则文件)
 *   2. 设置 desc->vm_ops = &powerfs_file_vmops (fault/page_mkwrite 带 cap)
 *
 * 相比旧 .mmap 路径: mmap_prepare 在 VFS 分配 vma 前调用,
 * 可以提前拿到 cap, 避免 page fault 时才发现 cap miss 导致竞态.
 * msync(MS_INVALIDATE) 时 page_mkwrite 持有 cap ref, 不会被 cap_recall
 * 中途回收 page cache, 修复 mmap + msync 写数据丢失竞态.
 */
static int powerfs_mmap_prepare(struct vm_area_desc *desc)
{
    struct address_space *mapping = desc->file->f_mapping;

    if (!mapping->a_ops->read_folio)
        return -ENOEXEC;

    desc->vm_ops = &powerfs_file_vmops;
    return 0;
}

/*
 * powerfs_copy_file_range - P2-5: 文件间复制 (sendfile/copy_file_range syscall).
 *
 * 对齐  xxx_copy_file_range (file.c L3148):
 *   1. 同 fs 内: 尝试 splice_copy_file_range (page cache splice, 零用户态拷贝)
 *   2. 跨 fs 或不支持: fallback generic_copy_file_range (用户态 read/write)
 *
 * PowerFS 当前没有服务端 OSD offload ( RADOS copy-from)，但 splice
 * 路径已能在 page cache 层做高效复制，避免用户态往返拷贝。
 *
 * cap 管理: src 端拿 FILE_SHARED (读), dst 端拿 FILE_WR (写),
 * 复制完成后释放 refs。
 */
static ssize_t powerfs_copy_file_range(struct file *src_file, loff_t src_off,
                                        struct file *dst_file, loff_t dst_off,
                                        size_t len, unsigned int flags)
{
    struct inode *src_inode = file_inode(src_file);
    struct inode *dst_inode = file_inode(dst_file);
    struct powerfs_inode_info *src_pi = POWERFS_I(src_inode);
    struct powerfs_inode_info *dst_pi = POWERFS_I(dst_inode);
    unsigned int src_got = 0, dst_got = 0;
    ssize_t ret;

    /* flags must be 0 (no SPLICE_F_MOVE etc) */
    if (flags != 0)
        return -EINVAL;

    /* src 端: 拿读 cap */
    ret = powerfs_get_caps(src_inode, src_file,
                           POWERFS_CAP_FILE_SHARED,
                           POWERFS_CAP_FILE_CACHE,
                           -1, &src_got);
    if (ret < 0)
        src_got = 0;

    /* dst 端: 拿写 cap */
    ret = powerfs_get_caps(dst_inode, dst_file,
                           POWERFS_CAP_FILE_WR,
                           POWERFS_CAP_FILE_WR,
                           -1, &dst_got);
    if (ret < 0)
        dst_got = 0;

    /* 用 splice_copy_file_range (page cache → pipe → page cache).
     * 跨 fs 场景 splice 也能处理 (VFS splice_file_range 内部会做检查). */
    ret = splice_copy_file_range(src_file, src_off, dst_file, dst_off, len);

    if (src_got)
        powerfs_cap_put_refs(src_pi, src_got);
    if (dst_got)
        powerfs_cap_put_refs(dst_pi, dst_got);

    return ret;
}

/* ========== File Locking (POSIX record locks + BSD flock) ==========
 * 设计决策 (同  xxx_lock.c 的初版架构):
 *   先做**单机一致性锁**: 委托 VFS 通用框架 (posix_lock_file / locks_lock_inode_wait)
 *   在内存 inode 内维护锁冲突链表和阻塞队列. 多客户端场景下, 跨节点锁仲裁
 *   需 Filer 侧新增 LOCK/UNLOCK 协议帧, 路线图:
 *     Phase 1 (本地, 当前): 单机多进程/多线程正确互斥. 覆盖 90% 单机使用场景.
 *     Phase 2 (分布式): wrapper 内先本地冲突检测, 再发 Filer LOCK RPC 仲裁.
 *
 * 为什么不返回 -ENOSYS (P0 修复前的行为):
 *   大量应用 (SQLite, PostgreSQL, Maildir, Python fcntl.flock, git) 依赖
 *   fcntl(F_SETLK) / flock() == 0 的成功路径; -ENOSYS 会直接 panic 应用.
 */

/*
 * powerfs_lock — POSIX 记录锁 (fcntl(2) F_SETLK/F_SETLKW/F_GETLK/F_OFD_*).
 *
 * VFS 层传 cmd: F_SETLK (非阻塞), F_SETLKW (阻塞), F_GETLK (查询冲突锁),
 * 以及 OFD 版本 (F_OFD_SETLK etc). struct file_lock *fl 含锁类型/范围/owner.
 * 直接用 VFS 通用 posix_lock_file 实现, 内核已维护冲突检测 + 文件锁阻塞队列
 * (fl->fl_blocked 链表 + wake_up). 此函数 EXPORT_SYMBOL.
 */
int powerfs_lock(struct file *filp, int cmd, struct file_lock *fl)
{
    struct inode *inode = file_inode(filp);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int err;

    if (unlikely(!pi || pi->shutdown))
        return -ENOTCONN;

    pr_debug("powerfs: lock ino=%lu cmd=%d type=%d pid=%d\n",
             inode->i_ino, cmd, fl->c.flc_type, current->pid);

    /* P0 单机 VFS 通用锁表:
     *   posix_lock_file(file, fl, conflock) ——
     *     [in] file: 目标文件
     *     [in] fl:   新锁请求 (类型/范围/owner)
     *     [out] conflock: GETLK 场景返回首个冲突锁; SET/UNLOCK 传 NULL
     *   内核内部已实现冲突检测、阻塞等待 (SETLKW)、信号唤醒.
     *   此函数 EXPORT_SYMBOL (fs/locks.c:1404). */
    err = posix_lock_file(filp, fl, NULL);

    if (err < 0)
        pr_debug_ratelimited("powerfs: lock ino=%lu failed: %d\n",
                             inode->i_ino, err);
    return err;
}

/*
 * powerfs_flock — BSD 风格 flock(2).
 *
 * VFS 把 LOCK_SH/LOCK_EX/LOCK_UN (+ LOCK_NB) 转换成 file_lock 结构 (fl_type
 * F_RDLCK/F_WRLCK/F_UNLCK, fl_flags |= FL_FLOCK 标记) 后传入.
 * 通用入口 locks_lock_inode_wait 内部按 FL_FLOCK 分派到 flock_lock_inode_wait
 * 处理冲突检测 + 等待. 此函数 EXPORT_SYMBOL.
 */
int powerfs_flock(struct file *filp, int cmd, struct file_lock *fl)
{
    struct inode *inode = file_inode(filp);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int err;

    if (unlikely(!pi || pi->shutdown))
        return -ENOTCONN;

    /* flock 锁的 fl_flags 需要 FL_FLOCK: VFS 传进来时通常已设置, 但为了
     * 兜底 (lockf 共享路径) 我们显式标记. */
    fl->c.flc_flags |= FL_FLOCK;

    pr_debug("powerfs: flock ino=%lu cmd=%d type=%d pid=%d\n",
             inode->i_ino, cmd, fl->c.flc_type, current->pid);

    err = locks_lock_inode_wait(inode, fl);
    if (err < 0)
        pr_debug_ratelimited("powerfs: flock ino=%lu failed: %d\n",
                             inode->i_ino, err);
    return err;
}

/*
 * powerfs_dir_fsync — 目录 fsync(2).
 *
 * 目录没有数据页, 只需同步元数据 (mode/uid/gid/mtime/ctime/size 等).
 * - write_inode_now: 调 super_ops->write_inode (powerfs_write_inode),
 *   后者最终发 powerfs_net_setattr 推送属性.
 * - cap_flush: 若目录级 XATTR/权限 dirty, 也在此路径推 Filer.
 *
 * 数据库场景 (e.g. SQLite commit) 在事务后通常 fsync(data file) + fsync(dir fd)
 * 确保 rename/create 落盘. 若目录 fsync 为空, crash+恢复后目录条目丢失.
 */
int powerfs_dir_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int err, ret;

    (void)start; (void)end; (void)datasync;
    if (unlikely(!pi || pi->shutdown))
        return -ENOTCONN;

    pr_debug("powerfs: dir_fsync ino=%lu\n", inode->i_ino);

    /* Step 1: 同步 inode 属性 (mode/mtime etc.) 到后端 (super write_inode).
     * 第二参数 wait=1: 阻塞直到 write_inode 完成 (真正发 net_setattr). */
    err = write_inode_now(inode, 1);

    /* Step 2: 若 i_dirty_caps 有位 (XATTR/AUTH 脏), 同步 cap_flush.
     * 目录一般没 WR_DATA, 但 setfacl/chmod/chown 后 AUTH_EXCL/XATTR_EXCL 会脏. */
    ret = 0;
    if (pi->i_dirty_caps)
        ret = powerfs_cap_flush(pi, pi->i_dirty_caps);

    return err ?: ret;
}
/*
 * powerfs_ioctl — P1-3: 基础 ioctl 实现 (替代旧占位).
 *
 * 策略 (Linux 6.17 new fileattr 子系统):
 *   FS_IOC_GETFLAGS/SETFLAGS/FSGETXATTR/FSSETXATTR 直接委托内核通用函数
 *   ioctl_getflags / ioctl_setflags / ioctl_fsgetxattr / ioctl_fssetxattr,
 *   它们会 vfs_fileattr_get/set → 我们的 fileattr_get/set 回调.
 *   FITRIM 仍手动处理 (PowerFS 无块设备 discard).
 *
 * 通用函数返回 -ENOIOCTLCMD 时降级为 -ENOTTY (用户 ENOTTY).
 */
long powerfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct inode *inode = file_inode(file);
    void __user *argp = (void __user *)arg;
    long ret = -ENOTTY;

    pr_debug("powerfs: ioctl cmd=0x%x ino=%lu\n", cmd, inode->i_ino);

    switch (cmd) {
    /* ================================================================
     * 1. FS_IOC_GETFLAGS / SETFLAGS — chattr(1) / lsattr(1) 走 fileattr
     * ================================================================ */
    case FS_IOC_GETFLAGS:
        ret = ioctl_getflags(file, (unsigned int __user *)argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    case FS_IOC_SETFLAGS:
        ret = ioctl_setflags(file, (unsigned int __user *)argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    /* ================================================================
     * 2. FS_IOC_FSGETXATTR / FSSETXATTR — chattr 2.0 扩展属性
     * ================================================================ */
    case FS_IOC_FSGETXATTR:
        ret = ioctl_fsgetxattr(file, argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    case FS_IOC_FSSETXATTR:
        ret = ioctl_fssetxattr(file, argp);
        if (ret == -ENOIOCTLCMD)
            ret = -ENOTTY;
        break;

    /* ================================================================
     * 3. FITRIM — fstrim -av 空间回收
     *
     * PowerFS 是对象存储架构 (Volume → Needles)，无底层块设备 discard，
     * 返回 -EOPNOTSUPP 让 fstrim 跳过而非报错. 若未来实现 Needle GC，
     * 可在此触发并返回已回收字节数.
     * ================================================================ */
    case FITRIM: {
        struct super_block *sb = inode->i_sb;
        struct fstrim_range range;

        if (!capable(CAP_SYS_ADMIN))
            return -EPERM;
        if (copy_from_user(&range, argp, sizeof(range)))
            return -EFAULT;
        if (range.len < sb->s_blocksize)
            return -EINVAL;
        pr_info_once("powerfs: FITRIM not supported (object storage, no block discard)\n");
        return -EOPNOTSUPP;
    }

    default:
        ret = -ENOTTY;
        break;
    }

    return ret;
}

/*
 * 文件操作表 - 尽可能复用 VFS 通用实现
 *
 * 参考 ramfs_file_operations (fs/ramfs/file-mmu.c)
 */
const struct file_operations powerfs_file_operations = {
    .open           = powerfs_file_open,
    .read_iter      = powerfs_file_read_iter,
    .write_iter     = powerfs_file_write_iter,
    .mmap           = powerfs_mmap,          /* < 6.17 兼容路径 */
    .mmap_prepare   = powerfs_mmap_prepare,  /* P2-6: 6.17 提前设 vm_ops, 修复 msync 竞态 */
    .release        = powerfs_file_release,
    .flush          = powerfs_flush,         /* P0-1: close(2) 前每次调用(含 dup/管道)都 flush+sync size
                                               * 对齐 FUSE fn flush L9006，修复 tee/pipe MD5 mismatch */
    .fsync          = powerfs_fsync,
    .fallocate      = powerfs_fallocate,
    .lock           = powerfs_lock,       /* P0-1 fix: POSIX record locks (fcntl) */
    .flock          = powerfs_flock,      /* P0-2 fix: BSD flock(2) */
    .unlocked_ioctl = powerfs_ioctl,      /* P1-3 扩展实现 */
    .compat_ioctl   = compat_ptr_ioctl,   /* P1-3 32-bit user on 64-bit kernel */
    .splice_read    = powerfs_splice_read,  /* cap ref 管理 + CACHE/copy 降级 */
    .splice_write   = iter_file_splice_write,
    .llseek         = powerfs_llseek,        /* P0-2: SEEK_END/DATA/HOLE 先 RPC 拉 Filer 权威 size
                                               * 对齐 ceph_llseek，避免 stale i_size seek 后 write 错 offset */
    .setlease       = simple_nosetlease,  /* P2-4: 明确拒绝 F_SETLEASE delegations，
                                           * 防止远端写时不触发本地 break，导致 rsync/WAL silent stale */
    .copy_file_range = powerfs_copy_file_range,  /* P2-5: 同 fs splice + 跨 fs generic fallback */
};

/* ========== fallocate 回调 (空间预分配) ==========
 *
 * PowerFS 不预分配物理块 (空间按需分配), fallocate 仅更新 i_size:
 * - mode 0 (默认): 扩展 i_size 到 offset+len, 页面全零
 * - FALLOC_FL_KEEP_SIZE: 不改变 i_size (no-op)
 * - FALLOC_FL_PUNCH_HOLE: 释放页面缓存 (truncate_pagecache_range)
 *
 * 参考: brd_fallocate (ramfs), shmem_fallocate (tmpfs)
 */
static long powerfs_fallocate(struct file *file, int mode,
                              loff_t offset, loff_t len)
{
    struct inode *inode = file_inode(file);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t new_size = offset + len;
    unsigned int got = 0;
    int cap_err, ret;

    /* Only support default, KEEP_SIZE, and PUNCH_HOLE modes */
    if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
        return -EOPNOTSUPP;

    /* PUNCH_HOLE requires KEEP_SIZE */
    if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
        return -EOPNOTSUPP;

    if (offset < 0 || len <= 0)
        return -EINVAL;

    /* 对齐  fallocate: 需持有 FILE_EXCL (改 size/hole) + AUTH_EXCL (改 attrs).
     * i_rwsem 会在 inode_lock 获取, 此处先在锁外阻塞拿 cap, 避免持有锁时网络阻塞. */
    cap_err = powerfs_get_caps(inode, file,
                               POWERFS_CAP_FILE_EXCL,
                               POWERFS_CAP_FILE_EXCL | POWERFS_CAP_AUTH_EXCL,
                               new_size, &got);
    if (cap_err < 0) {
        pr_debug("powerfs: fallocate get_caps EXCL ino=%lu ret=%d, continue\n",
                 inode->i_ino, cap_err);
        got = 0;
    }

    inode_lock(inode);

    if (mode & FALLOC_FL_PUNCH_HOLE) {
        /* PUNCH_HOLE: deallocate data in [offset, new_size), reads as zeros.
         * File size is unchanged (KEEP_SIZE is required for PUNCH_HOLE).
         *
         * O-10: For FLAT files, truncate_pagecache_range alone is
         * insufficient — it only zeroes/removes pagecache pages but
         * does not update the server needle. After pagecache eviction
         * (memory pressure, posix_fadvise, refresh_work invalidation),
         * reads re-fetch the original (non-zero) data from the server.
         *
         * Fix: zero the punched region in pagecache, mark pages dirty,
         * and trigger synchronous writeback so the server needle is
         * updated with zeros. This mirrors the O-03 extend path.
         *
         * For non-FLAT files (INLINE), data lives in inline_data (not
         * the server needle), so truncate_pagecache_range is sufficient.
         */
        if (pi->placement == POWERFS_PLACEMENT_FLAT &&
            pi->volume_id && pi->file_key) {
            loff_t file_size = i_size_read(inode);
            loff_t punch_end = min(new_size, file_size);
            pgoff_t start_pg, end_pg, pg;
            int dirty_count = 0;

            if (offset >= file_size) {
                /* Hole entirely beyond file size — no-op */
                ret = 0;
                goto falloc_done;
            }

            start_pg = offset >> PAGE_SHIFT;
            end_pg = (punch_end - 1) >> PAGE_SHIFT;

            for (pg = start_pg; pg <= end_pg; pg++) {
                struct page *page;
                size_t pg_off = (size_t)pg << PAGE_SHIFT;
                size_t z_start, z_end;
                bool need_read = false;

                /* Calculate zero range within this page.
                 * z_start/z_end are offsets within the page (0..PAGE_SIZE).
                 */
                z_start = (pg_off < (size_t)offset)
                          ? (size_t)(offset - pg_off) : 0;
                z_end = (pg_off + PAGE_SIZE > (size_t)punch_end)
                        ? (size_t)(punch_end - pg_off) : PAGE_SIZE;

                /* If the page is partially outside the hole (first or
                 * last page with non-page-aligned boundaries), we need
                 * to read existing data to preserve the non-punched
                 * portion. Pages fully within the hole can be zeroed
                 * without reading from the server.
                 */
                if (z_start > 0 || z_end < PAGE_SIZE)
                    need_read = true;

                if (need_read) {
                    page = read_mapping_page(inode->i_mapping, pg, NULL);
                    if (IS_ERR(page)) {
                        /* Server read failed — create zero page */
                        page = find_or_create_page(
                            inode->i_mapping, pg, GFP_NOFS);
                        if (!page)
                            continue;
                        zero_user_segment(page, 0, PAGE_SIZE);
                        SetPageUptodate(page);
                    } else {
                        lock_page(page);
                    }
                } else {
                    /* Page is entirely within the hole — create/reuse
                     * zero page without reading from server.
                     */
                    page = find_or_create_page(
                        inode->i_mapping, pg, GFP_NOFS);
                    if (!page)
                        continue;
                    zero_user_segment(page, 0, PAGE_SIZE);
                    SetPageUptodate(page);
                }

                /* Zero the punched portion (for partial pages, this
                 * preserves data outside the hole). For full pages,
                 * z_start=0, z_end=PAGE_SIZE (already zeroed above).
                 */
                zero_user_segment(page, z_start, z_end);
                set_page_dirty(page);
                unlock_page(page);
                put_page(page);
                dirty_count++;
            }

            pr_debug("powerfs: FALLOCATE punch_hole FLAT ino=%lu offset=%llu len=%llu, zeroed %d pages\n",
                    inode->i_ino, (unsigned long long)offset,
                    (unsigned long long)len, dirty_count);

            /* Synchronous writeback to update server needle with zeros
             * in the punched region.
             */
            filemap_write_and_wait(inode->i_mapping);
        } else {
            /* Non-FLAT (INLINE): just release page cache (original
             * behavior). INLINE data is in inline_data, not the server
             * needle, so truncate_pagecache_range is sufficient.
             */
            truncate_pagecache_range(inode, offset, new_size - 1);
            if (pi->placement == POWERFS_PLACEMENT_INLINE)
                pi->inline_dirty = true;
        }
        ret = 0;
    } else if (!(mode & FALLOC_FL_KEEP_SIZE)) {
        /* Default mode: extend file size */
        loff_t old_size = i_size_read(inode);

        if (new_size > old_size) {
            i_size_write(inode, new_size);
            mark_inode_dirty(inode);

            /* O-03: For FLAT files, explicitly zero the extended region
             * [old_size, new_size) and trigger synchronous writeback so
             * the server needle is extended with zeros. Without this:
             *   - The extended region has no pagecache pages; a subsequent
             *     writeback RMW gap-fill would find no pages and produce
             *     uninitialized gap data.
             *   - The server needle retains its old size; if refresh_work
             *     later invalidates the pagecache (multi-client NOTIFY),
             *     reads of the extended region would fetch stale/short
             *     data from the server.
             * This mirrors the powerfs_setattr extend path (K2-14).
             */
            if (pi->placement == POWERFS_PLACEMENT_FLAT &&
                pi->volume_id && pi->file_key) {
                pgoff_t start_pg = old_size >> PAGE_SHIFT;
                pgoff_t end_pg = (new_size - 1) >> PAGE_SHIFT;
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
                         * Otherwise, we need valid data before old_size.
                         */
                        need_read = (off > 0);
                    }

                    if (need_read) {
                        /* Read existing page to preserve data before
                         * old_size. read_mapping_page handles pagecache
                         * lookup and server fetch.
                         */
                        page = read_mapping_page(inode->i_mapping,
                                                  pg, NULL);
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
                         * stale data).
                         */
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

                pr_debug("powerfs: FALLOCATE extend FLAT ino=%lu old=%llu new=%llu, zeroed %d pages\n",
                        inode->i_ino, (unsigned long long)old_size,
                        (unsigned long long)new_size, dirty_count);

                /* Synchronous writeback to update server needle with
                 * zeros in the extended region. Do NOT re-dirty after
                 * writeback; rely on refresh_work's K2-14 skip to keep
                 * the zeroed pages resident in pagecache.
                 */
                filemap_write_and_wait(inode->i_mapping);
            } else if (pi->placement == POWERFS_PLACEMENT_INLINE) {
                pi->inline_dirty = true;
            }
        }
        ret = 0;
    } else {
        /* KEEP_SIZE: no-op (no physical pre-allocation) */
        ret = 0;
    }

falloc_done:
    inode_unlock(inode);

    /* fallocate 成功 (punch/extend) 后, 标记 EXCL/AUTH dirty.
     * INLINE: inline_dirty 已在分支内置位, 这里额外 mark cap dirty 供 revoke 感知. */
    if (ret == 0 && !(mode & FALLOC_FL_KEEP_SIZE))
        powerfs_cap_mark_dirty(pi, POWERFS_CAP_AUTH_EXCL | POWERFS_CAP_FILE_EXCL);

    if (got)
        powerfs_cap_put_refs(pi, got);
    return ret;
}
