#!/bin/bash
# T6 阶段稳定性测试: 长时间持续运行 + 内存泄漏 + 高并发压力
#
# 测试内容:
#   T0: 编译 + 静态验证 (make + verify_module.sh)
#   T1: QEMU 启动 + 挂载 + 记录初始 slab/mem 基线
#   T2: 持续顺序写 (10 分钟, STAB_T2_DURATION 调整)
#   T3: 持续随机读写混合 (30 分钟, STAB_T3_DURATION 调整)
#   T4: 高并发压力 (10 分钟, STAB_T4_DURATION 调整)
#   T5: 内存泄漏检测 (5 分钟 x2 循环, STAB_T5_DURATION 调整)
#   T6: 长时间挂载稳定性 (10 分钟空闲, STAB_DURATION 调整)
#   T7: 卸载 + 最终检查 (与 T1 基线对比)
#
# 核心原则:
#   - 从小到大逐个确认 (10min → 30min → 1h), 前一档未通过不进入下一档
#   - 每步检查内核状态 (dmesg/slab/meminfo/D 状态)
#   - 持续运行期间定期检查 dmesg/slab/mem
#   - 内存泄漏检测: 对比多次 slab/mem 数据 (slab 增长 < 10%, MemAvailable 下降 < 5%)
#   - 无 fio 时用 dd/cp 替代, 但标注 "non-fio fallback"
#
# 运行环境: HOST (通过 SSH 控制 VM)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - 或由本脚本 T1 自动完成
#
# 用法:
#   ./test_t6_stability.sh            # 运行全部 (T0-T7)
#   ./test_t6_stability.sh 2          # 仅运行 T2
#   ./test_t6_stability.sh 2 5 6      # 运行 T2+T5+T6
#
# 环境变量 (覆盖默认时长, 单位: 秒):
#   STAB_T2_DURATION  T2 持续顺序写时长          (默认 600)
#   STAB_T3_DURATION  T3 持续随机读写时长         (默认 1800)
#   STAB_T4_DURATION  T4 高并发压力时长           (默认 600)
#   STAB_T5_DURATION  T5 单次写+删循环时长         (默认 300)
#   STAB_DURATION      T6 空闲挂载稳定性时长        (默认 600)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

# 各阶段持续时间 (秒, 可通过环境变量覆盖)
T2_DURATION="${STAB_T2_DURATION:-600}"
T3_DURATION="${STAB_T3_DURATION:-1800}"
T4_DURATION="${STAB_T4_DURATION:-600}"
T5_DURATION="${STAB_T5_DURATION:-300}"
T6_DURATION="${STAB_DURATION:-600}"

PASS=0
FAIL=0
WARN=0
SKIP=0
SLAB_INIT=""        # T1 记录的初始 slab (inode dentry)
MEM_INIT=""         # T1 记录的初始 MemAvailable (KB)
HAS_FIO=0
TREND_FILE="/tmp/t6_stab_trend.txt"   # slab/mem 趋势记录文件

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

# 检查 dmesg 是否有异常 (oops/bug/kasan/stall/lockup/oom)
# 参数 $1: 基线行号 (可选, 默认检查全部)
# 返回: 0=正常, 1=有异常
check_dmesg_clean() {
    local base="${1:-0}"
    local log
    log=$(dmesg_since "$base" 2>/dev/null)

    local errors
    errors=$(echo "$log" | grep -E 'BUG:|Oops:|general protection fault|unable to handle|NULL pointer dereference|KASAN:|RCU stall|workqueue lockup|hung task|soft lockup|hard lockup|NMI watchdog|call trace|Kernel panic|Segmentation fault|Out of memory|oom-kill|invoked oom-killer' || true)

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
        echo "  ${C_RED}D-state powerfs threads detected:${C_RESET}"
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
        ok "dmesg clean (no oops/bug/kasan/stall/oom)"
    else
        ng "dmesg anomaly (${desc})"
        state_ok=1
    fi

    # 2. 无 D 状态线程
    if check_d_state; then
        ok "no D-state powerfs threads"
    else
        ng "D-state (hung) powerfs threads detected (${desc})"
        state_ok=1
    fi

    # 3. serial 日志 lockup/panic 检查 (补充 dmesg, VM 卡死时也能查)
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        local serial_errors
        serial_errors=$(grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task|Out of memory|oom-kill' "${qemu_log}" 2>/dev/null | tail -5 || true)
        if [ -z "$serial_errors" ]; then
            ok "serial log clean (no lockup/panic/oom)"
        else
            ng "serial log lockup/panic/oom detected (${desc})"
            echo "$serial_errors" | sed 's/^/      /'
            state_ok=1
        fi
    fi

    return $state_ok
}

# 记录一行 slab/mem 趋势到 TREND_FILE
# 参数: $1=label $2=slab_str(inode dentry) $3=mem_kb
record_trend() {
    local label="$1"
    local slab_str="$2"
    local mem="$3"
    local ts
    ts=$(date '+%H:%M:%S')
    echo "${ts} ${label} slab(inode+dentry)=${slab_str} MemAvailable=${mem}KB" >> "${TREND_FILE}"
    echo "    [trend] ${ts} ${label} slab=${slab_str} mem=${mem}KB"
}

# 检查 VM 内是否有 fio
has_fio_available() {
    local fio_path
    fio_path=$(vm "which fio 2>/dev/null || echo no" 2>/dev/null)
    if [ "$fio_path" = "no" ] || [ -z "$fio_path" ]; then
        return 1
    fi
    return 0
}

# ============================================================
# T0: 编译 + 静态验证
# ============================================================
test_t0_compile() {
    section "T0: compile + static verification"

    # T0-1: 编译
    echo "  [T0-1] compile powerfs.ko..."
    cd "${POWERFS_MOD_DIR}"
    local build_log
    build_log=$(make clean 2>&1 && make -j$(nproc) 2>&1)
    local build_ret=$?

    if [ $build_ret -eq 0 ] && [ -f powerfs.ko ]; then
        ok "powerfs.ko compiled ($(ls -la powerfs.ko | awk '{print $5}') bytes)"
    else
        ng "powerfs.ko compile failed"
        echo "$build_log" | tail -20 | sed 's/^/    /'
        cd "${SCRIPT_DIR}"
        return 1
    fi

    # 编译警告检查 (非阻断)
    local warnings
    warnings=$(echo "$build_log" | grep -iE 'warning:' | grep -v 'Wno-' || true)
    if [ -z "$warnings" ]; then
        ok "no compile warnings"
    else
        warn "compile has warnings (non-blocking):"
        echo "$warnings" | head -5 | sed 's/^/    /'
    fi

    # T0-2: 静态符号验证
    echo "  [T0-2] verify_module.sh..."
    if bash verify_module.sh 2>&1 | tail -5 | grep -q "ALL VERIFICATION TESTS PASSED"; then
        ok "verify_module.sh all passed"
    else
        ng "verify_module.sh has failures"
        bash verify_module.sh 2>&1 | grep '\[FAIL\]' | sed 's/^/    /'
        cd "${SCRIPT_DIR}"
        return 1
    fi

    cd "${SCRIPT_DIR}"
    return 0
}

# ============================================================
# T1: QEMU 启动 + 挂载 + 记录初始基线
# ============================================================
test_t1_mount() {
    section "T1: QEMU boot + mount + baseline"

    # T1-1: 检查 Docker 后端服务
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

    # T1-2: 检查 QEMU 运行
    echo "  [T1-2] check QEMU status..."
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
    echo "  [T1-3] check VM SSH reachability..."
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

    # T1-5: 模块引用计数
    echo "  [T1-5] check module refcount..."
    local mod_info
    mod_info=$(vm "lsmod | grep powerfs" 2>/dev/null)
    if [ -n "$mod_info" ]; then
        ok "powerfs module loaded: ${mod_info}"
    else
        ng "powerfs module not loaded"
        return 1
    fi

    # T1-6: 挂载后 30s dmesg 观察
    echo "  [T1-6] 30s observation after mount..."
    local base
    base=$(dmesg_line_count)
    sleep 30
    if check_kernel_state "30s after mount" "$base"; then
        ok "kernel state normal 30s after mount"
    else
        ng "kernel state abnormal 30s after mount"
        return 1
    fi

    # 记录初始 slab 和 meminfo 基线
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "  -> initial slab (inode dentry): ${SLAB_INIT}"
    echo "  -> initial MemAvailable: ${MEM_INIT} KB"
    : > "${TREND_FILE}"
    record_trend "T1-baseline" "${SLAB_INIT}" "${MEM_INIT}"

    # T1-7: 检查 VM 内 fio
    echo "  [T1-7] check VM fio availability..."
    if has_fio_available; then
        ok "fio available in VM"
        HAS_FIO=1
    else
        warn "fio not available in VM, will use dd/cp fallback (marked 'non-fio fallback')"
        HAS_FIO=0
    fi

    return 0
}

# ============================================================
# T2: 持续顺序写 (默认 10 分钟)
# ============================================================
test_t2_seq_write() {
    section "T2: sustained sequential write (${T2_DURATION}s)"

    local base
    base=$(dmesg_line_count)
    local slab_before mem_before
    slab_before=$(get_powerfs_slab_total)
    mem_before=$(get_mem_available)
    echo "  -> before T2 slab: ${slab_before}"
    echo "  -> before T2 MemAvailable: ${mem_before} KB"
    record_trend "T2-start" "${slab_before}" "${mem_before}"

    # 运行写压力测试 (fio 或 dd 替代)
    local ret=0
    if [ "${HAS_FIO}" = "1" ]; then
        ret=$(_t2_run_fio)
    else
        warn "non-fio fallback: using dd loop for ${T2_DURATION}s"
        ret=$(_t2_run_dd)
    fi
    [ $ret -ne 0 ] && return 1

    # T2 完成后检查
    echo ""
    echo "  [T2-post] post-run checks..."
    local slab_after mem_after
    slab_after=$(get_powerfs_slab_total)
    mem_after=$(get_mem_available)
    echo "  -> after T2 slab: ${slab_after} (before: ${slab_before})"
    echo "  -> after T2 MemAvailable: ${mem_after} KB (before: ${mem_before} KB)"
    record_trend "T2-end" "${slab_after}" "${mem_after}"

    # 内核状态最终检查
    if check_kernel_state "T2 sustained write" "$base"; then
        ok "T2 kernel state clean"
    else
        ng "T2 kernel state abnormal"
        return 1
    fi

    # IO 错误检查 (dmesg 中的 IO error/EIO)
    local io_err
    io_err=$(dmesg_since "$base" 2>/dev/null | grep -iE 'I/O error|EIO|read error|write error|buffer I/O error' | head -10 || true)
    if [ -z "$io_err" ]; then
        ok "no IO errors in dmesg"
    else
        ng "IO errors detected in dmesg"
        echo "$io_err" | sed 's/^/    /'
        return 1
    fi

    # 清理测试文件
    vm "rm -f ${MNT}/stab_seq_write.* ${MNT}/t2_dd_* 2>/dev/null" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T2 sustained sequential write passed"
    return 0
}

# T2 fio 子流程: 后台运行 + 每 30s 检查 dmesg/slab/mem
# 依赖调用者 test_t2_seq_write 中的 $base
_t2_run_fio() {
    echo "  [T2-fio] fio sequential write (bs=1M size=500M runtime=${T2_DURATION}s)..."
    vm "fio --name=stab_seq_write --directory=${MNT} --rw=write --bs=1M --size=500M --runtime=${T2_DURATION} --time_based --end_fsync=1 --numjobs=1 --group_reporting 2>&1 | tail -10" 2>/dev/null &
    local fio_pid=$!

    local check_interval=30
    local elapsed=0
    while [ $elapsed -lt $T2_DURATION ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        [ $elapsed -gt $T2_DURATION ] && elapsed=$T2_DURATION
        echo "    [${elapsed}s] periodic check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly detected at ${elapsed}s during fio write"
            kill $fio_pid 2>/dev/null
            wait $fio_pid 2>/dev/null
            return 1
        fi
        local s m
        s=$(get_powerfs_slab_total)
        m=$(get_mem_available)
        record_trend "T2-${elapsed}s" "${s}" "${m}"
        echo "    [${elapsed}s] dmesg clean"
    done

    wait $fio_pid 2>/dev/null
    local wret=$?
    if [ $wret -eq 0 ]; then
        ok "fio sequential write completed (${T2_DURATION}s)"
        return 0
    else
        ng "fio sequential write exited with error (ret=${wret})"
        return 1
    fi
}

# T2 dd 替代方案 (non-fio fallback)
# 依赖调用者 test_t2_seq_write 中的 $base
_t2_run_dd() {
    echo "  [T2-dd] non-fio fallback: dd loop write ${T2_DURATION}s"
    vm "end_time=\$((\$(date +%s) + ${T2_DURATION})); i=0; while [ \$(date +%s) -lt \$end_time ]; do i=\$((i+1)); dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${MNT}/t2_dd_\${i}.bin; done; echo done" 2>/dev/null &
    local dd_pid=$!

    local check_interval=30
    local elapsed=0
    while [ $elapsed -lt $T2_DURATION ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        [ $elapsed -gt $T2_DURATION ] && elapsed=$T2_DURATION
        echo "    [${elapsed}s] periodic check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly detected at ${elapsed}s during dd write"
            kill $dd_pid 2>/dev/null
            wait $dd_pid 2>/dev/null
            return 1
        fi
        local s m
        s=$(get_powerfs_slab_total)
        m=$(get_mem_available)
        record_trend "T2-dd-${elapsed}s" "${s}" "${m}"
        echo "    [${elapsed}s] dmesg clean"
    done

    wait $dd_pid 2>/dev/null
    ok "dd loop write completed (${T2_DURATION}s)"
    return 0
}

# ============================================================
# T3: 持续随机读写混合 (默认 30 分钟)
# ============================================================
test_t3_rand_rw() {
    section "T3: sustained random read/write mix (${T3_DURATION}s)"

    local base
    base=$(dmesg_line_count)

    # T3-pre: 预先生成已知 MD5 的文件用于完成后完整性校验
    echo "  [T3-pre] create known file for integrity check..."
    local known_file="${MNT}/t3_known.bin"
    local known_md5
    known_md5=$(vm "dd if=/dev/urandom bs=1M count=10 2>/dev/null | tee ${known_file} | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$known_md5" ]; then
        ng "failed to create known file for integrity check"
        return 1
    fi
    echo "    known file MD5 (before): ${known_md5}"

    local slab_before mem_before
    slab_before=$(get_powerfs_slab_total)
    mem_before=$(get_mem_available)
    echo "  -> before T3 slab: ${slab_before}"
    echo "  -> before T3 MemAvailable: ${mem_before} KB"
    record_trend "T3-start" "${slab_before}" "${mem_before}"

    # 运行随机读写混合 (fio 或 dd+cp 替代)
    local ret=0
    if [ "${HAS_FIO}" = "1" ]; then
        ret=$(_t3_run_fio)
    else
        warn "non-fio fallback: using multi-process dd+cp loop for ${T3_DURATION}s"
        ret=$(_t3_run_dd)
    fi
    [ $ret -ne 0 ] && return 1

    # T3 完成后检查
    echo ""
    echo "  [T3-post] post-run checks..."
    local slab_after mem_after
    slab_after=$(get_powerfs_slab_total)
    mem_after=$(get_mem_available)
    echo "  -> after T3 slab: ${slab_after} (before: ${slab_before})"
    echo "  -> after T3 MemAvailable: ${mem_after} KB (before: ${mem_before} KB)"
    record_trend "T3-end" "${slab_after}" "${mem_after}"

    # 数据完整性校验 (已知文件 MD5)
    echo "  [T3-integrity] verify known file MD5..."
    local known_md5_after
    known_md5_after=$(vm "cat ${known_file} | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    known file MD5 (after):  ${known_md5_after}"
    if [ "$known_md5" = "$known_md5_after" ]; then
        ok "known file MD5 consistent"
    else
        ng "known file MD5 mismatch (before=${known_md5} after=${known_md5_after})"
        return 1
    fi

    # 内核状态检查
    if check_kernel_state "T3 randrw" "$base"; then
        ok "T3 kernel state clean"
    else
        ng "T3 kernel state abnormal"
        return 1
    fi

    # slab 持续增长检测 (重点关注内存泄漏)
    local slab_before_sum slab_after_sum
    slab_before_sum=$(echo "$slab_before" | awk '{print $1+$2}')
    slab_after_sum=$(echo "$slab_after" | awk '{print $1+$2}')
    if [ "${slab_before_sum:-0}" -gt 0 ]; then
        local slab_growth_pct
        slab_growth_pct=$(( (slab_after_sum - slab_before_sum) * 100 / slab_before_sum ))
        if [ $slab_growth_pct -lt 20 ]; then
            ok "slab growth ${slab_growth_pct}% during T3 (acceptable, < 20%)"
        else
            warn "slab growth ${slab_growth_pct}% during T3 (potential leak, will verify in T5)"
        fi
    fi

    # 清理
    vm "rm -f ${known_file} ${MNT}/stab_rand_rw.* ${MNT}/t3_dd_* 2>/dev/null" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T3 sustained random read/write mix passed"
    return 0
}

# T3 fio 子流程: 后台运行 + 每 60s 检查 dmesg/slab/mem/D 状态
# 依赖调用者 test_t3_rand_rw 中的 $base
_t3_run_fio() {
    echo "  [T3-fio] fio randrw (bs=4k size=200M runtime=${T3_DURATION}s numjobs=4 rwmixread=70)..."
    vm "fio --name=stab_rand_rw --directory=${MNT} --rw=randrw --rwmixread=70 --bs=4k --size=200M --runtime=${T3_DURATION} --time_based --numjobs=4 --group_reporting 2>&1 | tail -10" 2>/dev/null &
    local fio_pid=$!

    local check_interval=60
    local elapsed=0
    while [ $elapsed -lt $T3_DURATION ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        [ $elapsed -gt $T3_DURATION ] && elapsed=$T3_DURATION
        echo "    [${elapsed}s] periodic check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly at ${elapsed}s during randrw"
            kill $fio_pid 2>/dev/null
            wait $fio_pid 2>/dev/null
            return 1
        fi
        if ! check_d_state; then
            ng "D-state powerfs threads at ${elapsed}s during randrw"
            kill $fio_pid 2>/dev/null
            wait $fio_pid 2>/dev/null
            return 1
        fi
        local s m
        s=$(get_powerfs_slab_total)
        m=$(get_mem_available)
        record_trend "T3-${elapsed}s" "${s}" "${m}"
        echo "    [${elapsed}s] dmesg + D-state clean"
    done

    wait $fio_pid 2>/dev/null
    local wret=$?
    if [ $wret -eq 0 ]; then
        ok "fio randrw completed (${T3_DURATION}s)"
        return 0
    else
        ng "fio randrw exited with error (ret=${wret})"
        return 1
    fi
}

# T3 dd 替代方案 (non-fio fallback): 多进程 dd + cp 循环
# 依赖调用者 test_t3_rand_rw 中的 $base
_t3_run_dd() {
    echo "  [T3-dd] non-fio fallback: 4-process dd+cp loop for ${T3_DURATION}s"
    # 4 个并发循环: dd 写 + cp 复制 (模拟随机读写混合)
    vm "end_time=\$((\$(date +%s) + ${T3_DURATION})); for j in 1 2 3 4; do (i=0; while [ \$(date +%s) -lt \$end_time ]; do i=\$((i+1)); dd if=/dev/urandom bs=4k count=256 2>/dev/null > ${MNT}/t3_dd_w_\${j}_\${i}.bin; cp ${MNT}/t3_dd_w_\${j}_\${i}.bin ${MNT}/t3_dd_r_\${j}_\${i}.bin 2>/dev/null; done) & done; wait; echo done" 2>/dev/null &
    local dd_pid=$!

    local check_interval=60
    local elapsed=0
    while [ $elapsed -lt $T3_DURATION ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        [ $elapsed -gt $T3_DURATION ] && elapsed=$T3_DURATION
        echo "    [${elapsed}s] periodic check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly at ${elapsed}s during dd+cp"
            kill $dd_pid 2>/dev/null
            wait $dd_pid 2>/dev/null
            return 1
        fi
        if ! check_d_state; then
            ng "D-state powerfs threads at ${elapsed}s during dd+cp"
            kill $dd_pid 2>/dev/null
            wait $dd_pid 2>/dev/null
            return 1
        fi
        local s m
        s=$(get_powerfs_slab_total)
        m=$(get_mem_available)
        record_trend "T3-dd-${elapsed}s" "${s}" "${m}"
        echo "    [${elapsed}s] dmesg + D-state clean"
    done

    wait $dd_pid 2>/dev/null
    ok "dd+cp loop completed (${T3_DURATION}s)"
    return 0
}

# ============================================================
# T4: 高并发压力 (默认 10 分钟)
# ============================================================
test_t4_pressure() {
    section "T4: high concurrency pressure (${T4_DURATION}s)"

    local base
    base=$(dmesg_line_count)
    local slab_before mem_before
    slab_before=$(get_powerfs_slab_total)
    mem_before=$(get_mem_available)
    echo "  -> before T4 slab: ${slab_before}"
    echo "  -> before T4 MemAvailable: ${mem_before} KB"
    record_trend "T4-start" "${slab_before}" "${mem_before}"

    # 运行高并发压力 (fio 32 jobs 或 32 个 dd 进程替代)
    local ret=0
    if [ "${HAS_FIO}" = "1" ]; then
        ret=$(_t4_run_fio)
    else
        warn "non-fio fallback: using 32 background dd processes for ${T4_DURATION}s"
        ret=$(_t4_run_dd)
    fi
    [ $ret -ne 0 ] && return 1

    # T4 完成后检查
    echo ""
    echo "  [T4-post] post-run checks..."
    local slab_after mem_after
    slab_after=$(get_powerfs_slab_total)
    mem_after=$(get_mem_available)
    echo "  -> after T4 slab: ${slab_after} (before: ${slab_before})"
    echo "  -> after T4 MemAvailable: ${mem_after} KB (before: ${mem_before} KB)"
    record_trend "T4-end" "${slab_after}" "${mem_after}"

    # 内核状态检查 (重点关注 panic/deadlock/hung task)
    if check_kernel_state "T4 pressure" "$base"; then
        ok "T4 kernel state clean (no panic/deadlock/hung task)"
    else
        ng "T4 kernel state abnormal (panic/deadlock/hung task)"
        return 1
    fi

    # 清理
    vm "rm -f ${MNT}/stab_pressure.* ${MNT}/t4_dd_* 2>/dev/null" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T4 high concurrency pressure passed"
    return 0
}

# T4 fio 子流程: 32 jobs 高并发 + 每 30s 检查 dmesg/D 状态
# 依赖调用者 test_t4_pressure 中的 $base
_t4_run_fio() {
    echo "  [T4-fio] fio randrw (bs=4k size=100M runtime=${T4_DURATION}s numjobs=32 group_reporting)..."
    vm "fio --name=stab_pressure --directory=${MNT} --rw=randrw --bs=4k --size=100M --runtime=${T4_DURATION} --time_based --numjobs=32 --group_reporting 2>&1 | tail -10" 2>/dev/null &
    local fio_pid=$!

    local check_interval=30
    local elapsed=0
    while [ $elapsed -lt $T4_DURATION ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        [ $elapsed -gt $T4_DURATION ] && elapsed=$T4_DURATION
        echo "    [${elapsed}s] periodic check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly at ${elapsed}s during pressure (panic/deadlock)"
            kill $fio_pid 2>/dev/null
            wait $fio_pid 2>/dev/null
            return 1
        fi
        if ! check_d_state; then
            ng "D-state (hung) powerfs threads at ${elapsed}s during pressure"
            kill $fio_pid 2>/dev/null
            wait $fio_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg + D-state clean"
    done

    wait $fio_pid 2>/dev/null
    local wret=$?
    if [ $wret -eq 0 ]; then
        ok "fio pressure completed (${T4_DURATION}s)"
        return 0
    else
        ng "fio pressure exited with error (ret=${wret})"
        return 1
    fi
}

# T4 dd 替代方案 (non-fio fallback): 32 个后台 dd 进程
# 依赖调用者 test_t4_pressure 中的 $base
_t4_run_dd() {
    echo "  [T4-dd] non-fio fallback: 32 background dd processes for ${T4_DURATION}s"
    vm "end_time=\$((\$(date +%s) + ${T4_DURATION})); for j in \$(seq 1 32); do (i=0; while [ \$(date +%s) -lt \$end_time ]; do i=\$((i+1)); dd if=/dev/urandom bs=4k count=256 2>/dev/null > ${MNT}/t4_dd_\${j}_\${i}.bin; done) & done; wait; echo done" 2>/dev/null &
    local dd_pid=$!

    local check_interval=30
    local elapsed=0
    while [ $elapsed -lt $T4_DURATION ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        [ $elapsed -gt $T4_DURATION ] && elapsed=$T4_DURATION
        echo "    [${elapsed}s] periodic check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly at ${elapsed}s during dd pressure"
            kill $dd_pid 2>/dev/null
            wait $dd_pid 2>/dev/null
            return 1
        fi
        if ! check_d_state; then
            ng "D-state (hung) powerfs threads at ${elapsed}s during dd pressure"
            kill $dd_pid 2>/dev/null
            wait $dd_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg + D-state clean"
    done

    wait $dd_pid 2>/dev/null
    ok "dd pressure completed (${T4_DURATION}s)"
    return 0
}

# ============================================================
# T5: 内存泄漏检测 (默认单次循环 5 分钟, 共 2 次循环)
# ============================================================
test_t5_leak() {
    section "T5: memory leak detection (${T5_DURATION}s per cycle, 2 cycles)"

    local base
    base=$(dmesg_line_count)

    # 5a: 记录初始 slab/mem (挂载后基线)
    echo "  [T5a] record initial slab/mem (mount baseline)..."
    local slab_5a mem_5a
    slab_5a=$(get_powerfs_slab_total)
    mem_5a=$(get_mem_available)
    echo "    slab_5a (inode dentry): ${slab_5a}"
    echo "    mem_5a: ${mem_5a} KB"
    record_trend "T5a-initial" "${slab_5a}" "${mem_5a}"

    # 5b: fio 顺序写 + rm 循环 (第一个循环)
    echo "  [T5b] run write + rm loop cycle 1 (${T5_DURATION}s)..."
    _t5_run_cycle "b"
    ok "T5b write+rm loop cycle 1 completed"

    # 5c: drop_caches 后记录 slab/mem
    echo "  [T5c] drop_caches + record slab/mem (after cycle 1)..."
    vm "echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 2
    local slab_5c mem_5c
    slab_5c=$(get_powerfs_slab_total)
    mem_5c=$(get_mem_available)
    echo "    slab_5c (inode dentry): ${slab_5c}"
    echo "    mem_5c: ${mem_5c} KB"
    record_trend "T5c-after-drop1" "${slab_5c}" "${mem_5c}"

    # 5d: 再次运行 fio + rm 循环 (第二个循环)
    echo "  [T5d] run write + rm loop cycle 2 (${T5_DURATION}s)..."
    _t5_run_cycle "d"
    ok "T5d write+rm loop cycle 2 completed"

    # 5e: drop_caches 后记录 slab/mem
    echo "  [T5e] drop_caches + record slab/mem (after cycle 2)..."
    vm "echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 2
    local slab_5e mem_5e
    slab_5e=$(get_powerfs_slab_total)
    mem_5e=$(get_mem_available)
    echo "    slab_5e (inode dentry): ${slab_5e}"
    echo "    mem_5e: ${mem_5e} KB"
    record_trend "T5e-after-drop2" "${slab_5e}" "${mem_5e}"

    # 5f: 对比 5a/5c/5e 判断是否有泄漏
    echo ""
    echo "  [T5f] leak analysis (compare 5a / 5c / 5e)..."
    local slab_5a_sum slab_5c_sum slab_5e_sum
    slab_5a_sum=$(echo "$slab_5a" | awk '{print $1+$2}')
    slab_5c_sum=$(echo "$slab_5c" | awk '{print $1+$2}')
    slab_5e_sum=$(echo "$slab_5e" | awk '{print $1+$2}')
    echo "    slab sum: 5a=${slab_5a_sum}  5c=${slab_5c_sum}  5e=${slab_5e_sum}"
    echo "    mem (KB): 5a=${mem_5a}  5c=${mem_5c}  5e=${mem_5e}"

    local leak_fail=0

    # 判断标准 1: slab 增长 < 10% (对比 5a vs 5e)
    if [ "${slab_5a_sum:-0}" -gt 0 ]; then
        local slab_growth_pct
        slab_growth_pct=$(( (slab_5e_sum - slab_5a_sum) * 100 / slab_5a_sum ))
        if [ $slab_growth_pct -lt 10 ]; then
            ok "slab growth ${slab_growth_pct}% (5a->5e, < 10% threshold) — no leak"
        else
            ng "slab growth ${slab_growth_pct}% (5a->5e, >= 10% threshold) — potential leak"
            leak_fail=1
        fi
    else
        warn "slab_5a sum is 0, skip slab growth check"
    fi

    # 判断标准 2: MemAvailable 下降 < 5% (对比 5a vs 5e)
    if [ "${mem_5a:-0}" -gt 0 ]; then
        local mem_drop_pct
        mem_drop_pct=$(( (mem_5a - mem_5e) * 100 / mem_5a ))
        if [ $mem_drop_pct -lt 5 ]; then
            ok "MemAvailable drop ${mem_drop_pct}% (5a->5e, < 5% threshold) — no leak"
        else
            ng "MemAvailable drop ${mem_drop_pct}% (5a->5e, >= 5% threshold) — potential leak"
            leak_fail=1
        fi
    else
        warn "mem_5a is 0, skip mem drop check"
    fi

    # 内核状态检查
    if ! check_kernel_state "T5 leak detection" "$base"; then
        ng "T5 kernel state abnormal"
        return 1
    fi

    # 清理
    vm "rm -f ${MNT}/stab_leak_b.* ${MNT}/stab_leak_d.* ${MNT}/t5_b_* ${MNT}/t5_d_* 2>/dev/null" 2>/dev/null
    vm "sync" 2>/dev/null

    if [ $leak_fail -ne 0 ]; then
        ng "T5 memory leak detection FAILED (slab or mem threshold exceeded)"
        return 1
    fi

    ok "T5 memory leak detection passed"
    return 0
}

# T5 单次写+删循环 (fio 或 dd 替代)
# 参数 $1: 循环标签 (b 或 d)
_t5_run_cycle() {
    local tag="$1"
    if [ "${HAS_FIO}" = "1" ]; then
        # fio 后台写, 同时后台 rm 循环删除独立文件制造 churn
        vm "fio --name=stab_leak_${tag} --directory=${MNT} --rw=write --bs=1M --size=100M --runtime=${T5_DURATION} --time_based --numjobs=1 --group_reporting 2>&1 | tail -5" 2>/dev/null &
        local fio_pid=$!
        # 并发 rm 循环: 创建并删除独立文件 (不干扰 fio 自身文件)
        vm "end_time=\$((\$(date +%s) + ${T5_DURATION})); i=0; while [ \$(date +%s) -lt \$end_time ]; do i=\$((i+1)); dd if=/dev/zero bs=1M count=1 2>/dev/null > ${MNT}/t5_${tag}_churn_\${i}.bin; rm -f ${MNT}/t5_${tag}_churn_\$((i-3)).bin 2>/dev/null; done; echo done" 2>/dev/null &
        local churn_pid=$!
        wait $fio_pid 2>/dev/null
        wait $churn_pid 2>/dev/null
    else
        warn "non-fio fallback: dd write + rm loop (${T5_DURATION}s)"
        vm "end_time=\$((\$(date +%s) + ${T5_DURATION})); i=0; while [ \$(date +%s) -lt \$end_time ]; do i=\$((i+1)); dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${MNT}/t5_${tag}_\${i}.bin; rm -f ${MNT}/t5_${tag}_\$((i-5)).bin 2>/dev/null; done; echo done" 2>/dev/null
    fi
    # 清理本循环所有测试文件
    vm "rm -f ${MNT}/stab_leak_${tag}.* ${MNT}/t5_${tag}_* 2>/dev/null" 2>/dev/null
    vm "sync" 2>/dev/null
}

# ============================================================
# T6: 长时间挂载稳定性 (默认 10 分钟空闲, STAB_DURATION 调整)
# ============================================================
test_t6_long_mount() {
    section "T6: long mount stability (${T6_DURATION}s idle)"

    local base
    base=$(dmesg_line_count)
    local slab_before mem_before
    slab_before=$(get_powerfs_slab_total)
    mem_before=$(get_mem_available)
    echo "  -> before T6 slab: ${slab_before}"
    echo "  -> before T6 MemAvailable: ${mem_before} KB"
    record_trend "T6-start" "${slab_before}" "${mem_before}"

    # T6-1: 空闲等待, 每 60s 检查 dmesg + D 状态 + oom + slab
    echo "  [T6-1] idle wait ${T6_DURATION}s (periodic check every 60s)..."
    local check_interval=60
    local elapsed=0
    local fail=0
    while [ $elapsed -lt $T6_DURATION ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        [ $elapsed -gt $T6_DURATION ] && elapsed=$T6_DURATION
        echo "    [${elapsed}s] periodic check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly at ${elapsed}s during idle (hung/oom)"
            fail=1
            break
        fi
        if ! check_d_state; then
            ng "D-state (hung) powerfs threads at ${elapsed}s during idle"
            fail=1
            break
        fi
        # oom 检查
        local oom
        oom=$(dmesg_since "$base" 2>/dev/null | grep -iE 'Out of memory|oom-kill|invoked oom-killer' | head -3 || true)
        if [ -n "$oom" ]; then
            ng "OOM detected at ${elapsed}s during idle"
            echo "$oom" | sed 's/^/    /'
            fail=1
            break
        fi
        # slab 记录 (检测空闲期间是否持续增长)
        local s m
        s=$(get_powerfs_slab_total)
        m=$(get_mem_available)
        record_trend "T6-${elapsed}s" "${s}" "${m}"
        echo "    [${elapsed}s] dmesg + D-state + oom clean"
    done

    if [ $fail -ne 0 ]; then
        return 1
    fi
    ok "T6 idle wait ${T6_DURATION}s completed, no anomaly"

    # T6-2: 空闲后做一次读写验证确认功能正常
    echo "  [T6-2] post-idle read/write verification..."
    local rw_base
    rw_base=$(dmesg_line_count)

    # 小文件写入
    if vm "echo t6_verify > ${MNT}/t6_verify.txt" 2>/dev/null; then
        ok "post-idle write OK"
    else
        ng "post-idle write FAILED"
        return 1
    fi

    # 读取验证
    local content
    content=$(vm "cat ${MNT}/t6_verify.txt" 2>/dev/null)
    if [ "$content" = "t6_verify" ]; then
        ok "post-idle read OK (content matches)"
    else
        ng "post-idle read mismatch (got '${content}')"
        return 1
    fi

    # 1M 文件 MD5 完整性校验
    local md5_w md5_r
    md5_w=$(vm "dd if=/dev/urandom bs=1M count=1 2>/dev/null | tee ${MNT}/t6_md5.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    vm "sync" 2>/dev/null
    md5_r=$(vm "cat ${MNT}/t6_md5.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ "$md5_w" = "$md5_r" ] && [ -n "$md5_w" ]; then
        ok "post-idle 1M MD5 consistent"
    else
        ng "post-idle 1M MD5 mismatch (w=${md5_w} r=${md5_r})"
        return 1
    fi

    # 内核状态检查
    if ! check_kernel_state "T6 post-idle rw" "$rw_base"; then
        ng "T6 post-idle kernel state abnormal"
        return 1
    fi

    # slab 增长检测 (空闲期间 slab 不应持续增长)
    local slab_after slab_before_sum slab_after_sum
    slab_after=$(get_powerfs_slab_total)
    slab_before_sum=$(echo "$slab_before" | awk '{print $1+$2}')
    slab_after_sum=$(echo "$slab_after" | awk '{print $1+$2}')
    if [ "${slab_before_sum:-0}" -gt 0 ]; then
        local slab_growth_pct
        slab_growth_pct=$(( (slab_after_sum - slab_before_sum) * 100 / slab_before_sum ))
        if [ $slab_growth_pct -le 0 ]; then
            ok "slab not growing during idle (growth ${slab_growth_pct}%)"
        elif [ $slab_growth_pct -lt 10 ]; then
            warn "slab slight growth ${slab_growth_pct}% during idle (acceptable)"
        else
            ng "slab continuous growth ${slab_growth_pct}% during idle (potential leak)"
            return 1
        fi
    fi
    record_trend "T6-end" "${slab_after}" "$(get_mem_available)"

    # 清理
    vm "rm -f ${MNT}/t6_verify.txt ${MNT}/t6_md5.bin" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T6 long mount stability passed"
    return 0
}

# ============================================================
# T7: 卸载 + 最终检查 (与 T1 基线对比)
# ============================================================
test_t7_unmount() {
    section "T7: unmount + final check (vs T1 baseline)"

    local base
    base=$(dmesg_line_count)

    # T7-1: 清理测试文件
    echo "  [T7-1] cleanup test files..."
    vm "rm -f ${MNT}/stab_* ${MNT}/t2_dd_* ${MNT}/t3_* ${MNT}/t4_* ${MNT}/t5_* ${MNT}/t6_* 2>/dev/null" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "test files cleaned"

    # T7-2: umount
    echo "  [T7-2] umount powerfs..."
    local umount_ret
    umount_ret=$(vm "timeout 30 umount ${MNT} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -eq 0 ]; then
        ok "umount OK"
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
        ok "rmmod OK"
    else
        ng "rmmod failed: ${rmmod_ret}"
        echo "    check: lsmod | grep powerfs (may still have references)"
        return 1
    fi
    sleep 2

    # T7-4: 卸载后 dmesg 检查
    echo "  [T7-4] post-unmount dmesg check..."
    if check_dmesg_clean "$base"; then
        ok "post-unmount dmesg clean"
    else
        ng "post-unmount dmesg anomaly"
        return 1
    fi

    # T7-5: slab 全部释放
    echo "  [T7-5] slab release check..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab all released"
    else
        warn "powerfs slab still has remnants:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # T7-6: 内存恢复检查 (与 T1 基线对比)
    echo "  [T7-6] memory recovery check (vs T1 baseline)..."
    local mem_final
    mem_final=$(get_mem_available)
    echo "  -> final MemAvailable: ${mem_final} KB (T1 baseline: ${MEM_INIT:-N/A} KB)"
    record_trend "T7-final" "$(get_powerfs_slab_total)" "${mem_final}"

    if [ "${MEM_INIT:-0}" -gt 0 ]; then
        local diff=$(( ${MEM_INIT} - ${mem_final} ))
        if [ $diff -lt 50000 ]; then
            ok "memory recovered well (diff ${diff} KB, < 50MB)"
        else
            warn "memory diff large (${diff} KB, >= 50MB), potential leak"
        fi
    fi

    # T7-7: 重新挂载 (便于后续测试)
    echo "  [T7-7] remount powerfs (for subsequent tests)..."
    vm "mount -t powerfs none ${MNT}" 2>/dev/null
    sleep 2
    if check_mount; then
        ok "powerfs remounted at ${MNT}"
    else
        warn "powerfs remount failed (manual check needed)"
    fi

    # 输出趋势文件汇总
    echo ""
    echo "  [T7-trend] slab/mem trend (${TREND_FILE}):"
    if [ -f "${TREND_FILE}" ]; then
        sed 's/^/    /' "${TREND_FILE}"
    fi

    return 0
}

# ============================================================
# 主流程
# ============================================================
main() {
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  T6 Stability Test: sustained + leak + pressure      ${C_RESET}"
    echo -e "${C_CYAN}║  Principle: progressive 10m->30m->1h, kernel state   ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo "  Durations: T2=${T2_DURATION}s T3=${T3_DURATION}s T4=${T4_DURATION}s T5=${T5_DURATION}s T6=${T6_DURATION}s"
    echo "  fio: $([ "${HAS_FIO}" = "1" ] && echo "available" || echo "not set yet (checked in T1)")"
    echo "  Trend file: ${TREND_FILE}"

    # 测试列表 (顺序执行, 前一档失败则停止后续)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_seq_write"
        "3:test_t3_rand_rw"
        "4:test_t4_pressure"
        "5:test_t5_leak"
        "6:test_t6_long_mount"
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
    echo "  Trend file: ${TREND_FILE}"

    if [ "$FAIL" -gt 0 ]; then
        echo ""
        echo -e "  ${C_RED}✗ T6 stability test has failures${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  First failure: T${failed_test}"
        fi
        echo ""
        echo "  Troubleshooting:"
        echo "    1. VM dmesg:        ./qemuctl.sh log powerfs"
        echo "    2. Serial monitor:  ./qemuctl.sh serial-tail"
        echo "    3. Backend log:     ./qemuctl.sh service log filer-1"
        echo "    4. Slab/mem trend:  cat ${TREND_FILE}"
        echo "    5. Rerun single:    ./test_t6_stability.sh <T-num>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ T6 stability test all passed${C_RESET}"
    echo ""
    echo "  T6 verified:"
    echo "    - Compile + static verification"
    echo "    - QEMU mount + baseline recorded"
    echo "    - Sustained sequential write (${T2_DURATION}s)"
    echo "    - Sustained random read/write mix (${T3_DURATION}s)"
    echo "    - High concurrency pressure (${T4_DURATION}s)"
    echo "    - Memory leak detection (${T5_DURATION}s x2 cycles)"
    echo "    - Long mount stability (${T6_DURATION}s idle)"
    echo "    - Unmount + memory recovery"
    echo ""
    exit 0
}

main "$@"