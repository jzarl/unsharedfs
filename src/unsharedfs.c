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

#define UNSHAREDFS_VERSION_STRING "unsharedfs 1.0-rc1"

#include "fs.h"

#include <fuse.h>
#include <fuse_opt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const struct fuse_operations unsharedfs_operations = {
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
	.utimens = unsharedfs_utimens,
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
	//TODO: .fallocate
};

static void unsharedfs_usage()
{
	printf( "Redirect file system access to another directory depending on the user id.\n"
			"\n"
			"Usage: unsharedfs -o allow_other [OPTIONS] BASEDIR MOUNTPOINT\n"
			"\n"
			"Options:\n"
			"  BASEDIR                   Base directory.\n"
			"                            All access for a user with a given uid is diverted\n"
			"                            from MOUNTPOINT/path to BASEDIR/uid/path.\n"
			"\n"
			"  -h, --help                Print this and exit.\n"
			"  -V, --version             Print version number and exit.\n"
			"\n"
			"File system behavior:\n"
			"      --fallback=dir        When the UID directory for a user does not exist,\n"
			"                            divert access to this path (relative to basedir).\n"
			"      --no-check-ownership  Allow access to the uid directory even if the owner\n"
			"                            does not match the directory name.\n"
			"      --use-gid             Use group id (gid) instead of the user id to determine\n"
			"                            the diverted path. Currently this implies \"--no-check-ownership\"\n"
			"\n"
			"FUSE options:\n"
			"  -o opt[,opt,...]          Mount options.\n"
			"  -o allow_other            Required for regular operation of unsharedfs.\n"
			"  -r, -o ro                 Mount strictly read-only.\n"
			"  -d, -o debug              Enable debug output (implies -f).\n"
			"  -f                        Foreground operation.\n"
			"\n"
		  );
}

enum unsharedfs_opt_key {
	KEY_VERSION,
	KEY_HELP,
	KEY_FALLBACK,
	KEY_ALLOW_OTHER,
	KEY_NO_CHECK_OWNERSHIP,
	KEY_USE_GID
};
static const struct fuse_opt unsharedfs_options[] = {
	// {char * template, int offset, int key}
	FUSE_OPT_KEY( "--version", KEY_VERSION ),
	FUSE_OPT_KEY( "-h", KEY_HELP),
	FUSE_OPT_KEY( "--help", KEY_HELP),
	FUSE_OPT_KEY( "--fallback=", KEY_FALLBACK),
	FUSE_OPT_KEY( "--no-check-ownership", KEY_NO_CHECK_OWNERSHIP),
	FUSE_OPT_KEY( "--use-gid", KEY_USE_GID),
	FUSE_OPT_KEY( "allow_other", KEY_ALLOW_OTHER),
	FUSE_OPT_END
};

/* for a description of this function, see the fuse_opt_proc_t definition in fuse_opt.h. */
static int unsharedfs_parse_options(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	struct unsharedfs_state *pdata = (struct unsharedfs_state*) data;
	// return 0 to consume the argument
	// return 1 to pass the argument to fuse
	// return -1 to abort
	switch ((enum unsharedfs_opt_key)key)
	{
		case KEY_VERSION:
			printf("%s\n",UNSHAREDFS_VERSION_STRING);
			printf( "\n"
					"Copyright (C) 2014 Johannes Zarl\n"
					"This is free software; see the source for copying conditions.  There is NO\n"
					"warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
					"\n"
					"Written by Johannes Zarl.\n"
				  );
			exit(0);
		break;
		case KEY_HELP:
			unsharedfs_usage();
			exit(0);
		break;
		case KEY_FALLBACK:
		{
			const int prefixlen = 11;
			size_t len = strlen(arg) - prefixlen;
			if ( strncmp("--fallback=",arg,prefixlen) != 0 )
			{
				fprintf(stderr, "Error in option parsing!");
				return -1;
			}
			pdata->defaultdir = malloc(len+1);
			if (pdata->defaultdir == NULL)
			{
				perror("unsharedfs parse options: malloc failed");
				return -1;
			}
			strncpy(pdata->defaultdir,arg + prefixlen,len);
			pdata->defaultdir[len] = '\0';
			return 0;
		}
		break;
		case KEY_NO_CHECK_OWNERSHIP:
			pdata->check_ownership = 0;
			return 0;
		break;
		case KEY_USE_GID:
			pdata->fsmode = GID_ONLY;
			pdata->check_ownership = 0;
			return 0;
		break;
		case KEY_ALLOW_OTHER:
			pdata->allow_other_isset = 1;
			return 1;
		break;
		default:
			if ( pdata->rootdir == 0 )
			{
				pdata->rootdir = realpath(arg, NULL);
				return 0;
			}
		return 1;
	}
}

int main(int argc, char *argv[])
{
	int fuse_stat;
	struct fuse_args args = FUSE_ARGS_INIT(argc,argv);
	struct unsharedfs_state *pdata;

	pdata = malloc(sizeof(struct unsharedfs_state));
	if (pdata == NULL) {
		perror("unsharedfs init");
		return 1;
	}

	// save original uid/gid:
	pdata->base_uid = getuid();
	pdata->base_gid = getgid();
	pdata->check_ownership = 0;
	pdata->fsmode = UID_ONLY;

	if (fuse_opt_parse(&args, pdata, unsharedfs_options, unsharedfs_parse_options) == -1)
	{
		/** error parsing options */
		return 1;
	}

	if ( getuid() != 0 && geteuid() != 0 )
	{
		fprintf(stderr,"warning: file system needs root privileges for proper function.\n");
		fprintf(stderr,"All accesses will be redirected to %s/%d and be executed under the uid of the current user.\n",pdata->rootdir,getuid());
	}
	if ( ! pdata->allow_other_isset )
	{
		fprintf(stderr,"warning: allow_other is not set. Specify \"-o allow_other\" to allow other users to access the mount point.\n");
		return 1;
	}

	// turn over control to fuse
	fuse_stat = fuse_main(args.argc, args.argv, &unsharedfs_operations, pdata);
	if ( fuse_stat != 0 )
		fprintf(stderr, "fuse_main returned %d\n", fuse_stat);

	return fuse_stat;
}
