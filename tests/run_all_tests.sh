#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# PowerFS 内核文件系统 T1-T8 自动化测试执行器
#
# 按 kernel-test-plan.md 实施顺序串行执行所有测试阶段:
#   T1 (VFS 基础) → T2 (正确性) → T3 (布局功能 K1-K4)
#                                    ↓
#   T7 (可靠性) ← T6 (稳定性) ← T5 (性能) ← T4 (集成) ← T8 (持久化)
#
# 用法:
#   ./run_all_tests.sh                     # 全量: 环境准备 + T1-T8
#   ./run_all_tests.sh --no-env            # 跳过环境准备 (假设已 deploy+mount)
#   ./run_all_tests.sh -c                  # 失败时继续 (默认失败即停止)
#   ./run_all_tests.sh -s T1               # 仅运行 T1
#   ./run_all_tests.sh -s T3               # 仅运行 T3 (K1-K4)
#   ./run_all_tests.sh -s T1 T2 T8         # 仅运行 T1, T2, T8
#   ./run_all_tests.sh --no-env -s T4      # 跳过环境准备, 仅 T4
#
# 退出码:
#   0  全部通过
#   1  有失败用例
#   2  环境准备失败
#   3  参数错误

set -u

# ============================================================
# 全局变量
# ============================================================
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VM_DIR="$(cd "${SCRIPT_DIR}/../vm" && pwd)"
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
    C_DIM='\033[2m'
    C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_BOLD=''; C_DIM=''; C_RESET=''
fi

# 选项
SKIP_ENV=0
CONTINUE_ON_FAILURE=0
SELECTED_STAGES=()

# 全局统计
declare -A STAGE_STATUS        # STAGE_STATUS[T1]="PASS"|"FAIL"|"SKIP"|"RUN"
declare -A STAGE_DURATION      # STAGE_DURATION[T1]=120 (秒)
declare -A STAGE_PASS_COUNT    # STAGE_PASS_COUNT[T1]=45
declare -A STAGE_FAIL_COUNT    # STAGE_FAIL_COUNT[T1]=0
declare -A STAGE_WARN_COUNT
declare -A STAGE_SKIP_COUNT
declare -A STAGE_LOG_FILE      # STAGE_LOG_FILE[T1]=path

TOTAL_START_TIME=0
TOTAL_END_TIME=0

# ============================================================
# 工具函数
# ============================================================
log()   { echo -e "$1" | tee -a "${COMBINED_LOG}"; }
info()  { log "  ${C_DIM}[INFO]${C_RESET} $1"; }
warn()  { log "  ${C_YELLOW}[WARN]${C_RESET} $1"; }
err()   { log "  ${C_RED}[ERROR]${C_RESET} $1"; }

banner() {
    local msg="$1"
    local len=${#msg}
    local bar=""
    local i
    for ((i=0; i<len+4; i++)); do bar+="═"; done
    log ""
    log "${C_CYAN}╔${bar}╗${C_RESET}"
    log "${C_CYAN}║  ${msg}  ║${C_RESET}"
    log "${C_CYAN}╚${bar}╝${C_RESET}"
    log ""
}

section() {
    log ""
    log "${C_CYAN}━━━ $1 ━━━${C_RESET}"
}

# 当前时间戳 (秒)
now() { date +%s; }

# 格式化耗时
fmt_duration() {
    local sec=$1
    local h=$((sec / 3600))
    local m=$(( (sec % 3600) / 60 ))
    local s=$((sec % 60))
    if [ $h -gt 0 ]; then
        printf "%dh%02dm%02ds" $h $m $s
    elif [ $m -gt 0 ]; then
        printf "%dm%02ds" $m $s
    else
        printf "%ds" $s
    fi
}

# 从测试脚本输出中提取 PASS/FAIL/WARN/SKIP 计数
# 参数 $1: 日志文件路径
# 输出: 写入 STAGE_PASS_COUNT 等关联数组
extract_counts() {
    local log_file="$1"
    local stage="$2"
    local pass fail warn skip

    pass=$(grep -oE 'PASS:\s*[0-9]+' "$log_file" 2>/dev/null | tail -1 | grep -oE '[0-9]+' || echo 0)
    fail=$(grep -oE 'FAIL:\s*[0-9]+' "$log_file" 2>/dev/null | tail -1 | grep -oE '[0-9]+' || echo 0)
    warn=$(grep -oE 'WARN:\s*[0-9]+' "$log_file" 2>/dev/null | tail -1 | grep -oE '[0-9]+' || echo 0)
    skip=$(grep -oE 'SKIP:\s*[0-9]+' "$log_file" 2>/dev/null | tail -1 | grep -oE '[0-9]+' || echo 0)

    STAGE_PASS_COUNT[$stage]=$pass
    STAGE_FAIL_COUNT[$stage]=$fail
    STAGE_WARN_COUNT[$stage]=$warn
    STAGE_SKIP_COUNT[$stage]=$skip
}

# ============================================================
# 环境准备
# ============================================================
prepare_environment() {
    banner "Stage 0: Environment Preparation"

    local env_start
    env_start=$(now)

    # 1. 检查 powerfs.ko 是否存在
    if [ ! -f "${POWERFS_MOD_DIR}/powerfs.ko" ]; then
        warn "powerfs.ko 不存在, 需要部署"
    fi

    # 2. 启动后端服务
    section "Step 1: Start backend services"
    if "${VM_DIR}/qemuctl.sh" service start 2>&1 | tee -a "${COMBINED_LOG}"; then
        info "后端服务已启动"
    else
        err "后端服务启动失败"
        return 1
    fi

    # 检查服务健康
    sleep 3
    local svc_count
    svc_count=$(docker ps --format '{{.Names}}' 2>/dev/null | grep -cE 'master-1|volume-1|filer-1' || echo 0)
    if [ "$svc_count" -lt 3 ]; then
        err "后端服务不完整 (仅 ${svc_count}/3)"
        return 1
    fi
    info "后端服务健康 (${svc_count}/3)"

    # 3. 部署 powerfs.ko + 启动 QEMU
    section "Step 2: Deploy powerfs.ko and start QEMU"
    if "${VM_DIR}/qemuctl.sh" deploy 2>&1 | tee -a "${COMBINED_LOG}"; then
        info "QEMU 部署成功"
    else
        err "QEMU 部署失败"
        return 1
    fi

    # 等待 QEMU 启动
    sleep 10
    if ! pgrep -f "qemu-system-x86_64.*bzImage" >/dev/null 2>&1; then
        err "QEMU 进程未运行"
        return 1
    fi
    info "QEMU 运行中 (PID: $(pgrep -f 'qemu-system-x86_64.*bzImage' | head -1))"

    # 4. 挂载 powerfs
    section "Step 3: Mount powerfs in VM"
    if "${VM_DIR}/qemuctl.sh" mount 2>&1 | tee -a "${COMBINED_LOG}"; then
        info "powerfs 挂载成功"
    else
        err "powerfs 挂载失败"
        return 1
    fi

    # 5. 环境验证
    section "Step 4: Verify environment"
    if "${VM_DIR}/qemuctl.sh" status 2>&1 | tee -a "${COMBINED_LOG}" | grep -q "SSH 连接: 正常"; then
        info "环境验证通过"
    else
        warn "环境状态检查有警告, 继续尝试..."
    fi

    local env_end
    env_end=$(now)
    info "环境准备耗时: $(fmt_duration $((env_end - env_start)))"

    return 0
}

# ============================================================
# 单阶段测试运行
# ============================================================
# run_stage STAGE_NAME SCRIPT_PATH
run_stage() {
    local stage="$1"
    local script="$2"
    local script_name
    script_name=$(basename "$script")

    STAGE_STATUS[$stage]="RUN"
    local stage_log="${RUN_DIR}/${stage}_${script_name}.log"
    STAGE_LOG_FILE[$stage]="$stage_log"

    banner "Stage ${stage}: ${script_name}"

    local start
    start=$(now)

    info "脚本: ${script}"
    info "日志: ${stage_log}"
    info "开始时间: $(date '+%Y-%m-%d %H:%M:%S')"

    # 运行测试脚本, 输出同时显示到终端和日志文件
    if bash "$script" 2>&1 | tee "$stage_log" | tee -a "${COMBINED_LOG}"; then
        STAGE_STATUS[$stage]="PASS"
        log ""
        log "  ${C_GREEN}[STAGE ${stage} PASSED]${C_RESET}"
    else
        STAGE_STATUS[$stage]="FAIL"
        log ""
        log "  ${C_RED}[STAGE ${stage} FAILED]${C_RESET}"
    fi

    local end
    end=$(now)
    local duration=$((end - start))
    STAGE_DURATION[$stage]=$duration

    extract_counts "$stage_log" "$stage"

    info "耗时: $(fmt_duration $duration)"
    info "统计: PASS=${STAGE_PASS_COUNT[$stage]} FAIL=${STAGE_FAIL_COUNT[$stage]} WARN=${STAGE_WARN_COUNT[$stage]} SKIP=${STAGE_SKIP_COUNT[$stage]}"

    # 返回脚本退出状态 (0=成功, 非0=失败)
    if [ "${STAGE_STATUS[$stage]}" = "PASS" ]; then
        return 0
    else
        return 1
    fi
}

# 运行 T3 (包含 K1-K4 四个子阶段)
run_t3_layout() {
    banner "Stage T3: Layout Functional Tests (K1-K4)"

    local t3_start
    t3_start=$(now)
    local t3_failed=0

    local sub_stages=(
        "K1:test_k1_layout.sh:协议对齐 + Flat 修复"
        "K2:test_k2_inline.sh:Inline 小文件 + 自动迁移"
        "K3:test_k3_stripe.sh:Stripe/WideStripe 多卷读写"
        "K4:test_k4_reliability.sh:可靠性 failover + CRC32 + EC"
    )

    for entry in "${sub_stages[@]}"; do
        local sub="${entry%%:*}"
        local rest="${entry#*:}"
        local script="${rest%%:*}"
        local desc="${rest#*:}"

        log ""
        log "${C_BOLD}--- T3.${sub}: ${desc} ---${C_RESET}"

        if ! run_stage "T3${sub}" "${VM_DIR}/${script}"; then
            t3_failed=1
            if [ "$CONTINUE_ON_FAILURE" -eq 0 ]; then
                warn "T3${sub} 失败, 跳过 T3 后续子阶段"
                # 标记剩余子阶段为 SKIP
                local skip_remaining=0
                for e in "${sub_stages[@]}"; do
                    if [ "$skip_remaining" -eq 1 ]; then
                        local sn="${e%%:*}"
                        STAGE_STATUS["T3${sn}"]="SKIP"
                    fi
                    if [ "$e" = "$entry" ]; then
                        skip_remaining=1
                    fi
                done
                break
            fi
        fi
    done

    # T3 整体状态
    local t3_end
    t3_end=$(now)
    STAGE_DURATION[T3]=$((t3_end - t3_start))

    # 汇总 T3 子阶段计数
    local total_pass=0 total_fail=0 total_warn=0 total_skip=0
    for entry in "${sub_stages[@]}"; do
        local sub="${entry%%:*}"
        total_pass=$((total_pass + ${STAGE_PASS_COUNT[T3${sub}]:-0}))
        total_fail=$((total_fail + ${STAGE_FAIL_COUNT[T3${sub}]:-0}))
        total_warn=$((total_warn + ${STAGE_WARN_COUNT[T3${sub}]:-0}))
        total_skip=$((total_skip + ${STAGE_SKIP_COUNT[T3${sub}]:-0}))
    done
    STAGE_PASS_COUNT[T3]=$total_pass
    STAGE_FAIL_COUNT[T3]=$total_fail
    STAGE_WARN_COUNT[T3]=$total_warn
    STAGE_SKIP_COUNT[T3]=$total_skip

    if [ $t3_failed -eq 0 ]; then
        STAGE_STATUS[T3]="PASS"
        log ""
        log "  ${C_GREEN}[STAGE T3 PASSED]${C_RESET} (耗时 $(fmt_duration ${STAGE_DURATION[T3]}))"
        return 0
    else
        STAGE_STATUS[T3]="FAIL"
        log ""
        log "  ${C_RED}[STAGE T3 FAILED]${C_RESET} (耗时 $(fmt_duration ${STAGE_DURATION[T3]}))"
        return 1
    fi
}

# ============================================================
# 测试阶段定义 (按 kernel-test-plan.md 实施顺序)
# ============================================================
# 格式: "STAGE:SCRIPT:DESCRIPTION"
get_all_stages() {
    cat <<'EOF'
T1:test_t1_vfs_basic.sh:VFS 基础操作
T2:test_t2_correctness.sh:文件系统正确性
T3:RUN_T3_LAYOUT:布局功能 (K1-K4)
T4:test_t4_integration.sh:集成测试
T8:test_t8_persistence.sh:数据持久化
T5:test_t5_performance.sh:性能测试
T6:test_t6_stability.sh:稳定性测试
T7:test_t7_reliability.sh:可靠性测试
EOF
}

# 判断某阶段是否需要运行
should_run_stage() {
    local stage="$1"
    if [ ${#SELECTED_STAGES[@]} -eq 0 ]; then
        return 0  # 未指定则全部运行
    fi
    for s in "${SELECTED_STAGES[@]}"; do
        if [ "$s" = "$stage" ]; then
            return 0
        fi
    done
    return 1
}

# ============================================================
# 汇总报告
# ============================================================
print_summary() {
    local total_duration=$((TOTAL_END_TIME - TOTAL_START_TIME))

    banner "Test Results Summary"

    # 阶段明细表
    log "${C_BOLD}$(printf '%-6s %-12s %-8s %-8s %-8s %-8s %-10s' "Stage" "Status" "PASS" "FAIL" "WARN" "SKIP" "Duration")${C_RESET}"
    log "$(printf '%-6s %-12s %-8s %-8s %-8s %-8s %-10s' "------" "------------" "--------" "--------" "--------" "--------" "----------")"

    local total_pass=0 total_fail=0 total_warn=0 total_skip=0
    local passed=0 failed=0 skipped=0

    while IFS=: read -r stage script desc; do
        [ -z "$stage" ] && continue

        local status="${STAGE_STATUS[$stage]:-SKIP}"
        local pass="${STAGE_PASS_COUNT[$stage]:-0}"
        local fail="${STAGE_FAIL_COUNT[$stage]:-0}"
        local warn="${STAGE_WARN_COUNT[$stage]:-0}"
        local skip="${STAGE_SKIP_COUNT[$stage]:-0}"
        local dur="${STAGE_DURATION[$stage]:-0}"
        local dur_fmt
        dur_fmt=$(fmt_duration "$dur")

        # 颜色
        local status_colored
        case "$status" in
            PASS) status_colored="${C_GREEN}PASS${C_RESET}      " ;;
            FAIL) status_colored="${C_RED}FAIL${C_RESET}      " ;;
            SKIP) status_colored="${C_YELLOW}SKIP${C_RESET}      " ;;
            *)    status_colored="${C_DIM}N/A${C_RESET}       " ;;
        esac

        log "$(printf '%-6s %-22s %-8s %-8s %-8s %-8s %-10s' \
            "$stage" "$status_colored" "$pass" "$fail" "$warn" "$skip" "$dur_fmt")"

        total_pass=$((total_pass + pass))
        total_fail=$((total_fail + fail))
        total_warn=$((total_warn + warn))
        total_skip=$((total_skip + skip))

        case "$status" in
            PASS) passed=$((passed + 1)) ;;
            FAIL) failed=$((failed + 1)) ;;
            SKIP) skipped=$((skipped + 1)) ;;
        esac
    done < <(get_all_stages)

    log ""
    log "$(printf '%-6s %-12s %-8s %-8s %-8s %-8s %-10s' "------" "------------" "--------" "--------" "--------" "--------" "----------")"
    log "${C_BOLD}$(printf '%-6s %-12s %-8s %-8s %-8s %-8s %-10s' "TOTAL" "—" "$total_pass" "$total_fail" "$total_warn" "$total_skip" "$(fmt_duration $total_duration)")${C_RESET}"
    log ""

    # 阶段统计
    log "阶段统计: ${C_GREEN}PASS=${passed}${C_RESET}  ${C_RED}FAIL=${failed}${C_RESET}  ${C_YELLOW}SKIP=${skipped}${C_RESET}"
    log "总耗时: $(fmt_duration $total_duration)"
    log "运行 ID: ${RUN_ID}"
    log "日志目录: ${RUN_DIR}"
    log "合并日志: ${COMBINED_LOG}"
    log ""

    # 失败阶段明细
    if [ $failed -gt 0 ]; then
        log "${C_RED}失败阶段:${C_RESET}"
        while IFS=: read -r stage script desc; do
            [ -z "$stage" ] && continue
            if [ "${STAGE_STATUS[$stage]:-}" = "FAIL" ]; then
                log "  - ${stage}: ${desc}"
                log "    日志: ${STAGE_LOG_FILE[$stage]:-N/A}"
            fi
        done < <(get_all_stages)
        log ""
    fi

    # 排查建议
    if [ $failed -gt 0 ]; then
        log "${C_BOLD}排查建议:${C_RESET}"
        log "  1. 查看失败阶段日志: cat <日志路径>"
        log "  2. VM dmesg:          ./qemuctl.sh log powerfs"
        log "  3. 实时监控:          ./qemuctl.sh monitor powerfs"
        log "  4. 后端日志:          ./qemuctl.sh service log filer-1"
        log "  5. 重跑失败阶段:      ./run_all_tests.sh --no-env -s <STAGE>"
        log ""
    fi

    # 写入 summary 文件
    {
        echo "PowerFS Kernel Test Summary"
        echo "Run ID: ${RUN_ID}"
        echo "Date: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "Duration: $(fmt_duration $total_duration)"
        echo ""
        printf '%-6s %-8s %-6s %-6s %-6s %-6s %-10s\n' "Stage" "Status" "PASS" "FAIL" "WARN" "SKIP" "Duration"
        while IFS=: read -r stage script desc; do
            [ -z "$stage" ] && continue
            printf '%-6s %-8s %-6s %-6s %-6s %-6s %-10s\n' \
                "$stage" \
                "${STAGE_STATUS[$stage]:-SKIP}" \
                "${STAGE_PASS_COUNT[$stage]:-0}" \
                "${STAGE_FAIL_COUNT[$stage]:-0}" \
                "${STAGE_WARN_COUNT[$stage]:-0}" \
                "${STAGE_SKIP_COUNT[$stage]:-0}" \
                "$(fmt_duration ${STAGE_DURATION[$stage]:-0})"
        done < <(get_all_stages)
        echo ""
        echo "TOTAL: PASS=${passed} FAIL=${failed} SKIP=${skipped}"
        echo "Total counts: PASS=${total_pass} FAIL=${total_fail} WARN=${total_warn} SKIP=${total_skip}"
    } > "${SUMMARY_FILE}"

    info "汇总报告已保存: ${SUMMARY_FILE}"

    if [ $failed -gt 0 ]; then
        return 1
    fi
    return 0
}

# ============================================================
# 参数解析
# ============================================================
parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            --no-env)
                SKIP_ENV=1
                shift
                ;;
            -c|--continue)
                CONTINUE_ON_FAILURE=1
                shift
                ;;
            -s|--stage)
                if [ $# -lt 2 ]; then
                    echo "Error: --stage requires an argument" >&2
                    exit 3
                fi
                SELECTED_STAGES+=("$2")
                shift 2
                ;;
            -h|--help)
                print_help
                exit 0
                ;;
            *)
                echo "Error: unknown option '$1'" >&2
                print_help >&2
                exit 3
                ;;
        esac
    done

    # 验证选择的阶段
    if [ ${#SELECTED_STAGES[@]} -gt 0 ]; then
        local valid_stages
        valid_stages=$(get_all_stages | cut -d: -f1 | tr '\n' ' ')
        for s in "${SELECTED_STAGES[@]}"; do
            if ! echo "$valid_stages" | grep -qw "$s"; then
                echo "Error: invalid stage '$s'. Valid: ${valid_stages}" >&2
                exit 3
            fi
        done
    fi
}

print_help() {
    cat <<'EOF'
PowerFS 内核文件系统 T1-T8 自动化测试执行器

用法:
    ./run_all_tests.sh [OPTIONS] [-s STAGE ...]

选项:
    --no-env            跳过环境准备 (假设已 service start + deploy + mount)
    -c, --continue      失败时继续运行下一阶段 (默认失败即停止)
    -s, --stage STAGE   只运行指定阶段 (可多次指定)
    -h, --help          显示帮助

阶段 (按实施顺序):
    T1  VFS 基础操作
    T2  文件系统正确性
    T3  布局功能 (K1-K4)
    T4  集成测试
    T8  数据持久化
    T5  性能测试
    T6  稳定性测试
    T7  可靠性测试

示例:
    ./run_all_tests.sh                     # 全量测试 (含环境准备)
    ./run_all_tests.sh --no-env            # 跳过环境准备
    ./run_all_tests.sh -c                  # 失败时继续
    ./run_all_tests.sh -s T1 -s T2         # 仅 T1, T2
    ./run_all_tests.sh --no-env -s T3      # 仅 T3

输出:
    output/test-results/run_<timestamp>/
        summary.log                       汇总报告
        combined.log                      合并日志
        T1_test_t1_vfs_basic.sh.log       各阶段独立日志
        ...
EOF
}

# ============================================================
# 主流程
# ============================================================
main() {
    parse_args "$@"

    # 创建运行目录
    mkdir -p "${RUN_DIR}"

    TOTAL_START_TIME=$(now)

    banner "PowerFS Kernel Filesystem T1-T8 Automated Test"
    info "运行 ID: ${RUN_ID}"
    info "日志目录: ${RUN_DIR}"
    info "选项: SKIP_ENV=${SKIP_ENV} CONTINUE_ON_FAILURE=${CONTINUE_ON_FAILURE}"

    if [ ${#SELECTED_STAGES[@]} -gt 0 ]; then
        info "指定阶段: ${SELECTED_STAGES[*]}"
    else
        info "运行全部阶段: T1 T2 T3 T4 T8 T5 T6 T7"
    fi
    log ""

    # 环境准备
    if [ "$SKIP_ENV" -eq 0 ]; then
        if ! prepare_environment; then
            err "环境准备失败, 终止测试"
            TOTAL_END_TIME=$(now)
            STAGE_STATUS[ENV]="FAIL"
            print_summary
            exit 2
        fi
        STAGE_STATUS[ENV]="PASS"
        log ""
    else
        info "跳过环境准备 (--no-env)"
        STAGE_STATUS[ENV]="SKIP"
        log ""
    fi

    # 按顺序运行测试阶段
    local global_failed=0

    while IFS=: read -r stage script desc; do
        [ -z "$stage" ] && continue

        # 是否需要运行
        if ! should_run_stage "$stage"; then
            STAGE_STATUS[$stage]="SKIP"
            log "${C_DIM}  [SKIP] Stage ${stage} (未选择)${C_RESET}"
            continue
        fi

        # 检查是否因前序失败而跳过
        if [ $global_failed -eq 1 ] && [ "$CONTINUE_ON_FAILURE" -eq 0 ]; then
            STAGE_STATUS[$stage]="SKIP"
            log "${C_DIM}  [SKIP] Stage ${stage} (因前序失败而跳过)${C_RESET}"
            continue
        fi

        # 运行阶段
        local stage_ret=0
        if [ "$script" = "RUN_T3_LAYOUT" ]; then
            # T3 特殊处理 (K1-K4 子阶段)
            run_t3_layout || stage_ret=1
        else
            run_stage "$stage" "${VM_DIR}/${script}" || stage_ret=1
        fi

        if [ $stage_ret -ne 0 ]; then
            global_failed=1
            if [ "$CONTINUE_ON_FAILURE" -eq 0 ]; then
                warn "Stage ${stage} 失败, 后续阶段将被跳过 (使用 -c 可继续)"
            else
                warn "Stage ${stage} 失败, 继续运行下一阶段 (--continue)"
            fi
        fi
    done < <(get_all_stages)

    TOTAL_END_TIME=$(now)

    # 汇总报告
    print_summary
    local summary_ret=$?

    # 最终输出
    log ""
    if [ $summary_ret -eq 0 ]; then
        banner "${C_GREEN}ALL TESTS PASSED${C_RESET}"
        exit 0
    else
        banner "${C_RED}SOME TESTS FAILED${C_RESET}"
        exit 1
    fi
}

main "$@"
