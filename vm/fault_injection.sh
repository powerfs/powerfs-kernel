#!/bin/bash
# PowerFS Phase F 故障注入辅助函数库
#
# 用途: 被 test_phase_f_v2.sh source 使用, 提供统一的故障注入原语.
#
# 故障注入方式对照:
#   docker stop          - 进程退出, 发 FIN  → sk_state_change 即时感知 (毫秒级)
#   docker pause         - 进程冻结, 不发 FIN → keepalive 检测 (~11s)
#   docker network disconnect - 网络分区, 不发 FIN → keepalive 检测 (~11s)
#   docker exec kill -9  - 模拟 panic, 进程异常退出 → 发 RST/FIN
#
# 网络信息:
#   Docker network: docker_powerfs-network (172.30.0.0/16)
#   filer-1: 172.30.0.31:9334
#   filer-2: 172.30.0.32:9334
#   filer-3: 172.30.0.33:9334

# SSH 到 VM 的统一接口 (密码 powerfs, 端口 2223, 与 qemuctl.sh 一致)
SSH_VM="sshpass -p powerfs ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -p 2223 root@localhost"

# Docker network 名称 (用于 network disconnect/connect)
DOCKER_NET="${DOCKER_NET:-docker_powerfs-network}"

# Filer 列表
FILERS=(filer-1 filer-2 filer-3)
FILER_IPS=(172.30.0.31 172.30.0.32 172.30.0.33)
FILER_PORT=9334

# ---------- VM 交互 ----------

# 在 VM 内执行命令
vm() {
    $SSH_VM "$@" 2>&1 | grep -v '^Warning: Permanently added'
    return ${PIPESTATUS[0]}
}

# 获取 VM 内 dmesg 行数 (作为新增日志基线)
dmesg_line_count() {
    $SSH_VM "dmesg | wc -l" 2>/dev/null
}

# 获取宿主机 qemu.log 行数 (作为 serial 日志基线)
serial_line_count() {
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ]; then
        wc -l < "${qemu_log}" 2>/dev/null
    else
        echo 0
    fi
}

# 获取 qemu.log 新增行 (从基线行开始)
serial_since() {
    local base=$1
    local qemu_log="${SCRIPT_DIR}/output/qemu.log"
    if [ -f "${qemu_log}" ] && [ "${base:-0}" -gt 0 ]; then
        tail -n +${base} "${qemu_log}" 2>/dev/null
    elif [ -f "${qemu_log}" ]; then
        cat "${qemu_log}" 2>/dev/null
    fi
}

# 获取 VM 内新增 dmesg (从基线行开始)
dmesg_since() {
    local base=$1
    $SSH_VM "dmesg | tail -n +${base}" 2>/dev/null
}

# 检查 VM 是否存活 (SSH 可达)
vm_alive() {
    $SSH_VM "echo ALIVE" >/dev/null 2>&1
}

# 检查 powerfs 是否仍挂载
check_mount() {
    $SSH_VM "mount | grep -q 'on /mnt/pfs type powerfs'" 2>/dev/null
}

# ---------- Filer 状态 ----------

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

# 获取所有 filer 健康状态
all_filers_healthy() {
    for f in "${FILERS[@]}"; do
        if ! wait_filer_healthy "$f"; then
            return 1
        fi
    done
    return 0
}

# 获取当前 leader filer (从 dmesg 解析最近一次 redirect/leader 日志)
# 返回 filer 容器名 (filer-1/filer-2/filer-3) 或空
get_current_leader() {
    local dmesg_out
    dmesg_out=$($SSH_VM "dmesg" 2>/dev/null)

    # 尝试从 redirect 日志解析 leader 地址 (格式: "redirect to leader 172.30.0.36:9334")
    local leader_addr
    leader_addr=$(echo "$dmesg_out" | grep -oE 'redirect to leader [0-9.]+' | tail -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' || true)

    if [ -z "$leader_addr" ]; then
        # 无 redirect 日志, 尝试从 "shard.*VALID.*filer" 解析
        leader_addr=$(echo "$dmesg_out" | grep -oE 'route -> VALID.*filer' | tail -1 | grep -oE '172\.30\.0\.[0-9]+' || true)
        if [ -n "$leader_addr" ]; then
            # VALID 行不含 IP, 含 filer 编号; 从 redirect 日志再找
            leader_addr=$(echo "$dmesg_out" | grep -oE 'leader 172\.30\.0\.[0-9]+' | tail -1 | grep -oE '172\.30\.0\.[0-9]+' || true)
        fi
    fi

    if [ -z "$leader_addr" ]; then
        # 仍无信息, 默认 filer-1 是 leader (Raft 通常选 id 最小的)
        echo "filer-1"
        return
    fi

    # IP → 容器名映射
    case "$leader_addr" in
        172.30.0.35) echo "filer-1" ;;
        172.30.0.36) echo "filer-2" ;;
        172.30.0.37) echo "filer-3" ;;
        *) echo "filer-1" ;;
    esac
}

# ---------- 故障注入原语 ----------

# 停止单个 filer (发 FIN, 测试即时感知)
inject_stop_filer() {
    local filer=$1
    docker stop "$filer" >/dev/null 2>&1
}

# 启动单个 filer
inject_start_filer() {
    local filer=$1
    docker start "$filer" >/dev/null 2>&1
}

# 停止所有 filer
inject_stop_all_filers() {
    local f
    for f in "${FILERS[@]}"; do
        docker stop "$f" >/dev/null 2>&1 &
    done
    wait
}

# 启动指定 filer (并发, 等待 healthy)
inject_start_filers() {
    local f
    for f in "$@"; do
        docker start "$f" >/dev/null 2>&1 &
    done
    wait
    # 等待 Raft 选举完成
    sleep 3
}

# 恢复所有 filer 到 healthy
inject_restore_all_filers() {
    local f
    for f in "${FILERS[@]}"; do
        docker start "$f" >/dev/null 2>&1
    done
    for f in "${FILERS[@]}"; do
        wait_filer_healthy "$f" || true
    done
}

# 网络分区单个 filer (静默死亡, 不发 FIN, 测试 keepalive)
# 用 docker network disconnect 实现
inject_network_partition() {
    local filer=$1
    docker network disconnect "$DOCKER_NET" "$filer" >/dev/null 2>&1 || true
}

# 恢复网络分区
inject_network_restore() {
    local filer=$1
    docker network connect "$DOCKER_NET" "$filer" >/dev/null 2>&1 || true
    sleep 1
}

# 暂停 filer 进程 (静默死亡, 进程冻结不发 FIN)
inject_pause_filer() {
    local filer=$1
    docker pause "$filer" >/dev/null 2>&1
}

# 恢复暂停的 filer
inject_unpause_filer() {
    local filer=$1
    docker unpause "$filer" >/dev/null 2>&1
}

# 模拟 filer panic (docker exec kill -9 主进程)
inject_panic_filer() {
    local filer=$1
    # 获取 filer 容器内 powerfs-filer 进程 PID 并 kill -9
    docker exec "$filer" pkill -9 powerfs-filer 2>/dev/null || true
    # docker 会自动重启 (restart=always), 等待恢复
}

# ---------- 内核日志检查 ----------

# 检查 dmesg 是否有 crash/OOPS/UAF
# 注意: 正则需排除命令行参数 "panic=-1" 和 kmemleak 初始化日志等误报
check_kernel_crash() {
    local dmesg_out
    dmesg_out=$($SSH_VM "dmesg" 2>/dev/null)

    # 检查 OOPS/BUG/panic (用 ^ 匹配行首, 排除命令行参数中的 "panic=-1")
    if echo "$dmesg_out" | grep -qE "^BUG:|^Oops:|Kernel panic|kernel BUG|Unable to handle|Call Trace"; then
        echo "CRASH"
        return
    fi

    # 检查 UAF (排除 kmemleak 初始化日志, 只匹配实际泄漏报告)
    if echo "$dmesg_out" | grep -qE "use-after-free|KASAN:|kmemleak:.*new leak|kmemleak:.*unreferenced object|general protection fault"; then
        echo "UAF"
        return
    fi

    # 检查 WARNING (非致命, 但需关注)
    if echo "$dmesg_out" | grep -qE "WARNING: CPU:.*PID:"; then
        echo "WARNING"
        return
    fi

    echo "OK"
}

# 获取调度器线程数 (从 dmesg 解析)
get_sched_thread_count() {
    local dmesg_out
    dmesg_out=$($SSH_VM "dmesg" 2>/dev/null)
    echo "$dmesg_out" | grep -oE 'started [0-9]+ scheduler threads' | grep -oE '[0-9]+' | head -1
}

# 获取 VM 内调度器线程 (从 /proc 解析)
get_sched_threads_proc() {
    $SSH_VM "ps | grep 'pfs_sched' | grep -v grep" 2>/dev/null
}

# 检查连接池状态 (从 dmesg 解析最近的连接信息)
get_connection_status() {
    local dmesg_out
    dmesg_out=$($SSH_VM "dmesg | tail -50" 2>/dev/null)
    echo "$dmesg_out" | grep -E 'filer.*connected|filer.*state|reconnect'
}

# ---------- 时间测量 ----------

# 获取当前时间戳 (毫秒)
timestamp_ms() {
    date +%s%3N
}

# 计算两个时间戳差值 (毫秒)
elapsed_ms() {
    local start=$1
    local end=$2
    echo $((end - start))
}
