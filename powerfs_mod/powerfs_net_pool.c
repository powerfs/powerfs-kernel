/* SPDX-License-Identifier: GPL-2.0 */
/* powerfs_net_pool.c - split from powerfs_net.c (mechanical refactor) */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/un.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/hashtable.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/inet.h>
#include <linux/statfs.h>
#include <linux/rbtree.h>
#include <linux/kref.h>
#include <linux/unaligned.h>
#include <linux/crc32.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include "powerfs_ec.h"

#include <net/sock.h>
#include <net/tcp.h>
#include <net/net_namespace.h>
#include <net/inet_sock.h>

#include "powerfs_net.h"
#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_flow.h"

#include "powerfs_net_internal.h"

/* ========== 初始化/清理 ========== */

/**
 * powerfs_net_init - 初始化 powerfs-net 子系统
 *
 * 仅初始化 CRC32C 表和 discover 序列号. 实际的 per-filer 连接池由
 * powerfs_conn_pool_init (在 fill_super 中) 创建, 由 powerfs_net_pool_init
 * 初始化 g_pool 基础字段 (servers[], pool_lock 等).
 */
int powerfs_net_init(void)
{
    /* 初始化 CRC32C 表 */
    if (!crc32c_table_init)
        powerfs_crc32c_init_table();

    atomic_set(&g_discover_seq, 0);

    g_initialized = true;

    pr_debug("powerfs: net subsystem initialized\n");
    return 0;
}

/**
 * powerfs_net_exit - 清理 powerfs-net 子系统
 *
 * 兜底清理: 若 umount 路径未执行 (直接 rmmod), 这里调用 pool_exit
 * 停止新架构连接池 (per-filer 连接 + 调度器线程).
 */
void powerfs_net_exit(void)
{
    if (!g_initialized)
        return;

    /* 清理连接池 (销毁 pool mutex)
     * 注意: kill_sb 已调用 pool_cleanup，但 pool_exit 还没调用；
     *       如果 umount 没执行（直接 rmmod），这里兜底 */
    powerfs_net_pool_exit();

    g_initialized = false;

    pr_debug("powerfs: net subsystem exited\n");
}

/* ========== 多连接池实现 ========== */

/* 全局连接池状态
 * (g_pool 已在文件顶部声明, 这里仅声明 g_pool_initialized) */
bool g_pool_initialized = false;

/**
 * powerfs_net_pool_init - 初始化连接池
 */
int powerfs_net_pool_init(void)
{
    int i;

    /* v2: 防御性释放可能残留的调度器 (remount 场景: 若上次的 conn_pool_exit
     * 未执行, schedulers 可能仍分配). memset 会清零指针导致泄漏, 故先释放.
     * sched_exit 是幂等的 (schedulers==NULL 时 no-op). */
    powerfs_sched_exit();

    /* 初始化连接池 */
    memset(&g_pool, 0, sizeof(g_pool));
    mutex_init(&g_pool.pool_lock);
    atomic_set(&g_pool.active_filer_idx, 0);
    atomic_set(&g_pool.active_master_idx, 0);
    atomic_set(&g_pool.active_volume_idx, 0);
    atomic_set(&g_pool.leader_idx, -1);
    atomic_set(&g_pool.leader_known, 0);
    atomic_set(&g_pool.failover_count, 0);

    /* 初始化服务器条目 */
    for (i = 0; i < POWERFS_NET_MAX_SERVERS; i++) {
        memset(&g_pool.servers[i], 0, sizeof(g_pool.servers[i]));
        g_pool.servers[i].last_check_time = jiffies;
    }

    g_pool_initialized = true;

    pr_debug("powerfs: connection pool initialized\n");
    return 0;
}

/**
 * powerfs_net_pool_exit - 清理连接池
 */
void powerfs_net_pool_exit(void)
{
    if (!g_pool_initialized)
        return;

    /* 完整清理连接池: 取消 heartbeat/reconnect delayed_work (防 timer wheel
     * 卸载后访问已释放模块内存 → page fault panic) + 断开连接 + 停止调度器
     * 线程 (pfs_rx/pfs_vrx/pfs_tx, conn_pool_exit 内部调 powerfs_sched_exit)
     * + 销毁 reconn_wq. 正常 umount 路径 (kill_sb→net_pool_cleanup) 已做,
     * 此处兜底 rmmod 直接触发 module_exit 的场景 (尤其 conn_pool_init
     * 半途失败: 线程/delayed_work 已创建但 sbi->pool_initialized=false 致
     * kill_sb 跳过清理). conn_pool_exit 幂等 (count=0/schedulers=NULL/wq=NULL
     * 时均 no-op), 与 kill_sb 路径重复调用安全. */
    powerfs_conn_pool_exit();

    mutex_destroy(&g_pool.pool_lock);
    g_pool_initialized = false;

    pr_debug("powerfs: connection pool exited\n");
}

/**
 * powerfs_net_set_stopping - 设置 stopping 标志, 让所有等待的 send_request
 * 立即返回 -ENOTCONN. 在 kill_sb 中网络清零前调用, 防止 reconnect_work
 * 访问已清零的 g_pool.
 */
void powerfs_net_set_stopping(void)
{
    /* 新架构: g_pool.stopping 让 powerfs_request_submit / per-conn
     * reconnect_work 立即停止.
     * kill_sb 在 sync_filesystem 之后调用本函数, 必须让新架构立即停止
     * 接收请求与重连, 否则在 conn_pool_exit 真正设置 g_pool.stopping
     * 之前的窗口内, reconnect_work 仍会尝试连接即将被清理的 filer. */
    atomic_set(&g_pool.stopping, 1);
}

bool powerfs_net_is_stopping(void)
{
    return atomic_read(&g_pool.stopping) != 0;
}

/**
 * powerfs_net_pool_cleanup - 清理连接池上所有资源
 *
 * 关闭所有活动连接，清理 delta 状态，重置服务器列表
 * 用于文件系统卸载时清理
 */
void powerfs_net_pool_cleanup(void)
{
    int i;

    if (!g_pool_initialized)
        return;

    /* 安全措施: 确保 stopping 已设置 (防止 reconnect_work 在清零后访问 g_pool) */
    atomic_set(&g_pool.stopping, 1);

    /* 清理新连接池 (per-filer 连接, 待处理请求) */
    powerfs_conn_pool_exit();

    mutex_lock(&g_pool.pool_lock);

    /* 重置所有服务器条目 */
    for (i = 0; i < POWERFS_NET_MAX_SERVERS; i++) {
        memset(&g_pool.servers[i], 0, sizeof(g_pool.servers[i]));
    }

    /* 重置计数 */
    g_pool.server_count = 0;
    g_pool.filer_count = 0;
    g_pool.master_count = 0;
    g_pool.volume_count = 0;

    atomic_set(&g_pool.active_filer_idx, 0);
    atomic_set(&g_pool.active_master_idx, 0);
    atomic_set(&g_pool.active_volume_idx, 0);
    atomic_set(&g_pool.leader_idx, -1);
    atomic_set(&g_pool.leader_known, 0);

    mutex_unlock(&g_pool.pool_lock);

    pr_debug("powerfs: connection pool cleaned up\n");
}

/**
 * powerfs_net_add_server - 添加服务器到池
 */
int powerfs_net_add_server(const char *addr, __u16 port,
                           enum powerfs_net_server_type type)
{
    int idx;

    if (!addr)
        return -EINVAL;

    mutex_lock(&g_pool.pool_lock);

    if (g_pool.server_count >= POWERFS_NET_MAX_SERVERS) {
        mutex_unlock(&g_pool.pool_lock);
        pr_err("powerfs: server pool full\n");
        return -ENOSPC;
    }

    idx = g_pool.server_count;
    strncpy(g_pool.servers[idx].addr, addr, sizeof(g_pool.servers[idx].addr) - 1);
    g_pool.servers[idx].port = port;
    g_pool.servers[idx].type = type;
    g_pool.servers[idx].is_leader = false;
    g_pool.servers[idx].last_check_time = jiffies;
    g_pool.server_count++;

    /* 更新类型计数 */
    switch (type) {
    case POWERFS_NET_SERVER_FILER:
        g_pool.filer_count++;
        break;
    case POWERFS_NET_SERVER_MASTER:
        g_pool.master_count++;
        break;
    case POWERFS_NET_SERVER_VOLUME:
    case POWERFS_NET_SERVER_VOLUME_META:
        g_pool.volume_count++;
        break;
    }

    mutex_unlock(&g_pool.pool_lock);

    pr_debug("powerfs: added server %s:%u (type=%d)\n", addr, port, type);
    return idx;
}

/**
 * powerfs_net_remove_server - 从池移除服务器
 */
int powerfs_net_remove_server(const char *addr, __u16 port)
{
    int i;
    int removed = -1;

    mutex_lock(&g_pool.pool_lock);

    for (i = 0; i < g_pool.server_count; i++) {
        if (strcmp(g_pool.servers[i].addr, addr) == 0 &&
            g_pool.servers[i].port == port) {
            /* 更新类型计数 */
            switch (g_pool.servers[i].type) {
            case POWERFS_NET_SERVER_FILER:
                g_pool.filer_count--;
                break;
            case POWERFS_NET_SERVER_MASTER:
                g_pool.master_count--;
                break;
            case POWERFS_NET_SERVER_VOLUME:
            case POWERFS_NET_SERVER_VOLUME_META:
                g_pool.volume_count--;
                break;
            }

            /* 移除服务器 */
            if (i < g_pool.server_count - 1) {
                memmove(&g_pool.servers[i], &g_pool.servers[i + 1],
                        (g_pool.server_count - i - 1) * sizeof(struct powerfs_net_server_entry));
            }
            g_pool.server_count--;
            removed = i;
            break;
        }
    }

    mutex_unlock(&g_pool.pool_lock);

    if (removed >= 0) {
        pr_debug("powerfs: removed server %s:%u\n", addr, port);
        return 0;
    }
    return -ENOENT;
}

/**
 * powerfs_net_set_primary - 设置主 Filer 地址 (兼容旧接口)
 */
int powerfs_net_set_primary(const char *addr, __u16 port)
{
    int ret;

    /* 如果已有服务器，移除第一个 Filer */
    mutex_lock(&g_pool.pool_lock);
    {
        int i;
        for (i = 0; i < g_pool.server_count; i++) {
            if (g_pool.servers[i].type == POWERFS_NET_SERVER_FILER) {
                /* 找到第一个 Filer，标记为 leader */
                g_pool.servers[i].is_leader = true;
                atomic_set(&g_pool.leader_idx, i);
                atomic_set(&g_pool.leader_known, 1);
                mutex_unlock(&g_pool.pool_lock);
                return 0;
            }
        }
    }
    mutex_unlock(&g_pool.pool_lock);

    /* 添加新 Filer */
    ret = powerfs_net_add_server(addr, port, POWERFS_NET_SERVER_FILER);
    if (ret >= 0) {
        /* 标记为 leader */
        mutex_lock(&g_pool.pool_lock);
        g_pool.servers[ret].is_leader = true;
        atomic_set(&g_pool.leader_idx, ret);
        atomic_set(&g_pool.leader_known, 1);
        mutex_unlock(&g_pool.pool_lock);
    }

    return ret >= 0 ? 0 : ret;
}

/**
 * powerfs_net_set_filers - 设置多个 Filer 地址
 */
int powerfs_net_set_filers(const char *addrs, const char *ports)
{
    char addr_buf[1024];
    char port_buf[512];
    char *addr_token, *port_token;
    int ret;
    int count = 0;

    if (!addrs || !ports)
        return -EINVAL;

    strncpy(addr_buf, addrs, sizeof(addr_buf) - 1);
    strncpy(port_buf, ports, sizeof(port_buf) - 1);
    addr_buf[sizeof(addr_buf) - 1] = '\0';
    port_buf[sizeof(port_buf) - 1] = '\0';

    {
        char *addr_save = addr_buf;
        char *port_save = port_buf;

        addr_token = strsep(&addr_save, ",");
        port_token = strsep(&port_save, ",");

        while (addr_token && port_token) {
            __u16 port = simple_strtoul(port_token, NULL, 10);

            /* 去除可能的空格 */
            while (*addr_token == ' ') addr_token++;
            while (*port_token == ' ') port_token++;

            if (*addr_token == '\0' || *port_token == '\0')
                break;

            ret = powerfs_net_add_server(addr_token, port, POWERFS_NET_SERVER_FILER);
            if (ret >= 0) {
                if (count == 0) {
                    /* 第一个 Filer 标记为 leader */
                    mutex_lock(&g_pool.pool_lock);
                    g_pool.servers[ret].is_leader = true;
                    atomic_set(&g_pool.leader_idx, ret);
                    atomic_set(&g_pool.leader_known, 1);
                    mutex_unlock(&g_pool.pool_lock);
                }
                count++;
            }

            addr_token = strsep(&addr_save, ",");
            port_token = strsep(&port_save, ",");
        }
    }

    pr_debug("powerfs: set %d filer addresses\n", count);
    return count > 0 ? 0 : -ENOENT;
}

/**
 * powerfs_net_set_master - 设置 Master 地址
 */
int powerfs_net_set_master(const char *addr, __u16 port)
{
    return powerfs_net_add_server(addr, port, POWERFS_NET_SERVER_MASTER);
}
