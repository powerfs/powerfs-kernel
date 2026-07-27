/*
 * PowerFS 内核模块 - 通信层
 *
 * 重构: 直接使用 powerfs-net TCP 协议与 Filer 通信
 *
 * 架构:
 *   VFS 回调 -> powerfs_comm_* -> powerfs_net_* (TCP + TLV) -> Filer
 *
 * 功能:
 *   - 提供与旧 API 兼容的接口
 *   - 内部将 powerfs_msg_type 映射到 powerfs-net MsgType
 *   - 负责请求/响应数据的编解码
 *
 * 注意: 字符设备 /dev/powerfs_comm 已不再需要
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/jiffies.h>

#include "powerfs.h"
#include "powerfs_comm.h"
#include "powerfs_net.h"

/* ========== 消息类型映射 ========== */

/**
 * powerfs_msg_type_to_net - 将内核消息类型转换为 powerfs-net MsgType
 */
static __u16 powerfs_msg_type_to_net(__u32 msg_type)
{
    switch (msg_type) {
    case POWERFS_MSG_PING:      return POWERFS_NET_MSG_PING;
    case POWERFS_MSG_LOOKUP:    return POWERFS_NET_MSG_LOOKUP;
    case POWERFS_MSG_GETATTR:   return POWERFS_NET_MSG_GETATTR;
    case POWERFS_MSG_SETATTR:   return POWERFS_NET_MSG_SETATTR;
    case POWERFS_MSG_MKDIR:     return POWERFS_NET_MSG_MKDIR;
    case POWERFS_MSG_CREATE:    return POWERFS_NET_MSG_CREATE;
    case POWERFS_MSG_UNLINK:    return POWERFS_NET_MSG_UNLINK;
    case POWERFS_MSG_RMDIR:     return POWERFS_NET_MSG_RMDIR;
    case POWERFS_MSG_RENAME:    return POWERFS_NET_MSG_RENAME;
    case POWERFS_MSG_READDIR:   return POWERFS_NET_MSG_READDIR;
    case POWERFS_MSG_SYMLINK:   return POWERFS_NET_MSG_SYMLINK;
    case POWERFS_MSG_READLINK:  return POWERFS_NET_MSG_READLINK;
    case POWERFS_MSG_LINK:      return POWERFS_NET_MSG_LINK;
    case POWERFS_MSG_READ:      return POWERFS_NET_MSG_READ;
    case POWERFS_MSG_WRITE:     return POWERFS_NET_MSG_WRITE;
    case POWERFS_MSG_FSYNC:     return POWERFS_NET_MSG_PING; /* 复用 PING 表示同步 */
    case POWERFS_MSG_STATFS:    return POWERFS_NET_MSG_STATFS;
    default:                    return 0; /* 未知类型 */
    }
}

/**
 * powerfs_net_status_to_errno - 将 powerfs-net 状态码转换为 errno
 */
static int powerfs_net_status_to_errno(__u16 status)
{
    switch (status) {
    case POWERFS_NET_STATUS_OK:              return 0;
    case POWERFS_NET_STATUS_ERR_NOT_FOUND:   return -ENOENT;
    case POWERFS_NET_STATUS_ERR_ALREADY_EXISTS: return -EEXIST;
    case POWERFS_NET_STATUS_ERR_PERMISSION:  return -EPERM;
    case POWERFS_NET_STATUS_ERR_IO:          return -EIO;
    case POWERFS_NET_STATUS_ERR_INVALID_ARG: return -EINVAL;
    case POWERFS_NET_STATUS_ERR_NOT_DIR:     return -ENOTDIR;
    case POWERFS_NET_STATUS_ERR_IS_DIR:      return -EISDIR;
    case POWERFS_NET_STATUS_ERR_NO_SPACE:    return -ENOSPC;
    case POWERFS_NET_STATUS_ERR_BAD_FD:      return -EBADF;
    case POWERFS_NET_STATUS_ERR_SERVER:      return -EREMOTEIO;
    default:                                 return -EIO;
    }
}

/* ========== 数据编码/解码辅助 ========== */

/**
 * encode_kernel_request - 将内核请求数据编码为 TLV body
 *
 * @msg_type: 内核消息类型
 * @req_data: 内核请求数据 (powerfs_*_req 结构)
 * @req_hdr: 内核消息头 (包含 ino, data_len 等)
 * @body_out: 输出 TLV body 缓冲区
 * @body_cap: body 缓冲区容量
 * @body_len_out: 输出 body 长度
 *
 * 返回: 0 成功, 负值失败
 */
static int encode_kernel_request(__u32 msg_type, void *req_data,
                                  struct powerfs_msg_header *req_hdr,
                                  __u8 *body_out, size_t body_cap,
                                  size_t *body_len_out)
{
    struct powerfs_tlv_enc enc;

    powerfs_tlv_enc_init(&enc, body_out, body_cap);

    switch (msg_type) {
    case POWERFS_MSG_LOOKUP: {
        struct powerfs_lookup_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, req->dir_ino);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                                req->name, strlen(req->name));
        break;
    }

    case POWERFS_MSG_GETATTR: {
        struct powerfs_getattr_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, req->ino);
        break;
    }

    case POWERFS_MSG_SETATTR: {
        struct powerfs_setattr_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, req->ino);
        if (req->ia_valid & POWERFS_ATTR_MODE)
            powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_MODE, req->mode);
        if (req->ia_valid & POWERFS_ATTR_UID)
            powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_UID, req->uid);
        if (req->ia_valid & POWERFS_ATTR_GID)
            powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_GID, req->gid);
        if (req->ia_valid & POWERFS_ATTR_SIZE)
            powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_SIZE, req->size);
        if (req->ia_valid & POWERFS_ATTR_MTIME)
            powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_MTIME, req->mtime_sec);
        if (req->ia_valid & POWERFS_ATTR_ATIME)
            powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_ATIME, req->atime_sec);
        if (req->ia_valid & POWERFS_ATTR_CTIME)
            powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_CTIME, req->ctime_sec);
        break;
    }

    case POWERFS_MSG_CREATE:
    case POWERFS_MSG_MKDIR: {
        struct powerfs_create_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, req->dir_ino);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                                req->name, strlen(req->name));
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_MODE, req->mode);
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_UID, req->uid);
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_GID, req->gid);
        powerfs_tlv_enc_u8(&enc, POWERFS_NET_FLD_IS_DIR,
                            (msg_type == POWERFS_MSG_MKDIR) ? 1 : 0);
        break;
    }

    case POWERFS_MSG_UNLINK:
    case POWERFS_MSG_RMDIR: {
        struct powerfs_remove_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, req->dir_ino);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                                req->name, strlen(req->name));
        break;
    }

    case POWERFS_MSG_RENAME: {
        struct powerfs_rename_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, req->old_dir_ino);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                                req->old_name, req->old_name_len);
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_NEW_PARENT_INO,
                            req->new_dir_ino);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NEW_NAME,
                                req->new_name, req->new_name_len);
        break;
    }

    case POWERFS_MSG_READDIR: {
        struct powerfs_readdir_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, req->dir_ino);
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, req->offset);
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_COUNT, req->max_entries);
        break;
    }

    case POWERFS_MSG_SYMLINK: {
        struct powerfs_symlink_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, req->dir_ino);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                                req->name, req->name_len);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_SYMLINK_TARGET,
                                req->symname, req->symname_len);
        break;
    }

    case POWERFS_MSG_READLINK: {
        struct powerfs_readlink_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, req->ino);
        break;
    }

    case POWERFS_MSG_LINK: {
        struct powerfs_link_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, req->ino);
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_PARENT_INO, req->dir_ino);
        powerfs_tlv_enc_string(&enc, POWERFS_NET_FLD_NAME,
                                req->name, strlen(req->name));
        break;
    }

    case POWERFS_MSG_READ: {
        struct powerfs_read_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, req->ino);
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, req->offset);
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_DATA_LEN, req->length);
        break;
    }

    case POWERFS_MSG_WRITE: {
        struct powerfs_write_req *req = req_data;

        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_INO, req->ino);
        powerfs_tlv_enc_u64(&enc, POWERFS_NET_FLD_OFFSET, req->offset);
        powerfs_tlv_enc_u32(&enc, POWERFS_NET_FLD_DATA_LEN, req->length);
        break;
    }

    case POWERFS_MSG_STATFS:
    case POWERFS_MSG_FSYNC:
    case POWERFS_MSG_PING:
        /* 这些请求没有 body */
        break;

    default:
        pr_err("powerfs: unknown msg type %u\n", msg_type);
        return -EINVAL;
    }

    *body_len_out = powerfs_tlv_enc_len(&enc);
    return 0;
}

/**
 * decode_kernel_response - 将 TLV 响应数据解码为内核响应结构
 *
 * @msg_type: 内核消息类型
 * @resp_body: TLV 响应 body
 * @resp_body_len: body 长度
 * @resp_data: 响应 data (如读数据)
 * @resp_data_len: data 长度
 * @resp_hdr_out: 输出内核响应头 (包含 status, ino 等)
 * @resp_struct_out: 输出内核响应结构
 *
 * 返回: 0 成功, 负值失败
 */
static int decode_kernel_response(__u32 msg_type,
                                    const __u8 *resp_body, size_t resp_body_len,
                                    const __u8 *resp_data, size_t resp_data_len,
                                    struct powerfs_msg_header *resp_hdr_out,
                                    void *resp_struct_out)
{
    struct powerfs_tlv_dec dec;

    /* 响应 body 为空表示成功 (对于无数据响应) */
    if (resp_body_len == 0)
        return 0;

    powerfs_tlv_dec_init(&dec, resp_body, resp_body_len);

    switch (msg_type) {
    case POWERFS_MSG_LOOKUP:
    case POWERFS_MSG_CREATE:
    case POWERFS_MSG_MKDIR:
    case POWERFS_MSG_SYMLINK: {
        /*
         * 这些操作返回 EntryInfo 结构
         * 统一解码为通用的 inode 属性
         */
        __u64 ino = 0;
        __u32 mode = 0, uid = 0, gid = 0, nlink = 0;
        __u64 size = 0, mtime = 0;

        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_INO, &ino);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_MODE, &mode);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_UID, &uid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_GID, &gid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_NLINK, &nlink);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, &size);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_MTIME, &mtime);

        /* 根据消息类型填充不同的响应结构 */
        switch (msg_type) {
        case POWERFS_MSG_LOOKUP:
        case POWERFS_MSG_SYMLINK: {
            struct powerfs_lookup_resp *resp = resp_struct_out;
            resp->ino = ino;
            resp->mode = mode;
            resp->uid = uid;
            resp->gid = gid;
            resp->nlink = nlink;
            resp->size = size;
            resp->mtime_sec = mtime;
            break;
        }
        case POWERFS_MSG_CREATE:
        case POWERFS_MSG_MKDIR: {
            struct powerfs_create_resp *resp = resp_struct_out;
            resp->ino = ino;
            resp->mode = mode;
            resp->uid = uid;
            resp->gid = gid;
            resp->nlink = nlink;
            resp->size = size;
            resp->mtime_sec = mtime;
            break;
        }
        }

        if (resp_hdr_out)
            resp_hdr_out->ino = ino;
        break;
    }

    case POWERFS_MSG_GETATTR: {
        struct powerfs_getattr_resp *resp = resp_struct_out;

        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_MODE, &resp->mode);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_NLINK, &resp->nlink);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_UID, &resp->uid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_GID, &resp->gid);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, &resp->size);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_ATIME, &resp->atime_sec);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_MTIME, &resp->mtime_sec);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_CTIME, &resp->ctime_sec);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_BLOCKS, &resp->blocks);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_BLKSIZE, &resp->blksize);
        break;
    }

    case POWERFS_MSG_SETATTR:
    case POWERFS_MSG_TRUNCATE: {
        struct powerfs_setattr_resp *resp = resp_struct_out;

        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_MODE, &resp->mode);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_NLINK, &resp->nlink);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_UID, &resp->uid);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_GID, &resp->gid);
        powerfs_tlv_dec_u64(&dec, POWERFS_NET_FLD_SIZE, &resp->size);
        break;
    }

    case POWERFS_MSG_READDIR: {
        /*
         * readdir 响应包含 TLV 编码的目录项数组
         * 数据直接放在 resp_data 中返回
         */
        if (resp_hdr_out && resp_data && resp_data_len > 0) {
            /* 通过 hdr->data_len 告知调用者数据位置 */
            resp_hdr_out->data_len = resp_data_len;
        }
        break;
    }

    case POWERFS_MSG_READ: {
        /* 读数据在 resp_data 中 */
        if (resp_hdr_out) {
            resp_hdr_out->data_len = resp_data_len;
        }
        break;
    }

    case POWERFS_MSG_WRITE: {
        struct powerfs_write_resp *resp = resp_struct_out;
        __u32 written = 0;

        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_DATA_LEN, &written);
        if (resp)
            resp->written = written;
        break;
    }

    case POWERFS_MSG_STATFS: {
        /* statfs 响应直接在 body 中 */
        if (resp_struct_out && resp_body_len >= sizeof(struct kstatfs)) {
            memcpy(resp_struct_out, resp_body, sizeof(struct kstatfs));
        }
        break;
    }

    case POWERFS_MSG_READLINK: {
        struct powerfs_readlink_resp *resp = resp_struct_out;

        powerfs_tlv_dec_string(&dec, POWERFS_NET_FLD_SYMLINK_TARGET,
                                resp->target, sizeof(resp->target));
        resp->len = strlen(resp->target);
        break;
    }

    case POWERFS_MSG_UNLINK:
    case POWERFS_MSG_RMDIR:
    case POWERFS_MSG_RENAME:
    case POWERFS_MSG_LINK:
    case POWERFS_MSG_FSYNC:
    case POWERFS_MSG_PING:
        /* 这些操作无响应数据 */
        break;

    default:
        pr_debug("powerfs: decode_resp: msg_type=%u body_len=%zu\n",
                 msg_type, resp_body_len);
        break;
    }

    return 0;
}

/* ========== 传输层公共 API ========== */

static bool g_comm_connected = false;

/**
 * powerfs_comm_init - 初始化通信层
 */
int powerfs_comm_init(void)
{
    int ret;

    /* 初始化 powerfs-net 子系统 (内部会尝试连接服务器) */
    ret = powerfs_net_init();
    if (ret < 0) {
        pr_err("powerfs: net init failed: %d\n", ret);
        return ret;
    }

    /* 检查连接状态 (powerfs_net_init 内部已尝试连接) */
    g_comm_connected = powerfs_net_is_connected();
    if (g_comm_connected)
        pr_info("powerfs: connected to backend server\n");
    else
        pr_warn("powerfs: not connected yet, will retry\n");

    pr_info("powerfs: comm layer initialized (net mode)\n");
    return 0;
}

/**
 * powerfs_comm_exit - 清理通信层
 */
void powerfs_comm_exit(void)
{
    g_comm_connected = false;
    powerfs_net_exit();
    pr_info("powerfs: comm layer exited\n");
}

/**
 * powerfs_comm_connect - 建立到 Filer 的连接
 */
int powerfs_comm_connect(const char *addr, __u16 port)
{
    int ret;

    ret = powerfs_net_connect(addr, port);
    if (ret == 0)
        g_comm_connected = true;

    return ret;
}

/**
 * powerfs_comm_disconnect - 断开连接
 */
void powerfs_comm_disconnect(void)
{
    g_comm_connected = false;
    powerfs_net_disconnect();
}

/**
 * powerfs_comm_is_connected - 检查连接状态
 */
bool powerfs_comm_is_connected(void)
{
    return g_comm_connected && powerfs_net_is_connected();
}

/**
 * powerfs_comm_send_request - 发送同步请求并等待响应
 *
 * @req_hdr: 请求头 (包含 type, ino, data_len, seq)
 * @req_data: 请求数据 (powerfs_*_req 结构)
 * @resp_hdr: 输出响应头
 * @resp_data: 输出响应数据 (powerfs_*_resp 结构)
 * @timeout_ms: 超时毫秒数
 *
 * 返回: 0 成功, 负值 errno
 */
int powerfs_comm_send_request(struct powerfs_msg_header *req_hdr,
                               void *req_data,
                               struct powerfs_msg_header *resp_hdr,
                               void *resp_data,
                               int timeout_ms)
{
    __u16 net_msg_type;
    __u8 *body;           /* TLV body 缓冲区 */
    size_t body_len = 0;
    __u8 *resp_body;      /* TLV 响应 body */
    size_t resp_body_len = 0;
    __u8 *resp_data_buf;  /* 响应数据 (用于 read/write) */
    size_t resp_data_len = 0;
    int ret;
    int net_status;

    /* 检查连接状态 */
    if (!powerfs_net_is_connected()) {
        pr_debug("powerfs: not connected, request type=%u\n", req_hdr->type);
        return -ENOTCONN;
    }

    /* 分配缓冲区 (使用动态分配以避免栈溢出) */
    body = kmalloc(1024, GFP_KERNEL);
    resp_body = kmalloc(1024, GFP_KERNEL);
    resp_data_buf = kmalloc(65536, GFP_KERNEL);
    if (!body || !resp_body || !resp_data_buf) {
        kfree(body);
        kfree(resp_body);
        kfree(resp_data_buf);
        return -ENOMEM;
    }

    /* 映射消息类型 */
    net_msg_type = powerfs_msg_type_to_net(req_hdr->type);
    if (net_msg_type == 0) {
        pr_err("powerfs: unknown msg type: %u\n", req_hdr->type);
        kfree(body);
        kfree(resp_body);
        kfree(resp_data_buf);
        return -EINVAL;
    }

    /* 编码请求数据为 TLV body */
    ret = encode_kernel_request(req_hdr->type, req_data, req_hdr,
                                 body, 1024, &body_len);
    if (ret < 0) {
        pr_err("powerfs: encode request failed: %d\n", ret);
        kfree(body);
        kfree(resp_body);
        kfree(resp_data_buf);
        return ret;
    }

    /* 发送请求并等待响应 */
    ret = powerfs_net_send_request(net_msg_type,
                                    body, body_len,
                                    NULL, 0,  /* TODO: write 数据 */
                                    resp_body, 1024,
                                    resp_data_buf, 65536,
                                    timeout_ms,
                                    &resp_body_len, &resp_data_len);

    kfree(body);

    if (ret < 0) {
        pr_debug("powerfs: send_request failed: %d (type=%u)\n",
                 ret, req_hdr->type);

        /* 如果是连接断开，标记为未连接 */
        if (ret == -EPIPE || ret == -ECONNRESET)
            g_comm_connected = false;

        kfree(resp_body);
        kfree(resp_data_buf);
        return ret;
    }

    /* powerfs_net_send_request 返回:
     *   0: 成功
     *   >0: powerfs-net 协议状态码
     */
    net_status = ret;

    if (resp_hdr) {
        resp_hdr->type = req_hdr->type;
        resp_hdr->seq = req_hdr->seq;
        resp_hdr->status = net_status;
        resp_hdr->data_len = resp_data_len;
        resp_hdr->ino = req_hdr->ino;
    }

    /* 解码响应数据 */
    if (resp_data && net_status == 0) {
        decode_kernel_response(req_hdr->type,
                                resp_body, resp_body_len,
                                resp_data_buf, resp_data_len,
                                resp_hdr, resp_data);
    }

    kfree(resp_body);
    kfree(resp_data_buf);

    /* 转换状态码 */
    return (net_status == 0) ? 0 : powerfs_net_status_to_errno((__u16)net_status);
}

/**
 * powerfs_comm_submit_notify - 提交通知 (目前未使用)
 */
int powerfs_comm_submit_notify(struct powerfs_msg_header *req_hdr,
                                void *req_data)
{
    (void)req_hdr;
    (void)req_data;
    /* 通知目前通过主动失效通道处理 */
    return 0;
}

/* ========== 便捷方法 (非冲突版本) ========== */

int powerfs_comm_read(struct inode *inode, loff_t offset, size_t length,
                      __u8 *buf, size_t *read_len)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_read_req req_data;
    __u8 *resp_data;
    int ret;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_READ;
    req_hdr.ino = inode->i_ino;

    memset(&req_data, 0, sizeof(req_data));
    req_data.ino = inode->i_ino;
    req_data.offset = offset;
    req_data.length = min_t(size_t, length, 65536);

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    /* 分配响应数据缓冲区 */
    resp_data = kmalloc(req_data.length, GFP_KERNEL);
    if (!resp_data)
        return -ENOMEM;

    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, resp_data, 10000);
    if (ret == 0) {
        memcpy(buf, resp_data, resp_hdr.data_len);
        if (read_len)
            *read_len = resp_hdr.data_len;
    }

    kfree(resp_data);
    return ret;
}

int powerfs_comm_write(struct inode *inode, loff_t offset, const __u8 *data,
                       size_t length, size_t *written)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_write_req *req_data;
    struct powerfs_write_resp resp_data;
    int ret;

    /* 分配请求结构 + 写数据 */
    req_data = kmalloc(sizeof(*req_data) + length, GFP_KERNEL);
    if (!req_data)
        return -ENOMEM;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_WRITE;
    req_hdr.ino = inode->i_ino;

    memset(req_data, 0, sizeof(*req_data));
    req_data->ino = inode->i_ino;
    req_data->offset = offset;
    req_data->length = length;

    /* 拷贝写数据到请求缓冲区后面 */
    if (length > 0)
        memcpy((__u8 *)(req_data + 1), data, length);

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(&resp_data, 0, sizeof(resp_data));

    ret = powerfs_comm_send_request(&req_hdr, req_data,
                                     &resp_hdr, &resp_data, 10000);
    kfree(req_data);

    if (ret == 0 && written)
        *written = resp_data.written;

    return ret;
}

int powerfs_comm_readdir(struct inode *dir, __u64 offset, __u32 count,
                         struct powerfs_dirent *entries, __u32 *actual_count)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_readdir_req req_data;
    __u8 *resp_data;
    int ret;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_READDIR;
    req_hdr.ino = dir->i_ino;

    memset(&req_data, 0, sizeof(req_data));
    req_data.dir_ino = dir->i_ino;
    req_data.offset = offset;
    req_data.max_entries = count;

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    /* 分配响应缓冲区 */
    resp_data = kmalloc(4096, GFP_KERNEL);
    if (!resp_data)
        return -ENOMEM;

    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, resp_data, 5000);
    if (ret == 0) {
        /* 解析 readdir 响应 */
        struct powerfs_tlv_dec dec;
        __u32 i;

        powerfs_tlv_dec_init(&dec, resp_data, resp_hdr.data_len);
        powerfs_tlv_dec_u32(&dec, POWERFS_NET_FLD_COUNT, actual_count);

        /* 解码目录项 */
        for (i = 0; i < *actual_count && i < count; i++) {
            /* 简化实现: 仅返回计数 */
            /* 完整实现需要解析嵌套的 Entry TLV */
        }
    }

    kfree(resp_data);
    return ret;
}

int powerfs_comm_readlink(struct inode *inode, char *target, size_t buflen)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    struct powerfs_readlink_req req_data;
    struct powerfs_readlink_resp resp_data;
    int ret;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_READLINK;
    req_hdr.ino = inode->i_ino;

    memset(&req_data, 0, sizeof(req_data));
    req_data.ino = inode->i_ino;

    memset(&resp_hdr, 0, sizeof(resp_hdr));
    memset(&resp_data, 0, sizeof(resp_data));

    ret = powerfs_comm_send_request(&req_hdr, &req_data,
                                     &resp_hdr, &resp_data, 2000);
    if (ret == 0 && resp_data.len > 0) {
        size_t copy_len = min_t(size_t, resp_data.len, buflen - 1);
        memcpy(target, resp_data.target, copy_len);
        target[copy_len] = '\0';
    }

    return ret;
}

int powerfs_comm_statfs(struct kstatfs *stats)
{
    struct powerfs_msg_header req_hdr, resp_hdr;
    int ret;

    memset(&req_hdr, 0, sizeof(req_hdr));
    req_hdr.type = POWERFS_MSG_STATFS;

    memset(&resp_hdr, 0, sizeof(resp_hdr));

    ret = powerfs_comm_send_request(&req_hdr, NULL,
                                     &resp_hdr, stats, 2000);
    return ret;
}

/* ========== 导出符号 ========== */

EXPORT_SYMBOL_GPL(powerfs_comm_init);
EXPORT_SYMBOL_GPL(powerfs_comm_exit);
EXPORT_SYMBOL_GPL(powerfs_comm_is_connected);
EXPORT_SYMBOL_GPL(powerfs_comm_send_request);
EXPORT_SYMBOL_GPL(powerfs_comm_submit_notify);
EXPORT_SYMBOL_GPL(powerfs_comm_connect);
EXPORT_SYMBOL_GPL(powerfs_comm_disconnect);
EXPORT_SYMBOL_GPL(powerfs_comm_read);
EXPORT_SYMBOL_GPL(powerfs_comm_write);
EXPORT_SYMBOL_GPL(powerfs_comm_readdir);
EXPORT_SYMBOL_GPL(powerfs_comm_readlink);
EXPORT_SYMBOL_GPL(powerfs_comm_statfs);
