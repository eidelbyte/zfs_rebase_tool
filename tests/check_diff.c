/*
 * The diff layer's tests: the text zfs diff prints, parsed, and the
 * pruning it buys. Cells ZX1 to ZX11 and ZC20. ZX1 to ZX5 and ZX11
 * are the captured file tests/data/probe.diff, field by field; ZX6
 * is the escaping, which is zfs diff's own and not the manifest's;
 * ZX9 is the modified directory, which is an M line whose classify
 * column is a slash; ZX10 is every malformed line I can think of.
 * ZX7, ZX8 and ZC20 are zr_diff_apply_unchanged over the probe
 * fixture, built as three real directories and walked, because the
 * oracle it speaks to wants walks and not bare trees: the word it
 * takes goes through a real zr_oracle_unchanged on a real oracle,
 * and the proof that it landed is that a pool whose bytes differ
 * comes out with base's handle anyway, unread.
 *
 * ZX12, the escaping confirmed against a live pool with a torture
 * name, stays with the box probe; nothing here can produce it.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diff.h"
#include "fixture.h"
#include "name.h"
#include "walk.h"
#include "yellow.h"

#define	PATHMAX		1024
#define	SCAN_MIN	16

/* The mountpoint the captured file was printed with. */
#define	PROBE_MNT	"/tmp/zrtdiff-mnt/clone"

/* The mountpoint the written-out diffs below use. */
#define	MNT		"/mnt/from"

/*
 * The diff base->from of the probe fixture: a changed, b removed,
 * the {h1 h2} pool given a third link, n added, the root directory
 * changed, d renamed to e.
 */
static const char probe_text[] =
	"M\tF\t" MNT "/a\n"
	"-\tF\t" MNT "/b\n"
	"M\tF\t" MNT "/h2\t(+1)\n"
	"+\tF\t" MNT "/n\n"
	"M\t/\t" MNT "/\n"
	"R\t/\t" MNT "/d\t" MNT "/e\n";

/*
 * ZX8's diff, which is a lie about the same trees: it says /keep
 * moved and says nothing else. /keep/k is never named, and must
 * still fall with the directory it sits under.
 */
static const char rename_text[] =
	"R\t/\t" MNT "/keep\t" MNT "/kept\n";

/*
 * What the pruning takes off the oracle for the probe diff: the pool
 * of /keep/k, two bytes on base and two on from, is the one pair the
 * oracle would otherwise have opened and read. /keep is a directory,
 * which costs no bytes, and every other pool is either named by the
 * diff or absent from base.
 */
#define	SKIPPED_BYTES	((uint64_t)4)

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

static void
join(char *out, size_t outlen, const char *a, const char *b)
{
	int n;

	n = snprintf(out, outlen, "%s%s", a, b);
	CHECK(n > 0 && (size_t)n < outlen);
}

/* Every name under one root, parents before children. */
struct scan {
	char	**s_path;
	int	*s_isdir;
	int	s_n;
	int	s_cap;
};

static void
scan_push(struct scan *s, const char *rel, int isdir)
{
	char **np;
	int *ni, cap;

	if (s->s_n == s->s_cap) {
		cap = s->s_cap == 0 ? SCAN_MIN : s->s_cap * 2;
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

/* Breadth first and never recursing, as the other tests do it. */
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

/* One diff text as a stream, which is what the parser reads. */
static FILE *
as_stream(const char *text)
{
	FILE *fp;
	size_t n;

	fp = tmpfile();
	CHECK(fp != NULL);
	n = strlen(text);
	if (n > 0)
		CHECK(fwrite(text, 1, n, fp) == n);
	CHECK(fflush(fp) == 0);
	rewind(fp);
	return (fp);
}

static int
parse_text(const char *text, const char *mnt, struct zr_diff *d, char *err,
    size_t errlen)
{
	FILE *fp;
	int rc;

	fp = as_stream(text);
	rc = zr_diff_parse(fp, mnt, d, err, errlen);
	CHECK(fclose(fp) == 0);
	return (rc);
}

static void
check_path(const struct zr_diff_entry *e, const char *want)
{
	CHECK(e->zd_pathlen == strlen(want));
	CHECK(memcmp(e->zd_path, want, e->zd_pathlen) == 0);
}

static void
check_newpath(const struct zr_diff_entry *e, const char *want)
{
	CHECK(e->zd_newlen == strlen(want));
	CHECK(memcmp(e->zd_newpath, want, e->zd_newlen) == 0);
}

/*
 * ZX1 to ZX5, ZX9 and ZX11: the captured file, every field of every
 * line. The root directory's line is a mountpoint and nothing else,
 * and comes back as "/".
 */
static void
check_probe(void)
{
	static const char kind[6] = { 'M', '-', 'M', '+', 'M', 'R' };
	static const char type[6] = { 'F', 'F', 'F', 'F', '/', '/' };
	static const char *const path[6] = {
		"/a", "/b", "/h2", "/n", "/", "/d"
	};
	static const int32_t delta[6] = { 0, 0, 1, 0, 0, 0 };
	struct zr_diff d;
	FILE *fp;
	char err[256];
	int i, moddirs;

	memset(&d, 0, sizeof (d));
	fp = fopen("tests/data/probe.diff", "rb");
	CHECK(fp != NULL);
	err[0] = 'x';
	CHECK(zr_diff_parse(fp, PROBE_MNT, &d, err, sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	CHECK(fclose(fp) == 0);
	CHECK(d.zd_n == 6);
	moddirs = 0;
	for (i = 0; i < 6; i++) {
		CHECK(d.zd_entries[i].zd_kind == kind[i]);
		CHECK(d.zd_entries[i].zd_type == type[i]);
		check_path(&d.zd_entries[i], path[i]);
		CHECK(d.zd_entries[i].zd_linkdelta == delta[i]);
		if (kind[i] == 'R')
			check_newpath(&d.zd_entries[i], "/e");
		else
			CHECK(d.zd_entries[i].zd_newpath == NULL);
		if (d.zd_entries[i].zd_kind == 'M' &&
		    d.zd_entries[i].zd_type == '/')
			moddirs++;
	}
	CHECK(moddirs == 1);
	CHECK(d.zd_entries[4].zd_type == '/');
	zr_diff_fini(&d);
	CHECK(d.zd_n == 0);
	CHECK(d.zd_entries == NULL);
}

/*
 * ZX6: zfs diff's escaping is a backslash and exactly four octal
 * digits for everything that is not printable seven-bit ASCII, so a
 * space is \0040, a backslash is \0134, and one character outside
 * ASCII arrives as its UTF-8 bytes one escape each.
 */
static void
check_escapes(void)
{
	static const char text[] =
	    "M\tF\t" MNT "/a\\0040b\n"
	    "M\tF\t" MNT "/\\0303\\0251\n"
	    "M\tF\t" MNT "/x\\0134y\n"
	    "M\tF\t" MNT "/\\0011tab\n";
	struct zr_diff d;
	char err[256];

	memset(&d, 0, sizeof (d));
	CHECK(parse_text(text, MNT, &d, err, sizeof (err)) == 0);
	CHECK(d.zd_n == 4);
	check_path(&d.zd_entries[0], "/a b");
	check_path(&d.zd_entries[1], "/\303\251");
	CHECK(d.zd_entries[1].zd_pathlen == 3);
	check_path(&d.zd_entries[2], "/x\\y");
	check_path(&d.zd_entries[3], "/\011tab");
	zr_diff_fini(&d);
}

/* ZX10: a bad escape, named by the line it is on. */
static void
check_bad_escape(void)
{
	static const char *const bad[3] = {
		"M\tF\t" MNT "/a\n" "M\tF\t" MNT "/b\\040c\n",
		"M\tF\t" MNT "/a\n" "M\tF\t" MNT "/b\\0089\n",
		"M\tF\t" MNT "/a\n" "M\tF\t" MNT "/b\\012\n"
	};
	struct zr_diff d;
	char err[256];
	int i;

	for (i = 0; i < 3; i++) {
		memset(&d, 0, sizeof (d));
		CHECK(parse_text(bad[i], MNT, &d, err, sizeof (err)) == -1);
		CHECK(strstr(err, "line 2") != NULL);
		CHECK(strstr(err, "escape") != NULL);
		CHECK(d.zd_n == 0);
		CHECK(d.zd_entries == NULL);
		zr_diff_fini(&d);
	}
}

/*
 * ZX10: a path that is not under the mountpoint, named by its line.
 * A prefix that matches but does not end at a slash is not under it
 * either.
 */
static void
check_outside(void)
{
	static const char *const bad[2] = {
		"M\tF\t" MNT "/a\n" "M\tF\t/elsewhere/b\n",
		"M\tF\t" MNT "/a\n" "M\tF\t" MNT "x/b\n"
	};
	struct zr_diff d;
	char err[256];
	int i;

	for (i = 0; i < 2; i++) {
		memset(&d, 0, sizeof (d));
		CHECK(parse_text(bad[i], MNT, &d, err, sizeof (err)) == -1);
		CHECK(strstr(err, "line 2") != NULL);
		CHECK(strstr(err, "mountpoint") != NULL);
		zr_diff_fini(&d);
	}
}

/*
 * ZX5: the link count delta, both ways. A dataset mounted at "/" is
 * the one case where the printed path holds a doubled slash, since
 * libzfs joins the mountpoint and the path without looking; it
 * collapses.
 */
static void
check_delta(void)
{
	static const char text[] =
	    "M\tF\t" MNT "/x\t(-2)\n"
	    "M\tF\t" MNT "/y\t(+12)\n";
	static const char root_text[] =
	    "M\tF\t//x\n"
	    "M\t/\t//\n";
	struct zr_diff d;
	char err[256];

	memset(&d, 0, sizeof (d));
	CHECK(parse_text(text, MNT, &d, err, sizeof (err)) == 0);
	CHECK(d.zd_n == 2);
	CHECK(d.zd_entries[0].zd_linkdelta == -2);
	check_path(&d.zd_entries[0], "/x");
	CHECK(d.zd_entries[1].zd_linkdelta == 12);
	zr_diff_fini(&d);

	memset(&d, 0, sizeof (d));
	CHECK(parse_text(root_text, "/", &d, err, sizeof (err)) == 0);
	CHECK(d.zd_n == 2);
	check_path(&d.zd_entries[0], "/x");
	check_path(&d.zd_entries[1], "/");
	zr_diff_fini(&d);
}

/* ZX4 and ZX6 together: the new path of a rename is escaped too. */
static void
check_rename(void)
{
	static const char text[] =
	    "R\t/\t" MNT "/d\t" MNT "/e\\0040f\n"
	    "R\tF\t" MNT "/g\\0040h\t" MNT "/i\n";
	struct zr_diff d;
	char err[256];

	memset(&d, 0, sizeof (d));
	CHECK(parse_text(text, MNT, &d, err, sizeof (err)) == 0);
	CHECK(d.zd_n == 2);
	CHECK(d.zd_entries[0].zd_kind == 'R');
	CHECK(d.zd_entries[0].zd_type == '/');
	check_path(&d.zd_entries[0], "/d");
	check_newpath(&d.zd_entries[0], "/e f");
	check_path(&d.zd_entries[1], "/g h");
	check_newpath(&d.zd_entries[1], "/i");
	zr_diff_fini(&d);
}

/* ZX10: every other way a line can be wrong. */
static void
check_malformed(void)
{
	static const char *const bad[8] = {
		"M\tF\n",
		"M\tF\t" MNT "/a\t(+1)\tmore\n",
		"X\tF\t" MNT "/a\n",
		"MM\tF\t" MNT "/a\n",
		"M\tZ\t" MNT "/a\n",
		"R\tF\t" MNT "/a\n",
		"+\tF\t" MNT "/a\t(+1)\n",
		"M\tF\t" MNT "/a\t(+x)\n"
	};
	struct zr_diff d;
	char err[256];
	int i;

	for (i = 0; i < 8; i++) {
		memset(&d, 0, sizeof (d));
		err[0] = '\0';
		CHECK(parse_text(bad[i], MNT, &d, err, sizeof (err)) == -1);
		CHECK(strstr(err, "line 1") != NULL);
		CHECK(d.zd_n == 0);
		zr_diff_fini(&d);
	}
	/* An empty file is a diff with nothing in it, not an error. */
	memset(&d, 0, sizeof (d));
	CHECK(parse_text("", MNT, &d, err, sizeof (err)) == 0);
	CHECK(d.zd_n == 0);
	zr_diff_fini(&d);
}

/* The probe fixture as three walked directories under one oracle. */
struct trees {
	char			t_root[PATHMAX];
	struct zr_names		*t_ns;
	struct zr_walk		t_w[3];
	struct zr_oracle	*t_o;
};

/*
 * Build the fixture's three trees, walk them into one shared name
 * table and open an oracle over the walks. tweak rewrites from's
 * /keep/k with different bytes of the same length, which is how the
 * pruning is proved: without the diff's word the oracle reads that
 * pair and calls it different.
 */
static void
trees_open(struct trees *t, int tweak)
{
	static const char *const sub[3] = { "/base", "/from", "/onto" };
	struct zr_fixture *fx;
	char dir[PATHMAX], err[512];
	FILE *fp;
	int i;

	(void) strcpy(t->t_root, "/tmp/zrdiff.XXXXXX");
	CHECK(mkdtemp(t->t_root) != NULL);
	CHECK(zr_fixture_load("tests/fixtures/probe.zrt", &fx, err,
	    sizeof (err)) == 0);
	for (i = 0; i < 3; i++) {
		join(dir, sizeof (dir), t->t_root, sub[i]);
		CHECK(mkdir(dir, 0755) == 0);
		CHECK(zr_fixture_build(fx, (enum zr_fixture_tree)i,
		    dir) == 0);
	}
	zr_fixture_free(fx);
	if (tweak) {
		join(dir, sizeof (dir), t->t_root, "/from/keep/k");
		fp = fopen(dir, "wb");
		CHECK(fp != NULL);
		CHECK(fputs("K\n", fp) != EOF);
		CHECK(fclose(fp) == 0);
	}
	t->t_ns = zr_names_create();
	CHECK(t->t_ns != NULL);
	for (i = 0; i < 3; i++) {
		join(dir, sizeof (dir), t->t_root, sub[i]);
		CHECK(zr_walk(dir, t->t_ns, &t->t_w[i], err,
		    sizeof (err)) == 0);
	}
	CHECK(zr_oracle_init(&t->t_o, &t->t_w[0], &t->t_w[1],
	    &t->t_w[2]) == 0);
}

static void
trees_close(struct trees *t)
{
	int i;

	zr_oracle_fini(t->t_o);
	for (i = 2; i >= 0; i--)
		zr_walk_fini(&t->t_w[i]);
	zr_names_destroy(t->t_ns);
	rmtree(t->t_root);
}

/* The content handles of one path on base and on from. */
static void
handles(const struct trees *t, const char *path, uint32_t *out)
{
	zr_name_t nm;
	zr_pool_t p;
	int i;

	nm = zr_names_lookup(t->t_ns, path, strlen(path));
	CHECK(nm != ZR_NAME_NONE);
	for (i = 0; i < 2; i++) {
		p = zr_tree_pool(&t->t_w[i].zw_tree, nm);
		CHECK(p != ZR_POOL_NONE);
		out[i] = t->t_w[i].zw_tree.zt_pools[p].zp_content;
		CHECK(out[i] != ZR_CONTENT_NONE);
	}
}

struct result {
	int		r_marked;
	uint64_t	r_bytes;
	uint32_t	r_k[2];		/* /keep/k on base and from */
	uint32_t	r_a[2];		/* /a on base and from */
};

/*
 * One whole pass: build, prune with text when it is not NULL, assign
 * and read back. The oracle is the real one, so the pruning's word
 * goes through zr_oracle_unchanged and shows up as a handle.
 */
static void
run_case(int tweak, const char *text, struct result *r)
{
	struct trees t;
	struct zr_diff d;
	char err[512];

	memset(r, 0, sizeof (*r));
	memset(&d, 0, sizeof (d));
	trees_open(&t, tweak);
	if (text != NULL) {
		CHECK(parse_text(text, MNT, &d, err, sizeof (err)) == 0);
		r->r_marked = zr_diff_apply_unchanged(&d, &t.t_w[0],
		    &t.t_w[1], 1, t.t_o);
		CHECK(r->r_marked >= 0);
	}
	CHECK(zr_oracle_assign(t.t_o, err, sizeof (err)) == 0);
	r->r_bytes = zr_oracle_bytes_read(t.t_o);
	handles(&t, "/keep/k", r->r_k);
	handles(&t, "/a", r->r_a);
	zr_diff_fini(&d);
	trees_close(&t);
}

/*
 * ZX7, ZX8 and ZC20. Exactly two pools of from go unreported by the
 * probe diff and are present in base under one pool: /keep and
 * /keep/k. Everything else is named -- a, the root and h2 outright,
 * b as removed, n as added, d and e as the rename -- or has no pool
 * in base at all, which is /e, /e/f, /n and the {h1 h2 h3} pool that
 * gained a name.
 */
static void
check_apply(void)
{
	struct result plain, pruned, lied;

	run_case(0, NULL, &plain);
	run_case(0, probe_text, &pruned);
	CHECK(pruned.r_marked == 2);
	CHECK(plain.r_bytes - pruned.r_bytes == SKIPPED_BYTES);
	CHECK(plain.r_k[0] == plain.r_k[1]);
	CHECK(pruned.r_k[0] == pruned.r_k[1]);
	CHECK(plain.r_a[0] != plain.r_a[1]);
	CHECK(pruned.r_a[0] != pruned.r_a[1]);

	/*
	 * The same two pools, now with from's /keep/k holding other
	 * bytes of the same length. Unpruned the oracle reads them
	 * and says they differ; pruned it never opens them and they
	 * carry base's handle. That is the fast path, and it names
	 * which pools were marked and not just how many.
	 */
	run_case(1, NULL, &plain);
	run_case(1, probe_text, &pruned);
	CHECK(pruned.r_marked == 2);
	CHECK(plain.r_bytes - pruned.r_bytes == SKIPPED_BYTES);
	CHECK(plain.r_k[0] != plain.r_k[1]);
	CHECK(pruned.r_k[0] == pruned.r_k[1]);
	CHECK(pruned.r_a[0] != pruned.r_a[1]);

	/*
	 * ZX8. The invented rename names /keep and nothing else, so
	 * /keep/k, which no line mentions, falls with it and keeps
	 * its own bytes; a and the root, which nothing names now,
	 * are the two pools marked instead, and a comes out with
	 * base's handle although its bytes differ in length.
	 */
	run_case(1, rename_text, &lied);
	CHECK(lied.r_marked == 2);
	CHECK(lied.r_a[0] == lied.r_a[1]);
	CHECK(lied.r_k[0] != lied.r_k[1]);
}

/* The arguments the pruning refuses rather than guesses at. */
static void
check_apply_args(void)
{
	struct trees t;
	struct zr_diff d;
	char err[512];

	memset(&d, 0, sizeof (d));
	trees_open(&t, 0);
	CHECK(parse_text(probe_text, MNT, &d, err, sizeof (err)) == 0);
	CHECK(zr_diff_apply_unchanged(NULL, &t.t_w[0], &t.t_w[1], 1,
	    t.t_o) == -1);
	CHECK(zr_diff_apply_unchanged(&d, NULL, &t.t_w[1], 1, t.t_o) == -1);
	CHECK(zr_diff_apply_unchanged(&d, &t.t_w[0], NULL, 1, t.t_o) == -1);
	CHECK(zr_diff_apply_unchanged(&d, &t.t_w[0], &t.t_w[1], 1,
	    NULL) == -1);
	CHECK(zr_diff_apply_unchanged(&d, &t.t_w[0], &t.t_w[1], 0,
	    t.t_o) == -1);
	CHECK(zr_diff_apply_unchanged(&d, &t.t_w[0], &t.t_w[1], 3,
	    t.t_o) == -1);
	/* onto is the other side, and it is marked against base too. */
	CHECK(zr_diff_apply_unchanged(&d, &t.t_w[0], &t.t_w[2], 2,
	    t.t_o) >= 0);
	zr_diff_fini(&d);
	trees_close(&t);
}

int
main(void)
{
	(void) umask(022);
	check_probe();
	check_escapes();
	check_bad_escape();
	check_outside();
	check_delta();
	check_rename();
	check_malformed();
	check_apply();
	check_apply_args();
	printf("check_diff: %d checks passed\n", checks);
	return (0);
}
