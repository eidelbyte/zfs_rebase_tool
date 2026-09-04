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
 *
 * The resolution, at the foot of this header, is the manifest's
 * companion: the same tree grammar and the same escaping, carrying a
 * choice per conflicted name instead of an action.
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
	ZR_ACT_DUP,	/* a new object copied from onto's own PATH */
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

/*
 * ---------------------------------------------------------------
 * The resolution (v4-manifest.md, section 8): the document the
 * conflicts stage hands back. The tool writes it beside the manifest
 * at the same moment, as a skeleton with one line per conflicted
 * name, and the person -- or a picker acting for them, or --take-onto
 * and --take-from at the start -- changes one field per line. Lines
 * are never added by hand and never removed: the file is the record
 * of what was chosen, and it is complete when nothing is unanswered.
 *
 * It is the manifest's own tree grammar, so it shares the escaping,
 * the scoped directories and the two dots that close them; what it
 * does not share is the actions. A choice can be verified against the
 * side it names, where an action could not be attributed to one, and
 * the tool derives the actions from the choices itself.
 * ---------------------------------------------------------------
 */

/* What was chosen for one name. "-" is the only unanswered one. */
enum zr_choice {
	ZR_CH_NONE,	/* "-": nobody has answered this yet */
	ZR_CH_KEEP,	/* the result stands as it is, hand merges too */
	ZR_CH_ONTO,	/* onto's object at this name, or absent */
	ZR_CH_FROM	/* from's object at this name, or absent */
};

/* What a resolution line is about. */
enum zr_rline_kind {
	ZR_RL_CONFLICT,	/* a name of conflict group N of the manifest */
	ZR_RL_DRIFT	/* a clean name a verify found changed */
};

/*
 * One line of a resolution. The path is the decoded bytes, absolute
 * and without a trailing slash, exactly as struct zr_action holds
 * one; isdir is what the trailing slash of the line said, and the
 * group is the manifest's conflict number on a conflict line and 0 on
 * a drift line.
 */
struct zr_rline {
	enum zr_rline_kind	zl_kind;
	enum zr_choice		zl_choice;
	unsigned char		*zl_path;	/* absolute, no trailing / */
	size_t			zl_pathlen;
	uint32_t		zl_group;
	int			zl_isdir;
};

/*
 * One resolution. The three snapshots are the manifest's own, and a
 * caller holds them against its record before it believes a word of
 * the document: a resolution for another rebase describes another
 * tree. The two declared counts are what the header said, which the
 * parse has checked against the lines; the writer derives them again
 * from the lines, so the two can never drift apart.
 *
 * The lines are in document order, which is the manifest's walk
 * order for a skeleton. Directories that only scope other lines are
 * not lines and are not kept: the writer derives them from the paths
 * again, the way zr_parsed_write does.
 */
struct zr_resolution {
	char			*zs_base;
	char			*zs_from;
	char			*zs_onto;
	zr_mode_t		zs_mode;
	uint32_t		zs_names_declared;
	uint32_t		zs_unanswered_declared;
	struct zr_rline		*zs_lines;
	uint32_t		zs_nlines;
	uint32_t		zs_cap;		/* what zs_lines holds */
};

/*
 * Read one resolution. Returns 0 with *out filled, or -1 with err
 * holding "line L: reason" and *out left safe to hand to
 * zr_resolution_fini. An action word where a choice belongs, a header
 * that is not a resolution's, a count that does not match the lines
 * and a drift line left unanswered are all refusals.
 */
int zr_resolution_parse(FILE *in, struct zr_resolution *out, char *err,
    size_t errlen);

/*
 * Write one resolution in the bytes section 8 prints, so that parse
 * then write is the identity. Returns 0, or -1 on an allocation or a
 * write failure.
 */
int zr_resolution_write(FILE *out, const struct zr_resolution *r);

void zr_resolution_fini(struct zr_resolution *r);

/*
 * The skeleton of one parsed manifest: its three snapshots and its
 * mode, and one conflict line per conflict mark of the tree section,
 * in manifest order, each keeping its group number and its directory
 * flag and taking the choice def -- ZR_CH_NONE for the "-" a fresh
 * run writes, or the side a --take flag named. A manifest with no
 * conflicts gives an empty document. Returns 0, or -1 out of memory
 * with *out safe to hand to zr_resolution_fini.
 */
int zr_resolution_skeleton(const struct zr_parsed *m, enum zr_choice def,
    struct zr_resolution *out);

/*
 * Add one drift line: a clean name a verify found changed, with the
 * choice the picker is offered. The path is the decoded bytes,
 * absolute and without a trailing slash. Returns 0, or -1 out of
 * memory or on a path or a choice a drift line cannot carry.
 */
int zr_resolution_add_drift(struct zr_resolution *r, const unsigned char *path,
    size_t len, int isdir, enum zr_choice ch);

/* How many lines are still unanswered. The document is complete at 0. */
uint32_t zr_resolution_unanswered(const struct zr_resolution *r);

/* The word one choice is written as: "-", "keep", "onto" or "from". */
const char *zr_choice_str(enum zr_choice ch);

#endif	/* ZR_MANIFEST_H */
