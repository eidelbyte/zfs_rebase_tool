/*
 * The verify tests. First the outcome grid, one small case at a
 * time: three directories built by hand -- onto as the rebase found
 * it, from as the manifest reads it, and a result doctored into the
 * state under test -- a manifest written as text, and the
 * classification held against what the state says. Every action kind
 * is taken through done, pending and drifted, the directory removal
 * over a conflicted name through blocked, and the information line
 * through an edit, an addition, a name a conflict covers and a name
 * that is simply still onto's.
 *
 * Then the whole thing over one scenario: three trees walked,
 * decided, emitted and parsed, applied to a copy of onto, classified,
 * doctored, classified again, applied again with the report as the
 * list of what to leave alone, and classified once more. The
 * scenario's manifest holds a directory whose removal a conflicted
 * child blocks, which is the shape sprint 4 never applied. Last, the
 * idempotence itself: the manifest applied twice over a pristine copy
 * lands in one place, name for name, pool for pool and byte for byte.
 *
 * The family is ZY of tests/MATRIX.md, and ZC25 of the oracle's is
 * here too, since the pair entry point this classifier asks
 * everything through is new with it. Covered: ZY1 through ZY36.
 * ZY37, ZY38 and ZY39 are the box's -- a real ACL and both extended
 * attribute namespaces in a comparison, a kill at a gate, and the
 * dataset form.
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

#include "apply.h"
#include "decide.h"
#include "manifest.h"
#include "name.h"
#include "verify.h"
#include "walk.h"
#include "yellow.h"

#define	PATHMAX		1024
#define	TEXTMAX		8192
#define	SCAN_MIN	16

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

/* Every name a scan found under one root, parents before children. */
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

static void
mkfile(const char *root, const char *rel, const char *text, mode_t mode)
{
	char full[PATHMAX];
	size_t len;
	int fd;

	join(full, sizeof (full), root, rel);
	fd = open(full, O_CREAT | O_TRUNC | O_WRONLY, mode);
	CHECK(fd >= 0);
	len = strlen(text);
	CHECK(write(fd, text, len) == (ssize_t)len);
	CHECK(close(fd) == 0);
	CHECK(chmod(full, mode) == 0);
}

static void
mkdirp(const char *root, const char *rel, mode_t mode)
{
	char full[PATHMAX];

	join(full, sizeof (full), root, rel);
	CHECK(mkdir(full, mode) == 0);
	CHECK(chmod(full, mode) == 0);
}

static void
mklink(const char *root, const char *rel, const char *torel)
{
	char full[PATHMAX], to[PATHMAX];

	join(full, sizeof (full), root, rel);
	join(to, sizeof (to), root, torel);
	CHECK(link(to, full) == 0);
}

static void
rmname(const char *root, const char *rel)
{
	char full[PATHMAX];

	join(full, sizeof (full), root, rel);
	CHECK(unlink(full) == 0);
}

static void
chmodp(const char *root, const char *rel, mode_t mode)
{
	char full[PATHMAX];

	join(full, sizeof (full), root, rel);
	CHECK(chmod(full, mode) == 0);
}

static int
absent(const char *root, const char *rel)
{
	char full[PATHMAX];
	struct stat st;

	join(full, sizeof (full), root, rel);
	return (lstat(full, &st) != 0 && errno == ENOENT);
}

/*
 * The manifest document around one tree section body and, when the
 * body marks a conflict, the records it points at. The header is the
 * least a parse will take and both counts must be right, since the
 * parse checks them against the lines.
 */
static void
parse_doc(struct zr_parsed *p, const char *body, int nactions, int nconf,
    const char *records)
{
	char text[TEXTMAX], err[256];
	FILE *f;
	int n;

	n = snprintf(text, sizeof (text),
	    "#rebase-manifest 4\n"
	    "#base b\n"
	    "#from f\n"
	    "#onto o\n"
	    "#mode strict\n"
	    "#actions %d\n"
	    "#conflicts %d\n"
	    "/\n"
	    "%s"
	    "..\n"
	    "%s", nactions, nconf, body, records);
	CHECK(n > 0 && (size_t)n < sizeof (text));
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fputs(text, f) != EOF);
	rewind(f);
	err[0] = '\0';
	if (zr_manifest_parse(f, p, err, sizeof (err)) != 0)
		printf("  parse: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(fclose(f) == 0);
}

/* The index of the action on one path, which the tests name by hand. */
static uint32_t
idx_of(const struct zr_parsed *p, const char *path)
{
	uint32_t i;

	for (i = 0; i < p->zp_nactions; i++) {
		if (strcmp((const char *)p->zp_actions[i].za_path, path) == 0)
			return (i);
	}
	printf("  no action on %s\n", path);
	CHECK(0);
	return (0);
}

/*
 * ---------------------------------------------------------------
 * The outcome grid: one onto tree, one from tree, one result tree,
 * all three by hand, and a manifest written as text.
 * ---------------------------------------------------------------
 */

struct vshape {
	char		vs_root[PATHMAX];
	char		vs_onto[PATHMAX];
	char		vs_from[PATHMAX];
	char		vs_res[PATHMAX];
	struct zr_names	*vs_ns;
};

static void
vshape_init(struct vshape *v)
{
	char tmpl[] = "/tmp/zrverify.XXXXXX";

	memset(v, 0, sizeof (*v));
	CHECK(mkdtemp(tmpl) != NULL);
	join(v->vs_root, sizeof (v->vs_root), tmpl, "");
	join(v->vs_onto, sizeof (v->vs_onto), tmpl, "/onto");
	join(v->vs_from, sizeof (v->vs_from), tmpl, "/from");
	join(v->vs_res, sizeof (v->vs_res), tmpl, "/res");
	CHECK(mkdir(v->vs_onto, 0755) == 0);
	CHECK(mkdir(v->vs_from, 0755) == 0);
	CHECK(mkdir(v->vs_res, 0755) == 0);
	v->vs_ns = zr_names_create();
	CHECK(v->vs_ns != NULL);
}

static void
vshape_fini(struct vshape *v)
{
	zr_names_destroy(v->vs_ns);
	rmtree(v->vs_root);
}

/*
 * Walk the three, build an oracle over them in the order verify
 * wants -- onto, from, result -- and classify. zr_oracle_assign is
 * never called: the classifier asks pairs, and that is the whole of
 * what it needs the oracle for.
 */
static void
vshape_run(struct vshape *v, const char *body, int nactions, int nconf,
    const char *records, struct zr_parsed *p, struct zr_verify_report *rep)
{
	struct zr_walk wo, wf, wr;
	struct zr_oracle *o;
	char err[512];

	parse_doc(p, body, nactions, nconf, records);
	err[0] = '\0';
	CHECK(zr_walk(v->vs_onto, v->vs_ns, &wo, err, sizeof (err)) == 0);
	CHECK(zr_walk(v->vs_from, v->vs_ns, &wf, err, sizeof (err)) == 0);
	CHECK(zr_walk(v->vs_res, v->vs_ns, &wr, err, sizeof (err)) == 0);
	CHECK(zr_oracle_init(&o, &wo, &wf, &wr) == 0);
	err[0] = '\0';
	if (zr_verify(p, o, &wo, &wf, &wr, rep, err, sizeof (err)) != 0)
		printf("  verify: %s\n", err);
	CHECK(err[0] == '\0');
	zr_oracle_fini(o);
	zr_walk_fini(&wr);
	zr_walk_fini(&wf);
	zr_walk_fini(&wo);
}

/* One action, one expected outcome, and no information line. */
static void
vshape_one(struct vshape *v, const char *body, const char *path,
    enum zr_outcome want)
{
	struct zr_verify_report rep;
	struct zr_parsed p;
	uint32_t i;

	vshape_run(v, body, 1, 0, "", &p, &rep);
	i = idx_of(&p, path);
	if (rep.zv_outcome[i] != want) {
		printf("  %s: %s, wanted %s\n", path,
		    zr_outcome_str(rep.zv_outcome[i]), zr_outcome_str(want));
	}
	CHECK(rep.zv_outcome[i] == want);
	CHECK(rep.zv_count[want] == 1);
	CHECK(rep.zv_first[want] == i);
	CHECK(rep.zv_ninfo == 0);
	CHECK(rep.zv_first_info == ZR_NAME_NONE);
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
}

/* ZY1, ZY2, ZY3: the three states of a leaf removal. */
static void
check_rm_states(void)
{
	struct vshape v;

	vshape_init(&v);
	mkfile(v.vs_onto, "/x", "onto bytes\n", 0644);
	mkfile(v.vs_res, "/x", "onto bytes\n", 0644);
	vshape_one(&v, "    x rm\n", "/x", ZR_OC_PENDING);
	mkfile(v.vs_res, "/x", "somebody else\n", 0644);
	vshape_one(&v, "    x rm\n", "/x", ZR_OC_DRIFTED);
	rmname(v.vs_res, "/x");
	vshape_one(&v, "    x rm\n", "/x", ZR_OC_DONE);
	vshape_fini(&v);
}

/*
 * ZY4, ZY20: the removal of a directory a conflict holds open. The
 * directory is blocked, which is a state and not drift; the conflict
 * mark itself is classified as nothing and counted nowhere.
 */
static void
check_rm_blocked(void)
{
	struct zr_verify_report rep;
	struct zr_parsed p;
	struct vshape v;
	uint32_t i;
	static const char body[] =
	    "    d/ rm\n"
	    "        c conflict 1\n"
	    "        ..\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /d/c changed on both sides\n"
	    "  base ()\n"
	    "  from ()\n"
	    "  onto ({/d/c}x)\n";

	vshape_init(&v);
	mkdirp(v.vs_onto, "/d", 0755);
	mkfile(v.vs_onto, "/d/c", "contested\n", 0644);
	mkdirp(v.vs_res, "/d", 0755);
	mkfile(v.vs_res, "/d/c", "contested\n", 0644);
	vshape_run(&v, body, 1, 1, records, &p, &rep);
	CHECK(p.zp_nactions == 2);
	i = idx_of(&p, "/d");
	CHECK(rep.zv_outcome[i] == ZR_OC_BLOCKED);
	CHECK(rep.zv_count[ZR_OC_BLOCKED] == 1);
	CHECK(rep.zv_first[ZR_OC_BLOCKED] == i);
	CHECK(rep.zv_outcome[idx_of(&p, "/d/c")] == ZR_OC_DONE);
	CHECK(rep.zv_count[ZR_OC_DONE] == 0);
	CHECK(rep.zv_first[ZR_OC_DONE] == ZR_ACTION_NONE);
	CHECK(rep.zv_count[ZR_OC_PENDING] == 0);
	CHECK(rep.zv_count[ZR_OC_DRIFTED] == 0);
	CHECK(rep.zv_ninfo == 0);
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
	vshape_fini(&v);
}

/* ZY5, ZY6, ZY7, ZY8: the four states of a link. */
static void
check_ln_states(void)
{
	struct vshape v;

	vshape_init(&v);
	mkfile(v.vs_onto, "/a", "anchor\n", 0644);
	mkfile(v.vs_onto, "/x", "not yet\n", 0644);
	mkfile(v.vs_res, "/a", "anchor\n", 0644);
	/* the name is not there at all */
	vshape_one(&v, "    x ln /a\n", "/x", ZR_OC_PENDING);
	/* the name still holds what onto put there */
	mkfile(v.vs_res, "/x", "not yet\n", 0644);
	vshape_one(&v, "    x ln /a\n", "/x", ZR_OC_PENDING);
	/* the name holds something nobody asked for */
	mkfile(v.vs_res, "/x", "a stray edit\n", 0644);
	vshape_one(&v, "    x ln /a\n", "/x", ZR_OC_DRIFTED);
	/* the name is the anchor's own object */
	rmname(v.vs_res, "/x");
	mklink(v.vs_res, "/x", "/a");
	vshape_one(&v, "    x ln /a\n", "/x", ZR_OC_DONE);
	vshape_fini(&v);
}

/*
 * ZY9, ZY10, ZY12: a cp of a name onto never had. Absent is what
 * onto had there, so an absent name is pending and not done.
 */
static void
check_cp_new(void)
{
	struct vshape v;

	vshape_init(&v);
	mkfile(v.vs_from, "/n", "from bytes\n", 0644);
	vshape_one(&v, "    n cp /n\n", "/n", ZR_OC_PENDING);
	mkfile(v.vs_res, "/n", "someone else\n", 0644);
	vshape_one(&v, "    n cp /n\n", "/n", ZR_OC_DRIFTED);
	mkfile(v.vs_res, "/n", "from bytes\n", 0644);
	vshape_one(&v, "    n cp /n\n", "/n", ZR_OC_DONE);
	vshape_fini(&v);
}

/* ZY11: a cp over a name onto did have: still onto's is pending. */
static void
check_cp_over(void)
{
	struct vshape v;

	vshape_init(&v);
	mkfile(v.vs_from, "/n", "from bytes\n", 0644);
	mkfile(v.vs_onto, "/n", "onto bytes\n", 0644);
	mkfile(v.vs_res, "/n", "onto bytes\n", 0644);
	vshape_one(&v, "    n cp /n\n", "/n", ZR_OC_PENDING);
	mkfile(v.vs_res, "/n", "from bytes\n", 0644);
	vshape_one(&v, "    n cp /n\n", "/n", ZR_OC_DONE);
	vshape_fini(&v);
}

/*
 * ZY13, ZY14, ZY15: dup severs. The two states have the same bytes
 * and the same attributes, so what tells them apart is whether the
 * name is still the anchor's own file, which the result walk knows.
 */
static void
check_dup_states(void)
{
	struct vshape v;

	vshape_init(&v);
	mkfile(v.vs_onto, "/a", "shared\n", 0644);
	mklink(v.vs_onto, "/b", "/a");
	mkfile(v.vs_res, "/a", "shared\n", 0644);
	mklink(v.vs_res, "/b", "/a");
	vshape_one(&v, "    b dup /a\n", "/b", ZR_OC_PENDING);
	rmname(v.vs_res, "/b");
	mkfile(v.vs_res, "/b", "shared\n", 0644);
	vshape_one(&v, "    b dup /a\n", "/b", ZR_OC_DONE);
	chmodp(v.vs_res, "/b", 0600);
	vshape_one(&v, "    b dup /a\n", "/b", ZR_OC_DRIFTED);
	vshape_fini(&v);
}

/* ZY16, ZY17, ZY18: the three states of a write. */
static void
check_write_states(void)
{
	struct vshape v;

	vshape_init(&v);
	mkfile(v.vs_from, "/w", "the new bytes\n", 0644);
	mkfile(v.vs_onto, "/w", "the old bytes\n", 0644);
	mkfile(v.vs_res, "/w", "the old bytes\n", 0644);
	vshape_one(&v, "    w write /w\n", "/w", ZR_OC_PENDING);
	mkfile(v.vs_res, "/w", "a stray edit ..\n", 0644);
	vshape_one(&v, "    w write /w\n", "/w", ZR_OC_DRIFTED);
	mkfile(v.vs_res, "/w", "the new bytes\n", 0644);
	vshape_one(&v, "    w write /w\n", "/w", ZR_OC_DONE);
	vshape_fini(&v);
}

/*
 * ZY19: a write keeps the object, so the second name onto gave it
 * sees the new bytes through it. A result that wrote one name and
 * left the other behind matches neither the manifest nor onto, which
 * is drift -- unless the manifest speaks of that other name itself,
 * and then it is the manifest's business and not the write's.
 */
static void
check_write_pool(void)
{
	struct zr_verify_report rep;
	struct zr_parsed p;
	struct vshape v;

	vshape_init(&v);
	mkfile(v.vs_from, "/w", "the new bytes\n", 0644);
	mkfile(v.vs_onto, "/w", "the old bytes\n", 0644);
	mklink(v.vs_onto, "/w2", "/w");
	mkfile(v.vs_res, "/w", "the new bytes\n", 0644);
	mklink(v.vs_res, "/w2", "/w");
	vshape_one(&v, "    w write /w\n", "/w", ZR_OC_DONE);
	/* the link torn and one half left as it was */
	rmname(v.vs_res, "/w2");
	mkfile(v.vs_res, "/w2", "the old bytes\n", 0644);
	vshape_one(&v, "    w write /w\n", "/w", ZR_OC_DRIFTED);
	/* the same tree, with the manifest saying what became of /w2 */
	rmname(v.vs_res, "/w2");
	vshape_run(&v, "    w write /w\n    w2 rm\n", 2, 0, "", &p, &rep);
	CHECK(rep.zv_outcome[idx_of(&p, "/w")] == ZR_OC_DONE);
	CHECK(rep.zv_outcome[idx_of(&p, "/w2")] == ZR_OC_DONE);
	CHECK(rep.zv_count[ZR_OC_DONE] == 2);
	CHECK(rep.zv_ninfo == 0);
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
	vshape_fini(&v);
}

/*
 * ZY22, ZY23, ZY24: the information line. A name nobody spoke for
 * that the result no longer holds as onto did is one, whether it was
 * edited or added; a name that still matches is not.
 */
static void
check_info_lines(void)
{
	struct zr_verify_report rep;
	struct zr_parsed p;
	struct vshape v;

	vshape_init(&v);
	mkdirp(v.vs_onto, "/d", 0755);
	mkfile(v.vs_onto, "/d/x", "untouched\n", 0644);
	mkdirp(v.vs_res, "/d", 0755);
	mkfile(v.vs_res, "/d/x", "untouched\n", 0644);
	vshape_run(&v, "", 0, 0, "", &p, &rep);
	CHECK(rep.zv_ninfo == 0);
	CHECK(rep.zv_first_info == ZR_NAME_NONE);
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
	/* an edit to a name the manifest never mentions */
	mkfile(v.vs_res, "/d/x", "an edit!!\n", 0644);
	vshape_run(&v, "", 0, 0, "", &p, &rep);
	CHECK(rep.zv_ninfo == 1);
	CHECK(rep.zv_first_info == zr_names_lookup(v.vs_ns, "/d/x", 4));
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
	/* and a name onto never had at all */
	mkfile(v.vs_res, "/extra", "brand new\n", 0644);
	vshape_run(&v, "", 0, 0, "", &p, &rep);
	CHECK(rep.zv_ninfo == 2);
	CHECK(rep.zv_first_info == zr_names_lookup(v.vs_ns, "/d/x", 4));
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
	vshape_fini(&v);
}

/*
 * ZY21: a conflicted name is left alone entirely, and so is
 * everything under a conflicted directory. The same doctored tree
 * gives an information line when nothing marks the directory.
 */
static void
check_info_conflicted(void)
{
	struct zr_verify_report rep;
	struct zr_parsed p;
	struct vshape v;
	static const char body[] =
	    "    d/ conflict 1\n"
	    "        ..\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /d changed on both sides\n"
	    "  base ()\n"
	    "  from ()\n"
	    "  onto ({/d}x)\n";

	vshape_init(&v);
	mkdirp(v.vs_onto, "/d", 0755);
	mkfile(v.vs_onto, "/d/x", "untouched\n", 0644);
	mkdirp(v.vs_res, "/d", 0755);
	mkfile(v.vs_res, "/d/x", "an edit!!\n", 0644);
	vshape_run(&v, body, 0, 1, records, &p, &rep);
	CHECK(p.zp_nactions == 1);
	CHECK(rep.zv_outcome[0] == ZR_OC_DONE);
	CHECK(rep.zv_count[ZR_OC_DONE] == 0);
	CHECK(rep.zv_ninfo == 0);
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
	/* the same result, with nothing conflicted over it */
	vshape_run(&v, "", 0, 0, "", &p, &rep);
	CHECK(rep.zv_ninfo == 1);
	CHECK(rep.zv_first_info == zr_names_lookup(v.vs_ns, "/d/x", 4));
	zr_verify_report_fini(&rep);
	zr_parsed_fini(&p);
	vshape_fini(&v);
}

/*
 * ZC25: the pair entry point the classifier asks everything
 * through. A pair it has already put in one class answers without
 * reading a byte, and so does a pair it has already found
 * different; a position or a pool the oracle does not hold is an
 * error and not a verdict.
 */
static void
check_oracle_pairs(void)
{
	struct zr_walk wo, wf, wr;
	struct zr_oracle *o;
	zr_pool_t po, pr, qo, qr;
	struct vshape v;
	uint64_t n1, n2;
	char err[512];

	vshape_init(&v);
	mkfile(v.vs_onto, "/same", "the same bytes\n", 0644);
	mkfile(v.vs_res, "/same", "the same bytes\n", 0644);
	mkfile(v.vs_onto, "/other", "one\n", 0644);
	mkfile(v.vs_res, "/other", "two\n", 0644);
	err[0] = '\0';
	CHECK(zr_walk(v.vs_onto, v.vs_ns, &wo, err, sizeof (err)) == 0);
	CHECK(zr_walk(v.vs_from, v.vs_ns, &wf, err, sizeof (err)) == 0);
	CHECK(zr_walk(v.vs_res, v.vs_ns, &wr, err, sizeof (err)) == 0);
	CHECK(zr_oracle_init(&o, &wo, &wf, &wr) == 0);
	po = zr_tree_pool(&wo.zw_tree, zr_names_lookup(v.vs_ns, "/same", 5));
	pr = zr_tree_pool(&wr.zw_tree, zr_names_lookup(v.vs_ns, "/same", 5));
	qo = zr_tree_pool(&wo.zw_tree, zr_names_lookup(v.vs_ns, "/other", 6));
	qr = zr_tree_pool(&wr.zw_tree, zr_names_lookup(v.vs_ns, "/other", 6));
	CHECK(po != ZR_POOL_NONE && pr != ZR_POOL_NONE);
	CHECK(qo != ZR_POOL_NONE && qr != ZR_POOL_NONE);
	CHECK(zr_oracle_equal(o, 0, po, 2, pr, err, sizeof (err)) == 1);
	n1 = zr_oracle_bytes_read(o);
	CHECK(n1 > 0);
	CHECK(zr_oracle_equal(o, 0, po, 2, pr, err, sizeof (err)) == 1);
	CHECK(zr_oracle_bytes_read(o) == n1);
	CHECK(zr_oracle_equal(o, 0, qo, 2, qr, err, sizeof (err)) == 0);
	n2 = zr_oracle_bytes_read(o);
	CHECK(n2 > n1);
	CHECK(zr_oracle_equal(o, 0, qo, 2, qr, err, sizeof (err)) == 0);
	CHECK(zr_oracle_bytes_read(o) == n2);
	err[0] = '\0';
	CHECK(zr_oracle_equal(o, 3, po, 2, pr, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0');
	err[0] = '\0';
	CHECK(zr_oracle_equal(o, 0, wo.zw_tree.zt_npools, 2, pr, err,
	    sizeof (err)) == -1);
	CHECK(err[0] != '\0');
	zr_oracle_fini(o);
	zr_walk_fini(&wr);
	zr_walk_fini(&wf);
	zr_walk_fini(&wo);
	vshape_fini(&v);
}

/*
 * ---------------------------------------------------------------
 * The scenario: one rebase decided, emitted, applied and classified,
 * end to end over directories.
 * ---------------------------------------------------------------
 */

/*
 * base holds the pool {p q r}, the file w, the file b, the untouched
 * keep and the directory d with two files. from severs q out of the
 * pool, edits w, deletes b and the whole of d, and adds n. onto
 * edits the pool through p and edits d/c, which nobody else can
 * decide: so the rebase removes d, and cannot, because the conflict
 * on d/c holds the directory open.
 */
static void
build_base(const char *root)
{
	mkfile(root, "/p", "x1\n", 0644);
	mklink(root, "/q", "/p");
	mklink(root, "/r", "/p");
	mkfile(root, "/w", "w0\n", 0644);
	mkfile(root, "/b", "b0\n", 0644);
	mkfile(root, "/keep", "k0\n", 0644);
	mkdirp(root, "/d", 0755);
	mkfile(root, "/d/c", "c0\n", 0644);
	mkfile(root, "/d/k", "dk0\n", 0644);
}

static void
build_from(const char *root)
{
	mkfile(root, "/p", "x1\n", 0644);
	mkfile(root, "/q", "x1\n", 0644);
	mklink(root, "/r", "/q");
	mkfile(root, "/w", "w1\n", 0644);
	mkfile(root, "/keep", "k0\n", 0644);
	mkfile(root, "/n", "n0\n", 0644);
}

static void
build_onto(const char *root)
{
	mkfile(root, "/p", "y1\n", 0644);
	mklink(root, "/q", "/p");
	mklink(root, "/r", "/p");
	mkfile(root, "/w", "w0\n", 0644);
	mkfile(root, "/b", "b0\n", 0644);
	mkfile(root, "/keep", "k0\n", 0644);
	mkdirp(root, "/d", 0755);
	mkfile(root, "/d/c", "c1\n", 0644);
	mkfile(root, "/d/k", "dk0\n", 0644);
}

struct scene {
	char			sc_root[PATHMAX];
	char			sc_base[PATHMAX];
	char			sc_from[PATHMAX];
	char			sc_onto[PATHMAX];
	struct zr_names		*sc_ns;
	struct zr_walk		sc_wb, sc_wf, sc_wo;
	struct zr_oracle	*sc_oracle;
	struct zr_decision	sc_d;
	struct zr_parsed	sc_p;
};

/* Walk, assign content, decide, emit, parse: the --posix pipeline. */
static void
scene_init(struct scene *s)
{
	char tmpl[] = "/tmp/zrverifys.XXXXXX";
	struct zr_manifest_hdr hdr;
	char err[512];
	FILE *f;

	memset(s, 0, sizeof (*s));
	CHECK(mkdtemp(tmpl) != NULL);
	join(s->sc_root, sizeof (s->sc_root), tmpl, "");
	join(s->sc_base, sizeof (s->sc_base), tmpl, "/base");
	join(s->sc_from, sizeof (s->sc_from), tmpl, "/from");
	join(s->sc_onto, sizeof (s->sc_onto), tmpl, "/onto");
	CHECK(mkdir(s->sc_base, 0755) == 0);
	CHECK(mkdir(s->sc_from, 0755) == 0);
	CHECK(mkdir(s->sc_onto, 0755) == 0);
	build_base(s->sc_base);
	build_from(s->sc_from);
	build_onto(s->sc_onto);
	s->sc_ns = zr_names_create();
	CHECK(s->sc_ns != NULL);
	err[0] = '\0';
	CHECK(zr_walk(s->sc_base, s->sc_ns, &s->sc_wb, err,
	    sizeof (err)) == 0);
	CHECK(zr_walk(s->sc_from, s->sc_ns, &s->sc_wf, err,
	    sizeof (err)) == 0);
	CHECK(zr_walk(s->sc_onto, s->sc_ns, &s->sc_wo, err,
	    sizeof (err)) == 0);
	CHECK(zr_oracle_init(&s->sc_oracle, &s->sc_wb, &s->sc_wf,
	    &s->sc_wo) == 0);
	if (zr_oracle_assign(s->sc_oracle, err, sizeof (err)) != 0)
		printf("  oracle: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(zr_decide(&s->sc_wb.zw_tree, &s->sc_wf.zw_tree,
	    &s->sc_wo.zw_tree, ZR_MODE_STRICT, &s->sc_d) == 0);
	CHECK(s->sc_d.zd_nconflicts == 1);
	hdr.base = s->sc_base;
	hdr.from = s->sc_from;
	hdr.onto = s->sc_onto;
	hdr.mode = ZR_MODE_STRICT;
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_emit(f, &hdr, &s->sc_wb.zw_tree, &s->sc_wf.zw_tree,
	    &s->sc_wo.zw_tree, &s->sc_d) == 0);
	rewind(f);
	err[0] = '\0';
	if (zr_manifest_parse(f, &s->sc_p, err, sizeof (err)) != 0)
		printf("  parse: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(fclose(f) == 0);
	/* the manifest this scenario is here to exercise */
	CHECK(s->sc_p.zp_actions_declared == 7);
	CHECK(s->sc_p.zp_nactions == 8);
	CHECK(s->sc_p.zp_nrecords == 1);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/b")].za_kind ==
	    ZR_ACT_RM);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/d")].za_kind ==
	    ZR_ACT_RM);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/d")].za_isdir == 1);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/d/c")].za_kind ==
	    ZR_ACT_CONFLICT);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/d/k")].za_kind ==
	    ZR_ACT_RM);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/n")].za_kind ==
	    ZR_ACT_CP);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/q")].za_kind ==
	    ZR_ACT_DUP);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/r")].za_kind ==
	    ZR_ACT_LN);
	CHECK(s->sc_p.zp_actions[idx_of(&s->sc_p, "/w")].za_kind ==
	    ZR_ACT_WRITE);
}

static void
scene_fini(struct scene *s)
{
	zr_parsed_fini(&s->sc_p);
	zr_decision_fini(&s->sc_d);
	zr_oracle_fini(s->sc_oracle);
	zr_walk_fini(&s->sc_wo);
	zr_walk_fini(&s->sc_wf);
	zr_walk_fini(&s->sc_wb);
	zr_names_destroy(s->sc_ns);
	rmtree(s->sc_root);
}

/* One fresh copy of onto to work on. */
static void
scene_work(struct scene *s, const char *name, char *out, size_t outlen)
{
	join(out, outlen, s->sc_root, name);
	CHECK(mkdir(out, 0755) == 0);
	build_onto(out);
}

static void
scene_apply(struct scene *s, const char *work,
    const struct zr_verify_report *skip, struct zr_apply_stats *st)
{
	char err[512];

	err[0] = '\0';
	if (zr_apply_with(&s->sc_p, work, &s->sc_wf, &s->sc_wo, skip, st, err,
	    sizeof (err)) != 0)
		printf("  apply: %s\n", err);
	CHECK(err[0] == '\0');
}

static void
scene_classify(struct scene *s, const char *work, struct zr_verify_report *rep)
{
	struct zr_oracle *o;
	struct zr_walk wr;
	char err[512];

	err[0] = '\0';
	if (zr_walk(work, s->sc_ns, &wr, err, sizeof (err)) != 0)
		printf("  walk: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(zr_oracle_init(&o, &s->sc_wo, &s->sc_wf, &wr) == 0);
	err[0] = '\0';
	if (zr_verify(&s->sc_p, o, &s->sc_wo, &s->sc_wf, &wr, rep, err,
	    sizeof (err)) != 0)
		printf("  verify: %s\n", err);
	CHECK(err[0] == '\0');
	zr_oracle_fini(o);
	zr_walk_fini(&wr);
}

/*
 * ZY25, ZY31, ZY32, ZY33, ZY34: the manifest applied, classified,
 * doctored, classified again, and applied with the report as the
 * list of what to leave alone.
 */
static void
check_scenario(void)
{
	struct zr_verify_report rep;
	struct zr_apply_stats st;
	char work[PATHMAX];
	struct scene s;
	uint32_t ib, id, idk, in, iq, ir, iw;

	scene_init(&s);
	ib = idx_of(&s.sc_p, "/b");
	id = idx_of(&s.sc_p, "/d");
	idk = idx_of(&s.sc_p, "/d/k");
	in = idx_of(&s.sc_p, "/n");
	iq = idx_of(&s.sc_p, "/q");
	ir = idx_of(&s.sc_p, "/r");
	iw = idx_of(&s.sc_p, "/w");
	scene_work(&s, "/work", work, sizeof (work));

	/*
	 * ZY34: the apply leaves the blocked directory without being
	 * told to, since the manifest's own conflict mark says so.
	 */
	scene_apply(&s, work, NULL, &st);
	CHECK(st.zs_rm == 2);		/* /b and /d/k; /d is blocked */
	CHECK(st.zs_skipped == 1);
	CHECK(st.zs_cp == 1 && st.zs_dup == 1 && st.zs_ln == 1);
	CHECK(st.zs_write == 1);
	CHECK(absent(work, "/b"));
	CHECK(absent(work, "/d/k"));
	CHECK(!absent(work, "/d/c"));

	scene_classify(&s, work, &rep);
	CHECK(rep.zv_count[ZR_OC_DONE] == 6);
	CHECK(rep.zv_count[ZR_OC_BLOCKED] == 1);
	CHECK(rep.zv_count[ZR_OC_PENDING] == 0);
	CHECK(rep.zv_count[ZR_OC_DRIFTED] == 0);
	CHECK(rep.zv_first[ZR_OC_DONE] == ib);
	CHECK(rep.zv_first[ZR_OC_BLOCKED] == id);
	CHECK(rep.zv_first[ZR_OC_PENDING] == ZR_ACTION_NONE);
	CHECK(rep.zv_first[ZR_OC_DRIFTED] == ZR_ACTION_NONE);
	CHECK(rep.zv_outcome[id] == ZR_OC_BLOCKED);
	CHECK(rep.zv_ninfo == 0);
	zr_verify_report_fini(&rep);

	/* now the doctoring: one of each state, in manifest order */
	rmname(work, "/n");
	mkfile(work, "/w", "a stray edit\n", 0644);
	mkfile(work, "/keep", "k9\n", 0644);
	chmodp(work, "/q", 0600);
	scene_classify(&s, work, &rep);
	CHECK(rep.zv_outcome[ib] == ZR_OC_DONE);
	CHECK(rep.zv_outcome[id] == ZR_OC_BLOCKED);
	CHECK(rep.zv_outcome[idk] == ZR_OC_DONE);
	CHECK(rep.zv_outcome[in] == ZR_OC_PENDING);
	CHECK(rep.zv_outcome[iq] == ZR_OC_DRIFTED);
	CHECK(rep.zv_outcome[ir] == ZR_OC_DONE);
	CHECK(rep.zv_outcome[iw] == ZR_OC_DRIFTED);
	CHECK(rep.zv_count[ZR_OC_DONE] == 3);
	CHECK(rep.zv_count[ZR_OC_PENDING] == 1);
	CHECK(rep.zv_count[ZR_OC_BLOCKED] == 1);
	CHECK(rep.zv_count[ZR_OC_DRIFTED] == 2);
	CHECK(rep.zv_first[ZR_OC_DONE] == ib);
	CHECK(rep.zv_first[ZR_OC_PENDING] == in);
	CHECK(rep.zv_first[ZR_OC_BLOCKED] == id);
	CHECK(rep.zv_first[ZR_OC_DRIFTED] == iq);
	CHECK(rep.zv_ninfo == 1);
	CHECK(rep.zv_first_info == zr_names_lookup(s.sc_ns, "/keep", 5));

	/*
	 * The repair. The two removals and the blocked directory are
	 * left alone; the pending and the drifted are performed. The
	 * link is the one action a report cannot be taken at its word
	 * for: the dup just before it made a new object at its anchor,
	 * so it is done again (ZY33).
	 */
	scene_apply(&s, work, &rep, &st);
	CHECK(st.zs_skipped == 3);
	CHECK(st.zs_rm == 0);
	CHECK(st.zs_cp == 1 && st.zs_dup == 1 && st.zs_write == 1);
	CHECK(st.zs_ln == 1);
	zr_verify_report_fini(&rep);

	scene_classify(&s, work, &rep);
	CHECK(rep.zv_count[ZR_OC_DONE] == 6);
	CHECK(rep.zv_count[ZR_OC_BLOCKED] == 1);
	CHECK(rep.zv_count[ZR_OC_PENDING] == 0);
	CHECK(rep.zv_count[ZR_OC_DRIFTED] == 0);
	/* the information line is nobody's to repair, and stays */
	CHECK(rep.zv_ninfo == 1);
	CHECK(rep.zv_first_info == zr_names_lookup(s.sc_ns, "/keep", 5));
	zr_verify_report_fini(&rep);
	scene_fini(&s);
}

/*
 * Two walks of the same tree at two moments: the same names, pooled
 * the same way, holding the same objects. Inode numbers are not
 * among them -- a cp that ran again made a new file for the name --
 * and neither are the times, which the oracle never compares.
 */
static void
same_trees(struct scene *s, const char *a, const char *b)
{
	const struct zr_pool *pa;
	struct zr_walk wa, wb;
	struct zr_oracle *o;
	zr_pool_t qa, qb;
	uint32_t n, i;
	char err[512];

	err[0] = '\0';
	CHECK(zr_walk(a, s->sc_ns, &wa, err, sizeof (err)) == 0);
	CHECK(zr_walk(b, s->sc_ns, &wb, err, sizeof (err)) == 0);
	CHECK(zr_oracle_init(&o, &wa, &wb, &s->sc_wf) == 0);
	CHECK(wa.zw_tree.zt_npools == wb.zw_tree.zt_npools);
	n = zr_names_count(s->sc_ns);
	for (i = 0; i < n; i++) {
		qa = zr_tree_pool(&wa.zw_tree, i);
		qb = zr_tree_pool(&wb.zw_tree, i);
		if ((qa == ZR_POOL_NONE) != (qb == ZR_POOL_NONE))
			printf("  %s: only one walk has it\n",
			    zr_names_str(s->sc_ns, i, NULL));
		CHECK((qa == ZR_POOL_NONE) == (qb == ZR_POOL_NONE));
		if (qa == ZR_POOL_NONE)
			continue;
		pa = &wa.zw_tree.zt_pools[qa];
		CHECK(pa->zp_nnames == wb.zw_tree.zt_pools[qb].zp_nnames);
		CHECK(zr_oracle_equal(o, 0, qa, 1, qb, err,
		    sizeof (err)) == 1);
	}
	zr_oracle_fini(o);
	zr_walk_fini(&wb);
	zr_walk_fini(&wa);
}

/*
 * ZY26, ZY27, ZY28, ZY29, ZY30: the manifest applied twice over a
 * pristine copy. The second run finds every removal already made,
 * every directory already there, every link already standing and
 * every object in the way of a cp its own, and leaves the tree where
 * the first run left it.
 */
static void
check_twice(void)
{
	struct zr_apply_stats st1, st2;
	char once[PATHMAX], twice[PATHMAX];
	struct scene s;

	scene_init(&s);
	scene_work(&s, "/once", once, sizeof (once));
	scene_work(&s, "/twice", twice, sizeof (twice));
	scene_apply(&s, once, NULL, &st1);
	scene_apply(&s, twice, NULL, &st1);
	scene_apply(&s, twice, NULL, &st2);
	CHECK(st2.zs_rm == st1.zs_rm);
	CHECK(st2.zs_ln == st1.zs_ln);
	CHECK(st2.zs_cp == st1.zs_cp);
	CHECK(st2.zs_dup == st1.zs_dup);
	CHECK(st2.zs_write == st1.zs_write);
	CHECK(st2.zs_skipped == 1);
	same_trees(&s, once, twice);
	scene_fini(&s);
}

/*
 * ZY35: a directory the manifest removes that is not empty and has
 * no conflicted name under it is still the loud failure it was. The
 * blocked case is the conflict's, and nothing else borrows it.
 */
static void
check_rm_still_loud(void)
{
	struct zr_apply_stats st;
	struct zr_parsed p;
	struct zr_walk wf;
	struct vshape v;
	char err[512];

	vshape_init(&v);
	mkdirp(v.vs_res, "/d", 0755);
	mkfile(v.vs_res, "/d/c", "still here\n", 0644);
	parse_doc(&p, "    d/ rm\n    ..\n", 1, 0, "");
	err[0] = '\0';
	CHECK(zr_walk(v.vs_from, v.vs_ns, &wf, err, sizeof (err)) == 0);
	err[0] = '\0';
	CHECK(zr_apply(&p, v.vs_res, &wf, &wf, &st, err, sizeof (err)) == -1);
	if (strstr(err, "rmdir") == NULL)
		printf("  message lacks \"rmdir\": %s\n", err);
	CHECK(strstr(err, "rmdir") != NULL);
	CHECK(!absent(v.vs_res, "/d/c"));
	zr_walk_fini(&wf);
	zr_parsed_fini(&p);
	vshape_fini(&v);
}

int
main(void)
{
	check_rm_states();
	check_rm_blocked();
	check_ln_states();
	check_cp_new();
	check_cp_over();
	check_dup_states();
	check_write_states();
	check_write_pool();
	check_info_lines();
	check_info_conflicted();
	check_oracle_pairs();
	check_scenario();
	check_twice();
	check_rm_still_loud();

	printf("check_verify: %d checks passed\n", checks);
	return (0);
}
