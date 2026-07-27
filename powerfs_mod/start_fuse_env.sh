#!/bin/bash
# =============================================================================
# PowerFS Test Environment — FUSE Containerized Environment Startup
#
# Purpose:
#   Build and start the complete PowerFS FUSE test environment using
#   docker-compose.test.yml. This includes:
#     - Redis (metadata store)
#     - 3x Master nodes (Raft consensus)
#     - 3x Volume nodes (data shards)
#     - 3x Filer nodes (metadata + routing)
#     - 1x FUSE client (mounted at /mnt/powerfs inside container)
#     - 1x Benchmark container (for running tests)
#
# Prerequisites:
#   - Docker and Docker Compose installed
#   - PowerFS binaries compiled (cargo build --release)
#   - Port availability: 6380, 9433-9435, 8180-8182, 8191-8193,
#     8988-8993, 9344-9345
#   - /dev/fuse available on host
#
# Usage:
#   # Start full FUSE test environment
#   sudo ./start_fuse_env.sh
#
#   # Start and wait for health check (default 120s timeout)
#   sudo ./start_fuse_env.sh --wait
#
#   # Rebuild images first
#   sudo ./start_fuse_env.sh --build
#
#   # Skip FUSE client (backend only)
#   sudo ./start_fuse_env.sh --backend-only
#
#   # Force restart (clean state)
#   sudo ./start_fuse_env.sh --clean
#
#   # Tail FUSE logs after startup
#   sudo ./start_fuse_env.sh --logs
#
# Environment Variables:
#   COMPOSE_FILE   Override compose file (default: docker/docker-compose.test.yml)
#   COMPOSE_WORKDIR Docker compose working directory (default: project root)
#   FUSE_MOUNT     Host mount point for FUSE (default: /tmp/powerfs/test)
#   LOG_DIR        Log directory (default: /tmp/powerfs/logs)
#   BUILD_ONLY     Set to 1 to only build, not start
# =============================================================================

set -e

# ========== Configuration ==========
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

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wait)       WAIT_FOR_HEALTH=1 ;;
        --build)      BUILD_FIRST=1 ;;
        --backend-only) BACKEND_ONLY=1 ;;
        --clean)      CLEAN_START=1 ;;
        --logs)       TAIL_LOGS=1 ;;
        --help|-h)
            echo "Usage: $0 [--wait] [--build] [--backend-only] [--clean] [--logs]"
            echo ""
            echo "Options:"
            echo "  --wait         Wait for all services to become healthy (120s timeout)"
            echo "  --build        Rebuild Docker images before starting"
            echo "  --backend-only Start only backend (no FUSE client)"
            echo "  --clean        Clean all data volumes before starting"
            echo "  --logs         Tail FUSE client logs after startup"
            exit 0
            ;;
        *) echo "Unknown option: $1 (use --help)"; exit 1 ;;
    esac
    shift
done

# ========== Color Output ==========
if [ -t 1 ]; then
    C_RED='\033[0;31m'
    C_GREEN='\033[0;32m'
    C_YELLOW='\033[0;33m'
    C_BLUE='\033[0;34m'
    C_CYAN='\033[0;36m'
    C_RESET='\033[0m'
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_BLUE=''; C_CYAN=''; C_RESET=''
fi

log_info()  { echo -e "${C_BLUE}[INFO]${C_RESET}  $(date +%H:%M:%S) $*"; }
log_pass()  { echo -e "${C_GREEN}[PASS]${C_RESET}  $*"; }
log_warn()  { echo -e "${C_YELLOW}[WARN]${C_RESET}  $*"; }
log_error() { echo -e "${C_RED}[ERROR]${C_RESET} $*"; }
log_step()  { echo -e "\n${C_CYAN}━━━ $* ━━━${C_RESET}"; }

# ========== Pre-flight Checks ==========
check_prerequisites() {
    log_step "Pre-flight Checks"
    
    # Check docker
    if ! command -v docker >/dev/null 2>&1; then
        log_error "Docker not found. Install Docker first."
        exit 1
    fi
    log_pass "Docker available: $(docker --version 2>/dev/null)"
    
    # Check docker compose
    if docker compose version >/dev/null 2>&1; then
        COMPOSE_CMD="docker compose"
        log_pass "Docker Compose (plugin): $(docker compose version 2>/dev/null)"
    elif command -v docker-compose >/dev/null 2>&1; then
        COMPOSE_CMD="docker-compose"
        log_pass "docker-compose: $(docker-compose --version 2>/dev/null)"
    else
        log_error "Docker Compose not found. Install docker compose plugin or docker-compose."
        exit 1
    fi
    
    # Check compose file
    if [ ! -f "${COMPOSE_WORKDIR}/${COMPOSE_FILE}" ]; then
        log_error "Compose file not found: ${COMPOSE_WORKDIR}/${COMPOSE_FILE}"
        log_error "  Expected location: ${COMPOSE_WORKDIR}"
        log_error "  Current dir: $(pwd)"
        exit 1
    fi
    log_pass "Compose file found: ${COMPOSE_FILE}"
    
    # Check /dev/fuse
    if [ -e /dev/fuse ]; then
        log_pass "/dev/fuse available"
    else
        log_warn "/dev/fuse not found on host. FUSE client may not work."
        log_info "  Load fuse module: sudo modprobe fuse"
    fi
    
    # Check mount point directory
    mkdir -p "$FUSE_MOUNT" 2>/dev/null || {
        log_warn "Cannot create mount point $FUSE_MOUNT (may need sudo)"
    }
    mkdir -p "$LOG_DIR" 2>/dev/null || true
}

# ========== Cleanup ==========
clean_environment() {
    log_step "Cleaning Environment"
    
    cd "$COMPOSE_WORKDIR"
    
    log_info "Stopping all containers..."
    $COMPOSE_CMD -f "$COMPOSE_FILE" down --volumes --remove-orphans 2>/dev/null || true
    sleep 2
    
    log_info "Removing FUSE mount point..."
    if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
        umount -f "$FUSE_MOUNT" 2>/dev/null || fusermount -u "$FUSE_MOUNT" 2>/dev/null || true
    fi
    rm -rf "$FUSE_MOUNT" 2>/dev/null || true
    
    log_pass "Environment cleaned"
}

# ========== Build ==========
build_images() {
    log_step "Building Docker Images"
    
    cd "$COMPOSE_WORKDIR"
    
    log_info "Building PowerFS images (this may take a while)..."
    log_info "  Context: ${PROJECT_ROOT}"
    log_info "  Dockerfile: docker/Dockerfile"
    
    $COMPOSE_CMD -f "$COMPOSE_FILE" build ${BUILD_SERVICES:+--services $BUILD_SERVICES} 2>&1 | tee "$LOG_DIR/build.log"
    
    if [ "${PIPESTATUS[0]}" -eq 0 ]; then
        log_pass "Docker images built successfully"
    else
        log_error "Docker build failed. See $LOG_DIR/build.log"
        log_error "  Try: cd ${PROJECT_ROOT} && cargo build --release"
        exit 1
    fi
}

# ========== Start Services ==========
start_backend() {
    log_step "Starting Backend Services"
    
    cd "$COMPOSE_WORKDIR"
    
    # Start backend (Redis, Masters, Volumes, Filers)
    log_info "Starting Redis + Masters + Volumes + Filers..."
    
    local backend_services="redis master-1 master-2 master-3 volume-1 volume-2 volume-3 filer-1 filer-2 filer-3"
    
    $COMPOSE_CMD -f "$COMPOSE_FILE" up -d $backend_services 2>&1 | tee -a "$LOG_DIR/startup.log"
    
    log_info "Waiting for backend to be ready (30s)..."
    sleep 30
    
    # Check backend health
    local healthy=0
    local total=9
    local timeout=30
    
    for service in redis master-1 master-2 master-3 volume-1 volume-2 volume-3 filer-1 filer-2 filer-3; do
        local status
        status=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps "$service" --format json 2>/dev/null | grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4)
        if [ "$status" = "healthy" ]; then
            ((healthy++))
        else
            local container
            container=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps -q "$service" 2>/dev/null)
            if [ -n "$container" ]; then
                # Give more time
                sleep 5
                status=$($COMPOSE_CMD -f "$COMPOSE_FILE" ps "$service" --format json 2>/dev/null | grep -o '"Health":"[^"]*"' | head -1 | cut -d'"' -f4)
                [ "$status" = "healthy" ] && ((healthy++))
            fi
        fi
    done
    
    log_info "Backend health: $healthy/$total services healthy"
    
    if [ "$healthy" -lt 9 ]; then
        log_warn "Some backend services not healthy yet"
        log_info "  Check: $COMPOSE_CMD -f $COMPOSE_FILE ps"
        log_info "  Logs:  $COMPOSE_CMD -f $COMPOSE_FILE logs <service>"
    else
        log_pass "All backend services healthy"
    fi
}

start_fuse_client() {
    log_step "Starting FUSE Client"
    
    if [ "$BACKEND_ONLY" -eq 1 ]; then
        log_info "Skipping FUSE client (--backend-only mode)"
        return 0
    fi
    
    cd "$COMPOSE_WORKDIR"
    
    # Clean any stale mount first
    if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
        log_info "Unmounting stale FUSE mount..."
        umount -f "$FUSE_MOUNT" 2>/dev/null || fusermount -u "$FUSE_MOUNT" 2>/dev/null || true
    fi
    mkdir -p "$FUSE_MOUNT"
    
    log_info "Starting FUSE client container..."
    $COMPOSE_CMD -f "$COMPOSE_FILE" up -d fuse-test 2>&1 | tee -a "$LOG_DIR/startup.log"
    
    if [ "$WAIT_FOR_HEALTH" -eq 1 ]; then
        log_info "Waiting for FUSE mount to be ready (120s timeout)..."
        local waited=0
        while [ "$waited" -lt 120 ]; do
            if mountpoint "$FUSE_MOUNT" 2>/dev/null && ls "$FUSE_MOUNT" >/dev/null 2>&1; then
                log_pass "FUSE mount ready at $FUSE_MOUNT"
                return 0
            fi
            sleep 3; waited=$((waited + 3))
        done
        log_warn "Timeout waiting for FUSE mount. Check container logs:"
        log_info "  $COMPOSE_CMD -f $COMPOSE_FILE logs fuse-test"
    else
        sleep 15
        if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
            log_pass "FUSE mount detected at $FUSE_MOUNT"
        else
            log_warn "FUSE mount not detected. Check container status:"
            log_info "  $COMPOSE_CMD -f $COMPOSE_FILE ps fuse-test"
        fi
    fi
}

start_benchmark() {
    log_step "Starting Benchmark Container"
    
    cd "$COMPOSE_WORKDIR"
    
    if [ "$BACKEND_ONLY" -eq 1 ]; then
        return 0
    fi
    
    log_info "Starting benchmark container..."
    $COMPOSE_CMD -f "$COMPOSE_FILE" up -d benchmark 2>&1 | tee -a "$LOG_DIR/startup.log"
    
    log_info "Benchmark container ready. Connect with:"
    log_info "  $COMPOSE_CMD -f $COMPOSE_FILE exec benchmark bash"
}

# ========== Status Report ==========
show_status() {
    log_step "Environment Status"
    
    cd "$COMPOSE_WORKDIR"
    
    echo ""
    $COMPOSE_CMD -f "$COMPOSE_FILE" ps 2>/dev/null | tee -a "$LOG_DIR/status.log"
    echo ""
    
    # FUSE mount status
    if mountpoint "$FUSE_MOUNT" 2>/dev/null; then
        log_pass "FUSE mounted at: $FUSE_MOUNT"
        log_info "  Try: ls $FUSE_MOUNT"
    else
        log_warn "FUSE not mounted (yet)"
    fi
    
    # Useful commands
    echo ""
    log_info "Useful commands:"
    log_info "  List services: $COMPOSE_CMD -f $COMPOSE_FILE ps"
    log_info "  View logs:     $COMPOSE_CMD -f $COMPOSE_FILE logs -f <service>"
    log_info "  Shell in FUSE: $COMPOSE_CMD -f $COMPOSE_FILE exec fuse-test bash"
    log_info "  Stop all:      $COMPOSE_CMD -f $COMPOSE_FILE down"
    log_info "  Run tests:     cd kernel/powerfs_mod && sudo ./test_multi_node.sh"
    echo ""
}

# ========== Main ==========
main() {
    echo ""
    echo -e "${C_CYAN}╔══════════════════════════════════════════════════════════╗${C_RESET}"
    echo -e "${C_CYAN}║  PowerFS FUSE Test Environment Startup                  ${C_RESET}"
    echo -e "${C_CYAN}╚══════════════════════════════════════════════════════════╝${C_RESET}"
    echo ""
    echo -e "  ${C_BLUE}Compose file:${C_RESET}  ${COMPOSE_WORKDIR}/${COMPOSE_FILE}"
    echo -e "  ${C_BLUE}Project root:${C_RESET}  ${PROJECT_ROOT}"
    echo -e "  ${C_BLUE}FUSE mount:${C_RESET}    ${FUSE_MOUNT}"
    echo -e "  ${C_BLUE}Log dir:${C_RESET}       ${LOG_DIR}"
    echo ""
    
    check_prerequisites
    
    if [ "$CLEAN_START" -eq 1 ]; then
        clean_environment
    fi
    
    if [ "$BUILD_FIRST" -eq 1 ]; then
        build_images
    fi
    
    # Check if images exist
    cd "$COMPOSE_WORKDIR"
    if ! $COMPOSE_CMD -f "$COMPOSE_FILE" images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | grep -q "powerfs:latest"; then
        log_warn "No powerfs:latest image found. Building..."
        build_images
    fi
    
    start_backend
    start_fuse_client
    start_benchmark
    show_status
    
    if [ "$TAIL_LOGS" -eq 1 ] && [ "$BACKEND_ONLY" -eq 0 ]; then
        log_info "Tailing FUSE client logs (Ctrl+C to exit)..."
        cd "$COMPOSE_WORKDIR"
        $COMPOSE_CMD -f "$COMPOSE_FILE" logs -f fuse-test 2>/dev/null || true
    fi
}

main "$@"