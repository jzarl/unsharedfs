Unshared FS for FUSE
====================

Redirect file system access depending on the UID.  This allows several users to
access the "same" absolute path without interfering with each other.  Every
user gets their own individual *view* on the path.


IMPORTANT
---------

Do audit the code before you use it in any security-critical application.  The
code in its current form **has not been audited** in any form, and likely contains
bugs that are exploitable by a malicious user!


How it works
------------

Suppose you have two users *userA* and *userB*. Both users shall be able to
access the directory /my-directory, but they shall have two independent views
on the directory.

First, you have to create the physical locations that both users will be accessing:

```
mkdir /unshared
theUID=`id -u userA`
mkdir /unshared/$theUID
chown userA /unshared/$theUID
theUID=`id -u userB`
mkdir /unshared/$theUID
chown userB /unshared/$theUID
```

or, if you prefer for loops in bash:
```
rootdir=/unshared
for user in userA userB
do
	if theuid=`id -u "$user"`
	then
		mkdir "$rootdir/$theuid"
		chown "$user" "$rootdir/$theuid"
	fi
done
```


After this, you can mount the unshared file system:

```
unsharedfs -o allow_other /unshared /my-directory
```

Note that the unshared file system needs to be mounted by the root user in
order to be effective (even when *user_allow_other* has been set in fuse.conf).
Otherwise, the FUSE file system does not know the UID of the user that is
accessing the mountpoint, and all access will be redirected to the
UID-directory of the user that *mounted* the file system.


Installation
------------

1. make
2. make install PREFIX=/some/prefix

If you want to update the manpage, you need the [help2man](http://www.gnu.org/software/help2man/) utiltiy.
The manpage is created (semi-)automatically from the usage information of the
unsharedfs binary and additional text in the file doc/unsharedfs.h2m.
If you make any changes in either one, update the manpage by running:
```
make update-man
```
