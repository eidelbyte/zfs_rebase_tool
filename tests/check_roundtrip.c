/*
 * Every fixture, round tripped. For each .zrt file of
 * tests/fixtures the three trees are built as directories, walked
 * into one name table, given content and decided in the fixture's
 * own mode; the manifest is emitted, parsed back and written out
 * again, and the bytes must be the ones the emitter wrote. Then
 * the fixture's expect block is parsed and held against the parse
 * of the emitted text, action by action and record by record --
 * everything but the three dataset lines, which name temporary
 * directories here and datasets on the box.
 *
 * The fixtures are found by scanning the directory, so a fixture
 * added later is round tripped by this test the day it lands. Cells
 * ZV19, ZM42 and ZM51.
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

#include "decide.h"
#include "fixture.h"
#include "manifest.h"
#include "name.h"
#include "walk.h"
#include "yellow.h"

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
#define	GROW		16
#define	FIXDIR		"tests/fixtures"
#define	DOTZRT		".zrt"
#define	PERMISSIVE	"-permissive.zrt"

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

/* A growing list of names: the fixtures, and one scan of a tree. */
struct list {
	char	**l_name;
	int	*l_isdir;
	int	l_n;
	int	l_cap;
};

static void
list_add(struct list *l, const char *name, int isdir)
{
	char **np;
	int *ni, cap;

	if (l->l_n == l->l_cap) {
		cap = l->l_cap == 0 ? GROW : l->l_cap * 2;
		np = realloc(l->l_name, (size_t)cap * sizeof (char *));
		CHECK(np != NULL);
		l->l_name = np;
		ni = realloc(l->l_isdir, (size_t)cap * sizeof (int));
		CHECK(ni != NULL);
		l->l_isdir = ni;
		l->l_cap = cap;
	}
	l->l_name[l->l_n] = malloc(strlen(name) + 1);
	CHECK(l->l_name[l->l_n] != NULL);
	(void) strcpy(l->l_name[l->l_n], name);
	l->l_isdir[l->l_n] = isdir;
	l->l_n++;
}

static void
list_free(struct list *l)
{
	int i;

	for (i = 0; i < l->l_n; i++)
		free(l->l_name[i]);
	free(l->l_name);
	free(l->l_isdir);
	memset(l, 0, sizeof (*l));
}

static void
scan_dir(struct list *l, const char *root, const char *rel)
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
		list_add(l, child, S_ISDIR(st.st_mode) ? 1 : 0);
	}
	CHECK(closedir(d) == 0);
}

/* Children before parents, which is the scan order reversed. */
static void
rmtree(const char *root)
{
	char full[PATHMAX];
	struct list l;
	int i;

	memset(&l, 0, sizeof (l));
	scan_dir(&l, root, "");
	for (i = 0; i < l.l_n; i++) {
		if (l.l_isdir[i])
			scan_dir(&l, root, l.l_name[i]);
	}
	for (i = l.l_n - 1; i >= 0; i--) {
		join(full, sizeof (full), root, l.l_name[i]);
		if (l.l_isdir[i])
			CHECK(rmdir(full) == 0);
		else
			CHECK(unlink(full) == 0);
	}
	list_free(&l);
	CHECK(rmdir(root) == 0);
}

static int
name_cmp(const void *a, const void *b)
{
	const char *const *x = a, *const *y = b;

	return (strcmp(*x, *y));
}

/*
 * Every fixture the directory holds, in name order. Only the names
 * are sorted: the other column of the list belongs to the tree scan
 * and a fixture is never a directory.
 */
static void
list_fixtures(struct list *l)
{
	struct dirent *de;
	size_t n, k = strlen(DOTZRT);
	DIR *d;

	memset(l, 0, sizeof (*l));
	d = opendir(FIXDIR);
	if (d == NULL)
		printf("  %s: %s\n", FIXDIR, strerror(errno));
	CHECK(d != NULL);
	while ((de = readdir(d)) != NULL) {
		n = strlen(de->d_name);
		if (n > k && strcmp(de->d_name + n - k, DOTZRT) == 0)
			list_add(l, de->d_name, 0);
	}
	CHECK(closedir(d) == 0);
	qsort(l->l_name, (size_t)l->l_n, sizeof (char *), name_cmp);
}

/* The mode the file name asks for. */
static zr_mode_t
mode_of(const char *name)
{
	size_t n = strlen(name), k = strlen(PERMISSIVE);

	if (n >= k && strcmp(name + n - k, PERMISSIVE) == 0)
		return (ZR_MODE_PERMISSIVE);
	return (ZR_MODE_STRICT);
}

/* Read one temporary file back whole and close it. */
static char *
slurp(FILE *f, size_t *lenp)
{
	char *buf;
	long n;

	CHECK(fflush(f) == 0);
	CHECK(fseek(f, 0, SEEK_END) == 0);
	n = ftell(f);
	CHECK(n >= 0);
	CHECK(fseek(f, 0, SEEK_SET) == 0);
	buf = malloc((size_t)n + 1);
	CHECK(buf != NULL);
	CHECK(fread(buf, 1, (size_t)n, f) == (size_t)n);
	buf[n] = '\0';
	(void) fclose(f);
	*lenp = (size_t)n;
	return (buf);
}

/* Byte for byte, and on a mismatch print both and where they part. */
static void
compare(const char *tag, const char *got, size_t gotlen, const char *want,
    size_t wantlen)
{
	size_t i;

	checks++;
	if (gotlen == wantlen && memcmp(got, want, gotlen) == 0)
		return;
	for (i = 0; i < gotlen && i < wantlen && got[i] == want[i]; i++)
		continue;
	printf("%s: the manifest differs at byte %lu\n", tag,
	    (unsigned long)i);
	printf("--- want, %lu bytes ---\n%.*s", (unsigned long)wantlen,
	    (int)wantlen, want);
	printf("--- got, %lu bytes ---\n%.*s", (unsigned long)gotlen,
	    (int)gotlen, got);
	printf("--- end ---\n");
	exit(1);
}

/* One manifest that must parse, through a file as the parser wants. */
static void
parse_ok(const char *tag, const char *text, size_t len,
    struct zr_parsed *out)
{
	char err[256];
	FILE *f;
	int rc;

	err[0] = '\0';
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fwrite(text, 1, len, f) == len);
	CHECK(fflush(f) == 0);
	CHECK(fseek(f, 0, SEEK_SET) == 0);
	rc = zr_manifest_parse(f, out, err, sizeof (err));
	(void) fclose(f);
	checks++;
	if (rc != 0) {
		printf("%s: the parse failed: %s\n", tag, err);
		exit(1);
	}
}

/*
 * Two parses of one manifest, field by field. The three dataset
 * lines are left out: the emitter was handed the temporary
 * directories the trees were built in, and the fixture names the
 * datasets the box will use.
 */
static void
same_parse(const char *tag, const struct zr_parsed *a,
    const struct zr_parsed *b)
{
	uint32_t i;

	checks++;
	if (a->zp_nactions != b->zp_nactions ||
	    a->zp_nrecords != b->zp_nrecords) {
		printf("%s: %u actions and %u records against %u and %u\n",
		    tag, a->zp_nactions, a->zp_nrecords, b->zp_nactions,
		    b->zp_nrecords);
		exit(1);
	}
	CHECK(a->zp_mode == b->zp_mode);
	CHECK(a->zp_actions_declared == b->zp_actions_declared);
	CHECK(a->zp_conflicts_declared == b->zp_conflicts_declared);
	for (i = 0; i < a->zp_nactions; i++) {
		const struct zr_action *x = &a->zp_actions[i];
		const struct zr_action *y = &b->zp_actions[i];

		CHECK(x->za_kind == y->za_kind);
		CHECK(x->za_pathlen == y->za_pathlen);
		CHECK(memcmp(x->za_path, y->za_path, x->za_pathlen) == 0);
		CHECK((x->za_arg == NULL) == (y->za_arg == NULL));
		CHECK(x->za_arglen == y->za_arglen);
		CHECK(x->za_arg == NULL ||
		    memcmp(x->za_arg, y->za_arg, x->za_arglen) == 0);
		CHECK(x->za_isdir == y->za_isdir);
		CHECK(x->za_conflict == y->za_conflict);
	}
	for (i = 0; i < a->zp_nrecords; i++) {
		const struct zr_record *x = &a->zp_records[i];
		const struct zr_record *y = &b->zp_records[i];

		CHECK(x->zr_num == y->zr_num);
		CHECK(x->zr_flags == y->zr_flags);
		CHECK(strcmp(x->zr_why, y->zr_why) == 0);
		CHECK(strcmp(x->zr_base, y->zr_base) == 0);
		CHECK(strcmp(x->zr_from, y->zr_from) == 0);
		CHECK(strcmp(x->zr_onto, y->zr_onto) == 0);
	}
}

/* The three trees of one fixture, built, walked and decided. */
struct run {
	char			r_dir[3][PATHMAX];
	struct zr_names		*r_ns;
	struct zr_walk		r_w[3];
	struct zr_oracle	*r_oracle;
	struct zr_decision	r_d;
};

static void
run_build(struct run *r, const struct zr_fixture *fx, const char *tag,
    const char *root, zr_mode_t mode)
{
	static const char *const sub[3] = { "/base", "/from", "/onto" };
	char err[512];
	int i;

	memset(r, 0, sizeof (*r));
	for (i = 0; i < 3; i++) {
		join(r->r_dir[i], sizeof (r->r_dir[i]), root, sub[i]);
		CHECK(mkdir(r->r_dir[i], 0755) == 0);
		CHECK(zr_fixture_build(fx, (enum zr_fixture_tree)i,
		    r->r_dir[i]) == 0);
	}
	r->r_ns = zr_names_create();
	CHECK(r->r_ns != NULL);
	for (i = 0; i < 3; i++) {
		err[0] = '\0';
		if (zr_walk(r->r_dir[i], r->r_ns, &r->r_w[i], err,
		    sizeof (err)) != 0)
			printf("%s: walk: %s\n", tag, err);
		CHECK(err[0] == '\0');
	}
	CHECK(zr_oracle_init(&r->r_oracle, &r->r_w[0], &r->r_w[1],
	    &r->r_w[2]) == 0);
	err[0] = '\0';
	if (zr_oracle_assign(r->r_oracle, err, sizeof (err)) != 0)
		printf("%s: content: %s\n", tag, err);
	CHECK(err[0] == '\0');
	CHECK(zr_decide(&r->r_w[0].zw_tree, &r->r_w[1].zw_tree,
	    &r->r_w[2].zw_tree, mode, &r->r_d) == 0);
}

static void
run_fini(struct run *r)
{
	int i;

	zr_decision_fini(&r->r_d);
	zr_oracle_fini(r->r_oracle);
	for (i = 0; i < 3; i++)
		zr_walk_fini(&r->r_w[i]);
	zr_names_destroy(r->r_ns);
}

/*
 * One fixture: emit, parse, write, and the expect block against the
 * parse of what was emitted.
 */
static void
one(const char *name)
{
	char tmpl[256];
	char path[PATHMAX], err[512];
	struct zr_manifest_hdr hdr;
	struct zr_fixture *fx = NULL;
	struct zr_parsed pa, pb;
	struct run r;
	const char *expect;
	char *got, *back;
	size_t gotlen = 0, backlen = 0;
	FILE *f;

	join(path, sizeof (path), FIXDIR "/", name);
	err[0] = '\0';
	if (zr_fixture_load(path, &fx, err, sizeof (err)) != 0)
		printf("%s: %s\n", path, err);
	CHECK(fx != NULL);
	tmp_template(tmpl, sizeof (tmpl), "zrround.XXXXXX");
	CHECK(mkdtemp(tmpl) != NULL);
	hdr.mode = mode_of(name);
	run_build(&r, fx, path, tmpl, hdr.mode);
	hdr.base = r.r_dir[0];
	hdr.from = r.r_dir[1];
	hdr.onto = r.r_dir[2];
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_emit(f, &hdr, &r.r_w[0].zw_tree,
	    &r.r_w[1].zw_tree, &r.r_w[2].zw_tree, &r.r_d) == 0);
	got = slurp(f, &gotlen);
	parse_ok(path, got, gotlen, &pa);
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_parsed_write(f, &pa) == 0);
	back = slurp(f, &backlen);
	compare(path, back, backlen, got, gotlen);
	free(back);
	expect = zr_fixture_expect(fx);
	if (expect == NULL) {
		printf("%s: no expect block\n", path);
	} else {
		parse_ok(path, expect, strlen(expect), &pb);
		same_parse(path, &pa, &pb);
		zr_parsed_fini(&pb);
	}
	zr_parsed_fini(&pa);
	free(got);
	run_fini(&r);
	zr_fixture_free(fx);
	rmtree(tmpl);
}

int
main(void)
{
	struct list l;
	int i;

	list_fixtures(&l);
	CHECK(l.l_n > 0);
	for (i = 0; i < l.l_n; i++)
		one(l.l_name[i]);
	printf("check_roundtrip: %d checks passed over %d fixtures\n",
	    checks, l.l_n);
	list_free(&l);
	return (0);
}
