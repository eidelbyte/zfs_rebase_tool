/*
 * A stand-in for <curses.h>, for tools/xcheck-freebsd.sh alone.
 *
 * FreeBSD carries ncurses as contrib/ncurses/include/curses.h.in, a
 * template the base build generates the real header from, so a cross
 * check against a FreeBSD source tree has no <curses.h> to include --
 * the same reason that script writes an osreldate.h of its own. This
 * declares what src/plugins/picker/screen.c uses and nothing more, in
 * the shapes ncurses declares them in, so that a call with the wrong
 * arity or the wrong argument type still fails the check. It is never
 * compiled into anything: the box's make freebsd builds against the
 * real header and links -lncursesw, which is the authority.
 *
 * newterm takes a char * here on purpose. ncurses spells it
 * NCURSES_CONST, which is empty on the mac's ncurses and const on
 * FreeBSD's, and screen.c hands it a buffer of its own so that both
 * are satisfied without a cast; a const char * here would let a
 * change that passes the environment's own string through slip past
 * the check on this machine and fail on the mac.
 */

#ifndef	ZR_XCHECK_CURSES_H
#define	ZR_XCHECK_CURSES_H

#include <stdio.h>

typedef unsigned long chtype;
typedef struct screen SCREEN;
typedef struct _win_st WINDOW;

struct _win_st {
	int	_maxy;
	int	_maxx;
};

extern WINDOW *stdscr;

#define	OK		0
#define	ERR		(-1)
#define	TRUE		1
#define	FALSE		0

#define	A_NORMAL	0x00000000UL
#define	A_REVERSE	0x00040000UL
#define	A_BOLD		0x00200000UL
#define	A_DIM		0x00100000UL

#define	COLOR_BLACK	0
#define	COLOR_RED	1
#define	COLOR_GREEN	2
#define	COLOR_YELLOW	3
#define	COLOR_BLUE	4
#define	COLOR_MAGENTA	5
#define	COLOR_CYAN	6
#define	COLOR_WHITE	7

#define	COLOR_PAIR(n)	((int)(n) << 8)

#define	ACS_ULCORNER	((chtype)'l')
#define	ACS_URCORNER	((chtype)'k')
#define	ACS_LTEE	((chtype)'t')
#define	ACS_RTEE	((chtype)'u')
#define	ACS_BTEE	((chtype)'v')
#define	ACS_TTEE	((chtype)'w')
#define	ACS_HLINE	((chtype)'q')
#define	ACS_VLINE	((chtype)'x')

#define	KEY_DOWN	0402
#define	KEY_UP		0403
#define	KEY_HOME	0406
#define	KEY_NPAGE	0522
#define	KEY_PPAGE	0523
#define	KEY_ENTER	0527
#define	KEY_END		0550
#define	KEY_RESIZE	0632

#define	getmaxyx(win, y, x)	((y) = (win)->_maxy + 1, (x) = (win)->_maxx + 1)

extern SCREEN *newterm(char *, FILE *, FILE *);
extern int delscreen(SCREEN *);
extern int endwin(void);
extern int cbreak(void);
extern int noecho(void);
extern int nonl(void);
extern int keypad(WINDOW *, int);
extern int scrollok(WINDOW *, int);
extern int clearok(WINDOW *, int);
extern int curs_set(int);
extern int COLORS;
int has_colors(void);
extern int start_color(void);
extern int use_default_colors(void);
extern int init_pair(short, short, short);
extern int attrset(int);
extern int erase(void);
extern int refresh(void);
extern int getch(void);
extern int mvaddch(int, int, chtype);
extern int mvaddnstr(int, int, const char *, int);
extern int mvhline(int, int, chtype, int);
extern int resizeterm(int, int);

#endif	/* ZR_XCHECK_CURSES_H */
