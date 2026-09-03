/*
 * The manifest emitter. The result namespace is built as arrays of
 * nodes, one per name that has something to say plus every directory
 * on the way to one, and walked pre-order with the parent links as
 * the stack: nothing here recurses. The walk fixes manifest order,
 * manifest order fixes each result pool's anchor, and the anchors
 * decide which result pool keeps which onto object. Then the walk
 * runs again to write the lines, and the groups the tree section
 * marked are written out as records in the order it named them.
 */

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
	uint32_t	zn_parent;
	uint32_t	zn_first;
	uint32_t	zn_next;
	uint32_t	zn_ord;		/* position in manifest order */
	uint32_t	zn_group;	/* the group a conflict mark names */
	uint32_t	zn_cnum;	/* that group's number in the file */
	uint8_t		zn_act;
	uint8_t		zn_dir;
	uint8_t		zn_show;
};

/* Everything one emit run carries. */
struct zm {
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
	nd->zn_parent = 0;
	nd->zn_first = ZM_NONE;
	nd->zn_next = ZM_NONE;
	nd->zn_ord = 0;
	nd->zn_group = ZM_NONE;
	nd->zn_cnum = 0;
	nd->zn_act = ZM_NOTHING;
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
 * pool's first such name in manifest order.
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
	return (best != ZR_NAME_NONE ? best : n);
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
	if (n == m->zm_anchor[rk]) {
		q = m->zm_keeps[rk];
		if (q != ZR_POOL_NONE &&
		    m->zm_t[ZM_ONTO]->zt_pools[q].zp_content ==
		    d->zd_pools[rk].zr_content)
			return;
		nd->zn_act = q != ZR_POOL_NONE ? ZM_WRITE : ZM_CP;
		nd->zn_arg = zm_from_path(m, n, rk);
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

static void
zm_emit_line(const struct zm *m, FILE *out, uint32_t k, uint32_t depth)
{
	const struct zm_node *nd = &m->zm_nodes[k];
	const char *leaf, *p;
	size_t leaflen = 0, len = 0;

	zm_indent(out, depth);
	if (k == 0) {
		(void) fputc('/', out);
	} else {
		p = zr_names_str(m->zm_ns, nd->zn_name, &len);
		if (p == NULL) {
			(void) fputc('?', out);
		} else {
			leaf = zm_leaf(p, len, &leaflen);
			zm_vis(m, out, leaf, leaflen);
		}
		if (nd->zn_dir != 0)
			(void) fputc('/', out);
	}
	switch (nd->zn_act) {
	case ZM_RM:
		(void) fputs(" rm", out);
		break;
	case ZM_LN:
		(void) fputs(" ln ", out);
		zm_vis_name(m, out, nd->zn_arg);
		break;
	case ZM_CP:
		(void) fputs(" cp ", out);
		zm_vis_name(m, out, nd->zn_arg);
		break;
	case ZM_WRITE:
		(void) fputs(" write ", out);
		zm_vis_name(m, out, nd->zn_arg);
		break;
	case ZM_CONFLICT:
		(void) fprintf(out, " conflict %u", nd->zn_cnum);
		break;
	default:
		break;
	}
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
	rc = ferror(out) != 0 ? -1 : 0;
done:
	zm_fini(&m);
	return (rc);
}
