# PowerFS Kernel Module — Test Documentation

This document describes how to use the PowerFS kernel module test suites to validate Delta Sync, multi-node connection pools, leader failover, and related functionality.

## Test Scripts Overview

| Script | Purpose | Requires Backend | Run Time |
|--------|---------|------------------|----------|
| `verify_module.sh` | Static compilation & symbol export check | No | ~5 seconds |
| `test_multi_node.sh` | Full integration test suite | Yes | ~2-10 minutes |

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

## 7. Reference

- FUSE Client implementation: `powerfs-fuse/src/metadata_cache.rs` — Reference Delta Sync implementation
- PowerFS Net protocol: `powerfs-net/src/protocol.rs` — Protocol definitions
- Kernel net layer: `kernel/powerfs_mod/powerfs_net.c` — Kernel-side powerfs-net client
- Kernel VFS layer: `kernel/powerfs_mod/powerfs_fs.c` — VFS operations with Delta Sync