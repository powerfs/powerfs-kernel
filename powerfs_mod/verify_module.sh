#!/bin/bash
# =============================================================================
# PowerFS Kernel Module — Compilation & Symbol Export Verification
#
# Purpose:
#   Static verification of the compiled powerfs.ko kernel module without
#   requiring a running backend. Designed for CI/CD pipelines to validate:
#     1. Compilation artifact integrity (ELF format, architecture)
#     2. Module metadata (version, name)
#     3. Delta Sync API symbol exports
#     4. Multi-node / Leader management symbol exports
#     5. VFS callback definitions
#     6. Object file completeness
#
# Prerequisites:
#   - Compiled powerfs.ko and corresponding .o files in current directory
#   - Standard Linux tools: nm, modinfo, file
#
# Usage:
#   # Run in the module build directory
#   cd kernel/powerfs_mod && bash verify_module.sh
#
#   # Specify module name
#   MODULE_NAME=powerfs bash verify_module.sh
#
# Exit Codes:
#   0 - All checks passed
#   1 - One or more checks failed
#
# Environment Variables:
#   MODULE_NAME   Module name (default: powerfs)
# =============================================================================

MODULE_NAME="${MODULE_NAME:-powerfs}"
PASS=0
FAIL=0
TOTAL=0

# ========== Color Output ==========
if [ -t 1 ]; then
    C_RED='\033[0;31m'
    C_GREEN='\033[0;32m'
    C_YELLOW='\033[0;33m'
    C_BLUE='\033[0;34m'
    C_CYAN='\033[0;36m'
    C_RESET='\033[0m'
else
    C_RED=''
    C_GREEN=''
    C_YELLOW=''
    C_BLUE=''
    C_CYAN=''
    C_RESET=''
fi

# ========== Output Functions ==========
log_pass() {
    PASS=$((PASS + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${C_GREEN}[PASS]${C_RESET} $*"
}

log_fail() {
    FAIL=$((FAIL + 1)); TOTAL=$((TOTAL + 1))
    echo -e "  ${C_RED}[FAIL]${C_RESET} $*"
}

log_warn() {
    echo -e "  ${C_YELLOW}[WARN]${C_RESET} $*"
}

log_info() {
    echo -e "  ${C_BLUE}[INFO]${C_RESET} $*"
}

run_test() {
    local name="$1"
    local cmd="$2"
    echo -n "  ${C_CYAN}Test:${C_RESET} $name ... "
    if eval "$cmd" 2>/dev/null; then
        log_pass "$name"
        return 0
    else
        log_fail "$name"
        return 1
    fi
}

section_header() {
    echo ""
    echo -e "${C_CYAN}━━━ $* ━━━${C_RESET}"
}

# ========== Check Prerequisites ==========
if ! command -v nm >/dev/null 2>&1; then
    echo "ERROR: 'nm' command not found (binutils required)"
    exit 1
fi

if ! command -v file >/dev/null 2>&1; then
    echo "ERROR: 'file' command not found"
    exit 1
fi

# ========== Start Verification ==========
echo ""
echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
echo -e "${C_CYAN}║  PowerFS Kernel Module Verification                  ${C_RESET}"
echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"
echo ""

MODULE_FILE="${MODULE_NAME}.ko"

if [ ! -f "$MODULE_FILE" ]; then
    echo -e "  ${C_RED}FATAL:${C_RESET} Module file '$MODULE_FILE' not found in $(pwd)"
    echo "  Build it first:"
    echo "    make -C /lib/modules/\$(uname -r)/build M=\$(pwd) modules"
    exit 1
fi

# Pre-load nm output for all symbol checks
NM_OUTPUT=$(nm "$MODULE_FILE" 2>/dev/null)

# ============================================================================
# 1. Compilation Artifacts
# ============================================================================
section_header "1. Compilation Artifacts"
run_test "Module file exists" "test -f $MODULE_FILE"
run_test "Module file non-empty" "test -s $MODULE_FILE"
run_test "Module has read permissions" "test -r $MODULE_FILE"

# ============================================================================
# 2. ELF Basic Information
# ============================================================================
section_header "2. ELF Basic Information"
run_test "Valid ELF format" "file $MODULE_FILE | grep -q 'ELF'"
run_test "Target architecture: x86-64" "file $MODULE_FILE | grep -q 'x86-64'"

# Additional ELF checks
ELF_INFO=$(file "$MODULE_FILE" 2>/dev/null)
log_info "ELF details: $ELF_INFO"
run_test "Contains debug info (not stripped)" 'echo "$ELF_INFO" | grep -q "debug_info\|not stripped"'

# ============================================================================
# 3. Module Metadata
# ============================================================================
section_header "3. Module Metadata"
MODINFO_OUT=$(modinfo "$MODULE_FILE" 2>/dev/null)
run_test "modinfo produces output" "test -n '$MODINFO_OUT'"
run_test "Module version field present" 'echo "$MODINFO_OUT" | grep -q "version\|srcversion"'
run_test "Module name field present" 'echo "$MODINFO_OUT" | grep -q "^name:"'
run_test "Module filename field present" 'echo "$MODINFO_OUT" | grep -q "filename:"'

# ============================================================================
# 4. Delta Sync API Symbol Exports
# ============================================================================
section_header "4. Delta Sync API Symbols"

DELTA_SYMBOLS=(
    "powerfs_net_pool_init"
    "powerfs_net_pool_cleanup"
    "powerfs_net_add_server"
    "powerfs_net_is_connected"
    "powerfs_net_lookup"
    "powerfs_net_disconnect"
)

for sym in "${DELTA_SYMBOLS[@]}"; do
    run_test "$sym exported" 'echo "$NM_OUTPUT" | grep -q "$sym$"'
done

# ============================================================================
# 5. Multi-Node / Leader Management Symbols
# ============================================================================
section_header "5. Multi-Node / Leader Symbols"

LEADER_SYMBOLS=(
    "powerfs_net_remove_server"
)

for sym in "${LEADER_SYMBOLS[@]}"; do
    run_test "$sym exported" 'echo "$NM_OUTPUT" | grep -q "$sym$"'
done

# ============================================================================
# 7. VFS Callback Definitions
# ============================================================================
section_header "7. VFS Callback Definitions"

VFS_SYMBOLS=(
    "powerfs_d_revalidate"
    "powerfs_dentry_operations"
    "powerfs_fill_super"
    "powerfs_kill_sb_super"
)

for sym in "${VFS_SYMBOLS[@]}"; do
    run_test "$sym defined" 'echo "$NM_OUTPUT" | grep -q "$sym"'
done

# ============================================================================
# 8. Compilation Artifact Completeness
# ============================================================================
section_header "8. Object File Completeness"

OBJ_FILES=(
    "powerfs_fs.o"
    "powerfs_net.o"
    "powerfs_tlv.o"
    "powerfs_transport.o"
    "powerfs_serializer.o"
    "powerfs_mod.o"
)

for obj in "${OBJ_FILES[@]}"; do
    run_test "$obj exists" "test -f $obj"
done

# ============================================================================
# Summary
# ============================================================================
echo ""
echo -e "${C_CYAN}╔══════════════════════════════════════════════════════╗${C_RESET}"
echo -e "${C_CYAN}║  Verification Summary                                 ${C_RESET}"
echo -e "${C_CYAN}╚══════════════════════════════════════════════════════╝${C_RESET}"
echo ""
echo -e "  ${C_BLUE}Total:${C_RESET}   $TOTAL"
echo -e "  ${C_GREEN}Passed:${C_RESET}  $PASS"
echo -e "  ${C_RED}Failed:${C_RESET}  $FAIL"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo -e "  ${C_GREEN}✓ ALL VERIFICATION TESTS PASSED${C_RESET}"
    echo ""
    echo "  Module is ready for integration testing."
    exit 0
else
    echo -e "  ${C_RED}✗ ${FAIL} VERIFICATION TEST(S) FAILED${C_RESET}"
    echo ""
    echo "  Failed checks:"
    echo "    grep '\[FAIL\]' <this_output>"
    echo ""
    echo "  Troubleshooting:"
    echo "    1. Rebuild the module: make clean && make"
    echo "    2. Check for missing symbol implementations"
    echo "    3. Verify the .o files are in the same directory"
    exit 1
fi