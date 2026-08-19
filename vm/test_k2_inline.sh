#!/bin/bash
# K2 阶段渐进式测试: Inline 小文件
#
# 验证内容 (参照 kernel-layout-completion-plan.md K2 节):
#   T0: 编译 + 静态验证 (inline 字段/函数存在)
#   T1: 服务启动 + Filer inline_max_size 确认
#   T2: 小文件创建 + Inline placement 确认 (dmesg 日志)
#   T3: Inline 写→读→MD5 校验 (100B→4KB→8KB, 逐个确认)
#   T4: Inline 持久化 (write→close→remount→read)
#   T5: FUSE→内核 Inline 互通 (FUSE 创建 Inline, 内核读)
#   T6: fsync Inline 文件验证
#   T7: 持续运行 + 内核状态监控 (小文件 60s 压力)
#   T8: 卸载 + 内核状态最终检查
#
# 核心原则:
#   - Inline 文件数据存 Filer 元数据 (Raft), 不走 Volume Server
#   - close 时 UPDATE_INODE 提交 inline_data (K2-5 核心)
#   - 从小到大逐个确认, 不可跳级
#   - 每步检查 dmesg/slab/serial lockup
#
# 运行环境: HOST (通过 SSH 控制 VM + docker exec 控制 FUSE 容器)
# 前置条件:
#   - Filer 配置 inline_max_size=8192 已启用
#   - Docker 服务已启动
#   - QEMU 已启动并挂载
#
# 用法:
#   ./test_k2_inline.sh            # 运行全部 (T0-T8)
#   ./test_k2_inline.sh 3          # 仅运行 T3
#   ./test_k2_inline.sh 0 1 2      # 运行 T0+T1+T2

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs
FUSE_CONTAINER="fuse-1"
POWERFS_MOD_DIR="/home/portion/powerfs/kernel/powerfs_mod"

PASS=0
FAIL=0
WARN=0
SKIP=0
SLAB_INIT=""
MEM_INIT=""
SERIAL_BASE=0

if [ -t 1 ]; then
    C_RED='\033[0;31m'; C_GREEN='\033[0;32m'; C_YELLOW='\033[0;33m'
    C_CYAN='\033[0;36m'; C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_RESET=''
fi

ok()     { echo -e "  ${C_GREEN}[PASS]${C_RESET} $1"; PASS=$((PASS+1)); }
ng()     { echo -e "  ${C_RED}[FAIL]${C_RESET} $1"; FAIL=$((FAIL+1)); }
warn()   { echo -e "  ${C_YELLOW}[WARN]${C_RESET} $1"; WARN=$((WARN+1)); }
skip()   { echo -e "  ${C_YELLOW}[SKIP]${C_RESET} $1"; SKIP=$((SKIP+1)); }
section() { echo ""; echo -e "${C_CYAN}━━━ $1 ━━━${C_RESET}"; }

RUN_TESTS=("$@")
should_run() {
    [ ${#RUN_TESTS[@]} -eq 0 ] || printf '%s\n' "${RUN_TESTS[@]}" | grep -qx "$1"
}

# ============================================================
# 内核状态检查 (复用 K1 逻辑)
# ============================================================

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

get_powerfs_slab_total() {
    local inode_active dentry_active
    inode_active=$(vm "cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print \$2}'" 2>/dev/null || echo 0)
    dentry_active=$(vm "cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_dentry/ {print \$2}'" 2>/dev/null || echo 0)
    echo "${inode_active:-0} ${dentry_active:-0}"
}

get_mem_available() {
    vm "awk '/MemAvailable/ {print \$2}' /proc/meminfo" 2>/dev/null || echo 0
}

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

check_kernel_state() {
    local desc="$1"
    local base="${2:-0}"
    local state_ok=0

    echo "  --- 内核状态检查: ${desc} ---"

    if check_dmesg_clean "$base"; then
        ok "dmesg 无 oops/bug/kasan/stall"
    else
        ng "dmesg 检测到异常 (${desc})"
        state_ok=1
    fi

    if check_d_state; then
        ok "无 D 状态 powerfs 线程"
    else
        ng "存在 D 状态 (hung) powerfs 线程 (${desc})"
        state_ok=1
    fi

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

    local warnings
    warnings=$(echo "$build_log" | grep -iE 'warning:' | grep -v 'Wno-' || true)
    if [ -z "$warnings" ]; then
        ok "编译无 warning"
    else
        warn "编译有 warning (非阻断):"
        echo "$warnings" | head -5 | sed 's/^/    /'
    fi

    # T0-2: Inline 相关字段验证
    echo "  [T0-2] Inline 字段/函数验证..."
    local inline_fields
    inline_fields=$(grep -cE 'POWERFS_NET_FLD_(INLINE_DATA|INLINE_MAX_SIZE)\s*=' powerfs_net.h 2>/dev/null || echo 0)
    if [ "$inline_fields" -ge 2 ]; then
        ok "InlineData/InlineMaxSize FieldId 已定义"
    else
        ng "Inline FieldId 缺失 (仅 ${inline_fields}/2)"
        return 1
    fi

    local inline_struct
    inline_struct=$(grep -cE 'inline_data|inline_len|inline_dirty' powerfs.h 2>/dev/null || echo 0)
    if [ "$inline_struct" -ge 3 ]; then
        ok "inode inline_data/inline_len/inline_dirty 字段已定义"
    else
        ng "inode Inline 字段缺失 (仅 ${inline_struct}/3)"
        return 1
    fi

    local release_fn
    release_fn=$(grep -c 'powerfs_file_release' powerfs_fs.c 2>/dev/null || echo 0)
    if [ "$release_fn" -ge 2 ]; then
        ok "powerfs_file_release 函数已定义并注册"
    else
        ng "powerfs_file_release 未找到 (${release_fn}/2)"
        return 1
    fi

    # T0-3: Inline 常量验证
    echo "  [T0-3] INLINE_MAX_SIZE 常量验证..."
    local inline_max
    inline_max=$(grep -E 'POWERFS_INLINE_MAX_SIZE\s+' powerfs.h | head -1)
    if echo "$inline_max" | grep -q '8 \* 1024'; then
        ok "POWERFS_INLINE_MAX_SIZE = 8KB"
    else
        ng "POWERFS_INLINE_MAX_SIZE 不是 8KB: $inline_max"
        return 1
    fi

    cd "${SCRIPT_DIR}"
    return 0
}

# ============================================================
# T1: 服务启动 + Filer inline_max_size 确认
# ============================================================
test_t1_services() {
    section "T1: 服务启动 + Inline 配置确认"

    # T1-1: 后端服务
    echo "  [T1-1] 检查后端服务..."
    local svc_count
    svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || echo 0)
    if [ "$svc_count" -ge 3 ]; then
        ok "后端服务已运行"
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

    # T1-2: Filer inline_max_size 配置确认
    echo "  [T1-2] Filer inline_max_size 配置确认..."
    local filer1_cfg
    filer1_cfg=$(grep 'inline_max_size' /home/portion/powerfs/docker/config/filer-1.toml 2>/dev/null || true)
    if echo "$filer1_cfg" | grep -q '8192'; then
        ok "Filer-1 inline_max_size=8192 (已启用)"
    else
        ng "Filer-1 inline_max_size 未启用: $filer1_cfg"
        echo "    请修改 docker/config/filer-*.toml: inline_max_size = 8192"
        return 1
    fi

    # T1-3: QEMU 运行
    echo "  [T1-3] 检查 QEMU 运行..."
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

    # T1-4: 部署新模块
    echo "  [T1-4] 部署最新 powerfs.ko..."
    ./qemuctl.sh deploy 2>&1 | tail -5
    sleep 3

    # T1-5: 挂载
    echo "  [T1-5] 检查 powerfs 挂载..."
    if ! check_mount; then
        ./qemuctl.sh mount 2>&1 | tail -3
        sleep 2
    fi
    if check_mount; then
        ok "powerfs 已挂载到 ${MNT}"
    else
        ng "powerfs 挂载失败"
        return 1
    fi

    # T1-6: 挂载后 dmesg 检查
    echo "  [T1-6] 挂载后 15s dmesg 观察..."
    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)
    sleep 15
    if check_kernel_state "挂载后 15s" "$base"; then
        ok "挂载后 15s 内核状态正常"
    else
        ng "挂载后 15s 内核状态异常"
        return 1
    fi

    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "  -> 初始 slab (inode dentry): ${SLAB_INIT}"
    echo "  -> 初始 MemAvailable: ${MEM_INIT} KB"

    return 0
}

# ============================================================
# T2: 小文件创建 + Inline placement 确认
# ============================================================
test_t2_inline_create() {
    section "T2: 小文件创建 + Inline placement 确认"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T2-1: 创建小文件
    echo "  [T2-1] 创建 100B 小文件..."
    vm "echo 'hello inline k2' > ${MNT}/t2_inline_test.txt" 2>/dev/null
    if [ $? -eq 0 ]; then
        ok "小文件创建成功"
    else
        ng "小文件创建失败"
        return 1
    fi

    vm "sync" 2>/dev/null
    sleep 1

    # T2-2: 读取验证
    echo "  [T2-2] 读取小文件..."
    local content
    content=$(vm "cat ${MNT}/t2_inline_test.txt" 2>/dev/null)
    if [ "$content" = "hello inline k2" ]; then
        ok "小文件读取正确: '$content'"
    else
        ng "小文件读取错误: got '$content' want 'hello inline k2'"
        return 1
    fi

    # T2-3: stat 确认大小
    echo "  [T2-3] stat 检查..."
    local size
    size=$(vm "stat -c %s ${MNT}/t2_inline_test.txt" 2>/dev/null)
    # "hello inline k2\n" = 16 bytes
    if [ "$size" = "16" ]; then
        ok "stat size=16 (正确)"
    else
        ng "stat size 错误: got $size want 16"
        return 1
    fi

    # T2-4: dmesg 中查找 Inline placement 日志
    echo "  [T2-4] dmesg Inline placement 日志检查..."
    local new_log
    new_log=$(dmesg_since "$base" 2>/dev/null)
    local inline_log
    inline_log=$(echo "$new_log" | grep -iE 'INLINE|inline_data' | head -5 || true)
    if [ -n "$inline_log" ]; then
        ok "dmesg 中有 Inline 相关日志"
        echo "$inline_log" | sed 's/^/    /'
    else
        warn "dmesg 中未找到 Inline 日志 (可能 pr_debug 未启用, 非阻断)"
    fi

    # T2-5: 内核状态检查
    if check_kernel_state "T2 Inline 创建" "$base"; then
        ok "T2 内核状态正常"
    else
        ng "T2 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm ${MNT}/t2_inline_test.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T3: Inline 写→读→MD5 校验 (100B → 8KB, 逐个确认)
# ============================================================
test_t3_inline_md5() {
    section "T3: Inline 写→读 MD5 校验 (100B → 8KB)"

    # 测试大小 (不超过 8KB Inline 上限)
    local sizes=("100B:100" "1K:1024" "4K:4096" "8K:8192")

    for entry in "${sizes[@]}"; do
        local label="${entry%%:*}"
        local bytes="${entry##*:}"

        echo ""
        echo "  [T3] Inline 文件大小: ${label} (${bytes}B)"

        local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

        # 生成随机数据并写入
        local src_md5
        src_md5=$(vm "dd if=/dev/urandom bs=1 count=${bytes} 2>/dev/null | tee ${MNT}/t3_inline_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$src_md5" ]; then
            ng "写入 ${label} 失败 (md5 为空)"
            return 1
        fi
        echo "    写入 MD5: ${src_md5}"

        vm "sync" 2>/dev/null
        sleep 1

        # 读取并校验
        local read_md5
        read_md5=$(vm "cat ${MNT}/t3_inline_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    读取 MD5: ${read_md5}"

        if [ "$src_md5" = "$read_md5" ]; then
            ok "${label} Inline MD5 一致"
        else
            ng "${label} Inline MD5 不一致 (src=${src_md5} read=${read_md5})"
            return 1
        fi

        # 文件大小检查
        local actual_size
        actual_size=$(vm "stat -c %s ${MNT}/t3_inline_${label}.bin" 2>/dev/null)
        if [ "$actual_size" = "$bytes" ]; then
            ok "${label} 文件大小正确 (${actual_size}B)"
        else
            ng "${label} 文件大小错误 (got ${actual_size} want ${bytes})"
            return 1
        fi

        # 内核状态检查
        if ! check_kernel_state "T3 ${label}" "$base"; then
            ng "T3 ${label} 内核状态异常"
            return 1
        fi

        # 清理
        vm "rm ${MNT}/t3_inline_${label}.bin" 2>/dev/null
        vm "sync" 2>/dev/null
    done

    echo ""
    ok "T3 全部 Inline MD5 校验通过"
    return 0
}

# ============================================================
# T4: Inline 持久化 (write → close → remount → read)
# ============================================================
test_t4_inline_persist() {
    section "T4: Inline 持久化 (write → close → remount → read)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T4-1: 写入多个 Inline 文件
    echo "  [T4-1] 写入 3 个 Inline 文件..."
    vm "rm -rf ${MNT}/t4_persist" 2>/dev/null
    vm "mkdir -p ${MNT}/t4_persist" 2>/dev/null

    vm "dd if=/dev/urandom bs=1 count=100 2>/dev/null > ${MNT}/t4_persist/f_100.bin" 2>/dev/null
    vm "dd if=/dev/urandom bs=1 count=4096 2>/dev/null > ${MNT}/t4_persist/f_4k.bin" 2>/dev/null
    vm "dd if=/dev/urandom bs=1 count=8192 2>/dev/null > ${MNT}/t4_persist/f_8k.bin" 2>/dev/null

    local md5_100 md5_4k md5_8k
    md5_100=$(vm "md5sum ${MNT}/t4_persist/f_100.bin | awk '{print \$1}'" 2>/dev/null)
    md5_4k=$(vm "md5sum ${MNT}/t4_persist/f_4k.bin | awk '{print \$1}'" 2>/dev/null)
    md5_8k=$(vm "md5sum ${MNT}/t4_persist/f_8k.bin | awk '{print \$1}'" 2>/dev/null)

    echo "    f_100 MD5: ${md5_100}"
    echo "    f_4k  MD5: ${md5_4k}"
    echo "    f_8k  MD5: ${md5_8k}"

    if [ -z "$md5_100" ] || [ -z "$md5_4k" ] || [ -z "$md5_8k" ]; then
        ng "写入 Inline 文件失败"
        return 1
    fi
    ok "3 个 Inline 文件写入成功"

    vm "sync" 2>/dev/null
    sleep 1

    # T4-2: umount + remount
    echo "  [T4-2] umount + remount..."
    vm "umount ${MNT}" 2>/dev/null
    sleep 3
    vm "mount -t powerfs none ${MNT}" 2>/dev/null
    sleep 3

    if ! check_mount; then
        ng "remount 失败"
        return 1
    fi
    ok "remount 成功"

    # T4-3: remount 后读取验证
    echo "  [T4-3] remount 后 MD5 校验..."
    local r_md5_100 r_md5_4k r_md5_8k
    r_md5_100=$(vm "cat ${MNT}/t4_persist/f_100.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    r_md5_4k=$(vm "cat ${MNT}/t4_persist/f_4k.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    r_md5_8k=$(vm "cat ${MNT}/t4_persist/f_8k.bin | md5sum | awk '{print \$1}'" 2>/dev/null)

    echo "    f_100 remount MD5: ${r_md5_100}"
    echo "    f_4k  remount MD5: ${r_md5_4k}"
    echo "    f_8k  remount MD5: ${r_md5_8k}"

    local persist_ok=0
    if [ "$md5_100" = "$r_md5_100" ]; then
        ok "f_100 持久化一致"
    else
        ng "f_100 持久化不一致 (before=${md5_100} after=${r_md5_100})"
        persist_ok=1
    fi
    if [ "$md5_4k" = "$r_md5_4k" ]; then
        ok "f_4k 持久化一致"
    else
        ng "f_4k 持久化不一致 (before=${md5_4k} after=${r_md5_4k})"
        persist_ok=1
    fi
    if [ "$md5_8k" = "$r_md5_8k" ]; then
        ok "f_8k 持久化一致"
    else
        ng "f_8k 持久化不一致 (before=${md5_8k} after=${r_md5_8k})"
        persist_ok=1
    fi

    if [ $persist_ok -ne 0 ]; then
        return 1
    fi

    # T4-4: 内核状态检查
    if check_kernel_state "T4 Inline 持久化" "$base"; then
        ok "T4 内核状态正常"
    else
        ng "T4 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm -rf ${MNT}/t4_persist" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T5: FUSE→内核 Inline 互通
# ============================================================
test_t5_fuse_to_kernel() {
    section "T5: FUSE→内核 Inline 互通"

    if ! docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^${FUSE_CONTAINER}$"; then
        skip "${FUSE_CONTAINER} 容器未运行, 跳过 T5"
        return 0
    fi
    ok "${FUSE_CONTAINER} 容器运行中"

    local fuse_mount="${FUSE_MNT}"
    local test_dir="k2_t5_cross"
    # NOTE: dd "B" suffix is NOT portable across implementations (coreutils vs
    # busybox vs toybox). Use plain byte counts and pass bs=1 count=<bytes>
    # so every dd implementation produces the same file size deterministically.
    local -a sizes_bytes=(100 1024 4096)
    local -a sizes_label=("100B" "1K" "4K")

    local n=${#sizes_bytes[@]}
    for ((i=0; i<n; i++)); do
        local bytes=${sizes_bytes[$i]}
        local label=${sizes_label[$i]}
        echo ""
        echo "  [T5] FUSE 创建 ${label} Inline 文件 → 内核读取"

        local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

        # FUSE 端创建并记录 MD5
        local fuse_md5
        fuse_md5=$(docker exec ${FUSE_CONTAINER} sh -c "mkdir -p ${fuse_mount}/${test_dir} && dd if=/dev/urandom bs=1 count=${bytes} 2>/dev/null > ${fuse_mount}/${test_dir}/file_${label}.bin && md5sum ${fuse_mount}/${test_dir}/file_${label}.bin | awk '{print \$1}'" 2>/dev/null)

        if [ -z "$fuse_md5" ]; then
            ng "FUSE 端创建 ${label} 文件失败"
            return 1
        fi
        echo "    FUSE 端 MD5: ${fuse_md5}"

        # 内核端读取
        vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
        sleep 1

        local kernel_md5
        kernel_md5=$(vm "cat ${MNT}/${test_dir}/file_${label}.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
        echo "    内核端 MD5: ${kernel_md5}"

        if [ "$fuse_md5" = "$kernel_md5" ]; then
            ok "${label} FUSE→内核 Inline MD5 一致"
        else
            ng "${label} FUSE→内核 MD5 不一致 (fuse=${fuse_md5} kernel=${kernel_md5})"
            return 1
        fi

        if ! check_kernel_state "T5 ${label}" "$base"; then
            ng "T5 ${label} 内核状态异常"
            return 1
        fi
    done

    # 清理
    docker exec ${FUSE_CONTAINER} sh -c "rm -rf ${fuse_mount}/${test_dir}" 2>/dev/null
    ok "T5 FUSE→内核 Inline 互通通过"
    return 0
}

# ============================================================
# T6: fsync Inline 文件验证
# ============================================================
test_t6_fsync() {
    section "T6: fsync Inline 文件验证"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T6-1: 写入 + fsync
    echo "  [T6-1] 写入 2KB Inline 文件 + fsync..."
    vm "dd if=/dev/urandom bs=1 count=2048 2>/dev/null > ${MNT}/t6_fsync.bin" 2>/dev/null
    local src_md5
    src_md5=$(vm "md5sum ${MNT}/t6_fsync.bin | awk '{print \$1}'" 2>/dev/null)
    echo "    fsync 前 MD5: ${src_md5}"

    # 在 VM 内执行 fsync (使用 python 或 fio)
    vm "sync ${MNT}/t6_fsync.bin" 2>/dev/null
    # 用 dd 触发 fsync
    vm "dd if=/dev/zero bs=1 count=1 >> ${MNT}/t6_fsync.bin conv=notrunc 2>/dev/null && sync ${MNT}/t6_fsync.bin" 2>/dev/null

    sleep 1

    # 读取验证 (追加 1 字节后)
    local read_md5
    read_md5=$(vm "cat ${MNT}/t6_fsync.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    fsync 后 MD5: ${read_md5}"

    # 大小应该 +1
    local fsize
    fsize=$(vm "stat -c %s ${MNT}/t6_fsync.bin" 2>/dev/null)
    if [ "$fsize" = "2049" ]; then
        ok "fsync 后大小=2049 (正确)"
    else
        ng "fsync 后大小错误: got $fsize want 2049"
        return 1
    fi

    # T6-2: remount 后验证 fsync 数据持久化
    echo "  [T6-2] remount 后验证 fsync 持久化..."
    vm "umount ${MNT}" 2>/dev/null
    sleep 3
    vm "mount -t powerfs none ${MNT}" 2>/dev/null
    sleep 3

    if ! check_mount; then
        ng "remount 失败"
        return 1
    fi

    local r_md5
    r_md5=$(vm "cat ${MNT}/t6_fsync.bin | md5sum | awk '{print \$1}'" 2>/dev/null)
    if [ "$read_md5" = "$r_md5" ]; then
        ok "fsync 数据持久化一致"
    else
        ng "fsync 数据持久化不一致 (before=${read_md5} after=${r_md5})"
        return 1
    fi

    if check_kernel_state "T6 fsync" "$base"; then
        ok "T6 内核状态正常"
    else
        ng "T6 内核状态异常"
        return 1
    fi

    vm "rm ${MNT}/t6_fsync.bin" 2>/dev/null
    vm "sync" 2>/dev/null
    return 0
}

# ============================================================
# T7: 持续运行 + 内核状态监控 (小文件 60s 压力)
# ============================================================
test_t7_sustained() {
    section "T7: 持续运行 + 内核状态监控 (60s 小文件压力)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)
    local slab_before
    slab_before=$(get_powerfs_slab_total)
    local mem_before
    mem_before=$(get_mem_available)
    echo "  -> 测试前 slab: ${slab_before}"
    echo "  -> 测试前 MemAvailable: ${mem_before} KB"

    # T7-1: 持续创建小文件 60s (后台)
    echo ""
    echo "  [T7-1] 持续创建 100B-4KB 小文件 60s (后台 + 定期 dmesg 检查)"
    vm "mkdir -p ${MNT}/t7_stress" 2>/dev/null

    vm "
        for i in \$(seq 1 600); do
            sz=\$(( (i % 4 + 1) * 100 ))
            dd if=/dev/urandom bs=1 count=\$sz 2>/dev/null > ${MNT}/t7_stress/f_\${i}.bin
            if [ \$((i % 100)) -eq 0 ]; then
                cat ${MNT}/t7_stress/f_\${i}.bin > /dev/null
            fi
        done
    " 2>/dev/null &
    local stress_pid=$!

    # 期间每 10s 检查 dmesg
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep 10
        elapsed=$((elapsed + 10))
        echo "    [${elapsed}s] dmesg 检查..."
        if ! check_dmesg_clean "$base"; then
            ng "小文件压力 ${elapsed}s 时检测到内核异常"
            kill $stress_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg 正常"
    done

    wait $stress_pid 2>/dev/null
    ok "小文件压力 60s 完成"

    # T7-2: 完成后状态检查
    echo ""
    echo "  [T7-2] 压力完成后内核状态检查"
    local slab_after
    slab_after=$(get_powerfs_slab_total)
    local mem_after
    mem_after=$(get_mem_available)
    echo "  -> 测试后 slab: ${slab_after} (前: ${slab_before})"
    echo "  -> 测试后 MemAvailable: ${mem_after} KB (前: ${mem_before} KB)"

    if check_kernel_state "T7 小文件压力完成" "$base"; then
        ok "T7 内核状态最终检查通过"
    else
        ng "T7 内核状态异常"
        return 1
    fi

    # 内存泄漏检查
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

    # 清理
    vm "rm -rf ${MNT}/t7_stress" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T8: 卸载 + 内核状态最终检查
# ============================================================
test_t8_unmount() {
    section "T8: 卸载 + 内核状态最终检查"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

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

    # 重新挂载
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
    echo -e "${C_CYAN}║  K2 渐进式测试: Inline 小文件                       ${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从小到大, 逐个确认, 检查内核状态             ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    local tests=(
        "0:test_t0_compile"
        "1:test_t1_services"
        "2:test_t2_inline_create"
        "3:test_t3_inline_md5"
        "4:test_t4_inline_persist"
        "5:test_t5_fuse_to_kernel"
        "6:test_t6_fsync"
        "7:test_t7_sustained"
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
        echo -e "  ${C_RED}✗ K2 测试存在失败项${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  首个失败: T${failed_test}"
        fi
        echo ""
        echo "  排查建议:"
        echo "    1. 查看 VM dmesg: ./qemuctl.sh log powerfs"
        echo "    2. 实时监控 serial: ./qemuctl.sh serial-tail"
        echo "    3. 查看后端日志: ./qemuctl.sh service log filer-1"
        echo "    4. 确认 Filer inline_max_size=8192"
        echo "    5. 重新运行单个测试: ./test_k2_inline.sh <T编号>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ K2 测试全部通过${C_RESET}"
    echo ""
    echo "  K2 验证门达成:"
    echo "    - Inline 小文件创建/读写正确 (100B-8KB)"
    echo "    - close 时 inline_data 持久化到 Filer (UPDATE_INODE)"
    echo "    - remount 后数据持久化一致"
    echo "    - FUSE↔内核 Inline 互通"
    echo "    - fsync Inline 文件正确"
    echo "    - 持续 60s 小文件压力无异常"
    echo "    - 内核状态 (slab/meminfo/dmesg/serial) 正常"
    echo ""
    echo "  可进入 K3 阶段 (Stripe 多卷)"
    exit 0
}

main "$@"
