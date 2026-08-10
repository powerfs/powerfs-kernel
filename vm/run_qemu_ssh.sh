#!/bin/bash
# PowerFS QEMU 启动脚本 (带 SSH 访问)
# 使用 QEMU 用户网络模式 + 端口转发，支持 Host 免密 SSH 访问 VM

set -e

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
QEMU_DISK="${OUTPUT_DIR}/qemu_disk.img"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 网络配置
SSH_PORT="2222"  # Host 端 SSH 端口
VM_SSH_PORT="22" # VM 端 SSH 端口

# QEMU 参数
MEM_SIZE="4096"  # 内存 4GB
CPU_CORES="4"    # CPU 核心数
KVM_ENABLED=""   # KVM 加速

# 检查 KVM 支持
if [ -e /dev/kvm ] && sudo test -r /dev/kvm && sudo test -w /dev/kvm; then
    KVM_ENABLED="-enable-kvm"
    echo "KVM 加速已启用"
else
    KVM_ENABLED=""
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

# 创建虚拟磁盘 (用于测试)
if [ ! -f "${QEMU_DISK}" ]; then
    echo "=== 创建虚拟磁盘 (1GB) ==="
    qemu-img create -f qcow2 "${QEMU_DISK}" 1G
fi

echo "=== 启动 PowerFS 虚拟机 (SSH 模式) ==="
echo "内核: ${KERNEL_IMAGE}"
echo "Initramfs: ${INITRAMFS}"
echo "内存: ${MEM_SIZE}MB"
echo "CPU: ${CPU_CORES} 核心"
echo "网络: 用户模式 + SSH 端口转发"
echo "SSH 访问: ssh -p ${SSH_PORT} root@localhost"
echo ""

# 构建内核命令行参数 (使用 QEMU 用户网络)
# QEMU 用户网络: VM 为 10.0.2.x 网段，Host 网关为 10.0.2.2
CMDLINE="console=ttyS0 root=/dev/ram0 rw init=/init quiet loglevel=3"
CMDLINE="${CMDLINE} net.ifnames=0 biosdevname=0"

# 启动 QEMU (用户网络模式 + SSH 端口转发)
# 使用 sudo 以获取 KVM 访问权限
sudo qemu-system-x86_64 \
    ${KVM_ENABLED} \
    -m "${MEM_SIZE}" \
    -smp "${CPU_CORES}" \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRAMFS}" \
    -append "${CMDLINE}" \
    -drive file="${QEMU_DISK}",format=qcow2,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:${VM_SSH_PORT} \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
    -nographic \
    -serial mon:stdio \
    -monitor none \
    -display none

echo ""
echo "虚拟机已退出"
