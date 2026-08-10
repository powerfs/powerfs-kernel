#!/bin/bash
# PowerFS 内核文件系统测试全量执行器
#
# 按实施顺序串行执行 T1-T8 所有阶段, 支持门禁、日志隔离、汇总报告.
#
# 实施顺序 (来自 kernel-test-plan.md 第4章):
#   T1 (VFS 基础) → T2 (正确性) → T3 (布局功能 K1-K4) → T4 (集成)
#                 → T8 (持久化) → T5 (性能) → T6 (稳定性) → T7 (可靠性)
#
# 每阶段全部 PASS 才进入下一阶段 (门禁原则, 失败即停止).
#
# 用法:
#   ./run_all_tests.sh                # 全量测试 (含环境准备: service start + deploy + mount)
#   ./run_all_tests.sh --no-env       # 跳过环境准备 (假设已 deploy + mount)
#   ./run_all_tests.sh -c             # 失败时继续 (默认失败即停止, 符合门禁原则)
#   ./run_all_tests.sh -s T1 -s T2    # 仅运行指定阶段 (可多次指定)
#   ./run_all_tests.sh --no-env -s T3 # 仅 T3 (含 K1-K4)
#
# 退出码: 0=全通过, 1=有失败, 2=环境失败, 3=参数错误

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VM_DIR="${SCRIPT_DIR}"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
POWERFS_MOD_DIR="${PROJECT_ROOT}/kernel/powerfs_mod"
RESULT_DIR="${VM_DIR}/output/test-results"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
RUN_ID="run_${TIMESTAMP}"
RUN_DIR="${RESULT_DIR}/${RUN_ID}"
SUMMARY_FILE="${RUN_DIR}/summary.log"
COMBINED_LOG="${RUN_DIR}/combined.log"

# 颜色输出
if [ -t 1 ]; then
    C_RED='\033[0;31m'
    C_GREEN='\033[0;32m'
    C_YELLOW='\033[0;33m'
    C_CYAN='\033[0;36m'
    C_BOLD='\033[1m'
    C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_BOLD=''; C_RESET=''
fi

# 参数解析
NO_ENV=0
CONTINUE_ON_FAIL=0
SELECTED_STAGES=()

usage() {
    cat <<EOF
Usage: $0 [OPTIONS] [STAGES...]

Options:
  --no-env          Skip environment preparation (assume services + mount ready)
  -c, --continue    Continue on failure (default: stop on first failure, gate mode)
  -s STAGE          Run only specified stage (T1/T2/T3/T4/T5/T6/T7/T8, repeatable)
  -h, --help        Show this help

Examples:
  $0                          # Full run with env preparation
  $0 --no-env                 # Skip env prep
  $0 --no-env -s T1 -s T2     # Only T1 and T2
  $0 -c                       # Continue on failure (debug mode)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --no-env)    NO_ENV=1; shift ;;
        -c|--continue) CONTINUE_ON_FAIL=1; shift ;;
        -s)          SELECTED_STAGES+=("$2"); shift 2 ;;
        -h|--help)   usage; exit 0 ;;
        *)           echo "Unknown option: $1"; usage; exit 3 ;;
    esac
done

# 测试阶段定义 (顺序: T1→T2→T3→T4→T8→T5→T6→T7)
# 格式: "STAGE_ID:DESCRIPTION:SCRIPT:ESTIMATED_MINUTES"
STAGES=(
    "T1:VFS basic operations:test_t1_vfs_basic.sh:5"
    "T2:Filesystem correctness:test_t2_correctness.sh:10"
    "T3:Layout features (K1-K4):test_k3_stripe.sh:15"
    "T4:Integration (FUSE<->kernel):test_t4_integration.sh:10"
    "T8:Data persistence:test_t8_persistence.sh:15"
    "T5:Performance (fio/mdtest):test_t5_performance.sh:30"
    "T6:Stability:test_t6_stability.sh:60"
    "T7:Reliability (fault injection):test_t7_reliability.sh:20"
)

should_run_stage() {
    local stage="$1"
    if [ ${#SELECTED_STAGES[@]} -eq 0 ]; then
        return 0  # 未指定则全跑
    fi
    for s in "${SELECTED_STAGES[@]}"; do
        if [ "$s" = "$stage" ]; then
            return 0
        fi
    done
    return 1
}

# 创建运行目录
mkdir -p "${RUN_DIR}"

# 全局状态
GLOBAL_PASS=0
GLOBAL_FAIL=0
GLOBAL_SKIP=0
STAGE_RESULTS=()  # "STAGE:STATUS:DURATION_SEC"
START_EPOCH=$(date +%s)

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "${COMBINED_LOG}"
}

log_section() {
    local msg="$1"
    echo "" | tee -a "${COMBINED_LOG}"
    echo -e "${C_CYAN}${C_BOLD}================================================================${C_RESET}" | tee -a "${COMBINED_LOG}"
    echo -e "${C_CYAN}${C_BOLD} ${msg}${C_RESET}" | tee -a "${COMBINED_LOG}"
    echo -e "${C_CYAN}${C_BOLD}================================================================${C_RESET}" | tee -a "${COMBINED_LOG}"
}

# ============================================================
# 环境准备
# ============================================================
prepare_env() {
    log_section "Environment Preparation"

    if [ $NO_ENV -eq 1 ]; then
        log "SKIP env preparation (--no-env)"
        return 0
    fi

    log "Starting backend services..."
    cd "${VM_DIR}"
    if ./qemuctl.sh service start 2>&1 | tee -a "${COMBINED_LOG}" | tail -10; then
        log "Backend services started"
    else
        log "ERROR: service start failed"
        return 1
    fi

    log "Checking QEMU status..."
    local qemu_pid
    qemu_pid=$(pgrep -f "qemu-system-x86_64.*bzImage" 2>/dev/null | head -1)
    if [ -z "$qemu_pid" ]; then
        log "QEMU not running, deploying..."
        if ./qemuctl.sh deploy 2>&1 | tee -a "${COMBINED_LOG}" | tail -20; then
            log "QEMU deployed"
        else
            log "ERROR: QEMU deploy failed"
            return 1
        fi
        sleep 15
    else
        log "QEMU already running (PID: ${qemu_pid})"
    fi

    log "Mounting powerfs in VM..."
    # mount 由 test_t1_vfs_basic.sh T1 阶段负责; 这里仅做兜底检查
    if ! sshpass -p powerfs ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
         -o LogLevel=ERROR -o ConnectTimeout=5 -p 2223 root@localhost \
         "mount | grep -q 'on /mnt/pfs type powerfs'" 2>/dev/null; then
        log "powerfs not mounted, will be mounted by T1 stage"
    else
        log "powerfs already mounted"
    fi

    return 0
}

# ============================================================
# 执行单个测试阶段
# ============================================================
run_stage() {
    local stage_id="$1"
    local stage_desc="$2"
    local script="$3"
    local est_min="$4"

    local script_path="${VM_DIR}/${script}"

    if ! should_run_stage "$stage_id"; then
        log "SKIP ${stage_id} (not selected)"
        GLOBAL_SKIP=$((GLOBAL_SKIP + 1))
        STAGE_RESULTS+=("${stage_id}:SKIP:0")
        return 0
    fi

    if [ ! -f "${script_path}" ]; then
        log "ERROR: ${stage_id} script not found: ${script_path}"
        GLOBAL_FAIL=$((GLOBAL_FAIL + 1))
        STAGE_RESULTS+=("${stage_id}:MISSING:0")
        return 1
    fi

    log_section "${stage_id}: ${stage_desc} (estimated ~${est_min} min)"

    local stage_log="${RUN_DIR}/${stage_id}_${script}.log"
    local stage_start=$(date +%s)

    log "Running: bash ${script_path}"
    log "Stage log: ${stage_log}"

    # 执行测试脚本, 实时输出到 stdout 和 stage_log
    if bash "${script_path}" 2>&1 | tee "${stage_log}"; then
        local stage_end=$(date +%s)
        local duration=$((stage_end - stage_start))
        log "${stage_id} PASSED (duration: ${duration}s)"
        GLOBAL_PASS=$((GLOBAL_PASS + 1))
        STAGE_RESULTS+=("${stage_id}:PASS:${duration}")
        return 0
    else
        local stage_end=$(date +%s)
        local duration=$((stage_end - stage_start))
        local rc=$?
        log "${stage_id} FAILED (rc=${rc}, duration: ${duration}s)"
        GLOBAL_FAIL=$((GLOBAL_FAIL + 1))
        STAGE_RESULTS+=("${stage_id}:FAIL:${duration}")
        return 1
    fi
}

# ============================================================
# 汇总报告
# ============================================================
generate_summary() {
    local end_epoch=$(date +%s)
    local total_duration=$((end_epoch - START_EPOCH))

    cat > "${SUMMARY_FILE}" <<EOF
================================================================
PowerFS Kernel Filesystem Test Summary
================================================================
Run ID:         ${RUN_ID}
Run Directory:  ${RUN_DIR}
Start Time:     $(date -d @${START_EPOCH} '+%Y-%m-%d %H:%M:%S')
End Time:       $(date -d @${end_epoch} '+%Y-%m-%d %H:%M:%S')
Total Duration: ${total_duration}s ($(( total_duration / 60 ))m $(( total_duration % 60 ))s)

Stages:
EOF

    echo "" >> "${SUMMARY_FILE}"
    printf "%-6s %-10s %s\n" "STAGE" "STATUS" "DURATION" >> "${SUMMARY_FILE}"
    printf "%-6s %-10s %s\n" "-----" "------" "--------" >> "${SUMMARY_FILE}"
    for r in "${STAGE_RESULTS[@]}"; do
        local sid="${r%%:*}"
        local rest="${r#*:}"
        local status="${rest%%:*}"
        local dur="${rest#*:}"
        printf "%-6s %-10s %ss\n" "$sid" "$status" "$dur" >> "${SUMMARY_FILE}"
    done

    cat >> "${SUMMARY_FILE}" <<EOF

Counts:
  PASS: ${GLOBAL_PASS}
  FAIL: ${GLOBAL_FAIL}
  SKIP: ${GLOBAL_SKIP}

Result: $([ ${GLOBAL_FAIL} -eq 0 ] && echo "ALL PASSED" || echo "HAS FAILURES")
================================================================
EOF

    # 输出到屏幕
    cat "${SUMMARY_FILE}"
    log "Summary saved to: ${SUMMARY_FILE}"
}

# ============================================================
# 主流程
# ============================================================
main() {
    echo -e "${C_CYAN}${C_BOLD}"
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  PowerFS Kernel Filesystem Test Runner                       ║"
    echo "║  Stages: T1 -> T2 -> T3 -> T4 -> T8 -> T5 -> T6 -> T7       ║"
    echo "║  Gate: each stage must PASS before next stage                ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo -e "${C_RESET}"

    log "Run ID: ${RUN_ID}"
    log "Run directory: ${RUN_DIR}"
    log "Combined log: ${COMBINED_LOG}"
    log "Options: NO_ENV=${NO_ENV}, CONTINUE_ON_FAIL=${CONTINUE_ON_FAIL}"
    if [ ${#SELECTED_STAGES[@]} -gt 0 ]; then
        log "Selected stages: ${SELECTED_STAGES[*]}"
    else
        log "Selected stages: ALL"
    fi

    # 环境准备
    if ! prepare_env; then
        log "FATAL: environment preparation failed"
        generate_summary
        exit 2
    fi

    # 按顺序执行各阶段
    local stage_failed=0
    for entry in "${STAGES[@]}"; do
        local stage_id="${entry%%:*}"
        local rest="${entry#*:}"
        local stage_desc="${rest%%:*}"
        local rest2="${rest#*:}"
        local script="${rest2%%:*}"
        local est_min="${rest2##*:}"

        # 若前序阶段失败且非 continue 模式, 跳过后续
        if [ $stage_failed -eq 1 ] && [ $CONTINUE_ON_FAIL -eq 0 ]; then
            if should_run_stage "$stage_id"; then
                log "SKIP ${stage_id} (due to previous failure, gate mode)"
                GLOBAL_SKIP=$((GLOBAL_SKIP + 1))
                STAGE_RESULTS+=("${stage_id}:GATE_SKIPPED:0")
            fi
            continue
        fi

        if ! run_stage "$stage_id" "$stage_desc" "$script" "$est_min"; then
            stage_failed=1
            log "Stage ${stage_id} FAILED"
            if [ $CONTINUE_ON_FAIL -eq 0 ]; then
                log "Stopping due to gate mode (use -c to continue on failure)"
            fi
        fi
    done

    # 汇总
    log_section "Test Summary"
    generate_summary

    # 退出码
    if [ ${GLOBAL_FAIL} -eq 0 ]; then
        log "All stages PASSED"
        exit 0
    else
        log "Some stages FAILED"
        exit 1
    fi
}

main "$@"
