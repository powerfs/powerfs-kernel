# PowerFS 内核模块 Makefile
# 基于 Linux 6.2 内核 + 内核态 powerfs-net (TCP) 架构

# 内核源码路径 (使用 6.2 内核源码)
KERNELDIR ?= /home/portion/powerfs/linux-6.17

# PowerFS 模块配置
PWD := $(shell pwd)
MODULE_NAME := powerfs

# 源文件
obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-objs := \
    powerfs_mod.o \
    powerfs_fs.o \
    powerfs_net.o \
    powerfs_tlv.o \
    powerfs_transport.o \
    powerfs_serializer.o \
    powerfs_flow.o \
    powerfs_ec.o \
    tlk_codec.o \
    lock_client.o \
    page_cache.o

# 编译选项
ccflags-y += -Wall -Wextra -Wno-unused-parameter
ccflags-y += -Wno-type-limits
ccflags-y += -Wno-unused-variable
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-format
ccflags-y += -Wno-unused-result
ccflags-y += -Wno-maybe-uninitialized
ccflags-y += -Wno-error
ccflags-y += -Wno-stack-protector
ccflags-y += -Wframe-larger-than=8192
ccflags-y += -I$(PWD)
ccflags-y += -DMODULE_NAME=\"$(MODULE_NAME)\"
ccflags-y += -DCONFIG_POWERFS_NET=1

# 网络相关链接选项 (内核模块不需要显式链接)
# ldflags-y += -lnet

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
	@echo "  make help       - 显示此帮助"
	@echo ""
	@echo "交叉编译 (使用特定内核):"
	@echo "  make KERNELDIR=/path/to/linux-6.2"
