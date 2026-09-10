/*
 * The run's guards that need no pool, and the launcher. A real run
 * wants ZFS, root and a box; what is here is the questions the run
 * asks that are pure functions of a decision and the two walks
 * beside it, and the fork that opens a child on the resolution,
 * which any machine can answer.
 *
 * The first is the system file flags against the securelevel. The
 * sysctl that says what securelevel this box is at is FreeBSD's and
 * is the box's, but the rule it feeds takes the level as an
 * argument, so the rule itself is asked here over a decision and two
 * walks built by hand -- three trees of two names, one of them
 * changed on from and the other the same everywhere, decided in
 * strict mode, with the flags put on the attributes of whichever
 * pool the case is about.
 *
 * The second is src/launch.c, driven with small shell scripts as the
 * child: a script that records the arguments it was given, exits as
 * it is told, signals its own parent or the tool's, or puts a pty
 * into raw mode to see the terminal handed back. Nothing here needs
 * a pool either: the launcher knows nothing of ZFS, and the gate
 * that calls it is the box's.
 *
 * Matrix cells (tests/MATRIX.md): ZX242, and ZI13 to ZI23 of family
 * ZI. ZX23, the refusal a real run makes at a real securelevel,
 * stays the box's: raising the level wants a reboot. ZI24 onward are
 * the gate after the child, on real datasets, in box/run-resolution.sh.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "decide.h"
#include "launch.h"
#include "name.h"
#include "run.h"
#include "walk.h"

static int checks;

#define	CHECK(x)							\
	do {								\
		checks++;						\
		if (!(x)) {						\
			printf("%s:%d: check failed: %s\n", __FILE__,	\
			    __LINE__, #x);				\
			exit(1);					\
		}							\
	} while (0)

static struct zr_names *names;

/* A walk with no filesystem under it: the tree and the attributes. */
static void
walk_init(struct zr_walk *w)
{
	memset(w, 0, sizeof (struct zr_walk));
	w->zw_rootfd = -1;
	CHECK(zr_tree_init(&w->zw_tree, names) == 0);
}

/* One name, its own pool, and the content handle the case wants. */
static zr_pool_t
walk_add(struct zr_walk *w, const char *path, uint64_t ino, uint32_t content)
{
	zr_name_t n;
	zr_pool_t q;

	n = zr_names_intern(names, path, strlen(path));
	CHECK(n != ZR_NAME_NONE);
	q = zr_tree_add(&w->zw_tree, n, ino, ZR_T_FILE, 1);
	CHECK(q != ZR_POOL_NONE);
	w->zw_tree.zt_pools[q].zp_content = content;
	return (q);
}

/* Sealed, with one attribute record per pool and no flags on any. */
static void
walk_seal(struct zr_walk *w)
{
	CHECK(zr_tree_seal(&w->zw_tree) == 0);
	w->zw_nattrs = w->zw_tree.zt_npools;
	w->zw_attrs = calloc((size_t)(w->zw_nattrs == 0 ? 1 : w->zw_nattrs),
	    sizeof (struct zr_attr));
	CHECK(w->zw_attrs != NULL);
}

static void
walk_free(struct zr_walk *w)
{
	free(w->zw_attrs);
	w->zw_attrs = NULL;
	w->zw_nattrs = 0;
	zr_tree_fini(&w->zw_tree);
}

/* The refusal must name this word, and the message is printed if not. */
static void
says(const char *err, const char *word)
{
	if (strstr(err, word) == NULL)
		printf("  message lacks \"%s\": %s\n", word, err);
	CHECK(strstr(err, word) != NULL);
}

/*
 * ZX242: the flag guard, both sides of it. /a is base's and onto's
 * still and changed on from, so the decision writes from's object
 * there and rewrites onto's; /b is the same object in all three and
 * the decision says nothing about it at all.
 */
static void
check_flags_guard(void)
{
	struct zr_walk wb, wf, wo;
	struct zr_decision d;
	char err[512];
	zr_pool_t fa, oa, fb, ob;

	names = zr_names_create();
	CHECK(names != NULL);
	walk_init(&wb);
	walk_init(&wf);
	walk_init(&wo);
	(void) walk_add(&wb, "/a", 1, 10);
	(void) walk_add(&wb, "/b", 2, 30);
	fa = walk_add(&wf, "/a", 1, 20);
	fb = walk_add(&wf, "/b", 2, 30);
	oa = walk_add(&wo, "/a", 1, 10);
	ob = walk_add(&wo, "/b", 2, 30);
	walk_seal(&wb);
	walk_seal(&wf);
	walk_seal(&wo);
	CHECK(zr_decide(&wb.zw_tree, &wf.zw_tree, &wo.zw_tree,
	    ZR_MODE_STRICT, &d) == 0);

	/* nothing carries a flag: nothing to refuse, at any level */
	err[0] = 'x';
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 0);
	CHECK(err[0] == '\0');

#if defined(SF_IMMUTABLE) && defined(SF_APPEND) && defined(SF_NOUNLINK)
	/* onto's side: the apply would have to clear it, and cannot */
	wo.zw_attrs[oa].za_flags = (uint32_t)SF_IMMUTABLE;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 1);
	says(err, "securelevel 1");
	says(err, "/a");
	says(err, "onto's side");

	/* at securelevel 0 the apply clears it itself: nothing refused */
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 0, err,
	    sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	CHECK(zr_flags_refused(&d, &wo, &wf, names, -1, err,
	    sizeof (err)) == 0);

	/* from's side: the apply would write it on, and it would stick */
	wo.zw_attrs[oa].za_flags = 0;
	wf.zw_attrs[fa].za_flags = (uint32_t)SF_NOUNLINK;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 1);
	says(err, "/a");
	says(err, "from's side");

	/* both at once: onto's is read first, and it is the one named */
	wo.zw_attrs[oa].za_flags = (uint32_t)SF_APPEND;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 1);
	says(err, "onto's side");

	/*
	 * A flag on the name the decision leaves alone, on either side
	 * or on both: the apply never touches that object, so there is
	 * nothing the securelevel could stop.
	 */
	wo.zw_attrs[oa].za_flags = 0;
	wf.zw_attrs[fa].za_flags = 0;
	wo.zw_attrs[ob].za_flags = (uint32_t)SF_IMMUTABLE;
	wf.zw_attrs[fb].za_flags = (uint32_t)SF_IMMUTABLE;
	CHECK(zr_flags_refused(&d, &wo, &wf, names, 1, err,
	    sizeof (err)) == 0);
	CHECK(err[0] == '\0');
#else
	(void) fa;
	(void) oa;
	(void) fb;
	(void) ob;
	printf("skip the flag cases: this platform has no system flags\n");
#endif

	zr_decision_fini(&d);
	walk_free(&wo);
	walk_free(&wf);
	walk_free(&wb);
	zr_names_destroy(names);
	names = NULL;
}

/*
 * ---------------------------------------------------------------
 * The launcher: family ZI, cells ZI13 to ZI23.
 * ---------------------------------------------------------------
 */

/*
 * The scratch a launch case wants: a directory of its own, a file
 * standing in for the resolution, a script to be the child, and a
 * record file the script writes what it was given into. The path of
 * that record and the pid of this process go to the child in the
 * environment, which passes through the fork untouched, so that the
 * argv the child is given holds nothing but what the launcher put
 * there and a script that signals the tool knows which pid to aim
 * at whether the shell exec'd it or forked it.
 */
#define	ZL_LINE		1024
#define	ZL_RESBYTES	"# not a real resolution, and nothing reads it\n"

struct scratch {
	char	root[ZL_LINE];
	char	res[ZL_LINE];
	char	rec[ZL_LINE];
	char	script[ZL_LINE];
};

static void
scratch_open(struct scratch *sc)
{
	const char *d = getenv("TMPDIR");
	char pid[32];
	FILE *fp;

	(void) snprintf(sc->root, sizeof (sc->root), "%s/zrlaunch.XXXXXX",
	    d != NULL && d[0] != '\0' ? d : "/tmp");
	CHECK(mkdtemp(sc->root) != NULL);
	(void) snprintf(sc->res, sizeof (sc->res), "%s/resolution", sc->root);
	(void) snprintf(sc->rec, sizeof (sc->rec), "%s/record", sc->root);
	(void) snprintf(sc->script, sizeof (sc->script), "%s/child.sh",
	    sc->root);
	fp = fopen(sc->res, "w");
	CHECK(fp != NULL);
	CHECK(fputs(ZL_RESBYTES, fp) != EOF);
	CHECK(fclose(fp) == 0);
	(void) snprintf(pid, sizeof (pid), "%ld", (long)getpid());
	CHECK(setenv("ZR_LAUNCH_RECORD", sc->rec, 1) == 0);
	CHECK(setenv("ZR_LAUNCH_TOOL", pid, 1) == 0);
}

static void
scratch_close(struct scratch *sc)
{
	(void) unlink(sc->script);
	(void) unlink(sc->rec);
	(void) unlink(sc->res);
	CHECK(rmdir(sc->root) == 0);
}

/* The child, as a shell script this test wrote and can run. */
static void
write_script(const struct scratch *sc, const char *body)
{
	FILE *fp;

	fp = fopen(sc->script, "w");
	CHECK(fp != NULL);
	CHECK(fputs("#!/bin/sh\n", fp) != EOF);
	CHECK(fputs(body, fp) != EOF);
	CHECK(fclose(fp) == 0);
	CHECK(chmod(sc->script, 0755) == 0);
}

/* What the script wrote down, one argument per line. */
static int
record_lines(const struct scratch *sc, char line[][ZL_LINE], int max)
{
	FILE *fp;
	size_t len;
	int n = 0;

	fp = fopen(sc->rec, "r");
	if (fp == NULL)
		return (0);
	while (n < max && fgets(line[n], ZL_LINE, fp) != NULL) {
		len = strlen(line[n]);
		if (len > 0 && line[n][len - 1] == '\n')
			line[n][len - 1] = '\0';
		n++;
	}
	CHECK(fclose(fp) == 0);
	return (n);
}

/* The bytes of a file, which is how "untouched" is asserted. */
static void
file_is(const char *path, const char *want)
{
	char buf[ZL_LINE];
	FILE *fp;
	size_t n;

	fp = fopen(path, "r");
	CHECK(fp != NULL);
	n = fread(buf, 1, sizeof (buf) - 1, fp);
	buf[n] = '\0';
	CHECK(fclose(fp) == 0);
	CHECK(strcmp(buf, want) == 0);
}

/* A launch of this script, with the four tree paths a picker gets. */
static void
launch_on(struct zr_launch *lp, const struct scratch *sc, const char *cmd)
{
	memset(lp, 0, sizeof (*lp));
	lp->command = cmd;
	lp->resolution = sc->res;
	lp->base = "/b";
	lp->from = "/f";
	lp->onto = "/o";
	lp->result = "/r";
}

/* The script run with no launcher at all, for a positive proof. */
static void
run_bare(const char *script)
{
	pid_t pid;
	int status = 0;

	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		(void) execl(script, script, (char *)NULL);
		_exit(127);
	}
	while (waitpid(pid, &status, 0) < 0)
		CHECK(errno == EINTR);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/*
 * ZI13, ZI14: what the child is given. The resolution's path is the
 * last argument, byte for byte, and a command that carries its own
 * flags reaches the child as those flags and then the path, in
 * order: the value is one string and the tool never splits it, so
 * the shell splits it as it splits any command line and the path
 * arrives as a positional parameter after it.
 *
 * ZI13 is the family's positive proof: a process really ran on the
 * document, since the record file is the child's own writing.
 */
static void
check_child_arguments(void)
{
	struct zr_launch lp;
	struct scratch sc;
	char line[4][ZL_LINE];
	char cmd[ZL_LINE * 2];
	char err[512];
	int n;

	scratch_open(&sc);
	write_script(&sc, "for a in \"$@\"; do\n"
	    "\tprintf '%s\\n' \"$a\" >> \"$ZR_LAUNCH_RECORD\"\n"
	    "done\nexit 0\n");

	/* ZI13: the path alone, and it is the path we gave. */
	launch_on(&lp, &sc, sc.script);
	err[0] = 'x';
	CHECK(zr_launch(&lp, err, sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	n = record_lines(&sc, line, 4);
	CHECK(n == 1);
	CHECK(strcmp(line[0], sc.res) == 0);

	/* ZI14: "CMD --flag" is one command, and --flag comes first. */
	CHECK(unlink(sc.rec) == 0);
	(void) snprintf(cmd, sizeof (cmd), "%s --flag", sc.script);
	launch_on(&lp, &sc, cmd);
	CHECK(zr_launch(&lp, err, sizeof (err)) == 0);
	n = record_lines(&sc, line, 4);
	CHECK(n == 2);
	CHECK(strcmp(line[0], "--flag") == 0);
	CHECK(strcmp(line[1], sc.res) == 0);
	scratch_close(&sc);
}

/*
 * ZI15, ZI16: the status the child exited with is the whole of the
 * answer. 0 is the one that lets the tool go on and leaves no
 * message; anything else is reported with the number in it. The
 * launcher does not touch the document either way -- reading it back
 * is the gate's work and not the launcher's -- so the file is as the
 * child left it, which here is as the test wrote it.
 */
static void
check_child_status(void)
{
	struct zr_launch lp;
	struct scratch sc;
	char err[512];

	scratch_open(&sc);
	write_script(&sc, "exit 0\n");
	launch_on(&lp, &sc, sc.script);
	err[0] = 'x';
	CHECK(zr_launch(&lp, err, sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	file_is(sc.res, ZL_RESBYTES);

	write_script(&sc, "exit 3\n");
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	says(err, "3");
	file_is(sc.res, ZL_RESBYTES);
	scratch_close(&sc);
}

/*
 * ZI17: a child that dies of a signal is not a child that exited,
 * and the reason says which signal it was, by number and by the name
 * the system gives it.
 */
static void
check_child_signal(void)
{
	struct zr_launch lp;
	struct scratch sc;
	char err[512];

	scratch_open(&sc);
	write_script(&sc, "kill -TERM $$\nsleep 5\n");
	launch_on(&lp, &sc, sc.script);
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	says(err, "signal");
	says(err, strsignal(SIGTERM));
	scratch_close(&sc);
}

/*
 * ZI18: a command that is not there at all. The shell says 127 for
 * it, which is the one status that means the command never ran, and
 * the reason names the command rather than blaming an editor that
 * never started. Its own complaint goes to /dev/null, which is the
 * test keeping the check's output clean and no part of the rule.
 */
static void
check_child_missing(void)
{
	struct zr_launch lp;
	struct scratch sc;
	static const char cmd[] = "zr-no-such-command-ZI18 2>/dev/null";
	char err[512];

	scratch_open(&sc);
	launch_on(&lp, &sc, cmd);
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	says(err, "zr-no-such-command-ZI18");
	says(err, "could not be run");
	scratch_close(&sc);
}

/* The disposition of one signal, for the before-and-after of ZI19. */
static void
disposition(int sig, struct sigaction *out)
{
	memset(out, 0, sizeof (*out));
	CHECK(sigaction(sig, NULL, out) == 0);
}

/*
 * ZI19: SIGINT while the tool waits belongs to the child. The
 * launcher ignores it the way system(3) does, so a script that sends
 * one to the tool and then exits 0 is reported 0 and this process is
 * still here to say so. The three dispositions the launcher put on
 * are the ones it found again afterwards, which is what keeps the
 * run's own handlers from being lost at the gate.
 */
static void
check_parent_interrupt(void)
{
	struct sigaction bint, bquit, bterm, aint, aquit, aterm;
	struct zr_launch lp;
	struct scratch sc;
	char err[512];

	scratch_open(&sc);
	write_script(&sc, "kill -INT $ZR_LAUNCH_TOOL\nexit 0\n");
	launch_on(&lp, &sc, sc.script);
	disposition(SIGINT, &bint);
	disposition(SIGQUIT, &bquit);
	disposition(SIGTERM, &bterm);
	CHECK(zr_launch(&lp, err, sizeof (err)) == 0);
	disposition(SIGINT, &aint);
	disposition(SIGQUIT, &aquit);
	disposition(SIGTERM, &aterm);
	CHECK(aint.sa_handler == bint.sa_handler);
	CHECK(aquit.sa_handler == bquit.sa_handler);
	CHECK(aterm.sa_handler == bterm.sa_handler);
	scratch_close(&sc);
}

/*
 * ZI20: SIGTERM to the tool while it waits is forwarded to the
 * child. The script sends one and then sleeps five seconds; the
 * launcher passes it on, the sleep never finishes, the launch is
 * reported non-zero, and this process -- which would have died of
 * that signal with no handler on it -- is still here. The clock is
 * the assertion that the forward happened: without it the wait would
 * have run the sleep out.
 */
static void
check_parent_terminate(void)
{
	struct zr_launch lp;
	struct scratch sc;
	time_t t0, t1;
	char err[512];

	scratch_open(&sc);
	write_script(&sc, "kill -TERM $ZR_LAUNCH_TOOL\nsleep 5\n");
	launch_on(&lp, &sc, sc.script);
	t0 = time(NULL);
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	t1 = time(NULL);
	CHECK(t1 - t0 < 5);
	/*
	 * And a child that catches the forwarded signal and leaves
	 * with 0: the tool was told to stop, so the launch is still
	 * reported non-zero, naming the signal. The sleep goes to the
	 * background and the wait is what the trap interrupts, since a
	 * shell runs a trap only once its foreground command is over.
	 */
	write_script(&sc, "trap 'exit 0' TERM\nkill -TERM $ZR_LAUNCH_TOOL\n"
	    "sleep 5 &\nwait\nexit 0\n");
	t0 = time(NULL);
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	t1 = time(NULL);
	CHECK(t1 - t0 < 5);
	says(err, "SIGTERM");
	scratch_close(&sc);
}

/*
 * ZI21: the terminal handed back as it was found (ground rule 6).
 * The test opens a pty, puts the slave on its own standard input so
 * that the launcher's isatty sees a terminal, and runs a script that
 * puts it into raw mode.
 *
 * The same script is run once with no launcher first, and ICANON is
 * what that half asserts on: it is on in a fresh terminal and off in
 * a raw one, so the script really changes the termios and the
 * equality below is not a comparison of two terminals nobody
 * touched. The bytes to compare against are read after that trial
 * and not before it, because a tcsetattr of any settings at all
 * leaves the kernel's own PENDIN bit in what the next tcgetattr
 * gives back: the question this cell asks is whether the launcher
 * gives back what it saved, and what it saved is what stands here.
 */
static void
check_terminal_restored(void)
{
	struct termios orig, raw, before, after;
	struct zr_launch lp;
	struct scratch sc;
	const char *name;
	char err[512];
	int master, slave, keep;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0) {
		printf("skip ZI21: no pty here (%s)\n", strerror(errno));
		return;
	}
	CHECK(grantpt(master) == 0);
	CHECK(unlockpt(master) == 0);
	name = ptsname(master);
	CHECK(name != NULL);
	slave = open(name, O_RDWR | O_NOCTTY);
	CHECK(slave >= 0);
	scratch_open(&sc);
	write_script(&sc, "stty raw\nexit 0\n");
	keep = dup(STDIN_FILENO);
	CHECK(keep >= 0);
	CHECK(dup2(slave, STDIN_FILENO) == STDIN_FILENO);

	/* the script on its own: a raw terminal is not a cooked one */
	CHECK(tcgetattr(STDIN_FILENO, &orig) == 0);
	CHECK((orig.c_lflag & (tcflag_t)ICANON) != 0);
	run_bare(sc.script);
	CHECK(tcgetattr(STDIN_FILENO, &raw) == 0);
	CHECK((raw.c_lflag & (tcflag_t)ICANON) == 0);
	CHECK(tcsetattr(STDIN_FILENO, TCSANOW, &orig) == 0);

	/* and through the launcher, which puts back what it saved */
	CHECK(tcgetattr(STDIN_FILENO, &before) == 0);
	CHECK((before.c_lflag & (tcflag_t)ICANON) != 0);
	launch_on(&lp, &sc, sc.script);
	CHECK(zr_launch(&lp, err, sizeof (err)) == 0);
	CHECK(tcgetattr(STDIN_FILENO, &after) == 0);
	CHECK(memcmp(&before, &after, sizeof (before)) == 0);

	CHECK(dup2(keep, STDIN_FILENO) == STDIN_FILENO);
	CHECK(close(keep) == 0);
	CHECK(close(slave) == 0);
	CHECK(close(master) == 0);
	scratch_close(&sc);
}

/*
 * ZI22: the built-in child, in either build, since what the two
 * builds share is what this asserts. With the picker in it (PICKER=
 * yes) the picker opens the two documents before it touches a
 * terminal, and this scratch has a resolution with no manifest beside
 * it, so the child refuses on the spot; with PICKER=no the stub entry
 * says this build has no picker. Either way it is one line on stderr
 * -- the line under this test's own output -- and an exit of 2, with
 * no curses anywhere near this program's terminal.
 * To the launcher that is a non-zero exit like any other. Nothing
 * about it is special: the fork, the wait and the status are the
 * same code the named command goes through.
 *
 * ZI23: and the argv that child is given, which is the contract the
 * picker's own issues are written against: the program's name, the
 * resolution, and then base, from, onto and the result, in that
 * order, with a NULL after them and nothing else.
 */
static void
check_builtin_child(void)
{
	struct zr_launch lp;
	struct scratch sc;
	char *argv[ZR_LAUNCH_ARGC + 1];
	char err[512];

	scratch_open(&sc);
	launch_on(&lp, &sc, NULL);
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	says(err, "2");

	CHECK(zr_launch_argv(&lp, argv, ZR_LAUNCH_ARGC + 1) ==
	    ZR_LAUNCH_ARGC);
	CHECK(strcmp(argv[0], "zfs_rebase-picker") == 0);
	CHECK(strcmp(argv[1], sc.res) == 0);
	CHECK(strcmp(argv[2], "/b") == 0);
	CHECK(strcmp(argv[3], "/f") == 0);
	CHECK(strcmp(argv[4], "/o") == 0);
	CHECK(strcmp(argv[5], "/r") == 0);
	CHECK(argv[ZR_LAUNCH_ARGC] == NULL);

	/* an array that cannot hold them is refused, not overrun */
	CHECK(zr_launch_argv(&lp, argv, ZR_LAUNCH_ARGC) == -1);
	/* and a tree with no path reaches the child as "", never NULL */
	lp.base = NULL;
	CHECK(zr_launch_argv(&lp, argv, ZR_LAUNCH_ARGC + 1) ==
	    ZR_LAUNCH_ARGC);
	CHECK(strcmp(argv[2], "") == 0);
	scratch_close(&sc);
}

int
main(void)
{
	check_flags_guard();
	check_child_arguments();
	check_child_status();
	check_child_signal();
	check_child_missing();
	check_parent_interrupt();
	check_parent_terminate();
	check_terminal_restored();
	check_builtin_child();
	printf("check_run: %d checks passed\n", checks);
	return (0);
}
