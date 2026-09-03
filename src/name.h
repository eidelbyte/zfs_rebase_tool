/*
 * The shared name table and the per-tree pool tables: paths interned
 * into small dense ids, and one pool per file holding every name that
 * reaches it. All three trees share one name table so that later
 * phases can compare pools across trees by name id alone.
 */

#ifndef	ZR_NAME_H
#define	ZR_NAME_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t zr_name_t;		/* id in the shared table */
typedef uint32_t zr_pool_t;		/* index in one tree's pool array */

#define	ZR_NAME_NONE	((zr_name_t)-1)
#define	ZR_POOL_NONE	((zr_pool_t)-1)
#define	ZR_CONTENT_NONE	((uint32_t)-1)

typedef enum zr_type {
	ZR_T_FILE,
	ZR_T_DIR,
	ZR_T_SYMLINK,
	ZR_T_CHR,
	ZR_T_BLK,
	ZR_T_FIFO,
	ZR_T_SOCK
} zr_type_t;

/*
 * The shared name table. Paths are byte strings given as (pointer,
 * length); nothing here ever calls strlen on caller memory.
 */
struct zr_names;

struct zr_names *zr_names_create(void);
void zr_names_destroy(struct zr_names *ns);
zr_name_t zr_names_intern(struct zr_names *ns, const char *path, size_t len);
zr_name_t zr_names_lookup(const struct zr_names *ns, const char *path,
    size_t len);
const char *zr_names_str(const struct zr_names *ns, zr_name_t id,
    size_t *lenp);
uint32_t zr_names_count(const struct zr_names *ns);
zr_name_t zr_names_parent(const struct zr_names *ns, zr_name_t id);

/*
 * One file and every name it has in one tree. Directories are pools
 * with exactly one name.
 */
struct zr_pool {
	zr_type_t	zp_type;
	uint64_t	zp_ino;
	uint32_t	zp_nlink;	/* st_nlink as the walk reported it */
	uint32_t	zp_nnames;
	zr_name_t	*zp_names;	/* ascending by id once sealed */
	uint32_t	zp_content;	/* opaque, ZR_CONTENT_NONE until set */
};

/*
 * One tree: the pools of one snapshot, plus the name-to-pool map over
 * the shared table. Fields are public; treat them as read-only
 * outside name.c.
 */
struct zr_tree {
	struct zr_names	*zt_names;
	struct zr_pool	*zt_pools;
	uint32_t	zt_npools;
	uint32_t	zt_cap;
	zr_pool_t	*zt_by_name;	/* by name id; ZR_POOL_NONE if none */
	uint32_t	zt_by_name_cap;
	void		*zt_ino_hash;	/* private to name.c */
	int		zt_sealed;
};

int zr_tree_init(struct zr_tree *tr, struct zr_names *ns);
void zr_tree_fini(struct zr_tree *tr);
zr_pool_t zr_tree_add(struct zr_tree *tr, zr_name_t nm, uint64_t ino,
    zr_type_t type, uint32_t nlink);
zr_pool_t zr_tree_pool(const struct zr_tree *tr, zr_name_t nm);
zr_pool_t zr_tree_pool_by_ino(const struct zr_tree *tr, uint64_t ino);
int zr_tree_seal(struct zr_tree *tr);
int zr_tree_verify(const struct zr_tree *tr, char *err, size_t errlen);

#endif	/* ZR_NAME_H */
