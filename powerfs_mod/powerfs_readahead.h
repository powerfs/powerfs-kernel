/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_readahead.h - ML 自适应预取 Phase A-0 (kernel 客户端)
 *
 * 详见 docs/ml-prefetch-kernel-rdma-plan.md §4.5-4.8:
 *   - 独立模块 (per-inode 策略缓存 + xattr 加载)
 *   - 三状态语义 (readahead_policy_cached / disabled)
 * xattr 协议 user.powerfs.readahead_policy = <MB 数>
 *
 * 目录级继承 (与 write_predict 对称):
 *   - 文件无 xattr 时递归查父目录 (最多 10 层), 命中则继承父目录策略
 *   - 文件级 xattr 优先于父目录 (可在单个文件上覆盖目录策略)
 *   - 在数据目录设一次 xattr, 所有子文件批量继承, 避免逐文件设置
 *
 * Phase A-0 (本文件): 手工基线, xattr 由 setfattr 设置, 无 ML
 * Phase A-1: filer 端 NN 训练 + 自动下发 (通过 version 字段失效)
 */
#ifndef _POWERFS_READAHEAD_H
#define _POWERFS_READAHEAD_H

#include <linux/types.h>
#include <linux/fs.h>

struct inode;
struct powerfs_inode_info;
struct powerfs_sb_info;

/* xattr 名 — 与 filer 端 / setfattr 共用 */
#define POWERFS_READAHEAD_XATTR_NAME "user.powerfs.readahead_policy"

/* mount option readahead= 的合法值 */
enum powerfs_readahead_mount_mode {
    POWERFS_READAHEAD_AUTO = 0,   /* 默认: 查 xattr, 无 xattr 用 VFS 默认 */
    POWERFS_READAHEAD_OFF,       /* off: 全局禁用 ML, 用 VFS 默认 */
};

/**
 * powerfs_readahead_apply - 在 read_iter 入口改 file->f_ra.ra_pages
 *
 * 调用时机: powerfs_file_read_iter 在 generic_file_read_iter 之前.
 * 不在 netfs issue_read 内调用 (subreq 已切好, 无法改 window).
 *
 * 行为:
 *   - 全局禁用 (mount readahead=off) 或 pi->readahead_policy_disabled=true:
 *     不改 file->f_ra (走 VFS 默认)
 *   - pi->readahead_policy_cached=true:
 *     用 pi->readahead_mb 改 file->f_ra.ra_pages (= readahead_mb * 1024 / PAGE_SIZE)
 *     readahead_mb=0 → ra_pages=0 关闭预取
 *   - pi->readahead_policy_cached=false:
 *     首次触发 xattr RPC 加载, 缓存结果; RPC 失败不阻塞 I/O, 用 VFS 默认
 *
 * 返回 0 = 成功 (file->f_ra 已调整或保持默认); <0 = 错误 (调用方忽略, 用 VFS 默认)
 */
int powerfs_readahead_apply(struct file *file, struct inode *inode);

/**
 * powerfs_readahead_invalidate - 标记 per-inode 策略缓存失效
 *
 * A-1 阶段: filer PushDelta 通知策略更新时调用, 促使下次 read_iter 重新查 xattr.
 * A-0 阶段: 不调用 (策略只在 setfattr 时手工变, 进程内重启即重新查).
 */
void powerfs_readahead_invalidate(struct inode *inode);

/**
 * powerfs_readahead_inode_init - inode 分配时初始化 readahead 字段
 *
 * 由 alloc_inode 调用, 字段归零 (cached=false, disabled 继承 sbi 全局).
 */
void powerfs_readahead_inode_init(struct powerfs_inode_info *pi);

/* ====================================================================
 * A-1.1 IO trace 采集 (详见 docs/ml-prefetch-kernel-rdma-plan.md §4.7)
 * ==================================================================== */

/* IO 种类 */
#define POWERFS_IO_TRACE_READ  0
#define POWERFS_IO_TRACE_WRITE 1

/* 采样率: 每 100 次记录 1 次 */
#define POWERFS_IO_TRACE_SAMPLE_RATE 100

/* ring buffer 大小 (最近 N 次访问) */
#define POWERFS_IO_TRACE_RING_SIZE  16

/* flush 阈值 */
#define POWERFS_IO_TRACE_FLUSH_BATCH  64   /* 累积 64 条触发 flush */
#define POWERFS_IO_TRACE_FLUSH_INTERVAL_MS 1000  /* 或每 1s flush */

/**
 * powerfs_io_trace_record - 记录一次 IO 访问 (采样 1/100)
 *
 * 调用时机: powerfs_file_read_iter / write_iter 入口.
 * 仅在 sample_counter % 100 == 0 时记录, 避免性能损失.
 *
 * @inode: 文件 inode
 * @offset: 本次 IO 的起始 offset
 * @kind: POWERFS_IO_TRACE_READ / WRITE
 *
 * 无锁: 用 WRITE_ONCE/READ_ONCE 保护字段, 并发安全 (best-effort trace).
 */
void powerfs_io_trace_record(struct inode *inode, loff_t offset, int kind);

/**
 * powerfs_io_trace_flush_init - 初始化全局 trace flush workqueue
 *
 * 由模块 init 调用, 启动定时 flush (1s).
 */
int powerfs_io_trace_flush_init(void);

/**
 * powerfs_io_trace_flush_exit - 停止 flush workqueue
 *
 * 由模块 exit 调用.
 */
void powerfs_io_trace_flush_exit(void);

#endif /* _POWERFS_READAHEAD_H */
