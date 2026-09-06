/* The real run's options and entry point. */

#ifndef ZR_RUN_H
#define	ZR_RUN_H

#include <stddef.h>

#include "decide.h"
#include "manifest.h"

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
 * it. A dataset that carries any zfs_rebase: property of its own is
 * an open rebase and is refused: a rebase that reached done cleared
 * its record, so there is nothing to overwrite and no flag that
 * would.
 *
 * unrelated is the one exception to all of that. Two sides that
 * share no origin have no branch point to work out, so the
 * derivation is skipped, the pruning is off -- an object number
 * means nothing across two lineages -- and the base is base, a
 * snapshot neither side is older than. The two go together: base is
 * NULL without unrelated and unrelated is nothing without base, and
 * the driver refuses either alone as a usage error (ruled
 * 2026-09-06; there is no empty-tree base any more).
 */
struct zr_run_opts {
	const char	*from;		/* pool/fs@snap or pool/fs */
	const char	*onto;		/* pool/fs@snap or pool/fs */
	const char	*result;	/* the clone, or the snapshot name */
	const char	*outpath;	/* manifest file, or NULL */
	const char	*base;		/* --base SNAP, or NULL */
	zr_mode_t	mode;
	int		dryrun;		/* manifest only, nothing created */
	int		unrelated;	/* --allow-unrelated: no derivation */
	int		quiet;		/* latched in the record at start */
	/*
	 * The three flags of the conflicts gate. takeonto and
	 * takefrom write the skeleton answered onto or from instead
	 * of unanswered, which the record keeps so that --restart
	 * writes the same document again; they exclude each other and
	 * the driver refuses both. nomerge holds the run at the gate
	 * however the skeleton reads, for a script that means to edit
	 * it before the merge. Where neither takes hold, a run whose
	 * skeleton came out complete goes on through the gate by
	 * itself, since a complete resolution plus the command that
	 * asked for the rebase is the signal the gate waits for.
	 *
	 * --no-gui is not here. What it asks of the gate -- go on
	 * when the resolution is complete, stop when it is not -- is
	 * what the gate does while there is no picker to launch, so
	 * nothing in the run reads it.
	 */
	int		takeonto;
	int		takefrom;
	int		nomerge;
	int		verbose;
};

int zr_run(const struct zr_run_opts *);

/*
 * What a verb was given (documents-design.md, section 6). A run is
 * named by result, the dataset carrying the record -- a snapshot
 * name is taken as its dataset, so both spellings of the dataset
 * form's --result find the same rebase -- or by path, the manifest
 * of that run, whose header names the dataset: zp_result in the
 * clone form, and the dataset of zp_onto in the dataset form, where
 * zp_result is the pre-apply snapshot as --result spelled it. Given
 * both, the two must name each other: the record's manifest property
 * must be that file and the header must name that dataset, and a
 * mismatch is a refusal that says both sides.
 *
 * from and onto are optional and change nothing: a verb reads the
 * two sides from the header, and a person who names them is saying
 * which rebase they think this is, which is checked against the
 * header by name and by guid.
 *
 * nomerge is --continue's alone; the others read it not at all.
 */
struct zr_verb_opts {
	const char	*result;	/* -r, or NULL */
	const char	*path;		/* MANIFEST, or NULL */
	const char	*from;		/* -f, or NULL */
	const char	*onto;		/* -t, or NULL */
	int		nomerge;
	int		verbose;
};

/*
 * The dataset that carries the record of the run a manifest
 * describes, by the rule above: 0 with buf filled, or -1 with one
 * line in err. It reads a parsed header and nothing else -- no file,
 * no pool -- which is what puts the rule within reach of a machine
 * with no ZFS in it.
 */
int zr_run_dataset(const struct zr_parsed *p, char *buf, size_t buflen,
    char *err, size_t errlen);

/*
 * The verbs on a rebase that already exists. Each finds its run as
 * struct zr_verb_opts says, and a dataset with no record of ours is
 * refused untouched. The record is the four properties; everything
 * else each verb needs is in the header of the manifest the record
 * names.
 *
 * zr_continue takes the rebase on from the gate its record names,
 * through the gates that are left, in one process: the recorded
 * manifest is applied again (which is idempotent, so what is already
 * true is left alone), the choices of the resolution after it once
 * every one of them is answered, and then done, which releases the
 * holds. An unanswered resolution is where the rebase waits.
 * The checks of the schedule are made on the way, under no flag: at
 * the conflicts gate the drift found becomes lines of the resolution
 * with the choice keep, and at the done gate the final check reports
 * and exits 3 where it finds drift, done being reached all the same.
 * Nothing here repairs: the one fix is applying1's own self-check.
 * With nomerge it stops at the conflicts gate
 * whatever the resolution says, and refuses altogether from a
 * record already past the merge: applying2 and done have no gate
 * left for the flag to hold.
 *
 * zr_restart puts the result back as onto was and applies again from
 * the first gate: the clone form destroys the clone and makes it
 * again from the header's onto snapshot with the same record, and
 * the dataset form rolls the dataset back to it. Nothing is decided
 * again: the recorded manifest is the decision, and the resolution
 * goes back to its skeleton, which is what discarding its edits
 * means.
 *
 * zr_report is the --verify verb: it classifies and prints and
 * writes nothing at all, finding each input by name and then by
 * guid, and saying which actions it could not check and why. Exit 0
 * clean and 3 where anything drifted -- an action pending or
 * drifted, or a name the manifest never spoke for that the result no
 * longer holds as onto had it; blocked and unchecked are states and
 * not faults.
 */
int zr_continue(const struct zr_verb_opts *);
int zr_restart(const struct zr_verb_opts *);
int zr_report(const struct zr_verb_opts *);

/*
 * Undo one rebase: release the holds the manifest's header names,
 * put the result back -- destroying the clone in the clone form, and
 * in the dataset form rolling the dataset back to its pre-apply
 * snapshot, destroying that snapshot, taking the record off and
 * mounting the dataset where it belongs again -- destroy any
 * snapshot the tool took for itself, remove the manifest the record
 * names and the resolution beside it, and take the run directory
 * away. Only a dataset carrying the record -- the hold tag and the
 * manifest path, both as local values -- is touched, and nothing is
 * ever removed recursively. It can be run again over a half-aborted
 * rebase.
 *
 * Where the manifest is gone there is no header to read, and what
 * is left is the tag: the holds are given back by walking the
 * result's pool for them, the private mount is undone, the record
 * is cleared, and nothing is destroyed or rolled back at all, since
 * the form of the run is one of the things the manifest was
 * carrying. It says so, and says what to run for each form.
 */
int zr_abort(const struct zr_verb_opts *);

#endif	/* ZR_RUN_H */
