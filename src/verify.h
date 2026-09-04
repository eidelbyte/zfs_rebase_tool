/*
 * verify: hold a parsed manifest against the tree in front of us and
 * say, of every action, whether it is done, still pending, blocked by
 * a conflict or drifted. Nothing here writes. It is what --verify
 * reports and what an idempotent apply reads to know what it may
 * leave alone.
 */

#ifndef	ZR_VERIFY_H
#define	ZR_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "manifest.h"
#include "name.h"
#include "walk.h"
#include "yellow.h"

/*
 * What one action says about the tree.
 *
 * Every action has two objects it can be held against: the one the
 * action produces, and the one onto held at that name before the
 * rebase, either of which may be absence. The result holding the
 * first is done and the second is pending; neither is drifted, which
 * is an edit made outside the rebase. Blocked is the one outcome
 * that is neither about: a directory the manifest removes that still
 * holds a conflicted name, which cannot go until the conflict is
 * answered. That is a state, not a fault, and never drift.
 *
 * Unchecked is not about the result at all: it is an action whose
 * question cannot be put, because a tree it would have to be read
 * against is not there any more. A verify run long after the rebase
 * finished meets that -- a snapshot destroyed, or one the tool made
 * itself and destroyed at done -- and the honest answer to "is this
 * done?" is then that nobody can say. Like blocked, it is never a
 * fault.
 */
enum zr_outcome {
	ZR_OC_DONE,
	ZR_OC_PENDING,
	ZR_OC_BLOCKED,
	ZR_OC_DRIFTED,
	ZR_OC_UNCHECKED
};

#define	ZR_OC_COUNT	5

/*
 * The trees the caller could not walk, if any, for the missing
 * argument of zr_verify. Only these two can be missing: the result
 * is the tree being verified, and a verify of a result that is not
 * there is not a question. A missing tree is still passed as a walk
 * -- an empty one will do -- because the oracle wants three of them;
 * the mask is what stops it ever being asked.
 */
#define	ZR_MISS_ONTO	0x1
#define	ZR_MISS_FROM	0x2

/* No action had that outcome: what zv_first carries then. */
#define	ZR_ACTION_NONE	((uint32_t)-1)

/*
 * One classification of one manifest. zv_outcome has an entry for
 * every line of m->zp_actions, conflict marks included, so that an
 * index into it is an index into the manifest; a conflict mark is
 * given ZR_OC_DONE, since there is nothing to do about it, and is
 * counted nowhere. The counts and the firsts are over the other
 * actions, in manifest order, ZR_ACTION_NONE where there were none.
 *
 * The info line is the stray-edit detector: a name the result holds
 * that no action names, that no conflict covers, that is not another
 * name of an object some action made, and that the result does not
 * hold as onto did -- an edit, or a name onto never had at all. It
 * is reported and never repaired.
 */
struct zr_verify_report {
	enum zr_outcome	*zv_outcome;	/* one per action; owned here */
	uint32_t	zv_nactions;	/* how many, for the apply's check */
	uint32_t	zv_count[ZR_OC_COUNT];
	uint32_t	zv_first[ZR_OC_COUNT];
	uint32_t	zv_ninfo;
	zr_name_t	zv_first_info;	/* ZR_NAME_NONE when zv_ninfo is 0 */
};

/*
 * Classify m against the three walked trees: onto as the rebase
 * found it, from as the manifest reads it, and result as it stands
 * now. All three share one name table, and o is an oracle built with
 * zr_oracle_init(&o, onto, from, result) over exactly those three,
 * so that the oracle's positions 0, 1 and 2 are onto, from and
 * result. zr_oracle_assign need not have been run: only
 * zr_oracle_equal is asked, and what it unions on the way is only
 * ever the truth.
 *
 * What "matches" means is the oracle's word -- type, attributes and
 * what the type adds, the bytes of a regular file included -- so an
 * untouched file really is read. Which names are one object is
 * answered inside the result walk, where a pool is exactly that,
 * since inode numbers do not carry from one tree to another.
 *
 * missing is 0 when all three trees are really there, and otherwise
 * the ZR_MISS_ bits of the ones that are not: every action that
 * would have to read one of those is ZR_OC_UNCHECKED, and with onto
 * missing there are no information lines either, since the names
 * nobody spoke for have nothing to be held against.
 *
 * Returns 0 with *out filled, or -1 with a message in err when
 * errlen is not 0. Either way *out is safe to hand to
 * zr_verify_report_fini.
 */
int zr_verify(const struct zr_parsed *m, struct zr_oracle *o,
    const struct zr_walk *onto, const struct zr_walk *from,
    const struct zr_walk *result, unsigned missing,
    struct zr_verify_report *out, char *err, size_t errlen);

void zr_verify_report_fini(struct zr_verify_report *r);

/* "done", "pending", "blocked", "drifted", "unchecked". */
const char *zr_outcome_str(enum zr_outcome oc);

/*
 * Does the manifest mark a name under this directory conflict? That
 * is the one thing that can stop the removal of a directory the
 * manifest says to remove. The classifier and the apply both ask it,
 * so that blocked means one thing in both.
 */
int zr_verify_blocked(const struct zr_parsed *m, const unsigned char *dir,
    size_t dirlen);

#endif	/* ZR_VERIFY_H */
