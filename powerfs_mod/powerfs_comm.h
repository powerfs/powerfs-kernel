/*
 * PowerFS 通信协议定义
 *
 * 内核文件系统与用户态代理之间的通信接口
 *
 * 架构:
 *   内核文件系统 -> /dev/powerfs_comm -> 用户态代理 -> gRPC -> 服务端
 *
 * 通信方式:
 *   - ioctl: 同步 RPC 调用 (lookup, getattr, mkdir 等)
 *   - mmap:  共享内存，批量消息传递
 *   - poll:  异步通知 (用户态代理等待新请求)
 */

#ifndef _POWERFS_COMM_H
#define _POWERFS_COMM_H

#include <linux/types.h>

/* ========== Ioctl 命令定义 ========== */

#define POWERFS_IOCTL_MAGIC    'p'

/* 测试命令 */
#define POWERFS_IOCTL_PING     _IO(POWERFS_IOCTL_MAGIC, 1)

/* 获取协议版本 */
#define POWERFS_IOCTL_VERSION  _IOR(POWERFS_IOCTL_MAGIC, 2, __u32)

/* 同步请求 (用户态代理调用) */
#define POWERFS_IOCTL_REQUEST  _IOWR(POWERFS_IOCTL_MAGIC, 10, struct powerfs_ioctl_req)

/* 用户态代理提交响应 (写入 CQ) */
#define POWERFS_IOCTL_SUBMIT_RESP _IOW(POWERFS_IOCTL_MAGIC, 11, struct powerfs_ioctl_req)

/* 用户态代理获取请求 (从 SQ 读取) */
#define POWERFS_IOCTL_GET_REQ   _IOWR(POWERFS_IOCTL_MAGIC, 12, struct powerfs_ioctl_req)

/* ========== Ioctl 命令定义 (扩展) ========== */

/* 主动失效通知 (用户态→内核) */
#define POWERFS_IOCTL_INVALIDATE \
    _IOW(POWERFS_IOCTL_MAGIC, 20, struct powerfs_invalidate_req)

/* ========== 消息类型定义 ========== */

enum powerfs_msg_type {
    POWERFS_MSG_NONE = 0,
    POWERFS_MSG_PING = 1,       /* 测试 ping */

    /* 元数据请求 (MVP) */
    POWERFS_MSG_LOOKUP = 10,    /* 查找文件 */
    POWERFS_MSG_GETATTR = 11,   /* 获取属性 */
    POWERFS_MSG_SETATTR = 12,   /* 设置属性 */
    POWERFS_MSG_MKDIR = 13,     /* 创建目录 */
    POWERFS_MSG_CREATE = 14,    /* 创建文件 */
    POWERFS_MSG_UNLINK = 15,    /* 删除文件 */
    POWERFS_MSG_RMDIR = 16,     /* 删除目录 */
    POWERFS_MSG_RENAME = 17,    /* 重命名 */
    POWERFS_MSG_READDIR = 18,   /* 读目录 */
    POWERFS_MSG_SYMLINK = 19,   /* 创建符号链接 */
    POWERFS_MSG_READLINK = 20,  /* 读取符号链接目标 */
    POWERFS_MSG_LINK = 21,      /* 创建硬链接 */
    POWERFS_MSG_MKNOD = 22,     /* 创建设备节点 */

    /* 数据路径 */
    POWERFS_MSG_READ = 30,      /* 读数据 */
    POWERFS_MSG_WRITE = 31,     /* 写数据 */
    POWERFS_MSG_FSYNC = 32,     /* 数据同步 */
    POWERFS_MSG_TRUNCATE = 33,  /* 文件截断 */

    /* 文件系统统计 */
    POWERFS_MSG_STATFS = 40,    /* 获取文件系统统计 */

    /* 主动失效通知 (用户态→内核) */
    POWERFS_MSG_INVALIDATE_NOTIFY = 50,  /* dentry/inode 失效 */
};

/* 失效类型标志 */
#define POWERFS_INVALIDATE_DENTRY  (1 << 0)  /* dentry 失效 */
#define POWERFS_INVALIDATE_INODE   (1 << 1)  /* inode 失效 */
#define POWERFS_INVALIDATE_ALL     (1 << 2)  /* 全部失效 */
#define POWERFS_INVALIDATE_DIR     (1 << 3)  /* 目录内容失效 (dir_complete = false) */

/* ========== 请求/响应头部 ========== */

struct powerfs_msg_header {
    __u32 type;       /* 消息类型 powerfs_msg_type */
    __u32 seq;        /* 序列号 */
    __u32 status;     /* 状态码 (0 表示成功，负值表示错误) */
    __u32 data_len;   /* 数据长度 */
    __u64 ino;        /* inode 号 */
};

/* ========== Ioctl 请求结构 ========== */

struct powerfs_ioctl_req {
    struct powerfs_msg_header hdr;
    void __user *data;      /* 请求/响应数据指针 */
    __u32 data_size;        /* 数据缓冲区大小 */
};

/* ========== 共享内存布局 ========== */

#define POWERFS_COMM_MAGIC     0x50575243  /* "PWRC" */
#define POWERFS_COMM_VERSION   1

#define POWERFS_MAX_REQUESTS   64    /* 最大请求数 */
#define POWERFS_MAX_DATA_SIZE  4096  /* 单条消息最大数据 */

/*
 * 共享内存头部
 * 位于 mmap 区域的起始位置
 */
struct powerfs_shm_header {
    __u32 magic;                /* 魔数 POWERFS_COMM_MAGIC */
    __u32 version;              /* 协议版本 */
    __u32 sq_head;              /* 请求队列头 (内核写) */
    __u32 sq_tail;              /* 请求队列尾 (用户态读) */
    __u32 cq_head;              /* 响应队列头 (用户态写) */
    __u32 cq_tail;              /* 响应队列尾 (内核读) */
    __u32 max_requests;         /* 最大请求数 */
    __u32 max_data_size;        /* 单条消息最大数据 */
    __u64 data_offset;          /* 数据区偏移 */
};

/*
 * 队列条目
 */
struct powerfs_shm_entry {
    struct powerfs_msg_header hdr;
    __u32 data_offset;          /* 数据在数据区中的偏移 */
    __u32 data_len;             /* 数据长度 */
};

/* ========== 计算共享内存大小 ========== */

#define POWERFS_SHM_SIZE(max_req, max_data) \
    (sizeof(struct powerfs_shm_header) + \
     sizeof(struct powerfs_shm_entry) * (max_req) + \
     (max_data) * (max_req))

/* 页对齐 (mmap 需要页对齐的大小) */
#define POWERFS_SHM_SIZE_ALIGNED(max_req, max_data) \
    ((POWERFS_SHM_SIZE(max_req, max_data) + 4095) & ~4095ULL)

/* ========== 请求/响应数据结构 ========== */

/* LOOKUP 请求数据 */
struct powerfs_lookup_req {
    __u64 dir_ino;
    char name[POWERFS_MAX_DATA_SIZE - sizeof(__u64)];
};

/* LOOKUP 响应数据 */
struct powerfs_lookup_resp {
    __u64 ino;
    __u32 mode;
    __u32 uid;
    __u32 gid;
    __u32 nlink;
    __u64 size;
    __u64 mtime_sec;
    __u32 mtime_nsec;
    __u32 padding;
};

/* GETATTR 请求数据 */
struct powerfs_getattr_req {
    __u64 ino;
};

/* GETATTR 响应数据 */
struct powerfs_getattr_resp {
    __u32 mode;
    __u32 nlink;
    __u32 uid;
    __u32 gid;
    __u64 size;
    __u64 atime_sec;
    __u32 atime_nsec;
    __u32 padding1;
    __u64 mtime_sec;
    __u32 mtime_nsec;
    __u32 padding2;
    __u64 ctime_sec;
    __u32 ctime_nsec;
    __u32 padding3;
    __u64 blocks;
    __u32 blksize;
    __u32 padding4;
};

/* MKDIR/CREATE 请求数据 */
struct powerfs_create_req {
    __u64 dir_ino;
    __u64 new_ino;    /* 内核分配的 inode 号，通知代理使用 */
    __u32 mode;
    __u32 uid;
    __u32 gid;
    __u32 padding;
    char name[POWERFS_MAX_DATA_SIZE - 2 * sizeof(__u64) - 4 * sizeof(__u32)];
};

/* MKDIR/CREATE 响应数据 */
struct powerfs_create_resp {
    __u64 ino;
    __u32 mode;
    __u32 uid;
    __u32 gid;
    __u32 nlink;
    __u64 size;
    __u64 mtime_sec;
    __u32 mtime_nsec;
    __u32 padding;
};

/* UNLINK/RMDIR 请求数据 */
struct powerfs_remove_req {
    __u64 dir_ino;
    char name[POWERFS_MAX_DATA_SIZE - sizeof(__u64)];
};

/* READDIR 请求数据 */
struct powerfs_readdir_req {
    __u64 dir_ino;
    __u64 offset;
    __u32 max_entries;
    __u32 padding;
};

/* READDIR 响应数据 (单个目录项) */
struct powerfs_dirent {
    __u64 ino;
    __u32 type;
    __u32 name_len;
    char name[256];
};

/* READ 请求数据 */
struct powerfs_read_req {
    __u64 ino;
    __u64 offset;
    __u32 length;
    __u32 padding;
};

/* READ 响应数据 (数据直接放在响应中，最大 4KB) */
struct powerfs_read_resp {
    __u32 length;
    __u32 padding;
    __u8 data[POWERFS_MAX_DATA_SIZE - sizeof(__u32) - sizeof(__u32)];
};

/* WRITE 请求数据 (数据紧跟在头部后面) */
struct powerfs_write_req {
    __u64 ino;
    __u64 offset;
    __u32 length;
    __u32 padding;
    /* data follows */
};

/* WRITE 响应数据 */
struct powerfs_write_resp {
    __u32 written;
    __u32 padding;
};

/* SETATTR 请求数据 */
struct powerfs_setattr_req {
    __u64 ino;
    __u32 ia_valid;     /* 有效的属性掩码 */
    __u32 mode;         /* ATTR_MODE */
    __u32 uid;          /* ATTR_UID */
    __u32 gid;          /* ATTR_GID */
    __u64 size;         /* ATTR_SIZE */
    __u64 atime_sec;    /* ATTR_ATIME */
    __u32 atime_nsec;
    __u32 padding1;
    __u64 mtime_sec;    /* ATTR_MTIME */
    __u32 mtime_nsec;
    __u32 padding2;
    __u64 ctime_sec;    /* ATTR_CTIME */
    __u32 ctime_nsec;
    __u32 padding3;
};

/* SETATTR 响应数据 */
struct powerfs_setattr_resp {
    __u32 mode;
    __u32 nlink;
    __u32 uid;
    __u32 gid;
    __u64 size;
    __u64 atime_sec;
    __u32 atime_nsec;
    __u32 padding1;
    __u64 mtime_sec;
    __u32 mtime_nsec;
    __u32 padding2;
    __u64 ctime_sec;
    __u32 ctime_nsec;
    __u32 padding3;
    __u64 blocks;
    __u32 blksize;
    __u32 padding4;
};

/* RENAME 请求数据 */
struct powerfs_rename_req {
    __u64 old_dir_ino;
    __u64 new_dir_ino;
    __u32 old_name_len;
    __u32 new_name_len;
    __u32 flags;
    __u32 padding;
    char old_name[256];
    char new_name[256];
};

/* RENAME 响应数据 */
struct powerfs_rename_resp {
    __u32 padding;
};

/* ========== SYMLINK 请求/响应 ========== */

/* SYMLINK 请求数据 */
struct powerfs_symlink_req {
    __u64 dir_ino;
    __u32 name_len;
    __u32 symname_len;
    char name[256];
    char symname[POWERFS_MAX_DATA_SIZE - 2 * sizeof(__u64) - 2 * sizeof(__u32) - 256];
};

/* SYMLINK 响应数据 */
struct powerfs_symlink_resp {
    __u64 ino;
    __u32 mode;
    __u32 padding;
};

/* ========== READLINK 请求/响应 ========== */

/* READLINK 请求数据 */
struct powerfs_readlink_req {
    __u64 ino;
};

/* READLINK 响应数据 (符号链接目标路径) */
struct powerfs_readlink_resp {
    __u32 len;
    __u32 padding;
    char target[POWERFS_MAX_DATA_SIZE - 2 * sizeof(__u32)];
};

/* ========== LINK (硬链接) 请求/响应 ========== */

/* LINK 请求数据 */
struct powerfs_link_req {
    __u64 ino;          /* 目标文件 ino */
    __u64 dir_ino;      /* 新父目录 ino */
    char name[256];
};

/* LINK 响应数据 */
struct powerfs_link_resp {
    __u32 padding;
};

/* ========== MKNOD 请求/响应 ========== */

/* MKNOD 请求数据 */
struct powerfs_mknod_req {
    __u64 dir_ino;
    __u32 mode;
    __u32 uid;
    __u32 gid;
    __u32 padding;
    __u64 dev;          /* 设备号 */
    char name[256];
};

/* MKNOD 响应数据 */
struct powerfs_mknod_resp {
    __u64 ino;
    __u32 mode;
    __u32 padding;
};

/* ========== FSYNC 请求/响应 ========== */

/* FSYNC 请求数据 */
struct powerfs_fsync_req {
    __u64 ino;
    __u32 datasync;     /* 1=只同步数据, 0=同步数据+元数据 */
    __u32 padding;
};

/* FSYNC 响应数据 */
struct powerfs_fsync_resp {
    __u32 padding;
};

/* ========== TRUNCATE 请求/响应 ========== */

/* TRUNCATE 请求数据 */
struct powerfs_truncate_req {
    __u64 ino;
    __u64 size;
};

/* TRUNCATE 响应数据 */
struct powerfs_truncate_resp {
    __u64 size;
    __u32 padding;
};

/* ========== STATFS 请求/响应 ========== */

/* STATFS 请求数据 (空) */
struct powerfs_statfs_req {
    __u32 padding;
};

/* STATFS 响应数据 */
struct powerfs_statfs_resp {
    __u64 blocks;       /* 总块数 */
    __u64 bfree;        /* 空闲块数 */
    __u64 bavail;       /* 可用块数 */
    __u64 files;        /* 总文件数 */
    __u64 ffree;        /* 空闲文件数 */
    __u32 bsize;        /* 块大小 */
    __u32 namelen;      /* 文件名最大长度 */
    __u32 frsize;       /* 碎片大小 */
    __u32 padding;
};

/* ========== INVALIDATE_NOTIFY 请求 ========== */

/*
 * 主动失效通知请求结构
 * 用户态代理通过 ioctl 发送此结构到内核
 * 内核根据 flags 失效对应的 dentry/inode 缓存
 */
struct powerfs_invalidate_req {
    __u32 flags;        /* 失效类型 (POWERFS_INVALIDATE_*) */
    __u32 count;        /* 失效条目数量 */
    __u64 ino[];        /* 要失效的 inode 号数组 (柔性数组) */
};

/* SETATTR 掩码 (对应 Linux ATTR_* 标志) */
#define POWERFS_ATTR_MODE     (1 << 0)
#define POWERFS_ATTR_UID      (1 << 1)
#define POWERFS_ATTR_GID      (1 << 2)
#define POWERFS_ATTR_SIZE     (1 << 3)
#define POWERFS_ATTR_ATIME    (1 << 4)
#define POWERFS_ATTR_MTIME    (1 << 5)
#define POWERFS_ATTR_CTIME    (1 << 6)

#endif /* _POWERFS_COMM_H */
