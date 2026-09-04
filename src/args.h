/* The command line: one table of flags, one struct, one refusal. */

#ifndef	ZR_ARGS_H
#define	ZR_ARGS_H

#include <stddef.h>

#include "decide.h"

/*
 * What the command asks for. The fresh run is the default and the
 * one with no word of its own; --continue, --restart and --abort are
 * verbs on a rebase that already exists, and --verify is a verb when
 * it stands alone on a --result and a flag otherwise. The last three
 * are the project's own harness aids: they take the first argument
 * position, they have no short form, and they are no part of
 * ordinary use.
 */
enum zr_verb {
	ZR_VERB_RUN = 0,
	ZR_VERB_CONTINUE,
	ZR_VERB_RESTART,
	ZR_VERB_ABORT,
	ZR_VERB_REPORT,			/* --verify alone on a result */
	ZR_VERB_POSIX,
	ZR_VERB_BUILD_FIXTURE,
	ZR_VERB_EDIT_FIXTURE
};

/*
 * The whole command line, parsed and nothing more: no file is
 * opened, no dataset is looked at and nothing is decided here, so
 * every refusal below is reachable on a machine with no ZFS in it.
 * Every string points into argv, which outlives the run.
 *
 * za_arg holds the operands of the harness verbs, in the order they
 * were given: two for --build-fixture and three for the other two.
 */
struct zr_args {
	enum zr_verb	za_verb;
	const char	*za_from;	/* -f, --from, --off-of */
	const char	*za_onto;	/* -t, --onto, --to */
	const char	*za_result;	/* -r, --result */
	const char	*za_manifest;	/* -o, --manifest */
	const char	*za_base;	/* -b, --base */
	const char	*za_arg[3];
	zr_mode_t	za_mode;	/* -p, --permissive-merge */
	int		za_dryrun;	/* -n, --dry-run */
	int		za_verbose;	/* -v, --verbose */
	int		za_verify;	/* -V, --verify */
	int		za_overwrite;	/* -w, --overwrite */
	int		za_unrelated;	/* -u, --allow-unrelated */
	int		za_takeonto;	/* -O, --take-onto */
	int		za_takefrom;	/* -F, --take-from */
	int		za_nogui;	/* -G, --no-gui */
	int		za_nomerge;	/* -M, --no-merge */
};

/*
 * Parse argv into out. Returns 0, or -1 with one line in err saying
 * what was wrong -- the caller prints that line and the usage text
 * and exits 2. err may be NULL only if the caller has no use for the
 * reason; out is written whole either way, so a refused parse leaves
 * no half-filled struct behind.
 *
 * Every flag has a long form and a short form and the two parse to
 * the same struct. A long form takes its value as --name VALUE or as
 * --name=VALUE; a short form takes the next argument. Short flags do
 * not bundle: -nv is not -n -v, it is an unknown option.
 */
int zr_args_parse(int argc, char **argv, struct zr_args *out, char *err,
    size_t errlen);

#endif	/* ZR_ARGS_H */
