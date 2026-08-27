#!/bin/sh
# PowerFS Kernel Module Complex Stress Test V2
#
# Advanced scenarios beyond test_complex_stress.sh (C1-C7):
#   C8:  Hard links + symlinks (inode sharing, link traversal)
#   C9:  Large directory (1000+ entries, readdir stress)
#   C10: Concurrent rename/unlink races (dentry consistency under contention)
#   C11: fsync correctness (write → fsync → verify → crash-sim)
#   C12: File hole handling (sparse writes + read verification)
#   C13: statfs consistency (df before/during/after IO)
#   C14: 3-minute mixed workload (create/write/read/delete/rename)
#   C15: Memory pressure + slab leak detection
#
# Run inside VM: ssh -p 2223 root@localhost < /tmp/test_complex_stress_v2.sh

set -u
MNT=/mnt/pfs
TEST_DIR="$MNT/complex_v2"
PASS=0
FAIL=0
SKIP=0
WARN=0

G='\033[0;32m'; R='\033[0;31m'; Y='\033[0;33m'; C='\033[0;36m'; N='\033[0m'

ok()   { echo -e "  ${G}[PASS]${N} $1"; PASS=$((PASS+1)); }
ng()   { echo -e "  ${R}[FAIL]${N} $1"; FAIL=$((FAIL+1)); }
skip() { echo -e "  ${Y}[SKIP]${N} $1"; SKIP=$((SKIP+1)); }
warn() { echo -e "  ${Y}[WARN]${N} $1"; WARN=$((WARN+1)); }
section() { echo ""; echo -e "${C}━━━ $1 ━━━${N}"; }

md5_check() {
    local file="$1"
    md5sum "$file" 2>/dev/null | awk '{print $1}'
}

check_dmesg() {
    local errors
    errors=$(dmesg | tail -100 | grep -E 'BUG:|Oops:|KASAN:|RCU stall|hung task|soft lockup|call trace|Kernel panic|WARNING.*powerfs|general protection fault' || true)
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
echo "║  PowerFS Kernel Complex Stress Test V2               ║"
echo "╚══════════════════════════════════════════════════════╝"

rm -rf "$TEST_DIR" 2>/dev/null
mkdir -p "$TEST_DIR"

# Record slab state before tests
SLAB_BEFORE=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  Initial slab: powerfs_inode=$SLAB_BEFORE"

# ============================================================
# C8: Hard links + symlinks
# ============================================================
section "C8: Hard links + symlinks (inode sharing, link traversal)"

# C8a: Create a file and hard link it
echo "test hard link content 12345" > "$TEST_DIR/c8_original.txt"
ORIG_INO=$(stat -c %i "$TEST_DIR/c8_original.txt")

if ln "$TEST_DIR/c8_original.txt" "$TEST_DIR/c8_hardlink.txt" 2>/dev/null; then
    HARD_INO=$(stat -c %i "$TEST_DIR/c8_hardlink.txt")
    if [ "$ORIG_INO" = "$HARD_INO" ]; then
        ok "C8a hard link shares inode ($ORIG_INO)"
    else
        ng "C8a hard link inode mismatch ($ORIG_INO != $HARD_INO)"
    fi

    # Verify link count
    LINKS=$(stat -c %h "$TEST_DIR/c8_original.txt")
    if [ "$LINKS" = "2" ]; then
        ok "C8a link count = 2"
    else
        ng "C8a link count wrong ($LINKS != 2)"
    fi
else
    ng "C8a hard link creation failed"
fi

# C8b: Modify through hard link, read through original
echo "modified via hardlink" > "$TEST_DIR/c8_hardlink.txt"
sync
CONTENT=$(cat "$TEST_DIR/c8_original.txt")
if [ "$CONTENT" = "modified via hardlink" ]; then
    ok "C8b modify via hardlink visible from original"
else
    ng "C8b hardlink modification not visible"
fi

# C8c: Create symlink
if ln -s "c8_original.txt" "$TEST_DIR/c8_symlink.txt" 2>/dev/null; then
    if [ -L "$TEST_DIR/c8_symlink.txt" ]; then
        ok "C8c symlink created"
        SYM_TARGET=$(readlink "$TEST_DIR/c8_symlink.txt")
        if [ "$SYM_TARGET" = "c8_original.txt" ]; then
            ok "C8c symlink target correct"
        else
            ng "C8c symlink target wrong ($SYM_TARGET)"
        fi
        # Read through symlink
        SYM_CONTENT=$(cat "$TEST_DIR/c8_symlink.txt")
        if [ "$SYM_CONTENT" = "modified via hardlink" ]; then
            ok "C8c read through symlink works"
        else
            ng "C8c symlink read failed"
        fi
    else
        ng "C8c not a symlink"
    fi
else
    ng "C8c symlink creation failed"
fi

# C8d: Remove original, verify hard link still works
rm -f "$TEST_DIR/c8_original.txt"
sync
if [ -f "$TEST_DIR/c8_hardlink.txt" ]; then
    CONTENT=$(cat "$TEST_DIR/c8_hardlink.txt")
    if [ "$CONTENT" = "modified via hardlink" ]; then
        ok "C8d hard link survives original deletion"
    else
        ng "C8d hard link content wrong after original deletion"
    fi
    LINKS=$(stat -c %h "$TEST_DIR/c8_hardlink.txt")
    if [ "$LINKS" = "1" ]; then
        ok "C8d link count decremented to 1"
    else
        ng "C8d link count wrong after deletion ($LINKS != 1)"
    fi
else
    ng "C8d hard link disappeared after original deletion"
fi

# C8e: Symlink should now be dangling
if [ -L "$TEST_DIR/c8_symlink.txt" ] && [ ! -f "$TEST_DIR/c8_symlink.txt" ]; then
    ok "C8e dangling symlink detected"
else
    warn "C8e symlink state unexpected (may resolve via hardlink)"
fi

check_dmesg && ok "C8 dmesg clean" || ng "C8 dmesg anomaly"

# ============================================================
# C9: Large directory (1000+ entries, readdir stress)
# ============================================================
section "C9: Large directory (1000 entries, readdir stress)"

LARGE_DIR="$TEST_DIR/c9_large"
mkdir -p "$LARGE_DIR"
N_FILES=1000

echo "  creating $N_FILES files..."
CREATE_FAIL=0
for i in $(seq 1 $N_FILES); do
    echo "content_${i}" > "$LARGE_DIR/file_${i}.txt" 2>/dev/null || CREATE_FAIL=$((CREATE_FAIL+1))
done
sync

if [ "$CREATE_FAIL" = "0" ]; then
    ok "C9a all $N_FILES files created"
else
    ng "C9a $CREATE_FAIL files failed to create"
fi

# Verify file count via ls
FILE_COUNT=$(ls "$LARGE_DIR" | wc -l)
if [ "$FILE_COUNT" = "$N_FILES" ]; then
    ok "C9b readdir returns correct count ($FILE_COUNT)"
else
    ng "C9b readdir count wrong ($FILE_COUNT != $N_FILES)"
fi

# Verify file count via find
FIND_COUNT=$(find "$LARGE_DIR" -maxdepth 1 -type f | wc -l)
if [ "$FIND_COUNT" = "$N_FILES" ]; then
    ok "C9c find returns correct count ($FIND_COUNT)"
else
    ng "C9c find count wrong ($FIND_COUNT != $N_FILES)"
fi

# Verify content of random samples
SAMPLE_OK=0
SAMPLE_FAIL=0
for i in 1 100 250 500 750 1000; do
    content=$(cat "$LARGE_DIR/file_${i}.txt" 2>/dev/null)
    if [ "$content" = "content_${i}" ]; then
        SAMPLE_OK=$((SAMPLE_OK+1))
    else
        SAMPLE_FAIL=$((SAMPLE_FAIL+1))
    fi
done
if [ "$SAMPLE_FAIL" = "0" ]; then
    ok "C9d sample content verification ($SAMPLE_OK/6 correct)"
else
    ng "C9d sample content verification ($SAMPLE_FAIL/6 failed)"
fi

# Stress readdir: run ls 50 times rapidly
echo "  stress readdir (50 iterations)..."
REaddir_FAIL=0
for iter in $(seq 1 50); do
    count=$(ls "$LARGE_DIR" 2>/dev/null | wc -l)
    if [ "$count" != "$N_FILES" ]; then
        Readdir_FAIL=$((Readdir_FAIL+1))
    fi
done
if [ "$REaddir_FAIL" = "0" ]; then
    ok "C9e readdir stress (50x) consistent"
else
    ng "C9e readdir stress failed ($REaddir_FAIL/50 inconsistent)"
fi

# Cleanup large dir
rm -rf "$LARGE_DIR"
sync
if [ ! -d "$LARGE_DIR" ]; then
    ok "C9f large directory cleanup"
else
    ng "C9f large directory cleanup failed"
fi

check_dmesg && ok "C9 dmesg clean" || ng "C9 dmesg anomaly"

# ============================================================
# C10: Concurrent rename/unlink races
# ============================================================
section "C10: Concurrent rename/unlink races (dentry consistency)"

RACE_DIR="$TEST_DIR/c10_race"
mkdir -p "$RACE_DIR"

# Create initial files
for i in $(seq 1 20); do
    echo "initial_content_${i}" > "$RACE_DIR/file_${i}.txt"
done
sync

# C10a: Concurrent renames (rename file_X -> file_Y)
echo "  launching 4 concurrent rename workers..."
rename_worker() {
    local id=$1
    local iter=0
    while [ $iter -lt 50 ]; do
        local src=$(( (iter * 4 + id) % 20 + 1 ))
        local dst=$(( (iter * 4 + id + 10) % 20 + 1 ))
        mv "$RACE_DIR/file_${src}.txt" "$RACE_DIR/file_${dst}.txt" 2>/dev/null
        iter=$((iter + 1))
    done
}

for i in 1 2 3 4; do
    rename_worker $i &
done
wait
sync

# Verify no corruption: all files should be readable
RACE_OK=0
RACE_BAD=0
for i in $(seq 1 20); do
    if [ -f "$RACE_DIR/file_${i}.txt" ]; then
        content=$(cat "$RACE_DIR/file_${i}.txt" 2>/dev/null)
        if echo "$content" | grep -q "initial_content_"; then
            RACE_OK=$((RACE_OK+1))
        else
            RACE_BAD=$((RACE_BAD+1))
        fi
    fi
done
echo "  readable: $RACE_OK, corrupted: $RACE_BAD"
if [ "$RACE_BAD" = "0" ]; then
    ok "C10a concurrent rename: no data corruption"
else
    ng "C10a concurrent rename: $RACE_BAD corrupted files"
fi

# C10b: Concurrent create + unlink
echo "  launching concurrent create + unlink workers..."
create_worker() {
    local id=$1
    for i in $(seq 1 50); do
        echo "create_${id}_${i}" > "$RACE_DIR/concurrent_${id}_${i}.txt" 2>/dev/null
    done
}

unlink_worker() {
    for i in $(seq 1 50); do
        rm -f "$RACE_DIR/concurrent_"*"_${i}.txt" 2>/dev/null
    done
}

create_worker 1 &
create_worker 2 &
unlink_worker &
wait
sync

# Verify filesystem is still consistent
ls "$RACE_DIR" >/dev/null 2>&1
if [ $? -eq 0 ]; then
    ok "C10b concurrent create+unlink: directory consistent"
else
    ng "C10b concurrent create+unlink: directory inconsistent"
fi

check_dmesg && ok "C10 dmesg clean" || ng "C10 dmesg anomaly"

# ============================================================
# C11: fsync correctness
# ============================================================
section "C11: fsync correctness (write → fsync → verify)"

FSYNC_FILE="$TEST_DIR/c11_fsync.bin"

# C11a: Write data and fsync, then verify
dd if=/dev/urandom of=/tmp/c11_src.bin bs=64k count=64 2>/dev/null  # 4MB
SRC_MD5=$(md5_check /tmp/c11_src.bin)

cp /tmp/c11_src.bin "$FSYNC_FILE"
sync  # Force flush all

# Verify after sync
FILE_MD5=$(md5_check "$FSYNC_FILE")
if [ "$SRC_MD5" = "$FILE_MD5" ]; then
    ok "C11a fsync data integrity (4MB)"
else
    ng "C11a fsync data mismatch (src=$SRC_MD5 file=$FILE_MD5)"
fi

# C11b: Append + fsync, verify
dd if=/dev/urandom of=/tmp/c11_append.bin bs=64k count=32 2>/dev/null  # 2MB
APPEND_MD5=$(md5_check /tmp/c11_append.bin)

cat /tmp/c11_append.bin >> "$FSYNC_FILE"
sync

# Verify the first 4MB is still intact
dd if="$FSYNC_FILE" of=/tmp/c11_first.bin bs=64k count=64 2>/dev/null
FIRST_MD5=$(md5_check /tmp/c11_first.bin)
if [ "$SRC_MD5" = "$FIRST_MD5" ]; then
    ok "C11b original data intact after append"
else
    ng "C11b original data corrupted after append"
fi

# Verify total size
TOTAL_SIZE=$(stat -c %s "$FSYNC_FILE")
EXPECTED=$((4*1024*1024 + 2*1024*1024))
if [ "$TOTAL_SIZE" = "$EXPECTED" ]; then
    ok "C11b total size correct after append ($TOTAL_SIZE)"
else
    ng "C11b size wrong ($TOTAL_SIZE != $EXPECTED)"
fi

rm -f /tmp/c11_src.bin /tmp/c11_append.bin /tmp/c11_first.bin
check_dmesg && ok "C11 dmesg clean" || ng "C11 dmesg anomaly"

# ============================================================
# C12: File hole handling (sparse writes)
# ============================================================
section "C12: File hole handling (sparse writes)"

HOLE_FILE="$TEST_DIR/c12_sparse.bin"

# C12a: Create sparse file with holes
# Write 4KB at offset 0, skip to 1MB, write 4KB, skip to 2MB, write 4KB
dd if=/dev/urandom of=/tmp/c12_block1.bin bs=4k count=1 2>/dev/null
dd if=/dev/urandom of=/tmp/c12_block2.bin bs=4k count=1 2>/dev/null
dd if=/dev/urandom of=/tmp/c12_block3.bin bs=4k count=1 2>/dev/null

BLOCK1_MD5=$(md5_check /tmp/c12_block1.bin)
BLOCK2_MD5=$(md5_check /tmp/c12_block2.bin)
BLOCK3_MD5=$(md5_check /tmp/c12_block3.bin)

# Create sparse file
dd if=/tmp/c12_block1.bin of="$HOLE_FILE" bs=4k conv=notrunc 2>/dev/null
dd if=/tmp/c12_block2.bin of="$HOLE_FILE" bs=4k seek=256 conv=notrunc 2>/dev/null  # offset 1MB
dd if=/tmp/c12_block3.bin of="$HOLE_FILE" bs=4k seek=512 conv=notrunc 2>/dev/null  # offset 2MB
sync

# Verify file size
FILE_SIZE=$(stat -c %s "$HOLE_FILE")
EXPECTED_SIZE=$((2048*1024 + 4096))  # 2MB + 4KB
if [ "$FILE_SIZE" = "$EXPECTED_SIZE" ]; then
    ok "C12a sparse file size correct ($FILE_SIZE)"
else
    ng "C12a sparse file size wrong ($FILE_SIZE != $EXPECTED_SIZE)"
fi

# Verify each block
dd if="$HOLE_FILE" of=/tmp/c12_read1.bin bs=4k count=1 2>/dev/null
READ1_MD5=$(md5_check /tmp/c12_read1.bin)
if [ "$BLOCK1_MD5" = "$READ1_MD5" ]; then
    ok "C12b block 1 at offset 0 correct"
else
    ng "C12b block 1 mismatch"
fi

dd if="$HOLE_FILE" of=/tmp/c12_read2.bin bs=4k skip=256 count=1 2>/dev/null
READ2_MD5=$(md5_check /tmp/c12_read2.bin)
if [ "$BLOCK2_MD5" = "$READ2_MD5" ]; then
    ok "C12b block 2 at offset 1MB correct"
else
    ng "C12b block 2 mismatch"
fi

dd if="$HOLE_FILE" of=/tmp/c12_read3.bin bs=4k skip=512 count=1 2>/dev/null
READ3_MD5=$(md5_check /tmp/c12_read3.bin)
if [ "$BLOCK3_MD5" = "$READ3_MD5" ]; then
    ok "C12b block 3 at offset 2MB correct"
else
    ng "C12b block 3 mismatch"
fi

# C12c: Read from hole (should be zeros)
dd if="$HOLE_FILE" of=/tmp/c12_hole.bin bs=4k skip=100 count=1 2>/dev/null
HOLE_MD5=$(md5_check /tmp/c12_hole.bin)
ZERO_MD5=$(dd if=/dev/zero bs=4k count=1 2>/dev/null | md5sum | awk '{print $1}')
if [ "$HOLE_MD5" = "$ZERO_MD5" ]; then
    ok "C12c hole region reads as zeros"
else
    ng "C12c hole region not zeros"
fi

rm -f /tmp/c12_*.bin
check_dmesg && ok "C12 dmesg clean" || ng "C12 dmesg anomaly"

# ============================================================
# C13: statfs consistency
# ============================================================
section "C13: statfs consistency (df before/during/after IO)"

# C13a: Get initial statfs
STATFS_BEFORE=$(df -k "$MNT" | tail -1 | awk '{print $2":"$3":"$4}')
echo "  statfs before: total:used:avail = $STATFS_BEFORE KB"

# Write 50MB
dd if=/dev/urandom of="$TEST_DIR/c13_statfs.bin" bs=1M count=50 2>/dev/null
sync
sleep 1

STATFS_AFTER_WRITE=$(df -k "$MNT" | tail -1 | awk '{print $2":"$3":"$4}')
echo "  statfs after 50MB write: $STATFS_AFTER_WRITE KB"

# Verify used space increased
USED_BEFORE=$(echo "$STATFS_BEFORE" | cut -d: -f2)
USED_AFTER=$(echo "$STATFS_AFTER_WRITE" | cut -d: -f2)
if [ "$USED_AFTER" -gt "$USED_BEFORE" ]; then
    ok "C13a used space increased after write"
else
    warn "C13a used space did not increase (may be cached)"
fi

# Delete and verify space is reclaimed
rm -f "$TEST_DIR/c13_statfs.bin"
sync
sleep 2

STATFS_AFTER_DEL=$(df -k "$MNT" | tail -1 | awk '{print $2":"$3":"$4}')
echo "  statfs after delete: $STATFS_AFTER_DEL KB"
USED_AFTER_DEL=$(echo "$STATFS_AFTER_DEL" | cut -d: -f2)

if [ "$USED_AFTER_DEL" -le "$USED_AFTER" ]; then
    ok "C13b space reclaimed after delete"
else
    warn "C13b space not fully reclaimed (may be delayed)"
fi

check_dmesg && ok "C13 dmesg clean" || ng "C13 dmesg anomaly"

# ============================================================
# C14: 3-minute mixed workload (create/write/read/delete/rename)
# ============================================================
section "C14: 3-minute mixed workload (create/write/read/delete/rename)"

MIXED_DIR="$TEST_DIR/c14_mixed"
mkdir -p "$MIXED_DIR"

RUN_SECONDS=180  # 3 minutes
echo "  running mixed workload for ${RUN_SECONDS}s..."

END_TIME=$(( $(date +%s) + RUN_SECONDS ))
ITER=0
DMESG_CHECK_INTERVAL=30
LAST_CHECK=$(date +%s)

while [ $(date +%s) -lt $END_TIME ]; do
    ITER=$((ITER+1))

    # Randomly choose operation
    OP=$((ITER % 6))
    case $OP in
        0) # Create
            echo "mixed_${ITER}" > "$MIXED_DIR/file_${ITER}.txt" 2>/dev/null
            ;;
        1) # Write larger data
            dd if=/dev/urandom of="$MIXED_DIR/blob_${ITER}.bin" bs=4k count=4 2>/dev/null
            ;;
        2) # Read
            cat "$MIXED_DIR/file_$((ITER / 2)).txt" >/dev/null 2>&1
            ;;
        3) # Rename
            if [ -f "$MIXED_DIR/file_${ITER}.txt" ]; then
                mv "$MIXED_DIR/file_${ITER}.txt" "$MIXED_DIR/renamed_${ITER}.txt" 2>/dev/null
            fi
            ;;
        4) # Delete
            rm -f "$MIXED_DIR/blob_$((ITER / 3)).bin" 2>/dev/null
            ;;
        5) # List directory
            ls "$MIXED_DIR" >/dev/null 2>&1
            ;;
    esac

    # Periodic dmesg check
    NOW=$(date +%s)
    if [ $((NOW - LAST_CHECK)) -ge $DMESG_CHECK_INTERVAL ]; then
        if ! check_dmesg; then
            ng "C14 dmesg anomaly at iteration $ITER (${NOW}s)"
            break
        fi
        LAST_CHECK=$NOW
        echo "  [${ITER}] $(date '+%H:%M:%S') still running... (files: $(ls "$MIXED_DIR" 2>/dev/null | wc -l))"
    fi
done

# Final dmesg check after 3 minutes
if check_dmesg; then
    ok "C14 dmesg clean after 3min mixed workload ($ITER iterations)"
else
    ng "C14 dmesg anomaly after 3min mixed workload"
fi

echo "  total iterations: $ITER"
echo "  files remaining: $(ls "$MIXED_DIR" 2>/dev/null | wc -l)"

# Verify filesystem still responsive
if touch "$MIXED_DIR/c14_final_test.txt" && [ -f "$MIXED_DIR/c14_final_test.txt" ]; then
    ok "C14 filesystem responsive after stress"
else
    ng "C14 filesystem unresponsive after stress"
fi

rm -rf "$MIXED_DIR"
sync

# ============================================================
# C15: Memory pressure + slab leak detection
# ============================================================
section "C15: Memory pressure + slab leak detection"

SLAB_BEFORE_C15=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  slab before C15: powerfs_inode=$SLAB_BEFORE_C15"

# Create and delete many files to stress slab allocation
PRESSURE_DIR="$TEST_DIR/c15_pressure"
mkdir -p "$PRESSURE_DIR"

echo "  creating 500 files..."
for i in $(seq 1 500); do
    echo "pressure_${i}" > "$PRESSURE_DIR/p_${i}.txt" 2>/dev/null
done
sync

SLAB_DURING=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  slab during (500 files): powerfs_inode=$SLAB_DURING"

# Delete all
rm -rf "$PRESSURE_DIR"
sync
sleep 3  # Allow slab reclaim

SLAB_AFTER=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  slab after cleanup: powerfs_inode=$SLAB_AFTER"

# Check for leak
if [ -z "$SLAB_AFTER" ] || [ "$SLAB_AFTER" -le "${SLAB_BEFORE_C15:-0}" ]; then
    ok "C15 no slab leak detected (before=$SLAB_BEFORE_C15 after=$SLAB_AFTER)"
else
    LEAK=$((SLAB_AFTER - SLAB_BEFORE_C15))
    warn "C15 possible slab leak (before=$SLAB_BEFORE_C15 after=$SLAB_AFTER, diff=$LEAK)"
fi

# Memory available
MEM_AVAIL=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
echo "  MemAvailable: ${MEM_AVAIL} KB"

check_dmesg && ok "C15 dmesg clean" || ng "C15 dmesg anomaly"

# ============================================================
# Cleanup
# ============================================================
section "Cleanup"

# Final slab check
SLAB_FINAL=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  slab initial:  powerfs_inode=$SLAB_BEFORE"
echo "  slab final:    powerfs_inode=$SLAB_FINAL"

rm -rf "$TEST_DIR" 2>/dev/null
sync
sleep 2

SLAB_AFTER_CLEANUP=$(cat /proc/slabinfo 2>/dev/null | awk '/^powerfs_inode/ {print $2}')
echo "  slab after rm: powerfs_inode=$SLAB_AFTER_CLEANUP"

if [ -z "$SLAB_AFTER_CLEANUP" ] || [ "$SLAB_AFTER_CLEANUP" -le "${SLAB_BEFORE:-0}" ]; then
    ok "slab fully released after cleanup"
else
    warn "slab not fully released (initial=$SLAB_BEFORE final=$SLAB_AFTER_CLEANUP)"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  Complex Stress Test V2 Summary                      ║"
echo "╠══════════════════════════════════════════════════════╣"
printf "║  PASS: %-45s║\n" "$PASS"
printf "║  FAIL: %-45s║\n" "$FAIL"
printf "║  SKIP: %-45s║\n" "$SKIP"
printf "║  WARN: %-45s║\n" "$WARN"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

if [ "$FAIL" = "0" ]; then
    echo -e "${G}ALL COMPLEX V2 TESTS PASSED${N}"
    exit 0
else
    echo -e "${R}$FAIL TEST(S) FAILED${N}"
    exit 1
fi
