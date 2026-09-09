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
	ZO_QUIET,
	ZO_TAKEONTO,
	ZO_TAKEFROM,
	ZO_INTERACTIVE,
	ZO_NOMERGE,
	ZO_CONTINUE,
	ZO_RESTART,
	ZO_ABORT,
	ZO_DRYRUN,
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
	{ "quiet",		'q', 0, ZO_QUIET },
	{ "take-onto",		'O', 0, ZO_TAKEONTO },
	{ "take-from",		'F', 0, ZO_TAKEFROM },
	{ "interactive",	'i', 0, ZO_INTERACTIVE },
	{ "no-merge",		'M', 0, ZO_NOMERGE },
	{ "continue",		'c', 0, ZO_CONTINUE },
	{ "restart",		'R', 0, ZO_RESTART },
	{ "abort",		'A', 0, ZO_ABORT },
	{ "dry-run",		'n', 0, ZO_DRYRUN },
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
 * One argument of the command, matched against the table. The caller
 * has already taken the one operand of ordinary use -- a manifest's
 * path, which begins with no dash -- so everything that reaches here
 * is meant as a flag, and a word that is not one is a refusal rather
 * than the end of the options. *i is advanced over a value the flag
 * took.
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
	case ZO_QUIET:
		out->za_quiet = 1;
		break;
	case ZO_TAKEONTO:
		out->za_takeonto = 1;
		break;
	case ZO_TAKEFROM:
		out->za_takefrom = 1;
		break;
	case ZO_INTERACTIVE:
		out->za_interactive = 1;
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
 * themselves and no two of them go together, and --verify names the
 * report always: it is a verb and only a verb (documents-design.md,
 * section 7). There is no request form left to tell it apart from --
 * the checks run on a schedule of their own and no flag asks for one
 * -- so the word decides the command by itself, and the refusals
 * below are what a command that used to mean a start with the flag
 * gets instead.
 *
 * A verb that names two commands is caught here; --verify beside one
 * of the other three is caught in za_verb_flags, which sees the verb
 * this chose and the flag both.
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
	else if (out->za_verify != 0)
		out->za_verb = ZR_VERB_REPORT;
	else
		out->za_verb = ZR_VERB_RUN;
	return (0);
}

/*
 * The three flags of the conflicts gate. --take-onto and --take-from
 * answer the skeleton the fresh run writes, so they belong to that
 * run alone: no verb writes a skeleton, and --restart writes the one
 * the run asked for by reading the record. --interactive and
 * --no-merge belong to the two commands that reach the gate.
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
		if (out->za_interactive != 0)
			return (za_no(err, errlen, "--interactive is for a "
			    "rebase and --continue, which reach the conflicts "
			    "gate, not for %s", word));
		if (out->za_nomerge != 0)
			return (za_no(err, errlen, "--no-merge is for a "
			    "rebase and --continue, which reach the conflicts "
			    "gate, not for %s", word));
	}
	return (0);
}

/*
 * The verbs on a rebase that already exists. Each names the run it
 * acts on with IDENT, its one operand -- the result, its pre-apply
 * snapshot, its manifest's path or its run directory, which the
 * driver resolves and cross-checks -- and takes the two sides, the
 * gate flags --continue is allowed and -v, and nothing else, because
 * there is nothing for a flag of a fresh run to act on: a rebase is
 * decided once, and an open one is settled by --continue or --abort
 * whatever flags are given.
 *
 * --result is a start flag and no verb takes it (ruled 2026-09-08,
 * documents-design.md section 11.4): it names what a run is to make,
 * and a verb acts on one that exists. The refusal says so rather
 * than reading it as the identifier, because a command written for
 * the older spelling must not quietly do something else.
 *
 * --from and --onto are the exception among those. A verb reads the
 * two sides from the header and needs neither, but a person who
 * names them is saying which rebase they think this is, and the
 * driver checks them against the header by name and by guid. What
 * they can never do is change what a verb acts on.
 */
static int
za_verb_flags(const struct zr_args *out, char *err, size_t errlen)
{
	const char *word = za_verb_word(out->za_verb);

	/*
	 * --verify is a verb and only a verb (documents-design.md,
	 * section 7): the final check is standard now, made by
	 * whichever invocation reaches the done gate, so there is no
	 * request left for the word to be. Beside one of the three
	 * that move a rebase it is refused rather than ignored,
	 * because a command written for the older meaning must not
	 * quietly do something else.
	 */
	if (out->za_verify != 0 && out->za_verb != ZR_VERB_REPORT)
		return (za_no(err, errlen, "--verify is a verb of its own, "
		    "and so is %s; the final check is standard now and no "
		    "flag asks for it", word));
	/*
	 * And beside the flags that start one. A dry run is a start's
	 * other spelling; --result is the flag that names what a
	 * start makes. --from and --onto stand beside a verb, where
	 * they name no rebase and are checked against the header.
	 */
	if (out->za_verb == ZR_VERB_REPORT && out->za_dryrun != 0)
		return (za_no(err, errlen, "--dry-run decides a rebase and "
		    "writes its manifest; --verify reports on one that "
		    "exists, and the two are not one command"));
	if (out->za_result != NULL)
		return (za_no(err, errlen, "--result names the result of a "
		    "run you are starting; a verb takes the rebase's name: "
		    "%s IDENT, which is the result, its pre-apply snapshot, "
		    "its manifest's path or its run directory", word));
	if (out->za_ident == NULL)
		return (za_no(err, errlen, "%s needs the rebase it acts on: "
		    "IDENT, which is the result, its pre-apply snapshot, its "
		    "manifest's path or its run directory", word));
	/*
	 * -o chooses where a manifest is written, and the start is
	 * the only command that writes one: the record names the path
	 * from then on and done acts on it, so there is no later
	 * moment at which the choice could be made.
	 */
	if (out->za_manifest != NULL)
		return (za_no(err, errlen, "%s reads the manifest from the "
		    "record the start wrote; -o says nothing to it", word));
	/*
	 * --quiet is latched at the start, in the record, and is the
	 * whole run's: a verb that arrives later reads it there and
	 * has nothing to say about it.
	 */
	if (out->za_quiet != 0)
		return (za_no(err, errlen, "%s reads --quiet from the record "
		    "the start wrote; --quiet says nothing to it", word));
	if (out->za_dryrun != 0 || out->za_unrelated != 0 ||
	    out->za_base != NULL || out->za_mode != ZR_MODE_STRICT)
		return (za_no(err, errlen, "%s takes IDENT and the flags of "
		    "the gate; the rest belong to a fresh run", word));
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
	 * An identifier names a rebase that exists, and this command
	 * makes one: what a start has to say about where its own
	 * manifest goes it says with -o, and what it calls what it
	 * makes it says with --result.
	 */
	if (out->za_ident != NULL)
		return (za_no(err, errlen, "\"%s\": an identifier names a "
		    "rebase to a verb, and this command starts one",
		    out->za_ident));
	/*
	 * A base is given only where there is none to work out: with
	 * a shared origin the branch point is what it is, and a
	 * second opinion about it is not something the tool could act
	 * on. Where there is no shared origin there is nothing to
	 * derive and nothing to fall back on either, so the flag that
	 * says the two sides are unrelated needs the base with it.
	 */
	if (out->za_base != NULL && out->za_unrelated == 0)
		return (za_no(err, errlen, "--base is given with "
		    "--allow-unrelated, where there is no branch point to "
		    "derive"));
	if (out->za_unrelated != 0 && out->za_base == NULL)
		return (za_no(err, errlen, "--allow-unrelated needs --base: "
		    "two sides that share no origin have no branch point to "
		    "derive, and the tool invents none"));
	/*
	 * A dry run creates nothing that would need a name, so it
	 * ignores --result rather than demanding one.
	 */
	if (out->za_result == NULL && out->za_dryrun == 0)
		return (za_no(err, errlen, "a rebase needs --result, the "
		    "name of what it makes"));
	/*
	 * --quiet silences the final check's report, and a dry run
	 * makes no check: it writes a manifest and stops. There is
	 * nothing for the flag to quieten and no record to latch it
	 * in.
	 */
	if (out->za_quiet != 0 && out->za_dryrun != 0)
		return (za_no(err, errlen, "--dry-run makes no check to "
		    "report; --quiet says nothing to it"));
	/*
	 * And the four gate flags, for the same reason (R22). A dry
	 * run stops at the manifest: it writes no skeleton for
	 * --take-onto or --take-from to answer and reaches no
	 * conflicts gate for --interactive or --no-merge to hold, so
	 * there is nothing for the flag to act on and no record to
	 * latch it in. Refused rather than ignored, because a command
	 * that says how to answer the conflicts and is not going to be
	 * asked has been written under a wrong idea of what it does.
	 */
	if (out->za_dryrun != 0 && (out->za_takeonto != 0 ||
	    out->za_takefrom != 0 || out->za_interactive != 0 ||
	    out->za_nomerge != 0))
		return (za_no(err, errlen, "--dry-run writes a manifest and "
		    "stops; %s says nothing to it",
		    out->za_takeonto != 0 ? "--take-onto" :
		    out->za_takefrom != 0 ? "--take-from" :
		    out->za_interactive != 0 ? "--interactive" :
		    "--no-merge"));
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
		/*
		 * The one operand of ordinary use, wherever it stands
		 * among the flags: IDENT, which names the rebase a
		 * verb acts on. A word beginning with a dash is a flag
		 * or nothing, so a path that begins with one is
		 * spelled ./-name, as it is for every other tool.
		 */
		if (argv[i][0] != '-') {
			if (argv[i][0] == '\0')
				return (za_no(err, errlen, "an empty "
				    "argument names no rebase"));
			if (out->za_ident != NULL)
				return (za_no(err, errlen, "one identifier "
				    "names one rebase; \"%s\" is a second",
				    argv[i]));
			out->za_ident = argv[i];
			continue;
		}
		if (za_one(argc, argv, &i, &opt, &val, err, errlen) != 0)
			return (-1);
		za_set(out, opt, val, &cont, &rest, &abrt);
	}
	if (za_verb(out, cont, rest, abrt, err, errlen) != 0)
		return (-1);
	if (za_gate_flags(out, err, errlen) != 0)
		return (-1);
	if (out->za_verb != ZR_VERB_RUN)
		return (za_verb_flags(out, err, errlen));
	return (za_run_flags(out, err, errlen));
}
