/*
 * zr_decide: the v4 decision. See decide.h for the contract and the
 * theory notes for the rules; every step below names the sentence
 * of the note it implements.
 */

#include <stdlib.h>
#include <string.h>

#include "decide.h"

/* Union-find over name ids. */
static zr_name_t
uf_find(zr_name_t *parent, zr_name_t n)
{
	zr_name_t root = n;

	while (parent[root] != root)
		root = parent[root];
	while (parent[n] != root) {
		zr_name_t next = parent[n];
		parent[n] = root;
		n = next;
	}
	return (root);
}

static void
uf_union(zr_name_t *parent, zr_name_t a, zr_name_t b)
{
	a = uf_find(parent, a);
	b = uf_find(parent, b);
	if (a != b)
		parent[b < a ? a : b] = (b < a ? b : a);
}

/* A small open-addressing set of uint64 keys, for base-pool pairs. */
struct u64set {
	uint64_t	*keys;
	uint32_t	cap;
	uint32_t	n;
};

#define	U64_EMPTY	((uint64_t)-1)

static int
u64set_init(struct u64set *s, uint32_t hint)
{
	uint32_t i;

	s->cap = 16;
	while (s->cap < hint * 2)
		s->cap *= 2;
	s->n = 0;
	s->keys = malloc((size_t)s->cap * sizeof (uint64_t));
	if (s->keys == NULL)
		return (-1);
	for (i = 0; i < s->cap; i++)
		s->keys[i] = U64_EMPTY;
	return (0);
}

static uint32_t
u64_hash(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	return ((uint32_t)k);
}

static int u64set_add(struct u64set *s, uint64_t k);

static int
u64set_grow(struct u64set *s)
{
	struct u64set old = *s;
	uint32_t i;

	if (u64set_init(s, old.cap) != 0) {
		*s = old;
		return (-1);
	}
	for (i = 0; i < old.cap; i++)
		if (old.keys[i] != U64_EMPTY)
			(void) u64set_add(s, old.keys[i]);
	free(old.keys);
	return (0);
}

static int
u64set_add(struct u64set *s, uint64_t k)
{
	uint32_t i;

	if (s->n * 2 >= s->cap && u64set_grow(s) != 0)
		return (-1);
	i = u64_hash(k) & (s->cap - 1);
	while (s->keys[i] != U64_EMPTY) {
		if (s->keys[i] == k)
			return (0);
		i = (i + 1) & (s->cap - 1);
	}
	s->keys[i] = k;
	s->n++;
	return (0);
}

static int
u64set_has(const struct u64set *s, uint64_t k)
{
	uint32_t i = u64_hash(k) & (s->cap - 1);

	while (s->keys[i] != U64_EMPTY) {
		if (s->keys[i] == k)
			return (1);
		i = (i + 1) & (s->cap - 1);
	}
	return (0);
}

static void
u64set_fini(struct u64set *s)
{
	free(s->keys);
	s->keys = NULL;
}

static uint64_t
pair_key(zr_pool_t a, zr_pool_t b)
{
	if (b < a) {
		zr_pool_t t = a;
		a = b;
		b = t;
	}
	return (((uint64_t)a << 32) | b);
}

/* Everything the passes share. */
struct ctx {
	const struct zr_tree	*t[3];		/* base, from, onto */
	zr_mode_t		mode;
	uint32_t		nnames;
	uint8_t			*state;
	zr_name_t		*parent;	/* union-find, result classes */
	uint32_t		*klass;		/* name -> result class index */
	uint32_t		nclass;
	uint32_t		*pnode;		/* union-find over side pools */
	uint32_t		npnode;		/* from pools + onto pools */
	uint32_t		*gindex;	/* pool node root -> group */
	struct zr_decision	*out;
};

#define	T_BASE	0
#define	T_FROM	1
#define	T_ONTO	2

static int
settled(const struct ctx *c, zr_name_t n)
{
	return ((c->state[n] & (ZR_NS_SURVIVES | ZR_NS_CONTESTED)) ==
	    ZR_NS_SURVIVES);
}

static int
inbase(const struct ctx *c, zr_name_t n)
{
	return ((c->state[n] & ZR_NS_BASE) != 0);
}

/* Side pool node id: from pools first, then onto pools. */
static uint32_t
pnode_id(const struct ctx *c, int side, zr_pool_t q)
{
	return (side == T_FROM ? q : c->t[T_FROM]->zt_npools + q);
}

static uint32_t
pnode_find(struct ctx *c, uint32_t x)
{
	uint32_t root = x;

	while (c->pnode[root] != root)
		root = c->pnode[root];
	while (c->pnode[x] != root) {
		uint32_t next = c->pnode[x];
		c->pnode[x] = root;
		x = next;
	}
	return (root);
}

static void
pnode_union(struct ctx *c, uint32_t a, uint32_t b)
{
	a = pnode_find(c, a);
	b = pnode_find(c, b);
	if (a != b)
		c->pnode[b < a ? a : b] = (b < a ? b : a);
}

/* The group a surviving name belongs to, through any side pool of it. */
static uint32_t
group_of_name(struct ctx *c, zr_name_t n)
{
	zr_pool_t q;

	q = zr_tree_pool(c->t[T_FROM], n);
	if (q != ZR_POOL_NONE)
		return (c->gindex[pnode_find(c, pnode_id(c, T_FROM, q))]);
	q = zr_tree_pool(c->t[T_ONTO], n);
	return (c->gindex[pnode_find(c, pnode_id(c, T_ONTO, q))]);
}

static void
flag(struct ctx *c, zr_name_t n, uint32_t f, zr_name_t why0, zr_name_t why1)
{
	struct zr_group *g = &c->out->zd_groups[group_of_name(c, n)];

	if (g->zg_flags == 0) {
		g->zg_why[0] = why0;
		g->zg_why[1] = why1;
	}
	g->zg_flags |= f;
}

/*
 * Names. "A name survives iff both sides hold it, or exactly one side
 * holds it and base does not. A name both sides invented is
 * contested."
 */
static void
pass_names(struct ctx *c)
{
	zr_name_t n;

	for (n = 0; n < c->nnames; n++) {
		int b = zr_tree_pool(c->t[T_BASE], n) != ZR_POOL_NONE;
		int f = zr_tree_pool(c->t[T_FROM], n) != ZR_POOL_NONE;
		int o = zr_tree_pool(c->t[T_ONTO], n) != ZR_POOL_NONE;
		uint8_t s = 0;

		if (b)
			s |= ZR_NS_BASE;
		if (f)
			s |= ZR_NS_FROM;
		if (o)
			s |= ZR_NS_ONTO;
		if ((b && f && o) || (f && !b) || (o && !b))
			s |= ZR_NS_SURVIVES;
		if (f && o && !b)
			s |= ZR_NS_CONTESTED;
		c->state[n] = s;
		c->parent[n] = n;
	}
}

/*
 * Pools, step 1: "A pool that fans in or has an emerging name on its
 * base face stays one node." Judged on settled names.
 */
static void
pass_lumps(struct ctx *c)
{
	int side;

	for (side = T_FROM; side <= T_ONTO; side++) {
		const struct zr_tree *t = c->t[side];
		zr_pool_t qi;

		for (qi = 0; qi < t->zt_npools; qi++) {
			const struct zr_pool *q = &t->zt_pools[qi];
			zr_pool_t origin = ZR_POOL_NONE;
			int norigin = 0, hasnew = 0;
			uint32_t i;

			for (i = 0; i < q->zp_nnames; i++) {
				zr_name_t n = q->zp_names[i];
				zr_pool_t p;

				if (!settled(c, n))
					continue;
				if (!inbase(c, n)) {
					hasnew = 1;
					continue;
				}
				p = zr_tree_pool(c->t[T_BASE], n);
				if (norigin == 0) {
					origin = p;
					norigin = 1;
				} else if (p != origin) {
					norigin = 2;
				}
			}
			if (norigin < 2 && !hasnew)
				continue;
			for (i = 1; i < q->zp_nnames; i++) {
				zr_name_t n = q->zp_names[i];
				zr_name_t first = ZR_NAME_NONE;
				uint32_t j;

				if (!settled(c, n))
					continue;
				for (j = 0; j < i; j++) {
					if (settled(c, q->zp_names[j])) {
						first = q->zp_names[j];
						break;
					}
				}
				if (first != ZR_NAME_NONE)
					uf_union(c->parent, first, n);
			}
		}
	}
}

struct label_ent {
	zr_pool_t	f, o;
	zr_name_t	n;
};

static int
label_cmp(const void *a, const void *b)
{
	const struct label_ent *x = a, *y = b;

	if (x->f != y->f)
		return (x->f < y->f ? -1 : 1);
	if (x->o != y->o)
		return (x->o < y->o ? -1 : 1);
	return (x->n < y->n ? -1 : (x->n > y->n));
}

/*
 * Pools, step 2: "A pool that sits inside one base pool is replaced by
 * its labels on the working face." A label is the names one from pool
 * shares with one onto pool; such names are together in the result.
 */
static int
pass_labels(struct ctx *c)
{
	struct label_ent *ents;
	uint32_t n, cnt = 0, i;

	ents = malloc((size_t)c->nnames * sizeof (*ents) + 1);
	if (ents == NULL)
		return (-1);
	for (n = 0; n < c->nnames; n++) {
		if (!settled(c, n))
			continue;
		if ((c->state[n] & (ZR_NS_FROM | ZR_NS_ONTO)) !=
		    (ZR_NS_FROM | ZR_NS_ONTO))
			continue;
		ents[cnt].f = zr_tree_pool(c->t[T_FROM], n);
		ents[cnt].o = zr_tree_pool(c->t[T_ONTO], n);
		ents[cnt].n = n;
		cnt++;
	}
	qsort(ents, cnt, sizeof (*ents), label_cmp);
	for (i = 1; i < cnt; i++)
		if (ents[i].f == ents[i - 1].f && ents[i].o == ents[i - 1].o)
			uf_union(c->parent, ents[i - 1].n, ents[i].n);
	free(ents);
	return (0);
}

/* Number the result classes; contested names are placed later. */
static int
pass_classes(struct ctx *c)
{
	zr_name_t n;

	c->nclass = 0;
	for (n = 0; n < c->nnames; n++)
		c->klass[n] = (uint32_t)-1;
	for (n = 0; n < c->nnames; n++) {
		zr_name_t root;

		if (!settled(c, n))
			continue;
		root = uf_find(c->parent, n);
		if (c->klass[root] == (uint32_t)-1)
			c->klass[root] = c->nclass++;
		c->klass[n] = c->klass[root];
	}
	return (0);
}

/*
 * Groups: face local groups of the working face. Two side pools are
 * connected when they share a surviving name; base pools are listed
 * with the group their surviving names' side pools fall in.
 */
static int
pass_groups(struct ctx *c)
{
	uint32_t i, ngroups = 0;
	zr_name_t n;

	c->npnode = c->t[T_FROM]->zt_npools + c->t[T_ONTO]->zt_npools;
	for (i = 0; i < c->npnode; i++)
		c->pnode[i] = i;
	for (n = 0; n < c->nnames; n++) {
		if ((c->state[n] & ZR_NS_SURVIVES) == 0)
			continue;
		if ((c->state[n] & (ZR_NS_FROM | ZR_NS_ONTO)) ==
		    (ZR_NS_FROM | ZR_NS_ONTO))
			pnode_union(c,
			    pnode_id(c, T_FROM, zr_tree_pool(c->t[T_FROM], n)),
			    pnode_id(c, T_ONTO, zr_tree_pool(c->t[T_ONTO], n)));
	}
	for (i = 0; i < c->npnode; i++)
		c->gindex[i] = (uint32_t)-1;
	for (i = 0; i < c->npnode; i++) {
		uint32_t r = pnode_find(c, i);
		if (c->gindex[r] == (uint32_t)-1)
			c->gindex[r] = ngroups++;
	}
	c->out->zd_groups = calloc(ngroups ? ngroups : 1,
	    sizeof (struct zr_group));
	if (c->out->zd_groups == NULL)
		return (-1);
	c->out->zd_ngroups = ngroups;
	for (i = 0; i < ngroups; i++) {
		c->out->zd_groups[i].zg_why[0] = ZR_NAME_NONE;
		c->out->zd_groups[i].zg_why[1] = ZR_NAME_NONE;
	}
	return (0);
}

/*
 * Conflict: "a base pool that fans out on one side's base face, two of
 * whose edges end up in the same face local group of step 3."
 */
static void
pass_healed(struct ctx *c)
{
	const struct zr_tree *base = c->t[T_BASE];
	zr_pool_t pi;
	int side;

	for (pi = 0; pi < base->zt_npools; pi++) {
		const struct zr_pool *p = &base->zt_pools[pi];

		for (side = T_FROM; side <= T_ONTO; side++) {
			uint32_t i, j;

			for (i = 0; i < p->zp_nnames; i++) {
				zr_name_t a = p->zp_names[i];
				zr_pool_t qa;

				if (!settled(c, a))
					continue;
				qa = zr_tree_pool(c->t[side], a);
				for (j = i + 1; j < p->zp_nnames; j++) {
					zr_name_t b = p->zp_names[j];

					if (!settled(c, b))
						continue;
					if (zr_tree_pool(c->t[side], b) == qa)
						continue;
					if (c->klass[a] == c->klass[b]) {
						flag(c, a, ZR_CF_HEALED_SPLIT,
						    a, b);
						return;
					}
				}
			}
		}
	}
}

/*
 * Conflict: "a side pool with an emerging name and with base names,
 * every one of which is dying on the other side's base face."
 */
static void
pass_orphaned(struct ctx *c)
{
	int side;

	for (side = T_FROM; side <= T_ONTO; side++) {
		const struct zr_tree *t = c->t[side];
		zr_pool_t qi;

		for (qi = 0; qi < t->zt_npools; qi++) {
			const struct zr_pool *q = &t->zt_pools[qi];
			zr_name_t added = ZR_NAME_NONE;
			int hasbase = 0, basealive = 0;
			uint32_t i;

			for (i = 0; i < q->zp_nnames; i++) {
				zr_name_t n = q->zp_names[i];

				if (inbase(c, n)) {
					hasbase = 1;
					if (c->state[n] & ZR_NS_SURVIVES)
						basealive = 1;
				} else if (settled(c, n) &&
				    added == ZR_NAME_NONE) {
					added = n;
				}
			}
			if (added != ZR_NAME_NONE && hasbase && !basealive)
				flag(c, added, ZR_CF_ORPHANED_ADD, added,
				    ZR_NAME_NONE);
		}
	}
}

/* The settled class a side pool's names fall in: -1 none, -2 several. */
static long
pool_class(const struct ctx *c, const struct zr_pool *q)
{
	long k = -1;
	uint32_t i;

	for (i = 0; i < q->zp_nnames; i++) {
		zr_name_t n = q->zp_names[i];

		if (!settled(c, n))
			continue;
		if (k == -1)
			k = c->klass[n];
		else if (k != (long)c->klass[n])
			return (-2);
	}
	return (k);
}

/* Contested co-members of x in q, as a sorted id list; count returned. */
static uint32_t
comembers(const struct ctx *c, const struct zr_pool *q, zr_name_t x,
    zr_name_t *buf)
{
	uint32_t i, cnt = 0;

	for (i = 0; i < q->zp_nnames; i++) {
		zr_name_t n = q->zp_names[i];

		if (n != x && (c->state[n] & ZR_NS_CONTESTED))
			buf[cnt++] = n;
	}
	return (cnt);
}

/*
 * Conflict: "a contested name whose from-pool and onto-pool land in
 * different groups, or in different sets of contested co-members, or
 * whose pool-mates on one side span two groups." Then placement.
 */
static int
pass_contested(struct ctx *c)
{
	zr_name_t *bf, *bo, x;
	uint32_t maxn = 0, i;

	for (i = 0; i < c->t[T_FROM]->zt_npools; i++)
		if (c->t[T_FROM]->zt_pools[i].zp_nnames > maxn)
			maxn = c->t[T_FROM]->zt_pools[i].zp_nnames;
	for (i = 0; i < c->t[T_ONTO]->zt_npools; i++)
		if (c->t[T_ONTO]->zt_pools[i].zp_nnames > maxn)
			maxn = c->t[T_ONTO]->zt_pools[i].zp_nnames;
	bf = malloc((size_t)(maxn + 1) * sizeof (*bf) * 2);
	if (bf == NULL)
		return (-1);
	bo = bf + maxn + 1;

	for (x = 0; x < c->nnames; x++) {
		const struct zr_pool *qf, *qo;
		long kf, ko;
		uint32_t nf, no;

		if ((c->state[x] & ZR_NS_CONTESTED) == 0)
			continue;
		qf = &c->t[T_FROM]->zt_pools[zr_tree_pool(c->t[T_FROM], x)];
		qo = &c->t[T_ONTO]->zt_pools[zr_tree_pool(c->t[T_ONTO], x)];
		kf = pool_class(c, qf);
		ko = pool_class(c, qo);
		nf = comembers(c, qf, x, bf);
		no = comembers(c, qo, x, bo);
		if (kf == -2 || ko == -2 || kf != ko || nf != no ||
		    memcmp(bf, bo, nf * sizeof (*bf)) != 0) {
			/* group through the from pool: both are one group */
			flag(c, x, ZR_CF_CONTESTED_HOME, x, ZR_NAME_NONE);
		}
		/* placement: join the class, or the co-members' class */
		if (kf >= 0) {
			for (i = 0; i < qf->zp_nnames; i++) {
				if (settled(c, qf->zp_names[i])) {
					uf_union(c->parent, qf->zp_names[i], x);
					break;
				}
			}
		} else if (nf > 0) {
			uf_union(c->parent, bf[0], x);
		}
	}
	free(bf);
	return (0);
}

/*
 * Conflict, strict mode: "two base pools share a result pool, but no
 * pool on either side fans in over both."
 */
static int
pass_unexpressed(struct ctx *c)
{
	struct u64set covered;
	uint32_t *scratch, *off;
	zr_pool_t *bp;
	zr_name_t *memb;
	uint32_t maxn = 0, i, k, total;
	int side, rc = 0;
	zr_name_t n;

	if (c->mode != ZR_MODE_STRICT)
		return (0);
	if (u64set_init(&covered, 64) != 0)
		return (-1);
	/* pairs of base pools some side pool holds survivors of */
	for (side = T_FROM; side <= T_ONTO; side++) {
		const struct zr_tree *t = c->t[side];

		for (i = 0; i < t->zt_npools; i++)
			if (t->zt_pools[i].zp_nnames > maxn)
				maxn = t->zt_pools[i].zp_nnames;
	}
	/*
	 * One scratch block for the whole pass, carved into three
	 * regions. An undersized block here segfaulted once, so every
	 * region's bound is spelled out.
	 *
	 *   bp    (maxn + nnames + 1) * 2 slots, as before: the base
	 *         pools of one side pool, or of one class, in the first
	 *         half; at offset nnames, a witness name per base pool
	 *         of the class.
	 *   off   nclass + 1 slots: the class index's offsets.
	 *   memb  nnames slots: the class index's members. A settled
	 *         base name has exactly one class, so nnames bounds
	 *         every class list together.
	 */
	if (maxn < c->nnames)
		maxn = c->nnames;
	scratch = malloc((((size_t)maxn + c->nnames + 1) * 2 +
	    ((size_t)c->nclass + 1) + c->nnames) * sizeof (*scratch));
	if (scratch == NULL) {
		u64set_fini(&covered);
		return (-1);
	}
	bp = scratch;
	off = scratch + ((size_t)maxn + c->nnames + 1) * 2;
	memb = off + (size_t)c->nclass + 1;
	for (side = T_FROM; side <= T_ONTO; side++) {
		const struct zr_tree *t = c->t[side];
		zr_pool_t qi;

		for (qi = 0; qi < t->zt_npools; qi++) {
			const struct zr_pool *q = &t->zt_pools[qi];
			uint32_t cnt = 0, a, b;

			for (i = 0; i < q->zp_nnames; i++) {
				zr_name_t m = q->zp_names[i];

				if (settled(c, m) && inbase(c, m))
					bp[cnt++] =
					    zr_tree_pool(c->t[T_BASE], m);
			}
			for (a = 0; a < cnt; a++) {
				for (b = a + 1; b < cnt; b++) {
					if (bp[a] == bp[b])
						continue;
					if (u64set_add(&covered,
					    pair_key(bp[a], bp[b])) != 0)
						rc = -1;
				}
			}
		}
	}
	/*
	 * Index the settled base names by result class: count, prefix
	 * sum, fill in ascending name order, then shift the offsets
	 * back. Class k is memb[off[k] .. off[k + 1]), ascending, the
	 * order a scan of the name table gave. pass_classes gave every
	 * settled name one class below nclass, which is what bounds
	 * both regions. The step below then walks its own class and
	 * not the whole name table, which is what makes it linear.
	 */
	for (k = 0; k <= c->nclass; k++)
		off[k] = 0;
	for (n = 0; n < c->nnames; n++)
		if (settled(c, n) && inbase(c, n))
			off[c->klass[n]]++;
	total = 0;
	for (k = 0; k <= c->nclass; k++) {
		uint32_t cnt = off[k];

		off[k] = total;
		total += cnt;
	}
	for (n = 0; n < c->nnames; n++)
		if (settled(c, n) && inbase(c, n))
			memb[off[c->klass[n]]++] = n;
	for (k = c->nclass; k > 0; k--)
		off[k] = off[k - 1];
	off[0] = 0;
	/* every class: every pair of its base pools must be covered */
	for (k = 0; k < c->nclass && rc == 0; k++) {
		uint32_t cnt = 0, a, b, e;
		zr_name_t first = ZR_NAME_NONE;

		for (e = off[k]; e < off[k + 1]; e++) {
			n = memb[e];
			bp[cnt] = zr_tree_pool(c->t[T_BASE], n);
			for (a = 0; a < cnt; a++)
				if (bp[a] == bp[cnt])
					break;
			if (a == cnt) {
				bp[cnt + c->nnames] = n;
				cnt++;
			}
			if (first == ZR_NAME_NONE)
				first = n;
		}
		for (a = 0; a < cnt; a++) {
			for (b = a + 1; b < cnt; b++) {
				if (!u64set_has(&covered,
				    pair_key(bp[a], bp[b]))) {
					flag(c, first, ZR_CF_UNEXPRESSED,
					    bp[a + c->nnames],
					    bp[b + c->nnames]);
					a = cnt;
					break;
				}
			}
		}
	}
	free(scratch);
	u64set_fini(&covered);
	return (rc);
}

/*
 * Yellow. Per base name: the three-way of its content, with adoption
 * in permissive mode; per result pool: agreement.
 */
#define	V_SAME	0
#define	V_DEAD	1
#define	V_LIT	2
#define	V_REF	3	/* follows the kept names of pool q on side s */
#define	V_PAIR	4	/* both changed with a ref involved; compare later */

struct val {
	uint8_t		kind;
	uint8_t		side;
	uint32_t	c;	/* literal, or the ref's fallback content */
	zr_pool_t	q;	/* the ref's side pool */
	struct val	*a, *b;	/* pair halves */
};

static uint32_t
content_in(const struct zr_tree *t, zr_name_t n)
{
	zr_pool_t q = zr_tree_pool(t, n);

	if (q == ZR_POOL_NONE)
		return (ZR_CONTENT_NONE);
	return (t->zt_pools[q].zp_content);
}

static void
side_value(const struct ctx *c, int side, zr_name_t n, struct val *v)
{
	uint32_t b = content_in(c->t[T_BASE], n);
	uint32_t s = content_in(c->t[side], n);

	memset(v, 0, sizeof (*v));
	v->side = (uint8_t)side;
	if (s == ZR_CONTENT_NONE) {
		v->kind = V_DEAD;
		return;
	}
	if (s == b) {
		v->kind = V_SAME;
		return;
	}
	v->c = s;
	if (c->mode == ZR_MODE_PERMISSIVE) {
		zr_pool_t qi = zr_tree_pool(c->t[side], n);
		const struct zr_pool *q = &c->t[side]->zt_pools[qi];
		uint32_t i;

		for (i = 0; i < q->zp_nnames; i++) {
			zr_name_t m = q->zp_names[i];

			if (inbase(c, m) && content_in(c->t[T_BASE], m) == s) {
				v->kind = V_REF;
				v->q = qi;
				return;
			}
		}
	}
	v->kind = V_LIT;
}

static int
val_eq(const struct val *x, const struct val *y)
{
	if (x->kind != y->kind)
		return (0);
	switch (x->kind) {
	case V_LIT:
		return (x->c == y->c);
	case V_REF:
		return (x->side == y->side && x->q == y->q);
	default:
		return (1);
	}
}

#define	R_PENDING	((uint32_t)-2)
#define	R_CONFLICT	((uint32_t)-3)
#define	R_DEAD		((uint32_t)-4)

/* Resolve one value against the concrete table; R_PENDING if not yet. */
static uint32_t
resolve(const struct ctx *c, const struct val *v, const uint32_t *concrete)
{
	uint32_t i, ra, rb;
	const struct zr_pool *q;
	int alive = 0;

	switch (v->kind) {
	case V_SAME:
		return (R_CONFLICT);	/* never asked */
	case V_DEAD:
		return (R_DEAD);
	case V_LIT:
		return (v->c);
	case V_REF:
		/*
		 * Order-free: every surviving kept name must be resolved,
		 * and they must agree. A torn kept pool cannot be followed.
		 */
		q = &c->t[v->side]->zt_pools[v->q];
		ra = R_PENDING;
		for (i = 0; i < q->zp_nnames; i++) {
			zr_name_t m = q->zp_names[i];

			if (!inbase(c, m) ||
			    content_in(c->t[T_BASE], m) != v->c)
				continue;
			if ((c->state[m] & ZR_NS_SURVIVES) == 0)
				continue;
			alive = 1;
			if (concrete[m] == R_PENDING)
				return (R_PENDING);
			if (concrete[m] == R_CONFLICT)
				return (R_CONFLICT);
			if (ra == R_PENDING)
				ra = concrete[m];
			else if (concrete[m] != ra)
				return (R_CONFLICT);
		}
		return (alive ? ra : v->c);
	default:
		ra = resolve(c, v->a, concrete);
		rb = resolve(c, v->b, concrete);
		if (ra == R_PENDING || rb == R_PENDING)
			return (R_PENDING);
		return (ra == rb ? ra : R_CONFLICT);
	}
}

static int
pass_yellow(struct ctx *c)
{
	struct zr_decision *out = c->out;
	struct val *vf, *vo;
	uint32_t *concrete, k, i;
	zr_name_t n;
	int progress;

	vf = calloc((size_t)c->nnames * 2 + 1, sizeof (*vf));
	concrete = malloc((size_t)c->nnames * sizeof (*concrete) + 1);
	if (vf == NULL || concrete == NULL) {
		free(vf);
		free(concrete);
		return (-1);
	}
	vo = vf + c->nnames;

	/* per base name: the three-way */
	for (n = 0; n < c->nnames; n++) {
		struct val *f = &vf[n], *o = &vo[n];

		concrete[n] = R_PENDING;
		if (!inbase(c, n)) {
			f->kind = V_LIT;
			f->c = ZR_CONTENT_NONE;	/* placed below */
			continue;
		}
		side_value(c, T_FROM, n, f);
		side_value(c, T_ONTO, n, o);
		if (f->kind == V_SAME && o->kind == V_SAME) {
			f->kind = V_LIT;
			f->c = content_in(c->t[T_BASE], n);
		} else if (f->kind == V_SAME) {
			*f = *o;
		} else if (o->kind == V_SAME) {
			/* f stands */
		} else if (val_eq(f, o)) {
			/* f stands */
		} else if ((f->kind == V_LIT && o->kind == V_LIT) ||
		    f->kind == V_DEAD || o->kind == V_DEAD) {
			flag(c, n, ZR_CF_CHANGED_BOTH, n, ZR_NAME_NONE);
			f->kind = V_LIT;
			f->c = ZR_CONTENT_NONE;
		} else {
			struct val *pa = &vo[n];	/* keep o's storage */
			struct val tmp = *f;

			*pa = *o;
			f->kind = V_PAIR;
			f->a = NULL;
			f->b = pa;
			/* stash f's own value in the spare half */
			f->a = malloc(sizeof (*f->a));
			if (f->a == NULL) {
				free(vf);
				free(concrete);
				return (-1);
			}
			*f->a = tmp;
		}
	}
	/* resolve adoptions to a fixed point */
	do {
		progress = 0;
		for (n = 0; n < c->nnames; n++) {
			uint32_t r;

			if (!inbase(c, n) || concrete[n] != R_PENDING)
				continue;
			if (vf[n].kind == V_LIT && vf[n].c == ZR_CONTENT_NONE) {
				/* flagged already */
				concrete[n] = R_CONFLICT;
				progress = 1;
				continue;
			}
			r = resolve(c, &vf[n], concrete);
			if (r == R_PENDING)
				continue;
			if (r == R_CONFLICT)
				flag(c, n, ZR_CF_CHANGED_BOTH, n, ZR_NAME_NONE);
			concrete[n] = r;
			progress = 1;
		}
	} while (progress);
	for (n = 0; n < c->nnames; n++) {
		if (inbase(c, n) && concrete[n] == R_PENDING) {
			flag(c, n, ZR_CF_CHANGED_BOTH, n, ZR_NAME_NONE);
			concrete[n] = R_CONFLICT;
		}
		if (vf[n].kind == V_PAIR)
			free(vf[n].a);
	}

	/* per result pool: agreement, then the pool's content */
	for (k = 0; k < out->zd_npools; k++) {
		struct zr_result_pool *rp = &out->zd_pools[k];
		uint32_t v = R_PENDING, cf = R_PENDING, co = R_PENDING;
		zr_name_t first = ZR_NAME_NONE, other = ZR_NAME_NONE;

		for (i = 0; i < rp->zr_nnames; i++) {
			n = rp->zr_names[i];
			if (inbase(c, n)) {
				uint32_t r = concrete[n];

				if (r == R_CONFLICT)
					continue;
				if (v == R_PENDING) {
					v = r;
					first = n;
				} else if (r != v && other == ZR_NAME_NONE) {
					other = n;
				}
			} else {
				uint32_t a = content_in(c->t[T_FROM], n);
				uint32_t b = content_in(c->t[T_ONTO], n);

				if (a != ZR_CONTENT_NONE)
					cf = a;
				if (b != ZR_CONTENT_NONE)
					co = b;
			}
		}
		if (other != ZR_NAME_NONE) {
			flag(c, first, ZR_CF_DISAGREE, first, other);
			continue;
		}
		if (v != R_PENDING) {
			rp->zr_content = v;
			continue;
		}
		/* no base name: a fresh pool, both sides must agree */
		if (cf != R_PENDING && co != R_PENDING && cf != co) {
			flag(c, rp->zr_names[0], ZR_CF_DISAGREE,
			    rp->zr_names[0], ZR_NAME_NONE);
			continue;
		}
		rp->zr_content = (cf != R_PENDING ? cf : co);
	}
	free(vf);
	free(concrete);
	return (0);
}

static int
name_cmp(const void *a, const void *b)
{
	zr_name_t x = *(const zr_name_t *)a, y = *(const zr_name_t *)b;

	return (x < y ? -1 : (x > y));
}

/* Materialize the result pools from the union-find, contested placed. */
static int
pass_pools(struct ctx *c)
{
	struct zr_decision *out = c->out;
	uint32_t *count, k;
	zr_name_t n;

	/* renumber classes now that contested names are placed */
	c->nclass = 0;
	for (n = 0; n < c->nnames; n++)
		c->klass[n] = (uint32_t)-1;
	for (n = 0; n < c->nnames; n++) {
		zr_name_t root;

		if ((c->state[n] & ZR_NS_SURVIVES) == 0)
			continue;
		root = uf_find(c->parent, n);
		if (c->klass[root] == (uint32_t)-1)
			c->klass[root] = c->nclass++;
		c->klass[n] = c->klass[root];
	}
	out->zd_pools = calloc(c->nclass ? c->nclass : 1,
	    sizeof (*out->zd_pools));
	count = calloc(c->nclass ? c->nclass : 1, sizeof (*count));
	if (out->zd_pools == NULL || count == NULL) {
		free(count);
		return (-1);
	}
	out->zd_npools = c->nclass;
	for (n = 0; n < c->nnames; n++)
		if (c->state[n] & ZR_NS_SURVIVES)
			count[c->klass[n]]++;
	for (k = 0; k < c->nclass; k++) {
		out->zd_pools[k].zr_names = malloc((size_t)count[k] *
		    sizeof (zr_name_t));
		if (out->zd_pools[k].zr_names == NULL) {
			free(count);
			return (-1);
		}
		out->zd_pools[k].zr_content = ZR_CONTENT_NONE;
		count[k] = 0;
	}
	for (n = 0; n < c->nnames; n++) {
		if ((c->state[n] & ZR_NS_SURVIVES) == 0) {
			out->zd_result_of[n] = ZR_POOL_NONE;
			continue;
		}
		k = c->klass[n];
		out->zd_pools[k].zr_names[count[k]++] = n;
		out->zd_result_of[n] = k;
	}
	for (k = 0; k < c->nclass; k++) {
		struct zr_result_pool *rp = &out->zd_pools[k];

		rp->zr_nnames = count[k];
		qsort(rp->zr_names, rp->zr_nnames, sizeof (zr_name_t),
		    name_cmp);
		rp->zr_group = group_of_name(c, rp->zr_names[0]);
	}
	free(count);
	return (0);
}

/* Fill each group's pool lists for the conflict record. */
static int
pass_group_members(struct ctx *c)
{
	struct zr_decision *out = c->out;
	uint32_t i, g;
	int side;
	zr_name_t n;

	for (side = T_FROM; side <= T_ONTO; side++) {
		const struct zr_tree *t = c->t[side];

		for (i = 0; i < t->zt_npools; i++) {
			struct zr_group *gp = &out->zd_groups[
			    c->gindex[pnode_find(c, pnode_id(c, side, i))]];

			if (side == T_FROM)
				gp->zg_nfrom++;
			else
				gp->zg_nonto++;
		}
	}
	/* base pools: through any surviving name's side pool; count once */
	for (n = 0; n < c->nnames; n++) {
		zr_pool_t p;

		if (!inbase(c, n) || (c->state[n] & ZR_NS_SURVIVES) == 0)
			continue;
		p = zr_tree_pool(c->t[T_BASE], n);
		if (c->t[T_BASE]->zt_pools[p].zp_names[0] != n)
			continue;	/* one count per base pool */
		out->zd_groups[group_of_name(c, n)].zg_nbase++;
	}
	for (g = 0; g < out->zd_ngroups; g++) {
		struct zr_group *gp = &out->zd_groups[g];

		gp->zg_base = calloc(gp->zg_nbase + 1, sizeof (zr_pool_t));
		gp->zg_from = calloc(gp->zg_nfrom + 1, sizeof (zr_pool_t));
		gp->zg_onto = calloc(gp->zg_nonto + 1, sizeof (zr_pool_t));
		if (gp->zg_base == NULL || gp->zg_from == NULL ||
		    gp->zg_onto == NULL)
			return (-1);
		gp->zg_nbase = gp->zg_nfrom = gp->zg_nonto = 0;
	}
	for (side = T_FROM; side <= T_ONTO; side++) {
		const struct zr_tree *t = c->t[side];

		for (i = 0; i < t->zt_npools; i++) {
			struct zr_group *gp = &out->zd_groups[
			    c->gindex[pnode_find(c, pnode_id(c, side, i))]];

			if (side == T_FROM)
				gp->zg_from[gp->zg_nfrom++] = i;
			else
				gp->zg_onto[gp->zg_nonto++] = i;
		}
	}
	for (n = 0; n < c->nnames; n++) {
		zr_pool_t p;
		struct zr_group *gp;

		if (!inbase(c, n) || (c->state[n] & ZR_NS_SURVIVES) == 0)
			continue;
		p = zr_tree_pool(c->t[T_BASE], n);
		if (c->t[T_BASE]->zt_pools[p].zp_names[0] != n)
			continue;
		gp = &out->zd_groups[group_of_name(c, n)];
		gp->zg_base[gp->zg_nbase++] = p;
	}
	return (0);
}

const char *
zr_conflict_name(uint32_t f)
{
	switch (f) {
	case ZR_CF_HEALED_SPLIT:
		return ("healed-split");
	case ZR_CF_ORPHANED_ADD:
		return ("orphaned-add");
	case ZR_CF_CONTESTED_HOME:
		return ("contested-home");
	case ZR_CF_UNEXPRESSED:
		return ("unexpressed-sharing");
	case ZR_CF_CHANGED_BOTH:
		return ("changed-both");
	case ZR_CF_DISAGREE:
		return ("disagree");
	default:
		return ("?");
	}
}

void
zr_decision_fini(struct zr_decision *d)
{
	uint32_t i;

	for (i = 0; i < d->zd_npools; i++)
		free(d->zd_pools[i].zr_names);
	free(d->zd_pools);
	for (i = 0; i < d->zd_ngroups; i++) {
		free(d->zd_groups[i].zg_base);
		free(d->zd_groups[i].zg_from);
		free(d->zd_groups[i].zg_onto);
	}
	free(d->zd_groups);
	free(d->zd_state);
	free(d->zd_result_of);
	memset(d, 0, sizeof (*d));
}

int
zr_decide(const struct zr_tree *base, const struct zr_tree *from,
    const struct zr_tree *onto, zr_mode_t mode, struct zr_decision *out)
{
	struct ctx c;
	uint32_t i;
	int rc = -1;

	memset(&c, 0, sizeof (c));
	memset(out, 0, sizeof (*out));
	c.t[T_BASE] = base;
	c.t[T_FROM] = from;
	c.t[T_ONTO] = onto;
	c.mode = mode;
	c.out = out;
	c.nnames = zr_names_count(base->zt_names);
	out->zd_nnames = c.nnames;

	c.state = calloc(c.nnames + 1, sizeof (*c.state));
	c.parent = malloc(((size_t)c.nnames + 1) * sizeof (*c.parent));
	c.klass = malloc(((size_t)c.nnames + 1) * sizeof (*c.klass));
	c.npnode = from->zt_npools + onto->zt_npools;
	c.pnode = malloc(((size_t)c.npnode + 1) * sizeof (*c.pnode));
	c.gindex = malloc(((size_t)c.npnode + 1) * sizeof (*c.gindex));
	out->zd_result_of = malloc(((size_t)c.nnames + 1) *
	    sizeof (*out->zd_result_of));
	if (c.state == NULL || c.parent == NULL || c.klass == NULL ||
	    c.pnode == NULL || c.gindex == NULL || out->zd_result_of == NULL)
		goto done;
	out->zd_state = c.state;

	pass_names(&c);
	pass_lumps(&c);
	if (pass_labels(&c) != 0 || pass_classes(&c) != 0 ||
	    pass_groups(&c) != 0)
		goto done;
	pass_healed(&c);
	pass_orphaned(&c);
	if (pass_contested(&c) != 0 || pass_unexpressed(&c) != 0)
		goto done;
	if (pass_pools(&c) != 0 || pass_group_members(&c) != 0)
		goto done;
	if (pass_yellow(&c) != 0)
		goto done;
	for (i = 0; i < out->zd_ngroups; i++)
		if (out->zd_groups[i].zg_flags != 0)
			out->zd_nconflicts++;
	for (i = 0; i < out->zd_npools; i++)
		if (out->zd_groups[out->zd_pools[i].zr_group].zg_flags != 0)
			out->zd_pools[i].zr_content = ZR_CONTENT_NONE;
	rc = 0;
done:
	free(c.parent);
	free(c.klass);
	free(c.pnode);
	free(c.gindex);
	if (rc != 0) {
		free(c.state);
		out->zd_state = NULL;
		zr_decision_fini(out);
	}
	return (rc);
}
