/* The real run's options and entry point. */

#ifndef ZR_RUN_H
#define	ZR_RUN_H

#include "decide.h"

#define	ZR_NAME_MAX	1024	/* dataset names and mountpoints */

/*
 * The two sides are snapshots the user took; the tool takes none.
 * The base is not given: it is the branch point, and the run works
 * it out from the origin chains of the two. result is the dataset
 * the rebased clone is created as, and may be NULL only for a dry
 * run, which creates nothing.
 */
struct zr_run_opts {
	const char	*from;		/* pool/fs@snap */
	const char	*onto;		/* pool/fs@snap */
	const char	*result;	/* pool/fs, the clone to create */
	const char	*outpath;	/* manifest file, or NULL */
	zr_mode_t	mode;
	int		dryrun;		/* manifest only, no clone, no apply */
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
 * true is left alone), the resolution after it if the conflict
 * manager has left one, and then done, which releases the holds.
 * With verify it prints the classification of every action as it
 * goes and repairs drift on the clean ones; conflicted names are
 * never touched.
 *
 * zr_restart destroys the result and clones it again from the
 * recorded onto snapshot with the same record, then continues from
 * the first gate. Nothing is decided again: the recorded manifest is
 * the decision, and a resolution's edits are discarded by
 * definition.
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
 * Undo one rebase: release the holds its record names, destroy the
 * result dataset, remove the manifest the record names, and take the
 * run directory away. Only a dataset carrying the record -- the hold
 * tag and the manifest path, both as local values -- is touched, and
 * nothing is ever removed recursively. It can be run again over a
 * half-aborted rebase.
 */
int zr_abort(const char *result, int verbose);

#endif	/* ZR_RUN_H */
