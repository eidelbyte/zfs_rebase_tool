/*
 * zr_manifest_emit: the one output of a rebase run. Turns a decision
 * over three trees into the v4 manifest document, the scoped walk of
 * the result namespace and the conflict records after it.
 */

#ifndef	ZR_MANIFEST_H
#define	ZR_MANIFEST_H

#include <stdio.h>

#include "decide.h"

/*
 * The header lines. The three dataset names are the snapshots the
 * engine actually read and go out verbatim, unescaped.
 */
struct zr_manifest_hdr {
	const char	*base;
	const char	*from;
	const char	*onto;
	zr_mode_t	mode;
};

/*
 * Write the manifest of one decision over the three trees it was made
 * from. Returns 0, or -1 on an allocation or a write failure.
 */
int zr_manifest_emit(FILE *out, const struct zr_manifest_hdr *hdr,
    const struct zr_tree *base, const struct zr_tree *from,
    const struct zr_tree *onto, const struct zr_decision *d);

#endif	/* ZR_MANIFEST_H */
