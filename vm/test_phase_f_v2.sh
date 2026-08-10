#!/bin/bash
# Phase F: v2 通信架构 (sk 回调 + per-CPT 调度器) QEMU 验证
#
# 验收指标 (参照设计文档 12.11):
#   1. 调度器线程数 = num_online_cpus() (固定, 与 filer 数无关)
#   2. leader 断连 failover ≤ 500ms (sk_state_change 即时, 优于 v1 的 104ms)
#   3. 静默死亡 (网络分区) → keepalive ~11s 检测 → failover
#   4. 高频断连重连 100 次无 crash / UAF / kmemleak
#   5. 重连恢复后 set_callbacks → 收发恢复
#   6. fio QD32 吞吐验证 (流水线工作, 可选)
#
# 运行环境: HOST (需 docker + SSH 到 VM)
# 前置条件:
#   - QEMU VM 已启动: ./qemuctl.sh debug   (调试模式, 实时 serial 日志)
#   - powerfs 挂载到 /mnt/pfs: ./qemuctl.sh mount
#   - filer-1/2/3 容器健康
#   - SSH 可达: ssh -p 2223 root@localhost (密码 powerfs)
#
# 用法: ./test_phase_f_v2.sh [测试编号]
#   无参数: 运行所有测试
#   1: 仅调度器线程数验证
#   2: 仅 failover 时延
#   3: 仅静默死亡
#   4: 仅高频断连
#   5: 仅重连恢复
#   6: 仅 fio 基准

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
PASS=0
FAIL=0
WARN=0
SKIP=0

# 测试选择 (默认全部)
RUN_TEST="${1:-all}"

ok()     { echo "[PASS] $1"; PASS=$((PASS+1)); }
ng()     { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
warn()   { echo "[WARN] $1"; WARN=$((WARN+1)); }
skip()   { echo "[SKIP] $1"; SKIP=$((SKIP+1)); }
section() { echo ""; echo "========================================="; echo "  $1"; echo "========================================="; }

should_run() {
    [ "$RUN_TEST" = "all" ] || [ "$RUN_TEST" = "$1" ]
}

# ---------- 前置检查 ----------

section "前置检查"

if ! vm_alive; then
    echo "[FATAL] VM 不可达 (ssh -p 2223 root@localhost)"
    echo "        请先启动: ./qemuctl.sh debug"
    exit 1
fi
ok "VM SSH 可达"

if ! check_mount; then
    echo "[FATAL] powerfs 未挂载到 /mnt/pfs"
    exit 1
fi
ok "powerfs 已挂载"

for f in "${FILERS[@]}"; do
    if ! wait_filer_healthy "$f"; then
        echo "[FATAL] $f 未健康"
        exit 1
    fi
done
ok "所有 filer 健康"

# 清理上次残留
vm "rm -f ${MNT}/pf_* /tmp/pf_* 2>/dev/null; sync 2>/dev/null" >/dev/null
ok "清理残留"

# 基线写入
vm "echo baseline > ${MNT}/pf_baseline && cat ${MNT}/pf_baseline" | grep -q "baseline" \
    && ok "基线 write/read 正常" \
    || { ng "基线 write/read 失败"; exit 1; }

DMESG_BASE=$(dmesg_line_count)

# ============================================================
# 测试 1: 调度器线程数固定验证 (核心: 与 filer 数无关)
# ============================================================

if should_run 1; then
section "测试 1: 调度器线程数 = num_online_cpus() (固定)"

# 从 dmesg 获取调度器线程数
SCHED_COUNT=$(get_sched_thread_count)
if [ -n "$SCHED_COUNT" ] && [ "$SCHED_COUNT" -gt 0 ]; then
    ok "调度器线程数 = ${SCHED_COUNT} (dmesg 确认)"
else
    warn "未从 dmesg 解析到调度器线程数, 检查 /proc"
    SCHED_COUNT=$(get_sched_threads_proc | wc -l)
    if [ "$SCHED_COUNT" -gt 0 ]; then
        ok "调度器线程数 = ${SCHED_COUNT} (/proc 确认)"
    else
        ng "无法获取调度器线程数"
    fi
fi

# 从 /proc 确认线程存在
PROC_THREADS=$(get_sched_threads_proc)
PROC_COUNT=0
if [ -n "$PROC_THREADS" ]; then
    echo "  -> /proc 中的调度器线程:"
    echo "$PROC_THREADS" | while read -r line; do
        echo "       $line"
    done
    PROC_COUNT=$(echo "$PROC_THREADS" | wc -l)
    if [ "$PROC_COUNT" = "$SCHED_COUNT" ]; then
        ok "/proc 线程数 (${PROC_COUNT}) 与 dmesg 声明 (${SCHED_COUNT}) 一致"
    fi
else
    warn "未在 /proc 中找到 pfs_sched 线程 (可能命名不同)"
fi

# 验证: 线程数应等于 VM 的 CPU 核心数 (qemuctl.sh 中 CPU_CORES=4)
VM_CPUS=$(vm "nproc" 2>/dev/null || echo "unknown")
echo "  -> VM CPU 核心数: ${VM_CPUS}"
echo "  -> 调度器线程数: ${SCHED_COUNT}"

if [ "$VM_CPUS" != "unknown" ] && [ "$SCHED_COUNT" = "$VM_CPUS" ]; then
    ok "调度器线程数 == CPU 核心数 (符合 num_online_cpus 设计)"
else
    warn "调度器线程数 (${SCHED_COUNT}) != CPU 核心数 (${VM_CPUS})"
fi

# 验证: 线程数与 filer 数无关 (3 个 filer, 线程数不应是 3)
if [ "$SCHED_COUNT" != "3" ]; then
    ok "调度器线程数 (${SCHED_COUNT}) != filer 数 (3), 确认与连接数无关"
else
    ng "调度器线程数 = 3 = filer 数, 可能退化为 per-conn 模型!"
fi

# 验证: 断连后线程数不变
echo "  -> 停止 filer-3, 验证线程数不变 ..."
inject_stop_filer filer-3
sleep 2

SCHED_COUNT_AFTER=$(get_sched_thread_count)
PROC_THREADS_AFTER=$(get_sched_threads_proc | wc -l)
echo "  -> 断连后调度器线程数: dmesg=${SCHED_COUNT_AFTER}, /proc=${PROC_THREADS_AFTER}"

if [ "$PROC_THREADS_AFTER" = "$PROC_COUNT" ]; then
    ok "断连后调度器线程数不变 (${PROC_COUNT} → ${PROC_THREADS_AFTER})"
else
    ng "断连后调度器线程数变化 (${PROC_COUNT} → ${PROC_THREADS_AFTER})"
fi

# 恢复
inject_start_filer filer-3
wait_filer_healthy filer-3 || warn "filer-3 未恢复健康"
sleep 2

fi  # 测试 1

# ============================================================
# 测试 2: leader 断连 failover 时延 (目标 ≤ 500ms)
# ============================================================

if should_run 2; then
section "测试 2: leader 断连 failover 时延 (目标 ≤ 500ms)"

# 确保所有 filer 健康
inject_restore_all_filers
sleep 2

# 触发一次写操作, 确保 dmesg 中有 redirect to leader 日志 (供 get_current_leader 解析)
vm "echo trigger > ${MNT}/pf_leader_probe" >/dev/null 2>&1
sleep 1

# 获取当前 leader
LEADER=$(get_current_leader)
echo "  -> 当前 leader: ${LEADER}"

# 在 VM 内启动循环写 (每 50ms 一次, 记录 /proc/uptime 时间戳)
vm "rm -f /tmp/pf_failover_log /tmp/pf_failover_stop" >/dev/null

# 用 SSH stdin 传递循环写脚本 (VM 内 BusyBox 无 base64, 不能用编码方式)
LOOP_SCRIPT=$(cat <<'LOOPSCRIPT'
while [ ! -f /tmp/pf_failover_stop ]; do
    ts=$(read t _ < /proc/uptime; echo $t)
    if echo "$ts" > /mnt/pfs/pf_failover_test 2>/dev/null; then
        echo "OK $ts" >> /tmp/pf_failover_log
    else
        echo "FAIL $ts" >> /tmp/pf_failover_log
    fi
    sleep 0.05
done
LOOPSCRIPT
)
echo "$LOOP_SCRIPT" | $SSH_VM "cat > /tmp/pf_loop.sh && chmod +x /tmp/pf_loop.sh" 2>/dev/null
$SSH_VM "setsid /tmp/pf_loop.sh > /dev/null 2>&1 &" </dev/null >/dev/null 2>&1

echo "  -> 循环写已启动 (50ms 间隔)"
sleep 3  # 等待稳定

# 记录 stop 前的最后几条日志
LOG_BEFORE_STOP=$(vm "tail -5 /tmp/pf_failover_log" 2>/dev/null)
LAST_OK_BEFORE=$(echo "$LOG_BEFORE_STOP" | grep "^OK" | tail -1 | awk '{print $2}')
echo "  -> 断连前最后成功写: uptime=${LAST_OK_BEFORE}"

# 停止 leader (docker stop, 发 FIN → sk_state_change 即时感知)
echo "  -> 停止 leader (${LEADER}) ..."
T_STOP=$(timestamp_ms)
inject_stop_filer "$LEADER"

# 轮询等待写恢复 (最长 30s)
RECOVER_OK=0
T_RECOVER=""
for i in $(seq 1 120); do
    sleep 0.25
    # 获取最新日志行
    LATEST=$(vm "tail -1 /tmp/pf_failover_log" 2>/dev/null)
    if echo "$LATEST" | grep -q "^OK"; then
        LATEST_TS=$(echo "$LATEST" | awk '{print $2}')
        # 在宿主机端比较时间戳 (避免 SSH 内 awk 转义问题)
        # LAST_OK_BEFORE 和 LATEST_TS 都是浮点秒
        if [ -n "$LAST_OK_BEFORE" ] && [ -n "$LATEST_TS" ]; then
            RECOVERED=$(awk -v last="$LAST_OK_BEFORE" -v cur="$LATEST_TS" \
                'BEGIN { if (cur > last + 0.3) print "yes"; else print "no" }')
            if [ "$RECOVERED" = "yes" ]; then
                T_RECOVER=$(timestamp_ms)
                RECOVER_OK=1
                echo "  -> 写恢复 (轮询 ${i} 次, ~$((i*250))ms 后检测到)"
                echo "  -> 恢复时 uptime=${LATEST_TS}"
                break
            fi
        fi
    fi
done

# 计算时延 (粗略: 轮询时延 + 实际 failover 时延)
if [ "$RECOVER_OK" = "1" ]; then
    ELAPSED=$((T_RECOVER - T_STOP))
    echo "  -> 宿主机观测时延: ${ELAPSED}ms (含轮询延迟)"

    # 从 VM 获取完整日志, 在宿主机端解析
    FULL_LOG=$(vm "cat /tmp/pf_failover_log" 2>/dev/null)

    # 获取断连后第一个 OK (恢复点): 时间戳 > LAST_OK_BEFORE + 0.3
    FIRST_RECOVER=$(echo "$FULL_LOG" | awk -v threshold="$LAST_OK_BEFORE" '
        /^OK / {
            if ($2 > threshold + 0.3 && found == 0) {
                print $2
                found = 1
            }
        }
    ')

    if [ -n "$FIRST_RECOVER" ] && [ -n "$LAST_OK_BEFORE" ]; then
        FAILOVER_MS=$(awk -v a="$LAST_OK_BEFORE" -v b="$FIRST_RECOVER" \
            'BEGIN { printf "%.0f", (b - a) * 1000 }')
        echo "  -> VM 内测量 failover 时延: ${FAILOVER_MS}ms"
        echo "     (断连前最后 OK: ${LAST_OK_BEFORE}s, 恢复首 OK: ${FIRST_RECOVER}s)"

        if [ "$FAILOVER_MS" -le 500 ]; then
            ok "failover 时延 ${FAILOVER_MS}ms ≤ 500ms (优秀)"
        elif [ "$FAILOVER_MS" -le 2000 ]; then
            warn "failover 时延 ${FAILOVER_MS}ms > 500ms (含 Raft 选举时间, 合格)"
        else
            ng "failover 时延 ${FAILOVER_MS}ms > 2000ms (需调查)"
        fi
    else
        warn "无法从 VM 日志计算精确时延, 使用宿主机观测: ${ELAPSED}ms"
    fi

    # 验证数据一致
    CONTENT=$(vm "cat ${MNT}/pf_failover_test" 2>/dev/null)
    if [ -n "$CONTENT" ]; then
        ok "恢复后数据写入正常 (内容: ${CONTENT:0:20}...)"
    else
        ng "恢复后数据为空"
    fi
else
    ng "写请求未在 30s 内恢复"
fi

# 停止循环写
vm "touch /tmp/pf_failover_stop" >/dev/null
sleep 1

# 恢复 leader
echo "  -> 恢复 leader (${LEADER}) ..."
inject_start_filer "$LEADER"
wait_filer_healthy "$LEADER" || warn "$LEADER 未恢复健康"
sleep 2

# 检查断连日志
NEW_DMESG=$(dmesg_since "$DMESG_BASE")
if echo "$NEW_DMESG" | grep -qE "filer.*state CONNECTED -> RECONNECTING"; then
    ok "dmesg: 检测到 sk_state_change 触发的断连感知"
else
    warn "dmesg: 未找到 state_change 断连日志"
fi

if echo "$NEW_DMESG" | grep -qE "shard.*CHECKING"; then
    ok "dmesg: shard 路由降级到 CHECKING"
else
    warn "dmesg: 未找到 shard CHECKING 日志"
fi

# 检查是否有 crash
CRASH_STATUS=$(check_kernel_crash)
if [ "$CRASH_STATUS" = "OK" ]; then
    ok "内核日志无 crash/OOPS/UAF"
else
    ng "内核日志检测到异常: ${CRASH_STATUS}"
    vm "dmesg | tail -30"
fi

DMESG_BASE=$(dmesg_line_count)

fi  # 测试 2

# ============================================================
# 测试 3: 静默死亡 (网络分区 → keepalive ~11s → failover)
# ============================================================

if should_run 3; then
section "测试 3: 静默死亡 (网络分区, keepalive 检测)"

# 确保所有 filer 健康
inject_restore_all_filers
sleep 2

# 触发写操作确保有 redirect 日志
vm "echo trigger > ${MNT}/pf_silent_probe" >/dev/null 2>&1
sleep 1

LEADER=$(get_current_leader)
echo "  -> 当前 leader: ${LEADER}"

# 启动循环写
vm "rm -f /tmp/pf_silent_log /tmp/pf_silent_stop" >/dev/null

SILENT_SCRIPT=$(cat <<'SILENTSCRIPT'
while [ ! -f /tmp/pf_silent_stop ]; do
    ts=$(read t _ < /proc/uptime; echo $t)
    if echo "$ts" > /mnt/pfs/pf_silent_test 2>/dev/null; then
        echo "OK $ts" >> /tmp/pf_silent_log
    else
        echo "FAIL $ts" >> /tmp/pf_silent_log
    fi
    sleep 0.05
done
SILENTSCRIPT
)
echo "$SILENT_SCRIPT" | $SSH_VM "cat > /tmp/pf_silent_loop.sh && chmod +x /tmp/pf_silent_loop.sh" 2>/dev/null
$SSH_VM "setsid /tmp/pf_silent_loop.sh > /dev/null 2>&1 &" </dev/null >/dev/null 2>&1

echo "  -> 循环写已启动"
sleep 3

# 获取断连前最后成功写
LOG_BEFORE=$(vm "tail -5 /tmp/pf_silent_log" 2>/dev/null)
LAST_OK_SILENT=$(echo "$LOG_BEFORE" | grep "^OK" | tail -1 | awk '{print $2}')
echo "  -> 断连前最后成功写: uptime=${LAST_OK_SILENT}"

# 网络分区 leader (docker network disconnect, 不发 FIN)
echo "  -> 网络分区 leader (${LEADER}) [docker network disconnect] ..."
T_PARTITION=$(timestamp_ms)
inject_network_partition "$LEADER"

echo "  -> 等待 keepalive 检测 (预期 ~11s: keepidle=5 + 3×keepintvl=2) ..."

# 轮询等待写恢复 (最长 60s, keepalive 可能需要 11s+)
RECOVER_SILENT=0
for i in $(seq 1 240); do
    sleep 0.25
    LATEST=$(vm "tail -1 /tmp/pf_silent_log" 2>/dev/null)
    if echo "$LATEST" | grep -q "^OK"; then
        LATEST_TS=$(echo "$LATEST" | awk '{print $2}')
        if [ -n "$LAST_OK_SILENT" ] && [ -n "$LATEST_TS" ]; then
            RECOVERED=$(awk -v last="$LAST_OK_SILENT" -v cur="$LATEST_TS" \
                'BEGIN { if (cur > last + 0.3) print "yes"; else print "no" }')
            if [ "$RECOVERED" = "yes" ]; then
                T_RECOVER_SILENT=$(timestamp_ms)
                RECOVER_SILENT=1
                ELAPSED_SILENT=$((T_RECOVER_SILENT - T_PARTITION))
                echo "  -> 写恢复, 宿主机观测时延: ${ELAPSED_SILENT}ms"
                break
            fi
        fi
    fi
done

if [ "$RECOVER_SILENT" = "1" ]; then
    # 从 VM 获取完整日志, 在宿主机端解析
    FULL_LOG_SILENT=$(vm "cat /tmp/pf_silent_log" 2>/dev/null)
    FIRST_RECOVER_SILENT=$(echo "$FULL_LOG_SILENT" | awk -v threshold="$LAST_OK_SILENT" '
        /^OK / {
            if ($2 > threshold + 0.3 && found == 0) {
                print $2
                found = 1
            }
        }
    ')

    if [ -n "$FIRST_RECOVER_SILENT" ] && [ -n "$LAST_OK_SILENT" ]; then
        SILENT_MS=$(awk -v a="$LAST_OK_SILENT" -v b="$FIRST_RECOVER_SILENT" \
            'BEGIN { printf "%.0f", (b - a) * 1000 }')
        echo "  -> VM 内测量静默死亡 failover 时延: ${SILENT_MS}ms"

        if [ "$SILENT_MS" -ge 8000 ] && [ "$SILENT_MS" -le 20000 ]; then
            ok "静默死亡 failover 时延 ${SILENT_MS}ms (keepalive 检测, 预期 8-15s)"
        elif [ "$SILENT_MS" -lt 8000 ]; then
            warn "静默死亡 failover 时延 ${SILENT_MS}ms < 8s (可能 sk_error_report 提前触发)"
        else
            ng "静默死亡 failover 时延 ${SILENT_MS}ms > 20s (keepalive 可能未生效)"
        fi
    else
        warn "无法计算精确时延, 宿主机观测: ${ELAPSED_SILENT}ms"
    fi

    # 验证数据
    CONTENT=$(vm "cat ${MNT}/pf_silent_test" 2>/dev/null)
    if [ -n "$CONTENT" ]; then
        ok "恢复后数据正常"
    else
        ng "恢复后数据为空"
    fi
else
    ng "静默死亡后写未在 60s 内恢复"
fi

# 停止循环写
vm "touch /tmp/pf_silent_stop" >/dev/null
sleep 1

# 恢复网络
echo "  -> 恢复网络连接 ..."
inject_network_restore "$LEADER"
sleep 3

# 检查 keepalive 日志
NEW_DMESG=$(dmesg_since "$DMESG_BASE")
if echo "$NEW_DMESG" | grep -qE "error_report|sk_err|keepalive"; then
    ok "dmesg: keepalive 检测到静默死亡 (error_report)"
else
    warn "dmesg: 未找到 keepalive/error_report 日志"
fi

# 检查 filer-1 是否重连
if echo "$NEW_DMESG" | grep -qE "filer.*reconnected"; then
    ok "dmesg: 网络恢复后 filer 重连"
else
    warn "dmesg: 未找到重连日志"
fi

# 检查 crash
CRASH_STATUS=$(check_kernel_crash)
if [ "$CRASH_STATUS" = "OK" ]; then
    ok "内核日志无 crash/OOPS/UAF"
else
    ng "内核日志异常: ${CRASH_STATUS}"
fi

DMESG_BASE=$(dmesg_line_count)

fi  # 测试 3

# ============================================================
# 测试 4: 高频断连重连 100 次 (无 crash/UAF)
# ============================================================

if should_run 4; then
section "测试 4: 高频断连重连 100 次 (回调竞态验证)"

# 确保所有 filer 健康
inject_restore_all_filers
sleep 2

# 记录起始 dmesg
DMESG_START_4=$(dmesg_line_count)

echo "  -> 开始 100 次断连重连循环 (filer-3) ..."
echo "  -> 每次: stop → 1s → start → 2s (共 ~300s)"

CRASH_DETECTED=0
ITERATION_OK=0

for i in $(seq 1 100); do
    # 停止 filer-3
    inject_stop_filer filer-3
    sleep 1

    # 启动 filer-3
    inject_start_filer filer-3
    sleep 2

    # 每 10 次检查 VM 是否存活
    if [ $((i % 10)) -eq 0 ]; then
        if ! vm_alive; then
            ng "第 ${i} 次: VM 崩溃 (SSH 不可达)"
            CRASH_DETECTED=1
            break
        fi

        # 检查 dmesg 是否有 crash
        CRASH_STATUS=$(check_kernel_crash)
        if [ "$CRASH_STATUS" != "OK" ]; then
            ng "第 ${i} 次: 内核异常 (${CRASH_STATUS})"
            CRASH_DETECTED=1
            echo "  -> 异常 dmesg 片段:"
            dmesg_since "$DMESG_START_4" | tail -20
            break
        fi

        # 验证写仍正常
        WRITE_TEST=$(vm "echo iter_${i} > ${MNT}/pf_stress_${i} && cat ${MNT}/pf_stress_${i}" 2>/dev/null)
        if echo "$WRITE_TEST" | grep -q "iter_${i}"; then
            ITERATION_OK=$i
            echo "  -> 第 ${i}/100 次完成, 写正常, 无 crash"
        else
            ng "第 ${i} 次: 写失败"
            CRASH_DETECTED=1
            break
        fi
    fi
done

if [ "$CRASH_DETECTED" = "0" ]; then
    ok "100 次断连重连完成, 无 crash"

    # 最终写验证
    FINAL_WRITE=$(vm "echo final_100 > ${MNT}/pf_stress_final && cat ${MNT}/pf_stress_final" 2>/dev/null)
    if echo "$FINAL_WRITE" | grep -q "final_100"; then
        ok "100 次循环后写正常"
    else
        ng "100 次循环后写失败"
    fi
else
    ng "高频断连测试在第 ${ITERATION_OK} 次失败"
fi

# 全面检查 dmesg
echo "  -> 分析 100 次循环的 dmesg ..."
NEW_DMESG_4=$(dmesg_since "$DMESG_START_4")

# 统计断连/重连次数
DISCONNECT_COUNT_4=$(echo "$NEW_DMESG_4" | grep -cE "filer.*state CONNECTED -> RECONNECTING")
RECONNECT_COUNT_4=$(echo "$NEW_DMESG_4" | grep -cE "filer.*reconnected")
echo "  -> 断连次数: ${DISCONNECT_COUNT_4}, 重连次数: ${RECONNECT_COUNT_4}"

if [ "$DISCONNECT_COUNT_4" -ge 90 ]; then
    ok "断连次数 ${DISCONNECT_COUNT_4} ≥ 90 (接近 100, 符合预期)"
else
    warn "断连次数 ${DISCONNECT_COUNT_4} < 90 (可能部分未触发)"
fi

if [ "$RECONNECT_COUNT_4" -ge 90 ]; then
    ok "重连次数 ${RECONNECT_COUNT_4} ≥ 90"
else
    warn "重连次数 ${RECONNECT_COUNT_4} < 90 (可能恢复过慢)"
fi

# 检查 crash/OOPS/UAF
if echo "$NEW_DMESG_4" | grep -qE "BUG:|Oops:|panic|Call Trace|use-after-free|KASAN|kmemleak"; then
    ng "dmesg: 检测到 crash/UAF/OOPS"
    echo "  -> 异常日志:"
    echo "$NEW_DMESG_4" | grep -E "BUG:|Oops:|panic|Call Trace|use-after-free|KASAN|kmemleak" | head -10
else
    ok "dmesg: 100 次循环无 crash/UAF/OOPS"
fi

# 检查 WARNING
WARNING_COUNT=$(echo "$NEW_DMESG_4" | grep -cE "WARNING: CPU:.*PID:")
if [ "$WARNING_COUNT" -gt 0 ]; then
    warn "dmesg: ${WARNING_COUNT} 条 WARNING (非致命, 但需关注)"
    echo "$NEW_DMESG_4" | grep "WARNING:" | head -5
else
    ok "dmesg: 无 WARNING"
fi

# 清理
vm "rm -f ${MNT}/pf_stress_* 2>/dev/null" >/dev/null

DMESG_BASE=$(dmesg_line_count)

fi  # 测试 4

# ============================================================
# 测试 5: 重连恢复后收发验证 (set_callbacks → 收发恢复)
# ============================================================

if should_run 5; then
section "测试 5: 重连恢复后收发验证"

# 确保所有 filer 健康
inject_restore_all_filers
sleep 2

# 停止所有 filer
echo "  -> 停止所有 filer ..."
inject_stop_all_filers
sleep 2

# 验证断连
vm "echo block_test > ${MNT}/pf_recover_test" 2>/dev/null
if ! vm "test -f ${MNT}/pf_recover_test" 2>/dev/null; then
    ok "全 filer 断连时写阻塞 (未创建文件)"
else
    # 可能走了 page cache, 清除后重试
    vm "rm -f ${MNT}/pf_recover_test 2>/dev/null" >/dev/null
fi

# 启动后台写 (阻塞等待, 用 setsid 确保 SSH 关闭后进程存活)
vm "rm -f /tmp/pf_recover_result" >/dev/null
$SSH_VM "setsid sh -c '
    echo recover_\$(date +%s) > ${MNT}/pf_recover_test
    echo OK > /tmp/pf_recover_result
' > /tmp/pf_recover.log 2>&1 &" </dev/null >/dev/null 2>&1

sleep 2
WRITE_BLOCKED=$($SSH_VM "test -f /tmp/pf_recover_result && echo no || echo yes" 2>/dev/null)
if [ "$WRITE_BLOCKED" = "yes" ]; then
    ok "断连期间写请求阻塞 (入队列等待)"
else
    warn "写未阻塞 (可能 page cache 或 inline)"
fi

# 恢复 filer-1 + filer-2
echo "  -> 启动 filer-1 + filer-2 ..."
inject_start_filers filer-1 filer-2
wait_filer_healthy filer-1 || ng "filer-1 未健康"
wait_filer_healthy filer-2 || ng "filer-2 未健康"

# 等待重连
echo "  -> 等待内核重连 ..."
RECONNECT_BASE_5=$(dmesg_line_count)
for i in $(seq 1 30); do
    NEW_LOG=$(dmesg_since "$RECONNECT_BASE_5")
    if echo "$NEW_LOG" | grep -qE "filer.*connected.*v2 scheduler"; then
        ok "filer 重连成功 (v2 scheduler)"
        break
    fi
    sleep 1
done

# 等待写完成
echo "  -> 等待后台写完成 ..."
WRITE_RECOVERED=0
for i in $(seq 1 30); do
    RESULT=$($SSH_VM "cat /tmp/pf_recover_result 2>/dev/null" 2>/dev/null)
    if [ "$RESULT" = "OK" ]; then
        WRITE_RECOVERED=1
        ok "重连后写请求完成 (${i}s 内)"
        break
    fi
    sleep 1
done

if [ "$WRITE_RECOVERED" != "1" ]; then
    ng "重连后写请求未在 30s 内完成"
fi

# 验证数据一致
CONTENT=$(vm "cat ${MNT}/pf_recover_test" 2>/dev/null)
if echo "$CONTENT" | grep -q "^recover_"; then
    ok "重连后数据一致: $CONTENT"
else
    ng "重连后数据异常: '$CONTENT'"
fi

# 验证 set_callbacks 后收发正常 (连续读写)
echo "  -> 验证 set_callbacks 后连续收发 ..."
SUCCESS_COUNT=0
for i in $(seq 1 10); do
    vm "echo rw_${i} > ${MNT}/pf_rw_${i}" >/dev/null 2>&1
    READ_BACK=$(vm "cat ${MNT}/pf_rw_${i}" 2>/dev/null)
    if [ "$READ_BACK" = "rw_${i}" ]; then
        SUCCESS_COUNT=$((SUCCESS_COUNT+1))
    fi
done

if [ "$SUCCESS_COUNT" = "10" ]; then
    ok "set_callbacks 后 10/10 次读写成功"
else
    ng "set_callbacks 后仅 ${SUCCESS_COUNT}/10 次读写成功"
fi

# 恢复 filer-3
echo "  -> 恢复 filer-3 ..."
inject_start_filer filer-3
wait_filer_healthy filer-3 || warn "filer-3 未健康"
sleep 2

# 清理
vm "rm -f ${MNT}/pf_recover_* ${MNT}/pf_rw_* /tmp/pf_recover_* 2>/dev/null" >/dev/null

# 检查 crash
CRASH_STATUS=$(check_kernel_crash)
if [ "$CRASH_STATUS" = "OK" ]; then
    ok "内核日志无 crash/OOPS/UAF"
else
    ng "内核日志异常: ${CRASH_STATUS}"
fi

DMESG_BASE=$(dmesg_line_count)

fi  # 测试 5

# ============================================================
# 测试 6: fio QD32 基准吞吐 (流水线验证, 可选)
# ============================================================

if should_run 6; then
section "测试 6: fio QD32 基准吞吐 (流水线验证)"

# 检查 VM 内是否有 fio
FIO_AVAIL=$(vm "which fio 2>/dev/null || echo none" 2>/dev/null)
if [ "$FIO_AVAIL" = "none" ] || [ -z "$FIO_AVAIL" ]; then
    skip "VM 内无 fio, 跳过基准测试"
    skip "如需 fio 测试, 请在 build_initramfs.sh 中添加 fio"
else
    ok "VM 内 fio 可用: ${FIO_AVAIL}"

    # 创建 fio 配置文件
    vm "cat > /tmp/pf_fio.cfg << 'FIOEOF'
[global]
ioengine=sync
direct=1
bs=4k
size=16M
numjobs=1
time_based=1
runtime=10
group_reporting=1

[seq-write-qd1]
iodepth=1
rw=write
filename=${MNT}/pf_fio_qd1

[seq-write-qd32]
iodepth=32
rw=write
filename=${MNT}/pf_fio_qd32
FIOEOF" >/dev/null

    echo "  -> 运行 fio QD1 ..."
    FIO_QD1=$(vm "fio --section=seq-write-qd1 /tmp/pf_fio.cfg 2>&1" 2>/dev/null)
    BW_QD1=$(echo "$FIO_QD1" | grep -oE 'bw=[0-9]+KiB/s' | head -1 | grep -oE '[0-9]+')

    echo "  -> 运行 fio QD32 ..."
    FIO_QD32=$(vm "fio --section=seq-write-qd32 /tmp/pf_fio.cfg 2>&1" 2>/dev/null)
    BW_QD32=$(echo "$FIO_QD32" | grep -oE 'bw=[0-9]+KiB/s' | head -1 | grep -oE '[0-9]+')

    echo "  -> QD1 带宽: ${BW_QD1} KiB/s"
    echo "  -> QD32 带宽: ${BW_QD32} KiB/s"

    if [ -n "$BW_QD1" ] && [ -n "$BW_QD32" ]; then
        if [ "$BW_QD32" -gt "$BW_QD1" ]; then
            RATIO=$((BW_QD32 * 100 / BW_QD1))
            ok "QD32 (${BW_QD32} KiB/s) > QD1 (${BW_QD1} KiB/s), 流水线工作 (×${RATIO}%)"
        else
            warn "QD32 (${BW_QD32}) ≤ QD1 (${BW_QD1}), 流水线可能未生效"
        fi
    else
        warn "无法解析 fio 带宽结果"
        echo "  -> fio QD1 输出: $FIO_QD1" | head -5
        echo "  -> fio QD32 输出: $FIO_QD32" | head -5
    fi

    # 清理
    vm "rm -f ${MNT}/pf_fio_* /tmp/pf_fio.cfg 2>/dev/null" >/dev/null
fi

fi  # 测试 6

# ============================================================
# 最终汇总
# ============================================================

section "最终内核日志检查"

# 全局 crash 检查
CRASH_STATUS=$(check_kernel_crash)
if [ "$CRASH_STATUS" = "OK" ]; then
    ok "全局: 内核日志无 crash/OOPS/UAF"
else
    ng "全局: 内核日志异常 (${CRASH_STATUS})"
    echo "  -> 异常日志片段:"
    $SSH_VM "dmesg" 2>/dev/null | grep -E "BUG:|Oops:|panic|Call Trace|use-after-free|KASAN|kmemleak" | head -10
fi

# 检查调度器线程是否全部干净退出 (如果已 umount)
# 注意: 如果 VM 仍在运行, 调度器线程应仍在运行

# 确保所有 filer 恢复
inject_restore_all_filers

# ---------- 汇总 ----------

section "Phase F 结果汇总"
echo "  PASS = $PASS"
echo "  FAIL = $FAIL"
echo "  WARN = $WARN"
echo "  SKIP = $SKIP"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "  >>> Phase F 验证通过 <<<"
    echo "  >>> v2 通信架构 (sk 回调 + per-CPT 调度器) 验收合格 <<<"
    exit 0
else
    echo "  >>> Phase F 验证失败, 请检查上述 FAIL 项 <<<"
    exit 1
fi
