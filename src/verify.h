/*
 * verify: hold a parsed manifest against the tree in front of us and
 * say, of every action, whether it is done, still pending, blocked by
 * a conflict or drifted, and of every name no action spoke for
 * whether the result holds it as onto did. Nothing here writes. It is
 * what --verify reports, what an idempotent apply reads to know what
 * it may leave alone, and what the repair of applying1 works from.
 *
 * One verify, at every gate. The expected tree is onto's names with
 * the manifest's actions applied abstractly: from's object where an
 * action put it, onto's where none spoke for it, nothing under an rm.
 * The result is held against that on two axes -- name participation
 * (the set of names, and which of them are one object) and content
 * (the oracle's word: type, attributes, bytes) -- and the difference
 * is what this reports. Conflicted names are skipped on both axes.
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
 * What one name no action spoke for says about the tree. The
 * expected object of such a name is onto's own, or nothing where onto
 * never had it or where an rm removed the directory above it, and
 * these are the four ways the result can differ from that:
 *
 *	gone		onto had it and the result does not
 *	extra		the result has it and nothing expected it
 *	changed		there, but not the object onto had
 *	unpooled	there and equal, but no longer one object
 *			with the untouched names it shared onto's with
 *
 * The first two are the name axis and the last two the content and
 * the pooling; between them they are the "missing, extra, wrong
 * pool, wrong bytes" of the plan.
 */
enum zr_diff {
	ZR_DF_GONE,
	ZR_DF_EXTRA,
	ZR_DF_CHANGED,
	ZR_DF_UNPOOLED
};

#define	ZR_DF_COUNT	4

/*
 * One such name. zn_anchor is the name the repair mends this one
 * from where the mending is a link rather than a copy: for gone and
 * changed, another name of the same onto pool that the result still
 * holds as onto had it, so that a pool of two names is not severed by
 * putting one of them back; for unpooled, the name this one has left,
 * which is that same anchor. It is ZR_NAME_NONE when there is no such
 * name, which is every extra and every single-name pool.
 */
struct zr_namediff {
	zr_name_t	zn_name;
	zr_name_t	zn_anchor;
	enum zr_diff	zn_kind;
};

/*
 * One classification of one manifest. zv_outcome has an entry for
 * every line of m->zp_actions, conflict marks included, so that an
 * index into it is an index into the manifest; a conflict mark is
 * given ZR_OC_DONE, since there is nothing to do about it, and is
 * counted nowhere. The counts and the firsts are over the other
 * actions, in manifest order, ZR_ACTION_NONE where there were none.
 *
 * zv_diffs is the rest of the tree: one entry per name no action
 * spoke for that the result does not hold as the expected tree says,
 * in name order, owned here and freed by zr_verify_report_fini. A
 * name a conflict covers is in no entry, and neither is a name that
 * shares a result pool with a name some action made -- the second
 * name of a written file changed because the write said so, and
 * nobody is told about it twice. zv_dcount and zv_dfirst are derived
 * from the list, ZR_NAME_NONE where a kind has no entry.
 */
struct zr_verify_report {
	enum zr_outcome	*zv_outcome;	/* one per action; owned here */
	uint32_t	zv_nactions;	/* how many, for the apply's check */
	uint32_t	zv_count[ZR_OC_COUNT];
	uint32_t	zv_first[ZR_OC_COUNT];
	struct zr_namediff *zv_diffs;	/* one per name; owned here */
	uint32_t	zv_ndiffs;
	uint32_t	zv_dcount[ZR_DF_COUNT];
	zr_name_t	zv_dfirst[ZR_DF_COUNT];
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
 * missing the name list is empty, since the names nobody spoke for
 * have nothing to be held against.
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

/* "gone", "extra", "changed", "unpooled". */
const char *zr_diff_str(enum zr_diff df);

/*
 * Does the manifest mark a name under this directory conflict? That
 * is the one thing that can stop the removal of a directory the
 * manifest says to remove. The classifier and the apply both ask it,
 * so that blocked means one thing in both.
 */
int zr_verify_blocked(const struct zr_parsed *m, const unsigned char *dir,
    size_t dirlen);

#endif	/* ZR_VERIFY_H */
