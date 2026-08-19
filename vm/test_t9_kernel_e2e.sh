#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# T9: 内核源码 E2E 测试（打包 → 解包 → 编译 → 删除）
#
# 在 PowerFS 上执行完整的内核源码生命周期测试：
#   T9.01  拷贝 tarball 到 PowerFS         (大文件写入 ~238MB)
#   T9.02  解压 tarball                    (批量创建 ~96k 文件)
#   T9.03  make defconfig                  (exec + 配置文件创建)
#   T9.04  make -jN                        (重度读/写/exec 压力)
#   T9.05  rm -rf                          (批量 unlink + rmdir)
#
# 每阶段捕获 EIO/错误计数、文件数、耗时，并检查内核状态 (dmesg).
#
# 使用方式:
#   ./test_t9_kernel_e2e.sh                # 运行全部
#   ./test_t9_kernel_e2e.sh 2 3 5          # 只运行 T2 T3 T5
#
# 环境变量:
#   KERNEL_TARBALL   内核 tarball 路径 (默认: /home/portion/linux_6.17.0.orig.tar.gz)
#   SKIP_BUILD       设为 1 跳过编译阶段 (仅打包/解包/删除)

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
HOST_TARBALL="${KERNEL_TARBALL:-/home/portion/linux_6.17.0.orig.tar.gz}"
SHARE_DIR="${SCRIPT_DIR}/share"
T9_DIR="${MNT}/t9_kernel_e2e"
T9_LOG="/tmp/t9_kernel_e2e.log"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

PASS=0
FAIL=0
WARN=0
SKIP=0
FAILED_TEST=""

ok()   { echo -e "${GREEN}[PASS]${NC} $*"; ((PASS++)); }
ng()   { echo -e "${RED}[FAIL]${NC} $*"; ((FAIL++)); FAILED_TEST="${FAILED_TEST} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; ((WARN++)); }
skip() { echo -e "${GRAY}[SKIP]${NC} $*"; ((SKIP++)); }
section() { echo -e "\n${CYAN}==== $* ====${NC}"; }

# ---------- 内核状态检查 ----------
check_dmesg_clean() {
    local since="$1"
    local label="$2"
    local dmesg_out
    dmesg_out=$(dmesg_since "$since" 2>/dev/null)
    if [ -n "$dmesg_out" ]; then
        if echo "$dmesg_out" | grep -qiE 'BUG|panic|oops|WARNING|hung_task|lockup|slab-out|use-after-free|null.pointer|general.protection'; then
            warn "${label}: dmesg has potential issues:"
            echo "$dmesg_out" | head -10 | sed 's/^/    /'
            return 1
        fi
    fi
    return 0
}

check_kernel_state() {
    local label="$1"
    local dmesg_base="$2"
    check_dmesg_clean "$dmesg_base" "$label"
}

# ---------- 选择性运行 ----------
should_run() {
    [ ${#RUN_TESTS[@]} -eq 0 ] && return 0
    local t="$1"
    for x in "${RUN_TESTS[@]}"; do
        [ "$x" = "$t" ] && return 0
    done
    return 1
}

RUN_TESTS=("$@")

# ---------- 辅助函数 ----------
vm_has_cmd() {
    vm "command -v $1 >/dev/null 2>&1" 2>/dev/null
}

count_eio() {
    local file="$1"
    [ -f "$file" ] || { echo 0; return; }
    grep -ciE 'input/output error|EIO|no space left|read-only|stale file' "$file" 2>/dev/null || true
}

# ============================================================
# T9.01: 拷贝 tarball 到 PowerFS
# ============================================================
test_t1_copy() {
    section "T9.01: copy tarball to PowerFS"
    local base=$(dmesg_line_count)

    if [ ! -f "$HOST_TARBALL" ]; then
        skip "T9.01 copy tarball (not found: $HOST_TARBALL)"
        return 0
    fi

    # 确保 tarball 在 9p share 目录中 (VM 可通过 /mnt/host 访问)
    local tarball_name
    tarball_name=$(basename "$HOST_TARBALL")
    local share_tarball="${SHARE_DIR}/${tarball_name}"
    if [ ! -f "$share_tarball" ]; then
        echo "  Copying tarball to 9p share dir..."
        cp "$HOST_TARBALL" "$share_tarball"
    fi
    local tar_size
    tar_size=$(stat -c %s "$HOST_TARBALL" 2>/dev/null || echo 0)
    echo "  Tarball: ${tarball_name} ($(( tar_size / 1024 / 1024 )) MB)"

    # 清理旧测试目录
    vm "rm -rf ${T9_DIR}" 2>/dev/null
    vm "mkdir -p ${T9_DIR}" 2>/dev/null

    # 在 VM 内: 从 9p share 拷贝到 PowerFS
    local vm_share_path="/mnt/host/${tarball_name}"
    echo "  Copying ${vm_share_path} -> ${T9_DIR}/ ..."
    local start end rc
    start=$(date +%s)
    vm "cp ${vm_share_path} ${T9_DIR}/ 2>&1" > "$T9_LOG" 2>&1
    rc=$?
    end=$(date +%s)
    local dur=$((end - start))
    local eio=$(count_eio "$T9_LOG")
    echo "  exit=${rc}, duration=${dur}s, EIO=${eio}"

    if [ "$rc" -eq 0 ] && [ "$eio" -eq 0 ]; then
        ok "Tarball copied to PowerFS in ${dur}s ($(( tar_size / 1024 / 1024 )) MB)"
    else
        ng "T9.01 copy tarball (exit=${rc}, EIO=${eio})"
    fi

    check_kernel_state "T9.01 copy" "$base"
}

# ============================================================
# T9.02: 解压 tarball
# ============================================================
test_t2_unpack() {
    section "T9.02: unpack tarball on PowerFS"
    local base=$(dmesg_line_count)

    local tarball_name
    tarball_name=$(basename "$HOST_TARBALL")

    echo "  Unpacking ${T9_DIR}/${tarball_name} ..."
    local start end rc
    start=$(date +%s)
    vm "cd ${T9_DIR} && tar xzf ${tarball_name} 2>&1" > "$T9_LOG" 2>&1
    rc=$?
    end=$(date +%s)
    local dur=$((end - start))

    local file_count dir_count eio
    file_count=$(vm "find ${T9_DIR} -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d ' \r')
    dir_count=$(vm "find ${T9_DIR} -type d 2>/dev/null | wc -l" 2>/dev/null | tr -d ' \r')
    eio=$(count_eio "$T9_LOG")
    echo "  files=${file_count}, dirs=${dir_count}, exit=${rc}, EIO=${eio}, dur=${dur}s"

    local min_files=10000
    if [ "$rc" -eq 0 ] && [ "$eio" -eq 0 ] && [ "${file_count:-0}" -gt "$min_files" ]; then
        ok "Unpack succeeded (${file_count} files in ${dur}s, no EIO)"
    else
        ng "T9.02 unpack (files=${file_count}, EIO=${eio}, min=${min_files})"
    fi

    check_kernel_state "T9.02 unpack" "$base"
}

# ============================================================
# T9.03: make defconfig
# ============================================================
test_t3_defconfig() {
    section "T9.03: make defconfig"
    local base=$(dmesg_line_count)

    if [ "${SKIP_BUILD:-0}" = "1" ]; then
        skip "T9.03 defconfig (SKIP_BUILD=1)"
        return 0
    fi

    if ! vm_has_cmd "make"; then
        skip "T9.03 defconfig (no make in VM)"
        return 0
    fi

    local src_dir
    src_dir=$(vm "find ${T9_DIR} -maxdepth 1 -type d | tail -1" 2>/dev/null | tr -d ' \r')
    echo "  Source dir: ${src_dir}"

    if [ -z "$src_dir" ]; then
        ng "T9.03 defconfig (source dir not found)"
        return 0
    fi

    vm "cd ${src_dir} && make defconfig 2>&1" >> "$T9_LOG" 2>&1
    local rc=$?
    echo "  defconfig exit=${rc}"

    if [ "$rc" -eq 0 ]; then
        ok "Kernel configured (defconfig)"
    else
        ng "T9.03 defconfig (exit=${rc})"
    fi

    check_kernel_state "T9.03 defconfig" "$base"
}

# ============================================================
# T9.04: make -jN
# ============================================================
test_t4_build() {
    section "T9.04: make -jN"
    local base=$(dmesg_line_count)

    if [ "${SKIP_BUILD:-0}" = "1" ]; then
        skip "T9.04 build (SKIP_BUILD=1)"
        return 0
    fi

    if ! vm_has_cmd "make" || ! vm_has_cmd "gcc"; then
        skip "T9.04 build (no make/gcc in VM)"
        return 0
    fi

    local src_dir
    src_dir=$(vm "find ${T9_DIR} -maxdepth 1 -type d | tail -1" 2>/dev/null | tr -d ' \r')

    local jobs
    jobs=$(vm "nproc 2>/dev/null || echo 2" 2>/dev/null | tr -d ' \r')
    echo "  Building with -j${jobs} (this may take a long time)..."

    local start end rc
    start=$(date +%s)
    vm "cd ${src_dir} && make -j${jobs} 2>&1" >> "$T9_LOG" 2>&1
    rc=$?
    end=$(date +%s)
    local dur=$((end - start))

    local obj_count ko_count eio build_err
    obj_count=$(vm "find ${src_dir} -name '*.o' -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d ' \r')
    ko_count=$(vm "find ${src_dir} -name '*.ko' -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d ' \r')
    eio=$(count_eio "$T9_LOG")
    build_err=$(grep -ciE '^make.*\[.*Error|fatal error|cannot find|No such file' "$T9_LOG" 2>/dev/null || echo 0)
    echo "  .o=${obj_count}, .ko=${ko_count}, exit=${rc}, EIO=${eio}, make_err=${build_err}, dur=${dur}s"

    if [ "$rc" -eq 0 ] && [ "$eio" -eq 0 ]; then
        ok "Kernel build succeeded (${obj_count} .o, ${ko_count} .ko in ${dur}s)"
    else
        ng "T9.04 build (exit=${rc}, EIO=${eio}, make_err=${build_err})"
    fi

    check_kernel_state "T9.04 build" "$base"
}

# ============================================================
# T9.05: rm -rf (cleanup)
# ============================================================
test_t5_delete() {
    section "T9.05: rm -rf (cleanup)"
    local base=$(dmesg_line_count)

    echo "  Removing ${T9_DIR} ..."
    local start end rc
    start=$(date +%s)
    vm "rm -rf ${T9_DIR} 2>&1" >> "$T9_LOG" 2>&1
    rc=$?
    end=$(date +%s)
    local dur=$((end - start))

    local remaining eio
    remaining=$(vm "test -d ${T9_DIR} && echo EXISTS || echo GONE" 2>/dev/null | tr -d ' \r')
    eio=$(count_eio "$T9_LOG")

    local remaining_files=0
    if [ "$remaining" = "EXISTS" ]; then
        remaining_files=$(vm "find ${T9_DIR} -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d ' \r')
        warn "rm -rf returned exit=${rc} but ${remaining_files} files remain, retrying..."
        local retry_start retry_end
        retry_start=$(date +%s)
        vm "rm -rf ${T9_DIR} 2>&1" >> "$T9_LOG" 2>&1
        retry_end=$(date +%s)
        dur=$((dur + retry_end - retry_start))
        remaining=$(vm "test -d ${T9_DIR} && echo EXISTS || echo GONE" 2>/dev/null | tr -d ' \r')
        remaining_files=$(vm "find ${T9_DIR} -type f 2>/dev/null | wc -l" 2>/dev/null | tr -d ' \r')
    fi

    echo "  status=${remaining}, remaining_files=${remaining_files}, EIO=${eio}, dur=${dur}s"

    if [ "$remaining" = "GONE" ] && [ "$eio" -eq 0 ]; then
        ok "Cleanup succeeded in ${dur}s"
    else
        ng "T9.05 rm -rf (status=${remaining}, files=${remaining_files}, EIO=${eio})"
    fi

    check_kernel_state "T9.05 delete" "$base"
}

# ============================================================
# 主流程
# ============================================================
echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  T9: Kernel Source E2E (pack/unpack/compile/delete)          ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo -e "${NC}"

# 前置检查
if ! vm_alive; then
    echo "ERROR: VM not reachable"
    exit 1
fi

if ! check_mount; then
    echo "ERROR: powerfs not mounted at ${MNT}"
    exit 1
fi

echo "VM kernel: $(vm 'uname -r' 2>/dev/null | tr -d ' \r')"
echo "Tarball:   ${HOST_TARBALL}"
echo "Skip build: ${SKIP_BUILD:-0}"
echo ""

# 执行测试
should_run 1 && test_t1_copy
should_run 2 && test_t2_unpack
should_run 3 && test_t3_defconfig
should_run 4 && test_t4_build
should_run 5 && test_t5_delete

# 汇总
echo ""
echo "============================================================"
echo "  T9 Summary: kernel source E2E on PowerFS"
echo "============================================================"
echo "  PASS: ${PASS}"
echo "  FAIL: ${FAIL}"
echo "  WARN: ${WARN}"
echo "  SKIP: ${SKIP}"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "  Failed: ${FAILED_TEST}"
    echo ""
fi

if [ "$FAIL" -eq 0 ]; then
    echo "RESULT: ALL PASS — kernel source E2E on PowerFS succeeded"
    exit 0
else
    echo "RESULT: ${FAIL} FAILURE(S)"
    exit 1
fi
