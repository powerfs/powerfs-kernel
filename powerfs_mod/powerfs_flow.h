/* SPDX-License-Identifier: GPL-2.0 */
/*
 * powerfs_flow.h - PowerFS kernel client flow control (Phase 2)
 *
 * 独立流控模块: 代码集中, 不分散到 net/fs 中, 便于调试和策略扩展.
 *
 * Phase 2 核心机制:
 *   1. 服务端在响应帧 flags bits 6-7 编码 load_factor (0-3)
 *   2. 客户端 pfs_rx_dispatch 解析 load_factor, 调 powerfs_flow_on_load_factor()
 *   3. VFS 入口 (lookup/readdir/write_begin) 调 powerfs_flow_admit() 排队
 *   4. admit() 根据 per-conn load_factor 自适应降低并发上限
 *
 * 设计参照: Ceph backoff、Lustre adaptive timeouts
 */
#ifndef POWERFS_FLOW_H
#define POWERFS_FLOW_H

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/time.h>

#include "powerfs_net.h"

/* ========== 常量 ========== */

/* flow_idx 映射: filer conns [0, MAX_FILERS), volume conns [MAX_FILERS, MAX_FILERS+MAX_VOLUMES) */
#define POWERFS_FLOW_MAX_CONNS  \
    (POWERFS_NET_MAX_FILERS + POWERFS_NET_MAX_VOLUMES)

/* 默认配置 */
#define POWERFS_FLOW_MAX_ACTIVE_GLOBAL    256  /* 全局最大并发请求 */
#define POWERFS_FLOW_MAX_ACTIVE_PER_CONN  64   /* per-conn 最大并发请求 */

/* EWMA 延迟参数 */
#define POWERFS_FLOW_EWMA_ALPHA_SHIFT  3  /* alpha = 1/8 */
#define POWERFS_FLOW_SLOW_LAT_THRESH_NS  (200 * NSEC_PER_MSEC)  /* 200ms → 标记慢连接 */

/* load_factor 等级 (对应服务端 flags bits 6-7) */
#define POWERFS_FLOW_LF_IDLE      0  /* 0-25%:  空闲, 无降速 */
#define POWERFS_FLOW_LF_NORMAL    1  /* 25-50%: 正常, 降到 75% */
#define POWERFS_FLOW_LF_BUSY      2  /* 50-75%: 较忙, 降到 50% */
#define POWERFS_FLOW_LF_OVERLOAD  3  /* 75%+:   满载, 降到 25% */

/* ========== 枚举 ========== */

/* 流控操作类型 (不同 op 可有不同策略) */
enum powerfs_flow_op {
    POWERFS_FLOW_OP_READ     = 0,  /* 读 (read_folio / read_iter) */
    POWERFS_FLOW_OP_WRITE    = 1,  /* 写 (write_begin / write_iter) */
    POWERFS_FLOW_OP_LOOKUP   = 2,  /* 元数据查找 (lookup / getattr) */
    POWERFS_FLOW_OP_READDIR  = 3,  /* 目录读取 */
    POWERFS_FLOW_OP_WRITEBACK = 4, /* 回写 (writepages) */
    POWERFS_FLOW_OP_LEASE    = 5,  /* lease 操作 (acquire/renew/release) */
    POWERFS_FLOW_OP_MAX,
};

/* 流控决策 */
enum powerfs_flow_decision {
    POWERFS_FLOW_ADMIT  = 0,  /* 允许执行 */
    POWERFS_FLOW_QUEUE  = 1,  /* 排队等待 (调用方短暂 sleep 后重试) */
    POWERFS_FLOW_REJECT = 2,  /* 拒绝 (返回 -EBUSY) */
};

/* ========== 统计结构 ========== */

/* Per-conn 统计 (全原子操作, 无锁) */
struct powerfs_flow_conn_stats {
    /* 在途请求 */
    atomic_t    active_reqs;
    /* 服务端负载因子 (0-3, pfs_rx_dispatch 更新) */
    atomic_t    server_load_factor;
    /* 慢连接标记 (EWMA 延迟超阈值) */
    atomic_t    slow;
    /* 累计统计 */
    atomic64_t  total_reqs;
    atomic64_t  total_errs;
    atomic64_t  total_bytes;
    atomic64_t  total_lat_ns;
    /* EWMA 平均延迟 (ns) */
    atomic64_t  ewma_lat_ns;
    /* 最近一次 load_factor 更新时间 (jiffies) */
    atomic_long_t lf_update_jiffies;
};

/* 全局统计 */
struct powerfs_flow_global {
    atomic_t    active_reqs;     /* 全局在途请求 */
    atomic_t    active_conns;    /* 活跃连接数 */
    atomic_t    slow_conns;      /* 慢连接数 */
    atomic64_t  total_reqs;
    atomic64_t  total_errs;
    atomic64_t  total_bytes_sent;
    atomic64_t  total_bytes_recv;
};

/* 流控控制器 (全局单例) */
struct powerfs_flow_controller {
    struct powerfs_flow_global   global;
    struct powerfs_flow_conn_stats conns[POWERFS_FLOW_MAX_CONNS];

    /* 配置 (可通过 debugfs 调整) */
    unsigned int  max_active_global;
    unsigned int  max_active_per_conn;
    /* 慢连接延迟阈值 (ns) */
    unsigned long slow_lat_thresh_ns;

    /* debugfs 根目录 */
    struct dentry *debugfs_root;
};

/* ========== API ========== */

/* 初始化 / 退出 (module_init/exit 调用) */
int  powerfs_flow_init(void);
void powerfs_flow_exit(void);

/* flow_idx 映射辅助函数 */
static inline int powerfs_flow_filer_idx(int filer_idx) {
    return filer_idx;
}
static inline int powerfs_flow_volume_idx(int vol_idx) {
    return POWERFS_NET_MAX_FILERS + vol_idx;
}

/*
 * powerfs_flow_admit - VFS 入口准入检查 (锁外调用)
 *
 * @op:      操作类型
 * @flow_idx: 连接索引 (powerfs_flow_filer_idx / powerfs_flow_volume_idx 返回值)
 *
 * 返回决策:
 *   ADMIT  - 允许执行, 调用方应接着调 powerfs_flow_record_start()
 *   QUEUE  - 排队, 调用方 usleep_range(100, 500) 后重试
 *   REJECT - 拒绝, 调用方返回 -EBUSY (仅在 load_factor=3 满载时)
 *
 * Phase 2: 根据 per-conn server_load_factor 自适应降低并发上限:
 *   lf=0 → 100%, lf=1 → 75%, lf=2 → 50%, lf=3 → 25%
 */
enum powerfs_flow_decision powerfs_flow_admit(enum powerfs_flow_op op, int flow_idx);

/*
 * powerfs_flow_record_start - 请求开始 (admit 后, 发送前调用)
 *
 * @flow_idx: 连接索引
 * @est_bytes: 预估字节数 (0=未知)
 */
void powerfs_flow_record_start(int flow_idx, unsigned int est_bytes);

/*
 * powerfs_flow_record_complete - 请求完成 (收到响应后调用)
 *
 * @flow_idx: 连接索引
 * @lat_ns:   端到端延迟 (ns)
 * @bytes:    实际传输字节数
 * @error:    是否出错 (true=错误)
 */
void powerfs_flow_record_complete(int flow_idx, u64 lat_ns, unsigned int bytes, bool error);

/*
 * powerfs_flow_on_load_factor - 收到服务端 load_factor 反馈
 *
 * 在 pfs_rx_dispatch 中调用: 从响应帧 flags bits 6-7 提取 load_factor.
 * 存储到 per-conn 统计, admit() 据此自适应调整.
 *
 * @flow_idx: 连接索引
 * @lf:       服务端负载因子 (0-3)
 */
void powerfs_flow_on_load_factor(int flow_idx, u8 lf);

/*
 * powerfs_flow_conn_active - 获取 per-conn 在途请求数 (debugfs/日志用)
 */
int powerfs_flow_conn_active(int flow_idx);

/*
 * powerfs_flow_conn_load_factor - 获取 per-conn 当前 load_factor (debugfs 用)
 */
u8 powerfs_flow_conn_load_factor(int flow_idx);

#endif /* POWERFS_FLOW_H */
