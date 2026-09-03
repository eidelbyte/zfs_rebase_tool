/*
 * fixture: the .zrt parser and the two builders that make a fixture
 * real. The parser turns one text file into three lists of entries
 * and an expect block; zr_fixture_build writes a tree with ordinary
 * POSIX calls; zr_fixture_to_tree builds the same tree as pools in
 * memory. tests/fixtures/FORMAT.md is the format.
 */

#define	_XOPEN_SOURCE	700
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

#define	FX_MAXFIELD	8		/* PATH TYPE ARG mode uid gid, spare */
#define	FX_NOTOKEN	0xffffffffu
#define	FX_SLURP_MIN	8192
#define	FX_SLURP_MAX	(64u * 1024u * 1024u)
#define	FX_TOK_MIN	16
#define	FX_ENTS_MIN	16

/*
 * One entry line. fe_arg holds a link's target path or a symlink's
 * target string, both decoded, and is NULL otherwise. fe_token is the
 * token table index of a file's token or of a symlink's target, and
 * FX_NOTOKEN for a directory or a link, whose content comes from the
 * entry that owns their pool. fe_pool is the index of that entry: a
 * link points at its target, everything else at itself.
 */
struct fx_entry {
	char		*fe_path;
	size_t		fe_pathlen;
	char		*fe_arg;
	size_t		fe_arglen;
	uint32_t	fe_token;
	uint32_t	fe_pool;
	uint32_t	fe_mode;
	uint32_t	fe_uid;
	uint32_t	fe_gid;
	int		fe_type;
	int		fe_has_mode;
	int		fe_has_uid;
	int		fe_has_gid;
};

struct fx_tree {
	struct fx_entry	*ft_ents;
	uint32_t	ft_n;
	uint32_t	ft_cap;
};

struct zr_fixture {
	struct fx_tree	zf_trees[3];
	char		**zf_tok;	/* the token table, interned */
	size_t		*zf_toklen;
	uint32_t	zf_ntok;
	uint32_t	zf_tokcap;
	char		*zf_expect;	/* NULL when there is no block */
};

/* One line split on runs of spaces and tabs. */
struct fx_line {
	const char	*fl_f[FX_MAXFIELD];
	size_t		fl_len[FX_MAXFIELD];
	int		fl_n;
	int		fl_over;	/* the line held more than fl_n */
};

static const char *const fx_treename[3] = { "base", "from", "onto" };
static const char *const fx_typename[4] = { "file", "link", "dir", "symlink" };
static const char *const fx_attrname[3] = { "mode=", "uid=", "gid=" };
static const size_t fx_attrlen[3] = { 5, 4, 4 };

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
 * never grows, so a buffer the size of the field always fits.
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
 * Intern one token. The table is small and scanned linearly on
 * purpose: a fixture holds a handful of distinct contents, and the
 * index it hands back is the content handle itself.
 */
static uint32_t
fx_intern(struct zr_fixture *fx, const char *s, size_t len)
{
	char **tv;
	size_t *lv;
	char *copy;
	uint32_t i, cap;

	for (i = 0; i < fx->zf_ntok; i++) {
		if (fx->zf_toklen[i] == len &&
		    memcmp(fx->zf_tok[i], s, len) == 0)
			return (i);
	}
	if (fx->zf_ntok == fx->zf_tokcap) {
		cap = fx->zf_tokcap == 0 ? FX_TOK_MIN : fx->zf_tokcap * 2;
		if (cap >= ZR_FX_DIR_CONTENT)
			return (FX_NOTOKEN);
		tv = realloc(fx->zf_tok, (size_t)cap * sizeof (char *));
		if (tv == NULL)
			return (FX_NOTOKEN);
		fx->zf_tok = tv;
		lv = realloc(fx->zf_toklen, (size_t)cap * sizeof (size_t));
		if (lv == NULL)
			return (FX_NOTOKEN);
		fx->zf_toklen = lv;
		fx->zf_tokcap = cap;
	}
	copy = malloc(len + 1);
	if (copy == NULL)
		return (FX_NOTOKEN);
	memcpy(copy, s, len);
	copy[len] = '\0';
	fx->zf_tok[fx->zf_ntok] = copy;
	fx->zf_toklen[fx->zf_ntok] = len;
	return (fx->zf_ntok++);
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
	if (which == 0) {
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
	if (which == 1) {
		e->fe_uid = (uint32_t)n;
		e->fe_has_uid = 1;
	} else {
		e->fe_gid = (uint32_t)n;
		e->fe_has_gid = 1;
	}
	return (0);
}

/*
 * The trailing attributes, which come at most once each and in the
 * order mode= uid= gid=. Walking one cursor through that order is
 * what enforces both rules at once.
 */
static int
fx_parse_attrs(struct fx_entry *e, const struct fx_line *fl, int first,
    char *err, size_t errlen, int line)
{
	const char *f;
	size_t flen;
	int i, k, slot, matched;

	slot = 0;
	for (i = first; i < fl->fl_n; i++) {
		f = fl->fl_f[i];
		flen = fl->fl_len[i];
		matched = 0;
		while (slot < 3) {
			k = slot++;
			if (flen <= fx_attrlen[k] ||
			    memcmp(f, fx_attrname[k], fx_attrlen[k]) != 0)
				continue;
			if (fx_attr_value(e, k, f + fx_attrlen[k],
			    flen - fx_attrlen[k]) != 0) {
				return (fx_errf(err, errlen, line,
				    "%s wants %s", fx_attrname[k], k == 0 ?
				    "one to four octal digits" :
				    "a decimal number under 2^32"));
			}
			matched = 1;
			break;
		}
		if (!matched) {
			return (fx_errf(err, errlen, line, "\"%.*s\" is not "
			    "an attribute here; the order is mode= uid= gid=,"
			    " each at most once", (int)flen, f));
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
	if (fx_parse_attrs(e, fl, need, err, errlen, line) != 0)
		return (-1);

	if (type == FX_FILE) {
		for (i = 0; i < fl->fl_len[2]; i++) {
			if (fl->fl_f[2][i] < 0x21 || fl->fl_f[2][i] > 0x7e) {
				return (fx_errf(err, errlen, line, "a token "
				    "is printable ASCII with no whitespace"));
			}
		}
		e->fe_token = fx_intern(fx, fl->fl_f[2], fl->fl_len[2]);
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
		e->fe_token = fx_intern(fx, arg, arglen);
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
	uint32_t i, k;

	if (fx == NULL)
		return;
	for (k = 0; k < 3; k++) {
		for (i = 0; i < fx->zf_trees[k].ft_n; i++) {
			free(fx->zf_trees[k].ft_ents[i].fe_path);
			free(fx->zf_trees[k].ft_ents[i].fe_arg);
		}
		free(fx->zf_trees[k].ft_ents);
	}
	for (i = 0; i < fx->zf_ntok; i++)
		free(fx->zf_tok[i]);
	free(fx->zf_tok);
	free(fx->zf_toklen);
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
 * chown before chmod, because chown may drop the set-id bits a mode
 * just asked for. A symlink's mode is not honoured anywhere this tool
 * runs, so mode= passes it by; lchown does reach it.
 */
static int
fx_attrs(const char *full, const struct fx_entry *e)
{
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
	return (0);
}

int
zr_fixture_build(const struct zr_fixture *fx, enum zr_fixture_tree which,
    const char *rootdir)
{
	const struct fx_tree *t;
	const struct fx_entry *e;
	char *full, *tgt;
	size_t rootlen;
	uint32_t i;
	int rc, saved;

	if (fx == NULL || rootdir == NULL || (unsigned)which > ZR_FX_ONTO) {
		errno = EINVAL;
		return (-1);
	}
	if (fx_dir_empty(rootdir) != 0)
		return (-1);
	t = &fx->zf_trees[which];
	rootlen = strlen(rootdir);
	for (i = 0; i < t->ft_n; i++) {
		e = &t->ft_ents[i];
		full = fx_join(rootdir, rootlen, e->fe_path, e->fe_pathlen);
		if (full == NULL)
			return (-1);
		switch (e->fe_type) {
		case FX_DIR:
			rc = mkdir(full, 0755);
			break;
		case FX_FILE:
			rc = fx_write_file(full, fx->zf_tok[e->fe_token],
			    fx->zf_toklen[e->fe_token]);
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
				return (-1);
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
			return (-1);
		}
	}
	return (0);
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

static uint32_t
fx_handle(const struct fx_entry *e)
{
	if (e->fe_type == FX_DIR)
		return (ZR_FX_DIR_CONTENT);
	if (e->fe_type == FX_SYMLINK)
		return (e->fe_token | ZR_FX_SYMLINK_BIT);
	return (e->fe_token);
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
		    fx_handle(&t->ft_ents[e->fe_pool]);
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
