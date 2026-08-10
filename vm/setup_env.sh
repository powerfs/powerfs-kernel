#!/bin/bash
# PowerFS QEMU 内核开发环境一键安装脚本
# 需要在主机上使用 sudo 运行: sudo bash setup_env.sh

set -e

echo "=== 安装 QEMU 和内核编译工具链 ==="

apt-get update

# QEMU
apt-get install -y qemu-system-x86 qemu-utils

# 内核编译工具
apt-get install -y build-essential flex bison libelf-dev dwarves libssl-dev libncurses5-dev libssl-dev

# initramfs 工具
apt-get install -y busybox-static cpio gzip

# GDB 调试
apt-get install -y gdb

echo "=== 环境安装完成 ==="
echo "QEMU 版本:"
qemu-system-x86_64 --version | head -1
echo ""
echo "GDB 版本:"
gdb --version | head -1
echo ""
echo "下一步: 运行 ./build_kernel.sh 编译内核"
echo "       运行 ./build_initramfs.sh 构建根文件系统"
echo "       运行 ./run_qemu.sh 启动虚拟机"
