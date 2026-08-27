/*
 * PowerFS 内核态 powerfs-net 协议实现 (模块入口)
 *
 * 直接在内核中实现 powerfs-net 二进制协议，通过 TCP 连接与 Filer 通信。
 * 本文件拆分后仅保留模块入口、全局变量定义和符号导出。实现逻辑分布在
 * powerfs_net_*.c 中，跨文件内部符号声明见 powerfs_net_internal.h。
 */

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


/* ========== 全局连接上下文 ========== */

bool g_initialized = false;

/* 前向声明: g_pool 定义在后面 (多连接池实现段) */
struct powerfs_net_pool g_pool;

/* Global ShardMap for range-based inode → shard_id routing.
 * Equivalent to powerfs-allocator::ShardMap in the FUSE client.
 * Initialized in discover_filers() from Master topology (0xBD entries
 * or fallback to from_shard_count). */
struct shard_map g_shard_map = {
    .entry_count = 0,
    .lock = __SPIN_LOCK_UNLOCKED(g_shard_map.lock),
};


/* ========== 导出符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_net_is_connected);
EXPORT_SYMBOL_GPL(powerfs_net_recently_disconnected);
EXPORT_SYMBOL_GPL(powerfs_net_pick_timeout);
EXPORT_SYMBOL_GPL(powerfs_net_send_request);
EXPORT_SYMBOL_GPL(powerfs_net_lookup);
EXPORT_SYMBOL_GPL(powerfs_net_lookup_timeout);
EXPORT_SYMBOL_GPL(powerfs_net_getattr);
EXPORT_SYMBOL_GPL(powerfs_net_setattr);
EXPORT_SYMBOL_GPL(powerfs_net_create);
EXPORT_SYMBOL_GPL(powerfs_net_unlink);
EXPORT_SYMBOL_GPL(powerfs_net_rename);
EXPORT_SYMBOL_GPL(powerfs_net_readdir);
EXPORT_SYMBOL_GPL(powerfs_net_readdir_timeout);
EXPORT_SYMBOL_GPL(powerfs_net_read);
EXPORT_SYMBOL_GPL(powerfs_net_write);
EXPORT_SYMBOL_GPL(powerfs_net_statfs);
EXPORT_SYMBOL_GPL(powerfs_net_symlink);
EXPORT_SYMBOL_GPL(powerfs_net_readlink);
EXPORT_SYMBOL_GPL(powerfs_net_link);
EXPORT_SYMBOL_GPL(powerfs_net_ping);
EXPORT_SYMBOL_GPL(powerfs_net_init);
EXPORT_SYMBOL_GPL(powerfs_net_exit);


/* ========== 导出新符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_net_pool_init);
EXPORT_SYMBOL_GPL(powerfs_net_pool_exit);
EXPORT_SYMBOL_GPL(powerfs_net_pool_cleanup);
EXPORT_SYMBOL_GPL(powerfs_net_add_server);
EXPORT_SYMBOL_GPL(powerfs_net_remove_server);
EXPORT_SYMBOL_GPL(powerfs_net_set_primary);
EXPORT_SYMBOL_GPL(powerfs_net_set_filers);
EXPORT_SYMBOL_GPL(powerfs_net_discover_filers);
EXPORT_SYMBOL_GPL(powerfs_net_set_master);
EXPORT_SYMBOL_GPL(powerfs_net_set_volume);
EXPORT_SYMBOL_GPL(powerfs_net_get_volume_count);
EXPORT_SYMBOL_GPL(powerfs_net_get_volume_conn);
EXPORT_SYMBOL_GPL(powerfs_net_find_volume_conn);
EXPORT_SYMBOL_GPL(powerfs_net_send_to_volume);
EXPORT_SYMBOL_GPL(powerfs_net_write_needle);
EXPORT_SYMBOL_GPL(powerfs_net_read_needle);
EXPORT_SYMBOL_GPL(powerfs_net_discover_volumes);
EXPORT_SYMBOL_GPL(powerfs_net_renew_lease);
EXPORT_SYMBOL_GPL(powerfs_net_register_client);
EXPORT_SYMBOL_GPL(powerfs_net_deregister_client);
