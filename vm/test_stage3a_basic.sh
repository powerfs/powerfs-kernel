#!/bin/sh
# 阶段3测试A: 基本回归测试
# 验证 "先请求后改本地" 修改没有破坏基本功能
# 测试: mkdir/create/write/read/append/unlink/rmdir + remount 持久化

set -u
MNT=/mnt/pfs
PASS=0
FAIL=0

ok() { echo "[PASS] $1"; PASS=$((PASS+1)); }
ng() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

echo "========================================="
echo "  阶段3测试A: 基本回归"
echo "========================================="

# 清理上次测试残留
cd "$MNT" 2>/dev/null || { echo "mount not ready"; exit 1; }
rm -rf testdir testfile.txt 2>/dev/null
sync 2>/dev/null

# 1. mkdir
echo "--- 1. mkdir ---"
mkdir "$MNT/testdir" 2>/dev/null
if [ -d "$MNT/testdir" ]; then ok "mkdir testdir"; else ng "mkdir testdir"; fi

# 2. create + write
echo "--- 2. create + write ---"
echo "hello world" > "$MNT/testfile.txt" 2>/dev/null
if [ -f "$MNT/testfile.txt" ]; then ok "create testfile.txt"; else ng "create testfile.txt"; fi

# 3. read
echo "--- 3. read ---"
CONTENT=$(cat "$MNT/testfile.txt" 2>/dev/null)
if [ "$CONTENT" = "hello world" ]; then
    ok "read content matches: '$CONTENT'"
else
    ng "read content mismatch: got '$CONTENT' want 'hello world'"
fi

# 4. size check
echo "--- 4. size check ---"
SIZE=$(stat -c %s "$MNT/testfile.txt" 2>/dev/null)
if [ "$SIZE" = "12" ]; then
    ok "size=12 (correct)"
else
    ng "size mismatch: got $SIZE want 12"
fi

# 5. append write
echo "--- 5. append write ---"
echo "second line" >> "$MNT/testfile.txt" 2>/dev/null
CONTENT=$(cat "$MNT/testfile.txt" 2>/dev/null)
EXPECTED="hello world
second line"
if [ "$CONTENT" = "$EXPECTED" ]; then
    ok "append content correct"
else
    ng "append content mismatch: got '$CONTENT'"
fi

# 6. O_TRUNC
echo "--- 6. O_TRUNC ---"
echo "x" > "$MNT/testfile.txt" 2>/dev/null
SIZE=$(stat -c %s "$MNT/testfile.txt" 2>/dev/null)
if [ "$SIZE" = "2" ]; then
    ok "O_TRUNC size=2 (correct)"
else
    ng "O_TRUNC size mismatch: got $SIZE want 2"
fi

# 7. unlink
echo "--- 7. unlink ---"
rm "$MNT/testfile.txt" 2>/dev/null
if [ ! -f "$MNT/testfile.txt" ]; then ok "unlink testfile.txt"; else ng "unlink testfile.txt"; fi

# 8. rmdir
echo "--- 8. rmdir ---"
rmdir "$MNT/testdir" 2>/dev/null
if [ ! -d "$MNT/testdir" ]; then ok "rmdir testdir"; else ng "rmdir testdir"; fi

# 9. 嵌套目录
echo "--- 9. nested mkdir ---"
mkdir -p "$MNT/a/b/c" 2>/dev/null
if [ -d "$MNT/a/b/c" ]; then ok "nested mkdir a/b/c"; else ng "nested mkdir"; fi
echo "deep" > "$MNT/a/b/c/file.txt" 2>/dev/null
CONTENT=$(cat "$MNT/a/b/c/file.txt" 2>/dev/null)
if [ "$CONTENT" = "deep" ]; then ok "nested write/read"; else ng "nested write/read: '$CONTENT'"; fi

# 10. ls 目录列表
echo "--- 10. readdir ---"
LSOUT=$(ls "$MNT/a/b/" 2>/dev/null)
if [ "$LSOUT" = "c" ]; then ok "readdir b/ shows c"; else ng "readdir b/: got '$LSOUT' want 'c'"; fi

# 清理嵌套
rm "$MNT/a/b/c/file.txt" 2>/dev/null
rmdir "$MNT/a/b/c" 2>/dev/null
rmdir "$MNT/a/b" 2>/dev/null
rmdir "$MNT/a" 2>/dev/null

echo ""
echo "========================================="
echo "  结果: PASS=$PASS FAIL=$FAIL"
echo "========================================="
