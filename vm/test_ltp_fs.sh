#!/bin/sh
# LTP Filesystem Test Runner for PowerFS
#
# Runs standard Linux Test Project (LTP) filesystem test binaries against
# the powerfs mount at /mnt/pfs. Tests are grouped into sections, each with
# independent dmesg capture to detect kernel anomalies.
#
# Test categories:
#   S1: fsstress     - multi-process filesystem stress (fsstress)
#   S2: fsx          - filesystem exerciser (fsx-linux)
#   S3: ftest01-08   - LTP filesystem test suite (ftest01..ftest08)
#   S4: growfiles    - grow/shrink file test (doio/growfiles)
#   S5: stream01-05  - stream IO tests (stream01..stream05)
#   S6: inode01      - inode allocation stress
#   S7: lftest       - large file test (4GB seek)
#   S8: read_all     - read all files recursively
#   S9: syscalls     - key syscall tests (open/mkdir/rename/unlink/...)
#
# Prerequisites:
#   - LTP binaries deployed at /root/ltp-deploy/
#   - bash available at /root/ltp-deploy/bin/bash
#   - powerfs mounted at /mnt/pfs
#
# Run: ssh -p 2223 root@localhost < test_ltp_fs.sh

set -u

MNT=/mnt/pfs
LTP=/root/ltp-deploy
BASH="$LTP/bin/bash"
DMESG_DIR="/tmp/ltp_dmesg"
PASS=0
FAIL=0
SKIP=0
CUR_SCENARIO=""

G='\033[0;32m'; R='\033[0;31m'; Y='\033[0;33m'; C='\033[0;36m'; N='\033[0m'

ok()   { echo -e "  ${G}[PASS]${N} $1"; PASS=$((PASS+1)); }
ng()   { echo -e "  ${R}[FAIL]${N} $1"; FAIL=$((FAIL+1)); }
skip() { echo -e "  ${Y}[SKIP]${N} $1"; SKIP=$((SKIP+1)); }

section() {
    CUR_SCENARIO="$1"
    echo ""
    echo -e "${C}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${N}"
    echo -e "${C}  $1${N}"
    echo -e "${C}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${N}"
    # Clear dmesg ring buffer for this section
    dmesg -c > /dev/null 2>&1
    # Clean test directory
    rm -rf "$MNT/ltp_test" 2>/dev/null
    mkdir -p "$MNT/ltp_test" 2>/dev/null
}

check_dmesg() {
    local logfile="$DMESG_DIR/$(echo "$CUR_SCENARIO" | sed 's/[^a-zA-Z0-9_]/_/g').log"
    dmesg > "$logfile" 2>/dev/null

    local lines
    lines=$(wc -l < "$logfile" 2>/dev/null) || lines=0

    # Check for kernel anomalies
    local errors
    errors=$(grep -E 'BUG:|Oops:|KASAN:|RCU stall|hung task|soft lockup|call trace|Kernel panic|WARNING.*powerfs|general protection fault|unable to handle' "$logfile" 2>/dev/null | head -10)

    if [ -z "$errors" ]; then
        ok "no kernel anomalies (dmesg: ${lines} lines)"
    else
        ng "kernel anomaly detected (dmesg: ${lines} lines):"
        echo "$errors" | while read -r line; do
            echo -e "    ${R}$line${N}"
        done
    fi
    echo -e "  ${Y}dmesg${N}: saved to ${logfile}"
}

# Run a single test binary with timeout
# Usage: run_test "name" /path/to/binary arg1 arg2 ...
# Detects LTP's TCONF (test configuration skip) and counts it as SKIP
# instead of FAIL. LTP returns non-zero exit code for TCONF, but the
# output contains "TCONF" or "skipped" markers.
run_test() {
    local name="$1"
    shift
    echo -e "  ${Y}RUN${N} $name ..."
    if timeout 60 "$@" > "/tmp/ltp_out_$$.txt" 2>&1; then
        ok "$name"
    else
        local rc=$?
        if [ $rc -eq 124 ]; then
            ng "$name (timeout after 60s)"
        else
            # Check if LTP reported TCONF (test configuration skip)
            # LTP uses TBROK for broken tests and TCONF for unsupported config
            if grep -qE 'TCONF|skipped [1-9]|Summary:.*skipped' "/tmp/ltp_out_$$.txt" 2>/dev/null &&
               ! grep -qE 'TFAIL|failed [1-9]|Summary:.*failed' "/tmp/ltp_out_$$.txt" 2>/dev/null; then
                skip "$name (LTP TCONF - not applicable)"
            else
                # Show last few lines of output for debugging
                local tail_out
                tail_out=$(tail -5 "/tmp/ltp_out_$$.txt" 2>/dev/null)
                ng "$name (exit=$rc)"
                if [ -n "$tail_out" ]; then
                    echo "$tail_out" | while read -r line; do
                        echo "        $line"
                    done
                fi
            fi
        fi
    fi
    rm -f "/tmp/ltp_out_$$.txt"
}

# ============================================================
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  PowerFS LTP Filesystem Test Suite                      ║"
echo "║  Target: /mnt/pfs (powerfs)                             ║"
echo "║  LTP binaries: /root/ltp-deploy/                        ║"
echo "╚══════════════════════════════════════════════════════════╝"

# Prepare
mkdir -p "$DMESG_DIR"
dmesg -c > /dev/null 2>&1

# Verify environment
if ! mount | grep -q "on $MNT type powerfs"; then
    echo -e "${R}ERROR: powerfs not mounted at $MNT${N}"
    exit 1
fi
if [ ! -x "$BASH" ]; then
    echo -e "${R}ERROR: bash not found at $BASH${N}"
    exit 1
fi

echo -e "${G}Environment OK: powerfs mounted, bash available${N}"

# --- Mount /dev/shm as tmpfs (CRITICAL for LTP IPC) ---
# LTP's setup_ipc() checks access("/dev/shm", F_OK). If /dev/shm doesn't exist,
# it creates the IPC shared memory file in the test tmpdir. On powerfs, mmap()
# of this file causes SIGBUS during memset. Mounting /dev/shm as tmpfs avoids this.
if [ ! -d /dev/shm ]; then
    mkdir -p /dev/shm
fi
if ! mount | grep -q 'on /dev/shm'; then
    mount -t tmpfs none /dev/shm 2>/dev/null && echo "  mounted /dev/shm as tmpfs"
fi

# --- LTP framework setup: create loop device + mkfs wrapper ---
# Many LTP tests use .mount_device=1 and need a block device.
# Create /dev/loop0 so the framework can find a free device.
# Create mkfs.ext2 wrapper so the framework can format with ext2.
# If mkfs is unavailable, the framework falls back to tmpfs.
setup_ltp_env() {
    # Fix /tmp permissions: LTP's tst_tmpdir() calls mkdtemp() which requires
    # /tmp to be world-writable with sticky bit (1777). Without this, tests
    # like symlink03 fail with EACCES.
    if [ "$(stat -c %a /tmp 2>/dev/null)" != "1777" ]; then
        chmod 1777 /tmp 2>/dev/null && echo "  fixed /tmp permissions to 1777"
    fi

    # Create loop device nodes if missing
    local i=0
    while [ $i -lt 4 ]; do
        if [ ! -e "/dev/loop$i" ]; then
            mknod "/dev/loop$i" b 7 "$i" 2>/dev/null && echo "  created /dev/loop$i"
        fi
        i=$((i + 1))
    done
    # Create mkfs.ext2 wrapper (busybox mke2fs)
    # NOTE: LTP's tst_mkfs() may pass combined options like "-b 1024" as a
    # single argv element (e.g., stat04 uses snprintf(opt_bsize, "-b %i")).
    # The wrapper must re-split such arguments so busybox mke2fs receives
    # "-b" and "1024" as separate args. Using unquoted $@ achieves this.
    if [ ! -e /usr/sbin/mkfs.ext2 ]; then
        mkdir -p /usr/sbin 2>/dev/null
        cat > /usr/sbin/mkfs.ext2 << 'MKFSEOF'
#!/bin/sh
# mkfs.ext2 wrapper for busybox mke2fs
# Re-split arguments on whitespace: LTP passes "-b 1024" as one argv element
exec busybox mke2fs $@
MKFSEOF
        chmod +x /usr/sbin/mkfs.ext2 2>/dev/null
    fi
    # Ensure losetup is in PATH
    if ! which losetup >/dev/null 2>&1; then
        mkdir -p /usr/sbin 2>/dev/null
        ln -sf /bin/busybox /usr/sbin/losetup 2>/dev/null
    fi
}

setup_ltp_env

# Free any loop devices from previous runs
free_loop_devices() {
    local i=0
    while [ $i -lt 4 ]; do
        if [ -e "/dev/loop$i" ]; then
            busybox losetup -d "/dev/loop$i" 2>/dev/null || true
        fi
        i=$((i + 1))
    done
}

echo -e "${G}LTP framework env: loop devices + mkfs wrapper ready${N}"

# ============================================================
# S1: fsstress - multi-process filesystem stress test
# ============================================================
section "S1: fsstress (4 procs x 500 ops x 2 loops)"

if [ -x "$LTP/fs/fsstress" ]; then
    mkdir -p "$MNT/ltp_test/fsstress"
    # -d: dir, -p: nproc, -n: nops, -l: loops
    run_test "fsstress p4 n500 l2" \
        "$LTP/fs/fsstress" -d "$MNT/ltp_test/fsstress" -p 4 -n 500 -l 2 -v
    check_dmesg
else
    skip "fsstress binary not found"
    check_dmesg
fi

# ============================================================
# S2: fsx-linux - filesystem exerciser (mmap-based stress)
# ============================================================
section "S2: fsx-linux (500 ops on powerfs + ramfs baseline)"

if [ -x "$LTP/fs/fsx-linux" ]; then
    # fsx-linux uses tst_test framework: accepts -N (numops), -l (max size MB)
    # It creates its own test file in $TMPDIR/LTP_fsx*/
    # Test 1: on powerfs (TMPDIR -> /mnt/pfs)
    mkdir -p "$MNT/ltp_test/fsx"
    echo -e "  ${Y}RUN${N} fsx-linux on powerfs (N500)..."
    if TMPDIR="$MNT/ltp_test/fsx" timeout 30 "$LTP/fs/fsx-linux" -N 500 > /tmp/fsx_pfs.log 2>&1; then
        ok "fsx-linux on powerfs"
    else
        rc=$?
        if grep -q "Bus error" /tmp/fsx_pfs.log 2>/dev/null; then
            ng "fsx-linux on powerfs (Bus error - powerfs mmap issue)"
        elif [ $rc -eq 124 ]; then
            ng "fsx-linux on powerfs (timeout)"
        else
            ng "fsx-linux on powerfs (exit=$rc)"
        fi
        tail -3 /tmp/fsx_pfs.log 2>/dev/null | while read -r line; do echo "        $line"; done
    fi
    # Test 2: on ramfs /tmp (baseline - should pass)
    echo -e "  ${Y}RUN${N} fsx-linux on ramfs (baseline, N500)..."
    if timeout 30 "$LTP/fs/fsx-linux" -N 500 > /tmp/fsx_tmp.log 2>&1; then
        ok "fsx-linux on ramfs (baseline)"
    else
        rc=$?
        ng "fsx-linux on ramfs (exit=$rc)"
        tail -3 /tmp/fsx_tmp.log 2>/dev/null | while read -r line; do echo "        $line"; done
    fi
    check_dmesg
else
    skip "fsx-linux binary not found"
    check_dmesg
fi

# ============================================================
# S3: ftest01-08 - LTP filesystem test suite
# ============================================================
section "S3: ftest01-08 (filesystem test suite)"

FT_FAIL=0
FT_PASS=0
for i in 1 2 3 4 5 6 7 8; do
    if [ -x "$LTP/fs/ftest0$i" ]; then
        mkdir -p "$MNT/ltp_test/ftest"
        # ftest uses environment variable for base directory
        # ftest01-08 use files in current directory or specified path
        cd "$MNT/ltp_test/ftest"
        rc=0
        if timeout 30 "$LTP/fs/ftest0$i" > "/tmp/ftest0$i.log" 2>&1; then
            ok "ftest0$i"
            FT_PASS=$((FT_PASS+1))
        else
            rc=$?
            if [ $rc -eq 124 ]; then
                ng "ftest0$i (timeout)"
            else
                ng "ftest0$i (exit=$rc)"
            fi
            FT_FAIL=$((FT_FAIL+1))
            tail -3 "/tmp/ftest0$i.log" 2>/dev/null | while read -r line; do
                echo "        $line"
            done
        fi
        cd /
        rm -rf "$MNT/ltp_test/ftest"/*
    else
        skip "ftest0$i not found"
    fi
done
echo -e "  ftest summary: ${G}PASS=$FT_PASS${N} ${R}FAIL=$FT_FAIL${N}"
check_dmesg

# ============================================================
# S4: growfiles - grow/shrink file test
# ============================================================
section "S4: growfiles (file grow/shrink stress)"

if [ -x "$LTP/fs/growfiles" ]; then
    mkdir -p "$MNT/ltp_test/growfiles"
    # growfiles: -W: no write, -d: dir, -b: base filename
    # -B: lower limit, -u: upper limit, -b: base name
    run_test "growfiles" \
        "$LTP/fs/growfiles" -d "$MNT/ltp_test/growfiles" -b gf -B 100 -u 10000 -n 10
    check_dmesg
else
    skip "growfiles binary not found"
    check_dmesg
fi

# ============================================================
# S5: stream01-05 - stream IO tests
# ============================================================
section "S5: stream01-05 (stream IO)"

ST_PASS=0
ST_FAIL=0
for i in 1 2 3 4 5; do
    if [ -x "$LTP/fs/stream0$i" ]; then
        mkdir -p "$MNT/ltp_test/stream"
        cd "$MNT/ltp_test/stream"
        if timeout 30 "$LTP/fs/stream0$i" > "/tmp/stream0$i.log" 2>&1; then
            ok "stream0$i"
            ST_PASS=$((ST_PASS+1))
        else
            rc=$?
            if [ $rc -eq 124 ]; then
                ng "stream0$i (timeout)"
            else
                ng "stream0$i (exit=$rc)"
            fi
            ST_FAIL=$((ST_FAIL+1))
        fi
        cd /
        rm -rf "$MNT/ltp_test/stream"/*
    else
        skip "stream0$i not found"
    fi
done
echo -e "  stream summary: ${G}PASS=$ST_PASS${N} ${R}FAIL=$ST_FAIL${N}"
check_dmesg

# ============================================================
# S6: inode01 - inode allocation stress (needs .mount_device=1)
# ============================================================
section "S6: inode01 (inode allocation, mount_device)"

if [ -x "$LTP/fs/inode01" ]; then
    # inode01 uses .mount_device=1: LTP framework creates a loop device,
    # formats it (ext2 if mkfs available, else tmpfs), and mounts it.
    # Run from /tmp so tmpdir is on ramfs, not powerfs.
    free_loop_devices
    cd /tmp
    run_test "inode01 (framework-managed device)" "$LTP/fs/inode01"
    cd /
    free_loop_devices
    check_dmesg
else
    skip "inode01 binary not found"
    check_dmesg
fi

# ============================================================
# S7: lftest - large file test
# ============================================================
section "S7: lftest (large file seek)"

if [ -x "$LTP/fs/lftest" ]; then
    mkdir -p "$MNT/ltp_test/lftest"
    cd "$MNT/ltp_test/lftest"
    # lftest creates a large file and tests seek operations
    run_test "lftest" "$LTP/fs/lftest"
    cd /
    check_dmesg
else
    skip "lftest binary not found"
    check_dmesg
fi

# ============================================================
# S8: read_all - read all files recursively (needs -d directory)
# ============================================================
section "S8: read_all (recursive read on powerfs)"

if [ -x "$LTP/fs/read_all" ]; then
    # read_all uses tst_test framework: requires -d <directory> argument
    # Create a test tree on powerfs first
    mkdir -p "$MNT/ltp_test/readall"
    for i in 1 2 3 4 5; do
        echo "test data $i" > "$MNT/ltp_test/readall/file_$i"
    done
    mkdir -p "$MNT/ltp_test/readall/subdir"
    echo "subdir data" > "$MNT/ltp_test/readall/subdir/nested"
    mkdir -p "$MNT/ltp_test/readall/subdir/deep"
    echo "deep data" > "$MNT/ltp_test/readall/subdir/deep/nested2"

    # read_all: -d <dir> (required), -r (recursive, but default behavior)
    # The test reads all files in the directory tree
    run_test "read_all -d /mnt/pfs/ltp_test/readall" \
        "$LTP/fs/read_all" -d "$MNT/ltp_test/readall"
    check_dmesg
else
    skip "read_all binary not found"
    check_dmesg
fi

# ============================================================
# S9: Syscall tests (key VFS operations)
# ============================================================
section "S9: syscall tests (open/mkdir/rename/unlink/truncate/...)"

SC_PASS=0
SC_FAIL=0
SC_SKIP=0

# List of syscall test directories and their tests
# Format: "binary_name"
SYSCALL_TESTS="open01 open02 open03 open04 open05 open06 open07 open08 open09 open10 open11 open12 open13 open14 \
    mkdir03 mkdir05 mkdir09 \
    rename01 rename02 rename03 rename05 rename08 rename09 rename10 rename11 rename12 rename13 rename14 rename15 \
    unlink05 unlink06 unlink07 unlink08 unlink09 \
    truncate01 truncate02 truncate03 \
    fcntl01 fcntl02 fcntl03 fcntl05 fcntl07 fcntl08 fcntl09 fcntl10 fcntl11 fcntl14 fcntl18 fcntl19 fcntl20 \
    fallocate01 fallocate02 fallocate03 fallocate04 fallocate05 fallocate06 \
    stat01 stat02 stat03 stat04 \
    readdir01 readdir21 \
    link01 link02 link03 link04 link05 \
    symlink01 symlink02 symlink03 symlink04 \
    utime01 utime02 utime03 utime04 utime05 utime06 \
    chmod01 chmod02 chmod03 chmod04 chmod05 chmod06 chmod07 \
    chown01 chown02 chown03 chown04 chown05 \
    write01 write02 write03 write04 write05 write06 \
    read01 read02 read03 \
    rmdir01 rmdir02 rmdir03 \
    close01 close02"

for test in $SYSCALL_TESTS; do
    if [ -x "$LTP/syscalls/$test" ]; then
        # Run on powerfs: set TMPDIR to powerfs so LTP's tst_tmpdir() creates
        # temp files (including test_file) on powerfs, not on /tmp (ramfs).
        # This is critical for tests like read02 that check filesystem-specific
        # behavior (e.g., O_DIRECT support).
        # NOTE: chmod 1777 is required because some old-style LTP tests (e.g.
        # symlink03) call setuid(nobody) BEFORE tst_tmpdir(), so mkdtemp()
        # runs as "nobody" and needs world-writable parent directory.
        mkdir -p "$MNT/ltp_test/syscalls"
        chmod 1777 "$MNT/ltp_test/syscalls"
        cd "$MNT/ltp_test/syscalls"
        free_loop_devices 2>/dev/null
        rc=0
        if TMPDIR="$MNT/ltp_test/syscalls" timeout 30 "$LTP/syscalls/$test" > "/tmp/sc_$test.log" 2>&1; then
            ok "$test (on powerfs)"
            SC_PASS=$((SC_PASS+1))
        else
            rc=$?
            # Check if LTP reported TCONF (test configuration skip, not a failure)
            # TCONF means the test is not applicable (e.g., readdir21 tests the
            # obsolete __NR_readdir syscall which is not supported on x86_64)
            if grep -qE 'TCONF|Summary:.*skipped' "/tmp/sc_$test.log" 2>/dev/null &&
               ! grep -qE 'TFAIL|Summary:.*failed' "/tmp/sc_$test.log" 2>/dev/null; then
                skip "$test (LTP TCONF - not applicable)"
                SC_SKIP=$((SC_SKIP+1))
            # Check if it failed due to mount_device requirement
            elif grep -q "Failed to acquire device" "/tmp/sc_$test.log" 2>/dev/null; then
                # Retry from /tmp with loop device (tests ext2/tmpfs, not powerfs)
                cd /tmp
                free_loop_devices 2>/dev/null
                if timeout 30 "$LTP/syscalls/$test" > "/tmp/sc_${test}_dev.log" 2>&1; then
                    ok "$test (on tmpfs/ext2 via loop device)"
                    SC_PASS=$((SC_PASS+1))
                else
                    rc2=$?
                    if [ $rc2 -eq 124 ]; then
                        ng "$test (timeout on loop device)"
                    else
                        ng "$test (exit=$rc2 on loop device)"
                    fi
                    SC_FAIL=$((SC_FAIL+1))
                    tail -2 "/tmp/sc_${test}_dev.log" 2>/dev/null | while read -r line; do echo "        $line"; done
                fi
            elif [ $rc -eq 124 ]; then
                ng "$test (timeout)"
                SC_FAIL=$((SC_FAIL+1))
            else
                ng "$test (exit=$rc)"
                SC_FAIL=$((SC_FAIL+1))
                tail -2 "/tmp/sc_$test.log" 2>/dev/null | while read -r line; do echo "        $line"; done
            fi
        fi
        cd /
        rm -rf "$MNT/ltp_test/syscalls"/* 2>/dev/null
        free_loop_devices 2>/dev/null
    else
        skip "$test (not found)"
        SC_SKIP=$((SC_SKIP+1))
    fi
done

echo ""
echo -e "  syscall summary: ${G}PASS=$SC_PASS${N} ${R}FAIL=$SC_FAIL${N} ${Y}SKIP=$SC_SKIP${N}"
check_dmesg

# ============================================================
# S10: fsstress extended - longer run for stability
# ============================================================
section "S10: fsstress extended (8 procs x 1000 ops x 3 loops)"

if [ -x "$LTP/fs/fsstress" ]; then
    mkdir -p "$MNT/ltp_test/fsstress2"
    # Longer run with more processes for stability testing
    # This section should run for at least 1 minute
    run_test "fsstress p8 n1000 l3" \
        "$LTP/fs/fsstress" -d "$MNT/ltp_test/fsstress2" -p 8 -n 1000 -l 3 -v
    check_dmesg
else
    skip "fsstress binary not found"
    check_dmesg
fi

# ============================================================
# Cleanup
# ============================================================
rm -rf "$MNT/ltp_test" 2>/dev/null

# ============================================================
# Final Summary
# ============================================================
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║  LTP Test Summary                                       ║"
echo "╠══════════════════════════════════════════════════════════╣"
printf "║  %-24s %5d  ║\n" "PASS:" "$PASS"
printf "║  %-24s %5d  ║\n" "FAIL:" "$FAIL"
printf "║  %-24s %5d  ║\n" "SKIP:" "$SKIP"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

# Check overall dmesg for any anomalies across all sections
echo -e "${C}━━━ Final dmesg check ━━━${N}"
ALL_ANOMALIES=0
for f in "$DMESG_DIR"/*.log; do
    [ -f "$f" ] || continue
    errs=$(grep -cE 'BUG:|Oops:|KASAN:|RCU stall|hung task|soft lockup|call trace|Kernel panic|WARNING.*powerfs|general protection fault' "$f" 2>/dev/null) || errs=0
    if [ "$errs" -gt 0 ]; then
        echo -e "  ${R}$f: $errs anomalies${N}"
        ALL_ANOMALIES=$((ALL_ANOMALIES + errs))
    fi
done

if [ "$ALL_ANOMALIES" -eq 0 ]; then
    echo -e "  ${G}No kernel anomalies detected in any section${N}"
else
    echo -e "  ${R}Total anomalies: $ALL_ANOMALIES${N}"
fi

echo ""
echo "Done."
