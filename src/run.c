/*
 * The real run: snapshots, holds, a read-only working clone, the walk
 * of three snapshots through .zfs/snapshot, zfs diff for the unchanged
 * set, decide, manifest, apply, re-walk, release. Everything here is
 * library calls; nothing is exec'd. The ZFS operations themselves are
 * in zfsops.c and exist only in the FreeBSD build.
 */

#include <errno.h>
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

struct run {
	struct zr_run_opts	o;
	struct zr_zfs		*zfs;
	char			runid[32];
	char			snapname[48];		/* rebase-<runid> */
	char			fromsnap[ZR_NAME_MAX];	/* from@rebase-id */
	char			ontosnap[ZR_NAME_MAX];
	char			clone[ZR_NAME_MAX];	/* onto-rebase-id */
	char			workmnt[ZR_NAME_MAX];	/* WORKDIR/<runid> */
	char			basemnt[ZR_NAME_MAX];	/* mountpoints */
	char			frommnt[ZR_NAME_MAX];
	char			ontomnt[ZR_NAME_MAX];
	int			snapped, held, cloned, walked;
	struct zr_names		*names;
	struct zr_walk		wb, wf, wo;
	struct zr_oracle	*oracle;
	struct zr_decision	d;
	char			err[512];
};

static int
fail(struct run *r, int code, const char *what)
{
	(void) fprintf(stderr, "zfs_rebase: %s: %s\n", what, r->err);
	return (code);
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

/* Every dataset must be mounted, and the three must agree on names. */
static int
preconditions(struct run *r)
{
	static const char *props[] = { "casesensitivity", "normalization" };
	static const char *want[] = { "sensitive", "none" };
	const char *ds[3];
	char baseds[ZR_NAME_MAX], buf[64];
	int i, p;

	dataset_of(r->o.base, baseds, sizeof (baseds));
	ds[0] = baseds;
	ds[1] = r->o.from;
	ds[2] = r->o.onto;
	if (strchr(r->o.base, '@') == NULL) {
		(void) snprintf(r->err, sizeof (r->err),
		    "%s is not a snapshot", r->o.base);
		return (-1);
	}
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
	if (zr_zfs_get(r->zfs, ds[0], "mountpoint", r->basemnt,
	    sizeof (r->basemnt), r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get(r->zfs, ds[1], "mountpoint", r->frommnt,
	    sizeof (r->frommnt), r->err, sizeof (r->err)) != 0 ||
	    zr_zfs_get(r->zfs, ds[2], "mountpoint", r->ontomnt,
	    sizeof (r->ontomnt), r->err, sizeof (r->err)) != 0)
		return (-1);
	return (0);
}

/* WORKDIR/<runid>, root-only, so nothing else can look in. */
static int
workdir(struct run *r)
{
	if (mkdir(WORKDIR, 0700) != 0 && errno != EEXIST) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", WORKDIR,
		    strerror(errno));
		return (-1);
	}
	(void) snprintf(r->workmnt, sizeof (r->workmnt), "%s/%s", WORKDIR,
	    r->runid);
	if (mkdir(r->workmnt, 0700) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "%s: %s", r->workmnt,
		    strerror(errno));
		return (-1);
	}
	return (0);
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
	snapdir(path, sizeof (path), r->basemnt, r->o.base);
	if (zr_walk(path, r->names, &r->wb, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 1;
	snapdir(path, sizeof (path), r->frommnt, r->snapname);
	if (zr_walk(path, r->names, &r->wf, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 2;
	snapdir(path, sizeof (path), r->ontomnt, r->snapname);
	if (zr_walk(path, r->names, &r->wo, r->err, sizeof (r->err)) != 0)
		return (-1);
	r->walked = 3;

	if (zr_oracle_init(&r->oracle, &r->wb, &r->wf, &r->wo) != 0) {
		(void) snprintf(r->err, sizeof (r->err), "out of memory");
		return (-1);
	}
	/* zfs diff from the base snapshot to each side: the unchanged set */
	if (zr_zfs_diff(r->zfs, r->o.base, r->fromsnap, r->frommnt, &df,
	    r->err, sizeof (r->err)) != 0)
		return (-1);
	marked = zr_diff_apply_unchanged(&df, &r->wb, &r->wf, 1, r->oracle);
	zr_diff_fini(&df);
	if (zr_zfs_diff(r->zfs, r->o.base, r->ontosnap, r->ontomnt, &dfo,
	    r->err, sizeof (r->err)) != 0)
		return (-1);
	marked += zr_diff_apply_unchanged(&dfo, &r->wb, &r->wo, 2, r->oracle);
	zr_diff_fini(&dfo);
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
	if (zr_zfs_set_readonly(r->zfs, r->clone, 0, r->err,
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
	if (zr_zfs_set_readonly(r->zfs, r->clone, 1, r->err,
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
	if (r->cloned && !keep_clone) {
		if (zr_zfs_destroy(r->zfs, r->clone, r->err,
		    sizeof (r->err)) != 0)
			(void) fprintf(stderr, "zfs_rebase: destroy %s: %s\n",
			    r->clone, r->err);
		(void) rmdir(r->workmnt);
	}
	/* holds die with the cleanup descriptor, snapshots stay */
	if (r->zfs != NULL)
		zr_zfs_close(r->zfs);
}

int
zr_run(const struct zr_run_opts *o)
{
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
	(void) snprintf(r.runid, sizeof (r.runid), "%lld-%ld",
	    (long long)time(NULL), (long)getpid());
	(void) snprintf(r.snapname, sizeof (r.snapname), "rebase-%s",
	    r.runid);
	if (zr_zfs_open(&r.zfs, r.snapname, r.err, sizeof (r.err)) != 0)
		return (fail(&r, EXIT_PRECOND, "libzfs"));
	if (preconditions(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "precondition");
		goto done;
	}
	(void) snprintf(r.fromsnap, sizeof (r.fromsnap), "%s@%s", o->from,
	    r.snapname);
	(void) snprintf(r.ontosnap, sizeof (r.ontosnap), "%s@%s", o->onto,
	    r.snapname);
	(void) snprintf(r.clone, sizeof (r.clone), "%s-%s", o->onto,
	    r.snapname);

	/* 1. snapshot both sides; 2. hold all three */
	if (zr_zfs_snapshot(r.zfs, o->from, r.snapname, r.err,
	    sizeof (r.err)) != 0 ||
	    zr_zfs_snapshot(r.zfs, o->onto, r.snapname, r.err,
	    sizeof (r.err)) != 0) {
		rc = fail(&r, EXIT_PRECOND, "snapshot");
		goto done;
	}
	r.snapped = 1;
	if (zr_zfs_hold(r.zfs, o->base, r.err, sizeof (r.err)) != 0 ||
	    zr_zfs_hold(r.zfs, r.fromsnap, r.err, sizeof (r.err)) != 0 ||
	    zr_zfs_hold(r.zfs, r.ontosnap, r.err, sizeof (r.err)) != 0) {
		rc = fail(&r, EXIT_PRECOND, "hold");
		goto done;
	}
	r.held = 1;

	/* 3. the working clone, read-only from birth, unless a dry run */
	if (!o->dryrun) {
		if (workdir(&r) != 0) {
			rc = fail(&r, EXIT_PRECOND, "workdir");
			goto done;
		}
		if (zr_zfs_clone(r.zfs, r.ontosnap, r.clone, r.workmnt, r.err,
		    sizeof (r.err)) != 0) {
			rc = fail(&r, EXIT_PRECOND, "clone");
			goto done;
		}
		r.cloned = 1;
	}

	/* 4. read, 5. decide */
	if (read_trees(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "read");
		goto done;
	}
	if (zr_decide(&r.wb.zw_tree, &r.wf.zw_tree, &r.wo.zw_tree, o->mode,
	    &r.d) != 0) {
		(void) snprintf(r.err, sizeof (r.err), "out of memory");
		rc = fail(&r, EXIT_INTERNAL, "decide");
		goto done;
	}

	if (!o->dryrun && securelevel_guard(&r) != 0) {
		rc = fail(&r, EXIT_PRECOND, "precondition");
		goto done;
	}

	/* 6. the manifest */
	hdr.base = o->base;
	hdr.from = r.fromsnap;
	hdr.onto = r.ontosnap;
	hdr.mode = o->mode;
	if (o->outpath != NULL) {
		out = fopen(o->outpath, "w");
		if (out == NULL) {
			(void) snprintf(r.err, sizeof (r.err), "%s: %s",
			    o->outpath, strerror(errno));
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
	if (r.d.zd_nconflicts != 0) {
		(void) fprintf(stderr, "zfs_rebase: %u conflict%s; nothing "
		    "applied%s\n", r.d.zd_nconflicts,
		    r.d.zd_nconflicts == 1 ? "" : "s",
		    o->dryrun ? "" : "; the clone and the holds are kept");
		keep = !o->dryrun;
		rc = EXIT_CONFLICTS;
		goto done;
	}
	if (o->dryrun) {
		rc = EXIT_CLEAN;
		goto done;
	}

	/* 7. apply, 8. verify */
	if (apply_manifest(&r, &hdr) != 0) {
		keep = 1;
		rc = fail(&r, EXIT_INTERNAL, "apply");
		goto done;
	}
	if (verify_clone(&r) != 0) {
		keep = 1;
		rc = fail(&r, EXIT_INTERNAL, "verify");
		goto done;
	}
	(void) fprintf(stderr, "zfs_rebase: %s is the rebased tree, read-only "
	    "at %s\n", r.clone, r.workmnt);
	keep = 1;
	rc = EXIT_CLEAN;
done:
	teardown(&r, keep);
	return (rc);
}
