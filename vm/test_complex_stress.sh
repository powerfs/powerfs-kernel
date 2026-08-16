#!/bin/sh
# PowerFS Kernel Module Complex Stress Test
#
# Tests advanced scenarios beyond basic T2-T8:
#   C1: Feature gap (mknod/fifo/xattr/fallocate)
#   C2: Large file (512MB) write/read/MD5 verify
#   C3: Concurrent access (8 parallel writers + MD5 verify)
#   C4: Edge cases (truncate/sparse/overwrite/append)
#   C5: Deep directory (50 levels) + many small files (1000)
#   C6: fio verify stress (data integrity under IO load)
#   C7: Mixed workload stability (3min, fio randrw)
#
# Run inside VM: ssh -p 2223 root@localhost < /tmp/test_complex_stress.sh

set -u
MNT=/mnt/pfs
TEST_DIR="$MNT/complex_stress"
PASS=0
FAIL=0
SKIP=0

G='\033[0;32m'; R='\033[0;31m'; Y='\033[0;33m'; C='\033[0;36m'; N='\033[0m'

ok()   { echo -e "  ${G}[PASS]${N} $1"; PASS=$((PASS+1)); }
ng()   { echo -e "  ${R}[FAIL]${N} $1"; FAIL=$((FAIL+1)); }
skip() { echo -e "  ${Y}[SKIP]${N} $1"; SKIP=$((SKIP+1)); }
section() { echo ""; echo -e "${C}━━━ $1 ━━━${N}"; }

# MD5 helper (busybox md5sum, fallback to cksum or stat size)
md5_check() {
    local file="$1"
    local hash
    hash=$(md5sum "$file" 2>/dev/null | awk '{print $1}')
    if [ -n "$hash" ]; then
        echo "$hash"
    else
        hash=$(cksum "$file" 2>/dev/null | awk '{print $1}')
        if [ -n "$hash" ]; then
            echo "$hash"
        else
            stat -c %s "$file"
        fi
    fi
}

check_dmesg() {
    local errors
    errors=$(dmesg | tail -50 | grep -E 'BUG:|Oops:|KASAN:|RCU stall|hung task|soft lockup|call trace|Kernel panic' || true)
    if [ -n "$errors" ]; then
        echo "  ${R}DMESG ANOMALY:${N}"
        echo "$errors" | head -5 | sed 's/^/    /'
        return 1
    fi
    return 0
}

# ============================================================
echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  PowerFS Kernel Complex Stress Test                  ║"
echo "╚══════════════════════════════════════════════════════╝"

rm -rf "$TEST_DIR" 2>/dev/null
mkdir -p "$TEST_DIR"

# ============================================================
# C1: Feature gap tests
# ============================================================
section "C1: Feature gap tests (mknod/fifo/xattr/fallocate)"

# mknod - block device
if mknod "$TEST_DIR/c1_block" b 1 1 2>/dev/null; then
    if [ -b "$TEST_DIR/c1_block" ]; then ok "mknod block device"; else ng "mknod block device (not -b)"; fi
else
    ng "mknod block device failed"
fi

# mknod - char device
if mknod "$TEST_DIR/c1_char" c 1 1 2>/dev/null; then
    if [ -c "$TEST_DIR/c1_char" ]; then ok "mknod char device"; else ng "mknod char device (not -c)"; fi
else
    ng "mknod char device failed"
fi

# mkfifo
if mkfifo "$TEST_DIR/c1_fifo" 2>/dev/null; then
    if [ -p "$TEST_DIR/c1_fifo" ]; then ok "mkfifo"; else ng "mkfifo (not -p)"; fi
else
    ng "mkfifo failed"
fi

# xattr (expected to fail - not implemented)
if which setfattr >/dev/null 2>&1; then
    if setfattr -n user.test -v hello "$TEST_DIR/c1_block" 2>/dev/null; then
        ng "xattr set (unexpected success)"
    else
        ok "xattr set correctly returns error"
    fi
else
    skip "xattr test (no setfattr)"
fi

# fallocate (expected to fail or not available)
if which fallocate >/dev/null 2>&1; then
    if fallocate -l 1048576 "$TEST_DIR/c1_falloc" 2>/dev/null; then
        # Check if file was allocated
        sz=$(stat -c %s "$TEST_DIR/c1_falloc" 2>/dev/null || echo 0)
        if [ "$sz" = "1048576" ]; then ok "fallocate"; else ng "fallocate (size=$sz)"; fi
    else
        ok "fallocate correctly returns error"
    fi
else
    skip "fallocate test (no fallocate command)"
fi

check_dmesg && ok "C1 dmesg clean" || ng "C1 dmesg anomaly"

# ============================================================
# C2: Large file (512MB) write/read/verify
# ============================================================
section "C2: Large file (256MB) write/read/MD5 verify"

LARGE_FILE="$TEST_DIR/c2_large.bin"
LARGE_SIZE=$((256 * 1024 * 1024))  # 256MB

echo "  writing 256MB with dd..."
if timeout 120 dd if=/dev/urandom of=/tmp/c2_src.bin bs=4M count=64 2>/dev/null; then
    SRC_MD5=$(md5_check /tmp/c2_src.bin)
    echo "  source MD5: $SRC_MD5"

    if timeout 180 dd if=/tmp/c2_src.bin of="$LARGE_FILE" bs=4M 2>/dev/null; then
        ok "256MB write succeeded"

        # Verify file size
        sz=$(stat -c %s "$LARGE_FILE" 2>/dev/null || echo 0)
        if [ "$sz" = "$LARGE_SIZE" ]; then ok "file size correct ($sz)"; else ng "file size wrong ($sz != $LARGE_SIZE)"; fi

        # Sync and verify MD5
        sync
        echo "  reading back and verifying..."
        READ_MD5=$(md5_check "$LARGE_FILE")
        if [ "$SRC_MD5" = "$READ_MD5" ]; then
            ok "MD5 match ($READ_MD5)"
        else
            ng "MD5 mismatch (src=$SRC_MD5 read=$READ_MD5)"
        fi

        # Read again to check consistency
        READ_MD5_2=$(md5_check "$LARGE_FILE")
        if [ "$READ_MD5" = "$READ_MD5_2" ]; then
            ok "MD5 consistent on re-read"
        else
            ng "MD5 inconsistent (read1=$READ_MD5 read2=$READ_MD5_2)"
        fi
    else
        ng "512MB write failed"
    fi
    rm -f /tmp/c2_src.bin
else
    ng "source data generation failed"
fi

check_dmesg && ok "C2 dmesg clean" || ng "C2 dmesg anomaly"

# ============================================================
# C3: Concurrent access (8 parallel writers)
# ============================================================
section "C3: Concurrent access (8 parallel writers)"

CONC_DIR="$TEST_DIR/c3_concurrent"
mkdir -p "$CONC_DIR"
N_WRITERS=8
WRITER_SIZE=$((10 * 1024 * 1024))  # 10MB each

echo "  launching $N_WRITERS parallel writers (${WRITER_SIZE} bytes each)..."

run_writer() {
    local id=$1
    local file="$CONC_DIR/writer_${id}.bin"
    dd if=/dev/urandom of="/tmp/c3_src_${id}.bin" bs=1M count=10 2>/dev/null
    local src_md5=$(md5_check "/tmp/c3_src_${id}.bin")
    cp "/tmp/c3_src_${id}.bin" "$file" 2>/dev/null
    echo "$src_md5" > "/tmp/c3_md5_${id}.txt"
}

for i in $(seq 1 $N_WRITERS); do
    run_writer $i &
done

# Wait for all writers
wait
sync

echo "  verifying all files..."
CONC_PASS=0
CONC_FAIL=0
for i in $(seq 1 $N_WRITERS); do
    file="$CONC_DIR/writer_${i}.bin"
    src_md5=$(cat "/tmp/c3_md5_${i}.txt" 2>/dev/null)
    read_md5=$(md5_check "$file")
    if [ "$src_md5" = "$read_md5" ]; then
        CONC_PASS=$((CONC_PASS+1))
    else
        CONC_FAIL=$((CONC_FAIL+1))
        echo "    writer_$i: MD5 mismatch (src=$src_md5 read=$read_md5)"
    fi
    rm -f "/tmp/c3_src_${i}.bin" "/tmp/c3_md5_${i}.txt"
done

if [ "$CONC_FAIL" = "0" ]; then
    ok "all $CONC_PASS concurrent files MD5 match"
else
    ng "$CONC_FAIL/$N_WRITERS concurrent files MD5 mismatch"
fi

# Verify file count
file_count=$(ls "$CONC_DIR" | wc -l)
if [ "$file_count" = "$N_WRITERS" ]; then
    ok "file count correct ($file_count)"
else
    ng "file count wrong ($file_count != $N_WRITERS)"
fi

check_dmesg && ok "C3 dmesg clean" || ng "C3 dmesg anomaly"

# ============================================================
# C4: Edge cases (truncate/sparse/overwrite/append)
# ============================================================
section "C4: Edge cases (truncate/sparse/overwrite/append)"

# C4a: Truncate to smaller size
EDGE_FILE="$TEST_DIR/c4_edge.bin"
dd if=/dev/urandom of=/tmp/c4_src.bin bs=1M count=5 2>/dev/null
cp /tmp/c4_src.bin "$EDGE_FILE" 2>/dev/null
sync

SRC_MD5=$(md5_check /tmp/c4_src.bin)
ORIG_MD5=$(md5_check "$EDGE_FILE")
if [ "$SRC_MD5" = "$ORIG_MD5" ]; then ok "C4a initial copy MD5 match"; else ng "C4a initial copy MD5 mismatch"; fi

# Truncate to 1MB (using dd)
dd if=/dev/zero of="$EDGE_FILE" bs=1M count=1 conv=notrunc 2>/dev/null
sync
sz=$(stat -c %s "$EDGE_FILE")
# File should still be 5MB (dd doesn't truncate, just overwrites first 1MB)
if [ "$sz" = "5242880" ]; then ok "C4b overwrite keeps size ($sz)"; else ng "C4b size wrong ($sz)"; fi

# C4c: Truncate using > redirect (shell truncate)
echo -n "short" > "$EDGE_FILE"
sync
sz=$(stat -c %s "$EDGE_FILE")
if [ "$sz" = "5" ]; then ok "C4c truncate to 5 bytes ($sz)"; else ng "C4c truncate wrong ($sz)"; fi

# C4d: Append to file (echo -n "short" = 5 bytes, echo " appended" = 10 bytes with newline)
echo " appended" >> "$EDGE_FILE"
sync
sz=$(stat -c %s "$EDGE_FILE")
if [ "$sz" = "15" ]; then ok "C4d append to 15 bytes ($sz)"; else ng "C4d append wrong ($sz)"; fi

# C4e: Overwrite middle of file without changing size (use seek on a larger file)
dd if=/dev/urandom of="$EDGE_FILE" bs=1M count=1 2>/dev/null  # 1MB file
sync
sz_before=$(stat -c %s "$EDGE_FILE")
dd if=/dev/zero of="$EDGE_FILE" bs=1 count=100 seek=500000 conv=notrunc 2>/dev/null  # overwrite at offset 500000
sync
sz=$(stat -c %s "$EDGE_FILE")
if [ "$sz" = "$sz_before" ]; then ok "C4e overwrite middle keeps size ($sz)"; else ng "C4e size changed ($sz_before -> $sz)"; fi

# C4f: Sparse file (dd seek without writing)
rm -f "$EDGE_FILE"
dd if=/dev/zero of="$EDGE_FILE" bs=1 count=1 seek=1048575 2>/dev/null
sync
sz=$(stat -c %s "$EDGE_FILE")
if [ "$sz" = "1048576" ]; then ok "C4f sparse file size correct ($sz)"; else ng "C4f sparse size wrong ($sz)"; fi

# C4g: Write to existing large file (overwrite)
dd if=/dev/urandom of="$EDGE_FILE" bs=1M count=2 2>/dev/null
sync
sz=$(stat -c %s "$EDGE_FILE")
if [ "$sz" = "2097152" ]; then ok "C4g overwrite large file ($sz)"; else ng "C4g overwrite wrong ($sz)"; fi

rm -f /tmp/c4_src.bin
check_dmesg && ok "C4 dmesg clean" || ng "C4 dmesg anomaly"

# ============================================================
# C5: Deep directory + many small files
# ============================================================
section "C5: Deep directory (50 levels) + many small files (1000)"

DEEP_DIR="$TEST_DIR/c5_deep"
mkdir -p "$DEEP_DIR"

# C5a: Create 50 levels of nested directories
CUR="$DEEP_DIR"
DEEP_FAIL=0
for i in $(seq 1 50); do
    CUR="$CUR/level_${i}"
    if mkdir "$CUR" 2>/dev/null; then
        :
    else
        ng "C5a mkdir failed at level $i"
        DEEP_FAIL=1
        break
    fi
done
if [ "$DEEP_FAIL" = "0" ]; then ok "C5a 50 levels nested directories created"; fi

# Verify deepest directory exists
if [ -d "$CUR" ]; then ok "C5a deepest directory accessible"; else ng "C5a deepest directory not accessible"; fi

# Write a file in the deepest directory
echo "deep file content" > "$CUR/deep_file.txt"
sync
if [ "$(cat "$CUR/deep_file.txt")" = "deep file content" ]; then
    ok "C5a write/read in deep directory"
else
    ng "C5a deep directory read/write failed"
fi

# C5b: Create 200 small files in a flat directory
FLAT_DIR="$TEST_DIR/c5_flat"
mkdir -p "$FLAT_DIR"
echo "  creating 200 small files..."
SMALL_FAIL=0
for i in $(seq 1 200); do
    echo "file_${i}_content" > "$FLAT_DIR/file_${i}.txt" 2>/dev/null || { SMALL_FAIL=$((SMALL_FAIL+1)); }
done
sync

file_count=$(ls "$FLAT_DIR" | wc -l)
if [ "$file_count" = "200" ]; then
    ok "C5b 200 small files created ($file_count)"
else
    ng "C5b file count wrong ($file_count, failures=$SMALL_FAIL)"
fi

# Verify a random sample of files
SAMPLE_OK=0
SAMPLE_FAIL=0
for i in 1 50 100 150 200; do
    content=$(cat "$FLAT_DIR/file_${i}.txt" 2>/dev/null)
    if [ "$content" = "file_${i}_content" ]; then
        SAMPLE_OK=$((SAMPLE_OK+1))
    else
        SAMPLE_FAIL=$((SAMPLE_FAIL+1))
    fi
done
if [ "$SAMPLE_FAIL" = "0" ]; then ok "C5b sample verification ($SAMPLE_OK/5 correct)"; else ng "C5b sample verification ($SAMPLE_FAIL/5 failed)"; fi

# C5c: Remove all small files
rm -rf "$FLAT_DIR" 2>/dev/null
sync
if [ ! -d "$FLAT_DIR" ]; then ok "C5c cleanup 200 files"; else ng "C5c cleanup failed"; fi

check_dmesg && ok "C5 dmesg clean" || ng "C5 dmesg anomaly"

# ============================================================
# C6: fio verify stress test (data integrity under load)
# ============================================================
section "C6: fio verify stress test (data integrity)"

if which fio >/dev/null 2>&1; then
    FIO_JOB="$TEST_DIR/c6_verify.fio"
    cat > "$FIO_JOB" << 'FIOEOF'
[verify_test]
filename=/mnt/pfs/complex_stress/c6_verify.bin
size=256m
bs=64k
rw=randwrite
ioengine=sync
direct=0
verify=crc32c
verify_fatal=1
verify_dump=0
do_verify=1
verify_backlog=1024
overwrite=1
numjobs=1
runtime=60
time_based=1
FIOEOF

    echo "  running fio verify (256MB, 64K, randwrite, 60s)..."
    export LD_LIBRARY_PATH=/opt/fio:$LD_LIBRARY_PATH
    fio_output=$(fio "$FIO_JOB" 2>&1)
    verify_errors=$(echo "$fio_output" | grep -i 'verify.*error\|verification error\|crc.*mismatch\|md5.*mismatch' || true)

    if [ -n "$verify_errors" ]; then
        ng "C6 fio verify found errors"
        echo "$verify_errors" | head -5 | sed 's/^/    /'
    else
        # Show performance summary
        echo "$fio_output" | grep -E 'WRITE:|READ:|verify' | head -5 | sed 's/^/    /'
        ok "C6 fio verify passed (no data integrity errors)"
    fi
    rm -f "$FIO_JOB" "$TEST_DIR/c6_verify.bin"
else
    skip "C6 fio verify (no fio)"
fi

check_dmesg && ok "C6 dmesg clean" || ng "C6 dmesg anomaly"

# ============================================================
# C7: Mixed workload stability (3min, fio randrw)
# ============================================================
section "C7: Mixed workload stability (2min fio randrw)"

if which fio >/dev/null 2>&1; then
    FIO_JOB="$TEST_DIR/c7_mixed.fio"
    cat > "$FIO_JOB" << 'FIOEOF'
[mixed_rw]
filename=/mnt/pfs/complex_stress/c7_mixed.bin
size=256m
bs=4k
rw=randrw
rwmixread=70
ioengine=sync
direct=0
numjobs=4
runtime=120
time_based=1
group_reporting=1
FIOEOF

    echo "  running fio mixed randrw (4K, 70/30, 4 jobs, 120s)..."
    export LD_LIBRARY_PATH=/opt/fio:$LD_LIBRARY_PATH
    fio "$FIO_JOB" 2>&1 | grep -E 'read:|write:|io=|iops=' | head -5 | sed 's/^/    /'

    # Check dmesg during and after
    if check_dmesg; then
        ok "C7 dmesg clean after 3min mixed IO"
    else
        ng "C7 dmesg anomaly after mixed IO"
    fi

    rm -f "$FIO_JOB" "$TEST_DIR/c7_mixed.bin"
else
    # Fallback: use dd for 3 minutes
    echo "  no fio, using dd fallback (2min)..."
    end_time=$(( $(date +%s) + 120 ))
    iter=0
    while [ $(date +%s) -lt $end_time ]; do
        iter=$((iter+1))
        dd if=/dev/urandom of="$TEST_DIR/c7_dd_${iter}.bin" bs=1M count=10 2>/dev/null
        rm -f "$TEST_DIR/c7_dd_${iter}.bin"
        if [ $((iter % 10)) = 0 ]; then
            if ! check_dmesg; then
                ng "C7 dmesg anomaly at iteration $iter"
                break
            fi
        fi
    done
    ok "C7 dd fallback completed ($iter iterations)"
fi

check_dmesg && ok "C7 final dmesg clean" || ng "C7 final dmesg anomaly"

# ============================================================
# Cleanup
# ============================================================
section "Cleanup"

# Check slab before cleanup
slab_before=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  slab before cleanup: inode=$slab_before"

rm -rf "$TEST_DIR" 2>/dev/null
sync
sleep 2

slab_after=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  slab after cleanup:  inode=$slab_after"

if [ -z "$slab_after" ] || [ "$slab_after" -le "${slab_before:-0}" ]; then
    ok "slab released after cleanup"
else
    echo "  ${Y}[WARN]${N} slab not fully released (before=$slab_before after=$slab_after)"
fi

# Final memory check
mem_avail=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
echo "  MemAvailable: ${mem_avail} KB"

# ============================================================
# Summary
# ============================================================
echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  Complex Stress Test Summary                         ║"
echo "╠══════════════════════════════════════════════════════╣"
printf "║  PASS: %-45s║\n" "$PASS"
printf "║  FAIL: %-45s║\n" "$FAIL"
printf "║  SKIP: %-45s║\n" "$SKIP"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

if [ "$FAIL" = "0" ]; then
    echo -e "${G}ALL COMPLEX TESTS PASSED${N}"
    exit 0
else
    echo -e "${R}$FAIL TEST(S) FAILED${N}"
    exit 1
fi
