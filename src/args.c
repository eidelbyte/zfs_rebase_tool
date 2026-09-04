/*
 * zfs_rebase: the command line.
 *
 * One table says which letter goes with which word, and the usage
 * text, zfs_rebase.8 and README.md carry the same letters. Every
 * flag of the tool has both forms and the two parse to the same
 * struct; the harness verbs -- --posix, --build-fixture and
 * --edit-fixture -- are long only, since they are no part of
 * ordinary use.
 *
 * The parse acts on nothing. It fills a struct that main.c
 * dispatches on and the unit tests read, which is what puts every
 * refusal here within reach of a machine with no ZFS at all.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "args.h"

/* One flag of the table. */
enum zr_optid {
	ZO_FROM = 1,
	ZO_ONTO,
	ZO_RESULT,
	ZO_PERM,
	ZO_VERBOSE,
	ZO_MANIFEST,
	ZO_VERIFY,
	ZO_TAKEONTO,
	ZO_TAKEFROM,
	ZO_NOGUI,
	ZO_NOMERGE,
	ZO_CONTINUE,
	ZO_RESTART,
	ZO_ABORT,
	ZO_DRYRUN,
	ZO_OVERWRITE,
	ZO_UNRELATED,
	ZO_BASE
};

struct zr_opt {
	const char	*zo_long;	/* the word after "--" */
	char		zo_short;	/* the letter, or 0 for none */
	char		zo_takes;	/* it takes a value */
	int		zo_id;
};

/*
 * The table, in the order of the plan's options-to-behaviors table.
 * Two words share an id where the tool has an older spelling of a
 * flag: --off-of for --from and --to for --onto. A spelling with no
 * letter of its own is the alias, never the primary.
 */
static const struct zr_opt zr_opts[] = {
	{ "from",		'f', 1, ZO_FROM },
	{ "off-of",		 0,  1, ZO_FROM },
	{ "onto",		't', 1, ZO_ONTO },
	{ "to",			 0,  1, ZO_ONTO },
	{ "result",		'r', 1, ZO_RESULT },
	{ "permissive-merge",	'p', 0, ZO_PERM },
	{ "verbose",		'v', 0, ZO_VERBOSE },
	{ "manifest",		'o', 1, ZO_MANIFEST },
	{ "verify",		'V', 0, ZO_VERIFY },
	{ "take-onto",		'O', 0, ZO_TAKEONTO },
	{ "take-from",		'F', 0, ZO_TAKEFROM },
	{ "no-gui",		'G', 0, ZO_NOGUI },
	{ "no-merge",		'M', 0, ZO_NOMERGE },
	{ "continue",		'c', 0, ZO_CONTINUE },
	{ "restart",		'R', 0, ZO_RESTART },
	{ "abort",		'a', 0, ZO_ABORT },
	{ "dry-run",		'n', 0, ZO_DRYRUN },
	{ "overwrite",		'w', 0, ZO_OVERWRITE },
	{ "allow-unrelated",	'u', 0, ZO_UNRELATED },
	{ "base",		'b', 1, ZO_BASE }
};

#define	ZA_NOPT	(sizeof (zr_opts) / sizeof (zr_opts[0]))

/* One line saying what was wrong with the command, and the refusal. */
static int
za_no(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err != NULL && errlen > 0) {
		va_start(ap, fmt);
		(void) vsnprintf(err, errlen, fmt, ap);
		va_end(ap);
	}
	return (-1);
}

/* What to call the command in a refusal that names it. */
static const char *
za_verb_word(enum zr_verb v)
{
	switch (v) {
	case ZR_VERB_CONTINUE:
		return ("--continue");
	case ZR_VERB_RESTART:
		return ("--restart");
	case ZR_VERB_ABORT:
		return ("--abort");
	case ZR_VERB_REPORT:
		return ("--verify");
	case ZR_VERB_POSIX:
		return ("--posix");
	case ZR_VERB_BUILD_FIXTURE:
		return ("--build-fixture");
	case ZR_VERB_EDIT_FIXTURE:
		return ("--edit-fixture");
	case ZR_VERB_RUN:
	default:
		return ("a rebase");
	}
}

/*
 * One argument of the command, matched against the table. Every
 * argument in the flag section is a flag -- the tool takes no
 * operands there -- so anything else is a refusal rather than the
 * end of the options. *i is advanced over a value the flag took.
 */
static int
za_one(int argc, char **argv, int *i, const struct zr_opt **opt,
    const char **val, char *err, size_t errlen)
{
	const char *a = argv[*i];
	const char *eq = NULL;
	size_t n, k;

	*opt = NULL;
	*val = NULL;
	if (a[0] != '-' || a[1] == '\0')
		return (za_no(err, errlen, "\"%s\" is not an option", a));
	if (a[1] == '-') {
		eq = strchr(a + 2, '=');
		n = eq != NULL ? (size_t)(eq - (a + 2)) : strlen(a + 2);
		for (k = 0; k < ZA_NOPT; k++) {
			if (strlen(zr_opts[k].zo_long) == n &&
			    strncmp(a + 2, zr_opts[k].zo_long, n) == 0) {
				*opt = &zr_opts[k];
				break;
			}
		}
	} else if (a[2] == '\0') {
		for (k = 0; k < ZA_NOPT; k++) {
			if (zr_opts[k].zo_short == a[1]) {
				*opt = &zr_opts[k];
				break;
			}
		}
	}
	if (*opt == NULL)
		return (za_no(err, errlen, "unknown option \"%s\"", a));
	if ((*opt)->zo_takes == 0) {
		if (eq != NULL)
			return (za_no(err, errlen, "%s takes no value", a));
		return (0);
	}
	if (eq != NULL) {
		*val = eq + 1;
	} else if (*i + 1 < argc) {
		*val = argv[++(*i)];
	} else {
		return (za_no(err, errlen, "%s needs a value", a));
	}
	if ((*val)[0] == '\0')
		return (za_no(err, errlen, "%s needs a value", a));
	return (0);
}

/* The flag, written into the struct. */
static void
za_set(struct zr_args *out, const struct zr_opt *opt, const char *val,
    int *cont, int *rest, int *abrt)
{
	switch (opt->zo_id) {
	case ZO_FROM:
		out->za_from = val;
		break;
	case ZO_ONTO:
		out->za_onto = val;
		break;
	case ZO_RESULT:
		out->za_result = val;
		break;
	case ZO_PERM:
		out->za_mode = ZR_MODE_PERMISSIVE;
		break;
	case ZO_VERBOSE:
		out->za_verbose = 1;
		break;
	case ZO_MANIFEST:
		out->za_manifest = val;
		break;
	case ZO_VERIFY:
		out->za_verify = 1;
		break;
	case ZO_TAKEONTO:
		out->za_takeonto = 1;
		break;
	case ZO_TAKEFROM:
		out->za_takefrom = 1;
		break;
	case ZO_NOGUI:
		out->za_nogui = 1;
		break;
	case ZO_NOMERGE:
		out->za_nomerge = 1;
		break;
	case ZO_CONTINUE:
		*cont = 1;
		break;
	case ZO_RESTART:
		*rest = 1;
		break;
	case ZO_ABORT:
		*abrt = 1;
		break;
	case ZO_DRYRUN:
		out->za_dryrun = 1;
		break;
	case ZO_OVERWRITE:
		out->za_overwrite = 1;
		break;
	case ZO_UNRELATED:
		out->za_unrelated = 1;
		break;
	case ZO_BASE:
	default:
		out->za_base = val;
		break;
	}
}

/*
 * --posix: three plain directories in and a manifest out, which is
 * how the decision is exercised where there is no ZFS. It takes the
 * mode and the manifest path and nothing else, since it creates no
 * rebase for the rest of the flags to speak about.
 */
static int
za_posix(int argc, char **argv, struct zr_args *out, char *err, size_t errlen)
{
	const struct zr_opt *opt;
	const char *val;
	int i, dummy = 0;

	out->za_verb = ZR_VERB_POSIX;
	for (i = 2; i < argc && argv[i][0] == '-'; i++) {
		if (za_one(argc, argv, &i, &opt, &val, err, errlen) != 0)
			return (-1);
		if (opt->zo_id != ZO_PERM && opt->zo_id != ZO_MANIFEST)
			return (za_no(err, errlen, "--posix takes -p and -o "
			    "and no other option"));
		za_set(out, opt, val, &dummy, &dummy, &dummy);
	}
	if (argc - i != 3)
		return (za_no(err, errlen, "--posix takes three directories: "
		    "BASEDIR FROMDIR ONTODIR"));
	out->za_arg[0] = argv[i];
	out->za_arg[1] = argv[i + 1];
	out->za_arg[2] = argv[i + 2];
	return (0);
}

/* --build-fixture and --edit-fixture: operands only, no flags. */
static int
za_fixture(int argc, char **argv, struct zr_args *out, int n, char *err,
    size_t errlen)
{
	int i;

	if (argc - 2 != n)
		return (za_no(err, errlen, "%s takes %s", argv[1],
		    n == 2 ? "FIXTURE DIR" : "FIXTURE TREE DIR"));
	for (i = 0; i < n; i++)
		out->za_arg[i] = argv[i + 2];
	return (0);
}

/*
 * Which command this is. --continue, --restart and --abort each name
 * themselves and no two of them go together; --verify names the
 * report only when it stands alone on a result, since with a rebase
 * or a --continue it is the flag asking for the final check.
 */
static int
za_verb(struct zr_args *out, int cont, int rest, int abrt, char *err,
    size_t errlen)
{
	if (cont + rest + abrt > 1)
		return (za_no(err, errlen, "--continue, --restart and --abort "
		    "are three verbs; give one"));
	if (cont != 0)
		out->za_verb = ZR_VERB_CONTINUE;
	else if (rest != 0)
		out->za_verb = ZR_VERB_RESTART;
	else if (abrt != 0)
		out->za_verb = ZR_VERB_ABORT;
	else if (out->za_verify != 0 && out->za_from == NULL &&
	    out->za_onto == NULL)
		out->za_verb = ZR_VERB_REPORT;
	else
		out->za_verb = ZR_VERB_RUN;
	return (0);
}

/*
 * The three flags of the conflicts gate. --take-onto and --take-from
 * answer the skeleton the fresh run writes, so they belong to that
 * run alone: no verb writes a skeleton, and --restart writes the one
 * the run asked for by reading the record. --no-gui and --no-merge
 * belong to the two commands that reach the gate.
 */
static int
za_gate_flags(const struct zr_args *out, char *err, size_t errlen)
{
	const char *word = za_verb_word(out->za_verb);

	if (out->za_takeonto != 0 && out->za_takefrom != 0)
		return (za_no(err, errlen, "--take-onto and --take-from ask "
		    "for opposite skeletons; give one"));
	if (out->za_verb != ZR_VERB_RUN &&
	    (out->za_takeonto != 0 || out->za_takefrom != 0))
		return (za_no(err, errlen, "%s answers the skeleton a fresh "
		    "run writes, and %s writes none",
		    out->za_takeonto != 0 ? "--take-onto" : "--take-from",
		    word));
	if (out->za_verb != ZR_VERB_RUN && out->za_verb != ZR_VERB_CONTINUE) {
		if (out->za_nogui != 0)
			return (za_no(err, errlen, "--no-gui is for a rebase "
			    "and --continue, which reach the conflicts gate, "
			    "not for %s", word));
		if (out->za_nomerge != 0)
			return (za_no(err, errlen, "--no-merge is for a "
			    "rebase and --continue, which reach the conflicts "
			    "gate, not for %s", word));
	}
	return (0);
}

/*
 * The verbs on a result. Each goes alone -- one --result, the gate
 * flags --continue is allowed, -v, and nothing else -- because there
 * is nothing for a flag of a fresh run to act on: --overwrite
 * replaces a record no verb is writing, and an open rebase is
 * settled by --continue or --abort whatever flags are given.
 */
static int
za_verb_flags(const struct zr_args *out, char *err, size_t errlen)
{
	const char *word = za_verb_word(out->za_verb);

	if (out->za_result == NULL)
		return (za_no(err, errlen, "%s needs --result, the dataset "
		    "carrying the record", word));
	if (out->za_from != NULL || out->za_onto != NULL)
		return (za_no(err, errlen, "%s reads the two sides from the "
		    "record; --from and --onto say nothing to it", word));
	if (out->za_manifest != NULL)
		return (za_no(err, errlen, "%s reads the manifest the record "
		    "names; --manifest says nothing to it", word));
	if (out->za_dryrun != 0 || out->za_overwrite != 0 ||
	    out->za_unrelated != 0 || out->za_base != NULL ||
	    out->za_mode != ZR_MODE_STRICT)
		return (za_no(err, errlen, "%s takes --result and the flags "
		    "of the gate; the rest belong to a fresh run", word));
	/*
	 * --verify is the verb here, or the flag --continue carries.
	 * On --restart and --abort it is neither: one applies again
	 * and the other takes the rebase away.
	 */
	if (out->za_verify != 0 && (out->za_verb == ZR_VERB_RESTART ||
	    out->za_verb == ZR_VERB_ABORT))
		return (za_no(err, errlen, "%s makes no check to report; "
		    "--verify says nothing to it", word));
	return (0);
}

/* A fresh run: the two sides, and a name for what it makes. */
static int
za_run_flags(const struct zr_args *out, char *err, size_t errlen)
{
	if (out->za_from == NULL || out->za_onto == NULL)
		return (za_no(err, errlen, "a rebase needs --from and --onto, "
		    "the two sides"));
	/*
	 * A dry run creates nothing that would need a name, so it
	 * ignores --result rather than demanding one.
	 */
	if (out->za_result == NULL && out->za_dryrun == 0)
		return (za_no(err, errlen, "a rebase needs --result, the "
		    "name of what it makes"));
	return (0);
}

int
zr_args_parse(int argc, char **argv, struct zr_args *out, char *err,
    size_t errlen)
{
	const struct zr_opt *opt;
	const char *val;
	int i, cont = 0, rest = 0, abrt = 0;

	memset(out, 0, sizeof (*out));
	out->za_mode = ZR_MODE_STRICT;
	out->za_verb = ZR_VERB_RUN;
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (argc < 2)
		return (za_no(err, errlen, "nothing to do"));
	if (strcmp(argv[1], "--posix") == 0)
		return (za_posix(argc, argv, out, err, errlen));
	if (strcmp(argv[1], "--build-fixture") == 0) {
		out->za_verb = ZR_VERB_BUILD_FIXTURE;
		return (za_fixture(argc, argv, out, 2, err, errlen));
	}
	if (strcmp(argv[1], "--edit-fixture") == 0) {
		out->za_verb = ZR_VERB_EDIT_FIXTURE;
		return (za_fixture(argc, argv, out, 3, err, errlen));
	}
	for (i = 1; i < argc; i++) {
		if (za_one(argc, argv, &i, &opt, &val, err, errlen) != 0)
			return (-1);
		za_set(out, opt, val, &cont, &rest, &abrt);
	}
	if (za_verb(out, cont, rest, abrt, err, errlen) != 0)
		return (-1);
	if (za_gate_flags(out, err, errlen) != 0)
		return (-1);
	/*
	 * A base is given only where there is none to work out: with
	 * a shared origin the branch point is what it is, and a
	 * second opinion about it is not something the tool could act
	 * on.
	 */
	if (out->za_base != NULL && out->za_unrelated == 0)
		return (za_no(err, errlen, "--base is given with "
		    "--allow-unrelated, where there is no branch point to "
		    "derive"));
	if (out->za_verb != ZR_VERB_RUN)
		return (za_verb_flags(out, err, errlen));
	return (za_run_flags(out, err, errlen));
}
