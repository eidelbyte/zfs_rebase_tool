/*
 * zr_manifest_emit: the one output of a rebase run. Turns a decision
 * over three trees into the v4 manifest document, the scoped walk of
 * the result namespace and the conflict records after it.
 *
 * zr_manifest_parse is its inverse. It reads one such document back
 * into the actions it states, every path absolute and decoded, and the
 * conflict records the tree section points at. zr_parsed_write puts a
 * parsed manifest back on the wire in the emitter's own bytes, so that
 * emit, parse and write round trip exactly.
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

/* The five things a name line can say. */
enum zr_act_kind {
	ZR_ACT_RM,
	ZR_ACT_LN,
	ZR_ACT_CP,
	ZR_ACT_WRITE,
	ZR_ACT_CONFLICT
};

/*
 * One name line that had an action. Paths are the decoded bytes, so a
 * name holding a space or a newline is here as those bytes and not as
 * its escaping; the length is what counts, though both paths are also
 * terminated for a caller that wants to print them.
 */
struct zr_action {
	enum zr_act_kind	za_kind;
	unsigned char		*za_path;	/* absolute, no trailing / */
	size_t			za_pathlen;
	unsigned char		*za_arg;	/* ln, cp, write; else NULL */
	size_t			za_arglen;
	uint32_t		za_conflict;	/* conflict N; else 0 */
	int			za_isdir;	/* the line had a slash */
};

/* One conflict record, its four lines kept as the text they hold. */
struct zr_record {
	uint32_t	zr_num;
	uint32_t	zr_flags;	/* ZR_CF_* from the class list */
	char		*zr_why;	/* after "why  " */
	char		*zr_base;	/* after "base " */
	char		*zr_from;
	char		*zr_onto;
};

/*
 * One parsed manifest. The actions are in manifest order and include
 * the conflict marks; zp_actions_declared is what the header claimed,
 * which the parse has checked against the rm, ln, cp and write lines.
 * Directories that only scope other lines are not actions and are not
 * kept: zr_parsed_write derives them from the action paths again.
 */
struct zr_parsed {
	char			*zp_base;
	char			*zp_from;
	char			*zp_onto;
	zr_mode_t		zp_mode;
	uint32_t		zp_actions_declared;
	uint32_t		zp_conflicts_declared;
	struct zr_action	*zp_actions;
	uint32_t		zp_nactions;
	struct zr_record	*zp_records;
	uint32_t		zp_nrecords;
};

/*
 * Read one manifest. Returns 0 with *out filled, or -1 with err holding
 * "line L: reason" and *out left safe to hand to zr_parsed_fini.
 */
int zr_manifest_parse(FILE *in, struct zr_parsed *out, char *err,
    size_t errlen);

void zr_parsed_fini(struct zr_parsed *p);

/*
 * Write a parsed manifest out again, byte for byte as the emitter
 * would have written it. Returns 0, or -1 on an allocation or a write
 * failure.
 */
int zr_parsed_write(FILE *out, const struct zr_parsed *p);

#endif	/* ZR_MANIFEST_H */
