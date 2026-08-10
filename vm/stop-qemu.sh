#!/bin/bash
# PowerFS QEMU 停止脚本
#
# 仅终止 qemu-system-x86_64 进程，释放 qcow2 文件锁。
# 不清理 TAP 设备 (tap0 由 setup_network.sh 持久创建，供下次启动复用)。
#
# 用法: ./stop-vm.sh

set -u

PIDS=$(pgrep -f qemu-system-x86_64 || true)

if [ -z "${PIDS}" ]; then
    echo "没有运行中的 QEMU 进程"
    exit 0
fi

echo "停止 QEMU 进程: ${PIDS}"

# QEMU 经 TAP 访问需要 root，进程为 root 拥有，需 sudo
sudo kill ${PIDS} 2>/dev/null || true

# 等待退出 (最多 5 秒)，未自行退出则强制 SIGKILL
for i in $(seq 1 10); do
    REMAIN=$(pgrep -f qemu-system-x86_64 || true)
    if [ -z "${REMAIN}" ]; then
        echo "QEMU 已退出"
        exit 0
    fi
    sleep 0.5
done

echo "未自行退出，强制终止: ${REMAIN}"
sudo kill -9 ${REMAIN} 2>/dev/null || true

# 最终确认
REMAIN=$(pgrep -f qemu-system-x86_64 || true)
if [ -n "${REMAIN}" ]; then
    echo "警告: 仍有进程残留: ${REMAIN}" >&2
    exit 1
fi

echo "QEMU 已强制退出"
