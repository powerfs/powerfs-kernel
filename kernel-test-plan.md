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

## 4. 实施顺序

```
T1 (VFS 基础) → T2 (正确性) → T3 (布局功能) → T4 (集成) → T8 (持久化)
                                                                    ↓
                          T7 (可靠性) ← T6 (稳定性) ← T5 (性能)
```

**每阶段验证门**：前一阶段全部 PASS 才进入下一阶段。

## 5. 测试环境

- QEMU 虚拟机（`vm/qemuctl.sh`）
- 调试模式：`./qemuctl.sh debug`（loglevel=7 + slub_debug=FZP）
- 集群：容器化 Master + Filer + Volume（宿主机）
- 内核客户端在 QEMU 内
- FUSE 客户端在 fuse-1 容器内

## 6. 测试结果记录

每阶段测试完成后，记录：
- PASS/FAIL 统计
- 发现的问题和修复
- 性能数据（T5 阶段）
- 内核状态检查结果（dmesg/slab/meminfo）
