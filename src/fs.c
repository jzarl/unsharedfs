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

// The FUSE API has been changed a number of times.  So, our code
// needs to define the version of the API that we assume.  As of this
// writing, the most current API version is 26
#define FUSE_USE_VERSION 26

// for utimensat
#define _XOPEN_SOURCE 700
// for vsyslog
#define _BSD_SOURCE

#include "fs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <fuse_opt.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/fsuid.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <sys/stat.h>
#include <stdarg.h>
#ifdef HAVE_SYSLOG
#include <syslog.h>
#endif

#define PRIVATE_DATA ((struct unsharedfs_state *) fuse_get_context()->private_data)

#ifndef HAVE_SYSLOG
#	define LOG_ERR     0
#	define LOG_WARNING 1
#	define LOG_NOTICE  2
#	define LOG_INFO    3
#	define LOG_DEBUG   4
#endif

void logmsg(int prio, const char *fmt, ...)
{
	va_list args;
	va_start( args, fmt);

#ifdef HAVE_SYSLOG
	if (prio < LOG_DEBUG && PRIVATE_DATA->use_syslog)
		vsyslog(prio,fmt,args);
#endif
	// when in foreground-mode, this gets printed:
	vfprintf(stderr,fmt,args);
	va_end(args);
	fprintf(stderr,"\n");
}

// the buffer size used for error messages
#define ERRMSG_MAX 512

/**
 * Compute the diverted full path for a relative path.
 * The path supplied by fuse is always relative to the mountpoint,
 * and has been sanitized.
 *
 * This function basically prepends BASEDIR/UID (or GID) and does some
 * checks to validate the result. If an error is detected, errno is set accordingly.
 *
 * @param fpath the a reference to the return buffer
 * @param path the relative path to the mountpoint
 * @return 1 on success, 0 on error.
 */
static int unsharedfs_fullpath(char fpath[PATH_MAX], const char *path)
{
	struct stat sb;
	size_t pathlen;
	struct unsharedfs_state *pdata = PRIVATE_DATA;
	// size_t is big enough for either uid_t or gid_t:
	size_t ugid;

	if ( pdata->fsmode == UID_ONLY )
		ugid = fuse_get_context()->uid;
	else
		ugid = fuse_get_context()->gid;

	// assemble "base" directory:
	pathlen = snprintf(fpath,PATH_MAX,"%s/%ld",pdata->rootdir,ugid);
	if ( pathlen >= PATH_MAX )
	{
		logmsg(LOG_ERR,"Long path truncated: %s",path);
		errno = ENAMETOOLONG;
		return 0;
	}

	// does base directory exist?
	if ( stat(fpath,&sb) != 0 )
	{
		// is a fallback directory defined?
		if (pdata->defaultdir)
		{ // no uid check in this case
			if (PATH_MAX <= snprintf(fpath,PATH_MAX,"%s/%s%s",pdata->rootdir,pdata->defaultdir,path) )
			{
				logmsg(LOG_ERR,"Long path truncated: %s",path);
				errno = ENAMETOOLONG;
				return 0;
			}
			logmsg(LOG_DEBUG,"diverting to fallback directory %s/%s",pdata->rootdir,pdata->defaultdir);
			return 1;
		}
		logmsg(LOG_WARNING,"missing directory: %s/%ld",pdata->rootdir,ugid);
		errno = EBUSY;
		return 0;
	}
	
	// base directory is a directory?
	if ( ! (S_IFDIR & sb.st_mode) )
	{
		logmsg(LOG_ERR,"not a directory: %s/%ld",pdata->rootdir,ugid);
		errno = ENOTDIR;
		return 0;
	}
	
	// uid matches owner?
	if ( pdata->check_ownership && ugid != sb.st_uid )
	{
		// pin name to uid:
		logmsg(LOG_ERR,"directory name does not match owner: %s/%ld (owner: %d)",pdata->rootdir,ugid,sb.st_uid);
		errno = EACCES;
		return 0;
	}
	
	// add "path" component:
	if (strlen(path) + pathlen + 1 >= PATH_MAX)
	{
		logmsg(LOG_ERR,"path too long: %s%s",fpath,path);
		errno = ENAMETOOLONG;
		return 0;
	}
	strncat(fpath,path,PATH_MAX-1-pathlen);
	return 1;
}

/**
 * Take the uid/gid of the current context.
 */
static void unsharedfs_take_context_id()
{
	// some internal fuse calls have an empty context:
	if ( fuse_get_context()->pid == 0 )
		return;

	// from the manpage:
	// On success, the previous value of fsgid is returned.  On error, the current value of fsgid is returned.
	if ( setfsgid(fuse_get_context()->gid) != PRIVATE_DATA->base_gid)
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		logmsg(LOG_WARNING,"unsharedfs_take_context_id: failed to set fsgid from %d to %d: %s"
				,PRIVATE_DATA->base_gid
				,fuse_get_context()->gid
				,errmsg
		   );
	}
	// from the manpage:
	// On success, the previous value of fsuid is returned.  On error, the current value of fsuid is returned.
	if ( setfsuid(fuse_get_context()->uid) != PRIVATE_DATA->base_uid )
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		logmsg(LOG_WARNING,"unsharedfs_take_context_id: failed to set fsuid from %d to %d: %s"
				,PRIVATE_DATA->base_uid
				,fuse_get_context()->uid
				,errmsg
		   );
	}
}
/**
 * Drop the uid/gid of the current context.
 */
static void unsharedfs_drop_context_id()
{
	// some internal fuse calls have an empty context:
	if ( fuse_get_context()->pid == 0 )
		return;

	if ( setfsuid(PRIVATE_DATA->base_uid) != fuse_get_context()->uid )
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		logmsg(LOG_WARNING,"unsharedfs_drop_context_id: failed to set fsuid from %d to %d: %s"
				,fuse_get_context()->uid
				,PRIVATE_DATA->base_uid
				,errmsg
		   );
	}
	if ( setfsgid(PRIVATE_DATA->base_gid) != fuse_get_context()->gid)
	{
		char errmsg[ERRMSG_MAX];
		if ( strerror_r(errno, errmsg,ERRMSG_MAX) != 0 )
			errmsg[0] = '\0';
		logmsg(LOG_WARNING,"unsharedfs_drop_context_id: failed to set egid from %d to %d: %s"
				,fuse_get_context()->gid
				,PRIVATE_DATA->base_gid
				,errmsg
		   );
	}
}

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
		return -errno;

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
		return -errno;

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
 * This is called for creation of all non-directory, non-symlink nodes.
 * If the filesystem defines a create() method, then for regular files that will be called instead.
 */
int unsharedfs_mknod(const char *path, mode_t mode, dev_t dev)
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -errno;

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
		return -errno;

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
		return -errno;

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
		return -errno;

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
		return -errno;

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
		return -errno;
	if (!unsharedfs_fullpath(fnewpath, newpath))
		return -errno;

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
		return -errno;
	if (!unsharedfs_fullpath(fnewpath, newpath))
		return -errno;

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
		return -errno;

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
		return -errno;

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
		return -errno;

	unsharedfs_take_context_id();
	retstat = truncate(fpath, newsize);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;

	return retstat;
}

/**
 * Change the access and modification times of a file with nanosecond resolution.
 *
 * This supersedes the old utime() interface. New applications should use this.
 * See the utimensat(2) man page for details.
 *
 * Introduced in version 2.6
 */
int unsharedfs_utimens(const char *path, const struct timespec tv[2])
{
	int retstat = 0;
	char fpath[PATH_MAX];

	if (!unsharedfs_fullpath(fpath, path))
		return -errno;

	unsharedfs_take_context_id();
	// fpath is absolute -> dirfd parameter (AT_FDCWD) is ignored
	retstat = utimensat(AT_FDCWD, fpath, tv, 0);
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
		return -errno;

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
int unsharedfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int retstat = 0;

	unsharedfs_take_context_id();
	// unsharedfs_open() already put the file handle into fi->fh.
	// with flag_nopath, path is not even set!
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
	// unsharedfs_open() already put the file handle into fi->fh.
	// with flag_nopath, path is not even set!
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
		return -errno;

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
	// We need to close the file.  Had we allocated any resources
	// (buffers etc) we'd need to free them here as well.
	// unsharedfs_open() already put the file handle into fi->fh.
	// with flag_nopath, path is not even set!
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

	// unsharedfs_open() already put the file handle into fi->fh.
	// with flag_nopath, path is not even set!
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
		return -errno;

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
		return -errno;

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
		return -errno;

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
		return -errno;

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
		return -errno;

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
	int retstat = 0;
	DIR *dp;
	struct dirent *de;

	// unsharedfs_open() already put the (directory-)file handle into fi->fh.
	// once again, no need for fullpath -- but note that I need to cast fi->fh
	// with flag_nopath, path is not even set!
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

	// unsharedfs_open() already put the file handle into fi->fh.
	// with flag_nopath, path is not even set!
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
	logmsg(LOG_INFO,"initialising unsharedfs with base uid/gid %d/%d at %s"
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
	struct unsharedfs_state *pdata = (struct unsharedfs_state*) userdata;

	logmsg(LOG_INFO,"releasing unsharedfs at %s",pdata->rootdir);
#ifdef HAVE_SYSLOG
	closelog();
#endif

	// not strictly necessary, since the memory is freed on exit anyways:
	free(pdata->rootdir);
	free(pdata->defaultdir);
	free(pdata);
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
		return -errno;

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
		return -errno;

	unsharedfs_take_context_id();
	// fd = creat(fpath, mode);
	// some programs seemingly don't cope well with O_WRONLY
	fd = open(fpath, O_CREAT | O_EXCL | O_RDWR, mode);
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

	// unsharedfs_open() already put the file handle into fi->fh.
	// with flag_nopath, path is not even set!
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
int unsharedfs_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi)
{
	int retstat = 0;

	// unsharedfs_open() already put the file handle into fi->fh.
	// with flag_nopath, path is not even set!
	unsharedfs_take_context_id();
	retstat = fstat(fi->fh, statbuf);
	unsharedfs_drop_context_id();
	if (retstat < 0)
		retstat = -errno;


	return retstat;
}


