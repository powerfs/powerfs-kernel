# PowerFS 内核模块 Makefile
# 基于 Linux 6.2 内核 + 内核态 powerfs-net (TCP) 架构
#
# 两段式标准 kbuild 结构:
#   pass 1 (KERNELRELEASE 为空): 开发态裸 make / qemuctl / CI 入口,
#                                负责转发到内核树 KERNELDIR
#   pass 2 (KERNELRELEASE 非空): kbuild 解析 obj-m/objs/ccflags
#                                (裸 make 递归进入; DKMS 直接从这层进入:
#                                 dkms 以 `make -C <ksrc> M=<build_dir>` 调用)

# 模块与路径 (两段都会用到, 放守卫外面)
MODULE_NAME := powerfs
PWD := $(shell pwd)

# 开发态裸 make 的内核源码路径 (DKMS 构建时由 -C 指定, 不走这个默认值)
KERNELDIR ?= /home/portion/powerfs/linux-6.17

# ============================================================
# pass 1: 开发态入口
# ============================================================
ifeq ($(KERNELRELEASE),)

# 检查是否有内核源码
ifeq ($(wildcard $(KERNELDIR)/Makefile),)
    $(warning 警告: 内核源码目录 $(KERNELDIR) 不存在)
    $(warning 请设置 KERNELDIR 变量: make KERNELDIR=/path/to/kernel/source)
endif

all:
	@echo "========================================"
	@echo "  PowerFS 内核模块编译"
	@echo "  内核版本: $(shell cat $(KERNELDIR)/Makefile 2>/dev/null | head -3 | grep VERSION || echo 'unknown')"
	@echo "  源码路径: $(KERNELDIR)"
	@echo "========================================"
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
	@echo ""
	@echo "编译产物:"
	@ls -lh *.ko 2>/dev/null || echo "  (无 .ko 文件)"

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	@rm -f *.o *.ko *.mod *.mod.c *.mod.o .*.o.cmd .*.cmd
	@rm -f test_tlk_codec

install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install
	depmod -a

load:
	@echo "加载 powerfs 模块..."
	insmod $(MODULE_NAME).ko
	@echo "模块已加载"
	@dmesg | tail -5

unload:
	@echo "卸载 powerfs 模块..."
	rmmod $(MODULE_NAME)
	@echo "模块已卸载"

test:
	@echo "模块信息:"
	@modinfo $(MODULE_NAME).ko 2>/dev/null || echo "  (需要先编译)"
	@echo ""
	@echo "加载后可用命令:"
	@echo "  lsmod | grep powerfs"
	@echo "  dmesg | tail -20"
	@echo "  mount -t powerfs /dev/null /mnt/powerfs"

# Userspace codec round-trip test (phase 4-6 §6e).
# Compiles tlk_codec.c with -U__KERNEL__ to validate the dual-environment
# codec against test_tlk_codec.c. The kernel build picks the same .c
# file up via $(MODULE_NAME)-objs above; this target is independent so
# the codec can be regression-tested without a kernel tree.
TLK_CODEC_CFLAGS := -Wall -Wextra -Werror -U__KERNEL__ -I$(PWD)

test_codec: test_tlk_codec.c tlk_codec.c tlk_codec.h
	$(CC) $(TLK_CODEC_CFLAGS) -o test_tlk_codec \
		test_tlk_codec.c tlk_codec.c
	@echo "=== running tlk_codec round-trip tests ==="
	./test_tlk_codec
	@echo "=== all tests passed ==="

clean_codec:
	@rm -f test_tlk_codec

# 开发辅助命令
help:
	@echo "PowerFS 内核模块 Makefile 帮助"
	@echo "=============================="
	@echo ""
	@echo "常用命令:"
	@echo "  make            - 编译模块"
	@echo "  make clean      - 清理编译产物"
	@echo "  make install    - 安装模块到系统"
	@echo "  make load       - 加载模块 (需要 root)"
	@echo "  make unload     - 卸载模块 (需要 root)"
	@echo "  make test       - 显示模块信息"
	@echo "  make help       - 显示帮助"
	@echo ""
	@echo "交叉编译 (使用特定内核):"
	@echo "  make KERNELDIR=/path/to/linux-6.2"

# ============================================================
# pass 2: kbuild 解析 (裸 make 递归进入, 或 DKMS 直接进入)
# ============================================================
else

# 源文件
obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-objs := \
    powerfs_mod.o \
    powerfs_super.o \
    powerfs_inode.o \
    powerfs_file.o \
    powerfs_dir.o \
    powerfs_addr.o \
    powerfs_dentry.o \
    powerfs_caps.o \
    powerfs_locks.o \
    powerfs_lease.o \
    powerfs_xattr.o \
    powerfs_net.o \
    powerfs_net_shard.o \
    powerfs_net_sock.o \
    powerfs_net_conn.o \
    powerfs_net_req.o \
    powerfs_net_inode.o \
    powerfs_net_data.o \
    powerfs_net_xattr.o \
    powerfs_net_pool.o \
    powerfs_net_discover.o \
    powerfs_net_lease.o \
    powerfs_net_tcp_ops.o \
    powerfs_tlv.o \
    powerfs_transport.o \
    powerfs_serializer.o \
    powerfs_flow.o \
    powerfs_ec.o \
    page_cache.o \
    powerfs_readahead.o

# 写预测 + 内容指纹去重 (powerfs_write_predict.c).
# 缺省关闭: 该功能 hook 在写热路径 (xattr 查询 / SHA-256 / 同步 RPC),
# 尚未在生产负载验证, 默认不编译以保证正常写零开销零影响.
# 启用: make WRITE_PREDICT=y (定义 CONFIG_POWERFS_WRITE_PREDICT,
# 头文件中的 static inline 空实现自动切换为真实实现).
WRITE_PREDICT ?= n
ifeq ($(WRITE_PREDICT),y)
    $(MODULE_NAME)-objs += powerfs_write_predict.o
    ccflags-y += -DCONFIG_POWERFS_WRITE_PREDICT=1
endif

# RDMA 传输 (powerfs_net_rdma.c, 依赖 rdma_cm/ib_core/ib_verbs 符号).
# kbuild 解析本文件前会先 include 目标内核树的 include/config/auto.conf,
# 因此 CONFIG_INFINIBAND 直接反映目标内核配置 (=y/=m 都编入, 开发态裸
# make 与 DKMS 行为一致); 也可用 make CONFIG_INFINIBAND=y 强制启用.
ifneq ($(filter y Y 1,$(CONFIG_INFINIBAND)),)
    $(MODULE_NAME)-objs += powerfs_net_rdma.o
    ccflags-y += -DCONFIG_POWERFS_RDMA=1
endif

# NOTE: tlk_codec.o / lock_client.o have been REMOVED from the kernel link.
#
# Both modules were dead code (grep confirms zero FS/NET callers):
#   - tlk_codec.c/h defined an independent wire protocol (msg_type + length
#     LE + TLK_FIELD_*) that was never plugged into the powerfs-net TLV
#     transport layer; the actual cap negotiation uses MsgType 0x91~0x95
#     in powerfs_net_cap_* (powerfs_net.c + powerfs_fs.c cap_open_grant).
#   - lock_client.c's acquire/release/renew/revoke_ack all returned
#     -ENOSYS when transport == NULL, and the transport was never wired
#     up; the per-inode MDLock state machine (powerfs_mdlock_rdma etc.)
#     was declared but never called from any VFS path.
#
# Architecture alignment (Ceph MDS Locker model): lock arbitration / state
# machines live *exclusively* on the Filer leader (lock_arbiter.rs).
# Kernel/FUSE clients only cache cap tokens + issued bits and respond to
# recall by flushing dirty pages + sending CapRecallAck (0x92).
#
# The files remain in-tree for source history / future audit; the
# independent userspace round-trip target (make test_codec) still works
# via the explicit tlk_codec.c file list in pass 1.

# 编译选项
ccflags-y += -Wall -Wextra -Wno-unused-parameter
ccflags-y += -Wno-type-limits
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-format
ccflags-y += -Wno-unused-result
ccflags-y += -Wno-maybe-uninitialized
ccflags-y += -Wno-error
ccflags-y += -Wno-error=unused-label
ccflags-y += -Wno-stack-protector
ccflags-y += -Wframe-larger-than=8192
ccflags-y += -I$(src)
ccflags-y += -DMODULE_NAME=\"$(MODULE_NAME)\"
ccflags-y += -DCONFIG_POWERFS_NET=1

# 网络相关链接选项 (内核模块不需要显式链接)
# ldflags-y += -lnet

endif # ifeq ($(KERNELRELEASE),)
