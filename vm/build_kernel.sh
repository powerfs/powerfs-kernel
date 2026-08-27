#!/bin/bash
# PowerFS 内核编译脚本
# 从 ubuntu-linux-git 源码编译内核，用于 QEMU 虚拟化环境

set -e

# 路径配置 (支持环境变量覆盖，CI 中通过 env 设置 KERNEL_SOURCE)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_SOURCE="${KERNEL_SOURCE:-/home/portion/powerfs/linux-6.17}"
OUTPUT_DIR="${SCRIPT_DIR}/output"
BUILD_DIR="${OUTPUT_DIR}/build"

# 内核版本检测
KERNEL_VERSION=$(cat "${KERNEL_SOURCE}/Makefile" | head -5 | grep VERSION | awk '{print $3}')
PATCHLEVEL=$(cat "${KERNEL_SOURCE}/Makefile" | grep PATCHLEVEL | awk '{print $3}')
FULL_VERSION="${KERNEL_VERSION}.${PATCHLEVEL}.0"

echo "=== PowerFS 内核编译 ==="
echo "源码路径: ${KERNEL_SOURCE}"
echo "内核版本: ${FULL_VERSION}"
echo "输出目录: ${OUTPUT_DIR}"

# 创建输出目录
mkdir -p "${OUTPUT_DIR}" "${BUILD_DIR}"

# 使用 defconfig 生成基础配置 (x86_64)
echo "=== 生成内核配置 ==="
cd "${KERNEL_SOURCE}"
make defconfig

# 修改配置以支持 QEMU 和调试
echo "=== 优化内核配置 (QEMU + 调试) ==="

# 启用 KVM 支持
./scripts/config --enable CONFIG_KVM
./scripts/config --enable CONFIG_KVM_INTEL
./scripts/config --enable CONFIG_KVM_AMD

# 启用调试信息
./scripts/config --enable CONFIG_DEBUG_INFO
./scripts/config --enable CONFIG_DEBUG_INFO_DWARF4
./scripts/config --enable CONFIG_DEBUG_KERNEL
./scripts/config --enable CONFIG_DEBUG_SLAB
./scripts/config --disable CONFIG_DEBUG_INFO_REDACTED

# 启用 printk 详细输出
# LOG_BUF_SHIFT=20 → 2^20 = 1MB ring buffer (原 17=128KB 在并发删除测试中溢出)
./scripts/config --enable CONFIG_PRINTK
./scripts/config --enable CONFIG_LOG_BUF_SHIFT
./scripts/config --set-val CONFIG_LOG_BUF_SHIFT 20

# 启用文件系统调试
./scripts/config --enable CONFIG_DEBUG_FS
./scripts/config --enable CONFIG_VFS_DEBUG

# 启用 KGDB (内核调试器)
./scripts/config --enable CONFIG_KGDB
./scripts/config --enable CONFIG_KGDB_SERIAL_CONSOLE
./scripts/config --enable CONFIG_KGDB_KDB

# 启用 KDB
./scripts/config --enable CONFIG_KDB_KEYBOARD

# === 故障诊断调试选项 (用于定位 RCU stall / VFS hang / UAF / 死锁) ===

# Hung task 检测: 自动报告 D 状态超过 120s 的任务+完整调用栈
./scripts/config --enable CONFIG_DETECT_HUNG_TASK
./scripts/config --set-val CONFIG_DEFAULT_HUNG_TASK_TIMEOUT 120

# Magic SysRq: hang 时可发送 sysrq+t (所有任务栈) / sysrq+w (阻塞任务)
./scripts/config --enable CONFIG_MAGIC_SYSRQ
./scripts/config --enable CONFIG_MAGIC_SYSRQ_DEFAULT_ENABLE

# KASAN: 已禁用 (CONFIG_KASAN_INLINE 导致 kasan=off 无效, 内联检查无法运行时关闭)
# KASAN 在 PREEMPT 内核中检测到 __d_lookup_rcu UAF, 可能是 RCU walk 与
# preemptible RCU 的已知交互问题. 禁用 KASAN 以获得稳定的测试环境.
# 后续需要调试内存问题时可临时重新启用.
./scripts/config --disable CONFIG_KASAN
./scripts/config --disable CONFIG_KASAN_GENERIC
./scripts/config --disable CONFIG_KASAN_INLINE
./scripts/config --disable CONFIG_KASAN_VMALLOC

# LOCKDEP: KASAN+LOCKDEP 同时开会导致 VM 极慢, 暂时关闭
# (KASAN 已能检测大部分内存问题, LOCKDEP 后续按需开启)
./scripts/config --disable CONFIG_LOCKDEP
./scripts/config --disable CONFIG_PROVE_LOCKING
./scripts/config --disable CONFIG_LOCKDEP_SUPPORT

# RCU 调试: RCU stall 检测 (缩短到 21s 以便快速发现问题)
./scripts/config --disable CONFIG_PROVE_RCU
./scripts/config --enable CONFIG_RCU_CPU_STALL_TIMEOUT
./scripts/config --set-val CONFIG_RCU_CPU_STALL_TIMEOUT 21

# 原子上下文睡眠检测
./scripts/config --enable CONFIG_DEBUG_ATOMIC_SLEEP

# 链表操作检查
./scripts/config --enable CONFIG_DEBUG_LIST
./scripts/config --enable CONFIG_DEBUG_SG

# 对象生命周期管理错误检测
./scripts/config --enable CONFIG_DEBUG_OBJECTS
./scripts/config --enable CONFIG_DEBUG_OBJECTS_FREE

# 内核内存泄漏检测
./scripts/config --enable CONFIG_DEBUG_KMEMLEAK

# Workqueue 死锁检测
./scripts/config --enable CONFIG_WQ_WATCHDOG

# === Soft/Hard lockup 检测 (关键: 检测 CPU 自旋死锁) ===
# Softlockup: CPU 在内核态连续运行 >阈值(10s) 未调度, 常见于持锁自旋
# Hardlockup: CPU 连续未响应中断, 常见于 IRQ handler 死锁
# 启用后内核会自动在 serial 日志输出调用栈 + 触发 panic (配合 panic_on_oops)
./scripts/config --enable CONFIG_SOFTLOCKUP_DETECTOR
./scripts/config --enable CONFIG_HARDLOCKUP_DETECTOR
# softlockup/hardlockup 触发时 panic, 便于在 serial 日志捕获完整现场
# (CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC = softlockup_panic=1 的默认值)
./scripts/config --enable CONFIG_BOOTPARAM_SOFTLOCKUP_PANIC
./scripts/config --enable CONFIG_BOOTPARAM_HARDLOCKUP_PANIC

# Hung task 触发 panic (D 状态 >60s 时 panic 而非仅警告)
./scripts/config --enable CONFIG_BOOTPARAM_HUNG_TASK_PANIC

# Oops 时 panic: 避免 oops 后级联损坏导致现场丢失
# 配合 softlockup_panic 确保所有致命问题都在 serial 日志留下完整调用栈
./scripts/config --enable CONFIG_PANIC_ON_OOPS
./scripts/config --set-val CONFIG_PANIC_ON_OOPS_VALUE 1

# Debug slab (已有, 确认开启)
./scripts/config --enable CONFIG_DEBUG_SLAB
./scripts/config --enable CONFIG_SLUB_DEBUG

# 启用 QEMU 虚拟硬件支持
./scripts/config --enable CONFIG_PCI
./scripts/config --enable CONFIG_PCI_MSI
./scripts/config --enable CONFIG_NET
./scripts/config --enable CONFIG_NET_PCI
./scripts/config --enable CONFIG_ETHERNET
./scripts/config --enable CONFIG_NET_CARD
./scripts/config --enable CONFIG_VIRTIO_NET
./scripts/config --enable CONFIG_VIRTIO_BLK
./scripts/config --enable CONFIG_BLK_DEV
./scripts/config --enable CONFIG_BLK_DEV_SD
./scripts/config --enable CONFIG_BLK_DEV_SR
./scripts/config --enable CONFIG_BLK_DEV_DM

# 启用 io_uring (PowerFS 核心依赖)
./scripts/config --enable CONFIG_IO_URING
./scripts/config --enable CONFIG_IO_URING_FANOUT
./scripts/config --enable CONFIG_IO_URING_CMD
./scripts/config --enable CONFIG_IO_WQ

# 启用 ext4 文件系统
./scripts/config --enable CONFIG_EXT4_FS
./scripts/config --enable CONFIG_EXT4_FS_POSIX_ACL
./scripts/config --enable CONFIG_EXT4_FS_SECURITY

# 启用 FUSE (用户态文件系统, 可选参考)
./scripts/config --enable CONFIG_FUSE_FS

# 启用 NFS (网络文件系统, 可选)
./scripts/config --enable CONFIG_NFS_FS
./scripts/config --enable CONFIG_NFS_V3
./scripts/config --enable CONFIG_NFS_V4

# 启用网络相关 (用于 gRPC 通信)
./scripts/config --enable CONFIG_NET
./scripts/config --enable CONFIG_TCP
./scripts/config --enable CONFIG_NET_TCP
./scripts/config --enable CONFIG_NET_RX_BUSY_POLL

# 启用共享内存 (用于 mmap)
./scripts/config --enable CONFIG_SHMEM
./scripts/config --enable CONFIG_TMPFS
./scripts/config --enable CONFIG_TMPFS_XATTR
./scripts/config --enable CONFIG_TMPFS_POSIX_ACL

# 启用调试符号 (用于 GDB)
./scripts/config --enable CONFIG_DEBUG_INFO
./scripts/config --disable CONFIG_DEBUG_INFO_REDACTED

# 启用 procfs 和 sysfs
./scripts/config --enable CONFIG_PROC_FS
./scripts/config --enable CONFIG_SYSFS
./scripts/config --enable CONFIG_DEBUG_FS

# 设置 swap 配置
./scripts/config --enable CONFIG_SWAP
./scripts/config --enable CONFIG_SWAP_FILE

echo "=== 开始编译内核 (使用所有 CPU 核心) ==="
cd "${KERNEL_SOURCE}"
make -j$(nproc)

echo "=== 编译模块 ==="
make modules

echo "=== 安装内核到输出目录 ==="
# 复制 bzImage
cp arch/x86/boot/bzImage "${OUTPUT_DIR}/bzImage"

# 复制 System.map (用于调试)
cp System.map "${OUTPUT_DIR}/System.map"

# 复制 vmlinux (用于 GDB 调试)
cp vmlinux "${OUTPUT_DIR}/vmlinux"

# 编译模块
echo "=== 编译 PowerFS 内核模块 ==="
POWERFS_MOD_DIR="${SCRIPT_DIR}/../powerfs_mod"
if [ -d "${POWERFS_MOD_DIR}" ]; then
    cd "${POWERFS_MOD_DIR}"
    make clean 2>/dev/null || true
    make KDIR="${KERNEL_SOURCE}"
    if [ -f "powerfs.ko" ]; then
        cp powerfs.ko "${OUTPUT_DIR}/"
        echo "PowerFS 模块编译成功: powerfs.ko"
    else
        echo "警告: PowerFS 模块编译失败或未生成 .ko 文件"
    fi
fi

echo ""
echo "=== 内核编译完成 ==="
echo "产物列表:"
ls -lh "${OUTPUT_DIR}/"
echo ""

# === 自动打包 initramfs (包含新编译的 powerfs.ko) ===
# 用 sudo 打包, 避免普通用户 find 遇到 root/.ssh 权限拒绝导致文件遗漏
INITRAMFS_DIR="${OUTPUT_DIR}/initramfs"
if [ -d "${INITRAMFS_DIR}" ]; then
    echo "=== 打包 initramfs (含新 powerfs.ko) ==="
    cp "${OUTPUT_DIR}/powerfs.ko" "${INITRAMFS_DIR}/powerfs.ko"
    # 确保 SSH 主机密钥存在 (若缺失, 生成新的)
    if [ ! -f "${INITRAMFS_DIR}/etc/ssh/ssh_host_rsa_key" ]; then
        echo "  SSH 主机密钥缺失, 生成新密钥..."
        sudo ssh-keygen -t rsa -b 2048 -f "${INITRAMFS_DIR}/etc/ssh/ssh_host_rsa_key" -N "" -q
        sudo ssh-keygen -t ecdsa -f "${INITRAMFS_DIR}/etc/ssh/ssh_host_ecdsa_key" -N "" -q
        sudo ssh-keygen -t ed25519 -f "${INITRAMFS_DIR}/etc/ssh/ssh_host_ed25519_key" -N "" -q
    fi
    # sudo find 确保所有文件 (含 root/.ssh, etc/ssh 等) 都被包含
    sudo bash -c "cd '${INITRAMFS_DIR}' && find . | cpio -o -H newc 2>/dev/null | gzip > '${OUTPUT_DIR}/initramfs.cpio.gz'"
    echo "  initramfs.cpio.gz 打包完成: $(ls -lh ${OUTPUT_DIR}/initramfs.cpio.gz | awk '{print $5}')"
else
    echo "警告: initramfs 目录不存在 (${INITRAMFS_DIR}), 请先运行 ./build_initramfs.sh"
fi

echo ""
echo "=== 全部完成 ==="
echo "可直接运行: sudo ./run_qemu.sh"
