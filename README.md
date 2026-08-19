# PowerFS Kernel Filesystem (powerfs.ko)

The kernel-space client of PowerFS. It mounts the distributed storage backend (Master + Filer + Volume Server) as a Linux kernel filesystem, providing a POSIX interface aligned with the FUSE client.

## Architecture

```
  User Process
     │  POSIX (open/read/write/...)
     ▼
┌─────────────────────────────────────┐
│  powerfs.ko (this repo)              │
│  ┌──────────┐  ┌────────────────┐    │
│  │ VFS Layer│  │ FileLayout Parse│    │
│  │ (inode/  │  │ (Inline/Stripe/ │    │
│  │  dentry/ │  │  WideStripe/EC) │    │
│  │  page)   │  └────────────────┘    │
│  └────┬─────┘  ┌────────────────┐    │
│       │        │ Lease Lock Mgmt │    │
│       ▼        │ (linearizability)│   │
│  ┌──────────┐  └────────────────┘    │
│  │ Network  │  ┌────────────────┐    │
│  │ (TX/RX   │  │ Flow Control + │    │
│  │  threads)│  │ Reconnect      │    │
│  └────┬─────┘  └────────────────┘    │
└───────┼─────────────────────────────┘
        │  TLV binary protocol (TCP)
        ▼
  ┌───────────┐  ┌─────────┐  ┌────────────┐
  │  Master   │  │  Filer  │  │ Volume Srv │
  │(topology/ │  │(metadata)│  │  (data)    │
  │ allocation)│ │         │  │            │
  └───────────┘  └─────────┘  └────────────┘
```

## Directory Layout

```
kernel/
├── powerfs_mod/           Kernel module source
│   ├── powerfs_mod.c        Module entry + mount parameter parsing
│   ├── powerfs_fs.c         VFS operations (inode/dentry/page cache)
│   ├── powerfs_net.c        Network protocol layer (TLV send/recv)
│   ├── powerfs_transport.c  Transport layer (TX/RX threads + reconnect)
│   ├── powerfs_tlv.c        TLV encode/decode
│   ├── powerfs_serializer.c Serialization/deserialization
│   ├── powerfs_flow.c       Flow control (backpressure + queue mgmt)
│   ├── powerfs_ec.c         Reed-Solomon EC encode/decode
│   ├── powerfs.h            Core data structures
│   ├── powerfs_net.h        Protocol definitions (MsgType/FieldId)
│   ├── Makefile             Kernel module build
│   ├── verify_module.sh     Static symbol verification
│   ├── start_kernel_env.sh  Kernel test environment setup
│   └── start_fuse_env.sh    FUSE comparison environment setup
│
├── tests/                 Test scripts (git-tracked)
│   ├── run_all_tests.sh     T1-T8 full test executor
│   ├── run_t1_local.sh      Local script logic validation (no QEMU)
│   ├── concurrent_test.c    Concurrent test program
│   └── simple_test.c        Basic test program
│
├── vm/                    QEMU test environment (.gitignore)
│   ├── qemuctl.sh           QEMU control (deploy/mount/log)
│   ├── test_t1_vfs_basic.sh T1: VFS basic operations
│   ├── test_t2_correctness  T2: Filesystem correctness
│   ├── test_k1_layout.sh    K1: Protocol alignment + Flat
│   ├── test_k2_inline.sh    K2: Inline small files
│   ├── test_k3_stripe.sh    K3: Stripe multi-volume
│   ├── test_k4_reliability  K4: Reliability failover
│   ├── test_t4_integration  T4: Integration test
│   ├── test_t5_performance  T5: Performance (fio/mdtest)
│   ├── test_t6_stability    T6: Stability
│   ├── test_t7_reliability  T7: Reliability
│   ├── test_t8_persistence  T8: Data persistence
│   ├── test_t9_kernel_e2e   T9: Kernel source E2E (pack/unpack/build/delete)
│   └── fault_injection.sh   Fault injection tool
│
├── kernel-test-plan.md    Test plan (T1-T8 + T9 xfstests)
├── kernel-layout-completion-plan.md  Layout completion plan (K1-K4)
├── kernel-lease-plan.md   Lease lock plan
├── kernel-net-resilience-plan.md     Network resilience plan
├── powerfs-net-design.md  Network architecture design
├── flow-control-design.md Flow control design
├── file-layout-design.md  File layout design
└── kernel-volume.md       Volume management documentation
```

## Building

### Prerequisites

- Linux kernel source (6.17+)
- Kernel build toolchain (gcc, make, bc, flex, bison)
- QEMU (x86_64, for testing)

### Build the kernel module

```bash
cd powerfs_mod
make clean && make -j$(nproc)
# Output: powerfs.ko
```

### Static verification

```bash
cd powerfs_mod
bash verify_module.sh
```

## Testing

### Testing Principles

1. All kernel debugging is performed inside a QEMU virtual machine; never test directly on the host
2. Verify incrementally from small to large (1KB → 1GB); do not skip levels
3. After each test, check dmesg/slab/meminfo — not just application return values
4. Sustained I/O for ≥1 minute + periodic dmesg checks
5. All tests are executed via scripts

### Full Test Suite (T1-T8 + T9)

```bash
cd tests

# Full run (includes environment setup: start backend + QEMU + mount)
./run_all_tests.sh

# Skip environment setup
./run_all_tests.sh --no-env

# Run only specific stages
./run_all_tests.sh -s T1 -s T2

# Continue on failure (default: stop on first failure)
./run_all_tests.sh -c
```

Test execution follows a gating sequence:

```
T1 (VFS basics) → T2 (correctness) → T3 (layout K1-K4) → T4 (integration) → T8 (persistence)
                                                                                    ↓
                              T7 (reliability) ← T6 (stability) ← T5 (performance)
                                                                                    ↓
                                                                              T9 (kernel E2E)
```

### Local Script Validation (no QEMU required)

```bash
cd tests
./run_t1_local.sh              # Validate T1 script logic
./run_t1_local.sh 2 3          # Validate only T2, T3
```

### Single-Stage Testing

```bash
cd vm
./qemuctl.sh service start     # Start backend services
./qemuctl.sh deploy            # Build + deploy to QEMU
./qemuctl.sh mount             # Mount powerfs

./test_t1_vfs_basic.sh         # All T1 tests
./test_t1_vfs_basic.sh 3       # Only T1 test 3
```

See [kernel-test-plan.md](kernel-test-plan.md) for details.

## Features

| Feature | Status | Description |
|---------|--------|-------------|
| Flat single-volume R/W | ✅ | Basic file storage |
| Inline small files | ✅ | <8KB inline storage + auto-migration |
| Stripe multi-volume | ✅ | Cross-volume parallelism + anti-affinity |
| WideStripe | ✅ | 256-volume range compression |
| Replicated reliability | ✅ | Read-path failover + state machine |
| CRC32 verification | ✅ | Post-read verification; returns EIO on mismatch |
| EC (4+2) | ✅ | Reed-Solomon encode/decode + degraded read |
| Lease locking | ✅ | Linearizability (Follower→Leader) |
| Network resilience | ✅ | Disconnect queuing + auto-reconnect + leader switchover |
| Flow control | ✅ | Backpressure + multi-queue + priority |

## Module Parameters

```bash
# Mount example
mount -t powerfs -o master_addr=10.0.2.10:9334,master_addr=10.0.2.11:9334 /mnt/pfs
```

| Parameter | Description | Default |
|-----------|-------------|---------|
| master_addr | Master address (up to 3) | None (required) |

Filer and Volume addresses are dynamically discovered via Master; no configuration needed.

## Related Repositories

| Repository | Description |
|------------|-------------|
| powerfs | Main repo (FUSE client + Master + Filer + Volume Server) |
| **kernel** | This repo (kernel client powerfs.ko) |

## License

GPL-2.0
