How to create a new release
===========================

Prerequisites
-------------

 - Build requisites for unsharedfs
 - help2man (for updating the manpage


Steps
-----

 1. Adopt version string in src/unsharedfs.c
    E.g. "1.1git" becomes "1.1" in:
    ```C++
#define UNSHAREDFS_VERSION_STRING "unsharedfs 1.1git"
	```

 2. Update docs:
    ```
	make update-man
	```

 3. Update changelog in doc/changelog.txt
 
 4. Commit and tag
    ```
	#git add ...
	git commit
	git tag unsharedfs-1.1
	```
  
 5. Update version string for next release:
    E.g. "1.1" becomes "1.2git" in src/unsharedfs.c:
    ```C++
#define UNSHAREDFS_VERSION_STRING "unsharedfs 1.1"
	```


 
