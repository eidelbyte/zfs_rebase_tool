/*
 * zfs_rebase picker: does the terminal database give this terminal
 * an alternate screen?
 *
 * The one question screen.c asks of <term.h>, kept in a file of its
 * own because that header defines macros named lines and columns,
 * which no other file of the picker can live with. Asked after
 * newterm, when cur_term is the terminal the picker is on.
 *
 * FreeBSD's base ncurses reads its capabilities out of termcap, and
 * the termcap chain for xterm there carries no te/ti pair, so curses
 * draws the picker on the main screen and leaves its last frame in
 * the scrollback (the box, 2026-09-10). Where this says no and the
 * terminal is an xterm kind, screen.c switches to the alternate
 * screen itself, around curses, with the sequence every such
 * terminal honors.
 */

#include <curses.h>

#include "picker.h"

/*
 * Last on purpose: <term.h> defines lines and columns as macros, and
 * merge.h, which picker.h brings in, has a field named lines.
 */
#include <term.h>

int
zr_pk_term_has_alt(void)
{
	const char *s = tigetstr("smcup");

	return (s != NULL && s != (const char *)-1 && s[0] != '\0');
}
