#!/bin/bash
# PowerFS VM 清理脚本
#
# 一次性完成:
#   1. 终止 QEMU 进程 (释放 qcow2 文件锁)
#   2. 清理 initramfs 构建目录 (含 root 所有权文件, 需 sudo rm -rf)
#
# 将所有需 sudo 授权的操作封装进此脚本, 避免交互式逐条授权。
#
# 用法: ./clear-vm.sh

set -u

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
INITRAMFS_DIR="${OUTPUT_DIR}/initramfs"

echo "=== [1/2] 停止 QEMU 进程 ==="
PIDS=$(pgrep -f qemu-system-x86_64 || true)
if [ -z "${PIDS}" ]; then
    echo "  没有运行中的 QEMU 进程"
else
    echo "  停止 QEMU 进程: ${PIDS}"
    sudo kill ${PIDS} 2>/dev/null || true
    # 等待最多 5 秒
    for i in $(seq 1 10); do
        REMAIN=$(pgrep -f qemu-system-x86_64 || true)
        if [ -z "${REMAIN}" ]; then
            break
        fi
        sleep 0.5
    done
    if [ -n "${REMAIN}" ]; then
        echo "  未自行退出, 强制终止: ${REMAIN}"
        sudo kill -9 ${REMAIN} 2>/dev/null || true
    fi
    echo "  QEMU 已退出"
fi

echo "=== [2/2] 清理 initramfs 构建目录 ==="
if [ -d "${INITRAMFS_DIR}" ]; then
    sudo rm -rf "${INITRAMFS_DIR}"
    echo "  已清理 ${INITRAMFS_DIR}"
else
    echo "  目录不存在, 跳过: ${INITRAMFS_DIR}"
fi

echo "=== 清理完成 ==="
