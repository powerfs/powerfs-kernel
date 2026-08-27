#!/usr/bin/env python3
"""Mechanically split powerfs_net.c (10222 lines) into 11 .c files.

Pure physical migration: line-range extraction + prepend includes +
#include "powerfs_net_internal.h" + remove leading `static ` (except
`static inline` local helpers). No function-body logic changes.
"""
import os

SRC = "/home/portion/powerfs/kernel/powerfs_mod/powerfs_net.c"
DST = "/home/portion/powerfs/kernel/powerfs_mod"

with open(SRC, "r") as f:
    lines = f.readlines()  # 0-indexed; lines[i] == source line i+1

# Shared include block = original L19-59 (0-indexed [18:59])
INCLUDES = "".join(lines[18:59])


def make_preamble(name):
    return (
        "/* SPDX-License-Identifier: GPL-2.0 */\n"
        "/* powerfs_net_" + name + ".c - split from powerfs_net.c (mechanical refactor) */\n"
        "\n"
        + INCLUDES
        + '\n#include "powerfs_net_internal.h"\n'
        "\n"
    )


def transform_line(line):
    """Strip leading 'static ' unless the line is 'static inline' (local helper)."""
    if line.startswith("static inline "):
        return line
    if line.startswith("static "):
        return line[7:]
    return line


def extract(ranges):
    out = []
    for (s, e) in ranges:
        for i in range(s - 1, e):
            out.append(transform_line(lines[i]))
    return "".join(out)


# (start, end) 1-indexed inclusive line ranges per target file.
FILES = {
    "shard":    [(77, 183), (228, 305), (316, 423)],
    "sock":     [(425, 906)],
    # conn.c keeps powerfs_conn_get/put (static inline) at L1338-1350;
    # excludes L2190-2199 (decode forward decls), L2222 (topology fwd decl),
    # L2225 (TOPOLOGY_REFRESH_PENDING_BIT #define — in internal.h).
    "conn":     [(907, 2189), (2200, 2221), (2223, 2224), (2226, 3529)],
    "req":      [(3530, 4962)],
    # inode.c excludes L4974-4976 (parse_file_layout forward decl — in internal.h)
    "inode":    [(4963, 4973), (4977, 6201), (6597, 7039)],
    "data":     [(6202, 6596), (8005, 8511)],
    "xattr":    [(7040, 7344)],
    # pool.c gets init/exit fn bodies (L7345-7388); EXPORT block L7389-7412
    # stays in powerfs_net.c. multi-pool section L7414-7727.
    "pool":     [(7345, 7388), (7414, 7727)],
    "discover": [(7728, 8003), (9162, 10199)],
    "lease":    [(8512, 9161)],
}

# Special post-processing per file (string replacements after transform).
ANON_STATFS = (
    "struct {\n"
    "    __u64 total_size;\n"
    "    __u64 free_size;\n"
    "    __u64 total_files;\n"
    "    __u64 free_inodes;\n"
    "    __u32 block_size;\n"
    "    unsigned long cached_jiffies;\n"
    "    bool valid;\n"
    "} g_statfs_cache;\n"
)
TTL_DEFINE = "#define POWERFS_STATFS_CACHE_TTL    (30 * HZ)  /* 30 seconds */\n"

for name, ranges in FILES.items():
    path = os.path.join(DST, "powerfs_net_" + name + ".c")
    content = make_preamble(name) + extract(ranges)

    if name == "inode":
        # Replace anonymous struct with named struct (matches internal.h).
        if ANON_STATFS in content:
            content = content.replace(ANON_STATFS,
                                      "struct powerfs_statfs_cache g_statfs_cache;\n")
        else:
            print("WARNING: anon statfs struct not found in inode.c")
        # Remove duplicate TTL #define (already in internal.h).
        content = content.replace(TTL_DEFINE, "")

    with open(path, "w") as f:
        f.write(content)
    print("powerfs_net_%s.c: %d lines" % (name, content.count("\n")))

# Rebuild powerfs_net.c as the slim entry point:
#   file header + includes + #include internal.h + globals (L60-76)
#   + export block 1 (L7389-7413) + export block 2 (L10201-10222)
net = (
    "/*\n"
    " * PowerFS 内核态 powerfs-net 协议实现 (模块入口)\n"
    " *\n"
    " * 直接在内核中实现 powerfs-net 二进制协议，通过 TCP 连接与 Filer 通信。\n"
    " * 本文件拆分后仅保留模块入口、全局变量定义和符号导出。实现逻辑分布在\n"
    " * powerfs_net_*.c 中，跨文件内部符号声明见 powerfs_net_internal.h。\n"
    " */\n"
    "\n"
    + INCLUDES
    + '\n#include "powerfs_net_internal.h"\n'
    "\n"
    + "".join(transform_line(l) for l in lines[59:76])   # L60-76 globals
    + "\n"
    + "".join(transform_line(l) for l in lines[7388:7413])  # L7389-7413
    + "\n"
    + "".join(transform_line(l) for l in lines[10200:10222])  # L10201-10222
)
net_path = os.path.join(DST, "powerfs_net.c")
with open(net_path, "w") as f:
    f.write(net)
print("powerfs_net.c: %d lines" % net.count("\n"))

print("Split complete.")
