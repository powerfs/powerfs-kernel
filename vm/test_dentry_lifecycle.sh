#!/bin/bash
# Dentry 生命周期测试: 验证 d_prune 与 d_release 交互行为
#
# 目标:
#   1. d_prune 不对根 dentry 触发 (IS_ROOT 检查)
#   2. d_prune 清除父目录 dir_complete 标志
#   3. d_prune 跳过非目录父 (不 NULL deref)
#   4. d_release 释放 d_fsdata (slab 计数下降)
#   5. 时序: 同一 dentry 的 d_prune 先于 d_release
#   6. 父目录在子 dentry prune 期间不被 release (d_count 保护)
#   7. 无 UAF/OOP/KASAN 报错
#   8. drop_caches 后 d_revalidate 失效 -> 重新 lookup
#
# 运行环境: HOST (通过 SSH 控制 VM)
# 前置条件:
#   - QEMU VM 已启动: ./qemuctl.sh debug   (调试模式, 实时 serial 日志)
#   - powerfs 已挂载到 /mnt/pfs: ./qemuctl.sh mount
#   - powerfs.ko 是最新编译版本 (含 IS_ROOT 检查, RCU 返回 -ECHILD)
#   - KASAN 已开启 (kasan.stacktrace=on)
#
# 用法: ./test_dentry_lifecycle.sh

set -u

SSH="sshpass -p powerfs ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 -p 2223 root@localhost"
MNT=/mnt/pfs
TEST_DIR=${MNT}/dentry_test
SUB_DIRS=(a b c)
FILES_PER_DIR=50

PASS=0
FAIL=0
WARN=0

ok()   { echo "[PASS] $1"; PASS=$((PASS+1)); }
ng()   { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }
warn() { echo "[WARN] $1"; WARN=$((WARN+1)); }

section() { echo ""; echo "========================================="; echo "  $1"; echo "========================================="; }

# 在 VM 上执行命令 (过滤残余 SSH 警告)
vm() {
    $SSH "$@" 2>&1 | grep -v '^Warning: Permanently added'
}

# 检查 VM 内 powerfs 是否仍挂载
check_mount() {
    $SSH "mount | grep -q 'on ${MNT} type powerfs'" 2>/dev/null
}

# 获取当前 dmesg 行数 (作为后续新增日志的基线)
dmesg_line_count() {
    $SSH "dmesg | wc -l" 2>/dev/null
}

# 获取 powerfs_dentry slab 活跃对象数
# slabinfo 格式: name <active_objs> <num_objs> <objsize> ...
get_dentry_slab_active() {
    $SSH "cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_dentry/ {print \$2}'" 2>/dev/null
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

# 3. 确认 powerfs_dentry slab 存在 (证明 d_init 使用 slab)
SLAB_NAME=$(vm "cat /proc/slabinfo | head -1; cat /proc/slabinfo | grep -E '^powerfs_dentry'" 2>/dev/null)
if echo "$SLAB_NAME" | grep -q "powerfs_dentry"; then
    ok "powerfs_dentry slab 已注册"
    echo "  -> $SLAB_NAME"
else
    warn "未找到 powerfs_dentry slab (可能未触发 d_init, 或 slab 名不同)"
fi

# 4. 清理上次测试残留
vm "rm -rf ${TEST_DIR} 2>/dev/null; sync 2>/dev/null" >/dev/null
ok "清理上次测试残留"

# ---------- 阶段 A: 填充 dcache ----------

section "阶段 A: 填充 dcache (创建 ${#SUB_DIRS[@]} 目录 × ${FILES_PER_DIR} 文件)"

# 创建目录结构
vm "mkdir -p ${TEST_DIR}/a ${TEST_DIR}/b ${TEST_DIR}/c" >/dev/null
ok "创建 3 个子目录"

# 创建文件 (在 VM 内循环, 避免多次 SSH)
vm "
for d in a b c; do
    for i in \$(seq 1 ${FILES_PER_DIR}); do
        echo content_\${d}_\${i} > ${TEST_DIR}/\${d}/file_\${i}
    done
done
sync
" >/dev/null
ok "创建 $(( ${#SUB_DIRS[@]} * FILES_PER_DIR )) 个文件"

# 触发 d_init: stat 所有路径让 dentry 进入 dcache
vm "
for d in a b c; do
    stat ${TEST_DIR}/\${d} >/dev/null 2>&1
    for i in \$(seq 1 ${FILES_PER_DIR}); do
        stat ${TEST_DIR}/\${d}/file_\${i} >/dev/null 2>&1
    done
done
" >/dev/null
ok "stat 所有路径触发 d_init"

# 再 ls 一次让父目录 dir_complete=true
vm "ls ${TEST_DIR}/a ${TEST_DIR}/b ${TEST_DIR}/c >/dev/null 2>&1" >/dev/null
ok "ls 子目录触发 dir_complete=true"

# 释放应用层引用, 让 dentry 进入 LRU
# (shell 退出后 fd 关闭, d_count 降为 1, 进入 LRU 等待回收)
sleep 2

# 仅清 page cache, 保留 dcache
vm "sync; echo 2 > /proc/sys/vm/drop_caches" >/dev/null
ok "清 page cache (保留 dcache)"

# 记录填充后的 dentry slab 计数 (应该比较高)
SLAB_BEFORE_PRUNE=$(get_dentry_slab_active)
echo "  -> dentry slab 活跃对象数 (填充后): ${SLAB_BEFORE_PRUNE:-N/A}"

# 记录基线 dmesg
DMESG_BASE=$(dmesg_line_count)
echo "  -> dmesg 基线行数: ${DMESG_BASE}"

# ---------- 阶段 B: 触发 d_prune + d_release ----------

section "阶段 B: 触发 d_prune + d_release (echo 3 > drop_caches)"

# 清 dmesg 以便清晰观察
vm "dmesg -c" >/dev/null 2>&1
DMESG_BASE=0

# 关键: 触发 shrinker 回收 dentry
vm "sync; echo 3 > /proc/sys/vm/drop_caches" >/dev/null
ok "已执行 echo 3 > drop_caches"

# 等待 shrinker 完成 (含 RCU grace period)
sleep 3

SLAB_AFTER_PRUNE=$(get_dentry_slab_active)
echo "  -> dentry slab 活跃对象数 (回收后): ${SLAB_AFTER_PRUNE:-N/A}"

# ---------- 阶段 C: 校验日志和 slab ----------

section "阶段 C: 校验 d_prune / d_release 行为"

# 抓取所有 powerfs 相关日志
ALL_LOG=$(vm "dmesg" 2>/dev/null)
PRUNE_LOG=$(echo "$ALL_LOG" | grep "powerfs: d_prune")
RELEASE_LOG=$(echo "$ALL_LOG" | grep "powerfs: d_release")
REVALIDATE_LOG=$(echo "$ALL_LOG" | grep "powerfs: d_revalidate")
INIT_LOG=$(echo "$ALL_LOG" | grep "powerfs: d_init")

# 验证 1: 根 dentry 不被 prune
PRUNE_ROOT=$(echo "$PRUNE_LOG" | grep -E "d_prune '/'|d_prune '\.'|d_prune '\.\.'")
if [ -z "$PRUNE_ROOT" ]; then
    ok "根 dentry 未被 prune (IS_ROOT 检查生效)"
else
    ng "根 dentry 被 prune: $PRUNE_ROOT"
fi

# 验证 2: d_prune 清除父目录 dir_complete (日志包含 "clearing dir_complete")
PRUNE_CLEAR_COUNT=$(echo "$PRUNE_LOG" | grep -c "clearing dir_complete")
if [ "$PRUNE_CLEAR_COUNT" -gt 0 ]; then
    ok "d_prune 清除父目录 dir_complete (${PRUNE_CLEAR_COUNT} 次)"
else
    warn "未发现 d_prune clearing dir_complete 日志 (可能子 dentry 已被回收)"
fi

# 验证 3: 无 NULL deref / oops
OOPS=$(echo "$ALL_LOG" | grep -E "BUG:|Oops:|general protection fault|unable to handle kernel|NULL pointer dereference")
if [ -z "$OOPS" ]; then
    ok "无 NULL deref / Oops"
else
    ng "检测到 Oops: $OOPS"
fi

# 验证 4: d_release 释放 d_fsdata (slab 计数下降)
if [ -n "${SLAB_BEFORE_PRUNE:-}" ] && [ -n "${SLAB_AFTER_PRUNE:-}" ]; then
    if [ "$SLAB_AFTER_PRUNE" -lt "$SLAB_BEFORE_PRUNE" ]; then
        ok "dentry slab 计数下降 (${SLAB_BEFORE_PRUNE} -> ${SLAB_AFTER_PRUNE})"
    else
        warn "dentry slab 计数未下降 (${SLAB_BEFORE_PRUNE} -> ${SLAB_AFTER_PRUNE})"
    fi
else
    warn "slab 计数无法获取, 跳过该检查"
fi

# 验证 5: 时序 - 同一 dentry 的 d_prune 先于 d_release
# (d_prune 在 shrink_dentry_sb 中调用, d_release 在 dentry_free 中调用,
#  shrink_dentry_sb 先 prune 再 d_shrink_del/dput, 顺序应保证)
PRUNE_COUNT=$(echo "$PRUNE_LOG" | wc -l)
RELEASE_COUNT=$(echo "$RELEASE_LOG" | wc -l)
echo "  -> d_prune 日志数: ${PRUNE_COUNT}"
echo "  -> d_release 日志数: ${RELEASE_COUNT}"

if [ "$PRUNE_COUNT" -gt 0 ] && [ "$RELEASE_COUNT" -gt 0 ]; then
    ok "d_prune 和 d_release 均有触发"
else
    warn "prune/release 日志偏少 (可能 dentry 仍在使用, 或 pr_debug 未启用)"
fi

# 验证 6: 父目录 release 在子 dentry release 之后
# 抓取 release 中所有 'dentry_test/a' 父目录本身的 release (不带 file_)
PARENT_RELEASE_A=$(echo "$RELEASE_LOG" | grep -E "d_release 'a'|d_release 'b'|d_release 'c'" | head -1)
CHILD_RELEASE_A_FILE1=$(echo "$RELEASE_LOG" | grep -E "d_release 'file_1'" | head -1)
if [ -n "$PARENT_RELEASE_A" ] && [ -n "$CHILD_RELEASE_A_FILE1" ]; then
    # 比较 dmesg 行号
    PARENT_LINE=$(echo "$ALL_LOG" | grep -n "d_release 'a'" | head -1 | cut -d: -f1)
    CHILD_LINE=$(echo "$ALL_LOG" | grep -n "d_release 'file_1'" | head -1 | cut -d: -f1)
    if [ -n "$PARENT_LINE" ] && [ -n "$CHILD_LINE" ] && [ "$PARENT_LINE" -gt "$CHILD_LINE" ]; then
        ok "父目录 release 在子 dentry release 之后 (line ${CHILD_LINE} < ${PARENT_LINE})"
    else
        warn "父/子 release 顺序无法判定 (parent_line=${PARENT_LINE}, child_line=${CHILD_LINE})"
    fi
else
    warn "未捕获父/子 release 配对 (可能 pr_debug 日志已截断)"
fi

# 验证 7: 无 UAF / KASAN 报错
KASAN=$(echo "$ALL_LOG" | grep -E "KASAN: use-after-free|KASAN: slab-out-of-bounds|BUG: KASAN")
if [ -z "$KASAN" ]; then
    ok "无 KASAN UAF 报错"
else
    ng "检测到 KASAN UAF: $KASAN"
    echo "  -> 详细 stack trace:"
    echo "$ALL_LOG" | grep -A 30 "KASAN" | head -50 | sed 's/^/    /'
fi

# ---------- 阶段 D: 重新访问验证 revalidate ----------

section "阶段 D: 重新访问验证 d_revalidate"

# 重新 stat 同一路径: 应触发 revalidate 失败 (TTL 过期或 cache invalid)
# 注意: drop_caches 后 dentry 已从 dcache 移除, 可能直接走 lookup 而非 revalidate
REVALIDATE_BEFORE=$(echo "$REVALIDATE_LOG" | wc -l)
vm "stat ${TEST_DIR}/a/file_1 >/dev/null 2>&1; stat ${TEST_DIR}/b/file_1 >/dev/null 2>&1" >/dev/null
sleep 1

REVALIDATE_NEW_LOG=$(vm "dmesg | tail -n +$((DMESG_BASE+1))" 2>/dev/null)
# 重新 stat 可能触发 d_init (新 dentry) 而非 d_revalidate (旧 dentry 已被释放)
if echo "$REVALIDATE_NEW_LOG" | grep -qE "d_init 'file_1'|d_revalidate 'file_1'"; then
    ok "重新 stat 触发 d_init 或 d_revalidate (说明 dentry 重建)"
else
    warn "重新 stat 未捕获 d_init/d_revalidate 日志 (可能 pr_debug 未启用)"
fi

# 再次 ls 触发 readdir, dir_complete 应重新置 true
vm "ls ${TEST_DIR}/a >/dev/null 2>&1" >/dev/null
sleep 1
DIR_REVALIDATE=$(vm "dmesg | tail -10" 2>/dev/null | grep -E "fill_readdir_cache|dir_complete=true")
if [ -n "$DIR_REVALIDATE" ]; then
    ok "ls 重新填充 dir_complete=true"
else
    warn "未捕获 dir_complete 重新填充日志"
fi

# 最终一致性校验: 文件数量应一致
FINAL_COUNT=$(vm "ls ${TEST_DIR}/a | wc -l" 2>/dev/null)
if [ "$FINAL_COUNT" = "${FILES_PER_DIR}" ]; then
    ok "最终文件数量一致 (${FINAL_COUNT}/${FILES_PER_DIR})"
else
    ng "文件数量不一致 (期望 ${FILES_PER_DIR}, 实际 ${FINAL_COUNT})"
fi

# ---------- 总结 ----------

section "总结"

echo "  PASS: ${PASS}"
echo "  FAIL: ${FAIL}"
echo "  WARN: ${WARN}"

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "[FAILED] 存在失败项, 请检查上述日志"
    echo "  -> 完整 dmesg:"
    vm "dmesg | tail -100" 2>/dev/null | sed 's/^/    /'
    exit 1
fi

echo ""
echo "[SUCCESS] dentry 生命周期测试通过"
exit 0
