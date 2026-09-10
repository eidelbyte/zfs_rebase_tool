/*
 * The built-in picker's screen 1: the list, in curses, and the
 * terminal discipline around it.
 *
 * The model (model.c) holds the two documents and answers key codes;
 * this file is the only one of the picker that knows what a terminal
 * is. It draws the rows the model built -- the mockup's layout, in
 * sprints/sprint-6/picker-mockup.html -- maps the keys of plan
 * section 3.3 onto enum zr_pk_key, and acts on the four answers:
 * nothing, redraw, open (screen 2, which this build says it has not
 * got), and exit.
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

/*
 * What Enter says until picker-merge lands. The model has already
 * decided the row can be opened three ways (ZR_PK_OPEN); the screen
 * it would open is the next issue's.
 */
#define	PK_NOMERGE	"the merge view is not in this build yet"

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
 * The colors of the mockup, as the eight a terminal has. A style is
 * one of these crossed with whether the row is the selected one: the
 * cursor's row is a dark blue band, so every color has a second pair
 * with the blue background, and a terminal with no colors gets
 * A_REVERSE for the band and A_BOLD for what would have been colored.
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

/* Which pair a color is, unselected and on the cursor's band. */
static short
pk_pair(int co, int sel)
{
	return ((short)(1 + co + (sel != 0 ? PK_CO_N : 0)));
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
		(void) init_pair(pk_pair(co, 0), f, bg);
		(void) init_pair(pk_pair(co, 1), f < 0 ? COLOR_WHITE : f,
		    COLOR_BLUE);
	}
	pk_color = 1;
}

/* One style: the color, or bold and reverse where there is none. */
static chtype
pk_style(int co, int sel)
{
	chtype at = A_NORMAL;

	if (co == PK_CO_DIM)
		at |= A_DIM;
	if (pk_color != 0)
		return (at | (chtype)COLOR_PAIR(pk_pair(co, sel)));
	if (sel != 0)
		at |= A_REVERSE;
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
 * be a line printed while curses is up.
 */
static void
pk_draw_bar(int y, const char *note)
{
	chtype at = pk_style(PK_CO_DIM, 0);
	char buf[PK_LINEBUF];

	if (y < 0 || y >= pk_h || pk_w < 3)
		return;
	(void) mvaddch(y, 0, ACS_VLINE | at);
	(void) mvhline(y, 1, ' ' | (chtype)A_REVERSE, pk_w - 2);
	(void) mvaddch(y, pk_w - 1, ACS_VLINE | at);
	(void) snprintf(buf, sizeof (buf), " %s",
	    note != NULL ? note : PK_KEYS);
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
		pk_draw_bar(y, note);
	(void) refresh();
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
	int c, k, n;

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
			note = PK_NOMERGE;
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
