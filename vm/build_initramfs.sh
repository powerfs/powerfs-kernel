#!/bin/bash
# PowerFS initramfs 构建脚本
# 使用 BusyBox 创建最小化根文件系统，用于 QEMU 启动
# 注意: 构建过程中用 sudo 确保文件所有权为 root，否则 VM 中 sshd 无法读取

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/output"
INITRAMFS_DIR="${OUTPUT_DIR}/initramfs"
INITRAMFS_IMG="${OUTPUT_DIR}/initramfs.cpio.gz"
POWERFS_MOD_DIR="${SCRIPT_DIR}/../powerfs_mod"

echo "=== PowerFS initramfs 构建 ==="

# 清理旧的 initramfs (使用 sudo, 因上次构建可能留下 root 所有权文件)
sudo rm -rf "${INITRAMFS_DIR}"
mkdir -p "${INITRAMFS_DIR}"

# 创建基础目录结构
echo "=== 创建目录结构 ==="
cd "${INITRAMFS_DIR}"
mkdir -p bin sbin etc proc sys dev dev/pts tmp var/log var/run mnt run/sshd var/empty
mkdir -p root home
mkdir -p lib lib64
mkdir -p etc/init.d
mkdir -p etc/modprobe.d
mkdir -p etc/ssh

# 注意: 不要在创建文件之前 chown，否则用户没有写入权限
# sudo chown 会在打包前执行

# 创建基本的系统配置文件 (从主机拷贝用户配置)
# 使用主机的 /etc/passwd, /etc/shadow, /etc/group, /etc/gshadow
# 完整拷贝，确保用户、用户组、密码等配置与主机一致

# 首先从主机拷贝配置 (使用 sudo 来读取受保护的文件)
if [ -f /etc/passwd ] && sudo [ -f /etc/shadow ]; then
    echo "=== 从主机完整拷贝用户配置 ==="
    # 完整拷贝主机配置文件
    sudo cp /etc/passwd etc/passwd
    sudo cp /etc/shadow etc/shadow
    sudo cp /etc/group etc/group
    sudo cp /etc/gshadow etc/gshadow
    
    # 修复 shell 路径 (initramfs 中只有 /bin/sh，没有 /bin/bash)
    echo "修复用户 shell 路径..."
    sudo sed -i 's|/bin/bash|/bin/sh|g' etc/passwd
    sudo sed -i 's|/usr/sbin/nologin|/sbin/nologin|g' etc/passwd
    # 确保 root 和 portion 使用 /bin/sh
    sudo sed -i 's|:root:/root:/bin/sh|:root:/root:/bin/sh|g' etc/passwd
    sudo sed -i 's|portion:x:1001:1001:[^:]*:/home/portion:[^:]*|portion:x:1001:1001:jjh,,,:/home/portion:/bin/sh|g' etc/passwd
    
    # 设置正确的权限和所有权
    sudo chown root:root etc/passwd etc/shadow etc/group etc/gshadow
    sudo chmod 644 etc/passwd
    sudo chmod 640 etc/shadow
    sudo chmod 644 etc/group
    sudo chmod 640 etc/gshadow
    
    echo "已完整从主机拷贝用户配置"
    echo "passwd 行数: $(wc -l < etc/passwd)"
    echo "shadow 行数: $(wc -l < etc/shadow)"
    echo "group 行数: $(wc -l < etc/group)"
    echo "gshadow 行数: $(wc -l < etc/gshadow)"
    echo ""
    echo "关键用户 (shell 已修复):"
    grep -E '^(root|portion|sshd):' etc/passwd
    echo "组列表:"
    cat etc/group
else
    echo "主机用户配置不存在，使用默认配置"
    sudo bash -c "cat > etc/passwd << 'EOF'
root::0:0:root:/root:/bin/bash
portion:x:1001:1001:jjh,,,:/home/portion:/bin/bash
sshd:x:128:65534::/run/sshd:/usr/sbin/nologin
EOF"
    sudo bash -c "cat > etc/shadow << 'EOF'
root:!:10000:0:99999:7:::
portion:!:10000:0:99999:7:::
sshd:*:10000:0:99999:7:::
EOF"
    sudo bash -c "cat > etc/group << 'EOF'
root:x:0:
portion:x:1001:
nogroup:x:65534:
EOF"
    sudo bash -c "cat > etc/gshadow << 'EOF'
root:!:10000:0:99999:7:::
portion:!:10000:0:99999:7:::
nogroup:!::
EOF"
    sudo chown root:root etc/passwd etc/shadow etc/group etc/gshadow
    sudo chmod 644 etc/passwd
    sudo chmod 640 etc/shadow
    sudo chmod 644 etc/group
    sudo chmod 640 etc/gshadow
fi

# 创建 /etc/nsswitch.conf (glibc NSS 配置，sshd 查找用户需要)
cat > etc/nsswitch.conf << 'NSSHEOF'
passwd: files
group: files
shadow: files
gshadow: files
hosts: files
networks: files
NSSHEOF

# 创建 home 目录和运行时目录
sudo mkdir -p home/portion
sudo mkdir -p run/sshd
sudo mkdir -p var/empty

# 预创建 root 用户的 SSH 配置
sudo mkdir -p root/.ssh
sudo chown root:root root/.ssh
sudo chmod 700 root/.ssh

# 预创建 portion 用户的 SSH 配置
sudo mkdir -p home/portion/.ssh
sudo chown 1001:1001 home/portion/.ssh
sudo chmod 700 home/portion/.ssh

# 预创建 authorized_keys (使用主机公钥)
SSH_PUBKEY="${HOME}/.ssh/id_rsa.pub"
if sudo [ -f "${SSH_PUBKEY}" ]; then
    sudo cp "${SSH_PUBKEY}" root/.ssh/authorized_keys
    sudo chown root:root root/.ssh/authorized_keys
    sudo chmod 600 root/.ssh/authorized_keys
    
    sudo cp "${SSH_PUBKEY}" home/portion/.ssh/authorized_keys
    sudo chown 1001:1001 home/portion/.ssh/authorized_keys
    sudo chmod 600 home/portion/.ssh/authorized_keys
    
    echo "已预创建 SSH authorized_keys"
else
    echo "警告: 未找到主机 SSH 公钥 ${SSH_PUBKEY}"
fi

sudo chown 1001:1001 home/portion 2>/dev/null || true
sudo chmod 755 home/portion
sudo chmod 755 run/sshd
sudo chmod 555 var/empty

# 创建 /usr/sbin/nologin (如果主机上有)
if [ -f /usr/sbin/nologin ]; then
    cp /usr/sbin/nologin sbin/nologin 2>/dev/null || true
fi
if [ -f /bin/false ]; then
    cp /bin/false bin/false 2>/dev/null || true
fi

# 复制 BusyBox
echo "=== 安装 BusyBox ==="
BUSYBOX_PATH=$(which busybox 2>/dev/null || echo "/usr/bin/busybox")
if [ -f "${BUSYBOX_PATH}" ]; then
    cp "${BUSYBOX_PATH}" bin/busybox
    # 创建符号链接 (常用命令)
    for cmd in ash cat ls cp mv rm mkdir mount umount ps echo cat grep chmod chown \
               ln sleep kill sh bash date dd df du free killall more ping rmdir \
               stat sync top true false yes env env find halt init insmod \
               ip lsmod modprobe netstat poweroff reboot rmmod route \
               stty swapoff swapon telnet test uname uptime watch wc wget \
               whoami xargs yes zcat chroot clear cmp comm cut diff dirname \
               head less sed tail tr uniq awk basename cal dir expr factor \
               groups id printenv printf seq sleep tee time timeout tty \
               wc wget which who xxd strings strace ltrace; do
        if bin/busybox | grep -q "${cmd}"; then
            ln -sf busybox bin/${cmd} 2>/dev/null || true
        fi
    done
else
    echo "警告: BusyBox 未找到，使用 /bin/sh"
fi

# 创建必要的设备节点
echo "=== 创建设备节点 ==="
mknod dev/console c 5 1 2>/dev/null || echo "需要 sudo 创建设备节点"
mknod dev/null c 1 3 2>/dev/null || echo "需要 sudo 创建设备节点"
mknod dev/tty c 5 0 2>/dev/null || echo "需要 sudo 创建设备节点"
mknod dev/zero c 1 5 2>/dev/null || echo "需要 sudo 创建设备节点"
mknod dev/random c 1 8 2>/dev/null || echo "需要 sudo 创建设备节点"
mknod dev/urandom c 1 9 2>/dev/null || echo "需要 sudo 创建设备节点"

# 如果 mknod 失败 (无权限)，创建一个脚本来在启动时创建
if [ ! -c dev/console ]; then
    cat > etc/init.d/01-create-devs << 'DEVEOF'
#!/bin/sh
# 创建设备节点 (如果未预先创建)
mknod /dev/console c 5 1
mknod /dev/null c 1 3
mknod /dev/tty c 5 0
mknod /dev/zero c 1 5
mknod /dev/random c 1 8
mknod /dev/urandom c 1 9
DEVEOF
fi

# 创建 init 脚本 (系统启动入口)
echo "=== 创建 init 脚本 ==="
cat > init << 'INITEOF'
#!/bin/sh
# PowerFS initramfs init 脚本
# 抛弃用户态代理方式，直接使用内核态 powerfs-net 通信

echo ""
echo "=========================================="
echo "  PowerFS Virtual Environment"
echo "  Kernel Filesystem (powerfs-net)"
echo "=========================================="
echo ""

# 设置 PATH
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
export HOME=/root

# 挂载基本文件系统 (先挂载 proc 才能读取 /proc/cmdline)
echo "挂载 proc, sysfs..."
mount -t proc proc /proc
mount -t sysfs sysfs /sys

# 从内核命令行解析 PowerFS Master 地址
# 架构: 只需配置 Master 地址 (3 个 Raft 节点), Filer/Volume 地址
# 全部通过 Master 动态发现 (GetTopology / ListFilers)
# QEMU 启动时通过 -append 传递:
#   powerfs_master_addr=172.30.0.11,172.30.0.12,172.30.0.13  (master 地址)
#   powerfs_master_port=9334                                  (master net_port)
CMDLINE=$(cat /proc/cmdline 2>/dev/null)
POWERFS_MASTER_ADDR=$(echo "$CMDLINE" | grep -o 'powerfs_master_addr=[^ ]*' | head -1 | cut -d= -f2)
POWERFS_MASTER_PORT=$(echo "$CMDLINE" | grep -o 'powerfs_master_port=[^ ]*' | head -1 | cut -d= -f2)
echo "内核命令行: $CMDLINE"

# 如果未指定，使用默认值
if [ -z "$POWERFS_MASTER_ADDR" ]; then
    POWERFS_MASTER_ADDR="172.30.0.11,172.30.0.12,172.30.0.13"
    echo "[WARN] 未指定 powerfs_master_addr，使用默认: $POWERFS_MASTER_ADDR"
fi
if [ -z "$POWERFS_MASTER_PORT" ]; then
    POWERFS_MASTER_PORT="9334"
fi

echo "[INFO] PowerFS 后端:"
echo "  Master:            ${POWERFS_MASTER_ADDR}:${POWERFS_MASTER_PORT}"
echo "  Filer/Volume:      通过 Master 动态发现"

# 使用 tmpfs 挂载 /dev
echo "挂载 tmpfs 到 /dev..."
mkdir -p /dev
mount -t tmpfs tmpfs /dev

# 创建设备节点
echo "创建设备节点..."
mknod /dev/console c 5 1 2>/dev/null
mknod /dev/null c 1 3 2>/dev/null
mknod /dev/tty c 5 0 2>/dev/null
mknod /dev/zero c 1 5 2>/dev/null
mknod /dev/full c 1 7 2>/dev/null
mknod /dev/random c 1 8 2>/dev/null
mknod /dev/urandom c 1 9 2>/dev/null
mknod /dev/ptmx c 5 2 2>/dev/null
chmod 666 /dev/ptmx 2>/dev/null

# 创建 /dev/pts 目录
mkdir -p /dev/pts
chmod 755 /dev/pts

# 挂载 devpts
echo "挂载 devpts (PTY 支持)..."
mount -t devpts devpts /dev/pts 2>/dev/null || mount -t devpts -o newinstance devpts /dev/pts 2>/dev/null

# 设置控制台
stty raw -echo < /dev/console 2>/dev/null

# 配置网络
# eth0: virtio-net-pci (TAP -> Docker 网桥 powerfs-network, IP: 172.30.0.100)
# Docker powerfs-network 网段为 172.30.0.0/16, 网关 172.30.0.1,
# tap0 已由 setup_network.sh 桥接到 br-xxx (172.30.0.1).
# 注意: 之前误用 172.20.0.100/24 + 网关 172.20.0.1, 与实际网桥网段不匹配,
# 导致 VM 无法访问 Docker 容器 (filer 等).
echo "配置网络..."
if [ -x /bin/ip ]; then
    # eth0: TAP 网络 (用于访问 Docker 容器, 必须与 powerfs-network 同网段)
    ip link set eth0 up 2>/dev/null || true
    ip addr add 172.30.0.100/16 dev eth0 2>/dev/null || true

    # 默认路由通过 TAP 网桥 (172.30.0.1 是 Docker 网桥网关)
    ip route del default 2>/dev/null || true
    ip route add default via 172.30.0.1 dev eth0 2>/dev/null || true

    # 打印网络配置
    echo "  eth0 (TAP -> powerfs-network):"
    ip addr show eth0 2>/dev/null | grep inet || echo "    未配置"

    # eth1: QEMU 用户网络 (SSH 端口转发 host:2223 -> guest:22)
    # QEMU user-net 默认网段 10.0.2.0/24, guest 用 10.0.2.15
    # 不配置则 hostfwd 转发的包无法到达 sshd (banner exchange timeout)
    ip link set eth1 up 2>/dev/null || true
    ip addr add 10.0.2.15/24 dev eth1 2>/dev/null || true
    echo "  eth1 (QEMU user-net -> SSH forward):"
    ip addr show eth1 2>/dev/null | grep inet || echo "    未配置"
fi

# 等待网络就绪
sleep 1

# 挂载 Host 共享目录 (9p virtfs, 用于快速部署 powerfs.ko 和测试脚本)
echo "挂载 Host 共享目录 (9p virtfs)..."
mkdir -p /mnt/host
mount -t 9p -o trans=virtio,version=9p2000.L hostshare /mnt/host 2>/dev/null
if [ $? -eq 0 ]; then
    echo "[OK] Host 共享目录已挂载: /mnt/host"
else
    echo "[INFO] 9p 挂载失败 (可忽略, 不影响基本功能)"
fi

# 加载 PowerFS 内核模块 (只需 Master 地址, Filer/Volume 通过 Master 动态发现)
# 优先从 9p 共享目录加载 (hot deploy), 回退到 initramfs 内置的 powerfs.ko
POWERFS_KO=""
if [ -f /mnt/host/powerfs.ko ]; then
    POWERFS_KO="/mnt/host/powerfs.ko"
    echo "[INFO] 使用 9p 共享目录的 powerfs.ko (hot deploy)"
elif [ -f /powerfs.ko ]; then
    POWERFS_KO="/powerfs.ko"
    echo "[INFO] 使用 initramfs 内置的 powerfs.ko"
fi

if [ -n "$POWERFS_KO" ]; then
    echo "加载 PowerFS 内核模块 (powerfs-net 模式)..."
    echo "  Master:            ${POWERFS_MASTER_ADDR}:${POWERFS_MASTER_PORT}"
    echo "  Filer/Volume:      通过 Master 动态发现"
    echo "  模块路径:          ${POWERFS_KO}"

    # 加载模块, 只传递 master_addr/master_port
    # Filer 和 Volume 地址在 fill_super 时通过 Master 动态发现
    insmod "${POWERFS_KO}" \
        master_addr="${POWERFS_MASTER_ADDR}" \
        master_port="${POWERFS_MASTER_PORT}" \
        shard_count="${POWERFS_SHARD_COUNT:-2}"

    if [ $? -eq 0 ]; then
        echo "[OK] PowerFS 模块加载成功 (Master 动态发现 filer/volume)"
    else
        echo "[FAIL] PowerFS 模块加载失败!"
        dmesg | tail -20
    fi
fi

# 显示模块信息
echo ""
echo "内核模块:"
lsmod 2>/dev/null || echo "(lsmod 不可用)"

# 设置 root 密码 (用于密码登录)
echo "root:powerfs" | chpasswd 2>/dev/null || \
echo "root:\$1\$powerfs\$abcdefghijklmnopqrstuv" | chpasswd -e 2>/dev/null || true

# 启动 OpenSSH SSH 服务器
if [ -x /sbin/sshd ]; then
    echo "启动 OpenSSH SSH 服务器..."
    
    mkdir -p /var/empty /run/sshd
    chmod 555 /var/empty
    chmod 755 /run/sshd
    
    # 修复权限
    chown root:root /root/.ssh 2>/dev/null
    chmod 700 /root/.ssh 2>/dev/null
    chown root:root /root/.ssh/authorized_keys 2>/dev/null
    chmod 600 /root/.ssh/authorized_keys 2>/dev/null
    chown 1001:1001 /home/portion/.ssh 2>/dev/null
    chmod 700 /home/portion/.ssh 2>/dev/null
    
    # 修复主机密钥权限
    for key_file in /etc/ssh/ssh_host_*_key; do
        [ -f "${key_file}" ] && chown root:root "${key_file}" && chmod 600 "${key_file}"
    done
    for pub_key in /etc/ssh/ssh_host_*_key.pub; do
        [ -f "${pub_key}" ] && chown root:root "${pub_key}" && chmod 644 "${pub_key}"
    done
    
    # 确保 SSHD 配置允许 root 密码登录
    cat > /etc/ssh/sshd_config << 'SSHEOF'
Port 22
ListenAddress 0.0.0.0
PermitRootLogin yes
PasswordAuthentication yes
PubkeyAuthentication yes
AuthorizedKeysFile .ssh/authorized_keys
PermitEmptyPasswords no
ChallengeResponseAuthentication no
UsePAM no
Subsystem sftp /usr/lib/openssh/sftp-server
SSHEOF
    
    /sbin/sshd -f /etc/ssh/sshd_config -D -e &
    SSHD_PID=$!
    sleep 1
    
    if kill -0 ${SSHD_PID} 2>/dev/null; then
        echo "[OK] SSH 服务器已启动 (PID: ${SSHD_PID})"
        echo "  root 密码: powerfs"
    else
        echo "[FAIL] SSH 启动失败!"
    fi
fi

# 显示内核信息
echo ""
echo "内核版本: $(uname -r)"
echo "系统时间: $(date)"
echo ""
echo "=========================================="
echo "  PowerFS powerfs-net 开发环境就绪"
echo "=========================================="
echo ""
echo "后端服务器: ${POWERFS_ADDR}:${POWERFS_PORT}"
echo ""
echo "常用命令:"
echo "  lsmod                           - 查看已加载模块"
echo "  dmesg | tail -50                - 查看内核日志"
echo "  cat /proc/filesystems | grep powerfs  - 检查 powerfs 文件系统"
echo "  mount -t powerfs none /mnt/pfs  - 挂载 PowerFS"
echo "  echo 'hello' > /mnt/pfs/test.txt - 写入测试"
echo "  cat /mnt/pfs/test.txt            - 读取测试"
echo ""

# 启动交互式 shell
if [ -x /bin/sh ]; then
    /bin/sh
elif [ -x /bin/ash ]; then
    /bin/ash
else
    while true; do sleep 1; done
fi

# 关机
umount -a 2>/dev/null
poweroff -f 2>/dev/null || halt -f 2>/dev/null
INITEOF

chmod +x init

# 添加 PowerFS 模块
# 优先从编译目录复制最新 powerfs.ko, 回退到 output 目录缓存
echo "=== 添加 PowerFS 模块 ==="
POWERFS_KO=""
if [ -f "${POWERFS_MOD_DIR}/powerfs.ko" ]; then
    POWERFS_KO="${POWERFS_MOD_DIR}/powerfs.ko"
    echo "  从编译目录复制最新 powerfs.ko"
elif [ -f "${OUTPUT_DIR}/powerfs.ko" ]; then
    POWERFS_KO="${OUTPUT_DIR}/powerfs.ko"
    echo "  [WARN] 编译目录无 powerfs.ko, 使用 output 目录缓存 (可能过期)"
fi
if [ -n "${POWERFS_KO}" ]; then
    cp "${POWERFS_KO}" powerfs.ko
    # 同步到 output 目录供下次回退使用
    cp "${POWERFS_KO}" "${OUTPUT_DIR}/powerfs.ko"
    echo "  已添加 powerfs.ko 到 initramfs ($(ls -la powerfs.ko | awk '{print $5}') bytes)"
else
    echo "  [ERROR] powerfs.ko 未找到, 请先在 ${POWERFS_MOD_DIR} 执行 make"
    exit 1
fi

# 添加 OpenSSH SSH 服务器 (替代 dropbear)
echo "=== 添加 OpenSSH SSH 服务器 ==="
SSHD_PATH=$(which sshd 2>/dev/null || echo "/usr/sbin/sshd")
if [ -f "${SSHD_PATH}" ]; then
    # 1. 复制 sshd 二进制
    cp "${SSHD_PATH}" sbin/sshd
    chmod +x sbin/sshd
    echo "已添加 sshd 到 initramfs"
    
    # 2. 复制 sshd 所需的动态库
    echo "复制 sshd 动态库..."
    # libnss_files 是 OpenSSH 查找用户必需的库
    NSS_FILES_LIB=$(find /usr/lib -name "libnss_files.so.*" 2>/dev/null | grep -v "\.a$" | head -1)
    if [ -n "${NSS_FILES_LIB}" ]; then
        cp "${NSS_FILES_LIB}" lib/
        echo "  已添加 libnss_files (用户查找必需)"
    fi
    for lib in libwrap.so.0 libselinux.so.1 libsystemd.so.0 libutil.so.1 libz.so.1 libcrypt.so.1 libgssapi_krb5.so.2 libkrb5.so.3 libcom_err.so.2 libcrypto.so.1.1 libc.so.6 libpthread.so.0 libdl.so.2 librt.so.1 libm.so.6 libcap.so.2 libcap-ng.so.0 libpam.so.0 libpam_misc.so.0 libaudit.so.1 libkeyutils.so.1 libnsl.so.1 libtinfo.so.5 libtinfo.so.6 libpcre2-8.so.0 liblzma.so.5 liblz4.so.1 libgcrypt.so.20 libk5crypto.so.3 libkrb5support.so.0 libresolv.so.2 libgpg-error.so.0; do
        lib_path=$(find /lib /usr/lib /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu -name "${lib}" 2>/dev/null | head -1)
        if [ -n "${lib_path}" ]; then
            cp "${lib_path}" lib/ 2>/dev/null
            echo "  已添加 ${lib}"
        fi
    done
    # 复制 ld-linux
    cp /lib64/ld-linux-x86-64.so.2 lib64/ 2>/dev/null
    
    # 3. 创建 /etc/ssh 目录和配置文件
    mkdir -p etc/ssh
    cat > etc/ssh/sshd_config << 'SSHEOF'
# OpenSSH Server Configuration for PowerFS VM
Port 22
Protocol 2

# 认证配置
PermitRootLogin yes
PubkeyAuthentication yes
AuthorizedKeysFile .ssh/authorized_keys
PasswordAuthentication yes
PermitEmptyPasswords no
ChallengeResponseAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no

# 启用 RSA 算法 (OpenSSH 8.2 默认禁用 ssh-rsa)
# HostKeyAlgorithms 允许主机密钥使用 RSA
HostKeyAlgorithms rsa-sha2-256,rsa-sha2-512,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,ssh-ed25519
# PubkeyAcceptedKeyTypes 允许客户端使用 RSA 公钥
PubkeyAcceptedKeyTypes rsa-sha2-256,rsa-sha2-512,ssh-rsa,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,ssh-ed25519

# 注意: OpenSSH 8.2+ 已废弃 UsePrivilegeSeparation，不再需要配置

# 会话配置
X11Forwarding no
AllowTcpForwarding no
PrintMotd no
AcceptEnv LANG LC_*

# 日志
SyslogFacility AUTH
LogLevel INFO

# 子系统
Subsystem sftp /usr/lib/openssh/sftp-server

# 允许的用户
AllowUsers root portion
SSHEOF
    
    # 4. 生成主机密钥
    SSH_KEY_DIR="${OUTPUT_DIR}/ssh_host_keys"
    mkdir -p "${SSH_KEY_DIR}"
    
    for key_type in rsa ecdsa ed25519; do
        key_file="ssh_host_${key_type}_key"
        if [ ! -f "${SSH_KEY_DIR}/${key_file}" ]; then
            echo "生成 ${key_type} 主机密钥..."
            ssh-keygen -t "${key_type}" -f "${SSH_KEY_DIR}/${key_file}" -N "" -q 2>/dev/null || true
        fi
        if [ -f "${SSH_KEY_DIR}/${key_file}" ]; then
            cp "${SSH_KEY_DIR}/${key_file}" "etc/ssh/${key_file}" 2>/dev/null || true
            cp "${SSH_KEY_DIR}/${key_file}.pub" "etc/ssh/${key_file}.pub" 2>/dev/null || true
        fi
    done
    
    # 5. 拷贝 Host 的 SSH 公钥 (用于免密登录)
    SSH_PUBKEY="${HOME}/.ssh/id_rsa.pub"
    if [ -f "${SSH_PUBKEY}" ]; then
        # 存储公钥到一个临时位置，供 init 脚本使用
        cp "${SSH_PUBKEY}" etc/host_rsa_pubkey.pub
        echo "已保存 Host SSH 公钥到 initramfs (id_rsa.pub)"
    else
        echo "提示: 未找到 Host SSH 公钥，需要手动配置"
    fi
    
    # 6. 创建 /run/sshd 目录 (运行时需要)
    mkdir -p run/sshd
    echo "已添加 OpenSSH sshd 配置"
else
    echo "提示: sshd 未安装，跳过 SSH 服务器配置"
    echo "      运行: sudo apt-get install openssh-server"
fi

# 注意: 已抛弃用户态代理方式，改用内核态 powerfs-net 直接通信
# 不再需要 powerfs_proxy 二进制程序

# 添加静态 fio 二进制 (用于 T5 性能测试)
echo "=== 添加 fio 性能测试工具 ==="
FIO_BIN=""
# 优先使用预构建的静态 fio
for fio_path in /tmp/fio-build/fio /tmp/fio_bundle/fio "${OUTPUT_DIR}/fio"; do
    if [ -f "${fio_path}" ]; then
        FIO_BIN="${fio_path}"
        break
    fi
done
if [ -n "${FIO_BIN}" ]; then
    cp "${FIO_BIN}" bin/fio
    chmod +x bin/fio
    # 同步到 output 目录缓存
    cp "${FIO_BIN}" "${OUTPUT_DIR}/fio" 2>/dev/null || true
    echo "  已添加 fio 到 initramfs ($(ls -la bin/fio | awk '{print $5}') bytes)"
else
    echo "  [WARN] 未找到 fio 二进制, T5 性能测试将需要手动安装"
    echo "  预期路径: /tmp/fio-build/fio (静态构建)"
fi

# 创建简单的挂载工具
cat > bin/mount_powerfs << 'MEOF'
#!/bin/sh
# PowerFS 快捷挂载脚本
if [ $# -lt 1 ]; then
    echo "用法: mount_powerfs <device> [mountpoint]"
    echo "示例: mount_powerfs /dev/sda1 /mnt"
    exit 1
fi

DEVICE=$1
MOUNTPOINT=${2:-/mnt}

echo "挂载 PowerFS: ${DEVICE} -> ${MOUNTPOINT}"
mkdir -p "${MOUNTPOINT}"
mount -t powerfs "${DEVICE}" "${MOUNTPOINT}"
if [ $? -eq 0 ]; then
    echo "挂载成功!"
else
    echo "挂载失败!"
fi
MEOF
chmod +x bin/mount_powerfs

# 创建调试辅助脚本
cat > bin/fs_debug << 'FEOF'
#!/bin/sh
# PowerFS 文件系统调试工具
echo "=== PowerFS 调试信息 ==="
echo ""
echo "内核日志 (最后50行):"
dmesg | tail -50
echo ""
echo "已加载模块:"
lsmod | grep -i powerfs
echo ""
echo "挂载的文件系统:"
mount | grep -i powerfs
echo ""
echo "/proc/filesystems:"
cat /proc/filesystems | grep -i powerfs
FEOF
chmod +x bin/fs_debug

# === 关键: 打包前修复文件所有权和权限 ===
echo "=== 修复文件所有权 ==="

# 创建必要的目录结构
sudo mkdir -p "${INITRAMFS_DIR}/home/portion/.ssh" 2>/dev/null || true
sudo mkdir -p "${INITRAMFS_DIR}/root/.ssh" 2>/dev/null || true

# 修复所有文件的所有权为 root:root (除了用户家目录)
sudo chown -R root:root "${INITRAMFS_DIR}/etc" 2>/dev/null || true
sudo chown -R root:root "${INITRAMFS_DIR}/sbin" 2>/dev/null || true
sudo chown -R root:root "${INITRAMFS_DIR}/bin" 2>/dev/null || true
sudo chown -R root:root "${INITRAMFS_DIR}/lib" 2>/dev/null || true
sudo chown -R root:root "${INITRAMFS_DIR}/lib64" 2>/dev/null || true
sudo chown -R root:root "${INITRAMFS_DIR}/root" 2>/dev/null || true
sudo chown -R root:root "${INITRAMFS_DIR}/var" 2>/dev/null || true
sudo chown -R root:root "${INITRAMFS_DIR}/run" 2>/dev/null || true

# 设置 portion 用户的家目录所有权
sudo chown -R 1001:1001 "${INITRAMFS_DIR}/home/portion" 2>/dev/null || true

# 设置正确的权限
sudo chmod 644 "${INITRAMFS_DIR}/etc/passwd" 2>/dev/null || true
sudo chmod 640 "${INITRAMFS_DIR}/etc/shadow" 2>/dev/null || true
sudo chmod 644 "${INITRAMFS_DIR}/etc/group" 2>/dev/null || true
sudo chmod 640 "${INITRAMFS_DIR}/etc/gshadow" 2>/dev/null || true

# SSH 相关权限
sudo chmod 600 "${INITRAMFS_DIR}/etc/ssh/ssh_host_rsa_key" 2>/dev/null || true
sudo chmod 600 "${INITRAMFS_DIR}/etc/ssh/ssh_host_ecdsa_key" 2>/dev/null || true
sudo chmod 600 "${INITRAMFS_DIR}/etc/ssh/ssh_host_ed25519_key" 2>/dev/null || true
sudo chmod 644 "${INITRAMFS_DIR}/etc/ssh/ssh_host_rsa_key.pub" 2>/dev/null || true
sudo chmod 644 "${INITRAMFS_DIR}/etc/ssh/ssh_host_ecdsa_key.pub" 2>/dev/null || true
sudo chmod 644 "${INITRAMFS_DIR}/etc/ssh/ssh_host_ed25519_key.pub" 2>/dev/null || true
sudo chmod 644 "${INITRAMFS_DIR}/etc/ssh/sshd_config" 2>/dev/null || true

# root 用户 SSH 目录
sudo chmod 700 "${INITRAMFS_DIR}/root/.ssh" 2>/dev/null || true

# portion 用户 SSH 目录
sudo chmod 700 "${INITRAMFS_DIR}/home/portion/.ssh" 2>/dev/null || true

# sshd 所需目录
sudo chown root:root "${INITRAMFS_DIR}/var/empty" 2>/dev/null || true
sudo chmod 555 "${INITRAMFS_DIR}/var/empty" 2>/dev/null || true
sudo chmod 755 "${INITRAMFS_DIR}/run/sshd" 2>/dev/null || true

# 确保 nologin 文件存在且可执行
if [ ! -f "${INITRAMFS_DIR}/sbin/nologin" ]; then
    sudo cp /usr/sbin/nologin "${INITRAMFS_DIR}/sbin/nologin" 2>/dev/null || true
    sudo chmod 755 "${INITRAMFS_DIR}/sbin/nologin" 2>/dev/null || true
fi

echo "检查关键文件权限:"
ls -la "${INITRAMFS_DIR}/etc/passwd" "${INITRAMFS_DIR}/etc/shadow" "${INITRAMFS_DIR}/etc/group" 2>&1
ls -la "${INITRAMFS_DIR}/etc/ssh/" 2>&1
echo ""

# 打包 initramfs
echo "=== 打包 initramfs ==="
cd "${INITRAMFS_DIR}"
# 使用 sudo 执行 cpio，因为有些文件权限为 600
sudo find . -print0 | sudo cpio --null -ov --format=newc --owner root:root 2>/dev/null | gzip -9 > "${INITRAMFS_IMG}"

echo ""
echo "=== initramfs 构建完成 ==="
echo "initramfs 镜像: ${INITRAMFS_IMG}"
ls -lh "${INITRAMFS_IMG}"
echo ""
echo "下一步: 运行 ./run_qemu.sh 启动虚拟机"
echo "       或使用 GDB 调试: ./run_qemu_debug.sh"
