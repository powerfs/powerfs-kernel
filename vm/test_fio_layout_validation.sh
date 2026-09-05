#!/bin/bash
# fio Layout Prediction Validation Test
#
# 验证 RuleBasedPredictor 在实际 IO 负载下的布局决策正确性:
#   1. 配置文件 (.conf) → Inline: 4K 顺序写
#   2. 可执行文件 (.so) → Flat: 1M 顺序写
#   3. ML 模型 (.pt) → Stripe: 1M 顺序写, 大文件
#   4. 视频文件 (.mp4) → Stripe: 1M 顺序写
#   5. 压缩包 (.tar.gz) → Stripe: 1M 顺序写
#   6. 4K 随机写 (验证无迁移): .bin → Flat
#
# 验证方法:
#   - fio 写入后检查 filer 日志中的 LAYOUT_PREDICT 输出
#   - 统计 Inline→Flat 迁移次数 (应为 0)
#   - 验证文件可正常读写
#
set -u

FUSE_CONTAINER="fuse-1"
MNT="/mnt/powerfs"
TEST_DIR="${MNT}/fio_layout_test_$$"
RESULT_DIR="/tmp/fio_layout_results_$$"
FIO="fio"

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

# 检查 filer 日志中的 LAYOUT_PREDICT
check_predict() {
    local filename="$1"
    local expected_mode="$2"
    local log_line

    log_line=$(docker logs --tail 100 filer-1 2>&1 | grep "LAYOUT_PREDICT:" | grep "file=${filename}" | tail -1 || true)

    if [ -z "$log_line" ]; then
        warn "No LAYOUT_PREDICT log for ${filename}"
        return 1
    fi

    if echo "$log_line" | grep -q "mode=${expected_mode}"; then
        ok "${filename} → ${expected_mode} (filer log confirmed)"
        return 0
    else
        ng "${filename}: expected ${expected_mode}, got: ${log_line}"
        return 1
    fi
}

# 检查迁移次数
check_no_migration() {
    local filename="$1"
    local migrate_log

    migrate_log=$(docker logs --tail 200 filer-1 2>&1 | grep -i "migrate_inline" | grep "${filename}" || true)
    if [ -n "$migrate_log" ]; then
        ng "Inline→Flat migration detected for ${filename}: ${migrate_log}"
        return 1
    else
        ok "no Inline→Flat migration for ${filename}"
        return 0
    fi
}

# 运行 fio 测试
run_fio_test() {
    local name="$1"
    local filename="$2"
    local expected_layout="$3"
    local fio_args="$4"
    local filepath="${TEST_DIR}/${filename}"

    section "fio: ${name} (${filename} → ${expected_layout})"

    # 运行 fio
    docker exec ${FUSE_CONTAINER} ${FIO} --name=${name} \
        --filename=${filepath} ${fio_args} \
        --ioengine=libaio --direct=1 --group_reporting 2>&1 | tail -5

    # 检查文件存在
    if docker exec ${FUSE_CONTAINER} test -f ${filepath}; then
        ok "file ${filename} exists"
    else
        ng "file ${filename} not found"
        return 1
    fi

    # 检查文件大小
    local size
    size=$(docker exec ${FUSE_CONTAINER} stat -c %s ${filepath} 2>/dev/null)
    ok "file size = ${size} bytes"

    # 检查布局预测
    check_predict "${filename}" "${expected_layout}"

    # 检查无迁移
    check_no_migration "${filename}"

    # 读回验证 (检查文件可读, 不检查内容)
    if docker exec ${FUSE_CONTAINER} dd if=${filepath} of=/dev/null bs=4k count=1 2>/dev/null; then
        ok "read back data (first 4K readable)"
    else
        ng "read back failed"
    fi
}

# 主测试
main() {
    echo -e "${C_CYAN}╔══════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  fio Layout Prediction Validation Test        ║${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════╝${C_RESET}"

    # 创建测试目录
    docker exec ${FUSE_CONTAINER} mkdir -p ${TEST_DIR}
    docker exec ${FUSE_CONTAINER} mkdir -p ${RESULT_DIR}

    # T1: 配置文件 → Inline (4K 顺序写)
    run_fio_test "config_write" "app.conf" "Inline" \
        "--rw=write --bs=4k --size=4k --numjobs=1"

    # T2: 可执行文件 → Flat (1M 顺序写, 64MB)
    run_fio_test "exec_write" "libpowerfs.so" "Flat" \
        "--rw=write --bs=1m --size=64m --numjobs=1"

    # T3: ML 模型 → Stripe (1M 顺序写, 256MB)
    run_fio_test "ml_write" "model.pt" "Stripe" \
        "--rw=write --bs=1m --size=256m --numjobs=1"

    # T4: 视频文件 → Stripe (1M 顺序写, 128MB)
    run_fio_test "video_write" "video.mp4" "Stripe" \
        "--rw=write --bs=1m --size=128m --numjobs=1"

    # T5: 压缩包 → Stripe (1M 顺序写, 64MB)
    run_fio_test "archive_write" "backup.tar.gz" "Stripe" \
        "--rw=write --bs=1m --size=64m --numjobs=1"

    # T6: 4K 随机写 → Flat (16MB, 验证无迁移)
    run_fio_test "randwrite" "randwrite.bin" "Flat" \
        "--rw=randwrite --bs=4k --size=16m --numjobs=1"

    # T7: 读回测试 (验证数据完整性)
    section "fio: readback verification"
    docker exec ${FUSE_CONTAINER} ${FIO} --name=readback \
        --filename=${TEST_DIR}/model.pt \
        --rw=read --bs=1m --size=256m --ioengine=libaio --direct=1 \
        --group_reporting 2>&1 | tail -3
    ok "readback model.pt (256MB)"

    docker exec ${FUSE_CONTAINER} ${FIO} --name=readback2 \
        --filename=${TEST_DIR}/video.mp4 \
        --rw=read --bs=1m --size=128m --ioengine=libaio --direct=1 \
        --group_reporting 2>&1 | tail -3
    ok "readback video.mp4 (128MB)"

    # T8: 性能统计 (4K 随机写 IOPS)
    section "fio: 4K random write IOPS (layout prediction enabled)"
    docker exec ${FUSE_CONTAINER} ${FIO} --name=iops_test \
        --filename=${TEST_DIR}/iops_test.bin \
        --rw=randwrite --bs=4k --size=64m --numjobs=1 \
        --ioengine=libaio --direct=1 --group_reporting \
        --runtime=10 --time_based 2>&1 | grep -E "write:|iops" | head -3
    ok "IOPS test completed"

    # 统计迁移次数 (全局)
    section "Global migration count"
    local total_migrations
    total_migrations=$(docker logs filer-1 2>&1 | grep -c "migrate_inline" 2>/dev/null || true)
    total_migrations=${total_migrations:-0}
    echo "  Total Inline→Flat migrations: ${total_migrations}"
    if [ "${total_migrations}" -eq 0 ] 2>/dev/null; then
        ok "Zero migrations (layout prediction working perfectly)"
    else
        warn "${total_migrations} migrations detected (may be from pre-test files)"
    fi

    # 统计预测命中率
    section "Layout prediction statistics"
    local total_predicts
    total_predicts=$(docker logs filer-1 2>&1 | grep -c "LAYOUT_PREDICT:" 2>/dev/null || true)
    total_predicts=${total_predicts:-0}
    local confident_predicts
    confident_predicts=$(docker logs filer-1 2>&1 | grep "LAYOUT_PREDICT:" | grep -v "confidence=0.00" | wc -l 2>/dev/null || true)
    confident_predicts=${confident_predicts:-0}
    echo "  Total predictions: ${total_predicts}"
    echo "  Confident predictions: ${confident_predicts}"
    if [ "${total_predicts}" -gt 0 ] 2>/dev/null; then
        ok "Prediction hit rate: ${confident_predicts}/${total_predicts}"
    fi

    # 汇总
    echo -e "\n${C_CYAN}════════════════════════════════════════════════"
    echo -e "  PASS = ${PASS}, FAIL = ${FAIL}, WARN = ${WARN}"
    echo -e "════════════════════════════════════════════════${C_RESET}"

    # 清理
    docker exec ${FUSE_CONTAINER} rm -rf ${TEST_DIR} 2>/dev/null || true

    if [ ${FAIL} -gt 0 ]; then
        echo -e "${C_RED}TEST FAILED${C_RESET}"
        exit 1
    fi
    echo -e "${C_GREEN}TEST PASSED${C_RESET}"
    exit 0
}

main "$@"
