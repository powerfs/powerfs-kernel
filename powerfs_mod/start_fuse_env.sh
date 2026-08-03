#!/bin/bash
# =============================================================================
# PowerFS — Fast FUSE Test Environment Startup
#
# This script starts pre-built Docker containers for PowerFS testing.
# Images MUST be pre-built first with:
#   sudo ./build_powerfs_image.sh
#
# Usage:
#   sudo ./start_fuse_env.sh              # Quick start (fire & forget)
#   sudo ./start_fuse_env.sh --wait       # Start + wait for all services ready
#   sudo ./start_fuse_env.sh --backend-only # Start backend only (no FUSE)
#   sudo ./start_fuse_env.sh --rebuild    # Build images first, then start
#   sudo ./start_fuse_env.sh --clean      # Clean volumes, then start fresh
#   sudo ./start_fuse_env.sh --logs       # Start + tail FUSE logs
#
# Architecture:
#   Wave 1: Redis (foundation)
#   Wave 2: Masters (Raft consensus)
#   Wave 3: Volumes + Filers (data + metadata)
#   Wave 4: FUSE client + Benchmark
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DOCKER_DIR="${PROJECT_ROOT}/docker"

COMPOSE_FILE="${COMPOSE_FILE:-docker-compose.test.yml}"
COMPOSE_WORKDIR="${COMPOSE_WORKDIR:-$DOCKER_DIR}"
FUSE_MOUNT="${FUSE_MOUNT:-/tmp/powerfs/test}"
LOG_DIR="${LOG_DIR:-/tmp/powerfs/logs}"

WAIT_FOR_HEALTH=0
BUILD_FIRST=0
BACKEND_ONLY=0
CLEAN_START=0
TAIL_LOGS=0
START_TIMEOUT="${START_TIMEOUT:-180}"   # Total timeout for --wait mode

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wait)         WAIT_FOR_HEALTH=1 ;;
        --rebuild|--build) BUILD_FIRST=1 ;;
        --backend-only) BACKEND_ONLY=1 ;;
        --clean)        CLEAN_START=1 ;;
        --logs)         TAIL_LOGS=1 ;;
        --help|-h)
            echo "Usage: $0 [--wait] [--rebuild] [--backend-only] [--clean] [--logs]"
            echo ""
            echo "Options:"
            echo "  --wait           Wait for all services healthy (default 180s timeout)"
            echo "  --rebuild        Build Docker images before starting"
            echo "  --backend-only   Start backend only (Redis+Masters+Volumes+Filers)"
            echo "  --clean          Clean all volumes before fresh start"
            echo "  --logs           Tail FUSE client logs after startup"
            echo ""
            echo "Quick start (after initial build):"
            echo "  sudo ./build_powerfs_image.sh    # First time: build images"
            echo "  sudo ./start_fuse_env.sh --wait   # Start and wait for ready"
            echo ""
            echo "Environment Variables:"
            echo "  FUSE_MOUNT       Host mount point (default: /tmp/powerfs/test)"
            echo "  START_TIMEOUT    Max seconds to wait (default: 180)"
            echo "  COMPOSE_FILE     Compose filename (default: docker-compose.test.yml)"
            exit 0
            ;;
        *) echo "Unknown option: $1 (use --help)"; exit 1 ;;
    esac
    shift
done

# Color output
if [ -t 1 ]; then
    R='\033[0;31m'; G='\033[0;32m'; Y='\033[0;33m'
    B='\033[0;34m'; C='\033[0;36m'; W='\033[1m'; N='\033[0m'
else
    R=''; G=''; Y=''; B=''; C=''; W=''; N=''
fi
log_info()  { echo -e "${B}[INFO]${N}  $(date +%H:%M:%S) $*"; }
log_pass()  { echo -e "${G}[PASS]${N}  $*"; }
log_warn()  { echo -e "${Y}[WARN]${N}  $*"; }
log_error() { echo -e "${R}[ERROR]${N} $*"; }
log_step()  { echo -e "\n${C}${W}━━━ $* ━━━${N}"; }

# Safe tee: only writes to log file if LOG_DIR is writable
SAFETEE() {
    if [ -n "$LOG_DIR" ] && [ -w "$LOG_DIR" ]; then
        tee -a "$LOG_DIR/$1"
    else
        cat
    fi
}

# ========== Pre-flight ==========
preflight() {
    log_step "Pre-flight Checks"
    
    if ! command -v docker >/dev/null 2>&1; then
        log_error "Docker not found."
        exit 1
    fi
    log_pass "Docker: $(docker --version)"
    
    if docker compose version >/dev/null 2>&1; then
        COMPOSE_CMD="docker compose"
    elif command -v docker-compose >/dev/null 2>&1; then
        COMPOSE_CMD="docker-compose"
    else
        log_error "Docker Compose not found."
        exit 1
    fi
    log_pass "Compose: $(docker compose version 2>/dev/null || docker-compose --version)"
    
    if [ ! -f "${COMPOSE_WORKDIR}/${COMPOSE_FILE}" ]; then
        log_error "Compose file not found: ${COMPOSE_WORKDIR}/${COMPOSE_FILE}"
        exit 1
    fi
    log_pass "Compose file: ${COMPOSE_FILE}"
    
    # Check /dev/fuse
    if [ -e /dev/fuse ]; then
        log_pass "/dev/fuse available"
    else
        log_warn "/dev/fuse not found. Run: sudo modprobe fuse"
    fi
    
    # Ensure mount point and log dir
    mkdir -p "$FUSE_MOUNT" 2>/dev/null || true
    if ! mkdir -p "$LOG_DIR" 2>/dev/null; then
        LOG_DIR="/tmp/powerfs_logs_$(date +%s)"
        mkdir -p "$LOG_DIR" 2>/dev/null || true
        log_warn "Cannot write to $LOG_DIR, using $LOG_DIR instead"
    fi
    
    # Check if images exist
    if ! docker image inspect powerfs:latest >/dev/null 2>&1; then
        echo ""
        log_error "Docker image 'powerfs:latest' not found!"
        echo ""
        log_info "  Build it first:"
        log_info "    sudo ./build_powerfs_image.sh"
        echo ""
        log_info "  Or use --rebuild to build and start:"
        log_info "    sudo ./start_fuse_env.sh --rebuild"
        echo ""
        exit 1
    fi
    log_pass "Base image 'powerfs:latest' found"
    
    if [ "$BACKEND_ONLY" -eq 0 ] && ! docker image inspect powerfs-test:latest >/dev/null 2>&1; then
        log_warn "Image 'powerfs-test:latest' not found (needed for FUSE client)"
        log_info "  Build it: sudo ./build_powerfs_image.sh"
    fi
}

# ========== Cleanup ==========
cleanup() {
    log_step "Cleaning Environment"
    
    cd "$COMPOSE_WORKDIR"
    
    # Stop containers
    log_info "Stopping existing containers..."
    $COMPOSE_CMD -f "$COMPOSE_FILE" down --remove-orphans 2>/dev/null || true
    
    # Clean FUSE mount
    if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
        log_info "Unmounting FUSE..."
        umount -f "$FUSE_MOUNT" 2>/dev/null || fusermount -u "$FUSE_MOUNT" 2>/dev/null || true
    fi
    rm -rf "$FUSE_MOUNT" 2>/dev/null || true
    sleep 1
    
    if [ "$CLEAN_START" -eq 1 ]; then
        log_info "Removing all data volumes..."
        $COMPOSE_CMD -f "$COMPOSE_FILE" down -v --remove-orphans 2>/dev/null || true
    fi
    
    log_pass "Environment cleaned"
}

# ========== Build (if needed) ==========
maybe_build() {
    if [ "$BUILD_FIRST" -eq 1 ]; then
        log_step "Building Images (--rebuild)"
        cd "$SCRIPT_DIR"
        bash "$SCRIPT_DIR/build_powerfs_image.sh" --force
    fi
}

# ========== Health Check ==========
# Poll a single service until healthy or timeout (with progress output)
wait_service() {
    local service="$1"
    local timeout="${2:-30}"
    local interval="${3:-1}"
    local elapsed=0
    
    log_info "Waiting for '$service' to be ready (timeout: ${timeout}s)..."
    
    while [ "$elapsed" -lt "$timeout" ]; do
        local status
        status=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps "$service" --format json 2>/dev/null \
            | grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4)
        
        local container_id
        container_id=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps -q "$service" 2>/dev/null)
        
        if [ "$status" = "healthy" ]; then
            printf "\r  ${G}✓${N} %s ready (%ds)\n" "$service" "$elapsed"
            return 0
        fi
        
        # Check if container is running at all
        if [ -n "$container_id" ]; then
            local container_state
            container_state=$(docker inspect --format '{{.State.Status}}' "$container_id" 2>/dev/null)
            if [ "$container_state" = "exited" ] || [ "$container_state" = "dead" ]; then
                printf "\n"
                log_error "  $service: container exited (check logs)"
                return 1
            fi
        fi
        
        # Progress indicator
        printf "\r  ${C}⋯${N} %s waiting... (%ds)  " "$service" "$elapsed"
        
        sleep "$interval"
        elapsed=$((elapsed + interval))
    done
    printf "\n"
    
    log_warn "Timeout waiting for '$service' (${timeout}s)"
    return 1
}

# Wait for multiple services with parallel polling
wait_services() {
    local timeout="$1"
    shift
    local services=("$@")
    local total=${#services[@]}
    local elapsed=0
    
    log_info "Waiting for ${total} services (timeout: ${timeout}s)..."
    
    while [ "$elapsed" -lt "$timeout" ]; do
        local healthy=0
        local unhealthy=""
        
        for svc in "${services[@]}"; do
            local status
            status=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps "$svc" --format json 2>/dev/null \
                | grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4)
            
            if [ "$status" = "healthy" ]; then
                healthy=$((healthy + 1))
            else
                unhealthy="$unhealthy $svc"
            fi
        done
        
        if [ "$healthy" -eq "$total" ]; then
            log_pass "All $total services healthy (${elapsed}s)"
            return 0
        fi
        
        # Progress indicator
        printf "\r  ${G}[%02d/%02d]${N} %d/%d healthy (%ds)  " \
            "$healthy" "$total" "$healthy" "$total" "$elapsed"
        
        sleep 2
        elapsed=$((elapsed + 2))
    done
    echo ""  # New line after progress
    
    log_warn "Timeout waiting for services. Healthy: $healthy/$total"
    log_info "  Unhealthy:$unhealthy"
    return 1
}

# ========== Wave Start ==========
start_wave1_redis() {
    log_step "Wave 1: Starting Redis"
    
    cd "$COMPOSE_WORKDIR"
    $COMPOSE_CMD -f "$COMPOSE_FILE" up -d redis 2>&1 | SAFETEE startup.log
    
    if [ "$WAIT_FOR_HEALTH" -eq 1 ]; then
        wait_service "redis" 30 2 || {
            log_error "Redis failed to become healthy"
            return 1
        }
    else
        sleep 3
    fi
    log_pass "Redis ready"
}

start_wave2_masters() {
    log_step "Wave 2: Starting Masters (Raft)"
    
    cd "$COMPOSE_WORKDIR"
    $COMPOSE_CMD -f "$COMPOSE_FILE" up -d master-1 master-2 master-3 2>&1 | SAFETEE startup.log
    
    if [ "$WAIT_FOR_HEALTH" -eq 1 ]; then
        # Wait only for master-1 (leader will be elected eventually)
        wait_service "master-1" 45 || {
            log_warn "master-1 not healthy yet"
        }
    else
        sleep 10
    fi
    log_pass "Masters started"
}

start_wave3_data() {
    log_step "Wave 3: Starting Volumes + Filers"
    
    cd "$COMPOSE_WORKDIR"
    $COMPOSE_CMD -f "$COMPOSE_FILE" up -d \
        volume-1 volume-2 volume-3 \
        filer-1 filer-2 filer-3 2>&1 | SAFETEE startup.log
    
    if [ "$WAIT_FOR_HEALTH" -eq 1 ]; then
        # Wait for primary services, skip waiting for all replicas
        wait_services 60 volume-1 filer-1 || {
            log_warn "Primary data services not healthy yet"
        }
    else
        sleep 15
    fi
    log_pass "Data services started"
}

start_wave4_fuse() {
    if [ "$BACKEND_ONLY" -eq 1 ]; then
        log_info "Skipping FUSE client (--backend-only)"
        return 0
    fi
    
    # Wait for minimum required services before starting FUSE
    if [ "$WAIT_FOR_HEALTH" -eq 1 ]; then
        log_info "Ensuring minimum backend services are ready..."
        wait_services 45 master-1 volume-1 filer-1 || {
            log_warn "Minimum backend services not all healthy"
        }
    fi
    
    log_step "Wave 4: Starting FUSE Client"
    
    cd "$COMPOSE_WORKDIR"
    
    # Clean stale mount
    if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
        log_info "Cleaning stale FUSE mount..."
        umount -f "$FUSE_MOUNT" 2>/dev/null || fusermount -u "$FUSE_MOUNT" 2>/dev/null || true
        sleep 1
    fi
    mkdir -p "$FUSE_MOUNT"
    
    $COMPOSE_CMD -f "$COMPOSE_FILE" up -d fuse-test benchmark 2>&1 | SAFETEE startup.log
    
    if [ "$WAIT_FOR_HEALTH" -eq 1 ]; then
        log_info "Waiting for FUSE container to be healthy (timeout: ${START_TIMEOUT}s)..."
        local elapsed=0
        local check_interval=2
        
        while [ "$elapsed" -lt "$START_TIMEOUT" ]; do
            # Check FUSE container health via Docker healthcheck
            local fuse_status
            fuse_status=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps fuse-test --format json 2>/dev/null \
                | grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4)
            
            if [ "$fuse_status" = "healthy" ]; then
                printf "\r  ${G}✓${N} FUSE container healthy (%ds)\n" "$elapsed"
                return 0
            fi
            
            printf "\r  ${C}⋯${N} Waiting for FUSE container... (%ds) [status: %s]  " "$elapsed" "${fuse_status:-starting}"
            
            if [ "$fuse_status" = "unhealthy" ]; then
                echo ""
                log_warn "FUSE container unhealthy, checking logs..."
                $COMPOSE_CMD -f "$COMPOSE_FILE" logs fuse-test --tail 10 2>/dev/null || true
            fi
            
            sleep "$check_interval"
            elapsed=$((elapsed + check_interval))
        done
        printf "\n"
        log_warn "Timeout waiting for FUSE container"
        log_info "  Check: $COMPOSE_CMD -f $COMPOSE_FILE ps"
        log_info "  Logs:  $COMPOSE_CMD -f $COMPOSE_FILE logs fuse-test"
        return 1
    else
        sleep 10
        if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
            log_pass "FUSE mount detected at $FUSE_MOUNT"
        else
            log_info "FUSE mount not yet ready (check later with: mountpoint $FUSE_MOUNT)"
        fi
    fi
}

# ========== Status Report ==========
show_status() {
    log_step "Environment Status"
    
    cd "$COMPOSE_WORKDIR"
    
    echo ""
    $COMPOSE_CMD -f "$COMPOSE_FILE" ps 2>/dev/null | SAFETEE status.log
    echo ""
    
    # FUSE mount
    if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
        log_pass "FUSE mounted at: $FUSE_MOUNT"
        log_info "  Test: ls $FUSE_MOUNT"
    else
        if [ "$BACKEND_ONLY" -eq 0 ]; then
            log_warn "FUSE not mounted yet (may still be starting)"
        fi
    fi
    
    # Container quick health summary
    echo ""
    log_info "Quick health summary:"
    for svc in redis master-1 master-2 master-3 volume-1 volume-2 volume-3 filer-1 filer-2 filer-3; do
        local status
        status=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps "$svc" --format json 2>/dev/null \
            | grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4)
        local state
        [ "$status" = "healthy" ] && state="${G}OK${N}" || state="${Y}--${N}"
        echo -e "  $svc: $state"
    done
    
    if [ "$BACKEND_ONLY" -eq 0 ]; then
        local fuse_status
        fuse_status=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps fuse-test --format json 2>/dev/null \
            | grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4)
        local state
        [ "$fuse_status" = "healthy" ] && state="${G}OK${N}" || state="${Y}--${N}"
        echo -e "  fuse-test: $state"
    fi
    
    # Commands
    echo ""
    log_info "Useful commands:"
    log_info "  List:     $COMPOSE_CMD -f $COMPOSE_FILE ps"
    log_info "  Logs:     $COMPOSE_CMD -f $COMPOSE_FILE logs -f <service>"
    log_info "  FUSE sh:  $COMPOSE_CMD -f $COMPOSE_FILE exec fuse-test bash"
    log_info "  Bench sh: $COMPOSE_CMD -f $COMPOSE_FILE exec benchmark bash"
    log_info "  Stop:     sudo ./stop_test_env.sh"
    log_info "  Kernel:   sudo ./start_kernel_env.sh"
    echo ""
}

# ========== Main ==========
main() {
    echo ""
    echo -e "${C}${W}╔══════════════════════════════════════════════════════════╗${N}"
    echo -e "${C}${W}║  PowerFS Fast Test Environment Startup                  ${N}"
    echo -e "${C}${W}╚══════════════════════════════════════════════════════════╝${N}"
    echo ""
    echo -e "  ${B}Compose:${N}    ${COMPOSE_WORKDIR}/${COMPOSE_FILE}"
    echo -e "  ${B}Mount:${N}      ${FUSE_MOUNT}"
    echo -e "  ${B}Wait mode:${N}  $([ "$WAIT_FOR_HEALTH" -eq 1 ] && echo 'yes (timeout: '${START_TIMEOUT}'s)' || echo 'no (fire & forget)')"
    echo -e "  ${B}Backend-only:${N} $([ "$BACKEND_ONLY" -eq 1 ] && echo 'yes' || echo 'no')"
    echo ""
    
    # Step 1: Pre-flight
    preflight
    
    # Step 2: Clean if requested
    if [ "$CLEAN_START" -eq 1 ]; then
        cleanup
    fi
    
    # Step 3: Build if requested
    maybe_build
    
    # Step 4: Start services in waves
    local start_time
    start_time=$(date +%s)
    
    start_wave1_redis || true
    start_wave2_masters || true
    start_wave3_data || true
    start_wave4_fuse || true
    
    local end_time
    end_time=$(date +%s)
    
    echo -e "\n${G}${W}━━━ Startup completed in $((end_time - start_time))s ━━━${N}"
    
    # Step 5: Show status
    show_status
    
    # Step 6: Tail logs if requested
    if [ "$TAIL_LOGS" -eq 1 ] && [ "$BACKEND_ONLY" -eq 0 ]; then
        log_info "Tailing FUSE client logs (Ctrl+C to exit)..."
        cd "$COMPOSE_WORKDIR"
        $COMPOSE_CMD -f "$COMPOSE_FILE" logs -f fuse-test 2>/dev/null || true
    fi
}

main "$@"