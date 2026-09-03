/*
 * The name-table tests: one shared table under three trees, the
 * interning, lookup and parent rules, the pool, seal and verify
 * rules, and a growth run over 100000 names.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "name.h"

#define	NGROW	100000

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

static zr_name_t
intern(struct zr_names *ns, const char *s)
{
	return (zr_names_intern(ns, s, strlen(s)));
}

static zr_name_t
look(struct zr_names *ns, const char *s)
{
	return (zr_names_lookup(ns, s, strlen(s)));
}

/*
 * Ids are dense from 0 in first-intern order, interning is stable,
 * lookup agrees with it, and the (pointer, length) contract holds:
 * only the given bytes are read.
 */
static void
test_intern(struct zr_names *ns)
{
	const char *s;
	size_t len;
	static const char raw[] = "/zz/qq";

	CHECK(intern(ns, "/") == 0);
	CHECK(intern(ns, "/a") == 1);
	CHECK(intern(ns, "/b") == 2);
	CHECK(intern(ns, "/c") == 3);
	CHECK(intern(ns, "/d") == 4);
	CHECK(intern(ns, "/d/f") == 5);
	CHECK(zr_names_count(ns) == 6);

	CHECK(intern(ns, "/d/f") == 5);
	CHECK(intern(ns, "/") == 0);
	CHECK(zr_names_count(ns) == 6);
	CHECK(look(ns, "/d/f") == 5);
	CHECK(look(ns, "/nope") == ZR_NAME_NONE);

	s = zr_names_str(ns, 5, &len);
	CHECK(s != NULL);
	CHECK(len == 4);
	CHECK(memcmp(s, "/d/f", 4) == 0);
	CHECK(s[4] == '\0');
	s = zr_names_str(ns, 0, &len);
	CHECK(s != NULL && len == 1 && s[0] == '/');
	CHECK(zr_names_str(ns, 6, &len) == NULL);
	CHECK(zr_names_str(ns, ZR_NAME_NONE, &len) == NULL);

	CHECK(intern(ns, "a") == ZR_NAME_NONE);
	CHECK(intern(ns, "/a/") == ZR_NAME_NONE);
	CHECK(intern(ns, "//a") == ZR_NAME_NONE);
	CHECK(intern(ns, "/a/./b") == ZR_NAME_NONE);
	CHECK(intern(ns, "/a/../b") == ZR_NAME_NONE);
	CHECK(zr_names_intern(ns, "", 0) == ZR_NAME_NONE);
	CHECK(intern(ns, "/a//b") == ZR_NAME_NONE);
	CHECK(intern(ns, "/..") == ZR_NAME_NONE);
	CHECK(intern(ns, "/.") == ZR_NAME_NONE);
	CHECK(zr_names_count(ns) == 6);

	/* the length rules, not a terminator the caller never promised */
	CHECK(zr_names_intern(ns, raw, 3) == 6);
	CHECK(zr_names_lookup(ns, raw, 3) == 6);
	CHECK(look(ns, "/zz") == 6);
	CHECK(look(ns, "/zz/qq") == ZR_NAME_NONE);
	CHECK(zr_names_count(ns) == 7);
}

/*
 * parent is a lookup, not a construction: it answers only for parents
 * that are themselves interned.
 */
static void
test_parent(struct zr_names *ns)
{
	zr_name_t z;

	CHECK(zr_names_parent(ns, look(ns, "/d/f")) == look(ns, "/d"));
	CHECK(zr_names_parent(ns, look(ns, "/a")) == 0);
	CHECK(zr_names_parent(ns, 0) == ZR_NAME_NONE);
	CHECK(zr_names_parent(ns, ZR_NAME_NONE) == ZR_NAME_NONE);
	CHECK(zr_names_parent(ns, 4242) == ZR_NAME_NONE);

	z = intern(ns, "/x/y/z");
	CHECK(z == 7);
	CHECK(zr_names_parent(ns, z) == ZR_NAME_NONE);
	CHECK(intern(ns, "/x/y") == 8);
	CHECK(zr_names_parent(ns, z) == 8);
	CHECK(zr_names_parent(ns, 8) == ZR_NAME_NONE);
}

/*
 * base: /a ino 1; /b and /c ino 2 nlink 2; /d a directory ino 3;
 * /d/f ino 4. /c is added before /b so that seal has something to
 * sort.
 */
static void
build_base(struct zr_tree *tr, struct zr_names *ns)
{
	zr_pool_t pb, pc;

	CHECK(zr_tree_init(tr, ns) == 0);
	CHECK(zr_tree_add(tr, look(ns, "/a"), 1, ZR_T_FILE, 1) == 0);
	pc = zr_tree_add(tr, look(ns, "/c"), 2, ZR_T_FILE, 2);
	pb = zr_tree_add(tr, look(ns, "/b"), 2, ZR_T_FILE, 2);
	CHECK(pc == 1);
	CHECK(pb == pc);
	CHECK(zr_tree_add(tr, look(ns, "/d"), 3, ZR_T_DIR, 2) == 2);
	CHECK(zr_tree_add(tr, look(ns, "/d/f"), 4, ZR_T_FILE, 1) == 3);
	CHECK(tr->zt_npools == 4);
}

/* from: /a and /b are the hardlink pool here, /c is alone. */
static void
build_from(struct zr_tree *tr, struct zr_names *ns)
{
	CHECK(zr_tree_init(tr, ns) == 0);
	CHECK(zr_tree_add(tr, look(ns, "/a"), 1, ZR_T_FILE, 2) == 0);
	CHECK(zr_tree_add(tr, look(ns, "/b"), 1, ZR_T_FILE, 2) == 0);
	CHECK(zr_tree_add(tr, look(ns, "/c"), 2, ZR_T_FILE, 1) == 1);
	CHECK(zr_tree_add(tr, look(ns, "/d"), 3, ZR_T_DIR, 2) == 2);
	CHECK(zr_tree_add(tr, look(ns, "/d/f"), 4, ZR_T_FILE, 1) == 3);
	CHECK(tr->zt_npools == 4);
}

/* onto: no /b at all. */
static void
build_onto(struct zr_tree *tr, struct zr_names *ns)
{
	CHECK(zr_tree_init(tr, ns) == 0);
	CHECK(zr_tree_add(tr, look(ns, "/a"), 1, ZR_T_FILE, 1) == 0);
	CHECK(zr_tree_add(tr, look(ns, "/c"), 2, ZR_T_FILE, 1) == 1);
	CHECK(zr_tree_add(tr, look(ns, "/d"), 3, ZR_T_DIR, 2) == 2);
	CHECK(zr_tree_add(tr, look(ns, "/d/f"), 4, ZR_T_FILE, 1) == 3);
	CHECK(tr->zt_npools == 4);
}

static void
test_trees(struct zr_names *ns)
{
	struct zr_tree base, from, onto;
	const struct zr_pool *pool;
	char err[256];
	zr_name_t a, b, c, d, f, spare;
	zr_pool_t p;

	a = look(ns, "/a");
	b = look(ns, "/b");
	c = look(ns, "/c");
	d = look(ns, "/d");
	f = look(ns, "/d/f");
	spare = look(ns, "/x/y");
	build_base(&base, ns);
	build_from(&from, ns);
	build_onto(&onto, ns);

	/* the name map and the inode map answer the same thing */
	CHECK(zr_tree_pool(&base, b) == zr_tree_pool_by_ino(&base, 2));
	CHECK(zr_tree_pool(&base, c) == zr_tree_pool_by_ino(&base, 2));
	CHECK(zr_tree_pool(&base, a) == zr_tree_pool_by_ino(&base, 1));
	CHECK(zr_tree_pool(&base, f) == zr_tree_pool_by_ino(&base, 4));
	CHECK(zr_tree_pool_by_ino(&base, 99) == ZR_POOL_NONE);
	CHECK(zr_tree_pool(&base, spare) == ZR_POOL_NONE);
	CHECK(zr_tree_pool(&base, ZR_NAME_NONE) == ZR_POOL_NONE);

	/* one table, three trees, three different shapes */
	CHECK(zr_tree_pool(&base, a) != zr_tree_pool(&base, b));
	CHECK(zr_tree_pool(&from, a) == zr_tree_pool(&from, b));
	CHECK(zr_tree_pool(&onto, b) == ZR_POOL_NONE);
	CHECK(zr_tree_pool(&onto, a) != ZR_POOL_NONE);

	p = zr_tree_pool(&base, b);
	pool = &base.zt_pools[p];
	CHECK(pool->zp_ino == 2);
	CHECK(pool->zp_type == ZR_T_FILE);
	CHECK(pool->zp_nlink == 2);
	CHECK(pool->zp_nnames == 2);
	CHECK(pool->zp_content == ZR_CONTENT_NONE);
	/* /c was added first, so before seal the ids run backwards */
	CHECK(pool->zp_names[0] == c);
	CHECK(pool->zp_names[1] == b);

	/* verify holds before sealing too */
	CHECK(zr_tree_verify(&base, err, sizeof (err)) == 0);

	/* every rejection changes nothing */
	CHECK(zr_tree_add(&base, a, 1, ZR_T_FILE, 1) == ZR_POOL_NONE);
	CHECK(zr_tree_add(&base, a, 7, ZR_T_FILE, 1) == ZR_POOL_NONE);
	CHECK(zr_tree_add(&base, spare, 3, ZR_T_DIR, 2) == ZR_POOL_NONE);
	CHECK(zr_tree_add(&base, spare, 1, ZR_T_DIR, 1) == ZR_POOL_NONE);
	CHECK(zr_tree_add(&base, spare, 1, ZR_T_FILE, 5) == ZR_POOL_NONE);
	CHECK(zr_tree_add(&base, ZR_NAME_NONE, 8, ZR_T_FILE, 1) ==
	    ZR_POOL_NONE);
	CHECK(zr_tree_add(&base, 4242, 8, ZR_T_FILE, 1) == ZR_POOL_NONE);
	CHECK(base.zt_npools == 4);
	CHECK(zr_tree_pool(&base, spare) == ZR_POOL_NONE);
	CHECK(base.zt_pools[zr_tree_pool(&base, a)].zp_nnames == 1);

	/* seal sorts each pool's names ascending by id */
	CHECK(base.zt_sealed == 0);
	CHECK(zr_tree_seal(&base) == 0);
	CHECK(base.zt_sealed != 0);
	pool = &base.zt_pools[p];
	CHECK(pool->zp_names[0] == b);
	CHECK(pool->zp_names[1] == c);
	CHECK(zr_tree_seal(&from) == 0);
	CHECK(zr_tree_seal(&onto) == 0);
	pool = &from.zt_pools[zr_tree_pool(&from, a)];
	CHECK(pool->zp_names[0] == a);
	CHECK(pool->zp_names[1] == b);

	/* a sealed tree takes no more names */
	CHECK(zr_tree_add(&base, spare, 9, ZR_T_FILE, 1) == ZR_POOL_NONE);
	CHECK(base.zt_npools == 4);

	CHECK(zr_tree_verify(&base, err, sizeof (err)) == 0);
	CHECK(zr_tree_verify(&from, err, sizeof (err)) == 0);
	CHECK(zr_tree_verify(&onto, err, sizeof (err)) == 0);
	CHECK(zr_tree_verify(&onto, NULL, 0) == 0);
	CHECK(d != ZR_NAME_NONE);

	zr_tree_fini(&base);
	zr_tree_fini(&from);
	zr_tree_fini(&onto);
	CHECK(base.zt_pools == NULL);
	CHECK(base.zt_npools == 0);
}

/* two names on an inode the walk said had three links */
static void
test_verify_fails(struct zr_names *ns)
{
	struct zr_tree tr;
	char err[256];

	CHECK(zr_tree_init(&tr, ns) == 0);
	CHECK(zr_tree_add(&tr, look(ns, "/a"), 1, ZR_T_FILE, 3) == 0);
	CHECK(zr_tree_add(&tr, look(ns, "/b"), 1, ZR_T_FILE, 3) == 0);
	memset(err, 0, sizeof (err));
	CHECK(zr_tree_verify(&tr, err, sizeof (err)) == -1);
	CHECK(strcmp(err, "/a: 2 names but nlink 3") == 0);
	CHECK(zr_tree_verify(&tr, NULL, 0) == -1);
	zr_tree_fini(&tr);
}

/*
 * Nothing has a fixed limit: intern 100000 generated paths, find
 * every one again with the id it was given, then make each its own
 * pool in one tree.
 */
static void
test_growth(void)
{
	struct zr_names *ns;
	struct zr_tree tr;
	const char *s;
	char buf[64];
	char err[256];
	size_t len;
	uint32_t i;
	int n;

	ns = zr_names_create();
	CHECK(ns != NULL);
	for (i = 0; i < NGROW; i++) {
		n = snprintf(buf, sizeof (buf), "/g%u/f%u", i % 97, i);
		CHECK(n > 0 && (size_t)n < sizeof (buf));
		CHECK(zr_names_intern(ns, buf, (size_t)n) == i);
	}
	CHECK(zr_names_count(ns) == NGROW);
	for (i = 0; i < NGROW; i++) {
		n = snprintf(buf, sizeof (buf), "/g%u/f%u", i % 97, i);
		CHECK(zr_names_lookup(ns, buf, (size_t)n) == i);
		s = zr_names_str(ns, i, &len);
		CHECK(s != NULL);
		CHECK(len == (size_t)n);
		CHECK(memcmp(s, buf, len) == 0);
	}
	CHECK(zr_names_intern(ns, "/g0/f0", 6) == 0);
	CHECK(zr_names_count(ns) == NGROW);

	CHECK(zr_tree_init(&tr, ns) == 0);
	for (i = 0; i < NGROW; i++)
		CHECK(zr_tree_add(&tr, i, i + 1, ZR_T_FILE, 1) == i);
	CHECK(tr.zt_npools == NGROW);
	for (i = 0; i < NGROW; i++) {
		CHECK(zr_tree_pool(&tr, i) == i);
		CHECK(zr_tree_pool_by_ino(&tr, i + 1) == i);
	}
	CHECK(zr_tree_seal(&tr) == 0);
	CHECK(zr_tree_verify(&tr, err, sizeof (err)) == 0);
	zr_tree_fini(&tr);
	zr_names_destroy(ns);
}

int
main(void)
{
	struct zr_names *ns;

	ns = zr_names_create();
	CHECK(ns != NULL);
	test_intern(ns);
	test_parent(ns);
	test_trees(ns);
	test_verify_fails(ns);
	zr_names_destroy(ns);
	test_growth();
	printf("check_name: %d checks passed\n", checks);
	return (0);
}
