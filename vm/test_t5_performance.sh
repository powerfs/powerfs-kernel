#!/bin/bash
# T5 阶段性能测试: 顺序/随机/Stripe/多线程/元数据 基准
#
# 测试内容:
#   T0: 编译 + 静态验证 (make + verify_module.sh)
#   T1: QEMU 启动 + 挂载 (复用 qemuctl.sh) + 检查 fio/mdtest
#   T2: 顺序读写基准 (1K/4K/1M/1G block)
#   T3: 随机读写基准 (4K/64K + 70/30 混合)
#   T4: Stripe vs Flat 性能对比
#   T5: 多线程并发 IO (4/8/16 jobs)
#   T6: 元数据性能 (mdtest 或 touch/stat/rm)
#   T7: 性能报告生成 + 清理 + umount + rmmod
#
# 核心原则:
#   - 内核正确性 != 应用完成, 必须检查 dmesg/slab/meminfo/D 状态
#   - 从小到大逐个确认, 每步检查内核状态
#   - fio 测试用标准命令, 不可用脚本替代
#   - 无 fio 时 skip + warn, T6 元数据用 touch/stat 替代
#
# 运行环境: HOST (通过 SSH 控制 VM + docker exec 控制 FUSE 容器)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - 或由本脚本 T1 自动完成
#
# 用法:
#   ./test_t5_performance.sh            # 运行全部 (T0-T7)
#   ./test_t5_performance.sh 2          # 仅运行 T2
#   ./test_t5_performance.sh 2 4 6      # 运行 T2+T4+T6

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs           # FUSE 容器内挂载点 (fuse-1)
FUSE_CONTAINER="fuse-1"         # FUSE 容器名 (docker-compose-single.yml)
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

# 结果记录文件 (host 侧)
RESULTS_FILE="/tmp/t5_results.txt"
REPORT_FILE="${MNT}/../t5_perf_report.txt"

PASS=0
FAIL=0
WARN=0
SKIP=0
SLAB_INIT=""
MEM_INIT=""
SERIAL_BASE=0
HAS_FIO=0
HAS_MDTEST=0

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
        echo "  ${C_RED}检测到内核异常:${C_RESET}"
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
        echo "  ${C_RED}检测到 D 状态 powerfs 线程:${C_RESET}"
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

    echo "  --- 内核状态检查: ${desc} ---"

    # 1. dmesg 无异常
    if check_dmesg_clean "$base"; then
        ok "dmesg 无 oops/bug/kasan/stall"
    else
        ng "dmesg 检测到异常 (${desc})"
        state_ok=1
    fi

    # 2. 无 D 状态线程
    if check_d_state; then
        ok "无 D 状态 powerfs 线程"
    else
        ng "存在 D 状态 (hung) powerfs 线程 (${desc})"
        state_ok=1
    fi

    # 3. serial 日志 lockup 检查 (补充 dmesg, VM 卡死时也能查)
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        local serial_errors
        serial_errors=$(serial_since "${SERIAL_BASE}" | grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task' 2>/dev/null | tail -5 || true)
        if [ -z "$serial_errors" ]; then
            ok "serial 日志无 lockup/panic"
        else
            ng "serial 日志检测到 lockup/panic (${desc})"
            echo "$serial_errors" | sed 's/^/      /'
            state_ok=1
        fi
    fi

    return $state_ok
}

# ============================================================
# fio 结果解析 + 记录
# ============================================================

# 从 fio 输出中解析指定操作 (read/write) 的 BW/IOPS/latency
# 参数 $1: fio 完整输出, $2: 操作 (read|write)
# 输出: "BW IOPS lat_us" (BW 含单位如 124MiB)
parse_fio_op() {
    local out="$1"
    local op="$2"
    local line bw iops lat_val lat_unit lat_us

    # per-job 摘要行: "  write: IOPS=124, BW=124MiB/s (130MB/s)(...)"
    line=$(echo "$out" | grep -E "^[[:space:]]+${op}:[[:space:]]+IOPS=" | head -1)
    bw=$(echo "$line" | grep -oE '(BW|bw)=[0-9.]+(KiB|MiB|GiB)/s' | head -1 | sed 's/^[Bb][Ww]=//;s|/s||')
    iops=$(echo "$line" | grep -oE 'IOPS=[0-9]+' | head -1 | sed 's/IOPS=//')
    # 兜底: 从 Run status 行 "124IOPS" 解析
    if [ -z "$iops" ]; then
        iops=$(echo "$out" | grep -oE '[0-9]+IOPS' | head -1 | sed 's/IOPS//')
    fi

    # 完成延迟 clat: "    clat (usec): min=50, max=8000, avg=120.5, stdev=50.0"
    # 取该 op 行之后最近的 clat 行 (mix 场景下 read/write 各有 clat)
    local op_line_no clat_line
    op_line_no=$(echo "$out" | grep -nE "^[[:space:]]+${op}:[[:space:]]+IOPS=" | head -1 | cut -d: -f1)
    if [ -n "$op_line_no" ]; then
        clat_line=$(echo "$out" | awk -v n="$op_line_no" 'NR>n && /clat \((usec|msec)\)/{print; exit}')
    else
        clat_line=$(echo "$out" | grep -E "clat \((usec|msec)\)" | head -1)
    fi
    lat_unit=$(echo "$clat_line" | grep -oE 'usec|msec')
    lat_val=$(echo "$clat_line" | grep -oE 'avg=[0-9.]+' | head -1 | sed 's/avg=//')

    if [ -n "$lat_val" ] && [ -n "$lat_unit" ] && [ "$lat_val" != "0" ]; then
        if [ "$lat_unit" = "msec" ]; then
            lat_us=$(awk "BEGIN{printf \"%.1f\", ${lat_val}*1000}")
        else
            lat_us=$(awk "BEGIN{printf \"%.1f\", ${lat_val}}")
        fi
    elif [ -n "$iops" ]; then
        if [ "$iops" -gt 0 ] 2>/dev/null; then
            lat_us=$(awk "BEGIN{printf \"%.1f\", 1000000.0/${iops}}")
        else
            lat_us="N/A"
        fi
    else
        lat_us="N/A"
    fi

    echo "${bw:-N/A} ${iops:-N/A} ${lat_us}"
}

# 记录一行结果到 RESULTS_FILE 并打印
record_line() {
    echo "$1" >> "${RESULTS_FILE}"
    echo "    -> $1"
}

# 运行 fio 测试 + 解析 + 记录 + 内核状态检查
# 参数: $1=label $2=name $3=parse_op(read|write|mix) $4=directory $5=rw $6..=extra
run_fio_test() {
    local label="$1" name="$2" parse_op="$3" directory="$4" rw="$5"
    shift 5
    local extra="$*"

    echo "  [${label}] fio ${name} (rw=${rw} ${extra})..."

    # 清理可能残留的同名文件
    vm "rm -f ${directory}/${name}.* 2>/dev/null" 2>/dev/null

    local base
    base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local fio_out
    fio_out=$(vm "fio --name=${name} --directory=${directory} --rw=${rw} ${extra} --direct=0 --ioengine=psync --group_reporting --runtime=30 --time_based 2>&1" 2>/dev/null)

    # 显示关键摘要行
    echo "$fio_out" | grep -E '(WRITE|READ):.*(bw=|IOPS=)' | tail -4 | sed 's/^/    /'

    if [ "$parse_op" = "mix" ]; then
        local pr pw prb pri prl pwb pwi pwl
        pr=$(parse_fio_op "$fio_out" "read")
        pw=$(parse_fio_op "$fio_out" "write")
        prb=$(echo "$pr" | awk '{print $1}')
        pri=$(echo "$pr" | awk '{print $2}')
        prl=$(echo "$pr" | awk '{print $3}')
        pwb=$(echo "$pw" | awk '{print $1}')
        pwi=$(echo "$pw" | awk '{print $2}')
        pwl=$(echo "$pw" | awk '{print $3}')
        record_line "${label} ${name} (read)  BW=${prb} IOPS=${pri} lat=${prl}us"
        record_line "${label} ${name} (write) BW=${pwb} IOPS=${pwi} lat=${pwl}us"
    else
        local p pb pi pl
        p=$(parse_fio_op "$fio_out" "$parse_op")
        pb=$(echo "$p" | awk '{print $1}')
        pi=$(echo "$p" | awk '{print $2}')
        pl=$(echo "$p" | awk '{print $3}')
        record_line "${label} ${name} BW=${pb} IOPS=${pi} lat=${pl}us"
    fi

    # 清理测试文件
    vm "rm -f ${directory}/${name}.* 2>/dev/null; sync" 2>/dev/null

    if check_kernel_state "${label} ${name}" "$base"; then
        ok "${label} 内核状态正常"
        return 0
    else
        ng "${label} 内核状态异常"
        return 1
    fi
}

# ============================================================
# T0: 编译 + 静态验证
# ============================================================
test_t0_compile() {
    section "T0: 编译 + 静态验证"

    # T0-1: 编译
    echo "  [T0-1] 编译 powerfs.ko..."
    cd "${POWERFS_MOD_DIR}"
    local build_log
    build_log=$(make clean 2>&1 && make -j$(nproc) 2>&1)
    local build_ret=$?

    if [ $build_ret -eq 0 ] && [ -f powerfs.ko ]; then
        ok "powerfs.ko 编译成功 ($(ls -la powerfs.ko | awk '{print $5}') bytes)"
    else
        ng "powerfs.ko 编译失败"
        echo "$build_log" | tail -20 | sed 's/^/    /'
        cd "${SCRIPT_DIR}"
        return 1
    fi

    # 编译警告检查 (非阻断)
    local warnings
    warnings=$(echo "$build_log" | grep -iE 'warning:' | grep -v 'Wno-' || true)
    if [ -z "$warnings" ]; then
        ok "编译无 warning"
    else
        warn "编译有 warning (非阻断):"
        echo "$warnings" | head -5 | sed 's/^/    /'
    fi

    # T0-2: 静态符号验证
    echo "  [T0-2] 静态符号验证 (verify_module.sh)..."
    if bash verify_module.sh 2>&1 | tail -5 | grep -q "ALL VERIFICATION TESTS PASSED"; then
        ok "verify_module.sh 全部通过"
    else
        ng "verify_module.sh 有失败项"
        bash verify_module.sh 2>&1 | grep '\[FAIL\]' | sed 's/^/    /'
        cd "${SCRIPT_DIR}"
        return 1
    fi

    cd "${SCRIPT_DIR}"
    return 0
}

# ============================================================
# T1: QEMU 启动 + 挂载 + fio/mdtest 检查
# ============================================================
test_t1_mount() {
    section "T1: QEMU 启动 + 挂载 + 工具检查"

    # T1-1: 检查 Docker 服务
    echo "  [T1-1] 检查后端服务..."
    local svc_count
    svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || echo 0)
    if [ "$svc_count" -ge 3 ]; then
        ok "后端服务已运行 (master/volume/filer)"
    else
        warn "后端服务未完全启动, 尝试启动..."
        ./qemuctl.sh service start 2>&1 | tail -5
        sleep 5
        svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || echo 0)
        if [ "$svc_count" -ge 3 ]; then
            ok "后端服务已启动"
        else
            ng "后端服务启动失败"
            return 1
        fi
    fi

    # T1-2: 检查 QEMU 运行
    echo "  [T1-2] 检查 QEMU 运行状态..."
    local qemu_pid
    qemu_pid=$(pgrep -f "qemu-system-x86_64.*bzImage" 2>/dev/null | head -1)
    if [ -z "$qemu_pid" ]; then
        warn "QEMU 未运行, 启动中..."
        ./qemuctl.sh deploy 2>&1 | tail -10
        sleep 10
        qemu_pid=$(pgrep -f "qemu-system-x86_64.*bzImage" 2>/dev/null | head -1)
    fi
    if [ -n "$qemu_pid" ]; then
        ok "QEMU 运行中 (PID: ${qemu_pid})"
    else
        ng "QEMU 启动失败"
        return 1
    fi

    # T1-3: SSH 可达
    echo "  [T1-3] 检查 VM SSH 可达..."
    if vm_alive; then
        ok "VM SSH 可达"
    else
        ng "VM SSH 不可达"
        return 1
    fi

    # T1-4: powerfs 已挂载
    echo "  [T1-4] 检查 powerfs 挂载..."
    if ! check_mount; then
        warn "powerfs 未挂载, 尝试挂载..."
        ./qemuctl.sh mount 2>&1 | tail -3
        sleep 2
    fi
    if check_mount; then
        ok "powerfs 已挂载到 ${MNT}"
    else
        ng "powerfs 挂载失败"
        return 1
    fi

    # T1-5: 模块引用计数
    echo "  [T1-5] 检查模块引用计数..."
    local mod_info
    mod_info=$(vm "lsmod | grep powerfs" 2>/dev/null)
    if [ -n "$mod_info" ]; then
        ok "powerfs 模块已加载: ${mod_info}"
    else
        ng "powerfs 模块未加载"
        return 1
    fi

    # T1-6: 挂载后稳定观察 + 内核状态
    echo "  [T1-6] 挂载后 10s 稳定观察..."
    local base
    base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)
    sleep 10
    if check_kernel_state "挂载后 10s" "$base"; then
        ok "挂载后内核状态正常"
    else
        ng "挂载后内核状态异常"
        return 1
    fi

    # 记录初始 slab 和 meminfo
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "  -> 初始 slab (inode dentry): ${SLAB_INIT}"
    echo "  -> 初始 MemAvailable: ${MEM_INIT} KB"

    # T1-7: 检查 VM 内 fio
    echo "  [T1-7] 检查 VM 内 fio..."
    local has_fio
    has_fio=$(vm "which fio 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has_fio" = "no" ]; then
        warn "VM 内无 fio, 尝试安装..."
        vm "apt-get install -y fio >/dev/null 2>&1 || apk add fio >/dev/null 2>&1 || echo install_fail" 2>/dev/null | tail -1
        has_fio=$(vm "which fio 2>/dev/null || echo no" 2>/dev/null)
    fi
    if [ "$has_fio" = "no" ]; then
        warn "VM 内 fio 不可用, fio 基准测试 (T2/T3/T4/T5) 将跳过"
        HAS_FIO=0
    else
        ok "VM 内 fio 可用: ${has_fio}"
        HAS_FIO=1
    fi

    # T1-8: 检查 VM 内 mdtest
    echo "  [T1-8] 检查 VM 内 mdtest..."
    local has_mdtest
    has_mdtest=$(vm "which mdtest 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has_mdtest" = "no" ]; then
        warn "VM 内无 mdtest, T6 元数据测试将用 touch/stat/rm 替代"
        HAS_MDTEST=0
    else
        ok "VM 内 mdtest 可用: ${has_mdtest}"
        HAS_MDTEST=1
    fi

    return 0
}

# ============================================================
# T2: 顺序读写基准 (1K/4K/1M/1G block)
# ============================================================
test_t2_seq() {
    section "T2: 顺序读写基准 (1K/4K/1M/1G)"

    if [ "${HAS_FIO}" != "1" ]; then
        warn "VM 内无 fio, 跳过 T2 顺序读写基准"
        skip "T2 (无 fio)"
        return 0
    fi

    # 2a/2b: 1KB block
    run_fio_test "T2a" "seq_write_1k" "write" "${MNT}" "write" "--bs=1k --size=100M --end_fsync=1" || return 1
    run_fio_test "T2b" "seq_read_1k"  "read"  "${MNT}" "read"  "--bs=1k --size=100M" || return 1
    # 2c/2d: 4KB block
    run_fio_test "T2c" "seq_write_4k" "write" "${MNT}" "write" "--bs=4k --size=100M --end_fsync=1" || return 1
    run_fio_test "T2d" "seq_read_4k"  "read"  "${MNT}" "read"  "--bs=4k --size=100M" || return 1
    # 2e/2f: 1MB block
    run_fio_test "T2e" "seq_write_1m" "write" "${MNT}" "write" "--bs=1m --size=100M --end_fsync=1" || return 1
    run_fio_test "T2f" "seq_read_1m"  "read"  "${MNT}" "read"  "--bs=1m --size=100M" || return 1
    # 2g/2h: 1GB 大文件
    run_fio_test "T2g" "seq_write_1g" "write" "${MNT}" "write" "--bs=1m --size=1G --end_fsync=1" || return 1
    run_fio_test "T2h" "seq_read_1g"  "read"  "${MNT}" "read"  "--bs=1m --size=1G" || return 1

    ok "T2 顺序读写基准完成"
    return 0
}

# ============================================================
# T3: 随机读写基准 (4K/64K + 70/30 混合)
# ============================================================
test_t3_rand() {
    section "T3: 随机读写基准 (4K/64K + 70/30 混合)"

    if [ "${HAS_FIO}" != "1" ]; then
        warn "VM 内无 fio, 跳过 T3 随机读写基准"
        skip "T3 (无 fio)"
        return 0
    fi

    run_fio_test "T3a" "rand_write_4k"  "write" "${MNT}" "randwrite" "--bs=4k --size=100M --end_fsync=1" || return 1
    run_fio_test "T3b" "rand_read_4k"   "read"  "${MNT}" "randread"  "--bs=4k --size=100M" || return 1
    run_fio_test "T3c" "rand_write_64k" "write" "${MNT}" "randwrite" "--bs=64k --size=100M --end_fsync=1" || return 1
    run_fio_test "T3d" "rand_read_64k"  "read"  "${MNT}" "randread"  "--bs=64k --size=100M" || return 1
    run_fio_test "T3e" "rand_rw_mix"    "mix"   "${MNT}" "randrw"    "--bs=4k --size=100M --rwmixread=70" || return 1

    ok "T3 随机读写基准完成"
    return 0
}

# ============================================================
# T4: Stripe vs Flat 性能对比
# ============================================================
test_t4_stripe_vs_flat() {
    section "T4: Stripe vs Flat 性能对比"

    if [ "${HAS_FIO}" != "1" ]; then
        warn "VM 内无 fio, 跳过 T4"
        skip "T4 (无 fio)"
        return 0
    fi

    local flat_dir="${MNT}/t4_flat"
    local stripe_kernel_dir="${MNT}/t4_stripe"
    local stripe_fuse_dir="${FUSE_MNT}/t4_stripe"

    # 创建 Flat 目录 (内核端)
    vm "mkdir -p ${flat_dir}" 2>/dev/null

    # 创建 Stripe 目录 + xattr (FUSE 端, stripe:4:1MB)
    local stripe_ok=0
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${stripe_fuse_dir} && mkdir -p ${stripe_fuse_dir}" 2>/dev/null
        if docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.placement -v 'stripe:4:1MB' ${stripe_fuse_dir}" 2>/dev/null; then
            ok "Stripe 目录 xattr 设置成功 (stripe:4:1MB)"
            stripe_ok=1
            # 内核端 lookup, 触发元数据同步
            vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
            sleep 1
        else
            warn "Stripe xattr 设置失败, 仅测试 Flat (T4c/T4d 跳过)"
        fi
    else
        warn "${FUSE_CONTAINER} 容器未运行, 仅测试 Flat (T4c/T4d 跳过)"
    fi

    # 4a/4b: Flat 模式
    run_fio_test "T4a" "flat_write" "write" "${flat_dir}" "write" "--bs=1m --size=100M --end_fsync=1" || return 1
    run_fio_test "T4b" "flat_read"  "read"  "${flat_dir}" "read"  "--bs=1m --size=100M" || return 1

    # 4c/4d: Stripe 模式
    if [ "$stripe_ok" = "1" ]; then
        run_fio_test "T4c" "stripe_write" "write" "${stripe_kernel_dir}" "write" "--bs=1m --size=100M --end_fsync=1" || return 1
        run_fio_test "T4d" "stripe_read"  "read"  "${stripe_kernel_dir}" "read"  "--bs=1m --size=100M" || return 1
    else
        skip "T4c/T4d (Stripe 目录不可用)"
    fi

    # 4e: Flat vs Stripe 对比 (信息性, 数据见报告)
    echo "  [T4e] Flat vs Stripe 性能对比:"
    if [ "$stripe_ok" = "1" ]; then
        ok "Flat 与 Stripe 数据均已采集, 对比见报告 ${REPORT_FILE}"
    else
        warn "Stripe 数据缺失, 仅 Flat 数据, 对比见报告"
    fi

    # 清理
    vm "rm -rf ${flat_dir}" 2>/dev/null
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${stripe_fuse_dir}" 2>/dev/null
    vm "sync" 2>/dev/null
    return 0
}

# ============================================================
# T5: 多线程并发 IO (4/8/16 jobs)
# ============================================================
test_t5_multithread() {
    section "T5: 多线程并发 IO (4/8/16 jobs)"

    if [ "${HAS_FIO}" != "1" ]; then
        warn "VM 内无 fio, 跳过 T5"
        skip "T5 (无 fio)"
        return 0
    fi

    # 5a/5b: 4 线程顺序读写 (聚合 BW/IOPS)
    run_fio_test "T5a" "mt_seq_write_4j" "write" "${MNT}" "write" "--bs=1m --size=50M --numjobs=4 --end_fsync=1" || return 1
    run_fio_test "T5b" "mt_seq_read_4j"  "read"  "${MNT}" "read"  "--bs=1m --size=50M --numjobs=4" || return 1
    # 5c: 8 线程随机读
    run_fio_test "T5c" "mt_rand_read_8j" "read"  "${MNT}" "randread" "--bs=4k --size=50M --numjobs=8" || return 1
    # 5d: 16 线程随机读写混合
    run_fio_test "T5d" "mt_rand_rw_16j"  "mix"   "${MNT}" "randrw"  "--bs=4k --size=50M --numjobs=16 --rwmixread=70" || return 1

    ok "T5 多线程并发 IO 完成 (聚合 BW/IOPS 已记录)"
    return 0
}

# ============================================================
# T6: 元数据性能 (mdtest 或 touch/stat/rm 替代)
# ============================================================

# 文件元数据: touch/stat/rm 各 100 次 (无 mdtest 时替代)
meta_test_files() {
    local n=100
    echo "  [T6c] touch/stat/rm ${n} 文件..."
    local start end elapsed total_ops ops_sec
    start=$(date +%s%N)
    vm "for i in \$(seq 1 ${n}); do touch ${MNT}/t6_meta_f\${i}; done" 2>/dev/null
    vm "for i in \$(seq 1 ${n}); do stat ${MNT}/t6_meta_f\${i} >/dev/null 2>&1; done" 2>/dev/null
    vm "for i in \$(seq 1 ${n}); do rm ${MNT}/t6_meta_f\${i}; done" 2>/dev/null
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))
    [ "$elapsed" -le 0 ] && elapsed=1
    total_ops=$((n * 3))
    ops_sec=$(awk "BEGIN{printf \"%.1f\", ${total_ops}*1000.0/${elapsed}}")
    record_line "T6c meta_files_${n} create+stat+rm total=${elapsed}ms ops/s=${ops_sec}"
    ok "T6c 文件元数据: ${elapsed}ms, ${ops_sec} ops/s"
}

# 目录元数据: mkdir/rmdir 各 100 次 (无 mdtest 时替代)
meta_test_dirs() {
    local n=100
    echo "  [T6d] mkdir/rmdir ${n} 目录..."
    local start end elapsed total_ops ops_sec
    start=$(date +%s%N)
    vm "for i in \$(seq 1 ${n}); do mkdir ${MNT}/t6_meta_d\${i}; done" 2>/dev/null
    vm "for i in \$(seq 1 ${n}); do rmdir ${MNT}/t6_meta_d\${i}; done" 2>/dev/null
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))
    [ "$elapsed" -le 0 ] && elapsed=1
    total_ops=$((n * 2))
    ops_sec=$(awk "BEGIN{printf \"%.1f\", ${total_ops}*1000.0/${elapsed}}")
    record_line "T6d meta_dirs_${n} mkdir+rmdir total=${elapsed}ms ops/s=${ops_sec}"
    ok "T6d 目录元数据: ${elapsed}ms, ${ops_sec} ops/s"
}

test_t6_metadata() {
    section "T6: 元数据性能"

    local base
    base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    if [ "${HAS_MDTEST}" = "1" ]; then
        # 6a: mdtest 文件 create/stat/delete
        echo "  [T6a] mdtest -n 1000 -F (文件 create/stat/delete)..."
        vm "mkdir -p ${MNT}/t6_mdtest_f" 2>/dev/null
        local out6a
        out6a=$(vm "cd ${MNT}/t6_mdtest_f && mdtest -n 1000 -F 2>&1" 2>/dev/null)
        echo "$out6a" | grep -iE 'creation|stat|removal|SUMMARY' | head -10 | sed 's/^/    /'
        record_line "T6a mdtest_files (-n 1000 -F):"
        echo "$out6a" | grep -iE 'File (creation|stat|removal)' | while read -r l; do
            record_line "  $l"
        done
        vm "rm -rf ${MNT}/t6_mdtest_f" 2>/dev/null

        # 6b: mdtest 目录 create/delete
        echo "  [T6b] mdtest -n 100 -T (目录 create/delete)..."
        vm "mkdir -p ${MNT}/t6_mdtest_d" 2>/dev/null
        local out6b
        out6b=$(vm "cd ${MNT}/t6_mdtest_d && mdtest -n 100 -T 2>&1" 2>/dev/null)
        echo "$out6b" | grep -iE 'creation|removal|SUMMARY|directory' | head -10 | sed 's/^/    /'
        record_line "T6b mdtest_dirs (-n 100 -T):"
        echo "$out6b" | grep -iE 'Directory (creation|stat|removal)' | while read -r l; do
            record_line "  $l"
        done
        vm "rm -rf ${MNT}/t6_mdtest_d" 2>/dev/null
    else
        # 6c/6d: 无 mdtest, 用 touch/stat/rm + mkdir/rmdir 替代
        warn "VM 内无 mdtest, 使用 touch/stat/rm + mkdir/rmdir 替代"
        meta_test_files
        meta_test_dirs
    fi

    if check_kernel_state "T6 元数据" "$base"; then
        ok "T6 内核状态正常"
        return 0
    else
        ng "T6 内核状态异常"
        return 1
    fi
}

# ============================================================
# T7: 性能报告生成 + 清理 + umount + rmmod
# ============================================================
test_t7_report_cleanup() {
    section "T7: 性能报告生成 + 清理"

    local base
    base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T7-1: 生成性能报告
    echo "  [T7-1] 生成性能报告 ${REPORT_FILE}..."
    mkdir -p "$(dirname "${REPORT_FILE}")" 2>/dev/null
    {
        echo "============================================"
        echo " PowerFS T5 性能测试报告"
        echo " 生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo " 挂载点: ${MNT}"
        echo "============================================"
        echo ""
        echo "[全部测试结果]"
        if [ -f "${RESULTS_FILE}" ]; then
            cat "${RESULTS_FILE}"
        else
            echo "  (无结果数据)"
        fi
        echo ""
        echo "[Flat vs Stripe 对比]"
        grep -E '^T4[abcd]' "${RESULTS_FILE}" 2>/dev/null | sed 's/^/  /' || true
        echo ""
        echo "[多线程聚合]"
        grep -E '^T5[abcd]' "${RESULTS_FILE}" 2>/dev/null | sed 's/^/  /' || true
        echo ""
        echo "[元数据性能]"
        grep -E '^T6[abcd]' "${RESULTS_FILE}" 2>/dev/null | sed 's/^/  /' || true
        echo ""
        echo "============================================"
        echo "说明: BW=带宽 IOPS=每秒IO数 lat=平均完成延迟(us)"
        echo "      fio 参数: --direct=0 --ioengine=psync --group_reporting --runtime=30 --time_based"
        echo "============================================"
    } > "${REPORT_FILE}"
    ok "报告已生成: ${REPORT_FILE}"
    echo "    --- 报告预览 ---"
    sed 's/^/    /' "${REPORT_FILE}"

    # T7-2: 清理测试文件
    echo "  [T7-2] 清理测试文件..."
    vm "rm -rf ${MNT}/t4_flat ${MNT}/t4_stripe ${MNT}/t6_mdtest_f ${MNT}/t6_mdtest_d ${MNT}/t6_meta_* ${MNT}/seq_* ${MNT}/rand_* ${MNT}/mt_* 2>/dev/null" 2>/dev/null
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${FUSE_MNT}/t4_stripe 2>/dev/null" 2>/dev/null
    vm "sync" 2>/dev/null
    ok "测试文件已清理"

    # T7-3: umount
    echo "  [T7-3] umount powerfs..."
    local umount_ret
    umount_ret=$(vm "timeout 30 umount ${MNT} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -eq 0 ]; then
        ok "umount 成功"
    else
        ng "umount 失败/超时: ${umount_ret}"
        return 1
    fi
    sleep 2

    # T7-4: rmmod
    echo "  [T7-4] rmmod powerfs..."
    local rmmod_ret
    rmmod_ret=$(vm "rmmod powerfs 2>&1" 2>/dev/null)
    ret=$?
    if [ $ret -eq 0 ]; then
        ok "rmmod 成功"
    else
        ng "rmmod 失败: ${rmmod_ret}"
        echo "    可能仍有引用, 检查: lsmod | grep powerfs"
        return 1
    fi
    sleep 2

    # T7-5: 卸载后最终检查
    echo "  [T7-5] 卸载后最终检查..."
    if check_dmesg_clean "$base"; then
        ok "卸载后 dmesg 无异常"
    else
        ng "卸载后 dmesg 检测到异常"
        return 1
    fi

    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab 全部释放"
    else
        warn "powerfs slab 仍有残留:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # 内存恢复检查
    local mem_final
    mem_final=$(get_mem_available)
    echo "  -> 最终 MemAvailable: ${mem_final} KB (初始: ${MEM_INIT:-N/A} KB)"

    # 重新挂载 (便于后续测试)
    echo "  [T7] 重新挂载 powerfs (便于后续测试)..."
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
    echo -e "${C_CYAN}║  T5 性能测试: 顺序/随机/Stripe/多线程/元数据        ${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从小到大, 逐个确认, 检查内核状态             ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 初始化结果文件
    : > "${RESULTS_FILE}"
    {
        echo "PowerFS T5 性能测试 - $(date '+%Y-%m-%d %H:%M:%S')"
        echo "================================"
    } >> "${RESULTS_FILE}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_seq"
        "3:test_t3_rand"
        "4:test_t4_stripe_vs_flat"
        "5:test_t5_multithread"
        "6:test_t6_metadata"
        "7:test_t7_report_cleanup"
    )

    local executed=0
    local failed_test=""

    for entry in "${tests[@]}"; do
        local tid="${entry%%:*}"
        local func="${entry##*:}"

        if ! should_run "$tid"; then
            skip "T${tid} (未选择)"
            continue
        fi

        if [ -n "$failed_test" ]; then
            skip "T${tid} (因 T${failed_test} 失败而跳过)"
            continue
        fi

        if $func; then
            executed=$((executed + 1))
        else
            failed_test="$tid"
            echo ""
            echo -e "  ${C_RED}T${tid} 失败, 后续测试跳过${C_RESET}"
            executed=$((executed + 1))
        fi
    done

    # 总结
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  测试总结                                            ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo -e "  ${C_GREEN}PASS:${C_RESET}  ${PASS}"
    echo -e "  ${C_RED}FAIL:${C_RESET}  ${FAIL}"
    echo -e "  ${C_YELLOW}WARN:${C_RESET}  ${WARN}"
    echo -e "  ${C_YELLOW}SKIP:${C_RESET}  ${SKIP}"
    echo ""
    echo "  结果文件: ${RESULTS_FILE}"
    echo "  性能报告: ${REPORT_FILE}"
    echo ""

    if [ "$FAIL" -gt 0 ]; then
        echo -e "  ${C_RED}✗ T5 性能测试存在失败项${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  首个失败: T${failed_test}"
        fi
        echo ""
        echo "  排查建议:"
        echo "    1. 查看 VM dmesg: ./qemuctl.sh log powerfs"
        echo "    2. 实时监控 serial: ./qemuctl.sh serial-tail"
        echo "    3. 查看后端日志: ./qemuctl.sh service log filer-1"
        echo "    4. 确认 VM 内 fio 可用 (无 fio 时 fio 基准将跳过)"
        echo "    5. 重新运行单个测试: ./test_t5_performance.sh <T编号>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ T5 性能测试完成${C_RESET}"
    echo ""
    echo "  T5 验证内容:"
    echo "    - 编译 + 静态验证通过"
    echo "    - QEMU 挂载 + 内核状态正常"
    echo "    - 顺序读写基准 (1K/4K/1M/1G)"
    echo "    - 随机读写基准 (4K/64K + 70/30 混合)"
    echo "    - Stripe vs Flat 性能对比"
    echo "    - 多线程并发 IO (4/8/16 jobs)"
    echo "    - 元数据性能 (mdtest 或 touch/stat 替代)"
    echo "    - 性能报告已生成"
    echo ""
    exit 0
}

main "$@"
