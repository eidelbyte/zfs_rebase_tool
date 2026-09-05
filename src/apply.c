/*
 * apply: write the actions of a parsed manifest into the onto tree.
 * The root is opened once and every operation is relative to that
 * descriptor with the link never followed, so no path built here can
 * leave the tree. Actions run in manifest order; only the removal of
 * a directory waits, until the last action under it has run. Bytes
 * and attributes come from the walked from tree. Extended
 * attributes, the ACL and the file flags are the only writes POSIX
 * never standardised; they live in the one platform section below.
 *
 * Every action means "make this true", not "do this once": a name
 * already gone is removed, a directory already there is created, a
 * link already standing is linked, and an object in the way of a cp
 * is replaced. So the second run of a manifest over the tree the
 * first one left succeeds and lands in the same place, which is what
 * makes a fresh apply, a --continue and a repair one code path.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
/*
 * lpathconf, lchflags and the _PC_ACL_ names sit behind
 * __BSD_VISIBLE, which _XOPEN_SOURCE alone switches off. Defining
 * both is how FreeBSD's sys/_visible.h expects a program to ask for
 * POSIX 2008 and the BSD extensions together.
 */
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif
#ifdef __linux__
/* copy_file_range is glibc's, behind _GNU_SOURCE. */
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

#include "apply.h"
#include "manifest.h"
#include "name.h"
#include "verify.h"
#include "walk.h"
#include "yellow.h"

#define	ZA_BUFSZ	(64u * 1024u)
#define	ZA_PEND_MIN	16
#define	ZA_PERM		07777		/* the mode bits chmod sets */
#define	ZA_CREAT	0777		/* the mode a create asks for */

/*
 * Which document is being applied, which is the whole of what the
 * steps below need to know about their caller: the same copy, link
 * and unlink serve all three, and only the counter they add to
 * differs.
 */
#define	ZA_BY_MANIFEST	0		/* the manifest's own actions */
#define	ZA_BY_REPAIR	1		/* applying1's repair, out of onto */
#define	ZA_BY_CHOICE	2		/* the resolution's choices */

/* The driver's stop flag; see apply.h. */
volatile sig_atomic_t zr_apply_stop = 0;

/*
 * The action the harness's gate names, or zero for no gate at all;
 * see apply.h. One process runs one rebase, so a file static is the
 * whole of what it takes to get the number here.
 */
static unsigned int za_pause_at;

void
zr_apply_pause_at(unsigned int n)
{
	za_pause_at = n;
}

/*
 * And the same for the choices of a resolution; see apply.h. The
 * count is a local of zr_apply_choices and not a static, so every
 * call counts from one.
 */
static unsigned int za_cpause_at;

void
zr_apply_choice_pause_at(unsigned int n)
{
	za_cpause_at = n;
}

/*
 * ---------------------------------------------------------------
 * The platform section. Everything outside it is plain POSIX; here
 * are the extended attributes, the ACL and the file flags, and the
 * kernel-side copy. FreeBSD is the target -- there the tool writes
 * a ZFS clone, and that section is written first and in full. macOS
 * is the development stand-in, where this core is built and tested
 * without ZFS; Linux is a courtesy. Each platform supplies:
 *
 *	za_setxattrs()	make the target's attribute set equal to the
 *			from object's: set every one of from's, remove
 *			every one the target has that from lacks
 *	za_setacl()	write from's ACL, or, where from had none and
 *			the filesystem is NFSv4, strip the target's
 *			back to the trivial one the mode says
 *	za_setflags()	the file flags, where the platform has them
 *
 * Each returns 0 on success and -1 with errno set on failure. A
 * filesystem with no attributes, no ACL and no flags is a success,
 * not a failure. ZA_HAVE_ST_FLAGS says the platform's struct stat
 * has st_flags; ZA_HAVE_COPY_FILE_RANGE says the kernel can copy a
 * range without the bytes passing through this program.
 * ---------------------------------------------------------------
 */

#if defined(__FreeBSD__) || defined(__APPLE__)
#define	ZA_HAVE_ST_FLAGS	1	/* struct stat carries st_flags */
#endif
#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__linux__)
#define	ZA_HAVE_XATTRS		1
#endif

/*
 * The POSIX 2008 names for the two times a copy carries. macOS
 * spells them st_atimespec and st_mtimespec and aliases the POSIX
 * names only at its own feature level, which _XOPEN_SOURCE switches
 * off.
 */
#ifdef __APPLE__
#define	ZA_ATIME(s)	((s)->st_atimespec)
#define	ZA_MTIME(s)	((s)->st_mtimespec)
#else
#define	ZA_ATIME(s)	((s)->st_atim)
#define	ZA_MTIME(s)	((s)->st_mtim)
#endif

#ifdef ZA_HAVE_XATTRS

/*
 * The errnos that mean "there is nothing here to write" rather than
 * "the apply failed": a filesystem with no attribute support, a
 * namespace this process may not touch -- the system namespace is
 * root's on FreeBSD -- and the several spellings of "no such
 * attribute", which a delete races into.
 */
static int
za_absent(int e)
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

/* Does the from object carry this attribute, under its stored name? */
static int
za_has(const struct zr_attr *at, const char *name, size_t len)
{
	uint32_t i;

	for (i = 0; i < at->za_nxattrs; i++) {
		if (strlen(at->za_xattrs[i].zx_name) == len &&
		    memcmp(at->za_xattrs[i].zx_name, name, len) == 0)
			return (1);
	}
	return (0);
}

#endif	/* ZA_HAVE_XATTRS */

#if defined(__FreeBSD__)

#include <sys/param.h>

#include <sys/acl.h>
#include <sys/extattr.h>

#if defined(__FreeBSD_version) && __FreeBSD_version >= 1300000
#define	ZA_HAVE_COPY_FILE_RANGE	1
#endif

/* "system." plus an attribute name of at most 255 bytes, plus a NUL */
#define	ZA_FBSD_NAME	264

/*
 * The walk stores a FreeBSD attribute under its namespace prefix, so
 * that the two namespaces cannot collide in za_xattrs. Undo that
 * here. A name with neither prefix came from a walk on another
 * platform and belongs in the user namespace, which is the only one
 * an unprivileged process may write.
 */
static void
za_ns_split(const char *name, int *ns, const char **bare)
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

/*
 * Remove from one namespace every attribute the from object lacks.
 * extattr_list_link returns the names as a run of (one length byte,
 * that many bytes) pairs, none of them terminated, which is what the
 * walk reads too.
 */
static int
za_prune_ns(const char *full, int ns, const char *prefix,
    const struct zr_attr *at)
{
	char qname[ZA_FBSD_NAME];
	char *list;
	ssize_t want, n;
	size_t i, plen, len;
	int rc;

	want = extattr_list_link(full, ns, NULL, 0);
	if (want < 0)
		return (za_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = extattr_list_link(full, ns, list, (size_t)want);
	if (n < 0) {
		free(list);
		return (za_absent(errno) ? 0 : -1);
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
		if (za_has(at, qname, plen + len))
			continue;
		if (extattr_delete_link(full, ns, qname + plen) != 0 &&
		    !za_absent(errno))
			rc = -1;
	}
	free(list);
	return (rc);
}

static int
za_setxattrs(const char *full, const struct zr_attr *at)
{
	const char *bare;
	uint32_t i;
	int ns;

	for (i = 0; i < at->za_nxattrs; i++) {
		za_ns_split(at->za_xattrs[i].zx_name, &ns, &bare);
		if (extattr_set_link(full, ns, bare,
		    at->za_xattrs[i].zx_value,
		    at->za_xattrs[i].zx_len) < 0)
			return (-1);
	}
	if (za_prune_ns(full, EXTATTR_NAMESPACE_USER, "user.", at) != 0)
		return (-1);
	return (za_prune_ns(full, EXTATTR_NAMESPACE_SYSTEM, "system.", at));
}

/*
 * Which flavor of ACL the target carries: ZFS has NFSv4 ACLs, UFS
 * has POSIX.1e, and a filesystem with neither answers no to both.
 * lpathconf asks of the link itself, as the walk's reader does.
 */
static int
za_acl_flavor(const char *full, acl_type_t *typep)
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
 * The walk stores nothing for an NFSv4 ACL the mode already says in
 * full. Writing nothing would leave whatever the onto object had, so
 * a non-trivial ACL would survive a write that was meant to make the
 * object equal to from's. Strip it back to what the mode says.
 */
static int
za_acl_strip(const char *full)
{
	acl_t a, s;
	int rc;

	a = acl_get_link_np(full, ACL_TYPE_NFS4);
	if (a == NULL)
		return (za_absent(errno) || errno == EINVAL ? 0 : -1);
	s = acl_strip_np(a, 0);
	(void) acl_free(a);
	if (s == NULL)
		return (-1);
	rc = acl_set_link_np(full, ACL_TYPE_NFS4, s);
	(void) acl_free(s);
	return (rc);
}

/*
 * A POSIX.1e directory has two ACLs, the one that governs it and the
 * one its new children inherit, and both are written. An NFSv4 ACL
 * has one, inheritance being written into its entries.
 *
 * The walk kept the acl_t itself, so what goes on the object here is
 * the structure the from side's kernel handed out, entry for entry
 * and bit for bit. There is no text in between to print and parse
 * back, and so nothing an id that will not resolve, or a spelling
 * either end renders differently, could change on the way.
 *
 * acl_set_link_np sorts a POSIX.1e ACL in place before it submits
 * it (lib/libc/posix1e/acl_set.c), so this hands the walk's own
 * structure to a call that may reorder it. That is a normalisation,
 * not a change of meaning -- a POSIX.1e ACL is a set, and the
 * kernel stores it sorted anyway -- and an NFSv4 ACL, the ZFS case
 * and the one whose order is meaning, is never touched.
 */
static int
za_setacl(const char *full, const struct zr_attr *at, int isdir)
{
	acl_type_t type;

	if (za_acl_flavor(full, &type) == 0)
		return (0);
	if (at->za_acl == NULL) {
		if (type == ACL_TYPE_NFS4)
			return (za_acl_strip(full));
		if (isdir && acl_delete_def_link_np(full) != 0 &&
		    !za_absent(errno) && errno != EINVAL)
			return (-1);
		return (0);
	}
	if (acl_set_link_np(full, type, at->za_acl) != 0)
		return (-1);
	if (type != ACL_TYPE_ACCESS || !isdir)
		return (0);
	if (at->za_dacl == NULL) {
		if (acl_delete_def_link_np(full) != 0 && !za_absent(errno) &&
		    errno != EINVAL)
			return (-1);
		return (0);
	}
	return (acl_set_link_np(full, ACL_TYPE_DEFAULT, at->za_dacl));
}

static int
za_setflags(const char *full, uint32_t flags)
{
	return (lchflags(full, (unsigned long)flags));
}

#elif defined(__APPLE__)

#include <sys/acl.h>
#include <sys/xattr.h>

static int
za_setxattrs(const char *full, const struct zr_attr *at)
{
	char *list;
	ssize_t want, n;
	size_t i, len;
	uint32_t k;
	int rc;

	for (k = 0; k < at->za_nxattrs; k++) {
		if (setxattr(full, at->za_xattrs[k].zx_name,
		    at->za_xattrs[k].zx_value, at->za_xattrs[k].zx_len, 0,
		    XATTR_NOFOLLOW) != 0)
			return (-1);
	}
	want = listxattr(full, NULL, 0, XATTR_NOFOLLOW);
	if (want < 0)
		return (za_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = listxattr(full, list, (size_t)want, XATTR_NOFOLLOW);
	if (n < 0) {
		free(list);
		return (za_absent(errno) ? 0 : -1);
	}
	rc = 0;
	/* the list is the names, each one NUL-terminated */
	for (i = 0; rc == 0 && i < (size_t)n; i += len + 1) {
		len = strlen(list + i);
		if (len == 0 || za_has(at, list + i, len))
			continue;
		if (removexattr(full, list + i, XATTR_NOFOLLOW) != 0 &&
		    !za_absent(errno))
			rc = -1;
	}
	free(list);
	return (rc);
}

/*
 * The stand-in: one extended ACL, no flavors to tell apart, no
 * default ACL to write and no strip to undo a trivial one. What this
 * proves is that the text the walk kept goes back on the object;
 * NFSv4 against POSIX.1e is FreeBSD's to answer.
 */
static int
za_setacl(const char *full, const struct zr_attr *at, int isdir)
{
	acl_t a;
	int rc;

	(void) isdir;
	if (at->za_acl == NULL)
		return (0);
	a = acl_from_text(at->za_acl);
	if (a == NULL)
		return (-1);
	rc = acl_set_link_np(full, ACL_TYPE_EXTENDED, a);
	(void) acl_free(a);
	return (rc);
}

static int
za_setflags(const char *full, uint32_t flags)
{
	return (lchflags(full, flags));
}

#elif defined(__linux__)

#include <sys/xattr.h>

#define	ZA_HAVE_COPY_FILE_RANGE	1

static int
za_setxattrs(const char *full, const struct zr_attr *at)
{
	char *list;
	ssize_t want, n;
	size_t i, len;
	uint32_t k;
	int rc;

	for (k = 0; k < at->za_nxattrs; k++) {
		if (lsetxattr(full, at->za_xattrs[k].zx_name,
		    at->za_xattrs[k].zx_value, at->za_xattrs[k].zx_len,
		    0) != 0)
			return (-1);
	}
	want = llistxattr(full, NULL, 0);
	if (want < 0)
		return (za_absent(errno) ? 0 : -1);
	list = malloc((size_t)want + 1);
	if (list == NULL)
		return (-1);
	n = llistxattr(full, list, (size_t)want);
	if (n < 0) {
		free(list);
		return (za_absent(errno) ? 0 : -1);
	}
	rc = 0;
	/* the list is the names, each one NUL-terminated */
	for (i = 0; rc == 0 && i < (size_t)n; i += len + 1) {
		len = strlen(list + i);
		if (len == 0 || za_has(at, list + i, len))
			continue;
		if (lremovexattr(full, list + i) != 0 && !za_absent(errno))
			rc = -1;
	}
	free(list);
	return (rc);
}

/* A POSIX.1e ACL on Linux is already one of the xattrs above. */
static int
za_setacl(const char *full, const struct zr_attr *at, int isdir)
{
	(void) full;
	(void) at;
	(void) isdir;
	return (0);
}

/* No file flags in the stat Linux hands back, so none to write. */
static int
za_setflags(const char *full, uint32_t flags)
{
	(void) full;
	(void) flags;
	return (0);
}

#else

/* An unknown platform still applies; it just writes no attributes. */
static int
za_setxattrs(const char *full, const struct zr_attr *at)
{
	(void) full;
	(void) at;
	return (0);
}

static int
za_setacl(const char *full, const struct zr_attr *at, int isdir)
{
	(void) full;
	(void) at;
	(void) isdir;
	return (0);
}

static int
za_setflags(const char *full, uint32_t flags)
{
	(void) full;
	(void) flags;
	return (0);
}

#endif	/* platform section ends */

/*
 * The apply's own state. zc_full is scratch: onto_root with one
 * action path appended, which is what the platform calls above need,
 * since none of them takes a descriptor and a name. zc_pend is the
 * stack of directories whose rm is waiting for its scope to close;
 * it borrows the paths from the parsed manifest, which outlives it.
 */
struct za_ctx {
	int			zc_rootfd;
	int			zc_by;		/* one of ZA_BY_ above */
	const struct zr_parsed	*zc_m;		/* for the conflict marks */
	const struct zr_walk	*zc_from;
	const struct zr_walk	*zc_onto;	/* dup source, or NULL */
	struct zr_apply_stats	*zc_st;
	char			*zc_err;
	size_t			zc_errlen;
	char			*zc_root;
	size_t			zc_rootlen;
	char			*zc_full;
	size_t			zc_fullcap;
	const unsigned char	**zc_pend;
	size_t			*zc_pendlen;
	uint32_t		zc_npend;
	uint32_t		zc_pendcap;
	unsigned char		*zc_buf;
};

/* One from object: where its bytes are and what it looks like. */
struct za_src {
	const struct zr_walk	*zs_walk;	/* from, or onto for dup */
	zr_name_t		zs_name;
	const struct zr_attr	*zs_at;
	zr_type_t		zs_type;
};

static int
za_failp(struct za_ctx *c, const unsigned char *path, const char *step)
{
	if (c->zc_err != NULL && c->zc_errlen > 0) {
		(void) snprintf(c->zc_err, c->zc_errlen, "%s: %s: %s",
		    (const char *)path, step, strerror(errno));
	}
	return (-1);
}

static int
za_failpx(struct za_ctx *c, const unsigned char *path, const char *what)
{
	if (c->zc_err != NULL && c->zc_errlen > 0) {
		(void) snprintf(c->zc_err, c->zc_errlen, "%s: %s",
		    (const char *)path, what);
	}
	return (-1);
}

static int
za_fail(struct za_ctx *c, const struct zr_action *a, const char *step)
{
	return (za_failp(c, a->za_path, step));
}

static int
za_failx(struct za_ctx *c, const struct zr_action *a, const char *what)
{
	return (za_failpx(c, a->za_path, what));
}

/* A re-stat found something the from tree does not say. */
static int
za_differs(struct za_ctx *c, const struct zr_action *a, const char *step,
    const char *what)
{
	if (c->zc_err != NULL && c->zc_errlen > 0) {
		(void) snprintf(c->zc_err, c->zc_errlen,
		    "%s: after %s the %s of the result is not the one the "
		    "manifest asked for", (const char *)a->za_path, step,
		    what);
	}
	return (-1);
}

/*
 * The paths openat wants: the table's are absolute, and the root
 * itself is never the subject of an action, so dropping the leading
 * slash is all there is to it.
 */
static const char *
za_rel(const unsigned char *path)
{
	return ((const char *)path + 1);
}

/*
 * A path this apply will act on. The tree section cannot spell a
 * "." or a ".." component -- two dots close a scope -- but the
 * argument of an ln is a free path from the document, and it goes
 * straight to linkat. Refuse anything that is not one plain absolute
 * path, so that no name can climb out of the root.
 */
static int
za_path_ok(struct za_ctx *c, const struct zr_action *a,
    const unsigned char *p, size_t len)
{
	size_t i, seg;

	if (len < 2 || p[0] != '/' || p[len - 1] == '/')
		return (za_failx(c, a, "a path that is not one absolute path "
		    "under the root"));
	seg = 1;
	for (i = 1; i <= len; i++) {
		if (i != len && p[i] != '/')
			continue;
		if (i == seg)
			return (za_failx(c, a, "a path with an empty "
			    "component"));
		if (i - seg == 1 && p[seg] == '.')
			return (za_failx(c, a, "a path with a \".\" "
			    "component"));
		if (i - seg == 2 && p[seg] == '.' && p[seg + 1] == '.')
			return (za_failx(c, a, "a path with a \"..\" "
			    "component"));
		seg = i + 1;
	}
	return (0);
}

/*
 * onto_root's own path, with any trailing slashes cut, so that the
 * platform calls -- which take a path, never a descriptor and a name
 * -- can be handed the root followed by an action's absolute path.
 */
static int
za_root_copy(struct za_ctx *c, const char *root)
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

/* The root's path with one action path appended, in zc_full. */
static const char *
za_full(struct za_ctx *c, const unsigned char *path, size_t len)
{
	char *buf;
	size_t need;

	need = c->zc_rootlen + len + 1;
	if (need > c->zc_fullcap) {
		buf = realloc(c->zc_full, need);
		if (buf == NULL)
			return (NULL);
		c->zc_full = buf;
		c->zc_fullcap = need;
	}
	memcpy(c->zc_full, c->zc_root, c->zc_rootlen);
	memcpy(c->zc_full + c->zc_rootlen, path, len);
	c->zc_full[c->zc_rootlen + len] = '\0';
	return (c->zc_full);
}

static int
za_type(mode_t m, zr_type_t *out)
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

/*
 * The object an action's argument names, in the walk w (from for cp
 * and write, onto for dup), and its attributes.
 */
static int
za_source(struct za_ctx *c, const struct zr_action *a,
    const struct zr_walk *w, struct za_src *src)
{
	const struct zr_tree *t;
	zr_name_t nm;
	zr_pool_t pool;

	if (w == NULL)
		return (za_failx(c, a, "dup needs the onto tree"));
	t = &w->zw_tree;
	nm = zr_names_lookup(t->zt_names, (const char *)a->za_arg,
	    a->za_arglen);
	if (nm == ZR_NAME_NONE)
		return (za_failx(c, a, "the source tree holds no such path"));
	pool = zr_tree_pool(t, nm);
	if (pool == ZR_POOL_NONE || pool >= w->zw_nattrs)
		return (za_failx(c, a, "the source tree holds no such path"));
	src->zs_walk = w;
	src->zs_name = nm;
	src->zs_at = &w->zw_attrs[pool];
	src->zs_type = t->zt_pools[pool].zp_type;
	return (0);
}

/*
 * The from object's own stat, which is where the times come from:
 * zr_attr keeps none, the content oracle having no use for them. A
 * regular file is already open for its bytes and is fstat'd through
 * that descriptor before a byte is read, so the atime that reaches
 * the result is the one the snapshot had. Nothing else can be opened
 * for the purpose -- zr_walk_openat cannot open a symbolic link at
 * all, and opening a fifo or a device would block or disturb it --
 * so those are read with an fstatat of the from root, which is the
 * same descriptor-relative call that never follows a link.
 */
static int
za_from_stat(struct za_ctx *c, const struct zr_action *a,
    const struct zr_walk *w, struct stat *st)
{
	const char *rel;

	rel = a->za_arglen == 1 ? "." : za_rel(a->za_arg);
	if (fstatat(w->zw_rootfd, rel, st, AT_SYMLINK_NOFOLLOW) != 0)
		return (za_fail(c, a, "stat of the source object"));
	return (0);
}

/* The bytes of one file, from an open descriptor to an open one. */
static int
za_copy_rw(struct za_ctx *c, const struct zr_action *a, int in, int out,
    uint64_t *np)
{
	ssize_t n, w, off;

	for (;;) {
		n = read(in, c->zc_buf, ZA_BUFSZ);
		if (n < 0)
			return (za_fail(c, a, "read of the from file"));
		if (n == 0)
			return (0);
		for (off = 0; off < n; off += w) {
			w = write(out, c->zc_buf + off, (size_t)(n - off));
			if (w < 0)
				return (za_fail(c, a, "write"));
			if (w == 0) {
				errno = EIO;
				return (za_fail(c, a, "write"));
			}
		}
		*np += (uint64_t)n;
	}
}

/*
 * The same bytes, asked of the kernel first where it can do the copy
 * itself -- on ZFS that is a block clone rather than a read and a
 * write. A kernel that declines for this pair of files says so with
 * one of a handful of errnos, and then the loop above does it.
 */
static int
za_copy(struct za_ctx *c, const struct zr_action *a, int in, int out,
    uint64_t *np)
{
#ifdef ZA_HAVE_COPY_FILE_RANGE
	ssize_t n;

	for (;;) {
		n = copy_file_range(in, NULL, out, NULL, (size_t)ZA_BUFSZ, 0);
		if (n == 0)
			return (0);
		if (n > 0) {
			*np += (uint64_t)n;
			continue;
		}
		if (errno == EXDEV || errno == EINVAL || errno == ENOSYS ||
		    errno == EOPNOTSUPP || errno == EBADF)
			break;
		return (za_fail(c, a, "copy_file_range"));
	}
	if (lseek(in, 0, SEEK_SET) < 0 || lseek(out, 0, SEEK_SET) < 0)
		return (za_fail(c, a, "seek"));
	*np = 0;
#endif
	return (za_copy_rw(c, a, in, out, np));
}

/*
 * Set the mode. A symbolic link's own mode is not settable
 * everywhere -- Linux's fchmodat refuses AT_SYMLINK_NOFOLLOW
 * outright -- and where the platform declines it is skipped, not an
 * error: the mode of a link means nothing on a filesystem that does
 * not keep one.
 */
static int
za_chmod(struct za_ctx *c, const struct zr_action *a, mode_t mode,
    int islink)
{
	if (fchmodat(c->zc_rootfd, za_rel(a->za_path), mode & ZA_PERM,
	    AT_SYMLINK_NOFOLLOW) == 0)
		return (0);
	if (islink && (errno == ENOTSUP || errno == EOPNOTSUPP ||
	    errno == EINVAL))
		return (0);
	return (za_fail(c, a, "chmod"));
}

/*
 * Every attribute of the from object, onto the target, in the one
 * order that works: the owner first, because a chown clears the
 * setuid and setgid bits, so the mode must follow it; then the
 * extended attributes and the ACL, each of which touches the times;
 * then the times themselves; then the flags, last, because one of
 * them is immutable and nothing can be written after it.
 */
static int
za_attrs(struct za_ctx *c, const struct zr_action *a,
    const struct za_src *src, const struct stat *fst)
{
	const struct zr_attr *at = src->zs_at;
	struct timespec ts[2];
	const char *full, *rel;
	struct stat st;
	int islink;

	rel = za_rel(a->za_path);
	islink = src->zs_type == ZR_T_SYMLINK;
	if (fstatat(c->zc_rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (za_fail(c, a, "stat"));
	if (st.st_uid != at->za_uid || st.st_gid != at->za_gid) {
		if (fchownat(c->zc_rootfd, rel, at->za_uid, at->za_gid,
		    AT_SYMLINK_NOFOLLOW) != 0)
			return (za_fail(c, a, "chown"));
	}
	if (za_chmod(c, a, at->za_mode, islink) != 0)
		return (-1);
	full = za_full(c, a->za_path, a->za_pathlen);
	if (full == NULL) {
		errno = ENOMEM;
		return (za_fail(c, a, "memory"));
	}
	if (za_setxattrs(full, at) != 0)
		return (za_fail(c, a, "extended attributes"));
	if (za_setacl(full, at, src->zs_type == ZR_T_DIR) != 0)
		return (za_fail(c, a, "acl"));
	ts[0] = ZA_ATIME(fst);
	ts[1] = ZA_MTIME(fst);
	if (utimensat(c->zc_rootfd, rel, ts, AT_SYMLINK_NOFOLLOW) != 0)
		return (za_fail(c, a, "times"));
#ifdef ZA_HAVE_ST_FLAGS
	/*
	 * Read again: the steps above stamp the object, and what a
	 * stamp sets is not in the word the first stat read.
	 */
	if (fstatat(c->zc_rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (za_fail(c, a, "re-stat"));
	if (ZR_ST_FLAGS(&st) != at->za_flags &&
	    za_setflags(full, at->za_flags) != 0)
		return (za_fail(c, a, "flags"));
#else
	if (za_setflags(full, at->za_flags) != 0)
		return (za_fail(c, a, "flags"));
#endif
	return (0);
}

/*
 * What the action promised, read back off the filesystem. The type
 * always; for a cp and a write also the mode, the owner, the size of
 * a regular file and the flags. A symbolic link's mode is left out,
 * since setting it is the one attribute step a platform may decline.
 */
static int
za_verify(struct za_ctx *c, const struct zr_action *a,
    const struct za_src *src, const char *step, uint64_t size)
{
	const struct zr_attr *at = src->zs_at;
	struct stat st;
	zr_type_t type;

	if (fstatat(c->zc_rootfd, za_rel(a->za_path), &st,
	    AT_SYMLINK_NOFOLLOW) != 0)
		return (za_fail(c, a, "re-stat"));
	if (za_type(st.st_mode, &type) != 0 || type != src->zs_type)
		return (za_differs(c, a, step, "type"));
	if (type != ZR_T_SYMLINK &&
	    (st.st_mode & ZA_PERM) != (at->za_mode & ZA_PERM))
		return (za_differs(c, a, step, "mode"));
	if (st.st_uid != at->za_uid)
		return (za_differs(c, a, step, "owner"));
	if (st.st_gid != at->za_gid)
		return (za_differs(c, a, step, "group"));
	if (type == ZR_T_FILE && (uint64_t)st.st_size != size)
		return (za_differs(c, a, step, "size"));
#ifdef ZA_HAVE_ST_FLAGS
	if (ZR_ST_FLAGS(&st) != at->za_flags)
		return (za_differs(c, a, step, "flags"));
#endif
	return (0);
}

/*
 * Clear the way for a cp. A leaf goes: it is a name the result gives
 * to something else, or a symbolic link being replaced, or the same
 * name an earlier run of this manifest made. A directory where the
 * action wants a directory is that directory, made by an earlier run
 * with the children of the lines that follow inside it, and it
 * stays: what the action still owes it is its attributes, which are
 * written below. A directory where the action wants anything else is
 * the type-change shape, where the rm of the old directory closed on
 * the line before this one -- so what is left is empty and rmdir
 * takes it, and anything still inside is a manifest that did not
 * remove its children.
 */
static int
za_clear(struct za_ctx *c, const struct zr_action *a, zr_type_t want)
{
	const char *rel = za_rel(a->za_path);
	struct stat st;

	if (fstatat(c->zc_rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		if (errno == ENOENT)
			return (0);
		return (za_fail(c, a, "stat"));
	}
	if (!S_ISDIR(st.st_mode)) {
		if (unlinkat(c->zc_rootfd, rel, 0) != 0)
			return (za_fail(c, a, "unlink"));
		return (0);
	}
	if (want == ZR_T_DIR)
		return (0);
	if (unlinkat(c->zc_rootfd, rel, AT_REMOVEDIR) != 0)
		return (za_fail(c, a, "rmdir of the directory in the way"));
	return (0);
}

/* The bytes of the from file into an already open descriptor. */
static int
za_pour(struct za_ctx *c, const struct zr_action *a,
    const struct za_src *src, int out, struct stat *fst, uint64_t *size)
{
	uint64_t nb = 0;
	int in, rc;

	in = zr_walk_openat(src->zs_walk, src->zs_name, O_RDONLY);
	if (in < 0)
		return (za_fail(c, a, "open of the from file"));
	if (fstat(in, fst) != 0) {
		rc = za_fail(c, a, "stat of the from file");
		(void) close(in);
		return (rc);
	}
	rc = za_copy(c, a, in, out, &nb);
	(void) close(in);
	c->zc_st->zs_bytes += nb;
	*size = (uint64_t)fst->st_size;
	return (rc);
}

/*
 * cp: a new object with the type, bytes and attributes of from's.
 * dup: the same, from the object onto holds at the argument, for a
 * severed half that must carry bytes only onto has. The source is
 * read from the onto walk (a snapshot, or the tree before apply);
 * the anchor keeps that object unchanged, which is what makes the
 * copy honest.
 */
static int
za_do_cp(struct za_ctx *c, const struct zr_action *a,
    const struct zr_walk *w, const char *what)
{
	struct za_src src;
	struct stat fst;
	const char *rel;
	uint64_t size = 0;
	mode_t mode;
	int fd, rc;

	if (za_path_ok(c, a, a->za_arg, a->za_arglen) != 0 ||
	    za_source(c, a, w, &src) != 0 ||
	    za_clear(c, a, src.zs_type) != 0)
		return (-1);
	rel = za_rel(a->za_path);
	mode = src.zs_at->za_mode & ZA_CREAT;
	switch (src.zs_type) {
	case ZR_T_DIR:
		/* the directory an earlier run made is the one wanted */
		if (mkdirat(c->zc_rootfd, rel, mode) != 0 && errno != EEXIST)
			return (za_fail(c, a, "mkdir"));
		if (za_from_stat(c, a, w, &fst) != 0)
			return (-1);
		break;
	case ZR_T_FILE:
		fd = openat(c->zc_rootfd, rel, O_CREAT | O_EXCL | O_WRONLY |
		    O_NOFOLLOW | O_CLOEXEC, mode);
		if (fd < 0)
			return (za_fail(c, a, "create"));
		rc = za_pour(c, a, &src, fd, &fst, &size);
		if (close(fd) != 0 && rc == 0)
			rc = za_fail(c, a, "close");
		if (rc != 0)
			return (-1);
		break;
	case ZR_T_SYMLINK:
		if (src.zs_at->za_target == NULL)
			return (za_failx(c, a, "the from symlink has no "
			    "target"));
		if (symlinkat(src.zs_at->za_target, c->zc_rootfd, rel) != 0)
			return (za_fail(c, a, "symlink"));
		if (za_from_stat(c, a, w, &fst) != 0)
			return (-1);
		break;
	case ZR_T_CHR:
	case ZR_T_BLK:
		mode |= src.zs_type == ZR_T_CHR ? S_IFCHR : S_IFBLK;
		if (mknodat(c->zc_rootfd, rel, mode,
		    (dev_t)src.zs_at->za_rdev) != 0)
			return (za_fail(c, a, "mknod"));
		if (za_from_stat(c, a, w, &fst) != 0)
			return (-1);
		break;
	case ZR_T_FIFO:
		if (mkfifoat(c->zc_rootfd, rel, mode) != 0)
			return (za_fail(c, a, "mkfifo"));
		if (za_from_stat(c, a, w, &fst) != 0)
			return (-1);
		break;
	default:
		return (za_failx(c, a, "cannot recreate a socket"));
	}
	if (za_attrs(c, a, &src, &fst) != 0 ||
	    za_verify(c, a, &src, what, size) != 0)
		return (-1);
	if (c->zc_by == ZA_BY_REPAIR)
		c->zc_st->zs_restored++;
	else if (c->zc_by == ZA_BY_CHOICE)
		c->zc_st->zs_made++;
	else if (w == c->zc_from)
		c->zc_st->zs_cp++;
	else
		c->zc_st->zs_dup++;
	return (0);
}

/*
 * A symbolic link's target cannot be rewritten in place: there is no
 * call for it, so the name is unlinked and made again. That is the
 * one place where a write does not keep the object's identity, and
 * it can only bite a symbolic link with a second name, which the
 * walk would have pooled and the manifest would have said ln for.
 */
static int
za_relink(struct za_ctx *c, const struct zr_action *a, const char *target)
{
	const char *rel = za_rel(a->za_path);
	unsigned char *buf;
	size_t len;
	ssize_t n;
	int same;

	len = strlen(target);
	buf = malloc(len + 2);
	if (buf == NULL) {
		errno = ENOMEM;
		return (za_fail(c, a, "memory"));
	}
	n = readlinkat(c->zc_rootfd, rel, (char *)buf, len + 2);
	if (n < 0) {
		free(buf);
		return (za_fail(c, a, "readlink"));
	}
	same = n == (ssize_t)len && memcmp(buf, target, len) == 0;
	free(buf);
	if (same)
		return (0);
	if (unlinkat(c->zc_rootfd, rel, 0) != 0)
		return (za_fail(c, a, "unlink"));
	if (symlinkat(target, c->zc_rootfd, rel) != 0)
		return (za_fail(c, a, "symlink"));
	return (0);
}

/*
 * write: the object at the path stays the object it is, so every one
 * of its names sees the new bytes and the new attributes. Only a
 * regular file has bytes to replace; for everything else a write is
 * its attributes.
 */
static int
za_do_write(struct za_ctx *c, const struct zr_action *a)
{
	struct za_src src;
	struct stat st, fst;
	const char *rel;
	uint64_t size = 0;
	zr_type_t type;
	int fd, rc;

	if (za_path_ok(c, a, a->za_arg, a->za_arglen) != 0 ||
	    za_source(c, a, c->zc_from, &src) != 0)
		return (-1);
	rel = za_rel(a->za_path);
	if (fstatat(c->zc_rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (za_fail(c, a, "stat of the object to write"));
	if (za_type(st.st_mode, &type) != 0 || type != src.zs_type)
		return (za_failx(c, a, "a write cannot change the type of "
		    "an object"));
	if (type == ZR_T_FILE) {
		fd = openat(c->zc_rootfd, rel, O_WRONLY | O_TRUNC |
		    O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return (za_fail(c, a, "open"));
		rc = za_pour(c, a, &src, fd, &fst, &size);
		if (close(fd) != 0 && rc == 0)
			rc = za_fail(c, a, "close");
		if (rc != 0)
			return (-1);
	} else {
		if (type == ZR_T_SYMLINK) {
			if (src.zs_at->za_target == NULL)
				return (za_failx(c, a, "the from symlink has "
				    "no target"));
			if (za_relink(c, a, src.zs_at->za_target) != 0)
				return (-1);
		}
		if (za_from_stat(c, a, c->zc_from, &fst) != 0)
			return (-1);
	}
	if (za_attrs(c, a, &src, &fst) != 0 ||
	    za_verify(c, a, &src, "write", size) != 0)
		return (-1);
	c->zc_st->zs_write++;
	return (0);
}

/*
 * Are these two names one object already? Two stats and no write:
 * how the ln below knows there is nothing left to do, how the apply
 * confirms an ln a report called done before it takes the report's
 * word for it, and how a choice knows a name is pooled where the
 * resolution wants it.
 */
static int
za_pooled(struct za_ctx *c, const unsigned char *path,
    const unsigned char *anchor)
{
	struct stat st, ast;

	if (fstatat(c->zc_rootfd, za_rel(path), &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (0);
	if (fstatat(c->zc_rootfd, za_rel(anchor), &ast,
	    AT_SYMLINK_NOFOLLOW) != 0)
		return (0);
	return (st.st_ino == ast.st_ino && st.st_dev == ast.st_dev);
}

static int
za_linked(struct za_ctx *c, const struct zr_action *a)
{
	return (za_pooled(c, a->za_path, a->za_arg));
}

/* One link made or found standing, counted for whoever asked for it. */
static void
za_linked_up(struct za_ctx *c)
{
	if (c->zc_by == ZA_BY_REPAIR)
		c->zc_st->zs_relinked++;
	else if (c->zc_by == ZA_BY_CHOICE)
		c->zc_st->zs_linked++;
	else
		c->zc_st->zs_ln++;
}

/*
 * ln: the path becomes another name of the object the anchor names,
 * both of them in the result tree. The anchor is looked at before
 * anything is unlinked, so a manifest naming an anchor that is not
 * there leaves the tree as it found it. A path that is the anchor's
 * object already is the link this action asks for: it stands, and
 * nothing is unlinked to make it again.
 */
static int
za_do_ln(struct za_ctx *c, const struct zr_action *a)
{
	const char *rel, *arel;
	struct stat ast, st;
	zr_type_t atype, type;

	if (za_path_ok(c, a, a->za_arg, a->za_arglen) != 0)
		return (-1);
	rel = za_rel(a->za_path);
	arel = za_rel(a->za_arg);
	if (fstatat(c->zc_rootfd, arel, &ast, AT_SYMLINK_NOFOLLOW) != 0) {
		if (c->zc_err != NULL && c->zc_errlen > 0) {
			(void) snprintf(c->zc_err, c->zc_errlen,
			    "%s: ln: %s: %s", (const char *)a->za_path,
			    (const char *)a->za_arg, strerror(errno));
		}
		return (-1);
	}
	if (fstatat(c->zc_rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		if (st.st_ino == ast.st_ino && st.st_dev == ast.st_dev) {
			za_linked_up(c);
			return (0);
		}
		if (S_ISDIR(st.st_mode))
			return (za_failx(c, a, "a directory is in the way of "
			    "a link"));
		if (unlinkat(c->zc_rootfd, rel, 0) != 0)
			return (za_fail(c, a, "unlink"));
	} else if (errno != ENOENT) {
		return (za_fail(c, a, "stat"));
	}
	if (linkat(c->zc_rootfd, arel, c->zc_rootfd, rel, 0) != 0)
		return (za_fail(c, a, "link"));
	if (fstatat(c->zc_rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (za_fail(c, a, "re-stat"));
	if (za_type(st.st_mode, &type) != 0 ||
	    za_type(ast.st_mode, &atype) != 0 || type != atype)
		return (za_failpx(c, a->za_path, "after ln the type of the "
		    "result is not the anchor's"));
	if (st.st_ino != ast.st_ino)
		return (za_failpx(c, a->za_path, "after ln the result is not "
		    "the anchor's object"));
	za_linked_up(c);
	return (0);
}

/* Is path inside the directory dir? */
static int
za_under(const unsigned char *dir, size_t dirlen, const unsigned char *path,
    size_t len)
{
	return (len > dirlen && path[dirlen] == '/' &&
	    memcmp(path, dir, dirlen) == 0);
}

/* Hold one directory's rm until its scope closes. */
static int
za_pend(struct za_ctx *c, const struct zr_action *a)
{
	const unsigned char **pp;
	size_t *lp;
	uint32_t cap;

	if (c->zc_npend == c->zc_pendcap) {
		cap = c->zc_pendcap != 0 ? c->zc_pendcap * 2 : ZA_PEND_MIN;
		pp = realloc(c->zc_pend, (size_t)cap * sizeof (*pp));
		if (pp == NULL) {
			errno = ENOMEM;
			return (za_fail(c, a, "memory"));
		}
		c->zc_pend = pp;
		lp = realloc(c->zc_pendlen, (size_t)cap * sizeof (*lp));
		if (lp == NULL) {
			errno = ENOMEM;
			return (za_fail(c, a, "memory"));
		}
		c->zc_pendlen = lp;
		c->zc_pendcap = cap;
	}
	c->zc_pend[c->zc_npend] = a->za_path;
	c->zc_pendlen[c->zc_npend] = a->za_pathlen;
	c->zc_npend++;
	return (0);
}

/*
 * The one ordering rule. Every directory whose rm is waiting and
 * which the next action's path does not lie under has had its last
 * child; remove it now. A NULL path is the end of the manifest,
 * where every one of them is due.
 *
 * A directory already gone is a removal that is true, and a
 * directory that will not go because a conflicted name is still
 * inside it is the one removal the manifest cannot ask for yet: it
 * is left, counted as skipped, and the choices of the resolution are
 * what free it. Anything else that will not go is a manifest
 * that did not remove its children, which is loud.
 */
static int
za_close_to(struct za_ctx *c, const unsigned char *path, size_t len)
{
	const unsigned char *dir;
	size_t dirlen;

	while (c->zc_npend > 0) {
		dir = c->zc_pend[c->zc_npend - 1];
		dirlen = c->zc_pendlen[c->zc_npend - 1];
		if (path != NULL && za_under(dir, dirlen, path, len))
			break;
		if (unlinkat(c->zc_rootfd, za_rel(dir), AT_REMOVEDIR) != 0) {
			if ((errno == ENOTEMPTY || errno == EEXIST) &&
			    zr_verify_blocked(c->zc_m, dir, dirlen) != 0) {
				c->zc_npend--;
				c->zc_st->zs_skipped++;
				continue;
			}
			if (errno != ENOENT)
				return (za_failp(c, dir, "rmdir"));
		}
		c->zc_npend--;
		c->zc_st->zs_rm++;
	}
	return (0);
}

/*
 * rm: a leaf goes now, a directory when its scope closes. A name
 * that is not there is the state the action asks for and not a
 * failure, which is what lets a manifest run twice.
 */
static int
za_do_rm(struct za_ctx *c, const struct zr_action *a)
{
	if (a->za_isdir != 0)
		return (za_pend(c, a));
	if (unlinkat(c->zc_rootfd, za_rel(a->za_path), 0) != 0 &&
	    errno != ENOENT)
		return (za_fail(c, a, "unlink"));
	c->zc_st->zs_rm++;
	return (0);
}

static void
za_ctx_fini(struct za_ctx *c)
{
	if (c->zc_rootfd >= 0)
		(void) close(c->zc_rootfd);
	free(c->zc_root);
	free(c->zc_full);
	free(c->zc_pend);
	free(c->zc_pendlen);
	free(c->zc_buf);
	memset(c, 0, sizeof (struct za_ctx));
	c->zc_rootfd = -1;
}

/*
 * An action a report marked done or blocked, at the moment the apply
 * reaches it. Everything is taken on the report's word but the ln,
 * whose anchor is a name of the result: a cp, a dup or a write
 * performed since the report was written may have put a new object
 * at that name, and this one would still be on the old one. It is
 * the one dependency an action of this manifest has on another, and
 * two stats settle it.
 */
static int
za_skippable(struct za_ctx *c, const struct zr_action *a)
{
	if (a->za_kind != ZR_ACT_LN)
		return (1);
	if (za_path_ok(c, a, a->za_arg, a->za_arglen) != 0)
		return (0);
	return (za_linked(c, a));
}

int
zr_apply_with(const struct zr_parsed *m, const char *onto_root,
    const struct zr_walk *from, const struct zr_walk *onto,
    const struct zr_verify_report *skip, struct zr_apply_stats *st,
    char *err, size_t errlen)
{
	struct zr_apply_stats spare;
	const struct zr_action *a;
	struct za_ctx c;
	unsigned int performed = 0;
	uint32_t i;
	int rc = -1;

	memset(&c, 0, sizeof (struct za_ctx));
	c.zc_rootfd = -1;
	c.zc_m = m;
	c.zc_from = from;
	c.zc_onto = onto;
	c.zc_err = err;
	c.zc_errlen = errlen;
	c.zc_st = st != NULL ? st : &spare;
	memset(c.zc_st, 0, sizeof (struct zr_apply_stats));
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (m == NULL || onto_root == NULL || from == NULL ||
	    from->zw_rootfd < 0) {
		errno = EINVAL;
		return (za_failp(&c, (const unsigned char *)"(the apply)",
		    "arguments"));
	}
	if (skip != NULL && (skip->zv_nactions != m->zp_nactions ||
	    (m->zp_nactions != 0 && skip->zv_outcome == NULL))) {
		errno = EINVAL;
		return (za_failp(&c, (const unsigned char *)"(the apply)",
		    "a report of another manifest"));
	}
	if (za_root_copy(&c, onto_root) != 0) {
		errno = ENOMEM;
		(void) za_failp(&c, (const unsigned char *)onto_root,
		    "memory");
		goto out;
	}
	c.zc_buf = malloc(ZA_BUFSZ);
	if (c.zc_buf == NULL) {
		errno = ENOMEM;
		(void) za_failp(&c, (const unsigned char *)onto_root,
		    "memory");
		goto out;
	}
	c.zc_rootfd = open(onto_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
	    O_CLOEXEC);
	if (c.zc_rootfd < 0) {
		(void) za_failp(&c, (const unsigned char *)onto_root, "open");
		goto out;
	}
	for (i = 0; i < m->zp_nactions; i++) {
		/*
		 * Between two actions is the one place the apply can
		 * be left: the action before is finished and the one
		 * after has not begun. The pending directory removals
		 * are dropped rather than run, because their scopes
		 * were never closed; what that leaves is a tree part
		 * way through the manifest, which is a clone, and
		 * --abort destroys it whole.
		 */
		if (zr_apply_stop != 0) {
			if (err != NULL && errlen > 0)
				(void) snprintf(err, errlen, "interrupted");
			goto out;
		}
		a = &m->zp_actions[i];
		if (a->za_kind == ZR_ACT_CONFLICT)
			continue;
		if (za_path_ok(&c, a, a->za_path, a->za_pathlen) != 0)
			goto out;
		if (za_close_to(&c, a->za_path, a->za_pathlen) != 0)
			goto out;
		/*
		 * The scope above is closed for every action, skipped
		 * or not: which directories have had their last child
		 * is the manifest's own order and no report's business.
		 */
		if (skip != NULL && (skip->zv_outcome[i] == ZR_OC_DONE ||
		    skip->zv_outcome[i] == ZR_OC_BLOCKED) &&
		    za_skippable(&c, a) != 0) {
			c.zc_st->zs_skipped++;
			continue;
		}
		/*
		 * The harness's gate, counted over the actions this
		 * apply really performs: the one before it is
		 * finished and this one has not begun, which is the
		 * same place a signal leaves the loop.
		 */
		if (++performed == za_pause_at)
			(void) raise(SIGSTOP);
		switch (a->za_kind) {
		case ZR_ACT_RM:
			if (za_do_rm(&c, a) != 0)
				goto out;
			break;
		case ZR_ACT_LN:
			if (za_do_ln(&c, a) != 0)
				goto out;
			break;
		case ZR_ACT_CP:
			if (za_do_cp(&c, a, c.zc_from, "cp") != 0)
				goto out;
			break;
		case ZR_ACT_DUP:
			if (za_do_cp(&c, a, c.zc_onto, "dup") != 0)
				goto out;
			break;
		default:
			if (za_do_write(&c, a) != 0)
				goto out;
			break;
		}
	}
	if (za_close_to(&c, NULL, 0) != 0)
		goto out;
	rc = 0;
out:
	za_ctx_fini(&c);
	return (rc);
}

/*
 * ---------------------------------------------------------------
 * The repair, and the self-check that runs it. Everything above is
 * the manifest's; this is the rest of the tree -- the names no
 * action spoke for -- which at applying1 must be onto's still.
 * ---------------------------------------------------------------
 */

/* One entry of the report's name list, with the path it stands for. */
struct za_item {
	const char	*zi_path;
	size_t		zi_len;
	zr_name_t	zi_name;
	zr_name_t	zi_anchor;
	enum zr_diff	zi_kind;
};

/*
 * By path, which puts a parent before everything under it: a
 * directory's path is a prefix of its children's and a prefix sorts
 * first. The restores then run forwards and the removals backwards,
 * so that a directory is made before what goes inside it and emptied
 * before it is taken away.
 */
static int
za_bypath(const void *va, const void *vb)
{
	const struct za_item *a = va;
	const struct za_item *b = vb;
	size_t n = a->zi_len < b->zi_len ? a->zi_len : b->zi_len;
	int c = memcmp(a->zi_path, b->zi_path, n);

	if (c != 0)
		return (c);
	if (a->zi_len == b->zi_len)
		return (0);
	return (a->zi_len < b->zi_len ? -1 : 1);
}

/*
 * A path of the name table copied where an action can hold it: the
 * paths of a zr_action are not const, and the table's bytes are not
 * the repair's to hand out that way.
 */
struct za_pathbuf {
	unsigned char	*zb_buf;
	size_t		zb_cap;
};

static int
za_stash(struct za_pathbuf *b, const char *p, size_t len)
{
	unsigned char *n;

	if (len + 1 > b->zb_cap) {
		n = realloc(b->zb_buf, len + 1);
		if (n == NULL)
			return (-1);
		b->zb_buf = n;
		b->zb_cap = len + 1;
	}
	memcpy(b->zb_buf, p, len);
	b->zb_buf[len] = '\0';
	return (0);
}

/* The action the repair hands to the same cp, ln and rm as the rest. */
static void
za_made(struct zr_action *a, enum zr_act_kind kind, struct za_pathbuf *path,
    size_t len, struct za_pathbuf *arg, size_t arglen)
{
	memset(a, 0, sizeof (struct zr_action));
	a->za_kind = kind;
	a->za_path = path->zb_buf;
	a->za_pathlen = len;
	a->za_arg = arg != NULL ? arg->zb_buf : NULL;
	a->za_arglen = arg != NULL ? arglen : 0;
}

/*
 * One name made again out of the tree w, which is the side that has
 * it: its bytes, its type and its attributes, at the very same path.
 * That is one operation with two callers -- the repair, which puts a
 * name back as onto had it, and a choice of onto or from, which puts
 * a name where that side has it -- so it is written once. A dup is
 * what it is: the argument is the path itself, read out of the tree
 * the caller names rather than out of the manifest's from.
 */
static int
za_put_back(struct za_ctx *c, struct za_pathbuf *pb, const char *path,
    size_t len, const struct zr_walk *w, const char *what)
{
	struct zr_action a;

	if (za_stash(pb, path, len) != 0) {
		errno = ENOMEM;
		return (za_failp(c, (const unsigned char *)path, "memory"));
	}
	za_made(&a, ZR_ACT_DUP, pb, len, pb, len);
	if (za_path_ok(c, &a, a.za_path, a.za_pathlen) != 0)
		return (-1);
	return (za_do_cp(c, &a, w, what));
}

/*
 * One name made another name of the object at anchor, both of them
 * in the tree being written. The repair mends a torn pool with it and
 * a choice pools the names of a group with it.
 */
static int
za_link_onto(struct za_ctx *c, struct za_pathbuf *pb, const char *path,
    size_t len, struct za_pathbuf *ab, const char *anchor, size_t alen)
{
	struct zr_action a;

	if (za_stash(pb, path, len) != 0 || za_stash(ab, anchor, alen) != 0) {
		errno = ENOMEM;
		return (za_failp(c, (const unsigned char *)path, "memory"));
	}
	za_made(&a, ZR_ACT_LN, pb, len, ab, alen);
	if (za_path_ok(c, &a, a.za_path, a.za_pathlen) != 0)
		return (-1);
	return (za_do_ln(c, &a));
}

/* One name nothing expected, whatever kind of thing it turned out to be. */
static int
za_rm_one(struct za_ctx *c, const struct zr_action *a)
{
	const char *rel = za_rel(a->za_path);
	struct stat st;
	int isdir;

	if (fstatat(c->zc_rootfd, rel, &st, AT_SYMLINK_NOFOLLOW) != 0) {
		if (errno == ENOENT)
			return (0);
		return (za_fail(c, a, "stat"));
	}
	isdir = S_ISDIR(st.st_mode);
	if (unlinkat(c->zc_rootfd, rel, isdir ? AT_REMOVEDIR : 0) != 0 &&
	    errno != ENOENT)
		return (za_fail(c, a, isdir ? "rmdir" : "unlink"));
	if (c->zc_by == ZA_BY_CHOICE)
		c->zc_st->zs_dropped++;
	else
		c->zc_st->zs_removed++;
	return (0);
}

/* The stop flag, between two names of the repair. */
static int
za_stopped(struct za_ctx *c)
{
	if (zr_apply_stop == 0)
		return (0);
	if (c->zc_err != NULL && c->zc_errlen > 0)
		(void) snprintf(c->zc_err, c->zc_errlen, "interrupted");
	return (-1);
}

int
zr_apply_repair(const struct zr_verify_report *rep, const char *onto_root,
    const struct zr_names *names, const struct zr_walk *onto,
    struct zr_apply_stats *st, char *err, size_t errlen)
{
	static const unsigned char self[] = "(the repair)";
	struct zr_apply_stats spare;
	struct za_pathbuf pb, ab;
	struct za_item *items = NULL;
	const struct za_item *it;
	struct zr_action a;
	struct za_ctx c;
	zr_name_t *made = NULL;
	zr_name_t anchor;
	zr_pool_t po;
	const char *ap;
	size_t alen;
	uint32_t i, n;
	int rc = -1;

	memset(&c, 0, sizeof (struct za_ctx));
	memset(&pb, 0, sizeof (struct za_pathbuf));
	memset(&ab, 0, sizeof (struct za_pathbuf));
	c.zc_rootfd = -1;
	c.zc_by = ZA_BY_REPAIR;
	c.zc_onto = onto;
	c.zc_err = err;
	c.zc_errlen = errlen;
	c.zc_st = st != NULL ? st : &spare;
	memset(c.zc_st, 0, sizeof (struct zr_apply_stats));
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (rep == NULL || onto_root == NULL || names == NULL ||
	    onto == NULL) {
		errno = EINVAL;
		return (za_failp(&c, self, "arguments"));
	}
	n = rep->zv_ndiffs;
	if (n == 0)
		return (0);
	if (onto->zw_rootfd < 0) {
		errno = EINVAL;
		return (za_failpx(&c, self, "there is no onto tree to put "
		    "anything back from"));
	}
	items = malloc((size_t)n * sizeof (struct za_item));
	if (items == NULL) {
		errno = ENOMEM;
		return (za_failp(&c, self, "memory"));
	}
	for (i = 0; i < n; i++) {
		items[i].zi_name = rep->zv_diffs[i].zn_name;
		items[i].zi_anchor = rep->zv_diffs[i].zn_anchor;
		items[i].zi_kind = rep->zv_diffs[i].zn_kind;
		items[i].zi_len = 0;
		items[i].zi_path = zr_names_str(names, items[i].zi_name,
		    &items[i].zi_len);
		if (items[i].zi_path == NULL) {
			errno = EINVAL;
			(void) za_failpx(&c, self, "a name the table does not "
			    "hold");
			goto out;
		}
	}
	qsort(items, (size_t)n, sizeof (struct za_item), za_bypath);
	if (onto->zw_tree.zt_npools != 0) {
		made = malloc((size_t)onto->zw_tree.zt_npools *
		    sizeof (zr_name_t));
		if (made == NULL) {
			errno = ENOMEM;
			(void) za_failp(&c, self, "memory");
			goto out;
		}
		for (i = 0; i < onto->zw_tree.zt_npools; i++)
			made[i] = ZR_NAME_NONE;
	}
	if (za_root_copy(&c, onto_root) != 0) {
		errno = ENOMEM;
		(void) za_failp(&c, (const unsigned char *)onto_root,
		    "memory");
		goto out;
	}
	c.zc_buf = malloc(ZA_BUFSZ);
	if (c.zc_buf == NULL) {
		errno = ENOMEM;
		(void) za_failp(&c, (const unsigned char *)onto_root,
		    "memory");
		goto out;
	}
	c.zc_rootfd = open(onto_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
	    O_CLOEXEC);
	if (c.zc_rootfd < 0) {
		(void) za_failp(&c, (const unsigned char *)onto_root, "open");
		goto out;
	}
	/*
	 * First what nothing expected, children before parents, so
	 * that a directory somebody left behind is empty by the time
	 * its own turn comes and a name in the way of a restore is
	 * gone before the restore.
	 */
	for (i = n; i > 0; i--) {
		it = &items[i - 1];
		if (it->zi_kind != ZR_DF_EXTRA)
			continue;
		if (za_stopped(&c) != 0 ||
		    za_stash(&pb, it->zi_path, it->zi_len) != 0)
			goto out;
		za_made(&a, ZR_ACT_RM, &pb, it->zi_len, NULL, 0);
		if (za_path_ok(&c, &a, a.za_path, a.za_pathlen) != 0 ||
		    za_rm_one(&c, &a) != 0)
			goto out;
	}
	/*
	 * Then what onto had, parents before children. A name whose
	 * pool has another name the result still holds is linked to
	 * it rather than copied, so that putting one name back does
	 * not sever the pool; where the whole pool has to be made
	 * again, the first name of it is copied and the rest are
	 * linked to that one.
	 */
	for (i = 0; i < n; i++) {
		it = &items[i];
		if (it->zi_kind != ZR_DF_GONE &&
		    it->zi_kind != ZR_DF_CHANGED)
			continue;
		if (za_stopped(&c) != 0)
			goto out;
		anchor = it->zi_anchor;
		po = zr_tree_pool(&onto->zw_tree, it->zi_name);
		if (anchor == ZR_NAME_NONE && made != NULL &&
		    po != ZR_POOL_NONE)
			anchor = made[po];
		if (anchor != ZR_NAME_NONE) {
			ap = zr_names_str(names, anchor, &alen);
			if (ap == NULL) {
				errno = EINVAL;
				(void) za_failp(&c, self, "the anchor");
				goto out;
			}
			if (za_link_onto(&c, &pb, it->zi_path, it->zi_len, &ab,
			    ap, alen) != 0)
				goto out;
			continue;
		}
		if (za_put_back(&c, &pb, it->zi_path, it->zi_len, onto,
		    "restore") != 0)
			goto out;
		if (made != NULL && po != ZR_POOL_NONE)
			made[po] = it->zi_name;
	}
	/* And last the pools somebody tore, which are two names apiece. */
	for (i = 0; i < n; i++) {
		it = &items[i];
		if (it->zi_kind != ZR_DF_UNPOOLED ||
		    it->zi_anchor == ZR_NAME_NONE)
			continue;
		ap = zr_names_str(names, it->zi_anchor, &alen);
		if (za_stopped(&c) != 0)
			goto out;
		if (ap == NULL) {
			errno = EINVAL;
			(void) za_failp(&c, self, "the anchor");
			goto out;
		}
		if (za_link_onto(&c, &pb, it->zi_path, it->zi_len, &ab, ap,
		    alen) != 0)
			goto out;
	}
	rc = 0;
out:
	free(items);
	free(made);
	free(pb.zb_buf);
	free(ab.zb_buf);
	za_ctx_fini(&c);
	return (rc);
}

/* The message a failed self-check leaves, which names the first of them. */
static void
za_verdict(char *err, size_t errlen, const struct zr_parsed *m,
    const struct zr_verify_report *rep, const struct zr_names *names)
{
	const char *what;
	uint32_t i;
	int k;

	if (err == NULL || errlen == 0)
		return;
	if (rep->zv_count[ZR_OC_PENDING] != 0 ||
	    rep->zv_count[ZR_OC_DRIFTED] != 0) {
		i = rep->zv_count[ZR_OC_PENDING] != 0 ?
		    rep->zv_first[ZR_OC_PENDING] : rep->zv_first[ZR_OC_DRIFTED];
		(void) snprintf(err, errlen, "after the apply, %u pending and "
		    "%u drifted, first %s", rep->zv_count[ZR_OC_PENDING],
		    rep->zv_count[ZR_OC_DRIFTED],
		    (const char *)m->zp_actions[i].za_path);
		return;
	}
	for (k = 0; k < ZR_DF_COUNT; k++) {
		if (rep->zv_dcount[k] == 0)
			continue;
		what = zr_names_str(names, rep->zv_dfirst[k], NULL);
		(void) snprintf(err, errlen, "after the apply, %u name%s "
		    "outside the manifest the repair did not mend, first %s "
		    "%s", rep->zv_ndiffs, rep->zv_ndiffs == 1 ? "" : "s",
		    zr_diff_str((enum zr_diff)k), what != NULL ? what : "?");
		return;
	}
}

int
zr_apply_check(const struct zr_parsed *m, const char *onto_root,
    struct zr_names *names, struct zr_walk *onto, struct zr_walk *from,
    unsigned missing, int fix, struct zr_apply_stats *st, char *err,
    size_t errlen)
{
	struct zr_apply_stats spare;
	struct zr_verify_report rep;
	struct zr_oracle *o = NULL;
	struct zr_walk wr;
	int pass, walked = 0, rc = -1;

	if (st == NULL)
		st = &spare;
	memset(st, 0, sizeof (struct zr_apply_stats));
	memset(&rep, 0, sizeof (struct zr_verify_report));
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (m == NULL || onto_root == NULL || names == NULL || onto == NULL ||
	    from == NULL) {
		if (err != NULL && errlen > 0)
			(void) snprintf(err, errlen, "the check: arguments");
		return (-1);
	}
	/*
	 * Twice at most: once as the apply left the tree, and once
	 * more after a repair, which is the only thing between the
	 * two that writes.
	 */
	for (pass = 0; pass < 2; pass++) {
		if (zr_walk(onto_root, names, &wr, err, errlen) != 0)
			goto out;
		walked = 1;
		if (zr_oracle_init(&o, onto, from, &wr) != 0) {
			if (err != NULL && errlen > 0)
				(void) snprintf(err, errlen, "the check: out "
				    "of memory");
			goto out;
		}
		if (zr_verify(m, o, onto, from, &wr, missing, &rep, err,
		    errlen) != 0)
			goto out;
		if (pass != 0 || fix == 0 || rep.zv_ndiffs == 0)
			break;
		if (zr_apply_repair(&rep, onto_root, names, onto, st, err,
		    errlen) != 0)
			goto out;
		zr_verify_report_fini(&rep);
		zr_oracle_fini(o);
		o = NULL;
		zr_walk_fini(&wr);
		walked = 0;
	}
	if (rep.zv_count[ZR_OC_PENDING] != 0 ||
	    rep.zv_count[ZR_OC_DRIFTED] != 0 ||
	    (fix != 0 && rep.zv_ndiffs != 0)) {
		za_verdict(err, errlen, m, &rep, names);
		goto out;
	}
	rc = 0;
out:
	zr_verify_report_fini(&rep);
	if (o != NULL)
		zr_oracle_fini(o);
	if (walked != 0)
		zr_walk_fini(&wr);
	return (rc);
}

/*
 * ---------------------------------------------------------------
 * The resolution's choices. The manifest says what to do; the
 * resolution says which side won, and the actions are derived here.
 * A name that chose a side becomes that side's object at that same
 * name, or goes where the side has no such name; the names of one
 * group that chose one side are pooled as that side pools them; a
 * name that chose keep is not touched at all. Last come the
 * directory removals a conflict blocked, which go through when the
 * choices left the directory empty.
 * ---------------------------------------------------------------
 */

/* No earlier line of the document is this one's anchor. */
#define	ZA_NO_ANCHOR	((uint32_t)-1)

/*
 * What one line comes to, worked out before a byte is written: the
 * tree its choice names and the pool it has there, whether that tree
 * has the name at all, and which earlier line this one is pooled
 * with.
 */
struct za_pick {
	const struct zr_walk	*zk_side;	/* NULL for keep */
	int			zk_tree;	/* the oracle's 0 or 1 */
	zr_pool_t		zk_pool;	/* in that side's tree */
	uint32_t		zk_anchor;	/* an earlier line, or none */
	int			zk_drop;	/* the side has no such name */
};

/*
 * Does this tree hold that name, and in which of its pools? A name
 * the tree does not have, or has without a pool the walk kept
 * attributes for, is a name a choice of that side removes.
 */
static int
za_side_pool(const struct zr_walk *w, const unsigned char *path, size_t len,
    zr_pool_t *poolp)
{
	zr_name_t nm;
	zr_pool_t po;

	*poolp = ZR_POOL_NONE;
	nm = zr_names_lookup(w->zw_tree.zt_names, (const char *)path, len);
	if (nm == ZR_NAME_NONE)
		return (0);
	po = zr_tree_pool(&w->zw_tree, nm);
	if (po == ZR_POOL_NONE || po >= w->zw_nattrs)
		return (0);
	*poolp = po;
	return (1);
}

/*
 * Does the result already hold the chosen side's object at this
 * name? The oracle's word -- the type, the attributes and the bytes
 * of a regular file, and never the times -- asked of the result walk
 * this apply made before it wrote anything. That walk stays an
 * honest source of the answer while the apply runs, because nothing
 * here writes into an object that already exists: a copy unlinks the
 * name and makes a new one, a link only moves a name, and the one
 * thing kept in place, a directory that is already there, is a pool
 * of one name that no later line can ask about again.
 *
 * Returns 1 equal, 0 not, -1 when the comparison could not be made.
 */
static int
za_choice_done(struct za_ctx *c, struct zr_oracle *o, const struct zr_walk *rw,
    int tree, zr_pool_t spool, const unsigned char *path, size_t len)
{
	zr_pool_t rp;

	if (za_side_pool(rw, path, len, &rp) == 0)
		return (0);
	return (zr_oracle_equal(o, 2, rp, tree, spool, c->zc_err,
	    c->zc_errlen));
}

/* The first line a choice changed, which is what a failed check names. */
static void
za_first_line(struct za_ctx *c, uint32_t i)
{
	if (c->zc_st->zs_line == ZR_LINE_NONE || i < c->zc_st->zs_line)
		c->zc_st->zs_line = i;
}

/*
 * The removals the conflicts blocked, now that the choices have had
 * their say. In reverse manifest order, so a child directory is
 * tried before its parent: one that is gone was removed at applying1
 * and is nothing to do; one that is empty goes now; one that still
 * holds something stays, because the resolution cemented a name
 * under it or the person put one there, and that is a state and not
 * a fault.
 */
static int
za_late_rmdirs(struct za_ctx *c, const struct zr_parsed *m)
{
	const struct zr_action *a;
	uint32_t i;

	for (i = m->zp_nactions; i > 0; i--) {
		a = &m->zp_actions[i - 1];
		if (a->za_kind != ZR_ACT_RM || a->za_isdir == 0)
			continue;
		if (za_stopped(c) != 0 ||
		    za_path_ok(c, a, a->za_path, a->za_pathlen) != 0)
			return (-1);
		if (unlinkat(c->zc_rootfd, za_rel(a->za_path),
		    AT_REMOVEDIR) == 0) {
			c->zc_st->zs_latedirs++;
			continue;
		}
		if (errno == ENOENT)
			continue;
		if (errno == ENOTEMPTY || errno == EEXIST) {
			c->zc_st->zs_skipped++;
			continue;
		}
		return (za_failp(c, a->za_path, "rmdir"));
	}
	return (0);
}

/* Every line's pick, and the pooling that ties some of them together. */
static void
za_picks(const struct zr_resolution *res, struct za_pick *picks,
    struct zr_walk *onto, struct zr_walk *from)
{
	const struct zr_rline *l, *al;
	struct za_pick *p;
	uint32_t i, j;

	for (i = 0; i < res->zs_nlines; i++) {
		l = &res->zs_lines[i];
		p = &picks[i];
		p->zk_side = NULL;
		p->zk_tree = -1;
		p->zk_pool = ZR_POOL_NONE;
		p->zk_anchor = ZA_NO_ANCHOR;
		p->zk_drop = 0;
		if (l->zl_choice == ZR_CH_KEEP)
			continue;
		p->zk_side = l->zl_choice == ZR_CH_ONTO ? onto : from;
		p->zk_tree = l->zl_choice == ZR_CH_ONTO ? 0 : 1;
		if (za_side_pool(p->zk_side, l->zl_path, l->zl_pathlen,
		    &p->zk_pool) == 0) {
			p->zk_drop = 1;
			continue;
		}
		/*
		 * The anchor is the first line of this same group that
		 * chose this same side and that the side holds in this
		 * same pool. A drift line has no group and is never
		 * anybody's anchor, and never has one.
		 */
		if (l->zl_kind != ZR_RL_CONFLICT)
			continue;
		for (j = 0; j < i; j++) {
			al = &res->zs_lines[j];
			if (al->zl_kind != ZR_RL_CONFLICT ||
			    al->zl_group != l->zl_group ||
			    al->zl_choice != l->zl_choice ||
			    picks[j].zk_drop != 0 ||
			    picks[j].zk_pool != p->zk_pool)
				continue;
			p->zk_anchor = picks[j].zk_anchor == ZA_NO_ANCHOR ?
			    j : picks[j].zk_anchor;
			break;
		}
	}
}

int
zr_apply_choices(const struct zr_resolution *res, const struct zr_parsed *m,
    const char *root, struct zr_names *names, struct zr_walk *onto,
    struct zr_walk *from, struct zr_apply_stats *st, char *err, size_t errlen)
{
	static const unsigned char self[] = "(the choices)";
	struct zr_apply_stats spare;
	struct za_pathbuf pb, ab;
	struct za_pick *picks = NULL;
	struct zr_oracle *o = NULL;
	const struct zr_rline *l, *al;
	struct zr_action a;
	struct za_ctx c;
	struct zr_walk wr;
	uint64_t was;
	uint32_t i;
	unsigned int acted = 0;		/* the harness's gate counts these */
	int walked = 0, eq, rc = -1;

	memset(&c, 0, sizeof (struct za_ctx));
	memset(&pb, 0, sizeof (struct za_pathbuf));
	memset(&ab, 0, sizeof (struct za_pathbuf));
	memset(&a, 0, sizeof (struct zr_action));
	c.zc_rootfd = -1;
	c.zc_by = ZA_BY_CHOICE;
	c.zc_m = m;
	c.zc_from = from;
	c.zc_onto = onto;
	c.zc_err = err;
	c.zc_errlen = errlen;
	c.zc_st = st != NULL ? st : &spare;
	memset(c.zc_st, 0, sizeof (struct zr_apply_stats));
	c.zc_st->zs_line = ZR_LINE_NONE;
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (res == NULL || m == NULL || root == NULL || names == NULL ||
	    onto == NULL || from == NULL || onto->zw_rootfd < 0 ||
	    from->zw_rootfd < 0) {
		errno = EINVAL;
		return (za_failp(&c, self, "arguments"));
	}
	/*
	 * Every line read before any of them is acted on, so that a
	 * document that is not complete, or that spells a path this
	 * apply will not touch, leaves the tree exactly as it was.
	 */
	for (i = 0; i < res->zs_nlines; i++) {
		l = &res->zs_lines[i];
		a.za_path = l->zl_path;
		a.za_pathlen = l->zl_pathlen;
		if (l->zl_choice == ZR_CH_NONE)
			return (za_failpx(&c, l->zl_path, "the choice is "
			    "still \"-\" and the resolution is not complete"));
		if (za_path_ok(&c, &a, a.za_path, a.za_pathlen) != 0)
			return (-1);
	}
	/*
	 * The result as it stands before any of this is done, which is
	 * what the oracle is asked about. A walk that failed is still a
	 * walk to finalise, which is why the flag is set first.
	 */
	walked = 1;
	if (zr_walk(root, names, &wr, err, errlen) != 0)
		goto out;
	if (zr_oracle_init(&o, onto, from, &wr) != 0) {
		(void) za_failpx(&c, self, "the two sides and the result do "
		    "not make an oracle");
		goto out;
	}
	if (res->zs_nlines != 0) {
		picks = malloc((size_t)res->zs_nlines *
		    sizeof (struct za_pick));
		if (picks == NULL) {
			errno = ENOMEM;
			(void) za_failp(&c, self, "memory");
			goto out;
		}
		za_picks(res, picks, onto, from);
	}
	if (za_root_copy(&c, root) != 0) {
		errno = ENOMEM;
		(void) za_failp(&c, (const unsigned char *)root, "memory");
		goto out;
	}
	c.zc_buf = malloc(ZA_BUFSZ);
	if (c.zc_buf == NULL) {
		errno = ENOMEM;
		(void) za_failp(&c, (const unsigned char *)root, "memory");
		goto out;
	}
	c.zc_rootfd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
	    O_CLOEXEC);
	if (c.zc_rootfd < 0) {
		(void) za_failp(&c, (const unsigned char *)root, "open");
		goto out;
	}
	/*
	 * First what the choices make, in document order, which is the
	 * walk's, so that a directory is there before the names under
	 * it. A name already holding the side's object is left alone,
	 * and so is one already pooled where the group wants it.
	 */
	for (i = 0; i < res->zs_nlines; i++) {
		l = &res->zs_lines[i];
		if (za_stopped(&c) != 0)
			goto out;
		if (l->zl_choice == ZR_CH_KEEP) {
			c.zc_st->zs_kept++;
			continue;
		}
		if (picks[i].zk_drop != 0)
			continue;
		if (picks[i].zk_anchor != ZA_NO_ANCHOR) {
			al = &res->zs_lines[picks[i].zk_anchor];
			if (za_pooled(&c, l->zl_path, al->zl_path) != 0) {
				c.zc_st->zs_skipped++;
				continue;
			}
			za_first_line(&c, i);
			if (++acted == za_cpause_at)
				(void) raise(SIGSTOP);
			if (za_link_onto(&c, &pb, (const char *)l->zl_path,
			    l->zl_pathlen, &ab, (const char *)al->zl_path,
			    al->zl_pathlen) != 0)
				goto out;
			continue;
		}
		eq = za_choice_done(&c, o, &wr, picks[i].zk_tree,
		    picks[i].zk_pool, l->zl_path, l->zl_pathlen);
		if (eq < 0)
			goto out;
		if (eq > 0) {
			c.zc_st->zs_skipped++;
			continue;
		}
		za_first_line(&c, i);
		if (++acted == za_cpause_at)
			(void) raise(SIGSTOP);
		if (za_put_back(&c, &pb, (const char *)l->zl_path,
		    l->zl_pathlen, picks[i].zk_side,
		    zr_choice_str(l->zl_choice)) != 0)
			goto out;
	}
	/*
	 * Then what the choices take away, backwards, so that a
	 * directory neither side has is empty of the names neither
	 * side has by the time its own turn comes.
	 */
	for (i = res->zs_nlines; i > 0; i--) {
		l = &res->zs_lines[i - 1];
		if (picks[i - 1].zk_drop == 0)
			continue;
		if (za_stopped(&c) != 0)
			goto out;
		if (za_stash(&pb, (const char *)l->zl_path,
		    l->zl_pathlen) != 0) {
			errno = ENOMEM;
			(void) za_failp(&c, l->zl_path, "memory");
			goto out;
		}
		za_made(&a, ZR_ACT_RM, &pb, l->zl_pathlen, NULL, 0);
		was = c.zc_st->zs_dropped;
		if (++acted == za_cpause_at)
			(void) raise(SIGSTOP);
		if (za_rm_one(&c, &a) != 0)
			goto out;
		if (c.zc_st->zs_dropped == was)
			c.zc_st->zs_skipped++;
		else
			za_first_line(&c, i - 1);
	}
	if (za_late_rmdirs(&c, m) != 0)
		goto out;
	rc = 0;
out:
	free(picks);
	free(pb.zb_buf);
	free(ab.zb_buf);
	if (o != NULL)
		zr_oracle_fini(o);
	if (walked != 0)
		zr_walk_fini(&wr);
	za_ctx_fini(&c);
	return (rc);
}
