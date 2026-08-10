#!/bin/bash
# PowerFS QEMU 启动脚本 (内核开发专用)
# 使用 TAP 设备连接 Docker 网桥，实现 VM 与容器网络互通
# 抛弃用户态代理，直接使用内核态 powerfs-net 通信

set -e

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
QEMU_DISK="${OUTPUT_DIR}/qemu_disk.img"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 网络配置
TAP_DEVICE="tap0"
VM_IP="172.30.0.100"
GATEWAY="172.30.0.1"
DNS="8.8.8.8"

# PowerFS 后端服务器配置 (Filer powerfs-net 端口)
# filer-1: 172.30.0.35, net_port=9334 (TLV 协议)
POWERFS_ADDR="${POWERFS_ADDR:-172.30.0.35}"
POWERFS_PORT="${POWERFS_PORT:-9334}"

# QEMU 参数
MEM_SIZE="4096"  # 内存 4GB
CPU_CORES="4"    # CPU 核心数
KVM_ENABLED=""   # KVM 加速

# 检查 KVM 支持
if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
    KVM_ENABLED="-enable-kvm"
    echo "KVM 加速已启用"
else
    echo "KVM 不可用，将使用软件模拟 (较慢)"
fi

# 检查内核镜像
if [ ! -f "${KERNEL_IMAGE}" ]; then
    echo "错误: 内核镜像不存在 ${KERNEL_IMAGE}"
    echo "请先运行: ./build_kernel.sh"
    exit 1
fi

# 检查 initramfs
if [ ! -f "${INITRAMFS}" ]; then
    echo "错误: initramfs 不存在 ${INITRAMFS}"
    echo "请先运行: ./build_initramfs.sh"
    exit 1
fi

# 检查 TAP 设备
if ! ip link show ${TAP_DEVICE} &>/dev/null; then
    echo "错误: TAP 设备 ${TAP_DEVICE} 不存在"
    echo "请先运行: sudo ./setup_network.sh"
    exit 1
fi

# 创建虚拟磁盘 (用于测试)
if [ ! -f "${QEMU_DISK}" ]; then
    echo "=== 创建虚拟磁盘 (1GB) ==="
    qemu-img create -f qcow2 "${QEMU_DISK}" 1G
fi

echo "=== 启动 PowerFS 虚拟机 (powerfs-net 模式) ==="
echo "内核: ${KERNEL_IMAGE}"
echo "Initramfs: ${INITRAMFS}"
echo "内存: ${MEM_SIZE}MB"
echo "CPU: ${CPU_CORES} 核心"
echo "网络: TAP 设备 ${TAP_DEVICE}"
echo "VM IP: ${VM_IP}"
echo ""
echo "PowerFS 后端 (powerfs-net): ${POWERFS_ADDR}:${POWERFS_PORT}"
echo "通信方式: 内核态 TCP (抛弃用户态代理)"
echo ""

# 构建内核命令行参数
# powerfs_addr/powerfs_port: 传递给 init 脚本，用于 insmod 加载模块
CMDLINE="console=ttyS0 root=/dev/ram0 rw init=/init quiet loglevel=3"
CMDLINE="${CMDLINE} net.ifnames=0 biosdevname=0"
CMDLINE="${CMDLINE} powerfs_addr=${POWERFS_ADDR}"
CMDLINE="${CMDLINE} powerfs_port=${POWERFS_PORT}"

# 启动 QEMU
qemu-system-x86_64 \
    ${KVM_ENABLED} \
    -m "${MEM_SIZE}" \
    -smp "${CPU_CORES}" \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRAMFS}" \
    -append "${CMDLINE}" \
    -drive file="${QEMU_DISK}",format=qcow2,if=virtio \
    -netdev tap,id=net0,ifname=${TAP_DEVICE},script=no,downscript=no,vhost=on \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
    -nographic \
    -serial mon:stdio \
    -monitor none \
    -display none

echo ""
echo "虚拟机已退出"

# 使用示例:
# POWERFS_ADDR=192.168.1.100 POWERFS_PORT=8888 ./run_qemu_kernel.sh
