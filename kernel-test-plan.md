# 内核文件系统测试方案

> 文档创建：2026-08-09
> 状态：**实施中**
> 关联文档：[kernel-layout-completion-plan.md](kernel-layout-completion-plan.md)

---

## 1. 测试目标

内核文件系统（powerfs.ko）K1-K4 代码实现和代码检查已完成，本方案规划全面的测试验证，从简单到复杂，逐级确认正确性。

## 2. 测试原则

1. **内核状态检查**：每次测试后检查 dmesg/slab/meminfo/D 状态，不只看应用返回值
2. **持续运行**：单次操作通过不代表稳定，连续 IO ≥1 分钟 + 定期 dmesg 检查
3. **从小到大**：先 1KB → 4KB → 64KB → 1MB → 10MB → 100MB → 1GB，不可跳级
4. **脚本化**：所有测试通过脚本运行，避免手工操作遗漏检查项
5. **QEMU 环境**：所有内核调试在 QEMU 虚拟机中进行，禁止宿主机直接测试

## 3. 测试阶段规划

### 阶段 T1: VFS 基础操作测试（独立功能验证）

**目标**：逐个验证 VFS 操作的正确性。

| 测试 ID | 测试内容 | 通过标准 |
|---------|---------|---------|
| T1.1 | 文件 CRUD：create/open/write/read/close/stat/truncate | MD5 一致，stat 正确 |
| T1.2 | 目录操作：mkdir/rmdir/readdir/rename/unlink/symlink/link | 目录树结构正确 |
| T1.3 | 权限测试：chmod/chown/utimes | 权限位/时间戳正确 |
| T1.4 | 特殊文件：mknod (fifo/sock) | 创建成功，可访问 |
| T1.5 | 边界测试：空文件/最大路径名/特殊字符文件名 | 无异常 |
| T1.6 | 并发读写：多进程同时读写同一文件 | 无 corruption，无 panic |

**脚本**：`vm/test_t1_vfs_basic.sh`

---

### 阶段 T2: 文件系统正确性测试

**目标**：验证真实工作负载下的数据完整性。

| 测试 ID | 测试内容 | 通过标准 |
|---------|---------|---------|
| T2.1 | 文件拷贝：cp -r 大目录树（1000+ 文件） | diff 源/目标一致 |
| T2.2 | 压缩/解压：tar czf + tar xzf | MD5 一致 |
| T2.3 | 源码编译：在 powerfs 上 make Linux 源码 | 编译成功，无 IO 错误 |
| T2.4 | rsync 同步：rsync -a 源码到 powerfs | 文件完整一致 |
| T2.5 | git 操作：在 powerfs 上 git clone/commit | 操作成功 |

**脚本**：`vm/test_t2_correctness.sh`

---

### 阶段 T3: 布局功能测试（K1-K4 已有脚本）

**目标**：验证 Inline/Stripe/WideStripe/EC 布局功能。

| 测试 ID | 测试内容 | 脚本 |
|---------|---------|------|
| T3.1 | K1 Flat 读写互通 | `test_k1_layout.sh` |
| T3.2 | K2 Inline 小文件 + 迁移 | `test_k2_inline.sh` |
| T3.3 | K3 Stripe 多卷读写 | `test_k3_stripe.sh` |
| T3.4 | K4 可靠性 failover + CRC32 + EC | `test_k4_reliability.sh` |

---

### 阶段 T4: 集成测试

**目标**：验证多客户端互操作和 remount 一致性。

| 测试 ID | 测试内容 | 通过标准 |
|---------|---------|---------|
| T4.1 | FUSE 创建 → 内核读取（Flat/Inline/Stripe） | MD5 一致 |
| T4.2 | 内核创建 → FUSE 读取（Flat/Inline/Stripe） | MD5 一致 |
| T4.3 | remount 后数据一致性 | 文件列表/内容不变 |
| T4.4 | FUSE 和内核同时挂载，并发读写 | 无 corruption |

**脚本**：`vm/test_t4_integration.sh`

---

### 阶段 T8: 数据持久化测试

**目标**：验证所有修改操作在 umount → remount（或 rmmod → insmod）后数据一致。

| 测试 ID | 测试内容 | 通过标准 |
|---------|---------|---------|
| T8.1 | 写入持久化：小(100B)/中(1MB)/大(10MB)/覆盖写/append | remount 后 MD5 一致 |
| T8.2 | 创建/删除持久化：文件/目录/目录树 | remount 后存在性正确 |
| T8.3 | 硬链接持久化：nlink/内容/原文件删除后存活 | nlink 和 MD5 一致 |
| T8.4 | 软链接持久化：绝对路径/相对路径 | readlink 和 cat 正确 |
| T8.5 | truncate 持久化：扩展/缩小/清零 | size 和内容一致 |
| T8.6 | 元数据持久化：chmod/chown/utimes/目录权限 | mode/uid/gid/mtime 一致 |
| T8.7 | fsync 持久化：fsync+drop_caches/fsync+remount/close flush | MD5 一致 |
| T8.8 | rename 持久化：文件 rename/目录 rename | 旧路径不存在，新路径可读 |
| T8.9 | 综合场景：多操作混合 + 部分删除 | manifest 一致，hardlink 存活 |
| T8.10 | 完整模块重载：umount+rmmod+insmod+mount | 数据完整 |

**脚本**：`vm/test_t8_persistence.sh`

---

### 阶段 T5: 性能测试

**目标**：评估内核文件系统的 IO 性能。

| 测试 ID | 测试内容 | 工具 |
|---------|---------|------|
| T5.1 | 顺序读写基准 (1KB-1GB) | fio |
| T5.2 | 随机读写基准 (4K-1M bs) | fio |
| T5.3 | Stripe vs Flat 性能对比 | fio |
| T5.4 | Inline vs Flat 小文件性能 | mdtest |
| T5.5 | 元数据性能 (create/stat/delete) | mdtest |
| T5.6 | 多线程并发 IO | fio --numjobs |

**脚本**：`vm/test_t5_performance.sh`
**输出**：性能评估报告 (MD 格式)

---

### 阶段 T6: 稳定性测试

**目标**：验证长时间运行的稳定性。

| 测试 ID | 测试内容 | 持续时间 | 通过标准 |
|---------|---------|---------|---------|
| T6.1 | 持续顺序写 | 10 分钟 | 无 IO 错误，dmesg 无异常 |
| T6.2 | 持续随机读写混合 | 30 分钟 | 无 corruption，slab 无泄漏 |
| T6.3 | 高并发压力 (32 线程) | 10 分钟 | 无 panic/deadlock |
| T6.4 | 内存泄漏检测 | 1 小时 | slab/meminfo 稳定 |
| T6.5 | 长时间挂载稳定性 | 1 小时 | 无 hung task，无 oom |

**脚本**：`vm/test_t6_stability.sh`

---

### 阶段 T7: 可靠性测试

**目标**：验证故障场景下的数据安全。

| 测试 ID | 测试内容 | 通过标准 |
|---------|---------|---------|
| T7.1 | 网络断连恢复 | 断连期间请求入队，恢复后完成 |
| T7.2 | Volume Server 故障 failover | 主 volume 故障，从副本读取成功 |
| T7.3 | Filer 故障切换 | Filer leader 切换，客户端自动重连 |
| T7.4 | CRC32 不匹配检测 | 注入损坏数据，返回 EIO |
| T7.5 | EC 降级读 | 1-2 分片丢失，降级重建成功 |
| T7.6 | umount 排空 | umount 后无残留内存/dentry |

**脚本**：`vm/test_t7_reliability.sh`

---

### 阶段 T9: xfstests 补充测试（可选，非主线）

**定位**：T1-T8 全部 PASS 后的**可选补充**，不阻塞主线流程，按需运行特定用例。

**目的**：用社区标准 POSIX/VFS 兼容性测试集，补充自研脚本未覆盖的边界条件和并发竞争场景。

#### 9.1 源码与构建

xfstests 源码位于 [kernel/xfstests-dev](file:///home/portion/powerfs/kernel/xfstests-dev)（git remote 指向 `git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git`，当前为空仓库，需拉取）。

```bash
# 拉取源码
cd kernel/xfstests-dev
git fetch --depth=1 origin master
git checkout master

# 安装依赖（Ubuntu/Debian）
sudo apt install -y build-essential automake libtool libaio-dev \
    libuuid1 uuid-dev libattr1-dev xfsprogs-dev

# 编译
make -j$(nproc) 2>&1 | tail -20

# 验证
./check --help 2>&1 | head -5
```

#### 9.2 配置文件

**FUSE 客户端**：复用 [tests/xfstests/powerfs.conf](file:///home/portion/powerfs/tests/xfstests/powerfs.conf)（已有，FSTYP="fuse.powerfs"）。

**内核客户端**：需新增 `tests/xfstests/powerfs-kernel.localconfig`（T1-T8 PASS 后按需创建），关键配置：

```bash
export TEST_DEV=powerfs               # 网络文件系统无块设备
export TEST_DIR=/mnt/pfs              # 内核挂载点（QEMU 内）
export SCRATCH_MNT=/mnt/pfs-scratch   # 第二挂载点（需内核支持多实例，或复用）
export FSTYP=powerfs
export MKFS_OPTIONS=""                # 无 mkfs，留空
export MOUNT_OPTIONS=""
# 关键：powerfs 无 mkfs 程序，需 stub mkfs.powerfs 返回 0
```

#### 9.3 推荐用例清单（按需运行）

优先选择**针对内核 VFS 回调和 PowerFS 已知风险点**的用例，避免跑完整 generic 组（500+ 用例，耗时长且部分不适用如 quota/fiemap/reflink）。

| 用例组 | 用例 ID | 覆盖点 | 优先级 |
|--------|---------|--------|--------|
| 基础读写 | generic/001, 002, 005 | pwrite/pread 边界 | 高 |
| 文件属性 | generic/006, 007, 009, 068 | stat/utimes/xattr | 高 |
| 目录操作 | generic/011, 012, 013, 014, 015 | mkdir/rmdir/readdir/lookup | 高 |
| 硬软链接 | generic/020, 021, 022, 031, 032, 033 | link/symlink/rename | 高 |
| fsync 语义 | generic/076, 113, 125 | fsync 后崩溃一致性 | 高 |
| truncate | generic/080, 126, 127 | ftruncate + 读写竞争 | 中 |
| O_DIRECT | generic/075, 263, 410 | DIO 路径 | 中 |
| 并发竞争 | generic/091, 092, 127, 263 | rename/truncate/write race | 中 |
| 大文件 | generic/299, 300, 476 | 大文件 IO + EIO 处理 | 中 |
| 权限 | generic/062, 063, 064 | permission/inode_times | 低 |

**排除用例**（PowerFS 不适用）：
- quota 组：PowerFS 无 quota 支持
- reflink/cow 组：无 reflink
- fiemap 组：无 fiemap 接口
- generic/083, 084（fallocate）：已实现 powerfs_fallocate 回调（支持默认/KEEP_SIZE/PUNCH_HOLE 模式），可放开测试

#### 9.4 运行方式

**前置条件**：
1. T1-T8 全部 PASS（确认基础功能稳定，避免在 xfstests 中定位底层 bug）
2. QEMU 已挂载 powerfs（`./qemuctl.sh mount`）或 FUSE 容器已挂载
3. xfstests 已编译完成

**单用例运行**（推荐，便于定位）：
```bash
cd kernel/xfstests-dev

# FUSE 客户端（容器内执行）
./check -c /home/portion/powerfs/tests/xfstests/powerfs.conf \
    generic/001 generic/002 generic/011

# 内核客户端（QVM 内执行，需先准备 localconfig）
./check generic/001 generic/002
```

**分组运行**（批量，结果汇总在 `results/` 目录）：
```bash
# 仅运行 quick 组（约 100 个用例，10-30 分钟）
./check -g quick

# 运行推荐用例清单（自定义 group 文件）
echo "generic/001 generic/002 generic/005 generic/011 generic/020 generic/031 generic/068 generic/076 generic/080" \
    > tests/powerfs-recommended.list
./check -g powerfs-recommended
```

**结果分析**：
```bash
# 查看失败用例
cat results/powerfs-recommended.badlist

# 查看单个用例详细输出
cat results/generic/001.out.bad
cat results/generic/001.full
```

#### 9.5 验证门

- **不阻塞主线**：xfstests 失败用例不阻断 PowerFS 发布，仅作为问题发现渠道
- **分类处理**：
  - POSIX 兼容性问题（如 xattr/utimes 行为不符）：记录并修复
  - 不适用特性（quota/reflink）：在 config 中 `EXCLUDE` 排除
  - 已实现特性（fallocate/xattr 已实现）：powerfs_fallocate 支持 KEEP_SIZE/PUNCH_HOLE，xattr 使用 simple_xattr 内存存储
- **回归用例**：修复后的失败用例纳入回归清单，后续每次重大改动后重跑

#### 9.6 注意事项

1. **现有 `tests/xfstests/run_xfstests.sh` 有缺陷**：
   - 使用 `xfstests -c config -g group` 命令格式，实际 xfstests 用 `./check` + `local.config` 文件
   - 启动 master/volume/fuse 的逻辑与 QEMU 内核测试环境不匹配
   - 建议重写为 `vm/test_t9_xfstests.sh`（按需，T1-T8 PASS 后实施）

2. **内核客户端多挂载点**：xfstests 需要 TEST_DIR 和 SCRATCH_MNT 两个独立挂载点，需确认内核模块是否支持多实例挂载（`mount -t powerfs` 多次）。若不支持，scratch 测试用例需排除或复用同一挂载点。

3. **网络文件系统特殊处理**：
   - `MKFS_POWERFS` 需 stub（`#!/bin/sh; exit 0`），避免 xfstests 调用 mkfs 失败
   - `MOUNT_POWERFS` 可能需自定义（实际由 `qemuctl.sh mount` 完成，xfstests 不应尝试 mount）

---

## 4. 实施顺序

```
T1 (VFS 基础) → T2 (正确性) → T3 (布局功能) → T4 (集成) → T8 (持久化)
                                                                    ↓
                          T7 (可靠性) ← T6 (稳定性) ← T5 (性能)
                                                                    ↓
                                              T9 (xfstests 可选补充，按需)
```

**每阶段验证门**：前一阶段全部 PASS 才进入下一阶段。

**T9 特殊**：非主线，T1-T8 全部 PASS 后按需运行，不阻塞发布。

## 5. 测试环境

- QEMU 虚拟机（`vm/qemuctl.sh`）
- 调试模式：`./qemuctl.sh debug`（loglevel=7 + slub_debug=FZP）
- 集群：容器化 Master + Filer + Volume（宿主机）
- 内核客户端在 QEMU 内
- FUSE 客户端在 fuse-1 容器内

## 6. 自动化测试脚本

### 6.1 全量测试执行器：`tests/run_all_tests.sh`

按实施顺序串行执行 T1-T8 所有阶段，支持门禁、日志隔离、汇总报告。

```bash
cd kernel/tests

# 全量测试 (含环境准备: service start + deploy + mount)
./run_all_tests.sh

# 跳过环境准备 (假设已 deploy + mount)
./run_all_tests.sh --no-env

# 失败时继续 (默认失败即停止, 符合门禁原则)
./run_all_tests.sh -c

# 仅运行指定阶段 (可多次指定)
./run_all_tests.sh -s T1 -s T2
./run_all_tests.sh --no-env -s T3      # 仅 T3 (含 K1-K4)
```

**输出目录**：`output/test-results/run_<timestamp>/`
- `summary.log`：汇总报告（阶段明细 + 计数 + 耗时）
- `combined.log`：合并日志
- `<STAGE>_<script>.log`：各阶段独立日志

**退出码**：0=全通过, 1=有失败, 2=环境失败, 3=参数错误

### 6.2 本地脚本逻辑验证：`tests/run_t1_local.sh`

在本地（无 QEMU/后端服务）验证 T1 测试脚本逻辑，用于开发期快速迭代。

```bash
cd kernel/tests

./run_t1_local.sh              # 运行全部 T0-T7 (本地文件系统)
./run_t1_local.sh 2 3          # 仅运行 T2, T3
./run_t1_local.sh --clean      # 清理 mock 环境
```

**原理**：将挂载点指向 `/tmp/mock_pfs`，`vm()` 函数本地执行，mock docker/pgrep/umount/rmmod 命令，source 真实 `test_t1_vfs_basic.sh` 的函数定义后覆盖 mock 函数。不修改原脚本。

**限制**：本地非 root 时 `/proc/slabinfo` 读取会产生 WARN（非阻断），chown 测试已自动适配当前用户 uid:gid。

### 6.3 单阶段测试脚本

各阶段独立测试脚本（可单独运行，支持选择性执行子测试）：

| 阶段 | 脚本 | 选择性运行 |
|------|------|-----------|
| T1 | `test_t1_vfs_basic.sh` | `./test_t1_vfs_basic.sh 2 3` (T2+T3) |
| T2 | `test_t2_correctness.sh` | `./test_t2_correctness.sh 4` (T4) |
| T3 (K1) | `test_k1_layout.sh` | `./test_k1_layout.sh 3` (T3) |
| T3 (K2) | `test_k2_inline.sh` | `./test_k2_inline.sh 5` (T5) |
| T3 (K3) | `test_k3_stripe.sh` | `./test_k3_stripe.sh 4` (T4) |
| T3 (K4) | `test_k4_reliability.sh` | `./test_k4_reliability.sh 5` (T5) |
| T4 | `test_t4_integration.sh` | `./test_t4_integration.sh 2` (T2) |
| T5 | `test_t5_performance.sh` | `./test_t5_performance.sh 3` (T3) |
| T6 | `test_t6_stability.sh` | `./test_t6_stability.sh 4` (T4) |
| T7 | `test_t7_reliability.sh` | `./test_t7_reliability.sh 5` (T5) |
| T8 | `test_t8_persistence.sh` | `./test_t8_persistence.sh 2` (T2) |

## 7. 测试结果记录

每阶段测试完成后，记录：
- PASS/FAIL 统计
- 发现的问题和修复
- 性能数据（T5 阶段）
- 内核状态检查结果（dmesg/slab/meminfo）
