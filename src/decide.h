/*
 * zr_decide: the v4 decision over three trees. Green (names and
 * pools) per v4-green-pooling.md, yellow (content) per
 * v4-yellow-content.md, the mode per v4-permissive-merge.md.
 *
 * Input: three sealed trees sharing one name table, with every
 * pool's zp_content set to an opaque handle where equal handles mean
 * equal bytes (ZR_CONTENT_NONE is never equal to anything).
 *
 * Output: the result pools with their content handles, one group per
 * face local group of the working face carrying the conflict classes
 * that fired in it, and per-name state for the emitter.
 */

#ifndef ZR_DECIDE_H
#define	ZR_DECIDE_H

#include "name.h"

typedef enum zr_mode {
	ZR_MODE_STRICT,
	ZR_MODE_PERMISSIVE
} zr_mode_t;

/* Conflict classes, as bits; a group may carry several. */
#define	ZR_CF_HEALED_SPLIT	0x01u
#define	ZR_CF_ORPHANED_ADD	0x02u
#define	ZR_CF_CONTESTED_HOME	0x04u
#define	ZR_CF_UNEXPRESSED	0x08u
#define	ZR_CF_CHANGED_BOTH	0x10u
#define	ZR_CF_DISAGREE		0x20u
#define	ZR_CF_GREEN		(ZR_CF_HEALED_SPLIT | ZR_CF_ORPHANED_ADD | \
				ZR_CF_CONTESTED_HOME | ZR_CF_UNEXPRESSED)

/* Per-name state bits in zd_state[]. */
#define	ZR_NS_BASE		0x01u	/* base holds it */
#define	ZR_NS_FROM		0x02u	/* from holds it */
#define	ZR_NS_ONTO		0x04u	/* onto holds it */
#define	ZR_NS_SURVIVES		0x08u	/* in the result */
#define	ZR_NS_CONTESTED		0x10u	/* both sides invented it */

struct zr_result_pool {
	uint32_t	zr_nnames;
	zr_name_t	*zr_names;	/* ascending by id */
	uint32_t	zr_content;	/* NONE in a conflicted group */
	uint32_t	zr_group;	/* index into zd_groups */
};

/*
 * One face local group of the working face: the from and onto pools
 * connected by shared surviving names, plus the base pools their
 * base names came from. The conflict record lists exactly these.
 */
struct zr_group {
	uint32_t	zg_flags;	/* ZR_CF_* that fired here */
	zr_name_t	zg_why[2];	/* for the why line; NONE if unused */
	uint32_t	zg_nbase, zg_nfrom, zg_nonto;
	zr_pool_t	*zg_base;	/* pool indices per tree */
	zr_pool_t	*zg_from;
	zr_pool_t	*zg_onto;
};

struct zr_decision {
	struct zr_result_pool	*zd_pools;
	uint32_t		zd_npools;
	struct zr_group		*zd_groups;
	uint32_t		zd_ngroups;
	uint8_t			*zd_state;	/* indexed by name id */
	zr_pool_t		*zd_result_of;	/* per name; or NONE */
	uint32_t		zd_nnames;	/* size of the two arrays */
	uint32_t		zd_nconflicts;	/* groups with zg_flags != 0 */
};

/*
 * Decide. The trees must be sealed and share one name table. Returns
 * 0 on success (conflicts are a successful decision: read zd_groups),
 * -1 on allocation failure. Never modifies the trees.
 */
int zr_decide(const struct zr_tree *base, const struct zr_tree *from,
    const struct zr_tree *onto, zr_mode_t mode, struct zr_decision *out);

void zr_decision_fini(struct zr_decision *);

/* The class names as the manifest and the batteries spell them. */
const char *zr_conflict_name(uint32_t single_flag);

#endif	/* ZR_DECIDE_H */
