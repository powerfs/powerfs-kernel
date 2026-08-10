#!/bin/bash
# PowerFS 内核开发环境一键构建
# 按顺序执行: 编译内核 -> 构建 initramfs -> 启动 QEMU

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "============================================"
echo "  PowerFS 内核开发环境 - 一键构建"
echo "============================================"
echo ""

# 步骤 1: 编译内核
echo "[1/3] 编译内核..."
echo ""
bash "${SCRIPT_DIR}/build_kernel.sh"

# 步骤 2: 构建 initramfs
echo ""
echo "[2/3] 构建 initramfs..."
echo ""
bash "${SCRIPT_DIR}/build_initramfs.sh"

# 步骤 3: 启动 QEMU (可选)
echo ""
echo "[3/3] 环境准备完成!"
echo ""
echo "============================================"
echo "  产物位置: ${SCRIPT_DIR}/output/"
echo "============================================"
echo ""
echo "启动虚拟机 (正常模式):"
echo "  ./run_qemu.sh"
echo ""
echo "启动虚拟机 (GDB 调试模式):"
echo "  ./run_qemu_debug.sh"
echo ""
echo "附加 GDB 调试器:"
echo "  ./attach_gdb.sh"
echo ""
echo "查看产物:"
ls -lh "${SCRIPT_DIR}/output/" 2>/dev/null || echo "(产物目录为空)"
echo ""
