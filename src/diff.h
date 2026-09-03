/*
 * diff: the text "zfs diff -F -H" prints, parsed into entries, and
 * the pruning that text buys. zfs diff names every object that
 * changed between two snapshots, so a pool of the side tree it never
 * names holds byte for byte what base holds, and is never read. The
 * parser is portable and is tested here over a captured file; the
 * runner that produces the text is zfsops.c and builds only on the
 * box.
 */

#ifndef	ZR_DIFF_H
#define	ZR_DIFF_H

#include <stdio.h>

#include <stddef.h>
#include <stdint.h>

#include "walk.h"
#include "yellow.h"

/*
 * One line of the diff. The paths are decoded bytes, absolute within
 * the dataset -- "/a/b", and "/" for the root -- with the dataset's
 * mountpoint, which zfs diff prints in front of every path, taken
 * off. They are NUL-terminated as a convenience, since no name holds
 * a NUL, but the length is what says how long they are.
 */
struct zr_diff_entry {
	char		zd_kind;	/* 'M', '-', '+', 'R' */
	char		zd_type;	/* the -F type column */
	unsigned char	*zd_path;
	size_t		zd_pathlen;
	unsigned char	*zd_newpath;	/* for R; else NULL */
	size_t		zd_newlen;
	int32_t		zd_linkdelta;	/* the (+N)/(-N) value, else 0 */
};

struct zr_diff {
	struct zr_diff_entry *zd_entries;
	uint32_t	zd_n;
};

/*
 * Parse the whole of in, which is "zfs diff -F -H" text: the classify
 * column is expected and the timestamp column is not. mountpoint is
 * the dataset's mountpoint, which every path is under; a path that is
 * not under it is an error naming the line, as is a malformed line or
 * a bad escape. Returns 0 with out filled, or -1 with a message in
 * err when errlen is not 0. Either way out must be handed to
 * zr_diff_fini.
 */
int zr_diff_parse(FILE *in, const char *mountpoint, struct zr_diff *out,
    char *err, size_t errlen);

void zr_diff_fini(struct zr_diff *d);

/*
 * The pruning. For every pool of side -- which is from when
 * side_index is 1 and onto when it is 2 -- whose names are all
 * present in base under one base pool of the same size, and none of
 * whose names the diff reports as changed, removed, added, renamed
 * or under a renamed directory, tell the oracle it holds what that
 * base pool holds. Returns the number of pools marked, or -1 on an
 * argument the trees cannot answer for or no memory. base and side
 * must share one name table.
 */
int zr_diff_apply_unchanged(const struct zr_diff *d,
    const struct zr_walk *base, struct zr_walk *side, int side_index,
    struct zr_oracle *o);

#endif	/* ZR_DIFF_H */
