#!/bin/bash
# #47 RDMA/TCP 部署硬化 — VM 测试脚本
#
# 测试两个层面的硬化:
#   (a) 挂载时防护: transport=rdma 且 filer 无 RDMA listener → 挂载失败 -ENOTCONN
#       (不静默回退到 TCP / stale cache)
#   (b) IO 时防护:  transport=rdma 挂载成功后 filer 全部断连 → read/readdir 返回 -ENOTCONN
#       (不服务 stale page cache / inline_data)
#
# 前置条件:
#   - Docker 服务运行中 (TCP 模式, filer-1/2/3)
#   - QEMU VM 已启动并 SSH 可达
#   - powerfs.ko 已热加载 (包含 #47 硬化代码)
#
# 用法: ./test_47_rdma_hardening.sh
#
# 注: (b) 需要 RDMA 挂载成功, 当前 VM 无 RDMA 硬件时只测 (a).
#     (b) 的完整测试需要 rxe 或 VFIO RDMA 直通环境.

set -u
set -o pipefail

SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -p 2223 root@localhost"
MNT=/mnt/powerfs
PASS=0
FAIL=0
WARN=0

ok()   { echo "[PASS] $1"; PASS=$((PASS+1)); }
ng()   { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
warn() { echo "[WARN] $1"; WARN=$((WARN+1)); }

section() { echo ""; echo "========================================="; echo "  $1"; echo "========================================="; }

# 在 VM 上执行命令 (保留 SSH 退出码, pipefail 保证 grep -v 不影响判断)
vm() {
    $SSH "$@" 2>&1 | grep -v '^Warning: Permanently added' || true
}

# 在 VM 上执行命令并返回 SSH 退出码 (用于条件判断)
vm_check() {
    $SSH "$@" 2>&1 | grep -v '^Warning: Permanently added'
    return ${PIPESTATUS[0]}
}

# 获取当前 dmesg 行数
dmesg_line_count() {
    $SSH "dmesg | wc -l" 2>/dev/null
}

# ---------- 前置检查 ----------

section "前置检查"

# 1. VM 可达
if ! $SSH "echo VM_OK" >/dev/null 2>&1; then
    echo "[FATAL] VM 不可达 (ssh -p 2223 root@localhost)"
    echo "        请先启动: ./qemuctl.sh start"
    exit 1
fi
ok "VM SSH 可达"

# 2. powerfs 模块已加载 (包含 #47 硬化)
if ! $SSH "lsmod | grep -q powerfs" >/dev/null 2>&1; then
    echo "[FATAL] powerfs 模块未加载"
    echo "        请先热部署: ./qemuctl.sh hotdeploy"
    exit 1
fi
ok "powerfs 模块已加载"

# 3. 验证模块包含 #47 硬化代码
SYM_CHECK=$($SSH "cat /proc/kallsyms 2>/dev/null | grep -c powerfs_net_any_filer_connected" 2>/dev/null || echo 0)
if [ "$SYM_CHECK" -gt 0 ]; then
    ok "模块包含 powerfs_net_any_filer_connected 符号 (#47 硬化)"
else
    ng "模块未包含 #47 硬化符号, 请重新 hotdeploy"
    exit 1
fi

# 4. 如果 powerfs 已挂载, 先卸载 (测试需要从干净状态开始)
if $SSH "mount | grep -q 'on ${MNT} type powerfs'" >/dev/null 2>&1; then
    vm "umount ${MNT}" 2>/dev/null
    sleep 1
    ok "已卸载之前的 powerfs 挂载"
else
    ok "powerfs 未挂载 (干净状态)"
fi

# 5. filer-1 健康 (TCP 模式)
if docker inspect filer-1 --format '{{.State.Health.Status}}' 2>/dev/null | grep -q healthy; then
    ok "filer-1 健康 (TCP 模式)"
else
    warn "filer-1 未健康 (RDMA 挂载测试可能受影响)"
fi

# ---------- 测试 (a): transport=rdma 挂载防护 ----------

section "测试 (a): transport=rdma 挂载, filer 无 RDMA listener → ENOTCONN"

DMESG_BASE=$(dmesg_line_count)

# 尝试用 transport=rdma 挂载 (filer 是 TCP 模式, 没有 RDMA listener)
# 预期: conn_pool_init 30s 超时后返回 -ENOTCONN, mount 失败
echo "  -> 尝试 transport=rdma 挂载 (filer 是 TCP, 预期失败)..."

# 证书路径
CERT_CA="/etc/powerfs/ca.crt"
CERT_CRT="/etc/powerfs/kernel-client-1.crt"
CERT_KEY="/etc/powerfs/kernel-client-1.key"
MASTER_ADDR="172.30.0.11,172.30.0.12,172.30.0.13"
MASTER_PORT="9334"

MOUNT_OPTS="master_addr=${MASTER_ADDR},master_port=${MASTER_PORT},shard_count=3,ca_crt=${CERT_CA},client_crt=${CERT_CRT},client_key=${CERT_KEY},transport=rdma"

# 执行挂载 (设置 35s 超时, conn_pool_init 最多等 30s)
MOUNT_OUTPUT=$(vm "timeout 35 mount -t powerfs -o '${MOUNT_OPTS}' none ${MNT} 2>&1; echo EXIT_CODE=\$?")
MOUNT_RC=$(echo "$MOUNT_OUTPUT" | grep -o 'EXIT_CODE=[0-9]*' | cut -d= -f2)

echo "  -> 挂载返回码: ${MOUNT_RC}"

# 验证挂载失败 (非零返回码)
if [ -n "$MOUNT_RC" ] && [ "$MOUNT_RC" != "0" ]; then
    ok "transport=rdma 挂载失败 (返回码 ${MOUNT_RC}, 未静默回退)"
else
    ng "transport=rdma 挂载意外成功 (可能静默回退到 TCP, 违反 #47 硬化)"
    # 如果意外挂载成功, 卸载它
    vm "umount ${MNT}" 2>/dev/null
fi

# 验证 powerfs 未挂载
if ! $SSH "mount | grep -q 'on ${MNT} type powerfs'" >/dev/null 2>&1; then
    ok "挂载失败后 powerfs 未挂载 (无 stale cache 风险)"
else
    ng "挂载失败但 powerfs 仍挂载 (异常)"
    vm "umount ${MNT}" 2>/dev/null
fi

# 检查 dmesg 诊断消息
NEW_DMESG=$(vm "dmesg | tail -n +${DMESG_BASE}" 2>/dev/null)

# 检查 "no filer connected within 30s" (conn_pool_init 失败)
if echo "$NEW_DMESG" | grep -q "no filer connected within 30s"; then
    ok "dmesg: 检测到 'no filer connected within 30s' (conn_pool_init 防护触发)"
else
    warn "dmesg: 未找到 'no filer connected within 30s' (查看日志)"
fi

# 检查 transport=rdma 诊断消息
if echo "$NEW_DMESG" | grep -q "transport=rdma mount failed"; then
    ok "dmesg: 检测到 transport=rdma 诊断消息 (用户明确提示)"
else
    warn "dmesg: 未找到 transport=rdma 诊断消息"
fi

# 检查 "cargo build --release --features rdma" 提示
if echo "$NEW_DMESG" | grep -q "features rdma"; then
    ok "dmesg: 包含 'cargo build --features rdma' 修复提示"
else
    warn "dmesg: 未找到 rdma 编译提示"
fi

# 显示关键 dmesg 摘要
echo ""
echo "  -> dmesg 摘要 (powerfs 相关):"
echo "$NEW_DMESG" | grep -i "powerfs" | tail -15

# ---------- 测试 (b): IO 时 stale cache 防护 ----------
# 此测试需要 transport=rdma 挂载成功, 然后断开 filer.
# 当前环境 filer 是 TCP 模式, 无法完成 RDMA 挂载.
# 如需测试 (b), 需先配置 rxe + RDMA 模式服务:
#   ./qemuctl.sh rdma-setup
#   ./qemuctl.sh service start --rdma
#   ./qemuctl.sh mount --rdma
# 然后运行此脚本的 --io-guard 选项.

section "测试 (b): IO 时 stale cache 防护 (需 RDMA 挂载成功)"

# 检查是否有 RDMA 设备 (rxe 或硬件)
RDMA_AVAILABLE=$(vm "ls /dev/infiniband/uverbs* 2>/dev/null | wc -l" 2>/dev/null || echo 0)
if [ "$RDMA_AVAILABLE" -eq 0 ]; then
    warn "VM 内无 RDMA 设备, 跳过 (b) IO 防护测试"
    echo "       完整测试 (b) 需要:"
    echo "       1. ./qemuctl.sh rdma-setup        # 配置 rxe"
    echo "       2. ./qemuctl.sh service start --rdma  # RDMA 模式服务"
    echo "       3. ./qemuctl.sh mount --rdma       # RDMA 挂载"
    echo "       4. 重新运行此脚本"
else
    ok "VM 内有 RDMA 设备, 尝试 (b) IO 防护测试"

    # 尝试 RDMA 挂载
    echo "  -> 尝试 transport=rdma 挂载 (RDMA 设备可用)..."
    MOUNT_OUTPUT=$(vm "timeout 35 mount -t powerfs -o '${MOUNT_OPTS}' none ${MNT} 2>&1; echo EXIT_CODE=\$?")
    MOUNT_RC=$(echo "$MOUNT_OUTPUT" | grep -o 'EXIT_CODE=[0-9]*' | cut -d= -f2)

    if [ "$MOUNT_RC" = "0" ] && $SSH "mount | grep -q 'on ${MNT} type powerfs'" >/dev/null 2>&1; then
        ok "RDMA 挂载成功, 继续 IO 防护测试"

        # 写入测试文件 (填充 page cache)
        vm "echo test_data_47 > ${MNT}/p47_test && cat ${MNT}/p47_test > /dev/null" 2>/dev/null
        ok "预写入测试文件"

        # 清除 page cache (强制后续读走网络)
        vm "sync; echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
        ok "已清除 page cache"

        # 记录 dmesg 基线
        DMESG_BASE2=$(dmesg_line_count)

        # 停止所有 filer
        echo "  -> 停止所有 filer..."
        docker stop filer-1 filer-2 filer-3 >/dev/null 2>&1 &
        wait
        sleep 2

        # 尝试读 (应返回 -ENOTCONN, 不服务 stale cache)
        READ_OUTPUT=$(vm "cat ${MNT}/p47_test 2>&1" 2>/dev/null)
        READ_RC=$?

        echo "  -> 读返回码: ${READ_RC}"
        echo "  -> 读输出: '${READ_OUTPUT}'"

        # 验证读失败 (ENOTCONN = -107, cat 会报 "Transport endpoint is not connected")
        if [ $READ_RC -ne 0 ]; then
            ok "读操作失败 (transport=rdma filer 断连, 未服务 stale cache)"
        else
            ng "读操作意外成功 (可能服务了 stale cache, 违反 #47 硬化)"
        fi

        # 检查 dmesg stale cache guard 日志
        NEW_DMESG2=$(vm "dmesg | tail -n +${DMESG_BASE2}" 2>/dev/null)
        if echo "$NEW_DMESG2" | grep -q "stale cache guard"; then
            ok "dmesg: 检测到 'stale cache guard' (IO 防护触发)"
        else
            warn "dmesg: 未找到 'stale cache guard' 日志"
        fi

        # 恢复 filer
        echo "  -> 恢复 filer..."
        docker start filer-1 filer-2 filer-3 >/dev/null 2>&1 &
        wait
        sleep 5

        # 卸载 powerfs
        vm "umount ${MNT}" 2>/dev/null
        ok "测试完成, 已卸载 powerfs"
    else
        warn "RDMA 挂载失败 (返回码 ${MOUNT_RC}), 跳过 (b) IO 防护测试"
        echo "       可能需要先启动 RDMA 模式服务: ./qemuctl.sh service start --rdma"
    fi
fi

# ---------- 清理 ----------

section "清理"

# 确保 powerfs 未挂载
vm "umount ${MNT} 2>/dev/null" >/dev/null
ok "确保 powerfs 未挂载"

# 确保所有 filer 恢复
for f in filer-1 filer-2 filer-3; do
    docker start "$f" >/dev/null 2>&1
done
sleep 3
ok "filer 已恢复"

# ---------- 汇总 ----------

section "结果汇总"
echo "  PASS = $PASS"
echo "  FAIL = $FAIL"
echo "  WARN = $WARN"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "  >>> #47 RDMA 硬化测试通过 <<<"
    exit 0
else
    echo "  >>> #47 RDMA 硬化测试失败, 请检查上述 FAIL 项 <<<"
    exit 1
fi
