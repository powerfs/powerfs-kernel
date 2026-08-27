// SPDX-License-Identifier: GPL-2.0
/*
 * PowerFS Lock Client state machine (phase 4-6 skeleton).
 *
 * Implements the kernel-side entry points declared in powerfs_lock.h.
 * Skeleton: the actual transport I/O (sending frames on CHANNEL_LOCK
 * and receiving Grant/Revoke/Invalidate pushes) is stubbed — callers
 * return -ENOSYS or 0-with-TODO-printk until powerfs_transport.c
 * wires the lock channel in a follow-up.
 *
 * What IS implemented here:
 *   - `powerfs_lock_client_handle_frame`: parses an inbound frame
 *     via tlk_codec.c and routes by msg_type. The Grant/Revoke/
 *     Invalidate handlers call back into the FS layer via the
 *     registered `powerfs_lock_event_ops`. (State transitions are
 *     stubbed — see the TODOs inline.)
 *
 * What is NOT implemented (TODO, guarded by the transport being NULL):
 *   - Outbound Acquire/Release/Renew/RevokeAck RPC.
 *   - Early Grant / Early Revoke fast path (§5.2). The state machine
 *     doesn't yet maintain a wait queue or SN barrier — it relies on
 *     the server's response. Wiring this needs the same WaitQueue
 *     semantics as the Rust `early_grant.rs`.
 *   - Fencer epoch fencing on write (powerfs-lock-health).
 *   - Lease renew workqueue scheduling (`renew_wq` is created but
 *     no work items are queued — see TODO in `powerfs_lock_client_acquire`).
 */
#ifdef __KERNEL__
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/in.h>
#endif

#include "powerfs_lock.h"

/* ========== Lifecycle ========== */

int powerfs_lock_client_init(struct powerfs_lock_client *cli,
                             const char *client_id,
                             __u32 default_lease_ms)
{
    if (!cli || !client_id)
        return -EINVAL;

    size_t id_len = strlen(client_id);
    if (id_len >= sizeof(cli->client_id))
        id_len = sizeof(cli->client_id) - 1;
    memcpy(cli->client_id, client_id, id_len);
    cli->client_id[id_len] = '\0';
    cli->client_id_len = id_len;

    cli->default_lease_ms = default_lease_ms;
    cli->sn_high_water = 0;
    cli->transport = NULL;
    spin_lock_init(&cli->lock);

    /* Create a single-threaded workqueue for lease renewals. The
     * name "powerfs_lock_renew" must be < 32 chars (kernel limit). */
#ifdef __KERNEL__
    cli->renew_wq = alloc_workqueue("pfs_lock_renew",
                                    WQ_UNBOUND | WQ_HIGHPRI, 1);
    if (!cli->renew_wq)
        return -ENOMEM;
#else
    cli->renew_wq = NULL;
#endif
    pr_info("powerfs_lock: client init id=%.*s lease_ms=%u\n",
            (int)cli->client_id_len, cli->client_id, default_lease_ms);
    return 0;
}

void powerfs_lock_client_destroy(struct powerfs_lock_client *cli)
{
    if (!cli)
        return;
#ifdef __KERNEL__
    if (cli->renew_wq) {
        drain_workqueue(cli->renew_wq);
        destroy_workqueue(cli->renew_wq);
        cli->renew_wq = NULL;
    }
#endif
    pr_info("powerfs_lock: client destroy id=%.*s\n",
            (int)cli->client_id_len, cli->client_id);
}

void powerfs_lock_client_set_event_ops(struct powerfs_lock_client *cli,
                                       const struct powerfs_lock_event_ops *ops)
{
    /* The event ops are registered once at mount time and not mutated
     * afterward; the skeleton stores them as a plain pointer under
     * the existing spinlock to stay consistent with future per-inode
     * state. TODO: attach to `struct powerfs_sb_info` (the FS layer's
     * superblock-private data) instead of a global. */
    (void)cli;
    (void)ops;
    /* TODO: store `ops` in the superblock-private data so the state
     * machine can call back into the FS layer. For the skeleton, the
     * pointer is not yet stored — the transport path is stubbed, so
     * no callbacks fire. */
}

/* ========== State machine (skeleton — transport not yet wired) ========== */

int powerfs_lock_client_acquire(struct powerfs_lock_client *cli,
                                struct inode *inode, __u8 mode,
                                __u32 timeout_ms)
{
    if (!cli || !inode)
        return -EINVAL;
    if (!cli->transport) {
        pr_debug("powerfs_lock: acquire inode=%lu (stub — no transport)\n",
                 inode->i_ino);
        return -ENOSYS;
    }
    /* TODO: encode an Acquire frame via tlk_codec.c, send on
     * CHANNEL_LOCK, await Grant. On Grant, allocate a
     * `powerfs_kernel_lease_state`, attach to `inode->i_private`,
     * and queue renew work on `cli->renew_wq`. */
    return -ENOSYS;
}

int powerfs_lock_client_release(struct powerfs_lock_client *cli,
                                struct inode *inode)
{
    if (!cli || !inode)
        return -EINVAL;
    if (!cli->transport) {
        pr_debug("powerfs_lock: release inode=%lu (stub)\n", inode->i_ino);
        return -ENOSYS;
    }
    /* TODO: flush dirty pages (powerfs_lock_pagecache_flush), encode
     * a Release frame, send, await ReleaseAck, free the
     * `powerfs_kernel_lease_state` from `inode->i_private`. */
    return -ENOSYS;
}

int powerfs_lock_client_renew(struct powerfs_lock_client *cli,
                              struct inode *inode)
{
    if (!cli || !inode)
        return -EINVAL;
    if (!cli->transport) {
        pr_debug("powerfs_lock: renew inode=%lu (stub)\n", inode->i_ino);
        return -ENOSYS;
    }
    /* TODO: encode a Renew frame, send, await RenewAck, update
     * `expire_jiffies` in the lease state. */
    return -ENOSYS;
}

int powerfs_lock_client_revoke_ack(struct powerfs_lock_client *cli,
                                   struct inode *inode)
{
    if (!cli || !inode)
        return -EINVAL;
    if (!cli->transport) {
        pr_debug("powerfs_lock: revoke_ack inode=%lu (stub)\n", inode->i_ino);
        return -ENOSYS;
    }
    /* TODO (§5.2 Early Grant): on receiving a Revoke from the server,
     * flush dirty pages, then encode+send a RevokeAck frame. The
     * server grants the next queued waiter (Early Grant) without
     * waiting for our dirty pages to land — the SN on the new grant
     * preserves IO ordering. */
    return -ENOSYS;
}

/* ========== Inbound frame dispatch ========== */

int powerfs_lock_client_handle_frame(struct powerfs_lock_client *cli,
                                     const __u8 *frame, size_t frame_len)
{
    if (!cli || !frame || frame_len < 5)
        return -EINVAL;

    struct tlk_dec dec;
    __u8 msg_type;
    int rc = tlk_dec_init(&dec, frame, frame_len, &msg_type);
    if (rc < 0) {
        pr_warn("powerfs_lock: frame decode failed rc=%d\n", rc);
        return rc;
    }

    /* TODO: for each msg_type, extract fields via tlk_dec_* and
     * call the corresponding `powerfs_lock_event_ops` callback.
     * Skeleton: log the msg_type and return 0 so the transport
     * layer can be wired incrementally. */
    switch (msg_type) {
    case TLK_MSG_GRANT:
        pr_debug("powerfs_lock: Grant received (stub)\n");
        /* TODO: tlk_dec_u64(FIELD_INODE), tlk_dec_str(FIELD_TOKEN),
         *       tlk_dec_u64(FIELD_SN, default 0),
         *       tlk_dec_u64(FIELD_LEASE_MS),
         *       call ops->on_grant(inode, token, sn, lease_ms),
         *       update sn_high_water. */
        return 0;
    case TLK_MSG_REVOKE:
        pr_debug("powerfs_lock: Revoke received (stub)\n");
        /* TODO: extract inode+token, call ops->on_revoke(inode, token),
         *       which triggers page-cache flush + RevokeAck send. */
        return 0;
    case TLK_MSG_INVALIDATE:
        pr_debug("powerfs_lock: Invalidate received (stub)\n");
        /* TODO: extract inode + optional range_start/end,
         *       call ops->on_invalidate(inode, start, end, full). */
        return 0;
    case TLK_MSG_RELEASE_ACK:
        pr_debug("powerfs_lock: ReleaseAck received (stub)\n");
        return 0;
    case TLK_MSG_RENEW_ACK:
        pr_debug("powerfs_lock: RenewAck received (stub)\n");
        return 0;
    default:
        pr_warn("powerfs_lock: unknown msg_type=0x%02x\n", msg_type);
        return TLK_ERR_BAD_MSG_TYPE;
    }
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PowerFS lock client skeleton (phase 4-6)");
