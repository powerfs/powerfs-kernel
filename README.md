# PowerFS 内核文件系统 (powerfs.ko)

PowerFS 的内核态客户端，将分布式存储后端（Master + Filer + Volume Server）挂载为 Linux 内核文件系统，提供 POSIX 接口，与 FUSE 客户端功能对齐。

## 架构

```
  用户进程
     │  POSIX (open/read/write/...)
     ▼
┌─────────────────────────────────────┐
│  powerfs.ko (本仓库)                │
│  ┌──────────┐  ┌────────────────┐  │
│  │ VFS 层   │  │ FileLayout 解析 │  │
│  │ (inode/  │  │ (Inline/Stripe/ │  │
│  │  dentry/ │  │  WideStripe/EC) │  │
│  │  page)   │  └────────────────┘  │
│  └────┬─────┘  ┌────────────────┐  │
│       │        │ Lease 锁管理     │  │
│       ▼        │ (线性一致性)     │  │
│  ┌──────────┐  └────────────────┘  │
│  │ 网络层   │  ┌────────────────┐  │
│  │ (TX/RX   │  │ 流控 + 重连     │  │
│  │  线程)   │  └────────────────┘  │
│  └────┬─────┘                      │
└───────┼─────────────────────────────┘
        │  TLV 二进制协议 (TCP)
        ▼
  ┌───────────┐  ┌─────────┐  ┌────────────┐
  │  Master   │  │  Filer  │  │ Volume Srv │
  │ (拓扑/分配)│  │(元数据) │  │  (数据)    │
  └───────────┘  └─────────┘  └────────────┘
```

## 目录结构

```
kernel/
├── powerfs_mod/           内核模块源码
│   ├── powerfs_mod.c        模块入口 + 挂载参数解析
│   ├── powerfs_fs.c         VFS 操作 (inode/dentry/page cache)
│   ├── powerfs_net.c        网络协议层 (TLV 收发)
│   ├── powerfs_transport.c  传输层 (TX/RX 线程 + 重连)
│   ├── powerfs_tlv.c        TLV 编解码
│   ├── powerfs_serializer.c 序列化/反序列化
│   ├── powerfs_flow.c       流控 (背压 + 队列管理)
│   ├── powerfs_ec.c         Reed-Solomon EC 编解码
│   ├── powerfs.h            核心数据结构
│   ├── powerfs_net.h        协议定义 (消息类型/FieldId)
│   ├── Makefile             内核模块编译
│   ├── verify_module.sh     静态符号验证
│   ├── start_kernel_env.sh  内核测试环境启动
│   └── start_fuse_env.sh    FUSE 对照环境启动
│
├── tests/                 测试脚本 (Git 跟踪)
│   ├── run_all_tests.sh     T1-T8 全量测试执行器
│   ├── run_t1_local.sh      本地脚本逻辑验证 (无需 QEMU)
│   ├── concurrent_test.c    并发测试程序
│   └── simple_test.c        基础测试程序
│
├── vm/                    QEMU 测试环境 (.gitignore)
│   ├── qemuctl.sh           QEMU 控制 (deploy/mount/log)
│   ├── test_t1_vfs_basic.sh T1: VFS 基础操作
│   ├── test_t2_correctness  T2: 文件系统正确性
│   ├── test_k1_layout.sh    K1: 协议对齐 + Flat
│   ├── test_k2_inline.sh    K2: Inline 小文件
│   ├── test_k3_stripe.sh    K3: Stripe 多卷
│   ├── test_k4_reliability  K4: 可靠性 failover
│   ├── test_t4_integration  T4: 集成测试
│   ├── test_t5_performance  T5: 性能 (fio/mdtest)
│   ├── test_t6_stability    T6: 稳定性
│   ├── test_t7_reliability  T7: 可靠性
│   ├── test_t8_persistence  T8: 数据持久化
│   └── fault_injection.sh   故障注入工具
│
├── kernel-test-plan.md    测试方案 (T1-T8 + T9 xfstests)
├── kernel-layout-completion-plan.md  布局完善方案 (K1-K4)
├── kernel-lease-plan.md   Lease 锁方案
├── kernel-net-resilience-plan.md     网络韧性方案
├── powerfs-net-design.md  网络架构设计
├── flow-control-design.md 流控设计
├── file-layout-design.md  文件布局设计
└── kernel-volume.md       Volume 管理文档
```

## 编译

### 依赖

- Linux 内核源码 (6.17+)
- 内核构建工具链 (gcc, make, bc, flex, bison)
- QEMU (x86_64, 用于测试)

### 编译内核模块

```bash
cd powerfs_mod
make clean && make -j$(nproc)
# 产物: powerfs.ko
```

### 静态验证

```bash
cd powerfs_mod
bash verify_module.sh
```

## 测试

### 测试原则

1. 所有内核调试在 QEMU 虚拟机中进行，禁止宿主机直接测试
2. 从小到大逐级验证 (1KB → 1GB)，不可跳级
3. 每次测试后检查 dmesg/slab/meminfo，不只看应用返回值
4. 连续 IO ≥1 分钟 + 定期 dmesg 检查
5. 所有测试通过脚本执行

### 全量测试 (T1-T8)

```bash
cd tests

# 全量 (含环境准备: 启动后端 + QEMU + 挂载)
./run_all_tests.sh

# 跳过环境准备
./run_all_tests.sh --no-env

# 仅运行指定阶段
./run_all_tests.sh -s T1 -s T2

# 失败时继续 (默认失败即停止)
./run_all_tests.sh -c
```

测试顺序遵循门禁原则：

```
T1 (VFS 基础) → T2 (正确性) → T3 (布局 K1-K4) → T4 (集成) → T8 (持久化)
                                                                       ↓
                         T7 (可靠性) ← T6 (稳定性) ← T5 (性能)
```

### 本地脚本验证 (无需 QEMU)

```bash
cd tests
./run_t1_local.sh              # 验证 T1 脚本逻辑
./run_t1_local.sh 2 3          # 仅验证 T2, T3
```

### 单阶段测试

```bash
cd vm
./qemuctl.sh service start     # 启动后端服务
./qemuctl.sh deploy            # 编译 + 部署 QEMU
./qemuctl.sh mount             # 挂载 powerfs

./test_t1_vfs_basic.sh         # T1 全部
./test_t1_vfs_basic.sh 3       # T1 仅 T3
```

详见 [kernel-test-plan.md](kernel-test-plan.md)。

## 功能特性

| 特性 | 状态 | 说明 |
|------|------|------|
| Flat 单卷读写 | ✅ | 基础文件存储 |
| Inline 小文件 | ✅ | <8KB 内联存储 + 自动迁移 |
| Stripe 多卷条带 | ✅ | 跨卷并行 + anti-affinity |
| WideStripe | ✅ | 256 卷范围压缩 |
| Reliability 副本 | ✅ | 读路径 failover + 状态机 |
| CRC32 校验 | ✅ | 读后校验，不匹配返回 EIO |
| EC (4+2) | ✅ | Reed-Solomon 编解码 + 降级读 |
| Lease 锁 | ✅ | 线性一致性 (Follower→Leader) |
| 网络韧性 | ✅ | 断连入队 + 自动重连 + Leader 切换 |
| 流控 | ✅ | 背压 + 多队列 + 优先级 |

## 模块参数

```bash
# 挂载示例
mount -t powerfs -o master_addr=10.0.2.10:9334,master_addr=10.0.2.11:9334 /mnt/pfs
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| master_addr | Master 地址 (最多 3 个) | 无 (必填) |

Filer 和 Volume 地址通过 Master 动态发现，无需配置。

## 相关仓库

| 仓库 | 说明 |
|------|------|
| powerfs | 主仓库 (FUSE 客户端 + Master + Filer + Volume Server) |
| **kernel** | 本仓库 (内核客户端 powerfs.ko) |

## License

GPL-2.0
