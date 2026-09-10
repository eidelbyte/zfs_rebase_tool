/* The built-in picker: the child -i runs when it names no command. */

#ifndef	ZR_PICKER_H
#define	ZR_PICKER_H

#include <stddef.h>
#include <stdint.h>

#include "manifest.h"
#include "merge.h"

/*
 * The entry the launcher calls in the forked child
 * (sprints/sprint-6/implementation-plan.md, section 3.2), with the
 * argv zr_launch_argv builds:
 *
 *	zfs_rebase-picker RESOLUTION BASE FROM ONTO RESULT
 *
 * where the last four are the directories the trees are at and "" is
 * a tree there is no path for. The status it returns is the child's:
 * 0 the document written with no name left unanswered, so the tool
 * goes on; 1 saved and stop, the answers so far written and the gate
 * left standing; 2 abandoned with nothing written, which is also
 * what a picker that cannot open the terminal exits with. To the
 * tool anything but 0 is one thing -- the gate stands -- and the
 * picker's own three meanings are the person's.
 *
 * The picker depends on the documents and the name codec and on
 * nothing else of the tool: never on the driver, never on the ZFS
 * layer (ground rule 1). It could be lifted out whole.
 */
int zr_picker_main(int argc, char **argv);

/*
 * altscreen.c: does the terminal database give the terminal curses
 * is on an alternate screen (smcup)? Asked after newterm.
 */
int zr_pk_term_has_alt(void);

/*
 * ---------------------------------------------------------------
 * The model (plan section 3.5): the picker with no terminal in it.
 * It takes the child's argv, reads the two documents, joins them
 * into one row per resolution line, takes key codes, and writes the
 * document back. It knows nothing of curses, and screen.c needs
 * nothing but this header to draw it.
 * ---------------------------------------------------------------
 */

/*
 * How many arguments the child is given, argv[0] counted. This is
 * ZR_LAUNCH_ARGC of src/launch.h said again, because the picker
 * includes none of the tool's own headers; the launcher's is the
 * contract and the two must agree.
 */
#define	ZR_PK_ARGC	6

/* Where the four tree paths start in that argv; argv[1] is the file. */
#define	ZR_PK_ARGV_RES	1
#define	ZR_PK_ARGV_TREE	2

/* The keys of screen 1, as codes: screen.c maps the terminal to these. */
enum zr_pk_key {
	ZR_PK_UP,
	ZR_PK_DOWN,
	ZR_PK_TOP,
	ZR_PK_BOTTOM,
	ZR_PK_FROM,	/* f: from's object at this name */
	ZR_PK_ONTO,	/* o: onto's */
	ZR_PK_KEEP,	/* k: the result stands as it is */
	ZR_PK_CLEAR,	/* -: back to unanswered, on a conflict line only */
	ZR_PK_GROUP,	/* g: the next name of this row's group */
	ZR_PK_ENTER,	/* the merge view, where there is one */
	ZR_PK_SAVE,	/* s: write and stay */
	ZR_PK_WRITE,	/* w: write and go, when nothing is unanswered */
	ZR_PK_QUIT,	/* q */
	/*
	 * Screen 2's own, which mean nothing on the list and which the
	 * list's keys mean nothing beside: while a merge is open every
	 * key goes to it and to nothing else. ZR_PK_WRITE is in both
	 * screens and means the merge write in this one.
	 */
	ZR_PK_PICK_FROM,	/* 1: this hunk takes from's lines */
	ZR_PK_PICK_ONTO,	/* 2: this hunk takes onto's */
	ZR_PK_BASE,		/* b: the hunk's base range in its place */
	ZR_PK_NEXT,		/* n: the next conflicting hunk */
	ZR_PK_PREV,		/* p: the one before */
	ZR_PK_TOGGLE,		/* c: the stable stretches folded away */
	ZR_PK_BACK		/* Esc: back to the list, the choice kept */
};

/* What the screen must do about a key the model has just taken. */
enum zr_pk_act {
	ZR_PK_NOTHING,
	ZR_PK_REDRAW,
	ZR_PK_OPEN,	/* screen 2, on the row the cursor is on */
	ZR_PK_EXIT	/* with zr_pk_status */
};

/*
 * What a resolution line is, which is not what the document's own
 * two kinds are: a conflict line whose name the manifest does not
 * mark is a person's instruction and has no record behind it
 * (v4-manifest.md section 8), so it draws and answers like the tool's
 * own but has no group and no detail.
 */
enum zr_pk_line {
	ZR_PK_L_CONFLICT,	/* a conflict line the manifest marks */
	ZR_PK_L_DRIFT,		/* a drift line a gate wrote */
	ZR_PK_L_HAND		/* a conflict line a hand added */
};

/*
 * What the object at a name is on one tree, which is the TY column.
 * Text and binary are git's rule (plan section 3.4): a NUL in the
 * first ZR_PK_SNIFF bytes of a regular file makes it binary. A link
 * is never followed. ZR_PK_O_MIXED is not a tree's answer but a
 * row's: the trees hold objects of different types at that name.
 */
enum zr_pk_obj {
	ZR_PK_O_ABSENT,
	ZR_PK_O_TEXT,
	ZR_PK_O_BINARY,
	ZR_PK_O_DIR,
	ZR_PK_O_LINK,
	ZR_PK_O_SPECIAL,
	ZR_PK_O_MIXED
};

/* How far into a regular file the NUL is looked for. */
#define	ZR_PK_SNIFF	8000

/* The four trees, in the argv's own order. */
enum zr_pk_tree {
	ZR_PK_T_BASE,
	ZR_PK_T_FROM,
	ZR_PK_T_ONTO,
	ZR_PK_T_RESULT
};

#define	ZR_PK_NTREE	4

/*
 * The three trees a merge is made of, which are the first three of
 * the four: base, from and onto. The result is not one of them -- it
 * is where the answer lands -- and the enum's order is what lets a
 * side be indexed by ZR_PK_T_BASE, ZR_PK_T_FROM and ZR_PK_T_ONTO.
 */
#define	ZR_PK_NSIDE	3

/*
 * The F/O column: what each side did to the name, read off the three
 * trees and nothing else. A name the side has that base had not is an
 * add, one base had that the side has not is a delete, one both hold
 * is an edit -- the manifest has already said the name is contested,
 * so a side that still holds it is a side that touched it -- and a
 * name neither base nor the side ever had is none of the three.
 */
enum zr_pk_fo {
	ZR_PK_FO_NONE,
	ZR_PK_FO_ADD,
	ZR_PK_FO_DEL,
	ZR_PK_FO_EDIT
};

/*
 * One row: one line of the resolution, joined to the manifest's
 * record for its group and to the four trees.
 *
 * The choice is not here. It lives on zk_line, the document's own
 * line, and zr_pk_choice reads it there: one fact in one place, so
 * that what the screen draws and what the writer writes can never
 * differ (the lesson of cell ZP54, which the header's counts learned
 * first). zk_name and zk_namelen alias that line's path, which is
 * the tree-relative name in decoded bytes with its leading slash --
 * the bytes to draw, and the bytes to join onto a tree path.
 *
 * zk_group is the manifest's link and is 0 on any row that has no
 * record behind it -- a drift line, and a hand-added conflict line,
 * whose group number the theory says is not read. What such a line
 * spelled is still on zk_line->zl_group and goes back out untouched.
 *
 * zk_rec is the manifest's conflict record for this row's group: the
 * why line (zr_why), the class bits (zr_flags, ZR_CF_* of decide.h)
 * and the three trees' pools (zr_base, zr_from, zr_onto). It is NULL
 * for a drift line and for a hand-added one, whose group number is
 * not read at all.
 */
struct zr_pk_row {
	enum zr_pk_line		zk_kind;
	struct zr_rline		*zk_line;	/* the document's own line */
	const unsigned char	*zk_name;	/* zk_line's path */
	size_t			zk_namelen;
	uint32_t		zk_group;	/* 0 where there is none */
	int			zk_isdir;
	const struct zr_record	*zk_rec;	/* the manifest's, or NULL */
	enum zr_pk_obj		zk_ty;		/* the row's own TY */
	enum zr_pk_obj		zk_obj[ZR_PK_NTREE];
	uint64_t		zk_size[ZR_PK_NTREE];
	enum zr_pk_fo		zk_fo[2];	/* from, then onto */
};

/*
 * The header's counts. Every one is recomputed from the rows when a
 * choice changes; none of them is ever written into the document,
 * whose #names and #unanswered are the library writer's own count of
 * the lines (v4-manifest.md section 8).
 *
 * zc_conflicts counts every conflict line, the hand-added ones with
 * the tool's own. zc_groups counts the groups the marked ones name,
 * since a hand-added line pools with nobody.
 */
struct zr_pk_counts {
	uint32_t	zc_names;
	uint32_t	zc_conflicts;
	uint32_t	zc_groups;
	uint32_t	zc_drift;
	uint32_t	zc_unanswered;
};

/*
 * The messages the model has to give: what a key would not do and
 * why, and what the documents said at open. Nothing here is ever
 * printed by the model -- ground rule 6 forbids a write to stdout or
 * stderr while curses is up -- so they are queued and the screen
 * prints them after endwin, oldest first. The queue is fixed and
 * drops the oldest when it is full: a person who pressed a refused
 * key twenty times wants the last few lines, and no allocation can
 * fail in a key handler. A line taken off the queue points into it
 * until another message is queued over that slot.
 */
#define	ZR_PK_NMSG	8
#define	ZR_PK_MSGLEN	200

/*
 * ---------------------------------------------------------------
 * Screen 2 (plan section 3.4): a text conflict opened three ways.
 * ---------------------------------------------------------------
 *
 * What a row carries while its merge is open: the three objects read
 * whole off the trees, the merge over them, and where the keys are
 * pointing. It is opened by zr_pk_merge_open on the cursor's row and
 * closed by Esc, by a write that went through, or by zr_pk_fini.
 *
 * The three buffers are OWNED here and BORROWED by the struct zr_m3
 * (merge.h): the merge copies nothing and holds pointers into them,
 * so they are freed only after zr_m3_fini, which zr_pk_merge_close
 * does in that order. pm_bytes[ZR_PK_T_BASE] is NULL where the row
 * has no base object at all, which is the add/add form and which
 * zr_m3_open reads as such; an empty base OBJECT is a pointer with a
 * length of 0, which is a different thing.
 *
 * pm_cursor is a chunk index, always a ZR_M3_CONFLICT chunk, and is
 * pm_m3.nchunks -- past the end -- when the merge has no conflicting
 * hunk at all. The picks live on the chunks and nowhere else, so
 * folding the stable stretches away cannot lose one.
 */
struct zr_pk_merge {
	int		pm_open;	/* a merge is open on pm_row */
	uint32_t	pm_row;		/* the row it was opened on */
	struct zr_m3	pm_m3;		/* the chunks, and the picks on them */
	unsigned char	*pm_bytes[ZR_PK_NSIDE];
	size_t		pm_len[ZR_PK_NSIDE];
	uint32_t	pm_cursor;	/* the hunk the keys act on */
	int		pm_only;	/* c: the stable stretches folded */
	int		pm_base;	/* b: base in place of the answer */
};

/*
 * The picker. Everything it has is here: the two documents as the
 * parsers gave them, the rows over the resolution's lines, the
 * cursor, the counts, the queue, and the five paths it was given.
 *
 * pk_saved says a write happened, pk_done that the last one was the
 * one that finishes the rebase; the two are what zr_pk_status reads.
 * pk_dirty says a choice has changed since the last write.
 */
struct zr_picker {
	struct zr_resolution	pk_res;
	struct zr_parsed	pk_man;
	struct zr_pk_row	*pk_rows;
	uint32_t		pk_nrows;
	uint32_t		pk_cursor;
	struct zr_pk_counts	pk_counts;
	int			pk_dirty;
	int			pk_saved;
	int			pk_done;
	char			*pk_respath;
	char			*pk_manpath;
	char			*pk_tree[ZR_PK_NTREE];
	char			pk_msg[ZR_PK_NMSG][ZR_PK_MSGLEN];
	uint32_t		pk_nmsg;
	uint32_t		pk_msghead;
	struct zr_pk_merge	pk_merge;
};

/*
 * Open the two documents the child's argv names and build the rows.
 * Returns 0, or -1 with one line in err and *out safe to hand to
 * zr_pk_fini.
 *
 * It refuses what --continue refuses and what the join refuses: an
 * argv that is not ZR_PK_ARGC words, a resolution the parser will not
 * have (two lines for one name, a name that is ".", ".." or holds a
 * "/", a header that is not a resolution's, a count that misses), no
 * manifest beside the resolution, and a manifest naming other
 * snapshots or other guids than the resolution does. Nothing here
 * touches a terminal, so a refusal is a line and an exit and never a
 * half-drawn screen.
 *
 * An empty resolution opens with no rows. A document nothing is
 * unanswered in opens like any other: -i means open the picker,
 * whether or not everything is decided (ruling 2 of the plan). A
 * conflict line the manifest marks that the document lacks is NOT
 * added back here -- that is the gate's work at the next read
 * (documents-design.md section 11.5) -- but is queued as a message.
 */
int zr_pk_open(struct zr_picker *out, int argc, char **argv, char *err,
    size_t errlen);

/*
 * One key. Nothing else changes a choice or the cursor.
 *
 * The rules, which are the model's whole half of screen 1: f, o and k
 * answer any line; "-" clears a conflict line and refuses a drift
 * line, since only a conflict line starts unanswered; g moves to the
 * next name of this row's group and wraps inside it; Enter asks for
 * the merge view, which exists for a conflict line whose base, from
 * and onto objects are all text, and for one whose base is absent
 * with both sides text -- the add/add form, which gets the two-way
 * compare (plan section 3.4); s writes and stays; w writes and
 * leaves with 0 when nothing is unanswered and otherwise says which
 * name is the first that is; q leaves with 2, or with 1 when
 * something was saved. A key that refuses says why in the queue.
 *
 * While a merge is open (zr_pk_merge_open) every key goes to screen
 * 2 instead and the list's own keys do nothing: 1 and 2 answer the
 * hunk the cursor is on, b shows its base range, n and p move
 * between the conflicting hunks, c folds the stable stretches away,
 * Esc closes the merge with the row's choice as it was, and w writes
 * the merged bytes into the result's object -- refused, with the
 * library's line, while any hunk is unpicked (ruling 7).
 */
enum zr_pk_act zr_pk_key(struct zr_picker *pk, enum zr_pk_key key);

/*
 * Write the document, through zr_doc_write with zr_resolution_write
 * as the emitter: the bytes to a .tmp sibling, then a rename, so a
 * reader finds one whole document or the other and never half
 * (documents-design.md section 11.2). The header's two counts are the
 * emitter's own count of the lines.
 *
 * Returns 0, or -1 with one line in err and the document as it was.
 * s and w go through here; a caller of its own must ask whether
 * anything is unanswered first, which is what w does.
 */
int zr_pk_write(struct zr_picker *pk, char *err, size_t errlen);

void zr_pk_fini(struct zr_picker *pk);

/*
 * The child's status (plan section 3.2): 0 the document written with
 * nothing unanswered, 1 saved and stopping, 2 abandoned. A picker
 * that never opened is a 2.
 */
int zr_pk_status(const struct zr_picker *pk);

/*
 * ---------------------------------------------------------------
 * Screen 1 (plan section 3.3): the list, in curses, over a picker
 * that is already open. It is declared here and lives in screen.c,
 * the one file of the picker that knows what a terminal is.
 * ---------------------------------------------------------------
 */

/*
 * Draw the list and take keys until the model says to leave.
 * Returns 0 when the picker ran, and the status is then
 * zr_pk_status's; returns -1 with one line in err when the terminal
 * was refused before curses started -- standard input or output that
 * is not a terminal, no TERM in the environment, a TERM terminfo
 * does not know -- with nothing drawn and the terminal untouched,
 * which the caller reports and leaves with 2.
 *
 * The picker's whole terminal discipline is in that call (ground
 * rule 6): the termios read before curses starts and put back on
 * every way out of it, a fatal signal's included, and not one byte
 * written to stdout or stderr while curses is up. The messages the
 * model queued are still queued when it returns, for the caller to
 * print once the terminal is its own again.
 */
int zr_pk_screen(struct zr_picker *pk, char *err, size_t errlen);

/* The rows, the cursor and the counts, for the screen. */
uint32_t zr_pk_nrows(const struct zr_picker *pk);
const struct zr_pk_row *zr_pk_row(const struct zr_picker *pk, uint32_t i);
uint32_t zr_pk_cursor(const struct zr_picker *pk);
const struct zr_pk_counts *zr_pk_counts(const struct zr_picker *pk);
int zr_pk_dirty(const struct zr_picker *pk);

/* One row's choice, which lives on the document's line and nowhere else. */
enum zr_choice zr_pk_choice(const struct zr_pk_row *row);

/* How many rows this group holds, for the detail line under the list. */
uint32_t zr_pk_group_names(const struct zr_picker *pk, uint32_t group);

/*
 * Can the row at i be opened three ways -- two, in the add/add form
 * -- and if not, why not? The reason goes into buf as a sentence
 * fragment ("onto is binary") and is what comes back; NULL comes
 * back where the row opens. That is the answer ZR_PK_ENTER gives,
 * asked without pressing the key.
 */
int zr_pk_can_open(const struct zr_picker *pk, uint32_t i);
const char *zr_pk_why_not(const struct zr_picker *pk, uint32_t i, char *buf,
    size_t buflen);

/*
 * Open the merge on the cursor's row: the three objects read whole
 * off the trees the row names, and zr_m3_open over them. Returns 0
 * with the merge open and the keys of screen 2 live, or -1 with one
 * line queued and nothing open -- a row that has no merge view, an
 * object that will not read, and the library's own refusals, of
 * which delete/edit is the one a row can still reach.
 *
 * zr_pk_merge is what the screen draws from and is NULL while no
 * merge is open. zr_pk_merge_hunk is the cursor's place among the
 * conflicting hunks, 1-based, and 0 where there is no such hunk:
 * the title's "hunk N of M", whose M is pm_m3.nconflict.
 */
int zr_pk_merge_open(struct zr_picker *pk);
void zr_pk_merge_close(struct zr_picker *pk);
struct zr_pk_merge *zr_pk_merge(struct zr_picker *pk);
uint32_t zr_pk_merge_hunk(const struct zr_picker *pk);

/*
 * One line into the queue the screen drains after endwin, for the
 * screen's own use: it has no other way to say anything, since
 * nothing may be written to a stream while curses is up (ground
 * rule 6). The model queues its own lines and does not need this.
 */
void zr_pk_note(struct zr_picker *pk, const char *line);

/* The paths it was given: the four trees, "" for a tree with none. */
const char *zr_pk_path(const struct zr_picker *pk, enum zr_pk_tree tree);
const char *zr_pk_respath(const struct zr_picker *pk);
const char *zr_pk_manpath(const struct zr_picker *pk);

/*
 * The oldest queued message, taken off the queue, or NULL when there
 * are none: the screen drains these after endwin and prints them in
 * this order. zr_pk_last peeks at the newest without taking it, for a
 * screen that wants to show the last refusal in place while it is up.
 * Both point into the picker's own queue.
 */
const char *zr_pk_msg(struct zr_picker *pk);
const char *zr_pk_last(const struct zr_picker *pk);

#endif	/* ZR_PICKER_H */
