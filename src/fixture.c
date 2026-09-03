/*
 * fixture: the .zrt parser and the two builders that make a fixture
 * real. The parser turns one text file into three lists of entries
 * and an expect block; zr_fixture_build writes a tree with ordinary
 * POSIX calls and the platform's own calls for the attributes POSIX
 * never standardised; zr_fixture_to_tree builds the same tree as
 * pools in memory, giving each pool a handle that stands for
 * everything the content oracle compares. tests/fixtures/FORMAT.md
 * is the format.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
/*
 * lchflags, strtofflags and the ACL calls sit behind __BSD_VISIBLE,
 * which _XOPEN_SOURCE alone switches off; walk.c asks the same way.
 */
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fixture.h"
#include "name.h"
#include "vis.h"

#define	FX_FILE		0
#define	FX_LINK		1
#define	FX_DIR		2
#define	FX_SYMLINK	3

/* PATH TYPE ARG mode uid gid flags acl, and sixteen xattrs */
#define	FX_MAXFIELD	24
#define	FX_NOTOKEN	0xffffffffu
#define	FX_SLURP_MIN	8192
#define	FX_SLURP_MAX	(64u * 1024u * 1024u)
#define	FX_TAB_MIN	16
#define	FX_TAB_MAX	0x10000000u	/* far under ZR_CONTENT_NONE */
#define	FX_ENTS_MIN	16
#define	FX_XA_MIN	4
#define	FX_DESC_MIN	128
#define	FX_XA_MAXNAME	255		/* EXTATTR_MAXNAMELEN */

#define	FX_NS_USER	"user."
#define	FX_NS_SYSTEM	"system."

/* The attributes an entry line can carry, in the order it carries them. */
#define	FX_A_MODE	0
#define	FX_A_UID	1
#define	FX_A_GID	2
#define	FX_A_FLAGS	3
#define	FX_A_XATTR	4
#define	FX_A_ACL	5
#define	FX_A_N		6

/*
 * ---------------------------------------------------------------
 * The platform section, the mirror of walk.c's. Everything outside
 * it is plain POSIX; here are the three attributes POSIX never
 * standardised, each written the way the walk reads it back:
 *
 *	fx_setx()	one extended attribute, on the link itself,
 *			in the namespace the name's prefix names
 *	fx_setacl()	the ACL, from its acl_from_text(3) text
 *	fx_setflags()	the file flags, already a number
 *	fx_flags_value() the chflags(1) names as that number
 *
 * All of them return 0, or -1 with errno set. FX_HAVE_FFLAGS says
 * the platform has file flags and can name them, and without it the
 * parser refuses a flags= attribute outright: a platform that cannot
 * name a flag cannot set one, and its walk reads none back either.
 * FX_HAVE_ACL says an ACL can be set here, which only the platform
 * line's own platform can do; parsing an acl= needs nothing.
 * ---------------------------------------------------------------
 */

#if defined(__FreeBSD__) || defined(__APPLE__)
#define	FX_HAVE_FFLAGS	1
#endif
#if defined(__FreeBSD__)
#define	FX_HAVE_ACL	1
#define	FX_PLATFORM	"freebsd"	/* what a platform line calls us */
#endif
#if defined(__FreeBSD__)
#include <sys/acl.h>
#include <sys/extattr.h>
#elif defined(__APPLE__)
#include <sys/xattr.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

/*
 * One extended attribute of the link itself. The name is the walk's
 * own spelling, "user.NAME" or "system.NAME"; on FreeBSD that is a
 * namespace and a bare name put back together, and everywhere else
 * it is the literal name, which is how the walk lists it.
 */
static int
fx_setx(const char *full, const char *name, const unsigned char *val,
    size_t len)
{
#if defined(__FreeBSD__)
	const char *bare;
	ssize_t n;
	int ns;

	if (strncmp(name, FX_NS_USER, sizeof (FX_NS_USER) - 1) == 0) {
		ns = EXTATTR_NAMESPACE_USER;
		bare = name + sizeof (FX_NS_USER) - 1;
	} else {
		ns = EXTATTR_NAMESPACE_SYSTEM;
		bare = name + sizeof (FX_NS_SYSTEM) - 1;
	}
	n = extattr_set_link(full, ns, bare, val, len);
	if (n < 0)
		return (-1);
	if ((size_t)n != len) {
		errno = EIO;
		return (-1);
	}
	return (0);
#elif defined(__APPLE__)
	return (setxattr(full, name, val, len, 0, XATTR_NOFOLLOW));
#elif defined(__linux__)
	return (lsetxattr(full, name, val, len, 0));
#else
	(void) full;
	(void) name;
	(void) val;
	(void) len;
	errno = ENOTSUP;
	return (-1);
#endif
}

/*
 * The ACL of the link itself, from the text acl_from_text(3) takes.
 * NFSv4 is the only flavor a fixture writes: it is what ZFS has, and
 * the platform line keeps such a fixture off every other platform.
 */
static int
fx_setacl(const char *full, const char *text)
{
#ifdef FX_HAVE_ACL
	acl_t a;
	int rc, saved;

	a = acl_from_text(text);
	if (a == NULL)
		return (-1);
	rc = acl_set_link_np(full, ACL_TYPE_NFS4, a);
	saved = errno;
	(void) acl_free(a);
	errno = saved;
	return (rc);
#else
	(void) full;
	(void) text;
	errno = ENOTSUP;
	return (-1);
#endif
}

static int
fx_setflags(const char *full, uint32_t flags)
{
#ifdef FX_HAVE_FFLAGS
	return (lchflags(full, flags));
#else
	(void) full;
	(void) flags;
	errno = ENOTSUP;
	return (-1);
#endif
}

#ifdef FX_HAVE_FFLAGS

/*
 * The chflags(1) names, comma separated, as the number lchflags(2)
 * takes. strtofflags(3) writes into the string it is handed and
 * leaves the pointer on the name it choked on, so it gets a copy of
 * the field and the caller gets that name back to put in its
 * message. Returns 0, or -1 with bad holding the offending name.
 */
static int
fx_flags_value(const char *text, size_t len, uint32_t *out, char *bad,
    size_t badlen)
{
	unsigned long set = 0, clr = 0;
	char *buf, *p;
	int rc;

	buf = malloc(len + 1);
	if (buf == NULL) {
		(void) snprintf(bad, badlen, "out of memory");
		return (-1);
	}
	memcpy(buf, text, len);
	buf[len] = '\0';
	p = buf;
	rc = strtofflags(&p, &set, &clr);
	if (rc != 0)
		(void) snprintf(bad, badlen, "%s", p);
	else
		*out = (uint32_t)set;
	free(buf);
	return (rc == 0 ? 0 : -1);
}

#endif	/* FX_HAVE_FFLAGS */

/* One extended attribute of a fixture: bytes, not a string. */
struct fx_xattr {
	char		*fa_name;	/* NUL-terminated, the walk's own */
	unsigned char	*fa_val;
	size_t		fa_len;
};

/*
 * One entry line. fe_arg holds a link's target path or a symlink's
 * target string, both decoded, and is NULL otherwise. fe_token is the
 * token table index of a file's token or of a symlink's target, and
 * FX_NOTOKEN for a directory or a link, whose content comes from the
 * entry that owns their pool. fe_pool is the index of that entry: a
 * link points at its target, everything else at itself, and the
 * owner's fe_content is the handle the whole pool wears.
 */
struct fx_entry {
	char		*fe_path;
	size_t		fe_pathlen;
	char		*fe_arg;
	size_t		fe_arglen;
	char		*fe_acl;	/* NUL-terminated text, else NULL */
	struct fx_xattr	*fe_xattr;	/* sorted by name, bytewise */
	uint32_t	fe_nxattr;
	uint32_t	fe_token;
	uint32_t	fe_pool;
	uint32_t	fe_content;	/* meaningful on a pool owner */
	uint32_t	fe_mode;
	uint32_t	fe_uid;
	uint32_t	fe_gid;
	uint32_t	fe_flags;
	int		fe_type;
	int		fe_has_mode;
	int		fe_has_uid;
	int		fe_has_gid;
	int		fe_has_flags;
};

struct fx_tree {
	struct fx_entry	*ft_ents;
	uint32_t	ft_n;
	uint32_t	ft_cap;
};

/*
 * A table of interned blobs. Two of them: the tokens, whose text a
 * file is written from, and the content classes, whose index is the
 * handle zr_fixture_to_tree hands a pool. Both are small and scanned
 * linearly on purpose -- a fixture holds a handful of each.
 */
struct fx_tab {
	char		**ft_p;
	size_t		*ft_len;
	uint32_t	ft_n;
	uint32_t	ft_cap;
};

struct zr_fixture {
	struct fx_tree	zf_trees[3];
	struct fx_tab	zf_tok;		/* the tokens */
	struct fx_tab	zf_cls;		/* the content classes */
	const char	*zf_platform;	/* "freebsd", or NULL */
	int		zf_platline;
	char		*zf_expect;	/* NULL when there is no block */
};

/* One line split on runs of spaces and tabs. */
struct fx_line {
	const char	*fl_f[FX_MAXFIELD];
	size_t		fl_len[FX_MAXFIELD];
	int		fl_n;
	int		fl_over;	/* the line held more than fl_n */
};

/* The builder's defaults, which are what an absent attribute means. */
struct fx_deflt {
	uint32_t	fd_filemode;
	uint32_t	fd_dirmode;
	uint32_t	fd_uid;
	uint32_t	fd_gid;
};

/* One pool's attributes, every name of it folded in, later wins. */
struct fx_eff {
	const struct fx_xattr	**ef_x;
	uint32_t		ef_nx;
	uint32_t		ef_cap;
	const char		*ef_acl;
	uint32_t		ef_mode;
	uint32_t		ef_uid;
	uint32_t		ef_gid;
	uint32_t		ef_flags;
	int			ef_has_mode;
	int			ef_has_uid;
	int			ef_has_gid;
};

/* A growing byte string: one pool's content descriptor. */
struct fx_buf {
	char		*fb_p;
	size_t		fb_n;
	size_t		fb_cap;
};

static const char *const fx_treename[3] = { "base", "from", "onto" };
static const char *const fx_typename[4] = { "file", "link", "dir", "symlink" };
static const char *const fx_attrname[FX_A_N] = {
	"mode=", "uid=", "gid=", "flags=", "xattr=", "acl="
};
static const size_t fx_attrlen[FX_A_N] = { 5, 4, 4, 6, 6, 4 };

/*
 * Every rejection goes through here, so that every message names the
 * line the reader must go and look at.
 */
static int
fx_errf(char *err, size_t errlen, int line, const char *fmt, ...)
{
	va_list ap;
	char msg[192];

	va_start(ap, fmt);
	(void) vsnprintf(msg, sizeof (msg), fmt, ap);
	va_end(ap);
	if (err != NULL && errlen > 0)
		(void) snprintf(err, errlen, "line %d: %s", line, msg);
	return (-1);
}

/*
 * Read the whole file. Fixtures are small by nature, so the cap is
 * there to turn a mistaken argument into an error and not a swap
 * storm.
 */
static char *
fx_slurp(const char *path, size_t *lenp)
{
	FILE *fp;
	char *buf, *nb;
	size_t cap, len, n;

	fp = fopen(path, "rb");
	if (fp == NULL)
		return (NULL);
	cap = FX_SLURP_MIN;
	len = 0;
	buf = malloc(cap);
	if (buf == NULL) {
		(void) fclose(fp);
		return (NULL);
	}
	for (;;) {
		if (len + 1 == cap) {
			if (cap >= FX_SLURP_MAX) {
				errno = EFBIG;
				break;
			}
			nb = realloc(buf, cap * 2);
			if (nb == NULL)
				break;
			buf = nb;
			cap *= 2;
		}
		n = fread(buf + len, 1, cap - 1 - len, fp);
		len += n;
		if (n == 0) {
			if (ferror(fp))
				break;
			(void) fclose(fp);
			buf[len] = '\0';
			*lenp = len;
			return (buf);
		}
	}
	n = (size_t)errno;
	(void) fclose(fp);
	free(buf);
	errno = (int)n;
	return (NULL);
}

/*
 * A path is absolute, is not the root, ends in no slash, has no
 * empty, "." or ".." component, and holds no NUL, which no path can.
 */
static int
fx_path_ok(const char *p, size_t len)
{
	size_t i, start;

	if (len < 2 || p[0] != '/' || p[len - 1] == '/')
		return (0);
	start = 1;
	for (i = 1; i <= len; i++) {
		if (i < len && p[i] != '/')
			continue;
		if (i == start)
			return (0);
		if (i - start == 1 && p[start] == '.')
			return (0);
		if (i - start == 2 && p[start] == '.' && p[start + 1] == '.')
			return (0);
		start = i + 1;
	}
	for (i = 0; i < len; i++) {
		if (p[i] == '\0')
			return (0);
	}
	return (1);
}

static void
fx_split(const char *s, size_t len, struct fx_line *fl)
{
	size_t i, start;

	fl->fl_n = 0;
	fl->fl_over = 0;
	i = 0;
	while (i < len) {
		while (i < len && (s[i] == ' ' || s[i] == '\t'))
			i++;
		if (i == len)
			break;
		start = i;
		while (i < len && s[i] != ' ' && s[i] != '\t')
			i++;
		if (fl->fl_n == FX_MAXFIELD) {
			fl->fl_over = 1;
			return;
		}
		fl->fl_f[fl->fl_n] = s + start;
		fl->fl_len[fl->fl_n] = i - start;
		fl->fl_n++;
	}
}

static int
fx_is(const struct fx_line *fl, int i, const char *word)
{
	size_t n = strlen(word);

	return (fl->fl_len[i] == n && memcmp(fl->fl_f[i], word, n) == 0);
}

/*
 * Decode one vis-encoded field into a fresh NUL-terminated buffer.
 * Returns 0, -1 on a bad escape, or -2 when out of memory. A decode
 * never grows, so a buffer the size of the field always fits. The
 * length is the decoded one: a value may hold a NUL, and the
 * terminator is there for everything that may not.
 */
static int
fx_decode(const char *s, size_t len, char **outp, size_t *outlenp)
{
	char *buf;
	size_t n;

	buf = malloc(len + 1);
	if (buf == NULL)
		return (-2);
	if (zr_vis_decode(s, len, (unsigned char *)buf, len + 1, &n) != 0) {
		free(buf);
		return (-1);
	}
	buf[n] = '\0';
	*outp = buf;
	*outlenp = n;
	return (0);
}

/*
 * Intern one blob, its index the value the caller keeps. Returns
 * FX_NOTOKEN when the table cannot grow to hold it.
 */
static uint32_t
fx_intern(struct fx_tab *tb, const void *s, size_t len)
{
	char **pv;
	size_t *lv;
	char *copy;
	uint32_t i, cap;

	for (i = 0; i < tb->ft_n; i++) {
		if (tb->ft_len[i] == len &&
		    memcmp(tb->ft_p[i], s, len) == 0)
			return (i);
	}
	if (tb->ft_n == tb->ft_cap) {
		cap = tb->ft_cap == 0 ? FX_TAB_MIN : tb->ft_cap * 2;
		if (cap >= FX_TAB_MAX)
			return (FX_NOTOKEN);
		pv = realloc(tb->ft_p, (size_t)cap * sizeof (char *));
		if (pv == NULL)
			return (FX_NOTOKEN);
		tb->ft_p = pv;
		lv = realloc(tb->ft_len, (size_t)cap * sizeof (size_t));
		if (lv == NULL)
			return (FX_NOTOKEN);
		tb->ft_len = lv;
		tb->ft_cap = cap;
	}
	copy = malloc(len + 1);
	if (copy == NULL)
		return (FX_NOTOKEN);
	if (len > 0)
		memcpy(copy, s, len);
	copy[len] = '\0';
	tb->ft_p[tb->ft_n] = copy;
	tb->ft_len[tb->ft_n] = len;
	return (tb->ft_n++);
}

static void
fx_tab_free(struct fx_tab *tb)
{
	uint32_t i;

	for (i = 0; i < tb->ft_n; i++)
		free(tb->ft_p[i]);
	free(tb->ft_p);
	free(tb->ft_len);
	memset(tb, 0, sizeof (*tb));
}

static struct fx_entry *
fx_add_entry(struct fx_tree *t)
{
	struct fx_entry *ne;
	uint32_t cap;

	if (t->ft_n == t->ft_cap) {
		cap = t->ft_cap == 0 ? FX_ENTS_MIN : t->ft_cap * 2;
		ne = realloc(t->ft_ents, (size_t)cap *
		    sizeof (struct fx_entry));
		if (ne == NULL)
			return (NULL);
		t->ft_ents = ne;
		t->ft_cap = cap;
	}
	ne = &t->ft_ents[t->ft_n++];
	memset(ne, 0, sizeof (*ne));
	ne->fe_token = FX_NOTOKEN;
	ne->fe_content = FX_NOTOKEN;
	ne->fe_pool = t->ft_n - 1;
	return (ne);
}

/* The index of the entry with this exact path, or -1. */
static long
fx_find(const struct fx_tree *t, const char *p, size_t len)
{
	uint32_t i;

	for (i = 0; i < t->ft_n; i++) {
		if (t->ft_ents[i].fe_pathlen == len &&
		    memcmp(t->ft_ents[i].fe_path, p, len) == 0)
			return ((long)i);
	}
	return (-1);
}

static int
fx_attr_value(struct fx_entry *e, int which, const char *v, size_t len)
{
	uint64_t n = 0;
	size_t i;

	if (len == 0)
		return (-1);
	if (which == FX_A_MODE) {
		if (len > 4)
			return (-1);
		for (i = 0; i < len; i++) {
			if (v[i] < '0' || v[i] > '7')
				return (-1);
			n = n * 8 + (uint64_t)(v[i] - '0');
		}
		e->fe_mode = (uint32_t)n;
		e->fe_has_mode = 1;
		return (0);
	}
	for (i = 0; i < len; i++) {
		if (v[i] < '0' || v[i] > '9')
			return (-1);
		n = n * 10 + (uint64_t)(v[i] - '0');
		if (n > 0xffffffffu)
			return (-1);
	}
	if (which == FX_A_UID) {
		e->fe_uid = (uint32_t)n;
		e->fe_has_uid = 1;
	} else {
		e->fe_gid = (uint32_t)n;
		e->fe_has_gid = 1;
	}
	return (0);
}

/* flags=NAME[,NAME...], the chflags(1) names, as one number. */
static int
fx_attr_flags(struct fx_entry *e, const char *v, size_t len, char *err,
    size_t errlen, int line)
{
#ifdef FX_HAVE_FFLAGS
	char bad[64];

	if (len == 0) {
		return (fx_errf(err, errlen, line,
		    "flags= wants one chflags(1) name at least"));
	}
	if (fx_flags_value(v, len, &e->fe_flags, bad, sizeof (bad)) != 0) {
		return (fx_errf(err, errlen, line, "flags=%.*s: \"%s\" is "
		    "not a file flag chflags(1) knows", (int)len, v, bad));
	}
	e->fe_has_flags = 1;
	return (0);
#else
	(void) e;
	(void) v;
	(void) len;
	return (fx_errf(err, errlen, line, "flags= wants a platform with BSD "
	    "file flags, which this one has not; FreeBSD and macOS have "
	    "them"));
#endif
}

/*
 * One more extended attribute of this entry, kept in the bytewise
 * name order the walk sorts into, which is the order the fixture
 * must have written them in. Out of order or twice is a rejection:
 * the format reads like the walk or it says nothing.
 */
static int
fx_attr_xattr(const struct zr_fixture *fx, struct fx_entry *e, const char *v,
    size_t len, char *err, size_t errlen, int line)
{
	struct fx_xattr *tab;
	const char *colon;
	char *name;
	unsigned char *val;
	size_t namelen, vlen, plen, i;
	int cmp;

	colon = memchr(v, ':', len);
	if (colon == NULL) {
		return (fx_errf(err, errlen, line, "xattr= is NAME:VALUE, "
		    "and \"%.*s\" holds no colon", (int)len, v));
	}
	namelen = (size_t)(colon - v);
	for (i = 0; i < namelen; i++) {
		if (v[i] < 0x21 || v[i] > 0x7e) {
			return (fx_errf(err, errlen, line, "an xattr name is "
			    "printable ASCII with no whitespace"));
		}
	}
	if (namelen > sizeof (FX_NS_USER) - 1 &&
	    memcmp(v, FX_NS_USER, sizeof (FX_NS_USER) - 1) == 0) {
		plen = sizeof (FX_NS_USER) - 1;
	} else if (namelen > sizeof (FX_NS_SYSTEM) - 1 &&
	    memcmp(v, FX_NS_SYSTEM, sizeof (FX_NS_SYSTEM) - 1) == 0) {
		plen = sizeof (FX_NS_SYSTEM) - 1;
		if (fx->zf_platform == NULL) {
			return (fx_errf(err, errlen, line, "\"%.*s\" is in "
			    "the system namespace, which only the platform "
			    "of a \"platform freebsd\" line can set",
			    (int)namelen, v));
		}
	} else {
		return (fx_errf(err, errlen, line, "\"%.*s\" names no "
		    "namespace; an xattr name is \"user.NAME\" or "
		    "\"system.NAME\", the way the walk spells it",
		    (int)namelen, v));
	}
	if (namelen - plen > FX_XA_MAXNAME) {
		return (fx_errf(err, errlen, line,
		    "an xattr name is at most %d bytes past its namespace",
		    FX_XA_MAXNAME));
	}
	name = malloc(namelen + 1);
	if (name == NULL)
		return (fx_errf(err, errlen, line, "out of memory"));
	memcpy(name, v, namelen);
	name[namelen] = '\0';
	if (e->fe_nxattr > 0) {
		cmp = strcmp(e->fe_xattr[e->fe_nxattr - 1].fa_name, name);
		if (cmp >= 0) {
			free(name);
			return (fx_errf(err, errlen, line, "the xattr \"%.*s\""
			    " comes %s; a line lists its attributes in "
			    "bytewise name order, each name once",
			    (int)namelen, v, cmp == 0 ? "twice" :
			    "before one it must follow"));
		}
	}
	vlen = len - namelen - 1;
	val = malloc(vlen + 1);
	if (val == NULL) {
		free(name);
		return (fx_errf(err, errlen, line, "out of memory"));
	}
	if (zr_vis_decode(colon + 1, vlen, val, vlen + 1, &vlen) != 0) {
		free(name);
		free(val);
		return (fx_errf(err, errlen, line, "bad escape in the value "
		    "of \"%.*s\"", (int)namelen, v));
	}
	tab = realloc(e->fe_xattr, (size_t)(e->fe_nxattr + 1) *
	    sizeof (struct fx_xattr));
	if (tab == NULL) {
		free(name);
		free(val);
		return (fx_errf(err, errlen, line, "out of memory"));
	}
	e->fe_xattr = tab;
	tab[e->fe_nxattr].fa_name = name;
	tab[e->fe_nxattr].fa_val = val;
	tab[e->fe_nxattr].fa_len = vlen;
	e->fe_nxattr++;
	return (0);
}

/*
 * acl=VISTEXT. Nothing here knows how to build an ACL anywhere but
 * on the platform line's platform, so the line is what makes an
 * acl= legal at all.
 */
static int
fx_attr_acl(const struct zr_fixture *fx, struct fx_entry *e, const char *v,
    size_t len, char *err, size_t errlen, int line)
{
	size_t n;

	if (fx->zf_platform == NULL) {
		return (fx_errf(err, errlen, line, "acl= wants a \"platform "
		    "freebsd\" line: an ACL is built nowhere else"));
	}
	if (len == 0)
		return (fx_errf(err, errlen, line, "acl= wants ACL text"));
	switch (fx_decode(v, len, &e->fe_acl, &n)) {
	case -1:
		return (fx_errf(err, errlen, line,
		    "bad escape in the ACL text \"%.*s\"", (int)len, v));
	case -2:
		return (fx_errf(err, errlen, line, "out of memory"));
	default:
		break;
	}
	if (strlen(e->fe_acl) != n) {
		return (fx_errf(err, errlen, line,
		    "the ACL text holds a NUL byte"));
	}
	return (0);
}

/*
 * The trailing attributes, which come in the order mode= uid= gid=
 * flags= xattr= acl= and at most once each, xattr= excepted. Walking
 * one cursor through that order is what enforces both rules at once.
 */
static int
fx_parse_attrs(const struct zr_fixture *fx, struct fx_entry *e,
    const struct fx_line *fl, int first, char *err, size_t errlen, int line)
{
	const char *f, *v;
	size_t flen, vlen;
	int i, k, slot, kind;

	slot = 0;
	for (i = first; i < fl->fl_n; i++) {
		f = fl->fl_f[i];
		flen = fl->fl_len[i];
		kind = -1;
		for (k = slot; k < FX_A_N; k++) {
			if (flen >= fx_attrlen[k] &&
			    memcmp(f, fx_attrname[k], fx_attrlen[k]) == 0) {
				kind = k;
				break;
			}
		}
		if (kind < 0) {
			return (fx_errf(err, errlen, line, "\"%.*s\" is not "
			    "an attribute here; the order is mode= uid= gid= "
			    "flags= xattr= acl=, each at most once but "
			    "xattr=", (int)flen, f));
		}
		slot = kind == FX_A_XATTR ? kind : kind + 1;
		v = f + fx_attrlen[kind];
		vlen = flen - fx_attrlen[kind];
		switch (kind) {
		case FX_A_FLAGS:
			if (fx_attr_flags(e, v, vlen, err, errlen, line) != 0)
				return (-1);
			break;
		case FX_A_XATTR:
			if (fx_attr_xattr(fx, e, v, vlen, err, errlen,
			    line) != 0)
				return (-1);
			break;
		case FX_A_ACL:
			if (fx_attr_acl(fx, e, v, vlen, err, errlen,
			    line) != 0)
				return (-1);
			break;
		default:
			if (fx_attr_value(e, kind, v, vlen) != 0) {
				return (fx_errf(err, errlen, line,
				    "%s wants %s", fx_attrname[kind],
				    kind == FX_A_MODE ?
				    "one to four octal digits" :
				    "a decimal number under 2^32"));
			}
			break;
		}
	}
	return (0);
}

/*
 * One entry line of the tree that is open. Every check the format
 * names happens here, in the order a reader would apply it.
 */
static int
fx_parse_entry(struct zr_fixture *fx, struct fx_tree *t,
    const struct fx_line *fl, char *err, size_t errlen, int line)
{
	struct fx_entry *e;
	char *p = NULL;
	char *arg = NULL;
	size_t plen, arglen, i;
	long slash, owner;
	int type, need;

	if (fl->fl_n < 2) {
		return (fx_errf(err, errlen, line,
		    "an entry is PATH TYPE [ARG], and the type is missing"));
	}
	switch (fx_decode(fl->fl_f[0], fl->fl_len[0], &p, &plen)) {
	case -1:
		return (fx_errf(err, errlen, line, "bad escape in the name "
		    "\"%.*s\": an escape is a backslash and three octal "
		    "digits", (int)fl->fl_len[0], fl->fl_f[0]));
	case -2:
		return (fx_errf(err, errlen, line, "out of memory"));
	default:
		break;
	}
	if (!fx_path_ok(p, plen)) {
		free(p);
		return (fx_errf(err, errlen, line, "\"%.*s\" is not an "
		    "absolute path without a trailing slash, \".\", \"..\" "
		    "or an empty component; the root is never listed",
		    (int)fl->fl_len[0], fl->fl_f[0]));
	}
	if (fx_find(t, p, plen) >= 0) {
		free(p);
		return (fx_errf(err, errlen, line,
		    "\"%.*s\" is already a name of this tree",
		    (int)fl->fl_len[0], fl->fl_f[0]));
	}
	for (slash = (long)plen - 1; p[slash] != '/'; slash--)
		continue;
	if (slash > 0) {
		owner = fx_find(t, p, (size_t)slash);
		if (owner < 0 || t->ft_ents[owner].fe_type != FX_DIR) {
			free(p);
			return (fx_errf(err, errlen, line, "the parent of "
			    "\"%.*s\" is not an earlier dir of this tree",
			    (int)fl->fl_len[0], fl->fl_f[0]));
		}
	}
	type = -1;
	for (i = 0; i < 4; i++) {
		if (fx_is(fl, 1, fx_typename[i]))
			type = (int)i;
	}
	if (type < 0) {
		free(p);
		return (fx_errf(err, errlen, line, "\"%.*s\" is not a type; "
		    "the types are file, link, dir and symlink",
		    (int)fl->fl_len[1], fl->fl_f[1]));
	}
	need = type == FX_DIR ? 2 : 3;
	if (fl->fl_n < need) {
		free(p);
		return (fx_errf(err, errlen, line, "%s takes an argument",
		    fx_typename[type]));
	}
	e = fx_add_entry(t);
	if (e == NULL) {
		free(p);
		return (fx_errf(err, errlen, line, "out of memory"));
	}
	e->fe_path = p;
	e->fe_pathlen = plen;
	e->fe_type = type;
	if (fx_parse_attrs(fx, e, fl, need, err, errlen, line) != 0)
		return (-1);

	if (type == FX_FILE) {
		for (i = 0; i < fl->fl_len[2]; i++) {
			if (fl->fl_f[2][i] < 0x21 || fl->fl_f[2][i] > 0x7e) {
				return (fx_errf(err, errlen, line, "a token "
				    "is printable ASCII with no whitespace"));
			}
		}
		e->fe_token = fx_intern(&fx->zf_tok, fl->fl_f[2],
		    fl->fl_len[2]);
		if (e->fe_token == FX_NOTOKEN)
			return (fx_errf(err, errlen, line, "out of memory"));
		return (0);
	}
	if (type == FX_DIR)
		return (0);

	switch (fx_decode(fl->fl_f[2], fl->fl_len[2], &arg, &arglen)) {
	case -1:
		return (fx_errf(err, errlen, line, "bad escape in the target "
		    "\"%.*s\"", (int)fl->fl_len[2], fl->fl_f[2]));
	case -2:
		return (fx_errf(err, errlen, line, "out of memory"));
	default:
		break;
	}
	e->fe_arg = arg;
	e->fe_arglen = arglen;
	if (type == FX_SYMLINK) {
		if (arglen == 0 || strlen(arg) != arglen) {
			return (fx_errf(err, errlen, line, "a symlink target "
			    "is not empty and holds no NUL"));
		}
		e->fe_token = fx_intern(&fx->zf_tok, arg, arglen);
		if (e->fe_token == FX_NOTOKEN)
			return (fx_errf(err, errlen, line, "out of memory"));
		return (0);
	}
	owner = fx_find(t, arg, arglen);
	if (owner < 0 || t->ft_ents[owner].fe_type != FX_FILE) {
		return (fx_errf(err, errlen, line, "the link target \"%.*s\" "
		    "is not an earlier file of this tree",
		    (int)fl->fl_len[2], fl->fl_f[2]));
	}
	e->fe_pool = (uint32_t)owner;
	return (0);
}

/*
 * What an absent attribute means: the mode the builder's mkdir(2)
 * and open(2) land on under this process's umask, and this process's
 * own owner and group, which are what lchown(2) is never asked to
 * change. Reading the umask means setting it, which is why this is
 * done once and why the fixture reader is single threaded.
 */
static void
fx_defaults(struct fx_deflt *dv)
{
	mode_t m;

	m = umask(0);
	(void) umask(m);
	dv->fd_filemode = 0644u & ~(uint32_t)m;
	dv->fd_dirmode = 0755u & ~(uint32_t)m;
	dv->fd_uid = (uint32_t)getuid();
	dv->fd_gid = (uint32_t)getgid();
}

static int
fx_buf_raw(struct fx_buf *b, const void *p, size_t n)
{
	char *nb;
	size_t cap;

	if (n == 0)
		return (0);
	if (b->fb_n + n > b->fb_cap) {
		cap = b->fb_cap == 0 ? FX_DESC_MIN : b->fb_cap;
		while (cap < b->fb_n + n)
			cap *= 2;
		nb = realloc(b->fb_p, cap);
		if (nb == NULL)
			return (-1);
		b->fb_p = nb;
		b->fb_cap = cap;
	}
	memcpy(b->fb_p + b->fb_n, p, n);
	b->fb_n += n;
	return (0);
}

/* One number, behind its tag, so no two fields can run together. */
static int
fx_buf_num(struct fx_buf *b, char tag, uint64_t v)
{
	char s[32];
	int n;

	n = snprintf(s, sizeof (s), "%c%llu;", tag, (unsigned long long)v);
	if (n < 0 || (size_t)n >= sizeof (s))
		return (-1);
	return (fx_buf_raw(b, s, (size_t)n));
}

/* One run of bytes, behind its tag and its length. */
static int
fx_buf_blob(struct fx_buf *b, char tag, const void *p, size_t n)
{
	char hdr[32];
	int k;

	k = snprintf(hdr, sizeof (hdr), "%c%llu:", tag, (unsigned long long)n);
	if (k < 0 || (size_t)k >= sizeof (hdr))
		return (-1);
	if (fx_buf_raw(b, hdr, (size_t)k) != 0)
		return (-1);
	return (fx_buf_raw(b, p, n));
}

/*
 * One more extended attribute of the pool, in name order. A name
 * this pool already carries is replaced, because the later name of a
 * pool set it later and one file has one value for one name.
 */
static int
fx_eff_xattr(struct fx_eff *ef, const struct fx_xattr *x)
{
	const struct fx_xattr **nx;
	uint32_t i, cap;
	int cmp;

	for (i = 0; i < ef->ef_nx; i++) {
		cmp = strcmp(ef->ef_x[i]->fa_name, x->fa_name);
		if (cmp == 0) {
			ef->ef_x[i] = x;
			return (0);
		}
		if (cmp > 0)
			break;
	}
	if (ef->ef_nx == ef->ef_cap) {
		cap = ef->ef_cap == 0 ? FX_XA_MIN : ef->ef_cap * 2;
		nx = realloc(ef->ef_x, (size_t)cap *
		    sizeof (const struct fx_xattr *));
		if (nx == NULL)
			return (-1);
		ef->ef_x = nx;
		ef->ef_cap = cap;
	}
	memmove(&ef->ef_x[i + 1], &ef->ef_x[i],
	    (size_t)(ef->ef_nx - i) * sizeof (const struct fx_xattr *));
	ef->ef_x[i] = x;
	ef->ef_nx++;
	return (0);
}

/*
 * Everything the content oracle would compare about one pool, in one
 * byte string: the type, the bytes, and the attributes every name of
 * the pool folded into it, an absent one resolved to what the
 * builder would leave behind. Every field is tagged and every run of
 * bytes carries its length, so two descriptors are equal exactly
 * when the pools they stand for are.
 */
static int
fx_pool_desc(const struct zr_fixture *fx, const struct fx_tree *t,
    uint32_t owner, const struct fx_deflt *dv, struct fx_buf *out)
{
	struct fx_eff ef;
	const struct fx_entry *e, *o;
	uint32_t i, k;
	int rc = -1;

	memset(&ef, 0, sizeof (ef));
	o = &t->ft_ents[owner];
	for (i = 0; i < t->ft_n; i++) {
		e = &t->ft_ents[i];
		if (e->fe_pool != owner)
			continue;
		if (e->fe_has_mode) {
			ef.ef_mode = e->fe_mode;
			ef.ef_has_mode = 1;
		}
		if (e->fe_has_uid) {
			ef.ef_uid = e->fe_uid;
			ef.ef_has_uid = 1;
		}
		if (e->fe_has_gid) {
			ef.ef_gid = e->fe_gid;
			ef.ef_has_gid = 1;
		}
		if (e->fe_has_flags)
			ef.ef_flags = e->fe_flags;
		if (e->fe_acl != NULL)
			ef.ef_acl = e->fe_acl;
		for (k = 0; k < e->fe_nxattr; k++) {
			if (fx_eff_xattr(&ef, &e->fe_xattr[k]) != 0)
				goto out;
		}
	}
	if (!ef.ef_has_mode) {
		ef.ef_mode = o->fe_type == FX_DIR ? dv->fd_dirmode :
		    dv->fd_filemode;
	}
	if (o->fe_type == FX_SYMLINK)
		ef.ef_mode = 0;		/* nothing here honours one */
	if (!ef.ef_has_uid)
		ef.ef_uid = dv->fd_uid;
	if (!ef.ef_has_gid)
		ef.ef_gid = dv->fd_gid;

	if (fx_buf_num(out, 't', (uint64_t)o->fe_type) != 0)
		goto out;
	if (o->fe_type == FX_FILE) {
		if (fx_buf_blob(out, 'c', fx->zf_tok.ft_p[o->fe_token],
		    fx->zf_tok.ft_len[o->fe_token]) != 0)
			goto out;
	} else if (o->fe_type == FX_SYMLINK) {
		if (fx_buf_blob(out, 'c', o->fe_arg, o->fe_arglen) != 0)
			goto out;
	} else if (fx_buf_blob(out, 'c', "", 0) != 0) {
		goto out;
	}
	if (fx_buf_num(out, 'm', ef.ef_mode) != 0 ||
	    fx_buf_num(out, 'u', ef.ef_uid) != 0 ||
	    fx_buf_num(out, 'g', ef.ef_gid) != 0 ||
	    fx_buf_num(out, 'f', ef.ef_flags) != 0 ||
	    fx_buf_num(out, 'x', ef.ef_nx) != 0)
		goto out;
	for (i = 0; i < ef.ef_nx; i++) {
		if (fx_buf_blob(out, 'n', ef.ef_x[i]->fa_name,
		    strlen(ef.ef_x[i]->fa_name)) != 0)
			goto out;
		if (fx_buf_blob(out, 'v', ef.ef_x[i]->fa_val,
		    ef.ef_x[i]->fa_len) != 0)
			goto out;
	}
	if (ef.ef_acl == NULL)
		rc = fx_buf_num(out, 'a', 0);
	else
		rc = fx_buf_blob(out, 'A', ef.ef_acl, strlen(ef.ef_acl));
out:
	free(ef.ef_x);
	return (rc);
}

/*
 * One content handle per pool of all three trees: the index of its
 * descriptor in the class table, so equal handles mean pools a walk
 * of the built trees would call equal, and different handles mean
 * pools it would call different.
 */
static int
fx_assign_handles(struct zr_fixture *fx)
{
	struct fx_deflt dv;
	struct fx_buf b;
	struct fx_tree *t;
	uint32_t h, i;
	int k, rc = 0;

	fx_defaults(&dv);
	memset(&b, 0, sizeof (b));
	for (k = 0; rc == 0 && k < 3; k++) {
		t = &fx->zf_trees[k];
		for (i = 0; i < t->ft_n; i++) {
			if (t->ft_ents[i].fe_pool != i)
				continue;
			b.fb_n = 0;
			if (fx_pool_desc(fx, t, i, &dv, &b) != 0) {
				rc = -1;
				break;
			}
			h = fx_intern(&fx->zf_cls, b.fb_p, b.fb_n);
			if (h == FX_NOTOKEN) {
				rc = -1;
				break;
			}
			t->ft_ents[i].fe_content = h;
		}
	}
	free(b.fb_p);
	return (rc);
}

int
zr_fixture_load(const char *path, struct zr_fixture **out, char *err,
    size_t errlen)
{
	struct zr_fixture *fx;
	struct fx_line fl;
	char *buf;
	size_t flen, off, eol, next, i;
	int line, ntrees, cur, which, k, rc;

	if (path == NULL || out == NULL)
		return (fx_errf(err, errlen, 0, "no fixture to load"));
	buf = fx_slurp(path, &flen);
	if (buf == NULL)
		return (fx_errf(err, errlen, 0, "%s: %s", path,
		    strerror(errno)));
	fx = malloc(sizeof (struct zr_fixture));
	if (fx == NULL) {
		free(buf);
		return (fx_errf(err, errlen, 0, "out of memory"));
	}
	memset(fx, 0, sizeof (struct zr_fixture));

	line = 1;
	for (i = 0; i < flen; i++) {
		if (buf[i] == '\n')
			line++;
		else if (buf[i] == '\0')
			break;
	}
	if (i < flen) {
		rc = fx_errf(err, errlen, line, "a NUL byte; a fixture is "
		    "ASCII text and escapes every other byte");
		goto fail;
	}

	ntrees = 0;
	cur = -1;
	line = 0;
	off = 0;
	while (off < flen) {
		line++;
		eol = off;
		while (eol < flen && buf[eol] != '\n')
			eol++;
		next = eol < flen ? eol + 1 : flen;
		fx_split(buf + off, eol - off, &fl);
		off = next;
		if (fl.fl_n == 0 || fl.fl_f[0][0] == '#')
			continue;
		if (fl.fl_over) {
			rc = fx_errf(err, errlen, line, "more than %d fields",
			    FX_MAXFIELD);
			goto fail;
		}
		if (fx_is(&fl, 0, "platform")) {
			if (ntrees != 0) {
				rc = fx_errf(err, errlen, line, "platform "
				    "after tree %s; the line comes before "
				    "the first tree", fx_treename[0]);
				goto fail;
			}
			if (fx->zf_platform != NULL) {
				rc = fx_errf(err, errlen, line,
				    "platform again; it is given once");
				goto fail;
			}
			if (fl.fl_n != 2 || !fx_is(&fl, 1, "freebsd")) {
				rc = fx_errf(err, errlen, line, "a platform "
				    "line is \"platform freebsd\", the one "
				    "platform a fixture can demand");
				goto fail;
			}
			fx->zf_platform = "freebsd";
			fx->zf_platline = line;
			continue;
		}
		if (fx_is(&fl, 0, "tree")) {
			if (fl.fl_n != 2) {
				rc = fx_errf(err, errlen, line, "a tree line "
				    "is \"tree base\", \"tree from\" or "
				    "\"tree onto\"");
				goto fail;
			}
			which = -1;
			for (k = 0; k < 3; k++) {
				if (fx_is(&fl, 1, fx_treename[k]))
					which = k;
			}
			if (which < 0) {
				rc = fx_errf(err, errlen, line, "\"%.*s\" is "
				    "not a tree; they are base, from and onto",
				    (int)fl.fl_len[1], fl.fl_f[1]);
				goto fail;
			}
			if (ntrees == 3) {
				rc = fx_errf(err, errlen, line, "tree %s "
				    "again; each tree comes once",
				    fx_treename[which]);
				goto fail;
			}
			if (which != ntrees) {
				rc = fx_errf(err, errlen, line, "expected "
				    "tree %s here, not tree %s; the order is "
				    "base, from, onto", fx_treename[ntrees],
				    fx_treename[which]);
				goto fail;
			}
			cur = which;
			ntrees++;
			continue;
		}
		if (fx_is(&fl, 0, "expect")) {
			if (fl.fl_n != 1) {
				rc = fx_errf(err, errlen, line,
				    "expect stands alone on its line");
				goto fail;
			}
			if (ntrees != 3) {
				rc = fx_errf(err, errlen, line, "expect "
				    "before all three trees are given");
				goto fail;
			}
			fx->zf_expect = malloc(flen - next + 1);
			if (fx->zf_expect == NULL) {
				rc = fx_errf(err, errlen, line,
				    "out of memory");
				goto fail;
			}
			memcpy(fx->zf_expect, buf + next, flen - next);
			fx->zf_expect[flen - next] = '\0';
			off = flen;
			break;
		}
		if (cur < 0) {
			rc = fx_errf(err, errlen, line,
			    "an entry before the first tree line");
			goto fail;
		}
		if (fx_parse_entry(fx, &fx->zf_trees[cur], &fl, err, errlen,
		    line) != 0) {
			rc = -1;
			goto fail;
		}
	}
	if (ntrees != 3) {
		rc = fx_errf(err, errlen, line, "tree %s is missing",
		    fx_treename[ntrees]);
		goto fail;
	}
	if (fx_assign_handles(fx) != 0) {
		rc = fx_errf(err, errlen, line, "out of memory");
		goto fail;
	}
	free(buf);
	*out = fx;
	return (0);
fail:
	free(buf);
	zr_fixture_free(fx);
	return (rc);
}

void
zr_fixture_free(struct zr_fixture *fx)
{
	struct fx_entry *e;
	uint32_t i, j;
	int k;

	if (fx == NULL)
		return;
	for (k = 0; k < 3; k++) {
		for (i = 0; i < fx->zf_trees[k].ft_n; i++) {
			e = &fx->zf_trees[k].ft_ents[i];
			for (j = 0; j < e->fe_nxattr; j++) {
				free(e->fe_xattr[j].fa_name);
				free(e->fe_xattr[j].fa_val);
			}
			free(e->fe_xattr);
			free(e->fe_acl);
			free(e->fe_path);
			free(e->fe_arg);
		}
		free(fx->zf_trees[k].ft_ents);
	}
	fx_tab_free(&fx->zf_tok);
	fx_tab_free(&fx->zf_cls);
	free(fx->zf_expect);
	free(fx);
}

const char *
zr_fixture_expect(const struct zr_fixture *fx)
{
	if (fx == NULL)
		return (NULL);
	return (fx->zf_expect);
}

const char *
zr_fixture_platform(const struct zr_fixture *fx)
{
	if (fx == NULL)
		return (NULL);
	return (fx->zf_platform);
}

static char *
fx_join(const char *root, size_t rootlen, const char *rel, size_t rellen)
{
	char *s;

	s = malloc(rootlen + rellen + 1);
	if (s == NULL) {
		errno = ENOMEM;
		return (NULL);
	}
	memcpy(s, root, rootlen);
	memcpy(s + rootlen, rel, rellen);
	s[rootlen + rellen] = '\0';
	return (s);
}

static int
fx_dir_empty(const char *dir)
{
	DIR *d;
	struct dirent *de;
	int n, saved;

	d = opendir(dir);
	if (d == NULL)
		return (-1);
	n = 0;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		n = 1;
		break;
	}
	saved = errno;
	(void) closedir(d);
	errno = saved;
	if (n != 0) {
		errno = ENOTEMPTY;
		return (-1);
	}
	return (0);
}

static int
fx_write_file(const char *full, const char *tok, size_t toklen)
{
	char *buf;
	ssize_t w;
	size_t off, want;
	int fd, saved;

	want = toklen + 1;
	buf = malloc(want);
	if (buf == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	memcpy(buf, tok, toklen);
	buf[toklen] = '\n';
	fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		saved = errno;
		free(buf);
		errno = saved;
		return (-1);
	}
	for (off = 0; off < want; off += (size_t)w) {
		w = write(fd, buf + off, want - off);
		if (w <= 0) {
			saved = w < 0 ? errno : EIO;
			(void) close(fd);
			free(buf);
			errno = saved;
			return (-1);
		}
	}
	free(buf);
	return (close(fd));
}

/*
 * Everything but the file flags, in the order apply.c uses and for
 * the same reasons: chown before chmod, because chown may drop the
 * set-id bits a mode just asked for; the extended attributes and the
 * ACL after the mode, because setting a mode rewrites an NFSv4 ACL.
 * A symlink's mode is not honoured anywhere this tool runs, so mode=
 * passes it by; lchown and the l-forms of the rest do reach it.
 */
static int
fx_attrs(const char *full, const struct fx_entry *e)
{
	uint32_t i;
	uid_t u;
	gid_t g;

	if (e->fe_has_uid || e->fe_has_gid) {
		u = e->fe_has_uid ? (uid_t)e->fe_uid : (uid_t)-1;
		g = e->fe_has_gid ? (gid_t)e->fe_gid : (gid_t)-1;
		if (lchown(full, u, g) != 0)
			return (-1);
	}
	if (e->fe_has_mode && e->fe_type != FX_SYMLINK) {
		if (chmod(full, (mode_t)e->fe_mode) != 0)
			return (-1);
	}
	for (i = 0; i < e->fe_nxattr; i++) {
		if (fx_setx(full, e->fe_xattr[i].fa_name,
		    e->fe_xattr[i].fa_val, e->fe_xattr[i].fa_len) != 0)
			return (-1);
	}
	if (e->fe_acl != NULL && fx_setacl(full, e->fe_acl) != 0)
		return (-1);
	return (0);
}

/* Is this host the platform a fixture's platform line demands? */
static int
fx_platform_ok(const struct zr_fixture *fx)
{
#ifdef FX_PLATFORM
	return (fx->zf_platform == NULL ||
	    strcmp(fx->zf_platform, FX_PLATFORM) == 0);
#else
	return (fx->zf_platform == NULL);
#endif
}

/*
 * The file flags of every object, once each and last of all: an
 * immutable file cannot be given an attribute afterwards, and an
 * immutable directory cannot be given a child, so the flags of a
 * directory wait for its children. Entries run backwards here, which
 * is children before parents, and a pool is flagged from the last of
 * its names that said flags= -- the same later-wins rule the other
 * attributes follow, which a backwards pass would otherwise invert.
 */
static int
fx_flags_pass(const struct fx_tree *t, const char *rootdir, size_t rootlen,
    const char **atp)
{
	const struct fx_entry *e;
	uint32_t *last;
	char *full;
	uint32_t i;
	int rc = 0, saved;

	last = malloc((size_t)(t->ft_n == 0 ? 1 : t->ft_n) *
	    sizeof (uint32_t));
	if (last == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	for (i = 0; i < t->ft_n; i++)
		last[i] = FX_NOTOKEN;
	for (i = 0; i < t->ft_n; i++) {
		if (t->ft_ents[i].fe_has_flags)
			last[t->ft_ents[i].fe_pool] = i;
	}
	for (i = t->ft_n; rc == 0 && i > 0; i--) {
		e = &t->ft_ents[i - 1];
		if (!e->fe_has_flags || last[e->fe_pool] != i - 1)
			continue;
		full = fx_join(rootdir, rootlen, e->fe_path, e->fe_pathlen);
		if (full == NULL) {
			rc = -1;
			break;
		}
		rc = fx_setflags(full, e->fe_flags);
		saved = errno;
		free(full);
		errno = saved;
		if (rc != 0)
			*atp = e->fe_path;
	}
	free(last);
	return (rc);
}

int
zr_fixture_build_err(const struct zr_fixture *fx, enum zr_fixture_tree which,
    const char *rootdir, char *err, size_t errlen)
{
	const struct fx_tree *t;
	const struct fx_entry *e;
	const char *at = NULL;
	char *full, *tgt;
	size_t rootlen;
	uint32_t i;
	int rc, saved;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (fx == NULL || rootdir == NULL || (unsigned)which > ZR_FX_ONTO) {
		errno = EINVAL;
		return (-1);
	}
	if (!fx_platform_ok(fx)) {
		(void) fx_errf(err, errlen, fx->zf_platline, "this fixture "
		    "says \"platform %s\", and builds on no other platform",
		    fx->zf_platform);
		errno = ENOTSUP;
		return (-1);
	}
	if (fx_dir_empty(rootdir) != 0)
		goto failed;
	t = &fx->zf_trees[which];
	rootlen = strlen(rootdir);
	for (i = 0; i < t->ft_n; i++) {
		e = &t->ft_ents[i];
		full = fx_join(rootdir, rootlen, e->fe_path, e->fe_pathlen);
		if (full == NULL)
			goto failed;
		switch (e->fe_type) {
		case FX_DIR:
			rc = mkdir(full, 0755);
			break;
		case FX_FILE:
			rc = fx_write_file(full,
			    fx->zf_tok.ft_p[e->fe_token],
			    fx->zf_tok.ft_len[e->fe_token]);
			break;
		case FX_SYMLINK:
			rc = symlink(e->fe_arg, full);
			break;
		default:
			tgt = fx_join(rootdir, rootlen,
			    t->ft_ents[e->fe_pool].fe_path,
			    t->ft_ents[e->fe_pool].fe_pathlen);
			if (tgt == NULL) {
				free(full);
				goto failed;
			}
			rc = link(tgt, full);
			saved = errno;
			free(tgt);
			errno = saved;
			break;
		}
		if (rc == 0)
			rc = fx_attrs(full, e);
		saved = errno;
		free(full);
		if (rc != 0) {
			errno = saved;
			at = e->fe_path;
			goto failed;
		}
	}
	if (fx_flags_pass(t, rootdir, rootlen, &at) != 0)
		goto failed;
	return (0);
failed:
	if (err != NULL && errlen > 0 && err[0] == '\0') {
		(void) snprintf(err, errlen, "%s%s: %s", rootdir,
		    at == NULL ? "" : at, strerror(errno));
	}
	return (-1);
}

int
zr_fixture_build(const struct zr_fixture *fx, enum zr_fixture_tree which,
    const char *rootdir)
{
	return (zr_fixture_build_err(fx, which, rootdir, NULL, 0));
}

static zr_type_t
fx_zrtype(int type)
{
	if (type == FX_DIR)
		return (ZR_T_DIR);
	if (type == FX_SYMLINK)
		return (ZR_T_SYMLINK);
	return (ZR_T_FILE);
}

int
zr_fixture_to_tree(const struct zr_fixture *fx, enum zr_fixture_tree which,
    struct zr_names *ns, struct zr_tree *out)
{
	const struct fx_tree *t;
	const struct fx_entry *e;
	uint32_t *nlink;
	uint32_t i;
	zr_name_t nm;
	zr_pool_t p;

	if (fx == NULL || ns == NULL || out == NULL ||
	    (unsigned)which > ZR_FX_ONTO)
		return (-1);
	t = &fx->zf_trees[which];
	nlink = calloc(t->ft_n == 0 ? 1 : t->ft_n, sizeof (uint32_t));
	if (nlink == NULL)
		return (-1);
	for (i = 0; i < t->ft_n; i++)
		nlink[t->ft_ents[i].fe_pool]++;
	if (zr_tree_init(out, ns) != 0) {
		free(nlink);
		return (-1);
	}
	for (i = 0; i < t->ft_n; i++) {
		e = &t->ft_ents[i];
		nm = zr_names_intern(ns, e->fe_path, e->fe_pathlen);
		if (nm == ZR_NAME_NONE)
			goto fail;
		p = zr_tree_add(out, nm, (uint64_t)e->fe_pool + 1,
		    fx_zrtype(e->fe_type), nlink[e->fe_pool]);
		if (p == ZR_POOL_NONE)
			goto fail;
		out->zt_pools[p].zp_content =
		    t->ft_ents[e->fe_pool].fe_content;
	}
	free(nlink);
	if (zr_tree_seal(out) != 0) {
		zr_tree_fini(out);
		return (-1);
	}
	return (0);
fail:
	free(nlink);
	zr_tree_fini(out);
	return (-1);
}
