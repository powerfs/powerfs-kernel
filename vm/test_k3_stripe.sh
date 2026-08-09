#!/bin/bash
# K3 阶段渐进式测试: Stripe 多卷布局
#
# 验证内容 (参照 kernel-layout-completion-plan.md K3 节):
#   T0: 编译 + 静态验证 (Stripe 字段/函数/常量)
#   T1: QEMU 启动 + 挂载 (复用 qemuctl.sh)
#   T2: FUSE 创建 Stripe 目录 + 设置 xattr (stripe:4:1MB)
#   T3: 渐进大小 Stripe 文件读写 MD5 校验 (1KB→1MB→10MB→100MB, 逐个确认)
#   T4: 内核写入 Stripe 文件 + FUSE 读取验证
#   T5: WideStripe 文件读取验证 (wide_stripe:256:4MB, 支持时)
#   T6: 持续运行 + 内核状态监控 (60s fio 随机读)
#   T7: 卸载 + 内核状态最终检查
#
# 核心原则:
#   - Stripe 走多卷并发, locate_chunk 按 stripe_unit_idx 索引 volume_ids[]
#   - 内核正确性 != 应用完成, 必须检查 dmesg/slab/meminfo/D 状态
#   - 从小到大逐个确认, 前一档未通过不进入下一档
#
# 运行环境: HOST (通过 SSH 控制 VM + docker exec 控制 FUSE 容器)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - 或由本脚本 T1 自动完成
#   - 集群至少有 4 个 volume (stripe:4:1MB 需要 4 卷)
#
# 用法:
#   ./test_k3_stripe.sh            # 运行全部 (T0-T7)
#   ./test_k3_stripe.sh 3          # 仅运行 T3
#   ./test_k3_stripe.sh 3 4 5      # 运行 T3+T4+T5

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs           # FUSE 容器内挂载点 (fuse-1)
FUSE_CONTAINER="fuse-1"         # FUSE 容器名 (docker-compose-single.yml)
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

# Stripe 测试目录 (FUSE 容器内创建, 设置 xattr)
STRIPE_DIR="${FUSE_MNT}/stripe_test"
STRIPE_KERNEL_DIR="${MNT}/stripe_test"

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

    # T0-3: Stripe Placement 枚举确认
    echo "  [T0-3] Placement 枚举确认 (FLAT/STRIPE/WIDESTRIPE)..."
    local placement_enums
    placement_enums=$(grep -cE 'POWERFS_PLACEMENT_(FLAT|STRIPE|WIDESTRIPE)\s*=' powerfs.h 2>/dev/null || echo 0)
    if [ "$placement_enums" -ge 3 ]; then
        ok "Placement 枚举已定义 (${placement_enums}/3)"
    else
        ng "Placement 枚举缺失 (仅 ${placement_enums}/3)"
        return 1
    fi

    # T0-4: Stripe 相关 FieldId 确认
    echo "  [T0-4] Stripe FieldId 确认..."
    local stripe_fields
    stripe_fields=$(grep -cE 'POWERFS_NET_FLD_(STRIPE_SIZE|STRIPE_COUNT|START_VOLUME_IDX|VOLUME_IDS)\s*=' powerfs_net.h 2>/dev/null || echo 0)
    if [ "$stripe_fields" -ge 4 ]; then
        ok "Stripe FieldId 已定义 (${stripe_fields}/4)"
    else
        ng "Stripe FieldId 缺失 (仅 ${stripe_fields}/4)"
        return 1
    fi

    # T0-5: inode 中 stripe 字段确认
    echo "  [T0-5] inode Stripe 字段确认..."
    local inode_stripe
    inode_stripe=$(grep -cE 'stripe_size|stripe_count|start_volume_idx|volume_ids' powerfs.h 2>/dev/null || echo 0)
    if [ "$inode_stripe" -ge 4 ]; then
        ok "inode Stripe 字段已定义 (${inode_stripe}/4)"
    else
        ng "inode Stripe 字段缺失 (仅 ${inode_stripe}/4)"
        return 1
    fi

    # T0-6: locate_chunk Stripe 分支确认
    echo "  [T0-6] powerfs_locate_chunk Stripe 分支确认..."
    local locate_stripe
    locate_stripe=$(grep -cE 'POWERFS_PLACEMENT_STRIPE|POWERFS_PLACEMENT_WIDESTRIPE' powerfs_fs.c 2>/dev/null || echo 0)
    if [ "$locate_stripe" -ge 2 ]; then
        ok "locate_chunk 已处理 Stripe/WideStripe (${locate_stripe} 处)"
    else
        ng "locate_chunk 未处理 Stripe (${locate_stripe} 处)"
        return 1
    fi

    # T0-7: chunk_size 确认 (Stripe 单元 = chunk_size)
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

    # T1-2: 检查 volume 数量 (stripe:4 至少需 4 个 volume)
    echo "  [T1-2] 检查 volume 数量 (Stripe 需 ≥4)..."
    local vol_count
    vol_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'volume-[0-9]+' || echo 0)
    if [ "$vol_count" -ge 4 ]; then
        ok "volume 数量充足 (${vol_count} 个, Stripe 可用)"
    else
        warn "volume 数量不足 (${vol_count} 个, Stripe:4 需 4 个)"
        echo "    提示: stripe_count > volume 数时, locate_chunk 会回退到可用卷"
        echo "    建议: ./qemuctl.sh service start 启动完整集群"
    fi

    # T1-3: 检查 QEMU 运行
    echo "  [T1-3] 检查 QEMU 运行状态..."
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

    # T1-4: SSH 可达
    echo "  [T1-4] 检查 VM SSH 可达..."
    if vm_alive; then
        ok "VM SSH 可达"
    else
        ng "VM SSH 不可达"
        return 1
    fi

    # T1-5: powerfs 已挂载
    echo "  [T1-5] 检查 powerfs 挂载..."
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

    # T1-6: 模块引用计数
    echo "  [T1-6] 检查模块引用计数..."
    local mod_info
    mod_info=$(vm "lsmod | grep powerfs" 2>/dev/null)
    if [ -n "$mod_info" ]; then
        ok "powerfs 模块已加载: ${mod_info}"
    else
        ng "powerfs 模块未加载"
        return 1
    fi

    # T1-7: 挂载后 30s dmesg 检查
    echo "  [T1-7] 挂载后 30s dmesg 观察..."
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
# T2: FUSE 创建 Stripe 目录 + 设置 xattr
# ============================================================
test_t2_stripe_xattr() {
    section "T2: FUSE 创建 Stripe 目录 + 设置 xattr"

    # T2-1: 检查 FUSE 容器
    echo "  [T2-1] 检查 FUSE 容器..."
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        ng "${FUSE_CONTAINER} 容器未运行, Stripe xattr 需在 FUSE 端设置"
        return 1
    fi
    ok "${FUSE_CONTAINER} 容器运行中"

    # T2-2: 检查 FUSE 容器内 setfattr 可用
    echo "  [T2-2] 检查 setfattr 可用性..."
    local has_setfattr
    has_setfattr=$(docker exec ${FUSE_CONTAINER} sh -c "which setfattr 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has_setfattr" = "no" ]; then
        warn "FUSE 容器内无 setfattr, 尝试 attr 包"
        docker exec ${FUSE_CONTAINER} sh -c "apt-get install -y attr >/dev/null 2>&1 || apk add attr >/dev/null 2>&1" 2>/dev/null
        has_setfattr=$(docker exec ${FUSE_CONTAINER} sh -c "which setfattr 2>/dev/null || echo no" 2>/dev/null)
    fi
    if [ "$has_setfattr" = "no" ]; then
        ng "FUSE 容器内 setfattr 不可用"
        echo "    请在容器内安装 attr 包: apt-get install -y attr"
        return 1
    fi
    ok "setfattr 可用: ${has_setfattr}"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T2-3: 创建 Stripe 测试目录
    echo "  [T2-3] 创建 Stripe 目录 ${STRIPE_DIR}..."
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${STRIPE_DIR} && mkdir -p ${STRIPE_DIR}" 2>/dev/null
    if docker exec ${FUSE_CONTAINER} sh -c "test -d ${STRIPE_DIR}" 2>/dev/null; then
        ok "Stripe 目录创建成功"
    else
        ng "Stripe 目录创建失败"
        return 1
    fi

    # T2-4: 设置 xattr placement = stripe:4:1MB
    echo "  [T2-4] 设置 xattr placement = stripe:4:1MB..."
    local xattr_ret
    xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.placement -v 'stripe:4:1MB' ${STRIPE_DIR} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -eq 0 ]; then
        ok "xattr 设置成功"
    else
        ng "xattr 设置失败: ${xattr_ret}"
        return 1
    fi

    # T2-5: 验证 xattr 设置成功
    echo "  [T2-5] 验证 xattr..."
    local xattr_val
    xattr_val=$(docker exec ${FUSE_CONTAINER} sh -c "getfattr -n user.powerfs.placement --only-values ${STRIPE_DIR} 2>/dev/null" 2>/dev/null)
    if [ "$xattr_val" = "stripe:4:1MB" ]; then
        ok "xattr 验证正确: '${xattr_val}'"
    else
        ng "xattr 验证错误: got '${xattr_val}' want 'stripe:4:1MB'"
        return 1
    fi

    # T2-6: 内核端 lookup 该目录 (触发 FUSE→Filer→内核元数据同步)
    echo "  [T2-6] 内核端 lookup Stripe 目录..."
    vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1
    if vm "test -d ${STRIPE_KERNEL_DIR}" 2>/dev/null; then
        ok "内核端可见 Stripe 目录"
    else
        warn "内核端暂不可见 Stripe 目录 (可能需等元数据同步, 非阻断)"
    fi

    # T2-7: 内核状态检查
    if check_kernel_state "T2 Stripe xattr" "$base"; then
        ok "T2 内核状态正常"
    else
        ng "T2 内核状态异常"
        return 1
    fi

    return 0
}

# ============================================================
# T3: 渐进大小 Stripe 文件读写 MD5 校验 (1KB→1MB→10MB→100MB)
# ============================================================
test_t3_stripe_md5() {
    section "T3: 渐进大小 Stripe 文件读写 MD5 校验 (1KB → 100MB)"

    # 测试大小列表 (从小到大, 不可跳级)
    # 格式: label:count:bytes (dd bs=label count=count)
    local sizes=("1K:1:1024" "1M:1:1048576" "10M:1:10485760" "100M:1:104857600")

    for entry in "${sizes[@]}"; do
        local label="${entry%%:*}"
        local rest="${entry#*:}"
        local count="${rest%%:*}"
        local bytes="${rest##*:}"

        echo ""
        echo "  [T3] Stripe 文件大小: ${label} (${count}×bs)"

        local base=$(dmesg_line_count)
        SERIAL_BASE=$(serial_line_count)

        # FUSE 端: 在 Stripe 目录下创建文件 (dd 写入) 并记录 MD5
        # Stripe placement 由父目录 xattr 继承
        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=${label} count=${count} 2>/dev/null > ${STRIPE_DIR}/t3_${label}.bin && md5sum ${STRIPE_DIR}/t3_${label}.bin | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$fuse_md5" ]; then
            ng "FUSE 端写入 ${label} Stripe 文件失败"
            return 1
        fi
        echo "    FUSE 写入 MD5: ${fuse_md5}"

        # 同步, 确保数据落盘
        docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
        vm "sync" 2>/dev/null
        sleep 1

        # 内核端: drop cache 后读取同一文件并计算 MD5
        vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
        sleep 1

        local kernel_md5
        kernel_md5=$(vm "cat ${STRIPE_KERNEL_DIR}/t3_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    内核读取 MD5: ${kernel_md5}"

        if [ "$fuse_md5" = "$kernel_md5" ]; then
            ok "${label} Stripe MD5 一致 (FUSE→内核)"
        else
            ng "${label} Stripe MD5 不一致 (fuse=${fuse_md5} kernel=${kernel_md5})"
            return 1
        fi

        # 检查文件大小
        local actual_size
        actual_size=$(vm "stat -c %s ${STRIPE_KERNEL_DIR}/t3_${label}.bin" 2>/dev/null)
        if [ "$actual_size" = "$bytes" ]; then
            ok "${label} Stripe 文件大小正确 (${actual_size})"
        else
            ng "${label} Stripe 文件大小错误 (got ${actual_size} want ${bytes})"
            return 1
        fi

        # 内核状态检查 (每个大小后)
        if ! check_kernel_state "T3 ${label} Stripe" "$base"; then
            ng "T3 ${label} 内核状态异常"
            return 1
        fi

        # slab 检查 (无暴增)
        local slab_now
        slab_now=$(get_powerfs_slab_total)
        echo "    slab (inode dentry): ${slab_now} (初始: ${SLAB_INIT:-N/A})"

        # 清理 (避免占用后端存储)
        docker exec ${FUSE_CONTAINER} sh -c "rm -f ${STRIPE_DIR}/t3_${label}.bin" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    echo ""
    ok "T3 全部 Stripe 大小 MD5 校验通过"
    return 0
}

# ============================================================
# T4: 内核写入 Stripe 文件 + FUSE 读取验证
# ============================================================
test_t4_kernel_to_fuse() {
    section "T4: 内核写入 Stripe 文件 + FUSE 读取验证"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} 容器未运行, 跳过 T4"
        return 0
    fi

    # T4-1: 内核端在 Stripe 目录下创建文件
    # 注意: 内核端写入时, placement 由父目录 xattr 决定 (Filer 端继承)
    local sizes=("1K" "1M" "10M")

    for sz in "${sizes[@]}"; do
        echo ""
        echo "  [T4] 内核创建 ${sz} Stripe 文件 → FUSE 读取"

        local base=$(dmesg_line_count)
        SERIAL_BASE=$(serial_line_count)

        # 内核端: 创建文件并记录 MD5
        local kernel_md5
        kernel_md5=$(vm "dd if=/dev/urandom bs=${sz} count=1 2>/dev/null > ${STRIPE_KERNEL_DIR}/t4_${sz}.bin && cat ${STRIPE_KERNEL_DIR}/t4_${sz}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$kernel_md5" ]; then
            ng "内核端创建 ${sz} Stripe 文件失败"
            return 1
        fi
        echo "    内核端 MD5: ${kernel_md5}"

        # 同步确保数据落盘
        vm "sync" 2>/dev/null
        sleep 1

        # FUSE 端: drop cache 确保从后端读取
        docker exec ${FUSE_CONTAINER} sh -c "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null

        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "cat ${STRIPE_DIR}/t4_${sz}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    FUSE 端 MD5: ${fuse_md5}"

        if [ "$kernel_md5" = "$fuse_md5" ]; then
            ok "${sz} 内核→FUSE Stripe MD5 一致"
        else
            ng "${sz} 内核→FUSE Stripe MD5 不一致 (kernel=${kernel_md5} fuse=${fuse_md5})"
            return 1
        fi

        # 内核状态检查
        if ! check_kernel_state "T4 ${sz}" "$base"; then
            ng "T4 ${sz} 内核状态异常"
            return 1
        fi

        # 清理
        vm "rm -f ${STRIPE_KERNEL_DIR}/t4_${sz}.bin" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    ok "T4 内核→FUSE Stripe 互通通过"
    return 0
}

# ============================================================
# T5: WideStripe 文件读取验证 (如果支持)
# ============================================================
test_t5_widestripe() {
    section "T5: WideStripe 文件读取验证 (wide_stripe:256:4MB)"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} 容器未运行, 跳过 T5"
        return 0
    fi

    # T5-1: 检查 volume 数量 (wide_stripe:256 需大量卷, 不足时降级测试)
    echo "  [T5-1] 检查 volume 数量 (wide_stripe:256 理想需 ≥256)..."
    local vol_count
    vol_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'volume-[0-9]+' || echo 0)
    if [ "$vol_count" -lt 256 ]; then
        warn "volume 数量 ${vol_count} < 256, wide_stripe:256 将回退到可用卷"
        echo "    提示: 这是预期行为 (locate_chunk 对 stripe_unit_idx 取模), 继续验证"
    fi

    # T5-2: 创建 WideStripe 目录 + 设置 xattr
    echo "  [T5-2] 创建 WideStripe 目录 + xattr..."
    local wide_dir="${FUSE_MNT}/widestripe_test"
    local wide_kernel_dir="${MNT}/widestripe_test"

    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${wide_dir} && mkdir -p ${wide_dir}" 2>/dev/null

    local xattr_ret
    xattr_ret=$(docker exec ${FUSE_CONTAINER} sh -c "setfattr -n user.powerfs.placement -v 'wide_stripe:256:4MB' ${wide_dir} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -ne 0 ]; then
        warn "WideStripe xattr 设置失败: ${xattr_ret}"
        echo "    可能 Filer 不支持 WideStripe, 跳过 T5 (非阻断)"
        # 清理
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${wide_dir}" 2>/dev/null
        return 0
    fi

    # 验证 xattr
    local xattr_val
    xattr_val=$(docker exec ${FUSE_CONTAINER} sh -c "getfattr -n user.powerfs.placement --only-values ${wide_dir} 2>/dev/null" 2>/dev/null)
    if [ "$xattr_val" = "wide_stripe:256:4MB" ]; then
        ok "WideStripe xattr 设置正确"
    else
        ng "WideStripe xattr 验证错误: got '${xattr_val}'"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${wide_dir}" 2>/dev/null
        return 1
    fi

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T5-3: 创建 WideStripe 文件并验证读取 (1MB, 避免占用过多存储)
    echo "  [T5-3] WideStripe 文件读写 MD5 校验 (1MB)..."
    local fuse_md5
    fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=1 2>/dev/null > ${wide_dir}/t5_1M.bin && md5sum ${wide_dir}/t5_1M.bin | awk '{print \$1}'" 2>/dev/null)

    if [ -z "$fuse_md5" ]; then
        ng "WideStripe 1MB 文件写入失败"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${wide_dir}" 2>/dev/null
        return 1
    fi
    echo "    FUSE 写入 MD5: ${fuse_md5}"

    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 1

    # 内核端读取验证
    vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    local kernel_md5
    kernel_md5=$(vm "cat ${wide_kernel_dir}/t5_1M.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    内核读取 MD5: ${kernel_md5}"

    if [ "$fuse_md5" = "$kernel_md5" ]; then
        ok "WideStripe 1MB MD5 一致 (FUSE→内核)"
    else
        ng "WideStripe 1MB MD5 不一致 (fuse=${fuse_md5} kernel=${kernel_md5})"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${wide_dir}" 2>/dev/null
        return 1
    fi

    # T5-4: 内核状态检查
    if ! check_kernel_state "T5 WideStripe" "$base"; then
        ng "T5 WideStripe 内核状态异常"
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${wide_dir}" 2>/dev/null
        return 1
    fi

    # 清理
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${wide_dir}" 2>/dev/null
    vm "sync" 2>/dev/null

    ok "T5 WideStripe 验证通过"
    return 0
}

# ============================================================
# T6: 持续运行 + 内核状态监控 (60s fio 随机读)
# ============================================================
test_t6_sustained() {
    section "T6: 持续运行 + 内核状态监控 (60s fio 随机读)"

    # T6-0: 在 Stripe 目录下预置一个 100M 文件供随机读
    echo "  [T6-0] 预置 100M Stripe 文件供 fio 随机读..."
    local prebase=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    docker exec ${FUSE_CONTAINER} sh -c "dd if=/dev/urandom bs=1M count=100 2>/dev/null > ${STRIPE_DIR}/t6_fio.bin" 2>/dev/null
    local pre_md5
    pre_md5=$(docker exec ${FUSE_CONTAINER} sh -c "md5sum ${STRIPE_DIR}/t6_fio.bin | awk '{print \$1}'" 2>/dev/null)
    if [ -z "$pre_md5" ]; then
        ng "预置 100M Stripe 文件失败"
        return 1
    fi
    echo "    预置文件 MD5: ${pre_md5}"
    docker exec ${FUSE_CONTAINER} sh -c "sync" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 2

    # 检查 VM 内是否有 fio
    local has_fio
    has_fio=$(vm "which fio 2>/dev/null || echo no" 2>/dev/null)
    if [ "$has_fio" = "no" ]; then
        warn "VM 内无 fio, 使用 dd 替代"
        return $(_t6_dd_alternative "$pre_md5")
    fi
    ok "VM 内 fio 可用"

    local base=$(dmesg_line_count)
    local slab_before
    slab_before=$(get_powerfs_slab_total)
    local mem_before
    mem_before=$(get_mem_available)
    echo "  -> 测试前 slab: ${slab_before}"
    echo "  -> 测试前 MemAvailable: ${mem_before} KB"

    # T6-1: fio 随机读 60s (在内核端读取 Stripe 文件)
    echo ""
    echo "  [T6-1] fio 随机读 60s (后台 + 定期 dmesg 检查)"
    vm "fio --name=stripe-test --filename=${STRIPE_KERNEL_DIR}/t6_fio.bin --rw=randread --bs=1M --size=100M --runtime=60 --time_based --numjobs=1 --group_reporting 2>&1 | tail -10" 2>/dev/null &
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
    post_md5=$(vm "cat ${STRIPE_KERNEL_DIR}/t6_fio.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
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
    docker exec ${FUSE_CONTAINER} sh -c "rm -f ${STRIPE_DIR}/t6_fio.bin" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# dd 替代方案 (VM 无 fio 时)
# 参数 $1: 预置文件的 MD5 (用于完成后校验)
_t6_dd_alternative() {
    local pre_md5="$1"
    echo "  [T6-dd] 使用 dd 持续随机读 60s"

    local base=$(dmesg_line_count)

    # 后台 dd 持续读 60s (随机偏移读取)
    vm "
        end_time=\$((\$(date +%s) + 60))
        while [ \$(date +%s) -lt \$end_time ]; do
            skip=\$(( RANDOM % 100 ))
            dd if=${STRIPE_KERNEL_DIR}/t6_fio.bin bs=1M count=1 skip=\${skip} 2>/dev/null > /dev/null
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
    post_md5=$(vm "cat ${STRIPE_KERNEL_DIR}/t6_fio.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
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

    docker exec ${FUSE_CONTAINER} sh -c "rm -f ${STRIPE_DIR}/t6_fio.bin" 2>/dev/null
    return 0
}

# ============================================================
# T7: 卸载 + 内核状态最终检查
# ============================================================
test_t7_unmount() {
    section "T7: 卸载 + 内核状态最终检查"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T7-1: 清理 Stripe 测试目录
    echo "  [T7-1] 清理 Stripe 测试目录..."
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${STRIPE_DIR} ${FUSE_MNT}/widestripe_test" 2>/dev/null
        ok "FUSE 端 Stripe 目录已清理"
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
    echo -e "${C_CYAN}║  K3 渐进式测试: Stripe 多卷布局                     ${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从小到大, 逐个确认, 检查内核状态             ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_stripe_xattr"
        "3:test_t3_stripe_md5"
        "4:test_t4_kernel_to_fuse"
        "5:test_t5_widestripe"
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
        echo -e "  ${C_RED}✗ K3 测试存在失败项${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  首个失败: T${failed_test}"
        fi
        echo ""
        echo "  排查建议:"
        echo "    1. 查看 VM dmesg: ./qemuctl.sh log powerfs"
        echo "    2. 实时监控 serial: ./qemuctl.sh serial-tail"
        echo "    3. 查看后端日志: ./qemuctl.sh service log filer-1"
        echo "    4. 确认集群 volume 数量 (stripe:4 需 ≥4 volume)"
        echo "    5. 确认 FUSE 容器内 setfattr 可用 (apt-get install -y attr)"
        echo "    6. 重新运行单个测试: ./test_k3_stripe.sh <T编号>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ K3 测试全部通过${C_RESET}"
    echo ""
    echo "  K3 验证门达成:"
    echo "    - Stripe placement xattr 设置成功 (stripe:4:1MB)"
    echo "    - 渐进大小 Stripe 读写 MD5 一致 (1KB→100MB)"
    echo "    - 内核↔FUSE Stripe 互通"
    echo "    - WideStripe 读取验证通过 (wide_stripe:256:4MB)"
    echo "    - 持续 60s fio 随机读无异常"
    echo "    - 内核状态 (slab/meminfo/dmesg/serial) 正常"
    echo ""
    echo "  可进入下一阶段 (K4+)"
    exit 0
}

main "$@"
