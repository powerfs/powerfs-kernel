#!/bin/bash
# =============================================================================
# PowerFS Test Environment — Kernel Filesystem Environment Startup
#
# Purpose:
#   Build, load, and configure the PowerFS kernel module for testing.
#   This sets up the kernel-level filesystem stack (powerfs.ko) to
#   communicate with backend services (Filer/Master/Volume) using
#   the powerfs_net TCP protocol, bypassing FUSE entirely.
#
# Prerequisites:
#   - Kernel headers installed (linux-headers-$(uname -r))
#   - Build tools: gcc, make, kernel build infrastructure
#   - Backend services running (use start_fuse_env.sh first)
#   - Root privileges for insmod/mount operations
#
# Usage:
#   # Build module and mount with defaults
#   sudo ./start_kernel_env.sh
#
#   # Build only (no mount)
#   sudo ./start_kernel_env.sh --build-only
#
#   # Mount with specific multi-node configuration
#   sudo ./start_kernel_env.sh \
#       --filers "172.21.0.31:9334,172.21.0.32:9334,172.21.0.33:9334" \
#       --master "172.21.0.11:9333" \
#       --volume "172.21.0.21:8080"
#
#   # Mount with FUSE test environment defaults (mapped host ports)
#   sudo ./start_kernel_env.sh --from-fuse-env
#
#   # Unmount only (keep module loaded)
#   sudo ./start_kernel_env.sh --unmount
#
#   # Full reset (unload module + clean mount)
#   sudo ./start_kernel_env.sh --reset
#
# Environment Variables:
#   POWERFS_MODULE    Module name (default: powerfs)
#   POWERFS_MOUNT     Mount point (default: /mnt/powerfs)
#   POWERFS_FILERS    Comma-separated Filer host:port list
#   POWERFS_MASTER    Master host:port
#   POWERFS_VOLUME    Volume host:port
#   KERNEL_BUILD_DIR  Kernel build directory (auto-detected)
# =============================================================================

set -e

# ========== Configuration ==========
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODULE_NAME="${POWERFS_MODULE:-powerfs}"
MODULE_DIR="$SCRIPT_DIR"
MODULE_FILE="${MODULE_DIR}/${MODULE_NAME}.ko"
MOUNT_POINT="${POWERFS_MOUNT:-/mnt/powerfs}"
LOG_DIR="/tmp/powerfs/logs"
BUILD_DIR="${KERNEL_BUILD_DIR:-/lib/modules/$(uname -r)/build}"

# Default backend addresses (mapped from docker-compose.test.yml)
# When FUSE test env is running, these are the HOST-mapped ports
#   filer-1-test:  172.21.0.31:9334  (host: 8988/8989)
#   filer-2-test:  172.21.0.32:9334  (host: 8990/8991)
#   filer-3-test:  172.21.0.33:9334  (host: 8992/8993)
#   master-1-test: 172.21.0.11:9333  (host: 9433)
#   volume-1-test:  172.21.0.21:8080  (host: 8180)
DEFAULT_FILERS="${POWERFS_FILERS:-127.0.0.1:8988,127.0.0.1:8990,127.0.0.1:8992}"
DEFAULT_MASTER="${POWERFS_MASTER:-127.0.0.1:9433}"
DEFAULT_VOLUME="${POWERFS_VOLUME:-127.0.0.1:8180}"

BUILD_ONLY=0
UNMOUNT_ONLY=0
RESET=0
FROM_FUSE_ENV=0
FILERS="$DEFAULT_FILERS"
MASTER="$DEFAULT_MASTER"
VOLUME="$DEFAULT_VOLUME"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-only)    BUILD_ONLY=1 ;;
        --unmount)       UNMOUNT_ONLY=1 ;;
        --reset)         RESET=1 ;;
        --from-fuse-env) FROM_FUSE_ENV=1 ;;
        --filers)        FILERS="$2"; shift ;;
        --master)        MASTER="$2"; shift ;;
        --volume)        VOLUME="$2"; shift ;;
        --mount)         MOUNT_POINT="$2"; shift ;;
        --help|-h)
            echo "Usage: $0 [--build-only|--unmount|--reset]"
            echo "         [--filers addr_list] [--master addr] [--volume addr] [--mount path]"
            echo ""
            echo "Options:"
            echo "  --build-only      Compile module only, don't mount"
            echo "  --unmount         Unmount only, keep module loaded"
            echo "  --reset           Unload module and clean mount point"
            echo "  --from-fuse-env   Use default FUSE test env port mappings"
            echo "  --filers LIST     Comma-separated Filer host:port (default: 3 local)"
            echo "  --master ADDR     Master host:port"
            echo "  --volume ADDR     Volume host:port"
            echo "  --mount PATH      Mount point (default: /mnt/powerfs)"
            exit 0
            ;;
        *) echo "Unknown option: $1 (use --help)"; exit 1 ;;
    esac
    shift
done

# ========== Color Output ==========
if [ -t 1 ]; then
    C_RED='\033[0;31m'; C_GREEN='\033[0;32m'; C_YELLOW='\033[0;33m'
    C_BLUE='\033[0;34m'; C_CYAN='\033[0;36m'; C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_BLUE=''; C_CYAN=''; C_RESET=''
fi

log_info()  { echo -e "${C_BLUE}[INFO]${C_RESET}  $(date +%H:%M:%S) $*"; }
log_pass()  { echo -e "${C_GREEN}[PASS]${C_RESET}  $*"; }
log_warn()  { echo -e "${C_YELLOW}[WARN]${C_RESET}  $*"; }
log_error() { echo -e "${C_RED}[ERROR]${C_RESET} $*"; }
log_step()  { echo -e "\n${C_CYAN}━━━ $* ━━━${C_RESET}"; }

# ========== Utility Functions ==========

check_root() {
    if [ "$EUID" -ne 0 ]; then
        log_error "This script must be run as root (for insmod/mount)"
        log_info "  Try: sudo $0 $*"
        exit 1
    fi
}

check_fuse_backend() {
    local addr="$1"
    local host port
    host="${addr%:*}"
    port="${addr#*:}"
    
    # Try via docker first
    if command -v docker >/dev/null 2>&1; then
        if docker ps --format '{{.Ports}}' 2>/dev/null | grep -q "${port}->"; then
            return 0
        fi
    fi
    
    # Try direct TCP
    timeout 2 bash -c "echo >/dev/tcp/$host/$port" 2>/dev/null && return 0
    return 1
}

ensure_mount_point() {
    if mountpoint "$MOUNT_POINT" 2>/dev/null; then
        log_warn "$MOUNT_POINT is already mounted"
        return 0
    fi
    
    mkdir -p "$MOUNT_POINT" 2>/dev/null || {
        log_error "Cannot create mount point $MOUNT_POINT"
        exit 1
    }
    log_pass "Mount point ready: $MOUNT_POINT"
}

# ========== Module Build ==========
build_module() {
    log_step "Building Kernel Module"
    
    if [ -f "$MODULE_FILE" ]; then
        local module_mtime
        module_mtime=$(stat -c '%Y' "$MODULE_FILE" 2>/dev/null)
        local src_mtime
        src_mtime=$(find "$MODULE_DIR" -name "*.c" -newer "$MODULE_FILE" 2>/dev/null | head -1)
        
        if [ -z "$src_mtime" ]; then
            log_info "Module $MODULE_FILE is up to date"
            return 0
        fi
        log_info "Source files newer than module, rebuilding..."
    fi
    
    # Check kernel build directory
    if [ ! -d "$BUILD_DIR" ]; then
        log_error "Kernel build directory not found: $BUILD_DIR"
        log_info "  Install: apt-get install linux-headers-$(uname -r)"
        exit 1
    fi
    
    # Build
    log_info "Running make in $MODULE_DIR..."
    cd "$MODULE_DIR"
    
    make clean 2>/dev/null || true
    make -C "$BUILD_DIR" M="$MODULE_DIR" modules 2>&1 | tee "$LOG_DIR/build_$(date +%Y%m%d_%H%M%S).log"
    
    if [ "${PIPESTATUS[0]}" -eq 0 ] && [ -f "$MODULE_FILE" ]; then
        log_pass "Module built: $MODULE_FILE"
        local size
        size=$(stat -c '%s' "$MODULE_FILE" 2>/dev/null)
        log_info "  Size: $size bytes"
    else
        log_error "Module build failed"
        log_info "  Check the build log for details"
        exit 1
    fi
    
    # Verify module
    if command -v nm >/dev/null 2>&1; then
        local sym_count
        sym_count=$(nm "$MODULE_FILE" 2>/dev/null | grep -c " T ")
        log_info "  Exports: $sym_count symbols"
    fi
}

# ========== Module Load ==========
load_module() {
    log_step "Loading Kernel Module"
    
    # Unload if already loaded
    if lsmod | grep -q "$MODULE_NAME"; then
        log_info "Module $MODULE_NAME already loaded, unloading first..."
        rmmod "$MODULE_NAME" 2>/dev/null || true
        sleep 1
    fi
    
    # Load
    log_info "Loading $MODULE_FILE..."
    insmod "$MODULE_FILE" 2>&1 | tee "$LOG_DIR/load_$(date +%Y%m%d_%H%M%S).log"
    
    if lsmod | grep -q "$MODULE_NAME"; then
        log_pass "Module loaded: $MODULE_NAME"
        
        # Show dmesg
        log_info "Kernel log entries:"
        dmesg 2>/dev/null | grep -i powerfs | tail -5 | while IFS= read -r line; do
            log_info "  $line"
        done
    else
        log_error "Module failed to load"
        log_info "  Check: dmesg | tail -20"
        exit 1
    fi
    
    # Verify filesystem type registered
    if grep -q "powerfs" /proc/filesystems 2>/dev/null; then
        log_pass "Filesystem type 'powerfs' registered in kernel"
    else
        log_warn "Filesystem type not found in /proc/filesystems"
        log_info "  May need: modprobe $MODULE_NAME"
    fi
}

# ========== Mount ==========
mount_filesystem() {
    log_step "Mounting PowerFS Filesystem"
    
    ensure_mount_point
    
    # Parse first filer for mount (kernel module connects to first)
    local first_filer
    first_filer=$(echo "$FILERS" | cut -d',' -f1)
    local filer_host filer_port
    filer_host="${first_filer%:*}"
    filer_port="${first_filer#*:}"
    
    local master_host master_port
    master_host="${MASTER%:*}"
    master_port="${MASTER#*:}"
    
    local volume_host volume_port
    volume_host="${VOLUME%:*}"
    volume_port="${VOLUME#*:}"
    
    log_info "Mount configuration:"
    log_info "  Filer(s):  $FILERS"
    log_info "  Master:    $MASTER"
    log_info "  Volume:    $VOLUME"
    log_info "  Mount:     $MOUNT_POINT"
    
    # Build mount options
    local mount_opts="filer=${filer_host}:${filer_port},master=${master_host}:${master_port},volume=${volume_host}:${volume_port}"
    
    # For multi-filer, pass as comma-separated
    local filer_opt=""
    local first=1
    for addr in $(echo "$FILERS" | tr ',' ' '); do
        if [ "$first" -eq 1 ]; then
            filer_opt="$addr"
            first=0
        else
            filer_opt="${filer_opt},${addr}"
        fi
    done
    
    mount_opts="filer=${filer_opt},master=${MASTER},volume=${VOLUME}"
    
    log_info "  mount -t powerfs -o $mount_opts none $MOUNT_POINT"
    
    # Check if filer is reachable
    if ! check_fuse_backend "$first_filer"; then
        log_warn "Filer $first_filer not reachable, mount may fail"
        log_info "  Start backend first: sudo ./start_fuse_env.sh"
        log_info "  Or use DUMMY_MODE: DUMMY_MODE=1 ./test_multi_node.sh"
    fi
    
    # Mount
    mount -t powerfs -o "$mount_opts" none "$MOUNT_POINT" 2>&1 | tee "$LOG_DIR/mount_$(date +%Y%m%d_%H%M%S).log"
    
    if mountpoint "$MOUNT_POINT" 2>/dev/null; then
        log_pass "PowerFS mounted at: $MOUNT_POINT"
        
        # Quick sanity check
        if ls "$MOUNT_POINT" >/dev/null 2>&1; then
            log_pass "Filesystem accessible (ls succeeded)"
        else
            log_warn "Mount succeeded but ls failed (check backend)"
        fi
    else
        log_error "Mount failed"
        log_info "  Check: dmesg | tail -30"
        log_info "  Check mount log: $LOG_DIR/mount_*.log"
        exit 1
    fi
}

# ========== Unmount ==========
unmount_filesystem() {
    log_step "Unmounting PowerFS"
    
    if mountpoint "$MOUNT_POINT" 2>/dev/null; then
        log_info "Unmounting $MOUNT_POINT..."
        umount "$MOUNT_POINT" 2>/dev/null || umount -f "$MOUNT_POINT" 2>/dev/null || {
            log_warn "Force unmount failed, trying lazy..."
            umount -l "$MOUNT_POINT" 2>/dev/null || true
        }
        sleep 1
        
        if ! mountpoint "$MOUNT_POINT" 2>/dev/null; then
            log_pass "Unmounted successfully"
        else
            log_error "Failed to unmount $MOUNT_POINT"
            log_info "  Try: sudo umount -l $MOUNT_POINT"
        fi
    else
        log_info "$MOUNT_POINT is not mounted"
    fi
}

# ========== Module Unload ==========
unload_module() {
    log_step "Unloading Kernel Module"
    
    # Unmount first
    if mountpoint "$MOUNT_POINT" 2>/dev/null; then
        unmount_filesystem
        sleep 2
    fi
    
    # Remove
    if lsmod | grep -q "$MODULE_NAME"; then
        log_info "Removing module $MODULE_NAME..."
        rmmod "$MODULE_NAME" 2>/dev/null || rmmod -f "$MODULE_NAME" 2>/dev/null || true
        sleep 1
        
        if lsmod | grep -q "$MODULE_NAME"; then
            log_warn "Module still loaded after rmmod"
            log_info "  May be in use. Check: lsmod | grep $MODULE_NAME"
        else
            log_pass "Module unloaded"
        fi
    else
        log_info "Module $MODULE_NAME is not loaded"
    fi
}

# ========== Quick Smoke Test ==========
smoke_test() {
    log_step "Quick Smoke Test"
    
    if ! mountpoint "$MOUNT_POINT" 2>/dev/null; then
        log_warn "Not mounted, skipping smoke test"
        return 0
    fi
    
    local test_dir="${MOUNT_POINT}/__kernel_smoke_$$"
    
    # Create
    mkdir -p "$test_dir" 2>/dev/null || {
        log_warn "mkdir failed (backend may not support)"
        return 0
    }
    
    # Write + Read
    echo "kernel_smoke_test_$(date +%s)" > "$test_dir/test.txt" 2>/dev/null
    local content
    content=$(cat "$test_dir/test.txt" 2>/dev/null)
    
    if [ -n "$content" ]; then
        log_pass "Write + Read test passed: $content"
    else
        log_warn "Write test: cannot read back (check backend connectivity)"
    fi
    
    # Delta Sync test
    echo "v2" > "$test_dir/test.txt" 2>/dev/null
    content=$(cat "$test_dir/test.txt" 2>/dev/null)
    if [ "$content" = "v2" ]; then
        log_pass "Delta Sync cache invalidation works"
    else
        log_warn "Delta Sync: stale content or read failed"
    fi
    
    # Cleanup
    rm -rf "$test_dir" 2>/dev/null || true
    log_info "Smoke test complete"
}

# ========== Main ==========
main() {
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  PowerFS Kernel Filesystem Environment Setup             ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo -e "  ${C_BLUE}Module:${C_RESET}    ${MODULE_NAME}.ko"
    echo -e "  ${C_BLUE}Mount:${C_RESET}     ${MOUNT_POINT}"
    echo -e "  ${C_BLUE}Build dir:${C_RESET} ${BUILD_DIR}"
    echo -e "  ${C_BLUE}Filers:${C_RESET}    ${FILERS}"
    echo -e "  ${C_BLUE}Master:${C_RESET}    ${MASTER}"
    echo -e "  ${C_BLUE}Volume:${C_RESET}    ${VOLUME}"
    echo ""
    
    check_root
    
    # Create log directory
    mkdir -p "$LOG_DIR" 2>/dev/null || true
    
    if [ "$RESET" -eq 1 ]; then
        unload_module
        rm -rf "$MOUNT_POINT" 2>/dev/null || true
        log_pass "Environment reset complete"
        return 0
    fi
    
    if [ "$UNMOUNT_ONLY" -eq 1 ]; then
        unmount_filesystem
        return 0
    fi
    
    # Build module
    build_module
    
    if [ "$BUILD_ONLY" -eq 1 ]; then
        log_pass "Build complete. Module ready for loading."
        log_info "  Load: sudo insmod $MODULE_FILE"
        log_info "  Mount: sudo ./start_kernel_env.sh --filers $FILERS ..."
        return 0
    fi
    
    # Load module
    load_module
    
    # Mount
    mount_filesystem
    
    # Smoke test
    smoke_test
    
    # Summary
    log_step "Kernel Environment Ready"
    echo ""
    log_pass "PowerFS kernel module active"
    log_info "  Mount point: $MOUNT_POINT"
    log_info "  Module:      $MODULE_NAME"
    log_info "  Mode:        kernel-level (powerfs_net TCP protocol)"
    echo ""
    log_info "Run integration tests:"
    log_info "  cd ${MODULE_DIR} && sudo ./test_multi_node.sh"
    log_info ""
    log_info "Check module status:"
    log_info "  lsmod | grep $MODULE_NAME"
    log_info "  dmesg | grep -i powerfs"
    log_info ""
}