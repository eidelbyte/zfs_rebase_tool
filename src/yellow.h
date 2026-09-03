/*
 * yellow: the content oracle. Three walked trees go in and one
 * opaque content handle per pool comes out, written into every
 * pool's zp_content, where equal handles mean equal content and
 * nothing else does. What "equal content" is here is what
 * v4-yellow-content.md leaves to this layer in its sections 2 and
 * 7: the attributes always, a directory by those alone since its
 * entries are pools of their own, a file by its bytes, and the
 * times never.
 */

#ifndef	ZR_YELLOW_H
#define	ZR_YELLOW_H

#include "walk.h"

struct zr_oracle;

/*
 * An oracle over three walked trees, which must be sealed, share one
 * name table and outlive it. Returns 0 with *out set to an oracle
 * the caller frees with zr_oracle_fini, or -1.
 */
int zr_oracle_init(struct zr_oracle **out, struct zr_walk *base,
    struct zr_walk *from, struct zr_walk *onto);

/*
 * The caller knows -- from zfs diff, which reads the dnode and not
 * the bytes -- that pool of tree, 1 for from and 2 for onto, holds
 * exactly what base_pool of base holds. The two are put in one class
 * on that word, and neither is ever read. Returns 0, or -1 on an
 * argument the oracle cannot place.
 */
int zr_oracle_unchanged(struct zr_oracle *o, int tree, zr_pool_t pool,
    zr_pool_t base_pool);

/*
 * Compare every green-adjacent pair -- two pools of different trees
 * holding one name -- that is not already in one class, put the
 * equal ones in one class, and write one handle per class into the
 * zp_content of every pool of all three trees, dense from 0. A pair
 * is read at most once however many names it shares. Returns 0, or
 * -1 with a message naming the tree and the path in err when errlen
 * is not 0: a read that fails is a failure, never a verdict of
 * "different".
 */
int zr_oracle_assign(struct zr_oracle *o, char *err, size_t errlen);

/* Every byte read from either side, for the tests and the report. */
uint64_t zr_oracle_bytes_read(const struct zr_oracle *o);

void zr_oracle_fini(struct zr_oracle *o);

#endif	/* ZR_YELLOW_H */
