#!/bin/bash
# =============================================================================
# PowerFS 内核模块验证测试
# 
# 无后端连接的内核模块基础功能验证
# 用于 CI/CD 环境快速验证模块编译和符号导出
# =============================================================================

MODULE_NAME="${MODULE_NAME:-powerfs}"
MOUNT_POINT="${MOUNT_POINT:-/mnt/powerfs_test}"
PASS=0
FAIL=0
TOTAL=0

log_pass() {
    ((PASS++)); ((TOTAL++))
    echo "  [PASS] $*"
}

log_fail() {
    ((FAIL++)); ((TOTAL++))
    echo "  [FAIL] $*"
}

log_info() {
    echo "  [INFO] $*"
}

run_test() {
    local name="$1"
    local cmd="$2"
    echo -n "  测试: $name ... "
    if eval "$cmd" 2>/dev/null; then
        log_pass "$name"
        return 0
    else
        log_fail "$name"
        return 1
    fi
}

echo "=============================================="
echo "PowerFS 内核模块验证测试"
echo "=============================================="

# 1. 编译产物检查
echo ""
echo "1. 编译产物"
run_test "powerfs.ko 存在" "test -f powerfs.ko"
run_test "powerfs.ko 非空" "test -s powerfs.ko"

# 2. ELF 基本信息
echo ""
echo "2. ELF 基本信息"
run_test "ELF 格式正确" "file powerfs.ko | grep -q 'ELF'"
run_test "目标架构匹配" "file powerfs.ko | grep -q 'x86-64'"

# 3. 模块信息
echo ""
echo "3. 模块信息"
run_test "模块版本可读" "modinfo powerfs.ko 2>/dev/null | grep -q 'version\|srcversion'"
run_test "模块名称正确" "modinfo powerfs.ko 2>/dev/null | grep -q 'name'"

# 4. Delta Sync 符号导出检查
echo ""
echo "4. Delta Sync 符号导出"
NM_OUTPUT=$(nm powerfs.ko 2>/dev/null)
run_test "powerfs_net_pool_init 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_pool_init'"
run_test "powerfs_net_pool_cleanup 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_pool_cleanup'"
run_test "powerfs_net_add_server 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_add_server'"
run_test "powerfs_net_find_leader 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_find_leader'"
run_test "powerfs_net_failover 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_failover'"
run_test "powerfs_net_start_monitor 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_start_monitor'"
run_test "powerfs_net_stop_monitor 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_stop_monitor'"
run_test "powerfs_net_set_path_generation 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_set_path_generation'"
run_test "powerfs_net_get_path_generation 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_get_path_generation'"
run_test "powerfs_net_path_stale 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_path_stale'"
run_test "powerfs_net_invalidate_path 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_invalidate_path'"
run_test "powerfs_net_invalidate_dir 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_invalidate_dir'"
run_test "powerfs_net_is_connected 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_is_connected'"
run_test "powerfs_net_lookup 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_lookup'"
run_test "powerfs_net_disconnect 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_disconnect'"

# 5. 多节点/Leader 符号
echo ""
echo "5. 多节点/Leader 符号"
run_test "powerfs_net_switch_leader 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_switch_leader'"
run_test "powerfs_net_leader_ping 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_leader_ping'"
run_test "powerfs_net_has_leader 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_has_leader'"
run_test "powerfs_net_get_leader_idx 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_get_leader_idx'"
run_test "powerfs_net_remove_server 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_remove_server'"

# 6. Delta Sync 实现符号
echo ""
echo "6. Delta Sync 实现符号"
run_test "powerfs_net_pull_delta 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_pull_delta'"
run_test "powerfs_net_push_delta 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_push_delta'"
run_test "powerfs_net_full_sync 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_full_sync'"
run_test "powerfs_net_get_global_generation 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_get_global_generation'"
run_test "powerfs_net_inc_global_generation 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_inc_global_generation'"
run_test "powerfs_net_clear_all_generations 已导出" "echo '$NM_OUTPUT' | grep -q 'powerfs_net_clear_all_generations'"

# 7. VFS 回调符号
echo ""
echo "7. VFS 回调符号"
run_test "powerfs_d_revalidate 已定义" "echo '$NM_OUTPUT' | grep -q 'powerfs_d_revalidate'"
run_test "powerfs_dentry_operations 已定义" "echo '$NM_OUTPUT' | grep -q 'powerfs_dentry_operations'"
run_test "powerfs_fill_super 已定义" "echo '$NM_OUTPUT' | grep -q 'powerfs_fill_super'"
run_test "powerfs_kill_sb_super 已定义" "echo '$NM_OUTPUT' | grep -q 'powerfs_kill_sb_super'"

# 8. 代码静态分析
echo ""
echo "8. 代码完整性"
run_test "powerfs_fs.o 编译产物存在" "test -f powerfs_fs.o"
run_test "powerfs_net.o 编译产物存在" "test -f powerfs_net.o"
run_test "powerfs_tlv.o 编译产物存在" "test -f powerfs_tlv.o"
run_test "powerfs_transport.o 编译产物存在" "test -f powerfs_transport.o"
run_test "powerfs_serializer.o 编译产物存在" "test -f powerfs_serializer.o"
run_test "powerfs_mod.o 编译产物存在" "test -f powerfs_mod.o"

# 汇总
echo ""
echo "=============================================="
echo "测试汇总"
echo "=============================================="
echo "  总数: $TOTAL"
echo "  通过: $PASS"
echo "  失败: $FAIL"
echo "=============================================="

if [ $FAIL -eq 0 ]; then
    echo "  ✓ 所有验证测试通过!"
    exit 0
else
    echo "  ✗ 有 $FAIL 个验证测试失败"
    exit 1
fi