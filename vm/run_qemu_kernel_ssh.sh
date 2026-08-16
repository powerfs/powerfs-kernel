#!/bin/bash
# PowerFS QEMU 启动脚本 (内核开发 + SSH 访问)
# 双网卡模式:
#   eth0: TAP 设备 -> Docker 网桥 (访问容器服务)
#   eth1: QEMU 用户网络 + SSH 端口转发 (Host 免密 SSH 访问 VM)

set -e

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
QEMU_DISK="${OUTPUT_DIR}/qemu_disk.img"

# 网络配置
TAP_DEVICE="tap0"
VM_IP="172.20.0.100"
GATEWAY="172.20.0.1"

# SSH 配置
SSH_PORT="2223"  # Host 端 SSH 端口 (避免冲突)
VM_SSH_PORT="22" # VM 端 SSH 端口

# PowerFS 后端服务器配置
# Filer IP: 172.30.0.31/32/33, powerfs-net 端口: 9334
# Master IP: 172.30.0.11/12/13, net_port: 9334
# Volume IP: 172.30.0.21/22/23, net_port: 8901
POWERFS_ADDR="${POWERFS_ADDR:-172.30.0.31,172.30.0.32,172.30.0.33}"
POWERFS_PORT="${POWERFS_PORT:-9334}"
POWERFS_MASTER_ADDR="${POWERFS_MASTER_ADDR:-172.30.0.11,172.30.0.12,172.30.0.13}"
POWERFS_MASTER_PORT="${POWERFS_MASTER_PORT:-9334}"
POWERFS_VOLUME_ADDR="${POWERFS_VOLUME_ADDR:-172.30.0.21,172.30.0.22,172.30.0.23}"
POWERFS_VOLUME_PORT="${POWERFS_VOLUME_PORT:-8901}"

# 9p 共享目录 (Host <-> VM 文件共享, 用于 hot deploy powerfs.ko 和测试脚本)
SHARE_DIR="/home/portion/powerfs/kernel/vm/share"

# QEMU 参数
MEM_SIZE="4096"
CPU_CORES="4"
KVM_ENABLED=""

# 检查 KVM 支持
if [ -e /dev/kvm ] && sudo test -r /dev/kvm && sudo test -w /dev/kvm; then
    KVM_ENABLED="-enable-kvm"
    echo "KVM 加速已启用"
else
    echo "KVM 不可用，将使用软件模拟 (较慢)"
fi

# 检查内核镜像
if [ ! -f "${KERNEL_IMAGE}" ]; then
    echo "错误: 内核镜像不存在 ${KERNEL_IMAGE}"
    exit 1
fi

# 检查 initramfs
if [ ! -f "${INITRAMFS}" ]; then
    echo "错误: initramfs 不存在 ${INITRAMFS}"
    exit 1
fi

# 检查 TAP 设备
if ! ip link show ${TAP_DEVICE} &>/dev/null; then
    echo "错误: TAP 设备 ${TAP_DEVICE} 不存在"
    echo "请先运行: sudo ./setup_network.sh"
    exit 1
fi

# 创建虚拟磁盘
if [ ! -f "${QEMU_DISK}" ]; then
    echo "=== 创建虚拟磁盘 (1GB) ==="
    qemu-img create -f qcow2 "${QEMU_DISK}" 1G
fi

echo "=========================================="
echo "  PowerFS QEMU 启动 (SSH + powerfs-net)"
echo "=========================================="
echo "内核: ${KERNEL_IMAGE}"
echo "Initramfs: ${INITRAMFS}"
echo "内存: ${MEM_SIZE}MB, CPU: ${CPU_CORES} 核"
echo ""
echo "网络 (双网卡):"
echo "  eth0: TAP -> Docker 网桥 (VM IP: ${VM_IP})"
echo "  eth1: 用户网络 + SSH 转发 (Host: localhost:${SSH_PORT})"
echo ""
echo "PowerFS 后端 (powerfs-net):"
echo "  Filer (fallback):  ${POWERFS_ADDR}:${POWERFS_PORT}"
echo "  Master:            ${POWERFS_MASTER_ADDR}:${POWERFS_MASTER_PORT}"
echo "  Volume:            ${POWERFS_VOLUME_ADDR}:${POWERFS_VOLUME_PORT}"
echo ""
echo "SSH 登录: ssh -p ${SSH_PORT} root@localhost"
echo "=========================================="

# 构建内核命令行参数
# loglevel=7 + 去掉 quiet: 让所有内核日志 (含 powerfs pr_info/pr_warn) 输出到 serial
# printk.time=1: 每条日志带时间戳, 方便定位 stall 触发时刻
# rcupdate.rcu_cpu_stall_suppress=0: 确保 RCU stall 警告不被抑制
CMDLINE="console=ttyS0 root=/dev/ram0 rw init=/init loglevel=7"
CMDLINE="${CMDLINE} net.ifnames=0 biosdevname=0"
CMDLINE="${CMDLINE} printk.time=1"
CMDLINE="${CMDLINE} powerfs_addr=${POWERFS_ADDR}"
CMDLINE="${CMDLINE} powerfs_port=${POWERFS_PORT}"
CMDLINE="${CMDLINE} powerfs_master_addr=${POWERFS_MASTER_ADDR}"
CMDLINE="${CMDLINE} powerfs_master_port=${POWERFS_MASTER_PORT}"
CMDLINE="${CMDLINE} powerfs_volume_addr=${POWERFS_VOLUME_ADDR}"
CMDLINE="${CMDLINE} powerfs_volume_port=${POWERFS_VOLUME_PORT}"
CMDLINE="${CMDLINE} kasan=off"
CMDLINE="${CMDLINE} wq_watchdog_thresh=0"
CMDLINE="${CMDLINE} slub_debug=FZP"
CMDLINE="${CMDLINE} rcupdate.rcu_cpu_stall_suppress=0"

# 启动 QEMU (双网卡模式 + 9p 共享)
# 使用 sudo 以获取 KVM 访问权限
# 9p virtfs: mount_tag=hostshare, VM 中 mount -t 9p -o trans=virtio hostshare /mnt/host
# security_model=passthrough: 保留 Host 文件权限 (root 可读写)
sudo qemu-system-x86_64 \
    ${KVM_ENABLED} \
    -m "${MEM_SIZE}" \
    -smp "${CPU_CORES}" \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRAMFS}" \
    -append "${CMDLINE}" \
    -drive file="${QEMU_DISK}",format=qcow2,if=virtio \
    -netdev tap,id=net0,ifname=${TAP_DEVICE},script=no,downscript=no \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
    -netdev user,id=net1,hostfwd=tcp::${SSH_PORT}-:${VM_SSH_PORT} \
    -device e1000,netdev=net1,mac=52:54:00:12:34:57 \
    -virtfs local,path="${SHARE_DIR}",mount_tag=hostshare,security_model=passthrough,id=hostshare \
    -nographic \
    -serial mon:stdio \
    -monitor none \
    -display none

echo ""
echo "虚拟机已退出"

# 使用示例:
# POWERFS_ADDR=172.20.0.35 POWERFS_PORT=9334 ./run_qemu_kernel_ssh.sh
# SSH 登录: ssh -p 2223 root@localhost
