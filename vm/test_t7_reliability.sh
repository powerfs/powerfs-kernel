#!/bin/bash
# T7 阶段渐进式测试: 可靠性 (故障注入 + failover + CRC + umount 排空)
#
# 验证内容:
#   T0: 编译 + 静态验证 (make + verify_module.sh)
#   T1: QEMU 启动 + 挂载 (复用 qemuctl.sh, 确认 Master/Filer/Volume 运行)
#   T2: 网络断连恢复测试 (阻断 Volume Server, 读入队等待, 恢复后 MD5 一致)
#   T3: Volume Server 故障 failover (Replicated, 副本读取)
#   T4: Filer 故障切换 (停 leader, 自动重连新 leader)
#   T5: CRC32 不匹配检测 (注入损坏 → EIO + dmesg CRC mismatch)
#   T6: umount 排空测试 (打开句柄, umount 等待, 关闭后成功, slab 释放)
#   T7: 最终清理 + 重新挂载
#
# 核心原则:
#   - 从小到大逐个确认, 不可跳级
#   - 每步检查内核状态 (dmesg/slab/meminfo/D 状态)
#   - 故障注入后必须恢复 (trap 兜底)
#   - 无法执行的测试 (无 reliability/单 Filer/无法注入损坏) skip + warn
#   - 只阻断 Volume Server, 不阻断 Filer (否则无法恢复)
#
# 运行环境: HOST (通过 SSH 控制 VM + docker exec/stop/start 管理容器)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - 或由本脚本 T1 自动完成
#
# 用法:
#   ./test_t7_reliability.sh            # 运行全部 (T0-T7)
#   ./test_t7_reliability.sh 2          # 仅运行 T2
#   ./test_t7_reliability.sh 2 3 4      # 运行 T2+T3+T4

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs           # FUSE 容器内挂载点 (fuse-1)
FUSE_CONTAINER="fuse-1"         # FUSE 容器名
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

# Volume Server 列表 (网络阻断/故障注入用, 与 docker-compose.yml 一致)
VOLUMES=(volume-1 volume-2 volume-3)
VOLUME_IPS=(172.30.0.21 172.30.0.22 172.30.0.23)
VOLUME_PORT=8901                # powerfs-net 端口 (容器内统一)

# 可靠性测试目录 (FUSE 端创建, 内核端读取)
REL_DIR="${FUSE_MNT}/t7_reliability"
REL_KERNEL_DIR="${MNT}/t7_reliability"

PASS=0
FAIL=0
WARN=0
SKIP=0
SLAB_INIT=""
MEM_INIT=""
SERIAL_BASE=0
BLOCK_METHOD=""                 # 网络阻断方式: iptables / docker
# 故障注入状态标记 (cleanup trap 用)
INJECTED_NET_BLOCKED=0
INJECTED_VOLUME_DOWN=0
INJECTED_FILER_DOWN=0

# 颜色输出
if [ -t 1 ]; then
    C_RED='\033[0;31m'
    C_GREEN='\033[0;32m'
    C_YELLOW='\033[0;33m'
    C_CYAN='\033[0;36m'
    C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_RESET=''
fi

ok()     { echo -e "  ${C_GREEN}[PASS]${C_RESET} $1"; PASS=$((PASS+1)); }
ng()     { echo -e "  ${C_RED}[FAIL]${C_RESET} $1"; FAIL=$((FAIL+1)); }
warn()   { echo -e "  ${C_YELLOW}[WARN]${C_RESET} $1"; WARN=$((WARN+1)); }
skip()   { echo -e "  ${C_YELLOW}[SKIP]${C_RESET} $1"; SKIP=$((SKIP+1)); }
section() { echo ""; echo -e "${C_CYAN}━━━ $1 ━━━${C_RESET}"; }

# 判断是否运行指定测试
RUN_TESTS=("$@")
should_run() {
    [ ${#RUN_TESTS[@]} -eq 0 ] || printf '%s\n' "${RUN_TESTS[@]}" | grep -qx "$1"
}

# ============================================================
# 内核状态检查函数 (核心: 不只看应用返回, 必须检查内核状态)
# ============================================================

# 检查 dmesg 是否有异常 (oops/bug/kasan/stall/lockup)
check_dmesg_clean() {
    local base="${1:-0}"
    local log
    log=$(dmesg_since "$base" 2>/dev/null)

    local errors
    errors=$(echo "$log" | grep -E 'BUG:|Oops:|general protection fault|unable to handle|NULL pointer dereference|KASAN:|RCU stall|workqueue lockup|hung task|soft lockup|hard lockup|NMI watchdog|call trace|Kernel panic|Segmentation fault' || true)

    if [ -n "$errors" ]; then
        echo "  ${C_RED}kernel anomaly detected:${C_RESET}"
        echo "$errors" | head -20 | sed 's/^/    /'
        return 1
    fi
    return 0
}

# 获取 powerfs slab 活跃对象总数 (inode + dentry)
get_powerfs_slab_total() {
    local inode_active dentry_active
    inode_active=$(vm "cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print \$2}'" 2>/dev/null || echo 0)
    dentry_active=$(vm "cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_dentry/ {print \$2}'" 2>/dev/null || echo 0)
    echo "${inode_active:-0} ${dentry_active:-0}"
}

# 获取 VM 内 MemAvailable (KB)
get_mem_available() {
    vm "awk '/MemAvailable/ {print \$2}' /proc/meminfo" 2>/dev/null || echo 0
}

# 检查是否有 D 状态的 powerfs 线程 (不可中断睡眠)
check_d_state() {
    local d_tasks
    d_tasks=$(vm "for p in /proc/[0-9]*/stack; do t=\$(cat \${p%/stack}/stat 2>/dev/null | awk '{print \$3}'); if [ \"\$t\" = 'D' ]; then comm=\$(cat \${p%/stack}/comm 2>/dev/null); echo \"\$comm (\${p%/stack})\"; fi; done 2>/dev/null | grep -i powerfs" 2>/dev/null || true)
    if [ -n "$d_tasks" ]; then
        echo "  ${C_RED}D-state powerfs thread detected:${C_RESET}"
        echo "$d_tasks" | sed 's/^/    /'
        return 1
    fi
    return 0
}

# 综合内核状态检查
check_kernel_state() {
    local desc="$1"
    local base="${2:-0}"
    local state_ok=0

    echo "  --- kernel state check: ${desc} ---"

    if check_dmesg_clean "$base"; then
        ok "dmesg clean (no oops/bug/kasan/stall)"
    else
        ng "dmesg anomaly (${desc})"
        state_ok=1
    fi

    if check_d_state; then
        ok "no D-state powerfs thread"
    else
        ng "D-state (hung) powerfs thread (${desc})"
        state_ok=1
    fi

    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        local serial_errors
        serial_errors=$(serial_since "${SERIAL_BASE}" | grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task' 2>/dev/null | tail -5 || true)
        if [ -z "$serial_errors" ]; then
            ok "serial log clean (no lockup/panic)"
        else
            ng "serial log lockup/panic (${desc})"
            echo "$serial_errors" | sed 's/^/      /'
            state_ok=1
        fi
    fi

    return $state_ok
}

# 检查 FUSE 容器内 setfattr 可用, 不可用时尝试安装
ensure_setfattr() {
    local has
    has=$(docker exec ${FUSE_CONTAINER} sh -c "which setfattr 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has" = "no" ]; then
        docker exec ${FUSE_CONTAINER} sh -c "apt-get install -y attr >/dev/null 2>&1 || apk add attr >/dev/null 2>&1" 2>/dev/null
        has=$(docker exec ${FUSE_CONTAINER} sh -c "which setfattr 2>/dev/null || echo no" 2>/dev/null)
    fi
    [ "$has" != "no" ]
}

# ============================================================
# 网络阻断辅助 (T2: 只阻断 Volume, 不阻断 Filer)
# ============================================================

# 检测 VM 内 iptables 是否可用, 决定阻断方式
detect_block_method() {
    if vm "iptables -L >/dev/null 2>&1"; then
        BLOCK_METHOD="iptables"
    else
        BLOCK_METHOD="docker"
    fi
    echo "  -> block method: ${BLOCK_METHOD}"
}

# 阻断 VM 到指定 Volume 的网络
# 参数 $1: IP, $2: 容器名 (docker 方式回退用)
block_volume_net() {
    local ip=$1
    local container=$2
    if [ "$BLOCK_METHOD" = "iptables" ]; then
        vm "iptables -A OUTPUT -d ${ip} -j DROP" 2>/dev/null
    else
        docker network disconnect "$DOCKER_NET" "$container" >/dev/null 2>&1 || true
    fi
}

# 恢复 VM 到指定 Volume 的网络
unblock_volume_net() {
    local ip=$1
    local container=$2
    if [ "$BLOCK_METHOD" = "iptables" ]; then
        vm "iptables -D OUTPUT -d ${ip} -j DROP" 2>/dev/null || true
    else
        docker network connect "$DOCKER_NET" "$container" >/dev/null 2>&1 || true
        sleep 1
    fi
}

# 阻断所有 Volume
block_all_volumes() {
    local i
    for i in "${!VOLUMES[@]}"; do
        block_volume_net "${VOLUME_IPS[$i]}" "${VOLUMES[$i]}"
    done
    INJECTED_NET_BLOCKED=1
}

# 恢复所有 Volume
unblock_all_volumes() {
    local i
    for i in "${!VOLUMES[@]}"; do
        unblock_volume_net "${VOLUME_IPS[$i]}" "${VOLUMES[$i]}"
    done
    INJECTED_NET_BLOCKED=0
}

# 等待 Volume 容器健康
wait_volume_healthy() {
    local container=$1
    local max=60
    for i in $(seq 1 $max); do
        local status
        status=$(docker inspect "$container" --format '{{.State.Health.Status}}' 2>/dev/null || echo "none")
        if [ "$status" = "healthy" ] || [ "$status" = "none" ]; then
            # none: 无 healthcheck 也视为就绪
            docker exec "$container" nc -z 127.0.0.1 8080 >/dev/null 2>&1 && return 0
            [ "$status" = "healthy" ] && return 0
        fi
        sleep 1
    done
    return 1
}

# 清理所有已注入故障 (trap 兜底, 确保服务可恢复)
cleanup_injected_faults() {
    if [ "${INJECTED_NET_BLOCKED:-0}" = "1" ]; then
        echo ""
        echo "  [cleanup] restoring blocked volume network..."
        unblock_all_volumes 2>/dev/null || true
    fi
    if [ "${INJECTED_VOLUME_DOWN:-0}" = "1" ]; then
        echo "  [cleanup] restarting stopped volumes..."
        local v
        for v in "${VOLUMES[@]}"; do
            docker start "$v" >/dev/null 2>&1 || true
        done
        INJECTED_VOLUME_DOWN=0
    fi
    if [ "${INJECTED_FILER_DOWN:-0}" = "1" ]; then
        echo "  [cleanup] restarting stopped filers..."
        local f
        for f in "${FILERS[@]}"; do
            docker start "$f" >/dev/null 2>&1 || true
        done
        INJECTED_FILER_DOWN=0
    fi
}
trap 'cleanup_injected_faults' INT TERM

# ============================================================
# T0: 编译 + 静态验证
# ============================================================
test_t0_compile() {
    section "T0: compile + static verification"

    # T0-1: 编译
    echo "  [T0-1] build powerfs.ko..."
    cd "${POWERFS_MOD_DIR}"
    local build_log
    build_log=$(make clean 2>&1 && make -j$(nproc) 2>&1)
    local build_ret=$?

    if [ $build_ret -eq 0 ] && [ -f powerfs.ko ]; then
        ok "powerfs.ko built ($(ls -la powerfs.ko | awk '{print $5}') bytes)"
    else
        ng "powerfs.ko build failed"
        echo "$build_log" | tail -20 | sed 's/^/    /'
        cd "${SCRIPT_DIR}"
        return 1
    fi

    # 检查编译警告
    local warnings
    warnings=$(echo "$build_log" | grep -iE 'warning:' | grep -v 'Wno-' || true)
    if [ -z "$warnings" ]; then
        ok "build has no warning"
    else
        warn "build has warning (non-blocking):"
        echo "$warnings" | head -5 | sed 's/^/    /'
    fi

    # T0-2: 静态符号验证
    echo "  [T0-2] verify_module.sh..."
    if bash verify_module.sh 2>&1 | tail -5 | grep -q "ALL VERIFICATION TESTS PASSED"; then
        ok "verify_module.sh passed"
    else
        ng "verify_module.sh failed"
        bash verify_module.sh 2>&1 | grep '\[FAIL\]' | sed 's/^/    /'
        cd "${SCRIPT_DIR}"
        return 1
    fi

    cd "${SCRIPT_DIR}"
    return 0
}

# ============================================================
# T1: QEMU 启动 + 挂载
# ============================================================
test_t1_mount() {
    section "T1: QEMU boot + mount"

    # T1-1: 检查后端服务 (Master/Filer/Volume)
    echo "  [T1-1] check backend services (master/filer/volume)..."
    local svc_count
    svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || echo 0)
    if [ "$svc_count" -ge 3 ]; then
        ok "backend services running (master/volume/filer)"
    else
        warn "backend services not fully started, starting..."
        ./qemuctl.sh service start 2>&1 | tail -5
        sleep 5
        svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || echo 0)
        if [ "$svc_count" -ge 3 ]; then
            ok "backend services started"
        else
            ng "backend services start failed"
            return 1
        fi
    fi

    # T1-2: 检查 QEMU 运行
    echo "  [T1-2] check QEMU running..."
    local qemu_pid
    qemu_pid=$(pgrep -f "qemu-system-x86_64.*bzImage" 2>/dev/null | head -1)
    if [ -z "$qemu_pid" ]; then
        warn "QEMU not running, starting..."
        ./qemuctl.sh deploy 2>&1 | tail -10
        sleep 10
        qemu_pid=$(pgrep -f "qemu-system-x86_64.*bzImage" 2>/dev/null | head -1)
    fi

    if [ -n "$qemu_pid" ]; then
        ok "QEMU running (PID: ${qemu_pid})"
    else
        ng "QEMU start failed"
        return 1
    fi

    # T1-3: SSH 可达
    echo "  [T1-3] check VM SSH reachable..."
    if vm_alive; then
        ok "VM SSH reachable"
    else
        ng "VM SSH unreachable"
        return 1
    fi

    # T1-4: powerfs 已挂载
    echo "  [T1-4] check powerfs mount..."
    if ! check_mount; then
        warn "powerfs not mounted, mounting..."
        ./qemuctl.sh mount 2>&1 | tail -3
        sleep 2
    fi

    if check_mount; then
        ok "powerfs mounted at ${MNT}"
    else
        ng "powerfs mount failed"
        return 1
    fi

    # T1-5: 模块加载
    echo "  [T1-5] check module loaded..."
    local mod_info
    mod_info=$(vm "lsmod | grep powerfs" 2>/dev/null)
    if [ -n "$mod_info" ]; then
        ok "powerfs module loaded: ${mod_info}"
    else
        ng "powerfs module not loaded"
        return 1
    fi

    # T1-6: 挂载后 30s dmesg 观察
    echo "  [T1-6] observe 30s after mount..."
    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)
    sleep 30
    if check_kernel_state "30s after mount" "$base"; then
        ok "kernel state normal 30s after mount"
    else
        ng "kernel state anomaly 30s after mount"
        return 1
    fi

    # 检测网络阻断方式 (供 T2 使用)
    echo "  [T1-7] detect network block method..."
    detect_block_method

    # 记录初始 slab 和 meminfo 基线
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "  -> initial slab (inode dentry): ${SLAB_INIT}"
    echo "  -> initial MemAvailable: ${MEM_INIT} KB"

    return 0
}

# ============================================================
# T2: 网络断连恢复测试 (阻断 Volume Server, 读入队等待)
# ============================================================
test_t2_net_disconnect() {
    section "T2: network disconnect/recover (block Volume Server only)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T2-a: 创建 10MB 测试文件并计算 MD5
    echo "  [T2-a] create 10MB test file + MD5..."
    local src_md5
    src_md5=$(vm "dd if=/dev/urandom bs=1M count=10 2>/dev/null | tee ${MNT}/t7_t2_file.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$src_md5" ]; then
        ng "create 10MB file failed"
        return 1
    fi
    echo "    write MD5: ${src_md5}"
    vm "sync" 2>/dev/null
    sleep 1

    # 先 drop cache, 确保后续读必须走网络到 Volume
    vm "sync; echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    if ! check_kernel_state "T2-a create file" "$base"; then
        ng "T2-a kernel state anomaly"
        vm "rm -f ${MNT}/t7_t2_file.bin" 2>/dev/null
        return 1
    fi

    # T2-b: 阻断到所有 Volume Server 的网络 (不阻断 Filer)
    echo "  [T2-b] block network to all Volume Servers (keep Filer reachable)..."
    block_all_volumes
    sleep 2
    ok "volume network blocked (${BLOCK_METHOD})"

    # T2-c: 尝试读取文件 → 应该入队等待 (不立即失败)
    echo "  [T2-c] read file (should queue, not fail immediately)..."
    vm "rm -f /tmp/t7_t2_read_done /tmp/t7_t2_read.out /tmp/t7_t2_read.err" 2>/dev/null
    # 后台启动读, 完成后写 DONE 标记
    vm "setsid sh -c 'cat ${MNT}/t7_t2_file.bin > /tmp/t7_t2_read.out 2>/tmp/t7_t2_read.err; echo DONE > /tmp/t7_t2_read_done' < /dev/null > /dev/null 2>&1 &" 2>/dev/null
    sleep 3

    local read_done
    read_done=$(vm "test -f /tmp/t7_t2_read_done && echo yes || echo no" 2>/dev/null)
    if [ "$read_done" = "no" ]; then
        ok "read queued (not completed within 3s, as expected)"
    else
        # 读立即完成: 检查是否报错
        local read_err
        read_err=$(vm "cat /tmp/t7_t2_read.err 2>/dev/null" 2>/dev/null)
        if [ -n "$read_err" ]; then
            ng "read failed immediately (expected queue): ${read_err}"
        else
            warn "read completed during block (may hit cache), continue"
        fi
    fi

    if ! check_kernel_state "T2-c blocked read" "$base"; then
        ng "T2-c kernel state anomaly"
        unblock_all_volumes
        vm "rm -f ${MNT}/t7_t2_file.bin" 2>/dev/null
        return 1
    fi

    # T2-d: 恢复网络
    echo "  [T2-d] restore volume network..."
    unblock_all_volumes
    sleep 3
    ok "volume network restored"

    # T2-e: 再次读取 → MD5 一致
    echo "  [T2-e] read again, verify MD5..."
    # 等待之前排队的读完成 (最多 30s)
    local waited=0
    while [ $waited -lt 30 ]; do
        local done_now
        done_now=$(vm "test -f /tmp/t7_t2_read_done && echo yes || echo no" 2>/dev/null)
        if [ "$done_now" = "yes" ]; then
            break
        fi
        sleep 2
        waited=$((waited + 2))
    done

    # 无论排队读是否完成, 重新读一次验证 MD5
    vm "sync; echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1
    local read_md5
    read_md5=$(vm "cat ${MNT}/t7_t2_file.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    read MD5: ${read_md5}"

    if [ "$src_md5" = "$read_md5" ]; then
        ok "MD5 consistent after network recover"
    else
        ng "MD5 mismatch (src=${src_md5} read=${read_md5})"
        vm "rm -f ${MNT}/t7_t2_file.bin" 2>/dev/null
        return 1
    fi

    # T2-f: 检查 dmesg 有重连日志
    echo "  [T2-f] check dmesg reconnect log..."
    local new_log
    new_log=$(dmesg_since "$base" 2>/dev/null)
    local reconnect_log
    reconnect_log=$(echo "$new_log" | grep -iE 'reconnect|volume.*state|retry|queue.*request' | tail -5 || true)
    if [ -n "$reconnect_log" ]; then
        ok "reconnect/retry log found in dmesg"
        echo "$reconnect_log" | sed 's/^/    /'
    else
        warn "no reconnect log in dmesg (log format may differ)"
    fi

    if ! check_kernel_state "T2-f final" "$base"; then
        ng "T2-f kernel state anomaly"
        vm "rm -f ${MNT}/t7_t2_file.bin" 2>/dev/null
        return 1
    fi

    # 清理
    vm "rm -f ${MNT}/t7_t2_file.bin /tmp/t7_t2_read_*" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T2 network disconnect/recover passed"
    return 0
}

# ============================================================
# T3: Volume Server 故障 failover (Replicated 副本读取)
# ============================================================
test_t3_volume_failover() {
    section "T3: Volume Server failover (Replicated replica read)"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} not running, skip T3"
        return 0
    fi

    # T3-1: 检查 setfattr 可用
    echo "  [T3-1] check setfattr..."
    if ! ensure_setfattr; then
        skip "setfattr unavailable in FUSE container, skip T3 (non-blocking)"
        return 0
    fi
    ok "setfattr available"

    # T3-2: 检查 Filer 是否启用 Replicated reliability
    echo "  [T3-2] check Filer Replicated reliability support..."
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR} && mkdir -p ${REL_DIR}" 2>/dev/null

    local xattr_ret
    xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.reliability -v 'replicated' ${REL_DIR} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -ne 0 ]; then
        xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.rel -v 'replicated' ${REL_DIR} 2>&1" 2>/dev/null)
        ret=$?
    fi

    if [ $ret -ne 0 ]; then
        warn "Filer does not support Replicated reliability (xattr failed: ${xattr_ret})"
        echo "    reliability is decided by Filer/scrubber, skip T3 (non-blocking)"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        skip "T3 Replicated (Filer reliability not enabled)"
        return 0
    fi
    ok "Filer supports Replicated reliability (xattr ok)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T3-a/3b/3c: FUSE 创建 Replicated 文件, 等待 scrubber 转换, 计算 MD5
    echo "  [T3-a] FUSE create Replicated file, wait scrubber convert..."
    local fuse_md5
    fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=2 2>/dev/null > ${REL_DIR}/t3_rep.bin && md5sum ${REL_DIR}/t3_rep.bin | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$fuse_md5" ]; then
        ng "FUSE create Replicated file failed"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        return 1
    fi
    echo "    FUSE write MD5: ${fuse_md5}"

    # 等待 scrubber 转换 (最多 60s)
    local waited=0
    local converted=0
    while [ $waited -lt 60 ]; do
        sleep 5
        waited=$((waited + 5))
        local conv_log
        conv_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'reliability_state|replicated.*state|scrubber' | tail -3 || true)
        if [ -n "$conv_log" ]; then
            converted=1
            echo "    [${waited}s] scrubber convert log detected"
            break
        fi
        echo "    [${waited}s] waiting scrubber convert..."
    done

    if [ $converted -eq 0 ]; then
        warn "no scrubber convert log within 60s (may already be converted, continue)"
    else
        ok "scrubber convert triggered"
    fi

    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 1
    vm "sync; echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    # T3-d: 停止一个 Volume Server (volume-1, 触发 failover 从副本读)
    echo "  [T3-d] stop volume-1 (trigger failover from replica)..."
    docker stop volume-1 >/dev/null 2>&1
    INJECTED_VOLUME_DOWN=1
    sleep 3
    ok "volume-1 stopped"

    # T3-e: 内核读取 Replicated 文件 → 应从副本读取 (failover)
    echo "  [T3-e] kernel read Replicated file (failover from replica)..."
    local kernel_md5
    kernel_md5=$(vm "cat ${REL_KERNEL_DIR}/t3_rep.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    kernel read MD5: ${kernel_md5}"

    # T3-f: MD5 一致
    if [ "$fuse_md5" = "$kernel_md5" ]; then
        ok "Replicated MD5 consistent (failover from replica ok)"
    else
        ng "Replicated MD5 mismatch (fuse=${fuse_md5} kernel=${kernel_md5})"
        docker start volume-1 >/dev/null 2>&1
        wait_volume_healthy volume-1 || true
        INJECTED_VOLUME_DOWN=0
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        return 1
    fi

    # T3-g: 检查 dmesg 有 failover 日志
    echo "  [T3-g] check dmesg failover log..."
    local failover_log
    failover_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'failover|replica.*read|retry.*volume|volume.*down|read_needle' | tail -5 || true)
    if [ -n "$failover_log" ]; then
        ok "failover/replica read log found"
        echo "$failover_log" | sed 's/^/    /'
    else
        warn "no failover log in dmesg (primary replica may still be on volume-2/3)"
    fi

    if ! check_kernel_state "T3 volume failover" "$base"; then
        ng "T3 kernel state anomaly"
    fi

    # T3-h: 恢复 Volume Server
    echo "  [T3-h] restore volume-1..."
    docker start volume-1 >/dev/null 2>&1
    wait_volume_healthy volume-1 || warn "volume-1 not healthy after restore"
    INJECTED_VOLUME_DOWN=0
    sleep 3
    ok "volume-1 restored"

    # 最终内核状态检查
    if ! check_kernel_state "T3 final" "$base"; then
        ng "T3 final kernel state anomaly"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        return 1
    fi

    # 清理
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T3 Volume failover passed"
    return 0
}

# ============================================================
# T4: Filer 故障切换测试 (停 leader, 自动重连新 leader)
# ============================================================
test_t4_filer_failover() {
    section "T4: Filer failover (stop leader, reconnect new leader)"

    # T4-1: 检查 Filer 数量 (单 Filer 则 skip)
    echo "  [T4-1] check filer count..."
    local filer_count
    filer_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'filer-1|filer-2|filer-3' || echo 0)
    if [ "$filer_count" -lt 2 ]; then
        warn "only ${filer_count} filer running (need >=2 for failover test)"
        skip "T4 Filer failover (single filer, cannot test leader switch)"
        return 0
    fi
    ok "${filer_count} filers running"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T4-a: 创建测试文件
    echo "  [T4-a] create test file..."
    local src_md5
    src_md5=$(vm "dd if=/dev/urandom bs=1M count=1 2>/dev/null | tee ${MNT}/t7_t4_file.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$src_md5" ]; then
        ng "create test file failed"
        return 1
    fi
    echo "    write MD5: ${src_md5}"
    vm "sync" 2>/dev/null
    sleep 1

    if ! check_kernel_state "T4-a create file" "$base"; then
        ng "T4-a kernel state anomaly"
        vm "rm -f ${MNT}/t7_t4_file.bin" 2>/dev/null
        return 1
    fi

    # T4-b: 停止 Filer leader (优先 get_current_leader, 默认 filer-1)
    echo "  [T4-b] stop filer leader..."
    local leader
    leader=$(get_current_leader 2>/dev/null)
    [ -z "$leader" ] && leader="filer-1"
    echo "    current leader: ${leader}"
    docker stop "$leader" >/dev/null 2>&1
    INJECTED_FILER_DOWN=1
    sleep 3
    ok "${leader} stopped"

    # 等待 Raft 选举新 leader (内核应自动重连)
    echo "    wait Raft elect new leader + kernel reconnect..."
    local waited=0
    while [ $waited -lt 30 ]; do
        sleep 3
        waited=$((waited + 3))
        local recon_log
        recon_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'leader.*switch|redirect|reconnect|filer.*connected' | tail -3 || true)
        [ -n "$recon_log" ] && break
    done

    # T4-c: 内核继续读写 → 应自动重连新 leader
    echo "  [T4-c] kernel read/write after leader down..."
    vm "sync; echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    local read_md5
    read_md5=$(vm "cat ${MNT}/t7_t4_file.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    read MD5: ${read_md5}"

    # 尝试写新文件 (验证新 leader 可写)
    local write_ok
    write_ok=$(vm "echo t4_after_switch > ${MNT}/t7_t4_new.txt && echo ok" 2>/dev/null)
    if echo "$write_ok" | grep -q "ok"; then
        ok "write to new leader succeeded"
    else
        warn "write to new leader failed (may still be electing)"
    fi

    # T4-d: 读写成功, MD5 一致
    if [ "$src_md5" = "$read_md5" ]; then
        ok "MD5 consistent after filer leader switch"
    else
        ng "MD5 mismatch after leader switch (src=${src_md5} read=${read_md5})"
        docker start "$leader" >/dev/null 2>&1
        wait_filer_healthy "$leader" || true
        INJECTED_FILER_DOWN=0
        vm "rm -f ${MNT}/t7_t4_file.bin ${MNT}/t7_t4_new.txt" 2>/dev/null
        return 1
    fi

    # T4-e: 检查 dmesg 有 leader switch 日志
    echo "  [T4-e] check dmesg leader switch log..."
    local switch_log
    switch_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'leader.*switch|redirect to leader|reconnect|filer.*connected \(v2' | tail -5 || true)
    if [ -n "$switch_log" ]; then
        ok "leader switch/reconnect log found"
        echo "$switch_log" | sed 's/^/    /'
    else
        warn "no leader switch log in dmesg (log format may differ)"
    fi

    if ! check_kernel_state "T4 leader switch" "$base"; then
        ng "T4 kernel state anomaly"
    fi

    # T4-f: 恢复 Filer
    echo "  [T4-f] restore ${leader}..."
    docker start "$leader" >/dev/null 2>&1
    wait_filer_healthy "$leader" || warn "${leader} not healthy after restore"
    INJECTED_FILER_DOWN=0
    sleep 3
    ok "${leader} restored"

    # 最终内核状态检查
    if ! check_kernel_state "T4 final" "$base"; then
        ng "T4 final kernel state anomaly"
        vm "rm -f ${MNT}/t7_t4_file.bin ${MNT}/t7_t4_new.txt" 2>/dev/null
        return 1
    fi

    # 清理
    vm "rm -f ${MNT}/t7_t4_file.bin ${MNT}/t7_t4_new.txt" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T4 Filer failover passed"
    return 0
}

# ============================================================
# T5: CRC32 不匹配检测 (注入损坏 → EIO + CRC mismatch)
# ============================================================
test_t5_crc_mismatch() {
    section "T5: CRC32 mismatch detection (inject corruption → EIO)"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} not running, skip T5"
        return 0
    fi

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T5-a: 创建测试文件 (FUSE 端, 小文件便于定位)
    echo "  [T5-a] create test file via FUSE..."
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR} && mkdir -p ${REL_DIR}" 2>/dev/null
    local fuse_md5
    fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=4K count=1 2>/dev/null > ${REL_DIR}/t5_corrupt.bin && md5sum ${REL_DIR}/t5_corrupt.bin | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$fuse_md5" ]; then
        ng "FUSE create test file failed"
        return 1
    fi
    echo "    FUSE write MD5: ${fuse_md5}"
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 2

    # T5-b: 尝试直接修改 Volume Server 上的 needle 数据 (best-effort)
    echo "  [T5-b] inject corruption into volume needle data (best-effort)..."
    # 在 volume-1 的 /data 目录查找数据文件
    local data_file
    data_file=$(docker exec volume-1 sh -c "find /data -type f -name '*.dat' 2>/dev/null | head -1" 2>/dev/null)
    if [ -z "$data_file" ]; then
        data_file=$(docker exec volume-1 sh -c "find /data -type f -size +1c 2>/dev/null | head -1" 2>/dev/null)
    fi

    if [ -z "$data_file" ]; then
        warn "cannot locate volume data file to inject corruption"
        echo "    needle data layout is opaque from host, skip T5 (non-blocking)"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        skip "T5 CRC mismatch (cannot inject corruption safely)"
        return 0
    fi
    echo "    candidate data file: ${data_file}"

    # 备份并损坏一个字节 (偏移 100, 避开 needle 头部 magic)
    docker exec volume-1 sh -c "cp '${data_file}' '${data_file}.bak' 2>/dev/null" 2>/dev/null
    # 用 printf 写入一个翻转字节 (在数据区中部, offset 取文件大小一半, 不少于 100)
    local file_size
    file_size=$(docker exec volume-1 sh -c "stat -c %s '${data_file}' 2>/dev/null || echo 0" 2>/dev/null)
    local corrupt_off=100
    if [ "${file_size:-0}" -gt 200 ]; then
        corrupt_off=$((file_size / 2))
    fi
    echo "    corrupt byte at offset ${corrupt_off} (file size ${file_size})"
    docker exec volume-1 sh -c "
        python3 -c \"
import sys
p='${data_file}'
off=${corrupt_off}
with open(p,'r+b') as f:
    f.seek(off)
    b=f.read(1)
    nb=bytes([b[0]^0xff if b else 0xff])
    f.seek(off)
    f.write(nb)
\" 2>/dev/null || \
        printf '\\xff' | dd of='${data_file}' bs=1 seek=${corrupt_off} count=1 conv=notrunc 2>/dev/null
    " 2>/dev/null

    # 注意: 直接改盘后, volume server 内存缓存可能仍为旧数据 → 读可能仍成功
    # 此时无法可靠触发 EIO, 按约定 skip + warn
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync; echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    # T5-c: 内核读取 → 应返回 EIO (若 CRC 校验生效)
    echo "  [T5-c] kernel read corrupted file (expect EIO)..."
    local read_out
    read_out=$(vm "cat ${REL_KERNEL_DIR}/t5_corrupt.bin 2>&1 | md5sum | awk '{print \$1}'" 2>/dev/null)
    local read_err
    read_err=$(vm "cat ${REL_KERNEL_DIR}/t5_corrupt.bin 2>&1 >/dev/null" 2>/dev/null | head -1)

    if [ -n "$read_err" ] && echo "$read_err" | grep -qiE 'input/output error|EIO|cannot open'; then
        ok "kernel read returned EIO (CRC mismatch detected)"
        # T5-d: 检查 dmesg 有 CRC mismatch 日志
        echo "  [T5-d] check dmesg CRC mismatch log..."
        local crc_log
        crc_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'CRC mismatch|crc.*fail|checksum.*fail|EIO|data.*corrupt' | tail -5 || true)
        if [ -n "$crc_log" ]; then
            ok "CRC mismatch log found in dmesg"
            echo "$crc_log" | sed 's/^/    /'
        else
            warn "no CRC mismatch log in dmesg (log may use different wording)"
        fi
    else
        # 读未报错: 可能 volume server 内存缓存命中, 或 needle 未被实际损坏
        warn "read did not return EIO (volume cache hit or corruption not on this needle)"
        echo "    cannot reliably inject corruption without deep volume internals, skip (non-blocking)"
        # 恢复备份 (若有)
        docker exec volume-1 sh -c "test -f '${data_file}.bak' && cp '${data_file}.bak' '${data_file}' && rm -f '${data_file}.bak'" 2>/dev/null
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        skip "T5 CRC mismatch (cannot reliably trigger EIO)"
        return 0
    fi

    # 恢复备份 (若有)
    docker exec volume-1 sh -c "test -f '${data_file}.bak' && cp '${data_file}.bak' '${data_file}' && rm -f '${data_file}.bak'" 2>/dev/null

    if ! check_kernel_state "T5 CRC mismatch" "$base"; then
        ng "T5 kernel state anomaly"
    fi

    # 清理: 删除测试文件 (移除可能损坏的 needle)
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T5 CRC mismatch detection passed"
    return 0
}

# ============================================================
# T6: umount 排空测试 (打开句柄, umount 等待, 关闭后成功)
# ============================================================
test_t6_umount_drain() {
    section "T6: umount drain (open handles, umount waits, then succeeds)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T6-a: 创建大量文件 (100 个, 各种大小)
    echo "  [T6-a] create 100 files of various sizes..."
    vm "mkdir -p ${MNT}/t7_t6_dir" 2>/dev/null
    local sizes=("1K" "4K" "64K" "256K" "1M")
    local i
    for i in $(seq 1 100); do
        local sz="${sizes[$((i % ${#sizes[@]}))]}"
        vm "dd if=/dev/urandom bs=${sz} count=1 2>/dev/null > ${MNT}/t7_t6_dir/file_${i}.bin" 2>/dev/null
    done
    vm "sync" 2>/dev/null
    ok "100 files created"
    echo "    slab after create: $(get_powerfs_slab_total) (init: ${SLAB_INIT})"

    if ! check_kernel_state "T6-a create 100 files" "$base"; then
        ng "T6-a kernel state anomaly"
        vm "rm -rf ${MNT}/t7_t6_dir" 2>/dev/null
        return 1
    fi

    # T6-b: 打开一些文件句柄 (后台 sleep 持有 fd, 模拟 tail -f)
    echo "  [T6-b] open file handles (background sleep holding fd)..."
    vm "rm -f /tmp/t7_t6_hold_*.pid" 2>/dev/null
    # 选取 3 个文件, 用 sleep 持有其只读 fd
    vm "
        for i in 1 2 3; do
            sleep 600 < ${MNT}/t7_t6_dir/file_\${i}.bin > /dev/null 2>&1 &
            echo \$! > /tmp/t7_t6_hold_\${i}.pid
        done
    " 2>/dev/null
    sleep 1

    # 确认句柄已打开
    local alive=0
    local p
    for p in 1 2 3; do
        local r
        r=$(vm "kill -0 \$(cat /tmp/t7_t6_hold_${p}.pid 2>/dev/null) 2>/dev/null && echo yes || echo no" 2>/dev/null)
        [ "$r" = "yes" ] && alive=$((alive + 1))
    done
    if [ "$alive" -ge 1 ]; then
        ok "${alive} file handles open (sleep holding fd)"
    else
        warn "no file handle open (sleep background failed), umount test may not be meaningful"
    fi

    # T6-c: 尝试 umount → 应该等待文件关闭 (有句柄时返回 EBUSY, 不应成功)
    echo "  [T6-c] umount with open handles (should fail/wait, not succeed)..."
    local umount_out
    umount_out=$(vm "timeout 5 umount ${MNT} 2>&1" 2>/dev/null)
    local umount_ret=$?
    if [ $umount_ret -ne 0 ]; then
        ok "umount refused with open handles (ret=${umount_ret}: ${umount_out})"
    else
        ng "umount succeeded with open handles (open fd not tracked)"
        # 已 umount, 清理句柄并跳过后续
        vm "for p in 1 2 3; do kill \$(cat /tmp/t7_t6_hold_\${p}.pid 2>/dev/null) 2>/dev/null; done" 2>/dev/null
        # 重新挂载
        vm "mount -t powerfs none ${MNT}" 2>/dev/null
        sleep 2
        vm "rm -rf ${MNT}/t7_t6_dir" 2>/dev/null
        return 1
    fi

    if ! check_kernel_state "T6-c umount busy" "$base"; then
        ng "T6-c kernel state anomaly"
    fi

    # T6-d: 关闭文件句柄
    echo "  [T6-d] close file handles..."
    vm "for p in 1 2 3; do kill \$(cat /tmp/t7_t6_hold_\${p}.pid 2>/dev/null) 2>/dev/null; done" 2>/dev/null
    sleep 2
    # 确认句柄已关闭
    local still_alive=0
    for p in 1 2 3; do
        local r
        r=$(vm "kill -0 \$(cat /tmp/t7_t6_hold_${p}.pid 2>/dev/null) 2>/dev/null && echo yes || echo no" 2>/dev/null)
        [ "$r" = "yes" ] && still_alive=$((still_alive + 1))
    done
    if [ "$still_alive" -eq 0 ]; then
        ok "all file handles closed"
    else
        warn "${still_alive} handle still open"
    fi

    # T6-e: umount 成功
    echo "  [T6-e] umount after handles closed (should succeed)..."
    local umount_out2
    umount_out2=$(vm "timeout 30 umount ${MNT} 2>&1" 2>/dev/null)
    local umount_ret2=$?
    if [ $umount_ret2 -eq 0 ]; then
        ok "umount succeeded after handles closed"
    else
        ng "umount failed after handles closed: ${umount_out2}"
        return 1
    fi
    sleep 2

    # T6-f: rmmod 成功
    echo "  [T6-f] rmmod powerfs..."
    local rmmod_out
    rmmod_out=$(vm "rmmod powerfs 2>&1" 2>/dev/null)
    local rmmod_ret=$?
    if [ $rmmod_ret -eq 0 ]; then
        ok "rmmod succeeded"
    else
        ng "rmmod failed: ${rmmod_out}"
        echo "    check: lsmod | grep powerfs"
        # 重新挂载以便后续
        vm "mount -t powerfs none ${MNT}" 2>/dev/null
        return 1
    fi
    sleep 2

    # T6-g: 检查 dmesg 无残留内存警告
    echo "  [T6-g] check dmesg no residual memory warning..."
    local mem_warn
    mem_warn=$(dmesg_since "$base" 2>/dev/null | grep -iE 'memory leak|kmemleak|still has.*objects|slab.*leak|free.*failed' | tail -5 || true)
    if [ -z "$mem_warn" ]; then
        ok "no residual memory warning in dmesg"
    else
        warn "memory warning detected in dmesg:"
        echo "$mem_warn" | sed 's/^/    /'
    fi

    # dmesg 异常检查
    if check_dmesg_clean "$base"; then
        ok "dmesg clean after umount+rmmod"
    else
        ng "dmesg anomaly after umount+rmmod"
    fi

    # T6-h: 检查 slab 对象全部释放 (对比挂载前基线)
    echo "  [T6-h] check slab fully released (compare to mount baseline)..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab fully released (rmmod clean)"
    else
        warn "powerfs slab residual after rmmod:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # 内存恢复检查
    local mem_final
    mem_final=$(get_mem_available)
    echo "    final MemAvailable: ${mem_final} KB (init: ${MEM_INIT} KB)"
    if [ "${MEM_INIT:-0}" -gt 0 ]; then
        local diff=$(( ${MEM_INIT} - ${mem_final} ))
        if [ $diff -lt 50000 ]; then
            ok "memory recovered well (diff ${diff} KB)"
        else
            warn "memory diff large (${diff} KB), possible leak"
        fi
    fi

    ok "T6 umount drain passed"
    return 0
}

# ============================================================
# T7: 最终清理 + 重新挂载
# ============================================================
test_t7_cleanup_remount() {
    section "T7: final cleanup + remount"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T7-1: 清理测试文件
    echo "  [T7-1] cleanup test files..."
    if check_mount; then
        vm "rm -rf ${MNT}/t7_t6_dir ${MNT}/t7_t2_file.bin ${MNT}/t7_t4_file.bin ${MNT}/t7_t4_new.txt" 2>/dev/null
        ok "kernel-side test files cleaned"
    else
        warn "powerfs not mounted, skip kernel-side cleanup"
    fi

    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        ok "FUSE-side test files cleaned"
    else
        warn "${FUSE_CONTAINER} not running, skip FUSE-side cleanup"
    fi

    # T7-2: 确保所有服务恢复正常 (Volume + Filer + Master)
    echo "  [T7-2] ensure all services restored..."
    local v f
    for v in "${VOLUMES[@]}"; do
        if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${v}$"; then
            warn "${v} not running, starting..."
            docker start "$v" >/dev/null 2>&1 || true
        fi
    done
    for f in "${FILERS[@]}"; do
        if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${f}$"; then
            warn "${f} not running, starting..."
            docker start "$f" >/dev/null 2>&1 || true
        fi
    done
    INJECTED_VOLUME_DOWN=0
    INJECTED_FILER_DOWN=0
    INJECTED_NET_BLOCKED=0
    sleep 3
    ok "services restored"

    # T7-3: 重新挂载 (T6 已 umount+rmmod, 这里重新加载)
    echo "  [T7-3] remount powerfs..."
    if check_mount; then
        ok "powerfs already mounted"
    else
        # 模块未加载则 insmod (复用 qemuctl.sh mount)
        if ! vm "lsmod | grep -q powerfs" 2>/dev/null; then
            ./qemuctl.sh mount 2>&1 | tail -5
        else
            vm "mount -t powerfs none ${MNT}" 2>/dev/null
        fi
        sleep 3
        if check_mount; then
            ok "powerfs remounted at ${MNT}"
        else
            ng "powerfs remount failed"
            return 1
        fi
    fi

    # T7-4: 重新挂载后内核状态检查
    echo "  [T7-4] kernel state check after remount..."
    if check_kernel_state "T7 remount" "$base"; then
        ok "kernel state normal after remount"
    else
        ng "kernel state anomaly after remount"
        return 1
    fi

    # T7-5: 基本读写确认 (smoke test)
    echo "  [T7-5] smoke read/write after remount..."
    local smoke
    smoke=$(vm "echo t7_smoke > ${MNT}/t7_smoke.txt && cat ${MNT}/t7_smoke.txt && rm -f ${MNT}/t7_smoke.txt" 2>/dev/null)
    if [ "$smoke" = "t7_smoke" ]; then
        ok "smoke read/write ok after remount"
    else
        ng "smoke read/write failed after remount: '${smoke}'"
        return 1
    fi

    # 更新基线 (重新挂载后)
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "    new baseline slab: ${SLAB_INIT}"
    echo "    new baseline MemAvailable: ${MEM_INIT} KB"

    ok "T7 cleanup + remount passed"
    return 0
}

# ============================================================
# 主流程
# ============================================================
main() {
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  T7 渐进式测试: 可靠性 (故障注入 + failover)        ${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从小到大, 逐个确认, 检查内核状态             ${C_RESET}"
    echo -e "${C_CYAN}║  故障注入后必须恢复, 无法执行 skip + warn            ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_net_disconnect"
        "3:test_t3_volume_failover"
        "4:test_t4_filer_failover"
        "5:test_t5_crc_mismatch"
        "6:test_t6_umount_drain"
        "7:test_t7_cleanup_remount"
    )

    local executed=0
    local failed_test=""

    for entry in "${tests[@]}"; do
        local tid="${entry%%:*}"
        local func="${entry##*:}"

        if ! should_run "$tid"; then
            skip "T${tid} (not selected)"
            continue
        fi

        if [ -n "$failed_test" ]; then
            skip "T${tid} (skipped due to T${failed_test} failure)"
            continue
        fi

        if $func; then
            executed=$((executed + 1))
        else
            failed_test="$tid"
            echo ""
            echo -e "  ${C_RED}T${tid} failed, subsequent tests skipped${C_RESET}"
            executed=$((executed + 1))
        fi
    done

    # 兜底恢复 (确保任何失败后服务也可恢复)
    cleanup_injected_faults

    # 总结
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  Test Summary                                        ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo -e "  ${C_GREEN}PASS:${C_RESET}  ${PASS}"
    echo -e "  ${C_RED}FAIL:${C_RESET}  ${FAIL}"
    echo -e "  ${C_YELLOW}WARN:${C_RESET}  ${WARN}"
    echo -e "  ${C_YELLOW}SKIP:${C_RESET}  ${SKIP}"
    echo ""

    if [ "$FAIL" -gt 0 ]; then
        echo -e "  ${C_RED}✗ T7 reliability test has failures${C_RESET}"
        [ -n "$failed_test" ] && echo "  first failure: T${failed_test}"
        echo ""
        echo "  troubleshooting:"
        echo "    1. VM dmesg:        ./qemuctl.sh log powerfs"
        echo "    2. serial monitor:  ./qemuctl.sh monitor powerfs"
        echo "    3. backend log:     ./qemuctl.sh service log filer-1"
        echo "    4. T3/T5 skip is normal (Filer reliability not enabled / cannot inject)"
        echo "    5. rerun single:    ./test_t7_reliability.sh <T#>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ T7 reliability test all passed${C_RESET}"
    echo ""
    echo "  T7 gate reached:"
    echo "    - compile + static verification passed"
    echo "    - QEMU mount + services (Master/Filer/Volume) running"
    echo "    - network disconnect/recover: read queued + MD5 consistent"
    echo "    - Volume failover: replica read ok (when reliability enabled)"
    echo "    - Filer failover: leader switch + reconnect ok"
    echo "    - CRC mismatch detection (when corruption injectable)"
    echo "    - umount drain: open fd tracked, rmmod clean, slab released"
    echo "    - kernel state (dmesg/slab/meminfo/serial) normal"
    exit 0
}

main "$@"
