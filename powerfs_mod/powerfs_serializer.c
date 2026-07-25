/*
 * PowerFS 序列化抽象层实现
 *
 * 当前实现: 直接 memcpy (过渡方案)
 * 后续可替换为 nanopb Protobuf 实现
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "powerfs_serializer.h"

/* ========== 当前序列化器实例 ========== */

/* 默认的 direct 实现 */
static int powerfs_direct_serialize(u32 msg_type, void *req_struct,
                                     u32 *out_len, u8 *out_buf);
static int powerfs_direct_deserialize(u32 msg_type, u8 *in_buf,
                                       u32 in_len, void *resp_struct);
static int powerfs_direct_init(void);
static void powerfs_direct_destroy(void);

static struct powerfs_serializer direct_serializer = {
    .serialize_request  = powerfs_direct_serialize,
    .deserialize_response = powerfs_direct_deserialize,
    .init               = powerfs_direct_init,
    .destroy            = powerfs_direct_destroy,
};

/* 当前使用的序列化器 */
static struct powerfs_serializer *current_serializer = NULL;

/* ========== 结构体大小查询表 ========== */

/**
 * powerfs_get_req_size - 获取指定消息类型的请求结构体大小
 */
u32 powerfs_get_req_size(u32 msg_type)
{
    switch (msg_type) {
    case POWERFS_MSG_LOOKUP:
        return sizeof(struct powerfs_lookup_req);
    case POWERFS_MSG_GETATTR:
        return sizeof(struct powerfs_getattr_req);
    case POWERFS_MSG_SETATTR:
        return sizeof(struct powerfs_setattr_req);
    case POWERFS_MSG_MKDIR:
    case POWERFS_MSG_CREATE:
        return sizeof(struct powerfs_create_req);
    case POWERFS_MSG_UNLINK:
    case POWERFS_MSG_RMDIR:
        return sizeof(struct powerfs_remove_req);
    case POWERFS_MSG_RENAME:
        return sizeof(struct powerfs_rename_req);
    case POWERFS_MSG_READDIR:
        return sizeof(struct powerfs_readdir_req);
    case POWERFS_MSG_SYMLINK:
        return sizeof(struct powerfs_symlink_req);
    case POWERFS_MSG_READLINK:
        return sizeof(struct powerfs_readlink_req);
    case POWERFS_MSG_LINK:
        return sizeof(struct powerfs_link_req);
    case POWERFS_MSG_MKNOD:
        return sizeof(struct powerfs_mknod_req);
    case POWERFS_MSG_READ:
        return sizeof(struct powerfs_read_req);
    case POWERFS_MSG_WRITE:
        return sizeof(struct powerfs_write_req);
    case POWERFS_MSG_FSYNC:
        return sizeof(struct powerfs_fsync_req);
    case POWERFS_MSG_TRUNCATE:
        return sizeof(struct powerfs_truncate_req);
    case POWERFS_MSG_STATFS:
        return sizeof(struct powerfs_statfs_req);
    default:
        pr_warn("powerfs: unknown msg type %u for req size\n", msg_type);
        return 0;
    }
}

/**
 * powerfs_get_resp_size - 获取指定消息类型的响应结构体大小
 */
u32 powerfs_get_resp_size(u32 msg_type)
{
    switch (msg_type) {
    case POWERFS_MSG_LOOKUP:
        return sizeof(struct powerfs_lookup_resp);
    case POWERFS_MSG_GETATTR:
        return sizeof(struct powerfs_getattr_resp);
    case POWERFS_MSG_SETATTR:
        return sizeof(struct powerfs_setattr_resp);
    case POWERFS_MSG_MKDIR:
    case POWERFS_MSG_CREATE:
        return sizeof(struct powerfs_create_resp);
    case POWERFS_MSG_UNLINK:
    case POWERFS_MSG_RMDIR:
        return 0;  /* 无响应数据 */
    case POWERFS_MSG_RENAME:
        return sizeof(struct powerfs_rename_resp);
    case POWERFS_MSG_READDIR:
        return sizeof(struct powerfs_dirent);  /* 单个目录项 */
    case POWERFS_MSG_SYMLINK:
        return sizeof(struct powerfs_symlink_resp);
    case POWERFS_MSG_READLINK:
        return sizeof(struct powerfs_readlink_resp);
    case POWERFS_MSG_LINK:
        return sizeof(struct powerfs_link_resp);
    case POWERFS_MSG_MKNOD:
        return sizeof(struct powerfs_mknod_resp);
    case POWERFS_MSG_READ:
        return sizeof(struct powerfs_read_resp);
    case POWERFS_MSG_WRITE:
        return sizeof(struct powerfs_write_resp);
    case POWERFS_MSG_FSYNC:
        return sizeof(struct powerfs_fsync_resp);
    case POWERFS_MSG_TRUNCATE:
        return sizeof(struct powerfs_truncate_resp);
    case POWERFS_MSG_STATFS:
        return sizeof(struct powerfs_statfs_resp);
    default:
        pr_warn("powerfs: unknown msg type %u for resp size\n", msg_type);
        return 0;
    }
}

/* ========== Direct 实现: memcpy ========== */

/**
 * powerfs_direct_serialize - 直接 memcpy 序列化
 */
static int powerfs_direct_serialize(u32 msg_type, void *req_struct,
                                     u32 *out_len, u8 *out_buf)
{
    u32 size;

    if (!req_struct || !out_len || !out_buf)
        return -EINVAL;

    size = powerfs_get_req_size(msg_type);
    if (size == 0) {
        pr_warn("powerfs: direct serialize: unknown msg type %u\n", msg_type);
        return -EINVAL;
    }

    if (size > POWERFS_MAX_DATA_SIZE) {
        pr_warn("powerfs: direct serialize: size %u too large\n", size);
        return -E2BIG;
    }

    /* 直接 memcpy */
    memcpy(out_buf, req_struct, size);
    *out_len = size;

    pr_debug("powerfs: direct serialize msg_type=%u size=%u\n", msg_type, size);
    return 0;
}

/**
 * powerfs_direct_deserialize - 直接 memcpy 反序列化
 */
static int powerfs_direct_deserialize(u32 msg_type, u8 *in_buf,
                                       u32 in_len, void *resp_struct)
{
    u32 size;

    if (!in_buf || !resp_struct)
        return -EINVAL;

    size = powerfs_get_resp_size(msg_type);
    if (size == 0) {
        /* 有些响应没有数据 (如 unlink/rmdir) */
        if (in_len == 0)
            return 0;
        pr_warn("powerfs: direct deserialize: unknown msg type %u\n", msg_type);
        return -EINVAL;
    }

    if (in_len < size) {
        pr_warn("powerfs: direct deserialize: data too short (%u < %u)\n",
                in_len, size);
        return -EINVAL;
    }

    /* 直接 memcpy */
    memcpy(resp_struct, in_buf, size);

    pr_debug("powerfs: direct deserialize msg_type=%u size=%u\n", msg_type, size);
    return 0;
}

static int powerfs_direct_init(void)
{
    pr_info("powerfs: direct serializer initialized\n");
    return 0;
}

static void powerfs_direct_destroy(void)
{
    pr_info("powerfs: direct serializer destroyed\n");
}

/* ========== nanopb 实现 (占位符，阶段1+ 实现) ========== */

int powerfs_serialize_nanopb(u32 msg_type, void *req_struct,
                             u32 *out_len, u8 *out_buf)
{
    /* 阶段1+ 实现: 使用 nanopb 序列化 */
    pr_warn("powerfs: nanopb serializer not implemented yet\n");
    return -ENOSYS;
}

int powerfs_deserialize_nanopb(u32 msg_type, u8 *in_buf,
                               u32 in_len, void *resp_struct)
{
    /* 阶段1+ 实现: 使用 nanopb 反序列化 */
    pr_warn("powerfs: nanopb deserializer not implemented yet\n");
    return -ENOSYS;
}

/* ========== 序列化器管理 ========== */

/**
 * powerfs_set_serializer - 切换序列化器
 */
int powerfs_set_serializer(struct powerfs_serializer *serializer)
{
    if (!serializer)
        return -EINVAL;

    /* 销毁旧的 */
    if (current_serializer && current_serializer->destroy)
        current_serializer->destroy();

    /* 初始化新的 */
    if (serializer->init) {
        int ret = serializer->init();
        if (ret)
            return ret;
    }

    current_serializer = serializer;
    pr_info("powerfs: serializer switched to %p\n", serializer);

    return 0;
}

/**
 * powerfs_get_serializer - 获取当前序列化器
 */
struct powerfs_serializer *powerfs_get_serializer(void)
{
    return current_serializer;
}

/**
 * powerfs_serializer_init - 初始化序列化子系统
 */
int powerfs_serializer_init(void)
{
    pr_info("powerfs: initializing serializer subsystem...\n");

    /* 默认使用 direct 实现 */
    current_serializer = &direct_serializer;
    if (current_serializer->init)
        current_serializer->init();

    pr_info("powerfs: serializer subsystem initialized (direct mode)\n");
    return 0;
}

/**
 * powerfs_serializer_exit - 清理序列化子系统
 */
void powerfs_serializer_exit(void)
{
    pr_info("powerfs: destroying serializer subsystem...\n");

    if (current_serializer && current_serializer->destroy)
        current_serializer->destroy();

    current_serializer = NULL;

    pr_info("powerfs: serializer subsystem destroyed\n");
}
