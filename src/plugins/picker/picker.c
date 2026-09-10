/*
 * The built-in picker's entry: the four steps, in the one order
 * ground rule 6 allows.
 *
 *   1. Open the two documents the argv names. This happens before
 *      any terminal work at all, so that a refusal -- a resolution
 *      the parser will not have, no manifest beside it, a manifest
 *      naming another rebase -- is one line on stderr and an exit of
 *      2 with the terminal never touched.
 *   2. Draw screen 1 and take keys, which is screen.c and the whole
 *      of the terminal discipline: the termios saved before curses
 *      starts and put back on every way out, and not one byte
 *      written to stdout or stderr while it is up.
 *   3. Drain what the model queued, once the terminal is the
 *      person's again, oldest line first.
 *   4. Return the status the model kept: 0 the document written with
 *      nothing unanswered, 1 saved and stopping, 2 abandoned or
 *      refused (plan section 3.2).
 *
 * It returns and never exits. The launcher calls this in a forked
 * child and leaves through _exit of what comes back, after
 * fflush(NULL) of its own; the standalone binary's main returns it
 * too. Nothing here calls exit(3), which is why the atexit hook
 * screen.c installs is a belt against a library that does and not a
 * path of ours.
 */

#include <stdio.h>

#include "picker.h"

/* What the picker's own lines are signed with, the child's argv[0]. */
#define	PK_PROG		"zfs_rebase-picker"

/* One line, and never while curses is up. */
#define	PK_ERRLEN	512

int
zr_picker_main(int argc, char **argv)
{
	struct zr_picker pk;
	char err[PK_ERRLEN];
	const char *msg;
	int status, rc;

	err[0] = '\0';
	if (zr_pk_open(&pk, argc, argv, err, sizeof (err)) != 0) {
		(void) fprintf(stderr, "%s: %s\n", PK_PROG, err);
		zr_pk_fini(&pk);
		return (2);
	}
	rc = zr_pk_screen(&pk, err, sizeof (err));
	while ((msg = zr_pk_msg(&pk)) != NULL)
		(void) fprintf(stderr, "%s: %s\n", PK_PROG, msg);
	if (rc != 0) {
		/*
		 * The terminal was refused before curses started, so
		 * nothing was drawn and this is the first thing the
		 * person sees. The tool prints its own "the editor
		 * exited 2" line and the -c IDENT -i hint after it.
		 */
		(void) fprintf(stderr, "%s: %s\n", PK_PROG, err);
		status = 2;
	} else {
		status = zr_pk_status(&pk);
	}
	zr_pk_fini(&pk);
	return (status);
}
