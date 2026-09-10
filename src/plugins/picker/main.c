/*
 * The picker as a program of its own.
 *
 * The tool never runs this binary: --interactive forks and calls
 * zr_picker_main in the child (plan section 3.1, ruling 4), so the
 * two are the same objects and not two builds of one screen. This
 * exists so that the picker can be run by hand on a resolution and
 * the manifest beside it -- a --posix fixture's pair, say, with no
 * pool and no tool anywhere near it -- and so that the pty tests
 * have a child to run (cells ZP110, ZP113 and ZP114).
 *
 * The argv is the launcher's, word for word:
 *
 *	zfs_rebase-picker RESOLUTION BASE FROM ONTO RESULT
 *
 * with "" for a tree there is no path for. The status is the
 * picker's own three: 0 written and complete, 1 saved and stopping,
 * 2 abandoned, refused, or given no terminal.
 */

#include <stdio.h>

#include "picker.h"

int
main(int argc, char **argv)
{
	if (argc != ZR_PK_ARGC) {
		(void) fprintf(stderr, "usage: %s RESOLUTION BASE FROM ONTO "
		    "RESULT\n", argc > 0 && argv[0] != NULL ? argv[0] :
		    "zfs_rebase-picker");
		return (2);
	}
	return (zr_picker_main(argc, argv));
}
