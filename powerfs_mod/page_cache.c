// SPDX-License-Identifier: GPL-2.0
/*
 * PowerFS Lock Page-Cache Invalidation Helpers implementation
 * (phase 4-6 skeleton).
 *
 * See page_cache.h for design rationale. Thin wrappers over:
 *   - filemap_fdatawrite_range + filemap_fdatawait_range  (flush)
 *   - invalidate_inode_pages2_range                        (invalidate)
 *
 * These helpers are intentionally split from lock_client.c so the
 * state machine file stays free of pagemap.h dependencies — the
 * lock client only needs the function prototypes in powerfs_lock.h
 * to call into here.
 *
 * Kernel-only: not built in the userspace codec round-trip test.
 */
#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>     /* filemap_fdatawrite_range, filemap_fdatawait_range,
                                * invalidate_inode_pages2_range */
#include <linux/errno.h>
#include <linux/printk.h>

#include "page_cache.h"

/* filemap_write_and_wait_range combines fdatawrite + fdatawait in one
 * call. We split them anyway because the plan (§3.2 decision 4)
 * explicitly names `filemap_fdatawrite_range` + `filemap_fdatawait_range`
 * as the two-step sequence — this gives the lock state machine a
 * future hook to interpose between writeback kickoff and wait, e.g.
 * to send a "release-pending" RPC to the Filer while writeback is in
 * flight (an Early-Grant optimization point, see docs/lock-optimization-plan.md
 * §5.2). The single-call form would lose that seam.
 */

int powerfs_lock_pagecache_flush(struct inode *inode, __u64 start, __u64 end,
                                 bool full_inode)
{
    if (!inode)
        return -EINVAL;
    if (full_inode) {
        start = 0;
        end = LLONG_MAX;
    }

    /* Step 1: kick off writeback for the locked range. Returns 0
     * unconditionally on this kernel — writeback errors are surfaced
     * in step 2's wait. */
    filemap_fdatawrite_range(inode->i_mapping, start, end);

    /* Step 2: wait for writeback to complete, returning any EIO/ENOSPC
     * encountered. The lock caller treats non-zero as "release must
     * still proceed, but warn — the volume server may have stale data
     * and a future reader may get an inconsistent copy." */
    int ret = filemap_fdatawait_range(inode->i_mapping, start, end);
    if (ret)
        pr_warn("powerfs_lock: pagecache flush ino=%lu [%llu,%llu] ret=%d\n",
                inode->i_ino, start, full_inode ? (__u64)LLONG_MAX : end, ret);
    return ret;
}

int powerfs_lock_pagecache_invalidate(struct inode *inode, __u64 start,
                                      __u64 end, bool full_inode)
{
    if (!inode)
        return -EINVAL;
    if (full_inode) {
        /* invalidate_inode_pages2_range's signature takes page-cache
         * indices, not byte offsets. For full-inode invalidation use
         * the simpler `invalidate_mapping_pages` with the whole range
         * (matching the existing usage in powerfs_fs.c refresh_work). */
        return invalidate_mapping_pages(inode->i_mapping, 0, (pgoff_t)-1);
    }

    /* For range invalidation, convert byte offsets to page indices.
     * Page size is PAGE_SIZE — using offset_idx arithmetic that matches
     * what the kernel's own `invalidate_inode_pages2_range` does
     * internally, but we open-code it here so callers see explicit
     * semantics: the page containing `start`'s first byte through the
     * page containing `end`'s last byte are both dropped. */
    pgoff_t start_idx = start >> PAGE_SHIFT;
    pgoff_t end_idx = end >> PAGE_SHIFT;
    if (end_idx < start_idx) {
        /* 0-length range — caller asked for nothing. */
        return 0;
    }
    /* invalidate_inode_pages2_range is inclusive start, exclusive end
     * on page indices. Add 1 to end_idx to include the page holding
     * the last byte. */
    if (end_idx == (pgoff_t)-1)
        end_idx = 0;  /* avoid overflow: caller wants "to end of file" */

    return invalidate_inode_pages2_range(inode->i_mapping, start_idx,
                                         end_idx + 1);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PowerFS lock page-cache invalidation helpers (phase 4-6)");
#endif /* __KERNEL__ */
