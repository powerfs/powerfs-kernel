#!/bin/bash
# PowerFS 多客户端一致性测试
# Client A: SSH port 2223 (VM1, 172.30.0.100)
# Client B: SSH port 2224 (VM2, 172.30.0.101)

# Parse arguments
MODE="full"        # smoke | full
OUTPUT="text"      # text | junit
DMESG_CHECK=false
JUNIT_FILE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --mode=*)      MODE="${1#*=}"; shift ;;
        --mode)        MODE="$2"; shift 2 ;;
        --output=*)    OUTPUT="${1#*=}"; shift ;;
        --output)      OUTPUT="$2"; shift 2 ;;
        --dmesg-check) DMESG_CHECK=true; shift ;;
        --junit-file=*) JUNIT_FILE="${1#*=}"; shift ;;
        --junit-file)  JUNIT_FILE="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--mode=smoke|full] [--output=text|junit] [--dmesg-check] [--junit-file=FILE]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Smoke mode: only run core cases
SMOKE_CASES="MC-001 MC-002 MC-101 MC-201 MC-203 MC-302 MC-304"
should_run() {
    case "$1" in
        MC-001|MC-002|MC-101|MC-201|MC-203|MC-302|MC-304) return 0 ;;
        *) return 1 ;;
    esac
}

# Test results for JUnit XML
declare -a TEST_NAMES TEST_RESULTS TEST_TIMES TEST_MESSAGES
TEST_COUNT=0

# Record test result
record_test() {
    local name="$1" result="$2" time="$3" msg="${4:-}"
    TEST_NAMES[$TEST_COUNT]="$name"
    TEST_RESULTS[$TEST_COUNT]="$result"
    TEST_TIMES[$TEST_COUNT]="$time"
    TEST_MESSAGES[$TEST_COUNT]="$msg"
    TEST_COUNT=$((TEST_COUNT+1))
}

# Case timing
CASE_START=0
case_start() { CASE_START=$(date +%s); }
case_end() { echo $(($(date +%s) - CASE_START)); }

# dmesg anomaly check helper
check_dmesg() {
    local label="$1" port="$2" baseline="$3"
    local new_dmesg=$(ssh -p $port -o StrictHostKeyChecking=no root@localhost "dmesg | tail -n +$((baseline+1))" 2>/dev/null)
    local anomaly=$(echo "$new_dmesg" | grep -iE 'BUG:|Oops:|KASAN:|Poison overwritten|Object corrupt|slab_err|Call Trace.*powerfs|WARNING.*powerfs|general protection fault|unable to handle' | grep -v "report a bug" | head -5)
    if [ -n "$anomaly" ]; then
        echo "DMESG ANOMALY on $label:"
        echo "$anomaly"
        return 1
    fi
    return 0
}

SSH_A="ssh -p 2223 -o StrictHostKeyChecking=no root@localhost"
SSH_B="ssh -p 2224 -o StrictHostKeyChecking=no root@localhost"
PFS=/mnt/pfs

PASS=0
FAIL=0
SKIP=0

ok()   {
    local case_id="${1%% *}"
    local elapsed=$(case_end)
    echo "PASS: $1"
    PASS=$((PASS+1))
    record_test "$case_id" PASS "$elapsed" "$1"
}
ng()   {
    local case_id="${1%% *}"
    local elapsed=$(case_end)
    echo "FAIL: $1"
    FAIL=$((FAIL+1))
    record_test "$case_id" FAIL "$elapsed" "$1"
}
skip() {
    local case_id="${1%% *}"
    local elapsed=$(case_end)
    echo "SKIP: $1"
    SKIP=$((SKIP+1))
    record_test "$case_id" SKIP "$elapsed" "$1"
}

# Helper: run on client A
run_a() { $SSH_A "$1" 2>/dev/null; }
run_b() { $SSH_B "$1" 2>/dev/null; }

# Helper: wait for NOTIFY propagation
wait_notify() { sleep ${1:-${WAIT_NOTIFY_DEFAULT:-2}}; }

# dmesg baseline
DMESG_BASELINE_A=0
DMESG_BASELINE_B=0
if [ "$DMESG_CHECK" = "true" ]; then
    DMESG_BASELINE_A=$(run_a 'dmesg | wc -l')
    DMESG_BASELINE_B=$(run_b 'dmesg | wc -l')
    echo "dmesg baseline: A=$DMESG_BASELINE_A B=$DMESG_BASELINE_B lines"
fi

echo "============================================"
echo "  PowerFS Multi-Client Consistency Test"
echo "============================================"
echo ""

# === MC-001: Dual VM mount ===
if [ "$MODE" != "smoke" ] || should_run "MC-001"; then case_start
echo "=== MC-001: Dual VM mount ==="
MNT_A=$(run_a 'mount | grep -c powerfs')
MNT_B=$(run_b 'mount | grep -c powerfs')
if [ "$MNT_A" = "1" ] && [ "$MNT_B" = "1" ]; then ok "MC-001 dual mount"; else ng "MC-001 mount A=$MNT_A B=$MNT_B"; fi
fi

# === MC-002: Shared directory visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-002"; then case_start
echo "=== MC-002: Shared directory visibility ==="
run_a "rm -rf $PFS/mc_shared && mkdir -p $PFS/mc_shared"
wait_notify 3
DIR_B=$(run_b "ls $PFS/ | grep -c mc_shared")
if [ "$DIR_B" = "1" ]; then ok "MC-002 shared dir visible"; else ng "MC-002 shared dir not visible (B sees $DIR_B)"; fi
fi

# === MC-003: Shared file visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-003"; then case_start
echo "=== MC-003: Shared file visibility ==="
run_a "touch $PFS/mc_shared/f1"
wait_notify 3
FILE_B=$(run_b "ls $PFS/mc_shared/ | grep -c f1")
if [ "$FILE_B" = "1" ]; then ok "MC-003 shared file visible"; else ng "MC-003 shared file not visible"; fi
fi

# === MC-101: Write visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-101"; then case_start
echo "=== MC-101: Write visibility ==="
run_a "echo hello > $PFS/mc_shared/wtest && sync"
wait_notify 3
READ_B=$(run_b "cat $PFS/mc_shared/wtest")
if [ "$READ_B" = "hello" ]; then ok "MC-101 write visible"; else ng "MC-101 write not visible (B read '$READ_B')"; fi
fi

# === MC-102: Overwrite visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-102"; then case_start
echo "=== MC-102: Overwrite visibility ==="
run_a "echo world > $PFS/mc_shared/wtest && sync"
wait_notify 3
READ_B=$(run_b "cat $PFS/mc_shared/wtest")
if [ "$READ_B" = "world" ]; then ok "MC-102 overwrite visible"; else ng "MC-102 overwrite not visible (B read '$READ_B')"; fi
fi

# === MC-103: Large file write ===
if [ "$MODE" != "smoke" ] || should_run "MC-103"; then case_start
echo "=== MC-103: Large file write (1MB) ==="
run_a "dd if=/dev/urandom of=$PFS/mc_shared/big bs=1M count=1 2>/dev/null && sync"
wait_notify 3
HASH_A=$(run_a "md5sum $PFS/mc_shared/big | cut -d' ' -f1")
HASH_B=$(run_b "md5sum $PFS/mc_shared/big | cut -d' ' -f1")
if [ "$HASH_A" = "$HASH_B" ]; then ok "MC-103 large file hash match ($HASH_A)"; else ng "MC-103 hash mismatch A=$HASH_A B=$HASH_B"; fi
fi

# === MC-104: Truncate down visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-104"; then case_start
echo "=== MC-104: Truncate down visibility ==="
run_a "truncate -s 100 $PFS/mc_shared/wtest && sync"
wait_notify 3
SZ_B=$(run_b "stat -c %s $PFS/mc_shared/wtest")
if [ "$SZ_B" = "100" ]; then ok "MC-104 truncate visible (size=100)"; else ng "MC-104 truncate not visible (B sees size=$SZ_B)"; fi
fi

# === MC-105: Truncate up visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-105"; then case_start
echo "=== MC-105: Truncate up visibility ==="
run_a "truncate -s 8192 $PFS/mc_shared/wtest && sync"
wait_notify 3
SZ_B=$(run_b "stat -c %s $PFS/mc_shared/wtest")
if [ "$SZ_B" = "8192" ]; then ok "MC-105 extend visible (size=8192)"; else ng "MC-105 extend not visible (B sees size=$SZ_B)"; fi
fi

# === MC-106: fallocate extend visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-106"; then case_start
echo "=== MC-106: fallocate extend visibility ==="
run_a "rm -f $PFS/mc_shared/fatest && dd if=/dev/zero of=$PFS/mc_shared/fatest bs=4096 count=4 2>/dev/null && sync"
wait_notify 3
run_a "fallocate -l 65536 $PFS/mc_shared/fatest && sync"
wait_notify 3
SZ_B=$(run_b "stat -c %s $PFS/mc_shared/fatest")
if [ "$SZ_B" = "65536" ]; then
    # Check extended region is zeros
    NZ=$(run_b "dd if=$PFS/mc_shared/fatest bs=1 skip=16384 count=49152 2>/dev/null | tr -d '\\0' | wc -c")
    if [ "$NZ" = "0" ]; then ok "MC-106 fallocate extend visible + zeros"; else ng "MC-106 extend region has $NZ non-zero bytes"; fi
else
    ng "MC-106 fallocate size not visible (B sees $SZ_B)"
fi
fi

# === MC-107: PUNCH_HOLE visibility (O-10) ===
if [ "$MODE" != "smoke" ] || should_run "MC-107"; then case_start
echo "=== MC-107: PUNCH_HOLE cross-client visibility (O-10) ==="
# Need C program for PUNCH_HOLE since BusyBox doesn't support -p
# Use python or direct syscall
run_a "rm -f $PFS/mc_shared/ptest"
# Write 16KB known pattern on A
run_a "dd if=/dev/urandom of=$PFS/mc_shared/ptest bs=4096 count=4 2>/dev/null && sync"
wait_notify 3
# B reads to populate pagecache
run_b "dd if=$PFS/mc_shared/ptest of=/dev/null bs=4096 count=4 2>/dev/null"
# A punches hole using C program
run_a "/mnt/host/test_fallocate_modes $PFS/mc_shared > /dev/null 2>&1; echo done"
# Actually use a simpler approach: A writes zeros manually to simulate
# Since BusyBox lacks fallocate -p, use C program on A
HOLE_RESULT=$(run_a '
cat > /tmp/punch_test.c << "CEOF"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/falloc.h>
#include <sys/stat.h>
int main() {
    int fd = open("'$PFS'/mc_shared/ptest", O_WRONLY);
    if (fd < 0) { perror("open"); return 1; }
    int ret = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096, 4096);
    if (ret < 0) { perror("fallocate"); return 1; }
    fsync(fd);
    close(fd);
    return 0;
}
CEOF
gcc -o /tmp/punch_test /tmp/punch_test.c 2>/dev/null && /tmp/punch_test && echo PUNCH_OK || echo PUNCH_FAIL
')
wait_notify 3
# B reads punched region
NZ_B=$(run_b "dd if=$PFS/mc_shared/ptest bs=1 skip=4096 count=4096 2>/dev/null | tr -d '\\0' | wc -c")
if echo "$HOLE_RESULT" | grep -q PUNCH_OK; then
    if [ "$NZ_B" = "0" ]; then ok "MC-107 PUNCH_HOLE visible (B sees zeros)"; else ng "MC-107 PUNCH_HOLE not visible (B sees $NZ_B non-zero)"; fi
else
    skip "MC-107 PUNCH_HOLE (no gcc on VM or punch failed: $HOLE_RESULT)"
fi
fi

# === MC-108: Concurrent write different offsets ===
if [ "$MODE" != "smoke" ] || should_run "MC-108"; then case_start
echo "=== MC-108: Concurrent write different offsets ==="
run_a "rm -f $PFS/mc_shared/cwtest && truncate -s 8192 $PFS/mc_shared/cwtest"
wait_notify 3
run_a "dd if=/dev/urandom of=$PFS/mc_shared/cwtest bs=4096 count=1 seek=0 conv=notrunc 2>/dev/null && sync" &
run_b "dd if=/dev/urandom of=$PFS/mc_shared/cwtest bs=4096 count=1 seek=1 conv=notrunc 2>/dev/null && sync" &
wait
wait
wait_notify 3
HASH_A=$(run_a "md5sum $PFS/mc_shared/cwtest | cut -d' ' -f1")
HASH_B=$(run_b "md5sum $PFS/mc_shared/cwtest | cut -d' ' -f1")
if [ "$HASH_A" = "$HASH_B" ]; then ok "MC-108 concurrent write consistent ($HASH_A)"; else ng "MC-108 concurrent write divergent A=$HASH_A B=$HASH_B"; fi
fi

# === MC-201: unlink visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-201"; then case_start
echo "=== MC-201: unlink visibility ==="
run_a "rm -f $PFS/mc_shared/f1"
wait_notify 3
STAT_B=$(run_b "ls $PFS/mc_shared/f1 2>&1")
if echo "$STAT_B" | grep -qi "No such"; then ok "MC-201 unlink visible"; else ng "MC-201 unlink not visible (B: $STAT_B)"; fi
fi

# === MC-202: mkdir visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-202"; then case_start
echo "=== MC-202: mkdir visibility ==="
run_a "mkdir -p $PFS/mc_shared/dir1"
wait_notify 3
DIR_B=$(run_b "ls -d $PFS/mc_shared/dir1 2>/dev/null | grep -c dir1")
if [ "$DIR_B" = "1" ]; then ok "MC-202 mkdir visible"; else ng "MC-202 mkdir not visible"; fi
fi

# === MC-203: rmdir visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-203"; then case_start
echo "=== MC-203: rmdir visibility ==="
run_a "rmdir $PFS/mc_shared/dir1"
wait_notify 3
DIR_B=$(run_b "ls -d $PFS/mc_shared/dir1 2>/dev/null | grep -c dir1")
if [ "$DIR_B" = "0" ]; then ok "MC-203 rmdir visible"; else ng "MC-203 rmdir not visible"; fi
fi

# === MC-204: rename visibility ===
if [ "$MODE" != "smoke" ] || should_run "MC-204"; then case_start
echo "=== MC-204: rename visibility ==="
run_a "echo data > $PFS/mc_shared/oldname"
wait_notify 3
run_a "mv $PFS/mc_shared/oldname $PFS/mc_shared/newname"
wait_notify 3
OLD_B=$(run_b "ls $PFS/mc_shared/oldname 2>&1 | grep -c 'No such'")
NEW_B=$(run_b "cat $PFS/mc_shared/newname 2>/dev/null")
if [ "$OLD_B" = "1" ] && [ "$NEW_B" = "data" ]; then ok "MC-204 rename visible"; else ng "MC-204 rename not fully visible (old_gone=$OLD_B new_content='$NEW_B')"; fi
fi

# === MC-301: pagecache stale (INLINE file) ===
if [ "$MODE" != "smoke" ] || should_run "MC-301"; then case_start
echo "=== MC-301: pagecache stale - INLINE file ==="
run_a "rm -f $PFS/mc_shared/stale1 && echo v1 > $PFS/mc_shared/stale1 && sync"
wait_notify 3
# B reads to cache
run_b "cat $PFS/mc_shared/stale1 > /dev/null"
# A modifies
run_a "echo v2 > $PFS/mc_shared/stale1 && sync"
wait_notify 3
READ_B=$(run_b "cat $PFS/mc_shared/stale1")
if [ "$READ_B" = "v2" ]; then ok "MC-301 INLINE stale resolved (B sees v2)"; else ng "MC-301 INLINE stale (B sees '$READ_B' expected v2)"; fi
fi

# === MC-302: pagecache stale (FLAT file) — O-04 KEY TEST ===
if [ "$MODE" != "smoke" ] || should_run "MC-302"; then case_start
echo "=== MC-302: pagecache stale - FLAT file (O-04 KEY TEST) ==="
run_a "rm -f $PFS/mc_shared/stale2"
# Create FLAT file (>8KB to trigger migration)
run_a "dd if=/dev/urandom of=$PFS/mc_shared/stale2 bs=4096 count=4 2>/dev/null && sync"
wait_notify 3
HASH_ORIG=$(run_a "md5sum $PFS/mc_shared/stale2 | cut -d' ' -f1")
# B reads to populate pagecache
run_b "md5sum $PFS/mc_shared/stale2 > /dev/null 2>&1"
# A overwrites the FLAT file
run_a "dd if=/dev/urandom of=$PFS/mc_shared/stale2 bs=4096 count=4 2>/dev/null && sync"
run_a "dd if=/dev/urandom of=$PFS/mc_shared/stale2 bs=4096 count=4 2>/dev/null && sync"
wait_notify 3
HASH_NEW_A=$(run_a "md5sum $PFS/mc_shared/stale2 | cut -d' ' -f1")
HASH_B=$(run_b "md5sum $PFS/mc_shared/stale2 | cut -d' ' -f1")
if [ "$HASH_NEW_A" != "$HASH_ORIG" ]; then
    if [ "$HASH_B" = "$HASH_NEW_A" ]; then
        ok "MC-302 FLAT stale resolved (O-04 NOT an issue: B sees new data)"
    elif [ "$HASH_B" = "$HASH_ORIG" ]; then
        ng "MC-302 FLAT STALE READ (O-04 CONFIRMED: B sees old data, expected new)"
    else
        ng "MC-302 FLAT inconsistent (A=$HASH_NEW_A B=$HASH_B orig=$HASH_ORIG)"
    fi
else
    skip "MC-302 FLAT stale (A overwrite produced same hash, inconclusive)"
fi
fi

# === MC-303: Directory lease expiry ===
if [ "$MODE" != "smoke" ] || should_run "MC-303"; then case_start
echo "=== MC-303: Directory lease expiry ==="
run_a "mkdir -p $PFS/mc_shared/dirlist"
wait_notify 3
# B lists directory
run_b "ls $PFS/mc_shared/dirlist/ > /dev/null 2>&1"
# A creates files in directory
run_a "touch $PFS/mc_shared/dirlist/file1"
wait_notify 3
LS_B=$(run_b "ls $PFS/mc_shared/dirlist/ | grep -c file1")
if [ "$LS_B" = "1" ]; then ok "MC-303 dir lease expired (B sees new file)"; else ng "MC-303 dir lease stale (B doesn't see new file)"; fi
fi

# === MC-304: inode attribute cache (chmod) ===
if [ "$MODE" != "smoke" ] || should_run "MC-304"; then case_start
echo "=== MC-304: inode attribute cache (chmod) ==="
run_a "rm -f $PFS/mc_shared/chmod_test && echo x > $PFS/mc_shared/chmod_test && chmod 600 $PFS/mc_shared/chmod_test"
wait_notify 3
# B stats
run_b "stat -c %a $PFS/mc_shared/chmod_test > /dev/null 2>&1"
# A changes mode
run_a "chmod 644 $PFS/mc_shared/chmod_test"
wait_notify 3
MODE_B=$(run_b "stat -c %a $PFS/mc_shared/chmod_test 2>/dev/null")
if [ "$MODE_B" = "644" ]; then ok "MC-304 chmod visible (mode=644)"; else ng "MC-304 chmod stale (B sees mode=$MODE_B expected 644)"; fi
fi

# === Cleanup ===
run_a "rm -rf $PFS/mc_shared" 2>/dev/null

echo ""
echo "============================================"
echo "  Results: PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
echo "============================================"

# dmesg anomaly check
if [ "$DMESG_CHECK" = "true" ]; then
    echo ""
    echo "=== dmesg anomaly check ==="
    DMESG_ANOMALY=0
    if ! check_dmesg "VM A" 2223 "$DMESG_BASELINE_A"; then
        DMESG_ANOMALY=1
    fi
    if ! check_dmesg "VM B" 2224 "$DMESG_BASELINE_B"; then
        DMESG_ANOMALY=1
    fi
    if [ "$DMESG_ANOMALY" = "1" ]; then
        echo "FAIL: dmesg anomaly detected"
        FAIL=$((FAIL+1))
    else
        echo "PASS: no dmesg anomaly"
        PASS=$((PASS+1))
    fi
fi

# Generate JUnit XML if requested
if [ "$OUTPUT" = "junit" ] || [ -n "$JUNIT_FILE" ]; then
    JXML="/tmp/multi-client-results.xml"
    [ -n "$JUNIT_FILE" ] && JXML="$JUNIT_FILE"

    TOTAL_TIME=0
    for t in "${TEST_TIMES[@]}"; do TOTAL_TIME=$((TOTAL_TIME + t)); done

    echo '<?xml version="1.0" encoding="UTF-8"?>' > "$JXML"
    echo "<testsuite name=\"multi-client\" tests=\"$TEST_COUNT\" failures=\"$FAIL\" skipped=\"$SKIP\" time=\"$TOTAL_TIME\">" >> "$JXML"

    for i in $(seq 0 $((TEST_COUNT-1))); do
        name="${TEST_NAMES[$i]}"
        result="${TEST_RESULTS[$i]}"
        time="${TEST_TIMES[$i]}"
        msg="${TEST_MESSAGES[$i]}"

        case "$result" in
            PASS) echo "  <testcase name=\"$name\" time=\"$time\"/>" >> "$JXML" ;;
            FAIL) echo "  <testcase name=\"$name\" time=\"$time\"><failure message=\"$msg\" type=\"REGRESSION\">$msg</failure></testcase>" >> "$JXML" ;;
            SKIP) echo "  <testcase name=\"$name\" time=\"$time\"><skipped message=\"$msg\"/></testcase>" >> "$JXML" ;;
        esac
    done

    echo "</testsuite>" >> "$JXML"
    echo "JUnit XML written to $JXML"
fi

[ "$FAIL" -eq 0 ]
