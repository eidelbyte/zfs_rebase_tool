/*
 * The fixture tests: the probe scenario of v4-manifest.md section 7
 * parsed, built as real directories and walked back, and built again
 * as pools in memory; a second inline fixture for the symlinks and
 * attributes the probe has none of; then every rejection the format
 * promises.
 */

#define	_XOPEN_SOURCE	700
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
	{ "/dd/s", E_SYM, "t", NULL, 1, -1 }
};

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

/* Children before parents, which is the walk order reversed. */
static void
rmtree(const char *root)
{
	char full[PATHMAX];
	struct walk w;
	int i;

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
 * The pools the spec implies, without a filesystem: the hardlink pool
 * of base holds h1 and h2, the one of from holds h1, h2 and h3, and
 * the content handles say which files share bytes across the trees.
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

	/* the directories share one handle, and no file wears it */
	CHECK(content(&base, ns, "/d") == ZR_FX_DIR_CONTENT);
	CHECK(content(&base, ns, "/keep") == ZR_FX_DIR_CONTENT);
	CHECK(content(&from, ns, "/e") == ZR_FX_DIR_CONTENT);
	CHECK(content(&base, ns, "/a") < ZR_FX_DIR_CONTENT);
	CHECK(content(&onto, ns, "/keep/k") < ZR_FX_DIR_CONTENT);
	CHECK(content(&base, ns, "/a") != ZR_CONTENT_NONE);

	zr_tree_fini(&base);
	zr_tree_fini(&from);
	zr_tree_fini(&onto);
	zr_names_destroy(ns);
}

/*
 * The second fixture: a symlink target that needs escaping, a symlink
 * whose target is also a file's token, and the three attributes.
 */
static void
check_extra(const char *root)
{
	char spec[512], path[PATHMAX], full[PATHMAX], err[256];
	struct zr_fixture *fx = NULL;
	struct zr_names *ns;
	struct zr_tree tr;
	struct stat st;
	FILE *fp;
	size_t len;
	int n;

	n = snprintf(spec, sizeof (spec),
	    "# a symlink, an escaped target and the attributes\n"
	    "tree base\n"
	    "\t/l symlink /a\\040b\n"
	    "\t/f file t mode=0600\n"
	    "\t/dd dir mode=0700\n"
	    "\t/dd/s symlink t mode=0777 uid=%lu gid=%lu\n"
	    "tree from\n"
	    "tree onto\n",
	    (unsigned long)getuid(), (unsigned long)getgid());
	CHECK(n > 0 && (size_t)n < sizeof (spec));
	len = (size_t)n;
	join(path, sizeof (path), root, "/extra.zrt");
	fp = fopen(path, "wb");
	CHECK(fp != NULL);
	CHECK(fwrite(spec, 1, len, fp) == len);
	CHECK(fclose(fp) == 0);

	CHECK(zr_fixture_load(path, &fx, err, sizeof (err)) == 0);
	CHECK(fx != NULL);
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
	CHECK(content(&tr, ns, "/dd") == ZR_FX_DIR_CONTENT);
	CHECK((content(&tr, ns, "/l") & ZR_FX_SYMLINK_BIT) != 0);
	CHECK(content(&tr, ns, "/l") != content(&tr, ns, "/dd/s"));
	/*
	 * The symlink to "t" and the file whose token is "t" agree
	 * only below the high bit.
	 */
	CHECK(content(&tr, ns, "/dd/s") ==
	    (content(&tr, ns, "/f") | ZR_FX_SYMLINK_BIT));
	CHECK(content(&tr, ns, "/f") != content(&tr, ns, "/dd/s"));
	zr_tree_fini(&tr);
	zr_names_destroy(ns);

	zr_fixture_free(fx);
	rmtree(full);
	join(path, sizeof (path), root, "/extra.zrt");
	CHECK(unlink(path) == 0);
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
	check_rejections(root);

	rmtree(root);
	printf("check_fixture: %d checks passed\n", checks);
	return (0);
}
