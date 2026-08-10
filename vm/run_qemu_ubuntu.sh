#!/bin/bash
#
# PowerFS 内核调试环境 - Ubuntu VM 启动脚本
# 使用磁盘镜像方式启动，节省内存
#

set -e

# 配置
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"

KERNEL="${OUTPUT_DIR}/bzImage"
DISK="${OUTPUT_DIR}/ubuntu_disk.img"

# QEMU 参数
QEMU_MEMORY="2G"
QEMU_CPUS="2"
SSH_PORT=2222
HOST_IP="127.0.0.1"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# 检查文件是否存在
if [ ! -f "${KERNEL}" ]; then
    log_error "内核镜像不存在: ${KERNEL}"
    exit 1
fi

if [ ! -f "${DISK}" ]; then
    log_error "磁盘镜像不存在: ${DISK}"
    exit 1
fi

# 检查 KVM 支持
KVM_ENABLED=""
if [ -e /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    KVM_ENABLED="-enable-kvm"
    log_info "检测到 KVM 支持，将使用硬件加速"
else
    log_warn "未检测到 KVM，将使用软件模拟 (较慢)"
fi

# 检查是否有已运行的 QEMU 实例
if pgrep -f "qemu-system.*ubuntu" > /dev/null 2>&1; then
    log_warn "检测到已运行的 QEMU 实例，是否终止? (y/N)"
    read -r answer
    if [[ "$answer" =~ ^[Yy] ]]; then
        pkill -f "qemu-system.*ubuntu" 2>/dev/null || true
        sleep 2
    else
        exit 1
    fi
fi

# 显示配置
echo ""
echo "========================================"
echo "  PowerFS Ubuntu 调试 VM"
echo "========================================"
echo ""
echo "配置信息:"
echo "  内核: ${KERNEL}"
echo "  磁盘: ${DISK}"
echo "  内存: ${QEMU_MEMORY}"
echo "  CPU: ${QEMU_CPUS}"
echo "  KVM: ${KVM_ENABLED:-disabled}"
echo ""
echo "网络配置:"
echo "  模式: 用户模式 (User Mode)"
echo "  SSH 端口转发: localhost:${SSH_PORT} -> VM:22"
echo ""
echo "SSH 连接命令 (在另一个终端):"
echo "  ssh -p ${SSH_PORT} -i ~/.ssh/id_rsa root@${HOST_IP}"
echo "  ssh -p ${SSH_PORT} portion@${HOST_IP}  (密码登录)"
echo ""

# 启动 QEMU（使用磁盘镜像作为根文件系统）
log_info "启动 QEMU..."

exec qemu-system-x86_64 \
    ${KVM_ENABLED} \
    -m "${QEMU_MEMORY}" \
    -smp "${QEMU_CPUS}" \
    -kernel "${KERNEL}" \
    -append "root=/dev/vda rw console=ttyS0,115200 net.ifnames=0 init=/init loglevel=3" \
    -net nic,model=virtio-net-pci \
    -net user,hostfwd=tcp::${SSH_PORT}-:22 \
    -drive "file=${DISK},format=raw,if=virtio" \
    -nographic \
    -serial mon:stdio \
    -no-reboot
