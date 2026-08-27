/*
 * powerfs_dir.c - split from powerfs_fs.c
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
    u64 ts_entry, ts_net, net_dur_us, total_us;

    (void)flags;

    ts_entry = ktime_get_ns();

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
        __u64 volume_id = 0, file_key = 0;
        struct powerfs_file_layout lookup_layout;
        int timeout_ms;

        /* K3: 零初始化, 确保 err/ENOENT 路径 kfree(volume_ids=NULL) 安全 */
        memset(&lookup_layout, 0, sizeof(lookup_layout));

        pr_debug("powerfs: lookup '%pd' via powerfs_net\n", dentry);

        /* Phase 1: 超时策略.
         *
         * lookup 在 VFS 持有 dir->i_rwsem 读锁时调用, 长超时会阻塞
         * 整个目录的所有操作. 使用固定 2s 短超时:
         *   - 正常 RPC 在 LAN 内 <10ms, 2s 足够
         *   - 断连/filer 重启时快速返回 -EAGAIN, VFS 上层自动重试
         *   - 不使用 powerfs_net_pick_timeout (自适应 10s), 因为
         *     lookup 在 i_rwsem 内, 必须快速释放锁
         *
         * 重试由 VFS 上层处理 (namei.c __lookup_slow 循环),
         * 每次重试间隔由 VFS 调度, 提供自然退避. */
        timeout_ms = POWERFS_LOOKUP_TIMEOUT_MS;  /* 2s: short timeout under i_rwsem */

        /* 通过 powerfs_net 直接查询 (含时间戳 + volume_id/file_key + layout) */
        ts_net = ktime_get_ns();
        err = powerfs_net_lookup_timeout(dir->i_ino, dentry->d_name.name,
                                          strlen(dentry->d_name.name),
                                          &ino, &mode, &uid, &gid,
                                          &size, &nlink,
                                          &mtime, &atime, &ctime,
                                          &volume_id, &file_key,
                                          &lookup_layout,
                                          timeout_ms);
        net_dur_us = div_u64(ktime_get_ns() - ts_net, 1000);

        /* 断连/重连期间超时或网络不可达, 返回 -EAGAIN 让 VFS/应用层重试.
         * - ETIMEDOUT: 2s 内未收到响应 (filer 不可达或繁忙)
         * - ENOTCONN: disconnect_one 已 complete(-ENOTCONN) (在途请求被取消)
         * - ESHUTDOWN: pool 正在 stopping
         *
         * 2s 短超时策略: 不等待 filer 重启完成, 快速返回 -EAGAIN 让 VFS 重试.
         * VFS 的 __lookup_slow 循环会自动重试, 每次重试间隔由调度器决定,
         * filer 恢复后下一次 lookup 即可成功. 避免在 i_rwsem 读锁内长时间阻塞. */
        if (err == -ETIMEDOUT || err == -ENOTCONN || err == -ESHUTDOWN) {
            total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
            pr_warn("powerfs: LOOKUP '%pd' EAGAIN err=%d net=%lluus total=%lluus\n",
                    dentry, err, net_dur_us, total_us);
            return ERR_PTR(-EAGAIN);
        }

        if (err == 0 && ino != 0) {
            /* 找到文件: 创建 inode */
            pr_debug("powerfs: lookup '%pd' found ino=%llu mode=%o\n",
                     dentry, (unsigned long long)ino, mode);

            inode = powerfs_iget(dir->i_sb, ino);
            if (IS_ERR(inode)) {
                pr_warn("powerfs: lookup '%pd' iget failed: %ld\n",
                        dentry, PTR_ERR(inode));
                /* K3: iget 失败, 释放 parse 分配的 volume_ids/inline_data/replica_chunks */
                kfree(lookup_layout.volume_ids);
                kfree(lookup_layout.inline_data);
                kfree(lookup_layout.replica_chunks);
                kfree(lookup_layout.ec_chunks);
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
                inode_set_mtime(inode, mtime, 0);
                inode_set_atime(inode, atime, 0);
                inode_set_ctime(inode, ctime, 0);

                {
                    struct powerfs_inode_info *pi = POWERFS_I(inode);
                    spin_lock(&pi->i_lock);
                    pi->cache_valid = true;
                    /* K2-11: Initialize content_size to the server's size so
                     * that refresh_work's != check works correctly. Without
                     * this, content_size stays at 0 (from alloc_inode) and
                     * refresh_work would always skip size updates. */
                    pi->content_size = size;
                    /* 数据直连: 存储 volume_id/file_key 用于 ReadNeedle/WriteNeedle.
                     * 目录的 volume_id/file_key 为 0 (无数据). */
                    pi->volume_id = volume_id;
                    pi->file_key = file_key;
                    /* K3: 应用 FileLayout (placement/volume_ids 等).
                     * Stripe 文件在 lookup 时即获取 volume_ids, 无需等待 getattr. */
                    powerfs_apply_layout_to_inode(pi, &lookup_layout);
                    spin_unlock(&pi->i_lock);
                }

                unlock_new_inode(inode);
            } else {
                struct powerfs_inode_info *pi = POWERFS_I(inode);
                u64 local_content_size;
                /* 已有 inode (非 I_NEW): d_revalidate 返回 0 触发 re-lookup.
                 * 用 Filer 返回的权威属性更新现有 inode, 否则跨客户端
                 * 修改后内核仍用旧的 i_size/volume_id/file_key, 导致
                 * 读取空内容 (size=0 跳过 read) 或 needle not found. */
                /* Read content_size before inode->i_lock (lock ordering). */
                spin_lock(&pi->i_lock);
                local_content_size = pi->content_size;
                spin_unlock(&pi->i_lock);

                spin_lock(&inode->i_lock);
                inode->i_mode = mode;
                inode->i_uid = make_kuid(&init_user_ns, uid);
                inode->i_gid = make_kgid(&init_user_ns, gid);
                /* Don't overwrite i_size if local content_size != server size:
                 * local truncate/write set a size that the server hasn't
                 * reflected yet (stale GETATTR). See refresh_work.
                 * K2-11: Changed from <= to == to handle both extend and
                 * truncate cases (local can be < or > server size). */
                if (local_content_size == size &&
                    i_size_read(inode) != size) {
                    i_size_write(inode, size);
                }
                set_nlink(inode, nlink);
                inode_set_mtime(inode, mtime, 0);
                inode_set_atime(inode, atime, 0);
                inode_set_ctime(inode, ctime, 0);
                spin_unlock(&inode->i_lock);

                {
                    spin_lock(&pi->i_lock);
                    /* K2-11: Only sync content_size when local matches server
                     * (no pending local truncate/write). */
                    if (local_content_size == size)
                        pi->content_size = size;
                    pi->cache_valid = true;
                    pi->cache_expire = jiffies + POWERFS_INODE_CACHE_TTL;
                    /* 仅在 Filer 返回非零值时更新 volume_id/file_key.
                     * close 前的 lookup 可能返回 0 (Filer 端 chunks 在 close 时才同步),
                     * 若用 0 覆盖已有值, 会导致 writeback/read locate 失败. */
                    if (volume_id != 0)
                        pi->volume_id = volume_id;
                    if (file_key != 0)
                        pi->file_key = file_key;
                    /* K3: 更新 FileLayout (placement 可能从 Flat 切换到 Stripe) */
                    powerfs_apply_layout_to_inode(pi, &lookup_layout);
                    spin_unlock(&pi->i_lock);
                }
                pr_debug("powerfs: lookup '%pd' updated existing inode ino=%llu size=%llu vid=%llu fkey=%llu\n",
                        dentry, (unsigned long long)ino,
                        (unsigned long long)size,
                        (unsigned long long)volume_id,
                        (unsigned long long)file_key);
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
             * 中实例化 dentry 的正确接口 (参考 xxx_lookup -> d_splice_alias
             * -> __d_add)。
             */
            d_add(dentry, inode);

            /* Per-dentry lease: lookup 成功后填充 (对齐  __update_dentry_lease).
             * 正 dentry: lease 有效期间信任缓存, d_revalidate Layer 1 命中. */
            powerfs_fill_dentry_lease(dentry, dir, 0);

            /* 目录级 lease: lookup 成功后续约父目录 lease.
             * 一次 RPC 同时完成查询+续约, 后续同目录的 d_revalidate
             * 全部 RCU 命中, 无网络交互. */
            WRITE_ONCE(POWERFS_I(dir)->dir_lease_expire,
                       jiffies + POWERFS_DIR_LEASE_TTL);

            pr_debug("powerfs: lookup '%pd' completed\n", dentry);
            total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
            pr_info_ratelimited("powerfs: LOOKUP '%pd' found ino=%llu net=%lluus total=%lluus\n",
                    dentry, (unsigned long long)ino, net_dur_us, total_us);
            if (sbi->client)
                powerfs_update_metadata_metrics(&sbi->client->metrics,
                                                ns_to_ktime(ts_entry), ktime_get(), 0);
            return NULL;
        }

        if (err == -ENOENT) {
            /* 文件不存在: 不缓存负 dentry.
             *
             * 核心根因: Linux VFS 对负 dentry (inode==NULL) 在 lookup_fast
             * 阶段**找到后立即返回 ENOENT, 不调用 d_revalidate**
             * (namei.c lookup_fast 命中 DCACHE negative 直接 return,
             *  d_revalidate 仅在 positive dentry + LOOKUP_OPEN 时才
             *  被调用, 或在 REF 路径需要确认 dentry 状态时).
             * 这意味着:
             *   1. SSH 1: CREATE FILE (成功, 写/关)
             *   2. SSH 2: 某次竞态下 LOOKUP RPC 返回 ENOENT → 此处
             *      d_add(NULL) → negative dentry 入 dcache
             *   3. SSH 2 下一次 md5sum FILE → lookup_fast → 命中
             *      negative dentry → 立即 ENOENT → 永不 d_revalidate
             *      → 永不 LOOKUP RPC → 永久 "file not found"
             *   4. SSH 2 stat FILE → 幸运的走了 lookup_slow (如果
             *      dentry 还没进 hash) → LOOKUP 成功 → positive dentry
             *      → 这就是我们看到 "stat OK md5sum FAIL" 诡异现象!
             *
             * 彻底 fix: ENOENT 时 never cache negative dentry.
             * 替代 d_add(NULL): d_drop(unhash) + d_invalidate + return NULL.
             * 代价: 每次 ENOENT 都走 RPC, 但正确性优先.
             * 后续优化: 在 ->d_revalidate 成功后 (若 VFS 终于对
             * negative 开始调 d_revalidate) 再考虑 1s 短 TTL negative
             * cache, 对齐 FUSE attr_timeout=1 entry_timeout=1.
             */
            pr_debug("powerfs: lookup '%pd' not found (powerfs_net, no negative cache)\n",
                     dentry);
            kfree(lookup_layout.volume_ids);
            kfree(lookup_layout.inline_data);
            kfree(lookup_layout.replica_chunks);
            kfree(lookup_layout.ec_chunks);

            /* 在 lookup 结束回调中不添加负 dentry。
             * d_in_lookup dentry 直接 d_drop unhashed + return NULL，
             * VFS 下次 LOOKUP 同一 name 时 lookup_fast miss →
             * 走 lookup_real → powerfs_lookup → LOOKUP RPC，
             * 避免 stale negative 缓存。*/
            if (!d_unhashed(dentry))
                d_drop(dentry);

            /* 目录级 lease: ENOENT 也续约 (避免父目录每次都 miss) */
            WRITE_ONCE(POWERFS_I(dir)->dir_lease_expire,
                       jiffies + POWERFS_DIR_LEASE_TTL);
            total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
            pr_info_ratelimited("powerfs: LOOKUP '%pd' enoent (no-cache) net=%lluus total=%lluus\n",
                    dentry, net_dur_us, total_us);
            if (sbi->client)
                powerfs_update_metadata_metrics(&sbi->client->metrics,
                                                ns_to_ktime(ts_entry), ktime_get(), 0);
            return NULL;
        }

        /* 其他错误: 返回错误给 VFS, 不缓存负 dentry (避免误缓存 "不存在").
         * Phase 1: 之前是 "记录 + 添加负 dentry", 这会让网络瞬态错误被
         * 当成 "文件不存在" 缓存, 后续访问该文件继续返回 ENOENT.
         * 现在返回错误, 让 VFS/userspace 看到真实错误码并重试. */
        total_us = div_u64(ktime_get_ns() - ts_entry, 1000);
        pr_warn("powerfs: LOOKUP '%pd' error=%d net=%lluus total=%lluus\n",
                dentry, err, net_dur_us, total_us);
        /* K3: err 路径 lookup_layout 已零初始化, kfree(NULL) 安全 */
        kfree(lookup_layout.volume_ids);
        kfree(lookup_layout.inline_data);
        kfree(lookup_layout.replica_chunks);
        kfree(lookup_layout.ec_chunks);
        return ERR_PTR(err);
    }

    /* === 纯本地模式: 添加负 dentry === */
    pr_debug("powerfs: lookup '%pd' not found (local mode)\n", dentry);
    d_add(dentry, NULL);
    if (sbi->client)
        powerfs_update_metadata_metrics(&sbi->client->metrics,
                                        ns_to_ktime(ts_entry), ktime_get(), 0);
    return NULL;
}

/* ========== P1-2: create 核心 helper (供 mknod 和 atomic_open 共享) ========== */

/*
 * __powerfs_do_create_core - 原子创建 inode (Filer RPC + 本地 inode 构造)
 *
 * 与 powerfs_mknod 的区别：
 *   - 不 d_instantiate()、不加 dir_entry、不更新父目录时间戳
 *   - 只负责 net_create → new_inode → 应用 layout 的核心路径
 *   - 返回新创建的 inode (带引用计数，调用者负责 iput/d_instantiate)
 *
 * 用于 atomic_open: 把 "lookup→create→open" 合成一个回调，避免 VFS
 * 在 lookup 和 create 之间的窗口 (TOCTOU) 被其他客户端抢先。
 */
static struct inode *__powerfs_do_create_core(struct mnt_idmap *idmap,
                                               struct inode *dir,
                                               struct dentry *dentry,
                                               umode_t mode, dev_t dev)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct inode *inode;
    u64 new_ino;
    u64 mknod_volume_id = 0, mknod_file_key = 0;
    struct powerfs_file_layout mknod_layout;
    bool mknod_has_layout = false;

    (void)idmap;
    (void)dev;

    if (S_ISREG(mode) || S_ISDIR(mode) || S_ISFIFO(mode) ||
        S_ISBLK(mode) || S_ISCHR(mode) || S_ISSOCK(mode)) {
        u64 remote_ino = 0;
        u64 volume_id = 0, file_key = 0;
        struct powerfs_file_layout layout = {0};
        int rerr = powerfs_net_create(dir->i_ino, dentry->d_name.name,
                                       dentry->d_name.len, mode,
                                       from_kuid(&init_user_ns, current_fsuid()),
                                       from_kgid(&init_user_ns, current_fsgid()),
                                       S_ISDIR(mode), &remote_ino,
                                       &volume_id, &file_key, &layout);
        if (rerr) {
            pr_warn("powerfs: net_create '%pd' failed: %d\n", dentry, rerr);
            return ERR_PTR(rerr);
        }
        new_ino = remote_ino ? remote_ino
                             : (u64)atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
        mknod_volume_id = volume_id;
        mknod_file_key = file_key;
        mknod_layout = layout;
        mknod_has_layout = true;
    } else {
        new_ino = atomic_inc_return(&sbi->next_ino) + POWERFS_INO_START;
    }

    inode = powerfs_new_inode(dir->i_sb, mode, new_ino,
                               dir->i_ino, dentry->d_name.name);
    if (!inode)
        return ERR_PTR(-ENOSPC);

    if (S_ISDIR(mode)) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        WRITE_ONCE(pi->dir_complete, true);
        pi->i_flags |= POWERFS_I_COMPLETE;
        WRITE_ONCE(pi->dir_lease_expire, jiffies + POWERFS_DIR_LEASE_TTL);
    }

    if (S_ISREG(mode) && mknod_volume_id != 0) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        spin_lock(&pi->i_lock);
        pi->volume_id = mknod_volume_id;
        pi->file_key = mknod_file_key;
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: do_create_core '%pd' ino=%lu volume_id=%llu file_key=%llu\n",
                 dentry, inode->i_ino, mknod_volume_id, mknod_file_key);
    }

    if (S_ISREG(mode) && mknod_has_layout) {
        struct powerfs_inode_info *pi = POWERFS_I(inode);
        spin_lock(&pi->i_lock);
        powerfs_apply_layout_to_inode(pi, &mknod_layout);
        spin_unlock(&pi->i_lock);
        pr_debug("powerfs: do_create_core '%pd' ino=%lu placement=%u reliability=%u\n",
                 dentry, inode->i_ino, pi->placement, pi->reliability);
    } else if (mknod_has_layout && (mknod_layout.volume_ids || mknod_layout.inline_data ||
                                    mknod_layout.replica_chunks || mknod_layout.ec_chunks)) {
        kfree(mknod_layout.volume_ids);
        kfree(mknod_layout.inline_data);
        kfree(mknod_layout.replica_chunks);
        kfree(mknod_layout.ec_chunks);
    }

    return inode;
}

/* ========== 目录操作: mknod (通用创建函数) ========== */

/*
 * powerfs_mknod - 创建文件/目录/设备节点 (内部通用函数)
 *
 * 参考 ramfs_mknod (fs/ramfs/inode.c)
 */
int powerfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
                          struct dentry *dentry, umode_t mode, dev_t dev)
{
    struct inode *inode;
    u64 new_ino;
    int type;
    int ret;

    pr_debug("powerfs: mknod '%pd' mode=%o in dir=%lu\n",
             dentry, mode, dir->i_ino);

    /* P2-7: Quota check — 文件数配额 */
    ret = powerfs_quota_check_max_files(dir);
    if (ret)
        return ret;

    /* 核心 create 逻辑 (P1-2 共享 helper):
     *   net_create (Filer Raft) → new_inode (本地) → 应用 layout.
     * 失败直接返回 errno; 成功返回带引用的 inode. */
    inode = __powerfs_do_create_core(idmap, dir, dentry, mode, dev);
    if (IS_ERR(inode)) {
        int rerr = PTR_ERR(inode);
        pr_warn("powerfs: mknod '%pd' do_create_core failed: %d\n", dentry, rerr);
        return rerr;
    }
    new_ino = inode->i_ino;

    /* 关联 dentry 和 inode (d_instantiate，不重复 hash) */
    d_instantiate(dentry, inode);

    /* 更新父目录时间戳 */
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

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

    /* 本地 mutation 清父目录 lease */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: mknod '%pd' success, ino=%llu\n",
             dentry, new_ino);

    return 0;
}

/* ========== 目录操作: mkdir ========== */

struct dentry *powerfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
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
        return ERR_PTR(err);
    }

    /* 增加父目录 nlink (".." 指向父目录) */
    inc_nlink(dir);

    /* 获取新创建的 inode 用于远程同步 */
    inode = d_inode(dentry);

    pr_debug("powerfs: mkdir '%pd' success, ino=%lu\n",
            dentry, inode ? inode->i_ino : 0);

    return NULL;
}

/* ========== 目录操作: rmdir ========== */

int powerfs_rmdir(struct inode *dir, struct dentry *dentry)
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
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 减少被删除目录的链接数 (参考 simple_rmdir, 不 ihold/iput) */
    /* Guard: filer 的 DecrementNlink 可能已通过 NOTIFY → refresh_work →
     * set_nlink 将 i_nlink 置 0, 此时 drop_nlink 会下溢触发 WARNING. */
    if (inode->i_nlink > 0)
        drop_nlink(inode);

    /* 清空子目录的目录项链表 */
    powerfs_clear_dir_entries(inode);

    /* Invalidate the removed directory's own lease. Without this, the inode
     * may stay in the inode cache (dentry references) with stale
     * dir_complete=true + future dir_lease_expire. When the Filer reuses
     * this inode number for a new directory, iget5_locked returns the cached
     * inode, and readdir takes the fast-path with an empty dir_entries list,
     * skipping the Filer fetch (T3b/T3c/T9a/T9b "Directory not empty"). */
    powerfs_invalidate_dir_lease(inode);

    /* 从父目录的本地目录项链表中移除 */
    powerfs_remove_dir_entry(dir, dentry->d_name.name);

    /* 减少父目录的链接数 (因删除了一个子目录) */
    if (dir->i_nlink > 0)
        drop_nlink(dir);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: rmdir '%pd' success\n", dentry);

    /*
     * 注意: 不要在这里 iput(inode)
     *       让 do_rmdir 的 iput 统一处理
     */
    return 0;
}

/* ========== 目录操作: create ========== */

int powerfs_create(struct mnt_idmap *idmap, struct inode *dir,
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

    pr_debug("powerfs: create '%pd' success, ino=%lu\n",
             dentry, inode ? inode->i_ino : 0);

    return 0;
}

/* ========== P1-2: atomic_open (原子创建+打开，消除 TOCTOU) ========== */

/*
 * powerfs_atomic_open - 把 VFS "lookup → create → open" 三步合成一次回调
 *
 * 解决的核心问题 (对齐  xxx_atomic_open):
 *   - 原先 VFS: lookup(负 dentry ENOTDIR) → schedule → 另一客户端同文件名 CREATE
 *     → 回到本客户端 → .create 收到 -EEXIST，但 VFS 期望 O_CREAT|O_EXCL 时
 *     才返回 EEXIST，普通 O_CREAT 应当静默返回已存在文件; 反之亦然
 *   - 竞态窗口 (TOCTOU): tar/gcc 编译临时文件、flock(O_CREAT|O_EXCL) 互斥
 *     NFSv4 export 原子 open CREATE 全部因此失败
 *
 * 实现要点:
 *   1. flags &= ~O_TRUNC: atomic_open 先不 truncate，VFS 在后续权限检查后
 *      用 notify_change 单独 truncate，避免越权
 *   2. O_CREAT 分支: 调用共享的 __powerfs_do_create_core (Filer 原子 RPC)，
 *      用 d_splice_alias 把 inode 拼到 dentry，d_splice_alias 内部已处理
 *      in-lookup/hashed 两种 dentry 状态
 *   3. 最后调用 finish_open(): 一次性把 file->private_data 绑定 + 调
 *      powerfs_file_open 拿 cap 引用，VFS 不再进入后续 ->create 回调
 *   4. 若 !O_CREAT 且 dentry 不在 lookup 态 (已 hashed 的负 dentry)，直接
 *      -ENOENT；若 d_in_lookup 则 d_add(NULL) 完成 lookup，VFS 走后续
 *      普通路径
 */
int powerfs_atomic_open(struct inode *dir, struct dentry *dentry,
                                struct file *file, unsigned open_flag,
                                umode_t create_mode)
{
    /* 6.17 atomic_open 不提供 mnt_idmap 形参, 用 file_mnt_idmap() 自取.
     * 对齐  xxx_atomic_open file.c L779: idmap = file_mnt_idmap(file). */
    struct mnt_idmap *idmap = file_mnt_idmap(file);
    struct inode *inode = NULL;
    int err;
    unsigned flags = open_flag;

    if (dentry->d_name.len > NAME_MAX)
        return -ENAMETOOLONG;

    /* O_TRUNC 在权限检查之后由 VFS 做，这里先清除 */
    flags &= ~O_TRUNC;

    pr_debug("powerfs: atomic_open '%pd' in dir=%lu flags=0x%x mode=0%o d_in_lookup=%d d_inode_is_null=%d\n",
             dentry, dir->i_ino, flags, create_mode, !!d_in_lookup(dentry),
             (int)!d_inode(dentry));

    if (flags & O_CREAT) {
        /* —— 原子创建分支: 复用 __powerfs_do_create_core —— */
        int type;
        struct dentry *dn;

        /* POSIX O_CREAT 语义:
         *   - O_EXCL | O_CREAT: 不存在才创建, 存在则 -EEXIST
         *   - O_CREAT (没有 O_EXCL): 存在就直接打开已存在文件,
         *     只有不存在时才创建.
         *
         * 由于 powerfs_net_create 目前 API 不带 O_EXCL flag,
         * Filer 侧 CREATE 总是覆盖式创建 (成功+返回新 inode 号),
         * 导致 "shell >> FILE" (O_CREAT|O_APPEND) 场景把之前写
         * 的文件又覆盖了, 丢失内容.
         *
         * 一致性 fix: 在 O_CREAT 前, 对于 !O_EXCL case 先同步
         * LOOKUP 一次:
         *   [hit positive] → goto existing_lookup 打开已存在文件 (安全)
         *   [miss ENOENT]  → 真正调用 create_core 创建.
         *   [其他错误]    → propagate error.
         * O_EXCL 时跳过预 LOOKUP, 直接 CREATE (让 Filer 返回 -EEXIST
         * 时 atomic_open 再向上抛).
         */
        if (!(flags & O_EXCL)) {
            struct dentry *(*lookup_fn)(struct inode *, struct dentry *, unsigned int);
            struct dentry *lres;
            lookup_fn = (typeof(lookup_fn))dir->i_op->lookup;
            pr_debug("powerfs: atomic_open '%pd' O_CREAT !O_EXCL → pre-lookup\n",
                     dentry);
            lres = lookup_fn(dir, dentry, 0);
            if (IS_ERR(lres)) {
                long lerr = PTR_ERR(lres);
                if (lerr != -ENOENT) {
                    pr_warn("powerfs: atomic_open '%pd' pre-lookup failed: %ld\n",
                            dentry, lerr);
                    return (int)lerr;
                }
                /* ENOENT: powerfs_lookup d_drop + return NULL → 需要创建 */
                pr_debug("powerfs: atomic_open '%pd' pre-lookup ENOENT → creating\n",
                         dentry);
            } else {
                struct inode *dino_after = d_inode(dentry);
                if (dino_after) {
                    pr_debug("powerfs: atomic_open '%pd' pre-lookup HIT positive ino=%lu → existing_lookup\n",
                             dentry, dino_after->i_ino);
                    /* dentry 已经 hashed, 解除 lookup 态 (若还在) */
                    d_lookup_done(dentry);
                    goto existing_lookup;
                }
                pr_debug("powerfs: atomic_open '%pd' pre-lookup NULL (ENOENT no-cache) → creating\n",
                         dentry);
            }
        }

        /* FIX BUG at dcache.c:2993: d_splice_alias() requires d_unhashed(dentry).
         * When VFS calls atomic_open after a prior LOOKUP that instantiated a
         * hashed NEGATIVE dentry (via d_add(NULL)), dentry is already hashed and
         * BUG_ON(!d_unhashed(dentry)) fires.  d_drop removes the stale neg
         * dentry from dcache hash, restoring unhashed state required by
         * d_splice_alias (matches NFS/overlayfs atomic_open idiom). */
        if (!d_unhashed(dentry))
            d_drop(dentry);

        inode = __powerfs_do_create_core(idmap, dir, dentry,
                                          create_mode | S_IFREG, 0);
        if (IS_ERR(inode)) {
            int rerr = PTR_ERR(inode);
            inode = NULL;
            /* -EEXIST 视为文件已存在 (O_EXCL 时向上抛，否则走已存在路径) */
            if (rerr == -EEXIST && !(flags & O_EXCL)) {
                pr_debug("powerfs: atomic_open '%pd' EEXIST,!O_EXCL → treat as existing\n",
                         dentry);
                goto existing_lookup;
            }
            pr_warn("powerfs: atomic_open '%pd' do_create_core failed: %d\n",
                    dentry, rerr);
            return rerr;
        }

        /* d_splice_alias: 把新 inode 拼到 dentry，兼容 d_in_lookup / hashed
         * 两种态。若返回 alias dentry，也能用（ 做法 WARN_ON != dentry） */
        dn = d_splice_alias(inode, dentry);
        WARN_ON_ONCE(dn && dn != dentry);
        inode = NULL; /* d_splice_alias 已转移引用 */

        /* 尾部工作 (与 mknod 对称): 更新父目录时间戳 + dir_entry + 清 lease */
        {
            struct timespec64 now = current_time(dir);
            inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
            inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
        }
        type = S_IFREG;
        powerfs_add_dir_entry(dir, d_inode(dentry)->i_ino, type, dentry->d_name.name);
        powerfs_invalidate_dir_lease(dir);

        /* 标记 file: 通知上层这个 file 是这次刚创建的 */
        file->f_mode |= FMODE_CREATED;

        /* finish_open: 调 powerfs_file_open 拿 cap 引用 + 绑定 file 私有结构
         *  file.c L982: err = finish_open(file, dentry, xxx_open) */
        err = finish_open(file, dentry, powerfs_file_open);
        pr_debug("powerfs: atomic_open '%pd' CREATE+OPEN finish_open err=%d f_mode_created=%d\n",
                 dentry, err, !!(file->f_mode & FMODE_CREATED));
        return err;
    }

existing_lookup:
    /* —— 非创建分支: LOOKUP + OPEN (或 EEXIST fallthrough) —— */
    {
        bool in_lookup = !!d_in_lookup(dentry);
        struct inode *dinode = d_inode(dentry);
        bool dn_neg = !dinode;

        pr_debug("powerfs: atomic_open existing_lookup '%pd' in_lookup=%d inode=%p dn_neg=%d O_CREAT=%d O_EXCL=%d\n",
                dentry, (int)in_lookup, (void*)dinode, (int)dn_neg,
                !!(open_flag & O_CREAT), !!(open_flag & O_EXCL));

        /* ===== Case 1: 已实例化 (not in_lookup) ===== */
        if (!in_lookup) {
            if (dn_neg) {
                if (!d_unhashed(dentry))
                    d_drop(dentry);
                return -ENOENT;
            }
            err = finish_open(file, dentry, powerfs_file_open);
            pr_debug("powerfs: atomic_open existing_lookup positive ino=%lu finish_open rc=%d\n",
                    dinode->i_ino, err);
            return err;
        }

        /* ===== Case 2: in_lookup + !O_CREAT (md5sum O_RDONLY scenario) =====
         *
         * 不能用 finish_no_open(file, dentry): 它会设置 FMODE_PATH +
         * return 0, VFS 误以为 "atomic_open 完成了 O_PATH 类型 open",
         * 而实际上并没有真正打开文件, 后续 read/write 失败.
         *
         * 也不能 d_add(NULL) 实例化负 dentry + finish_no_open:
         * 负 dentry 会被 VFS lookup_fast 立即 ENOENT, 不调 d_revalidate,
         * 造成 stale negative cache 死锁 (除非不缓存 negative).
         *
         * 正确策略: 同步调用 ->lookup (powerfs_lookup) 把 in_lookup
         * dentry 完整实例化.  powerfs_lookup 对于 ENOENT 会 d_drop
         * unhashed (不缓存 negative), 对于 found 会 d_add positive.
         * 然后 d_lookup_done(dentry) 解除 in_lookup 状态, 让 VFS 可以
         * 继续走正常 ->open 回调 (powerfs_file_open).
         *
         * 参考: NFS atomic_open !O_CREAT 路径 fallback 直接
         * nfs_lookup_real 完成 dentry 实例化.
         */
        if (dn_neg) {
            struct dentry *(*lookup_fn)(struct inode *, struct dentry *, unsigned int);
            struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
            struct dentry *res;

            lookup_fn = (typeof(lookup_fn))dir->i_op->lookup;
            pr_debug("powerfs: atomic_open in_lookup !O_CREAT → doing self-lookup '%pd'\n",
                    dentry);
            res = lookup_fn(dir, dentry, 0);
            /* lookup 语义: 返回 NULL 表示成功 (dentry 已实例化),
             * 返回 ERR_PTR(err) 表示错误. dentry 本身在 lookup 中
             * 被修改. */
            if (IS_ERR_OR_NULL(res)) {
                long lerr = PTR_ERR(res);
                if (IS_ERR(res) && lerr != -ENOENT) {
                    d_lookup_done(dentry);
                    return (int)lerr;
                }
                /* ENOENT 或 NULL: dentry 更新过了.
                 * 如果 lookup ENOENT 时 powerfs_lookup 做了
                 * d_drop(dentry) → unhashed → VFS will propagate ENOENT */
            }
            d_lookup_done(dentry);

            /* After self-lookup: re-check state. */
            dinode = d_inode(dentry);
            dn_neg = !dinode;
            if (dn_neg) {
                pr_debug("powerfs: atomic_open in_lookup !O_CREAT self-lookup ENOENT '%pd'\n", dentry);
                if (sbi && sbi->client) {
                    /* no cache */
                }
                return -ENOENT;
            }
            /* Positive: finish_open normally. */
            err = finish_open(file, dentry, powerfs_file_open);
            pr_debug("powerfs: atomic_open in_lookup !O_CREAT positive ino=%lu finish_open rc=%d\n",
                    dinode->i_ino, err);
            return err;
        }

        /* in_lookup + positive (稀有 case): 直接 finish_open. */
        err = finish_open(file, dentry, powerfs_file_open);
        pr_debug("powerfs: atomic_open in_lookup positive (rare) ino=%lu finish_open rc=%d\n",
                dinode->i_ino, err);
        return err;
    }
}

/* ========== 目录操作: unlink ========== */

int powerfs_unlink(struct inode *dir, struct dentry *dentry)
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
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 减少 inode 链接数 (参考 simple_unlink, 不 ihold/iput, 由 VFS dentry 管理) */
    /* Guard: filer 可能已通过 NOTIFY → refresh_work 将 i_nlink 置 0. */
    if (inode->i_nlink > 0)
        drop_nlink(inode);

    /* 从本地目录项链表中移除 */
    powerfs_remove_dir_entry(dir, dentry->d_name.name);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: unlink '%pd' success\n", dentry);

    return 0;
}

/* ========== 目录操作: symlink ========== */

int powerfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
                            struct dentry *dentry, const char *symname)
{
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(dir->i_sb);
    struct inode *inode;
    struct powerfs_inode_info *pi;
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

    pi = POWERFS_I(inode);

    /* 使用 page cache 存储符号链接目标 */
    err = page_symlink(inode, symname, strlen(symname) + 1);
    if (err) {
        iput(inode);
        return err;
    }

    /* Filer 会将 symlink target 存储为 InlineData (placement=Inline).
     * 本地也存储一份到 inline_data, 以便 page cache 被 evict 后
     * netfs_issue_read 能从 inline_data 恢复 (而非回退到 volume_read
     * 导致 -EINVAL, 因为 Inline 文件无 needle). */
    {
        size_t target_len = strlen(symname) + 1;
        u8 *buf = kvmalloc(target_len, GFP_KERNEL);
        if (buf) {
            memcpy(buf, symname, target_len);
            spin_lock(&pi->i_lock);
            kfree(pi->inline_data);
            pi->inline_data = buf;
            pi->inline_len = target_len;
            pi->placement = POWERFS_PLACEMENT_INLINE;
            pi->inline_dirty = false;  /* Filer 已知 target */
            spin_unlock(&pi->i_lock);
        }
    }

    /* 关联 dentry 和 inode (用 d_instantiate, 不能用 d_add — 见 powerfs_mknod 注释) */
    d_instantiate(dentry, inode);

    /* 更新父目录时间戳 */
    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 添加目录项到本地链表 */
    powerfs_add_dir_entry(dir, new_ino, S_IFLNK, dentry->d_name.name);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    pr_debug("powerfs: symlink '%pd' success, ino=%llu\n",
             dentry, (unsigned long long)new_ino);

    return 0;
}

/* ========== 目录操作: readlink ========== */

/*
 * powerfs_readlink - 读取符号链接目标
 *
 * 参考 xxx_readlink (fs/xxx/symlink.c)
 *
 * 策略:
 *   - 通信层可用时: 优先从服务端获取最新目标路径
 *   - 纯内存模式: 直接从 page cache 读取
 */
int powerfs_readlink(struct dentry *dentry, char *buffer, int buflen)
{
    struct inode *inode = d_inode(dentry);
    struct powerfs_inode_info *pi;
    struct page *page;
    void *page_addr;
    u32 len;

    pr_debug("powerfs: readlink '%pd'\n", dentry);

    if (!inode)
        return -ENOENT;

    if (!S_ISLNK(inode->i_mode))
        return -EINVAL;

    if (inode->i_size == 0) {
        /* Symlink size may be 0 if created by an older Filer that didn't
         * set size=target.len(). Fall through to inline_data / GETATTR
         * instead of returning empty string. */
        pr_debug("powerfs: readlink '%pd' size=0, trying inline/getattr\n", dentry);
    }

    len = min_t(u32, (u64)inode->i_size, buflen - 1);
    if (len == 0)
        len = buflen - 1;  /* fallback: allow GETATTR to fill the target */

    /* 1. Try page cache first (fast path, works before remount). */
    page = find_get_page(inode->i_mapping, 0);
    if (!page) {
        pi = POWERFS_I(inode);

        /* 2. Check inline_data (set by powerfs_symlink or GETATTR response). */
        spin_lock(&pi->i_lock);
        if (pi->inline_data && pi->inline_len > 0) {
            u32 copy_len = min_t(u32, pi->inline_len, len);
            memcpy(buffer, pi->inline_data, copy_len);
            buffer[copy_len] = '\0';
            spin_unlock(&pi->i_lock);
            pr_debug("powerfs: readlink '%pd' from inline_data: '%s'\n",
                     dentry, buffer);
            return 0;
        }
        spin_unlock(&pi->i_lock);

        /* 3. No inline_data: fetch via GETATTR (symlink target is stored as
         * inline_data on the Filer, but LOOKUP response may not include it).
         * After GETATTR, inline_data is populated and we can read from it. */
        {
            struct powerfs_file_layout layout = {0};
            __u32 mode = 0, uid = 0, gid = 0, nlink = 0;
            __u64 size = 0, mtime = 0, atime = 0, ctime = 0;
            __u64 volume_id = 0, file_key = 0;
            int ret;

            ret = powerfs_net_getattr(inode->i_ino, &mode, &uid, &gid,
                                      &size, &nlink, &mtime, &atime, &ctime,
                                      &volume_id, &file_key, &layout,
                                      NULL, NULL, NULL, NULL, NULL);  /* P1-5: readlink 不需要 rstat */
            if (ret == 0) {
                spin_lock(&pi->i_lock);
                powerfs_apply_layout_to_inode(pi, &layout);
                if (pi->inline_data && pi->inline_len > 0) {
                    u32 copy_len = min_t(u32, pi->inline_len, len);
                    memcpy(buffer, pi->inline_data, copy_len);
                    buffer[copy_len] = '\0';
                    spin_unlock(&pi->i_lock);
                    pr_debug("powerfs: readlink '%pd' from GETATTR inline: '%s'\n",
                             dentry, buffer);
                    kfree(layout.volume_ids);
                    kfree(layout.replica_chunks);
                    kfree(layout.ec_chunks);
                    return 0;
                }
                spin_unlock(&pi->i_lock);
            }
            kfree(layout.volume_ids);
            kfree(layout.inline_data);
            kfree(layout.replica_chunks);
            kfree(layout.ec_chunks);
        }

        /* 4. GETATTR didn't return inline_data: try read_cache_page as
         * last resort (triggers netfs_read_folio for Flat files). */
        page = read_cache_page(inode->i_mapping, 0, NULL, NULL);
        if (IS_ERR(page)) {
            pr_warn("powerfs: readlink '%pd' read_cache_page failed: %ld\n",
                    dentry, PTR_ERR(page));
            buffer[0] = '\0';
            return 0;
        }
    }

    page_addr = kmap(page);
    memcpy(buffer, page_addr, len);
    buffer[len] = '\0';
    kunmap(page);
    put_page(page);

    pr_debug("powerfs: readlink '%pd' from cache: '%s'\n", dentry, buffer);
    return 0;
}

/* ========== 目录操作: link (硬链接) ========== */

/*
 * powerfs_link - 创建硬链接
 *
 * 参考 xxx_link (fs/xxx/dir.c)
 *
 * 策略 (本地操作 + 异步通知，与mkdir/create/unlink保持一致):
 *   1. 本地增加 nlink 和 dentry
 *   2. 异步通知代理增加后端硬链接计数
 */
int powerfs_link(struct dentry *old_dentry, struct inode *dir,
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
     * d_instantiate 不递增 i_count! 每个 dentry 必须持有独立的 i_count,
     * 参考 simple_link / ramfs_link 均在 d_instantiate 前调用 ihold.
     * 用 d_instantiate (不能用 d_add): new_dentry 已被 lookup 路径加入 hash 链.
     */
    inc_nlink(inode);
    ihold(inode);
    d_instantiate(new_dentry, inode);

    {
        struct timespec64 now = current_time(dir);
        inode_set_mtime(dir, now.tv_sec, now.tv_nsec);
        inode_set_ctime(dir, now.tv_sec, now.tv_nsec);
    }

    /* 添加目录项到本地链表 */
    powerfs_add_dir_entry(dir, inode->i_ino,
                          inode->i_mode & S_IFMT,
                          new_dentry->d_name.name);

    /* Phase 1: 本地 mutation 清父目录 lease, 下次 readdir 重新拉取. */
    powerfs_invalidate_dir_lease(dir);

    return 0;
}
/* ========== rename ========== */

/*
 * powerfs_rename - 重命名/移动文件
 *
 * 参考 xxx_rename (fs/xxx/dir.c)
 *
 * 策略 (与 FUSE 侧对齐, POSIX-safe 同步模型):
 *   - 先通过 powerfs_net_rename 同步等 Filer Raft 提交 (POSIX 要求 rename
 *     成功返回后其他客户端立即可见, 所以必须等持久化).
 *   - Filer rename 成功后:
 *       1. 无条件从本地 dir_entries 移除 old_name 和 new_name
 *          (即使 new_dentry 不是 really positive, Filer 已 commit rename-over-replace,
 *           旧 target entry 必须被清掉; 否则后续 lookup/readdir 可能返回
 *           stale 8 字节 inode, 直到 dir_lease 过期).
 *       2. 对 target inode 做 pagecache invalidate + d_drop(new_dentry).
 *       3. 对 source inode 做 pagecache invalidate + d_drop(old_dentry).
 *          (VFS rename 成功返回后会调用 d_move, 但我们本地 dir_entries
 *           自己管理, 所以还要先 drop 掉).
 *
 * 处理情况:
 *   - 同一目录内重命名
 *   - 跨目录移动
 *   - 目标已存在 (覆盖)
 */
int powerfs_rename(struct mnt_idmap *idmap,
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
        struct inode *target_inode = NULL;

        strncpy(old_name_buf, old_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        old_name_buf[POWERFS_MAX_NAME_LEN] = '\0';
        strncpy(new_name_buf, new_dentry->d_name.name, POWERFS_MAX_NAME_LEN);
        new_name_buf[POWERFS_MAX_NAME_LEN] = '\0';

        /* 保存替换目标 inode (用于后续 pagecache invalidate),
         * 需要在 d_move/d_drop 之前从 new_dentry 读出. */
        if (d_really_is_positive(new_dentry)) {
            target_inode = d_inode(new_dentry);

            /* 不支持 RENAME_EXCHANGE */
            if (flags & RENAME_EXCHANGE)
                return -EINVAL;

            if (!target_inode)
                return -ENOENT;

            /* 检查是否可以删除目标 */
            if (S_ISDIR(target_inode->i_mode)) {
                /* 目录必须为空 */
                if (!simple_empty(new_dentry))
                    return -ENOTEMPTY;
            }
        }
        /* RENAME_NOREPLACE: VFS already rejected the case where new_dentry
         * is positive (vfs_rename returns -EEXIST before calling i_op->rename).
         * Do NOT return EEXIST when new_dentry is negative/NULL — that breaks
         * `mv` (coreutils 8.30) which calls renameat2(RENAME_NOREPLACE) first
         * to detect an existing target, then falls back to plain rename().
         * A negative dentry (stale cache) made us return EEXIST, and mv,
         * seeing the target "exists" but stat() returning ENOENT, reported
         * "cannot overwrite non-directory" (T3-3f regression). */

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

        /* ── Phase A: 本地 dir_entries 清理 (对齐 fuse 侧 cache.rename 修复) ──
         *
         * 关键: 无条件从本地 dir_entries 移除 new_name.
         * d_really_is_positive(new_dentry) 仅反映 VFS dcache, 不反映
         * 我们自己的 dir_entries. Filer 已经 commit rename-over-replace,
         * 旧 target entry 必须被清掉, 否则:
         *   - readdir 通过 dir_entries 返回旧 8 字节条目 (例如 ar 的
         *     !<arch>\n magic-only libtest.a).
         *   - lookup 先查 dir_entries -> 返回旧 inode.
         *
         * 必须在 add_dir_entry 之前调用 remove_dir_entry, 否则
         * add_dir_entry 中的同名复用会保留旧 target inode. */
        powerfs_remove_dir_entry(new_dir, new_name_buf);
        powerfs_remove_dir_entry(old_dir, old_name_buf);

        /* ── Phase B: VFS dcache + pagecache 失效 (对齐 FUSE 侧的
         * FUSE_NOTIFY_INVAL_ENTRY + FUSE_NOTIFY_INVAL_INODE) ──
         *
         * Target inode (被替换的旧文件):
         *   - drop_nlink 已做在下方, 但还需清理 pagecache:
         *     内核可能还缓存着旧文件内容 (8 字节 magic),
         *     不清掉的话, 后续 read 可能通过 old alias 访问到 stale 数据.
         *   - d_drop(new_dentry) 强制下次 lookup 重新进入 powerfs_lookup.
         *
         * Source inode (被移动的文件):
         *   - 其在旧路径下的任何 dcache alias 都已无效.
         *   - d_drop(old_dentry) 后 VFS 会 d_move 它 (重命名回正常 dentry
         *     位置), 但 d_drop 保证旧 alias 不再被 DCACHE 哈希链命中. */
        if (target_inode) {
            pr_debug("powerfs: rename-over-replace: inval pages target ino=%lu\n",
                     target_inode->i_ino);
            /* 非阻塞丢弃 clean page, 跳过 dirty/locked page.
             * (rename 的目标通常是已 close 过的文件, page 应为 clean.) */
            invalidate_mapping_pages(target_inode->i_mapping, 0, (pgoff_t)-1);
            /* 从 DCACHE 哈希链摘掉 new_dentry (如果 still hashed),
             * 避免后续 path walk 命中 stale alias. */
            d_drop(new_dentry);
        }
        /* Source inode pagecache invalidate: 它被移到了新路径,
         * 旧路径下的任何 read/write/mmap alias 都应失效.
         * 内核 rename_succeeded 会处理 inode alias list, 但我们主动做
         * invalidate_mapping_pages 以防 VFS 跳过某些情况下 (例如 noopen). */
        invalidate_mapping_pages(inode->i_mapping, 0, (pgoff_t)-1);
        d_drop(old_dentry);

        /* ── Phase C: nlink / 时间戳 / dir_entries 更新 ── */

        /*
         * 目标已存在: 减少目标 inode 的 nlink
         * 参考 ramfs_rename (fs/ramfs/inode.c)
         */
        if (target_inode) {
            if (S_ISDIR(target_inode->i_mode)) {
                if (new_dir->i_nlink > 0)
                    drop_nlink(new_dir);
            }
            if (target_inode->i_nlink > 0)
                drop_nlink(target_inode);
        }

        /*
         * 目录跨移动: drop_nlink(old_dir) + inc_nlink(new_dir)
         * 参考 simple_rename_exchange
         */
        if (S_ISDIR(inode->i_mode) && old_dir != new_dir) {
            if (old_dir->i_nlink > 0)
                drop_nlink(old_dir);
            inc_nlink(new_dir);
        }

        /* 更新时间戳 (6.17: 使用 inode_set_mtime_to_ts/inode_set_ctime_to_ts) */
        {
            struct timespec64 now = current_time(old_dir);
            inode_set_mtime_to_ts(old_dir, now);
            inode_set_ctime_to_ts(old_dir, now);
            if (old_dir != new_dir) {
                now = current_time(new_dir);
                inode_set_mtime_to_ts(new_dir, now);
                inode_set_ctime_to_ts(new_dir, now);
            }
        }

        mark_inode_dirty(old_dir);
        if (old_dir != new_dir)
            mark_inode_dirty(new_dir);

        /* 在新目录添加新名称 (或在同一目录更新) */
        {
            unsigned int entry_type = inode->i_mode & S_IFMT;
            powerfs_add_dir_entry(new_dir, inode->i_ino,
                                  entry_type, new_name_buf);
        }

        /* Phase 1: 本地 mutation 清两个父目录的 lease + epoch++.
         * rename 涉及 old_dir 和 new_dir (可能相同), 都要清,
         * 下次 readdir 重新拉取, 看到重命名后的结果. */
        powerfs_invalidate_dir_lease(old_dir);
        if (old_dir != new_dir)
            powerfs_invalidate_dir_lease(new_dir);
    }

    pr_debug("powerfs: rename '%pd' -> '%pd' success\n",
             old_dentry, new_dentry);

    return 0;
}

/* ========== readdir: 目录读取操作 ========== */

/*
 * powerfs_dir_open - 打开目录文件
 *
 * 参考 xxx_dir_open (fs/xxx/dir.c)
 * 分配目录文件私有数据，初始化readdir状态
 */
int powerfs_dir_open(struct inode *inode, struct file *file)
{
    struct powerfs_dir_file_info *dfi;
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: dir_open ino=%lu\n", inode->i_ino);

    dfi = kzalloc(sizeof(*dfi), GFP_KERNEL);
    if (!dfi)
        return -ENOMEM;

    dfi->file = file;
    dfi->dir = pi;
    dfi->last_ino = 0;
    dfi->last_name = NULL;
    dfi->next_offset = 0;
    dfi->cached_entries = NULL;
    dfi->cached_count = 0;
    dfi->cached_index = 0;
    mutex_init(&dfi->lock);

    file->private_data = dfi;

    /* Cap 引用: 目录打开 → PIN + FILE_SHARED (对齐  xxx_init_file dir 路径) */
    spin_lock(&pi->i_lock);
    pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]++;
    pi->i_last_rd = jiffies;
    spin_unlock(&pi->i_lock);
    powerfs_cap_get_refs(pi, POWERFS_CAP_PIN | POWERFS_CAP_FILE_SHARED);

    pr_debug("powerfs: dir_open success, fop=%p\n", file->f_op);

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
    struct powerfs_inode_info *pi = POWERFS_I(inode);

    pr_debug("powerfs: dir_release ino=%lu\n", inode->i_ino);

    /* Cap 引用释放 (与 dir_open 对称) */
    spin_lock(&pi->i_lock);
    if (pi->i_nr_by_mode[POWERFS_FILE_MODE_RD] > 0)
        pi->i_nr_by_mode[POWERFS_FILE_MODE_RD]--;
    spin_unlock(&pi->i_lock);
    powerfs_cap_put_refs(pi, POWERFS_CAP_PIN | POWERFS_CAP_FILE_SHARED);

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
    if (!powerfs_net_is_connected())
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
        /* 没有更多条目，标记目录完整 (I_COMPLETE)
         * 对齐 : __xxx_dir_set_complete(ci) + i_flags |= I_COMPLETE. */
        spin_lock(&dpi->i_lock);
        dpi->dir_complete = true;
        dpi->i_flags |= POWERFS_I_COMPLETE;
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

    /* 获取成功，标记目录内容已完整缓存 (I_COMPLETE)
     * 对齐 : xxx_readdir_prepopulate → __xxx_dir_set_complete. */
    spin_lock(&dpi->i_lock);
    dpi->dir_complete = true;
    dpi->i_flags |= POWERFS_I_COMPLETE;
    spin_unlock(&dpi->i_lock);

    pr_debug("powerfs: fill_readdir_cache got %u entries (dir_complete=true)\n",
             dfi->cached_count);
    return 0;
}

/* ========== 目录项管理 (本地 readdir) ========== */

/**
 * powerfs_add_dir_entry - 添加目录项到链表
 *
 * 使用 dir_mutex 保护.
 * 如果同名条目已存在且标记为 deleted, 复用该条目 (un-delete + 更新元数据),
 * 避免同名重复条目堆积.
 */
int powerfs_add_dir_entry(struct inode *dir, u64 ino,
                                  unsigned int type, const char *name)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry, *existing = NULL;

    if (!S_ISDIR(dir->i_mode))
        return 0;

    mutex_lock(&dpi->dir_mutex);

    /* 检查是否已有同名条目 (可能是 deleted 状态) */
    list_for_each_entry(entry, &dpi->dir_entries, list) {
        if (strcmp(entry->name, name) == 0) {
            existing = entry;
            break;
        }
    }

    if (existing) {
        /* 复用已有条目: un-delete + 更新元数据 */
        if (existing->deleted) {
            pr_debug("powerfs: add_dir_entry UN_DELETE dir_ino=%lu name='%s' "
                    "old_ino=%llu new_ino=%llu (was deleted, now reactivated)\n",
                    dir->i_ino, name, existing->ino, ino);
        }
        existing->deleted = false;
        existing->ino = ino;
        existing->type = type;
        /* 本地 mutation 创建/复用的条目 fetch_epoch 保持 0, 不参与 stale 清理
         * (本地刚 create 但未 sync 到 Filer 的条目不应被下次 refetch 误删).
         * 待下次 refetch 见到同名时再更新为当前 fetch_epoch. */
        existing->fetch_epoch = 0;
        mutex_unlock(&dpi->dir_mutex);
        return 0;
    }

    mutex_unlock(&dpi->dir_mutex);

    /* 没有同名条目, 分配新条目 */
    entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;

    entry->ino = ino;
    entry->type = type;
    entry->deleted = false;
    entry->fetch_epoch = 0;  /* 本地新增, 未被 refetch 见过, 不参与 stale 清理 */
    strncpy(entry->name, name, POWERFS_MAX_NAME_LEN);
    entry->name[POWERFS_MAX_NAME_LEN] = '\0';

    mutex_lock(&dpi->dir_mutex);
    list_add_tail(&entry->list, &dpi->dir_entries);
    mutex_unlock(&dpi->dir_mutex);

    return 0;
}

/**
 * powerfs_remove_dir_entry - 标记目录项为已删除
 *
 * 使用 dir_mutex 保护.
 *
 * 重要: 不物理删除链表节点, 只标记 deleted=true.
 * 原因: powerfs_readdir 使用 ctx->pos 作为链表索引, 物理删除会导致
 *       后续 getdents 调用的索引偏移, 漏掉未发射的条目 (C9f bug 根因).
 *       标记删除保持链表结构稳定, readdir 跳过 deleted 条目即可.
 *       deleted 条目在以下场景被清理:
 *       - powerfs_clear_dir_entries (rmdir/evict)
 *       - powerfs_add_dir_entry 同名复用
 *       - powerfs_readdir Filer 重新拉取时 un-delete (文件被重建)
 *       - powerfs_compact_dir_entries (O-05: readdir ctx->pos==0 时阈值触发)
 */
int powerfs_remove_dir_entry(struct inode *dir, const char *name)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry;
    int total = 0, deleted_count = 0;

    if (!S_ISDIR(dir->i_mode))
        return 0;

    mutex_lock(&dpi->dir_mutex);
    list_for_each_entry(entry, &dpi->dir_entries, list) {
        total++;
        if (entry->deleted)
            deleted_count++;
        if (strcmp(entry->name, name) == 0 && !entry->deleted) {
            entry->deleted = true;
            deleted_count++;
            pr_debug("powerfs: remove_dir_entry MARK_DELETED dir_ino=%lu "
                    "name='%s' entry_ino=%llu (total=%d deleted=%d active=%d)\n",
                    dir->i_ino, name, entry->ino, total, deleted_count,
                    total - deleted_count);
            mutex_unlock(&dpi->dir_mutex);
            return 0;
        }
    }
    mutex_unlock(&dpi->dir_mutex);

    pr_warn("powerfs: remove_dir_entry NOT_FOUND dir_ino=%lu name='%s' "
            "(total=%d deleted=%d active=%d)\n",
            dir->i_ino, name, total, deleted_count, total - deleted_count);

    return -ENOENT;
}

/**
 * powerfs_clear_dir_entries - 清空目录项链表
 *
 * 使用 dir_mutex 保护
 */
void powerfs_clear_dir_entries(struct inode *dir)
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

/**
 * powerfs_compact_dir_entries - 物理清理已标记 deleted 的目录条目
 *
 * O-05: 防止长生命周期目录 (如 /tmp) 的 deleted 条目无限堆积导致内存泄漏.
 *
 * 清理条件: deleted 条目数 >= 64 且占总数 50% 以上
 * 调用时机: powerfs_readdir 入口且 ctx->pos == 0 (新读或 rewind)
 *           此时无 stale ctx->pos 需要保持, 物理删除安全
 * 锁: 调用方须持 dir_mutex
 *
 * 返回: 清理的条目数
 */
static int powerfs_compact_dir_entries(struct inode *dir)
{
    struct powerfs_inode_info *dpi = POWERFS_I(dir);
    struct powerfs_dir_entry *entry, *tmp;
    int total = 0, deleted = 0, removed = 0;

    /* 统计总数和已删除数 */
    list_for_each_entry(entry, &dpi->dir_entries, list) {
        total++;
        if (entry->deleted)
            deleted++;
    }

    /* 未达阈值, 不清理 */
    if (deleted < 64 || deleted * 2 < total)
        return 0;

    /* 物理删除所有 deleted 条目 */
    list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
        if (entry->deleted) {
            list_del_init(&entry->list);
            kfree(entry);
            removed++;
        }
    }

    pr_info("powerfs: compact_dir_entries dir_ino=%lu compacted %d/%d entries "
            "(remaining=%d)\n",
            dir->i_ino, removed, total, total - removed);

    return removed;
}

/* Phase 1: 本地 mutation 后清父目录 lease + bump shared_gen.
 * 调用时机: mkdir/rmdir/create/unlink/symlink/link/rename 网络请求成功后,
 *           在修改本地数据结构的同时清目录 lease.
 * 效果: 下次 readdir 看到 lease 过期会重新拉取, 看到自己刚加/删的项.
 *       i_shared_gen++ 使所有子 dentry 的 dir_shared_gen 失效,
 *       d_revalidate Layer 2 不再命中 → 触发 re-lookup.
 *       清 I_COMPLETE 使负 dentry 不再被信任 (目录内容已变).
 * 注意: 调用方已持 dir->i_rwsem 写锁 (VFS 保证), 此处无需额外锁. */
void powerfs_invalidate_dir_lease(struct inode *dir)
{
    struct powerfs_inode_info *dpi;

    if (!dir || !S_ISDIR(dir->i_mode))
        return;
    dpi = POWERFS_I(dir);
    /* dir_mutex 保护 dir_entries 链表; dir_lease_expire/epoch 是 atomic-like
     * 字段, 用 WRITE_ONCE 配合 readdir 的 READ_ONCE. */
    mutex_lock(&dpi->dir_mutex);
    WRITE_ONCE(dpi->dir_lease_expire, 0);
    dpi->dir_lease_epoch++;
    /* Bump i_shared_gen: 使所有子 dentry 的 dir_shared_gen 不再匹配.
     * 对齐  atomic_inc(&ci->i_shared_gen) + Rust cache.rs bump_dir_version.
     * 对齐 Rust: "Bump dir_version so stale dentries with dir_shared_gen
     * mismatch are detected." */
    atomic_inc(&dpi->i_shared_gen);
    /* 清 I_COMPLETE: 目录内容已变, 不再信任负 dentry.
     * 对齐 : __xxx_dir_clear_complete(ci). */
    dpi->i_flags &= ~POWERFS_I_COMPLETE;
    dpi->dir_complete = false;
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

    pr_debug("powerfs: readdir ENTER dir_ino=%lu ctx_pos=%lld "
             "dir_complete=%d lease_expire=%ld lease_epoch=%u\n",
             dir->i_ino, (s64)ctx->pos,
             READ_ONCE(dpi->dir_complete),
             READ_ONCE(dpi->dir_lease_expire),
             dpi->dir_lease_epoch);

    /* DEBUG: temporary info-level logging to diagnose empty readdir */
    {
        int active = 0, deleted = 0;
        struct powerfs_dir_entry *e;
        mutex_lock(&dpi->dir_mutex);
        list_for_each_entry(e, &dpi->dir_entries, list) {
            if (e->deleted) deleted++; else active++;
        }
        mutex_unlock(&dpi->dir_mutex);
        pr_info("powerfs: READDIR_DEBUG dir_ino=%lu ctx_pos=%lld "
                "dir_complete=%d lease_expire=%ld connected=%d "
                "entries: active=%d deleted=%d\n",
                dir->i_ino, (s64)ctx->pos,
                READ_ONCE(dpi->dir_complete),
                READ_ONCE(dpi->dir_lease_expire),
                powerfs_net_is_connected(),
                active, deleted);
    }

    /* O-05: 清理 deleted 条目, 防止内存泄漏.
     * 仅在 ctx->pos == 0 (新读/rewind) 时清理, 此时无 stale 位置需保持. */
    if (ctx->pos == 0) {
        mutex_lock(&dpi->dir_mutex);
        powerfs_compact_dir_entries(dir);
        mutex_unlock(&dpi->dir_mutex);
    }

    /* 处理 "." 和 ".." */
    if (ctx->pos == 0) {
        if (!dir_emit_dots(file, ctx))
            return 0;
    }

    pos = ctx->pos - 2;

    /* === Phase 1: 目录 lease fast-path ===
     * dir_complete && dir_lease_expire 未过期: 直接用本地缓存, 不发网络.
     * 30s 内重复 readdir 走纯内存 (POWERFS_DIR_LEASE_TTL).
     * lease 过期或 dir_complete=false: 清缓存重新拉取 (本地 mutation 后). */
    if (READ_ONCE(dpi->dir_complete) &&
        time_before(jiffies, READ_ONCE(dpi->dir_lease_expire))) {
        pr_debug("powerfs: readdir ino=%lu lease fast-path (cached)\n",
                 dir->i_ino);
        goto emit_cached;
    }

    /* 如果目录 lease 过期或缓存为空, 且网络可用, 从 Filer 获取目录列表.
     * lease 过期但缓存非空: 先清缓存 (避免新旧条目混合). */
    if (powerfs_net_is_connected()) {
        struct powerfs_net_dir_entry *net_entries;
        __u32 net_count = 0;
        bool has_more = false;
        char last_name[256] = "";
        int ret;
        int timeout_ms;
        bool pulled_any = false;  /* 是否已拉取到至少一页 (超时容错) */
        u64 fetch_epoch;

        /* lease 过期: 标记需要重新拉取, 但不清空 dir_entries.
         * 原因: 本地 mutation (create/mkdir) 通过 powerfs_add_dir_entry 添加的条目
         * 可能尚未出现在 Filer 的 readdir 响应中, 清空会导致这些条目丢失.
         * 网络拉取时通过 ino 去重, 旧条目保留, 新条目追加. */
        if (READ_ONCE(dpi->dir_complete) &&
            time_after(jiffies, READ_ONCE(dpi->dir_lease_expire))) {
            int total = 0, del = 0;
            struct powerfs_dir_entry *e;
            list_for_each_entry(e, &dpi->dir_entries, list) {
                total++;
                if (e->deleted) del++;
            }
            pr_debug("powerfs: readdir REFETCH dir_ino=%lu ctx_pos=%lld "
                    "lease_expired (entries: total=%d deleted=%d active=%d, "
                    "epoch=%u)\n",
                    dir->i_ino, (s64)ctx->pos, total, del,
                    total - del, dpi->dir_lease_epoch);
            WRITE_ONCE(dpi->dir_complete, false);
            WRITE_ONCE(dpi->dir_lease_expire, 0);
        }

        /* Bump fetch_epoch: 本次 refetch 的"generation". 拉取过程中遇到
         * 同名条目时把它的 fetch_epoch 更新为当前值; refetch 结束后所有
         * active 条目中 fetch_epoch < 当前值的说明 Filer 不再返回,
         * 标记 deleted (清理 stale active, 解决历史遗留 dir_entries 堆积). */
        mutex_lock(&dpi->dir_mutex);
        dpi->dir_fetch_epoch++;
        fetch_epoch = dpi->dir_fetch_epoch;
        mutex_unlock(&dpi->dir_mutex);

        net_entries = kmalloc_array(256, sizeof(*net_entries), GFP_KERNEL);
        if (!net_entries)
            return -ENOMEM;

        /* Phase 1: 短超时策略 (同 lookup). */
        timeout_ms = powerfs_net_pick_timeout(POWERFS_READDIR_TIMEOUT_MS);

        /* 从 Filer 获取目录条目 (分页循环直到获取全部) */
        do {
            ret = powerfs_net_readdir_timeout(dir->i_ino, last_name, 256,
                                              net_entries, 256,
                                              &net_count, &has_more,
                                              timeout_ms);
            if (ret < 0) {
                /* Phase 1: 瞬态错误处理.
                 * - 已拉取部分页: 用已拉取的, 不返回错误 (best-effort).
                 * - 一页都没拉到:
                 *   * 缓存有内容 (lease 过期但未清): 用旧缓存
                 *   * 缓存为空: 返回 -EAGAIN 让 VFS/应用层重试 */
                if (pulled_any) {
                    pr_warn("powerfs: readdir ino=%lu partial fetch err=%d, "
                            "using entries pulled so far\n",
                            dir->i_ino, ret);
                    break;
                }
                if (!list_empty(&dpi->dir_entries)) {
                    pr_warn("powerfs: readdir ino=%lu net err=%d, "
                            "using stale cache\n", dir->i_ino, ret);
                    break;
                }
                if (ret == -ETIMEDOUT || ret == -ENOTCONN ||
                    ret == -ESHUTDOWN) {
                    pr_warn("powerfs: readdir ino=%lu transient err=%d "
                            "(timeout_ms=%d), return -EAGAIN\n",
                            dir->i_ino, ret, timeout_ms);
                    kfree(net_entries);
                    return -EAGAIN;
                }
                pr_warn("powerfs: readdir ino=%lu net error: %d\n",
                        dir->i_ino, ret);
                kfree(net_entries);
                return ret;
            }
            pulled_any = true;

            /* 将网络条目添加到本地缓存 */
            mutex_lock(&dpi->dir_mutex);
            for (__u32 i = 0; i < net_count; i++) {
                struct powerfs_dir_entry *de;
                struct powerfs_net_dir_entry *ne = &net_entries[i];

                /* 更新 last_name 用于分页 — 必须在每个条目上更新,
                 * 不能只在非重复条目上更新. 否则当所有条目都是缓存中
                 * 已有的重复条目时 (如本地 create 已添加), last_name
                 * 不变, 导致分页循环重复请求同一页 (infinite loop). */
                strncpy(last_name, ne->name, sizeof(last_name) - 1);
                last_name[sizeof(last_name) - 1] = '\0';

                /* 检查是否已存在 (按名称去重, 不能用 ino — hardlink
                 * 的多个目录项共享同一 inode 但名称不同) */
                bool found = false;
                list_for_each_entry(de, &dpi->dir_entries, list) {
                    if (strcmp(de->name, ne->name) == 0) {
                        found = true;
                        /* 如果本地标记为 deleted 但 Filer 仍返回该条目,
                         * 说明文件被重建 (同名新 inode), un-delete 并更新元数据. */
                        if (de->deleted) {
                            pr_debug("powerfs: readdir REFETCH_UN_DELETE "
                                    "dir_ino=%lu name='%s' old_ino=%llu "
                                    "new_ino=%llu (Filer still has it, "
                                    "file was re-created)\n",
                                    dir->i_ino, ne->name, de->ino,
                                    ne->ino);
                            de->deleted = false;
                            de->ino = ne->ino;
                            de->type = ne->mode & S_IFMT;
                        }
                        /* 标记本次 refetch 见过此条目 (Filer 仍返回该 name),
                         * 防止下方 stale-active 清理把它误删. */
                        de->fetch_epoch = fetch_epoch;
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
                de->deleted = false;
                de->fetch_epoch = fetch_epoch;
                strncpy(de->name, ne->name, POWERFS_MAX_NAME_LEN);
                de->name[POWERFS_MAX_NAME_LEN] = '\0';
                list_add_tail(&de->list, &dpi->dir_entries);
            }
            mutex_unlock(&dpi->dir_mutex);
        } while (has_more && net_count > 0);

        kfree(net_entries);

        /* Stale-active 清理: 本次 refetch (fetch_epoch) 未被 Filer 返回的
         * active 条目说明 Filer 已删除该 name (其他客户端 unlink/rmdir),
         * 标记 deleted. 跳过:
         *   - 已 deleted 的 (保持链表位置稳定, 等待 compact_dir_entries 阈值清理)
         *   - pulled_any=false 的 (一页都没拉到, 不应基于空响应清掉全部本地条目)
         *   - fetch_epoch == 0 的旧条目 (首次 refetch 前创建的本地 entry, 不应清掉)
         * 注: 本地刚 create 但未 sync 到 Filer 的条目 fetch_epoch == 0 (从未被
         * refetch 见过), 不会被本次清理影响 — 仍保留, 等下次 refetch 见到后再更新. */
        if (pulled_any) {
            int swept = 0, kept_local = 0;
            struct powerfs_dir_entry *e;
            mutex_lock(&dpi->dir_mutex);
            list_for_each_entry(e, &dpi->dir_entries, list) {
                if (e->deleted)
                    continue;
                if (e->fetch_epoch == 0) {
                    kept_local++;
                    continue;
                }
                if (e->fetch_epoch != fetch_epoch) {
                    e->deleted = true;
                    swept++;
                }
            }
            mutex_unlock(&dpi->dir_mutex);
            if (swept > 0) {
                pr_info("powerfs: readdir SWEEP_STALE dir_ino=%lu "
                        "fetch_epoch=%llu swept=%d active entries not seen "
                        "by Filer (kept_local_unsynced=%d)\n",
                        dir->i_ino, fetch_epoch, swept, kept_local);
            }
        }

        /* 拉取成功 (或部分成功): 设置 dir_complete + I_COMPLETE + dir_lease_expire.
         * 部分成功时也设 dir_complete (避免反复部分拉取), 下次 lease 过期再补.
         * 对齐 : __xxx_dir_set_complete + I_COMPLETE. */
        WRITE_ONCE(dpi->dir_complete, true);
        dpi->i_flags |= POWERFS_I_COMPLETE;
        WRITE_ONCE(dpi->dir_lease_expire, jiffies + POWERFS_DIR_LEASE_TTL);
    }

    /* DEBUG: log state right before emit */
    {
        int active = 0, deleted = 0;
        struct powerfs_dir_entry *e;
        mutex_lock(&dpi->dir_mutex);
        list_for_each_entry(e, &dpi->dir_entries, list) {
            if (e->deleted) deleted++; else active++;
        }
        mutex_unlock(&dpi->dir_mutex);
        pr_info("powerfs: READDIR_EMIT_DEBUG dir_ino=%lu ctx_pos=%lld "
                "dir_complete=%d entries: active=%d deleted=%d\n",
                dir->i_ino, (s64)ctx->pos,
                READ_ONCE(dpi->dir_complete), active, deleted);
    }

emit_cached:

    /*
     * 第一阶段: 在锁内复制目录项到临时缓冲区
     */
    {
        struct dentry_emit_entry {
            u64 ino;
            unsigned int type;
            unsigned short namelen;
            char name[POWERFS_MAX_NAME_LEN + 1];
        } *buf;
        int count = 0;
        int max = 256;
        int i;

        buf = kmalloc_array(max, sizeof(*buf), GFP_KERNEL);
        if (!buf) {
            /* 分配失败，直接在锁内 emit (回退方案) */
            int fb_skipped_deleted = 0, fb_emitted = 0;
            mutex_lock(&dpi->dir_mutex);
            list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
                unsigned char d_type;
                if (pos > 0) { pos--; continue; }
                /* 跳过已标记删除的条目, 但仍消耗位置槽以保持 ctx->pos 索引稳定 */
                if (entry->deleted) { ctx->pos++; fb_skipped_deleted++; continue; }
                switch (entry->type) {
                case S_IFREG:  d_type = DT_REG; break;
                case S_IFDIR:  d_type = DT_DIR; break;
                case S_IFLNK:  d_type = DT_LNK; break;
                default:       d_type = DT_UNKNOWN; break;
                }
                if (!dir_emit(ctx, entry->name, strlen(entry->name),
                              entry->ino, d_type)) {
                    pr_debug("powerfs: readdir EMIT_FALLBACK dir_ino=%lu "
                             "ctx_pos=%lld emitted=%d skipped_deleted=%d "
                             "(buf full, returning early)\n",
                             dir->i_ino, (s64)ctx->pos, fb_emitted,
                             fb_skipped_deleted);
                    mutex_unlock(&dpi->dir_mutex);
                    return 0;
                }
                ctx->pos++;
                fb_emitted++;
            }
            pr_debug("powerfs: readdir EMIT_FALLBACK_DONE dir_ino=%lu "
                     "ctx_pos=%lld emitted=%d skipped_deleted=%d (EOF)\n",
                     dir->i_ino, (s64)ctx->pos, fb_emitted,
                     fb_skipped_deleted);
            mutex_unlock(&dpi->dir_mutex);
            return 0;
        }

        {
            int skipped_deleted = 0;
            loff_t pos_start = ctx->pos;

            mutex_lock(&dpi->dir_mutex);

            list_for_each_entry_safe(entry, tmp, &dpi->dir_entries, list) {
                if (count >= max)
                    break;
                if (pos > 0) {
                    pos--;
                    continue;
                }
                /* 跳过已标记删除的条目, 但仍消耗位置槽以保持 ctx->pos 索引稳定.
                 * 这确保 rm -rf 在 unlink 子条目后, 后续 getdents 不会因
                 * 链表节点移除而导致 ctx->pos 偏移、漏掉未发射的条目 (C9f bug). */
                if (entry->deleted) {
                    ctx->pos++;
                    skipped_deleted++;
                    continue;
                }
                buf[count].ino = entry->ino;
                buf[count].type = entry->type;
                buf[count].namelen = strlen(entry->name);
                memcpy(buf[count].name, entry->name, buf[count].namelen + 1);
                count++;
            }

            mutex_unlock(&dpi->dir_mutex);

            if (skipped_deleted > 0)
                pr_debug("powerfs: readdir EMIT_SKIP dir_ino=%lu "
                        "pos_start=%lld pos_end=%lld copied=%d "
                        "skipped_deleted=%d (buf_max=%d)\n",
                        dir->i_ino, (s64)pos_start, (s64)ctx->pos,
                        count, skipped_deleted, max);
        }

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
                pr_debug("powerfs: readdir EMIT_PARTIAL dir_ino=%lu "
                         "ctx_pos=%lld emitted=%d/%d (userspace buf full)\n",
                         dir->i_ino, (s64)ctx->pos, i, count);
                kfree(buf);
                return 0;
            }

            ctx->pos++;
        }

        if (count > 0)
            pr_debug("powerfs: readdir EMIT_DONE dir_ino=%lu "
                     "ctx_pos=%lld emitted=%d (EOF or buf exhausted)\n",
                     dir->i_ino, (s64)ctx->pos, count);

        kfree(buf);
    }

    return 0;
}

/* 目录文件操作表 - 使用本地链表实现 readdir
 *
 * P0-4 fix: 补齐 fsync/lock/flock/ioctl, 与文件 fops 对应函数共用实现.
 *   - lock/flock: 目录级文件锁 (NFS 导出、Maildir、git index.lock 等会用)
 *   - fsync: 事务场景 fsync(dir_fd) 确保 create/rename 持久化
 *   - ioctl: 预留扩展 (ACL ioctl 等, 但 ACL 通 xattr 路径完成) */
const struct file_operations powerfs_dir_operations = {
    .open           = powerfs_dir_open,
    .release        = powerfs_dir_release,
    .iterate_shared = powerfs_readdir,
    .llseek         = generic_file_llseek,
    .read           = generic_read_dir,   /* read() on dir returns -EISDIR (POSIX) */
    .fsync          = powerfs_dir_fsync,  /* P0-4 fix: fsync(dir_fd) 同步元数据+dirty xattr */
    .lock           = powerfs_lock,       /* P0-4 fix: 目录 POSIX 记录锁 */
    .flock          = powerfs_flock,      /* P0-4 fix: 目录 BSD flock */
    .unlocked_ioctl = powerfs_ioctl,      /* P1-3 扩展实现 */
    .compat_ioctl   = compat_ptr_ioctl,   /* P1-3 32-bit compat */
    .setlease       = simple_nosetlease,  /* P2-4: 明确拒绝 F_SETLEASE delegations，
                                           * 防止 silent data stale (远端写不 break local lease) */
};
