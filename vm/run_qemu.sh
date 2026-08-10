#!/bin/bash
# PowerFS QEMU 启动脚本 (用户网络模式)
# 用于简单的内核模块开发和调试
# 抛弃用户态代理，直接使用内核态 powerfs-net 通信

set -e

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
QEMU_DISK="${OUTPUT_DIR}/qemu_disk.img"

# PowerFS 后端服务器配置
# 双网卡模式: TAP (172.30.0.x Docker network) + user-net (10.0.2.x SSH)
# 架构: 只需配置 Master 地址 (3 个 Raft 节点), Filer/Volume 通过 Master 动态发现
# Master net_port=9334, Filer/Volume 地址在 fill_super 时通过 GetTopology 发现
POWERFS_MASTER_ADDR="${POWERFS_MASTER_ADDR:-172.30.0.11,172.30.0.12,172.30.0.13}"
POWERFS_MASTER_PORT="${POWERFS_MASTER_PORT:-9334}"

# QEMU 参数
MEM_SIZE="8192"  # 内存 8GB (KASAN 需要 8G+ 用于 shadow memory)
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

# 创建虚拟磁盘 (用于测试)
if [ ! -f "${QEMU_DISK}" ]; then
    echo "=== 创建虚拟磁盘 (512MB) ==="
    qemu-img create -f qcow2 "${QEMU_DISK}" 512M
fi

echo "=== 启动 PowerFS 虚拟机 (powerfs-net 模式) ==="
echo "内核: ${KERNEL_IMAGE}"
echo "Initramfs: ${INITRAMFS}"
echo "内存: ${MEM_SIZE}MB"
echo "CPU: ${CPU_CORES} 核心"
echo "网络: 用户模式 (VM IP: 10.0.2.15, 宿主 IP: 10.0.2.2)"
echo "PowerFS Master: ${POWERFS_MASTER_ADDR}:${POWERFS_MASTER_PORT}"
echo ""

# 构建内核命令行参数 (调试模式: 提高日志级别, 启用 hung task / kasan 栈跟踪)
CMDLINE="console=ttyS0 root=/dev/ram0 rw init=/init loglevel=7"
CMDLINE="${CMDLINE} net.ifnames=0 biosdevname=0"
CMDLINE="${CMDLINE} powerfs_master_addr=${POWERFS_MASTER_ADDR}"
CMDLINE="${CMDLINE} powerfs_master_port=${POWERFS_MASTER_PORT}"
CMDLINE="${CMDLINE} hung_task_timeout=120"
CMDLINE="${CMDLINE} kasan.stacktrace=on"
CMDLINE="${CMDLINE} kasan.enabled=0"
CMDLINE="${CMDLINE} slub_debug=FZP"
CMDLINE="${CMDLINE} panic=-1"

# 启动 QEMU (双网卡: TAP for Docker network + user-net for SSH)
# -cpu host: 透传宿主 CPU (QEMU >= 5.0 可加 tsc-freq=auto)
# -rtc base=utc,clock=host: 使用宿主时钟, 减少 VM 时间漂移
# -no-hpet: 禁用 HPET, 减少定时器开销
qemu-system-x86_64 \
    ${KVM_ENABLED} \
    -cpu host \
    -rtc base=utc,clock=host \
    -no-hpet \
    -m "${MEM_SIZE}" \
    -smp "${CPU_CORES}" \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRAMFS}" \
    -append "${CMDLINE}" \
    -drive file="${QEMU_DISK}",format=qcow2,if=virtio,cache=writeback \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no,vhost=on \
    -device virtio-net-pci,netdev=net0,mq=on,vectors=6 \
    -netdev user,id=net1,hostfwd=tcp::5555-:22 \
    -device virtio-net-pci,netdev=net1 \
    -nographic \
    -serial mon:stdio \
    -monitor none \
    -display none

echo ""
echo "虚拟机已退出"

# 使用示例:
# POWERFS_MASTER_ADDR=172.30.0.11,172.30.0.12,172.30.0.13 POWERFS_MASTER_PORT=9334 ./run_qemu.sh
