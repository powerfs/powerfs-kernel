#!/bin/bash
# PowerFS QEMU VM2 启动脚本 (多客户端测试 - Client B)
# 与 VM1 的差异: tap1, SSH 2224, MAC 不同, 独立磁盘

set -e

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
QEMU_DISK="${OUTPUT_DIR}/qemu_disk2.img"
SHARE_DIR="/home/portion/powerfs/kernel/vm/share"

TAP_DEVICE="tap1"
SSH_PORT="2224"
VM_SSH_PORT="22"

POWERFS_ADDR="${POWERFS_ADDR:-172.30.0.31,172.30.0.32,172.30.0.33}"
POWERFS_PORT="${POWERFS_PORT:-9334}"
POWERFS_MASTER_ADDR="${POWERFS_MASTER_ADDR:-172.30.0.11,172.30.0.12,172.30.0.13}"
POWERFS_MASTER_PORT="${POWERFS_MASTER_PORT:-9334}"
POWERFS_VOLUME_ADDR="${POWERFS_VOLUME_ADDR:-172.30.0.21,172.30.0.22,172.30.0.23}"
POWERFS_VOLUME_PORT="${POWERFS_VOLUME_PORT:-8901}"

MEM_SIZE="4096"
CPU_CORES="4"
KVM_ENABLED=""

if [ -e /dev/kvm ] && sudo test -r /dev/kvm && sudo test -w /dev/kvm; then
    KVM_ENABLED="-enable-kvm"
    echo "KVM 加速已启用"
else
    echo "KVM 不可用，将使用软件模拟"
fi

if [ ! -f "${KERNEL_IMAGE}" ]; then
    echo "错误: 内核镜像不存在 ${KERNEL_IMAGE}"
    exit 1
fi

if [ ! -f "${INITRAMFS}" ]; then
    echo "错误: initramfs 不存在 ${INITRAMFS}"
    exit 1
fi

if [ ! -f "${QEMU_DISK}" ]; then
    echo "=== 创建 VM2 虚拟磁盘 (1GB) ==="
    qemu-img create -f qcow2 "${QEMU_DISK}" 1G
fi

if ! ip link show ${TAP_DEVICE} &>/dev/null; then
    echo "错误: TAP 设备 ${TAP_DEVICE} 不存在"
    echo "请先创建: sudo ip tuntap add dev tap1 mode tap && sudo ip link set tap1 master br-xxx && sudo ip link set tap1 up"
    exit 1
fi

echo "=========================================="
echo "  PowerFS QEMU VM2 (Client B)"
echo "=========================================="
echo "SSH: ssh -p ${SSH_PORT} root@localhost"
echo "TAP: ${TAP_DEVICE}"
echo "=========================================="

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

sudo qemu-system-x86_64 \
    ${KVM_ENABLED} \
    -m "${MEM_SIZE}" \
    -smp "${CPU_CORES}" \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRAMFS}" \
    -append "${CMDLINE}" \
    -drive file="${QEMU_DISK}",format=qcow2,if=virtio \
    -netdev tap,id=net0,ifname=${TAP_DEVICE},script=no,downscript=no \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:58 \
    -netdev user,id=net1,hostfwd=tcp::${SSH_PORT}-:${VM_SSH_PORT} \
    -device e1000,netdev=net1,mac=52:54:00:12:34:59 \
    -virtfs local,path="${SHARE_DIR}",mount_tag=hostshare,security_model=passthrough,id=hostshare \
    -nographic \
    -serial mon:stdio \
    -monitor none \
    -display none
