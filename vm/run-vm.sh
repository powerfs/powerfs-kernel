#!/bin/bash
# PowerFS VM 启动脚本 (统一入口, 内置正确后端地址)
#
# 直接调用 run_qemu_kernel_ssh.sh, 默认配置全部 3 个 filer 节点.
# 内核模块支持逗号分隔的多地址, 使 find_leader 能在单点断连时 failover.
# 可通过环境变量覆盖: POWERFS_ADDR=... POWERFS_PORT=... ./run-vm.sh
#
# 用法: ./run-vm.sh

set -u

cd "$(dirname "$0")"

# 默认后端: 全部 3 个 filer (docker_powerfs-network IP + powerfs-net 端口)
# 逗号分隔, 内核 fill_super 会解析并逐个 add_server
export POWERFS_ADDR="${POWERFS_ADDR:-172.30.0.35,172.30.0.36,172.30.0.37}"
export POWERFS_PORT="${POWERFS_PORT:-9334}"

exec bash run_qemu_kernel_ssh.sh
