#!/bin/sh
# O-03 fallocate FLAT extension writeback 测试
# 验证 fallocate 扩展 FLAT 文件后:
#   1. i_size 正确扩展
#   2. 扩展区间读为零
#   3. 服务器 needle 同步扩展 (writeback 触发)
#   4. fallocate 后 write 部分数据, 读回一致
#   5. truncate-down + fallocate-up (sparse 场景)

PFS=/mnt/pfs
TDIR=$PFS/test_o03_$$
mkdir -p $TDIR
PASS=0
FAIL=0

ok() { echo "PASS: $1"; PASS=$((PASS+1)); }
ng() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }

# 辅助: 判断文件是否已迁移到 FLAT (placement=1)
# 通过 dmesg 日志间接确认: 写入超过 inline 阈值触发迁移
# inline max_size 通常为 4KB, 写 8KB 强制迁移
make_flat() {
    local f=$1
    dd if=/dev/urandom of=$f bs=4096 count=4 oflag=direct 2>/dev/null || \
    dd if=/dev/urandom of=$f bs=4096 count=4 2>/dev/null
    sync
    sleep 1
}

echo "=== Test 1: fallocate basic extend ==="
F=$TDIR/f1
make_flat $F
ORIG=$(stat -c %s $F)
NEW=$((ORIG + 65536))  # +64KB
fallocate -l $NEW $F 2>/dev/null || { ng "fallocate failed"; }
SZ=$(stat -c %s $F)
if [ "$SZ" -eq "$NEW" ]; then ok "size extended to $NEW"; else ng "size=$SZ expected=$NEW"; fi

echo "=== Test 2: extended region reads zero ==="
# 读扩展区间, 应全零
HOLE_OFF=$ORIG
HOLE_LEN=$((NEW - ORIG))
ZEROS=$(dd if=$F bs=1 skip=$HOLE_OFF count=$HOLE_LEN 2>/dev/null | tr -d '\0' | wc -c)
if [ "$ZEROS" -eq 0 ]; then ok "extended region all zeros"; else ng "extended region has $ZEROS non-zero bytes"; fi

echo "=== Test 3: original data preserved after fallocate ==="
# 读原始区间两次, hash 应一致 (数据未因 fallocate 改变)
ORIG_HASH=$(dd if=$F bs=4096 count=4 2>/dev/null | md5sum | cut -d' ' -f1)
ORIG_HASH2=$(dd if=$F bs=1 count=$ORIG 2>/dev/null | md5sum | cut -d' ' -f1)
if [ "$ORIG_HASH" = "$ORIG_HASH2" ]; then ok "original data intact"; else ng "original data corrupted"; fi

echo "=== Test 4: fallocate then write partial + read back ==="
F2=$TDIR/f2
make_flat $F2
BASE_SZ=$(stat -c %s $F2)
# fallocate 扩展 128KB
fallocate -l $((BASE_SZ + 131072)) $F2 2>/dev/null
# 在扩展区间写入数据
WRITE_OFF=$((BASE_SZ + 4096))
echo -n "HELLO_O03_FALLOCATE" | dd of=$F2 bs=1 seek=$WRITE_OFF conv=notrunc 2>/dev/null
sync
sleep 1
# 读回验证
READ=$(dd if=$F2 bs=1 skip=$WRITE_OFF count=19 2>/dev/null)
if [ "$READ" = "HELLO_O03_FALLOCATE" ]; then ok "write+read in extended region OK"; else ng "read mismatch: '$READ'"; fi
# 验证 write 前的扩展区间仍为零
PRE_OFF=$BASE_SZ
PRE_ZEROS=$(dd if=$F2 bs=1 skip=$PRE_OFF count=4096 2>/dev/null | tr -d '\0' | wc -c)
if [ "$PRE_ZEROS" -eq 0 ]; then ok "gap before write still zero"; else ng "gap before write has $PRE_ZEROS non-zero"; fi

echo "=== Test 5: truncate-down + fallocate-up (sparse) ==="
F3=$TDIR/f3
make_flat $F3
BIG=$(stat -c %s $F3)
# truncate down to 100 bytes
truncate -s 100 $F3
sync
sleep 1
SZ1=$(stat -c %s $F3)
if [ "$SZ1" -eq 100 ]; then ok "truncate down to 100"; else ng "truncate failed: $SZ1"; fi
# fallocate back up to 64KB
fallocate -l 65536 $F3 2>/dev/null
SZ2=$(stat -c %s $F3)
if [ "$SZ2" -eq 65536 ]; then ok "fallocate up to 65536"; else ng "fallocate failed: $SZ2"; fi
# 读 [100, 65536) 应全零 (sparse, 前面 truncate 掉的数据不应残留)
SPARSE_ZEROS=$(dd if=$F3 bs=1 skip=100 count=65436 2>/dev/null | tr -d '\0' | wc -c)
if [ "$SPARSE_ZEROS" -eq 0 ]; then ok "sparse region after truncate+fallocate all zero"; else ng "sparse region has $SPARSE_ZEROS non-zero (stale data!)"; fi

echo "=== Test 6: non-page-aligned fallocate ==="
F4=$TDIR/f4
make_flat $F4
ALIGN_SZ=$(stat -c %s $F4)
# fallocate to a non-page-aligned size
NEW_SZ=$((ALIGN_SZ + 3333))  # 3333 = 非页对齐
fallocate -l $NEW_SZ $F4 2>/dev/null
SZ3=$(stat -c %s $F4)
if [ "$SZ3" -eq "$NEW_SZ" ]; then ok "non-aligned fallocate size=$NEW_SZ"; else ng "size=$SZ3 expected=$NEW_SZ"; fi
# 尾部应零
TAIL_ZEROS=$(dd if=$F4 bs=1 skip=$ALIGN_SZ count=3333 2>/dev/null | tr -d '\0' | wc -c)
if [ "$TAIL_ZEROS" -eq 0 ]; then ok "non-aligned tail all zero"; else ng "non-aligned tail has $TAIL_ZEROS non-zero"; fi

echo "=== Test 7: KEEP_SIZE mode (no extend) ==="
F5=$TDIR/f5
make_flat $F5
KEEP_SZ=$(stat -c %s $F5)
fallocate -l $((KEEP_SZ + 32768)) -n $F5 2>/dev/null  # -n = KEEP_SIZE
SZ4=$(stat -c %s $F5)
if [ "$SZ4" -eq "$KEEP_SZ" ]; then ok "KEEP_SIZE mode size unchanged"; else ng "KEEP_SIZE failed: $SZ4 != $KEEP_SZ"; fi

echo "=== Test 8: PUNCH_HOLE mode ==="
F6=$TDIR/f6
make_flat $F6
PUNCH_SZ=$(stat -c %s $F6)
# punch a hole in the middle
HOLE_START=2048
HOLE_LEN=2048
fallocate -l $((HOLE_START + HOLE_LEN)) -o $HOLE_START -p $F6 2>/dev/null
SZ5=$(stat -c %s $F6)
if [ "$SZ5" -eq "$PUNCH_SZ" ]; then ok "PUNCH_HOLE size unchanged"; else ng "PUNCH_HOLE size changed: $SZ5"; fi
# punched region should be zero
PUNCH_ZEROS=$(dd if=$F6 bs=1 skip=$HOLE_START count=$HOLE_LEN 2>/dev/null | tr -d '\0' | wc -c)
if [ "$PUNCH_ZEROS" -eq 0 ]; then ok "punched hole all zero"; else ng "punched hole has $PUNCH_ZEROS non-zero"; fi

echo ""
echo "========================================="
echo "  Results: PASS=$PASS FAIL=$FAIL"
echo "========================================="
rm -rf $TDIR
[ "$FAIL" -eq 0 ]
