/* SPDX-License-Identifier: GPL-2.0 */
/*
 * powerfs_net_internal.h - 内部跨文件符号声明 (powerfs_net 拆分后)
 *
 * 本头文件仅供 powerfs_net_*.c 内部使用, 不对外暴露. 对外 API 仍在
 * powerfs_net.h. 原本 powerfs_net.c 10222 行拆分成 11 个 .c 文件后,
 * 原 static 函数 / static 变量需要跨文件调用, 故去 static 改放此处声明.
 *
 * 命名约定:
 *   - 函数沿用原 powerfs_/powerfs_net_/powerfs_conn_/pfs_ 前缀, 不改名
 *   - 全局变量沿用原 g_ 前缀
 *   - static inline 小工具 (pfs_route_hash) 保留在 header 里
 *
 * 拆分布局 (与 powerfs_fs.c 拆分风格对齐):
 *   powerfs_net.c            - 模块入口 + 全局变量 + init/exit + EXPORT
 *   powerfs_net_shard.c      - ShardMap + CRC32C + 帧编解码
 *   powerfs_net_sock.c       - Socket 辅助 + 帧收发 + 握手
 *   powerfs_net_conn.c       - 连接池 + sched + RX/TX + callbacks + heartbeat
 *   powerfs_net_req.c        - 请求生命周期 + req_tree + RPC 核心
 *   powerfs_net_inode.c      - inode/dir RPC (lookup/getattr/create/...)
 *   powerfs_net_data.c       - 数据 RPC (read_ec/read/write + volume + needle)
 *   powerfs_net_xattr.c      - xattr RPC
 *   powerfs_net_pool.c       - 多连接池 + set_* + statfs cache
 *   powerfs_net_discover.c   - Filer/Volume 发现 + client 注册
 *   powerfs_net_lease.c      - lease + cap RPC + cap notify dispatch
 */
#ifndef POWERFS_NET_INTERNAL_H
#define POWERFS_NET_INTERNAL_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <linux/socket.h>
#include <linux/kref.h>
#include <linux/rbtree.h>

#include "powerfs_net.h"
#include "powerfs.h"

/* ========================================================================
 * 全局变量 (原 powerfs_net.c 中的 static 变量, 改为 extern 供跨文件访问)
 * ======================================================================== */

/* 模块初始化标志 (powerfs_net.c) */
extern bool g_initialized;

/* 全局连接池 (定义在 powerfs_net.c, 各 .c 文件直接访问字段) */
extern struct powerfs_net_pool g_pool;

/* discover_filers 用的序列号计数器 (定义在 powerfs_net_shard.c) */
extern atomic_t g_discover_seq;

/* CRC32C 查表 (定义在 powerfs_net_shard.c) */
extern __u32 crc32c_table[256];
extern bool crc32c_table_init;

/* 多连接池初始化标志 (定义在 powerfs_net_pool.c) */
extern bool g_pool_initialized;

/* topology refresh work + pending bit (定义在 powerfs_net_conn.c) */
extern struct work_struct g_topology_refresh_work;
extern unsigned long g_topology_refresh_flags;
#define TOPOLOGY_REFRESH_PENDING_BIT  0UL

/* statfs cache (定义在 powerfs_net_pool.c, 匿名 struct 命名后跨文件可见) */
struct powerfs_statfs_cache {
    __u64 total_size;
    __u64 free_size;
    __u64 total_files;
    __u64 free_inodes;
    __u32 block_size;
    unsigned long cached_jiffies;
    bool valid;
};
extern struct powerfs_statfs_cache g_statfs_cache;
extern spinlock_t g_statfs_cache_lock;
#define POWERFS_STATFS_CACHE_TTL    (30 * HZ)  /* 30 seconds */

/* ========================================================================
 * ShardMap + CRC32C + 帧编解码 (powerfs_net_shard.c)
 * ======================================================================== */
void powerfs_crc32c_init_table(void);

/* 计算 route_hash: 高7位=client_id hash, 低1位=channel (防错乱校验).
 * 保留 inline 放 header 里 (与原 powerfs_net.c 一致). */
static inline __u8 pfs_route_hash(__u64 client_id, __u8 channel)
{
    __u64 h = client_id;
    h ^= h >> 32;
    h *= 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    return ((__u8)(h >> 25) << 1) | (channel & 0x01);
}

/* ========================================================================
 * Socket 辅助 + 帧收发 + 握手 (powerfs_net_sock.c)
 * ======================================================================== */
struct socket *powerfs_net_create_tcp_socket(void);
int powerfs_net_tcp_connect(struct socket *sock, const char *addr, __u16 port);
void powerfs_net_close_socket(struct socket *sock);

int pfs_frame_send_nonblock(struct socket *sock,
                            struct powerfs_net_frame_hdr *hdr,
                            const __u8 *body, size_t body_len,
                            const __u8 *data, size_t data_len,
                            struct powerfs_request *req);

/* 裸 socket 握手 (master 发现阶段, 无连接池上下文) */
int powerfs_net_do_handshake(struct socket *sock);

/* 连接池内握手 (填充 conn->client_id 等) */
int powerfs_conn_do_handshake(struct socket *sock,
                              struct powerfs_net_server_conn *conn);

/* ========================================================================
 * 连接池 + sched + RX/TX + callbacks + heartbeat (powerfs_net_conn.c)
 * ======================================================================== */

/* 连接对象生命周期 */
int powerfs_conn_get_filer_idx(struct powerfs_net_server_conn *conn);
void powerfs_conn_release(struct kref *kref);
/* powerfs_conn_get/put 保留 static inline 定义在 powerfs_net_conn.c (仅本文件使用) */
int pfs_conn_flow_idx(struct powerfs_net_server_conn *conn);

/* 调度器选择 */
struct powerfs_net_sched *pfs_pick_sched(const char *addr);
struct powerfs_net_sched *pfs_pick_vol_sched(const char *addr,
                                             enum powerfs_net_server_type type);
int  powerfs_sched_init(void);
void powerfs_sched_exit(void);

/* sk 回调 (data_ready/write_space/state_change/error_report 由 TCP stack 调用) */
void pfs_data_ready(struct sock *sk);
void pfs_write_space(struct sock *sk);
void pfs_state_change(struct sock *sk);
void pfs_error_report(struct sock *sk);

/* RX/TX 入队回调 (sk 回调 → sched 队列) */
void pfs_rx_callback(struct powerfs_net_server_conn *conn);
void pfs_tx_callback(struct powerfs_net_server_conn *conn);
void pfs_conn_set_callbacks(struct powerfs_net_server_conn *conn);
void pfs_conn_reset_callbacks(struct powerfs_net_server_conn *conn);

/* RX/TX kthread */
int pfs_rx_thread_fn(void *arg);
int pfs_tx_thread_fn(void *arg);

/* RX 非阻塞状态机 helpers */
void pfs_rx_reset_partial(struct powerfs_net_server_conn *conn);
int  pfs_conn_alloc_rxbuffers(struct powerfs_net_server_conn *conn);
void pfs_conn_free_rxbuffers(struct powerfs_net_server_conn *conn);
int  pfs_rx_step(struct powerfs_net_server_conn *conn);
void pfs_rx_dispatch(struct powerfs_net_server_conn *conn);
void pfs_process_receive(struct powerfs_net_server_conn *conn);
void pfs_process_transmit(struct powerfs_net_server_conn *conn);
void pfs_tx_schedule(struct powerfs_net_server_conn *conn);
void pfs_conn_remove_from_sched(struct powerfs_net_server_conn *conn);

/* disconnect / reconnect / topology / heartbeat 工作 */
void powerfs_conn_disconnect_work_fn(struct work_struct *work);
void powerfs_conn_reconnect_work_fn(struct work_struct *work);
void topology_refresh_worker(struct work_struct *work);
int  powerfs_heartbeat_send_one(const char *master_addr, __u16 master_port,
                                 const __u8 *req_body, size_t req_body_len);
void powerfs_heartbeat_work_fn(struct work_struct *work);

/* cap NOTIFY 回调函数指针 (定义在 powerfs_net_conn.c, lease.c 注册/设置) */
extern powerfs_cap_recall_notify_fn g_cap_recall_notify_fn;
extern powerfs_cap_upgrade_notify_fn g_cap_upgrade_notify_fn;

/* ========================================================================
 * 请求对象 + req_tree + RPC 核心 (powerfs_net_req.c)
 * ======================================================================== */
void powerfs_req_timeout_fn(struct work_struct *work);
void powerfs_req_tree_insert(struct powerfs_net_server_conn *conn,
                             struct powerfs_request *req);
struct powerfs_request *
powerfs_req_tree_lookup(struct powerfs_net_server_conn *conn, __u32 seq);
void powerfs_req_tree_remove(struct powerfs_net_server_conn *conn,
                             struct powerfs_request *req);
int powerfs_request_do_send(struct powerfs_request *req,
                             struct powerfs_net_server_conn *conn);

int powerfs_net_parse_redirect(const __u8 *body, size_t body_len,
                               char *addr, size_t addr_cap,
                               __u16 *port);

/* ========================================================================
 * inode / dir 解析辅助 (powerfs_net_inode.c)
 * ======================================================================== */
void parse_file_layout(struct powerfs_tlv_dec *dec,
                        struct powerfs_file_layout *layout);
int parse_placement_field(const __u8 *val, size_t len,
                           struct powerfs_file_layout *layout);
int parse_reliability_field(const __u8 *val, size_t len,
                             struct powerfs_file_layout *layout);
int parse_replica_chunks_field(const __u8 *val, size_t len,
                                struct powerfs_file_layout *layout);

/* ========================================================================
 * 数据 RPC (powerfs_net_data.c)
 * ======================================================================== */
int powerfs_net_read_ec(struct powerfs_inode_info *pi, __u64 ino,
                         __u64 offset, __u32 length,
                         __u8 *buf, size_t buf_cap, __u32 *read_len);

/* ========================================================================
 * Volume 直连辅助 (powerfs_net_data.c)
 * 返回 conn_idx (>=0) 或负错误码, 不返回 conn 指针
 * ======================================================================== */
bool pfs_is_meta_msg(__u16 msg_type);
int  pfs_find_vol_conn_by_addr(const char *ip, __u16 port,
                                enum powerfs_net_server_type type);
int  pfs_ensure_volume_conn(const char *ip, __u16 port,
                             enum powerfs_net_server_type type);

/* ========================================================================
 * Cap / Lease 解码辅助 (powerfs_net_lease.c)
 * ======================================================================== */
int decode_cap_grant_fields(struct powerfs_tlv_dec *dec,
                             char *token_out, size_t *token_len_out,
                             __u8 *cap_set_out, __u64 *epoch_out,
                             __u64 *sn_out, __u64 *duration_ms_out);
int decode_cap_recall_body(const __u8 *body, size_t body_len,
                            u64 *ino_out,
                            char *token_out, size_t token_cap, size_t *token_len_out,
                            __u8 *recall_mask_out, __u8 *retain_mask_out,
                            __u64 *epoch_out);
int decode_cap_upgrade_body(const __u8 *body, size_t body_len,
                             u64 *ino_out,
                             char *token_out, size_t token_cap, size_t *token_len_out,
                             __u8 *new_granted_out,
                             __u64 *epoch_out, __u64 *sn_out);

#endif /* POWERFS_NET_INTERNAL_H */
