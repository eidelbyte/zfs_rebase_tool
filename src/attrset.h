/*
 * attrset: set attributes on an object by path.
 *
 * The routines the apply stamps an object with, factored out so the
 * picker can use the same code. Every call takes a full path and
 * never follows a symbolic link at the last component. The platform
 * section is the same code that was in apply.c; nothing is
 * duplicated.
 */
#ifndef	ZR_ATTRSET_H
#define	ZR_ATTRSET_H

#include "walk.h"	/* struct zr_attr, zr_acl_t */

/*
 * Set the extended attributes of the object at path to match at:
 * every name in at is written, and every name on the object that at
 * does not hold is removed. Returns 0 or -1 with errno set.
 */
int zr_setxattrs(const char *path, const struct zr_attr *at);

/*
 * Set the ACL (and on a FreeBSD POSIX.1e directory the default ACL)
 * to match at. An absent ACL in at strips any non-trivial one where
 * that makes sense. Returns 0 or -1 with errno set.
 */
int zr_setacl(const char *path, const struct zr_attr *at, int isdir);

/*
 * Set the file flags. On a platform with no st_flags this is a
 * no-op that succeeds. Returns 0 or -1 with errno set.
 */
int zr_setflags(const char *path, uint32_t flags);

/*
 * Set the permission bits. A symbolic link whose platform refuses
 * the call is not an error: the mode of a link means nothing on a
 * filesystem that does not keep one.
 */
int zr_chmod(const char *path, mode_t mode, int islink);

#endif	/* ZR_ATTRSET_H */
