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
 * The signal handling is system(3)'s, with one addition. SIGINT and
 * SIGQUIT are ignored while the child runs, so that a Ctrl-C at the
 * terminal reaches the editor, which is in the same process group,
 * and not the tool waiting behind it. A SIGTERM to the tool is
 * forwarded to the child instead of taking the tool down and
 * orphaning it: the handler does the one thing a handler may do
 * here, kill(2), which is async-signal-safe, and SIGTERM is blocked
 * around the fork so that the pid it reads is either set or the
 * signal is still pending.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
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
 * Whether a SIGTERM was forwarded during this launch. A SIGTERM to
 * the tool means stop, and forwarding it is only how the child
 * hears about it: whatever the child then does -- dies of it, or
 * catches it and exits 0 -- the launch reports non-zero, so that the
 * gate stands (the plan, section 2.3).
 */
static volatile sig_atomic_t zl_termed;

static void
zl_forward(int sig)
{
	pid_t pid = (pid_t)zl_child;

	zl_termed = 1;
	if (pid > 0)
		(void) kill(pid, sig);
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
	struct sigaction ign, fwd, oint, oquit, oterm;
	struct termios saved, now;
	sigset_t block, oldmask;
	char script[ZL_SCRIPT_MAX];
	char *argv[ZR_LAUNCH_ARGC + 1];
	pid_t pid, got;
	int status = 0, code, hastio = 0;

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
	/*
	 * The terminal, saved before anything can change it. isatty
	 * of standard input is the question, because that is what a
	 * curses program and a full-screen editor open (ground rule
	 * 6); where it is no terminal there is nothing to save and
	 * nothing to put back.
	 */
	if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0)
		hastio = 1;
	memset(&ign, 0, sizeof (ign));
	(void) sigemptyset(&ign.sa_mask);
	ign.sa_flags = 0;
	ign.sa_handler = SIG_IGN;
	memset(&fwd, 0, sizeof (fwd));
	(void) sigemptyset(&fwd.sa_mask);
	fwd.sa_flags = 0;
	fwd.sa_handler = zl_forward;
	(void) sigemptyset(&block);
	(void) sigaddset(&block, SIGTERM);
	(void) sigprocmask(SIG_BLOCK, &block, &oldmask);
	zl_child = 0;
	zl_termed = 0;
	(void) sigaction(SIGINT, &ign, &oint);
	(void) sigaction(SIGQUIT, &ign, &oquit);
	(void) sigaction(SIGTERM, &fwd, &oterm);
	pid = fork();
	if (pid == 0)
		zl_run_child(lp, script, argv, &oldmask);
	if (pid < 0) {
		code = errno;
		zl_unhandle(&oint, &oquit, &oterm);
		(void) sigprocmask(SIG_SETMASK, &oldmask, NULL);
		zl_say(err, errlen, "cannot fork: %s", strerror(code));
		return (-1);
	}
	/*
	 * The pid first and the unblock after it: a SIGTERM that
	 * arrived in between is pending, is delivered here, and finds
	 * the child it is to be forwarded to.
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
	if (hastio && tcgetattr(STDIN_FILENO, &now) == 0 &&
	    memcmp(&now, &saved, sizeof (saved)) != 0)
		(void) tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
	if (got < 0) {
		zl_say(err, errlen, "the editor could not be waited for: %s",
		    strerror(code));
		return (-1);
	}
	/*
	 * A forwarded SIGTERM decides the answer before the status
	 * does: the tool was told to stop, and a child that caught the
	 * signal and left with 0 does not turn that into a go-on.
	 */
	if (zl_termed != 0) {
		zl_say(err, errlen, "stopped by SIGTERM while the editor "
		    "ran");
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
