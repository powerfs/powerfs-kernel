/*
 * powerfs_fs.c - split into per-subsystem files (CephFS-style layout)
 *
 * The original 13977-line powerfs_fs.c has been split into:
 *   powerfs_super.c  - super_operations + module-level (fill_super, metrics,
 *                      proc/debugfs, export ops, quota, statfs)
 *   powerfs_inode.c  - inode lifecycle + inode_operations (iget, alloc/free,
 *                      evict, init_inode, getattr/setattr/permission, fileattr,
 *                      posix acl, inode_operations tables, refresh work)
 *   powerfs_file.c   - file_operations (open/release/flush/llseek/fsync,
 *                      read/write iter, mmap, fallocate, lock/flock, ioctl)
 *   powerfs_dir.c    - directory operations (create/mkdir/mknod/unlink/
 *                      symlink/link/rename/readdir, dir entry management)
 *   powerfs_addr.c   - address_space_operations (writepages/writepage/
 *                      write_begin/write_end, direct_IO, dirty_folio, aops)
 *   powerfs_dentry.c - dentry_operations + dedup (d_init/d_release/
 *                      d_revalidate/d_prune/d_delete, dentry invalidate)
 *   powerfs_caps.c   - capability management (cap issue/revoke/flush/
 *                      check, get/put refs, cap notify)
 *   powerfs_locks.c  - MDLock subsystem (dead code, #if 0; Ceph MDS Locker)
 *   powerfs_lease.c  - lease management (ensure/release/renew)
 *   powerfs_xattr.c  - xattr operations (handler get/set, listxattr)
 *
 * Cross-file declarations live in powerfs_vfs.h.
 *
 * This file is no longer compiled (removed from Makefile); it is kept as a
 * historical marker. All implementation now lives in the files above.
 */
