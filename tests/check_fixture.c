/*
 * The fixture tests: the probe scenario of v4-manifest.md section 7
 * parsed, built as real directories and walked back, and built again
 * as pools in memory; a second inline fixture for the symlinks and
 * the owner attributes the probe has none of; a third for the file
 * flags and extended attributes, built and walked back with zr_walk
 * so that what the fixture said is what the walk reads; the two
 * fixtures a platform line makes the box's, which parse here and
 * build nowhere; then every rejection the format promises. Cells
 * ZF1 to ZF16, ZF18, ZF19 and ZF21 to ZF26; ZF17 and ZF20 are the
 * box's.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fixture.h"
#include "name.h"
#include "walk.h"

/* The platforms with file flags, which are the ones a flags= needs. */
#if defined(__FreeBSD__) || defined(__APPLE__)
#define	HAVE_FFLAGS	1
#endif

#define	E_FILE	0
#define	E_LINK	1
#define	E_DIR	2
#define	E_SYM	3

#define	PATHMAX	1024
#define	WALK_MIN	16

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

/*
 * One row of what a built tree must look like. ee_arg is a file's or
 * a link's token, or a symlink's target; ee_same is the name this one
 * shares an inode with, if any; ee_nlink is the pool's size.
 */
struct expent {
	const char	*ee_path;
	int		ee_type;
	const char	*ee_arg;
	const char	*ee_same;
	int		ee_nlink;
	int		ee_mode;	/* -1 when the entry gave none */
};

static const struct expent exp_base[] = {
	{ "/a", E_FILE, "x", NULL, 1, -1 },
	{ "/b", E_FILE, "y", NULL, 1, -1 },
	{ "/h1", E_FILE, "h", "/h2", 2, -1 },
	{ "/h2", E_LINK, "h", "/h1", 2, -1 },
	{ "/d", E_DIR, NULL, NULL, 1, -1 },
	{ "/d/f", E_FILE, "f", NULL, 1, -1 },
	{ "/keep", E_DIR, NULL, NULL, 1, -1 },
	{ "/keep/k", E_FILE, "k", NULL, 1, -1 }
};

static const struct expent exp_from[] = {
	{ "/a", E_FILE, "a2", NULL, 1, -1 },
	{ "/h1", E_FILE, "h2", "/h3", 3, -1 },
	{ "/h2", E_LINK, "h2", "/h1", 3, -1 },
	{ "/h3", E_LINK, "h2", "/h2", 3, -1 },
	{ "/e", E_DIR, NULL, NULL, 1, -1 },
	{ "/e/f", E_FILE, "f", NULL, 1, -1 },
	{ "/keep", E_DIR, NULL, NULL, 1, -1 },
	{ "/keep/k", E_FILE, "k", NULL, 1, -1 },
	{ "/n", E_FILE, "n", NULL, 1, -1 }
};

static const struct expent exp_onto[] = {
	{ "/a", E_FILE, "a3", NULL, 1, -1 },
	{ "/b", E_FILE, "y", NULL, 1, -1 },
	{ "/h1", E_FILE, "h", "/h2", 2, -1 },
	{ "/h2", E_LINK, "h", "/h1", 2, -1 },
	{ "/d", E_DIR, NULL, NULL, 1, -1 },
	{ "/d/f", E_FILE, "f", NULL, 1, -1 },
	{ "/keep", E_DIR, NULL, NULL, 1, -1 },
	{ "/keep/k", E_FILE, "k2", NULL, 1, -1 }
};

static const struct expent exp_extra[] = {
	{ "/l", E_SYM, "/a b", NULL, 1, -1 },
	{ "/f", E_FILE, "t", NULL, 1, 0600 },
	{ "/dd", E_DIR, NULL, NULL, 1, 0700 },
	{ "/dd/s", E_SYM, "t", NULL, 1, -1 },
	{ "/dd/s2", E_SYM, "t", NULL, 1, -1 }
};

#ifdef HAVE_FFLAGS
/*
 * The flags and extended attributes fixture: an immutable directory
 * whose child was written before the flag went on, two attributes
 * on that child and a flag of its own, an attribute set through the
 * second name of a hardlink pool, and a file with the same token as
 * both and nothing else, which is what makes them different.
 */
static const struct expent exp_attrs[] = {
	{ "/d", E_DIR, NULL, NULL, 1, -1 },
	{ "/d/f", E_FILE, "t", NULL, 1, -1 },
	{ "/e", E_DIR, NULL, NULL, 1, -1 },
	{ "/h1", E_FILE, "t", "/h2", 2, -1 },
	{ "/h2", E_LINK, "t", "/h1", 2, -1 },
	{ "/plain", E_FILE, "t", NULL, 1, -1 }
};
#endif	/* HAVE_FFLAGS */

/* Every name a walk found under one root, parents before children. */
struct walk {
	char	**w_path;
	int	*w_isdir;
	int	w_n;
	int	w_cap;
};

static void
walk_push(struct walk *w, const char *rel, int isdir)
{
	char **np;
	int *ni, cap;

	if (w->w_n == w->w_cap) {
		cap = w->w_cap == 0 ? WALK_MIN : w->w_cap * 2;
		np = realloc(w->w_path, (size_t)cap * sizeof (char *));
		CHECK(np != NULL);
		w->w_path = np;
		ni = realloc(w->w_isdir, (size_t)cap * sizeof (int));
		CHECK(ni != NULL);
		w->w_isdir = ni;
		w->w_cap = cap;
	}
	w->w_path[w->w_n] = malloc(strlen(rel) + 1);
	CHECK(w->w_path[w->w_n] != NULL);
	(void) strcpy(w->w_path[w->w_n], rel);
	w->w_isdir[w->w_n] = isdir;
	w->w_n++;
}

static void
join(char *out, size_t outlen, const char *a, const char *b)
{
	int n;

	n = snprintf(out, outlen, "%s%s", a, b);
	CHECK(n > 0 && (size_t)n < outlen);
}

/* Read one directory and record every name in it. */
static void
walk_scan(struct walk *w, const char *root, const char *rel)
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
		walk_push(w, child, S_ISDIR(st.st_mode) ? 1 : 0);
	}
	CHECK(closedir(d) == 0);
}

/*
 * The whole tree under root, breadth first and never recursing: each
 * directory found is scanned in turn from the list it was added to,
 * so parents always come before their children.
 */
static void
walk_tree(struct walk *w, const char *root)
{
	int i;

	memset(w, 0, sizeof (*w));
	walk_scan(w, root, "");
	for (i = 0; i < w->w_n; i++) {
		if (w->w_isdir[i])
			walk_scan(w, root, w->w_path[i]);
	}
}

static void
walk_free(struct walk *w)
{
	int i;

	for (i = 0; i < w->w_n; i++)
		free(w->w_path[i]);
	free(w->w_path);
	free(w->w_isdir);
	memset(w, 0, sizeof (*w));
}

/*
 * Take the file flags off everything under root, parents first. A
 * fixture may have made a file immutable, and unlink(2) cannot
 * remove one; it may have made a directory immutable, and then
 * nothing can be removed from it at all.
 */
static void
clearflags(const char *root)
{
#ifdef HAVE_FFLAGS
	char full[PATHMAX];
	struct walk w;
	int i;

	(void) lchflags(root, 0);
	walk_tree(&w, root);
	for (i = 0; i < w.w_n; i++) {
		join(full, sizeof (full), root, w.w_path[i]);
		(void) lchflags(full, 0);
	}
	walk_free(&w);
#else
	(void) root;
#endif
}

/* Children before parents, which is the walk order reversed. */
static void
rmtree(const char *root)
{
	char full[PATHMAX];
	struct walk w;
	int i;

	clearflags(root);
	walk_tree(&w, root);
	for (i = w.w_n - 1; i >= 0; i--) {
		join(full, sizeof (full), root, w.w_path[i]);
		if (w.w_isdir[i])
			CHECK(rmdir(full) == 0);
		else
			CHECK(unlink(full) == 0);
	}
	walk_free(&w);
	CHECK(rmdir(root) == 0);
}

static void
check_bytes(const char *full, const char *tok)
{
	char buf[256];
	size_t want, got;
	FILE *fp;

	want = strlen(tok) + 1;
	CHECK(want < sizeof (buf));
	fp = fopen(full, "rb");
	CHECK(fp != NULL);
	got = fread(buf, 1, sizeof (buf), fp);
	CHECK(fclose(fp) == 0);
	CHECK(got == want);
	CHECK(memcmp(buf, tok, want - 1) == 0);
	CHECK(buf[want - 1] == '\n');
}

static void
check_link(const char *full, const char *tgt)
{
	char buf[PATHMAX];
	ssize_t n;

	n = readlink(full, buf, sizeof (buf) - 1);
	CHECK(n > 0 && (size_t)n < sizeof (buf));
	buf[n] = '\0';
	CHECK(strcmp(buf, tgt) == 0);
}

/*
 * The built tree holds exactly the rows of the table, each with the
 * right type, bytes, link count and target.
 */
static void
check_built(const char *root, const struct expent *tbl, int n)
{
	char full[PATHMAX], other[PATHMAX];
	struct stat st, st2;
	struct walk w;
	int i, j, seen;

	for (i = 0; i < n; i++) {
		join(full, sizeof (full), root, tbl[i].ee_path);
		CHECK(lstat(full, &st) == 0);
		if (tbl[i].ee_type == E_DIR) {
			CHECK(S_ISDIR(st.st_mode));
		} else if (tbl[i].ee_type == E_SYM) {
			CHECK(S_ISLNK(st.st_mode));
			check_link(full, tbl[i].ee_arg);
		} else {
			CHECK(S_ISREG(st.st_mode));
			CHECK((unsigned long)st.st_nlink ==
			    (unsigned long)tbl[i].ee_nlink);
			check_bytes(full, tbl[i].ee_arg);
		}
		if (tbl[i].ee_mode >= 0 && tbl[i].ee_type != E_SYM) {
			CHECK((int)(st.st_mode & 07777) == tbl[i].ee_mode);
		}
		if (tbl[i].ee_same != NULL) {
			join(other, sizeof (other), root, tbl[i].ee_same);
			CHECK(lstat(other, &st2) == 0);
			CHECK(st.st_ino == st2.st_ino);
			CHECK(st.st_dev == st2.st_dev);
		}
	}
	walk_tree(&w, root);
	CHECK(w.w_n == n);
	for (i = 0; i < w.w_n; i++) {
		seen = 0;
		for (j = 0; j < n; j++) {
			if (strcmp(w.w_path[i], tbl[j].ee_path) == 0)
				seen++;
		}
		CHECK(seen == 1);
	}
	walk_free(&w);
}

static zr_pool_t
poolof(const struct zr_tree *tr, struct zr_names *ns, const char *path)
{
	zr_pool_t p;

	p = zr_tree_pool(tr, zr_names_lookup(ns, path, strlen(path)));
	CHECK(p != ZR_POOL_NONE);
	return (p);
}

static uint32_t
content(const struct zr_tree *tr, struct zr_names *ns, const char *path)
{
	return (tr->zt_pools[poolof(tr, ns, path)].zp_content);
}

/* The pools of one tree carry distinct synthetic inode numbers. */
static void
check_inos(const struct zr_tree *tr)
{
	uint32_t i, j;

	for (i = 0; i < tr->zt_npools; i++) {
		CHECK(tr->zt_pools[i].zp_ino != 0);
		for (j = i + 1; j < tr->zt_npools; j++)
			CHECK(tr->zt_pools[i].zp_ino !=
			    tr->zt_pools[j].zp_ino);
	}
}

/*
 * ZF4: the pools the spec implies, without a filesystem. The
 * hardlink pool of base holds h1 and h2, the one of from holds h1,
 * h2 and h3, and the content handles say which files share bytes
 * across the trees.
 */
static void
check_pools(const struct zr_fixture *fx)
{
	struct zr_tree base, from, onto;
	struct zr_names *ns;
	const struct zr_pool *p;
	char err[256];

	ns = zr_names_create();
	CHECK(ns != NULL);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_BASE, ns, &base) == 0);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_FROM, ns, &from) == 0);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_ONTO, ns, &onto) == 0);
	CHECK(base.zt_sealed != 0);
	CHECK(from.zt_sealed != 0);
	CHECK(onto.zt_sealed != 0);
	CHECK(zr_tree_verify(&base, err, sizeof (err)) == 0);
	CHECK(zr_tree_verify(&from, err, sizeof (err)) == 0);
	CHECK(zr_tree_verify(&onto, err, sizeof (err)) == 0);
	CHECK(base.zt_npools == 7);
	CHECK(from.zt_npools == 7);
	CHECK(onto.zt_npools == 7);
	check_inos(&base);
	check_inos(&from);
	check_inos(&onto);

	CHECK(poolof(&base, ns, "/h1") == poolof(&base, ns, "/h2"));
	p = &base.zt_pools[poolof(&base, ns, "/h1")];
	CHECK(p->zp_nnames == 2);
	CHECK(p->zp_nlink == 2);
	CHECK(p->zp_type == ZR_T_FILE);
	CHECK(poolof(&from, ns, "/h1") == poolof(&from, ns, "/h2"));
	CHECK(poolof(&from, ns, "/h2") == poolof(&from, ns, "/h3"));
	p = &from.zt_pools[poolof(&from, ns, "/h1")];
	CHECK(p->zp_nnames == 3);
	CHECK(p->zp_nlink == 3);
	CHECK(poolof(&base, ns, "/a") != poolof(&base, ns, "/b"));
	CHECK(zr_tree_pool(&from, zr_names_lookup(ns, "/b", 2)) ==
	    ZR_POOL_NONE);
	CHECK(zr_tree_pool(&base, zr_names_lookup(ns, "/h3", 3)) ==
	    ZR_POOL_NONE);
	p = &base.zt_pools[poolof(&base, ns, "/d")];
	CHECK(p->zp_type == ZR_T_DIR);
	CHECK(p->zp_nnames == 1);

	/* equal tokens, equal handles, in one tree and across three */
	CHECK(content(&base, ns, "/b") == content(&onto, ns, "/b"));
	CHECK(content(&base, ns, "/h1") == content(&onto, ns, "/h1"));
	CHECK(content(&base, ns, "/h1") == content(&onto, ns, "/h2"));
	CHECK(content(&base, ns, "/d/f") == content(&from, ns, "/e/f"));
	CHECK(content(&base, ns, "/keep/k") == content(&from, ns, "/keep/k"));

	/* different tokens, different handles */
	CHECK(content(&base, ns, "/a") != content(&from, ns, "/a"));
	CHECK(content(&base, ns, "/a") != content(&onto, ns, "/a"));
	CHECK(content(&from, ns, "/a") != content(&onto, ns, "/a"));
	CHECK(content(&base, ns, "/a") != content(&base, ns, "/b"));
	CHECK(content(&base, ns, "/h1") != content(&from, ns, "/h1"));
	CHECK(content(&base, ns, "/keep/k") != content(&onto, ns, "/keep/k"));

	/*
	 * Directories with the attributes the builder defaults to are
	 * one content, in one tree and across three, and no file is
	 * ever that content.
	 */
	CHECK(content(&base, ns, "/d") == content(&base, ns, "/keep"));
	CHECK(content(&base, ns, "/d") == content(&from, ns, "/e"));
	CHECK(content(&base, ns, "/d") == content(&onto, ns, "/keep"));
	CHECK(content(&base, ns, "/a") != content(&base, ns, "/d"));
	CHECK(content(&onto, ns, "/keep/k") != content(&base, ns, "/keep"));
	CHECK(content(&base, ns, "/a") != ZR_CONTENT_NONE);

	zr_tree_fini(&base);
	zr_tree_fini(&from);
	zr_tree_fini(&onto);
	zr_names_destroy(ns);
}

/* Write one inline spec under root and load it. */
static struct zr_fixture *
load_spec(const char *root, const char *name, const char *spec)
{
	struct zr_fixture *fx = NULL;
	char path[PATHMAX], err[256];
	size_t len;
	FILE *fp;

	join(path, sizeof (path), root, name);
	len = strlen(spec);
	fp = fopen(path, "wb");
	CHECK(fp != NULL);
	CHECK(fwrite(spec, 1, len, fp) == len);
	CHECK(fclose(fp) == 0);
	err[0] = '\0';
	if (zr_fixture_load(path, &fx, err, sizeof (err)) != 0)
		printf("  %s: %s\n", path, err);
	CHECK(fx != NULL);
	CHECK(unlink(path) == 0);
	return (fx);
}

/*
 * ZF2, ZF3, ZF25: the second fixture -- a symlink target that needs
 * escaping, a symlink whose target is also a file's token, the owner
 * attributes, and the mode a symlink does not keep.
 */
static void
check_extra(const char *root)
{
	char spec[512], path[PATHMAX], full[PATHMAX], err[256];
	struct zr_fixture *fx = NULL;
	struct zr_names *ns;
	struct zr_tree tr;
	struct stat st;
	int n;

	n = snprintf(spec, sizeof (spec),
	    "# a symlink, an escaped target and the attributes\n"
	    "tree base\n"
	    "\t/l symlink /a\\040b\n"
	    "\t/f file t mode=0600\n"
	    "\t/dd dir mode=0700\n"
	    "\t/dd/s symlink t mode=0777 uid=%lu gid=%lu\n"
	    "\t/dd/s2 symlink t\n"
	    "tree from\n"
	    "tree onto\n",
	    (unsigned long)getuid(), (unsigned long)getgid());
	CHECK(n > 0 && (size_t)n < sizeof (spec));
	fx = load_spec(root, "/extra.zrt", spec);
	CHECK(zr_fixture_expect(fx) == NULL);

	join(full, sizeof (full), root, "/extra");
	CHECK(mkdir(full, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_BASE, full) == 0);
	check_built(full, exp_extra, (int)(sizeof (exp_extra) /
	    sizeof (exp_extra[0])));

	/* uid and gid reached the symlink itself, its mode did not */
	join(path, sizeof (path), full, "/dd/s");
	CHECK(lstat(path, &st) == 0);
	CHECK(S_ISLNK(st.st_mode));
	CHECK(st.st_uid == getuid());
	CHECK(st.st_gid == getgid());

	/* building over a directory that is not empty is refused */
	CHECK(zr_fixture_build(fx, ZR_FX_BASE, full) == -1);
	CHECK(errno == ENOTEMPTY);

	ns = zr_names_create();
	CHECK(ns != NULL);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_BASE, ns, &tr) == 0);
	CHECK(zr_tree_verify(&tr, err, sizeof (err)) == 0);
	CHECK(tr.zt_pools[poolof(&tr, ns, "/l")].zp_type == ZR_T_SYMLINK);
	CHECK(content(&tr, ns, "/l") != content(&tr, ns, "/dd/s"));
	/*
	 * A symlink whose target is a file's token is not that file's
	 * content: the type is part of the handle, as it is part of
	 * what the oracle compares.
	 */
	CHECK(content(&tr, ns, "/f") != content(&tr, ns, "/dd/s"));
	/*
	 * A symlink's mode is not honoured anywhere this tool runs, so
	 * the handle passes it by just as the builder does: these two
	 * differ in mode= alone and in nothing the filesystem keeps.
	 * The owner and group they name are the ones the builder would
	 * have left behind anyway, which is the same resolution.
	 */
	CHECK(content(&tr, ns, "/dd/s") == content(&tr, ns, "/dd/s2"));
	/* the directory's mode= is honoured, and does reach its handle */
	CHECK(content(&tr, ns, "/dd") != ZR_CONTENT_NONE);
	zr_tree_fini(&tr);
	zr_names_destroy(ns);

	zr_fixture_free(fx);
	rmtree(full);
}

#ifdef HAVE_FFLAGS

/*
 * ZF7, ZF9 to ZF12, ZF23, ZF24 and ZF26: file flags and extended
 * attributes, built and then read back with the walk itself, which
 * is the only proof that a fixture's attributes and the walk's are
 * the same attributes. The immutable directory is the ordering test
 * -- its child was written first, and a builder that set the flag
 * when it made the directory could not have put anything in it.
 * The attributes go on the pool through whichever name carries
 * them, /h2 being the second name of the file /h1, and every one of
 * them tells the handles apart from /plain, whose token is the same
 * and whose attributes are none.
 */
static void
check_attrs(const char *root)
{
	static const char spec[] =
	    "# file flags and extended attributes\n"
	    "tree base\n"
	    "\t/d dir flags=uchg\n"
	    "\t/d/f file t flags=nodump xattr=user.a: xattr=user.b:v\\040w\n"
	    "\t/e dir\n"
	    "\t/h1 file t\n"
	    "\t/h2 link /h1 xattr=user.z:zz\n"
	    "\t/plain file t\n"
	    "tree from\n"
	    "tree onto\n";
	char full[PATHMAX], err[256];
	struct zr_fixture *fx;
	const struct zr_attr *at;
	struct zr_names *ns;
	struct zr_walk w;
	struct zr_tree tr;

	fx = load_spec(root, "/attrs.zrt", spec);
	join(full, sizeof (full), root, "/attrs");
	CHECK(mkdir(full, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_BASE, full) == 0);
	check_built(full, exp_attrs, (int)(sizeof (exp_attrs) /
	    sizeof (exp_attrs[0])));

	ns = zr_names_create();
	CHECK(ns != NULL);
	err[0] = '\0';
	if (zr_walk(full, ns, &w, err, sizeof (err)) != 0)
		printf("  walk: %s\n", err);
	CHECK(err[0] == '\0');

	/* the two attributes of the child, in name order, and its flag */
	at = &w.zw_attrs[poolof(&w.zw_tree, ns, "/d/f")];
	CHECK(at->za_nxattrs == 2);
	CHECK(strcmp(at->za_xattrs[0].zx_name, "user.a") == 0);
	CHECK(at->za_xattrs[0].zx_len == 0);
	CHECK(strcmp(at->za_xattrs[1].zx_name, "user.b") == 0);
	CHECK(at->za_xattrs[1].zx_len == 3);
	CHECK(memcmp(at->za_xattrs[1].zx_value, "v w", 3) == 0);
	CHECK(at->za_flags == UF_NODUMP);

	/* the directory kept its child and became immutable after it */
	at = &w.zw_attrs[poolof(&w.zw_tree, ns, "/d")];
	CHECK(at->za_flags == UF_IMMUTABLE);
	at = &w.zw_attrs[poolof(&w.zw_tree, ns, "/e")];
	CHECK(at->za_flags == 0);

	/* the second name of a pool set the attribute on the file */
	at = &w.zw_attrs[poolof(&w.zw_tree, ns, "/h1")];
	CHECK(at->za_nxattrs == 1);
	CHECK(strcmp(at->za_xattrs[0].zx_name, "user.z") == 0);
	CHECK(at->za_xattrs[0].zx_len == 2);
	CHECK(memcmp(at->za_xattrs[0].zx_value, "zz", 2) == 0);
	CHECK(poolof(&w.zw_tree, ns, "/h2") ==
	    poolof(&w.zw_tree, ns, "/h1"));

	at = &w.zw_attrs[poolof(&w.zw_tree, ns, "/plain")];
	CHECK(at->za_nxattrs == 0);
	CHECK(at->za_flags == 0);
	zr_walk_fini(&w);
	zr_names_destroy(ns);

	/* one token, four pools, and the attributes tell them apart */
	ns = zr_names_create();
	CHECK(ns != NULL);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_BASE, ns, &tr) == 0);
	CHECK(zr_tree_verify(&tr, err, sizeof (err)) == 0);
	CHECK(content(&tr, ns, "/d/f") != content(&tr, ns, "/plain"));
	CHECK(content(&tr, ns, "/h1") != content(&tr, ns, "/plain"));
	CHECK(content(&tr, ns, "/h1") != content(&tr, ns, "/d/f"));
	CHECK(content(&tr, ns, "/h1") == content(&tr, ns, "/h2"));
	CHECK(content(&tr, ns, "/d") != content(&tr, ns, "/e"));
	zr_tree_fini(&tr);
	zr_names_destroy(ns);

	zr_fixture_free(fx);
	rmtree(full);
}

#endif	/* HAVE_FFLAGS */

/*
 * ZF18 and ZF21: one of the two fixtures a platform line makes the
 * box's. It parses here, whatever this platform is, and its
 * attribute reaches the handles -- base and onto agree and from
 * does not, which is what makes the expect block a write. Building
 * it is refused off its own platform, in words naming the line that
 * said so.
 */
static void
check_boxonly(const char *root, const char *path)
{
	struct zr_tree base, from, onto;
	struct zr_fixture *fx = NULL;
	struct zr_names *ns;
	char dir[PATHMAX], err[256];

	err[0] = '\0';
	if (zr_fixture_load(path, &fx, err, sizeof (err)) != 0)
		printf("  %s: %s\n", path, err);
	CHECK(fx != NULL);
	CHECK(zr_fixture_platform(fx) != NULL);
	CHECK(strcmp(zr_fixture_platform(fx), "freebsd") == 0);
	CHECK(zr_fixture_expect(fx) != NULL);

	ns = zr_names_create();
	CHECK(ns != NULL);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_BASE, ns, &base) == 0);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_FROM, ns, &from) == 0);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_ONTO, ns, &onto) == 0);
	CHECK(content(&base, ns, "/A") == content(&onto, ns, "/A"));
	CHECK(content(&base, ns, "/A") != content(&from, ns, "/A"));
	zr_tree_fini(&base);
	zr_tree_fini(&from);
	zr_tree_fini(&onto);
	zr_names_destroy(ns);

	join(dir, sizeof (dir), root, "/boxonly");
	CHECK(mkdir(dir, 0755) == 0);
#ifndef __FreeBSD__
	err[0] = '\0';
	errno = 0;
	CHECK(zr_fixture_build_err(fx, ZR_FX_BASE, dir, err,
	    sizeof (err)) == -1);
	CHECK(errno == ENOTSUP);
	if (strstr(err, "platform freebsd") == NULL)
		printf("  build: %s\n", err);
	CHECK(strstr(err, "platform freebsd") != NULL);
	CHECK(strstr(err, "line 1:") != NULL);
	CHECK(zr_fixture_build(fx, ZR_FX_BASE, dir) == -1);
#endif
	CHECK(rmdir(dir) == 0);
	zr_fixture_free(fx);
}

/* One spec that must be rejected, with the line it must name. */
static void
reject(const char *root, const char *spec, int line, const char *what)
{
	struct zr_fixture *fx = NULL;
	char path[PATHMAX], err[256], want[32];
	size_t len;
	FILE *fp;

	join(path, sizeof (path), root, "/bad.zrt");
	len = strlen(spec);
	fp = fopen(path, "wb");
	CHECK(fp != NULL);
	CHECK(fwrite(spec, 1, len, fp) == len);
	CHECK(fclose(fp) == 0);
	err[0] = '\0';
	CHECK(zr_fixture_load(path, &fx, err, sizeof (err)) == -1);
	CHECK(fx == NULL);
	(void) snprintf(want, sizeof (want), "line %d:", line);
	if (strstr(err, want) == NULL)
		printf("  %s: wanted \"%s\", got \"%s\"\n", what, want, err);
	CHECK(strstr(err, want) != NULL);
	CHECK(unlink(path) == 0);
}

/* ZF6, ZF8, ZF13 to ZF16, ZF19 and ZF22: every rejection in turn. */
static void
check_rejections(const char *root)
{
	reject(root,
	    "tree base\n"
	    "/a file x\n"
	    "/a file y\n"
	    "tree from\n"
	    "tree onto\n", 3, "a name listed twice");
	reject(root,
	    "tree base\n"
	    "/h2 link /h1\n"
	    "tree from\n"
	    "tree onto\n", 2, "a link to an unknown target");
	reject(root,
	    "tree base\n"
	    "/d/f file f\n"
	    "/d dir\n"
	    "tree from\n"
	    "tree onto\n", 2, "a child before its parent");
	reject(root,
	    "tree base\n"
	    "/a file x\n"
	    "tree from\n"
	    "/a file x\n", 4, "a tree missing");
	reject(root,
	    "tree base\n"
	    "tree onto\n"
	    "tree from\n", 2, "trees out of order");
	reject(root,
	    "tree base\n"
	    "/bad\\8ish file x\n"
	    "tree from\n"
	    "tree onto\n", 2, "a bad escape in a name");
	reject(root,
	    "tree base\n"
	    "/a file x mode=0648\n"
	    "tree from\n"
	    "tree onto\n", 2, "a mode that is not octal");
	reject(root,
	    "tree base\n"
	    "/a file x flags=nodump gid=0\n"
	    "tree from\n"
	    "tree onto\n", 2, "attributes out of order");
	reject(root,
	    "tree base\n"
	    "/a file x flags=nosuchflag\n"
	    "tree from\n"
	    "tree onto\n", 2, "a flag chflags does not know");
	reject(root,
	    "tree base\n"
	    "/a file x xattr=user.b:1 xattr=user.a:2\n"
	    "tree from\n"
	    "tree onto\n", 2, "xattrs out of name order");
	reject(root,
	    "tree base\n"
	    "/a file x xattr=user.a:1 xattr=user.a:2\n"
	    "tree from\n"
	    "tree onto\n", 2, "one xattr name twice");
	reject(root,
	    "tree base\n"
	    "/a file x xattr=user.a\n"
	    "tree from\n"
	    "tree onto\n", 2, "an xattr with no colon");
	reject(root,
	    "tree base\n"
	    "/a file x xattr=plain:1\n"
	    "tree from\n"
	    "tree onto\n", 2, "an xattr in no namespace");
	reject(root,
	    "tree base\n"
	    "/a file x xattr=system.a:1\n"
	    "tree from\n"
	    "tree onto\n", 2, "a system xattr with no platform line");
	reject(root,
	    "tree base\n"
	    "/a file x xattr=user.a:\\8\n"
	    "tree from\n"
	    "tree onto\n", 2, "a bad escape in an xattr value");
	reject(root,
	    "tree base\n"
	    "/a file x acl=owner@:rwxp:allow\n"
	    "tree from\n"
	    "tree onto\n", 2, "an acl with no platform line");
	reject(root,
	    "tree base\n"
	    "platform freebsd\n"
	    "tree from\n"
	    "tree onto\n", 2, "a platform line after a tree line");
	reject(root,
	    "platform freebsd\n"
	    "platform freebsd\n"
	    "tree base\n"
	    "tree from\n"
	    "tree onto\n", 2, "a platform line twice");
	reject(root,
	    "platform sunos\n"
	    "tree base\n"
	    "tree from\n"
	    "tree onto\n", 1, "a platform nothing can build");
}

int
main(void)
{
	char root[] = "/tmp/zrfix.XXXXXX";
	char dir[PATHMAX], err[256];
	struct zr_fixture *fx = NULL;
	const char *expect;

	err[0] = '\0';
	if (zr_fixture_load("tests/fixtures/probe.zrt", &fx, err,
	    sizeof (err)) != 0)
		printf("  load: %s\n", err);
	CHECK(fx != NULL);

	expect = zr_fixture_expect(fx);
	CHECK(expect != NULL);
	CHECK(strncmp(expect, "#rebase-manifest 4\n", 19) == 0);
	CHECK(strstr(expect, "\nconflict 1 changed-both\n") != NULL);
	CHECK(strstr(expect, "  onto ({/a}z)\n") != NULL);
	CHECK(strstr(expect, "expect") == NULL);

	check_pools(fx);

	CHECK(mkdtemp(root) != NULL);
	join(dir, sizeof (dir), root, "/base");
	CHECK(mkdir(dir, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_BASE, dir) == 0);
	check_built(dir, exp_base, (int)(sizeof (exp_base) /
	    sizeof (exp_base[0])));
	join(dir, sizeof (dir), root, "/from");
	CHECK(mkdir(dir, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_FROM, dir) == 0);
	check_built(dir, exp_from, (int)(sizeof (exp_from) /
	    sizeof (exp_from[0])));
	join(dir, sizeof (dir), root, "/onto");
	CHECK(mkdir(dir, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_ONTO, dir) == 0);
	check_built(dir, exp_onto, (int)(sizeof (exp_onto) /
	    sizeof (exp_onto[0])));
	zr_fixture_free(fx);

	check_extra(root);
#ifdef HAVE_FFLAGS
	check_attrs(root);
#endif
	check_boxonly(root, "tests/fixtures/freebsd/acl-nfsv4.zrt");
	check_boxonly(root, "tests/fixtures/freebsd/sysxattr.zrt");
	check_rejections(root);

	rmtree(root);
	printf("check_fixture: %d checks passed\n", checks);
	return (0);
}
