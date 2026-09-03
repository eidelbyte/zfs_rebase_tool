/*
 * The real run: holds on the three snapshots the user named, a
 * read-only working clone of onto's snapshot under the name the user
 * asked for, the walk of those three snapshots through .zfs/snapshot,
 * zfs diff for the unchanged set, decide, manifest, apply, re-walk.
 * Everything here is library calls; nothing is exec'd. The ZFS
 * operations themselves are in zfsops.c and exist only in the
 * FreeBSD build.
 *
 * The tool takes no snapshots and destroys none of the user's. The
 * trees it reads are snapshots the user made, it only holds them for
 * the length of the run, and the one dataset it creates is the
 * result clone, named by --result. Only the two sides are named:
 * the base is the branch point, and the run works it out by walking
 * the two origin chains back to the dataset they share.
 *
 * What survives what. Every hold is filed against the cleanup
 * descriptor on /dev/zfs, so any exit at all -- a clean return, an
 * error, SIGKILL -- closes that descriptor and the kernel drops the
 * holds with it. A crash or a power loss leaves them in the pool,
 * and the next import drops them: dsl_pool_clean_tmp_userrefs, which
 * spa_load calls. So the only things a hard kill can leave behind
 * are the result clone and the manifest file, and
 *
 *	zfs_rebase --abort --result NAME
 *
 * removes both. While the clone lives, onto's snapshot cannot be
 * destroyed; that is ZFS's own rule about a clone's origin and not
 * something this tool arranges.
 *
 * The clone carries zfs_rebase:state from birth, and the run moves
 * it through: "created", then "conflicts" (the decision has
 * conflicts, so nothing is applied and the clone is onto's tree
 * unchanged), or "applying" (readonly is off), "applied" (the result
 * verified and readonly is back on), or "failed", or "interrupted".
 *
 * A signal that would ordinarily end the process -- INT, TERM, HUP
 * -- is caught instead and only raises a flag. Before every phase,
 * and between two actions of the apply, the flag is looked at: if it
 * is up the run stops there and says so. Before the apply that
 * destroys the clone as any other failure before the apply does;
 * inside the apply the clone is kept, put back to read-only and
 * marked "interrupted", and --abort takes it away. The handlers do
 * not ask for SA_RESTART, so a read or a write already in a slow
 * call fails with EINTR rather than starting over, and the phase
 * that owns it reports that failure in the ordinary way. SIGPIPE is
 * ignored for the length of the run, which is what zfs(8) does
 * around zfs_show_diffs: the diff runs a thread over a pipe, and a
 * failure at one end must not kill the process before libzfs can say
 * what went wrong.
 */

#include <errno.h>
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
#include "diff.h"
#include "manifest.h"
#include "name.h"
#include "run.h"
#include "walk.h"
#include "yellow.h"
#include "zfsops.h"

#define	EXIT_CLEAN	0
#define	EXIT_CONFLICTS	1
#define	EXIT_PRECOND	2
#define	EXIT_INTERNAL	3

#define	WORKDIR		"/var/run/zfs_rebase"

/*
 * A dataset or a snapshot name is at most ZFS_MAX_DATASET_NAME_LEN,
 * which is 256; the origin chains are sized by that rather than by
 * ZR_NAME_MAX, which has to hold a mountpoint. Thirty-two links is
 * a clone of a clone of a clone thirty-two deep, and a chain longer
 * than that is refused rather than followed.
 */
#define	ZR_SNAP_MAX	256
#define	ZR_CHAIN_MAX	32

struct run {
	struct zr_run_opts	o;
	struct zr_zfs		*zfs;
	char			holdtag[64];	/* zfs_rebase-<time>-<pid> */
	char			base[ZR_NAME_MAX];	/* the branch point */
	char			rundir[ZR_NAME_MAX];	/* WORKDIR/<result> */
	char			workmnt[ZR_NAME_MAX];	/* <rundir>/mnt */
	char			manpath[ZR_NAME_MAX];	/* the manifest */
	char			basemnt[ZR_NAME_MAX];	/* mountpoints */
	char			frommnt[ZR_NAME_MAX];
	char			ontomnt[ZR_NAME_MAX];
	int			dirmade, cloned, walked;
	struct zr_names		*names;
	struct zr_walk		wb, wf, wo;
	struct zr_oracle	*oracle;
	struct zr_decision	d;
	char			err[512];
};

/*
 * The signals the run catches, and SIGPIPE, which it ignores. The
 * handler does the one thing a handler may do here: raise the flag
 * apply.c and every phase boundary read.
 */
static const int zr_sigs[] = { SIGPIPE, SIGINT, SIGTERM, SIGHUP };
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
	for (i = 0; i < ZR_NSIG; i++) {
		sa.sa_handler = zr_sigs[i] == SIGPIPE ? SIG_IGN : on_signal;
		(void) sigaction(zr_sigs[i], &sa, &saved[i]);
	}
}

static void
signals_restore(const struct sigaction *saved)
{
	size_t i;

	for (i = 0; i < ZR_NSIG; i++)
		(void) sigaction(zr_sigs[i], &saved[i], NULL);
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

/* Where the run's own state is, and how to be rid of it. */
static void
abort_hint(const struct run *r)
{
	(void) fprintf(stderr, "zfs_rebase: the clone %s is kept; "
	    "zfs_rebase --abort --result %s removes the run\n", r->o.result,
	    r->o.result);
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

	nf = origin_chain(r, r->o.from, fc);
	if (nf < 0)
		return (-1);
	no = origin_chain(r, r->o.onto, oc);
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
	if (strcmp(base, r->o.from) == 0) {
		(void) snprintf(r->err, sizeof (r->err),
		    "onto already contains from; nothing to rebase");
		return (-1);
	}
	if (strcmp(base, r->o.onto) == 0) {
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
 * must be mounted, and the three must agree on names.
 */
static int
preconditions(struct run *r)
{
	static const char *props[] = { "casesensitivity", "normalization" };
	static const char *want[] = { "sensitive", "none" };
	char ds[3][ZR_NAME_MAX], buf[64];
	int i, p;

	if (derive_base(r) != 0)
		return (-1);
	dataset_of(r->base, ds[0], sizeof (ds[0]));
	dataset_of(r->o.from, ds[1], sizeof (ds[1]));
	dataset_of(r->o.onto, ds[2], sizeof (ds[2]));
	for (i = 0; i < 3; i++) {
		if (zr_zfs_get(r->zfs, ds[i], "mounted", buf, sizeof (buf),
		    r->err, sizeof (r->err)) != 0)
			return (-1);
		if (strcmp(buf, "yes") != 0) {
			(void) snprintf(r->err, sizeof (r->err),
			    "%s is not mounted", ds[i]);
			return (-1);
		}
		for (p = 0; p < 2; p++) {
			if (zr_zfs_get(r->zfs, ds[i], props[p], buf,
			    sizeof (buf), r->err, sizeof (r->err)) != 0)
				return (-1);
			if (strcmp(buf, want[p]) != 0) {
				(void) snprintf(r->err, sizeof (r->err),
				    "%s has %s=%s; need %s", ds[i], props[p],
				    buf, want[p]);
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
	if (!r->o.dryrun && result_ok(r, ds[2]) != 0)
		return (-1);
	if (zr_zfs_get(r->zfs, ds[0], "mountpoint", r->basemnt,
	    sizeof (r->basemnt), r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get(r->zfs, ds[1], "mountpoint", r->frommnt,
	    sizeof (r->frommnt), r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get(r->zfs, ds[2], "mountpoint", r->ontomnt,
	    sizeof (r->ontomnt), r->err, sizeof (r->err)) != 0)
		return (-1);
	return (0);
}

/*
 * WORKDIR/<result>, root-only, so nothing else can look in. The
 * dataset name character set is [A-Za-z0-9_.:-] and the separator,
 * so the name is a path already and the tree under WORKDIR mirrors
 * the dataset tree. Every component is made 0700 and may exist; the
 * leaf may not, because a leaf that is there is another run of this
 * result.
 */
static int
make_rundir(struct run *r)
{
	size_t top = sizeof (WORKDIR) - 1;
	char *p;

	if ((size_t)snprintf(r->rundir, sizeof (r->rundir), "%s/%s", WORKDIR,
	    r->o.result) >= sizeof (r->rundir)) {
		(void) snprintf(r->err, sizeof (r->err), "%s/%s: %s", WORKDIR,
		    r->o.result, strerror(ENAMETOOLONG));
		return (-1);
	}
	if (mkdir(WORKDIR, 0700) != 0 && errno != EEXIST) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", WORKDIR,
		    strerror(errno));
		return (-1);
	}
	for (p = r->rundir + top + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(r->rundir, 0700) != 0 && errno != EEXIST) {
			(void) snprintf(r->err, sizeof (r->err), "%s: %s",
			    r->rundir, strerror(errno));
			*p = '/';
			return (-1);
		}
		*p = '/';
	}
	if (mkdir(r->rundir, 0700) != 0) {
		if (errno == EEXIST)
			(void) snprintf(r->err, sizeof (r->err),
			    "a run for %s is in place (%s); zfs_rebase "
			    "--abort --result %s removes it", r->o.result,
			    r->rundir, r->o.result);
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

/* The state marker is a record, not a step: a failure to set it warns. */
static void
set_state(struct run *r, const char *state)
{
	char e[512];

	if (!r->cloned)
		return;
	if (zr_zfs_set_user(r->zfs, r->o.result, ZR_PROP_STATE, state, e,
	    sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s=%s: %s\n",
		    ZR_PROP_STATE, state, e);
}

/*
 * The clone was created with the manifest path the run intended; now
 * that the file is there, resolve it and record what it really is,
 * so that --abort unlinks the file this run wrote whatever directory
 * it was started from.
 */
static void
record_manifest(struct run *r)
{
	char e[512];
	char *real;

	if (!r->cloned || r->manpath[0] == '\0')
		return;
	real = realpath(r->manpath, NULL);
	if (real == NULL) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", r->manpath,
		    strerror(errno));
		return;
	}
	if (zr_zfs_set_user(r->zfs, r->o.result, ZR_PROP_MANIFEST, real, e,
	    sizeof (e)) != 0)
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
		    ZR_PROP_MANIFEST, e);
	else
		(void) snprintf(r->manpath, sizeof (r->manpath), "%s", real);
	free(real);
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

static int
read_trees(struct run *r)
{
	char path[ZR_NAME_MAX * 2];
	struct zr_diff df, dfo;
	int marked;

	r->names = zr_names_create();
	if (r->names == NULL)
		return (-1);
	snapdir(path, sizeof (path), r->basemnt, r->base);
	if (zr_walk(path, r->names, &r->wb, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 1;
	if (stopped(r) != 0)
		return (-1);
	snapdir(path, sizeof (path), r->frommnt, r->o.from);
	if (zr_walk(path, r->names, &r->wf, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 2;
	if (stopped(r) != 0)
		return (-1);
	snapdir(path, sizeof (path), r->ontomnt, r->o.onto);
	if (zr_walk(path, r->names, &r->wo, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 3;
	if (stopped(r) != 0)
		return (-1);

	if (zr_oracle_init(&r->oracle, &r->wb, &r->wf, &r->wo) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "out of memory");
		return (-1);
	}
	/* zfs diff from the base snapshot to each side: the unchanged set */
	if (zr_zfs_diff(r->zfs, r->base, r->o.from, r->frommnt, &df,
	    r->err, sizeof (r->err)) != 0)
		return (-1);
	marked = zr_diff_apply_unchanged(&df, &r->wb, &r->wf, 1, r->oracle);
	zr_diff_fini(&df);
	if (stopped(r) != 0)
		return (-1);
	if (zr_zfs_diff(r->zfs, r->base, r->o.onto, r->ontomnt, &dfo,
	    r->err, sizeof (r->err)) != 0)
		return (-1);
	marked += zr_diff_apply_unchanged(&dfo, &r->wb, &r->wo, 2, r->oracle);
	zr_diff_fini(&dfo);
	if (stopped(r) != 0)
		return (-1);
	if (r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: %d pools unchanged\n",
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

/* Emit to a temp file, parse it back, and apply that: one path. */
static int
apply_manifest(struct run *r, const struct zr_manifest_hdr *hdr)
{
	struct zr_parsed parsed;
	struct zr_apply_stats st;
	FILE *fp = tmpfile();
	int rc = -1;

	if (fp == NULL) {
		(void) snprintf(r->err, sizeof (r->err), "tmpfile: %s",
		    strerror(errno));
		return (-1);
	}
	memset(&parsed, 0, sizeof (parsed));
	if (zr_manifest_emit(fp, hdr, &r->wb.zw_tree, &r->wf.zw_tree,
	    &r->wo.zw_tree, &r->d) != 0) {
		(void) snprintf(r->err, sizeof (r->err),
		    "cannot write manifest");
		goto done;
	}
	rewind(fp);
	if (zr_manifest_parse(fp, &parsed, r->err, sizeof (r->err)) != 0)
		goto done;
	set_state(r, "applying");
	if (zr_zfs_set_readonly(r->zfs, r->o.result, 0, r->err,
	    sizeof (r->err)) != 0)
		goto done;
	rc = zr_apply(&parsed, r->workmnt, &r->wf, &r->wo, &st, r->err,
	    sizeof (r->err));
	if (rc == 0 && r->o.verbose)
		(void) fprintf(stderr, "zfs_rebase: applied %llu rm %llu ln "
		    "%llu cp %llu dup %llu write, %llu bytes\n",
		    (unsigned long long)st.zs_rm, (unsigned long long)st.zs_ln,
		    (unsigned long long)st.zs_cp, (unsigned long long)st.zs_dup,
		    (unsigned long long)st.zs_write,
		    (unsigned long long)st.zs_bytes);
	if (zr_zfs_set_readonly(r->zfs, r->o.result, 1, r->err,
	    sizeof (r->err)) != 0)
		rc = -1;
done:
	zr_parsed_fini(&parsed);
	(void) fclose(fp);
	return (rc);
}

/*
 * After apply: walk the clone and check every result pool against the
 * decision by names, pooling and (through the oracle's handles) bytes.
 * Any difference is an internal error; the clone is left for
 * inspection.
 */
static int
verify_clone(struct run *r)
{
	struct zr_walk w;
	uint32_t i, j;
	int rc = -1;

	if (zr_walk(r->workmnt, r->names, &w, r->err, sizeof (r->err)) != 0)
		return (-1);
	for (i = 0; i < r->d.zd_npools; i++) {
		const struct zr_result_pool *rp = &r->d.zd_pools[i];
		zr_pool_t q;

		if (r->d.zd_groups[rp->zr_group].zg_flags != 0)
			continue;	/* conflicted: left as onto had it */
		q = zr_tree_pool(&w.zw_tree, rp->zr_names[0]);
		if (q == ZR_POOL_NONE ||
		    w.zw_tree.zt_pools[q].zp_nnames != rp->zr_nnames) {
			size_t len;

			(void) snprintf(r->err, sizeof (r->err),
			    "after apply, %s has %u names, decided %u",
			    zr_names_str(r->names, rp->zr_names[0], &len),
			    q == ZR_POOL_NONE ? 0 :
			    w.zw_tree.zt_pools[q].zp_nnames, rp->zr_nnames);
			goto done;
		}
		for (j = 0; j < rp->zr_nnames; j++) {
			if (zr_tree_pool(&w.zw_tree, rp->zr_names[j]) != q) {
				size_t len;

				(void) snprintf(r->err, sizeof (r->err),
				    "after apply, %s is not with its pool",
				    zr_names_str(r->names, rp->zr_names[j],
				    &len));
				goto done;
			}
		}
	}
	rc = 0;
done:
	zr_walk_fini(&w);
	return (rc);
}

static void
teardown(struct run *r, int keep_clone)
{
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
	if (!keep_clone) {
		if (r->cloned) {
			if (zr_zfs_destroy(r->zfs, r->o.result, r->err,
			    sizeof (r->err)) != 0)
				(void) fprintf(stderr,
				    "zfs_rebase: destroy %s: %s\n",
				    r->o.result, r->err);
			/*
			 * Only the run's own manifest goes, and it
			 * has to, because it is inside the directory
			 * about to be removed. A file the user named
			 * with -o is theirs; the run wrote it where
			 * they asked and does not take it back.
			 */
			if (r->o.outpath == NULL)
				(void) unlink(r->manpath);
		}
		if (r->dirmade)
			rmdir_run(r->o.result);
	}
	/* holds die with the cleanup descriptor, snapshots stay */
	if (r->zfs != NULL)
		zr_zfs_close(r->zfs);
}

int
zr_run(const struct zr_run_opts *o)
{
	struct sigaction saved[ZR_NSIG];
	struct run r;
	struct zr_manifest_hdr hdr;
	FILE *out = stdout;
	int rc = EXIT_INTERNAL, keep = 0;

	memset(&r, 0, sizeof (r));
	r.o = *o;
	if (geteuid() != 0) {
		(void) fprintf(stderr, "zfs_rebase: must run as root\n");
		return (EXIT_PRECOND);
	}
	/*
	 * The tag every hold is filed under. Two runs over the same
	 * snapshots must not collide, and each takes its own tag, so
	 * one finishing does not release the other's hold.
	 */
	(void) snprintf(r.holdtag, sizeof (r.holdtag), "zfs_rebase-%lld-%ld",
	    (long long)time(NULL), (long)getpid());
	signals_install(saved);
	if (zr_zfs_open(&r.zfs, r.holdtag, r.err, sizeof (r.err)) != 0) {
		rc = fail(&r, EXIT_PRECOND, "libzfs");
		goto done;
	}
	if (preconditions(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "precondition");
		goto done;
	}

	/* 1. hold all three, so that none can go while the run reads it */
	if (zr_zfs_hold(r.zfs, r.base, r.err, sizeof (r.err)) != 0 ||
	    zr_zfs_hold(r.zfs, o->from, r.err, sizeof (r.err)) != 0 ||
	    zr_zfs_hold(r.zfs, o->onto, r.err, sizeof (r.err)) != 0) {
		rc = fail(&r, EXIT_PRECOND, "hold");
		goto done;
	}

	/* 2. the working clone, read-only from birth, unless a dry run */
	if (!o->dryrun) {
		if (make_rundir(&r) != 0) {
			rc = fail(&r, EXIT_PRECOND, "run directory");
			goto done;
		}
		(void) snprintf(r.manpath, sizeof (r.manpath), "%s",
		    o->outpath != NULL ? o->outpath : "");
		if (r.manpath[0] == '\0')
			(void) snprintf(r.manpath, sizeof (r.manpath),
			    "%s/manifest", r.rundir);
		if (zr_zfs_clone(r.zfs, o->onto, o->result, r.workmnt,
		    r.manpath, r.err, sizeof (r.err)) != 0) {
			rc = fail(&r, EXIT_PRECOND, "clone");
			goto done;
		}
		r.cloned = 1;
	} else if (o->outpath != NULL) {
		(void) snprintf(r.manpath, sizeof (r.manpath), "%s",
		    o->outpath);
	}

	/* 3. read, 4. decide */
	if (read_trees(&r) != 0) {
		rc = fail(&r, zr_apply_stop != 0 ? EXIT_INTERNAL :
		    EXIT_PRECOND, "read");
		goto done;
	}
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

	/* 5. the manifest */
	hdr.base = r.base;
	hdr.from = o->from;
	hdr.onto = o->onto;
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
	record_manifest(&r);
	if (r.d.zd_nconflicts != 0) {
		(void) fprintf(stderr, "zfs_rebase: %u conflict%s; nothing "
		    "applied\n", r.d.zd_nconflicts,
		    r.d.zd_nconflicts == 1 ? "" : "s");
		if (!o->dryrun) {
			set_state(&r, "conflicts");
			manifest_note(&r);
			abort_hint(&r);
			keep = 1;
		}
		rc = EXIT_CONFLICTS;
		goto done;
	}
	if (o->dryrun) {
		rc = EXIT_CLEAN;
		goto done;
	}

	/* 6. apply, 7. verify */
	if (stopped(&r) != 0) {
		rc = fail(&r, EXIT_INTERNAL, "apply");
		goto done;	/* nothing written yet: the clone goes */
	}
	if (apply_manifest(&r, &hdr) != 0) {
		keep = 1;
		rc = fail(&r, EXIT_INTERNAL, "apply");
		set_state(&r, zr_apply_stop != 0 ? "interrupted" : "failed");
		manifest_note(&r);
		abort_hint(&r);
		goto done;
	}
	if (verify_clone(&r) != 0) {
		keep = 1;
		rc = fail(&r, EXIT_INTERNAL, "verify");
		set_state(&r, zr_apply_stop != 0 ? "interrupted" : "failed");
		manifest_note(&r);
		abort_hint(&r);
		goto done;
	}
	set_state(&r, "applied");
	(void) fprintf(stderr, "zfs_rebase: %s is the rebased tree, read-only "
	    "at %s\n", o->result, r.workmnt);
	manifest_note(&r);
	keep = 1;
	rc = EXIT_CLEAN;
done:
	teardown(&r, keep);
	signals_restore(saved);
	return (rc);
}

/*
 * --abort: take one run's leavings away and nothing else. A hard
 * kill can leave the result clone and the manifest behind -- the
 * holds go by themselves -- and this is how they go.
 *
 * The refusal is the point of the marker. Only a dataset that
 * carries zfs_rebase:state is destroyed, so a mistyped or a
 * remembered-wrong name cannot cost the user a dataset of their own;
 * every state is fair game, "applying" included, because a process
 * killed part way through the apply leaves exactly that and this is
 * what clears it. Nothing is removed recursively: the one file this
 * unlinks is the manifest the run itself recorded, and every
 * directory goes by rmdir, which will not touch one that is not
 * empty.
 */
int
zr_abort(const char *result, int verbose)
{
	char manifest[ZR_NAME_MAX], dir[ZR_NAME_MAX];
	char state[64], err[512];
	struct zr_zfs *z = NULL;
	struct stat sb;
	int rc = EXIT_INTERNAL, hasdir, hasds, hasman = 0;

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
	if (zr_zfs_open(&z, "zfs_rebase-abort", err, sizeof (err)) != 0) {
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
		int got = zr_zfs_get_user(z, result, ZR_PROP_STATE, state,
		    sizeof (state), err, sizeof (err));

		if (got < 0) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n", result,
			    err);
			rc = EXIT_PRECOND;
			goto done;
		}
		if (got == 0) {
			(void) fprintf(stderr, "zfs_rebase: %s is not a "
			    "zfs_rebase result; nothing was touched\n",
			    result);
			rc = EXIT_PRECOND;
			goto done;
		}
		if (verbose)
			(void) fprintf(stderr, "zfs_rebase: %s is at %s\n",
			    result, state);
		hasman = zr_zfs_get_user(z, result, ZR_PROP_MANIFEST, manifest,
		    sizeof (manifest), err, sizeof (err));
		if (hasman < 0) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
			    ZR_PROP_MANIFEST, err);
			hasman = 0;
		}
		if (hasman > 0 && unlink(manifest) != 0) {
			if (errno != ENOENT)
				(void) fprintf(stderr, "zfs_rebase: %s: %s\n",
				    manifest, strerror(errno));
			hasman = 0;
		}
		if (zr_zfs_destroy(z, result, err, sizeof (err)) != 0) {
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    result, err);
			goto done;
		}
		(void) fprintf(stderr, "zfs_rebase: destroyed %s\n", result);
		if (hasman > 0)
			(void) fprintf(stderr, "zfs_rebase: removed the "
			    "manifest %s\n", manifest);
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
