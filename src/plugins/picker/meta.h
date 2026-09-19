/*
 * meta: the metadata three-way for a picker row.
 *
 * One row per attribute: mode, owner, group, flags, each xattr by
 * full name (union of the three trees' names), ACL, default ACL.
 * The resolution rule mirrors the content merge: from equal to base
 * yields onto's value; onto equal to base yields from's; from equal
 * to onto is that value; else a conflict that needs a pick.
 *
 * The structure is pure and independent of zr_m3: an attribute merge
 * is not a text merge, and bending zr_m3 to hold it would be the
 * wrong thing (design note C).
 */
#ifndef	ZR_META_H
#define	ZR_META_H

#include "../../walk.h"		/* struct zr_attr, zr_acl_t */

/*
 * Attribute kinds. ZR_MK_XATTR rows are the union of the three
 * trees' xattr names, in sorted order.
 */
enum zr_mk_kind {
	ZR_MK_MODE,
	ZR_MK_OWNER,
	ZR_MK_GROUP,
	ZR_MK_FLAGS,
	ZR_MK_XATTR,	/* one per name in the union */
	ZR_MK_ACL,
	ZR_MK_DACL
};

/* Where a resolved row's value came from, or none for a conflict. */
enum zr_mk_src {
	ZR_MK_NONE,	/* conflict, unpicked */
	ZR_MK_FROM,	/* from changed it, or both changed the same way */
	ZR_MK_ONTO,	/* onto changed it */
	ZR_MK_BASE	/* unchanged: all three agree */
};

/*
 * One metadata row. The three values are stored as indices into the
 * parent zr_pk_meta's attr array for scalar attrs, or as pointers
 * into the xattr arrays. For simplicity, we store a "present" flag
 * per tree and the comparison result. The actual values are read
 * from the parent's per-tree struct zr_attr.
 */
struct zr_mk_row {
	enum zr_mk_kind	mr_kind;
	const char		*mr_name;	/* xattr name, or NULL */
	/*
	 * mr_present[t]: whether tree t has this attribute.
	 * For mode/owner/group/flags this is always true if the
	 * tree's object exists.
	 */
	int			mr_present[3];	/* base, from, onto */
	enum zr_mk_src		mr_src;		/* resolved source */
	int			mr_pick;	/* -1 none, 0 from, 1 onto */
	int			mr_conflict;	/* 1 if a real conflict */
};

/*
 * The metadata merge of one row of the picker. The struct zr_attr
 * values are BORROWED: the caller keeps them alive until close.
 */
struct zr_pk_meta {
	int			mm_have[3];	/* whether tree exists */
	struct zr_mk_row	*mm_rows;
	uint32_t		mm_nrows;
	uint32_t		mm_nconflict;
	uint32_t		mm_npicked;
	uint32_t		mm_cursor;	/* conflict row index */
	int			mm_open;
};

/*
 * Build the metadata rows from three attribute sets. The attrs are
 * BORROWED: the caller keeps them alive until zr_pk_meta_close.
 * have[t] says whether tree t exists at all. Returns 0 or -1.
 */
int zr_pk_meta_open(struct zr_pk_meta *out,
    const struct zr_attr *base, int have_base,
    const struct zr_attr *from, int have_from,
    const struct zr_attr *onto, int have_onto);

void zr_pk_meta_close(struct zr_pk_meta *mm);

/* Pick from (0) or onto (1) for the conflict row under the cursor. */
int zr_pk_meta_pick(struct zr_pk_meta *mm, int pick);

/* Unpick: back to conflict. */
int zr_pk_meta_unpick(struct zr_pk_meta *mm);

/* Move to the next/prev conflict row. Returns 1 if moved. */
int zr_pk_meta_next(struct zr_pk_meta *mm);
int zr_pk_meta_prev(struct zr_pk_meta *mm);

/* All conflicts picked? */
int zr_pk_meta_complete(const struct zr_pk_meta *mm);

/* The first unpicked conflict index, or -1 if all picked. */
int zr_pk_meta_first_unpicked(const struct zr_pk_meta *mm, uint32_t *out);

/*
 * Build the resolved attribute set. The caller owns it and must
 * hand it to zr_attr_free. Returns 0 or -1 with a line in err.
 */
int zr_pk_meta_result(const struct zr_pk_meta *mm,
    const struct zr_attr *base,
    const struct zr_attr *from,
    const struct zr_attr *onto,
    struct zr_attr *out, char *err, size_t errlen);

/*
 * Whether two objects' attributes differ per the same rule as
 * zo_attrs_equal: mode, uid, gid, flags, xattrs, ACL, default ACL.
 * Returns 1 if they differ, 0 if the same.
 */
int zr_attrs_differ(const struct zr_attr *a, const struct zr_attr *b);

/*
 * Render an ACL to text for the metadata view. On FreeBSD the text
 * uses numeric ids. On other platforms the walk already stored
 * acl_to_text's output, so this returns a copy of it. Returns a
 * malloced string the caller frees with free(3), or NULL when the
 * ACL is absent or the platform has no ACL support. Never fails on
 * an absent ACL.
 */
char *zr_acl_to_text(zr_acl_t acl);

/*
 * Render file flags to a name string. On FreeBSD and macOS this is
 * fflagstostr(3); elsewhere hex. The result is malloced and the
 * caller frees it. Returns NULL only on allocation failure; 0 flags
 * yield "-".
 */
char *zr_flags_to_text(uint32_t flags);

#endif	/* ZR_META_H */
