#!/bin/bash
# PowerFS QEMU 统一管理脚本
#
# 用法: ./qemuctl.sh <command> [options]
#
# 命令分组:
#   [构建]
#     build         编译 powerfs.ko + 构建 initramfs (不重新编译内核)
#     build-all     编译内核 + powerfs.ko + initramfs (全量构建)
#     deploy        一键重新构建 powerfs.ko 并重启 QEMU (常用迭代)
#     hotdeploy     快速热部署: 编译 ko → 9p 共享 → SSH 热加载 (不重启 QEMU)
#
#   [QEMU 生命周期]
#     start         启动 QEMU 虚拟机 (后台, 日志重定向到 qemu.log, 正常测试用)
#     debug         启动 QEMU 调试模式 (后台 + 调试参数 + 实时 tail serial 日志)
#                   (loglevel=7 slub_debug=FZP rcu_stall=on, stall/oops 排障用)
#     stop          停止 QEMU 虚拟机
#     restart       重启 QEMU (stop + start)
#     status        查看 QEMU 运行状态
#
#   [VM 操作]
#     ssh           SSH 登录到 VM
#     mount         在 VM 内挂载 PowerFS
#     umount        在 VM 内卸载 PowerFS
#     exec <cmd>    在 VM 内执行命令
#     log [grep]    查看 VM dmesg 日志 (可选 grep 关键词, 需 SSH 通)
#     monitor [grep] 实时监控 VM dmesg (tail -f, Ctrl-C 退出, 需 SSH 通)
#     serial [grep] [N]  读宿主机 qemu.log 最后 N 行 (VM 卡死也能用, 默认 100)
#     serial-tail [grep] 实时 tail 宿主机 qemu.log (VM 卡死也能用)
#
#   [Docker 服务]
#     service start     启动 master/volume/filer 服务
#     service stop      停止服务
#     service restart   重启服务
#     service status    查看服务状态
#     service log <n>   查看服务日志 (master-1/volume-1/filer-1 等)
#
#   [环境]
#     clean         清理环境 (停止 QEMU + 清理 initramfs 构建目录)
#     clean-all     清理全部 (QEMU + initramfs + docker volumes)
#     net           配置 TAP 网络 (调用 setup_network.sh)
#
# 示例:
#   ./qemuctl.sh service start && ./qemuctl.sh deploy
#   ./qemuctl.sh ssh
#   ./qemuctl.sh log powerfs
#   ./qemuctl.sh monitor powerfs
#   ./qemuctl.sh exec "ls -la /mnt/powerfs/"
#   ./qemuctl.sh service log filer-1

set -u

# ============================================================
# 路径配置
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"
POWERFS_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
KERNEL_SOURCE="/home/portion/powerfs/ubuntu-linux-git"
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"
DOCKER_DIR="${POWERFS_ROOT}/docker"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
QEMU_DISK="${OUTPUT_DIR}/qemu_disk.img"
QEMU_LOG="${OUTPUT_DIR}/qemu.log"
QEMU_PID_FILE="${OUTPUT_DIR}/qemu.pid"

# SSH 配置
SSH_PORT="2223"
SSH_USER="root"
SSH_PASS="powerfs"

# 网络配置
TAP_DEVICE="tap0"
VM_IP="172.30.0.100"
GATEWAY="172.30.0.1"

# PowerFS 后端服务地址 — 只需 Master 地址 (3 个 Raft 节点)
# Filer/Volume 地址全部通过 Master 动态发现 (GetTopology / ListFilers)
POWERFS_MASTER_ADDR="${POWERFS_MASTER_ADDR:-172.30.0.11,172.30.0.12,172.30.0.13}"
POWERFS_MASTER_PORT="${POWERFS_MASTER_PORT:-9334}"

# RDMA 模式开关 (由 service --rdma 设置)
# 启用后:
#   - 使用独立 docker-compose.rdma.yml (非覆盖, host 网络模式)
#   - 先停止 TCP 服务再启动 RDMA 服务 (避免端口冲突)
#   - 服务地址改为 172.30.0.1 (宿主机 powerfs-br0 IP)
USE_RDMA="${USE_RDMA:-0}"

# RDMA 模式下的 Master 地址 (host 网络, 单节点)
if [ "${USE_RDMA}" = "1" ]; then
    POWERFS_MASTER_ADDR="172.30.0.1"
    POWERFS_MASTER_PORT="9334"
fi

# VFIO RDMA 直通开关 (用于内核态 powerfs.ko 的 RDMA 传输测试).
# =1 时将 host 上的 mlx5 VF (VFIO_BDF) 通过 vfio-pci 直通给 QEMU VM.
# 前置条件:
#   - BIOS 开启 VT-d, 内核 cmdline intel_iommu=on iommu=pt
#   - SR-IOV 已启用: echo N > /sys/class/infiniband/mlx5_X/device/sriov_numvfs
#   - VF 已解绑 mlx5_core 并绑定 vfio-pci 驱动
# 用法: USE_VFIO_RDMA=1 VFIO_BDF=0000:a0:02.3 ./qemuctl.sh start
USE_VFIO_RDMA="${USE_VFIO_RDMA:-0}"
# VF 设备 BDF (默认取 host 上第一个绑定 vfio-pci 的 ConnectX VF).
VFIO_BDF="${VFIO_BDF:-}"
# 当 USE_VFIO_RDMA=1 且未指定时, 自动探测 host 上 mlx5 VF 绑定 vfio-pci 的设备.
# 过滤 mlx5 vendor (15b3) 避免误选其他 vfio-pci 设备 (如 GPU/VF).
if [ "${USE_VFIO_RDMA}" = "1" ] && [ -z "${VFIO_BDF}" ]; then
    VFIO_BDF=""
    for dev in /sys/bus/pci/devices/*; do
        [ -L "${dev}/driver" ] || continue
        readlink "${dev}/driver" 2>/dev/null | grep -q "vfio-pci" || continue
        # 仅选 ConnectX (Mellanox vendor 0x15b3) 的 VF
        _vendor=$(cat "${dev}/vendor" 2>/dev/null)
        [ "${_vendor}" = "0x15b3" ] || continue
        # 必须是 VF (有 physfn 符号链接指向父 PF), 跳过 PF 自身
        [ -L "${dev}/physfn" ] || continue
        VFIO_BDF="$(basename "${dev}")"
        break
    done
    # 规范 BDF 前缀: domain:bus:dev.func (8 位 domain + 冒号)
    if [ -n "${VFIO_BDF}" ] && [ "${VFIO_BDF:0:5}" != "0000:" ]; then
        VFIO_BDF="0000:${VFIO_BDF}"
    fi
fi

# QEMU 参数
MEM_SIZE="4096"
CPU_CORES="4"

# Host 共享目录 (通过 9p virtfs 挂载到 VM /mnt/host)
# 用于快速部署 powerfs.ko 和共享测试脚本, 避免 rebuild initramfs
SHARE_DIR="${POWERFS_ROOT}/kernel/vm/share"
SHARE_TAG="hostshare"

# ============================================================
# 颜色输出
# ============================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }
title() { echo -e "\n${BLUE}=== $* ===${NC}"; }
step()  { echo -e "${CYAN}[STEP]${NC} $*"; }

# ============================================================
# Docker Compose 文件列表辅助函数
# 根据 USE_RDMA 开关返回 -f 参数列表
# RDMA 模式使用独立 docker-compose.rdma.yml (非覆盖)
# ============================================================
compose_files() {
    if [ "${USE_RDMA}" = "1" ]; then
        echo "-f ${DOCKER_DIR}/docker-compose.rdma.yml"
    else
        echo "-f ${DOCKER_DIR}/docker-compose.yml"
    fi
}

# ============================================================
# SSH 辅助函数
# ============================================================
ssh_vm() {
    ssh \
        -o StrictHostKeyChecking=no \
        -o ConnectTimeout=5 \
        -o PreferredAuthentications=publickey \
        -o PubkeyAuthentication=yes \
        -p "${SSH_PORT}" \
        "${SSH_USER}@localhost" "$@"
}

# 检查 QEMU 是否运行
is_qemu_running() {
    pgrep -f "qemu-system-x86_64.*${KERNEL_IMAGE}" >/dev/null 2>&1
}

# 获取 QEMU PID
get_qemu_pid() {
    pgrep -f "qemu-system-x86_64.*${KERNEL_IMAGE}" | head -1
}

# ============================================================
# 命令: build
# ============================================================
cmd_build() {
    title "编译 powerfs.ko"
    cd "${POWERFS_MOD_DIR}"
    make clean 2>/dev/null || true
    make -j$(nproc) 2>&1 | tail -5
    if [ ! -f "${POWERFS_MOD_DIR}/powerfs.ko" ]; then
        error "powerfs.ko 编译失败"
        exit 1
    fi
    info "powerfs.ko 编译成功 ($(ls -la powerfs.ko | awk '{print $5}') bytes)"

    title "构建 initramfs"
    cd "${SCRIPT_DIR}"
    bash build_initramfs.sh 2>&1 | tail -10
    info "initramfs 构建完成"
}

# ============================================================
# 命令: build-all
# ============================================================
cmd_build_all() {
    title "编译内核"
    cd "${SCRIPT_DIR}"
    bash build_kernel.sh 2>&1 | tail -10
    info "内核编译完成"

    cmd_build
}

# ============================================================
# 命令: deploy (一键重新构建并重启 QEMU)
# ============================================================
cmd_deploy() {
    title "一键部署: 重新构建 powerfs.ko + 重启 QEMU"
    step "1/3 重新构建 powerfs.ko + initramfs..."
    cmd_build

    step "2/3 停止旧 QEMU..."
    cmd_stop 2>/dev/null

    step "3/3 启动新 QEMU..."
    cmd_start
}

# ============================================================
# 命令: hotdeploy (快速部署: 编译 ko + 9p 共享 + SSH 热加载)
# 不重建 initramfs, 不重启 QEMU, 仅编译 powerfs.ko 并通过 9p + SSH 热加载
# 前提: QEMU 已启动且 9p 共享目录已挂载 (/mnt/host)
# ============================================================
cmd_hotdeploy() {
    title "热部署: 编译 powerfs.ko → 9p 共享 → SSH 热加载"

    if ! is_qemu_running; then
        error "QEMU 未运行, 请先启动: ./qemuctl.sh start"
        return 1
    fi

    step "1/4 编译 powerfs.ko..."
    cd "${POWERFS_MOD_DIR}" || { error "无法进入 ${POWERFS_MOD_DIR}"; return 1; }
    if ! make -j"$(nproc)" 2>&1 | tail -5; then
        error "编译失败"
        return 1
    fi
    if [ ! -f powerfs.ko ]; then
        error "powerfs.ko 未生成"
        return 1
    fi
    info "编译成功: $(ls -lh powerfs.ko | awk '{print $5}')"

    step "2/4 复制到 9p 共享目录..."
    mkdir -p "${SHARE_DIR}"
    cp powerfs.ko "${SHARE_DIR}/powerfs.ko"
    info "已复制到 ${SHARE_DIR}/powerfs.ko"

    step "3/4 SSH 热加载 (umount → rmmod → insmod → mount)..."
    local cmdline master_addr master_port shard_count cert_ca cert_crt cert_key
    cmdline=$(ssh_vm "cat /proc/cmdline" 2>/dev/null)
    master_addr=$(echo "$cmdline" | grep -o 'powerfs_master_addr=[^ ]*' | head -1 | cut -d= -f2)
    master_port=$(echo "$cmdline" | grep -o 'powerfs_master_port=[^ ]*' | head -1 | cut -d= -f2)
    shard_count=$(echo "$cmdline" | grep -o 'shard_count=[^ ]*' | head -1 | cut -d= -f2)
    master_addr=${master_addr:-${POWERFS_MASTER_ADDR}}
    master_port=${master_port:-${POWERFS_MASTER_PORT}}
    shard_count=${shard_count:-3}
    # 首选 initramfs 打包的 /etc/powerfs/ (VM 重启后仍存在), 兼容 /tmp/
    cert_ca="${POWERFS_CA_CRT:-/etc/powerfs/ca.crt}"
    cert_crt="${POWERFS_CLIENT_CRT:-/etc/powerfs/kernel-client-1.crt}"
    cert_key="${POWERFS_CLIENT_KEY:-/etc/powerfs/kernel-client-1.key}"

    # 执行热加载 (在 VM 内部完成 umount/rmmod/insmod/mount)
    info "执行: umount → rmmod → insmod → mount"
    local hotload_script mount_opts
    mount_opts="master_addr=${master_addr},master_port=${master_port},shard_count=${shard_count},ca_crt=${cert_ca},client_crt=${cert_crt},client_key=${cert_key}"
    hotload_script=$(cat <<HOTLOAD
set +e
# 1. umount powerfs (如果已挂载)
if mount | grep -q 'on /mnt/powerfs type powerfs'; then
    umount /mnt/powerfs 2>/dev/null || umount -l /mnt/powerfs 2>/dev/null
    sleep 1
fi
# 2. rmmod (如果已加载)
if lsmod | grep -q powerfs; then
    rmmod powerfs
    sleep 1
fi
# 3. insmod from 9p share (module_param 已移除, 所有参数通过 mount -o 传递)
insmod /mnt/host/powerfs.ko
insmod_ret=\$?
if [ \$insmod_ret -ne 0 ] && [ \$insmod_ret -ne 17 ]; then
    echo "HOTDEPLOY_FAIL: insmod failed ret=\$insmod_ret"
    dmesg | tail -10
    exit \$insmod_ret
fi
sleep 1
# 4. mount (所有参数通过 -o 传递: master/证书/shard_count)
mkdir -p /mnt/powerfs
mount -t powerfs -o "${mount_opts}" none /mnt/powerfs
mount_ret=\$?
sleep 2
# 5. verify
if mount | grep -q 'on /mnt/powerfs type powerfs'; then
    echo "HOTDEPLOY_OK"
else
    echo "HOTDEPLOY_FAIL: mount failed ret=\$mount_ret"
    echo "opts: ${mount_opts}"
    dmesg | tail -20
fi
HOTLOAD
)

    local result
    result=$(ssh_vm "$hotload_script" 2>&1)
    echo "$result" | grep -v "^$"

    step "4/4 验证..."
    if echo "$result" | grep -q "HOTDEPLOY_OK"; then
        info "热部署成功! powerfs.ko 已更新并重新挂载"
        echo "  模块: $(ssh_vm 'lsmod | grep powerfs' 2>/dev/null)"
        echo "  挂载: $(ssh_vm 'mount | grep powerfs' 2>/dev/null)"
    else
        error "热部署失败"
        echo "$result" | tail -15
        return 1
    fi
}

# ============================================================
# 命令: start
# ============================================================
cmd_start() {
    title "启动 QEMU 虚拟机"

    # 检查是否已有 QEMU 运行
    if is_qemu_running; then
        warn "QEMU 已在运行 (PID: $(get_qemu_pid))"
        echo "  如需重启: ./qemuctl.sh restart"
        echo "  如需重新部署: ./qemuctl.sh deploy"
        return 0
    fi

    # 检查文件
    if [ ! -f "${KERNEL_IMAGE}" ]; then
        error "内核镜像不存在: ${KERNEL_IMAGE}"
        echo "  请先运行: ./qemuctl.sh build-all"
        exit 1
    fi
    if [ ! -f "${INITRAMFS}" ]; then
        error "initramfs 不存在: ${INITRAMFS}"
        echo "  请先运行: ./qemuctl.sh build"
        exit 1
    fi

    # 检查 TAP 设备
    if ! ip link show ${TAP_DEVICE} &>/dev/null; then
        warn "TAP 设备 ${TAP_DEVICE} 不存在，尝试创建..."
        sudo bash "${SCRIPT_DIR}/setup_network.sh" 2>&1 || true
        if ! ip link show ${TAP_DEVICE} &>/dev/null; then
            error "TAP 设备创建失败"
            echo "  请手动运行: sudo ./setup_network.sh"
            exit 1
        fi
    else
        # TAP 已存在, 验证是否仍在正确的 bridge 上
        # (docker compose down/up 后旧 bridge 可能已消失)
        local _br
        _br=$(ip link show ${TAP_DEVICE} 2>/dev/null | grep -oE 'master [^ ]+' | awk '{print $2}')
        if [ -n "${_br}" ] && ! ip link show "${_br}" &>/dev/null 2>&1; then
            warn "TAP 设备 ${TAP_DEVICE} 连接的网桥 ${_br} 已消失，重新配置..."
            sudo bash "${SCRIPT_DIR}/setup_network.sh" 2>&1 || true
        elif [ -z "${_br}" ]; then
            warn "TAP 设备 ${TAP_DEVICE} 未连接到任何网桥，重新配置..."
            sudo bash "${SCRIPT_DIR}/setup_network.sh" 2>&1 || true
        fi
    fi

    # 检查 Docker 服务状态 (QEMU 需要 master/volume/filer 可达)
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -qE "master-1|volume-1|filer-1"; then
        warn "Docker 服务似乎未启动 (未检测到 master-1/volume-1/filer-1)"
        echo "  内核客户端将无法挂载 PowerFS"
        echo "  建议运行: ./qemuctl.sh service start"
        echo ""
    fi

    # 创建虚拟磁盘 (首次启动)
    if [ ! -f "${QEMU_DISK}" ]; then
        info "创建虚拟磁盘 (1GB)..."
        qemu-img create -f qcow2 "${QEMU_DISK}" 1G >/dev/null
    fi

    # Rotate qemu.log: 保留上一份 .1, 清空当前日志, 防止 append 模式无限增长.
    # panic=-1 自动重启会追加新 boot 段, 多次压力测试后文件可达 GB 级.
    if [ -f "${QEMU_LOG}" ]; then
        local log_size=$(stat -c%s "${QEMU_LOG}" 2>/dev/null || echo 0)
        if [ "${log_size}" -gt 10485760 ]; then  # >10MB
            mv "${QEMU_LOG}" "${QEMU_LOG}.1"
            info "qemu.log rotated (was $((log_size/1024/1024))MB -> .1 backup)"
        else
            : > "${QEMU_LOG}"
            info "qemu.log truncated (was $((log_size/1024))KB)"
        fi
    fi

    # 检查 KVM 支持
    local kvm_flag=""
    if [ -e /dev/kvm ] && sudo test -r /dev/kvm && sudo test -w /dev/kvm; then
        kvm_flag="-enable-kvm"
        info "KVM 加速已启用"
    else
        warn "KVM 不可用, 使用软件模拟 (较慢)"
    fi

    # 构建内核命令行参数
    # loglevel=4 (WARNING): 捕获所有 lockup 警告 (RCU stall=CRIT, hung task=ERR,
    #   workqueue lockup=WARN, softlockup=EMERG). loglevel=3 会漏掉 WARN 级别.
    # 不使用 quiet: quiet 会抑制启动期日志, 影响早期 lockup 定位.
    # softlockup_thresh=10: CPU 自旋 >10s 触发 softlockup 警告 + panic
    # hung_task_timeout_secs=60: D 状态 >60s 触发 (默认 120s 太慢)
    # rcupdate.rcu_cpu_stall_timeout=21: RCU stall 21s 检测
    # wq_watchdog_thresh=30: workqueue 卡住 30s 检测
    local cmdline="console=ttyS0 root=/dev/ram0 rw init=/init loglevel=4"
    cmdline="${cmdline} net.ifnames=0 biosdevname=0"
    cmdline="${cmdline} powerfs_master_addr=${POWERFS_MASTER_ADDR}"
    cmdline="${cmdline} powerfs_master_port=${POWERFS_MASTER_PORT}"
    cmdline="${cmdline} kasan=off"
    cmdline="${cmdline} softlockup_thresh=10"
    cmdline="${cmdline} hung_task_timeout_secs=60"
    cmdline="${cmdline} rcupdate.rcu_cpu_stall_timeout=21"
    cmdline="${cmdline} wq_watchdog_thresh=30"
    cmdline="${cmdline} panic_on_oops=1"
    cmdline="${cmdline} softlockup_panic=1"
    cmdline="${cmdline} hardlockup_panic=1"
    cmdline="${cmdline} hung_task_panic=1"
    cmdline="${cmdline} rcupdate.rcu_cpu_stall_panic=1"
    cmdline="${cmdline} panic=-1"

    # ===== VFIO RDMA 直通 =====
    local vfio_flag=""
    if [ "${USE_VFIO_RDMA}" = "1" ]; then
        if [ -z "${VFIO_BDF}" ]; then
            error "USE_VFIO_RDMA=1 但未找到可用的 vfio-pci 绑定的 VF"
            echo "  请确保: echo 1 > /sys/class/infiniband/mlx5_<X>/device/sriov_numvfs"
            echo "          然后将 VF BDF 解绑 mlx5_core 并绑定 vfio-pci."
            exit 1
        fi
        # 校验 BDF 路径存在且驱动为 vfio-pci.
        if [ ! -e "/sys/bus/pci/devices/${VFIO_BDF}" ]; then
            error "VFIO BDF 不存在: /sys/bus/pci/devices/${VFIO_BDF}"
            exit 1
        fi
        local _drv
        _drv="$(basename "$(readlink /sys/bus/pci/devices/${VFIO_BDF}/driver 2>/dev/null)" 2>/dev/null)"
        if [ "${_drv}" != "vfio-pci" ]; then
            error "VFIO BDF ${VFIO_BDF} 的驱动是 '${_drv}', 不是 vfio-pci"
            exit 1
        fi
        # 要求 vfio-pci 设备组 /dev/vfio/<iommu_group> 可访问.
        local _iommu_group
        _iommu_group="$(basename "$(readlink /sys/bus/pci/devices/${VFIO_BDF}/iommu_group 2>/dev/null)" 2>/dev/null)"
        if [ -z "${_iommu_group}" ] || [ ! -c "/dev/vfio/${_iommu_group}" ]; then
            error "VFIO ${VFIO_BDF} 没有可用的 /dev/vfio/<group> (IOMMU group: ${_iommu_group:-none})"
            exit 1
        fi
        if ! sudo test -r "/dev/vfio/${_iommu_group}" || ! sudo test -w "/dev/vfio/${_iommu_group}"; then
            sudo chmod 666 "/dev/vfio/${_iommu_group}" 2>/dev/null || true
        fi
        vfio_flag="-device vfio-pci,host=${VFIO_BDF}"
        info "VFIO RDMA 直通: ${VFIO_BDF} (iommu_group=${_iommu_group})"
    fi

    info "启动 QEMU (后台运行, 日志: ${QEMU_LOG})"
    info "  内核:    ${KERNEL_IMAGE}"
    info "  Initramfs: ${INITRAMFS}"
    info "  内存/CPU: ${MEM_SIZE}MB / ${CPU_CORES} 核"
    info "  SSH:     localhost:${SSH_PORT} (user: ${SSH_USER}, pass: ${SSH_PASS})"
    info "  VM IP:   ${VM_IP} (通过 TAP 访问 Docker 网络)"

    # 后台启动 QEMU, 日志重定向到文件
    # 使用 nohup + disown 确保 QEMU 不依赖当前终端
    sudo nohup qemu-system-x86_64 \
        ${kvm_flag} \
        -m "${MEM_SIZE}" \
        -smp "${CPU_CORES}" \
        -kernel "${KERNEL_IMAGE}" \
        -initrd "${INITRAMFS}" \
        -append "${cmdline}" \
        -drive file="${QEMU_DISK}",format=qcow2,if=virtio \
        -netdev tap,id=net0,ifname=${TAP_DEVICE},script=no,downscript=no \
        -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
        -netdev user,id=net1,hostfwd=tcp::${SSH_PORT}-:22 \
        -device e1000,netdev=net1,mac=52:54:00:12:34:57 \
        -virtfs local,path=${SHARE_DIR},mount_tag=${SHARE_TAG},security_model=mapped-xattr,id=${SHARE_TAG} \
        ${vfio_flag} \
        -nographic \
        -serial file:"${QEMU_LOG}" \
        -monitor none \
        -display none \
        > "${QEMU_LOG}.stdout" 2>&1 &

    local sudo_pid=$!
    disown ${sudo_pid} 2>/dev/null || true

    # 记录 PID (sudo 的 PID, 不是 QEMU 的, 但用于跟踪)
    echo "${sudo_pid}" > "${QEMU_PID_FILE}"

    # 等待 QEMU 进程出现
    sleep 2
    if ! is_qemu_running; then
        error "QEMU 启动失败, 查看日志: ${QEMU_LOG}"
        echo "  最后 10 行日志:"
        tail -10 "${QEMU_LOG}" 2>/dev/null || echo "  (无日志)"
        return 1
    fi

    # 等待 VM 启动 (SSH 就绪)
    info "等待 VM 启动 (SSH 就绪)..."
    local tries=0
    while [ ${tries} -lt 30 ]; do
        sleep 2
        if ssh_vm "echo OK" 2>/dev/null | grep -q "OK"; then
            info "VM 已启动, SSH 就绪"
            echo ""
            echo "  SSH 登录:    ./qemuctl.sh ssh"
            echo "  挂载 PFS:    ./qemuctl.sh mount"
            echo "  查看日志:    ./qemuctl.sh log"
            echo "  实时监控:    ./qemuctl.sh monitor powerfs"
            echo "  QEMU 日志:   ${QEMU_LOG}"
            return 0
        fi
        tries=$((tries + 1))
        printf "."
    done
    echo ""
    error "VM 启动超时 (60s), 请检查 QEMU 日志"
    echo "  QEMU 日志: ${QEMU_LOG}"
    echo "  最后 20 行:"
    tail -20 "${QEMU_LOG}" 2>/dev/null
    return 1
}

# ============================================================
# 命令: debug (后台启动 + 调试参数 + 实时 tail 日志)
#
# 与 start 的区别:
#   - 调试内核参数: loglevel=7, slub_debug=FZP, rcupdate.rcu_cpu_stall_suppress=0,
#     printk.time=1 (stall/oops 日志全开, lockup 检测与 start 一致)
#   - 启动后自动 tail -f qemu.log, 实时观察 serial 输出 (RCU stall/workqueue lockup 等)
#   - Ctrl-C 仅退出 tail, QEMU 继续后台运行 (可用 stop 停止)
#
# 适用场景: RCU stall / workqueue lockup / dentry 哈希链损坏等需要实时 serial 日志的调试.
# 注意: start 用 loglevel=4 + kasan=off + softlockup/wq_watchdog 全开, 适合正常测试;
#       debug 额外开 loglevel=7 + slub_debug=FZP + printk.time, 适合深度排障.
# ============================================================
cmd_debug() {
    title "启动 QEMU 虚拟机 (调试模式: 后台 + tail 日志)"

    # 检查是否已有 QEMU 运行
    if is_qemu_running; then
        warn "QEMU 已在运行 (PID: $(get_qemu_pid))"
        echo "  如需重启: ./qemuctl.sh restart && ./qemuctl.sh debug"
        echo "  如需重新部署: ./qemuctl.sh deploy && ./qemuctl.sh debug"
        echo "  直接 tail 现有日志: tail -f ${QEMU_LOG}"
        return 0
    fi

    # 检查文件
    if [ ! -f "${KERNEL_IMAGE}" ]; then
        error "内核镜像不存在: ${KERNEL_IMAGE}"
        echo "  请先运行: ./qemuctl.sh build-all"
        exit 1
    fi
    if [ ! -f "${INITRAMFS}" ]; then
        error "initramfs 不存在: ${INITRAMFS}"
        echo "  请先运行: ./qemuctl.sh build"
        exit 1
    fi

    # 检查 TAP 设备
    if ! ip link show ${TAP_DEVICE} &>/dev/null; then
        warn "TAP 设备 ${TAP_DEVICE} 不存在，尝试创建..."
        sudo bash "${SCRIPT_DIR}/setup_network.sh" 2>&1 || true
        if ! ip link show ${TAP_DEVICE} &>/dev/null; then
            error "TAP 设备创建失败"
            echo "  请手动运行: sudo ./setup_network.sh"
            exit 1
        fi
    else
        # TAP 已存在, 验证是否仍在正确的 bridge 上
        # (docker compose down/up 后旧 bridge 可能已消失)
        local _br
        _br=$(ip link show ${TAP_DEVICE} 2>/dev/null | grep -oE 'master [^ ]+' | awk '{print $2}')
        if [ -n "${_br}" ] && ! ip link show "${_br}" &>/dev/null 2>&1; then
            warn "TAP 设备 ${TAP_DEVICE} 连接的网桥 ${_br} 已消失，重新配置..."
            sudo bash "${SCRIPT_DIR}/setup_network.sh" 2>&1 || true
        elif [ -z "${_br}" ]; then
            warn "TAP 设备 ${TAP_DEVICE} 未连接到任何网桥，重新配置..."
            sudo bash "${SCRIPT_DIR}/setup_network.sh" 2>&1 || true
        fi
    fi

    # 检查 Docker 服务状态
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -qE "master-1|volume-1|filer-1"; then
        warn "Docker 服务似乎未启动 (未检测到 master-1/volume-1/filer-1)"
        echo "  内核客户端将无法挂载 PowerFS"
        echo "  建议运行: ./qemuctl.sh service start"
        echo ""
    fi

    # 创建虚拟磁盘 (首次启动)
    if [ ! -f "${QEMU_DISK}" ]; then
        info "创建虚拟磁盘 (1GB)..."
        qemu-img create -f qcow2 "${QEMU_DISK}" 1G >/dev/null
    fi

    # Rotate qemu.log: 保留上一份 .1, 清空当前日志, 防止 append 模式无限增长.
    # panic=-1 自动重启会追加新 boot 段, 多次压力测试后文件可达 GB 级.
    if [ -f "${QEMU_LOG}" ]; then
        local log_size=$(stat -c%s "${QEMU_LOG}" 2>/dev/null || echo 0)
        if [ "${log_size}" -gt 10485760 ]; then  # >10MB
            mv "${QEMU_LOG}" "${QEMU_LOG}.1"
            info "qemu.log rotated (was $((log_size/1024/1024))MB -> .1 backup)"
        else
            : > "${QEMU_LOG}"
            info "qemu.log truncated (was $((log_size/1024))KB)"
        fi
    fi

    # 检查 KVM 支持
    local kvm_flag=""
    if [ -e /dev/kvm ] && sudo test -r /dev/kvm && sudo test -w /dev/kvm; then
        kvm_flag="-enable-kvm"
        info "KVM 加速已启用"
    else
        warn "KVM 不可用, 使用软件模拟 (较慢)"
    fi

    # 构建调试内核命令行参数 (全开 stall/oops/lockup 检测)
    # loglevel=7: 输出所有内核日志 (含 powerfs pr_info/pr_warn)
    # printk.time=1: 每条日志带时间戳, 方便定位 stall 触发时刻
    # slub_debug=FZP: SLUB 分配器检测 (F=Z=_redzone, P=poison, 用于 UAF/double-free 定位)
    # rcupdate.rcu_cpu_stall_suppress=0: 确保 RCU stall 警告不被抑制
    # softlockup_thresh=10: CPU 自旋 >10s 触发 (调试时仍需检测, 不能关闭)
    # hung_task_timeout_secs=60: D 状态 >60s 触发 (调试时缩短到 60s)
    # wq_watchdog_thresh=30: workqueue 卡住 30s 检测 (不再关闭, lockup 必须可见)
    # panic_on_oops=1: oops 时 panic, 确保完整调用栈在 serial 日志
    # panic=-1: 立即 panic 不重启 (保留现场供 serial 日志捕获)
    # kasan=off: KASAN 与 SLUB 调试互斥, 调试 dentry/slab 问题时优先 slub_debug
    local cmdline="console=ttyS0 root=/dev/ram0 rw init=/init loglevel=7"
    cmdline="${cmdline} net.ifnames=0 biosdevname=0"
    cmdline="${cmdline} printk.time=1"
    cmdline="${cmdline} powerfs_master_addr=${POWERFS_MASTER_ADDR}"
    cmdline="${cmdline} powerfs_master_port=${POWERFS_MASTER_PORT}"
    cmdline="${cmdline} kasan=off"
    cmdline="${cmdline} softlockup_thresh=10"
    cmdline="${cmdline} hung_task_timeout_secs=60"
    cmdline="${cmdline} rcupdate.rcu_cpu_stall_timeout=21"
    cmdline="${cmdline} rcupdate.rcu_cpu_stall_suppress=0"
    cmdline="${cmdline} wq_watchdog_thresh=30"
    cmdline="${cmdline} slub_debug=FZP"
    cmdline="${cmdline} panic_on_oops=1"
    cmdline="${cmdline} softlockup_panic=1"
    cmdline="${cmdline} hardlockup_panic=1"
    cmdline="${cmdline} hung_task_panic=1"
    cmdline="${cmdline} rcupdate.rcu_cpu_stall_panic=1"
    cmdline="${cmdline} panic=-1"

    # ===== VFIO RDMA 直通 (与 cmd_start 相同逻辑) =====
    local vfio_flag=""
    if [ "${USE_VFIO_RDMA}" = "1" ]; then
        if [ -z "${VFIO_BDF}" ]; then
            error "USE_VFIO_RDMA=1 但未找到可用的 vfio-pci 绑定的 VF"
            exit 1
        fi
        if [ ! -e "/sys/bus/pci/devices/${VFIO_BDF}" ]; then
            error "VFIO BDF 不存在: /sys/bus/pci/devices/${VFIO_BDF}"
            exit 1
        fi
        local _drv
        _drv="$(basename "$(readlink /sys/bus/pci/devices/${VFIO_BDF}/driver 2>/dev/null)" 2>/dev/null)"
        if [ "${_drv}" != "vfio-pci" ]; then
            error "VFIO BDF ${VFIO_BDF} 的驱动是 '${_drv}', 不是 vfio-pci"
            exit 1
        fi
        local _iommu_group
        _iommu_group="$(basename "$(readlink /sys/bus/pci/devices/${VFIO_BDF}/iommu_group 2>/dev/null)" 2>/dev/null)"
        if [ -z "${_iommu_group}" ] || [ ! -c "/dev/vfio/${_iommu_group}" ]; then
            error "VFIO ${VFIO_BDF} 没有可用的 /dev/vfio/<group> (IOMMU group: ${_iommu_group:-none})"
            exit 1
        fi
        if ! sudo test -r "/dev/vfio/${_iommu_group}" || ! sudo test -w "/dev/vfio/${_iommu_group}"; then
            sudo chmod 666 "/dev/vfio/${_iommu_group}" 2>/dev/null || true
        fi
        vfio_flag="-device vfio-pci,host=${VFIO_BDF}"
        info "VFIO RDMA 直通: ${VFIO_BDF} (iommu_group=${_iommu_group})"
    fi

    info "启动 QEMU (后台运行 + 调试参数, 日志: ${QEMU_LOG})"
    info "  内核:      ${KERNEL_IMAGE}"
    info "  Initramfs: ${INITRAMFS}"
    info "  内存/CPU:  ${MEM_SIZE}MB / ${CPU_CORES} 核"
    info "  SSH:       localhost:${SSH_PORT} (user: ${SSH_USER}, pass: ${SSH_PASS})"
    info "  VM IP:     ${VM_IP} (通过 TAP 访问 Docker 网络)"
    info "  调试参数:  loglevel=7 slub_debug=FZP softlockup=10s rcu_stall=21s wq_watchdog=30s panic_on_oops+lockup_panic"

    # 后台启动 QEMU, serial 日志重定向到文件 (与 start 一致)
    sudo nohup qemu-system-x86_64 \
        ${kvm_flag} \
        -m "${MEM_SIZE}" \
        -smp "${CPU_CORES}" \
        -kernel "${KERNEL_IMAGE}" \
        -initrd "${INITRAMFS}" \
        -append "${cmdline}" \
        -drive file="${QEMU_DISK}",format=qcow2,if=virtio \
        -netdev tap,id=net0,ifname=${TAP_DEVICE},script=no,downscript=no \
        -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
        -netdev user,id=net1,hostfwd=tcp::${SSH_PORT}-:22 \
        -device e1000,netdev=net1,mac=52:54:00:12:34:57 \
        -virtfs local,path=${SHARE_DIR},mount_tag=${SHARE_TAG},security_model=mapped-xattr,id=${SHARE_TAG} \
        ${vfio_flag} \
        -nographic \
        -serial file:"${QEMU_LOG}" \
        -monitor none \
        -display none \
        > "${QEMU_LOG}.stdout" 2>&1 &

    local sudo_pid=$!
    disown ${sudo_pid} 2>/dev/null || true
    echo "${sudo_pid}" > "${QEMU_PID_FILE}"

    # 等待 QEMU 进程出现
    sleep 2
    if ! is_qemu_running; then
        error "QEMU 启动失败, 查看日志: ${QEMU_LOG}"
        echo "  最后 10 行日志:"
        tail -10 "${QEMU_LOG}" 2>/dev/null || echo "  (无日志)"
        return 1
    fi

    # 等待 VM 启动 (SSH 就绪), 同时后台 tail 日志
    info "等待 VM 启动 (SSH 就绪), 同时输出 serial 日志..."
    local tries=0
    while [ ${tries} -lt 30 ]; do
        sleep 2
        # 实时输出已产生的 serial 日志 (启动阶段)
        if [ -f "${QEMU_LOG}" ]; then
            tail -n 5 "${QEMU_LOG}" 2>/dev/null
        fi
        if ssh_vm "echo OK" 2>/dev/null | grep -q "OK"; then
            info "VM 已启动, SSH 就绪"
            echo ""
            echo "  SSH 登录:    ./qemuctl.sh ssh"
            echo "  挂载 PFS:    ./qemuctl.sh mount"
            echo "  dmesg 监控:  ./qemuctl.sh monitor powerfs"
            echo "  停止 VM:     ./qemuctl.sh stop"
            echo ""
            break
        fi
        tries=$((tries + 1))
        printf "."
    done
    echo ""

    if [ ${tries} -ge 30 ]; then
        warn "VM 启动超时 (60s), 仍将进入 tail 模式"
        echo "  QEMU 日志: ${QEMU_LOG}"
    fi

    # 实时 tail serial 日志 (Ctrl-C 退出, QEMU 继续后台运行)
    title "实时 serial 日志 (Ctrl-C 退出, QEMU 继续后台运行)"
    info "退出 tail 后用 './qemuctl.sh stop' 停止 VM"
    echo ""
    # 记录 tail 起始位置, 避免重复输出启动阶段日志
    local start_line=0
    if [ -f "${QEMU_LOG}" ]; then
        start_line=$(wc -l < "${QEMU_LOG}" 2>/dev/null || echo 0)
    fi
    # tail -f 从当前末尾开始 (避免刷屏), Ctrl-C 退出
    tail -n +$((start_line + 1)) -f "${QEMU_LOG}" 2>/dev/null || true
    echo ""
    info "已退出日志监控 (QEMU 仍在后台运行)"
    echo "  停止 VM:  ./qemuctl.sh stop"
    echo "  SSH 登录: ./qemuctl.sh ssh"
}

# ============================================================
# 命令: stop
# ============================================================
cmd_stop() {
    title "停止 QEMU 虚拟机"

    if ! is_qemu_running; then
        info "没有运行中的 QEMU 进程"
        rm -f "${QEMU_PID_FILE}" 2>/dev/null || true
        return 0
    fi

    local pids=$(pgrep -f "qemu-system-x86_64.*${KERNEL_IMAGE}")
    info "停止 QEMU 进程: ${pids}"
    sudo kill ${pids} 2>/dev/null || true

    # 等待退出 (最多 5 秒)
    for i in $(seq 1 10); do
        if ! is_qemu_running; then
            info "QEMU 已退出"
            sudo rm -f "${QEMU_DISK}.lock" 2>/dev/null || true
            rm -f "${QEMU_PID_FILE}" 2>/dev/null || true
            return 0
        fi
        sleep 0.5
    done

    # 强制终止
    local remain=$(pgrep -f "qemu-system-x86_64.*${KERNEL_IMAGE}")
    warn "未自行退出, 强制终止: ${remain}"
    sudo kill -9 ${remain} 2>/dev/null || true
    sleep 1
    sudo rm -f "${QEMU_DISK}.lock" 2>/dev/null || true
    rm -f "${QEMU_PID_FILE}" 2>/dev/null || true

    if is_qemu_running; then
        error "仍有进程残留: $(pgrep -f 'qemu-system-x86_64')"
        return 1
    fi
    info "QEMU 已强制退出"
}

# ============================================================
# 命令: restart
# ============================================================
cmd_restart() {
    title "重启 QEMU"
    cmd_stop
    sleep 1
    cmd_start
}

# ============================================================
# 命令: clean
# ============================================================
cmd_clean() {
    title "清理环境"

    echo "[1/2] 停止 QEMU..."
    cmd_stop 2>/dev/null

    echo "[2/2] 清理 initramfs 构建目录..."
    local initramfs_dir="${OUTPUT_DIR}/initramfs"
    if [ -d "${initramfs_dir}" ]; then
        sudo rm -rf "${initramfs_dir}"
        info "已清理 ${initramfs_dir}"
    else
        info "目录不存在, 跳过"
    fi

    # 清理磁盘锁
    sudo rm -f "${QEMU_DISK}.lock" 2>/dev/null || true
    info "清理完成"
}

# ============================================================
# 命令: clean-all (清理全部, 包括 docker volumes)
# ============================================================
cmd_clean_all() {
    title "清理全部环境 (QEMU + initramfs + docker volumes)"

    echo "[1/3] 停止 QEMU..."
    cmd_stop 2>/dev/null

    echo "[2/3] 清理 initramfs 构建目录..."
    local initramfs_dir="${OUTPUT_DIR}/initramfs"
    if [ -d "${initramfs_dir}" ]; then
        sudo rm -rf "${initramfs_dir}"
        info "已清理 ${initramfs_dir}"
    fi

    echo "[3/3] 停止并清理 Docker 服务..."
    if [ -f "${DOCKER_DIR}/docker-compose.yml" ]; then
        cd "${DOCKER_DIR}"
        docker compose down -v 2>&1 | tail -5
        info "Docker 服务已停止, volumes 已清理"
    else
        info "未找到 docker-compose.yml, 跳过"
    fi

    sudo rm -f "${QEMU_DISK}.lock" 2>/dev/null || true
    info "全部清理完成"
}

# ============================================================
# 命令: status
# ============================================================
cmd_status() {
    title "QEMU 运行状态"

    if is_qemu_running; then
        info "QEMU 运行中 (PID: $(get_qemu_pid))"
        # 检查 SSH 连通性
        if ssh_vm "echo OK" 2>/dev/null | grep -q "OK"; then
            info "SSH 连接: 正常 (localhost:${SSH_PORT})"
        else
            warn "SSH 连接: 不可达"
        fi
    else
        warn "QEMU 未运行"
    fi

    echo ""
    echo "文件状态:"
    for f in "${KERNEL_IMAGE}" "${INITRAMFS}" "${QEMU_DISK}"; do
        if [ -f "$f" ]; then
            printf "  [OK]   %-20s %s bytes\n" "$(basename $f)" "$(ls -la $f | awk '{print $5}')"
        else
            printf "  [MISS] %-20s\n" "$(basename $f)"
        fi
    done

    # powerfs.ko 时间戳
    local ko="${POWERFS_MOD_DIR}/powerfs.ko"
    if [ -f "${ko}" ]; then
        echo ""
        echo "powerfs.ko: $(stat -c '%y' ${ko} | cut -d. -f1)"
    fi

    # Docker 服务状态
    echo ""
    echo "Docker 服务:"
    if command -v docker &>/dev/null; then
        local containers=$(docker ps --format '{{.Names}}\t{{.Status}}' 2>/dev/null | \
                          grep -E "master|volume|filer|redis" || true)
        if [ -n "${containers}" ]; then
            echo "${containers}" | while IFS=$'\t' read -r name status; do
                printf "  [OK]   %-20s %s\n" "${name}" "${status}"
            done
        else
            echo "  [MISS] 无运行中的 PowerFS 服务"
            echo "         启动: ./qemuctl.sh service start"
        fi
    else
        echo "  [SKIP] docker 命令不可用"
    fi
}

# ============================================================
# 命令: ssh
# ============================================================
cmd_ssh() {
    if ! is_qemu_running; then
        error "QEMU 未运行, 请先启动: ./qemuctl.sh start"
        exit 1
    fi
    info "SSH 登录到 VM (localhost:${SSH_PORT})"
    ssh_vm
}

# ============================================================
# 命令: mount [--rdma]
# 在 VM 内挂载 PowerFS, --rdma 时附加 transport=rdma mount 选项
# ============================================================
cmd_mount() {
    title "在 VM 内挂载 PowerFS"
    if ! is_qemu_running; then
        error "QEMU 未运行"
        exit 1
    fi

    # 解析 --rdma 参数
    local use_rdma=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --rdma) use_rdma=1; shift ;;
            *) shift ;;
        esac
    done

    # 证书路径 (VM 内路径, 支持环境变量覆盖)
    # Master 强制证书认证: 必须提供 ca_crt/client_crt/client_key
    # 首选 initramfs 预打包的 /etc/powerfs/ (VM 重启后仍存在), 兼容 /tmp/
    local cert_ca="${POWERFS_CA_CRT:-/etc/powerfs/ca.crt}"
    local cert_crt="${POWERFS_CLIENT_CRT:-/etc/powerfs/kernel-client-1.crt}"
    local cert_key="${POWERFS_CLIENT_KEY:-/etc/powerfs/kernel-client-1.key}"
    local mount_opts="master_addr=${POWERFS_MASTER_ADDR},master_port=${POWERFS_MASTER_PORT},shard_count=3,ca_crt=${cert_ca},client_crt=${cert_crt},client_key=${cert_key}"

    if [ "${use_rdma}" = "1" ]; then
        mount_opts="${mount_opts},transport=rdma"
        info "使用 RDMA 传输挂载 (mount 选项附加 transport=rdma)"
    fi

    # 如果 ko 未加载 (重启后 lsmod 没 powerfs), 先从 9p share insmod。
    # 注: powerfs.ko 通过 -virtfs 共享到 VM 的 /mnt/host/powerfs.ko。
    local load_script
    load_script=$(cat <<INSERTSCRIPT
set +e
mount -t 9p -o trans=virtio,version=9p2000.L,access=client ${SHARE_TAG} /mnt/host 2>/dev/null
if ! lsmod | grep -q powerfs; then
    insmod /mnt/host/powerfs.ko
    ir=\$?
    if [ \$ir -ne 0 ] && [ \$ir -ne 17 ]; then
        echo "INSMOD_FAIL rc=\$ir"
        dmesg | tail -10
        exit \$ir
    fi
    sleep 1
fi
mkdir -p /mnt/powerfs
mount -t powerfs -o "${mount_opts}" none /mnt/powerfs 2>&1
mr=\$?
if [ \$mr -ne 0 ]; then
    echo "MOUNT_FAIL rc=\$mr"
    dmesg | tail -20
fi
INSERTSCRIPT
)
    local result
    result=$(ssh_vm "$load_script" 2>&1)
    echo "$result" | grep -v "^$"
    if echo "$result" | grep -q "MOUNT_FAIL\|INSMOD_FAIL"; then
        error "挂载失败"
        return 1
    fi
    info "PowerFS 已挂载到 /mnt/powerfs (opts: ${mount_opts})"
    ssh_vm "mount | grep powerfs"
}

# ============================================================
# 命令: umount
# ============================================================
cmd_umount() {
    title "在 VM 内卸载 PowerFS"
    if ! is_qemu_running; then
        error "QEMU 未运行"
        exit 1
    fi
    ssh_vm "umount /mnt/powerfs 2>&1 || echo '未挂载或卸载失败'"
    info "卸载完成"
}

# ============================================================
# 命令: log
# ============================================================
cmd_log() {
    local grep_pattern="${1:-}"
    if ! is_qemu_running; then
        error "QEMU 未运行"
        exit 1
    fi
    if [ -n "${grep_pattern}" ]; then
        info "dmesg | grep '${grep_pattern}' (最后 50 行)"
        ssh_vm "dmesg | grep -iE '${grep_pattern}'" 2>&1 | tail -50
    else
        info "dmesg (最后 50 行)"
        ssh_vm "dmesg" 2>&1 | tail -50
    fi
}

# ============================================================
# 命令: monitor (实时监控 dmesg)
# ============================================================
cmd_monitor() {
    local grep_pattern="${1:-}"
    if ! is_qemu_running; then
        error "QEMU 未运行"
        exit 1
    fi
    info "实时监控 VM dmesg (Ctrl-C 退出)"
    if [ -n "${grep_pattern}" ]; then
        info "过滤关键词: ${grep_pattern}"
        ssh_vm "dmesg -w | grep --line-buffered -iE '${grep_pattern}'" 2>&1
    else
        ssh_vm "dmesg -w" 2>&1
    fi
}

# ============================================================
# 命令: serial (直接读宿主机 qemu.log 文件, VM 卡死也能用)
#
# 与 monitor 的区别:
#   - monitor 用 SSH 执行 dmesg -w, VM 卡死时 SSH 不通, 无法查看
#   - serial 直接 tail 宿主机上的 qemu.log 文件, 即使 VM 完全 hang 也能看
#
# 适用场景: VM 卡死/lockup/panic 后 SSH 断开, 需要查看最后的 serial 日志
# ============================================================
cmd_serial() {
    local grep_pattern="${1:-}"
    local lines="${2:-100}"

    if [ ! -f "${QEMU_LOG}" ]; then
        error "串口日志文件不存在: ${QEMU_LOG}"
        echo "  QEMU 可能未启动过, 或日志路径错误"
        exit 1
    fi

    if [ -n "${grep_pattern}" ]; then
        info "qemu.log | grep '${grep_pattern}' (最后 ${lines} 行匹配)"
        grep -iE "${grep_pattern}" "${QEMU_LOG}" 2>/dev/null | tail -n "${lines}"
    else
        info "qemu.log 最后 ${lines} 行 (VM 卡死时也可读):"
        echo "  日志路径: ${QEMU_LOG}"
        echo "  文件大小: $(du -h "${QEMU_LOG}" | awk '{print $1}')"
        echo ""
        tail -n "${lines}" "${QEMU_LOG}" 2>/dev/null
    fi
}

# ============================================================
# 命令: serial-tail (实时 tail 宿主机 qemu.log, VM 卡死也能用)
# ============================================================
cmd_serial_tail() {
    local grep_pattern="${1:-}"

    if [ ! -f "${QEMU_LOG}" ]; then
        error "串口日志文件不存在: ${QEMU_LOG}"
        echo "  QEMU 可能未启动过, 或日志路径错误"
        exit 1
    fi

    info "实时 tail qemu.log (Ctrl-C 退出, VM 卡死也能看)"
    echo "  日志路径: ${QEMU_LOG}"
    echo ""
    if [ -n "${grep_pattern}" ]; then
        info "过滤关键词: ${grep_pattern}"
        tail -n 50 -f "${QEMU_LOG}" 2>/dev/null | grep --line-buffered -iE "${grep_pattern}"
    else
        tail -n 50 -f "${QEMU_LOG}" 2>/dev/null
    fi
}

# ============================================================
# 命令: exec
# ============================================================
cmd_exec() {
    local cmd="${1:-}"
    if [ -z "${cmd}" ]; then
        error "请提供要执行的命令"
        echo "  用法: ./qemuctl.sh exec \"ls -la /mnt/powerfs/\""
        exit 1
    fi
    if ! is_qemu_running; then
        error "QEMU 未运行"
        exit 1
    fi
    info "执行: ${cmd}"
    ssh_vm "${cmd}" 2>&1
}

# ============================================================
# 命令: net (配置 TAP 网络)
# ============================================================
cmd_net() {
    title "配置 TAP 网络"
    sudo bash "${SCRIPT_DIR}/setup_network.sh"
}

# ============================================================
# 命令: rdma-setup
# 在宿主机上配置 rxe (Soft-RoCE) RDMA 环境:
#   1. 加载 rdma_rxe 内核模块
#   2. 在 docker bridge (powerfs-br0) 上创建 rxe 设备
#   3. 验证 /dev/infiniband/uverbs0 和 rdma_cm 设备已创建
#   4. 显示 RDMA 设备状态
#
# rxe 封装 RDMA over UDP, 宿主机 + 容器 + VM (TAP 在同一 bridge)
# 均可使用同一 rxe 设备通信。
# ============================================================
cmd_rdma_setup() {
    title "配置宿主机 rxe (Soft-RoCE) RDMA 环境"

    local netdev="${1:-powerfs-br0}"

    step "1/5 检查 rdma_rxe 模块..."
    if lsmod | grep -q rdma_rxe; then
        info "rdma_rxe 模块已加载"
    else
        info "加载 rdma_rxe 模块..."
        sudo modprobe rdma_rxe 2>&1 || {
            error "rdma_rxe 加载失败, 请确认内核已编译 CONFIG_RDMA_RXE=y"
            error "  查看内核配置: zcat /proc/config.gz | grep RDMA_RXE"
            return 1
        }
        info "rdma_rxe 模块已加载"
    fi

    step "2/5 检查 netdev '${netdev}'..."
    if ! ip link show "${netdev}" &>/dev/null; then
        warn "netdev '${netdev}' 不存在"
        echo "  可用网络设备:"
        ip -br link show 2>/dev/null | head -10
        echo ""
        warn "尝试使用宿主机主网卡替代 (例如 eth0, ens33, enp0s...)"
        read -r -p "请输入 netdev 名称 (或 Ctrl-C 退出): " netdev
        if [ -z "${netdev}" ] || ! ip link show "${netdev}" &>/dev/null; then
            error "无效的 netdev: ${netdev}"
            return 1
        fi
    fi
    info "使用 netdev: ${netdev}"

    step "3/5 创建 rxe 设备..."
    if rdma link show 2>/dev/null | grep -q "rxe"; then
        info "rxe 设备已存在, 跳过创建"
    else
        # 注: 宿主机可能已有 mlx5 硬件 RDMA, rxe 会创建为 uverbs2
        # sysfs 参数名是 "add" (非 "add_link"), 值为 netdev 名称
        # 用 printf 避免结尾换行触发 "Invalid argument"
        if printf '%s' "${netdev}" | sudo tee /sys/module/rdma_rxe/parameters/add 2>/dev/null | grep -q "${netdev}"; then
            info "rxe 设备已创建 (netdev=${netdev})"
        else
            error "rxe 创建失败"
            return 1
        fi
    fi

    step "4/5 验证 /dev/infiniband 设备..."
    # rxe 通常创建为 uverbs2 (前两个可能是 mlx5 硬件)
    local rxe_uverbs=""
    for i in 0 1 2 3 4; do
        if [ -c "/dev/infiniband/uverbs${i}" ]; then
            # 检查此 uverbs 设备对应的 IB 设备名
            local ib_dev=$(cat "/sys/class/infiniband_verbs/uverbs${i}/device/infiniband/"* 2>/dev/null || \
                           ls /sys/class/infiniband_verbs/uverbs${i}/../ 2>/dev/null | head -1)
            info "/dev/infiniband/uverbs${i} → ${ib_dev:-unknown}"
            if [ "${ib_dev}" = "rxe0" ] || echo "${ib_dev}" | grep -q "rxe"; then
                rxe_uverbs="uverbs${i}"
            fi
        fi
    done

    if [ -n "${rxe_uverbs}" ]; then
        info "rxe0 对应设备: /dev/infiniband/${rxe_uverbs}"
    else
        warn "未找到 rxe 对应的 uverbs 设备"
    fi

    # rdma_cm 设备 (所有 RDMA 设备共用)
    if [ -c /dev/infiniband/rdma_cm ]; then
        info "/dev/infiniband/rdma_cm 已就绪"
    else
        warn "/dev/infiniband/rdma_cm 缺失"
    fi

    step "5/5 RDMA 设备状态..."
    rdma link show 2>/dev/null || true
    echo ""
    if command -v ibv_devinfo &>/dev/null; then
        info "ibv_devinfo 输出 (宿主机, 可能缺 rxe provider):"
        ibv_devinfo 2>/dev/null | head -20
    else
        warn "ibverbs-utils 未安装在宿主机 (容器内有)"
    fi

    if [ -n "${rxe_uverbs}" ] && [ -c /dev/infiniband/rdma_cm ]; then
        echo ""
        info "RDMA 环境就绪, 可启动 RDMA 模式服务:"
        echo "  ./qemuctl.sh service start --rdma"
        echo "  ./qemuctl.sh mount --rdma"
    else
        echo ""
        error "RDMA 设备不完整, 检查内核配置和 rxe 模块"
        return 1
    fi
}

# ============================================================
# 命令: service (管理 Docker 服务)
#
# 用法:
#   service start [--rdma]   启动服务 (TCP 默认, --rdma 启用 RDMA 传输)
#   service stop [--rdma]    停止服务
#   service restart [--rdma] 重启服务
#   service status           查看服务状态 (含 RDMA 设备检查)
#   service log <n>          查看服务日志
# ============================================================
cmd_service() {
    local action="${1:-status}"
    shift || true
    local target=""
    local rdma_flag=0

    # 解析参数: --rdma 和 target
    while [ $# -gt 0 ]; do
        case "$1" in
            --rdma)
                rdma_flag=1
                shift
                ;;
            *)
                target="$1"
                shift
                ;;
        esac
    done

    # 设置全局 RDMA 开关
    USE_RDMA="${rdma_flag}"

    if [ "${USE_RDMA}" = "1" ]; then
        info "RDMA 模式已启用 (附加 docker-compose.rdma.yml)"
        # 检查 RDMA 前置条件
        if ! ls /dev/infiniband/uverbs0 &>/dev/null; then
            warn "/dev/infiniband/uverbs0 不存在, 请先配置 rxe (参见: rdma-setup)"
            warn "  sudo modprobe rdma_rxe"
            warn "  sudo rdma link add rxe0 type rxe <netdev>"
        fi
    fi

    case "${action}" in
        start)
            cmd_service_start
            ;;
        stop)
            cmd_service_stop
            ;;
        restart)
            cmd_service_stop
            sleep 2
            cmd_service_start
            ;;
        status)
            cmd_service_status
            ;;
        log)
            if [ -z "${target}" ]; then
                error "请提供服务名: master-1, volume-1, filer-1 等"
                echo "  用法: ./qemuctl.sh service log filer-1"
                exit 1
            fi
            cmd_service_log "${target}"
            ;;
        *)
            error "未知 service 命令: ${action}"
            echo "  可用: start [--rdma], stop [--rdma], restart [--rdma], status, log"
            exit 1
            ;;
    esac
}

cmd_service_start() {
    title "启动 Docker 服务 (RDMA=${USE_RDMA})"

    if [ "${USE_RDMA}" = "1" ]; then
        if [ ! -f "${DOCKER_DIR}/docker-compose.rdma.yml" ]; then
            error "未找到 docker-compose.rdma.yml: ${DOCKER_DIR}/docker-compose.rdma.yml"
            exit 1
        fi
        # RDMA 前置条件检查
        if ! ls /dev/infiniband/uverbs2 &>/dev/null; then
            error "/dev/infiniband/uverbs2 不存在, 请先执行: ./qemuctl.sh rdma-setup"
            exit 1
        fi
        # 先停止 TCP 服务 (避免端口冲突)
        local tcp_running=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE "master-1|volume-1|filer-1" || echo 0)
        if [ "${tcp_running}" -gt 0 ]; then
            warn "检测到 TCP 服务运行中, 先停止以避免端口冲突..."
            cd "${DOCKER_DIR}"
            docker compose -f docker-compose.yml down 2>&1 | tail -3
            sleep 2
        fi
    else
        if [ ! -f "${DOCKER_DIR}/docker-compose.yml" ]; then
            error "未找到 docker-compose.yml: ${DOCKER_DIR}/docker-compose.yml"
            exit 1
        fi
    fi

    local cf_files
    cf_files=$(compose_files)
    cd "${DOCKER_DIR}"

    # 检查服务是否已在运行
    local running=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE "master-1|volume-1|filer-1" || echo 0)
    if [ "${running}" -ge 3 ]; then
        warn "服务已在运行 (master-1/volume-1/filer-1)"
        cmd_service_status
        return 0
    fi

    step "1/3 启动 Redis + Master..."
    docker compose ${cf_files} up -d redis master-1 2>&1 | tail -5

    # 等待 master 就绪
    info "等待 master 就绪..."
    local tries=0
    while [ ${tries} -lt 30 ]; do
        if docker exec master-1 nc -z 127.0.0.1 9333 2>/dev/null; then
            info "master-1 就绪"
            break
        fi
        sleep 1
        tries=$((tries + 1))
    done

    step "2/3 启动 Volume..."
    docker compose ${cf_files} up -d volume-1 2>&1 | tail -5

    info "等待 volume 就绪..."
    tries=0
    while [ ${tries} -lt 30 ]; do
        if docker exec volume-1 nc -z 127.0.0.1 8080 2>/dev/null; then
            info "volume-1 就绪"
            break
        fi
        sleep 1
        tries=$((tries + 1))
    done

    step "3/3 启动 Filer..."
    docker compose ${cf_files} up -d filer-1 2>&1 | tail -5

    info "等待 filer 就绪..."
    tries=0
    while [ ${tries} -lt 30 ]; do
        if docker exec filer-1 nc -z 127.0.0.1 8888 2>/dev/null; then
            info "filer-1 就绪"
            break
        fi
        sleep 1
        tries=$((tries + 1))
    done

    echo ""
    info "服务启动完成 (RDMA=${USE_RDMA}, Master=${POWERFS_MASTER_ADDR})"
    cmd_service_status
}

cmd_service_stop() {
    title "停止 Docker 服务"

    if [ ! -f "${DOCKER_DIR}/docker-compose.yml" ]; then
        error "未找到 docker-compose.yml"
        exit 1
    fi

    local cf_files
    cf_files=$(compose_files)
    cd "${DOCKER_DIR}"
    docker compose ${cf_files} down 2>&1 | tail -5
    info "Docker 服务已停止 (RDMA=${USE_RDMA})"
}

cmd_service_status() {
    title "Docker 服务状态"

    if ! command -v docker &>/dev/null; then
        error "docker 命令不可用"
        return 1
    fi

    # RDMA 设备和 rxe 状态检查
    echo "RDMA 环境:"
    if ls /dev/infiniband/uverbs0 &>/dev/null 2>&1; then
        printf "  ${GREEN}%-15s${NC} %s\n" "uverbs0:" "/dev/infiniband/uverbs0 (可用)"
    else
        printf "  ${YELLOW}%-15s${NC} %s\n" "uverbs0:" "未找到 (TCP 模式可用, RDMA 模式需先配置 rxe)"
    fi
    if command -v rdma &>/dev/null; then
        local rxe_links=$(rdma link show 2>/dev/null | grep -c "rxe" || echo 0)
        if [ "${rxe_links}" -gt 0 ]; then
            printf "  ${GREEN}%-15s${NC} %s\n" "rxe:" "${rxe_links} 个 rxe 设备"
            rdma link show 2>/dev/null | head -3 | while read -r line; do
                echo "    ${line}"
            done
        else
            printf "  ${YELLOW}%-15s${NC} %s\n" "rxe:" "无 rxe 设备 (RDMA 模式需先创建)"
        fi
    else
        printf "  ${YELLOW}%-15s${NC} %s\n" "rdma:" "rdma-tools 未安装"
    fi
    if [ "${USE_RDMA}" = "1" ]; then
        printf "  ${GREEN}%-15s${NC} %s\n" "transport:" "RDMA (docker-compose.rdma.yml 已附加)"
    else
        printf "  ${GREEN}%-15s${NC} %s\n" "transport:" "TCP (默认)"
    fi
    echo ""

    local services=("redis" "master-1" "master-2" "master-3" \
                    "volume-1" "volume-2" "volume-3" \
                    "filer-1" "filer-2" "filer-3")

    echo "服务状态:"
    printf "  %-15s %-15s %s\n" "SERVICE" "STATUS" "PORTS"
    printf "  %-15s %-15s %s\n" "-------" "------" "-----"

    for svc in "${services[@]}"; do
        local status=$(docker inspect -f '{{.State.Status}}' "${svc}" 2>/dev/null || echo "missing")
        local ports=$(docker port "${svc}" 2>/dev/null | tr '\n' ' ' || echo "")
        if [ "${status}" = "running" ]; then
            printf "  ${GREEN}%-15s${NC} %-15s %s\n" "${svc}" "${status}" "${ports}"
        else
            printf "  ${RED}%-15s${NC} %-15s %s\n" "${svc}" "${status}" "${ports}"
        fi
    done
}

cmd_service_log() {
    local svc="${1}"
    title "Docker 服务日志: ${svc} (最后 50 行)"

    if ! docker ps -a --format '{{.Names}}' 2>/dev/null | grep -q "^${svc}$"; then
        error "容器 ${svc} 不存在"
        echo "  运行中的容器:"
        docker ps --format '    {{.Names}}' 2>/dev/null
        exit 1
    fi

    docker logs --tail 50 "${svc}" 2>&1
}

# ============================================================
# 主入口
# ============================================================
print_help() {
    cat << 'EOF'
PowerFS QEMU 统一管理脚本

用法: ./qemuctl.sh <command> [options]

[构建]
  build             编译 powerfs.ko + 构建 initramfs
  build-all         编译内核 + powerfs.ko + initramfs (全量)
  deploy            一键重新构建 powerfs.ko + 重启 QEMU (常用迭代)

[QEMU 生命周期]
  start             启动 QEMU 虚拟机 (后台, 日志重定向, 正常测试用)
  debug             启动 QEMU 调试模式 (后台 + 调试参数 + 实时 tail serial 日志)
                    (loglevel=7 slub_debug=FZP rcu_stall=on, stall/oops 排障用)
  stop              停止 QEMU 虚拟机
  restart           重启 QEMU (stop + start)
  status            查看 QEMU + Docker 服务状态

[VM 操作]
  ssh               SSH 登录到 VM
  mount [--rdma]    在 VM 内挂载 PowerFS (--rdma 附加 transport=rdma 选项)
  umount            在 VM 内卸载 PowerFS
  exec <cmd>        在 VM 内执行命令
  log [grep]        查看 VM dmesg 日志 (可选 grep 关键词, 需 SSH 通)
  monitor [grep]    实时监控 VM dmesg (tail -f, Ctrl-C 退出, 需 SSH 通)
  serial [grep] [N]  读宿主机 qemu.log 最后 N 行 (VM 卡死也能用, 默认 100)
  serial-tail [grep] 实时 tail 宿主机 qemu.log (VM 卡死也能用)

[RDMA]
  rdma-setup [netdev]  配置宿主机 rxe (Soft-RoCE) RDMA 环境
                       netdev 默认 powerfs-br0, 也可指定 eth0/ens33 等

[Docker 服务]
  service start [--rdma]   启动 master/volume/filer 服务 (--rdma 启用 RDMA 传输)
  service stop [--rdma]    停止服务
  service restart [--rdma] 重启服务
  service status           查看服务状态 (含 RDMA 设备检查)
  service log <n>          查看服务日志 (master-1/volume-1/filer-1 等)

[环境]
  clean             清理环境 (停止 QEMU + 清理 initramfs 构建目录)
  clean-all         清理全部 (QEMU + initramfs + docker volumes)
  net               配置 TAP 网络 (调用 setup_network.sh)

RDMA 端到端测试流程:
  1. ./qemuctl.sh rdma-setup                # 配置宿主机 rxe
  2. ./qemuctl.sh service start --rdma      # 启动 RDMA 模式服务
  3. ./qemuctl.sh service status            # 验证 RDMA 设备 + 服务状态
  4. ./qemuctl.sh deploy                    # 部署 powerfs.ko + 启动 QEMU
  5. ./qemuctl.sh mount --rdma               # 在 VM 内以 RDMA 挂载
  6. ./qemuctl.sh monitor powerfs           # 监控 dmesg

示例:
  ./qemuctl.sh service start && ./qemuctl.sh deploy
  ./qemuctl.sh debug                    # 调试模式 (stall/oops 排障, 实时 serial 日志)
  ./qemuctl.sh ssh
  ./qemuctl.sh log powerfs
  ./qemuctl.sh monitor powerfs
  ./qemuctl.sh serial "BUG\|Oops\|stall\|lockup" 200  # VM 卡死时查 lockup 日志
  ./qemuctl.sh serial-tail "powerfs\|stall"            # 实时监控 serial 日志
  ./qemuctl.sh exec "cat /mnt/powerfs/test.txt"
  ./qemuctl.sh service log filer-1
EOF
}

main() {
    local cmd="${1:-help}"
    shift || true

    case "${cmd}" in
        build)          cmd_build "$@" ;;
        build-all)      cmd_build_all "$@" ;;
        deploy)         cmd_deploy "$@" ;;
        hotdeploy)      cmd_hotdeploy "$@" ;;
        start)          cmd_start "$@" ;;
        debug)          cmd_debug "$@" ;;
        stop)           cmd_stop "$@" ;;
        restart)        cmd_restart "$@" ;;
        status)         cmd_status "$@" ;;
        ssh)            cmd_ssh "$@" ;;
        mount)          cmd_mount "$@" ;;
        umount)         cmd_umount "$@" ;;
        log)            cmd_log "$@" ;;
        monitor)        cmd_monitor "$@" ;;
        serial)         cmd_serial "$@" ;;
        serial-tail)    cmd_serial_tail "$@" ;;
        exec)           cmd_exec "$@" ;;
        net)            cmd_net "$@" ;;
        rdma-setup)     cmd_rdma_setup "$@" ;;
        service)        cmd_service "$@" ;;
        clean)          cmd_clean "$@" ;;
        clean-all)      cmd_clean_all "$@" ;;
        help|--help|-h) print_help ;;
        *)
            error "未知命令: ${cmd}"
            echo ""
            print_help
            exit 1
            ;;
    esac
}

main "$@"
