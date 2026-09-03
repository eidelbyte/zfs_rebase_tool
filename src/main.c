/*
 * zfs_rebase: standalone rebase of one ZFS filesystem onto another.
 * This is the scaffold; the driver lands with the run-driver issue.
 */

#include <stdio.h>
#include <stdlib.h>

static const char usage[] =
	"usage: zfs_rebase [-n] [-p] [-o FILE] BASE@SNAP FROM ONTO\n"
	"       zfs_rebase --posix [-p] [-o FILE] BASEDIR FROMDIR ONTODIR\n"
	"  -n   dry run: write the manifest, create nothing\n"
	"  -p   permissive-merge mode\n"
	"  -o   write the manifest to FILE instead of stdout\n";

int
main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	fputs(usage, stderr);
	fputs("zfs_rebase: not implemented yet\n", stderr);
	return (2);
}
