/*
 * diff: parse what "zfs diff -F -H" printed, and turn the silence in
 * it into work the content oracle never has to do.
 *
 * The format is lib/libzfs/libzfs_diff.c and nothing else.
 * stream_bytes() writes a byte as itself when it is greater than a
 * space, less than DEL and not a backslash, and otherwise writes a
 * backslash and exactly four octal digits, so space, tab, backslash,
 * DEL and every eighth-bit byte arrive escaped and a literal tab is
 * always a column separator. print_file() writes the change
 * character, the classify column when -F is on and the path;
 * print_link_change() writes the same with a trailing "(%+d)" and
 * the change character 'M'; print_rename() writes 'R', the classify
 * column, the old path and the new path, separated by a tab because
 * ZFS_DIFF_PARSEABLE sets di->scripted. print_cmn() writes the
 * dataset's mountpoint in front of every path. The timestamp column
 * is a fourth flag we do not pass.
 */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diff.h"
#include "name.h"
#include "walk.h"
#include "yellow.h"

#define	ZD_ESCAPE	'\\'
#define	ZD_SEP		'\t'
#define	ZD_LINE_MIN	128
#define	ZD_LINE_MAX	65536		/* a mistaken file, not a diff */
#define	ZD_ENT_MIN	16
#define	ZD_FIELDS	5		/* four are legal; the fifth traps */

#define	ZD_OK		0
#define	ZD_EBAD		(-1)
#define	ZD_ENOMEM	(-2)

/* One line of input, without its newline and always NUL-terminated. */
struct zd_line {
	char		*zl_buf;
	size_t		zl_cap;
	size_t		zl_len;
};

/* One tab-separated column of one line. */
struct zd_field {
	const char	*zf_p;
	size_t		zf_len;
};

/* One path the diff named, for the sorted sets the pruning searches. */
struct zd_ref {
	const char	*zr_p;
	size_t		zr_len;
};

static int
zd_fail(char *err, size_t errlen, unsigned long line, const char *msg)
{
	if (err != NULL && errlen > 0)
		(void) snprintf(err, errlen, "line %lu: %s", line, msg);
	return (-1);
}

/* Append one byte, growing the line buffer and keeping the NUL. */
static int
zd_line_put(struct zd_line *l, char c)
{
	char *nb;
	size_t cap;

	if (l->zl_len + 2 > l->zl_cap) {
		if (l->zl_cap >= ZD_LINE_MAX)
			return (-1);
		cap = l->zl_cap == 0 ? ZD_LINE_MIN : l->zl_cap * 2;
		nb = realloc(l->zl_buf, cap);
		if (nb == NULL)
			return (-1);
		l->zl_buf = nb;
		l->zl_cap = cap;
	}
	l->zl_buf[l->zl_len++] = c;
	l->zl_buf[l->zl_len] = '\0';
	return (0);
}

/*
 * Read one line without its newline. Returns 1 for a line, 0 at end
 * of input and -1 on a read error, a line past the cap, or no
 * memory. A file whose last line has no newline still yields it.
 */
static int
zd_getline(FILE *in, struct zd_line *l)
{
	int c;

	l->zl_len = 0;
	if (l->zl_cap > 0)
		l->zl_buf[0] = '\0';
	for (;;) {
		c = fgetc(in);
		if (c == EOF)
			break;
		if (c == '\n')
			return (1);
		if (zd_line_put(l, (char)c) != 0)
			return (-1);
	}
	if (ferror(in))
		return (-1);
	return (l->zl_len > 0 ? 1 : 0);
}

/*
 * Split a line on tabs. Returns the number of columns, or -1 when
 * there are more than maxf of them.
 */
static int
zd_split(const char *s, size_t len, struct zd_field *f, int maxf)
{
	size_t i, start;
	int n;

	n = 0;
	start = 0;
	for (i = 0; i <= len; i++) {
		if (i < len && s[i] != ZD_SEP)
			continue;
		if (n == maxf)
			return (-1);
		f[n].zf_p = s + start;
		f[n].zf_len = i - start;
		n++;
		start = i + 1;
	}
	return (n);
}

/*
 * Undo stream_bytes(). Decoding never grows, so the input length is
 * room enough. Returns ZD_OK with *outp allocated and *outlen set,
 * ZD_EBAD on an escape that is short, holds a digit outside 0-7 or
 * names a value above 0377, or ZD_ENOMEM.
 */
static int
zd_decode(const char *p, size_t len, unsigned char **outp, size_t *outlen)
{
	unsigned char *out;
	size_t i, n;
	int k, v;

	out = malloc(len + 1);
	if (out == NULL)
		return (ZD_ENOMEM);
	i = 0;
	n = 0;
	while (i < len) {
		if (p[i] != ZD_ESCAPE) {
			out[n++] = (unsigned char)p[i++];
			continue;
		}
		if (len - i < 5) {
			free(out);
			return (ZD_EBAD);
		}
		v = 0;
		for (k = 1; k <= 4; k++) {
			char c = p[i + (size_t)k];

			if (c < '0' || c > '7') {
				free(out);
				return (ZD_EBAD);
			}
			v = v * 8 + (c - '0');
		}
		if (v > 0377) {
			free(out);
			return (ZD_EBAD);
		}
		out[n++] = (unsigned char)v;
		i += 5;
	}
	out[n] = '\0';
	*outp = out;
	*outlen = n;
	return (ZD_OK);
}

/*
 * Take the mountpoint off a decoded path and leave the absolute
 * dataset-relative form. Runs of slashes collapse and a trailing
 * slash goes, which is what the root line and a dataset mounted at
 * "/" leave behind; the root becomes "/". Returns ZD_OK, ZD_EBAD
 * when the path is not under the mountpoint, or ZD_ENOMEM.
 */
static int
zd_strip(const unsigned char *p, size_t len, const char *mnt, size_t mntlen,
    unsigned char **outp, size_t *outlen)
{
	unsigned char *out;
	size_t i, n;

	if (len < mntlen || memcmp(p, mnt, mntlen) != 0)
		return (ZD_EBAD);
	p += mntlen;
	len -= mntlen;
	if (len != 0 && p[0] != '/')
		return (ZD_EBAD);
	out = malloc(len + 2);
	if (out == NULL)
		return (ZD_ENOMEM);
	n = 0;
	for (i = 0; i < len; i++) {
		if (p[i] == '/' && n > 0 && out[n - 1] == '/')
			continue;
		out[n++] = p[i];
	}
	while (n > 1 && out[n - 1] == '/')
		n--;
	if (n == 0)
		out[n++] = '/';
	out[n] = '\0';
	*outp = out;
	*outlen = n;
	return (ZD_OK);
}

/* Decode one path column and take the mountpoint off it. */
static int
zd_path(const struct zd_field *f, const char *mnt, size_t mntlen,
    unsigned char **outp, size_t *outlen, const char **why)
{
	unsigned char *raw;
	size_t rawlen;
	int rc;

	rc = zd_decode(f->zf_p, f->zf_len, &raw, &rawlen);
	if (rc != ZD_OK) {
		*why = rc == ZD_EBAD ? "bad escape in the path" :
		    "out of memory";
		return (-1);
	}
	rc = zd_strip(raw, rawlen, mnt, mntlen, outp, outlen);
	free(raw);
	if (rc != ZD_OK) {
		*why = rc == ZD_EBAD ? "path is not under the mountpoint" :
		    "out of memory";
		return (-1);
	}
	return (0);
}

/*
 * The classify column, which is get_what() of libzfs_diff.c: block
 * and character device, directory, door, fifo, symbolic link, event
 * port, socket, regular file, and the '?' it falls back to.
 */
static int
zd_type_ok(char c)
{
	return (c == 'B' || c == 'C' || c == '/' || c == '>' || c == '|' ||
	    c == '@' || c == 'P' || c == '=' || c == 'F' || c == '?');
}

/* The "(%+d)" print_link_change() writes. The sign is always there. */
static int
zd_delta(const char *p, size_t len, int32_t *out)
{
	long v;
	size_t i;
	int neg;

	if (len < 4 || p[0] != '(' || p[len - 1] != ')')
		return (-1);
	if (p[1] == '+')
		neg = 0;
	else if (p[1] == '-')
		neg = 1;
	else
		return (-1);
	v = 0;
	for (i = 2; i + 1 < len; i++) {
		if (p[i] < '0' || p[i] > '9')
			return (-1);
		v = v * 10 + (p[i] - '0');
		if (v > 2147483647L)
			return (-1);
	}
	*out = (int32_t)(neg ? -v : v);
	return (0);
}

static void
zd_entry_fini(struct zr_diff_entry *e)
{
	free(e->zd_path);
	free(e->zd_newpath);
	e->zd_path = NULL;
	e->zd_newpath = NULL;
}

/* Append one entry, doubling the array. The entry's paths move. */
static int
zd_push(struct zr_diff *d, uint32_t *cap, const struct zr_diff_entry *e)
{
	struct zr_diff_entry *ne;
	uint32_t nc;

	if (d->zd_n == *cap) {
		if (*cap >= (uint32_t)1 << 30)
			return (-1);
		nc = *cap == 0 ? ZD_ENT_MIN : *cap * 2;
		ne = realloc(d->zd_entries, (size_t)nc * sizeof (*ne));
		if (ne == NULL)
			return (-1);
		d->zd_entries = ne;
		*cap = nc;
	}
	d->zd_entries[d->zd_n++] = *e;
	return (0);
}

void
zr_diff_fini(struct zr_diff *d)
{
	uint32_t i;

	if (d == NULL)
		return;
	for (i = 0; i < d->zd_n; i++)
		zd_entry_fini(&d->zd_entries[i]);
	free(d->zd_entries);
	d->zd_entries = NULL;
	d->zd_n = 0;
}

int
zr_diff_parse(FILE *in, const char *mountpoint, struct zr_diff *out,
    char *err, size_t errlen)
{
	struct zd_line l;
	struct zd_field f[ZD_FIELDS];
	struct zr_diff_entry e;
	const char *why;
	unsigned long line;
	size_t mntlen;
	uint32_t cap;
	int nf, rc, want;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (out == NULL)
		return (-1);
	out->zd_entries = NULL;
	out->zd_n = 0;
	if (in == NULL || mountpoint == NULL)
		return (zd_fail(err, errlen, 0, "no input"));
	mntlen = strlen(mountpoint);
	while (mntlen > 0 && mountpoint[mntlen - 1] == '/')
		mntlen--;
	memset(&l, 0, sizeof (l));
	memset(&e, 0, sizeof (e));
	cap = 0;
	line = 0;
	why = NULL;
	for (;;) {
		rc = zd_getline(in, &l);
		if (rc < 0) {
			why = "cannot read the diff";
			goto fail;
		}
		if (rc == 0)
			break;
		line++;
		if (l.zl_len == 0)
			continue;
		nf = zd_split(l.zl_buf, l.zl_len, f, ZD_FIELDS);
		if (nf < 3 || nf > 4) {
			why = "wrong number of columns";
			goto fail;
		}
		memset(&e, 0, sizeof (e));
		if (f[0].zf_len != 1 || (f[0].zf_p[0] != 'M' &&
		    f[0].zf_p[0] != '-' && f[0].zf_p[0] != '+' &&
		    f[0].zf_p[0] != 'R')) {
			why = "unknown change column";
			goto fail;
		}
		e.zd_kind = f[0].zf_p[0];
		if (f[1].zf_len != 1 || !zd_type_ok(f[1].zf_p[0])) {
			why = "unknown classify column";
			goto fail;
		}
		e.zd_type = f[1].zf_p[0];
		want = e.zd_kind == 'R' ? 4 : 3;
		if (nf < want) {
			why = "a rename wants both paths";
			goto fail;
		}
		if (nf == 4 && e.zd_kind != 'R') {
			if (e.zd_kind != 'M') {
				why = "a link count belongs to an M line";
				goto fail;
			}
			if (zd_delta(f[3].zf_p, f[3].zf_len,
			    &e.zd_linkdelta) != 0) {
				why = "malformed link count";
				goto fail;
			}
		}
		if (zd_path(&f[2], mountpoint, mntlen, &e.zd_path,
		    &e.zd_pathlen, &why) != 0)
			goto fail;
		if (e.zd_kind == 'R' && zd_path(&f[3], mountpoint, mntlen,
		    &e.zd_newpath, &e.zd_newlen, &why) != 0)
			goto fail;
		if (zd_push(out, &cap, &e) != 0) {
			why = "out of memory";
			goto fail;
		}
		memset(&e, 0, sizeof (e));
	}
	free(l.zl_buf);
	return (0);
fail:
	zd_entry_fini(&e);
	zr_diff_fini(out);
	free(l.zl_buf);
	return (zd_fail(err, errlen, line, why));
}

/* Bytewise order over (pointer, length), the shorter first on a tie. */
static int
zd_ref_cmp(const void *a, const void *b)
{
	const struct zd_ref *x = a;
	const struct zd_ref *y = b;
	size_t n;
	int c;

	n = x->zr_len < y->zr_len ? x->zr_len : y->zr_len;
	c = n == 0 ? 0 : memcmp(x->zr_p, y->zr_p, n);
	if (c != 0)
		return (c);
	if (x->zr_len == y->zr_len)
		return (0);
	return (x->zr_len < y->zr_len ? -1 : 1);
}

static int
zd_has(const struct zd_ref *set, size_t n, const char *p, size_t len)
{
	struct zd_ref key;

	if (n == 0)
		return (0);
	key.zr_p = p;
	key.zr_len = len;
	return (bsearch(&key, set, n, sizeof (*set), zd_ref_cmp) != NULL);
}

/*
 * The two sorted sets the pruning searches: every path the diff
 * named at all, and the rename paths alone, which are the ones a
 * name can sit under. Both point into the diff's own bytes.
 */
static int
zd_sets(const struct zr_diff *d, struct zd_ref **allp, size_t *nallp,
    struct zd_ref **renp, size_t *nrenp)
{
	const struct zr_diff_entry *e;
	struct zd_ref *all, *ren;
	size_t nall, nren;
	uint32_t i;

	*allp = NULL;
	*nallp = 0;
	*renp = NULL;
	*nrenp = 0;
	if (d->zd_n == 0)
		return (0);
	all = malloc((size_t)d->zd_n * 2 * sizeof (*all));
	if (all == NULL)
		return (-1);
	ren = malloc((size_t)d->zd_n * 2 * sizeof (*ren));
	if (ren == NULL) {
		free(all);
		return (-1);
	}
	nall = 0;
	nren = 0;
	for (i = 0; i < d->zd_n; i++) {
		e = &d->zd_entries[i];
		if (e->zd_path == NULL)
			continue;
		all[nall].zr_p = (const char *)e->zd_path;
		all[nall].zr_len = e->zd_pathlen;
		nall++;
		if (e->zd_kind != 'R')
			continue;
		ren[nren++] = all[nall - 1];
		if (e->zd_newpath == NULL)
			continue;
		all[nall].zr_p = (const char *)e->zd_newpath;
		all[nall].zr_len = e->zd_newlen;
		nall++;
		ren[nren++] = all[nall - 1];
	}
	if (nall > 1)
		qsort(all, nall, sizeof (*all), zd_ref_cmp);
	if (nren > 1)
		qsort(ren, nren, sizeof (*ren), zd_ref_cmp);
	*allp = all;
	*nallp = nall;
	*renp = ren;
	*nrenp = nren;
	return (0);
}

/*
 * Is this name spoken for? Either the diff named it outright, or one
 * of its ancestors is a rename path, which moved it whatever else
 * happened. The ancestor walk climbs the name table, so it is a loop
 * over the depth of the name and never a recursion.
 */
static int
zd_spoken_for(const struct zr_names *ns, zr_name_t nm,
    const struct zd_ref *all, size_t nall, const struct zd_ref *ren,
    size_t nren)
{
	const char *s;
	zr_name_t an;
	size_t len;

	s = zr_names_str(ns, nm, &len);
	if (s == NULL || zd_has(all, nall, s, len))
		return (1);
	for (an = zr_names_parent(ns, nm); an != ZR_NAME_NONE;
	    an = zr_names_parent(ns, an)) {
		s = zr_names_str(ns, an, &len);
		if (s == NULL || zd_has(ren, nren, s, len))
			return (1);
	}
	return (0);
}

int
zr_diff_apply_unchanged(const struct zr_diff *d, const struct zr_walk *base,
    struct zr_walk *side, int side_index, struct zr_oracle *o)
{
	const struct zr_pool *sp, *bp;
	const struct zr_names *ns;
	struct zd_ref *all, *ren;
	size_t nall, nren;
	zr_pool_t b, p;
	uint32_t j;
	int marked, ok;

	if (d == NULL || base == NULL || side == NULL || o == NULL)
		return (-1);
	if (side_index != 1 && side_index != 2)
		return (-1);
	ns = side->zw_tree.zt_names;
	if (ns == NULL || ns != base->zw_tree.zt_names)
		return (-1);
	if (side->zw_tree.zt_sealed == 0 || base->zw_tree.zt_sealed == 0)
		return (-1);
	if (side->zw_tree.zt_npools > (uint32_t)INT_MAX)
		return (-1);
	if (zd_sets(d, &all, &nall, &ren, &nren) != 0)
		return (-1);
	marked = 0;
	for (p = 0; p < side->zw_tree.zt_npools; p++) {
		sp = &side->zw_tree.zt_pools[p];
		if (sp->zp_nnames == 0)
			continue;
		b = zr_tree_pool(&base->zw_tree, sp->zp_names[0]);
		if (b == ZR_POOL_NONE)
			continue;
		bp = &base->zw_tree.zt_pools[b];
		/*
		 * A pool that lost or gained a name is a different
		 * object here even when its bytes did not move. zfs
		 * diff reports that as a link count, but the count
		 * names only one of the pool's paths, so the sizes
		 * are compared as well.
		 */
		if (bp->zp_nnames != sp->zp_nnames)
			continue;
		ok = 1;
		for (j = 0; j < sp->zp_nnames; j++) {
			if (zr_tree_pool(&base->zw_tree,
			    sp->zp_names[j]) != b) {
				ok = 0;
				break;
			}
			if (zd_spoken_for(ns, sp->zp_names[j], all, nall,
			    ren, nren)) {
				ok = 0;
				break;
			}
		}
		if (ok == 0)
			continue;
		if (zr_oracle_unchanged(o, side_index, p, b) != 0) {
			free(all);
			free(ren);
			return (-1);
		}
		marked++;
	}
	free(all);
	free(ren);
	return (marked);
}
