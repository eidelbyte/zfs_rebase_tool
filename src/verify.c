/*
 * verify: the classifier of verify.h. One pass over the manifest,
 * naming for each action the object it produces and the object onto
 * had at that name, and asking the content oracle which of the two
 * the result holds; then one pass over the whole name table, holding
 * every name nobody spoke for against the object onto had there --
 * which is absence where onto never had it, and absence again where
 * an rm took the directory above it away.
 *
 * The second pass is over the names of all three trees and not over
 * the result's alone: a name onto had that the result no longer holds
 * is exactly what a stray delete leaves, and a pass that only walked
 * what the result holds could never see it.
 *
 * From the conflicts gate on there is a third document, the
 * resolution, and a third pass: every name one of its lines covers is
 * spoken for by the choice on that line and not by the second pass,
 * and an onto or a from line is held against the object that side has
 * at the name, the way an action is held against the object it makes.
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

/* What the two documents said about one name, kept by name id. */
#define	ZV_ACTED	0x1	/* an action line names it */
#define	ZV_CONFLICT	0x2	/* and that line is a conflict mark */
#define	ZV_RMDIR	0x4	/* and that line removes a directory */
#define	ZV_CHOSEN	0x8	/* a resolution line names it */

/* How many entries the name list starts at and grows by. */
#define	ZV_DIFF_MIN	16

struct zv_ctx {
	const struct zr_parsed	*zc_m;
	const struct zr_resolution *zc_res;	/* NULL before conflicts */
	struct zr_oracle	*zc_o;
	const struct zr_walk	*zc_w[3];
	const struct zr_names	*zc_names;
	unsigned char		*zc_mark;	/* by name id */
	uint32_t		zc_nnames;
	unsigned char		*zc_pmark;	/* by result pool index */
	uint32_t		zc_npools;
	uint32_t		zc_dcap;	/* the name list's capacity */
	unsigned		zc_miss;	/* trees not there */
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
 * Is this one of the trees the caller could not walk? Only onto and
 * from can be. An action that would have to read one of them is
 * unchecked: not done, not pending and above all not drifted, since
 * drift is a statement about the result and this is a statement
 * about what there is left to compare it with.
 */
static int
zv_gone(const struct zv_ctx *c, int t)
{
	if (t == ZV_ONTO)
		return ((c->zc_miss & ZR_MISS_ONTO) != 0);
	if (t == ZV_FROM)
		return ((c->zc_miss & ZR_MISS_FROM) != 0);
	return (0);
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

/*
 * Is this name inside a directory the manifest marked one of these
 * ways? ZV_CONFLICT says a conflict covers everything under it, so
 * that nothing there is classified at all; ZV_RMDIR says the
 * manifest takes the directory away, so that what the expected tree
 * holds under it is nothing.
 */
static int
zv_under(const struct zv_ctx *c, zr_name_t nm, unsigned char what)
{
	zr_name_t up;

	for (up = zr_names_parent(c->zc_names, nm); up != ZR_NAME_NONE;
	    up = zr_names_parent(c->zc_names, up)) {
		if (up >= c->zc_nnames)
			break;
		if ((c->zc_mark[up] & what) != 0)
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
	if (zv_gone(c, ZV_ONTO) != 0) {
		*out = ZR_OC_UNCHECKED;
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
	if (zv_gone(c, ZV_ONTO) != 0) {
		*out = ZR_OC_UNCHECKED;
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
	if (zv_gone(c, src) != 0) {
		*out = ZR_OC_UNCHECKED;
		return (0);
	}
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
	if (zv_gone(c, ZV_ONTO) != 0) {
		*out = ZR_OC_UNCHECKED;
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

	if (zv_gone(c, ZV_FROM) != 0) {
		*out = ZR_OC_UNCHECKED;
		return (0);
	}
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
	if (zv_gone(c, ZV_ONTO) != 0) {
		*out = ZR_OC_UNCHECKED;
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
 * is what keeps the name list honest: the second name of a written
 * file did change, and the action on the first name is why, so it is
 * not a stray edit and nobody is told about it twice.
 */
static void
zv_marks(struct zv_ctx *c)
{
	const struct zr_action *a;
	const struct zr_rline *l;
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
		if (a->za_kind == ZR_ACT_RM && a->za_isdir != 0)
			c->zc_mark[nm] |= ZV_RMDIR;
		pr = zv_pool(c, ZV_RESULT, nm);
		if (pr != ZR_POOL_NONE && pr < c->zc_npools)
			c->zc_pmark[pr] = 1;
	}
	/*
	 * And every name a resolution line covers, which is the name
	 * itself and, where the line is a directory, everything under
	 * it: the manifest's own scoping, asked the same way. The
	 * result pool is not marked here as an action's is. A choice
	 * speaks for a name and not for an object, and a name that
	 * merely shares an object with a chosen one is nobody's
	 * choice: leaving it in the list is what lets a hand edit to
	 * it still be seen.
	 */
	if (c->zc_res == NULL)
		return;
	for (i = 0; i < c->zc_res->zs_nlines; i++) {
		l = &c->zc_res->zs_lines[i];
		nm = zv_name(c, l->zl_path, l->zl_pathlen);
		if (nm == ZR_NAME_NONE || nm >= c->zc_nnames)
			continue;
		c->zc_mark[nm] |= ZV_CHOSEN;
	}
}

/*
 * Is this name one the second pass may speak about at all? A name
 * some action names has its own outcome; a name a conflict covers is
 * nobody's to judge; a name a resolution line covers is spoken for by
 * that choice, which is the same exemption for the same reason; and a
 * name that shares a result pool with a name an action made is that
 * action's doing.
 */
static int
zv_untouched(const struct zv_ctx *c, zr_name_t nm)
{
	zr_pool_t pr;

	if ((c->zc_mark[nm] & (ZV_ACTED | ZV_CHOSEN)) != 0)
		return (0);
	if (zv_under(c, nm, ZV_CONFLICT | ZV_CHOSEN) != 0)
		return (0);
	pr = zv_pool(c, ZV_RESULT, nm);
	if (pr != ZR_POOL_NONE && pr < c->zc_npools && c->zc_pmark[pr] != 0)
		return (0);
	return (1);
}

/*
 * The name of onto's pool po that the result still holds as onto had
 * it, lowest id first, or ZR_NAME_NONE where there is none. It is the
 * name a repair links to rather than copying, so that putting one
 * name of a pool back does not sever it from the others. Returns 0
 * with *out set, or -1 with err set.
 */
static int
zv_anchor(struct zv_ctx *c, zr_pool_t po, zr_name_t *out)
{
	const struct zr_pool *p;
	zr_pool_t pr;
	zr_name_t n;
	uint32_t i;
	int eq;

	*out = ZR_NAME_NONE;
	if (po == ZR_POOL_NONE)
		return (0);
	p = &c->zc_w[ZV_ONTO]->zw_tree.zt_pools[po];
	for (i = 0; i < p->zp_nnames; i++) {
		n = p->zp_names[i];
		if (n >= c->zc_nnames || zv_untouched(c, n) == 0)
			continue;
		if (zv_under(c, n, ZV_RMDIR) != 0)
			continue;
		pr = zv_pool(c, ZV_RESULT, n);
		if (pr == ZR_POOL_NONE)
			continue;
		eq = zv_same(c, ZV_ONTO, po, ZV_RESULT, pr);
		if (eq < 0)
			return (-1);
		if (eq != 0) {
			*out = n;
			return (0);
		}
	}
	return (0);
}

/* One name onto the list, with the count and the first of its kind. */
static int
zv_add(struct zv_ctx *c, struct zr_verify_report *out, zr_name_t nm,
    enum zr_diff kind, zr_name_t anchor)
{
	struct zr_namediff *d;
	uint32_t cap;

	if (out->zv_ndiffs == c->zc_dcap) {
		cap = c->zc_dcap != 0 ? c->zc_dcap * 2 : ZV_DIFF_MIN;
		d = realloc(out->zv_diffs, (size_t)cap * sizeof (*d));
		if (d == NULL) {
			zv_failx(c->zc_err, c->zc_errlen,
			    "verify: out of memory");
			return (-1);
		}
		out->zv_diffs = d;
		c->zc_dcap = cap;
	}
	d = &out->zv_diffs[out->zv_ndiffs++];
	d->zn_name = nm;
	d->zn_anchor = anchor;
	d->zn_kind = kind;
	if (out->zv_dcount[kind] == 0)
		out->zv_dfirst[kind] = nm;
	out->zv_dcount[kind]++;
	return (0);
}

/*
 * The names nobody spoke for, over the whole shared table: the
 * expected object of each is onto's own, and nothing where onto never
 * had it or where an rm took the directory above it away. Four ways
 * to differ, and the name axis is asked before the content axis
 * because a name that is not there has no content to ask about.
 */
static int
zv_names(struct zv_ctx *c, struct zr_verify_report *out)
{
	zr_name_t nm, root, anchor;
	zr_pool_t po, pr;
	int eq;

	/*
	 * With onto gone there is nothing to hold the untouched names
	 * against: every one of them would have to be reported, and
	 * saying that of a whole tree says nothing at all.
	 */
	if (zv_gone(c, ZV_ONTO) != 0)
		return (0);
	/*
	 * The root is not one of the names. It is the tree itself,
	 * the manifest has no line that could speak of it -- the root
	 * line of the tree section carries no action -- and what it
	 * holds in the result is the dataset's own root, put there by
	 * the clone and not by anything a rebase did.
	 */
	root = zr_names_lookup(c->zc_names, "/", 1);
	for (nm = 0; nm < c->zc_nnames; nm++) {
		if (nm == root)
			continue;
		po = zv_pool(c, ZV_ONTO, nm);
		pr = zv_pool(c, ZV_RESULT, nm);
		if (po == ZR_POOL_NONE && pr == ZR_POOL_NONE)
			continue;
		if (zv_untouched(c, nm) == 0)
			continue;
		if (po == ZR_POOL_NONE || zv_under(c, nm, ZV_RMDIR) != 0) {
			/* nothing was expected here */
			if (pr != ZR_POOL_NONE &&
			    zv_add(c, out, nm, ZR_DF_EXTRA,
			    ZR_NAME_NONE) != 0)
				return (-1);
			continue;
		}
		if (pr == ZR_POOL_NONE) {
			if (zv_anchor(c, po, &anchor) != 0 ||
			    zv_add(c, out, nm, ZR_DF_GONE, anchor) != 0)
				return (-1);
			continue;
		}
		eq = zv_same(c, ZV_ONTO, po, ZV_RESULT, pr);
		if (eq < 0)
			return (-1);
		if (zv_anchor(c, po, &anchor) != 0)
			return (-1);
		if (eq == 0) {
			if (zv_add(c, out, nm, ZR_DF_CHANGED, anchor) != 0)
				return (-1);
			continue;
		}
		/*
		 * There and equal: the one thing left to ask is
		 * whether it is still one object with the names it
		 * shared onto's with. The anchor is the first of them
		 * the result holds, so a pool torn in two is one entry
		 * and not two.
		 */
		if (anchor != ZR_NAME_NONE && anchor != nm &&
		    zv_pool(c, ZV_RESULT, anchor) != pr &&
		    zv_add(c, out, nm, ZR_DF_UNPOOLED, anchor) != 0)
			return (-1);
	}
	return (0);
}

/*
 * Are the names of one conflict group that chose this side still one
 * object in the result, as that side holds them? The resolution says
 * "the names of one group that chose the same side pool as that side
 * pools them", so a name holding the right bytes outside that pool is
 * not what was chosen -- it is the tearing a dup makes, reported for
 * the operator to look at exactly as zv_kept_pool reports a write's.
 *
 * A drift line is nobody's group: it is one name a verify found
 * changed, and group 0 is not a group, so nothing is asked of it.
 * Absence has no pool either, and there is nothing to ask there.
 */
static int
zv_group_pooled(const struct zv_ctx *c, uint32_t li, int side, zr_pool_t ps,
    zr_pool_t pr)
{
	const struct zr_resolution *res = c->zc_res;
	const struct zr_rline *l = &res->zs_lines[li];
	const struct zr_rline *o;
	zr_name_t nm;
	uint32_t i;

	if (ps == ZR_POOL_NONE || l->zl_kind != ZR_RL_CONFLICT ||
	    l->zl_group == 0)
		return (1);
	for (i = 0; i < res->zs_nlines; i++) {
		o = &res->zs_lines[i];
		if (i == li || o->zl_kind != ZR_RL_CONFLICT ||
		    o->zl_group != l->zl_group ||
		    o->zl_choice != l->zl_choice)
			continue;
		nm = zv_name(c, o->zl_path, o->zl_pathlen);
		if (zv_pool(c, side, nm) != ps)
			continue;
		if (zv_pool(c, ZV_RESULT, nm) != pr)
			return (0);
	}
	return (1);
}

/*
 * One choice of onto or from. The expected object is that side's at
 * the name and absence where the side had none; the original is
 * onto's, as it is for an action. The side is asked first, so that a
 * name already holding what was chosen is done whatever became of
 * onto -- which is zv_make's own order -- and onto is asked only when
 * the answer was no.
 */
static int
zv_choice(struct zv_ctx *c, uint32_t li, zr_name_t nm, zr_pool_t po,
    zr_pool_t pr, enum zr_outcome *out)
{
	const struct zr_rline *l = &c->zc_res->zs_lines[li];
	zr_pool_t ps;
	int side, eq;

	side = l->zl_choice == ZR_CH_FROM ? ZV_FROM : ZV_ONTO;
	if (zv_gone(c, side) != 0) {
		*out = ZR_OC_UNCHECKED;
		return (0);
	}
	ps = zv_pool(c, side, nm);
	eq = zv_same(c, side, ps, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	if (eq != 0) {
		*out = zv_group_pooled(c, li, side, ps, pr) != 0 ?
		    ZR_OC_DONE : ZR_OC_DRIFTED;
		return (0);
	}
	if (zv_gone(c, ZV_ONTO) != 0) {
		*out = ZR_OC_UNCHECKED;
		return (0);
	}
	eq = zv_same(c, ZV_ONTO, po, ZV_RESULT, pr);
	if (eq < 0)
		return (-1);
	*out = eq != 0 ? ZR_OC_PENDING : ZR_OC_DRIFTED;
	return (0);
}

/*
 * The resolution, line by line and in its own order. keep is never
 * compared and "-" is a conflict nobody has answered: both are given
 * done, which is the outcome of a line there is nothing to do about,
 * and counted nowhere -- the counts are the onto and the from lines,
 * which are the only ones that can be held against anything.
 */
static int
zv_lines(struct zv_ctx *c, struct zr_verify_report *out)
{
	const struct zr_rline *l;
	enum zr_outcome oc;
	zr_pool_t po, pr;
	zr_name_t nm;
	uint32_t i;

	for (i = 0; i < c->zc_res->zs_nlines; i++) {
		l = &c->zc_res->zs_lines[i];
		out->zv_rline[i] = ZR_OC_DONE;
		if (l->zl_choice != ZR_CH_ONTO && l->zl_choice != ZR_CH_FROM)
			continue;
		nm = zv_name(c, l->zl_path, l->zl_pathlen);
		po = zv_pool(c, ZV_ONTO, nm);
		pr = zv_pool(c, ZV_RESULT, nm);
		if (zv_choice(c, i, nm, po, pr, &oc) != 0)
			return (-1);
		out->zv_rline[i] = oc;
		if (out->zv_rcount[oc] == 0)
			out->zv_rfirst[oc] = i;
		out->zv_rcount[oc]++;
	}
	return (0);
}

int
zr_verify_with(const struct zr_parsed *m, const struct zr_resolution *res,
    struct zr_oracle *o, const struct zr_walk *onto,
    const struct zr_walk *from, const struct zr_walk *result,
    unsigned missing, struct zr_verify_report *out, char *err, size_t errlen)
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
	for (i = 0; i < ZR_OC_COUNT; i++) {
		out->zv_first[i] = ZR_ACTION_NONE;
		out->zv_rfirst[i] = ZR_ACTION_NONE;
	}
	for (i = 0; i < ZR_DF_COUNT; i++)
		out->zv_dfirst[i] = ZR_NAME_NONE;
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (m == NULL || o == NULL || onto == NULL || from == NULL ||
	    result == NULL) {
		zv_failx(err, errlen, "verify: arguments");
		return (-1);
	}
	memset(&c, 0, sizeof (struct zv_ctx));
	c.zc_m = m;
	c.zc_res = res;
	c.zc_o = o;
	c.zc_w[ZV_ONTO] = onto;
	c.zc_w[ZV_FROM] = from;
	c.zc_w[ZV_RESULT] = result;
	c.zc_miss = missing;
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
	out->zv_nrlines = res != NULL ? res->zs_nlines : 0;
	if (out->zv_nrlines != 0) {
		out->zv_rline = malloc((size_t)out->zv_nrlines *
		    sizeof (enum zr_outcome));
		if (out->zv_rline == NULL) {
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
	if (out->zv_nrlines != 0 && zv_lines(&c, out) != 0) {
		rc = -1;
		goto done;
	}
	rc = zv_names(&c, out);
done:
	free(c.zc_pmark);
	free(c.zc_mark);
	return (rc);
}

int
zr_verify(const struct zr_parsed *m, struct zr_oracle *o,
    const struct zr_walk *onto, const struct zr_walk *from,
    const struct zr_walk *result, unsigned missing,
    struct zr_verify_report *out, char *err, size_t errlen)
{
	return (zr_verify_with(m, NULL, o, onto, from, result, missing, out,
	    err, errlen));
}

void
zr_verify_report_fini(struct zr_verify_report *r)
{
	int i;

	if (r == NULL)
		return;
	free(r->zv_outcome);
	free(r->zv_diffs);
	free(r->zv_rline);
	memset(r, 0, sizeof (struct zr_verify_report));
	for (i = 0; i < ZR_OC_COUNT; i++) {
		r->zv_first[i] = ZR_ACTION_NONE;
		r->zv_rfirst[i] = ZR_ACTION_NONE;
	}
	for (i = 0; i < ZR_DF_COUNT; i++)
		r->zv_dfirst[i] = ZR_NAME_NONE;
}

const char *
zr_diff_str(enum zr_diff df)
{
	switch (df) {
	case ZR_DF_GONE:
		return ("gone");
	case ZR_DF_EXTRA:
		return ("extra");
	case ZR_DF_CHANGED:
		return ("changed");
	case ZR_DF_UNPOOLED:
		return ("unpooled");
	}
	return ("unknown");
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
	case ZR_OC_UNCHECKED:
		return ("unchecked");
	}
	return ("unknown");
}
