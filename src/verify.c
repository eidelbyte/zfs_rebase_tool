/*
 * verify: the classifier of verify.h. One pass over the manifest,
 * naming for each action the object it produces and the object onto
 * had at that name, and asking the content oracle which of the two
 * the result holds; then one pass over the names nobody spoke for,
 * which should still be onto's and are an information line when they
 * are not.
 *
 * The trees are the oracle's three positions: onto, from, result.
 * Nothing here opens a file itself -- every read is the oracle's, and
 * so is every memo of a pair already settled.
 */

#define	_XOPEN_SOURCE	700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "manifest.h"
#include "name.h"
#include "verify.h"
#include "walk.h"
#include "yellow.h"

/* The three walks, at the positions the oracle indexes them by. */
#define	ZV_ONTO		0
#define	ZV_FROM		1
#define	ZV_RESULT	2

/* What the manifest said about one name, kept by name id. */
#define	ZV_ACTED	0x1	/* an action line names it */
#define	ZV_CONFLICT	0x2	/* and that line is a conflict mark */

struct zv_ctx {
	const struct zr_parsed	*zc_m;
	struct zr_oracle	*zc_o;
	const struct zr_walk	*zc_w[3];
	const struct zr_names	*zc_names;
	unsigned char		*zc_mark;	/* by name id */
	uint32_t		zc_nnames;
	unsigned char		*zc_pmark;	/* by result pool index */
	uint32_t		zc_npools;
	char			*zc_err;
	size_t			zc_errlen;
};

static void
zv_failp(struct zv_ctx *c, const unsigned char *path, const char *what)
{
	if (c->zc_err == NULL || c->zc_errlen == 0)
		return;
	(void) snprintf(c->zc_err, c->zc_errlen, "%s: %s",
	    (const char *)path, what);
}

static void
zv_failx(char *err, size_t errlen, const char *what)
{
	if (err == NULL || errlen == 0)
		return;
	(void) snprintf(err, errlen, "%s", what);
}

static zr_name_t
zv_name(const struct zv_ctx *c, const unsigned char *path, size_t len)
{
	return (zr_names_lookup(c->zc_names, (const char *)path, len));
}

/* The pool one name has in one of the three trees, or ZR_POOL_NONE. */
static zr_pool_t
zv_pool(const struct zv_ctx *c, int t, zr_name_t nm)
{
	return (zr_tree_pool(&c->zc_w[t]->zw_tree, nm));
}

/*
 * Do two pools hold the same object? Absent on both sides is the
 * same, absent on one is not. Returns 1, 0, or -1 with err set.
 */
static int
zv_same(struct zv_ctx *c, int ta, zr_pool_t pa, int tb, zr_pool_t pb)
{
	if (pa == ZR_POOL_NONE || pb == ZR_POOL_NONE)
		return (pa == pb ? 1 : 0);
	return (zr_oracle_equal(c->zc_o, ta, pa, tb, pb, c->zc_err,
	    c->zc_errlen));
}

/* Is any name of the manifest's conflict marks inside this directory? */
int
zr_verify_blocked(const struct zr_parsed *m, const unsigned char *dir,
    size_t dirlen)
{
	const struct zr_action *a;
	uint32_t i;

	if (m == NULL || dir == NULL || dirlen == 0)
		return (0);
	for (i = 0; i < m->zp_nactions; i++) {
		a = &m->zp_actions[i];
		if (a->za_kind != ZR_ACT_CONFLICT)
			continue;
		if (a->za_pathlen > dirlen && a->za_path[dirlen] == '/' &&
		    memcmp(a->za_path, dir, dirlen) == 0)
			return (1);
	}
	return (0);
}

/* Is this name inside a directory the manifest marked conflict? */
static int
zv_conflicted(const struct zv_ctx *c, zr_name_t nm)
{
	zr_name_t up;

	for (up = zr_names_parent(c->zc_names, nm); up != ZR_NAME_NONE;
	    up = zr_names_parent(c->zc_names, up)) {
		if (up >= c->zc_nnames)
			break;
		if ((c->zc_mark[up] & ZV_CONFLICT) != 0)
			return (1);
	}
	return (0);
}

/*
 * rm: what it produces is absence, so the name gone is done. A
 * directory still there with a conflicted name under it is the one
 * removal that cannot be made, which is blocked and not drift.
 */
static int
zv_rm(struct zv_ctx *c, const struct zr_action *a, zr_pool_t po, zr_pool_t pr,
    enum zr_outcome *out)
{
	int eq;

	if (pr == ZR_POOL_NONE) {
		*out = ZR_OC_DONE;
		return (0);
	}
	if (a->za_isdir != 0 &&
	    zr_verify_blocked(c->zc_m, a->za_path, a->za_pathlen) != 0) {
		*out = ZR_OC_BLOCKED;
		return (0);
	}
	eq = zv_same(c, ZV_ONTO, po, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	*out = eq != 0 ? ZR_OC_PENDING : ZR_OC_DRIFTED;
	return (0);
}

/*
 * ln: what it produces is one object under two names, so the
 * question is pooling and not content, and it is asked of the result
 * walk alone. A name that is not there yet, or that still holds what
 * onto put there, is pending; anything else is drift.
 */
static int
zv_ln(struct zv_ctx *c, const struct zr_action *a, zr_pool_t po, zr_pool_t pr,
    enum zr_outcome *out)
{
	zr_pool_t pa;
	int eq;

	pa = zv_pool(c, ZV_RESULT, zv_name(c, a->za_arg, a->za_arglen));
	if (pr != ZR_POOL_NONE && pr == pa) {
		*out = ZR_OC_DONE;
		return (0);
	}
	if (pr == ZR_POOL_NONE) {
		*out = ZR_OC_PENDING;
		return (0);
	}
	eq = zv_same(c, ZV_ONTO, po, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	*out = eq != 0 ? ZR_OC_PENDING : ZR_OC_DRIFTED;
	return (0);
}

/*
 * cp and dup: a new object at the name, out of the from tree for a
 * cp and out of the onto tree itself for a dup. dup severs, so an
 * object equal to the anchor's because it is still the anchor's own
 * file has not been made yet: that is the pending state, and the
 * result walk is where it shows.
 */
static int
zv_make(struct zv_ctx *c, const struct zr_action *a, zr_pool_t po,
    zr_pool_t pr, enum zr_outcome *out)
{
	zr_name_t an;
	zr_pool_t ps, pa;
	int src, eq;

	src = a->za_kind == ZR_ACT_CP ? ZV_FROM : ZV_ONTO;
	an = zv_name(c, a->za_arg, a->za_arglen);
	ps = zv_pool(c, src, an);
	if (ps == ZR_POOL_NONE) {
		zv_failp(c, a->za_arg, "the tree the manifest copies from "
		    "holds no such path");
		return (-1);
	}
	eq = zv_same(c, src, ps, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	if (eq != 0 && a->za_kind == ZR_ACT_DUP) {
		pa = zv_pool(c, ZV_RESULT, an);
		if (pa != ZR_POOL_NONE && pa == pr)
			eq = 0;
	}
	if (eq != 0) {
		*out = ZR_OC_DONE;
		return (0);
	}
	eq = zv_same(c, ZV_ONTO, po, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	*out = eq != 0 ? ZR_OC_PENDING : ZR_OC_DRIFTED;
	return (0);
}

/*
 * A write keeps the object, so every other name onto gave that
 * object sees the new bytes through it. A name of onto's pool that
 * the manifest says nothing about -- no rm, no ln away, no dup
 * severing it, no conflict mark -- and that the result does not hold
 * in this same pool is a torn pool: neither the manifest's outcome
 * nor onto's own, which is drift. Note that a re-write mends the
 * bytes and not the tearing; what is reported here is for the
 * operator to look at.
 */
static int
zv_kept_pool(const struct zv_ctx *c, zr_pool_t po, zr_pool_t pr)
{
	const struct zr_pool *p;
	zr_name_t n;
	uint32_t i;

	if (po == ZR_POOL_NONE)
		return (1);
	p = &c->zc_w[ZV_ONTO]->zw_tree.zt_pools[po];
	for (i = 0; i < p->zp_nnames; i++) {
		n = p->zp_names[i];
		if (n < c->zc_nnames && (c->zc_mark[n] & ZV_ACTED) != 0)
			continue;
		if (zv_pool(c, ZV_RESULT, n) != pr)
			return (0);
	}
	return (1);
}

static int
zv_write(struct zv_ctx *c, const struct zr_action *a, zr_pool_t po,
    zr_pool_t pr, enum zr_outcome *out)
{
	zr_pool_t ps;
	int eq;

	ps = zv_pool(c, ZV_FROM, zv_name(c, a->za_arg, a->za_arglen));
	if (ps == ZR_POOL_NONE) {
		zv_failp(c, a->za_arg, "the tree the manifest writes from "
		    "holds no such path");
		return (-1);
	}
	eq = zv_same(c, ZV_FROM, ps, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	if (eq != 0 && zv_kept_pool(c, po, pr) != 0) {
		*out = ZR_OC_DONE;
		return (0);
	}
	eq = zv_same(c, ZV_ONTO, po, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	*out = eq != 0 ? ZR_OC_PENDING : ZR_OC_DRIFTED;
	return (0);
}

/*
 * Every name of the manifest, and every pool of the result one of
 * them reaches, marked before anything is classified. The pool mark
 * is what keeps the information lines honest: the second name of a
 * written file did change, and the action on the first name is why,
 * so it is not a stray edit and nobody is told about it twice.
 */
static void
zv_marks(struct zv_ctx *c)
{
	const struct zr_action *a;
	zr_pool_t pr;
	zr_name_t nm;
	uint32_t i;

	for (i = 0; i < c->zc_m->zp_nactions; i++) {
		a = &c->zc_m->zp_actions[i];
		nm = zv_name(c, a->za_path, a->za_pathlen);
		if (nm == ZR_NAME_NONE || nm >= c->zc_nnames)
			continue;
		c->zc_mark[nm] |= ZV_ACTED;
		if (a->za_kind == ZR_ACT_CONFLICT)
			c->zc_mark[nm] |= ZV_CONFLICT;
		pr = zv_pool(c, ZV_RESULT, nm);
		if (pr != ZR_POOL_NONE && pr < c->zc_npools)
			c->zc_pmark[pr] = 1;
	}
}

/* The names nobody spoke for, which should be onto's still. */
static int
zv_info(struct zv_ctx *c, struct zr_verify_report *out)
{
	zr_pool_t pr;
	zr_name_t nm;
	int eq;

	for (nm = 0; nm < c->zc_nnames; nm++) {
		pr = zv_pool(c, ZV_RESULT, nm);
		if (pr == ZR_POOL_NONE || (c->zc_mark[nm] & ZV_ACTED) != 0)
			continue;
		if (pr < c->zc_npools && c->zc_pmark[pr] != 0)
			continue;
		if (zv_conflicted(c, nm) != 0)
			continue;
		eq = zv_same(c, ZV_ONTO, zv_pool(c, ZV_ONTO, nm), ZV_RESULT,
		    pr);
		if (eq < 0)
			return (-1);
		if (eq != 0)
			continue;
		if (out->zv_ninfo == 0)
			out->zv_first_info = nm;
		out->zv_ninfo++;
	}
	return (0);
}

int
zr_verify(const struct zr_parsed *m, struct zr_oracle *o,
    const struct zr_walk *onto, const struct zr_walk *from,
    const struct zr_walk *result, struct zr_verify_report *out, char *err,
    size_t errlen)
{
	const struct zr_action *a;
	enum zr_outcome oc = ZR_OC_DONE;
	struct zv_ctx c;
	zr_pool_t po, pr;
	zr_name_t nm;
	uint32_t i;
	int rc = -1;

	if (out == NULL)
		return (-1);
	memset(out, 0, sizeof (struct zr_verify_report));
	out->zv_first_info = ZR_NAME_NONE;
	for (i = 0; i < ZR_OC_COUNT; i++)
		out->zv_first[i] = ZR_ACTION_NONE;
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (m == NULL || o == NULL || onto == NULL || from == NULL ||
	    result == NULL) {
		zv_failx(err, errlen, "verify: arguments");
		return (-1);
	}
	memset(&c, 0, sizeof (struct zv_ctx));
	c.zc_m = m;
	c.zc_o = o;
	c.zc_w[ZV_ONTO] = onto;
	c.zc_w[ZV_FROM] = from;
	c.zc_w[ZV_RESULT] = result;
	c.zc_err = err;
	c.zc_errlen = errlen;
	c.zc_names = onto->zw_tree.zt_names;
	if (c.zc_names == NULL ||
	    c.zc_names != from->zw_tree.zt_names ||
	    c.zc_names != result->zw_tree.zt_names) {
		zv_failx(err, errlen, "verify: the three trees do not share "
		    "one name table");
		return (-1);
	}
	c.zc_nnames = zr_names_count(c.zc_names);
	if (c.zc_nnames != 0) {
		c.zc_mark = calloc((size_t)c.zc_nnames, 1);
		if (c.zc_mark == NULL) {
			zv_failx(err, errlen, "verify: out of memory");
			return (-1);
		}
	}
	c.zc_npools = result->zw_tree.zt_npools;
	if (c.zc_npools != 0) {
		c.zc_pmark = calloc((size_t)c.zc_npools, 1);
		if (c.zc_pmark == NULL) {
			zv_failx(err, errlen, "verify: out of memory");
			goto done;
		}
	}
	out->zv_nactions = m->zp_nactions;
	if (m->zp_nactions != 0) {
		out->zv_outcome = malloc((size_t)m->zp_nactions *
		    sizeof (enum zr_outcome));
		if (out->zv_outcome == NULL) {
			zv_failx(err, errlen, "verify: out of memory");
			goto done;
		}
	}
	zv_marks(&c);
	for (i = 0; i < m->zp_nactions; i++) {
		a = &m->zp_actions[i];
		if (a->za_kind == ZR_ACT_CONFLICT) {
			out->zv_outcome[i] = ZR_OC_DONE;
			continue;
		}
		nm = zv_name(&c, a->za_path, a->za_pathlen);
		po = zv_pool(&c, ZV_ONTO, nm);
		pr = zv_pool(&c, ZV_RESULT, nm);
		switch (a->za_kind) {
		case ZR_ACT_RM:
			rc = zv_rm(&c, a, po, pr, &oc);
			break;
		case ZR_ACT_LN:
			rc = zv_ln(&c, a, po, pr, &oc);
			break;
		case ZR_ACT_CP:
		case ZR_ACT_DUP:
			rc = zv_make(&c, a, po, pr, &oc);
			break;
		default:
			rc = zv_write(&c, a, po, pr, &oc);
			break;
		}
		if (rc != 0) {
			rc = -1;
			goto done;
		}
		out->zv_outcome[i] = oc;
		if (out->zv_count[oc] == 0)
			out->zv_first[oc] = i;
		out->zv_count[oc]++;
	}
	rc = zv_info(&c, out);
done:
	free(c.zc_pmark);
	free(c.zc_mark);
	return (rc);
}

void
zr_verify_report_fini(struct zr_verify_report *r)
{
	if (r == NULL)
		return;
	free(r->zv_outcome);
	memset(r, 0, sizeof (struct zr_verify_report));
	r->zv_first_info = ZR_NAME_NONE;
}

const char *
zr_outcome_str(enum zr_outcome oc)
{
	switch (oc) {
	case ZR_OC_DONE:
		return ("done");
	case ZR_OC_PENDING:
		return ("pending");
	case ZR_OC_BLOCKED:
		return ("blocked");
	case ZR_OC_DRIFTED:
		return ("drifted");
	}
	return ("unknown");
}
