/*
 * PowerFS 内核模块 - 通信层
 *
 * 实现内核与用户态代理的通信
 *
 * 架构:
 *   字符设备 + ioctl + mmap 共享内存
 *
 * 功能:
 *   - ioctl: 同步 RPC 调用 (ping, version, request)
 *   - mmap:  共享内存环形队列 (SQ + CQ)
 *   - wait_queue: 阻塞等待响应
 *
 * 参考:
 *   - 类似 io_uring 的 SQ/CQ 环形队列设计
 *   - 字符设备 + ioctl 同步调用模式
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/poll.h>
#include <linux/delay.h>
#include <linux/dcache.h>
#include <linux/backing-dev.h>
#include <linux/pagemap.h>
#include <linux/list.h>

#include "powerfs.h"
#include "powerfs_comm.h"

/* 字符设备名称 */
#define POWERFS_COMM_DEV_NAME "powerfs_comm"

/* 主设备号 (动态分配) */
static dev_t powerfs_devt;
static struct cdev powerfs_cdev;
static struct class *powerfs_class;

/*
 * 通信设备私有数据
 *
 * 每个 open 的文件描述符有一个私有的通信上下文
 */
struct powerfs_comm_ctx {
    struct device *dev;

    /* 共享内存 */
    struct page *shm_page;        /* 分配的页面 */
    void *shm_base;               /* 内核虚拟地址 (kmapped) */
    size_t shm_size;
    unsigned long shm_order;      /* 页面阶数 */
    struct powerfs_shm_header *shm_hdr;
    struct powerfs_shm_entry *sq_entries;
    struct powerfs_shm_entry *cq_entries;
    void *data_area;

    /* 等待队列 */
    wait_queue_head_t waitq;          /* 用户态代理等待新请求 */
    wait_queue_head_t response_waitq; /* 内核等待响应 */

    /* 序列号 */
    atomic_t seq;

    /* 设备打开计数 */
    int opened;

    /* 锁 */
    spinlock_t lock;
};

/* 全局通信上下文 (目前只有一个) */
static struct powerfs_comm_ctx *g_comm_ctx = NULL;

/* 通信连接状态 (用户态代理已连接) */
static bool g_comm_connected = false;

/* ========== 共享内存操作 ========== */

/* 前向声明 */
static bool powerfs_shm_sq_full(struct powerfs_comm_ctx *ctx);
static bool powerfs_shm_sq_empty(struct powerfs_comm_ctx *ctx);
static bool powerfs_shm_cq_full(struct powerfs_comm_ctx *ctx);
static bool powerfs_shm_cq_empty(struct powerfs_comm_ctx *ctx);
static int powerfs_shm_submit_request(struct powerfs_comm_ctx *ctx,
                                      struct powerfs_msg_header *req_hdr,
                                      void *req_data);
static int powerfs_shm_get_request(struct powerfs_comm_ctx *ctx,
                                   struct powerfs_msg_header *req_hdr,
                                   void *req_data,
                                   int *req_data_len);
static int powerfs_shm_submit_response(struct powerfs_comm_ctx *ctx,
                                        struct powerfs_msg_header *resp_hdr,
                                        void *resp_data);
static int powerfs_shm_poll_response(struct powerfs_comm_ctx *ctx,
                                      u32 seq,
                                      struct powerfs_msg_header *resp_hdr,
                                      void *resp_data,
                                      int *resp_data_len);

/*
 * 计算共享内存大小 (页对齐)
 */
static size_t powerfs_shm_calc_size(void)
{
    return POWERFS_SHM_SIZE_ALIGNED(POWERFS_MAX_REQUESTS, POWERFS_MAX_DATA_SIZE);
}

/*
 * 初始化共享内存
 */
static int powerfs_shm_init(struct powerfs_comm_ctx *ctx)
{
    size_t size;
    int i;
    struct powerfs_shm_header *hdr;
    u8 *base;
    unsigned long order;

    size = powerfs_shm_calc_size();
    pr_info("powerfs: shm size = %zu bytes (%zu KB)\n", size, size / 1024);

    /* 计算需要的页面阶数 */
    order = get_order(size);
    ctx->shm_order = order;

    /* 分配连续物理页面 */
    ctx->shm_page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
    if (!ctx->shm_page) {
        pr_err("powerfs: failed to allocate shm pages (order=%lu)\n", order);
        return -ENOMEM;
    }

    /* 映射到内核虚拟地址 */
    ctx->shm_base = page_address(ctx->shm_page);
    if (!ctx->shm_base) {
        pr_err("powerfs: failed to map shm pages\n");
        __free_pages(ctx->shm_page, order);
        ctx->shm_page = NULL;
        return -ENOMEM;
    }

    ctx->shm_size = size;
    base = (u8 *)ctx->shm_base;

    /* 设置头部 */
    hdr = (struct powerfs_shm_header *)base;
    hdr->magic = POWERFS_COMM_MAGIC;
    hdr->version = POWERFS_COMM_VERSION;
    hdr->sq_head = 0;
    hdr->sq_tail = 0;
    hdr->cq_head = 0;
    hdr->cq_tail = 0;
    hdr->max_requests = POWERFS_MAX_REQUESTS;
    hdr->max_data_size = POWERFS_MAX_DATA_SIZE;

    /* SQ 条目区域 */
    ctx->shm_hdr = hdr;
    ctx->sq_entries = (struct powerfs_shm_entry *)(base + sizeof(*hdr));

    /* CQ 条目区域 */
    ctx->cq_entries = ctx->sq_entries + POWERFS_MAX_REQUESTS;

    /* 数据区 */
    ctx->data_area = (void *)(ctx->cq_entries + POWERFS_MAX_REQUESTS);
    hdr->data_offset = (u64)((u8 *)ctx->data_area - base);

    /* 初始化所有条目 */
    for (i = 0; i < POWERFS_MAX_REQUESTS; i++) {
        ctx->sq_entries[i].data_offset = i * POWERFS_MAX_DATA_SIZE;
        ctx->cq_entries[i].data_offset = i * POWERFS_MAX_DATA_SIZE;
    }

    pr_info("powerfs: shm initialized (sq=%p, cq=%p, data=%p)\n",
            ctx->sq_entries, ctx->cq_entries, ctx->data_area);

    return 0;
}

/*
 * 清理共享内存
 */
static void powerfs_shm_cleanup(struct powerfs_comm_ctx *ctx)
{
    if (ctx->shm_page) {
        __free_pages(ctx->shm_page, ctx->shm_order);
        ctx->shm_page = NULL;
    }
    ctx->shm_base = NULL;
}

/* ========== 字符设备 open/release ========== */

static int powerfs_comm_open(struct inode *inode, struct file *filp)
{
    struct powerfs_comm_ctx *ctx = g_comm_ctx;

    if (!ctx) {
        pr_err("powerfs: comm not initialized\n");
        return -ENODEV;
    }

    /* 只允许一个用户态代理打开 */
    spin_lock(&ctx->lock);
    if (ctx->opened) {
        spin_unlock(&ctx->lock);
        pr_warn("powerfs: device already opened\n");
        return -EBUSY;
    }
    ctx->opened = 1;
    spin_unlock(&ctx->lock);

    filp->private_data = ctx;

    /* 标记通信已连接 */
    g_comm_connected = true;

    pr_info("powerfs: comm device opened (connected)\n");
    return 0;
}

static int powerfs_comm_release(struct inode *inode, struct file *filp)
{
    struct powerfs_comm_ctx *ctx = filp->private_data;

    if (ctx) {
        spin_lock(&ctx->lock);
        ctx->opened = 0;
        spin_unlock(&ctx->lock);

        /* 唤醒所有等待者 */
        wake_up_all(&ctx->waitq);
        wake_up_all(&ctx->response_waitq);
    }

    /* 标记通信已断开 */
    g_comm_connected = false;

    pr_info("powerfs: comm device closed (disconnected)\n");
    return 0;
}

/* ========== 通信状态查询 ========== */

/**
 * powerfs_comm_is_connected - 检查通信层是否已连接
 *
 * 注意: 本地缓存模式下始终返回 false，禁用代理通信
 * 这避免了高并发下的锁竞争和 RCU stall
 */
bool powerfs_comm_is_connected(void)
{
    return false;  /* 本地缓存模式: 始终返回 false */
}

/* ========== INVALIDATE_NOTIFY 处理 ========== */

/**
 * powerfs_handle_invalidate - 处理主动失效通知
 *
 * 用户态代理通过 POWERFS_IOCTL_INVALIDATE ioctl 调用
 * 用于失效内核缓存中的 dentry/inode
 *
 * @req: 失效通知请求 (包含 flags 和 inode 号数组)
 *
 * 处理流程:
 *   1. 根据 flags 确定失效范围
 *   2. 遍历 ino 数组
 *   3. 从 inode 哈希表中查找对应 inode
 *   4. 失效页缓存 (如果是 inode 失效)
 *   5. 标记 dentry 为无效 (过期 lease)
 */
int powerfs_handle_invalidate(struct powerfs_invalidate_req *req)
{
    struct super_block *sb;
    u32 i;
    int ret = 0;

    if (!req || req->count == 0)
        return -EINVAL;

    pr_info("powerfs: invalidate notify (flags=0x%x, count=%u)\n",
            req->flags, req->count);

    /* 获取全局超级块 */
    sb = powerfs_get_sb();
    if (!sb) {
        pr_warn("powerfs: invalidate - no super_block available\n");
        return -ENODEV;
    }

    for (i = 0; i < req->count; i++) {
        u64 ino = req->ino[i];
        struct inode *inode;

        if (ino == 0)
            continue;

        pr_debug("powerfs: invalidate ino=%llu\n",
                 (unsigned long long)ino);

        /* 查找 inode (不创建新的) */
        inode = powerfs_find_inode(sb, ino);
        if (!inode) {
            pr_debug("powerfs: invalidate ino=%llu - inode not in cache\n",
                     (unsigned long long)ino);
            continue;
        }

        /*
         * 根据失效类型执行不同操作
         */
        if (req->flags & POWERFS_INVALIDATE_INODE) {
            /*
             * inode 失效: 清理页缓存
             *
             * 调用 invalidate_inode_pages2 释放所有脏页和写回
             * 这会确保下次读取时从服务端重新获取
             */
            pr_info("powerfs: invalidate inode %llu page cache\n",
                    (unsigned long long)ino);

            /* 同步写回脏页，然后失效所有页面 */
            invalidate_inode_pages2(inode->i_mapping);

            /* 标记 inode 缓存为无效，下次 lookup 会重新查询 */
            {
                struct powerfs_inode_info *pi = POWERFS_I(inode);
                spin_lock(&pi->i_lock);
                pi->cache_valid = false;
                pi->cache_expire = 0;
                spin_unlock(&pi->i_lock);
            }
        }

        if (req->flags & POWERFS_INVALIDATE_DENTRY) {
            /*
             * dentry 失效: 强制下次 lookup 重新查询
             *
             * 遍历 inode 的所有 dentry，标记 lease 为过期
             * 这样 d_revalidate 会返回 0，触发重新 lookup
             */
            pr_info("powerfs: invalidate dentry for ino %llu\n",
                    (unsigned long long)ino);

            /* 遍历 inode 的 dentry 链表，使所有关联的 dentry 失效 */
            if (!hlist_empty(&inode->i_dentry)) {
                struct dentry *dentry;
                struct hlist_node *tmp;

                hlist_for_each_entry_safe(dentry, tmp, &inode->i_dentry,
                                         d_u.d_alias) {
                    struct powerfs_dentry_info *di;

                    spin_lock(&dentry->d_lock);
                    di = dentry->d_fsdata;
                    if (di) {
                        /* 标记 lease 为已过期 (立即过期) */
                        di->lease_expire = jiffies - 1;
                        pr_debug("powerfs: invalidated dentry '%pd' (ino=%llu)\n",
                                 dentry, (unsigned long long)ino);
                    }
                    spin_unlock(&dentry->d_lock);
                }
            }
        }

        if (req->flags & POWERFS_INVALIDATE_DIR) {
            /*
             * 目录内容失效: 清除 dir_complete 标志
             *
             * 当服务端目录内容变更时 (新增/删除文件)，用户态代理
             * 发送此通知，内核收到后:
             *   1. 清除 inode 的 dir_complete 标志
             *   2. 标记该目录的所有子 dentry 为失效
             *   3. readdir 下次调用时会重新从服务端获取目录项
             */
            struct powerfs_inode_info *pi = POWERFS_I(inode);

            pr_info("powerfs: invalidate dir content for ino %llu\n",
                    (unsigned long long)ino);

            /* 清除 dir_complete 标志 */
            spin_lock(&pi->i_lock);
            pi->dir_complete = false;
            spin_unlock(&pi->i_lock);

            /* 标记该目录的子 dentry 为失效 (使 lookup 重新查询) */
            if (!hlist_empty(&inode->i_dentry)) {
                struct dentry *dentry;
                struct hlist_node *tmp;

                hlist_for_each_entry_safe(dentry, tmp, &inode->i_dentry,
                                         d_u.d_alias) {
                    struct powerfs_dentry_info *di;

                    spin_lock(&dentry->d_lock);
                    di = dentry->d_fsdata;
                    if (di) {
                        di->lease_expire = jiffies - 1;
                        pr_debug("powerfs: invalidated dir dentry '%pd'\n",
                                 dentry);
                    }
                    spin_unlock(&dentry->d_lock);
                }
            }
        }

        if (req->flags & POWERFS_INVALIDATE_ALL) {
            /* 全部失效: 清除该 inode 的所有缓存 */
            pr_info("powerfs: invalidate ALL for ino %llu\n",
                    (unsigned long long)ino);

            /* 失效页缓存 */
            invalidate_inode_pages2(inode->i_mapping);

            /* 标记 inode 缓存无效 */
            {
                struct powerfs_inode_info *pi = POWERFS_I(inode);
                spin_lock(&pi->i_lock);
                pi->cache_valid = false;
                pi->cache_expire = 0;
                pi->dir_complete = false;
                spin_unlock(&pi->i_lock);
            }

            /* 标记所有关联的 dentry 失效 */
            if (!hlist_empty(&inode->i_dentry)) {
                struct dentry *dentry;
                struct hlist_node *tmp;

                hlist_for_each_entry_safe(dentry, tmp, &inode->i_dentry,
                                         d_u.d_alias) {
                    struct powerfs_dentry_info *di;

                    spin_lock(&dentry->d_lock);
                    di = dentry->d_fsdata;
                    if (di) {
                        di->lease_expire = jiffies - 1;
                    }
                    spin_unlock(&dentry->d_lock);
                }
            }
        }

        iput(inode);  /* 释放 powerfs_find_inode 的引用 */
    }

    return ret;
}

/* ========== Ioctl 处理 ========== */

static long powerfs_comm_ioctl(struct file *filp, unsigned int cmd,
                                unsigned long arg)
{
    struct powerfs_comm_ctx *ctx = filp->private_data;
    struct powerfs_ioctl_req ioctl_req;
    void *kernel_data = NULL;
    int ret = 0;
    u32 version;

    if (!ctx)
        return -ENODEV;

    pr_debug("powerfs: ioctl cmd=0x%x\n", cmd);

    switch (cmd) {
    case POWERFS_IOCTL_PING:
        pr_info("powerfs: PING received\n");
        return 0;

    case POWERFS_IOCTL_VERSION:
        version = POWERFS_COMM_VERSION;
        if (copy_to_user((void __user *)arg, &version, sizeof(version)))
            return -EFAULT;
        return 0;

    case POWERFS_IOCTL_GET_REQ:
        /* 用户态代理获取请求 */
        if (copy_from_user(&ioctl_req, (void __user *)arg, sizeof(ioctl_req)))
            return -EFAULT;

        /* 分配内核缓冲区 */
        if (ioctl_req.data_size > 0 && ioctl_req.data_size <= POWERFS_MAX_DATA_SIZE) {
            kernel_data = kmalloc(ioctl_req.data_size, GFP_KERNEL);
            if (!kernel_data)
                return -ENOMEM;
        }

        spin_lock(&ctx->lock);
        ret = powerfs_shm_get_request(ctx, &ioctl_req.hdr,
                                       kernel_data,
                                       (int *)&ioctl_req.hdr.data_len);
        spin_unlock(&ctx->lock);

        if (ret >= 0) {
            /* 拷贝数据回用户空间 */
            if (kernel_data && ioctl_req.hdr.data_len > 0 && ioctl_req.data) {
                if (copy_to_user(ioctl_req.data, kernel_data, ioctl_req.hdr.data_len)) {
                    kfree(kernel_data);
                    return -EFAULT;
                }
            }
            if (copy_to_user((void __user *)arg, &ioctl_req, sizeof(ioctl_req))) {
                kfree(kernel_data);
                return -EFAULT;
            }
            ret = 0;
        }

        kfree(kernel_data);
        return ret;

    case POWERFS_IOCTL_SUBMIT_RESP:
        /* 用户态代理提交响应 */
        if (copy_from_user(&ioctl_req, (void __user *)arg, sizeof(ioctl_req)))
            return -EFAULT;

        /* 拷贝用户数据到内核 */
        if (ioctl_req.hdr.data_len > 0 && ioctl_req.hdr.data_len <= POWERFS_MAX_DATA_SIZE) {
            kernel_data = kmalloc(ioctl_req.hdr.data_len, GFP_KERNEL);
            if (!kernel_data)
                return -ENOMEM;
            if (copy_from_user(kernel_data, ioctl_req.data, ioctl_req.hdr.data_len)) {
                kfree(kernel_data);
                return -EFAULT;
            }
        }

        spin_lock(&ctx->lock);
        ret = powerfs_shm_submit_response(ctx, &ioctl_req.hdr, kernel_data);
        spin_unlock(&ctx->lock);

        kfree(kernel_data);

        if (ret >= 0) {
            /* 唤醒等待响应的内核线程 */
            wake_up_interruptible(&ctx->response_waitq);
            return 0;
        }
        return ret;

    case POWERFS_IOCTL_INVALIDATE: {
        /*
         * 用户态代理发送主动失效通知
         * 用于通知内核失效特定的 dentry/inode 缓存
         */
        struct powerfs_invalidate_req __user *user_req;
        struct powerfs_invalidate_req *kernel_req;
        u32 count;
        size_t req_size;

        user_req = (struct powerfs_invalidate_req __user *)arg;

        /* 首先获取 count 字段 */
        if (copy_from_user(&count, &user_req->count, sizeof(count)))
            return -EFAULT;

        if (count == 0 || count > 1024)  /* 限制最大数量 */
            return -EINVAL;

        /* 计算请求结构大小 (包含柔性数组) */
        req_size = sizeof(struct powerfs_invalidate_req) + count * sizeof(__u64);

        kernel_req = kmalloc(req_size, GFP_KERNEL);
        if (!kernel_req)
            return -ENOMEM;

        /* 拷贝整个请求结构 */
        if (copy_from_user(kernel_req, user_req, req_size)) {
            kfree(kernel_req);
            return -EFAULT;
        }

        pr_info("powerfs: ioctl INVALIDATE (flags=0x%x, count=%u)\n",
                kernel_req->flags, kernel_req->count);

        /* 处理失效通知 */
        ret = powerfs_handle_invalidate(kernel_req);

        kfree(kernel_req);
        return ret;
    }

    default:
        pr_debug("powerfs: unknown ioctl cmd=0x%x\n", cmd);
        return -ENOTTY;
    }

    return ret;
}

/* ========== mmap 处理 ========== */

static int powerfs_comm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    struct powerfs_comm_ctx *ctx = filp->private_data;
    unsigned long len = vma->vm_end - vma->vm_start;
    unsigned long pfn;
    int ret;

    if (!ctx)
        return -ENODEV;

    pr_info("powerfs: mmap request len=%lu shm_size=%zu\n", len, ctx->shm_size);

    /* 检查大小 */
    if (len > ctx->shm_size) {
        pr_warn("powerfs: mmap size too large (%lu > %zu)\n", len, ctx->shm_size);
        return -EINVAL;
    }

    /* 获取第一个页面的物理帧号 */
    pfn = page_to_pfn(ctx->shm_page);

    /* 映射物理页面到用户空间 */
    ret = remap_pfn_range(vma, vma->vm_start, pfn, len, vma->vm_page_prot);
    if (ret) {
        pr_err("powerfs: remap_pfn_range failed: %d\n", ret);
        return ret;
    }

    /* 设置 VMA 标志 */
    vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP | VM_LOCKED;

    pr_info("powerfs: mmap success\n");

    return 0;
}

/* ========== poll 处理 (用户态代理等待请求) ========== */

/*
 * powerfs_comm_poll - poll 系统调用实现
 *
 * 使用 poll_wait 注册等待队列，当 SQ 有新请求时通过
 * wake_up_interruptible 唤醒代理进程。
 */
static __poll_t powerfs_comm_poll(struct file *filp, struct poll_table_struct *wait)
{
    struct powerfs_comm_ctx *ctx;
    __poll_t mask = 0;

    /* 验证 filp 有效性 */
    if (!filp)
        return EPOLLERR;

    ctx = filp->private_data;
    if (!ctx)
        return EPOLLERR;

    /* 注册到等待队列，允许 poll 阻塞等待 */
    poll_wait(filp, &ctx->waitq, wait);

    /* 持有自旋锁检查状态 */
    spin_lock(&ctx->lock);
    if (!ctx->shm_hdr || !ctx->opened) {
        spin_unlock(&ctx->lock);
        return EPOLLERR;
    }

    /* 检查 SQ 是否有数据 */
    if (READ_ONCE(ctx->shm_hdr->sq_head) != READ_ONCE(ctx->shm_hdr->sq_tail)) {
        mask |= EPOLLIN | EPOLLRDNORM;
    }
    spin_unlock(&ctx->lock);

    return mask;
}

/* ========== 内核侧接口: 发送请求并等待响应 ========== */

/*
 * powerfs_shm_sq_full - 检查 SQ 队列是否满
 */
static bool powerfs_shm_sq_full(struct powerfs_comm_ctx *ctx)
{
    u32 head = READ_ONCE(ctx->shm_hdr->sq_head);
    u32 tail = READ_ONCE(ctx->shm_hdr->sq_tail);
    return ((head + 1) % POWERFS_MAX_REQUESTS) == tail;
}

/*
 * powerfs_shm_cq_empty - 检查 CQ 队列是否空
 */
static bool powerfs_shm_cq_empty(struct powerfs_comm_ctx *ctx)
{
    u32 head = READ_ONCE(ctx->shm_hdr->cq_head);
    u32 tail = READ_ONCE(ctx->shm_hdr->cq_tail);
    return head == tail;
}

/*
 * powerfs_shm_sq_empty - 检查 SQ 队列是否空
 */
static bool powerfs_shm_sq_empty(struct powerfs_comm_ctx *ctx)
{
    u32 head = READ_ONCE(ctx->shm_hdr->sq_head);
    u32 tail = READ_ONCE(ctx->shm_hdr->sq_tail);
    return head == tail;
}

/*
 * powerfs_shm_cq_full - 检查 CQ 队列是否满
 */
static bool powerfs_shm_cq_full(struct powerfs_comm_ctx *ctx)
{
    u32 head = READ_ONCE(ctx->shm_hdr->cq_head);
    u32 tail = READ_ONCE(ctx->shm_hdr->cq_tail);
    return ((tail + 1) % POWERFS_MAX_REQUESTS) == head;
}

/*
 * powerfs_shm_submit_request - 将请求提交到 SQ 队列
 *
 * 返回请求在 SQ 中的索引，失败返回负值
 */
static int powerfs_shm_submit_request(struct powerfs_comm_ctx *ctx,
                                    struct powerfs_msg_header *req_hdr,
                                    void *req_data)
{
    struct powerfs_shm_entry *entry;
    u32 head, idx;
    void *data_ptr;

    /* 检查数据大小限制 */
    if (req_hdr->data_len > POWERFS_MAX_DATA_SIZE) {
        pr_warn("powerfs: data too large (%u > %u)\n",
                req_hdr->data_len, POWERFS_MAX_DATA_SIZE);
        return -EFBIG;
    }

    /* 检查队列是否满 */
    if (powerfs_shm_sq_full(ctx)) {
        pr_warn("powerfs: SQ queue full\n");
        return -ENOSPC;
    }

    head = READ_ONCE(ctx->shm_hdr->sq_head);
    idx = head % POWERFS_MAX_REQUESTS;
    entry = &ctx->sq_entries[idx];

    /* 填充请求头部 */
    memcpy(&entry->hdr, req_hdr, sizeof(struct powerfs_msg_header));
    entry->data_len = req_hdr->data_len;

    /* 拷贝请求数据 */
    if (req_data && req_hdr->data_len > 0) {
        data_ptr = (u8 *)ctx->data_area + entry->data_offset;
        memcpy(data_ptr, req_data, req_hdr->data_len);
    }

    /* 更新 SQ 头指针 (release 语义) */
    smp_store_release(&ctx->shm_hdr->sq_head,
                       (head + 1) % POWERFS_MAX_REQUESTS);

    pr_debug("powerfs: submit req type=%u seq=%u idx=%u data_len=%u\n",
             req_hdr->type, req_hdr->seq, idx, req_hdr->data_len);

    return idx;
}

/*
 * powerfs_shm_poll_response - 从 CQ 队列获取响应
 *
 * 返回响应在 CQ 中的索引，失败返回负值
 * 支持跳过序列不匹配的过时响应
 */
static int powerfs_shm_poll_response(struct powerfs_comm_ctx *ctx,
                                     u32 seq,
                                     struct powerfs_msg_header *resp_hdr,
                                     void *resp_data,
                                     int *resp_data_len)
{
    struct powerfs_shm_entry *entry;
    u32 tail, head, idx;
    void *data_ptr;
    int max_scan;

    if (powerfs_shm_cq_empty(ctx))
        return -EAGAIN;

    tail = READ_ONCE(ctx->shm_hdr->cq_tail);
    head = READ_ONCE(ctx->shm_hdr->cq_head);

    /* 最多扫描整个 CQ，跳过过时条目 */
    max_scan = POWERFS_MAX_REQUESTS;

    while (max_scan-- > 0 && tail != head) {
        idx = tail % POWERFS_MAX_REQUESTS;
        entry = &ctx->cq_entries[idx];

        /* 跳过过时条目 (序列不匹配) */
        if (entry->hdr.seq != seq) {
            pr_debug("powerfs: cq skip stale seq=%u (want %u)\n",
                     entry->hdr.seq, seq);
            smp_store_release(&ctx->shm_hdr->cq_tail,
                               (tail + 1) % POWERFS_MAX_REQUESTS);
            tail = (tail + 1) % POWERFS_MAX_REQUESTS;
            continue;
        }

        /* 找到了匹配的响应 */
        /* 拷贝响应头部 */
        memcpy(resp_hdr, &entry->hdr, sizeof(struct powerfs_msg_header));

        /* 拷贝响应数据 */
        if (resp_data && entry->data_len > 0 &&
            entry->data_len <= POWERFS_MAX_DATA_SIZE) {
            data_ptr = (u8 *)ctx->data_area + entry->data_offset;
            memcpy(resp_data, data_ptr, entry->data_len);
            if (resp_data_len)
                *resp_data_len = entry->data_len;
        }

        /* 更新 CQ 尾指针 */
        smp_store_release(&ctx->shm_hdr->cq_tail,
                           (tail + 1) % POWERFS_MAX_REQUESTS);

        pr_debug("powerfs: poll resp type=%u seq=%u status=%d idx=%u\n",
                 resp_hdr->type, resp_hdr->seq, resp_hdr->status, idx);

        return idx;
    }

    /* 扫描完也没找到匹配的响应 */
    pr_debug("powerfs: cq no matching response for seq=%u\n", seq);
    return -EAGAIN;
}

/*
 * powerfs_shm_get_request - 用户态代理从 SQ 获取请求
 *
 * 返回索引，失败返回负值
 */
static int powerfs_shm_get_request(struct powerfs_comm_ctx *ctx,
                                    struct powerfs_msg_header *req_hdr,
                                    void *req_data,
                                    int *req_data_len)
{
    struct powerfs_shm_entry *entry;
    u32 tail, idx;
    void *data_ptr;

    if (powerfs_shm_sq_empty(ctx))
        return -EAGAIN;

    tail = READ_ONCE(ctx->shm_hdr->sq_tail);
    idx = tail % POWERFS_MAX_REQUESTS;
    entry = &ctx->sq_entries[idx];

    /* 拷贝请求头部 */
    memcpy(req_hdr, &entry->hdr, sizeof(struct powerfs_msg_header));

    /* 拷贝请求数据 */
    if (req_data && entry->data_len > 0 &&
        entry->data_len <= POWERFS_MAX_DATA_SIZE) {
        data_ptr = (u8 *)ctx->data_area + entry->data_offset;
        memcpy(req_data, data_ptr, entry->data_len);
        if (req_data_len)
            *req_data_len = entry->data_len;
    }

    /* 更新 SQ 尾指针 (消费请求) */
    smp_store_release(&ctx->shm_hdr->sq_tail,
                       (tail + 1) % POWERFS_MAX_REQUESTS);

    pr_debug("powerfs: get req type=%u seq=%u idx=%u\n",
             req_hdr->type, req_hdr->seq, idx);

    return idx;
}

/*
 * powerfs_shm_submit_response - 用户态代理向 CQ 提交响应
 *
 * 返回索引，失败返回负值
 */
static int powerfs_shm_submit_response(struct powerfs_comm_ctx *ctx,
                                        struct powerfs_msg_header *resp_hdr,
                                        void *resp_data)
{
    struct powerfs_shm_entry *entry;
    u32 tail, idx;
    void *data_ptr;

    if (powerfs_shm_cq_full(ctx)) {
        pr_warn("powerfs: CQ queue full\n");
        return -ENOSPC;
    }

    tail = READ_ONCE(ctx->shm_hdr->cq_tail);
    idx = tail % POWERFS_MAX_REQUESTS;
    entry = &ctx->cq_entries[idx];

    /* 填充响应头部 */
    memcpy(&entry->hdr, resp_hdr, sizeof(struct powerfs_msg_header));
    entry->data_len = resp_hdr->data_len;

    /* 拷贝响应数据 */
    if (resp_data && resp_hdr->data_len > 0 &&
        resp_hdr->data_len <= POWERFS_MAX_DATA_SIZE) {
        data_ptr = (u8 *)ctx->data_area + entry->data_offset;
        memcpy(data_ptr, resp_data, resp_hdr->data_len);
    }

    /* 更新 CQ 头指针 (produce 响应) */
    smp_store_release(&ctx->shm_hdr->cq_head,
                       (tail + 1) % POWERFS_MAX_REQUESTS);

    pr_debug("powerfs: submit resp type=%u seq=%u status=%d idx=%u\n",
             resp_hdr->type, resp_hdr->seq, resp_hdr->status, idx);

    return idx;
}

/* 前向声明 */
static void powerfs_shm_drain_cq(struct powerfs_comm_ctx *ctx);

/*
 * powerfs_comm_send_request - 发送同步请求并等待响应
 *
 * 流程:
 *   1. 分配序列号
 *   2. 将请求写入 SQ
 *   3. 唤醒用户态代理
 *   4. 阻塞等待 CQ 中的响应 (带超时)
 *   5. 从 CQ 读取响应并返回
 */
int powerfs_comm_send_request(struct powerfs_msg_header *req_hdr,
                               void *req_data,
                               struct powerfs_msg_header *resp_hdr,
                               void *resp_data,
                               int timeout_ms)
{
    struct powerfs_comm_ctx *ctx = g_comm_ctx;
    u32 seq;
    int ret;

    if (!ctx || !ctx->opened) {
        pr_debug("powerfs: comm not connected\n");
        return -ENOTCONN;
    }

    /* 分配序列号 */
    seq = atomic_inc_return(&ctx->seq);
    req_hdr->seq = seq;

    pr_debug("powerfs: send_request type=%u seq=%u data_len=%u timeout=%dms\n",
             req_hdr->type, seq, req_hdr->data_len, timeout_ms);

    /*
     * 非阻塞模式: 仅提交请求，不等待响应
     *
     * 原因:
     *   1. 高并发下同步等待会导致 SQ 队列满 + RCU stall
     *   2. 元数据操作在本地缓存模式下不需要等待代理响应
     *   3. 如果 SQ 满，立即返回错误，调用者回退到本地模式
     *
     * 注意: 不在自旋锁内做额外操作，减少锁竞争
     */
    spin_lock(&ctx->lock);
    ret = powerfs_shm_submit_request(ctx, req_hdr, req_data);
    spin_unlock(&ctx->lock);

    if (ret < 0) {
        pr_debug("powerfs: submit request failed (seq=%u): %d\n", seq, ret);
        return ret;
    }

    /* 唤醒用户态代理处理请求 */
    wake_up_interruptible(&ctx->waitq);

    /* 立即返回成功，不等待响应 */
    pr_debug("powerfs: request submitted seq=%u (no wait)\n", seq);
    return 0;
}

/*
 * powerfs_shm_drain_cq - 清空 CQ 中所有待处理的响应
 *
 * 用于异步通知场景: 内核提交请求后不等待响应
 * 但代理仍会写入响应，如果不清理 CQ 会导致其填满
 */
static void powerfs_shm_drain_cq(struct powerfs_comm_ctx *ctx)
{
    u32 head, tail;
    int count = 0;

    while (!powerfs_shm_cq_empty(ctx)) {
        tail = READ_ONCE(ctx->shm_hdr->cq_tail);
        head = READ_ONCE(ctx->shm_hdr->cq_head);

        /* 没有新响应 */
        if (tail == head)
            break;

        /* 跳过一个响应 */
        smp_store_release(&ctx->shm_hdr->cq_tail,
                          (tail + 1) % POWERFS_MAX_REQUESTS);
        count++;
    }

    if (count > 0)
        pr_debug("powerfs: drained %d CQ entries\n", count);
}

/*
 * powerfs_comm_submit_notify - 异步提交通知请求 (非阻塞，不等待响应)
 *
 * 用于删除、重命名等操作，不需要等待代理确认。
 * 这些操作在本地修改完成后，仅需通知代理更新后端状态。
 *
 * 策略:
 *   - 非阻塞: SQ 满时立即返回，不重试
 *   - 尽力而为: 通知失败不阻塞主路径，避免 RCU stall
 *   - 后续阶段可通过批量通知或重试机制改进
 *
 * 流程:
 *   1. 分配序列号
 *   2. 尝试清空 CQ 中积压的响应
 *   3. 尝试将请求写入 SQ (仅一次，不重试)
 *   4. 唤醒用户态代理
 *   5. 立即返回
 */
int powerfs_comm_submit_notify(struct powerfs_msg_header *req_hdr,
                               void *req_data)
{
    struct powerfs_comm_ctx *ctx = g_comm_ctx;
    u32 seq;
    int ret;

    if (!ctx || !ctx->opened) {
        pr_debug("powerfs: comm not connected, notify skipped\n");
        return -ENOTCONN;
    }

    /* 分配序列号 */
    seq = atomic_inc_return(&ctx->seq);
    req_hdr->seq = seq;

    pr_debug("powerfs: submit_notify type=%u seq=%u data_len=%u\n",
             req_hdr->type, seq, req_hdr->data_len);

    /* 非阻塞: 仅尝试一次，SQ 满则放弃 */
    spin_lock(&ctx->lock);
    ret = powerfs_shm_submit_request(ctx, req_hdr, req_data);
    spin_unlock(&ctx->lock);

    if (ret < 0) {
        /* 通知失败不阻塞，直接返回 */
        pr_debug("powerfs: notify failed (type=%u seq=%u): %d\n",
                 req_hdr->type, seq, ret);
        return ret;
    }

    /* 唤醒用户态代理处理请求 */
    wake_up_interruptible(&ctx->waitq);

    /* 立即返回，不等待响应 */
    return 0;
}

/* ========== write 处理 (用户态代理通知内核有响应) ========== */

/*
 * powerfs_comm_write - 用户态代理通知内核响应已就绪
 *
 * MMAP 模式下，代理程序直接在用户态更新 CQ 队列
 * 然后通过 write 系统调用通知内核有新的响应可用
 */
static ssize_t powerfs_comm_write(struct file *filp, const char __user *buf,
                                   size_t count, loff_t *ppos)
{
    struct powerfs_comm_ctx *ctx = filp->private_data;

    if (!ctx)
        return -ENODEV;

    pr_debug("powerfs: write received (%zu bytes), waking up response_waitq\n", count);

    /* 唤醒等待响应的内核线程 */
    wake_up_interruptible(&ctx->response_waitq);

    return count;
}

/* ========== 字符设备操作表 ========== */

static const struct file_operations powerfs_comm_fops = {
    .owner          = THIS_MODULE,
    .open           = powerfs_comm_open,
    .release        = powerfs_comm_release,
    .unlocked_ioctl = powerfs_comm_ioctl,
    .mmap           = powerfs_comm_mmap,
    .poll           = powerfs_comm_poll,
    .write          = powerfs_comm_write,
};

/* ========== 初始化/清理 ========== */

/*
 * powerfs_comm_init - 初始化通信设备
 */
int powerfs_comm_init(void)
{
    int ret;
    struct device *dev;
    struct powerfs_comm_ctx *ctx;

    pr_info("powerfs: initializing comm device...\n");

    /* 分配全局上下文 */
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    g_comm_ctx = ctx;
    spin_lock_init(&ctx->lock);
    init_waitqueue_head(&ctx->waitq);
    init_waitqueue_head(&ctx->response_waitq);
    atomic_set(&ctx->seq, 0);

    /* 初始化共享内存 */
    ret = powerfs_shm_init(ctx);
    if (ret) {
        pr_err("powerfs: shm init failed: %d\n", ret);
        kfree(ctx);
        g_comm_ctx = NULL;
        return ret;
    }

    /* 动态分配主设备号 */
    ret = alloc_chrdev_region(&powerfs_devt, 0, 1, POWERFS_COMM_DEV_NAME);
    if (ret < 0) {
        pr_warn("powerfs: alloc_chrdev_region failed: %d\n", ret);
        powerfs_shm_cleanup(ctx);
        kfree(ctx);
        g_comm_ctx = NULL;
        return ret;
    }

    /* 初始化 cdev */
    cdev_init(&powerfs_cdev, &powerfs_comm_fops);
    powerfs_cdev.owner = THIS_MODULE;

    ret = cdev_add(&powerfs_cdev, powerfs_devt, 1);
    if (ret < 0) {
        pr_warn("powerfs: cdev_add failed: %d\n", ret);
        unregister_chrdev_region(powerfs_devt, 1);
        powerfs_shm_cleanup(ctx);
        kfree(ctx);
        g_comm_ctx = NULL;
        return ret;
    }

    /* 创建设备类 */
    powerfs_class = class_create(THIS_MODULE, POWERFS_COMM_DEV_NAME);
    if (IS_ERR(powerfs_class)) {
        pr_warn("powerfs: class_create failed\n");
        cdev_del(&powerfs_cdev);
        unregister_chrdev_region(powerfs_devt, 1);
        powerfs_shm_cleanup(ctx);
        kfree(ctx);
        g_comm_ctx = NULL;
        return PTR_ERR(powerfs_class);
    }

    dev = device_create(powerfs_class, NULL, powerfs_devt, NULL,
                        POWERFS_COMM_DEV_NAME);
    if (IS_ERR(dev)) {
        pr_warn("powerfs: device_create failed\n");
        class_destroy(powerfs_class);
        cdev_del(&powerfs_cdev);
        unregister_chrdev_region(powerfs_devt, 1);
        powerfs_shm_cleanup(ctx);
        kfree(ctx);
        g_comm_ctx = NULL;
        return PTR_ERR(dev);
    }

    ctx->dev = dev;

    pr_info("powerfs: comm device created (%s, major=%d)\n",
            POWERFS_COMM_DEV_NAME, MAJOR(powerfs_devt));
    pr_info("powerfs:   shm size: %zu bytes (%zu KB)\n",
            ctx->shm_size, ctx->shm_size / 1024);
    pr_info("powerfs:   max requests: %d\n", POWERFS_MAX_REQUESTS);
    pr_info("powerfs:   max data size: %d bytes\n", POWERFS_MAX_DATA_SIZE);

    return 0;
}

/*
 * powerfs_comm_exit - 销毁通信设备
 */
void powerfs_comm_exit(void)
{
    pr_info("powerfs: destroying comm device...\n");

    if (g_comm_ctx) {
        /* 唤醒所有等待者 */
        wake_up_all(&g_comm_ctx->waitq);
        wake_up_all(&g_comm_ctx->response_waitq);

        /* 清理共享内存 */
        powerfs_shm_cleanup(g_comm_ctx);

        kfree(g_comm_ctx);
        g_comm_ctx = NULL;
    }

    if (powerfs_class) {
        device_destroy(powerfs_class, powerfs_devt);
        class_destroy(powerfs_class);
        powerfs_class = NULL;
    }

    cdev_del(&powerfs_cdev);
    unregister_chrdev_region(powerfs_devt, 1);

    pr_info("powerfs: comm device destroyed\n");
}
