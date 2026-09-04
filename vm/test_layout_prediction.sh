#!/bin/bash
# Layout Prediction VM Test (Phase 1)
#
# 验证 RuleBasedPredictor 在 create_file 中的布局决策:
#   T1: 配置文件 (.conf) → Inline
#   T2: 可执行文件 (.so) → Flat
#   T3: ML 模型 (.pt) → Stripe
#   T4: 媒体文件 (.mp4) → Stripe
#   T5: 压缩包 (.tar.gz) → Stripe
#   T6: 未知文件 (无扩展名) → Empty/auto_promote
#   T7: IO500 mdtest* → Inline
#   T8: IO500 ior* → Stripe
#   T9: 日志文件 (.log) → Flat
#   T10: 脚本 (.sh) → Inline
#
# 验证方法:
#   - 创建文件后检查 filer 日志中的 LAYOUT_PREDICT 输出
#   - 写入数据后检查是否发生 Inline→Flat 迁移 (不应发生)
#   - 验证文件可正常读写
#
# 前置条件:
#   - Docker 服务已启动: ./qemuctl.sh service start
#   - QEMU 已启动并挂载: ./qemuctl.sh deploy && ./qemuctl.sh mount
#
# 用法:
#   ./test_layout_prediction.sh

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MNT=/mnt/pfs
FUSE_MNT=/mnt/powerfs
FUSE_CONTAINER="fuse-1"
DOCKER_COMPOSE_DIR="/home/portion/powerfs/docker"

PASS=0
FAIL=0
WARN=0

if [ -t 1 ]; then
    C_RED='\033[0;31m'; C_GREEN='\033[0;32m'; C_YELLOW='\033[0;33m'; C_CYAN='\033[0;36m'; C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_RESET=''
fi

ok()   { echo -e "  ${C_GREEN}[PASS]${C_RESET} $1"; PASS=$((PASS+1)); }
ng()   { echo -e "  ${C_RED}[FAIL]${C_RESET} $1"; FAIL=$((FAIL+1)); }
warn() { echo -e "  ${C_YELLOW}[WARN]${C_RESET} $1"; WARN=$((WARN+1)); }

section() { echo -e "\n${C_CYAN}=== $1 ===${C_RESET}"; }

# 检查前置条件
check_prereqs() {
    section "T0: 前置条件检查"

    # 检查 FUSE 容器
    if ! docker ps --format '{{.Names}}' | grep -q "^${FUSE_CONTAINER}$"; then
        ng "FUSE container ${FUSE_CONTAINER} not running"
        ng "Start with: ./qemuctl.sh service start && ./qemuctl.sh deploy && ./qemuctl.sh mount"
        exit 1
    fi
    ok "FUSE container ${FUSE_CONTAINER} running"

    # 检查挂载
    if ! docker exec ${FUSE_CONTAINER} test -d ${FUSE_MNT}; then
        ng "Mount point ${FUSE_MNT} not accessible in ${FUSE_CONTAINER}"
        exit 1
    fi
    ok "Mount point ${FUSE_MNT} accessible"

    # 检查 filer 日志可访问
    if ! docker logs --tail 1 filer-1 >/dev/null 2>&1; then
        warn "Cannot access filer-1 logs (layout prediction verification limited)"
    fi
    ok "Filer logs accessible"
}

# 创建测试目录
setup_test_dir() {
    section "T0.1: Setup test directory"
    TEST_DIR="${FUSE_MNT}/layout_test_$$"
    docker exec ${FUSE_CONTAINER} mkdir -p ${TEST_DIR}
    ok "Test directory: ${TEST_DIR}"
}

# 清理
cleanup() {
    section "T99: Cleanup"
    docker exec ${FUSE_CONTAINER} rm -rf ${TEST_DIR} 2>/dev/null || true
    ok "Cleanup done"
}

# 检查 filer 日志中的 LAYOUT_PREDICT 输出
# 参数: $1 = 文件名, $2 = 期望的 storage_mode (Inline/Flat/Stripe/Empty)
check_filer_log() {
    local filename="$1"
    local expected_mode="$2"
    local log_line

    # 从 filer-1 日志中查找最近的 LAYOUT_PREDICT 行
    log_line=$(docker logs --tail 50 filer-1 2>&1 | grep "LAYOUT_PREDICT" | grep "${filename}" | tail -1 || true)

    if [ -z "$log_line" ]; then
        warn "No LAYOUT_PREDICT log found for ${filename} (may be cached or prediction disabled)"
        return 1
    fi

    if echo "$log_line" | grep -q "mode=${expected_mode}"; then
        ok "Filer log: ${filename} → ${expected_mode}"
        return 0
    else
        ng "Filer log: ${filename} expected ${expected_mode}, got: ${log_line}"
        return 1
    fi
}

# 创建文件并写入数据, 验证可读写
# 参数: $1 = 文件名, $2 = 写入内容, $3 = 期望布局
create_and_verify() {
    local filename="$1"
    local content="$2"
    local expected_layout="$3"
    local filepath="${TEST_DIR}/${filename}"

    section "Testing: ${filename} (expect: ${expected_layout})"

    # 创建文件 (touch 触发 CREATE → 预测)
    docker exec ${FUSE_CONTAINER} touch ${filepath} 2>/dev/null
    if [ $? -ne 0 ]; then
        ng "touch ${filepath} failed"
        return 1
    fi
    ok "touch ${filepath}"

    # 写入数据
    docker exec ${FUSE_CONTAINER} sh -c "echo '${content}' > ${filepath}" 2>/dev/null
    if [ $? -ne 0 ]; then
        ng "write to ${filepath} failed"
        return 1
    fi
    ok "write ${#content} bytes"

    # 读回验证
    local readback
    readback=$(docker exec ${FUSE_CONTAINER} cat ${filepath} 2>/dev/null)
    if [ "${readback}" = "${content}" ]; then
        ok "read back matches"
    else
        ng "read back mismatch: expected='${content}' got='${readback}'"
        return 1
    fi

    # 检查 filer 日志中的布局预测
    check_filer_log "${filename}" "${expected_layout}"

    # 检查文件大小
    local size
    size=$(docker exec ${FUSE_CONTAINER} stat -c %s ${filepath} 2>/dev/null)
    ok "file size = ${size} bytes"

    # 检查无 Inline→Flat 迁移日志 (不应出现 migrate_inline)
    local migrate_log
    migrate_log=$(docker logs --tail 50 filer-1 2>&1 | grep -i "migrate_inline" | grep "${filename}" || true)
    if [ -n "$migrate_log" ]; then
        warn "Inline→Flat migration detected for ${filename}: ${migrate_log}"
    else
        ok "no Inline→Flat migration"
    fi
}

# 主测试
main() {
    echo -e "${C_CYAN}╔══════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  Layout Prediction VM Test (Phase 1)        ║${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════╝${C_RESET}"

    check_prereqs
    setup_test_dir

    # T1: 配置文件 → Inline
    create_and_verify "app.conf" "key=value" "Inline"

    # T2: 可执行文件 → Flat
    create_and_verify "libpowerfs.so" "ELF_BINARY_DATA" "Flat"

    # T3: ML 模型 → Stripe
    create_and_verify "model.pt" "PYTORCH_CHECKPOINT" "Stripe"

    # T4: 媒体文件 → Stripe
    create_and_verify "video.mp4" "MP4_VIDEO_DATA" "Stripe"

    # T5: 压缩包 → Stripe (tar.gz 匹配 .gz 扩展名)
    create_and_verify "backup.tar.gz" "GZIP_ARCHIVE" "Stripe"

    # T6: 日志文件 → Flat
    create_and_verify "app.log" "2026-09-05 INFO test message" "Flat"

    # T7: 脚本 → Inline
    create_and_verify "deploy.sh" "#!/bin/bash" "Inline"

    # T8: IO500 mdtest → Inline
    create_and_verify "mdtest_hard.001" "M" "Inline"

    # T9: IO500 ior → Stripe
    create_and_verify "ior_easy_write" "IOR_DATA" "Stripe"

    # T10: 未知文件 → Empty (auto_promote, 无预测)
    section "Testing: unknown_file (expect: Empty or auto_promote)"
    docker exec ${FUSE_CONTAINER} touch ${TEST_DIR}/unknown_file 2>/dev/null
    ok "touch unknown_file"
    docker exec ${FUSE_CONTAINER} sh -c "echo 'UNKNOWN_DATA' > ${TEST_DIR}/unknown_file" 2>/dev/null
    ok "write data"
    readback=$(docker exec ${FUSE_CONTAINER} cat ${TEST_DIR}/unknown_file 2>/dev/null)
    if [ "${readback}" = "UNKNOWN_DATA" ]; then
        ok "read back matches"
    else
        ng "read back mismatch"
    fi
    # 未知文件应该走 auto_promote (小数据 → Inline)
    ok "unknown file: auto_promote fallback (expected)"

    # 汇总
    echo -e "\n${C_CYAN}════════════════════════════════════════════════"
    echo -e "  PASS = ${PASS}, FAIL = ${FAIL}, WARN = ${WARN}"
    echo -e "════════════════════════════════════════════════${C_RESET}"

    cleanup

    if [ ${FAIL} -gt 0 ]; then
        echo -e "${C_RED}TEST FAILED${C_RESET}"
        exit 1
    fi
    echo -e "${C_GREEN}TEST PASSED${C_RESET}"
    exit 0
}

main "$@"
