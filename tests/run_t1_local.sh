#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# 本地验证 T1 测试脚本逻辑 (无需 QEMU/后端服务)
#
# 原理:
#   1. 创建本地目录 /tmp/mock_pfs 作为"挂载点"
#   2. 创建 mock powerfs_mod (让 T0 编译通过)
#   3. 创建 mock docker/pgrep/umount/rmmod (让 T1/T7 环境检查通过)
#   4. source 真实 test_t1_vfs_basic.sh (去掉 source fault_injection 和 main 调用)
#   5. 覆盖 vm() 为本地执行, 其他 fault_injection 函数 mock 为成功
#   6. 调用 main, 真实执行 T2-T6 文件操作
#
# 用法:
#   ./run_t1_local.sh              # 运行全部 T0-T7
#   ./run_t1_local.sh 2 3          # 仅运行 T2, T3
#   ./run_t1_local.sh --clean      # 清理 mock 环境

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REAL_SCRIPT="${SCRIPT_DIR}/../vm/test_t1_vfs_basic.sh"

MOCK_MNT=/tmp/mock_pfs
MOCK_MOD=/tmp/mock_powerfs_mod
MOCK_BIN=/tmp/mock_bin
TMP_SCRIPT=/tmp/test_t1_local_tmp.sh

# ============================================================
# 清理
# ============================================================
if [ "${1:-}" = "--clean" ]; then
    rm -rf "$MOCK_MNT" "$MOCK_MOD" "$MOCK_BIN" "$TMP_SCRIPT" 2>/dev/null || true
    echo "Mock 环境已清理"
    exit 0
fi

# ============================================================
# 1. 创建 mock 环境
# ============================================================
mkdir -p "$MOCK_MNT" "$MOCK_MOD" "$MOCK_BIN"

# mock Makefile (make 成功, 生成 powerfs.ko)
cat > "$MOCK_MOD/Makefile" <<'EOF'
all:
	@touch powerfs.ko
	@echo "Mock build done"
clean:
	@rm -f powerfs.ko
EOF

# mock powerfs.h (POWERFS_CHUNK_SIZE = 1MB)
cat > "$MOCK_MOD/powerfs.h" <<'EOF'
#define POWERFS_CHUNK_SIZE      (1 * 1024 * 1024)
#define POWERFS_INLINE_MAX_SIZE 8192
EOF

# mock powerfs_net.h (11 个 FileLayout FieldId 宏)
cat > "$MOCK_MOD/powerfs_net.h" <<'EOF'
#define POWERFS_NET_FLD_PLACEMENT          0xA0
#define POWERFS_NET_FLD_RELIABILITY        0xA1
#define POWERFS_NET_FLD_RELIABILITY_STATE  0xA2
#define POWERFS_NET_FLD_CHUNK_LAYOUT       0xA3
#define POWERFS_NET_FLD_STRIPE_SIZE        0xA4
#define POWERFS_NET_FLD_STRIPE_COUNT       0xA5
#define POWERFS_NET_FLD_START_VOLUME_IDX   0xA6
#define POWERFS_NET_FLD_VOLUME_IDS         0xAB
#define POWERFS_NET_FLD_START_NEEDLE_ID    0xA7
#define POWERFS_NET_FLD_CHUNK_SIZE         0xA8
#define POWERFS_NET_FLD_INLINE_DATA        0xA9
#define POWERFS_NET_FLD_INLINE_MAX_SIZE    0xAA
EOF

# mock verify_module.sh
cat > "$MOCK_MOD/verify_module.sh" <<'EOF'
#!/bin/bash
echo "[PASS] ELF format check"
echo "[PASS] Symbol table check"
echo "ALL VERIFICATION TESTS PASSED"
EOF
chmod +x "$MOCK_MOD/verify_module.sh"

# mock docker: ps 返回 3 个容器名 (让 T1-1 服务检查通过)
cat > "$MOCK_BIN/docker" <<'EOF'
#!/bin/bash
if [ "$1" = "ps" ]; then
    echo "master-1"
    echo "volume-1"
    echo "filer-1"
fi
exit 0
EOF
chmod +x "$MOCK_BIN/docker"

# mock pgrep: 总是返回假 PID (让 T1-2 QEMU 检查通过, 不触发 qemuctl deploy)
cat > "$MOCK_BIN/pgrep" <<'EOF'
#!/bin/bash
echo 12345
exit 0
EOF
chmod +x "$MOCK_BIN/pgrep"

# mock umount/rmmod: 总是成功 (让 T7 卸载检查通过)
for cmd in umount rmmod; do
    cat > "$MOCK_BIN/$cmd" <<'EOF'
#!/bin/bash
exit 0
EOF
    chmod +x "$MOCK_BIN/$cmd"
done

export PATH="$MOCK_BIN:$PATH"

# ============================================================
# 2. 准备修改后的脚本 (去掉 source fault_injection 和 main 调用)
# ============================================================
# - 去掉 source fault_injection.sh (我们会自己定义 mock 函数)
# - 去掉 main "$@" (我们要 source 后手动调用 main)
# - sleep 30 → sleep 1 (加速本地测试)
# - sleep 10 → sleep 1
# - chown 1000:1000 → 当前用户 uid:gid (非 root 用户无法 chown 到其他 uid)
CURRENT_UG="$(id -u):$(id -g)"
sed \
    -e '/^source "${SCRIPT_DIR}\/fault_injection.sh"$/d' \
    -e '/^main "\$@"$/d' \
    -e 's|sleep 30|sleep 1|g' \
    -e 's|sleep 10|sleep 1|g' \
    -e "s|1000:1000|${CURRENT_UG}|g" \
    "$REAL_SCRIPT" > "$TMP_SCRIPT"

if [ ! -s "$TMP_SCRIPT" ]; then
    echo "ERROR: 无法生成修改后的脚本, 请检查 $REAL_SCRIPT 是否存在"
    exit 1
fi

# ============================================================
# 3. source 加载测试函数定义 (此时不执行 main)
# ============================================================
# 注意: source 时会设置 MNT=/mnt/pfs, POWERFS_MOD_DIR=..., 我们随后覆盖
source "$TMP_SCRIPT"

# ============================================================
# 4. 覆盖为本地 mock 模式
# ============================================================
MNT="$MOCK_MNT"
POWERFS_MOD_DIR="$MOCK_MOD"

# vm 函数: 本地执行, 但对 lsmod/mount 返回 mock 输出
vm() {
    case "$*" in
        *lsmod*powerfs*)
            echo "powerfs 123456 1"
            return 0
            ;;
        *mount*powerfs*|*grep*powerfs*on*)
            echo "powerfs on ${MOCK_MNT} type powerfs (rw)"
            return 0
            ;;
        *)
            eval "$@" 2>&1
            return $?
            ;;
    esac
}

# 其他 fault_injection 函数 mock
vm_alive()      { return 0; }
check_mount()   { return 0; }
dmesg_line_count() { echo 0; }
dmesg_since()   { :; }
serial_line_count() { echo 0; }
serial_since()  { :; }

# ============================================================
# 5. 运行 main (传递参数, 如 "2 3" 选择性运行)
# ============================================================
echo ""
echo "============================================"
echo " 本地验证模式: test_t1_vfs_basic.sh"
echo " 挂载点: $MOCK_MNT"
echo " 模块目录: $MOCK_MOD"
echo "============================================"
echo ""

main "$@"
local ret=$?

# ============================================================
# 6. 清理临时脚本
# ============================================================
rm -f "$TMP_SCRIPT" 2>/dev/null || true

exit $ret
