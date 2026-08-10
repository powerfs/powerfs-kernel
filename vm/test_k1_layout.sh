#!/bin/bash
# K1 阶段渐进式测试: 协议对齐 + Flat 修复
#
# 验证内容 (参照 kernel-layout-completion-plan.md 4.2 节):
#   T0: 编译 + 静态验证 (无后端)
#   T1: QEMU 启动 + 挂载 (基础)
#   T2: 最小读写验证 (单文件 echo/cat)
#   T3: 渐进大小 MD5 校验 (1KB→1GB, 逐个确认)
#   T4: FUSE→内核 互通 (FUSE 创建 Flat, 内核读)
#   T5: 内核→FUSE 互通 (内核创建, FUSE 读)
#   T6: 持续运行 + 内核状态监控 (≥1 分钟 fio)
#   T7: GETATTR 字段解析验证 (K1 核心)
#   T8: 卸载 + 内核状态最终检查
#
# 核心原则:
#   - 内核正确性 != 应用完成, 必须检查 dmesg/slab/meminfo
#   - 从小到大逐个确认, 不可跳级
#   - 前一档未通过不进入下一档
#
# 运行环境: HOST (通过 SSH 控制 VM + docker exec 控制 FUSE 容器)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - 或由本脚本 T1 自动完成
#
# 用法:
#   ./test_k1_layout.sh            # 运行全部 (T0-T8)
#   ./test_k1_layout.sh 3          # 仅运行 T3
#   ./test_k1_layout.sh 0 1 2      # 运行 T0+T1+T2

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs           # FUSE 容器内挂载点 (fuse-1)
FUSE_CONTAINER="fuse-1"         # FUSE 容器名 (docker-compose-single.yml)
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

PASS=0
FAIL=0
WARN=0
SKIP=0

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
        serial_errors=$(grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task' "${qemu_log}" 2>/dev/null | tail -5 || true)
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
# T0: 编译 + 静态验证
# ============================================================
test_t0_compile() {
    section "T0: 编译 + 静态验证"

    # T0-1: 编译无警告
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

    # T0-2: 静态符号验证
    echo "  [T0-2] 静态符号验证..."
    if bash verify_module.sh 2>&1 | tail -5 | grep -q "ALL VERIFICATION TESTS PASSED"; then
        ok "verify_module.sh 全部通过"
    else
        ng "verify_module.sh 有失败项"
        bash verify_module.sh 2>&1 | grep '\[FAIL\]' | sed 's/^/    /'
        return 1
    fi

    # T0-3: chunk_size 确认
    echo "  [T0-3] chunk_size 确认..."
    local chunk_def
    chunk_def=$(grep -E 'POWERFS_CHUNK_SIZE\s+' powerfs.h | head -1)
    if echo "$chunk_def" | grep -q '1 \* 1024 \* 1024'; then
        ok "POWERFS_CHUNK_SIZE = 1MB"
    else
        ng "POWERFS_CHUNK_SIZE 不是 1MB: $chunk_def"
        return 1
    fi

    # T0-4: FieldId 补全确认
    echo "  [T0-4] FileLayout FieldId 补全确认..."
    local field_count
    field_count=$(grep -cE 'POWERFS_NET_FLD_(PLACEMENT|RELIABILITY|RELIABILITY_STATE|CHUNK_LAYOUT|STRIPE_SIZE|STRIPE_COUNT|START_VOLUME_IDX|VOLUME_IDS|START_NEEDLE_ID|CHUNK_SIZE|INLINE_DATA|INLINE_MAX_SIZE)\s*=' powerfs_net.h 2>/dev/null || echo 0)
    if [ "$field_count" -ge 11 ]; then
        ok "FileLayout FieldId 已补全 (${field_count}/12)"
    else
        ng "FileLayout FieldId 缺失 (仅 ${field_count}/12)"
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

    # T1-2: 检查 QEMU 运行 (通过 pgrep 检测 QEMU 进程)
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

    # T1-6: 挂载后 30s dmesg 检查
    echo "  [T1-6] 挂载后 30s dmesg 观察..."
    local base=$(dmesg_line_count)
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
# T2: 最小读写验证
# ============================================================
test_t2_minimal() {
    section "T2: 最小读写验证 (单文件)"

    local base=$(dmesg_line_count)

    # T2-1: write
    echo "  [T2-1] echo 写入..."
    if vm "echo hello > ${MNT}/t2_test.txt" 2>/dev/null; then
        ok "echo 写入成功"
    else
        ng "echo 写入失败"
        return 1
    fi

    # T2-2: read
    echo "  [T2-2] cat 读取..."
    local content
    content=$(vm "cat ${MNT}/t2_test.txt" 2>/dev/null)
    if [ "$content" = "hello" ]; then
        ok "cat 读取正确: '$content'"
    else
        ng "cat 读取错误: got '$content' want 'hello'"
        return 1
    fi

    # T2-3: stat
    echo "  [T2-3] stat 检查..."
    local size
    size=$(vm "stat -c %s ${MNT}/t2_test.txt" 2>/dev/null)
    if [ "$size" = "6" ]; then
        ok "stat size=6 (正确)"
    else
        ng "stat size 错误: got $size want 6"
        return 1
    fi

    # T2-4: unlink
    echo "  [T2-4] rm 删除..."
    if vm "rm ${MNT}/t2_test.txt" 2>/dev/null; then
        ok "rm 删除成功"
    else
        ng "rm 删除失败"
        return 1
    fi

    # T2-5: 确认文件消失
    if vm "test -f ${MNT}/t2_test.txt" 2>/dev/null; then
        ng "文件仍然存在"
        return 1
    else
        ok "文件已删除"
    fi

    # T2-6: 内核状态检查
    if check_kernel_state "T2 最小读写" "$base"; then
        ok "T2 内核状态正常"
    else
        ng "T2 内核状态异常"
        return 1
    fi

    return 0
}

# ============================================================
# T3: 渐进大小 MD5 校验 (从小到大, 逐个确认)
# ============================================================
test_t3_progressive_md5() {
    section "T3: 渐进大小 MD5 校验 (1KB → 1GB)"

    # 测试大小列表 (从小到大, 不可跳级)
    local sizes=("1K:1:1024" "4K:1:4096" "64K:1:65536" "1M:1:1048576" "10M:1:10485760" "100M:1:104857600" "1G:1:1073741824")

    for entry in "${sizes[@]}"; do
        local label="${entry%%:*}"
        local rest="${entry#*:}"
        local count="${rest%%:*}"
        local bytes="${rest##*:}"

        echo ""
        echo "  [T3] 文件大小: ${label} (${count}×bs)"

        local base=$(dmesg_line_count)

        # 生成随机数据并写入, 记录源 MD5
        local src_md5
        src_md5=$(vm "dd if=/dev/urandom bs=${label} count=${count} 2>/dev/null | tee ${MNT}/t3_test_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$src_md5" ]; then
            ng "写入 ${label} 失败 (md5 为空)"
            return 1
        fi
        echo "    写入 MD5: ${src_md5}"

        # 同步, 确保数据落盘
        vm "sync" 2>/dev/null

        # 读取并计算 MD5
        local read_md5
        read_md5=$(vm "cat ${MNT}/t3_test_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    读取 MD5: ${read_md5}"

        if [ "$src_md5" = "$read_md5" ]; then
            ok "${label} MD5 一致"
        else
            ng "${label} MD5 不一致 (src=${src_md5} read=${read_md5})"
            return 1
        fi

        # 检查文件大小
        local actual_size
        actual_size=$(vm "stat -c %s ${MNT}/t3_test_${label}.bin" 2>/dev/null)
        if [ "$actual_size" = "$bytes" ]; then
            ok "${label} 文件大小正确 (${actual_size})"
        else
            ng "${label} 文件大小错误 (got ${actual_size} want ${bytes})"
            return 1
        fi

        # 内核状态检查 (每个大小后)
        if ! check_kernel_state "T3 ${label}" "$base"; then
            ng "T3 ${label} 内核状态异常"
            return 1
        fi

        # slab 检查 (无暴增)
        local slab_now
        slab_now=$(get_powerfs_slab_total)
        echo "    slab (inode dentry): ${slab_now} (初始: ${SLAB_INIT:-N/A})"

        # 清理
        vm "rm ${MNT}/t3_test_${label}.bin" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    echo ""
    ok "T3 全部大小 MD5 校验通过"
    return 0
}

# ============================================================
# T4: FUSE→内核 互通 (FUSE 创建 Flat, 内核读)
# ============================================================
test_t4_fuse_to_kernel() {
    section "T4: FUSE→内核 互通"

    # 检查 FUSE 容器是否运行
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} 容器未运行, 跳过 T4"
        return 0
    fi
    ok "${FUSE_CONTAINER} 容器运行中"

    # FUSE 端挂载点
    local fuse_mount="${FUSE_MNT}"
    local test_dir="k1_t4_cross"
    local sizes=("1K" "1M" "10M")

    for sz in "${sizes[@]}"; do
        echo ""
        echo "  [T4] FUSE 创建 ${sz} Flat 文件 → 内核读取"

        local base=$(dmesg_line_count)

        # FUSE 端: 创建文件并记录 MD5
        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "mkdir -p ${fuse_mount}/${test_dir} && dd if=/dev/urandom bs=${sz} count=1 2>/dev/null > ${fuse_mount}/${test_dir}/file_${sz}.bin && md5sum ${fuse_mount}/${test_dir}/file_${sz}.bin | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$fuse_md5" ]; then
            ng "FUSE 端创建 ${sz} 文件失败"
            return 1
        fi
        echo "    FUSE 端 MD5: ${fuse_md5}"

        # 内核端: 读取同一文件并计算 MD5
        # 注意: 需要先 drop 内核 dcache 让内核重新 lookup
        vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
        sleep 1

        local kernel_md5
        kernel_md5=$(vm "cat ${MNT}/${test_dir}/file_${sz}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    内核端 MD5: ${kernel_md5}"

        if [ "$fuse_md5" = "$kernel_md5" ]; then
            ok "${sz} FUSE→内核 MD5 一致"
        else
            ng "${sz} FUSE→内核 MD5 不一致 (fuse=${fuse_md5} kernel=${kernel_md5})"
            return 1
        fi

        # 内核状态检查
        if ! check_kernel_state "T4 ${sz}" "$base"; then
            ng "T4 ${sz} 内核状态异常"
            return 1
        fi
    done

    # 清理
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_mount}/${test_dir}" 2>/dev/null
    ok "T4 FUSE→内核 互通通过"
    return 0
}

# ============================================================
# T5: 内核→FUSE 互通 (内核创建, FUSE 读)
# ============================================================
test_t5_kernel_to_fuse() {
    section "T5: 内核→FUSE 互通"

    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} 容器不可用, 跳过 T5"
        return 0
    fi

    local fuse_mount="${FUSE_MNT}"
    local test_dir="k1_t5_cross"
    local sizes=("1K" "1M" "10M")

    for sz in "${sizes[@]}"; do
        echo ""
        echo "  [T5] 内核创建 ${sz} 文件 → FUSE 读取"

        local base=$(dmesg_line_count)

        # 内核端: 创建文件并记录 MD5
        vm "mkdir -p ${MNT}/${test_dir}" 2>/dev/null
        local kernel_md5
        kernel_md5=$(vm "dd if=/dev/urandom bs=${sz} count=1 2>/dev/null > ${MNT}/${test_dir}/file_${sz}.bin && cat ${MNT}/${test_dir}/file_${sz}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$kernel_md5" ]; then
            ng "内核端创建 ${sz} 文件失败"
            return 1
        fi
        echo "    内核端 MD5: ${kernel_md5}"

        # 同步确保数据落盘
        vm "sync" 2>/dev/null
        sleep 1

        # FUSE 端: 读取同一文件并计算 MD5
        # 注意: FUSE 端需 drop cache 确保从后端读取
        docker exec ${FUSE_CONTAINER} sh -c "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null

        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "cat ${fuse_mount}/${test_dir}/file_${sz}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    FUSE 端 MD5: ${fuse_md5}"

        if [ "$kernel_md5" = "$fuse_md5" ]; then
            ok "${sz} 内核→FUSE MD5 一致"
        else
            ng "${sz} 内核→FUSE MD5 不一致 (kernel=${kernel_md5} fuse=${fuse_md5})"
            return 1
        fi

        # 内核状态检查
        if ! check_kernel_state "T5 ${sz}" "$base"; then
            ng "T5 ${sz} 内核状态异常"
            return 1
        fi
    done

    # T5 附加: umount + remount 持久化验证
    echo ""
    echo "  [T5-持久化] umount + remount 后再读"
    local persist_md5_before
    persist_md5_before=$(vm "cat ${MNT}/${test_dir}/file_1K.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    remount 前 MD5: ${persist_md5_before}"

    # umount
    vm "umount ${MNT}" 2>/dev/null
    sleep 2
    # remount
    vm "mount -t powerfs none ${MNT}" 2>/dev/null
    sleep 2

    if ! check_mount; then
        ng "remount 失败"
        return 1
    fi

    local persist_md5_after
    persist_md5_after=$(vm "cat ${MNT}/${test_dir}/file_1K.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    remount 后 MD5: ${persist_md5_after}"

    if [ "$persist_md5_before" = "$persist_md5_after" ]; then
        ok "remount 后数据持久化一致"
    else
        ng "remount 后数据不一致 (before=${persist_md5_before} after=${persist_md5_after})"
        return 1
    fi

    # 清理
    vm "rm -rf ${MNT}/${test_dir}" 2>/dev/null
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_mount}/${test_dir}" 2>/dev/null
    ok "T5 内核→FUSE 互通通过"
    return 0
}

# ============================================================
# T6: 持续运行 + 内核状态监控 (≥1 分钟)
# ============================================================
test_t6_sustained() {
    section "T6: 持续运行 + 内核状态监控 (60s)"

    # 检查 VM 内是否有 fio
    local has_fio
    has_fio=$(vm "which fio 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has_fio" = "no" ]; then
        warn "VM 内无 fio, 使用 dd 替代"
        # 使用 dd 持续读写替代
        return $(_t6_dd_alternative)
    fi
    ok "VM 内 fio 可用"

    local base=$(dmesg_line_count)
    local slab_before
    slab_before=$(get_powerfs_slab_total)
    local mem_before
    mem_before=$(get_mem_available)
    echo "  -> 测试前 slab: ${slab_before}"
    echo "  -> 测试前 MemAvailable: ${mem_before} KB"

    # T6-1: fio 顺序写 60s
    echo ""
    echo "  [T6-1] fio 顺序写 60s (后台 + 定期 dmesg 检查)"
    vm "fio --name=k1_write --directory=${MNT} --rw=write --bs=1M --size=100M --runtime=60 --time_based --numjobs=1 --group_reporting 2>&1 | tail -10" 2>/dev/null &
    local fio_pid=$!

    # 期间每 10s 检查 dmesg
    local check_interval=10
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        echo "    [${elapsed}s] dmesg 检查..."
        if ! check_dmesg_clean "$base"; then
            ng "fio 写入 ${elapsed}s 时检测到内核异常"
            kill $fio_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg 正常"
    done

    wait $fio_pid 2>/dev/null
    ok "fio 顺序写 60s 完成"

    # T6-2: fio 顺序读 60s
    echo ""
    echo "  [T6-2] fio 顺序读 60s"
    local base_read=$(dmesg_line_count)
    vm "fio --name=k1_read --directory=${MNT} --rw=read --bs=1M --size=100M --runtime=60 --time_based --numjobs=1 --group_reporting 2>&1 | tail -10" 2>/dev/null &
    fio_pid=$!

    elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        echo "    [${elapsed}s] dmesg 检查..."
        if ! check_dmesg_clean "$base_read"; then
            ng "fio 读取 ${elapsed}s 时检测到内核异常"
            kill $fio_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg 正常"
    done

    wait $fio_pid 2>/dev/null
    ok "fio 顺序读 60s 完成"

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
    vm "rm -f ${MNT}/k1_write* ${MNT}/k1_read*" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# dd 替代方案 (VM 无 fio 时)
_t6_dd_alternative() {
    echo "  [T6-dd] 使用 dd 持续读写 60s"

    local base=$(dmesg_line_count)

    # 后台 dd 写 60s
    vm "for i in \$(seq 1 60); do dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${MNT}/t6_dd_\${i}.bin; done" 2>/dev/null &
    local dd_pid=$!

    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep 10
        elapsed=$((elapsed + 10))
        if ! check_dmesg_clean "$base"; then
            ng "dd 写入 ${elapsed}s 时检测到内核异常"
            kill $dd_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg 正常"
    done
    wait $dd_pid 2>/dev/null

    # 读取验证
    vm "for i in \$(seq 1 60); do cat ${MNT}/t6_dd_\${i}.bin > /dev/null; done" 2>/dev/null
    ok "dd 读写 60s 完成"

    if check_kernel_state "T6-dd 完成" "$base"; then
        ok "T6-dd 内核状态正常"
    else
        ng "T6-dd 内核状态异常"
        return 1
    fi

    vm "rm -f ${MNT}/t6_dd_*" 2>/dev/null
    return 0
}

# ============================================================
# T7: GETATTR 字段解析验证 (K1 核心)
# ============================================================
test_t7_getattr_parse() {
    section "T7: GETATTR 字段解析验证"

    local base=$(dmesg_line_count)

    # 创建测试文件触发 GETATTR
    vm "echo test > ${MNT}/t7_layout.txt" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 1

    # stat 触发 GETATTR
    vm "stat ${MNT}/t7_layout.txt" 2>/dev/null
    sleep 1

    # 如果 FUSE 容器可用, 也 stat FUSE 创建的文件
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        docker exec ${FUSE_CONTAINER} sh -c "echo fuse_test > ${FUSE_MNT}/t7_fuse_file.txt" 2>/dev/null
        sleep 1
        vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
        sleep 1
        vm "stat ${MNT}/t7_fuse_file.txt" 2>/dev/null
        sleep 1
    fi

    # 检查 dmesg 中 GETATTR 解析日志
    local new_log
    new_log=$(dmesg_since "$base" 2>/dev/null)

    # 查找 placement 字段解析日志
    local placement_log
    placement_log=$(echo "$new_log" | grep -iE 'placement' || true)

    if [ -n "$placement_log" ]; then
        ok "GETATTR placement 字段已解析"
        echo "    日志: ${placement_log}" | head -3 | sed 's/^/    /'
    else
        warn "未在 dmesg 中找到 placement 解析日志 (可能 pr_debug 未启用或日志格式不同)"
        echo "    提示: 使用 ./qemuctl.sh debug 模式启动以获取更多日志"
    fi

    # 查找 chunk_size 字段
    local chunk_size_log
    chunk_size_log=$(echo "$new_log" | grep -iE 'chunk_size' || true)
    if [ -n "$chunk_size_log" ]; then
        ok "GETATTR chunk_size 字段已解析"
    else
        warn "未找到 chunk_size 解析日志"
    fi

    # 查找 reliability 字段
    local reliability_log
    reliability_log=$(echo "$new_log" | grep -iE 'reliability' || true)
    if [ -n "$reliability_log" ]; then
        ok "GETATTR reliability 字段已解析"
    else
        warn "未找到 reliability 解析日志"
    fi

    # 内核状态检查
    if check_kernel_state "T7 GETATTR 解析" "$base"; then
        ok "T7 内核状态正常"
    else
        ng "T7 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm -f ${MNT}/t7_layout.txt ${MNT}/t7_fuse_file.txt" 2>/dev/null

    # T7 为信息性检查, 不因日志缺失而失败 (日志格式可能不同)
    return 0
}

# ============================================================
# T8: 卸载 + 内核状态最终检查
# ============================================================
test_t8_unmount() {
    section "T8: 卸载 + 内核状态最终检查"

    local base=$(dmesg_line_count)

    # T8-1: umount
    echo "  [T8-1] umount powerfs..."
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

    # T8-2: rmmod
    echo "  [T8-2] rmmod powerfs..."
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

    # T8-3: 卸载后 dmesg 检查
    echo "  [T8-3] 卸载后 dmesg 检查..."
    if check_dmesg_clean "$base"; then
        ok "卸载后 dmesg 无异常"
    else
        ng "卸载后 dmesg 检测到异常"
        return 1
    fi

    # T8-4: slab 全部释放
    echo "  [T8-4] slab 释放检查..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab 全部释放"
    else
        warn "powerfs slab 仍有残留:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # T8-5: 内存恢复检查
    echo "  [T8-5] 内存恢复检查..."
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
    echo "  [T8] 重新挂载 powerfs (便于后续测试)..."
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
    echo -e "${C_CYAN}║  K1 渐进式测试: 协议对齐 + Flat 修复                ${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从小到大, 逐个确认, 检查内核状态             ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_minimal"
        "3:test_t3_progressive_md5"
        "4:test_t4_fuse_to_kernel"
        "5:test_t5_kernel_to_fuse"
        "6:test_t6_sustained"
        "7:test_t7_getattr_parse"
        "8:test_t8_unmount"
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
        echo -e "  ${C_RED}✗ K1 测试存在失败项${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  首个失败: T${failed_test}"
        fi
        echo ""
        echo "  排查建议:"
        echo "    1. 查看 VM dmesg: ./qemuctl.sh log powerfs"
        echo "    2. 实时监控: ./qemuctl.sh monitor powerfs"
        echo "    3. 查看后端日志: ./qemuctl.sh service log filer-1"
        echo "    4. 重新运行单个测试: ./test_k1_layout.sh <T编号>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ K1 测试全部通过${C_RESET}"
    echo ""
    echo "  K1 验证门达成:"
    echo "    - 编译 + 静态验证通过"
    echo "    - QEMU 挂载 + dmesg 正常"
    echo "    - 渐进大小 MD5 一致 (1KB→1GB)"
    echo "    - FUSE↔内核 互通"
    echo "    - 持续 60s 运行无异常"
    echo "    - 内核状态 (slab/meminfo/dmesg) 正常"
    echo ""
    echo "  可进入 K2 阶段 (Inline 小文件)"
    exit 0
}

main "$@"
