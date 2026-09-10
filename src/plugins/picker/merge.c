/*
 * merge.c: the glue between the carried libdiff and the adapted diff3
 * walk, and the public face of the merge. merge.h has the contract;
 * zfs-rebase-theory/v4-merge3.md is the theory of record and this file
 * implements its sections 2 and 5, where diff3.c implements its
 * section 3.
 *
 * Everything libdiff does not do for us is done here, because libdiff
 * is carried with zero local edits (plan section 3.6, and the copy's
 * UPSTREAM). Three things fall out of that:
 *
 *  - The line rule is ours. A line is the bytes up to and including a
 *    newline, and splitting is on newline alone. libdiff's stock
 *    atomizer, diff_atomize_text_by_line(), also breaks a line on a
 *    bare carriage return and folds a following newline into it, which
 *    is a different rule, so this file supplies its own atomizer
 *    through the atomize_func field of struct diff_config, which is
 *    what that field is for.
 *
 *  - The algorithm configuration is ours. libdiff's assembled
 *    configurations live in its diff/diff.c sample program, which the
 *    copy does not carry, so m3_patience below is the same nesting
 *    written out here: patience first, patience again on each section
 *    between the lines patience pinned, and forward Myers (falling
 *    back to divide-and-conquer Myers when its state would not fit) on
 *    a section where patience finds nothing to pin. That is
 *    diff_config_patience. It is NOT the example in diff_main.h's
 *    comment, which puts Myers at the top with patience as a fallback:
 *    that is diff_config_myers_then_patience and a different diff.
 *
 *  - Binary is decided here, before libdiff runs, by git's rule: a NUL
 *    byte in the first 8000 bytes. libdiff's own
 *    DIFF_ATOMIZER_FOUND_BINARY_DATA is a different rule (a NUL
 *    anywhere) and is not used.
 *
 * The one thing this file reaches into libdiff for is the chunk
 * record. struct diff_chunk is opaque in the installed diff_main.h and
 * its accessors live in diff_output.c, which the copy does not carry
 * (it prints diffs, and the merge walks the chunk list itself), so
 * this file includes the copy's lib/diff_internal.h for the struct's
 * definition. That is a read of a carried header and not an edit to
 * one.
 */

#include <sys/types.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arraylist.h>
#include <diff_main.h>

#include "diff_internal.h"

#include "merge.h"

/* git's rule: a NUL in this many leading bytes makes an object binary. */
#define	M3_BINARY_PROBE	8000

static void
m3_fail(char *err, size_t errlen, const char *what)
{
	if (err != NULL && errlen > 0)
		(void) snprintf(err, errlen, "merge: %s", what);
}

/*
 * The atomizer, in the shape diff_atomize_text.c's memory-mapped path
 * has: fill d->atoms, one atom per line, with the atom's length taking
 * in the newline. The hash runs over the line's bytes short of the
 * newline, as upstream's does; it is only a cheap way to find
 * mismatching atoms, and diff_atom_cmp compares the whole record,
 * length included, so a line that ends the file without a newline
 * never compares equal to the same text with one.
 */
static int
m3_atomize(void *func_data, struct diff_data *d)
{
	const uint8_t *pos = d->data;
	const uint8_t *end = pos + d->len;

	(void) func_data;
	ARRAYLIST_INIT(d->atoms, 128);
	while (pos < end) {
		const uint8_t *start = pos;
		struct diff_atom *atom;
		unsigned int hash = 0;

		while (pos < end && *pos != '\n') {
			hash = diff_atom_hash_update(hash, *pos);
			pos++;
		}
		if (pos < end)
			pos++;
		ARRAYLIST_ADD(atom, d->atoms);
		if (atom == NULL)
			return (ENOMEM);
		atom->root = d;
		atom->pos = (off_t)(start - d->data);
		atom->at = start;
		atom->len = (off_t)(pos - start);
		atom->hash = hash;
	}
	return (DIFF_RC_OK);
}

static const struct diff_algo_config m3_patience;
static const struct diff_algo_config m3_myers_then_myers_divide;
static const struct diff_algo_config m3_myers_divide;

static const struct diff_algo_config m3_myers_then_myers_divide = {
	.impl = diff_algo_myers,
	.permitted_state_size = 1024 * 1024 * sizeof (int),
	/* When the forward trace would not fit, divide and conquer. */
	.fallback_algo = &m3_myers_divide,
};

static const struct diff_algo_config m3_patience = {
	.impl = diff_algo_patience,
	/* After subdivision, patience again. */
	.inner_algo = &m3_patience,
	/* Where nothing is common-unique, Myers. */
	.fallback_algo = &m3_myers_then_myers_divide,
};

static const struct diff_algo_config m3_myers_divide = {
	.impl = diff_algo_myers_divide,
	/* When division succeeded, start from the top. */
	.inner_algo = &m3_myers_then_myers_divide,
};

static const struct diff_config m3_config = {
	.atomize_func = m3_atomize,
	.algo = &m3_patience,
};

int
zr_m3_text(const unsigned char *bytes, size_t len)
{
	size_t n = len < M3_BINARY_PROBE ? len : M3_BINARY_PROBE;

	if (bytes == NULL || n == 0)
		return (1);
	return (memchr(bytes, '\0', n) == NULL);
}

/*
 * Cut a file into line records. The bytes are borrowed; a NULL buffer
 * becomes a valid pointer to nothing, so that no arithmetic anywhere
 * below runs on a null pointer.
 */
static int
m3_split(struct zr_m3_file *f, const unsigned char *bytes, size_t len,
    char *err, size_t errlen)
{
	size_t i, off;
	uint32_t n = 0, k = 0;

	f->bytes = bytes != NULL ? bytes : (const unsigned char *)"";
	f->len = bytes != NULL ? len : 0;
	f->lines = NULL;
	f->nlines = 0;
	if (f->len > UINT32_MAX) {
		m3_fail(err, errlen, "the object is too large to merge");
		return (-1);
	}
	for (i = 0; i < f->len; i++)
		if (f->bytes[i] == '\n')
			n++;
	if (f->len != 0 && f->bytes[f->len - 1] != '\n')
		n++;
	if (n == 0)
		return (0);
	f->lines = calloc(n, sizeof (*f->lines));
	if (f->lines == NULL) {
		m3_fail(err, errlen, "out of memory");
		return (-1);
	}
	off = 0;
	while (off < f->len) {
		const unsigned char *nl;
		size_t linelen;

		nl = memchr(f->bytes + off, '\n', f->len - off);
		linelen = nl != NULL ? (size_t)(nl - (f->bytes + off)) + 1 :
		    f->len - off;
		f->lines[k].off = (uint32_t)off;
		f->lines[k].len = (uint32_t)linelen;
		k++;
		off += linelen;
	}
	f->nlines = n;
	return (0);
}

/* The first byte of line lo, which is where a range of records starts. */
static const unsigned char *
m3_at(const struct zr_m3_file *f, uint32_t lo)
{
	return (f->bytes + f->lines[lo].off);
}

/* The bytes of the records [lo, hi), which are contiguous in the file. */
static size_t
m3_span(const struct zr_m3_file *f, uint32_t lo, uint32_t hi)
{
	if (hi <= lo)
		return (0);
	return ((size_t)f->lines[hi - 1].off + f->lines[hi - 1].len -
	    f->lines[lo].off);
}

/*
 * Run one two-way diff and hand back its result and the two diff_data
 * it references, which must outlive it. Returns 0, or -1 with err.
 */
static int
m3_diff(const unsigned char *lb, size_t llen, const unsigned char *rb,
    size_t rlen, struct diff_data *dl, struct diff_data *dr,
    struct diff_result **out, char *err, size_t errlen)
{
	struct diff_result *r;

	memset(dl, 0, sizeof (*dl));
	memset(dr, 0, sizeof (*dr));
	*out = NULL;
	if (diff_atomize_file(dl, &m3_config, NULL, lb, (off_t)llen,
	    0) != DIFF_RC_OK ||
	    diff_atomize_file(dr, &m3_config, NULL, rb, (off_t)rlen,
	    0) != DIFF_RC_OK) {
		m3_fail(err, errlen, "the file could not be cut into lines");
		goto fail;
	}
	r = diff_main(&m3_config, dl, dr);
	if (r == NULL) {
		m3_fail(err, errlen, "out of memory");
		goto fail;
	}
	if (r->rc != DIFF_RC_OK) {
		diff_result_free(r);
		m3_fail(err, errlen, "the two-way diff failed");
		goto fail;
	}
	*out = r;
	return (0);
fail:
	diff_data_free(dl);
	diff_data_free(dr);
	return (-1);
}

/*
 * Fold one libdiff chunk list into one edit script: a run of plus and
 * minus chunks with no same chunk between them is one entry, whose
 * base range is the left side's and whose side range is the right's.
 * That swap is the slot assignment of v4-merge3.md section 3 -- base
 * on the axis -- carried out on the way in, so that no second diff is
 * ever run to get the other direction.
 */
static int
m3_script(const struct zr_m3_file *base, const struct zr_m3_file *side,
    struct zr_m3_entry **out, uint32_t *nout, char *err, size_t errlen)
{
	struct diff_data dl, dr;
	struct diff_result *r = NULL;
	struct zr_m3_entry *ents = NULL;
	unsigned int i;
	uint32_t n = 0, bc = 0, sc = 0, blo = 0, slo = 0;
	int open = 0;

	*out = NULL;
	*nout = 0;
	if (m3_diff(base->bytes, base->len, side->bytes, side->len, &dl, &dr,
	    &r, err, errlen) != 0)
		return (-1);
	ents = calloc((size_t)r->chunks.len + 1, sizeof (*ents));
	if (ents == NULL) {
		m3_fail(err, errlen, "out of memory");
		goto fail;
	}
	for (i = 0; i < r->chunks.len; i++) {
		const struct diff_chunk *c = &r->chunks.head[i];
		enum diff_chunk_type t = diff_chunk_type(c);

		if (t == CHUNK_ERROR) {
			m3_fail(err, errlen, "the two-way diff left a chunk "
			    "unsolved");
			goto fail;
		}
		if (t == CHUNK_EMPTY)
			continue;
		if (t == CHUNK_SAME) {
			if (open) {
				ents[n].base_lo = blo;
				ents[n].base_hi = bc;
				ents[n].side_lo = slo;
				ents[n].side_hi = sc;
				n++;
				open = 0;
			}
		} else if (!open) {
			open = 1;
			blo = bc;
			slo = sc;
		}
		bc += c->left_count;
		sc += c->right_count;
	}
	if (open) {
		ents[n].base_lo = blo;
		ents[n].base_hi = bc;
		ents[n].side_lo = slo;
		ents[n].side_hi = sc;
		n++;
	}
	if (bc != base->nlines || sc != side->nlines) {
		m3_fail(err, errlen, "the two-way diff does not cover its "
		    "files");
		goto fail;
	}
	diff_result_free(r);
	diff_data_free(&dl);
	diff_data_free(&dr);
	*out = ents;
	*nout = n;
	return (0);
fail:
	free(ents);
	diff_result_free(r);
	diff_data_free(&dl);
	diff_data_free(&dr);
	return (-1);
}

/*
 * The add/add form, which has no base to anchor against: one two-way
 * compare of from against onto, stable where they agree and conflict
 * where they do not, every base range empty, and the same pick rule.
 *
 * The walk would in fact give the same answer over an empty base --
 * both scripts insert their whole file at the one empty base range and
 * step 4 makes one chunk of it -- so this is a matter of what the
 * screen says rather than of what the merge decides (v4-merge3.md
 * section 5). An empty base OBJECT still goes through the walk; it is a
 * base that is absent altogether that comes here.
 */
static int
m3_twoway(struct zr_m3 *m, char *err, size_t errlen)
{
	struct diff_data dl, dr;
	struct diff_result *r = NULL;
	struct zr_m3_chunk *ch = NULL, *c;
	unsigned int i;
	uint32_t n = 0, fc = 0, oc = 0, flo = 0, olo = 0;
	int open = 0;

	if (m3_diff(m->from.bytes, m->from.len, m->onto.bytes, m->onto.len,
	    &dl, &dr, &r, err, errlen) != 0)
		return (-1);
	ch = calloc((size_t)r->chunks.len + 1, sizeof (*ch));
	if (ch == NULL) {
		m3_fail(err, errlen, "out of memory");
		goto fail;
	}
	for (i = 0; i < r->chunks.len; i++) {
		const struct diff_chunk *dc = &r->chunks.head[i];
		enum diff_chunk_type t = diff_chunk_type(dc);

		if (t == CHUNK_ERROR) {
			m3_fail(err, errlen, "the two-way diff left a chunk "
			    "unsolved");
			goto fail;
		}
		if (t == CHUNK_EMPTY)
			continue;
		if (t == CHUNK_SAME) {
			if (open) {
				c = &ch[n++];
				c->kind = ZR_M3_CONFLICT;
				c->from_lo = flo;
				c->from_hi = fc;
				c->onto_lo = olo;
				c->onto_hi = oc;
				open = 0;
			}
			c = &ch[n++];
			c->kind = ZR_M3_STABLE;
			c->from_lo = fc;
			c->from_hi = fc + dc->left_count;
			c->onto_lo = oc;
			c->onto_hi = oc + dc->right_count;
		} else if (!open) {
			open = 1;
			flo = fc;
			olo = oc;
		}
		fc += dc->left_count;
		oc += dc->right_count;
	}
	if (open) {
		c = &ch[n++];
		c->kind = ZR_M3_CONFLICT;
		c->from_lo = flo;
		c->from_hi = fc;
		c->onto_lo = olo;
		c->onto_hi = oc;
	}
	if (fc != m->from.nlines || oc != m->onto.nlines) {
		m3_fail(err, errlen, "the two-way compare does not cover its "
		    "files");
		goto fail;
	}
	diff_result_free(r);
	diff_data_free(&dl);
	diff_data_free(&dr);
	m->chunks = ch;
	m->nchunks = n;
	return (0);
fail:
	free(ch);
	diff_result_free(r);
	diff_data_free(&dl);
	diff_data_free(&dr);
	return (-1);
}

int
zr_m3_open(struct zr_m3 *m, const unsigned char *base, size_t baselen,
    const unsigned char *from, size_t fromlen, const unsigned char *onto,
    size_t ontolen, char *err, size_t errlen)
{
	struct zr_m3_entry *d13 = NULL, *d23 = NULL;
	uint32_t m1 = 0, m2 = 0, i;

	memset(m, 0, sizeof (*m));
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (from == NULL || onto == NULL) {
		m3_fail(err, errlen, base != NULL ?
		    "one side has no object at this name: delete against "
		    "edit is a choice and not a merge" :
		    "neither a base nor both sides: there is nothing to "
		    "merge");
		return (-1);
	}
	if (base != NULL && !zr_m3_text(base, baselen)) {
		m3_fail(err, errlen, "base is binary: a NUL byte in its "
		    "first 8000 bytes");
		return (-1);
	}
	if (!zr_m3_text(from, fromlen)) {
		m3_fail(err, errlen, "from is binary: a NUL byte in its "
		    "first 8000 bytes");
		return (-1);
	}
	if (!zr_m3_text(onto, ontolen)) {
		m3_fail(err, errlen, "onto is binary: a NUL byte in its "
		    "first 8000 bytes");
		return (-1);
	}
	m->has_base = base != NULL;
	if (m3_split(&m->base, base, baselen, err, errlen) != 0 ||
	    m3_split(&m->from, from, fromlen, err, errlen) != 0 ||
	    m3_split(&m->onto, onto, ontolen, err, errlen) != 0)
		goto fail;
	if (m->has_base) {
		if (m3_script(&m->base, &m->from, &d13, &m1, err,
		    errlen) != 0 ||
		    m3_script(&m->base, &m->onto, &d23, &m2, err,
		    errlen) != 0)
			goto fail;
		if (zr_m3_walk(&m->base, &m->from, &m->onto, d13, m1, d23, m2,
		    &m->chunks, &m->nchunks, err, errlen) != 0)
			goto fail;
		free(d13);
		free(d23);
		d13 = NULL;
		d23 = NULL;
	} else if (m3_twoway(m, err, errlen) != 0) {
		goto fail;
	}
	for (i = 0; i < m->nchunks; i++)
		if (m->chunks[i].kind == ZR_M3_CONFLICT)
			m->nconflict++;
	return (0);
fail:
	free(d13);
	free(d23);
	zr_m3_fini(m);
	return (-1);
}

void
zr_m3_pick(struct zr_m3 *m, uint32_t chunk, int pick)
{
	if (chunk >= m->nchunks)
		return;
	if (m->chunks[chunk].kind != ZR_M3_CONFLICT)
		return;
	if (pick != ZR_M3_PICK_NONE && pick != ZR_M3_PICK_FROM &&
	    pick != ZR_M3_PICK_ONTO)
		return;
	m->chunks[chunk].pick = pick;
}

uint32_t
zr_m3_unpicked(const struct zr_m3 *m)
{
	uint32_t i, n = 0;

	for (i = 0; i < m->nchunks; i++)
		if (m->chunks[i].kind == ZR_M3_CONFLICT &&
		    m->chunks[i].pick == ZR_M3_PICK_NONE)
			n++;
	return (n);
}

int
zr_m3_first_unpicked(const struct zr_m3 *m, uint32_t *chunk)
{
	uint32_t i;

	for (i = 0; i < m->nchunks; i++)
		if (m->chunks[i].kind == ZR_M3_CONFLICT &&
		    m->chunks[i].pick == ZR_M3_PICK_NONE) {
			*chunk = i;
			return (0);
		}
	return (-1);
}

/*
 * Which file and which of its records one chunk contributes: the
 * answer table of v4-merge3.md section 6. A stable chunk takes
 * base's records, or from's in the add/add form, where base has none
 * and the two sides agree.
 */
int
zr_m3_answer(const struct zr_m3 *m, uint32_t chunk, uint32_t *lo, uint32_t *hi)
{
	const struct zr_m3_chunk *c;

	if (chunk >= m->nchunks)
		return (-1);
	c = &m->chunks[chunk];
	switch (c->kind) {
	case ZR_M3_STABLE:
		if (m->has_base) {
			*lo = c->base_lo;
			*hi = c->base_hi;
			return (ZR_M3_F_BASE);
		}
		break;
	case ZR_M3_ONTO:
		*lo = c->onto_lo;
		*hi = c->onto_hi;
		return (ZR_M3_F_ONTO);
	case ZR_M3_CONFLICT:
		if (c->pick == ZR_M3_PICK_ONTO) {
			*lo = c->onto_lo;
			*hi = c->onto_hi;
			return (ZR_M3_F_ONTO);
		}
		break;
	case ZR_M3_FROM:
	case ZR_M3_SAME:
	default:
		break;
	}
	*lo = c->from_lo;
	*hi = c->from_hi;
	return (ZR_M3_F_FROM);
}

/* The same answer, as the file the result copies its bytes out of. */
static void
m3_choice(const struct zr_m3 *m, uint32_t chunk, const struct zr_m3_file **f,
    uint32_t *lo, uint32_t *hi)
{
	switch (zr_m3_answer(m, chunk, lo, hi)) {
	case ZR_M3_F_BASE:
		*f = &m->base;
		break;
	case ZR_M3_F_ONTO:
		*f = &m->onto;
		break;
	default:
		*f = &m->from;
		break;
	}
}

int
zr_m3_result(const struct zr_m3 *m, unsigned char **out, size_t *outlen,
    char *err, size_t errlen)
{
	const struct zr_m3_file *f;
	unsigned char *buf;
	size_t total = 0, at = 0;
	uint32_t i, lo, hi, first;

	*out = NULL;
	*outlen = 0;
	if (zr_m3_first_unpicked(m, &first) == 0) {
		if (err != NULL && errlen > 0)
			(void) snprintf(err, errlen, "merge: chunk %lu is a "
			    "conflict and has not been picked",
			    (unsigned long)first);
		return (-1);
	}
	for (i = 0; i < m->nchunks; i++) {
		m3_choice(m, i, &f, &lo, &hi);
		total += m3_span(f, lo, hi);
	}
	buf = malloc(total != 0 ? total : 1);
	if (buf == NULL) {
		m3_fail(err, errlen, "out of memory");
		return (-1);
	}
	for (i = 0; i < m->nchunks; i++) {
		size_t n;

		m3_choice(m, i, &f, &lo, &hi);
		n = m3_span(f, lo, hi);
		if (n != 0) {
			memcpy(buf + at, m3_at(f, lo), n);
			at += n;
		}
	}
	*out = buf;
	*outlen = total;
	return (0);
}

int
zr_m3_hint(const struct zr_m3 *m, uint32_t chunk, struct zr_m3_hint *h,
    char *err, size_t errlen)
{
	const struct zr_m3_chunk *c;
	struct diff_data dl, dr;
	struct diff_result *r = NULL;
	unsigned int i;
	uint32_t fn, on, fc = 0, oc = 0, k;

	memset(h, 0, sizeof (*h));
	if (chunk >= m->nchunks ||
	    m->chunks[chunk].kind != ZR_M3_CONFLICT) {
		m3_fail(err, errlen, "that chunk is not a conflict");
		return (-1);
	}
	c = &m->chunks[chunk];
	fn = c->from_hi - c->from_lo;
	on = c->onto_hi - c->onto_lo;
	h->from_marks = calloc(fn != 0 ? fn : 1, 1);
	h->onto_marks = calloc(on != 0 ? on : 1, 1);
	if (h->from_marks == NULL || h->onto_marks == NULL) {
		m3_fail(err, errlen, "out of memory");
		goto fail;
	}
	h->from_n = fn;
	h->onto_n = on;
	memset(h->from_marks, ZR_M3_HINT_DIFF, fn);
	memset(h->onto_marks, ZR_M3_HINT_DIFF, on);
	if (fn == 0 || on == 0)
		return (0);
	if (m3_diff(m3_at(&m->from, c->from_lo),
	    m3_span(&m->from, c->from_lo, c->from_hi),
	    m3_at(&m->onto, c->onto_lo),
	    m3_span(&m->onto, c->onto_lo, c->onto_hi), &dl, &dr, &r, err,
	    errlen) != 0)
		goto fail;
	for (i = 0; i < r->chunks.len; i++) {
		const struct diff_chunk *dc = &r->chunks.head[i];
		enum diff_chunk_type t = diff_chunk_type(dc);

		if (t == CHUNK_SAME) {
			for (k = 0; k < dc->left_count && fc + k < fn; k++)
				h->from_marks[fc + k] = ZR_M3_HINT_SAME;
			for (k = 0; k < dc->right_count && oc + k < on; k++)
				h->onto_marks[oc + k] = ZR_M3_HINT_SAME;
		}
		fc += dc->left_count;
		oc += dc->right_count;
	}
	diff_result_free(r);
	diff_data_free(&dl);
	diff_data_free(&dr);
	return (0);
fail:
	zr_m3_hint_fini(h);
	return (-1);
}

void
zr_m3_hint_fini(struct zr_m3_hint *h)
{
	free(h->from_marks);
	free(h->onto_marks);
	memset(h, 0, sizeof (*h));
}

void
zr_m3_fini(struct zr_m3 *m)
{
	free(m->base.lines);
	free(m->from.lines);
	free(m->onto.lines);
	free(m->chunks);
	memset(m, 0, sizeof (*m));
}
