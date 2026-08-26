/*
 * powerfs_xattr.c - split from powerfs_fs.c
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

/* ========== xattr 回调 (simple_xattr L1 cache + Filer Raft L2) ==========
 *
 * xattr 持久化架构:
 *   - L1 缓存: 内存 simple_xattr (pi->xattrs), 用于加速 GET/LIST
 *   - L2 持久化: Filer Raft (通过 powerfs-net TLV: SET_XATTR=0x38/GET_XATTR=0x39/
 *                 REMOVE_XATTR=0x3a/LIST_XATTR=0x3b)
 *
 * 一致性策略:
 *   - GET: 先 L1 cache, miss → net getxattr → 成功则回填 L1
 *   - SET: 同步 net setxattr → Filer Raft 确认 → 更新 L1 cache
 *   - REMOVE: 同步 net removexattr → 成功 → 从 L1 cache 清除
 *   - LIST: 先 L1 cache 非空直接用; 为空 → net listxattr 回填 L1
 *   - CapFlush XATTR_EXCL: L1 条目逐条重推 (recall/fsync 场景兜底)
 *
 * Kernel 6.17 移除了 inode_operations.setxattr/getxattr/removexattr,
 * 改用 xattr_handler 注册到 super_block.s_xattr.
 * 支持 user.* / trusted.* / security.* / system.posix_acl_* 前缀.
 */

static int powerfs_xattr_handler_get(const struct xattr_handler *handler,
                                     struct dentry *unused, struct inode *inode,
                                     const char *name, void *buffer, size_t size)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    __u64 shard_id;
    const char *full_name;
    size_t name_len;
    int ret;

    full_name = xattr_full_name(handler, name);

    /* 快速路径: L1 simple_xattr cache hit */
    ret = simple_xattr_get(&pi->xattrs, full_name, buffer, size);
    if (ret != -ENODATA)
        return ret;  /* hit (>=0) 或 -ERANGE 等直接返回 */

    /* 慢速路径: cache miss → net getxattr 查询 Filer */
    shard_id = powerfs_calc_shard_id(inode->i_ino);
    name_len = strlen(full_name);

    if (buffer && size > 0) {
        size_t got_len = 0;
        ret = powerfs_net_getxattr(shard_id, inode->i_ino,
                                   full_name, name_len,
                                   (__u8 *)buffer, size, &got_len);
        if (ret == 0) {
            struct simple_xattr *old;
            old = simple_xattr_set(&pi->xattrs, full_name, buffer, got_len, 0);
            if (!IS_ERR(old))
                simple_xattr_free(old);
            return (int)got_len;
        }
        return ret;
    } else {
        /* buffer=NULL/size=0: VFS probe 语义, 返回 value 长度 */
        size_t got_len = 0;
        __u8 stackbuf[256];
        __u8 *tmpbuf = stackbuf;
        size_t tmpcap = sizeof(stackbuf);

        ret = powerfs_net_getxattr(shard_id, inode->i_ino,
                                   full_name, name_len,
                                   tmpbuf, tmpcap, &got_len);
        if (ret == -ERANGE)
            return (int)got_len;
        if (ret == 0) {
            struct simple_xattr *old;
            old = simple_xattr_set(&pi->xattrs, full_name, tmpbuf, got_len, 0);
            if (!IS_ERR(old))
                simple_xattr_free(old);
            return (int)got_len;
        }
        return ret;
    }
}

static int powerfs_xattr_handler_set(const struct xattr_handler *handler,
                                     struct mnt_idmap *idmap,
                                     struct dentry *unused, struct inode *inode,
                                     const char *name, const void *value,
                                     size_t size, int flags)
{
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    __u64 shard_id;
    const char *full_name;
    size_t name_len;
    struct simple_xattr *old_xattr;
    int ret;

    (void)idmap;

    if (size > XATTR_SIZE_MAX)
        return -E2BIG;

    full_name = xattr_full_name(handler, name);
    name_len = strlen(full_name);
    shard_id = powerfs_calc_shard_id(inode->i_ino);

    /* value==NULL && size==0: VFS removexattr 语义 */
    if (!value && size == 0) {
        ret = powerfs_net_removexattr(shard_id, inode->i_ino,
                                      full_name, name_len);
        if (ret < 0)
            return ret;

        old_xattr = simple_xattr_set(&pi->xattrs, full_name, NULL, 0, 0);
        if (!IS_ERR(old_xattr))
            simple_xattr_free(old_xattr);

        powerfs_cap_mark_dirty(pi, POWERFS_CAP_XATTR_EXCL);
        pi->i_xattr_version++;
        return 0;
    }

    /* SETXATTR: 先 Filer 持久化 (Raft), 成功后才更新 L1 cache */
    ret = powerfs_net_setxattr(shard_id, inode->i_ino,
                               full_name, name_len,
                               (const __u8 *)value, size);
    if (ret < 0)
        return ret;

    old_xattr = simple_xattr_set(&pi->xattrs, full_name, value, size, flags);
    if (IS_ERR(old_xattr)) {
        pr_warn_ratelimited("powerfs: setxattr ino=%lu name=%s "
                            "simple_xattr_set cache fill failed: %ld (ignored)\n",
                            inode->i_ino, full_name, PTR_ERR(old_xattr));
    } else {
        simple_xattr_free(old_xattr);
    }

    powerfs_cap_mark_dirty(pi, POWERFS_CAP_XATTR_EXCL);
    pi->i_xattr_version++;
    return 0;
}

ssize_t powerfs_listxattr(struct dentry *dentry, char *buffer,
                                 size_t size)
{
    struct inode *inode = d_inode(dentry);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    ssize_t l1_ret;

    /* 快速路径: L1 cache 非空时直接返回 */
    l1_ret = simple_xattr_list(inode, &pi->xattrs, buffer, size);
    if (l1_ret > 0 || (l1_ret == 0 && size == 0))
        return l1_ret;

    /* 慢速路径: L1 cache 空 → net listxattr 拉取 + 回填 L1 */
    {
        __u64 shard_id = powerfs_calc_shard_id(inode->i_ino);
        char *list_buf;
        size_t list_cap = (size > 0) ? size : 4096;
        size_t list_len = 0;
        int ret;

        if (buffer && size > 0) {
            list_buf = buffer;
        } else {
            list_buf = kmalloc(list_cap, GFP_KERNEL);
            if (!list_buf)
                return -ENOMEM;
        }

        ret = powerfs_net_listxattr(shard_id, inode->i_ino,
                                    list_buf, list_cap, &list_len);
        if (ret == -ERANGE && !(buffer && size > 0)) {
            kfree(list_buf);
            list_cap = list_len;
            list_buf = kmalloc(list_cap, GFP_KERNEL);
            if (!list_buf)
                return -ENOMEM;
            list_len = 0;
            ret = powerfs_net_listxattr(shard_id, inode->i_ino,
                                        list_buf, list_cap, &list_len);
        }
        if (ret < 0) {
            if (list_buf != buffer)
                kfree(list_buf);
            return ret;
        }

        /* 回填 L1: 遍历 NUL-separated keys, 逐个 getxattr + simple_xattr_set */
        {
            size_t pos = 0;
            while (pos < list_len) {
                const char *key = list_buf + pos;
                size_t klen = strlen(key);
                size_t vcap = XATTR_SIZE_MAX;
                __u8 *vbuf;
                size_t vlen = 0;

                vbuf = kmalloc(vcap, GFP_KERNEL);
                if (vbuf) {
                    if (powerfs_net_getxattr(shard_id, inode->i_ino,
                                             key, klen,
                                             vbuf, vcap, &vlen) == 0) {
                        struct simple_xattr *old;
                        old = simple_xattr_set(&pi->xattrs, key, vbuf, vlen, 0);
                        if (!IS_ERR(old))
                            simple_xattr_free(old);
                    }
                    kfree(vbuf);
                }
                pos += klen + 1;
            }
        }

        if (!buffer && size == 0) {
            kfree(list_buf);
            return simple_xattr_list(inode, &pi->xattrs, NULL, 0);
        }
        if (list_buf == buffer)
            return (ssize_t)list_len;

        kfree(list_buf);
        return simple_xattr_list(inode, &pi->xattrs, NULL, 0);
    }
}

static const struct xattr_handler powerfs_security_xattr_handler = {
    .prefix = XATTR_SECURITY_PREFIX,
    .get = powerfs_xattr_handler_get,
    .set = powerfs_xattr_handler_set,
};

static const struct xattr_handler powerfs_trusted_xattr_handler = {
    .prefix = XATTR_TRUSTED_PREFIX,
    .get = powerfs_xattr_handler_get,
    .set = powerfs_xattr_handler_set,
};

static const struct xattr_handler powerfs_user_xattr_handler = {
    .prefix = XATTR_USER_PREFIX,
    .get = powerfs_xattr_handler_get,
    .set = powerfs_xattr_handler_set,
};

const struct xattr_handler * const powerfs_xattr_handlers[] = {
    &powerfs_security_xattr_handler,
    &powerfs_trusted_xattr_handler,
    &powerfs_user_xattr_handler,
    NULL
};
