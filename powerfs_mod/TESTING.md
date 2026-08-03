# PowerFS Kernel Module — Test Documentation

This document describes how to set up test environments and run test suites to validate Delta Sync, multi-node connection pools, leader failover, and related functionality.

## Quick Start

```bash
# 1. First time: Build Docker images (one-time, ~5-15 min)
sudo ./build_powerfs_image.sh

# 2. Start backend (FUSE containerized environment, fast)
sudo ./start_fuse_env.sh --wait

# 3. Build and mount kernel module (connects to the backend)
sudo ./start_kernel_env.sh --from-fuse-env

# 4. Run integration tests
sudo ./test_multi_node.sh

# 5. Stop everything when done
sudo ./stop_test_env.sh
```

## Test Environment Scripts

| Script | Purpose | Requires Docker | Run Time |
|--------|---------|-----------------|----------|
| `build_powerfs_image.sh` | **Build** Docker images (one-time) | Yes | ~5-15 min |
| `start_fuse_env.sh` | **Start** pre-built FUSE containerized backend | Yes | ~1-2 min |
| `start_kernel_env.sh` | Build, load, mount kernel module | No | ~30 sec |
| `stop_test_env.sh` | Stop & clean all test environments | Yes | ~10 sec |

## Test Scripts Overview

| Script | Purpose | Requires Backend | Run Time |
|--------|---------|------------------|----------|
| `verify_module.sh` | Static compilation & symbol export check | No | ~5 seconds |
| `test_multi_node.sh` | Full integration test suite | Yes | ~2-10 minutes |

---

## 0. Test Environment Setup & Management

PowerFS testing requires two layers:
1. **Backend services** (Master, Volume, Filer) — provided by the FUSE containerized environment
2. **Client layer** — either the FUSE client (container) or the kernel module (local)

### 0.0 Docker Image Building (`build_powerfs_image.sh`)

**One-time** build script that compiles Rust binaries and creates Docker images. Run this first before starting the test environment.

**Produces:**
- `powerfs:latest` — Base image with master/volume/filer binaries
- `powerfs-test:latest` — Extended image with FUSE client + benchmark tools (fio)

**Usage:**
```bash
# First time: Build everything
sudo ./build_powerfs_image.sh

# Rebuild after code changes
sudo ./build_powerfs_image.sh --force

# Skip Rust compilation (use existing target/release/ binaries)
sudo ./build_powerfs_image.sh --skip-rust

# Build Docker without layer cache
sudo ./build_powerfs_image.sh --no-cache
```

**What it does:**
1. Checks prerequisites (cargo, docker, docker compose)
2. Compiles Rust binaries via `cargo build --release` (if not skipped)
3. Builds `powerfs:latest` Docker image
4. Builds `powerfs-test:latest` Docker image
5. Verifies images and lists contents

**Time estimate:**
- Rust build: 5-15 min (first time)
- Docker build: 1-3 min
- Total: ~6-18 min

**Subsequent runs** (after first build) only need `docker build` if Rust code changed, or nothing if only config changes.

### 0.1 FUSE Containerized Environment (`start_fuse_env.sh`)

Starts the complete PowerFS backend cluster plus FUSE client using **pre-built** Docker images. For fast startup, images must be built first with `build_powerfs_image.sh`.

**Services started (in 4 waves):**
- **Wave 1**: `redis` — Metadata store
- **Wave 2**: `master-1`, `master-2`, `master-3` — Raft consensus (ports 9433-9435)
- **Wave 3**: `volume-1`, `volume-2`, `volume-3`, `filer-1`, `filer-2`, `filer-3` — Data + Metadata routing (ports 8180-8182, 8988-8993)
- **Wave 4**: `fuse-test`, `benchmark` — FUSE client mounted at `/tmp/powerfs/test`

**Usage:**
```bash
# Quick start (fire & forget)
sudo ./start_fuse_env.sh

# Start and wait for health (recommended)
sudo ./start_fuse_env.sh --wait

# Start, rebuild images first (calls build_powerfs_image.sh)
sudo ./start_fuse_env.sh --build --wait

# Start backend only (no FUSE client)
sudo ./start_fuse_env.sh --backend-only

# Clean start (removes all data volumes)
sudo ./start_fuse_env.sh --clean --wait

# Tail FUSE logs after startup
sudo ./start_fuse_env.sh --wait --logs
```

**Timeout control:**
```bash
# Custom timeout for --wait mode (default: 180s)
START_TIMEOUT=300 sudo ./start_fuse_env.sh --wait
```

**Ports mapped to host:**

| Service | Container Port | Host Port |
|---------|---------------|-----------|
| master-1 | 9333 | 9433 |
| master-2 | 9333 | 9434 |
| master-3 | 9333 | 9435 |
| volume-1 | 8080 | 8180 |
| volume-2 | 8080 | 8181 |
| volume-3 | 8080 | 8182 |
| filer-1 | 8888/8889 | 8988/8989 |
| filer-2 | 8888/8889 | 8990/8991 |
| filer-3 | 8888/8889 | 8992/8993 |

**Environment Variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `COMPOSE_FILE` | `docker-compose.test.yml` | Compose file name |
| `COMPOSE_WORKDIR` | `docker/` | Directory containing compose file |
| `FUSE_MOUNT` | `/tmp/powerfs/test` | Host mount point for FUSE |
| `LOG_DIR` | `/tmp/powerfs/logs` | Log file directory |
| `START_TIMEOUT` | `180` | Max seconds to wait for --wait mode |

**Performance optimization:**
- Health check intervals reduced (2-3s instead of 5-10s)
- `start_period` added to prevent premature health check failures
- Services started in parallel waves to minimize wait time
- Expected startup: ~1-2 min (vs ~6-10 min with old build-every-time approach)

### 0.2 Kernel Filesystem Environment (`start_kernel_env.sh`)

Builds the PowerFS kernel module, loads it, and mounts the filesystem to communicate with backend services via the `powerfs_net` TCP protocol (bypassing FUSE).

**Usage:**
```bash
# Build module and mount with FUSE test env defaults (host ports)
sudo ./start_kernel_env.sh --from-fuse-env

# Build only (no mount)
sudo ./start_kernel_env.sh --build-only

# Mount with custom multi-node configuration
sudo ./start_kernel_env.sh \
    --filers "10.0.0.1:9334,10.0.0.2:9334,10.0.0.3:9334" \
    --master "10.0.0.4:9333" \
    --volume "10.0.0.5:8080"

# Mount with default local config
sudo ./start_kernel_env.sh

# Unmount only (keep module loaded)
sudo ./start_kernel_env.sh --unmount

# Full reset (unload module + clean mount)
sudo ./start_kernel_env.sh --reset
```

**Default Address Mapping (with FUSE containerized env):**

When using `--from-fuse-env`, the script maps to the host-exposed ports from `docker-compose.test.yml`:

| Component | Host Address |
|-----------|-------------|
| Filer 1 | `127.0.0.1:8988` |
| Filer 2 | `127.0.0.1:8990` |
| Filer 3 | `127.0.0.1:8992` |
| Master | `127.0.0.1:9433` |
| Volume | `127.0.0.1:8180` |

**Environment Variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `POWERFS_MODULE` | `powerfs` | Kernel module name |
| `POWERFS_MOUNT` | `/mnt/powerfs` | Mount point |
| `POWERFS_FILERS` | `127.0.0.1:8988,...` | Filer addresses |
| `POWERFS_MASTER` | `127.0.0.1:9433` | Master address |
| `POWERFS_VOLUME` | `127.0.0.1:8180` | Volume address |
| `KERNEL_BUILD_DIR` | `/lib/modules/$(uname -r)/build` | Kernel headers path |

**What the script does:**
1. Detects kernel source tree and builds `powerfs.ko`
2. Loads module via `insmod`
3. Registers filesystem type in kernel
4. Mounts with multi-node configuration
5. Runs quick smoke test (create/read/Delta Sync)

### 0.3 Stopping Test Environments (`stop_test_env.sh`)

Safely stops and cleans all PowerFS test environments.

**Usage:**
```bash
# Stop everything (kernel + FUSE containers)
sudo ./stop_test_env.sh

# Stop kernel environment only
sudo ./stop_test_env.sh --kernel-only

# Stop FUSE containers only
sudo ./stop_test_env.sh --fuse-only

# Full cleanup - removes ALL data volumes (destructive!)
sudo ./stop_test_env.sh --clean

# Check current status without stopping
sudo ./stop_test_env.sh --status
```

**What the script does:**
1. Unmounts kernel mount point (`/mnt/powerfs`)
2. Unloads kernel module (`powerfs.ko`)
3. Stops FUSE client container
4. Unmounts FUSE host mount point
5. Stops all backend containers
6. Cleans up temporary directories

**Environment Variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `POWERFS_MODULE` | `powerfs` | Module to unload |
| `POWERFS_MOUNT` | `/mnt/powerfs` | Kernel mount to unmount |
| `FUSE_MOUNT` | `/tmp/powerfs/test` | FUSE mount to unmount |
| `COMPOSE_FILE` | `docker-compose.test.yml` | Compose file |

### 0.4 Environment Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Host Machine                             │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Kernel Module (powerfs.ko)                               │  │
│  │  Mount: /mnt/powerfs  |  powerfs_net TCP protocol         │  │
│  └───────────────────────┬───────────────────────────────────┘  │
│                          │                                       │
│                  TCP (host ports)                                │
│                          │                                       │
│  ┌───────────────────────▼───────────────────────────────────┐  │
│  │  Docker Containers (docker-compose.test.yml)              │  │
│  │                                                           │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐                  │  │
│  │  │  Redis  │  │ Master  │  │ Master  │  ...              │  │
│  │  │  :6380  │  │  :9433  │  │  :9434  │                  │  │
│  │  └─────────┘  └─────────┘  └─────────┘                  │  │
│  │                                                           │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐                  │  │
│  │  │  Volume │  │  Volume │  │  Volume │                   │  │
│  │  │  :8180  │  │  :8181  │  │  :8182  │                   │  │
│  │  └─────────┘  └─────────┘  └─────────┘                  │  │
│  │                                                           │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐                  │  │
│  │  │ Filer 1 │  │ Filer 2 │  │ Filer 3 │                   │  │
│  │  │ :8988   │  │ :8990   │  │ :8992   │                   │  │
│  │  └─────────┘  └─────────┘  └─────────┘                  │  │
│  │                                                           │  │
│  │  ┌─────────────────────────────────────────────────┐      │  │
│  │  │ FUSE Client  (mounted at /tmp/powerfs/test)     │      │  │
│  │  └─────────────────────────────────────────────────┘      │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 0.5 Typical Workflows

**Workflow A: Kernel Module Testing (Full Stack)**
```bash
# 1. Start backend
sudo ./start_fuse_env.sh --wait

# 2. Mount kernel module
sudo ./start_kernel_env.sh --from-fuse-env

# 3. Run kernel integration tests
sudo ./test_multi_node.sh

# 4. Cleanup
sudo ./stop_test_env.sh
```

**Workflow B: FUSE Client Testing**
```bash
# 1. Start full FUSE environment (backend + FUSE client)
sudo ./start_fuse_env.sh --wait

# 2. Test via FUSE mount point
ls /tmp/powerfs/test
echo "hello" > /tmp/powerfs/test/hello.txt
cat /tmp/powerfs/test/hello.txt

# 3. Connect to benchmark container for IO tests
docker compose -f docker/docker-compose.test.yml exec benchmark bash

# 4. Cleanup
sudo ./stop_test_env.sh --fuse-only
```

**Workflow C: Kernel Module Unit Tests (No Backend)**
```bash
# 1. Build and verify module
sudo ./start_kernel_env.sh --build-only

# 2. Run static verification
bash verify_module.sh

# 3. Dry-run integration tests
DUMMY_MODE=1 bash ./test_multi_node.sh
```

**Workflow D: Mixed Mode (Compare FUSE vs Kernel)**
```bash
# 1. Start backend only
sudo ./start_fuse_env.sh --backend-only --wait

# 2. Mount kernel module (shares same backend)
sudo ./start_kernel_env.sh --from-fuse-env

# 3. Run tests against kernel mount
sudo ./test_multi_node.sh

# 4. Compare with FUSE (start FUSE client separately)
docker compose -f docker/docker-compose.test.yml up -d fuse-test

# 5. Check both mount points
ls /mnt/powerfs/          # kernel
ls /tmp/powerfs/test/     # FUSE
```

---

## 1. Module Verification (`verify_module.sh`)

### Purpose

Validates the compiled `powerfs.ko` kernel module without requiring a running PowerFS backend. Designed for CI/CD pipelines and pre-commit checks.

### What It Checks

1. **Compilation Artifacts** — Verifies `powerfs.ko` exists, is non-empty, readable
2. **ELF Format** — Validates ELF format, architecture (x86-64), debug info presence
3. **Module Metadata** — Checks modinfo output for version, name, filename fields
4. **Delta Sync API Symbols** (15 symbols) — `powerfs_net_pool_init`, `powerfs_net_set_path_generation`, `powerfs_net_path_stale`, etc.
5. **Multi-Node / Leader Symbols** (5 symbols) — `powerfs_net_switch_leader`, `powerfs_net_failover`, etc.
6. **Delta Sync Implementation Symbols** (6 symbols) — `powerfs_net_pull_delta`, `powerfs_net_full_sync`, etc.
7. **VFS Callback Definitions** (4 symbols) — `powerfs_d_revalidate`, `powerfs_fill_super`, etc.
8. **Object File Completeness** (6 files) — Checks all `.o` intermediate files exist

### Usage

```bash
# Standard run (uses default module name "powerfs")
cd kernel/powerfs_mod
bash verify_module.sh

# Custom module name
MODULE_NAME=my_powerfs bash verify_module.sh

# Save output to a report
bash verify_module.sh 2>&1 | tee verification_report.txt
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All 46 checks passed |
| 1 | One or more checks failed |

### CI/CD Integration

```yaml
# Example GitHub Actions step
- name: Verify kernel module
  run: |
    cd kernel/powerfs_mod
    make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
    bash verify_module.sh
```

---

## 2. Multi-Node Integration Test (`test_multi_node.sh`)

### Purpose

End-to-end integration test that mounts the PowerFS kernel module against a live backend cluster and validates multi-node operations including leader election, failover, Delta Sync, and concurrent access.

### Prerequisites

1. **Kernel Module Compiled**
   ```bash
   cd kernel/powerfs_mod
   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
   ```

2. **PowerFS Backend Running** (default configuration):
   - At least 1 Filer node (default: `127.0.0.1:9001`)
   - For multi-node tests: 3 Filer nodes (ports 9001, 9002, 9003)
   - 1 Master node (default: `127.0.0.1:8001`)
   - 1 Volume node (default: `127.0.0.1:7001`)

3. **Root Privileges** (for mount/insmod operations)

### Test Cases

| # | Test Name | What It Validates |
|---|-----------|-------------------|
| 1 | Module Loading | Module load, metadata, basic health |
| 2 | Single Node Ops | Mount, mkdir, create, read, write, rmdir |
| 3 | Multi-Filer Connection | Connection pool init, leader discovery, Delta Sync consistency |
| 4 | Leader Failover | Failover infrastructure, symbol availability, data preservation |
| 5 | Reconnection & Recovery | Unmount/remount, inode stability, Delta Sync state recovery |
| 6 | Volume Shard Ops | Large file write/read, MD5 integrity, statfs, deletion |
| 7 | Concurrent Operations | 10 parallel writers, 100 files, content integrity verification |
| 8 | Delta Sync Generation | Version tracking (v1→v2→v3), batch operations, cache invalidation |
| 9 | Error Handling | Non-existent files, invalid paths, long names, special chars |
| 10 | Cleanup | Unmount, residual check, kernel log review |

### Usage

```bash
# Basic test with default configuration (single Filer)
sudo ./test_multi_node.sh

# Multi-node test with 3 Filers, 1 Master, 1 Volume
sudo ./test_multi_node.sh \
    "10.0.0.1:9001,10.0.0.2:9001,10.0.0.3:9001" \
    "10.0.0.4:8001" \
    "10.0.0.5:7001"

# Skip slow tests (Volume shard + Concurrent ops)
SKIP_SLOW=1 sudo ./test_multi_node.sh

# Run specific test only
TEST_ONLY=3 sudo ./test_multi_node.sh

# Dry-run mode (no actual mounting)
DUMMY_MODE=1 bash ./test_multi_node.sh

# Full custom configuration
PFS_FILER_ADDRS="filer1:9001,filer2:9001,filer3:9001" \
PFS_MASTER_ADDR="master:8001" \
PFS_VOLUME_ADDR="volume:7001" \
PFS_MOUNT_POINT="/mnt/test_powerfs" \
sudo ./test_multi_node.sh
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `POWERFS_MODULE` | `powerfs` | Kernel module name |
| `PFS_MOUNT_POINT` | `/mnt/powerfs` | Mount point directory |
| `PFS_FILER_ADDRS` | `127.0.0.1:9001,127.0.0.1:9002,127.0.0.1:9003` | Comma-separated Filer `host:port` addresses |
| `PFS_MASTER_ADDR` | `127.0.0.1:8001` | Master server `host:port` |
| `PFS_VOLUME_ADDR` | `127.0.0.1:7001` | Volume server `host:port` |
| `SKIP_SLOW` | `0` | Set to `1` to skip Volume shard and Concurrent test |
| `TEST_ONLY` | `0` | Set to test number (1-10) to run only that test |
| `DUMMY_MODE` | `0` | Set to `1` for dry-run without actual mount/insmod |

### Output

Results are printed to stdout (with colors when on terminal) and logged to a file:

```
Log file: /tmp/powerfs_multi_test_<PID>.log
```

Summary format:
```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 Test 1: Module Loading
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  [PASS] Module powerfs loaded
  [PASS] Module has valid metadata
...
════════════════════════════════════════
 Test Summary
════════════════════════════════════════
  Total:   42
  Passed:  42
  Failed:  0
  Time:    15s
  ✓ ALL TESTS PASSED
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | All tests passed |
| 1 | One or more tests failed |

### Interpreting Results

**[PASS]** — Test condition verified successfully.

**[FAIL]** — Test condition not met. Check the log file for details. Common causes:
- Backend not running or unreachable
- Module not loaded or wrong version
- Permission issues (need root)

**[WARN]** — Test could not fully verify but infrastructure is present. Example:
- "Cannot confirm leader discovery (check dmesg manually)" — leader check may need different dmesg patterns
- "Need 2+ Filers for failover test" — only 1 Filer configured

**[SKIP]** — Test intentionally skipped. Example:
- "Test 6 skipped (SKIP_SLOW=1)" — slow tests disabled

---

## 3. Testing Delta Sync Behavior

### How Delta Sync Works in the Kernel Module

The Delta Sync mechanism maintains cache consistency across nodes by tracking a **generation version** for each filesystem path:

```
VFS lookup → powerfs_d_revalidate()
                │
                ├── TTL expired? → invalidate cache
                ├── Generation stale? → powerfs_net_path_stale() → invalidate cache
                └── net_cache_valid? → invalidate cache
                │
                ▼
            Re-fetch metadata from Filer
```

### Manual Delta Sync Verification

You can manually verify Delta Sync behavior:

```bash
# Mount with multiple Filers
sudo mount -t powerfs -o filer=10.0.0.1:9001,master=10.0.0.4:8001,volume=10.0.0.5:7001 none /mnt/powerfs

# Create and modify files
cd /mnt/powerfs
echo "v1" > test.txt
cat test.txt  # Should show "v1"

echo "v2" > test.txt
cat test.txt  # Should show "v2" (cache invalidated by Delta Sync)

echo "v3" > test.txt  
cat test.txt  # Should show "v3"

# Check kernel logs for Delta Sync activity
dmesg | grep -i "delta\|generation\|stale\|invalidate"
```

### Expected dmesg Patterns

When Delta Sync is working correctly, you should see log entries like:
```
powerfs: d_revalidate '/test.txt' generation stale (cached=1, current=2)
powerfs: path '/test.txt' invalidated
powerfs: delta sync completed for path '/test.txt'
```

---

## 4. Testing Leader Failover

### Prerequisites for Real Failover Testing

To test actual leader failover (not just infrastructure verification):

1. Configure **3+ Filer nodes** in the cluster
2. Start all Filers
3. Mount with multi-Filer configuration
4. Kill the current leader Filer process
5. Verify the kernel module detects the failure and switches to next healthy Filer

### Automated Failover Test Behavior

Test 4 (`test_04_leader_failover`) behaves as follows:

- **With 2+ Filers**: Performs full failover verification — creates data, checks symbols, verifies data persistence
- **With 1 Filer**: Skips live failover but verifies the failover infrastructure exists (symbol availability)

### Manual Failover Testing

```bash
# 1. Mount with multi-Filer
sudo mount -t powerfs -o filer=10.0.0.1:9001,10.0.0.2:9001,master=10.0.0.4:8001,volume=10.0.0.5:7001 none /mnt/powerfs

# 2. Create test data
mkdir /mnt/powerfs/failover_test
echo "critical_data" > /mnt/powerfs/failover_test/data.txt

# 3. Find current leader
dmesg | grep -i "leader\|active.*filer"

# 4. Kill the leader Filer (on the remote node)
# ssh to leader host and kill the filer process

# 5. Wait for failover (monitor interval = 5s)
sleep 6

# 6. Verify data is still accessible
cat /mnt/powerfs/failover_test/data.txt  # Should still work via new leader

# 7. Check failover log
dmesg | grep -i "failover\|switch.*leader\|leader.*change"
```

---

## 5. Troubleshooting

### Module Won't Load

```bash
# Check kernel version
uname -r

# Rebuild with correct kernel headers
cd kernel/powerfs_mod
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# Check for missing symbols
nm powerfs.ko | grep " U "  # Shows undefined symbols

# Load and check dmesg
insmod powerfs.ko
dmesg | tail -20
```

### Mount Fails

```bash
# Check if module is loaded
lsmod | grep powerfs

# Check Filer connectivity
timeout 2 bash -c "echo >/dev/tcp/127.0.0.1/9001" && echo "Filer reachable" || echo "Filer unreachable"

# Check mount options
cat /proc/filesystems | grep powerfs
```

### Tests Fail But Infrastructure Seems OK

```bash
# Run verbose test with DUMMY_MODE to isolate infrastructure issues
DUMMY_MODE=1 bash ./test_multi_node.sh

# Check specific symbols
nm powerfs.ko | grep powerfs_net

# Run verification script
bash verify_module.sh
```

### Delta Sync Not Working

```bash
# Check if path generation tracking is active
dmesg | grep -i "generation\|delta.*sync"

# Force full sync
echo 1 > /sys/kernel/debug/powerfs/full_sync  # If debugfs interface exists

# Check dentry info
cat /sys/kernel/debug/powerfs/dentries 2>/dev/null || \
  dmesg | grep -i "dentry\|revalidate"
```

---

## 6. Architecture Reference

### Kernel Module Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      VFS Layer                              │
│  d_revalidate() ─── generation check ─── cache stale?       │
│  lookup()/create()/mkdir()/unlink()                        │
└───────────────────────┬─────────────────────────────────────┘
                        │
          ┌─────────────▼─────────────┐
          │    powerfs_net Layer       │
          │  ┌───────────────────────┐ │
          │  │  Connection Pool     │ │
          │  │  ┌───┐ ┌───┐ ┌───┐  │ │
          │  │  │F1 │ │F2 │ │F3 │  │ │
          │  │  └───┘ └───┘ └───┘  │ │
          │  │  ┌───┐ ┌───┐        │ │
          │  │  │M1 │ │V1 │        │ │
          │  │  └───┘ └───┘        │ │
          │  └───────────────────────┘ │
          │  ┌───────────────────────┐ │
          │  │  Delta Sync State    │ │
          │  │  path → generation   │ │
          │  │  global_generation   │ │
          │  └───────────────────────┘ │
          │  ┌───────────────────────┐ │
          │  │  Health Monitor      │ │
          │  │  (delayed_work)      │ │
          │  └───────────────────────┘ │
          └─────────────┬─────────────┘
                        │
              ┌─────────▼──────────┐
              │  powerfs-net TCP    │
              │  Protocol (TLV)     │
              └─────────┬──────────┘
                        │
              ┌─────────▼──────────┐
              │  PowerFS Backend    │
              │  Filer / Master /   │
              │  Volume Nodes       │
              └─────────────────────┘
```

### Key Data Structures

| Structure | File | Purpose |
|-----------|------|---------|
| `powerfs_net_pool` | powerfs_net.h | Multi-server connection pool with leader tracking |
| `powerfs_net_delta_state` | powerfs_net.h | Per-path generation tracking for Delta Sync |
| `powerfs_net_conn` | powerfs_net.h | Single TCP connection with request/response matching |
| `powerfs_dentry_info` | powerfs.h | Dentry private data with generation + path |
| `powerfs_inode_info` | powerfs.h | Inode private data with cache_valid + net_cache_valid |

### Key APIs

| API | Purpose |
|-----|---------|
| `powerfs_net_pool_init()` | Initialize multi-node connection pool |
| `powerfs_net_add_server()` | Register a Filer/Master/Volume node |
| `powerfs_net_find_leader()` | Auto-discover leader among Filers |
| `powerfs_net_failover()` | Switch to next healthy Filer |
| `powerfs_net_start_monitor()` | Start periodic health monitoring |
| `powerfs_net_set_path_generation()` | Track generation for a path |
| `powerfs_net_path_stale()` | Check if cached data is stale |
| `powerfs_net_invalidate_path()` | Force cache invalidation for a path |
| `powerfs_net_pull_delta()` | Pull incremental updates from backend |

---

## 7. IO500 Benchmark Testing in QEMU

### Purpose

Evaluate metadata and data I/O performance of the PowerFS kernel module running inside a QEMU virtual machine. This provides controlled benchmarking results for CI/CD regression and performance comparison.

### Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Host Machine                               │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                     QEMU Virtual Machine                      │  │
│  │  ┌─────────────────────────────────────────────────────────┐  │  │
│  │  │  PowerFS Kernel Module (powerfs.ko)                     │  │  │
│  │  │  Mount: /mnt/powerfs                                    │  │  │
│  │  │  powerfs_net TCP protocol (kernel-mode)                 │  │  │
│  │  └─────────────────────────┬───────────────────────────────┘  │  │
│  │                            │                                   │  │
│  │  ┌─────────────────────────▼───────────────────────────────┐  │  │
│  │  │  IO500 Benchmark (inside VM)                            │  │  │
│  │  │  mdtest-easy/hard, ior-easy, ior-rnd4K                 │  │  │
│  │  └─────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────┬───────────────────────────────────┘  │
│                              │ SSH (localhost:2223 → VM:22)          │
│                              ▼                                       │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  Docker Backend Services (172.20.0.0/16 network)              │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐          │  │
│  │  │  Redis  │  │ Master  │  │  Volume │  │  Filer  │          │  │
│  │  └─────────┘  └─────────┘  └─────────┘  └─────────┘          │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

### Prerequisites

1. **IO500 binary compiled**
   ```bash
   cd io500 && make clean && make
   # Binary: io500/io500
   ```

2. **PowerFS kernel module compiled**
   ```bash
   cd kernel/powerfs_mod
   make
   # Module: kernel/vm/output/powerfs.ko
   ```

3. **QEMU VM built**
   ```bash
   cd kernel/vm
   ./build_kernel.sh      # Build kernel bzImage
   ./build_initramfs.sh   # Build initramfs with SSH
   ```

4. **Backend Docker services** (Master, Volume, Filer, Redis)
   ```bash
   cd io500
   docker compose up -d master-1 volume-1 filer-1 redis
   ```

5. **QEMU network configured**
   ```bash
   cd kernel/vm
   sudo ./setup_network.sh
   ```

### Quick Start

```bash
# Run full IO500 benchmark in QEMU
cd kernel/powerfs_mod
sudo ./run_io500_qemu.sh

# The script will:
#   1. Start backend Docker services
#   2. Setup TAP network for QEMU ↔ Docker communication
#   3. Add IO500 binary to initramfs
#   4. Launch QEMU VM (4GB RAM, 4 CPUs, KVM)
#   5. Mount PowerFS kernel filesystem in VM
#   6. Run IO500 benchmark
#   7. Collect results and display performance summary
#   8. Cleanup (stop QEMU, unmount)
```

### IO500 Configuration

Default configuration uses `config-powerfs-kernel-quick.ini`:

```ini
[global]
datadir = /mnt/powerfs/io500-data
resultdir = /mnt/powerfs/io500-results
verbosity = 1
min_runtime = 5

[mdtest-easy]    # Metadata easy: 100 files create/stat/read/delete
[mdtest-hard]    # Metadata hard: 50 files with shared directory
[ior-easy]       # Data I/O: 1MB transfer, 100MB block
[ior-rnd4K]      # Random 4K I/O test
```

**Available config files:**

| File | Description | Duration |
|------|-------------|----------|
| `config-powerfs-kernel-quick.ini` | Quick test (metadata + small data) | ~1-2 min |
| `config-powerfs-quick.ini` | Basic metadata test | ~30s |
| `config-powerfs-medium.ini` | Medium metadata + data | ~5 min |
| `config-powerfs.ini` | Full benchmark suite | ~30 min |

### Command-Line Options

```bash
# Use specific config
sudo ./run_io500_qemu.sh --config config-powerfs-medium.ini

# Skip initramfs rebuild (for repeated tests)
sudo ./run_io500_qemu.sh --skip-build

# Skip backend startup (assume services are running)
sudo ./run_io500_qemu.sh --skip-backend

# Keep VM alive after test (for debugging)
sudo ./run_io500_qemu.sh --keep-alive
# Then SSH in: ssh -p 2223 root@localhost

# Custom backend address
sudo ./run_io500_qemu.sh --backend 172.20.0.35:9334

# Dry run (show plan without executing)
sudo ./run_io500_qemu.sh --dry-run
```

### Output and Results

Results are saved to: `io500/results/qemu-kernel-<timestamp>/`

```
io500/results/qemu-kernel-20260727_150000/
├── io500_summary.txt        # Benchmark summary with timestamp
├── io500_output.log         # Full IO500 output from VM
├── io500_results.tar.gz     # Raw results tarball
└── vm_info.txt              # VM memory/CPU info
```

**Example output:**
```
━━━ IO500 Benchmark Results ━━━

IO500 SCORE: total: 2300.61
BANDWIDTH:  bandwidth: 425.437149 GB/s
IOPS:       ops/sec: 716865
LATENCY:    max: 0.528000

Full output:
────────────────────────────────────────
#IO500 version: version 3.4
#IO500: $Id$
...
```

### Performance Expectations (QEMU Environment)

| Metric | Expected Range | Notes |
|--------|---------------|-------|
| mdtest-easy (create) | 100-500 ops/sec | Single-node, low concurrency |
| mdtest-hard (create) | 50-200 ops/sec | Shared directory contention |
| ior-easy write | 100-400 MB/s | Sequential write, depends on Volume |
| ior-easy read | 200-500 MB/s | Sequential read |
| ior-rnd4K | 50-200 MB/s | Random 4K I/O |

**Note:** QEMU adds overhead compared to bare metal. The kernel module communicates with backend via TAP network bridge, which adds latency. Actual production performance will be higher.

### Troubleshooting

**QEMU fails to start:**
```bash
# Check KVM availability
ls -la /dev/kvm
dmesg | grep kvm

# Check TAP device
ip link show tap0
ip addr show tap0
```

**Backend not reachable from VM:**
```bash
# Verify Docker containers are on the right network
docker network ls
docker inspect <container> | grep IPAddress

# Check TAP bridge connection
ip link show tap0
docker network inspect <network> | grep Bridge
```

**IO500 binary not found in VM:**
```bash
# Transfer manually
scp -P 2223 io500/io500 root@localhost:/io500/
scp -P 2223 io500/config-powerfs-kernel-quick.ini root@localhost:/io500/

# SSH and test
ssh -p 2223 root@localhost
ls -la /io500/io500
```

**Mount fails in VM:**
```bash
# Check kernel log
ssh -p 2223 root@localhost "dmesg | tail -30"

# Verify module loaded
ssh -p 2223 root@localhost "lsmod | grep powerfs"

# Try manual mount
ssh -p 2223 root@localhost "mount -t powerfs -o filer=172.20.0.35:9334,master=172.20.0.14:9333,volume=172.20.0.24:8080 none /mnt/powerfs"
```

### Script Reference

| Script | Purpose |
|--------|---------|
| `run_io500_qemu.sh` | Main IO500 benchmark orchestration |
| `start_fuse_env.sh` | Start backend Docker services |
| `start_kernel_env.sh` | Build/mount kernel module (host) |
| `stop_test_env.sh` | Stop test environments |
| `build_initramfs.sh` | Build initramfs with SSH |
| `run_qemu_kernel_ssh.sh` | Manual QEMU launch with SSH |
| `setup_network.sh` | Configure TAP network for QEMU |

---

## 8. Reference

- FUSE Client implementation: `powerfs-fuse/src/metadata_cache.rs` — Reference Delta Sync implementation
- PowerFS Net protocol: `powerfs-net/src/protocol.rs` — Protocol definitions
- Kernel net layer: `kernel/powerfs_mod/powerfs_net.c` — Kernel-side powerfs-net client
- Kernel VFS layer: `kernel/powerfs_mod/powerfs_fs.c` — VFS operations with Delta Sync
- IO500 benchmark: `io500/README.md` — IO500 documentation and configuration guide