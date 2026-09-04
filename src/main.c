/*
 * zfs_rebase: standalone rebase of one ZFS filesystem onto another.
 *
 * This file is the driver. The --posix mode, three plain directories
 * in and a manifest out, is complete and is how the pipeline runs
 * where there is no ZFS. The real mode (holds on the three snapshots
 * the user names, a clone under the name the user chooses, apply) is
 * zr_run in run.c and exists only in the FreeBSD build.
 * --build-fixture is a test aid that materializes a fixture's three
 * trees. The command line itself is args.c: this file dispatches on
 * what that parse left and does nothing else with argv.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "args.h"
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
	"usage: zfs_rebase [-p] [-v] [--manifest FILE] [--verify] "
	    "[--overwrite]\n"
	"                  [--allow-unrelated [--base SNAP]]\n"
	"                  [--take-onto | --take-from] [--no-gui] "
	    "[--no-merge]\n"
	"                  --from SNAP|DATASET --onto SNAP|DATASET --result "
	    "NAME\n"
	"       zfs_rebase --dry-run [-p] [--manifest FILE]\n"
	"                  --from SNAP|DATASET --onto SNAP|DATASET\n"
	"       zfs_rebase --continue [--verify] [--no-gui] [--no-merge]\n"
	"                  --result NAME\n"
	"       zfs_rebase --restart --result NAME\n"
	"       zfs_rebase --abort --result NAME\n"
	"       zfs_rebase --verify --result NAME\n"
	"       zfs_rebase --posix [-p] [-o FILE] BASEDIR FROMDIR ONTODIR\n"
	"       zfs_rebase --build-fixture FIXTURE DIR\n"
	"       zfs_rebase --edit-fixture FIXTURE TREE DIR\n"
	"every flag has both forms, and the two parse to the same run:\n"
	"  -f, --from SNAP|DS      the side whose changes are replayed "
	    "(--off-of)\n"
	"  -t, --onto SNAP|DS      the side they go onto (--to); it sets the "
	    "form\n"
	"  -r, --result NAME       the name the run makes; a verb's rebase\n"
	"  -p, --permissive-merge  permissive merge; strict is the default\n"
	"  -v, --verbose           counts and steps on stderr\n"
	"  -o, --manifest FILE     where the manifest goes, resolution beside "
	    "it\n"
	"  -V, --verify            report the final check; it never repairs\n"
	"  -O, --take-onto         answer every conflict of the skeleton onto\n"
	"  -F, --take-from         answer every conflict of the skeleton from\n"
	"  -G, --no-gui            go on at the conflicts gate when the\n"
	"                          resolution is complete and stop when it is\n"
	"                          not: the only behavior while there is no\n"
	"                          picker\n"
	"  -M, --no-merge          stop at the conflicts gate however the\n"
	"                          resolution is answered; an error past it\n"
	"  -c, --continue          take the rebase on from the gate it left\n"
	"  -R, --restart           the result back as onto was, applied again\n"
	"  -a, --abort             the rebase undone, as if it never happened\n"
	"  -n, --dry-run           the manifest only; --result is ignored\n"
	"  -w, --overwrite         in-place form: replace a record that is "
	    "done\n"
	"  -u, --allow-unrelated   no derivation of the base, and no pruning\n"
	"  -b, --base SNAP         with --allow-unrelated only: the base\n"
	"--posix, --build-fixture and --edit-fixture are the project's own\n"
	"test aids: they are long only and take the first argument position.\n"
	"exit: 0 done, 1 stopped at conflicts, 2 refused, 3 failed\n";

/*
 * A command that was not understood: the one line saying what was
 * wrong with it, and then the whole of the usage, since the reader
 * has just been told a flag is not where they thought it was.
 */
static int
die_usage(const char *why)
{
	if (why != NULL && why[0] != '\0')
		(void) fprintf(stderr, "zfs_rebase: %s\n", why);
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

/*
 * --edit-fixture: DIR, which already holds one built tree, made into
 * the fixture's TREE -- base, from or onto -- by the smallest set of
 * edits, so that everything the two trees agree on keeps its inode
 * and its ctime. The counts go to stdout, one line, six numbers.
 */
static int
edit_fixture(const char *path, const char *tree, const char *dir)
{
	static const char *sub[3] = { "base", "from", "onto" };
	struct zr_fixture_edit_stats st;
	struct zr_fixture *fx;
	char err[512];
	int i, which = -1;

	for (i = 0; i < 3; i++) {
		if (strcmp(tree, sub[i]) == 0)
			which = i;
	}
	if (which < 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: the tree to edit to "
		    "is base, from or onto\n", tree);
		return (EXIT_PRECOND);
	}
	if (zr_fixture_load(path, &fx, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: %s: %s\n", path, err);
		return (EXIT_PRECOND);
	}
	if (zr_fixture_edit(fx, (enum zr_fixture_tree)which, dir, &st, err,
	    sizeof (err)) != 0) {
		(void) fprintf(stderr, "zfs_rebase: edit %s: %s\n", dir, err);
		zr_fixture_free(fx);
		return (EXIT_PRECOND);
	}
	zr_fixture_free(fx);
	(void) printf("removed %llu created %llu rewritten %llu relinked %llu"
	    " attrs %llu untouched %llu\n",
	    (unsigned long long)st.ze_removed,
	    (unsigned long long)st.ze_created,
	    (unsigned long long)st.ze_rewritten,
	    (unsigned long long)st.ze_relinked,
	    (unsigned long long)st.ze_attrs,
	    (unsigned long long)st.ze_untouched);
	return (EXIT_CLEAN);
}

/*
 * The command line is args.c's; what is left here is the dispatch
 * over what it parsed. Nothing below reads argv again.
 *
 * --no-gui is parsed, refused where it does not belong, and goes no
 * further: what it asks of the conflicts gate -- go on when the
 * resolution is complete, stop when it is not -- is what the gate
 * does while there is no picker to launch, so there is nothing for
 * the run or the verb to do differently under it.
 */
int
main(int argc, char **argv)
{
	struct zr_args a;
	struct zr_run_opts ro;
	char err[512];

	if (zr_args_parse(argc, argv, &a, err, sizeof (err)) != 0)
		return (die_usage(err));
	switch (a.za_verb) {
	case ZR_VERB_BUILD_FIXTURE:
		return (build_fixture(a.za_arg[0], a.za_arg[1]));
	case ZR_VERB_EDIT_FIXTURE:
		return (edit_fixture(a.za_arg[0], a.za_arg[1], a.za_arg[2]));
	case ZR_VERB_POSIX:
		return (run_posix(a.za_arg[0], a.za_arg[1], a.za_arg[2],
		    a.za_mode, a.za_manifest));
	case ZR_VERB_CONTINUE:
		return (zr_continue(a.za_result, a.za_verify, a.za_nomerge,
		    a.za_verbose));
	case ZR_VERB_RESTART:
		return (zr_restart(a.za_result, a.za_verbose));
	case ZR_VERB_ABORT:
		return (zr_abort(a.za_result, a.za_verbose));
	case ZR_VERB_REPORT:
		return (zr_report(a.za_result, a.za_verbose));
	case ZR_VERB_RUN:
	default:
		break;
	}
	/*
	 * Each side is a snapshot or a dataset -- an argument with an
	 * '@' in it is a snapshot -- and --onto decides the form.
	 * --result is the clone's name in one form and the pre-apply
	 * snapshot's in the other, and a dry run ignores it, since it
	 * creates nothing that would need a name.
	 */
	ro.from = a.za_from;
	ro.onto = a.za_onto;
	ro.result = a.za_dryrun ? NULL : a.za_result;
	ro.outpath = a.za_manifest;
	ro.base = a.za_base;
	ro.mode = a.za_mode;
	ro.dryrun = a.za_dryrun;
	ro.overwrite = a.za_overwrite;
	ro.unrelated = a.za_unrelated;
	ro.verify = a.za_verify;
	ro.takeonto = a.za_takeonto;
	ro.takefrom = a.za_takefrom;
	ro.nomerge = a.za_nomerge;
	ro.verbose = a.za_verbose;
	return (zr_run(&ro));
}
