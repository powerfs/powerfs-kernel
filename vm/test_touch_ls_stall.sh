#!/bin/bash
# PowerFS RCU stall 隔离测试: 仅 touch + ls, 不触发 writeback
#
# 目的: 验证 stall 是否由 writeback 异步路径引起
# - touch: 创建空文件, 不写数据, 不触发 writeback
# - ls: 触发 d_revalidate (RCU path walk)
# - ls nonexist: 触发负 dentry 查找
#
# 用法: ./test_touch_ls_stall.sh [持续时间秒数]
# 默认: 120 秒

DURATION="${1:-120}"
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 -p 2223 root@localhost"

echo "=== PowerFS touch+ls stall 隔离测试 ==="
echo "持续时间: ${DURATION}s"
echo "操作: touch (无写数据) + ls + 负 dentry 查找"
echo "启动时间: $(date)"
echo ""

# 确保已挂载
$SSH 'mountpoint /mnt/pfs 2>/dev/null || { mkdir -p /mnt/pfs && mount -t powerfs none /mnt/pfs; }' 2>/dev/null

# 清理旧文件
$SSH 'rm -f /mnt/pfs/touch_test_* 2>/dev/null; ls /mnt/pfs/ > /dev/null 2>&1' 2>/dev/null

echo "=== 开始测试 ==="
START=$(date +%s)

$SSH "
START=\$(date +%s)
round=0
while true; do
    round=\$((round + 1))

    # touch: 创建空文件 (不写数据, 不触发 writeback)
    for i in \$(seq 1 10); do
        touch /mnt/pfs/touch_test_\${round}_\${i}.txt 2>&1
    done

    # ls: 触发 d_revalidate (正 dentry)
    ls /mnt/pfs/ > /dev/null 2>&1
    ls /mnt/pfs/ > /dev/null 2>&1
    ls /mnt/pfs/ > /dev/null 2>&1

    # ls 不存在文件: 触发负 dentry 查找 + d_revalidate
    ls /mnt/pfs/nonexist_\${round}_1 2>&1
    ls /mnt/pfs/nonexist_\${round}_2 2>&1
    ls /mnt/pfs/nonexist_\${round}_3 2>&1

    # stat 已存在文件: 触发正 dentry revalidate
    stat /mnt/pfs/touch_test_\${round}_1.txt > /dev/null 2>&1

    NOW=\$(date +%s)
    ELAPSED=\$((NOW - START))
    if [ \$ELAPSED -ge ${DURATION} ]; then
        echo \"完成 \${round} 轮, 耗时 \${ELAPSED}s\"
        break
    fi

    # 每 10 轮打印进度
    if [ \$((round % 10)) -eq 0 ]; then
        echo \"--- Round \${round}, \${ELAPSED}s ---\"
    fi
done

echo ''
echo '=== dmesg 检查 ==='
dmesg | grep -i 'rcu.*stall\|__d_lookup_rcu\|workqueue lockup\|oops\|BUG:' | tail -20
echo ''
echo '=== VM 状态 ==='
uptime
echo \"文件数: \$(ls /mnt/pfs/ | wc -l)\"
echo '=== 测试结束 ==='
" 2>&1

END=$(date +%s)
echo ""
echo "=== 总耗时: $((END - START))s ==="
echo "完成时间: $(date)"
