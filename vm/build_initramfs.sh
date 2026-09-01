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
    
    # 修复 shell 路径 (initramfs 已打包 bash，/bin/sh 是 bash symlink):
    #   1. 先把 /usr/sbin/nologin → /sbin/nologin (initramfs 中只有后者)
    #   2. 非 root/portion 且 shell=/bin/bash 的用户降到 /bin/sh (服务账户用 symlink 即可)
    #   3. 最后强制 root/portion → /bin/bash (这样直接进 bash 跑 #!/bin/bash 脚本更自然)
    echo "修复用户 shell 路径..."
    sudo sed -i 's|/usr/sbin/nologin|/sbin/nologin|g' etc/passwd
    sudo awk -F: 'BEGIN{OFS=":"} {
        if ($1 == "root") {
            $7 = "/bin/bash"
        } else if ($1 == "portion") {
            $7 = "/bin/bash"
            $5 = "jjh,,,"
        } else if ($7 == "/bin/bash") {
            $7 = "/bin/sh"
        }
        print
    }' etc/passwd > etc/passwd.tmp && sudo mv etc/passwd.tmp etc/passwd
    sudo chown root:root etc/passwd && sudo chmod 644 etc/passwd
    
    # 强制解锁 root shadow 条目并写入已知密码哈希:
    #   宿主拷贝来的 shadow 通常是 root:!: (密码锁定) 或 root:*:，
    #   SSH pam 通过 "PasswordAuthentication yes" 需要 root 有合法密码且非锁，
    #   所以在构建阶段直接替换密码字段，避免 init 阶段 chpasswd 因 NSS/chpasswd
    #   不存在或失败导致 SSH 永远 root 拒登。
    # 密码明文: powerfs
    # 选择 $1$ (MD5-based DES 扩展) 的原因：
    #   1. sshd_config 指定 UsePAM=no (参见 init 阶段 cat > /etc/ssh/sshd_config 段)，
    #      因此 OpenSSH 使用内部 xcrypt() = libcrypt.so 的 crypt(3) 接口校验密码，
    #      完全绕过 pam_unix.so 的 yescrypt/sha512 强制算法白名单。
    #   2. libcrypt1 (libcrypt.so.1.1.0) 兼容性层对 $1$ 有完整支持 (无论宿主 pam
    #      配置如何)，宿主环境 + initramfs 环境 100% 通过。
    #   3. 相比之前的 SHA-512 $6$ (2025/2/5 pam_unix 白名单可能缺)，
    #      MD5 $1$ 在 crypt(3) 层面上永不被删，保证开发 SSH 通道稳定可用。
    # 生成命令: openssl passwd -1 -salt powerfs_ powerfs
    ROOT_HASH='$1\$powerfs_\$DplPRhW0J6Z10oMgessGW0'
    # Fix: 上面单引号里 \$ 保留了反斜杠 → 变成 "\$1\$powerfs_"，
    # 传给 shadow 是错的。正确做法是完全不用反斜杠转义 (单引号原样保留 $)，
    # 再用 awk -v HASH= 安全传递 (避免 bash 在 sed 双引号内重解释 $powerfs_ 为空)。
    ROOT_HASH='$1$powerfs_$DplPRhW0J6Z10oMgessGW0'
    export PW_HASH
    PW_HASH="${ROOT_HASH}"
    if sudo grep -q '^root:' etc/shadow; then
        sudo awk -F: -v OFS=: -v HASH="${PW_HASH}" '
            $1 == "root" { $2 = HASH }
            { print }
        ' etc/shadow > etc/shadow.tmp && sudo mv etc/shadow.tmp etc/shadow
    else
        printf 'root:%s:%s:0:99999:7:::\n' "${PW_HASH}" "$(date +%s)" | sudo tee -a etc/shadow >/dev/null
    fi
    if sudo grep -q '^portion:' etc/shadow; then
        sudo awk -F: -v OFS=: -v HASH="${PW_HASH}" '
            $1 == "portion" { $2 = HASH }
            { print }
        ' etc/shadow > etc/shadow.tmp && sudo mv etc/shadow.tmp etc/shadow
    fi
    unset PW_HASH

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
               wc wget which who xxd strings strace ltrace \
               sha256sum sha1sum sha512sum md5sum; do
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
VM_IP=$(echo "$CMDLINE" | grep -o 'vm_ip=[^ ]*' | head -1 | cut -d= -f2)
echo "内核命令行: $CMDLINE"

# 如果未指定，使用默认值
if [ -z "$POWERFS_MASTER_ADDR" ]; then
    POWERFS_MASTER_ADDR="172.30.0.11,172.30.0.12,172.30.0.13"
    echo "[WARN] 未指定 powerfs_master_addr，使用默认: $POWERFS_MASTER_ADDR"
fi
if [ -z "$POWERFS_MASTER_PORT" ]; then
    POWERFS_MASTER_PORT="9334"
fi

# VM_IP: 默认 172.30.0.100 (VM1), VM2 通过内核命令行 vm_ip=172.30.0.101 指定
if [ -z "$VM_IP" ]; then
    VM_IP="172.30.0.100"
fi
echo "[INFO] VM IP: ${VM_IP} (通过 vm_ip= 内核参数配置)"

echo "[INFO] PowerFS 后端:"
echo "  Master:            ${POWERFS_MASTER_ADDR}:${POWERFS_MASTER_PORT}"
echo "  Filer/Volume:      通过 Master 动态发现"

# 使用 tmpfs 挂载 /dev
echo "挂载 tmpfs 到 /dev..."
mkdir -p /dev
mount -t tmpfs tmpfs /dev

# 创建设备节点 (顺序重要: mount tmpfs 后立即做，否则后续命令依赖 /dev/null 会失败)
echo "创建设备节点 (/dev tmpfs 就绪)..."
# 先用 PATH 里的 mknod (GNU coreutils)，再兜底 busybox
MKNOD_CMD=$(command -v mknod 2>/dev/null || true)
if [ -z "${MKNOD_CMD}" ] && command -v busybox >/dev/null 2>&1; then
    MKNOD_CMD="busybox mknod"
fi
if [ -z "${MKNOD_CMD}" ]; then
    echo "[WARN] mknod 命令在 PATH 中不存在，尝试从 /sbin /usr/sbin 搜索"
    for cand in /sbin/mknod /usr/sbin/mknod /bin/mknod /usr/bin/mknod; do
        if [ -x "${cand}" ]; then MKNOD_CMD="${cand}"; break; fi
    done
fi
if [ -z "${MKNOD_CMD}" ]; then
    echo "[ERROR] 找不到 mknod，/dev 下字符设备无法创建 (会导致 /dev/urandom /dev/null 不可用)"
else
    echo "  使用 mknod: ${MKNOD_CMD}"
fi
_mkdev() {
    # _mkdev <name> <c/b> <major> <minor> [mode]
    local name=$1 type=$2 maj=$3 min=$4 mode=${5:-666}
    # 确保父目录存在 (tmpfs 上 /dev 可能没子目录)
    mkdir -p "$(dirname "${name}")" 2>/dev/null || true
    # 如果目标已经存在 (上一次失败残留普通文件/symlink), 先 rm 避免 mknod "File exists"
    if [ -e "${name}" ] && [ ! -c "${name}" ] && [ ! -b "${name}" ]; then
        rm -f "${name}" 2>/dev/null || true
    fi
    local mk_rc=0
    if [ -n "${MKNOD_CMD}" ]; then
        ${MKNOD_CMD} "${name}" "${type}" "${maj}" "${min}" >/tmp/_mkdev_$$.log 2>&1
        mk_rc=$?
    fi
    if [ ! -c "${name}" ] && [ ! -b "${name}" ]; then
        local extra=""
        [ -s /tmp/_mkdev_$$.log ] && extra=": $(cat /tmp/_mkdev_$$.log)"
        echo "[WARN] ${name} 节点未创建 (mknod rc=${mk_rc}${extra})"
    else
        chmod "${mode}" "${name}" 2>/dev/null || true
    fi
    rm -f /tmp/_mkdev_$$.log 2>/dev/null || true
}
_mkdev /dev/console c 5 1 600
_mkdev /dev/null    c 1 3 666
_mkdev /dev/tty     c 5 0 666
_mkdev /dev/zero    c 1 5 666
_mkdev /dev/full    c 1 7 666
_mkdev /dev/random  c 1 8 666
_mkdev /dev/urandom c 1 9 666
_mkdev /dev/kmsg    c 1 11 600
# /dev/ptmx: 先 mknod 原始节点，下面挂载 devpts 后会替换为 /dev/pts/ptmx 的 symlink
_mkdev /dev/ptmx    c 5 2 666
echo "  /dev 节点创建完成: $(ls /dev 2>/dev/null | wc -l) entries"
# 快速健康检查 (如果失败会立即暴露，避免 100 项测试后才发现问题)
if [ -c /dev/null ] && [ -c /dev/urandom ] && [ -c /dev/zero ]; then
    echo "  /dev 健康检查 OK (null/urandom/zero 都是字符设备)"
else
    echo "  [WARN] /dev 健康检查失败: null=$(test -c /dev/null && echo OK || echo BAD)  urandom=$(test -c /dev/urandom && echo OK || echo BAD)  zero=$(test -c /dev/zero && echo OK || echo BAD)"
fi

# 创建 /dev/pts 目录
mkdir -p /dev/pts
chmod 755 /dev/pts

# 挂载 devpts
# 关键参数说明:
#   newinstance    : 每个挂载实例独立 (initramfs 推荐, 避免与 host 混淆)
#   ptmxmode=0666  : /dev/pts/ptmx 权限 666 (默认是 000 导致 OpenSSH PTY 打开失败 -> PTY allocation request failed)
#   mode=0620      : /dev/pts/N 的权限 (owner rw + group r)
echo "挂载 devpts (PTY 支持)..."
if ! mount -t devpts -o newinstance,ptmxmode=0666,mode=0620 devpts /dev/pts 2>/dev/null; then
    mount -t devpts devpts /dev/pts 2>/dev/null
fi
# 创建 /dev/ptmx -> /dev/pts/ptmx 的 symlink (devpts 规范推荐)
# 避免旧版 sshd/telnetd 直接 open("/dev/ptmx") 时找不到可用节点
ln -sf /dev/pts/ptmx /dev/ptmx 2>/dev/null || true
# 兜底: 如果 symlink 失败 (tmpfs 不支持 symlink 的边缘情况) 就 mknod 普通节点
if [ ! -r /dev/ptmx ]; then
    mknod /dev/ptmx c 5 2 2>/dev/null
    chmod 666 /dev/ptmx 2>/dev/null
fi

# 设置控制台
stty raw -echo < /dev/console 2>/dev/null

# 配置网络
# eth0: virtio-net-pci (TAP -> Docker 网桥 powerfs-network)
# VM_IP 通过内核命令行 vm_ip= 参数指定 (默认 172.30.0.100, VM2 用 172.30.0.101)
# Docker powerfs-network 网段为 172.30.0.0/16, 网关 172.30.0.1,
# tap0/tap1 已由 setup_network.sh 桥接到 br-xxx (172.30.0.1).
echo "配置网络..."
if [ -x /bin/ip ]; then
    # eth0: TAP 网络 (用于访问 Docker 容器, 必须与 powerfs-network 同网段)
    ip link set eth0 up 2>/dev/null || true
    ip addr add ${VM_IP}/16 dev eth0 2>/dev/null || true

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

# 创建 InfiniBand 用户态设备节点 (/dev/infiniband/uverbs0 等).
# 内核注册 IB 设备后, /sys/class/infiniband_verbs/uverbsN/dev 给出 "major:minor",
# 但 tmpfs /dev 不会自动 mknod (无 udev), 必须手动创建, 否则 ibv_devinfo 看不到设备,
# powerfs RDMA transport 无法打开 verbs context.
echo "创建 InfiniBand 设备节点..."
mkdir -p /dev/infiniband
for uv in /sys/class/infiniband_verbs/uverbs*; do
    [ -e "${uv}/dev" ] || continue
    name=$(basename "${uv}")
    major_minor=$(cat "${uv}/dev" 2>/dev/null)
    if [ -n "${major_minor}" ]; then
        maj=$(echo "${major_minor}" | cut -d: -f1)
        min=$(echo "${major_minor}" | cut -d: -f2)
        if [ -n "${MKNOD_CMD}" ]; then
            ${MKNOD_CMD} "/dev/infiniband/${name}" c "${maj}" "${min}" 2>/dev/null
            chmod 666 "/dev/infiniband/${name}" 2>/dev/null || true
            echo "  + /dev/infiniband/${name} (c ${maj}:${min})"
        fi
    fi
done
# rdma_cm 字符设备 (RDMA CM 用户态接口, rdma_cm 模块加载后出现)
if [ -e /sys/class/infiniband_cm/rdma_cm/dev ]; then
    cm_mm=$(cat /sys/class/infiniband_cm/rdma_cm/dev 2>/dev/null)
    if [ -n "${cm_mm}" ]; then
        cm_maj=$(echo "${cm_mm}" | cut -d: -f1)
        cm_min=$(echo "${cm_mm}" | cut -d: -f2)
        if [ -n "${MKNOD_CMD}" ] && [ ! -c /dev/infiniband/rdma_cm ]; then
            ${MKNOD_CMD} /dev/infiniband/rdma_cm c "${cm_maj}" "${cm_min}" 2>/dev/null
            chmod 666 /dev/infiniband/rdma_cm 2>/dev/null || true
            echo "  + /dev/infiniband/rdma_cm (c ${cm_maj}:${cm_min})"
        fi
    fi
fi
# umad (InfiniBand MAD 用户态访问, ibv_devices 枚举需要)
for um in /sys/class/infiniband_umad/umad*; do
    [ -e "${um}/dev" ] || continue
    uname=$(basename "${um}")
    um_mm=$(cat "${um}/dev" 2>/dev/null)
    if [ -n "${um_mm}" ] && [ -n "${MKNOD_CMD}" ]; then
        um_maj=$(echo "${um_mm}" | cut -d: -f1)
        um_min=$(echo "${um_mm}" | cut -d: -f2)
        ${MKNOD_CMD} "/dev/infiniband/${uname}" c "${um_maj}" "${um_min}" 2>/dev/null
        chmod 666 "/dev/infiniband/${uname}" 2>/dev/null || true
        echo "  + /dev/infiniband/${uname} (c ${um_maj}:${um_min})"
    fi
done
# issm (InfiniBand Subnet Manager 接口)
for is in /sys/class/infiniband_umad/issm*; do
    [ -e "${is}/dev" ] || continue
    iname=$(basename "${is}")
    is_mm=$(cat "${is}/dev" 2>/dev/null)
    if [ -n "${is_mm}" ] && [ -n "${MKNOD_CMD}" ]; then
        is_maj=$(echo "${is_mm}" | cut -d: -f1)
        is_min=$(echo "${is_mm}" | cut -d: -f2)
        ${MKNOD_CMD} "/dev/infiniband/${iname}" c "${is_maj}" "${is_min}" 2>/dev/null
        chmod 666 "/dev/infiniband/${iname}" 2>/dev/null || true
    fi
done
# 验证 IB 设备可见性
if command -v ibv_devices >/dev/null 2>&1; then
    echo "  ibv_devices:"
    ibv_devices 2>/dev/null | head -5
fi

# 挂载 Host 共享目录 (9p virtfs, 用于快速部署 powerfs.ko 和测试脚本).
# mount_tag fallback 顺序:
#   1) hostshare        — qemuctl2.sh 新版 (vm_share_tag 统一为 "hostshare")
#   2) hostshare_vm1    — qemuctl2.sh 老版 vm1
#   3) hostshare_vm2    — qemuctl2.sh 老版 vm2
# 4) hostshare_vm{1,2}  防止将来 qemu 脚本与 initramfs 不同步导致 9p 挂载静默
#    失败 → init 退用 initramfs 内置的旧 powerfs.ko (不含内核 RDMA PFSN 握手代码),
#    进而触发 ROOT36-D: filer 端 step=0 永不到 step-1, RPC 全 deadline exceeded.
echo "挂载 Host 共享目录 (9p virtfs)..."
mkdir -p /mnt/host
SHARE_MOUNTED=0
for share_tag in hostshare hostshare_vm1 hostshare_vm2; do
    mount -t 9p -o trans=virtio,version=9p2000.L "${share_tag}" /mnt/host 2>/dev/null
    if [ $? -eq 0 ] && [ -d /mnt/host ] && ls /mnt/host >/dev/null 2>&1; then
        SHARE_MOUNTED=1
        echo "[OK] Host 共享目录已挂载 (tag=${share_tag}): /mnt/host"
        break
    fi
done
if [ "${SHARE_MOUNTED}" -eq 1 ]; then
    # fio 动态库在 9p 共享目录, 设置 LD_LIBRARY_PATH 使 fio 可用
    if [ -d /mnt/host/fio-libs ]; then
        export LD_LIBRARY_PATH=/mnt/host/fio-libs
        echo "[OK] LD_LIBRARY_PATH=/mnt/host/fio-libs (fio 动态库)"
    fi
else
    echo "[WARN] 9p 挂载失败: tried tags hostshare / hostshare_vm1 / hostshare_vm2. " \
         "将只能使用 initramfs 内置的 powerfs.ko (可能过期, 导致 RDMA 握手缺失)."
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
    echo "  Shard count:       ${POWERFS_SHARD_COUNT:-3}"
    echo "  模块路径:          ${POWERFS_KO}"
    echo "  参数传递:          mount -o master_addr=... (NOT insmod param, per-mount)"

    # 加载模块 (所有 master/shard 参数都在 mount -o 时传递,
    # 不再走 module_param 全局共享, 支持多 mount point 独立配置)
    insmod "${POWERFS_KO}"

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

# 设置 root 密码 (用于密码登录)。
# 先解锁 root: 从 /etc/shadow 拷贝 /etc/shadow- 备份移除 "!" lock 前缀，
# 再调用 chpasswd，避免宿主 shadow 中 root:!: 导致 pam 拒绝登录。
if [ -f /etc/shadow ]; then
    # 移除 root 条目的密码锁 ('!' 或 '*')，解锁后才能通过 SSH 密码登录
    sed -i -e 's/^root:!:/root::/' -e 's/^root:\*:/root::/' /etc/shadow 2>/dev/null || true
fi
if [ -x /usr/sbin/chpasswd ] || [ -x /sbin/chpasswd ] || command -v chpasswd >/dev/null 2>&1; then
    echo "root:powerfs" | chpasswd 2>/dev/null
fi
# chpasswd 失败兜底：直接写 DES/MD5 哈希到 shadow (兼容 busybox)
RC=$?
if [ ${RC:-1} -ne 0 ] && [ -f /etc/shadow ]; then
    # root:$1$powerfs$abcdefghijklmnopqrstuv (MD5, salt=powerfs)
    sed -i 's|^root:[^:]*:|root:$1$powerfs$abcdefghijklmnopqrstuv:|' /etc/shadow 2>/dev/null || true
fi

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
# init 阶段尝试自动挂载 powerfs (路径对齐 FUSE 容器 /mnt/powerfs),
# 失败则由用户手工调用 mount_powerfs。
AUTO_MOUNT_OPTS="master_addr=${POWERFS_MASTER_ADDR},master_port=${POWERFS_MASTER_PORT},shard_count=${POWERFS_SHARD_COUNT:-3}"
if [ -f /etc/powerfs/ca.crt ]; then
    AUTO_MOUNT_OPTS="${AUTO_MOUNT_OPTS},ca_crt=/etc/powerfs/ca.crt"
fi
if [ -f /etc/powerfs/kernel-client-1.crt ]; then
    AUTO_MOUNT_OPTS="${AUTO_MOUNT_OPTS},client_crt=/etc/powerfs/kernel-client-1.crt"
fi
if [ -f /etc/powerfs/kernel-client-1.key ]; then
    AUTO_MOUNT_OPTS="${AUTO_MOUNT_OPTS},client_key=/etc/powerfs/kernel-client-1.key"
fi
mkdir -p /mnt/powerfs
if mount -t powerfs -o "${AUTO_MOUNT_OPTS}" none /mnt/powerfs 2>/dev/null; then
    echo "[OK] PowerFS 自动挂载成功: /mnt/powerfs"
else
    echo "[INFO] PowerFS 自动挂载失败，稍后请手动执行 mount_powerfs none /mnt/powerfs"
fi

# 创建 /mnt/pfs -> /mnt/powerfs 符号链接，兼容使用 MNT=/mnt/pfs 的测试脚本
ln -sf /mnt/powerfs /mnt/pfs 2>/dev/null

echo ""
echo "常用命令:"
echo "  lsmod                           - 查看已加载模块"
echo "  dmesg | tail -50                - 查看内核日志"
echo "  cat /proc/filesystems | grep powerfs  - 检查 powerfs 文件系统"
echo "  mount_powerfs none /mnt/powerfs - 挂载 PowerFS (与 FUSE 容器同路径)"
echo "  echo 'hello' > /mnt/powerfs/test.txt - 写入测试"
echo "  cat /mnt/powerfs/test.txt       - 读取测试"
echo ""
echo "完整 mount 示例 (per-mount opts):"
echo "  mount -t powerfs -o \"master_addr=172.30.0.11,172.30.0.12,172.30.0.13,master_port=9334,shard_count=3,ca_crt=/etc/powerfs/ca.crt,client_crt=/etc/powerfs/kernel-client-1.crt,client_key=/etc/powerfs/kernel-client-1.key\" none /mnt/powerfs"
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
    cp "${FIO_BIN}" bin/fio.real
    chmod +x bin/fio.real
    # 创建 wrapper 脚本设置 LD_LIBRARY_PATH (fio 是动态链接, 依赖 9p 共享的 fio-libs)
    cat > bin/fio << 'FIOEOF'
#!/bin/sh
if [ -d /mnt/host/fio-libs ]; then
    export LD_LIBRARY_PATH=/mnt/host/fio-libs
fi
exec /bin/fio.real "$@"
FIOEOF
    chmod +x bin/fio
    # 同步到 output 目录缓存
    cp "${FIO_BIN}" "${OUTPUT_DIR}/fio" 2>/dev/null || true
    echo "  已添加 fio 到 initramfs ($(ls -la bin/fio.real | awk '{print $5}') bytes)"
else
    echo "  [WARN] 未找到 fio 二进制, T5 性能测试将需要手动安装"
    echo "  预期路径: /tmp/fio-build/fio (静态构建)"
fi

# 打包 bash + 核心动态库 + GNU coreutils 工具链。
# 目标: test_t1_vfs_basic.sh / test_t2_correctness.sh 等 #!/bin/bash 语法脚本
# 可以在 VM 中原生运行，一套脚本在 VM 端 (kernel mount) 和 FUSE 容器
# 端 (fuse mount, mount_point=/mnt/powerfs) 完全复用，无需再手工转
# busybox sh 兼容语法。
echo "=== 打包 bash + GNU coreutils + 共享库 ==="
# 用绝对路径避免 sudo/非 sudo 环境的 PATH 差异。
# bash 在 Debian/Ubuntu 同时存在于 /bin 和 /usr/bin。
BASH_BIN=""
for candidate in /usr/bin/bash /bin/bash; do
    if [ -x "${candidate}" ]; then BASH_BIN="${candidate}"; break; fi
done
if [ -z "${BASH_BIN}" ]; then
    # 最后兜底: which/command -v (用户 PATH 可见)
    BASH_BIN=$(command -v bash 2>/dev/null) || true
fi
if [ -n "${BASH_BIN}" ] && [ -f "${BASH_BIN}" ]; then
    cp "${BASH_BIN}" bin/bash
    chmod +x bin/bash
    # 同时让 /bin/sh 用 bash 解释执行，保持向后兼容
    ln -sf bash bin/sh 2>/dev/null || true
    echo "  已添加 bash ($(ls -lh bin/bash | awk '{print $5}'))"

    # 1) 拷贝 bash/动态二进制的共享库 (含 ld-linux 解释器)
    #    辅助函数: ldd 展开 + 去重 + 软链接保留
    _copy_so() {
        local src="$1"
        local name
        if [ -z "${src}" -o ! -f "${src}" ]; then return 0; fi
        name=$(basename "${src}")
        # 已存在则跳过 (之前 sshd 打包阶段已经加入)
        if [ -f "lib/${name}" ]; then return 0; fi
        # 保留符号链接指向 (libxxx.so -> libxxx.so.N)，dlopen 按 soname 搜索
        if [ -L "${src}" ]; then
            local tgt
            tgt=$(readlink "${src}")
            if [ "${tgt#/}" != "${tgt}" ]; then
                # 绝对路径链接目标
                if [ -f "${tgt}" ]; then
                    cp "${tgt}" "lib/$(basename ${tgt})"
                    ln -sf "$(basename ${tgt})" "lib/${name}"
                fi
            else
                # 相对路径链接: 先拷贝目标到 lib/, 再创建同名字链接
                local srcdir=$(dirname "${src}")
                if [ -f "${srcdir}/${tgt}" ]; then
                    cp "${srcdir}/${tgt}" "lib/${tgt}" 2>/dev/null
                    ln -sf "${tgt}" "lib/${name}"
                fi
            fi
        else
            cp "${src}" "lib/${name}"
        fi
    }
    for so in $(ldd "${BASH_BIN}" 2>/dev/null | awk '/=> \// {print $3} /^\s*\/lib.*ld-linux/ {print $1}'); do
        _copy_so "${so}"
    done
    echo "  已添加 bash 依赖的共享库"

    # 2) 拷贝 GNU coreutils/常用二进制 (替换 busybox 受限版本)
    #    选取测试脚本实际使用的命令集合。/bin 和 /usr/bin 都搜一遍，
    #    避免 Debian/Ubuntu 下部分工具 symlink 位置差异。
    mkdir -p usr/bin
    UTIL_LIST=(
        /usr/bin/md5sum  /bin/md5sum
        /usr/bin/stat    /bin/stat
        /usr/bin/dd      /bin/dd
        /usr/bin/truncate
        /usr/bin/chmod   /bin/chmod
        /usr/bin/chown   /bin/chown
        /usr/bin/awk     /bin/awk
        /usr/bin/sort    /bin/sort
        /usr/bin/find    /bin/find
        /usr/bin/xargs   /bin/xargs
        /usr/bin/tar     /bin/tar
        /usr/bin/gzip    /bin/gzip
        /usr/bin/gunzip  /bin/gunzip
        /usr/bin/zcat    /bin/zcat
        /usr/bin/seq     /bin/seq
        /usr/bin/printf  /bin/printf
        /usr/bin/date    /bin/date
        /usr/bin/uptime  /bin/uptime
        /usr/bin/base64  /bin/base64
        /usr/bin/tee     /bin/tee
        /usr/bin/timeout
        /usr/bin/diff    /bin/diff
        /usr/bin/comm    /bin/comm
        /usr/bin/cmp     /bin/cmp
        /usr/bin/touch   /bin/touch
        /usr/bin/head    /bin/head
        /usr/bin/tail    /bin/tail
        /usr/bin/wc      /bin/wc
        /usr/bin/cut     /bin/cut
        /usr/bin/sed     /bin/sed
        /usr/bin/hexdump
        /usr/bin/xxd
        /usr/bin/rsync
        /usr/bin/readlink /bin/readlink
        /usr/bin/basename /bin/basename
        /usr/bin/dirname  /bin/dirname
        /usr/bin/realpath /bin/realpath
        /usr/bin/env      /bin/env
        /usr/bin/which    /bin/which
        /bin/dmesg        /usr/bin/dmesg
        /bin/mount        /usr/bin/mount
        /bin/umount       /usr/bin/umount
        /bin/lsblk        /usr/bin/lsblk
        /bin/lscpu        /usr/bin/lscpu
        /bin/free         /usr/bin/free
        /bin/ps           /usr/bin/ps
        /bin/ls           /usr/bin/ls
        /bin/cat          /usr/bin/cat
        /bin/rm           /usr/bin/rm
        /bin/cp           /usr/bin/cp
        /bin/mv           /usr/bin/mv
        /bin/ln           /usr/bin/ln
        /bin/mkdir        /usr/bin/mkdir
        /bin/rmdir        /usr/bin/rmdir
        /bin/chmod        /usr/bin/chmod
        /bin/chown        /usr/bin/chown
        /bin/sync         /usr/bin/sync
        /bin/sleep        /usr/bin/sleep
        /bin/echo         /usr/bin/echo
        /bin/test         /usr/bin/test
        /bin/[            /usr/bin/[
        /bin/grep         /usr/bin/grep
        /bin/mknod        /usr/bin/mknod
        /bin/mkfifo       /usr/bin/mkfifo
        /bin/kill         /usr/bin/kill
        /bin/date         /usr/bin/date
        /bin/hostname     /usr/bin/hostname
        /bin/id           /usr/bin/id
        /bin/pwd          /usr/bin/pwd
        /bin/uname        /usr/bin/uname
    )
    declare -A _seen=()
    for util in "${UTIL_LIST[@]}"; do
        name=$(basename "${util}")
        if [ -n "${_seen[$name]:-}" ]; then continue; fi   # 同名只取第一个 (优先 /usr/bin)
        if [ -x "${util}" ]; then
            _seen[$name]=1
            cp "${util}" "usr/bin/${name}" 2>/dev/null || continue
            chmod +x "usr/bin/${name}" 2>/dev/null
            # 展开这个工具的 .so 依赖
            for so in $(ldd "${util}" 2>/dev/null | awk '/=> \// {print $3} /^\s*\/lib.*ld-linux/ {print $1}'); do
                _copy_so "${so}"
            done
            # 同名符号链接到 /bin/ 以便 PATH=/bin 也能命中
            if [ ! -f "bin/${name}" ]; then
                ln -sf "/usr/bin/${name}" "bin/${name}" 2>/dev/null || true
            fi
        fi
    done
    echo "  已添加 coreutils/常用二进制: ${!_seen[*]}" | fold -w 100 -s | sed 's/^/    /'
    echo "  数量: ${#_seen[@]}"

    # 3) 确保 ld-linux 解释器存在 (所有动态程序都需要)
    if [ ! -f lib64/ld-linux-x86-64.so.2 ] && [ -f /lib64/ld-linux-x86-64.so.2 ]; then
        cp /lib64/ld-linux-x86-64.so.2 lib64/ld-linux-x86-64.so.2
    fi
    # 额外补齐 bash 可能缺失的 libreadline/libtinfo (历史 Ubuntu 版本差异)
    for extra in libreadline.so.8 libtinfo.so.6 libdl.so.2; do
        found=$(find /lib /usr/lib -name "${extra}" -not -name '*.a' 2>/dev/null | head -1)
        if [ -n "${found}" ]; then
            _copy_so "${found}"
        fi
    done
    echo "  共享库完整性校验:"
    ls -1 lib/ 2>/dev/null | wc -l | awk '{print "    已收录 so 数量: "$1}'
else
    echo "  [WARN] 宿主未安装 bash, 测试脚本在 VM 中只能用 busybox sh 兼容模式"
fi

# =====================================================================
# 打包 RDMA 诊断工具 (vfio-pci 直通 VF 时在 VM 内验证设备可见性)
#   - ibv_devices / ibv_devinfo / ibv_rc_pingpong / ibstatus (rdma-core)
#   - librdmacm / libibverbs / libnl-3 / libnl-route-3 依赖
#   - libmlx5 provider:  libmlx5-rdmav34.so (VM 内 mlx5_ib 驱动配对, 通过
#     /usr/lib/x86_64-linux-gnu/libibverbs/provider/ 提供)
#   - lspci: 查看 vfio 直通到 VM 的 PCI BDF 是否被 VM 内核枚举
# =====================================================================
echo "=== 打包 RDMA 诊断工具 (ibv_* / lspci) ==="
RDMA_BINS=(
    /usr/bin/ibv_devices
    /usr/bin/ibv_devinfo
    /usr/bin/ibv_rc_pingpong
    /usr/sbin/ibstatus
    /usr/bin/lspci
    /usr/bin/rdma
    /usr/sbin/setpci
)
for util in "${RDMA_BINS[@]}"; do
    name=$(basename "${util}")
    if [ ! -x "${util}" ]; then
        echo "  [SKIP] ${util} 不存在"
        continue
    fi
    cp "${util}" "usr/bin/${name}" 2>/dev/null || continue
    chmod +x "usr/bin/${name}" 2>/dev/null
    # 同名软链到 /bin
    if [ ! -f "bin/${name}" ]; then
        ln -sf "/usr/bin/${name}" "bin/${name}" 2>/dev/null || true
    fi
    # 展开 .so 依赖
    for so in $(ldd "${util}" 2>/dev/null | awk '/=> \// {print $3} /^\s*\/lib.*ld-linux/ {print $1}'); do
        if [ -n "${BASH_BIN}" ] && declare -F _copy_so >/dev/null 2>&1; then
            _copy_so "${so}"
        else
            sname=$(basename "${so}")
            [ -n "${sname}" ] && [ -f "${so}" ] && cp -L "${so}" "lib/${sname}" 2>/dev/null || true
        fi
    done
    echo "  + ${name}"
done
# 额外补齐 rdma-core provider 共享库 (用户态驱动, 需要被 libibverbs dlopen)
mkdir -p lib/libibverbs  lib/librdmacm
_provider_list=(
    "/usr/lib/x86_64-linux-gnu/libibverbs/libmlx5-rdmav34.so"
    "/usr/lib/x86_64-linux-gnu/libmlx5.so.1"
    "/usr/lib/x86_64-linux-gnu/libmlx4.so.1"
    "/usr/lib/x86_64-linux-gnu/libefa.so.1"
    "/usr/lib/x86_64-linux-gnu/librdmacm.so.1"
    "/usr/lib/x86_64-linux-gnu/libibverbs.so.1"
    "/usr/lib/x86_64-linux-gnu/libnl-3.so.200"
    "/usr/lib/x86_64-linux-gnu/libnl-route-3.so.200"
)
for f in "${_provider_list[@]}"; do
    if [ ! -f "${f}" ]; then continue; fi
    # 保留 symlink: 将 real file + symlink 都放到 lib/ (dlopen 用 soname 找, 目录结构
    # 必须和 host 一致: libmlx5-rdmav34.so 放到 lib/libibverbs/)
    dest="lib/$(echo "${f}" | sed 's|.*/x86_64-linux-gnu/||')"
    dest_dir="$(dirname "${dest}")"
    mkdir -p "${dest_dir}"
    if [ -L "${f}" ]; then
        tgt="$(readlink "${f}")"
        if [ "${tgt#/}" = "${tgt}" ] && [ -f "$(dirname "${f}")/${tgt}" ]; then
            # 相对链接: 复制目标再 ln
            cp "$(dirname "${f}")/${tgt}" "${dest_dir}/${tgt}" 2>/dev/null || true
            (cd "${dest_dir}" && ln -sf "${tgt}" "$(basename "${dest}")" 2>/dev/null || true)
        elif [ -f "${tgt}" ]; then
            cp "${tgt}" "lib/$(basename "${tgt}")" 2>/dev/null || true
        fi
    else
        cp "${f}" "${dest}" 2>/dev/null || true
    fi
    # 同步复制到 lib/ 根 (兼容一些实现不进子目录)
    cp -L "${f}" "lib/$(basename "${f}")" 2>/dev/null || true
done
# libnl-genl-3: libnl-route-3 间接依赖 (有时未被 ldd 展开, 以防万一)
for extra in libnl-genl-3.so.200 libpci.so.3 libkmod.so.2 libz.so.1 libzstd.so.1 liblzma.so.5 libudev.so.1 libcrypto.so.1.1 libpthread.so.0 libdl.so.2 libresolv.so.2; do
    found="$(find /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu -name "${extra}" -not -name '*.a' 2>/dev/null | head -1)"
    if [ -n "${found}" ]; then
        if declare -F _copy_so >/dev/null 2>&1; then
            _copy_so "${found}"
        else
            cp -L "${found}" "lib/$(basename "${found}")" 2>/dev/null || true
        fi
    fi
done
# 补齐 /etc/libibverbs.d/*.driver (rdma-core 运行时驱动描述文件, 否则 libmlx5 不被
# libibverbs 枚举, ibv_devices 看不到 mlx5 设备).
mkdir -p etc/libibverbs.d
if [ -d /etc/libibverbs.d ]; then
    for f in /etc/libibverbs.d/*; do
        [ -f "${f}" ] || continue
        cp "${f}" "etc/libibverbs.d/$(basename "${f}")"
        echo "  + etc/libibverbs.d/$(basename "${f}")"
    done
fi
echo "  RDMA 工具打包完成"

# 创建简单的挂载工具。
# 挂载点与 FUSE 容器一致: /mnt/powerfs, 保证 test_t1_vfs_basic.sh /
# test_t2_correctness.sh 两边同路径、一套脚本复用。
cat > bin/mount_powerfs << 'MEOF'
#!/bin/sh
# PowerFS 快捷挂载脚本
# 参数全通过 mount -o 传递 (master_addr/master_port/shard_count 不再是
# 全局 module_param), 避免多个 mount point 之间互相污染.

POWERFS_MOUNT_OPTS="${POWERFS_MOUNT_OPTS:-master_addr=${POWERFS_MASTER_ADDR:-172.30.0.11,172.30.0.12,172.30.0.13},master_port=${POWERFS_MASTER_PORT:-9334},shard_count=${POWERFS_SHARD_COUNT:-3}}"

# 证书 opts: 如果环境变量指定了证书路径, 追加到 mount opts。
# master 有 CA manager 时必填, 否则 RegisterClient 会被 PERMISSION_DENIED 拒绝。
# 默认使用 initramfs 预打包到 /etc/powerfs/ 的证书 (持久, VM 重启后不会清空)。
if [ -n "${POWERFS_CA_CRT:-}" ]; then
    POWERFS_MOUNT_OPTS="${POWERFS_MOUNT_OPTS},ca_crt=${POWERFS_CA_CRT}"
elif [ -f /etc/powerfs/ca.crt ]; then
    POWERFS_MOUNT_OPTS="${POWERFS_MOUNT_OPTS},ca_crt=/etc/powerfs/ca.crt"
fi
if [ -n "${POWERFS_CLIENT_CRT:-}" ]; then
    POWERFS_MOUNT_OPTS="${POWERFS_MOUNT_OPTS},client_crt=${POWERFS_CLIENT_CRT}"
elif [ -f /etc/powerfs/kernel-client-1.crt ]; then
    POWERFS_MOUNT_OPTS="${POWERFS_MOUNT_OPTS},client_crt=/etc/powerfs/kernel-client-1.crt"
fi
if [ -n "${POWERFS_CLIENT_KEY:-}" ]; then
    POWERFS_MOUNT_OPTS="${POWERFS_MOUNT_OPTS},client_key=${POWERFS_CLIENT_KEY}"
elif [ -f /etc/powerfs/kernel-client-1.key ]; then
    POWERFS_MOUNT_OPTS="${POWERFS_MOUNT_OPTS},client_key=/etc/powerfs/kernel-client-1.key"
fi

if [ $# -lt 1 ]; then
    echo "用法: mount_powerfs <device> [mountpoint]"
    echo "示例: mount_powerfs none /mnt/powerfs"
    echo "  mount_opts (env POWERFS_MOUNT_OPTS): $POWERFS_MOUNT_OPTS"
    exit 1
fi

DEVICE=$1
# 对齐 FUSE 容器 fuse.toml: mount_point = "/mnt/powerfs"
MOUNTPOINT=${2:-/mnt/powerfs}

echo "挂载 PowerFS: ${DEVICE} -> ${MOUNTPOINT} (opts=${POWERFS_MOUNT_OPTS})"
mkdir -p "${MOUNTPOINT}"
mount -t powerfs -o "${POWERFS_MOUNT_OPTS}" "${DEVICE}" "${MOUNTPOINT}"
RC=$?
if [ $RC -eq 0 ]; then
    echo "挂载成功!"
else
    echo "挂载失败! (rc=$RC) 检查 dmesg | tail -30"
fi
exit $RC
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

# 打包 CA 证书和客户端证书到 initramfs (/etc/powerfs 持久目录, 不会重启清空)
# Master enforce 模式下 RegisterClient/KeepConnected 需要 0xD4 ClientCert,
# 证书缺失会导致 mount 后所有请求被 blacklist.
echo "=== 打包 PowerFS 客户端证书 ==="
CERT_SRC_DIR="${SCRIPT_DIR}/share"
CERT_DST_DIR="${INITRAMFS_DIR}/etc/powerfs"
mkdir -p "${CERT_DST_DIR}"
for fname in ca.crt kernel-client-1.crt kernel-client-1.key; do
    if [ -f "${CERT_SRC_DIR}/${fname}" ]; then
        cp "${CERT_SRC_DIR}/${fname}" "${CERT_DST_DIR}/${fname}"
        echo "  已复制 ${fname} -> /etc/powerfs/${fname}"
    else
        echo "  [WARN] 未找到 ${CERT_SRC_DIR}/${fname}, mount 时需要手动提供证书路径"
    fi
done
# 证书权限: key 文件必须 0600, cert/ca 0644
chmod 600 "${CERT_DST_DIR}/kernel-client-1.key" 2>/dev/null || true
chmod 644 "${CERT_DST_DIR}/ca.crt" 2>/dev/null || true
chmod 644 "${CERT_DST_DIR}/kernel-client-1.crt" 2>/dev/null || true

# === 关键: 打包前修复文件所有权和权限 ===
echo "=== 修复文件所有权 ==="

# 创建必要的目录结构
sudo mkdir -p "${INITRAMFS_DIR}/home/portion/.ssh" 2>/dev/null || true
sudo mkdir -p "${INITRAMFS_DIR}/root/.ssh" 2>/dev/null || true

# 开发者便利: 把宿主当前用户所有的 SSH 公钥 (id_rsa.pub / id_ed25519.pub / id_ecdsa.pub 等)
# 拷贝到 VM root + portion 用户的 authorized_keys，这样开发机直接 `ssh root@127.0.0.1 -p 2223`
# 就能零密码登录（PubkeyAuthentication），完全绕过 shadow hash 算法 / PAM 兼容问题
# (之前反复出现的 UsePAM=no + SHA-512 vs MD5 白名单问题彻底消除)。
# authorized_keys 要求 owner=root mode=600，下方 chown/chmod 会统一修正。
HOST_SSH_DIR="${HOME}/.ssh"
AUTH_KEYS_ROOT="${INITRAMFS_DIR}/root/.ssh/authorized_keys"
AUTH_KEYS_PORTION="${INITRAMFS_DIR}/home/portion/.ssh/authorized_keys"
: > /tmp/_powerfs_host_pubkeys.tmp
if [ -d "${HOST_SSH_DIR}" ]; then
    for pub in "${HOST_SSH_DIR}"/id_*.pub "${HOST_SSH_DIR}"/authorized_keys; do
        [ -f "${pub}" ] && cat "${pub}" >> /tmp/_powerfs_host_pubkeys.tmp 2>/dev/null || true
    done
fi
if [ -s /tmp/_powerfs_host_pubkeys.tmp ]; then
    sudo cp /tmp/_powerfs_host_pubkeys.tmp "${AUTH_KEYS_ROOT}"
    sudo cp /tmp/_powerfs_host_pubkeys.tmp "${AUTH_KEYS_PORTION}"
    echo "=== 注入 SSH 公钥到 VM ==="
    echo "  root   authorized_keys: $(wc -l < "${AUTH_KEYS_ROOT}") 条"
    echo "  portion authorized_keys: $(wc -l < "${AUTH_KEYS_PORTION}") 条"
fi
rm -f /tmp/_powerfs_host_pubkeys.tmp

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
sudo chmod 600 "${INITRAMFS_DIR}/root/.ssh/authorized_keys" 2>/dev/null || true

# portion 用户 SSH 目录
sudo chmod 700 "${INITRAMFS_DIR}/home/portion/.ssh" 2>/dev/null || true
sudo chown -R 1001:1001 "${INITRAMFS_DIR}/home/portion/.ssh" 2>/dev/null || true
sudo chmod 600 "${INITRAMFS_DIR}/home/portion/.ssh/authorized_keys" 2>/dev/null || true

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
