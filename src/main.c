/*
 * zfs_rebase: standalone rebase of one ZFS filesystem onto another.
 *
 * This file is the driver. The --posix mode, three plain directories
 * in and a manifest out, is complete and is how the pipeline runs
 * where there is no ZFS. The real mode (snapshots, holds, a clone,
 * zfs diff, apply) is zr_run in run.c and exists only in the FreeBSD
 * build. --build-fixture is a test aid that materializes a fixture's
 * three trees.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "decide.h"
#include "fixture.h"
#include "manifest.h"
#include "name.h"
#include "run.h"
#include "walk.h"
#include "yellow.h"

#define	EXIT_CLEAN	0
#define	EXIT_CONFLICTS	1
#define	EXIT_PRECOND	2
#define	EXIT_INTERNAL	3

static const char usage[] =
	"usage: zfs_rebase [-n] [-p] [-v] [-o FILE] BASE@SNAP FROM ONTO\n"
	"       zfs_rebase --posix [-p] [-o FILE] BASEDIR FROMDIR ONTODIR\n"
	"       zfs_rebase --build-fixture FIXTURE DIR\n"
	"  -n   dry run: write the manifest, create no clone, apply nothing\n"
	"  -p   permissive-merge mode\n"
	"  -v   report counts on stderr\n"
	"  -o   write the manifest to FILE instead of stdout\n";

static int
die_usage(void)
{
	(void) fputs(usage, stderr);
	return (EXIT_PRECOND);
}

/*
 * --posix: walk three directories, assign content, decide, emit.
 * Exit 0 clean, 1 with conflicts, 2 on a bad tree, 3 on an internal
 * failure. The header names the directories as given.
 */
static int
run_posix(const char *bdir, const char *fdir, const char *odir,
    zr_mode_t mode, const char *outpath)
{
	struct zr_names *names;
	struct zr_walk wb, wf, wo;
	struct zr_oracle *oracle = NULL;
	struct zr_decision d;
	struct zr_manifest_hdr hdr;
	FILE *out = stdout;
	char err[512];
	int rc = EXIT_INTERNAL, walked = 0;

	memset(&d, 0, sizeof (d));
	names = zr_names_create();
	if (names == NULL) {
		(void) fprintf(stderr, "zfs_rebase: out of memory\n");
		return (EXIT_INTERNAL);
	}
	if (zr_walk(bdir, names, &wb, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: base: %s\n", err);
		rc = EXIT_PRECOND;
		goto done;
	}
	walked = 1;
	if (zr_walk(fdir, names, &wf, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: from: %s\n", err);
		rc = EXIT_PRECOND;
		goto done;
	}
	walked = 2;
	if (zr_walk(odir, names, &wo, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: onto: %s\n", err);
		rc = EXIT_PRECOND;
		goto done;
	}
	walked = 3;
	if (zr_oracle_init(&oracle, &wb, &wf, &wo) != 0 ||
	    zr_oracle_assign(oracle, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: content: %s\n",
		    oracle == NULL ? "out of memory" : err);
		goto done;
	}
	if (zr_decide(&wb.zw_tree, &wf.zw_tree, &wo.zw_tree, mode,
	    &d) != 0) {
		(void) fprintf(stderr, "zfs_rebase: decide: out of memory\n");
		goto done;
	}
	if (outpath != NULL) {
		out = fopen(outpath, "w");
		if (out == NULL) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n", outpath,
			    strerror(errno));
			goto done;
		}
	}
	hdr.base = bdir;
	hdr.from = fdir;
	hdr.onto = odir;
	hdr.mode = mode;
	if (zr_manifest_emit(out, &hdr, &wb.zw_tree, &wf.zw_tree,
	    &wo.zw_tree, &d) != 0) {
		(void) fprintf(stderr, "zfs_rebase: cannot write manifest\n");
		goto done;
	}
	if (out != stdout && fclose(out) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", outpath,
		    strerror(errno));
		out = stdout;
		goto done;
	}
	rc = d.zd_nconflicts ? EXIT_CONFLICTS : EXIT_CLEAN;
done:
	zr_decision_fini(&d);
	if (oracle != NULL)
		zr_oracle_fini(oracle);
	if (walked >= 3)
		zr_walk_fini(&wo);
	if (walked >= 2)
		zr_walk_fini(&wf);
	if (walked >= 1)
		zr_walk_fini(&wb);
	zr_names_destroy(names);
	return (rc);
}

/* --build-fixture: DIR/base, DIR/from, DIR/onto and DIR/expect. */
static int
build_fixture(const char *path, const char *dir)
{
	static const char *sub[3] = { "base", "from", "onto" };
	struct zr_fixture *fx;
	char err[512], buf[4096];
	const char *expect;
	int i;

	if (zr_fixture_load(path, &fx, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", path, err);
		return (EXIT_PRECOND);
	}
	for (i = 0; i < 3; i++) {
		(void) snprintf(buf, sizeof (buf), "%s/%s", dir, sub[i]);
		if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n", buf,
			    strerror(errno));
			zr_fixture_free(fx);
			return (EXIT_PRECOND);
		}
		if (zr_fixture_build(fx, (enum zr_fixture_tree)i, buf) != 0) {
			(void) fprintf(stderr, "zfs_rebase: build %s: %s\n",
			    buf, strerror(errno));
			zr_fixture_free(fx);
			return (EXIT_PRECOND);
		}
	}
	expect = zr_fixture_expect(fx);
	if (expect != NULL) {
		FILE *fp;

		(void) snprintf(buf, sizeof (buf), "%s/expect", dir);
		fp = fopen(buf, "w");
		if (fp == NULL || fputs(expect, fp) == EOF ||
		    fclose(fp) != 0) {
			(void) fprintf(stderr, "zfs_rebase: %s: %s\n", buf,
			    strerror(errno));
			zr_fixture_free(fx);
			return (EXIT_INTERNAL);
		}
	}
	zr_fixture_free(fx);
	return (EXIT_CLEAN);
}

int
main(int argc, char **argv)
{
	zr_mode_t mode = ZR_MODE_STRICT;
	const char *outpath = NULL;
	int posix = 0, dryrun = 0, verbose = 0, i;
	struct zr_run_opts ro;

	if (argc >= 2 && strcmp(argv[1], "--build-fixture") == 0) {
		if (argc != 4)
			return (die_usage());
		return (build_fixture(argv[2], argv[3]));
	}
	i = 1;
	if (argc >= 2 && strcmp(argv[1], "--posix") == 0) {
		posix = 1;
		i = 2;
	}
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-p") == 0) {
			mode = ZR_MODE_PERMISSIVE;
		} else if (strcmp(argv[i], "-n") == 0) {
			dryrun = 1;
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			outpath = argv[++i];
		} else {
			return (die_usage());
		}
	}
	if (argc - i != 3)
		return (die_usage());
	if (posix) {
		if (dryrun)
			return (die_usage());
		return (run_posix(argv[i], argv[i + 1], argv[i + 2], mode,
		    outpath));
	}
	ro.base = argv[i];
	ro.from = argv[i + 1];
	ro.onto = argv[i + 2];
	ro.outpath = outpath;
	ro.mode = mode;
	ro.dryrun = dryrun;
	ro.verbose = verbose;
	return (zr_run(&ro));
}
