/*
 * meta: the metadata three-way for a picker row.
 *
 * The resolution rule is the content merge's own: from equal to base
 * yields onto's value; onto equal to base yields from's; from equal
 * to onto is that value; else a conflict that needs a pick. Times
 * have no row: a file written twice from the same bytes is the same
 * content, and a rebase that said otherwise would conflict on every
 * copy (zo_attrs_equal's rule).
 */

#ifdef __FreeBSD__
#define	_XOPEN_SOURCE	700
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <sys/acl.h>
#elif defined(__APPLE__)
#include <sys/acl.h>
#endif

#if defined(__FreeBSD__) || defined(__APPLE__)
#define	MK_HAVE_FFLAGSTOSTR	1
#endif

#include "meta.h"

/*
 * Whether two objects' attributes differ per the same rule as
 * zo_attrs_equal: mode, uid, gid, flags, xattrs (name+len+bytes),
 * ACL, default ACL. Times excluded.
 */
static int
mk_xattrs_equal(const struct zr_attr *a, const struct zr_attr *b)
{
	uint32_t i;

	if (a->za_nxattrs != b->za_nxattrs)
		return (0);
	for (i = 0; i < a->za_nxattrs; i++) {
		if (strcmp(a->za_xattrs[i].zx_name,
		    b->za_xattrs[i].zx_name) != 0)
			return (0);
		if (a->za_xattrs[i].zx_len != b->za_xattrs[i].zx_len)
			return (0);
		if (a->za_xattrs[i].zx_len != 0 &&
		    memcmp(a->za_xattrs[i].zx_value,
		    b->za_xattrs[i].zx_value,
		    a->za_xattrs[i].zx_len) != 0)
			return (0);
	}
	return (1);
}

int
zr_attrs_differ(const struct zr_attr *a, const struct zr_attr *b)
{
	if (a->za_mode != b->za_mode || a->za_uid != b->za_uid ||
	    a->za_gid != b->za_gid || a->za_flags != b->za_flags)
		return (1);
	if (mk_xattrs_equal(a, b) == 0)
		return (1);
	if (zr_acl_equal(a->za_acl, b->za_acl) == 0)
		return (1);
	return (zr_acl_equal(a->za_dacl, b->za_dacl) == 0);
}

/* Find an xattr by name in an attr set. Returns the index or -1. */
static int
mk_xa_find(const struct zr_attr *at, const char *name)
{
	uint32_t i;

	for (i = 0; i < at->za_nxattrs; i++) {
		if (strcmp(at->za_xattrs[i].zx_name, name) == 0)
			return ((int)i);
	}
	return (-1);
}

/* Are two xattr values equal? */
static int
mk_xa_equal(const struct zr_xattr *a, const struct zr_xattr *b)
{
	if (a->zx_len != b->zx_len)
		return (0);
	if (a->zx_len == 0)
		return (1);
	return (memcmp(a->zx_value, b->zx_value, a->zx_len) == 0);
}

/*
 * Resolve one scalar attribute. The three values are compared; the
 * equal(a,b) function returns 1 if the two are the same.
 */
static void
mk_resolve(struct zr_mk_row *row, int have_base, int have_from,
    int have_onto, int fb_eq, int bo_eq, int fo_eq)
{
	row->mr_pick = -1;
	row->mr_conflict = 0;

	if (!have_from && !have_onto) {
		/* both absent: base or nothing, no conflict */
		row->mr_src = ZR_MK_BASE;
		return;
	}
	if (!have_from) {
		row->mr_src = ZR_MK_ONTO;
		return;
	}
	if (!have_onto) {
		row->mr_src = ZR_MK_FROM;
		return;
	}
	/* both present */
	if (!have_base) {
		/* add/add: from == onto is that, else conflict */
		if (fo_eq) {
			row->mr_src = ZR_MK_FROM;
		} else {
			row->mr_src = ZR_MK_NONE;
			row->mr_conflict = 1;
		}
		return;
	}
	/* all three present */
	if (fb_eq && bo_eq) {
		/* all same */
		row->mr_src = ZR_MK_BASE;
		return;
	}
	if (fb_eq) {
		/* from == base, onto changed */
		row->mr_src = ZR_MK_ONTO;
		return;
	}
	if (bo_eq) {
		/* onto == base, from changed */
		row->mr_src = ZR_MK_FROM;
		return;
	}
	if (fo_eq) {
		/* both changed the same way */
		row->mr_src = ZR_MK_FROM;
		return;
	}
	/* both changed differently */
	row->mr_src = ZR_MK_NONE;
	row->mr_conflict = 1;
}

/* Add one row to the array. Returns 0 or -1. */
static int
mk_add(struct zr_pk_meta *mm, enum zr_mk_kind kind, const char *name,
    int have_base, int have_from, int have_onto,
    int fb_eq, int bo_eq, int fo_eq)
{
	struct zr_mk_row *tab, *row;

	tab = realloc(mm->mm_rows, (size_t)(mm->mm_nrows + 1) *
	    sizeof (struct zr_mk_row));
	if (tab == NULL)
		return (-1);
	mm->mm_rows = tab;
	row = &tab[mm->mm_nrows];
	memset(row, 0, sizeof (*row));
	row->mr_kind = kind;
	row->mr_name = name;
	row->mr_present[0] = have_base;
	row->mr_present[1] = have_from;
	row->mr_present[2] = have_onto;
	mk_resolve(row, have_base, have_from, have_onto,
	    fb_eq, bo_eq, fo_eq);
	if (row->mr_conflict)
		mm->mm_nconflict++;
	mm->mm_nrows++;
	return (0);
}

/* Compare two uint32_t values. */
static int
mk_u32_eq(uint32_t a, uint32_t b)
{
	return (a == b);
}

/*
 * Compare two mode_t values on the permission bits only. A type
 * difference (S_IFMT) is content (C in the DIFF column) and reaches
 * no metadata row, since screen 2 never opens on it: the content
 * differs on a non-text object, and zr_pk_why_not refuses the open.
 */
static int
mk_mode_eq(mode_t a, mode_t b)
{
	return ((a & 07777) == (b & 07777));
}

int
zr_pk_meta_open(struct zr_pk_meta *out,
    const struct zr_attr *base, int have_base,
    const struct zr_attr *from, int have_from,
    const struct zr_attr *onto, int have_onto)
{
	static const struct zr_attr zero;
	const struct zr_attr *b, *f, *o;
	uint32_t xi, yi, zi;
	int rc;

	if (out == NULL)
		return (-1);
	memset(out, 0, sizeof (*out));

	b = have_base ? base : &zero;
	f = have_from ? from : &zero;
	o = have_onto ? onto : &zero;

	out->mm_have[0] = have_base;
	out->mm_have[1] = have_from;
	out->mm_have[2] = have_onto;

	/* mode */
	rc = mk_add(out, ZR_MK_MODE, NULL,
	    have_base, have_from, have_onto,
	    mk_mode_eq(b->za_mode, f->za_mode),
	    mk_mode_eq(b->za_mode, o->za_mode),
	    mk_mode_eq(f->za_mode, o->za_mode));
	if (rc != 0)
		goto fail;

	/* owner */
	rc = mk_add(out, ZR_MK_OWNER, NULL,
	    have_base, have_from, have_onto,
	    mk_u32_eq(b->za_uid, f->za_uid),
	    mk_u32_eq(b->za_uid, o->za_uid),
	    mk_u32_eq(f->za_uid, o->za_uid));
	if (rc != 0)
		goto fail;

	/* group */
	rc = mk_add(out, ZR_MK_GROUP, NULL,
	    have_base, have_from, have_onto,
	    mk_u32_eq(b->za_gid, f->za_gid),
	    mk_u32_eq(b->za_gid, o->za_gid),
	    mk_u32_eq(f->za_gid, o->za_gid));
	if (rc != 0)
		goto fail;

	/* flags */
	rc = mk_add(out, ZR_MK_FLAGS, NULL,
	    have_base, have_from, have_onto,
	    mk_u32_eq(b->za_flags, f->za_flags),
	    mk_u32_eq(b->za_flags, o->za_flags),
	    mk_u32_eq(f->za_flags, o->za_flags));
	if (rc != 0)
		goto fail;

	/*
	 * Xattrs: the union of the three trees' names, in sorted
	 * order. The arrays are already sorted by name in each attr
	 * set (walk.h). A name present in one tree and absent in the
	 * others is like an add on that side.
	 */
	xi = yi = zi = 0;
	while (xi < b->za_nxattrs || yi < f->za_nxattrs ||
	    zi < o->za_nxattrs) {
		const char *name;
		int hb, hf, ho;
		int bf, bo, fo;
		const struct zr_xattr *xb, *xf, *xo;

		/* find the smallest name among the three heads */
		name = NULL;
		if (xi < b->za_nxattrs)
			name = b->za_xattrs[xi].zx_name;
		if (yi < f->za_nxattrs) {
			if (name == NULL ||
			    strcmp(f->za_xattrs[yi].zx_name, name) < 0)
				name = f->za_xattrs[yi].zx_name;
		}
		if (zi < o->za_nxattrs) {
			if (name == NULL ||
			    strcmp(o->za_xattrs[zi].zx_name, name) < 0)
				name = o->za_xattrs[zi].zx_name;
		}

		/* which trees have this name */
		hb = (xi < b->za_nxattrs &&
		    strcmp(b->za_xattrs[xi].zx_name, name) == 0);
		hf = (yi < f->za_nxattrs &&
		    strcmp(f->za_xattrs[yi].zx_name, name) == 0);
		ho = (zi < o->za_nxattrs &&
		    strcmp(o->za_xattrs[zi].zx_name, name) == 0);

		xb = hb ? &b->za_xattrs[xi] : NULL;
		xf = hf ? &f->za_xattrs[yi] : NULL;
		xo = ho ? &o->za_xattrs[zi] : NULL;

		/* pairwise equality */
		bf = (hb && hf) ? mk_xa_equal(xb, xf) :
		    (!hb && !hf) ? 1 : 0;
		bo = (hb && ho) ? mk_xa_equal(xb, xo) :
		    (!hb && !ho) ? 1 : 0;
		fo = (hf && ho) ? mk_xa_equal(xf, xo) :
		    (!hf && !ho) ? 1 : 0;

		/*
		 * "have" for the resolve: a tree that exists but
		 * lacks this xattr is "present with absent value",
		 * which is a specific state (removed it). A tree
		 * that does not exist at all for this name cannot
		 * contribute. Use the tree-exists flag, not the
		 * xattr-found flag, to determine the three-way.
		 */
		rc = mk_add(out, ZR_MK_XATTR, name,
		    have_base, have_from, have_onto,
		    bf, bo, fo);
		if (rc != 0)
			goto fail;

		if (hb) xi++;
		if (hf) yi++;
		if (ho) zi++;
	}

	/*
	 * No tree has an xattr at all: one row with no name, so the
	 * view says "none" the way the acl row does, and a person can
	 * tell an empty set from one that was never read. It compares
	 * equal on every side, so it resolves and is never a conflict,
	 * and zr_pk_meta_result skips a row with no name.
	 */
	if (b->za_nxattrs == 0 && f->za_nxattrs == 0 &&
	    o->za_nxattrs == 0) {
		rc = mk_add(out, ZR_MK_XATTR, NULL,
		    have_base, have_from, have_onto, 1, 1, 1);
		if (rc != 0)
			goto fail;
	}

	/* ACL */
	rc = mk_add(out, ZR_MK_ACL, NULL,
	    have_base, have_from, have_onto,
	    zr_acl_equal(b->za_acl, f->za_acl),
	    zr_acl_equal(b->za_acl, o->za_acl),
	    zr_acl_equal(f->za_acl, o->za_acl));
	if (rc != 0)
		goto fail;

	/*
	 * Default ACL: only when some tree is a directory or any tree
	 * holds one, so a regular file does not show a row of dashes.
	 */
	if ((have_base && S_ISDIR(b->za_mode)) ||
	    (have_from && S_ISDIR(f->za_mode)) ||
	    (have_onto && S_ISDIR(o->za_mode)) ||
	    b->za_dacl != NULL || f->za_dacl != NULL ||
	    o->za_dacl != NULL) {
		rc = mk_add(out, ZR_MK_DACL, NULL,
		    have_base, have_from, have_onto,
		    zr_acl_equal(b->za_dacl, f->za_dacl),
		    zr_acl_equal(b->za_dacl, o->za_dacl),
		    zr_acl_equal(f->za_dacl, o->za_dacl));
		if (rc != 0)
			goto fail;
	}

	/* set cursor to first conflict */
	out->mm_cursor = out->mm_nrows;
	{
		uint32_t i;
		for (i = 0; i < out->mm_nrows; i++) {
			if (out->mm_rows[i].mr_conflict) {
				out->mm_cursor = i;
				break;
			}
		}
	}

	out->mm_open = 1;
	return (0);
fail:
	zr_pk_meta_close(out);
	return (-1);
}

void
zr_pk_meta_close(struct zr_pk_meta *mm)
{
	if (mm == NULL)
		return;
	free(mm->mm_rows);
	memset(mm, 0, sizeof (*mm));
}

int
zr_pk_meta_pick(struct zr_pk_meta *mm, int pick)
{
	struct zr_mk_row *row;

	if (mm == NULL || mm->mm_cursor >= mm->mm_nrows)
		return (-1);
	row = &mm->mm_rows[mm->mm_cursor];
	if (!row->mr_conflict)
		return (-1);
	if (row->mr_pick == pick)
		return (0);	/* already picked this way */
	if (row->mr_pick < 0)
		mm->mm_npicked++;
	row->mr_pick = pick;
	row->mr_src = (pick == 0) ? ZR_MK_FROM : ZR_MK_ONTO;
	return (0);
}

int
zr_pk_meta_unpick(struct zr_pk_meta *mm)
{
	struct zr_mk_row *row;

	if (mm == NULL || mm->mm_cursor >= mm->mm_nrows)
		return (-1);
	row = &mm->mm_rows[mm->mm_cursor];
	if (!row->mr_conflict || row->mr_pick < 0)
		return (-1);
	row->mr_pick = -1;
	row->mr_src = ZR_MK_NONE;
	mm->mm_npicked--;
	return (0);
}

int
zr_pk_meta_next(struct zr_pk_meta *mm)
{
	uint32_t i;

	if (mm == NULL || mm->mm_nconflict == 0)
		return (0);
	for (i = mm->mm_cursor + 1; i < mm->mm_nrows; i++) {
		if (mm->mm_rows[i].mr_conflict) {
			mm->mm_cursor = i;
			return (1);
		}
	}
	/* wrap */
	for (i = 0; i <= mm->mm_cursor && i < mm->mm_nrows; i++) {
		if (mm->mm_rows[i].mr_conflict) {
			mm->mm_cursor = i;
			return (1);
		}
	}
	return (0);
}

int
zr_pk_meta_prev(struct zr_pk_meta *mm)
{
	uint32_t i;

	if (mm == NULL || mm->mm_nconflict == 0)
		return (0);
	if (mm->mm_cursor > 0) {
		for (i = mm->mm_cursor - 1; ; i--) {
			if (mm->mm_rows[i].mr_conflict) {
				mm->mm_cursor = i;
				return (1);
			}
			if (i == 0)
				break;
		}
	}
	/* wrap */
	for (i = mm->mm_nrows; i > mm->mm_cursor; ) {
		i--;
		if (mm->mm_rows[i].mr_conflict) {
			mm->mm_cursor = i;
			return (1);
		}
	}
	return (0);
}

int
zr_pk_meta_complete(const struct zr_pk_meta *mm)
{
	if (mm == NULL)
		return (1);
	return (mm->mm_nconflict == mm->mm_npicked);
}

int
zr_pk_meta_first_unpicked(const struct zr_pk_meta *mm, uint32_t *out)
{
	uint32_t i;

	if (mm == NULL)
		return (-1);
	for (i = 0; i < mm->mm_nrows; i++) {
		if (mm->mm_rows[i].mr_conflict &&
		    mm->mm_rows[i].mr_pick < 0) {
			*out = i;
			return (0);
		}
	}
	return (-1);
}

/*
 * Copy an xattr. The name and value are duplicated.
 */
static int
mk_xa_copy(struct zr_xattr *dst, const struct zr_xattr *src)
{
	dst->zx_name = strdup(src->zx_name);
	if (dst->zx_name == NULL)
		return (-1);
	if (src->zx_len == 0) {
		dst->zx_value = malloc(1);
		dst->zx_len = 0;
	} else {
		dst->zx_value = malloc(src->zx_len);
		if (dst->zx_value != NULL)
			memcpy(dst->zx_value, src->zx_value, src->zx_len);
		dst->zx_len = src->zx_len;
	}
	if (dst->zx_value == NULL) {
		free(dst->zx_name);
		dst->zx_name = NULL;
		return (-1);
	}
	return (0);
}

/*
 * Duplicate an ACL.
 */
static zr_acl_t
mk_acl_dup(zr_acl_t src)
{
#if defined(__FreeBSD__)
	return (src != NULL ? acl_dup(src) : NULL);
#else
	if (src == NULL)
		return (NULL);
	return (strdup(src));
#endif
}

int
zr_pk_meta_result(const struct zr_pk_meta *mm,
    const struct zr_attr *base,
    const struct zr_attr *from,
    const struct zr_attr *onto,
    struct zr_attr *out, char *err, size_t errlen)
{
	const struct zr_attr *src;
	const struct zr_mk_row *row;
	uint32_t i, nxa = 0;

	if (mm == NULL || out == NULL)
		return (-1);
	memset(out, 0, sizeof (*out));

	if (!zr_pk_meta_complete(mm)) {
		if (err != NULL && errlen > 0)
			(void) snprintf(err, errlen,
			    "metadata has unpicked conflicts");
		return (-1);
	}

	for (i = 0; i < mm->mm_nrows; i++) {
		row = &mm->mm_rows[i];
		switch (row->mr_src) {
		case ZR_MK_FROM:
			src = from;
			break;
		case ZR_MK_ONTO:
			src = onto;
			break;
		case ZR_MK_BASE:
			src = base != NULL ? base : from;
			break;
		default:
			if (err != NULL && errlen > 0)
				(void) snprintf(err, errlen,
				    "metadata row %u is unresolved",
				    (unsigned)i);
			zr_attr_free(out);
			return (-1);
		}

		switch (row->mr_kind) {
		case ZR_MK_MODE:
			/*
			 * Keep the type bits from from (or onto if from
			 * is absent). The permission bits come from src.
			 */
			if (from != NULL)
				out->za_mode = (from->za_mode & ~(mode_t)07777)
				    | (src->za_mode & 07777);
			else if (onto != NULL)
				out->za_mode = (onto->za_mode & ~(mode_t)07777)
				    | (src->za_mode & 07777);
			else
				out->za_mode = src->za_mode & 07777;
			break;
		case ZR_MK_OWNER:
			out->za_uid = src->za_uid;
			break;
		case ZR_MK_GROUP:
			out->za_gid = src->za_gid;
			break;
		case ZR_MK_FLAGS:
			out->za_flags = src->za_flags;
			break;
		case ZR_MK_XATTR:
			/*
			 * If src has this xattr, add it. If src does
			 * not (the resolved value is "absent"), skip it
			 * so the write removes it.
			 */
			if (row->mr_name != NULL) {
				int idx = mk_xa_find(src, row->mr_name);
				if (idx >= 0) {
					struct zr_xattr *tab;
					tab = realloc(out->za_xattrs,
					    (size_t)(nxa + 1) *
					    sizeof (struct zr_xattr));
					if (tab == NULL) {
						zr_attr_free(out);
						return (-1);
					}
					out->za_xattrs = tab;
					if (mk_xa_copy(&tab[nxa],
					    &src->za_xattrs[idx]) != 0) {
						zr_attr_free(out);
						return (-1);
					}
					nxa++;
					out->za_nxattrs = nxa;
				}
			}
			break;
		case ZR_MK_ACL:
			out->za_acl = mk_acl_dup(src->za_acl);
			break;
		case ZR_MK_DACL:
			out->za_dacl = mk_acl_dup(src->za_dacl);
			break;
		}
	}
	return (0);
}

/*
 * ---------------------------------------------------------------
 * Rendering helpers for the metadata view.
 * ---------------------------------------------------------------
 */

char *
zr_acl_to_text(zr_acl_t acl)
{
#if defined(__FreeBSD__)
	char *txt, *dup;
	ssize_t len;

	if (acl == NULL)
		return (NULL);
	txt = acl_to_text_np(acl, &len, ACL_TEXT_NUMERIC_IDS);
	if (txt == NULL)
		return (NULL);
	dup = malloc((size_t)len + 1);
	if (dup != NULL)
		memcpy(dup, txt, (size_t)len + 1);
	(void) acl_free(txt);
	return (dup);
#else
	/*
	 * On the stand-in the walk already stored acl_to_text's
	 * output as a string, so a copy is all that is needed.
	 */
	if (acl == NULL)
		return (NULL);
	return (strdup(acl));
#endif
}

char *
zr_flags_to_text(uint32_t flags)
{
	char *s;

	if (flags == 0)
		return (strdup("-"));
#ifdef MK_HAVE_FFLAGSTOSTR
	s = fflagstostr((unsigned long)flags);
	if (s != NULL) {
		if (s[0] == '\0') {
			/* flags set but none named: fall back to hex */
			free(s);
			s = malloc(16);
			if (s != NULL)
				(void) snprintf(s, 16, "0x%x",
				    (unsigned)flags);
			return (s);
		}
		return (s);
	}
#endif
	s = malloc(16);
	if (s != NULL)
		(void) snprintf(s, 16, "0x%x", (unsigned)flags);
	return (s);
}
