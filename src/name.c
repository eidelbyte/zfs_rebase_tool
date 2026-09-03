/*
 * The shared name table and the per-tree pool tables. One table
 * interns every path of all three trees into a dense id; each tree
 * then keeps one pool per inode, the names that reach it, and a map
 * from name id back to pool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "name.h"

#define	ZR_ARENA_MIN	4096
#define	ZR_ENTS_MIN	64
#define	ZR_HASH_MIN	256
#define	ZR_INO_MIN	64
#define	ZR_POOLS_MIN	64
#define	ZR_BY_NAME_MIN	64
#define	ZR_CAP_MAX	0x40000000U

/*
 * One interned path: where its bytes start in the arena and how many
 * there are. A NUL follows the bytes so that zr_names_str can hand
 * out a C string, but the length is what the code compares on.
 */
struct zr_nameent {
	size_t	nn_off;
	size_t	nn_len;
};

struct zr_names {
	char			*zn_arena;
	size_t			zn_used;
	size_t			zn_acap;
	struct zr_nameent	*zn_ents;
	uint32_t		zn_count;
	uint32_t		zn_ecap;
	zr_name_t		*zn_hash;	/* ZR_NAME_NONE is empty */
	uint32_t		zn_hcap;	/* a power of two */
};

/*
 * The per-tree inode index that hides behind zt_ino_hash: open
 * addressing over a power-of-two table, a free slot marked by
 * ZR_POOL_NONE, kept under half full like the name hash.
 */
struct zr_inoslot {
	uint64_t	is_ino;
	zr_pool_t	is_pool;
};

struct zr_inohash {
	struct zr_inoslot	*ih_slots;
	uint32_t		ih_cap;
	uint32_t		ih_count;
};

static uint32_t
zn_fnv1a(const char *p, size_t len)
{
	uint32_t h;
	size_t i;

	h = 2166136261U;
	for (i = 0; i < len; i++) {
		h ^= (uint32_t)(unsigned char)p[i];
		h *= 16777619U;
	}
	return (h);
}

/*
 * A 64-bit mix so that consecutive inode numbers do not cluster in
 * the open-addressed table.
 */
static uint32_t
zn_mix64(uint64_t v)
{
	v ^= v >> 33;
	v *= 0xff51afd7ed558ccdULL;
	v ^= v >> 33;
	v *= 0xc4ceb9fe1a85ec53ULL;
	v ^= v >> 33;
	return ((uint32_t)v);
}

/*
 * A path is absolute, is either "/" or has no trailing slash, and has
 * no empty, "." or ".." component. Length comes from the caller;
 * nothing here reads past it.
 */
static int
zn_path_valid(const char *p, size_t len)
{
	size_t i, start;

	if (p == NULL || len == 0 || p[0] != '/')
		return (0);
	if (len == 1)
		return (1);
	if (p[len - 1] == '/')
		return (0);
	start = 1;
	for (i = 1; i <= len; i++) {
		if (i < len && p[i] != '/')
			continue;
		if (i == start)
			return (0);
		if (i - start == 1 && p[start] == '.')
			return (0);
		if (i - start == 2 && p[start] == '.' && p[start + 1] == '.')
			return (0);
		start = i + 1;
	}
	return (1);
}

static int
zn_grow_arena(struct zr_names *ns, size_t need)
{
	size_t cap;
	char *p;

	if (ns->zn_used + need <= ns->zn_acap)
		return (0);
	cap = ns->zn_acap != 0 ? ns->zn_acap : ZR_ARENA_MIN;
	while (cap < ns->zn_used + need) {
		if (cap > (size_t)-1 / 2)
			return (-1);
		cap *= 2;
	}
	p = realloc(ns->zn_arena, cap);
	if (p == NULL)
		return (-1);
	ns->zn_arena = p;
	ns->zn_acap = cap;
	return (0);
}

static int
zn_grow_ents(struct zr_names *ns)
{
	struct zr_nameent *p;
	uint32_t cap;

	if (ns->zn_count < ns->zn_ecap)
		return (0);
	if (ns->zn_ecap > ZR_CAP_MAX)
		return (-1);
	cap = ns->zn_ecap != 0 ? ns->zn_ecap * 2 : ZR_ENTS_MIN;
	p = realloc(ns->zn_ents, (size_t)cap * sizeof (struct zr_nameent));
	if (p == NULL)
		return (-1);
	ns->zn_ents = p;
	ns->zn_ecap = cap;
	return (0);
}

/*
 * Find the slot for a path: its id if the path is interned, else
 * ZR_NAME_NONE and, in *slotp, the free slot it would take.
 */
static zr_name_t
zn_probe(const struct zr_names *ns, const char *path, size_t len,
    uint32_t *slotp)
{
	const struct zr_nameent *e;
	uint32_t mask, slot;
	zr_name_t id;

	if (ns->zn_hcap == 0) {
		if (slotp != NULL)
			*slotp = 0;
		return (ZR_NAME_NONE);
	}
	mask = ns->zn_hcap - 1;
	slot = zn_fnv1a(path, len) & mask;
	for (;;) {
		id = ns->zn_hash[slot];
		if (id == ZR_NAME_NONE)
			break;
		e = &ns->zn_ents[id];
		if (e->nn_len == len &&
		    memcmp(ns->zn_arena + e->nn_off, path, len) == 0)
			break;
		slot = (slot + 1) & mask;
	}
	if (slotp != NULL)
		*slotp = slot;
	return (id);
}

/*
 * Grow the hash if inserting one more name would reach half full,
 * reinserting every id into the new table.
 */
static int
zn_grow_hash(struct zr_names *ns)
{
	const struct zr_nameent *e;
	zr_name_t *tab;
	uint32_t cap, i, mask, slot;

	if ((uint64_t)ns->zn_count + 1 <= (uint64_t)ns->zn_hcap / 2)
		return (0);
	if (ns->zn_hcap > ZR_CAP_MAX)
		return (-1);
	cap = ns->zn_hcap != 0 ? ns->zn_hcap * 2 : ZR_HASH_MIN;
	tab = malloc((size_t)cap * sizeof (zr_name_t));
	if (tab == NULL)
		return (-1);
	for (i = 0; i < cap; i++)
		tab[i] = ZR_NAME_NONE;
	mask = cap - 1;
	for (i = 0; i < ns->zn_count; i++) {
		e = &ns->zn_ents[i];
		slot = zn_fnv1a(ns->zn_arena + e->nn_off, e->nn_len) & mask;
		while (tab[slot] != ZR_NAME_NONE)
			slot = (slot + 1) & mask;
		tab[slot] = (zr_name_t)i;
	}
	free(ns->zn_hash);
	ns->zn_hash = tab;
	ns->zn_hcap = cap;
	return (0);
}

struct zr_names *
zr_names_create(void)
{
	struct zr_names *ns;

	ns = malloc(sizeof (struct zr_names));
	if (ns == NULL)
		return (NULL);
	memset(ns, 0, sizeof (struct zr_names));
	return (ns);
}

void
zr_names_destroy(struct zr_names *ns)
{
	if (ns == NULL)
		return;
	free(ns->zn_arena);
	free(ns->zn_ents);
	free(ns->zn_hash);
	free(ns);
}

zr_name_t
zr_names_intern(struct zr_names *ns, const char *path, size_t len)
{
	struct zr_nameent *e;
	uint32_t slot;
	zr_name_t id;

	if (ns == NULL || !zn_path_valid(path, len))
		return (ZR_NAME_NONE);
	if (ns->zn_count >= ZR_NAME_NONE - 1)
		return (ZR_NAME_NONE);
	id = zn_probe(ns, path, len, &slot);
	if (id != ZR_NAME_NONE)
		return (id);
	if (zn_grow_arena(ns, len + 1) != 0)
		return (ZR_NAME_NONE);
	if (zn_grow_ents(ns) != 0)
		return (ZR_NAME_NONE);
	if (zn_grow_hash(ns) != 0)
		return (ZR_NAME_NONE);
	/* the hash may have moved the free slot */
	(void) zn_probe(ns, path, len, &slot);
	e = &ns->zn_ents[ns->zn_count];
	e->nn_off = ns->zn_used;
	e->nn_len = len;
	memcpy(ns->zn_arena + ns->zn_used, path, len);
	ns->zn_arena[ns->zn_used + len] = '\0';
	ns->zn_used += len + 1;
	id = ns->zn_count;
	ns->zn_count++;
	ns->zn_hash[slot] = id;
	return (id);
}

zr_name_t
zr_names_lookup(const struct zr_names *ns, const char *path, size_t len)
{
	if (ns == NULL || !zn_path_valid(path, len))
		return (ZR_NAME_NONE);
	return (zn_probe(ns, path, len, NULL));
}

const char *
zr_names_str(const struct zr_names *ns, zr_name_t id, size_t *lenp)
{
	const struct zr_nameent *e;

	if (ns == NULL || id >= ns->zn_count)
		return (NULL);
	e = &ns->zn_ents[id];
	if (lenp != NULL)
		*lenp = e->nn_len;
	return (ns->zn_arena + e->nn_off);
}

uint32_t
zr_names_count(const struct zr_names *ns)
{
	if (ns == NULL)
		return (0);
	return (ns->zn_count);
}

zr_name_t
zr_names_parent(const struct zr_names *ns, zr_name_t id)
{
	const char *p;
	size_t cut, len;

	p = zr_names_str(ns, id, &len);
	if (p == NULL || len < 2)
		return (ZR_NAME_NONE);
	cut = len;
	while (cut > 0 && p[cut - 1] != '/')
		cut--;
	if (cut <= 1)
		return (zn_probe(ns, "/", 1, NULL));
	return (zn_probe(ns, p, cut - 1, NULL));
}

static struct zr_inoslot *
zt_ino_probe(const struct zr_inohash *ih, uint64_t ino)
{
	uint32_t mask, slot;

	if (ih == NULL || ih->ih_cap == 0)
		return (NULL);
	mask = ih->ih_cap - 1;
	slot = zn_mix64(ino) & mask;
	while (ih->ih_slots[slot].is_pool != ZR_POOL_NONE &&
	    ih->ih_slots[slot].is_ino != ino)
		slot = (slot + 1) & mask;
	return (&ih->ih_slots[slot]);
}

static int
zt_ino_grow(struct zr_inohash *ih)
{
	struct zr_inoslot *slots;
	uint32_t cap, i, mask, slot;

	if ((uint64_t)ih->ih_count + 1 <= (uint64_t)ih->ih_cap / 2)
		return (0);
	if (ih->ih_cap > ZR_CAP_MAX)
		return (-1);
	cap = ih->ih_cap != 0 ? ih->ih_cap * 2 : ZR_INO_MIN;
	slots = malloc((size_t)cap * sizeof (struct zr_inoslot));
	if (slots == NULL)
		return (-1);
	for (i = 0; i < cap; i++) {
		slots[i].is_ino = 0;
		slots[i].is_pool = ZR_POOL_NONE;
	}
	mask = cap - 1;
	for (i = 0; i < ih->ih_cap; i++) {
		if (ih->ih_slots[i].is_pool == ZR_POOL_NONE)
			continue;
		slot = zn_mix64(ih->ih_slots[i].is_ino) & mask;
		while (slots[slot].is_pool != ZR_POOL_NONE)
			slot = (slot + 1) & mask;
		slots[slot] = ih->ih_slots[i];
	}
	free(ih->ih_slots);
	ih->ih_slots = slots;
	ih->ih_cap = cap;
	return (0);
}

static int
zt_grow_pools(struct zr_tree *tr)
{
	struct zr_pool *p;
	uint32_t cap;

	if (tr->zt_npools < tr->zt_cap)
		return (0);
	if (tr->zt_cap > ZR_CAP_MAX)
		return (-1);
	cap = tr->zt_cap != 0 ? tr->zt_cap * 2 : ZR_POOLS_MIN;
	p = realloc(tr->zt_pools, (size_t)cap * sizeof (struct zr_pool));
	if (p == NULL)
		return (-1);
	tr->zt_pools = p;
	tr->zt_cap = cap;
	return (0);
}

/*
 * zt_by_name is indexed by name id, so it grows with the shared
 * table, not with this tree. New slots start out empty.
 */
static int
zt_grow_by_name(struct zr_tree *tr, zr_name_t nm)
{
	zr_pool_t *p;
	uint32_t cap, i, want;

	if (nm < tr->zt_by_name_cap)
		return (0);
	want = zr_names_count(tr->zt_names);
	cap = tr->zt_by_name_cap != 0 ? tr->zt_by_name_cap : ZR_BY_NAME_MIN;
	while (cap <= nm || cap < want) {
		if (cap > ZR_CAP_MAX)
			return (-1);
		cap *= 2;
	}
	p = realloc(tr->zt_by_name, (size_t)cap * sizeof (zr_pool_t));
	if (p == NULL)
		return (-1);
	for (i = tr->zt_by_name_cap; i < cap; i++)
		p[i] = ZR_POOL_NONE;
	tr->zt_by_name = p;
	tr->zt_by_name_cap = cap;
	return (0);
}

int
zr_tree_init(struct zr_tree *tr, struct zr_names *ns)
{
	struct zr_inohash *ih;

	if (tr == NULL || ns == NULL)
		return (-1);
	ih = malloc(sizeof (struct zr_inohash));
	if (ih == NULL)
		return (-1);
	memset(ih, 0, sizeof (struct zr_inohash));
	memset(tr, 0, sizeof (struct zr_tree));
	tr->zt_names = ns;
	tr->zt_ino_hash = ih;
	return (0);
}

void
zr_tree_fini(struct zr_tree *tr)
{
	struct zr_inohash *ih;
	uint32_t i;

	if (tr == NULL)
		return;
	for (i = 0; i < tr->zt_npools; i++)
		free(tr->zt_pools[i].zp_names);
	free(tr->zt_pools);
	free(tr->zt_by_name);
	ih = tr->zt_ino_hash;
	if (ih != NULL)
		free(ih->ih_slots);
	free(ih);
	memset(tr, 0, sizeof (struct zr_tree));
}

zr_pool_t
zr_tree_add(struct zr_tree *tr, zr_name_t nm, uint64_t ino, zr_type_t type,
    uint32_t nlink)
{
	struct zr_inohash *ih;
	struct zr_inoslot *is;
	struct zr_pool *pool;
	zr_name_t *names;
	zr_pool_t idx;

	if (tr == NULL || tr->zt_sealed || nm == ZR_NAME_NONE)
		return (ZR_POOL_NONE);
	if (nm >= zr_names_count(tr->zt_names))
		return (ZR_POOL_NONE);
	if (zt_grow_by_name(tr, nm) != 0)
		return (ZR_POOL_NONE);
	if (tr->zt_by_name[nm] != ZR_POOL_NONE)
		return (ZR_POOL_NONE);
	ih = tr->zt_ino_hash;
	is = zt_ino_probe(ih, ino);
	if (is != NULL && is->is_pool != ZR_POOL_NONE) {
		idx = is->is_pool;
		pool = &tr->zt_pools[idx];
		if (pool->zp_type != type || pool->zp_nlink != nlink)
			return (ZR_POOL_NONE);
		if (type == ZR_T_DIR)
			return (ZR_POOL_NONE);
		names = realloc(pool->zp_names,
		    (size_t)(pool->zp_nnames + 1) * sizeof (zr_name_t));
		if (names == NULL)
			return (ZR_POOL_NONE);
		names[pool->zp_nnames] = nm;
		pool->zp_names = names;
		pool->zp_nnames++;
		tr->zt_by_name[nm] = idx;
		return (idx);
	}
	if (zt_grow_pools(tr) != 0)
		return (ZR_POOL_NONE);
	if (zt_ino_grow(ih) != 0)
		return (ZR_POOL_NONE);
	names = malloc(sizeof (zr_name_t));
	if (names == NULL)
		return (ZR_POOL_NONE);
	/* growing the index moved the slot, so probe again */
	is = zt_ino_probe(ih, ino);
	idx = tr->zt_npools;
	pool = &tr->zt_pools[idx];
	pool->zp_type = type;
	pool->zp_ino = ino;
	pool->zp_nlink = nlink;
	pool->zp_nnames = 1;
	pool->zp_names = names;
	pool->zp_names[0] = nm;
	pool->zp_content = ZR_CONTENT_NONE;
	is->is_ino = ino;
	is->is_pool = idx;
	ih->ih_count++;
	tr->zt_npools++;
	tr->zt_by_name[nm] = idx;
	return (idx);
}

zr_pool_t
zr_tree_pool(const struct zr_tree *tr, zr_name_t nm)
{
	if (tr == NULL || nm >= tr->zt_by_name_cap)
		return (ZR_POOL_NONE);
	return (tr->zt_by_name[nm]);
}

zr_pool_t
zr_tree_pool_by_ino(const struct zr_tree *tr, uint64_t ino)
{
	const struct zr_inoslot *is;

	if (tr == NULL)
		return (ZR_POOL_NONE);
	is = zt_ino_probe(tr->zt_ino_hash, ino);
	if (is == NULL)
		return (ZR_POOL_NONE);
	return (is->is_pool);
}

static int
zt_name_cmp(const void *a, const void *b)
{
	zr_name_t x, y;

	x = *(const zr_name_t *)a;
	y = *(const zr_name_t *)b;
	if (x < y)
		return (-1);
	if (x > y)
		return (1);
	return (0);
}

int
zr_tree_seal(struct zr_tree *tr)
{
	struct zr_pool *pool;
	uint32_t i;

	if (tr == NULL)
		return (-1);
	for (i = 0; i < tr->zt_npools; i++) {
		pool = &tr->zt_pools[i];
		if (pool->zp_nnames > 1) {
			qsort(pool->zp_names, pool->zp_nnames,
			    sizeof (zr_name_t), zt_name_cmp);
		}
	}
	tr->zt_sealed = 1;
	return (0);
}

/*
 * Every non-directory pool must hold as many names as the walk saw
 * links; every directory must hold exactly one. Sealed or not.
 */
int
zr_tree_verify(const struct zr_tree *tr, char *err, size_t errlen)
{
	const struct zr_pool *pool;
	const char *nm;
	uint32_t i;

	if (tr == NULL)
		return (-1);
	for (i = 0; i < tr->zt_npools; i++) {
		pool = &tr->zt_pools[i];
		if (pool->zp_type == ZR_T_DIR) {
			if (pool->zp_nnames == 1)
				continue;
		} else if (pool->zp_nnames == pool->zp_nlink) {
			continue;
		}
		nm = NULL;
		if (pool->zp_nnames > 0) {
			nm = zr_names_str(tr->zt_names, pool->zp_names[0],
			    NULL);
		}
		if (nm == NULL)
			nm = "(unnamed)";
		if (err != NULL && errlen > 0) {
			if (pool->zp_type == ZR_T_DIR) {
				(void) snprintf(err, errlen,
				    "%s: %u names but a directory has 1",
				    nm, pool->zp_nnames);
			} else {
				(void) snprintf(err, errlen,
				    "%s: %u names but nlink %u", nm,
				    pool->zp_nnames, pool->zp_nlink);
			}
		}
		return (-1);
	}
	return (0);
}
