#!/bin/bash
# 内核状态独立检查脚本 (任意时刻可运行)
#
# 用途: 在 K1 测试期间或测试后, 快速检查内核状态是否正常.
#       不执行任何 IO 操作, 只读取内核状态.
#
# 检查项:
#   1. dmesg 异常扫描 (oops/bug/kasan/stall/lockup/hung task)
#   2. powerfs slab 活跃对象数 (inode/dentry/chunk_map)
#   3. MemAvailable 内存状态
#   4. D 状态 (uninterruptible) powerfs 线程
#   5. 模块加载状态 + 引用计数
#   6. 挂载点状态
#   7. powerfs 相关内核线程
#
# 运行环境: HOST (通过 SSH 读取 VM 内核状态)
# 前置条件: QEMU 已启动, SSH 可达
#
# 用法:
#   ./test_k1_state_check.sh              # 单次检查
#   ./test_k1_state_check.sh --watch      # 持续监控 (每 5s 刷新)
#   ./test_k1_state_check.sh --watch 10   # 持续监控 (每 10s 刷新)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

# 颜色
if [ -t 1 ]; then
    C_RED='\033[0;31m'
    C_GREEN='\033[0;32m'
    C_YELLOW='\033[0;33m'
    C_CYAN='\033[0;36m'
    C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_RESET=''
fi

WATCH_MODE=false
WATCH_INTERVAL=5

# 解析参数
while [ $# -gt 0 ]; do
    case "$1" in
        --watch|-w)
            WATCH_MODE=true
            shift
            if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then
                WATCH_INTERVAL=$1
                shift
            fi
            ;;
        *) shift ;;
    esac
done

# ============================================================
# 检查函数
# ============================================================

# 0. serial 日志 lockup 检查 (VM 卡死时也能用, 直接读宿主机 qemu.log)
check_serial_lockup() {
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ ! -f "${qemu_log}" ]; then
        echo -e "  ${C_YELLOW}[N/A]${C_RESET} qemu.log 不存在 (${qemu_log})"
        return 0
    fi

    # lockup 相关关键词 (比 dmesg 检查更全面, 含 softlockup/hardlockup/nmi)
    local lockup_patterns=(
        'BUG:'
        'Oops:'
        'general protection fault'
        'unable to handle kernel'
        'NULL pointer dereference'
        'KASAN:'
        'RCU stall'
        'workqueue lockup'
        'hung task'
        'soft lockup'
        'hard lockup'
        'NMI watchdog'
        'call trace'
        'Kernel panic'
        'not tainted'
    )

    local found=""
    for pattern in "${lockup_patterns[@]}"; do
        local matches
        matches=$(grep -E "${pattern}" "${qemu_log}" 2>/dev/null | tail -5 || true)
        if [ -n "$matches" ]; then
            found="${found}${matches}\n"
        fi
    done

    if [ -z "$found" ]; then
        echo -e "  ${C_GREEN}[OK]${C_RESET} serial 日志无 lockup/oops 异常"
        return 0
    else
        echo -e "  ${C_RED}[ERR]${C_RESET} serial 日志检测到 lockup/oops:"
        echo -e "$found" | head -20 | sed 's/^/    /'
        return 1
    fi
}

# 1. dmesg 异常扫描
check_dmesg_errors() {
    local log
    log=$(vm "dmesg" 2>/dev/null)

    local error_patterns=(
        'BUG:'
        'Oops:'
        'general protection fault'
        'unable to handle kernel'
        'NULL pointer dereference'
        'KASAN:'
        'RCU stall'
        'workqueue lockup'
        'hung task'
        'soft lockup'
        'hard lockup'
        'NMI watchdog'
        'call trace'
        'Kernel panic'
        'Segmentation fault'
    )

    local found_errors=""
    for pattern in "${error_patterns[@]}"; do
        local matches
        matches=$(echo "$log" | grep -E "$pattern" | grep -i powerfs || true)
        if [ -n "$matches" ]; then
            found_errors="${found_errors}${matches}\n"
        fi
    done

    # 也检查不含 powerfs 的严重错误 (BUG/Oops/KASAN)
    local severe
    severe=$(echo "$log" | grep -E 'BUG:|Oops:|KASAN:' | tail -5 || true)

    if [ -z "$found_errors" ] && [ -z "$severe" ]; then
        echo -e "  ${C_GREEN}[OK]${C_RESET} dmesg 无异常"
        return 0
    else
        echo -e "  ${C_RED}[ERR]${C_RESET} dmesg 检测到异常:"
        if [ -n "$found_errors" ]; then
            echo -e "    powerfs 相关:"
            echo -e "$found_errors" | head -10 | sed 's/^/      /'
        fi
        if [ -n "$severe" ]; then
            echo -e "    其他严重错误:"
            echo "$severe" | head -5 | sed 's/^/      /'
        fi
        return 1
    fi
}

# 2. powerfs slab 检查
check_slab() {
    local slab_info
    slab_info=$(vm "cat /proc/slabinfo 2>/dev/null | grep -E 'powerfs'" 2>/dev/null || true)

    if [ -z "$slab_info" ]; then
        echo -e "  ${C_YELLOW}[N/A]${C_RESET} 无 powerfs slab (模块未加载或未挂载)"
        return 0
    fi

    echo -e "  ${C_CYAN}[INFO]${C_RESET} powerfs slab 状态:"
    echo "$slab_info" | while read -r name active objs size _; do
        local active_pct=0
        if [ "$objs" -gt 0 ] 2>/dev/null; then
            active_pct=$(( active * 100 / objs ))
        fi
        printf "    %-25s active=%-8s objs=%-8s size=%-6s (%d%%)\n" \
               "$name" "$active" "$objs" "$size" "$active_pct"
    done
    return 0
}

# 3. 内存状态
check_memory() {
    local meminfo
    meminfo=$(vm "grep -E 'MemAvailable|MemFree|Slab|SReclaimable|SUnreclaim' /proc/meminfo" 2>/dev/null)

    echo -e "  ${C_CYAN}[INFO]${C_RESET} 内存状态:"
    echo "$meminfo" | sed 's/^/    /'
    return 0
}

# 4. D 状态线程检查
check_d_state() {
    local d_tasks
    # 查找所有 D 状态进程
    d_tasks=$(vm "for p in /proc/[0-9]*/stat; do state=\$(awk '{print \$3}' \$p 2>/dev/null); if [ \"\$state\" = 'D' ]; then pid=\$(basename \$(dirname \$p)); comm=\$(cat /proc/\$pid/comm 2>/dev/null); echo \"\$pid \$comm\"; fi; done 2>/dev/null" 2>/dev/null || true)

    if [ -z "$d_tasks" ]; then
        echo -e "  ${C_GREEN}[OK]${C_RESET} 无 D 状态线程"
        return 0
    fi

    local powerfs_d
    powerfs_d=$(echo "$d_tasks" | grep -i powerfs || true)

    if [ -n "$powerfs_d" ]; then
        echo -e "  ${C_RED}[ERR]${C_RESET} powerfs D 状态线程:"
        echo "$powerfs_d" | sed 's/^/    /'
        return 1
    else
        local d_count
        d_count=$(echo "$d_tasks" | wc -l)
        echo -e "  ${C_YELLOW}[WARN]${C_RESET} 有 ${d_count} 个非 powerfs D 状态线程 (非阻断)"
        echo "$d_tasks" | head -3 | sed 's/^/    /'
        return 0
    fi
}

# 5. 模块状态
check_module() {
    local mod_info
    mod_info=$(vm "lsmod | grep powerfs" 2>/dev/null || true)

    if [ -z "$mod_info" ]; then
        echo -e "  ${C_YELLOW}[N/A]${C_RESET} powerfs 模块未加载"
        return 0
    fi

    echo -e "  ${C_GREEN}[OK]${C_RESET} powerfs 模块已加载: ${mod_info}"
    return 0
}

# 6. 挂载点状态
check_mount() {
    local mount_info
    mount_info=$(vm "mount | grep powerfs" 2>/dev/null || true)

    if [ -z "$mount_info" ]; then
        echo -e "  ${C_YELLOW}[N/A]${C_RESET} powerfs 未挂载"
        return 0
    fi

    echo -e "  ${C_GREEN}[OK]${C_RESET} powerfs 已挂载: ${mount_info}"

    # 检查挂载点可访问性
    local ls_ret
    ls_ret=$(vm "ls /mnt/pfs/ 2>&1 | head -5" 2>/dev/null)
    if [ $? -eq 0 ]; then
        echo -e "  ${C_GREEN}[OK]${C_RESET} 挂载点可访问"
    else
        echo -e "  ${C_RED}[ERR]${C_RESET} 挂载点不可访问: ${ls_ret}"
        return 1
    fi
    return 0
}

# 7. powerfs 内核线程
check_kernel_threads() {
    local threads
    threads=$(vm "ps aux 2>/dev/null | grep -E 'powerfs|pfs_' | grep -v grep" 2>/dev/null || true)

    if [ -z "$threads" ]; then
        echo -e "  ${C_YELLOW}[WARN]${C_RESET} 未发现 powerfs 内核线程"
        return 0
    fi

    echo -e "  ${C_CYAN}[INFO]${C_RESET} powerfs 内核线程:"
    echo "$threads" | awk '{printf "    %-8s %-8s %s\n", $2, $8, $11}' | head -10
    return 0
}

# 8. 网络连接状态 (powerfs 与 filer 的连接)
check_connections() {
    local conns
    conns=$(vm "ss -tn 2>/dev/null | grep -E '9334|9333|8080'" 2>/dev/null || true)

    if [ -z "$conns" ]; then
        echo -e "  ${C_YELLOW}[WARN]${C_RESET} 无到后端服务的 TCP 连接"
        return 0
    fi

    local conn_count
    conn_count=$(echo "$conns" | wc -l)
    echo -e "  ${C_GREEN}[OK]${C_RESET} 到后端服务 TCP 连接: ${conn_count} 条"
    echo "$conns" | head -5 | sed 's/^/    /'
    return 0
}

# ============================================================
# 单次检查
# ============================================================
run_check_once() {
    echo ""
    echo -e "${C_CYAN}━━━ 内核状态检查 ($(date '+%H:%M:%S')) ━━━${C_RESET}"

    # serial 日志检查总是执行 (VM 卡死时也能用)
    check_serial_lockup

    if ! vm_alive; then
        echo -e "  ${C_RED}[FATAL]${C_RESET} VM 不可达 (SSH 不通), 可能已 hang/panic"
        echo -e "  ${C_CYAN}[HINT]${C_RESET} 用 './qemuctl.sh serial' 查看完整 serial 日志"
        echo -e "  ${C_CYAN}[HINT]${C_RESET} 用 './qemuctl.sh serial-tail' 实时监控 serial 日志"
        return 1
    fi

    check_module
    check_mount
    check_dmesg_errors
    check_d_state
    check_slab
    check_memory
    check_kernel_threads
    check_connections

    return 0
}

# ============================================================
# 主流程
# ============================================================
echo ""
echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
echo -e "${C_CYAN}║  PowerFS 内核状态检查                                ${C_RESET}"
if [ "$WATCH_MODE" = true ]; then
    echo -e "${C_CYAN}║  模式: 持续监控 (每 ${WATCH_INTERVAL}s 刷新, Ctrl-C 退出)${C_RESET}"
fi
echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

if [ "$WATCH_MODE" = true ]; then
    while true; do
        clear 2>/dev/null || true
        run_check_once
        echo ""
        echo -e "  ${C_CYAN}下次刷新: ${WATCH_INTERVAL}s 后 (Ctrl-C 退出)${C_RESET}"
        sleep "$WATCH_INTERVAL"
    done
else
    run_check_once
    echo ""
    echo -e "${C_CYAN}━━━ 检查完成 ━━━${C_RESET}"
fi
