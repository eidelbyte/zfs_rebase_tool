/*
 * The real run: holds on the three input snapshots, a working tree
 * to rebase into, the walk of those three snapshots through
 * .zfs/snapshot, the unchanged set read off those walks, decide,
 * manifest, apply, re-walk. Everything here is library calls;
 * nothing is exec'd. The ZFS operations themselves are in zfsops.c
 * and exist only in the FreeBSD build.
 *
 * Each side is a snapshot or a dataset, and --onto decides the form
 * of the whole run:
 *
 * The clone form, onto given as a snapshot. --result names a new
 * dataset, cloned from that snapshot read-only and with the
 * mountpoint property none, and mounted at the run's private
 * directory with zfs_mount_at; the record lives on the clone.
 * Nothing of the user's is written to at all. At done the clone is
 * unmounted and handed to the void -- readonly on, mountpoint still
 * none -- and the tool says how to place it, which is the user's
 * work and not the tool's.
 *
 * The dataset form, onto given as a dataset. --result names the
 * pre-apply snapshot the tool takes of it -- the short name after
 * the '@', or a full name whose dataset part is onto -- and the
 * rebase is made in that dataset itself, so the record lives on it.
 * Exclusivity is the unmount: the dataset is unmounted from its own
 * mountpoint and mounted at the run's private directory instead,
 * with its mountpoint property untouched and canmount set to noauto,
 * and it stays there for the whole of the rebase. Nothing is ever
 * forced; a dataset somebody is using is refused. It is handed home
 * at done and at --abort and at no other moment: a rebase waiting
 * for a conflict to be answered is a half rebased tree, and a half
 * rebased tree is not put back into service (documents-design.md,
 * section 5). The pre-apply snapshot is the user's own before-image:
 * it stays after done and only --abort takes it away, rolling the
 * dataset back to it first.
 *
 * A side given as a dataset is snapshotted by the tool under a
 * generated name and recorded as tool-made, and that snapshot lives
 * exactly as long as the rebase: it goes at done and at --abort. If
 * the user wanted it kept they would have passed a snapshot.
 *
 * Only the two sides are named: the base is the branch point, and
 * the run works it out by walking the two origin chains back to the
 * dataset they share.
 *
 * A rebase outlives its process. What makes it one thing rather than
 * a process's leavings is the record -- four user properties on the
 * result, and no more: the phase, the manifest's absolute path, the
 * hold tag, and the word that says the start was given --quiet --
 * and the three persistent holds, one per input snapshot, filed
 * under that tag with no cleanup descriptor. Everything else a verb
 * needs is in the header of the manifest the record names: the three
 * snapshots with their guids, which of them the tool made, the mode,
 * the form, the way the skeleton was answered, and in the dataset
 * form the pre-apply snapshot and the two properties to give back
 * (sprints/sprint-5/documents-design.md, sections 2 and 3). Either
 * document names the run: the property points at the manifest and
 * the header names the result.
 *
 * While the holds are there zfs destroy refuses the snapshots with
 * "dataset is busy" and zfs holds shows the tag; a stranded rebase
 * holds on purpose, because it is continuable. The record is read as
 * local values only: user properties inherit down the naming tree,
 * and an inherited value is not ours (zfsops.c). In the clone form
 * the create writes it, so it is there from the clone's first
 * instant; in the dataset form it is set on the dataset before
 * anything else is touched.
 *
 * So a hard kill leaves the result, the manifest file and the three
 * holds, and
 *
 *	zfs_rebase --abort NAME
 *
 * releases the holds and takes the rest away -- destroying the clone
 * in one form, rolling the dataset back to its pre-apply snapshot
 * and destroying that in the other. While a clone lives, onto's
 * snapshot cannot be destroyed either; that is ZFS's own rule about
 * a clone's origin and not something this tool arranges.
 *
 * The phase is written at the gates the run passes, and nowhere
 * else, so that what a kill leaves is the last gate reached:
 *
 *	decided -> applying1 -> conflicts -> applying2 -> done
 *	decided -> applying1 -> done		(no conflicts)
 *
 * done is no phase: at done every zfs_rebase: property is taken off
 * the result, so a dataset that carries one is always an open
 * rebase and one that carries none has no rebase, whatever its
 * history.
 *
 * "decided" goes down the moment the decision manifest has been
 * renamed over the birth manifest, which is before the skeleton is
 * written beside it: what it says is that the document the record
 * names is the decision and not the header the run was born with.
 * "applying1" goes down immediately before readonly comes off, and
 * the clean actions of the manifest are applied under it whether the
 * decision had conflicts or not. A conflict stops the names it
 * covers and nothing else, and whoever has to answer one should be
 * answering it over the tree the rest of the rebase has already
 * made. "conflicts" goes down after that apply verified, and is the
 * hand-off: the resolution, which this run wrote beside the manifest
 * as a skeleton of choices (v4-manifest.md, section 8), is answered
 * there -- by a person, by a picker acting for them, or by a --take
 * flag before it was written -- and "applying2" carries the answers
 * out. The gate keys on completeness: every line answered, and a
 * --continue, which is the human input the move needs. done is
 * reached after the re-walk verified and readonly is back on: the
 * holds are given back and then the record is taken off, in that
 * order, because the tag in the record is the only handle on those
 * holds and a kill between the two must leave the handle rather
 * than the holds.
 *
 * At birth there is no phase at all, and a stop writes none: what a
 * stop leaves is the gate it was working under, and --continue
 * resumes from exactly that. A record with no phase is therefore a
 * rebase born and not decided, which --continue and --restart
 * refuse -- there is no decision to carry out -- and --abort takes
 * away with everything the birth manifest's header gave it.
 *
 * The order a run acquires things in, which is the order --abort
 * and done give them back in, reversed (documents-design.md,
 * section 11.3): the pre-apply snapshot in the dataset form, the
 * run directory, the birth manifest, the record, the holds, the
 * snapshot the tool took of from, the take, and then -- after the
 * walks and the decision -- the decision manifest, the phase
 * "decided" and the skeleton. Everything from the birth manifest on
 * is in the header from that moment, so an --abort at any gate has
 * the form, the pre-apply snapshot and the two properties to put
 * back.
 *
 * The verbs further down this file work on a rebase that is already
 * there, and read the record and the manifest its path names and
 * nothing else: --continue takes it on from the gate the record
 * names, checking at every gate it passes as the schedule says;
 * --restart puts the result back as onto was -- by destroying the
 * clone and making it again, or by rolling the dataset back to its
 * pre-apply snapshot -- before doing the
 * same; --verify only reports; --abort takes the whole thing
 * away. None of them decides anything: the manifest the record names
 * is the decision, and it is made once. Each of them takes the
 * result over the same way the run did, in both forms, and leaves it
 * at the private mount unless it reached done.
 *
 * A signal that would ordinarily end the process -- INT, TERM, HUP
 * -- is caught instead and only raises a flag. Before every phase,
 * and between two actions of the apply, the flag is looked at: if it
 * is up the run stops there and says so. Before the apply that
 * destroys the clone as any other failure before the apply does;
 * inside the apply the clone is kept at applying1, put back to
 * read-only, and a later --continue resumes it or --abort takes it
 * away. The handlers do not ask for SA_RESTART, so a read or a
 * write already in a slow call fails with EINTR rather than starting
 * over, and the phase that owns it reports that failure in the
 * ordinary way.
 */

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#endif

#include "apply.h"
#include "decide.h"
#include "manifest.h"
#include "name.h"
#include "run.h"
#include "verify.h"
#include "walk.h"
#include "yellow.h"
#include "zfsops.h"

#define	EXIT_CLEAN	0
#define	EXIT_CONFLICTS	1
#define	EXIT_PRECOND	2
#define	EXIT_INTERNAL	3

/*
 * Where a run keeps its private mount point and its manifest. Not
 * /var/run: FreeBSD's cleanvar rc script (libexec/rc/rc.d/cleanvar)
 * deletes every regular file under /var/run at boot, and a rebase
 * that stops at conflicts can wait there for days and across a
 * reboot -- its manifest has to still be on disk when it does.
 * /var/db is the tree for exactly that, state a program owns and
 * keeps. The layout is unchanged: WORKDIR/<result as a path>, 0700,
 * with the clone mounted at mnt and the manifest beside it.
 */
#define	WORKDIR		"/var/db/zfs_rebase"

/*
 * The gates, and the second document of a run. The resolution sits
 * beside the manifest -- <rundir>/resolution beside <rundir>/manifest,
 * or FILE.resolution beside a -o FILE -- by that rule and by no
 * other: the record names the manifest, and where the manifest is
 * says where the resolution is (resolution_of, below).
 *
 * The first four are the values zfs_rebase:phase takes. "done" is
 * not one of them and is never written: it is the name of the last
 * gate, for the pause hook and for the messages, and what it leaves
 * on the result is no record at all.
 *
 * "decided" is written the moment the decision manifest is renamed
 * into place, which is before the skeleton beside it: a record with
 * no phase at all is a rebase that was born and never decided, and
 * the manifest its record names is the birth document
 * (documents-design.md, section 11.1).
 */
#define	ZR_PHASE_DECIDED	"decided"
#define	ZR_PHASE_APPLYING1	"applying1"
#define	ZR_PHASE_CONFLICTS	"conflicts"
#define	ZR_PHASE_APPLYING2	"applying2"
#define	ZR_GATE_DONE		"done"
#define	ZR_RESOLUTION		"resolution"

/*
 * The two forms a run can be in, as the run itself carries them.
 * What a verb reads back is the header's #form line (enum zr_hform),
 * which is where the form is recorded now.
 */
#define	ZR_FORM_CLONE		"clone"
#define	ZR_FORM_DATASET		"dataset"

/*
 * The whole record, in the order the dataset form writes it: the
 * manifest first and the tag after it, so that a set that fails part
 * way leaves something no verb will read, and the phase and the
 * quiet word after those, which the writers keep apart. It is what
 * done and --abort take away again, and what a fresh run holds a
 * dataset against: any one of these, as the dataset's own value, is
 * an open rebase.
 */
static const char *zr_record_props[] = {
	ZR_PROP_MANIFEST, ZR_PROP_TAG, ZR_PROP_PHASE, ZR_PROP_QUIET
};

#define	ZR_NRECORD	(sizeof (zr_record_props) / sizeof (zr_record_props[0]))

/*
 * What a header line carries where the run had no such thing: a dry
 * run's result and tag, a run that snapshotted neither side, a clone
 * form's pre-apply snapshot, and the base of a document written
 * before --allow-unrelated needed --base. "-" is how zfs(8) itself
 * spells a property with no value, and it is no snapshot name, since
 * every one of those has a pool and an '@' in it. An empty value
 * would say the same thing and cannot be written: the manifest's
 * header lines are trimmed of trailing blanks before they are read,
 * so a "#base " with nothing after it comes back as no header line
 * at all (manifest.c, zp_header).
 */
#define	ZR_NO_BASE		"-"

/*
 * What --take-onto and --take-from write into the header's #take
 * line, and what a run given neither writes: the word says which
 * choice the skeleton was written with, so that --restart can write
 * the same document again rather than an unanswered one.
 */
#define	ZR_TAKE_NONE		"-"
#define	ZR_TAKE_ONTO		"onto"
#define	ZR_TAKE_FROM		"from"

/* The word of the record, as the choice a skeleton is written with. */
static enum zr_choice
take_choice(const char *take)
{
	if (strcmp(take, ZR_TAKE_ONTO) == 0)
		return (ZR_CH_ONTO);
	if (strcmp(take, ZR_TAKE_FROM) == 0)
		return (ZR_CH_FROM);
	return (ZR_CH_NONE);
}

/*
 * The snapshot the tool takes of a side given as a dataset:
 * <dataset>@zfs_rebase-<the run's hold tag>, and the same with -2,
 * -3 and so on when that name is taken. The tag is unique to the run
 * already, so the suffix is for the pathological case only and the
 * bound is small.
 */
#define	ZR_MADE_PREFIX		"zfs_rebase-"
#define	ZR_MADE_TRIES		8

/*
 * A dataset or a snapshot name is at most ZFS_MAX_DATASET_NAME_LEN,
 * which is 256; the origin chains are sized by that rather than by
 * ZR_NAME_MAX, which has to hold a mountpoint. Thirty-two links is
 * a clone of a clone of a clone thirty-two deep, and a chain longer
 * than that is refused rather than followed.
 */
#define	ZR_SNAP_MAX	256
#define	ZR_CHAIN_MAX	32

/* "zr-" and twelve hex digits, with room to spare. */
#define	ZR_TAG_MAX	32

struct run {
	struct zr_run_opts	o;
	struct zr_zfs		*zfs;
	const char		*form;		/* ZR_FORM_ */
	char			tag[ZR_TAG_MAX];	/* the hold tag */
	/* The branch point, and "" for a run that has no base. */
	char			base[ZR_NAME_MAX];
	char			fromsnap[ZR_SNAP_MAX];	/* given or made */
	char			ontosnap[ZR_SNAP_MAX];	/* given or made */
	char			rds[ZR_NAME_MAX];	/* carries the record */
	char			ontods[ZR_NAME_MAX];	/* onto's dataset */
	char			rundir[ZR_NAME_MAX];	/* WORKDIR/<rds> */
	char			workmnt[ZR_NAME_MAX];	/* <rundir>/mnt */
	char			manpath[ZR_NAME_MAX];	/* the manifest */
	char			respath[ZR_NAME_MAX];	/* the resolution */
	char			basemnt[ZR_NAME_MAX];	/* mountpoints */
	char			frommnt[ZR_NAME_MAX];
	char			ontomnt[ZR_NAME_MAX];	/* where onto is read */
	char			ontohome[ZR_NAME_MAX];	/* onto's own place */
	char			roorig[8];	/* onto's readonly before */
	char			cmorig[16];	/* and its canmount */
	/*
	 * The guids of the three inputs, read off the snapshots
	 * themselves just before the manifest is written: they are
	 * the header's, which is the one place a rebase's identity is
	 * kept now. A run with no base carries the guid 0 with the
	 * "-" that says it had none.
	 */
	uint64_t		baseguid, fromguid, ontoguid;
	int			dirmade, cloned;
	/*
	 * Which of the run's walks are live, one bit each. The
	 * result's is the one the applying1 self-check made and left
	 * behind it, which the done gate reads rather than walking
	 * the tree a second time (R13 of the code review).
	 */
	int			walked;
	int			born;		/* the birth manifest is */
	/*
	 * Whether the unchanged set may be read off the walks. It may
	 * when base was derived from the two sides, which is what
	 * puts all three in one object-number space -- both forms do
	 * that. --allow-unrelated leaves it clear: across two
	 * lineages an object number means nothing, and a base given
	 * by hand is no proof of a shared one
	 * (sprints/sprint-5/string-audit.md, section 2).
	 */
	int			prune;
	int			recorded;	/* the record is written */
	int			madefrom;	/* the tool took from's snap */
	int			madeonto;	/* and onto's: a dry run */
	int			presnap;	/* the pre-apply snapshot is */
	int			privmnt;	/* onto is at workmnt */
	int			nheld;		/* holds taken, base first */
	uint32_t		unanswered;	/* lines of the skeleton */
	struct zr_names		*names;
	struct zr_walk		wb, wf, wo, wr;
	struct zr_oracle	*oracle;
	struct zr_decision	d;
	char			err[512];
};

/* The bits of run.walked, one per tree the run has read. */
#define	ZR_W_BASE	0x1
#define	ZR_W_FROM	0x2
#define	ZR_W_ONTO	0x4
#define	ZR_W_RESULT	0x8

/* Which form is this? The dataset form is the one that is unusual. */
static int
in_dataset_form(const struct run *r)
{
	return (r->form != NULL && strcmp(r->form, ZR_FORM_DATASET) == 0);
}

/* The word this run's --take flags write into the record. */
static const char *
run_take(const struct run *r)
{
	if (r->o.takeonto)
		return (ZR_TAKE_ONTO);
	if (r->o.takefrom)
		return (ZR_TAKE_FROM);
	return (ZR_TAKE_NONE);
}

/*
 * The signals the run catches. The handler does the one thing a
 * handler may do here: raise the flag apply.c and every phase
 * boundary read.
 */
static const int zr_sigs[] = { SIGINT, SIGTERM, SIGHUP };
#define	ZR_NSIG		(sizeof (zr_sigs) / sizeof (zr_sigs[0]))

static void
on_signal(int sig)
{
	(void) sig;
	zr_apply_stop = 1;
}

static void
signals_install(struct sigaction *saved)
{
	struct sigaction sa;
	size_t i;

	memset(&sa, 0, sizeof (sa));
	(void) sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;	/* no SA_RESTART: a slow call fails EINTR */
	sa.sa_handler = on_signal;
	for (i = 0; i < ZR_NSIG; i++)
		(void) sigaction(zr_sigs[i], &sa, &saved[i]);
}

static void
signals_restore(const struct sigaction *saved)
{
	size_t i;

	for (i = 0; i < ZR_NSIG; i++)
		(void) sigaction(zr_sigs[i], &saved[i], NULL);
}

/*
 * ZFS_REBASE_PAUSE=<gate>: the box harness's way into the middle of a
 * run. At the gate it names the tool stops itself with SIGSTOP; the
 * harness, which is waiting for exactly that, kills it, edits the
 * tree behind its back or looks at what it has written so far, and
 * sends SIGCONT. It is read once, at the start of a run and of the
 * verbs that pass a gate, and it exists for tests/box/run-kills.sh
 * and tests/box/run-strays.sh alone: it is documented in
 * tests/box/README.md, is in no usage text, and a name that is no
 * gate of ours is ignored in silence, since a test aid must never be
 * able to fail a real run.
 *
 * A gate is a point where the thing it names has just happened and
 * the next has not started:
 *
 *	held		the three holds are taken
 *	cloned		the clone is there, or the dataset is the
 *			run's own at its private mount, before any walk
 *	read		the walks and the pruning are done, before
 *			anything is decided
 *	manifest	the manifest is written and recorded, before
 *			the skeleton of the resolution is written
 *			beside it: the one window in which a rebase
 *			has one of its two documents and not the
 *			other
 *	decided		the manifest and the resolution are written and
 *			recorded, before applying1 is written
 *	applying1	that gate is written and readonly is off,
 *			before the first action
 *	conflicts	that gate is written, before the note that
 *			says what the run is waiting for
 *	applying2	that gate is written and readonly is off,
 *			before the choices are carried out
 *	done		that gate is written, before the release
 *	action:<n>	inside the apply, before the n'th action it
 *			performs (apply.c, zr_apply_pause_at)
 *	choice:<n>	inside applying2, before the n'th line of the
 *			resolution it carries out (apply.c,
 *			zr_apply_choice_pause_at)
 *
 * --posix reaches none of them and ignores the variable altogether.
 */
#define	ZR_PAUSE_ENV	"ZFS_REBASE_PAUSE"
#define	ZR_PAUSE_ACTION	"action:"
#define	ZR_PAUSE_CHOICE	"choice:"

static const char *zr_pause_gate;

/* The number after "action:" or "choice:", or 0 for anything else. */
static unsigned int
zr_pause_num(const char *s)
{
	unsigned long n;
	char *end;

	n = strtoul(s, &end, 10);
	if (*end != '\0' || n == 0 || n > UINT_MAX)
		return (0);
	return ((unsigned int)n);
}

static void
zr_pause_open(void)
{
	zr_pause_gate = getenv(ZR_PAUSE_ENV);
	if (zr_pause_gate == NULL)
		return;
	if (strncmp(zr_pause_gate, ZR_PAUSE_ACTION,
	    sizeof (ZR_PAUSE_ACTION) - 1) == 0)
		zr_apply_pause_at(zr_pause_num(zr_pause_gate +
		    sizeof (ZR_PAUSE_ACTION) - 1));
	else if (strncmp(zr_pause_gate, ZR_PAUSE_CHOICE,
	    sizeof (ZR_PAUSE_CHOICE) - 1) == 0)
		zr_apply_choice_pause_at(zr_pause_num(zr_pause_gate +
		    sizeof (ZR_PAUSE_CHOICE) - 1));
}

/* At this gate, and at no other, stop and wait for the harness. */
static void
zr_pause(const char *gate)
{
	if (zr_pause_gate != NULL && strcmp(zr_pause_gate, gate) == 0)
		(void) raise(SIGSTOP);
}

static int
fail(struct run *r, int code, const char *what)
{
	(void) fprintf(stderr, "zfs_rebase: %s: %s\n", what, r->err);
	return (code);
}

/* A phase boundary: has a signal come in while the last one ran? */
static int
stopped(struct run *r)
{
	if (zr_apply_stop == 0)
		return (0);
	(void) snprintf(r->err, sizeof (r->err), "interrupted");
	return (-1);
}

/*
 * A kept result is a rebase and not a leftover: it has its record and
 * its holds, and both verbs find it by the name the user gave.
 */
static void
kept_hint(const struct run *r)
{
	(void) fprintf(stderr, "zfs_rebase: %s is kept; zfs_rebase --continue "
	    "%s resumes it; zfs_rebase --abort %s removes it\n", r->rds,
	    r->rds, r->rds);
}

/*
 * The tag every hold of this rebase is filed under, and the tag its
 * record carries: "zr-" and twelve hex digits, being the second and
 * the pid. Two rebases that are alive at once cannot share those --
 * a pid belongs to one process at a time -- so no run can release
 * another's hold, which is the whole point of a tag. It is kept
 * short because zfs holds prints it, and it is derived rather than
 * random so that "never collides" is a fact and not a probability.
 */
static void
tag_make(char *buf, size_t len, const char *prefix)
{
	uint64_t v;

	v = ((uint64_t)(time(NULL) & 0x7fffffff) << 17) |
	    ((uint64_t)getpid() & 0x1ffff);
	(void) snprintf(buf, len, "%s%012llx", prefix,
	    (unsigned long long)v);
}

/* The dataset name of a snapshot or dataset argument, without @snap. */
static void
dataset_of(const char *arg, char *buf, size_t len)
{
	const char *at = strchr(arg, '@');
	size_t n = at != NULL ? (size_t)(at - arg) : strlen(arg);

	if (n >= len)
		n = len - 1;
	memcpy(buf, arg, n);
	buf[n] = '\0';
}

/*
 * The origin chain of a snapshot: the snapshot itself, then the
 * origin of its dataset, then the origin of that dataset, and so on
 * to a dataset that is not a clone. Every link is a snapshot that
 * exists, because ZFS will not destroy a clone's origin while the
 * clone lives; zfs promote re-roots the graph but leaves it a graph,
 * so the walk still ends. The dataset of a link is the part of the
 * name before its '@'. Returns the number of links, or -1.
 */
static int
origin_chain(struct run *r, const char *snap, char chain[][ZR_SNAP_MAX])
{
	char ds[ZR_SNAP_MAX], org[ZR_SNAP_MAX];
	int n;

	if (strlen(snap) >= ZR_SNAP_MAX) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", snap,
		    strerror(ENAMETOOLONG));
		return (-1);
	}
	(void) snprintf(chain[0], ZR_SNAP_MAX, "%s", snap);
	for (n = 1; ; n++) {
		dataset_of(chain[n - 1], ds, sizeof (ds));
		if (zr_zfs_get(r->zfs, ds, "origin", org, sizeof (org),
		    r->err, sizeof (r->err)) != 0)
			return (-1);
		if (strcmp(org, "-") == 0)
			return (n);
		if (n == ZR_CHAIN_MAX) {
			(void) snprintf(r->err, sizeof (r->err),
			    "%s sits more than %d origins deep", snap,
			    ZR_CHAIN_MAX);
			return (-1);
		}
		(void) snprintf(chain[n], ZR_SNAP_MAX, "%s", org);
	}
}

/*
 * The base is the branch point, and the run works it out rather than
 * being told. A base the user gave could only agree with this or
 * disagree with it, and a disagreement is not something the tool
 * could act on sensibly: the two sides are related the way the
 * origin graph says they are, and no other snapshot is the point
 * they last had in common.
 *
 * Walk from's chain outward and take the first dataset that onto's
 * chain has too -- the nearest dataset both descend from. Each side
 * names a snapshot of it, and the older of those two, by createtxg,
 * is the last state they agreed on.
 */
static int
derive_base(struct run *r)
{
	char fc[ZR_CHAIN_MAX][ZR_SNAP_MAX], oc[ZR_CHAIN_MAX][ZR_SNAP_MAX];
	char fds[ZR_SNAP_MAX], ods[ZR_SNAP_MAX];
	const char *a = NULL, *b = NULL, *base;
	uint64_t ta, tb;
	int nf, no, i, j;

	nf = origin_chain(r, r->fromsnap, fc);
	if (nf < 0)
		return (-1);
	no = origin_chain(r, r->ontosnap, oc);
	if (no < 0)
		return (-1);
	for (i = 0; i < nf && a == NULL; i++) {
		dataset_of(fc[i], fds, sizeof (fds));
		for (j = 0; j < no; j++) {
			dataset_of(oc[j], ods, sizeof (ods));
			if (strcmp(fds, ods) == 0) {
				a = fc[i];
				b = oc[j];
				break;
			}
		}
	}
	if (a == NULL) {
		(void) snprintf(r->err, sizeof (r->err),
		    "from and onto share no origin");
		return (-1);
	}
	/*
	 * createtxg orders them: it is the transaction the snapshot
	 * was taken in, the kernel's own count, and it is exact where
	 * a creation time would only be close.
	 */
	if (strcmp(a, b) == 0) {
		base = a;
	} else {
		if (zr_zfs_get_int(r->zfs, a, "createtxg", &ta, r->err,
		    sizeof (r->err)) != 0 ||
		    zr_zfs_get_int(r->zfs, b, "createtxg", &tb, r->err,
		    sizeof (r->err)) != 0)
			return (-1);
		base = ta <= tb ? a : b;
	}
	/*
	 * A base that is one of the two arguments means they never
	 * diverged: one is an ancestor of the other, and what the
	 * user wants there is not a rebase. Two snapshots of one
	 * dataset always land here, and so does a side given at or
	 * before the point the other forked from it.
	 */
	if (strcmp(base, r->fromsnap) == 0) {
		(void) snprintf(r->err, sizeof (r->err),
		    "onto already contains from; nothing to rebase");
		return (-1);
	}
	if (strcmp(base, r->ontosnap) == 0) {
		(void) snprintf(r->err, sizeof (r->err),
		    "from already contains onto; nothing to rebase");
		return (-1);
	}
	(void) snprintf(r->base, sizeof (r->base), "%s", base);
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: the base is %s\n",
		    r->base);
	return (0);
}

/* Below with the two sides, whose syntax --base shares. */
static int is_snapshot(const char *arg);

/*
 * --allow-unrelated: the two sides share no origin, so there is no
 * branch point to work out and the base is whatever the user says it
 * is -- or nothing at all.
 *
 * --base names a snapshot that exists and that neither side is older
 * than. A rebase replays what each side did after the base, so a
 * base taken after a side describes a state that side never passed
 * through and there is nothing sensible to replay against it.
 * createtxg orders them: it is the transaction the snapshot was
 * taken in, the kernel's own count, exact where a creation time
 * would only be close. Equal is allowed -- a base taken in the same
 * transaction as a side is that side's own state. That the base is
 * in one pool with the two sides, that its dataset is mounted, and
 * that it names as they do is checked with theirs below, since its
 * tree is read exactly the way theirs is.
 *
 * --base is not optional here (ruled 2026-09-06): with no branch
 * point to derive and no base given there is nothing to read the two
 * sides against, and the empty tree in its place made every name of
 * either side an add, which is a decision about two trees that were
 * never compared. The driver refuses the flag without it as a usage
 * error, and this is the same refusal for a caller of its own.
 */
static int
unrelated_base(struct run *r)
{
	static const char *const word[2] = { "from", "onto" };
	const char *side[2];
	uint64_t tb, ts;
	int i, rc;

	side[0] = r->fromsnap;
	side[1] = r->ontosnap;
	/*
	 * The one thing the derivation refuses that still holds here:
	 * one snapshot given twice is not two sides.
	 */
	if (strcmp(side[0], side[1]) == 0) {
		(void) snprintf(r->err, sizeof (r->err),
		    "from and onto are one snapshot; nothing to rebase");
		return (-1);
	}
	if (r->o.base == NULL) {
		(void) snprintf(r->err, sizeof (r->err),
		    "--allow-unrelated needs --base: there is no branch point "
		    "to derive and none to fall back on");
		return (-1);
	}
	if (!is_snapshot(r->o.base)) {
		(void) snprintf(r->err, sizeof (r->err),
		    "%s is a dataset, and --base wants a snapshot",
		    r->o.base);
		return (-1);
	}
	rc = zr_zfs_exists(r->zfs, r->o.base, r->err, sizeof (r->err));
	if (rc < 0)
		return (-1);
	if (rc == 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s does not exist",
		    r->o.base);
		return (-1);
	}
	if (zr_zfs_get_int(r->zfs, r->o.base, "createtxg", &tb, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
	for (i = 0; i < 2; i++) {
		if (zr_zfs_get_int(r->zfs, side[i], "createtxg", &ts, r->err,
		    sizeof (r->err)) != 0)
			return (-1);
		if (tb > ts) {
			(void) snprintf(r->err, sizeof (r->err), "base is "
			    "newer than %s: %s was taken in txg %llu and %s "
			    "in %llu", word[i], r->o.base,
			    (unsigned long long)tb, side[i],
			    (unsigned long long)ts);
			return (-1);
		}
	}
	(void) snprintf(r->base, sizeof (r->base), "%s", r->o.base);
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: the base is %s, as "
		    "given\n", r->base);
	return (0);
}

/*
 * The result must be in onto's pool, because a clone cannot cross
 * one; it must not exist yet; and its parent dataset must, because
 * the clone is created and not received and ZFS creates no
 * intermediate datasets for it.
 */
static int
result_ok(struct run *r, const char *ontods)
{
	char parent[ZR_NAME_MAX];
	const char *slash;
	size_t a, b;
	int rc;

	a = strcspn(ontods, "/");
	b = strcspn(r->o.result, "/");
	if (a != b || strncmp(ontods, r->o.result, a) != 0) {
		(void) snprintf(r->err, sizeof (r->err),
		    "%s is not in the pool %s is in", r->o.result, ontods);
		return (-1);
	}
	rc = zr_zfs_exists(r->zfs, r->o.result, r->err, sizeof (r->err));
	if (rc < 0)
		return (-1);
	if (rc != 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s exists already",
		    r->o.result);
		return (-1);
	}
	slash = strrchr(r->o.result, '/');
	if (slash == NULL) {
		(void) snprintf(r->err, sizeof (r->err),
		    "%s has no parent dataset", r->o.result);
		return (-1);
	}
	(void) snprintf(parent, sizeof (parent), "%.*s",
	    (int)(slash - r->o.result), r->o.result);
	rc = zr_zfs_exists(r->zfs, parent, r->err, sizeof (r->err));
	if (rc < 0)
		return (-1);
	if (rc == 0) {
		(void) snprintf(r->err, sizeof (r->err),
		    "%s does not exist, so %s cannot be created", parent,
		    r->o.result);
		return (-1);
	}
	return (0);
}

/*
 * The base is worked out first, since the rest of the checks are
 * about its dataset as much as the two sides'. Then every dataset
 * must be mounted, and they must agree on names. There are always
 * three: --allow-unrelated skips the derivation and takes the base
 * from --base, which it needs.
 *
 * All three of those are read as the numbers they are: mounted is a
 * boolean and the other two are index properties, so the words zfs(8)
 * would print are a rendering of the value and not the value
 * (sprints/sprint-5/string-audit.md, section 5). What each number
 * has to be is in zfsops.h, which says what fixes it in the OpenZFS
 * tree.
 */
static int
preconditions(struct run *r)
{
	static const char *const props[] = { "casesensitivity",
	    "normalization" };
	static const uint64_t want[] = { ZR_CASE_SENSITIVE,
	    ZR_NORMALIZE_NONE };
	static const char *const wantword[] = { "sensitive", "none" };
	char ds[3][ZR_NAME_MAX];
	uint64_t v;
	int i, p;

	if (r->o.unrelated) {
		if (unrelated_base(r) != 0)
			return (-1);
	} else {
		if (derive_base(r) != 0)
			return (-1);
		/*
		 * The base is the branch point of the two sides, so
		 * the three snapshots share one object-number space
		 * and the walks can say what is unchanged. That
		 * derivation is the only thing that licenses it:
		 * --posix has no base at all and never comes here,
		 * and --allow-unrelated leaves this clear, base or no
		 * base, because a name that two lineages happen to
		 * hold under one object number is not one object.
		 */
		r->prune = 1;
	}
	/* The three datasets: the base, derived or given, and the two sides. */
	dataset_of(r->base, ds[0], sizeof (ds[0]));
	dataset_of(r->fromsnap, ds[1], sizeof (ds[1]));
	dataset_of(r->ontosnap, ds[2], sizeof (ds[2]));
	for (i = 0; i < 3; i++) {
		if (zr_zfs_get_int(r->zfs, ds[i], "mounted", &v, r->err,
		    sizeof (r->err)) != 0)
			return (-1);
		if (v == ZR_NOT_MOUNTED) {
			(void) snprintf(r->err, sizeof (r->err),
			    "%s is not mounted", ds[i]);
			return (-1);
		}
		for (p = 0; p < 2; p++) {
			if (zr_zfs_get_int(r->zfs, ds[i], props[p], &v,
			    r->err, sizeof (r->err)) != 0)
				return (-1);
			if (v != want[p]) {
				(void) snprintf(r->err, sizeof (r->err),
				    "%s has %s index %llu; need %s", ds[i],
				    props[p], (unsigned long long)v,
				    wantword[p]);
				return (-1);
			}
		}
	}
	/* one pool: the name before the first slash must agree */
	for (i = 1; i < 3; i++) {
		size_t a = strcspn(ds[0], "/"), b = strcspn(ds[i], "/");

		if (a != b || strncmp(ds[0], ds[i], a) != 0) {
			(void) snprintf(r->err, sizeof (r->err),
			    "%s and %s are not in one pool", ds[0], ds[i]);
			return (-1);
		}
	}
	/*
	 * The clone form's result is a dataset that has to be made;
	 * the dataset form's is onto itself, which was checked before
	 * its snapshot was taken.
	 */
	if (!r->o.dryrun && !in_dataset_form(r) && result_ok(r, ds[2]) != 0)
		return (-1);
	if (zr_zfs_get(r->zfs, ds[0], "mountpoint", r->basemnt,
	    sizeof (r->basemnt), r->err, sizeof (r->err)) != 0)
		return (-1);
	if (zr_zfs_get(r->zfs, ds[1], "mountpoint", r->frommnt,
	    sizeof (r->frommnt), r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get(r->zfs, ds[2], "mountpoint", r->ontomnt,
	    sizeof (r->ontomnt), r->err, sizeof (r->err)) != 0)
		return (-1);
	return (0);
}

/*
 * An argument with an '@' in it is a snapshot and an argument
 * without one is a dataset. That is the whole of the syntax, and
 * --onto's answer decides the form of the run.
 */
static int
is_snapshot(const char *arg)
{
	return (strchr(arg, '@') != NULL);
}

/*
 * The snapshot the tool takes of a side the user gave as a dataset.
 * The name is <dataset>@zfs_rebase-<tag>, the tag being the run's
 * own hold tag, which is already unique to the run; a name that is
 * somehow taken is tried again with -2, -3 and so on, and the
 * generated snapshot is recorded as tool-made and lives exactly as
 * long as the rebase.
 */
static int
snapshot_input(struct run *r, const char *dataset, char *buf, size_t buflen)
{
	int i, n, rc;

	for (i = 1; i <= ZR_MADE_TRIES; i++) {
		if (i == 1)
			n = snprintf(buf, buflen, "%s@%s%s", dataset,
			    ZR_MADE_PREFIX, r->tag);
		else
			n = snprintf(buf, buflen, "%s@%s%s-%d", dataset,
			    ZR_MADE_PREFIX, r->tag, i);
		if (n < 0 || (size_t)n >= buflen) {
			(void) snprintf(r->err, sizeof (r->err), "%s: %s",
			    dataset, strerror(ENAMETOOLONG));
			return (-1);
		}
		rc = zr_zfs_snapshot(r->zfs, buf, r->err, sizeof (r->err));
		if (rc < 0)
			return (-1);
		if (rc == 0) {
			if (r->o.verbose)
				(void) fprintf(stderr, "zfs_rebase: took %s\n",
				    buf);
			return (0);
		}
	}
	(void) snprintf(r->err, sizeof (r->err),
	    "%s: no unused name for a snapshot of it", dataset);
	buf[0] = '\0';
	return (-1);
}

/*
 * The pre-apply snapshot of the dataset form, as --result spells it:
 * the short name after the '@', or a full name whose dataset part is
 * onto itself. Anything else is a name for another dataset's
 * snapshot, and a rebase that took it would be writing the record on
 * one dataset and the before-image of another.
 */
static int
result_snapshot(struct run *r)
{
	const char *at = strchr(r->o.result, '@');
	const char *shortname = at != NULL ? at + 1 : r->o.result;
	size_t n;

	if (at != NULL) {
		n = (size_t)(at - r->o.result);
		if (n != strlen(r->ontods) ||
		    strncmp(r->o.result, r->ontods, n) != 0) {
			(void) snprintf(r->err, sizeof (r->err), "--result %s "
			    "is not a snapshot of %s, which --onto names",
			    r->o.result, r->ontods);
			return (-1);
		}
	}
	if (shortname[0] == '\0' || strchr(shortname, '/') != NULL ||
	    strchr(shortname, '@') != NULL) {
		(void) snprintf(r->err, sizeof (r->err), "--result %s is no "
		    "name for a snapshot of %s", r->o.result, r->ontods);
		return (-1);
	}
	if ((size_t)snprintf(r->ontosnap, sizeof (r->ontosnap), "%s@%s",
	    r->ontods, shortname) >= sizeof (r->ontosnap)) {
		(void) snprintf(r->err, sizeof (r->err), "%s@%s: %s",
		    r->ontods, shortname, strerror(ENAMETOOLONG));
		return (-1);
	}
	return (0);
}

/*
 * Which form the run is in, and what the record will live on. The
 * clone form's result is the dataset --result names; the dataset
 * form's is onto itself, and --result names the snapshot taken of it
 * before anything is applied.
 *
 * A dry run creates nothing and ignores --result, so it has no
 * pre-apply snapshot to name: it takes a snapshot of its own of a
 * dataset onto, exactly as it does of a dataset from, and destroys
 * it again before it exits. It must read something, and a snapshot
 * is what a rebase reads.
 */
static int
choose_form(struct run *r)
{
	dataset_of(r->o.onto, r->ontods, sizeof (r->ontods));
	if (is_snapshot(r->o.onto)) {
		r->form = ZR_FORM_CLONE;
		(void) snprintf(r->ontosnap, sizeof (r->ontosnap), "%s",
		    r->o.onto);
		if (r->o.result == NULL)
			return (0);	/* a dry run names nothing */
		if (is_snapshot(r->o.result)) {
			(void) snprintf(r->err, sizeof (r->err), "--onto names "
			    "a snapshot, so --result names the dataset to "
			    "clone it as, and %s is a snapshot",
			    r->o.result);
			return (-1);
		}
		(void) snprintf(r->rds, sizeof (r->rds), "%s", r->o.result);
		return (0);
	}
	r->form = ZR_FORM_DATASET;
	(void) snprintf(r->rds, sizeof (r->rds), "%s", r->ontods);
	if (r->o.dryrun)
		return (0);
	return (result_snapshot(r));
}

/*
 * What the dataset form must know before it takes a snapshot of the
 * user's dataset: that the dataset is there and mounted, what its
 * readonly property is, and whether a rebase is already recorded on
 * it.
 *
 * Any one of the record's properties, as the dataset's own local
 * value, is an open rebase -- there is no phase until the first
 * gate, so a run killed before applying1 carries only the manifest
 * and the tag, and a rebase that reached done carries none of them
 * at all -- and no flag overrides that: --continue or --abort
 * settles it first. A dataset carrying none is free, whatever its
 * history.
 */
static int
onto_open(struct run *r)
{
	char val[ZR_NAME_MAX], phase[64], tag[ZR_TAG_MAX];
	uint64_t v;
	size_t i;
	int got;

	got = zr_zfs_exists(r->zfs, r->ontods, r->err, sizeof (r->err));
	if (got < 0)
		return (-1);
	if (got == 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s does not exist",
		    r->ontods);
		return (-1);
	}
	/*
	 * The two properties the run takes away and gives back, read
	 * here and nowhere else: what they are is what they were
	 * before this rebase touched anything, and the manifest's
	 * header is written from these and not from a second read
	 * after the take, which would find the tool's own values.
	 */
	if (zr_zfs_get(r->zfs, r->ontods, "readonly", r->roorig,
	    sizeof (r->roorig), r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get(r->zfs, r->ontods, "canmount", r->cmorig,
	    sizeof (r->cmorig), r->err, sizeof (r->err)) != 0)
		return (-1);
	/*
	 * canmount=off is a dataset with no home to be handed back
	 * to: the rebase ends by mounting it where it belongs, and a
	 * dataset that will not mount has no such place. Asked before
	 * the mounted question, which such a dataset fails too, so
	 * that the answer names the property and not the symptom; and
	 * before its snapshot is taken and before anything at all is
	 * written (documents-design.md, section 5).
	 */
	if (strcmp(r->cmorig, "off") == 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s has "
		    "canmount=off and no place to be handed back to; the "
		    "dataset form ends by mounting it where it belongs",
		    r->ontods);
		return (-1);
	}
	if (zr_zfs_get_int(r->zfs, r->ontods, "mounted", &v, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
	if (v == ZR_NOT_MOUNTED) {
		(void) snprintf(r->err, sizeof (r->err), "%s is not mounted",
		    r->ontods);
		return (-1);
	}
	got = 0;
	for (i = 0; i < ZR_NRECORD && got == 0; i++) {
		got = zr_zfs_get_user(r->zfs, r->ontods, zr_record_props[i],
		    val, sizeof (val), r->err, sizeof (r->err));
		if (got < 0)
			return (-1);
	}
	if (got == 0)
		return (0);		/* no record: a fresh dataset form */
	phase[0] = '\0';
	tag[0] = '\0';
	if (zr_zfs_get_user(r->zfs, r->ontods, ZR_PROP_PHASE, phase,
	    sizeof (phase), r->err, sizeof (r->err)) < 0 ||
	    zr_zfs_get_user(r->zfs, r->ontods, ZR_PROP_TAG, tag,
	    sizeof (tag), r->err, sizeof (r->err)) < 0)
		return (-1);
	if (tag[0] != '\0')
		(void) snprintf(r->err, sizeof (r->err), "%s carries a rebase "
		    "at \"%s\" under %s; a rebase is open here: --continue or "
		    "--abort settles it first", r->ontods,
		    phase[0] != '\0' ? phase : "no gate yet", tag);
	else
		(void) snprintf(r->err, sizeof (r->err), "%s carries %s of a "
		    "rebase and no hold tag; --abort settles it first",
		    r->ontods, zr_record_props[i - 1]);
	return (-1);
}

/*
 * Every component of path, made 0700 and allowed to be there
 * already. The dataset name character set is [A-Za-z0-9_.:-] and the
 * separator, so a dataset name is a path already and the tree under
 * WORKDIR mirrors the dataset tree. A rebase can outlive a reboot,
 * so the verbs make these again rather than assume them.
 */
static int
mkdir_p(const char *path, char *err, size_t errlen)
{
	char buf[ZR_NAME_MAX];
	char *p;

	if ((size_t)snprintf(buf, sizeof (buf), "%s", path) >= sizeof (buf)) {
		(void) snprintf(err, errlen, "%s: %s", path,
		    strerror(ENAMETOOLONG));
		return (-1);
	}
	for (p = buf + 1; ; p++) {
		if (*p != '/' && *p != '\0')
			continue;
		if (*p == '/') {
			*p = '\0';
			if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
				(void) snprintf(err, errlen, "%s: %s", buf,
				    strerror(errno));
				return (-1);
			}
			*p = '/';
			continue;
		}
		if (mkdir(buf, 0700) != 0 && errno != EEXIST) {
			(void) snprintf(err, errlen, "%s: %s", buf,
			    strerror(errno));
			return (-1);
		}
		return (0);
	}
}

/*
 * WORKDIR as the filesystem knows it, which is what every path under
 * it is built from and every path is compared against (R26 of the
 * code review). The manifest path a record carries went through
 * realpath when it was written, so the prefix it is held against has
 * to be resolved too: with a symlink anywhere in /var/db/zfs_rebase
 * a literal prefix would make a run's own documents read as the
 * user's -o pair and leave them behind. Latent on FreeBSD, where
 * /var/db is a real directory, and one call either way.
 *
 * It is resolved once and kept, since the answer cannot change under
 * a running verb. The tool makes the directory itself, so a call
 * that comes before it exists cannot resolve and falls back to the
 * literal name without caching it; the next call, after mkdir_p, has
 * the real one.
 */
static const char *
workdir(void)
{
	static char resolved[ZR_NAME_MAX];
	char buf[PATH_MAX];

	if (resolved[0] != '\0')
		return (resolved);
	if (realpath(WORKDIR, buf) == NULL ||
	    (size_t)snprintf(resolved, sizeof (resolved), "%s", buf) >=
	    sizeof (resolved)) {
		resolved[0] = '\0';
		return (WORKDIR);
	}
	return (resolved);
}

/*
 * The run directory of one result: WORKDIR/<result>, with the name
 * held against ZFS's own rule before it is ever a path (R19 of the
 * code review). The tree under WORKDIR mirrors the dataset tree
 * because a dataset name is a path already, which is true only of
 * names ZFS would take: "../../../../tmp/x" is not one, and --abort
 * is the verb that goes on when the dataset does not exist, so an
 * unvalidated name reaching rmdir_run is a name reaching rmdir. The
 * containment there is a prefix test and not a parse, and this is
 * what makes the prefix mean what it says.
 *
 * err may be NULL, for the callers that have nothing to report to.
 * Returns 0, or -1.
 */
static int
rundir_of(char *buf, size_t len, const char *result, char *err, size_t errlen)
{
	const char *top = workdir();

	if (result == NULL || zr_zfs_name_valid(result, 0, err, errlen) <= 0) {
		buf[0] = '\0';
		return (-1);
	}
	if ((size_t)snprintf(buf, len, "%s/%s", top, result) >= len) {
		if (err != NULL && errlen > 0)
			(void) snprintf(err, errlen, "%s/%s: %s", top, result,
			    strerror(ENAMETOOLONG));
		buf[0] = '\0';
		return (-1);
	}
	return (0);
}

/*
 * WORKDIR/<result>, root-only, so nothing else can look in. Every
 * parent may exist; the leaf may not, because a leaf that is there
 * is another run of this result.
 */
static int
make_rundir(struct run *r)
{
	char parent[ZR_NAME_MAX];
	char *slash;

	if (rundir_of(r->rundir, sizeof (r->rundir), r->rds, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
	(void) snprintf(parent, sizeof (parent), "%s", r->rundir);
	slash = strrchr(parent, '/');
	*slash = '\0';
	if (mkdir_p(parent, r->err, sizeof (r->err)) != 0)
		return (-1);
	if (mkdir(r->rundir, 0700) != 0) {
		if (errno == EEXIST)
			(void) snprintf(r->err, sizeof (r->err),
			    "a run for %s is in place (%s); where %s carries "
			    "no record that directory is what a crash left "
			    "before the record was written, and zfs_rebase "
			    "--abort %s removes either", r->rds,
			    r->rundir, r->rds, r->rds);
		else
			(void) snprintf(r->err, sizeof (r->err), "%s: %s",
			    r->rundir, strerror(errno));
		return (-1);
	}
	r->dirmade = 1;
	(void) snprintf(r->workmnt, sizeof (r->workmnt), "%s/mnt", r->rundir);
	if (mkdir(r->workmnt, 0700) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", r->workmnt,
		    strerror(errno));
		return (-1);
	}
	return (0);
}

/*
 * Take the run's directory tree away again: the mount point, the
 * directory itself, then every parent up to but not including
 * WORKDIR for as long as rmdir keeps succeeding. Nothing is ever
 * removed recursively, so a directory another run shares simply
 * refuses to go and the walk stops there.
 *
 * What comes back is the run directory's own errno, or 0 where it
 * went or was gone already: that one is the caller's to report,
 * since a directory that will not go is a rebase's leavings. A
 * parent that will not go is not, so the walk swallows those and
 * stops.
 */
static int
rmdir_run(const char *result)
{
	size_t top = strlen(workdir());
	char dir[ZR_NAME_MAX], mnt[ZR_NAME_MAX];
	char *slash;

	if (rundir_of(dir, sizeof (dir), result, NULL, 0) != 0)
		return (EINVAL);
	if ((size_t)snprintf(mnt, sizeof (mnt), "%s/mnt", dir) >=
	    sizeof (mnt))
		return (ENAMETOOLONG);
	(void) rmdir(mnt);
	if (rmdir(dir) != 0 && errno != ENOENT)
		return (errno);
	for (;;) {
		slash = strrchr(dir, '/');
		if (slash == NULL || (size_t)(slash - dir) <= top)
			return (0);
		*slash = '\0';
		if (rmdir(dir) != 0)
			return (0);
	}
}

/*
 * Where the resolution of this result goes when the manifest went to
 * the run directory: beside it, under the name every run before -o
 * used. It is also what a record written before the resolution had a
 * property of its own is read as.
 */
static void
resolution_path(char *buf, size_t len, const char *result)
{
	char dir[ZR_NAME_MAX];

	if (rundir_of(dir, sizeof (dir), result, NULL, 0) != 0) {
		buf[0] = '\0';
		return;
	}
	(void) snprintf(buf, len, "%s/%s", dir, ZR_RESOLUTION);
}

/*
 * And where it goes when -o named the manifest: FILE.resolution,
 * beside FILE, which is what "beside the manifest" means once the
 * user has chosen where the manifest lives. The suffix is appended
 * rather than substituted, so that no -o path can name a file the
 * user meant to keep.
 */
static void
resolution_beside(char *buf, size_t len, const char *manifest)
{
	(void) snprintf(buf, len, "%s.%s", manifest, ZR_RESOLUTION);
}

/*
 * Is the manifest the record names inside this result's run
 * directory? That is one string compare against the directory's path
 * and no parse of either side: the recorded path went through
 * realpath when it was written, so both are resolved and a prefix
 * means what it says. Everything the tool removes turns on this. The
 * run's own two documents live in the directory and go with it; a -o
 * manifest and the resolution beside it are the user's, wherever
 * they asked for them, and the tool removes no file outside the run
 * directory -- at done and at --abort alike (documents-design.md,
 * section 4).
 */
static int
in_rundir(const char *result, const char *manifest)
{
	char dir[ZR_NAME_MAX];
	size_t n;

	if (manifest == NULL || manifest[0] == '\0')
		return (0);
	if (rundir_of(dir, sizeof (dir), result, NULL, 0) != 0)
		return (0);
	n = strlen(dir);
	if (n + 1 >= sizeof (dir))
		return (0);
	dir[n++] = '/';
	dir[n] = '\0';
	return (strncmp(manifest, dir, n) == 0);
}

/*
 * And the rule itself, which is the only thing that says where a
 * resolution is: beside the manifest the record names, which means
 * <rundir>/resolution when the manifest is in the run directory and
 * MANIFEST.resolution when -o put it anywhere else. The record
 * carries no path of its own for it -- one document names the run,
 * and the other is beside it (documents-design.md, section 2).
 */
static void
resolution_of(char *buf, size_t len, const char *result, const char *manifest)
{
	if (manifest != NULL && manifest[0] != '\0' &&
	    !in_rundir(result, manifest))
		resolution_beside(buf, len, manifest);
	else
		resolution_path(buf, len, result);
}

/*
 * One document of a run, taken away. A file already gone is not a
 * failure -- an --abort can follow an --abort, and a person may have
 * tidied -- and one that will not go is said and nothing more: it is
 * a file, and the rebase it belonged to is over either way.
 */
static void
unlink_doc(const char *path)
{
	if (unlink(path) != 0 && errno != ENOENT)
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", path,
		    strerror(errno));
}

/*
 * ---------------------------------------------------------------
 * The settle: what every shutdown path does, and the order it does
 * it in (documents-design.md, section 11.3).
 *
 * The resources of a rebase, in the order a run acquires them:
 *
 *	1. the pre-apply snapshot (dataset form)
 *	2. the run directory (the lock)
 *	3. the birth manifest
 *	4. the record
 *	5. the holds
 *	6. the snapshot the tool took of from (#made)
 *	7. the take: readonly, canmount, the private mount
 *	8. the clone (clone form) -- created at step 4 with the record
 *	9. the decision manifest and the skeleton
 *
 * Every path that ends a rebase -- the done gate, --abort in either
 * form, and a run that takes itself away whole -- gives them back in
 * reverse and gives the metadata back last: the result is put into
 * working order before the tool loses any identifier it would need
 * to try again. So the walks are closed, the result is handed back
 * or destroyed, and only then are the holds released, the record
 * taken off and the run directory removed. The record names the tag,
 * and the tag is the only handle on the holds, so the holds go
 * first of those three and the record after them; the directory is
 * last, because it holds the documents the earlier steps read.
 *
 * The pre-apply snapshot and the tool's own from snapshot are
 * destroyed after the holds and never before them: a held snapshot
 * will not be destroyed. In the dataset form the pre-apply snapshot
 * is the header's #onto as well, so it is one of the three the tag
 * is released on.
 *
 * The unmount is the one step that can refuse, and it refuses for
 * the ordinary reason: somebody is standing in the private mount,
 * which at the conflicts gate is where they were asked to stand. So
 * nothing after it happens -- the record and the holds stay, the
 * mount path and the reason are printed, and the exit is 3 -- and
 * the next --continue or --abort finishes the settle when the mount
 * is free. 3 and not 2: 2 says a verb was refused before anything
 * was touched, and by this point the whole rebase has been applied.
 * The done gate's other 3, the drift verdict, is told apart by the
 * report: drift says the rebase is over and this says it is not.
 * done blocks on this and on nothing else.
 * ---------------------------------------------------------------
 */

/*
 * The run directory at done, which is where a rebase's traces end:
 * the two documents the run wrote there are unlinked -- and only
 * those, since a -o pair is the user's and stays -- and then mnt,
 * the directory and every empty parent up to WORKDIR go by rmdir,
 * never recursively. The settle came first, so mnt is empty: the
 * clone was handed to the void and the dataset home, both after the
 * walks were closed.
 *
 * Nothing here can change what the invocation returns. The rebase is
 * done -- the record is off the result and the holds are given back
 * -- and a directory that will not go costs the next run of this
 * result the EEXIST make_rundir raises and nothing else. So every
 * failure is reported and none is passed up.
 */
static void
rundir_done(const char *result, const char *manifest, int verbose)
{
	char resolution[ZR_NAME_MAX];
	int e;

	if (in_rundir(result, manifest)) {
		unlink_doc(manifest);
		resolution_of(resolution, sizeof (resolution), result,
		    manifest);
		unlink_doc(resolution);
	}
	e = rmdir_run(result);
	if (e != 0)
		(void) fprintf(stderr, "zfs_rebase: %s/%s: %s\n", workdir(),
		    result, strerror(e));
	else if (verbose)
		(void) fprintf(stderr, "zfs_rebase: removed %s/%s\n",
		    workdir(), result);
}

/*
 * Take the record off the result, which is what done does and what
 * --abort does in the dataset form: a clone carries its record away
 * with it when it is destroyed, and a dataset of the user's has to
 * be left as it was found. Inheriting a user property is how it is
 * removed, and one that is not there is not a failure, so this can
 * be run again.
 * Each failure warns and the rest still go: a record half taken off
 * is a record no verb will read, since the tag and the manifest are
 * what say "a rebase is here".
 */
static void
clear_record(struct zr_zfs *z, const char *dataset, int verbose)
{
	char e[512];
	size_t i;

	for (i = 0; i < ZR_NRECORD; i++) {
		if (zr_zfs_clear_user(z, dataset, zr_record_props[i], e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: %s on %s: %s\n",
			    zr_record_props[i], dataset, e);
	}
	if (verbose)
		(void) fprintf(stderr, "zfs_rebase: the record is off %s\n",
		    dataset);
}

/*
 * The dataset form's exclusivity, and it is the unmount that proves
 * it: a dataset somebody has a file open in, or a working directory
 * in, or a child dataset mounted under, will not unmount, and that
 * is the refusal. Nothing is forced. What the tool then has is a
 * dataset mounted where only it can reach, with the mountpoint
 * property untouched, so that giving it back is one mount call.
 *
 * The readonly property is touched only while the dataset is off its
 * mountpoint: here, before the private mount, and at the hand-back,
 * after the private mount is undone. libzfs answers a change of
 * readonly on a mounted dataset with a remount at the mountpoint
 * property, and the reason is not the changelist: readonly makes an
 * empty one, since changelist_gather (lib/libzfs/
 * libzfs_changelist.c) returns as soon as the property is not
 * mountpoint, sharenfs or sharesmb, so changelist_postfix has
 * nothing to walk. It is the namespace-property path instead:
 * readonly is one of the properties zfs_is_namespace_prop names
 * (lib/libzfs/libzfs_mount.c), and zfs_prop_set calls
 * zfs_mount_setattr for those whenever the dataset is mounted
 * (lib/libzfs/libzfs_dataset.c), which on FreeBSD has no
 * mount_setattr(2) and falls back to zfs_mount(zhp, MNTOPT_REMOUNT,
 * 0) (lib/libzfs/os/freebsd/libzfs_zmount.c). zfs_mount reads the
 * mountpoint property to find where to remount, so the remount
 * lands on a directory nothing is mounted on while the dataset sits
 * at the private mount, and comes back EINVAL from the kernel. The
 * kernel itself needs no remount -- readonly_changed_cb applies the
 * property to the live mount -- but libzfs makes one anyway. (The
 * clone form is exempt for the same reason: zfs_mount returns 0
 * without acting for a mountpoint of none.) So a dataset that was
 * read-only is made writable once, unmounted, for the private
 * mount's whole life, the header keeping what it was; the private
 * mount is root's alone and the per-stage flips are the clone
 * form's.
 */
static int
private_rw(struct zr_zfs *z, const char *dataset, char *err, size_t errlen)
{
	char ro[16];

	if (zr_zfs_get(z, dataset, "readonly", ro, sizeof (ro), err,
	    errlen) != 0)
		return (-1);
	if (strcmp(ro, "on") != 0)
		return (0);
	return (zr_zfs_set_readonly(z, dataset, 0, err, errlen));
}

/*
 * Give the dataset back to service: off the private mount, readonly
 * and canmount as the manifest's header says they were, and mounted
 * where its own mountpoint property says. A rebase reaches this
 * exactly twice -- at done and at --abort -- and at no gate in
 * between: a dataset waiting for its conflicts to be answered is a
 * half rebased tree, and a half rebased tree is not put back into
 * service (documents-design.md, section 5). A run that takes itself
 * away whole before it has written anything reaches it too, since
 * that is an --abort in all but name, and so does the take's own way
 * out when the private mount cannot be made.
 *
 * Only the private mount is undone. A dataset found at its own place
 * -- after an --abort that follows a mount by hand -- is left where
 * it is rather than unmounted for nothing, and one found nowhere is
 * mounted. Both properties go back before the mount, so that the
 * readonly change never meets a dataset mounted at the private mount
 * and the noauto the run wrote is gone before anything can act on
 * it.
 *
 * Returns 0 where the dataset is in service again, and -1 where it
 * is not: the unmount refused, or the mount did. Nothing after this
 * may run on a -1 -- not the release, not the record, not the
 * directory -- because the way back to this point is the record and
 * the header it names (the settle's comment above). A failure to put
 * one of the two properties back is not one of those: it is said and
 * the hand-back goes on, since a dataset at home with the wrong
 * readonly is one zfs set away and a dataset nowhere is not.
 */
static int
handback(struct zr_zfs *z, const char *dataset, const char *ro,
    const char *cm, const char *mnt, int verbose)
{
	char at[ZR_NAME_MAX], home[ZR_NAME_MAX], e[512];
	int rc;

	rc = zr_zfs_mounted_at(z, dataset, at, sizeof (at), e, sizeof (e));
	if (rc < 0)
		(void) fprintf(stderr, "zfs_rebase: where %s is mounted: "
		    "%s\n", dataset, e);
	if (rc > 0 && mnt != NULL && strcmp(at, mnt) == 0) {
		if (zr_zfs_unmount(z, dataset, e, sizeof (e)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: %s will not "
			    "unmount from %s: %s\n", dataset, mnt, e);
			return (-1);
		}
		rc = 0;
	}
	if (ro != NULL && ro[0] != '\0' &&
	    zr_zfs_set_readonly(z, dataset, strcmp(ro, "on") == 0, e,
	    sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: readonly=%s on %s: %s\n",
		    ro, dataset, e);
	if (cm != NULL && cm[0] != '\0' &&
	    zr_zfs_set_canmount(z, dataset, cm, e, sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: canmount=%s on %s: %s\n",
		    cm, dataset, e);
	/*
	 * And where it is now, asked again rather than assumed. A
	 * canmount change makes no changelist at all unless the new
	 * value is off on a mounted dataset (zfs_prop_set_list_flags,
	 * lib/libzfs/libzfs_dataset.c), so nothing above can have
	 * mounted it; the question is one property read, and its
	 * answer is what says whether to mount at all.
	 */
	if (rc == 0)
		rc = zr_zfs_mounted_at(z, dataset, at, sizeof (at), e,
		    sizeof (e));
	if (rc == 0 && zr_zfs_mount(z, dataset, e, sizeof (e)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s will not mount: %s\n",
		    dataset, e);
		return (-1);
	}
	/*
	 * And the answer asked for rather than assumed, which is what
	 * says the dataset is in service: zfs_mount returns 0 without
	 * acting where the mountpoint property is none or legacy
	 * (zfs_is_mountable, lib/libzfs/libzfs_mount.c), so the call
	 * above succeeding is not the same as the dataset being
	 * somewhere. A mountpoint that is not a path is not a failure
	 * -- there is nowhere for the tool to put it, and the person
	 * who set legacy mounts it their own way -- and it is said
	 * rather than passed over.
	 */
	if (rc == 0)
		rc = zr_zfs_mounted_at(z, dataset, at, sizeof (at), e,
		    sizeof (e));
	if (rc <= 0) {
		home[0] = '\0';
		(void) zr_zfs_get(z, dataset, "mountpoint", home,
		    sizeof (home), e, sizeof (e));
		if (home[0] != '\0' && home[0] == '/') {
			(void) fprintf(stderr, "zfs_rebase: %s is not mounted "
			    "at %s, which its mountpoint property names\n",
			    dataset, home);
			return (-1);
		}
		(void) fprintf(stderr, "zfs_rebase: %s has no mountpoint of "
		    "its own (%s) and is left unmounted\n", dataset,
		    home[0] != '\0' ? home : "unreadable");
		return (0);
	}
	if (verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is back where it "
		    "belongs, mounted at %s\n", dataset, at);
	return (0);
}

/*
 * The clone form's end of a rebase, which is no hand-back at all:
 * the clone has no home to go to, because its mountpoint property
 * was never a path. The private mount is undone and the clone is
 * left as a finished rebase should be -- unmounted, read-only, the
 * mountpoint property still none -- and the one useful thing the
 * tool can say is how to place it, which is the user's work and not
 * the tool's.
 *
 * Returns 0, or -1 where the unmount refused, which stops the settle
 * exactly as it does in the dataset form: the record and the holds
 * stay and the next verb finishes it.
 */
static int
to_the_void(struct zr_zfs *z, const char *clone, const char *mnt)
{
	char e[512];

	if (zr_zfs_unmount(z, clone, e, sizeof (e)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s will not unmount from "
		    "%s: %s\n", clone, mnt != NULL ? mnt : "its mount", e);
		return (-1);
	}
	(void) fprintf(stderr, "zfs_rebase: %s is the rebased tree, "
	    "unmounted; place it with zfs inherit mountpoint %s or zfs set "
	    "mountpoint=PATH %s\n", clone, clone, clone);
	return (0);
}

/*
 * The take. The unmount is the exclusivity; readonly goes off while
 * the dataset is off its mountpoint, where the change costs no
 * remount; canmount goes to noauto, so that a reboot in the middle
 * of a rebase leaves the half rebased tree unmounted until a verb
 * settles it rather than mounting it at boot; and then the private
 * mount. Every property is written while the dataset is mounted
 * nowhere, and the header keeps what each of them was.
 */
static int
exclusive(struct run *r)
{
	if (zr_zfs_unmount(r->zfs, r->ontods, r->err, sizeof (r->err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s is in use; unmount it "
		    "or give a snapshot\n", r->ontods);
		return (-1);
	}
	if (private_rw(r->zfs, r->ontods, r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_set_canmount(r->zfs, r->ontods, ZR_CANMOUNT_NOAUTO,
	    r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_mount_at(r->zfs, r->ontods, r->workmnt, r->err,
	    sizeof (r->err)) != 0) {
		/* it came from somewhere: put it back before giving up */
		(void) handback(r->zfs, r->ontods, r->roorig, r->cmorig,
		    r->workmnt, r->o.verbose);
		return (-1);
	}
	r->privmnt = 1;
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is this run's alone, "
		    "mounted at %s\n", r->ontods, r->workmnt);
	return (0);
}

static void
run_handback(struct run *r)
{
	char at[ZR_NAME_MAX], e[512];

	if (!r->privmnt)
		return;
	r->privmnt = 0;
	/*
	 * The done gate goes through the verbs' own machinery, which
	 * settles the result itself when it reaches done, so this can
	 * arrive at a dataset that is already home. Handing it back
	 * twice would unmount it from its own place for nothing.
	 */
	if (zr_zfs_mounted_at(r->zfs, r->ontods, at, sizeof (at), e,
	    sizeof (e)) > 0 && strcmp(at, r->workmnt) != 0)
		return;
	(void) handback(r->zfs, r->ontods, r->roorig, r->cmorig, r->workmnt,
	    r->o.verbose);
}

/*
 * While the dataset form holds onto, onto is not where its
 * mountpoint property says, so every snapshot of it is read through
 * the private mount's .zfs instead. That is the pre-apply snapshot
 * always, and the base as well when the base is a snapshot of onto,
 * which is what a from cloned out of onto derives.
 */
static void
retarget(struct run *r)
{
	char ds[ZR_SNAP_MAX];

	if (!in_dataset_form(r))
		return;
	dataset_of(r->base, ds, sizeof (ds));
	if (strcmp(ds, r->ontods) == 0)
		(void) snprintf(r->basemnt, sizeof (r->basemnt), "%s",
		    r->workmnt);
	(void) snprintf(r->ontohome, sizeof (r->ontohome), "%s", r->ontomnt);
	(void) snprintf(r->ontomnt, sizeof (r->ontomnt), "%s", r->workmnt);
}

/*
 * The phase is a gate the run has passed, not a step of it: a
 * failure to write one warns and the run goes on. The values are
 * "decided", "applying1", "conflicts" and "applying2", and no
 * others; at birth there is none, which says the run has not
 * decided yet, and at done the whole record goes.
 */
static void
put_phase(struct zr_zfs *z, const char *result, const char *phase)
{
	char e[512];

	if (zr_zfs_set_user(z, result, ZR_PROP_PHASE, phase, e,
	    sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s=%s: %s\n",
		    ZR_PROP_PHASE, phase, e);
}

static void
set_phase(struct run *r, const char *phase)
{
	if (!r->recorded)
		return;
	put_phase(r->zfs, r->rds, phase);
}

/*
 * The inputs in the order they are held: base, from, onto. The base
 * is "" for a run that has none, which is nothing to hold and
 * nothing to release, and the two sides keep their places either
 * way.
 */
static const char *
held_snap(const struct run *r, int i)
{
	if (i == 0)
		return (r->base);
	return (i == 1 ? r->fromsnap : r->ontosnap);
}

/*
 * Give this rebase's tag back, newest hold first. A release that
 * fails only warns: the others must still go, and a tag that is not
 * there is not a failure to begin with (zfsops.c).
 */
static void
release_holds(struct run *r)
{
	char e[512];

	while (r->nheld > 0) {
		const char *snap = held_snap(r, --r->nheld);

		if (snap[0] == '\0')
			continue;
		if (zr_zfs_release(r->zfs, snap, r->tag, e, sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: release %s: %s\n",
			    snap, e);
		else if (r->o.verbose)
			(void) fprintf(stderr, "zfs_rebase: released %s on "
			    "%s\n", r->tag, snap);
	}
}

/*
 * One persistent hold per input, taken after the clone exists so
 * that the tag is in the record before anything is filed under it:
 * a hold no record names is a hold nobody can find. A failure part
 * way gives back what was taken.
 */
static int
hold_inputs(struct run *r)
{
	int i;

	for (i = 0; i < 3; i++) {
		const char *snap = held_snap(r, i);

		if (snap[0] != '\0' && zr_zfs_hold(r->zfs, snap, r->tag,
		    r->err, sizeof (r->err)) != 0) {
			release_holds(r);
			return (-1);
		}
		r->nheld = i + 1;
	}
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: the inputs are held "
		    "under %s\n", r->tag);
	return (0);
}

/*
 * The record the clone is created with, which is the manifest's
 * path, the hold tag and, where the start was given --quiet, the
 * word that says so. Nothing else is ever written to a dataset by
 * this tool: what the rebase is belongs in the manifest's header,
 * which the same run writes a moment later. The strings point into
 * the run, which outlives the create.
 */
static void
fill_record(struct run *r, struct zr_rebase_record *rec)
{
	memset(rec, 0, sizeof (*rec));
	rec->manifest = r->manpath;
	rec->tag = r->tag;
	rec->quiet = r->o.quiet ? "yes" : NULL;
}

/*
 * The guid of each input, read from the snapshot itself
 * (zfs_prop_get_int, ZFS_PROP_GUID): it is what finds a snapshot
 * again after a rename or a promote, and the header prints it in
 * decimal beside the name. Read once, immediately before the
 * manifest is written, by every run including a dry one.
 *
 * A run with no base has no snapshot to read a guid off and keeps
 * the 0 it was zeroed with, which the header writes beside the "-"
 * of ZR_NO_BASE: the pair every verb reads as "there was no base",
 * which is not the same thing as a base that has gone missing.
 */
static int
read_guids(struct run *r)
{
	if (r->base[0] != '\0' &&
	    zr_zfs_get_int(r->zfs, r->base, "guid", &r->baseguid, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
	if (zr_zfs_get_int(r->zfs, r->fromsnap, "guid", &r->fromguid,
	    r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get_int(r->zfs, r->ontosnap, "guid", &r->ontoguid,
	    r->err, sizeof (r->err)) != 0)
		return (-1);
	return (0);
}

/*
 * The manifest's header, which is this rebase's identity
 * (v4-manifest.md, section 6) and, since the record was reduced to
 * four properties, the only place it is kept. The three names and
 * their guids, the form, the mode, which side the tool snapshotted
 * itself and which way the skeleton was answered are the run's own;
 * the result is the name the user asked for; and the dataset form
 * adds the pre-apply snapshot and the two properties the run has to
 * put back, both of them read before the take changed either of them
 * (onto_open) and never after it.
 *
 * A dry run creates nothing, holds nothing and takes no pre-apply
 * snapshot, so its result, its tag and its presnap are the "-" that
 * says the run had none of them.
 */
static void
fill_header(struct run *r, struct zr_manifest_hdr *h, char *stamp,
    size_t stamplen)
{
	memset(h, 0, sizeof (*h));
	h->result = r->o.dryrun ? ZR_NO_BASE : r->o.result;
	h->form = in_dataset_form(r) ? ZR_HFORM_DATASET : ZR_HFORM_CLONE;
	h->base = r->base;
	h->base_guid = r->baseguid;
	h->from = r->fromsnap;
	h->from_guid = r->fromguid;
	h->onto = r->ontosnap;
	h->onto_guid = r->ontoguid;
	if (in_dataset_form(r)) {
		h->presnap = r->presnap ? r->ontosnap : ZR_NO_BASE;
		h->readonly = r->roorig;
		h->canmount = r->cmorig;
	}
	/*
	 * made names the sides the tool snapshotted itself, which is
	 * from and never onto: the dataset form's pre-apply snapshot
	 * is the user's, named by them and kept after done.
	 */
	h->made = r->madefrom ? "from" : ZR_NO_BASE;
	h->tag = r->o.dryrun ? ZR_NO_BASE : r->tag;
	h->take = run_take(r);
	zr_manifest_stamp(stamp, stamplen);
	h->written = stamp;
	h->mode = r->o.mode;
}

/*
 * The birth manifest is written before the record, so the file is
 * already there when the path goes into the record: resolve it here
 * and let the create and every write after it carry what it really
 * is, so that every verb opens the manifest this run wrote whatever
 * directory it was started from -- and so that the resolution's
 * path, which is derived from this one, is absolute too. A path
 * that will not resolve is kept as it was given and said; it is
 * still a path this run can write to, and the verbs will resolve it
 * or fail to open it in their turn.
 */
static void
resolve_manifest(struct run *r)
{
	char *real;

	if (r->manpath[0] == '\0')
		return;
	real = realpath(r->manpath, NULL);
	if (real == NULL) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", r->manpath,
		    strerror(errno));
		return;
	}
	(void) snprintf(r->manpath, sizeof (r->manpath), "%s", real);
	free(real);
}

/*
 * The documents a run writes, as zr_doc_write wants them: one
 * function that puts the bytes on the stream it is given, and
 * whatever it needs behind the void pointer. None of them opens a
 * file -- the primitive does that, and does it atomically
 * (manifest.h) -- and none of them is the only writer of its bytes:
 * the emitter and the parser's writer share them.
 */
static int
emit_birth(FILE *out, void *arg)
{
	return (zr_manifest_birth(out, arg));
}

/* The decision: the header, the three trees and what they decided. */
struct emit_decision {
	const struct zr_manifest_hdr	*hdr;
	struct run			*r;
};

static int
emit_manifest(FILE *out, void *arg)
{
	struct emit_decision *e = arg;

	return (zr_manifest_emit(out, e->hdr, &e->r->wb.zw_tree,
	    &e->r->wf.zw_tree, &e->r->wo.zw_tree, &e->r->d));
}

static int
emit_resolution(FILE *out, void *arg)
{
	return (zr_resolution_write(out, arg));
}

/*
 * The birth manifest (documents-design.md, section 11.1): the whole
 * header, no body, written before the record so that from the
 * record's first instant the file it names exists and carries the
 * form, the three snapshots and their guids, the tag, #made, #take
 * and the dataset form's three. An --abort at any gate from here on
 * reads it and needs nothing else.
 */
static int
write_birth(struct run *r, struct zr_manifest_hdr *h)
{
	if (zr_doc_write(r->manpath, emit_birth, h, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
	r->born = 1;
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: %s carries this rebase's "
		    "header; the decision goes over it\n", r->manpath);
	return (0);
}

/*
 * The resolution, written beside the manifest and at the same moment,
 * out of the same bytes: the manifest just written is read back and
 * every conflict mark of it becomes one line with the choice "-",
 * which is the document section 8 calls the skeleton. Answering it is
 * what takes the rebase past the conflicts gate; a run with no
 * conflicts writes an empty one, so that the two documents of a
 * rebase are always both there and --abort and --restart have one
 * rule each rather than two.
 *
 * --take-onto and --take-from write the same lines with the choice
 * already made, which is a document complete from its first byte:
 * the run reads it back as answered and goes on through the gate
 * without stopping, unless --no-merge holds it there.
 *
 * The parse is not a formality. What is written here describes what
 * the file on disk says, exactly as apply_manifest applies what the
 * file says and not a second copy of it.
 */
static int
write_skeleton(struct run *r)
{
	struct zr_parsed parsed;
	struct zr_resolution res;
	FILE *in;
	int rc = -1;

	memset(&parsed, 0, sizeof (parsed));
	memset(&res, 0, sizeof (res));
	if (r->manpath[0] == '\0' || r->respath[0] == '\0') {
		(void) snprintf(r->err, sizeof (r->err),
		    "the run wrote no manifest to answer");
		return (-1);
	}
	in = fopen(r->manpath, "r");
	if (in == NULL) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", r->manpath,
		    strerror(errno));
		return (-1);
	}
	if (zr_manifest_parse(in, &parsed, r->err, sizeof (r->err)) != 0)
		goto done;
	if (zr_resolution_skeleton(&parsed, take_choice(run_take(r)),
	    &res) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "out of memory");
		goto done;
	}
	if (zr_doc_write(r->respath, emit_resolution, &res, r->err,
	    sizeof (r->err)) != 0)
		goto done;
	r->unanswered = zr_resolution_unanswered(&res);
	rc = 0;
done:
	zr_resolution_fini(&res);
	zr_parsed_fini(&parsed);
	(void) fclose(in);
	return (rc);
}

/* Say where the manifest went, when it went anywhere but stdout. */
static void
manifest_note(const struct run *r)
{
	if (r->manpath[0] != '\0')
		(void) fprintf(stderr, "zfs_rebase: the manifest is %s\n",
		    r->manpath);
}

/* <mountpoint>/.zfs/snapshot/<snap>, the read-only view of a snapshot. */
static void
snapdir(char *buf, size_t len, const char *mountpoint, const char *snap)
{
	const char *at = strchr(snap, '@');

	(void) snprintf(buf, len, "%s/.zfs/snapshot/%s", mountpoint,
	    at != NULL ? at + 1 : snap);
}

/*
 * A tree that was never there, as the empty tree: no names, no
 * pools, sealed, and no root descriptor to open a name against. It
 * is what a verb puts in the place of a side it cannot read
 * (empty_walk, below); no run reads a tree against it, since every
 * run has a base. Returns 0, or -1 out of memory.
 */
static int
empty_tree(struct zr_walk *w, struct zr_names *names)
{
	memset(w, 0, sizeof (*w));
	w->zw_rootfd = -1;
	if (zr_tree_init(&w->zw_tree, names) != 0 ||
	    zr_tree_seal(&w->zw_tree) != 0)
		return (-1);
	return (0);
}

static int
read_trees(struct run *r)
{
	char path[ZR_NAME_MAX * 2];
	uint32_t marked, m;

	r->names = zr_names_create();
	if (r->names == NULL)
		return (-1);
	/*
	 * The base, through its own .zfs/snapshot. Every run has one:
	 * it is derived from the two sides, or given with
	 * --allow-unrelated, which needs it.
	 */
	snapdir(path, sizeof (path), r->basemnt, r->base);
	if (zr_walk(path, r->names, &r->wb, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked |= ZR_W_BASE;
	if (stopped(r) != 0)
		return (-1);
	snapdir(path, sizeof (path), r->frommnt, r->fromsnap);
	if (zr_walk(path, r->names, &r->wf, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked |= ZR_W_FROM;
	if (stopped(r) != 0)
		return (-1);
	snapdir(path, sizeof (path), r->ontomnt, r->ontosnap);
	if (zr_walk(path, r->names, &r->wo, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked |= ZR_W_ONTO;
	if (stopped(r) != 0)
		return (-1);

	if (zr_oracle_init(&r->oracle, &r->wb, &r->wf, &r->wo) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "out of memory");
		return (-1);
	}
	/*
	 * The unchanged set, off the three walks and nothing else: a
	 * pool of either side that base holds under the same object
	 * number, generation, ctime, link count, type and names, with
	 * the same extended attributes and ACLs, is what base holds,
	 * and is never read (yellow.c; the ctime is trusted for the
	 * bytes, and yellow.h says why the attributes are compared
	 * besides). Only a derived base licenses that, so
	 * --allow-unrelated leaves r->prune clear and every pool is
	 * compared.
	 */
	marked = 0;
	if (r->prune) {
		if (zr_oracle_prune(r->oracle, 1, &marked) != 0 ||
		    zr_oracle_prune(r->oracle, 2, &m) != 0) {
			(void) snprintf(r->err, sizeof (r->err),
			    "the unchanged set");
			return (-1);
		}
		marked += m;
	}
	if (stopped(r) != 0)
		return (-1);
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: %u pools unchanged\n",
		    marked);
	if (zr_oracle_assign(r->oracle, r->err, sizeof (r->err)) != 0)
		return (-1);
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: %llu bytes compared\n",
		    (unsigned long long)zr_oracle_bytes_read(r->oracle));
	return (0);
}

/*
 * The three flags the guard is about, in the word the walk keeps.
 * Only a BSD spells them; where the platform has none the mask is
 * empty and the question below answers no for every object, which
 * is the right answer there.
 */
#if defined(SF_IMMUTABLE) && defined(SF_APPEND) && defined(SF_NOUNLINK)
#define	ZR_SYSFLAGS	((uint32_t)(SF_IMMUTABLE | SF_APPEND | SF_NOUNLINK))
#else
#define	ZR_SYSFLAGS	((uint32_t)0)
#endif

/* Does this side's pool carry one of the three? */
static int
sysflagged(const struct zr_walk *w, zr_pool_t i)
{
	return (i < w->zw_nattrs &&
	    (w->zw_attrs[i].za_flags & ZR_SYSFLAGS) != 0);
}

/* The result pool one name comes to, or none. */
static zr_pool_t
result_of(const struct zr_decision *d, zr_name_t n)
{
	if (n >= d->zd_nnames)
		return (ZR_POOL_NONE);
	return (d->zd_result_of[n]);
}

/*
 * Would the decision remove, rewrite or re-pool this object of
 * onto's? Any of the three has the apply take the flags off it
 * first, which above securelevel 0 it cannot do.
 */
static int
onto_changed(const struct zr_decision *d, const struct zr_pool *q)
{
	uint32_t j;

	for (j = 0; j < q->zp_nnames; j++) {
		zr_pool_t k = result_of(d, q->zp_names[j]);

		if (k == ZR_POOL_NONE)
			return (1);		/* removed */
		if (d->zd_pools[k].zr_content != q->zp_content ||
		    d->zd_pools[k].zr_nnames != q->zp_nnames)
			return (1);		/* rewritten or re-pooled */
	}
	return (0);
}

/*
 * Would the decision write this object of from's into the result?
 * The result takes from's object at one of its names, and onto does
 * not already hold that same object there -- so a cp, a write or an
 * attribute change carries it over, and za_attrs stamps the flag on
 * at the end of it. Where onto holds it already, pool for pool and
 * content for content, the manifest has nothing to say about the
 * name and nothing is written.
 */
static int
from_written(const struct zr_decision *d, const struct zr_tree *ot,
    const struct zr_pool *q)
{
	uint32_t j;

	for (j = 0; j < q->zp_nnames; j++) {
		zr_name_t n = q->zp_names[j];
		zr_pool_t k = result_of(d, n);
		zr_pool_t op;

		if (k == ZR_POOL_NONE ||
		    d->zd_pools[k].zr_content != q->zp_content)
			continue;	/* not from's object at this name */
		op = zr_tree_pool(ot, n);
		if (op != ZR_POOL_NONE &&
		    ot->zt_pools[op].zp_content == q->zp_content &&
		    ot->zt_pools[op].zp_nnames == d->zd_pools[k].zr_nnames)
			continue;	/* onto has it: nothing is written */
		return (1);
	}
	return (0);
}

/* One line naming the object and the side it is on; see run.h. */
static void
flags_say(char *err, size_t errlen, const struct zr_names *names,
    zr_name_t n, int level, const char *side, const char *what)
{
	const char *p;
	size_t l;

	if (err == NULL || errlen == 0)
		return;
	p = zr_names_str(names, n, &l);
	(void) snprintf(err, errlen, "securelevel %d: %s carries schg, "
	    "sappnd or sunlnk on %s's side and would %s", level,
	    p == NULL ? "(a name the table does not hold)" : p, side, what);
}

int
zr_flags_refused(const struct zr_decision *d, const struct zr_walk *onto,
    const struct zr_walk *from, const struct zr_names *names, int level,
    char *err, size_t errlen)
{
	const struct zr_tree *t;
	uint32_t i;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (level <= 0 || ZR_SYSFLAGS == 0 || d == NULL || onto == NULL ||
	    from == NULL || names == NULL)
		return (0);
	t = &onto->zw_tree;
	for (i = 0; i < t->zt_npools; i++) {
		if (sysflagged(onto, i) == 0 ||
		    onto_changed(d, &t->zt_pools[i]) == 0)
			continue;
		flags_say(err, errlen, names, t->zt_pools[i].zp_names[0],
		    level, "onto", "change");
		return (1);
	}
	t = &from->zw_tree;
	for (i = 0; i < t->zt_npools; i++) {
		if (sysflagged(from, i) == 0 ||
		    from_written(d, &onto->zw_tree, &t->zt_pools[i]) == 0)
			continue;
		flags_say(err, errlen, names, t->zt_pools[i].zp_names[0],
		    level, "from", "be written into the result");
		return (1);
	}
	return (0);
}

/*
 * The guard itself: what securelevel this box is at, and then the
 * question above. The sysctl is the only part of it FreeBSD alone
 * can answer, which is why it is the only part behind the #if; a
 * box that cannot be booted above securelevel 0 without a reboot
 * can still be asked what the rule says (tests/MATRIX.md, ZX23 and
 * ZX242).
 */
static int
securelevel_guard(struct run *r)
{
#if defined(__FreeBSD__)
	int level = 0;
	size_t len = sizeof (level);

	if (sysctlbyname("kern.securelevel", &level, &len, NULL, 0) != 0)
		return (0);
	if (zr_flags_refused(&r->d, &r->wo, &r->wf, r->names, level, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
#else
	(void) r;
#endif
	return (0);
}

/*
 * Parse the manifest this run wrote, apply that -- so that what is
 * applied is what the text on disk says and not a second copy of it
 * -- and make the applying1 self-check over the same document.
 * The file is <rundir>/manifest, or the -o path, whichever the run
 * recorded; --abort and --verify read that same file later, and a
 * --continue applies it again through read_manifest below. The round
 * trip a temporary copy used to make is made here against the real
 * file, which is the stronger of the two.
 */
static int
apply_manifest(struct run *r)
{
	struct zr_parsed parsed;
	struct zr_apply_stats st, rst;
	FILE *fp;
	int rc = -1;

	if (r->manpath[0] == '\0') {
		(void) snprintf(r->err, sizeof (r->err),
		    "the run wrote no manifest to apply");
		return (-1);
	}
	fp = fopen(r->manpath, "r");
	if (fp == NULL) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", r->manpath,
		    strerror(errno));
		return (-1);
	}
	memset(&parsed, 0, sizeof (parsed));
	if (zr_manifest_parse(fp, &parsed, r->err, sizeof (r->err)) != 0)
		goto done;
	/*
	 * The gate: written immediately before the result stops being
	 * read-only, so that a kill from here on leaves a record that
	 * says the tree was being written to.
	 */
	set_phase(r, ZR_PHASE_APPLYING1);
	if (!in_dataset_form(r) && zr_zfs_set_readonly(r->zfs, r->rds, 0,
	    r->err, sizeof (r->err)) != 0)
		goto done;
	zr_pause(ZR_PHASE_APPLYING1);
	/*
	 * No classification: what a fresh run applies to is onto's
	 * tree exactly -- a clone of the snapshot, or the dataset the
	 * snapshot was just taken of -- so every action of the
	 * manifest is still to be made and a classification could let
	 * none of them be left alone. A --continue over a tree
	 * somebody has already applied to is where one earns its
	 * keep.
	 */
	rc = zr_apply_with(&parsed, r->workmnt, &r->wf, &r->wo, NULL, &st,
	    r->err, sizeof (r->err));
	if (rc == 0 && r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: applied %llu rm %llu ln "
		    "%llu cp %llu dup %llu write, %llu bytes\n",
		    (unsigned long long)st.zs_rm, (unsigned long long)st.zs_ln,
		    (unsigned long long)st.zs_cp, (unsigned long long)st.zs_dup,
		    (unsigned long long)st.zs_write,
		    (unsigned long long)st.zs_bytes);
	/*
	 * And the self-check, which is the same one a --continue
	 * makes at this gate: the result walked again beside from and
	 * onto, the document held against it, the names no action
	 * spoke for put back as onto had them -- the result is this
	 * run's own until the conflicts gate, so anything else there
	 * is a stray -- and then every action must be done or
	 * blocked. It is made before readonly goes back on, because
	 * the putting back writes.
	 */
	if (rc == 0) {
		struct zr_apply_kept kept;

		/*
		 * The walk of the result the check ends with is the
		 * result as the done gate is about to ask about it --
		 * read-only from here and written by nothing in
		 * between -- so it is kept for the gate rather than
		 * thrown away and made again (R13 of the code review).
		 * The oracle over it is not: the gate is the check a
		 * --continue makes, and what it is handed are the
		 * three trees and not a memo of comparisons somebody
		 * else has already made.
		 */
		memset(&kept, 0, sizeof (kept));
		kept.zk_walk = &r->wr;
		rc = zr_apply_check(&parsed, r->workmnt, r->names, &r->wo,
		    &r->wf, 0, 1, &rst, &kept, r->err, sizeof (r->err));
		if (kept.zk_live != 0) {
			zr_oracle_fini(kept.zk_oracle);
			r->walked |= ZR_W_RESULT;
		}
		if (rc == 0 && r->o.verbose)
			(void) fprintf(stderr, "zfs_rebase: put back %llu "
			    "restored, %llu removed, %llu relinked\n",
			    (unsigned long long)rst.zs_restored,
			    (unsigned long long)rst.zs_removed,
			    (unsigned long long)rst.zs_relinked);
	}
	if (!in_dataset_form(r) && zr_zfs_set_readonly(r->zfs, r->rds, 1,
	    r->err, sizeof (r->err)) != 0)
		rc = -1;
done:
	zr_parsed_fini(&parsed);
	(void) fclose(fp);
	return (rc);
}

/*
 * Let the three trees and the name table go, which the run does as
 * soon as the self-check has passed. Nothing after that reads them, and
 * what they hold open is inside the result -- in the dataset form
 * inside a mount the run is about to hand back, and on a from
 * snapshot that a rebase reaching done destroys. Anything that has
 * to read them again opens them again, which is what the verbs do.
 */
static void
release_trees(struct run *r)
{
	if (r->oracle != NULL) {
		zr_oracle_fini(r->oracle);
		r->oracle = NULL;
	}
	if ((r->walked & ZR_W_RESULT) != 0)
		zr_walk_fini(&r->wr);
	if ((r->walked & ZR_W_ONTO) != 0)
		zr_walk_fini(&r->wo);
	if ((r->walked & ZR_W_FROM) != 0)
		zr_walk_fini(&r->wf);
	if ((r->walked & ZR_W_BASE) != 0)
		zr_walk_fini(&r->wb);
	r->walked = 0;
	if (r->names != NULL) {
		zr_names_destroy(r->names);
		r->names = NULL;
	}
}

/*
 * The base and the decision's oracle alone, let go of as soon as the
 * manifest is written: nothing after that reads either, and the two
 * sides and the result stay for the done gate, which is handed them
 * rather than walking the same three trees again (R13 of the code
 * review). A run that stops before the gate lets them go in
 * release_trees like everything else.
 */
static void
release_base(struct run *r)
{
	if (r->oracle != NULL) {
		zr_oracle_fini(r->oracle);
		r->oracle = NULL;
	}
	if ((r->walked & ZR_W_BASE) != 0) {
		zr_walk_fini(&r->wb);
		r->walked &= ~ZR_W_BASE;
	}
}

static void
teardown(struct run *r, int keep)
{
	char e[512];

	zr_decision_fini(&r->d);
	if (r->oracle != NULL)
		zr_oracle_fini(r->oracle);
	if ((r->walked & ZR_W_RESULT) != 0)
		zr_walk_fini(&r->wr);
	if ((r->walked & ZR_W_ONTO) != 0)
		zr_walk_fini(&r->wo);
	if ((r->walked & ZR_W_FROM) != 0)
		zr_walk_fini(&r->wf);
	if ((r->walked & ZR_W_BASE) != 0)
		zr_walk_fini(&r->wb);
	if (r->names != NULL)
		zr_names_destroy(r->names);
	/*
	 * The result, and only where the run is taking itself away
	 * whole: the walks are closed by now, so the snapshots under
	 * the private mount's .zfs are idle and the unmount can have
	 * it, and the dataset goes back the way an --abort would put
	 * it, its clone being destroyed below in any case. A kept run
	 * at a gate leaves the result where it is, at the private
	 * mount, which is what the next verb expects to find; and a
	 * run that reached done was settled by the done gate itself,
	 * which is the nested verb's, before this was reached.
	 */
	if (!keep)
		run_handback(r);
	if (!keep) {
		/*
		 * The holds go before the result, because the record
		 * that names their tag goes with it: a hold outliving
		 * the only thing that names it is a hold nobody can
		 * find. The tool-made snapshots go after the holds
		 * for the same reason the other way round -- a held
		 * snapshot will not be destroyed. Nothing is riskier
		 * for the order either way: a clone's origin cannot
		 * be destroyed while the clone lives, holds or no
		 * holds.
		 */
		release_holds(r);
		if (r->cloned) {
			if (zr_zfs_destroy(r->zfs, r->o.result, r->err,
			    sizeof (r->err)) != 0)
				(void) fprintf(stderr,
				    "zfs_rebase: destroy %s: %s\n",
				    r->o.result, r->err);
		} else if (r->recorded) {
			/*
			 * The dataset form wrote the record on a
			 * dataset of the user's, so it takes it off
			 * again. Nothing was applied: this path is
			 * only reached before the first write.
			 */
			clear_record(r->zfs, r->rds, r->o.verbose);
		}
		/*
		 * Only the run's own two documents go, and they have
		 * to, because they are inside the directory about to
		 * be removed. The birth manifest is written before the
		 * record, so what says there is a document to unlink
		 * is that write and not the record. Where -o named the
		 * manifest the pair is the user's: the run wrote them
		 * where they asked and does not take them back.
		 */
		if (r->born && r->o.outpath == NULL) {
			(void) unlink(r->manpath);
			(void) unlink(r->respath);
		}
		/*
		 * The pre-apply snapshot is the user's and is kept at
		 * done -- but this is a run that was discarded before
		 * it wrote anything, so it goes with the rest. So
		 * does the one a dry run took of a dataset onto for
		 * something to read.
		 */
		if ((r->presnap || r->madeonto) &&
		    zr_zfs_destroy_snap(r->zfs, r->ontosnap, e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    r->ontosnap, e);
		if (r->madefrom &&
		    zr_zfs_destroy_snap(r->zfs, r->fromsnap, e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    r->fromsnap, e);
		if (r->dirmade)
			(void) rmdir_run(r->rds);
	}
	/*
	 * A run that reached done left nothing here to do: the done
	 * gate released the holds, took the record off, destroyed the
	 * from snapshot it had taken itself and removed the run
	 * directory, in that order and after it had put the result
	 * back (the settle's comment above). A run kept at a gate
	 * keeps all of it: the rebase lives in it.
	 */
	/*
	 * A kept run keeps its holds: they are the rebase, and its
	 * record names the tag that gives them back.
	 */
	if (r->zfs != NULL)
		zr_zfs_close(r->zfs);
}

/*
 * The done gate, which is the verbs' own and is written with them
 * below: the run reaches it at the end of its last stage, and a
 * --continue reaches the same function at the same gate.
 */
static int final_verify(struct run *r, int *settled);

int
zr_run(const struct zr_run_opts *o)
{
	struct sigaction saved[ZR_NSIG];
	struct run r;
	struct zr_manifest_hdr hdr;
	struct zr_rebase_record rec;
	FILE *out = stdout;
	char cont[ZR_NAME_MAX];
	char stamp[ZR_STAMP_MAX];
	int rc = EXIT_INTERNAL, keep = 0, gocont = 0, settled = 0;

	memset(&r, 0, sizeof (r));
	memset(&rec, 0, sizeof (rec));
	cont[0] = '\0';
	r.o = *o;
	zr_pause_open();
	if (geteuid() != 0) {
		(void) fprintf(stderr, "zfs_rebase: must run as root\n");
		return (EXIT_PRECOND);
	}
	tag_make(r.tag, sizeof (r.tag), "zr-");
	signals_install(saved);
	if (zr_zfs_open(&r.zfs, r.err, sizeof (r.err)) != 0) {
		rc = fail(&r, EXIT_PRECOND, "libzfs");
		goto done;
	}
	/*
	 * 1. the forms. --onto decides which one this is, and in the
	 * dataset form the dataset itself is looked at before its
	 * snapshot is taken: it must be there, mounted, and carrying
	 * no rebase of ours at all.
	 */
	if (choose_form(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "usage");
		goto done;
	}
	if (in_dataset_form(&r) && onto_open(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "precondition");
		goto done;
	}
	/*
	 * 2. the snapshots the tool takes for itself. A side given as
	 * a dataset is snapshotted here, before anything reads it,
	 * and the header's #made line says the tool made it: it lives
	 * as long as the rebase and no longer. The dataset form's pre-apply
	 * snapshot is the user's, named by --result, so a name that
	 * is taken is refused rather than worked around.
	 */
	if (is_snapshot(o->from)) {
		(void) snprintf(r.fromsnap, sizeof (r.fromsnap), "%s",
		    o->from);
	} else {
		if (snapshot_input(&r, o->from, r.fromsnap,
		    sizeof (r.fromsnap)) != 0) {
			rc = fail(&r, EXIT_PRECOND, "snapshot");
			goto done;
		}
		r.madefrom = 1;
	}
	if (in_dataset_form(&r)) {
		if (o->dryrun) {
			if (snapshot_input(&r, r.ontods, r.ontosnap,
			    sizeof (r.ontosnap)) != 0) {
				rc = fail(&r, EXIT_PRECOND, "snapshot");
				goto done;
			}
			r.madeonto = 1;
		} else {
			int got = zr_zfs_snapshot(r.zfs, r.ontosnap, r.err,
			    sizeof (r.err));

			if (got > 0)
				(void) snprintf(r.err, sizeof (r.err),
				    "%s exists already, and --result names "
				    "the snapshot this rebase takes",
				    r.ontosnap);
			if (got != 0) {
				rc = fail(&r, EXIT_PRECOND, "precondition");
				goto done;
			}
			r.presnap = 1;
			if (o->verbose)
				(void) fprintf(stderr, "zfs_rebase: %s is "
				    "what %s was before the rebase\n",
				    r.ontosnap, r.ontods);
		}
	}
	if (preconditions(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "precondition");
		goto done;
	}
	/*
	 * The guids, read off the three snapshots themselves and by a
	 * dry run as well as a real one. They are the header's, and
	 * the header is written twice: once at birth, before the
	 * record, and once at the decision. Everything they are read
	 * from is in place by now -- the pre-apply snapshot is taken
	 * and the base is derived -- and nothing between here and the
	 * decision touches any of the three.
	 */
	if (read_guids(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "precondition");
		goto done;
	}

	/*
	 * 3. the birth manifest, the working tree -- a read-only
	 * clone in one form, the dataset itself taken over in the
	 * other -- the record, and then the three holds, which the
	 * record's tag names. A dry run creates nothing and holds
	 * nothing: it only reads, and it leaves no rebase behind to
	 * be continued or aborted.
	 */
	if (!o->dryrun) {
		if (make_rundir(&r) != 0) {
			rc = fail(&r, EXIT_PRECOND, "run directory");
			goto done;
		}
		if (o->outpath != NULL)
			(void) snprintf(r.manpath, sizeof (r.manpath), "%s",
			    o->outpath);
		else
			(void) snprintf(r.manpath, sizeof (r.manpath),
			    "%s/manifest", r.rundir);
		/*
		 * The header, whole, before anything else of this
		 * rebase exists to need it: the identity is known
		 * before the first walk, so an --abort at any gate
		 * from here on has the form, the pre-apply snapshot
		 * and the two properties to give back. The path is
		 * resolved once the file is there, and the record
		 * carries what it really is from the create on; the
		 * resolution's path follows from it by the one rule
		 * every verb reads it back with.
		 */
		fill_header(&r, &hdr, stamp, sizeof (stamp));
		if (write_birth(&r, &hdr) != 0) {
			rc = fail(&r, EXIT_INTERNAL, "manifest");
			goto done;
		}
		resolve_manifest(&r);
		resolution_of(r.respath, sizeof (r.respath), r.rds, r.manpath);
		fill_record(&r, &rec);
		if (in_dataset_form(&r)) {
			if (zr_zfs_write_record(r.zfs, r.rds, &rec, r.err,
			    sizeof (r.err)) != 0) {
				r.recorded = 1;	/* part of it may be there */
				rc = fail(&r, EXIT_PRECOND, "record");
				goto done;
			}
			r.recorded = 1;
		} else {
			/*
			 * The clone is born with the record on it,
			 * read-only and with no mountpoint of its
			 * own; the private mount is put on it here,
			 * exactly as the dataset form's take does,
			 * and is the only place it is ever mounted
			 * while the rebase is open.
			 */
			if (zr_zfs_clone(r.zfs, r.ontosnap, o->result, &rec,
			    r.err, sizeof (r.err)) != 0) {
				rc = fail(&r, EXIT_PRECOND, "clone");
				goto done;
			}
			r.cloned = 1;
			r.recorded = 1;
			if (zr_zfs_mount_at(r.zfs, o->result, r.workmnt,
			    r.err, sizeof (r.err)) != 0) {
				rc = fail(&r, EXIT_PRECOND, "clone");
				goto done;
			}
		}
		if (hold_inputs(&r) != 0) {
			rc = fail(&r, EXIT_PRECOND, "hold");
			goto done;
		}
		zr_pause("held");
		/*
		 * And the exclusivity, which is the unmount: from
		 * here the dataset is the run's alone and is handed
		 * back wherever the run stops.
		 */
		if (in_dataset_form(&r) && exclusive(&r) != 0) {
			rc = fail(&r, EXIT_PRECOND, "exclusivity");
			goto done;
		}
	} else if (o->outpath != NULL) {
		(void) snprintf(r.manpath, sizeof (r.manpath), "%s",
		    o->outpath);
	}
	/*
	 * A dry run reads onto where it stands, since it took no
	 * dataset over; a real one reads it through the private
	 * mount.
	 */
	if (!o->dryrun) {
		retarget(&r);
		zr_pause("cloned");
	}

	/* 4. read, 5. decide */
	if (read_trees(&r) != 0) {
		rc = fail(&r, zr_apply_stop != 0 ? EXIT_INTERNAL :
		    EXIT_PRECOND, "read");
		goto done;
	}
	zr_pause("read");
	if (zr_decide(&r.wb.zw_tree, &r.wf.zw_tree, &r.wo.zw_tree, o->mode,
	    &r.d) != 0) {
		(void) snprintf(r.err, sizeof (r.err), "out of memory");
		rc = fail(&r, EXIT_INTERNAL, "decide");
		goto done;
	}
	if (stopped(&r) != 0) {
		rc = fail(&r, EXIT_INTERNAL, "decide");
		goto done;
	}

	if (!o->dryrun && securelevel_guard(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "precondition");
		goto done;
	}

	/*
	 * 6. the decision, written whole over the header this run was
	 * born with -- the same file, the same path in the record,
	 * the same identity, and now the actions and the conflicts as
	 * well. The write is atomic, so what a reader finds there is
	 * one document or the other and never half of either, and the
	 * phase "decided" goes down the moment it is in place: that
	 * is what tells a verb the file is the decision.
	 *
	 * A dry run has no record and no file of its own to write
	 * over. It goes to the -o path or to standard output, which
	 * is its whole output.
	 */
	fill_header(&r, &hdr, stamp, sizeof (stamp));
	if (r.manpath[0] != '\0') {
		struct emit_decision ed;

		ed.hdr = &hdr;
		ed.r = &r;
		if (zr_doc_write(r.manpath, emit_manifest, &ed, r.err,
		    sizeof (r.err)) != 0) {
			rc = fail(&r, EXIT_INTERNAL, "manifest");
			goto done;
		}
	} else if (zr_manifest_emit(out, &hdr, &r.wb.zw_tree, &r.wf.zw_tree,
	    &r.wo.zw_tree, &r.d) != 0) {
		(void) snprintf(r.err, sizeof (r.err), "write failed");
		rc = fail(&r, EXIT_INTERNAL, "manifest");
		goto done;
	}
	set_phase(&r, ZR_PHASE_DECIDED);
	/*
	 * A dry run stops here: it created nothing to apply to, and
	 * its whole output is the manifest it just wrote.
	 */
	if (o->dryrun) {
		if (r.d.zd_nconflicts != 0) {
			(void) fprintf(stderr, "zfs_rebase: %u conflict%s; "
			    "nothing applied\n", r.d.zd_nconflicts,
			    r.d.zd_nconflicts == 1 ? "" : "s");
			rc = EXIT_CONFLICTS;
		} else {
			rc = EXIT_CLEAN;
		}
		goto done;
	}
	/*
	 * And the resolution beside it, in this same process: a
	 * conflicts gate the tool did not write the skeleton for
	 * would be a gate nobody could pass. The gate between the
	 * two writes is the harness's: it is the only moment at
	 * which a rebase has its decision and no resolution, and what
	 * a kill there leaves is a rebase at "decided" whose exits are
	 * --restart, which writes the skeleton again from the recorded
	 * manifest, and --abort.
	 */
	zr_pause("manifest");
	if (write_skeleton(&r) != 0) {
		rc = fail(&r, EXIT_INTERNAL, "resolution");
		goto done;
	}

	/* 7. applying1: the clean actions and the self-check after them */
	zr_pause(ZR_PHASE_DECIDED);
	if (stopped(&r) != 0) {
		rc = fail(&r, EXIT_INTERNAL, "apply");
		goto done;	/* nothing applied yet: the run goes */
	}
	/*
	 * A failure or a signal from here on leaves the phase at the
	 * gate the run reached -- applying1 -- and the result, its
	 * record and its holds in place. There is no failed phase and
	 * no interrupted phase: what a stop leaves is a gate, and a
	 * later --continue picks the rebase up from it.
	 *
	 * The conflicts, if the decision had any, wait until after
	 * this: what they cover is not in the manifest's actions at
	 * all, and the rest of the rebase is made whether they are
	 * answered or not.
	 */
	if (apply_manifest(&r) != 0) {
		keep = 1;
		rc = fail(&r, EXIT_INTERNAL, "apply");
		manifest_note(&r);
		kept_hint(&r);
		goto done;
	}
	keep = 1;
	/*
	 * The base and the decision's oracle go here, as they always
	 * did; the two sides, the result the self-check walked and
	 * the name table stay, because the done gate below is handed
	 * them (R13 of the code review). A run that stops before that
	 * gate lets them go with the rest.
	 */
	release_base(&r);
	/*
	 * A signal that came in while the apply or the self-check ran
	 * leaves the gate it came in under, and writes no new one.
	 */
	if (stopped(&r) != 0) {
		rc = fail(&r, EXIT_INTERNAL, "apply");
		manifest_note(&r);
		kept_hint(&r);
		goto done;
	}
	if (r.d.zd_nconflicts != 0) {
		/*
		 * Nothing of this run reads the trees again: what
		 * comes next is either the wait at the conflicts gate
		 * or a --continue, which opens the rebase for itself.
		 */
		release_trees(&r);
		/*
		 * The hand-off. The clean part of the rebase is in the
		 * result and the conflicts are the manifest's; the
		 * skeleton written above is where they are answered,
		 * and the gate is passed by a complete document and a
		 * command that says to go on.
		 *
		 * A skeleton written under --take-onto or --take-from
		 * is complete from the start, and so is one whose
		 * conflicts a run of the same rebase answered before
		 * a restart put the tree back. Where it is complete
		 * and --no-merge was not given, this run is the
		 * command that says to go on: it hands the result
		 * back, tears itself down and then takes the rebase
		 * through applying2 to done by the one code path a
		 * person's --continue uses. Where it is not, or where
		 * --no-merge was given, the rebase waits here.
		 */
		set_phase(&r, ZR_PHASE_CONFLICTS);
		zr_pause(ZR_PHASE_CONFLICTS);
		(void) fprintf(stderr, "zfs_rebase: %u conflict%s; the clean "
		    "actions are applied and %s waits at conflicts\n",
		    r.d.zd_nconflicts, r.d.zd_nconflicts == 1 ? "" : "s",
		    r.rds);
		if (r.unanswered != 0) {
			(void) fprintf(stderr, "zfs_rebase: %u name%s "
			    "unanswered in the resolution %s\n", r.unanswered,
			    r.unanswered == 1 ? "" : "s", r.respath);
		} else if (o->nomerge) {
			(void) fprintf(stderr, "zfs_rebase: the resolution %s "
			    "is answered in full, and --no-merge leaves the "
			    "merge to you\n", r.respath);
		} else {
			(void) fprintf(stderr, "zfs_rebase: the resolution %s "
			    "is answered in full; going on\n", r.respath);
			(void) snprintf(cont, sizeof (cont), "%s", r.rds);
			gocont = 1;
		}
		manifest_note(&r);
		if (!gocont)
			kept_hint(&r);
		rc = EXIT_CONFLICTS;
		goto done;
	}
	/*
	 * Done: the final check, then the holds given back, and then
	 * the record taken off. In that order, because the tag in the
	 * record is the only handle on those holds: a kill in between
	 * leaves a record whose holds are already released, which
	 * --abort and --continue both take in their stride, where the
	 * other order would leave holds nothing names. There is no
	 * "done" among the phases -- what says a rebase finished is
	 * that the result carries no record at all.
	 *
	 * The run reaches that gate through the same function a
	 * --continue reaches it through: the record is on the result
	 * already, so the check is made over the rebase and not over
	 * anything this process happens to be holding, and a run
	 * killed before it and continued later makes exactly the same
	 * check. It costs a second walk of from, onto and the result,
	 * which is what a check that is standard costs; there is no
	 * flag that would skip it (documents-design.md, section 7).
	 */
	rc = final_verify(&r, &settled);
	release_trees(&r);
	if (!settled) {
		/*
		 * The gate was not passed: the check could not be
		 * made at all, which is this program failing and not
		 * the tree drifting, or the result could not be given
		 * back, which is somebody standing in the private
		 * mount. Either way what is left is a rebase for a
		 * --continue or an --abort to take on, with its record
		 * and its holds where they were.
		 */
		manifest_note(&r);
		kept_hint(&r);
		goto done;
	}
	/*
	 * The verb reached the done gate and settled the result
	 * itself -- the dataset home, or the clone unmounted with its
	 * placement line printed -- so the teardown below has nothing
	 * left to do but its own closing. settled stays clear for
	 * exactly that reason: settling twice would unmount a dataset
	 * from its own place. rc is the check's own: 0, or 3 where it
	 * found drift, which done does not block on.
	 */
	r.nheld = 0;			/* the check gave them back */
	r.privmnt = 0;			/* and the result with them */
	r.recorded = 0;			/* and took the record off */
	/*
	 * And the manifest, where there is still one to name. The two
	 * documents a run wrote into its own directory go with that
	 * directory at done, so only a -o pair outlives the rebase and
	 * pointing at a path about to be unlinked would be a lie. The
	 * nested verb has already been through done and taken the
	 * directory.
	 */
	if (!in_rundir(r.rds, r.manpath))
		manifest_note(&r);
done:
	teardown(&r, keep);
	signals_restore(saved);
	/*
	 * The gate was passed by this run itself, and the rest of the
	 * rebase is a --continue: made after the teardown, so that
	 * this process has closed its walks and its libzfs handle
	 * before the verb opens its own, and made through zr_continue
	 * and not through some second path of the fresh run's own, so
	 * that what a person's --continue does and what this does are
	 * one thing. The result stays at the private mount across the
	 * two, in both forms, and the verb's take_over finds it
	 * there: there is no hand-back at this gate to undo.
	 * --no-merge cannot be set here: it is what would have
	 * stopped the run at the gate.
	 */
	if (gocont) {
		struct zr_verb_opts vo;

		memset(&vo, 0, sizeof (vo));
		vo.ident = cont;
		vo.verbose = o->verbose;
		rc = zr_continue(&vo);
	}
	return (rc);
}

/*
 * ---------------------------------------------------------------
 * The verbs on a rebase that already exists: --continue, --restart
 * and --verify. Everything they need is the record and the manifest
 * it names; nothing is decided again, because the manifest is the
 * decision. The gates are the same gates, written in the same
 * places, and the apply is the same idempotent apply, so a rebase
 * that a kill left half made is finished by doing the whole of it
 * again and leaving alone what is already true.
 * ---------------------------------------------------------------
 */

/* The record's inputs, in the order it names them and the holds go. */
#define	ZI_BASE		0
#define	ZI_FROM		1
#define	ZI_ONTO		2

/* The walks a verb keeps, in the order the verify oracle wants them. */
#define	ZS_ONTO		0
#define	ZS_FROM		1
#define	ZS_RESULT	2

/*
 * One rebase as a verb reads it: the record's four properties, and
 * the header of the manifest the record names, which is where every
 * other fact about the rebase lives (documents-design.md, sections 2
 * and 3). struct zr_rebase_record is what a create is handed --
 * pointers into the run that made it -- so a reader has to own the
 * strings they point at.
 *
 * phase is "" before the first gate and is never "done": a rebase
 * that reached done has no record to read. mode is not copied here;
 * it is zp_mode of the parse, which the verb keeps whole.
 */
struct record {
	struct zr_rebase_record	rec;
	char			manifest[ZR_NAME_MAX];	/* the property */
	char			tag[ZR_TAG_MAX];	/* and this one */
	char			phase[32];
	int			quiet;
	/* and from here down, the header's own */
	char			base[ZR_SNAP_MAX];
	char			from[ZR_SNAP_MAX];
	char			onto[ZR_SNAP_MAX];
	uint64_t		base_guid;
	uint64_t		from_guid;
	uint64_t		onto_guid;
	char			made[ZR_SNAP_MAX];	/* "from" or "-" */
	enum zr_hform		form;
	char			take[8];	/* "onto", "from" or "-" */
	char			presnap[ZR_SNAP_MAX];	/* dataset form */
	char			readonly[8];		/* and this */
	char			canmount[16];		/* and this */
};

/* One verb in flight. */
struct resume {
	struct zr_zfs		*zfs;
	int			zfslent;	/* the caller's, not to close */
	char			result[ZR_NAME_MAX];
	int			nomerge;	/* --no-merge on the command */
	int			report;		/* the verb is --verify */
	int			verbose;
	int			dataset;	/* the dataset form */
	int			privmnt;	/* it is at workmnt just now */
	int			settled;	/* it reached the done gate */
	int			post;		/* --verify of a settled one */
	int			mademnt;	/* and this check mounted it */
	struct record		rb;
	char			rundir[ZR_NAME_MAX];
	/*
	 * Where the result's tree is to be read. For a verb that moves
	 * a rebase that is <rundir>/mnt, the private mount the run
	 * made and every such verb takes over; for a report it is
	 * wherever the result is mounted, which is that same path only
	 * where the rebase is open, or where the check had to mount it
	 * there itself (documents-design.md, sections 7 and 11.6).
	 */
	char			workmnt[ZR_NAME_MAX];
	char			respath[ZR_NAME_MAX];	/* the resolution */
	char			given[ZR_NAME_MAX];	/* MANIFEST, resolved */
	char			tmptag[ZR_TAG_MAX];	/* the report's hold */
	char			found[3][ZR_SNAP_MAX];	/* by ZI_ */
	int			gone[3];
	unsigned		miss;		/* ZR_MISS_ of the walks */
	struct zr_names		*names;
	struct zr_walk		w[3];		/* by ZS_ */
	int			walked;		/* a bit per walk */
	struct zr_oracle	*oracle;
	struct zr_parsed	man;		/* the recorded manifest */
	int			parsed;
	struct zr_resolution	res;		/* the recorded resolution */
	int			hasres;		/* 1 read, 0 gone, -1 bad */
	char			reserr[512];	/* why, when it is -1 */
	int			writable;	/* readonly is off just now */
	char			err[512];
};

/* A message with a category, or without one when it names itself. */
static int
vfail(const struct resume *s, int code, const char *what)
{
	if (what != NULL)
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", what, s->err);
	else
		(void) fprintf(stderr, "zfs_rebase: %s\n", s->err);
	return (code);
}

/*
 * The refusal a rebase born and never decided gets from the verbs
 * that move one. Its record has no phase, so the file that record
 * names is the header the run was born with: there is no decision in
 * it and applying it would be applying nothing at all
 * (documents-design.md, section 11.1). --abort is the way out, and
 * has everything the header gave it.
 */
static int
undecided(struct resume *s)
{
	(void) snprintf(s->err, sizeof (s->err), "%s never reached its "
	    "decision: %s is the header it was born with and carries no "
	    "actions to apply; zfs_rebase --abort %s takes the rebase away",
	    s->result, s->rb.manifest, s->result);
	return (vfail(s, EXIT_PRECOND, NULL));
}

/* The recorded name of one input, "" when the record has none. */
static const char *
rec_snap(const struct record *rb, int i)
{
	if (i == ZI_BASE)
		return (rb->base);
	return (i == ZI_FROM ? rb->from : rb->onto);
}

static uint64_t
rec_guid(const struct record *rb, int i)
{
	if (i == ZI_BASE)
		return (rb->base_guid);
	return (i == ZI_FROM ? rb->from_guid : rb->onto_guid);
}

/* The form the header names, as a word a message can use. */
static const char *
form_word(enum zr_hform f)
{
	if (f == ZR_HFORM_DATASET)
		return ("dataset");
	return (f == ZR_HFORM_POSIX ? "posix" : "clone");
}

/* base, from, onto, as the record and the messages spell them. */
static const char *
input_word(int i)
{
	if (i == ZI_BASE)
		return ("base");
	return (i == ZI_FROM ? "from" : "onto");
}

/*
 * Does zfs_rebase:made say the tool took this input's snapshot
 * itself? Such a snapshot lives exactly as long as the rebase, so
 * after done it is gone on purpose, and a report says that rather
 * than calling it a loss. The property holds the words the create
 * put there, and a word is one only when nothing lettered adjoins
 * it.
 */
static int
made_says(const struct record *rb, const char *which)
{
	const char *p = rb->made;
	size_t n = strlen(which);

	while ((p = strstr(p, which)) != NULL) {
		if ((p == rb->made || p[-1] < 'a' || p[-1] > 'z') &&
		    (p[n] < 'a' || p[n] > 'z'))
			return (1);
		p += n;
	}
	return (0);
}

/* One user property of the result, as a local value. */
static int
rec_str(struct resume *s, const char *prop, char *buf, size_t buflen)
{
	return (zr_zfs_get_user(s->zfs, s->result, prop, buf, buflen, s->err,
	    sizeof (s->err)));
}

/* One string of the header, copied into the record's own buffer. */
static void
hdr_str(char *buf, size_t buflen, const char *val)
{
	(void) snprintf(buf, buflen, "%s", val != NULL ? val : "");
}

/*
 * The dataset that carries the record of the run a manifest
 * describes. The header names its run, and the two forms name it
 * differently (documents-design.md, section 6):
 *
 *	the clone form -- #result is the clone the run made, and the
 *	clone is the dataset that carries the record;
 *
 *	the dataset form -- #result is the pre-apply snapshot as
 *	--result spelled it, which may be the short name after the
 *	'@' and is no dataset at all. The rebase was made in the
 *	dataset #onto names, that dataset carries the record, and
 *	#onto is that snapshot's full name, so its dataset part is
 *	the answer. (#presnap is the same snapshot and says the same
 *	thing.)
 *
 * A --posix document describes no rebase and a header with no result
 * -- a dry run's, which writes "-" there -- describes one that was
 * never made. Returns 0 with buf filled, or -1 with one line in err.
 */
int
zr_run_dataset(const struct zr_parsed *p, char *buf, size_t buflen,
    char *err, size_t errlen)
{
	const char *name;

	buf[0] = '\0';
	if (p->zp_form == ZR_HFORM_POSIX) {
		(void) snprintf(err, errlen, "this is a --posix document and "
		    "names no rebase");
		return (-1);
	}
	if (p->zp_result == NULL || p->zp_result[0] == '\0' ||
	    strcmp(p->zp_result, ZR_NO_BASE) == 0) {
		(void) snprintf(err, errlen, "this names no result: a dry run "
		    "wrote it and no rebase was made");
		return (-1);
	}
	name = p->zp_form == ZR_HFORM_DATASET ? p->zp_onto : p->zp_result;
	if (name == NULL || name[0] == '\0') {
		(void) snprintf(err, errlen, "this names no onto snapshot, "
		    "and the dataset form's rebase is in that snapshot's "
		    "dataset");
		return (-1);
	}
	if (strlen(name) >= buflen) {
		(void) snprintf(err, errlen, "%s: %s", name,
		    strerror(ENAMETOOLONG));
		return (-1);
	}
	dataset_of(name, buf, buflen);
	if (buf[0] == '\0') {
		(void) snprintf(err, errlen, "%s names no dataset", name);
		return (-1);
	}
	return (0);
}

/*
 * Steps 1 and 4 of the identifier's resolution: the manifest at
 * path. Exported because it opens a file and no pool, so a machine
 * with no ZFS can hold the whole of it against a document.
 */
int
zr_ident_manifest(const char *path, struct zr_ident *out, char *err,
    size_t errlen)
{
	char reason[512];
	char *real;
	FILE *fp;

	memset(out, 0, sizeof (*out));
	/*
	 * realpath first, because that is what the start recorded
	 * (resolve_manifest) and what the record is compared with
	 * afterwards: a manifest named through a symlink, or from
	 * another directory, is the same manifest.
	 */
	real = realpath(path, NULL);
	if (real == NULL) {
		(void) snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return (-1);
	}
	if ((size_t)snprintf(out->zi_path, sizeof (out->zi_path), "%s",
	    real) >= sizeof (out->zi_path)) {
		(void) snprintf(err, errlen, "%s: %s", real,
		    strerror(ENAMETOOLONG));
		free(real);
		return (-1);
	}
	free(real);
	fp = fopen(out->zi_path, "r");
	if (fp == NULL) {
		(void) snprintf(err, errlen, "%s: %s", out->zi_path,
		    strerror(errno));
		return (-1);
	}
	if (zr_manifest_parse(fp, &out->zi_man, reason,
	    sizeof (reason)) != 0) {
		(void) fclose(fp);
		zr_parsed_fini(&out->zi_man);
		(void) snprintf(err, errlen, "%s: %s", out->zi_path, reason);
		memset(out, 0, sizeof (*out));
		return (-1);
	}
	(void) fclose(fp);
	out->zi_parsed = 1;
	/*
	 * And the header's own half of the identity: which dataset
	 * carries the record of the run this document describes.
	 */
	if (zr_run_dataset(&out->zi_man, out->zi_result,
	    sizeof (out->zi_result), reason, sizeof (reason)) != 0) {
		(void) snprintf(err, errlen, "%s: %s", out->zi_path, reason);
		return (-1);
	}
	return (0);
}

void
zr_ident_fini(struct zr_ident *id)
{
	if (id->zi_parsed != 0) {
		zr_parsed_fini(&id->zi_man);
		memset(&id->zi_man, 0, sizeof (id->zi_man));
		id->zi_parsed = 0;
	}
}

/*
 * More than one rebase answers to one identifier, which is refused
 * and never chosen between: every match is printed and the caller
 * gives up. Returns -1, the reason already on stderr.
 */
static int
ident_many(const char *ident, const char *what, const struct zr_zfs_found *f)
{
	unsigned i;

	(void) fprintf(stderr, "zfs_rebase: %u %s answer to %s, and one "
	    "identifier names one rebase:\n", f->zf_n, what, ident);
	for (i = 0; i < f->zf_kept; i++)
		(void) fprintf(stderr, "zfs_rebase:     %s\n", f->zf_name[i]);
	if (f->zf_n > f->zf_kept)
		(void) fprintf(stderr, "zfs_rebase:     and %u more\n",
		    f->zf_n - f->zf_kept);
	(void) fprintf(stderr, "zfs_rebase: name one of them in full\n");
	return (-1);
}

/*
 * The name a step matched, held against ZFS's own rule for a
 * dataset name before it is used to build a path
 * (documents-design.md, section 11.3). Steps 2 and 3 bring it back
 * from a pool and it can only be a name; steps 1 and 4 read it out
 * of a document, where anything at all could be written.
 */
static int
ident_result_ok(const struct zr_ident *id)
{
	char err[512];

	if (zr_zfs_name_valid(id->zi_result, 0, err, sizeof (err)) == 1)
		return (0);
	(void) fprintf(stderr, "zfs_rebase: %s, so it names no rebase\n",
	    err);
	return (-1);
}

/*
 * Steps 1 and 4, with the refusal printed and the parse given back
 * where the document could not be made to name a rebase: the caller
 * has nothing to free after a no.
 */
static int
ident_path_step(const char *path, struct zr_ident *id)
{
	char err[512];

	if (zr_ident_manifest(path, id, err, sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s\n", err);
	else if (ident_result_ok(id) == 0)
		return (0);
	zr_ident_fini(id);
	return (-1);
}

/*
 * Does this dataset carry the record: both properties, both local,
 * which is what has_record asks of a result and what the walk of the
 * pools asks of every dataset it meets. 1 or 0; a dataset that is
 * not there carries nothing.
 */
static int
ident_has_record(struct zr_zfs *z, const char *dataset)
{
	char buf[ZR_NAME_MAX], err[512];

	if (zr_zfs_exists(z, dataset, err, sizeof (err)) <= 0)
		return (0);
	return (zr_zfs_get_user(z, dataset, ZR_PROP_TAG, buf, sizeof (buf),
	    err, sizeof (err)) > 0 && zr_zfs_get_user(z, dataset,
	    ZR_PROP_MANIFEST, buf, sizeof (buf), err, sizeof (err)) > 0);
}

/*
 * The rebase an identifier names, in the five steps of
 * documents-design.md section 11.4, the first that matches winning.
 * Every refusal is printed here; the caller gives up with
 * EXIT_PRECOND. Returns 0 with id filled, or -1.
 *
 * The pool is open before this is called, because steps 2 and 3 ask
 * it what carries a record. Nothing is chosen between: where a step
 * has two answers the verb stops, and where one has an answer the
 * steps after it are never asked.
 */
static int
resolve_ident(struct zr_zfs *z, const char *ident, struct zr_ident *id)
{
	struct zr_zfs_found f;
	char ds[ZR_NAME_MAX], dir[ZR_NAME_MAX], err[512], nameerr[512];
	const char *at, *snap;
	struct stat sb;
	int valid;

	memset(id, 0, sizeof (*id));
	if (ident == NULL || ident[0] == '\0') {
		(void) fprintf(stderr, "zfs_rebase: no rebase was named\n");
		return (-1);
	}
	/*
	 * 1. An absolute path is a path and nothing else: a file that
	 * is not there is a refusal here, because the alternative is
	 * to go looking in the pools for a dataset named /tmp/x.
	 */
	if (ident[0] == '/')
		return (ident_path_step(ident, id));
	/*
	 * Held against ZFS's own rule as what it is spelled as: a name
	 * with an '@' in it is a snapshot's and is valid or not by the
	 * snapshot rule, so that "tank/x@nosuch", which names no
	 * rebase, is refused as that and not as no name at all.
	 */
	at = strchr(ident, '@');
	valid = zr_zfs_name_valid(ident, at != NULL, nameerr,
	    sizeof (nameerr)) == 1;
	if (at != NULL) {
		dataset_of(ident, ds, sizeof (ds));
		snap = at + 1;
	} else {
		(void) snprintf(ds, sizeof (ds), "%s", ident);
		snap = ident;
	}
	/*
	 * 2. A dataset carrying the record whose name is the
	 * identifier or ends in it: the clone form's result, spelled
	 * in full or short. A name with an '@' in it is no dataset
	 * name and this step is not asked.
	 */
	if (at == NULL) {
		/*
		 * The whole name first, asked of that dataset alone.
		 * A full dataset name is unique in ZFS and there is
		 * nothing for the walk to add to it: it is the rebase
		 * or it is not one, and a dataset somewhere else whose
		 * name happens to end in this one cannot make the name
		 * its owner wrote ambiguous.
		 */
		if (ident_has_record(z, ident) != 0) {
			(void) snprintf(id->zi_result, sizeof (id->zi_result),
			    "%s", ident);
			return (ident_result_ok(id));
		}
		if (zr_zfs_find_record(z, ident, NULL, &f, err,
		    sizeof (err)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: %s\n", err);
			return (-1);
		}
		if (f.zf_n > 1)
			return (ident_many(ident, "open rebases", &f));
		if (f.zf_n == 1) {
			(void) snprintf(id->zi_result, sizeof (id->zi_result),
			    "%s", f.zf_name[0]);
			return (ident_result_ok(id));
		}
	}
	/*
	 * 3. A snapshot of a dataset carrying the record, named for
	 * the identifier: the dataset form's pre-apply snapshot. The
	 * short spelling names the snapshot alone and any result can
	 * be wearing it; the full one names the dataset too, and the
	 * dataset is matched as in step 2, so "tank/main@pre" and
	 * "main@pre" find the same snapshot.
	 */
	if (at != NULL && ident_has_record(z, ds) != 0 &&
	    zr_zfs_exists(z, ident, err, sizeof (err)) > 0) {
		(void) snprintf(id->zi_result, sizeof (id->zi_result), "%s",
		    ds);
		return (ident_result_ok(id));
	}
	/*
	 * "ds@" names no snapshot at all, so there is no step to make
	 * of it and the path steps are still to come.
	 */
	if (at == NULL || at[1] != '\0') {
		if (zr_zfs_find_record(z, at != NULL ? ds : NULL, snap, &f,
		    err, sizeof (err)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: %s\n", err);
			return (-1);
		}
		if (f.zf_n > 1)
			return (ident_many(ident, "pre-apply snapshots", &f));
		if (f.zf_n == 1) {
			dataset_of(f.zf_name[0], id->zi_result,
			    sizeof (id->zi_result));
			return (ident_result_ok(id));
		}
	}
	/*
	 * 4. A relative path to a manifest of the user's. A regular
	 * file has to be standing there: anything else is not this
	 * step, and the last step is still to come.
	 */
	if (stat(ident, &sb) == 0 && S_ISREG(sb.st_mode))
		return (ident_path_step(ident, id));
	/*
	 * 5. A run directory of that name and nothing else: what a
	 * crash before the record leaves, which --abort takes away
	 * and no other verb has anything to do with. rundir_of is the
	 * one thing that builds a path under WORKDIR, and it holds the
	 * name against ZFS's own rule first (R19), so an identifier
	 * that is no dataset name cannot name a directory here.
	 */
	if (rundir_of(dir, sizeof (dir), ident, NULL, 0) == 0 &&
	    stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode)) {
		(void) snprintf(id->zi_result, sizeof (id->zi_result), "%s",
		    ident);
		id->zi_rundir = 1;
		return (0);
	}
	/*
	 * Nothing at all, and three ways of saying so. A word that is
	 * no dataset name and no path is the first: it could not have
	 * named a rebase, and the reason is ZFS's own, which is also
	 * the reason no path was ever built from it (R19).
	 */
	if (valid == 0) {
		(void) fprintf(stderr, "zfs_rebase: %s, and no manifest is at "
		    "that path either: it names no rebase\n", nameerr);
		return (-1);
	}
	/*
	 * A dataset of that name that carries no record is the
	 * second: it is either none of ours or a rebase that reached
	 * done, which took its record off and left the manifest as
	 * the only thing that still names it.
	 */
	if (zr_zfs_exists(z, ident, err, sizeof (err)) > 0) {
		(void) fprintf(stderr, "zfs_rebase: %s is not a zfs_rebase "
		    "result; nothing was touched. A rebase that reached done "
		    "left no record: give the manifest its run wrote\n",
		    ident);
		return (-1);
	}
	/* And a name nothing at all answers to is the third. */
	(void) fprintf(stderr, "zfs_rebase: no rebase answers to %s: no "
	    "manifest is at that path, no result of an open rebase carries "
	    "that name, and %s/%s is no run directory\n", ident, workdir(),
	    ident);
	return (-1);
}

/*
 * The manifest the record names, parsed once and kept: it is the
 * decision every verb applies and the header is the rebase's
 * identity, so the two are read together and nothing here is read
 * twice.
 *
 * Once, and not twice: where the identifier was a path, the
 * resolution has already parsed that very file to find this rebase,
 * and that parse is adopted rather than made again (R12 of the code
 * review). A record naming another file is read here, and the
 * resolution's parse goes back first, because one verb holds one
 * document.
 *
 * And this is the one place the two halves are held against each
 * other, for every way a verb can have been given its rebase (R4):
 * the dataset the header names has to be the dataset this record
 * sits on. Without it a verb that found its rebase by name would
 * take whatever file the record points at as the truth about it, and
 * a file that had been replaced -- two runs given the same -o path
 * -- would be applied to the wrong result.
 */
static int
read_manifest(struct resume *s)
{
	struct record *rb = &s->rb;
	char hds[ZR_NAME_MAX], reason[512];
	FILE *fp;
	int rc;

	if (s->parsed != 0 && strcmp(s->given, rb->manifest) != 0) {
		zr_parsed_fini(&s->man);
		memset(&s->man, 0, sizeof (s->man));
		s->parsed = 0;
	}
	if (s->parsed == 0) {
		fp = fopen(rb->manifest, "r");
		if (fp == NULL) {
			(void) snprintf(s->err, sizeof (s->err), "%s: %s",
			    rb->manifest, strerror(errno));
			return (-1);
		}
		rc = zr_manifest_parse(fp, &s->man, s->err, sizeof (s->err));
		s->parsed = 1;
		(void) fclose(fp);
		if (rc != 0)
			return (-1);
	}
	hdr_str(rb->base, sizeof (rb->base), s->man.zp_base);
	hdr_str(rb->from, sizeof (rb->from), s->man.zp_from);
	hdr_str(rb->onto, sizeof (rb->onto), s->man.zp_onto);
	rb->base_guid = s->man.zp_base_guid;
	rb->from_guid = s->man.zp_from_guid;
	rb->onto_guid = s->man.zp_onto_guid;
	hdr_str(rb->made, sizeof (rb->made), s->man.zp_made);
	hdr_str(rb->take, sizeof (rb->take), s->man.zp_take);
	hdr_str(rb->presnap, sizeof (rb->presnap), s->man.zp_presnap);
	hdr_str(rb->readonly, sizeof (rb->readonly), s->man.zp_readonly);
	hdr_str(rb->canmount, sizeof (rb->canmount), s->man.zp_canmount);
	rb->form = s->man.zp_form;
	/*
	 * The form the run was made in, which decides what the result
	 * is: a clone of the tool's own, or a dataset of the user's
	 * that the verb has to take over and hand back.
	 */
	s->dataset = rb->form == ZR_HFORM_DATASET;
	/* And the cross-check, by the header's own rule. */
	if (zr_run_dataset(&s->man, hds, sizeof (hds), reason,
	    sizeof (reason)) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s: %s",
		    rb->manifest, reason);
		return (-1);
	}
	if (strcmp(hds, s->result) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s is the manifest "
		    "of %s and %s carries it as its own: they are two "
		    "rebases", rb->manifest, hds, s->result);
		return (-1);
	}
	return (0);
}

/*
 * Is there a rebase in flight on this dataset at all: 1 with the two
 * properties read into the record, 0 where neither is there, -1 with
 * err set. A dataset carrying neither zfs_rebase:manifest nor
 * zfs_rebase:tag as a local value is no open rebase -- not a dataset
 * of the user's own that a mistyped name found, not one that only
 * inherits those properties from a parent, since zr_zfs_get_user
 * answers for the local value alone, and not one whose rebase
 * reached done, which took the record off.
 *
 * The last of those is the one a verb can still be about: --verify
 * of a settled result reads it from its manifest instead
 * (settled_open), and every other verb moves a rebase and has
 * nothing here to move.
 */
static int
has_record(struct resume *s)
{
	struct record *rb = &s->rb;
	int got;

	got = rec_str(s, ZR_PROP_MANIFEST, rb->manifest,
	    sizeof (rb->manifest));
	if (got < 0)
		return (-1);
	if (got == 0)
		return (0);
	got = rec_str(s, ZR_PROP_TAG, rb->tag, sizeof (rb->tag));
	if (got < 0)
		return (-1);
	return (got > 0);
}

/*
 * The whole record, and the refusal that guards every verb: a
 * dataset with none of it is not a zfs_rebase result and nothing
 * here touches it.
 *
 * What the two properties buy is the manifest, and the manifest's
 * header is the rest of the record: the three snapshots and their
 * guids, the form, what the tool snapshotted itself, the way the
 * skeleton was answered and the dataset form's own three.
 */
static int
read_record(struct resume *s)
{
	struct record *rb = &s->rb;
	char q[8];
	int got;

	got = has_record(s);
	if (got < 0)
		return (-1);
	if (got == 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s is not a "
		    "zfs_rebase result; nothing was touched", s->result);
		return (-1);
	}
	if (rec_str(s, ZR_PROP_PHASE, rb->phase, sizeof (rb->phase)) < 0)
		return (-1);
	got = rec_str(s, ZR_PROP_QUIET, q, sizeof (q));
	if (got < 0)
		return (-1);
	/*
	 * The start latched --quiet here for the whole run, and this
	 * is where every later invocation reads it: it silences the
	 * report of the final check at the done gate, and nothing
	 * else -- not the check, not its verdict, not the report of
	 * the conflicts gate and not the --verify verb's.
	 */
	rb->quiet = got > 0 && strcmp(q, "yes") == 0;
	rb->rec.manifest = rb->manifest;
	rb->rec.tag = rb->tag;
	rb->rec.quiet = rb->quiet ? "yes" : NULL;
	return (read_manifest(s));
}

/*
 * The run directory, the mount point and the resolution's path. The
 * record names the manifest and nothing else: where the resolution
 * is follows from where the manifest is, by the one rule the run
 * wrote it with, so a verb needs no path of its own to be told and
 * none can be guessed from the result's name alone.
 */
static int
resume_paths(struct resume *s)
{
	if (rundir_of(s->rundir, sizeof (s->rundir), s->result, s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	(void) snprintf(s->workmnt, sizeof (s->workmnt), "%s/mnt", s->rundir);
	resolution_of(s->respath, sizeof (s->respath), s->result,
	    s->rb.manifest);
	return (0);
}

/*
 * Whether the header names no base at all. No run writes such a
 * document any more -- --allow-unrelated needs --base, ruled
 * 2026-09-06 -- but one written before that, or by hand, carries
 * ZR_NO_BASE with the guid 0 in the base's place, and a verb reading
 * it must not go looking for a snapshot that was never there.
 * Nothing is there to find, to hold or to release, and no verb walks
 * the base in any case. The empty string is taken the same way.
 */
static int
no_base(const char *snap)
{
	return (snap[0] == '\0' || strcmp(snap, ZR_NO_BASE) == 0);
}

/*
 * How the inputs are looked for, which is one question per verb.
 */
#define	ZF_NAME		0	/* by name and guid; a miss stops the verb */
#define	ZF_GUID		1	/* and then by guid, over the whole pool */
#define	ZF_SETTLED	2	/* by name and guid, and no search at all */

/*
 * Every input the record names, found again. By name first, and the
 * guid must be the one the record kept: a snapshot destroyed and
 * taken again under the same name is another snapshot, and the
 * answers this rebase wrote do not describe it.
 *
 * ZF_GUID is the report's own way out on a rebase in flight. A
 * snapshot is its guid, where a name is only what it is called, so a
 * rename or a promote is followed here rather than reported as a
 * loss; what cannot be found by either is marked gone, and the
 * actions that would have had to read it come back unchecked. Under
 * ZF_NAME -- a --continue or a --restart, which have to read those
 * trees to write anything -- a missing input stops the verb instead.
 *
 * ZF_SETTLED is the check of a rebase that is over, and it searches
 * for nothing: the header names the three snapshots and a name that
 * is not there, or that another snapshot wears now, stops the check
 * (documents-design.md, section 7). The one exception is a from side
 * the tool snapshotted itself, which #made says so of: that snapshot
 * lives exactly as long as the rebase and is gone at done on
 * purpose, so it is marked gone and explain_gone says which actions
 * that leaves unchecked.
 */
static int
find_inputs(struct resume *s, int how)
{
	char pool[ZR_SNAP_MAX];
	uint64_t have;
	int i, ex, rc;
	int byguid = how == ZF_GUID;

	(void) snprintf(pool, sizeof (pool), "%.*s",
	    (int)strcspn(s->result, "/"), s->result);
	for (i = 0; i < 3; i++) {
		const char *want = rec_snap(&s->rb, i);

		/*
		 * A rebase whose header names no base. It is not
		 * missing: there was none, and a verb asks nothing of
		 * it.
		 */
		if (i == ZI_BASE && no_base(want)) {
			s->gone[i] = 1;
			s->found[i][0] = '\0';
			continue;
		}
		if (want[0] == '\0') {
			(void) snprintf(s->err, sizeof (s->err),
			    "the record names no %s snapshot", input_word(i));
			return (-1);
		}
		ex = zr_zfs_exists(s->zfs, want, s->err, sizeof (s->err));
		if (ex < 0)
			return (-1);
		if (ex > 0) {
			if (zr_zfs_get_int(s->zfs, want, "guid", &have,
			    s->err, sizeof (s->err)) != 0)
				return (-1);
			if (have == rec_guid(&s->rb, i)) {
				(void) snprintf(s->found[i],
				    sizeof (s->found[i]), "%s", want);
				continue;
			}
			(void) snprintf(s->err, sizeof (s->err), "%s exists "
			    "with guid %llu and the record kept %llu: a "
			    "different snapshot wears that name now", want,
			    (unsigned long long)have,
			    (unsigned long long)rec_guid(&s->rb, i));
		} else {
			(void) snprintf(s->err, sizeof (s->err),
			    "%s is gone", want);
			/*
			 * The one input a settled result is allowed
			 * to have lost: the snapshot the tool took of
			 * a side given as a dataset, which done
			 * destroyed because the rebase it belonged to
			 * had ended.
			 */
			if (how == ZF_SETTLED && i == ZI_FROM &&
			    made_says(&s->rb, input_word(i))) {
				s->gone[i] = 1;
				s->found[i][0] = '\0';
				continue;
			}
		}
		/*
		 * A snapshot the tool took itself is destroyed at
		 * done, and a rebase that reached done leaves no
		 * record for a verb to be reading here: an open one
		 * holds all three of its inputs, so a missing one is
		 * a missing one. The report is the exception and
		 * looks by guid before it gives up.
		 */
		if (!byguid)
			return (-1);
		rc = zr_zfs_find_guid(s->zfs, pool, rec_guid(&s->rb, i),
		    s->found[i], sizeof (s->found[i]), s->err,
		    sizeof (s->err));
		if (rc < 0)
			return (-1);
		if (rc == 0) {
			s->gone[i] = 1;
			s->found[i][0] = '\0';
			continue;
		}
		(void) fprintf(stderr, "zfs_rebase: the %s snapshot is %s "
		    "now; the record kept %s\n", input_word(i), s->found[i],
		    want);
	}
	if (s->gone[ZI_FROM] != 0)
		s->miss |= ZR_MISS_FROM;
	if (s->gone[ZI_ONTO] != 0)
		s->miss |= ZR_MISS_ONTO;
	return (0);
}

/*
 * One side as the command named it, against the header the record
 * names. A verb reads the two sides from that header and needs
 * neither, so what --from and --onto do here is say which rebase the
 * person thinks this is: the name must be the name the header kept,
 * and the snapshot wearing it now must be the snapshot the header
 * kept, which is the guid. That is res_input's shape, for res_input's
 * reason -- a name is what a snapshot is called and the guid is what
 * it is -- and the refusal prints both numbers.
 *
 * z NULL asks for the name alone: it is what the caller passes for
 * an input find_inputs could not find under its own name, which it
 * has already reported.
 */
static int
given_input(struct zr_zfs *z, const char *given, const char *want,
    uint64_t guid, int i, char *err, size_t errlen)
{
	uint64_t have;

	if (given == NULL)
		return (0);
	if (want == NULL || want[0] == '\0' || strcmp(given, want) != 0) {
		(void) snprintf(err, errlen, "the command names %s as the %s "
		    "and the rebase's %s is %s", given, input_word(i),
		    input_word(i), want != NULL && want[0] != '\0' ? want :
		    "nothing");
		return (-1);
	}
	if (z == NULL)
		return (0);
	if (zr_zfs_get_int(z, given, "guid", &have, err, errlen) != 0)
		return (-1);
	if (have != guid) {
		(void) snprintf(err, errlen, "%s has the guid %llu and the "
		    "rebase kept %llu: a different snapshot wears that name "
		    "now", given, (unsigned long long)have,
		    (unsigned long long)guid);
		return (-1);
	}
	return (0);
}

/*
 * What the command said about the rebase it named, against the
 * record and the header that were found. A manifest given as the
 * argument must be the manifest this record names -- the header
 * named the dataset, and the record has to name the file back --
 * and the two sides, where they were given, must be the header's.
 * Nothing here changes what the verb acts on; it only refuses to act
 * on a rebase the command described wrongly.
 */
static int
check_given(struct resume *s, const struct zr_verb_opts *o)
{
	const char *given[3];
	int i;

	if (s->given[0] != '\0' && strcmp(s->given, s->rb.manifest) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s carries the "
		    "manifest %s and the manifest given is %s: they are two "
		    "rebases", s->result, s->rb.manifest, s->given);
		return (-1);
	}
	given[ZI_BASE] = NULL;
	given[ZI_FROM] = o->from;
	given[ZI_ONTO] = o->onto;
	for (i = ZI_FROM; i <= ZI_ONTO; i++) {
		if (given_input(s->gone[i] != 0 ? NULL : s->zfs, given[i],
		    rec_snap(&s->rb, i), rec_guid(&s->rb, i), i, s->err,
		    sizeof (s->err)) != 0)
			return (-1);
	}
	return (0);
}

/*
 * A hold on each input the report found, for as long as this process
 * lives and no longer: the kernel gives a temporary hold back when
 * the descriptor it was filed against closes, and the death of the
 * process closes it however the process dies. The tag is this
 * report's own; the record's tag is the rebase's, and releasing that
 * afterwards would be releasing the rebase's grip on its own inputs.
 *
 * A hold that cannot be taken only warns. The report writes nothing
 * and can say nothing false because of it: what it would have
 * prevented is somebody destroying a snapshot in the middle of the
 * read, which the read itself would then fail on.
 */
static void
hold_for_report(struct resume *s)
{
	char e[512];
	int i;

	for (i = 0; i < 3; i++) {
		if (s->gone[i] != 0)
			continue;
		if (zr_zfs_hold_tmp(s->zfs, s->found[i], s->tmptag, e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: %s is not held for "
			    "this report: %s\n", s->found[i], e);
		else if (s->verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is held under "
			    "%s until this report ends\n", s->found[i],
			    s->tmptag);
	}
}

/*
 * Give the rebase's tag back on every input its record names. A
 * snapshot that is gone is nothing to release and a tag that is not
 * there is not a failure, which is what makes this safe to run again
 * over a rebase whose holds were already given back.
 */
static void
release_record(struct resume *s)
{
	char e[512];
	int i, ex;

	for (i = 0; i < 3; i++) {
		const char *snap = rec_snap(&s->rb, i);

		if (snap[0] == '\0' || (i == ZI_BASE && no_base(snap)))
			continue;
		ex = zr_zfs_exists(s->zfs, snap, e, sizeof (e));
		if (ex <= 0)
			continue;
		if (zr_zfs_release(s->zfs, snap, s->rb.tag, e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: release %s on "
			    "%s: %s\n", s->rb.tag, snap, e);
		else if (s->verbose)
			(void) fprintf(stderr, "zfs_rebase: released %s on "
			    "%s\n", s->rb.tag, snap);
	}
}

/*
 * The result is read-only except while a stage writes to it, and
 * whatever happens to a stage, read-only goes back on: the flag is
 * what stands between a rebased tree and an edit nobody meant.
 */
static int
ro_off(struct resume *s)
{
	/* the dataset form's private mount is writable for its life */
	if (!s->dataset && zr_zfs_set_readonly(s->zfs, s->result, 0,
	    s->err, sizeof (s->err)) != 0)
		return (-1);
	s->writable = 1;
	return (0);
}

static int
ro_on(struct resume *s)
{
	char e[512];

	if (s->writable == 0)
		return (0);
	if (!s->dataset &&
	    zr_zfs_set_readonly(s->zfs, s->result, 1, e, sizeof (e)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: readonly on %s: %s\n",
		    s->result, e);
		return (-1);
	}
	s->writable = 0;
	return (0);
}

/*
 * A tree that is not there, as the empty tree: no names, no pools,
 * sealed, and no root descriptor. The oracle wants three sealed
 * trees over one name table whatever it is asked, and the missing
 * mask is what tells the classifier that this one is only a place
 * holder and must never be asked a question.
 */
static int
empty_walk(struct resume *s, int slot)
{
	if (empty_tree(&s->w[slot], s->names) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "out of memory");
		return (-1);
	}
	s->walked |= 1 << slot;
	return (0);
}

/*
 * One side, read through its dataset's .zfs/snapshot directory, as
 * the run itself read it. An unmounted dataset is a tree that cannot
 * be reached that way: the report says so and goes on with it
 * missing, and a verb that has to write stops, since the bytes it
 * would write live there.
 */
static int
walk_side(struct resume *s, int which, int slot)
{
	char mnt[ZR_NAME_MAX], ds[ZR_SNAP_MAX];
	char path[ZR_NAME_MAX * 2];
	uint64_t mounted;

	if (s->gone[which] != 0)
		return (empty_walk(s, slot));
	dataset_of(s->found[which], ds, sizeof (ds));
	/*
	 * In the dataset form the result is a dataset of the user's
	 * that this verb has taken over, so its snapshots -- the
	 * recorded onto among them -- are under the private mount and
	 * not where the mountpoint property says.
	 */
	if (s->dataset && strcmp(ds, s->result) == 0) {
		snapdir(path, sizeof (path), s->workmnt, s->found[which]);
		if (zr_walk(path, s->names, &s->w[slot], s->err,
		    sizeof (s->err)) != 0)
			return (-1);
		s->walked |= 1 << slot;
		return (0);
	}
	if (zr_zfs_get_int(s->zfs, ds, "mounted", &mounted, s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	if (mounted == ZR_NOT_MOUNTED) {
		if (!s->report) {
			(void) snprintf(s->err, sizeof (s->err),
			    "%s is not mounted", ds);
			return (-1);
		}
		(void) fprintf(stderr, "zfs_rebase: %s is not mounted, so %s "
		    "cannot be read\n", ds, s->found[which]);
		s->gone[which] = 1;
		s->miss |= which == ZI_FROM ? ZR_MISS_FROM : ZR_MISS_ONTO;
		return (empty_walk(s, slot));
	}
	if (zr_zfs_get(s->zfs, ds, "mountpoint", mnt, sizeof (mnt), s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	snapdir(path, sizeof (path), mnt, s->found[which]);
	if (zr_walk(path, s->names, &s->w[slot], s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	s->walked |= 1 << slot;
	return (0);
}

/*
 * The oracle the classifier asks, over onto, from and the result in
 * that order. It is built again after every apply: what it remembers
 * about the result's pools was true of the tree before.
 */
static int
build_oracle(struct resume *s)
{
	if (s->oracle != NULL) {
		zr_oracle_fini(s->oracle);
		s->oracle = NULL;
	}
	if (zr_oracle_init(&s->oracle, &s->w[ZS_ONTO], &s->w[ZS_FROM],
	    &s->w[ZS_RESULT]) != 0) {
		(void) snprintf(s->err, sizeof (s->err),
		    "the three trees do not make an oracle");
		return (-1);
	}
	return (0);
}

/* The walk of the result and the oracle over it, let go of. */
static void
drop_result(struct resume *s)
{
	if (s->oracle != NULL) {
		zr_oracle_fini(s->oracle);
		s->oracle = NULL;
	}
	if ((s->walked & (1 << ZS_RESULT)) != 0) {
		zr_walk_fini(&s->w[ZS_RESULT]);
		s->walked &= ~(1 << ZS_RESULT);
	}
}

/* The result as it stands now, walked again beside the two sides. */
static int
rescan_result(struct resume *s)
{
	drop_result(s);
	if (zr_walk(s->workmnt, s->names, &s->w[ZS_RESULT], s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	s->walked |= 1 << ZS_RESULT;
	return (build_oracle(s));
}

/*
 * The same trees, taken from a self-check that has just walked them
 * rather than walked again. What zr_apply_check leaves behind it is
 * the result as it stood when the check passed, and nothing writes
 * between there and here, so this is the walk rescan_result would
 * have made and the oracle build_oracle would have built over it
 * (R13 of the code review).
 */
static void
adopt_result(struct resume *s, const struct zr_apply_kept *kept)
{
	s->walked |= 1 << ZS_RESULT;
	s->oracle = kept->zk_oracle;
}

/*
 * The result taken over for the length of this verb, exactly as the
 * run took it over and in both forms alike: at the run's own place,
 * where no writer but this verb can reach it, and read-only in the
 * clone form so that nothing can be in it while a stage or a walk
 * reads it. Only a verb that moves a rebase comes here; a report
 * takes nothing over and reads where the result stands
 * (report_mount). Three states are possible and all three are
 * ordinary:
 *
 *	at the private mount already, which is what a kill and what
 *	the gate between two verbs leave, and there is nothing to do;
 *	mounted nowhere, which is what a reboot leaves in either form
 *	-- a clone whose mountpoint is none and a dataset whose
 *	canmount is noauto are both mounted by nothing at boot -- and
 *	the private mount goes straight on;
 *	mounted somewhere else, which only a hand can have done, and
 *	the unmount that takes it back is the same refusal the take
 *	makes: a result somebody is using is not this verb's to move.
 *
 * After a reboot the directories under WORKDIR are still there but
 * nothing is mounted, and the mount point itself may have been taken
 * away by hand; mkdir_p makes both good.
 */
static int
take_over(struct resume *s)
{
	char at[ZR_NAME_MAX];
	int rc;

	if (mkdir_p(s->workmnt, s->err, sizeof (s->err)) != 0)
		return (-1);
	rc = zr_zfs_mounted_at(s->zfs, s->result, at, sizeof (at), s->err,
	    sizeof (s->err));
	if (rc < 0)
		return (-1);
	if (rc > 0 && strcmp(at, s->workmnt) == 0) {
		s->privmnt = 1;
	} else {
		if (rc > 0 && zr_zfs_unmount(s->zfs, s->result, s->err,
		    sizeof (s->err)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: %s is in use; "
			    "unmount it and try again\n", s->result);
			return (-1);
		}
		/*
		 * The dataset form's private mount is writable for
		 * its whole life, and the flip is made here while the
		 * dataset is off any mountpoint; the clone's readonly
		 * is the stages' own and is put back on below.
		 */
		if ((s->dataset && private_rw(s->zfs, s->result, s->err,
		    sizeof (s->err)) != 0) ||
		    zr_zfs_mount_at(s->zfs, s->result, s->workmnt, s->err,
		    sizeof (s->err)) != 0)
			return (-1);
		s->privmnt = 1;
		if (s->verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is this verb's "
			    "alone, mounted at %s\n", s->result, s->workmnt);
	}
	/*
	 * Read-only outside a stage is the clone form's rule, and the
	 * kernel applies the change to the live private mount with no
	 * remount at all (readonly_changed_cb; the box probe of
	 * 2026-09-06, sprints/sprint-5/probe-mount.txt, 2a to 2d).
	 * The dataset form's private mount is writable for its life
	 * (private_rw), and the header says what readonly was.
	 */
	if (s->dataset)
		return (0);
	return (zr_zfs_set_readonly(s->zfs, s->result, 1, s->err,
	    sizeof (s->err)));
}

/*
 * One of a resolution's three header lines against the manifest's.
 * The name is what the snapshot was called and the guid is what it
 * is, so a name that matches with a guid that does not is another
 * snapshot wearing the name and the refusal prints both numbers.
 */
static int
res_input(struct resume *s, const char *name, uint64_t guid, int i)
{
	const char *want = rec_snap(&s->rb, i);

	if (name == NULL || strcmp(name, want) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s names %s as the "
		    "%s and the record names %s", s->respath,
		    name != NULL ? name : "nothing", input_word(i), want);
		return (-1);
	}
	if (guid != rec_guid(&s->rb, i)) {
		(void) snprintf(s->err, sizeof (s->err), "%s gives the %s %s "
		    "the guid %llu and the record gives it %llu", s->respath,
		    input_word(i), name, (unsigned long long)guid,
		    (unsigned long long)rec_guid(&s->rb, i));
		return (-1);
	}
	return (0);
}

/*
 * The resolution the record names: 1 with *out parsed, 0 when there
 * is no such file, -1 with err set. It is the document of choices of
 * v4-manifest.md section 8, and it must carry the same three header
 * lines the manifest does, name and guid alike, since a resolution
 * written for another rebase describes another tree.
 *
 * Either way *out is safe to hand to zr_resolution_fini.
 */
static int
read_resolution(struct resume *s, struct zr_resolution *out)
{
	FILE *fp;
	int rc;

	memset(out, 0, sizeof (struct zr_resolution));
	fp = fopen(s->respath, "r");
	if (fp == NULL) {
		if (errno == ENOENT)
			return (0);
		(void) snprintf(s->err, sizeof (s->err), "%s: %s", s->respath,
		    strerror(errno));
		return (-1);
	}
	rc = zr_resolution_parse(fp, out, s->err, sizeof (s->err));
	(void) fclose(fp);
	if (rc != 0)
		return (-1);
	if (res_input(s, out->zs_base, out->zs_base_guid, ZI_BASE) != 0 ||
	    res_input(s, out->zs_from, out->zs_from_guid, ZI_FROM) != 0 ||
	    res_input(s, out->zs_onto, out->zs_onto_guid, ZI_ONTO) != 0)
		return (-1);
	return (1);
}

/*
 * The resolution is the tool's own file: it wrote the skeleton when
 * it wrote the manifest, so one that is not there is a precondition
 * failure and not a stage waiting to begin.
 */
static int
no_resolution(struct resume *s)
{
	(void) snprintf(s->err, sizeof (s->err), "%s is gone; the run wrote "
	    "it beside the manifest and %s cannot go on without it",
	    s->respath, s->result);
	return (vfail(s, EXIT_PRECOND, "resolution"));
}

/*
 * What waits at the conflicts gate, said the same way wherever the
 * rebase stopped there: how many of the resolution's names are still
 * unanswered, and where the file is. A resolution that cannot be read
 * says only where it should be; the verb that has to read it says the
 * rest.
 */
static void
unanswered_note(const struct resume *s, uint32_t left, uint32_t total)
{
	(void) fprintf(stderr, "zfs_rebase: %u of %u name%s unanswered in "
	    "the resolution %s\n", left, total, total == 1 ? "" : "s",
	    s->respath);
}

static void
conflicts_note(const struct resume *s)
{
	if (s->hasres <= 0)
		(void) fprintf(stderr, "zfs_rebase: the resolution is %s\n",
		    s->respath);
	else
		unanswered_note(s, zr_resolution_unanswered(&s->res),
		    s->res.zs_nlines);
}

/*
 * Hold one manifest against the trees as they stand, with the
 * resolution where the record had one: from the conflicts gate on it
 * is the third input, and it says which names are the person's and
 * which side a resolved name is to be held against.
 */
static int
classify(struct resume *s, const struct zr_parsed *m,
    struct zr_verify_report *out)
{
	return (zr_verify_with(m, s->hasres > 0 ? &s->res : NULL, s->oracle,
	    &s->w[ZS_ONTO], &s->w[ZS_FROM], &s->w[ZS_RESULT], s->miss, out,
	    s->err, sizeof (s->err)));
}

/*
 * The resolution's own block of the same report: one line per outcome
 * with its count and the first name that had it, and under -v every
 * line of the document with its choice and its outcome. A keep is
 * counted nowhere, because it is never compared -- the result stands
 * there by the person's word -- and neither is a name still
 * unanswered, so a document of nothing but those prints five zeroes,
 * which is the truth about it.
 */
static void
print_choices(const struct resume *s, const struct zr_verify_report *rep)
{
	uint32_t first;
	int i;

	if (rep->zv_nrlines == 0)
		return;
	for (i = 0; i < ZR_OC_COUNT; i++) {
		first = rep->zv_rfirst[i];
		if (first == ZR_ACTION_NONE) {
			(void) fprintf(stderr, "zfs_rebase:   the resolution: "
			    "%s %u\n", zr_outcome_str((enum zr_outcome)i),
			    rep->zv_rcount[i]);
			continue;
		}
		(void) fprintf(stderr, "zfs_rebase:   the resolution: %s %u, "
		    "first %s\n", zr_outcome_str((enum zr_outcome)i),
		    rep->zv_rcount[i],
		    (const char *)s->res.zs_lines[first].zl_path);
	}
}

/* And, under -v, every line of it: its name, its choice, its outcome. */
static void
print_lines(const struct resume *s, const struct zr_verify_report *rep)
{
	const struct zr_rline *l;
	uint32_t j;

	for (j = 0; j < rep->zv_nrlines; j++) {
		l = &s->res.zs_lines[j];
		(void) fprintf(stderr, "zfs_rebase:     %s %s %s\n",
		    (const char *)l->zl_path, zr_choice_str(l->zl_choice),
		    zr_outcome_str(rep->zv_rline[j]));
	}
}

/*
 * The report: one line per outcome with its count and the first
 * action that had it, then one line per kind of the names the
 * manifest never spoke for, with the first of each, and under -v the
 * whole list. The action counts are over the actions the header
 * declared, which is every line but the conflict marks: a mark is
 * nothing to do and is counted nowhere. what names the document,
 * since a rebase can have two of them.
 */
static void
print_report(const struct resume *s, const struct zr_parsed *m,
    const struct zr_verify_report *rep, const char *what)
{
	const char *nm;
	uint32_t first, j;
	int i;

	(void) fprintf(stderr, "zfs_rebase: %s: %s, %u action%s\n", s->result,
	    what, m->zp_actions_declared,
	    m->zp_actions_declared == 1 ? "" : "s");
	for (i = 0; i < ZR_OC_COUNT; i++) {
		first = rep->zv_first[i];
		if (first == ZR_ACTION_NONE) {
			(void) fprintf(stderr, "zfs_rebase:   %s %u\n",
			    zr_outcome_str((enum zr_outcome)i),
			    rep->zv_count[i]);
			continue;
		}
		(void) fprintf(stderr, "zfs_rebase:   %s %u, first %s\n",
		    zr_outcome_str((enum zr_outcome)i), rep->zv_count[i],
		    (const char *)m->zp_actions[first].za_path);
	}
	for (i = 0; i < ZR_DF_COUNT; i++) {
		nm = rep->zv_dfirst[i] == ZR_NAME_NONE ? NULL :
		    zr_names_str(s->names, rep->zv_dfirst[i], NULL);
		if (nm == NULL) {
			(void) fprintf(stderr, "zfs_rebase:   outside the "
			    "manifest: %s %u\n",
			    zr_diff_str((enum zr_diff)i), rep->zv_dcount[i]);
			continue;
		}
		(void) fprintf(stderr, "zfs_rebase:   outside the manifest: "
		    "%s %u, first %s\n", zr_diff_str((enum zr_diff)i),
		    rep->zv_dcount[i], nm);
	}
	print_choices(s, rep);
	if (s->verbose == 0)
		return;
	for (j = 0; j < rep->zv_ndiffs; j++) {
		nm = zr_names_str(s->names, rep->zv_diffs[j].zn_name, NULL);
		(void) fprintf(stderr, "zfs_rebase:     %s %s\n",
		    zr_diff_str(rep->zv_diffs[j].zn_kind),
		    nm != NULL ? nm : "?");
	}
	print_lines(s, rep);
}

/*
 * One applying stage: the gate, the classification the apply reads,
 * the apply, the re-walk and read-only again. m is the document this
 * stage applies -- the recorded manifest for applying1 -- and phase
 * is the gate to write before the first write, or NULL where the
 * gate must not move.
 *
 * The classification is made because the apply reads it, to know
 * what is already true and may be left alone, and it is not printed:
 * the reports of this tool are the two the schedule makes, at the
 * conflicts gate and at the done gate, and the --verify verb's
 * (documents-design.md, section 7). After the
 * apply comes zr_apply_check, the self-check both this and a fresh
 * run make: the result walked again, the same document classified
 * against it, and every action then done or blocked, since a pending
 * or a drifted one means the apply did not do what it said, which is
 * an internal failure and not drift.
 *
 * At applying1 the check also puts back the names no action spoke
 * for. That is the one fix in the tool and it is no flag: up to the
 * conflicts gate the result is the run's own, so a name that is not
 * what the expected tree says is a stray. From that gate on the
 * person is editing the tree, verify cannot tell their work from a
 * stray, and the names are left alone.
 */
static int
stage_apply(struct resume *s, const struct zr_parsed *m, const char *phase)
{
	struct zr_verify_report rep;
	struct zr_apply_stats st, rst;
	struct zr_apply_kept kept;
	int fix, rc = -1;

	fix = phase != NULL && strcmp(phase, ZR_PHASE_APPLYING1) == 0;
	memset(&rep, 0, sizeof (rep));
	if (phase != NULL)
		put_phase(s->zfs, s->result, phase);
	if (ro_off(s) != 0)
		return (-1);
	/*
	 * The gate this stage has just written, for the harness. A
	 * repair passes no gate and stops at none.
	 */
	if (phase != NULL)
		zr_pause(phase);
	if (classify(s, m, &rep) != 0)
		goto out;
	if (zr_apply_with(m, s->workmnt, &s->w[ZS_FROM], &s->w[ZS_ONTO], &rep,
	    &st, s->err, sizeof (s->err)) != 0)
		goto out;
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: applied %llu rm %llu ln "
		    "%llu cp %llu dup %llu write, %llu left alone, %llu "
		    "bytes\n", (unsigned long long)st.zs_rm,
		    (unsigned long long)st.zs_ln,
		    (unsigned long long)st.zs_cp,
		    (unsigned long long)st.zs_dup,
		    (unsigned long long)st.zs_write,
		    (unsigned long long)st.zs_skipped,
		    (unsigned long long)st.zs_bytes);
	zr_verify_report_fini(&rep);
	/*
	 * The check walks the result itself, and the walk it ends
	 * with is the one this verb goes on with, so the storage it
	 * is to land in is handed over before the call and the walk
	 * that was there is let go of first.
	 */
	drop_result(s);
	memset(&kept, 0, sizeof (kept));
	kept.zk_walk = &s->w[ZS_RESULT];
	if (zr_apply_check(m, s->workmnt, s->names, &s->w[ZS_ONTO],
	    &s->w[ZS_FROM], s->miss, fix, &rst, &kept, s->err,
	    sizeof (s->err)) != 0)
		goto out;
	if (fix != 0 && s->verbose)
		(void) fprintf(stderr, "zfs_rebase: put back %llu restored, "
		    "%llu removed, %llu relinked\n",
		    (unsigned long long)rst.zs_restored,
		    (unsigned long long)rst.zs_removed,
		    (unsigned long long)rst.zs_relinked);
	/*
	 * And the trees this verb goes on with, which the check left
	 * behind it: the stage after this one classifies against the
	 * result as it stands now.
	 */
	if (kept.zk_live != 0)
		adopt_result(s, &kept);
	else if (rescan_result(s) != 0)
		goto out;
	rc = 0;
out:
	zr_verify_report_fini(&rep);
	if (ro_on(s) != 0)
		rc = -1;
	return (rc);
}

/*
 * What one classification is worth as a verdict: 0 clean, and 1
 * where anything drifted. Four inputs, which are the two documents
 * and the tree between them. An action still pending or drifted at a
 * check is one the tree does not carry; a line of the resolution
 * pending or drifted is a choice the tree does not carry, and it is
 * the only place a resolved name can show at all, since a conflict
 * mark is counted in no action outcome and a chosen name is in no
 * entry of the name list; and a name the manifest never spoke for
 * that the result no longer holds as onto had it is the second axis
 * of the check, and what applying1's own repair works from. Blocked
 * and unchecked are states and not faults, a keep is never compared,
 * and a line still unanswered is a conflict nobody has answered
 * rather than a difference. One rule, read by the done gate and by
 * the --verify verb alike, so that "drift" means one thing wherever
 * the tool says it (documents-design.md, section 11.5).
 */
static int
found_drift(const struct zr_verify_report *rep)
{
	int i;

	if (rep->zv_count[ZR_OC_PENDING] != 0 ||
	    rep->zv_count[ZR_OC_DRIFTED] != 0 ||
	    rep->zv_rcount[ZR_OC_PENDING] != 0 ||
	    rep->zv_rcount[ZR_OC_DRIFTED] != 0)
		return (1);
	for (i = 0; i < ZR_DF_COUNT; i++) {
		if (rep->zv_dcount[i] != 0)
			return (1);
	}
	return (0);
}

/* Does the resolution already have a line on this exact name? */
static int
covered(const struct zr_resolution *r, const char *path, size_t len)
{
	uint32_t i;

	for (i = 0; i < r->zs_nlines; i++) {
		if (r->zs_lines[i].zl_pathlen == len &&
		    memcmp(r->zs_lines[i].zl_path, path, len) == 0)
			return (1);
	}
	return (0);
}

/*
 * The same question with the answer in hand: one bit per name id for
 * the names the resolution already has a line on. Both writers below
 * ask it once per conflict mark and once per entry of the name list,
 * and the scan above made each of them cost the whole document
 * (R20 of the code review). A line a writer adds sets its bit too,
 * since the scan would have found it from then on.
 *
 * A path no tree ever interned has no id and so no bit; the only
 * question that can be about such a path is one that has no id
 * either, and that one still goes to the scan. Out of memory is no
 * failure here: the bitmap is left unbuilt and every question goes
 * to the scan, which is what the code did before.
 */
struct rcover {
	unsigned char	*rv_bits;
	uint32_t	rv_n;		/* names the bitmap covers */
};

static void
rcover_set(struct rcover *rv, const struct zr_names *ns, const char *path,
    size_t len)
{
	zr_name_t nm;

	if (rv->rv_bits == NULL)
		return;
	nm = zr_names_lookup(ns, path, len);
	if (nm != ZR_NAME_NONE && nm < rv->rv_n)
		rv->rv_bits[nm >> 3] |= (unsigned char)(1u << (nm & 7));
}

static void
rcover_init(struct rcover *rv, const struct resume *s)
{
	uint32_t i;

	rv->rv_bits = NULL;
	rv->rv_n = s->names != NULL ? zr_names_count(s->names) : 0;
	if (rv->rv_n == 0)
		return;
	rv->rv_bits = calloc(((size_t)rv->rv_n + 7) / 8, 1);
	if (rv->rv_bits == NULL) {
		rv->rv_n = 0;
		return;
	}
	for (i = 0; i < s->res.zs_nlines; i++) {
		rcover_set(rv, s->names,
		    (const char *)s->res.zs_lines[i].zl_path,
		    s->res.zs_lines[i].zl_pathlen);
	}
}

static void
rcover_fini(struct rcover *rv)
{
	free(rv->rv_bits);
	rv->rv_bits = NULL;
	rv->rv_n = 0;
}

static int
rcover_has(const struct rcover *rv, const struct resume *s, const char *path,
    size_t len)
{
	zr_name_t nm;

	if (rv->rv_bits != NULL) {
		nm = zr_names_lookup(s->names, path, len);
		if (nm != ZR_NAME_NONE && nm < rv->rv_n)
			return ((rv->rv_bits[nm >> 3] &
			    (1u << (nm & 7))) != 0);
	}
	return (covered(&s->res, path, len));
}

/*
 * Is this name a directory? The result is asked first and onto after
 * it, since a name the result no longer holds is exactly what a gone
 * entry is. It decides one thing: the trailing slash of the line the
 * document gets, which is what says a name can scope others.
 */
static int
name_isdir(const struct resume *s, zr_name_t nm)
{
	const struct zr_tree *t;
	zr_pool_t p;
	int i;

	for (i = 0; i < 2; i++) {
		t = &s->w[i == 0 ? ZS_RESULT : ZS_ONTO].zw_tree;
		p = zr_tree_pool(t, nm);
		if (p != ZR_POOL_NONE)
			return (t->zt_pools[p].zp_type == ZR_T_DIR);
	}
	return (0);
}

/*
 * The drift the conflicts gate found, written into the resolution for
 * the picker to show. Every entry of the name list -- gone, extra,
 * changed, unpooled -- becomes one drift line with the choice keep,
 * which is what the tree already holds; the person may leave it at
 * that, or say onto to have the name put back as onto had it, or from
 * to have the manifest's own action made again. A name the resolution
 * already covers is not added a second time, and a conflicted name is
 * in no entry of that list to begin with.
 *
 * Only a --continue writes here: the --verify verb reports at this
 * gate and writes nothing anywhere, and nothing is written at
 * applying2 or at done. The document goes back to its path whole and
 * atomically, through the primitive the manifest and the skeleton
 * are written with (zr_doc_write): by this gate the file holds a
 * person's answers, and a rewrite torn by a crash would destroy work
 * only --restart could replace -- with the answers it exists to
 * discard (documents-design.md, section 11.2).
 *
 * A conflict line the manifest marks that the document no longer has
 * is put back here too, with the take mode's answer -- onto under
 * --take-onto, from under --take-from, and "-" where the run was
 * given neither, which puts the name back among the unanswered. The
 * manifest is what says a name is conflicted, and a hand edit cannot
 * take a conflict away by deleting the line that speaks for it
 * (documents-design.md, section 11.5).
 *
 * Returns 0, or -1 with err set.
 */
static int
add_drift(struct resume *s, const struct zr_verify_report *rep)
{
	const struct zr_action *a;
	struct rcover rv;
	const char *nm;
	size_t len;
	uint32_t i, n = 0, back = 0;

	rcover_init(&rv, s);
	for (i = 0; i < s->man.zp_nactions; i++) {
		a = &s->man.zp_actions[i];
		if (a->za_kind != ZR_ACT_CONFLICT ||
		    rcover_has(&rv, s, (const char *)a->za_path,
		    a->za_pathlen) != 0)
			continue;
		if (zr_resolution_add_conflict(&s->res, a->za_path,
		    a->za_pathlen, a->za_isdir, a->za_conflict,
		    take_choice(s->rb.take)) != 0) {
			(void) snprintf(s->err, sizeof (s->err), "%s: cannot "
			    "put back the conflict line %s", s->respath,
			    (const char *)a->za_path);
			rcover_fini(&rv);
			return (-1);
		}
		rcover_set(&rv, s->names, (const char *)a->za_path,
		    a->za_pathlen);
		back++;
	}
	for (i = 0; i < rep->zv_ndiffs; i++) {
		len = 0;
		nm = zr_names_str(s->names, rep->zv_diffs[i].zn_name, &len);
		if (nm == NULL || len == 0 ||
		    rcover_has(&rv, s, nm, len) != 0)
			continue;
		if (zr_resolution_add_drift(&s->res,
		    (const unsigned char *)nm, len,
		    name_isdir(s, rep->zv_diffs[i].zn_name),
		    ZR_CH_KEEP) != 0) {
			(void) snprintf(s->err, sizeof (s->err), "%s: cannot "
			    "take the drift line %s", s->respath, nm);
			rcover_fini(&rv);
			return (-1);
		}
		rcover_set(&rv, s->names, nm, len);
		n++;
	}
	rcover_fini(&rv);
	if (n == 0 && back == 0)
		return (0);
	if (zr_doc_write(s->respath, emit_resolution, &s->res, s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	if (n != 0)
		(void) fprintf(stderr, "zfs_rebase: %u drift line%s added to "
		    "the resolution %s\n", n, n == 1 ? "" : "s", s->respath);
	if (back != 0)
		(void) fprintf(stderr, "zfs_rebase: %u conflict line%s the "
		    "manifest marks put back into the resolution %s\n", back,
		    back == 1 ? "" : "s", s->respath);
	return (0);
}

/*
 * What the final check found, written into the resolution as the
 * record of it: a line the check calls pending or drifted set back
 * to "-", a name outside the manifest that drifted taken as a drift
 * line with that same "-", and a conflict line the manifest marks
 * that the document no longer has put back with it. The rebase is
 * over at this gate, so "-" is not a question waiting for an answer
 * any more: it is the tool saying that this name is nobody's word,
 * and a later --verify of the settled result reads these lines as
 * the record of what drifted (documents-design.md, section 11.5).
 *
 * *np is what changed, and anything at all is drift: a mark with no
 * line is a conflict nobody answered, which the done gate must not
 * call clean. The write is the atomic one every other write of this
 * document makes; a document that is not there is left alone, since
 * this gate writes a record and never a file.
 *
 * Returns 0, or -1 with err set.
 */
static int
done_lines(struct resume *s, const struct zr_verify_report *rep, uint32_t *np)
{
	const struct zr_action *a;
	struct rcover rv;
	const char *nm;
	size_t len;
	uint32_t i, n = 0;

	*np = 0;
	if (s->hasres <= 0)
		return (0);
	for (i = 0; i < rep->zv_nrlines && i < s->res.zs_nlines; i++) {
		if (rep->zv_rline[i] != ZR_OC_PENDING &&
		    rep->zv_rline[i] != ZR_OC_DRIFTED)
			continue;
		if (s->res.zs_lines[i].zl_choice == ZR_CH_NONE)
			continue;
		s->res.zs_lines[i].zl_choice = ZR_CH_NONE;
		n++;
	}
	rcover_init(&rv, s);
	for (i = 0; i < rep->zv_ndiffs; i++) {
		len = 0;
		nm = zr_names_str(s->names, rep->zv_diffs[i].zn_name, &len);
		if (nm == NULL || len == 0 ||
		    rcover_has(&rv, s, nm, len) != 0)
			continue;
		if (zr_resolution_add_drift(&s->res,
		    (const unsigned char *)nm, len,
		    name_isdir(s, rep->zv_diffs[i].zn_name),
		    ZR_CH_NONE) != 0) {
			(void) snprintf(s->err, sizeof (s->err), "%s: cannot "
			    "take the drift line %s", s->respath, nm);
			rcover_fini(&rv);
			return (-1);
		}
		rcover_set(&rv, s->names, nm, len);
		n++;
	}
	for (i = 0; i < s->man.zp_nactions; i++) {
		a = &s->man.zp_actions[i];
		if (a->za_kind != ZR_ACT_CONFLICT ||
		    rcover_has(&rv, s, (const char *)a->za_path,
		    a->za_pathlen) != 0)
			continue;
		if (zr_resolution_add_conflict(&s->res, a->za_path,
		    a->za_pathlen, a->za_isdir, a->za_conflict,
		    ZR_CH_NONE) != 0) {
			(void) snprintf(s->err, sizeof (s->err), "%s: cannot "
			    "put back the conflict line %s", s->respath,
			    (const char *)a->za_path);
			rcover_fini(&rv);
			return (-1);
		}
		rcover_set(&rv, s->names, (const char *)a->za_path,
		    a->za_pathlen);
		n++;
	}
	rcover_fini(&rv);
	if (n == 0)
		return (0);
	if (zr_doc_write(s->respath, emit_resolution, &s->res, s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	*np = n;
	return (0);
}

/*
 * The final check, at the done gate: one document held against the
 * result, reported, and its verdict in *drift. Nothing here touches
 * the tree and nothing here fails on what it finds. Drift at this
 * gate is reported and not blocked on -- an edit made while the
 * conflicts were being answered is the person's work, and a gate
 * that failed on it would block done for good -- so the only failure
 * is a classification that could not be made at all, which is this
 * program's and not the tree's.
 *
 * record says this is the done gate itself and not a report of it:
 * what the check found is then written into the resolution as the
 * record of it (done_lines), and what that write finds is drift too.
 * A write that fails is said and does not stop the gate, for the
 * same reason drift does not. The --verify verb passes 0 and writes
 * nothing anywhere.
 *
 * The report goes to stderr unless the start was given --quiet,
 * which the record carries for the whole run: this one report is
 * what that flag silences, and it silences nothing else, not the
 * verdict and not the exit status (documents-design.md, section 7).
 */
static int
final_check(struct resume *s, const struct zr_parsed *m, const char *what,
    int *drift, int record)
{
	struct zr_verify_report rep;
	uint32_t wrote = 0;
	int rc;

	memset(&rep, 0, sizeof (rep));
	rc = classify(s, m, &rep);
	if (rc == 0) {
		if (s->rb.quiet == 0)
			print_report(s, m, &rep, what);
		*drift = found_drift(&rep);
		/*
		 * The record of what was found, which done does not
		 * block on either: a write that failed is said and the
		 * gate is passed, because a rebase nothing could close
		 * for want of a file is the thing this gate must never
		 * become. What was found stands in the exit status
		 * whether or not it could be written down.
		 */
		if (record != 0) {
			if (done_lines(s, &rep, &wrote) != 0) {
				(void) fprintf(stderr, "zfs_rebase: the final "
				    "check could not be written down: %s\n",
				    s->err);
				*drift = 1;
			} else if (wrote != 0) {
				*drift = 1;
			}
		}
	}
	zr_verify_report_fini(&rep);
	return (rc);
}

/*
 * The conflicts gate's own verify: the manifest and the resolution
 * held against the result and reported, and then the drift written
 * into the resolution. Nothing here touches the tree.
 */
static int
conflicts_check(struct resume *s)
{
	struct zr_verify_report rep;
	int rc;

	memset(&rep, 0, sizeof (rep));
	rc = classify(s, &s->man, &rep);
	if (rc == 0) {
		print_report(s, &s->man, &rep, "the manifest");
		rc = add_drift(s, &rep);
	}
	zr_verify_report_fini(&rep);
	return (rc);
}

/*
 * The walks and everything read through them, closed. The unmount of
 * the private mount comes after this and never before it: the three
 * trees are read through that mount's .zfs, and a walk still holding
 * a descriptor there is a mount somebody is using. It is written to
 * be safe to call twice, since the settle calls it and resume_close
 * calls it again on the way out.
 */
static void
close_trees(struct resume *s)
{
	int i;

	if (s->oracle != NULL) {
		zr_oracle_fini(s->oracle);
		s->oracle = NULL;
	}
	for (i = 2; i >= 0; i--) {
		if ((s->walked & (1 << i)) != 0)
			zr_walk_fini(&s->w[i]);
	}
	s->walked = 0;
	if (s->names != NULL) {
		zr_names_destroy(s->names);
		s->names = NULL;
	}
}

/*
 * The result put back into service, which is the first step of the
 * settle and the one that can refuse: the dataset off the private
 * mount, both properties as the header kept them, mounted where its
 * own mountpoint property says and asked whether it is there; or, in
 * the clone form, the clone off the private mount and left in the
 * void with the line that says how to place it. The walks are closed
 * by the caller before this, so nothing of this process is standing
 * in the mount.
 *
 * Returns 0 with privmnt cleared, or -1 with the reason already
 * printed and the mount still where it was.
 */
static int
settle_result(struct resume *s)
{
	int rc;

	if (!s->privmnt)
		return (0);
	rc = s->dataset ? handback(s->zfs, s->result, s->rb.readonly,
	    s->rb.canmount, s->workmnt, s->verbose) :
	    to_the_void(s->zfs, s->result, s->workmnt);
	if (rc != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s was not given back, so "
		    "it is still this run's: the record and the holds are "
		    "kept, and the next zfs_rebase --continue or --abort "
		    "finishes the settle once nothing is standing in %s\n",
		    s->result, s->workmnt);
		return (-1);
	}
	s->privmnt = 0;
	return (0);
}

/*
 * The last gate. The final check is made here, over the manifest, by
 * whichever invocation arrives -- the fresh run's own done or a
 * --continue's, under no flag at all -- and only then is the rebase
 * given back, in the order the settle's comment above sets out: the
 * walks closed, the result put back into service, the holds
 * released, the record taken off, the tool's own from snapshot
 * destroyed and the run directory removed. The report and the
 * verdict are both computed before any of it, so what the check
 * found does not depend on what the settle does. What says a rebase
 * reached done is that nothing of it is left on the result: done is
 * no phase and is never written.
 *
 * The holds go before the record because the tag in the record is
 * the only handle on them, so a kill between the two must leave the
 * handle rather than the holds; and the result goes back before
 * either, because a dataset at the private mount with no record is a
 * dataset nothing names (R3 of the code review).
 *
 * done never blocks on drift. What the check finds is reported and
 * carried out in the exit status -- 3 rather than 0 -- and the gate
 * is passed all the same: the record cleared, the result settled and
 * the run directory taken away, exactly as on a clean pass. The
 * rebase is over either way, and a rebase that could not be closed
 * because somebody edited a file in it would be a rebase nothing
 * could ever end. Two things are not that: a check that cannot be
 * made at all, which is this program failing rather than the tree
 * drifting, and a result that cannot be given back, which is the
 * one promise done has to keep. Both leave the gate unpassed and the
 * rebase standing for the next verb, and both exit 3.
 *
 * The resolution is classified with it, since the check is one call:
 * a name kept is never compared, and a name answered onto or from is
 * held against that side's object.
 */
static int
done_gate(struct resume *s)
{
	char e[512];
	int drift = 0;

	if (final_check(s, &s->man, "the manifest", &drift, 1) != 0)
		return (vfail(s, EXIT_INTERNAL, "verify"));
	if (drift && s->rb.quiet == 0)
		(void) fprintf(stderr, "zfs_rebase: the final check found "
		    "drift, which done does not block on: %s keeps it, the "
		    "exit status says so, and the gate is passed all the "
		    "same\n", s->result);
	zr_pause(ZR_GATE_DONE);
	/*
	 * The clone is read-only outside a stage, and it goes to the
	 * void that way; the dataset form's readonly is the header's
	 * and the hand-back writes it.
	 */
	(void) ro_on(s);
	close_trees(s);
	if (settle_result(s) != 0)
		return (EXIT_INTERNAL);
	release_record(s);
	clear_record(s->zfs, s->result, s->verbose);
	/*
	 * The from snapshot the tool took for itself lives exactly as
	 * long as the rebase. It goes after the walks let go of it
	 * and after the holds were released, both of which have
	 * happened.
	 */
	if (made_says(&s->rb, "from")) {
		if (zr_zfs_destroy_snap(s->zfs, s->rb.from, e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    s->rb.from, e);
		else if (s->verbose)
			(void) fprintf(stderr, "zfs_rebase: %s was the tool's "
			    "own and is destroyed\n", s->rb.from);
	}
	/*
	 * And the directory last, since everything above read the
	 * documents in it. mnt is empty by now: the settle took the
	 * result off it.
	 */
	rundir_done(s->result, s->rb.manifest, s->verbose);
	s->settled = 1;
	if (s->dataset)
		(void) fprintf(stderr, "zfs_rebase: %s is the rebased tree, "
		    "and %s is what it was before\n", s->result,
		    s->rb.presnap);
	return (drift ? EXIT_INTERNAL : EXIT_CLEAN);
}

/* Has a signal come in? Then the gate reached is the gate that stays. */
static int
vstopped(struct resume *s)
{
	if (zr_apply_stop == 0)
		return (0);
	(void) snprintf(s->err, sizeof (s->err), "interrupted");
	return (-1);
}

/*
 * The choices of a complete resolution, made true on the result, and
 * the check that they hold. The check is idempotence: the same call
 * is made a second time over the same document and must change
 * nothing -- no name made the chosen side's, none removed, none
 * pooled onto its anchor, no directory freed -- because every line
 * that would change something now is a line the first call did not
 * make true. When one does, the message names it, and that is an
 * internal failure and not drift: it is the apply not doing what the
 * document said, which is this program's fault.
 *
 * Once verify-choices lands, this should also classify the
 * resolution against the trees after the second pass -- keep never
 * compared, onto and from held against that side -- the way
 * choices_hold does below, and say so in its own words rather than
 * in the final check's.
 */
static int
apply_choices(struct resume *s, const struct zr_resolution *res)
{
	struct zr_apply_stats st, again;
	struct zr_walk *pre;
	const char *first;

	/*
	 * The result as this verb last read it, which is the tree the
	 * first pass is made over: nothing has written to it since --
	 * the conflicts gate writes the resolution and never the tree
	 * -- so the walk in hand is the walk this call would have
	 * made for itself (R13 of the code review).
	 */
	pre = (s->walked & (1 << ZS_RESULT)) != 0 ? &s->w[ZS_RESULT] : NULL;
	if (zr_apply_choices(res, &s->man, s->workmnt, s->names,
	    &s->w[ZS_ONTO], &s->w[ZS_FROM], pre, &st, s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: the choices: %llu kept, "
		    "%llu made, %llu removed, %llu linked, %llu director%s "
		    "freed, %llu left alone, %llu bytes\n",
		    (unsigned long long)st.zs_kept,
		    (unsigned long long)st.zs_made,
		    (unsigned long long)st.zs_dropped,
		    (unsigned long long)st.zs_linked,
		    (unsigned long long)st.zs_latedirs,
		    st.zs_latedirs == 1 ? "y" : "ies",
		    (unsigned long long)st.zs_skipped,
		    (unsigned long long)st.zs_bytes);
	/*
	 * The tree as the first pass left it, walked once. It is what
	 * the second pass is made over, and, because that pass must
	 * change nothing, it is also the tree the check after this
	 * one asks about: the walk is made here, between the two
	 * passes, and stands for both. Where the second pass does
	 * change something the run stops below, and what the walk
	 * says about the tree stops mattering.
	 */
	if (rescan_result(s) != 0)
		return (-1);
	if (zr_apply_choices(res, &s->man, s->workmnt, s->names,
	    &s->w[ZS_ONTO], &s->w[ZS_FROM], &s->w[ZS_RESULT], &again, s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	if (again.zs_made == 0 && again.zs_dropped == 0 &&
	    again.zs_linked == 0 && again.zs_latedirs == 0)
		return (0);
	first = again.zs_line == ZR_LINE_NONE ? "a blocked directory" :
	    (const char *)res->zs_lines[again.zs_line].zl_path;
	(void) snprintf(s->err, sizeof (s->err), "a second pass over %s "
	    "changed %llu name%s and freed %llu director%s, first %s, so the "
	    "first pass did not make the document true", s->respath,
	    (unsigned long long)(again.zs_made + again.zs_dropped +
	    again.zs_linked), again.zs_made + again.zs_dropped +
	    again.zs_linked == 1 ? "" : "s",
	    (unsigned long long)again.zs_latedirs,
	    again.zs_latedirs == 1 ? "y" : "ies", first);
	return (-1);
}

/*
 * The resolution read again off the file, into the one copy this
 * verb goes on with -- the rule reset_resolution keeps and the rule
 * every stage keeps: what is applied and what is checked must be one
 * document, so there is one parse of it and never two
 * (documents-design.md, section 11.5). Returns 1 read, 0 gone, -1
 * with reserr set.
 */
static int
reread_resolution(struct resume *s)
{
	zr_resolution_fini(&s->res);
	s->hasres = read_resolution(s, &s->res);
	if (s->hasres < 0)
		(void) snprintf(s->reserr, sizeof (s->reserr), "%s", s->err);
	return (s->hasres);
}

/*
 * Does the side this line named have no such name? That is what
 * makes a line a removal, and the one shape a removal can fail to
 * be made in is a directory something is still inside.
 */
static int
side_lacks(const struct resume *s, const struct zr_rline *l)
{
	const struct zr_walk *w;
	zr_name_t nm;

	w = &s->w[l->zl_choice == ZR_CH_FROM ? ZS_FROM : ZS_ONTO];
	nm = zr_names_lookup(s->names, (const char *)l->zl_path,
	    l->zl_pathlen);
	if (nm == ZR_NAME_NONE)
		return (1);
	return (zr_tree_pool(&w->zw_tree, nm) == ZR_POOL_NONE);
}

/*
 * After the choices: the classification the second pass already
 * implies, made anyway, so that applying2 is checked the way
 * applying1 is -- by the one verify. Every onto and from line must
 * be done. The one exception is a directory line whose side has no
 * such directory and that is still there: something inside it is
 * being kept, so the removal could not be made, which is the
 * choice's form of blocked and no fault of the apply. It reads
 * pending where onto still holds the directory and drifted where
 * onto never had it either, and both are that same state
 * (documents-design.md, section 11.5). The done gate is where it is
 * counted: it goes into the exit status and into the document, and
 * it does not stop the rebase here.
 */
static int
choices_hold(struct resume *s)
{
	struct zr_verify_report rep;
	uint32_t i;
	int rc = -1;

	if (s->hasres <= 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s was not read "
		    "when this verb opened the rebase", s->respath);
		return (-1);
	}
	memset(&rep, 0, sizeof (rep));
	if (classify(s, &s->man, &rep) != 0)
		goto out;
	for (i = 0; i < rep.zv_nrlines; i++) {
		const struct zr_rline *l = &s->res.zs_lines[i];

		if (rep.zv_rline[i] == ZR_OC_DONE)
			continue;
		if ((rep.zv_rline[i] == ZR_OC_PENDING ||
		    rep.zv_rline[i] == ZR_OC_DRIFTED) && l->zl_isdir != 0 &&
		    side_lacks(s, l) != 0)
			continue;
		(void) snprintf(s->err, sizeof (s->err), "after the choices, "
		    "%s reads %s", (const char *)l->zl_path,
		    zr_outcome_str(rep.zv_rline[i]));
		goto out;
	}
	rc = 0;
out:
	zr_verify_report_fini(&rep);
	return (rc);
}

/*
 * applying2: the choices of the resolution, carried out. The document
 * is read again here, because this is a gate a --continue can arrive
 * at on its own, and a stage cannot begin without the document it is
 * the stage of. It is read into s->res and nowhere else, so that the
 * copy the choices are made from is the copy choices_hold and the
 * done gate are then made against.
 */
static int
stage2(struct resume *s)
{
	uint32_t left;
	int rc;

	rc = reread_resolution(s);
	if (rc < 0)
		return (vfail(s, EXIT_PRECOND, "resolution"));
	if (rc == 0)
		return (no_resolution(s));
	left = zr_resolution_unanswered(&s->res);
	if (left != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s is at applying2 "
		    "and %u name%s of %s went back to unanswered", s->result,
		    left, left == 1 ? "" : "s", s->respath);
		return (vfail(s, EXIT_PRECOND, NULL));
	}
	put_phase(s->zfs, s->result, ZR_PHASE_APPLYING2);
	rc = EXIT_INTERNAL;
	if (ro_off(s) != 0)
		goto out;
	zr_pause(ZR_PHASE_APPLYING2);
	/*
	 * And the trees this verb goes on with, which the choices have
	 * just changed: apply_choices walks the result between its two
	 * passes and the second pass leaves that walk true, so the
	 * done gate classifies against the result as it stands now.
	 */
	if (apply_choices(s, &s->res) != 0)
		goto out;
	if (choices_hold(s) != 0)
		goto out;
	rc = 0;
out:
	if (ro_on(s) != 0)
		rc = EXIT_INTERNAL;
	if (rc != 0)
		return (vfail(s, rc, "apply"));
	if (vstopped(s) != 0)
		return (vfail(s, EXIT_INTERNAL, "apply"));
	return (done_gate(s));
}

/*
 * The conflicts gate. The resolution is complete, in which case the
 * rebase goes on into applying2, or a name of it is still unanswered,
 * in which case this is where it waits and the phase does not move.
 * Completeness plus this --continue is the whole of the signal: the
 * move is made on human input, and nothing but the person who
 * answered the conflicts can say they are answered.
 *
 * Every --continue that arrives here checks first, under no flag,
 * and reports, and writes into the resolution and nowhere else. The
 * tree is the person's from this gate on -- they
 * are answering conflicts in it, by hand or through a picker -- and
 * nothing here can tell an edit of theirs from a stray, so nothing
 * here touches the tree. What it does instead is say what it found:
 * every name that no longer stands as onto had it becomes a drift
 * line with the choice keep, which the person can change to onto or
 * to from. The one fix in the tool is applying1's own self-check,
 * which ran before this gate was ever written.
 *
 * checked says that self-check has just run in this same invocation,
 * which is what arriving here from stage1 means: the tree was held
 * against the manifest and mended a moment ago, so the check here
 * would be the same check over the same walks and finds the same
 * nothing. It is skipped rather than made twice.
 */
static int
stage_conflicts(struct resume *s, int checked)
{
	uint32_t left, total;

	if (s->hasres < 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s", s->reserr);
		return (vfail(s, EXIT_PRECOND, "resolution"));
	}
	if (s->hasres == 0)
		return (no_resolution(s));
	if (checked == 0 && conflicts_check(s) != 0)
		return (vfail(s, EXIT_INTERNAL, "verify"));
	left = zr_resolution_unanswered(&s->res);
	total = s->res.zs_nlines;
	if (left != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: conflicts "
		    "unresolved\n", s->result);
		unanswered_note(s, left, total);
		return (EXIT_CONFLICTS);
	}
	/*
	 * The document is complete, which is half of the signal; the
	 * other half is the command, and --no-merge is the command
	 * saying not yet. The gate is left where it is, so the next
	 * --continue without the flag passes it.
	 */
	if (s->nomerge) {
		(void) fprintf(stderr, "zfs_rebase: %s: the resolution is "
		    "answered in full, and --no-merge leaves the merge to "
		    "you\n", s->result);
		unanswered_note(s, left, total);
		return (EXIT_CONFLICTS);
	}
	return (stage2(s));
}

/*
 * applying1: the recorded manifest, and the gate that follows it.
 *
 * The check of this stage is the stage's own self-check, which is
 * always on and is no flag's, and it is the one check in the tool
 * that mends what it finds: up to the conflicts gate the result is
 * the run's own, so a name that is not what the expected tree says
 * is a stray. A rebase whose decision declared no conflict goes
 * straight from here to the done gate and its final check.
 */
static int
stage1(struct resume *s)
{
	if (stage_apply(s, &s->man, ZR_PHASE_APPLYING1) != 0)
		return (vfail(s, EXIT_INTERNAL, "apply"));
	if (vstopped(s) != 0)
		return (vfail(s, EXIT_INTERNAL, "apply"));
	if (s->man.zp_conflicts_declared == 0)
		return (done_gate(s));
	put_phase(s->zfs, s->result, ZR_PHASE_CONFLICTS);
	zr_pause(ZR_PHASE_CONFLICTS);
	(void) fprintf(stderr, "zfs_rebase: %u conflict%s; the clean actions "
	    "are applied and %s waits at conflicts\n",
	    s->man.zp_conflicts_declared,
	    s->man.zp_conflicts_declared == 1 ? "" : "s", s->result);
	conflicts_note(s);
	/*
	 * A complete document and no --no-merge is the signal, whoever
	 * arrives with it: a --continue that reaches this gate from
	 * applying1 with the document already answered -- a --restart
	 * under a --take flag writes one, and so does somebody who
	 * answered the conflicts before the rebase was resumed -- goes
	 * on the way the fresh run does, through the one gate function.
	 * Under --no-merge, or with a name still unanswered, it stops,
	 * and that function says which.
	 */
	if (s->hasres > 0 && zr_resolution_unanswered(&s->res) == 0)
		return (stage_conflicts(s, 1));
	return (EXIT_CONFLICTS);
}

/*
 * Resume from the gate the record names. "decided" and "applying1"
 * are one place to start: the manifest is the decision either way,
 * and applying it again over a tree nothing was applied to is what
 * an idempotent apply does. There is no phase at all until the
 * decision, which is a rebase to abort and not one to resume
 * (undecided, refused by resume_open before the result is taken
 * over, and refused here for a caller that came another way). There
 * is no done to resume from either: a rebase that reached it carries
 * no record, and read_record has already refused.
 */
static int
continue_from(struct resume *s)
{
	const char *phase = s->rb.phase;

	if (phase[0] == '\0')
		return (undecided(s));

	/*
	 * --no-merge stops at the conflicts gate, so it says
	 * something only up to it. A rebase already at applying2 is
	 * past the merge: there is no gate left for the flag to hold,
	 * and carrying on regardless would be doing the one thing it
	 * was given to prevent.
	 */
	if (s->nomerge != 0 && strcmp(phase, ZR_PHASE_APPLYING2) == 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s is at \"%s\", "
		    "past the merge; --no-merge has no gate left to stop at",
		    s->result, phase);
		return (vfail(s, EXIT_PRECOND, NULL));
	}
	if (strcmp(phase, ZR_PHASE_DECIDED) == 0 ||
	    strcmp(phase, ZR_PHASE_APPLYING1) == 0)
		return (stage1(s));
	if (strcmp(phase, ZR_PHASE_CONFLICTS) == 0)
		return (stage_conflicts(s, 0));
	if (strcmp(phase, ZR_PHASE_APPLYING2) == 0)
		return (stage2(s));
	(void) snprintf(s->err, sizeof (s->err), "%s is at \"%s\", which is "
	    "no gate of this tool", s->result, phase);
	return (vfail(s, EXIT_PRECOND, NULL));
}

/*
 * The other half of --verify: a rebase that is over. A result that
 * reached done carries no record and, unless the run was given -o,
 * no manifest either -- done unlinked the one it wrote and took the
 * run directory with it -- so the only settled rebase there is to
 * ask about is one whose manifest the user kept, and the file is
 * what names it (documents-design.md, sections 4 and 7).
 *
 * What the record would have given, the header gives: the manifest
 * is the file that was named, the three snapshots and their guids
 * are its own, and #made says which of them the tool took for itself
 * and destroyed at done. The two documents are both required here,
 * where a rebase in flight can go on without the resolution and only
 * say so: the settled check holds the result against onto's names
 * with the manifest's actions and the resolution's choices applied,
 * and a check made without the choices would be answering a
 * different question.
 *
 * Nothing here searches the pool: each input is looked up by the
 * name the header kept, its guid must be the guid the header kept,
 * and a name that is gone or that another snapshot wears now stops
 * the check. Returns EXIT_CLEAN, or the status to give up with.
 */
static int
settled_open(struct resume *s, const struct zr_verb_opts *o)
{
	/*
	 * An identifier that found this rebase by name found it by
	 * its record, and this dataset has none: only a manifest can
	 * have named this one, and the resolution's own refusal says
	 * so before ever reaching here. It is held all the same,
	 * because a record can go between the two reads.
	 */
	if (s->given[0] == '\0') {
		(void) fprintf(stderr, "zfs_rebase: %s: no rebase in flight; "
		    "for a settled result give the manifest\n", s->result);
		return (EXIT_PRECOND);
	}
	(void) snprintf(s->rb.manifest, sizeof (s->rb.manifest), "%s",
	    s->given);
	if (read_manifest(s) != 0 || resume_paths(s) != 0)
		return (vfail(s, EXIT_PRECOND, NULL));
	s->hasres = read_resolution(s, &s->res);
	if (s->hasres == 0) {
		(void) fprintf(stderr, "zfs_rebase: %s is gone; a settled "
		    "result is checked against both documents, the manifest "
		    "and the resolution beside it\n", s->respath);
		return (EXIT_PRECOND);
	}
	if (s->hasres < 0)
		return (vfail(s, EXIT_PRECOND, "resolution"));
	if (find_inputs(s, ZF_SETTLED) != 0 || check_given(s, o) != 0)
		return (vfail(s, EXIT_PRECOND, NULL));
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is settled; %s is the "
		    "rebase it carried\n", s->result, s->rb.manifest);
	return (EXIT_CLEAN);
}

/*
 * What every verb does first: it must be root and libzfs must open.
 * Both come before the identifier is resolved, because two of its
 * steps ask the pools what carries a record.
 *
 * A handle already in s is one the caller lent -- the fresh run's
 * own, at its done gate -- and is opened by nobody here and closed
 * by nobody here (R13 of the code review). The two handles could
 * never disagree about anything: the tool leaves libzfs's mount
 * table cache off, so every lookup re-reads the system table. One
 * handle is simply one fewer.
 */
static int
resume_start(struct resume *s)
{
	if (geteuid() != 0) {
		(void) fprintf(stderr, "zfs_rebase: must run as root\n");
		return (EXIT_PRECOND);
	}
	if (s->zfs != NULL)
		return (EXIT_CLEAN);
	if (zr_zfs_open(&s->zfs, s->err, sizeof (s->err)) != 0)
		return (vfail(s, EXIT_PRECOND, "libzfs"));
	return (EXIT_CLEAN);
}

/*
 * And what every verb does with the result once it has been found:
 * it must carry a record, the manifest that record names must parse
 * and name it back, every input its header names must still be the
 * snapshot it named, and what the command said about the rebase
 * beyond its name must agree with all of that.
 *
 * The --verify verb is the one that also has a settled result to
 * answer for, and the record is what tells the two apart: settled_
 * open takes it from there. Returns EXIT_CLEAN, or the status to
 * give up with.
 */
static int
resume_found(struct resume *s, const struct zr_verb_opts *o, int byguid)
{
	/*
	 * In flight or settled, which only the report has to ask: a
	 * rebase that reached done took its record off, and every
	 * other verb moves a rebase and has nothing there to move.
	 * The dataset itself is asked for first, so that a result
	 * somebody destroyed is named as what it is rather than as a
	 * property that could not be read.
	 */
	if (s->report) {
		int got;

		got = zr_zfs_exists(s->zfs, s->result, s->err,
		    sizeof (s->err));
		if (got < 0)
			return (vfail(s, EXIT_PRECOND, NULL));
		if (got == 0) {
			(void) fprintf(stderr, "zfs_rebase: %s: there is no "
			    "such dataset\n", s->result);
			return (EXIT_PRECOND);
		}
		got = has_record(s);
		if (got < 0)
			return (vfail(s, EXIT_PRECOND, NULL));
		if (got == 0) {
			s->post = 1;
			return (settled_open(s, o));
		}
	}
	if (read_record(s) != 0 || resume_paths(s) != 0 ||
	    find_inputs(s, byguid ? ZF_GUID : ZF_NAME) != 0 ||
	    check_given(s, o) != 0)
		return (vfail(s, EXIT_PRECOND, NULL));
	/*
	 * A record with no phase at all is a rebase that was born and
	 * never decided: the file it names is the header this run was
	 * born with and carries no decision to carry out
	 * (documents-design.md, section 11.1). The verbs that move a
	 * rebase refuse it here, before they take the result over, so
	 * that it is left exactly where the kill left it; --abort has
	 * everything the header gave it and takes the rebase away.
	 * The report is not one of them: it says what the document
	 * says, which of a birth manifest is nothing.
	 */
	if (s->report == 0 && s->rb.phase[0] == '\0')
		return (undecided(s));
	/*
	 * The resolution, read once and kept: every gate from
	 * conflicts on classifies against it, and every verb that has
	 * to act on it reads it here rather than again. A document
	 * that cannot be read is not a refusal of its own -- the
	 * report says so and checks what it can -- so what went wrong
	 * is kept beside the verdict, for the verbs that do refuse.
	 */
	s->hasres = read_resolution(s, &s->res);
	if (s->hasres < 0)
		(void) snprintf(s->reserr, sizeof (s->reserr), "%s", s->err);
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is at %s, held under "
		    "%s\n", s->result, s->rb.phase[0] != '\0' ? s->rb.phase :
		    "no gate yet", s->rb.tag);
	return (EXIT_CLEAN);
}

/*
 * A verb, from the identifier it was given: root, libzfs, the five
 * steps of the resolution (documents-design.md, section 11.4), and
 * then the record. Returns EXIT_CLEAN, or the status to give up
 * with, every refusal already printed.
 */
static int
resume_open(struct resume *s, const struct zr_verb_opts *o, int byguid)
{
	struct zr_ident id;
	int rc;

	rc = resume_start(s);
	if (rc != EXIT_CLEAN)
		return (rc);
	if (resolve_ident(s->zfs, o->ident, &id) != 0)
		return (EXIT_PRECOND);
	(void) snprintf(s->result, sizeof (s->result), "%s", id.zi_result);
	(void) snprintf(s->given, sizeof (s->given), "%s", id.zi_path);
	/*
	 * The parse the resolution made of a manifest it was given,
	 * taken over whole: read_manifest adopts it where the record
	 * names that same file, and gives it back where it does not.
	 */
	s->man = id.zi_man;
	s->parsed = id.zi_parsed;
	memset(&id.zi_man, 0, sizeof (id.zi_man));
	id.zi_parsed = 0;
	/*
	 * A run directory with no record on any dataset is a crash
	 * leftover and not a rebase: there is nothing to continue, to
	 * restart or to report on, and --abort is what clears it.
	 */
	if (id.zi_rundir != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s/%s is the directory of "
		    "a run whose result carries no record; there is no rebase "
		    "to move, and zfs_rebase --abort %s removes it\n",
		    workdir(), s->result, o->ident);
		return (EXIT_PRECOND);
	}
	return (resume_found(s, o, byguid));
}

/*
 * And the same machinery over a result the caller already holds:
 * the fresh run's own done gate, which has just written the record
 * on that dataset and has nothing to resolve. It searches no pool
 * and reads no identifier; everything after the record is the same
 * path a verb takes, cross-check included.
 */
static int
resume_open_result(struct resume *s, const struct zr_verb_opts *o,
    const char *result, int byguid)
{
	int rc;

	rc = resume_start(s);
	if (rc != EXIT_CLEAN)
		return (rc);
	(void) snprintf(s->result, sizeof (s->result), "%s", result);
	return (resume_found(s, o, byguid));
}

/*
 * Where a report reads the result, which is the one thing --verify
 * does differently from every other verb: it takes nothing over. It
 * reads the result where it is mounted -- an open rebase's at the
 * private mount, which is where the rule puts it; a settled dataset
 * at home; a settled clone where a hand has placed it -- and mounts
 * it only where it is mounted nowhere, at the run directory's mnt
 * with the same call the run uses, reading it there and taking that
 * mount away again (resume_close). A settled clone is unmounted with
 * no mountpoint of its own (documents-design.md, section 5) and a
 * reboot leaves an open rebase's result the same way, so the one
 * branch serves both.
 *
 * No property of the result is touched either way: a report never
 * sets readonly and moves nothing that is where it should be
 * (documents-design.md, section 11.6). A clone outside a stage has
 * readonly on, so the mount it is read at is read-only, which is all
 * a check ever wanted of it.
 *
 * The run directory is the settled check's alone to make and to take
 * away. An open rebase's is the run's, holding its documents and the
 * mount point every verb after this one uses, so a report that had
 * to mount leaves the directory exactly as it found it.
 */
static int
report_mount(struct resume *s)
{
	char at[ZR_NAME_MAX];
	int rc;

	rc = zr_zfs_mounted_at(s->zfs, s->result, at, sizeof (at), s->err,
	    sizeof (s->err));
	if (rc < 0)
		return (-1);
	if (rc > 0) {
		(void) snprintf(s->workmnt, sizeof (s->workmnt), "%s", at);
		if (s->verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is mounted at "
			    "%s and is read there\n", s->result, s->workmnt);
		return (0);
	}
	if (mkdir_p(s->workmnt, s->err, sizeof (s->err)) != 0)
		return (-1);
	if (zr_zfs_mount_at(s->zfs, s->result, s->workmnt, s->err,
	    sizeof (s->err)) != 0) {
		if (s->post)
			(void) rmdir_run(s->result);
		return (-1);
	}
	s->mademnt = 1;
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is mounted nowhere; "
		    "this check mounts it at %s and takes that away again\n",
		    s->result, s->workmnt);
	return (0);
}

/* The clone mounted and the trees walked; the manifest is read. */
static int
resume_trees(struct resume *s)
{
	if (s->report ? report_mount(s) != 0 : take_over(s) != 0)
		return (-1);
	s->names = zr_names_create();
	if (s->names == NULL) {
		(void) snprintf(s->err, sizeof (s->err), "out of memory");
		return (-1);
	}
	/*
	 * onto, from and the result, and not the base. Nothing here
	 * decides anything -- the manifest is the decision -- and the
	 * classifier's oracle is over these three; the base is checked
	 * like the other inputs and its tree is never read.
	 */
	if (walk_side(s, ZI_ONTO, ZS_ONTO) != 0 ||
	    walk_side(s, ZI_FROM, ZS_FROM) != 0)
		return (-1);
	if (zr_walk(s->workmnt, s->names, &s->w[ZS_RESULT], s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	s->walked |= 1 << ZS_RESULT;
	return (build_oracle(s));
}

/*
 * The way out of every verb. The settle itself is not here: a verb
 * that reached done was settled at the done gate, in the order the
 * settle's comment sets out, and a verb that stopped short of done
 * -- at conflicts, at a refusal, at a caught signal, at a hand-back
 * that refused -- leaves the result at the private mount with its
 * record and its holds, which is where the next verb takes it from
 * (documents-design.md, section 5). What is left here is this
 * process's own: the readonly flag a stage left off, the walks, the
 * two documents in memory, the mount a settled check made for
 * itself, and the libzfs handle.
 */
static void
resume_close(struct resume *s)
{
	char e[512];
	int rc;

	(void) ro_on(s);
	close_trees(s);
	if (s->parsed != 0)
		zr_parsed_fini(&s->man);
	zr_resolution_fini(&s->res);
	/*
	 * And the mount a check made for itself, undone whatever the
	 * check found: a result that was mounted nowhere when the verb
	 * began is mounted nowhere when it ends (documents-design.md,
	 * sections 7 and 11.6). For a settled result the directory the
	 * mount needed goes with it, so a rebase that is over leaves
	 * nothing under WORKDIR either way; an open rebase's run
	 * directory is the run's and stays, since the next verb takes
	 * the rebase from there. Nothing here can change what the
	 * check returned; every failure is reported and none is passed
	 * up.
	 */
	if (s->mademnt) {
		s->mademnt = 0;
		if (zr_zfs_unmount(s->zfs, s->result, e, sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: unmount %s: %s\n",
			    s->result, e);
		if (s->post) {
			rc = rmdir_run(s->result);
			if (rc != 0)
				(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
				    s->rundir, strerror(rc));
			else if (s->verbose)
				(void) fprintf(stderr,
				    "zfs_rebase: removed %s\n", s->rundir);
		}
	}
	if (s->zfs != NULL && s->zfslent == 0)
		zr_zfs_close(s->zfs);
}

int
zr_continue(const struct zr_verb_opts *o)
{
	struct sigaction saved[ZR_NSIG];
	struct resume s;
	int rc;

	memset(&s, 0, sizeof (s));
	s.nomerge = o->nomerge;
	s.verbose = o->verbose;
	zr_pause_open();
	signals_install(saved);
	rc = resume_open(&s, o, 0);
	if (rc == EXIT_CLEAN) {
		rc = resume_trees(&s) != 0 ?
		    vfail(&s, EXIT_PRECOND, s.result) : continue_from(&s);
	}
	resume_close(&s);
	signals_restore(saved);
	return (rc);
}

/*
 * --restart's half of the resolution: the edits go with the tree they
 * were edits on. The skeleton is built again from the recorded
 * manifest, which is still the decision, so what comes back is the
 * document the run wrote in the first place -- every line unanswered,
 * or every line answered onto or from where the run was given a
 * --take flag, which the record keeps for exactly this. What is
 * discarded is the answering somebody did afterwards, and not the
 * instruction the rebase was started with.
 * A failure here is a failure of the restart: a rebase whose result
 * went back to onto and whose resolution still holds yesterday's
 * answers is worse than one that stopped.
 */
static int
reset_resolution(struct resume *s)
{
	struct zr_resolution res;
	int rc;

	if (zr_resolution_skeleton(&s->man, take_choice(s->rb.take),
	    &res) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "out of memory");
		zr_resolution_fini(&res);
		return (-1);
	}
	rc = zr_doc_write(s->respath, emit_resolution, &res, s->err,
	    sizeof (s->err));
	if (rc == 0 && s->verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is a skeleton again, "
		    "%u name%s, %u to answer\n", s->respath, res.zs_nlines,
		    res.zs_nlines == 1 ? "" : "s",
		    zr_resolution_unanswered(&res));
	zr_resolution_fini(&res);
	/*
	 * And the copy this verb goes on with, read back off the file
	 * that was just written: the classification the stage after
	 * this one makes must be against the document on disk, not
	 * against the answers the restart has just discarded.
	 */
	if (rc == 0)
		(void) reread_resolution(s);
	return (rc);
}

/*
 * The trees, the recorded manifest and the resolution put back to the
 * skeleton: what both forms of --restart do once the result is onto
 * again. Returns 0, or the status to give up with.
 */
static int
restart_from(struct resume *s)
{
	if (resume_trees(s) != 0)
		return (vfail(s, EXIT_PRECOND, s->result));
	if (reset_resolution(s) != 0)
		return (vfail(s, EXIT_INTERNAL, "resolution"));
	return (stage1(s));
}

int
zr_restart(const struct zr_verb_opts *o)
{
	struct sigaction saved[ZR_NSIG];
	struct resume s;
	int rc;

	memset(&s, 0, sizeof (s));
	s.verbose = o->verbose;
	zr_pause_open();
	signals_install(saved);
	rc = resume_open(&s, o, 0);
	if (rc != EXIT_CLEAN)
		goto done;
	/*
	 * --restart applies the manifest again from the first gate,
	 * and that reads both sides. A tree it would have to read and
	 * cannot is the end of it: there is nothing to start again
	 * from.
	 */
	if (s.miss != 0) {
		(void) snprintf(s.err, sizeof (s.err), "%s: a tree the "
		    "manifest reads is gone; there is nothing to restart it "
		    "from", s.result);
		rc = vfail(&s, EXIT_PRECOND, NULL);
		goto done;
	}
	if (s.dataset) {
		/*
		 * The dataset form puts the result back by rolling it
		 * to the pre-apply snapshot the header names, which
		 * is what the clone form's destroy-and-clone-again
		 * does: onto's tree exactly as it was, with the same
		 * record on it and the phase back at the decision,
		 * since the decision is what it is about to apply
		 * again. The rollback wants no unmount -- the
		 * kernel suspends and resumes the filesystem around
		 * it -- so it is made before the trees are read and
		 * the dataset is taken over, and nothing this process
		 * holds open is in the way.
		 *
		 * The phase goes down before the rollback and not
		 * after it: a kill between the two then leaves
		 * "decided" over a tree the apply is about to be made
		 * on from the start, which is what a --continue from
		 * that gate does anyway, where the other order would
		 * leave a later gate's name over a tree that has gone
		 * back to onto.
		 */
		put_phase(s.zfs, s.result, ZR_PHASE_DECIDED);
		(void) snprintf(s.rb.phase, sizeof (s.rb.phase), "%s",
		    ZR_PHASE_DECIDED);
		if (zr_zfs_rollback(s.zfs, s.result, s.rb.presnap, s.err,
		    sizeof (s.err)) != 0) {
			rc = vfail(&s, EXIT_INTERNAL, "rollback");
			goto done;
		}
		if (s.verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is %s again\n",
			    s.result, s.rb.presnap);
		rc = restart_from(&s);
		goto done;
	}
	if (s.rb.form != ZR_HFORM_CLONE) {
		(void) snprintf(s.err, sizeof (s.err), "%s was made in the %s "
		    "form, which this tool does not know", s.result,
		    form_word(s.rb.form));
		rc = vfail(&s, EXIT_PRECOND, NULL);
		goto done;
	}
	/*
	 * Destroy and clone again, with the record the old one carried:
	 * the same tag, so the holds it named are still this rebase's,
	 * the same manifest, which is still the decision, and no
	 * phase, because the new clone has passed no gate. Nothing is
	 * mounted here: the new clone's mountpoint is none like the
	 * old one's, and restart_from's take_over puts it at the
	 * private mount as it would after a reboot. The holds
	 * themselves are untouched -- they are on the snapshots and
	 * not on the clone -- and onto's snapshot cannot go while a
	 * clone of it lives, so there is no moment here where the
	 * inputs are unprotected.
	 */
	if (zr_zfs_destroy(s.zfs, s.result, s.err, sizeof (s.err)) != 0) {
		rc = vfail(&s, EXIT_INTERNAL, "destroy");
		goto done;
	}
	if (mkdir_p(s.workmnt, s.err, sizeof (s.err)) != 0 ||
	    zr_zfs_clone(s.zfs, s.rb.onto, s.result, &s.rb.rec, s.err,
	    sizeof (s.err)) != 0) {
		rc = vfail(&s, EXIT_INTERNAL, "clone");
		/*
		 * The record went with the clone, and the tag it named
		 * is the only handle on the three holds, so it is
		 * printed here rather than lost: this is the one
		 * moment in the tool where a hold can outlive the
		 * record that names it.
		 */
		(void) fprintf(stderr, "zfs_rebase: %s is destroyed and could "
		    "not be made again; %s, %s and %s are still held under "
		    "%s, which zfs release takes back\n", s.result, s.rb.base,
		    s.rb.from, s.rb.onto, s.rb.tag);
		goto done;
	}
	/*
	 * And the gate the fresh clone stands at, which is the
	 * decision it is about to have applied to it: the create
	 * carries the manifest and the tag and no phase, so this is
	 * the one write that puts it back. A kill in the moment
	 * between them leaves a clone whose record has no phase --
	 * born and not decided, by the rule -- and --abort, which
	 * that refusal names, takes it away with the header in hand.
	 */
	put_phase(s.zfs, s.result, ZR_PHASE_DECIDED);
	(void) snprintf(s.rb.phase, sizeof (s.rb.phase), "%s",
	    ZR_PHASE_DECIDED);
	if (s.verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is a fresh clone of "
		    "%s again\n", s.result, s.rb.onto);
	rc = restart_from(&s);
done:
	resume_close(&s);
	signals_restore(saved);
	return (rc);
}

/*
 * What the report could not check, and why. A tree that is not there
 * takes with it every action that would have had to be read against
 * it; the base takes nothing, since no verify reads it.
 */
static void
explain_gone(const struct resume *s)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (s->gone[i] == 0)
			continue;
		if (i == ZI_BASE && no_base(rec_snap(&s->rb, i))) {
			(void) fprintf(stderr, "zfs_rebase: this rebase's "
			    "manifest names no base; nothing is missing\n");
			continue;
		}
		if (made_says(&s->rb, input_word(i)))
			(void) fprintf(stderr, "zfs_rebase: %s was given as a "
			    "dataset, and the snapshot the tool took of it is "
			    "not there any more\n", input_word(i));
		else
			(void) fprintf(stderr, "zfs_rebase: %s %s is gone, by "
			    "name and by guid\n", input_word(i),
			    rec_snap(&s->rb, i));
		if (i == ZI_BASE)
			(void) fprintf(stderr, "zfs_rebase: the base is not "
			    "read by a verify; nothing turns on it\n");
		else
			(void) fprintf(stderr, "zfs_rebase: every action that "
			    "reads %s is unchecked\n", input_word(i));
	}
}

/* One document classified and printed, and what its outcome is worth. */
static int
report_one(struct resume *s, const struct zr_parsed *m, const char *what)
{
	struct zr_verify_report rep;
	int rc = EXIT_INTERNAL;

	memset(&rep, 0, sizeof (rep));
	if (classify(s, m, &rep) != 0) {
		rc = vfail(s, EXIT_INTERNAL, "verify");
		goto out;
	}
	print_report(s, m, &rep, what);
	rc = found_drift(&rep) ? EXIT_INTERNAL : EXIT_CLEAN;
out:
	zr_verify_report_fini(&rep);
	return (rc);
}

int
zr_report(const struct zr_verb_opts *o)
{
	struct resume s;
	int code;

	memset(&s, 0, sizeof (s));
	s.report = 1;			/* the report is the whole verb */
	s.verbose = o->verbose;
	tag_make(s.tmptag, sizeof (s.tmptag), "zrv-");
	code = resume_open(&s, o, 1);
	if (code != EXIT_CLEAN)
		goto done;
	hold_for_report(&s);
	if (resume_trees(&s) != 0) {
		code = vfail(&s, EXIT_PRECOND, s.result);
		goto done;
	}
	explain_gone(&s);
	code = report_one(&s, &s.man, "the manifest");
	/*
	 * The resolution was classified with it, line by line, and
	 * what is left to say of it is how much is still unanswered,
	 * which is what says whether the rebase can move at all. It is
	 * a report and it writes nothing: an unreadable resolution is
	 * said and does not change the outcome of the check that was
	 * asked for, and no drift line is added here -- that is the
	 * conflicts gate's, and only under a --continue.
	 */
	if (s.hasres < 0)
		(void) fprintf(stderr, "zfs_rebase: %s\n", s.reserr);
	else if (s.hasres == 0)
		(void) fprintf(stderr, "zfs_rebase: the resolution %s is "
		    "gone\n", s.respath);
	else
		unanswered_note(&s, zr_resolution_unanswered(&s.res),
		    s.res.zs_nlines);
done:
	resume_close(&s);
	return (code);
}

/*
 * May the gate be handed the run's own three trees instead of
 * reading them again? Only where they are the same three trees, read
 * the same way, and where taking the result over will move no mount:
 * the walks hold descriptors inside that mount, and a take that
 * unmounted it would leave them pointing at a filesystem nobody is
 * standing in any more.
 *
 * from and onto are snapshots held under the run's tag and cannot
 * have changed; the result is read-only and unwritten since the
 * self-check walked it. Anything else -- a snapshot the record now
 * names differently, a tree the verb could not find, a result
 * somebody moved -- and the gate walks for itself, which is what it
 * always did (R13 of the code review).
 */
static int
lend_ok(struct resume *s, const struct run *r)
{
	char at[ZR_NAME_MAX];
	int rc;

	if ((r->walked & (ZR_W_FROM | ZR_W_ONTO | ZR_W_RESULT)) !=
	    (ZR_W_FROM | ZR_W_ONTO | ZR_W_RESULT) || r->names == NULL)
		return (0);
	if (s->report != 0 || s->post != 0 || s->miss != 0 ||
	    s->gone[ZI_FROM] != 0 || s->gone[ZI_ONTO] != 0)
		return (0);
	if (s->dataset != in_dataset_form(r))
		return (0);
	if (strcmp(s->found[ZI_FROM], r->fromsnap) != 0 ||
	    strcmp(s->found[ZI_ONTO], r->ontosnap) != 0)
		return (0);
	if (strcmp(s->workmnt, r->workmnt) != 0)
		return (0);
	rc = zr_zfs_mounted_at(s->zfs, s->result, at, sizeof (at), s->err,
	    sizeof (s->err));
	return (rc > 0 && strcmp(at, s->workmnt) == 0);
}

/*
 * resume_trees' sibling, for the one caller that has the trees
 * already: the result taken over exactly as there, and then the
 * run's three walks and its name table moved into this verb, which
 * owns them from here -- close_trees closes them before the settle
 * unmounts anything, as it does with walks of its own. The oracle is
 * built here rather than taken, so that the gate compares what it is
 * given and inherits no memo of comparisons somebody else made.
 */
static int
lend_trees(struct resume *s, struct run *r)
{
	/*
	 * take_over and not report_mount: lend_ok has refused every
	 * verb but the fresh run's own gate, which is no report and
	 * has the result at the private mount already, so this take
	 * moves no mount and the walks below stay good.
	 */
	if (take_over(s) != 0)
		return (-1);
	s->w[ZS_ONTO] = r->wo;
	s->w[ZS_FROM] = r->wf;
	s->w[ZS_RESULT] = r->wr;
	s->walked = (1 << ZS_ONTO) | (1 << ZS_FROM) | (1 << ZS_RESULT);
	s->names = r->names;
	r->walked &= ~(ZR_W_FROM | ZR_W_ONTO | ZR_W_RESULT);
	r->names = NULL;
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: the final check reads the "
		    "three trees this run walked\n");
	return (build_oracle(s));
}

/*
 * The fresh run's done gate, made by the verbs' own machinery over
 * the record the run has just written: the result walked again
 * beside from and onto, every action classified, and the release and
 * the clearing of the record only after that. It is the same
 * function a --continue reaches at its own done gate, so a run
 * killed before it and continued later makes exactly this check and
 * no other one, and the record is what both of them read --
 * zfs_rebase:quiet included, which the start latched there.
 *
 * What the run lends it are inputs and not answers: the same libzfs
 * handle, the same three trees, and then the same function over
 * them. Where anything about the rebase has moved, lend_ok says so
 * and the gate reads everything for itself.
 *
 * *settled says whether the gate was passed, which the caller cannot
 * read off the status: 3 is the check that found drift, and done is
 * reached all the same, as well as the check that could not be made,
 * where the rebase is left standing for a --continue.
 */
static int
final_verify(struct run *r, int *settled)
{
	struct zr_verb_opts o;
	struct resume s;
	int rc;

	memset(&o, 0, sizeof (o));
	o.verbose = r->o.verbose;
	memset(&s, 0, sizeof (s));
	s.verbose = r->o.verbose;
	s.zfs = r->zfs;
	s.zfslent = 1;
	rc = resume_open_result(&s, &o, r->rds, 0);
	if (rc == EXIT_CLEAN) {
		if (lend_ok(&s, r) != 0) {
			rc = lend_trees(&s, r) != 0 ?
			    vfail(&s, EXIT_INTERNAL, "verify") : done_gate(&s);
		} else {
			/*
			 * The gate reads the trees itself, so this
			 * run lets go of its own first: they are held
			 * open inside the mount the take may have to
			 * move.
			 */
			release_trees(r);
			rc = resume_trees(&s) != 0 ?
			    vfail(&s, EXIT_INTERNAL, "verify") : done_gate(&s);
		}
	}
	*settled = s.settled;
	resume_close(&s);
	return (rc);
}

/*
 * The dataset form's half of --abort: the dataset is the user's and
 * is put back rather than destroyed. This is the settle's first
 * step, and only the first: the holds, the record and the two
 * snapshots come after it, in the caller, in the order the settle's
 * comment sets out.
 *
 *	roll the dataset back to the pre-apply snapshot the header
 *	names, which is what "as if the run never happened" means
 *	when the run wrote into a dataset of the user's;
 *	give the dataset back: off the private mount, readonly and
 *	canmount as the header says they were, mounted where its
 *	mountpoint property says, and asked whether it is there.
 *
 * The rollback is made first and while the dataset is still at the
 * private mount, which lzc_rollback_to takes in its stride: the
 * kernel suspends and resumes the filesystem around it and no mount
 * moves (zfs_ioc_rollback, module/zfs/zfs_ioctl.c; zfs(8) unmounts
 * nothing for a rollback either). So the tree that appears at home
 * is the rolled-back one and never the half rebased one, and a
 * rollback that fails leaves the dataset where the run had it, which
 * is where a second --abort expects to find it.
 *
 * A rollback that cannot be made -- the snapshot gone, or a newer
 * snapshot in the way -- stops the abort with the record intact,
 * because the alternative is to forget a rebase that is still in the
 * tree; so does a hand-back that cannot be made, which is somebody
 * standing in the private mount. Returns 0, or -1 with the reason
 * already printed.
 */
static int
abort_dataset(struct zr_zfs *z, const char *result, const char *snap,
    const char *ro, const char *cm, const char *rundir, int verbose)
{
	char mnt[ZR_NAME_MAX];
	char err[512];
	int rc;

	if (snap == NULL || snap[0] == '\0' ||
	    strcmp(snap, ZR_NO_BASE) == 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: the manifest names no "
		    "pre-apply snapshot, so %s cannot be put back\n", result,
		    result);
		return (-1);
	}
	rc = zr_zfs_exists(z, snap, err, sizeof (err));
	if (rc < 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", snap, err);
		return (-1);
	}
	if (rc == 0) {
		(void) fprintf(stderr, "zfs_rebase: %s is gone, so %s cannot "
		    "be rolled back to what it was; the record is left as it "
		    "is\n", snap, result);
		return (-1);
	}
	if (zr_zfs_rollback(z, result, snap, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: roll %s back to %s: %s\n",
		    result, snap, err);
		return (-1);
	}
	if (verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is %s again\n", result,
		    snap);
	/*
	 * And back to service, which is the one hand-back --abort
	 * makes and the same one done makes. A kill can have left the
	 * dataset at the run's own mount point, at its own, or
	 * nowhere at all; the hand-back undoes only the first, and
	 * puts both properties back in every case. The record and the
	 * holds are still on it while this runs, so a mount somebody
	 * is standing in stops the abort with everything it needs to
	 * be run again.
	 */
	(void) snprintf(mnt, sizeof (mnt), "%s/mnt", rundir);
	if (handback(z, result, ro, cm, mnt, verbose) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s was not given back, so "
		    "it is still this run's: the record and the holds are "
		    "kept, and the next zfs_rebase --abort finishes it once "
		    "nothing is standing in %s\n", result, mnt);
		return (-1);
	}
	return (0);
}

/*
 * The private mount undone, which is the first step of an --abort in
 * the clone form as it is in the dataset form: the result comes off
 * the run's own mount point before anything else is given back. A
 * result mounted somewhere else, or nowhere, is left where it is --
 * only this mount is the run's -- and one somebody is standing in
 * stops the abort with the words the done gate uses, since it is the
 * same refusal leaving the same rebase behind. zr_zfs_destroy would
 * unmount it too, but a busy mount would come back as "destroy",
 * which is not what refused. Returns 0, or -1 with the reason
 * printed.
 */
static int
undo_private(struct zr_zfs *z, const char *result, const char *mnt,
    int verbose)
{
	char at[ZR_NAME_MAX], err[512];

	if (zr_zfs_mounted_at(z, result, at, sizeof (at), err,
	    sizeof (err)) <= 0 || strcmp(at, mnt) != 0)
		return (0);
	if (zr_zfs_unmount(z, result, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s will not unmount from "
		    "%s: %s\n", result, mnt, err);
		(void) fprintf(stderr, "zfs_rebase: %s was not given back, so "
		    "it is still this run's: the record and the holds are "
		    "kept, and the next zfs_rebase --abort finishes it once "
		    "nothing is standing in %s\n", result, mnt);
		return (-1);
	}
	if (verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is off the private "
		    "mount %s\n", result, mnt);
	return (0);
}

/*
 * --abort where the manifest is gone: the record's tag is the whole
 * of what is left, and the holds it names are the one thing that
 * must not be left behind. So they are given back by walking the
 * result's pool for the tag, the private mount is undone where the
 * result is at it, and the record is taken off, which frees the name
 * for another rebase.
 *
 * Since the birth manifest there is no gate at which a record exists
 * and its file does not, so this is for a file that was actually
 * lost -- removed by a hand, or unreadable -- and for nothing else.
 *
 * The result is not destroyed and nothing is rolled back. Which form
 * the run was in, and what the pre-apply snapshot was called, were
 * the manifest's to say, and a tool that guessed would be destroying
 * a dataset it cannot identify. The one snapshot the run took for
 * itself is the exception: the header's #made would have named it,
 * and without the header the hold under the record's tag and the
 * name the run gave it -- the tag again -- say the same thing, so
 * the walk that releases the tag brings its name back and it is
 * destroyed, as it would be with the manifest in hand. It lives as
 * long as the rebase, and this is the end of the rebase.
 *
 * What it can read is the one property the two forms do not share.
 * A clone of this tool's making has mountpoint none for the whole of
 * its life, so a result whose mountpoint is a path is a dataset of
 * the user's and is mounted there -- an explicit mount works whatever
 * canmount says, which the box probe shows (probe-mount.txt, 3e) --
 * while a result whose mountpoint is none has no home to be put at
 * and is left unmounted, which is what an unplaced clone is. Neither
 * readonly nor canmount can be put back: what they were was the
 * header's to say. It prints the two commands for that, the command
 * for each form, and leaves the choice to the person.
 *
 * The order is the settle's, as far as this can follow it: the
 * result off the private mount and back where its mountpoint
 * property names, then the tag released and the run's own snapshot
 * destroyed, then the record taken off. A private mount that will
 * not go stops it before any of that, with the record and the tag
 * where they were, and the next --abort tries again. Returns 0, or
 * -1 with the reason printed.
 */
static int
abort_lost(struct zr_zfs *z, const char *result, const char *tag,
    const char *rundir, int verbose)
{
	char pool[ZR_SNAP_MAX], at[ZR_NAME_MAX], mnt[ZR_NAME_MAX];
	char home[ZR_NAME_MAX], made[ZR_SNAP_MAX], own[ZR_SNAP_MAX];
	char err[512];
	unsigned n = 0;
	int rc;

	/*
	 * The private mount, undone where the result is at it, and
	 * then the mountpoint property, which is the one thing that
	 * still tells the two forms apart: a path is a dataset of the
	 * user's and is mounted at it, none is a clone of this tool's
	 * and stays unmounted. This comes first, and nothing that
	 * follows runs without it: the record is the only thing left
	 * that names this rebase, and it is the last to go.
	 */
	(void) snprintf(mnt, sizeof (mnt), "%s/mnt", rundir);
	rc = zr_zfs_mounted_at(z, result, at, sizeof (at), err, sizeof (err));
	if (rc > 0 && strcmp(at, mnt) == 0) {
		if (zr_zfs_unmount(z, result, err, sizeof (err)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: %s will not "
			    "unmount from %s: %s\n", result, mnt, err);
			(void) fprintf(stderr, "zfs_rebase: %s was not given "
			    "back, so it is still this run's: the record and "
			    "the holds are kept, and the next zfs_rebase "
			    "--abort finishes it once nothing is standing in "
			    "%s\n", result, mnt);
			return (-1);
		}
		rc = 0;
		if (verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is off the "
			    "private mount %s\n", result, mnt);
	}
	home[0] = '\0';
	if (zr_zfs_get(z, result, "mountpoint", home, sizeof (home), err,
	    sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: mountpoint on %s: %s\n",
		    result, err);
	if (home[0] != '\0' && strcmp(home, ZR_MOUNTPOINT_NONE) != 0 &&
	    rc == 0 && zr_zfs_mount(z, result, err, sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s will not mount at %s: "
		    "%s\n", result, home, err);
	/*
	 * And only then the holds, by walking the pool for the tag,
	 * and the snapshot the run took for itself, which that walk
	 * found held under the tag and named with it.
	 */
	(void) snprintf(pool, sizeof (pool), "%.*s",
	    (int)strcspn(result, "/"), result);
	(void) snprintf(made, sizeof (made), "%s%s", ZR_MADE_PREFIX, tag);
	if (zr_zfs_release_tag(z, pool, tag, made, own, sizeof (own), &n,
	    err, sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: release %s in %s: %s\n",
		    tag, pool, err);
	else
		(void) fprintf(stderr, "zfs_rebase: released %s on %u "
		    "snapshot%s of %s\n", tag, n, n == 1 ? "" : "s", pool);
	if (own[0] != '\0') {
		if (zr_zfs_destroy_snap(z, own, err, sizeof (err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    own, err);
		else
			(void) fprintf(stderr, "zfs_rebase: %s was this run's "
			    "own snapshot, held under %s and named with it, "
			    "and is destroyed\n", own, tag);
	}
	clear_record(z, result, verbose);
	(void) fprintf(stderr, "zfs_rebase: the manifest file is gone, and "
	    "with it the form, the pre-apply snapshot and the two properties "
	    "to put back: %s was not destroyed and nothing was rolled back\n",
	    result);
	if (home[0] != '\0' && strcmp(home, ZR_MOUNTPOINT_NONE) != 0) {
		if (zr_zfs_mounted_at(z, result, at, sizeof (at), err,
		    sizeof (err)) > 0)
			(void) fprintf(stderr, "zfs_rebase: %s is mounted at "
			    "%s; its mountpoint property names %s\n", result,
			    at, home);
		else
			(void) fprintf(stderr, "zfs_rebase: %s is mounted "
			    "nowhere; its mountpoint property names %s\n",
			    result, home);
		(void) fprintf(stderr, "zfs_rebase: what readonly and "
		    "canmount were was the manifest's to say, so they are as "
		    "the run left them: zfs set readonly=VALUE %s and zfs set "
		    "canmount=VALUE %s put them back\n", result, result);
	} else {
		(void) fprintf(stderr, "zfs_rebase: %s has no mountpoint of "
		    "its own and is left unmounted; if it is a clone this "
		    "tool made it is yours to destroy or to place\n",
		    result);
	}
	(void) fprintf(stderr, "zfs_rebase: if %s is a clone this tool made: "
	    "zfs destroy %s\n", result, result);
	(void) fprintf(stderr, "zfs_rebase: if %s is a dataset of yours: zfs "
	    "rollback %s@PRE and zfs destroy %s@PRE, where PRE is the "
	    "pre-apply snapshot the run was started with\n", result, result,
	    result);
	return (0);
}

/*
 * --abort where there is no record to read: a run directory whose
 * result carries none, or whose result is not there at all (R2 and
 * R25 of the code review; documents-design.md, sections 11.1 and
 * 11.3). What the directory holds says which of three this is, and
 * the manifest in it is the only thing that can say what the rebase
 * was.
 *
 *	No manifest: the window before the birth write, the directory
 *	and its mnt and nothing else -- no hold, no record, nothing
 *	that says which rebase this was. The directory goes, and
 *	saying so is all there is to say.
 *
 *	A birth manifest, which is a header with no actions and no
 *	conflicts: the window between the birth write and the record.
 *	Nothing was written to the result and nothing was held. What
 *	goes with it is the snapshot the run took of from, which
 *	#made names, and -- because this window proves the rebase
 *	never got as far as a record -- the dataset form's pre-apply
 *	snapshot, which no done can have kept, since a done takes its
 *	run directory with it.
 *
 *	A decision manifest: a rebase whose result was destroyed
 *	under it (a --restart whose second clone failed, or a zfs
 *	destroy by hand), or a done whose unlink of the documents
 *	failed. Its holds are the thing that must not be left behind,
 *	and the header names the three snapshots and the tag they are
 *	filed under, so they are released here (R2). The pre-apply
 *	snapshot is named and left: this branch cannot tell a rebase
 *	that lost its result from a done, and done keeps that
 *	snapshot as the user's before-image.
 *
 * The release is made in both of the last two branches rather than
 * only the last, because it is one path and a birth manifest simply
 * has no holds to give back: a release of a tag that is not there is
 * not a failure (zr_zfs_release). The same goes for the #made
 * snapshot and for a document already unlinked, so this can be run
 * again over what a half-finished run of it left.
 *
 * A manifest that will not parse is the one thing this refuses: the
 * file says a rebase was here and nothing can be read out of it, so
 * nothing is released and nothing is removed, the parse's own reason
 * is printed, and the exit is 2 rather than clean. Returns
 * EXIT_CLEAN, or EXIT_PRECOND for that one.
 */
static int
abort_leftover(struct zr_zfs *z, const char *result, const char *dir,
    int verbose)
{
	char path[ZR_NAME_MAX], res[ZR_NAME_MAX], err[512];
	struct zr_parsed p;
	const char *snap, *tag;
	FILE *fp;
	int hasman = 0, parsed = 0, birth = 0, rmerr, i;

	memset(&p, 0, sizeof (p));
	(void) snprintf(path, sizeof (path), "%s/manifest", dir);
	fp = fopen(path, "r");
	if (fp != NULL) {
		hasman = 1;
		if (zr_manifest_parse(fp, &p, err, sizeof (err)) == 0)
			parsed = 1;
		else
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n", path,
			    err);
		(void) fclose(fp);
	}
	if (parsed != 0)
		birth = p.zp_actions_declared == 0 &&
		    p.zp_conflicts_declared == 0;
	if (hasman != 0 && parsed == 0) {
		(void) fprintf(stderr, "zfs_rebase: %s carries no record and "
		    "%s cannot be read, so what the run held and what it "
		    "took cannot be known: nothing was released and nothing "
		    "was removed\n", result, path);
		zr_parsed_fini(&p);
		return (EXIT_PRECOND);
	}
	if (hasman == 0) {
		(void) fprintf(stderr, "zfs_rebase: %s carries no record and "
		    "%s holds no manifest: what is left is the directory a "
		    "run made before it wrote anything\n", result, dir);
	} else if (birth != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s carries no record, and "
		    "%s is the header of a run that was killed before it "
		    "wrote one\n", result, path);
	} else {
		(void) fprintf(stderr, "zfs_rebase: %s carries no record, and "
		    "%s is the decision of a rebase whose result is not "
		    "there: what it left is the holds its header names\n",
		    result, path);
	}
	/*
	 * The holds, by the three names the header keeps and the tag
	 * it was filed under. A birth manifest has none of them taken
	 * yet and a done gave them back already, and neither is a
	 * failure here: this is the one path both take.
	 */
	tag = parsed != 0 && p.zp_tag != NULL ? p.zp_tag : NULL;
	for (i = 0; tag != NULL && i < 3; i++) {
		const char *nm = i == ZI_BASE ? p.zp_base :
		    (i == ZI_FROM ? p.zp_from : p.zp_onto);

		if (nm == NULL || nm[0] == '\0' || no_base(nm) ||
		    zr_zfs_exists(z, nm, err, sizeof (err)) <= 0)
			continue;
		if (zr_zfs_release(z, nm, tag, err, sizeof (err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: release %s on %s: "
			    "%s\n", tag, nm, err);
		else if (verbose)
			(void) fprintf(stderr, "zfs_rebase: released %s on "
			    "%s\n", tag, nm);
	}
	/*
	 * The snapshot the run took of from, which belongs to the
	 * rebase and goes with it, after its hold above. One that is
	 * already gone is not a failure.
	 */
	snap = p.zp_made != NULL && strcmp(p.zp_made, "from") == 0 ?
	    p.zp_from : NULL;
	if (snap != NULL && zr_zfs_exists(z, snap, err, sizeof (err)) > 0) {
		if (zr_zfs_destroy_snap(z, snap, err, sizeof (err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    snap, err);
		else
			(void) fprintf(stderr, "zfs_rebase: destroyed %s, "
			    "which the tool took itself\n", snap);
	}
	/*
	 * And the dataset form's pre-apply snapshot, which only the
	 * birth branch may destroy: there the manifest itself proves
	 * the run never reached its record, and a done cannot be the
	 * explanation because a done takes its run directory away.
	 * Under a decision it is named and left, since a rebase that
	 * lost its result and a done whose unlink failed have the
	 * same shape and one of them means to keep it.
	 */
	if (p.zp_form == ZR_HFORM_DATASET &&
	    p.zp_presnap != NULL && strcmp(p.zp_presnap, ZR_NO_BASE) != 0 &&
	    zr_zfs_exists(z, p.zp_presnap, err, sizeof (err)) > 0) {
		if (birth == 0)
			(void) fprintf(stderr, "zfs_rebase: %s is the "
			    "pre-apply snapshot that run took and is yours: "
			    "zfs destroy %s takes it away\n", p.zp_presnap,
			    p.zp_presnap);
		else if (zr_zfs_destroy_snap(z, p.zp_presnap, err,
		    sizeof (err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    p.zp_presnap, err);
		else
			(void) fprintf(stderr, "zfs_rebase: destroyed %s, the "
			    "pre-apply snapshot of a run killed before its "
			    "record\n", p.zp_presnap);
	}
	if (hasman != 0) {
		unlink_doc(path);
		resolution_path(res, sizeof (res), result);
		unlink_doc(res);
	}
	rmerr = rmdir_run(result);
	if (rmerr == 0)
		(void) fprintf(stderr, "zfs_rebase: removed %s\n", dir);
	else
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", dir,
		    strerror(rmerr));
	if (verbose)
		(void) fprintf(stderr, "zfs_rebase: %s was no rebase this "
		    "verb could move; nothing was rolled back and nothing of "
		    "the user's was destroyed\n", result);
	zr_parsed_fini(&p);
	return (EXIT_CLEAN);
}

/*
 * --abort: take one rebase away and nothing else. "As if the run
 * never happened", in the settle's order (documents-design.md,
 * section 11.3, and the comment over the settle above): the result
 * is put back first -- the clone destroyed, or the dataset rolled
 * back to its pre-apply snapshot and mounted where it belongs again
 * with both properties as the header kept them -- and only then are
 * the holds released, the two snapshots the rebase owned destroyed,
 * the record taken off, the two documents the run wrote into its own
 * directory unlinked and the run directory removed. A -o manifest
 * and the resolution beside it are the user's and stay: the tool
 * removes no file outside the run directory (documents-design.md,
 * section 4).
 *
 * Nothing after a step that refuses runs. A rollback that cannot be
 * made and a private mount somebody is standing in both stop the
 * abort with the record, the holds and the documents exactly as they
 * were, which is what a second --abort needs to try again; the
 * alternative is a dataset off its mountpoint that nothing names.
 *
 * The record is the key, and the refusal is the point of it. A
 * dataset that does not carry both zfs_rebase:manifest and
 * zfs_rebase:tag locally is not a zfs_rebase result and is left
 * alone, so a mistyped or a remembered-wrong name cannot cost the
 * user a dataset of their own, and neither can an inherited value:
 * a user property set on a parent shows up on every dataset beneath
 * it, and zr_zfs_get_user answers for the local value only. Every
 * phase is fair game, applying1 included, because a process killed
 * part way through the apply leaves exactly that and this is what
 * clears it. A rebase that reached done left no record at all and is
 * not this verb's to undo.
 *
 * What the record buys is the manifest, and the manifest's header is
 * everything else: the form, the three snapshots to release, which
 * of them the tool made, and the pre-apply snapshot and the two
 * property values the dataset form puts back. The file is there at
 * every gate, since the run writes the header before the record
 * (documents-design.md, section 11.1), so every --abort of a rebase
 * that has one has all of that; where the file is gone or will not
 * parse, abort_lost above is as far as this can go.
 *
 * A result with no record and a run directory of its own, and a
 * result that is not there at all with a run directory still
 * standing, both go to abort_leftover, which reads what the
 * directory holds and gives back what that names (R2 and R25). The
 * identifier's fifth step is that same directory, so a name that
 * resolves to nothing else reaches it here rather than "no such
 * run" (documents-design.md, section 11.4).
 *
 * It can be run again. A release of a tag that is not there, or of
 * a snapshot that is not there, is not a failure; a rollback to the
 * snapshot a dataset already sits at is not one; a document already
 * unlinked is not one either; and a run whose dataset is gone but
 * whose directory is not is finished by abort_leftover, which
 * releases what the header there names. Only when there is nothing
 * at all left does --abort say "no such run".
 * Nothing is removed recursively: the only files this unlinks are
 * the two the run wrote into its own directory, and every directory
 * goes by rmdir, which will not touch one that is not empty.
 */
int
zr_abort(const struct zr_verb_opts *o)
{
	char manifest[ZR_NAME_MAX], resolution[ZR_NAME_MAX];
	char result[ZR_NAME_MAX], given[ZR_NAME_MAX], hds[ZR_NAME_MAX];
	char dir[ZR_NAME_MAX], phase[64], tag[ZR_TAG_MAX], err[512];
	const int verbose = o->verbose;
	const char *snap;
	struct zr_ident id;
	struct zr_parsed p;
	struct zr_zfs *z = NULL;
	struct stat sb;
	FILE *fp;
	int rc = EXIT_INTERNAL, hasdir, hasds, got, parsed = 0, i, rmerr;

	if (geteuid() != 0) {
		(void) fprintf(stderr, "zfs_rebase: must run as root\n");
		return (EXIT_PRECOND);
	}
	memset(&p, 0, sizeof (p));
	if (zr_zfs_open(&z, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: libzfs: %s\n", err);
		return (EXIT_PRECOND);
	}
	/*
	 * Which rebase this is: the identifier, resolved as it is for
	 * the verbs that go through resume_open (documents-design.md,
	 * section 11.4). Its fifth step is the run directory with no
	 * record on any dataset, which is this verb's alone: --abort
	 * keeps its own path from here because it has to work where
	 * there is no record and no manifest left to read at all.
	 */
	if (resolve_ident(z, o->ident, &id) != 0) {
		rc = EXIT_PRECOND;
		goto done;
	}
	(void) snprintf(result, sizeof (result), "%s", id.zi_result);
	(void) snprintf(given, sizeof (given), "%s", id.zi_path);
	/*
	 * And the run directory, by the one rule that builds one: the
	 * name held against ZFS's own before it is a path, and WORKDIR
	 * resolved once (R19). --abort is the verb that goes on where
	 * the dataset does not exist, so a name the resolution took
	 * from a header must not reach rmdir_run unheld.
	 */
	if (rundir_of(dir, sizeof (dir), result, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s\n", err);
		rc = EXIT_PRECOND;
		goto done;
	}
	hasdir = stat(dir, &sb) == 0;
	hasds = zr_zfs_exists(z, result, err, sizeof (err));
	if (hasds < 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", result, err);
		rc = EXIT_PRECOND;
		goto done;
	}
	if (hasds == 0 && !hasdir) {
		(void) fprintf(stderr, "zfs_rebase: %s: no such run\n",
		    result);
		rc = EXIT_PRECOND;
		goto done;
	}
	if (hasds == 0) {
		/*
		 * The result is not there and its directory is: no
		 * record can be read off a dataset that does not
		 * exist, so what is left is whatever the directory
		 * holds. In the clone form that is the window before
		 * the record, where the clone had not been created
		 * yet and the birth manifest names what the run was
		 * going to be.
		 */
		rc = abort_leftover(z, result, dir, verbose);
		goto done;
	}
	got = zr_zfs_get_user(z, result, ZR_PROP_MANIFEST, manifest,
	    sizeof (manifest), err, sizeof (err));
	if (got < 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
		    ZR_PROP_MANIFEST, err);
		rc = EXIT_PRECOND;
		goto done;
	}
	if (got > 0) {
		got = zr_zfs_get_user(z, result, ZR_PROP_TAG, tag,
		    sizeof (tag), err, sizeof (err));
		if (got < 0) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
			    ZR_PROP_TAG, err);
			rc = EXIT_PRECOND;
			goto done;
		}
	}
	if (got == 0) {
		/*
		 * No record. With a run directory of its own
		 * that is the window before the record, which
		 * is a rebase's leavings and goes; without one
		 * the dataset is nobody's rebase and nothing
		 * here touches it.
		 */
		if (hasdir) {
			rc = abort_leftover(z, result, dir, verbose);
			goto done;
		}
		(void) fprintf(stderr, "zfs_rebase: %s is not a "
		    "zfs_rebase result; nothing was touched\n",
		    result);
		rc = EXIT_PRECOND;
		goto done;
	}
	/*
	 * And the other half of the cross-check: the header named this
	 * dataset, so this dataset's record has to name that file back.
	 */
	if (given[0] != '\0' && strcmp(given, manifest) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s carries the "
		    "manifest %s and the manifest given is %s: they "
		    "are two rebases\n", result, manifest, given);
		rc = EXIT_PRECOND;
		goto done;
	}
	if (verbose) {
		if (zr_zfs_get_user(z, result, ZR_PROP_PHASE, phase,
		    sizeof (phase), err, sizeof (err)) > 0)
			(void) fprintf(stderr, "zfs_rebase: %s is at "
			    "%s, held under %s\n", result, phase, tag);
		else
			(void) fprintf(stderr, "zfs_rebase: %s has no "
			    "gate yet, held under %s\n", result, tag);
	}
	/*
	 * The manifest, which is the rest of the record. A file that is
	 * gone or that will not parse leaves the tag and nothing else,
	 * and abort_lost does what can be done with that.
	 */
	if (id.zi_parsed != 0 && strcmp(id.zi_path, manifest) == 0) {
		/*
		 * The resolution parsed this very file to find this
		 * rebase; the parse moves here rather than being made
		 * again (R12).
		 */
		p = id.zi_man;
		memset(&id.zi_man, 0, sizeof (id.zi_man));
		id.zi_parsed = 0;
		parsed = 1;
	} else if ((fp = fopen(manifest, "r")) == NULL) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
		    manifest, strerror(errno));
	} else {
		if (zr_manifest_parse(fp, &p, err, sizeof (err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
			    manifest, err);
		else
			parsed = 1;
		(void) fclose(fp);
	}
	/*
	 * And the cross-check of the two halves, which read_manifest
	 * makes for every other verb (R4): the file this record names
	 * has to name this result back. A header that describes another
	 * rebase would have this abort release that run's snapshots and
	 * put back its properties, so it is refused; moving the file
	 * aside leaves the tag, which is what abort_lost works from.
	 */
	if (parsed != 0 && (zr_run_dataset(&p, hds, sizeof (hds), err,
	    sizeof (err)) != 0 || strcmp(hds, result) != 0)) {
		(void) fprintf(stderr, "zfs_rebase: %s is the manifest of %s "
		    "and %s carries it as its own: they are two rebases; move "
		    "that file aside to abort %s with its tag alone\n",
		    manifest, hds[0] != '\0' ? hds : err, result, result);
		rc = EXIT_PRECOND;
		goto done;
	}
	if (parsed == 0) {
		/*
		 * The two sides are checked against the
		 * header, and there is no header: a check
		 * that was asked for and cannot be made stops
		 * the abort rather than passing silently.
		 */
		if (o->from != NULL || o->onto != NULL) {
			(void) fprintf(stderr, "zfs_rebase: %s cannot "
			    "be read, so --from and --onto cannot be "
			    "checked against it; give neither to "
			    "abort what is left\n", manifest);
			rc = EXIT_PRECOND;
			goto done;
		}
		rc = abort_lost(z, result, tag, dir, verbose) == 0 ?
		    EXIT_CLEAN : EXIT_INTERNAL;
		/*
		 * And the run directory, if the mount is undone
		 * and nothing is left in it: an unreadable
		 * manifest is not this tool's to unlink, and
		 * rmdir will not take a directory that still
		 * holds one.
		 */
		if (rc == EXIT_CLEAN && hasdir) {
			(void) rmdir_run(result);
			if (stat(dir, &sb) != 0)
				(void) fprintf(stderr, "zfs_rebase: "
				    "removed %s\n", dir);
		}
		goto done;
	}
	/*
	 * The two sides as the command named them, against the header,
	 * before one thing is undone: a verb that was told which rebase
	 * this is and disagrees with the header acts on nothing.
	 */
	if (given_input(z, o->from, p.zp_from, p.zp_from_guid,
	    ZI_FROM, err, sizeof (err)) != 0 ||
	    given_input(z, o->onto, p.zp_onto, p.zp_onto_guid,
	    ZI_ONTO, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s\n", err);
		rc = EXIT_PRECOND;
		goto done;
	}
	/*
	 * The result itself, first of everything, which the form
	 * decides: a clone of the tool's own is destroyed, and a
	 * dataset of the user's is rolled back to the pre-apply
	 * snapshot and put back into service. The record and the holds
	 * are still on it while that runs, so a step of it that refuses
	 * -- the rollback, or an unmount somebody is standing in --
	 * leaves a rebase a second --abort can find and finish (the
	 * settle's comment above; R3 of the code review).
	 *
	 * The clone form takes the private mount off first and by the
	 * same call the rest of the settle uses, so that a mount
	 * somebody is standing in refuses as an unmount and not as a
	 * destroy; the clone then carries its record away with it.
	 */
	if (p.zp_form == ZR_HFORM_DATASET) {
		if (abort_dataset(z, result, p.zp_presnap,
		    p.zp_readonly, p.zp_canmount, dir,
		    verbose) != 0)
			goto done;
	} else {
		char mnt[ZR_NAME_MAX];

		(void) snprintf(mnt, sizeof (mnt), "%s/mnt", dir);
		if (undo_private(z, result, mnt, verbose) != 0)
			goto done;
		if (zr_zfs_destroy(z, result, err,
		    sizeof (err)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: destroy "
			    "%s: %s\n", result, err);
			goto done;
		}
		(void) fprintf(stderr, "zfs_rebase: destroyed %s\n",
		    result);
	}
	/*
	 * And only then the holds, by the three names the header keeps.
	 * One it does not name, or one gone from the pool, is nothing
	 * to release; a release that fails for any other reason stops
	 * the abort with the record intact, so that running it again
	 * can try the same thing. In the dataset form #onto is the
	 * pre-apply snapshot, so this is also what frees that snapshot
	 * to be destroyed below.
	 */
	for (i = 0; i < 3; i++) {
		const char *nm = i == ZI_BASE ? p.zp_base :
		    (i == ZI_FROM ? p.zp_from : p.zp_onto);
		int ex;

		if (nm == NULL || nm[0] == '\0' || no_base(nm)) {
			if (verbose)
				(void) fprintf(stderr, "zfs_rebase: "
				    "%s: the manifest names no %s\n",
				    manifest, input_word(i));
			continue;
		}
		ex = zr_zfs_exists(z, nm, err, sizeof (err));
		if (ex == 0) {
			if (verbose)
				(void) fprintf(stderr, "zfs_rebase: "
				    "%s is gone; nothing to release\n",
				    nm);
			continue;
		}
		if (ex < 0 || zr_zfs_release(z, nm, tag, err,
		    sizeof (err)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: release "
			    "%s on %s: %s\n", tag, nm, err);
			goto done;
		}
		if (verbose)
			(void) fprintf(stderr, "zfs_rebase: released "
			    "%s on %s\n", tag, nm);
	}
	/*
	 * The two snapshots the rebase owned, after the holds that were
	 * on them: the pre-apply snapshot, which the dataset form has
	 * just been rolled back to and which the rebase owned from the
	 * moment --result named it, and the snapshot the tool took of a
	 * side given as a dataset, which #made is what says there was.
	 */
	if (p.zp_form == ZR_HFORM_DATASET && p.zp_presnap != NULL &&
	    p.zp_presnap[0] != '\0' &&
	    strcmp(p.zp_presnap, ZR_NO_BASE) != 0 &&
	    zr_zfs_destroy_snap(z, p.zp_presnap, err, sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
		    p.zp_presnap, err);
	snap = p.zp_made != NULL && strcmp(p.zp_made, "from") == 0 ?
	    p.zp_from : NULL;
	if (snap != NULL) {
		if (zr_zfs_destroy_snap(z, snap, err,
		    sizeof (err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy "
			    "%s: %s\n", snap, err);
		else
			(void) fprintf(stderr, "zfs_rebase: destroyed "
			    "%s, which the tool took itself\n", snap);
	}
	/*
	 * And the record, last of what is on the result, because the
	 * tag in it is the only handle on the holds above and the path
	 * in it is the only handle on the documents below. The clone
	 * form has none to take off: the record went with the clone.
	 */
	if (p.zp_form == ZR_HFORM_DATASET) {
		clear_record(z, result, verbose);
		(void) fprintf(stderr, "zfs_rebase: %s is as it was "
		    "before the rebase\n", result);
	}
	/*
	 * And the two documents, but only the two this run wrote into
	 * its own directory. Where -o named the manifest, that file and
	 * the resolution beside it are the user's, here exactly as at
	 * done, and stay where they were asked for: an --abort takes
	 * the rebase away and not the record of what it was.
	 */
	if (in_rundir(result, manifest)) {
		if (unlink(manifest) == 0)
			(void) fprintf(stderr, "zfs_rebase: removed "
			    "the manifest %s\n", manifest);
		else if (errno != ENOENT)
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
			    manifest, strerror(errno));
		/* And the resolution the run wrote beside it. */
		resolution_of(resolution, sizeof (resolution), result,
		    manifest);
		if (unlink(resolution) == 0)
			(void) fprintf(stderr, "zfs_rebase: removed "
			    "the resolution %s\n", resolution);
		else if (errno != ENOENT)
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
			    resolution, strerror(errno));
	} else {
		(void) fprintf(stderr, "zfs_rebase: the manifest %s "
		    "and the resolution beside it are yours and "
		    "stay\n", manifest);
	}
	if (hasdir) {
		rmerr = rmdir_run(result);
		if (rmerr == 0)
			(void) fprintf(stderr, "zfs_rebase: removed %s\n",
			    dir);
		else
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n", dir,
			    strerror(rmerr));
	}
	rc = EXIT_CLEAN;
done:
	zr_ident_fini(&id);
	zr_parsed_fini(&p);
	zr_zfs_close(z);
	return (rc);
}
