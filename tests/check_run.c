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
 * The third is the conflicts gate's document half, which reads the
 * manifest and the resolution and no tree at all: the conflict lines
 * a hand edit took out of the document, put back. It is a pure
 * function of the two documents, so it is asked here over parses of
 * two strings, with no name table and no walk -- which is exactly
 * what the gate has when it is reached from applying1.
 *
 * Matrix cells (tests/MATRIX.md): ZX266 (the gate snapshot's name,
 * which is what --abort composes from the result and the tag when it
 * returns a rebase at applying2 to the conflicts gate), ZX242, ZX243
 * and ZX254 (where -o
 * points, asked before the pool is touched), ZX244 (the clone's name
 * out of --result, a bare name beside onto), ZX250 (the gate's
 * document half), and ZI13 to ZI23 and ZI40 of family ZI. ZX23, the
 * refusal a real run makes at a real securelevel, stays the box's:
 * raising the level wants a reboot. ZI24 onward are the gate after
 * the child, on real datasets, in box/run-resolution.sh, and ZX251 to
 * ZX253 are the pool halves of the same findings ZX250 and ZI40
 * close here.
 */

#include <sys/ioctl.h>
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
 * ZI19 and ZI41: SIGINT while the tool waits is decisive (ruled
 * 2026-09-15). The script ignores SIGINT, sends one to the tool and
 * then sleeps; the launcher kills the child with SIGTERM, reaps it
 * and reports the launch failed, so the sleep never runs out and a
 * child that would have exited 0 does not carry the run past the
 * gate. The clock is the assertion that the child was killed: without
 * it the wait would have run the sleep out. SIGQUIT is the same, and
 * the second half asks it the same way. The three dispositions the
 * launcher put on
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

	struct timespec t0, t1;

	scratch_open(&sc);
	/*
	 * The child ignores the signal the terminal delivers to its
	 * group, which is the editor this ruling is about: one that
	 * maps Ctrl-C to a key of its own and would go on editing. The
	 * sleep is in the background with a wait on it, since a shell
	 * runs a trap only once its foreground command is over, and
	 * what must cut the sleep short is the parent's kill.
	 */
	write_script(&sc, "trap '' INT\nkill -INT $ZR_LAUNCH_TOOL\n"
	    "sleep 5 &\nwait\nexit 0\n");
	launch_on(&lp, &sc, sc.script);
	disposition(SIGINT, &bint);
	disposition(SIGQUIT, &bquit);
	disposition(SIGTERM, &bterm);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
	CHECK(t1.tv_sec - t0.tv_sec < 5);
	says(err, "SIGINT");
	disposition(SIGINT, &aint);
	disposition(SIGQUIT, &aquit);
	disposition(SIGTERM, &aterm);
	CHECK(aint.sa_handler == bint.sa_handler);
	CHECK(aquit.sa_handler == bquit.sa_handler);
	CHECK(aterm.sa_handler == bterm.sa_handler);
	/* and no child of ours is left behind to reap */
	CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

	/* ZI41: SIGQUIT, the terminal's other way of saying stop */
	write_script(&sc, "trap '' QUIT\nkill -QUIT $ZR_LAUNCH_TOOL\n"
	    "sleep 5 &\nwait\nexit 0\n");
	CHECK(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
	CHECK(zr_launch(&lp, err, sizeof (err)) != 0);
	CHECK(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);
	CHECK(t1.tv_sec - t0.tv_sec < 5);
	says(err, "SIGQUIT");
	CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
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
	struct sigaction bttou, attou;
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
	/*
	 * ZI43: the belt ignores SIGTTOU across its tcsetattr, so that
	 * a tool in the background does not stop itself putting the
	 * terminal back, and puts the tool's own disposition back
	 * afterwards. This half asserts the second of those. It cannot
	 * fail against a launcher that never touches SIGTTOU, which is
	 * what it is for: it guards the guard, and the stop it prevents
	 * wants a background process group to provoke.
	 */
	disposition(SIGTTOU, &bttou);
	CHECK(zr_launch(&lp, err, sizeof (err)) == 0);
	disposition(SIGTTOU, &attou);
	CHECK(attou.sa_handler == bttou.sa_handler);
	CHECK(attou.sa_flags == bttou.sa_flags);
	CHECK(tcgetattr(STDIN_FILENO, &after) == 0);
	CHECK(memcmp(&before, &after, sizeof (before)) == 0);

	CHECK(dup2(keep, STDIN_FILENO) == STDIN_FILENO);
	CHECK(close(keep) == 0);
	CHECK(close(slave) == 0);
	CHECK(close(master) == 0);
	scratch_close(&sc);
}

/*
 * ZI42: the termios belt where standard input is no terminal. A run
 * whose stdin is redirected still has a controlling terminal, and the
 * child a person named opens it by name; the belt falls back to
 * /dev/tty so that such a run hands the terminal back as it found it.
 *
 * It wants a process whose controlling terminal the test owns, which
 * this process's is not, so the case is made in a fork of its own:
 * setsid for a session with no terminal, the pty's slave opened
 * without O_NOCTTY (and TIOCSCTTY where the system wants it asked)
 * for a controlling terminal, and /dev/null on standard input. The
 * child says what it found in its exit status, since a CHECK in
 * there would print and exit for the wrong process. The control is
 * the same one ZI21 makes: the script is run bare first, and a
 * terminal it did not change would make the comparison meaningless.
 *
 * Exit codes: 0 the belt put it back, 1 it did not, 2 the script
 * changed nothing so the case proves nothing, and 10 upward the
 * scaffolding could not be built, which is a skip and not a failure.
 */
#define	ZI42_NOSID	10
#define	ZI42_NOSLAVE	11
#define	ZI42_NOCTTY	12
#define	ZI42_NOATTR	13
#define	ZI42_NONULL	14
#define	ZI42_STDINTTY	15
#define	ZI42_LAUNCH	16

static int
devtty_child(const char *name, const struct zr_launch *lp, const char *script)
{
	struct termios before, bare, after;
	char err[512];
	int tty, fd, nul;

	if (setsid() < 0)
		_exit(ZI42_NOSID);
	fd = open(name, O_RDWR);
	if (fd < 0)
		_exit(ZI42_NOSLAVE);
#ifdef TIOCSCTTY
	(void) ioctl(fd, TIOCSCTTY, 0);
#endif
	tty = open("/dev/tty", O_RDWR);
	if (tty < 0)
		_exit(ZI42_NOCTTY);
	if (tcgetattr(tty, &before) != 0)
		_exit(ZI42_NOATTR);
	nul = open("/dev/null", O_RDWR);
	if (nul < 0)
		_exit(ZI42_NONULL);
	if (dup2(nul, STDIN_FILENO) != STDIN_FILENO)
		_exit(ZI42_NONULL);
	(void) close(nul);
	if (isatty(STDIN_FILENO))
		_exit(ZI42_STDINTTY);
	/* the control: the script on its own really changes it */
	run_bare(script);
	if (tcgetattr(tty, &bare) != 0)
		_exit(ZI42_NOATTR);
	if (memcmp(&before, &bare, sizeof (before)) == 0)
		_exit(2);
	if (tcsetattr(tty, TCSANOW, &before) != 0)
		_exit(ZI42_NOATTR);
	if (tcgetattr(tty, &before) != 0)
		_exit(ZI42_NOATTR);
	/* and through the launcher, which has no terminal on stdin */
	if (zr_launch(lp, err, sizeof (err)) != 0)
		_exit(ZI42_LAUNCH);
	if (tcgetattr(tty, &after) != 0)
		_exit(ZI42_NOATTR);
	_exit(memcmp(&before, &after, sizeof (before)) == 0 ? 0 : 1);
}

static void
check_terminal_devtty(void)
{
	struct zr_launch lp;
	struct scratch sc;
	const char *name;
	pid_t pid, got;
	int master, status = 0, code;

	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0) {
		printf("skip ZI42: no pty here (%s)\n", strerror(errno));
		return;
	}
	CHECK(grantpt(master) == 0);
	CHECK(unlockpt(master) == 0);
	name = ptsname(master);
	CHECK(name != NULL);
	scratch_open(&sc);
	write_script(&sc, "stty raw < /dev/tty\nexit 0\n");
	launch_on(&lp, &sc, sc.script);
	(void) fflush(NULL);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0)
		devtty_child(name, &lp, sc.script);
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid);
	CHECK(WIFEXITED(status));
	code = WEXITSTATUS(status);
	if (code >= ZI42_NOSID) {
		printf("skip ZI42: no controlling terminal to be had "
		    "(step %d)\n", code);
	} else {
		CHECK(code != 2);
		CHECK(code == 0);
	}
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

/* One path that must be refused for standing in the way already. */
static void
outdir_taken(const char *path)
{
	char err[512];

	err[0] = '\0';
	CHECK(zr_outdir_ok(path, err, sizeof (err)) != 0);
	says(err, path);
	says(err, "that path exists");
}

/*
 * ZX243: where -o points is asked before the pool is touched. The
 * directory must be there, be a directory and be writable, the
 * answer names the path and the directory, and a path with no slash
 * is the working directory, which is there. The unwritable case is
 * asked only of a process that is not root, since root writes
 * anywhere and access(2) says so.
 */
static void
check_outdir(void)
{
	struct scratch sc;
	char path[1024], err[512];
	FILE *f;

	scratch_open(&sc);
	(void) snprintf(path, sizeof (path), "%s/manifest", sc.root);
	CHECK(zr_outdir_ok(path, err, sizeof (err)) == 0);
	/*
	 * A path with no slash is the working directory, which is
	 * there. The name is one nothing puts in a tree of ours,
	 * because anything standing at it would now be a refusal and
	 * this case is about the directory and not the name.
	 */
	CHECK(zr_outdir_ok("zr-outdir-no-such-file", err,
	    sizeof (err)) == 0);

	(void) snprintf(path, sizeof (path), "%s/no-such/manifest", sc.root);
	err[0] = '\0';
	CHECK(zr_outdir_ok(path, err, sizeof (err)) != 0);
	says(err, "no-such");
	says(err, strerror(ENOENT));

	(void) snprintf(path, sizeof (path), "%s/afile", sc.root);
	f = fopen(path, "w");
	CHECK(f != NULL);
	if (f != NULL)
		(void) fclose(f);
	(void) snprintf(path, sizeof (path), "%s/afile/manifest", sc.root);
	err[0] = '\0';
	CHECK(zr_outdir_ok(path, err, sizeof (err)) != 0);
	says(err, "not a directory");

	/*
	 * ZX254: and what stands at the path itself, which is a
	 * refusal whatever it is (ruled 2026-09-15, "-o to existing
	 * should fail"). A directory is the commonest shape of the
	 * mistake -- -o given the place to write into rather than the
	 * file to write -- and a regular file is the other one, this
	 * rebase's own manifest from an earlier attempt or somebody
	 * else's. The containing directory is there and writable in
	 * every case here, so the guard is the only thing that can
	 * refuse them, and it refuses before the run directory and the
	 * pre-apply snapshot rather than at the rename with exit 3.
	 */
	(void) snprintf(path, sizeof (path), "%s/adir", sc.root);
	CHECK(mkdir(path, 0755) == 0);
	outdir_taken(path);
	(void) snprintf(path, sizeof (path), "%s/afile", sc.root);
	outdir_taken(path);
	/*
	 * And a symbolic link, live or dangling. The question is asked
	 * with lstat, so a link is a name that exists whether or not
	 * anything is at the other end: rename(2) would replace the
	 * link itself, and the thing the person pointed at would
	 * quietly stop being pointed at.
	 */
	(void) snprintf(path, sizeof (path), "%s/alink", sc.root);
	(void) unlink(path);
	if (symlink("adir", path) == 0) {
		outdir_taken(path);
		CHECK(unlink(path) == 0);
	}
	(void) snprintf(path, sizeof (path), "%s/adangler", sc.root);
	(void) unlink(path);
	if (symlink("nothing-is-here", path) == 0) {
		outdir_taken(path);
		CHECK(unlink(path) == 0);
	}
	/* and a path with nothing at it still passes */
	(void) snprintf(path, sizeof (path), "%s/adir/manifest", sc.root);
	CHECK(zr_outdir_ok(path, err, sizeof (err)) == 0);
	(void) snprintf(path, sizeof (path), "%s/adir", sc.root);
	CHECK(rmdir(path) == 0);

	if (geteuid() != 0) {
		(void) snprintf(path, sizeof (path), "%s/shut", sc.root);
		CHECK(mkdir(path, 0500) == 0);
		(void) snprintf(path, sizeof (path), "%s/shut/manifest",
		    sc.root);
		err[0] = '\0';
		CHECK(zr_outdir_ok(path, err, sizeof (err)) != 0);
		says(err, strerror(EACCES));
		(void) snprintf(path, sizeof (path), "%s/shut", sc.root);
		(void) chmod(path, 0700);
		CHECK(rmdir(path) == 0);
	}
	(void) snprintf(path, sizeof (path), "%s/afile", sc.root);
	CHECK(unlink(path) == 0);
	scratch_close(&sc);
}

/*
 * ZX244: the clone's name out of --result. A name with no slash goes
 * beside onto's dataset, under its parent, or under the pool where
 * onto is the pool's own dataset; a name with a slash is taken as
 * given; and a name that will not fit is refused rather than cut.
 */
static void
check_result_name(void)
{
	char out[64], big[300];

	CHECK(zr_result_name("zrm/onto", "rebased", out, sizeof (out)) == 0);
	CHECK(strcmp(out, "zrm/rebased") == 0);
	CHECK(zr_result_name("tank/home/main", "rebased", out,
	    sizeof (out)) == 0);
	CHECK(strcmp(out, "tank/home/rebased") == 0);
	CHECK(zr_result_name("tank", "rebased", out, sizeof (out)) == 0);
	CHECK(strcmp(out, "tank/rebased") == 0);
	CHECK(zr_result_name("tank/home/main", "tank/x/y", out,
	    sizeof (out)) == 0);
	CHECK(strcmp(out, "tank/x/y") == 0);
	memset(big, 'n', sizeof (big) - 1);
	big[sizeof (big) - 1] = '\0';
	CHECK(zr_result_name("tank/home/main", big, out, sizeof (out)) != 0);
}

/*
 * ---------------------------------------------------------------
 * The conflicts gate's document half: family ZX, cell ZX250.
 * ---------------------------------------------------------------
 */

/* The least header a manifest parse will take, and a resolution's. */
#define	ZG_MAN								\
	"#rebase-manifest 5\n#result -\n#form posix\n#base b 0\n"	\
	"#from f 0\n#onto o 0\n#made -\n#tag -\n#take -\n#written -\n"	\
	"#mode strict\n#actions 1\n#conflicts 3\n"

/* Three marked names and one plain action, and a record per group. */
#define	ZG_TREE								\
	"/\n    a conflict 1\n    b conflict 2\n    c conflict 3\n"	\
	"    d rm\n    ..\n"
#define	ZG_REC(n)							\
	"conflict " n " changed-both\n  why  x\n  base ()\n"		\
	"  from ()\n  onto ()\n"
#define	ZG_RECS	"\n" ZG_REC("1") ZG_REC("2") ZG_REC("3")

#define	ZG_RES(names, unans)						\
	"#rebase-resolution 5\n#base b 0\n#from f 0\n#onto o 0\n"	\
	"#mode strict\n#names " names "\n#unanswered " unans "\n"

/* One document out of a string, through a temporary file. */
static void
gate_manifest(struct zr_parsed *m)
{
	char err[256];
	FILE *f;

	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fputs(ZG_MAN ZG_TREE ZG_RECS, f) != EOF);
	rewind(f);
	err[0] = '\0';
	if (zr_manifest_parse(f, m, err, sizeof (err)) != 0)
		printf("  the manifest: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(fclose(f) == 0);
}

static void
gate_resolution(struct zr_resolution *r, const char *text)
{
	char err[256];
	FILE *f;

	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fputs(text, f) != EOF);
	rewind(f);
	err[0] = '\0';
	if (zr_resolution_parse(f, r, err, sizeof (err)) != 0)
		printf("  the resolution: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(fclose(f) == 0);
}

/* Which line of the document speaks for this name, or the count. */
static uint32_t
gate_line(const struct zr_resolution *r, const char *path)
{
	size_t len = strlen(path);
	uint32_t i;

	for (i = 0; i < r->zs_nlines; i++) {
		if (r->zs_lines[i].zl_pathlen == len &&
		    memcmp(r->zs_lines[i].zl_path, path, len) == 0)
			return (i);
	}
	return (r->zs_nlines);
}

/*
 * ZX250: the document half of the conflicts gate. A conflict line
 * the manifest marks that the resolution no longer has is put back
 * with the take mode's answer, and nothing else about the document
 * is touched: a line the hand left alone keeps the answer it was
 * given, the group and the directory flag come off the manifest's
 * own mark, and a document that speaks for every mark is left
 * exactly as it was. No name table is passed, which is what the
 * hand-off from applying1 has, so every covered-already question
 * goes to the scan.
 */
static void
check_gate_marks_back(void)
{
	struct zr_resolution r;
	struct zr_parsed m;
	char err[512];
	uint32_t back, i;

	gate_manifest(&m);
	CHECK(m.zp_conflicts_declared == 3);

	/* a hand took /b out and answered the rest */
	gate_resolution(&r, ZG_RES("2", "0")
	    "/\n    a conflict 1 onto\n    c conflict 3 from\n    ..\n");
	CHECK(r.zs_nlines == 2);
	err[0] = 'x';
	back = 99;
	CHECK(zr_conflicts_back(&m, &r, NULL, ZR_CH_NONE, &back, err,
	    sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	CHECK(back == 1);
	CHECK(r.zs_nlines == 3);
	i = gate_line(&r, "/b");
	CHECK(i < r.zs_nlines);
	CHECK(r.zs_lines[i].zl_kind == ZR_RL_CONFLICT);
	CHECK(r.zs_lines[i].zl_group == 2);
	CHECK(r.zs_lines[i].zl_choice == ZR_CH_NONE);
	CHECK(r.zs_lines[i].zl_isdir == 0);
	/* and the two the hand answered are as the hand left them */
	CHECK(r.zs_lines[gate_line(&r, "/a")].zl_choice == ZR_CH_ONTO);
	CHECK(r.zs_lines[gate_line(&r, "/c")].zl_choice == ZR_CH_FROM);
	/* the name is back among the unanswered, and the gate stops */
	CHECK(zr_resolution_unanswered(&r) == 1);
	/* asked again there is nothing left to put back */
	back = 99;
	CHECK(zr_conflicts_back(&m, &r, NULL, ZR_CH_NONE, &back, err,
	    sizeof (err)) == 0);
	CHECK(back == 0);
	CHECK(r.zs_nlines == 3);
	zr_resolution_fini(&r);

	/* the take mode's answer, where the record kept one */
	gate_resolution(&r, ZG_RES("2", "0")
	    "/\n    a conflict 1 onto\n    c conflict 3 onto\n    ..\n");
	CHECK(zr_conflicts_back(&m, &r, NULL, ZR_CH_ONTO, &back, err,
	    sizeof (err)) == 0);
	CHECK(back == 1);
	CHECK(r.zs_lines[gate_line(&r, "/b")].zl_choice == ZR_CH_ONTO);
	CHECK(zr_resolution_unanswered(&r) == 0);
	zr_resolution_fini(&r);

	gate_resolution(&r, ZG_RES("2", "0")
	    "/\n    a conflict 1 from\n    c conflict 3 from\n    ..\n");
	CHECK(zr_conflicts_back(&m, &r, NULL, ZR_CH_FROM, &back, err,
	    sizeof (err)) == 0);
	CHECK(r.zs_lines[gate_line(&r, "/b")].zl_choice == ZR_CH_FROM);
	zr_resolution_fini(&r);

	/* a document with nothing at all in it: every mark comes back */
	gate_resolution(&r, ZG_RES("0", "0") "/\n    ..\n");
	CHECK(r.zs_nlines == 0);
	CHECK(zr_conflicts_back(&m, &r, NULL, ZR_CH_NONE, &back, err,
	    sizeof (err)) == 0);
	CHECK(back == 3);
	CHECK(r.zs_nlines == 3);
	CHECK(zr_resolution_unanswered(&r) == 3);
	CHECK(gate_line(&r, "/a") < r.zs_nlines);
	CHECK(gate_line(&r, "/b") < r.zs_nlines);
	CHECK(gate_line(&r, "/c") < r.zs_nlines);
	/* and the name no mark spoke for is still nobody's line */
	CHECK(gate_line(&r, "/d") == r.zs_nlines);
	zr_resolution_fini(&r);

	/* a whole document, the skeleton as the run wrote it: no change */
	gate_resolution(&r, ZG_RES("3", "3")
	    "/\n    a conflict 1 -\n    b conflict 2 -\n"
	    "    c conflict 3 -\n    ..\n");
	CHECK(zr_conflicts_back(&m, &r, NULL, ZR_CH_NONE, &back, err,
	    sizeof (err)) == 0);
	CHECK(back == 0);
	CHECK(r.zs_nlines == 3);
	zr_resolution_fini(&r);

	/* nothing to put a line back into is a refusal and not a crash */
	err[0] = '\0';
	CHECK(zr_conflicts_back(NULL, NULL, NULL, ZR_CH_NONE, &back, err,
	    sizeof (err)) != 0);
	CHECK(err[0] != '\0');
	zr_parsed_fini(&m);
}

/*
 * ZI40: the forked built-in child closes every descriptor above the
 * three standard ones before the entry is called, which is what the
 * exec of a named command does for itself. The child cannot report
 * on its own descriptors -- it is the picker and says nothing about
 * them -- so the call it makes is asked here directly, in a fork of
 * this test's own, with a marker file the child writes before the
 * call and cannot write after it. The control comes first: a child
 * that makes no such call writes the marker either way, so the case
 * below is one that could fail.
 */
static void
check_child_closefds(void)
{
	struct scratch sc;
	struct stat st;
	char path[ZL_LINE];
	pid_t pid, got;
	int fd, status;

	scratch_open(&sc);
	(void) snprintf(path, sizeof (path), "%s/marker", sc.root);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	CHECK(fd > STDERR_FILENO);

	/* the control: no call, and the marker is written */
	(void) fflush(NULL);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0)
		_exit(write(fd, "control\n", 8) == 8 ? 0 : 1);
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(stat(path, &st) == 0 && st.st_size == 8);

	/* and the call: the same descriptor, and the write refused */
	CHECK(ftruncate(fd, 0) == 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	(void) fflush(NULL);
	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		if (write(fd, "before\n", 7) != 7)
			_exit(2);
		zr_launch_closefds();
		errno = 0;
		if (write(fd, "after\n", 6) != -1 || errno != EBADF)
			_exit(3);
		/* and the three the child is meant to keep */
		if (fcntl(STDIN_FILENO, F_GETFD) < 0 ||
		    fcntl(STDOUT_FILENO, F_GETFD) < 0 ||
		    fcntl(STDERR_FILENO, F_GETFD) < 0)
			_exit(4);
		_exit(0);
	}
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	CHECK(got == pid);
	CHECK(WIFEXITED(status));
	CHECK(WEXITSTATUS(status) == 0);
	/* the marker holds what the child wrote before the call, and no more */
	CHECK(stat(path, &st) == 0 && st.st_size == 7);
	CHECK(close(fd) == 0);
	CHECK(unlink(path) == 0);
	scratch_close(&sc);
}

/*
 * ZX266 and the name half of ZX265: the gate snapshot's name. It is a
 * function of the result and the tag and of nothing else, which is
 * what lets --abort compose it with no manifest to read and what
 * lets a crash between the snapshot and the record leave nothing
 * nameless. The prefix is the tool's own, so a person reading zfs
 * list sees at once whose it is; the suffix is what keeps it from
 * ever colliding with the name the tool takes for a side given as a
 * dataset, which is the bare prefix and tag.
 */
static void
check_gate_snap_name(void)
{
	char out[ZR_NAME_MAX], big[ZR_NAME_MAX];

	CHECK(zr_gate_snap_name("zrm/result", "zr-1a2b3c4d5e6f", out,
	    sizeof (out)) == 0);
	CHECK(strcmp(out, "zrm/result@zfs_rebase-zr-1a2b3c4d5e6f-gate") == 0);
	/* the tool's other name for the same tag, which this is not */
	CHECK(strcmp(out, "zrm/result@zfs_rebase-zr-1a2b3c4d5e6f") != 0);
	CHECK(zr_gate_snap_name("tank/home/main", "zr-000000000000", out,
	    sizeof (out)) == 0);
	CHECK(strcmp(out,
	    "tank/home/main@zfs_rebase-zr-000000000000-gate") == 0);

	/* a name that will not fit is refused, and never cut */
	memset(big, 'n', sizeof (big) - 1);
	big[sizeof (big) - 1] = '\0';
	out[0] = 'x';
	CHECK(zr_gate_snap_name(big, "zr-1a2b3c4d5e6f", out,
	    sizeof (out)) != 0);
	CHECK(out[0] == '\0');
	CHECK(zr_gate_snap_name("zrm/result", "zr-1a2b3c4d5e6f", out, 8) != 0);

	/* and arguments that name no rebase at all */
	CHECK(zr_gate_snap_name(NULL, "zr-1", out, sizeof (out)) != 0);
	CHECK(zr_gate_snap_name("zrm/result", NULL, out, sizeof (out)) != 0);
	CHECK(zr_gate_snap_name("", "zr-1", out, sizeof (out)) != 0);
	CHECK(zr_gate_snap_name("zrm/result", "", out, sizeof (out)) != 0);
	CHECK(zr_gate_snap_name("zrm/result", "zr-1", NULL,
	    sizeof (out)) != 0);
	CHECK(zr_gate_snap_name("zrm/result", "zr-1", out, 0) != 0);
	/*
	 * A result that is already a snapshot is not a result: the
	 * name would hold two '@' and answer to nothing.
	 */
	CHECK(zr_gate_snap_name("zrm/result@pre", "zr-1", out,
	    sizeof (out)) != 0);
}

/* One file with these bytes in it, made or replaced. */
static int
write_file(const char *path, const char *bytes)
{
	FILE *fp;

	fp = fopen(path, "w");
	if (fp == NULL)
		return (0);
	if (bytes[0] != '\0' && fputs(bytes, fp) == EOF) {
		(void) fclose(fp);
		return (0);
	}
	return (fclose(fp) == 0);
}

/* Is there no such sibling of this path? The atomic write's leavings. */
static int
unlink_says_enoent(const char *path, const char *suffix)
{
	char sib[1200];

	(void) snprintf(sib, sizeof (sib), "%s%s", path, suffix);
	return (unlink(sib) != 0 && errno == ENOENT);
}

/*
 * ---------------------------------------------------------------
 * The -i session file: family ZX, cells ZX270, ZX271, ZX273 to ZX275.
 * ---------------------------------------------------------------
 *
 * The file is what stops two --continue -i on one rebase, and what
 * has to be true after a SIGKILL: a file whose processes are gone is
 * stale and no lock at all. Everything here is reachable with no pool
 * -- a directory, a file and two pids -- which is the whole of the
 * mechanism; the two terminals themselves are the box's.
 */
static void
check_session_file(void)
{
	struct zr_session sn, back;
	struct scratch sc;
	char path[1024], err[512];
	pid_t who, dead;
	int status = 0;

	scratch_open(&sc);
	CHECK(zr_session_path(sc.root, path, sizeof (path)) == 0);
	CHECK(strncmp(path, sc.root, strlen(sc.root)) == 0);
	CHECK(strcmp(path + strlen(path) - 8, "/session") == 0);
	/* a path that will not fit is refused and never cut */
	CHECK(zr_session_path(sc.root, path, 4) != 0);
	CHECK(zr_session_path(NULL, path, sizeof (path)) != 0);
	CHECK(zr_session_path(sc.root, path, sizeof (path)) == 0);

	/* ZX274: no file is no session, and no error either */
	CHECK(zr_session_read(path, &back, err, sizeof (err)) == 0);

	/*
	 * ZX270: this process as the parent and itself as the child,
	 * written and read back field for field.
	 */
	zr_session_fill(&sn, getpid(), "continue", "nvim --wait");
	CHECK(sn.zn_parent == getpid());
	CHECK(sn.zn_child == getpid());
	CHECK(strcmp(sn.zn_verb, "continue") == 0);
	CHECK(strcmp(sn.zn_editor, "nvim --wait") == 0);
	CHECK(sn.zn_opened[0] != '\0');
	CHECK(zr_session_write(path, &sn, err, sizeof (err)) == 0);
	CHECK(zr_session_read(path, &back, err, sizeof (err)) == 1);
	CHECK(back.zn_parent == sn.zn_parent);
	CHECK(back.zn_child == sn.zn_child);
	CHECK(back.zn_pstart == sn.zn_pstart);
	CHECK(back.zn_cstart == sn.zn_cstart);
	CHECK(strcmp(back.zn_verb, sn.zn_verb) == 0);
	CHECK(strcmp(back.zn_editor, sn.zn_editor) == 0);
	CHECK(strcmp(back.zn_opened, sn.zn_opened) == 0);
	/* and the write left no sibling behind */
	CHECK(unlink_says_enoent(path, ".tmp"));

	/* ZX271: this process is alive, and is the one named */
	who = 0;
	CHECK(zr_session_live(&back, &who) == 1);
	CHECK(who == getpid());
	/* the child is asked first: it is the one with the tree */
	back.zn_parent = getpid();
	back.zn_child = getpid();
	CHECK(zr_session_live(&back, &who) == 1);
	CHECK(who == back.zn_child);

	/* the built-in picker's spelling of no editor */
	zr_session_fill(&sn, getpid(), "start", NULL);
	CHECK(strcmp(sn.zn_editor, "-") == 0);
	zr_session_fill(&sn, getpid(), "start", "");
	CHECK(strcmp(sn.zn_editor, "-") == 0);

	/*
	 * ZX273: a process that is gone. The test forks a child and
	 * reaps it, so the pid names something that certainly ran and
	 * certainly does not now, which is the state a SIGKILL of the
	 * tool leaves behind.
	 */
	dead = fork();
	CHECK(dead >= 0);
	if (dead == 0)
		_exit(0);
	while (waitpid(dead, &status, 0) < 0)
		CHECK(errno == EINTR);
	memset(&sn, 0, sizeof (sn));
	sn.zn_parent = dead;
	sn.zn_child = dead;
	who = 1;
	CHECK(zr_session_live(&sn, &who) == 0);
	CHECK(who == 0);
	/* and a session naming nobody at all */
	memset(&sn, 0, sizeof (sn));
	CHECK(zr_session_live(&sn, NULL) == 0);
	CHECK(zr_session_live(NULL, &who) == 0);

	/*
	 * ZX275: a pid that is alive with a start time that is not the
	 * one recorded is another process wearing the number, and is
	 * not the session. A start time of 0 is this system declining
	 * to say, and then the pid alone decides.
	 */
	zr_session_fill(&sn, getpid(), "continue", "-");
	if (sn.zn_cstart != 0) {
		sn.zn_cstart++;
		sn.zn_pstart++;
		CHECK(zr_session_live(&sn, &who) == 0);
		sn.zn_cstart--;
		sn.zn_pstart--;
		CHECK(zr_session_live(&sn, &who) == 1);
	} else {
		printf("skip ZX275: this system does not report a "
		    "process start time\n");
	}
	sn.zn_cstart = 0;
	sn.zn_pstart = 0;
	CHECK(zr_session_live(&sn, &who) == 1);

	/* a file that is not ours is said so rather than half read */
	CHECK(write_file(path, "#rebase-session 4\n#parent 1 0\n"));
	err[0] = '\0';
	CHECK(zr_session_read(path, &back, err, sizeof (err)) == -1);
	says(err, "no session of ours");
	CHECK(write_file(path, ""));
	err[0] = '\0';
	CHECK(zr_session_read(path, &back, err, sizeof (err)) == -1);
	says(err, "no session of ours");
	CHECK(unlink(path) == 0);
	scratch_close(&sc);
}

int
main(void)
{
	check_flags_guard();
	check_outdir();
	check_result_name();
	check_gate_snap_name();
	check_session_file();
	check_child_arguments();
	check_child_status();
	check_child_signal();
	check_child_missing();
	check_parent_interrupt();
	check_parent_terminate();
	check_terminal_restored();
	check_terminal_devtty();
	check_builtin_child();
	check_child_closefds();
	check_gate_marks_back();
	printf("check_run: %d checks passed\n", checks);
	return (0);
}
