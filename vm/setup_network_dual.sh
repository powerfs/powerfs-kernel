#!/bin/bash
# PowerFS QEMU 双 VM 网络配置脚本
# 配置 tap0 (VM1, 172.30.0.100) 和 tap1 (VM2, 172.30.0.101),
# 均连接到 Docker 网桥 powerfs-br0, 实现双 VM 与容器网络互通.
#
# 与 setup_network.sh (单 VM, 仅 tap0) 的关系:
#   双 VM CI (kernel-multi-client.yml) 的 Start VM1/VM2 步骤使用
#   run_qemu_kernel_ssh.sh (ifname=tap0) 和 run_qemu_vm2.sh (ifname=tap1),
#   两个 tap 设备都必须提前存在 (netdev script=no, QEMU 不会自行创建).

set -e

# 检查 root 权限
if [ "$EUID" -ne 0 ]; then
    echo "错误: 此脚本需要 root 权限"
    echo "请使用: sudo $0"
    exit 1
fi

# Docker compose 已通过 driver_opts 指定固定网桥名 powerfs-br0,
# 不再需要动态查找 (旧方案每次 docker compose down/up 后 bridge ID 会变).
DOCKER_BRIDGE="${DOCKER_BRIDGE:-powerfs-br0}"

# 如果 powerfs-br0 不存在, 回退到动态查找 (兼容旧配置)
if ! ip link show ${DOCKER_BRIDGE} &>/dev/null; then
    echo "[INFO] ${DOCKER_BRIDGE} 不存在, 回退到动态查找..."
    # 方法1: 通过 docker network inspect 查找
    DOCKER_BRIDGE=$(docker network inspect docker_powerfs-network \
        --format '{{range .Options}}{{.}}{{end}}' 2>/dev/null | grep -oE 'br-[0-9a-f]{12}' | head -1)
    # 方法2: 通过 network ID 推导
    if [ -z "${DOCKER_BRIDGE}" ]; then
        DOCKER_BRIDGE=$(docker network inspect docker_powerfs-network \
            --format '{{.Id}}' 2>/dev/null | head -c 12 | xargs -I{} echo "br-{}")
    fi
    # 方法3: 直接从 ip link 查找 br- 开头且 UP 的网桥
    if [ -z "${DOCKER_BRIDGE}" ] || ! ip link show ${DOCKER_BRIDGE} &>/dev/null; then
        DOCKER_BRIDGE=$(ip -o link show type bridge | awk -F': ' '{print $2}' | grep '^br-' | head -1)
    fi
fi

# 检查 Docker 网桥是否存在
if [ -z "${DOCKER_BRIDGE}" ] || ! ip link show ${DOCKER_BRIDGE} &>/dev/null; then
    echo "错误: 未找到 Docker powerfs-network 网桥"
    echo "请先启动 Docker 容器 (qemuctl.sh service start)"
    echo ""
    echo "可用网桥:"
    ip -o link show type bridge 2>/dev/null | awk -F': ' '{print "  - " $2}'
    exit 1
fi

echo "[INFO] 使用 Docker 网桥: ${DOCKER_BRIDGE}"

# 依次配置 tap0 (VM1) 和 tap1 (VM2)
for TAP in tap0 tap1; do
    echo ""
    echo "=== 配置 ${TAP} ==="
    # 如果 TAP 设备已存在，先删除 (幂等, 允许重复执行)
    if ip link show ${TAP} &>/dev/null; then
        echo "  移除旧的 TAP 设备 ${TAP}..."
        ip link delete ${TAP}
    fi
    ip tuntap add dev ${TAP} mode tap
    ip link set ${TAP} master ${DOCKER_BRIDGE}
    ip link set ${TAP} up
    echo "  [OK] ${TAP} 已创建并连接到 ${DOCKER_BRIDGE}"
done

echo ""
echo "========================================"
echo "  双 VM 网络配置完成!"
echo "========================================"
echo ""
echo "VM 网络配置 (init 脚本内设置):"
echo "  VM1 (tap0): 172.30.0.100/16"
echo "  VM2 (tap1): 172.30.0.101/16"
echo "  网关:       172.30.0.1"
echo "  Docker 网桥: ${DOCKER_BRIDGE}"
echo ""

# 显示 TAP 设备状态
echo "TAP 设备状态:"
ip -o link show tap0
ip -o link show tap1
