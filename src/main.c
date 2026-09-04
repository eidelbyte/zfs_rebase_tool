/*
 * zfs_rebase: standalone rebase of one ZFS filesystem onto another.
 *
 * This file is the driver. The --posix mode, three plain directories
 * in and a manifest out, is complete and is how the pipeline runs
 * where there is no ZFS. The real mode (holds on the three snapshots
 * the user names, a clone under the name the user chooses, zfs diff,
 * apply) is zr_run in run.c and exists only in the FreeBSD build.
 * --build-fixture is a test aid that materializes a fixture's three
 * trees.
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
	"usage: zfs_rebase [-n] [-p] [-v] [-o FILE] [--verify] "
	"--from SNAP --onto SNAP --result DATASET\n"
	"       zfs_rebase --continue [--verify] [-v] --result DATASET\n"
	"       zfs_rebase --restart [-v] --result DATASET\n"
	"       zfs_rebase --abort [-v] --result DATASET\n"
	"       zfs_rebase --verify [-v] --result DATASET\n"
	"       zfs_rebase --posix [-p] [-o FILE] BASEDIR FROMDIR ONTODIR\n"
	"       zfs_rebase --build-fixture FIXTURE DIR\n"
	"       zfs_rebase --edit-fixture FIXTURE TREE DIR\n"
	"  --from      the snapshot whose changes are replayed (--off-of)\n"
	"  --onto      the snapshot they are replayed onto; the base is\n"
	"              the branch point of the two, which the tool works\n"
	"              out\n"
	"  --result    the dataset the rebased clone is created as, and\n"
	"              for every verb the dataset carrying the record; a\n"
	"              snapshot name is taken as its dataset\n"
	"  --continue  take the rebase on from the gate it stopped at\n"
	"  --restart   destroy the result, clone it again from the\n"
	"              recorded onto snapshot and apply the recorded\n"
	"              manifest from the first gate\n"
	"  --abort     release the holds that result records, destroy it,\n"
	"              remove the manifest it recorded and its run\n"
	"              directory, and nothing else\n"
	"  --verify    alone on a result, report what is done, pending,\n"
	"              blocked, drifted or unchecked and write nothing;\n"
	"              with a rebase or a --continue, ask for the final\n"
	"              check, which --continue also makes a repair\n"
	"  -n   dry run: write the manifest, create nothing, hold nothing\n"
	"  -p   permissive-merge mode\n"
	"  -v   report counts on stderr\n"
	"  -o   write the manifest to FILE\n"
	"exit: 0 done, 1 stopped at conflicts, 2 refused, 3 failed\n";

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
 * One long option with its value, written either way round: --name
 * VALUE or --name=VALUE. Returns 1 with *val set and *i advanced
 * over the value, 0 when argv[*i] is some other option, and -1 when
 * it is this one with nothing to give.
 */
static int
longopt(int argc, char **argv, int *i, const char *name, const char **val)
{
	const char *a = argv[*i];
	size_t n = strlen(name);

	if (strncmp(a, name, n) != 0)
		return (0);
	if (a[n] == '=') {
		*val = a + n + 1;
		return ((*val)[0] == '\0' ? -1 : 1);
	}
	if (a[n] != '\0')
		return (0);		/* --namesomething, not this one */
	if (*i + 1 >= argc)
		return (-1);
	*val = argv[++(*i)];
	return (1);
}

/* --posix: the options are leading, the three directories follow. */
static int
main_posix(int argc, char **argv)
{
	zr_mode_t mode = ZR_MODE_STRICT;
	const char *outpath = NULL;
	int i;

	for (i = 2; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-p") == 0)
			mode = ZR_MODE_PERMISSIVE;
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			outpath = argv[++i];
		else
			return (die_usage());
	}
	if (argc - i != 3)
		return (die_usage());
	return (run_posix(argv[i], argv[i + 1], argv[i + 2], mode, outpath));
}

int
main(int argc, char **argv)
{
	const char *from = NULL, *onto = NULL, *result = NULL;
	const char *outpath = NULL, *v;
	zr_mode_t mode = ZR_MODE_STRICT;
	int dryrun = 0, verbose = 0, abrt = 0, verify = 0, cont = 0, rest = 0;
	struct zr_run_opts ro;
	int i, t;

	if (argc >= 2 && strcmp(argv[1], "--build-fixture") == 0) {
		if (argc != 4)
			return (die_usage());
		return (build_fixture(argv[2], argv[3]));
	}
	if (argc >= 2 && strcmp(argv[1], "--edit-fixture") == 0) {
		if (argc != 5)
			return (die_usage());
		return (edit_fixture(argv[2], argv[3], argv[4]));
	}
	if (argc >= 2 && strcmp(argv[1], "--posix") == 0)
		return (main_posix(argc, argv));
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0) {
			mode = ZR_MODE_PERMISSIVE;
		} else if (strcmp(argv[i], "-n") == 0) {
			dryrun = 1;
		} else if (strcmp(argv[i], "-v") == 0) {
			verbose = 1;
		} else if (strcmp(argv[i], "--abort") == 0) {
			abrt = 1;
		} else if (strcmp(argv[i], "--continue") == 0) {
			cont = 1;
		} else if (strcmp(argv[i], "--restart") == 0) {
			rest = 1;
		} else if (strcmp(argv[i], "--verify") == 0) {
			verify = 1;
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			outpath = argv[++i];
		} else if ((t = longopt(argc, argv, &i, "--from", &v)) != 0 ||
		    (t = longopt(argc, argv, &i, "--off-of", &v)) != 0) {
			if (t < 0)
				return (die_usage());
			from = v;
		} else if ((t = longopt(argc, argv, &i, "--onto", &v)) != 0) {
			if (t < 0)
				return (die_usage());
			onto = v;
		} else if ((t = longopt(argc, argv, &i, "--result",
		    &v)) != 0) {
			if (t < 0)
				return (die_usage());
			result = v;
		} else {
			return (die_usage());
		}
	}
	/*
	 * The verbs on a result. Each goes alone -- one --result, -v,
	 * and nothing else -- and no two of them go together.
	 * --verify is the odd one: with a rebase or a --continue it is
	 * a flag, asking for the final check, and on its own with a
	 * --result it is the verb that reports and writes nothing.
	 */
	if (abrt + cont + rest > 1)
		return (die_usage());
	if (abrt != 0 || cont != 0 || rest != 0 ||
	    (verify != 0 && from == NULL && onto == NULL)) {
		if (result == NULL || from != NULL || onto != NULL ||
		    outpath != NULL || dryrun != 0 || mode != ZR_MODE_STRICT)
			return (die_usage());
		if (abrt != 0 || rest != 0) {
			if (verify != 0)
				return (die_usage());
			return (abrt != 0 ? zr_abort(result, verbose) :
			    zr_restart(result, verbose));
		}
		if (cont != 0)
			return (zr_continue(result, verify, verbose));
		return (zr_report(result, verbose));
	}
	if (from == NULL || onto == NULL || (result == NULL && !dryrun))
		return (die_usage());
	if (strchr(from, '@') == NULL || strchr(onto, '@') == NULL) {
		(void) fprintf(stderr, "zfs_rebase: --from and --onto must "
		    "name snapshots, as pool/dataset@snapshot\n");
		return (EXIT_PRECOND);
	}
	ro.from = from;
	ro.onto = onto;
	ro.result = dryrun ? NULL : result;
	ro.outpath = outpath;
	ro.mode = mode;
	ro.dryrun = dryrun;
	ro.verify = verify;
	ro.verbose = verbose;
	return (zr_run(&ro));
}
