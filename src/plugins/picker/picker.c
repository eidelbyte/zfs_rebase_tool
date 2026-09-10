/*
 * The built-in picker, as this build has it: a stub that says there
 * is none.
 *
 * The launcher, the argv above and the gate that reads the document
 * back are finished; the two screens are not. Exit 2 is the status
 * section 3.2 gives to a picker that wrote nothing, and to the tool
 * it is a non-zero exit like any other: the gate stands and the
 * person carries on with an editor of their own, which -i CMD names.
 * The picker's issues replace this file and touch neither launch.c
 * nor run.c.
 */

#include <stdio.h>

#include "picker.h"

int
zr_picker_main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	(void) fprintf(stderr, "zfs_rebase: this build has no picker; name "
	    "an editor with -i CMD\n");
	return (2);
}
