# PowerFS Kernel Filesystem (powerfs.ko)

The in-kernel client for PowerFS. Mounts the distributed storage backend
(Master + Filer + Volume Servers) directly as a Linux kernel filesystem,
exposing a POSIX interface that is functionally aligned with the FUSE
client.

## Architecture

```
  User Process
       │  POSIX (open/read/write/...)
       ▼
┌───────────────────────────────────────────┐
│  powerfs.ko (this repository)             │
│  ┌──────────────┐  ┌───────────────────┐  │
│  │  VFS Layer   │  │  FileLayout Parse │  │
│  │ (inode/      │  │ (Inline/Stripe/   │  │
│  │  dentry/     │  │  WideStripe/EC)   │  │
│  │  page cache) │  └───────────────────┘  │
│  └──────┬───────┘  ┌───────────────────┐  │
│         │          │  Lease Manager     │  │
│         ▼          │ (linearizability)  │  │
│  ┌──────────────┐  └───────────────────┘  │
│  │  Net Layer   │  ┌───────────────────┐  │
│  │  (TX/RX      │  │ Flow Control +     │  │
│  │   Threads)   │  │ Reconnect          │  │
│  └──────┬───────┘  └───────────────────┘  │
└─────────┼──────────────────────────────────┘
          │  TLV binary protocol (TCP)
          ▼
  ┌─────────────┐  ┌─────────┐  ┌───────────────┐
  │   Master    │  │  Filer  │  │  Volume Srv   │
  │ (Topology / │  │(metadata│  │  (data chunks)│
  │ Allocation) │  │  + Raft)│  │               │
  └─────────────┘  └─────────┘  └───────────────┘
```

## Directory Layout

```
kernel/
├── powerfs_mod/           Kernel module sources
│   ├── powerfs_mod.c        Module entry + mount parameter parsing
│   ├── powerfs_fs.c         VFS operations (inode/dentry/page cache)
│   ├── powerfs_net.c        Network protocol layer (TLV send/recv)
│   ├── powerfs_transport.c  Transport layer (TX/RX threads + reconnect)
│   ├── powerfs_tlv.c        TLV encoder / decoder
│   ├── powerfs_serializer.c Serialization / deserialization
│   ├── powerfs_flow.c       Flow control (back-pressure + queue mgmt)
│   ├── powerfs_ec.c         Reed-Solomon erasure-coding codec
│   ├── powerfs.h            Core data structures
│   ├── powerfs_net.h        Protocol definitions (msg types / FieldId)
│   ├── Makefile             Kernel-module build rules
│   ├── verify_module.sh     Static symbol/ELF verification
│   ├── start_kernel_env.sh  Kernel-side test-environment bring-up
│   └── start_fuse_env.sh    FUSE control env for cross-client tests
│
├── tests/                 Test scripts (Git tracked)
│   ├── run_all_tests.sh     T1-T8 full-suite runner
│   ├── run_t1_local.sh      Local script-logic validation (no QEMU)
│   ├── concurrent_test.c    Concurrency test program
│   └── simple_test.c        Basic sanity test program
│
├── vm/                    QEMU test harness (.gitignore for build artifacts)
│   ├── qemuctl.sh           QEMU control (deploy/mount/logs)
│   ├── test_t1_vfs_basic.sh T1: Basic VFS operations
│   ├── test_t2_correctness  T2: Filesystem correctness
│   ├── test_k1_layout.sh    K1: Protocol alignment + Flat layout
│   ├── test_k2_inline.sh    K2: Inline small-files
│   ├── test_k3_stripe.sh    K3: Multi-volume striping
│   ├── test_k4_reliability  K4: Failover / reliability
│   ├── test_t4_integration  T4: Integration tests
│   ├── test_t5_performance  T5: Performance (fio/mdtest)
│   ├── test_t6_stability    T6: Long-run stability
│   ├── test_t7_reliability  T7: Reliability & failover
│   ├── test_t8_persistence  T8: Data persistence
│   └── fault_injection.sh   Fault-injection utilities
│
├── kernel-test-plan.md    Test plan (T1–T8 + T9 xfstests)
├── kernel-layout-completion-plan.md  Layout completion roadmap (K1–K4)
├── kernel-lease-plan.md   Lease / locking design
├── kernel-net-resilience-plan.md     Network resilience design
├── powerfs-net-design.md  Network architecture design
├── flow-control-design.md Flow-control design
├── file-layout-design.md  File-layout design
└── kernel-volume.md       Volume-management notes
```

## Building

### Prerequisites

- Linux kernel source (6.17+)
- Kernel build toolchain (`gcc`, `make`, `bc`, `flex`, `bison`)
- QEMU (x86_64) for running the VM test suite

### Build the kernel module

```bash
cd powerfs_mod
make clean && make -j$(nproc)
# artifact: powerfs.ko
```

### Static validation

```bash
cd powerfs_mod
bash verify_module.sh
```

Validates the compiled `.ko` without needing a running backend: ELF
format, architecture, module metadata, Delta-Sync / multi-node symbol
exports, VFS callback definitions, and object-file completeness.

## Testing

### Test Principles

1. All kernel-level testing runs inside a QEMU VM — **never** test on the
   bare-metal host directly.
2. Validate step-by-step from small to large (1 KB → 1 GB); do not skip
   stages.
3. After every stage, inspect `dmesg`, `slabinfo`, and `meminfo` — never
   rely on the application exit code alone.
4. Run continuous I/O for at least 1 minute with periodic `dmesg`
   health checks.
5. Every test is executed by script — reproducibility first.

### Full test suite (T1–T8)

```bash
cd tests

# Full run: bring up backend + QEMU + mount, then execute all stages
./run_all_tests.sh

# Skip environment bring-up (assumes backend & QEMU already up)
./run_all_tests.sh --no-env

# Execute only selected stages
./run_all_tests.sh -s T1 -s T2

# Continue on failure (default: stop at first failure)
./run_all_tests.sh -c
```

The execution order follows a gating dependency:

```
T1 (VFS basics) → T2 (correctness) → T3 (layout K1–K4) → T4 (integration) → T8 (persistence)
                                                                          ↓
                    T7 (reliability) ← T6 (stability) ← T5 (performance)
```

### Local script validation (QEMU not required)

```bash
cd tests
./run_t1_local.sh              # validate T1 script logic
./run_t1_local.sh 2 3          # only T2 and T3
```

### Single-stage runs

```bash
cd vm
./qemuctl.sh service start     # start backend services
./qemuctl.sh deploy            # build .ko + deploy + reboot QEMU
./qemuctl.sh mount             # mount PowerFS inside the VM

./test_t1_vfs_basic.sh         # all of T1
./test_t1_vfs_basic.sh 3       # only T1 sub-stage T3
```

See [kernel-test-plan.md](kernel-test-plan.md) for the complete testing
roadmap.

## Feature Matrix

| Feature | Status | Notes |
|---------|--------|-------|
| Flat single-volume R/W | ✅ | Basic file storage |
| Inline small-files | ✅ | <8 KB inline + auto migration above threshold |
| Stripe multi-volume | ✅ | Cross-volume parallelism + volume anti-affinity |
| WideStripe | ✅ | Range-compressed stripes over up to 256 volumes |
| Reliability (replicas) | ✅ | Read-path failover + state machine |
| CRC32 integrity | ✅ | Post-read verification; mismatch returns `EIO` |
| Erasure Coding (4+2) | ✅ | Reed-Solomon codec + degraded reads |
| Leases | ✅ | Linearizable (Follower → Leader promotion) |
| Network resilience | ✅ | Disconnect buffering + auto reconnect + leader switch |
| Flow control | ✅ | Back-pressure + multi-queue + priorities |

## Module Parameters

```bash
# Example mount
mount -t powerfs -o master_addr=10.0.2.10:9334,master_addr=10.0.2.11:9334 /mnt/pfs
```

| Parameter | Description | Default |
|-----------|-------------|---------|
| `master_addr` | Master endpoint(s) — up to 3 — in `host:port` form. | *(required)* |

Filer and Volume endpoints are discovered dynamically via the Master —
no additional configuration required.

## Related Repositories

| Repo | Role |
|------|------|
| `powerfs` | Main monorepo (FUSE client + Master + Filer + Volume Server) |
| **kernel** | This repository — in-kernel client (`powerfs.ko`) |

## License

GPL-2.0
