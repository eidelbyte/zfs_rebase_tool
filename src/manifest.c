/*
 * The manifest emitter. The result namespace is built as arrays of
 * nodes, one per name that has something to say plus every directory
 * on the way to one, and walked pre-order with the parent links as
 * the stack: nothing here recurses. The walk fixes manifest order,
 * manifest order fixes each result pool's anchor, and the anchors
 * decide which result pool keeps which onto object. Then the walk
 * runs again to write the lines, and the groups the tree section
 * marked are written out as records in the order it named them.
 *
 * One name can hold two lines. A surviving name whose result object
 * is not the type onto has at that name cannot be written in place,
 * so onto's directory goes out as a removal with its children and
 * its two dots, and the create of the new object takes the name
 * again on the line after that close.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "manifest.h"
#include "name.h"
#include "vis.h"

#define	ZM_BASE		0
#define	ZM_FROM		1
#define	ZM_ONTO		2

#define	ZM_NONE		((uint32_t)-1)
#define	ZM_INDENT	4

/* What a name line says. */
#define	ZM_NOTHING	0
#define	ZM_RM		1
#define	ZM_LN		2
#define	ZM_CP		3
#define	ZM_WRITE	4
#define	ZM_CONFLICT	5
#define	ZM_DUP		6	/* cp, but the bytes are onto's own */

/* Letters for the pool notation, in the order the format note gives. */
static const char zm_alphabet[] =
	"xyzwvutsrqponmlkjihgfedcbaXYZWVUTSRQPONMLKJIHGFEDCBA";

/* The classes, in the order one record lists them. */
static const uint32_t zm_class[] = {
	ZR_CF_HEALED_SPLIT, ZR_CF_ORPHANED_ADD, ZR_CF_CONTESTED_HOME,
	ZR_CF_UNEXPRESSED, ZR_CF_CHANGED_BOTH, ZR_CF_DISAGREE
};

#define	ZM_NCLASS	(sizeof (zm_class) / sizeof (zm_class[0]))

/*
 * The why sentence of each class in pieces: what stands between the
 * two names, NULL when the sentence names one name only, and what
 * follows the last name.
 */
struct zm_sentence {
	const char	*zs_mid;
	const char	*zs_end;
};

static const struct zm_sentence zm_why_text[ZM_NCLASS] = {
	{ " and ", " were split on one side and joined by the other" },
	{ NULL, " was added to a pool the other side deleted" },
	{ NULL, " was created on both sides in different pools" },
	{ " and ", " would share a pool that neither side joined" },
	{ NULL, " changed on both sides" },
	{ " and ", " end in one pool with different contents" }
};

/*
 * One name of the result namespace, or a directory only on the way to
 * one. Children hang off zn_first and zn_next in walk order.
 */
struct zm_node {
	zr_name_t	zn_name;	/* ZR_NAME_NONE only for the root */
	zr_name_t	zn_arg;		/* the ln, cp or write argument */
	zr_name_t	zn_arg2;	/* the argument of the second line */
	uint32_t	zn_parent;
	uint32_t	zn_first;
	uint32_t	zn_next;
	uint32_t	zn_ord;		/* position in manifest order */
	uint32_t	zn_group;	/* the group a conflict mark names */
	uint32_t	zn_cnum;	/* that group's number in the file */
	uint8_t		zn_act;
	uint8_t		zn_act2;	/* the create after a removal */
	uint8_t		zn_dir;
	uint8_t		zn_show;
};

/* Everything one emit run carries. */
struct zm {
	int			zm_err;		/* a content with no source */
	const struct zr_tree		*zm_t[3];
	const struct zr_decision	*zm_d;
	const struct zr_names		*zm_ns;
	uint32_t			zm_nnames;
	struct zm_node			*zm_nodes;
	uint32_t			zm_nnodes;
	uint32_t			*zm_node_of;	/* name -> node */
	uint32_t			*zm_order;	/* walk step -> node */
	zr_name_t			*zm_anchor;	/* result pool */
	zr_pool_t			*zm_keeps;	/* result pool */
	uint32_t			*zm_ogroup;	/* onto pool -> group */
	uint32_t			*zm_cnum;	/* group -> number */
	uint32_t			*zm_corder;	/* number -> group */
	uint32_t			*zm_letter;	/* one record's pools */
	uint32_t			zm_nletter;
	uint32_t			zm_maxletter;
	uint32_t			zm_nconf;
	char				*zm_scr;
	size_t				zm_scrcap;
};

/*
 * The last component of a path: the bytes that follow its final slash.
 */
static const char *
zm_leaf(const char *p, size_t len, size_t *leaflen)
{
	size_t cut = len;

	while (cut > 0 && p[cut - 1] != '/')
		cut--;
	*leaflen = len - cut;
	return (p + cut);
}

/*
 * Write bytes the way the format escapes them. The scratch buffer was
 * sized for the longest name in the table, so this cannot fail.
 */
static void
zm_vis(const struct zm *m, FILE *out, const char *p, size_t len)
{
	(void) zr_vis_encode((const unsigned char *)p, len, m->zm_scr,
	    m->zm_scrcap);
	(void) fputs(m->zm_scr, out);
}

static void
zm_vis_name(const struct zm *m, FILE *out, zr_name_t id)
{
	const char *p;
	size_t len = 0;

	p = zr_names_str(m->zm_ns, id, &len);
	if (p == NULL) {
		(void) fputc('?', out);
		return;
	}
	zm_vis(m, out, p, len);
}

static int
zm_scratch(struct zm *m)
{
	size_t max = 1, len = 0;
	zr_name_t n;

	for (n = 0; n < m->zm_nnames; n++) {
		if (zr_names_str(m->zm_ns, n, &len) != NULL && len > max)
			max = len;
	}
	if (max > ((size_t)-1 - 1) / 4)
		return (-1);
	m->zm_scrcap = max * 4 + 1;
	m->zm_scr = malloc(m->zm_scrcap);
	return (m->zm_scr != NULL ? 0 : -1);
}

static void
zm_node_init(struct zm_node *nd, zr_name_t name)
{
	nd->zn_name = name;
	nd->zn_arg = ZR_NAME_NONE;
	nd->zn_arg2 = ZR_NAME_NONE;
	nd->zn_parent = 0;
	nd->zn_first = ZM_NONE;
	nd->zn_next = ZM_NONE;
	nd->zn_ord = 0;
	nd->zn_group = ZM_NONE;
	nd->zn_cnum = 0;
	nd->zn_act = ZM_NOTHING;
	nd->zn_act2 = ZM_NOTHING;
	nd->zn_dir = 0;
	nd->zn_show = 0;
}

struct zm_kid {
	const char	*zk_leaf;
	size_t		zk_len;
	uint32_t	zk_parent;
	uint32_t	zk_node;
};

/*
 * Children of one directory sort by their leaf name as bytes. The
 * parent leads the key so that one sort orders every child list.
 */
static int
zm_kid_cmp(const void *a, const void *b)
{
	const struct zm_kid *x = a, *y = b;
	size_t n;
	int r;

	if (x->zk_parent != y->zk_parent)
		return (x->zk_parent < y->zk_parent ? -1 : 1);
	n = x->zk_len < y->zk_len ? x->zk_len : y->zk_len;
	r = n != 0 ? memcmp(x->zk_leaf, y->zk_leaf, n) : 0;
	if (r != 0)
		return (r);
	if (x->zk_len != y->zk_len)
		return (x->zk_len < y->zk_len ? -1 : 1);
	return (x->zk_node < y->zk_node ? -1 : (x->zk_node > y->zk_node));
}

/*
 * The result namespace and the tree over it: every name that survives,
 * every name onto holds that does not, and every directory on the way
 * to one. The root is node 0 whether or not the table holds "/".
 */
static int
zm_build(struct zm *m)
{
	struct zm_kid *kids;
	uint8_t *mark;
	zr_name_t n, root;
	uint32_t cnt, i, k;

	mark = calloc((size_t)m->zm_nnames + 1, 1);
	m->zm_node_of = malloc(((size_t)m->zm_nnames + 1) * sizeof (uint32_t));
	if (mark == NULL || m->zm_node_of == NULL) {
		free(mark);
		return (-1);
	}
	for (n = 0; n < m->zm_nnames; n++) {
		m->zm_node_of[n] = ZM_NONE;
		if ((m->zm_d->zd_state[n] &
		    (ZR_NS_SURVIVES | ZR_NS_ONTO)) != 0)
			mark[n] = 1;
	}
	for (n = 0; n < m->zm_nnames; n++) {
		zr_name_t p = n;

		if (mark[n] == 0)
			continue;
		for (;;) {
			p = zr_names_parent(m->zm_ns, p);
			if (p == ZR_NAME_NONE || mark[p] != 0)
				break;
			mark[p] = 1;
		}
	}
	root = zr_names_lookup(m->zm_ns, "/", 1);
	m->zm_nnodes = 1;
	for (n = 0; n < m->zm_nnames; n++)
		if (mark[n] != 0 && n != root)
			m->zm_nnodes++;
	m->zm_nodes = malloc((size_t)m->zm_nnodes * sizeof (struct zm_node));
	kids = malloc((size_t)m->zm_nnodes * sizeof (struct zm_kid));
	if (m->zm_nodes == NULL || kids == NULL) {
		free(mark);
		free(kids);
		return (-1);
	}
	zm_node_init(&m->zm_nodes[0], root);
	if (root != ZR_NAME_NONE)
		m->zm_node_of[root] = 0;
	k = 1;
	for (n = 0; n < m->zm_nnames; n++) {
		if (mark[n] == 0 || n == root)
			continue;
		zm_node_init(&m->zm_nodes[k], n);
		m->zm_node_of[n] = k;
		k++;
	}
	free(mark);
	cnt = 0;
	for (k = 1; k < m->zm_nnodes; k++) {
		const char *p;
		size_t len = 0;
		zr_name_t par;

		par = zr_names_parent(m->zm_ns, m->zm_nodes[k].zn_name);
		if (par != ZR_NAME_NONE && m->zm_node_of[par] != ZM_NONE)
			m->zm_nodes[k].zn_parent = m->zm_node_of[par];
		p = zr_names_str(m->zm_ns, m->zm_nodes[k].zn_name, &len);
		kids[cnt].zk_parent = m->zm_nodes[k].zn_parent;
		kids[cnt].zk_node = k;
		kids[cnt].zk_leaf = zm_leaf(p, len, &kids[cnt].zk_len);
		cnt++;
	}
	qsort(kids, cnt, sizeof (*kids), zm_kid_cmp);
	for (i = cnt; i > 0; i--) {
		uint32_t node = kids[i - 1].zk_node;
		uint32_t par = kids[i - 1].zk_parent;

		m->zm_nodes[node].zn_next = m->zm_nodes[par].zn_first;
		m->zm_nodes[par].zn_first = node;
	}
	free(kids);
	return (0);
}

/*
 * Manifest order: pre-order over the whole namespace, a directory's
 * children in leaf-name order. Walk order is what counts, not the
 * byte order of full paths.
 */
static void
zm_walk_order(struct zm *m)
{
	uint32_t cur = 0, i = 0;

	for (;;) {
		m->zm_nodes[cur].zn_ord = i;
		m->zm_order[i] = cur;
		i++;
		if (m->zm_nodes[cur].zn_first != ZM_NONE) {
			cur = m->zm_nodes[cur].zn_first;
			continue;
		}
		for (;;) {
			if (cur == 0)
				return;
			if (m->zm_nodes[cur].zn_next != ZM_NONE) {
				cur = m->zm_nodes[cur].zn_next;
				break;
			}
			cur = m->zm_nodes[cur].zn_parent;
		}
	}
}

static uint32_t
zm_ord(const struct zm *m, zr_name_t n)
{
	uint32_t k;

	if (n >= m->zm_nnames)
		return (ZM_NONE);
	k = m->zm_node_of[n];
	return (k == ZM_NONE ? ZM_NONE : m->zm_nodes[k].zn_ord);
}

/*
 * The anchor of a result pool is its first name in manifest order. The
 * onto object a result pool keeps is the onto pool whose own first
 * name in manifest order is that anchor; every other result pool
 * drawing names from that onto pool gets a new object instead.
 */
static int
zm_anchors(struct zm *m)
{
	const struct zr_decision *d = m->zm_d;
	const struct zr_tree *onto = m->zm_t[ZM_ONTO];
	uint32_t i, k;
	zr_pool_t q;

	m->zm_anchor = malloc(((size_t)d->zd_npools + 1) * sizeof (zr_name_t));
	m->zm_keeps = malloc(((size_t)d->zd_npools + 1) * sizeof (zr_pool_t));
	if (m->zm_anchor == NULL || m->zm_keeps == NULL)
		return (-1);
	for (k = 0; k < d->zd_npools; k++) {
		const struct zr_result_pool *rp = &d->zd_pools[k];
		zr_name_t best = ZR_NAME_NONE;
		uint32_t bo = ZM_NONE;

		for (i = 0; i < rp->zr_nnames; i++) {
			uint32_t o = zm_ord(m, rp->zr_names[i]);

			if (best == ZR_NAME_NONE || o < bo) {
				best = rp->zr_names[i];
				bo = o;
			}
		}
		m->zm_anchor[k] = best;
		m->zm_keeps[k] = ZR_POOL_NONE;
	}
	for (q = 0; q < onto->zt_npools; q++) {
		const struct zr_pool *p = &onto->zt_pools[q];
		zr_name_t first = ZR_NAME_NONE;
		uint32_t bo = ZM_NONE;

		for (i = 0; i < p->zp_nnames; i++) {
			uint32_t o = zm_ord(m, p->zp_names[i]);

			if (first == ZR_NAME_NONE || o < bo) {
				first = p->zp_names[i];
				bo = o;
			}
		}
		if (first == ZR_NAME_NONE ||
		    (d->zd_state[first] & ZR_NS_SURVIVES) == 0)
			continue;
		k = d->zd_result_of[first];
		if (k < d->zd_npools && m->zm_anchor[k] == first)
			m->zm_keeps[k] = q;
	}
	return (0);
}

/* Which group each onto pool sits in, for the names that only die. */
static int
zm_ogroups(struct zm *m)
{
	uint32_t g, i, n = m->zm_t[ZM_ONTO]->zt_npools;

	m->zm_ogroup = malloc(((size_t)n + 1) * sizeof (uint32_t));
	if (m->zm_ogroup == NULL)
		return (-1);
	for (i = 0; i < n; i++)
		m->zm_ogroup[i] = ZM_NONE;
	for (g = 0; g < m->zm_d->zd_ngroups; g++) {
		const struct zr_group *gp = &m->zm_d->zd_groups[g];

		for (i = 0; i < gp->zg_nonto; i++)
			if (gp->zg_onto[i] < n)
				m->zm_ogroup[gp->zg_onto[i]] = g;
	}
	return (0);
}

/* The trailing slash follows onto's idea of the type, else from's. */
static void
zm_types(struct zm *m)
{
	uint32_t k;

	m->zm_nodes[0].zn_dir = 1;
	for (k = 1; k < m->zm_nnodes; k++) {
		struct zm_node *nd = &m->zm_nodes[k];
		const struct zr_tree *t = m->zm_t[ZM_ONTO];
		zr_pool_t q;

		q = zr_tree_pool(t, nd->zn_name);
		if (q == ZR_POOL_NONE) {
			t = m->zm_t[ZM_FROM];
			q = zr_tree_pool(t, nd->zn_name);
		}
		if (q != ZR_POOL_NONE && t->zt_pools[q].zp_type == ZR_T_DIR)
			nd->zn_dir = 1;
		if (nd->zn_first != ZM_NONE)
			nd->zn_dir = 1;
	}
}

/*
 * The path a cp or a write names: a name from holds whose pool has the
 * result pool's content. The name itself when it will do, else the
 * pool's first such name in manifest order, else ZR_NAME_NONE: the
 * content is not from's, and a cp becomes a dup (below).
 */
static zr_name_t
zm_from_path(const struct zm *m, zr_name_t n, uint32_t k)
{
	const struct zr_result_pool *rp = &m->zm_d->zd_pools[k];
	const struct zr_tree *from = m->zm_t[ZM_FROM];
	uint32_t content = rp->zr_content;
	zr_name_t best = ZR_NAME_NONE;
	uint32_t bo = ZM_NONE, i;
	zr_pool_t q;

	q = zr_tree_pool(from, n);
	if (q != ZR_POOL_NONE && from->zt_pools[q].zp_content == content)
		return (n);
	for (i = 0; i < rp->zr_nnames; i++) {
		zr_name_t cand = rp->zr_names[i];
		uint32_t o;

		q = zr_tree_pool(from, cand);
		if (q == ZR_POOL_NONE ||
		    from->zt_pools[q].zp_content != content)
			continue;
		o = zm_ord(m, cand);
		if (best == ZR_NAME_NONE || o < bo) {
			best = cand;
			bo = o;
		}
	}
	return (best);
}

/*
 * When no name of from holds the result pool's content, the bytes are
 * onto's own: a split the other side made without an edit while onto
 * edited the shared file. The severed half is then a copy of the
 * onto object that another result pool keeps; that pool's anchor is
 * the path to copy from, and it still holds the object when the line
 * is applied. ZR_NAME_NONE when no kept onto object has the content.
 */
static zr_name_t
zm_dup_path(const struct zm *m, uint32_t k)
{
	const struct zr_decision *d = m->zm_d;
	const struct zr_tree *onto = m->zm_t[ZM_ONTO];
	uint32_t content = d->zd_pools[k].zr_content, k2;

	for (k2 = 0; k2 < d->zd_npools; k2++) {
		zr_pool_t q = m->zm_keeps[k2];

		if (k2 == k || q == ZR_POOL_NONE)
			continue;
		if (onto->zt_pools[q].zp_content == content)
			return (m->zm_anchor[k2]);
	}
	return (ZR_NAME_NONE);
}

/*
 * A surviving name onto holds as a directory whose result object is
 * something else. The directory cannot become a file in place, so the
 * name takes two lines: onto's object is removed and the new one is
 * created after the removal closes. The other way round -- onto
 * holding a leaf where the result is a directory -- is not this
 * shape and is not answered here.
 */
static int
zm_type_change(const struct zm *m, zr_name_t n)
{
	const struct zr_tree *from = m->zm_t[ZM_FROM];
	const struct zr_tree *onto = m->zm_t[ZM_ONTO];
	zr_pool_t qf = zr_tree_pool(from, n);
	zr_pool_t qo = zr_tree_pool(onto, n);

	if (qf == ZR_POOL_NONE || qo == ZR_POOL_NONE)
		return (0);
	if (onto->zt_pools[qo].zp_type != ZR_T_DIR)
		return (0);
	return (from->zt_pools[qf].zp_type != ZR_T_DIR);
}

/*
 * One name's line. A name in a conflicted group is marked and nothing
 * else; a name onto holds and the result does not is removed; the
 * anchor of a result pool writes or copies the bytes and every other
 * name of the pool links to the anchor, unless the anchor's own write
 * already reaches it through the onto object they share.
 */
static void
zm_action_of(struct zm *m, uint32_t k)
{
	struct zm_node *nd = &m->zm_nodes[k];
	const struct zr_decision *d = m->zm_d;
	zr_name_t n = nd->zn_name;
	uint32_t g, rk;
	zr_pool_t q;

	if (n == ZR_NAME_NONE || n >= m->zm_nnames)
		return;
	if ((d->zd_state[n] & ZR_NS_SURVIVES) == 0) {
		if ((d->zd_state[n] & ZR_NS_ONTO) == 0)
			return;
		q = zr_tree_pool(m->zm_t[ZM_ONTO], n);
		g = q != ZR_POOL_NONE ? m->zm_ogroup[q] : ZM_NONE;
		if (g != ZM_NONE && d->zd_groups[g].zg_flags != 0) {
			nd->zn_act = ZM_CONFLICT;
			nd->zn_group = g;
		} else {
			nd->zn_act = ZM_RM;
		}
		return;
	}
	rk = d->zd_result_of[n];
	if (rk >= d->zd_npools)
		return;
	g = d->zd_pools[rk].zr_group;
	if (g < d->zd_ngroups && d->zd_groups[g].zg_flags != 0) {
		nd->zn_act = ZM_CONFLICT;
		nd->zn_group = g;
		return;
	}
	if (zm_type_change(m, n) != 0) {
		nd->zn_act = ZM_RM;
		if (n == m->zm_anchor[rk]) {
			nd->zn_act2 = ZM_CP;
			nd->zn_arg2 = zm_from_path(m, n, rk);
			if (nd->zn_arg2 == ZR_NAME_NONE) {
				nd->zn_act2 = ZM_DUP;
				nd->zn_arg2 = zm_dup_path(m, rk);
			}
			if (nd->zn_arg2 == ZR_NAME_NONE)
				m->zm_err = 1;
		} else {
			nd->zn_act2 = ZM_LN;
			nd->zn_arg2 = m->zm_anchor[rk];
		}
		return;
	}
	if (n == m->zm_anchor[rk]) {
		q = m->zm_keeps[rk];
		if (q != ZR_POOL_NONE &&
		    m->zm_t[ZM_ONTO]->zt_pools[q].zp_content ==
		    d->zd_pools[rk].zr_content)
			return;
		nd->zn_act = q != ZR_POOL_NONE ? ZM_WRITE : ZM_CP;
		nd->zn_arg = zm_from_path(m, n, rk);
		if (nd->zn_arg == ZR_NAME_NONE && nd->zn_act == ZM_CP) {
			nd->zn_act = ZM_DUP;
			nd->zn_arg = zm_dup_path(m, rk);
		}
		if (nd->zn_arg == ZR_NAME_NONE)
			m->zm_err = 1;
		return;
	}
	q = zr_tree_pool(m->zm_t[ZM_ONTO], n);
	if (q != ZR_POOL_NONE && q == m->zm_keeps[rk])
		return;
	nd->zn_act = ZM_LN;
	nd->zn_arg = m->zm_anchor[rk];
}

/* Only names with something to say appear, plus the way to them. */
static void
zm_show(struct zm *m)
{
	uint32_t k;

	m->zm_nodes[0].zn_show = 1;
	for (k = 1; k < m->zm_nnodes; k++) {
		uint32_t p;

		if (m->zm_nodes[k].zn_act == ZM_NOTHING)
			continue;
		m->zm_nodes[k].zn_show = 1;
		p = m->zm_nodes[k].zn_parent;
		while (m->zm_nodes[p].zn_show == 0) {
			m->zm_nodes[p].zn_show = 1;
			p = m->zm_nodes[p].zn_parent;
		}
	}
}

/*
 * Number the conflicted groups in the order the walk first names them,
 * and count the lines that are actions. A conflicted group the walk
 * never named still gets a record, after the ones it did.
 */
static uint32_t
zm_number(struct zm *m)
{
	uint32_t g, i, nactions = 0;

	for (g = 0; g < m->zm_d->zd_ngroups; g++)
		m->zm_cnum[g] = 0;
	m->zm_nconf = 0;
	for (i = 0; i < m->zm_nnodes; i++) {
		struct zm_node *nd = &m->zm_nodes[m->zm_order[i]];

		if (nd->zn_act == ZM_CONFLICT) {
			g = nd->zn_group;
			if (m->zm_cnum[g] == 0) {
				m->zm_cnum[g] = ++m->zm_nconf;
				m->zm_corder[m->zm_nconf] = g;
			}
			nd->zn_cnum = m->zm_cnum[g];
		} else if (nd->zn_act != ZM_NOTHING) {
			nactions++;
		}
		if (nd->zn_act2 != ZM_NOTHING)
			nactions++;
	}
	for (g = 0; g < m->zm_d->zd_ngroups; g++) {
		if (m->zm_d->zd_groups[g].zg_flags == 0 || m->zm_cnum[g] != 0)
			continue;
		m->zm_cnum[g] = ++m->zm_nconf;
		m->zm_corder[m->zm_nconf] = g;
	}
	return (nactions);
}

static void
zm_indent(FILE *out, uint32_t depth)
{
	uint32_t i;

	for (i = 0; i < depth * ZM_INDENT; i++)
		(void) fputc(' ', out);
}

/* One line's action and the one argument it takes. */
static void
zm_emit_act(const struct zm *m, FILE *out, uint32_t act, zr_name_t arg,
    uint32_t cnum)
{
	switch (act) {
	case ZM_RM:
		(void) fputs(" rm", out);
		break;
	case ZM_LN:
		(void) fputs(" ln ", out);
		zm_vis_name(m, out, arg);
		break;
	case ZM_DUP:
		(void) fputs(" dup ", out);
		zm_vis_name(m, out, arg);
		break;
	case ZM_CP:
		(void) fputs(" cp ", out);
		zm_vis_name(m, out, arg);
		break;
	case ZM_WRITE:
		(void) fputs(" write ", out);
		zm_vis_name(m, out, arg);
		break;
	case ZM_CONFLICT:
		(void) fprintf(out, " conflict %u", cnum);
		break;
	default:
		break;
	}
}

/* The last component of one node's name, escaped. */
static void
zm_emit_leaf(const struct zm *m, FILE *out, uint32_t k)
{
	const char *leaf, *p;
	size_t leaflen = 0, len = 0;

	p = zr_names_str(m->zm_ns, m->zm_nodes[k].zn_name, &len);
	if (p == NULL) {
		(void) fputc('?', out);
		return;
	}
	leaf = zm_leaf(p, len, &leaflen);
	zm_vis(m, out, leaf, leaflen);
}

static void
zm_emit_line(const struct zm *m, FILE *out, uint32_t k, uint32_t depth)
{
	const struct zm_node *nd = &m->zm_nodes[k];

	zm_indent(out, depth);
	if (k == 0) {
		(void) fputc('/', out);
	} else {
		zm_emit_leaf(m, out, k);
		if (nd->zn_dir != 0)
			(void) fputc('/', out);
	}
	zm_emit_act(m, out, nd->zn_act, nd->zn_arg, nd->zn_cnum);
	(void) fputc('\n', out);
}

/*
 * The second line of a type change: the same name once more, this
 * time with no trailing slash, since what takes the name now is not
 * the directory whose close this line follows.
 */
static void
zm_emit_create(const struct zm *m, FILE *out, uint32_t k, uint32_t depth)
{
	const struct zm_node *nd = &m->zm_nodes[k];

	zm_indent(out, depth);
	zm_emit_leaf(m, out, k);
	zm_emit_act(m, out, nd->zn_act2, nd->zn_arg2, 0);
	(void) fputc('\n', out);
}

static uint32_t
zm_shown(const struct zm *m, uint32_t c)
{
	while (c != ZM_NONE && m->zm_nodes[c].zn_show == 0)
		c = m->zm_nodes[c].zn_next;
	return (c);
}

/*
 * The tree section: a directory's line, then its listed children one
 * indent deeper, then two dots at the children's indentation. The
 * root is the line "/" and its two dots end the section.
 */
static void
zm_emit_tree(const struct zm *m, FILE *out)
{
	uint32_t cur = 0, depth = 0, next;
	int down = 1;

	for (;;) {
		if (down != 0) {
			zm_emit_line(m, out, cur, depth);
			if (m->zm_nodes[cur].zn_dir != 0) {
				next = zm_shown(m, m->zm_nodes[cur].zn_first);
				if (next != ZM_NONE) {
					cur = next;
					depth++;
					continue;
				}
			}
		}
		if (m->zm_nodes[cur].zn_dir != 0) {
			zm_indent(out, depth + 1);
			(void) fputs("..\n", out);
		}
		if (m->zm_nodes[cur].zn_act2 != ZM_NOTHING)
			zm_emit_create(m, out, cur, depth);
		if (cur == 0)
			break;
		next = zm_shown(m, m->zm_nodes[cur].zn_next);
		if (next != ZM_NONE) {
			cur = next;
			down = 1;
			continue;
		}
		cur = m->zm_nodes[cur].zn_parent;
		depth--;
		down = 0;
	}
}

static int
zm_letters_alloc(struct zm *m)
{
	uint32_t g, max = 1, n;

	for (g = 0; g < m->zm_d->zd_ngroups; g++) {
		const struct zr_group *gp = &m->zm_d->zd_groups[g];

		n = gp->zg_nbase + gp->zg_nfrom + gp->zg_nonto;
		if (n > max)
			max = n;
	}
	m->zm_maxletter = max;
	m->zm_letter = malloc((size_t)max * sizeof (uint32_t));
	return (m->zm_letter != NULL ? 0 : -1);
}

/*
 * The letter a content handle wears inside one record: assigned on
 * first appearance, and none at all for a pool with no content.
 */
static char
zm_letter_for(struct zm *m, uint32_t content)
{
	uint32_t i;

	if (content == ZR_CONTENT_NONE)
		return ('\0');
	for (i = 0; i < m->zm_nletter; i++)
		if (m->zm_letter[i] == content)
			break;
	if (i == m->zm_nletter && m->zm_nletter < m->zm_maxletter)
		m->zm_letter[m->zm_nletter++] = content;
	if (i < sizeof (zm_alphabet) - 1)
		return (zm_alphabet[i]);
	return ('?');
}

/* A pool's names in manifest order, by insertion sort; pools are small. */
static void
zm_sort_names(const struct zm *m, const zr_name_t *in, uint32_t n,
    zr_name_t *out)
{
	uint32_t i, j;

	for (i = 0; i < n; i++) {
		zr_name_t v = in[i];
		uint32_t o = zm_ord(m, v);

		for (j = i; j > 0 && zm_ord(m, out[j - 1]) > o; j--)
			out[j] = out[j - 1];
		out[j] = v;
	}
}

/* One tree line of a record: {names}letter, comma separated. */
static void
zm_emit_pools(struct zm *m, FILE *out, int tree, const zr_pool_t *pools,
    uint32_t npools)
{
	const struct zr_tree *t = m->zm_t[tree];
	uint32_t done = 0, i, j, maxn = 1;
	zr_name_t *sorted;
	char c;

	for (i = 0; i < npools; i++)
		if (pools[i] < t->zt_npools &&
		    t->zt_pools[pools[i]].zp_nnames > maxn)
			maxn = t->zt_pools[pools[i]].zp_nnames;
	/* on allocation failure the names go out in id order, never lost */
	sorted = malloc((size_t)maxn * sizeof (*sorted));
	(void) fputc('(', out);
	for (i = 0; i < npools; i++) {
		const struct zr_pool *p;

		if (pools[i] >= t->zt_npools)
			continue;
		p = &t->zt_pools[pools[i]];
		if (done++ > 0)
			(void) fputc(',', out);
		(void) fputc('{', out);
		if (sorted != NULL)
			zm_sort_names(m, p->zp_names, p->zp_nnames, sorted);
		for (j = 0; j < p->zp_nnames; j++) {
			if (j > 0)
				(void) fputc(' ', out);
			zm_vis_name(m, out, sorted != NULL ? sorted[j] :
			    p->zp_names[j]);
		}
		(void) fputc('}', out);
		c = zm_letter_for(m, p->zp_content);
		if (c != '\0')
			(void) fputc(c, out);
	}
	(void) fputc(')', out);
	free(sorted);
}

static void
zm_emit_record(struct zm *m, FILE *out, uint32_t num, uint32_t g)
{
	const struct zr_group *gp = &m->zm_d->zd_groups[g];
	uint32_t first = ZM_NONE, i, nclass = 0;

	(void) fprintf(out, "conflict %u ", num);
	for (i = 0; i < ZM_NCLASS; i++) {
		if ((gp->zg_flags & zm_class[i]) == 0)
			continue;
		if (nclass > 0)
			(void) fputc(',', out);
		(void) fputs(zr_conflict_name(zm_class[i]), out);
		if (first == ZM_NONE)
			first = i;
		nclass++;
	}
	(void) fputc('\n', out);
	if (first == ZM_NONE)
		first = ZM_NCLASS - 1;
	(void) fputs("  why  ", out);
	zm_vis_name(m, out, gp->zg_why[0]);
	if (zm_why_text[first].zs_mid != NULL) {
		(void) fputs(zm_why_text[first].zs_mid, out);
		zm_vis_name(m, out, gp->zg_why[1]);
	}
	(void) fputs(zm_why_text[first].zs_end, out);
	(void) fputc('\n', out);
	m->zm_nletter = 0;
	(void) fputs("  base ", out);
	zm_emit_pools(m, out, ZM_BASE, gp->zg_base, gp->zg_nbase);
	(void) fputs("\n  from ", out);
	zm_emit_pools(m, out, ZM_FROM, gp->zg_from, gp->zg_nfrom);
	(void) fputs("\n  onto ", out);
	zm_emit_pools(m, out, ZM_ONTO, gp->zg_onto, gp->zg_nonto);
	(void) fputc('\n', out);
}

static void
zm_fini(struct zm *m)
{
	free(m->zm_nodes);
	free(m->zm_node_of);
	free(m->zm_order);
	free(m->zm_anchor);
	free(m->zm_keeps);
	free(m->zm_ogroup);
	free(m->zm_cnum);
	free(m->zm_corder);
	free(m->zm_letter);
	free(m->zm_scr);
	memset(m, 0, sizeof (*m));
}

static const char *
zm_text(const char *s)
{
	return (s != NULL ? s : "");
}

int
zr_manifest_emit(FILE *out, const struct zr_manifest_hdr *hdr,
    const struct zr_tree *base, const struct zr_tree *from,
    const struct zr_tree *onto, const struct zr_decision *d)
{
	struct zm m;
	uint32_t i, nactions;
	int rc = -1;

	if (out == NULL || hdr == NULL || base == NULL || from == NULL ||
	    onto == NULL || d == NULL || d->zd_state == NULL)
		return (-1);
	memset(&m, 0, sizeof (m));
	m.zm_t[ZM_BASE] = base;
	m.zm_t[ZM_FROM] = from;
	m.zm_t[ZM_ONTO] = onto;
	m.zm_d = d;
	m.zm_ns = onto->zt_names;
	m.zm_nnames = d->zd_nnames;
	if (zm_scratch(&m) != 0 || zm_build(&m) != 0)
		goto done;
	m.zm_order = malloc((size_t)m.zm_nnodes * sizeof (uint32_t));
	m.zm_cnum = malloc(((size_t)d->zd_ngroups + 1) * sizeof (uint32_t));
	m.zm_corder = malloc(((size_t)d->zd_ngroups + 2) * sizeof (uint32_t));
	if (m.zm_order == NULL || m.zm_cnum == NULL || m.zm_corder == NULL)
		goto done;
	zm_walk_order(&m);
	if (zm_ogroups(&m) != 0 || zm_anchors(&m) != 0 ||
	    zm_letters_alloc(&m) != 0)
		goto done;
	zm_types(&m);
	for (i = 0; i < m.zm_nnodes; i++)
		zm_action_of(&m, i);
	zm_show(&m);
	nactions = zm_number(&m);

	(void) fputs("#rebase-manifest 4\n", out);
	(void) fprintf(out, "#base %s\n", zm_text(hdr->base));
	(void) fprintf(out, "#from %s\n", zm_text(hdr->from));
	(void) fprintf(out, "#onto %s\n", zm_text(hdr->onto));
	(void) fprintf(out, "#mode %s\n",
	    hdr->mode == ZR_MODE_PERMISSIVE ? "permissive-merge" : "strict");
	(void) fprintf(out, "#actions %u\n", nactions);
	(void) fprintf(out, "#conflicts %u\n", m.zm_nconf);
	zm_emit_tree(&m, out);
	if (m.zm_nconf > 0) {
		(void) fputs("\n# a pool is one file and all its names: "
		    "{names}letter; same\n", out);
		(void) fputs("# letter, same bytes\n", out);
	}
	for (i = 1; i <= m.zm_nconf; i++)
		zm_emit_record(&m, out, i, m.zm_corder[i]);
	rc = (ferror(out) != 0 || m.zm_err != 0) ? -1 : 0;
done:
	zm_fini(&m);
	return (rc);
}

/*
 * The parser, the emitter read backwards. The tree section is scoped,
 * so the enclosing directories are an explicit stack of decoded paths
 * and a name line's absolute path is that stack and its own name:
 * nothing here recurses either. Indentation is read past; the stack is
 * what says where a line sits. Every rejection names its line.
 */

/* One open directory of the tree section. */
struct zp_scope {
	unsigned char	*zs_path;
	size_t		zs_len;
	int		zs_rm;		/* its line carried an rm */
};

/* One name line, kept so that a name repeated can be found. */
struct zp_seen {
	unsigned char	*zn_path;
	size_t		zn_len;
	uint32_t	zn_line;
	int		zn_second;	/* the create of a type change */
};

/* One conflict mark, kept until the records behind it are known. */
struct zp_ref {
	uint32_t	zf_num;
	uint32_t	zf_line;
};

/* Everything one parse run carries. */
struct zp {
	FILE			*zp_in;
	struct zr_parsed	*zp_out;
	char			*zp_err;
	size_t			zp_errlen;
	char			*zp_line;	/* the line just read */
	size_t			zp_cap;
	size_t			zp_len;
	uint32_t		zp_lineno;
	struct zp_scope		*zp_stack;
	uint32_t		zp_depth;
	uint32_t		zp_scap;
	unsigned char		*zp_closed;	/* what the last .. closed */
	size_t			zp_closedlen;
	int			zp_closed_rm;
	struct zp_seen		*zp_seen;
	uint32_t		zp_nseen;
	uint32_t		zp_ncap;
	uint32_t		zp_acap;
	uint32_t		zp_rcap;
	struct zp_ref		*zp_refs;
	uint32_t		zp_nrefs;
	uint32_t		zp_fcap;
	uint32_t		zp_aline;	/* where #actions sits */
	uint32_t		zp_cline;	/* where #conflicts sits */
};

/*
 * Every rejection goes through here, so that every message names the
 * line the reader must go and look at.
 */
static int
zp_errf(struct zp *p, const char *fmt, ...)
{
	va_list ap;
	char msg[192];

	va_start(ap, fmt);
	(void) vsnprintf(msg, sizeof (msg), fmt, ap);
	va_end(ap);
	if (p->zp_err != NULL && p->zp_errlen > 0)
		(void) snprintf(p->zp_err, p->zp_errlen, "line %u: %s",
		    p->zp_lineno, msg);
	return (-1);
}

/* One line without its newline. Returns 1, 0 at the end, -1 on failure. */
static int
zp_readline(struct zp *p)
{
	int c = 0;

	p->zp_len = 0;
	for (;;) {
		c = fgetc(p->zp_in);
		if (c == EOF)
			break;
		if (p->zp_len + 2 > p->zp_cap) {
			size_t cap = p->zp_cap != 0 ? p->zp_cap * 2 : 256;
			char *nb = realloc(p->zp_line, cap);

			if (nb == NULL)
				return (-1);
			p->zp_line = nb;
			p->zp_cap = cap;
		}
		if (c == '\n')
			break;
		p->zp_line[p->zp_len++] = (char)c;
	}
	if (c == EOF && p->zp_len == 0)
		return (0);
	p->zp_line[p->zp_len] = '\0';
	p->zp_lineno++;
	return (1);
}

/* The line just read, without its leading and trailing blanks. */
static void
zp_trim(const struct zp *p, const char **s, size_t *len)
{
	size_t a = 0, b = p->zp_len;

	while (a < b && (p->zp_line[a] == ' ' || p->zp_line[a] == '\t'))
		a++;
	while (b > a && (p->zp_line[b - 1] == ' ' ||
	    p->zp_line[b - 1] == '\t'))
		b--;
	*s = p->zp_line + a;
	*len = b - a;
}

/*
 * The next line that says something. Blank lines are skipped anywhere,
 * and comments too for every caller but the one reading the header.
 * Returns 1, 0 at the end of input, -1 on failure.
 */
static int
zp_next(struct zp *p, int skipcomment, const char **s, size_t *len)
{
	int rc;

	for (;;) {
		rc = zp_readline(p);
		if (rc < 0)
			return (zp_errf(p, "out of memory"));
		if (rc == 0)
			return (0);
		zp_trim(p, s, len);
		if (*len == 0)
			continue;
		if (skipcomment != 0 && **s == '#')
			continue;
		return (1);
	}
}

/* The next blank separated field of a line, or 0 at its end. */
static int
zp_field(const char *s, size_t len, size_t *pos, const char **f, size_t *flen)
{
	size_t i = *pos;

	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	if (i >= len)
		return (0);
	*f = s + i;
	while (i < len && s[i] != ' ' && s[i] != '\t')
		i++;
	*flen = (size_t)(s + i - *f);
	*pos = i;
	return (1);
}

/* A copy of one field as a terminated string. */
static char *
zp_dup(const char *s, size_t len)
{
	char *d = malloc(len + 1);

	if (d != NULL) {
		memcpy(d, s, len);
		d[len] = '\0';
	}
	return (d);
}

/*
 * Decode one escaped field into bytes of its own. A decoding is never
 * longer than its text, so one buffer of that size always holds it.
 */
static int
zp_decode(struct zp *p, const char *s, size_t len, const char *what,
    unsigned char **out, size_t *outlen)
{
	unsigned char *buf;
	size_t n = 0;

	buf = malloc(len + 1);
	if (buf == NULL)
		return (zp_errf(p, "out of memory"));
	if (zr_vis_decode(s, len, buf, len + 1, &n) != 0) {
		free(buf);
		return (zp_errf(p, "bad escape in the %s", what));
	}
	buf[n] = '\0';
	*out = buf;
	*outlen = n;
	return (0);
}

/* A count or a record number: decimal digits, and never overlong. */
static int
zp_uint(const char *s, size_t len, uint32_t *out)
{
	uint32_t v = 0;
	size_t i;

	if (len == 0 || len > 9)
		return (-1);
	for (i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9')
			return (-1);
		v = v * 10 + (uint32_t)(s[i] - '0');
	}
	*out = v;
	return (0);
}

/*
 * Manifest order over two absolute paths: component by component as
 * bytes, a parent before its children. The slash ranks below every
 * other byte, which is what puts /a/b before /a-1.
 */
static int
zp_path_cmp(const unsigned char *a, size_t alen, const unsigned char *b,
    size_t blen)
{
	size_t i, n = alen < blen ? alen : blen;

	for (i = 0; i < n; i++) {
		int x = a[i] == '/' ? -1 : (int)a[i];
		int y = b[i] == '/' ? -1 : (int)b[i];

		if (x != y)
			return (x < y ? -1 : 1);
	}
	if (alen != blen)
		return (alen < blen ? -1 : 1);
	return (0);
}

/* The absolute path of a name line: its scope and its own name. */
static int
zp_join(struct zp *p, const unsigned char *dir, size_t dirlen,
    const unsigned char *name, size_t namelen, unsigned char **out,
    size_t *outlen)
{
	size_t n = dirlen == 1 ? 0 : dirlen;
	unsigned char *buf;

	buf = malloc(n + namelen + 2);
	if (buf == NULL)
		return (zp_errf(p, "out of memory"));
	if (n > 0)
		memcpy(buf, dir, n);
	buf[n] = '/';
	memcpy(buf + n + 1, name, namelen);
	buf[n + namelen + 1] = '\0';
	*out = buf;
	*outlen = n + namelen + 1;
	return (0);
}

/* Open one directory; the stack owns its copy of the path. */
static int
zp_push(struct zp *p, const unsigned char *path, size_t len, int isrm)
{
	unsigned char *copy;

	if (p->zp_depth == p->zp_scap) {
		uint32_t cap = p->zp_scap != 0 ? p->zp_scap * 2 : 16;
		struct zp_scope *ns;

		ns = realloc(p->zp_stack, (size_t)cap * sizeof (*ns));
		if (ns == NULL)
			return (zp_errf(p, "out of memory"));
		p->zp_stack = ns;
		p->zp_scap = cap;
	}
	copy = malloc(len + 1);
	if (copy == NULL)
		return (zp_errf(p, "out of memory"));
	memcpy(copy, path, len);
	copy[len] = '\0';
	p->zp_stack[p->zp_depth].zs_path = copy;
	p->zp_stack[p->zp_depth].zs_len = len;
	p->zp_stack[p->zp_depth].zs_rm = isrm;
	p->zp_depth++;
	return (0);
}

/* Take one action; the parse owns its paths from here on. */
static int
zp_add_action(struct zp *p, struct zr_action *a)
{
	struct zr_parsed *o = p->zp_out;

	if (o->zp_nactions == p->zp_acap) {
		uint32_t cap = p->zp_acap != 0 ? p->zp_acap * 2 : 16;
		struct zr_action *na;

		na = realloc(o->zp_actions, (size_t)cap * sizeof (*na));
		if (na == NULL) {
			free(a->za_path);
			free(a->za_arg);
			return (zp_errf(p, "out of memory"));
		}
		o->zp_actions = na;
		p->zp_acap = cap;
	}
	o->zp_actions[o->zp_nactions++] = *a;
	return (0);
}

/* Keep one name line, to answer later whether a name came twice. */
static int
zp_add_seen(struct zp *p, const unsigned char *path, size_t len,
    int second)
{
	unsigned char *copy;

	if (p->zp_nseen == p->zp_ncap) {
		uint32_t cap = p->zp_ncap != 0 ? p->zp_ncap * 2 : 32;
		struct zp_seen *ns;

		ns = realloc(p->zp_seen, (size_t)cap * sizeof (*ns));
		if (ns == NULL)
			return (zp_errf(p, "out of memory"));
		p->zp_seen = ns;
		p->zp_ncap = cap;
	}
	copy = malloc(len + 1);
	if (copy == NULL)
		return (zp_errf(p, "out of memory"));
	memcpy(copy, path, len);
	copy[len] = '\0';
	p->zp_seen[p->zp_nseen].zn_path = copy;
	p->zp_seen[p->zp_nseen].zn_len = len;
	p->zp_seen[p->zp_nseen].zn_line = p->zp_lineno;
	p->zp_seen[p->zp_nseen].zn_second = second;
	p->zp_nseen++;
	return (0);
}

/* Take one conflict mark, with the line that will be blamed for it. */
static int
zp_add_ref(struct zp *p, uint32_t num)
{
	if (p->zp_nrefs == p->zp_fcap) {
		uint32_t cap = p->zp_fcap != 0 ? p->zp_fcap * 2 : 16;
		struct zp_ref *nr;

		nr = realloc(p->zp_refs, (size_t)cap * sizeof (*nr));
		if (nr == NULL)
			return (zp_errf(p, "out of memory"));
		p->zp_refs = nr;
		p->zp_fcap = cap;
	}
	p->zp_refs[p->zp_nrefs].zf_num = num;
	p->zp_refs[p->zp_nrefs].zf_line = p->zp_lineno;
	p->zp_nrefs++;
	return (0);
}

/* Take one conflict record. */
static int
zp_add_record(struct zp *p, const struct zr_record *r)
{
	struct zr_parsed *o = p->zp_out;

	if (o->zp_nrecords == p->zp_rcap) {
		uint32_t cap = p->zp_rcap != 0 ? p->zp_rcap * 2 : 8;
		struct zr_record *nr;

		nr = realloc(o->zp_records, (size_t)cap * sizeof (*nr));
		if (nr == NULL)
			return (zp_errf(p, "out of memory"));
		o->zp_records = nr;
		p->zp_rcap = cap;
	}
	o->zp_records[o->zp_nrecords++] = *r;
	return (0);
}

/* The six header keys, in the order the format writes them. */
static const char *const zp_hdrkey[] = {
	"#base", "#from", "#onto", "#mode", "#actions", "#conflicts"
};

#define	ZP_NHDR		(sizeof (zp_hdrkey) / sizeof (zp_hdrkey[0]))

/* One header line's value: three names, the mode and the two counts. */
static int
zp_header_value(struct zp *p, uint32_t which, const char *v, size_t vlen)
{
	struct zr_parsed *o = p->zp_out;

	switch (which) {
	case 0:
		o->zp_base = zp_dup(v, vlen);
		return (o->zp_base != NULL ? 0 : zp_errf(p, "out of memory"));
	case 1:
		o->zp_from = zp_dup(v, vlen);
		return (o->zp_from != NULL ? 0 : zp_errf(p, "out of memory"));
	case 2:
		o->zp_onto = zp_dup(v, vlen);
		return (o->zp_onto != NULL ? 0 : zp_errf(p, "out of memory"));
	case 3:
		if (vlen == 6 && memcmp(v, "strict", 6) == 0)
			o->zp_mode = ZR_MODE_STRICT;
		else if (vlen == 16 && memcmp(v, "permissive-merge", 16) == 0)
			o->zp_mode = ZR_MODE_PERMISSIVE;
		else
			return (zp_errf(p, "the mode is strict or "
			    "permissive-merge"));
		return (0);
	case 4:
		p->zp_aline = p->zp_lineno;
		if (zp_uint(v, vlen, &o->zp_actions_declared) != 0)
			return (zp_errf(p, "#actions wants a count"));
		return (0);
	default:
		p->zp_cline = p->zp_lineno;
		if (zp_uint(v, vlen, &o->zp_conflicts_declared) != 0)
			return (zp_errf(p, "#conflicts wants a count"));
		return (0);
	}
}

/*
 * The version line, which is the first line of the file and nothing
 * else, then the six headers in order. Any other line beginning with a
 * hash between them is a comment and is passed over.
 */
static int
zp_header(struct zp *p)
{
	const char *s = NULL;
	size_t klen, len = 0;
	uint32_t i;
	int rc;

	rc = zp_readline(p);
	if (rc < 0)
		return (zp_errf(p, "out of memory"));
	if (rc == 0)
		return (zp_errf(p, "expected #rebase-manifest 4"));
	zp_trim(p, &s, &len);
	if (len != 18 || memcmp(s, "#rebase-manifest 4", 18) != 0)
		return (zp_errf(p, "expected #rebase-manifest 4"));
	for (i = 0; i < ZP_NHDR; i++) {
		klen = strlen(zp_hdrkey[i]);
		for (;;) {
			rc = zp_next(p, 0, &s, &len);
			if (rc < 0)
				return (-1);
			if (rc == 0)
				return (zp_errf(p, "expected %s",
				    zp_hdrkey[i]));
			if (len > klen && s[klen] == ' ' &&
			    memcmp(s, zp_hdrkey[i], klen) == 0)
				break;
			if (*s != '#')
				return (zp_errf(p, "expected %s",
				    zp_hdrkey[i]));
		}
		if (zp_header_value(p, i, s + klen + 1,
		    len - klen - 1) != 0)
			return (-1);
	}
	return (0);
}

/*
 * One name line: NAME, then the action it carries and that action's
 * one argument. Its path is the open directories and this name, all of
 * it decoded, and an ln may only name a path the walk has passed.
 */
static int
zp_name_line(struct zp *p, const char *s, size_t len)
{
	struct zr_action a;
	unsigned char *name = NULL, *path = NULL;
	const char *f0 = NULL, *f1 = NULL, *f2 = NULL, *f3 = NULL;
	size_t f0len = 0, f1len = 0, f2len = 0, f3len = 0;
	size_t namelen = 0, pathlen = 0, pos = 0;
	int isdir = 0, hasarg = 0, hasact = 1, second = 0, rc;

	memset(&a, 0, sizeof (a));
	if (zp_field(s, len, &pos, &f0, &f0len) == 0)
		return (zp_errf(p, "a name line needs a name"));
	if (f0[f0len - 1] == '/') {
		isdir = 1;
		f0len--;
	}
	if (f0len == 0)
		return (zp_errf(p, "a name line needs a name"));
	if (zp_decode(p, f0, f0len, "name", &name, &namelen) != 0)
		return (-1);
	if (zp_join(p, p->zp_stack[p->zp_depth - 1].zs_path,
	    p->zp_stack[p->zp_depth - 1].zs_len, name, namelen, &path,
	    &pathlen) != 0) {
		free(name);
		return (-1);
	}
	free(name);
	if (zp_field(s, len, &pos, &f1, &f1len) == 0)
		hasact = 0;
	else if (f1len == 2 && memcmp(f1, "rm", 2) == 0)
		a.za_kind = ZR_ACT_RM;
	else if (f1len == 2 && memcmp(f1, "ln", 2) == 0)
		a.za_kind = ZR_ACT_LN;
	else if (f1len == 2 && memcmp(f1, "cp", 2) == 0)
		a.za_kind = ZR_ACT_CP;
	else if (f1len == 3 && memcmp(f1, "dup", 3) == 0)
		a.za_kind = ZR_ACT_DUP;
	else if (f1len == 5 && memcmp(f1, "write", 5) == 0)
		a.za_kind = ZR_ACT_WRITE;
	else if (f1len == 8 && memcmp(f1, "conflict", 8) == 0)
		a.za_kind = ZR_ACT_CONFLICT;
	else {
		free(path);
		return (zp_errf(p, "unknown action \"%.*s\"", (int)f1len,
		    f1));
	}
	if (hasact != 0)
		hasarg = zp_field(s, len, &pos, &f2, &f2len);
	if (hasact == 0) {
		if (isdir == 0) {
			free(path);
			return (zp_errf(p, "a name with no action needs a "
			    "trailing slash"));
		}
	} else if (a.za_kind == ZR_ACT_RM) {
		if (hasarg != 0) {
			free(path);
			return (zp_errf(p, "rm takes no argument"));
		}
	} else if (a.za_kind == ZR_ACT_CONFLICT) {
		if (hasarg == 0 || zp_uint(f2, f2len, &a.za_conflict) != 0 ||
		    a.za_conflict == 0) {
			free(path);
			return (zp_errf(p, "conflict wants a record number"));
		}
	} else {
		if (hasarg == 0) {
			free(path);
			return (zp_errf(p, "%.*s wants a path", (int)f1len,
			    f1));
		}
		if (f2[0] != '/') {
			free(path);
			return (zp_errf(p, "the path must be absolute"));
		}
		if (zp_decode(p, f2, f2len, "path", &a.za_arg,
		    &a.za_arglen) != 0) {
			free(path);
			return (-1);
		}
	}
	if (zp_field(s, len, &pos, &f3, &f3len) != 0) {
		free(path);
		free(a.za_arg);
		return (zp_errf(p, "too many fields on a name line"));
	}
	if (a.za_kind == ZR_ACT_LN && hasact != 0 &&
	    zp_path_cmp(a.za_arg, a.za_arglen, path, pathlen) >= 0) {
		free(path);
		free(a.za_arg);
		return (zp_errf(p, "the ln target is not earlier in manifest "
		    "order"));
	}
	if (p->zp_closed != NULL && p->zp_closed_rm != 0 && isdir == 0 &&
	    hasact != 0 && (a.za_kind == ZR_ACT_CP ||
	    a.za_kind == ZR_ACT_DUP || a.za_kind == ZR_ACT_LN) &&
	    p->zp_closedlen == pathlen &&
	    memcmp(p->zp_closed, path, pathlen) == 0)
		second = 1;
	free(p->zp_closed);
	p->zp_closed = NULL;
	if (zp_add_seen(p, path, pathlen, second) != 0) {
		free(path);
		free(a.za_arg);
		return (-1);
	}
	a.za_path = path;
	a.za_pathlen = pathlen;
	a.za_isdir = isdir;
	if (hasact != 0) {
		if (a.za_kind == ZR_ACT_CONFLICT &&
		    zp_add_ref(p, a.za_conflict) != 0) {
			free(path);
			return (-1);
		}
		if (zp_add_action(p, &a) != 0)
			return (-1);
	}
	rc = isdir != 0 ? zp_push(p, path, pathlen,
	    hasact != 0 && a.za_kind == ZR_ACT_RM) : 0;
	if (hasact == 0)
		free(path);
	return (rc);
}

/*
 * The tree section: the root line, then name lines and the two dots
 * that close each open directory, until the two dots that close the
 * root and end the section.
 */
static int
zp_tree(struct zp *p)
{
	const char *s = NULL;
	size_t len = 0;
	int rc;

	rc = zp_next(p, 1, &s, &len);
	if (rc < 0)
		return (-1);
	if (rc == 0 || len != 1 || *s != '/')
		return (zp_errf(p, "expected the root line \"/\""));
	if (zp_push(p, (const unsigned char *)"/", 1, 0) != 0)
		return (-1);
	for (;;) {
		rc = zp_next(p, 1, &s, &len);
		if (rc < 0)
			return (-1);
		if (rc == 0)
			return (zp_errf(p, "end of input inside the tree "
			    "section"));
		if (len == 2 && s[0] == '.' && s[1] == '.') {
			p->zp_depth--;
			free(p->zp_closed);
			p->zp_closed = p->zp_stack[p->zp_depth].zs_path;
			p->zp_closedlen = p->zp_stack[p->zp_depth].zs_len;
			p->zp_closed_rm = p->zp_stack[p->zp_depth].zs_rm;
			p->zp_stack[p->zp_depth].zs_path = NULL;
			if (p->zp_depth == 0)
				return (0);
			continue;
		}
		if (zp_name_line(p, s, len) != 0)
			return (-1);
	}
}

/* The bit one class name stands for, or 0 if it names no class. */
static uint32_t
zp_class(const char *s, size_t len)
{
	uint32_t i;

	for (i = 0; i < ZM_NCLASS; i++) {
		const char *nm = zr_conflict_name(zm_class[i]);

		if (strlen(nm) == len && memcmp(nm, s, len) == 0)
			return (zm_class[i]);
	}
	return (0);
}

/* A record's class list: class names separated by commas. */
static int
zp_classes(struct zp *p, const char *s, size_t len, uint32_t *flags)
{
	size_t a = 0, b;
	uint32_t bit;

	*flags = 0;
	for (;;) {
		for (b = a; b < len && s[b] != ','; b++)
			continue;
		bit = zp_class(s + a, b - a);
		if (bit == 0)
			return (zp_errf(p, "unknown class \"%.*s\"",
			    (int)(b - a), s + a));
		*flags |= bit;
		if (b >= len)
			return (0);
		a = b + 1;
	}
}

/* One of a record's four indented lines, kept as the text it holds. */
static int
zp_record_line(struct zp *p, const char *key, char **out)
{
	const char *s = NULL;
	size_t klen = strlen(key), len = 0, pos;
	int rc;

	rc = zp_next(p, 1, &s, &len);
	if (rc < 0)
		return (-1);
	if (rc == 0 || len < klen || memcmp(s, key, klen) != 0 ||
	    (len > klen && s[klen] != ' ' && s[klen] != '\t'))
		return (zp_errf(p, "expected the %s line of a record", key));
	pos = klen;
	while (pos < len && (s[pos] == ' ' || s[pos] == '\t'))
		pos++;
	*out = zp_dup(s + pos, len - pos);
	return (*out != NULL ? 0 : zp_errf(p, "out of memory"));
}

/*
 * The conflict section: one line naming a record's number and classes,
 * then its four lines in order. The records are numbered from 1 in the
 * order the tree section first named them, so they must arrive so.
 */
static int
zp_records(struct zp *p)
{
	struct zr_record r;
	const char *s = NULL, *f = NULL;
	size_t len = 0, flen = 0, pos;
	int rc;

	for (;;) {
		rc = zp_next(p, 1, &s, &len);
		if (rc < 0)
			return (-1);
		if (rc == 0)
			return (0);
		if (len == 2 && s[0] == '.' && s[1] == '.')
			return (zp_errf(p, "two dots outside the tree "
			    "section"));
		memset(&r, 0, sizeof (r));
		pos = 0;
		(void) zp_field(s, len, &pos, &f, &flen);
		if (flen != 8 || memcmp(f, "conflict", 8) != 0)
			return (zp_errf(p, "expected a conflict record"));
		if (zp_field(s, len, &pos, &f, &flen) == 0 ||
		    zp_uint(f, flen, &r.zr_num) != 0 ||
		    r.zr_num != p->zp_out->zp_nrecords + 1)
			return (zp_errf(p, "expected conflict %u",
			    p->zp_out->zp_nrecords + 1));
		if (zp_field(s, len, &pos, &f, &flen) == 0)
			return (zp_errf(p, "a record needs a class"));
		if (zp_classes(p, f, flen, &r.zr_flags) != 0)
			return (-1);
		if (zp_field(s, len, &pos, &f, &flen) != 0)
			return (zp_errf(p, "too many fields on a record"));
		if (zp_record_line(p, "why", &r.zr_why) != 0 ||
		    zp_record_line(p, "base", &r.zr_base) != 0 ||
		    zp_record_line(p, "from", &r.zr_from) != 0 ||
		    zp_record_line(p, "onto", &r.zr_onto) != 0) {
			free(r.zr_why);
			free(r.zr_base);
			free(r.zr_from);
			free(r.zr_onto);
			return (-1);
		}
		if (zp_add_record(p, &r) != 0) {
			free(r.zr_why);
			free(r.zr_base);
			free(r.zr_from);
			free(r.zr_onto);
			return (-1);
		}
	}
}

/* Repeated names sort together, and the later line comes second. */
static int
zp_seen_cmp(const void *a, const void *b)
{
	const struct zp_seen *x = a, *y = b;
	int r;

	r = zp_path_cmp(x->zn_path, x->zn_len, y->zn_path, y->zn_len);
	if (r != 0)
		return (r);
	return (x->zn_line < y->zn_line ? -1 : (x->zn_line > y->zn_line));
}

/*
 * A name appears in the tree section once, with one exception: a
 * type change writes a directory rm line, its scope, and then a leaf
 * line taking the same name with a cp or an ln. Every other repeat
 * is an error, blamed on the earliest line that repeats a name.
 */
static int
zp_repeats(struct zp *p)
{
	uint32_t bad = 0, i;

	if (p->zp_nseen < 2)
		return (0);
	qsort(p->zp_seen, p->zp_nseen, sizeof (*p->zp_seen),
	    zp_seen_cmp);
	for (i = 1; i < p->zp_nseen; i++) {
		const struct zp_seen *x = &p->zp_seen[i - 1];
		const struct zp_seen *y = &p->zp_seen[i];

		if (y->zn_second != 0 || zp_path_cmp(x->zn_path,
		    x->zn_len, y->zn_path, y->zn_len) != 0)
			continue;
		if (bad == 0 || y->zn_line < bad)
			bad = y->zn_line;
	}
	if (bad == 0)
		return (0);
	p->zp_lineno = bad;
	return (zp_errf(p, "this name is already in the tree section"));
}

/*
 * What only the whole file can answer: the two counts the header
 * promised, and a record behind every conflict mark of the tree. The
 * blame goes to the line that made the promise.
 */
static int
zp_finish(struct zp *p)
{
	struct zr_parsed *o = p->zp_out;
	uint32_t i, n = 0;

	if (zp_repeats(p) != 0)
		return (-1);
	for (i = 0; i < o->zp_nactions; i++)
		if (o->zp_actions[i].za_kind != ZR_ACT_CONFLICT)
			n++;
	if (n != o->zp_actions_declared) {
		p->zp_lineno = p->zp_aline;
		return (zp_errf(p, "#actions says %u but the tree has %u",
		    o->zp_actions_declared, n));
	}
	if (o->zp_nrecords != o->zp_conflicts_declared) {
		p->zp_lineno = p->zp_cline;
		return (zp_errf(p, "#conflicts says %u but there are %u",
		    o->zp_conflicts_declared, o->zp_nrecords));
	}
	for (i = 0; i < p->zp_nrefs; i++) {
		if (p->zp_refs[i].zf_num <= o->zp_nrecords)
			continue;
		p->zp_lineno = p->zp_refs[i].zf_line;
		return (zp_errf(p, "no record for conflict %u",
		    p->zp_refs[i].zf_num));
	}
	return (0);
}

void
zr_parsed_fini(struct zr_parsed *p)
{
	uint32_t i;

	if (p == NULL)
		return;
	free(p->zp_base);
	free(p->zp_from);
	free(p->zp_onto);
	for (i = 0; i < p->zp_nactions; i++) {
		free(p->zp_actions[i].za_path);
		free(p->zp_actions[i].za_arg);
	}
	free(p->zp_actions);
	for (i = 0; i < p->zp_nrecords; i++) {
		free(p->zp_records[i].zr_why);
		free(p->zp_records[i].zr_base);
		free(p->zp_records[i].zr_from);
		free(p->zp_records[i].zr_onto);
	}
	free(p->zp_records);
	memset(p, 0, sizeof (*p));
}

int
zr_manifest_parse(FILE *in, struct zr_parsed *out, char *err, size_t errlen)
{
	struct zp p;
	uint32_t i;
	int rc;

	if (out == NULL)
		return (-1);
	memset(out, 0, sizeof (*out));
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (in == NULL)
		return (-1);
	memset(&p, 0, sizeof (p));
	p.zp_in = in;
	p.zp_out = out;
	p.zp_err = err;
	p.zp_errlen = errlen;
	rc = zp_header(&p);
	if (rc == 0)
		rc = zp_tree(&p);
	if (rc == 0)
		rc = zp_records(&p);
	if (rc == 0)
		rc = zp_finish(&p);
	while (p.zp_depth > 0)
		free(p.zp_stack[--p.zp_depth].zs_path);
	for (i = 0; i < p.zp_nseen; i++)
		free(p.zp_seen[i].zn_path);
	free(p.zp_seen);
	free(p.zp_closed);
	free(p.zp_stack);
	free(p.zp_refs);
	free(p.zp_line);
	return (rc);
}

/*
 * Writing a parsed manifest back out. The tree section is not kept as
 * a tree: the action paths are, and every directory the walk needs is
 * an ancestor of one of them. Sorting the paths in manifest order puts
 * them in the walk's own order, and one stack of open directories
 * turns that list back into the scoped form.
 */

/* One line of the tree section: an action's path, or a scope. */
struct zw_ent {
	const unsigned char	*ze_path;
	size_t			ze_len;
	uint32_t		ze_act;		/* index, or ZM_NONE */
	int			ze_dir;
	int			ze_dup;		/* the second of a pair */
};

/* By path, and a directory before the leaf that repeats its name. */
static int
zw_cmp(const void *a, const void *b)
{
	const struct zw_ent *x = a, *y = b;
	int r;

	r = zp_path_cmp(x->ze_path, x->ze_len, y->ze_path, y->ze_len);
	if (r != 0)
		return (r);
	return (y->ze_dir - x->ze_dir);
}

/*
 * The two lines one name holds in a type change: onto's directory
 * removed, and the leaf that takes the name after its close. That
 * pair is the one repeat the tree section keeps as two lines.
 */
static int
zw_pair(const struct zr_parsed *pp, const struct zw_ent *d,
    const struct zw_ent *e)
{
	const struct zr_action *a;

	if (d->ze_dir == 0 || d->ze_act == ZM_NONE || e->ze_dir != 0 ||
	    e->ze_act == ZM_NONE)
		return (0);
	if (pp->zp_actions[d->ze_act].za_kind != ZR_ACT_RM)
		return (0);
	a = &pp->zp_actions[e->ze_act];
	return (a->za_kind == ZR_ACT_CP || a->za_kind == ZR_ACT_DUP ||
	    a->za_kind == ZR_ACT_LN);
}

/* One line per path: fold the repeated ancestors into one entry. */
static uint32_t
zw_dedupe(const struct zr_parsed *pp, struct zw_ent *e, uint32_t n)
{
	uint32_t i, k = 0;

	for (i = 1; i < n; i++) {
		int same = zp_path_cmp(e[k].ze_path, e[k].ze_len,
		    e[i].ze_path, e[i].ze_len) == 0;

		if (same != 0 && zw_pair(pp, &e[k], &e[i]) == 0) {
			if (e[i].ze_dir != 0)
				e[k].ze_dir = 1;
			if (e[i].ze_act != ZM_NONE)
				e[k].ze_act = e[i].ze_act;
			continue;
		}
		k++;
		e[k] = e[i];
		e[k].ze_dup = same;
	}
	return (n != 0 ? k + 1 : 0);
}

/* Every line the tree section needs, in manifest order. */
static struct zw_ent *
zw_entries(const struct zr_parsed *pp, uint32_t *np)
{
	struct zw_ent *e;
	uint32_t i, n = 0;
	size_t cap = 1, j;

	for (i = 0; i < pp->zp_nactions; i++) {
		cap++;
		for (j = 1; j < pp->zp_actions[i].za_pathlen; j++)
			if (pp->zp_actions[i].za_path[j] == '/')
				cap++;
	}
	e = malloc(cap * sizeof (*e));
	if (e == NULL)
		return (NULL);
	e[n].ze_path = (const unsigned char *)"/";
	e[n].ze_len = 1;
	e[n].ze_act = ZM_NONE;
	e[n].ze_dir = 1;
	e[n].ze_dup = 0;
	n++;
	for (i = 0; i < pp->zp_nactions; i++) {
		const struct zr_action *a = &pp->zp_actions[i];

		for (j = 1; j < a->za_pathlen; j++) {
			if (a->za_path[j] != '/')
				continue;
			e[n].ze_path = a->za_path;
			e[n].ze_len = j;
			e[n].ze_act = ZM_NONE;
			e[n].ze_dir = 1;
			e[n].ze_dup = 0;
			n++;
		}
		e[n].ze_path = a->za_path;
		e[n].ze_len = a->za_pathlen;
		e[n].ze_act = i;
		e[n].ze_dir = a->za_isdir;
		e[n].ze_dup = 0;
		n++;
	}
	qsort(e, n, sizeof (*e), zw_cmp);
	*np = zw_dedupe(pp, e, n);
	return (e);
}

/* Does the directory d hold the line e, at any depth below it? */
static int
zw_under(const struct zw_ent *d, const struct zw_ent *e)
{
	if (e->ze_len <= d->ze_len ||
	    memcmp(d->ze_path, e->ze_path, d->ze_len) != 0)
		return (0);
	return (d->ze_len == 1 || e->ze_path[d->ze_len] == '/');
}

/* Bytes as the format escapes them, one byte's escaping at a time. */
static void
zw_vis(FILE *out, const unsigned char *p, size_t len)
{
	char buf[8];
	size_t i;

	for (i = 0; i < len; i++) {
		(void) zr_vis_encode(p + i, 1, buf, sizeof (buf));
		(void) fputs(buf, out);
	}
}

/* One tree line: the name, its trailing slash, and its action. */
static void
zw_line(FILE *out, const struct zr_parsed *pp, const struct zw_ent *e,
    uint32_t depth)
{
	const struct zr_action *a;
	const char *leaf;
	size_t leaflen = 0;

	zm_indent(out, depth);
	if (e->ze_len == 1) {
		(void) fputc('/', out);
	} else {
		leaf = zm_leaf((const char *)e->ze_path, e->ze_len, &leaflen);
		zw_vis(out, (const unsigned char *)leaf, leaflen);
		if (e->ze_dir != 0)
			(void) fputc('/', out);
	}
	if (e->ze_act != ZM_NONE) {
		a = &pp->zp_actions[e->ze_act];
		switch (a->za_kind) {
		case ZR_ACT_RM:
			(void) fputs(" rm", out);
			break;
		case ZR_ACT_LN:
			(void) fputs(" ln ", out);
			zw_vis(out, a->za_arg, a->za_arglen);
			break;
		case ZR_ACT_CP:
			(void) fputs(" cp ", out);
			zw_vis(out, a->za_arg, a->za_arglen);
			break;
		case ZR_ACT_DUP:
			(void) fputs(" dup ", out);
			zw_vis(out, a->za_arg, a->za_arglen);
			break;
		case ZR_ACT_WRITE:
			(void) fputs(" write ", out);
			zw_vis(out, a->za_arg, a->za_arglen);
			break;
		default:
			(void) fprintf(out, " conflict %u", a->za_conflict);
			break;
		}
	}
	(void) fputc('\n', out);
}

/* One conflict record, its classes back in the format's own order. */
static void
zw_record(FILE *out, const struct zr_record *r)
{
	uint32_t i, nclass = 0;

	(void) fprintf(out, "conflict %u ", r->zr_num);
	for (i = 0; i < ZM_NCLASS; i++) {
		if ((r->zr_flags & zm_class[i]) == 0)
			continue;
		if (nclass++ > 0)
			(void) fputc(',', out);
		(void) fputs(zr_conflict_name(zm_class[i]), out);
	}
	(void) fprintf(out, "\n  why  %s\n", zm_text(r->zr_why));
	(void) fprintf(out, "  base %s\n", zm_text(r->zr_base));
	(void) fprintf(out, "  from %s\n", zm_text(r->zr_from));
	(void) fprintf(out, "  onto %s\n", zm_text(r->zr_onto));
}

/* One open directory of the write walk, and what closing it says. */
struct zw_open {
	uint32_t	zo_ent;		/* the directory's entry */
	uint32_t	zo_next;	/* the create after it, or none */
};

/* Close one directory, then write the create a type change left. */
static void
zw_close(FILE *out, const struct zr_parsed *pp, const struct zw_ent *e,
    const struct zw_open *o, uint32_t depth)
{
	zm_indent(out, depth + 1);
	(void) fputs("..\n", out);
	if (o->zo_next != ZM_NONE)
		zw_line(out, pp, &e[o->zo_next], depth);
}

int
zr_parsed_write(FILE *out, const struct zr_parsed *pp)
{
	struct zw_ent *e;
	struct zw_open *stack;
	uint32_t i, n = 0, sd = 0;

	if (out == NULL || pp == NULL)
		return (-1);
	e = zw_entries(pp, &n);
	stack = e != NULL ? malloc((size_t)n * sizeof (*stack)) : NULL;
	if (e == NULL || stack == NULL) {
		free(e);
		free(stack);
		return (-1);
	}
	(void) fputs("#rebase-manifest 4\n", out);
	(void) fprintf(out, "#base %s\n", zm_text(pp->zp_base));
	(void) fprintf(out, "#from %s\n", zm_text(pp->zp_from));
	(void) fprintf(out, "#onto %s\n", zm_text(pp->zp_onto));
	(void) fprintf(out, "#mode %s\n",
	    pp->zp_mode == ZR_MODE_PERMISSIVE ? "permissive-merge" :
	    "strict");
	(void) fprintf(out, "#actions %u\n", pp->zp_actions_declared);
	(void) fprintf(out, "#conflicts %u\n", pp->zp_conflicts_declared);
	for (i = 0; i < n; i++) {
		if (e[i].ze_dup != 0)
			continue;
		while (sd > 0 &&
		    zw_under(&e[stack[sd - 1].zo_ent], &e[i]) == 0) {
			sd--;
			zw_close(out, pp, e, &stack[sd], sd);
		}
		zw_line(out, pp, &e[i], sd);
		if (e[i].ze_dir == 0)
			continue;
		stack[sd].zo_ent = i;
		stack[sd].zo_next = i + 1 < n && e[i + 1].ze_dup != 0 ?
		    i + 1 : ZM_NONE;
		sd++;
	}
	while (sd > 0) {
		sd--;
		zw_close(out, pp, e, &stack[sd], sd);
	}
	free(stack);
	free(e);
	if (pp->zp_nrecords > 0) {
		(void) fputs("\n# a pool is one file and all its names: "
		    "{names}letter; same\n", out);
		(void) fputs("# letter, same bytes\n", out);
	}
	for (i = 0; i < pp->zp_nrecords; i++)
		zw_record(out, &pp->zp_records[i]);
	return (ferror(out) != 0 ? -1 : 0);
}
