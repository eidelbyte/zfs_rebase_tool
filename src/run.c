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
 * dataset, cloned from that snapshot read-only at the run's own
 * mountpoint, and the record lives on the clone. Nothing of the
 * user's is written to at all.
 *
 * The dataset form, onto given as a dataset. --result names the
 * pre-apply snapshot the tool takes of it -- the short name after
 * the '@', or a full name whose dataset part is onto -- and the
 * rebase is made in that dataset itself, so the record lives on it.
 * Exclusivity is the unmount: the dataset is unmounted from its own
 * mountpoint and mounted at the run's private directory instead,
 * with its mountpoint property untouched, and it goes back there
 * whenever the run stops. Nothing is ever forced; a dataset somebody
 * is using is refused. The pre-apply snapshot is the user's own
 * before-image: it stays after done and only --abort takes it away,
 * rolling the dataset back to it first.
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
 * a process's leavings is the record: the user properties the result
 * carries -- the three snapshots and their guids, which of them the
 * tool made, the mode, the form, the hold tag, whether a final
 * verify was asked for, the manifest's path, and in the dataset form
 * the readonly value to give back -- and the three persistent holds,
 * one per input snapshot, filed under that tag with no cleanup
 * descriptor. While they are there zfs destroy refuses the snapshots
 * with "dataset is busy" and zfs holds shows the tag; a stranded
 * rebase holds on purpose, because it is continuable. The record is
 * read as local values only: user properties inherit down the naming
 * tree, and an inherited value is not ours (zfsops.c). In the clone
 * form the create writes it, so it is there from the clone's first
 * instant; in the dataset form it is set on the dataset before
 * anything else is touched.
 *
 * So a hard kill leaves the result, the manifest file and the three
 * holds, and
 *
 *	zfs_rebase --abort --result NAME
 *
 * releases the holds and takes the rest away -- destroying the clone
 * in one form, rolling the dataset back to its pre-apply snapshot
 * and destroying that in the other. While a clone lives, onto's
 * snapshot cannot be destroyed either; that is ZFS's own rule about
 * a clone's origin and not something this tool arranges.
 *
 * The state is written at the gates the run passes, and nowhere
 * else, so that what a kill leaves is the last gate reached:
 *
 *	applying1 -> conflicts -> applying2 -> done
 *	applying1 -> done			(no conflicts)
 *
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
 * --continue, which is the human input the move needs. "done" is
 * written after the re-walk verified and readonly is back on -- and
 * before the holds are released, so that a kill in between leaves a
 * done record whose holds --abort still finds.
 *
 * Turning the choices into actions is apply-choices' work, which
 * follows this issue; until it lands every choice is treated as
 * keep, so applying2 writes nothing at all.
 *
 * At birth there is no state at all, and a stop writes none: what a
 * stop leaves is the gate it was working under, and --continue
 * resumes from exactly that.
 *
 * The verbs further down this file work on a rebase that is already
 * there, and read the record and nothing else: --continue takes it
 * on from the gate its record names and, with --verify, repairs the
 * drift it finds on the way; --restart puts the result back as onto
 * was -- by destroying the clone and making it again, or by rolling
 * the dataset back to its pre-apply snapshot -- before doing the
 * same; --verify alone only reports; --abort takes the whole thing
 * away. None of them decides anything: the manifest the record names
 * is the decision, and it is made once. Each of them takes the
 * dataset form's dataset over the same way the run did and hands it
 * back when it stops.
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
 * or FILE.resolution beside a -o FILE -- and the record names it, so
 * that every verb finds it the way it finds the manifest and never by
 * guessing a path.
 */
#define	ZR_STATE_APPLYING1	"applying1"
#define	ZR_STATE_CONFLICTS	"conflicts"
#define	ZR_STATE_APPLYING2	"applying2"
#define	ZR_STATE_DONE		"done"
#define	ZR_RESOLUTION		"resolution"

/*
 * The two forms, as zfs_rebase:form records them and as every verb
 * reads them back. A record with no form at all is a clone-form
 * record written before the dataset form existed.
 */
#define	ZR_FORM_CLONE		"clone"
#define	ZR_FORM_DATASET		"dataset"

/*
 * What the record and the manifest header carry for the base of a
 * run that had none -- --allow-unrelated without --base, which reads
 * the two sides against the empty tree. "-" is how zfs(8) itself
 * spells a property with no value, and it is no snapshot name, since
 * every one of those has a pool and an '@' in it. An empty value
 * would say the same thing and cannot be written: the manifest's
 * header lines are trimmed of trailing blanks before they are read,
 * so a "#base " with nothing after it comes back as no header line
 * at all (manifest.c, zp_header).
 */
#define	ZR_NO_BASE		"-"

/*
 * What --take-onto and --take-from write into the record, and what a
 * run given neither writes: the word says which choice the skeleton
 * was written with, so that --restart can write the same document
 * again rather than an unanswered one. A record made before the
 * property existed has none, which reads as ZR_TAKE_NONE.
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
	int			dirmade, cloned, walked;
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
	int			replacing;	/* --overwrite took a record */
	int			madefrom;	/* the tool took from's snap */
	int			madeonto;	/* and onto's: a dry run */
	int			presnap;	/* the pre-apply snapshot is */
	int			privmnt;	/* onto is at workmnt */
	int			dropfrom;	/* done: the made snap goes */
	int			nheld;		/* holds taken, base first */
	uint32_t		unanswered;	/* lines of the skeleton */
	struct zr_names		*names;
	struct zr_walk		wb, wf, wo;
	struct zr_oracle	*oracle;
	struct zr_decision	d;
	char			err[512];
};

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
 *	conflicts	that gate is written, before the hand-back
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
	    "--result %s resumes it; zfs_rebase --abort --result %s removes "
	    "it\n", r->rds, r->rds, r->rds);
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
 * Without --base there is no base snapshot at all: the base is the
 * empty tree, every name of either side is an add on that side, and
 * the decision is the union of the two with a conflict wherever they
 * disagree.
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
		r->base[0] = '\0';
		if (r->o.verbose)
			(void) fprintf(stderr, "zfs_rebase: there is no "
			    "base: the two sides are read against the empty "
			    "tree\n");
		return (0);
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
 * must be mounted, and they must agree on names. Under
 * --allow-unrelated there may be no base dataset to check at all,
 * and then it is the two sides alone that are looked at.
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
	int i, p, first;

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
	/*
	 * The three datasets, or the two of them a run with no base
	 * has: first is where the checks start, and the base is what
	 * it leaves out.
	 */
	dataset_of(r->base, ds[0], sizeof (ds[0]));
	dataset_of(r->fromsnap, ds[1], sizeof (ds[1]));
	dataset_of(r->ontosnap, ds[2], sizeof (ds[2]));
	first = r->base[0] != '\0' ? 0 : 1;
	for (i = first; i < 3; i++) {
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
	for (i = first + 1; i < 3; i++) {
		size_t a = strcspn(ds[first], "/"), b = strcspn(ds[i], "/");

		if (a != b || strncmp(ds[first], ds[i], a) != 0) {
			(void) snprintf(r->err, sizeof (r->err),
			    "%s and %s are not in one pool", ds[first],
			    ds[i]);
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
	if (r->base[0] != '\0' &&
	    zr_zfs_get(r->zfs, ds[0], "mountpoint", r->basemnt,
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
 * A record whose state is done is a rebase that finished, and
 * --overwrite replaces it: the properties are set again and the
 * state goes, so the new run starts at no gate. A record in any
 * other state is an open rebase -- there is no state at all until
 * the first gate, so a run killed before applying1 is one too -- and
 * no flag overrides that: --continue or --abort settles it first.
 */
static int
onto_open(struct run *r)
{
	char state[64], tag[ZR_TAG_MAX];
	uint64_t v;
	int got;

	got = zr_zfs_exists(r->zfs, r->ontods, r->err, sizeof (r->err));
	if (got < 0)
		return (-1);
	if (got == 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s does not exist",
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
	if (zr_zfs_get(r->zfs, r->ontods, "readonly", r->roorig,
	    sizeof (r->roorig), r->err, sizeof (r->err)) != 0)
		return (-1);
	got = zr_zfs_get_user(r->zfs, r->ontods, ZR_PROP_TAG, tag,
	    sizeof (tag), r->err, sizeof (r->err));
	if (got < 0)
		return (-1);
	if (got == 0)
		return (0);		/* no record: a fresh dataset form */
	state[0] = '\0';
	if (zr_zfs_get_user(r->zfs, r->ontods, ZR_PROP_STATE, state,
	    sizeof (state), r->err, sizeof (r->err)) < 0)
		return (-1);
	if (strcmp(state, ZR_STATE_DONE) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s carries a rebase "
		    "at \"%s\" under %s; a rebase is open here: --continue or "
		    "--abort it first", r->ontods,
		    state[0] != '\0' ? state : "no gate yet", tag);
		return (-1);
	}
	/*
	 * A record that reached done is replaced only when the user
	 * asks for it. A dry run is let past: it replaces nothing,
	 * creates nothing and holds nothing, and its whole output is
	 * the manifest it prints.
	 */
	if (r->o.dryrun)
		return (0);
	if (!r->o.overwrite) {
		(void) snprintf(r->err, sizeof (r->err), "%s carries a rebase "
		    "that reached done under %s; --overwrite replaces it",
		    r->ontods, tag);
		return (-1);
	}
	r->replacing = 1;
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: --overwrite: the record "
		    "of the rebase %s carries under %s is replaced\n",
		    r->ontods, tag);
	return (0);
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
 * WORKDIR/<result>, root-only, so nothing else can look in. Every
 * parent may exist; the leaf may not, because a leaf that is there
 * is another run of this result.
 */
static int
make_rundir(struct run *r)
{
	char parent[ZR_NAME_MAX];
	char *slash;

	if ((size_t)snprintf(r->rundir, sizeof (r->rundir), "%s/%s", WORKDIR,
	    r->rds) >= sizeof (r->rundir)) {
		(void) snprintf(r->err, sizeof (r->err), "%s/%s: %s", WORKDIR,
		    r->rds, strerror(ENAMETOOLONG));
		return (-1);
	}
	(void) snprintf(parent, sizeof (parent), "%s", r->rundir);
	slash = strrchr(parent, '/');
	*slash = '\0';
	if (mkdir_p(parent, r->err, sizeof (r->err)) != 0)
		return (-1);
	if (mkdir(r->rundir, 0700) != 0) {
		if (errno == EEXIST)
			(void) snprintf(r->err, sizeof (r->err),
			    "a run for %s is in place (%s); zfs_rebase "
			    "--abort --result %s removes it", r->rds,
			    r->rundir, r->rds);
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
 */
static void
rmdir_run(const char *result)
{
	size_t top = sizeof (WORKDIR) - 1;
	char dir[ZR_NAME_MAX];
	char *slash;

	if ((size_t)snprintf(dir, sizeof (dir), "%s/%s/mnt", WORKDIR,
	    result) >= sizeof (dir))
		return;
	(void) rmdir(dir);
	slash = strrchr(dir, '/');
	*slash = '\0';
	while (rmdir(dir) == 0) {
		slash = strrchr(dir, '/');
		if (slash == NULL || (size_t)(slash - dir) <= top)
			break;
		*slash = '\0';
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
	(void) snprintf(buf, len, "%s/%s/%s", WORKDIR, result, ZR_RESOLUTION);
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
 * Every property of the record, which is what --overwrite and
 * --abort take away again. The state is among them here, where the
 * writers keep it apart: what is being removed is the whole thing.
 */
static const char *zr_record_props[] = {
	ZR_PROP_BASE, ZR_PROP_BASE_GUID, ZR_PROP_FROM, ZR_PROP_FROM_GUID,
	ZR_PROP_ONTO, ZR_PROP_ONTO_GUID, ZR_PROP_MADE, ZR_PROP_MODE,
	ZR_PROP_FORM, ZR_PROP_TAG, ZR_PROP_VERIFY, ZR_PROP_TAKE,
	ZR_PROP_MANIFEST, ZR_PROP_RESOLUTION, ZR_PROP_READONLY,
	ZR_PROP_STATE
};

#define	ZR_NRECORD	(sizeof (zr_record_props) / sizeof (zr_record_props[0]))

/*
 * Take the record off a dataset the tool did not create, which is
 * the dataset form's own undoing: a clone carries its record away
 * with it when it is destroyed, and onto has to be left as it was
 * found. Inheriting a user property is how it is removed, and one
 * that is not there is not a failure, so this can be run again.
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
 * One document of the rebase before this one, when that rebase wrote
 * it in the run directory rather than where a -o put it. It has to
 * go: the directory is removed next, and rmdir will not take one
 * that still holds a file.
 */
static void
overwrite_file(struct run *r, const char *prop, const char *dir, size_t n,
    const char *what)
{
	char old[ZR_NAME_MAX], e[512];

	if (zr_zfs_get_user(r->zfs, r->rds, prop, old, sizeof (old), e,
	    sizeof (e)) > 0 && strncmp(old, dir, n) == 0 &&
	    unlink(old) == 0 && r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: removed %s, the %s of "
		    "the rebase before this one\n", old, what);
}

/*
 * What --overwrite takes away before the new run starts: the
 * finished rebase's two documents, when that rebase wrote them in the
 * run directory, and the run directory itself, which the new run
 * makes again. Every other property of the record is set again by the
 * new run; only the state has to go here, because the new run has
 * passed no gate and a leftover "done" would say it had.
 */
static void
overwrite_clear(struct run *r)
{
	char dir[ZR_NAME_MAX], e[512];
	size_t n;

	n = (size_t)snprintf(dir, sizeof (dir), "%s/%s/", WORKDIR, r->rds);
	if (n < sizeof (dir)) {
		overwrite_file(r, ZR_PROP_MANIFEST, dir, n, "manifest");
		overwrite_file(r, ZR_PROP_RESOLUTION, dir, n, "resolution");
	}
	if (zr_zfs_clear_user(r->zfs, r->rds, ZR_PROP_STATE, e,
	    sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s on %s: %s\n",
		    ZR_PROP_STATE, r->rds, e);
	rmdir_run(r->rds);
}

/*
 * The dataset form's exclusivity, and it is the unmount that proves
 * it: a dataset somebody has a file open in, or a working directory
 * in, or a child dataset mounted under, will not unmount, and that
 * is the refusal. Nothing is forced. What the tool then has is a
 * dataset mounted where only it can reach, with the mountpoint
 * property untouched, so that giving it back is one mount call.
 *
 * readonly goes on immediately after, which is the clone form's
 * flag exactly: the result is read-only except while a stage writes
 * to it. What the property was before this is in the record, and the
 * hand-back puts it back.
 */
static int
exclusive(struct run *r)
{
	char e[512];

	if (zr_zfs_unmount(r->zfs, r->ontods, r->err, sizeof (r->err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s is in use; unmount it "
		    "or give a snapshot\n", r->ontods);
		return (-1);
	}
	if (zr_zfs_mount_at(r->zfs, r->ontods, r->workmnt, r->err,
	    sizeof (r->err)) != 0) {
		/* it came from somewhere: put it back before giving up */
		if (zr_zfs_mount(r->zfs, r->ontods, e, sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: %s is unmounted "
			    "and will not mount: %s\n", r->ontods, e);
		return (-1);
	}
	r->privmnt = 1;
	if (zr_zfs_set_readonly(r->zfs, r->ontods, 1, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is this run's alone, "
		    "mounted at %s\n", r->ontods, r->workmnt);
	return (0);
}

/*
 * Give the dataset back to service: off the private mount, readonly
 * as the record says it was, and mounted where its own mountpoint
 * property says. This runs wherever the dataset form stops -- at
 * conflicts, at done, at a failure, at a signal -- because a rebase
 * that is waiting for an answer must not be holding a filesystem
 * hostage while it waits. A kill leaves it privately mounted, and
 * the next verb takes it from there.
 */
static void
handback(struct zr_zfs *z, const char *dataset, const char *ro, int verbose)
{
	char e[512];

	if (zr_zfs_unmount(z, dataset, e, sizeof (e)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s is still this run's: "
		    "%s\n", dataset, e);
		return;
	}
	if (ro != NULL && ro[0] != '\0' &&
	    zr_zfs_set_readonly(z, dataset, strcmp(ro, "on") == 0, e,
	    sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: readonly=%s on %s: %s\n",
		    ro, dataset, e);
	if (zr_zfs_mount(z, dataset, e, sizeof (e)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s will not mount: %s\n",
		    dataset, e);
		return;
	}
	if (verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is back where it "
		    "belongs\n", dataset);
}

static void
run_handback(struct run *r)
{
	char at[ZR_NAME_MAX], e[512];

	if (!r->privmnt)
		return;
	r->privmnt = 0;
	/*
	 * The final check a --verify run makes goes through the
	 * verbs' own machinery, which hands the dataset back itself
	 * when it is done, so this can arrive at a dataset that is
	 * already home. Handing it back twice would unmount it from
	 * its own place for nothing.
	 */
	if (zr_zfs_mounted_at(r->zfs, r->ontods, at, sizeof (at), e,
	    sizeof (e)) > 0 && strcmp(at, r->workmnt) != 0)
		return;
	handback(r->zfs, r->ontods, r->roorig, r->o.verbose);
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
 * The state is a gate the run has passed, not a step of it: a
 * failure to write one warns and the run goes on. The values are
 * "applying1", "conflicts", "applying2" and "done", and no others;
 * at birth there is none.
 */
static void
put_state(struct zr_zfs *z, const char *result, const char *state)
{
	char e[512];

	if (zr_zfs_set_user(z, result, ZR_PROP_STATE, state, e,
	    sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s=%s: %s\n",
		    ZR_PROP_STATE, state, e);
}

static void
set_state(struct run *r, const char *state)
{
	if (!r->recorded)
		return;
	put_state(r->zfs, r->rds, state);
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
 * The record the clone is created with. The guid of each input is
 * read from the snapshot itself (zfs_prop_get_int, ZFS_PROP_GUID)
 * and written as a decimal string, since a user property has no
 * other type; it is what finds a snapshot again after a rename or a
 * promote. The strings point into the run, which outlives the
 * create.
 */
static int
fill_record(struct run *r, struct zr_rebase_record *rec)
{
	/*
	 * A run with no base has no snapshot to read a guid off, and
	 * records ZR_NO_BASE and the guid 0 in their place: the pair
	 * every verb reads as "there was no base", which is not the
	 * same thing as a base that has gone missing.
	 */
	if (r->base[0] != '\0' &&
	    zr_zfs_get_int(r->zfs, r->base, "guid", &rec->base_guid, r->err,
	    sizeof (r->err)) != 0)
		return (-1);
	if (zr_zfs_get_int(r->zfs, r->fromsnap, "guid", &rec->from_guid,
	    r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get_int(r->zfs, r->ontosnap, "guid", &rec->onto_guid,
	    r->err, sizeof (r->err)) != 0)
		return (-1);
	rec->base = r->base[0] != '\0' ? r->base : ZR_NO_BASE;
	rec->from = r->fromsnap;
	rec->onto = r->ontosnap;
	/*
	 * made names the sides the tool snapshotted itself, which is
	 * from and never onto: the dataset form's pre-apply snapshot
	 * is the user's, named by them and kept after done.
	 */
	rec->made = r->madefrom ? "from" : "";
	rec->mode = r->o.mode == ZR_MODE_PERMISSIVE ? "permissive" : "strict";
	rec->form = r->form;
	rec->tag = r->tag;
	rec->verify = r->o.verify ? "yes" : "no";
	/*
	 * Which way the skeleton was answered when it was written,
	 * which is the run's own flag and nobody else's: --restart
	 * reads it back and writes the document the run wrote.
	 */
	rec->take = run_take(r);
	rec->manifest = r->manpath;
	rec->resolution = r->respath;
	/*
	 * The dataset form gives the dataset back as it found it, so
	 * what readonly was is part of the record; the clone form has
	 * nothing to put back and writes no such property.
	 */
	rec->readonly = in_dataset_form(r) ? r->roorig : NULL;
	return (0);
}

/*
 * The clone was created with the paths the run intended; now that the
 * file is there, resolve it and record what it really is, so that
 * --abort unlinks the file this run wrote whatever directory it was
 * started from.
 */
static void
record_path(struct run *r, const char *prop, char *path, size_t pathlen)
{
	char e[512];
	char *real;

	if (!r->recorded || path[0] == '\0')
		return;
	real = realpath(path, NULL);
	if (real == NULL) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", path,
		    strerror(errno));
		return;
	}
	if (zr_zfs_set_user(r->zfs, r->rds, prop, real, e, sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", prop, e);
	else
		(void) snprintf(path, pathlen, "%s", real);
	free(real);
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
	FILE *in, *out;
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
	out = fopen(r->respath, "w");
	if (out == NULL) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", r->respath,
		    strerror(errno));
		goto done;
	}
	if (zr_resolution_write(out, &res) != 0 || fclose(out) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s: write failed",
		    r->respath);
		goto done;
	}
	r->unanswered = zr_resolution_unanswered(&res);
	record_path(r, ZR_PROP_RESOLUTION, r->respath, sizeof (r->respath));
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
 * is the base of a run made with --allow-unrelated and no --base,
 * and it is what a verb puts in the place of a side it cannot read
 * (empty_walk, below). Returns 0, or -1 out of memory.
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
	 * The base, through its own .zfs/snapshot -- or the empty
	 * tree in its place, when --allow-unrelated was given no
	 * base: no third snapshot is read, every name of either side
	 * is an add on that side, and the decision is their union.
	 */
	if (r->base[0] != '\0') {
		snapdir(path, sizeof (path), r->basemnt, r->base);
		if (zr_walk(path, r->names, &r->wb, r->err,
		    sizeof (r->err)) != 0)
			return (-1);
	} else if (empty_tree(&r->wb, r->names) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "out of memory");
		return (-1);
	}
	r->walked = 1;
	if (stopped(r) != 0)
		return (-1);
	snapdir(path, sizeof (path), r->frommnt, r->fromsnap);
	if (zr_walk(path, r->names, &r->wf, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 2;
	if (stopped(r) != 0)
		return (-1);
	snapdir(path, sizeof (path), r->ontomnt, r->ontosnap);
	if (zr_walk(path, r->names, &r->wo, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 3;
	if (stopped(r) != 0)
		return (-1);

	if (zr_oracle_init(&r->oracle, &r->wb, &r->wf, &r->wo) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "out of memory");
		return (-1);
	}
	/*
	 * The unchanged set, off the three walks and nothing else: a
	 * pool of either side that base holds under the same object
	 * number, generation, ctime, link count, type and names is
	 * what base holds, and is never read (yellow.c). Only a
	 * derived base licenses that, so --allow-unrelated leaves
	 * r->prune clear and every pool is compared.
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
 * Above securelevel 0 the system immutable and append-only flags
 * cannot be cleared, so an object carrying them that the decision
 * would remove, rewrite or re-pool is refused before anything is
 * touched, naming the first such name. Only FreeBSD has the flags.
 */
static int
securelevel_guard(struct run *r)
{
#if defined(__FreeBSD__)
	int level = 0;
	size_t len = sizeof (level);
	const struct zr_tree *ot = &r->wo.zw_tree;
	uint32_t i, j;

	if (sysctlbyname("kern.securelevel", &level, &len, NULL, 0) != 0 ||
	    level <= 0)
		return (0);
	for (i = 0; i < ot->zt_npools; i++) {
		const struct zr_pool *q = &ot->zt_pools[i];
		const struct zr_attr *a = &r->wo.zw_attrs[i];
		int touched = 0;

		if ((a->za_flags & (SF_IMMUTABLE | SF_APPEND)) == 0)
			continue;
		for (j = 0; j < q->zp_nnames && !touched; j++) {
			zr_name_t n = q->zp_names[j];
			zr_pool_t k = r->d.zd_result_of[n];

			if (k == ZR_POOL_NONE)
				touched = 1;	/* removed */
			else if (r->d.zd_pools[k].zr_content != q->zp_content ||
			    r->d.zd_pools[k].zr_nnames != q->zp_nnames)
				touched = 1;	/* rewritten or re-pooled */
		}
		if (touched) {
			size_t l;

			(void) snprintf(r->err, sizeof (r->err),
			    "securelevel %d: %s carries schg or sappnd and "
			    "would change", level,
			    zr_names_str(r->names, q->zp_names[0], &l));
			return (-1);
		}
	}
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
	set_state(r, ZR_STATE_APPLYING1);
	if (zr_zfs_set_readonly(r->zfs, r->rds, 0, r->err,
	    sizeof (r->err)) != 0)
		goto done;
	zr_pause(ZR_STATE_APPLYING1);
	/*
	 * No report: what a fresh run applies to is onto's tree
	 * exactly -- a clone of the snapshot, or the dataset the
	 * snapshot was just taken of -- so every action of the
	 * manifest is still to be made and a classification could let
	 * none of them be left alone. --continue is where a report
	 * earns its keep.
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
		rc = zr_apply_check(&parsed, r->workmnt, r->names, &r->wo,
		    &r->wf, 0, 1, &rst, r->err, sizeof (r->err));
		if (rc == 0 && r->o.verbose)
			(void) fprintf(stderr, "zfs_rebase: put back %llu "
			    "restored, %llu removed, %llu relinked\n",
			    (unsigned long long)rst.zs_restored,
			    (unsigned long long)rst.zs_removed,
			    (unsigned long long)rst.zs_relinked);
	}
	if (zr_zfs_set_readonly(r->zfs, r->rds, 1, r->err,
	    sizeof (r->err)) != 0)
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
	if (r->walked >= 3)
		zr_walk_fini(&r->wo);
	if (r->walked >= 2)
		zr_walk_fini(&r->wf);
	if (r->walked >= 1)
		zr_walk_fini(&r->wb);
	r->walked = 0;
	if (r->names != NULL) {
		zr_names_destroy(r->names);
		r->names = NULL;
	}
}

static void
teardown(struct run *r, int keep)
{
	char e[512];

	zr_decision_fini(&r->d);
	if (r->oracle != NULL)
		zr_oracle_fini(r->oracle);
	if (r->walked >= 3)
		zr_walk_fini(&r->wo);
	if (r->walked >= 2)
		zr_walk_fini(&r->wf);
	if (r->walked >= 1)
		zr_walk_fini(&r->wb);
	if (r->names != NULL)
		zr_names_destroy(r->names);
	/*
	 * The dataset form gives the dataset back before anything
	 * else, kept run or not: the walks are closed by now, so the
	 * snapshots under its .zfs are idle and the unmount can have
	 * it. A run that is kept -- at conflicts, or after a failure
	 * -- keeps everything but this: a rebase waiting for an
	 * answer must not hold a filesystem out of service while it
	 * waits.
	 */
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
		 * be removed. Where -o named the manifest the pair is
		 * the user's: the run wrote them where they asked and
		 * does not take them back.
		 */
		if (r->recorded && r->o.outpath == NULL) {
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
			rmdir_run(r->rds);
	} else if (r->dropfrom) {
		/*
		 * done, and the from side was the tool's own
		 * snapshot: it lives exactly as long as the rebase,
		 * and the rebase is over. The holds were given back
		 * first, or this would find it busy.
		 */
		if (zr_zfs_destroy_snap(r->zfs, r->fromsnap, e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    r->fromsnap, e);
		else if (r->o.verbose)
			(void) fprintf(stderr, "zfs_rebase: %s was the tool's "
			    "own and is destroyed\n", r->fromsnap);
	}
	/*
	 * A kept run keeps its holds: they are the rebase, and its
	 * record names the tag that gives them back.
	 */
	if (r->zfs != NULL)
		zr_zfs_close(r->zfs);
}

/*
 * The final check --verify asks for, which is the verbs' own and is
 * written with them below: the run reaches it at its done gate, and
 * a --continue reaches the same function at the same gate.
 */
static int final_verify(struct run *r);

int
zr_run(const struct zr_run_opts *o)
{
	struct sigaction saved[ZR_NSIG];
	struct run r;
	struct zr_manifest_hdr hdr;
	struct zr_rebase_record rec;
	FILE *out = stdout;
	char cont[ZR_NAME_MAX];
	int rc = EXIT_INTERNAL, keep = 0, gocont = 0;

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
	 * snapshot is taken: it must be there, mounted, and free of
	 * any rebase but a finished one that --overwrite replaces.
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
	 * and the record says the tool made it: it lives as long as
	 * the rebase and no longer. The dataset form's pre-apply
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
	 * 3. the working tree -- a read-only clone in one form, the
	 * dataset itself taken over in the other -- the record, and
	 * then the three holds, which the record's tag names. A dry
	 * run creates nothing and holds nothing: it only reads, and
	 * it leaves no rebase behind to be continued or aborted.
	 */
	if (!o->dryrun) {
		if (r.replacing)
			overwrite_clear(&r);
		if (make_rundir(&r) != 0) {
			rc = fail(&r, EXIT_PRECOND, "run directory");
			goto done;
		}
		if (o->outpath != NULL) {
			(void) snprintf(r.manpath, sizeof (r.manpath), "%s",
			    o->outpath);
			resolution_beside(r.respath, sizeof (r.respath),
			    o->outpath);
		} else {
			(void) snprintf(r.manpath, sizeof (r.manpath),
			    "%s/manifest", r.rundir);
			resolution_path(r.respath, sizeof (r.respath), r.rds);
		}
		if (fill_record(&r, &rec) != 0) {
			rc = fail(&r, EXIT_PRECOND, "record");
			goto done;
		}
		if (in_dataset_form(&r)) {
			if (zr_zfs_write_record(r.zfs, r.rds, &rec, r.err,
			    sizeof (r.err)) != 0) {
				r.recorded = 1;	/* part of it may be there */
				rc = fail(&r, EXIT_PRECOND, "record");
				goto done;
			}
			r.recorded = 1;
		} else {
			if (zr_zfs_clone(r.zfs, r.ontosnap, o->result,
			    r.workmnt, &rec, r.err, sizeof (r.err)) != 0) {
				rc = fail(&r, EXIT_PRECOND, "clone");
				goto done;
			}
			r.cloned = 1;
			r.recorded = 1;
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

	/* 6. the manifest */
	hdr.base = r.base[0] != '\0' ? r.base : ZR_NO_BASE;
	hdr.from = r.fromsnap;
	hdr.onto = r.ontosnap;
	hdr.mode = o->mode;
	if (r.manpath[0] != '\0') {
		out = fopen(r.manpath, "w");
		if (out == NULL) {
			(void) snprintf(r.err, sizeof (r.err), "%s: %s",
			    r.manpath, strerror(errno));
			rc = fail(&r, EXIT_INTERNAL, "manifest");
			goto done;
		}
	}
	if (zr_manifest_emit(out, &hdr, &r.wb.zw_tree, &r.wf.zw_tree,
	    &r.wo.zw_tree, &r.d) != 0 || (out != stdout && fclose(out) != 0)) {
		(void) snprintf(r.err, sizeof (r.err), "write failed");
		rc = fail(&r, EXIT_INTERNAL, "manifest");
		goto done;
	}
	record_path(&r, ZR_PROP_MANIFEST, r.manpath, sizeof (r.manpath));
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
	 * which a rebase has a manifest and no resolution, and what
	 * a kill there leaves is a rebase whose exits are --restart,
	 * which writes the skeleton again from the recorded manifest,
	 * and --abort.
	 */
	zr_pause("manifest");
	if (write_skeleton(&r) != 0) {
		rc = fail(&r, EXIT_INTERNAL, "resolution");
		goto done;
	}

	/* 7. applying1: the clean actions and the self-check after them */
	zr_pause("decided");
	if (stopped(&r) != 0) {
		rc = fail(&r, EXIT_INTERNAL, "apply");
		goto done;	/* nothing written yet: the run goes */
	}
	/*
	 * A failure or a signal from here on leaves the state at the
	 * gate the run reached -- applying1 -- and the result, its
	 * record and its holds in place. There is no failed state and
	 * no interrupted state: what a stop leaves is a gate, and a
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
	release_trees(&r);
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
		set_state(&r, ZR_STATE_CONFLICTS);
		zr_pause(ZR_STATE_CONFLICTS);
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
	 * Done, and then the holds: written first so that a kill in
	 * between leaves a record that says the rebase finished and
	 * holds that --abort can still find and give back.
	 *
	 * A run that asked for --verify reaches that gate through the
	 * same function a --continue reaches it through: the record is
	 * on the result already, so the check is made over the record
	 * and not over anything this process happens to be holding,
	 * and a run killed before it and continued later makes exactly
	 * the same check. It costs a second walk of from, onto and the
	 * result, which is what asking for a check after the fact
	 * costs.
	 */
	if (o->verify) {
		rc = final_verify(&r);
		if (rc != EXIT_CLEAN) {
			manifest_note(&r);
			kept_hint(&r);
			goto done;
		}
		r.nheld = 0;		/* the check gave them back */
		r.privmnt = 0;		/* and the dataset with them */
	} else {
		set_state(&r, ZR_STATE_DONE);
		zr_pause(ZR_STATE_DONE);
		release_holds(&r);
		r.dropfrom = r.madefrom;
		if (in_dataset_form(&r))
			(void) fprintf(stderr, "zfs_rebase: %s is the rebased "
			    "tree, and %s is what it was before\n", r.rds,
			    r.ontosnap);
		else
			(void) fprintf(stderr, "zfs_rebase: %s is the rebased "
			    "tree, read-only at %s\n", r.rds, r.workmnt);
	}
	manifest_note(&r);
	rc = EXIT_CLEAN;
done:
	teardown(&r, keep);
	signals_restore(saved);
	/*
	 * The gate was passed by this run itself, and the rest of the
	 * rebase is a --continue: made after the teardown, so that
	 * the dataset form has handed the dataset back and given it
	 * up before the verb takes it over again, and made through
	 * zr_continue and not through some second path of the fresh
	 * run's own, so that what a person's --continue does and what
	 * this does are one thing. --no-merge cannot be set here: it
	 * is what would have stopped the run at the gate.
	 */
	if (gocont)
		rc = zr_continue(cont, o->verify, 0, o->verbose);
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
 * The record read back off a result. struct zr_rebase_record is what
 * a create is handed -- pointers into the run that made it -- so a
 * reader has to own the strings they point at. The state is not one
 * of them: the create writes none, and the gates write nothing else.
 */
struct record {
	struct zr_rebase_record	rec;
	char			base[ZR_SNAP_MAX];
	char			from[ZR_SNAP_MAX];
	char			onto[ZR_SNAP_MAX];
	char			made[ZR_SNAP_MAX];
	char			mode[16];
	char			form[16];
	char			tag[ZR_TAG_MAX];
	char			verify[8];
	char			take[8];	/* "onto", "from" or "-" */
	char			manifest[ZR_NAME_MAX];
	char			resolution[ZR_NAME_MAX];
	char			readonly[8];	/* the dataset form's own */
	char			state[32];	/* "" before the first gate */
};

/* One verb in flight. */
struct resume {
	struct zr_zfs		*zfs;
	char			result[ZR_NAME_MAX];
	int			verify;		/* --verify on the command */
	int			nomerge;	/* --no-merge on it */
	int			report;		/* the verb is --verify */
	int			verbose;
	int			dataset;	/* the dataset form */
	int			privmnt;	/* it is at workmnt just now */
	int			dropfrom;	/* done: the made snap goes */
	char			home[ZR_NAME_MAX];	/* where it belongs */
	struct record		rb;
	char			rundir[ZR_NAME_MAX];
	char			workmnt[ZR_NAME_MAX];	/* <rundir>/mnt */
	char			respath[ZR_NAME_MAX];	/* the resolution */
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
		return (rb->rec.base_guid);
	return (i == ZI_FROM ? rb->rec.from_guid : rb->rec.onto_guid);
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

/* One guid of the record, which the create wrote as decimal. */
static int
rec_int(struct resume *s, const char *prop, uint64_t *out)
{
	char buf[32];
	char *end;
	int got;

	*out = 0;
	got = rec_str(s, prop, buf, sizeof (buf));
	if (got <= 0)
		return (got);
	errno = 0;
	*out = strtoull(buf, &end, 10);
	if (errno != 0 || end == buf || *end != '\0') {
		(void) snprintf(s->err, sizeof (s->err),
		    "%s is \"%s\", which is no guid", prop, buf);
		return (-1);
	}
	return (1);
}

/*
 * The whole record, and the refusal that guards every verb: a
 * dataset carrying neither zfs_rebase:tag nor zfs_rebase:manifest as
 * a local value is not a zfs_rebase result, and nothing here touches
 * it -- not a dataset of the user's own that a mistyped name found,
 * and not one that only inherits those properties from a parent,
 * since zr_zfs_get_user answers for the local value alone.
 */
static int
read_record(struct resume *s)
{
	struct record *rb = &s->rb;
	int got;

	got = rec_str(s, ZR_PROP_TAG, rb->tag, sizeof (rb->tag));
	if (got < 0)
		return (-1);
	if (got > 0) {
		got = rec_str(s, ZR_PROP_MANIFEST, rb->manifest,
		    sizeof (rb->manifest));
		if (got < 0)
			return (-1);
	}
	if (got == 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s is not a "
		    "zfs_rebase result; nothing was touched", s->result);
		return (-1);
	}
	if (rec_str(s, ZR_PROP_BASE, rb->base, sizeof (rb->base)) < 0 ||
	    rec_str(s, ZR_PROP_FROM, rb->from, sizeof (rb->from)) < 0 ||
	    rec_str(s, ZR_PROP_ONTO, rb->onto, sizeof (rb->onto)) < 0 ||
	    rec_str(s, ZR_PROP_MADE, rb->made, sizeof (rb->made)) < 0 ||
	    rec_str(s, ZR_PROP_MODE, rb->mode, sizeof (rb->mode)) < 0 ||
	    rec_str(s, ZR_PROP_FORM, rb->form, sizeof (rb->form)) < 0 ||
	    rec_str(s, ZR_PROP_VERIFY, rb->verify, sizeof (rb->verify)) < 0 ||
	    rec_str(s, ZR_PROP_TAKE, rb->take, sizeof (rb->take)) < 0 ||
	    rec_str(s, ZR_PROP_RESOLUTION, rb->resolution,
	    sizeof (rb->resolution)) < 0 ||
	    rec_str(s, ZR_PROP_READONLY, rb->readonly,
	    sizeof (rb->readonly)) < 0 ||
	    rec_str(s, ZR_PROP_STATE, rb->state, sizeof (rb->state)) < 0 ||
	    rec_int(s, ZR_PROP_BASE_GUID, &rb->rec.base_guid) < 0 ||
	    rec_int(s, ZR_PROP_FROM_GUID, &rb->rec.from_guid) < 0 ||
	    rec_int(s, ZR_PROP_ONTO_GUID, &rb->rec.onto_guid) < 0)
		return (-1);
	rb->rec.base = rb->base;
	rb->rec.from = rb->from;
	rb->rec.onto = rb->onto;
	rb->rec.made = rb->made;
	rb->rec.mode = rb->mode;
	rb->rec.form = rb->form;
	rb->rec.tag = rb->tag;
	rb->rec.verify = rb->verify;
	/*
	 * A record written before the property existed carries no
	 * word, and a skeleton nobody answered in advance is what
	 * such a run wrote: the two say the same thing, so the empty
	 * value is read as ZR_TAKE_NONE and written back as that
	 * where a verb writes the record again.
	 */
	if (rb->take[0] == '\0')
		(void) snprintf(rb->take, sizeof (rb->take), "%s",
		    ZR_TAKE_NONE);
	rb->rec.take = rb->take;
	rb->rec.manifest = rb->manifest;
	rb->rec.resolution = rb->resolution;
	rb->rec.readonly = rb->readonly;
	/*
	 * The form the run was made in, which decides what the result
	 * is: a clone of the tool's own, or a dataset of the user's
	 * that the verb has to take over and hand back. A record with
	 * no form at all was written by the clone form before the
	 * dataset form existed.
	 */
	s->dataset = strcmp(rb->form, ZR_FORM_DATASET) == 0;
	return (0);
}

/* Did the run that made this record ask for the final check? */
static int
verify_asked(const struct record *rb)
{
	return (strcmp(rb->verify, "yes") == 0);
}

/*
 * The run directory, the mount point and the resolution's path. The
 * record names the resolution, as it names the manifest, and that is
 * where every verb looks: -o put it beside a manifest of the user's
 * choosing and no path can be guessed from the result's name alone.
 * A record written before the resolution had a property of its own
 * is read as the run directory's file, which is where such a run
 * would have looked for it.
 */
static int
resume_paths(struct resume *s)
{
	if ((size_t)snprintf(s->rundir, sizeof (s->rundir), "%s/%s", WORKDIR,
	    s->result) >= sizeof (s->rundir)) {
		(void) snprintf(s->err, sizeof (s->err), "%s/%s: %s", WORKDIR,
		    s->result, strerror(ENAMETOOLONG));
		return (-1);
	}
	(void) snprintf(s->workmnt, sizeof (s->workmnt), "%s/mnt", s->rundir);
	/*
	 * A record without the resolution's path is one a kill caught
	 * between the manifest and the skeleton (the "manifest" pause
	 * sits there). The path is then what the run would have chosen:
	 * beside the manifest, wherever the manifest went.
	 */
	if (s->rb.resolution[0] != '\0')
		(void) snprintf(s->respath, sizeof (s->respath), "%s",
		    s->rb.resolution);
	else if (s->rb.manifest[0] != '\0' &&
	    strncmp(s->rb.manifest, s->rundir, strlen(s->rundir)) != 0)
		resolution_beside(s->respath, sizeof (s->respath),
		    s->rb.manifest);
	else
		resolution_path(s->respath, sizeof (s->respath), s->result);
	return (0);
}

/*
 * Whether the record's base is the base of a run that had none:
 * --allow-unrelated without --base read the two sides against the
 * empty tree and wrote ZR_NO_BASE with the guid 0 in the record's
 * place for it. Nothing is there to find, to hold or to release, and
 * no verb walks the base in any case. The empty string is taken the
 * same way, for a record written by hand or by an older tool.
 */
static int
no_base(const char *snap)
{
	return (snap[0] == '\0' || strcmp(snap, ZR_NO_BASE) == 0);
}

/*
 * Every input the record names, found again. By name first, and the
 * guid must be the one the record kept: a snapshot destroyed and
 * taken again under the same name is another snapshot, and the
 * answers this rebase wrote do not describe it.
 *
 * byguid is the report's own way out. A snapshot is its guid, where
 * a name is only what it is called, so a rename or a promote is
 * followed here rather than reported as a loss; what cannot be found
 * by either is marked gone, and the actions that would have had to
 * read it come back unchecked. Without byguid -- a --continue or a
 * --restart, which have to read those trees to write anything -- a
 * missing input stops the verb instead.
 */
static int
find_inputs(struct resume *s, int byguid)
{
	char pool[ZR_SNAP_MAX];
	uint64_t have;
	int i, ex, rc;

	(void) snprintf(pool, sizeof (pool), "%.*s",
	    (int)strcspn(s->result, "/"), s->result);
	for (i = 0; i < 3; i++) {
		const char *want = rec_snap(&s->rb, i);

		/*
		 * A rebase made against the empty tree. Its base is
		 * not missing: there was none, and a verb asks
		 * nothing of it.
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
		}
		/*
		 * A snapshot the tool took itself is destroyed at
		 * done, so a rebase that reached its end is expected
		 * to be missing it and no verb stops for that. What
		 * is lost with it is the tree, and every verb that
		 * would have to read one says so where it is asked
		 * for: a repair at the done gate refuses, and a
		 * report calls those actions unchecked.
		 */
		if (ex == 0 && made_says(&s->rb, input_word(i)) &&
		    strcmp(s->rb.state, ZR_STATE_DONE) == 0) {
			s->gone[i] = 1;
			s->found[i][0] = '\0';
			continue;
		}
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
	if (zr_zfs_set_readonly(s->zfs, s->result, 0, s->err,
	    sizeof (s->err)) != 0)
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
	if (zr_zfs_set_readonly(s->zfs, s->result, 1, e, sizeof (e)) != 0) {
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

/* The result as it stands now, walked again beside the two sides. */
static int
rescan_result(struct resume *s)
{
	if (s->oracle != NULL) {
		zr_oracle_fini(s->oracle);
		s->oracle = NULL;
	}
	if ((s->walked & (1 << ZS_RESULT)) != 0) {
		zr_walk_fini(&s->w[ZS_RESULT]);
		s->walked &= ~(1 << ZS_RESULT);
	}
	if (zr_walk(s->workmnt, s->names, &s->w[ZS_RESULT], s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	s->walked |= 1 << ZS_RESULT;
	return (build_oracle(s));
}

/*
 * The dataset form's result, taken over for the length of this verb
 * exactly as the run took it over: unmounted from wherever it is and
 * mounted at the run's own place, with readonly on, so that no
 * writer can be in it while a stage or a walk reads it. A kill can
 * have left it privately mounted already, in which case there is
 * nothing to move.
 *
 * Its own mountpoint is remembered here rather than read again
 * later, since the property is what says where the dataset belongs
 * and nothing this verb does changes it.
 */
static int
take_over(struct resume *s)
{
	char at[ZR_NAME_MAX];
	int rc;

	if (zr_zfs_get(s->zfs, s->result, "mountpoint", s->home,
	    sizeof (s->home), s->err, sizeof (s->err)) != 0)
		return (-1);
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
		if (zr_zfs_mount_at(s->zfs, s->result, s->workmnt, s->err,
		    sizeof (s->err)) != 0) {
			char e[512];

			if (zr_zfs_mount(s->zfs, s->result, e,
			    sizeof (e)) != 0)
				(void) fprintf(stderr, "zfs_rebase: %s is "
				    "unmounted and will not mount: %s\n",
				    s->result, e);
			return (-1);
		}
		s->privmnt = 1;
		if (s->verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is this verb's "
			    "alone, mounted at %s\n", s->result, s->workmnt);
	}
	/*
	 * Read-only outside a stage, which is the clone form's own
	 * rule: ro_off and ro_on then work the same way in both.
	 */
	return (zr_zfs_set_readonly(s->zfs, s->result, 1, s->err,
	    sizeof (s->err)));
}

/*
 * The clone where the run left it. After a reboot the directories
 * under WORKDIR are still there but nothing is mounted, and the
 * mount point itself may have been taken away by hand, so both are
 * made good here. A result mounted anywhere but the run's own place
 * is not this run's to write into and is refused.
 */
static int
mount_result(struct resume *s)
{
	char buf[ZR_NAME_MAX];
	uint64_t mounted;

	if (s->dataset)
		return (take_over(s));
	if (zr_zfs_get(s->zfs, s->result, "mountpoint", buf, sizeof (buf),
	    s->err, sizeof (s->err)) != 0)
		return (-1);
	if (strcmp(buf, s->workmnt) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s is mounted at %s "
		    "and not at %s, which is this run's own place", s->result,
		    buf, s->workmnt);
		return (-1);
	}
	if (zr_zfs_get_int(s->zfs, s->result, "mounted", &mounted, s->err,
	    sizeof (s->err)) != 0)
		return (-1);
	if (mounted != ZR_NOT_MOUNTED)
		return (0);
	if (mkdir_p(s->workmnt, s->err, sizeof (s->err)) != 0 ||
	    zr_zfs_mount(s->zfs, s->result, s->err, sizeof (s->err)) != 0)
		return (-1);
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: mounted %s at %s\n",
		    s->result, s->workmnt);
	return (0);
}

/* The manifest the record names, which is the rebase's decision. */
static int
read_manifest(struct resume *s)
{
	FILE *fp;
	int rc;

	fp = fopen(s->rb.manifest, "r");
	if (fp == NULL) {
		(void) snprintf(s->err, sizeof (s->err), "%s: %s",
		    s->rb.manifest, strerror(errno));
		return (-1);
	}
	rc = zr_manifest_parse(fp, &s->man, s->err, sizeof (s->err));
	s->parsed = 1;
	(void) fclose(fp);
	return (rc);
}

/*
 * The resolution the record names: 1 with *out parsed, 0 when there
 * is no such file, -1 with err set. It is the document of choices of
 * v4-manifest.md section 8, and it must carry the same three header
 * lines the record does, since a resolution written for another
 * rebase describes another tree.
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
	if (out->zs_base == NULL || out->zs_from == NULL ||
	    out->zs_onto == NULL || strcmp(out->zs_base, s->rb.base) != 0 ||
	    strcmp(out->zs_from, s->rb.from) != 0 ||
	    strcmp(out->zs_onto, s->rb.onto) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s names other "
		    "snapshots than the record does", s->respath);
		return (-1);
	}
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
 * stage applies -- the recorded manifest for applying1, the
 * resolution for applying2 -- and state is the gate to write before
 * the first write, or NULL where the gate must not move.
 *
 * The classification is made whether anybody asked to see it: the
 * apply reads it to know what is already true and may be left alone,
 * and --verify only decides whether it is printed as well. After the
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
stage_apply(struct resume *s, const struct zr_parsed *m, const char *state,
    const char *what)
{
	struct zr_verify_report rep;
	struct zr_apply_stats st, rst;
	int fix, rc = -1;

	fix = state != NULL && strcmp(state, ZR_STATE_APPLYING1) == 0;
	memset(&rep, 0, sizeof (rep));
	if (state != NULL)
		put_state(s->zfs, s->result, state);
	if (ro_off(s) != 0)
		return (-1);
	/*
	 * The gate this stage has just written, for the harness. A
	 * repair passes no gate and stops at none.
	 */
	if (state != NULL)
		zr_pause(state);
	if (classify(s, m, &rep) != 0)
		goto out;
	if (s->verify)
		print_report(s, m, &rep, what);
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
	if (zr_apply_check(m, s->workmnt, s->names, &s->w[ZS_ONTO],
	    &s->w[ZS_FROM], s->miss, fix, &rst, s->err,
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
	if (rescan_result(s) != 0)
		goto out;
	rc = 0;
out:
	zr_verify_report_fini(&rep);
	if (ro_on(s) != 0)
		rc = -1;
	return (rc);
}

/*
 * One document held against the result and reported, which is what
 * the gates from conflicts on do with a verify: nothing here writes
 * and nothing here fails. A pending or a drifted action at one of
 * those gates is information and not a fault -- an edit made while
 * the conflicts were being answered is the person's work, and a gate
 * that failed on it would block done for good -- so the only failure
 * is a classification that could not be made at all.
 */
static int
final_check(struct resume *s, const struct zr_parsed *m, const char *what)
{
	struct zr_verify_report rep;
	int rc;

	memset(&rep, 0, sizeof (rep));
	rc = classify(s, m, &rep);
	if (rc == 0 && s->verify)
		print_report(s, m, &rep, what);
	zr_verify_report_fini(&rep);
	return (rc);
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
 * Only a --continue writes here, and only with --verify: a standalone
 * --verify writes nothing at any state, and nothing is written at
 * applying2 or at done. The document goes back to its recorded path
 * whole, as the manifest and the skeleton were written; nothing here
 * is a temporary file, since the file is the tool's own and a failure
 * to write it is a failure of the gate.
 *
 * Returns 0, or -1 with err set.
 */
static int
add_drift(struct resume *s, const struct zr_verify_report *rep)
{
	const char *nm;
	FILE *out;
	size_t len;
	uint32_t i, n = 0;

	for (i = 0; i < rep->zv_ndiffs; i++) {
		len = 0;
		nm = zr_names_str(s->names, rep->zv_diffs[i].zn_name, &len);
		if (nm == NULL || len == 0 || covered(&s->res, nm, len) != 0)
			continue;
		if (zr_resolution_add_drift(&s->res,
		    (const unsigned char *)nm, len,
		    name_isdir(s, rep->zv_diffs[i].zn_name),
		    ZR_CH_KEEP) != 0) {
			(void) snprintf(s->err, sizeof (s->err), "%s: cannot "
			    "take the drift line %s", s->respath, nm);
			return (-1);
		}
		n++;
	}
	if (n == 0)
		return (0);
	out = fopen(s->respath, "w");
	if (out == NULL) {
		(void) snprintf(s->err, sizeof (s->err), "%s: %s", s->respath,
		    strerror(errno));
		return (-1);
	}
	if (zr_resolution_write(out, &s->res) != 0 || fclose(out) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s: write failed",
		    s->respath);
		return (-1);
	}
	(void) fprintf(stderr, "zfs_rebase: %u drift line%s added to the "
	    "resolution %s\n", n, n == 1 ? "" : "s", s->respath);
	return (0);
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
 * The last gate. A record that asked for the final check gets it
 * here, over the manifest, and only then is done written and the
 * holds given back -- in that order, so that a kill in between leaves
 * a record that says the rebase finished and holds that --abort can
 * still find.
 *
 * The resolution is classified with it, since the check is one call:
 * a name kept is never compared, and a name answered onto or from is
 * held against that side's object. Neither a pending nor a drifted
 * line blocks this gate, any more than a pending action does -- an
 * edit made while the conflicts were being answered is the person's
 * work, and a gate that failed on it would block done for good.
 */
static int
done_gate(struct resume *s)
{
	if (verify_asked(&s->rb) &&
	    final_check(s, &s->man, "the manifest") != 0)
		return (vfail(s, EXIT_INTERNAL, "verify"));
	put_state(s->zfs, s->result, ZR_STATE_DONE);
	zr_pause(ZR_STATE_DONE);
	release_record(s);
	/*
	 * The from snapshot goes with the rebase when the tool took
	 * it, and it goes after the holds and after the walks, so it
	 * is only marked here; resume_close does it.
	 */
	s->dropfrom = made_says(&s->rb, "from");
	if (s->dataset)
		(void) fprintf(stderr, "zfs_rebase: %s is the rebased tree, "
		    "and %s is what it was before\n", s->result, s->rb.onto);
	else
		(void) fprintf(stderr, "zfs_rebase: %s is the rebased tree, "
		    "read-only at %s\n", s->result, s->workmnt);
	return (EXIT_CLEAN);
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
 * stage_apply does for a manifest, and print it under --verify.
 */
static int
apply_choices(struct resume *s, const struct zr_resolution *res)
{
	struct zr_apply_stats st, again;
	const char *first;

	if (zr_apply_choices(res, &s->man, s->workmnt, s->names,
	    &s->w[ZS_ONTO], &s->w[ZS_FROM], &st, s->err,
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
	if (zr_apply_choices(res, &s->man, s->workmnt, s->names,
	    &s->w[ZS_ONTO], &s->w[ZS_FROM], &again, s->err,
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
 * applying2: the choices of the resolution, carried out. The document
 * is read again here, because this is a gate a --continue can arrive
 * at on its own, and a stage cannot begin without the document it is
 * the stage of.
 */
/*
 * After the choices: the classification the second pass already
 * implies, made anyway, so that applying2 is checked the way
 * applying1 is -- by the one verify. Every onto and from line must
 * be done. The one exception is a directory line that reads
 * pending: its side has no such directory, and a name kept beneath
 * it holds it open, which is the choice's form of blocked and no
 * fault of the apply.
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
		if (rep.zv_rline[i] == ZR_OC_PENDING && l->zl_isdir)
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

static int
stage2(struct resume *s)
{
	struct zr_resolution res;
	uint32_t left;
	int rc;

	rc = read_resolution(s, &res);
	if (rc < 0) {
		zr_resolution_fini(&res);
		return (vfail(s, EXIT_PRECOND, "resolution"));
	}
	if (rc == 0) {
		zr_resolution_fini(&res);
		return (no_resolution(s));
	}
	left = zr_resolution_unanswered(&res);
	if (left != 0) {
		zr_resolution_fini(&res);
		(void) snprintf(s->err, sizeof (s->err), "%s is at applying2 "
		    "and %u name%s of %s went back to unanswered", s->result,
		    left, left == 1 ? "" : "s", s->respath);
		return (vfail(s, EXIT_PRECOND, NULL));
	}
	put_state(s->zfs, s->result, ZR_STATE_APPLYING2);
	rc = EXIT_INTERNAL;
	if (ro_off(s) != 0)
		goto out;
	zr_pause(ZR_STATE_APPLYING2);
	if (apply_choices(s, &res) != 0)
		goto out;
	/*
	 * And the trees this verb goes on with, which the choices have
	 * just changed: the done gate classifies against the result as
	 * it stands now.
	 */
	if (rescan_result(s) != 0 || choices_hold(s) != 0)
		goto out;
	rc = 0;
out:
	zr_resolution_fini(&res);
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
 * in which case this is where it waits and the state does not move.
 * Completeness plus this --continue is the whole of the signal: the
 * move is made on human input, and nothing but the person who
 * answered the conflicts can say they are answered.
 *
 * A verify asked for here reports, and writes into the resolution and
 * nowhere else. The tree is the person's from this gate on -- they
 * are answering conflicts in it, by hand or through a picker -- and
 * nothing here can tell an edit of theirs from a stray, so nothing
 * here touches the tree. What it does instead is say what it found:
 * every name that no longer stands as onto had it becomes a drift
 * line with the choice keep, which the person can change to onto or
 * to from. The one fix in the tool is applying1's own self-check,
 * which ran before this gate was ever written.
 */
static int
stage_conflicts(struct resume *s)
{
	uint32_t left, total;

	if (s->hasres < 0) {
		(void) snprintf(s->err, sizeof (s->err), "%s", s->reserr);
		return (vfail(s, EXIT_PRECOND, "resolution"));
	}
	if (s->hasres == 0)
		return (no_resolution(s));
	if (s->verify && conflicts_check(s) != 0)
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
 * Record that this rebase asks for the final check, in the word a
 * fresh run with --verify writes. It is the record's own property and
 * not this process's flag: the run may stop at conflicts and be
 * finished by another --continue that says nothing about verify, and
 * the check is made at done either way.
 */
static void
record_verify(struct resume *s)
{
	char e[512];

	if (zr_zfs_set_user(s->zfs, s->result, ZR_PROP_VERIFY, "yes", e,
	    sizeof (e)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s=yes: %s\n",
		    ZR_PROP_VERIFY, e);
		return;
	}
	(void) snprintf(s->rb.verify, sizeof (s->rb.verify), "yes");
	if (s->verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is recorded on %s; the "
		    "check is made at done\n", ZR_PROP_VERIFY, s->result);
}

/*
 * applying1: the recorded manifest, and the gate that follows it.
 *
 * --verify has no other meaning at this gate. The fix here is the
 * stage's own self-check, which is always on and is no flag's, and
 * the report the stage prints is the one stage_apply already makes;
 * what is left for the flag to do is to ask for the final check, so
 * it is written into the record and the done gate makes it.
 */
static int
stage1(struct resume *s)
{
	if (s->verify && verify_asked(&s->rb) == 0)
		record_verify(s);
	if (stage_apply(s, &s->man, ZR_STATE_APPLYING1, "the manifest") != 0)
		return (vfail(s, EXIT_INTERNAL, "apply"));
	if (vstopped(s) != 0)
		return (vfail(s, EXIT_INTERNAL, "apply"));
	if (s->man.zp_conflicts_declared == 0)
		return (done_gate(s));
	put_state(s->zfs, s->result, ZR_STATE_CONFLICTS);
	zr_pause(ZR_STATE_CONFLICTS);
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
		return (stage_conflicts(s));
	return (EXIT_CONFLICTS);
}

/*
 * A rebase that is already finished. Without --verify there is
 * nothing left but the cleanup a kill between the done gate and the
 * release would have skipped. With it, the two documents are reported
 * over one more time and nothing is written: after done the result is the
 * user's, new work in it is indistinguishable from drift, and a tool
 * that put onto's bytes back over it would be destroying work the
 * rebase never asked about.
 */
static int
stage_done(struct resume *s)
{
	if (s->verify) {
		if (final_check(s, &s->man, "the manifest") != 0)
			return (vfail(s, EXIT_INTERNAL, "verify"));
	}
	release_record(s);
	s->dropfrom = made_says(&s->rb, "from");
	if (s->dataset)
		(void) fprintf(stderr, "zfs_rebase: %s is done; %s is what it "
		    "was before\n", s->result, s->rb.onto);
	else
		(void) fprintf(stderr, "zfs_rebase: %s is done, read-only at "
		    "%s\n", s->result, s->workmnt);
	return (EXIT_CLEAN);
}

/*
 * Resume from the gate the record names. There is no state at all
 * until the first gate is written, and a run killed between the
 * clone and applying1 leaves exactly that: applying1 is where it
 * starts either way, since applying nothing again is what an
 * idempotent apply does over a tree nothing was applied to.
 */
static int
continue_from(struct resume *s)
{
	const char *state = s->rb.state;

	/*
	 * --no-merge stops at the conflicts gate, so it says
	 * something only up to it. A rebase already at applying2 or
	 * at done is past the merge: there is no gate left for the
	 * flag to hold, and carrying on regardless would be doing the
	 * one thing it was given to prevent.
	 */
	if (s->nomerge != 0 && (strcmp(state, ZR_STATE_APPLYING2) == 0 ||
	    strcmp(state, ZR_STATE_DONE) == 0)) {
		(void) snprintf(s->err, sizeof (s->err), "%s is at \"%s\", "
		    "past the merge; --no-merge has no gate left to stop at",
		    s->result, state);
		return (vfail(s, EXIT_PRECOND, NULL));
	}
	if (state[0] == '\0' || strcmp(state, ZR_STATE_APPLYING1) == 0)
		return (stage1(s));
	if (strcmp(state, ZR_STATE_CONFLICTS) == 0)
		return (stage_conflicts(s));
	if (strcmp(state, ZR_STATE_APPLYING2) == 0)
		return (stage2(s));
	if (strcmp(state, ZR_STATE_DONE) == 0)
		return (stage_done(s));
	(void) snprintf(s->err, sizeof (s->err), "%s is at \"%s\", which is "
	    "no gate of this tool", s->result, state);
	return (vfail(s, EXIT_PRECOND, NULL));
}

/*
 * What every verb does first: it must be root, libzfs must open, the
 * result must carry a record, and every input that record names must
 * still be the snapshot it named. Returns EXIT_CLEAN, or the status
 * to give up with.
 */
static int
resume_open(struct resume *s, const char *result, int byguid)
{
	dataset_of(result, s->result, sizeof (s->result));
	if (geteuid() != 0) {
		(void) fprintf(stderr, "zfs_rebase: must run as root\n");
		return (EXIT_PRECOND);
	}
	if (zr_zfs_open(&s->zfs, s->err, sizeof (s->err)) != 0)
		return (vfail(s, EXIT_PRECOND, "libzfs"));
	if (read_record(s) != 0 || resume_paths(s) != 0 ||
	    find_inputs(s, byguid) != 0)
		return (vfail(s, EXIT_PRECOND, NULL));
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
		    "%s\n", s->result, s->rb.state[0] != '\0' ? s->rb.state :
		    "no gate yet", s->rb.tag);
	return (EXIT_CLEAN);
}

/* The clone mounted, the manifest parsed, the trees walked. */
static int
resume_trees(struct resume *s)
{
	if (mount_result(s) != 0 || read_manifest(s) != 0)
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

static void
resume_close(struct resume *s)
{
	char e[512];
	int i;

	(void) ro_on(s);
	if (s->oracle != NULL)
		zr_oracle_fini(s->oracle);
	for (i = 2; i >= 0; i--) {
		if ((s->walked & (1 << i)) != 0)
			zr_walk_fini(&s->w[i]);
	}
	if (s->names != NULL)
		zr_names_destroy(s->names);
	if (s->parsed != 0)
		zr_parsed_fini(&s->man);
	zr_resolution_fini(&s->res);
	/*
	 * The dataset form gives the dataset back wherever the verb
	 * stops, and only now, with the walks closed: the snapshots
	 * under its .zfs are idle by this point and the unmount can
	 * have them. What the record says readonly was is what it
	 * goes back to.
	 */
	if (s->privmnt) {
		s->privmnt = 0;
		handback(s->zfs, s->result, s->rb.readonly, s->verbose);
	}
	/*
	 * A from snapshot the tool took itself lives exactly as long
	 * as the rebase, and the done gate is where the rebase ends.
	 * It goes after the walks let go of it and after the holds
	 * were released, which done did.
	 */
	if (s->dropfrom) {
		if (zr_zfs_destroy_snap(s->zfs, s->rb.from, e,
		    sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    s->rb.from, e);
		else if (s->verbose)
			(void) fprintf(stderr, "zfs_rebase: %s was the tool's "
			    "own and is destroyed\n", s->rb.from);
	}
	if (s->zfs != NULL)
		zr_zfs_close(s->zfs);
}

int
zr_continue(const char *result, int verify, int nomerge, int verbose)
{
	struct sigaction saved[ZR_NSIG];
	struct resume s;
	int rc;

	memset(&s, 0, sizeof (s));
	s.verify = verify;
	s.nomerge = nomerge;
	s.verbose = verbose;
	zr_pause_open();
	signals_install(saved);
	rc = resume_open(&s, result, 0);
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
	FILE *out;
	int rc = -1;

	if (zr_resolution_skeleton(&s->man, take_choice(s->rb.take),
	    &res) != 0) {
		(void) snprintf(s->err, sizeof (s->err), "out of memory");
		zr_resolution_fini(&res);
		return (-1);
	}
	out = fopen(s->respath, "w");
	if (out == NULL) {
		(void) snprintf(s->err, sizeof (s->err), "%s: %s", s->respath,
		    strerror(errno));
		zr_resolution_fini(&res);
		return (-1);
	}
	if (zr_resolution_write(out, &res) != 0 || fclose(out) != 0)
		(void) snprintf(s->err, sizeof (s->err), "%s: write failed",
		    s->respath);
	else
		rc = 0;
	if (rc == 0 && s->verbose)
		(void) fprintf(stderr, "zfs_rebase: %s is a skeleton again, "
		    "%u name%s, %u to answer\n", s->respath, res.zs_nlines,
		    res.zs_nlines == 1 ? "" : "s",
		    zr_resolution_unanswered(&res));
	zr_resolution_fini(&res);
	/*
	 * A record that never had the path (a kill between the manifest
	 * and the skeleton) gets it now, so that --abort can take the
	 * document this restart has just written away with the rest.
	 */
	if (rc == 0 && s->rb.resolution[0] == '\0') {
		char e[512];

		if (zr_zfs_set_user(s->zfs, s->result, ZR_PROP_RESOLUTION,
		    s->respath, e, sizeof (e)) != 0)
			(void) fprintf(stderr, "zfs_rebase: %s=%s: %s\n",
			    ZR_PROP_RESOLUTION, s->respath, e);
		else
			(void) snprintf(s->rb.resolution,
			    sizeof (s->rb.resolution), "%s", s->respath);
	}
	/*
	 * And the copy this verb goes on with, read back off the file
	 * that was just written: the classification the stage after
	 * this one makes must be against the document on disk, not
	 * against the answers the restart has just discarded.
	 */
	if (rc == 0) {
		zr_resolution_fini(&s->res);
		s->hasres = read_resolution(s, &s->res);
		if (s->hasres < 0)
			(void) snprintf(s->reserr, sizeof (s->reserr), "%s",
			    s->err);
	}
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
zr_restart(const char *result, int verbose)
{
	struct sigaction saved[ZR_NSIG];
	struct resume s;
	int rc;

	memset(&s, 0, sizeof (s));
	s.verbose = verbose;
	zr_pause_open();
	signals_install(saved);
	rc = resume_open(&s, result, 0);
	if (rc != EXIT_CLEAN)
		goto done;
	/*
	 * --restart applies the manifest again from the first gate,
	 * and that reads both sides. A rebase that reached done has
	 * destroyed a snapshot it took itself, and there is no
	 * starting again without it.
	 */
	if (s.miss != 0) {
		(void) snprintf(s.err, sizeof (s.err), "%s has reached done "
		    "and a tree the manifest reads is gone; there is nothing "
		    "to restart it from", s.result);
		rc = vfail(&s, EXIT_PRECOND, NULL);
		goto done;
	}
	if (s.dataset) {
		/*
		 * The dataset form puts the result back by rolling it
		 * to the pre-apply snapshot the record names, which
		 * is what the clone form's destroy-and-clone-again
		 * does: onto's tree exactly as it was, with the same
		 * record on it and no state, since it has passed no
		 * gate again. The rollback wants no unmount -- the
		 * kernel suspends and resumes the filesystem around
		 * it -- so it is made before the trees are read and
		 * the dataset is taken over, and nothing this process
		 * holds open is in the way.
		 */
		if (zr_zfs_rollback(s.zfs, s.result, s.rb.onto, s.err,
		    sizeof (s.err)) != 0) {
			rc = vfail(&s, EXIT_INTERNAL, "rollback");
			goto done;
		}
		s.rb.state[0] = '\0';
		if (zr_zfs_clear_user(s.zfs, s.result, ZR_PROP_STATE, s.err,
		    sizeof (s.err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: %s on %s: %s\n",
			    ZR_PROP_STATE, s.result, s.err);
		if (verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is %s again\n",
			    s.result, s.rb.onto);
		rc = restart_from(&s);
		goto done;
	}
	if (strcmp(s.rb.form, ZR_FORM_CLONE) != 0 && s.rb.form[0] != '\0') {
		(void) snprintf(s.err, sizeof (s.err), "%s was made in the %s "
		    "form, which this tool does not know", s.result,
		    s.rb.form);
		rc = vfail(&s, EXIT_PRECOND, NULL);
		goto done;
	}
	/*
	 * Destroy and clone again, with the record the old one carried:
	 * the same tag, so the holds it named are still this rebase's,
	 * the same manifest, which is still the decision, and no state,
	 * because the new clone has passed no gate. The holds
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
	    zr_zfs_clone(s.zfs, s.rb.onto, s.result, s.workmnt, &s.rb.rec,
	    s.err, sizeof (s.err)) != 0) {
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
	s.rb.state[0] = '\0';
	if (verbose)
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
			(void) fprintf(stderr, "zfs_rebase: this rebase had "
			    "no base: the two sides were read against the "
			    "empty tree\n");
			continue;
		}
		if (made_says(&s->rb, input_word(i)))
			(void) fprintf(stderr, "zfs_rebase: %s was given as a "
			    "dataset; its snapshot was destroyed at done\n",
			    input_word(i));
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
	rc = rep.zv_count[ZR_OC_PENDING] != 0 ||
	    rep.zv_count[ZR_OC_DRIFTED] != 0 ? EXIT_INTERNAL : EXIT_CLEAN;
out:
	zr_verify_report_fini(&rep);
	return (rc);
}

int
zr_report(const char *result, int verbose)
{
	struct resume s;
	int code;

	memset(&s, 0, sizeof (s));
	s.verify = 1;			/* the report is the whole verb */
	s.report = 1;
	s.verbose = verbose;
	tag_make(s.tmptag, sizeof (s.tmptag), "zrv-");
	code = resume_open(&s, result, 1);
	if (code != EXIT_CLEAN)
		goto done;
	hold_for_report(&s);
	if (resume_trees(&s) != 0) {
		code = vfail(&s, EXIT_PRECOND, s.result);
		goto done;
	}
	explain_gone(&s);
	if (strcmp(s.rb.state, ZR_STATE_DONE) == 0)
		(void) fprintf(stderr, "zfs_rebase: %s reached done: this "
		    "report is as of now, and the inputs have not been held "
		    "since it did, so what has changed in them since is not "
		    "something this can tell from what the rebase made\n",
		    s.result);
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
 * The final check a fresh run's --verify asked for, made by the
 * verbs' own machinery over the record the run has just written: the
 * result walked again beside from and onto, every action classified,
 * and done and the release only after that. It is the same function
 * --continue reaches at its own done gate, so a run killed before it
 * and continued later makes exactly this check and no other one.
 */
static int
final_verify(struct run *r)
{
	struct resume s;
	int rc;

	memset(&s, 0, sizeof (s));
	s.verify = 1;
	s.verbose = r->o.verbose;
	rc = resume_open(&s, r->rds, 0);
	if (rc == EXIT_CLEAN) {
		rc = resume_trees(&s) != 0 ?
		    vfail(&s, EXIT_INTERNAL, "verify") : done_gate(&s);
	}
	resume_close(&s);
	return (rc);
}

/*
 * The dataset form's half of --abort: the dataset is the user's and
 * is put back rather than destroyed. The holds are already released
 * by the time this runs, so nothing here finds a snapshot busy.
 *
 *	roll the dataset back to the pre-apply snapshot, which is
 *	what "as if the run never happened" means when the run wrote
 *	into a dataset of the user's;
 *	destroy that snapshot, which the rebase owned from the moment
 *	--result named it;
 *	take the record off, property by property;
 *	give the dataset back: readonly as the record says it was,
 *	mounted where its mountpoint property says.
 *
 * A rollback that cannot be made -- the snapshot gone, or a newer
 * snapshot in the way -- stops the abort with the record intact,
 * because the alternative is to forget a rebase that is still in the
 * tree. Returns 0, or -1 with the reason already printed.
 */
static int
abort_dataset(struct zr_zfs *z, const char *result, const char *rundir,
    int verbose)
{
	char snap[ZR_SNAP_MAX], ro[8], at[ZR_NAME_MAX], mnt[ZR_NAME_MAX];
	char err[512];
	int rc;

	ro[0] = '\0';
	if (zr_zfs_get_user(z, result, ZR_PROP_ONTO, snap, sizeof (snap), err,
	    sizeof (err)) <= 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: the record names no "
		    "onto snapshot, so %s cannot be put back\n", result,
		    result);
		return (-1);
	}
	(void) zr_zfs_get_user(z, result, ZR_PROP_READONLY, ro, sizeof (ro),
	    err, sizeof (err));
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
	if (zr_zfs_destroy_snap(z, snap, err, sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n", snap,
		    err);
	clear_record(z, result, verbose);
	/*
	 * And back to service. A kill can have left it at the run's
	 * own mount point, at its own, or nowhere at all; only the
	 * first has to be undone, and the other two want no unmount
	 * that could fail for nothing.
	 */
	(void) snprintf(mnt, sizeof (mnt), "%s/mnt", rundir);
	rc = zr_zfs_mounted_at(z, result, at, sizeof (at), err, sizeof (err));
	if (rc > 0 && strcmp(at, mnt) == 0) {
		handback(z, result, ro, verbose);
		return (0);
	}
	if (ro[0] != '\0' && zr_zfs_set_readonly(z, result,
	    strcmp(ro, "on") == 0, err, sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: readonly=%s on %s: %s\n",
		    ro, result, err);
	if (rc == 0 && zr_zfs_mount(z, result, err, sizeof (err)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s will not mount: %s\n",
		    result, err);
	return (0);
}

/*
 * --abort: take one rebase away and nothing else. "As if the run
 * never happened": the holds are released, the result is put back --
 * the clone destroyed, or the dataset rolled back to its pre-apply
 * snapshot, stripped of the record and mounted where it belongs
 * again -- the snapshots the tool took for itself are destroyed, the
 * two documents the record names -- the manifest and the resolution
 * -- are unlinked and the run directories go.
 *
 * The record is the key, and the refusal is the point of it. A
 * dataset that does not carry both zfs_rebase:tag and
 * zfs_rebase:manifest locally is not a zfs_rebase result and is left
 * alone, so a mistyped or a remembered-wrong name cannot cost the
 * user a dataset of their own, and neither can an inherited value:
 * a user property set on a parent shows up on every dataset beneath
 * it, and zr_zfs_get_user answers for the local value only. Every
 * state is fair game, applying1 included, because a process killed
 * part way through the apply leaves exactly that and this is what
 * clears it.
 *
 * It can be run again. The holds are released first, so a destroy
 * that fails leaves nothing held that a second --abort would have
 * to redo; a release of a tag that is not there, or of a snapshot
 * that is not there, is not a failure; a document already unlinked
 * is not one either; and a run whose dataset is gone but whose
 * directory is not is finished by removing the directory. Only when
 * there is nothing at all left does --abort say "no such run".
 * Nothing is removed recursively: the only files this unlinks are
 * the two the record names, and every directory goes by rmdir, which
 * will not touch one that is not empty.
 */
int
zr_abort(const char *result, int verbose)
{
	static const char *nameprop[3] = {
		ZR_PROP_BASE, ZR_PROP_FROM, ZR_PROP_ONTO
	};
	char manifest[ZR_NAME_MAX], resolution[ZR_NAME_MAX];
	char dir[ZR_NAME_MAX];
	char snap[ZR_SNAP_MAX], tag[ZR_TAG_MAX];
	char state[64], form[16], made[ZR_SNAP_MAX], err[512];
	struct zr_zfs *z = NULL;
	struct stat sb;
	int rc = EXIT_INTERNAL, hasdir, hasds, hasman = 0, i;

	if (geteuid() != 0) {
		(void) fprintf(stderr, "zfs_rebase: must run as root\n");
		return (EXIT_PRECOND);
	}
	if ((size_t)snprintf(dir, sizeof (dir), "%s/%s", WORKDIR, result) >=
	    sizeof (dir)) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", result,
		    strerror(ENAMETOOLONG));
		return (EXIT_PRECOND);
	}
	hasdir = stat(dir, &sb) == 0;
	if (zr_zfs_open(&z, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: libzfs: %s\n", err);
		return (EXIT_PRECOND);
	}
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
	if (hasds != 0) {
		int got = zr_zfs_get_user(z, result, ZR_PROP_TAG, tag,
		    sizeof (tag), err, sizeof (err));

		if (got < 0) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n", result,
			    err);
			rc = EXIT_PRECOND;
			goto done;
		}
		if (got > 0) {
			hasman = zr_zfs_get_user(z, result, ZR_PROP_MANIFEST,
			    manifest, sizeof (manifest), err, sizeof (err));
			if (hasman < 0) {
				(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
				    ZR_PROP_MANIFEST, err);
				rc = EXIT_PRECOND;
				goto done;
			}
		}
		if (got == 0 || hasman == 0) {
			(void) fprintf(stderr, "zfs_rebase: %s is not a "
			    "zfs_rebase result; nothing was touched\n",
			    result);
			rc = EXIT_PRECOND;
			goto done;
		}
		if (verbose) {
			if (zr_zfs_get_user(z, result, ZR_PROP_STATE, state,
			    sizeof (state), err, sizeof (err)) > 0)
				(void) fprintf(stderr, "zfs_rebase: %s is at "
				    "%s, held under %s\n", result, state, tag);
			else
				(void) fprintf(stderr, "zfs_rebase: %s has no "
				    "state yet, held under %s\n", result, tag);
		}
		/*
		 * The three inputs by name. One that is missing from
		 * the record, or gone from the pool, is nothing to
		 * release; a release that fails for any other reason
		 * stops the abort with the record intact, so that
		 * running it again can try the same thing.
		 */
		for (i = 0; i < 3; i++) {
			int ex;

			if (zr_zfs_get_user(z, result, nameprop[i], snap,
			    sizeof (snap), err, sizeof (err)) <= 0) {
				if (verbose)
					(void) fprintf(stderr, "zfs_rebase: "
					    "%s: no %s in the record\n",
					    result, nameprop[i]);
				continue;
			}
			ex = zr_zfs_exists(z, snap, err, sizeof (err));
			if (ex == 0) {
				if (verbose)
					(void) fprintf(stderr, "zfs_rebase: "
					    "%s is gone; nothing to release\n",
					    snap);
				continue;
			}
			if (ex < 0 || zr_zfs_release(z, snap, tag, err,
			    sizeof (err)) != 0) {
				(void) fprintf(stderr, "zfs_rebase: release "
				    "%s on %s: %s\n", tag, snap, err);
				goto done;
			}
			if (verbose)
				(void) fprintf(stderr, "zfs_rebase: released "
				    "%s on %s\n", tag, snap);
		}
		/*
		 * And then the result itself, which the form decides:
		 * a clone of the tool's own is destroyed, and a
		 * dataset of the user's is rolled back to the
		 * pre-apply snapshot, stripped of the record and put
		 * back into service.
		 */
		if (zr_zfs_get_user(z, result, ZR_PROP_FORM, form,
		    sizeof (form), err, sizeof (err)) < 0) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
			    ZR_PROP_FORM, err);
			goto done;
		}
		/*
		 * A snapshot the tool took of a side given as a
		 * dataset belongs to the rebase and goes with it, in
		 * either form. It is read before the dataset form
		 * takes the record off.
		 */
		made[0] = '\0';
		snap[0] = '\0';
		if (zr_zfs_get_user(z, result, ZR_PROP_MADE, made,
		    sizeof (made), err, sizeof (err)) > 0 &&
		    strstr(made, "from") != NULL)
			(void) zr_zfs_get_user(z, result, ZR_PROP_FROM, snap,
			    sizeof (snap), err, sizeof (err));
		if (strcmp(form, ZR_FORM_DATASET) == 0) {
			if (abort_dataset(z, result, dir, verbose) != 0)
				goto done;
			(void) fprintf(stderr, "zfs_rebase: %s is as it was "
			    "before the rebase\n", result);
		} else {
			if (zr_zfs_destroy(z, result, err,
			    sizeof (err)) != 0) {
				(void) fprintf(stderr, "zfs_rebase: destroy "
				    "%s: %s\n", result, err);
				goto done;
			}
			(void) fprintf(stderr, "zfs_rebase: destroyed %s\n",
			    result);
		}
		if (snap[0] != '\0') {
			if (zr_zfs_destroy_snap(z, snap, err,
			    sizeof (err)) != 0)
				(void) fprintf(stderr, "zfs_rebase: destroy "
				    "%s: %s\n", snap, err);
			else
				(void) fprintf(stderr, "zfs_rebase: destroyed "
				    "%s, which the tool took itself\n", snap);
		}
		if (unlink(manifest) == 0)
			(void) fprintf(stderr, "zfs_rebase: removed the "
			    "manifest %s\n", manifest);
		else if (errno != ENOENT)
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
			    manifest, strerror(errno));
		/*
		 * And the resolution the run wrote beside it. A record
		 * from before the resolution had a property of its own
		 * names none, and there is nothing to remove.
		 */
		if (zr_zfs_get_user(z, result, ZR_PROP_RESOLUTION, resolution,
		    sizeof (resolution), err, sizeof (err)) > 0) {
			if (unlink(resolution) == 0)
				(void) fprintf(stderr, "zfs_rebase: removed "
				    "the resolution %s\n", resolution);
			else if (errno != ENOENT)
				(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
				    resolution, strerror(errno));
		}
	}
	if (hasdir) {
		rmdir_run(result);
		(void) fprintf(stderr, "zfs_rebase: removed %s\n", dir);
	}
	rc = EXIT_CLEAN;
done:
	zr_zfs_close(z);
	return (rc);
}
