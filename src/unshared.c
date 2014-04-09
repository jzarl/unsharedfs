/*
 * Unshared File System
 * Copyright 2014 Johannes Zarl <johannes.zarl@jku.at>
 * A FUSE Filesystem that diverts access to an different locations
 * based on the accessor's uid.
 *
 * Bugs:
 *  - uses path length bounded by PATH_MAX
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
 * 
 * gcc -Wall `pkg-config fuse --cflags --libs` -o unshared unshared.c
 */

// The FUSE API has been changed a number of times.  So, our code
// needs to define the version of the API that we assume.  As of this
// writing, the most current API version is 26
#define FUSE_USE_VERSION 26

// for seteuid
#define _XOPEN_SOURCE 600

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/xattr.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#ifdef HAVE_SYSLOG
#	define LOG(prio, fmt, ...) syslog((prio),(fmt),__VA_ARGS__)
#else
#	define LOG_DEBUG
#	define LOG_INFO
#	define LOG_NOTICE
#	define LOG_WARNING
#	define LOG_ERR
#	define LOG(prio, fmt, ...) fprintf(stderr,(fmt),__VA_ARGS__)
#endif

#define ERRMSG_MAX 5

// maintain unsharedfs state in here
struct unsharedfs_state {
	uid_t base_uid;
	gid_t base_gid;
    char *rootdir;
};
#define PRIVATE_DATA ((struct unsharedfs_state *) fuse_get_context()->private_data)

//  All the paths I see are relative to the root of the mounted
//  filesystem.  In order to get to the underlying filesystem, I need to
//  have the mountpoint.  I'll save it away early on in main(), and then
//  whenever I need a path for something I'll call this to construct
//  it.
// return 0/false on overflow
static int unsharedfs_fullpath(char fpath[PATH_MAX], const char *path)
{
	int success = ( PATH_MAX > snprintf(fpath,PATH_MAX,"%s/%d%s",PRIVATE_DATA->rootdir,fuse_get_context()->uid,path) );
	if (!success)
		LOG(LOG_ERR,"Long path truncated: %s",path);
	return success;
}

/**
 * Take the uid/gid of the current context.
 */
static void unsharedfs_take_context_id()
{
	// Set gid first, because we won't be able after setting the uid.
	if ( setegid(fuse_get_context()->gid) != 0)
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		LOG(LOG_WARNING,"unsharedfs_take_context_id: failed to set egid from %d to %d: %s"
				,getegid()
				,fuse_get_context()->gid
				,errmsg
		   );
	}
	if ( seteuid(fuse_get_context()->uid) != 0 )
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		LOG(LOG_WARNING,"unsharedfs_take_context_id: failed to set euid from %d to %d: %s"
				,geteuid()
				,fuse_get_context()->uid
				,errmsg
		   );
	}
	LOG(LOG_DEBUG,"uid/gid = %d/%d, euid/egid = %d/%d",getuid(),getgid(),geteuid(),getegid());
}
/**
 * Drop the uid/gid of the current context.
 */
static void unsharedfs_drop_context_id()
{
	if ( seteuid(PRIVATE_DATA->base_uid) != 0)
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		LOG(LOG_WARNING,"unsharedfs_drop_context_id: failed to set euid from %d to %d: %s"
				,geteuid()
				,fuse_get_context()->uid
				,errmsg
		   );
	}
	if ( setegid(PRIVATE_DATA->base_gid) != 0)
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		LOG(LOG_WARNING,"unsharedfs_drop_context_id: failed to set egid from %d to %d: %s"
				,getegid()
				,fuse_get_context()->gid
				,errmsg
		   );
	}
	LOG(LOG_DEBUG,"uid/gid = %d/%d, euid/egid = %d/%d",getuid(),getgid(),geteuid(),getegid());
}

///////////////////////////////////////////////////////////
//
// Prototypes for all these functions, and the C-style comments,
// come indirectly from /usr/include/fuse.h
//
/** Get file attributes.
 *
 * Similar to stat().  The 'st_dev' and 'st_blksize' fields are
 * ignored.  The 'st_ino' field is ignored except if the 'use_ino'
 * mount option is given.
 */
int unsharedfs_getattr(const char *path, struct stat *statbuf)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = lstat(fpath, statbuf);
	unsharedfs_drop_context_id();
	if (retstat != 0)
		retstat = -errno;

	return retstat;
}

/** Read the target of a symbolic link
 *
 * The buffer should be filled with a null terminated string.  The
 * buffer size argument includes the space for the terminating
 * null character.  If the linkname is too long to fit in the
 * buffer, it should be truncated.  The return value should be 0
 * for success.
 */
// Note the system readlink() will truncate and lose the terminating
// null.  So, the size passed to to the system readlink() must be one
// less than the size passed to unsharedfs_readlink()
// unsharedfs_readlink() code by Bernardo F Costa (thanks!)
int unsharedfs_readlink(const char *path, char *link, size_t size)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = readlink(fpath, link, size - 1);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;
	else  {
		link[retstat] = '\0';
		retstat = 0;
	}

	return retstat;
}

/** Create a file node
 *
 * There is no create() operation, mknod() will be called for
 * creation of all non-directory, non-symlink nodes.
 */
// shouldn't that comment be "if" there is no.... ?
int unsharedfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	// On Linux this could just be 'mknod(path, mode, rdev)' but this
	//  is more portable
	if (S_ISREG(mode)) {
		retstat = open(fpath, O_CREAT | O_EXCL | O_WRONLY, mode);
		if (retstat < 0)
			retstat = -errno;
		else {
			retstat = close(retstat);
			if (retstat < 0)
				retstat = -errno;
		}
	} else
		if (S_ISFIFO(mode)) {
			retstat = mkfifo(fpath, mode);
			if (retstat < 0)
				retstat = -errno;
		} else {
			retstat = mknod(fpath, mode, dev);
			if (retstat < 0)
				retstat = -errno;
		}
	unsharedfs_drop_context_id();

	return retstat;
}

/** Create a directory */
int unsharedfs_mkdir(const char *path, mode_t mode)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = mkdir(fpath, mode);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Remove a file */
int unsharedfs_unlink(const char *path)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = unlink(fpath);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Remove a directory */
int unsharedfs_rmdir(const char *path)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = rmdir(fpath);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Create a symbolic link */
// The parameters here are a little bit confusing, but do correspond
// to the symlink() system call.  The 'path' is where the link points,
// while the 'link' is the link itself.  So we need to leave the path
// unaltered, but insert the link into the mounted directory.
int unsharedfs_symlink(const char *path, const char *link)
{
	int retstat = 0;
	char flink[PATH_MAX];

	if (!unsharedfs_fullpath(flink, link))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = symlink(path, flink);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Rename a file */
// both path and newpath are fs-relative
int unsharedfs_rename(const char *path, const char *newpath)
{
	int retstat = 0;
	char fpath[PATH_MAX];
	char fnewpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;
	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = rename(fpath, fnewpath);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Create a hard link to a file */
int unsharedfs_link(const char *path, const char *newpath)
{
	int retstat = 0;
	char fpath[PATH_MAX], fnewpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;
	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = link(fpath, fnewpath);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Change the permission bits of a file */
int unsharedfs_chmod(const char *path, mode_t mode)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = chmod(fpath, mode);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Change the owner and group of a file */
int unsharedfs_chown(const char *path, uid_t uid, gid_t gid)

{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = chown(fpath, uid, gid);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Change the size of a file */
int unsharedfs_truncate(const char *path, off_t newsize)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = truncate(fpath, newsize);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Change the access and/or modification times of a file */
/* note -- I'll want to change this as soon as 2.6 is in debian testing */
int unsharedfs_utime(const char *path, struct utimbuf *ubuf)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = utime(fpath, ubuf);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** File open operation
 *
 * No creation, or truncation flags (O_CREAT, O_EXCL, O_TRUNC)
 * will be passed to open().  Open should check if the operation
 * is permitted for the given flags.  Optionally open may also
 * return an arbitrary filehandle in the fuse_file_info structure,
 * which will be passed to all file operations.
 *
 * Changed in version 2.2
 */
int unsharedfs_open(const char *path, struct fuse_file_info *fi)
{
	int retstat = 0;
	int fd;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	fd = open(fpath, fi->flags);
	unsharedfs_drop_context_id();
	if (fd < 0)
		retstat = -errno;

	fi->fh = fd;
	return retstat;
}

/** Read data from an open file
 *
 * Read should return exactly the number of bytes requested except
 * on EOF or error, otherwise the rest of the data will be
 * substituted with zeroes.  An exception to this is when the
 * 'direct_io' mount option is specified, in which case the return
 * value of the read system call will reflect the return value of
 * this operation.
 *
 * Changed in version 2.2
 */
// I don't fully understand the documentation above -- it doesn't
// match the documentation for the read() system call which says it
// can return with anything up to the amount of data requested. nor
// with the fusexmp code which returns the amount of data also
// returned by read.
int unsharedfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	// 2014-04-07 ZaJ: TODO FIXME: is this correct? what exactly is this one doing???
	// no need to get fpath on this one, since I work from fi->fh not the path
	retstat = pread(fi->fh, buf, size, offset);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Write data to an open file
 *
 * Write should return exactly the number of bytes requested
 * except on error.  An exception to this is when the 'direct_io'
 * mount option is specified (see read operation).
 *
 * Changed in version 2.2
 */
// As  with read(), the documentation above is inconsistent with the
// documentation for the write() system call.
int unsharedfs_write(const char *path, const char *buf, size_t size, off_t offset,
		struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	// 2014-04-07 ZaJ: TODO FIXME: is this correct? what exactly is this one doing???
	// no need to get fpath on this one, since I work from fi->fh not the path
	retstat = pwrite(fi->fh, buf, size, offset);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Get file system statistics
 *
 * The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 *
 * Replaced 'struct statfs' parameter with 'struct statvfs' in
 * version 2.5
 */
int unsharedfs_statfs(const char *path, struct statvfs *statv)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	// get stats for underlying filesystem
	retstat = statvfs(fpath, statv);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Release an open file
 *
 * Release is called when there are no more references to an open
 * file: all file descriptors are closed and all memory mappings
 * are unmapped.
 *
 * For every open() call there will be exactly one release() call
 * with the same flags and file descriptor.  It is possible to
 * have a file opened more than once, in which case only the last
 * release will mean, that no more reads/writes will happen on the
 * file.  The return value of release is ignored.
 *
 * Changed in version 2.2
 */
int unsharedfs_release(const char *path, struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	// 2014-04-07 ZaJ: TODO FIXME: is this correct?
	// We need to close the file.  Had we allocated any resources
	// (buffers etc) we'd need to free them here as well.
	retstat = close(fi->fh);
	unsharedfs_drop_context_id();

	return retstat;
}

/** Synchronize file contents
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 *
 * Changed in version 2.2
 */
int unsharedfs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	if (datasync)
		retstat = fdatasync(fi->fh);
	else
		retstat = fsync(fi->fh);

	if (retstat < 0)
		retstat = -errno;
	unsharedfs_drop_context_id();

	return retstat;
}

/** Set extended attributes */
int unsharedfs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = lsetxattr(fpath, name, value, size, flags);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Get extended attributes */
int unsharedfs_getxattr(const char *path, const char *name, char *value, size_t size)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = lgetxattr(fpath, name, value, size);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** List extended attributes */
int unsharedfs_listxattr(const char *path, char *list, size_t size)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = llistxattr(fpath, list, size);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Remove extended attributes */
int unsharedfs_removexattr(const char *path, const char *name)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = lremovexattr(fpath, name);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/** Open directory
 *
 * This method should check if the open operation is permitted for
 * this  directory
 *
 * Introduced in version 2.3
 */
int unsharedfs_opendir(const char *path, struct fuse_file_info *fi)
{
	DIR *dp;
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	dp = opendir(fpath);
	unsharedfs_drop_context_id();
	if (dp == NULL)
		retstat = -errno;

	fi->fh = (intptr_t) dp;

	return retstat;
}

/** Read directory
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 * Introduced in version 2.3
 */
int unsharedfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
		struct fuse_file_info *fi)
{
	unsharedfs_take_context_id();
	// 2014-04-07 ZaJ: TODO FIXME  what about unshared_fullpath?
	int retstat = 0;
	DIR *dp;
	struct dirent *de;

	// once again, no need for fullpath -- but note that I need to cast fi->fh
	dp = (DIR *) (uintptr_t) fi->fh;

	// Every directory contains at least two entries: . and ..  If my
	// first call to the system readdir() returns NULL I've got an
	// error; near as I can tell, that's the only condition under
	// which I can get an error from readdir()
	de = readdir(dp);
	if (de == 0) {
		retstat = -errno;
		unsharedfs_drop_context_id();
		return retstat;
	}

	// This will copy the entire directory into the buffer.  The loop exits
	// when either the system readdir() returns NULL, or filler()
	// returns something non-zero.  The first case just means I've
	// read the whole directory; the second means the buffer is full.
	do {
		if (filler(buf, de->d_name, NULL, 0) != 0) {
			unsharedfs_drop_context_id();
			return -ENOMEM;
		}
	} while ((de = readdir(dp)) != NULL);

	unsharedfs_drop_context_id();
	return retstat;
}

/** Release directory
 *
 * Introduced in version 2.3
 */
int unsharedfs_releasedir(const char *path, struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	closedir((DIR *) (uintptr_t) fi->fh);
	unsharedfs_drop_context_id();

	return retstat;
}

/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 * Introduced in version 2.3
 * Changed in version 2.6
 */
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
void *unsharedfs_init(struct fuse_conn_info *conn)
{
	struct unsharedfs_state *pdata = PRIVATE_DATA;
#ifdef HAVE_SYSLOG
	openlog("unsharedfs",LOG_PID,LOG_USER);
#endif
	LOG(LOG_INFO,"initialising unsharedfs with base uid/gid %d/%d at %s"
			,pdata->base_uid
			,pdata->base_gid
			,pdata->rootdir);


	return pdata;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 *
 * Introduced in version 2.3
 */
void unsharedfs_destroy(void *userdata)
{
#ifdef HAVE_SYSLOG
	closelog();
#endif
}

/**
 * Check file access permissions
 *
 * This will be called for the access() system call.  If the
 * 'default_permissions' mount option is given, this method is not
 * called.
 *
 * This method is not called under Linux kernel versions 2.4.x
 *
 * Introduced in version 2.5
 */
int unsharedfs_access(const char *path, int mask)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	retstat = access(fpath, mask);
	unsharedfs_drop_context_id();

	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/**
 * Create and open a file
 *
 * If the file does not exist, first create it with the specified
 * mode, and then open it.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the mknod() and open() methods
 * will be called instead.
 *
 * Introduced in version 2.5
 */
int unsharedfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int retstat = 0;
	char fpath[PATH_MAX];
	int fd;

	if (!unsharedfs_fullpath(fpath, path))
		return -ENAMETOOLONG;

	unsharedfs_take_context_id();
	fd = creat(fpath, mode);
	unsharedfs_drop_context_id();
	if (fd < 0)
		retstat = -errno;

	fi->fh = fd;

	return retstat;
}

/**
 * Change the size of an open file
 *
 * This method is called instead of the truncate() method if the
 * truncation was invoked from an ftruncate() system call.
 *
 * If this method is not implemented or under Linux kernel
 * versions earlier than 2.6.15, the truncate() method will be
 * called instead.
 *
 * Introduced in version 2.5
 */
int unsharedfs_ftruncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	retstat = ftruncate(fi->fh, offset);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/**
 * Get attributes from an open file
 *
 * This method is called instead of the getattr() method if the
 * file information is available.
 *
 * Currently this is only called after the create() method if that
 * is implemented (see above).  Later it may be called for
 * invocations of fstat() too.
 *
 * Introduced in version 2.5
 */
// Since it's currently only called after unsharedfs_create(), and unsharedfs_create()
// opens the file, I ought to be able to just use the fd and ignore
// the path...
int unsharedfs_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	retstat = fstat(fi->fh, statbuf);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;


	return retstat;
}

struct fuse_operations unsharedfs_oper = {
	.getattr = unsharedfs_getattr,
	.readlink = unsharedfs_readlink,
	.mknod = unsharedfs_mknod,
	.mkdir = unsharedfs_mkdir,
	.unlink = unsharedfs_unlink,
	.rmdir = unsharedfs_rmdir,
	.symlink = unsharedfs_symlink,
	.rename = unsharedfs_rename,
	.link = unsharedfs_link,
	.chmod = unsharedfs_chmod,
	.chown = unsharedfs_chown,
	.truncate = unsharedfs_truncate,
	// TODO: implement utimens()
	.utime = unsharedfs_utime,
	.open = unsharedfs_open,
	.read = unsharedfs_read,
	.write = unsharedfs_write,
	.statfs = unsharedfs_statfs,
	.release = unsharedfs_release,
	.fsync = unsharedfs_fsync,
	.setxattr = unsharedfs_setxattr,
	.getxattr = unsharedfs_getxattr,
	.listxattr = unsharedfs_listxattr,
	.removexattr = unsharedfs_removexattr,
	.opendir = unsharedfs_opendir,
	.readdir = unsharedfs_readdir,
	.releasedir = unsharedfs_releasedir,
	.init = unsharedfs_init,
	.destroy = unsharedfs_destroy,
	.access = unsharedfs_access,
	.create = unsharedfs_create,
	.ftruncate = unsharedfs_ftruncate,
	.fgetattr = unsharedfs_fgetattr,
		//TODO: .utimns
		//TODO: .fallocate
};

void unsharedfs_usage()
{
	fprintf(stderr, "usage:  unsharedfs -o allow_other [FUSE and mount options] sourceDir mountPoint\n");
	abort();
}

int main(int argc, char *argv[])
{
	int fuse_stat;
	struct unsharedfs_state *pdata;

	// Perform some sanity checking on the command line:  make sure
	// there are enough arguments, and that neither of the last two
	// start with a hyphen (this will break if you actually have a
	// rootpoint or mountpoint whose name starts with a hyphen, but so
	// will a zillion other programs)
	if ((argc < 3) || (argv[argc-2][0] == '-') || (argv[argc-1][0] == '-'))
		unsharedfs_usage();

	pdata = malloc(sizeof(struct unsharedfs_state));
	if (pdata == NULL) {
		perror("main calloc");
		abort();
	}

	// save original uid/gid:
	pdata->base_uid = getuid();
	pdata->base_gid = getgid();

	// Pull the rootdir out of the argument list and save it in my
	// internal data
	pdata->rootdir = realpath(argv[argc-2], NULL);
	argv[argc-2] = argv[argc-1];
	argv[argc-1] = NULL;
	argc--;

	if ( getuid() != 0 && geteuid() != 0 )
	{
		fprintf(stderr,"warning: file system needs root privileges for proper function.\n");
		fprintf(stderr,"All accesses will be redirected to %s/%d and be executed under the uid of the current user.\n",pdata->rootdir,getuid());
	}

	// turn over control to fuse
	fprintf(stderr, "about to call fuse_main\n");
	fuse_stat = fuse_main(argc, argv, &unsharedfs_oper, pdata);
	fprintf(stderr, "fuse_main returned %d\n", fuse_stat);

	return fuse_stat;
}
