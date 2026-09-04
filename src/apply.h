/*
 * apply: turn the actions of a parsed manifest into the result tree.
 * The onto tree is the writable one; the walked from tree is where
 * the bytes and the attributes of every cp and write come from.
 *
 * Every action means "make this true", so an apply is idempotent: a
 * name already gone, a link already made, bytes already equal are
 * not failures, and a fresh run, a --continue and a repair are one
 * code path.
 */

#ifndef	ZR_APPLY_H
#define	ZR_APPLY_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#include "manifest.h"
#include "verify.h"
#include "walk.h"

/*
 * Set this from a signal handler and the apply stops between two
 * actions, at the first one it has not started, and fails with
 * "interrupted". It is only ever read and only ever set to 1, which
 * is what a handler is allowed to do to a volatile sig_atomic_t.
 */
extern volatile sig_atomic_t zr_apply_stop;

/*
 * The box harness's way into the middle of an apply: with n above
 * zero the apply stops itself with SIGSTOP immediately before it
 * performs its n'th action, counting the actions it performs and not
 * the ones a report let it leave alone, so that a kill or a stray
 * edit can land between two writes. run.c sets it from the gate
 * ZFS_REBASE_PAUSE names and nothing else does; zero, which is the
 * default, is no gate at all. A test aid, documented in
 * tests/box/README.md and in no usage text.
 */
void zr_apply_pause_at(unsigned int n);

/* What the apply did, for the report and for the tests. */
struct zr_apply_stats {
	uint64_t	zs_rm;
	uint64_t	zs_ln;
	uint64_t	zs_cp;
	uint64_t	zs_write;
	uint64_t	zs_dup;
	uint64_t	zs_bytes;	/* file bytes copied out of from */
	uint64_t	zs_skipped;	/* actions left alone, see below */
	/* and what the repair below did, which is the other three */
	uint64_t	zs_restored;	/* names put back out of onto */
	uint64_t	zs_removed;	/* names nothing expected, taken away */
	uint64_t	zs_relinked;	/* names linked back into their pool */
};

/*
 * Apply m to the tree at onto_root, reading from's bytes and
 * attributes for every cp and write. Actions run in manifest order,
 * except that the removal of a directory waits for its scope to
 * close. Every operation is relative to one descriptor on onto_root
 * and never follows a symbolic link, so nothing outside that tree is
 * read or written. Conflict marks do nothing.
 *
 * skip, when it is not NULL, is a report of zr_verify over this same
 * manifest: the actions it marks done or blocked are left alone and
 * counted in zs_skipped, everything else is performed. With a NULL
 * skip every action is performed, which over a tree an apply has
 * already been through is the same work again and lands in the same
 * place. Either way one removal is left alone without being asked:
 * the directory the manifest removes that still holds a conflicted
 * name, which cannot go until the conflict is answered.
 *
 * Returns 0 with *st filled, or -1 at the first failure with a
 * message naming the path, the step and the reason in err when
 * errlen is not 0. There is no undo: a failed apply leaves the tree
 * part way, which is why the caller works on a clone.
 */
int zr_apply_with(const struct zr_parsed *m, const char *onto_root,
    const struct zr_walk *from, const struct zr_walk *onto,
    const struct zr_verify_report *skip, struct zr_apply_stats *st,
    char *err, size_t errlen);

/*
 * The other half of an apply: the names no action spoke for, put
 * back as onto had them. rep is a report of zr_verify over the tree
 * at onto_root, names the table its walks share, and onto the walked
 * tree every restored object is copied out of.
 *
 *	gone and changed	onto's object again, linked from the
 *				entry's anchor where there is one and
 *				copied where there is not
 *	extra			unlinked, directories after what is
 *				inside them
 *	unpooled		linked back onto its anchor
 *
 * It is idempotent like the rest of the apply, and it can touch no
 * conflicted name, because zr_verify puts none in the list. Only the
 * applying1 stage calls it: from the conflicts gate on, the tree is
 * the person's to edit and a difference is theirs, not a stray.
 *
 * Returns 0 with the three repair fields of *st filled, or -1 with a
 * message in err when errlen is not 0.
 */
int zr_apply_repair(const struct zr_verify_report *rep, const char *onto_root,
    const struct zr_names *names, const struct zr_walk *onto,
    struct zr_apply_stats *st, char *err, size_t errlen);

/*
 * The self-check of an applying stage, made on the document it has
 * just applied: the tree at onto_root walked beside onto and from,
 * m classified against the three, and -- with fix, which is the
 * applying1 stage and nothing else -- the names of that
 * classification repaired, the tree walked again and classified
 * again. missing is the ZR_MISS_ mask of zr_verify.
 *
 * The verdict is that every action must be done or blocked: a
 * pending or a drifted one means the apply did not do what it said,
 * which is a failure of this program and not drift. With fix the
 * name list must also be empty, since the repair has just been
 * through it. Without fix the names are not looked at: from the
 * conflicts gate on they are the person's work.
 *
 * Returns 0 with the repair's counts in *st, or -1 with a message in
 * err when errlen is not 0.
 */
int zr_apply_check(const struct zr_parsed *m, const char *onto_root,
    struct zr_names *names, struct zr_walk *onto, struct zr_walk *from,
    unsigned missing, int fix, struct zr_apply_stats *st, char *err,
    size_t errlen);

#endif	/* ZR_APPLY_H */
