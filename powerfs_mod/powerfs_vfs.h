/*
 * powerfs_vfs.h - cross-file declarations for the split powerfs_fs.c
 *
 * Declares functions/variables/ops-structs that are referenced from more
 * than one translation unit. Functions only used within their own .c file
 * remain `static` and are NOT declared here.
 *
 * This header is included after "powerfs.h", so all core types
 * (struct powerfs_inode_info, struct powerfs_cap, etc.) are already visible.
 */
#ifndef _POWERFS_VFS_H
#define _POWERFS_VFS_H

#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/pagemap.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>
#include <linux/xattr.h>
#include <linux/mnt_idmapping.h>

/* ---- global variables (defined in super.c / inode.c) ---- */
extern struct kmem_cache *powerfs_inode_cachep;
extern struct kmem_cache *powerfs_dentry_cachep;
extern struct workqueue_struct *powerfs_refresh_wq;

/* ---- ops structs (referenced across files) ---- */
extern const struct file_operations powerfs_file_operations;
extern const struct file_operations powerfs_dir_operations;
extern const struct address_space_operations powerfs_aops;
extern const struct dentry_operations powerfs_dentry_operations;
extern const struct xattr_handler * const powerfs_xattr_handlers[];
extern const struct netfs_request_ops powerfs_netfs_ops;

/* ---- inode.c ---- */
int powerfs_write_inode(struct inode *inode, struct writeback_control *wbc);
void powerfs_setattr_work_fn(struct work_struct *work);
void powerfs_inode_init_once(void *foo);

/* ---- caps.c ---- */
void powerfs_cap_recall_notify_handler(u64 ino, const char *lease_token,
                                       size_t token_len, __u8 recall_mask,
                                       __u8 retain_mask, __u64 epoch);
void powerfs_cap_upgrade_notify_handler(u64 ino, const char *lease_token,
                                        size_t token_len, __u8 new_granted,
                                        __u64 epoch, __u64 sn);
int cap_open_grant_and_issue(struct powerfs_inode_info *pi, bool is_write_open);
int cap_send_release(struct powerfs_inode_info *pi, struct powerfs_cap *cap);

/* ---- dentry.c ---- */
void powerfs_fill_dentry_lease(struct dentry *dentry, struct inode *dir,
                               u64 lease_ttl_ms);

/* ---- dir.c ---- */
int powerfs_create(struct mnt_idmap *idmap, struct inode *dir,
                   struct dentry *dentry, umode_t mode, bool excl);
int powerfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
                  struct dentry *dentry, umode_t mode, dev_t dev);
struct dentry *powerfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
                             struct dentry *dentry, umode_t mode);
int powerfs_rmdir(struct inode *dir, struct dentry *dentry);
int powerfs_atomic_open(struct inode *dir, struct dentry *dentry,
                        struct file *file, unsigned open_flag,
                        umode_t create_mode);
int powerfs_unlink(struct inode *dir, struct dentry *dentry);
int powerfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
                    struct dentry *dentry, const char *symname);
int powerfs_link(struct dentry *old_dentry, struct inode *dir,
                 struct dentry *new_dentry);
int powerfs_add_dir_entry(struct inode *dir, u64 ino,
                          unsigned int type, const char *name);
int powerfs_remove_dir_entry(struct inode *dir, const char *name);
void powerfs_clear_dir_entries(struct inode *dir);
void powerfs_invalidate_dir_lease(struct inode *dir);

/* ---- file.c ---- */
int powerfs_file_open(struct inode *inode, struct file *file);
int powerfs_lock(struct file *filp, int cmd, struct file_lock *fl);
int powerfs_flock(struct file *filp, int cmd, struct file_lock *fl);
int powerfs_dir_fsync(struct file *file, loff_t start, loff_t end, int datasync);
long powerfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

/* ---- lease.c ---- */
void release_all_leases(struct inode *inode);
int ensure_lease(struct inode *inode, loff_t offset);
void powerfs_lease_renew_work_func(struct work_struct *work);
int powerfs_get_lease_token(struct inode *inode, loff_t offset,
                            char *token, size_t *token_len);

/* ---- xattr.c ---- */
ssize_t powerfs_listxattr(struct dentry *dentry, char *buffer, size_t size);

/* ---- super.c ---- */
int powerfs_quota_check_max_files(struct inode *dir);
int powerfs_quota_check_max_bytes(struct inode *inode, loff_t newlen);

#endif /* _POWERFS_VFS_H */
