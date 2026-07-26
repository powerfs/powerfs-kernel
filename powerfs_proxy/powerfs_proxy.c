/*
 * PowerFS 用户态代理程序
 * 负责与内核模块通信，并转发请求到 PowerFS 集群服务
 *
 * 通信方式：字符设备 /dev/powerfs_comm + ioctl + mmap 共享内存
 * 架构：
 *   内核文件系统 -> SQ (共享内存) -> 代理 -> 处理 -> CQ (共享内存) -> 内核文件系统
 *
 * 注意：此文件的结构体定义必须与内核 powerfs_comm.h 完全一致！
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ========== 与内核 powerfs_comm.h 完全一致的定义 ========== */

#define POWERFS_IOCTL_MAGIC    'p'
#define POWERFS_IOCTL_PING     _IO(POWERFS_IOCTL_MAGIC, 1)
#define POWERFS_IOCTL_VERSION  _IOR(POWERFS_IOCTL_MAGIC, 2, uint32_t)
#define POWERFS_IOCTL_REQUEST  _IOWR(POWERFS_IOCTL_MAGIC, 10, struct powerfs_ioctl_req)
#define POWERFS_IOCTL_SUBMIT_RESP _IOW(POWERFS_IOCTL_MAGIC, 11, struct powerfs_ioctl_req)
#define POWERFS_IOCTL_GET_REQ   _IOWR(POWERFS_IOCTL_MAGIC, 12, struct powerfs_ioctl_req)
#define POWERFS_IOCTL_INVALIDATE \
    _IOW(POWERFS_IOCTL_MAGIC, 20, struct powerfs_invalidate_req)

/* 消息类型 - 与内核完全一致 */
enum powerfs_msg_type {
    POWERFS_MSG_NONE = 0,
    POWERFS_MSG_PING = 1,
    POWERFS_MSG_LOOKUP = 10,
    POWERFS_MSG_GETATTR = 11,
    POWERFS_MSG_SETATTR = 12,
    POWERFS_MSG_MKDIR = 13,
    POWERFS_MSG_CREATE = 14,
    POWERFS_MSG_UNLINK = 15,
    POWERFS_MSG_RMDIR = 16,
    POWERFS_MSG_RENAME = 17,
    POWERFS_MSG_READDIR = 18,
    POWERFS_MSG_SYMLINK = 19,
    POWERFS_MSG_READLINK = 20,
    POWERFS_MSG_LINK = 21,
    POWERFS_MSG_MKNOD = 22,
    POWERFS_MSG_READ = 30,
    POWERFS_MSG_WRITE = 31,
    POWERFS_MSG_FSYNC = 32,
    POWERFS_MSG_TRUNCATE = 33,
    POWERFS_MSG_STATFS = 40,
    POWERFS_MSG_INVALIDATE_NOTIFY = 50,
};

/* 失效类型标志 */
#define POWERFS_INVALIDATE_DENTRY  (1 << 0)
#define POWERFS_INVALIDATE_INODE   (1 << 1)
#define POWERFS_INVALIDATE_ALL     (1 << 2)
#define POWERFS_INVALIDATE_DIR     (1 << 3)

/* 消息头部 - 与内核完全一致 */
struct powerfs_msg_header {
    uint32_t type;
    uint32_t seq;
    uint32_t status;
    uint32_t data_len;
    uint64_t ino;
};

/* Ioctl 请求结构 */
struct powerfs_ioctl_req {
    struct powerfs_msg_header hdr;
    void *data;
    uint32_t data_size;
};

/* 共享内存常量 */
#define POWERFS_COMM_MAGIC     0x50575243
#define POWERFS_COMM_VERSION   1
#define POWERFS_MAX_REQUESTS   256
#define POWERFS_MAX_DATA_SIZE  4096

/* 共享内存头部 - 与内核完全一致 */
struct powerfs_shm_header {
    uint32_t magic;
    uint32_t version;
    uint32_t sq_head;
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t cq_tail;
    uint32_t max_requests;
    uint32_t max_data_size;
    uint64_t data_offset;
};

/* 队列条目 - 与内核完全一致 */
struct powerfs_shm_entry {
    struct powerfs_msg_header hdr;
    uint32_t data_offset;
    uint32_t data_len;
};

/* 计算共享内存大小 */
#define POWERFS_SHM_SIZE(max_req, max_data) \
    (sizeof(struct powerfs_shm_header) + \
     sizeof(struct powerfs_shm_entry) * (max_req) + \
     (max_data) * (max_req))

#define POWERFS_SHM_SIZE_ALIGNED(max_req, max_data) \
    ((POWERFS_SHM_SIZE(max_req, max_data) + 4095) & ~4095ULL)

/* ========== 请求/响应数据结构 - 必须与内核完全一致 ========== */

struct powerfs_lookup_req {
    uint64_t dir_ino;
    char name[POWERFS_MAX_DATA_SIZE - sizeof(uint64_t)];
};

struct powerfs_lookup_resp {
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint32_t padding;
};

struct powerfs_getattr_req {
    uint64_t ino;
};

struct powerfs_getattr_resp {
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime_sec;
    uint32_t atime_nsec;
    uint32_t padding1;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint32_t padding2;
    uint64_t ctime_sec;
    uint32_t ctime_nsec;
    uint32_t padding3;
    uint64_t blocks;
    uint32_t blksize;
    uint32_t padding4;
};

struct powerfs_create_req {
    uint64_t dir_ino;
    uint64_t new_ino;    /* 内核分配的 inode 号 */
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t padding;
    char name[POWERFS_MAX_DATA_SIZE - 2 * sizeof(uint64_t) - 4 * sizeof(uint32_t)];
};

struct powerfs_create_resp {
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint32_t padding;
};

struct powerfs_remove_req {
    uint64_t dir_ino;
    char name[POWERFS_MAX_DATA_SIZE - sizeof(uint64_t)];
};

struct powerfs_readdir_req {
    uint64_t dir_ino;
    uint64_t offset;
    uint32_t max_entries;
    uint32_t padding;
};

struct powerfs_dirent {
    uint64_t ino;
    uint32_t type;
    uint32_t name_len;
    char name[256];
};

struct powerfs_read_req {
    uint64_t ino;
    uint64_t offset;
    uint32_t length;
    uint32_t padding;
};

struct powerfs_read_resp {
    uint32_t length;
    uint32_t padding;
    uint8_t data[POWERFS_MAX_DATA_SIZE - sizeof(uint32_t) - sizeof(uint32_t)];
};

struct powerfs_write_req {
    uint64_t ino;
    uint64_t offset;
    uint32_t length;
    uint32_t padding;
};

struct powerfs_write_resp {
    uint32_t written;
    uint32_t padding;
};

struct powerfs_setattr_req {
    uint64_t ino;
    uint32_t ia_valid;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime_sec;
    uint32_t atime_nsec;
    uint32_t padding1;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint32_t padding2;
    uint64_t ctime_sec;
    uint32_t ctime_nsec;
    uint32_t padding3;
};

struct powerfs_setattr_resp {
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime_sec;
    uint32_t atime_nsec;
    uint32_t padding1;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint32_t padding2;
    uint64_t ctime_sec;
    uint32_t ctime_nsec;
    uint32_t padding3;
    uint64_t blocks;
    uint32_t blksize;
    uint32_t padding4;
};

struct powerfs_rename_req {
    uint64_t old_dir_ino;
    uint64_t new_dir_ino;
    uint32_t old_name_len;
    uint32_t new_name_len;
    uint32_t flags;
    uint32_t padding;
    char old_name[256];
    char new_name[256];
};

struct powerfs_rename_resp {
    uint32_t padding;
};

struct powerfs_symlink_req {
    uint64_t dir_ino;
    uint32_t name_len;
    uint32_t symname_len;
    char name[256];
    char symname[POWERFS_MAX_DATA_SIZE - 2 * sizeof(uint64_t) - 2 * sizeof(uint32_t) - 256];
};

struct powerfs_symlink_resp {
    uint64_t ino;
    uint32_t mode;
    uint32_t padding;
};

struct powerfs_readlink_req {
    uint64_t ino;
};

struct powerfs_readlink_resp {
    uint32_t len;
    uint32_t padding;
    char target[POWERFS_MAX_DATA_SIZE - 2 * sizeof(uint32_t)];
};

struct powerfs_link_req {
    uint64_t ino;
    uint64_t dir_ino;
    char name[256];
};

struct powerfs_link_resp {
    uint32_t padding;
};

struct powerfs_mknod_req {
    uint64_t dir_ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t padding;
    uint64_t dev;
    char name[256];
};

struct powerfs_mknod_resp {
    uint64_t ino;
    uint32_t mode;
    uint32_t padding;
};

struct powerfs_fsync_req {
    uint64_t ino;
    uint32_t datasync;
    uint32_t padding;
};

struct powerfs_fsync_resp {
    uint32_t padding;
};

struct powerfs_truncate_req {
    uint64_t ino;
    uint64_t size;
};

struct powerfs_truncate_resp {
    uint64_t size;
    uint32_t padding;
};

struct powerfs_statfs_req {
    uint32_t padding;
};

struct powerfs_statfs_resp {
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    uint32_t bsize;
    uint32_t namelen;
    uint32_t frsize;
    uint32_t padding;
};

struct powerfs_invalidate_req {
    uint32_t flags;
    uint32_t count;
    uint64_t ino[];
};

/* SETATTR 掩码 */
#define POWERFS_ATTR_MODE     (1 << 0)
#define POWERFS_ATTR_UID      (1 << 1)
#define POWERFS_ATTR_GID      (1 << 2)
#define POWERFS_ATTR_SIZE     (1 << 3)
#define POWERFS_ATTR_ATIME    (1 << 4)
#define POWERFS_ATTR_MTIME    (1 << 5)
#define POWERFS_ATTR_CTIME    (1 << 6)

/* ========== 代理实现 ========== */

struct proxy_config {
    char comm_dev[256];
    char master_addr[256];
    uint16_t master_port;
    int verbose;
    int running;
    int use_mmap;
};

static struct proxy_config config = {
    .comm_dev = "/dev/powerfs_comm",
    .master_addr = "127.0.0.1",
    .master_port = 9333,
    .verbose = 1,
    .running = 1,
    .use_mmap = 1,
};

static int g_comm_fd = -1;

/* 共享内存指针 */
static void *g_shm_base = NULL;
static struct powerfs_shm_header *g_shm_hdr = NULL;
static struct powerfs_shm_entry *g_sq_entries = NULL;
static struct powerfs_shm_entry *g_cq_entries = NULL;
static void *g_data_area = NULL;

/* 全局互斥锁 - 保护文件系统状态的并发访问 */
static pthread_mutex_t g_fs_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ========== 内存文件系统存储 (模拟后端) ========== */

#define MAX_INODES 8192      /* 减小数组大小，避免内存问题 */
#define MAX_SYMLINK_TARGET 1024  /* 符号链接目标最大长度 */

struct inode_entry {
    int used;
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t atime_sec;
    uint32_t atime_nsec;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint64_t ctime_sec;
    uint32_t ctime_nsec;
    uint8_t *data;       /* 文件数据 */
    uint32_t data_size;
    char *target;        /* 符号链接目标 (动态分配) */
    int is_symlink;
};

static struct inode_entry g_inodes[MAX_INODES];
static uint64_t g_next_ino = 1;

/* 目录项缓存: parent_ino -> [(name, ino, type)] */
#define MAX_DENTRIES 64
#define MAX_DIR_ENTRIES 1024

struct dir_entry {
    uint64_t ino;
    char name[256];
    uint32_t type;  /* DT_REG, DT_DIR, DT_LNK */
    int used;
};

struct dir_cache {
    int used;
    uint64_t parent_ino;
    struct dir_entry entries[MAX_DIR_ENTRIES];
    uint32_t count;
};

static struct dir_cache g_dir_caches[MAX_DENTRIES];

/* 持久化存储路径 */
#define POWERFS_DATA_FILE "/var/powerfs_data.dat"
#define POWERFS_DATA_MAGIC 0x50574441  /* 'PTdA' */
#define POWERFS_DATA_VERSION 1

/* 持久化数据文件头 */
struct powerfs_data_header {
    uint32_t magic;
    uint32_t version;
    uint32_t inode_count;
    uint32_t dir_cache_count;
    uint64_t next_ino;
};

/* 持久化 inode 条目 */
struct persisted_inode {
    uint32_t used;
    uint64_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t atime_sec;
    uint32_t atime_nsec;
    uint64_t mtime_sec;
    uint32_t mtime_nsec;
    uint64_t ctime_sec;
    uint32_t ctime_nsec;
    uint32_t data_size;
    uint32_t is_symlink;
    uint32_t target_len;
};

/* 持久化目录缓存条目 */
struct persisted_dir_entry {
    uint64_t ino;
    uint32_t type;
    uint32_t name_len;
    char name[256];
};

struct persisted_dir_cache {
    uint32_t used;
    uint64_t parent_ino;
    uint32_t count;
    struct persisted_dir_entry entries[MAX_DIR_ENTRIES];
};

/* 前向声明 */
static struct inode_entry *find_inode(uint64_t ino);
static struct inode_entry *alloc_inode(void);
static struct dir_cache *alloc_dir_cache(uint64_t parent_ino);
static int open_comm_device(void);
static void close_comm_device(void);
static int setup_mmap(void);

/*
 * save_state - 保存当前状态到文件
 * 使用互斥锁保护，防止并发访问导致崩溃
 */
static int save_state(const char *filename)
{
    FILE *fp;
    struct powerfs_data_header header;
    uint32_t i, j;
    uint32_t inode_count = 0, dir_cache_count = 0;

    /* 获取锁保护状态读取 */
    pthread_mutex_lock(&g_fs_mutex);

    /* 统计有效 inode 和 dir cache */
    for (i = 0; i < MAX_INODES; i++) {
        if (g_inodes[i].used)
            inode_count++;
    }
    for (i = 0; i < MAX_DENTRIES; i++) {
        if (g_dir_caches[i].used)
            dir_cache_count++;
    }

    fp = fopen(filename, "wb");
    if (!fp) {
        printf("[proxy] 无法保存状态到 %s\n", filename);
        pthread_mutex_unlock(&g_fs_mutex);
        return -1;
    }

    /* 写文件头 */
    header.magic = POWERFS_DATA_MAGIC;
    header.version = POWERFS_DATA_VERSION;
    header.inode_count = inode_count;
    header.dir_cache_count = dir_cache_count;
    header.next_ino = g_next_ino;
    fwrite(&header, sizeof(header), 1, fp);

    /* 写 inode - 加锁保护下进行 */
    for (i = 0; i < MAX_INODES; i++) {
        struct persisted_inode pi;
        if (!g_inodes[i].used)
            continue;

        memset(&pi, 0, sizeof(pi));
        pi.used = g_inodes[i].used;
        pi.ino = g_inodes[i].ino;
        pi.mode = g_inodes[i].mode;
        pi.uid = g_inodes[i].uid;
        pi.gid = g_inodes[i].gid;
        pi.nlink = g_inodes[i].nlink;
        pi.size = g_inodes[i].size;
        pi.atime_sec = g_inodes[i].atime_sec;
        pi.atime_nsec = g_inodes[i].atime_nsec;
        pi.mtime_sec = g_inodes[i].mtime_sec;
        pi.mtime_nsec = g_inodes[i].mtime_nsec;
        pi.ctime_sec = g_inodes[i].ctime_sec;
        pi.ctime_nsec = g_inodes[i].ctime_nsec;
        pi.data_size = g_inodes[i].data_size;
        pi.is_symlink = g_inodes[i].is_symlink;
        pi.target_len = g_inodes[i].target ? strlen(g_inodes[i].target) : 0;

        fwrite(&pi, sizeof(pi), 1, fp);

        /* 写数据 - 检查指针有效性 */
        if (g_inodes[i].data && g_inodes[i].data_size > 0 && 
            g_inodes[i].data_size <= MAX_SYMLINK_TARGET * 1024) {
            fwrite(g_inodes[i].data, 1, g_inodes[i].data_size, fp);
        }

        /* 写符号链接目标 - 检查指针有效性 */
        if (g_inodes[i].target && pi.target_len > 0 && 
            pi.target_len <= MAX_SYMLINK_TARGET) {
            fwrite(g_inodes[i].target, 1, pi.target_len, fp);
        }
    }

    /* 写目录缓存 - 在锁保护下进行 */
    for (i = 0; i < MAX_DENTRIES; i++) {
        struct persisted_dir_cache pdc;
        if (!g_dir_caches[i].used)
            continue;

        /* 验证 count 有效性 */
        uint32_t safe_count = g_dir_caches[i].count;
        if (safe_count > MAX_DIR_ENTRIES)
            safe_count = MAX_DIR_ENTRIES;

        memset(&pdc, 0, sizeof(pdc));
        pdc.used = g_dir_caches[i].used;
        pdc.parent_ino = g_dir_caches[i].parent_ino;
        pdc.count = safe_count;

        for (j = 0; j < safe_count; j++) {
            /* 只保存已使用的条目 */
            if (g_dir_caches[i].entries[j].used) {
                pdc.entries[j].ino = g_dir_caches[i].entries[j].ino;
                pdc.entries[j].type = g_dir_caches[i].entries[j].type;
                pdc.entries[j].name_len = strlen(g_dir_caches[i].entries[j].name);
                strncpy(pdc.entries[j].name, g_dir_caches[i].entries[j].name, 255);
            }
        }

        fwrite(&pdc, sizeof(pdc), 1, fp);
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    /* 状态保存日志改为 verbose 级别控制 */
    if (config.verbose > 1) {
        fprintf(stderr, "[proxy] 状态已保存到 %s (inode=%u, dir_cache=%u)\n",
                filename, inode_count, dir_cache_count);
        fflush(stderr);
    }

    /* 释放锁 */
    pthread_mutex_unlock(&g_fs_mutex);
    
    return 0;
}

/*
 * load_state - 从文件加载状态
 */
static int load_state(const char *filename)
{
    FILE *fp;
    struct powerfs_data_header header;
    uint32_t i, j;

    fp = fopen(filename, "rb");
    if (!fp) {
        printf("[proxy] 未找到持久化数据文件，将使用空文件系统\n");
        return -1;
    }

    fprintf(stderr, "[proxy] 打开数据文件 %s 成功，大小=%ld\n",
            filename, ftell(fp));
    fflush(stderr);

    /* 读文件头 */
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fprintf(stderr, "[proxy] 读取数据文件头失败 (size=%zu)\n", sizeof(header));
        fflush(stderr);
        fclose(fp);
        return -1;
    }

    fprintf(stderr, "[proxy] 读取文件头: magic=0x%x version=%u inode_count=%u dir_cache_count=%u\n",
            header.magic, header.version, header.inode_count, header.dir_cache_count);
    fflush(stderr);

    if (header.magic != POWERFS_DATA_MAGIC) {
        printf("[proxy] 数据文件魔数不匹配\n");
        fclose(fp);
        return -1;
    }

    if (header.version != POWERFS_DATA_VERSION) {
        printf("[proxy] 数据文件版本不匹配\n");
        fclose(fp);
        return -1;
    }

    fprintf(stderr, "[proxy] 从 %s 加载状态 (inode=%u, dir_cache=%u)\n",
            filename, header.inode_count, header.dir_cache_count);

    /* 读 inode */
    for (i = 0; i < header.inode_count; i++) {
        struct persisted_inode pi;
        struct inode_entry *ie;

        if (fread(&pi, sizeof(pi), 1, fp) != 1) {
            fprintf(stderr, "[proxy] 读取 inode %u 失败\n", i);
            break;
        }

        /* 详细日志改为汇总输出，避免串口控制台过载导致 RCU stall */
        ie = find_inode(pi.ino);
        if (!ie)
            ie = alloc_inode();
        if (!ie) {
            printf("[proxy] 分配 inode 失败\n");
            break;
        }

        ie->used = pi.used;
        ie->ino = pi.ino;
        ie->mode = pi.mode;
        ie->uid = pi.uid;
        ie->gid = pi.gid;
        ie->nlink = pi.nlink;
        ie->size = pi.size;
        ie->atime_sec = pi.atime_sec;
        ie->atime_nsec = pi.atime_nsec;
        ie->mtime_sec = pi.mtime_sec;
        ie->mtime_nsec = pi.mtime_nsec;
        ie->ctime_sec = pi.ctime_sec;
        ie->ctime_nsec = pi.ctime_nsec;
        ie->data_size = pi.data_size;
        ie->is_symlink = pi.is_symlink;

        /* 读数据 */
        if (pi.data_size > 0) {
            ie->data = malloc(pi.data_size);
            if (ie->data) {
                if (fread(ie->data, 1, pi.data_size, fp) != pi.data_size) {
                    free(ie->data);
                    ie->data = NULL;
                    ie->data_size = 0;
                }
            } else {
                /* 跳过数据 */
                fseek(fp, pi.data_size, SEEK_CUR);
            }
        }

        /* 读符号链接目标 */
        if (pi.target_len > 0) {
            ie->target = malloc(pi.target_len + 1);
            if (ie->target) {
                if (fread(ie->target, 1, pi.target_len, fp) != pi.target_len) {
                    free(ie->target);
                    ie->target = NULL;
                    ie->is_symlink = 0;
                } else {
                    ie->target[pi.target_len] = '\0';
                }
            } else {
                fseek(fp, pi.target_len, SEEK_CUR);
            }
        }
    }

    /* 更新 next_ino */
    g_next_ino = header.next_ino;

    /* 读目录缓存 */
    for (i = 0; i < header.dir_cache_count; i++) {
        struct persisted_dir_cache pdc;
        struct dir_cache *dc;

        if (fread(&pdc, sizeof(pdc), 1, fp) != 1) {
            printf("[proxy] 读取 dir_cache %u 失败\n", i);
            break;
        }

        /* 详细日志改为汇总输出，避免串口控制台过载导致 RCU stall */
        dc = alloc_dir_cache(pdc.parent_ino);
        if (!dc) {
            fprintf(stderr, "[proxy] 分配 dir_cache 失败\n");
            break;
        }

        dc->count = pdc.count;
        for (j = 0; j < pdc.count && j < MAX_DIR_ENTRIES; j++) {
            dc->entries[j].used = 1;
            dc->entries[j].ino = pdc.entries[j].ino;
            dc->entries[j].type = pdc.entries[j].type;
            strncpy(dc->entries[j].name, pdc.entries[j].name, 255);
            dc->entries[j].name[255] = '\0';
        }
    }

    fclose(fp);
    fprintf(stderr, "[proxy] 状态加载完成 (inode=%u, dir_cache=%u)\n",
            header.inode_count, header.dir_cache_count);
    fflush(stderr);
    return 0;
}

/* 初始化根目录 */
static void init_inodes(void)
{
    memset(g_inodes, 0, sizeof(g_inodes));
    memset(g_dir_caches, 0, sizeof(g_dir_caches));

    /* 根 inode */
    g_inodes[1].used = 1;
    g_inodes[1].ino = 1;
    g_inodes[1].mode = 0755 | (4 << 12);  /* S_IFDIR */
    g_inodes[1].uid = 0;
    g_inodes[1].gid = 0;
    g_inodes[1].nlink = 2;
    g_inodes[1].size = 4096;
    g_inodes[1].atime_sec = time(NULL);
    g_inodes[1].mtime_sec = time(NULL);
    g_inodes[1].ctime_sec = time(NULL);

    /* 根目录缓存 */
    g_dir_caches[0].used = 1;
    g_dir_caches[0].parent_ino = 1;
    g_dir_caches[0].count = 0;

    /* 添加 . 和 .. */
    strncpy(g_dir_caches[0].entries[0].name, ".", 255);
    g_dir_caches[0].entries[0].ino = 1;
    g_dir_caches[0].entries[0].type = 4;  /* DT_DIR */
    g_dir_caches[0].entries[0].used = 1;

    strncpy(g_dir_caches[0].entries[1].name, "..", 255);
    g_dir_caches[0].entries[1].ino = 1;
    g_dir_caches[0].entries[1].type = 4;  /* DT_DIR */
    g_dir_caches[0].entries[1].used = 1;
    g_dir_caches[0].count = 2;

    g_next_ino = 2;

    printf("[proxy] 内存文件系统已初始化 (根目录 ino=1)\n");
}

static struct inode_entry *find_inode(uint64_t ino)
{
    uint32_t i;
    for (i = 0; i < MAX_INODES; i++) {
        if (g_inodes[i].used && g_inodes[i].ino == ino)
            return &g_inodes[i];
    }
    return NULL;
}

static struct inode_entry *alloc_inode(void)
{
    uint32_t i;
    for (i = 0; i < MAX_INODES; i++) {
        if (!g_inodes[i].used) {
            memset(&g_inodes[i], 0, sizeof(g_inodes[i]));
            g_inodes[i].used = 1;
            g_inodes[i].ino = g_next_ino++;
            g_inodes[i].atime_sec = time(NULL);
            g_inodes[i].mtime_sec = time(NULL);
            g_inodes[i].ctime_sec = time(NULL);
            return &g_inodes[i];
        }
    }
    return NULL;
}

static struct dir_cache *find_dir_cache(uint64_t parent_ino)
{
    uint32_t i;
    for (i = 0; i < MAX_DENTRIES; i++) {
        if (g_dir_caches[i].used && g_dir_caches[i].parent_ino == parent_ino)
            return &g_dir_caches[i];
    }
    return NULL;
}

static struct dir_cache *alloc_dir_cache(uint64_t parent_ino)
{
    uint32_t i;
    struct dir_cache *dc;

    dc = find_dir_cache(parent_ino);
    if (dc)
        return dc;

    for (i = 0; i < MAX_DENTRIES; i++) {
        if (!g_dir_caches[i].used) {
            memset(&g_dir_caches[i], 0, sizeof(g_dir_caches[i]));
            g_dir_caches[i].used = 1;
            g_dir_caches[i].parent_ino = parent_ino;
            g_dir_caches[i].count = 0;
            return &g_dir_caches[i];
        }
    }
    return NULL;
}

static int add_dir_entry(uint64_t parent_ino, const char *name, uint64_t ino, uint32_t type)
{
    struct dir_cache *dc;
    uint32_t i;

    dc = alloc_dir_cache(parent_ino);
    if (!dc)
        return -ENOMEM;

    /* 检查是否已存在 */
    for (i = 0; i < dc->count; i++) {
        if (dc->entries[i].used && strcmp(dc->entries[i].name, name) == 0) {
            /* 详细日志改为 verbose 级别控制 */
            dc->entries[i].ino = ino;
            dc->entries[i].type = type;
            return 0;
        }
    }

    if (dc->count >= MAX_DIR_ENTRIES) {
        fprintf(stderr, "[proxy] add_dir_entry: 目录已满 (count=%u max=%u)\n",
                dc->count, MAX_DIR_ENTRIES);
        return -ENOSPC;
    }

    strncpy(dc->entries[dc->count].name, name, 255);
    dc->entries[dc->count].name[255] = '\0';
    dc->entries[dc->count].ino = ino;
    dc->entries[dc->count].type = type;
    dc->entries[dc->count].used = 1;
    dc->count++;

    /* 详细日志改为 verbose 级别控制 */

    return 0;
}

static int remove_dir_entry(uint64_t parent_ino, const char *name)
{
    struct dir_cache *dc;
    uint32_t i;

    dc = find_dir_cache(parent_ino);
    if (!dc)
        return -ENOENT;

    for (i = 0; i < dc->count; i++) {
        if (dc->entries[i].used && strcmp(dc->entries[i].name, name) == 0) {
            dc->entries[i].used = 0;
            /* 移动后续条目 */
            if (i < dc->count - 1) {
                memmove(&dc->entries[i], &dc->entries[i + 1],
                        (dc->count - i - 1) * sizeof(struct dir_entry));
            }
            dc->count--;
            return 0;
        }
    }
    return -ENOENT;
}

/* ========== 请求处理函数 ========== */

static int handle_lookup(struct powerfs_lookup_req *req,
                         struct powerfs_lookup_resp *resp)
{
    struct dir_cache *dc;
    uint32_t i;

    memset(resp, 0, sizeof(*resp));
    dc = find_dir_cache(req->dir_ino);
    if (!dc) {
        resp->ino = 0;
        return -ENOENT;
    }

    for (i = 0; i < dc->count; i++) {
        if (dc->entries[i].used && strcmp(dc->entries[i].name, req->name) == 0) {
            struct inode_entry *ie = find_inode(dc->entries[i].ino);
            if (ie) {
                resp->ino = ie->ino;
                resp->mode = ie->mode;
                resp->uid = ie->uid;
                resp->gid = ie->gid;
                resp->nlink = ie->nlink;
                resp->size = ie->size;
                resp->mtime_sec = ie->mtime_sec;
                return 0;
            }
        }
    }

    return -ENOENT;
}

static int handle_getattr(struct powerfs_getattr_req *req,
                           struct powerfs_getattr_resp *resp)
{
    struct inode_entry *ie;

    memset(resp, 0, sizeof(*resp));
    ie = find_inode(req->ino);
    if (!ie)
        return -ENOENT;

    resp->mode = ie->mode;
    resp->nlink = ie->nlink;
    resp->uid = ie->uid;
    resp->gid = ie->gid;
    resp->size = ie->size;
    resp->atime_sec = ie->atime_sec;
    resp->mtime_sec = ie->mtime_sec;
    resp->ctime_sec = ie->ctime_sec;
    resp->blocks = (ie->size + 511) / 512;
    resp->blksize = 4096;

    return 0;
}

static int handle_setattr(struct powerfs_setattr_req *req,
                           struct powerfs_setattr_resp *resp)
{
    struct inode_entry *ie;

    memset(resp, 0, sizeof(*resp));
    ie = find_inode(req->ino);
    if (!ie)
        return -ENOENT;

    if (req->ia_valid & POWERFS_ATTR_MODE)
        ie->mode = req->mode;
    if (req->ia_valid & POWERFS_ATTR_UID)
        ie->uid = req->uid;
    if (req->ia_valid & POWERFS_ATTR_GID)
        ie->gid = req->gid;
    if (req->ia_valid & POWERFS_ATTR_SIZE)
        ie->size = req->size;
    if (req->ia_valid & POWERFS_ATTR_ATIME) {
        ie->atime_sec = req->atime_sec;
        ie->atime_nsec = req->atime_nsec;
    }
    if (req->ia_valid & POWERFS_ATTR_MTIME) {
        ie->mtime_sec = req->mtime_sec;
        ie->mtime_nsec = req->mtime_nsec;
    }
    if (req->ia_valid & POWERFS_ATTR_CTIME) {
        ie->ctime_sec = req->ctime_sec;
        ie->ctime_nsec = req->ctime_nsec;
    }

    /* 返回更新后的属性 */
    resp->mode = ie->mode;
    resp->nlink = ie->nlink;
    resp->uid = ie->uid;
    resp->gid = ie->gid;
    resp->size = ie->size;
    resp->atime_sec = ie->atime_sec;
    resp->mtime_sec = ie->mtime_sec;
    resp->ctime_sec = ie->ctime_sec;
    resp->blocks = (ie->size + 511) / 512;
    resp->blksize = 4096;

    return 0;
}

static int handle_create(struct powerfs_create_req *req,
                          struct powerfs_create_resp *resp, int is_dir)
{
    struct inode_entry *ie;
    mode_t mode;

    memset(resp, 0, sizeof(*resp));

    mode = req->mode;
    if (is_dir)
        mode |= (4 << 12);  /* S_IFDIR */
    else
        mode |= (8 << 12);  /* S_IFREG */

    /* 获取锁保护数据修改 */
    pthread_mutex_lock(&g_fs_mutex);

    /*
     * 如果内核分配了 inode 号 (new_ino > 0)，使用它
     * 否则 (new_ino == 0)，代理自行分配
     */
    if (req->new_ino > 0) {
        /* 内核指定了 inode 号，查找或创建对应的 inode */
        ie = find_inode(req->new_ino);
        if (ie) {
            /* 已存在 (重新挂载等场景)，直接使用 */
            ie->used = 1;
        } else {
            /* 创建新 inode，使用内核指定的 ino */
            uint32_t i;
            for (i = 0; i < MAX_INODES; i++) {
                if (!g_inodes[i].used) {
                    ie = &g_inodes[i];
                    memset(ie, 0, sizeof(*ie));
                    ie->used = 1;
                    ie->ino = req->new_ino;
                    ie->atime_sec = time(NULL);
                    ie->mtime_sec = time(NULL);
                    ie->ctime_sec = time(NULL);
                    break;
                }
            }
            if (!ie) {
                pthread_mutex_unlock(&g_fs_mutex);
                return -ENOSPC;
            }
        }
    } else {
        /* 旧协议或未指定 ino，自行分配 */
        ie = alloc_inode();
        if (!ie) {
            pthread_mutex_unlock(&g_fs_mutex);
            return -ENOSPC;
        }
    }

    ie->mode = mode;
    ie->uid = req->uid;
    ie->gid = req->gid;
    ie->nlink = is_dir ? 2 : 1;
    ie->size = is_dir ? 4096 : 0;

    /* 在父目录中添加条目 */
    add_dir_entry(req->dir_ino, req->name, ie->ino, is_dir ? 4 : 8);  /* 4=DT_DIR, 8=DT_REG */

    resp->ino = ie->ino;
    resp->mode = ie->mode;
    resp->uid = ie->uid;
    resp->gid = ie->gid;
    resp->nlink = ie->nlink;
    resp->size = ie->size;
    resp->mtime_sec = ie->mtime_sec;

    /* 释放锁 */
    pthread_mutex_unlock(&g_fs_mutex);

    return 0;
}

static int handle_remove(struct powerfs_remove_req *req, int is_dir)
{
    struct dir_cache *dc;
    uint32_t i;
    int ret = -ENOENT;

    /* 详细日志改为 verbose 级别控制，避免串口控制台过载导致 RCU stall */
    if (config.verbose > 1) {
        fprintf(stderr, "[proxy] handle_remove: dir_ino=%lu, name='%s', is_dir=%d\n",
               (unsigned long)req->dir_ino, req->name, is_dir);
    }

    /* 获取锁保护数据修改 */
    pthread_mutex_lock(&g_fs_mutex);

    dc = find_dir_cache(req->dir_ino);
    if (!dc) {
        pthread_mutex_unlock(&g_fs_mutex);
        return -ENOENT;
    }

    for (i = 0; i < dc->count && i < MAX_DIR_ENTRIES; i++) {
        if (dc->entries[i].used && strcmp(dc->entries[i].name, req->name) == 0) {
            uint64_t target_ino = dc->entries[i].ino;

            struct inode_entry *ie = find_inode(target_ino);
            if (ie) {
                if (is_dir) {
                    /* 检查目录是否为空 (除了 . 和 ..) */
                    if (ie->ino != 1) {
                        struct dir_cache *sub = find_dir_cache(ie->ino);
                        if (sub && sub->count > 2) {  /* 大于 . 和 .. */
                            pthread_mutex_unlock(&g_fs_mutex);
                            return -ENOTEMPTY;
                        }
                    }
                }

                /* 减少 nlink，仅当 nlink 归零时才真正删除 inode (处理硬链接) */
                if (ie->nlink > 0)
                    ie->nlink--;

                /* 从父目录移除目录项 */
                if (ie->ino != 1)
                    remove_dir_entry(req->dir_ino, req->name);

                /* 只有当 nlink 归零时才真正释放 inode */
                if (ie->nlink == 0) {
                    ie->used = 0;
                    if (ie->data) {
                        free(ie->data);
                        ie->data = NULL;
                    }
                    if (ie->target) {
                        free(ie->target);
                        ie->target = NULL;
                    }
                }

                ret = 0;
            }
            break;
        }
    }

    /* 释放锁 */
    pthread_mutex_unlock(&g_fs_mutex);
    return ret;
}

static int handle_rename(struct powerfs_rename_req *req,
                          struct powerfs_rename_resp *resp)
{
    struct dir_cache *old_dc, *new_dc;
    uint32_t i;
    uint64_t ino = 0;
    uint32_t type = 0;

    memset(resp, 0, sizeof(*resp));

    /* 详细日志改为 verbose 级别控制，避免串口控制台过载导致 RCU stall */
    if (config.verbose > 1) {
        fprintf(stderr, "[proxy] handle_rename: old_dir=%lu old_name='%s' -> new_dir=%lu new_name='%s'\n",
               (unsigned long)req->old_dir_ino, req->old_name,
               (unsigned long)req->new_dir_ino, req->new_name);
    }

    /* 获取锁保护数据修改 */
    pthread_mutex_lock(&g_fs_mutex);

    old_dc = find_dir_cache(req->old_dir_ino);
    if (!old_dc) {
        pthread_mutex_unlock(&g_fs_mutex);
        return -ENOENT;
    }

    /* 查找源文件 */
    for (i = 0; i < old_dc->count; i++) {
        if (old_dc->entries[i].used &&
            strcmp(old_dc->entries[i].name, req->old_name) == 0) {
            ino = old_dc->entries[i].ino;
            type = old_dc->entries[i].type;
            break;
        }
    }

    if (ino == 0) {
        pthread_mutex_unlock(&g_fs_mutex);
        return -ENOENT;
    }

    /*
     * 注意: rename 操作不改变 nlink!
     * rename 只是改变文件名 (dentry)，不涉及硬链接的创建或删除。
     * 内核 rename 操作不会改变 inode 的 nlink。
     */

    /* 检查目标是否存在并删除 (处理覆盖目标的情况) */
    new_dc = find_dir_cache(req->new_dir_ino);
    if (new_dc) {
        for (i = 0; i < new_dc->count; i++) {
            if (new_dc->entries[i].used &&
                strcmp(new_dc->entries[i].name, req->new_name) == 0) {
                struct inode_entry *target_ie = find_inode(new_dc->entries[i].ino);
                if (target_ie) {
                    /* 减少目标 inode 的 nlink (覆盖目标相当于 unlink) */
                    if (target_ie->nlink > 0)
                        target_ie->nlink--;
                    if (target_ie->nlink == 0) {
                        target_ie->used = 0;
                        if (target_ie->data) {
                            free(target_ie->data);
                            target_ie->data = NULL;
                        }
                        if (target_ie->target) {
                            free(target_ie->target);
                            target_ie->target = NULL;
                        }
                    }
                }
                new_dc->entries[i].used = 0;
                break;
            }
        }
    }

    /* 从源目录移除旧名称，添加到目标目录使用新名称 */
    remove_dir_entry(req->old_dir_ino, req->old_name);
    add_dir_entry(req->new_dir_ino, req->new_name, ino, type);

    /* 释放锁 */
    pthread_mutex_unlock(&g_fs_mutex);

    return 0;
}

static int handle_readdir(uint64_t dir_ino, uint64_t offset,
                           uint32_t max_entries,
                           struct powerfs_dirent *out_entries,
                           uint32_t *out_count)
{
    struct dir_cache *dc;
    uint32_t i, count = 0;

    dc = find_dir_cache(dir_ino);
    if (!dc) {
        *out_count = 0;
        return 0;
    }

    for (i = offset; i < dc->count && count < max_entries; i++) {
        if (dc->entries[i].used) {
            memset(&out_entries[count], 0, sizeof(struct powerfs_dirent));
            out_entries[count].ino = dc->entries[i].ino;
            out_entries[count].type = dc->entries[i].type;
            out_entries[count].name_len = strlen(dc->entries[i].name);
            strncpy(out_entries[count].name, dc->entries[i].name, 255);
            count++;
        }
    }

    *out_count = count;
    return 0;
}

static int handle_read(struct powerfs_read_req *req,
                        struct powerfs_read_resp *resp)
{
    struct inode_entry *ie;
    uint64_t end;

    memset(resp, 0, sizeof(*resp));
    ie = find_inode(req->ino);
    if (!ie)
        return -ENOENT;

    end = req->offset + req->length;
    if (end > ie->size)
        end = ie->size;

    if (req->offset >= ie->size) {
        resp->length = 0;
        return 0;
    }

    /* 限制单次读取不超过响应缓冲区大小 */
    uint32_t max_data = sizeof(resp->data);
    uint64_t read_len = end - req->offset;
    if (read_len > max_data)
        read_len = max_data;

    if (ie->data && req->offset < ie->data_size) {
        uint64_t copy_len = read_len;
        if (req->offset + copy_len > ie->data_size)
            copy_len = ie->data_size - req->offset;
        memcpy(resp->data, ie->data + req->offset, copy_len);
        resp->length = copy_len;
    } else {
        resp->length = 0;
    }

    return 0;
}

static int handle_write(struct powerfs_write_req *req,
                         struct powerfs_write_resp *resp,
                         const uint8_t *data)
{
    struct inode_entry *ie;
    uint64_t end;

    memset(resp, 0, sizeof(*resp));

    /* 获取锁保护数据修改 */
    pthread_mutex_lock(&g_fs_mutex);

    ie = find_inode(req->ino);
    if (!ie) {
        pthread_mutex_unlock(&g_fs_mutex);
        return -ENOENT;
    }

    end = req->offset + req->length;

    /* 扩展文件数据缓冲区 */
    if (end > ie->data_size) {
        uint8_t *new_data = realloc(ie->data, end);
        if (!new_data) {
            pthread_mutex_unlock(&g_fs_mutex);
            return -ENOMEM;
        }
        ie->data = new_data;
        memset(ie->data + ie->data_size, 0, end - ie->data_size);
        ie->data_size = end;
    }

    /* 写入数据 */
    if (data && req->length > 0) {
        memcpy(ie->data + req->offset, data, req->length);
    }

    if (end > ie->size)
        ie->size = end;

    ie->mtime_sec = time(NULL);
    resp->written = req->length;

    /* 释放锁 */
    pthread_mutex_unlock(&g_fs_mutex);

    return 0;
}

static int handle_symlink(struct powerfs_symlink_req *req,
                           struct powerfs_symlink_resp *resp)
{
    struct inode_entry *ie;
    uint32_t symname_len;

    memset(resp, 0, sizeof(*resp));

    ie = alloc_inode();
    if (!ie)
        return -ENOSPC;

    ie->mode = 0777 | (10 << 12);  /* S_IFLNK */
    ie->uid = 0;
    ie->gid = 0;
    ie->nlink = 1;
    ie->is_symlink = 1;

    /* 动态分配符号链接目标存储空间 */
    symname_len = strlen(req->symname);
    if (symname_len >= MAX_SYMLINK_TARGET)
        symname_len = MAX_SYMLINK_TARGET - 1;

    ie->target = malloc(symname_len + 1);
    if (!ie->target)
        return -ENOMEM;

    strncpy(ie->target, req->symname, symname_len);
    ie->target[symname_len] = '\0';
    ie->size = symname_len;

    add_dir_entry(req->dir_ino, req->name, ie->ino, 10);  /* 10=DT_LNK */

    resp->ino = ie->ino;
    resp->mode = ie->mode;

    return 0;
}

static int handle_readlink(struct powerfs_readlink_req *req,
                            struct powerfs_readlink_resp *resp)
{
    struct inode_entry *ie;

    memset(resp, 0, sizeof(*resp));
    ie = find_inode(req->ino);
    if (!ie)
        return -ENOENT;

    if (!ie->is_symlink)
        return -EINVAL;

    if (!ie->target) {
        resp->len = 0;
        resp->target[0] = '\0';
        return 0;
    }

    resp->len = strlen(ie->target);
    if (resp->len >= sizeof(resp->target))
        resp->len = sizeof(resp->target) - 1;

    memcpy(resp->target, ie->target, resp->len);
    resp->target[resp->len] = '\0';

    return 0;
}

static int handle_link(struct powerfs_link_req *req,
                       struct powerfs_link_resp *resp)
{
    struct inode_entry *ie;

    memset(resp, 0, sizeof(*resp));

    /* 获取锁保护数据修改 */
    pthread_mutex_lock(&g_fs_mutex);

    /* 查找源 inode */
    ie = find_inode(req->ino);
    if (!ie) {
        pthread_mutex_unlock(&g_fs_mutex);
        return -ENOENT;
    }

    /* 增加 nlink (硬链接创建) */
    ie->nlink++;

    /* 在目标目录添加目录项 (硬链接指向同一 inode) */
    add_dir_entry(req->dir_ino, req->name, req->ino,
                  (ie->mode & S_IFMT) == S_IFDIR ? 2 : 1);

    /* 释放锁 */
    pthread_mutex_unlock(&g_fs_mutex);

    return 0;
}

static int handle_statfs(struct powerfs_statfs_resp *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->blocks = 1024 * 1024;   /* 1TB */
    resp->bfree = 512 * 1024;
    resp->bavail = 256 * 1024;
    resp->files = MAX_INODES;
    resp->ffree = MAX_INODES - g_next_ino;
    resp->bsize = 4096;
    resp->namelen = 255;
    resp->frsize = 4096;
    return 0;
}

/* ========== 共享内存操作 ========== */

static int init_mmap(int fd)
{
    size_t shm_size;

    shm_size = POWERFS_SHM_SIZE_ALIGNED(POWERFS_MAX_REQUESTS, POWERFS_MAX_DATA_SIZE);

    printf("[proxy] sizeof(msg_header)=%zu sizeof(shm_header)=%zu sizeof(shm_entry)=%zu\n",
           sizeof(struct powerfs_msg_header),
           sizeof(struct powerfs_shm_header),
           sizeof(struct powerfs_shm_entry));
    printf("[proxy] POWERFS_SHM_SIZE(%d, %d) = %zu\n",
           POWERFS_MAX_REQUESTS, POWERFS_MAX_DATA_SIZE, shm_size);

    g_shm_base = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (g_shm_base == MAP_FAILED) {
        perror("[proxy] mmap 失败");
        return -EINVAL;
    }

    g_shm_hdr = (struct powerfs_shm_header *)g_shm_base;
    g_sq_entries = (struct powerfs_shm_entry *)((char *)g_shm_base + sizeof(struct powerfs_shm_header));
    g_cq_entries = g_sq_entries + POWERFS_MAX_REQUESTS;
    g_data_area = (char *)g_shm_base + sizeof(struct powerfs_shm_header) +
                   2 * sizeof(struct powerfs_shm_entry) * POWERFS_MAX_REQUESTS;

    printf("[proxy] mmap 成功: size=%zu KB, version=%d, max_req=%d, max_data=%d\n",
           shm_size / 1024, g_shm_hdr->version,
           g_shm_hdr->max_requests, g_shm_hdr->max_data_size);

    if (g_shm_hdr->magic != POWERFS_COMM_MAGIC) {
        printf("[proxy] 警告: 魔数不匹配!\n");
    }

    return 0;
}

static int get_request(struct powerfs_msg_header *hdr, void *data, uint32_t *data_len)
{
    uint32_t sq_tail, sq_head, idx;
    struct powerfs_shm_entry *entry;

    /* 使用 atomic 读取确保获取最新值 (acquire 语义) */
    sq_tail = __atomic_load_n(&g_shm_hdr->sq_tail, __ATOMIC_SEQ_CST);
    sq_head = __atomic_load_n(&g_shm_hdr->sq_head, __ATOMIC_ACQUIRE);

    if (sq_tail == sq_head)
        return -EAGAIN;  /* 队列空 */

    idx = sq_tail % POWERFS_MAX_REQUESTS;
    entry = &g_sq_entries[idx];

    /* acquire 语义确保看到 entry->hdr 和 data 的最新值 */
    __atomic_load_n(&entry->hdr.type, __ATOMIC_ACQUIRE);

    memcpy(hdr, &entry->hdr, sizeof(struct powerfs_msg_header));

    if (entry->data_len > 0 && data) {
        if (entry->data_len > POWERFS_MAX_DATA_SIZE) {
            printf("[proxy] 错误: 数据过长 %u\n", entry->data_len);
            return -EINVAL;
        }
        memcpy(data, g_data_area + entry->data_offset, entry->data_len);
        if (data_len)
            *data_len = entry->data_len;
    } else if (data_len) {
        *data_len = 0;
    }

    /* 使用 release 语义更新 tail 指针，确保先处理完数据 */
    __atomic_store_n(&g_shm_hdr->sq_tail, (sq_tail + 1) % POWERFS_MAX_REQUESTS, __ATOMIC_RELEASE);

    return 0;
}

static int submit_response(struct powerfs_msg_header *hdr, const void *data)
{
    uint32_t cq_head, cq_tail, idx;
    struct powerfs_shm_entry *entry;

    /* 使用 atomic 读取确保获取最新值 */
    cq_head = __atomic_load_n(&g_shm_hdr->cq_head, __ATOMIC_SEQ_CST);
    cq_tail = __atomic_load_n(&g_shm_hdr->cq_tail, __ATOMIC_ACQUIRE);

    /* 检查 CQ 是否已满 */
    uint32_t next_head = (cq_head + 1) % POWERFS_MAX_REQUESTS;
    if (next_head == cq_tail)
        return -ENOSPC;

    idx = cq_head % POWERFS_MAX_REQUESTS;
    entry = &g_cq_entries[idx];

    /* 填充响应 */
    memcpy(&entry->hdr, hdr, sizeof(struct powerfs_msg_header));
    entry->data_offset = idx * POWERFS_MAX_DATA_SIZE;
    entry->data_len = hdr->data_len;

    if (data && hdr->data_len > 0) {
        memcpy(g_data_area + entry->data_offset, data, hdr->data_len);
    }

    /* 使用 release 语义更新 head 指针，确保数据已写入完成 */
    __atomic_store_n(&g_shm_hdr->cq_head, next_head, __ATOMIC_RELEASE);

    /* 唤醒内核 */
    uint64_t val = 1;
    ssize_t ret = write(g_comm_fd, &val, sizeof(val));
    (void)ret;

    return 0;
}

/* ========== IOCTL 回退模式 (当 MMAP 失败时使用) ========== */

static int ioctl_get_request(struct powerfs_msg_header *hdr, void *data, uint32_t *data_len)
{
    struct powerfs_ioctl_req ioctl_req;
    static uint8_t ioctl_buf[POWERFS_MAX_DATA_SIZE];
    int ret;

    memset(&ioctl_req, 0, sizeof(ioctl_req));
    ioctl_req.data = ioctl_buf;
    ioctl_req.data_size = POWERFS_MAX_DATA_SIZE;

    ret = ioctl(g_comm_fd, POWERFS_IOCTL_GET_REQ, &ioctl_req);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -EAGAIN;
        if (errno == ETIMEDOUT)
            return -EAGAIN;
        return -EIO;
    }

    memcpy(hdr, &ioctl_req.hdr, sizeof(struct powerfs_msg_header));
    if (ioctl_req.hdr.data_len > 0 && data) {
        if (ioctl_req.hdr.data_len > POWERFS_MAX_DATA_SIZE)
            return -EINVAL;
        memcpy(data, ioctl_buf, ioctl_req.hdr.data_len);
        if (data_len)
            *data_len = ioctl_req.hdr.data_len;
    } else if (data_len) {
        *data_len = 0;
    }

    return 0;
}

static int ioctl_submit_response(struct powerfs_msg_header *hdr, const void *data)
{
    struct powerfs_ioctl_req ioctl_req;
    static uint8_t ioctl_buf[POWERFS_MAX_DATA_SIZE];
    int ret;

    memset(&ioctl_req, 0, sizeof(ioctl_req));
    memcpy(&ioctl_req.hdr, hdr, sizeof(struct powerfs_msg_header));
    ioctl_req.data = ioctl_buf;
    ioctl_req.data_size = hdr->data_len;

    if (data && hdr->data_len > 0 && hdr->data_len <= POWERFS_MAX_DATA_SIZE) {
        memcpy(ioctl_buf, data, hdr->data_len);
    }

    ret = ioctl(g_comm_fd, POWERFS_IOCTL_SUBMIT_RESP, &ioctl_req);
    if (ret < 0)
        return -EIO;

    return 0;
}

/* ========== 主事件循环 ========== */

static void process_request(struct powerfs_msg_header *req_hdr, void *req_data)
{
    struct powerfs_msg_header resp_hdr;
    void *resp_data = NULL;
    int ret = 0;
    uint32_t resp_data_size = 0;

    /* 详细日志改为 verbose 级别控制，避免串口控制台过载导致 RCU stall */
    if (config.verbose > 2) {
        fprintf(stderr, "[proxy] process_request: type=%u seq=%u data_len=%u\n",
                req_hdr->type, req_hdr->seq, req_hdr->data_len);
    }

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    resp_hdr.type = req_hdr->type;
    resp_hdr.seq = req_hdr->seq;

    switch (req_hdr->type) {
    case POWERFS_MSG_LOOKUP: {
        struct powerfs_lookup_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_lookup((struct powerfs_lookup_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_GETATTR: {
        struct powerfs_getattr_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_getattr((struct powerfs_getattr_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_SETATTR: {
        struct powerfs_setattr_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_setattr((struct powerfs_setattr_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_MKDIR:
    case POWERFS_MSG_CREATE: {
        struct powerfs_create_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_create((struct powerfs_create_req *)req_data, resp,
                            req_hdr->type == POWERFS_MSG_MKDIR);
        break;
    }
    case POWERFS_MSG_UNLINK:
    case POWERFS_MSG_RMDIR: {
        ret = handle_remove((struct powerfs_remove_req *)req_data,
                            req_hdr->type == POWERFS_MSG_RMDIR);
        break;
    }
    case POWERFS_MSG_RENAME: {
        struct powerfs_rename_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_rename((struct powerfs_rename_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_READDIR: {
        struct powerfs_readdir_req *req = (struct powerfs_readdir_req *)req_data;
        uint32_t max_entries = req->max_entries;
        uint32_t out_count = 0;

        resp_data_size = sizeof(struct powerfs_dirent) * max_entries;
        resp_data = calloc(1, resp_data_size);

        ret = handle_readdir(req->dir_ino, req->offset, max_entries,
                             (struct powerfs_dirent *)resp_data, &out_count);

        resp_data_size = sizeof(struct powerfs_dirent) * out_count;
        break;
    }
    case POWERFS_MSG_SYMLINK: {
        struct powerfs_symlink_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_symlink((struct powerfs_symlink_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_LINK: {
        struct powerfs_link_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_link((struct powerfs_link_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_READLINK: {
        struct powerfs_readlink_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_readlink((struct powerfs_readlink_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_READ: {
        struct powerfs_read_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_read((struct powerfs_read_req *)req_data, resp);
        break;
    }
    case POWERFS_MSG_WRITE: {
        struct powerfs_write_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);

        struct powerfs_write_req *wreq = (struct powerfs_write_req *)req_data;
        uint8_t *write_data = (uint8_t *)(wreq + 1);

        ret = handle_write(wreq, resp, write_data);
        break;
    }
    case POWERFS_MSG_FSYNC: {
        struct powerfs_fsync_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = 0;
        break;
    }
    case POWERFS_MSG_TRUNCATE: {
        struct powerfs_truncate_resp *resp = calloc(1, sizeof(*resp));
        struct powerfs_truncate_req *treq = (struct powerfs_truncate_req *)req_data;
        struct inode_entry *ie = find_inode(treq->ino);

        resp_data = resp;
        resp_data_size = sizeof(*resp);

        if (!ie) {
            ret = -ENOENT;
        } else {
            ie->size = treq->size;
            resp->size = treq->size;
            ret = 0;
        }
        break;
    }
    case POWERFS_MSG_STATFS: {
        struct powerfs_statfs_resp *resp = calloc(1, sizeof(*resp));
        resp_data = resp;
        resp_data_size = sizeof(*resp);
        ret = handle_statfs(resp);
        break;
    }
    case POWERFS_MSG_PING: {
        ret = 0;
        break;
    }
    default:
        printf("[proxy] 未知消息类型: %u\n", req_hdr->type);
        ret = -EINVAL;
        break;
    }

    resp_hdr.status = ret;
    resp_hdr.data_len = resp_data_size;

    if (config.verbose > 1) {
        printf("[proxy] 处理请求 type=%u seq=%u -> status=%d\n",
               req_hdr->type, req_hdr->seq, ret);
    }

    /*
     * 对于异步通知类型的消息，内核提交请求后不等待响应。
     * 这些消息在代理处理完成后，不需要写回 CQ，避免 CQ 填满。
     * 包含: MKDIR, CREATE, UNLINK, RMDIR, RENAME, WRITE, SETATTR
     * 注意: SYMLINK, LINK 需要同步响应（内核使用 powerfs_comm_send_request 等待响应）
     *
     * 内核通过 dget(dentry) 锁定 dentry 保证一致性，不依赖代理同步响应
     */
    switch (req_hdr->type) {
    case POWERFS_MSG_MKDIR:
    case POWERFS_MSG_CREATE:
    case POWERFS_MSG_UNLINK:
    case POWERFS_MSG_RMDIR:
    case POWERFS_MSG_RENAME:
    case POWERFS_MSG_WRITE:
    case POWERFS_MSG_SETATTR:
        /* 异步通知类型: 不写回 CQ 响应，仅通知内核 */
        if (config.verbose > 2) {
            printf("[proxy] 异步通知 type=%u seq=%u: 跳过响应写回\n",
                   req_hdr->type, req_hdr->seq);
        }
        {
            uint64_t val = 1;
            ssize_t wr = write(g_comm_fd, &val, sizeof(val));
            (void)wr;
        }
        break;
    default:
        /* 需要响应的类型: 写回 CQ (包括 SYMLINK, LINK, LOOKUP, READ 等) */
        if (config.use_mmap) {
            submit_response(&resp_hdr, resp_data);
        } else {
            ioctl_submit_response(&resp_hdr, resp_data);
        }
        break;
    }

    if (resp_data)
        free(resp_data);
}

static void event_loop(void)
{
    time_t last_save = time(NULL);
    int save_interval = 30;  /* 每30秒保存一次 (减少频率提升吞吐量) */
    int consecutive_errors = 0;

    printf("[proxy] 开始事件循环 (模式: %s, poll 模式)...\n",
           config.use_mmap ? "MMAP" : "IOCTL");

    while (config.running) {
        struct powerfs_msg_header req_hdr;
        uint8_t req_buf[POWERFS_MAX_DATA_SIZE];
        uint32_t data_len;
        int ret;
        time_t now;

        /* 验证文件描述符有效性 */
        if (g_comm_fd < 0) {
            fprintf(stderr, "[proxy] 文件描述符无效，重新打开...\n");
            g_comm_fd = open_comm_device();
            if (g_comm_fd < 0) {
                fprintf(stderr, "[proxy] 重新打开设备失败\n");
                break;
            }
            /* 重新设置 MMAP */
            if (config.use_mmap) {
                if (setup_mmap() < 0) {
                    fprintf(stderr, "[proxy] 重新设置 MMAP 失败\n");
                    break;
                }
            }
        }

        /* 使用 poll 等待请求 (带超时) */
        {
            struct pollfd pfd;
            pfd.fd = g_comm_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            /* 100ms 超时，避免长时间阻塞 */
            ret = poll(&pfd, 1, 100);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                /* 有数据可读，获取所有可用请求 */
                while (1) {
                    data_len = 0;
                    if (config.use_mmap) {
                        ret = get_request(&req_hdr, req_buf, &data_len);
                    } else {
                        ret = ioctl_get_request(&req_hdr, req_buf, &data_len);
                    }

                    if (ret != 0)
                        break;

                    consecutive_errors = 0;
                    process_request(&req_hdr, req_buf);
                }
            } else if (ret < 0) {
                /* poll 错误 */
                consecutive_errors++;
                if (consecutive_errors > 100) {
                    fprintf(stderr, "[proxy] poll 持续失败，重新打开设备...\n");
                    close_comm_device();
                    g_comm_fd = open_comm_device();
                    if (g_comm_fd >= 0 && config.use_mmap) {
                        setup_mmap();
                    }
                    consecutive_errors = 0;
                }
            }
        }

        /* 周期性保存状态 */
        now = time(NULL);
        if (now - last_save >= save_interval) {
            save_state(POWERFS_DATA_FILE);
            last_save = now;
        }
    }

    printf("[proxy] 事件循环退出\n");
    
    /* 最终保存 */
    save_state(POWERFS_DATA_FILE);
}

static int open_comm_device(void)
{
    int fd;

    fd = open(config.comm_dev, O_RDWR);
    if (fd < 0) {
        perror("[proxy] 无法打开通信设备");
        return -1;
    }

    printf("[proxy] 已打开通信设备: %s\n", config.comm_dev);

    /* PING 测试 */
    if (ioctl(fd, POWERFS_IOCTL_PING, NULL) < 0) {
        perror("[proxy] PING 失败");
        close(fd);
        return -1;
    }
    printf("[proxy] PING 成功\n");

    /* 获取版本 */
    {
        uint32_t version;
        if (ioctl(fd, POWERFS_IOCTL_VERSION, &version) < 0) {
            perror("[proxy] 获取版本失败");
        } else {
            printf("[proxy] 协议版本: %u\n", version);
        }
    }

    return fd;
}

/*
 * close_comm_device - 关闭通信设备
 */
static void close_comm_device(void)
{
    if (g_comm_fd >= 0) {
        /* 解除 mmap */
        if (g_shm_base && g_shm_base != MAP_FAILED) {
            size_t shm_size;
            shm_size = POWERFS_SHM_SIZE_ALIGNED(POWERFS_MAX_REQUESTS, POWERFS_MAX_DATA_SIZE);
            munmap(g_shm_base, shm_size);
            g_shm_base = NULL;
            g_shm_hdr = NULL;
            g_sq_entries = NULL;
            g_cq_entries = NULL;
            g_data_area = NULL;
        }
        
        close(g_comm_fd);
        g_comm_fd = -1;
        printf("[proxy] 通信设备已关闭\n");
    }
}

/*
 * setup_mmap - 设置 mmap 共享内存
 * 重新初始化 mmap 映射 (用于设备重连)
 */
static int setup_mmap(void)
{
    int ret;
    
    /* 清理旧的 mmap */
    if (g_shm_base && g_shm_base != MAP_FAILED) {
        size_t shm_size;
        shm_size = POWERFS_SHM_SIZE_ALIGNED(POWERFS_MAX_REQUESTS, POWERFS_MAX_DATA_SIZE);
        munmap(g_shm_base, shm_size);
        g_shm_base = NULL;
        g_shm_hdr = NULL;
        g_sq_entries = NULL;
        g_cq_entries = NULL;
        g_data_area = NULL;
    }
    
    /* 重新初始化 mmap */
    if (g_comm_fd >= 0) {
        ret = init_mmap(g_comm_fd);
        if (ret < 0) {
            fprintf(stderr, "[proxy] mmap 重新初始化失败: %d\n", ret);
            return ret;
        }
        printf("[proxy] mmap 重新初始化成功\n");
    } else {
        fprintf(stderr, "[proxy] 文件描述符无效，无法初始化 mmap\n");
        return -EBADF;
    }
    
    return 0;
}

static void signal_handler(int sig)
{
    printf("\n[proxy] 收到信号 %d, 正在退出...\n", sig);
    config.running = 0;
}

int main(int argc, char *argv[])
{
    int opt;

    printf("========================================\n");
    printf("  PowerFS 用户态代理程序\n");
    printf("  Ceph-style Architecture\n");
    printf("========================================\n");
    printf("  通信设备: %s\n", config.comm_dev);
    printf("  Master:   %s:%d\n", config.master_addr, config.master_port);
    printf("  MMAP:     %s\n", config.use_mmap ? "启用" : "禁用");
    printf("========================================\n\n");

    /* 解析命令行参数 */
    while ((opt = getopt(argc, argv, "d:m:p:qvi")) != -1) {
        switch (opt) {
        case 'd':
            strncpy(config.comm_dev, optarg, sizeof(config.comm_dev) - 1);
            break;
        case 'm':
            strncpy(config.master_addr, optarg, sizeof(config.master_addr) - 1);
            break;
        case 'p':
            config.master_port = atoi(optarg);
            break;
        case 'q':
            config.verbose = 0;
            break;
        case 'v':
            config.verbose = 1;
            break;
        case 'i':
            config.use_mmap = 0;  /* 禁用 MMAP，使用 IOCTL 模式 */
            break;
        default:
            break;
        }
    }

    /* 初始化内存文件系统 */
    init_inodes();

    /* 尝试从持久化文件加载状态 */
    load_state(POWERFS_DATA_FILE);

    /* 打开通信设备 */
    g_comm_fd = open_comm_device();
    if (g_comm_fd < 0)
        return 1;

    /* mmap 共享内存 */
    if (config.use_mmap) {
        if (init_mmap(g_comm_fd) < 0) {
            printf("[proxy] mmap 初始化失败，降级到 ioctl 模式\n");
            config.use_mmap = 0;
        }
    }

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 进入事件循环 */
    event_loop();

    /* 保存状态到持久化文件 */
    save_state(POWERFS_DATA_FILE);

    /* 清理 */
    printf("[proxy] 正在清理...\n");
    if (g_shm_base && g_shm_base != MAP_FAILED) {
        munmap(g_shm_base, POWERFS_SHM_SIZE_ALIGNED(POWERFS_MAX_REQUESTS, POWERFS_MAX_DATA_SIZE));
    }
    if (g_comm_fd >= 0)
        close(g_comm_fd);

    printf("[proxy] 已退出\n");
    return 0;
}
