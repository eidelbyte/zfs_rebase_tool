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

/*
 * The same way into the middle of the choices of a resolution: with
 * n above zero zr_apply_choices stops itself with SIGSTOP
 * immediately before it acts on its n'th line -- the lines whose
 * choice it carries out, which are the makes, the links and the
 * removals, and neither the keeps nor a make a comparison found
 * already true. It is counted per call, so the second pass a caller
 * makes over a document it has already applied counts from one
 * again and, being idempotent, reaches no line at all. run.c sets it
 * from the gate ZFS_REBASE_PAUSE names and nothing else does; zero,
 * which is the default, is no gate at all. A test aid, documented in
 * tests/box/README.md and in no usage text.
 */
void zr_apply_choice_pause_at(unsigned int n);

/* What the apply did, for the report and for the tests. */
struct zr_apply_stats {
	uint64_t	zs_rm;
	uint64_t	zs_ln;
	uint64_t	zs_cp;
	uint64_t	zs_write;
	uint64_t	zs_dup;
	uint64_t	zs_bytes;	/* file bytes copied out of from */
	uint64_t	zs_skipped;	/* actions left alone, see below */
	/* and what the repair below did, which is the next three */
	uint64_t	zs_restored;	/* names put back out of onto */
	uint64_t	zs_removed;	/* names nothing expected, taken away */
	uint64_t	zs_relinked;	/* names linked back into their pool */
	/* and what the choices of a resolution did, which is the rest */
	uint64_t	zs_kept;	/* names the choice keep left alone */
	uint64_t	zs_made;	/* names made the side's object */
	uint64_t	zs_dropped;	/* names the side does not have */
	uint64_t	zs_linked;	/* names pooled onto their anchor */
	uint64_t	zs_latedirs;	/* directories the choices freed */
	uint32_t	zs_line;	/* the first line a choice changed */
};

/* No line of the resolution was changed: what zs_line carries then. */
#define	ZR_LINE_NONE	((uint32_t)-1)

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
 * The other document: the resolution's choices, made true on the
 * tree at root. m is the manifest the resolution answers, needed for
 * the removals a conflict blocked; names, onto and from are the
 * shared name table and the two sides, which this walks the result
 * beside to know what is already true.
 *
 * One line at a time, in the document's order, which is the walk's,
 * so a parent is made before its children:
 *
 *	keep	nothing at all; the result stands as it is
 *	onto	the name becomes onto's object at that same name --
 *		bytes, type and attributes -- or goes if onto has no
 *		such name
 *	from	the same out of from's tree
 *	"-"	refused: the caller proves the document complete
 *		before it calls, and nothing is written when one is met
 *
 * The names of one conflict group that chose the same side and that
 * that side pools together are one object in the result too: the
 * first of them in document order is copied and the rest are linked
 * onto it. Names of a group that chose different sides, names that
 * side keeps apart, and drift lines, which have no group, are each
 * their own. The removals wait for the makes and then run backwards,
 * children before parents.
 *
 * Last come the directory removals of the manifest that a conflict
 * blocked, in reverse manifest order: one goes if the choices left
 * it empty and stays if they did not, which is the plan's blocked-rm
 * rule.
 *
 * It is idempotent like the rest of the apply. A name that already
 * holds the chosen side's object -- the content oracle's word, over
 * an oracle this builds itself -- is left alone and counted in
 * zs_skipped, and so is a name already pooled where it belongs and a
 * removal already made. A second call over the same document
 * therefore changes nothing, which is how the stage checks itself.
 *
 * Returns 0 with the choice fields of *st filled and zs_line naming
 * the first line that was changed, or -1 with a message in err when
 * errlen is not 0.
 */
int zr_apply_choices(const struct zr_resolution *res, const struct zr_parsed *m,
    const char *root, struct zr_names *names, struct zr_walk *onto,
    struct zr_walk *from, struct zr_apply_stats *st, char *err, size_t errlen);

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
