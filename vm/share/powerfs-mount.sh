#!/bin/sh
# =============================================================================
# PowerFS VM 内挂载/卸载辅助 — 地址/端口/证书/传输全部来自统一配置
# /mnt/host/powerfs.env (宿主机 kernel/vm/share/powerfs.env).
#
# 用法 (在 VM 内):
#   sh /mnt/host/powerfs-mount.sh            挂载 (未加载 ko 时自动 insmod)
#   sh /mnt/host/powerfs-mount.sh mount      同上
#   sh /mnt/host/powerfs-mount.sh umount     卸载并 rmmod
#   sh /mnt/host/powerfs-mount.sh reload     umount + rmmod + insmod + mount
#                                            (热部署新 powerfs.ko 后用)
#   sh /mnt/host/powerfs-mount.sh status     查看挂载状态
#
# 可用环境变量临时覆盖, 例如:
#   POWERFS_MASTER_ADDR=1.2.3.4 sh /mnt/host/powerfs-mount.sh
# =============================================================================
set -e

ENV_FILE="${POWERFS_ENV_FILE:-/mnt/host/powerfs.env}"
KO_FILE="${POWERFS_KO:-/mnt/host/powerfs.ko}"

# 载入统一配置 (若存在); 下方再给兜底默认值, 即使 env 缺失也能工作.
[ -f "$ENV_FILE" ] && . "$ENV_FILE"

: "${POWERFS_MASTER_ADDR:=192.168.100.4}"
: "${POWERFS_MASTER_PORT:=9334}"
: "${POWERFS_SHARD_COUNT:=1}"
: "${POWERFS_TRANSPORT:=rdma}"
: "${POWERFS_CA_CRT:=/etc/powerfs/ca.crt}"
: "${POWERFS_CLIENT_CRT:=/etc/powerfs/kernel-client-1.crt}"
: "${POWERFS_CLIENT_KEY:=/etc/powerfs/kernel-client-1.key}"
: "${POWERFS_MOUNT_POINT:=/mnt/powerfs}"

do_insmod() {
    if lsmod 2>/dev/null | grep -q '^powerfs '; then
        return 0
    fi
    echo "[powerfs-mount] insmod $KO_FILE"
    insmod "$KO_FILE"
}

do_mount() {
    do_insmod
    mkdir -p "$POWERFS_MOUNT_POINT"
    OPTS="master_addr=${POWERFS_MASTER_ADDR},master_port=${POWERFS_MASTER_PORT},shard_count=${POWERFS_SHARD_COUNT}"
    OPTS="${OPTS},ca_crt=${POWERFS_CA_CRT},client_crt=${POWERFS_CLIENT_CRT},client_key=${POWERFS_CLIENT_KEY}"
    if [ "$POWERFS_TRANSPORT" = "rdma" ]; then
        OPTS="${OPTS},transport=rdma"
    fi
    echo "[powerfs-mount] mount -> $POWERFS_MOUNT_POINT"
    echo "                 master=${POWERFS_MASTER_ADDR}:${POWERFS_MASTER_PORT} shards=${POWERFS_SHARD_COUNT} transport=${POWERFS_TRANSPORT}"
    mount -t powerfs -o "$OPTS" powerfs "$POWERFS_MOUNT_POINT"
    echo "[powerfs-mount] OK"
}

do_umount() {
    echo "[powerfs-mount] umount $POWERFS_MOUNT_POINT"
    umount -l "$POWERFS_MOUNT_POINT" 2>/dev/null || true
    if lsmod 2>/dev/null | grep -q '^powerfs '; then
        rmmod powerfs 2>/dev/null || true
    fi
}

case "${1:-mount}" in
    mount|"")
        do_mount
        ;;
    umount|unmount)
        do_umount
        ;;
    reload)
        do_umount
        sleep 1
        do_mount
        ;;
    status)
        if grep -q '^powerfs ' /proc/mounts 2>/dev/null; then
            grep '^powerfs ' /proc/mounts
        else
            echo "powerfs not mounted (target: $POWERFS_MOUNT_POINT)"
            exit 1
        fi
        ;;
    *)
        echo "usage: $0 [mount|umount|reload|status]" >&2
        exit 1
        ;;
esac
