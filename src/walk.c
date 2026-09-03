/*
 * walk: read one tree into the shared name table. An explicit stack
 * of open directories descends it without recursion, every entry is
 * lstat'd and never followed, each name joins its pool by inode
 * number, and the first name to reach a pool records the attributes
 * the content oracle will later compare. A directory's entries are
 * read in one pass and sorted before any of them is looked at, so
 * that the order of the walk -- and with it the name ids it hands
 * out -- is the same on every filesystem, not readdir's. Extended
 * attributes and the ACL are the only calls with no portable form;
 * they live in the one platform section below.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
/*
 * lpathconf and the _PC_ACL_ names sit behind __BSD_VISIBLE, which
 * _XOPEN_SOURCE alone switches off. Defining both is how FreeBSD's
 * sys/_visible.h expects a program to ask for POSIX 2008 and
 * the BSD extensions together.
 */
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "name.h"
#include "walk.h"

#define	ZW_STACK_MIN	16
#define	ZW_LINK_MIN	64
#define	ZW_XATTR_TRIES	4
#define	ZW_ENTS_MIN	32	/* entries in one directory's index */
#define	ZW_LEAVES_MIN	512	/* bytes in one directory's name buffer */

/*
 * dev_t is a signed 32-bit integer on some platforms, where a device
 * number with its top bit set would sign-extend into the top half of
 * a uint64_t. Mask to the width of the type instead. The shift is
 * split in two so that a 64-bit dev_t does not shift by 64.
 */
#define	ZW_DEVMASK	(((((uint64_t)1 << (sizeof (dev_t) * 8 - 1))) << 1) - 1)

/*
 * ---------------------------------------------------------------
 * The platform section. Everything outside it is plain POSIX; here
 * are the extended attributes and the ACL, which POSIX never
 * standardised. FreeBSD is the target -- there the tool reads ZFS
 * snapshots, and that section is written first and in full. macOS
 * is the development stand-in, where this core is built and tested
 * without ZFS; Linux is a courtesy. Each platform supplies the same
 * two functions:
 *
 *	zw_xattrs()	fill at->za_xattrs, sorted by name, bytewise
 *	zw_acl()	fill at->za_acl, and at->za_dacl where the
 *			platform has a default ACL
 *
 * Both return 0 on success and -1 with errno set on failure. An
 * entry with no attributes and no ACL is a success, not a failure.
 * ZW_HAVE_ST_FLAGS says the platform's struct stat has st_flags.
 * ---------------------------------------------------------------
 */

#if defined(__FreeBSD__) || defined(__APPLE__)
#define	ZW_HAVE_ST_FLAGS	1	/* struct stat carries st_flags */
#define	ZW_HAVE_ACL		1	/* an ACL this can render as text */
#endif
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__linux__)
#define	ZW_HAVE_XATTRS		1
#endif

#ifdef ZW_HAVE_XATTRS

/*
 * The errnos that mean "there is nothing to read here" rather than
 * "the walk failed": a filesystem with no attribute support, a
 * namespace this process may not read -- the system namespace is
 * root's on FreeBSD -- and the several spellings of "no such
 * attribute".
 */
static int
zw_absent(int e)
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

/*
 * Append one attribute, taking the value buffer over. On failure the
 * value still belongs to the caller, who frees it.
 */
static int
zw_xattr_add(struct zr_attr *at, const char *name, size_t namelen,
    unsigned char *val, size_t len)
{
	struct zr_xattr *tab;
	char *nm;

	nm = malloc(namelen + 1);
	if (nm == NULL)
		return (-1);
	tab = realloc(at->za_xattrs, (size_t)(at->za_nxattrs + 1) *
	    sizeof (struct zr_xattr));
	if (tab == NULL) {
		free(nm);
		return (-1);
	}
	at->za_xattrs = tab;
	memcpy(nm, name, namelen);
	nm[namelen] = '\0';
	tab[at->za_nxattrs].zx_name = nm;
	tab[at->za_nxattrs].zx_value = val;
	tab[at->za_nxattrs].zx_len = len;
	at->za_nxattrs++;
	return (0);
}

static int
zw_xattr_cmp(const void *a, const void *b)
{
	const struct zr_xattr *x, *y;

	x = (const struct zr_xattr *)a;
	y = (const struct zr_xattr *)b;
	return (strcmp(x->zx_name, y->zx_name));
}

/* strcmp compares as unsigned char, which is the bytewise order. */
static void
zw_xattr_sort(struct zr_attr *at)
{
	if (at->za_nxattrs > 1) {
		qsort(at->za_xattrs, at->za_nxattrs,
		    sizeof (struct zr_xattr), zw_xattr_cmp);
	}
}

#endif	/* ZW_HAVE_XATTRS */

#ifdef ZW_HAVE_ACL

/* Keep an ACL text of the platform's making in memory of ours. */
static char *
zw_dup_text(const char *s)
{
	size_t len;
	char *p;

	len = strlen(s);
	p = malloc(len + 1);
	if (p == NULL)
		return (NULL);
	memcpy(p, s, len + 1);
	return (p);
}

#endif	/* ZW_HAVE_ACL */

#if defined(__FreeBSD__)

#include <sys/acl.h>
#include <sys/extattr.h>

/* "system." plus an attribute name of at most 255 bytes, plus a NUL */
#define	ZW_FBSD_NAME	264

static int
zw_xattr_value(const char *full, int ns, const char *name,
    unsigned char **valp, size_t *lenp)
{
	unsigned char *buf;
	ssize_t want, n;
	int i;

	for (i = 0; i < ZW_XATTR_TRIES; i++) {
		want = extattr_get_link(full, ns, name, NULL, 0);
		if (want < 0)
			return (-1);
		buf = malloc((size_t)want + 1);
		if (buf == NULL)
			return (-1);
		n = extattr_get_link(full, ns, name, buf, (size_t)want);
		if (n >= 0) {
			*valp = buf;
			*lenp = (size_t)n;
			return (0);
		}
		free(buf);
		if (errno != ERANGE)
			return (-1);
	}
	errno = ERANGE;
	return (-1);
}

/*
 * One namespace of one entry, never following the link.
 * extattr_list_link returns the names as a run of (one length byte,
 * that many bytes) pairs, none of them terminated -- not the
 * NUL-separated list the other two platforms return. Each name is
 * stored under its namespace prefix so that the two namespaces
 * cannot collide in za_xattrs. A namespace this process may not read
 * -- the system one, for anybody but root -- is no attributes, not a
 * failed walk, which is what zw_absent above decides.
 */
static int
zw_xattr_ns(const char *full, int ns, const char *prefix, struct zr_attr *at)
{
	char qname[ZW_FBSD_NAME];
	unsigned char *val;
	char *list;
	ssize_t want, n;
	size_t i, plen, len, vlen;
	int rc;

	want = extattr_list_link(full, ns, NULL, 0);
	if (want < 0)
		return (zw_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = extattr_list_link(full, ns, list, (size_t)want);
	if (n < 0) {
		free(list);
		return (zw_absent(errno) ? 0 : -1);
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
		if (zw_xattr_value(full, ns, qname + plen, &val, &vlen) != 0) {
			if (zw_absent(errno))
				continue;
			rc = -1;
			break;
		}
		if (zw_xattr_add(at, qname, plen + len, val, vlen) != 0) {
			free(val);
			rc = -1;
		}
	}
	free(list);
	return (rc);
}

static int
zw_xattrs(int dfd, const char *leaf, const char *full,
    const struct stat *st, struct zr_attr *at)
{
	(void) dfd;
	(void) leaf;
	(void) st;
	if (zw_xattr_ns(full, EXTATTR_NAMESPACE_USER, "user.", at) != 0)
		return (-1);
	if (zw_xattr_ns(full, EXTATTR_NAMESPACE_SYSTEM, "system.", at) != 0)
		return (-1);
	zw_xattr_sort(at);
	return (0);
}

/*
 * Which flavor of ACL this path carries: ZFS has NFSv4 ACLs, UFS has
 * POSIX.1e, and a filesystem with neither answers no to both.
 * lpathconf asks of the link itself.
 */
static int
zw_acl_flavor(const char *full, acl_type_t *typep)
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

/*
 * One ACL as text. Numeric ids with the name appended keep the text
 * the same on a host that cannot resolve the id, which is what makes
 * two snapshots comparable. A trivial ACL -- one the mode bits
 * already say in full -- is stored as nothing, since the mode is
 * captured anyway and every ordinary file would otherwise carry one.
 */
static int
zw_acl_text(const char *full, acl_type_t type, int skiptrivial, char **outp)
{
	acl_t a;
	char *txt;
	int trivial;

	*outp = NULL;
	a = acl_get_link_np(full, type);
	if (a == NULL) {
		if (zw_absent(errno) || errno == EINVAL)
			return (0);
		return (-1);
	}
	trivial = 0;
	if (skiptrivial && acl_is_trivial_np(a, &trivial) != 0)
		trivial = 0;
	if (trivial) {
		(void) acl_free(a);
		return (0);
	}
	txt = acl_to_text_np(a, NULL, ACL_TEXT_NUMERIC_IDS |
	    ACL_TEXT_APPEND_ID);
	if (txt != NULL) {
		*outp = zw_dup_text(txt);
		(void) acl_free(txt);
	}
	(void) acl_free(a);
	if (txt != NULL && *outp == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	return (0);
}

/*
 * A POSIX.1e directory has two ACLs: the one that governs it and the
 * one its new children inherit. Both are kept, the default in
 * za_dacl. An NFSv4 ACL has one, inheritance being written into its
 * entries.
 */
static int
zw_acl(const char *full, const struct stat *st, struct zr_attr *at)
{
	acl_type_t type;

	at->za_acl = NULL;
	at->za_dacl = NULL;
	if (zw_acl_flavor(full, &type) == 0)
		return (0);
	if (type == ACL_TYPE_NFS4)
		return (zw_acl_text(full, ACL_TYPE_NFS4, 1, &at->za_acl));
	if (zw_acl_text(full, ACL_TYPE_ACCESS, 0, &at->za_acl) != 0)
		return (-1);
	if (!S_ISDIR(st->st_mode))
		return (0);
	return (zw_acl_text(full, ACL_TYPE_DEFAULT, 0, &at->za_dacl));
}

#elif defined(__APPLE__)

#include <sys/acl.h>
#include <sys/xattr.h>

/*
 * macOS has no *at() form of getxattr, so an entry that can be
 * opened is read through its descriptor and everything else through
 * the path with XATTR_NOFOLLOW.
 */
static int
zw_xattr_value(int fd, const char *full, const char *name,
    unsigned char **valp, size_t *lenp)
{
	unsigned char *buf;
	ssize_t want, n;
	int i;

	for (i = 0; i < ZW_XATTR_TRIES; i++) {
		if (fd >= 0)
			want = fgetxattr(fd, name, NULL, 0, 0, 0);
		else
			want = getxattr(full, name, NULL, 0, 0,
			    XATTR_NOFOLLOW);
		if (want < 0)
			return (-1);
		buf = malloc((size_t)want + 1);
		if (buf == NULL)
			return (-1);
		if (fd >= 0)
			n = fgetxattr(fd, name, buf, (size_t)want, 0, 0);
		else
			n = getxattr(full, name, buf, (size_t)want, 0,
			    XATTR_NOFOLLOW);
		if (n >= 0) {
			*valp = buf;
			*lenp = (size_t)n;
			return (0);
		}
		free(buf);
		if (errno != ERANGE)
			return (-1);
	}
	errno = ERANGE;
	return (-1);
}

static int
zw_xattrs(int dfd, const char *leaf, const char *full,
    const struct stat *st, struct zr_attr *at)
{
	unsigned char *val;
	char *list;
	ssize_t want, n;
	size_t i, len, vlen;
	int fd, rc;

	fd = -1;
	if (S_ISREG(st->st_mode) || S_ISDIR(st->st_mode))
		fd = openat(dfd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0)
		want = flistxattr(fd, NULL, 0, 0);
	else
		want = listxattr(full, NULL, 0, XATTR_NOFOLLOW);
	if (want < 0) {
		rc = zw_absent(errno) ? 0 : -1;
		goto out;
	}
	list = malloc((size_t)want + 1);
	if (list == NULL) {
		rc = -1;
		goto out;
	}
	if (fd >= 0)
		n = flistxattr(fd, list, (size_t)want, 0);
	else
		n = listxattr(full, list, (size_t)want, XATTR_NOFOLLOW);
	if (n < 0) {
		free(list);
		rc = zw_absent(errno) ? 0 : -1;
		goto out;
	}
	rc = 0;
	/* the list is the names, each one NUL-terminated */
	for (i = 0; rc == 0 && i < (size_t)n; i += len + 1) {
		len = strlen(list + i);
		if (len == 0)
			continue;
		if (zw_xattr_value(fd, full, list + i, &val, &vlen) != 0) {
			if (zw_absent(errno))
				continue;
			rc = -1;
			break;
		}
		if (zw_xattr_add(at, list + i, len, val, vlen) != 0) {
			free(val);
			rc = -1;
		}
	}
	free(list);
out:
	if (fd >= 0)
		(void) close(fd);
	if (rc == 0)
		zw_xattr_sort(at);
	return (rc);
}

/*
 * The stand-in: one extended ACL, no flavors to tell apart and no
 * default ACL to keep. acl_to_text is the only text form there is.
 */
static int
zw_acl(const char *full, const struct stat *st, struct zr_attr *at)
{
	acl_t a;
	char *txt;

	(void) st;
	at->za_acl = NULL;
	at->za_dacl = NULL;
	a = acl_get_link_np(full, ACL_TYPE_EXTENDED);
	if (a == NULL)
		return (0);
	txt = acl_to_text(a, NULL);
	if (txt != NULL) {
		at->za_acl = zw_dup_text(txt);
		(void) acl_free(txt);
	}
	(void) acl_free(a);
	if (txt != NULL && at->za_acl == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	return (0);
}

#elif defined(__linux__)

#include <sys/xattr.h>

static int
zw_xattr_value(const char *full, const char *name, unsigned char **valp,
    size_t *lenp)
{
	unsigned char *buf;
	ssize_t want, n;
	int i;

	for (i = 0; i < ZW_XATTR_TRIES; i++) {
		want = lgetxattr(full, name, NULL, 0);
		if (want < 0)
			return (-1);
		buf = malloc((size_t)want + 1);
		if (buf == NULL)
			return (-1);
		n = lgetxattr(full, name, buf, (size_t)want);
		if (n >= 0) {
			*valp = buf;
			*lenp = (size_t)n;
			return (0);
		}
		free(buf);
		if (errno != ERANGE)
			return (-1);
	}
	errno = ERANGE;
	return (-1);
}

static int
zw_xattrs(int dfd, const char *leaf, const char *full,
    const struct stat *st, struct zr_attr *at)
{
	unsigned char *val;
	char *list;
	ssize_t want, n;
	size_t i, len, vlen;
	int rc;

	(void) dfd;
	(void) leaf;
	(void) st;
	want = llistxattr(full, NULL, 0);
	if (want < 0)
		return (zw_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = llistxattr(full, list, (size_t)want);
	if (n < 0) {
		free(list);
		return (zw_absent(errno) ? 0 : -1);
	}
	rc = 0;
	/* the list is the names, each one NUL-terminated */
	for (i = 0; rc == 0 && i < (size_t)n; i += len + 1) {
		len = strlen(list + i);
		if (len == 0)
			continue;
		if (zw_xattr_value(full, list + i, &val, &vlen) != 0) {
			if (zw_absent(errno))
				continue;
			rc = -1;
			break;
		}
		if (zw_xattr_add(at, list + i, len, val, vlen) != 0) {
			free(val);
			rc = -1;
		}
	}
	free(list);
	if (rc == 0)
		zw_xattr_sort(at);
	return (rc);
}

/* A POSIX.1e ACL on Linux is already one of the xattrs above. */
static int
zw_acl(const char *full, const struct stat *st, struct zr_attr *at)
{
	(void) full;
	(void) st;
	at->za_acl = NULL;
	at->za_dacl = NULL;
	return (0);
}

#else

/* An unknown platform still walks; it just reads no attributes. */
static int
zw_xattrs(int dfd, const char *leaf, const char *full,
    const struct stat *st, struct zr_attr *at)
{
	(void) dfd;
	(void) leaf;
	(void) full;
	(void) st;
	(void) at;
	return (0);
}

static int
zw_acl(const char *full, const struct stat *st, struct zr_attr *at)
{
	(void) full;
	(void) st;
	at->za_acl = NULL;
	at->za_dacl = NULL;
	return (0);
}

#endif	/* platform section ends */

/*
 * One open directory on the descent stack: the name it has, and its
 * entries, read in one pass and sorted before the first is used.
 * zf_leaves holds the leaf names end to end, each NUL-terminated;
 * zf_ents holds their offsets, and the sort moves offsets, never
 * bytes. zf_next is how far the descent has got through them. The
 * directory stays open for its descriptor, which is what every entry
 * of it is reached through; both arrays are freed when it is popped.
 */
struct zw_frame {
	DIR		*zf_dir;
	zr_name_t	zf_name;
	char		*zf_leaves;
	size_t		zf_len;
	size_t		zf_cap;
	size_t		*zf_ents;
	uint32_t	zf_nents;
	uint32_t	zf_entcap;
	uint32_t	zf_next;
};

/*
 * The walk's own state. zc_full is scratch: the root's path with one
 * interned name appended, which is what the platform calls above and
 * the error messages need.
 */
struct zw_ctx {
	struct zr_walk	*zc_w;
	struct zr_names	*zc_ns;
	struct zw_frame	*zc_stack;
	uint32_t	zc_depth;
	uint32_t	zc_cap;
	char		*zc_root;
	size_t		zc_rootlen;
	char		*zc_full;
	size_t		zc_fullcap;
	char		*zc_err;
	size_t		zc_errlen;
};

static int
zw_fail(struct zw_ctx *c, const char *path, const char *what)
{
	if (c->zc_err != NULL && c->zc_errlen > 0) {
		(void) snprintf(c->zc_err, c->zc_errlen, "%s: %s: %s", path,
		    what, strerror(errno));
	}
	return (-1);
}

static int
zw_failx(struct zw_ctx *c, const char *path, const char *what)
{
	if (c->zc_err != NULL && c->zc_errlen > 0)
		(void) snprintf(c->zc_err, c->zc_errlen, "%s: %s", path, what);
	return (-1);
}

/*
 * The root's own path, with any trailing slashes cut, so that the
 * platform calls -- which take a path, never a descriptor and a name
 * -- can be handed the root followed by an interned name.
 */
static int
zw_root_copy(struct zw_ctx *c, const char *root)
{
	size_t len;

	len = strlen(root);
	while (len > 0 && root[len - 1] == '/')
		len--;
	c->zc_root = malloc(len + 1);
	if (c->zc_root == NULL)
		return (-1);
	memcpy(c->zc_root, root, len);
	c->zc_root[len] = '\0';
	c->zc_rootlen = len;
	return (0);
}

/*
 * The root's path with the name appended, in zc_full. The pointer is
 * good until the next call. The root's own name, "/", adds nothing
 * unless the root is the filesystem root, where it is the whole path.
 */
static const char *
zw_full(struct zw_ctx *c, zr_name_t nm)
{
	const char *p;
	char *buf;
	size_t len, need;

	p = zr_names_str(c->zc_ns, nm, &len);
	if (p == NULL)
		return (NULL);
	if (len == 1 && c->zc_rootlen != 0)
		len = 0;
	need = c->zc_rootlen + len + 1;
	if (need > c->zc_fullcap) {
		buf = realloc(c->zc_full, need);
		if (buf == NULL)
			return (NULL);
		c->zc_full = buf;
		c->zc_fullcap = need;
	}
	memcpy(c->zc_full, c->zc_root, c->zc_rootlen);
	memcpy(c->zc_full + c->zc_rootlen, p, len);
	c->zc_full[c->zc_rootlen + len] = '\0';
	return (c->zc_full);
}

/* The same, but with something printable when memory ran out. */
static const char *
zw_where(struct zw_ctx *c, zr_name_t nm)
{
	const char *p;

	p = zw_full(c, nm);
	return (p != NULL ? p : "(a name of the tree)");
}

/*
 * One leaf appended to the frame: its bytes to the name buffer, its
 * offset to the index. Nothing is compared here; the sort comes once
 * the whole directory has been read.
 */
static int
zw_add(struct zw_frame *fr, const char *leaf, size_t leaflen)
{
	char *nb;
	size_t *ne;
	size_t cap, need;
	uint32_t ecap;

	need = fr->zf_len + leaflen + 1;
	if (need > fr->zf_cap) {
		cap = fr->zf_cap != 0 ? fr->zf_cap : ZW_LEAVES_MIN;
		while (cap < need) {
			if (cap > (size_t)-1 / 2) {
				errno = ENOMEM;
				return (-1);
			}
			cap *= 2;
		}
		nb = realloc(fr->zf_leaves, cap);
		if (nb == NULL)
			return (-1);
		fr->zf_leaves = nb;
		fr->zf_cap = cap;
	}
	if (fr->zf_nents == fr->zf_entcap) {
		if (fr->zf_entcap > (uint32_t)-1 / 2) {
			errno = ENOMEM;
			return (-1);
		}
		ecap = fr->zf_entcap != 0 ? fr->zf_entcap * 2 : ZW_ENTS_MIN;
		ne = realloc(fr->zf_ents, (size_t)ecap * sizeof (size_t));
		if (ne == NULL)
			return (-1);
		fr->zf_ents = ne;
		fr->zf_entcap = ecap;
	}
	fr->zf_ents[fr->zf_nents] = fr->zf_len;
	fr->zf_nents++;
	memcpy(fr->zf_leaves + fr->zf_len, leaf, leaflen);
	fr->zf_leaves[fr->zf_len + leaflen] = '\0';
	fr->zf_len = need;
	return (0);
}

/*
 * Two leaves of one directory, as bytes: memcmp over the common
 * prefix, and the shorter first when one is a prefix of the other.
 * This is the order the manifest writes a directory's children in,
 * which is why the walk uses it too. Comparing bytes and not
 * characters keeps a name with a byte above 0x7f in the same place
 * whatever the sign of a char.
 */
static int
zw_leafcmp(const char *leaves, size_t a, size_t b)
{
	const char *x = leaves + a, *y = leaves + b;
	size_t alen, blen, n;
	int r;

	alen = strlen(x);
	blen = strlen(y);
	n = alen < blen ? alen : blen;
	r = n != 0 ? memcmp(x, y, n) : 0;
	if (r != 0)
		return (r);
	if (alen == blen)
		return (0);
	return (alen < blen ? -1 : 1);
}

/* One sift down of the heap the sort below builds. */
static void
zw_sift(const char *leaves, size_t *ents, uint32_t top, uint32_t n)
{
	uint32_t kid;
	size_t t;

	for (;;) {
		kid = top * 2 + 1;
		if (kid >= n)
			return;
		if (kid + 1 < n &&
		    zw_leafcmp(leaves, ents[kid], ents[kid + 1]) < 0)
			kid++;
		if (zw_leafcmp(leaves, ents[top], ents[kid]) >= 0)
			return;
		t = ents[top];
		ents[top] = ents[kid];
		ents[kid] = t;
		top = kid;
	}
}

/*
 * The index sorted into name order. A heap sort: in place, iterative
 * and O(n log n) whatever the directory holds, and it needs no
 * comparison context, which qsort has no portable way to pass.
 */
static void
zw_sort(const char *leaves, size_t *ents, uint32_t n)
{
	uint32_t i;
	size_t t;

	if (n < 2)
		return;
	for (i = n / 2; i > 0; i--)
		zw_sift(leaves, ents, i - 1, n);
	for (i = n; i > 1; i--) {
		t = ents[0];
		ents[0] = ents[i - 1];
		ents[i - 1] = t;
		zw_sift(leaves, ents, 0, i - 1);
	}
}

/*
 * The whole of one directory read into the frame, then sorted. "."
 * and ".." never enter it, and neither does a ".zfs" at the root:
 * that is ZFS's control directory, under which a snapshot mounts
 * itself the moment it is looked up, and it is never part of the
 * tree being rebased. The root is the first frame pushed, so a depth
 * of one is what makes this the root; deeper down, .zfs is an
 * ordinary name.
 */
static int
zw_read(struct zw_ctx *c, struct zw_frame *fr)
{
	struct dirent *de;
	const char *leaf;

	for (;;) {
		errno = 0;
		de = readdir(fr->zf_dir);
		if (de == NULL) {
			if (errno == 0)
				break;
			return (zw_fail(c, zw_where(c, fr->zf_name),
			    "readdir"));
		}
		leaf = de->d_name;
		if (leaf[0] == '.' && (leaf[1] == '\0' ||
		    (leaf[1] == '.' && leaf[2] == '\0')))
			continue;
		if (c->zc_depth == 1 && strcmp(leaf, ".zfs") == 0)
			continue;
		if (zw_add(fr, leaf, strlen(leaf)) != 0) {
			errno = ENOMEM;
			return (zw_fail(c, zw_where(c, fr->zf_name),
			    "entries"));
		}
	}
	zw_sort(fr->zf_leaves, fr->zf_ents, fr->zf_nents);
	return (0);
}

static void
zw_pop(struct zw_ctx *c)
{
	struct zw_frame *fr;

	c->zc_depth--;
	fr = &c->zc_stack[c->zc_depth];
	(void) closedir(fr->zf_dir);
	free(fr->zf_leaves);
	free(fr->zf_ents);
	memset(fr, 0, sizeof (struct zw_frame));
}

/*
 * Push one open directory and read it. The frame owns dp from here,
 * closing it whether this succeeds or not, so a caller that fails
 * has nothing left to close.
 */
static int
zw_push(struct zw_ctx *c, DIR *dp, zr_name_t nm)
{
	struct zw_frame *fr, *st;
	uint32_t cap;

	if (c->zc_depth == c->zc_cap) {
		cap = c->zc_cap != 0 ? c->zc_cap * 2 : ZW_STACK_MIN;
		st = realloc(c->zc_stack, (size_t)cap *
		    sizeof (struct zw_frame));
		if (st == NULL) {
			(void) closedir(dp);
			errno = ENOMEM;
			return (zw_fail(c, zw_where(c, nm), "stack"));
		}
		c->zc_stack = st;
		c->zc_cap = cap;
	}
	fr = &c->zc_stack[c->zc_depth];
	memset(fr, 0, sizeof (struct zw_frame));
	fr->zf_dir = dp;
	fr->zf_name = nm;
	c->zc_depth++;
	if (zw_read(c, fr) != 0) {
		zw_pop(c);
		return (-1);
	}
	return (0);
}

static void
zw_ctx_fini(struct zw_ctx *c)
{
	while (c->zc_depth > 0)
		zw_pop(c);
	free(c->zc_stack);
	free(c->zc_root);
	free(c->zc_full);
	memset(c, 0, sizeof (struct zw_ctx));
}

/*
 * A child's path is its parent's interned path, a slash and the
 * leaf, built once into a buffer of exactly that size and freed as
 * soon as the table has a copy. Nothing here walks back to the root.
 */
static zr_name_t
zw_child(struct zw_ctx *c, zr_name_t parent, const char *leaf, size_t leaflen)
{
	const char *pp;
	char *buf;
	size_t plen;
	zr_name_t nm;

	pp = zr_names_str(c->zc_ns, parent, &plen);
	if (pp == NULL)
		return (ZR_NAME_NONE);
	if (plen == 1)		/* the root: its slash is the separator */
		plen = 0;
	buf = malloc(plen + leaflen + 2);
	if (buf == NULL)
		return (ZR_NAME_NONE);
	memcpy(buf, pp, plen);
	buf[plen] = '/';
	memcpy(buf + plen + 1, leaf, leaflen);
	buf[plen + leaflen + 1] = '\0';
	nm = zr_names_intern(c->zc_ns, buf, plen + leaflen + 1);
	free(buf);
	return (nm);
}

static int
zw_type(mode_t m, zr_type_t *out)
{
	switch (m & S_IFMT) {
	case S_IFREG:
		*out = ZR_T_FILE;
		break;
	case S_IFDIR:
		*out = ZR_T_DIR;
		break;
	case S_IFLNK:
		*out = ZR_T_SYMLINK;
		break;
	case S_IFCHR:
		*out = ZR_T_CHR;
		break;
	case S_IFBLK:
		*out = ZR_T_BLK;
		break;
	case S_IFIFO:
		*out = ZR_T_FIFO;
		break;
	case S_IFSOCK:
		*out = ZR_T_SOCK;
		break;
	default:
		return (-1);
	}
	return (0);
}

/* st_size is a hint, not a promise: read until the target fits. */
static int
zw_readlink(int dfd, const char *leaf, const struct stat *st, char **outp)
{
	char *buf, *nb;
	size_t cap;
	ssize_t n;

	cap = ZW_LINK_MIN;
	if (st->st_size > 0)
		cap = (size_t)st->st_size + 1;
	buf = NULL;
	for (;;) {
		nb = realloc(buf, cap);
		if (nb == NULL) {
			free(buf);
			return (-1);
		}
		buf = nb;
		n = readlinkat(dfd, leaf, buf, cap);
		if (n < 0) {
			free(buf);
			return (-1);
		}
		if ((size_t)n < cap) {
			buf[n] = '\0';
			*outp = buf;
			return (0);
		}
		if (cap > (size_t)-1 / 2) {
			free(buf);
			errno = ENAMETOOLONG;
			return (-1);
		}
		cap *= 2;
	}
}

/*
 * Everything one pool holds, taken the first time a name reaches it.
 * The pool indices a walk sees rise one at a time, so the array is
 * grown to the index in hand.
 */
static int
zw_capture(struct zw_ctx *c, zr_pool_t pool, zr_name_t nm,
    const struct stat *st, int dfd, const char *leaf)
{
	struct zr_walk *w;
	struct zr_attr *at, *tab;
	const char *full;
	uint32_t i;

	w = c->zc_w;
	if (pool >= w->zw_nattrs) {
		tab = realloc(w->zw_attrs, (size_t)(pool + 1) *
		    sizeof (struct zr_attr));
		if (tab == NULL)
			return (-1);
		for (i = w->zw_nattrs; i <= pool; i++)
			memset(&tab[i], 0, sizeof (struct zr_attr));
		w->zw_attrs = tab;
		w->zw_nattrs = pool + 1;
	}
	at = &w->zw_attrs[pool];
	at->za_mode = st->st_mode;
	at->za_uid = st->st_uid;
	at->za_gid = st->st_gid;
#ifdef ZW_HAVE_ST_FLAGS
	at->za_flags = (uint32_t)st->st_flags;
#else
	at->za_flags = 0;
#endif
	at->za_size = (uint64_t)st->st_size;
	at->za_rdev = (uint64_t)st->st_rdev & ZW_DEVMASK;
	if (S_ISLNK(st->st_mode) &&
	    zw_readlink(dfd, leaf, st, &at->za_target) != 0)
		return (-1);
	full = zw_full(c, nm);
	if (full == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	if (zw_xattrs(dfd, leaf, full, st, at) != 0)
		return (-1);
	return (zw_acl(full, st, at));
}

/*
 * One entry: stat it, refuse another filesystem, give it a name and
 * a pool, and record the pool's attributes if this is its first
 * name. *dpp comes back non-NULL when the entry is a directory the
 * caller must descend.
 */
static int
zw_entry(struct zw_ctx *c, zr_name_t parent, int dfd, const char *leaf,
    zr_name_t *nmp, DIR **dpp)
{
	struct stat st;
	zr_type_t type;
	zr_pool_t pool;
	zr_name_t nm;
	uint32_t nlink;
	DIR *dp;
	int fd;

	*dpp = NULL;
	nm = zw_child(c, parent, leaf, strlen(leaf));
	if (nm == ZR_NAME_NONE) {
		errno = ENOMEM;
		return (zw_fail(c, zw_where(c, parent), "intern"));
	}
	*nmp = nm;
	if (fstatat(dfd, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (zw_fail(c, zw_where(c, nm), "lstat"));
	if (((uint64_t)st.st_dev & ZW_DEVMASK) != c->zc_w->zw_dev) {
		if (c->zc_err != NULL && c->zc_errlen > 0) {
			(void) snprintf(c->zc_err, c->zc_errlen,
			    "nested mount at %s", zw_where(c, nm));
		}
		return (-1);
	}
	if (zw_type(st.st_mode, &type) != 0)
		return (zw_failx(c, zw_where(c, nm), "not a file of any "
		    "type the walk knows"));
	/*
	 * A directory's link count counts its subdirectories, not its
	 * names: it has exactly one, so that is what the pool is told.
	 */
	nlink = type == ZR_T_DIR ? 1 : (uint32_t)st.st_nlink;
	pool = zr_tree_add(&c->zc_w->zw_tree, nm, (uint64_t)st.st_ino, type,
	    nlink);
	if (pool == ZR_POOL_NONE)
		return (zw_failx(c, zw_where(c, nm), "the tree refused it"));
	if (pool >= c->zc_w->zw_nattrs &&
	    zw_capture(c, pool, nm, &st, dfd, leaf) != 0)
		return (zw_fail(c, zw_where(c, nm), "attributes"));
	if (type != ZR_T_DIR)
		return (0);
	fd = openat(dfd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
	    O_CLOEXEC);
	if (fd < 0)
		return (zw_fail(c, zw_where(c, nm), "open"));
	dp = fdopendir(fd);
	if (dp == NULL) {
		(void) close(fd);
		return (zw_fail(c, zw_where(c, nm), "fdopendir"));
	}
	*dpp = dp;
	return (0);
}

/* The root itself: the name "/", a directory, and the tree's device. */
static int
zw_root(struct zw_ctx *c, const char *root, zr_name_t *nmp, DIR **dpp)
{
	struct zr_walk *w;
	struct stat st;
	zr_pool_t pool;
	zr_name_t nm;
	DIR *dp;
	int fd;

	w = c->zc_w;
	fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return (zw_fail(c, root, "open"));
	w->zw_rootfd = fd;
	if (fstat(fd, &st) != 0)
		return (zw_fail(c, root, "stat"));
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return (zw_fail(c, root, "root"));
	}
	w->zw_dev = (uint64_t)st.st_dev & ZW_DEVMASK;
	nm = zr_names_intern(c->zc_ns, "/", 1);
	if (nm == ZR_NAME_NONE) {
		errno = ENOMEM;
		return (zw_fail(c, root, "intern"));
	}
	pool = zr_tree_add(&w->zw_tree, nm, (uint64_t)st.st_ino, ZR_T_DIR, 1);
	if (pool == ZR_POOL_NONE)
		return (zw_failx(c, root, "the tree refused the root"));
	if (zw_capture(c, pool, nm, &st, fd, ".") != 0)
		return (zw_fail(c, root, "attributes"));
	fd = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return (zw_fail(c, root, "open"));
	dp = fdopendir(fd);
	if (dp == NULL) {
		(void) close(fd);
		return (zw_fail(c, root, "fdopendir"));
	}
	*dpp = dp;
	*nmp = nm;
	return (0);
}

int
zr_walk(const char *root, struct zr_names *names, struct zr_walk *out,
    char *err, size_t errlen)
{
	struct zw_ctx c;
	struct zw_frame *fr;
	const char *leaf;
	zr_name_t nm;
	DIR *dp;
	int dfd;

	if (out == NULL)
		return (-1);
	memset(out, 0, sizeof (struct zr_walk));
	out->zw_rootfd = -1;
	memset(&c, 0, sizeof (struct zw_ctx));
	c.zc_w = out;
	c.zc_ns = names;
	c.zc_err = err;
	c.zc_errlen = errlen;
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (root == NULL || names == NULL) {
		errno = EINVAL;
		return (zw_fail(&c, "(no root)", "walk"));
	}
	if (zr_tree_init(&out->zw_tree, names) != 0) {
		errno = ENOMEM;
		return (zw_fail(&c, root, "tree"));
	}
	if (zw_root_copy(&c, root) != 0) {
		errno = ENOMEM;
		(void) zw_fail(&c, root, "memory");
		goto fail;
	}
	if (zw_root(&c, root, &nm, &dp) != 0)
		goto fail;
	if (zw_push(&c, dp, nm) != 0)
		goto fail;
	/*
	 * The descent, in the order the entries were sorted into: a
	 * directory's names before any of their children, and the
	 * children of one name before the next name of that
	 * directory. leaf points into the frame's own buffer, which
	 * nothing moves while the entry is being looked at.
	 */
	while (c.zc_depth > 0) {
		fr = &c.zc_stack[c.zc_depth - 1];
		if (fr->zf_next == fr->zf_nents) {
			zw_pop(&c);
			continue;
		}
		leaf = fr->zf_leaves + fr->zf_ents[fr->zf_next];
		fr->zf_next++;
		dfd = dirfd(fr->zf_dir);
		if (zw_entry(&c, fr->zf_name, dfd, leaf, &nm, &dp) != 0)
			goto fail;
		if (dp == NULL)
			continue;
		/* the push may move the stack, so fr is stale after it */
		if (zw_push(&c, dp, nm) != 0)
			goto fail;
	}
	if (zr_tree_seal(&out->zw_tree) != 0) {
		(void) zw_failx(&c, root, "the tree would not seal");
		goto fail;
	}
	if (zr_tree_verify(&out->zw_tree, err, errlen) != 0)
		goto fail;
	zw_ctx_fini(&c);
	return (0);
fail:
	zw_ctx_fini(&c);
	return (-1);
}

void
zr_walk_fini(struct zr_walk *w)
{
	struct zr_attr *at;
	uint32_t i, j;

	if (w == NULL)
		return;
	for (i = 0; i < w->zw_nattrs; i++) {
		at = &w->zw_attrs[i];
		free(at->za_target);
		for (j = 0; j < at->za_nxattrs; j++) {
			free(at->za_xattrs[j].zx_name);
			free(at->za_xattrs[j].zx_value);
		}
		free(at->za_xattrs);
		free(at->za_acl);
		free(at->za_dacl);
	}
	free(w->zw_attrs);
	if (w->zw_rootfd >= 0)
		(void) close(w->zw_rootfd);
	zr_tree_fini(&w->zw_tree);
	memset(w, 0, sizeof (struct zr_walk));
	w->zw_rootfd = -1;
}

int
zr_walk_openat(const struct zr_walk *w, zr_name_t nm, int oflags)
{
	const char *p;
	size_t len;

	if (w == NULL || w->zw_rootfd < 0) {
		errno = EINVAL;
		return (-1);
	}
	p = zr_names_str(w->zw_tree.zt_names, nm, &len);
	if (p == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (len == 1)
		return (fcntl(w->zw_rootfd, F_DUPFD_CLOEXEC, 0));
	/* the table's paths are absolute; openat wants them relative */
	return (openat(w->zw_rootfd, p + 1, oflags | O_NOFOLLOW | O_CLOEXEC));
}
