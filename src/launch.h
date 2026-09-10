/* The child that edits the resolution at the conflicts gate. */

#ifndef	ZR_LAUNCH_H
#define	ZR_LAUNCH_H

#include <stddef.h>

/*
 * What --interactive asks for, as one call. The tool's whole
 * contract with a picker is a child process, the resolution file it
 * edits and its exit status: nothing passes in memory, and the
 * document is read back off the disk afterwards through the parser
 * --continue reads it with (sprints/sprint-6/implementation-plan.md,
 * ground rule 2).
 *
 * command is -i's value, the command line that edits the file, and
 * NULL asks for the built-in picker. A value keeps its own flags --
 * "code --wait" is one command and is never split -- because the
 * child is
 *
 *	/bin/sh -c 'CMD "$@"' CMD RESOLUTION
 *
 * which is how git runs an editor: the command's flags survive, and
 * the path arrives as a positional parameter that the shell never
 * interpolates into anything.
 *
 * resolution is the path that child is given. The other four are the
 * directories the tool has the trees at -- the two sides and the
 * base under their .zfs/snapshot paths, the result at its private
 * mount -- which only the built-in picker is handed, in the argv
 * zr_launch_argv builds; "" stands for a tree there is no path for,
 * which is what a verb has for base and what a side that is gone
 * has. An editor somebody named gets the resolution's path alone and
 * finds the manifest beside it by the sibling rule.
 */
struct zr_launch {
	const char	*command;	/* -i's value; NULL is the built-in */
	const char	*resolution;	/* the resolution's path */
	const char	*base;		/* the four trees' directories, for */
	const char	*from;		/* the built-in; "" where a tree has */
	const char	*onto;		/* no path */
	const char	*result;
};

/* How many arguments the built-in child is given, argv[0] counted. */
#define	ZR_LAUNCH_ARGC	6

/*
 * Fork the child, wait for it, and say what became of it. Returns 0
 * when it exited 0 -- the one status that means the tool may go on
 * -- and otherwise non-zero with one line in err: the status it
 * exited with, the signal it died of, the command that could not be
 * run at all, or the fork that failed. Every one of those leaves the
 * gate standing, and nothing here reopens the child by itself
 * (ruling 3 of the plan).
 *
 * stdout and stderr are flushed before the fork, so that no buffer
 * of the parent's is written twice. While it waits the parent
 * ignores SIGINT and SIGQUIT the way system(3) does, so that a
 * Ctrl-C reaches the editor and not the tool, and forwards a SIGTERM
 * to the child; the three dispositions it found are put back after
 * the wait. Where its own standard input is a terminal it saves the
 * termios before the fork and puts them back after the wait if the
 * child left them changed -- the launcher's half of ground rule 6,
 * which stands whether the child is our picker or an editor that
 * crashed.
 *
 * One launch is in flight at a time: the tool is one process at one
 * gate, and the pid the SIGTERM handler forwards to is a variable of
 * this file's.
 */
int zr_launch(const struct zr_launch *lp, char *err, size_t errlen);

/*
 * The built-in child's argv, built into the caller's array:
 *
 *	zfs_rebase-picker RESOLUTION BASE FROM ONTO RESULT
 *
 * and a NULL after them, so n must be ZR_LAUNCH_ARGC + 1 or more.
 * Returns the argument count, or -1 where the array is too small.
 * The entries point into lp's own strings, which the caller owns;
 * a NULL there is given to the child as "".
 */
int zr_launch_argv(const struct zr_launch *lp, char *argv[], int n);

#endif	/* ZR_LAUNCH_H */
