#!/usr/bin/env bash
#
# dkms.sh — register, build and install powerfs.ko through DKMS.
#
# After `install`, the module auto-rebuilds for every newly installed
# kernel (AUTOINSTALL=yes + distro dkms hook); load it with `modprobe powerfs`.
#
# Target-kernel prerequisites:
#   - Linux 6.17+ (BUILD_EXCLUSIVE_KERNEL in dkms.conf enforces this)
#   - prepared kernel build tree at /lib/modules/$(uname -r)/build
#   - simple_xattr_* symbols exported (custom kernels need
#     kernel/patches/export-simple-xattr-symbols-linux-6.17.patch applied)
#
# Usage:
#   sudo ./dkms.sh build [extra dkms args]
#       Stage sources, `dkms add` + `dkms build` only — does NOT install into
#       /lib/modules. Useful for compile validation without touching the
#       running kernel. Against a custom kernel tree, point dkms at it the
#       standard way (dkms skips its mrproper/prepare step for /lib/modules
#       paths; --kernelsourcedir on a raw source tree triggers mrproper and
#       fails on dkms 2.8 because 6.17 removed the prepare-all target):
#         sudo ln -s /path/to/linux-6.17 /lib/modules/6.17.0/build
#         sudo ./dkms.sh build -k 6.17.0
#
#   sudo ./dkms.sh install [extra dkms args]
#       add + build + install into /lib/modules + depmod (normal client path)
#
#   ./dkms.sh status
#       Show DKMS state for powerfs
#
#   sudo ./dkms.sh uninstall
#       Remove powerfs from DKMS for all kernels and delete staged sources
#
# Requires: dkms, rsync. On Secure Boot hosts the module must be MOK-signed
# before modprobe (DKMS does not sign automatically).
set -euo pipefail

MODULE="powerfs"
VERSION="2.0.0"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE_DIR="/usr/src/${MODULE}-${VERSION}"

die() {
    echo "error: $*" >&2
    exit 1
}

need_root() {
    [ "$(id -u)" -eq 0 ] || die "this action requires root — re-run with sudo"
}

# Keep /usr/src staging in sync with this source directory, minus build
# artifacts. Idempotent; --delete removes stale files from previous versions.
stage_sources() {
    command -v rsync >/dev/null || die "rsync not found (install the rsync package)"
    echo "==> staging $MODULE-$VERSION sources to $STAGE_DIR"
    mkdir -p "$STAGE_DIR"
    rsync -a --delete \
        --exclude='*.o' \
        --exclude='*.ko' \
        --exclude='*.mod' \
        --exclude='*.mod.c' \
        --exclude='*.mod.o' \
        --exclude='.*.cmd' \
        --exclude='.tmp_versions/' \
        --exclude='Module.symvers' \
        --exclude='modules.order' \
        --exclude='test_tlk_codec' \
        "$SRC_DIR"/ "$STAGE_DIR"/
}

# Remove any previous DKMS registration (built/installed for any kernel),
# then register the freshly staged source tree.
dkms_register() {
    if dkms status -m "$MODULE" -v "$VERSION" 2>/dev/null | grep -q .; then
        echo "==> removing previous DKMS registration"
        dkms remove -m "$MODULE" -v "$VERSION" --all >/dev/null 2>&1 || true
    fi
    echo "==> dkms add $MODULE/$VERSION"
    dkms add -m "$MODULE" -v "$VERSION"
}

do_build() {
    need_root
    stage_sources
    dkms_register
    echo "==> dkms build $MODULE/$VERSION $*"
    dkms build -m "$MODULE" -v "$VERSION" "$@"
    echo "==> build complete (not installed into /lib/modules)"
}

do_install() {
    need_root
    stage_sources
    dkms_register
    echo "==> dkms build $MODULE/$VERSION $*"
    dkms build -m "$MODULE" -v "$VERSION" "$@"
    echo "==> dkms install $MODULE/$VERSION"
    dkms install -m "$MODULE" -v "$VERSION" "$@"
    echo "==> installed; load with: modprobe $MODULE"
}

do_status() {
    dkms status "$MODULE"
}

do_uninstall() {
    need_root
    echo "==> dkms remove $MODULE/$VERSION (all kernels)"
    dkms remove -m "$MODULE" -v "$VERSION" --all || \
        echo "note: $MODULE/$VERSION was not registered with DKMS"
    rm -rf "$STAGE_DIR"
    echo "==> uninstall complete"
}

case "${1:-}" in
    build)
        shift
        do_build "$@"
        ;;
    install)
        shift
        do_install "$@"
        ;;
    status)
        do_status
        ;;
    uninstall)
        do_uninstall
        ;;
    ""|-h|--help|help)
        sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)
        die "unknown action '$1' (use build | install | status | uninstall)"
        ;;
esac
