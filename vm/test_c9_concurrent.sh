#!/bin/sh
# C9 并发删除场景测试
#
# 验证 dir_entries deleted 标记机制在并发场景下的正确性:
#   S1: 4 进程并发删除 500 文件 (分片删除)
#   S2: readdir + unlink 并发 (ls 同时 rm)
#   S3: 删除 + 重建竞争 (un-delete 路径)
#   S4: 2 进程交叉创建/删除不同文件
#   S5: 大目录 rm -rf (1000 文件, 单进程但 getdents 多次)
#   S6: 并发 rm -rf 同一目录的子目录
#
# 日志捕获: 每个场景独立清除/保存 dmesg, 避免 ring buffer 溢出丢失日志.
# 日志文件保存在 /tmp/c9_dmesg/ 目录下, 按场景命名.
#
# 运行: ssh -p 2223 root@localhost < test_c9_concurrent.sh

set -u
MNT=/mnt/pfs
BASE="$MNT/c9_concurrent"
DMESG_DIR="/tmp/c9_dmesg"
PASS=0
FAIL=0
CUR_SCENARIO=""

G='\033[0;32m'; R='\033[0;31m'; Y='\033[0;33m'; C='\033[0;36m'; N='\033[0m'

ok()   { echo -e "  ${G}[PASS]${N} $1"; PASS=$((PASS+1)); }
ng()   { echo -e "  ${R}[FAIL]${N} $1"; FAIL=$((FAIL+1)); }

# 每个场景开始时: 清除 dmesg ring buffer, 确保该场景的日志不被之前的覆盖
section() {
    CUR_SCENARIO="$1"
    echo ""
    echo -e "${C}━━━ $1 ━━━${N}"
    dmesg -c > /dev/null 2>&1
}

# 每个场景结束时: 保存 dmesg 到文件, 并检查内核异常
check_dmesg() {
    local logfile="$DMESG_DIR/$(echo "$CUR_SCENARIO" | sed 's/[^a-zA-Z0-9_]/_/g').log"
    dmesg > "$logfile" 2>/dev/null

    local lines
    lines=$(wc -l < "$logfile" 2>/dev/null) || lines=0
    local mark_del
    mark_del=$(grep -c "MARK_DELETED" "$logfile" 2>/dev/null) || mark_del=0
    local un_del
    un_del=$(grep -c "UN_DELETE" "$logfile" 2>/dev/null) || un_del=0
    local refetch
    refetch=$(grep -c "REFETCH " "$logfile" 2>/dev/null) || refetch=0
    local emit_skip
    emit_skip=$(grep -c "EMIT_SKIP" "$logfile" 2>/dev/null) || emit_skip=0
    local not_found
    not_found=$(grep -c "NOT_FOUND" "$logfile" 2>/dev/null) || not_found=0

    echo -e "  ${Y}dmesg${N}: ${lines} lines | MARK_DELETED=${mark_del} UN_DELETE=${un_del} REFETCH=${refetch} EMIT_SKIP=${emit_skip} NOT_FOUND=${not_found}"
    echo -e "  ${Y}dmesg${N}: saved to ${logfile}"

    local errors
    errors=$(grep -E 'BUG:|Oops:|KASAN:|RCU stall|hung task|soft lockup|call trace|Kernel panic|WARNING.*powerfs' "$logfile" | head -5)
    if [ -z "$errors" ]; then
        ok "no kernel anomalies"
    else
        ng "kernel anomaly: $errors"
    fi
}

# 最终汇总: 合并所有场景的日志统计
dmesg_final_summary() {
    echo ""
    echo -e "${C}━━━ dmesg Log Summary ━━━${N}"
    local total_lines=0
    local total_mark=0
    local total_undel=0
    local total_refetch=0
    local total_skip=0
    local total_notfound=0

    for f in "$DMESG_DIR"/*.log; do
        [ -f "$f" ] || continue
        local lines
        lines=$(wc -l < "$f") || lines=0
        local mark
        mark=$(grep -c "MARK_DELETED" "$f" 2>/dev/null) || mark=0
        local undel
        undel=$(grep -c "UN_DELETE" "$f" 2>/dev/null) || undel=0
        local refetch
        refetch=$(grep -c "REFETCH " "$f" 2>/dev/null) || refetch=0
        local skip
        skip=$(grep -c "EMIT_SKIP" "$f" 2>/dev/null) || skip=0
        local notfound
        notfound=$(grep -c "NOT_FOUND" "$f" 2>/dev/null) || notfound=0

        total_lines=$((total_lines + lines))
        total_mark=$((total_mark + mark))
        total_undel=$((total_undel + undel))
        total_refetch=$((total_refetch + refetch))
        total_skip=$((total_skip + skip))
        total_notfound=$((total_notfound + notfound))

        printf "  %-40s lines=%-5s MARK=%-4s UNDEL=%-3s REFETCH=%-3s SKIP=%-3s NOTFOUND=%s\n" \
            "$(basename "$f")" "$lines" "$mark" "$undel" "$refetch" "$skip" "$notfound"
    done

    echo ""
    printf "  %-40s lines=%-5s MARK=%-4s UNDEL=%-3s REFETCH=%-3s SKIP=%-3s NOTFOUND=%s\n" \
        "TOTAL" "$total_lines" "$total_mark" "$total_undel" "$total_refetch" "$total_skip" "$total_notfound"
}

# 初始化
mkdir -p "$DMESG_DIR"
rm -f "$DMESG_DIR"/*.log 2>/dev/null
# 确保 /dev/kmsg 存在 (busybox dmesg 需要它)
mknod /dev/kmsg c 1 11 2>/dev/null

rm -rf "$BASE" 2>/dev/null
mkdir -p "$BASE"

echo "=== C9 Concurrent Deletion Test ==="
echo "Start: $(date)"
echo "dmesg logs: $DMESG_DIR/"

# ============================================================
# S1: 4 进程并发删除 500 文件
# ============================================================
section "S1: 4-process concurrent delete (500 files)"

S1_DIR="$BASE/s1_concurrent"
mkdir -p "$S1_DIR"
N_S1=500

for i in $(seq 1 $N_S1); do
    echo "data_$i" > "$S1_DIR/file_$i.txt"
done
sync

ACTUAL=$(ls "$S1_DIR" 2>/dev/null | wc -l)
if [ "$ACTUAL" -eq "$N_S1" ]; then
    ok "S1a created $N_S1 files (got $ACTUAL)"
else
    ng "S1a expected $N_S1 files, got $ACTUAL"
fi

# 4 进程并发删除: 每个进程删除不同的分片
for proc in 0 1 2 3; do
    (
        offset=$((proc + 1))
        i=$offset
        while [ "$i" -le "$N_S1" ]; do
            rm -f "$S1_DIR/file_$i.txt" 2>/dev/null
            i=$((i + 4))
        done
    ) &
done
wait

sync
REMAINING=$(ls "$S1_DIR" 2>/dev/null | wc -l)
if [ "$REMAINING" -eq 0 ]; then
    ok "S1b all $N_S1 files deleted concurrently (remaining=$REMAINING)"
else
    ng "S1b expected 0 remaining, got $REMAINING"
fi

rmdir "$S1_DIR" 2>/dev/null
if [ ! -d "$S1_DIR" ]; then
    ok "S1c directory removed"
else
    ng "S1c directory still exists"
fi

check_dmesg

# ============================================================
# S2: readdir + unlink 并发
# ============================================================
section "S2: concurrent readdir + unlink (300 files)"

S2_DIR="$BASE/s2_rw_race"
mkdir -p "$S2_DIR"
N_S2=300

for i in $(seq 1 $N_S2); do
    echo "content_$i" > "$S2_DIR/f_$i.dat"
done
sync

# 后台进程: 持续 ls 目录 (触发 readdir) 约 5 秒
(
    count=0
    while [ "$count" -lt 200 ]; do
        ls "$S2_DIR" > /dev/null 2>&1
        count=$((count + 1))
    done
) &
LS_PID=$!

# 主进程: 并发删除所有文件
for i in $(seq 1 $N_S2); do
    rm -f "$S2_DIR/f_$i.dat" 2>/dev/null
done

wait $LS_PID 2>/dev/null
sync

REMAINING=$(ls "$S2_DIR" 2>/dev/null | wc -l)
if [ "$REMAINING" -eq 0 ]; then
    ok "S2a all files deleted during concurrent readdir (remaining=$REMAINING)"
else
    ng "S2a expected 0 remaining, got $REMAINING"
fi

rm -rf "$S2_DIR" 2>/dev/null
if [ ! -d "$S2_DIR" ]; then
    ok "S2b directory removed"
else
    ng "S2b directory still exists"
fi

check_dmesg

# ============================================================
# S3: 删除 + 重建竞争 (un-delete 路径)
# ============================================================
section "S3: delete + recreate race (un-delete path)"

S3_DIR="$BASE/s3_undelete"
mkdir -p "$S3_DIR"
N_S3=100

for i in $(seq 1 $N_S3); do
    echo "v1_$i" > "$S3_DIR/r_$i.txt"
done
sync

# 先 ls 填充 dir_entries (确保后续删除触发 MARK_DELETED)
ls "$S3_DIR" > /dev/null 2>&1

# 进程 A: 删除所有文件
# 进程 B: 同时重建部分文件 (触发 un-delete)
(
    for i in $(seq 1 $N_S3 2); do
        echo "v2_$i" > "$S3_DIR/r_$i.txt" 2>/dev/null
    done
) &
RECREATE_PID=$!

# 删除所有文件 (包括被重建的)
for i in $(seq 1 $N_S3); do
    rm -f "$S3_DIR/r_$i.txt" 2>/dev/null
done

wait $RECREATE_PID 2>/dev/null

# 再次删除 (清理重建的文件)
for i in $(seq 1 $N_S3 2); do
    rm -f "$S3_DIR/r_$i.txt" 2>/dev/null
done
sync

REMAINING=$(ls "$S3_DIR" 2>/dev/null | wc -l)
if [ "$REMAINING" -eq 0 ]; then
    ok "S3a all files cleaned after delete+recreate race (remaining=$REMAINING)"
else
    ng "S3a expected 0 remaining, got $REMAINING"
    ls "$S3_DIR" 2>/dev/null | head -5
fi

rm -rf "$S3_DIR" 2>/dev/null
if [ ! -d "$S3_DIR" ]; then
    ok "S3b directory removed"
else
    ng "S3b directory still exists"
fi

check_dmesg

# ============================================================
# S4: 2 进程交叉创建/删除
# ============================================================
section "S4: 2-process interleaved create/delete"

S4_DIR="$BASE/s4_interleave"
mkdir -p "$S4_DIR"

# 先创建初始文件并 ls 填充 dir_entries
for i in $(seq 1 200); do
    echo "init_$i" > "$S4_DIR/file_$i.txt"
done
sync
ls "$S4_DIR" > /dev/null 2>&1

# 进程 A: 创建奇数文件, 删除偶数文件
# 进程 B: 创建偶数文件, 删除奇数文件
(
    for i in $(seq 1 2 200); do
        echo "A_$i" > "$S4_DIR/file_$i.txt" 2>/dev/null
    done
    for i in $(seq 2 2 200); do
        rm -f "$S4_DIR/file_$i.txt" 2>/dev/null
    done
) &

(
    for i in $(seq 2 2 200); do
        echo "B_$i" > "$S4_DIR/file_$i.txt" 2>/dev/null
    done
    for i in $(seq 1 2 200); do
        rm -f "$S4_DIR/file_$i.txt" 2>/dev/null
    done
) &

wait
sync

# 最终清理: 删除所有可能残留的文件
for i in $(seq 1 200); do
    rm -f "$S4_DIR/file_$i.txt" 2>/dev/null
done
sync

REMAINING=$(ls "$S4_DIR" 2>/dev/null | wc -l)
if [ "$REMAINING" -eq 0 ]; then
    ok "S4a interleaved create/delete cleaned up (remaining=$REMAINING)"
else
    ng "S4a expected 0 remaining, got $REMAINING"
fi

rm -rf "$S4_DIR" 2>/dev/null
if [ ! -d "$S4_DIR" ]; then
    ok "S4b directory removed"
else
    ng "S4b directory still exists"
fi

check_dmesg

# ============================================================
# S5: 大目录 rm -rf (1000 文件, getdents 多次)
# ============================================================
section "S5: large dir rm -rf (1000 files)"

S5_DIR="$BASE/s5_large"
mkdir -p "$S5_DIR"
N_S5=1000

for i in $(seq 1 $N_S5); do
    echo "large_$i" > "$S5_DIR/lfile_$i.txt"
done
sync

ACTUAL=$(ls "$S5_DIR" 2>/dev/null | wc -l)
if [ "$ACTUAL" -eq "$N_S5" ]; then
    ok "S5a created $N_S5 files"
else
    ng "S5a expected $N_S5, got $ACTUAL"
fi

# rm -rf (会触发多次 getdents + 大量 unlink)
rm -rf "$S5_DIR"
sync

if [ ! -d "$S5_DIR" ]; then
    ok "S5b large directory rm -rf succeeded"
else
    ng "S5b directory still exists"
fi

check_dmesg

# ============================================================
# S6: 并发 rm -rf 同一目录的子目录
# ============================================================
section "S6: concurrent rm -rf subdirectories"

S6_DIR="$BASE/s6_multi_rmdir"
mkdir -p "$S6_DIR"

# 创建 10 个子目录, 每个子目录 50 文件
for d in $(seq 1 10); do
    mkdir -p "$S6_DIR/sub_$d"
    for f in $(seq 1 50); do
        echo "data_${d}_${f}" > "$S6_DIR/sub_$d/file_$f.txt"
    done
done
sync

# 先 ls 父目录填充 dir_entries
ls "$S6_DIR" > /dev/null 2>&1

# 10 进程并发 rm -rf 各自的子目录
for d in $(seq 1 10); do
    ( rm -rf "$S6_DIR/sub_$d" ) &
done
wait
sync

REMAINING=$(ls "$S6_DIR" 2>/dev/null | wc -l)
if [ "$REMAINING" -eq 0 ]; then
    ok "S6a all 10 subdirectories removed concurrently"
else
    ng "S6a expected 0 remaining, got $REMAINING"
fi

rm -rf "$S6_DIR" 2>/dev/null
if [ ! -d "$S6_DIR" ]; then
    ok "S6b parent directory removed"
else
    ng "S6b parent directory still exists"
fi

check_dmesg

# ============================================================
# Cleanup
# ============================================================
rm -rf "$BASE" 2>/dev/null
sync

# ============================================================
# Summary
# ============================================================
echo ""
echo "=== Summary ==="
echo "PASS: $PASS"
echo "FAIL: $FAIL"
echo "End: $(date)"

dmesg_final_summary

if [ "$FAIL" -eq 0 ]; then
    echo ""
    echo "ALL C9 CONCURRENT TESTS PASSED"
else
    echo ""
    echo "SOME TESTS FAILED"
fi
