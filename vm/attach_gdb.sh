#!/bin/bash
# PowerFS GDB 调试脚本
# 附加到 QEMU 虚拟机的 GDB 服务器端口进行内核调试

set -e

OUTPUT_DIR="/home/portion/powerfs/kernel/vm/output"
VMLINUX="${OUTPUT_DIR}/vmlinux"
GDB_PORT="1234"

# 检查 vmlinux (带调试符号的内核)
if [ ! -f "${VMLINUX}" ]; then
    echo "错误: vmlinux 不存在 ${VMLINUX}"
    echo "请先运行: ./build_kernel.sh (需要启用 CONFIG_DEBUG_INFO)"
    exit 1
fi

# 检查 GDB 是否已运行
if ! command -v gdb &> /dev/null; then
    echo "错误: GDB 未安装"
    echo "请先运行: sudo apt-get install -y gdb"
    exit 1
fi

echo "=== PowerFS GDB 调试器 ==="
echo "连接到 QEMU GDB 服务器 (端口: ${GDB_PORT})"
echo ""
echo "内核符号: ${VMLINUX}"
echo ""
echo "常用 GDB 命令:"
echo "  continue (c)          - 继续运行"
echo "  step (s)              - 单步进入"
echo "  next (n)              - 单步跳过"
echo "  break <function>      - 设置断点"
echo "  list (l)              - 查看源代码"
echo "  print <variable> (p)  - 打印变量"
echo "  backtrace (bt)        - 查看调用栈"
echo "  info threads          - 查看线程"
echo "  quit (q)              - 退出 GDB"
echo ""
echo "PowerFS 相关断点建议:"
echo "  break powerfs_init"
echo "  break powerfs_mount"
echo "  break powerfs_read"
echo "  break powerfs_write"
echo "  break powerfs_lookup"
echo ""

# 启动 GDB
gdb "${VMLINUX}" \
    -ex "target remote :${GDB_PORT}" \
    -ex "break powerfs_init" \
    -ex "break powerfs_mount" \
    -ex "echo \n使用 'continue' 开始运行\n" \
    -ex "echo 使用 'break <func>' 设置更多断点\n"
