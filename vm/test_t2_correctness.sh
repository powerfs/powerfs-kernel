#!/bin/bash
# T2 阶段渐进式测试: 文件系统正确性
#
# 验证内容 (参照 kernel-layout-completion-plan.md):
#   T0: 编译 + 静态验证 (无后端)
#   T1: QEMU 启动 + 挂载 (复用 qemuctl.sh)
#   T2: 文件拷贝正确性 (目录树 cp + diff + 文件数)
#   T3: 压缩/解压正确性 (tar.gz 打包解包 + MD5 一致性)
#   T4: 源码编译测试 (小项目 make, 大量小文件 IO)
#   T5: rsync 同步测试 (VM 有 rsync 时)
#   T6: git 操作测试 (VM 有 git 时)
#   T7: 持续运行 + 内核状态监控 (60s 反复 cp/tar)
#   T8: 卸载 + 最终检查 (umount + rmmod + dmesg/slab/mem)
#
# 核心原则:
#   - 内核正确性 != 应用完成, 必须检查 dmesg/slab/meminfo
#   - 从小到大逐个确认, 不可跳级
#   - 前一档未通过不进入下一档
#   - VM 缺少工具 (rsync/git/make) 时 skip + warn, 不阻断
#
# 运行环境: HOST (通过 SSH 控制 VM)
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#   - 或由本脚本 T1 自动完成
#
# 用法:
#   ./test_t2_correctness.sh            # 运行全部 (T0-T8)
#   ./test_t2_correctness.sh 3          # 仅运行 T3
#   ./test_t2_correctness.sh 0 1 2      # 运行 T0+T1+T2

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
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
        ng "dmesg anomaly detected (${desc})"
        state_ok=1
    fi

    # 2. 无 D 状态线程
    if check_d_state; then
        ok "no D-state powerfs thread"
    else
        ng "D-state (hung) powerfs thread exists (${desc})"
        state_ok=1
    fi

    # 3. serial 日志 lockup 检查 (补充 dmesg, VM 卡死时也能查)
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        local serial_errors
        serial_errors=$(grep -E 'soft lockup|hard lockup|NMI watchdog|Kernel panic|BUG:|Oops:|RCU stall|workqueue lockup|hung task' "${qemu_log}" 2>/dev/null | tail -5 || true)
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

# VM 内命令是否可用
# 参数 $1: 命令名
# 返回: 0=可用, 1=不可用
vm_has_cmd() {
    local cmd="$1"
    local found
    found=$(vm "command -v $cmd 2>/dev/null || echo NOFOUND" 2>/dev/null)
    [ "$found" != "NOFOUND" ] && [ -n "$found" ]
}

# ============================================================
# T0: 编译 + 静态验证
# ============================================================
test_t0_compile() {
    section "T0: compile + static verification"

    # T0-1: 编译无警告
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

    # 检查编译警告
    local warnings
    warnings=$(echo "$build_log" | grep -iE 'warning:' | grep -v 'Wno-' || true)
    if [ -z "$warnings" ]; then
        ok "compile no warning"
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
# T1: QEMU 启动 + 挂载 (复用 qemuctl.sh)
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
        warn "backend not fully running, starting..."
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
    echo "  [T1-3] check VM SSH reachable..."
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

    # T1-6: 挂载后 30s dmesg 检查
    echo "  [T1-6] 30s dmesg observation after mount..."
    local base=$(dmesg_line_count)
    sleep 30
    if check_kernel_state "30s after mount" "$base"; then
        ok "kernel state OK 30s after mount"
    else
        ng "kernel state anomaly 30s after mount"
        return 1
    fi

    # 记录初始 slab 和 meminfo
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo "  -> initial slab (inode dentry): ${SLAB_INIT}"
    echo "  -> initial MemAvailable: ${MEM_INIT} KB"

    # 清理可能残留的 T2 测试数据
    vm "rm -rf ${MNT}/t2_* /tmp/t2_src /tmp/t2_*" 2>/dev/null
    vm "sync" 2>/dev/null

    return 0
}

# ============================================================
# T2: 文件拷贝正确性
# ============================================================
test_t2_copy() {
    section "T2: file copy correctness"

    local base=$(dmesg_line_count)

    # T2a: 在 VM /tmp 创建目录树 (100 个文件, 1K-100K 各档)
    echo "  [T2a] create source tree in /tmp (100 files)..."
    vm "mkdir -p /tmp/t2_src" 2>/dev/null
    if ! vm "test -d /tmp/t2_src" 2>/dev/null; then
        ng "mkdir /tmp/t2_src failed"
        return 1
    fi

    # 生成 100 个文件, file_$i 大小为 i*10 KB (10K..1000K)
    vm "for i in \$(seq 1 100); do dd if=/dev/urandom bs=1K count=\$((i*10)) of=/tmp/t2_src/file_\$i 2>/dev/null; done" 2>/dev/null
    local src_count
    src_count=$(vm "find /tmp/t2_src -type f | wc -l" 2>/dev/null | tr -d ' ')
    if [ "$src_count" = "100" ]; then
        ok "source tree created (100 files)"
    else
        ng "source tree file count wrong (got ${src_count}, want 100)"
        return 1
    fi

    if ! check_kernel_state "T2a create source tree" "$base"; then
        ng "T2a kernel state anomaly"
        return 1
    fi

    # T2b: cp -r 到 powerfs
    echo "  [T2b] cp -r /tmp/t2_src -> ${MNT}/t2_copy..."
    local cp_base=$(dmesg_line_count)
    if vm "cp -r /tmp/t2_src ${MNT}/t2_copy" 2>/dev/null; then
        ok "cp -r succeeded"
    else
        ng "cp -r failed"
        return 1
    fi
    vm "sync" 2>/dev/null

    if ! check_kernel_state "T2b cp -r" "$cp_base"; then
        ng "T2b kernel state anomaly"
        return 1
    fi

    # T2c: diff -r 无差异
    echo "  [T2c] diff -r source vs copy..."
    local diff_base=$(dmesg_line_count)
    local diff_out
    diff_out=$(vm "diff -r /tmp/t2_src ${MNT}/t2_copy 2>&1" 2>/dev/null)
    if [ -z "$diff_out" ]; then
        ok "diff -r no difference"
    else
        ng "diff -r found differences:"
        echo "$diff_out" | head -20 | sed 's/^/    /'
        return 1
    fi

    if ! check_kernel_state "T2c diff -r" "$diff_base"; then
        ng "T2c kernel state anomaly"
        return 1
    fi

    # T2d: 文件数确认
    echo "  [T2d] confirm file count..."
    local copy_count
    copy_count=$(vm "find ${MNT}/t2_copy -type f | wc -l" 2>/dev/null | tr -d ' ')
    if [ "$copy_count" = "100" ]; then
        ok "copy file count correct (100)"
    else
        ng "copy file count wrong (got ${copy_count}, want 100)"
        return 1
    fi

    # 附加: 总大小一致性
    local src_size copy_size
    src_size=$(vm "du -sb /tmp/t2_src 2>/dev/null | awk '{print \$1}'" 2>/dev/null)
    copy_size=$(vm "du -sb ${MNT}/t2_copy 2>/dev/null | awk '{print \$1}'" 2>/dev/null)
    if [ -n "$src_size" ] && [ "$src_size" = "$copy_size" ]; then
        ok "total size consistent (${src_size} bytes)"
    else
        warn "total size mismatch (src=${src_size} copy=${copy_size})"
    fi

    if check_kernel_state "T2 overall" "$base"; then
        ok "T2 kernel state OK"
    else
        ng "T2 kernel state anomaly"
        return 1
    fi

    return 0
}

# ============================================================
# T3: 压缩/解压正确性
# ============================================================
test_t3_compress() {
    section "T3: compress/decompress correctness"

    local base=$(dmesg_line_count)

    # T3a: tar czf 打包
    echo "  [T3a] tar czf /tmp/t2_src -> ${MNT}/t2_archive.tar.gz..."
    local tar_base=$(dmesg_line_count)
    # 使用 -C / 避免绝对路径警告, 解压后为 tmp/t2_src
    if vm "tar czf ${MNT}/t2_archive.tar.gz -C / tmp/t2_src 2>/dev/null" 2>/dev/null; then
        ok "tar czf succeeded"
    else
        ng "tar czf failed"
        return 1
    fi
    vm "sync" 2>/dev/null

    # 确认归档文件存在且非空
    local arch_size
    arch_size=$(vm "stat -c %s ${MNT}/t2_archive.tar.gz 2>/dev/null" 2>/dev/null)
    if [ -n "$arch_size" ] && [ "$arch_size" -gt 0 ] 2>/dev/null; then
        ok "archive created (${arch_size} bytes)"
    else
        ng "archive empty or missing"
        return 1
    fi

    if ! check_kernel_state "T3a tar czf" "$tar_base"; then
        ng "T3a kernel state anomaly"
        return 1
    fi

    # T3b: 解压到 ${MNT}/t2_extract
    echo "  [T3b] tar xzf -> ${MNT}/t2_extract..."
    local xzf_base=$(dmesg_line_count)
    vm "mkdir -p ${MNT}/t2_extract" 2>/dev/null
    if vm "tar xzf ${MNT}/t2_archive.tar.gz -C ${MNT}/t2_extract 2>/dev/null" 2>/dev/null; then
        ok "tar xzf succeeded"
    else
        ng "tar xzf failed"
        return 1
    fi
    vm "sync" 2>/dev/null

    if ! check_kernel_state "T3b tar xzf" "$xzf_base"; then
        ng "T3b kernel state anomaly"
        return 1
    fi

    # T3c: diff -r 解压结果与源
    echo "  [T3c] diff -r source vs extracted..."
    local diff_base=$(dmesg_line_count)
    local diff_out
    diff_out=$(vm "diff -r /tmp/t2_src ${MNT}/t2_extract/tmp/t2_src 2>&1" 2>/dev/null)
    if [ -z "$diff_out" ]; then
        ok "diff -r no difference (extract matches source)"
    else
        ng "diff -r found differences:"
        echo "$diff_out" | head -20 | sed 's/^/    /'
        return 1
    fi

    if ! check_kernel_state "T3c diff -r" "$diff_base"; then
        ng "T3c kernel state anomaly"
        return 1
    fi

    # T3d: MD5 校验 tar.gz 文件一致性 (两次读取 MD5 一致 + 列表校验)
    echo "  [T3d] MD5 consistency check on archive..."
    local md5_base=$(dmesg_line_count)
    local md5_first md5_second
    md5_first=$(vm "md5sum ${MNT}/t2_archive.tar.gz 2>/dev/null | awk '{print \$1}'" 2>/dev/null)
    # drop cache 后再读一次, 验证从后端读取一致
    vm "sync; echo 2 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1
    md5_second=$(vm "md5sum ${MNT}/t2_archive.tar.gz 2>/dev/null | awk '{print \$1}'" 2>/dev/null)
    echo "    first  MD5: ${md5_first}"
    echo "    second MD5: ${md5_second}"
    if [ -n "$md5_first" ] && [ "$md5_first" = "$md5_second" ]; then
        ok "archive MD5 consistent across reads"
    else
        ng "archive MD5 inconsistent (first=${md5_first} second=${md5_second})"
        return 1
    fi

    # 归档内容文件数校验
    local arch_file_count
    arch_file_count=$(vm "tar tzf ${MNT}/t2_archive.tar.gz 2>/dev/null | grep -c 'file_'" 2>/dev/null | tr -d ' ')
    if [ "$arch_file_count" = "100" ]; then
        ok "archive contains 100 files"
    else
        ng "archive file count wrong (got ${arch_file_count}, want 100)"
        return 1
    fi

    if ! check_kernel_state "T3d MD5 check" "$md5_base"; then
        ng "T3d kernel state anomaly"
        return 1
    fi

    # 清理归档与解压目录
    vm "rm -rf ${MNT}/t2_archive.tar.gz ${MNT}/t2_extract" 2>/dev/null

    if check_kernel_state "T3 overall" "$base"; then
        ok "T3 kernel state OK"
    else
        ng "T3 kernel state anomaly"
        return 1
    fi

    return 0
}

# ============================================================
# T4: 源码编译测试 (大量小文件 IO)
# ============================================================
test_t4_build() {
    section "T4: source build test"

    local base=$(dmesg_line_count)

    # 检查 VM 内是否有 make 和 cc
    echo "  [T4-pre] check build tools in VM..."
    if ! vm_has_cmd "make"; then
        warn "no 'make' in VM, skip T4"
        skip "T4 source build (no make)"
        return 0
    fi
    if ! vm_has_cmd "cc" && ! vm_has_cmd "gcc"; then
        warn "no compiler (cc/gcc) in VM, skip T4"
        skip "T4 source build (no compiler)"
        return 0
    fi
    ok "build tools available in VM"

    # T4a: 在 ${MNT}/t2_build 中创建简单 C 项目
    echo "  [T4a] create simple C project in ${MNT}/t2_build..."
    vm "rm -rf ${MNT}/t2_build && mkdir -p ${MNT}/t2_build" 2>/dev/null

    # 写入 hello.c (VM 内可能无 heredoc, 用 echo 拼接)
    vm "cat > ${MNT}/t2_build/hello.c <<'CEOF'
#include <stdio.h>
int main(int argc, char **argv) {
    int i;
    for (i = 0; i < 100; i++) {
        printf(\"hello powerfs %d\\n\", i);
    }
    return 0;
}
CEOF" 2>/dev/null

    # 写入 Makefile (使用 .RECIPEPREFIX = > 避免通过 SSH 传递 tab 的脆弱性)
    vm "cat > ${MNT}/t2_build/Makefile <<'MEOF'
.RECIPEPREFIX = >
CC ?= cc
CFLAGS ?= -O2 -Wall
all: hello
hello: hello.c
>\$(CC) \$(CFLAGS) -o hello hello.c
clean:
>rm -f hello
MEOF" 2>/dev/null

    if vm "test -f ${MNT}/t2_build/hello.c && test -f ${MNT}/t2_build/Makefile" 2>/dev/null; then
        ok "C project created (hello.c + Makefile)"
    else
        ng "failed to create C project"
        return 1
    fi

    # T4b: make 编译
    echo "  [T4b] make in ${MNT}/t2_build..."
    local make_base=$(dmesg_line_count)
    local make_out
    make_out=$(vm "cd ${MNT}/t2_build && make 2>&1" 2>/dev/null)
    local make_ret=$?
    echo "$make_out" | head -10 | sed 's/^/    /'
    if [ $make_ret -eq 0 ]; then
        ok "make succeeded"
    else
        ng "make failed (ret=${make_ret})"
        return 1
    fi

    # T4c: 确认编译产物存在
    echo "  [T4c] verify build artifact..."
    if vm "test -f ${MNT}/t2_build/hello" 2>/dev/null; then
        local bin_size
        bin_size=$(vm "stat -c %s ${MNT}/t2_build/hello" 2>/dev/null)
        ok "binary 'hello' built (${bin_size} bytes)"
    else
        ng "binary 'hello' not found"
        return 1
    fi

    # 运行二进制验证可执行
    local run_out
    run_out=$(vm "${MNT}/t2_build/hello 2>&1 | head -3" 2>/dev/null)
    if echo "$run_out" | grep -q "hello powerfs"; then
        ok "binary runs correctly"
    else
        warn "binary output unexpected: ${run_out}"
    fi

    # T4d: 内核状态检查 (编译过程大量小文件 IO)
    if check_kernel_state "T4 make" "$make_base"; then
        ok "T4 kernel state OK"
    else
        ng "T4 kernel state anomaly"
        return 1
    fi

    # 附加: make clean 测试 (删除产物)
    vm "cd ${MNT}/t2_build && make clean 2>&1" 2>/dev/null
    if vm "test ! -f ${MNT}/t2_build/hello" 2>/dev/null; then
        ok "make clean removed binary"
    else
        warn "make clean did not remove binary"
    fi

    # slab 检查
    local slab_now
    slab_now=$(get_powerfs_slab_total)
    echo "    slab (inode dentry): ${slab_now} (initial: ${SLAB_INIT:-N/A})"

    # 清理
    vm "rm -rf ${MNT}/t2_build" 2>/dev/null

    if ! check_kernel_state "T4 overall" "$base"; then
        ng "T4 overall kernel state anomaly"
        return 1
    fi

    return 0
}

# ============================================================
# T5: rsync 同步测试 (VM 有 rsync 时)
# ============================================================
test_t5_rsync() {
    section "T5: rsync sync test"

    # 检查 VM 内是否有 rsync
    if ! vm_has_cmd "rsync"; then
        warn "no 'rsync' in VM, skip T5"
        skip "T5 rsync (no rsync in VM)"
        return 0
    fi
    ok "rsync available in VM"

    local base=$(dmesg_line_count)

    # T5a: rsync -a 同步
    echo "  [T5a] rsync -a /tmp/t2_src/ -> ${MNT}/t2_rsync/..."
    local rsync_base=$(dmesg_line_count)
    if vm "rsync -a /tmp/t2_src/ ${MNT}/t2_rsync/ 2>&1" 2>/dev/null; then
        ok "rsync -a succeeded"
    else
        ng "rsync -a failed"
        return 1
    fi
    vm "sync" 2>/dev/null

    if ! check_kernel_state "T5a rsync" "$rsync_base"; then
        ng "T5a kernel state anomaly"
        return 1
    fi

    # T5b: diff -r 无差异
    echo "  [T5b] diff -r source vs rsync..."
    local diff_base=$(dmesg_line_count)
    local diff_out
    diff_out=$(vm "diff -r /tmp/t2_src ${MNT}/t2_rsync 2>&1" 2>/dev/null)
    if [ -z "$diff_out" ]; then
        ok "diff -r no difference"
    else
        ng "diff -r found differences:"
        echo "$diff_out" | head -20 | sed 's/^/    /'
        return 1
    fi

    # T5c: 内核状态检查
    if ! check_kernel_state "T5b diff -r" "$diff_base"; then
        ng "T5b kernel state anomaly"
        return 1
    fi

    # 文件数确认
    local rsync_count
    rsync_count=$(vm "find ${MNT}/t2_rsync -type f | wc -l" 2>/dev/null | tr -d ' ')
    if [ "$rsync_count" = "100" ]; then
        ok "rsync file count correct (100)"
    else
        ng "rsync file count wrong (got ${rsync_count}, want 100)"
        return 1
    fi

    # 清理
    vm "rm -rf ${MNT}/t2_rsync" 2>/dev/null

    if check_kernel_state "T5 overall" "$base"; then
        ok "T5 kernel state OK"
    else
        ng "T5 kernel state anomaly"
        return 1
    fi

    return 0
}

# ============================================================
# T6: git 操作测试 (VM 有 git 时)
# ============================================================
test_t6_git() {
    section "T6: git operations test"

    # 检查 VM 内是否有 git
    if ! vm_has_cmd "git"; then
        warn "no 'git' in VM, skip T6"
        skip "T6 git (no git in VM)"
        return 0
    fi
    ok "git available in VM"

    local base=$(dmesg_line_count)

    # T6a: git init
    echo "  [T6a] git init in ${MNT}/t2_git..."
    local git_base=$(dmesg_line_count)
    vm "rm -rf ${MNT}/t2_git && mkdir -p ${MNT}/t2_git" 2>/dev/null
    if vm "cd ${MNT}/t2_git && git init 2>&1" 2>/dev/null | grep -qi "initialized\|already"; then
        ok "git init succeeded"
    else
        ng "git init failed"
        return 1
    fi

    # 配置 user (git commit 需要)
    vm "cd ${MNT}/t2_git && git config user.email 'test@powerfs.local' && git config user.name 'powerfs test'" 2>/dev/null

    if ! check_kernel_state "T6a git init" "$git_base"; then
        ng "T6a kernel state anomaly"
        return 1
    fi

    # T6b: 创建文件 + git add + git commit
    echo "  [T6b] create file + git add + git commit..."
    local add_base=$(dmesg_line_count)
    vm "cd ${MNT}/t2_git && echo 'hello powerfs git' > README.md && for i in \$(seq 1 10); do echo \"line \$i\" > file_\$i.txt; done" 2>/dev/null
    if vm "cd ${MNT}/t2_git && git add . 2>&1" 2>/dev/null; then
        ok "git add succeeded"
    else
        ng "git add failed"
        return 1
    fi

    local commit_out
    commit_out=$(vm "cd ${MNT}/t2_git && git commit -m 'initial commit' 2>&1" 2>/dev/null)
    if echo "$commit_out" | grep -qi "master\|main"; then
        ok "git commit succeeded"
    else
        ng "git commit failed: ${commit_out}"
        return 1
    fi

    if ! check_kernel_state "T6b git add+commit" "$add_base"; then
        ng "T6b kernel state anomaly"
        return 1
    fi

    # T6c: git log 确认提交记录
    echo "  [T6c] git log verify..."
    local log_out
    log_out=$(vm "cd ${MNT}/t2_git && git log --oneline 2>&1" 2>/dev/null)
    if echo "$log_out" | grep -q "initial commit"; then
        ok "git log shows commit"
    else
        ng "git log empty or wrong: ${log_out}"
        return 1
    fi

    # T6d: git status 确认 clean
    echo "  [T6d] git status clean..."
    local status_out
    status_out=$(vm "cd ${MNT}/t2_git && git status --porcelain 2>&1" 2>/dev/null)
    if [ -z "$status_out" ]; then
        ok "git status clean"
    else
        ng "git status not clean: ${status_out}"
        return 1
    fi

    # 附加: git checkout / 修改后再 status
    vm "cd ${MNT}/t2_git && echo 'modified' >> README.md" 2>/dev/null
    local status2
    status2=$(vm "cd ${MNT}/t2_git && git status --porcelain 2>&1" 2>/dev/null)
    if [ -n "$status2" ]; then
        ok "git status detects modification"
    else
        warn "git status did not detect modification"
    fi

    # T6e: 内核状态检查
    if check_kernel_state "T6 git ops" "$base"; then
        ok "T6 kernel state OK"
    else
        ng "T6 kernel state anomaly"
        return 1
    fi

    # 清理
    vm "rm -rf ${MNT}/t2_git" 2>/dev/null

    return 0
}

# ============================================================
# T7: 持续运行 + 内核状态监控 (60s 反复 cp/tar)
# ============================================================
test_t7_sustained() {
    section "T7: sustained run + kernel monitoring (60s)"

    local base=$(dmesg_line_count)
    local slab_before
    slab_before=$(get_powerfs_slab_total)
    local mem_before
    mem_before=$(get_mem_available)
    echo "  -> before slab: ${slab_before}"
    echo "  -> before MemAvailable: ${mem_before} KB"

    # 记录源 MD5 (用于最终校验)
    local src_md5
    src_md5=$(vm "find /tmp/t2_src -type f -exec md5sum {} \; 2>/dev/null | sort | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "  -> source tree aggregate MD5: ${src_md5}"

    # T7a: 后台反复 cp/tar 操作 60s
    echo "  [T7a] sustained cp/tar for 60s (background)..."
    vm "end_time=\$((\$(date +%s) + 60)); i=0; while [ \$(date +%s) -lt \$end_time ]; do i=\$((i+1)); cp -r /tmp/t2_src ${MNT}/t7_run_\${i} 2>/dev/null; rm -rf ${MNT}/t7_run_\${i} 2>/dev/null; tar czf ${MNT}/t7_tar_\${i}.tar.gz -C / tmp/t2_src 2>/dev/null; rm -f ${MNT}/t7_tar_\${i}.tar.gz 2>/dev/null; done; echo DONE_\${i}" 2>/dev/null &
    local bg_pid=$!

    # T7b: 每 10s 检查 dmesg
    local check_interval=10
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        sleep $check_interval
        elapsed=$((elapsed + check_interval))
        echo "    [${elapsed}s] dmesg check..."
        if ! check_dmesg_clean "$base"; then
            ng "kernel anomaly detected at ${elapsed}s during sustained run"
            kill $bg_pid 2>/dev/null
            wait $bg_pid 2>/dev/null
            vm "rm -rf ${MNT}/t7_run_* ${MNT}/t7_tar_*" 2>/dev/null
            return 1
        fi
        echo "    [${elapsed}s] dmesg clean"

        # D 状态检查
        if ! check_d_state; then
            ng "D-state powerfs thread at ${elapsed}s"
            kill $bg_pid 2>/dev/null
            wait $bg_pid 2>/dev/null
            vm "rm -rf ${MNT}/t7_run_* ${MNT}/t7_tar_*" 2>/dev/null
            return 1
        fi
    done

    wait $bg_pid 2>/dev/null
    ok "sustained cp/tar 60s completed"

    # 清理残余文件
    vm "rm -rf ${MNT}/t7_run_* ${MNT}/t7_tar_*" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 2

    # T7c: 最终 MD5 校验数据完整性
    echo "  [T7c] final MD5 integrity check..."
    local src_md5_after
    src_md5_after=$(vm "find /tmp/t2_src -type f -exec md5sum {} \; 2>/dev/null | sort | md5sum | awk '{print \$1}'" 2>/dev/null)
    echo "    source tree aggregate MD5 (after): ${src_md5_after}"
    if [ -n "$src_md5" ] && [ "$src_md5" = "$src_md5_after" ]; then
        ok "source tree MD5 intact after sustained run"
    else
        ng "source tree MD5 changed (before=${src_md5} after=${src_md5_after})"
        return 1
    fi

    # 最终内核状态检查
    local slab_after
    slab_after=$(get_powerfs_slab_total)
    local mem_after
    mem_after=$(get_mem_available)
    echo "  -> after slab: ${slab_after} (before: ${slab_before})"
    echo "  -> after MemAvailable: ${mem_after} KB (before: ${mem_before} KB)"

    if check_kernel_state "T7 sustained complete" "$base"; then
        ok "T7 kernel state OK"
    else
        ng "T7 kernel state anomaly"
        return 1
    fi

    # 内存泄漏检查 (MemAvailable 下降不超过 10%)
    if [ "${mem_before:-0}" -gt 0 ]; then
        local mem_drop_pct=$(( (mem_before - mem_after) * 100 / mem_before ))
        if [ $mem_drop_pct -lt 10 ]; then
            ok "memory leak check passed (MemAvailable drop ${mem_drop_pct}%)"
        else
            warn "MemAvailable drop ${mem_drop_pct}%, possible memory leak"
        fi
    fi

    # hung task 检查
    local hung
    hung=$(vm "dmesg | grep 'hung task' 2>/dev/null | grep -i powerfs" 2>/dev/null || true)
    if [ -z "$hung" ]; then
        ok "no hung task"
    else
        ng "hung task detected: $hung"
        return 1
    fi

    return 0
}

# ============================================================
# T8: 卸载 + 最终检查
# ============================================================
test_t8_unmount() {
    section "T8: unmount + final check"

    local base=$(dmesg_line_count)

    # T8-0: 清理测试文件
    echo "  [T8-0] cleanup test files..."
    vm "rm -rf ${MNT}/t2_* /tmp/t2_src /tmp/t2_*" 2>/dev/null
    vm "sync" 2>/dev/null
    sleep 2
    ok "test files cleaned"

    # T8-1: umount
    echo "  [T8-1] umount powerfs..."
    local umount_ret
    umount_ret=$(vm "timeout 30 umount ${MNT} 2>&1" 2>/dev/null)
    local ret=$?
    if [ $ret -eq 0 ]; then
        ok "umount succeeded"
    else
        ng "umount failed/timeout: ${umount_ret}"
        return 1
    fi

    sleep 2

    # T8-2: rmmod
    echo "  [T8-2] rmmod powerfs..."
    local rmmod_ret
    rmmod_ret=$(vm "rmmod powerfs 2>&1" 2>/dev/null)
    ret=$?
    if [ $ret -eq 0 ]; then
        ok "rmmod succeeded"
    else
        ng "rmmod failed: ${rmmod_ret}"
        echo "    possible refcount, check: lsmod | grep powerfs"
        return 1
    fi

    sleep 2

    # T8-3: 卸载后 dmesg 检查
    echo "  [T8-3] dmesg check after unmount..."
    if check_dmesg_clean "$base"; then
        ok "dmesg clean after unmount"
    else
        ng "dmesg anomaly after unmount"
        return 1
    fi

    # T8-4: slab 全部释放
    echo "  [T8-4] slab release check..."
    local slab_remaining
    slab_remaining=$(vm "cat /proc/slabinfo | grep powerfs" 2>/dev/null || true)
    if [ -z "$slab_remaining" ]; then
        ok "powerfs slab fully released"
    else
        warn "powerfs slab still remains:"
        echo "$slab_remaining" | sed 's/^/    /'
    fi

    # T8-5: 内存恢复检查
    echo "  [T8-5] memory recovery check..."
    local mem_final
    mem_final=$(get_mem_available)
    echo "  -> final MemAvailable: ${mem_final} KB (initial: ${MEM_INIT} KB)"

    if [ "${MEM_INIT:-0}" -gt 0 ]; then
        local diff=$(( ${MEM_INIT} - ${mem_final} ))
        if [ $diff -lt 50000 ]; then
            ok "memory recovery good (diff ${diff} KB)"
        else
            warn "memory diff large (${diff} KB), possible leak"
        fi
    fi

    # T8-6: D 状态最终检查
    echo "  [T8-6] final D-state check..."
    if check_d_state; then
        ok "no D-state powerfs thread after unmount"
    else
        ng "D-state powerfs thread remains after unmount"
        return 1
    fi

    # 重新挂载以便后续使用
    echo ""
    echo "  [T8] remount powerfs (for subsequent tests)..."
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
    echo -e "${C_CYAN}║  T2 correctness test: file system correctness        ${C_RESET}"
    echo -e "${C_CYAN}║  principle: small-to-large, per-step kernel check    ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"

    # 测试列表 (顺序执行, 前一档失败则停止)
    local tests=(
        "0:test_t0_compile"
        "1:test_t1_mount"
        "2:test_t2_copy"
        "3:test_t3_compress"
        "4:test_t4_build"
        "5:test_t5_rsync"
        "6:test_t6_git"
        "7:test_t7_sustained"
        "8:test_t8_unmount"
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
    echo -e "${C_CYAN}║  test summary                                        ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo -e "  ${C_GREEN}PASS:${C_RESET}  ${PASS}"
    echo -e "  ${C_RED}FAIL:${C_RESET}  ${FAIL}"
    echo -e "  ${C_YELLOW}WARN:${C_RESET}  ${WARN}"
    echo -e "  ${C_YELLOW}SKIP:${C_RESET}  ${SKIP}"
    echo ""

    if [ "$FAIL" -gt 0 ]; then
        echo -e "  ${C_RED}✗ T2 correctness test has failures${C_RESET}"
        if [ -n "$failed_test" ]; then
            echo "  first failure: T${failed_test}"
        fi
        echo ""
        echo "  troubleshooting:"
        echo "    1. VM dmesg: ./qemuctl.sh log powerfs"
        echo "    2. live monitor: ./qemuctl.sh monitor powerfs"
        echo "    3. backend logs: ./qemuctl.sh service log filer-1"
        echo "    4. rerun single test: ./test_t2_correctness.sh <T-num>"
        exit 1
    fi

    echo -e "  ${C_GREEN}✓ T2 correctness test all passed${C_RESET}"
    echo ""
    echo "  T2 verification gate achieved:"
    echo "    - compile + static verification passed"
    echo "    - QEMU mount + dmesg clean"
    echo "    - file copy correctness (cp + diff)"
    echo "    - compress/decompress correctness (tar + MD5)"
    echo "    - source build test (small files IO)"
    echo "    - rsync/git operations (when available)"
    echo "    - sustained 60s run no anomaly"
    echo "    - kernel state (slab/meminfo/dmesg) clean"
    echo ""
    exit 0
}

main "$@"
