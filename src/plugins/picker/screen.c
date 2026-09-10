/*
 * The built-in picker's screens: the list and the merge, in curses,
 * and the terminal discipline around them.
 *
 * The model (model.c) holds the two documents and answers key codes;
 * this file is the only one of the picker that knows what a terminal
 * is. It draws the rows the model built -- the mockup's layout, in
 * sprints/sprint-6/picker-mockup.html -- maps the keys of plan
 * sections 3.3 and 3.4 onto enum zr_pk_key, and acts on the four
 * answers: nothing, redraw, open (screen 2, the merge, which is the
 * second half of this file) and exit.
 *
 * Ground rule 6 is the heart of the file: "graphical startup and
 * tear down is critical to not destroying the terminal of the user
 * post-exit" (the author, 2026-09-10). The rule, and where each half
 * of it is here:
 *
 *   - Refuse before touching the terminal. pk_refuse checks that
 *     standard input and output are terminals, that TERM is set, and
 *     that terminfo knows the name -- the last through newterm
 *     itself, which returns NULL rather than exiting when the
 *     database has no such entry. Nothing is drawn on any of those
 *     paths and the termios are never written.
 *   - The termios are read once, before curses starts, and put back
 *     by pk_restore, which is idempotent and is called from every
 *     way out: the normal return, the error return, an atexit hook,
 *     and the handler for SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGSEGV
 *     and SIGBUS, which restores, puts the signal's own disposition
 *     back and raises it again, so that the picker dies of what
 *     killed it and the shell sees the truth.
 *   - SIGWINCH sets a flag the loop reads; nothing is drawn in the
 *     handler. A resize that takes the window below the floor ends
 *     the picker the way the floor does at startup.
 *   - A window smaller than PK_MIN_COLS by PK_MIN_ROWS is refused
 *     rather than drawn into (ruled 2026-09-10: "refuse and exit
 *     without continuing the merge (even if resolution file is
 *     complete, consider it a user-kill on gui)"). At startup the
 *     size comes from the terminal itself, before curses is opened,
 *     so nothing is drawn at all; a shrink while it is up ends
 *     curses, puts the termios back and leaves with the same line
 *     and the same status, dropping whatever was not saved, which is
 *     what a kill does.
 *   - Not one byte reaches stdout or stderr while curses is up. The
 *     model queues its messages and never prints them; the caller
 *     drains the queue after this call returns, which is after
 *     endwin. A refusal decided before curses starts is a line the
 *     caller prints, and it is printed before anything is drawn
 *     because nothing ever is.
 *
 * The handlers are installed before newterm on purpose: ncurses puts
 * its own cleanup handlers on SIGINT, SIGQUIT, SIGTERM and SIGWINCH
 * only where it finds the signal still at SIG_DFL (its
 * CatchIfDefault), so installing first is what keeps ours.
 *
 * Every byte this file draws is ASCII or a curses ACS macro, which
 * the terminal renders as line drawing and curses degrades to ASCII
 * where it cannot (ground rule 5). Names hold any byte but NUL and
 * are drawn through the manifest's own escaping, zr_vis_encode, so a
 * control byte in a name never reaches the terminal.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
#define	__BSD_VISIBLE	1	/* SIGWINCH, and TIOCGWINSZ with it */
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <sys/ioctl.h>
#include <sys/types.h>

#include <curses.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "picker.h"
#include "vis.h"

/* The key bar of the mockup, drawn when there is nothing to say. */
#define	PK_KEYS		"up/dn move  f/o/k choose  - clear  enter open " \
			"(text)  g group  s save  w write+continue  q quit"

/* The title in the top rule. */
#define	PK_TITLE	" zfs_rebase: conflicts "

/*
 * How tall each block of the layout is. The list is what is left
 * after the blocks that are drawn, and pk_geom drops them in this
 * order when the window is too short (ZP75): the blank line under
 * the column titles, the two detail lines with their two rules, the
 * header block, the column titles, and last the key bar. The list is
 * never dropped: a window one line tall is one row of the list.
 */
/*
 * The floor: the window the picker will draw in at all (cell ZP75).
 * The mockup is 100 columns; 80 by 24 is the terminal every terminal
 * is, and the layout at that size is the whole of it with a shorter
 * name column. Below it the picker refuses rather than drawing a
 * broken screen, at startup and on a resize alike.
 */
#define	PK_MIN_COLS	80
#define	PK_MIN_ROWS	24

/*
 * The blocks are still dropped in the order below when the window is
 * shorter than the layout, which the floor now makes unreachable
 * from a terminal the picker agreed to open. It stays because it is
 * what guarantees the other half of ZP75 -- that nothing is ever
 * written outside the window -- on a terminal whose size neither
 * TIOCGWINSZ nor terminfo told the truth about.
 */
#define	PK_H_HEAD	4	/* rule, identity, counts, rule */
#define	PK_H_TITLES	1
#define	PK_H_BLANK	1
#define	PK_H_DETAIL	4	/* rule, why, trees, rule */
#define	PK_H_BAR	1

/* Where the columns stand, and how the name column gives way first. */
#define	PK_X_TY		2
#define	PK_X_FO		6
#define	PK_X_GRP	11
#define	PK_X_NAME	18
#define	PK_W_NAME_MIN	8	/* below this the choice column moves in */

/* One vis-encoded name, and one line of text built for the screen. */
#define	PK_NAMEBUF	1024
#define	PK_LINEBUF	512

/*
 * Room for a terminal's name. newterm(3) takes a char * on the mac's
 * ncurses and a const char * on FreeBSD's ncursesw, so the name goes
 * into a buffer of ours and the environment's own string is never
 * handed over: a cast would drop the qualifier, which -Wcast-qual
 * refuses, and writing through the pointer is nobody's business
 * anyway. A terminfo name is a file name in the database and is far
 * inside this.
 */
#define	PK_TERMLEN	128

/*
 * The colors of the mockup, as the eight a terminal has. The cursor's
 * row is a band of the terminal's own colors swapped -- the key
 * bar's look -- and gives up the per-column colors for its one row.
 * The first cut gave it a blue background under each color, and on
 * the author's scheme that blue was a light teal that lost the text
 * (the box, 2026-09-10: "let's use grey"). Reverse video is the one
 * band every scheme can read, since it is made of the two colors the
 * scheme already puts against each other. A terminal with no colors
 * gets the same band and A_BOLD for what would have been colored.
 */
#define	PK_CO_PLAIN	0
#define	PK_CO_FROM	1	/* magenta, the mockup's from */
#define	PK_CO_ONTO	2	/* yellow, the mockup's onto */
#define	PK_CO_GREEN	3	/* add, keep, text */
#define	PK_CO_RED	4	/* delete, binary */
#define	PK_CO_CYAN	5	/* the kinds that are neither */
#define	PK_CO_DIM	6	/* unanswered, labels, the box */
#define	PK_CO_N		7

/*
 * The terminal, as it was found and as it is now. Everything a
 * signal handler reads is here and is sig_atomic_t; pk_tio is
 * written once, before the handlers can fire, and only read after.
 */
static struct termios		pk_tio;
static volatile sig_atomic_t	pk_tio_saved;
static volatile sig_atomic_t	pk_restored;
static volatile sig_atomic_t	pk_up;		/* curses is up */
static volatile sig_atomic_t	pk_winch;

/* The window as the last pk_geom read it, and whether colors are on. */
static int	pk_h;
static int	pk_w;
static int	pk_color;

/* What the geometry came to for one draw. */
struct pk_geom {
	int	g_head;		/* whether each block is drawn at all */
	int	g_titles;
	int	g_blank;
	int	g_detail;
	int	g_bar;
	int	g_listy;	/* the first line of the list */
	int	g_listh;	/* how many rows fit in it */
	int	g_namex;
	int	g_namew;
	int	g_choicex;
};

/*
 * ---------------------------------------------------------------
 * The terminal: saved, restored, and restored again.
 * ---------------------------------------------------------------
 */

/*
 * Put the terminal back exactly as it was found. Idempotent, so that
 * the normal return, the error return, the atexit hook and a signal
 * handler may all call it and only the first does anything.
 *
 * endwin(3) is the one curses call the library documents as safe to
 * make from a signal handler, and it is what puts the screen back to
 * the shell's; tcsetattr(3) and the rest here are async-signal-safe
 * by POSIX. Nothing else is called.
 */
static void
pk_restore(void)
{
	if (pk_restored != 0)
		return;
	pk_restored = 1;
	if (pk_up != 0) {
		pk_up = 0;
		(void) endwin();
	}
	if (pk_tio_saved != 0)
		(void) tcsetattr(STDIN_FILENO, TCSADRAIN, &pk_tio);
}

/* exit(), whoever calls it: the terminal is still the person's. */
static void
pk_atexit(void)
{
	pk_restore();
}

/*
 * A signal that ends the process. Put the terminal back, put the
 * signal's own disposition back, and raise it again: the picker then
 * dies of what killed it -- the wait status says SIGINT and not an
 * exit of 2 -- and a SIGSEGV still dumps its core.
 */
static void
pk_onsig(int sig)
{
	pk_restore();
	(void) signal(sig, SIG_DFL);
	(void) raise(sig);
}

/* A resize is a flag and a redraw, and nothing is drawn from here. */
static void
pk_onwinch(int sig)
{
	(void) sig;
	pk_winch = 1;
}

/*
 * The handlers and the atexit hook, installed before curses starts.
 * SIGQUIT is in the list with the five ground rule 6 names: the
 * screen leaves ISIG on, so Ctrl-backslash reaches the picker as
 * surely as Ctrl-C does, and a core dump with the terminal in raw
 * mode is exactly what the rule is about.
 */
static void
pk_arm(void)
{
	static const int fatal[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP,
		SIGSEGV, SIGBUS };
	struct sigaction sa;
	size_t i;

	memset(&sa, 0, sizeof (sa));
	(void) sigemptyset(&sa.sa_mask);
	sa.sa_handler = pk_onsig;
	for (i = 0; i < sizeof (fatal) / sizeof (fatal[0]); i++)
		(void) sigaction(fatal[i], &sa, NULL);
	sa.sa_handler = pk_onwinch;
	(void) sigaction(SIGWINCH, &sa, NULL);
	(void) atexit(pk_atexit);
}

/*
 * The window, from the terminal and not from curses, so that the
 * floor can be judged before curses is opened and nothing is drawn
 * on the way to refusing. Returns 0 with the size, or -1 where the
 * terminal will not say: an answer of 0 by 0 is no answer.
 */
static int
pk_winsize(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0)
		return (-1);
	if (ws.ws_row == 0 || ws.ws_col == 0)
		return (-1);
	*rows = (int)ws.ws_row;
	*cols = (int)ws.ws_col;
	return (0);
}

/* Is this window one the picker will not draw in? */
static int
pk_toosmall(int rows, int cols)
{
	return (rows < PK_MIN_ROWS || cols < PK_MIN_COLS);
}

/* The one line both refusals give, in the terminal's own numbers. */
static void
pk_saysmall(char *err, size_t errlen, int rows, int cols)
{
	(void) snprintf(err, errlen, "the terminal is %d by %d; the picker "
	    "needs %d by %d", cols, rows, PK_MIN_COLS, PK_MIN_ROWS);
}

/*
 * ---------------------------------------------------------------
 * Colors and styles.
 * ---------------------------------------------------------------
 */

/* Which pair a color is. */
static short
pk_pair(int co)
{
	return ((short)(1 + co));
}

static void
pk_colors(void)
{
	static const short fg[PK_CO_N] = { -1, COLOR_MAGENTA, COLOR_YELLOW,
		COLOR_GREEN, COLOR_RED, COLOR_CYAN, -1 };
	short bg = COLOR_BLACK;
	short f;
	int co;

	if (!has_colors() || start_color() != OK)
		return;
	/*
	 * -1 is the terminal's own foreground and background, which is
	 * what keeps the picker inside the person's color scheme. A
	 * curses without the extension gets white on black, which is
	 * the same picture on the terminals that have no default.
	 */
	if (use_default_colors() == OK)
		bg = -1;
	for (co = 0; co < PK_CO_N; co++) {
		f = fg[co];
		if (f < 0 && bg != -1)
			f = COLOR_WHITE;
		(void) init_pair(pk_pair(co), f, bg);
	}
	pk_color = 1;
}

/*
 * One style: the color, or bold where there is none; and on the
 * cursor's band the terminal's own two colors swapped, whatever the
 * column's color would have been.
 */
static chtype
pk_style(int co, int sel)
{
	chtype at = A_NORMAL;

	if (sel != 0) {
		at = A_REVERSE;
		if (pk_color != 0)
			at |= (chtype)COLOR_PAIR(pk_pair(PK_CO_PLAIN));
		return (at);
	}
	if (co == PK_CO_DIM)
		at |= A_DIM;
	if (pk_color != 0)
		return (at | (chtype)COLOR_PAIR(pk_pair(co)));
	if (co != PK_CO_PLAIN && co != PK_CO_DIM)
		at |= A_BOLD;
	return (at);
}

/*
 * ---------------------------------------------------------------
 * Drawing, all of it clipped to the window there is (ZP75).
 * ---------------------------------------------------------------
 */

/*
 * One string at (y, x) in one style, truncated at the last column
 * before the right border. Nothing is ever written outside the
 * window: a y or an x past its edge draws nothing at all. The new x
 * comes back, so that a line can be built left to right in pieces of
 * different colors, and it comes back whether the piece was drawn or
 * clipped, so the pieces after it clip too.
 */
static int
pk_putx(int y, int x, int co, int sel, const char *s)
{
	int room, len;

	len = (int)strlen(s);
	if (y < 0 || y >= pk_h || x < 0)
		return (x + len);
	room = pk_w - 1 - x;
	if (room <= 0)
		return (x + len);
	if (len > room)
		len = room;
	(void) attrset(pk_style(co, sel));
	(void) mvaddnstr(y, x, s, len);
	(void) attrset(A_NORMAL);
	return (x + (int)strlen(s));
}

static void
pk_put(int y, int x, int co, int sel, const char *s)
{
	(void) pk_putx(y, x, co, sel, s);
}

/* The two side borders of one line. */
static void
pk_side(int y)
{
	chtype at = pk_style(PK_CO_DIM, 0);

	if (y < 0 || y >= pk_h || pk_w < 2)
		return;
	(void) mvaddch(y, 0, ACS_VLINE | at);
	(void) mvaddch(y, pk_w - 1, ACS_VLINE | at);
}

/*
 * One horizontal rule with the corners the caller names, and a title
 * laid into it where there is one. This is the mockup's
 * "+-- zfs_rebase: conflicts ---+" in ACS.
 */
static void
pk_rule(int y, chtype left, chtype right, const char *title)
{
	chtype at = pk_style(PK_CO_DIM, 0);

	if (y < 0 || y >= pk_h || pk_w < 2)
		return;
	(void) mvaddch(y, 0, left | at);
	if (pk_w > 2)
		(void) mvhline(y, 1, ACS_HLINE | at, pk_w - 2);
	(void) mvaddch(y, pk_w - 1, right | at);
	if (title != NULL)
		pk_put(y, 3, PK_CO_PLAIN, 0, title);
}

/* The band under the cursor's row: the whole line, inside the box. */
static void
pk_band(int y)
{
	if (y < 0 || y >= pk_h || pk_w < 3)
		return;
	(void) mvhline(y, 1, ' ' | pk_style(PK_CO_PLAIN, 1), pk_w - 2);
}

/*
 * What fits where. The blocks are dropped in the order ZP75 asks
 * for, and the name column gives way before anything else does.
 */
static void
pk_geom(struct pk_geom *g)
{
	int chrome;

	getmaxyx(stdscr, pk_h, pk_w);
	g->g_head = 1;
	g->g_titles = 1;
	g->g_blank = 1;
	g->g_detail = 1;
	g->g_bar = 1;
	for (;;) {
		chrome = (g->g_head != 0 ? PK_H_HEAD : 0) +
		    (g->g_titles != 0 ? PK_H_TITLES : 0) +
		    (g->g_blank != 0 ? PK_H_BLANK : 0) +
		    (g->g_detail != 0 ? PK_H_DETAIL : 0) +
		    (g->g_bar != 0 ? PK_H_BAR : 0);
		if (pk_h - chrome >= 1)
			break;
		if (g->g_blank != 0)
			g->g_blank = 0;
		else if (g->g_detail != 0)
			g->g_detail = 0;
		else if (g->g_head != 0)
			g->g_head = 0;
		else if (g->g_titles != 0)
			g->g_titles = 0;
		else if (g->g_bar != 0)
			g->g_bar = 0;
		else
			break;
	}
	g->g_listy = (g->g_head != 0 ? PK_H_HEAD : 0) +
	    (g->g_titles != 0 ? PK_H_TITLES : 0) +
	    (g->g_blank != 0 ? PK_H_BLANK : 0);
	g->g_listh = pk_h - chrome;
	if (g->g_listh < 0)
		g->g_listh = 0;
	g->g_namex = PK_X_NAME;
	g->g_choicex = pk_w - 10;
	if (g->g_choicex - g->g_namex - 2 < PK_W_NAME_MIN)
		g->g_choicex = pk_w - 2;
	g->g_namew = g->g_choicex - g->g_namex - 2;
	if (g->g_namew < 1)
		g->g_namew = 1;
}

/*
 * ---------------------------------------------------------------
 * A row, column by column.
 * ---------------------------------------------------------------
 */

/*
 * TY: the mockup's five letters, plus "?" for the row whose trees
 * hold objects of different types (ZR_PK_O_MIXED, cell ZP11) and a
 * blank for a name no tree holds at all (ZP15). The colors are the
 * mockup's: text green, binary red, and every other kind cyan, "?"
 * with them because it is the same news -- there is no merge view.
 */
static char
pk_ty_glyph(enum zr_pk_obj ty)
{
	switch (ty) {
	case ZR_PK_O_TEXT:
		return ('T');
	case ZR_PK_O_BINARY:
		return ('B');
	case ZR_PK_O_DIR:
		return ('D');
	case ZR_PK_O_LINK:
		return ('L');
	case ZR_PK_O_SPECIAL:
		return ('S');
	case ZR_PK_O_MIXED:
		return ('?');
	default:
		return (' ');
	}
}

static int
pk_ty_co(enum zr_pk_obj ty)
{
	switch (ty) {
	case ZR_PK_O_TEXT:
		return (PK_CO_GREEN);
	case ZR_PK_O_BINARY:
		return (PK_CO_RED);
	case ZR_PK_O_ABSENT:
		return (PK_CO_DIM);
	default:
		return (PK_CO_CYAN);
	}
}

/*
 * F/O: A an add in green, D a delete in red, E an edit in the side's
 * own color, and "-" dim where the side did nothing to the name.
 */
static char
pk_fo_glyph(enum zr_pk_fo fo)
{
	switch (fo) {
	case ZR_PK_FO_ADD:
		return ('A');
	case ZR_PK_FO_DEL:
		return ('D');
	case ZR_PK_FO_EDIT:
		return ('E');
	default:
		return ('-');
	}
}

static int
pk_fo_co(enum zr_pk_fo fo, int side)
{
	switch (fo) {
	case ZR_PK_FO_ADD:
		return (PK_CO_GREEN);
	case ZR_PK_FO_DEL:
		return (PK_CO_RED);
	case ZR_PK_FO_EDIT:
		return (side == 0 ? PK_CO_FROM : PK_CO_ONTO);
	default:
		return (PK_CO_DIM);
	}
}

/* The same word the document spells, as one letter. */
static char
pk_choice_glyph(enum zr_choice ch)
{
	switch (ch) {
	case ZR_CH_FROM:
		return ('F');
	case ZR_CH_ONTO:
		return ('O');
	case ZR_CH_KEEP:
		return ('K');
	default:
		return ('-');
	}
}

static int
pk_choice_co(enum zr_choice ch)
{
	switch (ch) {
	case ZR_CH_FROM:
		return (PK_CO_FROM);
	case ZR_CH_ONTO:
		return (PK_CO_ONTO);
	case ZR_CH_KEEP:
		return (PK_CO_GREEN);
	default:
		return (PK_CO_DIM);
	}
}

/*
 * The name, in the manifest's own escaping and cut to the column.
 * A name may hold any byte but NUL, so what is drawn is what
 * zr_vis_encode makes of it and never the bytes themselves. A name
 * too long for the column keeps its tail, which is the part that
 * tells one file from another, behind an ellipsis; the cut may fall
 * inside an escape, which is a display and not a document.
 */
static void
pk_name_field(const struct zr_pk_row *row, int width, char *out, size_t outlen)
{
	char buf[PK_NAMEBUF];
	size_t len;

	(void) zr_vis_encode(row->zk_name, row->zk_namelen, buf,
	    sizeof (buf));
	len = strlen(buf);
	if (width < 1)
		width = 1;
	if (len <= (size_t)width || outlen < 5 || width < 5) {
		(void) snprintf(out, outlen, "%.*s", width, buf);
		return;
	}
	(void) snprintf(out, outlen, "...%s", buf + len - (size_t)width + 3);
}

/* GRP: the group's number, or what the row has instead of one. */
static void
pk_grp_field(const struct zr_pk_row *row, char *out, size_t outlen, int *co)
{
	if (row->zk_kind == ZR_PK_L_DRIFT) {
		(void) snprintf(out, outlen, "drift");
		*co = PK_CO_ONTO;
		return;
	}
	if (row->zk_group == 0) {
		(void) snprintf(out, outlen, "hand");
		*co = PK_CO_DIM;
		return;
	}
	(void) snprintf(out, outlen, "%u", (unsigned)row->zk_group);
	*co = PK_CO_DIM;
}

static void
pk_draw_row(const struct zr_picker *pk, const struct pk_geom *g, int y,
    uint32_t i)
{
	char field[PK_LINEBUF], one[2];
	const struct zr_pk_row *row = zr_pk_row(pk, i);
	int sel = (i == zr_pk_cursor(pk));
	int co;

	if (row == NULL)
		return;
	if (sel != 0)
		pk_band(y);
	one[1] = '\0';
	one[0] = pk_ty_glyph(row->zk_ty);
	pk_put(y, PK_X_TY, pk_ty_co(row->zk_ty), sel, one);
	one[0] = pk_fo_glyph(row->zk_fo[0]);
	pk_put(y, PK_X_FO, pk_fo_co(row->zk_fo[0], 0), sel, one);
	pk_put(y, PK_X_FO + 1, PK_CO_DIM, sel, "/");
	one[0] = pk_fo_glyph(row->zk_fo[1]);
	pk_put(y, PK_X_FO + 2, pk_fo_co(row->zk_fo[1], 1), sel, one);
	pk_grp_field(row, field, sizeof (field), &co);
	pk_put(y, PK_X_GRP, co, sel, field);
	pk_name_field(row, g->g_namew, field, sizeof (field));
	pk_put(y, g->g_namex, PK_CO_PLAIN, sel, field);
	one[0] = pk_choice_glyph(zr_pk_choice(row));
	pk_put(y, g->g_choicex, pk_choice_co(zr_pk_choice(row)), sel, one);
}

/*
 * ---------------------------------------------------------------
 * The blocks.
 * ---------------------------------------------------------------
 */

static const char *
pk_mode_word(zr_mode_t mode)
{
	return (mode == ZR_MODE_PERMISSIVE ? "permissive" : "strict");
}

/* A name the documents carry, or a dash where there is none. */
static const char *
pk_or_dash(const char *s)
{
	return (s != NULL && s[0] != '\0' ? s : "-");
}

/*
 * The header: what rebase this is, and what it is holding. The
 * identity is the resolution's own three lines and the manifest's
 * result; the counts are the model's five.
 */
static void
pk_draw_head(const struct zr_picker *pk)
{
	const struct zr_pk_counts *c = zr_pk_counts(pk);
	char line[PK_LINEBUF];
	int x;

	pk_rule(0, ACS_ULCORNER, ACS_URCORNER, PK_TITLE);
	pk_side(1);
	x = pk_putx(1, 1, PK_CO_DIM, 0, " from ");
	x = pk_putx(1, x, PK_CO_FROM, 0, pk_or_dash(pk->pk_res.zs_from));
	x = pk_putx(1, x, PK_CO_DIM, 0, "   onto ");
	x = pk_putx(1, x, PK_CO_ONTO, 0, pk_or_dash(pk->pk_res.zs_onto));
	x = pk_putx(1, x, PK_CO_DIM, 0, "   result ");
	x = pk_putx(1, x, PK_CO_GREEN, 0, pk_or_dash(pk->pk_man.zp_result));
	x = pk_putx(1, x, PK_CO_DIM, 0, "   ");
	(void) pk_putx(1, x, PK_CO_DIM, 0, pk_mode_word(pk->pk_res.zs_mode));
	pk_side(2);
	(void) snprintf(line, sizeof (line), " %u names, %u conflicts in %u "
	    "groups, %u unanswered; %u drift", (unsigned)c->zc_names,
	    (unsigned)c->zc_conflicts, (unsigned)c->zc_groups,
	    (unsigned)c->zc_unanswered, (unsigned)c->zc_drift);
	pk_put(2, 1, PK_CO_PLAIN, 0, line);
	pk_rule(3, ACS_LTEE, ACS_RTEE, NULL);
}

static void
pk_draw_titles(const struct pk_geom *g, int y)
{
	pk_side(y);
	pk_put(y, PK_X_TY - 1, PK_CO_DIM, 0, " TY  F/O  GRP");
	pk_put(y, g->g_namex, PK_CO_DIM, 0, "NAME");
	pk_put(y, g->g_choicex, PK_CO_DIM, 0, "CHOICE");
}

static void
pk_draw_list(const struct zr_picker *pk, const struct pk_geom *g, uint32_t top)
{
	uint32_t n = zr_pk_nrows(pk);
	uint32_t i;
	int y;

	for (y = 0; y < g->g_listh; y++) {
		pk_side(g->g_listy + y);
		i = top + (uint32_t)y;
		if (i < n)
			pk_draw_row(pk, g, g->g_listy + y, i);
	}
	if (n == 0)
		pk_put(g->g_listy, g->g_namex, PK_CO_DIM, 0,
		    "the resolution has no lines");
}

/* One tree of the detail's second line: its size, and what it did. */
static int
pk_draw_tree(int y, int x, const struct zr_pk_row *row, int tree, int co,
    const char *label, const char *verb)
{
	char buf[PK_LINEBUF];

	x = pk_putx(y, x, co, 0, label);
	if (row->zk_obj[tree] == ZR_PK_O_ABSENT)
		(void) snprintf(buf, sizeof (buf), "absent");
	else
		(void) snprintf(buf, sizeof (buf), "%llu B",
		    (unsigned long long)row->zk_size[tree]);
	x = pk_putx(y, x, PK_CO_PLAIN, 0, buf);
	if (verb != NULL) {
		x = pk_putx(y, x, PK_CO_PLAIN, 0, " ");
		x = pk_putx(y, x, co, 0, verb);
	}
	return (pk_putx(y, x, PK_CO_PLAIN, 0, "  "));
}

static const char *
pk_fo_word(enum zr_pk_fo fo)
{
	switch (fo) {
	case ZR_PK_FO_ADD:
		return ("add");
	case ZR_PK_FO_DEL:
		return ("delete");
	case ZR_PK_FO_EDIT:
		return ("edit");
	default:
		return ("untouched");
	}
}

/* The class the manifest gave the group, one word per bit it set. */
static void
pk_class_field(uint32_t flags, char *out, size_t outlen)
{
	uint32_t bit;
	size_t at = 0;
	int n;

	out[0] = '\0';
	for (bit = 1; bit != 0 && at + 1 < outlen; bit <<= 1) {
		if ((flags & bit) == 0)
			continue;
		n = snprintf(out + at, outlen - at, "%s%s",
		    at == 0 ? "" : "+", zr_conflict_name(bit));
		if (n < 0 || (size_t)n >= outlen - at)
			break;
		at += (size_t)n;
	}
}

/*
 * The two lines under the list: the selected row's why line and its
 * three trees. A row with no record behind it -- a drift line, and a
 * conflict line a hand added -- says so, so that the pane is never
 * the last row's still standing (ZP6).
 */
static void
pk_draw_detail(const struct zr_picker *pk, int y)
{
	const struct zr_pk_row *row = zr_pk_row(pk, zr_pk_cursor(pk));
	char buf[PK_LINEBUF];
	uint32_t names;
	int x;

	pk_rule(y, ACS_LTEE, ACS_RTEE, NULL);
	pk_side(y + 1);
	pk_side(y + 2);
	pk_rule(y + 3, ACS_LTEE, ACS_RTEE, NULL);
	if (row == NULL)
		return;
	if (row->zk_rec == NULL) {
		if (row->zk_kind == ZR_PK_L_DRIFT)
			pk_put(y + 1, 1, PK_CO_DIM, 0, " why  a drift line a "
			    "gate wrote; the manifest has no record for it");
		else
			pk_put(y + 1, 1, PK_CO_DIM, 0, " why  a conflict line "
			    "a hand added; the manifest has no record for it");
		pk_put(y + 2, 1, PK_CO_DIM, 0,
		    " base -  from -  onto -  in no group");
		return;
	}
	x = pk_putx(y + 1, 1, PK_CO_DIM, 0, " why  ");
	x = pk_putx(y + 1, x, PK_CO_PLAIN, 0, pk_or_dash(row->zk_rec->zr_why));
	pk_class_field(row->zk_rec->zr_flags, buf, sizeof (buf));
	if (buf[0] != '\0') {
		x = pk_putx(y + 1, x, PK_CO_PLAIN, 0, "  ");
		(void) pk_putx(y + 1, x, PK_CO_CYAN, 0, buf);
	}
	x = pk_draw_tree(y + 2, 1, row, ZR_PK_T_BASE, PK_CO_DIM, " base ",
	    NULL);
	x = pk_draw_tree(y + 2, x, row, ZR_PK_T_FROM, PK_CO_FROM, "from ",
	    pk_fo_word(row->zk_fo[0]));
	x = pk_draw_tree(y + 2, x, row, ZR_PK_T_ONTO, PK_CO_ONTO, "onto ",
	    pk_fo_word(row->zk_fo[1]));
	names = zr_pk_group_names(pk, row->zk_group);
	if (names == 1)
		(void) snprintf(buf, sizeof (buf), "group %u is this one name",
		    (unsigned)row->zk_group);
	else
		(void) snprintf(buf, sizeof (buf), "group %u is %u names",
		    (unsigned)row->zk_group, (unsigned)names);
	(void) pk_putx(y + 2, x, PK_CO_DIM, 0, buf);
}

/*
 * The key bar, which is also where a refused key says why. The
 * message is peeked at and never taken off the queue: the caller
 * prints the whole queue after endwin, and a line printed here would
 * be a line printed while curses is up. The line to draw is the
 * caller's, since the two screens have two sets of keys.
 */
static void
pk_draw_bar(int y, const char *line)
{
	chtype at = pk_style(PK_CO_DIM, 0);
	char buf[PK_LINEBUF];

	if (y < 0 || y >= pk_h || pk_w < 3)
		return;
	(void) mvaddch(y, 0, ACS_VLINE | at);
	(void) mvhline(y, 1, ' ' | (chtype)A_REVERSE, pk_w - 2);
	(void) mvaddch(y, pk_w - 1, ACS_VLINE | at);
	(void) snprintf(buf, sizeof (buf), " %s", line);
	(void) attrset(A_REVERSE);
	(void) mvaddnstr(y, 1, buf, pk_w - 2);
	(void) attrset(A_NORMAL);
}

static void
pk_draw(const struct zr_picker *pk, const struct pk_geom *g, uint32_t top,
    const char *note)
{
	int y;

	(void) erase();
	if (g->g_head != 0)
		pk_draw_head(pk);
	y = g->g_listy;
	if (g->g_titles != 0)
		pk_draw_titles(g, y - (g->g_blank != 0 ? 2 : 1));
	if (g->g_blank != 0)
		pk_side(y - 1);
	pk_draw_list(pk, g, top);
	y = g->g_listy + g->g_listh;
	if (g->g_detail != 0) {
		pk_draw_detail(pk, y);
		y += PK_H_DETAIL;
	}
	if (g->g_bar != 0)
		pk_draw_bar(y, note != NULL ? note : PK_KEYS);
	(void) refresh();
}

/*
 * ---------------------------------------------------------------
 * Screen 2: a text conflict, three ways (plan section 3.4).
 * ---------------------------------------------------------------
 *
 * The chunk sequence the merge library built is the whole of what is
 * drawn here (v4-merge3.md section 1: "The screens are built from the
 * chunk sequence and nothing else"). One walk of that array with the
 * three cursors builds two row lists:
 *
 *   - the side rows, one row shared by the FROM and ONTO panes, so
 *     that one chunk occupies the same screen rows in both (ZP102).
 *     A chunk is as tall as the largest of its three ranges; a pane
 *     with fewer lines than that fills the rest with base's lines
 *     where that side removed them -- the mockup's minus line -- and
 *     with nothing where it did not.
 *   - the result rows, which are the answer of v4-merge3.md section
 *     6, asked of zr_m3_answer so that what is drawn and what
 *     zr_m3_result writes come off one table. A conflict chunk that
 *     has not been picked draws its two halves between the three
 *     marker lines; the markers live here, on the screen, and reach
 *     no file by any path (ruling 7).
 *
 * The keys are the model's: 1 and 2 answer the hunk under the cursor,
 * b puts its base range in the result pane's place, n and p move
 * between the conflicting hunks, c folds every stretch that needs no
 * choice away, w
 * writes and Esc goes back. Nothing here changes a chunk or a choice
 * itself, and nothing here writes a file.
 */

/* The key bar of screen 2. There is no result editor this sprint. */
#define	PK_MKEYS	"1/2 take from/onto  b base  n/p hunk  " \
			"c conflicts only  w write  esc back"

/*
 * One drawn cell: the gutter, the line number and the text, in
 * PK_M_HEAD columns plus what is left for the text. The mockup's
 * "+  3  ifconfig_em0_ipv6=..." is gutter, a space, the number in
 * PK_M_NUMW columns, and two spaces.
 */
#define	PK_M_HEAD	8
#define	PK_M_NUMW	4

/* The rows that are not the panes: three rules, two headers, a bar. */
#define	PK_M_CHROME	6

/* What a cell holds. A blank one is a filler row and draws nothing. */
#define	PK_MR_BLANK	0
#define	PK_MR_LINE	1
#define	PK_MR_MARK	2
#define	PK_MR_FOLD	3

/* The four fixed lines a cell can be, by index into pk_markword. */
#define	PK_MK_FROM	0
#define	PK_MK_MID	1
#define	PK_MK_ONTO	2
#define	PK_MK_NOBASE	3

static const char *const pk_markword[] = {
	"<<<<<<< from", "=======", ">>>>>>> onto",
	"... base holds no lines here ..."
};

struct pk_cell {
	unsigned char	c_kind;		/* PK_MR_* */
	unsigned char	c_file;		/* ZR_M3_F_*, for PK_MR_LINE */
	unsigned char	c_gut;		/* the gutter character */
	unsigned char	c_co;		/* PK_CO_* */
	unsigned char	c_num;		/* draw the line's number */
	unsigned char	c_mark;		/* PK_MK_*, for PK_MR_MARK */
	uint32_t	c_line;		/* the line, or the fold's count */
};

/* One row of the side panes, and one of the result pane. */
struct pk_srow {
	uint32_t	s_chunk;
	struct pk_cell	s_from;
	struct pk_cell	s_onto;
};

struct pk_rrow {
	uint32_t	r_chunk;
	struct pk_cell	r_cell;
};

/*
 * What one draw of screen 2 needs: the two row lists, where each
 * pane is scrolled to, and the inside-conflict hint for the hunk the
 * cursor is on. The hint is the cursor's alone -- it is one libdiff
 * run per conflict chunk and the hunk being decided is the one whose
 * agreeing lines are worth dimming; every other conflict line draws
 * as a difference, which it is.
 */
struct pk_view {
	struct pk_srow		*v_side;
	uint32_t		v_nside;
	uint32_t		v_stop;
	struct pk_rrow		*v_res;
	uint32_t		v_nres;
	uint32_t		v_rtop;
	struct zr_m3_hint	v_hint;
	int			v_hinted;
	uint32_t		v_hchunk;
};

static void
pk_view_fini(struct pk_view *v)
{
	if (v->v_hinted != 0)
		zr_m3_hint_fini(&v->v_hint);
	free(v->v_side);
	free(v->v_res);
	memset(v, 0, sizeof (*v));
}

static uint32_t
pk_max3(uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t n = a > b ? a : b;

	return (n > c ? n : c);
}

static void
pk_cell_line(struct pk_cell *cell, int file, uint32_t line, int gut, int co,
    int num)
{
	memset(cell, 0, sizeof (*cell));
	cell->c_kind = PK_MR_LINE;
	cell->c_file = (unsigned char)file;
	cell->c_line = line;
	cell->c_gut = (unsigned char)gut;
	cell->c_co = (unsigned char)co;
	cell->c_num = (unsigned char)(num != 0);
}

static void
pk_cell_mark(struct pk_cell *cell, int mark, int co)
{
	memset(cell, 0, sizeof (*cell));
	cell->c_kind = PK_MR_MARK;
	cell->c_mark = (unsigned char)mark;
	cell->c_co = (unsigned char)co;
	cell->c_gut = '!';
}

static void
pk_cell_fold(struct pk_cell *cell, uint32_t lines)
{
	memset(cell, 0, sizeof (*cell));
	cell->c_kind = PK_MR_FOLD;
	cell->c_line = lines;
	cell->c_co = PK_CO_DIM;
	cell->c_gut = ' ';
}

/*
 * What color a conflict line takes in a side pane: dim where the
 * hint says the two sides hold that line in common, and the
 * difference color where they do not. The hint is a hint and nothing
 * more -- it never splits a chunk and never reaches the sequence
 * (merge.h) -- so a chunk with no hint of its own draws every line
 * as a difference.
 */
static int
pk_hint_co(const struct pk_view *v, uint32_t chunk, int side, uint32_t r)
{
	const unsigned char *marks;
	uint32_t n;

	if (v->v_hinted == 0 || v->v_hchunk != chunk)
		return (PK_CO_RED);
	marks = side == 0 ? v->v_hint.from_marks : v->v_hint.onto_marks;
	n = side == 0 ? v->v_hint.from_n : v->v_hint.onto_n;
	if (marks == NULL || r >= n || marks[r] != ZR_M3_HINT_SAME)
		return (PK_CO_RED);
	return (PK_CO_DIM);
}

/*
 * One side pane's cell for row r of chunk ci. The side is 0 for from
 * and 1 for onto. A side that did not touch the chunk draws its lines
 * plain; one that did draws them with a plus, or a bang inside a
 * conflict, and then base's lines with a minus for the rows it has
 * left over, which is where the lines it removed are.
 */
static void
pk_side_cell(struct pk_cell *cell, const struct zr_pk_merge *mg,
    const struct pk_view *v, uint32_t ci, int side, uint32_t r)
{
	const struct zr_m3_chunk *c = &mg->pm_m3.chunks[ci];
	uint32_t nb = c->base_hi - c->base_lo;
	uint32_t lo = side == 0 ? c->from_lo : c->onto_lo;
	uint32_t ns = side == 0 ? c->from_hi - c->from_lo :
	    c->onto_hi - c->onto_lo;
	int changed;

	memset(cell, 0, sizeof (*cell));
	changed = c->kind != ZR_M3_STABLE &&
	    !(c->kind == ZR_M3_FROM && side == 1) &&
	    !(c->kind == ZR_M3_ONTO && side == 0);
	if (r < ns) {
		int file = side == 0 ? ZR_M3_F_FROM : ZR_M3_F_ONTO;

		if (c->kind == ZR_M3_CONFLICT)
			pk_cell_line(cell, file, lo + r, '!',
			    pk_hint_co(v, ci, side, r), 1);
		else if (changed != 0)
			pk_cell_line(cell, file, lo + r, '+', PK_CO_GREEN, 1);
		else
			pk_cell_line(cell, file, lo + r, ' ', PK_CO_PLAIN, 1);
		return;
	}
	if (changed != 0 && r < nb)
		pk_cell_line(cell, ZR_M3_F_BASE, c->base_lo + r, '-',
		    PK_CO_RED, 0);
}

/*
 * The side panes' rows, counted when out is NULL and filled when it
 * is not, so that the two passes cannot fall out of step.
 */
static uint32_t
pk_side_fill(const struct zr_pk_merge *mg, const struct pk_view *v,
    struct pk_srow *out)
{
	const struct zr_m3 *m = &mg->pm_m3;
	uint32_t i, r, n = 0;

	for (i = 0; i < m->nchunks; i++) {
		const struct zr_m3_chunk *c = &m->chunks[i];
		uint32_t nf = c->from_hi - c->from_lo;
		uint32_t no = c->onto_hi - c->onto_lo;
		uint32_t tall;

		if (mg->pm_only != 0 && c->kind != ZR_M3_CONFLICT) {
			/*
			 * Conflicts only means conflicts only (the author,
			 * on the box, 2026-09-10): every chunk that needs
			 * no choice -- stable, one side's own, both the
			 * same -- folds, and a run of them is one row.
			 */
			uint32_t first = i, sf = 0, so = 0;

			while (i < m->nchunks &&
			    m->chunks[i].kind != ZR_M3_CONFLICT) {
				const struct zr_m3_chunk *k = &m->chunks[i];

				sf += k->from_hi - k->from_lo;
				so += k->onto_hi - k->onto_lo;
				i++;
			}
			i--;
			if (out != NULL) {
				out[n].s_chunk = first;
				pk_cell_fold(&out[n].s_from, sf);
				pk_cell_fold(&out[n].s_onto, so);
			}
			n++;
			continue;
		}
		tall = pk_max3(c->base_hi - c->base_lo, nf, no);
		for (r = 0; r < tall; r++) {
			if (out != NULL) {
				out[n].s_chunk = i;
				pk_side_cell(&out[n].s_from, mg, v, i, 0, r);
				pk_side_cell(&out[n].s_onto, mg, v, i, 1, r);
			}
			n++;
		}
	}
	return (n);
}

/* The result pane's rows, counted and filled the same way. */
static uint32_t
pk_res_fill(const struct zr_pk_merge *mg, struct pk_rrow *out)
{
	const struct zr_m3 *m = &mg->pm_m3;
	uint32_t i, r, n = 0, lo = 0, hi = 0;
	int file, gut, co;

	for (i = 0; i < m->nchunks; i++) {
		const struct zr_m3_chunk *c = &m->chunks[i];

		if (mg->pm_only != 0 && c->kind != ZR_M3_CONFLICT) {
			/* the same run as the side panes', by its answers */
			uint32_t first = i, sum = 0;

			while (i < m->nchunks &&
			    m->chunks[i].kind != ZR_M3_CONFLICT) {
				(void) zr_m3_answer(m, i, &lo, &hi);
				sum += hi - lo;
				i++;
			}
			i--;
			if (out != NULL) {
				out[n].r_chunk = first;
				pk_cell_fold(&out[n].r_cell, sum);
			}
			n++;
			continue;
		}
		if (mg->pm_base != 0 && i == mg->pm_cursor) {
			/* b: the hunk's base range, which every chunk keeps */
			if (c->base_hi == c->base_lo) {
				if (out != NULL) {
					out[n].r_chunk = i;
					pk_cell_mark(&out[n].r_cell,
					    PK_MK_NOBASE, PK_CO_DIM);
				}
				n++;
				continue;
			}
			for (r = c->base_lo; r < c->base_hi; r++) {
				if (out != NULL) {
					out[n].r_chunk = i;
					pk_cell_line(&out[n].r_cell,
					    ZR_M3_F_BASE, r, 'b', PK_CO_DIM, 1);
				}
				n++;
			}
			continue;
		}
		if (c->kind == ZR_M3_CONFLICT && c->pick == ZR_M3_PICK_NONE) {
			/* the two halves between the markers, until picked */
			if (out != NULL) {
				out[n].r_chunk = i;
				pk_cell_mark(&out[n].r_cell, PK_MK_FROM,
				    PK_CO_FROM);
			}
			n++;
			for (r = c->from_lo; r < c->from_hi; r++) {
				if (out != NULL) {
					out[n].r_chunk = i;
					pk_cell_line(&out[n].r_cell,
					    ZR_M3_F_FROM, r, '!', PK_CO_FROM,
					    1);
				}
				n++;
			}
			if (out != NULL) {
				out[n].r_chunk = i;
				pk_cell_mark(&out[n].r_cell, PK_MK_MID,
				    PK_CO_DIM);
			}
			n++;
			for (r = c->onto_lo; r < c->onto_hi; r++) {
				if (out != NULL) {
					out[n].r_chunk = i;
					pk_cell_line(&out[n].r_cell,
					    ZR_M3_F_ONTO, r, '!', PK_CO_ONTO,
					    1);
				}
				n++;
			}
			if (out != NULL) {
				out[n].r_chunk = i;
				pk_cell_mark(&out[n].r_cell, PK_MK_ONTO,
				    PK_CO_ONTO);
			}
			n++;
			continue;
		}
		file = zr_m3_answer(m, i, &lo, &hi);
		if (c->kind == ZR_M3_STABLE) {
			gut = ' ';
			co = PK_CO_PLAIN;
		} else if (c->kind == ZR_M3_CONFLICT) {
			gut = '!';
			co = file == ZR_M3_F_ONTO ? PK_CO_ONTO : PK_CO_FROM;
		} else {
			gut = '+';
			co = PK_CO_GREEN;
		}
		for (r = lo; r < hi; r++) {
			if (out != NULL) {
				out[n].r_chunk = i;
				pk_cell_line(&out[n].r_cell, file, r, gut, co,
				    1);
			}
			n++;
		}
	}
	return (n);
}

/*
 * Build both row lists for the state the merge is in now. The two
 * scroll positions are kept across a rebuild and clipped afterwards,
 * so that a pick redraws in place rather than jumping to the top.
 * Returns -1 out of memory, with the view emptied.
 */
static int
pk_view_build(const struct zr_pk_merge *mg, struct pk_view *v)
{
	uint32_t stop = v->v_stop, rtop = v->v_rtop;
	char err[PK_LINEBUF];

	pk_view_fini(v);
	v->v_stop = stop;
	v->v_rtop = rtop;
	if (mg->pm_cursor < mg->pm_m3.nchunks &&
	    zr_m3_hint(&mg->pm_m3, mg->pm_cursor, &v->v_hint, err,
	    sizeof (err)) == 0) {
		v->v_hinted = 1;
		v->v_hchunk = mg->pm_cursor;
	}
	v->v_nside = pk_side_fill(mg, v, NULL);
	v->v_nres = pk_res_fill(mg, NULL);
	if (v->v_nside != 0) {
		v->v_side = calloc(v->v_nside, sizeof (*v->v_side));
		if (v->v_side == NULL)
			goto fail;
		(void) pk_side_fill(mg, v, v->v_side);
	}
	if (v->v_nres != 0) {
		v->v_res = calloc(v->v_nres, sizeof (*v->v_res));
		if (v->v_res == NULL)
			goto fail;
		(void) pk_res_fill(mg, v->v_res);
	}
	return (0);
fail:
	pk_view_fini(v);
	return (-1);
}

/*
 * ---------------------------------------------------------------
 * Screen 2, drawn.
 * ---------------------------------------------------------------
 */

/* Where the blocks of screen 2 stand in the window there is. */
struct pk_mgeom {
	int	g_sidey;	/* the first row of the two side panes */
	int	g_sideh;
	int	g_resy;		/* the first row of the result pane */
	int	g_resh;
	int	g_lx;		/* the from pane */
	int	g_lw;
	int	g_divx;		/* the line between the two */
	int	g_rx;		/* the onto pane */
	int	g_rw;
	int	g_bary;
};

static void
pk_mgeom(struct pk_mgeom *g)
{
	int avail;

	getmaxyx(stdscr, pk_h, pk_w);
	avail = pk_h - PK_M_CHROME;
	if (avail < 2)
		avail = 2;
	g->g_sidey = 2;
	g->g_sideh = avail / 2;
	g->g_resh = avail - g->g_sideh;
	g->g_resy = g->g_sidey + g->g_sideh + 2;
	g->g_bary = g->g_resy + g->g_resh + 1;
	g->g_lx = 1;
	g->g_lw = (pk_w - 3) / 2;
	if (g->g_lw < 1)
		g->g_lw = 1;
	g->g_divx = g->g_lx + g->g_lw;
	g->g_rx = g->g_divx + 1;
	g->g_rw = pk_w - 1 - g->g_rx;
	if (g->g_rw < 1)
		g->g_rw = 1;
}

/* One string, clipped to a pane's own width as well as the window's. */
static void
pk_putm(int y, int x, int w, int co, int sel, const char *s)
{
	char buf[PK_LINEBUF];

	if (w <= 0)
		return;
	if ((size_t)w >= sizeof (buf))
		w = (int)sizeof (buf) - 1;
	(void) snprintf(buf, (size_t)w + 1, "%s", s);
	pk_put(y, x, co, sel, buf);
}

/*
 * One line of one file, as bytes a terminal may be shown: a tab to
 * the next multiple of eight, a printable byte as itself, and every
 * other byte as a backslash and three octal digits, which is the
 * manifest's own escaping (vis.h) said over file text rather than
 * over a name. The trailing newline is not drawn; that a line lacks
 * one is a fact the merge keeps and the write carries, and it is not
 * something the screen can show in a column.
 */
static void
pk_line_text(const struct zr_m3 *m, int file, uint32_t idx, char *out,
    size_t outlen)
{
	const struct zr_m3_file *f;
	const struct zr_m3_line *ln;
	size_t at = 0, i, n;
	unsigned char ch;

	out[0] = '\0';
	if (file == ZR_M3_F_BASE)
		f = &m->base;
	else if (file == ZR_M3_F_ONTO)
		f = &m->onto;
	else
		f = &m->from;
	if (idx >= f->nlines)
		return;
	ln = &f->lines[idx];
	n = ln->len;
	while (n > 0 && f->bytes[ln->off + n - 1] == '\n')
		n--;
	for (i = 0; i < n && at + 5 < outlen; i++) {
		ch = f->bytes[ln->off + i];
		if (ch == '\t') {
			out[at++] = ' ';
			while ((at % 8) != 0 && at + 5 < outlen)
				out[at++] = ' ';
			continue;
		}
		if (ch < 0x20 || ch >= 0x7f) {
			(void) snprintf(out + at, outlen - at, "\\%03o", ch);
			at += 4;
			continue;
		}
		out[at++] = (char)ch;
	}
	out[at] = '\0';
}

/* One cell: the gutter, the number, and what is left for the text. */
static void
pk_draw_cell(const struct zr_m3 *m, int y, int x, int w,
    const struct pk_cell *c, int sel)
{
	char text[PK_LINEBUF], num[PK_M_NUMW + 1], gut[2];

	if (c->c_kind == PK_MR_BLANK || w <= 0)
		return;
	gut[0] = (char)c->c_gut;
	gut[1] = '\0';
	pk_put(y, x, c->c_co, sel, gut);
	if (c->c_kind == PK_MR_FOLD) {
		(void) snprintf(text, sizeof (text),
		    "... %lu line%s with no conflict ...",
		    (unsigned long)c->c_line, c->c_line == 1 ? "" : "s");
		pk_putm(y, x + PK_M_HEAD, w - PK_M_HEAD, PK_CO_DIM, sel, text);
		return;
	}
	if (c->c_kind == PK_MR_MARK) {
		pk_putm(y, x + PK_M_HEAD, w - PK_M_HEAD, c->c_co, sel,
		    pk_markword[c->c_mark]);
		return;
	}
	if (c->c_num != 0) {
		(void) snprintf(num, sizeof (num), "%*lu", PK_M_NUMW,
		    (unsigned long)(c->c_line + 1));
		pk_put(y, x + 2, PK_CO_DIM, sel, num);
	}
	pk_line_text(m, c->c_file, c->c_line, text, sizeof (text));
	pk_putm(y, x + PK_M_HEAD, w - PK_M_HEAD, c->c_co, sel, text);
}

/* The title in the top rule: the name, what it is, and which hunk. */
static void
pk_merge_title(const struct zr_picker *pk, const struct zr_pk_merge *mg,
    char *out, size_t outlen)
{
	const struct zr_pk_row *row = zr_pk_row(pk, mg->pm_row);
	char name[PK_NAMEBUF], grp[32], hunk[64];
	int nw = pk_w - 44;

	if (nw < 8)
		nw = 8;
	pk_name_field(row, nw, name, sizeof (name));
	if (row->zk_kind == ZR_PK_L_DRIFT)
		(void) snprintf(grp, sizeof (grp), "drift");
	else if (row->zk_group == 0)
		(void) snprintf(grp, sizeof (grp), "hand");
	else
		(void) snprintf(grp, sizeof (grp), "group %u",
		    (unsigned)row->zk_group);
	if (mg->pm_m3.nconflict == 0)
		(void) snprintf(hunk, sizeof (hunk), "nothing to answer");
	else
		(void) snprintf(hunk, sizeof (hunk), "hunk %u of %u",
		    (unsigned)zr_pk_merge_hunk(pk),
		    (unsigned)mg->pm_m3.nconflict);
	(void) snprintf(out, outlen, " %s  text  %c/%c  %s  %s ", name,
	    pk_fo_glyph(row->zk_fo[0]), pk_fo_glyph(row->zk_fo[1]), grp, hunk);
}

static void
pk_draw_merge(struct zr_picker *pk, const struct zr_pk_merge *mg,
    const struct pk_view *v, const struct pk_mgeom *g, const char *note)
{
	const struct zr_m3 *m = &mg->pm_m3;
	chtype at = pk_style(PK_CO_DIM, 0);
	char buf[PK_LINEBUF];
	uint32_t i;
	int y, x;

	(void) erase();
	/*
	 * The rule, then the line between the panes hanging from it,
	 * then the title over both: a title long enough to reach the
	 * divider is worth more than the tee it covers.
	 */
	pk_rule(0, ACS_ULCORNER, ACS_URCORNER, NULL);
	if (g->g_divx < pk_w - 1)
		(void) mvaddch(0, g->g_divx, ACS_TTEE | at);
	pk_merge_title(pk, mg, buf, sizeof (buf));
	pk_put(0, 3, PK_CO_PLAIN, 0, buf);
	/* the two pane headers, each cut to its own pane */
	pk_side(1);
	x = pk_putx(1, g->g_lx, PK_CO_FROM, 0, " FROM ");
	pk_putm(1, x, g->g_divx - x, PK_CO_DIM, 0,
	    pk_or_dash(pk->pk_res.zs_from));
	x = pk_putx(1, g->g_rx, PK_CO_ONTO, 0, " ONTO ");
	pk_putm(1, x, pk_w - 1 - x, PK_CO_DIM, 0,
	    pk_or_dash(pk->pk_res.zs_onto));
	(void) mvaddch(1, g->g_divx, ACS_VLINE | at);
	for (y = 0; y < g->g_sideh; y++) {
		int ly = g->g_sidey + y;

		pk_side(ly);
		(void) mvaddch(ly, g->g_divx, ACS_VLINE | at);
		i = v->v_stop + (uint32_t)y;
		if (i >= v->v_nside)
			continue;
		pk_draw_cell(m, ly, g->g_lx, g->g_lw, &v->v_side[i].s_from, 0);
		pk_draw_cell(m, ly, g->g_rx, g->g_rw, &v->v_side[i].s_onto, 0);
	}
	y = g->g_sidey + g->g_sideh;
	pk_rule(y, ACS_LTEE, ACS_RTEE, NULL);
	if (g->g_divx < pk_w - 1)
		(void) mvaddch(y, g->g_divx, ACS_BTEE | at);
	/* the result pane's own header, with the two toggles it has */
	pk_side(y + 1);
	x = pk_putx(y + 1, 1, PK_CO_GREEN, 0, " RESULT ");
	(void) snprintf(buf, sizeof (buf), "[%c] base  [%c] conflicts only",
	    mg->pm_base != 0 ? 'x' : ' ', mg->pm_only != 0 ? 'x' : ' ');
	if (pk_w - 1 - (int)strlen(buf) > x + 1) {
		pk_putm(y + 1, x, pk_w - 2 - (int)strlen(buf) - x, PK_CO_DIM,
		    0, pk_or_dash(pk->pk_man.zp_result));
		pk_put(y + 1, pk_w - 1 - (int)strlen(buf), PK_CO_DIM, 0, buf);
	} else {
		pk_putm(y + 1, x, pk_w - 1 - x, PK_CO_DIM, 0,
		    pk_or_dash(pk->pk_man.zp_result));
	}
	for (y = 0; y < g->g_resh; y++) {
		int ry = g->g_resy + y;
		int sel;

		pk_side(ry);
		i = v->v_rtop + (uint32_t)y;
		if (i >= v->v_nres)
			continue;
		sel = v->v_res[i].r_chunk == mg->pm_cursor &&
		    mg->pm_cursor < m->nchunks;
		if (sel != 0)
			pk_band(ry);
		pk_draw_cell(m, ry, 1, pk_w - 2, &v->v_res[i].r_cell, sel);
	}
	pk_rule(g->g_bary - 1, ACS_LTEE, ACS_RTEE, NULL);
	pk_draw_bar(g->g_bary, note != NULL ? note : PK_MKEYS);
	(void) refresh();
}

/*
 * The two panes scrolled so that the hunk the cursor is on is in
 * both. A hunk taller than a pane shows its head; a merge with no
 * conflicting hunk at all is left where it stands.
 */
static void
pk_fit(uint32_t *top, uint32_t first, uint32_t last, int h)
{
	uint32_t hh = (uint32_t)(h > 0 ? h : 1);

	if (last >= *top + hh)
		*top = last - hh + 1;
	if (first < *top)
		*top = first;
}

static void
pk_clamp(uint32_t *top, uint32_t n, int h)
{
	uint32_t hh = (uint32_t)(h > 0 ? h : 1);

	if (n <= hh) {
		*top = 0;
		return;
	}
	if (*top > n - hh)
		*top = n - hh;
}

static void
pk_mscroll(const struct zr_pk_merge *mg, struct pk_view *v,
    const struct pk_mgeom *g)
{
	uint32_t i, first = 0, last = 0;
	int have = 0;

	for (i = 0; i < v->v_nside; i++) {
		if (v->v_side[i].s_chunk != mg->pm_cursor)
			continue;
		if (have == 0)
			first = i;
		have = 1;
		last = i;
	}
	if (have != 0)
		pk_fit(&v->v_stop, first, last, g->g_sideh);
	pk_clamp(&v->v_stop, v->v_nside, g->g_sideh);
	have = 0;
	for (i = 0; i < v->v_nres; i++) {
		if (v->v_res[i].r_chunk != mg->pm_cursor)
			continue;
		if (have == 0)
			first = i;
		have = 1;
		last = i;
	}
	if (have != 0)
		pk_fit(&v->v_rtop, first, last, g->g_resh);
	pk_clamp(&v->v_rtop, v->v_nres, g->g_resh);
}

/*
 * ---------------------------------------------------------------
 * The keys, and the loop.
 * ---------------------------------------------------------------
 */

/*
 * The terminal's key to the model's code, or -1 for a key that is
 * not in the table: the screen swallows that one rather than handing
 * the model a code its own enum does not have.
 */
static int
pk_map(int c)
{
	switch (c) {
	case KEY_UP:
		return (ZR_PK_UP);
	case KEY_DOWN:
		return (ZR_PK_DOWN);
	case KEY_HOME:
		return (ZR_PK_TOP);
	case KEY_END:
		return (ZR_PK_BOTTOM);
	case 'f':
		return (ZR_PK_FROM);
	case 'o':
		return (ZR_PK_ONTO);
	case 'k':
		return (ZR_PK_KEEP);
	case '-':
		return (ZR_PK_CLEAR);
	case 'g':
		return (ZR_PK_GROUP);
	case KEY_ENTER:
	case '\r':
	case '\n':
		return (ZR_PK_ENTER);
	case 's':
		return (ZR_PK_SAVE);
	case 'w':
		return (ZR_PK_WRITE);
	case 'q':
		return (ZR_PK_QUIT);
	default:
		return (-1);
	}
}

/*
 * The same for screen 2, whose keys are its own: the list's keys mean
 * nothing while a merge is open, and these mean nothing on the list.
 * Escape is one byte and is read as one, since keypad(3) has already
 * folded every escape SEQUENCE into a KEY_ code of its own.
 */
static int
pk_map_merge(int c)
{
	switch (c) {
	case '1':
		return (ZR_PK_PICK_FROM);
	case '2':
		return (ZR_PK_PICK_ONTO);
	case 'b':
		return (ZR_PK_BASE);
	case 'n':
		return (ZR_PK_NEXT);
	case 'p':
		return (ZR_PK_PREV);
	case 'c':
		return (ZR_PK_TOGGLE);
	case 'w':
		return (ZR_PK_WRITE);
	case '\033':
		return (ZR_PK_BACK);
	default:
		return (-1);
	}
}

/* The window the list scrolls in, so that the cursor is always in it. */
static void
pk_scroll(const struct zr_picker *pk, const struct pk_geom *g, uint32_t *top)
{
	uint32_t cur = zr_pk_cursor(pk);
	uint32_t n = zr_pk_nrows(pk);
	uint32_t h = (uint32_t)(g->g_listh > 0 ? g->g_listh : 1);

	if (cur < *top)
		*top = cur;
	else if (cur >= *top + h)
		*top = cur - h + 1;
	if (n > h && *top > n - h)
		*top = n - h;
	if (n <= h)
		*top = 0;
}

/*
 * A resize. The size comes from the terminal itself and goes into
 * curses, because the picker keeps SIGWINCH rather than leaving it
 * to the library's own handler (see the file's head), and the next
 * draw then paints the whole window again.
 */
static void
pk_resize(void)
{
	int rows, cols;

	if (pk_winsize(&rows, &cols) == 0)
		(void) resizeterm(rows, cols);
	(void) clearok(stdscr, TRUE);
}

/*
 * Screen 2's loop, which is screen 1's shape with screen 2's keys.
 * It ends when the merge closes -- Esc, or a write that went through
 * -- and the model is what closes it, so the condition is simply
 * whether one is still open.
 *
 * Returns 0 to go back to the list, 1 where the input went away (the
 * picker leaves, the way it leaves on the list) and -1 where the
 * window went below the floor, which pk_loop hands on unchanged: a
 * resize under screen 2 is a kill of the same screen (ZP75), and
 * leaving through pk_loop is what keeps every way out on the one
 * pk_restore.
 */
static int
pk_merge_loop(struct zr_picker *pk)
{
	const char *note = NULL, *before, *now;
	const struct zr_pk_merge *mg;
	struct pk_mgeom g;
	struct pk_view v;
	int c, k, rc = 0;

	memset(&v, 0, sizeof (v));
	while ((mg = zr_pk_merge(pk)) != NULL) {
		if (pk_winch != 0) {
			pk_winch = 0;
			pk_resize();
		}
		pk_mgeom(&g);
		if (pk_toosmall(pk_h, pk_w) != 0) {
			rc = -1;
			break;
		}
		if (pk_view_build(mg, &v) != 0) {
			zr_pk_note(pk, "the merge view ran out of memory");
			zr_pk_merge_close(pk);
			break;
		}
		pk_mscroll(mg, &v, &g);
		pk_draw_merge(pk, mg, &v, &g, note);
		c = getch();
		if (c == ERR) {
			if (pk_winch != 0)
				continue;
			rc = 1;
			break;
		}
		if (c == KEY_RESIZE)
			continue;
		k = pk_map_merge(c);
		if (k < 0)
			continue;
		before = zr_pk_last(pk);
		(void) zr_pk_key(pk, (enum zr_pk_key)k);
		now = zr_pk_last(pk);
		note = (now != NULL && now != before) ? now : NULL;
	}
	pk_view_fini(&v);
	return (rc);
}

/*
 * Draw, take a key, act. The model decides everything a key means;
 * this reads the four answers.
 *
 * A key press clears the note under the list, and a key that queued
 * a message puts that message there instead of the key bar: the
 * model's queue is a ring, so a line that was not there before the
 * key is a line the key made.
 *
 * getch returning ERR is a signal that interrupted the read -- a
 * resize, which the flag says -- or an input that has gone away, in
 * which case the picker leaves the way q leaves, rather than
 * spinning on a terminal nobody is at.
 *
 * Returns 0 where the model said to leave, and -1 where the window
 * went below the floor: the caller then ends curses, puts the
 * terminal back and leaves with the floor's own line and status 2,
 * whatever the answers on the screen were (ZP75).
 */
static int
pk_loop(struct zr_picker *pk)
{
	const char *note = NULL, *before, *now;
	struct pk_geom g;
	uint32_t top = 0;
	int c, k, n, rc;

	for (;;) {
		if (pk_winch != 0) {
			pk_winch = 0;
			pk_resize();
		}
		pk_geom(&g);
		if (pk_toosmall(pk_h, pk_w) != 0)
			return (-1);
		pk_scroll(pk, &g, &top);
		pk_draw(pk, &g, top, note);
		c = getch();
		if (c == ERR) {
			if (pk_winch != 0)
				continue;
			return (0);
		}
		if (c == KEY_RESIZE)
			continue;
		if (c == KEY_PPAGE || c == KEY_NPAGE) {
			note = NULL;
			for (n = g.g_listh > 1 ? g.g_listh - 1 : 1; n > 0; n--)
				(void) zr_pk_key(pk, c == KEY_PPAGE ?
				    ZR_PK_UP : ZR_PK_DOWN);
			continue;
		}
		k = pk_map(c);
		if (k < 0)
			continue;
		before = zr_pk_last(pk);
		now = NULL;
		switch (zr_pk_key(pk, (enum zr_pk_key)k)) {
		case ZR_PK_EXIT:
			return (0);
		case ZR_PK_OPEN:
			/*
			 * Screen 2, on the row the cursor is on. A merge
			 * that will not open is a queued line and the
			 * list still up; one that opens takes the keys
			 * until it closes, and what it says on the way
			 * out is the note the list then carries.
			 */
			if (zr_pk_merge_open(pk) == 0) {
				rc = pk_merge_loop(pk);
				if (rc < 0)
					return (-1);
				if (rc > 0)
					return (0);
			}
			now = zr_pk_last(pk);
			note = (now != NULL && now != before) ? now : NULL;
			continue;
		default:
			now = zr_pk_last(pk);
			note = (now != NULL && now != before) ? now : NULL;
			continue;
		}
	}
}

/*
 * ---------------------------------------------------------------
 * The way in, and every way out.
 * ---------------------------------------------------------------
 */

int
zr_pk_screen(struct zr_picker *pk, char *err, size_t errlen)
{
	char termname[PK_TERMLEN];
	const char *term;
	int rows, cols, rc;
	SCREEN *sp;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (pk == NULL) {
		(void) snprintf(err, errlen, "there is nothing to draw");
		return (-1);
	}
	/*
	 * The three refusals, all of them before a terminal is opened
	 * or a termios is read, so that a picker that cannot draw has
	 * drawn nothing (plan section 2.6, cells ZP70 to ZP72).
	 */
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		(void) snprintf(err, errlen, "the picker needs a terminal; "
		    "name an editor with -i CMD instead");
		return (-1);
	}
	term = getenv("TERM");
	if (term == NULL || term[0] == '\0') {
		(void) snprintf(err, errlen, "the picker needs TERM set to "
		    "the terminal's type");
		return (-1);
	}
	if (strlen(term) >= sizeof (termname)) {
		(void) snprintf(err, errlen, "TERM names no terminal that "
		    "terminfo could hold");
		return (-1);
	}
	(void) snprintf(termname, sizeof (termname), "%s", term);
	/*
	 * The floor, read from the terminal itself so that a window
	 * too small to draw in is refused with curses never opened
	 * and not one byte written (ZP75). A terminal that will not
	 * say how big it is passes here and is judged again by the
	 * loop's own check, before its first draw.
	 */
	if (pk_winsize(&rows, &cols) == 0 && pk_toosmall(rows, cols) != 0) {
		pk_saysmall(err, errlen, rows, cols);
		return (-1);
	}
	if (tcgetattr(STDIN_FILENO, &pk_tio) != 0) {
		(void) snprintf(err, errlen, "the terminal's settings could "
		    "not be read");
		return (-1);
	}
	pk_tio_saved = 1;
	pk_restored = 0;
	pk_arm();
	/*
	 * newterm and not initscr: initscr prints a line and calls
	 * exit(3) when the terminal cannot be set up, which would be
	 * both a write while the picker is starting and an exit the
	 * launcher never asked for. newterm hands back NULL instead
	 * and says nothing.
	 */
	sp = newterm(termname, stdout, stdin);
	if (sp == NULL) {
		pk_restore();
		(void) snprintf(err, errlen, "terminfo has no terminal of "
		    "type %s", termname);
		return (-1);
	}
	pk_up = 1;
	(void) cbreak();
	(void) noecho();
	(void) nonl();
	(void) keypad(stdscr, TRUE);
	(void) scrollok(stdscr, FALSE);
	(void) curs_set(0);
	pk_colors();
	rc = pk_loop(pk);
	pk_restore();
	(void) delscreen(sp);
	/*
	 * A terminal with no alternate screen -- FreeBSD's termcap
	 * chain for xterm has none -- keeps the picker's last screen
	 * in the scrollback, and endwin leaves the cursor on its last
	 * row, where the first line the tool prints would land on the
	 * key bar (the box, 2026-09-10). One newline puts what follows
	 * under it; with an alternate screen it costs one blank line.
	 */
	(void) fputc('\n', stderr);
	if (rc != 0) {
		/*
		 * The window went below the floor while the picker was
		 * up. The terminal is the person's again; what they had
		 * not saved is gone, which is what a kill of the screen
		 * does, and the line and the status are the floor's own
		 * (ruled 2026-09-10).
		 */
		pk_saysmall(err, errlen, pk_h, pk_w);
		return (-1);
	}
	return (0);
}
