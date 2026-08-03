#!/bin/bash
# =============================================================================
# PowerFS Kernel Filesystem — IO500 Benchmark in QEMU
#
# Purpose:
#   Run IO500 benchmark against the PowerFS kernel module inside a QEMU VM,
#   evaluating metadata and data I/O performance under virtualized environment.
#
# Prerequisites:
#   - Docker running (for backend services: Filer, Master, Volume, Redis)
#   - QEMU installed with KVM support
#   - PowerFS kernel module compiled (powerfs.ko)
#   - IO500 binary compiled (io500/io500)
#   - SSH key-based authentication to VM
#
# Usage:
#   # Quick test (metadata + small data I/O)
#   sudo ./run_io500_qemu.sh
#
#   # Full test with custom IO500 config
#   sudo ./run_io500_qemu.sh --config config-powerfs.ini
#
#   # Skip build, reuse existing VM
#   sudo ./run_io500_qemu.sh --skip-build
#
#   # Keep VM alive after test (for debugging)
#   sudo ./run_io500_qemu.sh --keep-alive
#
#   # Custom backend address (for non-default Docker network)
#   sudo ./run_io500_qemu.sh --backend 172.20.0.35:9334
#
# Architecture:
#   ┌──────────────┐    ┌──────────────┐    ┌─────────────────┐
#   │   QEMU VM    │────│   Host SSH   │────│     Host        │
#   │  (powerfs.ko)│    │  :2223->:22  │    │  Docker Cont.   │
#   │  IO500 test  │    └──────────────┘    │  (Filer, Master) │
#   └──────┬───────┘                         └─────────────────┘
#          │ powerfs-net TCP
#          ▼
#   ┌──────────────────────────────────────────┐
#   │  Docker Network (172.20.0.0/16)          │
#   │  TAP: tap0 → br-xxx → container IPs       │
#   └──────────────────────────────────────────┘
# =============================================================================

set -e

# ========== Configuration ==========
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
IO500_DIR="${PROJECT_ROOT}/io500"
KERNEL_VM_DIR="${PROJECT_ROOT}/kernel/vm"
VM_OUTPUT_DIR="${KERNEL_VM_DIR}/output"

# IO500 configuration
IO500_CONFIG="${IO500_CONFIG:-config-powerfs-kernel-quick.ini}"
IO500_CONFIG_PATH="${IO500_DIR}/${IO500_CONFIG}"
IO500_BIN="${IO500_DIR}/io500"

# VM configuration
VM_IP="172.20.0.100"
GATEWAY="172.20.0.1"
SSH_PORT="2223"
SSH_USER="root"
VM_MEM="4096"
VM_CPUS="4"
TAP_DEVICE="tap0"

# PowerFS backend (Docker container IPs on 172.20.0.0/16 network)
BACKEND_ADDR="${BACKEND_ADDR:-172.20.0.35}"
BACKEND_PORT="${BACKEND_PORT:-9334}"
MASTER_ADDR="${MASTER_ADDR:-172.20.0.14}"
MASTER_PORT="${MASTER_PORT:-9333}"
VOLUME_ADDR="${VOLUME_ADDR:-172.20.0.24}"
VOLUME_PORT="${VOLUME_PORT:-8080}"

# IO500 mount point inside VM
IO500_MOUNT="/mnt/powerfs"
IO500_DATA="${IO500_MOUNT}/io500-data"
IO500_RESULTS="${IO500_MOUNT}/io500-results"

# Result collection
RESULT_DIR="${PROJECT_ROOT}/io500/results/qemu-kernel-$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULT_DIR"
LOG_DIR="/tmp/powerfs/io500-qemu"
mkdir -p "$LOG_DIR"

# Flags
SKIP_BUILD=0
KEEP_ALIVE=0
SKIP_BACKEND=0
DRY_RUN=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)       IO500_CONFIG="$2"; IO500_CONFIG_PATH="${IO500_DIR}/${IO500_CONFIG}"; shift ;;
        --skip-build)   SKIP_BUILD=1 ;;
        --skip-backend) SKIP_BACKEND=1 ;;
        --keep-alive)   KEEP_ALIVE=1 ;;
        --backend)      BACKEND_ADDR="$2"; shift ;;
        --dry-run)      DRY_RUN=1 ;;
        --help|-h)
            cat << 'HELPEOF'
Usage: sudo ./run_io500_qemu.sh [OPTIONS]

Options:
  --config FILE       IO500 config file (default: config-powerfs-kernel-quick.ini)
  --skip-build        Skip initramfs rebuild (reuse existing)
  --skip-backend      Skip backend startup (assume running)
  --keep-alive        Keep VM running after test (for debugging)
  --backend ADDR      Backend address (default: 172.20.0.35:9334)
  --dry-run           Show plan without executing
  --help              Show this help

Architecture:
  QEMU VM (powerfs.ko) → powerfs-net TCP → Docker backend (Filer/Master/Volume)
  Host SSH access via port forwarding (localhost:2223 → VM:22)
HELPEOF
            exit 0
            ;;
        *) echo "Unknown option: $1 (use --help)"; exit 1 ;;
    esac
    shift
done

# ========== Color Output ==========
if [ -t 1 ]; then
    C_RED='\033[0;31m'; C_GREEN='\033[0;32m'; C_YELLOW='\033[0;33m'
    C_BLUE='\033[0;34m'; C_CYAN='\033[0;36m'; C_MAGENTA='\033[0;35m'; C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_BLUE=''; C_CYAN=''; C_MAGENTA=''; C_RESET=''
fi

log_info()  { echo -e "${C_BLUE}[INFO]${C_RESET}  $(date +%H:%M:%S) $*"; }
log_pass()  { echo -e "${C_GREEN}[PASS]${C_RESET}  $*"; }
log_warn()  { echo -e "${C_YELLOW}[WARN]${C_RESET}  $*"; }
log_error() { echo -e "${C_RED}[ERROR]${C_RESET} $*"; }
log_step()  { echo -e "\n${C_CYAN}━━━ $* ━━━${C_RESET}"; }
log_result(){ echo -e "${C_MAGENTA}[RESULT]${C_RESET} $*"; }

# ========== Utility Functions ==========
check_root() {
    if [ "$EUID" -ne 0 ]; then
        log_error "This script must be run as root (for QEMU/KVM access)"
        log_info "  Try: sudo $0 $*"
        exit 1
    fi
}

check_dependencies() {
    log_step "Checking Prerequisites"
    
    local missing=()
    
    # QEMU
    if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
        missing+=("qemu-system-x86_64")
    else
        log_pass "QEMU: $(qemu-system-x86_64 --version 2>&1 | head -1)"
    fi
    
    # KVM
    if [ -e /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        log_pass "KVM acceleration available"
    else
        log_warn "KVM not available (will use TCG, slower)"
    fi
    
    # SSH
    if command -v ssh >/dev/null 2>&1; then
        log_pass "SSH client available"
    else
        missing+=("ssh")
    fi
    
    # IO500 binary
    if [ -f "$IO500_BIN" ]; then
        log_pass "IO500 binary: $IO500_BIN"
    else
        log_warn "IO500 binary not found at $IO500_BIN"
        log_info "  Build it: cd $IO500_DIR && make clean && make"
        log_info "  Or copy: cp io500/build/io500 $IO500_BIN"
    fi
    
    # IO500 config
    if [ -f "$IO500_CONFIG_PATH" ]; then
        log_pass "IO500 config: $IO500_CONFIG_PATH"
    else
        log_warn "IO500 config not found: $IO500_CONFIG_PATH"
        log_info "  Available configs:"
        ls "$IO500_DIR"/config-*.ini 2>/dev/null | while IFS= read -r f; do
            log_info "    $(basename "$f")"
        done
    fi
    
    # PowerFS kernel module
    local mod_path="${VM_OUTPUT_DIR}/powerfs.ko"
    if [ -f "$mod_path" ]; then
        log_pass "PowerFS module: $mod_path"
        local size
        size=$(stat -c '%s' "$mod_path" 2>/dev/null)
        log_info "  Size: $size bytes"
    else
        log_warn "PowerFS module not found at $mod_path"
        log_info "  Build it: cd kernel/powerfs_mod && make"
    fi
    
    # Kernel image
    if [ -f "${VM_OUTPUT_DIR}/bzImage" ]; then
        log_pass "Kernel image: ${VM_OUTPUT_DIR}/bzImage"
    else
        log_warn "Kernel image not found"
        log_info "  Build it: cd kernel/vm && ./build_kernel.sh"
    fi
    
    # initramfs
    if [ -f "${VM_OUTPUT_DIR}/initramfs.cpio.gz" ]; then
        log_pass "initramfs: ${VM_OUTPUT_DIR}/initramfs.cpio.gz"
    else
        log_warn "initramfs not found"
        log_info "  Build it: cd kernel/vm && ./build_initramfs.sh"
    fi
    
    if [ ${#missing[@]} -gt 0 ]; then
        log_error "Missing dependencies: ${missing[*]}"
        exit 1
    fi
}

# ========== Step 1: Start Backend Services ==========
start_backend() {
    if [ "$SKIP_BACKEND" -eq 1 ]; then
        log_info "Skipping backend startup (--skip-backend)"
        return 0
    fi
    
    log_step "Starting PowerFS Backend Services"
    
    local compose_file="${PROJECT_ROOT}/docker/docker-compose.test.yml"
    if [ ! -f "$compose_file" ]; then
        # Try alternative locations
        compose_file="${PROJECT_ROOT}/io500/docker-compose-test.yml"
    fi
    
    if [ ! -f "$compose_file" ]; then
        log_warn "Docker compose file not found, assuming backend is already running"
        return 0
    fi
    
    log_info "Using compose file: $compose_file"
    
    # Start services
    cd "$(dirname "$compose_file")"
    docker compose -f "$(basename "$compose_file")" up -d master-1 volume-1 filer-1 redis 2>&1 | tee "$LOG_DIR/backend_start.log"
    
    # Wait for services to be ready
    log_info "Waiting for backend services to be ready..."
    local max_wait=30
    local waited=0
    
    while [ "$waited" -lt "$max_wait" ]; do
        # Check Redis
        if timeout 1 bash -c "echo >/dev/tcp/${BACKEND_ADDR}/${MASTER_PORT}" 2>/dev/null; then
            log_pass "Backend services reachable"
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
        log_info "  Waiting... (${waited}s / ${max_wait}s)"
    done
    
    log_warn "Backend services may not be ready after ${max_wait}s"
    log_info "  Continuing anyway, mount will fail if backend unreachable"
}

# ========== Step 2: Setup Network ==========
setup_network() {
    log_step "Setting up QEMU Network"
    
    # Check TAP device
    if ip link show "$TAP_DEVICE" &>/dev/null; then
        log_info "TAP device $TAP_DEVICE already exists"
        # Ensure it's up
        ip link set "$TAP_DEVICE" up 2>/dev/null || true
        return 0
    fi
    
    # Get Docker bridge name
    local bridge
    bridge=$(docker network inspect bridge 2>/dev/null | grep -o '"Name": "[^"]*br-[^"]*"' | head -1 | cut -d'"' -f4)
    
    # If not found, try to find any Docker bridge
    if [ -z "$bridge" ]; then
        bridge=$(ip link show type bridge 2>/dev/null | grep br- | head -1 | awk '{print $2}')
    fi
    
    if [ -z "$bridge" ]; then
        # Use docker-compose network
        local compose_net
        compose_net=$(docker network ls --format '{{.Name}}' 2>/dev/null | grep -i powerfs | head -1)
        if [ -z "$compose_net" ]; then
            compose_net=$(docker network ls --format '{{.Name}}' 2>/dev/null | grep -i test | head -1)
        fi
        if [ -n "$compose_net" ]; then
            bridge=$(docker network inspect "$compose_net" 2>/dev/null | grep -o '"Name": "[^"]*br-[^"]*"' | head -1 | cut -d'"' -f4)
        fi
    fi
    
    if [ -z "$bridge" ]; then
        log_warn "Cannot find Docker bridge, using default tap setup"
        bridge="br-io500"
    fi
    
    log_info "Using bridge: $bridge"
    
    # Create TAP device
    if ip link show "$TAP_DEVICE" &>/dev/null; then
        ip link delete "$TAP_DEVICE" 2>/dev/null || true
    fi
    
    ip tuntap add dev "$TAP_DEVICE" mode tap 2>/dev/null || {
        log_error "Cannot create TAP device (need root privileges)"
        return 1
    }
    
    # Connect to bridge
    ip link set "$TAP_DEVICE" master "$bridge" 2>/dev/null || true
    ip link set "$TAP_DEVICE" up
    
    log_pass "TAP device $TAP_DEVICE created and connected to $bridge"
}

# ========== Step 3: Prepare initramfs with IO500 ==========
prepare_initramfs() {
    if [ "$SKIP_BUILD" -eq 1 ] && [ -f "${VM_OUTPUT_DIR}/initramfs.cpio.gz" ]; then
        log_info "Skipping initramfs build (--skip-build)"
        return 0
    fi
    
    log_step "Preparing initramfs with IO500"
    
    local initramfs_dir="${VM_OUTPUT_DIR}/initramfs"
    
    # Build initramfs if needed
    if [ ! -d "$initramfs_dir" ] || [ ! -f "${VM_OUTPUT_DIR}/initramfs.cpio.gz" ]; then
        log_info "Building initramfs from scratch..."
        cd "$KERNEL_VM_DIR"
        bash build_initramfs.sh 2>&1 | tee "$LOG_DIR/build_initramfs.log"
        cd "$SCRIPT_DIR"
    fi
    
    local work_dir="${VM_OUTPUT_DIR}/initramfs"
    
    if [ ! -d "$work_dir" ]; then
        log_error "initramfs directory not found after build"
        return 1
    fi
    
    # Add IO500 binary
    if [ -f "$IO500_BIN" ]; then
        log_info "Adding IO500 binary to initramfs..."
        mkdir -p "${work_dir}/io500"
        cp "$IO500_BIN" "${work_dir}/io500/"
        
        # Make executable
        chmod +x "${work_dir}/io500/io500"
        
        # Copy IO500 config
        if [ -f "$IO500_CONFIG_PATH" ]; then
            cp "$IO500_CONFIG_PATH" "${work_dir}/io500/"
            log_info "Added IO500 config: $(basename "$IO500_CONFIG_PATH")"
        fi
        
        # Copy IO500 libraries
        log_info "Copying IO500 runtime libraries..."
        local libs
        libs=$(ldd "$IO500_BIN" 2>/dev/null | awk '{print $3}' | grep -v "not found")
        for lib in $libs; do
            [ -n "$lib" ] && cp "$lib" "${work_dir}/lib/" 2>/dev/null || true
        done
        
        log_pass "IO500 binary added to initramfs"
    else
        log_warn "IO500 binary not found, skipping"
    fi
    
    # Re-pack initramfs
    log_info "Re-packing initramfs..."
    cd "$work_dir"
    sudo find . -print0 2>/dev/null | sudo cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "${VM_OUTPUT_DIR}/initramfs.cpio.gz"
    cd "$SCRIPT_DIR"
    
    local initramfs_size
    initramfs_size=$(du -h "${VM_OUTPUT_DIR}/initramfs.cpio.gz" 2>/dev/null | cut -f1)
    log_pass "initramfs repacked: ${VM_OUTPUT_DIR}/initramfs.cpio.gz ($initramfs_size)"
}

# ========== Step 4: Launch QEMU VM ==========
QEMU_PID=""

launch_qemu() {
    log_step "Launching QEMU Virtual Machine"
    
    local kernel_image="${VM_OUTPUT_DIR}/bzImage"
    local initramfs="${VM_OUTPUT_DIR}/initramfs.cpio.gz"
    local qemu_disk="${VM_OUTPUT_DIR}/qemu_disk.img"
    
    # Check for existing VM
    if pgrep -f "qemu-system.*powerfs" >/dev/null 2>&1; then
        log_warn "Existing QEMU VM detected, stopping it first..."
        pkill -f "qemu-system.*powerfs" 2>/dev/null || true
        sleep 2
    fi
    
    # Create disk if not exists
    if [ ! -f "$qemu_disk" ]; then
        log_info "Creating virtual disk (2GB)..."
        qemu-img create -f qcow2 "$qemu_disk" 2G
    fi
    
    # Build kernel command line with PowerFS backend config
    local cmdline="console=ttyS0 root=/dev/ram0 rw init=/init quiet loglevel=3"
    cmdline="${cmdline} net.ifnames=0 biosdevname=0"
    cmdline="${cmdline} powerfs_addr=${BACKEND_ADDR}"
    cmdline="${cmdline} powerfs_port=${BACKEND_PORT}"
    
    # Detect KVM
    local kvm_flag=""
    if [ -e /dev/kvm ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        kvm_flag="-enable-kvm"
        log_info "KVM acceleration enabled"
    else
        log_warn "KVM not available, using TCG (slower)"
    fi
    
    log_info "QEMU configuration:"
    log_info "  Kernel: $kernel_image"
    log_info "  initramfs: $initramfs"
    log_info "  Memory: ${VM_MEM}MB, CPUs: ${VM_CPUS}"
    log_info "  Backend: ${BACKEND_ADDR}:${BACKEND_PORT}"
    log_info "  SSH: localhost:${SSH_PORT} → VM:22"
    
    # Launch QEMU in background
    local qemu_log="${LOG_DIR}/qemu_$(date +%Y%m%d_%H%M%S).log"
    
    sudo qemu-system-x86_64 \
        ${kvm_flag} \
        -m "${VM_MEM}" \
        -smp "${VM_CPUS}" \
        -kernel "${kernel_image}" \
        -initrd "${initramfs}" \
        -append "${cmdline}" \
        -drive file="${qemu_disk}",format=qcow2,if=virtio \
        -netdev tap,id=net0,ifname=${TAP_DEVICE},script=no,downscript=no \
        -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 \
        -netdev user,id=net1,hostfwd=tcp::${SSH_PORT}-:22 \
        -device e1000,netdev=net1,mac=52:54:00:12:34:57 \
        -nographic \
        -serial mon:stdio \
        -monitor none \
        -display none \
        > "$qemu_log" 2>&1 &
    
    QEMU_PID=$!
    log_info "QEMU PID: $QEMU_PID, log: $qemu_log"
    
    # Wait for VM to boot
    log_info "Waiting for VM to boot (up to 60s)..."
    local max_wait=60
    local waited=0
    
    while [ "$waited" -lt "$max_wait" ]; do
        if ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
               -p "$SSH_PORT" "${SSH_USER}@localhost" "echo ready" >/dev/null 2>&1; then
            log_pass "VM booted and SSH ready (${waited}s)"
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
        log_info "  Waiting... (${waited}s / ${max_wait}s)"
    done
    
    log_error "VM failed to boot within ${max_wait}s"
    log_info "  Check QEMU log: $qemu_log"
    log_info "  Last 20 lines:"
    tail -20 "$qemu_log" | while IFS= read -r line; do
        log_info "    $line"
    done
    return 1
}

# ========== Step 5: Mount PowerFS in VM ==========
mount_powerfs_in_vm() {
    log_step "Mounting PowerFS Kernel Filesystem in VM"
    
    local mount_cmd
    mount_cmd="mkdir -p ${IO500_MOUNT} && mount -t powerfs -o filer=${BACKEND_ADDR}:${BACKEND_PORT},master=${MASTER_ADDR}:${MASTER_PORT},volume=${VOLUME_ADDR}:${VOLUME_PORT} none ${IO500_MOUNT}"
    
    log_info "Mount command: $mount_cmd"
    
    # Execute mount via SSH
    if ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" "$mount_cmd" 2>&1 | tee -a "$LOG_DIR/vm_mount.log"; then
        log_pass "PowerFS mounted at ${IO500_MOUNT} in VM"
    else
        local ret=${PIPESTATUS[0]}
        log_error "Mount failed (exit code: $ret)"
        log_info "  VM kernel log:"
        ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" "dmesg | tail -30" 2>/dev/null | tee -a "$LOG_DIR/vm_dmesg.log"
        return 1
    fi
    
    # Verify mount
    if ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" "mountpoint ${IO500_MOUNT} && ls ${IO500_MOUNT} >/dev/null 2>&1" 2>/dev/null; then
        log_pass "Mount verified: filesystem accessible"
    else
        log_warn "Mount verified but filesystem not responding"
    fi
}

# ========== Step 6: Run IO500 in VM ==========
run_io500_in_vm() {
    log_step "Running IO500 Benchmark in QEMU VM"
    
    local io500_dir="/io500"
    local io500_bin="${io500_dir}/io500"
    local io500_cfg="${io500_dir}/$(basename "$IO500_CONFIG")"
    
    log_info "IO500 config: $IO500_CONFIG"
    log_info "IO500 mount: ${IO500_MOUNT}"
    
    # Create test directories in VM
    ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
        "mkdir -p ${IO500_DATA} ${IO500_RESULTS}" 2>/dev/null
    
    # Verify IO500 binary exists in VM
    local verify_cmd
    verify_cmd="test -f ${io500_bin} && echo 'exists' || echo 'missing'"
    local io500_status
    io500_status=$(ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" "$verify_cmd" 2>/dev/null)
    
    if [ "$io500_status" != "exists" ]; then
        log_warn "IO500 binary not found in VM at $io500_bin"
        log_info "  Copying from host..."
        if [ -f "$IO500_BIN" ]; then
            scp -o StrictHostKeyChecking=no -P "$SSH_PORT" "$IO500_BIN" \
                "${SSH_USER}@localhost:${io500_bin}" 2>/dev/null
            scp -o StrictHostKeyChecking=no -P "$SSH_PORT" "$IO500_CONFIG_PATH" \
                "${SSH_USER}@localhost:${io500_cfg}" 2>/dev/null
            ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
                "chmod +x ${io500_bin}" 2>/dev/null
        else
            log_error "No IO500 binary available on host"
            return 1
        fi
    fi
    
    # Modify config for VM mount point
    local modified_config="${RESULT_DIR}/config-qemu-test.ini"
    cat > "$modified_config" << CFGEOF
[global]
datadir = ${IO500_DATA}
resultdir = ${IO500_RESULTS}
verbosity = 1
min_runtime = 5

[mdtest-easy]
API = POSIX
n = 100
run = TRUE

[mdtest-hard]
API = POSIX
n = 50
run = TRUE

[ior-easy]
API = POSIX
transferSize = 1m
blockSize = 100m
filePerProc = TRUE
run = TRUE

[ior-rnd4K]
API = POSIX
run = TRUE
CFGEOF
    
    # Transfer modified config to VM
    local vm_cfg="${io500_dir}/config-qemu-test.ini"
    scp -o StrictHostKeyChecking=no -P "$SSH_PORT" "$modified_config" \
        "${SSH_USER}@localhost:${vm_cfg}" 2>/dev/null
    
    # Run IO500 via SSH
    local io500_cmd
    io500_cmd="cd ${io500_dir} && ./io500 ${vm_cfg} 2>&1 | tee ${IO500_RESULTS}/io500_output.log"
    
    log_info "Executing IO500 in VM (this may take 1-5 minutes)..."
    log_info "  Command: $io500_cmd"
    
    local io500_start
    io500_start=$(date +%s)
    
    # Run IO500 with timeout
    if ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
        "timeout 600 $io500_cmd" 2>&1 | tee "$LOG_DIR/io500_run.log"; then
        local io500_end
        io500_end=$(date +%s)
        local elapsed=$((io500_end - io500_start))
        log_pass "IO500 completed in ${elapsed}s"
    else
        local ret=${PIPESTATUS[0]}
        log_error "IO500 test failed (exit code: $ret) after ${elapsed}s"
        log_info "  Check VM log: $LOG_DIR/io500_run.log"
        return 1
    fi
    
    # Copy results from VM
    log_info "Collecting results from VM..."
    mkdir -p "$RESULT_DIR"
    
    ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
        "tar czf /tmp/io500_results.tar.gz -C ${IO500_RESULTS} ." 2>/dev/null
    
    scp -o StrictHostKeyChecking=no -P "$SSH_PORT" \
        "${SSH_USER}@localhost:/tmp/io500_results.tar.gz" \
        "${RESULT_DIR}/" 2>/dev/null || true
    
    if [ -f "${RESULT_DIR}/io500_results.tar.gz" ]; then
        tar xzf "${RESULT_DIR}/io500_results.tar.gz" -C "$RESULT_DIR" 2>/dev/null
        log_pass "Results extracted to $RESULT_DIR"
    fi
    
    # Also check the log file directly
    ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
        "cat ${IO500_RESULTS}/io500_output.log" 2>/dev/null >> "${RESULT_DIR}/io500_output.log"
    
    return 0
}

# ========== Step 7: Parse and Display Results ==========
display_results() {
    log_step "IO500 Benchmark Results"
    
    local output_log="${LOG_DIR}/io500_run.log"
    local summary_file="${RESULT_DIR}/io500_summary.txt"
    
    echo "========================================"
    echo "  PowerFS Kernel — IO500 Results"
    echo "  QEMU VM (4GB RAM, 4CPUs, KVM)"
    echo "========================================"
    echo ""
    echo "Timestamp: $(date)"
    echo "Config:    $IO500_CONFIG"
    echo "Mount:     ${IO500_MOUNT} (kernel powerfs.ko)"
    echo ""
    
    local total_score="N/A"
    
    # Parse IO500 output
    if [ -f "$output_log" ]; then
        local in_section=0
        while IFS= read -r line; do
            # Check for key metrics
            if echo "$line" | grep -qi "score\|SCORE"; then
                log_result "IO500 SCORE: $line"
                total_score=$(echo "$line" | grep -oP '[\d.]+' | head -1)
            fi
            if echo "$line" | grep -qi "bandwidth\|BW\|MB/s\|GB/s"; then
                log_result "BANDWIDTH: $line"
            fi
            if echo "$line" | grep -qi "ops/sec\|op/s\|IOPS"; then
                log_result "IOPS: $line"
            fi
            if echo "$line" | grep -qi "latency\|avg\|max"; then
                log_result "LATENCY: $line"
            fi
            if echo "$line" | grep -qi "throughput\|THR"; then
                log_result "THROUGHPUT: $line"
            fi
        done < "$output_log"
        
        # Also check for summary line
        log_info "Full output:"
        echo "────────────────────────────────────────"
        cat "$output_log"
        echo "────────────────────────────────────────"
    else
        log_warn "No IO500 output found in $output_log"
    fi
    
    # Collect VM performance info
    log_info "VM performance info:"
    ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
        "echo '--- Memory ---' && free -h && echo '' && echo '--- CPU Info ---' && cat /proc/cpuinfo | grep -E 'model name|processor' | head -5" 2>/dev/null | tee -a "${RESULT_DIR}/vm_info.txt"
    
    # Create summary file
    cat > "$summary_file" << SUMMARYEOF
PowerFS Kernel Filesystem — IO500 Benchmark Summary
===================================================
Date:        $(date)
Config:      $IO500_CONFIG
VM Memory:   ${VM_MEM}MB
VM CPUs:     ${VM_CPUS}
Backend:     ${BACKEND_ADDR}:${BACKEND_PORT}
Mount Point: ${IO500_MOUNT}

Results Directory: $RESULT_DIR
Log Directory:     $LOG_DIR
SUMMARYEOF
    
    log_pass "Summary saved to: $summary_file"
}

# ========== Step 8: Cleanup ==========
cleanup() {
    if [ "$KEEP_ALIVE" -eq 1 ]; then
        log_step "Keeping VM alive (--keep-alive)"
        log_info "  SSH access: ssh -p $SSH_PORT ${SSH_USER}@localhost"
        log_info "  PowerFS mount: $IO500_MOUNT"
        log_info "  Press Ctrl+C or kill QEMU to stop"
        return 0
    fi
    
    log_step "Cleaning Up"
    
    # Unmount PowerFS in VM
    if ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
        "mountpoint ${IO500_MOUNT} 2>/dev/null" 2>/dev/null; then
        log_info "Unmounting PowerFS in VM..."
        ssh -o StrictHostKeyChecking=no -p "$SSH_PORT" "${SSH_USER}@localhost" \
            "umount ${IO500_MOUNT} 2>/dev/null || umount -l ${IO500_MOUNT} 2>/dev/null || true" 2>/dev/null
    fi
    
    # Stop QEMU
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        log_info "Stopping QEMU (PID: $QEMU_PID)..."
        kill "$QEMU_PID" 2>/dev/null || true
        sleep 2
        kill -9 "$QEMU_PID" 2>/dev/null || true
        log_pass "QEMU stopped"
    else
        # Try to kill by name
        pkill -f "qemu-system.*powerfs" 2>/dev/null || true
        log_info "QEMU process cleaned up"
    fi
    
    # Clean up TAP
    if [ "$SKIP_BACKEND" -eq 0 ]; then
        log_info "TAP device $TAP_DEVICE left intact (backend may still be running)"
    else
        if ip link show "$TAP_DEVICE" &>/dev/null; then
            ip link delete "$TAP_DEVICE" 2>/dev/null || true
            log_info "TAP device removed"
        fi
    fi
    
    log_pass "Cleanup complete"
}

# ========== Main ==========
main() {
    local start_time
    start_time=$(date +%s)
    
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  PowerFS Kernel — IO500 Benchmark in QEMU                 ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo -e "  ${C_BLUE}IO500 Config:${C_RESET}    $IO500_CONFIG"
    echo -e "  ${C_BLUE}Backend:${C_RESET}         ${BACKEND_ADDR}:${BACKEND_PORT}"
    echo -e "  ${C_BLUE}Mount Point:${C_RESET}     $IO500_MOUNT"
    echo -e "  ${C_BLUE}VM:${C_RESET}              ${VM_MEM}MB RAM, ${VM_CPUS} CPUs, KVM"
    echo -e "  ${C_BLUE}Result Dir:${C_RESET}      $RESULT_DIR"
    echo ""
    
    check_root
    check_dependencies
    
    if [ "$DRY_RUN" -eq 1 ]; then
        log_info "Dry run mode - would execute:"
        log_info "  1. Start backend (docker compose)"
        log_info "  2. Setup TAP network"
        log_info "  3. Prepare initramfs with IO500"
        log_info "  4. Launch QEMU VM"
        log_info "  5. Mount PowerFS in VM"
        log_info "  6. Run IO500 benchmark"
        log_info "  7. Collect and display results"
        log_info "  8. Cleanup"
        exit 0
    fi
    
    # Step 1: Start backend
    start_backend
    
    # Step 2: Setup network
    setup_network
    
    # Step 3: Prepare initramfs
    prepare_initramfs
    
    # Step 4: Launch QEMU
    launch_qemu || {
        log_error "Failed to launch QEMU VM"
        cleanup
        exit 1
    }
    
    # Step 5: Mount PowerFS
    mount_powerfs_in_vm || {
        log_error "Failed to mount PowerFS in VM"
        cleanup
        exit 1
    }
    
    # Step 6: Run IO500
    run_io500_in_vm || {
        log_error "IO500 test failed"
        cleanup
        exit 1
    }
    
    # Step 7: Display results
    display_results
    
    # Calculate elapsed time
    local end_time
    end_time=$(date +%s)
    local elapsed=$((end_time - start_time))
    log_info "Total time: ${elapsed}s"
    
    # Step 8: Cleanup
    cleanup
    
    echo ""
    echo -e "${C_GREEN}╔══════════════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_GREEN}║  Benchmark Complete                                      ${C_RESET}"
    echo -e "${C_GREEN}╚══════════════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo "Results: $RESULT_DIR"
    echo "Logs:    $LOG_DIR"
    echo ""
}

# Trap for cleanup
trap 'log_error "Interrupted!"; cleanup; exit 130' INT TERM

main