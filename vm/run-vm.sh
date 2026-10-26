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

# 载入统一地址/集群配置 (single source of truth: share/powerfs.env)
[ -f "./share/powerfs.env" ] && . "./share/powerfs.env"

# 默认后端: 全部 filer (from-cluster 或 powerfs.env 默认值)
# 逗号分隔, 内核 fill_super 会解析并逐个 add_server.
export POWERFS_PORT="${POWERFS_PORT:-${POWERFS_MASTER_PORT:-9334}}"
: "${POWERFS_MASTER_ADDR:?POWERFS_MASTER_ADDR not set — check share/powerfs.env}"
# POWERFS_ADDR 用于内核 mount - 通过 POWERFS_FILER_ADDR 派生 (space → comma)
if [ -z "${POWERFS_ADDR:-}" ]; then
    export POWERFS_ADDR=$(echo "${POWERFS_FILER_ADDR:-$POWERFS_MASTER_ADDR}" | tr ' ' ',')
fi

exec bash run_qemu_kernel_ssh.sh
