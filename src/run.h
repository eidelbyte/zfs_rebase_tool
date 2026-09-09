/* The real run's options and entry point. */

#ifndef ZR_RUN_H
#define	ZR_RUN_H

#include <stddef.h>

#include "decide.h"
#include "manifest.h"
#include "walk.h"

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
	 * --interactive is not here. What it asks for is the picker
	 * at the gate, and there is none to launch in this build, so
	 * the gate is headless with the flag or without it and
	 * nothing in the run reads it.
	 */
	int		takeonto;
	int		takefrom;
	int		nomerge;
	int		verbose;
};

int zr_run(const struct zr_run_opts *);

/*
 * Would a system file flag stop the apply at this securelevel?
 *
 * The apply takes an object's immutable, append-only and no-unlink
 * flags off before it removes, rewrites or changes it, and puts back
 * what the decision asks for afterwards (src/apply.c, za_unlock_st).
 * Above securelevel 0 that is not possible for the system three:
 * zfs_freebsd_setattr calls securelevel_gt(cred, 0) before it will
 * change the flags of an object carrying schg, sappnd or sunlnk, and
 * returns EPERM. Setting one is allowed at every level; only taking
 * it off is not. So two kinds of object are refused, before anything
 * is touched:
 *
 *	onto's, the side the result is written over: one carrying a
 *	system flag at a name the decision would remove, rewrite or
 *	re-pool has to be unlocked first, and cannot be;
 *
 *	from's, the side the result is written out of: one carrying a
 *	system flag that the decision puts into the result has that
 *	flag written by za_attrs, and is locked from that moment. A
 *	--continue that has to redo the action, or the applying1
 *	self-check's put-back, then meets the same EPERM the onto half
 *	exists to prevent, with the tree part written.
 *
 * Only the system flags enter this. The user three (uchg, uappnd,
 * uunlnk) come off for the owner at any securelevel, and this tool
 * runs as root; ZFS refuses to set them at all (EOPNOTSUPP), so on
 * the target they can only arrive on a tree from another
 * filesystem. Only a BSD has any of it.
 *
 * level is the securelevel to answer for, which is why this is a
 * function and not the sysctl read itself: the caller reads
 * kern.securelevel and this decides. Returns 1 with one line in err
 * naming the first such path and the side it is on, or 0 when there
 * is nothing to refuse -- which is every level of 0 or less, where
 * the apply clears the flags itself, and every platform that has no
 * system flags to carry.
 */
int zr_flags_refused(const struct zr_decision *d, const struct zr_walk *onto,
    const struct zr_walk *from, const struct zr_names *names, int level,
    char *err, size_t errlen);

/*
 * What a verb was given (documents-design.md, section 11.4). One
 * identifier names the rebase, and it is resolved in five steps, the
 * first that matches winning:
 *
 *	1. an absolute path: the -o manifest at it, whose header
 *	   names the dataset that carries the record (zr_run_dataset);
 *	   a file that is not there is a refusal here and never a
 *	   fall-through to a name;
 *	2. a dataset carrying the record whose name is the identifier
 *	   or ends in "/" and it: the clone form's result, in full or
 *	   by its short name;
 *	3. a snapshot of a dataset carrying the record whose name
 *	   after the '@' is the identifier, or the whole snapshot
 *	   spelled out: the dataset form's pre-apply snapshot;
 *	4. a relative path to a manifest of the user's;
 *	5. a run directory of that name, which is what a crash before
 *	   the record leaves and what --abort takes away.
 *
 * Steps 2 and 3 search every imported pool, and more than one match
 * at a step is refused with the matches printed and never chosen
 * between. Whatever matched, the two halves are then cross-checked:
 * the record must name the manifest and the header must name the
 * dataset the record sits on.
 *
 * from and onto are optional and change nothing: a verb reads the
 * two sides from the header, and a person who names them is saying
 * which rebase they think this is, which is checked against the
 * header by name and by guid.
 *
 * nomerge is --continue's alone; the others read it not at all.
 */
struct zr_verb_opts {
	const char	*ident;		/* IDENT, the one operand */
	const char	*from;		/* -f, or NULL */
	const char	*onto;		/* -t, or NULL */
	int		nomerge;
	int		verbose;
};

/*
 * What an identifier resolved to. zi_result is the dataset that
 * carries the rebase's record, which is what every verb works from;
 * zi_path is the manifest the command named, empty where a name
 * found the rebase; zi_man is that file's parse, kept so that the
 * document is read once and not twice; and zi_rundir says the
 * identifier found nothing but a run directory, which is a leftover
 * for --abort and no rebase for anything else.
 */
struct zr_ident {
	char			zi_result[ZR_NAME_MAX];
	char			zi_path[ZR_NAME_MAX];
	struct zr_parsed	zi_man;
	int			zi_parsed;
	int			zi_rundir;
};

/*
 * Steps 1 and 4 of that resolution: the manifest at path, resolved
 * with realpath -- which is what the start recorded, so a file named
 * through a symlink or from another directory is the same file --
 * parsed, and its header read for the dataset that carries the
 * record. Returns 0 with out filled, or -1 with one line in err that
 * names the path. It opens a file and no pool, which is what puts
 * the path steps within reach of a machine with no ZFS in it.
 *
 * A parse it made is out's to free, which zr_ident_fini does; the
 * struct is written whole either way, so a refusal leaves nothing
 * half filled behind.
 */
int zr_ident_manifest(const char *path, struct zr_ident *out, char *err,
    size_t errlen);

/* The parse an identifier's resolution kept, given back. */
void zr_ident_fini(struct zr_ident *id);

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
