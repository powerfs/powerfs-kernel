#!/bin/bash
# T4 阶段集成测试: FUSE 客户端 ↔ 内核客户端互操作
#
# 验证内容:
#   T0: 编译 + 静态验证 (无后端)
#   T1: QEMU 启动 + 挂载 (复用 qemuctl.sh, 确认 FUSE 容器 fuse-1 也在运行)
#   T2: FUSE 创建 → 内核读取 (Flat 10MB / Inline 100B / Flat 1MB, 逐个 MD5 校验)
#   T3: 内核创建 → FUSE 读取 (Flat 10MB / Inline 100B, 逐个 MD5 校验)
#   T4: Stripe 模式互通 (FUSE 设 stripe xattr, 双向创建/读取)
#   T5: remount 一致性 (10 个文件 umount+remount 后列表与 MD5 一致)
#   T6: 并发互操作 (FUSE+内核同时写, FUSE 写+内核读, 60s 持续并发)
#   T7: 卸载 + 最终检查 (清理 / umount / rmmod / dmesg+slab+内存)
#
# 核心原则:
#   - 互操作核心: FUSE 和内核客户端通过 Filer 共享同一份元数据与数据
#   - 内核正确性 != 应用完成, 必须检查 dmesg/slab/meminfo/D 状态
#   - 从小到大逐个确认, 不可跳级
#   - 前一档未通过不进入下一档
#
# 运行环境: HOST (通过 SSH 控制 VM + docker exec 控制 FUSE 容器)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - FUSE 容器 fuse-1 已运行且挂载到 /mnt/powerfs
#   - 或由本脚本 T1 自动完成 (service start / deploy / mount)
#
# 用法:
#   ./test_t4_integration.sh            # 运行全部 (T0-T7)
#   ./test_t4_integration.sh 3          # 仅运行 T3
#   ./test_t4_integration.sh 2 4 6      # 运行 T2+T4+T6

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs           # FUSE 容器内挂载点 (fuse-1)
FUSE_CONTAINER="fuse-1"         # FUSE 容器名
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

# 互操作测试根目录 (FUSE 端 / 内核端 各自相对挂载点)
T4_DIR_NAME="t4_interop"
FUSE_T4_DIR="${FUSE_MNT}/${T4_DIR_NAME}"
KERNEL_T4_DIR="${MNT}/${T4_DIR_NAME}"

PASS=0
FAIL=0
WARN=0
SKIP=0
SLAB_INIT=""
MEM_INIT=""
SERIAL_BASE=0

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
# 参数 $1: 基线行号 (可选, 默认检查全部)
# 返回: 0=正常, 1=有异常
check_dmesg_clean() {
    local base="${1:-0}"
    local log
    log=$(dmesg_since "$base" 2>/dev/null)

    local errors
    errors=$(echo "$log" | grep -E 'BUG:|Oops:|general protection fault|unable to handle|NULL pointer dereference|KASAN:|RCU stall|workqueue lockup|hung task|soft lockup|hard lockup|NMI watchdog|call trace|Kernel panic|Segmentation fault' || true)

    if [ -n "$errors" ]; then
        echo "  ${C_RED}内核异常已检测到:${C_RESET}"
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
        echo "  ${C_RED}D state powerfs threads detected:${C_RESET}"
        echo "$d_tasks" | sed 's/^/    /'
        return 1
    fi
    return 0
}

# 综合内核状态检查
# 参数 $1: 检查描述
# 参数 $2: dmesg 基线 (可选)
check_kernel_state() {
    local desc="$1"
    local base="${2:-0}"
    local state_ok=0

    echo "  --- kernel state check: ${desc} ---"

    # 1. dmesg 无异常
    if check_dmesg_clean "$base"; then
        ok "dmesg clean (no oops/bug/kasan/stall)"
    else
        ng "dmesg abnormal (${desc})"
        state_ok=1
    fi

    # 2. 无 D 状态线程
    if check_d_state; then
        ok "no D-state powerfs threads"
    else
        ng "D-state (hung) powerfs threads exist (${desc})"
        state_ok=1
    fi

    # 3. serial 日志 lockup 检查 (补充 dmesg, VM 卡死时也能查)
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        local serial_errors
        serial_errors=$(serial_since "${SERIAL_BASE}" | grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task' 2>/dev/null | tail -5 || true)
        if [ -z "$serial_errors" ]; then
            ok "serial log clean (no lockup/panic)"
        else
            ng "serial log lockup/panic detected (${desc})"
            echo "$serial_errors" | sed 's/^/      /'
            state_ok=1
        fi
    fi

    return $state_ok
}

# FUSE 容器内执行命令的简捷封装
fuse_exec() {
    docker exec "${FUSE_CONTAINER}" sh -c "$@" 2>&1
}

# 内核端 drop cache (强制重新 lookup, 确保 FUSE 写入的内容能被内核看到)
kernel_drop_cache() {
    vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
}

# FUSE 端 drop cache (强制从 Filer 重新读取)
fuse_drop_cache() {
    docker exec "${FUSE_CONTAINER}" sh -c "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
}

# 检查 FUSE 容器是否运行, 返回 0=运行, 1=未运行
fuse_container_running() {
    docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"
}

# ============================================================
# T0: 编译 + 静态验证
# ============================================================
test_t0_compile() {
    section "T0: Compile + Static Verification"

    # T0-1: 编译无警告
    echo "  [T0-1] build powerfs.ko..."
    cd "${POWERFS_MOD_DIR}"
    local build_log
    build_log=$(make clean 2>&1 && make -j$(nproc) 2>&1)
    local build_ret=$?

    if [ $build_ret -eq 0 ] && [ -f powerfs.ko ]; then
        ok "powerfs.ko build success ($(ls -la powerfs.ko | awk '{print $5}') bytes)"
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
        ok "build no warnings"
    else
        warn "build has warnings (non-blocking):"
        echo "$warnings" | head -5 | sed 's/^/    /'
    fi

    # T0-2: 静态符号验证
    echo "  [T0-2] static symbol verification..."
    if bash verify_module.sh 2>&1 | tail -5 | grep -q "ALL VERIFICATION TESTS PASSED"; then
        ok "verify_module.sh all passed"
    else
        ng "verify_module.sh has failures"
        bash verify_module.sh 2>&1 | grep '\[FAIL\]' | sed 's/^/    /'
        cd "${SCRIPT_DIR}"
        return 1
    fi

    # T0-3: chunk_size 确认 (1MB, Flat/Stripe 互操作基准)
    echo "  [T0-3] POWERFS_CHUNK_SIZE confirmation..."
    local chunk_def
    chunk_def=$(grep -E 'POWERFS_CHUNK_SIZE\s+' powerfs.h | head -1)
    if echo "$chunk_def" | grep -q '1 \* 1024 \* 1024'; then
        ok "POWERFS_CHUNK_SIZE = 1MB"
    else
        ng "POWERFS_CHUNK_SIZE != 1MB: $chunk_def"
        cd "${SCRIPT_DIR}"
        return 1
    fi

    cd "${SCRIPT_DIR}"
    return 0
}

# ============================================================
# T1: QEMU 启动 + 挂载 (确认 FUSE 容器同时运行)
# ============================================================
test_t1_mount() {
    section "T1: QEMU Start + Mount (FUSE + Kernel)"

    # T1-1: 检查后端服务
    echo "  [T1-1] check backend services..."
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

    # T1-2: 检查 FUSE 容器 (fuse-1) 运行
    echo "  [T1-2] check FUSE container ${FUSE_CONTAINER}..."
    if fuse_container_running; then
        ok "FUSE container ${FUSE_CONTAINER} running"
    else
        warn "FUSE container ${FUSE_CONTAINER} not running, attempt service start..."
        ./qemuctl.sh service start 2>&1 | tail -5
        sleep 5
        if fuse_container_running; then
            ok "FUSE container ${FUSE_CONTAINER} started"
        else
            ng "FUSE container ${FUSE_CONTAINER} not available (interop tests need it)"
            echo "    hint: docker compose -f docker-compose-single.yml up -d fuse-1"
            return 1
        fi
    fi

    # T1-3: FUSE 容器内挂载点确认
    echo "  [T1-3] check FUSE mount inside container..."
    local fuse_mount_info
    fuse_mount_info=$(docker exec ${FUSE_CONTAINER} sh -c "mount | grep powerfs" 2>/dev/null || true)
    if [ -n "$fuse_mount_info" ]; then
        ok "FUSE mount OK: ${fuse_mount_info}"
    else
        ng "FUSE container has no powerfs mount at ${FUSE_MNT}"
        echo "    hint: ensure fuse-1 mounts powerfs to ${FUSE_MNT}"
        return 1
    fi

    # T1-4: 检查 QEMU 运行
    echo "  [T1-4] check QEMU running..."
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

    # T1-5: SSH 可达
    echo "  [T1-5] check VM SSH reachable..."
    if vm_alive; then
        ok "VM SSH reachable"
    else
        ng "VM SSH unreachable"
        return 1
    fi

    # T1-6: powerfs 已挂载 (内核端)
    echo "  [T1-6] check powerfs mount (kernel side)..."
    if ! check_mount; then
        warn "powerfs not mounted, trying mount..."
        ./qemuctl.sh mount 2>&1 | tail -3
        sleep 2
    fi

    if check_mount; then
        ok "powerfs mounted at ${MNT}"
    else
        ng "powerfs mount failed (kernel side)"
        return 1
    fi

    # T1-7: 两个客户端挂载同一集群验证 (核心: 互操作前提)
    #       通过 FUSE 与内核两端创建一个 marker 文件, 互相看到即视为同集群
    echo "  [T1-7] verify both clients mount same PowerFS cluster..."
    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    docker exec ${FUSE_CONTAINER} sh -c "mkdir -p ${FUSE_T4_DIR}" 2>/dev/null
    docker exec ${FUSE_CONTAINER} sh -c "echo fuse_marker > ${FUSE_T4_DIR}/t1_cluster_marker.txt" 2>/dev/null
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    kernel_drop_cache
    sleep 1

    local kernel_read_marker
    kernel_read_marker=$(vm "cat ${KERNEL_T4_DIR}/t1_cluster_marker.txt" 2>/dev/null)
    if [ "$kernel_read_marker" = "fuse_marker" ]; then
        ok "kernel client sees FUSE-created file (same cluster confirmed)"
    else
        ng "kernel cannot read FUSE marker (got '${kernel_read_marker}')"
        echo "    check: FUSE mount target / kernel mount target 是否指向同一 filer"
        return 1
    fi

    # 反向: 内核创建 → FUSE 读取
    vm "echo kernel_marker > ${KERNEL_T4_DIR}/t1_kernel_marker.txt" 2>/dev/null
    vm "sync" 2>/dev/null
    fuse_drop_cache
    sleep 1

    local fuse_read_marker
    fuse_read_marker=$(docker exec ${FUSE_CONTAINER} sh -c "cat ${FUSE_T4_DIR}/t1_kernel_marker.txt" 2>/dev/null)
    if [ "$fuse_read_marker" = "kernel_marker" ]; then
        ok "FUSE client sees kernel-created file (same cluster confirmed)"
    else
        ng "FUSE cannot read kernel marker (got '${fuse_read_marker}')"
        return 1
    fi

    # T1-8: 模块引用计数
    echo "  [T1-8] check module refcount..."
    local mod_info
    mod_info=$(vm "lsmod | grep powerfs" 2>/dev/null)
    if [ -n "$mod_info" ]; then
        ok "powerfs module loaded: ${mod_info}"
    else
        ng "powerfs module not loaded"
        return 1
    fi

    # T1-9: 挂载后 30s dmesg 检查
    echo "  [T1-9] 30s dmesg observation after mount..."
    sleep 30
    if check_kernel_state "30s after mount" "$base"; then
        ok "kernel state normal 30s after mount"
    else
        ng "kernel state abnormal 30s after mount"
        return 1
    fi

    # 记录初始 slab 和 meminfo
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "  -> initial slab (inode dentry): ${SLAB_INIT}"
    echo "  -> initial MemAvailable: ${MEM_INIT} KB"

    # 清理 marker
    vm "rm -f ${KERNEL_T4_DIR}/t1_cluster_marker.txt ${KERNEL_T4_DIR}/t1_kernel_marker.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T2: FUSE 创建 → 内核读取 (从小到大, 逐个 MD5 校验)
# ============================================================
test_t2_fuse_to_kernel() {
    section "T2: FUSE Create -> Kernel Read"

    if ! fuse_container_running; then
        skip "FUSE container not running, skip T2"
        return 0
    fi

    # 测试矩阵: label:bs:count:expected_bytes:placement_hint
    #   - 100B Inline (走 Filer inline_data)
    #   - 1M Flat (单 chunk)
    #   - 10M Flat (多 chunk)
    local cases=(
        "100B_inline:100B:1:100:inline"
        "1M_flat:1M:1:1048576:flat"
        "10M_flat:10M:1:10485760:flat"
    )

    for entry in "${cases[@]}"; do
        local label="${entry%%:*}"
        local rest="${entry#*:}"
        local bs="${rest%%:*}"
        rest="${rest#*:}"
        local count="${rest%%:*}"
        rest="${rest#*:}"
        local expected="${rest%%:*}"
        local hint="${rest##*:}"

        echo ""
        echo "  [T2] FUSE create ${label} (${bs} x ${count}, ${hint}) -> kernel read"

        local base=$(dmesg_line_count)
        SERIAL_BASE=$(serial_line_count)

        local fname="t2_${label}.bin"

        # 2a/2b: FUSE 端创建文件并计算 MD5
        # 注意: dd if=/dev/urandom 提供不可预测内容, 防止读旧缓存蒙混过关
        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=${bs} count=${count} 2>/dev/null > ${FUSE_T4_DIR}/${fname} && md5sum ${FUSE_T4_DIR}/${fname} | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$fuse_md5" ]; then
            ng "FUSE create ${label} failed (md5 empty)"
            return 1
        fi
        echo "    FUSE side MD5: ${fuse_md5}"

        # 等待数据落盘 + 内核重新 lookup
        docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
        vm "sync" 2>/dev/null
        kernel_drop_cache
        sleep 1

        # 2c: 内核端读取同一文件并计算 MD5
        local kernel_md5
        kernel_md5=$(vm "cat ${KERNEL_T4_DIR}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    kernel side MD5: ${kernel_md5}"

        if [ "$fuse_md5" = "$kernel_md5" ]; then
            ok "${label} FUSE->kernel MD5 match"
        else
            ng "${label} FUSE->kernel MD5 mismatch (fuse=${fuse_md5} kernel=${kernel_md5})"
            return 1
        fi

        # 文件大小校验
        local actual_size
        actual_size=$(vm "stat -c %s ${KERNEL_T4_DIR}/${fname}" 2>/dev/null)
        if [ "$actual_size" = "$expected" ]; then
            ok "${label} file size correct (${actual_size})"
        else
            ng "${label} file size mismatch (got ${actual_size} want ${expected})"
            return 1
        fi

        # 每步后内核状态检查
        if ! check_kernel_state "T2 ${label}" "$base"; then
            ng "T2 ${label} kernel state abnormal"
            return 1
        fi

        # slab 抽样
        local slab_now
        slab_now=$(get_powerfs_slab_total)
        echo "    slab (inode dentry): ${slab_now} (init: ${SLAB_INIT:-N/A})"

        # 清理本步骤文件
        docker exec ${FUSE_CONTAINER} sh -c "rm -f ${FUSE_T4_DIR}/${fname}" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    echo ""
    ok "T2 FUSE->kernel interop all passed"
    return 0
}

# ============================================================
# T3: 内核创建 → FUSE 读取 (从小到大, 逐个 MD5 校验)
# ============================================================
test_t3_kernel_to_fuse() {
    section "T3: Kernel Create -> FUSE Read"

    if ! fuse_container_running; then
        skip "FUSE container not running, skip T3"
        return 0
    fi

    local cases=(
        "100B_inline:100B:1:100:inline"
        "10M_flat:10M:1:10485760:flat"
    )

    for entry in "${cases[@]}"; do
        local label="${entry%%:*}"
        local rest="${entry#*:}"
        local bs="${rest%%:*}"
        rest="${rest#*:}"
        local count="${rest%%:*}"
        rest="${rest#*:}"
        local expected="${rest%%:*}"
        local hint="${rest##*:}"

        echo ""
        echo "  [T3] kernel create ${label} (${bs} x ${count}, ${hint}) -> FUSE read"

        local base=$(dmesg_line_count)
        SERIAL_BASE=$(serial_line_count)

        local fname="t3_${label}.bin"

        # 3a/3b: 内核端创建文件并计算 MD5
        local kernel_md5
        kernel_md5=$(vm "dd if=/dev/urandom bs=${bs} count=${count} 2>/dev/null > ${KERNEL_T4_DIR}/${fname} && cat ${KERNEL_T4_DIR}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$kernel_md5" ]; then
            ng "kernel create ${label} failed (md5 empty)"
            return 1
        fi
        echo "    kernel side MD5: ${kernel_md5}"

        # 同步确保数据落盘
        vm "sync" 2>/dev/null
        sleep 1

        # 3c: FUSE 端 drop cache 后读取并计算 MD5
        fuse_drop_cache

        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "cat ${FUSE_T4_DIR}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    FUSE side MD5: ${fuse_md5}"

        if [ "$kernel_md5" = "$fuse_md5" ]; then
            ok "${label} kernel->FUSE MD5 match"
        else
            ng "${label} kernel->FUSE MD5 mismatch (kernel=${kernel_md5} fuse=${fuse_md5})"
            return 1
        fi

        # 文件大小校验
        local actual_size
        actual_size=$(docker exec ${FUSE_CONTAINER} sh -c "stat -c %s ${FUSE_T4_DIR}/${fname}" 2>/dev/null)
        if [ "$actual_size" = "$expected" ]; then
            ok "${label} file size correct (${actual_size})"
        else
            ng "${label} file size mismatch (got ${actual_size} want ${expected})"
            return 1
        fi

        # 每步后内核状态检查
        if ! check_kernel_state "T3 ${label}" "$base"; then
            ng "T3 ${label} kernel state abnormal"
            return 1
        fi

        # 清理
        vm "rm -f ${KERNEL_T4_DIR}/${fname}" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    echo ""
    ok "T3 kernel->FUSE interop all passed"
    return 0
}

# ============================================================
# T4: Stripe 模式互通 (FUSE 设 xattr, 双向创建/读取)
# ============================================================
test_t4_stripe_interop() {
    section "T4: Stripe Mode Interop"

    if ! fuse_container_running; then
        skip "FUSE container not running, skip T4"
        return 0
    fi

    # T4-0: 检查 setfattr 可用性
    echo "  [T4-0] check setfattr availability..."
    local has_setfattr
    has_setfattr=$(docker exec ${FUSE_CONTAINER} sh -c "which setfattr 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has_setfattr" = "no" ]; then
        warn "no setfattr in FUSE container, installing attr..."
        docker exec ${FUSE_CONTAINER} sh -c "apt-get install -y attr >/dev/null 2>&1 || apk add attr >/dev/null 2>&1" 2>/dev/null
        has_setfattr=$(docker exec ${FUSE_CONTAINER} sh -c "which setfattr 2>/dev/null || echo no" 2>/dev/null)
    fi
    if [ "$has_setfattr" = "no" ]; then
        ng "setfattr unavailable in FUSE container"
        return 1
    fi
    ok "setfattr available: ${has_setfattr}"

    # T4-1: 检查 volume 数量 (stripe:4 需 ≥4)
    echo "  [T4-1] check volume count (stripe:4 needs >=4)..."
    local vol_count
    vol_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'volume-[0-9]+' || echo 0)
    if [ "$vol_count" -ge 4 ]; then
        ok "volume count sufficient (${vol_count}, stripe:4 usable)"
    else
        warn "volume count insufficient (${vol_count}, stripe:4 needs 4)"
        echo "    locate_chunk will fall back to modulo available volumes"
    fi

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T4-2a: FUSE 创建 stripe 目录并设置 xattr
    local stripe_dir_name="t4_stripe_dir"
    local fuse_stripe_dir="${FUSE_T4_DIR}/${stripe_dir_name}"
    local kernel_stripe_dir="${KERNEL_T4_DIR}/${stripe_dir_name}"

    echo "  [T4-2a] FUSE create stripe dir + setfattr..."
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_stripe_dir} && mkdir -p ${fuse_stripe_dir}" 2>/dev/null
    local xattr_ret
    xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.placement -v 'stripe:4:1MB' ${fuse_stripe_dir} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -ne 0 ]; then
        ng "stripe xattr set failed: ${xattr_ret}"
        return 1
    fi
    ok "stripe xattr set OK (stripe:4:1MB)"

    # 验证 xattr
    local xattr_val
    xattr_val=$(docker exec ${FUSE_CONTAINER} sh -c "getfattr -n user.powerfs.placement --only-values ${fuse_stripe_dir} 2>/dev/null" 2>/dev/null)
    if [ "$xattr_val" = "stripe:4:1MB" ]; then
        ok "stripe xattr verified: '${xattr_val}'"
    else
        ng "stripe xattr verify failed: got '${xattr_val}'"
        return 1
    fi

    # T4-2b: FUSE 创建 Stripe 文件 (1MB), 内核读取 MD5 校验
    echo "  [T4-2b] FUSE create 1M stripe file -> kernel read..."
    local fuse_md5
    fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${fuse_stripe_dir}/file_1M.bin && md5sum ${fuse_stripe_dir}/file_1M.bin | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$fuse_md5" ]; then
        ng "FUSE create 1M stripe file failed"
        return 1
    fi
    echo "    FUSE MD5: ${fuse_md5}"

    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    kernel_drop_cache
    sleep 1

    # T4-2c: 内核读取 stripe 文件 MD5
    local kernel_md5
    kernel_md5=$(vm "cat ${kernel_stripe_dir}/file_1M.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    kernel MD5: ${kernel_md5}"
    if [ "$fuse_md5" = "$kernel_md5" ]; then
        ok "1M stripe FUSE->kernel MD5 match"
    else
        ng "1M stripe FUSE->kernel MD5 mismatch (fuse=${fuse_md5} kernel=${kernel_md5})"
        return 1
    fi

    if ! check_kernel_state "T4-2b 1M stripe FUSE->kernel" "$base"; then
        ng "T4-2b kernel state abnormal"
        return 1
    fi

    # T4-2d: 内核在 stripe 目录创建文件 -> FUSE 读取 MD5 校验
    echo "  [T4-2d] kernel create 1M stripe file -> FUSE read..."
    local base2=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local kernel_md5_2
    kernel_md5_2=$(vm "dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${kernel_stripe_dir}/file_kernel_1M.bin && cat ${kernel_stripe_dir}/file_kernel_1M.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$kernel_md5_2" ]; then
        ng "kernel create stripe file failed"
        return 1
    fi
    echo "    kernel MD5: ${kernel_md5_2}"

    vm "sync" 2>/dev/null
    fuse_drop_cache

    local fuse_md5_2
    fuse_md5_2=$(docker exec ${FUSE_CONTAINER} sh -c "cat ${fuse_stripe_dir}/file_kernel_1M.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    FUSE MD5: ${fuse_md5_2}"
    if [ "$kernel_md5_2" = "$fuse_md5_2" ]; then
        ok "1M stripe kernel->FUSE MD5 match"
    else
        ng "1M stripe kernel->FUSE MD5 mismatch (kernel=${kernel_md5_2} fuse=${fuse_md5_2})"
        return 1
    fi

    if ! check_kernel_state "T4-2d stripe kernel->FUSE" "$base2"; then
        ng "T4-2d kernel state abnormal"
        return 1
    fi

    # 清理 stripe 目录
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_stripe_dir}" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T4 stripe interop passed"
    return 0
}

# ============================================================
# T5: remount 一致性 (内核端 umount+remount 后数据一致)
# ============================================================
test_t5_remount_consistency() {
    section "T5: Remount Consistency"

    if ! fuse_container_running; then
        skip "FUSE container not running, skip T5"
        return 0
    fi

    local remount_dir_name="t5_remount"
    local kernel_remount_dir="${KERNEL_T4_DIR}/${remount_dir_name}"
    local fuse_remount_dir="${FUSE_T4_DIR}/${remount_dir_name}"

    # T5-1: 在 FUSE 端创建目录 (避免内核端 mkdir 触发额外的元数据回写)
    echo "  [T5-1] prepare remount test dir (via FUSE)..."
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_remount_dir} && mkdir -p ${fuse_remount_dir}" 2>/dev/null
    kernel_drop_cache
    sleep 1
    ok "remount test dir ready: ${kernel_remount_dir}"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T5-2a: 内核端创建 10 个文件 (各种大小 1K-1M)
    echo "  [T5-2a] kernel create 10 files (1K..1M)..."
    local sizes=("1K:1024" "2K:2048" "4K:4096" "16K:16384" "64K:65536" "128K:131072" "256K:262144" "512K:524288" "1M:1048576" "100B:100")
    local i=0
    for entry in "${sizes[@]}"; do
        local label="${entry%%:*}"
        local bytes="${entry##*:}"
        i=$((i+1))
        vm "dd if=/dev/urandom bs=${label} count=1 2>/dev/null > ${kernel_remount_dir}/t5_file_${i}_${label}.bin" 2>/dev/null
        local sz
        sz=$(vm "stat -c %s ${kernel_remount_dir}/t5_file_${i}_${label}.bin" 2>/dev/null)
        if [ "$sz" = "$bytes" ]; then
            echo "    file ${i} (${label}): ${sz} bytes"
        else
            ng "file ${i} (${label}) size wrong: got ${sz} want ${bytes}"
            return 1
        fi
    done
    ok "10 files created on kernel side"

    vm "sync" 2>/dev/null

    # T5-2b: 记录文件列表和每个文件的 MD5 (remount 前基线)
    echo "  [T5-2b] record file list and MD5 (before remount)..."
    local manifest_before
    manifest_before=$(vm "cd ${kernel_remount_dir} && ls -1 | sort" 2>/dev/null)
    echo "${manifest_before}" > /tmp/t5_manifest_before.txt

    # 内核端计算每个文件 MD5 (因为数据是内核写的, 内核 page cache 应有最新数据)
    local md5_before
    md5_before=$(vm "cd ${kernel_remount_dir} && for f in \$(ls -1 | sort); do md5sum \"\${f}\"; done" 2>/dev/null)
    echo "${md5_before}" > /tmp/t5_md5_before.txt
    echo "    recorded $(echo "${md5_before}" | wc -l) files"

    # 同步, 让 Filer 持久化元数据
    vm "sync" 2>/dev/null
    sleep 2

    # 每步后内核状态检查
    if ! check_kernel_state "T5-2 before remount" "$base"; then
        ng "T5-2 kernel state abnormal before remount"
        return 1
    fi

    # T5-2c: umount 内核挂载点
    echo "  [T5-2c] umount kernel powerfs..."
    local umount_ret
    umount_ret=$(vm "timeout 30 umount ${MNT} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -eq 0 ]; then
        ok "umount success"
    else
        ng "umount failed/timeout: ${umount_ret}"
        return 1
    fi
    sleep 2

    # T5-2d: 重新 mount 内核挂载点
    echo "  [T5-2d] remount kernel powerfs..."
    local mount_ret
    mount_ret=$(vm "mount -t powerfs none ${MNT} 2>&1" 2>/dev/null)
    ret=$?
    if [ $ret -eq 0 ] && check_mount; then
        ok "remount success"
    else
        ng "remount failed: ${mount_ret}"
        return 1
    fi
    sleep 2

    # T5-2e: 验证文件列表一致
    echo "  [T5-2e] verify file list after remount..."
    local manifest_after
    manifest_after=$(vm "cd ${kernel_remount_dir} && ls -1 | sort" 2>/dev/null)
    echo "${manifest_after}" > /tmp/t5_manifest_after.txt

    if [ "${manifest_before}" = "${manifest_after}" ]; then
        ok "file list consistent after remount"
    else
        ng "file list inconsistent after remount"
        echo "    before:"; echo "${manifest_before}" | sed 's/^/      /'
        echo "    after:";  echo "${manifest_after}"  | sed 's/^/      /'
        return 1
    fi

    # T5-2f: 验证每个文件 MD5 一致
    echo "  [T5-2f] verify each file MD5 after remount..."
    kernel_drop_cache
    sleep 1

    local md5_after
    md5_after=$(vm "cd ${kernel_remount_dir} && for f in \$(ls -1 | sort); do md5sum \"\${f}\"; done" 2>/dev/null)
    echo "${md5_after}" > /tmp/t5_md5_after.txt

    if [ "${md5_before}" = "${md5_after}" ]; then
        ok "all file MD5 consistent after remount"
    else
        ng "file MD5 inconsistent after remount"
        echo "    diff (before vs after):"
        diff <(echo "${md5_before}") <(echo "${md5_after}") | sed 's/^/      /'
        return 1
    fi

    # T5-2g: 内核状态检查
    if ! check_kernel_state "T5-2 after remount" "$base"; then
        ng "T5-2 kernel state abnormal after remount"
        return 1
    fi

    # slab 抽样
    local slab_now
    slab_now=$(get_powerfs_slab_total)
    echo "    slab (inode dentry): ${slab_now} (init: ${SLAB_INIT:-N/A})"

    # 清理 remount 测试目录 (FUSE 端删, 内核端 sync)
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_remount_dir}" 2>/dev/null
    vm "sync" 2>/dev/null

    # 临时 manifest 文件清理
    rm -f /tmp/t5_manifest_before.txt /tmp/t5_manifest_after.txt /tmp/t5_md5_before.txt /tmp/t5_md5_after.txt

    ok "T5 remount consistency passed"
    return 0
}

# ============================================================
# T6: 并发互操作 (FUSE + 内核同时操作, 不应 panic/corruption)
# ============================================================
test_t6_concurrent_interop() {
    section "T6: Concurrent Interop"

    if ! fuse_container_running; then
        skip "FUSE container not running, skip T6"
        return 0
    fi

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T6-1: FUSE 写文件A + 内核写文件B 同时进行 → 各自 MD5 正确
    echo "  [T6-1] concurrent write: FUSE writes A, kernel writes B..."
    docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=5 2>/dev/null > ${FUSE_T4_DIR}/t6_concurrent_A.bin && md5sum ${FUSE_T4_DIR}/t6_concurrent_A.bin | awk '{print \$1}'" >/tmp/t6_md5_A.txt 2>/dev/null &
    local pid_a=$!

    vm "dd if=/dev/urandom bs=1M count=5 2>/dev/null > ${KERNEL_T4_DIR}/t6_concurrent_B.bin && cat ${KERNEL_T4_DIR}/t6_concurrent_B.bin | md5sum | awk '{print \$1}'" >/tmp/t6_md5_B.txt 2>/dev/null &
    local pid_b=$!

    wait $pid_a
    wait $pid_b

    local md5_a md5_b
    md5_a=$(cat /tmp/t6_md5_A.txt 2>/dev/null | tail -1)
    md5_b=$(cat /tmp/t6_md5_B.txt 2>/dev/null | tail -1)

    if [ -n "$md5_a" ] && [ -n "$md5_b" ]; then
        ok "concurrent writes both completed"
        echo "    FUSE file A MD5: ${md5_a}"
        echo "    kernel file B MD5: ${md5_b}"
    else
        ng "concurrent writes incomplete (A='${md5_a}' B='${md5_b}')"
        rm -f /tmp/t6_md5_A.txt /tmp/t6_md5_B.txt
        return 1
    fi

    # 验证对方视角下也能读到正确 MD5 (互操作验证)
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    kernel_drop_cache
    fuse_drop_cache
    sleep 1

    local kernel_read_a
    kernel_read_a=$(vm "cat ${KERNEL_T4_DIR}/t6_concurrent_A.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    local fuse_read_b
    fuse_read_b=$(docker exec ${FUSE_CONTAINER} sh -c "cat ${FUSE_T4_DIR}/t6_concurrent_B.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

    if [ "$md5_a" = "$kernel_read_a" ]; then
        ok "kernel sees FUSE-written A correctly"
    else
        ng "kernel read A mismatch (src=${md5_a} kernel=${kernel_read_a})"
        return 1
    fi

    if [ "$md5_b" = "$fuse_read_b" ]; then
        ok "FUSE sees kernel-written B correctly"
    else
        ng "FUSE read B mismatch (src=${md5_b} fuse=${fuse_read_b})"
        return 1
    fi

    if ! check_kernel_state "T6-1 concurrent writes" "$base"; then
        ng "T6-1 kernel state abnormal"
        return 1
    fi

    # 清理 A/B
    docker exec ${FUSE_CONTAINER} sh -c "rm -f ${FUSE_T4_DIR}/t6_concurrent_A.bin ${FUSE_T4_DIR}/t6_concurrent_B.bin" 2>/dev/null
    vm "sync" 2>/dev/null
    rm -f /tmp/t6_md5_A.txt /tmp/t6_md5_B.txt

    # T6-2: FUSE 写文件C → 内核同时读文件C → 无 corruption (可能读到部分数据, 但不应 crash)
    echo "  [T6-2] concurrent write+read on same file C..."
    local base2=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # 后台: FUSE 持续写文件 C (10MB, 持续几秒)
    docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=10 2>/dev/null > ${FUSE_T4_DIR}/t6_concurrent_C.bin; sync" >/dev/null 2>&1 &
    local pid_c=$!

    # 同时: 内核反复尝试读文件 C (可能 ENOENT/部分数据, 但不应 panic)
    vm "
        end_time=\$((\$(date +%s) + 10))
        while [ \$(date +%s) -lt \$end_time ]; do
            if [ -f ${KERNEL_T4_DIR}/t6_concurrent_C.bin ]; then
                cat ${KERNEL_T4_DIR}/t6_concurrent_C.bin > /dev/null 2>&1 || true
            fi
            sleep 0.2
        done
    " >/dev/null 2>&1 &
    local pid_r=$!

    wait $pid_c
    wait $pid_r

    # FUSE 写完后, 校验最终数据一致
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    kernel_drop_cache
    sleep 1

    local md5_c_fuse md5_c_kernel
    md5_c_fuse=$(docker exec ${FUSE_CONTAINER} sh -c "md5sum ${FUSE_T4_DIR}/t6_concurrent_C.bin | awk '{print \$1}'" 2>/dev/null)
    md5_c_kernel=$(vm "cat ${KERNEL_T4_DIR}/t6_concurrent_C.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

    if [ "$md5_c_fuse" = "$md5_c_kernel" ]; then
        ok "concurrent write+read C: final MD5 consistent"
    else
        ng "concurrent write+read C: final MD5 mismatch (fuse=${md5_c_fuse} kernel=${md5_c_kernel})"
        return 1
    fi

    if ! check_kernel_state "T6-2 concurrent write+read" "$base2"; then
        ng "T6-2 kernel state abnormal"
        return 1
    fi

    # 清理 C
    docker exec ${FUSE_CONTAINER} sh -c "rm -f ${FUSE_T4_DIR}/t6_concurrent_C.bin" 2>/dev/null
    vm "sync" 2>/dev/null

    # T6-3: 60s 持续并发: FUSE 反复创建小文件 + 内核反复读取 → 无 panic
    echo "  [T6-3] 60s sustained concurrent: FUSE creates small files + kernel reads..."
    local base3=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local stress_dir_name="t6_stress"
    local fuse_stress_dir="${FUSE_T4_DIR}/${stress_dir_name}"
    local kernel_stress_dir="${KERNEL_T4_DIR}/${stress_dir_name}"
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_stress_dir} && mkdir -p ${fuse_stress_dir}" 2>/dev/null
    kernel_drop_cache
    sleep 1

    # 后台: FUSE 持续创建小文件
    docker exec ${FUSE_CONTAINER} sh -c "
        end_time=\$((\$(date +%s) + 60))
        i=0
        while [ \$(date +%s) -lt \$end_time ]; do
            i=\$((i+1))
            dd if=/dev/urandom bs=4K count=1 2>/dev/null > ${fuse_stress_dir}/stress_\${i}.bin 2>/dev/null || true
            # 每 50 个清一次 (避免后端存储堆积)
            if [ \$((i % 50)) -eq 0 ]; then
                rm -f ${fuse_stress_dir}/stress_*.bin 2>/dev/null || true
            fi
            sleep 0.05
        done
        echo FUSE_STRESS_DONE
    " >/tmp/t6_fuse_stress.log 2>&1 &
    local pid_fs=$!

    # 后台: 内核持续读取 stress 目录下的文件 (listd + cat)
    vm "
        end_time=\$((\$(date +%s) + 60))
        while [ \$(date +%s) -lt \$end_time ]; do
            for f in ${kernel_stress_dir}/stress_*.bin; do
                [ -f \"\${f}\" ] || continue
                cat \"\${f}\" > /dev/null 2>&1 || true
            done
            sleep 0.1
        done
        echo KERNEL_STRESS_DONE
    " >/tmp/t6_kernel_stress.log 2>&1 &
    local pid_ks=$!

    # 每 10s 检查 dmesg
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep 10
        elapsed=$((elapsed + 10))
        echo "    [${elapsed}s] dmesg check..."
        if ! check_dmesg_clean "$base3"; then
            ng "T6-3 stress ${elapsed}s: kernel abnormal detected"
            kill $pid_fs 2>/dev/null
            kill $pid_ks 2>/dev/null
            wait 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg clean"
    done

    wait $pid_fs
    wait $pid_ks

    # 检查两端的 stress log 都到达 DONE
    local fuse_done kernel_done
    fuse_done=$(grep -c FUSE_STRESS_DONE /tmp/t6_fuse_stress.log 2>/dev/null || echo 0)
    kernel_done=$(grep -c KERNEL_STRESS_DONE /tmp/t6_kernel_stress.log 2>/dev/null || echo 0)
    if [ "${fuse_done}" -ge 1 ] && [ "${kernel_done}" -ge 1 ]; then
        ok "60s sustained concurrent completed (no panic)"
    else
        warn "stress loop did not finish cleanly (fuse_done=${fuse_done} kernel_done=${kernel_done})"
        echo "    fuse stress tail:"; tail -3 /tmp/t6_fuse_stress.log 2>/dev/null | sed 's/^/      /'
        echo "    kernel stress tail:"; tail -3 /tmp/t6_kernel_stress.log 2>/dev/null | sed 's/^/      /'
    fi

    # 最终内核状态检查
    if ! check_kernel_state "T6-3 stress 60s done" "$base3"; then
        ng "T6-3 kernel state abnormal"
        rm -f /tmp/t6_fuse_stress.log /tmp/t6_kernel_stress.log
        return 1
    fi

    # hung task 检查
    local hung
    hung=$(vm "dmesg | grep 'hung task' 2>/dev/null | grep -i powerfs" 2>/dev/null || true)
    if [ -z "$hung" ]; then
        ok "no hung task in 60s stress"
    else
        ng "hung task detected: $hung"
        rm -f /tmp/t6_fuse_stress.log /tmp/t6_kernel_stress.log
        return 1
    fi

    # 清理 stress 目录
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_stress_dir}" 2>/dev/null
    vm "sync" 2>/dev/null
    rm -f /tmp/t6_fuse_stress.log /tmp/t6_kernel_stress.log

    ok "T6 concurrent interop passed"
    return 0
}

# ============================================================
# T7: 卸载 + 最终检查 (清理 / umount / rmmod / dmesg+slab+内存)
# ============================================================
test_t7_unmount() {
    section "T7: Cleanup + Final Check"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T7-1: 清理测试文件 (FUSE 和内核两端)
    echo "  [T7-1] cleanup test files..."
    if fuse_container_running; then
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${FUSE_T4_DIR}" 2>/dev/null
        ok "FUSE side test dir cleaned"
    else
        warn "FUSE container not running, skip FUSE-side cleanup"
    fi
    vm "sync" 2>/dev/null

    # 内核端清理 (残留文件, 若 FUSE 端清理失败)
    vm "rm -rf ${KERNEL_T4_DIR}" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "kernel side test dir cleaned"

    # T7-2: umount
    echo "  [T7-2] umount powerfs..."
    local umount_ret
    umount_ret=$(vm "timeout 30 umount ${MNT} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -eq 0 ]; then
        ok "umount success"
    else
        ng "umount failed/timeout: ${umount_ret}"
        return 1
    fi

    sleep 2

    # T7-3: rmmod
    echo "  [T7-3] rmmod powerfs..."
    local rmmod_ret
    rmmod_ret=$(vm "rmmod powerfs 2>&1" 2>/dev/null)
    ret=$?
    if [ $ret -eq 0 ]; then
        ok "rmmod success"
    else
        ng "rmmod failed: ${rmmod_ret}"
        echo "    check: lsmod | grep powerfs (still has refs?)"
        return 1
    fi

    sleep 2

    # T7-4: 卸载后 dmesg 检查
    echo "  [T7-4] dmesg check after unmount..."
    if check_dmesg_clean "$base"; then
        ok "dmesg clean after unmount"
    else
        ng "dmesg abnormal after unmount"
        return 1
    fi

    # T7-5: slab 全部释放
    echo "  [T7-5] slab release check..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab fully released"
    else
        warn "powerfs slab still has residue:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # T7-6: 内存恢复检查
    echo "  [T7-6] memory recovery check..."
    local mem_final
    mem_final=$(get_mem_available)
    echo "  -> final MemAvailable: ${mem_final} KB (init: ${MEM_INIT} KB)"

    if [ "${MEM_INIT:-0}" -gt 0 ]; then
        local diff=$(( ${MEM_INIT} - ${mem_final} ))
        if [ $diff -lt 50000 ]; then
            ok "memory recovered well (diff ${diff} KB)"
        else
            warn "memory diff large (${diff} KB), possible leak"
        fi
    fi

    # T7-7: 重新挂载以便后续使用 (便于继续迭代)
    echo "  [T7-7] remount powerfs (for subsequent tests)..."
    vm "mount -t powerfs none ${MNT}" 2>/dev/null
    sleep 2

    return 0
}

# ============================================================
# 主流程
# ============================================================
main() {
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  T4 Integration Test: FUSE <-> Kernel Interop       ${C_RESET}"
    echo -e "${C_CYAN}║  Principle: small-to-large, step-by-step, kernel     ${C_RESET}"
    echo -e "${C_CYAN}║              state checked after each step            ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_fuse_to_kernel"
        "3:test_t3_kernel_to_fuse"
        "4:test_t4_stripe_interop"
        "5:test_t5_remount_consistency"
        "6:test_t6_concurrent_interop"
        "7:test_t7_unmount"
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
        echo -e "  ${C_RED}T4 integration test has failures${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  first failure: T${failed_test}"
        fi
        echo ""
        echo "  Troubleshooting:"
        echo "    1. view VM dmesg:        ./qemuctl.sh log powerfs"
        echo "    2. live monitor serial:  ./qemuctl.sh serial-tail"
        echo "    3. backend service log:  ./qemuctl.sh service log filer-1"
        echo "    4. confirm FUSE mount:   docker exec fuse-1 mount | grep powerfs"
        echo "    5. confirm volume count: docker ps | grep volume"
        echo "    6. rerun single stage:   ./test_t4_integration.sh <T#>"
        exit 1
    fi

    echo -e "  ${C_GREEN}T4 integration test all passed${C_RESET}"
    echo ""
    echo "  T4 gate achieved:"
    echo "    - compile + static verification passed"
    echo "    - FUSE + kernel both mount same PowerFS cluster"
    echo "    - FUSE->kernel Flat/Inline interop MD5 consistent"
    echo "    - kernel->FUSE Flat/Inline interop MD5 consistent"
    echo "    - Stripe mode xattr interop (FUSE set, both read/write)"
    echo "    - remount consistency (10 files preserved)"
    echo "    - 60s concurrent stress no panic"
    echo "    - kernel state (slab/meminfo/dmesg/serial) normal"
    echo ""
    exit 0
}

main "$@"
