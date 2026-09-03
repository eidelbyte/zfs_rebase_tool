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
 * manifest can be wrong, each demanding the line its error names. The
 * family is ZM of tests/MATRIX.md and every test below names the cells
 * it closes.
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
 * The note's section 7: the probe scenario with an onto side that
 * edited a differently and edited keep/k. b is deleted on from, d is
 * renamed to e, the pool {h1 h2} is edited through one name and gains
 * a third, keep/k needs nothing, n is new, and a conflicts.
 */
static const char want_probe[] =
	"#rebase-manifest 4\n"
	"#base zrtdiff/fs@base\n"
	"#from zrtdiff/from@work\n"
	"#onto zrtdiff/onto@work\n"
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

	hdr.base = "zrtdiff/fs@base";
	hdr.from = "zrtdiff/from@work";
	hdr.onto = "zrtdiff/onto@work";
	hdr.mode = ZR_MODE_STRICT;
	run(&w, &hdr, "probe", want_probe);
	world_fini(&w);
}

/*
 * A leaf holding a space and the two bytes of an accented letter, in
 * a tree line, in a cp argument and in every line of a record; and a
 * directory whose own name needs escaping too.
 */
static const char want_escapes[] =
	"#rebase-manifest 4\n"
	"#base zrt/base@s\n"
	"#from zrt/from@s\n"
	"#onto zrt/onto@s\n"
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

	hdr.base = "zrt/base@s";
	hdr.from = "zrt/from@s";
	hdr.onto = "zrt/onto@s";
	hdr.mode = ZR_MODE_STRICT;
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
	"#rebase-manifest 4\n"
	"#base zrt/base@s\n"
	"#from zrt/from@s\n"
	"#onto zrt/onto@s\n"
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

	hdr.base = "zrt/base@s";
	hdr.from = "zrt/from@s";
	hdr.onto = "zrt/onto@s";
	hdr.mode = ZR_MODE_STRICT;
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

/* Two parses of one manifest agree in every field. */
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
	CHECK(strcmp(a->zp_base, b->zp_base) == 0);
	CHECK(strcmp(a->zp_from, b->zp_from) == 0);
	CHECK(strcmp(a->zp_onto, b->zp_onto) == 0);
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
 * manifest as the one the emitter wrote here, so the two parses must
 * agree field by field. The fixture is the document the box will
 * compare against, and this is what ties it to the code.
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
	compare("fixture expect", expect, strlen(expect), want_probe);
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
	    "#rebase-manifest 4\n#base b\n#from f\n#onto o\n"
	    "#mode strict\n#actions 3\n#conflicts 0\n"
	    "/\n"
	    "    d/ rm\n"
	    "        ..\n"
	    "    e/ cp /e\n"
	    "        f cp /e/f\n"
	    "        ..\n"
	    "    ..\n");
	roundtrip("walk order",
	    "#rebase-manifest 4\n#base b\n#from f\n#onto o\n"
	    "#mode strict\n#actions 2\n#conflicts 0\n"
	    "/\n"
	    "    a/\n"
	    "        b rm\n"
	    "        ..\n"
	    "    a-1 rm\n"
	    "    ..\n");
	roundtrip("every class",
	    "#rebase-manifest 4\n#base b\n#from f\n#onto o\n"
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
	    "#rebase-manifest 4\n#base b\n#from f\n#onto o\n"
	    "#mode strict\n#actions 0\n#conflicts 0\n"
	    "/\n"
	    "    ..\n");
}

/* The seven header lines of a manifest to be rejected further down. */
#define	RJ(acts, confs)							\
	"#rebase-manifest 4\n#base b\n#from f\n#onto o\n"		\
	"#mode strict\n#actions " acts "\n#conflicts " confs "\n"

/* The four lines of a record, for the rejections that need one. */
#define	RJ_REC	"  why  x\n  base ()\n  from ()\n  onto ()\n"

/*
 * ZM34 to ZM38 and ZM43 to ZM50: every way a manifest can be wrong,
 * one test each, and each one demanding the line the error names. The
 * header is seven lines, so the tree section starts at line 8.
 */
static void
test_rejections(void)
{
	/* ZM43: the version line is the first line or the file is not one */
	reject("version", "#rebase-manifest 5\n" RJ("0", "0"), "line 1: ");
	/* ZM34: an action nobody defined */
	reject("action", RJ("1", "0") "/\n    a zap /b\n    ..\n",
	    "line 9: ");
	/* ZM35: an escape that runs off the end of the name */
	reject("escape", RJ("1", "0") "/\n    a\\09 rm\n    ..\n",
	    "line 9: ");
	/* ZM36: the root closes and the tree goes on */
	reject("early close",
	    RJ("2", "0") "/\n    a rm\n    ..\n    b rm\n    ..\n",
	    "line 11: ");
	/* ZM36: the file ends with the root still open */
	reject("no close", RJ("1", "0") "/\n    a rm\n", "line 9: ");
	/* ZM37: the tree section without its root line */
	reject("no root", RJ("1", "0") "    a rm\n    ..\n", "line 8: ");
	/* ZM38: an ln naming a path the walk has not reached yet */
	reject("ln later", RJ("2", "0") "/\n    a ln /b\n    b rm\n    ..\n",
	    "line 9: ");
	/* ZM44: an ln naming itself, which is the same rule at zero */
	reject("ln self", RJ("1", "0") "/\n    a ln /a\n    ..\n",
	    "line 9: ");
	/*
	 * ZM45: a leaf given children. Indentation says nothing, so the
	 * inner line is read as the leaf's sibling and the two dots
	 * meant to close the leaf close the root instead, leaving the
	 * last two dots outside the section.
	 */
	reject("leaf parent",
	    RJ("2", "0") "/\n    a rm\n        b rm\n        ..\n    ..\n",
	    "line 12: ");
	/* ZM46: a name with neither an action nor a trailing slash */
	reject("bare leaf", RJ("0", "0") "/\n    a\n    ..\n", "line 9: ");
	/* ZM47: the count the header promised is not the count there is */
	reject("count", RJ("2", "0") "/\n    a rm\n    ..\n", "line 6: ");
	/* ZM48: a conflict mark pointing past the last record */
	reject("no record",
	    RJ("0", "1") "/\n    a conflict 2\n    ..\n"
	    "conflict 1 disagree\n" RJ_REC, "line 9: ");
	/* ZM49: a class the theory does not name */
	reject("class",
	    RJ("0", "1") "/\n    a conflict 1\n    ..\n"
	    "conflict 1 bogus\n" RJ_REC, "line 11: ");
	/* ZM50: records numbered 1..K in the order the tree named them */
	reject("record order",
	    RJ("0", "2") "/\n    a conflict 1\n    b conflict 2\n    ..\n"
	    "conflict 2 disagree\n" RJ_REC "conflict 1 disagree\n" RJ_REC,
	    "line 12: ");
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
	printf("check_manifest: %d checks passed\n", checks);
	return (0);
}
