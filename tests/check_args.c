/*
 * Tests for the command line (src/args.c): every long form and its
 * short form parsing to the same run, the aliases, the two ways a
 * long form carries its value, the harness verbs, and every refusal
 * the parse makes. Nothing here opens a file or looks at a dataset,
 * which is what puts the whole of the driver's grammar within reach
 * of a machine with no ZFS in it.
 *
 * Matrix cells (tests/MATRIX.md, family ZX): ZX100 to ZX121,
 * ZX137 to ZX139 for --quiet and for the --overwrite that is gone,
 * ZX188 and ZX189 for --verify as a verb and only a verb,
 * ZX180 to ZX187 for IDENT as the one operand, -o at the start
 * alone, --allow-unrelated needing --base, and the two sides on a
 * verb, and ZX221 to ZX225 for the identifier itself: the operand
 * every verb takes, the --result that is a start's flag alone, and
 * the two steps of the resolution that read a document.
 *
 * The last of those, and ZX187 with them, are src/run.c's rules for
 * reading a run's dataset out of a header and for a manifest an
 * identifier names: they take a parsed header, or a file, and open
 * no pool, so they belong with the rest of what a machine with no
 * ZFS can settle about naming a run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "args.h"
#include "manifest.h"
#include "run.h"

#define	NELEM(a)	(sizeof (a) / sizeof ((a)[0]))

static unsigned long checks;

static void
check_at(int cond, const char *file, int line, const char *expr)
{
	checks++;
	if (!cond) {
		fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
		exit(1);
	}
}

#define	CHECK(x)	check_at((x) ? 1 : 0, __FILE__, __LINE__, #x)

/*
 * The words of a command, as objects rather than as literals: two
 * spellings of one command are built out of the very same value
 * strings, so the two parsed structs hold the same pointers and the
 * whole of them compares with memcmp. A field added to struct
 * zr_args is therefore covered by every pair below without a line
 * being added here.
 */
static char w_prog[] = "zfs_rebase";
static char w_from[] = "--from";
static char w_offof[] = "--off-of";
static char w_onto[] = "--onto";
static char w_to[] = "--to";
static char w_result[] = "--result";
static char w_perm[] = "--permissive-merge";
static char w_verbose[] = "--verbose";
static char w_manifest[] = "--manifest";
static char w_verify[] = "--verify";
static char w_takeonto[] = "--take-onto";
static char w_takefrom[] = "--take-from";
static char w_interactive[] = "--interactive";
static char w_nomerge[] = "--no-merge";
static char w_continue[] = "--continue";
static char w_restart[] = "--restart";
static char w_abort[] = "--abort";
static char w_dryrun[] = "--dry-run";
static char w_quiet[] = "--quiet";
static char w_unrelated[] = "--allow-unrelated";
static char w_base[] = "--base";
static char w_posix[] = "--posix";
static char w_build[] = "--build-fixture";
static char w_edit[] = "--edit-fixture";

static char s_from[] = "-f";
static char s_onto[] = "-t";
static char s_result[] = "-r";
static char s_perm[] = "-p";
static char s_verbose[] = "-v";
static char s_manifest[] = "-o";
static char s_verify[] = "-V";
static char s_takeonto[] = "-O";
static char s_takefrom[] = "-F";
static char s_interactive[] = "-i";
static char s_nomerge[] = "-M";
static char s_continue[] = "-c";
static char s_restart[] = "-R";
static char s_abort[] = "-a";
static char s_dryrun[] = "-n";
static char s_quiet[] = "-q";
static char s_unrelated[] = "-u";
static char s_base[] = "-b";

static char v_from[] = "tank/topic@work";
static char v_onto[] = "tank/main@now";
static char v_result[] = "tank/rebased";
static char v_manifest[] = "/tmp/manifest";
static char v_base[] = "tank/old@base";
static char v_bdir[] = "/tmp/b";
static char v_fdir[] = "/tmp/f";
static char v_odir[] = "/tmp/o";
static char v_fixture[] = "tests/fixtures/x.zrt";
static char v_tree[] = "from";

/* The long word this letter stands for, so a pair can be respelled. */
static const struct {
	char	*zp_long;
	char	*zp_short;
} zr_pairs[] = {
	{ w_from, s_from },
	{ w_onto, s_onto },
	{ w_result, s_result },
	{ w_perm, s_perm },
	{ w_verbose, s_verbose },
	{ w_manifest, s_manifest },
	{ w_verify, s_verify },
	{ w_takeonto, s_takeonto },
	{ w_takefrom, s_takefrom },
	{ w_interactive, s_interactive },
	{ w_nomerge, s_nomerge },
	{ w_continue, s_continue },
	{ w_restart, s_restart },
	{ w_abort, s_abort },
	{ w_dryrun, s_dryrun },
	{ w_quiet, s_quiet },
	{ w_unrelated, s_unrelated },
	{ w_base, s_base }
};

/*
 * A template under TMPDIR, or /tmp without it, for the one test here
 * that puts a document on disk: an identifier that is a path is
 * resolved by reading the file at it.
 */
static void
tmp_template(char *buf, size_t len, const char *leaf)
{
	const char *d = getenv("TMPDIR");

	(void) snprintf(buf, len, "%s/%s", d != NULL && d[0] != '\0' ?
	    d : "/tmp", leaf);
}

/* The whole header and no body, which is what a run writes at birth. */
static int
emit_birth(FILE *out, void *arg)
{
	return (zr_manifest_birth(out, arg));
}

/* This word as its letter, or the word itself where it has none. */
static char *
short_of(char *word)
{
	size_t i;

	for (i = 0; i < NELEM(zr_pairs); i++) {
		if (strcmp(word, zr_pairs[i].zp_long) == 0)
			return (zr_pairs[i].zp_short);
	}
	return (word);
}

/* Parse a command that must be understood, and give back the struct. */
static struct zr_args
parse_ok(char **argv, int argc)
{
	struct zr_args a;
	char err[512];

	err[0] = 'x';
	CHECK(zr_args_parse(argc, argv, &a, err, sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	return (a);
}

/* Parse a command that must be refused, with one line saying why. */
static void
parse_bad(char **argv, int argc)
{
	struct zr_args a;
	char err[512];

	CHECK(zr_args_parse(argc, argv, &a, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0');
	CHECK(strchr(err, '\n') == NULL);
}

/*
 * One command written with long forms, and the same command with
 * every long form replaced by its letter: the two must parse to the
 * same run, byte for byte.
 */
static void
check_pair(char **argv, int argc)
{
	char *sv[16];
	struct zr_args a, b;
	int i;

	CHECK(argc <= (int)NELEM(sv));
	for (i = 0; i < argc; i++)
		sv[i] = short_of(argv[i]);
	a = parse_ok(argv, argc);
	b = parse_ok(sv, argc);
	CHECK(memcmp(&a, &b, sizeof (a)) == 0);
}

/*
 * ZX100, ZX102, ZX103, ZX104, ZX105, ZX106, ZX137: the fresh run's
 * flags, one command per flag added to the smallest run that is
 * legal, each written both ways and parsed to the same struct.
 */
static void
test_pairs_run(void)
{
	char *base[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result };
	char *with[10];
	char *one[] = { w_perm, w_verbose, w_takeonto, w_takefrom,
	    w_interactive, w_nomerge, w_quiet };
	char *val[] = { w_manifest, w_base };
	size_t i;
	int n;

	/* ZX100: --from, --onto and --result themselves. */
	check_pair(base, (int)NELEM(base));

	/* ZX102, ZX103, ZX105, ZX106, ZX137: one flag, no value. */
	for (i = 0; i < NELEM(one); i++) {
		memcpy(with, base, sizeof (base));
		n = (int)NELEM(base);
		with[n++] = one[i];
		check_pair(with, n);
	}
	/*
	 * ZX102, ZX103: the two that take a value. --base and
	 * --allow-unrelated are legal only with each other, so the
	 * one command carries both and covers the letters of both.
	 */
	for (i = 0; i < NELEM(val); i++) {
		memcpy(with, base, sizeof (base));
		n = (int)NELEM(base);
		if (val[i] == w_base)
			with[n++] = w_unrelated;
		with[n++] = val[i];
		with[n++] = val[i] == w_base ? v_base : v_manifest;
		check_pair(with, n);
	}
	/* ZX104: a dry run, which needs no --result. */
	{
		char *dry[] = { w_prog, w_dryrun, w_from, v_from, w_onto,
		    v_onto };

		check_pair(dry, (int)NELEM(dry));
	}
}

/* ZX107, ZX108, ZX109, ZX110: the verbs, both spellings. */
static void
test_pairs_verbs(void)
{
	char *cont[] = { w_prog, w_continue, v_result };
	char *contv[] = { w_prog, w_continue, w_interactive, w_nomerge,
	    w_verbose, v_result };
	char *rest[] = { w_prog, w_restart, v_result };
	char *abrt[] = { w_prog, w_abort, w_verbose, v_result };
	char *rep[] = { w_prog, w_verify, v_result };
	struct zr_args a;

	check_pair(cont, (int)NELEM(cont));
	check_pair(contv, (int)NELEM(contv));
	check_pair(rest, (int)NELEM(rest));
	check_pair(abrt, (int)NELEM(abrt));
	check_pair(rep, (int)NELEM(rep));

	a = parse_ok(cont, (int)NELEM(cont));
	CHECK(a.za_verb == ZR_VERB_CONTINUE);
	CHECK(a.za_ident == v_result && a.za_result == NULL);
	a = parse_ok(contv, (int)NELEM(contv));
	CHECK(a.za_verb == ZR_VERB_CONTINUE);
	CHECK(a.za_verify == 0 && a.za_interactive == 1 && a.za_nomerge == 1);
	CHECK(a.za_verbose == 1);
	a = parse_ok(rest, (int)NELEM(rest));
	CHECK(a.za_verb == ZR_VERB_RESTART);
	a = parse_ok(abrt, (int)NELEM(abrt));
	CHECK(a.za_verb == ZR_VERB_ABORT && a.za_verbose == 1);
	/*
	 * ZX110: --verify is the verb, always. One word and one
	 * meaning: the command that used to be a start with the flag
	 * is a refusal now (ZX189), and nothing the word stands
	 * beside makes it anything but the report.
	 */
	a = parse_ok(rep, (int)NELEM(rep));
	CHECK(a.za_verb == ZR_VERB_REPORT && a.za_verify == 1);
	{
		char *sole[] = { w_prog, w_verify, v_manifest };
		char *side[] = { w_prog, w_verify, v_result, w_from, v_from };
		char *loud[] = { w_prog, w_verify, w_verbose, v_result };

		a = parse_ok(sole, (int)NELEM(sole));
		CHECK(a.za_verb == ZR_VERB_REPORT && a.za_ident == v_manifest);
		a = parse_ok(side, (int)NELEM(side));
		CHECK(a.za_verb == ZR_VERB_REPORT && a.za_from == v_from);
		CHECK(a.za_ident == v_result);
		a = parse_ok(loud, (int)NELEM(loud));
		CHECK(a.za_verb == ZR_VERB_REPORT && a.za_verbose == 1);
		check_pair(sole, (int)NELEM(sole));
		check_pair(side, (int)NELEM(side));
	}
}

/*
 * ZX101: --off-of is --from and --to is --onto, the two spellings
 * the tool had before the letters, and neither has a letter of its
 * own.
 */
static void
test_aliases(void)
{
	char *named[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result };
	char *alias[] = { w_prog, w_offof, v_from, w_to, v_onto,
	    w_result, v_result };
	struct zr_args a, b;

	a = parse_ok(named, (int)NELEM(named));
	b = parse_ok(alias, (int)NELEM(alias));
	CHECK(memcmp(&a, &b, sizeof (a)) == 0);
	CHECK(a.za_from == v_from && a.za_onto == v_onto);
}

/* ZX111: --name VALUE and --name=VALUE are one flag written twice. */
static void
test_equals_form(void)
{
	static char joined[] = "--manifest=/tmp/manifest";
	static char jshort[] = "--from=tank/topic@work";
	char *apart[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_manifest, v_manifest };
	char *together[] = { w_prog, jshort, w_onto, v_onto,
	    w_result, v_result, joined };
	struct zr_args a, b;

	a = parse_ok(apart, (int)NELEM(apart));
	b = parse_ok(together, (int)NELEM(together));
	CHECK(a.za_verb == b.za_verb);
	CHECK(strcmp(a.za_from, b.za_from) == 0);
	CHECK(strcmp(a.za_manifest, b.za_manifest) == 0);
	/* A flag that takes nothing takes nothing either way round. */
	{
		static char eq[] = "--verify=yes";
		char *bad[] = { w_prog, eq, w_result, v_result };

		parse_bad(bad, (int)NELEM(bad));
	}
}

/*
 * ZX112: the three harness verbs. They take the first argument
 * position, they have no letter, and --posix takes the mode and the
 * manifest path and nothing else.
 */
static void
test_harness_verbs(void)
{
	char *posix[] = { w_prog, w_posix, w_perm, w_manifest, v_manifest,
	    v_bdir, v_fdir, v_odir };
	char *posixs[] = { w_prog, w_posix, s_perm, s_manifest, v_manifest,
	    v_bdir, v_fdir, v_odir };
	char *build[] = { w_prog, w_build, v_fixture, v_bdir };
	char *edit[] = { w_prog, w_edit, v_fixture, v_tree, v_bdir };
	struct zr_args a, b;

	a = parse_ok(posix, (int)NELEM(posix));
	CHECK(a.za_verb == ZR_VERB_POSIX);
	CHECK(a.za_mode == ZR_MODE_PERMISSIVE);
	CHECK(a.za_manifest == v_manifest);
	CHECK(a.za_arg[0] == v_bdir && a.za_arg[1] == v_fdir);
	CHECK(a.za_arg[2] == v_odir);
	b = parse_ok(posixs, (int)NELEM(posixs));
	CHECK(memcmp(&a, &b, sizeof (a)) == 0);

	a = parse_ok(build, (int)NELEM(build));
	CHECK(a.za_verb == ZR_VERB_BUILD_FIXTURE);
	CHECK(a.za_arg[0] == v_fixture && a.za_arg[1] == v_bdir);
	a = parse_ok(edit, (int)NELEM(edit));
	CHECK(a.za_verb == ZR_VERB_EDIT_FIXTURE);
	CHECK(a.za_arg[1] == v_tree && a.za_arg[2] == v_bdir);

	/* The wrong number of operands, and a flag --posix has no use for. */
	{
		char *two[] = { w_prog, w_posix, v_bdir, v_fdir };
		char *four[] = { w_prog, w_build, v_fixture, v_bdir, v_odir };
		char *three[] = { w_prog, w_edit, v_fixture, v_bdir };
		char *odd[] = { w_prog, w_posix, w_verify, v_bdir, v_fdir,
		    v_odir };

		parse_bad(two, (int)NELEM(two));
		parse_bad(four, (int)NELEM(four));
		parse_bad(three, (int)NELEM(three));
		parse_bad(odd, (int)NELEM(odd));
	}
	/* They are long only: no letter spells them. */
	{
		static char dash_p[] = "-P";
		char *no[] = { w_prog, dash_p, v_bdir, v_fdir, v_odir };

		parse_bad(no, (int)NELEM(no));
	}
}

/* ZX113: --take-onto and --take-from ask for opposite skeletons. */
static void
test_take_exclusive(void)
{
	char *both[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_takeonto, w_takefrom };
	char *bshort[] = { w_prog, s_from, v_from, s_onto, v_onto,
	    s_result, v_result, s_takefrom, s_takeonto };

	parse_bad(both, (int)NELEM(both));
	parse_bad(bshort, (int)NELEM(bshort));
}

/*
 * ZX114: a --take flag answers the skeleton a fresh run writes, and
 * no verb writes one; --restart writes the skeleton the record's own
 * word asks for, which is why the flag has nothing to say there
 * either.
 */
static void
test_take_on_verbs(void)
{
	char *verb[] = { w_continue, w_restart, w_abort, w_verify };
	char *take[] = { w_takeonto, w_takefrom };
	char *cmd[6];
	size_t i, j;

	for (i = 0; i < NELEM(verb); i++) {
		for (j = 0; j < NELEM(take); j++) {
			cmd[0] = w_prog;
			cmd[1] = verb[i];
			cmd[2] = v_result;
			cmd[3] = take[j];
			parse_bad(cmd, 4);
		}
	}
}

/*
 * ZX115, ZX116: --interactive and --no-merge belong to the two
 * commands that reach the conflicts gate -- a fresh run and
 * --continue -- and to nothing else.
 */
static void
test_gate_flags_on_verbs(void)
{
	char *verb[] = { w_restart, w_abort, w_verify };
	char *gate[] = { w_interactive, w_nomerge };
	char *cmd[6];
	char *run[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_interactive, w_nomerge };
	char *cont[] = { w_prog, w_continue, v_result, w_interactive,
	    w_nomerge };
	struct zr_args a;
	size_t i, j;

	for (i = 0; i < NELEM(verb); i++) {
		for (j = 0; j < NELEM(gate); j++) {
			cmd[0] = w_prog;
			cmd[1] = verb[i];
			cmd[2] = v_result;
			cmd[3] = gate[j];
			parse_bad(cmd, 4);
		}
	}
	a = parse_ok(run, (int)NELEM(run));
	CHECK(a.za_verb == ZR_VERB_RUN);
	CHECK(a.za_interactive == 1 && a.za_nomerge == 1);
	a = parse_ok(cont, (int)NELEM(cont));
	CHECK(a.za_verb == ZR_VERB_CONTINUE);
	CHECK(a.za_interactive == 1 && a.za_nomerge == 1);
}

/*
 * ZX117, ZX184: the two go together. --base is given where there is
 * no branch point to derive, and --allow-unrelated says there is
 * none: with nothing to derive and nothing given there is no third
 * tree to read the two sides against, and the empty tree that used
 * to stand there is gone (ruled 2026-09-06).
 */
static void
test_base_and_unrelated(void)
{
	char *base[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_base, v_base };
	char *unrel[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_unrelated };
	char *both[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_unrelated, w_base, v_base };
	char *dry[] = { w_prog, w_dryrun, w_from, v_from, w_onto, v_onto,
	    w_unrelated };
	struct zr_args a;

	parse_bad(base, (int)NELEM(base));
	parse_bad(unrel, (int)NELEM(unrel));
	unrel[7] = s_unrelated;
	parse_bad(unrel, (int)NELEM(unrel));
	parse_bad(dry, (int)NELEM(dry));
	a = parse_ok(both, (int)NELEM(both));
	CHECK(a.za_unrelated == 1 && a.za_base == v_base);
}

/*
 * ZX118: what is not a flag of ours. Short flags do not bundle, an
 * option needs the value it takes, and an operand where a flag
 * belongs is a refusal rather than the end of the options. The
 * retired spellings of the gate flag that --interactive replaced,
 * --no-gui and -G, are words of no one now and refused like any
 * other unknown option.
 */
static void
test_bad_words(void)
{
	static char unknown[] = "--no-such-flag";
	static char bundled[] = "-nv";
	static char letter[] = "-z";
	static char dashes[] = "--";
	static char dash[] = "-";
	static char attached[] = "-ftank/topic@work";
	static char nogui[] = "--no-gui";
	static char guiletter[] = "-G";
	char *cmd[8];
	char *words[] = { unknown, bundled, letter, dashes, dash, attached,
	    nogui, guiletter };
	size_t i;

	for (i = 0; i < NELEM(words); i++) {
		cmd[0] = w_prog;
		cmd[1] = w_from;
		cmd[2] = v_from;
		cmd[3] = w_onto;
		cmd[4] = v_onto;
		cmd[5] = w_result;
		cmd[6] = v_result;
		cmd[7] = words[i];
		parse_bad(cmd, 8);
	}
	/* A flag that takes a value, with nothing after it. */
	{
		char *hanging[] = { w_prog, w_from, v_from, w_onto, v_onto,
		    w_result };
		static char empty[] = "--result=";
		char *blank[] = { w_prog, w_from, v_from, w_onto, v_onto,
		    empty };

		parse_bad(hanging, (int)NELEM(hanging));
		parse_bad(blank, (int)NELEM(blank));
	}
	/* And no command at all. */
	{
		char *none[] = { w_prog };

		parse_bad(none, (int)NELEM(none));
	}
}

/*
 * ZX119: which way the skeleton is to be answered, as the struct
 * carries it. The word that reaches the record is run.c's, and the
 * property itself is the box's; what is settled here is that the
 * flag arrives and that neither flag means neither.
 */
static void
test_take_choice(void)
{
	char *plain[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result };
	char *cmd[8];
	struct zr_args a;

	a = parse_ok(plain, (int)NELEM(plain));
	CHECK(a.za_takeonto == 0 && a.za_takefrom == 0);
	memcpy(cmd, plain, sizeof (plain));
	cmd[7] = w_takeonto;
	a = parse_ok(cmd, 8);
	CHECK(a.za_takeonto == 1 && a.za_takefrom == 0);
	cmd[7] = w_takefrom;
	a = parse_ok(cmd, 8);
	CHECK(a.za_takeonto == 0 && a.za_takefrom == 1);
}

/*
 * ZX120, ZX138, ZX188: a verb names its run and takes the gate flags
 * --continue is allowed, the two sides and -v, and nothing of a
 * fresh run's: there is nothing there for --quiet to silence, since
 * the start latched that in the record, nothing for -o to choose,
 * since the start chose it and the record names it, and two verbs at
 * once are two commands. --verify is a verb of its own now, so it is
 * a second one beside each of the three that move a rebase.
 */
static void
test_verb_flags(void)
{
	char *noident[] = { w_prog, w_continue };
	char *withman[] = { w_prog, w_continue, v_result, w_manifest,
	    v_manifest };
	char *withquiet[] = { w_prog, w_continue, v_result, w_quiet };
	char *withmode[] = { w_prog, w_restart, v_result, w_perm };
	char *withdry[] = { w_prog, w_abort, v_result, w_dryrun };
	char *verifycont[] = { w_prog, w_continue, w_verify, v_result };
	char *verifyrest[] = { w_prog, w_restart, w_verify, v_result };
	char *verifyabrt[] = { w_prog, w_abort, w_verify, v_result };
	char *two[] = { w_prog, w_continue, w_abort, v_result };
	char *three[] = { w_prog, w_continue, w_restart, w_abort, v_result };

	parse_bad(noident, (int)NELEM(noident));
	parse_bad(withman, (int)NELEM(withman));
	parse_bad(withquiet, (int)NELEM(withquiet));
	parse_bad(withmode, (int)NELEM(withmode));
	parse_bad(withdry, (int)NELEM(withdry));
	parse_bad(verifycont, (int)NELEM(verifycont));
	parse_bad(verifyrest, (int)NELEM(verifyrest));
	parse_bad(verifyabrt, (int)NELEM(verifyabrt));
	parse_bad(two, (int)NELEM(two));
	parse_bad(three, (int)NELEM(three));
}

/*
 * ZX121: a fresh run names the two sides and, unless it is a dry
 * run, a name for what it makes. A dry run creates nothing that
 * would need one and ignores --result rather than demanding it.
 */
static void
test_run_flags(void)
{
	char *nofrom[] = { w_prog, w_onto, v_onto, w_result, v_result };
	char *noonto[] = { w_prog, w_from, v_from, w_result, v_result };
	char *noresult[] = { w_prog, w_from, v_from, w_onto, v_onto };
	char *dry[] = { w_prog, w_dryrun, w_from, v_from, w_onto, v_onto };
	char *dryres[] = { w_prog, w_dryrun, w_from, v_from, w_onto, v_onto,
	    w_result, v_result };
	struct zr_args a;

	parse_bad(nofrom, (int)NELEM(nofrom));
	parse_bad(noonto, (int)NELEM(noonto));
	parse_bad(noresult, (int)NELEM(noresult));
	a = parse_ok(dry, (int)NELEM(dry));
	CHECK(a.za_verb == ZR_VERB_RUN && a.za_dryrun == 1);
	CHECK(a.za_result == NULL);
	a = parse_ok(dryres, (int)NELEM(dryres));
	CHECK(a.za_dryrun == 1 && a.za_result == v_result);
}

/*
 * ZX138: --quiet is a start option and nothing else. It is latched
 * in the record for the whole run, so a verb that arrives later has
 * nothing to say about it, and a dry run makes no check whose report
 * there would be to silence.
 *
 * ZX139: --overwrite is gone, both spellings. A record that reached
 * done is no record at all now -- done takes it off -- so there is
 * nothing left for the flag to replace, and the word is refused the
 * way any word of another tool's is.
 */
static void
test_quiet_and_overwrite(void)
{
	static char w_over[] = "--overwrite";
	static char s_over[] = "-w";
	char *verb[] = { w_continue, w_restart, w_abort, w_verify };
	char *dry[] = { w_prog, w_dryrun, w_from, v_from, w_onto, v_onto,
	    w_quiet };
	char *run[] = { w_prog, w_from, v_from, w_onto, v_onto, w_result,
	    v_result };
	char *cmd[9];
	struct zr_args a;
	size_t i;

	/* --quiet on each of the four verbs, both spellings. */
	for (i = 0; i < NELEM(verb); i++) {
		cmd[0] = w_prog;
		cmd[1] = verb[i];
		cmd[2] = v_result;
		cmd[3] = w_quiet;
		parse_bad(cmd, 4);
		cmd[3] = s_quiet;
		parse_bad(cmd, 4);
	}
	/* and on a dry run, which has no check to report. */
	parse_bad(dry, (int)NELEM(dry));
	dry[6] = s_quiet;
	parse_bad(dry, (int)NELEM(dry));
	/* On a fresh run it is the flag it is. */
	memcpy(cmd, run, sizeof (run));
	cmd[7] = w_quiet;
	a = parse_ok(cmd, 8);
	CHECK(a.za_verb == ZR_VERB_RUN && a.za_quiet == 1);
	cmd[7] = s_quiet;
	a = parse_ok(cmd, 8);
	CHECK(a.za_verb == ZR_VERB_RUN && a.za_quiet == 1);
	a = parse_ok(run, (int)NELEM(run));
	CHECK(a.za_quiet == 0);
	/* And --overwrite is a word this tool does not know. */
	memcpy(cmd, run, sizeof (run));
	cmd[7] = w_over;
	parse_bad(cmd, 8);
	cmd[7] = s_over;
	parse_bad(cmd, 8);
	cmd[0] = w_prog;
	cmd[1] = w_continue;
	cmd[2] = v_result;
	cmd[3] = w_over;
	parse_bad(cmd, 4);
}

/*
 * ZX180, ZX181, ZX221, ZX222: a verb names its rebase with IDENT,
 * its one operand, and with nothing else. The operand stands
 * wherever it is written among the flags, since the tool has no
 * end-of-options word; one identifier names one rebase, so a second
 * operand is a refusal and not a second run; and --result, which
 * names the result of a run you are starting, is refused beside
 * every verb rather than read as the identifier.
 */
static void
test_ident_operand(void)
{
	char *verb[] = { w_continue, w_restart, w_abort, w_verify };
	enum zr_verb want[] = { ZR_VERB_CONTINUE, ZR_VERB_RESTART,
	    ZR_VERB_ABORT, ZR_VERB_REPORT };
	char *cmd[8];
	struct zr_args a;
	size_t i;

	for (i = 0; i < NELEM(verb); i++) {
		/* a name */
		cmd[0] = w_prog;
		cmd[1] = verb[i];
		cmd[2] = v_result;
		a = parse_ok(cmd, 3);
		CHECK(a.za_verb == want[i]);
		CHECK(a.za_ident == v_result && a.za_result == NULL);
		check_pair(cmd, 3);
		/* a path */
		cmd[2] = v_manifest;
		a = parse_ok(cmd, 3);
		CHECK(a.za_verb == want[i]);
		CHECK(a.za_ident == v_manifest);
		check_pair(cmd, 3);
		/* and before the flags as readily as after them */
		cmd[2] = v_result;
		cmd[3] = w_verbose;
		a = parse_ok(cmd, 4);
		CHECK(a.za_ident == v_result && a.za_verbose == 1);
		cmd[2] = w_verbose;
		cmd[3] = v_result;
		a = parse_ok(cmd, 4);
		CHECK(a.za_ident == v_result && a.za_verbose == 1);
		/* by neither */
		cmd[2] = w_verbose;
		parse_bad(cmd, 3);
		/* and two identifiers are two rebases */
		cmd[2] = v_result;
		cmd[3] = v_manifest;
		parse_bad(cmd, 4);
		/*
		 * ZX222: --result beside the verb, alone, beside an
		 * identifier and in both spellings. It is a start's
		 * flag and a verb takes none of it.
		 */
		cmd[2] = w_result;
		cmd[3] = v_result;
		parse_bad(cmd, 4);
		cmd[2] = s_result;
		parse_bad(cmd, 4);
		cmd[2] = w_result;
		cmd[3] = v_result;
		cmd[4] = v_manifest;
		parse_bad(cmd, 5);
		cmd[2] = w_result;
		cmd[3] = v_result;
		cmd[4] = w_from;
		cmd[5] = v_from;
		parse_bad(cmd, 6);
	}
}

/*
 * ZX183: -o is a start option and a dry run's. On a verb it is
 * refused, like --quiet: the start chose where the manifest goes,
 * the record names the path from then on and done acts on it, so
 * there is no later moment at which the choice could be made.
 */
static void
test_manifest_flag_on_verbs(void)
{
	char *verb[] = { w_continue, w_restart, w_abort, w_verify };
	char *cmd[7];
	struct zr_args a;
	size_t i;

	for (i = 0; i < NELEM(verb); i++) {
		cmd[0] = w_prog;
		cmd[1] = verb[i];
		cmd[2] = v_result;
		cmd[3] = w_manifest;
		cmd[4] = v_manifest;
		parse_bad(cmd, 5);
		cmd[3] = s_manifest;
		parse_bad(cmd, 5);
		/* and beside a path as the identifier, which is not -o */
		cmd[2] = v_manifest;
		cmd[3] = w_manifest;
		cmd[4] = v_manifest;
		parse_bad(cmd, 5);
	}
	/* On a start and on a dry run it is the flag it is. */
	{
		char *run[] = { w_prog, w_from, v_from, w_onto, v_onto,
		    w_result, v_result, w_manifest, v_manifest };
		char *dry[] = { w_prog, w_dryrun, w_from, v_from, w_onto,
		    v_onto, w_manifest, v_manifest };

		a = parse_ok(run, (int)NELEM(run));
		CHECK(a.za_verb == ZR_VERB_RUN);
		CHECK(a.za_manifest == v_manifest && a.za_ident == NULL);
		a = parse_ok(dry, (int)NELEM(dry));
		CHECK(a.za_dryrun == 1 && a.za_manifest == v_manifest);
	}
}

/*
 * ZX185: a start takes no operand, and neither does a dry run. An
 * identifier names a rebase that exists and a start makes one: what
 * a start has to say about where its manifest goes it says with -o,
 * and what it calls what it makes it says with --result.
 */
static void
test_operand_on_a_start(void)
{
	char *run[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, v_manifest };
	char *dry[] = { w_prog, w_dryrun, w_from, v_from, w_onto, v_onto,
	    v_manifest };
	char *first[] = { w_prog, v_manifest, w_from, v_from, w_onto,
	    v_onto, w_result, v_result };

	parse_bad(run, (int)NELEM(run));
	parse_bad(dry, (int)NELEM(dry));
	parse_bad(first, (int)NELEM(first));
}

/*
 * ZX186, ZX189: --from and --onto are accepted on a verb, where they
 * name no rebase and change nothing: the driver checks them against
 * the header by name and by guid. Every verb takes them the same
 * way, one or both, beside the identifier that says which rebase
 * this is. What no verb takes is --result, which is what a start
 * calls the thing it makes (ZX222, and the loop above).
 */
static void
test_sides_on_verbs(void)
{
	char *verb[] = { w_continue, w_restart, w_abort };
	char *cmd[9];
	struct zr_args a;
	size_t i;

	for (i = 0; i < NELEM(verb); i++) {
		cmd[0] = w_prog;
		cmd[1] = verb[i];
		cmd[2] = v_result;
		cmd[3] = w_from;
		cmd[4] = v_from;
		cmd[5] = w_onto;
		cmd[6] = v_onto;
		/* both sides beside the identifier */
		a = parse_ok(cmd, 7);
		CHECK(a.za_ident == v_result);
		CHECK(a.za_from == v_from && a.za_onto == v_onto);
		check_pair(cmd, 7);
		/* and one side alone */
		cmd[5] = w_verbose;
		a = parse_ok(cmd, 6);
		CHECK(a.za_from == v_from && a.za_onto == NULL);
		CHECK(a.za_ident == v_result);
		check_pair(cmd, 6);
		/* the identifier written as a path takes them too */
		cmd[2] = v_manifest;
		a = parse_ok(cmd, 6);
		CHECK(a.za_ident == v_manifest && a.za_from == v_from);
	}
	/* --verify on a path is the verb, sides or no sides. */
	cmd[0] = w_prog;
	cmd[1] = w_verify;
	cmd[2] = v_manifest;
	cmd[3] = w_from;
	cmd[4] = v_from;
	cmd[5] = w_onto;
	cmd[6] = v_onto;
	a = parse_ok(cmd, 7);
	CHECK(a.za_verb == ZR_VERB_REPORT && a.za_verify == 1);
	CHECK(a.za_ident == v_manifest && a.za_from == v_from);
	/* and beside a name, one side of it */
	cmd[2] = v_result;
	cmd[5] = w_verbose;
	a = parse_ok(cmd, 6);
	CHECK(a.za_verb == ZR_VERB_REPORT && a.za_from == v_from);
	CHECK(a.za_ident == v_result);
	/*
	 * ZX189: the sides with --result is the command that used to
	 * be a start with the flag, and it is refused rather than
	 * read as the report: a person who wrote it meant to rebase,
	 * and the checks they were asking for are standard now.
	 */
	cmd[2] = w_result;
	cmd[3] = v_result;
	cmd[4] = w_from;
	cmd[5] = v_from;
	cmd[6] = w_onto;
	cmd[7] = v_onto;
	parse_bad(cmd, 8);
	cmd[1] = s_verify;
	parse_bad(cmd, 8);
}

/*
 * ZX189: --verify is a verb and only a verb, so nothing that starts
 * a rebase goes with it. --dry-run is the other spelling of a start
 * and is refused by name; the flags only a start takes are refused
 * by the rule that refuses them on every verb, which the --verify
 * verb is now one of.
 */
static void
test_verify_is_a_verb(void)
{
	char *start[] = { w_manifest, w_quiet, w_perm, w_takeonto,
	    w_takefrom, w_result };
	char *cmd[8];
	size_t i;

	for (i = 0; i < NELEM(start); i++) {
		cmd[0] = w_prog;
		cmd[1] = w_verify;
		cmd[2] = v_result;
		cmd[3] = start[i];
		cmd[4] = v_manifest;	/* only -o and --result read it */
		parse_bad(cmd, start[i] == w_manifest ||
		    start[i] == w_result ? 5 : 4);
	}
	/* --dry-run, with the sides it needs and without them */
	{
		char *dry[] = { w_prog, w_verify, w_dryrun, v_result };
		char *dryf[] = { w_prog, w_verify, w_dryrun, w_from, v_from,
		    w_onto, v_onto };

		parse_bad(dry, (int)NELEM(dry));
		parse_bad(dryf, (int)NELEM(dryf));
	}
	/* and --allow-unrelated with the --base it needs */
	{
		char *unrel[] = { w_prog, w_verify, v_result, w_unrelated,
		    w_base, v_base };

		parse_bad(unrel, (int)NELEM(unrel));
	}
}

/*
 * ZX223, ZX224, ZX225: the two steps of the identifier's resolution
 * that read a document (zr_ident_manifest in src/run.c). A path is
 * resolved with realpath, the file is parsed, and the header's own
 * rule says which dataset carries the record; a path with no file at
 * it, a file that is no manifest, and a document that names no
 * rebase are each a refusal there, with the path in the line. The
 * step opens a file and no pool, so the whole of it answers on a
 * machine with no ZFS.
 *
 * The refusal is what makes step 1 an end and not a fall-through: an
 * absolute path is a path, and the tool does not go looking in the
 * pools for a dataset called /tmp/manifest.
 */
static void
test_ident_manifest(void)
{
	static char pre[] = "pre";
	char tmpl[256], path[512], gone[512], bent[512], real[512];
	struct zr_manifest_hdr hdr;
	struct zr_ident id;
	char err[512];
	FILE *fp;

	tmp_template(tmpl, sizeof (tmpl), "zrident.XXXXXX");
	CHECK(mkdtemp(tmpl) != NULL);
	(void) snprintf(path, sizeof (path), "%s/manifest", tmpl);
	(void) snprintf(bent, sizeof (bent), "%s/./manifest", tmpl);
	(void) snprintf(gone, sizeof (gone), "%s/no-such-manifest", tmpl);
	memset(&hdr, 0, sizeof (hdr));
	hdr.result = v_result;
	hdr.form = ZR_HFORM_CLONE;
	hdr.base = "tank/proj@v1";
	hdr.base_guid = 11;
	hdr.from = v_from;
	hdr.from_guid = 22;
	hdr.onto = v_onto;
	hdr.onto_guid = 33;
	hdr.made = "-";
	hdr.tag = "zr-0f1e2d3c4b5a";
	hdr.take = "-";
	hdr.written = "2026-09-08T10:11:12Z";
	hdr.mode = ZR_MODE_STRICT;
	err[0] = '\0';
	CHECK(zr_doc_write(path, emit_birth, &hdr, err, sizeof (err)) == 0);

	/* ZX223: the clone form, whose #result is the result itself */
	err[0] = 'x';
	CHECK(zr_ident_manifest(path, &id, err, sizeof (err)) == 0);
	CHECK(strcmp(id.zi_result, v_result) == 0);
	CHECK(id.zi_path[0] == '/' && strstr(id.zi_path, "/manifest") != NULL);
	CHECK(id.zi_parsed == 1 && id.zi_rundir == 0);
	CHECK(id.zi_man.zp_onto != NULL);
	CHECK(strcmp(id.zi_man.zp_onto, v_onto) == 0);
	(void) snprintf(real, sizeof (real), "%s", id.zi_path);
	zr_ident_fini(&id);
	CHECK(id.zi_parsed == 0);

	/*
	 * ZX223: and the path is resolved, not copied: the same file
	 * named another way is the same manifest, which is what the
	 * record is compared with afterwards.
	 */
	CHECK(zr_ident_manifest(bent, &id, err, sizeof (err)) == 0);
	CHECK(strcmp(id.zi_path, real) == 0);
	zr_ident_fini(&id);

	/*
	 * ZX223: and the dataset form, where #result is the pre-apply
	 * snapshot and the dataset that carries the record is #onto's.
	 * The same rule zr_run_dataset holds over a header in memory,
	 * made here over a document on disk.
	 */
	hdr.form = ZR_HFORM_DATASET;
	hdr.result = pre;
	hdr.presnap = "tank/main@pre";
	hdr.readonly = "off";
	hdr.canmount = "on";
	hdr.onto = "tank/main@now";
	CHECK(zr_doc_write(path, emit_birth, &hdr, err, sizeof (err)) == 0);
	CHECK(zr_ident_manifest(path, &id, err, sizeof (err)) == 0);
	CHECK(strcmp(id.zi_result, "tank/main") == 0);
	zr_ident_fini(&id);

	/* ZX225: a dry run's document, which names no rebase */
	hdr.form = ZR_HFORM_CLONE;
	hdr.result = "-";
	CHECK(zr_doc_write(path, emit_birth, &hdr, err, sizeof (err)) == 0);
	err[0] = '\0';
	CHECK(zr_ident_manifest(path, &id, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0' && strstr(err, real) != NULL);
	CHECK(id.zi_result[0] == '\0');
	zr_ident_fini(&id);

	/* ZX224: a file that is no manifest at all */
	fp = fopen(path, "w");
	CHECK(fp != NULL);
	CHECK(fputs("this is not a manifest\n", fp) != EOF);
	CHECK(fclose(fp) == 0);
	err[0] = '\0';
	CHECK(zr_ident_manifest(path, &id, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0' && strstr(err, real) != NULL);
	CHECK(id.zi_parsed == 0);
	zr_ident_fini(&id);

	/* ZX224: and a path with nothing at it, which is the refusal */
	err[0] = '\0';
	CHECK(zr_ident_manifest(gone, &id, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0' && strstr(err, gone) != NULL);
	CHECK(id.zi_parsed == 0 && id.zi_result[0] == '\0');
	zr_ident_fini(&id);

	CHECK(unlink(path) == 0);
	CHECK(rmdir(tmpl) == 0);
}

/*
 * ZX187: the rule that reads a run's dataset out of a manifest's
 * header (zr_run_dataset in src/run.c). The clone form's #result is
 * the clone itself; the dataset form's is the pre-apply snapshot as
 * --result spelled it, so the dataset is #onto's, stripped at the
 * '@'. A --posix document names no rebase and a dry run's "-" names
 * none either. It reads a parsed header and opens nothing, which is
 * why it is tested here beside the command line.
 */
static void
test_run_dataset(void)
{
	static char clone[] = "tank/rebased";
	static char onto[] = "tank/main@now";
	static char pre[] = "pre";
	static char preful[] = "tank/main@pre";
	static char dash[] = "-";
	struct zr_parsed p;
	char ds[64], err[512];

	memset(&p, 0, sizeof (p));
	p.zp_form = ZR_HFORM_CLONE;
	p.zp_result = clone;
	p.zp_onto = onto;
	CHECK(zr_run_dataset(&p, ds, sizeof (ds), err, sizeof (err)) == 0);
	CHECK(strcmp(ds, clone) == 0);

	/* the dataset form, --result spelled short and spelled full */
	p.zp_form = ZR_HFORM_DATASET;
	p.zp_result = pre;
	p.zp_onto = preful;
	CHECK(zr_run_dataset(&p, ds, sizeof (ds), err, sizeof (err)) == 0);
	CHECK(strcmp(ds, "tank/main") == 0);
	p.zp_result = preful;
	CHECK(zr_run_dataset(&p, ds, sizeof (ds), err, sizeof (err)) == 0);
	CHECK(strcmp(ds, "tank/main") == 0);

	/* a header with no result: a dry run's, and no rebase was made */
	p.zp_form = ZR_HFORM_CLONE;
	p.zp_result = dash;
	err[0] = '\0';
	CHECK(zr_run_dataset(&p, ds, sizeof (ds), err, sizeof (err)) == -1);
	CHECK(err[0] != '\0' && ds[0] == '\0');
	p.zp_result = NULL;
	CHECK(zr_run_dataset(&p, ds, sizeof (ds), err, sizeof (err)) == -1);

	/* a --posix document, which describes no rebase at all */
	p.zp_form = ZR_HFORM_POSIX;
	p.zp_result = clone;
	CHECK(zr_run_dataset(&p, ds, sizeof (ds), err, sizeof (err)) == -1);

	/* the dataset form with no onto: the dataset is not in it */
	p.zp_form = ZR_HFORM_DATASET;
	p.zp_result = pre;
	p.zp_onto = NULL;
	CHECK(zr_run_dataset(&p, ds, sizeof (ds), err, sizeof (err)) == -1);
	/* and a name that will not fit is no name */
	p.zp_onto = preful;
	CHECK(zr_run_dataset(&p, ds, 4, err, sizeof (err)) == -1);
}

int
main(void)
{
	test_pairs_run();
	test_pairs_verbs();
	test_aliases();
	test_equals_form();
	test_harness_verbs();
	test_take_exclusive();
	test_take_on_verbs();
	test_gate_flags_on_verbs();
	test_base_and_unrelated();
	test_bad_words();
	test_take_choice();
	test_verb_flags();
	test_run_flags();
	test_quiet_and_overwrite();
	test_ident_operand();
	test_manifest_flag_on_verbs();
	test_operand_on_a_start();
	test_sides_on_verbs();
	test_verify_is_a_verb();
	test_ident_manifest();
	test_run_dataset();
	printf("check_args: %lu checks passed\n", checks);
	return (0);
}
