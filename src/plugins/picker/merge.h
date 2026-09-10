/*
 * The three-way text merge behind the picker's second screen: three
 * files of bytes in, a sequence of chunks out, and the merged bytes
 * once every conflict chunk has been picked.
 *
 * The theory of record is zfs-rebase-theory/v4-merge3.md in the
 * freebsd-development repository, checked by v4-merge3check.py beside
 * it and exported as tests/battery/merge/merge3.txt, which is this
 * code's acceptance bar. The plan is sprints/sprint-6/
 * implementation-plan.md, sections 3.4 and 3.6.
 *
 * The shape of it. Two two-way diffs by the carried libdiff, base
 * against from and base against onto, fold into two edit scripts that
 * share base as their axis; the walk of diff3.c, adapted into
 * diff3.c beside this header, reads them together and emits the
 * changed stretches; the stable stretches are the gaps between them.
 * Every chunk carries three line ranges, one into each file, and a
 * kind. Every chunk keeps its base range -- the ruling of 2026-09-09
 * -- which is why base sits on the axis and why no refinement past
 * git's "eager" level happens here: a sub-conflict cut by a diff of
 * the two sides against each other would have no base range to keep.
 *
 * Nothing here knows about curses, the resolution, the manifest or a
 * pool. It is three buffers of bytes and the answer about them.
 */

#ifndef	ZR_MERGE_H
#define	ZR_MERGE_H

#include <stddef.h>
#include <stdint.h>

/*
 * A chunk's kind (v4-merge3.md section 1).
 *
 *	ZR_M3_STABLE	all three hold the same lines
 *	ZR_M3_FROM	from changed the stretch and onto did not
 *	ZR_M3_ONTO	onto changed the stretch and from did not
 *	ZR_M3_SAME	both sides changed it, to the same lines
 *	ZR_M3_CONFLICT	both sides changed it, differently
 *
 * ZR_M3_SAME is not a conflict. That is git's eager level, which is
 * both our floor and our ceiling; GNU diff3 -m brackets it, and
 * v4-merge3.md sections 3.5 and 4 say why we do not follow it there.
 */
enum zr_m3_kind {
	ZR_M3_STABLE,
	ZR_M3_FROM,
	ZR_M3_ONTO,
	ZR_M3_SAME,
	ZR_M3_CONFLICT
};

/* What a conflict chunk's pick holds. */
#define	ZR_M3_PICK_NONE	0
#define	ZR_M3_PICK_FROM	1
#define	ZR_M3_PICK_ONTO	2

/*
 * One chunk. The three ranges are half-open and zero-based, over the
 * line tables below, and any of them may be empty: an insertion by
 * from is a chunk whose base range is empty, a deletion by both is a
 * chunk whose from and onto ranges are both empty. pick is read only
 * for a ZR_M3_CONFLICT chunk and is one of the three values above.
 */
struct zr_m3_chunk {
	enum zr_m3_kind	kind;
	uint32_t	base_lo;
	uint32_t	base_hi;
	uint32_t	from_lo;
	uint32_t	from_hi;
	uint32_t	onto_lo;
	uint32_t	onto_hi;
	int		pick;
};

/*
 * One line record: an offset into the file's bytes and a length. The
 * length takes in the newline where the line has one, so that "b" and
 * "b\n" are two different lines and the missing final newline is part
 * of the last line's identity rather than an absence at the end of
 * the file (v4-merge3.md section 5). Two records are equal when their
 * bytes are equal, which is the only comparison anywhere in the
 * merge.
 */
struct zr_m3_line {
	uint32_t	off;
	uint32_t	len;
};

/*
 * A file: the bytes, and the line table cut from them.
 *
 * The bytes are BORROWED. The caller owns them and must keep them
 * alive and unchanged for the life of the struct zr_m3; the merge
 * reads them for the diffs, for the record comparisons and for the
 * result, and copies them nowhere. The line table is OWNED and is
 * freed by zr_m3_fini.
 */
struct zr_m3_file {
	const unsigned char	*bytes;
	size_t			len;
	struct zr_m3_line	*lines;
	uint32_t		nlines;
};

/*
 * One merge. Filled by zr_m3_open, emptied by zr_m3_fini.
 *
 * has_base is 0 for the add/add form, where there is no base object
 * to anchor against: the chunks are then a two-way compare of from
 * against onto, stable where they agree and conflict where they do
 * not, and every base range is empty.
 */
struct zr_m3 {
	struct zr_m3_file	base;
	struct zr_m3_file	from;
	struct zr_m3_file	onto;
	int			has_base;
	struct zr_m3_chunk	*chunks;
	uint32_t		nchunks;
	uint32_t		nconflict;
};

/*
 * The inside-conflict hint (v4-merge3.md section 4). One byte per
 * line of the conflict chunk's from half and one per line of its onto
 * half: ZR_M3_HINT_SAME where the diff of the two halves matched that
 * line against the other side, ZR_M3_HINT_DIFF where it did not. It
 * is the diff git's zealous refinement would run and it is taken for
 * the eye alone: it marks which lines inside the chunk differ so that
 * the two side panes can gutter them. It never splits a chunk, never
 * changes a kind and never reaches the chunk sequence.
 */
#define	ZR_M3_HINT_SAME	0
#define	ZR_M3_HINT_DIFF	1

struct zr_m3_hint {
	unsigned char	*from_marks;
	uint32_t	from_n;
	unsigned char	*onto_marks;
	uint32_t	onto_n;
};

/*
 * Is this object text? git's rule, and the merge's: an object is
 * binary if a NUL byte appears in its first 8000 bytes. Returns 1 for
 * text and 0 for binary; an empty object, and a NULL one, are text.
 * zr_m3_open applies this to all three before it asks libdiff for
 * anything, and libdiff's own binary notion (a NUL anywhere, raised
 * as DIFF_ATOMIZER_FOUND_BINARY_DATA) is not used.
 */
int zr_m3_text(const unsigned char *bytes, size_t len);

/*
 * Run the two diffs and the walk.
 *
 * base may be NULL, which is the add/add form: neither side's name
 * existed there before, so there is nothing to anchor against, and
 * what comes back is a two-way compare of from against onto under the
 * same pick rule. An empty base OBJECT is a non-NULL pointer with
 * baselen 0, which is a different thing and goes through the walk.
 *
 * from and onto must both be present. One of them absent with a base
 * present is delete/edit, which is a choice and not a merge: the row
 * takes from or onto or keep like any other and nothing opens. This
 * refuses it, and says so in err.
 *
 * Returns 0, or -1 with err filled in: an object that is binary, an
 * object too large for the line table, out of memory, or delete/edit.
 * On failure nothing is left allocated.
 */
int zr_m3_open(struct zr_m3 *m, const unsigned char *base, size_t baselen,
    const unsigned char *from, size_t fromlen, const unsigned char *onto,
    size_t ontolen, char *err, size_t errlen);

/*
 * Answer one conflict chunk: ZR_M3_PICK_FROM, ZR_M3_PICK_ONTO, or
 * ZR_M3_PICK_NONE to take the answer back. A chunk index out of range
 * and a chunk that is not a conflict are ignored, and the last pick
 * on a chunk stands.
 */
void zr_m3_pick(struct zr_m3 *m, uint32_t chunk, int pick);

/* How many conflict chunks are still unanswered. 0 means writable. */
uint32_t zr_m3_unpicked(const struct zr_m3 *m);

/*
 * The index of the first unanswered conflict chunk, in file order.
 * Returns 0 and sets *chunk, or -1 when there is none.
 */
int zr_m3_first_unpicked(const struct zr_m3 *m, uint32_t *chunk);

/*
 * The merged bytes, malloc'd; the caller frees them. *out is never
 * NULL, and *outlen may be 0.
 *
 * Refused with -1, and err naming the first unanswered conflict
 * chunk, while any conflict is unpicked. That is the ruling of
 * 2026-09-10: the picker may not write a result which still contains
 * conflicts, and no marker is ever produced here by any path -- the
 * two halves between markers exist on the screen and nowhere else.
 *
 * A stable chunk takes base's records (from's, in the add/add form,
 * where base has none and the two agree); a from-only chunk from's, an
 * onto-only chunk onto's, a both-same chunk from's, which are onto's
 * too; a conflict chunk the picked side's. Each record carries its own
 * newline or lack of one, so a missing final newline on the record
 * that ends the result is kept.
 */
int zr_m3_result(const struct zr_m3 *m, unsigned char **out, size_t *outlen,
    char *err, size_t errlen);

/*
 * The inside-conflict diff of one conflict chunk's two halves,
 * computed on demand. Returns 0 and fills *h, or -1 with err: a chunk
 * that is not a conflict, or out of memory. Free it with
 * zr_m3_hint_fini. The chunk sequence is not touched.
 */
int zr_m3_hint(const struct zr_m3 *m, uint32_t chunk, struct zr_m3_hint *h,
    char *err, size_t errlen);

void zr_m3_hint_fini(struct zr_m3_hint *h);

/* Free the line tables and the chunk array. The bytes are not ours. */
void zr_m3_fini(struct zr_m3 *m);

/*
 * The walk's input, which is diff3.c's business and merge.c's to
 * build. One entry of one edit script: a stretch the two-way diff did
 * not match, as a range into the side and a range into base, either of
 * which may be empty. The entries of one script are in order and two
 * of them always have at least one matched base line between them,
 * since each stretch was maximal. That is the shape diff3.c's
 * readin() builds out of diff's hunk headers and the shape libdiff's
 * chunk list folds into: a run of plus and minus chunks with no same
 * chunk between them is one entry.
 */
struct zr_m3_entry {
	uint32_t	side_lo;
	uint32_t	side_hi;
	uint32_t	base_lo;
	uint32_t	base_hi;
};

/*
 * The walk itself (diff3.c). d13 is from's script and d23 is onto's,
 * both anchored on base; both arrays are MODIFIED IN PLACE, as
 * diff3.c's merge() modifies its own, because the walk widens two
 * entries until they coincide. base is read for its line count alone;
 * from and onto are read for the record comparison that decides
 * both-same from conflict.
 *
 * On success *out is a malloc'd chunk array, *nout its length, and the
 * caller frees it. Returns -1 with err on a bad script or out of
 * memory.
 */
int zr_m3_walk(const struct zr_m3_file *base, const struct zr_m3_file *from,
    const struct zr_m3_file *onto, struct zr_m3_entry *d13, uint32_t m1,
    struct zr_m3_entry *d23, uint32_t m2, struct zr_m3_chunk **out,
    uint32_t *nout, char *err, size_t errlen);

#endif	/* ZR_MERGE_H */
