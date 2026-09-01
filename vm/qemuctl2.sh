#!/bin/bash
# PowerFS 双 QEMU VM 管理脚本 (硬件 RDMA SR-IOV VFIO)
#
# 同时启动 2 个 QEMU VM，每个 VM 直通一个 mlx5_1 (Active IB) 的 SR-IOV VF。
# 每 VM 拥有独立的 SSH 端口 / TAP 设备 / eth0 IP / ib0 IP / VFIO BDF / PID / 日志 / 磁盘。
#
# 默认分配 (可通过同名环境变量覆盖):
#                           VM1 (id=1)            VM2 (id=2)
#   SSH port:              2223                  2224
#   TAP device:            tap0                  tap1
#   MAC (TAP eth0):        52:54:00:12:34:56     52:54:00:12:34:58
#   MAC (user-net eth1):   52:54:00:12:34:57     52:54:00:12:34:59
#   eth0 (docker-net) IP:  172.30.0.100          172.30.0.101
#   ib0 (IPoIB) IP:        192.168.100.100       192.168.100.101
#   VFIO VF BDF:           自动探测 mlx5_1 的 前两个 VF (virtfn0, virtfn1)
#
# 用法:
#   ./qemuctl2.sh <command> [args]
#
# 命令:
#   start                     同时启动 VM1 + VM2（含 SR-IOV 扩 VF + TAP + vfio 绑定）
#   stop                      停止 VM1 + VM2
#   restart                   stop + start
#   status                    查看两 VM 状态 (PID/SSH连通性/IB设备/IPoIB IP)
#   ssh     <vm1|vm2>         SSH 登录到指定 VM
#   exec    <vm1|vm2> "cmd"   在指定 VM 内执行命令
#   log     <vm1|vm2> [kw]    查看指定 VM 的 dmesg (可选 grep 关键词)
#   serial  <vm1|vm2> [N]     读取指定 VM 的 qemu serial 日志最后 N 行 (默认 100)
#   mount   <vm1|vm2>         在指定 VM 内挂载 PowerFS (transport=rdma)
#   umount  <vm1|vm2>         在指定 VM 内卸载 PowerFS
#   prepare-sriov             仅准备 SR-IOV (numvfs=2) + vfio bind + TAP，不启动 VM
#
# 示例:
#   ./qemuctl2.sh start
#   ./qemuctl2.sh exec vm1 "bash /mnt/host/rc18f_stress.sh"
#   ./qemuctl2.sh exec vm2 "insmod /mnt/host/powerfs.ko && \
#        mount -t powerfs -o transport=rdma,... powerfs /mnt/powerfs"
#   ./qemuctl2.sh status
#   ./qemuctl2.sh stop
#
# 前置:
#   * host BIOS 已开启 VT-d, cmdline 有 intel_iommu=on
#   * mlx5_1 端口 Active (200Gb/s InfiniBand, IPoIB 192.168.100.3/24)
#   * Docker master/volume/filer 运行 (172.30.0.0/16 + 192.168.100.3 RDMA net_port)
#   * VM1 当前运行时会被先 stop (释放正在占用的 VFIO VF virtfn0) — 建议单/双 VM 二选一

set -u

# ============================================================
# 基础路径 (同 qemuctl.sh)
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"
POWERFS_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
KERNEL_SOURCE="${KERNEL_SOURCE:-/home/portion/powerfs/ubuntu-linux-git}"
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"
DOCKER_DIR="${POWERFS_ROOT}/docker"
KERNEL_IMAGE="${OUTPUT_DIR}/bzImage"
INITRAMFS="${OUTPUT_DIR}/initramfs.cpio.gz"
QEMU_DISK_BASE="${OUTPUT_DIR}/qemu_disk.img"
SHARE_DIR="${POWERFS_ROOT}/kernel/vm/share"
SHARE_TAG_BASE="hostshare"

# 确认内核 + initramfs 存在
if [ ! -f "${KERNEL_IMAGE}" ] || [ ! -f "${INITRAMFS}" ]; then
    echo "[ERROR] Missing ${KERNEL_IMAGE} or ${INITRAMFS} — run: ./qemuctl.sh build-all"
    exit 1
fi

# ============================================================
# 每 VM 参数
# ============================================================
SSH_USER="root"
SSH_PASS="powerfs"
MEM_SIZE="${MEM_SIZE:-4096}"
CPU_CORES="${CPU_CORES:-4}"

# 网关 + Docker powerfs-network 网段
GATEWAY="172.30.0.1"
POWERFS_MASTER_ADDR="${POWERFS_MASTER_ADDR:-172.30.0.11,172.30.0.12,172.30.0.13}"
POWERFS_MASTER_PORT="${POWERFS_MASTER_PORT:-9334}"

# VM1 配置
VM1_NAME="${VM1_NAME:-vm1}"
VM1_SSH_PORT="${VM1_SSH_PORT:-2223}"
VM1_TAP="${VM1_TAP:-tap0}"
VM1_ETH0_IP="${VM1_ETH0_IP:-172.30.0.100}"
VM1_IB0_IP="${VM1_IB0_IP:-192.168.100.100}"
VM1_MAC0="${VM1_MAC0:-52:54:00:12:34:56}"
VM1_MAC1="${VM1_MAC1:-52:54:00:12:34:57}"
VM1_VF_IDX="${VM1_VF_IDX:-0}"   # virtfnN on mlx5_1 (0 = first VF)

# VM2 配置
VM2_NAME="${VM2_NAME:-vm2}"
VM2_SSH_PORT="${VM2_SSH_PORT:-2224}"
VM2_TAP="${VM2_TAP:-tap1}"
VM2_ETH0_IP="${VM2_ETH0_IP:-172.30.0.101}"
VM2_IB0_IP="${VM2_IB0_IP:-192.168.100.101}"
VM2_MAC0="${VM2_MAC0:-52:54:00:12:34:58}"
VM2_MAC1="${VM2_MAC1:-52:54:00:12:34:59}"
VM2_VF_IDX="${VM2_VF_IDX:-1}"   # virtfnN on mlx5_1 (1 = second VF)

# SR-IOV PF: mlx5_1 (link ACTIVE, max 16 VFs)
IB_PF_NAME="${IB_PF_NAME:-mlx5_1}"
IB_NUM_VFS="${IB_NUM_VFS:-2}"  # need at least 2 for two VMs

# ============================================================
# 颜色输出
# ============================================================
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m';  CYAN='\033[0;36m';  NC='\033[0m'
# 注意: info/warn/step/title 全部写入 stderr — 因为 ensure_sriov_and_vfio 用 stdout 返
# 回 BDF 列表, 调用者用 vf_bdfs="$(ensure_sriov_and_vfio)" 捕获, 任何 stdout 污染都会导致
# BDF 被解析为 "mlx5_1 (...) sriov_numvfs=..." 这种无效字符串, 最后 qemu VFIO 参数错误.
info()  { echo -e "${GREEN}[INFO]${NC}  $*" >&2; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*" >&2; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }
title() { echo -e "\n${BLUE}=== $* ===${NC}" >&2; }
step()  { echo -e "${CYAN}[STEP]${NC} $*" >&2; }

# ============================================================
# Helpers — vm_id → 各路径/端口函数
# ============================================================
vm_val() {
    # vm_val <vm1|vm2> <KEY>
    case "$1" in
        vm1|1) local n=1 ;;
        vm2|2) local n=2 ;;
        *)  error "Unknown vm id: $1 (expect vm1|vm2)"; return 1 ;;
    esac
    local key="$2"
    case "${n}${key}" in
        1NAME) echo "${VM1_NAME}" ;;
        1SSH)  echo "${VM1_SSH_PORT}" ;;
        1TAP)  echo "${VM1_TAP}" ;;
        1EIP)  echo "${VM1_ETH0_IP}" ;;
        1IIP)  echo "${VM1_IB0_IP}" ;;
        1MAC0) echo "${VM1_MAC0}" ;;
        1MAC1) echo "${VM1_MAC1}" ;;
        1VFI)  echo "${VM1_VF_IDX}" ;;
        2NAME) echo "${VM2_NAME}" ;;
        2SSH)  echo "${VM2_SSH_PORT}" ;;
        2TAP)  echo "${VM2_TAP}" ;;
        2EIP)  echo "${VM2_ETH0_IP}" ;;
        2IIP)  echo "${VM2_IB0_IP}" ;;
        2MAC0) echo "${VM2_MAC0}" ;;
        2MAC1) echo "${VM2_MAC1}" ;;
        2VFI)  echo "${VM2_VF_IDX}" ;;
        *) error "Unknown key: $key for vm $n"; return 1 ;;
    esac
}
vm_pid_file()    { echo "${OUTPUT_DIR}/qemu_$(vm_val $1 NAME).pid"; }
vm_log()         { echo "${OUTPUT_DIR}/qemu_$(vm_val $1 NAME).log"; }
vm_disk()        { echo "${OUTPUT_DIR}/qemu_disk_$(vm_val $1 NAME).img"; }
# NOTE: mount_tag must be "hostshare" exactly because the initramfs init script
# (build_initramfs.sh L491) hardcodes: `mount -t 9p -o trans=virtio hostshare /mnt/host`.
# Each VM has its own -virtfs fsdev instance with unique id= (share_tag_$vm), so
# the mount_tag can safely be the same literal "hostshare" across VMs (QEMU scope
# is per -virtfs invocation).
vm_share_tag()   { echo "hostshare"; }
vm_qemu_match()  { echo "qemu_$(vm_val $1 NAME).pid"; }

# ============================================================
# SSH helper — ssh_vm <vm1|vm2> <cmd...>
# ============================================================
ssh_vm() {
    local vid="$1"; shift
    local port
    port="$(vm_val "${vid}" SSH)"
    sshpass -p "${SSH_PASS}" ssh -q \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=8 \
        -p "${port}" \
        "${SSH_USER}@localhost" "$@"
}
is_vm_running() {
    local pidfile
    pidfile="$(vm_pid_file "$1")"
    [ -f "${pidfile}" ] || return 1
    local pid
    pid="$(cat "${pidfile}" 2>/dev/null)"
    [ -n "${pid}" ] || return 1
    # pgrep by pidfile pid
    kill -0 "${pid}" 2>/dev/null && return 0
    # fallback: pgrep for any qemu matching the vm-name pidfile string
    pgrep -f "$(vm_qemu_match "$1")" >/dev/null 2>&1
}
get_vm_qemu_pid() {
    local pid=$(pgrep -f "$(vm_qemu_match "$1")" | head -1)
    [ -n "${pid}" ] && echo "${pid}"
}

# ============================================================
# SR-IOV + VFIO 准备
#   1. Ensure mlx5_1 sriov_numvfs >= IB_NUM_VFS (=2)
#   2. Bind the first N VFs to vfio-pci
#   3. Returns BDFs per VF index via side-channel files (stdout space-sep list)
# ============================================================
ensure_sriov_and_vfio() {
    local pf_sysfs="/sys/class/infiniband/${IB_PF_NAME}/device"
    if [ ! -d "${pf_sysfs}" ]; then
        error "IB PF ${IB_PF_NAME} not found at ${pf_sysfs}"
        return 1
    fi
    local pf_pci
    pf_pci="$(basename "$(readlink -f "${pf_sysfs}")")"   # 0000:a0:00.1
    local totalvfs
    totalvfs="$(cat "${pf_sysfs}/sriov_totalvfs" 2>/dev/null)"
    if [ "${totalvfs}" -lt "${IB_NUM_VFS}" ]; then
        error "${IB_PF_NAME} only supports sriov_totalvfs=${totalvfs}, need ${IB_NUM_VFS}"
        return 1
    fi
    local cur_numvfs
    cur_numvfs="$(cat "${pf_sysfs}/sriov_numvfs" 2>/dev/null)"
    info "${IB_PF_NAME} (${pf_pci}) sriov_numvfs=${cur_numvfs} / total=${totalvfs}"

    if [ "${cur_numvfs}" -lt "${IB_NUM_VFS}" ]; then
        step "  Increase sriov_numvfs: ${cur_numvfs} → ${IB_NUM_VFS}"
        # mlx5_core 拒绝在 numvfs 从 "非 0 值 → 更高值" 的直接修改 (EBUSY).
        # 安全流程: 先降到 0 (释放所有 VF) 再升到目标值.
        if ! sudo bash -c "echo ${IB_NUM_VFS} > /sys/bus/pci/devices/${pf_pci}/sriov_numvfs" 2>/dev/null; then
            warn "    Direct increase failed, fallback: unbind existing VFs → numvfs=0 → numvfs=${IB_NUM_VFS}"
            local j
            for j in $(seq 0 $(( cur_numvfs - 1 )) ); do
                local vlink="${pf_sysfs}/virtfn${j}"
                [ -L "${vlink}" ] || continue
                local vabs="$(readlink -f "${vlink}")"
                local vbdf="${vabs##*/}"
                local vdrv="$(basename "$(readlink "${vabs}/driver" 2>/dev/null)" 2>/dev/null)"
                if [ "${vdrv}" = "vfio-pci" ]; then
                    echo "${vbdf}" | sudo tee "/sys/bus/pci/drivers/vfio-pci/unbind" >/dev/null 2>&1 || true
                elif [ "${vdrv}" = "mlx5_core" ]; then
                    echo "${vbdf}" | sudo tee "/sys/bus/pci/drivers/mlx5_core/unbind" >/dev/null 2>&1 || true
                fi
            done
            sudo bash -c "echo 0 > /sys/bus/pci/devices/${pf_pci}/sriov_numvfs" \
                || { error "Failed to set sriov_numvfs=0"; return 1; }
            sleep 1
            sudo bash -c "echo ${IB_NUM_VFS} > /sys/bus/pci/devices/${pf_pci}/sriov_numvfs" \
                || { error "Failed to set sriov_numvfs=${IB_NUM_VFS}"; return 1; }
        fi
    fi

    # 为前 IB_NUM_VFS 个 VF 做: unbind mlx5_core + bind vfio-pci
    local -a bdfs=()
    for i in $(seq 0 $(( IB_NUM_VFS - 1 )) ); do
        local vf_link="${pf_sysfs}/virtfn${i}"
        if [ ! -L "${vf_link}" ]; then
            error "virtfn${i} symlink missing on ${IB_PF_NAME} — SR-IOV enable failed"
            return 1
        fi
        local vf_abs
        vf_abs="$(readlink -f "${vf_link}")"
        local vf_bdf="${vf_abs##*/}"   # e.g. 0000:a0:02.3
        local drv
        drv="$(basename "$(readlink "${vf_abs}/driver" 2>/dev/null)" 2>/dev/null)"

        if [ "${drv}" != "vfio-pci" ]; then
            step "  bind vfio-pci: VF${i} ${vf_bdf} (was driver=${drv:-unbound})"
            local vendor="$(cat "${vf_abs}/vendor" 2>/dev/null)"
            local device="$(cat "${vf_abs}/device" 2>/dev/null)"
            if [ "${drv}" = "mlx5_core" ]; then
                # unbind from mlx5_core
                echo "${vf_bdf}" | sudo tee "/sys/bus/pci/drivers/mlx5_core/unbind" >/dev/null 2>&1 \
                    || warn "    unbind mlx5_core failed (may be ok)"
            fi
            # set override for vfio-pci then bind
            echo "${vendor#0x} ${device#0x}" | sudo tee "/sys/bus/pci/drivers/vfio-pci/new_id" >/dev/null 2>&1 || true
            echo "${vf_bdf}" | sudo tee "/sys/bus/pci/drivers/vfio-pci/bind" >/dev/null 2>&1 \
                || { error "  Failed to bind ${vf_bdf} to vfio-pci"; return 1; }
        else
            info "  VF${i} ${vf_bdf} already vfio-pci ✓"
        fi
        # chmod /dev/vfio/<iommu_group>
        local iommu_grp
        iommu_grp="$(basename "$(readlink "${vf_abs}/iommu_group" 2>/dev/null)" 2>/dev/null)"
        if [ -n "${iommu_grp}" ] && [ -c "/dev/vfio/${iommu_grp}" ]; then
            sudo chmod 666 "/dev/vfio/${iommu_grp}" 2>/dev/null || true
        fi
        bdfs+=("${vf_bdf}")
    done

    # ============================================================
    # 4. 为每个新 VF 分配合法 node_guid + port_guid 并 link-state=enable
    #   mlx5_core 在 numvfs 动态增长后 (virtfn1,2,...) 不会自动为它们生成 GUID，
    #   此时若直通 VF，VM 内 mlx5 VF 枚举 node_guid=all-zero → GID 表为空 →
    #   rdma_resolve_addr ENODEV。这里使用 PF node_guid 低字节 + VF index 派生出
    #   全局唯一 GUID (ea:a1/VF0 ea:a2/VF1 ea:a3/VF2 ... 作为 node, ea:b1/VF0 ea:b2...
    #   作为 port)，并通过 `ip link set <IPoIB> vf N state enable` 让 SM 重新枚举。
    # ============================================================
    local ib_iface
    ib_iface="$(ls "${pf_sysfs}/device/net/" 2>/dev/null | head -1)"
    if [ -n "${ib_iface}" ]; then
        # 解析 PF node guid 格式: 5c:25:73:03:00:cf:ea:a3, 保留前14 hex chars, 后2字节替换
        local pf_node
        pf_node="$(cat "${pf_sysfs}/node_guid" 2>/dev/null | tr ':' ' ')"
        local a b c d e f g
        read a b c d e f g _ <<<"${pf_node}"
        for i in $(seq 0 $(( IB_NUM_VFS - 1 )) ); do
            # 派生 node_guid: pf_prefix + (ea : (a1+VFidx*2))
            # 派生 port_guid: pf_prefix + (ea : (b1+VFidx*2))
            local h_node=$(( 0xa1 + 2 * i ))
            local h_port=$(( 0xb1 + 2 * i ))
            # check current
            local cur_info
            cur_info="$(ip -d link show "${ib_iface}" 2>/dev/null | grep -E "vf ${i} ")":
            local cur_node cur_port
            cur_node="$(echo "${cur_info}" | grep -oE "NODE_GUID [0-9a-f:]+" | awk "{print \$2}")"
            cur_port="$(echo "${cur_info}" | grep -oE "PORT_GUID [0-9a-f:]+" | awk "{print \$2}")"
            local want_node="${a}:${b}:${c}:${d}:${e}:${f}:${g}:$(printf '%02x' "${h_node}")"
            local want_port="${a}:${b}:${c}:${d}:${e}:${f}:${g}:$(printf '%02x' "${h_port}")"
            if [ "${cur_node}" != "${want_node}" ]; then
                step "  VF${i}: set NODE_GUID ${cur_node:-EMPTY} → ${want_node}"
                sudo ip link set "${ib_iface}" vf "${i}" node_guid "${want_node}" >/dev/null 2>&1 \
                    || warn "    (non-fatal: ip link set node_guid ${i} failed)"
            fi
            if [ "${cur_port}" != "${want_port}" ]; then
                step "  VF${i}: set PORT_GUID ${cur_port:-EMPTY} → ${want_port}"
                sudo ip link set "${ib_iface}" vf "${i}" port_guid "${want_port}" >/dev/null 2>&1 \
                    || warn "    (non-fatal: ip link set port_guid ${i} failed)"
            fi
            # ensure link-state=enable (otherwise SM ignores VF → vm NO-CARRIER)
            if ! echo "${cur_info}" | grep -q "link-state enable"; then
                sudo ip link set "${ib_iface}" vf "${i}" state enable 2>/dev/null \
                    && info "  VF${i} link-state=enable" || true
            else
                info "  VF${i} GUID + link-state OK ✓"
            fi
        done
    else
        warn "No IPoIB netdev for ${IB_PF_NAME} — cannot set VF GUIDs; VM-side RDMA may fail (no-SGID)"
    fi

    # Emit BDFs one per line, callers capture: map VF idx to BDF
    printf "%s\n" "${bdfs[@]}"
}

# ============================================================
# TAP 准备: tap0 tap1 都创建并接 powerfs-br0
# ============================================================
ensure_taps() {
    # find docker bridge
    local DOCKER_BRIDGE
    if ip link show powerfs-br0 &>/dev/null; then
        DOCKER_BRIDGE="powerfs-br0"
    else
        DOCKER_BRIDGE="$(docker network inspect docker_powerfs-network \
            --format '{{range .Options}}{{.}}{{end}}' 2>/dev/null | grep -oE 'br-[0-9a-f]{12}' | head -1)"
    fi
    if [ -z "${DOCKER_BRIDGE}" ] || ! ip link show "${DOCKER_BRIDGE}" &>/dev/null; then
        error "Docker powerfs-network bridge not found — start services first"
        return 1
    fi
    info "Docker bridge: ${DOCKER_BRIDGE}"

    local tap
    for tap in "${VM1_TAP}" "${VM2_TAP}"; do
        if ! ip link show "${tap}" &>/dev/null; then
            step "  Create TAP ${tap} + attach to ${DOCKER_BRIDGE}"
            sudo ip tuntap add dev "${tap}" mode tap
        else
            local cur_br
            cur_br="$(basename "$(readlink "/sys/class/net/${tap}/master" 2>/dev/null)" 2>/dev/null)"
            if [ "${cur_br}" != "${DOCKER_BRIDGE}" ]; then
                step "  Reattach TAP ${tap} (was ${cur_br:-none}) → ${DOCKER_BRIDGE}"
                sudo ip link set "${tap}" nomaster 2>/dev/null || true
                sudo ip link set "${tap}" master "${DOCKER_BRIDGE}" 2>/dev/null
            fi
        fi
        sudo ip link set "${tap}" up
        info "  TAP ${tap} up on bridge ${DOCKER_BRIDGE} ✓"
    done
}

# ============================================================
# Disk copy: vm2 需要独立 qcow2，从基础盘拷贝 (qcow2 内部 copy-on-write，用 cp 即可)
# ============================================================
ensure_vm_disk() {
    local vid="$1"
    local target
    target="$(vm_disk "${vid}")"
    if [ ! -f "${QEMU_DISK_BASE}" ]; then
        error "Base disk not found: ${QEMU_DISK_BASE} — run ./qemuctl.sh build"
        return 1
    fi
    if [ ! -f "${target}" ]; then
        step "  Copy base disk for $(vm_val "${vid}" NAME): ${target}"
        cp --sparse=always "${QEMU_DISK_BASE}" "${target}" 2>/dev/null || cp "${QEMU_DISK_BASE}" "${target}"
    fi
}

# ============================================================
# SR-IOV prepare public wrapper
# ============================================================
cmd_prepare_sriov() {
    title "Prepare SR-IOV VFIO + TAP network (no VM start)"
    ensure_sriov_and_vfio >/dev/null
    ensure_taps >/dev/null
    info "SR-IOV + TAP ready"
}

# ============================================================
# 启动单 VM
#   start_single <vm1|vm2> <VF_BDF>
# ============================================================
start_single() {
    local vid="$1"
    local vf_bdf="$2"
    local name ssh_port tap eip iip mac0 mac1
    name="$(vm_val "${vid}" NAME)"
    ssh_port="$(vm_val "${vid}" SSH)"
    tap="$(vm_val "${vid}" TAP)"
    eip="$(vm_val "${vid}" EIP)"
    iip="$(vm_val "${vid}" IIP)"
    mac0="$(vm_val "${vid}" MAC0)"
    mac1="$(vm_val "${vid}" MAC1)"
    local disk pidfile logfile share_tag
    disk="$(vm_disk "${vid}")"
    pidfile="$(vm_pid_file "${vid}")"
    logfile="$(vm_log "${vid}")"
    share_tag="$(vm_share_tag "${vid}")"

    title "Start ${name} — ssh=:${ssh_port} eth0=${eip} ib0=${iip} VF=${vf_bdf}"

    if is_vm_running "${vid}"; then
        warn "${name} already running (PID: $(get_vm_qemu_pid "${vid}"))"
        return 0
    fi

    # cmdline: vm_ip= for eth0, add ib_ip= for ib0 auto-config if initramfs supports it
    # (fallback: each test script can set ib0 manually; this param is friendly sugar)
    local cmdline
    cmdline="console=ttyS0 root=/dev/ram0 rw init=/init loglevel=4"
    cmdline="${cmdline} net.ifnames=0 biosdevname=0"
    cmdline="${cmdline} vm_ip=${eip} ib_ip=${iip}"
    cmdline="${cmdline} powerfs_master_addr=${POWERFS_MASTER_ADDR}"
    cmdline="${cmdline} powerfs_master_port=${POWERFS_MASTER_PORT}"
    cmdline="${cmdline} kasan=off"
    cmdline="${cmdline} softlockup_thresh=10 hung_task_timeout_secs=60"
    cmdline="${cmdline} rcupdate.rcu_cpu_stall_timeout=21 wq_watchdog_thresh=30"
    cmdline="${cmdline} panic_on_oops=1 softlockup_panic=1 hardlockup_panic=1"
    cmdline="${cmdline} hung_task_panic=1 rcupdate.rcu_cpu_stall_panic=1 panic=-1"

    info "  kernel:   ${KERNEL_IMAGE}"
    info "  initramfs:${INITRAMFS}"
    info "  disk:     ${disk}"
    info "  tap:      ${tap}  → eth0 ${eip}/16 gw=${GATEWAY}"
    info "  user-net: :${ssh_port} → sshd 10.0.2.15"
    info "  vfio:     ${vf_bdf}  → VM ib0 ${iip}/24"

    # kvm flag
    local kvm_flag=""
    [ -c /dev/kvm ] && kvm_flag="-enable-kvm"

    # Note: 9p share_tag must be unique per VM (same sharedir OK, tag must differ)
    sudo nohup qemu-system-x86_64 \
        ${kvm_flag} \
        -name "powerfs-${name},debug-threads=on" \
        -m "${MEM_SIZE}" \
        -smp "${CPU_CORES}" \
        -kernel "${KERNEL_IMAGE}" \
        -initrd "${INITRAMFS}" \
        -append "${cmdline}" \
        -drive file="${disk}",format=qcow2,if=virtio \
        -netdev tap,id=net0,ifname=${tap},script=no,downscript=no \
        -device virtio-net-pci,netdev=net0,mac=${mac0} \
        -netdev user,id=net1,hostfwd=tcp::${ssh_port}-:22 \
        -device e1000,netdev=net1,mac=${mac1} \
        -virtfs local,path=${SHARE_DIR},mount_tag=${share_tag},security_model=mapped-xattr,id=${share_tag} \
        -device vfio-pci,host=${vf_bdf} \
        -nographic \
        -serial file:"${logfile}" \
        -monitor none \
        -display none \
        -pidfile "${pidfile}.tmp" \
        > "${logfile}.stdout" 2>&1 &

    local launcher_pid=$!
    disown ${launcher_pid} 2>/dev/null || true
    sleep 1

    # Copy pid from pidfile.tmp to canonical pidfile (qemu itself writes it;
    # pidfile.tmp owned by qemu (root), we need sudo to read it).
    for _ in 1 2 3 4 5; do
        if [ -f "${pidfile}.tmp" ]; then
            sudo cat "${pidfile}.tmp" > "${pidfile}" 2>/dev/null
            sudo rm -f "${pidfile}.tmp" 2>/dev/null
            [ -s "${pidfile}" ] && break
        fi
        sleep 1
    done
    if [ ! -s "${pidfile}" ]; then
        echo "${launcher_pid}" > "${pidfile}"
    fi

    sleep 2
    if ! is_vm_running "${vid}"; then
        error "${name} failed to start — see ${logfile}"
        tail -10 "${logfile}" 2>/dev/null
        return 1
    fi
    info "${name} QEMU up (PID $(cat "${pidfile}")), waiting SSH..."

    local tries=0
    while [ ${tries} -lt 40 ]; do
        sleep 2
        if ssh_vm "${vid}" "echo OK" 2>/dev/null | grep -q "OK"; then
            info "${name} SSH ready (localhost:${ssh_port})"
            # SSH up → initialize IB/RDMA networking inside VM once:
            #   a) ib0 UP + flush old addr + assign ib_ip/${iip}
            #   b) 192.168.100.0/24 route metric 10 via ib0 (HIGHER PRIO than eth0 172.30 default
            #      route via 172.30.0.1), otherwise rdma_resolve_addr picks eth0 for dest 192.168 and
            #      returns -ENODEV (no RDMA device on eth0).
            ssh_vm "${vid}" >/dev/null 2>&1 <<VM_INIT_NET || true
ip link set ib0 up 2>/dev/null
ip addr flush dev ib0 2>/dev/null
ip addr add ${iip}/24 dev ib0
# IB subnet route via ib0 with low metric (higher priority)
ip route add 192.168.100.0/24 dev ib0 proto kernel scope link src ${iip} metric 10 2>/dev/null
# Verify route
ping -c 1 -W 1 -I ib0 192.168.100.3 >/dev/null 2>&1 || true
VM_INIT_NET
            return 0
        fi
        tries=$((tries + 1))
        printf "."
    done
    echo ""
    warn "${name} SSH not ready after 80s — check ${logfile}"
    return 1
}

# ============================================================
# 停止单 VM
# ============================================================
stop_single() {
    local vid="$1"
    local name
    name="$(vm_val "${vid}" NAME)"
    title "Stop ${name}"

    if ! is_vm_running "${vid}"; then
        info "${name} not running"
        rm -f "$(vm_pid_file "${vid}")"
        return 0
    fi

    local pids
    pids="$(pgrep -f "powerfs-${name}\|$(vm_qemu_match "${vid}")")"
    [ -z "${pids}" ] && pids="$(get_vm_qemu_pid "${vid}")"
    if [ -z "${pids}" ]; then
        warn "no qemu process for ${name} — cleaning pidfile"
        rm -f "$(vm_pid_file "${vid}")"
        return 0
    fi
    info "  kill ${pids}"
    sudo kill ${pids} 2>/dev/null || true

    for _ in $(seq 1 10); do
        if ! is_vm_running "${vid}"; then
            info "${name} exited cleanly"
            sudo rm -f "$(vm_disk "${vid}").lock" 2>/dev/null || true
            rm -f "$(vm_pid_file "${vid}")"
            return 0
        fi
        sleep 0.5
    done
    warn "${name} still alive, kill -9"
    sudo kill -9 $(pgrep -f "powerfs-${name}\|$(vm_qemu_match "${vid}")") 2>/dev/null || true
    sleep 1
    sudo rm -f "$(vm_disk "${vid}").lock" 2>/dev/null
    rm -f "$(vm_pid_file "${vid}")"
}

# ============================================================
# Commands
# ============================================================
cmd_start() {
    title "START 2 QEMU VMs (Hardware RDMA SR-IOV VFIO)"

    # 若原单 VM 已启动 (tap0 + 老的 qemuctl 方式), 先停掉, 防止与 vm1 冲突
    if pgrep -f "qemu-system-x86_64.*${KERNEL_IMAGE}" >/dev/null 2>&1; then
        # Only kill if it's the qemuctl one (no -name powerfs-vm1/vm2 marker + no hostshare_vm tag)
        local legacy_pid
        legacy_pid="$(pgrep -f "qemu-system-x86_64.*${KERNEL_IMAGE}" | head -1)"
        if [ -n "${legacy_pid}" ]; then
            # Only kill if cmdline lacks powerfs-vm* marker (legacy qemuctl)
            local legacy_cmd
            legacy_cmd="$(ps -p ${legacy_pid} -o args= 2>/dev/null || true)"
            if ! echo "${legacy_cmd}" | grep -q "powerfs-vm"; then
                warn "Detected legacy single-VM QEMU — stopping to release VF + tap0"
                sudo kill "${legacy_pid}" 2>/dev/null; sleep 2
                sudo kill -9 "${legacy_pid}" 2>/dev/null; sleep 1
                sudo rm -f "${OUTPUT_DIR}/qemu_disk.img.lock" 2>/dev/null
                rm -f "${OUTPUT_DIR}/qemu.pid"
            fi
        fi
    fi

    step "[1/5] SR-IOV: ensure ${IB_PF_NAME} has ${IB_NUM_VFS} VFs + vfio bind"
    local vf_bdfs
    vf_bdfs="$(ensure_sriov_and_vfio)"
    if [ $? -ne 0 ] || [ -z "${vf_bdfs}" ]; then
        error "SR-IOV setup failed — ensure VT-d/IOMMU enabled, mlx5_core + vfio-pci loaded"
        exit 1
    fi
    local -a BDFS=()
    while IFS= read -r line; do [ -n "${line}" ] && BDFS+=("${line}"); done <<<"${vf_bdfs}"
    if [ "${#BDFS[@]}" -lt "${IB_NUM_VFS}" ]; then
        error "Expected ${IB_NUM_VFS} VF BDFs, got ${#BDFS[@]}: ${BDFS[*]}"
        exit 1
    fi
    info "  VF0=${BDFS[0]}  VF1=${BDFS[1]}"

    step "[2/5] Network: create TAP ${VM1_TAP}/${VM2_TAP} + bridge attach"
    ensure_taps || { error "TAP setup failed"; exit 1; }

    step "[3/5] Per-VM qcow2 disk (base → vm1 / vm2 independent copy)"
    ensure_vm_disk vm1
    ensure_vm_disk vm2

    # start vm2 first (new VF), then vm1 — vm1 uses the VF the legacy config was using
    step "[4/5] Launch VM2 (SSH=:${VM2_SSH_PORT}, eth0=${VM2_ETH0_IP}, ib0=${VM2_IB0_IP})"
    start_single vm2 "${BDFS[1]}"
    local vm2_rc=$?

    step "[5/5] Launch VM1 (SSH=:${VM1_SSH_PORT}, eth0=${VM1_ETH0_IP}, ib0=${VM1_IB0_IP})"
    start_single vm1 "${BDFS[0]}"
    local vm1_rc=$?

    echo ""
    if [ ${vm1_rc} -eq 0 ] && [ ${vm2_rc} -eq 0 ]; then
        echo -e "${GREEN}==== BOTH VMs STARTED OK ====${NC}"
    else
        echo -e "${RED}==== SOME VMS FAILED ====${NC} (vm1_rc=${vm1_rc} vm2_rc=${vm2_rc})"
    fi
    echo "  VM1  SSH:  sshpass -p${SSH_PASS} ssh -p${VM1_SSH_PORT} ${SSH_USER}@localhost"
    echo "         or: ${0} ssh vm1"
    echo "         or: ${0} exec vm1 \"command\""
    echo "         eth0: ${VM1_ETH0_IP} (docker net)   ib0: ${VM1_IB0_IP} (RDMA IPoIB)"
    echo "  VM2  SSH:  sshpass -p${SSH_PASS} ssh -p${VM2_SSH_PORT} ${SSH_USER}@localhost"
    echo "         or: ${0} ssh vm2"
    echo "         eth0: ${VM2_ETH0_IP} (docker net)   ib0: ${VM2_IB0_IP} (RDMA IPoIB)"
    echo "  serial logs: $(vm_log vm1)   $(vm_log vm2)"
    echo "  Stop both:   ${0} stop"
}

cmd_stop() {
    stop_single vm1
    stop_single vm2
    info "Both VMs stopped"
}

cmd_restart() {
    cmd_stop
    sleep 1
    cmd_start
}

cmd_status() {
    title "VM Status"
    local vid
    for vid in vm1 vm2; do
        local name ssh_port eip iip pid state
        name="$(vm_val "${vid}" NAME)"
        ssh_port="$(vm_val "${vid}" SSH)"
        eip="$(vm_val "${vid}" EIP)"
        iip="$(vm_val "${vid}" IIP)"
        if is_vm_running "${vid}"; then
            pid="$(get_vm_qemu_pid "${vid}")"
            state="UP (pid=${pid})"
            local ssh_ok="N/A" ib_state="N/A" ib_ip="N/A"
            if ssh_vm "${vid}" "echo OK" 2>/dev/null | grep -q "OK"; then
                ssh_ok="OK"
                ib_state="$(ssh_vm "${vid}" "ibstat 2>/dev/null | grep -E 'State:|Physical state:' | tr -s ' ' | paste - -" 2>/dev/null | head -1)"
                ib_ip="$(ssh_vm "${vid}" "ip -4 addr show ib0 2>/dev/null | grep inet | awk '{print \$2}' | head -1" 2>/dev/null)"
            fi
            printf "  ${GREEN}%-5s${NC}  %-26s  SSH=%-3s  eth0=%-15s  ib0=%-21s  verbs=%s\n" \
                "${vid^^}" "${state}" "${ssh_ok}" "${eip}" "${ib_ip:-n/a}" "${ib_state:-n/a}"
        else
            state="DOWN"
            printf "  ${RED}%-5s${NC}  %-26s  SSH=%-3s  eth0=%-15s  ib0=%-21s  verbs=%s\n" \
                "${vid^^}" "${state}" "-" "${eip}" "${iip} (cfg)" "-"
        fi
    done
}

ssh_with_login() {
    local vid="$1"
    if ! is_vm_running "${vid}"; then
        error "$(vm_val "${vid}" NAME) not running"
        return 1
    fi
    exec sshpass -p "${SSH_PASS}" ssh \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=8 \
        -p "$(vm_val "${vid}" SSH)" \
        "${SSH_USER}@localhost"
}

exec_in_vm() {
    local vid="$1"; shift
    if ! is_vm_running "${vid}"; then
        error "$(vm_val "${vid}" NAME) not running"; return 1
    fi
    ssh_vm "${vid}" "$@"
}

log_vm() {
    local vid="$1"
    local kw="${2:-}"
    if ! is_vm_running "${vid}"; then
        error "$(vm_val "${vid}" NAME) not running"; return 1
    fi
    if [ -n "${kw}" ]; then
        ssh_vm "${vid}" "dmesg | grep -iE '${kw}'"
    else
        ssh_vm "${vid}" "dmesg"
    fi
}

serial_vm() {
    local vid="$1"
    local N="${2:-100}"
    local f
    f="$(vm_log "${vid}")"
    if [ ! -f "${f}" ]; then
        error "no serial log for ${vid}: ${f}"
        return 1
    fi
    tail -n "${N}" "${f}"
}

mount_vm() {
    local vid="$1"
    if ! is_vm_running "${vid}"; then
        error "$(vm_val "${vid}" NAME) not running"; return 1
    fi
    local iip
    iip="$(vm_val "${vid}" IIP)"
    info "Mount PowerFS on $(vm_val "${vid}" NAME) with transport=rdma (ib0=${iip})"
    ssh_vm "${vid}" "
set +e
CA=/etc/powerfs/ca.crt
CRT=/etc/powerfs/kernel-client-1.crt
KEY=/etc/powerfs/kernel-client-1.key
# Ensure IB net config: flush → addr → route (same priority rule as start_single)
ip link set ib0 up 2>/dev/null
ip addr flush dev ib0 2>/dev/null
ip addr add ${iip}/24 dev ib0
ip route del 192.168.100.0/24 table main 2>/dev/null
ip route add 192.168.100.0/24 dev ib0 proto kernel scope link src ${iip} metric 10 2>/dev/null
# Sanity check RDMA link pingable via ib0
if ! ping -c 2 -W 1 -I ib0 192.168.100.3 >/dev/null 2>&1; then
  echo 'IB_UNREACHABLE: cannot ping 192.168.100.3 via ib0'
  echo 'current route:'; ip route get 192.168.100.3
  exit 2
fi
# Clean prior state
umount -l /mnt/powerfs 2>/dev/null; sleep 1
lsmod | grep -q powerfs && ( rmmod powerfs 2>/dev/null; sleep 1 )
# ============================================================
# ROOT36-D defensive 9p mount: ensure /mnt/host is really mounted before
# insmod /mnt/host/powerfs.ko.  Running VMs were launched with qemu
# mount_tag=hostshare_\$name (old qemuctl2 before tag sync fix), while
# fresh boots via new build_initramfs.sh try mount_tag=hostshare first.
# Try all tags; only treat as mounted if we can actually stat powerfs.ko.
# If none work we fall back to initramfs-built-in /powerfs.ko with a WARN.
# ============================================================
mkdir -p /mnt/host 2>/dev/null
if [ ! -f /mnt/host/powerfs.ko ]; then
  umount /mnt/host 2>/dev/null
  for _tag in hostshare hostshare_vm1 hostshare_vm2; do
    mount -t 9p -o trans=virtio,version=9p2000.L "\${_tag}" /mnt/host >/dev/null 2>&1
    if [ -f /mnt/host/powerfs.ko ]; then
      _ksz=\$(stat -c%s /mnt/host/powerfs.ko 2>/dev/null)
      echo '9P_HOST_OK: mounted tag='"\${_tag}"' /mnt/host powerfs.ko size='"\${_ksz}"
      break
    fi
    umount /mnt/host 2>/dev/null
  done
fi
if [ -f /mnt/host/powerfs.ko ]; then
  insmod /mnt/host/powerfs.ko && echo 'INSMOD_OK: /mnt/host/powerfs.ko'
else
  echo 'WARN_9P_FALLBACK: /mnt/host powerfs.ko missing - use builtin /powerfs.ko'
  insmod /powerfs.ko 2>/dev/null
fi
timeout 35 mount -t powerfs \
  -o master_addr=172.30.0.1,master_port=9334,shard_count=1 \
  -o ca_crt=\${CA},client_crt=\${CRT},client_key=\${KEY} \
  -o transport=rdma powerfs /mnt/powerfs
RC=\$?
echo MOUNT_RC=\${RC}
# RELIABLE check: /proc/mounts powerfs line with transport=rdma must exist
grep -E '^powerfs /mnt/powerfs powerfs .*transport=rdma' /proc/mounts >/dev/null 2>&1 &&
  ( echo 'PROC_MOUNTS_CHECK=PASS'
    mount | grep powerfs
    # sanity readdir (timeout 5s avoids hanging on stale session)
    timeout 5 ls /mnt/powerfs ) ||
  ( echo 'PROC_MOUNTS_CHECK=FAIL (transport=rdma not actually mounted)'
    dmesg | tail -6 )
" 2>&1
}

umount_vm() {
    local vid="$1"
    if ! is_vm_running "${vid}"; then
        info "$(vm_val "${vid}" NAME) not running — noop"
        return 0
    fi
    ssh_vm "${vid}" 'umount -l /mnt/powerfs 2>/dev/null; sleep 1; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; rmmod powerfs 2>/dev/null; echo UMK_RC=$?; lsmod | grep powerfs || echo "powerfs unloaded"' 2>&1
}

# ============================================================
# Usage / dispatch
# ============================================================
usage() {
    grep '^# ' "${BASH_SOURCE[0]}" | head -60
}

CMD="${1:-}"
[ $# -ge 1 ] && shift
case "${CMD}" in
    start)          cmd_start "$@" ;;
    stop)           cmd_stop ;;
    restart)        cmd_restart ;;
    status)         cmd_status ;;
    ssh)            ssh_with_login "${1:-vm1}" ;;
    exec)           exec_in_vm "${1:-vm1}" "${*:2}" ;;
    log)            log_vm "${1:-vm1}" "${2:-}" ;;
    serial)         serial_vm "${1:-vm1}" "${2:-100}" ;;
    mount)          mount_vm "${1:-vm1}" ;;
    umount)         umount_vm "${1:-vm1}" ;;
    prepare-sriov)  cmd_prepare_sriov ;;
    ""|-h|--help|help) usage ;;
    *) error "Unknown command: ${CMD}"; usage ;;
esac
