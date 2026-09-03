/*
 * powerfs_addr.c - split from powerfs_fs.c
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

/* ========== 地址空间操作 (page cache) ========== */

/* powerfs_read_folio 已移除 (Stage C): read 路径由 netfs_read_folio +
 * powerfs_netfs_issue_read 接管, 参照 xxx_aops.read_folio = netfs_read_folio. */

/* ========== Stage C: writepages 批量异步写入 ==========
 *
 * 参照 xxx_writepages_start 模式, 批量收集脏页减少 work item 数量:
 *   1. writepages 遍历脏页, 收集到 powerfs_writepage_work 批量结构
 *   2. batch 满 (sbi->write_batch_pages 页) 或遍历完成时提交 work item
 *   3. workqueue 线程串行发送 batch 内所有页面
 *   4. max_active=4 限制全局并发 worker 数
 *
 * 批量大小可配 (mount option write_batch_kb):
 *   - 默认 64KB (16 pages): TCP 网络
 *   - ROCE 推荐 1MB (256 pages): 高吞吐
 *   - 最大 stripe size 64MB (16384 pages): 单 stripe 一次提交
 *
 * 性能: 1MB 文件, batch=1MB → 1 个 work item (vs 旧方案 256 个)
 *
 * work 结构体使用 kvmalloc 单块分配 (header + 3 个动态数组), 因为
 * max 可达 16384 页 (数组 ~384KB, 超过 kmalloc 上限). */

struct powerfs_writepage_work {
    struct work_struct work;
    struct inode *inode;
    int num_pages;
    int max_pages;
    struct page **pages;     /* max_pages 项, 紧随结构体后 */
    loff_t *offsets;         /* max_pages 项, 紧随 pages 后 */
    size_t *counts;          /* max_pages 项, 紧随 offsets 后 */
    /* 异步提交计数: 跟踪尚未完成的 needle 写入. work_fn 持有 +1 ref,
     * 每个 needle ctx 完成时 dec. 归零时执行 final cleanup (iput/dec/kvfree).
     * 防止提前归零: work_fn 在所有 ctx 提交后才 dec 自己的 ref. */
    atomic_t pending_needles;
    /* RCU 延迟释放头: work_struct 在 wpw 中, work_fn 返回后 workqueue 仍
     * 访问 work->data. 用 call_rcu 延迟 kvfree 到 RCU 宽限期后, 确保
     * workqueue 对 work_struct 的访问已完成. */
    struct rcu_head rcu;
};

/*
 * powerfs_alloc_write_batch - 分配批量写 work 结构 (含动态数组)
 *
 * 单块 kvmalloc: [header][pages[]][offsets[]][counts[]]
 * 大批量 (如 64MB=16384 页) 时数组 ~384KB, 需 kvmalloc 自动回退 vmalloc.
 */
static struct powerfs_writepage_work *
powerfs_alloc_write_batch(int max_pages, gfp_t gfp)
{
    struct powerfs_writepage_work *batch;
    size_t arr_sz = (size_t)max_pages *
                    (sizeof(struct page *) + sizeof(loff_t) + sizeof(size_t));
    size_t sz = sizeof(*batch) + arr_sz;

    batch = kvmalloc(sz, gfp | __GFP_ZERO);
    if (!batch)
        return NULL;

    batch->max_pages = max_pages;
    batch->num_pages = 0;
    batch->pages = (struct page **)(batch + 1);
    batch->offsets = (loff_t *)(batch->pages + max_pages);
    batch->counts = (size_t *)(batch->offsets + max_pages);
    return batch;
}

/*
 * ========== Stage D: page writeback 异步提交模式 ==========
 *
 * 旧同步实现 (powerfs_net_write_needle/read_needle) 在 workqueue 线程内
 * wait_for_completion 等待响应 (最长 30s/请求), 多个 needle 串行 →
 * workqueue lockup 598s. 新实现将 read_needle + write_needle 改为异步提交:
 *
 *   writepage_work_fn (workqueue 线程, 快速返回):
 *     1. 按 needle_id 分组 batch 内页面
 *     2. 每个 needle 组: alloc ctx (含 2MB needle_buf), 获取 lease (同步, 快速),
 *        submit read_needle_async (非阻塞入队)
 *     3. work_fn 返回, workqueue 线程立即释放
 *
 *   read_cb (调度器 RX 上下文, 响应到达时):
 *     1. 读取结果填入 ctx->needle_buf (scheduler 已完成 memcpy)
 *     2. 合并脏页数据到 needle_buf (read-modify-write 的 modify)
 *     3. submit write_needle_async (非阻塞入队)
 *
 *   write_cb (调度器 RX 上下文, 写响应到达时):
 *     1. end_page_writeback + put_page (清除该 needle 的所有页)
 *     2. free ctx (needle_buf + ctx)
 *     3. atomic_dec pending_needles; 归零则 final_cleanup (iput/dec/kvfree wpw)
 *
 * 引用计数 (pending_needles):
 *   - 初始 = num_needle_groups + 1 (work_fn 持有 +1 ref)
 *   - 每个 ctx 完成 (成功或失败) dec 1
 *   - work_fn 在所有 ctx 提交后 dec 自己的 +1
 *   - 归零时 final_cleanup, 防止提前归零 (work_fn 仍在访问 wpw)
 *
 * 超时兜底: 异步请求用 delayed_work (system_wq) 在 deadline 到期时以
 *   -ETIMEDOUT 完成, 防止 page 永久滞留 PageWriteback → hung task.
 */

/* Per-needle 异步上下文 (一个 needle 组对应一个 ctx) */
struct powerfs_wb_ctx {
    struct powerfs_writepage_work *wpw;   /* 所属 batch */
    int needle_start_idx;                  /* wpw->pages 中的起始索引 */
    int needle_end_idx;                    /* 结束索引 (exclusive) */
    __u64 volume_id;
    __u64 needle_id;
    __u8 *needle_buf;                      /* 2MB (RMW) 或 extent_size (blob) */
    __u32 needle_len;                      /* needle 有效数据长度 / blob size */
    __u64 blob_offset;                     /* WriteNeedleBlob: offset within needle */
    char lease_token[64];
    size_t lease_token_len;
    /* 持久缓冲区: 异步请求的 req_body / resp_body, 存活到 callback 触发 */
    __u8 req_body[256];
    __u8 resp_body[64];
};

/* 前向声明 */
static void powerfs_wb_final_cleanup(struct powerfs_writepage_work *wpw);
static void powerfs_wb_fail_pages(struct powerfs_wb_ctx *ctx, int err);
static int powerfs_wb_read_cb(struct powerfs_request *req);
static int powerfs_wb_write_cb(struct powerfs_request *req);

/* powerfs_wb_needle_full_coverage - 检查脏页是否完全覆盖 needle 范围
 *
 * 当 needle 组内脏页完全覆盖 [needle_start, needle_start + max_needle_len)
 * 且 max_needle_len == POWERFS_CHUNK_SIZE 时, 返回 true. 此时 RMW 的 read
 * 阶段是多余的 (所有旧数据都将被覆盖), 可跳过 read 直接 write.
 *
 * 要求 max_needle_len == POWERFS_CHUNK_SIZE 的原因: 当 i_size 未对齐到
 * chunk 边界时 (max_needle_len < POWERFS_CHUNK_SIZE), server needle 可能
 * 保留 truncate 前的 stale 数据. 直接 write 短 needle 只覆盖前 N 字节,
 * 不会清除 stale 数据. 仅当写入完整 1MB chunk 时, server needle 被完全
 * 替换, 无 stale 风险.
 */
static bool powerfs_wb_needle_full_coverage(struct powerfs_writepage_work *wpw,
                                            int group_start, int group_end,
                                            loff_t needle_start,
                                            __u32 max_needle_len)
{
    loff_t need_end = needle_start + max_needle_len;
    loff_t covered = needle_start;
    int i;

    if (max_needle_len != POWERFS_CHUNK_SIZE)
        return false;

    for (i = group_start; i < group_end; i++) {
        loff_t offset = wpw->offsets[i];
        size_t count = wpw->counts[i];

        if (count == 0)
            continue;

        /* Page must be within needle range */
        if (offset < needle_start || offset + count > need_end)
            return false;

        /* Check contiguity: no gap before this page */
        if (offset > covered)
            return false;

        /* Extend covered range */
        if (offset + count > covered)
            covered = offset + count;
    }

    return covered >= need_end;
}

/* powerfs_wb_submit_write_direct - 跳过 read, 直接从脏页填充 needle_buf 并提交 write
 *
 * 当脏页完全覆盖 needle 范围时调用. needle_buf 已 zero-init, 只需将脏页
 * memcpy 到对应位置, 然后提交 write_needle_async.
 *
 * 此函数在 work_fn 上下文 (process context) 执行, 可安全调用 kmap.
 * 提交成功后 ctx 由 write_cb 异步清理; 提交失败由调用方清理.
 *
 * 返回 0 = 提交成功 (ctx 由 write_cb 异步释放); <0 = 提交失败 (调用方清理).
 */
static int powerfs_wb_submit_write_direct(struct powerfs_wb_ctx *ctx)
{
    struct powerfs_writepage_work *wpw = ctx->wpw;
    struct inode *inode = wpw->inode;
    int j, ret;

    /* 填充 needle_buf: 将脏页数据 memcpy 到 needle_buf 对应位置 */
    for (j = ctx->needle_start_idx; j < ctx->needle_end_idx; j++) {
        struct page *page = wpw->pages[j];
        loff_t offset = wpw->offsets[j];
        size_t count = wpw->counts[j];
        size_t offset_in_needle;

        if (count == 0)
            continue;

        offset_in_needle = offset % POWERFS_CHUNK_SIZE;
        {
            char *kaddr = kmap_local_page(page);
            memcpy(ctx->needle_buf + offset_in_needle, kaddr, count);
            kunmap_local(kaddr);
        }
    }

    /* 全覆盖: needle_len = POWERFS_CHUNK_SIZE */
    ctx->needle_len = POWERFS_CHUNK_SIZE;

    pr_debug("powerfs: WB_WRITE_DIRECT ino=%lu nid=%llu len=%u (skip read)\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            ctx->needle_len);

    ret = powerfs_net_write_needle_async(
        ctx->volume_id, ctx->needle_id, inode->i_ino,
        ctx->needle_buf, ctx->needle_len,
        ctx->lease_token_len > 0 ? ctx->lease_token : NULL,
        ctx->lease_token_len,
        ctx->req_body, sizeof(ctx->req_body),
        ctx->resp_body, sizeof(ctx->resp_body),
        30000, powerfs_wb_write_cb, ctx);

    return ret;
}

/* powerfs_wb_submit_blob_extents - 对非全覆盖的 needle 组, 按 extent 提交 WriteNeedleBlob
 *
 * 遍历 [group_start, group_end) 内的脏页, 将连续页合并为 extent,
 * 对每个 extent 创建独立 ctx 并提交 powerfs_net_write_needle_blob_async.
 *
 * 每个 extent 的 ctx 独立分配 (kzalloc ctx + kmalloc buf), 由 write_cb 异步释放.
 * Volume Server coalescer 合并同一 needle 的多次 partial write.
 *
 * pending_needles 调整:
 *   调用前: 该 needle 组已被 first pass 计为 1
 *   函数内: 若提交了 N 个 extent, atomic_add(N-1) 补偿差额
 *   每个 extent 成功: write_cb 异步 dec; 失败: 函数内同步 dec
 *
 * 返回: 提交的 extent 数量 (0 = 全部失败或无有效页)
 */
static int powerfs_wb_submit_blob_extents(struct powerfs_writepage_work *wpw,
                                          int group_start, int group_end,
                                          __u64 volume_id, __u64 needle_id,
                                          struct inode *inode)
{
    int i = group_start;
    int extents_submitted = 0;

    while (i < group_end) {
        int ext_start;
        loff_t ext_offset;
        size_t ext_size = 0;
        size_t token_len = 0;
        struct powerfs_wb_ctx *ctx;
        __u8 *buf;
        int ret;
        int j;

        /* Skip count==0 pages */
        while (i < group_end && wpw->counts[i] == 0)
            i++;
        if (i >= group_end)
            break;
        ext_start = i;

        /* Find contiguous extent */
        ext_offset = wpw->offsets[i];
        ext_size = wpw->counts[i];
        i++;
        while (i < group_end && wpw->counts[i] > 0 &&
               wpw->offsets[i] == wpw->offsets[i - 1] + wpw->counts[i - 1]) {
            ext_size += wpw->counts[i];
            i++;
        }

        /* Allocate ctx */
        ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
        if (!ctx) {
            struct powerfs_wb_ctx fail_ctx = {
                .wpw = wpw,
                .needle_start_idx = ext_start,
                .needle_end_idx = i,
            };
            powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
            /* Account for this failed extent */
            if (extents_submitted == 0 && i >= group_end) {
                /* No successful submissions; the group's +1 in
                 * pending_needles needs to be decremented */
                if (atomic_dec_and_test(&wpw->pending_needles))
                    powerfs_wb_final_cleanup(wpw);
            }
            continue;
        }

        /* Allocate extent data buffer */
        buf = kmalloc(ext_size, GFP_NOFS);
        if (!buf) {
            struct powerfs_wb_ctx fail_ctx = {
                .wpw = wpw,
                .needle_start_idx = ext_start,
                .needle_end_idx = i,
            };
            kfree(ctx);
            powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
            if (extents_submitted == 0 && i >= group_end) {
                if (atomic_dec_and_test(&wpw->pending_needles))
                    powerfs_wb_final_cleanup(wpw);
            }
            continue;
        }

        /* Copy page data into buffer */
        {
            size_t copied = 0;
            for (j = ext_start; j < i; j++) {
                char *kaddr;
                if (wpw->counts[j] == 0)
                    continue;
                kaddr = kmap_local_page(wpw->pages[j]);
                memcpy(buf + copied, kaddr, wpw->counts[j]);
                kunmap_local(kaddr);
                copied += wpw->counts[j];
            }
        }

        /* Setup ctx */
        ctx->wpw = wpw;
        ctx->needle_start_idx = ext_start;
        ctx->needle_end_idx = i;
        ctx->volume_id = volume_id;
        ctx->needle_id = needle_id;
        ctx->needle_buf = buf;
        ctx->needle_len = ext_size;
        ctx->blob_offset = ext_offset % POWERFS_CHUNK_SIZE;

        /* Get lease */
        if (powerfs_get_lease_token(inode, ext_offset,
                                    ctx->lease_token, &token_len)) {
            pr_warn("powerfs: blob lease ino=%lu, continuing without\n",
                    inode->i_ino);
        }
        ctx->lease_token_len = token_len;

        /* Adjust pending_needles for additional extents */
        if (extents_submitted > 0)
            atomic_inc(&wpw->pending_needles);

        /* Submit WriteNeedleBlob */
        pr_debug("powerfs: WB_BLOB_SUBMIT ino=%lu nid=%llu off=%llu len=%zu\n",
                inode->i_ino, (unsigned long long)needle_id,
                (unsigned long long)ctx->blob_offset, ext_size);

        ret = powerfs_net_write_needle_blob_async(
            volume_id, needle_id, inode->i_ino,
            ctx->blob_offset,
            buf, ext_size,
            ctx->lease_token_len > 0 ? ctx->lease_token : NULL,
            ctx->lease_token_len,
            ctx->req_body, sizeof(ctx->req_body),
            ctx->resp_body, sizeof(ctx->resp_body),
            30000, powerfs_wb_write_cb, ctx);

        if (ret) {
            pr_warn("powerfs: blob submit ino=%lu nid=%llu off=%llu err=%d\n",
                    inode->i_ino, (unsigned long long)needle_id,
                    (unsigned long long)ctx->blob_offset, ret);
            powerfs_wb_fail_pages(ctx, ret);
            kfree(buf);
            kfree(ctx);
            /* Decrement: this extent's submission failed */
            if (atomic_dec_and_test(&wpw->pending_needles))
                powerfs_wb_final_cleanup(wpw);
        } else {
            extents_submitted++;
        }
    }

    /* If no extents were submitted (all failed), the group's +1 in
     * pending_needles needs to be decremented */
    if (extents_submitted == 0) {
        if (atomic_dec_and_test(&wpw->pending_needles))
            powerfs_wb_final_cleanup(wpw);
    }

    return extents_submitted;
}

/* powerfs_wb_fail_pages - 失败一个 ctx 范围内的所有页面
 *
 * 用于 ctx 分配失败或异步提交失败时, 同步清理页面 (在 work_fn 上下文).
 * 调用方负责 dec pending_needles + free ctx. */
static void powerfs_wb_fail_pages(struct powerfs_wb_ctx *ctx, int err)
{
    struct powerfs_writepage_work *wpw = ctx->wpw;
    int j;

    for (j = ctx->needle_start_idx; j < ctx->needle_end_idx; j++) {
        struct page *p = wpw->pages[j];
        if (wpw->counts[j] == 0)
            continue;
        if (err < 0)
            mapping_set_error(p->mapping, err);
        end_page_writeback(p);
        put_page(p);
    }
}

/* powerfs_wb_free_rcu - RCU 回调: 延迟释放 wpw 内存
 *
 * work_struct 在 wpw 中, work_fn 返回后 workqueue 子系统仍访问 work->data
 * (清除 WORK_BUSY_PENDING 等标志). 直接 kvfree 会导致 use-after-free
 * (assign_work NULL deref oops). 用 call_rcu 延迟到 RCU 宽限期后释放,
 * 确保 workqueue 对 work_struct 的访问已完成. */
static void powerfs_wb_free_rcu(struct rcu_head *rcu)
{
    struct powerfs_writepage_work *wpw =
        container_of(rcu, struct powerfs_writepage_work, rcu);
    kvfree(wpw);
}

/* powerfs_wb_final_cleanup - 所有 needle 完成后的最终清理
 *
 * 释放 batch 级资源: inode 引用, wb_in_flight 计数.
 * wpw 内存通过 call_rcu 延迟释放 (work_struct 生命周期约束).
 * 由最后一个完成的 ctx (或 work_fn 的自身 ref) 触发.
 *
 * 并发安全: VFS writeback 子系统通过 PAGECACHE_TAG_DIRTY →
 * PAGECACHE_TAG_WRITEBACK 的原子转换保证同一页面不会被并发 writeback.
 * 不同 needle 组的 WriteNeedleBlob 请求天然独立, 同一 needle 的并发
 * partial write 由 Volume Server coalescer 安全合并. */
static void powerfs_wb_final_cleanup(struct powerfs_writepage_work *wpw)
{
    struct inode *inode = wpw->inode;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);

    pr_debug("powerfs: WB_FINAL_CLEANUP ino=%lu wb_in_flight=%d\n",
            inode->i_ino, atomic_read(&sbi->wb_in_flight));
    atomic_dec(&sbi->wb_in_flight);

    iput(inode);
    /* 延迟释放 wpw: work_struct 在 wpw 中, workqueue 在 work_fn 返回后
     * 仍访问 work->data. call_rcu 等待 RCU 宽限期后释放. */
    call_rcu(&wpw->rcu, powerfs_wb_free_rcu);
}

/* powerfs_wb_read_cb - read_needle 异步完成回调
 *
 * 由 powerfs_req_complete 在调度器 RX 上下文 (process context) 调用.
 * 1. 处理读取结果 (NOT_FOUND=新 needle, 其他错误=失败页面)
 * 2. 合并脏页数据到 needle_buf (read-modify-write 的 modify)
 * 3. 提交 write_needle_async 进入第二阶段
 *
 * 注意: 此函数在调度器线程上下文执行, 应避免长时间阻塞. 合并操作是
 * N×4KB memcpy (16页=64KB ≈ 32µs), 可接受. write_needle_async 仅入队, 不阻塞. */
static int powerfs_wb_read_cb(struct powerfs_request *req)
{
    struct powerfs_wb_ctx *ctx = req->priv;
    struct powerfs_writepage_work *wpw = ctx->wpw;
    struct inode *inode = wpw->inode;
    int ret;

    pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu err=%d status=%u pages=[%d,%d) needle_len=%u\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            req->error, req->resp_status,
            ctx->needle_start_idx, ctx->needle_end_idx, ctx->needle_len);

    /* 解析读取结果 */
    if (req->error < 0) {
        /* 网络错误 (断连/超时/截断): 失败页面 */
        pr_warn("powerfs: wb read_cb ino=%lu nid=%llu net_err=%d\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                req->error);
        powerfs_wb_fail_pages(ctx, req->error);
        powerfs_request_free(req);
        kvfree(ctx->needle_buf);
        kfree(ctx);
        if (atomic_dec_and_test(&wpw->pending_needles))
            powerfs_wb_final_cleanup(wpw);
        return 0;
    }

    if (req->resp_status == POWERFS_NET_STATUS_ERR_NOT_FOUND) {
        /* needle 不存在 (新文件首次写): 全零 needle, 正常继续 */
        ctx->needle_len = 0;
    } else if (req->resp_status != POWERFS_NET_STATUS_OK) {
        /* 其他服务端错误: 失败页面 */
        int err = net_status_to_errno(req->resp_status);
        pr_warn("powerfs: wb read_cb ino=%lu nid=%llu status=%u err=%d\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                req->resp_status, err);
        powerfs_wb_fail_pages(ctx, err);
        powerfs_request_free(req);
        kvfree(ctx->needle_buf);
        kfree(ctx);
        if (atomic_dec_and_test(&wpw->pending_needles))
            powerfs_wb_final_cleanup(wpw);
        return 0;
    } else {
        /* 读取成功: scheduler 已将 needle 内容写入 ctx->needle_buf */
        ctx->needle_len = (__u32)req->resp_data_len;
    }

    pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu after_resp status=%u resp_data_len=%llu needle_len=%u\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            req->resp_status, (unsigned long long)req->resp_data_len,
            ctx->needle_len);

    powerfs_request_free(req);  /* 释放 read 请求 */

    /* K2-8: The Volume Server may return empty or partial needle data,
     * especially right after inline→Flat migration when the server hasn't
     * committed the data yet. If we proceed with RMW using only the server
     * data, any gaps between dirty pages are zeroed → data corruption.
     *
     * Optimization: fill from page cache ONLY when the server returned
     * empty or partial data (needle_len < max_needle_len). When the server
     * has complete data, dirty pages will be overlaid in the modify step
     * below, and clean pages are already consistent with the server — no
     * need to traverse the pagecache. This avoids 256 find_get_page+kmap
     * per needle in the common writeback path.
     *
     * Pages in the page cache are at least as new as the server data (they
     * were either read from the server or written by this client), so
     * overwriting server data with page cache data is safe.
     *
     * We fill from needle_start to min(i_size, needle_start + CHUNK_SIZE),
     * covering the full needle range (not just the dirty page range). */
    {
        loff_t first_offset = wpw->offsets[ctx->needle_start_idx];
        loff_t needle_start = first_offset - (first_offset % POWERFS_CHUNK_SIZE);
        loff_t isize = i_size_read(inode);
        __u32 max_needle_len = (__u32)min_t(loff_t, isize - needle_start,
                                            POWERFS_CHUNK_SIZE);
        loff_t fill_end = min_t(loff_t, isize,
                                needle_start + POWERFS_CHUNK_SIZE);
        pgoff_t start_pg = needle_start >> PAGE_SHIFT;
        pgoff_t end_pg = (fill_end + PAGE_SIZE - 1) >> PAGE_SHIFT;
        pgoff_t pg;
        int pages_found = 0;

        pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu filling range %llu-%llu (i_size=%llu) from pagecache, server_needle_len=%u\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                (unsigned long long)needle_start,
                (unsigned long long)fill_end,
                (unsigned long long)isize, ctx->needle_len);

        /* Skip pagecache fill when server already has complete data.
         * Dirty pages will be overlaid in the modify step below. */
        if (ctx->needle_len >= max_needle_len) {
            pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu skip pagecache fill (server_len=%u >= max=%u)\n",
                    inode->i_ino, (unsigned long long)ctx->needle_id,
                    ctx->needle_len, max_needle_len);
            goto skip_pagecache_fill;
        }

        for (pg = start_pg; pg < end_pg; pg++) {
            struct page *pgc = find_get_page(inode->i_mapping, pg);
            if (pgc) {
                size_t off_in_needle = (pg << PAGE_SHIFT) - needle_start;
                size_t copy_len = min_t(size_t, PAGE_SIZE,
                                        fill_end - (pg << PAGE_SHIFT));
                char *kaddr = kmap_local_page(pgc);
                memcpy(ctx->needle_buf + off_in_needle, kaddr, copy_len);
                kunmap_local(kaddr);
                put_page(pgc);
                pages_found++;
                if (off_in_needle + copy_len > ctx->needle_len)
                    ctx->needle_len = off_in_needle + copy_len;
            }
        }
        pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu pagecache fill done: found %d pages, needle_len=%u\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                pages_found, ctx->needle_len);
    skip_pagecache_fill:
        ; /* label target */
    }

    /* K2-10: After truncate, the server needle may still have stale data
     * beyond i_size. When the file is extended again, pages beyond the
     * old truncate point may not be in the page cache. If we write the
     * needle with the stale server data, subsequent reads will return
     * the old data instead of zeros.
     *
     * Fix: truncate needle_len to i_size and zero out data beyond i_size.
     * This ensures the server needle doesn't retain stale data from
     * before the truncate. The buffer was zeroed initially, so we just
     * need to adjust needle_len. */
    {
        loff_t isize = i_size_read(inode);
        loff_t first_offset = wpw->offsets[ctx->needle_start_idx];
        loff_t needle_start = first_offset - (first_offset % POWERFS_CHUNK_SIZE);
        __u32 max_needle_len = (__u32)min_t(loff_t, isize - needle_start,
                                             POWERFS_CHUNK_SIZE);

        if (ctx->needle_len > max_needle_len) {
            /* K2-10/K2-12: After truncate, the server needle still has stale
             * data beyond i_size. The Volume Server does NOT truncate the
             * needle when a shorter write is submitted — it only overwrites
             * the first N bytes. So we must submit the FULL original server
             * needle length (with zeros beyond i_size) to overwrite the
             * stale data. The memset below zeros out the stale region. */
            pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu zeroing stale data beyond i_size (needle_len=%u, i_size=%llu, needle_start=%llu) — keeping full needle_len to overwrite server\n",
                    inode->i_ino, (unsigned long long)ctx->needle_id,
                    ctx->needle_len,
                    (unsigned long long)isize,
                    (unsigned long long)needle_start);
            /* Zero out data beyond i_size to prevent stale data */
            if (max_needle_len < POWERFS_CHUNK_SIZE)
                memset(ctx->needle_buf + max_needle_len, 0,
                       ctx->needle_len - max_needle_len);
            /* Do NOT reduce needle_len: submit the full server needle length
             * so the server overwrites stale data with zeros. */
        } else if (ctx->needle_len < max_needle_len) {
            /* K2-11: File may have been extended via ftruncate() to a sparse
             * region beyond the current server needle length. The new region
             * has no pages in the page cache (no data was written there), so
             * the pagecache fill loop above did not extend needle_len.
             *
             * The needle_buf was zero-initialized (kvzalloc), so the gap is
             * already zeros - which is the correct content for the sparse
             * region. Extend needle_len to cover the full range up to i_size
             * so the server needle size matches i_size and subsequent reads
             * of the extended region return zeros instead of short reads. */
            pr_debug("powerfs: WB_READ_CB ino=%lu nid=%llu extending needle from %u to %u (i_size=%llu, needle_start=%llu, sparse region zero-filled)\n",
                    inode->i_ino, (unsigned long long)ctx->needle_id,
                    ctx->needle_len, max_needle_len,
                    (unsigned long long)isize,
                    (unsigned long long)needle_start);
            ctx->needle_len = max_needle_len;
        }
    }

    /* 合并脏页数据到 needle_buf (read-modify-write 的 modify) */
    {
        int j;
        for (j = ctx->needle_start_idx; j < ctx->needle_end_idx; j++) {
            struct page *page = wpw->pages[j];
            loff_t offset = wpw->offsets[j];
            size_t count = wpw->counts[j];
            size_t offset_in_needle;

            if (count == 0)
                continue;

            offset_in_needle = offset % POWERFS_CHUNK_SIZE;
            {
                char *kaddr = kmap_local_page(page);
                memcpy(ctx->needle_buf + offset_in_needle, kaddr, count);
                kunmap_local(kaddr);
            }
            if (offset_in_needle + count > ctx->needle_len)
                ctx->needle_len = offset_in_needle + count;
        }
    }

    /* 提交 write_needle_async (第二阶段) */
    pr_debug("powerfs: WB_WRITE_SUBMIT ino=%lu nid=%llu len=%u\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            ctx->needle_len);
    ret = powerfs_net_write_needle_async(
        ctx->volume_id, ctx->needle_id, inode->i_ino,
        ctx->needle_buf, ctx->needle_len,
        ctx->lease_token_len > 0 ? ctx->lease_token : NULL,
        ctx->lease_token_len,
        ctx->req_body, sizeof(ctx->req_body),
        ctx->resp_body, sizeof(ctx->resp_body),
        30000, powerfs_wb_write_cb, ctx);

    if (ret) {
        /* 提交失败: callback 不会触发, 手动清理 */
        pr_warn("powerfs: wb read_cb ino=%lu nid=%llu write submit failed: %d\n",
                inode->i_ino, (unsigned long long)ctx->needle_id, ret);
        powerfs_wb_fail_pages(ctx, ret);
        kvfree(ctx->needle_buf);
        kfree(ctx);
        if (atomic_dec_and_test(&wpw->pending_needles))
            powerfs_wb_final_cleanup(wpw);
    }

    return 0;
}

/* powerfs_wb_write_cb - write_needle 异步完成回调
 *
 * 写响应到达时调用, 清除该 needle 所有页的 PageWriteback, 释放 ctx.
 * 最后一个 needle 完成时触发 final_cleanup. */
static int powerfs_wb_write_cb(struct powerfs_request *req)
{
    struct powerfs_wb_ctx *ctx = req->priv;
    struct powerfs_writepage_work *wpw = ctx->wpw;
    struct inode *inode = wpw->inode;
    int err = 0;

    pr_info("powerfs: WB_WRITE_CB ino=%lu nid=%llu err=%d status=%u pages=[%d,%d)\n",
            inode->i_ino, (unsigned long long)ctx->needle_id,
            req->error, req->resp_status,
            ctx->needle_start_idx, ctx->needle_end_idx);

    if (req->error < 0) {
        err = req->error;
    } else if (req->resp_status != POWERFS_NET_STATUS_OK) {
        err = net_status_to_errno(req->resp_status);
    }

    if (err)
        pr_warn("powerfs: wb write_cb ino=%lu nid=%llu err=%d status=%u\n",
                inode->i_ino, (unsigned long long)ctx->needle_id,
                err, req->resp_status);

    /* 完成该 needle 的所有页面 */
    {
        int j;
        for (j = ctx->needle_start_idx; j < ctx->needle_end_idx; j++) {
            struct page *p = wpw->pages[j];
            if (wpw->counts[j] == 0)
                continue;
            if (err)
                mapping_set_error(p->mapping, err);
            end_page_writeback(p);
            put_page(p);
        }
    }

    powerfs_request_free(req);
    kvfree(ctx->needle_buf);
    kfree(ctx);

    if (atomic_dec_and_test(&wpw->pending_needles))
        powerfs_wb_final_cleanup(wpw);

    return 0;
}

/*
 * powerfs_writepage_work_fn - 批量异步写 workqueue 函数 (异步提交模式)
 *
 * 按 needle_id 分组, 每组根据脏页覆盖率选择提交路径:
 *   1. 全覆盖: powerfs_wb_submit_write_direct — 跳过 read, 直接从脏页填充
 *      needle_buf 并提交 write_needle (短期优化, 消除读放大).
 *   2. 非全覆盖: powerfs_wb_submit_blob_extents — 按 extent 合并脏页,
 *      提交 write_needle_blob partial write (中期优化, 消除 RMW 读放大).
 *
 * work_fn 在所有 ctx 提交后立即返回 (不阻塞等待网络响应), 由 write_cb
 * 异步回调完成实际写入并清除 PageWriteback.
 */
static void powerfs_writepage_work_fn(struct work_struct *work)
{
    struct powerfs_writepage_work *wpw =
        container_of(work, struct powerfs_writepage_work, work);
    struct inode *inode = wpw->inode;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    int i;
    int num_groups = 0;
    int group_start = -1;
    __u64 group_needle_id = 0;
    __u64 group_volume_id = 0;

    pr_info("powerfs: WP_START ino=%lu npages=%d\n",
            inode->i_ino, wpw->num_pages);

    if (powerfs_net_is_stopping())
        goto fail_all;

    /* 第一遍: 完成 count==0 的空页, 统计 needle 组数.
     * K3: 按 offset 调用 powerfs_locate_chunk 获取 (volume_id, needle_id),
     *     分组判断改为 (needle_id, volume_id) 元组, 确保 Stripe 模式下
     *     不同 volume 的 chunk 分到不同组. */
    for (i = 0; i < wpw->num_pages; i++) {
        struct page *page = wpw->pages[i];
        loff_t offset = wpw->offsets[i];
        size_t count = wpw->counts[i];
        __u64 needle_id, volume_id;
        int loc_ret;

        cond_resched();

        if (count == 0) {
            /* 空页: 直接完成, 不参与 needle 分组 */
            end_page_writeback(page);
            put_page(page);
            continue;
        }

        spin_lock(&pi->i_lock);
        loc_ret = powerfs_locate_chunk(pi, offset, &volume_id, &needle_id);
        spin_unlock(&pi->i_lock);
        if (loc_ret) {
            pr_warn("powerfs: writepage locate ino=%lu offset=%lld err=%d\n",
                    inode->i_ino, offset, loc_ret);
            mapping_set_error(page->mapping, loc_ret);
            end_page_writeback(page);
            put_page(page);
            continue;
        }

        if (group_start < 0) {
            /* 开始新组 */
            group_start = i;
            group_needle_id = needle_id;
            group_volume_id = volume_id;
            num_groups++;
        } else if (needle_id != group_needle_id ||
                   volume_id != group_volume_id) {
            /* needle/volume 切换: 当前组结束, 开始新组 */
            group_start = i;
            group_needle_id = needle_id;
            group_volume_id = volume_id;
            num_groups++;
        }
    }

    /* 若无有效页面 (全为 count==0 或 locate 失败), 直接清理 */
    if (num_groups == 0) {
        atomic_dec(&sbi->wb_in_flight);
        iput(inode);
        /* 延迟释放: 同 final_cleanup, work_fn 上下文不能直接 kvfree wpw. */
        call_rcu(&wpw->rcu, powerfs_wb_free_rcu);
        return;
    }

    /* 设置计数: num_groups + 1 (work_fn 持有 +1 ref, 防止提前归零) */
    atomic_set(&wpw->pending_needles, num_groups + 1);

    /* 第二遍: 为每个 needle 组创建 ctx 并异步提交 read_needle.
     * K3: 每个页面持锁 locate 获取 (volume_id, needle_id), 组切换时用
     *     cur_volume_id/cur_needle_id 设置 ctx (Stripe 模式下每组 volume 不同). */
    {
        int cur_start = -1;
        __u64 cur_needle_id = 0;
        __u64 cur_volume_id = 0;

        for (i = 0; i < wpw->num_pages; i++) {
            loff_t offset = wpw->offsets[i];
            size_t count = wpw->counts[i];
            __u64 needle_id, volume_id;
            int loc_ret;

            if (count == 0)
                continue;

            spin_lock(&pi->i_lock);
            loc_ret = powerfs_locate_chunk(pi, offset, &volume_id, &needle_id);
            spin_unlock(&pi->i_lock);
            if (loc_ret) {
                /* locate 失败: 跳过该页 (第一遍已 end_page_writeback) */
                continue;
            }

            if (cur_start < 0) {
                cur_start = i;
                cur_needle_id = needle_id;
                cur_volume_id = volume_id;
            } else if (needle_id != cur_needle_id ||
                       volume_id != cur_volume_id) {
                /* 提交 [cur_start, i) 这一组 */
                struct powerfs_wb_ctx *ctx;
                loff_t group_offset;
                loff_t needle_start;
                loff_t isize;
                __u32 max_needle_len;
                size_t token_len = 0;
                int ret;

                group_offset = wpw->offsets[cur_start];
                needle_start = group_offset -
                    (group_offset % POWERFS_CHUNK_SIZE);
                isize = i_size_read(inode);
                max_needle_len = (__u32)min_t(loff_t,
                    isize - needle_start, POWERFS_CHUNK_SIZE);

                /* 中期优化: 非全覆盖时走 WriteNeedleBlob partial write,
                 * 消除 RMW 读放大; 函数内部自管理 ctx 分配与 pending_needles */
                if (!powerfs_wb_needle_full_coverage(wpw, cur_start, i,
                                                     needle_start,
                                                     max_needle_len)) {
                    powerfs_wb_submit_blob_extents(wpw, cur_start, i,
                                                   cur_volume_id,
                                                   cur_needle_id, inode);
                    goto next_group;
                }

                /* 全覆盖: 跳过 read, 直接从脏页填充 needle_buf 并提交 write */
                ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
                if (!ctx) {
                    struct powerfs_wb_ctx fail_ctx = {
                        .wpw = wpw,
                        .needle_start_idx = cur_start,
                        .needle_end_idx = i,
                    };
                    powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                    atomic_dec(&wpw->pending_needles);
                    goto next_group;
                }

                ctx->needle_buf = kvzalloc(POWERFS_CHUNK_SIZE, GFP_NOFS);
                if (!ctx->needle_buf) {
                    struct powerfs_wb_ctx fail_ctx = {
                        .wpw = wpw,
                        .needle_start_idx = cur_start,
                        .needle_end_idx = i,
                    };
                    kfree(ctx);
                    powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                    atomic_dec(&wpw->pending_needles);
                    goto next_group;
                }
                memset(ctx->needle_buf, 0, POWERFS_CHUNK_SIZE);

                ctx->wpw = wpw;
                ctx->needle_start_idx = cur_start;
                ctx->needle_end_idx = i;
                ctx->volume_id = cur_volume_id;
                ctx->needle_id = cur_needle_id;
                ctx->needle_len = 0;

                /* 获取 lease (同步, 快速: ensure_lease 有快速路径) */
                if (powerfs_get_lease_token(inode, group_offset,
                                            ctx->lease_token,
                                            &token_len)) {
                    pr_warn("powerfs: writepage lease ino=%lu stripe=%llu, continuing without lease\n",
                            inode->i_ino,
                            (unsigned long long)(group_offset / POWERFS_STRIPE_SIZE
                                                 * POWERFS_STRIPE_SIZE));
                }
                ctx->lease_token_len = token_len;

                ret = powerfs_wb_submit_write_direct(ctx);
                if (ret) {
                    powerfs_wb_fail_pages(ctx, ret);
                    kvfree(ctx->needle_buf);
                    kfree(ctx);
                    if (atomic_dec_and_test(&wpw->pending_needles))
                        powerfs_wb_final_cleanup(wpw);
                }

next_group:
                cur_start = i;
                cur_needle_id = needle_id;
                cur_volume_id = volume_id;
            }
        }

        /* 提交最后一组 [cur_start, num_pages) */
        if (cur_start >= 0) {
            struct powerfs_wb_ctx *ctx;
            loff_t group_offset;
            loff_t needle_start;
            loff_t isize;
            __u32 max_needle_len;
            size_t token_len = 0;
            int ret;

            group_offset = wpw->offsets[cur_start];
            needle_start = group_offset -
                (group_offset % POWERFS_CHUNK_SIZE);
            isize = i_size_read(inode);
            max_needle_len = (__u32)min_t(loff_t,
                isize - needle_start, POWERFS_CHUNK_SIZE);

            /* 中期优化: 非全覆盖时走 WriteNeedleBlob partial write */
            if (!powerfs_wb_needle_full_coverage(wpw, cur_start,
                                                 wpw->num_pages,
                                                 needle_start,
                                                 max_needle_len)) {
                powerfs_wb_submit_blob_extents(wpw, cur_start,
                                               wpw->num_pages,
                                               cur_volume_id,
                                               cur_needle_id, inode);
                goto last_group_done;
            }

            /* 全覆盖: 跳过 read, 直接从脏页填充 needle_buf 并提交 write */
            ctx = kzalloc(sizeof(*ctx), GFP_NOFS);
            if (!ctx) {
                struct powerfs_wb_ctx fail_ctx = {
                    .wpw = wpw,
                    .needle_start_idx = cur_start,
                    .needle_end_idx = wpw->num_pages,
                };
                powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                atomic_dec(&wpw->pending_needles);
                goto last_group_done;
            }

            ctx->needle_buf = kvzalloc(POWERFS_CHUNK_SIZE, GFP_NOFS);
            if (!ctx->needle_buf) {
                struct powerfs_wb_ctx fail_ctx = {
                    .wpw = wpw,
                    .needle_start_idx = cur_start,
                    .needle_end_idx = wpw->num_pages,
                };
                kfree(ctx);
                powerfs_wb_fail_pages(&fail_ctx, -ENOMEM);
                atomic_dec(&wpw->pending_needles);
                goto last_group_done;
            }
            memset(ctx->needle_buf, 0, POWERFS_CHUNK_SIZE);

            ctx->wpw = wpw;
            ctx->needle_start_idx = cur_start;
            ctx->needle_end_idx = wpw->num_pages;
            ctx->volume_id = cur_volume_id;
            ctx->needle_id = cur_needle_id;
            ctx->needle_len = 0;

            if (powerfs_get_lease_token(inode, group_offset,
                                        ctx->lease_token,
                                        &token_len)) {
                pr_warn("powerfs: writepage final lease ino=%lu stripe=%llu, continuing without lease\n",
                        inode->i_ino,
                        (unsigned long long)(group_offset / POWERFS_STRIPE_SIZE
                                             * POWERFS_STRIPE_SIZE));
            }
            ctx->lease_token_len = token_len;

            ret = powerfs_wb_submit_write_direct(ctx);
            if (ret) {
                powerfs_wb_fail_pages(ctx, ret);
                kvfree(ctx->needle_buf);
                kfree(ctx);
                if (atomic_dec_and_test(&wpw->pending_needles))
                    powerfs_wb_final_cleanup(wpw);
            }
        }
    }

last_group_done:
    /* work_fn 自身 ref: 所有 ctx 已提交 (或失败已处理), dec 自身引用.
     * 若所有 ctx 已完成 (极端竞态: 提交后立即回调), 此处归零触发 final_cleanup. */
    if (atomic_dec_and_test(&wpw->pending_needles))
        powerfs_wb_final_cleanup(wpw);

    pr_debug("powerfs: WP_SUBMIT_DONE ino=%lu groups=%d\n",
            inode->i_ino, num_groups);
    return;  /* workqueue 线程立即释放 */

fail_all:
    for (i = 0; i < wpw->num_pages; i++) {
        mapping_set_error(wpw->pages[i]->mapping, -EIO);
        end_page_writeback(wpw->pages[i]);
        put_page(wpw->pages[i]);
    }
    atomic_dec(&sbi->wb_in_flight);
    iput(inode);
    /* 延迟释放: 同 final_cleanup, work_fn 上下文不能直接 kvfree wpw
     * (work_struct 生命周期约束). */
    call_rcu(&wpw->rcu, powerfs_wb_free_rcu);
}

/*
 * powerfs_writepages - 批量 writeback 入口
 *
 * 自己遍历脏页 (filemap_get_folios_tag), 批量收集到 work item.
 * 替代 VFS 默认的 write_cache_pages + writepage 逐页模式.
 *
 * 并发控制 (参考 Ceph ceph_writepages_start):
 *   - max_active 限制全局并发 worker (workqueue 级)
 *   - wb_in_flight 全局 in-flight 上限 (throttle)
 *   - 无 per-inode mutex: 不同 needle 组的 WriteNeedleBlob 天然并行
 *     同一页不被并发处理 (VFS page tag 原子转换保证)
 *     同一 needle 的并发 partial write 由 Volume Server coalescer 合并
 */
int powerfs_writepages(struct address_space *mapping,
                               struct writeback_control *wbc)
{
    struct inode *inode = mapping->host;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct folio_batch fbatch;
    pgoff_t index = wbc->range_start >> PAGE_SHIFT;
    pgoff_t end = wbc->range_end >> PAGE_SHIFT;
    struct powerfs_writepage_work *batch = NULL;
    int batch_pages = sbi->write_batch_pages;
    int ret = 0;

    pr_info("powerfs: WPAGES ino=%lu range=%llu-%llu nr_to_write=%ld sync_mode=%d placement=%u\n",
            inode->i_ino, wbc->range_start, wbc->range_end, wbc->nr_to_write,
            wbc->sync_mode, pi->placement);

    /* 目录无数据页: 跳过 writeback 避免无意义的网络 I/O.
     * 根目录 (ino=1) 的元数据修改 (nlink/mtime) 由 write_inode 同步,
     * 不走 page cache writeback 路径. */
    if (S_ISDIR(inode->i_mode)) {
        pr_debug("powerfs: WPAGES skip directory ino=%lu\n", inode->i_ino);
        return 0;
    }

    /* K2: Inline 文件不走 Volume Server writeback.
     * 数据已在 write_end 中同步到 inline_data 缓冲,
     * close 时通过 UPDATE_INODE 提交到 Filer.
     * 这里清理脏页标记, 让 writeback 认为已完成.
     *
     * ROOT46: 必须用 folio_clear_dirty_for_io + start/end_writeback
     * 完整流程, 而非 folio_clear_dirty. 后者只清 PG_dirty 标志位,
     * 不清 xarray 的 PAGECACHE_TAG_DIRTY tag, 导致 refresh_work 的
     * mapping_tagged(PAGECACHE_TAG_DIRTY) 误判为有脏页, 跳过 page
     * cache invalidate, 其他客户端写入后本地读到旧数据. */
    if (POWERFS_I(inode)->placement == POWERFS_PLACEMENT_INLINE) {
        struct folio_batch fbatch2;
        pgoff_t idx2 = wbc->range_start >> PAGE_SHIFT;
        pgoff_t end2 = wbc->range_end >> PAGE_SHIFT;

        pr_debug("powerfs: WPAGES INLINE ino=%lu, clean dirty pages\n", inode->i_ino);
        folio_batch_init(&fbatch2);
        while (filemap_get_folios_tag(mapping, &idx2, end2,
                                       PAGECACHE_TAG_DIRTY, &fbatch2) > 0) {
            int i;
            for (i = 0; i < folio_batch_count(&fbatch2); i++) {
                struct folio *f = fbatch2.folios[i];
                folio_lock(f);
                /* 清 PG_dirty → start_writeback 清 xarray DIRTY tag
                 * 并设 WRITEBACK tag → end_writeback 清 WRITEBACK tag.
                 * 无实际 I/O (INLINE 数据已在 write_end 同步到 inline_data). */
                if (folio_clear_dirty_for_io(f)) {
                    folio_start_writeback(f);
                    folio_end_writeback(f);
                }
                folio_unlock(f);
                folio_put(f);
            }
            folio_batch_init(&fbatch2);
            if (idx2 > end2)
                break;
        }
        return 0;
    }

    if (powerfs_net_is_stopping()) {
        pr_debug("powerfs: WPAGES net stopping, skip ino=%lu\n", inode->i_ino);
        return 0;
    }

    /* 并发限制: 防止过多 work item 同时阻塞在网络 I/O 导致 workqueue lockup.
     * 如果已有太多 in-flight work item, 让 VFS 稍后重试. */
    if (atomic_read(&sbi->wb_in_flight) >= POWERFS_WB_MAX_IN_FLIGHT) {
        pr_debug("powerfs: WPAGES throttled ino=%lu in_flight=%d\n",
                inode->i_ino, atomic_read(&sbi->wb_in_flight));
        wbc->pages_skipped += wbc->nr_to_write;
        return 0;
    }

    /* 无 per-inode writeback mutex: 参考 Ceph ceph_writepages_start,
     * 不取任何 per-inode 锁。VFS writeback 子系统通过 page tag 原子转换
     * (DIRTY → WRITEBACK) 保证同一页面不被并发 writeback; 不同 needle
     * 组的 WriteNeedleBlob 请求天然独立, 可并行 in-flight。 */

    folio_batch_init(&fbatch);

    while (index <= end) {
        int nr_pages, i;

        nr_pages = filemap_get_folios_tag(mapping, &index, end,
                                           PAGECACHE_TAG_DIRTY, &fbatch);
        if (!nr_pages) {
            pr_debug("powerfs: WPAGES no dirty pages, index=%lu end=%lu\n",
                    index, end);
            break;
        }
        pr_debug("powerfs: WPAGES found %d dirty pages, index=%lu\n",
                nr_pages, index);

        for (i = 0; i < nr_pages; i++) {
            struct page *page = folio_page(fbatch.folios[i], 0);
            loff_t offset;
            size_t count = PAGE_SIZE;
            u64 cur_needle_idx;

            lock_page(page);
            if (!PageDirty(page)) {
                pr_debug("powerfs: WPAGES page not dirty, skip\n");
                unlock_page(page);
                continue;
            }

            clear_page_dirty_for_io(page);

            offset = page_offset(page);
            /* 按 needle (chunk) 边界分组: 当页面的 chunk_idx 变化时,
             * 提交当前 batch, 确保同一 needle 的所有页面在同一个 batch
             * 中完成 read-modify-write. 避免多个 batch 并发对同一 needle
             * 做 RMW 导致数据覆盖 (1MB 文件 16 个 batch 并发覆盖问题). */
            cur_needle_idx = offset / POWERFS_CHUNK_SIZE;
            if (batch && batch->num_pages > 0) {
                u64 prev_needle_idx = batch->offsets[batch->num_pages - 1]
                                      / POWERFS_CHUNK_SIZE;
                if (cur_needle_idx != prev_needle_idx) {
                    /* needle 边界变化: 提交当前 batch */
                    pr_info("powerfs: WPAGES SUBMIT ino=%lu batch npages=%d needle_idx=%llu offset=%lld-%lld\n",
                            inode->i_ino, batch->num_pages, prev_needle_idx,
                            batch->offsets[0],
                            batch->offsets[batch->num_pages - 1] + batch->counts[batch->num_pages - 1]);
                    atomic_inc(&sbi->wb_in_flight);
                    queue_work(sbi->writeback_wq, &batch->work);
                    batch = NULL;
                }
            }

            pr_debug("powerfs: WPAGES page ino=%lu offset=%lld i_size=%lld\n",
                    inode->i_ino, offset, i_size_read(inode));
            if (offset >= i_size_read(inode)) {
                pr_debug("powerfs: WPAGES offset >= i_size, SKIP page\n");
                unlock_page(page);
                continue;
            }
            if (offset + count > i_size_read(inode))
                count = i_size_read(inode) - offset;

            /* 分配 batch (如果当前没有 pending) */
            if (!batch) {
                struct inode *grabbed;
                batch = powerfs_alloc_write_batch(batch_pages, GFP_NOFS);
                if (!batch) {
                    redirty_page_for_writepage(wbc, page);
                    unlock_page(page);
                    continue;
                }
                grabbed = igrab(inode);
                if (!grabbed) {
                    /* inode 正在被 evict (I_FREEING), 无法提交 writeback.
                     * 释放 batch, redirty 页面. */
                    kvfree(batch);
                    batch = NULL;
                    redirty_page_for_writepage(wbc, page);
                    unlock_page(page);
                    continue;
                }
                INIT_WORK(&batch->work, powerfs_writepage_work_fn);
                batch->inode = grabbed;
            }

            /* 添加页面到 batch */
            get_page(page);
            batch->pages[batch->num_pages] = page;
            batch->offsets[batch->num_pages] = offset;
            batch->counts[batch->num_pages] = count;
            batch->num_pages++;

            set_page_writeback(page);
            unlock_page(page);

            /* batch 满了，提交到 workqueue */
            if (batch->num_pages >= batch_pages) {
                pr_info("powerfs: WPAGES FULL ino=%lu batch npages=%d offset=%lld-%lld\n",
                        inode->i_ino, batch->num_pages,
                        batch->offsets[0],
                        batch->offsets[batch->num_pages - 1] + batch->counts[batch->num_pages - 1]);
                atomic_inc(&sbi->wb_in_flight);
                queue_work(sbi->writeback_wq, &batch->work);
                batch = NULL;
            }

            wbc->nr_to_write--;
            if (wbc->nr_to_write <= 0)
                goto done;
        }
        folio_batch_release(&fbatch);
        cond_resched();
    }

done:
    folio_batch_release(&fbatch);

    /* 提交剩余的 batch */
    if (batch) {
        if (batch->num_pages > 0) {
            atomic_inc(&sbi->wb_in_flight);
            queue_work(sbi->writeback_wq, &batch->work);
        } else {
            iput(batch->inode);
            kvfree(batch);
        }
    }

    return ret;
}

/*
 * powerfs_writepage - 单页 writeback (fallback, VFS 内部路径使用)
 *
 * writepages 注册后, VFS 优先调用 writepages. writepage 仅作为 fallback:
 *   - migrate_pages 等内核内部路径可能直接调用 writepage
 *   - 保持与旧接口兼容
 */
int powerfs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct inode *inode = page->mapping->host;
    struct super_block *sb = inode->i_sb;
    struct powerfs_sb_info *sbi = POWERFS_SB_INFO(sb);
    struct powerfs_writepage_work *wpw;
    loff_t offset = page_offset(page);
    size_t count = PAGE_SIZE;

    if (powerfs_net_is_stopping()) {
        redirty_page_for_writepage(wbc, page);
        unlock_page(page);
        return 0;
    }

    if (offset >= i_size_read(inode)) {
        unlock_page(page);
        return 0;
    }
    if (offset + count > i_size_read(inode))
        count = i_size_read(inode) - offset;

    /* 无 per-inode mutex: 与 powerfs_writepages 一致, 参考 Ceph. */

    wpw = powerfs_alloc_write_batch(1, GFP_NOFS);
    if (!wpw) {
        redirty_page_for_writepage(wbc, page);
        unlock_page(page);
        return 0;
    }

    INIT_WORK(&wpw->work, powerfs_writepage_work_fn);
    wpw->inode = igrab(inode);
    if (!wpw->inode) {
        /* inode 正在被 evict (I_FREEING), 无法提交 writeback.
         * 释放 wpw, redirty 页面. */
        kvfree(wpw);
        redirty_page_for_writepage(wbc, page);
        unlock_page(page);
        return 0;
    }
    get_page(page);
    wpw->pages[0] = page;
    wpw->offsets[0] = offset;
    wpw->counts[0] = count;
    wpw->num_pages = 1;

    set_page_writeback(page);
    unlock_page(page);
    atomic_inc(&sbi->wb_in_flight);
    queue_work(sbi->writeback_wq, &wpw->work);

    return 0;
}

/*
 * powerfs_write_begin - 写开始 (Stage C: 对接 netfs 子系统)
 *
 * 参照 xxx_write_begin (fs/xxx/addr.c):
 *   调用 netfs_write_begin 让 netfs 管理页面准备, 包括:
 *   - 分配并锁定 folio
 *   - 若 folio 不在 page cache 或非 uptodate, 通过 issue_read
 *     从 Filer 拉取现有数据 (处理 partial write 的 read-modify-write)
 *   - folio 返回时已锁定, 调用者写入后通过 write_end 解锁
 *
 * 替代旧的 grab_cache_page_write_begin + zero_user 方案:
 *   旧方案对 partial write (offset/len 未覆盖整页) 会丢失未写入部分
 *   的现有数据, 因为 zero_user 把整页清零. netfs_write_begin 会先读取
 *   现有数据, write_end 只更新写入部分, 保证 read-modify-write 正确性.
 */
int powerfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
                         loff_t pos, unsigned int len, struct folio **foliop,
                         void **fsdata)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    struct folio *folio = NULL;
    unsigned int got_caps = 0;
    int ret;

    pr_debug("powerfs: write_begin ino=%lu pos=%lld len=%u i_size=%lld\n",
            inode->i_ino, pos, len, i_size_read(inode));

    /* P2-7: Quota check — 字节配额 (写入后 i_size 不能超过父目录 max_bytes) */
    {
        loff_t newlen = pos + len;
        if (newlen > i_size_read(inode)) {
            ret = powerfs_quota_check_max_bytes(inode, newlen);
            if (ret)
                return ret;
        }
    }

    /* ========== Cap 仲裁 (对齐  xxx_write_begin → try_get_caps) ==========
     *
     * 写页面必须至少拿到 FILE_SHARED (读) + FILE_WR (写) 位.
     *   - FULL (同时拿到 FILE_CACHE + FILE_EXCL): 可完全信任本地缓存, write_end
     *     直接写 pagecache 并标记 dirty, revoke 时 flush 保障一致性.
     *   - PARTIAL (只拿到 FILE_SHARED|WR): 本地能写但没独占权, 也放行.
     *     netfs_write_begin 遇到 partial page 会从服务端回源拉现有数据,
     *     不会产生 stale 数据.
     *   - NONE (连 FILE_SHARED 都没): 仍然放行 (降级模式, 等 cap 协议对接
     *     服务端 grant 到位后严格化). try_get_caps 会触发 check_caps 催
     *     服务端 AcquireCap, 下次 IO 再命中.
     *
     * 使用 nonblock=true: write_begin 是 VFS 页缓存准备路径, 不可长时间阻塞.
     * got_caps 通过 *fsdata 带到 write_end 对称 cap_put_refs. */
    if (iocb) {
        powerfs_try_get_caps(pi,
                             POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_WR,
                             POWERFS_CAP_FILE_SHARED | POWERFS_CAP_FILE_CACHE |
                             POWERFS_CAP_FILE_WR     | POWERFS_CAP_FILE_EXCL,
                             true, &got_caps);
    }
    *fsdata = (void *)(uintptr_t)got_caps;

    /* page_symlink() 调用时 iocb==NULL (无 file 描述符),
     * 不能走 netfs_write_begin (需要 file 参数).
     * 改用 __filemap_get_folio 直接获取 page cache folio. */
    if (!iocb) {
        folio = __filemap_get_folio(mapping, pos / PAGE_SIZE,
                                    FGP_WRITEBEGIN | FGP_LOCK | FGP_CREAT,
                                    mapping_gfp_mask(mapping));
        if (!folio)
            return -ENOMEM;
        *foliop = folio;
        return 0;
    }

    ret = netfs_write_begin(&pi->netfs, iocb->ki_filp, inode->i_mapping,
                            pos, len, &folio, NULL);
    if (ret < 0) {
        pr_warn("powerfs: write_begin netfs_write_begin failed: %d\n", ret);
        /* netfs 失败时对称释放 try_get_caps 占的引用, 避免泄漏. */
        if (got_caps)
            powerfs_cap_put_refs(pi, got_caps);
        return ret;
    }

    pr_debug("powerfs: WB_BEGIN ok ino=%lu folio=%px order=%d locked=%d uptodate=%d index=%lu\n",
            inode->i_ino, folio, folio_order(folio),
            folio_test_locked(folio), folio_test_uptodate(folio),
            folio->index);

    WARN_ON_ONCE(!folio_test_locked(folio));
    *foliop = folio;
    /* 注意: *fsdata 已在函数开头写入 got_caps, 这里不要覆盖,
     * 这样 write_end 能取出它做对称 cap_put_refs. */
    return 0;
}

/*
 * powerfs_migrate_inline_to_flat - K2-7 Inline → Flat 自动迁移
 *
 * 对齐 FUSE write inline migrate (powerfs-fuse/src/fuse.rs L3446):
 *   1. 快照 inline_data (持 i_lock)
 *   2. 调 Filer MIGRATE_INLINE_ALLOC 分配 (volume_id, needle_id)
 *   3. 同步写 merged_data 到 Volume Server (WriteNeedle, lease_token=NULL)
 *   4. 持锁切换 inode: placement=Flat + volume_id + file_key + 清 inline_data
 *
 * crash safety (对齐 FUSE):
 *   - Filer 分配后不修改 inode, 保留 inline_data
 *   - 客户端崩溃后 Filer 仍有 inline_data, 文件仍可作 Inline 读
 *   - 分配的 needle_id 泄漏 (可接受, 同 CREATE 失败)
 *   - 客户端写 Volume Server 成功后, close 时 UPDATE_INODE_SIZE_CHUNKS
 *     原子完成切换 (清除 inline_data + 设 Flat chunks)
 *
 * 注意: 本函数在 write_end 中调用, 持有 folio lock. 网络 I/O 期间
 *       folio lock 被持有, 但 inline 路径已在 write_end 中做 kvmalloc
 *       (可睡眠), 故阻塞操作可接受. 迁移数据最大 8KB, 网络往返 <100ms.
 *
 * 返回 0 成功, 负数错误码 (网络错误透传, inline_data 保持原状).
 */
static int powerfs_migrate_inline_to_flat(struct inode *inode,
                                          struct powerfs_inode_info *pi)
{
    u8 *snap_data = NULL;
    u32 snap_len = 0;
    u64 shard_id, ino = inode->i_ino;
    u64 volume_id = 0, file_key = 0;
    int ret;

    /* 1. 持锁快照 inline_data (网络 I/O 不能持 spinlock) */
    spin_lock(&pi->i_lock);
    if (!pi->inline_data || pi->inline_len == 0) {
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: MIGRATE ino=%lu no inline_data to migrate\n", ino);
        return -EINVAL;
    }
    snap_len = pi->inline_len;
    spin_unlock(&pi->i_lock);

    snap_data = kmalloc(snap_len, GFP_KERNEL);
    if (!snap_data) {
        pr_warn("powerfs: MIGRATE ino=%lu kmalloc %u failed\n", ino, snap_len);
        return -ENOMEM;
    }
    spin_lock(&pi->i_lock);
    if (pi->inline_data && pi->inline_len == snap_len) {
        memcpy(snap_data, pi->inline_data, snap_len);
    } else {
        spin_unlock(&pi->i_lock);
        pr_warn("powerfs: MIGRATE ino=%lu inline_data changed during snapshot\n", ino);
        kfree(snap_data);
        return -EAGAIN;
    }
    spin_unlock(&pi->i_lock);

    pr_info("powerfs: MIGRATE ino=%lu inline_len=%u → triggering Flat migration\n",
            ino, snap_len);

    /* 2. 调 Filer MIGRATE_INLINE_ALLOC 分配 (volume_id, needle_id).
     * shard_id = shard_map_route(parent_ino) — 区间路由, 对齐 FUSE ShardMap. */
    shard_id = shard_map_route(pi->parent_ino ? pi->parent_ino : ino);
    ret = powerfs_net_migrate_inline_alloc(shard_id, ino, &volume_id, &file_key);
    if (ret < 0) {
        pr_warn("powerfs: MIGRATE ino=%lu alloc failed: %d, inline buffer unmodified\n",
                ino, ret);
        kfree(snap_data);
        return ret;  /* 透传网络错误 (-ENOTCONN/-ETIMEDOUT 等), 非 EFBIG */
    }

    /* 3. 同步写 snap_data 到 Volume Server (WriteNeedle).
     * lease_token=NULL: Volume Server 不校验 lease (net_handler.rs L92).
     * ClientId="kernel-client" 必须发送 (write_needle L5934 注释). */
    ret = powerfs_net_write_needle(volume_id, file_key, ino,
                                    snap_data, snap_len,
                                    NULL, 0);
    if (ret < 0) {
        pr_warn("powerfs: MIGRATE ino=%lu write_needle failed: %d, needle_id=%#llx leaked\n",
                ino, ret, (unsigned long long)file_key);
        kfree(snap_data);
        return ret;  /* 透传网络错误, 非 EFBIG */
    }

    pr_info("powerfs: MIGRATE ino=%lu write_needle OK volume_id=%llu needle_id=%#llx size=%u\n",
            ino, (unsigned long long)volume_id,
            (unsigned long long)file_key, snap_len);

    /* 4. 持锁切换 inode 到 Flat: 释放 inline_data, 更新布局元数据.
     * 后续 write 走 Flat writeback 路径, close 时 UPDATE_INODE_SIZE_CHUNKS
     * 同步 size+chunks 到 Filer (原子清除 inline_data + 设 Flat chunks). */
    spin_lock(&pi->i_lock);
    pi->placement = POWERFS_PLACEMENT_FLAT;
    pi->volume_id = volume_id;
    pi->file_key = file_key;
    pi->layout_chunk_size = POWERFS_CHUNK_SIZE;
    kfree(pi->inline_data);
    pi->inline_data = NULL;
    pi->inline_len = 0;
    pi->inline_dirty = false;
    spin_unlock(&pi->i_lock);

    kfree(snap_data);

    /* K2-9: After migration, the Volume Server only has inline_data (≤8KB).
     * Pages beyond inline_data's range were written via write_end (which
     * marks them dirty) but INLINE writeback cleared their dirty flags
     * without writing to the server. These clean pages are only in the
     * page cache — if they get evicted (memory pressure, refresh_work),
     * their data is lost forever.
     *
     * Fix: mark ALL pages in the page cache as dirty. This ensures the
     * next FLAT writeback will RMW all pages (not just the currently
     * dirty ones) and produce a complete needle on the Volume Server.
     * The overhead is one writeback of the full file, which is acceptable
     * since migration is a one-time event.
     *
     * Note: migration is called from write_end which holds the current
     * folio's lock. Use folio_trylock to avoid deadlock — if a folio
     * is already locked (the current write_end folio), it's already
     * dirty (write_end marks it dirty before calling migrate). */
    {
        struct folio_batch fbatch;
        pgoff_t idx = 0;
        int re_dirty_count = 0;

        folio_batch_init(&fbatch);
        while (filemap_get_folios(inode->i_mapping, &idx,
                                  (pgoff_t)-1, &fbatch) > 0) {
            int i;
            for (i = 0; i < folio_batch_count(&fbatch); i++) {
                struct folio *f = fbatch.folios[i];
                if (!folio_test_dirty(f)) {
                    if (folio_trylock(f)) {
                        if (!folio_test_dirty(f))
                            folio_mark_dirty(f);
                        folio_unlock(f);
                        re_dirty_count++;
                    }
                    /* If trylock fails, folio is locked by current
                     * write_end — it's already dirty, skip. */
                }
                folio_put(f);
            }
            folio_batch_init(&fbatch);
        }
        if (re_dirty_count > 0) {
            pr_info("powerfs: MIGRATE ino=%lu re-dirtied %d clean pages (data beyond inline_data not on server)\n",
                    ino, re_dirty_count);
        }
    }

    pr_info("powerfs: MIGRATE ino=%lu → Flat done, subsequent writes → Volume Server\n", ino);
    return 0;
}

/*
 * powerfs_write_end - 写结束 (Stage C: 纯 page cache 更新, 无网络 IO)
 *
 * 参照 xxx_write_end (fs/xxx/addr.c):
 *   - 标记 folio uptodate
 *   - 更新本地 i_size (i_size_write + mark_inode_dirty)
 *   - folio_mark_dirty 让 writeback 子系统负责持久化
 *   - 不做任何网络 IO, 数据持久化由 writepage 异步完成,
 *     i_size 同步由 write_inode (writeback 时) 完成
 *
 * 替代旧的同步写方案:
 *   旧方案在 write_end 中同步调用 powerfs_net_write + powerfs_net_setattr,
 *   导致 write() 系统调用阻塞在网络往返上, 高并发下性能差.
 *   Stage C 改为纯 page cache 更新, write() 立即返回,
 *   持久化推迟到 writeback (writepage + write_inode).
 */
int powerfs_write_end(const struct kiocb *iocb, struct address_space *mapping,
                       loff_t pos, unsigned int len, unsigned int copied,
                       struct folio *folio, void *fsdata)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi = POWERFS_I(inode);
    loff_t end_pos = pos + copied;
    unsigned int got_caps = (unsigned int)(uintptr_t)fsdata;

    pr_debug("powerfs: WB_END ino=%lu pos=%lld copied=%u len=%u i_size=%lld uptodate=%d\n",
            inode->i_ino, pos, copied, len, i_size_read(inode),
            folio_test_uptodate(folio));

    if (copied > 0) {
        if (!folio_test_uptodate(folio)) {
            if (copied < len) {
                pr_debug("powerfs: WB_END partial write, return 0 for retry\n");
                copied = 0;
                goto out;
            }
            folio_mark_uptodate(folio);
        }
        /* ROOT35: Mark folio + cap dirty BEFORE updating i_size.
         * Closes the race window where refresh_work checks
         * mapping_tagged(PAGECACHE_TAG_DIRTY) between i_size_write
         * and folio_mark_dirty, finds no dirty pages, then overwrites
         * i_size with stale server value (0) + invalidate_mapping_pages
         * discards the just-written data → 0-byte files. */
        folio_mark_dirty(folio);
        powerfs_cap_mark_dirty(pi, POWERFS_CAP_FILE_WR);
        if (end_pos > i_size_read(inode)) {
            spin_lock(&inode->i_lock);
            if (end_pos > i_size_read(inode))
                i_size_write(inode, end_pos);
            spin_unlock(&inode->i_lock);
            mark_inode_dirty(inode);
        }

        /* K2: Inline 模式 — 同步写入数据到 inline_data 缓冲.
         * writeback 不会将 Inline 文件的数据发送到 Volume Server,
         * 而是在 close 时通过 UPDATE_INODE 提交 inline_data 到 Filer.
         *
         * 这里在 write_end 中直接拷贝 folio 数据到 inline_data,
         * 确保 inline_data 始终与 page cache 同步.
         *
         * 并发保护: 持 i_lock 修改 inline_data.
         * 注意: kvmalloc 在持 spinlock 时可能睡眠 (GFP_KERNEL),
         * 因此先在锁外分配, 再持锁替换. */
        if (pi->placement == POWERFS_PLACEMENT_INLINE) {
            size_t folio_off = offset_in_folio(folio, pos);
            size_t need_len = end_pos;
            u8 *new_buf = NULL;

            /* K2: 仅首次写入(pos==0)输出日志, 避免 bs=1 时 8192 条日志淹没 serial */
            if (pos == 0)
                pr_info("powerfs: WB_END INLINE first write ino=%lu copied=%u end=%zu\n",
                        inode->i_ino, copied, end_pos);

            /* K2-7: 检查是否超出 inline 硬上限 (8KB).
             * 超出时截断到 INLINE_MAX_SIZE, 写入后触发迁移到 Flat.
             * 迁移阈值 = min(inline_max_size × 1.5, INLINE_MAX_SIZE),
             * 对齐 FUSE fuse.rs L3444. */
            if (need_len > POWERFS_INLINE_MAX_SIZE) {
                pr_warn("powerfs: WB_END INLINE ino=%lu write end=%zu > INLINE_MAX=%d, "
                        "will migrate after write\n",
                        inode->i_ino, need_len, POWERFS_INLINE_MAX_SIZE);
                need_len = POWERFS_INLINE_MAX_SIZE;
                copied = min_t(unsigned int, copied,
                               POWERFS_INLINE_MAX_SIZE - pos);
                if (copied == 0) {
                    /* pos 已超 8KB, 无法写入 inline_data.
                     * 返回 -EFBIG 触发上层重试 (此时若已迁移则走 Flat). */
                    folio_unlock(folio);
                    folio_put(folio);
                    return -EFBIG;
                }
                end_pos = pos + copied;
            }

            /* 若 inline_data 未分配或不够大, 在锁外分配新 buffer */
            spin_lock(&pi->i_lock);
            if (!pi->inline_data || pi->inline_len < need_len) {
                u8 *old = pi->inline_data;
                u32 old_len = pi->inline_len;
                spin_unlock(&pi->i_lock);

                /* 锁外分配 (GFP_KERNEL 可睡眠) */
                new_buf = kvmalloc(need_len, GFP_KERNEL);
                if (!new_buf) {
                    pr_warn("powerfs: WB_END INLINE ino=%lu kvmalloc %zu failed\n",
                            inode->i_ino, need_len);
                    /* 分配失败不影响 page cache, writeback 仍会标脏页.
                     * inline_data 不更新, close 时可能丢失数据.
                     * 后续可回退到 Flat 模式. */
                    goto inline_done;
                }
                /* 拷贝旧数据到新 buffer */
                if (old && old_len > 0)
                    memcpy(new_buf, old, old_len);
                /* 新区域清零 */
                if (need_len > old_len)
                    memset(new_buf + old_len, 0, need_len - old_len);

                /* 持锁替换 */
                spin_lock(&pi->i_lock);
                kfree(pi->inline_data);
                pi->inline_data = new_buf;
                pi->inline_len = need_len;
                /* new_buf 所有权已转移, 防止下方 kfree */
                new_buf = NULL;
            }

            /* 从 folio 拷贝写入的数据到 inline_data */
            if (pi->inline_data && pos + copied <= pi->inline_len) {
                void *kaddr = kmap_local_folio(folio, folio_off);
                memcpy(pi->inline_data + pos, kaddr, copied);
                kunmap_local(kaddr);
                pi->inline_dirty = true;
                pr_debug("powerfs: WB_END INLINE ino=%lu pos=%lld copied=%u inline_len=%u\n",
                        inode->i_ino, pos, copied, pi->inline_len);
            }
            spin_unlock(&pi->i_lock);

            /* 分配了但未使用 (被并发覆盖) 的 buffer 释放 */
            kfree(new_buf);

            /* K2-7: 检查是否需要迁移到 Flat.
             * 迁移阈值 = min(inline_max_size × 1.5, POWERFS_INLINE_MAX_SIZE).
             * 对齐 FUSE fuse.rs L3444: 滞后窗口避免边界抖动.
             * 迁移成功: inode 切换到 Flat, 后续 write 走 Flat writeback.
             * 迁移失败: 透传网络错误码, inline_data 保持原状 (close 时同步到 Filer). */
            {
                u32 migrate_threshold = min(pi->inline_max_size * 3 / 2,
                                            (u32)POWERFS_INLINE_MAX_SIZE);
                /* 注意: 用 >= 而非 >, 否则当 inline_max_size == POWERFS_INLINE_MAX_SIZE
                 * (如 8192) 时, migrate_threshold=8192, inline_len 最大也是 8192,
                 * 条件 inline_len > 8192 永远为 false, 迁移不会触发. */
                if (pi->inline_len >= migrate_threshold) {
                    int mig_ret = powerfs_migrate_inline_to_flat(inode, pi);
                    if (mig_ret < 0) {
                        pr_warn("powerfs: WB_END INLINE ino=%lu migrate failed: %d\n",
                                inode->i_ino, mig_ret);
                        folio_unlock(folio);
                        folio_put(folio);
                        if (got_caps)
                            powerfs_cap_put_refs(pi, got_caps);
                        return mig_ret;
                    }
                    /* 迁移成功: inode 已切换到 Flat.
                     * folio 仍标记 dirty, writeback 会走 Flat 路径写 Volume Server.
                     * close 时 UPDATE_INODE_SIZE_CHUNKS 同步 size+chunks 到 Filer. */
                }
            }
        }
    }

inline_done:
out:
    /* 与 write_begin 中 try_get_caps 对称, 释放 cap 引用.
     * got_caps 为 0 时直接跳过 (write_begin 中 !iocb 或 need 不满足时). */
    if (got_caps)
        powerfs_cap_put_refs(pi, got_caps);
    folio_unlock(folio);
    folio_put(folio);

    return copied;
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

/*
 * powerfs_direct_IO - O_DIRECT 回调 (fallback to buffered I/O)
 *
 * PowerFS 是基于 netfs/page cache 的网络文件系统, 不支持真正的 direct I/O
 * (DMA 直传用户缓冲区). 但很多应用 (如 LTP read02 测试) 会用 O_DIRECT 打开
 * 文件. 在内核 6.17 中, do_dentry_open() 检查:
 *   if ((f->f_flags & O_DIRECT) && !(f->f_mode & FMODE_CAN_ODIRECT))
 *       return -EINVAL;
 * FMODE_CAN_ODIRECT 仅在 a_ops->direct_IO 非 NULL 时设置 (fs/open.c:978).
 *
 * 若不提供 direct_IO 回调, open(O_DIRECT) 会返回 EINVAL, 导致依赖 O_DIRECT
 * 的测试和应用无法运行.
 *
 * 返回 0 的语义: generic_file_read_iter() 中, direct_IO 返回 0 表示
 * "0 bytes transferred", 随后 fallthrough 到 filemap_read() (buffered I/O):
 *   retval = mapping->a_ops->direct_IO(iocb, iter);  // = 0
 *   if (retval >= 0) { iocb->ki_pos += retval; count -= retval; }  // no-op
 *   if (retval < 0 || !count || IS_DAX(inode)) return retval;     // false
 *   if (iocb->ki_pos >= i_size_read(inode)) return retval;        // false
 *   return filemap_read(iocb, iter, retval);  // ← buffered read
 *
 * 这与 ramfs/tmpfs 不同 (它们不支持 O_DIRECT open), 但与 btrfs 压缩文件
 * 短读 fallback 行为一致 (mm/filemap.c:2898 注释).
 */
static ssize_t powerfs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
    return 0;
}

/* 地址空间操作表 - 内核 6.2 netfs 风格
 *
 * .dirty_folio 必须设置: folio_mark_dirty() 内部通过
 * mapping->a_ops->dirty_folio() 间接调用 (mm/page-writeback.c),
 * 若未设置则为 NULL, write_end 标脏页时触发 NULL instruction fetch oops.
 * 参考 fs/nfs/file.c, fs/btrfs/inode.c, fs/xxx/addr.c (均设置 dirty_folio).
 * 我们不使用 buffer_heads, 故用 filemap_dirty_folio (与 nfs/btrfs/zonefs 一致)
 *   外加 PowerFS cap 封装: 脏页同步标 i_dirty_caps 的 CAP_WR_DATA.
 *
 * .invalidate_folio / .release_folio 对接 netfs 子系统通用实现:
 *   truncate/invalidate/shrinker 触发时需清理 netfs 私有资源 (struct netfs_folio,
 *   缓存列表, subrequest). 若缺失, 内存会泄漏, 且截断后读回 stale 数据.
 */

/*
 * PowerFS custom dirty_folio: 页面被标记 dirty 时, 同步标记 pi->i_dirty_caps 的
 * CAP_WR_DATA. 如果 PG_dirty 被设置但 i_dirty_caps 没有 CAP_WR_DATA,
 * 后续 cap_recall → WR_DATA 不在 flushing mask → 跳过写回 → recall_ack 提前
 * 返回, 服务端授 EXCLUSIVE 给新 writer 后其 read_folio 拉到 Filer 旧版 0 填充页.
 *
 * 对齐  xxx_dirty_folio (addr.c L80-L133): 先 VFS 标准 PG_dirty +
 * accounting, 再追加 FS 侧 cap/wrbuffer 跟踪. PowerFS 没有 snap context,
 * 只需追加 dirty_caps 标记.
 */
static bool powerfs_dirty_folio(struct address_space *mapping, struct folio *folio)
{
    struct inode *inode = mapping->host;
    struct powerfs_inode_info *pi;
    bool ret;

    /* Step 1: VFS 标准 dirty 路径.
     *   __folio_mark_dirty → 增加 NR_FILE_DIRTY accounting, 挂 wb list, memcg 记账 */
    ret = filemap_dirty_folio(mapping, folio);

    /* Step 2: 追加 PowerFS cap dirty_caps 跟踪.
     * 跳过条件: inode NULL (匿名 mapping), folio 最终未脏, inode shutdown. */
    if (unlikely(!inode || !folio_test_dirty(folio)))
        return ret;
    pi = POWERFS_I(inode);
    if (unlikely(!pi || pi->shutdown))
        return ret;

    /* 标 CAP_WR_DATA: 只要有一页被标 dirty, recall 时就要走 filemap writeback
     * 把整个 inode 的脏页写回. 粒度粗但安全; write_end 里也会重复标位
     * (idempotent), 不会有副作用. */
    spin_lock(&pi->i_lock);
    pi->i_dirty_caps |= POWERFS_CAP_WR_DATA;
    spin_unlock(&pi->i_lock);

    return ret;
}

const struct address_space_operations powerfs_aops = {
    .read_folio       = netfs_read_folio,     /* Stage C: netfs 子系统管理 folio 生命周期 */
    .readahead        = netfs_readahead,      /* Stage C: 批量预读 */
    .writepages       = powerfs_writepages,   /* 批量 writeback (6.17: 无 writepage 回调) */
    .write_begin      = powerfs_write_begin,
    .write_end        = powerfs_write_end,
    .dirty_folio      = powerfs_dirty_folio,  /* P0-7 fix: 脏页同步标 CAP_WR_DATA dirty */
    .invalidate_folio = netfs_invalidate_folio, /* P0-6 fix: truncate/invalidate 清理 netfs folio 资源 */
    .release_folio    = netfs_release_folio,    /* P0-6 fix: shrinker 回收前释放 netfs 资源 */
    .direct_IO        = powerfs_direct_IO,    /* O_DIRECT fallback to buffered I/O */
    .bmap             = powerfs_bmap,
    .migrate_folio    = filemap_migrate_folio, /* P1-4: 支持 NUMA 页迁移 / memory hotplug / CRIU */
};
