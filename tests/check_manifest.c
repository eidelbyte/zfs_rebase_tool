/*
 * The manifest emitter's tests. Three scenarios are built by hand
 * through the name table, decided in strict mode and emitted into a
 * temporary file, then compared byte for byte with the text the
 * format note prints: the probe scenario of the note's section 7, a
 * name that needs escaping in both a tree line and a conflict record,
 * and a result pool whose anchor onto invented, so that its write
 * carries a path from a name other than its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decide.h"
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
 * Emit into a temporary file and read it back whole. Portable C: no
 * open_memstream, no feature-test macros.
 */
static char *
emit(struct world *w, const struct zr_manifest_hdr *hdr,
    const struct zr_decision *d, size_t *lenp)
{
	FILE *f;
	char *buf;
	long n;

	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_emit(f, hdr, &w->w_t[W_BASE], &w->w_t[W_FROM],
	    &w->w_t[W_ONTO], d) == 0);
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

int
main(void)
{
	test_probe();
	test_escapes();
	test_foreign_path();
	printf("check_manifest: %d checks passed\n", checks);
	return (0);
}
