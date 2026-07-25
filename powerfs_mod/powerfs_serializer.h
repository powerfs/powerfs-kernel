/*
 * PowerFS 序列化抽象层
 *
 * 设计目标:
 *   - 为内核与用户态代理之间的消息序列化提供统一接口
 *   - 当前默认实现使用 C 结构体直接 memcpy (过渡方案)
 *   - 预留 nanopb Protobuf 替换点，方便后续升级
 *
 * 迁移路径:
 *   阶段0 (当前): 直接 memcpy 实现，简单高效
 *   阶段1+: 替换为 nanopb 实现，支持 Protobuf 序列化
 */

#ifndef _POWERFS_SERIALIZER_H
#define _POWERFS_SERIALIZER_H

#include <linux/types.h>
#include "powerfs_comm.h"

/* ========== 序列化操作接口 ========== */

/**
 * struct powerfs_serializer - 序列化操作接口
 *
 * 提供统一的序列化/反序列化接口
 * 当前默认实现直接使用 memcpy
 * 后续可替换为 nanopb 实现
 */
struct powerfs_serializer {
    /* 将请求结构体序列化为字节流 */
    int (*serialize_request)(u32 msg_type, void *req_struct,
                              u32 *out_len, u8 *out_buf);

    /* 从字节流反序列化为响应结构体 */
    int (*deserialize_response)(u32 msg_type, u8 *in_buf,
                                u32 in_len, void *resp_struct);

    /* 初始化序列化器 */
    int (*init)(void);

    /* 销毁序列化器 */
    void (*destroy)(void);
};

/* ========== 默认实现: 直接 memcpy ========== */

/**
 * powerfs_serialize_direct - 直接 memcpy 序列化请求
 *
 * 当前过渡方案: 直接将结构体内容作为字节流传递
 * 优点: 简单、高效、无额外开销
 * 缺点: 依赖结构体布局，不同版本可能不兼容
 *
 * @msg_type: 消息类型 (用于确定结构体大小)
 * @req_struct: 请求结构体指针
 * @out_len: 输出序列化后长度
 * @out_buf: 输出缓冲区
 *
 * 返回: 0 成功, 负值失败
 */
int powerfs_serialize_direct(u32 msg_type, void *req_struct,
                             u32 *out_len, u8 *out_buf);

/**
 * powerfs_deserialize_direct - 直接 memcpy 反序列化响应
 *
 * 当前过渡方案: 直接从字节流拷贝到结构体
 *
 * @msg_type: 消息类型 (用于确定结构体大小)
 * @in_buf: 输入字节流
 * @in_len: 输入长度
 * @resp_struct: 输出响应结构体指针
 *
 * 返回: 0 成功, 负值失败
 */
int powerfs_deserialize_direct(u32 msg_type, u8 *in_buf,
                               u32 in_len, void *resp_struct);

/* ========== nanopb 实现 (阶段1+ 预留) ========== */

/**
 * powerfs_serialize_nanopb - nanopb 序列化请求 (预留)
 *
 * 阶段1+ 实现: 使用 nanopb 将 Protobuf 结构体序列化
 *
 * @msg_type: 消息类型
 * @req_struct: 请求结构体 (Protobuf 结构体)
 * @out_len: 输出序列化后长度
 * @out_buf: 输出缓冲区
 *
 * 返回: 0 成功, 负值失败
 */
int powerfs_serialize_nanopb(u32 msg_type, void *req_struct,
                             u32 *out_len, u8 *out_buf);

/**
 * powerfs_deserialize_nanopb - nanopb 反序列化响应 (预留)
 *
 * 阶段1+ 实现: 使用 nanopb 从字节流反序列化 Protobuf 结构体
 *
 * @msg_type: 消息类型
 * @in_buf: 输入字节流
 * @in_len: 输入长度
 * @resp_struct: 输出 Protobuf 结构体
 *
 * 返回: 0 成功, 负值失败
 */
int powerfs_deserialize_nanopb(u32 msg_type, u8 *in_buf,
                               u32 in_len, void *resp_struct);

/* ========== 序列化器管理 ========== */

/**
 * powerfs_set_serializer - 切换序列化器
 *
 * 运行时可替换序列化实现
 * 当前默认使用 powerfs_serialize_direct
 *
 * @serializer: 新的序列化器实现
 *
 * 返回: 0 成功, 负值失败
 */
int powerfs_set_serializer(struct powerfs_serializer *serializer);

/**
 * powerfs_get_serializer - 获取当前序列化器
 *
 * 返回: 当前序列化器指针
 */
struct powerfs_serializer *powerfs_get_serializer(void);

/**
 * powerfs_serializer_init - 初始化序列化子系统
 *
 * 默认使用 direct 实现
 */
int powerfs_serializer_init(void);

/**
 * powerfs_serializer_exit - 清理序列化子系统
 */
void powerfs_serializer_exit(void);

/* ========== 辅助函数: 结构体大小查询 ========== */

/**
 * powerfs_get_req_size - 获取指定消息类型的请求结构体大小
 *
 * @msg_type: 消息类型
 *
 * 返回: 结构体大小 (字节), 0 表示未知类型
 */
u32 powerfs_get_req_size(u32 msg_type);

/**
 * powerfs_get_resp_size - 获取指定消息类型的响应结构体大小
 *
 * @msg_type: 消息类型
 *
 * 返回: 结构体大小 (字节), 0 表示未知类型
 */
u32 powerfs_get_resp_size(u32 msg_type);

#endif /* _POWERFS_SERIALIZER_H */
