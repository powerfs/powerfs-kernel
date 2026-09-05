#!/bin/bash
# K2-content-aware: Inline → Flat/Stripe 内容感知迁移测试
#
# 验证 powerfs_migrate_inline_out() 的内容感知决策:
#   - 二进制内容 (urandom/ELF) → Stripe 多卷迁移
#   - 文本内容 (重复行文本)    → Flat 单卷迁移
#
# 测试覆盖 (T0-T9, 从简单到复杂):
#   T0: 编译 + 静态验证 (函数/字段/常量存在)
#   T1: 服务启动 + 模块部署 + 挂载
#   T2: 12KB 二进制 (urandom) → Stripe 迁移 + dmesg 检查
#   T3: 12KB 文本 → Flat 迁移 + dmesg 检查
#   T4: 真实 ELF 二进制 (> 8KB) → Stripe + 可执行验证
#   T5: 20KB 配置文本 → Flat + 读回校验
#   T6: remount 持久化 (Stripe + Flat 文件 MD5 校验)
#   T7: fio 4K randwrite (可选, 若 VM 内有 fio)
#   T8: 60s 混合压力 (二进制+文本交替, 定期 dmesg)
#   T9: 卸载 + 内核状态最终检查
#
# 核心检查点:
#   - dmesg "MIGRATE ino=X → Stripe migration (content-aware)"
#   - dmesg "MIGRATE ino=X → Flat migration (content-aware)"
#   - dmesg "MIGRATE ino=X → Stripe done" / "→ Flat done"
#   - dmesg 无 BUG/Oops/KASAN/lockup
#   - 数据 MD5 一致 + remount 持久化
#
# 运行环境: HOST (通过 SSH 控制 VM)
# 前置条件:
#   - Docker 服务已启动 (master/volume/filer)
#   - QEMU 可启动 (./qemuctl.sh deploy)
#
# 用法:
#   ./test_k2_content_aware.sh            # 运行全部 T0-T9
#   ./test_k2_content_aware.sh 2          # 仅运行 T2
#   ./test_k2_content_aware.sh 2 3 4      # 运行 T2+T3+T4

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

# VM 内挂载点 (与 qemuctl.sh / fault_injection.sh POWERFS_MOUNTPOINT 对齐)
MNT="${POWERFS_MOUNTPOINT:-/mnt/powerfs}"
FUSE_MNT="/mnt/powerfs"
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
# 内核状态检查 (复用 K2 逻辑)
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

# 检查 dmesg 中是否包含指定迁移模式日志
# 参数: $1 = base line, $2 = 期望模式 (Stripe|Flat), $3 = 文件名 (用于日志)
check_migrate_log() {
    local base="$1"
    local expected_mode="$2"
    local fname="$3"
    local new_log
    new_log=$(dmesg_since "$base" 2>/dev/null)

    # 查找 "MIGRATE ino=X → <mode> migration (content-aware)" 触发日志
    local trigger_log
    trigger_log=$(echo "$new_log" | grep -E "MIGRATE ino=[0-9]+ → ${expected_mode} migration" | tail -1 || true)
    if [ -n "$trigger_log" ]; then
        ok "[${fname}] 触发 ${expected_mode} 迁移: ${trigger_log##*powerfs: }"
    else
        ng "[${fname}] 未找到 ${expected_mode} migration 触发日志"
        # 输出所有 MIGRATE 日志辅助排查
        echo "$new_log" | grep "MIGRATE" | tail -5 | sed 's/^/    /'
        return 1
    fi

    # 查找 "MIGRATE ino=X → <mode> done" 完成日志
    local done_log
    done_log=$(echo "$new_log" | grep -E "MIGRATE ino=[0-9]+ → ${expected_mode} done" | tail -1 || true)
    if [ -n "$done_log" ]; then
        ok "[${fname}] ${expected_mode} 迁移完成"
    else
        ng "[${fname}] 未找到 ${expected_mode} done 完成日志"
        return 1
    fi

    return 0
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

    echo "  [T0-2] 内容感知函数/字段验证..."
    # powerfs_migrate_inline_out (重命名后)
    local migrate_fn
    migrate_fn=$(grep -c 'powerfs_migrate_inline_out' powerfs_addr.c 2>/dev/null || echo 0)
    if [ "$migrate_fn" -ge 2 ]; then
        ok "powerfs_migrate_inline_out 函数已定义并调用 (${migrate_fn} 处)"
    else
        ng "powerfs_migrate_inline_out 未找到 (${migrate_fn}/2)"
        return 1
    fi

    # powerfs_detect_content_binary
    local detect_fn
    detect_fn=$(grep -c 'powerfs_detect_content_binary' powerfs_addr.c 2>/dev/null || echo 0)
    if [ "$detect_fn" -ge 2 ]; then
        ok "powerfs_detect_content_binary 已定义并调用 (${detect_fn} 处)"
    else
        ng "powerfs_detect_content_binary 未找到 (${detect_fn}/2)"
        return 1
    fi

    # POWERFS_NET_FLD_DESIRED_MODE = 0xD6
    local desired_field
    desired_field=$(grep -c 'POWERFS_NET_FLD_DESIRED_MODE\s*=\s*0xD6' powerfs_net.h 2>/dev/null || echo 0)
    if [ "$desired_field" -ge 1 ]; then
        ok "POWERFS_NET_FLD_DESIRED_MODE = 0xD6 已定义"
    else
        ng "POWERFS_NET_FLD_DESIRED_MODE 未定义"
        return 1
    fi

    # powerfs_migrate_alloc_result 结构体
    local result_struct
    result_struct=$(grep -c 'struct powerfs_migrate_alloc_result' powerfs_net.h 2>/dev/null || echo 0)
    if [ "$result_struct" -ge 1 ]; then
        ok "powerfs_migrate_alloc_result 结构体已定义"
    else
        ng "powerfs_migrate_alloc_result 结构体未定义"
        return 1
    fi

    # 旧的 powerfs_migrate_inline_to_flat 应该已被替换
    # 注意: grep -c 无匹配时退出码=1 但已输出 "0", 用 || true 避免追加第二个 0
    local old_fn
    old_fn=$(grep -c 'powerfs_migrate_inline_to_flat' powerfs_addr.c 2>/dev/null || true)
    old_fn=${old_fn:-0}
    if [ "$old_fn" -eq 0 ]; then
        ok "旧函数名 powerfs_migrate_inline_to_flat 已移除"
    else
        ng "旧函数名 powerfs_migrate_inline_to_flat 仍存在 (${old_fn} 处)"
        return 1
    fi

    cd "${SCRIPT_DIR}"
    return 0
}

# ============================================================
# T1: 服务启动 + 模块部署 + 挂载
# ============================================================
test_t1_services() {
    section "T1: 服务启动 + 模块部署 + 挂载"

    # T1-1: 后端服务 (master 等 120s peers 超时, 需要长等待)
    echo "  [T1-1] 检查后端服务..."
    local svc_count
    svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || true)
    svc_count=${svc_count:-0}
    if [ "$svc_count" -ge 3 ]; then
        ok "后端服务已运行"
    else
        warn "后端服务未完全启动 (${svc_count}/3), 尝试启动..."
        # 先启动 redis + master
        ./qemuctl.sh service start 2>&1 | tail -5 || true
        # Master 单节点需要等 120s peers 超时; 检查 master 健康最多 180s
        echo "    等待 master 健康 (最多 180s, 单节点需等 peers 超时)..."
        local master_healthy=""
        for i in $(seq 1 36); do
            master_healthy=$(docker inspect master-1 --format '{{.State.Health.Status}}' 2>/dev/null || echo "")
            if [ "$master_healthy" = "healthy" ]; then
                echo "    master-1 healthy (${i}x5s)"
                break
            fi
            sleep 5
        done
        if [ "$master_healthy" != "healthy" ]; then
            ng "master-1 健康检查失败 (180s 超时)"
            return 1
        fi
        # master 健康后, 用 --no-deps 启动 volume + filer (跳过 depends_on)
        echo "    启动 volume-1 + filer-1 (--no-deps)..."
        (cd /home/portion/powerfs/docker && docker compose -f docker-compose.yml up -d --no-deps volume-1 filer-1 2>&1) | tail -5
        # 等待 volume + filer 健康
        for i in $(seq 1 24); do
            local vol_h fil_h
            vol_h=$(docker inspect volume-1 --format '{{.State.Health.Status}}' 2>/dev/null || echo "")
            fil_h=$(docker inspect filer-1 --format '{{.State.Health.Status}}' 2>/dev/null || echo "")
            if [ "$vol_h" = "healthy" ] && [ "$fil_h" = "healthy" ]; then
                echo "    volume-1 + filer-1 healthy (${i}x5s)"
                break
            fi
            sleep 5
        done
        svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || true)
        svc_count=${svc_count:-0}
        if [ "$svc_count" -ge 3 ]; then
            ok "后端服务已启动"
        else
            ng "后端服务启动失败"
            return 1
        fi
    fi

    # T1-2: QEMU 运行
    echo "  [T1-2] 检查 QEMU 运行..."
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

    # T1-3: 部署新模块
    echo "  [T1-3] 部署最新 powerfs.ko..."
    ./qemuctl.sh deploy 2>&1 | tail -5
    sleep 3

    # T1-4: 挂载
    echo "  [T1-4] 检查 powerfs 挂载..."
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

    # T1-5: 挂载后 dmesg 检查
    echo "  [T1-5] 挂载后 15s dmesg 观察..."
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

    # 清理可能残留的测试目录
    vm "rm -rf ${MNT}/k2_ca_*" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T2: 12KB 二进制 (urandom) → Stripe 迁移
# ============================================================
test_t2_binary_stripe() {
    section "T2: 12KB 二进制 (urandom) → Stripe 迁移"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local fname="k2_ca_t2_binary.bin"
    local bytes=12288  # 12KB > 8KB inline 阈值

    echo "  [T2-1] 生成 ${bytes}B 二进制 (urandom) 并写入..."
    local src_md5
    src_md5=$(vm "dd if=/dev/urandom bs=1 count=${bytes} 2>/dev/null | tee ${MNT}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)

    if [ -z "$src_md5" ]; then
        ng "写入 ${fname} 失败 (md5 为空)"
        return 1
    fi
    echo "    写入 MD5: ${src_md5}"
    ok "二进制文件写入成功"

    vm "sync" 2>/dev/null
    sleep 1

    # T2-2: dmesg 检查 Stripe 迁移
    echo "  [T2-2] dmesg Stripe 迁移日志检查..."
    check_migrate_log "$base" "Stripe" "${fname}" || return 1

    # T2-3: 文件大小检查
    echo "  [T2-3] stat 检查..."
    local actual_size
    actual_size=$(vm "stat -c %s ${MNT}/${fname}" 2>/dev/null)
    if [ "$actual_size" = "$bytes" ]; then
        ok "文件大小正确 (${actual_size}B)"
    else
        ng "文件大小错误 (got ${actual_size} want ${bytes})"
        return 1
    fi

    # T2-4: 读回 MD5 校验
    echo "  [T2-4] 读回 MD5 校验..."
    local read_md5
    read_md5=$(vm "cat ${MNT}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    读取 MD5: ${read_md5}"
    if [ "$src_md5" = "$read_md5" ]; then
        ok "二进制 MD5 一致 (Stripe 迁移后数据完整)"
    else
        ng "MD5 不一致 (src=${src_md5} read=${read_md5})"
        return 1
    fi

    # T2-5: 内核状态检查
    if check_kernel_state "T2 二进制 Stripe" "$base"; then
        ok "T2 内核状态正常"
    else
        ng "T2 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm ${MNT}/${fname}" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T3: 12KB 文本 → Flat 迁移
# ============================================================
test_t3_text_flat() {
    section "T3: 12KB 文本 → Flat 迁移"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local fname="k2_ca_t3_text.conf"
    local bytes=12288  # 12KB > 8KB inline 阈值

    echo "  [T3-1] 生成 ${bytes}B 文本并写入..."
    # 生成配置风格文本行 (printable ASCII), 重复填充到 12KB
    # 用 yes + head 组合, 然后 md5 + tee 写入
    local src_md5
    src_md5=$(vm "yes 'server.host=localhost\nserver.port=8080\ndebug=true\nmax_clients=100' | head -c ${bytes} | tee ${MNT}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)

    if [ -z "$src_md5" ]; then
        ng "写入 ${fname} 失败 (md5 为空)"
        return 1
    fi
    echo "    写入 MD5: ${src_md5}"
    ok "文本文件写入成功"

    vm "sync" 2>/dev/null
    sleep 1

    # T3-2: dmesg 检查 Flat 迁移
    echo "  [T3-2] dmesg Flat 迁移日志检查..."
    check_migrate_log "$base" "Flat" "${fname}" || return 1

    # T3-3: 文件大小检查
    echo "  [T3-3] stat 检查..."
    local actual_size
    actual_size=$(vm "stat -c %s ${MNT}/${fname}" 2>/dev/null)
    if [ "$actual_size" = "$bytes" ]; then
        ok "文件大小正确 (${actual_size}B)"
    else
        ng "文件大小错误 (got ${actual_size} want ${bytes})"
        return 1
    fi

    # T3-4: 读回 MD5 校验
    echo "  [T3-4] 读回 MD5 校验..."
    local read_md5
    read_md5=$(vm "cat ${MNT}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    读取 MD5: ${read_md5}"
    if [ "$src_md5" = "$read_md5" ]; then
        ok "文本 MD5 一致 (Flat 迁移后数据完整)"
    else
        ng "MD5 不一致 (src=${src_md5} read=${read_md5})"
        return 1
    fi

    # T3-5: 内核状态检查
    if check_kernel_state "T3 文本 Flat" "$base"; then
        ok "T3 内核状态正常"
    else
        ng "T3 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm ${MNT}/${fname}" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T4: 真实 ELF 二进制 → Stripe + 可执行验证
# ============================================================
test_t4_real_elf() {
    section "T4: 真实 ELF 二进制 → Stripe + 可执行验证"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T4-1: 寻找 VM 内 > 8KB 的 ELF 二进制
    echo "  [T4-1] 寻找 VM 内 > 8KB 的 ELF 二进制..."
    local src_bin
    src_bin=$(vm "ls -la /bin/busybox /init /usr/bin/sh 2>/dev/null | awk '\$5 > 8192 {print \$NF}' | head -1" 2>/dev/null)
    if [ -z "$src_bin" ]; then
        # fallback: 用 /bin/ls 或 /usr/bin/[ 之类
        src_bin=$(vm "for f in /bin/* /usr/bin/* /sbin/*; do [ -f \"\$f\" ] && sz=\$(stat -c %s \"\$f\" 2>/dev/null); [ \"\${sz:-0}\" -gt 8192 ] && echo \"\$f\" && break; done 2>/dev/null | head -1" 2>/dev/null)
    fi

    if [ -z "$src_bin" ]; then
        skip "T4: 未找到 > 8KB 的 ELF 二进制 (VM 环境精简), 跳过"
        return 0
    fi
    ok "源 ELF 二进制: ${src_bin}"

    local src_size
    src_size=$(vm "stat -c %s ${src_bin}" 2>/dev/null)
    local src_md5
    src_md5=$(vm "md5sum ${src_bin} | awk '{print \$1}'" 2>/dev/null)
    echo "    源大小: ${src_size}B, MD5: ${src_md5}"

    # T4-2: 复制到 powerfs
    echo "  [T4-2] 复制 ELF 到 powerfs..."
    local dst="${MNT}/k2_ca_t4_elf_copy"
    vm "cp ${src_bin} ${dst}" 2>/dev/null
    if [ $? -ne 0 ]; then
        ng "复制 ELF 失败"
        return 1
    fi
    ok "复制完成: ${dst}"

    vm "sync" 2>/dev/null
    sleep 1

    # T4-3: dmesg 检查 Stripe 迁移
    echo "  [T4-3] dmesg Stripe 迁移日志检查..."
    check_migrate_log "$base" "Stripe" "t4_elf_copy" || return 1

    # T4-4: MD5 校验
    echo "  [T4-4] MD5 校验..."
    local dst_md5
    dst_md5=$(vm "md5sum ${dst} | awk '{print \$1}'" 2>/dev/null)
    if [ "$src_md5" = "$dst_md5" ]; then
        ok "ELF MD5 一致"
    else
        ng "ELF MD5 不一致 (src=${src_md5} dst=${dst_md5})"
        return 1
    fi

    # T4-5: 可执行验证 (尝试运行 --help 或 --version)
    echo "  [T4-5] 可执行验证..."
    vm "chmod +x ${dst}" 2>/dev/null
    # 尝试 --help, 不强求成功 (有些二进制不支持 --help), 只要能 execve 不报 ENOSYS/ENOEXEC
    local exec_out
    exec_out=$(vm "${dst} --help 2>&1 | head -1" 2>/dev/null || vm "${dst} --version 2>&1 | head -1" 2>/dev/null || true)
    if [ -n "$exec_out" ]; then
        ok "ELF 可执行: ${exec_out}"
    else
        warn "ELF 执行无输出 (可能不支持 --help/--version), 检查 execve 不报错即可"
        # 用 file 命令确认是 ELF
        local file_out
        file_out=$(vm "file ${dst} 2>/dev/null || echo 'no file cmd'" 2>/dev/null)
        echo "    file: ${file_out}"
    fi

    # T4-6: 内核状态检查
    if check_kernel_state "T4 真实 ELF Stripe" "$base"; then
        ok "T4 内核状态正常"
    else
        ng "T4 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm ${dst}" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T5: 20KB 配置文本 → Flat + 读回校验
# ============================================================
test_t5_large_text() {
    section "T5: 20KB 配置文本 → Flat + 读回校验"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local fname="k2_ca_t5_large_config.conf"
    local bytes=20480  # 20KB > 8KB inline 阈值

    echo "  [T5-1] 生成 ${bytes}B 配置文本并写入..."
    local src_md5
    src_md5=$(vm "yes '# config line: host=localhost port=8080 user=admin pass=x debug=true verbose=2 max_conn=100 timeout=30' | head -c ${bytes} | tee ${MNT}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)

    if [ -z "$src_md5" ]; then
        ng "写入 ${fname} 失败"
        return 1
    fi
    echo "    写入 MD5: ${src_md5}"
    ok "配置文本写入成功"

    vm "sync" 2>/dev/null
    sleep 1

    # T5-2: dmesg 检查 Flat 迁移
    echo "  [T5-2] dmesg Flat 迁移日志检查..."
    check_migrate_log "$base" "Flat" "${fname}" || return 1

    # T5-3: 文件大小 + MD5 校验
    echo "  [T5-3] stat + MD5 校验..."
    local actual_size
    actual_size=$(vm "stat -c %s ${MNT}/${fname}" 2>/dev/null)
    if [ "$actual_size" = "$bytes" ]; then
        ok "文件大小正确 (${actual_size}B)"
    else
        ng "文件大小错误 (got ${actual_size} want ${bytes})"
        return 1
    fi

    local read_md5
    read_md5=$(vm "cat ${MNT}/${fname} | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    读取 MD5: ${read_md5}"
    if [ "$src_md5" = "$read_md5" ]; then
        ok "20KB 文本 MD5 一致"
    else
        ng "MD5 不一致 (src=${src_md5} read=${read_md5})"
        return 1
    fi

    # T5-4: 内容可读性 (前几行应为配置文本)
    echo "  [T5-4] 内容可读性验证..."
    local first_line
    first_line=$(vm "head -1 ${MNT}/${fname}" 2>/dev/null)
    if echo "$first_line" | grep -q '^# config line'; then
        ok "首行是配置文本: ${first_line:0:50}..."
    else
        ng "首行非预期配置文本: ${first_line}"
        return 1
    fi

    # T5-5: 内核状态检查
    if check_kernel_state "T5 20KB 文本 Flat" "$base"; then
        ok "T5 内核状态正常"
    else
        ng "T5 内核状态异常"
        return 1
    fi

    # 不清理, 留给 T6 remount 测试
    return 0
}

# ============================================================
# T6: remount 持久化 (Stripe + Flat 文件 MD5 校验)
# ============================================================
test_t6_remount_persist() {
    section "T6: remount 持久化 (Stripe + Flat)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T6-1: 写入 Stripe (二进制) + Flat (文本) 各一个
    echo "  [T6-1] 写入 Stripe (12KB binary) + Flat (12KB text)..."
    local bin_md5 txt_md5
    bin_md5=$(vm "dd if=/dev/urandom bs=1 count=12288 2>/dev/null | tee ${MNT}/k2_ca_t6_bin.dat | md5sum | awk '{print \$1}'" 2>/dev/null)
    txt_md5=$(vm "yes 'config_key=value_setting another_key=more_data' | head -c 12288 | tee ${MNT}/k2_ca_t6_txt.conf | md5sum | awk '{print \$1}'" 2>/dev/null)

    if [ -z "$bin_md5" ] || [ -z "$txt_md5" ]; then
        ng "T6 写入失败"
        return 1
    fi
    echo "    bin MD5: ${bin_md5}"
    echo "    txt MD5: ${txt_md5}"
    ok "两个文件写入成功"

    # 等待迁移完成
    vm "sync" 2>/dev/null
    sleep 2

    # T6-2: dmesg 检查两次迁移
    echo "  [T6-2] dmesg 迁移日志检查..."
    local new_log
    new_log=$(dmesg_since "$base" 2>/dev/null)
    local stripe_count flat_count
    stripe_count=$(echo "$new_log" | grep -cE "MIGRATE ino=[0-9]+ → Stripe migration" || echo 0)
    flat_count=$(echo "$new_log" | grep -cE "MIGRATE ino=[0-9]+ → Flat migration" || echo 0)
    if [ "$stripe_count" -ge 1 ] && [ "$flat_count" -ge 1 ]; then
        ok "检测到 Stripe (${stripe_count}) + Flat (${flat_count}) 迁移"
    else
        ng "迁移日志缺失 (stripe=${stripe_count} flat=${flat_count})"
        return 1
    fi

    # T6-3: umount + remount
    echo "  [T6-3] umount + remount..."
    vm "umount ${MNT}" 2>/dev/null
    sleep 3
    vm "mount -t powerfs none ${MNT}" 2>/dev/null
    sleep 3

    if ! check_mount; then
        ng "remount 失败"
        return 1
    fi
    ok "remount 成功"

    # T6-4: remount 后 MD5 校验
    echo "  [T6-4] remount 后 MD5 校验..."
    local r_bin_md5 r_txt_md5
    r_bin_md5=$(vm "cat ${MNT}/k2_ca_t6_bin.dat | md5sum | awk '{print \$1}'" 2>/dev/null)
    r_txt_md5=$(vm "cat ${MNT}/k2_ca_t6_txt.conf | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    bin remount MD5: ${r_bin_md5}"
    echo "    txt remount MD5: ${r_txt_md5}"

    local persist_ok=0
    if [ "$bin_md5" = "$r_bin_md5" ]; then
        ok "Stripe 文件 remount 持久化一致"
    else
        ng "Stripe 文件持久化不一致 (before=${bin_md5} after=${r_bin_md5})"
        persist_ok=1
    fi
    if [ "$txt_md5" = "$r_txt_md5" ]; then
        ok "Flat 文件 remount 持久化一致"
    else
        ng "Flat 文件持久化不一致 (before=${txt_md5} after=${r_txt_md5})"
        persist_ok=1
    fi
    if [ $persist_ok -ne 0 ]; then
        return 1
    fi

    # T6-5: 内核状态检查
    if check_kernel_state "T6 remount 持久化" "$base"; then
        ok "T6 内核状态正常"
    else
        ng "T6 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm ${MNT}/k2_ca_t6_bin.dat ${MNT}/k2_ca_t6_txt.conf" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T7: fio 4K randwrite (可选, 若 VM 内有 fio)
# ============================================================
test_t7_fio_randwrite() {
    section "T7: fio 4K randwrite (可选)"

    # 检查 VM 内是否有 fio
    if ! vm "command -v fio >/dev/null 2>&1" 2>/dev/null; then
        skip "T7: VM 内无 fio, 跳过 (可选测试)"
        return 0
    fi
    ok "VM 内 fio 可用"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    local fio_job="${MNT}/k2_ca_t7_fio.bin"
    local fio_size="1M"
    local fio_bs="4k"

    echo "  [T7-1] fio 4K randwrite ${fio_size} (触发 Stripe 迁移)..."
    # fio 默认写入二进制 pattern, 会触发 Stripe 迁移
    local fio_out
    fio_out=$(vm "fio --name=t7_randwrite --ioengine=sync --rw=randwrite --bs=${fio_bs} --size=${fio_size} --filename=${fio_job} --direct=1 --group_reporting 2>&1" 2>/dev/null)

    if [ $? -ne 0 ]; then
        ng "fio 执行失败"
        echo "$fio_out" | tail -10 | sed 's/^/    /'
        return 1
    fi
    ok "fio 执行完成"

    # 提取 IOPS
    local iops
    iops=$(echo "$fio_out" | grep -oE 'iops=[0-9]+' | head -1 | grep -oE '[0-9]+' || echo "N/A")
    echo "    4K randwrite IOPS: ${iops}"

    vm "sync" 2>/dev/null
    sleep 1

    # T7-2: dmesg 检查 (应有 Stripe 迁移 + 无异常)
    echo "  [T7-2] dmesg 检查..."
    local new_log
    new_log=$(dmesg_since "$base" 2>/dev/null)
    local migrate_count
    migrate_count=$(echo "$new_log" | grep -cE "MIGRATE ino=[0-9]+ → Stripe migration" || echo 0)
    if [ "$migrate_count" -ge 1 ]; then
        ok "检测到 Stripe 迁移 (${migrate_count} 次)"
    else
        warn "未检测到 Stripe 迁移 (可能 fio 文件 < 8KB inline 阈值? size=${fio_size})"
    fi

    # T7-3: 文件大小检查
    echo "  [T7-3] stat 检查..."
    local actual_size
    actual_size=$(vm "stat -c %s ${fio_job}" 2>/dev/null)
    local expected_bytes
    expected_bytes=$(echo "${fio_size}" | sed 's/M/*1024*1024/' | bc 2>/dev/null || echo 1048576)
    if [ "$actual_size" = "$expected_bytes" ]; then
        ok "fio 文件大小正确 (${actual_size}B)"
    else
        ng "fio 文件大小错误 (got ${actual_size} want ${expected_bytes})"
        return 1
    fi

    # T7-4: 内核状态检查
    if check_kernel_state "T7 fio 4K randwrite" "$base"; then
        ok "T7 内核状态正常"
    else
        ng "T7 内核状态异常"
        return 1
    fi

    # 清理
    vm "rm ${fio_job}" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T8: 60s 混合压力 (二进制+文本交替, 定期 dmesg)
# ============================================================
test_t8_mixed_stress() {
    section "T8: 60s 混合压力 (二进制+文本交替)"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)
    local slab_before
    slab_before=$(get_powerfs_slab_total)
    local mem_before
    mem_before=$(get_mem_available)
    echo "  -> 测试前 slab: ${slab_before}"
    echo "  -> 测试前 MemAvailable: ${mem_before} KB"

    # T8-1: 后台持续写入 (交替二进制/文本, 每个 10-20KB 触发迁移)
    echo ""
    echo "  [T8-1] 60s 混合写入 (二进制+文本交替, 定期 dmesg)"
    vm "mkdir -p ${MNT}/k2_ca_t8_stress" 2>/dev/null

    vm "
        end_time=\$((\$(date +%s) + 60))
        i=1
        while [ \$(date +%s) -lt \$end_time ]; do
            sz=\$(( (i % 10 + 10) * 1024 ))  # 10-20KB
            if [ \$((i % 2)) -eq 0 ]; then
                # 二进制
                dd if=/dev/urandom bs=1 count=\$sz 2>/dev/null > ${MNT}/k2_ca_t8_stress/bin_\${i}.dat
            else
                # 文本
                yes 'log_line: time=\$(date) level=info msg=test_message' | head -c \$sz > ${MNT}/k2_ca_t8_stress/txt_\${i}.conf
            fi
            # 每 20 个文件验证一个
            if [ \$((i % 20)) -eq 0 ]; then
                cat ${MNT}/k2_ca_t8_stress/bin_\${i}.dat > /dev/null 2>&1 || cat ${MNT}/k2_ca_t8_stress/txt_\${i}.conf > /dev/null 2>&1
            fi
            i=\$((i + 1))
        done
        echo \"written \$i files\"
    " 2>/dev/null &
    local stress_pid=$!

    # 期间每 10s 检查 dmesg
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep 10
        elapsed=$((elapsed + 10))
        echo "    [${elapsed}s] dmesg 检查..."
        if ! check_dmesg_clean "$base"; then
            ng "混合压力 ${elapsed}s 时检测到内核异常"
            kill $stress_pid 2>/dev/null
            wait $stress_pid 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg 正常"
    done

    wait $stress_pid 2>/dev/null
    ok "60s 混合压力完成"

    # T8-2: 检查迁移次数统计 (应有 Stripe + Flat 两种)
    echo ""
    echo "  [T8-2] 迁移次数统计..."
    local new_log
    new_log=$(dmesg_since "$base" 2>/dev/null)
    local stripe_migrations flat_migrations
    stripe_migrations=$(echo "$new_log" | grep -cE "MIGRATE ino=[0-9]+ → Stripe migration" || echo 0)
    flat_migrations=$(echo "$new_log" | grep -cE "MIGRATE ino=[0-9]+ → Flat migration" || echo 0)
    echo "    Stripe 迁移次数: ${stripe_migrations}"
    echo "    Flat   迁移次数: ${flat_migrations}"
    if [ "$stripe_migrations" -ge 1 ] && [ "$flat_migrations" -ge 1 ]; then
        ok "内容感知决策生效 (Stripe=${stripe_migrations} Flat=${flat_migrations})"
    else
        warn "迁移次数偏少 (stripe=${stripe_migrations} flat=${flat_migrations}), 检查写入量是否足够"
    fi

    # T8-3: 完成后状态检查
    echo ""
    echo "  [T8-3] 压力完成后内核状态检查"
    local slab_after
    slab_after=$(get_powerfs_slab_total)
    local mem_after
    mem_after=$(get_mem_available)
    echo "  -> 测试后 slab: ${slab_after} (前: ${slab_before})"
    echo "  -> 测试后 MemAvailable: ${mem_after} KB (前: ${mem_before} KB)"

    if check_kernel_state "T8 混合压力完成" "$base"; then
        ok "T8 内核状态最终检查通过"
    else
        ng "T8 内核状态异常"
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
    vm "rm -rf ${MNT}/k2_ca_t8_stress" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T9: 卸载 + 内核状态最终检查
# ============================================================
test_t9_unmount() {
    section "T9: 卸载 + 内核状态最终检查"

    local base=$(dmesg_line_count)
    SERIAL_BASE=$(serial_line_count)

    # T9-1: 清理残留文件
    echo "  [T9-1] 清理残留测试文件..."
    vm "rm -rf ${MNT}/k2_ca_*" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 1
    ok "清理完成"

    # T9-2: umount
    echo "  [T9-2] umount powerfs..."
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

    # T9-3: rmmod
    echo "  [T9-3] rmmod powerfs..."
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

    # T9-4: 卸载后 dmesg 检查
    echo "  [T9-4] 卸载后 dmesg 检查..."
    if check_dmesg_clean "$base"; then
        ok "卸载后 dmesg 无异常"
    else
        ng "卸载后 dmesg 检测到异常"
        return 1
    fi

    # T9-5: slab 全部释放
    echo "  [T9-5] slab 释放检查..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab 全部释放"
    else
        warn "powerfs slab 仍有残留:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # T9-6: 内存恢复检查
    echo "  [T9-6] 内存恢复检查..."
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
    echo "  [T9] 重新挂载 powerfs (便于后续测试)..."
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
    echo -e "${C_CYAN}║  K2-content-aware: Inline → Flat/Stripe 内容感知    ║${C_RESET}"
    echo -e "${C_CYAN}║  原则: 从简单到复杂, 二进制→Stripe 文本→Flat       ║${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    local tests=(
        "0:test_t0_compile"
        "1:test_t1_services"
        "2:test_t2_binary_stripe"
        "3:test_t3_text_flat"
        "4:test_t4_real_elf"
        "5:test_t5_large_text"
        "6:test_t6_remount_persist"
        "7:test_t7_fio_randwrite"
        "8:test_t8_mixed_stress"
        "9:test_t9_unmount"
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
    echo -e "${C_CYAN}║  测试总结                                            ║${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo -e "  ${C_GREEN}PASS:${C_RESET}  ${PASS}"
    echo -e "  ${C_RED}FAIL:${C_RESET}  ${FAIL}"
    echo -e "  ${C_YELLOW}WARN:${C_RESET}  ${WARN}"
    echo -e "  ${C_YELLOW}SKIP:${C_RESET}  ${SKIP}"
    echo ""

    if [ "$FAIL" -gt 0 ]; then
        echo -e "  ${C_RED}✗ K2 内容感知测试存在失败项${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  首个失败: T${failed_test}"
        fi
        echo ""
        echo "  排查建议:"
        echo "    1. 查看 VM dmesg: ./qemuctl.sh log powerfs"
        echo "    2. 实时监控 serial: ./qemuctl.sh serial-tail"
        echo "    3. 查看后端日志: ./qemuctl.sh service log filer-1"
        echo "    4. 确认 Filer inline_max_size=8192 (docker/config/filer-*.toml)"
        echo "    5. 确认 DesiredMode=0xD6 字段对齐 (Rust FieldId::DesiredMode)"
        echo "    6. 重新运行单个测试: ./test_k2_content_aware.sh <T编号>"
        echo "    7. 对比 ext4/btrfs (不修改测试, 发现问题是宝贝)"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ K2 内容感知测试全部通过${C_RESET}"
    echo ""
    echo "  K2-content-aware 验证门达成:"
    echo "    - 二进制内容 (urandom/ELF) → Stripe 迁移正确"
    echo "    - 文本内容 (配置/重复行) → Flat 迁移正确"
    echo "    - dmesg 迁移日志 (Stripe/Flat) 检出"
    echo "    - 迁移后数据 MD5 一致"
    echo "    - remount 持久化 (Stripe + Flat) 一致"
    echo "    - 60s 混合压力无异常"
    echo "    - 内核状态 (slab/meminfo/dmesg/serial) 正常"
    echo ""
    echo "  可进入下一阶段 (K3 Stripe 多卷并行写性能验证)"
    exit 0
}

main "$@"
