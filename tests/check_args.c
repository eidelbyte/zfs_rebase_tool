/*
 * Tests for the command line (src/args.c): every long form and its
 * short form parsing to the same run, the aliases, the two ways a
 * long form carries its value, the harness verbs, and every refusal
 * the parse makes. Nothing here opens a file or looks at a dataset,
 * which is what puts the whole of the driver's grammar within reach
 * of a machine with no ZFS in it.
 *
 * Matrix cells (tests/MATRIX.md, family ZX): ZX100 to ZX121.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"

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
static char w_nogui[] = "--no-gui";
static char w_nomerge[] = "--no-merge";
static char w_continue[] = "--continue";
static char w_restart[] = "--restart";
static char w_abort[] = "--abort";
static char w_dryrun[] = "--dry-run";
static char w_overwrite[] = "--overwrite";
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
static char s_nogui[] = "-G";
static char s_nomerge[] = "-M";
static char s_continue[] = "-c";
static char s_restart[] = "-R";
static char s_abort[] = "-a";
static char s_dryrun[] = "-n";
static char s_overwrite[] = "-w";
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
	{ w_nogui, s_nogui },
	{ w_nomerge, s_nomerge },
	{ w_continue, s_continue },
	{ w_restart, s_restart },
	{ w_abort, s_abort },
	{ w_dryrun, s_dryrun },
	{ w_overwrite, s_overwrite },
	{ w_unrelated, s_unrelated },
	{ w_base, s_base }
};

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
 * ZX100, ZX102, ZX103, ZX104, ZX105, ZX106: the fresh run's flags,
 * one command per flag added to the smallest run that is legal, each
 * written both ways and parsed to the same struct.
 */
static void
test_pairs_run(void)
{
	char *base[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result };
	char *with[10];
	char *one[] = { w_perm, w_verbose, w_verify, w_takeonto, w_takefrom,
	    w_nogui, w_nomerge, w_overwrite, w_unrelated };
	char *val[] = { w_manifest, w_base };
	size_t i;
	int n;

	/* ZX100: --from, --onto and --result themselves. */
	check_pair(base, (int)NELEM(base));

	/* ZX102, ZX103, ZX105, ZX106: one flag at a time, no value. */
	for (i = 0; i < NELEM(one); i++) {
		memcpy(with, base, sizeof (base));
		n = (int)NELEM(base);
		with[n++] = one[i];
		check_pair(with, n);
	}
	/*
	 * ZX102, ZX103: the two that take a value. --base is legal
	 * only with --allow-unrelated, which goes on with it.
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
	char *cont[] = { w_prog, w_continue, w_result, v_result };
	char *contv[] = { w_prog, w_continue, w_verify, w_nogui, w_nomerge,
	    w_verbose, w_result, v_result };
	char *rest[] = { w_prog, w_restart, w_result, v_result };
	char *abrt[] = { w_prog, w_abort, w_verbose, w_result, v_result };
	char *rep[] = { w_prog, w_verify, w_result, v_result };
	struct zr_args a;

	check_pair(cont, (int)NELEM(cont));
	check_pair(contv, (int)NELEM(contv));
	check_pair(rest, (int)NELEM(rest));
	check_pair(abrt, (int)NELEM(abrt));
	check_pair(rep, (int)NELEM(rep));

	a = parse_ok(cont, (int)NELEM(cont));
	CHECK(a.za_verb == ZR_VERB_CONTINUE);
	CHECK(a.za_result == v_result);
	a = parse_ok(contv, (int)NELEM(contv));
	CHECK(a.za_verb == ZR_VERB_CONTINUE);
	CHECK(a.za_verify == 1 && a.za_nogui == 1 && a.za_nomerge == 1);
	CHECK(a.za_verbose == 1);
	a = parse_ok(rest, (int)NELEM(rest));
	CHECK(a.za_verb == ZR_VERB_RESTART);
	a = parse_ok(abrt, (int)NELEM(abrt));
	CHECK(a.za_verb == ZR_VERB_ABORT && a.za_verbose == 1);
	/*
	 * ZX110: --verify alone on a result is the verb, and with a
	 * rebase it is the flag; one word, and the two sides told
	 * apart by whether the command names the sides.
	 */
	a = parse_ok(rep, (int)NELEM(rep));
	CHECK(a.za_verb == ZR_VERB_REPORT && a.za_verify == 1);
	{
		char *run[] = { w_prog, w_verify, w_from, v_from, w_onto,
		    v_onto, w_result, v_result };

		a = parse_ok(run, (int)NELEM(run));
		CHECK(a.za_verb == ZR_VERB_RUN && a.za_verify == 1);
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
			cmd[2] = w_result;
			cmd[3] = v_result;
			cmd[4] = take[j];
			parse_bad(cmd, 5);
		}
	}
}

/*
 * ZX115, ZX116: --no-gui and --no-merge belong to the two commands
 * that reach the conflicts gate -- a fresh run and --continue -- and
 * to nothing else.
 */
static void
test_gate_flags_on_verbs(void)
{
	char *verb[] = { w_restart, w_abort, w_verify };
	char *gate[] = { w_nogui, w_nomerge };
	char *cmd[6];
	char *run[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_nogui, w_nomerge };
	char *cont[] = { w_prog, w_continue, w_result, v_result, w_nogui,
	    w_nomerge };
	struct zr_args a;
	size_t i, j;

	for (i = 0; i < NELEM(verb); i++) {
		for (j = 0; j < NELEM(gate); j++) {
			cmd[0] = w_prog;
			cmd[1] = verb[i];
			cmd[2] = w_result;
			cmd[3] = v_result;
			cmd[4] = gate[j];
			parse_bad(cmd, 5);
		}
	}
	a = parse_ok(run, (int)NELEM(run));
	CHECK(a.za_verb == ZR_VERB_RUN);
	CHECK(a.za_nogui == 1 && a.za_nomerge == 1);
	a = parse_ok(cont, (int)NELEM(cont));
	CHECK(a.za_verb == ZR_VERB_CONTINUE);
	CHECK(a.za_nogui == 1 && a.za_nomerge == 1);
}

/* ZX117: --base is given where there is no branch point to derive. */
static void
test_base_wants_unrelated(void)
{
	char *alone[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_base, v_base };
	char *with[] = { w_prog, w_from, v_from, w_onto, v_onto,
	    w_result, v_result, w_unrelated, w_base, v_base };
	struct zr_args a;

	parse_bad(alone, (int)NELEM(alone));
	a = parse_ok(with, (int)NELEM(with));
	CHECK(a.za_unrelated == 1 && a.za_base == v_base);
}

/*
 * ZX118: what is not a flag of ours. Short flags do not bundle, an
 * option needs the value it takes, and an operand where a flag
 * belongs is a refusal rather than the end of the options.
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
	char *cmd[8];
	char *words[] = { unknown, bundled, letter, dashes, dash, attached };
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
 * ZX120: a verb takes --result, the gate flags --continue is allowed
 * and -v, and nothing of a fresh run's: there is nothing there for
 * --overwrite to replace or for --from to name, and two verbs at
 * once are two commands.
 */
static void
test_verb_flags(void)
{
	char *noresult[] = { w_prog, w_continue };
	char *withfrom[] = { w_prog, w_abort, w_result, v_result,
	    w_from, v_from };
	char *withman[] = { w_prog, w_continue, w_result, v_result,
	    w_manifest, v_manifest };
	char *withover[] = { w_prog, w_continue, w_result, v_result,
	    w_overwrite };
	char *withmode[] = { w_prog, w_restart, w_result, v_result, w_perm };
	char *withdry[] = { w_prog, w_abort, w_result, v_result, w_dryrun };
	char *verifyrest[] = { w_prog, w_restart, w_verify, w_result,
	    v_result };
	char *verifyabrt[] = { w_prog, w_abort, w_verify, w_result,
	    v_result };
	char *two[] = { w_prog, w_continue, w_abort, w_result, v_result };
	char *three[] = { w_prog, w_continue, w_restart, w_abort,
	    w_result, v_result };

	parse_bad(noresult, (int)NELEM(noresult));
	parse_bad(withfrom, (int)NELEM(withfrom));
	parse_bad(withman, (int)NELEM(withman));
	parse_bad(withover, (int)NELEM(withover));
	parse_bad(withmode, (int)NELEM(withmode));
	parse_bad(withdry, (int)NELEM(withdry));
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
	test_base_wants_unrelated();
	test_bad_words();
	test_take_choice();
	test_verb_flags();
	test_run_flags();
	printf("check_args: %lu checks passed\n", checks);
	return (0);
}
