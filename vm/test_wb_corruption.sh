#!/bin/bash
# 测试 writeback 数据损坏修复
set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

echo "=== 部署新模块 ==="
vm "sync; umount /mnt/pfs 2>/dev/null; rmmod powerfs 2>/dev/null; cp /mnt/host/powerfs.ko /powerfs.ko; insmod /powerfs.ko shard_count=2; mount -t powerfs none /mnt/pfs; echo mounted" 2>/dev/null

echo "=== 清理旧数据 ==="
vm "rm -rf /mnt/pfs/* /tmp/t2_src /tmp/t2_*" 2>/dev/null
vm "sync" 2>/dev/null

echo "=== 创建源数据 (100 文件) ==="
vm 'mkdir -p /tmp/t2_src; for i in $(seq 1 100); do dd if=/dev/urandom bs=1K count=$((i*10)) of=/tmp/t2_src/file_$i 2>/dev/null; done; find /tmp/t2_src -type f | wc -l' 2>/dev/null

echo "=== 10 次 tar czf 损坏测试 ==="
PASS=0
FAIL=0
for i in $(seq 1 10); do
    RET=$(vm "tar czf /mnt/pfs/test_${i}.tar.gz -C /tmp t2_src 2>/dev/null; sync; gzip -t /mnt/pfs/test_${i}.tar.gz 2>&1; echo ret=\$?" 2>/dev/null)
    if echo "$RET" | grep -q 'ret=0'; then
        PASS=$((PASS+1))
        echo "  attempt $i: OK"
    else
        FAIL=$((FAIL+1))
        echo "  attempt $i: FAIL ($RET)"
    fi
done
echo "=== 结果: PASS=$PASS FAIL=$FAIL ==="

echo "=== 清理 ==="
vm "rm -f /mnt/pfs/test_*.tar.gz" 2>/dev/null
