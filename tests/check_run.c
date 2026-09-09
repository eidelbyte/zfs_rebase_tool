/*
 * The run's guards that need no pool. A real run wants ZFS, root and
 * a box; what is here is the questions the run asks that are pure
 * functions of a decision and the two walks beside it, which any
 * machine can answer.
 *
 * So far that is one: the system file flags against the securelevel.
 * The sysctl that says what securelevel this box is at is FreeBSD's
 * and is the box's, but the rule it feeds takes the level as an
 * argument, so the rule itself is asked here over a decision and two
 * walks built by hand -- three trees of two names, one of them
 * changed on from and the other the same everywhere, decided in
 * strict mode, with the flags put on the attributes of whichever
 * pool the case is about.
 *
 * The family is ZX of tests/MATRIX.md. Covered here: ZX242. ZX23,
 * the refusal a real run makes at a real securelevel, stays the
 * box's: raising the level wants a reboot.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decide.h"
#include "name.h"
#include "run.h"
#include "walk.h"

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

static struct zr_names *names;

/* A walk with no filesystem under it: the tree and the attributes. */
static void
walk_init(struct zr_walk *w)
{
	memset(w, 0, sizeof (struct zr_walk));
	w->zw_rootfd = -1;
	CHECK(zr_tree_init(&w->zw_tree, names) == 0);
}

/* One name, its own pool, and the content handle the case wants. */
static zr_pool_t
walk_add(struct zr_walk *w, const char *path, uint64_t ino, uint32_t content)
{
	zr_name_t n;
	zr_pool_t q;

	n = zr_names_intern(names, path, strlen(path));
	CHECK(n != ZR_NAME_NONE);
	q = zr_tree_add(&w->zw_tree, n, ino, ZR_T_FILE, 1);
	CHECK(q != ZR_POOL_NONE);
	w->zw_tree.zt_pools[q].zp_content = content;
	return (q);
}

/* Sealed, with one attribute record per pool and no flags on any. */
static void
walk_seal(struct zr_walk *w)
{
	CHECK(zr_tree_seal(&w->zw_tree) == 0);
	w->zw_nattrs = w->zw_tree.zt_npools;
	w->zw_attrs = calloc((size_t)(w->zw_nattrs == 0 ? 1 : w->zw_nattrs),
	    sizeof (struct zr_attr));
	CHECK(w->zw_attrs != NULL);
}

static void
walk_free(struct zr_walk *w)
{
	free(w->zw_attrs);
	w->zw_attrs = NULL;
	w->zw_nattrs = 0;
	zr_tree_fini(&w->zw_tree);
}

/* The refusal must name this word, and the message is printed if not. */
static void
says(const char *err, const char *word)
{
	if (strstr(err, word) == NULL)
		printf("  message lacks \"%s\": %s\n", word, err);
	CHECK(strstr(err, word) != NULL);
}

/*
 * ZX242: the flag guard, both sides of it. /a is base's and onto's
 * still and changed on from, so the decision writes from's object
 * there and rewrites onto's; /b is the same object in all three and
 * the decision says nothing about it at all.
 */
static void
check_flags_guard(void)
{
	struct zr_walk wb, wf, wo;
	struct zr_decision d;
	char err[512];
	zr_pool_t fa, oa, fb, ob;

	names = zr_names_create();
	CHECK(names != NULL);
	walk_init(&wb);
	walk_init(&wf);
	walk_init(&wo);
	(void) walk_add(&wb, "/a", 1, 10);
	(void) walk_add(&wb, "/b", 2, 30);
	fa = walk_add(&wf, "/a", 1, 20);
	fb = walk_add(&wf, "/b", 2, 30);
	oa = walk_add(&wo, "/a", 1, 10);
	ob = walk_add(&wo, "/b", 2, 30);
	walk_seal(&wb);
	walk_seal(&wf);
	walk_seal(&wo);
	CHECK(zr_decide(&wb.zw_tree, &wf.zw_tree, &wo.zw_tree,
	    ZR_MODE_STRICT, &d) == 0);

	/* nothing carries a flag: nothing to refuse, at any level */
	err[0] = 'x';
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 0);
	CHECK(err[0] == '\0');

#if defined(SF_IMMUTABLE) && defined(SF_APPEND) && defined(SF_NOUNLINK)
	/* onto's side: the apply would have to clear it, and cannot */
	wo.zw_attrs[oa].za_flags = (uint32_t)SF_IMMUTABLE;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 1);
	says(err, "securelevel 1");
	says(err, "/a");
	says(err, "onto's side");

	/* at securelevel 0 the apply clears it itself: nothing refused */
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 0, err,
	    sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	CHECK(zr_flags_refused(&d, &wo, &wf, names, -1, err,
	    sizeof (err)) == 0);

	/* from's side: the apply would write it on, and it would stick */
	wo.zw_attrs[oa].za_flags = 0;
	wf.zw_attrs[fa].za_flags = (uint32_t)SF_NOUNLINK;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 1);
	says(err, "/a");
	says(err, "from's side");

	/* both at once: onto's is read first, and it is the one named */
	wo.zw_attrs[oa].za_flags = (uint32_t)SF_APPEND;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 1);
	says(err, "onto's side");

	/*
	 * A flag on the name the decision leaves alone, on either side
	 * or on both: the apply never touches that object, so there is
	 * nothing the securelevel could stop.
	 */
	wo.zw_attrs[oa].za_flags = 0;
	wf.zw_attrs[fa].za_flags = 0;
	wo.zw_attrs[ob].za_flags = (uint32_t)SF_IMMUTABLE;
	wf.zw_attrs[fb].za_flags = (uint32_t)SF_IMMUTABLE;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 0);
	CHECK(err[0] == '\0');
#else
	(void) fa;
	(void) oa;
	(void) fb;
	(void) ob;
	printf("skip the flag cases: this platform has no system flags\n");
#endif

	zr_decision_fini(&d);
	walk_free(&wo);
	walk_free(&wf);
	walk_free(&wb);
	zr_names_destroy(names);
	names = NULL;
}

int
main(void)
{
	check_flags_guard();
	printf("check_run: %d checks passed\n", checks);
	return (0);
}
