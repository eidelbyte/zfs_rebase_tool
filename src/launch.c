/*
 * zfs_rebase: the child that edits the resolution.
 *
 * One fork, one wait, one status. The tool holds no terminal state
 * of its own and passes nothing to the child in memory: the child
 * gets the resolution's path, the environment and the working
 * directory it was started in, and the tool reads the file back
 * afterwards through the parser --continue reads it with. What the
 * child was -- an editor somebody named with -i CMD, or the built-in
 * picker -- makes no difference to anything below the fork.
 *
 * The signal handling is not system(3)'s. SIGINT, SIGQUIT and SIGTERM
 * all stop the launch while the child runs: the tool kills the child,
 * reaps it, and reports the launch failed whatever the child's status
 * was, which leaves the gate standing and the caller printing how to
 * come back to it.
 *
 * SIGINT is there by the ruling of 2026-09-15 ("Ctrl+C should
 * probably kill the editor in any phase (and interrupt the forked
 * child basically), and the parent rebase process can proceed to a
 * gate and then die gracefully"). It used to be ignored the way
 * system(3) ignores it, on the reasoning that a Ctrl-C belongs to the
 * editor; the hole in that is an editor which maps Ctrl-C to a key of
 * its own, where the person's Ctrl-C did nothing and the run carried
 * on to applying2 as though the session had been finished on purpose.
 * SIGQUIT is the terminal's other way of saying stop and is the same
 * case, so it is handled the same way.
 *
 * What the tool sends the child is SIGTERM in every case, and never
 * the signal it received: the terminal has already delivered SIGINT
 * or SIGQUIT to the whole foreground group, the child among it, so a
 * child still running after one is a child that is ignoring it, and
 * sending it a second would be no more decisive than the first.
 *
 * The handler does the one thing a handler may do here, kill(2),
 * which is async-signal-safe, and the three are blocked around the
 * fork so that the pid it reads is either set or the signal is still
 * pending.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "launch.h"
#include "plugins/picker/picker.h"

/* The shell that runs a named command, and the fixed suffix. */
#define	ZL_SHELL	"/bin/sh"
#define	ZL_SUFFIX	" \"$@\""

/* The one string the tool builds: the command and that suffix. */
#define	ZL_SCRIPT_MAX	4096

/*
 * The child, for the handler that forwards SIGTERM to it. One launch
 * is in flight at a time -- the tool is one process at one gate --
 * so one variable is enough, and 0 means there is no child to
 * forward to.
 */
static volatile sig_atomic_t zl_child;

/*
 * The signal that stopped this launch, or 0. Any of the three means
 * stop, and killing the child is only how the child hears about it:
 * whatever it then does -- dies of the SIGTERM, or catches it and
 * exits 0 -- the launch reports non-zero, so that the gate stands
 * (the plan, section 2.3).
 */
static volatile sig_atomic_t zl_stopsig;

static void
zl_stop(int sig)
{
	pid_t pid = (pid_t)zl_child;

	zl_stopsig = sig;
	if (pid > 0)
		(void) kill(pid, SIGTERM);
}

/* The one spelled name each of the three goes into a message as. */
static const char *
zl_stopname(int sig)
{
	switch (sig) {
	case SIGINT:
		return ("SIGINT");
	case SIGQUIT:
		return ("SIGQUIT");
	case SIGTERM:
		return ("SIGTERM");
	default:
		break;
	}
	return ("a signal");
}

/* One line saying what became of the child. */
static void
zl_say(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || errlen == 0)
		return;
	va_start(ap, fmt);
	(void) vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

/*
 * One argv entry, from a string this file does not own. The picker's
 * entry takes char ** as every main does, and everything here is
 * const char *: the union is that conversion, made in one place and
 * written down, and nothing ever writes through what comes back. A
 * cast would drop the qualifier, which -Wcast-qual refuses. A tree
 * with no path is given as "", never as NULL, which no argv may
 * hold.
 */
static char *
zl_word(const char *s)
{
	union {
		const char	*cp;
		char		*p;
	} u;

	u.cp = s != NULL ? s : "";
	return (u.p);
}

/* The name of the signal a child died of, and never NULL. */
static const char *
zl_signame(int sig)
{
	const char *s = strsignal(sig);

	return (s != NULL ? s : "unknown");
}

/*
 * How far the loop below counts where the system will not say. A
 * descriptor of ours is never up there -- the tool opens a handful,
 * three walk roots and /dev/zfs among them -- and a loop with no
 * bound at all is what an unlimited RLIMIT_NOFILE would ask for.
 */
#define	ZL_FD_CEILING	65536L

void
zr_launch_closefds(void)
{
#if defined(__FreeBSD__)
	closefrom(3);
#else
	long i, max = sysconf(_SC_OPEN_MAX);

	if (max < 3 || max > ZL_FD_CEILING)
		max = ZL_FD_CEILING;
	for (i = 3; i < max; i++)
		(void) close((int)i);
#endif
}

int
zr_launch_argv(const struct zr_launch *lp, char *argv[], int n)
{
	if (lp == NULL || argv == NULL || n < ZR_LAUNCH_ARGC + 1)
		return (-1);
	argv[0] = zl_word("zfs_rebase-picker");
	argv[1] = zl_word(lp->resolution);
	argv[2] = zl_word(lp->base);
	argv[3] = zl_word(lp->from);
	argv[4] = zl_word(lp->onto);
	argv[5] = zl_word(lp->result);
	argv[ZR_LAUNCH_ARGC] = NULL;
	return (ZR_LAUNCH_ARGC);
}

/*
 * The dispositions the parent put on, taken off again. It is written
 * as one function because every way out of the wait takes all three
 * back, and because the order matters no more than the order they
 * went on in.
 */
static void
zl_unhandle(const struct sigaction *oint, const struct sigaction *oquit,
    const struct sigaction *oterm)
{
	zl_child = 0;
	(void) sigaction(SIGINT, oint, NULL);
	(void) sigaction(SIGQUIT, oquit, NULL);
	(void) sigaction(SIGTERM, oterm, NULL);
}

/*
 * The terminal the belt saves and puts back. Standard input where
 * that is a terminal, which is what a curses program and a full
 * screen editor open (ground rule 6); otherwise the controlling
 * terminal by name, since a run with its input redirected still has
 * one and the child a person named opens it that way too (question
 * 8a of open-questions-2026-09-15.md). Returns the descriptor, and
 * -1 where there is no terminal to be had; *own says whether the
 * descriptor is this function's to close.
 */
#define	ZL_TTY	"/dev/tty"

static int
zl_termfd(int *own)
{
	int fd;

	*own = 0;
	if (isatty(STDIN_FILENO))
		return (STDIN_FILENO);
	fd = open(ZL_TTY, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return (-1);
	*own = 1;
	return (fd);
}

/*
 * tcsetattr with SIGTTOU out of the way and put back. A tcsetattr
 * from a process that is not the terminal's foreground group raises
 * SIGTTOU, whose default is to stop the process: a child that took
 * the foreground and then died would leave the tool stopping itself
 * on the one call whose whole purpose is to leave the terminal usable
 * (question 8b). SIG_IGN makes the call go through instead. The
 * disposition around it is the tool's and is restored, so that
 * nothing outside this belt sees the change.
 */
static void
zl_setattr(int fd, const struct termios *t)
{
	struct sigaction ign, old;
	int saved = 0;

	memset(&ign, 0, sizeof (ign));
	(void) sigemptyset(&ign.sa_mask);
	ign.sa_flags = 0;
	ign.sa_handler = SIG_IGN;
	if (sigaction(SIGTTOU, &ign, &old) == 0)
		saved = 1;
	(void) tcsetattr(fd, TCSADRAIN, t);
	if (saved)
		(void) sigaction(SIGTTOU, &old, NULL);
}

/*
 * The child, from the moment fork returned 0. It restores the
 * default dispositions of the three signals the parent touched and
 * the mask it blocked SIGTERM in before it does anything else: an
 * editor must meet the terminal's signals as any other program
 * would, and the picker installs its own. This never returns.
 */
static void
zl_run_child(const struct zr_launch *lp, const char *script, char *argv[],
    const sigset_t *mask)
{
	struct sigaction dfl;
	int rc;

	memset(&dfl, 0, sizeof (dfl));
	(void) sigemptyset(&dfl.sa_mask);
	dfl.sa_flags = 0;
	dfl.sa_handler = SIG_DFL;
	(void) sigaction(SIGINT, &dfl, NULL);
	(void) sigaction(SIGQUIT, &dfl, NULL);
	(void) sigaction(SIGTERM, &dfl, NULL);
	(void) sigprocmask(SIG_SETMASK, mask, NULL);
	if (lp->command != NULL) {
		/*
		 * The named command is exec'd, and the exec is what
		 * closes the descriptors: every one the tool opens
		 * carries O_CLOEXEC (src/walk.c, src/zfsops.c).
		 */
		(void) execl(ZL_SHELL, "sh", "-c", script, lp->command,
		    lp->resolution, (char *)NULL);
		/*
		 * /bin/sh itself is not there, which is the one
		 * failure the shell cannot report for us. 127 is the
		 * status it would have given for a command it could
		 * not run, and the parent says the same thing of it.
		 */
		_exit(127);
	}
	/*
	 * The built-in is called and not exec'd, so no close-on-exec
	 * ever fires for it and every descriptor of the tool's is the
	 * child's too -- /dev/zfs among them, and whatever the tool was
	 * started with. An orphaned picker then holds them (L6 of the
	 * code review of 2026-09-11). They are closed here instead,
	 * which is what the exec would have done, and the picker opens
	 * everything it needs by path from its argv. Both gates let
	 * their three walks go before they come here, so the roots at
	 * the private mount and inside .zfs/snapshot are not in the set
	 * any more (L1); this is the second lock on the same door.
	 */
	zr_launch_closefds();
	rc = zr_picker_main(ZR_LAUNCH_ARGC, argv);
	/*
	 * Out through _exit and not exit: the parent's atexit hooks
	 * and stdio buffers are this process's too, and running them
	 * a second time here is how one line comes out twice. What
	 * the child itself wrote is flushed first.
	 */
	(void) fflush(NULL);
	_exit(rc);
}

int
zr_launch(const struct zr_launch *lp, char *err, size_t errlen)
{
	struct sigaction stp, oint, oquit, oterm;
	struct termios saved, now;
	sigset_t block, oldmask;
	char script[ZL_SCRIPT_MAX];
	char *argv[ZR_LAUNCH_ARGC + 1];
	pid_t pid, got;
	int status = 0, code, hastio = 0, tfd, ownfd = 0;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (lp == NULL || lp->resolution == NULL) {
		zl_say(err, errlen, "there is no resolution to open");
		return (-1);
	}
	script[0] = '\0';
	argv[0] = NULL;
	if (lp->command != NULL) {
		if (strlen(lp->command) + sizeof (ZL_SUFFIX) >
		    sizeof (script)) {
			zl_say(err, errlen, "the command is longer than the "
			    "%u bytes a shell script of ours may be",
			    (unsigned)sizeof (script));
			return (-1);
		}
		(void) snprintf(script, sizeof (script), "%s%s", lp->command,
		    ZL_SUFFIX);
	} else if (zr_launch_argv(lp, argv, (int)(sizeof (argv) /
	    sizeof (argv[0]))) < 0) {
		zl_say(err, errlen, "the picker's arguments do not fit");
		return (-1);
	}
	/*
	 * Nothing of ours is left in a buffer across the fork: the
	 * child inherits them, and a child that leaves through _exit
	 * without them being empty writes them out a second time.
	 */
	(void) fflush(stdout);
	(void) fflush(stderr);
	/* The terminal, saved before anything can change it. */
	tfd = zl_termfd(&ownfd);
	if (tfd >= 0 && tcgetattr(tfd, &saved) == 0)
		hastio = 1;
	memset(&stp, 0, sizeof (stp));
	(void) sigemptyset(&stp.sa_mask);
	stp.sa_flags = 0;
	stp.sa_handler = zl_stop;
	(void) sigemptyset(&block);
	(void) sigaddset(&block, SIGINT);
	(void) sigaddset(&block, SIGQUIT);
	(void) sigaddset(&block, SIGTERM);
	(void) sigprocmask(SIG_BLOCK, &block, &oldmask);
	zl_child = 0;
	zl_stopsig = 0;
	(void) sigaction(SIGINT, &stp, &oint);
	(void) sigaction(SIGQUIT, &stp, &oquit);
	(void) sigaction(SIGTERM, &stp, &oterm);
	pid = fork();
	if (pid == 0)
		zl_run_child(lp, script, argv, &oldmask);
	if (pid < 0) {
		code = errno;
		zl_unhandle(&oint, &oquit, &oterm);
		(void) sigprocmask(SIG_SETMASK, &oldmask, NULL);
		if (ownfd)
			(void) close(tfd);
		zl_say(err, errlen, "cannot fork: %s", strerror(code));
		return (-1);
	}
	/*
	 * The pid first and the unblock after it: one of the three
	 * that arrived in between is pending, is delivered here, and
	 * finds the child it is to be aimed at.
	 */
	zl_child = (sig_atomic_t)pid;
	(void) sigprocmask(SIG_SETMASK, &oldmask, NULL);
	do {
		got = waitpid(pid, &status, 0);
	} while (got < 0 && errno == EINTR);
	code = errno;			/* before the sigactions clobber it */
	zl_unhandle(&oint, &oquit, &oterm);
	/*
	 * And the terminal, if the child left it changed. Reading it
	 * back and comparing is what keeps this from writing over a
	 * terminal nobody touched. The comparison errs towards putting
	 * them back: a kernel is entitled to hand back a bit of its
	 * own that the settings did not ask for -- a BSD sets PENDIN
	 * after any tcsetattr at all -- and a difference that is only
	 * that costs one tcsetattr of the very bytes already there,
	 * where the other mistake would leave a terminal raw.
	 */
	if (hastio && tcgetattr(tfd, &now) == 0 &&
	    memcmp(&now, &saved, sizeof (saved)) != 0)
		zl_setattr(tfd, &saved);
	if (ownfd)
		(void) close(tfd);
	if (got < 0) {
		zl_say(err, errlen, "the editor could not be waited for: %s",
		    strerror(code));
		return (-1);
	}
	/*
	 * A signal decides the answer before the status does: the tool
	 * was told to stop, and a child that caught the SIGTERM this
	 * sent it and left with 0 does not turn that into a go-on.
	 */
	if (zl_stopsig != 0) {
		zl_say(err, errlen, "stopped by %s while the editor ran",
		    zl_stopname((int)zl_stopsig));
		return (-1);
	}
	if (WIFEXITED(status)) {
		code = WEXITSTATUS(status);
		if (code == 0)
			return (0);
		/*
		 * 127 is what a shell exits with when it could not run
		 * the command at all, and the command is what the
		 * person wrote: naming it is the difference between a
		 * typo they can see and an editor that failed.
		 */
		if (code == 127 && lp->command != NULL)
			zl_say(err, errlen, "%s could not be run",
			    lp->command);
		else
			zl_say(err, errlen, "the editor exited %d", code);
		return (code);
	}
	if (WIFSIGNALED(status)) {
		code = WTERMSIG(status);
		zl_say(err, errlen, "the editor died of signal %d (%s)", code,
		    zl_signame(code));
		return (-1);
	}
	zl_say(err, errlen, "the editor neither exited nor died");
	return (-1);
}
