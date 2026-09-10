/*
 * Stub <term.h> for the FreeBSD cross-check: the one function the
 * picker asks of it. The real header also defines macros named
 * lines and columns, which is why only altscreen.c includes it.
 */
#ifndef ZR_XCHECK_TERM_H
#define	ZR_XCHECK_TERM_H

extern char *tigetstr(const char *);

#endif
