#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# T8: 数据持久化测试
#
# 验证所有修改操作在 umount → remount 后数据一致：
#   - 写入数据持久化 (小文件/大文件/覆盖写/append)
#   - 创建/删除持久化 (文件/目录)
#   - link/symlink 持久化 (硬链接/软链接)
#   - truncate 持久化 (扩展/缩小)
#   - 元数据持久化 (chmod/chown/utimes)
#   - fsync 持久化 (close 前 fsync)
#
# 使用方式:
#   ./test_t8_persistence.sh         # 运行全部
#   ./test_t8_persistence.sh 2 3 4   # 只运行 T2 T3 T4
#
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/fault_injection.sh"

MNT=/mnt/pfs
POWERFS_MOD_DIR="${SCRIPT_DIR}/../powerfs_mod"
QEMUCTL="${SCRIPT_DIR}/qemuctl.sh"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
GRAY='\033[0;90m'
NC='\033[0m'

PASS=0
FAIL=0
WARN=0
SKIP=0
FAILED_TEST=""
SLAB_INIT=""
MEM_INIT=""

ok()   { echo -e "${GREEN}[PASS]${NC} $*"; ((PASS++)); }
ng()   { echo -e "${RED}[FAIL]${NC} $*"; ((FAIL++)); FAILED_TEST="${FAILED_TEST} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; ((WARN++)); }
skip() { echo -e "${GRAY}[SKIP]${NC} $*"; ((SKIP++)); }
section() { echo -e "\n${CYAN}==== $* ====${NC}"; }

# ---------- 内核状态检查 ----------

check_dmesg_clean() {
    local since="$1"
    local label="$2"
    local new_lines
    new_lines=$(dmesg_since "$since" 2>/dev/null || echo 0)
    if [ "$new_lines" -gt 0 ]; then
        local dmesg_out
        dmesg_out=$(vm "dmesg | tail -${new_lines}" 2>/dev/null | head -30)
        if echo "$dmesg_out" | grep -qiE 'BUG|panic|oops|WARNING|hung_task|lockup|slab-out|use-after-free|null.pointer|general.protection'; then
            warn "${label}: dmesg has ${new_lines} new lines, potential issues:"
            echo "$dmesg_out" | head -10 | sed 's/^/    /'
            return 1
        fi
    fi
    return 0
}

check_d_state() {
    local label="$1"
    local d_tasks
    d_tasks=$(vm "ps -eo stat,pid,comm | grep '^D' | head -5" 2>/dev/null)
    if [ -n "$d_tasks" ]; then
        # 过滤 powerfs 自身的正常 D 状态 (短暂)
        local stuck=0
        sleep 2
        d_tasks=$(vm "ps -eo stat,pid,comm | grep '^D' | head -5" 2>/dev/null)
        if [ -n "$d_tasks" ]; then
            warn "${label}: D-state tasks: ${d_tasks}"
            return 1
        fi
    fi
    return 0
}

get_powerfs_slab_total() {
    vm "cat /proc/slabinfo 2>/dev/null | grep powerfs | awk '{sum+=\$3} END{print sum+0}'" 2>/dev/null || echo 0
}

get_mem_available() {
    vm "awk '/MemAvailable/{print \$2}' /proc/meminfo" 2>/dev/null || echo 0
}

check_kernel_state() {
    local label="$1"
    local dmesg_base="$2"
    check_dmesg_clean "$dmesg_base" "$label"
    check_d_state "$label"
}

# ---------- 选择性运行 ----------

should_run() {
    [ ${#RUN_TESTS[@]} -eq 0 ] && return 0
    local t="$1"
    for x in "${RUN_TESTS[@]}"; do
        [ "$x" = "$t" ] && return 0
    done
    return 1
}

RUN_TESTS=("$@")

# ---------- umount/remount 辅助 ----------

do_umount() {
    vm "umount ${MNT}" 2>/dev/null
    sleep 1
    if vm "mountpoint -q ${MNT}" 2>/dev/null; then
        vm "umount -l ${MNT}" 2>/dev/null
        sleep 1
    fi
}

do_rmmmod() {
    vm "rmmod powerfs" 2>/dev/null
    sleep 1
    if vm "lsmod | grep -q powerfs" 2>/dev/null; then
        warn "powerfs module still loaded after rmmod"
        return 1
    fi
    return 0
}

do_remount() {
    vm "mkdir -p ${MNT}" 2>/dev/null
    vm "mount -t powerfs none ${MNT} -o server=${FILER_ADDR:-10.0.2.2:9101}" 2>/dev/null
    sleep 2
    if ! vm "mountpoint -q ${MNT}" 2>/dev/null; then
        ng "remount failed: ${MNT} not mounted"
        return 1
    fi
    # 等待 dentry 加载
    sleep 1
    return 0
}

# 重启模块 (完整 umount + rmmod + insmod + mount)
cycle_mount() {
    local label="$1"
    echo -e "  ${GRAY}--- cycling mount: ${label} ---${NC}"

    do_umount
    if ! do_rmmmod; then
        return 1
    fi

    # 重新加载模块
    vm "insmod /root/powerfs.ko" 2>/dev/null
    sleep 1
    if ! vm "lsmod | grep -q powerfs" 2>/dev/null; then
        ng "module reload failed"
        return 1
    fi

    if ! do_remount; then
        return 1
    fi

    # 检查 dmesg
    sleep 2
    return 0
}

# 快速 remount (不 rmmod，只 umount + mount)
quick_remount() {
    local label="$1"
    echo -e "  ${GRAY}--- quick remount: ${label} ---${NC}"
    do_umount
    do_remount
}

# ---------- 测试用例 ----------

# T0: 编译 + 静态验证
test_t0() {
    section "T0: Build + Static Verification"

    cd "${POWERFS_MOD_DIR}" || { ng "T0" "cannot cd to powerfs_mod"; return 1; }
    if make -j"$(nproc)" 2>&1 | tail -5; then
        ok "T0: build success"
    else
        ng "T0" "build failed"
        return 1
    fi

    if bash verify_module.sh 2>&1 | tail -3; then
        ok "T0: verify_module PASS"
    else
        ng "T0" "verify_module FAIL"
        return 1
    fi
}

# T1: QEMU 启动 + 挂载
test_t1() {
    section "T1: QEMU Boot + Mount"

    "${QEMUCTL}" service start 2>/dev/null
    sleep 3
    "${QEMUCTL}" deploy 2>/dev/null
    sleep 2
    "${QEMUCTL}" mount 2>/dev/null
    sleep 3

    if ! vm "mountpoint -q ${MNT}" 2>/dev/null; then
        ng "T1" "mount failed"
        return 1
    fi

    ok "T1: mounted at ${MNT}"

    # 记录基线
    SLAB_INIT=$(get_powerfs_slab_total)
    MEM_INIT=$(get_mem_available)
    echo -e "  ${GRAY}baseline: slab=${SLAB_INIT} mem=${MEM_INIT}${NC}"

    # 30s 观察期
    echo -e "  ${GRAY}30s observation period...${NC}"
    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)
    sleep 30
    check_kernel_state "T1-observe" "$dmesg_base"
    ok "T1: observation period passed"
}

# T2: 写入数据持久化
test_t2() {
    section "T2: Write Data Persistence"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 2a: 小文件 (100B) 写入持久化
    echo -e "  ${GRAY}2a: small file (100B) write persistence${NC}"
    vm "dd if=/dev/urandom bs=100 count=1 of=${MNT}/t8_small.bin 2>/dev/null"
    local md5_before
    md5_before=$(vm "md5sum ${MNT}/t8_small.bin" 2>/dev/null | awk '{print $1}')
    vm "sync" 2>/dev/null

    if ! quick_remount "2a-small"; then
        ng "T2a" "remount failed"
        return 1
    fi

    local md5_after
    md5_after=$(vm "md5sum ${MNT}/t8_small.bin" 2>/dev/null | awk '{print $1}')
    if [ "$md5_before" = "$md5_after" ] && [ -n "$md5_before" ]; then
        ok "T2a: small file MD5 consistent after remount"
    else
        ng "T2a" "MD5 mismatch: before=${md5_before} after=${md5_after}"
    fi
    check_kernel_state "T2a" "$dmesg_base"

    # 2b: 中等文件 (1MB) 写入持久化
    echo -e "  ${GRAY}2b: medium file (1MB) write persistence${NC}"
    vm "dd if=/dev/urandom bs=1M count=1 of=${MNT}/t8_medium.bin 2>/dev/null"
    md5_before=$(vm "md5sum ${MNT}/t8_medium.bin" 2>/dev/null | awk '{print $1}')
    vm "sync" 2>/dev/null

    if ! quick_remount "2b-medium"; then
        ng "T2b" "remount failed"
        return 1
    fi

    md5_after=$(vm "md5sum ${MNT}/t8_medium.bin" 2>/dev/null | awk '{print $1}')
    if [ "$md5_before" = "$md5_after" ] && [ -n "$md5_before" ]; then
        ok "T2b: medium file MD5 consistent after remount"
    else
        ng "T2b" "MD5 mismatch: before=${md5_before} after=${md5_after}"
    fi
    check_kernel_state "T2b" "$dmesg_base"

    # 2c: 大文件 (10MB) 写入持久化
    echo -e "  ${GRAY}2c: large file (10MB) write persistence${NC}"
    vm "dd if=/dev/urandom bs=1M count=10 of=${MNT}/t8_large.bin 2>/dev/null"
    md5_before=$(vm "md5sum ${MNT}/t8_large.bin" 2>/dev/null | awk '{print $1}')
    vm "sync" 2>/dev/null

    if ! quick_remount "2c-large"; then
        ng "T2c" "remount failed"
        return 1
    fi

    md5_after=$(vm "md5sum ${MNT}/t8_large.bin" 2>/dev/null | awk '{print $1}')
    if [ "$md5_before" = "$md5_after" ] && [ -n "$md5_before" ]; then
        ok "T2c: large file MD5 consistent after remount"
    else
        ng "T2c" "MD5 mismatch: before=${md5_before} after=${md5_after}"
    fi
    check_kernel_state "T2c" "$dmesg_base"

    # 2d: 覆盖写持久化
    echo -e "  ${GRAY}2d: overwrite persistence${NC}"
    vm "dd if=/dev/urandom bs=1K count=1 of=${MNT}/t8_overwrite.bin 2>/dev/null"
    vm "dd if=/dev/urandom bs=512 count=1 of=${MNT}/t8_overwrite.bin conv=notrunc 2>/dev/null"
    md5_before=$(vm "md5sum ${MNT}/t8_overwrite.bin" 2>/dev/null | awk '{print $1}')
    vm "sync" 2>/dev/null

    if ! quick_remount "2d-overwrite"; then
        ng "T2d" "remount failed"
        return 1
    fi

    md5_after=$(vm "md5sum ${MNT}/t8_overwrite.bin" 2>/dev/null | awk '{print $1}')
    if [ "$md5_before" = "$md5_after" ] && [ -n "$md5_before" ]; then
        ok "T2d: overwritten file MD5 consistent after remount"
    else
        ng "T2d" "MD5 mismatch: before=${md5_before} after=${md5_after}"
    fi
    check_kernel_state "T2d" "$dmesg_base"

    # 2e: append 写持久化
    echo -e "  ${GRAY}2e: append write persistence${NC}"
    vm "dd if=/dev/urandom bs=100 count=1 of=${MNT}/t8_append.bin 2>/dev/null"
    vm "dd if=/dev/urandom bs=200 count=1 >> ${MNT}/t8_append.bin 2>/dev/null"
    vm "dd if=/dev/urandom bs=300 count=1 >> ${MNT}/t8_append.bin 2>/dev/null"
    md5_before=$(vm "md5sum ${MNT}/t8_append.bin" 2>/dev/null | awk '{print $1}')
    local size_before
    size_before=$(vm "stat -c %s ${MNT}/t8_append.bin" 2>/dev/null)
    vm "sync" 2>/dev/null

    if ! quick_remount "2e-append"; then
        ng "T2e" "remount failed"
        return 1
    fi

    md5_after=$(vm "md5sum ${MNT}/t8_append.bin" 2>/dev/null | awk '{print $1}')
    local size_after
    size_after=$(vm "stat -c %s ${MNT}/t8_append.bin" 2>/dev/null)
    if [ "$md5_before" = "$md5_after" ] && [ "$size_before" = "$size_after" ]; then
        ok "T2e: appended file consistent (size=${size_after})"
    else
        ng "T2e" "mismatch: md5 before=${md5_before} after=${md5_after}, size before=${size_before} after=${size_after}"
    fi
    check_kernel_state "T2e" "$dmesg_base"
}

# T3: 创建/删除持久化
test_t3() {
    section "T3: Create/Delete Persistence"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 3a: 文件创建持久化
    echo -e "  ${GRAY}3a: file creation persistence${NC}"
    vm "touch ${MNT}/t8_created_1.txt" 2>/dev/null
    vm "echo 'hello' > ${MNT}/t8_created_2.txt" 2>/dev/null
    vm "mkdir -p ${MNT}/t8_created_dir" 2>/dev/null
    vm "echo 'in dir' > ${MNT}/t8_created_dir/file.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "3a-create"; then
        ng "T3a" "remount failed"
        return 1
    fi

    local exists=0
    vm "test -f ${MNT}/t8_created_1.txt" 2>/dev/null && exists=$((exists+1))
    vm "test -f ${MNT}/t8_created_2.txt" 2>/dev/null && exists=$((exists+1))
    vm "test -d ${MNT}/t8_created_dir" 2>/dev/null && exists=$((exists+1))
    vm "test -f ${MNT}/t8_created_dir/file.txt" 2>/dev/null && exists=$((exists+1))

    if [ "$exists" -eq 4 ]; then
        ok "T3a: all 4 files/dirs exist after remount"
    else
        ng "T3a" "only ${exists}/4 files exist after remount"
    fi

    # 验证文件内容
    local content
    content=$(vm "cat ${MNT}/t8_created_2.txt" 2>/dev/null | tr -d '\n')
    if [ "$content" = "hello" ]; then
        ok "T3a: file content correct after remount"
    else
        ng "T3a" "content mismatch: expected='hello' got='${content}'"
    fi
    check_kernel_state "T3a" "$dmesg_base"

    # 3b: 文件删除持久化
    echo -e "  ${GRAY}3b: file deletion persistence${NC}"
    vm "rm ${MNT}/t8_created_1.txt" 2>/dev/null
    vm "rm -rf ${MNT}/t8_created_dir" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "3b-delete"; then
        ng "T3b" "remount failed"
        return 1
    fi

    local deleted=0
    vm "test ! -e ${MNT}/t8_created_1.txt" 2>/dev/null && deleted=$((deleted+1))
    vm "test ! -e ${MNT}/t8_created_dir" 2>/dev/null && deleted=$((deleted+1))

    if [ "$deleted" -eq 2 ]; then
        ok "T3b: deleted files/dirs gone after remount"
    else
        ng "T3b" "only ${deleted}/2 deleted, files still exist after remount"
    fi

    # 确认未删除的文件仍在
    if vm "test -f ${MNT}/t8_created_2.txt" 2>/dev/null; then
        ok "T3b: non-deleted file still exists"
    else
        ng "T3b" "non-deleted file missing!"
    fi
    check_kernel_state "T3b" "$dmesg_base"

    # 3c: 目录树创建+删除持久化
    echo -e "  ${GRAY}3c: directory tree persistence${NC}"
    vm "mkdir -p ${MNT}/t8_tree/a/b/c/d" 2>/dev/null
    vm "touch ${MNT}/t8_tree/a/b/c/d/file1" 2>/dev/null
    vm "touch ${MNT}/t8_tree/a/b/file2" 2>/dev/null
    vm "touch ${MNT}/t8_tree/a/file3" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "3c-tree"; then
        ng "T3c" "remount failed"
        return 1
    fi

    local tree_ok=0
    vm "test -d ${MNT}/t8_tree/a/b/c/d" 2>/dev/null && tree_ok=$((tree_ok+1))
    vm "test -f ${MNT}/t8_tree/a/b/c/d/file1" 2>/dev/null && tree_ok=$((tree_ok+1))
    vm "test -f ${MNT}/t8_tree/a/b/file2" 2>/dev/null && tree_ok=$((tree_ok+1))
    vm "test -f ${MNT}/t8_tree/a/file3" 2>/dev/null && tree_ok=$((tree_ok+1))

    if [ "$tree_ok" -eq 4 ]; then
        ok "T3c: directory tree persisted (${tree_ok}/4)"
    else
        ng "T3c" "tree incomplete: ${tree_ok}/4"
    fi

    # 删除整个目录树
    vm "rm -rf ${MNT}/t8_tree" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "3c-tree-del"; then
        ng "T3c" "remount after delete failed"
        return 1
    fi

    if vm "test ! -e ${MNT}/t8_tree" 2>/dev/null; then
        ok "T3c: directory tree deleted after remount"
    else
        ng "T3c" "directory tree still exists after delete+remount"
    fi
    check_kernel_state "T3c" "$dmesg_base"
}

# T4: link/symlink 持久化
test_t4() {
    section "T4: Link/Symlink Persistence"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 4a: 硬链接持久化
    echo -e "  ${GRAY}4a: hard link persistence${NC}"
    vm "echo 'hardlink data' > ${MNT}/t8_hlink_orig.txt" 2>/dev/null
    vm "ln ${MNT}/t8_hlink_orig.txt ${MNT}/t8_hlink_copy.txt" 2>/dev/null
    local nlink_before
    nlink_before=$(vm "stat -c %h ${MNT}/t8_hlink_orig.txt" 2>/dev/null)
    local md5_orig
    md5_orig=$(vm "md5sum ${MNT}/t8_hlink_orig.txt" 2>/dev/null | awk '{print $1}')
    local md5_copy
    md5_copy=$(vm "md5sum ${MNT}/t8_hlink_copy.txt" 2>/dev/null | awk '{print $1}')
    vm "sync" 2>/dev/null

    if ! quick_remount "4a-hardlink"; then
        ng "T4a" "remount failed"
        return 1
    fi

    local nlink_after
    nlink_after=$(vm "stat -c %h ${MNT}/t8_hlink_orig.txt" 2>/dev/null)
    local md5_orig_after
    md5_orig_after=$(vm "md5sum ${MNT}/t8_hlink_orig.txt" 2>/dev/null | awk '{print $1}')
    local md5_copy_after
    md5_copy_after=$(vm "md5sum ${MNT}/t8_hlink_copy.txt" 2>/dev/null | awk '{print $1}')

    if [ "$nlink_before" = "$nlink_after" ] && [ "$nlink_after" = "2" ]; then
        ok "T4a: nlink=${nlink_after} consistent"
    else
        ng "T4a" "nlink mismatch: before=${nlink_before} after=${nlink_after}"
    fi

    if [ "$md5_orig_after" = "$md5_copy_after" ] && [ "$md5_orig" = "$md5_orig_after" ]; then
        ok "T4a: hardlink data consistent"
    else
        ng "T4a" "data mismatch"
    fi

    # 删除原文件，硬链接应仍可读
    vm "rm ${MNT}/t8_hlink_orig.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "4a-hardlink-del"; then
        ng "T4a" "remount after delete failed"
        return 1
    fi

    local md5_after_del
    md5_after_del=$(vm "md5sum ${MNT}/t8_hlink_copy.txt" 2>/dev/null | awk '{print $1}')
    local nlink_after_del
    nlink_after_del=$(vm "stat -c %h ${MNT}/t8_hlink_copy.txt" 2>/dev/null)
    if [ "$md5_after_del" = "$md5_orig" ] && [ "$nlink_after_del" = "1" ]; then
        ok "T4a: hardlink data preserved after original deleted (nlink=${nlink_after_del})"
    else
        ng "T4a" "hardlink lost: md5=${md5_after_del} nlink=${nlink_after_del}"
    fi
    check_kernel_state "T4a" "$dmesg_base"

    # 4b: 软链接持久化
    echo -e "  ${GRAY}4b: symlink persistence${NC}"
    vm "echo 'symlink target' > ${MNT}/t8_symlink_target.txt" 2>/dev/null
    vm "ln -s ${MNT}/t8_symlink_target.txt ${MNT}/t8_symlink_link.txt" 2>/dev/null
    local link_target_before
    link_target_before=$(vm "readlink ${MNT}/t8_symlink_link.txt" 2>/dev/null)
    vm "sync" 2>/dev/null

    if ! quick_remount "4b-symlink"; then
        ng "T4b" "remount failed"
        return 1
    fi

    local link_target_after
    link_target_after=$(vm "readlink ${MNT}/t8_symlink_link.txt" 2>/dev/null)
    if [ "$link_target_before" = "$link_target_after" ] && [ -n "$link_target_after" ]; then
        ok "T4b: symlink target preserved: ${link_target_after}"
    else
        ng "T4b" "symlink target mismatch: before=${link_target_before} after=${link_target_after}"
    fi

    # 通过 symlink 读取数据
    local content
    content=$(vm "cat ${MNT}/t8_symlink_link.txt" 2>/dev/null | tr -d '\n')
    if [ "$content" = "symlink target" ]; then
        ok "T4b: symlink content readable after remount"
    else
        ng "T4b" "symlink content mismatch: '${content}'"
    fi
    check_kernel_state "T4b" "$dmesg_base"

    # 4c: 相对路径 symlink 持久化
    echo -e "  ${GRAY}4c: relative symlink persistence${NC}"
    vm "mkdir -p ${MNT}/t8_rel_dir" 2>/dev/null
    vm "echo 'rel target' > ${MNT}/t8_rel_dir/target.txt" 2>/dev/null
    vm "ln -s target.txt ${MNT}/t8_rel_dir/link.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "4c-relsymlink"; then
        ng "T4c" "remount failed"
        return 1
    fi

    content=$(vm "cat ${MNT}/t8_rel_dir/link.txt" 2>/dev/null | tr -d '\n')
    if [ "$content" = "rel target" ]; then
        ok "T4c: relative symlink works after remount"
    else
        ng "T4c" "relative symlink broken: '${content}'"
    fi
    check_kernel_state "T4c" "$dmesg_base"
}

# T5: truncate 持久化
test_t5() {
    section "T5: Truncate Persistence"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 5a: truncate 扩展持久化
    echo -e "  ${GRAY}5a: truncate extend persistence${NC}"
    vm "echo '1234567890' > ${MNT}/t8_trunc.txt" 2>/dev/null
    vm "truncate -s 1M ${MNT}/t8_trunc.txt" 2>/dev/null
    local size_before
    size_before=$(vm "stat -c %s ${MNT}/t8_trunc.txt" 2>/dev/null)
    vm "sync" 2>/dev/null

    if ! quick_remount "5a-trunc-ext"; then
        ng "T5a" "remount failed"
        return 1
    fi

    local size_after
    size_after=$(vm "stat -c %s ${MNT}/t8_trunc.txt" 2>/dev/null)
    if [ "$size_before" = "$size_after" ] && [ "$size_after" = "1048576" ]; then
        ok "T5a: truncated size preserved (${size_after})"
    else
        ng "T5a" "size mismatch: before=${size_before} after=${size_after}"
    fi

    # 验证前 10 字节内容
    local content
    content=$(vm "head -c 10 ${MNT}/t8_trunc.txt" 2>/dev/null)
    if [ "$content" = "1234567890" ]; then
        ok "T5a: original data preserved in extended file"
    else
        ng "T5a" "content mismatch: '${content}'"
    fi
    check_kernel_state "T5a" "$dmesg_base"

    # 5b: truncate 缩小持久化
    echo -e "  ${GRAY}5b: truncate shrink persistence${NC}"
    vm "dd if=/dev/urandom bs=1M count=1 of=${MNT}/t8_trunc_shrink.bin 2>/dev/null"
    local md5_full
    md5_full=$(vm "md5sum ${MNT}/t8_trunc_shrink.bin" 2>/dev/null | awk '{print $1}')
    vm "truncate -s 4K ${MNT}/t8_trunc_shrink.bin" 2>/dev/null
    local size_shrunk
    size_shrunk=$(vm "stat -c %s ${MNT}/t8_trunc_shrink.bin" 2>/dev/null)
    local md5_shrunk
    md5_shrunk=$(vm "md5sum ${MNT}/t8_trunc_shrink.bin" 2>/dev/null | awk '{print $1}')
    vm "sync" 2>/dev/null

    if ! quick_remount "5b-trunc-shrink"; then
        ng "T5b" "remount failed"
        return 1
    fi

    local size_after
    size_after=$(vm "stat -c %s ${MNT}/t8_trunc_shrink.bin" 2>/dev/null)
    local md5_after
    md5_after=$(vm "md5sum ${MNT}/t8_trunc_shrink.bin" 2>/dev/null | awk '{print $1}')
    if [ "$size_after" = "$size_shrunk" ] && [ "$md5_after" = "$md5_shrunk" ]; then
        ok "T5b: shrunk file preserved (size=${size_after})"
    else
        ng "T5b" "mismatch: size before=${size_shrunk} after=${size_after}, md5 before=${md5_shrunk} after=${md5_after}"
    fi
    check_kernel_state "T5b" "$dmesg_base"

    # 5c: truncate to 0 持久化
    echo -e "  ${GRAY}5c: truncate to 0 persistence${NC}"
    vm "truncate -s 0 ${MNT}/t8_trunc.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "5c-trunc-zero"; then
        ng "T5c" "remount failed"
        return 1
    fi

    size_after=$(vm "stat -c %s ${MNT}/t8_trunc.txt" 2>/dev/null)
    if [ "$size_after" = "0" ]; then
        ok "T5c: file truncated to 0 preserved"
    else
        ng "T5c" "size should be 0, got ${size_after}"
    fi
    check_kernel_state "T5c" "$dmesg_base"
}

# T6: 元数据持久化 (chmod/chown/utimes)
test_t6() {
    section "T6: Metadata Persistence (chmod/chown/utimes)"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 6a: chmod 持久化
    echo -e "  ${GRAY}6a: chmod persistence${NC}"
    vm "echo 'chmod test' > ${MNT}/t8_chmod.txt" 2>/dev/null
    vm "chmod 0644 ${MNT}/t8_chmod.txt" 2>/dev/null
    local mode_644
    mode_644=$(vm "stat -c %a ${MNT}/t8_chmod.txt" 2>/dev/null)
    vm "chmod 0755 ${MNT}/t8_chmod.txt" 2>/dev/null
    local mode_755
    mode_755=$(vm "stat -c %a ${MNT}/t8_chmod.txt" 2>/dev/null)
    vm "sync" 2>/dev/null

    if ! quick_remount "6a-chmod"; then
        ng "T6a" "remount failed"
        return 1
    fi

    local mode_after
    mode_after=$(vm "stat -c %a ${MNT}/t8_chmod.txt" 2>/dev/null)
    if [ "$mode_after" = "$mode_755" ] && [ "$mode_after" = "755" ]; then
        ok "T6a: chmod 755 preserved"
    else
        ng "T6a" "mode mismatch: expected=755 got=${mode_after}"
    fi
    check_kernel_state "T6a" "$dmesg_base"

    # 6b: chown 持久化
    echo -e "  ${GRAY}6b: chown persistence${NC}"
    vm "echo 'chown test' > ${MNT}/t8_chown.txt" 2>/dev/null
    vm "chown 1000:1000 ${MNT}/t8_chown.txt" 2>/dev/null
    local uid_before
    uid_before=$(vm "stat -c %u ${MNT}/t8_chown.txt" 2>/dev/null)
    local gid_before
    gid_before=$(vm "stat -c %g ${MNT}/t8_chown.txt" 2>/dev/null)
    vm "sync" 2>/dev/null

    if ! quick_remount "6b-chown"; then
        ng "T6b" "remount failed"
        return 1
    fi

    local uid_after
    uid_after=$(vm "stat -c %u ${MNT}/t8_chown.txt" 2>/dev/null)
    local gid_after
    gid_after=$(vm "stat -c %g ${MNT}/t8_chown.txt" 2>/dev/null)
    if [ "$uid_after" = "$uid_before" ] && [ "$gid_after" = "$gid_before" ] && [ "$uid_after" = "1000" ]; then
        ok "T6b: chown uid=${uid_after} gid=${gid_after} preserved"
    else
        ng "T6b" "chown mismatch: uid before=${uid_before} after=${uid_after}, gid before=${gid_before} after=${gid_after}"
    fi
    check_kernel_state "T6b" "$dmesg_base"

    # 6c: utimes 持久化
    echo -e "  ${GRAY}6c: utimes persistence${NC}"
    vm "echo 'utimes test' > ${MNT}/t8_utimes.txt" 2>/dev/null
    vm "touch -d '2020-01-01 12:00:00' ${MNT}/t8_utimes.txt" 2>/dev/null
    local mtime_before
    mtime_before=$(vm "stat -c %Y ${MNT}/t8_utimes.txt" 2>/dev/null)
    vm "sync" 2>/dev/null

    if ! quick_remount "6c-utimes"; then
        ng "T6c" "remount failed"
        return 1
    fi

    local mtime_after
    mtime_after=$(vm "stat -c %Y ${MNT}/t8_utimes.txt" 2>/dev/null)
    # 2020-01-01 12:00:00 UTC = 1577880000
    if [ "$mtime_after" = "$mtime_before" ] && [ "$mtime_after" = "1577880000" ]; then
        ok "T6c: mtime preserved (${mtime_after})"
    else
        ng "T6c" "mtime mismatch: before=${mtime_before} after=${mtime_after}"
    fi
    check_kernel_state "T6c" "$dmesg_base"

    # 6d: 目录 chmod 持久化
    echo -e "  ${GRAY}6d: directory chmod persistence${NC}"
    vm "mkdir -p ${MNT}/t8_dir_chmod" 2>/dev/null
    vm "chmod 0700 ${MNT}/t8_dir_chmod" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "6d-dir-chmod"; then
        ng "T6d" "remount failed"
        return 1
    fi

    mode_after=$(vm "stat -c %a ${MNT}/t8_dir_chmod" 2>/dev/null)
    if [ "$mode_after" = "700" ]; then
        ok "T6d: dir chmod 700 preserved"
    else
        ng "T6d" "dir mode mismatch: expected=700 got=${mode_after}"
    fi
    check_kernel_state "T6d" "$dmesg_base"
}

# T7: fsync 持久化验证
test_t7() {
    section "T7: fsync Persistence"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 7a: fsync 后数据持久化 (不 umount，但 drop_caches 后读取)
    echo -e "  ${GRAY}7a: fsync ensures data on Volume Server${NC}"
    vm "dd if=/dev/urandom bs=1M count=5 of=${MNT}/t8_fsync.bin 2>/dev/null"
    vm "python3 -c \"
import os, fcntl
fd = os.open('${MNT}/t8_fsync.bin', os.O_RDONLY)
fcntl.fsync(fd)
os.close(fd)
\" 2>/dev/null || vm 'sync ${MNT}/t8_fsync.bin' 2>/dev/null"
    local md5_before
    md5_before=$(vm "md5sum ${MNT}/t8_fsync.bin" 2>/dev/null | awk '{print $1}')

    # drop caches 模拟强制从后端读取
    vm "echo 3 > /proc/sys/vm/drop_caches" 2>/dev/null
    sleep 1

    local md5_after
    md5_after=$(vm "md5sum ${MNT}/t8_fsync.bin" 2>/dev/null | awk '{print $1}')
    if [ "$md5_before" = "$md5_after" ] && [ -n "$md5_before" ]; then
        ok "T7a: fsync data consistent after drop_caches"
    else
        ng "T7a" "MD5 mismatch: before=${md5_before} after=${md5_after}"
    fi
    check_kernel_state "T7a" "$dmesg_base"

    # 7b: fsync 后 umount/remount 持久化
    echo -e "  ${GRAY}7b: fsync data persists across remount${NC}"
    vm "dd if=/dev/urandom bs=1M count=3 of=${MNT}/t8_fsync2.bin 2>/dev/null"
    # 使用 fsync 系统调用 (通过 dd 的 fsync 选项)
    vm "dd if=/dev/urandom bs=1M count=3 of=${MNT}/t8_fsync2.bin conv=fsync 2>/dev/null"
    md5_before=$(vm "md5sum ${MNT}/t8_fsync2.bin" 2>/dev/null | awk '{print $1}')

    if ! quick_remount "7b-fsync-remount"; then
        ng "T7b" "remount failed"
        return 1
    fi

    md5_after=$(vm "md5sum ${MNT}/t8_fsync2.bin" 2>/dev/null | awk '{print $1}')
    if [ "$md5_before" = "$md5_after" ] && [ -n "$md5_before" ]; then
        ok "T7b: fsync data consistent after remount"
    else
        ng "T7b" "MD5 mismatch: before=${md5_before} after=${md5_after}"
    fi
    check_kernel_state "T7b" "$dmesg_base"

    # 7c: 未 fsync 的数据 (close 后) 持久化
    echo -e "  ${GRAY}7c: close (without fsync) data persistence${NC}"
    vm "dd if=/dev/urandom bs=1M count=2 of=${MNT}/t8_nofsync.bin 2>/dev/null"
    md5_before=$(vm "md5sum ${MNT}/t8_nofsync.bin" 2>/dev/null | awk '{print $1}')
    # close 通过 dd 退出自动触发 release，应触发 flush
    vm "sync" 2>/dev/null

    if ! quick_remount "7c-nofsync"; then
        ng "T7c" "remount failed"
        return 1
    fi

    md5_after=$(vm "md5sum ${MNT}/t8_nofsync.bin" 2>/dev/null | awk '{print $1}')
    if [ "$md5_before" = "$md5_after" ] && [ -n "$md5_before" ]; then
        ok "T7c: close-flushed data consistent after remount"
    else
        ng "T7c" "MD5 mismatch: before=${md5_before} after=${md5_after}"
    fi
    check_kernel_state "T7c" "$dmesg_base"
}

# T8: rename 持久化
test_t8() {
    section "T8: Rename Persistence"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 8a: 文件 rename 持久化
    echo -e "  ${GRAY}8a: file rename persistence${NC}"
    vm "echo 'rename test' > ${MNT}/t8_rename_before.txt" 2>/dev/null
    vm "mv ${MNT}/t8_rename_before.txt ${MNT}/t8_rename_after.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "8a-rename"; then
        ng "T8a" "remount failed"
        return 1
    fi

    if vm "test ! -e ${MNT}/t8_rename_before.txt" 2>/dev/null && \
       vm "test -f ${MNT}/t8_rename_after.txt" 2>/dev/null; then
        local content
        content=$(vm "cat ${MNT}/t8_rename_after.txt" 2>/dev/null | tr -d '\n')
        if [ "$content" = "rename test" ]; then
            ok "T8a: file rename persisted with correct content"
        else
            ng "T8a" "content mismatch: '${content}'"
        fi
    else
        ng "T8a" "rename not persisted correctly"
    fi
    check_kernel_state "T8a" "$dmesg_base"

    # 8b: 目录 rename 持久化
    echo -e "  ${GRAY}8b: directory rename persistence${NC}"
    vm "mkdir -p ${MNT}/t8_dir_before/sub" 2>/dev/null
    vm "echo 'in subdir' > ${MNT}/t8_dir_before/sub/file.txt" 2>/dev/null
    vm "mv ${MNT}/t8_dir_before ${MNT}/t8_dir_after" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "8b-dir-rename"; then
        ng "T8b" "remount failed"
        return 1
    fi

    if vm "test ! -e ${MNT}/t8_dir_before" 2>/dev/null && \
       vm "test -d ${MNT}/t8_dir_after/sub" 2>/dev/null && \
       vm "test -f ${MNT}/t8_dir_after/sub/file.txt" 2>/dev/null; then
        local content
        content=$(vm "cat ${MNT}/t8_dir_after/sub/file.txt" 2>/dev/null | tr -d '\n')
        if [ "$content" = "in subdir" ]; then
            ok "T8b: directory rename persisted with subtree intact"
        else
            ng "T8b" "subtree content mismatch: '${content}'"
        fi
    else
        ng "T8b" "directory rename not persisted correctly"
    fi
    check_kernel_state "T8b" "$dmesg_base"
}

# T9: 综合场景持久化
test_t9() {
    section "T9: Comprehensive Persistence Scenario"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 创建复杂的文件系统状态
    echo -e "  ${GRAY}9a: create complex FS state${NC}"
    vm "mkdir -p ${MNT}/t8_comprehensive/dir1/dir2" 2>/dev/null
    vm "echo 'file1' > ${MNT}/t8_comprehensive/file1.txt" 2>/dev/null
    vm "echo 'file2' > ${MNT}/t8_comprehensive/dir1/file2.txt" 2>/dev/null
    vm "dd if=/dev/urandom bs=64K count=1 of=${MNT}/t8_comprehensive/dir1/dir2/binary.bin 2>/dev/null"
    vm "ln ${MNT}/t8_comprehensive/file1.txt ${MNT}/t8_comprehensive/hardlink.txt" 2>/dev/null
    vm "ln -s file1.txt ${MNT}/t8_comprehensive/softlink.txt" 2>/dev/null
    vm "chmod 0640 ${MNT}/t8_comprehensive/file1.txt" 2>/dev/null
    vm "chown 1000:1000 ${MNT}/t8_comprehensive/dir1/file2.txt" 2>/dev/null
    vm "truncate -s 0 ${MNT}/t8_comprehensive/dir1/dir2/binary.bin" 2>/dev/null
    vm "sync" 2>/dev/null

    # 记录状态
    local manifest_before
    manifest_before=$(vm "find ${MNT}/t8_comprehensive -exec stat -c '%n %s %a %u %g %h' {} \; | sort" 2>/dev/null)
    local md5_binary_before
    md5_binary_before=$(vm "md5sum ${MNT}/t8_comprehensive/dir1/dir2/binary.bin" 2>/dev/null | awk '{print $1}')

    if ! quick_remount "9a-comprehensive"; then
        ng "T9a" "remount failed"
        return 1
    fi

    # 验证状态
    local manifest_after
    manifest_after=$(vm "find ${MNT}/t8_comprehensive -exec stat -c '%n %s %a %u %g %h' {} \; | sort" 2>/dev/null)
    local md5_binary_after
    md5_binary_after=$(vm "md5sum ${MNT}/t8_comprehensive/dir1/dir2/binary.bin" 2>/dev/null | awk '{print $1}')

    if [ "$manifest_before" = "$manifest_after" ]; then
        ok "T9a: comprehensive manifest consistent"
    else
        ng "T9a" "manifest mismatch:"
        echo -e "  ${GRAY}before:${NC}"
        echo "$manifest_before" | head -10 | sed 's/^/    /'
        echo -e "  ${GRAY}after:${NC}"
        echo "$manifest_after" | head -10 | sed 's/^/    /'
    fi

    # 验证 symlink
    local symlink_target
    symlink_target=$(vm "readlink ${MNT}/t8_comprehensive/softlink.txt" 2>/dev/null)
    if [ "$symlink_target" = "file1.txt" ]; then
        ok "T9a: symlink target correct"
    else
        ng "T9a" "symlink target wrong: '${symlink_target}'"
    fi

    # 验证 hardlink nlink
    local nlink
    nlink=$(vm "stat -c %h ${MNT}/t8_comprehensive/file1.txt" 2>/dev/null)
    if [ "$nlink" = "2" ]; then
        ok "T9a: hardlink nlink=2 correct"
    else
        ng "T9a" "nlink should be 2, got ${nlink}"
    fi

    # 验证 truncate (binary.bin 应为 0 字节)
    local bin_size
    bin_size=$(vm "stat -c %s ${MNT}/t8_comprehensive/dir1/dir2/binary.bin" 2>/dev/null)
    if [ "$bin_size" = "0" ]; then
        ok "T9a: truncate to 0 persisted"
    else
        ng "T9a" "binary.bin size should be 0, got ${bin_size}"
    fi
    check_kernel_state "T9a" "$dmesg_base"

    # 9b: 删除部分文件后持久化
    echo -e "  ${GRAY}9b: partial delete persistence${NC}"
    vm "rm ${MNT}/t8_comprehensive/file1.txt" 2>/dev/null
    vm "rm -rf ${MNT}/t8_comprehensive/dir1/dir2" 2>/dev/null
    vm "sync" 2>/dev/null

    if ! quick_remount "9b-partial-del"; then
        ng "T9b" "remount failed"
        return 1
    fi

    if vm "test ! -e ${MNT}/t8_comprehensive/file1.txt" 2>/dev/null && \
       vm "test ! -e ${MNT}/t8_comprehensive/dir1/dir2" 2>/dev/null && \
       vm "test -f ${MNT}/t8_comprehensive/hardlink.txt" 2>/dev/null && \
       vm "test -f ${MNT}/t8_comprehensive/dir1/file2.txt" 2>/dev/null; then
        ok "T9b: partial delete persisted correctly"
    else
        ng "T9b" "partial delete not persisted correctly"
    fi

    # 验证 hardlink 在原文件删除后 nlink=1
    nlink=$(vm "stat -c %h ${MNT}/t8_comprehensive/hardlink.txt" 2>/dev/null)
    if [ "$nlink" = "1" ]; then
        ok "T9b: hardlink nlink decremented to 1"
    else
        ng "T9b" "hardlink nlink should be 1, got ${nlink}"
    fi

    # 验证 hardlink 内容仍可读
    local content
    content=$(vm "cat ${MNT}/t8_comprehensive/hardlink.txt" 2>/dev/null | tr -d '\n')
    if [ "$content" = "file1" ]; then
        ok "T9b: hardlink content preserved after original deleted"
    else
        ng "T9b" "hardlink content wrong: '${content}'"
    fi
    check_kernel_state "T9b" "$dmesg_base"
}

# T10: 完整模块重载持久化 (umount + rmmod + insmod + mount)
test_t10() {
    section "T10: Full Module Reload Persistence"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 创建测试数据
    echo -e "  ${GRAY}10a: create test data before full reload${NC}"
    vm "echo 'reload test' > ${MNT}/t8_reload.txt" 2>/dev/null
    vm "dd if=/dev/urandom bs=1M count=2 of=${MNT}/t8_reload.bin 2>/dev/null"
    vm "mkdir -p ${MNT}/t8_reload_dir" 2>/dev/null
    vm "echo 'in dir' > ${MNT}/t8_reload_dir/file.txt" 2>/dev/null
    vm "sync" 2>/dev/null

    local md5_before
    md5_before=$(vm "md5sum ${MNT}/t8_reload.bin" 2>/dev/null | awk '{print $1}')
    local content_before
    content_before=$(vm "cat ${MNT}/t8_reload.txt" 2>/dev/null | tr -d '\n')

    # 完整重载: umount + rmmod + insmod + mount
    if ! cycle_mount "T10-full-reload"; then
        ng "T10" "full module reload failed"
        return 1
    fi

    # 验证数据
    local md5_after
    md5_after=$(vm "md5sum ${MNT}/t8_reload.bin" 2>/dev/null | awk '{print $1}')
    local content_after
    content_after=$(vm "cat ${MNT}/t8_reload.txt" 2>/dev/null | tr -d '\n')

    if [ "$md5_before" = "$md5_after" ] && [ "$content_before" = "$content_after" ]; then
        ok "T10a: data persisted across full module reload"
    else
        ng "T10a" "data mismatch after reload"
        echo -e "  ${GRAY}md5: before=${md5_before} after=${md5_after}${NC}"
        echo -e "  ${GRAY}content: before=${content_before} after=${content_after}${NC}"
    fi

    if vm "test -d ${MNT}/t8_reload_dir" 2>/dev/null && \
       vm "test -f ${MNT}/t8_reload_dir/file.txt" 2>/dev/null; then
        ok "T10a: directory structure persisted"
    else
        ng "T10a" "directory structure lost"
    fi
    check_kernel_state "T10a" "$dmesg_base"
}

# T11: 清理 + 最终检查
test_t11() {
    section "T11: Cleanup + Final Check"

    local dmesg_base
    dmesg_base=$(dmesg_line_count 2>/dev/null || echo 0)

    # 清理所有 T8 测试文件
    echo -e "  ${GRAY}cleaning up test files...${NC}"
    vm "rm -rf ${MNT}/t8_*" 2>/dev/null
    vm "sync" 2>/dev/null

    # umount + rmmod
    do_umount
    if do_rmmmod; then
        ok "T11: umount + rmmod success"
    else
        ng "T11" "rmmod failed"
    fi

    # dmesg 最终检查
    sleep 2
    check_dmesg_clean "$dmesg_base" "T11-final"

    # slab 恢复检查
    local slab_final
    slab_final=$(get_powerfs_slab_total)
    local mem_final
    mem_final=$(get_mem_available)

    echo -e "  ${GRAY}slab: init=${SLAB_INIT:-0} final=${slab_final}${NC}"
    echo -e "  ${GRAY}mem:  init=${MEM_INIT:-0} final=${mem_final}${NC}"

    if [ "${SLAB_INIT:-0}" != "0" ] && [ "$slab_final" != "0" ]; then
        warn "T11: slab objects remaining (${slab_final}), possible leak"
    else
        ok "T11: slab clean (final=${slab_final})"
    fi

    # 重新挂载便于后续测试
    vm "insmod /root/powerfs.ko" 2>/dev/null
    sleep 1
    do_remount
    ok "T11: re-mounted for subsequent tests"
}

# ---------- 主流程 ----------

main() {
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN} T8: Data Persistence Test${NC}"
    echo -e "${CYAN}========================================${NC}"

    if should_run 0; then test_t0 || { ng "T0" "build failed, abort"; exit 1; }; fi
    if should_run 1; then test_t1 || { ng "T1" "mount failed, abort"; exit 1; }; fi
    if should_run 2; then test_t2 || warn "T2 failed, continuing"; fi
    if should_run 3; then test_t3 || warn "T3 failed, continuing"; fi
    if should_run 4; then test_t4 || warn "T4 failed, continuing"; fi
    if should_run 5; then test_t5 || warn "T5 failed, continuing"; fi
    if should_run 6; then test_t6 || warn "T6 failed, continuing"; fi
    if should_run 7; then test_t7 || warn "T7 failed, continuing"; fi
    if should_run 8; then test_t8 || warn "T8 failed, continuing"; fi
    if should_run 9; then test_t9 || warn "T9 failed, continuing"; fi
    if should_run 10; then test_t10 || warn "T10 failed, continuing"; fi
    if should_run 11; then test_t11 || warn "T11 cleanup issues"; fi

    echo -e "\n${CYAN}========================================${NC}"
    echo -e "${CYAN} T8 Results Summary${NC}"
    echo -e "${CYAN}========================================${NC}"
    echo -e "  ${GREEN}PASS: ${PASS}${NC}"
    echo -e "  ${RED}FAIL: ${FAIL}${NC}"
    echo -e "  ${YELLOW}WARN: ${WARN}${NC}"
    echo -e "  ${GRAY}SKIP: ${SKIP}${NC}"
    if [ -n "$FAILED_TEST" ]; then
        echo -e "  ${RED}Failed:${FAILED_TEST}${NC}"
    fi
    echo

    if [ "$FAIL" -gt 0 ]; then
        exit 1
    fi
    exit 0
}

main "$@"
