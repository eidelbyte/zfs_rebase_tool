/* The real run's options and entry point. */

#ifndef ZR_RUN_H
#define	ZR_RUN_H

#include "decide.h"

#define	ZR_NAME_MAX	1024	/* dataset names and mountpoints */

/*
 * Each side is a snapshot the user took or a dataset the tool
 * snapshots for itself, and --onto decides the form of the run:
 *
 *	onto a snapshot -- the clone form. result names the dataset
 *	the rebased clone is created as, read-only at the run's own
 *	mountpoint, and carries the record.
 *
 *	onto a dataset -- the dataset form. The rebase is made in that
 *	dataset, which carries the record, and result names the
 *	snapshot taken of it before anything is applied: the short
 *	name after the '@', or a full name whose dataset part is onto
 *	itself. That snapshot is the user's before-image and stays
 *	after done.
 *
 * The base is not given either way: it is the branch point, and the
 * run works it out from the origin chains of the two sides. result
 * may be NULL only for a dry run, which creates nothing and ignores
 * it. overwrite replaces a record whose rebase reached done, and is
 * the dataset form's alone: a clone-form result is a fresh dataset
 * with a fresh record and there is nothing there to overwrite.
 *
 * unrelated is the one exception to all of that. Two sides that
 * share no origin have no branch point to work out, so the
 * derivation is skipped, the pruning is off -- an object number
 * means nothing across two lineages -- and the base is base when the
 * user gave one, a snapshot neither side is older than, or the empty
 * tree when they did not, which makes every name an add on its side
 * and the decision the union of the two with a conflict wherever
 * they disagree. base is NULL without unrelated, which the driver
 * refuses as a usage error.
 */
struct zr_run_opts {
	const char	*from;		/* pool/fs@snap or pool/fs */
	const char	*onto;		/* pool/fs@snap or pool/fs */
	const char	*result;	/* the clone, or the snapshot name */
	const char	*outpath;	/* manifest file, or NULL */
	const char	*base;		/* --base SNAP, or NULL */
	zr_mode_t	mode;
	int		dryrun;		/* manifest only, nothing created */
	int		overwrite;	/* replace a record that is done */
	int		unrelated;	/* --allow-unrelated: no derivation */
	int		verify;		/* the demand for a final check */
	int		verbose;
};

int zr_run(const struct zr_run_opts *);

/*
 * The verbs on a rebase that already exists. result for all three is
 * the dataset carrying the record -- a snapshot name is taken as its
 * dataset, so both spellings find the same rebase -- and a dataset
 * with no record of ours is refused untouched.
 *
 * zr_continue takes the rebase on from the gate its record names,
 * through the gates that are left, in one process: the recorded
 * manifest is applied again (which is idempotent, so what is already
 * true is left alone), the choices of the resolution after it once
 * every one of them is answered, and then done, which releases the
 * holds. An unanswered resolution is where the rebase waits.
 * With verify it prints the classification of every action as it
 * goes and repairs drift on the clean ones; conflicted names are
 * never touched.
 *
 * zr_restart puts the result back as onto was and applies again from
 * the first gate: the clone form destroys the clone and makes it
 * again from the recorded onto snapshot with the same record, and
 * the dataset form rolls the dataset back to it. Nothing is decided
 * again: the recorded manifest is the decision, and the resolution
 * goes back to its skeleton, which is what discarding its edits
 * means.
 *
 * zr_report is --verify alone: it classifies and prints and writes
 * nothing at all, finding each input by name and then by guid, and
 * saying which actions it could not check and why. Exit 0 when
 * nothing is pending or drifted, 3 when something is.
 */
int zr_continue(const char *result, int verify, int verbose);
int zr_restart(const char *result, int verbose);
int zr_report(const char *result, int verbose);

/*
 * Undo one rebase: release the holds its record names, put the
 * result back -- destroying the clone in the clone form, and in the
 * dataset form rolling the dataset back to its pre-apply snapshot,
 * destroying that snapshot, taking the record off and mounting the
 * dataset where it belongs again -- destroy any snapshot the tool
 * took for itself, remove the manifest the record names, and take
 * the run directory away. Only a dataset carrying the record -- the
 * hold tag and the manifest path, both as local values -- is
 * touched, and nothing is ever removed recursively. It can be run
 * again over a half-aborted rebase.
 */
int zr_abort(const char *result, int verbose);

#endif	/* ZR_RUN_H */
