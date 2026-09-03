/*
 * yellow: the content oracle. Two pools of different trees are
 * compared only where they share a name, which is where the decision
 * will ask: type first, then the attributes every file carries, then
 * whatever the type adds -- a symlink's target, a device's number, a
 * regular file's size and then its bytes, streamed and abandoned at
 * the first chunk that differs. Equal pairs are unioned, so equality
 * travels through shared names, and one dense handle per class is
 * written into every pool of all three trees.
 */

#define	_XOPEN_SOURCE	700

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "name.h"
#include "walk.h"
#include "yellow.h"

#define	ZO_CHUNK	((size_t)128 * 1024)	/* one read buffer */
#define	ZO_SET_MIN	16
#define	ZO_SET_MAX	0x40000000U

/* The trees, in the order the oracle indexes their pools. */
static const char *const zo_treename[3] = { "base", "from", "onto" };

/* The three tree pairs one shared name can make. */
static const int zo_pairs[3][2] = { { 0, 1 }, { 0, 2 }, { 1, 2 } };

/*
 * A small open-addressing set of pair keys: the pairs compared
 * already, so that two pools sharing several names are read once
 * even when they turned out to differ. A key is the two pool
 * indices, the smaller in the high half, and since no index reaches
 * ZR_CONTENT_NONE no key can be the empty marker.
 */
struct zo_set {
	uint64_t	*zs_keys;
	uint32_t	zs_cap;		/* a power of two */
	uint32_t	zs_n;
};

#define	ZO_EMPTY	((uint64_t)-1)

struct zr_oracle {
	struct zr_walk	*zo_w[3];
	uint32_t	zo_off[3];	/* where a tree's pools start */
	uint32_t	zo_npools[3];
	uint32_t	zo_total;
	uint32_t	*zo_parent;	/* union-find over every pool */
	struct zo_set	zo_seen;
	unsigned char	*zo_buf[2];	/* one per side, ZO_CHUNK each */
	uint64_t	zo_bytes;
};

static int
zo_set_alloc(struct zo_set *s, uint32_t cap)
{
	uint32_t i;

	s->zs_keys = malloc((size_t)cap * sizeof (uint64_t));
	if (s->zs_keys == NULL)
		return (-1);
	s->zs_cap = cap;
	s->zs_n = 0;
	for (i = 0; i < cap; i++)
		s->zs_keys[i] = ZO_EMPTY;
	return (0);
}

static uint32_t
zo_hash(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	return ((uint32_t)k);
}

static void
zo_set_put(struct zo_set *s, uint64_t k)
{
	uint32_t i = zo_hash(k) & (s->zs_cap - 1);

	while (s->zs_keys[i] != ZO_EMPTY) {
		if (s->zs_keys[i] == k)
			return;
		i = (i + 1) & (s->zs_cap - 1);
	}
	s->zs_keys[i] = k;
	s->zs_n++;
}

static int
zo_set_grow(struct zo_set *s)
{
	struct zo_set ns;
	uint32_t i;

	if (s->zs_cap > ZO_SET_MAX)
		return (-1);
	if (zo_set_alloc(&ns, s->zs_cap * 2) != 0)
		return (-1);
	for (i = 0; i < s->zs_cap; i++) {
		if (s->zs_keys[i] != ZO_EMPTY)
			zo_set_put(&ns, s->zs_keys[i]);
	}
	free(s->zs_keys);
	*s = ns;
	return (0);
}

static int
zo_set_add(struct zo_set *s, uint64_t k)
{
	if (s->zs_n * 2 >= s->zs_cap && zo_set_grow(s) != 0)
		return (-1);
	zo_set_put(s, k);
	return (0);
}

static int
zo_set_has(const struct zo_set *s, uint64_t k)
{
	uint32_t i = zo_hash(k) & (s->zs_cap - 1);

	while (s->zs_keys[i] != ZO_EMPTY) {
		if (s->zs_keys[i] == k)
			return (1);
		i = (i + 1) & (s->zs_cap - 1);
	}
	return (0);
}

/* Union-find over the pools of all three trees, laid end to end. */
static uint32_t
zo_find(uint32_t *parent, uint32_t x)
{
	uint32_t root = x, next;

	while (parent[root] != root)
		root = parent[root];
	while (parent[x] != root) {
		next = parent[x];
		parent[x] = root;
		x = next;
	}
	return (root);
}

static void
zo_union(uint32_t *parent, uint32_t a, uint32_t b)
{
	a = zo_find(parent, a);
	b = zo_find(parent, b);
	if (a != b)
		parent[b < a ? a : b] = (b < a ? b : a);
}

/* A failure, named by the tree and the path it happened at. */
static void
zo_fail(struct zr_oracle *o, int t, zr_name_t nm, const char *what,
    char *err, size_t errlen)
{
	const char *path;
	size_t len;

	if (err == NULL || errlen == 0)
		return;
	path = zr_names_str(o->zo_w[t]->zw_tree.zt_names, nm, &len);
	if (path == NULL)
		path = "(a name of the tree)";
	(void) snprintf(err, errlen, "%s: %s: %s: %s", zo_treename[t], path,
	    what, strerror(errno));
}

/* An ACL text, a default ACL text or a symlink target: both or one. */
static int
zo_text_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return (a == b);
	return (strcmp(a, b) == 0);
}

/* The lists are sorted by name, so one pass compares them. */
static int
zo_xattrs_equal(const struct zr_attr *a, const struct zr_attr *b)
{
	const struct zr_xattr *x, *y;
	uint32_t i;

	if (a->za_nxattrs != b->za_nxattrs)
		return (0);
	for (i = 0; i < a->za_nxattrs; i++) {
		x = &a->za_xattrs[i];
		y = &b->za_xattrs[i];
		if (strcmp(x->zx_name, y->zx_name) != 0)
			return (0);
		if (x->zx_len != y->zx_len)
			return (0);
		if (x->zx_len != 0 &&
		    memcmp(x->zx_value, y->zx_value, x->zx_len) != 0)
			return (0);
	}
	return (1);
}

/*
 * The attributes every type carries. The times are not among them:
 * a file written twice from the same bytes is the same content, and
 * a rebase that said otherwise would conflict on every copy.
 */
static int
zo_attrs_equal(const struct zr_attr *a, const struct zr_attr *b)
{
	if (a->za_mode != b->za_mode || a->za_uid != b->za_uid ||
	    a->za_gid != b->za_gid || a->za_flags != b->za_flags)
		return (0);
	if (zo_xattrs_equal(a, b) == 0)
		return (0);
	if (zo_text_equal(a->za_acl, b->za_acl) == 0)
		return (0);
	return (zo_text_equal(a->za_dacl, b->za_dacl));
}

/* The two read buffers, taken only once a file is really compared. */
static int
zo_buffers(struct zr_oracle *o)
{
	if (o->zo_buf[0] != NULL && o->zo_buf[1] != NULL)
		return (0);
	if (o->zo_buf[0] == NULL)
		o->zo_buf[0] = malloc(ZO_CHUNK);
	if (o->zo_buf[1] == NULL)
		o->zo_buf[1] = malloc(ZO_CHUNK);
	if (o->zo_buf[0] == NULL || o->zo_buf[1] == NULL)
		return (-1);
	return (0);
}

/* One buffer's worth, or what is left of the file. */
static int
zo_read_chunk(int fd, unsigned char *buf, size_t *np)
{
	size_t got = 0;
	ssize_t n;

	while (got < ZO_CHUNK) {
		n = read(fd, buf + got, ZO_CHUNK - got);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	*np = got;
	return (0);
}

/*
 * The bytes of two regular files, a chunk from each side at a time,
 * abandoned at the first pair that differs. Every byte read is
 * counted, whether it was told anything or not. Returns 1 equal, 0
 * different, -1 with err set.
 */
static int
zo_bytes_equal(struct zr_oracle *o, int ta, zr_pool_t pa, int tb,
    zr_pool_t pb, char *err, size_t errlen)
{
	zr_name_t na, nb;
	size_t gota, gotb;
	int fda, fdb, rc;

	na = o->zo_w[ta]->zw_tree.zt_pools[pa].zp_names[0];
	nb = o->zo_w[tb]->zw_tree.zt_pools[pb].zp_names[0];
	if (zo_buffers(o) != 0) {
		errno = ENOMEM;
		zo_fail(o, ta, na, "buffers", err, errlen);
		return (-1);
	}
	fda = zr_walk_openat(o->zo_w[ta], na, O_RDONLY);
	if (fda < 0) {
		zo_fail(o, ta, na, "open", err, errlen);
		return (-1);
	}
	fdb = zr_walk_openat(o->zo_w[tb], nb, O_RDONLY);
	if (fdb < 0) {
		zo_fail(o, tb, nb, "open", err, errlen);
		(void) close(fda);
		return (-1);
	}
	rc = 1;
	for (;;) {
		if (zo_read_chunk(fda, o->zo_buf[0], &gota) != 0) {
			zo_fail(o, ta, na, "read", err, errlen);
			rc = -1;
			break;
		}
		o->zo_bytes += gota;
		if (zo_read_chunk(fdb, o->zo_buf[1], &gotb) != 0) {
			zo_fail(o, tb, nb, "read", err, errlen);
			rc = -1;
			break;
		}
		o->zo_bytes += gotb;
		if (gota != gotb) {
			rc = 0;
			break;
		}
		if (gota == 0)
			break;
		if (memcmp(o->zo_buf[0], o->zo_buf[1], gota) != 0) {
			rc = 0;
			break;
		}
	}
	(void) close(fda);
	(void) close(fdb);
	return (rc);
}

/*
 * Two pools, in the order that stops at the first difference: the
 * type, then the attributes, then what the type adds. A directory is
 * decided by its attributes alone -- its entries are pools of their
 * own, and a fifo and a socket have nothing else to hold. Returns 1
 * equal, 0 different, -1 with err set.
 */
static int
zo_pool_equal(struct zr_oracle *o, int ta, zr_pool_t pa, int tb,
    zr_pool_t pb, char *err, size_t errlen)
{
	const struct zr_attr *ata, *atb;
	zr_type_t type;

	type = o->zo_w[ta]->zw_tree.zt_pools[pa].zp_type;
	if (type != o->zo_w[tb]->zw_tree.zt_pools[pb].zp_type)
		return (0);
	ata = &o->zo_w[ta]->zw_attrs[pa];
	atb = &o->zo_w[tb]->zw_attrs[pb];
	if (zo_attrs_equal(ata, atb) == 0)
		return (0);
	switch (type) {
	case ZR_T_DIR:
	case ZR_T_FIFO:
	case ZR_T_SOCK:
		return (1);
	case ZR_T_SYMLINK:
		return (zo_text_equal(ata->za_target, atb->za_target));
	case ZR_T_CHR:
	case ZR_T_BLK:
		return (ata->za_rdev == atb->za_rdev);
	case ZR_T_FILE:
		if (ata->za_size != atb->za_size)
			return (0);
		return (zo_bytes_equal(o, ta, pa, tb, pb, err, errlen));
	}
	return (0);
}

/*
 * One green-adjacent pair. Two pools already in one class say what
 * they would have said, so nothing is read; a pair already compared
 * differed, and would differ again.
 */
static int
zo_pair(struct zr_oracle *o, int ta, zr_pool_t pa, int tb, zr_pool_t pb,
    char *err, size_t errlen)
{
	uint64_t key;
	uint32_t ga, gb;
	int eq;

	ga = o->zo_off[ta] + pa;
	gb = o->zo_off[tb] + pb;
	if (zo_find(o->zo_parent, ga) == zo_find(o->zo_parent, gb))
		return (0);
	key = ga < gb ? ((uint64_t)ga << 32) | gb : ((uint64_t)gb << 32) | ga;
	if (zo_set_has(&o->zo_seen, key))
		return (0);
	if (zo_set_add(&o->zo_seen, key) != 0) {
		errno = ENOMEM;
		zo_fail(o, ta, o->zo_w[ta]->zw_tree.zt_pools[pa].zp_names[0],
		    "pair set", err, errlen);
		return (-1);
	}
	eq = zo_pool_equal(o, ta, pa, tb, pb, err, errlen);
	if (eq < 0)
		return (-1);
	if (eq != 0)
		zo_union(o->zo_parent, ga, gb);
	return (0);
}

int
zr_oracle_init(struct zr_oracle **out, struct zr_walk *base,
    struct zr_walk *from, struct zr_walk *onto)
{
	struct zr_walk *w[3];
	struct zr_oracle *o;
	uint64_t total;
	uint32_t i;

	if (out == NULL || base == NULL || from == NULL || onto == NULL)
		return (-1);
	*out = NULL;
	w[0] = base;
	w[1] = from;
	w[2] = onto;
	total = 0;
	for (i = 0; i < 3; i++) {
		if (w[i]->zw_tree.zt_sealed == 0 ||
		    w[i]->zw_nattrs < w[i]->zw_tree.zt_npools ||
		    w[i]->zw_tree.zt_names != w[0]->zw_tree.zt_names)
			return (-1);
		total += w[i]->zw_tree.zt_npools;
	}
	if (total == 0 || total >= (uint64_t)ZR_CONTENT_NONE)
		return (-1);
	o = malloc(sizeof (struct zr_oracle));
	if (o == NULL)
		return (-1);
	memset(o, 0, sizeof (struct zr_oracle));
	o->zo_total = (uint32_t)total;
	for (i = 0; i < 3; i++) {
		o->zo_w[i] = w[i];
		o->zo_npools[i] = w[i]->zw_tree.zt_npools;
		if (i != 0)
			o->zo_off[i] = o->zo_off[i - 1] + o->zo_npools[i - 1];
	}
	o->zo_parent = malloc((size_t)o->zo_total * sizeof (uint32_t));
	if (o->zo_parent == NULL || zo_set_alloc(&o->zo_seen,
	    ZO_SET_MIN) != 0) {
		zr_oracle_fini(o);
		return (-1);
	}
	for (i = 0; i < o->zo_total; i++)
		o->zo_parent[i] = i;
	*out = o;
	return (0);
}

int
zr_oracle_unchanged(struct zr_oracle *o, int tree, zr_pool_t pool,
    zr_pool_t base_pool)
{
	if (o == NULL || (tree != 1 && tree != 2))
		return (-1);
	if (pool >= o->zo_npools[tree] || base_pool >= o->zo_npools[0])
		return (-1);
	zo_union(o->zo_parent, o->zo_off[tree] + pool,
	    o->zo_off[0] + base_pool);
	return (0);
}

int
zr_oracle_assign(struct zr_oracle *o, char *err, size_t errlen)
{
	zr_pool_t p[3];
	uint32_t *handle;
	uint32_t nnames, next, g, j;
	zr_name_t nm;
	int t, i, a, b;

	if (o == NULL)
		return (-1);
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	nnames = zr_names_count(o->zo_w[0]->zw_tree.zt_names);
	for (nm = 0; nm < nnames; nm++) {
		for (t = 0; t < 3; t++)
			p[t] = zr_tree_pool(&o->zo_w[t]->zw_tree, nm);
		for (i = 0; i < 3; i++) {
			a = zo_pairs[i][0];
			b = zo_pairs[i][1];
			if (p[a] == ZR_POOL_NONE || p[b] == ZR_POOL_NONE)
				continue;
			if (zo_pair(o, a, p[a], b, p[b], err, errlen) != 0)
				return (-1);
		}
	}
	/*
	 * One handle per class, dense from 0 and handed out in pool
	 * order. No class can be given ZR_CONTENT_NONE: there are
	 * fewer classes than pools, and there are fewer pools than
	 * that.
	 */
	handle = malloc((size_t)o->zo_total * sizeof (uint32_t));
	if (handle == NULL) {
		if (err != NULL && errlen > 0) {
			(void) snprintf(err, errlen,
			    "the content handles: %s", strerror(ENOMEM));
		}
		return (-1);
	}
	for (g = 0; g < o->zo_total; g++)
		handle[g] = ZR_CONTENT_NONE;
	next = 0;
	for (t = 0; t < 3; t++) {
		for (j = 0; j < o->zo_npools[t]; j++) {
			g = zo_find(o->zo_parent, o->zo_off[t] + j);
			if (handle[g] == ZR_CONTENT_NONE)
				handle[g] = next++;
			o->zo_w[t]->zw_tree.zt_pools[j].zp_content =
			    handle[g];
		}
	}
	free(handle);
	return (0);
}

uint64_t
zr_oracle_bytes_read(const struct zr_oracle *o)
{
	return (o != NULL ? o->zo_bytes : 0);
}

void
zr_oracle_fini(struct zr_oracle *o)
{
	if (o == NULL)
		return;
	free(o->zo_parent);
	free(o->zo_buf[0]);
	free(o->zo_buf[1]);
	free(o->zo_seen.zs_keys);
	free(o);
}
