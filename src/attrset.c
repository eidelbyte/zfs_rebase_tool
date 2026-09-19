/*
 * attrset: set attributes on an object by path.
 *
 * This is the platform section that was in apply.c, now shared by the
 * apply and the picker so that both stamp an object identically. The
 * callers differ: the apply works relative to a root descriptor, and
 * the picker works over a full path; what this file offers is the
 * path-based half, and the apply's own wrapper translates.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif
#ifdef __linux__
#define	_GNU_SOURCE
#endif

#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "attrset.h"

#define	ZR_PERM		07777		/* the mode bits chmod sets */

#if defined(__FreeBSD__) || defined(__APPLE__)
#define	ZR_HAVE_ST_FLAGS	1
#endif
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__linux__)
#define	ZR_HAVE_XATTRS		1
#endif

#ifdef ZR_HAVE_XATTRS

static int
zr_xa_absent(int e)
{
	if (e == ENOTSUP || e == EOPNOTSUPP || e == EPERM || e == EACCES)
		return (1);
#ifdef ENOATTR
	if (e == ENOATTR)
		return (1);
#endif
#ifdef ENODATA
	if (e == ENODATA)
		return (1);
#endif
	return (0);
}

static int
zr_xa_has(const struct zr_attr *at, const char *name, size_t len)
{
	uint32_t i;

	for (i = 0; i < at->za_nxattrs; i++) {
		if (strlen(at->za_xattrs[i].zx_name) == len &&
		    memcmp(at->za_xattrs[i].zx_name, name, len) == 0)
			return (1);
	}
	return (0);
}

#endif	/* ZR_HAVE_XATTRS */

#if defined(__FreeBSD__)

#include <sys/param.h>

#include <sys/acl.h>
#include <sys/extattr.h>

#define	ZR_FBSD_NAME	264

static void
zr_ns_split(const char *name, int *ns, const char **bare)
{
	if (strncmp(name, "user.", 5) == 0) {
		*ns = EXTATTR_NAMESPACE_USER;
		*bare = name + 5;
		return;
	}
	if (strncmp(name, "system.", 7) == 0) {
		*ns = EXTATTR_NAMESPACE_SYSTEM;
		*bare = name + 7;
		return;
	}
	*ns = EXTATTR_NAMESPACE_USER;
	*bare = name;
}

static int
zr_prune_ns(const char *full, int ns, const char *prefix,
    const struct zr_attr *at)
{
	char qname[ZR_FBSD_NAME];
	char *list;
	ssize_t want, n;
	size_t i, plen, len;
	int rc;

	want = extattr_list_link(full, ns, NULL, 0);
	if (want < 0)
		return (zr_xa_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = extattr_list_link(full, ns, list, (size_t)want);
	if (n < 0) {
		free(list);
		return (zr_xa_absent(errno) ? 0 : -1);
	}
	plen = strlen(prefix);
	memcpy(qname, prefix, plen);
	rc = 0;
	for (i = 0; rc == 0 && i < (size_t)n; i += len) {
		len = (size_t)(unsigned char)list[i];
		i++;
		if (i + len > (size_t)n || plen + len + 1 > sizeof (qname)) {
			errno = EINVAL;
			rc = -1;
			break;
		}
		memcpy(qname + plen, list + i, len);
		qname[plen + len] = '\0';
		if (zr_xa_has(at, qname, plen + len))
			continue;
		if (extattr_delete_link(full, ns, qname + plen) != 0 &&
		    !zr_xa_absent(errno))
			rc = -1;
	}
	free(list);
	return (rc);
}

int
zr_setxattrs(const char *path, const struct zr_attr *at)
{
	const char *bare;
	uint32_t i;
	int ns;

	for (i = 0; i < at->za_nxattrs; i++) {
		zr_ns_split(at->za_xattrs[i].zx_name, &ns, &bare);
		if (extattr_set_link(path, ns, bare,
		    at->za_xattrs[i].zx_value,
		    at->za_xattrs[i].zx_len) < 0)
			return (-1);
	}
	if (zr_prune_ns(path, EXTATTR_NAMESPACE_USER, "user.", at) != 0)
		return (-1);
	return (zr_prune_ns(path, EXTATTR_NAMESPACE_SYSTEM, "system.", at));
}

static int
zr_acl_flavor(const char *full, acl_type_t *typep)
{
	if (lpathconf(full, _PC_ACL_NFS4) > 0) {
		*typep = ACL_TYPE_NFS4;
		return (1);
	}
	if (lpathconf(full, _PC_ACL_EXTENDED) > 0) {
		*typep = ACL_TYPE_ACCESS;
		return (1);
	}
	return (0);
}

static int
zr_acl_strip(const char *full)
{
	acl_t a, s;
	int rc;

	a = acl_get_link_np(full, ACL_TYPE_NFS4);
	if (a == NULL)
		return (zr_xa_absent(errno) || errno == EINVAL ? 0 : -1);
	s = acl_strip_np(a, 0);
	(void) acl_free(a);
	if (s == NULL)
		return (-1);
	rc = acl_set_link_np(full, ACL_TYPE_NFS4, s);
	(void) acl_free(s);
	return (rc);
}

int
zr_setacl(const char *path, const struct zr_attr *at, int isdir)
{
	acl_type_t type;

	if (zr_acl_flavor(path, &type) == 0)
		return (0);
	if (at->za_acl == NULL) {
		if (type == ACL_TYPE_NFS4)
			return (zr_acl_strip(path));
		if (isdir && acl_delete_def_link_np(path) != 0 &&
		    !zr_xa_absent(errno) && errno != EINVAL)
			return (-1);
		return (0);
	}
	if (acl_set_link_np(path, type, at->za_acl) != 0)
		return (-1);
	if (type != ACL_TYPE_ACCESS || !isdir)
		return (0);
	if (at->za_dacl == NULL) {
		if (acl_delete_def_link_np(path) != 0 &&
		    !zr_xa_absent(errno) && errno != EINVAL)
			return (-1);
		return (0);
	}
	return (acl_set_link_np(path, ACL_TYPE_DEFAULT, at->za_dacl));
}

int
zr_setflags(const char *path, uint32_t flags)
{
	return (lchflags(path, flags));
}

#elif defined(__APPLE__)

#include <sys/acl.h>
#include <sys/xattr.h>

int
zr_setxattrs(const char *path, const struct zr_attr *at)
{
	char *list;
	ssize_t want, n;
	size_t i, len;
	uint32_t k;
	int rc;

	for (k = 0; k < at->za_nxattrs; k++) {
		if (setxattr(path, at->za_xattrs[k].zx_name,
		    at->za_xattrs[k].zx_value, at->za_xattrs[k].zx_len, 0,
		    XATTR_NOFOLLOW) != 0)
			return (-1);
	}
	want = listxattr(path, NULL, 0, XATTR_NOFOLLOW);
	if (want < 0)
		return (zr_xa_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = listxattr(path, list, (size_t)want, XATTR_NOFOLLOW);
	if (n < 0) {
		free(list);
		return (zr_xa_absent(errno) ? 0 : -1);
	}
	rc = 0;
	for (i = 0; rc == 0 && i < (size_t)n; i += len + 1) {
		len = strlen(list + i);
		if (len == 0 || zr_xa_has(at, list + i, len))
			continue;
		if (removexattr(path, list + i, XATTR_NOFOLLOW) != 0 &&
		    !zr_xa_absent(errno))
			rc = -1;
	}
	free(list);
	return (rc);
}

int
zr_setacl(const char *path, const struct zr_attr *at, int isdir)
{
	acl_t a;
	int rc;

	(void) isdir;
	if (at->za_acl == NULL)
		return (0);
	a = acl_from_text(at->za_acl);
	if (a == NULL)
		return (-1);
	rc = acl_set_link_np(path, ACL_TYPE_EXTENDED, a);
	(void) acl_free(a);
	return (rc);
}

int
zr_setflags(const char *path, uint32_t flags)
{
	return (lchflags(path, flags));
}

#elif defined(__linux__)

#include <sys/xattr.h>

int
zr_setxattrs(const char *path, const struct zr_attr *at)
{
	char *list;
	ssize_t want, n;
	size_t i, len;
	uint32_t k;
	int rc;

	for (k = 0; k < at->za_nxattrs; k++) {
		if (lsetxattr(path, at->za_xattrs[k].zx_name,
		    at->za_xattrs[k].zx_value, at->za_xattrs[k].zx_len,
		    0) != 0)
			return (-1);
	}
	want = llistxattr(path, NULL, 0);
	if (want < 0)
		return (zr_xa_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = llistxattr(path, list, (size_t)want);
	if (n < 0) {
		free(list);
		return (zr_xa_absent(errno) ? 0 : -1);
	}
	rc = 0;
	for (i = 0; rc == 0 && i < (size_t)n; i += len + 1) {
		len = strlen(list + i);
		if (len == 0 || zr_xa_has(at, list + i, len))
			continue;
		if (lremovexattr(path, list + i) != 0 && !zr_xa_absent(errno))
			rc = -1;
	}
	free(list);
	return (rc);
}

int
zr_setacl(const char *path, const struct zr_attr *at, int isdir)
{
	(void) path;
	(void) at;
	(void) isdir;
	return (0);
}

int
zr_setflags(const char *path, uint32_t flags)
{
	(void) path;
	(void) flags;
	return (0);
}

#else

int
zr_setxattrs(const char *path, const struct zr_attr *at)
{
	(void) path;
	(void) at;
	return (0);
}

int
zr_setacl(const char *path, const struct zr_attr *at, int isdir)
{
	(void) path;
	(void) at;
	(void) isdir;
	return (0);
}

int
zr_setflags(const char *path, uint32_t flags)
{
	(void) path;
	(void) flags;
	return (0);
}

#endif	/* platform section ends */

int
zr_chmod(const char *path, mode_t mode, int islink)
{
	if (fchmodat(AT_FDCWD, path, mode & ZR_PERM,
	    AT_SYMLINK_NOFOLLOW) == 0)
		return (0);
	if (islink && (errno == ENOTSUP || errno == EOPNOTSUPP ||
	    errno == EINVAL))
		return (0);
	return (-1);
}
