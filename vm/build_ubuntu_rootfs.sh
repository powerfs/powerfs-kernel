#!/bin/bash
#
# PowerFS 内核调试环境 - Ubuntu 根文件系统构建脚本
# 基于 debootstrap 创建最小 Ubuntu 系统，集成调试工具
#

set -e

# 配置参数
UBUNTU_VERSION="focal"  # Ubuntu 20.04
OUTPUT_DIR="${PWD}/output"
ROOTFS_DIR="${OUTPUT_DIR}/ubuntu_rootfs"
INITRAMFS_FILE="${OUTPUT_DIR}/ubuntu_initramfs.cpio.gz"
MIRROR_URL="http://mirrors.tuna.tsinghua.edu.cn/ubuntu"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# 检查 root 权限
if [ "$(id -u)" -ne 0 ]; then
    log_error "此脚本需要 root 权限运行"
    exit 1
fi

# 检查 debootstrap
if ! command -v debootstrap &> /dev/null; then
    log_error "debootstrap 未安装，请先运行: sudo apt-get install debootstrap"
    exit 1
fi

# 清理旧的构建
log_info "清理旧的构建..."
rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}"

# 步骤 1: 使用 debootstrap 创建最小 Ubuntu 系统
log_info "步骤 1: 使用 debootstrap 创建最小 Ubuntu ${UBUNTU_VERSION} 系统..."
log_info "下载基础系统包（可能需要几分钟）..."

debootstrap --arch=amd64 \
    --variant=minbase \
    --components=main,restricted,universe,multiverse \
    "${UBUNTU_VERSION}" \
    "${ROOTFS_DIR}" \
    "${MIRROR_URL}"

log_info "基础系统创建完成"

# 步骤 2: 配置 APT 源
log_info "步骤 2: 配置 APT 源..."

cat > "${ROOTFS_DIR}/etc/apt/sources.list" << EOF
deb ${MIRROR_URL} ${UBUNTU_VERSION} main restricted universe multiverse
deb ${MIRROR_URL} ${UBUNTU_VERSION}-updates main restricted universe multiverse
deb ${MIRROR_URL} ${UBUNTU_VERSION}-security main restricted universe multiverse
EOF

# 步骤 3: 配置基本系统
log_info "步骤 3: 配置基本系统..."

# 设置 hostname
echo "powerfs-vm" > "${ROOTFS_DIR}/etc/hostname"

# 设置 hosts
cat > "${ROOTFS_DIR}/etc/hosts" << 'EOF'
127.0.0.1   localhost
127.0.1.1   powerfs-vm
::1         localhost ip6-localhost ip6-loopback
EOF

# 设置时区为 UTC (或从主机获取)
if [ -f /etc/localtime ]; then
    cp /etc/localtime "${ROOTFS_DIR}/etc/localtime"
fi

# 设置 DNS
echo "nameserver 8.8.8.8" > "${ROOTFS_DIR}/etc/resolv.conf"
echo "nameserver 114.114.114.114" >> "${ROOTFS_DIR}/etc/resolv.conf"

# 创建必要的目录
mkdir -p "${ROOTFS_DIR}/proc" "${ROOTFS_DIR}/sys" "${ROOTFS_DIR}/dev/pts" \
    "${ROOTFS_DIR}/run" "${ROOTFS_DIR}/tmp" "${ROOTFS_DIR}/mnt" "${ROOTFS_DIR}/media" \
    "${ROOTFS_DIR}/var/log" "${ROOTFS_DIR}/var/lib" "${ROOTFS_DIR}/var/run"

# 设置权限
chmod 1777 "${ROOTFS_DIR}/tmp"
chmod 755 "${ROOTFS_DIR}/var/run"

# 步骤 4: 使用 chroot 安装 OpenSSH 和调试工具
log_info "步骤 4: 安装 OpenSSH 和调试工具..."

# 挂载必要的文件系统
mount --bind /proc "${ROOTFS_DIR}/proc"
mount --bind /sys "${ROOTFS_DIR}/sys"
mount --bind /dev "${ROOTFS_DIR}/dev"
mount --bind /dev/pts "${ROOTFS_DIR}/dev/pts"

# 清理函数
cleanup_mounts() {
    umount "${ROOTFS_DIR}/dev/pts" 2>/dev/null || true
    umount "${ROOTFS_DIR}/dev" 2>/dev/null || true
    umount "${ROOTFS_DIR}/sys" 2>/dev/null || true
    umount "${ROOTFS_DIR}/proc" 2>/dev/null || true
}

trap cleanup_mounts EXIT

# 进入 chroot 环境安装包
log_info "更新包列表..."
chroot "${ROOTFS_DIR}" apt-get update -qq

log_info "安装 OpenSSH 服务器..."
chroot "${ROOTFS_DIR}" apt-get install -y -qq openssh-server

log_info "安装调试工具..."
# 安装核心调试工具
chroot "${ROOTFS_DIR}" apt-get install -y -qq \
    strace \
    gdb \
    ltrace \
    sysstat \
    util-linux \
    psmisc \
    lsof \
    net-tools \
    iproute2 \
    vim \
    nano \
    curl \
    wget \
    fio \
    e2fsprogs \
    xfsprogs \
    btrfs-progs \
    htop \
    2>/dev/null || true

# 尝试安装可选工具 (可能不存在)
chroot "${ROOTFS_DIR}" apt-get install -y -qq \
    linux-tools-generic \
    iozone3 \
    2>/dev/null || true

log_info "安装完成"

# 清理缓存
chroot "${ROOTFS_DIR}" apt-get clean
chroot "${ROOTFS_DIR}" rm -rf /var/lib/apt/lists/*

# 步骤 5: 配置 SSH 服务
log_info "步骤 5: 配置 SSH 服务..."

SSHD_CONFIG="${ROOTFS_DIR}/etc/ssh/sshd_config"

# 备份原配置
cp "${SSHD_CONFIG}" "${SSHD_CONFIG}.bak" 2>/dev/null

# 配置 SSH
cat > "${SSHD_CONFIG}" << 'SSHEOF'
# PowerFS VM SSH 配置
Port 22

# 监听地址
ListenAddress 0.0.0.0

# 主机密钥
HostKey /etc/ssh/ssh_host_rsa_key
HostKey /etc/ssh/ssh_host_ecdsa_key
HostKey /etc/ssh/ssh_host_ed25519_key

# 密钥算法 (兼容旧客户端)
HostKeyAlgorithms rsa-sha2-256,rsa-sha2-512,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,ssh-ed25519
PubkeyAcceptedKeyTypes rsa-sha2-256,rsa-sha2-512,ssh-rsa,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,ssh-ed25519

# 认证配置
PermitRootLogin yes
PasswordAuthentication yes
PubkeyAuthentication yes
AuthorizedKeysFile .ssh/authorized_keys

# 允许密码登录
KbdInteractiveAuthentication no
UsePAM yes

# 会话配置
X11Forwarding yes
PrintMotd no
AcceptEnv LANG LC_*

# 子系统
Subsystem sftp /usr/lib/openssh/sftp-server

# 日志
LogLevel INFO
SSHEOF

# 生成主机密钥 (先删除旧密钥避免交互提示)
log_info "生成 SSH 主机密钥..."
chroot "${ROOTFS_DIR}" rm -f /etc/ssh/ssh_host_*_key /etc/ssh/ssh_host_*_key.pub
chroot "${ROOTFS_DIR}" ssh-keygen -t rsa -b 4096 -f /etc/ssh/ssh_host_rsa_key -N "" -q
chroot "${ROOTFS_DIR}" ssh-keygen -t ecdsa -b 256 -f /etc/ssh/ssh_host_ecdsa_key -N "" -q
chroot "${ROOTFS_DIR}" ssh-keygen -t ed25519 -f /etc/ssh/ssh_host_ed25519_key -N "" -q

# 步骤 6: 配置用户
log_info "步骤 6: 配置用户..."

# 使用主机的用户配置
if [ -f /etc/passwd ] && [ -f /etc/shadow ]; then
    log_info "从主机拷贝用户配置..."
    cp /etc/passwd "${ROOTFS_DIR}/etc/passwd"
    cp /etc/shadow "${ROOTFS_DIR}/etc/shadow"
    cp /etc/group "${ROOTFS_DIR}/etc/group"
    cp /etc/gshadow "${ROOTFS_DIR}/etc/gshadow"
    
    # 修复 shell 路径
    sed -i 's|/bin/bash|/bin/bash|g' "${ROOTFS_DIR}/etc/passwd"
    sed -i 's|/usr/sbin/nologin|/usr/sbin/nologin|g' "${ROOTFS_DIR}/etc/passwd"
fi

# 创建 portion 用户的家目录
HOME_PORTION="${ROOTFS_DIR}/home/portion"
mkdir -p "${HOME_PORTION}"
chown -R 1001:1001 "${HOME_PORTION}"

# 配置 portion 用户的 SSH
SSH_DIR="${HOME_PORTION}/.ssh"
mkdir -p "${SSH_DIR}"
chmod 700 "${SSH_DIR}"

# 从主机拷贝公钥
if [ -f "${HOME}/.ssh/id_rsa.pub" ]; then
    cp "${HOME}/.ssh/id_rsa.pub" "${SSH_DIR}/authorized_keys"
    chmod 600 "${SSH_DIR}/authorized_keys"
    chown -R 1001:1001 "${SSH_DIR}"
fi

# 配置 root 用户的 SSH
ROOT_SSH="${ROOTFS_DIR}/root/.ssh"
mkdir -p "${ROOT_SSH}"
chmod 700 "${ROOT_SSH}"

if [ -f "${HOME}/.ssh/id_rsa.pub" ]; then
    cp "${HOME}/.ssh/id_rsa.pub" "${ROOT_SSH}/authorized_keys"
    chmod 600 "${ROOT_SSH}/authorized_keys"
    chown -R 0:0 "${ROOT_SSH}"
fi

# 设置 root 密码 (可选，用于密码登录)
echo "root:powerfs" | chroot "${ROOTFS_DIR}" chpasswd 2>/dev/null || true

# 步骤 7: 创建 init 启动脚本
log_info "步骤 7: 创建 init 启动脚本..."

cat > "${ROOTFS_DIR}/init" << 'INITEOF'
#!/bin/bash
# PowerFS VM init 脚本

export PATH=/bin:/sbin:/usr/bin:/usr/sbin

echo "=== PowerFS 内核调试 VM 启动中 ==="

# 挂载基本文件系统
echo "挂载文件系统..."
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mount -t tmpfs tmpfs /dev

# 创建必要的设备节点
[ ! -c /dev/console ] && mknod /dev/console c 5 1
[ ! -c /dev/null ] && mknod /dev/null c 1 3
[ ! -c /dev/tty ] && mknod /dev/tty c 5 0
[ ! -c /dev/zero ] && mknod /dev/zero c 1 5
[ ! -c /dev/ptmx ] && mknod /dev/ptmx c 5 2

# 创建并挂载 devpts
mkdir -p /dev/pts
chmod 755 /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null

# 设置权限
chmod 666 /dev/ptmx 2>/dev/null

# 挂载其他文件系统
mount -a 2>/dev/null

# 设置主机名
hostname powerfs-vm

# 设置网络 (可选)
# ifconfig eth0 up 192.168.1.100 netmask 255.255.255.0

# 显示系统信息
echo ""
echo "=== 系统信息 ==="
echo "内核版本: $(uname -r)"
echo "系统时间: $(date)"
echo ""

# 启动 SSH 服务
echo "启动 OpenSSH 服务..."
mkdir -p /run/sshd
chmod 755 /run/sshd

/usr/sbin/sshd -D &
SSHD_PID=$!
echo "SSHD 已启动 (PID: $SSHD_PID)"

# 加载 PowerFS 内核模块 (如果存在)
if [ -f /powerfs.ko ]; then
    echo "加载 PowerFS 内核模块..."
    insmod /powerfs.ko
    if [ $? -eq 0 ]; then
        echo "PowerFS 模块加载成功"
    else
        echo "PowerFS 模块加载失败"
    fi
fi

echo ""
echo "=== PowerFS VM 已就绪 ==="
echo "SSH 连接: ssh -p 2222 root@localhost"
echo ""

# 启动 shell
exec /bin/bash
INITEOF

chmod +x "${ROOTFS_DIR}/init"

# 步骤 8: 禁用 systemd (确保我们的 /init 脚本被执行)
log_info "步骤 8: 禁用 systemd..."

# 备份并禁用 systemd 的 init
if [ -f "${ROOTFS_DIR}/sbin/init" ] || [ -L "${ROOTFS_DIR}/sbin/init" ]; then
    mv "${ROOTFS_DIR}/sbin/init" "${ROOTFS_DIR}/sbin/init.systemd.bak" 2>/dev/null
fi

# 确保 /init 是默认执行的 init
# 创建 /sbin/init 符号链接指向我们的 /init
ln -sf "/init" "${ROOTFS_DIR}/sbin/init" 2>/dev/null || true

# 禁用 systemd 的 systemctl (可选)
if [ -f "${ROOTFS_DIR}/bin/systemctl" ]; then
    mv "${ROOTFS_DIR}/bin/systemctl" "${ROOTFS_DIR}/bin/systemctl.bak" 2>/dev/null
fi

# 清理不需要的 systemd 文件以减小体积
rm -rf "${ROOTFS_DIR}/lib/systemd/system/multi-user.target" 2>/dev/null
rm -rf "${ROOTFS_DIR}/lib/systemd/system/sysinit.target.wants" 2>/dev/null

# 步骤 9: 打包为 initramfs
log_info "步骤 9: 打包为 initramfs..."

cd "${ROOTFS_DIR}"

# 创建 cpio 归档
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "${INITRAMFS_FILE}"

cd - > /dev/null

INITRAMFS_SIZE=$(du -h "${INITRAMFS_FILE}" | cut -f1)
log_info "initramfs 大小: ${INITRAMFS_SIZE}"
log_info "initramfs 路径: ${INITRAMFS_FILE}"

# 步骤 10: 清理
log_info "清理..."
cleanup_mounts

# 显示结果
echo ""
echo "========================================"
echo "  Ubuntu 根文件系统构建完成!"
echo "========================================"
echo ""
echo "输出文件:"
echo "  initramfs: ${INITRAMFS_FILE}"
echo "  大小: ${INITRAMFS_SIZE}"
echo ""
echo "使用方法:"
echo "  1. 启动 VM: cd ${OUTPUT_DIR} && ../run_qemu_ubuntu.sh"
echo "  2. SSH 连接: ssh -p 2222 -i ~/.ssh/id_rsa root@localhost"
echo ""
