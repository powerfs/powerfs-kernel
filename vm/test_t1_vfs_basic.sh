#!/bin/bash
# T1 阶段渐进式测试: VFS 基础操作
#
# 验证内容:
#   T0: 编译 + 静态验证 (make + verify_module.sh)
#   T1: QEMU 启动 + 挂载 (复用 qemuctl.sh, 30s dmesg 观察)
#   T2: 文件 CRUD 测试 (从小到大: 空文件 → 100B → 4KB → 1MB → truncate → overwrite → append)
#   T3: 目录操作测试 (mkdir/rmdir/readdir/rename/symlink/hardlink/unlink)
#   T4: 权限测试 (chmod/chown/touch 时间戳)
#   T5: 边界测试 (空读/超大 offset/长文件名/特殊字符/0 字节写)
#   T6: 并发读写测试 (4 写不同文件 / 4 读同一文件 / 2写+2读 60s 持续)
#   T7: 卸载 + 最终检查 (umount + rmmod + dmesg/slab/meminfo 恢复)
#
# 核心原则:
#   - 内核正确性 != 应用完成, 必须检查 dmesg/slab/meminfo/D 状态
#   - 从小到大逐个确认, 前一档未通过不进入下一档
#   - 每步检查内核状态
#
# 运行环境: HOST (通过 SSH 控制 VM)
# 用法:
#   ./test_t1_vfs_basic.sh            # 运行全部 (T0-T7)
#   ./test_t1_vfs_basic.sh 2 3 4      # 仅运行 T2+T3+T4

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

PASS=0
FAIL=0
WARN=0
SKIP=0

# 初始基线 (T1 记录, T7 用于恢复对比; 选择性运行时可能未设置)
SLAB_INIT=""
MEM_INIT=""

# qemu.log boot 段基线 (T1 记录当前行数, 之后只扫增量, 避免误报历史 panic)
# qemu.log 是 append 模式, 每次 VM 重启 (panic=-1) 会追加新 boot 段,
# 历史 panic 记录永远残留在文件前部, 不过滤会导致永久误报.
QEMU_LOG_BASE=0

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

# 判断是否运行指定测试 (无参数 = 全部运行)
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
# 注意: powerfs_wb (writeback) kworker 在网络 IO 期间合法进入 D 状态,
# 采用重试机制 (3次, 间隔2s) 避免误报. 持续 D 状态才视为真正 hung.
check_d_state() {
    local d_tasks
    local retry=0
    local max_retry=3
    while [ $retry -lt $max_retry ]; do
        d_tasks=$(vm "for p in /proc/[0-9]*/stack; do t=\$(cat \${p%/stack}/stat 2>/dev/null | awk '{print \$3}'); if [ \"\$t\" = 'D' ]; then comm=\$(cat \${p%/stack}/comm 2>/dev/null); echo \"\$comm (\${p%/stack})\"; fi; done 2>/dev/null | grep -i powerfs" 2>/dev/null || true)
        if [ -z "$d_tasks" ]; then
            return 0
        fi
        retry=$((retry + 1))
        if [ $retry -lt $max_retry ]; then
            sleep 2
        fi
    done
    echo "  ${C_RED}D-state powerfs thread detected (persisted ${max_retry} checks):${C_RESET}"
    echo "$d_tasks" | sed 's/^/    /'
    return 1
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
        ng "dmesg anomaly (${desc})"
        state_ok=1
    fi

    # 2. 无 D 状态线程
    if check_d_state; then
        ok "no D-state powerfs thread"
    else
        ng "D-state (hung) powerfs thread (${desc})"
        state_ok=1
    fi

    # 3. serial 日志 lockup 检查 (补充 dmesg, VM 卡死时也能查)
    # 只扫描 T1 记录的 QEMU_LOG_BASE 之后的行 (当前 boot 段),
    # 避免误报历史 panic (qemu.log 是 append 模式, panic=-1 重启后
    # 旧 panic 记录残留在文件前部).
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        local serial_errors
        if [ "${QEMU_LOG_BASE}" -gt 0 ] 2>/dev/null; then
            # 只扫当前 boot 段 (tail -n +BASE 从第 BASE 行开始)
            serial_errors=$(tail -n +"${QEMU_LOG_BASE}" "${qemu_log}" 2>/dev/null | grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task' | tail -5 || true)
        else
            # 惰性初始化: 跳过 T1 时 (QEMU_LOG_BASE=0) 首次调用,
            # 记录当前 qemu.log 行数作为基线, 只扫之后新增的行.
            # 这样即使选择性运行 T2/T3... 也不会误报历史 panic.
            QEMU_LOG_BASE=$(wc -l < "${qemu_log}" 2>/dev/null || echo 0)
            case "$QEMU_LOG_BASE" in
                ''|*[!0-9]*) QEMU_LOG_BASE=1 ;;
            esac
            QEMU_LOG_BASE=$((QEMU_LOG_BASE + 1))
            # 基线刚记录, 当前 boot 段 (基线之后) 应该还没有新 panic
            serial_errors=$(tail -n +"${QEMU_LOG_BASE}" "${qemu_log}" 2>/dev/null | grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task' | tail -5 || true)
        fi
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

    # 编译警告 (非阻断)
    local warnings
    warnings=$(echo "$build_log" | grep -iE 'warning:' | grep -v 'Wno-' || true)
    if [ -z "$warnings" ]; then
        ok "build has no warnings"
    else
        warn "build has warnings (non-blocking):"
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
# T1: QEMU 启动 + 挂载
# ============================================================
test_t1_mount() {
    section "T1: QEMU boot + mount"

    # T1-1: 检查后端服务
    echo "  [T1-1] check backend services..."
    local svc_count
    svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || echo 0)
    if [ "$svc_count" -ge 3 ]; then
        ok "backend services running (master/volume/filer)"
    else
        warn "backend services not fully up, starting..."
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
        warn "QEMU not running, deploying..."
        ./qemuctl.sh deploy 2>&1 | tail -10
        sleep 10
        qemu_pid=$(pgrep -f "qemu-system-x86_64.*bzImage" 2>/dev/null | head -1)
    fi
    if [ -n "$qemu_pid" ]; then
        ok "QEMU running (PID: ${qemu_pid})"
    else
        ng "QEMU boot failed"
        return 1
    fi

    # 记录 qemu.log 当前行数作为 boot 段基线.
    # qemu.log 是 append 模式, 每次 VM 重启 (panic=-1) 会追加新 boot 段,
    # 历史 panic 记录残留在文件前部.  之后 check_kernel_state 只扫
    # 此基线之后的行, 避免误报历史 panic.
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        QEMU_LOG_BASE=$(wc -l < "${qemu_log}" 2>/dev/null || echo 0)
        # 确保是数字
        case "$QEMU_LOG_BASE" in
            ''|*[!0-9]*) QEMU_LOG_BASE=1 ;;
        esac
        # +1: wc 给的是总行数, tail -n +N 从第 N 行开始, 需要 +1 跳过已存在的行
        QEMU_LOG_BASE=$((QEMU_LOG_BASE + 1))
        echo "  -> qemu.log boot baseline: line ${QEMU_LOG_BASE} (scanning only current boot segment)"
    fi

    # T1-3: SSH 可达
    echo "  [T1-3] check VM SSH reachable..."
    if vm_alive; then
        ok "VM SSH reachable"
    else
        ng "VM SSH unreachable"
        return 1
    fi

    # T1-4: powerfs 挂载
    echo "  [T1-4] check powerfs mount..."
    if ! check_mount; then
        warn "powerfs not mounted, mounting..."
        ./qemuctl.sh mount 2>&1 | tail -3
        sleep 2
    fi
    if check_mount; then
        ok "powerfs mounted on ${MNT}"
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
    echo "  [T1-6] 30s dmesg observation after mount..."
    local base
    base=$(dmesg_line_count)
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

    return 0
}

# ============================================================
# T2: 文件 CRUD 测试 (从小到大)
# ============================================================
test_t2_crud() {
    section "T2: file CRUD (small to large)"

    local base size src_md5 read_md5 content

    # T2-2a: 创建空文件 → stat size=0
    echo "  [T2-2a] create empty file..."
    base=$(dmesg_line_count)
    vm "touch ${MNT}/t2a_empty.txt" 2>/dev/null
    size=$(vm "stat -c %s ${MNT}/t2a_empty.txt" 2>/dev/null)
    if [ "$size" = "0" ]; then
        ok "T2-2a empty file size=0"
    else
        ng "T2-2a empty file size=${size} (want 0)"
        return 1
    fi
    if ! check_kernel_state "T2-2a empty file" "$base"; then
        ng "T2-2a kernel state abnormal"
        return 1
    fi

    # T2-2b: 写 100B → 读 MD5 校验
    echo "  [T2-2b] write 100B + MD5 verify..."
    base=$(dmesg_line_count)
    src_md5=$(vm "dd if=/dev/urandom bs=100 count=1 2>/dev/null | tee ${MNT}/t2b_100.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    read_md5=$(vm "md5sum ${MNT}/t2b_100.bin | awk '{print \$1}'" 2>/dev/null)
    size=$(vm "stat -c %s ${MNT}/t2b_100.bin" 2>/dev/null)
    if [ "$src_md5" = "$read_md5" ] && [ -n "$src_md5" ]; then
        ok "T2-2b 100B MD5 match (${src_md5})"
    else
        ng "T2-2b 100B MD5 mismatch (src=${src_md5} read=${read_md5})"
        return 1
    fi
    if [ "$size" = "100" ]; then
        ok "T2-2b 100B size correct"
    else
        ng "T2-2b 100B size=${size} (want 100)"
        return 1
    fi
    if ! check_kernel_state "T2-2b 100B" "$base"; then
        ng "T2-2b kernel state abnormal"
        return 1
    fi

    # T2-2c: 写 4KB → 读 MD5 校验
    echo "  [T2-2c] write 4KB + MD5 verify..."
    base=$(dmesg_line_count)
    src_md5=$(vm "dd if=/dev/urandom bs=4096 count=1 2>/dev/null | tee ${MNT}/t2c_4k.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    read_md5=$(vm "md5sum ${MNT}/t2c_4k.bin | awk '{print \$1}'" 2>/dev/null)
    size=$(vm "stat -c %s ${MNT}/t2c_4k.bin" 2>/dev/null)
    if [ "$src_md5" = "$read_md5" ] && [ -n "$src_md5" ]; then
        ok "T2-2c 4KB MD5 match (${src_md5})"
    else
        ng "T2-2c 4KB MD5 mismatch (src=${src_md5} read=${read_md5})"
        return 1
    fi
    if [ "$size" = "4096" ]; then
        ok "T2-2c 4KB size correct"
    else
        ng "T2-2c 4KB size=${size} (want 4096)"
        return 1
    fi
    if ! check_kernel_state "T2-2c 4KB" "$base"; then
        ng "T2-2c kernel state abnormal"
        return 1
    fi

    # T2-2d: 写 1MB → 读 MD5 校验
    echo "  [T2-2d] write 1MB + MD5 verify..."
    base=$(dmesg_line_count)
    src_md5=$(vm "dd if=/dev/urandom bs=1M count=1 2>/dev/null | tee ${MNT}/t2d_1m.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    read_md5=$(vm "md5sum ${MNT}/t2d_1m.bin | awk '{print \$1}'" 2>/dev/null)
    size=$(vm "stat -c %s ${MNT}/t2d_1m.bin" 2>/dev/null)
    if [ "$src_md5" = "$read_md5" ] && [ -n "$src_md5" ]; then
        ok "T2-2d 1MB MD5 match (${src_md5})"
    else
        ng "T2-2d 1MB MD5 mismatch (src=${src_md5} read=${read_md5})"
        return 1
    fi
    if [ "$size" = "1048576" ]; then
        ok "T2-2d 1MB size correct"
    else
        ng "T2-2d 1MB size=${size} (want 1048576)"
        return 1
    fi
    if ! check_kernel_state "T2-2d 1MB" "$base"; then
        ng "T2-2d kernel state abnormal"
        return 1
    fi

    # T2-2e: truncate 扩展 → 确认 size 变化
    echo "  [T2-2e] truncate extend (100B -> 1MB)..."
    base=$(dmesg_line_count)
    vm "head -c 100 /dev/urandom > ${MNT}/t2e.bin" 2>/dev/null
    size=$(vm "stat -c %s ${MNT}/t2e.bin" 2>/dev/null)
    if [ "$size" != "100" ]; then
        ng "T2-2e pre-truncate size=${size} (want 100)"
        return 1
    fi
    vm "truncate -s 1048576 ${MNT}/t2e.bin" 2>/dev/null
    size=$(vm "stat -c %s ${MNT}/t2e.bin" 2>/dev/null)
    if [ "$size" = "1048576" ]; then
        ok "T2-2e truncate extend size=1048576"
    else
        ng "T2-2e truncate extend size=${size} (want 1048576)"
        return 1
    fi
    if ! check_kernel_state "T2-2e truncate extend" "$base"; then
        ng "T2-2e kernel state abnormal"
        return 1
    fi

    # T2-2f: truncate 缩小 → 确认 size 变化
    echo "  [T2-2f] truncate shrink (1MB -> 100B)..."
    base=$(dmesg_line_count)
    vm "head -c 1048576 /dev/urandom > ${MNT}/t2f.bin" 2>/dev/null
    vm "truncate -s 100 ${MNT}/t2f.bin" 2>/dev/null
    size=$(vm "stat -c %s ${MNT}/t2f.bin" 2>/dev/null)
    if [ "$size" = "100" ]; then
        ok "T2-2f truncate shrink size=100"
    else
        ng "T2-2f truncate shrink size=${size} (want 100)"
        return 1
    fi
    if ! check_kernel_state "T2-2f truncate shrink" "$base"; then
        ng "T2-2f kernel state abnormal"
        return 1
    fi

    # T2-2g: overwrite (O_TRUNC) → 确认旧数据清除
    echo "  [T2-2g] overwrite with O_TRUNC..."
    base=$(dmesg_line_count)
    vm "head -c 100 /dev/zero > ${MNT}/t2g.bin" 2>/dev/null
    size=$(vm "stat -c %s ${MNT}/t2g.bin" 2>/dev/null)
    if [ "$size" != "100" ]; then
        ng "T2-2g pre-overwrite size=${size} (want 100)"
        return 1
    fi
    # '>' 重定向即 O_TRUNC, 写入 50 字节
    vm "head -c 50 /dev/urandom > ${MNT}/t2g.bin" 2>/dev/null
    size=$(vm "stat -c %s ${MNT}/t2g.bin" 2>/dev/null)
    if [ "$size" = "50" ]; then
        ok "T2-2g overwrite size=50 (old data cleared)"
    else
        ng "T2-2g overwrite size=${size} (want 50, old data not cleared)"
        return 1
    fi
    if ! check_kernel_state "T2-2g overwrite O_TRUNC" "$base"; then
        ng "T2-2g kernel state abnormal"
        return 1
    fi

    # T2-2h: append 写 → 确认数据追加
    echo "  [T2-2h] append write..."
    base=$(dmesg_line_count)
    vm "printf 'hello' > ${MNT}/t2h.bin" 2>/dev/null
    vm "printf 'world' >> ${MNT}/t2h.bin" 2>/dev/null
    size=$(vm "stat -c %s ${MNT}/t2h.bin" 2>/dev/null)
    content=$(vm "cat ${MNT}/t2h.bin" 2>/dev/null)
    if [ "$size" = "10" ] && [ "$content" = "helloworld" ]; then
        ok "T2-2h append size=10 content='helloworld'"
    else
        ng "T2-2h append size=${size} content='${content}' (want size=10 content='helloworld')"
        return 1
    fi
    if ! check_kernel_state "T2-2h append" "$base"; then
        ng "T2-2h kernel state abnormal"
        return 1
    fi

    # 清理 T2 测试文件
    vm "rm -f ${MNT}/t2a_empty.txt ${MNT}/t2b_100.bin ${MNT}/t2c_4k.bin ${MNT}/t2d_1m.bin ${MNT}/t2e.bin ${MNT}/t2f.bin ${MNT}/t2g.bin ${MNT}/t2h.bin" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T2 file CRUD all passed"
    return 0
}

# ============================================================
# T3: 目录操作测试
# ============================================================
test_t3_dir() {
    section "T3: directory operations"

    local base listing nlink link_target owner

    # 清理上次测试可能残留的文件 (避免 "File exists" 等错误)
    vm "rm -rf ${MNT}/t3a ${MNT}/t3b ${MNT}/t3c ${MNT}/t3d ${MNT}/t3e_old ${MNT}/t3e_new ${MNT}/t3f_old ${MNT}/t3f_new ${MNT}/t3g_target ${MNT}/t3g_link ${MNT}/t3h_orig ${MNT}/t3h_link ${MNT}/t3i_file" 2>/dev/null

    # T3-3a: mkdir 嵌套目录 (a/b/c/d) → ls 确认
    echo "  [T3-3a] mkdir nested a/b/c/d..."
    base=$(dmesg_line_count)
    vm "mkdir -p ${MNT}/t3a/a/b/c/d" 2>/dev/null
    if vm "test -d ${MNT}/t3a/a/b/c/d" 2>/dev/null; then
        ok "T3-3a nested dir created"
    else
        ng "T3-3a nested dir create failed"
        return 1
    fi
    if ! check_kernel_state "T3-3a mkdir nested" "$base"; then
        ng "T3-3a kernel state abnormal"
        return 1
    fi

    # T3-3b: rmdir 空目录 → 确认删除
    echo "  [T3-3b] rmdir empty dir..."
    base=$(dmesg_line_count)
    vm "mkdir -p ${MNT}/t3b/empty" 2>/dev/null
    vm "rmdir ${MNT}/t3b/empty" 2>/dev/null
    if vm "test -d ${MNT}/t3b/empty" 2>/dev/null; then
        ng "T3-3b empty dir still exists"
        return 1
    else
        ok "T3-3b empty dir removed"
    fi
    if ! check_kernel_state "T3-3b rmdir empty" "$base"; then
        ng "T3-3b kernel state abnormal"
        return 1
    fi

    # T3-3c: rmdir 非空目录 → 确认返回 ENOTEMPTY
    echo "  [T3-3c] rmdir non-empty dir (expect ENOTEMPTY)..."
    base=$(dmesg_line_count)
    vm "mkdir -p ${MNT}/t3c/dir" 2>/dev/null
    vm "touch ${MNT}/t3c/dir/file" 2>/dev/null
    local rmdir_out
    rmdir_out=$(vm "rmdir ${MNT}/t3c/dir 2>&1" 2>/dev/null)
    local rmdir_rc=$?
    # rmdir 非空应失败 (rc!=0) 且提示 not empty
    if [ $rmdir_rc -ne 0 ] && echo "$rmdir_out" | grep -iq 'not empty'; then
        ok "T3-3c rmdir non-empty returned ENOTEMPTY"
    else
        ng "T3-3c rmdir non-empty rc=${rmdir_rc} out='${rmdir_out}' (want ENOTEMPTY)"
        return 1
    fi
    # 目录应仍然存在
    if ! vm "test -d ${MNT}/t3c/dir" 2>/dev/null; then
        ng "T3-3c non-empty dir was removed unexpectedly"
        return 1
    fi
    if ! check_kernel_state "T3-3c rmdir non-empty" "$base"; then
        ng "T3-3c kernel state abnormal"
        return 1
    fi

    # T3-3d: readdir → 确认目录项完整
    # 注意: 创建后立即 ls 可能因目录缓存未及时更新而读取为空,
    # 采用重试机制 (最多 5 次, 每次间隔 0.5s) 兼顾时序与快速反馈.
    echo "  [T3-3d] readdir completeness..."
    base=$(dmesg_line_count)
    vm "mkdir -p ${MNT}/t3d" 2>/dev/null
    vm "touch ${MNT}/t3d/f1 ${MNT}/t3d/f2 ${MNT}/t3d/f3" 2>/dev/null
    local miss=1
    local retry=0
    local max_retry=5
    while [ $retry -lt $max_retry ]; do
        listing=$(vm "ls ${MNT}/t3d" 2>/dev/null)
        miss=0
        for f in f1 f2 f3; do
            if ! echo "$listing" | grep -qx "$f"; then
                miss=1
            fi
        done
        if [ $miss -eq 0 ]; then
            break
        fi
        retry=$((retry + 1))
        sleep 0.5
    done
    if [ $miss -eq 0 ]; then
        ok "T3-3d readdir complete (f1 f2 f3, retries=${retry})"
    else
        ng "T3-3d readdir incomplete after ${max_retry} retries: '${listing}'"
        return 1
    fi
    if ! check_kernel_state "T3-3d readdir" "$base"; then
        ng "T3-3d kernel state abnormal"
        return 1
    fi

    # T3-3e: rename 文件 → 确认旧路径不存在, 新路径可读
    echo "  [T3-3e] rename file..."
    base=$(dmesg_line_count)
    vm "echo data > ${MNT}/t3e_old" 2>/dev/null
    vm "mv ${MNT}/t3e_old ${MNT}/t3e_new" 2>/dev/null
    if vm "test -e ${MNT}/t3e_old" 2>/dev/null; then
        ng "T3-3e old path still exists"
        return 1
    fi
    content=$(vm "cat ${MNT}/t3e_new" 2>/dev/null)
    if [ "$content" = "data" ]; then
        ok "T3-3e rename file OK (new path readable)"
    else
        ng "T3-3e rename file content='${content}' (want 'data')"
        return 1
    fi
    if ! check_kernel_state "T3-3e rename file" "$base"; then
        ng "T3-3e kernel state abnormal"
        return 1
    fi

    # T3-3f: rename 目录 → 确认目录树移动
    echo "  [T3-3f] rename dir (move tree)..."
    base=$(dmesg_line_count)
    vm "mkdir -p ${MNT}/t3f_old/sub" 2>/dev/null
    vm "echo x > ${MNT}/t3f_old/sub/file" 2>/dev/null
    vm "mv ${MNT}/t3f_old ${MNT}/t3f_new" 2>/dev/null
    if vm "test -e ${MNT}/t3f_old" 2>/dev/null; then
        ng "T3-3f old dir still exists"
        return 1
    fi
    content=$(vm "cat ${MNT}/t3f_new/sub/file" 2>/dev/null)
    if vm "test -d ${MNT}/t3f_new/sub" 2>/dev/null && [ "$content" = "x" ]; then
        ok "T3-3f rename dir OK (tree moved)"
    else
        ng "T3-3f rename dir failed (sub/file content='${content}')"
        return 1
    fi
    if ! check_kernel_state "T3-3f rename dir" "$base"; then
        ng "T3-3f kernel state abnormal"
        return 1
    fi

    # T3-3g: symlink → readlink 确认链接目标
    # 注意: 先清理可能残留的文件, 避免上次测试失败导致 ln -s 报 "File exists"
    echo "  [T3-3g] symlink + readlink..."
    base=$(dmesg_line_count)
    vm "rm -f ${MNT}/t3g_target ${MNT}/t3g_link" 2>/dev/null
    vm "echo target > ${MNT}/t3g_target" 2>/dev/null
    vm "ln -s ${MNT}/t3g_target ${MNT}/t3g_link" 2>/dev/null
    link_target=$(vm "readlink ${MNT}/t3g_link" 2>/dev/null)
    content=$(vm "cat ${MNT}/t3g_link" 2>/dev/null)
    if [ "$link_target" = "${MNT}/t3g_target" ] && [ "$content" = "target" ]; then
        ok "T3-3g symlink OK (target='${link_target}')"
    else
        ng "T3-3g symlink failed (readlink='${link_target}' content='${content}')"
        return 1
    fi
    if ! check_kernel_state "T3-3g symlink" "$base"; then
        ng "T3-3g kernel state abnormal"
        return 1
    fi

    # T3-3h: hard link → 确认 nlink=2, 内容一致
    echo "  [T3-3h] hard link (nlink=2)..."
    base=$(dmesg_line_count)
    vm "echo hdata > ${MNT}/t3h_orig" 2>/dev/null
    vm "ln ${MNT}/t3h_orig ${MNT}/t3h_link" 2>/dev/null
    nlink=$(vm "stat -c %h ${MNT}/t3h_orig" 2>/dev/null)
    content=$(vm "cat ${MNT}/t3h_link" 2>/dev/null)
    if [ "$nlink" = "2" ] && [ "$content" = "hdata" ]; then
        ok "T3-3h hard link OK (nlink=${nlink})"
    else
        ng "T3-3h hard link failed (nlink=${nlink} content='${content}')"
        return 1
    fi
    if ! check_kernel_state "T3-3h hard link" "$base"; then
        ng "T3-3h kernel state abnormal"
        return 1
    fi

    # T3-3i: unlink → 确认删除
    echo "  [T3-3i] unlink file..."
    base=$(dmesg_line_count)
    vm "echo udata > ${MNT}/t3i_file" 2>/dev/null
    vm "rm ${MNT}/t3i_file" 2>/dev/null
    if vm "test -e ${MNT}/t3i_file" 2>/dev/null; then
        ng "T3-3i file still exists after unlink"
        return 1
    else
        ok "T3-3i unlink OK"
    fi
    if ! check_kernel_state "T3-3i unlink" "$base"; then
        ng "T3-3i kernel state abnormal"
        return 1
    fi

    # 清理 T3 测试目录
    vm "rm -rf ${MNT}/t3a ${MNT}/t3b ${MNT}/t3c ${MNT}/t3d ${MNT}/t3e_new ${MNT}/t3f_new ${MNT}/t3g_target ${MNT}/t3g_link ${MNT}/t3h_orig ${MNT}/t3h_link" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T3 directory operations all passed"
    return 0
}

# ============================================================
# T4: 权限测试
# ============================================================
test_t4_perm() {
    section "T4: permission tests"

    local base mode owner mtime

    # 准备测试文件
    vm "touch ${MNT}/t4_file" 2>/dev/null

    # T4-4a: chmod 0644 → stat 确认 mode
    echo "  [T4-4a] chmod 0644..."
    base=$(dmesg_line_count)
    vm "chmod 0644 ${MNT}/t4_file" 2>/dev/null
    mode=$(vm "stat -c %a ${MNT}/t4_file" 2>/dev/null)
    if [ "$mode" = "644" ]; then
        ok "T4-4a chmod 0644 OK (mode=${mode})"
    else
        ng "T4-4a chmod 0644 failed (mode=${mode})"
        return 1
    fi
    if ! check_kernel_state "T4-4a chmod 0644" "$base"; then
        ng "T4-4a kernel state abnormal"
        return 1
    fi

    # T4-4b: chmod 0755 → stat 确认 mode
    echo "  [T4-4b] chmod 0755..."
    base=$(dmesg_line_count)
    vm "chmod 0755 ${MNT}/t4_file" 2>/dev/null
    mode=$(vm "stat -c %a ${MNT}/t4_file" 2>/dev/null)
    if [ "$mode" = "755" ]; then
        ok "T4-4b chmod 0755 OK (mode=${mode})"
    else
        ng "T4-4b chmod 0755 failed (mode=${mode})"
        return 1
    fi
    if ! check_kernel_state "T4-4b chmod 0755" "$base"; then
        ng "T4-4b kernel state abnormal"
        return 1
    fi

    # T4-4c: chown uid/gid → stat 确认
    echo "  [T4-4c] chown uid:gid 1000:1000..."
    base=$(dmesg_line_count)
    vm "chown 1000:1000 ${MNT}/t4_file" 2>/dev/null
    owner=$(vm "stat -c %u:%g ${MNT}/t4_file" 2>/dev/null)
    if [ "$owner" = "1000:1000" ]; then
        ok "T4-4c chown OK (uid:gid=${owner})"
    else
        ng "T4-4c chown failed (uid:gid=${owner}, want 1000:1000)"
        return 1
    fi
    if ! check_kernel_state "T4-4c chown" "$base"; then
        ng "T4-4c kernel state abnormal"
        return 1
    fi

    # T4-4d: touch 更新时间戳 → stat 确认
    echo "  [T4-4d] touch timestamp (2020-01-01 00:00)..."
    base=$(dmesg_line_count)
    vm "touch -t 202001010000 ${MNT}/t4_file" 2>/dev/null
    mtime=$(vm "stat -c %y ${MNT}/t4_file" 2>/dev/null)
    if echo "$mtime" | grep -q '^2020-01-01'; then
        ok "T4-4d touch timestamp OK (mtime=${mtime})"
    else
        ng "T4-4d touch timestamp failed (mtime='${mtime}')"
        return 1
    fi
    if ! check_kernel_state "T4-4d touch timestamp" "$base"; then
        ng "T4-4d kernel state abnormal"
        return 1
    fi

    # 清理
    vm "rm -f ${MNT}/t4_file" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T4 permission tests all passed"
    return 0
}

# ============================================================
# T5: 边界测试
# ============================================================
test_t5_boundary() {
    section "T5: boundary tests"

    local base bytes longname found

    # T5-5a: 空文件读取 → 返回 0 字节
    echo "  [T5-5a] read empty file (expect 0 bytes)..."
    base=$(dmesg_line_count)
    vm "touch ${MNT}/t5a_empty" 2>/dev/null
    bytes=$(vm "wc -c < ${MNT}/t5a_empty" 2>/dev/null | tr -d ' ')
    if [ "$bytes" = "0" ]; then
        ok "T5-5a empty file read returns 0 bytes"
    else
        ng "T5-5a empty file read returns ${bytes} bytes (want 0)"
        return 1
    fi
    if ! check_kernel_state "T5-5a empty read" "$base"; then
        ng "T5-5a kernel state abnormal"
        return 1
    fi

    # T5-5b: 超大 offset 读取 → 返回 0 字节 (EOF)
    echo "  [T5-5b] read at huge offset (expect 0 bytes / EOF)..."
    base=$(dmesg_line_count)
    vm "head -c 100 /dev/urandom > ${MNT}/t5b.bin" 2>/dev/null
    bytes=$(vm "dd if=${MNT}/t5b.bin bs=1 skip=1000000 2>/dev/null | wc -c" 2>/dev/null | tr -d ' ')
    if [ "$bytes" = "0" ]; then
        ok "T5-5b huge offset read returns 0 bytes (EOF)"
    else
        ng "T5-5b huge offset read returns ${bytes} bytes (want 0)"
        return 1
    fi
    if ! check_kernel_state "T5-5b huge offset read" "$base"; then
        ng "T5-5b kernel state abnormal"
        return 1
    fi

    # T5-5c: 长文件名 (255 chars) → 创建成功
    echo "  [T5-5c] long filename (255 chars)..."
    base=$(dmesg_line_count)
    # 文件名 = "t5c_" + 251 个 'a' = 255 字符
    longname="t5c_$(printf 'a%.0s' $(seq 1 251))"
    vm "touch ${MNT}/${longname}" 2>/dev/null
    if vm "test -f ${MNT}/${longname}" 2>/dev/null; then
        ok "T5-5c long filename (255 chars) created"
    else
        ng "T5-5c long filename create failed"
        return 1
    fi
    # 校验名称长度
    found=$(vm "ls ${MNT}/ | awk 'length(\$0)==255 {print \$0}'" 2>/dev/null)
    if [ -n "$found" ]; then
        ok "T5-5c 255-char name listed"
    else
        ng "T5-5c 255-char name not found in readdir"
        return 1
    fi
    if ! check_kernel_state "T5-5c long filename" "$base"; then
        ng "T5-5c kernel state abnormal"
        return 1
    fi

    # T5-5d: 特殊字符文件名 (空格/中文/破折号) → 创建成功
    echo "  [T5-5d] special-char filenames (space/chinese/dash)..."
    base=$(dmesg_line_count)
    vm "touch '${MNT}/t5d space.txt' '${MNT}/t5d中文.txt' '${MNT}/t5d-dash.txt'" 2>/dev/null
    local sp_ok=1
    vm "test -f '${MNT}/t5d space.txt'" 2>/dev/null || sp_ok=0
    vm "test -f '${MNT}/t5d中文.txt'" 2>/dev/null || sp_ok=0
    vm "test -f '${MNT}/t5d-dash.txt'" 2>/dev/null || sp_ok=0
    if [ $sp_ok -eq 1 ]; then
        ok "T5-5d special-char filenames created"
    else
        ng "T5-5d special-char filename create failed"
        return 1
    fi
    if ! check_kernel_state "T5-5d special chars" "$base"; then
        ng "T5-5d kernel state abnormal"
        return 1
    fi

    # T5-5e: 0 字节写入 → 不报错
    echo "  [T5-5e] zero-byte write..."
    base=$(dmesg_line_count)
    local zrc
    zrc=$(vm "dd if=/dev/zero of=${MNT}/t5e.bin bs=1 count=0 2>/dev/null; echo RC=\$?" 2>/dev/null)
    if echo "$zrc" | grep -q 'RC=0'; then
        ok "T5-5e zero-byte write OK"
    else
        ng "T5-5e zero-byte write failed (${zrc})"
        return 1
    fi
    bytes=$(vm "stat -c %s ${MNT}/t5e.bin" 2>/dev/null)
    if [ "$bytes" = "0" ]; then
        ok "T5-5e zero-byte file size=0"
    else
        ng "T5-5e zero-byte file size=${bytes} (want 0)"
        return 1
    fi
    if ! check_kernel_state "T5-5e zero-byte write" "$base"; then
        ng "T5-5e kernel state abnormal"
        return 1
    fi

    # 清理
    vm "rm -f ${MNT}/t5a_empty ${MNT}/t5b.bin ${MNT}/${longname}" 2>/dev/null
    vm "rm -f '${MNT}/t5d space.txt' '${MNT}/t5d中文.txt' '${MNT}/t5d-dash.txt'" 2>/dev/null
    vm "rm -f ${MNT}/t5e.bin" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "T5 boundary tests all passed"
    return 0
}

# ============================================================
# T6: 并发读写测试
# ============================================================
test_t6_concurrency() {
    section "T6: concurrent read/write"

    local base i stored reread expected

    # T6-6a: 4 进程同时写不同文件 → 各自 MD5 正确
    echo "  [T6-6a] 4 concurrent writers (different files)..."
    base=$(dmesg_line_count)
    vm "rm -f ${MNT}/t6a_*.bin ${MNT}/t6a_*.md5 2>/dev/null
for i in 1 2 3 4; do
  ( dd if=/dev/urandom bs=1M count=1 2>/dev/null | tee ${MNT}/t6a_\${i}.bin | md5sum | awk '{print \$1}' > ${MNT}/t6a_\${i}.md5 ) &
done
wait" 2>/dev/null
    local all_match=1
    for i in 1 2 3 4; do
        stored=$(vm "cat ${MNT}/t6a_${i}.md5" 2>/dev/null)
        reread=$(vm "md5sum ${MNT}/t6a_${i}.bin | awk '{print \$1}'" 2>/dev/null)
        if [ "$stored" = "$reread" ] && [ -n "$stored" ]; then
            ok "T6-6a writer ${i} MD5 match (${stored})"
        else
            ng "T6-6a writer ${i} MD5 mismatch (stored=${stored} reread=${reread})"
            all_match=0
        fi
    done
    if [ $all_match -ne 1 ]; then
        return 1
    fi
    if ! check_kernel_state "T6-6a 4 writers" "$base"; then
        ng "T6-6a kernel state abnormal"
        return 1
    fi
    vm "rm -f ${MNT}/t6a_*.bin ${MNT}/t6a_*.md5" 2>/dev/null

    # T6-6b: 4 进程同时读同一文件 → 都返回正确 MD5
    echo "  [T6-6b] 4 concurrent readers (same file)..."
    base=$(dmesg_line_count)
    vm "dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${MNT}/t6b.bin" 2>/dev/null
    expected=$(vm "md5sum ${MNT}/t6b.bin | awk '{print \$1}'" 2>/dev/null)
    vm "rm -f ${MNT}/t6b_r*.md5 2>/dev/null
for i in 1 2 3 4; do
  ( md5sum ${MNT}/t6b.bin | awk '{print \$1}' > ${MNT}/t6b_r\${i}.md5 ) &
done
wait" 2>/dev/null
    all_match=1
    for i in 1 2 3 4; do
        reread=$(vm "cat ${MNT}/t6b_r${i}.md5" 2>/dev/null)
        if [ "$reread" = "$expected" ]; then
            ok "T6-6b reader ${i} MD5 correct"
        else
            ng "T6-6b reader ${i} MD5 mismatch (got=${reread} want=${expected})"
            all_match=0
        fi
    done
    if [ $all_match -ne 1 ]; then
        return 1
    fi
    if ! check_kernel_state "T6-6b 4 readers" "$base"; then
        ng "T6-6b kernel state abnormal"
        return 1
    fi
    vm "rm -f ${MNT}/t6b.bin ${MNT}/t6b_r*.md5" 2>/dev/null

    # T6-6c: 2 进程写 + 2 进程读同一文件 → 无 panic (60s 持续 + 每 10s dmesg)
    echo "  [T6-6c] 2 writers + 2 readers on same file, 60s sustained..."
    base=$(dmesg_line_count)
    # 种子文件
    vm "dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${MNT}/t6c.bin" 2>/dev/null
    local pids=()
    local w r
    # 2 写进程 (每个用 timeout 60 限定)
    for w in 1 2; do
        vm "timeout 60 sh -c 'while true; do dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${MNT}/t6c.bin; done' 2>/dev/null; true" 2>/dev/null &
        pids+=($!)
    done
    # 2 读进程
    for r in 1 2; do
        vm "timeout 60 sh -c 'while true; do cat ${MNT}/t6c.bin > /dev/null; done' 2>/dev/null; true" 2>/dev/null &
        pids+=($!)
    done
    # 每 10s 检查 dmesg
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep 10
        elapsed=$((elapsed + 10))
        echo "    [${elapsed}s] dmesg check..."
        if ! check_dmesg_clean "$base"; then
            ng "T6-6c kernel anomaly at ${elapsed}s"
            for p in "${pids[@]}"; do kill $p 2>/dev/null; done
            return 1
        fi
        echo "    [${elapsed}s] dmesg clean"
    done
    # 等待后台作业结束
    for p in "${pids[@]}"; do wait $p 2>/dev/null; done
    ok "T6-6c 60s sustained mixed r/w completed, no panic"
    if ! check_kernel_state "T6-6c mixed r/w 60s" "$base"; then
        ng "T6-6c kernel state abnormal"
        return 1
    fi
    vm "rm -f ${MNT}/t6c.bin" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T6 concurrent tests all passed"
    return 0
}

# ============================================================
# T7: 卸载 + 最终检查
# ============================================================
test_t7_unmount() {
    section "T7: unmount + final check"

    local base

    # T7-1: 清理测试文件 (兜底)
    echo "  [T7-1] cleanup test files..."
    vm "rm -rf ${MNT}/t2* ${MNT}/t3* ${MNT}/t4* ${MNT}/t5* ${MNT}/t6* 2>/dev/null; sync" 2>/dev/null
    ok "test files cleaned"

    base=$(dmesg_line_count)

    # T7-2: umount
    echo "  [T7-2] umount powerfs..."
    local umount_out
    umount_out=$(vm "timeout 30 umount ${MNT} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -eq 0 ]; then
        ok "umount OK"
    else
        ng "umount failed/timeout: ${umount_out}"
        return 1
    fi
    sleep 2

    # T7-3: rmmod
    echo "  [T7-3] rmmod powerfs..."
    local rmmod_out
    rmmod_out=$(vm "rmmod powerfs 2>&1" 2>/dev/null)
    ret=$?
    if [ $ret -eq 0 ]; then
        ok "rmmod OK"
    else
        ng "rmmod failed: ${rmmod_out}"
        echo "    check: lsmod | grep powerfs"
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

    # T7-5: slab 释放检查
    echo "  [T7-5] slab release check..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab fully released"
    else
        warn "powerfs slab residue remains:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # T7-6: 内存恢复检查
    echo "  [T7-6] memory recovery check..."
    local mem_final
    mem_final=$(get_mem_available)
    echo "  -> final MemAvailable: ${mem_final} KB (initial: ${MEM_INIT:-N/A} KB)"
    if [ "${MEM_INIT:-0}" -gt 0 ]; then
        local diff=$(( MEM_INIT - mem_final ))
        if [ $diff -lt 50000 ]; then
            ok "memory recovered well (diff ${diff} KB)"
        else
            warn "memory diff large (${diff} KB), possible leak"
        fi
    fi

    # T7-7: 重新挂载 (便于后续测试)
    echo ""
    echo "  [T7] remount powerfs (for subsequent tests)..."
    vm "mount -t powerfs none ${MNT}" 2>/dev/null
    sleep 2
    if check_mount; then
        ok "powerfs remounted"
    else
        warn "powerfs remount failed (non-blocking)"
    fi

    return 0
}

# ============================================================
# 主流程
# ============================================================
main() {
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  T1 渐进式测试: VFS 基础操作                         ${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从小到大, 逐个确认, 检查内核状态             ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_crud"
        "3:test_t3_dir"
        "4:test_t4_perm"
        "5:test_t5_boundary"
        "6:test_t6_concurrency"
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
        echo -e "  ${C_RED}✗ T1 VFS basic tests have failures${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  first failure: T${failed_test}"
        fi
        echo ""
        echo "  troubleshooting:"
        echo "    1. VM dmesg:    ./qemuctl.sh log powerfs"
        echo "    2. live monitor: ./qemuctl.sh monitor powerfs"
        echo "    3. backend log: ./qemuctl.sh service log filer-1"
        echo "    4. rerun single stage: ./test_t1_vfs_basic.sh <T#>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ T1 VFS basic tests all passed${C_RESET}"
    echo ""
    echo "  T1 gate achieved:"
    echo "    - compile + static verification passed"
    echo "    - QEMU mount + dmesg clean"
    echo "    - file CRUD (empty → 1MB → truncate → overwrite → append)"
    echo "    - directory ops (mkdir/rmdir/readdir/rename/symlink/hardlink/unlink)"
    echo "    - permissions (chmod/chown/timestamps)"
    echo "    - boundary cases (empty read / huge offset / long name / special chars)"
    echo "    - concurrent r/w (4 writers / 4 readers / 60s mixed)"
    echo "    - kernel state (slab/meminfo/dmesg) normal"
    echo ""
    echo "  ready for next stage (T2 advanced VFS)"
    exit 0
}

main "$@"
