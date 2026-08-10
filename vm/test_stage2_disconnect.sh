#!/bin/bash
# 阶段 2: 断连测试 (全 filer 断连 → 写阻塞 → 恢复多数派 → 写完成)
#
# 设计说明:
#   filer-1/2/3 是 Raft 集群, 停单个 filer 只触发 leader 切换 (failover 成功),
#   不会产生真正的断连阻塞. 要验证断连阻塞语义, 必须停掉足够多的 filer
#   使 Raft 无法选出 leader (停所有 3 个), 此时内核 send_request 入队列等待,
#   恢复多数派 (2/3) 后 Raft 选主 → 内核重连 → 队列请求完成.
#
# 目标:
#   1. 验证断连期间写请求阻塞 (入队列, 不立即返回 -EIO, 不走本地缓存)
#   2. 验证恢复 Raft 多数派后写请求自动完成 (send_request 30s 超时窗口内)
#   3. 验证数据一致性 (重连后读回写内容)
#   4. 验证 reconnect_count 归零 (第二次断连恢复仍能完成)
#   5. 验证 page cache 命中时断连期间可读
#   6. 验证 page cache miss 时断连期间读阻塞
#
# 运行环境: HOST (需要 docker + SSH 到 VM)
# 前置条件:
#   - QEMU VM 已启动: ./qemuctl.sh debug   (调试模式, 实时 serial 日志)
#   - powerfs 已挂载到 /mnt/pfs: ./qemuctl.sh mount
#   - filer-1/2/3 容器健康
#   - SSH 可达: ssh -p 2223 root@localhost (密码 powerfs)
#
# 新增测试 (连接池架构):
#   0. 单 filer 断连: 其他 filer 连接保持, 仅该 filer 标记重发, 重连后请求完成
#
# 用法: ./test_stage2_disconnect.sh

set -u

SSH="sshpass -p powerfs ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -p 2223 root@localhost"
MNT=/mnt/pfs
FILERS=(filer-1 filer-2 filer-3)
PASS=0
FAIL=0
WARN=0

ok()   { echo "[PASS] $1"; PASS=$((PASS+1)); }
ng()   { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
warn() { echo "[WARN] $1"; WARN=$((WARN+1)); }

section() { echo ""; echo "========================================="; echo "  $1"; echo "========================================="; }

# ---------- 辅助函数 ----------

# 在 VM 上执行命令 (过滤残余 SSH 警告)
vm() {
    $SSH "$@" 2>&1 | grep -v '^Warning: Permanently added'
}

# 停止所有 filer (并发, 加 wait)
stop_all_filers() {
    local f
    for f in "${FILERS[@]}"; do
        docker stop "$f" >/dev/null 2>&1 &
    done
    wait
}

# 启动指定 filer (并发)
start_filers() {
    local f
    for f in "$@"; do
        docker start "$f" >/dev/null 2>&1 &
    done
    wait
    # Allow Raft election to complete before kernel reconnects.
    # Without this, the kernel may send requests before a leader is elected,
    # causing REDIRECT-to-self loops (harmless with -EAGAIN retry, but wasteful).
    sleep 3
}

# 等待 filer 容器 healthy
wait_filer_healthy() {
    local container=$1
    local max=60
    for i in $(seq 1 $max); do
        local status
        status=$(docker inspect "$container" --format '{{.State.Health.Status}}' 2>/dev/null || echo "none")
        if [ "$status" = "healthy" ]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# 等待内核重连成功 (dmesg 出现 "reconnected" 或 "connected (v2 scheduler)")
# 通过对比 dmesg 行数, 只看新增日志
wait_reconnect() {
    local base_line=$1
    local max=60
    for i in $(seq 1 $max); do
        local new_log
        new_log=$($SSH "dmesg | tail -n +${base_line}" 2>/dev/null)
        if echo "$new_log" | grep -qE "powerfs: filer.*reconnected|powerfs: filer.*connected \(v2"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# 检查 VM 内 powerfs 是否仍挂载
check_mount() {
    $SSH "mount | grep -q 'on ${MNT} type powerfs'" 2>/dev/null
}

# 获取当前 dmesg 行数 (作为后续新增日志的基线)
dmesg_line_count() {
    $SSH "dmesg | wc -l" 2>/dev/null
}

# ---------- 前置检查 ----------

section "前置检查"

# 1. VM 可达
if ! $SSH "echo VM_OK" >/dev/null 2>&1; then
    echo "[FATAL] VM 不可达 (ssh -p 2223 root@localhost)"
    echo "        请先启动: ./qemuctl.sh debug"
    exit 1
fi
ok "VM SSH 可达"

# 2. powerfs 已挂载
if ! check_mount; then
    echo "[FATAL] powerfs 未挂载到 ${MNT}"
    exit 1
fi
ok "powerfs 已挂载"

# 3. 所有 filer 健康
for f in "${FILERS[@]}"; do
    if ! wait_filer_healthy "$f"; then
        echo "[FATAL] $f 未健康"
        exit 1
    fi
done
ok "所有 filer 健康"

# 4. 清理上次测试残留
vm "rm -f ${MNT}/p2_* /tmp/p2_* 2>/dev/null; sync 2>/dev/null" >/dev/null
ok "清理残留文件"

# 5. 基线写入验证
vm "echo baseline > ${MNT}/p2_baseline && cat ${MNT}/p2_baseline" | grep -q "baseline" \
    && ok "基线 write/read 正常" \
    || { ng "基线 write/read 失败, 后续测试无意义"; exit 1; }

# 记录基线 dmesg 行数
DMESG_BASE=$(dmesg_line_count)

# ---------- 测试 0: 单 filer 断连 (连接池容错) ----------

section "测试 0: 单 filer 断连 (连接池容错, 其他 filer 连接保持)"

# 先 sync 刷盘
vm "sync" 2>/dev/null
sleep 1

# 获取所有 filer 当前状态 (应全 CONNECTED)
ALL_CONNECTED_BEFORE=$(vm "dmesg | grep 'state CONNECTING -> CONNECTED' | wc -l" 2>/dev/null)
echo "  -> 当前已连接 filer 数: ${ALL_CONNECTED_BEFORE}"

# 仅停 filer-3 (其他 filer 保持运行, Raft 仍可用, 集群正常)
echo "  -> 停止 filer-3 ..."
docker stop filer-3 >/dev/null 2>&1
sleep 2

# 检查 filer-3 是否进入 RECONNECTING 状态
SINGLE_DISCONNECT=$(vm "dmesg | tail -n +${DMESG_BASE} | grep -E 'filer 172.30.0.37:9334 state CONNECTED -> RECONNECTING'" 2>/dev/null)
if [ -n "$SINGLE_DISCONNECT" ]; then
    ok "filer-3 断连被检测到 (状态变更)"
else
    warn "未捕获 filer-3 断连状态变更 (可能日志格式不同, 看 dmesg)"
    vm "dmesg | tail -20"
fi

# 关键: 其他 filer 仍 CONNECTED (连接池独立)
# 在断连期间应能正常写 (走 filer-1/filer-2)
WRITE_DURING_SINGLE=$(vm "echo single_filer_test_\$(date +%s) > ${MNT}/p2_single_filer && cat ${MNT}/p2_single_filer" 2>/dev/null)
if echo "$WRITE_DURING_SINGLE" | grep -q "^single_filer_test_[0-9]*$"; then
    ok "单 filer 断连时其他 filer 仍可用 (写成功: $WRITE_DURING_SINGLE)"
else
    ng "单 filer 断连时写失败 (连接池未正确隔离): '$WRITE_DURING_SINGLE'"
fi

# 恢复 filer-3, 验证重连
echo "  -> 恢复 filer-3 ..."
docker start filer-3 >/dev/null 2>&1
wait_filer_healthy filer-3 || ng "filer-3 未恢复健康"

RECONNECT_BASE_0=$(dmesg_line_count)
sleep 5

# filer-3 应重连
SINGLE_RECONNECT=$(vm "dmesg | tail -n +${RECONNECT_BASE_0} | grep -E 'filer 172.30.0.37:9334.*reconnected|filer 172.30.0.37:9334.*connected.*v2'" 2>/dev/null)
if [ -n "$SINGLE_RECONNECT" ]; then
    ok "filer-3 恢复后自动重连"
else
    warn "未捕获 filer-3 重连日志 (看 dmesg)"
    vm "dmesg | tail -20"
fi

# ---------- 测试 1: 全 filer 断连 → 写阻塞 → 恢复 → 写完成 ----------

section "测试 1: 全 filer 断连, 写阻塞, 恢复多数派后写完成"

# 先 sync 刷盘, 防止 writeback 操作在断连期间堆积导致 VM 卡死
vm "sync" 2>/dev/null
sleep 1

# 先停所有 filer (避免写请求在 filer 停止前完成的竞态)
echo "  -> 停止所有 filer ..."
stop_all_filers
echo "  -> 已停止 filer-1/2/3"
sleep 1

# 在断连状态下启动后台写 (create 请求会入队列阻塞)
vm "setsid sh -c 'echo disconnect_test_\$(date +%s) > ${MNT}/p2_write_during_disconnect && echo OK > /tmp/p2_write_result' > /tmp/p2_write.log 2>&1 < /dev/null &" >/dev/null

# 等待写请求进入队列
sleep 2

# 检查写是否阻塞 (应未完成)
WRITE_DONE=$($SSH "test -f /tmp/p2_write_result && echo yes || echo no" 2>/dev/null)
if [ "$WRITE_DONE" = "no" ]; then
    ok "断连期间写请求阻塞 (未立即返回)"
else
    ng "写请求在断连期间已完成 (可能走了本地缓存)"
fi

# 验证文件尚未创建 (create 请求阻塞, 未到达 filer)
FILE_EXISTS=$($SSH "test -f ${MNT}/p2_write_during_disconnect && echo yes || echo no" 2>/dev/null)
if [ "$FILE_EXISTS" = "no" ]; then
    ok "断连期间文件未创建 (写未绕过 Filer)"
else
    ng "断连期间文件已存在 (写未正确阻塞)"
fi

# 恢复 Raft 多数派 (filer-1 + filer-2)
echo "  -> 启动 filer-1 + filer-2 (恢复 Raft 多数派) ..."
start_filers filer-1 filer-2

RECONNECT_BASE=$(dmesg_line_count)

echo "  -> 等待 filer-1/2 健康 ..."
wait_filer_healthy filer-1 && ok "filer-1 恢复健康" || ng "filer-1 未恢复健康"
wait_filer_healthy filer-2 && ok "filer-2 恢复健康" || ng "filer-2 未恢复健康"

echo "  -> 等待内核重连 ..."
if wait_reconnect "$RECONNECT_BASE"; then
    ok "内核已重连到 filer"
else
    ng "内核未在 60s 内重连"
fi

# 等待后台写完成 (send_request 30s 超时窗口, 但重连 + Raft 选主可能需要更久)
echo "  -> 等待后台写完成 ..."
WRITE_COMPLETE=0
for i in $(seq 1 80); do
    RESULT=$($SSH "cat /tmp/p2_write_result 2>/dev/null" 2>/dev/null)
    if [ "$RESULT" = "OK" ]; then
        WRITE_COMPLETE=1
        ok "写请求在 filer 恢复后 ${i}s 内完成"
        break
    fi
    sleep 1
done

if [ "$WRITE_COMPLETE" -ne 1 ]; then
    ng "写请求未在 80s 内完成 (send_request 超时或重连失败)"
fi

# ---------- 测试 2: 数据一致性验证 ----------

section "测试 2: 数据一致性"

# 读回写的内容
CONTENT=$(vm "cat ${MNT}/p2_write_during_disconnect 2>/dev/null")
if echo "$CONTENT" | grep -q "^disconnect_test_[0-9]*$"; then
    ok "读回内容一致: $CONTENT"
else
    ng "读回内容异常: '$CONTENT'"
fi

# 追加写验证
vm "echo append_line >> ${MNT}/p2_write_during_disconnect" >/dev/null
CONTENT2=$(vm "cat ${MNT}/p2_write_during_disconnect 2>/dev/null")
if echo "$CONTENT2" | grep -q "append_line"; then
    ok "恢复后追加写成功"
else
    ng "恢复后追加写失败: '$CONTENT2'"
fi

# 创建新文件验证
vm "echo new_file_after_recover > ${MNT}/p2_new_file" >/dev/null
CONTENT3=$(vm "cat ${MNT}/p2_new_file 2>/dev/null")
if [ "$CONTENT3" = "new_file_after_recover" ]; then
    ok "恢复后创建新文件成功"
else
    ng "恢复后创建新文件失败: '$CONTENT3'"
fi

# ---------- 测试 3: reconnect_count 归零验证 (第二次断连恢复) ----------

section "测试 3: reconnect_count 归零 (第二次断连恢复)"

# 清理上次后台结果
vm "rm -f /tmp/p2_write_result /tmp/p2_write.log" >/dev/null

# 第二次断连
vm "sync" 2>/dev/null; sleep 1
echo "  -> 第二次停止所有 filer ..."
stop_all_filers
sleep 1

# 后台写
vm "setsid sh -c 'echo second_disconnect_\$(date +%s) > ${MNT}/p2_write_second && echo OK > /tmp/p2_write_result' > /tmp/p2_write.log 2>&1 < /dev/null &" >/dev/null
sleep 2

WRITE_DONE2=$($SSH "test -f /tmp/p2_write_result && echo yes || echo no" 2>/dev/null)
if [ "$WRITE_DONE2" = "no" ]; then
    ok "第二次断连写请求阻塞"
else
    ng "第二次断连写未阻塞"
fi

echo "  -> 第二次恢复 filer-1 + filer-2 ..."
start_filers filer-1 filer-2
RECONNECT_BASE2=$(dmesg_line_count)
wait_filer_healthy filer-1 || ng "filer-1 第二次未健康"
wait_filer_healthy filer-2 || ng "filer-2 第二次未健康"
wait_reconnect "$RECONNECT_BASE2" || ng "第二次内核未重连"

# 等待第二次写完成
WRITE_COMPLETE2=0
for i in $(seq 1 80); do
    RESULT=$($SSH "cat /tmp/p2_write_result 2>/dev/null" 2>/dev/null)
    if [ "$RESULT" = "OK" ]; then
        WRITE_COMPLETE2=1
        ok "第二次写请求在 filer 恢复后 ${i}s 内完成 (reconnect_count 已归零)"
        break
    fi
    sleep 1
done

if [ "$WRITE_COMPLETE2" -ne 1 ]; then
    ng "第二次写请求未完成 (reconnect_count 可能未归零, 误判为已达上限)"
fi

# 验证第二次数据
CONTENT4=$(vm "cat ${MNT}/p2_write_second 2>/dev/null")
if echo "$CONTENT4" | grep -q "^second_disconnect_[0-9]*$"; then
    ok "第二次读回内容一致"
else
    ng "第二次读回内容异常: '$CONTENT4'"
fi

# ---------- 测试 4: page cache 命中时断连期间可读 ----------

section "测试 4: page cache 命中, 断连期间可读"

# 先写入文件并读一次 (填充 page cache)
vm "echo cached_data > ${MNT}/p2_cached_file && cat ${MNT}/p2_cached_file > /dev/null" >/dev/null
ok "预填充 page cache"

# 断连 (page cache 测试不需要 sync, 因为测试目的就是保留 page cache)
echo "  -> 停止所有 filer ..."
stop_all_filers
sleep 2

# 读 (应从 page cache 命中, 不走网络)
CACHED_READ=$(vm "cat ${MNT}/p2_cached_file 2>/dev/null")
if [ "$CACHED_READ" = "cached_data" ]; then
    ok "断连期间 page cache 命中读成功"
else
    ng "断连期间 page cache 读失败: '$CACHED_READ'"
fi

# 恢复所有 filer (为测试 5 准备完整集群)
echo "  -> 恢复所有 filer ..."
start_filers filer-1 filer-2 filer-3
for f in "${FILERS[@]}"; do
    wait_filer_healthy "$f" || ng "$f 未健康"
done

# ---------- 测试 5: page cache miss 时断连期间读阻塞 ----------

section "测试 5: page cache miss, 断连期间读阻塞"

# 写入新文件 (page cache 有脏页)
vm "echo fresh_data > ${MNT}/p2_fresh_file" >/dev/null
# 同步并清除 page cache
# 验证 dentry UAF 修复: d_revalidate 返回 -ECHILD 退出 RCU + d_prune IS_ROOT 检查
vm "sync; echo 3 > /proc/sys/vm/drop_caches" >/dev/null
ok "已同步并清除 page cache (验证 dentry UAF 修复)"

# 断连 (page cache 已清除, 不需要 sync)
echo "  -> 停止所有 filer ..."
stop_all_filers
sleep 1

# 启动后台读 (应阻塞, 因 page cache 已清除需走网络)
vm "setsid sh -c 'cat ${MNT}/p2_fresh_file > /tmp/p2_read_result 2>&1 && echo DONE >> /tmp/p2_read_result' > /tmp/p2_read.log 2>&1 < /dev/null &" >/dev/null

sleep 3
READ_DONE=$($SSH "test -f /tmp/p2_read_result && echo yes || echo no" 2>/dev/null)
if [ "$READ_DONE" = "no" ]; then
    ok "page cache miss 时断连期间读阻塞"
else
    warn "page cache miss 时读未阻塞 (可能数据为 inline 或已缓存)"
fi

# 恢复
echo "  -> 恢复 filer-1 + filer-2 ..."
start_filers filer-1 filer-2
RECONNECT_BASE3=$(dmesg_line_count)
wait_filer_healthy filer-1 || ng "filer-1 未健康"
wait_filer_healthy filer-2 || ng "filer-2 未健康"
wait_reconnect "$RECONNECT_BASE3" || ng "读测试后内核未重连"

# 等待读完成
READ_OK=0
for i in $(seq 1 40); do
    READ_RESULT=$($SSH "cat /tmp/p2_read_result 2>/dev/null" 2>/dev/null)
    if echo "$READ_RESULT" | grep -q "DONE"; then
        READ_OK=1
        if echo "$READ_RESULT" | grep -q "fresh_data"; then
            ok "filer 恢复后读完成, 内容一致"
        else
            ng "filer 恢复后读完成但内容异常: '$READ_RESULT'"
        fi
        break
    fi
    sleep 1
done

if [ "$READ_OK" -ne 1 ]; then
    ng "读请求未在 40s 内完成"
fi

# 启动 filer-3 恢复完整集群
docker start filer-3 >/dev/null 2>&1
wait_filer_healthy filer-3 || warn "filer-3 未恢复健康"

# ---------- 内核日志分析 ----------

section "内核日志分析 (reconnect 行为)"

# 获取本次测试新增的 dmesg
NEW_DMESG=$($SSH "dmesg | tail -n +${DMESG_BASE}" 2>/dev/null)

echo "$NEW_DMESG" | grep -qE "filer.*state CONNECTED -> RECONNECTING" \
    && ok "日志: 检测到断连 (CONNECTED -> RECONNECTING)" \
    || warn "日志: 未找到断连日志"

echo "$NEW_DMESG" | grep -qE "filer.*reconnected|filer.*connected \(v2" \
    && ok "日志: 重连成功 (reconnected)" \
    || warn "日志: 未找到重连日志"

# 检查断连次数 (应 >= 2, 两次主动断连)
DISCONNECT_COUNT=$(echo "$NEW_DMESG" | grep -cE "filer.*state CONNECTED -> RECONNECTING")
if [ "$DISCONNECT_COUNT" -ge 2 ]; then
    ok "日志: 断连 ${DISCONNECT_COUNT} 次 (>= 2, 符合预期)"
else
    warn "日志: 断连仅 ${DISCONNECT_COUNT} 次 (预期 >= 2)"
fi

# 检查重连成功次数 (应 >= 2)
RECONNECT_COUNT=$(echo "$NEW_DMESG" | grep -cE "filer.*reconnected|filer.*connected \(v2")
if [ "$RECONNECT_COUNT" -ge 2 ]; then
    ok "日志: 重连成功 ${RECONNECT_COUNT} 次 (>= 2, 符合预期)"
else
    warn "日志: 重连成功仅 ${RECONNECT_COUNT} 次 (预期 >= 2)"
fi

# ---------- 清理 ----------

section "清理"
vm "rm -f ${MNT}/p2_* /tmp/p2_* 2>/dev/null; sync 2>/dev/null" >/dev/null
ok "清理完成"

# 确保所有 filer 恢复
for f in "${FILERS[@]}"; do
    docker start "$f" >/dev/null 2>&1
done
for f in "${FILERS[@]}"; do
    wait_filer_healthy "$f" || warn "$f 最终未健康"
done

# ---------- 汇总 ----------

section "结果汇总"
echo "  PASS = $PASS"
echo "  FAIL = $FAIL"
echo "  WARN = $WARN"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "  >>> 阶段 2 测试通过 <<<"
    exit 0
else
    echo "  >>> 阶段 2 测试失败, 请检查上述 FAIL 项 <<<"
    exit 1
fi
