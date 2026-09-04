/*
 * The fixture tests: the probe scenario of v4-manifest.md section 7
 * parsed, built as real directories and walked back, and built again
 * as pools in memory; a second inline fixture for the symlinks and
 * the owner attributes the probe has none of; a third for the file
 * flags and extended attributes, built and walked back with zr_walk
 * so that what the fixture said is what the walk reads; the two
 * fixtures a platform line makes the box's, which parse here and
 * build nowhere; then every rejection the format promises; and last
 * the editor, which turns one built tree into another and must land
 * where a build of that other one would have. Cells ZF1 to ZF16,
 * ZF18, ZF19, ZF21 to ZF26 and ZF33 to ZF49; ZF17, ZF20, ZF50 and
 * ZF51 are the box's.
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

/* Build one tree, and say why when it fails: the reason is the finding. */
static void
build_ok(const struct zr_fixture *fx, enum zr_fixture_tree which,
    const char *dir)
{
	char err[256];

	err[0] = '\0';
	if (zr_fixture_build_err(fx, which, dir, err, sizeof (err)) != 0)
		printf("  build %s: %s\n", dir, err);
	CHECK(err[0] == '\0');
}

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
	build_ok(fx, ZR_FX_BASE, full);
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
	build_ok(fx, ZR_FX_BASE, full);
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

/*
 * ---------------------------------------------------------------
 * --edit-fixture
 *
 * Every case below is run the same way: base is built into A from
 * nothing, A is edited into one of the other trees, and that tree is
 * built into B from nothing. Then A must equal B name for name,
 * pool for pool and byte for byte -- an edit and a build must land
 * in the same place -- while the objects the fixture leaves alone
 * between the two trees must still be the objects they were, with
 * their inode and their ctime intact, and the objects it changes
 * must not be. The second edit of the same tree must find nothing
 * left to do.
 * ---------------------------------------------------------------
 */

#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__linux__)
#define	HAVE_XATTRS	1
#endif

/* The ctime, which POSIX 2008 spells st_ctim and macOS does not. */
#if defined(__APPLE__)
#define	CTIME_OF(st)	((st).st_ctimespec)
#else
#define	CTIME_OF(st)	((st).st_ctim)
#endif

#define	SNAP_MIN	16

/* What one name was before an edit: which object, and when it moved. */
struct snapent {
	char		*se_path;
	uint64_t	se_ino;
	struct timespec	se_ct;
};

struct snap {
	struct snapent	*sn_e;
	int		sn_n;
	int		sn_cap;
};

/* What one name's inode must have done: survived, or not. */
#define	IX_KEPT	1
#define	IX_NEW	0

struct inoexp {
	const char	*ix_path;
	int		ix_kept;
};

static void
snap_take(struct snap *s, const char *root)
{
	char full[PATHMAX];
	struct snapent *ne;
	struct stat st;
	struct walk w;
	int i, cap;

	memset(s, 0, sizeof (*s));
	walk_tree(&w, root);
	s->sn_cap = w.w_n > SNAP_MIN ? w.w_n : SNAP_MIN;
	s->sn_e = calloc((size_t)s->sn_cap, sizeof (struct snapent));
	CHECK(s->sn_e != NULL);
	for (i = 0; i < w.w_n; i++) {
		join(full, sizeof (full), root, w.w_path[i]);
		CHECK(lstat(full, &st) == 0);
		ne = &s->sn_e[s->sn_n++];
		ne->se_path = malloc(strlen(w.w_path[i]) + 1);
		CHECK(ne->se_path != NULL);
		(void) strcpy(ne->se_path, w.w_path[i]);
		ne->se_ino = (uint64_t)st.st_ino;
		ne->se_ct = CTIME_OF(st);
	}
	cap = s->sn_cap;
	CHECK(s->sn_n <= cap);
	walk_free(&w);
}

static void
snap_free(struct snap *s)
{
	int i;

	for (i = 0; i < s->sn_n; i++)
		free(s->sn_e[i].se_path);
	free(s->sn_e);
	memset(s, 0, sizeof (*s));
}

static const struct snapent *
snap_find(const struct snap *s, const char *path)
{
	int i;

	for (i = 0; i < s->sn_n; i++) {
		if (strcmp(s->sn_e[i].se_path, path) == 0)
			return (&s->sn_e[i]);
	}
	return (NULL);
}

static int
same_ct(const struct timespec *a, const struct timespec *b)
{
	return (a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec);
}

/* One whole file, however long, so that two can be compared. */
static unsigned char *
readall(const char *full, size_t *lenp)
{
	unsigned char *buf, *nb;
	size_t cap, len;
	ssize_t n;
	FILE *fp;

	fp = fopen(full, "rb");
	CHECK(fp != NULL);
	cap = 256;
	len = 0;
	buf = malloc(cap);
	CHECK(buf != NULL);
	for (;;) {
		n = (ssize_t)fread(buf + len, 1, cap - len, fp);
		len += (size_t)n;
		if (len < cap)
			break;
		nb = realloc(buf, cap * 2);
		CHECK(nb != NULL);
		buf = nb;
		cap *= 2;
	}
	CHECK(ferror(fp) == 0);
	CHECK(fclose(fp) == 0);
	*lenp = len;
	return (buf);
}

static void
same_bytes(const char *ra, const char *rb, const char *path)
{
	char fa[PATHMAX], fb[PATHMAX];
	unsigned char *ba, *bb;
	size_t la, lb;

	join(fa, sizeof (fa), ra, path);
	join(fb, sizeof (fb), rb, path);
	ba = readall(fa, &la);
	bb = readall(fb, &lb);
	CHECK(la == lb);
	CHECK(la == 0 || memcmp(ba, bb, la) == 0);
	free(ba);
	free(bb);
}

/*
 * The attributes of two pools, field by field, the walk's own view.
 * The size of a directory is left out of it: it is how much room the
 * filesystem gave the entries, not what the entries are, and a
 * directory that lost one keeps the room on some of them.
 */
static void
same_attrs(const struct zr_attr *a, const struct zr_attr *b, zr_type_t type)
{
	uint32_t i;

	CHECK(a->za_mode == b->za_mode);
	CHECK(a->za_uid == b->za_uid);
	CHECK(a->za_gid == b->za_gid);
	CHECK(a->za_flags == b->za_flags);
	if (type != ZR_T_DIR)
		CHECK(a->za_size == b->za_size);
	CHECK(a->za_rdev == b->za_rdev);
	CHECK((a->za_target == NULL) == (b->za_target == NULL));
	if (a->za_target != NULL)
		CHECK(strcmp(a->za_target, b->za_target) == 0);
	CHECK(a->za_nxattrs == b->za_nxattrs);
	for (i = 0; i < a->za_nxattrs; i++) {
		CHECK(strcmp(a->za_xattrs[i].zx_name,
		    b->za_xattrs[i].zx_name) == 0);
		CHECK(a->za_xattrs[i].zx_len == b->za_xattrs[i].zx_len);
		CHECK(a->za_xattrs[i].zx_len == 0 ||
		    memcmp(a->za_xattrs[i].zx_value, b->za_xattrs[i].zx_value,
		    a->za_xattrs[i].zx_len) == 0);
	}
	CHECK(zr_acl_equal(a->za_acl, b->za_acl) == 1);
	CHECK(zr_acl_equal(a->za_dacl, b->za_dacl) == 1);
}

/*
 * The edited tree against the built one: the same names, the same
 * names on the same objects, the same types, the same attributes and
 * the same bytes. Both walks share one name table, so a name is one
 * id on both sides and the pools can be compared by name.
 */
static void
same_tree(const char *ra, const char *rb)
{
	const struct zr_pool *qa, *qb;
	struct zr_names *ns;
	struct zr_walk wa, wb;
	char err[256];
	uint32_t i, k, n;
	zr_pool_t pa, pb;

	ns = zr_names_create();
	CHECK(ns != NULL);
	err[0] = '\0';
	if (zr_walk(ra, ns, &wa, err, sizeof (err)) != 0)
		printf("  walk %s: %s\n", ra, err);
	CHECK(err[0] == '\0');
	if (zr_walk(rb, ns, &wb, err, sizeof (err)) != 0)
		printf("  walk %s: %s\n", rb, err);
	CHECK(err[0] == '\0');
	CHECK(wa.zw_tree.zt_npools == wb.zw_tree.zt_npools);
	n = zr_names_count(ns);
	for (i = 0; i < n; i++) {
		pa = zr_tree_pool(&wa.zw_tree, (zr_name_t)i);
		pb = zr_tree_pool(&wb.zw_tree, (zr_name_t)i);
		if (pa == ZR_POOL_NONE || pb == ZR_POOL_NONE) {
			printf("  %s: only in %s\n",
			    zr_names_str(ns, (zr_name_t)i, NULL),
			    pa == ZR_POOL_NONE ? rb : ra);
		}
		CHECK(pa != ZR_POOL_NONE && pb != ZR_POOL_NONE);
		qa = &wa.zw_tree.zt_pools[pa];
		qb = &wb.zw_tree.zt_pools[pb];
		CHECK(qa->zp_type == qb->zp_type);
		CHECK(qa->zp_nlink == qb->zp_nlink);
		CHECK(qa->zp_nnames == qb->zp_nnames);
		for (k = 0; k < qa->zp_nnames; k++)
			CHECK(qa->zp_names[k] == qb->zp_names[k]);
		same_attrs(&wa.zw_attrs[pa], &wb.zw_attrs[pb],
		    qa->zp_type);
		if (qa->zp_type == ZR_T_FILE && qa->zp_names[0] == i) {
			same_bytes(ra, rb,
			    zr_names_str(ns, (zr_name_t)i, NULL));
		}
	}
	zr_walk_fini(&wb);
	zr_walk_fini(&wa);
	zr_names_destroy(ns);
}

/*
 * Is this name one the fixture leaves alone between the two trees?
 * In both of them, one content handle -- which folds the type, the
 * bytes and every attribute -- and the same set of names on its
 * pool, which is the editor's own rule read off the spec alone.
 */
static int
spec_same(const struct zr_tree *a, const struct zr_tree *b, zr_name_t nm)
{
	const struct zr_pool *qa, *qb;
	zr_pool_t pa, pb;
	uint32_t i;

	pa = zr_tree_pool(a, nm);
	pb = zr_tree_pool(b, nm);
	if (pa == ZR_POOL_NONE || pb == ZR_POOL_NONE)
		return (0);
	qa = &a->zt_pools[pa];
	qb = &b->zt_pools[pb];
	if (qa->zp_content != qb->zp_content || qa->zp_nnames != qb->zp_nnames)
		return (0);
	for (i = 0; i < qa->zp_nnames; i++) {
		if (qa->zp_names[i] != qb->zp_names[i])
			return (0);
	}
	return (1);
}

/*
 * Does this directory gain or lose a child between the two trees?
 * The kernel moves a directory's ctime when its entries change,
 * whatever the directory itself is, so those are the names whose
 * ctime says nothing about whether the editor touched them.
 */
static int
kids_moved(const struct zr_tree *a, const struct zr_tree *b,
    struct zr_names *ns, zr_name_t nm)
{
	uint32_t i, n;
	int ina, inb;

	n = zr_names_count(ns);
	for (i = 0; i < n; i++) {
		if (zr_names_parent(ns, (zr_name_t)i) != nm)
			continue;
		ina = zr_tree_pool(a, (zr_name_t)i) != ZR_POOL_NONE;
		inb = zr_tree_pool(b, (zr_name_t)i) != ZR_POOL_NONE;
		if (ina != inb)
			return (1);
	}
	return (0);
}

static void
same_stats(const struct zr_fixture_edit_stats *got,
    const struct zr_fixture_edit_stats *want, const char *what)
{
	if (memcmp(got, want, sizeof (*got)) != 0) {
		printf("  %s: removed %llu created %llu rewritten %llu "
		    "relinked %llu attrs %llu untouched %llu\n", what,
		    (unsigned long long)got->ze_removed,
		    (unsigned long long)got->ze_created,
		    (unsigned long long)got->ze_rewritten,
		    (unsigned long long)got->ze_relinked,
		    (unsigned long long)got->ze_attrs,
		    (unsigned long long)got->ze_untouched);
	}
	CHECK(memcmp(got, want, sizeof (*got)) == 0);
}

/*
 * One case, end to end. want is the six counts the edit must
 * report, ino the names whose object must have survived the edit or
 * must not have.
 */
static void
run_edit(const char *root, struct zr_fixture *fx, enum zr_fixture_tree which,
    const char *what, const struct zr_fixture_edit_stats *want,
    const struct inoexp *ino)
{
	struct zr_fixture_edit_stats st, again;
	struct zr_tree base, side;
	const struct snapent *b4, *now;
	struct zr_names *ns;
	struct snap s0, s1;
	char a[PATHMAX], b[PATHMAX], err[256];
	uint64_t sum, names;
	uint32_t i, n;
	int k;

	join(a, sizeof (a), root, "/ed-a");
	join(b, sizeof (b), root, "/ed-b");
	CHECK(mkdir(a, 0755) == 0);
	CHECK(mkdir(b, 0755) == 0);
	build_ok(fx, ZR_FX_BASE, a);
	build_ok(fx, which, b);
	snap_take(&s0, a);

	err[0] = '\0';
	if (zr_fixture_edit(fx, which, a, &st, err, sizeof (err)) != 0)
		printf("  %s: %s\n", what, err);
	CHECK(err[0] == '\0');
	same_stats(&st, want, what);
	same_tree(a, b);
	snap_take(&s1, a);

	/* the two trees as the spec alone gives them, one name table */
	ns = zr_names_create();
	CHECK(ns != NULL);
	CHECK(zr_fixture_to_tree(fx, ZR_FX_BASE, ns, &base) == 0);
	CHECK(zr_fixture_to_tree(fx, which, ns, &side) == 0);
	n = zr_names_count(ns);
	sum = st.ze_removed + st.ze_created + st.ze_rewritten +
	    st.ze_relinked + st.ze_attrs + st.ze_untouched;
	names = 0;
	for (i = 0; i < n; i++) {
		const char *p = zr_names_str(ns, (zr_name_t)i, NULL);
		int inb = zr_tree_pool(&base, (zr_name_t)i) != ZR_POOL_NONE;
		int ins = zr_tree_pool(&side, (zr_name_t)i) != ZR_POOL_NONE;

		if (inb || ins)
			names++;
		if (!inb || !ins)
			continue;
		b4 = snap_find(&s0, p);
		now = snap_find(&s1, p);
		CHECK(b4 != NULL && now != NULL);
		if (spec_same(&base, &side, (zr_name_t)i)) {
			CHECK(b4->se_ino == now->se_ino);
			if (!kids_moved(&base, &side, ns, (zr_name_t)i))
				CHECK(same_ct(&b4->se_ct, &now->se_ct));
		} else {
			/*
			 * Something was done to it, so either it is
			 * another object now or the one it was moved.
			 * The ctime is nanoseconds here; a filesystem
			 * whose clock could not tell the build from
			 * the edit would fail this and should.
			 */
			CHECK(b4->se_ino != now->se_ino ||
			    !same_ct(&b4->se_ct, &now->se_ct));
		}
	}
	if (sum != names)
		printf("  %s: %llu decisions over %llu names\n", what,
		    (unsigned long long)sum, (unsigned long long)names);
	CHECK(sum == names);
	zr_tree_fini(&side);
	zr_tree_fini(&base);
	zr_names_destroy(ns);

	for (k = 0; ino != NULL && ino[k].ix_path != NULL; k++) {
		b4 = snap_find(&s0, ino[k].ix_path);
		now = snap_find(&s1, ino[k].ix_path);
		CHECK(b4 != NULL && now != NULL);
		if ((b4->se_ino == now->se_ino) != (ino[k].ix_kept != 0)) {
			printf("  %s: %s %s its inode\n", what,
			    ino[k].ix_path,
			    ino[k].ix_kept ? "lost" : "kept");
		}
		CHECK((b4->se_ino == now->se_ino) == (ino[k].ix_kept != 0));
	}

	/* the same edit again: nothing left to do, and nothing moved */
	CHECK(zr_fixture_edit(fx, which, a, &again, err,
	    sizeof (err)) == 0);
	CHECK(again.ze_removed == 0 && again.ze_created == 0 &&
	    again.ze_rewritten == 0 && again.ze_relinked == 0 &&
	    again.ze_attrs == 0);
	CHECK(again.ze_untouched == (uint64_t)s1.sn_n);
	snap_free(&s0);
	snap_take(&s0, a);
	CHECK(s0.sn_n == s1.sn_n);
	for (k = 0; k < s1.sn_n; k++) {
		b4 = snap_find(&s0, s1.sn_e[k].se_path);
		CHECK(b4 != NULL);
		CHECK(b4->se_ino == s1.sn_e[k].se_ino);
		CHECK(same_ct(&b4->se_ct, &s1.sn_e[k].se_ct));
	}
	same_tree(a, b);

	snap_free(&s1);
	snap_free(&s0);
	rmtree(a);
	rmtree(b);
}

/* One inline spec run through run_edit and freed. */
static void
edit_case(const char *root, const char *spec, enum zr_fixture_tree which,
    const char *what, const struct zr_fixture_edit_stats *want,
    const struct inoexp *ino)
{
	struct zr_fixture *fx;

	fx = load_spec(root, "/edit.zrt", spec);
	run_edit(root, fx, which, what, want, ino);
	zr_fixture_free(fx);
}

/* One fixture of the suite, loaded and run through run_edit. */
static void
edit_file(const char *root, const char *path, enum zr_fixture_tree which,
    const char *what, const struct zr_fixture_edit_stats *want,
    const struct inoexp *ino)
{
	struct zr_fixture *fx = NULL;
	char err[256];

	err[0] = '\0';
	if (zr_fixture_load(path, &fx, err, sizeof (err)) != 0)
		printf("  %s: %s\n", path, err);
	CHECK(fx != NULL);
	run_edit(root, fx, which, what, want, ino);
	zr_fixture_free(fx);
}

/*
 * ZF33 to ZF35 and ZF45: the probe fixture, which has a name
 * removed, a name created, a file edited, a pool grown by a name and
 * a whole directory replaced by another, and two names -- /keep and
 * /keep/k -- that neither side touches. Both of its sides are
 * edited, since both are what a replay would edit.
 */
static void
check_edit_probe(const char *root)
{
	static const struct inoexp fromino[] = {
		{ "/a", IX_KEPT },		/* rewritten in place */
		{ "/h1", IX_KEPT },		/* the pool grew a name */
		{ "/h2", IX_KEPT },
		{ "/keep", IX_KEPT },
		{ "/keep/k", IX_KEPT },
		{ NULL, 0 }
	};
	static const struct inoexp ontoino[] = {
		{ "/a", IX_KEPT },
		{ "/keep/k", IX_KEPT },
		{ "/h1", IX_KEPT },
		{ "/d/f", IX_KEPT },
		{ NULL, 0 }
	};
	static const struct zr_fixture_edit_stats fromwant = {
		3, 3, 1, 3, 0, 2
	};
	static const struct zr_fixture_edit_stats ontowant = {
		0, 0, 2, 0, 0, 6
	};
	edit_file(root, "tests/fixtures/probe.zrt", ZR_FX_FROM, "probe from",
	    &fromwant, fromino);
	edit_file(root, "tests/fixtures/probe.zrt", ZR_FX_ONTO, "probe onto",
	    &ontowant, ontoino);
}

/*
 * ZF47 to ZF49: three more of the suite, for what the probe has not
 * got. escapes.zrt is every byte the encoding has a rule for in a
 * leaf name, with one name leaving its own pool to join another's;
 * wide-pool.zrt is five names on one object gaining a sixth, where
 * the write must land on the object they all share and not on a copy
 * of it; dir-rm.zrt is a directory three levels deep going, which
 * only comes out right children before parents.
 */
static void
check_edit_more(const char *root)
{
	static const struct zr_fixture_edit_stats w_esc = { 1, 3, 0, 4, 0, 0 };
	static const struct zr_fixture_edit_stats w_wide =
	    { 0, 1, 0, 6, 0, 4 };
	static const struct zr_fixture_edit_stats w_rm = { 7, 0, 0, 0, 0, 1 };
	static const struct inoexp i_esc[] = {
		{ "/a b", IX_KEPT },		/* its pool grew a name */
		{ "/#", IX_KEPT },
		{ NULL, 0 }
	};
	static const struct inoexp i_wide[] = {
		{ "/d1/n1", IX_KEPT }, { "/d1/n2", IX_KEPT },
		{ "/d2/n3", IX_KEPT }, { "/d3/n4", IX_KEPT },
		{ "/d4/n5", IX_KEPT }, { "/d1", IX_KEPT },
		{ NULL, 0 }
	};
	static const struct inoexp i_rm[] = {
		{ "/keep", IX_KEPT }, { NULL, 0 }
	};

	edit_file(root, "tests/fixtures/escapes.zrt", ZR_FX_FROM,
	    "names that need escaping", &w_esc, i_esc);
	edit_file(root, "tests/fixtures/wide-pool.zrt", ZR_FX_FROM,
	    "a pool of five names", &w_wide, i_wide);
	edit_file(root, "tests/fixtures/dir-rm.zrt", ZR_FX_FROM,
	    "a directory three deep removed", &w_rm, i_rm);
}

/* ZF36 to ZF42: one case each, the smallest fixture that says it. */
static void
check_edit_cases(const char *root)
{
	static const struct zr_fixture_edit_stats w_edit = { 0, 0, 1, 0, 0, 1 };
	static const struct zr_fixture_edit_stats w_link = { 0, 0, 0, 3, 0, 0 };
	static const struct zr_fixture_edit_stats w_split =
	    { 0, 0, 0, 2, 0, 0 };
	static const struct zr_fixture_edit_stats w_move = { 1, 1, 0, 0, 0, 0 };
	static const struct zr_fixture_edit_stats w_empty =
	    { 1, 0, 0, 0, 0, 1 };
	static const struct zr_fixture_edit_stats w_type = { 1, 0, 1, 0, 0, 0 };
	static const struct zr_fixture_edit_stats w_sym = { 0, 0, 1, 0, 0, 1 };
	static const struct inoexp i_edit[] = {
		{ "/a", IX_KEPT }, { "/b", IX_KEPT }, { NULL, 0 }
	};
	static const struct inoexp i_link[] = {
		{ "/h1", IX_KEPT }, { "/h2", IX_KEPT }, { NULL, 0 }
	};
	static const struct inoexp i_split[] = {
		{ "/h1", IX_KEPT }, { "/h2", IX_NEW }, { NULL, 0 }
	};
	static const struct inoexp i_empty[] = {
		{ "/d", IX_KEPT }, { NULL, 0 }
	};
	static const struct inoexp i_type[] = {
		{ "/d", IX_NEW }, { NULL, 0 }
	};
	static const struct inoexp i_sym[] = {
		{ "/s", IX_NEW }, { "/k", IX_KEPT }, { NULL, 0 }
	};

	/* ZF36: a file's bytes, written through the name it had */
	edit_case(root,
	    "tree base\n"
	    "\t/a file x\n"
	    "\t/b file y\n"
	    "tree from\n"
	    "\t/a file x2\n"
	    "\t/b file y\n"
	    "tree onto\n", ZR_FX_FROM, "a file edited", &w_edit, i_edit);

	/* ZF37: a name linked onto a pool that stays */
	edit_case(root,
	    "tree base\n"
	    "\t/h1 file h\n"
	    "\t/h2 link /h1\n"
	    "tree from\n"
	    "\t/h1 file h\n"
	    "\t/h2 link /h1\n"
	    "\t/h3 link /h1\n"
	    "tree onto\n", ZR_FX_FROM, "a link added", &w_link, i_link);

	/*
	 * ZF38: a pool broken in two. The pool with the most names on
	 * the object keeps it -- here a tie, which the fixture's own
	 * order settles -- and the other name is made afresh. Neither
	 * is untouched: unlinking /h2 moved the ctime of the object
	 * /h1 is on, which is what the unchanged rule reads.
	 */
	edit_case(root,
	    "tree base\n"
	    "\t/h1 file h\n"
	    "\t/h2 link /h1\n"
	    "tree from\n"
	    "\t/h1 file h\n"
	    "\t/h2 file h\n"
	    "tree onto\n", ZR_FX_FROM, "a pool split", &w_split, i_split);

	/* ZF39: a rename, which the format has no word for */
	edit_case(root,
	    "tree base\n"
	    "\t/a file x\n"
	    "tree from\n"
	    "\t/b file x\n"
	    "tree onto\n", ZR_FX_FROM, "a rename", &w_move, NULL);

	/* ZF40: a directory that loses its child and stays */
	edit_case(root,
	    "tree base\n"
	    "\t/d dir\n"
	    "\t/d/f file f\n"
	    "tree from\n"
	    "\t/d dir\n"
	    "tree onto\n", ZR_FX_FROM, "a directory emptied", &w_empty,
	    i_empty);

	/* ZF41: a directory that becomes a file, its child gone first */
	edit_case(root,
	    "tree base\n"
	    "\t/d dir\n"
	    "\t/d/f file f\n"
	    "tree from\n"
	    "\t/d file t\n"
	    "tree onto\n", ZR_FX_FROM, "a directory to a file", &w_type,
	    i_type);

	/* ZF42: a symlink retargeted, which no filesystem does in place */
	edit_case(root,
	    "tree base\n"
	    "\t/s symlink a\n"
	    "\t/k file t\n"
	    "tree from\n"
	    "\t/s symlink b\n"
	    "\t/k file t\n"
	    "tree onto\n", ZR_FX_FROM, "a symlink retargeted", &w_sym, i_sym);
}

#ifdef HAVE_XATTRS

/* ZF43: one extended attribute changed and nothing else. */
static void
check_edit_xattr(const char *root)
{
	static const struct zr_fixture_edit_stats want = { 0, 0, 0, 0, 1, 0 };
	static const struct inoexp ino[] = {
		{ "/f", IX_KEPT }, { NULL, 0 }
	};

	edit_case(root,
	    "tree base\n"
	    "\t/f file t xattr=user.a:1\n"
	    "tree from\n"
	    "\t/f file t xattr=user.a:2\n"
	    "tree onto\n", ZR_FX_FROM, "an xattr edited", &want, ino);
}

#endif	/* HAVE_XATTRS */

#ifdef HAVE_FFLAGS

/*
 * ZF44: the file flags. First a flag changed on its own, then the
 * ordering the flags force: an immutable directory takes no child
 * and loses none, so its flag comes off before the child under it
 * goes and back on when the edit is over -- while the immutable file
 * beside it, which nothing asks anything of, is never touched at all
 * and keeps its ctime to prove it.
 */
static void
check_edit_flags(const char *root)
{
	static const struct zr_fixture_edit_stats w_one =
	    { 0, 0, 0, 0, 1, 0 };
	static const struct zr_fixture_edit_stats w_dir =
	    { 1, 0, 0, 0, 0, 2 };
	static const struct inoexp i_one[] = {
		{ "/f", IX_KEPT }, { NULL, 0 }
	};
	static const struct zr_fixture_edit_stats w_imm =
	    { 0, 0, 1, 0, 0, 0 };
	static const struct inoexp i_dir[] = {
		{ "/f", IX_KEPT }, { "/d", IX_KEPT }, { NULL, 0 }
	};

	edit_case(root,
	    "tree base\n"
	    "\t/f file t flags=nodump\n"
	    "tree from\n"
	    "\t/f file t flags=uchg\n"
	    "tree onto\n", ZR_FX_FROM, "a flag changed", &w_one, i_one);

	edit_case(root,
	    "tree base\n"
	    "\t/d dir flags=uchg\n"
	    "\t/d/g file g\n"
	    "\t/f file t flags=uchg\n"
	    "tree from\n"
	    "\t/d dir flags=uchg\n"
	    "\t/f file t flags=uchg\n"
	    "tree onto\n", ZR_FX_FROM, "under an immutable directory",
	    &w_dir, i_dir);

	/*
	 * An immutable file whose own bytes change: unflagged,
	 * written, flagged again, and still the object it was.
	 */
	edit_case(root,
	    "tree base\n"
	    "\t/f file t flags=uchg\n"
	    "tree from\n"
	    "\t/f file t2 flags=uchg\n"
	    "tree onto\n", ZR_FX_FROM, "an immutable file edited", &w_imm,
	    i_one);
}

#endif	/* HAVE_FFLAGS */

/*
 * ZF46: a fixture that says "platform freebsd" is edited nowhere
 * else, and says so in the same words a build does.
 */
static void
check_edit_platform(const char *root)
{
	struct zr_fixture *fx = NULL;
	char dir[PATHMAX], err[256];

	err[0] = '\0';
	CHECK(zr_fixture_load("tests/fixtures/freebsd/sysxattr.zrt", &fx, err,
	    sizeof (err)) == 0);
	CHECK(fx != NULL);
	join(dir, sizeof (dir), root, "/edplat");
	CHECK(mkdir(dir, 0755) == 0);
#ifndef __FreeBSD__
	err[0] = '\0';
	errno = 0;
	CHECK(zr_fixture_edit(fx, ZR_FX_FROM, dir, NULL, err,
	    sizeof (err)) == -1);
	CHECK(errno == ENOTSUP);
	CHECK(strstr(err, "platform freebsd") != NULL);
	CHECK(strstr(err, "line 1:") != NULL);
#endif
	CHECK(rmdir(dir) == 0);
	zr_fixture_free(fx);
}

int
main(void)
{
	char root[256];
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

	tmp_template(root, sizeof (root), "zrfix.XXXXXX");
	CHECK(mkdtemp(root) != NULL);
	join(dir, sizeof (dir), root, "/base");
	CHECK(mkdir(dir, 0755) == 0);
	build_ok(fx, ZR_FX_BASE, dir);
	check_built(dir, exp_base, (int)(sizeof (exp_base) /
	    sizeof (exp_base[0])));
	join(dir, sizeof (dir), root, "/from");
	CHECK(mkdir(dir, 0755) == 0);
	build_ok(fx, ZR_FX_FROM, dir);
	check_built(dir, exp_from, (int)(sizeof (exp_from) /
	    sizeof (exp_from[0])));
	join(dir, sizeof (dir), root, "/onto");
	CHECK(mkdir(dir, 0755) == 0);
	build_ok(fx, ZR_FX_ONTO, dir);
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
	check_edit_probe(root);
	check_edit_more(root);
	check_edit_cases(root);
#ifdef HAVE_XATTRS
	check_edit_xattr(root);
#endif
#ifdef HAVE_FFLAGS
	check_edit_flags(root);
#endif
	check_edit_platform(root);

	rmtree(root);
	printf("check_fixture: %d checks passed\n", checks);
	return (0);
}
