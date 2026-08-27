/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PowerFS Lock Page-Cache Invalidation Helpers (phase 4-6 skeleton).
 *
 * Binding page-cache lifetime to lock state (decision 4,
 * docs/lock-optimization-plan.md §3.2):
 *   - Before Release/RevokeAck: flush dirty pages covering the locked
 *     range so the volume server has the latest data before another
 *     client is granted the lease.
 *   - On Invalidate (server push): drop cached pages covering the
 *     range so the next read re-fetches the new data written by another
 *     client.
 *
 * The helpers are thin wrappers over the kernel's
 * `filemap_fdatawrite_range` + `filemap_fdatawait_range` and
 * `invalidate_inode_pages2_range`, factored out so the lock state
 * machine (lock_client.c) can call them without pulling in pagemap.h
 * (which is heavy and not needed elsewhere in the lock client).
 *
 * Kernel-only: these functions touch `inode->i_mapping` which is a
 * struct-address-space accessor not available in userspace tests. They
 * are excluded from the codec round-trip test build.
 */
#ifndef _POWERFS_LOCK_PAGE_CACHE_H
#define _POWERFS_LOCK_PAGE_CACHE_H

#ifdef __KERNEL__
#include <linux/fs.h>
#endif

/* Same prototypes as declared in powerfs_lock.h — repeated here so
 * callers can include this header directly without dragging in the
 * full lock-client API. */
int powerfs_lock_pagecache_flush(struct inode *inode, __u64 start, __u64 end,
                                 bool full_inode);
int powerfs_lock_pagecache_invalidate(struct inode *inode, __u64 start,
                                      __u64 end, bool full_inode);

#endif /* _POWERFS_LOCK_PAGE_CACHE_H */
