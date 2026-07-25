#!/bin/bash
# PowerFS 并发压力测试编译脚本

set -e

echo "========================================"
echo "编译 PowerFS 并发压力测试"
echo "========================================"

# 编译选项
CC=gcc
CFLAGS="-Wall -Wextra -O2"
TARGET="concurrent_test"
SRC="concurrent_test.c"

echo "编译 $SRC ..."
$CC $CFLAGS -o $TARGET $SRC

echo "编译完成: $TARGET"
echo ""
echo "使用方法:"
echo "  ./$TARGET"