/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PowerFS Lock Client — kernel-side interface (phase 4-6 skeleton).
 *
 * Mirrors the Rust `powerfs-lock` crate's types and the wire protocol
 * defined in `docs/lock-protocol.md`. The C kernel client implements
 * the protocol independently (no shared code with Rust — see
 * `docs/lock-optimization-plan.md` §3.2 decision 1).
 *
 * This header is the public interface for:
 *   - The TLV wire codec        (tlk_codec.c — see tlk_codec.h)
 *   - The lock state machine    (lock_client.c)
 *   - Page-cache invalidation   (page_cache.c)
 *
 * The state machine is a skeleton: acquire/release/renew/revoke_ack
 * have stub implementations that compile under the kernel build
 * system and are wired into the inode lifecycle (`inode->i_private`),
 * but the Early Grant / Early Revoke / SN fast paths are TODOs
 * guarded by `powerfs_lock_client_acquire` / `handle_revoke_ack`.
 *
 * Wire-protocol constants (msg types, field tags, modes, error
 * codes) live in tlk_codec.h — the single source for both the codec
 * and the state machine, kept in sync with the Rust side
 * (powerfs-lock-net/src/msg.rs) and docs/lock-protocol.md.
 */
#ifndef _POWERFS_LOCK_H
#define _POWERFS_LOCK_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#ifdef __KERNEL__
#include <linux/fs.h>
#include <linux/errno.h>
#endif

#include "tlk_codec.h"

/* ========== Lease state (per-inode, hung off inode->i_private) ==========
 *
 * Decision 4 (docs/lock-optimization-plan.md §3.2): lease state lives
 * in `inode->i_private` as `KernelLeaseState`. One entry per inode for
 * inode-level (metadata) locks; range-level locks live in the volume
 * server's range store, not here.
 *
 * Lifetime: created on first `acquire`, cleared on `release` or when
 * the inode is evicted. The token + sn together identify the grant
 * across leader switches (the Fencer epoch guards against zombie
 * clients — see powerfs-lock-health).
 */
struct powerfs_kernel_lease_state {
    /* Opaque lease token from the Filer (NUL-terminated for
     * printk; wire form has no NUL — see tlk_codec.c). */
    char token[64];
    size_t token_len;

    /* Global sequence number allocated by the Filer leader
     * (phase 4 §5.2/§5.3). sn == 0 means "modularization phase,
     * no SN" — see docs/lock-protocol.md §3 (TLK_FIELD_SN). */
    __u64 sn;

    /* Fencer epoch (powerfs-lock-health). A write with a stale
     * epoch is rejected by the volume server, fencing zombie
     * clients after split-brain. */
    __u64 epoch;

    /* Lease expiry in jiffies. Renewed by `lease_renew_work`. */
    unsigned long expire_jiffies;

    /* Lock mode (TLK_MODE_*). Inode-level locks are always
     * EXCLUSIVE; SHARED is for future read-side leasing. */
    __u8 mode;

    /* True between Revoke (server pushed) and RevokeAck (client
     * sent): the holder is flushing dirty pages before release. */
    bool revoking;
};

/* ========== Lock client (per-mount) ==========
 *
 * One per superblock. Holds the connection to the Filer's lock
 * service, the renew workqueue, and the SN high-water for fencing.
 * State machine skeleton: the actual RPC I/O is stubbed; callers
 * should wire `powerfs_lock_client_send` to the transport layer
 * (powerfs_transport.c) in a follow-up.
 */
struct powerfs_lock_client {
    /* Client identity (FUSE mount ID or kernel client UUID).
     * Sent in TLK_FIELD_CLIENT_ID. */
    char client_id[64];
    size_t client_id_len;

    /* Default lease duration in ms (TLK_FIELD_TIMEOUT_MS / TLK_FIELD_LEASE_MS). */
    __u32 default_lease_ms;

    /* Renew workqueue (one work item per held lease, queued on
     * `lease_renew_work` in `powerfs_kernel_lease_state`). */
    struct workqueue_struct *renew_wq;

    /* SN high-water mark (for fencing on leader switch). Updated
     * by `handle_grant` when the server allocates a new SN. */
    __u64 sn_high_water;

    /* TODO: connection to Filer's lock service. The skeleton leaves
     * this as void*; the real implementation wires a
     * `powerfs_transport` session on CHANNEL_LOCK (§7). */
    void *transport;

    /* Protects `sn_high_water` and the inode lease tree. */
    spinlock_t lock;
};

/* ========== Callbacks (adapter for LockEventHandler) ==========
 *
 * The Rust side (powerfs-lock-fuse) defines a `LockEventHandler` trait
 * with `on_grant` / `on_revoke` / `on_invalidate`. The kernel side
 * implements the same semantics via function pointers so the state
 * machine can call back into the FS layer without a hard dependency.
 */
struct powerfs_lock_event_ops {
    /* Called when a Grant arrives (or Early Grant on RevokeAck).
     * The inode's `i_private` is updated with the new token/sn. */
    int (*on_grant)(struct inode *inode, const char *token,
                    size_t token_len, __u64 sn, __u32 lease_ms);

    /* Called when a Revoke arrives (server wants the lease back).
     * The holder must flush dirty pages then send RevokeAck. */
    int (*on_revoke)(struct inode *inode, const char *token,
                     size_t token_len);

    /* Called when an Invalidate arrives (page cache must be
     * dropped for the inode, or a sub-range). range_start/end
     * are ignored for full_inode invalidate (both == 0). */
    int (*on_invalidate)(struct inode *inode, __u64 range_start,
                         __u64 range_end, bool full_inode);
};

/* ========== Public API (implemented in lock_client.c) ========== */

/* Lifecycle */
int powerfs_lock_client_init(struct powerfs_lock_client *cli,
                             const char *client_id,
                             __u32 default_lease_ms);
void powerfs_lock_client_destroy(struct powerfs_lock_client *cli);

/* State machine — acquire/release/renew/revoke_ack.
 *
 * These are the kernel-side entry points mirroring the Rust
 * `FuseLockManager` API. Skeleton: each returns 0 with a TODO
 * printk once the transport is wired; callers should treat -ENOSYS
 * as "feature not yet implemented, fall back to the existing
 * powerfs_lease path".
 */
int powerfs_lock_client_acquire(struct powerfs_lock_client *cli,
                                struct inode *inode, __u8 mode,
                                __u32 timeout_ms);
int powerfs_lock_client_release(struct powerfs_lock_client *cli,
                                struct inode *inode);
int powerfs_lock_client_renew(struct powerfs_lock_client *cli,
                              struct inode *inode);
int powerfs_lock_client_revoke_ack(struct powerfs_lock_client *cli,
                                   struct inode *inode);

/* Message dispatch — called when a full frame arrives on
 * CHANNEL_LOCK. Parses msg_type + payload via tlk_codec.c, then
 * routes to the state machine. Skeleton: returns -ENOSYS until the
 * transport layer is wired. */
int powerfs_lock_client_handle_frame(struct powerfs_lock_client *cli,
                                     const __u8 *frame, size_t frame_len);

/* Attach/detach event ops (the FS layer registers its callbacks at
 * mount time). */
void powerfs_lock_client_set_event_ops(struct powerfs_lock_client *cli,
                                       const struct powerfs_lock_event_ops *ops);

/* ========== Page-cache invalidation (page_cache.c) ========== */

/* Flush dirty pages for `inode` covering [start, end) (or the whole
 * inode if full_inode). Blocks until writeback completes. Called by
 * `on_revoke` before sending RevokeAck (decision 4: page cache
 * invalidation is bound to lock release). */
int powerfs_lock_pagecache_flush(struct inode *inode, __u64 start, __u64 end,
                                 bool full_inode);

/* Invalidate (drop) cached pages for `inode` covering [start, end)
 * (or the whole inode). Called by `on_invalidate` when the server
 * signals that another client wrote. */
int powerfs_lock_pagecache_invalidate(struct inode *inode, __u64 start,
                                      __u64 end, bool full_inode);

#endif /* _POWERFS_LOCK_H */
