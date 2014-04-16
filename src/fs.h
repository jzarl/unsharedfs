/*
 * Unshared File System
 * Copyright 2014 Johannes Zarl <johannes.zarl@jku.at>
 * A FUSE Filesystem that diverts access to a different locations
 * based on the accessor's uid.
 *
 * This filesystem is based on the FUSE tutorial by Joseph J. Pfeiffer,
 * available at http://www.cs.nmsu.edu/~pfeiffer/fuse-tutorial/
 *
 * This program can be distributed under the terms of the GNU GPLv3.
 * See the file COPYING.
 *
 * This code is derived from function prototypes found /usr/include/fuse/fuse.h
 * Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 * His code is licensed under the LGPLv2.
 * A copy of that code is included in the file fuse.h
 */

#ifndef UNSHAREDFS_FS_H_
#define UNSHAREDFS_FS_H_

// The FUSE API has been changed a number of times.  So, our code
// needs to define the version of the API that we assume.  As of this
// writing, the most current API version is 26
#define FUSE_USE_VERSION 26

#include <sys/types.h>
#include <fuse.h>

enum unsharedfs_fsmode {
	UID_ONLY,
	GID_ONLY
};
		   

// maintain unsharedfs state in here
struct unsharedfs_state {
	uid_t base_uid;
	gid_t base_gid;
    char *rootdir;
	char *defaultdir;
	int allow_other_isset;
	int fsmode;
	int check_ownership;
};

int unsharedfs_access(const char *path, int mask);
int unsharedfs_chmod(const char *path, mode_t mode);
int unsharedfs_chown(const char *path, uid_t uid, gid_t gid);
int unsharedfs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int unsharedfs_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi);
int unsharedfs_fsync(const char *path, int datasync, struct fuse_file_info *fi);
int unsharedfs_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi);
int unsharedfs_getattr(const char *path, struct stat *statbuf);
int unsharedfs_getxattr(const char *path, const char *name, char *value, size_t size);
int unsharedfs_link(const char *path, const char *newpath);
int unsharedfs_listxattr(const char *path, char *list, size_t size);
int unsharedfs_mkdir(const char *path, mode_t mode);
int unsharedfs_mknod(const char *path, mode_t mode, dev_t dev);
int unsharedfs_open(const char *path, struct fuse_file_info *fi);
int unsharedfs_opendir(const char *path, struct fuse_file_info *fi);
int unsharedfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int unsharedfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int unsharedfs_readlink(const char *path, char *link, size_t size);
int unsharedfs_release(const char *path, struct fuse_file_info *fi);
int unsharedfs_releasedir(const char *path, struct fuse_file_info *fi);
int unsharedfs_removexattr(const char *path, const char *name);
int unsharedfs_rename(const char *path, const char *newpath);
int unsharedfs_rmdir(const char *path);
int unsharedfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags);
int unsharedfs_statfs(const char *path, struct statvfs *statv);
int unsharedfs_symlink(const char *path, const char *link);
int unsharedfs_truncate(const char *path, off_t newsize);
int unsharedfs_unlink(const char *path);
int unsharedfs_utimens(const char *path, const struct timespec tv[2]);
int unsharedfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
void *unsharedfs_init(struct fuse_conn_info *conn);
void unsharedfs_destroy(void *userdata);
#endif
