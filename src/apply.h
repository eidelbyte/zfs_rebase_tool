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

/* What the apply did, for the report and for the tests. */
struct zr_apply_stats {
	uint64_t	zs_rm;
	uint64_t	zs_ln;
	uint64_t	zs_cp;
	uint64_t	zs_write;
	uint64_t	zs_dup;
	uint64_t	zs_bytes;	/* file bytes copied out of from */
	uint64_t	zs_skipped;	/* actions left alone, see below */
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

/* The same over a manifest nothing has classified: skip is NULL. */
int zr_apply(const struct zr_parsed *m, const char *onto_root,
    const struct zr_walk *from, const struct zr_walk *onto,
    struct zr_apply_stats *st, char *err, size_t errlen);

#endif	/* ZR_APPLY_H */
