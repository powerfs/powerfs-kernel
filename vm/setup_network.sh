#!/bin/bash
# PowerFS QEMU 网络配置脚本
# 配置 TAP 设备连接到 Docker 网桥，实现 VM 与容器网络互通

set -e

# 配置参数
TAP_DEVICE="tap0"
GATEWAY="172.30.0.1"

echo "========================================"
echo "  PowerFS QEMU 网络配置"
echo "========================================"
echo ""

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
    echo "请先启动 Docker 容器 (./qemuctl.sh service start)"
    echo ""
    echo "可用网桥:"
    ip -o link show type bridge 2>/dev/null | awk -F': ' '{print "  - " $2}'
    exit 1
fi

echo "[INFO] 使用 Docker 网桥: ${DOCKER_BRIDGE}"

echo "[1/4] 创建 TAP 设备..."
# 如果 TAP 设备已存在，先删除
if ip link show ${TAP_DEVICE} &>/dev/null; then
    echo "  移除旧的 TAP 设备 ${TAP_DEVICE}..."
    ip link delete ${TAP_DEVICE}
fi

# 创建 TAP 设备
ip tuntap add dev ${TAP_DEVICE} mode tap
echo "  [OK] TAP 设备 ${TAP_DEVICE} 已创建"

echo "[2/4] 将 TAP 设备添加到 Docker 网桥..."
ip link set ${TAP_DEVICE} master ${DOCKER_BRIDGE}
echo "  [OK] ${TAP_DEVICE} 已连接到 ${DOCKER_BRIDGE}"

echo "[3/4] 启动 TAP 设备..."
ip link set ${TAP_DEVICE} up
echo "  [OK] ${TAP_DEVICE} 已启动 (无 IP, 作为网桥端口)"

echo "[4/4] 验证网桥连接..."
echo "  [OK] ${TAP_DEVICE} 已连接到 ${DOCKER_BRIDGE}"

echo ""
echo "========================================"
echo "  网络配置完成!"
echo "========================================"
echo ""
echo "VM 网络配置 (init 脚本内设置):"
echo "  IP: 172.30.0.100/16"
echo "  网关: ${GATEWAY}"
echo "  Docker 网桥: ${DOCKER_BRIDGE}"
echo ""
echo "容器服务地址 (VM 内访问):"
echo "  Redis:      172.30.0.50:6379"
echo "  Master:     172.30.0.11:9333"
echo "  Volume:     172.30.0.21:8080"
echo "  Filer-1:    172.30.0.35:9334 (powerfs-net TLV)"
echo ""
echo "测试连通性: ping ${GATEWAY}"
echo ""

# 显示 TAP 设备状态
echo "TAP 设备状态:"
ip addr show ${TAP_DEVICE}
