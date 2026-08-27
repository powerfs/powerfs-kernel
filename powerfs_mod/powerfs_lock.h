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

#if 0 /* DEAD_CODE — entire lock_client / MDLock subsystem disabled, audit 2026-08-23.
     *
     * == Architecture violation ==
     * This header compiled in two parallel subsystems that were NEVER wired
     * to the kernel VFS or powerfs-net transport:
     *
     *  (a) The TLV wire codec (tlk_codec.c/.h) defined an INDEPENDENT byte
     *      protocol — {msg_type byte, LE u32 length, TLK_FIELD tagged body}
     *      — whose frame format has NO overlap with the powerfs-net frame
     *      format {magic(4), version(2), msg_type(u16), TLV FieldId(1B)+len(4B)
     *      actually used on the wire for MsgType 0x91 (CapOpenGrant) ~ 0x95
     *      (CapUpgradeNotify). Hence tlk_codec could never interoperate with
     *      the Rust lock_arbiter / cap_manager.
     *
     *  (b) The per-inode MDLock state machine (struct powerfs_mdlock, 8
     *      locks per inode) + its API (rdlock/wrlock/xlock/unlock/eval)
     *      was an attempt to replicate the Ceph MDS LOCKER server-side
     *      arbitration inside the kernel client. That breaks the Ceph
     *      invariant: lock arbitration / GATHER / Loner promotion is the
     *      exclusive responsibility of the Filer leader (lock_arbiter.rs).
     *      Clients must ONLY keep a lightweight per-inode cache of
     *      {issued_cap_mask, wanted_mask, epoch, token, sn} (compare
     *      powerfs-fuse/src/client_cap.rs struct ClientCap). Code audit
     *      found ZERO callers of mdlock_rdlock/wrlock/xlock from any VFS
     *      path, confirming this was never connected.
     *
     *  (c) The lock_client.c (per-mount powerfs_lock_client) skeleton
     *      returns -ENOSYS for every acquire/release/renew/revoke_ack
     *      when transport == NULL, and transport was never wired up.
     *
     * These files (powerfs_lock.h, tlk_codec.h, tlk_codec.c, lock_client.c,
     * test_tlk_codec.c) are preserved in-tree purely for source-code history
     * / future audit — they are no longer linked into powerfs.ko and the
     * Makefile's $(MODULE_NAME)-objs no longer reference them. The
     * independent `make test_codec` userspace round-trip target still
     * builds because it enumerates tlk_codec.c + test_tlk_codec.c
     * explicitly on the command line (no kernel .o linkage).
     *
     * If lock client work is ever re-started on the kernel side, align on:
     *   - Wire layer:  REUSE the existing powerfs-net MsgType 0x91~0x95 path
     *                  (powerfs_net_cap_open_grant / recall_ack / ...) that is
     *                  ALREADY working and ALREADY calls lock_arbiter.rs.
     *   - State layer: CLIENT-SIDE ONLY — cache issued/wanted/epoch/sn
     *                  per inode; do NOT reimplement the Locker state machine
     *                  (LONER / SHARED / GATHER / EXCL promotion).
     */

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

/* ==============================================================
 * MDLock — 独立锁对象 (对齐 Ceph MDS Locker 4 套状态机)
 *
 * 参考: src/mds/SimpleLock.h, src/mds/ScatterLock.h,
 *        src/mds/FileLock.h, src/mds/LocalLock.h
 * 设计文档: docs/mdlock-design.md
 *
 * Ceph 有 4 套独立锁状态机, 每套有不同的状态集和行为:
 * - LocalLock:  MDS 本地锁, 不涉及客户端 cap, 仅二态 AVAIL/LOCK
 * - SimpleLock: 排他写+共享读, rdlock/wrlock 原语, 支持 Loner
 * - ScatterLock: 多方共享写, MDS 间合并变更, 不输出客户端 cap
 * - FileLock: 扩展 SimpleLock, 完整 Loner + FILE cap 语义 + SYNC
 *
 * 每个 inode 持有 POWERFS_NUM_LOCK_TYPES 把独立锁对象,
 * 每把锁属于 4 套状态机之一, 有独立状态和 eval 行为.
 * chmod 只操作 AUTH 锁(SimpleLock), 完全不碰 FILE 锁(FileLock).
 *
 * Phase 1: 定义 + 内核端接通 (设计文档 §13 Phase 1)
 * ============================================================== */

/* 前向声明 */
struct powerfs_inode_info;

/* 锁状态机类别 — 对齐 Ceph 4 套锁状态机
 * 参考: src/mds/SimpleLock.h, ScatterLock.h, FileLock.h, LocalLock.h
 * 决定该锁使用哪套状态机和 eval 行为 */
enum powerfs_lock_class {
    LOCK_CLASS_LOCAL   = 0,  /* LocalLock:  MDS 本地锁, 无客户端 cap */
    LOCK_CLASS_SIMPLE  = 1,  /* SimpleLock: 排他写+共享读, 支持 Loner */
    LOCK_CLASS_SCATTER = 2,  /* ScatterLock: 多方共享写, MDS 间合并 */
    LOCK_CLASS_FILE    = 3,  /* FileLock: 扩展 SimpleLock + 完整 FILE cap */
};

/* PowerFS MDLock 类型 — 对齐 Ceph MDS lock types
 * 参考: src/mds/SimpleLock.h LockType, src/mds/locks.cc */
enum powerfs_lock_type {
    /* SimpleLock 类型 (排他写, 共享读) */
    POWERFS_LOCK_AUTH    = 0,  /* IAUTH:  inode 权限 (mode/uid/gid) */
    POWERFS_LOCK_LINK    = 1,  /* ILINK:   硬链接计数 (nlink) */
    POWERFS_LOCK_XATTR   = 2,  /* IXATTR:  扩展属性 (xattr) */
    POWERFS_LOCK_DN      = 3,  /* DN:      dentry 名称解析 (含 lease) */

    /* LocalLock 类型 (MDS 本地锁, 不涉及客户端 cap) */
    POWERFS_LOCK_SNAP    = 4,  /* ISNAP:   快照 (LocalLock) */

    /* FileLock 类型 (扩展 SimpleLock, 完整 Loner + FILE cap) */
    POWERFS_LOCK_FILE    = 5,  /* IFILE:   文件数据 (read/write/truncate) */

    /* ScatterLock 类型 (多方共享写, MDS 间合并) */
    POWERFS_LOCK_DFT     = 6,  /* IDFT:    目录分片 (dirfrag) */
    POWERFS_LOCK_NEST    = 7,  /* INEST:   嵌套目录 */

    POWERFS_NUM_LOCK_TYPES = 8,
};

/* MDLock 状态 — 合并 4 套状态机的所有状态
 * 参考: src/mds/SimpleLock.h, ScatterLock.h, FileLock.h, LocalLock.h
 *
 * 不同 class 使用不同的状态子集:
 *
 * LocalLock:     AVAILABLE, LOCK
 * SimpleLock:    AVAILABLE, SHARED, LONER, EXCL, GATHER, REVOKING
 * ScatterLock:   AVAILABLE, DSCATTER, EXCL, INACTIVE, SYNC, GATHER
 * FileLock:      AVAILABLE, SHARED, LONER, EXCL, GATHER, REVOKING, SYNC
 *
 * 状态转移图 (SimpleLock + FileLock):
 *
 *   AVAILABLE ──rdlock──> SHARED ──wrlock(单client)──> LONER
 *                            │                           │
 *                            │ wrlock(多client)           │ 新client
 *                            ▼                           ▼
 *                          SHARED <──recall ack── GATHER ──xlock──> EXCL
 *                                                      ▲
 *                                                      │
 *   EXCL ──unlock──> GATHER (等待所有 cap ACK) ──done──> AVAILABLE
 *
 * FileLock 额外:
 *   SHARED ──flush──> SYNC (只读, cap 已写回) ──recall──> AVAILABLE
 *   SYNC ──new write──> SHARED
 *
 * ScatterLock:
 *   AVAILABLE ──wrlock──> DSCATTER (多方共享写) ──xlock──> GATHER ──> EXCL
 *   DSCATTER ──inactive──> INACTIVE
 *   EXCL ──unlock──> SYNC ──recall──> AVAILABLE
 *
 * LocalLock:
 *   AVAILABLE ──lock──> LOCK ──unlock──> AVAILABLE
 */
enum powerfs_lock_state {
    /* === 共享状态 === */
    LOCK_ST_AVAILABLE = 0,   /* 无锁, 无持有者 (所有 class) */

    /* === SimpleLock + FileLock 状态 === */
    LOCK_ST_SHARED    = 1,   /* 共享态: 多方并发读 */
    LOCK_ST_LONER     = 2,   /* Loner 独占优化: 仅 1 client, 下发 exclusive cap */
    LOCK_ST_EXCL      = 3,   /* 完全独占: xlock 持有 */
    LOCK_ST_GATHER    = 4,   /* 正在收集 recall ACK */
    LOCK_ST_REVOKING  = 5,   /* 正在部分撤销 (recall 子集 cap) */

    /* === LocalLock 状态 === */
    LOCK_ST_LOCK      = 6,   /* LocalLock 独占: MDS 本地锁, 无客户端 cap */

    /* === ScatterLock 状态 === */
    LOCK_ST_DSCATTER  = 7,   /* ScatterLock 散射态: 多方共享写, MDS 间合并 */
    LOCK_ST_INACTIVE  = 8,   /* ScatterLock 非活跃: 无活跃 holder */
    LOCK_ST_SYNC      = 9,   /* 同步态: 只读, cap 已写回 (FileLock SYNC + ScatterLock SYNC_SCATTER) */
};

/* 锁原语类型 — 对齐 Ceph MDS Locker rdlock/wrlock/xlock */
enum powerfs_lock_op {
    LOCK_OP_RD       = 0,   /* rdlock: 共享读, 多方并发 */
    LOCK_OP_WR       = 1,   /* wrlock: 排他写 (SimpleLock 排他) */
    LOCK_OP_X        = 2,   /* xlock: 完全独占, 必须 recall 全部 cap */
    LOCK_OP_REMOTE_WR = 3,  /* remote_wrlock: 跨 shard 锁请求 */
};

/* GATHER 目标状态 — recall ACK 收齐后跃迁到哪个状态 */
enum powerfs_gather_target {
    GATHER_TO_EXCL    = 0,   /* xlock: 收齐后 → EXCL */
    GATHER_TO_SHARED  = 1,   /* 降级: 收齐后 → SHARED */
    GATHER_TO_LONER   = 2,   /* 升级: 收齐后 → LONER */
    GATHER_TO_AVAIL   = 3,   /* 释放: 收齐后 → AVAILABLE */
};

/* 锁持有者记录 — 每个 client session 持有一把锁的记录
 * 对齐 Ceph: std::set<client_id> simple_lock_t::wrlocks/gather */
struct powerfs_lock_holder {
    struct list_head list;          /* 挂到 mdlock->holders */

    __u64    client_id;             /* client session id */
    __u64    sn;                    /* 授予时的序列号 (fencing) */
    __u64    epoch;                 /* fencer epoch */
    __u32    duration_ms;           /* lease TTL */
    unsigned long expire_jiffies;   /* 过期时间 */

    /* 该 holder 在该锁上被授予的 cap 位 (eval 输出) */
    unsigned int granted_caps;

    /* 该 holder 在该锁上的 dirty caps (未写回的脏位) */
    unsigned int dirty_caps;

    /* recall 状态 */
    bool recall_in_flight;          /* 已发 recall, 等待 ACK */
    unsigned int recall_caps;       /* 正在 recall 的位 */
    unsigned int retain_caps;       /* recall 后保留的位 */

    /* back pointer */
    struct powerfs_mdlock *lock;
};

/* GATHER 等待项 — xlock/wrlock 等待 recall ACK 时的追踪
 * 对齐 Ceph: SimpleLock::gather_set */
struct powerfs_lock_gather {
    struct list_head list;          /* 挂到 mdlock->gather_list */
    __u64    client_id;             /* 等待 ACK 的 client */
    __u64    sn;                    /* recall 消息的序列号 */
    unsigned long sent_jiffies;     /* recall 发送时间 (timeout) */
    bool   acked;                   /* 是否已收到 ACK */
};

/* 锁请求等待项 — 被阻塞的锁请求排队 */
struct powerfs_lock_waiter {
    struct list_head list;          /* 挂到 mdlock->waiting */
    enum powerfs_lock_op   op;      /* 请求的原语 */
    __u64    client_id;             /* 请求方 client id */
    __u64    sn;                    /* 分配的 sn (fencing) */
    int      state;                 /* 等待状态 (0=active, -EINTR=cancelled) */
};

/* 独立锁对象 — 对齐 Ceph SimpleLock/FileLock/ScatterLock
 *
 * 每个 inode 持有 POWERFS_NUM_LOCK_TYPES 把 mdlock, 各自独立状态机.
 * 参考: src/mds/SimpleLock.h class SimpleLock */
struct powerfs_mdlock {
    /* 基本标识 */
    enum powerfs_lock_type  type;    /* 锁类型 (AUTH/LINK/.../FILE) */
    enum powerfs_lock_class cls;     /* 状态机类别 (LOCAL/SIMPLE/SCATTER/FILE) */
    enum powerfs_lock_state state;   /* 当前锁状态 (因 cls 而异) */

    /* 持有者列表 — 当前持有该锁的 client session 集合 */
    struct list_head holders;        /* powerfs_lock_holder 链表 */
    int holder_count;               /* 快速计数 */

    /* GATHER 等待列表 — 正在等待 recall ACK 的 client 集合 */
    struct list_head gather_list;    /* powerfs_lock_gather 链表 */
    int gather_remaining;           /* 剩余待 ACK 数 */
    enum powerfs_gather_target gather_target; /* ACK 收齐后目标状态 */

    /* 等待者队列 — 被阻塞的锁请求 */
    struct list_head waiting;        /* powerfs_lock_waiter 链表 */

    /* 权限评估结果 (eval 输出, 缓存) */
    unsigned int eval_issued;        /* 所有 holder issued 的并集 */
    unsigned int eval_wanted;       /* 所有 holder wanted 的并集 */

    /* back pointer */
    struct powerfs_inode_info *pi;
};

/* ========== MDLock 原语 API (对齐 Ceph MDS Locker) ========== */

/* rdlock: 获取共享读锁
 * - SimpleLock: 多方可同时 rdlock
 * - 如果当前 EXCL/GATHER 状态: 阻塞等待
 * 返回 0 成功, -EAGAIN 超时, -EINTR 中断 */
int powerfs_mdlock_rdlock(struct powerfs_inode_info *pi,
                          enum powerfs_lock_type type,
                          __u64 client_id,
                          struct powerfs_lock_holder **holder_out);

/* wrlock: 获取排他写锁
 * - SimpleLock: 排他, 只能 1 个 writer
 * - 如果 LONER 且同 client: 复用 (fast path)
 * - 如果其他人持有: → GATHER → recall → 降级后授予 */
int powerfs_mdlock_wrlock(struct powerfs_inode_info *pi,
                          enum powerfs_lock_type type,
                          __u64 client_id,
                          struct powerfs_lock_holder **holder_out);

/* xlock: 获取完全独占锁
 * - 必须 recall 该锁类型对应的全部 client cap
 * - rename/unlink/truncate/migrate 必须先拿 xlock
 * - xlock 持有期间禁止下发任何 dirty cap */
int powerfs_mdlock_xlock(struct powerfs_inode_info *pi,
                         enum powerfs_lock_type type,
                         __u64 client_id,
                         struct powerfs_lock_holder **holder_out);

/* unlock: 释放锁
 * - 从 EXCL 释放: → GATHER → 等待 flush 完成 → AVAILABLE
 * - 从 SHARED/LONER 释放: 直接移除 holder, 若 0 holder → AVAILABLE
 * - 从 LOCK (LocalLock) 释放: → AVAILABLE */
int powerfs_mdlock_unlock(struct powerfs_inode_info *pi,
                          enum powerfs_lock_type type,
                          __u64 sn);

/* recall_ack: 客户端 ACK recall, GATHER 计数减一
 * - gather_remaining == 0 时触发 on_gather_complete */
int powerfs_mdlock_recall_ack(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 client_id, __u64 sn);

/* ===== trylock 非阻塞版本 (对齐 Ceph: try_rdlock/try_wrlock) ===== */

/* try_rdlock: 非阻塞共享读锁, 不可获取时立即返回 -EAGAIN */
int powerfs_mdlock_try_rdlock(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 client_id,
                              struct powerfs_lock_holder **holder_out);

/* try_wrlock: 非阻塞排他写锁, 不可获取时立即返回 -EAGAIN */
int powerfs_mdlock_try_wrlock(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 client_id,
                              struct powerfs_lock_holder **holder_out);

/* try_xlock: 非阻塞完全独占锁, 不可获取时立即返回 -EAGAIN */
int powerfs_mdlock_try_xlock(struct powerfs_inode_info *pi,
                             enum powerfs_lock_type type,
                             __u64 client_id,
                             struct powerfs_lock_holder **holder_out);

/* ===== LocalLock 专用原语 (对齐 Ceph: LocalLock::lock/unlock) ===== */

/* local_lock: LocalLock 独占锁 (MDS 本地, 无客户端 cap)
 * 状态: AVAILABLE → LOCK
 * 用于 ISNAP 等不需要客户端协调的操作 */
int powerfs_mdlock_local_lock(struct powerfs_inode_info *pi,
                               enum powerfs_lock_type type);

/* local_unlock: 释放 LocalLock
 * 状态: LOCK → AVAILABLE */
int powerfs_mdlock_local_unlock(struct powerfs_inode_info *pi,
                                 enum powerfs_lock_type type);

/* ===== ScatterLock 专用原语 (对齐 Ceph: ScatterLock::scatter/unscatter) ===== */

/* scatter_wrlock: ScatterLock 共享写锁 (多方共享写, MDS 间合并)
 * 状态: AVAILABLE → DSCATTER (多方共享写)
 * 用于 IDFT/INEST 大目录分片 */
int powerfs_mdlock_scatter_wrlock(struct powerfs_inode_info *pi,
                                   enum powerfs_lock_type type,
                                   __u64 client_id,
                                   struct powerfs_lock_holder **holder_out);

/* scatter_unlock: 释放 ScatterLock 共享写
 * DSCATTER → INACTIVE (无活跃 holder) → AVAILABLE */
int powerfs_mdlock_scatter_unlock(struct powerfs_inode_info *pi,
                                  enum powerfs_lock_type type,
                                  __u64 sn);

/* ===== FileLock 专用原语 (对齐 Ceph: FileLock::flush_to_sync/sync_to_shared) ===== */

/* filelock_flush_to_sync: FileLock SHARED → SYNC
 * 客户端 flush 脏数据后调用, cap 降级为只读 */
int powerfs_mdlock_file_flush_to_sync(struct powerfs_inode_info *pi,
                                       enum powerfs_lock_type type,
                                       __u64 client_id);

/* filelock_sync_to_shared: FileLock SYNC → SHARED
 * 新写请求到来时从 SYNC 升级回 SHARED */
int powerfs_mdlock_file_sync_to_shared(struct powerfs_inode_info *pi,
                                        enum powerfs_lock_type type,
                                        __u64 client_id);

/* ===== sn fencing 机制 (对齐 Ceph: epoch + sn fencing) ===== */

/* 验证 sn 是否有效 (epoch 匹配)
 * 返回 true 如果 sn 属于当前 epoch (未被 fence) */
bool powerfs_mdlock_sn_valid(struct powerfs_inode_info *pi,
                              enum powerfs_lock_type type,
                              __u64 sn);

/* 强制 fencing: epoch bump, 使所有旧 sn 失效
 * 用于 recall 超时后的 force-reclaim */
void powerfs_mdlock_fence_epoch(struct powerfs_inode_info *pi,
                                 enum powerfs_lock_type type);

/* ===== eval: 锁状态 → cap 掩码 (事件驱动权限评估) ===== */

/* eval: 锁状态 → cap 掩码 (事件驱动权限评估)
 * 对齐 Ceph: SimpleLock::eval()
 * 触发: 锁状态变迁 / recall ACK 收齐 / holder 加入/移除 */
void powerfs_mdlock_eval(struct powerfs_mdlock *lock);

/* mdlock → cap: 聚合所有锁的 eval_issued 到 cap.issued */
unsigned int powerfs_mdlocks_to_cap_issued(struct powerfs_inode_info *pi);

/* cap → mdlock: 从收到的 CapRecall 消息拆解到各锁 */
void powerfs_cap_revoke_to_mdlocks(struct powerfs_inode_info *pi,
                                   unsigned int revoking);

/* ===== 初始化与清理 ===== */

/* 初始化 inode 的所有 mdlock 实例 */
void powerfs_init_mdlocks(struct powerfs_inode_info *pi);

/* 销毁 inode 的所有 mdlock 实例 (释放 holders/gather/waiters)
 * 在 powerfs_destroy_inode 中调用 */
void powerfs_destroy_mdlocks(struct powerfs_inode_info *pi);

/* ===== eval 触发: 过期 holder 清理 (对齐 Ceph: MDS tick) ===== */

/* 清理所有锁的过期 holder + 超时 GATHER
 * 应在定时器或关键路径中调用 */
void powerfs_mdlock_tick(struct powerfs_inode_info *pi);

/* ===== recall 发送回调 (通过函数指针, 解耦传输层) ===== */

/* recall 发送回调函数类型
 * 参数: inode, lock_type, client_id, sn, recall_caps, retain_caps
 * 返回: 0 成功, <0 发送失败 */
struct powerfs_mdlock_recall_ops {
    int (*send_recall)(struct inode *inode, enum powerfs_lock_type type,
                       __u64 client_id, __u64 sn,
                       unsigned int recall_caps, unsigned int retain_caps);
};

/* 注册 recall 发送回调 (模块初始化时调用) */
void powerfs_mdlock_set_recall_ops(const struct powerfs_mdlock_recall_ops *ops);

/* ===== 调试: lock status dump ===== */

/* dump 指定 inode 的所有锁状态到 buffer
 * 返回写入的字数, <0 表示 buffer 不够 */
int powerfs_mdlock_dump(struct powerfs_inode_info *pi, char *buf, int buflen);

/* dump 指定锁的状态 */
int powerfs_mdlock_dump_one(struct powerfs_mdlock *lock, char *buf, int buflen);

/* Recall 超时常量 (对齐 cap_manager.rs DEFAULT_RECALL_TIMEOUT_MS) */
#define MDLOCK_RECALL_TIMEOUT_MS  2000

/* 锁类型 → cap 位掩码映射 (eval 使用) */
extern const unsigned int powerfs_lock_type_cap_bits[POWERFS_NUM_LOCK_TYPES];

/* 锁类型 → 状态机类别映射
 * 对齐 Ceph: IAUTH/ILINK/IXATTR/DN=SimpleLock, ISNAP=LocalLock,
 * IFILE=FileLock, IDFT/INEST=ScatterLock */
extern const enum powerfs_lock_class powerfs_lock_type_class[POWERFS_NUM_LOCK_TYPES];

#endif /* DEAD_CODE — lock_client / MDLock subsystem (see block comment at L38) */

#endif /* _POWERFS_LOCK_H */
