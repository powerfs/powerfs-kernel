#!/bin/bash
# PowerFS QEMU GDB 调试模式启动脚本
# 启动虚拟机并在 GDB 服务器端口等待调试器连接

set -e

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
VMLINUX="${OUTPUT_DIR}/vmlinux"
QEMU_DISK="${OUTPUT_DIR}/qemu_disk.img"

# QEMU 参数
MEM_SIZE="2048"
CPU_CORES="4"
GDB_PORT="1234"

# 检查文件
if [ ! -f "${KERNEL_IMAGE}" ]; then
    echo "错误: 内核镜像不存在 ${KERNEL_IMAGE}"
    echo "请先运行: ./build_kernel.sh"
    exit 1
fi

if [ ! -f "${INITRAMFS}" ]; then
    echo "错误: initramfs 不存在 ${INITRAMFS}"
    echo "请先运行: ./build_initramfs.sh"
    exit 1
fi

# 创建磁盘
if [ ! -f "${QEMU_DISK}" ]; then
    qemu-img create -f qcow2 "${QEMU_DISK}" 512M
fi

echo "=== 启动 PowerFS 虚拟机 (GDB 调试模式) ==="
echo "内核: ${KERNEL_IMAGE}"
echo "Initramfs: ${INITRAMFS}"
echo "GDB 端口: ${GDB_PORT}"
echo ""
echo "在另一个终端运行 GDB:"
echo "  gdb ${VMLINUX}"
echo "  (gdb) target remote :${GDB_PORT}"
echo "  (gdb) continue"
echo ""
echo "或者运行: ./attach_gdb.sh"
echo ""

# 启动 QEMU (S 标志: 启动后暂停，等待 GDB 连接)
qemu-system-x86_64 \
    -m "${MEM_SIZE}" \
    -smp "${CPU_CORES}" \
    -kernel "${KERNEL_IMAGE}" \
    -initrd "${INITRAMFS}" \
    -append "console=ttyS0 root=/dev/ram0 rw init=/init loglevel=8" \
    -drive file="${QEMU_DISK}",format=qcow2,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::5555-:22 \
    -device virtio-net-pci,netdev=net0 \
    -s \
    -S \
    -nographic \
    -serial mon:stdio \
    -monitor none \
    -display none

echo "虚拟机已退出"
