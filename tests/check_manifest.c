/*
 * The manifest emitter's tests. Three scenarios are built by hand
 * through the name table, decided in strict mode and emitted into a
 * temporary file, then compared byte for byte with the text the
 * format note prints: the probe scenario of the note's section 7, a
 * name that needs escaping in both a tree line and a conflict record,
 * and a result pool whose anchor onto invented, so that its write
 * carries a path from a name other than its own.
 *
 * Then the parser, which reads those same texts back: every action and
 * every record checked field by field, the fixture's expect block
 * parsed against the emitted text, a parse written out again and
 * compared with the bytes it came from, and one test per way a
 * manifest can be wrong, each demanding the line its error names.
 *
 * Then the resolution of section 8, which is the same tree grammar
 * with a choice per name: the example parsed and written back, the
 * skeleton a manifest makes, a drift line added to one, and one test
 * per way a resolution can be wrong. The family is ZM of
 * tests/MATRIX.md and every test below names the cells it closes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decide.h"
#include "fixture.h"
#include "manifest.h"
#include "name.h"

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

/* One opaque handle per distinct content the scenarios name. */
#define	C_ROOT	1
#define	C_D	2
#define	C_E	3
#define	C_KEEP	4
#define	C_X	5
#define	C_Y	6
#define	C_H	7
#define	C_F	8
#define	C_K	9
#define	C_A2	10
#define	C_H2	11
#define	C_N	12
#define	C_A3	13
#define	C_K2	14

#define	W_BASE	0
#define	W_FROM	1
#define	W_ONTO	2

struct world {
	struct zr_names	*w_ns;
	struct zr_tree	w_t[3];
};

static void
world_init(struct world *w)
{
	int i;

	w->w_ns = zr_names_create();
	CHECK(w->w_ns != NULL);
	for (i = 0; i < 3; i++)
		CHECK(zr_tree_init(&w->w_t[i], w->w_ns) == 0);
}

static void
world_fini(struct world *w)
{
	int i;

	for (i = 0; i < 3; i++)
		zr_tree_fini(&w->w_t[i]);
	zr_names_destroy(w->w_ns);
}

/*
 * Give one tree a name. Two names with one inode are two names of one
 * pool, which is how a hardlink pool is built here.
 */
static void
add(struct world *w, int tree, const char *path, uint64_t ino,
    zr_type_t type, uint32_t nlink, uint32_t content)
{
	zr_name_t n;
	zr_pool_t q;

	n = zr_names_intern(w->w_ns, path, strlen(path));
	CHECK(n != ZR_NAME_NONE);
	q = zr_tree_add(&w->w_t[tree], n, ino, type, nlink);
	CHECK(q != ZR_POOL_NONE);
	w->w_t[tree].zt_pools[q].zp_content = content;
}

static void
seal(struct world *w)
{
	char err[128];
	int i;

	for (i = 0; i < 3; i++) {
		CHECK(zr_tree_seal(&w->w_t[i]) == 0);
		CHECK(zr_tree_verify(&w->w_t[i], err, sizeof (err)) == 0);
	}
}

/*
 * Read one temporary file back whole and close it. Portable C: no
 * open_memstream, no feature-test macros.
 */
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

/* Emit into a temporary file and read it back whole. */
static char *
emit(struct world *w, const struct zr_manifest_hdr *hdr,
    const struct zr_decision *d, size_t *lenp)
{
	FILE *f;

	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_emit(f, hdr, &w->w_t[W_BASE], &w->w_t[W_FROM],
	    &w->w_t[W_ONTO], d) == 0);
	return (slurp(f, lenp));
}

/*
 * Byte for byte, and on a mismatch print both texts and the offset
 * where they part.
 */
static void
compare(const char *tag, const char *got, size_t gotlen, const char *want)
{
	size_t i, wantlen = strlen(want);

	checks++;
	if (gotlen == wantlen && memcmp(got, want, gotlen) == 0)
		return;
	for (i = 0; i < gotlen && i < wantlen && got[i] == want[i]; i++)
		continue;
	printf("%s: the manifest differs at byte %lu\n", tag,
	    (unsigned long)i);
	printf("--- want, %lu bytes ---\n%s", (unsigned long)wantlen, want);
	printf("--- got, %lu bytes ---\n%.*s", (unsigned long)gotlen,
	    (int)gotlen, got);
	printf("--- end ---\n");
	exit(1);
}

static void
run(struct world *w, const struct zr_manifest_hdr *hdr, const char *tag,
    const char *want)
{
	struct zr_decision d;
	char *got;
	size_t gotlen = 0;

	seal(w);
	CHECK(zr_decide(&w->w_t[W_BASE], &w->w_t[W_FROM], &w->w_t[W_ONTO],
	    ZR_MODE_STRICT, &d) == 0);
	got = emit(w, hdr, &d, &gotlen);
	compare(tag, got, gotlen, want);
	free(got);
	zr_decision_fini(&d);
}

/*
 * The run part of the header every emitted text below carries: the
 * clone form of a real run, every line of v4-manifest.md section 6
 * in its place. From #mode on is the decision, which each scenario
 * writes for itself. The guids are 11, 22 and 33 so that a line that
 * lost one is visible at a glance.
 */
#define	H_RESULT	"zrt/rebased"
#define	H_TAG		"zr-1a2b3c4d5e6f"
#define	H_WRITTEN	"2026-09-06T13:04:11Z"

#define	H_CLONE(base, from, onto)					\
	"#rebase-manifest 5\n"						\
	"#result " H_RESULT "\n"					\
	"#form clone\n"						\
	"#base " base " 11\n"						\
	"#from " from " 22\n"						\
	"#onto " onto " 33\n"						\
	"#made -\n"							\
	"#tag " H_TAG "\n"						\
	"#take -\n"							\
	"#written " H_WRITTEN "\n"

/* The same header as the emitter is handed it. */
static void
clone_hdr(struct zr_manifest_hdr *h, const char *base, const char *from,
    const char *onto, zr_mode_t mode)
{
	memset(h, 0, sizeof (*h));
	h->result = H_RESULT;
	h->form = ZR_HFORM_CLONE;
	h->base = base;
	h->base_guid = 11;
	h->from = from;
	h->from_guid = 22;
	h->onto = onto;
	h->onto_guid = 33;
	h->made = "-";
	h->tag = H_TAG;
	h->take = "-";
	h->written = H_WRITTEN;
	h->mode = mode;
}

/*
 * From #mode on, which is where the decision starts. Above it the
 * header is the run, and two documents of one decision differ there:
 * a fixture's expect block is the posix form's and the texts here
 * are a clone-form run's.
 */
static const char *
decision(const char *doc)
{
	const char *at = strstr(doc, "\n#mode ");

	CHECK(at != NULL);
	return (at + 1);
}

/*
 * The note's section 7: the probe scenario with an onto side that
 * edited a differently and edited keep/k. b is deleted on from, d is
 * renamed to e, the pool {h1 h2} is edited through one name and gains
 * a third, keep/k needs nothing, n is new, and a conflicts.
 */
static const char want_probe[] =
	H_CLONE("zrtdiff/fs@base", "zrtdiff/from@work", "zrtdiff/onto@work")
	"#mode strict\n"
	"#actions 8\n"
	"#conflicts 1\n"
	"/\n"
	"    a conflict 1\n"
	"    b rm\n"
	"    d/ rm\n"
	"        f rm\n"
	"        ..\n"
	"    e/ cp /e\n"
	"        f cp /e/f\n"
	"        ..\n"
	"    h1 write /h1\n"
	"    h3 ln /h1\n"
	"    n cp /n\n"
	"    ..\n"
	"\n"
	"# a pool is one file and all its names: {names}letter; same\n"
	"# letter, same bytes\n"
	"conflict 1 changed-both\n"
	"  why  /a changed on both sides\n"
	"  base ({/a}x)\n"
	"  from ({/a}y)\n"
	"  onto ({/a}z)\n";

static void
test_probe(void)
{
	struct zr_manifest_hdr hdr;
	struct world w;

	world_init(&w);
	add(&w, W_BASE, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_BASE, "/a", 2, ZR_T_FILE, 1, C_X);
	add(&w, W_BASE, "/b", 3, ZR_T_FILE, 1, C_Y);
	add(&w, W_BASE, "/h1", 4, ZR_T_FILE, 2, C_H);
	add(&w, W_BASE, "/h2", 4, ZR_T_FILE, 2, C_H);
	add(&w, W_BASE, "/d", 5, ZR_T_DIR, 1, C_D);
	add(&w, W_BASE, "/d/f", 6, ZR_T_FILE, 1, C_F);
	add(&w, W_BASE, "/keep", 7, ZR_T_DIR, 1, C_KEEP);
	add(&w, W_BASE, "/keep/k", 8, ZR_T_FILE, 1, C_K);

	add(&w, W_FROM, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_FROM, "/a", 2, ZR_T_FILE, 1, C_A2);
	add(&w, W_FROM, "/h1", 4, ZR_T_FILE, 3, C_H2);
	add(&w, W_FROM, "/h2", 4, ZR_T_FILE, 3, C_H2);
	add(&w, W_FROM, "/h3", 4, ZR_T_FILE, 3, C_H2);
	add(&w, W_FROM, "/e", 5, ZR_T_DIR, 1, C_E);
	add(&w, W_FROM, "/e/f", 6, ZR_T_FILE, 1, C_F);
	add(&w, W_FROM, "/keep", 7, ZR_T_DIR, 1, C_KEEP);
	add(&w, W_FROM, "/keep/k", 8, ZR_T_FILE, 1, C_K);
	add(&w, W_FROM, "/n", 9, ZR_T_FILE, 1, C_N);

	add(&w, W_ONTO, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_ONTO, "/a", 2, ZR_T_FILE, 1, C_A3);
	add(&w, W_ONTO, "/b", 3, ZR_T_FILE, 1, C_Y);
	add(&w, W_ONTO, "/h1", 4, ZR_T_FILE, 2, C_H);
	add(&w, W_ONTO, "/h2", 4, ZR_T_FILE, 2, C_H);
	add(&w, W_ONTO, "/d", 5, ZR_T_DIR, 1, C_D);
	add(&w, W_ONTO, "/d/f", 6, ZR_T_FILE, 1, C_F);
	add(&w, W_ONTO, "/keep", 7, ZR_T_DIR, 1, C_KEEP);
	add(&w, W_ONTO, "/keep/k", 8, ZR_T_FILE, 1, C_K2);

	clone_hdr(&hdr, "zrtdiff/fs@base", "zrtdiff/from@work",
	    "zrtdiff/onto@work", ZR_MODE_STRICT);
	run(&w, &hdr, "probe", want_probe);
	world_fini(&w);
}

/*
 * A leaf holding a space and the two bytes of an accented letter, in
 * a tree line, in a cp argument and in every line of a record; and a
 * directory whose own name needs escaping too.
 */
static const char want_escapes[] =
	H_CLONE("zrt/base@s", "zrt/from@s", "zrt/onto@s")
	"#mode strict\n"
	"#actions 1\n"
	"#conflicts 1\n"
	"/\n"
	"    caf\\303\\251\\040x conflict 1\n"
	"    d\\040d/\n"
	"        n cp /d\\040d/n\n"
	"        ..\n"
	"    ..\n"
	"\n"
	"# a pool is one file and all its names: {names}letter; same\n"
	"# letter, same bytes\n"
	"conflict 1 changed-both\n"
	"  why  /caf\\303\\251\\040x changed on both sides\n"
	"  base ({/caf\\303\\251\\040x}x)\n"
	"  from ({/caf\\303\\251\\040x}y)\n"
	"  onto ({/caf\\303\\251\\040x}z)\n";

static void
test_escapes(void)
{
	struct zr_manifest_hdr hdr;
	struct world w;

	world_init(&w);
	add(&w, W_BASE, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_BASE, "/caf\303\251 x", 2, ZR_T_FILE, 1, C_X);
	add(&w, W_BASE, "/d d", 3, ZR_T_DIR, 1, C_D);

	add(&w, W_FROM, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_FROM, "/caf\303\251 x", 2, ZR_T_FILE, 1, C_A2);
	add(&w, W_FROM, "/d d", 3, ZR_T_DIR, 1, C_D);
	add(&w, W_FROM, "/d d/n", 4, ZR_T_FILE, 1, C_N);

	add(&w, W_ONTO, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_ONTO, "/caf\303\251 x", 2, ZR_T_FILE, 1, C_A3);
	add(&w, W_ONTO, "/d d", 3, ZR_T_DIR, 1, C_D);

	clone_hdr(&hdr, "zrt/base@s", "zrt/from@s", "zrt/onto@s",
	    ZR_MODE_STRICT);
	run(&w, &hdr, "escapes", want_escapes);
	world_fini(&w);
}

/*
 * onto invented /p as a second name of the file /q names, so /p is the
 * result pool's first name in manifest order and therefore its anchor.
 * The pool keeps onto's object, whose bytes from changed through /q,
 * so the write sits on /p and its path is /q: the anchor's own name is
 * nowhere in from.
 */
static const char want_foreign[] =
	H_CLONE("zrt/base@s", "zrt/from@s", "zrt/onto@s")
	"#mode strict\n"
	"#actions 1\n"
	"#conflicts 0\n"
	"/\n"
	"    p write /q\n"
	"    ..\n";

static void
test_foreign_path(void)
{
	struct zr_manifest_hdr hdr;
	struct world w;

	world_init(&w);
	add(&w, W_BASE, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_BASE, "/q", 2, ZR_T_FILE, 1, C_X);

	add(&w, W_FROM, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_FROM, "/q", 2, ZR_T_FILE, 1, C_A2);

	add(&w, W_ONTO, "/", 1, ZR_T_DIR, 1, C_ROOT);
	add(&w, W_ONTO, "/p", 2, ZR_T_FILE, 2, C_X);
	add(&w, W_ONTO, "/q", 2, ZR_T_FILE, 2, C_X);

	clone_hdr(&hdr, "zrt/base@s", "zrt/from@s", "zrt/onto@s",
	    ZR_MODE_STRICT);
	run(&w, &hdr, "foreign", want_foreign);
	world_fini(&w);
}

/*
 * Parse one manifest held in memory, through a temporary file for the
 * same reason the emitter's tests use one.
 */
static int
parse_text(const char *text, struct zr_parsed *out, char *err, size_t errlen)
{
	FILE *f;
	size_t n = strlen(text);
	int rc;

	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fwrite(text, 1, n, f) == n);
	CHECK(fflush(f) == 0);
	CHECK(fseek(f, 0, SEEK_SET) == 0);
	rc = zr_manifest_parse(f, out, err, errlen);
	(void) fclose(f);
	return (rc);
}

/* One manifest that must be accepted; on a rejection say why. */
static void
parse_ok(const char *tag, const char *text, struct zr_parsed *out)
{
	char err[192];

	err[0] = '\0';
	checks++;
	if (parse_text(text, out, err, sizeof (err)) != 0) {
		printf("%s: the parse failed: %s\n", tag, err);
		exit(1);
	}
}

/*
 * One manifest that must be rejected, with an error opening in the
 * text the caller expects: that is where the line number sits.
 */
static void
reject(const char *tag, const char *text, const char *want)
{
	struct zr_parsed p;
	char err[192];

	err[0] = '\0';
	CHECK(parse_text(text, &p, err, sizeof (err)) == -1);
	checks++;
	if (strncmp(err, want, strlen(want)) != 0) {
		printf("%s: want \"%s...\", got \"%s\"\n", tag, want, err);
		exit(1);
	}
	zr_parsed_fini(&p);
}

/* One parsed action, field by field. */
static void
check_action(const struct zr_parsed *p, uint32_t i, enum zr_act_kind kind,
    const char *path, const char *arg, int isdir, uint32_t cnum)
{
	const struct zr_action *a;

	CHECK(i < p->zp_nactions);
	a = &p->zp_actions[i];
	CHECK(a->za_kind == kind);
	CHECK(a->za_pathlen == strlen(path));
	CHECK(memcmp(a->za_path, path, a->za_pathlen) == 0);
	CHECK((a->za_arg == NULL) == (arg == NULL));
	if (arg != NULL) {
		CHECK(a->za_arglen == strlen(arg));
		CHECK(memcmp(a->za_arg, arg, a->za_arglen) == 0);
	}
	CHECK(a->za_isdir == isdir);
	CHECK(a->za_conflict == cnum);
}

/*
 * Two parses of one decision agree in every field of it. The run
 * part of the header is left out: the same decision is written with
 * one header by a run and another by --posix.
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

/* Parse a manifest, write it out again, and demand the same bytes. */
static void
roundtrip(const char *tag, const char *text)
{
	struct zr_parsed p;
	FILE *f;
	char *got;
	size_t gotlen = 0;

	parse_ok(tag, text, &p);
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_parsed_write(f, &p) == 0);
	got = slurp(f, &gotlen);
	compare(tag, got, gotlen, text);
	free(got);
	zr_parsed_fini(&p);
}

/*
 * ZM8, ZM29, ZM30, ZM33, ZM39: the section 7 manifest read back. Every
 * path is rebuilt from the scoping and not from any indentation, the
 * conflict mark keeps its number, and the legend and the blank line
 * before it are passed over on the way to the record.
 */
static void
test_parse_probe(void)
{
	struct zr_parsed p;

	parse_ok("parse probe", want_probe, &p);
	CHECK(strcmp(p.zp_base, "zrtdiff/fs@base") == 0);
	CHECK(strcmp(p.zp_from, "zrtdiff/from@work") == 0);
	CHECK(strcmp(p.zp_onto, "zrtdiff/onto@work") == 0);
	CHECK(p.zp_mode == ZR_MODE_STRICT);
	CHECK(p.zp_actions_declared == 8);
	CHECK(p.zp_conflicts_declared == 1);
	CHECK(p.zp_nactions == 9);
	check_action(&p, 0, ZR_ACT_CONFLICT, "/a", NULL, 0, 1);
	check_action(&p, 1, ZR_ACT_RM, "/b", NULL, 0, 0);
	check_action(&p, 2, ZR_ACT_RM, "/d", NULL, 1, 0);
	check_action(&p, 3, ZR_ACT_RM, "/d/f", NULL, 0, 0);
	check_action(&p, 4, ZR_ACT_CP, "/e", "/e", 1, 0);
	check_action(&p, 5, ZR_ACT_CP, "/e/f", "/e/f", 0, 0);
	check_action(&p, 6, ZR_ACT_WRITE, "/h1", "/h1", 0, 0);
	check_action(&p, 7, ZR_ACT_LN, "/h3", "/h1", 0, 0);
	check_action(&p, 8, ZR_ACT_CP, "/n", "/n", 0, 0);
	CHECK(p.zp_nrecords == 1);
	CHECK(p.zp_records[0].zr_num == 1);
	CHECK(p.zp_records[0].zr_flags == ZR_CF_CHANGED_BOTH);
	CHECK(strcmp(p.zp_records[0].zr_why,
	    "/a changed on both sides") == 0);
	CHECK(strcmp(p.zp_records[0].zr_base, "({/a}x)") == 0);
	CHECK(strcmp(p.zp_records[0].zr_from, "({/a}y)") == 0);
	CHECK(strcmp(p.zp_records[0].zr_onto, "({/a}z)") == 0);
	zr_parsed_fini(&p);
}

/*
 * ZM41: the expect block of tests/fixtures/probe.zrt is the same
 * decision as the one the emitter wrote here, so the two parses must
 * agree field by field from #mode on. The fixture is the document
 * the box will compare against, and this is what ties it to the
 * code; its own header is the posix form's, which is the run and
 * not the decision (ZH1).
 */
static void
test_parse_fixture(void)
{
	struct zr_fixture *fx = NULL;
	struct zr_parsed a, b;
	const char *expect;
	char err[256];

	err[0] = '\0';
	if (zr_fixture_load("tests/fixtures/probe.zrt", &fx, err,
	    sizeof (err)) != 0)
		printf("  load: %s\n", err);
	CHECK(fx != NULL);
	expect = zr_fixture_expect(fx);
	CHECK(expect != NULL);
	compare("fixture expect", decision(expect), strlen(decision(expect)),
	    decision(want_probe));
	parse_ok("parse fixture", expect, &a);
	parse_ok("parse probe again", want_probe, &b);
	same_parse("fixture against emitted", &a, &b);
	zr_parsed_fini(&a);
	zr_parsed_fini(&b);
	zr_fixture_free(fx);
}

/*
 * ZM40: parse then write is the identity on all three emitted
 * manifests. The directories the walk needs are not kept by the parse
 * and are worked out again from the action paths, so this also says
 * that set is exactly the one the emitter showed.
 */
static void
test_write_back(void)
{
	struct zr_parsed p;

	roundtrip("write probe", want_probe);
	roundtrip("write escapes", want_escapes);
	roundtrip("write foreign", want_foreign);
	/* ZM16, ZM17, ZM21: what the escaped and foreign texts decode to. */
	parse_ok("parse escapes", want_escapes, &p);
	CHECK(p.zp_nactions == 2);
	check_action(&p, 0, ZR_ACT_CONFLICT, "/caf\303\251 x", NULL, 0, 1);
	check_action(&p, 1, ZR_ACT_CP, "/d d/n", "/d d/n", 0, 0);
	CHECK(strcmp(p.zp_records[0].zr_why,
	    "/caf\\303\\251\\040x changed on both sides") == 0);
	zr_parsed_fini(&p);
	parse_ok("parse foreign", want_foreign, &p);
	CHECK(p.zp_nactions == 1);
	check_action(&p, 0, ZR_ACT_WRITE, "/p", "/q", 0, 0);
	CHECK(p.zp_nrecords == 0);
	zr_parsed_fini(&p);
}

/*
 * ZM12, ZM13, ZM15, ZM24, ZM27, ZM31: shapes the three scenarios above
 * do not reach, written here in the emitter's own form by hand: a
 * directory whose action has no children under it, walk order against
 * strcmp, a record carrying every class at once, and a tree with
 * nothing in it at all. The writer must give each one back unchanged.
 */
static void
test_write_shapes(void)
{
	roundtrip("empty directory",
	    H_CLONE("b", "f", "o")
	    "#mode strict\n#actions 3\n#conflicts 0\n"
	    "/\n"
	    "    d/ rm\n"
	    "        ..\n"
	    "    e/ cp /e\n"
	    "        f cp /e/f\n"
	    "        ..\n"
	    "    ..\n");
	roundtrip("walk order",
	    H_CLONE("b", "f", "o")
	    "#mode strict\n#actions 2\n#conflicts 0\n"
	    "/\n"
	    "    a/\n"
	    "        b rm\n"
	    "        ..\n"
	    "    a-1 rm\n"
	    "    ..\n");
	roundtrip("every class",
	    H_CLONE("b", "f", "o")
	    "#mode permissive-merge\n#actions 0\n#conflicts 1\n"
	    "/\n"
	    "    a conflict 1\n"
	    "    ..\n"
	    "\n"
	    "# a pool is one file and all its names: {names}letter; same\n"
	    "# letter, same bytes\n"
	    "conflict 1 healed-split,orphaned-add,contested-home,"
	    "unexpressed-sharing,changed-both,disagree\n"
	    "  why  /a and /b were split on one side and joined by the "
	    "other\n"
	    "  base ({/a}x,{/b}y)\n"
	    "  from ({/a /b}z)\n"
	    "  onto ()\n");
	roundtrip("nothing to do",
	    H_CLONE("b", "f", "o")
	    "#mode strict\n#actions 0\n#conflicts 0\n"
	    "/\n"
	    "    ..\n");
}

/* The thirteen header lines of a manifest to be rejected further down. */
#define	RJ(acts, confs)							\
	H_CLONE("b", "f", "o")						\
	"#mode strict\n#actions " acts "\n#conflicts " confs "\n"

/* The four lines of a record, for the rejections that need one. */
#define	RJ_REC	"  why  x\n  base ()\n  from ()\n  onto ()\n"

/*
 * ZM34 to ZM38 and ZM43 to ZM50: every way a manifest can be wrong,
 * one test each, and each one demanding the line the error names. The
 * clone form's header is thirteen lines, so the tree section starts
 * at line 14.
 */
static void
test_rejections(void)
{
	/* ZM43: the version line is the first line or the file is not one */
	reject("version", "#rebase-manifest 4\n" RJ("0", "0"), "line 1: ");
	/* ZM34: an action nobody defined */
	reject("action", RJ("1", "0") "/\n    a zap /b\n    ..\n",
	    "line 15: ");
	/* ZM35: an escape that runs off the end of the name */
	reject("escape", RJ("1", "0") "/\n    a\\09 rm\n    ..\n",
	    "line 15: ");
	/* ZM36: the root closes and the tree goes on */
	reject("early close",
	    RJ("2", "0") "/\n    a rm\n    ..\n    b rm\n    ..\n",
	    "line 17: ");
	/* ZM36: the file ends with the root still open */
	reject("no close", RJ("1", "0") "/\n    a rm\n", "line 15: ");
	/* ZM37: the tree section without its root line */
	reject("no root", RJ("1", "0") "    a rm\n    ..\n", "line 14: ");
	/* ZM38: an ln naming a path the walk has not reached yet */
	reject("ln later", RJ("2", "0") "/\n    a ln /b\n    b rm\n    ..\n",
	    "line 15: ");
	/* ZM44: an ln naming itself, which is the same rule at zero */
	reject("ln self", RJ("1", "0") "/\n    a ln /a\n    ..\n",
	    "line 15: ");
	/*
	 * ZM45: a leaf given children. Indentation says nothing, so the
	 * inner line is read as the leaf's sibling and the two dots
	 * meant to close the leaf close the root instead, leaving the
	 * last two dots outside the section.
	 */
	reject("leaf parent",
	    RJ("2", "0") "/\n    a rm\n        b rm\n        ..\n    ..\n",
	    "line 18: ");
	/* ZM46: a name with neither an action nor a trailing slash */
	reject("bare leaf", RJ("0", "0") "/\n    a\n    ..\n", "line 15: ");
	/* ZM47: the count the header promised is not the count there is */
	reject("count", RJ("2", "0") "/\n    a rm\n    ..\n", "line 12: ");
	/* ZM48: a conflict mark pointing past the last record */
	reject("no record",
	    RJ("0", "1") "/\n    a conflict 2\n    ..\n"
	    "conflict 1 disagree\n" RJ_REC, "line 15: ");
	/* ZM49: a class the theory does not name */
	reject("class",
	    RJ("0", "1") "/\n    a conflict 1\n    ..\n"
	    "conflict 1 bogus\n" RJ_REC, "line 17: ");
	/* ZM50: records numbered 1..K in the order the tree named them */
	reject("record order",
	    RJ("0", "2") "/\n    a conflict 1\n    b conflict 2\n    ..\n"
	    "conflict 2 disagree\n" RJ_REC "conflict 1 disagree\n" RJ_REC,
	    "line 18: ");
}

/*
 * ---------------------------------------------------------------
 * The resolution of v4-manifest.md section 8: the same tree grammar
 * with a choice per name. Cells ZM60 to ZM81 of family ZM.
 * ---------------------------------------------------------------
 */

/*
 * The three header names of the documents below, which are the
 * manifest's own: H_CLONE("b", "f", "o") writes those names and
 * those guids, and a skeleton of it must give them back.
 */
#define	R_BFO								\
	"#rebase-resolution 5\n#base b 11\n#from f 22\n#onto o 33\n"

/* The section 8 example, which is the grammar's own statement of itself. */
static const char want_res[] =
	"#rebase-resolution 5\n"
	"#base zrtdiff/fs@base 11\n"
	"#from zrtdiff/from@work 22\n"
	"#onto zrtdiff/onto@work 33\n"
	"#mode strict\n"
	"#names 4\n"
	"#unanswered 1\n"
	"/\n"
	"    a conflict 1 -\n"
	"    d/ conflict 2 onto\n"
	"        f conflict 2 onto\n"
	"        ..\n"
	"    k drift keep\n"
	"    ..\n";

/* One resolution parsed out of a string, through a temporary file. */
static int
res_parse_text(const char *text, struct zr_resolution *out, char *err,
    size_t errlen)
{
	FILE *f;
	int rc;

	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fwrite(text, 1, strlen(text), f) == strlen(text));
	CHECK(fseek(f, 0, SEEK_SET) == 0);
	rc = zr_resolution_parse(f, out, err, errlen);
	(void) fclose(f);
	return (rc);
}

static void
res_parse_ok(const char *tag, const char *text, struct zr_resolution *out)
{
	char err[192];

	err[0] = '\0';
	checks++;
	if (res_parse_text(text, out, err, sizeof (err)) != 0) {
		printf("%s: the parse failed: %s\n", tag, err);
		exit(1);
	}
}

/* One resolution that must be rejected, with the line its error names. */
static void
res_reject(const char *tag, const char *text, const char *want)
{
	struct zr_resolution r;
	char err[192];

	err[0] = '\0';
	CHECK(res_parse_text(text, &r, err, sizeof (err)) == -1);
	checks++;
	if (strncmp(err, want, strlen(want)) != 0) {
		printf("%s: want \"%s...\", got \"%s\"\n", tag, want, err);
		exit(1);
	}
	zr_resolution_fini(&r);
}

/* One resolution written into a temporary file and read back whole. */
static char *
res_write(const struct zr_resolution *r, size_t *lenp)
{
	FILE *f;

	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_resolution_write(f, r) == 0);
	return (slurp(f, lenp));
}

/* Parse a resolution, write it out again, and demand the same bytes. */
static void
res_roundtrip(const char *tag, const char *text)
{
	struct zr_resolution r;
	char *got;
	size_t gotlen = 0;

	res_parse_ok(tag, text, &r);
	got = res_write(&r, &gotlen);
	compare(tag, got, gotlen, text);
	free(got);
	zr_resolution_fini(&r);
}

/* One parsed line, field by field. */
static void
check_rline(const struct zr_resolution *r, uint32_t i,
    enum zr_rline_kind kind, const char *path, int isdir, uint32_t group,
    enum zr_choice ch)
{
	const struct zr_rline *l;

	CHECK(i < r->zs_nlines);
	l = &r->zs_lines[i];
	CHECK(l->zl_kind == kind);
	CHECK(l->zl_pathlen == strlen(path));
	CHECK(memcmp(l->zl_path, path, l->zl_pathlen) == 0);
	CHECK(l->zl_isdir == isdir);
	CHECK(l->zl_group == group);
	CHECK(l->zl_choice == ch);
}

/*
 * ZM60, ZM61, ZM66: the section 8 example, read back line by line --
 * the paths rebuilt from the scoping, the group numbers kept, the
 * choices as written -- then written out again byte for byte.
 */
static void
test_res_example(void)
{
	struct zr_resolution r;

	res_parse_ok("resolution example", want_res, &r);
	CHECK(strcmp(r.zs_base, "zrtdiff/fs@base") == 0);
	CHECK(strcmp(r.zs_from, "zrtdiff/from@work") == 0);
	CHECK(strcmp(r.zs_onto, "zrtdiff/onto@work") == 0);
	CHECK(r.zs_mode == ZR_MODE_STRICT);
	CHECK(r.zs_names_declared == 4);
	CHECK(r.zs_unanswered_declared == 1);
	CHECK(r.zs_nlines == 4);
	check_rline(&r, 0, ZR_RL_CONFLICT, "/a", 0, 1, ZR_CH_NONE);
	check_rline(&r, 1, ZR_RL_CONFLICT, "/d", 1, 2, ZR_CH_ONTO);
	check_rline(&r, 2, ZR_RL_CONFLICT, "/d/f", 0, 2, ZR_CH_ONTO);
	check_rline(&r, 3, ZR_RL_DRIFT, "/k", 0, 0, ZR_CH_KEEP);
	CHECK(zr_resolution_unanswered(&r) == 1);
	zr_resolution_fini(&r);
	res_roundtrip("resolution example", want_res);
	/* the words, which are the same table the parse read */
	CHECK(strcmp(zr_choice_str(ZR_CH_NONE), "-") == 0);
	CHECK(strcmp(zr_choice_str(ZR_CH_KEEP), "keep") == 0);
	CHECK(strcmp(zr_choice_str(ZR_CH_ONTO), "onto") == 0);
	CHECK(strcmp(zr_choice_str(ZR_CH_FROM), "from") == 0);
}

/*
 * A manifest with conflicts at four depths: a leaf, a directory and
 * its child, and one under two directories that carry nothing of
 * their own. The skeleton of it is what the tests below turn on.
 */
static const char man_conf[] =
	H_CLONE("b", "f", "o")
	"#mode strict\n"
	"#actions 1\n"
	"#conflicts 2\n"
	"/\n"
	"    a conflict 1\n"
	"    d/ conflict 2\n"
	"        f conflict 2\n"
	"        ..\n"
	"    e/\n"
	"        deep/\n"
	"            x conflict 2\n"
	"            ..\n"
	"        ..\n"
	"    k rm\n"
	"    ..\n"
	"\n"
	"# a pool is one file and all its names: {names}letter; same\n"
	"# letter, same bytes\n"
	"conflict 1 changed-both\n"
	"  why  /a changed on both sides\n"
	"  base ({/a}x)\n"
	"  from ({/a}y)\n"
	"  onto ({/a}z)\n"
	"conflict 2 disagree\n"
	"  why  /d and /d/f disagree\n"
	"  base ({/d}x)\n"
	"  from ({/d}y)\n"
	"  onto ({/d}z)\n";

/* The skeleton of it: every conflict mark, nothing else, unanswered. */
static const char want_skel[] =
	R_BFO
	"#mode strict\n"
	"#names 4\n"
	"#unanswered 4\n"
	"/\n"
	"    a conflict 1 -\n"
	"    d/ conflict 2 -\n"
	"        f conflict 2 -\n"
	"        ..\n"
	"    e/\n"
	"        deep/\n"
	"            x conflict 2 -\n"
	"            ..\n"
	"        ..\n"
	"    ..\n";

/* Parse one manifest and hand back the skeleton it makes. */
static char *
skeleton_of(const char *tag, const char *text, enum zr_choice def,
    size_t *lenp)
{
	struct zr_resolution r;
	struct zr_parsed p;
	char *got;

	parse_ok(tag, text, &p);
	CHECK(zr_resolution_skeleton(&p, def, &r) == 0);
	got = res_write(&r, lenp);
	zr_resolution_fini(&r);
	zr_parsed_fini(&p);
	return (got);
}

/*
 * ZM62, ZM63, ZM64, ZM67, ZM69: the skeleton. Every conflict mark of
 * the manifest becomes a line, in manifest order, keeping its group
 * number and its trailing slash; the rm is not one and neither are
 * the directories that only scope, which the writer derives again
 * from the paths. A manifest with no conflicts gives an empty
 * document. The default choice is what every line starts as, which is
 * what a --take flag will pass. And the skeleton the run path writes
 * -- parse the manifest file, skeleton, write -- is these same bytes,
 * since it is this same call over the same text.
 */
static void
test_res_skeleton(void)
{
	struct zr_resolution r;
	struct zr_parsed p;
	char *got, *again;
	size_t gotlen = 0, againlen = 0;

	got = skeleton_of("skeleton", man_conf, ZR_CH_NONE, &gotlen);
	compare("skeleton", got, gotlen, want_skel);
	again = skeleton_of("skeleton again", man_conf, ZR_CH_NONE,
	    &againlen);
	compare("skeleton twice", again, againlen, got);
	free(again);
	free(got);
	/* the lines themselves, not only the bytes they are written as */
	parse_ok("skeleton fields", man_conf, &p);
	CHECK(zr_resolution_skeleton(&p, ZR_CH_NONE, &r) == 0);
	CHECK(r.zs_nlines == 4);
	CHECK(zr_resolution_unanswered(&r) == 4);
	check_rline(&r, 0, ZR_RL_CONFLICT, "/a", 0, 1, ZR_CH_NONE);
	check_rline(&r, 1, ZR_RL_CONFLICT, "/d", 1, 2, ZR_CH_NONE);
	check_rline(&r, 2, ZR_RL_CONFLICT, "/d/f", 0, 2, ZR_CH_NONE);
	check_rline(&r, 3, ZR_RL_CONFLICT, "/e/deep/x", 0, 2, ZR_CH_NONE);
	zr_resolution_fini(&r);
	zr_parsed_fini(&p);
	/* a --take flag's skeleton: answered on every line from the start */
	got = skeleton_of("take onto", man_conf, ZR_CH_ONTO, &gotlen);
	CHECK(strstr(got, "#unanswered 0\n") != NULL);
	CHECK(strstr(got, "    a conflict 1 onto\n") != NULL);
	CHECK(strstr(got, "            x conflict 2 onto\n") != NULL);
	CHECK(strstr(got, " -\n") == NULL);
	free(got);
	/* and a manifest with nothing to answer */
	got = skeleton_of("no conflicts",
	    H_CLONE("b", "f", "o")
	    "#mode permissive-merge\n#actions 1\n#conflicts 0\n"
	    "/\n    a rm\n    ..\n", ZR_CH_NONE, &gotlen);
	compare("no conflicts", got, gotlen,
	    R_BFO "#mode permissive-merge\n#names 0\n#unanswered 0\n"
	    "/\n    ..\n");
	free(got);
}

/*
 * ZM65: a drift line added to a skeleton, which is what a verify at
 * the conflicts gate does, and the document written and read again
 * with both kinds of line in it. A drift line is answered by
 * definition, so the unanswered count does not move.
 */
static void
test_res_drift(void)
{
	struct zr_resolution r, back;
	struct zr_parsed p;
	char *got;
	size_t gotlen = 0;

	parse_ok("drift base", man_conf, &p);
	CHECK(zr_resolution_skeleton(&p, ZR_CH_NONE, &r) == 0);
	zr_parsed_fini(&p);
	CHECK(zr_resolution_add_drift(&r, (const unsigned char *)"/e/deep",
	    7, 1, ZR_CH_KEEP) == 0);
	CHECK(zr_resolution_add_drift(&r, (const unsigned char *)"/n", 2, 0,
	    ZR_CH_ONTO) == 0);
	CHECK(r.zs_nlines == 6);
	CHECK(zr_resolution_unanswered(&r) == 4);
	CHECK(r.zs_names_declared == 6);
	CHECK(r.zs_unanswered_declared == 4);
	/* a drift line is never unanswered, and a path is absolute */
	CHECK(zr_resolution_add_drift(&r, (const unsigned char *)"/n", 2, 0,
	    ZR_CH_NONE) == -1);
	CHECK(zr_resolution_add_drift(&r, (const unsigned char *)"n", 1, 0,
	    ZR_CH_KEEP) == -1);
	CHECK(zr_resolution_add_drift(&r, (const unsigned char *)"/n/", 3, 1,
	    ZR_CH_KEEP) == -1);
	got = res_write(&r, &gotlen);
	zr_resolution_fini(&r);
	compare("drift", got, gotlen,
	    R_BFO "#mode strict\n#names 6\n#unanswered 4\n"
	    "/\n"
	    "    a conflict 1 -\n"
	    "    d/ conflict 2 -\n"
	    "        f conflict 2 -\n"
	    "        ..\n"
	    "    e/\n"
	    "        deep/ drift keep\n"
	    "            x conflict 2 -\n"
	    "            ..\n"
	    "        ..\n"
	    "    n drift onto\n"
	    "    ..\n");
	res_parse_ok("drift back", got, &back);
	CHECK(back.zs_nlines == 6);
	check_rline(&back, 3, ZR_RL_DRIFT, "/e/deep", 1, 0, ZR_CH_KEEP);
	check_rline(&back, 5, ZR_RL_DRIFT, "/n", 0, 0, ZR_CH_ONTO);
	zr_resolution_fini(&back);
	free(got);
}

/*
 * ZM68: the escaping is the manifest's, in a resolution line: a
 * space, a hash and the two bytes of an accented letter, in a leaf
 * and in a directory that scopes another line.
 */
static const char want_res_esc[] =
	R_BFO
	"#mode strict\n"
	"#names 3\n"
	"#unanswered 1\n"
	"/\n"
	"    a\\043b drift keep\n"
	"    caf\\303\\251\\040x conflict 1 -\n"
	"    d\\040d/\n"
	"        n conflict 2 from\n"
	"        ..\n"
	"    ..\n";

static void
test_res_escapes(void)
{
	struct zr_resolution r;

	res_roundtrip("resolution escapes", want_res_esc);
	res_parse_ok("resolution escapes", want_res_esc, &r);
	check_rline(&r, 0, ZR_RL_DRIFT, "/a#b", 0, 0, ZR_CH_KEEP);
	check_rline(&r, 1, ZR_RL_CONFLICT, "/caf\303\251 x", 0, 1,
	    ZR_CH_NONE);
	check_rline(&r, 2, ZR_RL_CONFLICT, "/d d/n", 0, 2, ZR_CH_FROM);
	zr_resolution_fini(&r);
}

/* The seven header lines of a resolution to be rejected further down. */
#define	RR(names, unans)						\
	R_BFO "#mode strict\n#names " names "\n#unanswered " unans "\n"

/*
 * ZM70 to ZM81: every way a resolution can be wrong, one test each,
 * and each one demanding the line the error names. The header is
 * seven lines, so the tree section starts at line 8.
 */
static void
test_res_rejections(void)
{
	/* ZM70: a manifest is not a resolution, however well formed */
	res_reject("manifest header", want_probe, "line 1: ");
	/* ZM71: the version line is the first line or the file is not one */
	res_reject("version", "#rebase-resolution 4\n" RR("0", "0"),
	    "line 1: ");
	/* ZM72: the five actions of section 4 are not choices */
	res_reject("action rm", RR("1", "0") "/\n    a rm\n    ..\n",
	    "line 9: ");
	res_reject("action write",
	    RR("1", "0") "/\n    a write /a\n    ..\n", "line 9: ");
	res_reject("action dup", RR("1", "0") "/\n    a dup /a\n    ..\n",
	    "line 9: ");
	/* ZM73: and neither is anything else */
	res_reject("kind", RR("1", "0") "/\n    a zap keep\n    ..\n",
	    "line 9: ");
	/* ZM74: a choice outside the four */
	res_reject("choice", RR("1", "0") "/\n    a drift base\n    ..\n",
	    "line 9: ");
	/* ZM75: only a conflict line is unanswered */
	res_reject("drift dash", RR("1", "1") "/\n    a drift -\n    ..\n",
	    "line 9: ");
	/* ZM76: a conflict line without its group number */
	res_reject("no group", RR("1", "0") "/\n    a conflict keep\n"
	    "    ..\n", "line 9: ");
	res_reject("group zero", RR("1", "0") "/\n    a conflict 0 keep\n"
	    "    ..\n", "line 9: ");
	/* and a conflict line with a group and no choice */
	res_reject("no choice", RR("1", "0") "/\n    a conflict 1\n"
	    "    ..\n", "line 9: ");
	/* ZM77: the count the header promised is not the count there is */
	res_reject("names", RR("2", "0") "/\n    a drift keep\n    ..\n",
	    "line 6: ");
	/* ZM78: and the same for the count that says completeness */
	res_reject("unanswered", RR("1", "0") "/\n    a conflict 1 -\n"
	    "    ..\n", "line 7: ");
	/* ZM79: a name with neither a choice nor a trailing slash */
	res_reject("bare leaf", RR("0", "0") "/\n    a\n    ..\n",
	    "line 9: ");
	/* ZM80: there is no second section */
	res_reject("tail", RR("1", "0") "/\n    a drift keep\n    ..\n"
	    "conflict 1 disagree\n", "line 11: ");
	/* ZM81: a field the grammar has no room for */
	res_reject("fifth field",
	    RR("1", "0") "/\n    a conflict 1 keep now\n    ..\n",
	    "line 9: ");
	/* the shared machinery still holds: escapes, scoping, the root */
	res_reject("escape", RR("1", "0") "/\n    a\\09 drift keep\n"
	    "    ..\n", "line 9: ");
	res_reject("no root", RR("1", "0") "    a drift keep\n    ..\n",
	    "line 8: ");
}

/*
 * ---------------------------------------------------------------
 * The header of v4-manifest.md section 6: the rebase's identity.
 * Family ZH of tests/MATRIX.md, cells ZH1 to ZH30.
 * ---------------------------------------------------------------
 */

/* The dataset form's header, which is the clone form's and three more. */
#define	H_DATASET							\
	"#rebase-manifest 5\n"						\
	"#result tank/main@pre\n"					\
	"#form dataset\n"						\
	"#base tank/proj@v1 12345678901234567890\n"			\
	"#from tank/dev@v2 0\n"						\
	"#onto tank/main@v3 18446744073709551615\n"			\
	"#presnap tank/main@pre\n"					\
	"#readonly off\n"						\
	"#canmount noauto\n"						\
	"#made from\n"							\
	"#tag zr-1a2b3c4d5e6f\n"					\
	"#take onto\n"							\
	"#written 2026-09-06T13:04:11Z\n"

#define	H_BODY	"#mode strict\n#actions 1\n#conflicts 0\n/\n    a rm\n    ..\n"

/*
 * And the body of a birth document, which is the two counts at zero
 * and the empty tree section: the root line and the two dots that
 * close it (documents-design.md, section 11.1).
 */
#define	H_BIRTH	"#mode strict\n#actions 0\n#conflicts 0\n/\n    ..\n"

/* The dataset form's header as the emitter is handed it, H_DATASET. */
static void
dataset_hdr(struct zr_manifest_hdr *h)
{
	memset(h, 0, sizeof (*h));
	h->result = "tank/main@pre";
	h->form = ZR_HFORM_DATASET;
	h->base = "tank/proj@v1";
	h->base_guid = 12345678901234567890ULL;
	h->from = "tank/dev@v2";
	h->from_guid = 0;
	h->onto = "tank/main@v3";
	h->onto_guid = 18446744073709551615ULL;
	h->presnap = "tank/main@pre";
	h->readonly = "off";
	h->canmount = "noauto";
	h->made = "from";
	h->tag = H_TAG;
	h->take = "onto";
	h->written = H_WRITTEN;
	h->mode = ZR_MODE_STRICT;
}

/*
 * ZH1 to ZH14: a clone-form header parses, and every line of it is
 * the value it carried. The result, the form, the three names with
 * their guids, made, the tag, take and the time of the write are the
 * run; the mode and the two counts are the decision.
 */
static void
test_header_clone(void)
{
	struct zr_parsed p;

	parse_ok("clone header", RJ("1", "0") "/\n    a rm\n    ..\n", &p);
	CHECK(strcmp(p.zp_result, H_RESULT) == 0);
	CHECK(p.zp_form == ZR_HFORM_CLONE);
	CHECK(strcmp(p.zp_base, "b") == 0);
	CHECK(p.zp_base_guid == 11);
	CHECK(strcmp(p.zp_from, "f") == 0);
	CHECK(p.zp_from_guid == 22);
	CHECK(strcmp(p.zp_onto, "o") == 0);
	CHECK(p.zp_onto_guid == 33);
	CHECK(p.zp_presnap == NULL);
	CHECK(p.zp_readonly == NULL);
	CHECK(p.zp_canmount == NULL);
	CHECK(strcmp(p.zp_made, "-") == 0);
	CHECK(strcmp(p.zp_tag, H_TAG) == 0);
	CHECK(strcmp(p.zp_take, "-") == 0);
	CHECK(strcmp(p.zp_written, H_WRITTEN) == 0);
	CHECK(p.zp_mode == ZR_MODE_STRICT);
	CHECK(p.zp_actions_declared == 1);
	CHECK(p.zp_conflicts_declared == 0);
	zr_parsed_fini(&p);
	/* ZH15: and a header of either form is written back as it came */
	roundtrip("clone header", RJ("1", "0") "/\n    a rm\n    ..\n");
	roundtrip("dataset header", H_DATASET H_BODY);
}

/*
 * ZH16 to ZH19: the dataset form's three lines, which are there if
 * and only if #form says dataset, and the guids at both ends of what
 * a uint64 holds.
 */
static void
test_header_dataset(void)
{
	struct zr_parsed p;

	parse_ok("dataset header", H_DATASET H_BODY, &p);
	CHECK(strcmp(p.zp_result, "tank/main@pre") == 0);
	CHECK(p.zp_form == ZR_HFORM_DATASET);
	CHECK(p.zp_base_guid == 12345678901234567890ULL);
	CHECK(p.zp_from_guid == 0);
	CHECK(p.zp_onto_guid == 18446744073709551615ULL);
	CHECK(strcmp(p.zp_presnap, "tank/main@pre") == 0);
	CHECK(strcmp(p.zp_readonly, "off") == 0);
	CHECK(strcmp(p.zp_canmount, "noauto") == 0);
	CHECK(strcmp(p.zp_made, "from") == 0);
	CHECK(strcmp(p.zp_take, "onto") == 0);
	zr_parsed_fini(&p);
}

/*
 * ZH20: the posix form, which is the run part written as placeholders
 * so that one fixture emits one document wherever it is run.
 */
static void
test_header_posix(void)
{
	struct zr_parsed p;

	parse_ok("posix header",
	    "#rebase-manifest 5\n#result -\n#form posix\n"
	    "#base base 0\n#from from 0\n#onto onto 0\n"
	    "#made -\n#tag -\n#take -\n#written -\n" H_BODY, &p);
	CHECK(p.zp_form == ZR_HFORM_POSIX);
	CHECK(strcmp(p.zp_result, "-") == 0);
	CHECK(p.zp_base_guid == 0);
	CHECK(strcmp(p.zp_tag, "-") == 0);
	CHECK(strcmp(p.zp_written, "-") == 0);
	CHECK(p.zp_presnap == NULL);
	zr_parsed_fini(&p);
}

/* The clone form's thirteen header lines, one per row, in order. */
static const char *const zh_line[] = {
	"#rebase-manifest 5", "#result " H_RESULT, "#form clone",
	"#base b 11", "#from f 22", "#onto o 33", "#made -",
	"#tag " H_TAG, "#take -", "#written " H_WRITTEN, "#mode strict",
	"#actions 1", "#conflicts 0"
};

#define	ZH_NLINE	(sizeof (zh_line) / sizeof (zh_line[0]))

/*
 * A header built out of those lines, with one of them left out, one
 * of them swapped with the next, or one of them replaced. The tree
 * section after it is the same one line every time.
 */
static void
zh_build(char *buf, size_t buflen, uint32_t drop, uint32_t swap,
    uint32_t at, const char *with)
{
	uint32_t i, j;
	size_t n = 0;

	for (i = 0; i < ZH_NLINE; i++) {
		const char *line;

		if (i == drop)
			continue;
		j = i;
		if (i == swap)
			j = i + 1;
		else if (i == swap + 1)
			j = i - 1;
		line = j == at && with != NULL ? with : zh_line[j];
		n += (size_t)snprintf(buf + n, buflen - n, "%s\n", line);
		CHECK(n < buflen);
	}
	n += (size_t)snprintf(buf + n, buflen - n, "/\n    a rm\n    ..\n");
	CHECK(n < buflen);
}

/*
 * ZH21 and ZH22: every line of the header is required, and every one
 * of them is required where it is. A line left out and a pair of
 * lines swapped are refusals naming a line, and the whole header is
 * walked so that no line is exempt.
 */
static void
test_header_order(void)
{
	char buf[1024], tag[64];
	uint32_t i;

	for (i = 1; i < ZH_NLINE; i++) {
		zh_build(buf, sizeof (buf), i, ZH_NLINE, ZH_NLINE, NULL);
		(void) snprintf(tag, sizeof (tag), "%s missing", zh_line[i]);
		reject(tag, buf, "line ");
	}
	for (i = 1; i + 1 < ZH_NLINE; i++) {
		zh_build(buf, sizeof (buf), ZH_NLINE, i, ZH_NLINE, NULL);
		(void) snprintf(tag, sizeof (tag), "%s misordered",
		    zh_line[i]);
		reject(tag, buf, "line ");
	}
}

/* One header with one line replaced, which must be refused. */
static void
zh_bad(const char *tag, uint32_t at, const char *with)
{
	char buf[1024];

	zh_build(buf, sizeof (buf), ZH_NLINE, ZH_NLINE, at, with);
	reject(tag, buf, "line ");
}

/*
 * ZH23 to ZH28: a value of the shape no line of the header may have.
 * Every line that is a word out of a list, a name and a guid, a tag
 * or a time is given one that is none of those.
 */
static void
test_header_malformed(void)
{
	zh_bad("version", 0, "#rebase-manifest 4");
	zh_bad("version word", 0, "#rebase-manifest five");
	zh_bad("result empty", 1, "#result");
	zh_bad("form", 2, "#form snapshot");
	zh_bad("base no guid", 3, "#base b");
	zh_bad("base no name", 3, "#base 11");
	zh_bad("made", 6, "#made onto");
	zh_bad("tag", 7, "#tag zr-nothex");
	zh_bad("tag prefix", 7, "#tag 1a2b3c4d5e6f");
	zh_bad("take", 8, "#take both");
	zh_bad("written", 9, "#written 2026-09-06 13:04:11");
	zh_bad("written short", 9, "#written 2026-09-06T13:04:11");
	zh_bad("written word", 9, "#written now");
	zh_bad("mode", 10, "#mode merge");
	zh_bad("actions", 11, "#actions many");
	zh_bad("conflicts", 12, "#conflicts -1");
	/* ZH24: and a guid is decimal, in range, with no sign and no hex */
	zh_bad("guid overflow", 4, "#from f 18446744073709551616");
	zh_bad("guid huge", 4, "#from f 99999999999999999999");
	zh_bad("guid sign", 4, "#from f +1");
	zh_bad("guid negative", 4, "#from f -1");
	zh_bad("guid hex", 4, "#from f 0x10");
	zh_bad("guid hex digits", 4, "#from f 1a");
	zh_bad("guid empty", 4, "#from f ");
}

/*
 * ZH29 and ZH30: the three dataset-form lines belong to that form
 * alone. One of them in a clone-form header is refused by name, and
 * one of them missing from a dataset-form header is the missing line
 * every other missing line is.
 */
static void
test_header_form_lines(void)
{
	reject("presnap in a clone form",
	    "#rebase-manifest 5\n#result r\n#form clone\n"
	    "#base b 1\n#from f 2\n#onto o 3\n#presnap p\n"
	    "#made -\n#tag -\n#take -\n#written -\n" H_BODY, "line 7: ");
	reject("readonly in a clone form",
	    "#rebase-manifest 5\n#result r\n#form clone\n"
	    "#base b 1\n#from f 2\n#onto o 3\n#readonly on\n"
	    "#made -\n#tag -\n#take -\n#written -\n" H_BODY, "line 7: ");
	reject("canmount in a posix form",
	    "#rebase-manifest 5\n#result -\n#form posix\n"
	    "#base b 0\n#from f 0\n#onto o 0\n#canmount on\n"
	    "#made -\n#tag -\n#take -\n#written -\n" H_BODY, "line 7: ");
	reject("no presnap in a dataset form",
	    "#rebase-manifest 5\n#result r\n#form dataset\n"
	    "#base b 1\n#from f 2\n#onto o 3\n"
	    "#readonly on\n#canmount on\n#made -\n#tag -\n#take -\n"
	    "#written -\n" H_BODY, "line 7: ");
	reject("no canmount in a dataset form",
	    "#rebase-manifest 5\n#result r\n#form dataset\n"
	    "#base b 1\n#from f 2\n#onto o 3\n"
	    "#presnap p\n#readonly on\n#made -\n#tag -\n#take -\n"
	    "#written -\n" H_BODY, "line 9: ");
	/* ZH28: and the two properties are the words zfs(8) prints */
	reject("readonly word",
	    "#rebase-manifest 5\n#result r\n#form dataset\n"
	    "#base b 1\n#from f 2\n#onto o 3\n"
	    "#presnap p\n#readonly noauto\n#canmount on\n#made -\n#tag -\n"
	    "#take -\n#written -\n" H_BODY, "line 8: ");
	reject("canmount word",
	    "#rebase-manifest 5\n#result r\n#form dataset\n"
	    "#base b 1\n#from f 2\n#onto o 3\n"
	    "#presnap p\n#readonly on\n#canmount later\n#made -\n#tag -\n"
	    "#take -\n#written -\n" H_BODY, "line 9: ");
	/* a line the header has no room for at all */
	reject("a line too many",
	    "#rebase-manifest 5\n#result r\n#form clone\n"
	    "#base b 1\n#from f 2\n#onto o 3\n#verify yes\n"
	    "#made -\n#tag -\n#take -\n#written -\n" H_BODY, "line 7: ");
}

/*
 * ZH31: the stamp the writer puts on #written is one the parse
 * accepts, so a manifest written now is a manifest that reads back.
 */
static void
test_header_stamp(void)
{
	char stamp[ZR_STAMP_MAX], text[1024];
	struct zr_parsed p;

	zr_manifest_stamp(stamp, sizeof (stamp));
	CHECK(stamp[0] != '\0');
	(void) snprintf(text, sizeof (text),
	    "#rebase-manifest 5\n#result r\n#form clone\n"
	    "#base b 1\n#from f 2\n#onto o 3\n#made -\n#tag -\n#take -\n"
	    "#written %s\n" H_BODY, stamp);
	parse_ok("stamp", text, &p);
	CHECK(strcmp(p.zp_written, stamp) == 0);
	zr_parsed_fini(&p);
}

/*
 * ZH32 and ZH33: the resolution's own header, which is the three
 * names with the manifest's guids beside them, and the skeleton that
 * copies all six from the parse.
 */
static void
test_header_resolution(void)
{
	struct zr_resolution r;
	struct zr_parsed p;
	char *got;
	size_t gotlen = 0;

	res_parse_ok("resolution header", want_res, &r);
	CHECK(strcmp(r.zs_base, "zrtdiff/fs@base") == 0);
	CHECK(r.zs_base_guid == 11);
	CHECK(r.zs_from_guid == 22);
	CHECK(r.zs_onto_guid == 33);
	zr_resolution_fini(&r);
	/* a name with no guid, a guid that is no number, and a version */
	res_reject("no guid", "#rebase-resolution 5\n#base b\n"
	    "#from f 22\n#onto o 33\n#mode strict\n#names 0\n"
	    "#unanswered 0\n/\n    ..\n", "line 2: ");
	res_reject("guid hex", "#rebase-resolution 5\n#base b 0x1\n"
	    "#from f 22\n#onto o 33\n#mode strict\n#names 0\n"
	    "#unanswered 0\n/\n    ..\n", "line 2: ");
	/* the skeleton carries the manifest's six, name and guid alike */
	parse_ok("skeleton guids", man_conf, &p);
	CHECK(zr_resolution_skeleton(&p, ZR_CH_NONE, &r) == 0);
	CHECK(r.zs_base_guid == p.zp_base_guid);
	CHECK(r.zs_from_guid == p.zp_from_guid);
	CHECK(r.zs_onto_guid == p.zp_onto_guid);
	got = res_write(&r, &gotlen);
	CHECK(strstr(got, "#base b 11\n") != NULL);
	CHECK(strstr(got, "#from f 22\n") != NULL);
	CHECK(strstr(got, "#onto o 33\n") != NULL);
	free(got);
	zr_resolution_fini(&r);
	zr_parsed_fini(&p);
}

/*
 * ZH39 to ZH42: the birth document, which is the header a run writes
 * before its record and rewrites whole at the decision. It is a
 * manifest like any other -- the same thirteen lines, sixteen in the
 * dataset form -- with the two counts at zero and nothing in the
 * tree section, and it must parse, hold nothing to apply and be
 * written back byte for byte in either form.
 */
static void
test_header_birth(void)
{
	struct zr_manifest_hdr h;
	struct zr_parsed p;
	FILE *f;
	char *got;
	size_t gotlen = 0;

	clone_hdr(&h, "b", "f", "o", ZR_MODE_STRICT);
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_birth(f, &h) == 0);
	got = slurp(f, &gotlen);
	compare("birth clone", got, gotlen, H_CLONE("b", "f", "o") H_BIRTH);
	free(got);
	parse_ok("birth clone", H_CLONE("b", "f", "o") H_BIRTH, &p);
	CHECK(p.zp_nactions == 0);
	CHECK(p.zp_nrecords == 0);
	CHECK(p.zp_actions_declared == 0);
	CHECK(p.zp_conflicts_declared == 0);
	CHECK(strcmp(p.zp_result, H_RESULT) == 0);
	CHECK(p.zp_form == ZR_HFORM_CLONE);
	CHECK(strcmp(p.zp_tag, H_TAG) == 0);
	/* ZH41: the time of the write is on it, as on any manifest */
	CHECK(strcmp(p.zp_written, H_WRITTEN) == 0);
	zr_parsed_fini(&p);
	/* ZH40: and the parse is written back as the emitter wrote it */
	roundtrip("birth clone", H_CLONE("b", "f", "o") H_BIRTH);
	roundtrip("birth dataset", H_DATASET H_BIRTH);
	/*
	 * ZH42: the dataset form's three lines are on the birth
	 * document too -- they are what an --abort at a gate before
	 * the decision reads to put the dataset back -- and a header
	 * claiming that form without them is refused here as it is
	 * refused by the emitter (ZH36).
	 */
	dataset_hdr(&h);
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_birth(f, &h) == 0);
	got = slurp(f, &gotlen);
	compare("birth dataset", got, gotlen, H_DATASET H_BIRTH);
	free(got);
	parse_ok("birth dataset", H_DATASET H_BIRTH, &p);
	CHECK(p.zp_form == ZR_HFORM_DATASET);
	CHECK(strcmp(p.zp_presnap, "tank/main@pre") == 0);
	CHECK(strcmp(p.zp_readonly, "off") == 0);
	CHECK(strcmp(p.zp_canmount, "noauto") == 0);
	CHECK(p.zp_actions_declared == 0);
	zr_parsed_fini(&p);
	h.canmount = NULL;
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_birth(f, &h) == -1);
	CHECK(fclose(f) == 0);
}

int
main(void)
{
	test_probe();
	test_escapes();
	test_foreign_path();
	test_parse_probe();
	test_parse_fixture();
	test_write_back();
	test_write_shapes();
	test_rejections();
	test_res_example();
	test_res_skeleton();
	test_res_drift();
	test_res_escapes();
	test_res_rejections();
	test_header_clone();
	test_header_dataset();
	test_header_posix();
	test_header_order();
	test_header_malformed();
	test_header_form_lines();
	test_header_stamp();
	test_header_resolution();
	test_header_birth();
	printf("check_manifest: %d checks passed\n", checks);
	return (0);
}
