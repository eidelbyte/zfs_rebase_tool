/*
 * The walk tests: the probe fixture built as three directory trees
 * and walked back into one shared name table, then a tree the test
 * builds itself for the corners a fixture cannot describe -- depth,
 * odd bytes in a leaf, an empty directory, a fifo, a symlink, links
 * across directories, an extended attribute, the .zfs the root must
 * not walk into, and a directory built backwards, whose ids must
 * still ascend in name order. Crossing a mount point is not tested here:
 * making a mount needs root, so that cell stays deferred to the box
 * probe. So is ZW18, an ACL present: the two ACL models differ and
 * setting one on macOS without root proves nothing about ZFS. What
 * does not need a tree at all is ZW30, zr_acl_equal, which is
 * checked here on ACLs built in memory. ZW31 and ZW32, the
 * generation number and the change time the pruning compares, ride
 * along with ZW20 on every pool of the probe trees.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
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

#if defined(__APPLE__)
#include <sys/xattr.h>
#elif defined(__FreeBSD__)
#include <sys/extattr.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

#include "fixture.h"
#include "name.h"
#include "walk.h"

/*
 * A template under TMPDIR, or /tmp without it: the box's /tmp may be
 * a tmpfs, which has no extended attributes, and the tests that
 * build attributes must be pointed at a filesystem that has them.
 */
static void
tmp_template(char *buf, size_t len, const char *leaf)
{
	const char *d = getenv("TMPDIR");

	(void) snprintf(buf, len, "%s/%s", d != NULL && d[0] != '\0' ?
	    d : "/tmp", leaf);
}

#define	PATHMAX		1024
#define	WALK_MIN	16
#define	DEEP		64

/*
 * The two fields the pruning reads, spelled as walk.c spells them:
 * POSIX's st_ctim, which macOS calls st_ctimespec, and st_gen, which
 * only the BSDs have.
 */
#if defined(__APPLE__)
#define	ST_CTIM(st)	((st).st_ctimespec)
#else
#define	ST_CTIM(st)	((st).st_ctim)
#endif
#if defined(__FreeBSD__) || defined(__APPLE__)
#define	ST_GEN(st)	((uint64_t)(st).st_gen)
#else
#define	ST_GEN(st)	((uint64_t)0)
#endif

/* a space, a backslash, a hash, a control byte and two UTF-8 bytes */
#define	ODD		"o \\#\001\303\251x"

/*
 * ZW29: the entries of one directory, in the order they must be
 * interned in -- bytewise, a shorter name before the longer one it
 * is a prefix of, and a byte above 0x7f above every ASCII one, which
 * is where a signed char would have put it first. They are created
 * in the reverse of this.
 */
#define	ORD_N		6
static const char *const ord_name[ORD_N] = {
	"a", "a0", "ab", "b", "z", "\303\251"
};

#define	XA1		"user.zra"
#define	XA2		"user.zrtest"
#define	XA2VAL		"z\000r\377"
#define	XA2LEN		4

#define	DEVMASK	(((((uint64_t)1 << (sizeof (dev_t) * 8 - 1))) << 1) - 1)

static int checks;

#define	CHECK(x)							\
	do {								\
		checks++;						\
		if (!(x)) {						\
			printf("%s:%d: check failed: %s\n", __FILE__,	\
			    __LINE__, #x);				\
			exit(1);					\
		}							\
	} while (0)

/* One row of what a walked tree must hold. */
struct ent {
	const char	*e_path;
	zr_type_t	e_type;
	const char	*e_tok;		/* a file's token, else NULL */
	uint32_t	e_nlink;
};

static const struct ent exp_base[] = {
	{ "/", ZR_T_DIR, NULL, 1 },
	{ "/a", ZR_T_FILE, "x", 1 },
	{ "/b", ZR_T_FILE, "y", 1 },
	{ "/h1", ZR_T_FILE, "h", 2 },
	{ "/h2", ZR_T_FILE, "h", 2 },
	{ "/d", ZR_T_DIR, NULL, 1 },
	{ "/d/f", ZR_T_FILE, "f", 1 },
	{ "/keep", ZR_T_DIR, NULL, 1 },
	{ "/keep/k", ZR_T_FILE, "k", 1 }
};

static const struct ent exp_from[] = {
	{ "/", ZR_T_DIR, NULL, 1 },
	{ "/a", ZR_T_FILE, "a2", 1 },
	{ "/h1", ZR_T_FILE, "h2", 3 },
	{ "/h2", ZR_T_FILE, "h2", 3 },
	{ "/h3", ZR_T_FILE, "h2", 3 },
	{ "/e", ZR_T_DIR, NULL, 1 },
	{ "/e/f", ZR_T_FILE, "f", 1 },
	{ "/keep", ZR_T_DIR, NULL, 1 },
	{ "/keep/k", ZR_T_FILE, "k", 1 },
	{ "/n", ZR_T_FILE, "n", 1 }
};

static const struct ent exp_onto[] = {
	{ "/", ZR_T_DIR, NULL, 1 },
	{ "/a", ZR_T_FILE, "a3", 1 },
	{ "/b", ZR_T_FILE, "y", 1 },
	{ "/h1", ZR_T_FILE, "h", 2 },
	{ "/h2", ZR_T_FILE, "h", 2 },
	{ "/d", ZR_T_DIR, NULL, 1 },
	{ "/d/f", ZR_T_FILE, "f", 1 },
	{ "/keep", ZR_T_DIR, NULL, 1 },
	{ "/keep/k", ZR_T_FILE, "k2", 1 }
};

/* Every name a scan found under one root, parents before children. */
struct scan {
	char	**s_path;
	int	*s_isdir;
	int	s_n;
	int	s_cap;
};

static void
join(char *out, size_t outlen, const char *a, const char *b)
{
	int n;

	n = snprintf(out, outlen, "%s%s", a, b);
	CHECK(n > 0 && (size_t)n < outlen);
}

static void
scan_push(struct scan *s, const char *rel, int isdir)
{
	char **np;
	int *ni, cap;

	if (s->s_n == s->s_cap) {
		cap = s->s_cap == 0 ? WALK_MIN : s->s_cap * 2;
		np = realloc(s->s_path, (size_t)cap * sizeof (char *));
		CHECK(np != NULL);
		s->s_path = np;
		ni = realloc(s->s_isdir, (size_t)cap * sizeof (int));
		CHECK(ni != NULL);
		s->s_isdir = ni;
		s->s_cap = cap;
	}
	s->s_path[s->s_n] = malloc(strlen(rel) + 1);
	CHECK(s->s_path[s->s_n] != NULL);
	(void) strcpy(s->s_path[s->s_n], rel);
	s->s_isdir[s->s_n] = isdir;
	s->s_n++;
}

static void
scan_dir(struct scan *s, const char *root, const char *rel)
{
	char full[PATHMAX], child[PATHMAX];
	struct dirent *de;
	struct stat st;
	DIR *d;

	join(full, sizeof (full), root, rel);
	d = opendir(full);
	CHECK(d != NULL);
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		join(child, sizeof (child), rel, "/");
		join(child, sizeof (child), child, de->d_name);
		join(full, sizeof (full), root, child);
		CHECK(lstat(full, &st) == 0);
		scan_push(s, child, S_ISDIR(st.st_mode) ? 1 : 0);
	}
	CHECK(closedir(d) == 0);
}

/* Breadth first and never recursing, as check_fixture.c does it. */
static void
scan_tree(struct scan *s, const char *root)
{
	int i;

	memset(s, 0, sizeof (*s));
	scan_dir(s, root, "");
	for (i = 0; i < s->s_n; i++) {
		if (s->s_isdir[i])
			scan_dir(s, root, s->s_path[i]);
	}
}

/* Children before parents, which is the scan order reversed. */
static void
rmtree(const char *root)
{
	char full[PATHMAX];
	struct scan s;
	int i;

	scan_tree(&s, root);
	for (i = s.s_n - 1; i >= 0; i--) {
		join(full, sizeof (full), root, s.s_path[i]);
		if (s.s_isdir[i])
			CHECK(rmdir(full) == 0);
		else
			CHECK(unlink(full) == 0);
		free(s.s_path[i]);
	}
	free(s.s_path);
	free(s.s_isdir);
	CHECK(rmdir(root) == 0);
}

static zr_name_t
nameof(struct zr_names *ns, const char *path)
{
	zr_name_t nm;

	nm = zr_names_lookup(ns, path, strlen(path));
	if (nm == ZR_NAME_NONE)
		printf("  no such name: %s\n", path);
	CHECK(nm != ZR_NAME_NONE);
	return (nm);
}

static zr_pool_t
poolof(const struct zr_walk *w, struct zr_names *ns, const char *path)
{
	zr_pool_t p;

	p = zr_tree_pool(&w->zw_tree, nameof(ns, path));
	if (p == ZR_POOL_NONE)
		printf("  no pool for: %s\n", path);
	CHECK(p != ZR_POOL_NONE);
	return (p);
}

/* The bytes behind one name, read through the walk's own root. */
static void
check_token(const struct zr_walk *w, struct zr_names *ns, const char *path,
    const char *tok)
{
	char buf[256];
	ssize_t n;
	size_t want;
	int fd;

	want = strlen(tok) + 1;
	CHECK(want < sizeof (buf));
	fd = zr_walk_openat(w, nameof(ns, path), O_RDONLY);
	CHECK(fd >= 0);
	n = read(fd, buf, sizeof (buf));
	CHECK(close(fd) == 0);
	CHECK(n >= 0 && (size_t)n == want);
	CHECK(memcmp(buf, tok, want - 1) == 0);
	CHECK(buf[want - 1] == '\n');
}

/*
 * ZW20, ZW31 and ZW32: what the pool recorded is what a direct lstat
 * says, the generation number and the change time among it.
 */
static void
check_attr(const struct zr_walk *w, struct zr_names *ns, const char *root,
    const char *path)
{
	char full[PATHMAX];
	const struct zr_attr *at;
	struct stat st;

	at = &w->zw_attrs[poolof(w, ns, path)];
	join(full, sizeof (full), root, path);
	CHECK(lstat(full, &st) == 0);
	CHECK(at->za_mode == st.st_mode);
	CHECK(at->za_uid == st.st_uid);
	CHECK(at->za_gid == st.st_gid);
	CHECK(at->za_size == (uint64_t)st.st_size);
	CHECK(at->za_rdev == ((uint64_t)st.st_rdev & DEVMASK));
#if defined(__FreeBSD__) || defined(__APPLE__)
	CHECK(at->za_flags == (uint32_t)st.st_flags);
#else
	CHECK(at->za_flags == 0);
#endif
	CHECK(at->za_gen == ST_GEN(st));		/* ZW31 */
	CHECK(at->za_ctime.tv_sec == ST_CTIM(st).tv_sec);	/* ZW32 */
	CHECK(at->za_ctime.tv_nsec == ST_CTIM(st).tv_nsec);
}

/*
 * One built tree walked back: ZW1 a file with one name, ZW2 a
 * directory as a one-name pool that the walk descended, ZW8 the
 * hardlink pair, ZW20 the attributes, ZW25 the root as the name "/".
 */
static void
check_probe_tree(const char *root, struct zr_names *ns, const struct ent *tbl,
    int n, uint32_t npools)
{
	char err[256];
	struct zr_walk w;
	const struct zr_pool *p;
	int i;

	err[0] = '\0';
	if (zr_walk(root, ns, &w, err, sizeof (err)) != 0)
		printf("  walk: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(w.zw_tree.zt_sealed != 0);
	CHECK(zr_tree_verify(&w.zw_tree, err, sizeof (err)) == 0);
	CHECK(w.zw_tree.zt_npools == npools);
	CHECK(w.zw_nattrs == npools);
	CHECK(w.zw_rootfd >= 0);
	p = &w.zw_tree.zt_pools[poolof(&w, ns, "/")];
	CHECK(p->zp_type == ZR_T_DIR);
	CHECK(p->zp_nnames == 1);
	for (i = 0; i < n; i++) {
		p = &w.zw_tree.zt_pools[poolof(&w, ns, tbl[i].e_path)];
		CHECK(p->zp_type == tbl[i].e_type);
		CHECK(p->zp_nnames == tbl[i].e_nlink);
		CHECK(p->zp_nlink == tbl[i].e_nlink);
		check_attr(&w, ns, root, tbl[i].e_path);
		if (tbl[i].e_tok == NULL)
			continue;
		CHECK(w.zw_attrs[zr_tree_pool(&w.zw_tree,
		    nameof(ns, tbl[i].e_path))].za_size ==
		    (uint64_t)strlen(tbl[i].e_tok) + 1);
		check_token(&w, ns, tbl[i].e_path, tbl[i].e_tok);
	}
	/* ZW8: the pool of a hardlink is one pool under both names */
	CHECK(poolof(&w, ns, "/h1") == poolof(&w, ns, "/h2"));
	zr_walk_fini(&w);
	CHECK(w.zw_attrs == NULL);
	CHECK(w.zw_rootfd == -1);
}

static void
write_at(int dfd, const char *name, const char *tok)
{
	size_t len;
	int fd;

	len = strlen(tok);
	fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL, 0644);
	CHECK(fd >= 0);
	CHECK(write(fd, tok, len) == (ssize_t)len);
	CHECK(write(fd, "\n", 1) == 1);
	CHECK(close(fd) == 0);
}

/*
 * Set one extended attribute in the platform's user namespace. The
 * name the walk reports is "user.NAME" everywhere, which on FreeBSD
 * is the namespace and the bare name put back together.
 */
static int
setx(const char *path, const char *name, const void *val, size_t len)
{
#if defined(__APPLE__)
	return (setxattr(path, name, val, len, 0, XATTR_NOFOLLOW));
#elif defined(__FreeBSD__)
	ssize_t n;

	n = extattr_set_link(path, EXTATTR_NAMESPACE_USER, name + 5, val, len);
	return (n == (ssize_t)len ? 0 : -1);
#elif defined(__linux__)
	return (lsetxattr(path, name, val, len, 0));
#else
	(void) path;
	(void) name;
	(void) val;
	(void) len;
	return (-1);
#endif
}

/* The path of the deepest file, which is also what the walk interns. */
static void
deep_path(char *out, size_t outlen)
{
	size_t pos;
	int i, n;

	pos = 0;
	for (i = 0; i < DEEP; i++) {
		n = snprintf(out + pos, outlen - pos, "/d%02d", i);
		CHECK(n > 0 && (size_t)n < outlen - pos);
		pos += (size_t)n;
	}
	n = snprintf(out + pos, outlen - pos, "/leaf");
	CHECK(n > 0 && (size_t)n < outlen - pos);
}

/*
 * The tree the test builds itself. Every leaf here answers a cell
 * the probe fixture cannot reach.
 */
static void
build_odd(const char *root)
{
	char full[PATHMAX];
	int rootfd, fd, next, i;
	char nm[8];

	rootfd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(rootfd >= 0);

	/* ZW10: a chain 64 directories deep, built one openat at a time */
	fd = openat(rootfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	for (i = 0; i < DEEP; i++) {
		(void) snprintf(nm, sizeof (nm), "d%02d", i);
		CHECK(mkdirat(fd, nm, 0755) == 0);
		next = openat(fd, nm, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		CHECK(next >= 0);
		CHECK(close(fd) == 0);
		fd = next;
	}
	write_at(fd, "leaf", "deep");
	CHECK(close(fd) == 0);

	/* ZW13 an empty directory, ZW3 and ZW23 a dangling symlink */
	CHECK(mkdirat(rootfd, "empty", 0755) == 0);
	CHECK(symlinkat("no/such/target", rootfd, "sym") == 0);

	/* ZW11: the odd bytes, in a directory name and in a file name */
	CHECK(mkdirat(rootfd, ODD, 0755) == 0);
	fd = openat(rootfd, ODD, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	write_at(fd, ODD, "odd");
	CHECK(close(fd) == 0);

	/* ZW6: a fifo, which is a pool with no bytes */
	join(full, sizeof (full), root, "/fifo");
	CHECK(mkfifo(full, 0644) == 0);

	/* ZW9: one file under three names in three directories */
	CHECK(mkdirat(rootfd, "la", 0755) == 0);
	CHECK(mkdirat(rootfd, "lb", 0755) == 0);
	CHECK(mkdirat(rootfd, "lc", 0755) == 0);
	fd = openat(rootfd, "la", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	write_at(fd, "f", "three");
	CHECK(close(fd) == 0);
	CHECK(linkat(rootfd, "la/f", rootfd, "lb/f", 0) == 0);
	CHECK(linkat(rootfd, "la/f", rootfd, "lc/f", 0) == 0);

	/* ZW15 and ZW17: an empty value and a binary one, on one file */
	write_at(rootfd, "x", "xattrs");
	join(full, sizeof (full), root, "/x");
	CHECK(setx(full, XA2, XA2VAL, XA2LEN) == 0);
	CHECK(setx(full, XA1, "", 0) == 0);

	/* ZW28: the root's .zfs is skipped, one below it is not */
	CHECK(mkdirat(rootfd, ".zfs", 0755) == 0);
	CHECK(mkdirat(rootfd, "la/.zfs", 0755) == 0);

	/*
	 * ZW29: one directory whose entries are created backwards, so
	 * that a filesystem handing readdir the creation order gives
	 * the walk the reverse of the order it must intern them in.
	 */
	CHECK(mkdirat(rootfd, "ord", 0755) == 0);
	fd = openat(rootfd, "ord", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(fd >= 0);
	for (i = ORD_N - 1; i >= 0; i--)
		write_at(fd, ord_name[i], "ord");
	CHECK(close(fd) == 0);

	CHECK(close(rootfd) == 0);
}

/* ZW10: the deep file is there, with the path the stack built. */
static void
check_deep(const struct zr_walk *w, struct zr_names *ns)
{
	char path[PATHMAX];
	const struct zr_pool *p;

	deep_path(path, sizeof (path));
	p = &w->zw_tree.zt_pools[poolof(w, ns, path)];
	CHECK(p->zp_type == ZR_T_FILE);
	CHECK(p->zp_nnames == 1);
	check_token(w, ns, path, "deep");
}

/* ZW13 an empty directory, ZW2 a directory pool of one name. */
static void
check_empty(const struct zr_walk *w, struct zr_names *ns)
{
	const struct zr_pool *p;

	p = &w->zw_tree.zt_pools[poolof(w, ns, "/empty")];
	CHECK(p->zp_type == ZR_T_DIR);
	CHECK(p->zp_nnames == 1);
	/* ZW2: st_nlink of a directory is not a count of its names */
	CHECK(p->zp_nlink == 1);
	CHECK(w->zw_tree.zt_pools[poolof(w, ns, "/la")].zp_nlink == 1);
	CHECK(zr_tree_pool(&w->zw_tree,
	    zr_names_lookup(ns, "/empty/x", 8)) == ZR_POOL_NONE);
}

/* ZW3 and ZW23: the target of a dangling symlink reads back. */
static void
check_symlink(const struct zr_walk *w, struct zr_names *ns)
{
	const struct zr_attr *at;
	zr_pool_t pool;

	pool = poolof(w, ns, "/sym");
	CHECK(w->zw_tree.zt_pools[pool].zp_type == ZR_T_SYMLINK);
	at = &w->zw_attrs[pool];
	CHECK(at->za_target != NULL);
	CHECK(strcmp(at->za_target, "no/such/target") == 0);
	CHECK(at->za_size == (uint64_t)strlen("no/such/target"));
}

/* ZW11: the odd bytes survive readdir, the intern and the lookup. */
static void
check_odd(const struct zr_walk *w, struct zr_names *ns)
{
	const char *p;
	size_t len;
	zr_name_t nm;

	nm = nameof(ns, "/" ODD);
	CHECK(w->zw_tree.zt_pools[zr_tree_pool(&w->zw_tree, nm)].zp_type ==
	    ZR_T_DIR);
	nm = nameof(ns, "/" ODD "/" ODD);
	p = zr_names_str(ns, nm, &len);
	CHECK(p != NULL);
	CHECK(len == 2 * strlen(ODD) + 2);
	CHECK(memcmp(p + 1, ODD, strlen(ODD)) == 0);
	CHECK(p[1 + (int)strlen(ODD)] == '/');
	CHECK(w->zw_tree.zt_pools[zr_tree_pool(&w->zw_tree, nm)].zp_type ==
	    ZR_T_FILE);
	check_token(w, ns, "/" ODD "/" ODD, "odd");
}

/* ZW6: a fifo is a pool of its own, typed as one. */
static void
check_fifo(const struct zr_walk *w, struct zr_names *ns)
{
	const struct zr_pool *p;

	p = &w->zw_tree.zt_pools[poolof(w, ns, "/fifo")];
	CHECK(p->zp_type == ZR_T_FIFO);
	CHECK(p->zp_nnames == 1);
	CHECK(p->zp_nlink == 1);
}

/* ZW9: three names in three directories reach one pool. */
static void
check_links(const struct zr_walk *w, struct zr_names *ns)
{
	const struct zr_pool *p;
	zr_pool_t pool;

	pool = poolof(w, ns, "/la/f");
	CHECK(poolof(w, ns, "/lb/f") == pool);
	CHECK(poolof(w, ns, "/lc/f") == pool);
	p = &w->zw_tree.zt_pools[pool];
	CHECK(p->zp_nnames == 3);
	CHECK(p->zp_nlink == 3);
	CHECK(p->zp_names[0] < p->zp_names[1]);
	CHECK(p->zp_names[1] < p->zp_names[2]);
	check_token(w, ns, "/lb/f", "three");
}

/*
 * ZW14 a file with no attributes and no ACL, ZW15 two attributes in
 * name order rather than the order they were set in, ZW17 an empty
 * value beside a binary one. The order is only half proven here:
 * macOS hands the list back in bytewise order whatever order it was
 * written in, so the sort has nothing to do until FreeBSD, where
 * extattr_list_link returns the order the attributes are stored in.
 */
static void
check_xattrs(const struct zr_walk *w, struct zr_names *ns)
{
	const struct zr_attr *at;
	char path[PATHMAX];

	deep_path(path, sizeof (path));
	at = &w->zw_attrs[poolof(w, ns, path)];
	CHECK(at->za_nxattrs == 0);
	CHECK(at->za_xattrs == NULL);
	CHECK(at->za_acl == NULL);		/* ZW19 */
	CHECK(at->za_dacl == NULL);
	CHECK(at->za_target == NULL);

	at = &w->zw_attrs[poolof(w, ns, "/x")];
	CHECK(at->za_nxattrs == 2);
	CHECK(strcmp(at->za_xattrs[0].zx_name, XA1) == 0);
	CHECK(at->za_xattrs[0].zx_len == 0);
	CHECK(at->za_xattrs[0].zx_value != NULL);
	CHECK(strcmp(at->za_xattrs[1].zx_name, XA2) == 0);
	CHECK(at->za_xattrs[1].zx_len == XA2LEN);
	CHECK(memcmp(at->za_xattrs[1].zx_value, XA2VAL, XA2LEN) == 0);
}

#if defined(__FreeBSD__)

/*
 * One everyone@ entry appended to *ap, with the entry type, one
 * permission bit and one inheritance flag. The everyone@ tag is
 * what brands the ACL NFSv4, so the entry type and the flags are
 * accepted after it and not before.
 */
static void
nfs4_entry(acl_t *ap, acl_entry_type_t etype, acl_perm_t perm,
    acl_flag_t flag)
{
	acl_permset_t ps;
	acl_flagset_t fs;
	acl_entry_t e;

	CHECK(acl_create_entry(ap, &e) == 0);
	CHECK(acl_set_tag_type(e, ACL_EVERYONE) == 0);
	CHECK(acl_set_entry_type_np(e, etype) == 0);
	CHECK(acl_get_permset(e, &ps) == 0);
	CHECK(acl_clear_perms(ps) == 0);
	CHECK(acl_add_perm(ps, perm) == 0);
	CHECK(acl_set_permset(e, ps) == 0);
	CHECK(acl_get_flagset_np(e, &fs) == 0);
	CHECK(acl_clear_flags_np(fs) == 0);
	if (flag != 0)
		CHECK(acl_add_flag_np(fs, flag) == 0);
	CHECK(acl_set_flagset_np(e, fs) == 0);
}

/*
 * An NFSv4 ACL made in memory, no filesystem touched: shape 0 is an
 * inheriting allow followed by a deny, shape 1 is the same two the
 * other way round, and shape 2 is the allow alone.
 */
static zr_acl_t
nfs4_acl(int shape)
{
	acl_t a;

	a = acl_init(2);
	CHECK(a != NULL);
	if (shape == 1) {
		nfs4_entry(&a, ACL_ENTRY_TYPE_DENY, ACL_WRITE_DATA, 0);
		nfs4_entry(&a, ACL_ENTRY_TYPE_ALLOW, ACL_READ_DATA,
		    ACL_ENTRY_FILE_INHERIT);
		return (a);
	}
	nfs4_entry(&a, ACL_ENTRY_TYPE_ALLOW, ACL_READ_DATA,
	    ACL_ENTRY_FILE_INHERIT);
	if (shape == 0)
		nfs4_entry(&a, ACL_ENTRY_TYPE_DENY, ACL_WRITE_DATA, 0);
	return (a);
}

#endif	/* __FreeBSD__ */

/*
 * ZW30: ACL equality at its edges. Absent against absent is equal,
 * absent against present is not, two alike are equal and two unlike
 * are not, and a NULL is safe to free. On FreeBSD an ACL is the
 * acl_t itself, so two more things are asked of it that no text
 * comparison could be asked: that a shorter list is not equal to
 * the longer one it prefixes, and that the same two entries in the
 * other order are a different ACL, which they are, an NFSv4 ACL
 * being read in order. Those ACLs are built in memory, so this
 * needs no ACL-carrying filesystem and runs wherever the FreeBSD
 * build does. An ACL read off a real pool and written back is ZW18
 * and ZC9, deferred to the box (issue attr-cells).
 */
static void
check_acl_equal(void)
{
#if defined(__FreeBSD__)
	zr_acl_t a, b, c, d;

	CHECK(zr_acl_equal(NULL, NULL) == 1);
	a = nfs4_acl(0);
	b = nfs4_acl(0);
	c = nfs4_acl(1);
	d = nfs4_acl(2);
	CHECK(zr_acl_equal(NULL, a) == 0);
	CHECK(zr_acl_equal(a, NULL) == 0);
	CHECK(zr_acl_equal(a, b) == 1);
	CHECK(zr_acl_equal(a, c) == 0);		/* order is meaning */
	CHECK(zr_acl_equal(a, d) == 0);		/* the shorter list */
	CHECK(zr_acl_equal(d, a) == 0);
	/* again, so that the entry cursor is shown to reset */
	CHECK(zr_acl_equal(a, b) == 1);
	zr_acl_free(a);
	zr_acl_free(b);
	zr_acl_free(c);
	zr_acl_free(d);
#else
	char one[] = "user::rw-\n";
	char two[] = "user::rw-\n";
	char three[] = "user::r--\n";

	CHECK(zr_acl_equal(NULL, NULL) == 1);
	CHECK(zr_acl_equal(NULL, one) == 0);
	CHECK(zr_acl_equal(one, NULL) == 0);
	CHECK(zr_acl_equal(one, two) == 1);
	CHECK(zr_acl_equal(one, three) == 0);
	CHECK(zr_acl_equal(one, one) == 1);
#endif
	zr_acl_free(NULL);
}

/*
 * ZW29: the ids of one directory's names ascend in name order, not
 * in the order the entries were created, because the walk sorts a
 * directory before it interns any of it. Here they are consecutive
 * as well: every entry of ord is a file, so nothing is descended
 * between one name and the next.
 */
static void
check_order(const struct zr_walk *w, struct zr_names *ns)
{
	char path[PATHMAX];
	zr_name_t nm, prev;
	int i;

	prev = ZR_NAME_NONE;
	for (i = 0; i < ORD_N; i++) {
		join(path, sizeof (path), "/ord/", ord_name[i]);
		nm = nameof(ns, path);
		CHECK(zr_tree_pool(&w->zw_tree, nm) != ZR_POOL_NONE);
		if (i > 0)
			CHECK(nm == prev + 1);
		prev = nm;
	}
}

/* ZW28: ZFS's control directory is not a name of the tree. */
static void
check_dotzfs(const struct zr_walk *w, struct zr_names *ns)
{
	CHECK(zr_names_lookup(ns, "/.zfs", 5) == ZR_NAME_NONE);
	CHECK(w->zw_tree.zt_pools[poolof(w, ns, "/la/.zfs")].zp_type ==
	    ZR_T_DIR);
}

/*
 * The three probe trees, built as directories and walked back into
 * one shared name table, the way the tool will read three snapshots.
 */
static void
check_probe(const char *root)
{
	char dir[PATHMAX], err[256];
	struct zr_fixture *fx = NULL;
	struct zr_names *ns;

	err[0] = '\0';
	if (zr_fixture_load("tests/fixtures/probe.zrt", &fx, err,
	    sizeof (err)) != 0)
		printf("  load: %s\n", err);
	CHECK(fx != NULL);
	ns = zr_names_create();
	CHECK(ns != NULL);

	join(dir, sizeof (dir), root, "/base");
	CHECK(mkdir(dir, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_BASE, dir) == 0);
	check_probe_tree(dir, ns, exp_base, (int)(sizeof (exp_base) /
	    sizeof (exp_base[0])), 8);

	join(dir, sizeof (dir), root, "/from");
	CHECK(mkdir(dir, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_FROM, dir) == 0);
	check_probe_tree(dir, ns, exp_from, (int)(sizeof (exp_from) /
	    sizeof (exp_from[0])), 8);

	join(dir, sizeof (dir), root, "/onto");
	CHECK(mkdir(dir, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_ONTO, dir) == 0);
	check_probe_tree(dir, ns, exp_onto, (int)(sizeof (exp_onto) /
	    sizeof (exp_onto[0])), 8);

	zr_fixture_free(fx);
	zr_names_destroy(ns);
}

static void
check_odd_tree(const char *root)
{
	char err[256], path[PATHMAX];
	struct zr_names *ns;
	struct zr_walk w;
	uint32_t i, names;

	build_odd(root);
	ns = zr_names_create();
	CHECK(ns != NULL);
	err[0] = '\0';
	if (zr_walk(root, ns, &w, err, sizeof (err)) != 0)
		printf("  walk: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(zr_tree_verify(&w.zw_tree, err, sizeof (err)) == 0);

	/*
	 * 1 root, 64 chain directories and their file, empty, sym,
	 * the odd directory and its file, the fifo, three link
	 * directories and the one file they share, x, la/.zfs, and
	 * the ord directory with its six files. The root's own .zfs
	 * is not among them.
	 */
	CHECK(w.zw_tree.zt_npools == 84);
	CHECK(w.zw_nattrs == 84);
	names = 0;
	for (i = 0; i < w.zw_tree.zt_npools; i++)
		names += w.zw_tree.zt_pools[i].zp_nnames;
	CHECK(names == 86);

	check_deep(&w, ns);
	check_empty(&w, ns);
	check_symlink(&w, ns);
	check_odd(&w, ns);
	check_fifo(&w, ns);
	check_links(&w, ns);
	check_xattrs(&w, ns);
	check_dotzfs(&w, ns);
	check_order(&w, ns);
	deep_path(path, sizeof (path));
	check_attr(&w, ns, root, path);
	check_attr(&w, ns, root, "/fifo");
	check_attr(&w, ns, root, "/sym");
	check_attr(&w, ns, root, "/");

	zr_walk_fini(&w);
	zr_names_destroy(ns);
}

/*
 * A root that is not there, a root that is not a directory, and no
 * name table at all: each is an error naming what went wrong, and
 * each leaves something zr_walk_fini can take.
 */
static void
check_bad_root(const char *root)
{
	char err[256], full[PATHMAX];
	struct zr_names *ns;
	struct zr_walk w;

	ns = zr_names_create();
	CHECK(ns != NULL);
	err[0] = '\0';
	CHECK(zr_walk("/nonesuch/zrwalk", ns, &w, err, sizeof (err)) == -1);
	CHECK(strstr(err, "/nonesuch/zrwalk") != NULL);
	CHECK(strstr(err, strerror(ENOENT)) != NULL);
	CHECK(w.zw_rootfd == -1);
	CHECK(w.zw_nattrs == 0);
	zr_walk_fini(&w);

	/* the root must be a directory, and "x" is a file */
	join(full, sizeof (full), root, "/x");
	err[0] = '\0';
	CHECK(zr_walk(full, ns, &w, err, sizeof (err)) == -1);
	CHECK(strstr(err, full) != NULL);
	zr_walk_fini(&w);

	err[0] = '\0';
	CHECK(zr_walk("/", NULL, &w, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0');
	zr_walk_fini(&w);
	zr_names_destroy(ns);
}

int
main(void)
{
	char probe[256];
	char odd[256];

	check_acl_equal();

	tmp_template(probe, sizeof (probe), "zrwalk.XXXXXX");
	CHECK(mkdtemp(probe) != NULL);
	check_probe(probe);
	rmtree(probe);

	tmp_template(odd, sizeof (odd), "zrwalko.XXXXXX");
	CHECK(mkdtemp(odd) != NULL);
	check_odd_tree(odd);
	check_bad_root(odd);
	rmtree(odd);

	printf("check_walk: %d checks passed\n", checks);
	return (0);
}
