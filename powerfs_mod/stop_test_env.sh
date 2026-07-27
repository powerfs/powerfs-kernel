#!/bin/bash
# =============================================================================
# PowerFS Test Environment — Stop All Test Environments
#
# Purpose:
#   Safely stop and clean up all PowerFS test environments:
#     1. Unmount kernel module mount points
#     2. Unload kernel module
#     3. Stop Docker containers (FUSE test environment)
#     4. Clean up mount points and temporary files
#
# Usage:
#   # Stop everything (kernel + FUSE containers)
#   sudo ./stop_test_env.sh
#
#   # Stop only kernel environment
#   sudo ./stop_test_env.sh --kernel-only
#
#   # Stop only FUSE containers
#   sudo ./stop_test_env.sh --fuse-only
#
#   # Stop and CLEAN ALL DATA (destructive!)
#   sudo ./stop_test_env.sh --clean
#
#   # Show current environment status without stopping
#   sudo ./stop_test_env.sh --status
#
# Environment Variables:
#   POWERFS_MODULE    Module name (default: powerfs)
#   POWERFS_MOUNT     Kernel mount point (default: /mnt/powerfs)
#   FUSE_MOUNT        FUSE mount point (default: /tmp/powerfs/test)
#   COMPOSE_FILE      Docker compose file (default: docker-compose.test.yml)
# =============================================================================

set -e

# ========== Configuration ==========
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DOCKER_DIR="${PROJECT_ROOT}/docker"

MODULE_NAME="${POWERFS_MODULE:-powerfs}"
KERNEL_MOUNT="${POWERFS_MOUNT:-/mnt/powerfs}"
FUSE_MOUNT="${FUSE_MOUNT:-/tmp/powerfs/test}"
COMPOSE_FILE="${COMPOSE_FILE:-docker-compose.test.yml}"

KERNEL_ONLY=0
FUSE_ONLY=0
CLEAN_ALL=0
SHOW_STATUS=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --kernel-only) KERNEL_ONLY=1 ;;
        --fuse-only)   FUSE_ONLY=1 ;;
        --clean)       CLEAN_ALL=1 ;;
        --status)      SHOW_STATUS=1 ;;
        --help|-h)
            echo "Usage: $0 [--kernel-only|--fuse-only|--clean|--status]"
            echo ""
            echo "Options:"
            echo "  --kernel-only  Stop kernel environment only"
            echo "  --fuse-only    Stop FUSE containers only"
            echo "  --clean        Remove all data volumes (destructive)"
            echo "  --status       Show environment status, don't stop"
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

# ========== Status Check ==========
show_current_status() {
    log_step "Environment Status"
    
    echo ""
    echo -e "${C_CYAN}Kernel Module:${C_RESET}"
    if lsmod 2>/dev/null | grep -q "$MODULE_NAME"; then
        lsmod | grep "$MODULE_NAME"
        echo -e "  ${C_GREEN}Loaded${C_RESET}"
    else
        echo -e "  ${C_YELLOW}Not loaded${C_RESET}"
    fi
    
    echo ""
    echo -e "${C_CYAN}Mount Points:${C_RESET}"
    for mp in "$KERNEL_MOUNT" "$FUSE_MOUNT"; do
        if mountpoint "$mp" 2>/dev/null; then
            local mount_type
            mount_type=$(mount | grep "$mp" | awk '{print $1, $3, $5}')
            echo -e "  ${C_GREEN}$mp${C_RESET} [$mount_type]"
        else
            echo -e "  ${C_YELLOW}$mp${C_RESET} (not mounted)"
        fi
    done
    
    echo ""
    echo -e "${C_CYAN}Docker Containers:${C_RESET}"
    if command -v docker >/dev/null 2>&1; then
        local containers
        containers=$(docker ps -a --filter "name=master-1-test|master-2-test|master-3-test|volume-1-test|volume-2-test|volume-3-test|filer-1-test|filer-2-test|filer-3-test|fuse-test|benchmark|redis-test" --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}' 2>/dev/null)
        if [ -n "$containers" ]; then
            echo "$containers" | while IFS= read -r line; do
                echo "  $line"
            done
        else
            echo -e "  ${C_YELLOW}No PowerFS test containers running${C_RESET}"
        fi
    else
        echo -e "  ${C_YELLOW}Docker not available${C_RESET}"
    fi
    echo ""
}

# ========== Kernel Cleanup ==========
cleanup_kernel() {
    log_step "Cleaning Kernel Environment"
    
    local did_something=0
    
    # 1. Unmount kernel mount point
    if mountpoint "$KERNEL_MOUNT" 2>/dev/null; then
        log_info "Unmounting $KERNEL_MOUNT..."
        umount "$KERNEL_MOUNT" 2>/dev/null || umount -f "$KERNEL_MOUNT" 2>/dev/null || umount -l "$KERNEL_MOUNT" 2>/dev/null || {
            log_error "Cannot unmount $KERNEL_MOUNT"
            log_info "  Try: sudo umount -l $KERNEL_MOUNT"
        }
        sleep 1
        did_something=1
    else
        log_info "$KERNEL_MOUNT is not mounted"
    fi
    
    # 2. Unload kernel module
    if lsmod 2>/dev/null | grep -q "$MODULE_NAME"; then
        log_info "Unloading module $MODULE_NAME..."
        
        # Try graceful unload first
        local retries=5
        local unloaded=0
        while [ "$retries" -gt 0 ]; do
            if rmmod "$MODULE_NAME" 2>/dev/null; then
                unloaded=1
                break
            fi
            sleep 2
            retries=$((retries - 1))
        done
        
        if [ "$unloaded" -eq 0 ]; then
            log_warn "Graceful unload failed, trying force..."
            rmmod -f "$MODULE_NAME" 2>/dev/null || true
            sleep 1
        fi
        
        if lsmod 2>/dev/null | grep -q "$MODULE_NAME"; then
            log_warn "Module still loaded after rmmod (may be in use)"
            log_info "  Active references can prevent unload"
            log_info "  Check: lsmod | grep $MODULE_NAME"
        else
            log_pass "Module $MODULE_NAME unloaded"
        fi
        did_something=1
    else
        log_info "Module $MODULE_NAME is not loaded"
    fi
    
    # 3. Clean stale mount point
    if [ -d "$KERNEL_MOUNT" ] && ! mountpoint "$KERNEL_MOUNT" 2>/dev/null; then
        log_info "Cleaning mount point directory $KERNEL_MOUNT..."
        rmdir "$KERNEL_MOUNT" 2>/dev/null || log_info "  (non-empty, keeping directory)"
    fi
    
    if [ "$did_something" -eq 1 ]; then
        log_pass "Kernel environment cleaned"
    else
        log_info "Kernel environment already clean"
    fi
}

# ========== FUSE Cleanup ==========
cleanup_fuse() {
    log_step "Cleaning FUSE Container Environment"
    
    if ! command -v docker >/dev/null 2>&1; then
        log_warn "Docker not available, skipping container cleanup"
        return 0
    fi
    
    # Check if compose file exists
    local compose_path="${DOCKER_DIR}/${COMPOSE_FILE}"
    if [ ! -f "$compose_path" ]; then
        log_warn "Compose file not found: $compose_path"
        log_info "  Skipping Docker cleanup"
        return 0
    fi
    
    cd "$DOCKER_DIR"
    
    # Detect compose command
    local compose_cmd
    if docker compose version >/dev/null 2>&1; then
        compose_cmd="docker compose"
    elif command -v docker-compose >/dev/null 2>&1; then
        compose_cmd="docker-compose"
    else
        log_warn "Docker Compose not available"
        return 0
    fi
    
    # Stop FUSE container first (it depends on others)
    log_info "Stopping FUSE client container..."
    $compose_cmd -f "$COMPOSE_FILE" stop fuse-test 2>/dev/null || true
    
    # Unmount FUSE mount point on host
    if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
        log_info "Unmounting FUSE mount $FUSE_MOUNT..."
        umount -f "$FUSE_MOUNT" 2>/dev/null || fusermount -u "$FUSE_MOUNT" 2>/dev/null || true
        sleep 1
    fi
    
    # Stop benchmark container
    log_info "Stopping benchmark container..."
    $compose_cmd -f "$COMPOSE_FILE" stop benchmark 2>/dev/null || true
    
    # Stop all backend services
    log_info "Stopping all backend containers..."
    $compose_cmd -f "$COMPOSE_FILE" stop 2>/dev/null || true
    sleep 2
    
    if [ "$CLEAN_ALL" -eq 1 ]; then
        log_warn "CLEAN MODE: Removing all containers and volumes..."
        $compose_cmd -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
        log_pass "All containers and volumes removed"
    else
        # Just remove containers (keep volumes for data persistence)
        log_info "Removing containers (volumes preserved)..."
        $compose_cmd -f "$COMPOSE_FILE" down --remove-orphans 2>/dev/null || true
        log_pass "Containers stopped (data volumes preserved)"
    fi
    
    # Clean mount point
    if [ -d "$FUSE_MOUNT" ]; then
        rm -rf "$FUSE_MOUNT" 2>/dev/null || true
        log_info "Cleaned FUSE mount point: $FUSE_MOUNT"
    fi
}

# ========== Full Cleanup ==========
full_cleanup() {
    log_step "Complete Environment Cleanup"
    
    echo -e "${C_RED}╔══════════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_RED}║  DESTRUCTIVE: This will remove ALL test data!          ${C_RESET}"
    echo -e "${C_RED}╚══════════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    log_info "This will:"
    log_info "  1. Unmount all PowerFS mount points"
    log_info "  2. Unload kernel module"
    log_info "  3. Stop and remove all Docker containers"
    log_info "  4. REMOVE all Docker volumes (data loss!)"
    echo ""
    
    local confirm
    read -r -p "Type 'yes' to confirm: " confirm
    if [ "$confirm" != "yes" ]; then
        log_info "Aborted"
        return 1
    fi
    
    cleanup_kernel
    C_OLD="$CLEAN_ALL"
    CLEAN_ALL=1
    cleanup_fuse
    CLEAN_ALL="$C_OLD"
    
    # Clean temp files
    rm -rf /tmp/powerfs 2>/dev/null || true
    rm -f /tmp/powerfs_*.log 2>/dev/null || true
    
    log_pass "Complete cleanup finished"
}

# ========== Main ==========
main() {
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  PowerFS Test Environment Stop & Cleanup                ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    
    if [ "$SHOW_STATUS" -eq 1 ]; then
        show_current_status
        return 0
    fi
    
    if [ "$CLEAN_ALL" -eq 1 ]; then
        full_cleanup
        return 0
    fi
    
    # Determine what to stop
    local do_kernel=1
    local do_fuse=1
    
    if [ "$KERNEL_ONLY" -eq 1 ]; then
        do_fuse=0
        log_info "Stopping kernel environment only"
    elif [ "$FUSE_ONLY" -eq 1 ]; then
        do_kernel=0
        log_info "Stopping FUSE containers only"
    else
        log_info "Stopping all test environments"
    fi
    
    if [ "$do_kernel" -eq 1 ]; then
        cleanup_kernel
    fi
    
    if [ "$do_fuse" -eq 1 ]; then
        cleanup_fuse
    fi
    
    # Final status
    show_current_status
    
    log_step "Done"
    log_pass "Test environment stopped successfully"
    echo ""
    log_info "To restart kernel environment:"
    log_info "  sudo ./start_kernel_env.sh"
    echo ""
    log_info "To restart FUSE environment:"
    log_info "  sudo ./start_fuse_env.sh --wait"
    echo ""
}

main "$@"