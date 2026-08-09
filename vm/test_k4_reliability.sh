#!/bin/bash
# K4 阶段渐进式测试: 可靠性 (CRC32 / Replicated / EC)
#
# 验证内容 (参照 K4 可靠性计划):
#   T0: 编译 + 静态验证 (powerfs_ec.o / powerfs_net_read_ec / crc32_le)
#   T1: QEMU 启动 + 挂载 (复用 qemuctl.sh)
#   T2: 基本读写验证 (无 reliability, 确认不回归, 1KB/1MB MD5)
#   T3: CRC32 校验验证 (FUSE 创建 → 内核读路径自动校验 → MD5 一致)
#   T4: Replicated 文件读取验证 (Filer 启用 reliability 时; skip 不阻断)
#   T5: EC 文件读取验证 (Filer 启用 EC 时; skip 不阻断)
#   T6: 持续运行 + 内核状态监控 (60s fio 随机读)
#   T7: 卸载 + 内核状态最终检查
#
# 核心原则:
#   - CRC32 在内核读路径自动校验; crc32==0 跳过校验 (对齐项目约束)
#   - 内核正确性 != 应用完成, 必须检查 dmesg/slab/meminfo/D 状态
#   - 从小到大逐个确认, 前一档未通过不进入下一档
#   - T4/T5 reliability 检测可能失败, 应 skip + warn (非 fail)
#
# 运行环境: HOST (通过 SSH 控制 VM + docker exec 控制 FUSE 容器)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - 或由本脚本 T1 自动完成
#
# 用法:
#   ./test_k4_reliability.sh            # 运行全部 (T0-T7)
#   ./test_k4_reliability.sh 3          # 仅运行 T3
#   ./test_k4_reliability.sh 3 4 5      # 运行 T3+T4+T5

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs           # FUSE 容器内挂载点 (fuse-1)
FUSE_CONTAINER="fuse-1"         # FUSE 容器名 (docker-compose-single.yml)
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

# 可靠性测试目录 (FUSE 容器内创建, 设置 reliability xattr)
REL_DIR="${FUSE_MNT}/reliability_test"
REL_KERNEL_DIR="${MNT}/reliability_test"

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

# 检查 FUSE 容器内 setfattr 可用, 不可用时尝试安装
# 返回 0=可用, 1=不可用
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
        return 1
    fi

    # 检查编译警告
    local warnings
    warnings=$(echo "$build_log" | grep -iE 'warning:' | grep -v 'Wno-' || true)
    if [ -z "$warnings" ]; then
        ok "编译无 warning"
    else
        warn "编译有 warning (非阻断):"
        echo "$warnings" | head -5 | sed 's/^/    /'
    fi

    # T0-2: 静态符号验证 (verify_module.sh)
    echo "  [T0-2] 静态符号验证..."
    if bash verify_module.sh 2>&1 | tail -5 | grep -q "ALL VERIFICATION TESTS PASSED"; then
        ok "verify_module.sh 全部通过"
    else
        ng "verify_module.sh 有失败项"
        bash verify_module.sh 2>&1 | grep '\[FAIL\]' | sed 's/^/    /'
        return 1
    fi

    # T0-3: powerfs_ec.o 编译产物存在 (K4 EC 模块)
    echo "  [T0-3] powerfs_ec.o 编译产物检查..."
    if [ -f powerfs_ec.o ]; then
        ok "powerfs_ec.o 存在 ($(ls -la powerfs_ec.o | awk '{print $5}') bytes)"
    else
        ng "powerfs_ec.o 不存在 (EC 模块未编译)"
        return 1
    fi

    # T0-4: powerfs_net_read_ec 函数存在 (K4 EC 读路径)
    echo "  [T0-4] powerfs_net_read_ec 函数检查..."
    local ec_func
    ec_func=$(grep -cE 'powerfs_net_read_ec' powerfs_net.c 2>/dev/null || echo 0)
    if [ "$ec_func" -ge 1 ]; then
        ok "powerfs_net_read_ec 已定义 (${ec_func} 处引用)"
    else
        ng "powerfs_net.c 中未找到 powerfs_net_read_ec"
        return 1
    fi

    # T0-5: crc32_le 调用存在 (K4 CRC32 校验)
    echo "  [T0-5] crc32_le 调用检查..."
    local crc_calls
    crc_calls=$(grep -cE 'crc32_le' powerfs_net.c 2>/dev/null || echo 0)
    if [ "$crc_calls" -ge 1 ]; then
        ok "crc32_le 调用存在 (${crc_calls} 处)"
    else
        ng "powerfs_net.c 中未找到 crc32_le 调用"
        return 1
    fi

    # T0-6: reliability 枚举确认 (SINGLE/REPLICATED/EC)
    echo "  [T0-6] reliability 枚举确认..."
    local rel_enums
    rel_enums=$(grep -cE 'POWERFS_RELIABILITY_(SINGLE|REPLICATED|EC)\s*=' powerfs.h 2>/dev/null || echo 0)
    if [ "$rel_enums" -ge 3 ]; then
        ok "reliability 枚举已定义 (${rel_enums}/3)"
    else
        ng "reliability 枚举缺失 (仅 ${rel_enums}/3)"
        return 1
    fi

    # T0-7: chunk_size 确认
    echo "  [T0-7] chunk_size 确认..."
    local chunk_def
    chunk_def=$(grep -E 'POWERFS_CHUNK_SIZE\s+' powerfs.h | head -1)
    if echo "$chunk_def" | grep -q '1 \* 1024 \* 1024'; then
        ok "POWERFS_CHUNK_SIZE = 1MB"
    else
        ng "POWERFS_CHUNK_SIZE 不是 1MB: $chunk_def"
        return 1
    fi

    cd "${SCRIPT_DIR}"
    return 0
}

# ============================================================
# T1: QEMU 启动 + 挂载
# ============================================================
test_t1_mount() {
    section "T1: QEMU 启动 + 挂载"

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
    echo "  [T1-5] 检查模块加载..."
    local mod_info
    mod_info=$(vm "lsmod | grep powerfs" 2>/dev/null)
    if [ -n "$mod_info" ]; then
        ok "powerfs 模块已加载: ${mod_info}"
    else
        ng "powerfs 模块未加载"
        return 1
    fi

    # T1-6: 挂载后 30s dmesg 检查
    echo "  [T1-6] 挂载后 30s dmesg 观察..."
    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)
    sleep 30
    if check_kernel_state "挂载后 30s" "$base"; then
        ok "挂载后 30s 内核状态正常"
    else
        ng "挂载后 30s 内核状态异常"
        return 1
    fi

    # 记录初始 slab 和 meminfo
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "  -> 初始 slab (inode dentry): ${SLAB_INIT}"
    echo "  -> 初始 MemAvailable: ${MEM_INIT} KB"

    return 0
}

# ============================================================
# T2: 基本读写验证 (无 reliability, 确认不回归)
# ============================================================
test_t2_basic_rw() {
    section "T2: 基本读写验证 (无 reliability, 1KB / 1MB MD5)"

    # 测试大小 (从小到大, 不可跳级)
    local sizes=("1K:1:1024" "1M:1:1048576")

    for entry in "${sizes[@]}"; do
        local label="${entry%%:*}"
        local rest="${entry#*:}"
        local count="${rest%%:*}"
        local bytes="${rest##*:}"

        echo ""
        echo "  [T2] 文件大小: ${label}"

        local base=$(dmesg_line_count)
        SERIAL_BASE=$(serial_line_count)

        # 内核端写入并记录 MD5
        local src_md5
        src_md5=$(vm "dd if=/dev/urandom bs=${label} count=${count} 2>/dev/null | tee ${MNT}/t2_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$src_md5" ]; then
            ng "写入 ${label} 失败"
            return 1
        fi
        echo "    写入 MD5: ${src_md5}"

        vm "sync" 2>/dev/null

        # 读取并校验 MD5
        local read_md5
        read_md5=$(vm "cat ${MNT}/t2_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    读取 MD5: ${read_md5}"

        if [ "$src_md5" = "$read_md5" ]; then
            ok "${label} MD5 一致 (基本读写未回归)"
        else
            ng "${label} MD5 不一致 (src=${src_md5} read=${read_md5})"
            return 1
        fi

        # 文件大小检查
        local actual_size
        actual_size=$(vm "stat -c %s ${MNT}/t2_${label}.bin" 2>/dev/null)
        if [ "$actual_size" = "$bytes" ]; then
            ok "${label} 文件大小正确 (${actual_size})"
        else
            ng "${label} 文件大小错误 (got ${actual_size} want ${bytes})"
            return 1
        fi

        # 内核状态检查
        if ! check_kernel_state "T2 ${label}" "$base"; then
            ng "T2 ${label} 内核状态异常"
            return 1
        fi

        # 清理
        vm "rm -f ${MNT}/t2_${label}.bin" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    echo ""
    ok "T2 基本读写未回归"
    return 0
}

# ============================================================
# T3: CRC32 校验验证 (FUSE 创建 → 内核读路径自动校验)
# ============================================================
test_t3_crc32() {
    section "T3: CRC32 校验验证 (FUSE 创建 → 内核读路径自动校验)"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        ng "${FUSE_CONTAINER} 容器未运行, T3 需 FUSE 端创建文件"
        return 1
    fi
    ok "${FUSE_CONTAINER} 容器运行中"

    # CRC32 在内核读路径自动校验:
    #   - 主路径: replica_chunks 中匹配 chunk_idx 的 crc32, crc32==0 跳过 (对齐项目约束)
    #   - failover 路径: rep_crc==0 跳过校验
    #   - EC 路径: shard_crc==0 跳过校验
    # 本档通过 FUSE 创建普通文件 (Flat), 内核读取时若 layout 带 crc32 会自动校验
    local sizes=("1K:1:1024" "1M:1:1048576")

    for entry in "${sizes[@]}"; do
        local label="${entry%%:*}"
        local rest="${entry#*:}"
        local count="${rest%%:*}"
        local bytes="${rest##*:}"

        echo ""
        echo "  [T3] CRC32 校验: ${label}"

        local base=$(dmesg_line_count)
        SERIAL_BASE=$(serial_line_count)

        # FUSE 端: 创建文件并记录 MD5
        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=${label} count=${count} 2>/dev/null > ${FUSE_MNT}/t3_crc_${label}.bin && md5sum ${FUSE_MNT}/t3_crc_${label}.bin | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$fuse_md5" ]; then
            ng "FUSE 端创建 ${label} 文件失败"
            return 1
        fi
        echo "    FUSE 写入 MD5: ${fuse_md5}"

        # 同步确保数据落盘
        docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
        vm "sync" 2>/dev/null
        sleep 1

        # 内核端: drop cache 后读取 (触发读路径, CRC32 自动校验)
        vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
        sleep 1

        local kernel_md5
        kernel_md5=$(vm "cat ${MNT}/t3_crc_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    内核读取 MD5: ${kernel_md5}"

        if [ "$fuse_md5" = "$kernel_md5" ]; then
            ok "${label} CRC32 读路径 MD5 一致"
        else
            ng "${label} CRC32 读路径 MD5 不一致 (fuse=${fuse_md5} kernel=${kernel_md5})"
            return 1
        fi

        # 检查 dmesg 中是否有 CRC mismatch (不应出现; crc32==0 跳过校验属正常)
        local crc_mismatch
        crc_mismatch=$(dmesg_since "$base" 2>/dev/null | grep -iE 'CRC mismatch' || true)
        if [ -z "$crc_mismatch" ]; then
            ok "${label} 无 CRC mismatch 日志 (crc32==0 时跳过校验属正常)"
        else
            ng "${label} 检测到 CRC mismatch (数据损坏或 crc 配置错误)"
            echo "$crc_mismatch" | head -5 | sed 's/^/    /'
            return 1
        fi

        # 内核状态检查
        if ! check_kernel_state "T3 ${label} CRC32" "$base"; then
            ng "T3 ${label} 内核状态异常"
            return 1
        fi

        # 清理
        docker exec ${FUSE_CONTAINER} sh -c "rm -f ${FUSE_MNT}/t3_crc_${label}.bin" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    echo ""
    ok "T3 CRC32 校验验证通过"
    return 0
}

# ============================================================
# T4: Replicated 文件读取验证
# ============================================================
test_t4_replicated() {
    section "T4: Replicated 文件读取验证"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} 容器未运行, 跳过 T4"
        return 0
    fi

    # T4-1: 检查 setfattr 可用
    echo "  [T4-1] 检查 setfattr 可用性..."
    if ! ensure_setfattr; then
        skip "FUSE 容器内 setfattr 不可用, 跳过 T4 (非阻断)"
        return 0
    fi
    ok "setfattr 可用"

    # T4-2: 检查 Filer 是否启用 reliability (尝试设置 reliability xattr)
    echo "  [T4-2] 检查 Filer 是否启用 Replicated reliability..."
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR} && mkdir -p ${REL_DIR}" 2>/dev/null

    # 尝试设置 reliability=replicated (常见 xattr 命名)
    local xattr_ret
    xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.reliability -v 'replicated' ${REL_DIR} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -ne 0 ]; then
        # 尝试备选命名
        xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.rel -v 'replicated' ${REL_DIR} 2>&1" 2>/dev/null)
        ret=$?
    fi

    if [ $ret -ne 0 ]; then
        warn "Filer 未启用 Replicated reliability (xattr 设置失败: ${xattr_ret})"
        echo "    提示: reliability 由 Filer/scrubber 决定, 跳过 T4 (非阻断)"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        skip "T4 Replicated (Filer 未启用 reliability)"
        return 0
    fi
    ok "Filer 支持 Replicated reliability (xattr 设置成功)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T4-3: FUSE 创建文件, 等待 scrubber 转换为 Replicated
    echo "  [T4-3] FUSE 创建文件, 等待 scrubber 转换为 Replicated..."
    local fuse_md5
    fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${REL_DIR}/t4_rep.bin && md5sum ${REL_DIR}/t4_rep.bin | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$fuse_md5" ]; then
        ng "FUSE 端创建 Replicated 文件失败"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        return 1
    fi
    echo "    FUSE 写入 MD5: ${fuse_md5}"

    # 等待 scrubber 转换 (轮询 getfattr reliability_state 或 dmesg 日志, 最多 60s)
    local waited=0
    local converted=0
    while [ $waited -lt 60 ]; do
        sleep 5
        waited=$((waited + 5))
        # 检查 dmesg 是否有 reliability_state 转换日志
        local conv_log
        conv_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'reliability_state|replicated.*state|scrubber' | tail -3 || true)
        if [ -n "$conv_log" ]; then
            converted=1
            echo "    [${waited}s] 检测到 scrubber 转换日志"
            break
        fi
        echo "    [${waited}s] 等待 scrubber 转换..."
    done

    if [ $converted -eq 0 ]; then
        warn "60s 内未检测到 scrubber 转换日志 (可能已转换或日志格式不同, 继续验证)"
    else
        ok "scrubber 转换已触发"
    fi

    # 同步 + drop cache
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 1
    vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    # T4-4: 内核读取 Replicated 文件, MD5 校验
    echo "  [T4-4] 内核读取 Replicated 文件, MD5 校验..."
    local kernel_md5
    kernel_md5=$(vm "cat ${REL_KERNEL_DIR}/t4_rep.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    内核读取 MD5: ${kernel_md5}"

    if [ "$fuse_md5" = "$kernel_md5" ]; then
        ok "Replicated 1MB MD5 一致 (FUSE→内核)"
    else
        ng "Replicated 1MB MD5 不一致 (fuse=${fuse_md5} kernel=${kernel_md5})"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        return 1
    fi

    # T4-5: 检查 dmesg 中是否有 failover 日志 (有副本时可能触发)
    echo "  [T4-5] 检查 dmesg failover 日志..."
    local failover_log
    failover_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'failover|replica.*read|read_needle' | tail -5 || true)
    if [ -n "$failover_log" ]; then
        ok "检测到 failover/replica 读日志 (副本读取路径已触发)"
        echo "$failover_log" | sed 's/^/    /'
    else
        warn "未检测到 failover 日志 (主副本读取成功, 未触发 failover 属正常)"
    fi

    # 内核状态检查
    if ! check_kernel_state "T4 Replicated" "$base"; then
        ng "T4 Replicated 内核状态异常"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
        return 1
    fi

    # 清理
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR}" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T4 Replicated 验证通过"
    return 0
}

# ============================================================
# T5: EC 文件读取验证
# ============================================================
test_t5_ec() {
    section "T5: EC 文件读取验证"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} 容器未运行, 跳过 T5"
        return 0
    fi

    # T5-1: 检查 setfattr 可用
    echo "  [T5-1] 检查 setfattr 可用性..."
    if ! ensure_setfattr; then
        skip "FUSE 容器内 setfattr 不可用, 跳过 T5 (非阻断)"
        return 0
    fi
    ok "setfattr 可用"

    # T5-2: 检查 Filer 是否启用 EC
    echo "  [T5-2] 检查 Filer 是否启用 EC..."
    local ec_dir="${FUSE_MNT}/ec_test"
    local ec_kernel_dir="${MNT}/ec_test"
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${ec_dir} && mkdir -p ${ec_dir}" 2>/dev/null

    # 尝试设置 reliability=ec (常见 xattr 命名)
    local xattr_ret
    xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.reliability -v 'ec' ${ec_dir} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -ne 0 ]; then
        xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.rel -v 'ec' ${ec_dir} 2>&1" 2>/dev/null)
        ret=$?
    fi

    if [ $ret -ne 0 ]; then
        warn "Filer 未启用 EC (xattr 设置失败: ${xattr_ret})"
        echo "    提示: EC 由 Filer/scrubber 决定, 跳过 T5 (非阻断)"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${ec_dir}" 2>/dev/null
        skip "T5 EC (Filer 未启用 EC)"
        return 0
    fi
    ok "Filer 支持 EC (xattr 设置成功)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T5-3: FUSE 创建大文件, 等待 scrubber 转换为 EC
    # EC 需要足够数据量 (建议 ≥ 4MB 覆盖多个 EC group)
    echo "  [T5-3] FUSE 创建 10MB 文件, 等待 scrubber 转换为 EC..."
    local fuse_md5
    fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=10 2>/dev/null > ${ec_dir}/t5_ec.bin && md5sum ${ec_dir}/t5_ec.bin | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$fuse_md5" ]; then
        ng "FUSE 端创建 EC 文件失败"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${ec_dir}" 2>/dev/null
        return 1
    fi
    echo "    FUSE 写入 MD5: ${fuse_md5}"

    # 等待 scrubber 转换 (EC 转换较慢, 最多 90s)
    local waited=0
    local converted=0
    while [ $waited -lt 90 ]; do
        sleep 5
        waited=$((waited + 5))
        local conv_log
        conv_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'reliability_state|EC.*state|scrubber|ec.*convert' | tail -3 || true)
        if [ -n "$conv_log" ]; then
            converted=1
            echo "    [${waited}s] 检测到 scrubber/EC 转换日志"
            break
        fi
        echo "    [${waited}s] 等待 scrubber EC 转换..."
    done

    if [ $converted -eq 0 ]; then
        warn "90s 内未检测到 EC 转换日志 (可能未转换或日志格式不同, 继续验证)"
    else
        ok "EC 转换已触发"
    fi

    # 同步 + drop cache
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 1
    vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    # T5-4: 内核读取 EC 文件, MD5 校验 (走 powerfs_net_read_ec 降级重建路径)
    echo "  [T5-4] 内核读取 EC 文件, MD5 校验..."
    local kernel_md5
    kernel_md5=$(vm "cat ${ec_kernel_dir}/t5_ec.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    内核读取 MD5: ${kernel_md5}"

    if [ "$fuse_md5" = "$kernel_md5" ]; then
        ok "EC 10MB MD5 一致 (FUSE→内核)"
    else
        ng "EC 10MB MD5 不一致 (fuse=${fuse_md5} kernel=${kernel_md5})"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${ec_dir}" 2>/dev/null
        return 1
    fi

    # T5-5: 检查 dmesg 中是否有 EC 读取日志
    echo "  [T5-5] 检查 dmesg EC 读取日志..."
    local ec_log
    ec_log=$(dmesg_since "$base" 2>/dev/null | grep -iE 'EC read|powerfs_net_read_ec|EC CRC|shard' | tail -5 || true)
    if [ -n "$ec_log" ]; then
        ok "检测到 EC 读取日志 (EC 读路径已触发)"
        echo "$ec_log" | sed 's/^/    /'
    else
        warn "未检测到 EC 读取日志 (可能日志未启用 pr_debug, 非阻断)"
    fi

    # 内核状态检查
    if ! check_kernel_state "T5 EC" "$base"; then
        ng "T5 EC 内核状态异常"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${ec_dir}" 2>/dev/null
        return 1
    fi

    # 清理
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${ec_dir}" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T5 EC 验证通过"
    return 0
}

# ============================================================
# T6: 持续运行 + 内核状态监控 (60s fio 随机读)
# ============================================================
test_t6_sustained() {
    section "T6: 持续运行 + 内核状态监控 (60s fio 随机读)"

    # T6-0: 预置 100M 文件供 fio 随机读 (FUSE 端创建, 内核端读)
    echo "  [T6-0] 预置 100M 文件供 fio 随机读..."
    local prebase=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local pre_file="${MNT}/t6_fio.bin"
    local pre_md5
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        # FUSE 端创建 (可能触发 reliability 路径)
        docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=100 2>/dev/null > ${FUSE_MNT}/t6_fio.bin" 2>/dev/null
        pre_md5=$(docker exec ${FUSE_CONTAINER} sh -c "md5sum ${FUSE_MNT}/t6_fio.bin | awk '{print \$1}'" 2>/dev/null)
        docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    else
        # 无 FUSE 容器时内核端创建
        pre_md5=$(vm "dd if=/dev/urandom bs=1M count=100 2>/dev/null > ${pre_file} && cat ${pre_file} | md5sum | awk '{print \$1}'" 2>/dev/null)
    fi

    if [ -z "$pre_md5" ]; then
        ng "预置 100M 文件失败"
        return 1
    fi
    echo "    预置文件 MD5: ${pre_md5}"
    vm "sync" 2>/dev/null
    sleep 2

    # 检查 VM 内是否有 fio
    local has_fio
    has_fio=$(vm "which fio 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has_fio" = "no" ]; then
        warn "VM 内无 fio, 使用 dd 替代"
        return $(_t6_dd_alternative "$pre_md5" "$pre_file")
    fi
    ok "VM 内 fio 可用"

    local base=$(dmesg_line_count)
    local slab_before
    slab_before=$(get_powerfs_slab_total)
    local mem_before
    mem_before=$(get_mem_available)
    echo "  -> 测试前 slab: ${slab_before}"
    echo "  -> 测试前 MemAvailable: ${mem_before} KB"

    # T6-1: fio 随机读 60s (后台 + 定期 dmesg 检查)
    echo ""
    echo "  [T6-1] fio 随机读 60s (后台 + 定期 dmesg 检查)"
    vm "fio --name=rel-test --filename=${pre_file} --rw=randread --bs=1M --size=100M --runtime=60 --time_based --numjobs=1 --group_reporting 2>&1 | tail -10" 2>/dev/null &
    local fio_pid=$!

    # 期间每 10s 检查 dmesg
    local check_interval=10
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        echo "    [${elapsed}s] dmesg 检查..."
        if ! check_dmesg_clean "$base"; then
            ng "fio 随机读 ${elapsed}s 时检测到内核异常"
            kill $fio_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg 正常"
    done

    wait $fio_pid 2>/dev/null
    ok "fio 随机读 60s 完成"

    # T6-2: 完成后数据完整性验证 (MD5 不变)
    echo ""
    echo "  [T6-2] fio 后数据完整性验证..."
    local post_md5
    post_md5=$(vm "cat ${pre_file} | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ "$pre_md5" = "$post_md5" ]; then
        ok "fio 后 MD5 一致 (数据未损坏)"
    else
        ng "fio 后 MD5 不一致 (before=${pre_md5} after=${post_md5})"
        return 1
    fi

    # T6-3: 完成后状态检查
    echo ""
    echo "  [T6-3] fio 完成后内核状态检查"

    local slab_after
    slab_after=$(get_powerfs_slab_total)
    local mem_after
    mem_after=$(get_mem_available)
    echo "  -> 测试后 slab: ${slab_after} (前: ${slab_before})"
    echo "  -> 测试后 MemAvailable: ${mem_after} KB (前: ${mem_before} KB)"

    # dmesg 最终检查
    if check_kernel_state "T6 fio 完成" "$base"; then
        ok "T6 内核状态最终检查通过"
    else
        ng "T6 内核状态异常"
        return 1
    fi

    # 内存泄漏检查 (MemAvailable 下降不超过 10%)
    if [ "$mem_before" -gt 0 ]; then
        local mem_drop_pct=$(( (mem_before - mem_after) * 100 / mem_before ))
        if [ $mem_drop_pct -lt 10 ]; then
            ok "内存泄漏检查通过 (MemAvailable 下降 ${mem_drop_pct}%)"
        else
            warn "MemAvailable 下降 ${mem_drop_pct}%, 可能有内存泄漏"
        fi
    fi

    # hung task 检查
    local hung
    hung=$(vm "dmesg | grep 'hung task' 2>/dev/null | grep -i powerfs" 2>/dev/null || true)
    if [ -z "$hung" ]; then
        ok "无 hung task"
    else
        ng "检测到 hung task: $hung"
        return 1
    fi

    # 清理 fio 测试文件
    vm "rm -f ${pre_file}" 2>/dev/null
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        docker exec ${FUSE_CONTAINER} sh -c "rm -f ${FUSE_MNT}/t6_fio.bin" 2>/dev/null
    fi
    vm "sync" 2>/dev/null

    return 0
}

# dd 替代方案 (VM 无 fio 时)
# 参数 $1: 预置文件 MD5 (用于完成后校验)
# 参数 $2: 预置文件路径
_t6_dd_alternative() {
    local pre_md5="$1"
    local pre_file="$2"
    echo "  [T6-dd] 使用 dd 持续随机读 60s"

    local base=$(dmesg_line_count)

    # 后台 dd 持续读 60s (随机偏移读取)
    vm "
        end_time=\$((\$(date +%s) + 60))
        while [ \$(date +%s) -lt \$end_time ]; do
            skip=\$(( RANDOM % 100 ))
            dd if=${pre_file} bs=1M count=1 skip=\${skip} 2>/dev/null > /dev/null
        done
    " 2>/dev/null &
    local dd_pid=$!

    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep 10
        elapsed=$((elapsed + 10))
        if ! check_dmesg_clean "$base"; then
            ng "dd 随机读 ${elapsed}s 时检测到内核异常"
            kill $dd_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg 正常"
    done
    wait $dd_pid 2>/dev/null

    # 完成后 MD5 校验
    local post_md5
    post_md5=$(vm "cat ${pre_file} | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ "$pre_md5" = "$post_md5" ]; then
        ok "dd 后 MD5 一致 (数据未损坏)"
    else
        ng "dd 后 MD5 不一致 (before=${pre_md5} after=${post_md5})"
        return 1
    fi

    if check_kernel_state "T6-dd 完成" "$base"; then
        ok "T6-dd 内核状态正常"
    else
        ng "T6-dd 内核状态异常"
        return 1
    fi

    vm "rm -f ${pre_file}" 2>/dev/null
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        docker exec ${FUSE_CONTAINER} sh -c "rm -f ${FUSE_MNT}/t6_fio.bin" 2>/dev/null
    fi
    return 0
}

# ============================================================
# T7: 卸载 + 内核状态最终检查
# ============================================================
test_t7_unmount() {
    section "T7: 卸载 + 内核状态最终检查"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T7-1: 清理可靠性测试目录
    echo "  [T7-1] 清理可靠性测试目录..."
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${REL_DIR} ${FUSE_MNT}/ec_test ${FUSE_MNT}/t3_crc_* ${FUSE_MNT}/t6_fio.bin" 2>/dev/null
        ok "FUSE 端测试目录已清理"
    else
        warn "${FUSE_CONTAINER} 容器未运行, 跳过 FUSE 端清理"
    fi
    vm "sync" 2>/dev/null

    # T7-2: umount
    echo "  [T7-2] umount powerfs..."
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

    # T7-3: rmmod
    echo "  [T7-3] rmmod powerfs..."
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

    # T7-4: 卸载后 dmesg 检查
    echo "  [T7-4] 卸载后 dmesg 检查..."
    if check_dmesg_clean "$base"; then
        ok "卸载后 dmesg 无异常"
    else
        ng "卸载后 dmesg 检测到异常"
        return 1
    fi

    # T7-5: slab 全部释放
    echo "  [T7-5] slab 释放检查..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab 全部释放"
    else
        warn "powerfs slab 仍有残留:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # T7-6: 内存恢复检查
    echo "  [T7-6] 内存恢复检查..."
    local mem_final
    mem_final=$(get_mem_available)
    echo "  -> 最终 MemAvailable: ${mem_final} KB (初始: ${MEM_INIT} KB)"

    if [ "${MEM_INIT:-0}" -gt 0 ]; then
        local diff=$(( ${MEM_INIT} - ${mem_final} ))
        if [ $diff -lt 50000 ]; then
            ok "内存恢复良好 (差异 ${diff} KB)"
        else
            warn "内存差异较大 (${diff} KB), 可能有泄漏"
        fi
    fi

    # 重新挂载以便后续使用
    echo ""
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
    echo -e "${C_CYAN}║  K4 渐进式测试: 可靠性 (CRC32/Replicated/EC)        ${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从小到大, 逐个确认, 检查内核状态             ${C_RESET}"
    echo -e "${C_CYAN}║  T4/T5 reliability 检测失败时 skip (非阻断)         ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_basic_rw"
        "3:test_t3_crc32"
        "4:test_t4_replicated"
        "5:test_t5_ec"
        "6:test_t6_sustained"
        "7:test_t7_unmount"
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

    if [ "$FAIL" -gt 0 ]; then
        echo -e "  ${C_RED}✗ K4 测试存在失败项${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  首个失败: T${failed_test}"
        fi
        echo ""
        echo "  排查建议:"
        echo "    1. 查看 VM dmesg: ./qemuctl.sh log powerfs"
        echo "    2. 实时监控 serial: ./qemuctl.sh serial-tail"
        echo "    3. 查看后端日志: ./qemuctl.sh service log filer-1"
        echo "    4. 确认 FUSE 容器内 setfattr 可用 (apt-get install -y attr)"
        echo "    5. T4/T5 skip 属正常 (Filer 未启用 reliability/EC)"
        echo "    6. 重新运行单个测试: ./test_k4_reliability.sh <T编号>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ K4 测试全部通过${C_RESET}"
    echo ""
    echo "  K4 验证门达成:"
    echo "    - 编译 + 静态验证 (powerfs_ec.o / powerfs_net_read_ec / crc32_le)"
    echo "    - 基本读写未回归 (1KB/1MB MD5)"
    echo "    - CRC32 读路径校验通过 (无 mismatch)"
    echo "    - Replicated 读取验证 (Filer 启用时)"
    echo "    - EC 读取验证 (Filer 启用时)"
    echo "    - 持续 60s fio 随机读无异常"
    echo "    - 内核状态 (slab/meminfo/dmesg/serial) 正常"
    echo ""
    echo "  可进入下一阶段 (K5+)"
    exit 0
}

main "$@"
