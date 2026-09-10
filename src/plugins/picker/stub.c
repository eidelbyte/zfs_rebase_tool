/*
 * The picker's absence: what zr_picker_main is in a build made with
 * PICKER=no, which is the Makefile's knob and the port's PICKER
 * option (plan section 8 item 14). Everything else about the tool is
 * the same tool -- the gate is the same gate, and -i CMD forks the
 * command it names through /bin/sh exactly as it always did -- so
 * this file is the whole of what a person loses: -i with no command
 * says there is no picker here and leaves with 2.
 *
 * Two is what the picker itself leaves with when it is abandoned or
 * refused (plan section 3.2), and to the launcher any non-zero exit
 * is one thing: it reports the child's status, leaves the resolution
 * as it stands and holds the conflicts gate, so the rebase carries on
 * with an editor named on the next -i.
 *
 * The line is signed zfs_rebase and not zfs_rebase-picker, which is
 * how the launcher's own child argv[0] would have spelled it: there
 * is no picker in this build to sign it, and the tool is the program
 * the person ran.
 *
 * It returns and never exits, exactly like the entry it stands in
 * for: the launcher calls it in a forked child and leaves through
 * _exit of what comes back, after fflush(NULL) of its own.
 */

#include <stdio.h>

#include "picker.h"

int
zr_picker_main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	(void) fprintf(stderr, "zfs_rebase: this build has no picker; "
	    "name an editor with -i CMD\n");
	return (2);
}
