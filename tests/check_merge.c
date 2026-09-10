/*
 * check_merge: the three-way text merge held to merge-theory's
 * exported battery and to the properties of v4-merge3.md section 7.
 *
 * Cells ZP79, ZP80, ZP81, ZP89, ZP91, ZP95, ZP96, ZP97, ZP98, ZP99
 * and ZP104 to ZP109, plus the merge-library half of ZP78 and ZP82.
 * The key-driven and screen-driven cells of family ZP -- ZP83 to
 * ZP88, ZP90, ZP92, ZP93, ZP94 -- are picker-merge's, over the model:
 * this program links no curses, opens no terminal and writes no file.
 * It reads tests/battery/merge/merge3.txt (or the path given as
 * argv[1]) with strncmp and sscanf, the way tests/battery/README.md
 * describes, and for every case:
 *
 *   - runs zr_m3_open over the three files;
 *   - for a case marked exact, compares the chunk sequence to the one
 *     the battery states (ZP104);
 *   - for every case, asserts all six properties of section 7 and the
 *     five kind invariants that go with them (ZP105 to ZP108, and
 *     ZP109 for the paper's counterexamples, which are property-only
 *     cases and are asserted as such).
 *
 * Then the shapes a battery case cannot carry: the binary rule, the
 * write rule, the missing final newline in each position, the add/add
 * form with no base, delete/edit refused, the hint, and a result that
 * comes out byte-identical to one side.
 *
 * usage: check_merge [BATTERY]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugins/picker/merge.h"

#define	CHECK(x)							\
	do {								\
		checks++;						\
		if (!(x)) {						\
			printf("%s:%d: check failed: %s\n", __FILE__,	\
			    __LINE__, #x);				\
			exit(1);					\
		}							\
	} while (0)

#define	LINEMAX		512
#define	FILEMAX		8192
#define	CHUNKMAX	128
#define	NAMEMAX		64

#define	BATTERY_DEFAULT	"tests/battery/merge/merge3.txt"

static int checks;
static int cases;
static int exact_cases;

static const char *kindnames[] = {
	"stable", "from-only", "onto-only", "both-same", "conflict"
};

/* One file of a case, rebuilt from its line records. */
struct fdata {
	unsigned char	bytes[FILEMAX];
	size_t		len;
};

struct kase {
	char			name[NAMEMAX];
	int			exact;
	int			haschunks;
	struct fdata		base;
	struct fdata		from;
	struct fdata		onto;
	struct zr_m3_chunk	want[CHUNKMAX];
	uint32_t		nwant;
};

static const char *
kindname(enum zr_m3_kind k)
{
	return (kindnames[(int)k]);
}

static void
fput(struct fdata *f, const char *text, int eol)
{
	size_t n = strlen(text);

	CHECK(f->len + n + 1 <= FILEMAX);
	memcpy(f->bytes + f->len, text, n);
	f->len += n;
	if (eol)
		f->bytes[f->len++] = '\n';
}

/* Read one line, minus its newline. Returns 0 at end of file. */
static int
getln(FILE *fp, char *buf, size_t sz)
{
	size_t n;

	if (fgets(buf, (int)sz, fp) == NULL)
		return (0);
	n = strlen(buf);
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return (1);
}

/*
 * A file section: "base N", "from N" or "onto N", then exactly N line
 * records. A record is one letter, a space, then the line's text; a
 * record of length 1 is an empty line, and the letter says whether the
 * line ends with a newline (L) or does not (X, which is only ever a
 * file's last line).
 */
static void
read_file_lines(FILE *fp, const char *tag, struct fdata *f)
{
	char line[LINEMAX];
	unsigned int n = 0, i;

	CHECK(getln(fp, line, sizeof (line)));
	CHECK(strncmp(line, tag, strlen(tag)) == 0);
	CHECK(sscanf(line + strlen(tag), " %u", &n) == 1);
	memset(f, 0, sizeof (*f));
	for (i = 0; i < n; i++) {
		CHECK(getln(fp, line, sizeof (line)));
		CHECK(line[0] == 'L' || line[0] == 'X');
		fput(f, strlen(line) > 1 ? line + 2 : "", line[0] == 'L');
	}
}

static enum zr_m3_kind
parse_kind(const char *s)
{
	int i;

	for (i = 0; i < 5; i++)
		if (strcmp(s, kindnames[i]) == 0)
			return ((enum zr_m3_kind)i);
	printf("bad chunk kind: %s\n", s);
	exit(1);
}

/* One case, or 0 at end of file. */
static int
read_case(FILE *fp, struct kase *k)
{
	char line[LINEMAX];
	char mode[NAMEMAX];
	unsigned int n, i;

	do {
		if (!getln(fp, line, sizeof (line)))
			return (0);
	} while (line[0] == '#' || line[0] == '\0');
	memset(k, 0, sizeof (*k));
	CHECK(strncmp(line, "case ", 5) == 0);
	CHECK(sscanf(line, "case %63s %63s", k->name, mode) == 2);
	k->exact = strcmp(mode, "exact") == 0;
	read_file_lines(fp, "base", &k->base);
	read_file_lines(fp, "from", &k->from);
	read_file_lines(fp, "onto", &k->onto);
	CHECK(getln(fp, line, sizeof (line)));
	if (strncmp(line, "chunks ", 7) == 0) {
		CHECK(sscanf(line, "chunks %u", &n) == 1);
		CHECK(n <= CHUNKMAX);
		k->haschunks = 1;
		k->nwant = n;
		for (i = 0; i < n; i++) {
			char kind[NAMEMAX];
			struct zr_m3_chunk *c = &k->want[i];
			unsigned int r[6];

			CHECK(getln(fp, line, sizeof (line)));
			CHECK(sscanf(line, "%63s %u %u %u %u %u %u", kind,
			    &r[0], &r[1], &r[2], &r[3], &r[4], &r[5]) == 7);
			c->kind = parse_kind(kind);
			c->base_lo = r[0];
			c->base_hi = r[1];
			c->from_lo = r[2];
			c->from_hi = r[3];
			c->onto_lo = r[4];
			c->onto_hi = r[5];
		}
		CHECK(getln(fp, line, sizeof (line)));
	}
	CHECK(strcmp(line, "end") == 0);
	/* The flag and the section must agree (README, and section 8). */
	CHECK(k->exact == k->haschunks);
	return (1);
}

/* ---- the properties ------------------------------------------- */

static const struct zr_m3_file *
sideof(const struct zr_m3 *m, int which)
{
	if (which == 0)
		return (&m->base);
	if (which == 1)
		return (&m->from);
	return (&m->onto);
}

static void
rangeof(const struct zr_m3_chunk *c, int which, uint32_t *lo, uint32_t *hi)
{
	if (which == 0) {
		*lo = c->base_lo;
		*hi = c->base_hi;
	} else if (which == 1) {
		*lo = c->from_lo;
		*hi = c->from_hi;
	} else {
		*lo = c->onto_lo;
		*hi = c->onto_hi;
	}
}

/* Are two ranges of records the same lines? */
static int
same_lines(const struct zr_m3_file *a, uint32_t alo, uint32_t ahi,
    const struct zr_m3_file *b, uint32_t blo, uint32_t bhi)
{
	uint32_t i;

	if (ahi - alo != bhi - blo)
		return (0);
	for (i = 0; i < ahi - alo; i++) {
		const struct zr_m3_line *x = &a->lines[alo + i];
		const struct zr_m3_line *y = &b->lines[blo + i];

		if (x->len != y->len)
			return (0);
		if (memcmp(a->bytes + x->off, b->bytes + y->off, x->len) != 0)
			return (0);
	}
	return (1);
}

/*
 * Property 6: the ranges of each file partition it, in order, with no
 * gap and no overlap. Property 5, read as lines: the concatenation of
 * one file's ranges over every chunk gives that file back exactly --
 * which for a conflict chunk means taking its from range, so this is
 * the true form of "every conflict picked from", and NOT a claim about
 * the merged result (v4-merge3.md section 7, the note under 5).
 */
static void
check_partition(const struct zr_m3 *m, const char *name)
{
	int which;

	for (which = 0; which < 3; which++) {
		const struct zr_m3_file *f = sideof(m, which);
		uint32_t pos = 0, i, lo, hi;
		size_t at = 0;

		for (i = 0; i < m->nchunks; i++) {
			rangeof(&m->chunks[i], which, &lo, &hi);
			if (lo != pos || hi < lo) {
				printf("%s: chunk %lu does not partition "
				    "file %d\n", name, (unsigned long)i,
				    which);
				exit(1);
			}
			while (lo < hi) {
				const struct zr_m3_line *r = &f->lines[lo];

				if (at + r->len > f->len ||
				    memcmp(f->bytes + at, f->bytes + r->off,
				    r->len) != 0) {
					printf("%s: file %d is not rebuilt "
					    "by its ranges\n", name, which);
					exit(1);
				}
				at += r->len;
				lo++;
			}
			pos = hi;
		}
		CHECK(pos == f->nlines);
		CHECK(at == f->len);
	}
}

/* The five invariants the kinds carry (section 7, the closing list). */
static void
check_kinds(const struct zr_m3 *m, const char *name)
{
	uint32_t i;

	for (i = 0; i < m->nchunks; i++) {
		const struct zr_m3_chunk *c = &m->chunks[i];
		int bf = same_lines(&m->base, c->base_lo, c->base_hi,
		    &m->from, c->from_lo, c->from_hi);
		int bo = same_lines(&m->base, c->base_lo, c->base_hi,
		    &m->onto, c->onto_lo, c->onto_hi);
		int fo = same_lines(&m->from, c->from_lo, c->from_hi,
		    &m->onto, c->onto_lo, c->onto_hi);
		int ok = 1;

		switch (c->kind) {
		case ZR_M3_STABLE:
			ok = m->has_base ? (bf && bo && fo) : fo;
			break;
		case ZR_M3_FROM:
			ok = bo && !bf;
			break;
		case ZR_M3_ONTO:
			ok = bf && !bo;
			break;
		case ZR_M3_SAME:
			ok = fo && !bf;
			break;
		case ZR_M3_CONFLICT:
		default:
			ok = !fo;
			break;
		}
		if (!ok) {
			printf("%s: chunk %lu is a dishonest %s\n", name,
			    (unsigned long)i, kindname(c->kind));
			exit(1);
		}
	}
}

static void
show(const struct zr_m3 *m)
{
	uint32_t i;

	for (i = 0; i < m->nchunks; i++) {
		const struct zr_m3_chunk *c = &m->chunks[i];

		printf("  %s %lu %lu %lu %lu %lu %lu\n", kindname(c->kind),
		    (unsigned long)c->base_lo, (unsigned long)c->base_hi,
		    (unsigned long)c->from_lo, (unsigned long)c->from_hi,
		    (unsigned long)c->onto_lo, (unsigned long)c->onto_hi);
	}
}

/* Property 4: swapping the sides swaps the chunks and nothing else. */
static void
check_swap(const struct kase *k, const struct zr_m3 *m)
{
	struct zr_m3 sw;
	char err[256];
	uint32_t i;

	CHECK(zr_m3_open(&sw, k->base.bytes, k->base.len, k->onto.bytes,
	    k->onto.len, k->from.bytes, k->from.len, err,
	    sizeof (err)) == 0);
	if (sw.nchunks != m->nchunks) {
		printf("%s: swapping changes the number of chunks\n", k->name);
		exit(1);
	}
	for (i = 0; i < m->nchunks; i++) {
		const struct zr_m3_chunk *a = &m->chunks[i];
		const struct zr_m3_chunk *b = &sw.chunks[i];
		enum zr_m3_kind want = a->kind;

		if (want == ZR_M3_FROM)
			want = ZR_M3_ONTO;
		else if (want == ZR_M3_ONTO)
			want = ZR_M3_FROM;
		if (b->kind != want || b->base_lo != a->base_lo ||
		    b->base_hi != a->base_hi || b->from_lo != a->onto_lo ||
		    b->from_hi != a->onto_hi || b->onto_lo != a->from_lo ||
		    b->onto_hi != a->from_hi) {
			printf("%s: swapping changes chunk %lu\n", k->name,
			    (unsigned long)i);
			show(m);
			show(&sw);
			exit(1);
		}
	}
	zr_m3_fini(&sw);
}

/* The merged bytes with every conflict taken from one named side. */
static unsigned char *
resolve(struct zr_m3 *m, int side, size_t *len)
{
	unsigned char *out = NULL;
	char err[256];
	uint32_t i;

	for (i = 0; i < m->nchunks; i++)
		if (m->chunks[i].kind == ZR_M3_CONFLICT)
			zr_m3_pick(m, i, side);
	CHECK(zr_m3_unpicked(m) == 0);
	CHECK(zr_m3_result(m, &out, len, err, sizeof (err)) == 0);
	CHECK(out != NULL);
	return (out);
}

/*
 * Properties 1, 2 and 3: a side that changed nothing merges to the
 * other, and the same change on both sides merges to that change, in
 * each case with no conflict at all.
 */
static void
check_unchanged(const struct kase *k, struct zr_m3 *m)
{
	unsigned char *out;
	size_t len;
	char err[256];

	if (m->nconflict == 0) {
		CHECK(zr_m3_result(m, &out, &len, err, sizeof (err)) == 0);
		free(out);
	}
	if (k->from.len == k->base.len &&
	    memcmp(k->from.bytes, k->base.bytes, k->base.len) == 0) {
		CHECK(m->nconflict == 0);
		out = resolve(m, ZR_M3_PICK_FROM, &len);
		CHECK(len == k->onto.len);
		CHECK(memcmp(out, k->onto.bytes, len) == 0);
		free(out);
	}
	if (k->onto.len == k->base.len &&
	    memcmp(k->onto.bytes, k->base.bytes, k->base.len) == 0) {
		CHECK(m->nconflict == 0);
		out = resolve(m, ZR_M3_PICK_FROM, &len);
		CHECK(len == k->from.len);
		CHECK(memcmp(out, k->from.bytes, len) == 0);
		free(out);
	}
	if (k->from.len == k->onto.len &&
	    memcmp(k->from.bytes, k->onto.bytes, k->from.len) == 0) {
		CHECK(m->nconflict == 0);
		out = resolve(m, ZR_M3_PICK_FROM, &len);
		CHECK(len == k->from.len);
		CHECK(memcmp(out, k->from.bytes, len) == 0);
		free(out);
	}
}

/*
 * The hint never changes the chunk sequence (ZP89): compute it on
 * every conflict chunk and hold the sequence against a copy taken
 * before.
 */
static void
check_hint(struct zr_m3 *m, const char *name)
{
	struct zr_m3_chunk before[CHUNKMAX];
	struct zr_m3_hint h;
	char err[256];
	uint32_t i;

	if (m->nchunks == 0 || m->nchunks > CHUNKMAX)
		return;
	memcpy(before, m->chunks, m->nchunks * sizeof (*m->chunks));
	for (i = 0; i < m->nchunks; i++) {
		if (m->chunks[i].kind != ZR_M3_CONFLICT)
			continue;
		CHECK(zr_m3_hint(m, i, &h, err, sizeof (err)) == 0);
		CHECK(h.from_n == m->chunks[i].from_hi - m->chunks[i].from_lo);
		CHECK(h.onto_n == m->chunks[i].onto_hi - m->chunks[i].onto_lo);
		zr_m3_hint_fini(&h);
		CHECK(h.from_marks == NULL && h.onto_marks == NULL);
	}
	if (memcmp(before, m->chunks, m->nchunks * sizeof (*m->chunks)) != 0) {
		printf("%s: the hint moved the chunk sequence\n", name);
		exit(1);
	}
}

static void
run_case(const struct kase *k)
{
	struct zr_m3 m;
	char err[256];
	uint32_t i;

	CHECK(zr_m3_open(&m, k->base.bytes, k->base.len, k->from.bytes,
	    k->from.len, k->onto.bytes, k->onto.len, err,
	    sizeof (err)) == 0);
	if (k->exact) {
		exact_cases++;
		if (m.nchunks != k->nwant) {
			printf("%s: %lu chunks, the battery says %lu\n",
			    k->name, (unsigned long)m.nchunks,
			    (unsigned long)k->nwant);
			show(&m);
			exit(1);
		}
		for (i = 0; i < m.nchunks; i++) {
			const struct zr_m3_chunk *a = &m.chunks[i];
			const struct zr_m3_chunk *b = &k->want[i];

			if (a->kind != b->kind || a->base_lo != b->base_lo ||
			    a->base_hi != b->base_hi ||
			    a->from_lo != b->from_lo ||
			    a->from_hi != b->from_hi ||
			    a->onto_lo != b->onto_lo ||
			    a->onto_hi != b->onto_hi) {
				printf("%s: chunk %lu differs from the "
				    "battery\n", k->name, (unsigned long)i);
				show(&m);
				exit(1);
			}
		}
	}
	check_partition(&m, k->name);
	check_kinds(&m, k->name);
	check_swap(k, &m);
	check_hint(&m, k->name);
	check_unchanged(k, &m);
	zr_m3_fini(&m);
	cases++;
}

static void
run_battery(const char *path)
{
	FILE *fp = fopen(path, "r");
	struct kase *k;

	if (fp == NULL) {
		perror(path);
		printf("check_merge: the battery is "
		    BATTERY_DEFAULT ", or argv[1]\n");
		exit(1);
	}
	k = malloc(sizeof (*k));
	CHECK(k != NULL);
	while (read_case(fp, k))
		run_case(k);
	free(k);
	(void) fclose(fp);
	CHECK(cases > 0);
	printf("ok   %s (%d cases, %d exact)\n", path, cases, exact_cases);
}

/* ---- what a battery case cannot carry -------------------------- */

/*
 * The binary rule, git's: a NUL byte in the first 8000 bytes makes an
 * object binary, one after them does not, and an empty object is text.
 */
static void
check_text(void)
{
	static unsigned char big[9000];
	struct zr_m3 m;
	char err[256];

	memset(big, 'x', sizeof (big));
	CHECK(zr_m3_text((const unsigned char *)"abc\n", 4) == 1);
	CHECK(zr_m3_text((const unsigned char *)"", 0) == 1);
	CHECK(zr_m3_text(NULL, 0) == 1);
	CHECK(zr_m3_text(big, sizeof (big)) == 1);
	big[0] = '\0';
	CHECK(zr_m3_text(big, sizeof (big)) == 0);
	big[0] = 'x';
	big[7999] = '\0';
	CHECK(zr_m3_text(big, sizeof (big)) == 0);
	big[7999] = 'x';
	big[8000] = '\0';
	CHECK(zr_m3_text(big, sizeof (big)) == 1);
	big[8000] = 'x';

	/* And zr_m3_open refuses before libdiff is asked for anything. */
	err[0] = '\0';
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\n", 2,
	    (const unsigned char *)"a\0b\n", 4, (const unsigned char *)"a\n",
	    2, err, sizeof (err)) == -1);
	CHECK(strstr(err, "binary") != NULL);
	err[0] = '\0';
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\0", 2,
	    (const unsigned char *)"a\n", 2, (const unsigned char *)"a\n", 2,
	    err, sizeof (err)) == -1);
	CHECK(strstr(err, "binary") != NULL);
	err[0] = '\0';
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\n", 2,
	    (const unsigned char *)"a\n", 2, (const unsigned char *)"a\0", 2,
	    err, sizeof (err)) == -1);
	CHECK(strstr(err, "binary") != NULL);
}

/*
 * The write rule (ruling of 2026-09-10, ZP91): the result is refused
 * while any conflict chunk is unpicked, and the refusal names the
 * first; picking every conflict makes it writable; a merge with no
 * conflict is writable at once. No marker reaches the bytes by any
 * path, because there is no path here that writes one.
 */
static void
check_write_rule(void)
{
	struct zr_m3 m;
	unsigned char *out;
	size_t len;
	char err[256];
	uint32_t first;

	/* Two conflicts, far enough apart to stay two chunks. */
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\nb\nc\nd\ne\n", 10,
	    (const unsigned char *)"A\nb\nc\nd\nE\n", 10,
	    (const unsigned char *)"x\nb\nc\nd\ny\n", 10, err,
	    sizeof (err)) == 0);
	CHECK(m.nconflict == 2);
	CHECK(zr_m3_unpicked(&m) == 2);
	CHECK(zr_m3_first_unpicked(&m, &first) == 0);
	CHECK(first == 0);
	CHECK(m.chunks[first].kind == ZR_M3_CONFLICT);
	err[0] = '\0';
	CHECK(zr_m3_result(&m, &out, &len, err, sizeof (err)) == -1);
	CHECK(out == NULL);
	CHECK(strstr(err, "chunk 0") != NULL);
	CHECK(strstr(err, "conflict") != NULL);

	/* One of two picked is still not writable. */
	zr_m3_pick(&m, first, ZR_M3_PICK_FROM);
	CHECK(zr_m3_unpicked(&m) == 1);
	CHECK(zr_m3_result(&m, &out, &len, err, sizeof (err)) == -1);

	/* Taking a pick back puts it back where it was. */
	zr_m3_pick(&m, first, ZR_M3_PICK_NONE);
	CHECK(zr_m3_unpicked(&m) == 2);

	/* The last pick on a chunk stands. */
	zr_m3_pick(&m, first, ZR_M3_PICK_FROM);
	zr_m3_pick(&m, first, ZR_M3_PICK_ONTO);
	CHECK(m.chunks[first].pick == ZR_M3_PICK_ONTO);

	out = resolve(&m, ZR_M3_PICK_FROM, &len);
	CHECK(len == 10 && memcmp(out, "A\nb\nc\nd\nE\n", 10) == 0);
	CHECK(memchr(out, '<', len) == NULL);
	CHECK(memchr(out, '=', len) == NULL);
	CHECK(memchr(out, '>', len) == NULL);
	CHECK(memchr(out, '|', len) == NULL);
	free(out);
	out = resolve(&m, ZR_M3_PICK_ONTO, &len);
	CHECK(len == 10 && memcmp(out, "x\nb\nc\nd\ny\n", 10) == 0);
	free(out);
	zr_m3_fini(&m);

	/* A merge with no conflict is writable with no key pressed. */
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\nb\nc\n", 6,
	    (const unsigned char *)"A\nb\nc\n", 6,
	    (const unsigned char *)"a\nb\nC\n", 6, err,
	    sizeof (err)) == 0);
	CHECK(m.nconflict == 0);
	CHECK(zr_m3_unpicked(&m) == 0);
	CHECK(zr_m3_first_unpicked(&m, &first) == -1);
	CHECK(zr_m3_result(&m, &out, &len, err, sizeof (err)) == 0);
	CHECK(len == 6 && memcmp(out, "A\nb\nC\n", 6) == 0);
	free(out);
	zr_m3_fini(&m);
}

/* One merge, resolved one way, as bytes. */
static void
merged(const char *base, size_t baselen, const char *from, size_t fromlen,
    const char *onto, size_t ontolen, int side, const char *want,
    size_t wantlen)
{
	struct zr_m3 m;
	unsigned char *out;
	size_t len;
	char err[256];

	CHECK(zr_m3_open(&m, (const unsigned char *)base, baselen,
	    (const unsigned char *)from, fromlen,
	    (const unsigned char *)onto, ontolen, err, sizeof (err)) == 0);
	out = resolve(&m, side, &len);
	if (len != wantlen || memcmp(out, want, len) != 0) {
		printf("merged: got %d bytes, wanted %d\n", (int)len,
		    (int)wantlen);
		exit(1);
	}
	free(out);
	zr_m3_fini(&m);
}

/*
 * ZP97: a missing final newline is a fact about the last line and is
 * kept in the result, in each of the three positions and on both
 * halves of a conflict. ZP99: a result that is byte-identical to one
 * side is produced all the same.
 */
static void
check_final_newline(void)
{
	/* base lacks it, both sides have it: the result has it. */
	merged("a\nb", 3, "a\nb\n", 4, "a\nb", 3, ZR_M3_PICK_FROM,
	    "a\nb\n", 4);
	/* from lacks it, onto did not change: the result lacks it. */
	merged("a\nb\n", 4, "a\nb", 3, "a\nb\n", 4, ZR_M3_PICK_FROM,
	    "a\nb", 3);
	/* onto lacks it, from did not change: the result lacks it. */
	merged("a\nb\n", 4, "a\nb\n", 4, "a\nb", 3, ZR_M3_PICK_FROM,
	    "a\nb", 3);
	/* both dropped it, for different text: a conflict either way. */
	merged("a\nb\n", 4, "a\nX", 3, "a\nY", 3, ZR_M3_PICK_FROM,
	    "a\nX", 3);
	merged("a\nb\n", 4, "a\nX", 3, "a\nY", 3, ZR_M3_PICK_ONTO,
	    "a\nY", 3);
	/* One line with none, one side adding one. */
	merged("a", 1, "a\n", 2, "a", 1, ZR_M3_PICK_FROM, "a\n", 2);

	/* ZP99: the result is from exactly, and is produced. */
	merged("a\n", 2, "b\n", 2, "c\n", 2, ZR_M3_PICK_FROM, "b\n", 2);
	merged("a\nb\nc\n", 6, "a\nB\nc\n", 6, "a\nb\nc\n", 6,
	    ZR_M3_PICK_FROM, "a\nB\nc\n", 6);

	/* ZP98: an empty from, an empty onto, an empty base. */
	merged("a\nb\n", 4, "", 0, "a\nb\n", 4, ZR_M3_PICK_FROM, "", 0);
	merged("a\nb\n", 4, "a\nb\n", 4, "", 0, ZR_M3_PICK_FROM, "", 0);
	merged("a\nb\n", 4, "", 0, "", 0, ZR_M3_PICK_FROM, "", 0);
	merged("", 0, "a\n", 2, "", 0, ZR_M3_PICK_FROM, "a\n", 2);
	merged("", 0, "", 0, "", 0, ZR_M3_PICK_FROM, "", 0);
}

/*
 * ZP95: add/add has no base to anchor against, so it is a two-way
 * compare of from against onto with a pick, under the same write rule,
 * and every chunk's base range is empty. An empty base OBJECT is a
 * different thing and goes through the walk, which is why the battery
 * keeps the empty-base cases.
 */
static void
check_add_add(void)
{
	struct zr_m3 m;
	unsigned char *out;
	size_t len;
	char err[256];
	uint32_t i, first;

	CHECK(zr_m3_open(&m, NULL, 0, (const unsigned char *)"a\nb\nz\n", 6,
	    (const unsigned char *)"a\nc\nz\n", 6, err, sizeof (err)) == 0);
	CHECK(m.has_base == 0);
	CHECK(m.base.nlines == 0);
	CHECK(m.nconflict == 1);
	for (i = 0; i < m.nchunks; i++) {
		CHECK(m.chunks[i].base_lo == 0 && m.chunks[i].base_hi == 0);
		CHECK(m.chunks[i].kind == ZR_M3_STABLE ||
		    m.chunks[i].kind == ZR_M3_CONFLICT);
	}
	check_partition(&m, "add/add");
	check_kinds(&m, "add/add");
	check_hint(&m, "add/add");
	CHECK(zr_m3_first_unpicked(&m, &first) == 0);
	CHECK(zr_m3_result(&m, &out, &len, err, sizeof (err)) == -1);
	out = resolve(&m, ZR_M3_PICK_ONTO, &len);
	CHECK(len == 6 && memcmp(out, "a\nc\nz\n", 6) == 0);
	free(out);
	zr_m3_fini(&m);

	/* The two sides agreeing: no conflict, writable at once. */
	CHECK(zr_m3_open(&m, NULL, 0, (const unsigned char *)"a\nb\n", 4,
	    (const unsigned char *)"a\nb\n", 4, err, sizeof (err)) == 0);
	CHECK(m.nconflict == 0);
	CHECK(zr_m3_result(&m, &out, &len, err, sizeof (err)) == 0);
	CHECK(len == 4 && memcmp(out, "a\nb\n", 4) == 0);
	free(out);
	zr_m3_fini(&m);

	/* Two empty sides with no base: nothing to say. */
	CHECK(zr_m3_open(&m, NULL, 0, (const unsigned char *)"", 0,
	    (const unsigned char *)"", 0, err, sizeof (err)) == 0);
	CHECK(m.nchunks == 0 && m.nconflict == 0);
	CHECK(zr_m3_result(&m, &out, &len, err, sizeof (err)) == 0);
	CHECK(len == 0);
	free(out);
	zr_m3_fini(&m);
}

/*
 * ZP96: delete/edit is a choice and not a merge. One side has no
 * object at all, so there are not three files to merge and nothing
 * opens; the row takes from or onto or keep like any other.
 */
static void
check_delete_edit(void)
{
	struct zr_m3 m;
	char err[256];

	err[0] = '\0';
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\n", 2, NULL, 0,
	    (const unsigned char *)"A\n", 2, err, sizeof (err)) == -1);
	CHECK(strstr(err, "choice") != NULL);
	CHECK(strstr(err, "merge") != NULL);
	err[0] = '\0';
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\n", 2,
	    (const unsigned char *)"A\n", 2, NULL, 0, err,
	    sizeof (err)) == -1);
	CHECK(strstr(err, "choice") != NULL);
	err[0] = '\0';
	CHECK(zr_m3_open(&m, NULL, 0, (const unsigned char *)"A\n", 2, NULL,
	    0, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0');
}

/*
 * ZP89, in detail: the hint marks which lines inside a conflict chunk
 * the two sides share, and marks nothing outside the chunk. It is
 * computed on demand and thrown away; the chunk sequence is held
 * against a copy by check_hint, which every battery case runs.
 */
static void
check_hint_marks(void)
{
	struct zr_m3 m;
	struct zr_m3_hint h;
	char err[256];
	uint32_t i, conflict = 0;
	int found = 0;

	/*
	 * One conflict whose two halves share their middle line: the
	 * diff of the halves finds it, and the chunk stays one chunk.
	 */
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\nz\n", 4,
	    (const unsigned char *)"a\np\nq\nr\nz\n", 10,
	    (const unsigned char *)"a\ns\nq\nt\nz\n", 10, err,
	    sizeof (err)) == 0);
	for (i = 0; i < m.nchunks; i++)
		if (m.chunks[i].kind == ZR_M3_CONFLICT) {
			conflict = i;
			found++;
		}
	CHECK(found == 1);
	CHECK(zr_m3_hint(&m, conflict, &h, err, sizeof (err)) == 0);
	CHECK(h.from_n == 3 && h.onto_n == 3);
	CHECK(h.from_marks[0] == ZR_M3_HINT_DIFF);
	CHECK(h.from_marks[1] == ZR_M3_HINT_SAME);
	CHECK(h.from_marks[2] == ZR_M3_HINT_DIFF);
	CHECK(h.onto_marks[0] == ZR_M3_HINT_DIFF);
	CHECK(h.onto_marks[1] == ZR_M3_HINT_SAME);
	CHECK(h.onto_marks[2] == ZR_M3_HINT_DIFF);
	zr_m3_hint_fini(&h);

	/* A chunk that is not a conflict has no hint to give. */
	for (i = 0; i < m.nchunks; i++)
		if (m.chunks[i].kind != ZR_M3_CONFLICT) {
			err[0] = '\0';
			CHECK(zr_m3_hint(&m, i, &h, err,
			    sizeof (err)) == -1);
			CHECK(err[0] != '\0');
		}
	err[0] = '\0';
	CHECK(zr_m3_hint(&m, m.nchunks, &h, err, sizeof (err)) == -1);
	zr_m3_fini(&m);

	/* A conflict with an empty half: no marks on that side. */
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\nb\nz\n", 6,
	    (const unsigned char *)"a\nz\n", 4,
	    (const unsigned char *)"a\nB\nz\n", 6, err,
	    sizeof (err)) == 0);
	CHECK(m.nconflict == 1);
	for (i = 0; i < m.nchunks; i++)
		if (m.chunks[i].kind == ZR_M3_CONFLICT)
			conflict = i;
	CHECK(zr_m3_hint(&m, conflict, &h, err, sizeof (err)) == 0);
	CHECK(h.from_n == 0);
	CHECK(h.onto_n == 1);
	CHECK(h.onto_marks[0] == ZR_M3_HINT_DIFF);
	zr_m3_hint_fini(&h);
	zr_m3_fini(&m);
}

int
main(int argc, char **argv)
{
	run_battery(argc > 1 ? argv[1] : BATTERY_DEFAULT);
	check_text();
	check_write_rule();
	check_final_newline();
	check_add_add();
	check_delete_edit();
	check_hint_marks();
	printf("check_merge: %d checks passed\n", checks);
	return (0);
}
