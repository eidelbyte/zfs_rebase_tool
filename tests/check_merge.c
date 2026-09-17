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

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
/*
 * ZP132: the line ceiling of merge.h, which is what keeps the carried
 * patience diff's unchecked allocations out of reach (G2 with G1). A
 * file of one line over it is refused before libdiff is asked for
 * anything, with the count and the ceiling in the line; one line under
 * it is not refused for being too long.
 *
 * The file is built here and never checked in: four million newlines
 * is four megabytes of bytes and thirty-two of line table, which this
 * allocates and frees. Where the memory is not there the case says so
 * and passes, since what it is about is the refusal and not the
 * machine.
 */
static void
check_ceiling(void)
{
	size_t n = (size_t)ZR_M3_MAXLINES + 1;
	unsigned char *big;
	struct zr_m3 m;
	char err[256];

	big = malloc(n);
	if (big == NULL) {
		printf("skip ZP132: no room for %lu lines\n",
		    (unsigned long)n);
		return;
	}
	memset(big, '\n', n);
	err[0] = '\0';
	CHECK(zr_m3_open(&m, big, n, (const unsigned char *)"a\n", 2,
	    (const unsigned char *)"b\n", 2, err, sizeof (err)) == -1);
	CHECK(strstr(err, "too large") != NULL);
	CHECK(strstr(err, "4000001") != NULL);
	CHECK(strstr(err, "4000000") != NULL);
	/* and from, and onto: every side is held to it */
	err[0] = '\0';
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\n", 2, big, n,
	    (const unsigned char *)"b\n", 2, err, sizeof (err)) == -1);
	CHECK(strstr(err, "too large") != NULL);
	err[0] = '\0';
	CHECK(zr_m3_open(&m, (const unsigned char *)"a\n", 2,
	    (const unsigned char *)"b\n", 2, big, n, err,
	    sizeof (err)) == -1);
	CHECK(strstr(err, "too large") != NULL);
	/*
	 * The line under the ceiling is not refused for its length.
	 * The merge itself is not run here -- three files of four
	 * million lines is what the ceiling exists to bound -- so this
	 * asks the split alone, with two tiny sides against a base
	 * that is exactly at the ceiling, and only that the refusal
	 * does not name the size.
	 */
	err[0] = '\0';
	if (zr_m3_open(&m, big, n - 1, (const unsigned char *)"a\n", 2,
	    (const unsigned char *)"b\n", 2, err, sizeof (err)) == 0)
		zr_m3_fini(&m);
	CHECK(strstr(err, "too large") == NULL);
	free(big);
}

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
 * The properties the add/add form claims, and the ones it does not.
 *
 * Ruled by the author on 2026-09-15, on the review's finding G3: "the
 * two-way shortcut is fine, since the user specifies from and onto,
 * we'll just always do diff(from, onto)". So the sequence is the
 * two-way diff of from against onto in that order, it is finer than
 * the walk over an empty base object would give, and it is not the
 * mirror of itself under a swap -- from and onto are the person's
 * names for two sides of a rebase and not two interchangeable files
 * (v4-merge3.md section 5).
 *
 * What it does claim, and what this asserts:
 *
 *   1. every conflict picked from gives from's lines exactly;
 *   2. every conflict picked onto gives onto's lines exactly;
 *   3. the lines the two sides share are stable chunks, and a stable
 *      chunk's from and onto ranges hold the same lines, so a shared
 *      line appears once and not twice;
 *   4. every chunk's base range is empty, since there is no base.
 *
 * check_swap is NOT applied here, deliberately: property 4 of
 * v4-merge3.md section 7 is stated over merge(base, from, onto) and
 * the ruling above puts the add/add form outside it. Calling it here
 * would be asserting the one thing the ruling says is not true.
 */
static void
check_add_add_props(const char *tag, const char *from, size_t flen,
    const char *onto, size_t olen)
{
	unsigned char *out;
	struct zr_m3 m;
	char err[256];
	size_t len;
	uint32_t i;

	CHECK(zr_m3_open(&m, NULL, 0, (const unsigned char *)from, flen,
	    (const unsigned char *)onto, olen, err, sizeof (err)) == 0);
	CHECK(m.has_base == 0);
	for (i = 0; i < m.nchunks; i++) {
		const struct zr_m3_chunk *c = &m.chunks[i];

		/* 4: no base, so no base range */
		CHECK(c->base_lo == 0 && c->base_hi == 0);
		/* the two-way compare has these two kinds and no other */
		CHECK(c->kind == ZR_M3_STABLE || c->kind == ZR_M3_CONFLICT);
		/* 3: a shared stretch is one stretch, the same on both sides */
		if (c->kind != ZR_M3_STABLE)
			continue;
		CHECK(c->from_hi - c->from_lo == c->onto_hi - c->onto_lo);
		CHECK(same_lines(&m.from, c->from_lo, c->from_hi, &m.onto,
		    c->onto_lo, c->onto_hi));
	}
	check_partition(&m, tag);
	check_kinds(&m, tag);
	/* 1 and 2: each side, taken whole, is that side */
	out = resolve(&m, ZR_M3_PICK_FROM, &len);
	if (len != flen || memcmp(out, from, len) != 0) {
		printf("%s: picking from does not give from\n", tag);
		exit(1);
	}
	checks++;
	free(out);
	out = resolve(&m, ZR_M3_PICK_ONTO, &len);
	if (len != olen || memcmp(out, onto, len) != 0) {
		printf("%s: picking onto does not give onto\n", tag);
		exit(1);
	}
	checks++;
	free(out);
	zr_m3_fini(&m);
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

	/* and the four properties the form claims, over a handful of pairs */
	check_add_add_props("add/add differ", "a\nb\nz\n", 6, "a\nc\nz\n",
	    6);
	check_add_add_props("add/add agree", "a\nb\n", 4, "a\nb\n", 4);
	check_add_add_props("add/add nothing shared", "x\ny\n", 4,
	    "p\nq\nr\n", 6);
	check_add_add_props("add/add one side empty", "a\nb\n", 4, "", 0);
	check_add_add_props("add/add both empty", "", 0, "", 0);
	check_add_add_props("add/add shared run", "s\nt\nu\nF\n", 8,
	    "s\nt\nu\nO\n", 8);
	check_add_add_props("add/add no final newline", "a\nb", 3, "a\nc", 3);
	/*
	 * And the one thing the ruling says is true of the shortcut and
	 * was denied by the note it replaced: the sequence is not the
	 * mirror of itself under a swap. The review's own example, whose
	 * shared line is b one way round and a the other.
	 */
	{
		struct zr_m3 one, two;
		char e1[256], e2[256];

		CHECK(zr_m3_open(&one, NULL, 0, (const unsigned char *)"a\nb\n",
		    4, (const unsigned char *)"b\na\n", 4, e1,
		    sizeof (e1)) == 0);
		CHECK(zr_m3_open(&two, NULL, 0, (const unsigned char *)"b\na\n",
		    4, (const unsigned char *)"a\nb\n", 4, e2,
		    sizeof (e2)) == 0);
		CHECK(one.nchunks == 3 && two.nchunks == 3);
		CHECK(one.chunks[1].kind == ZR_M3_STABLE);
		CHECK(two.chunks[1].kind == ZR_M3_STABLE);
		/* the shared line is b one way round and a the other */
		CHECK(same_lines(&one.from, one.chunks[1].from_lo,
		    one.chunks[1].from_hi, &two.from, two.chunks[1].from_lo,
		    two.chunks[1].from_hi) == 0);
		zr_m3_fini(&one);
		zr_m3_fini(&two);
	}
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

/*
 * ---------------------------------------------------------------
 * The oracle: generated cases, our merge, and an outside one.
 * ---------------------------------------------------------------
 *
 * Cell ZP141, and finding G8 of the code review of 2026-09-11: the
 * battery pins 45 cases and nothing in the tree held the C against an
 * outside implementation on anything else, nor merged anything over
 * eight kilobytes. tools/merge-oracle.sh keeps its own job, which is
 * the REFERENCE against git and diff3 over those 45; this is the TOOL
 * against an outside merger over a corpus that is generated and
 * therefore unbounded.
 *
 * THE CORPUS. One 32-bit generator of our own, seeded, so a case is
 * the same case on every machine and is reproducible from its seed
 * and number alone. A base of N lines, seven in eight of them a line
 * that occurs once and the rest drawn from a pool of four, so that
 * the alignment is forced in the common case and the ambiguous one is
 * still met; a line an edit writes is never from that pool, so an
 * edit's own text cannot be what two implementations align
 * differently. One case in eight ends without a final newline, in all
 * three files alike.
 *
 * Each case is given a SHAPE before it is generated -- clean, mixed,
 * or conflicted -- and the edit sites are placed to produce it, so
 * that the three boxes below each see about a third of the corpus
 * instead of whatever a per-stretch dice roll happens to give. A case
 * has one to four edit sites however long it is, with at least two
 * untouched lines between them and at each end, so a file of thirty
 * thousand lines is a realistic merge of a few edits and not a file
 * rewritten line by line. An edit is a replacement, an insertion, a
 * deletion or a block moved past the stretch after it. A fifth of the
 * cases have no base at all.
 *
 * THE AUTHORITY, and why it is chosen at run time. FreeBSD's diff3
 * lost lines in merge mode until 2026: with one side's edit changing
 * the line count and the merge otherwise clean, diff3 -m dropped as
 * many lines after the edited stretch as the count had moved. The
 * smallest case is base "a\nb\n", from the same bytes as base, onto
 * "a\nN\nb\n", where diff3 20220517 answers "a\nN\n" and loses the
 * line b. The tree fixed it in usr.bin/diff3: 2cfca8e710f2 "fix merge
 * mode" (2026-02-13), 5ddfd1db271c, which bumped the version string
 * to the date GNU compatibility was reached ("FreeBSD diff3
 * 20260213"), and fe5341287c6c "Produce correct exit status"
 * (2026-03-02). So the version string is what says whether the bytes
 * can be trusted, and this asks for it once:
 *
 *   - FreeBSD diff3 OR_DIFF3_FIXED or later, or GNU diffutils' diff3
 *     (which FreeBSD 15.1-RELEASE ships in base as /usr/bin/diff3,
 *     "diff3 (GNU diffutils) 2.8.7", and which the theory's battery
 *     was recorded against): diff3 is the byte authority.
 *   - otherwise git merge-file -p --diff3, where git is installed.
 *     --diff3 is what clamps git to its eager level, which is our
 *     ceiling (v4-merge3.md section 4), as tools/merge-oracle.sh's
 *     header explains.
 *   - otherwise no byte authority: the classification alone is
 *     asserted, and the run says so in its own words.
 *
 * Every run prints the authority it used and the version it saw, so a
 * log from the box and a log from a mac read differently and both
 * read true. DIFF3 and GIT in the environment override the paths.
 *
 * Whether the outside merger found a conflict is read from its OUTPUT
 * -- a line of seven angle brackets -- and never from its exit
 * status, because the status was itself wrong in diff3 until
 * fe5341287c6c and the classification has to hold on the old one too.
 *
 * THE RELATION ASSERTED. Every case with a base falls in one of three
 * boxes and nothing else:
 *
 *   A. Our merge has no conflict chunk and no both-same chunk, and
 *      the outside merger produced no markers. Then the merged bytes
 *      are compared, ours against the authority's.
 *
 *   B. Our merge has a both-same chunk and no conflict chunk, and the
 *      outside merger bracketed something. That is the one known
 *      translation and not a disagreement: diff3 -m implies -A, which
 *      brackets a change both sides made identically, and FreeBSD's
 *      -A, -E and -X all do it, which was measured rather than
 *      assumed; git at its eager level does not, and neither do we
 *      (v4-merge3.md sections 3.5 and 4). Where git is the authority
 *      this box is empty, which the counts show.
 *
 *   C. Our merge has a conflict chunk and the outside merger
 *      bracketed something. Both found a conflict; WHERE they found
 *      it, how many they found and what lies between the markers are
 *      not compared.
 *
 * A case outside the three fails the run, printing the seed, the case
 * number, the three inputs' paths under the build directory and what
 * each side said.
 *
 * THE ONE PLACE THE RELATION IS PROPERTIES AND NOT BYTES. In box A a
 * byte difference is NOT by itself a failure. Two correct mergers may
 * align a stretch differently where lines repeat -- the alignment
 * class the review recorded for the battery -- and then merge cleanly
 * to different bytes, both answers being right. So a differing box A
 * is held to the properties of v4-merge3.md section 7 instead: every
 * chunk's ranges partition their file and rebuild it, so no line is
 * invented and none is lost, and every chunk's kind is honest about
 * its ranges. Those are checked on every case anyway; the case is
 * counted as "aligned differently" and printed, and only a property
 * failure stops the run. The corpus is built to keep that number
 * small -- lines that occur once, edits that never write a pooled
 * line -- so a jump in it is itself worth reading.
 *
 * The add/add cases have no base at all, so there is no third file to
 * hand the outside merger: an empty base FILE is a different input,
 * which goes through the walk (v4-merge3.md section 5). They are held
 * instead to the properties that form does claim, through the same
 * check_add_add_props the pinned cases use (ZP95).
 *
 * The files are written under the build directory and never in /tmp,
 * so that a failure leaves them where the person is already looking.
 */

/* The scratch directory, under the build directory the Makefile owns. */
#define	OR_DIR_DEFAULT	"build/oracle"

/* The first FreeBSD diff3 whose merge mode is right (2cfca8e710f2). */
#define	OR_DIFF3_FIXED	20260213u

/* The run's shape, all of it overridable on the command line. */
#define	OR_SEED		20260915u
#define	OR_CASES	60u
#define	OR_BIG		0u

/* One line of a case is short; a case is many of them. */
#define	OR_LINEMAX	32

/* The pool of lines that occur more than once, kept small on purpose. */
#define	OR_POOL		4

/* What a case is built to be. */
#define	OR_CLEAN	0	/* the two sides touch different stretches */
#define	OR_MIXED	1	/* and one stretch they both edit alike */
#define	OR_CONFLICT	2	/* and one they edit differently */

/* What an edit site is, on the two sides. */
#define	OR_E_FROM	0
#define	OR_E_ONTO	1
#define	OR_E_SAME	2
#define	OR_E_CONF	3

/* At most this many edits in one case, however long the file is. */
#define	OR_SITES	4

/* Which outside merger says what the bytes are. */
#define	OR_BY_NONE	0
#define	OR_BY_DIFF3	1
#define	OR_BY_GIT	2

struct or_gen {
	uint32_t	og_state;
};

/* xorshift32: ours, so that a case is the same case on every machine. */
static uint32_t
or_next(struct or_gen *g)
{
	uint32_t x = g->og_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g->og_state = x != 0 ? x : 0x9e3779b9u;
	return (g->og_state);
}

static uint32_t
or_upto(struct or_gen *g, uint32_t n)
{
	return (n == 0 ? 0 : or_next(g) % n);
}

/* A growable buffer of text, which is what a generated file is. */
struct or_text {
	char	*ot_buf;
	size_t	ot_len;
	size_t	ot_cap;
};

static void
or_addline(struct or_text *t, const char *line)
{
	size_t n = strlen(line);

	if (t->ot_len + n + 2 > t->ot_cap) {
		size_t want = (t->ot_cap != 0 ? t->ot_cap * 2 : 4096);

		while (want < t->ot_len + n + 2)
			want *= 2;
		t->ot_buf = realloc(t->ot_buf, want);
		CHECK(t->ot_buf != NULL);
		t->ot_cap = want;
	}
	memcpy(t->ot_buf + t->ot_len, line, n);
	t->ot_len += n;
	t->ot_buf[t->ot_len++] = '\n';
	t->ot_buf[t->ot_len] = '\0';
}

static void
or_free(struct or_text *t)
{
	free(t->ot_buf);
	memset(t, 0, sizeof (*t));
}

/*
 * One line of the corpus. The tag says where it came from, so that a
 * failure's three files can be read by eye: u a line that occurs
 * once, p one of the pool, and f, o or s a line from's edit, onto's
 * edit or the edit they both made.
 */
static void
or_line(char *out, size_t outlen, char tag, uint32_t n)
{
	(void) snprintf(out, outlen, "%c%u", tag, (unsigned)n);
}

/* One edit site: the base lines it covers and what the two sides do. */
struct or_site {
	uint32_t	os_lo;
	uint32_t	os_hi;
	int		os_kind;
};

/*
 * What one side writes in place of the base lines [lo, hi): a
 * replacement, an insertion before them, a deletion of them, or the
 * stretch with its two halves swapped, which is a block moved. tag is
 * the side's letter and k the site's number, so the two sides never
 * write the same line by accident where they are meant to differ.
 */
static void
or_edit(struct or_gen *g, struct or_text *t, char (*base)[OR_LINEMAX],
    uint32_t lo, uint32_t hi, char tag, uint32_t k)
{
	uint32_t kind = or_upto(g, 100), n, i;
	char line[OR_LINEMAX];

	if (kind < 20)
		return;				/* a deletion */
	if (kind < 45) {
		n = 1 + or_upto(g, 3);		/* an insertion */
		for (i = 0; i < n; i++) {
			or_line(line, sizeof (line), tag, k * 100 + i);
			or_addline(t, line);
		}
		for (i = lo; i < hi; i++)
			or_addline(t, base[i]);
		return;
	}
	if (kind < 80 || hi - lo < 2) {
		n = 1 + or_upto(g, 3);		/* a replacement */
		for (i = 0; i < n; i++) {
			or_line(line, sizeof (line), tag, k * 100 + i);
			or_addline(t, line);
		}
		return;
	}
	{					/* a block moved */
		uint32_t mid = lo + (hi - lo) / 2;

		for (i = mid; i < hi; i++)
			or_addline(t, base[i]);
		for (i = lo; i < mid; i++)
			or_addline(t, base[i]);
	}
}

/*
 * One case: a base of nlines lines and the two sides derived from it
 * by one to four edits, placed to give the case the shape it was
 * asked for. Returns the number of sites, which the counts print.
 */
static uint32_t
or_case(struct or_gen *g, uint32_t nlines, int shape, struct or_text *base,
    struct or_text *frm, struct or_text *onto)
{
	char (*lines)[OR_LINEMAX];
	struct or_site site[OR_SITES];
	uint32_t i, j, n = 0, pos = 0, want;
	int lasteol;

	memset(base, 0, sizeof (*base));
	memset(frm, 0, sizeof (*frm));
	memset(onto, 0, sizeof (*onto));
	lines = malloc((size_t)(nlines != 0 ? nlines : 1) * OR_LINEMAX);
	CHECK(lines != NULL);
	for (i = 0; i < nlines; i++) {
		if (or_upto(g, 8) != 0)
			or_line(lines[i], OR_LINEMAX, 'u', i);
		else
			or_line(lines[i], OR_LINEMAX, 'p', or_upto(g, OR_POOL));
	}
	for (i = 0; i < nlines; i++)
		or_addline(base, lines[i]);
	/*
	 * The sites: at least two untouched lines before each and after
	 * the last, so that two edits never touch. Changes with no
	 * stable line between them are one conflict to every merger and
	 * would crowd out the shapes this corpus is for.
	 */
	want = 1 + or_upto(g, OR_SITES);
	while (n < want && pos + 3 <= nlines) {
		uint32_t room = nlines - pos, skip, run;

		skip = 2 + or_upto(g, room / (want - n + 1) + 1);
		if (pos + skip + 3 > nlines)
			break;
		pos += skip;
		run = 1 + or_upto(g, 3);
		if (pos + run + 2 > nlines)
			run = nlines - pos - 2;
		site[n].os_lo = pos;
		site[n].os_hi = pos + run;
		site[n].os_kind = or_upto(g, 2) == 0 ? OR_E_FROM : OR_E_ONTO;
		pos += run;
		n++;
	}
	/*
	 * The shape, imposed on the sites that were placed: a mixed case
	 * has one both-same site and a conflicted one has a site the two
	 * sides edit differently. A file too short for two sites keeps
	 * the shape it can.
	 */
	if (n != 0 && shape == OR_MIXED)
		site[or_upto(g, n)].os_kind = OR_E_SAME;
	if (n != 0 && shape == OR_CONFLICT) {
		site[or_upto(g, n)].os_kind = OR_E_CONF;
		if (n > 1)
			site[or_upto(g, n)].os_kind = OR_E_SAME;
	}
	j = 0;
	for (i = 0; i < nlines; ) {
		if (j < n && i == site[j].os_lo) {
			uint32_t lo = site[j].os_lo, hi = site[j].os_hi;

			switch (site[j].os_kind) {
			case OR_E_FROM:
				or_edit(g, frm, lines, lo, hi, 'f', j + 1);
				for (i = lo; i < hi; i++)
					or_addline(onto, lines[i]);
				break;
			case OR_E_ONTO:
				for (i = lo; i < hi; i++)
					or_addline(frm, lines[i]);
				or_edit(g, onto, lines, lo, hi, 'o', j + 1);
				break;
			case OR_E_SAME: {
				/*
				 * The identical edit on both sides, which
				 * is the SAME kind: the same generator
				 * state is spent twice.
				 */
				struct or_gen save = *g;

				or_edit(g, frm, lines, lo, hi, 's', j + 1);
				*g = save;
				or_edit(g, onto, lines, lo, hi, 's', j + 1);
				break;
			}
			default:
				or_edit(g, frm, lines, lo, hi, 'f', j + 1);
				or_edit(g, onto, lines, lo, hi, 'o', j + 1);
				break;
			}
			i = hi;
			j++;
			continue;
		}
		or_addline(frm, lines[i]);
		or_addline(onto, lines[i]);
		i++;
	}
	lasteol = or_upto(g, 8) != 0;
	if (lasteol == 0) {
		/*
		 * The file ends without a final newline, which is a fact
		 * about its last line and must be the same fact in all
		 * three (v4-merge3.md section 5).
		 */
		struct or_text *all[3];
		int w;

		all[0] = base;
		all[1] = frm;
		all[2] = onto;
		for (w = 0; w < 3; w++)
			if (all[w]->ot_len > 0 &&
			    all[w]->ot_buf[all[w]->ot_len - 1] == '\n')
				all[w]->ot_buf[--all[w]->ot_len] = '\0';
	}
	free(lines);
	return (n);
}

/* The three files of one case, where a failure can be read afterwards. */
static void
or_write(const char *dir, const char *tag, const struct or_text *t,
    char *out, size_t outlen)
{
	FILE *f;

	(void) snprintf(out, outlen, "%s/%s", dir, tag);
	f = fopen(out, "w");
	if (f == NULL) {
		printf("oracle: %s: %s\n", out, strerror(errno));
		exit(1);
	}
	CHECK(fwrite(t->ot_buf, 1, t->ot_len, f) == t->ot_len);
	CHECK(fclose(f) == 0);
}

static char *
or_slurp(const char *path, size_t *lenp)
{
	long n;
	char *out;
	FILE *f;

	*lenp = 0;
	f = fopen(path, "r");
	if (f == NULL)
		return (NULL);
	CHECK(fseek(f, 0, SEEK_END) == 0);
	n = ftell(f);
	CHECK(n >= 0);
	CHECK(fseek(f, 0, SEEK_SET) == 0);
	out = malloc((size_t)n + 1);
	CHECK(out != NULL);
	CHECK(fread(out, 1, (size_t)n, f) == (size_t)n);
	CHECK(fclose(f) == 0);
	out[n] = '\0';
	*lenp = (size_t)n;
	return (out);
}

/*
 * Did the outside merger bracket anything? Read from the output and
 * never from the exit status, which was itself wrong in FreeBSD's
 * diff3 until fe5341287c6c and must not be depended on here.
 */
static int
or_bracketed(const char *buf, size_t len)
{
	size_t i;

	for (i = 0; i + 7 <= len; i++) {
		if (memcmp(buf + i, "<<<<<<<", 7) != 0)
			continue;
		if (i == 0 || buf[i - 1] == '\n')
			return (1);
	}
	return (0);
}

/* The first line at which two answers part, 1-based, or 0 where they do not. */
static uint32_t
or_firstdiff(const char *a, size_t alen, const char *b, size_t blen)
{
	size_t i, line = 1;

	for (i = 0; i < alen && i < blen; i++) {
		if (a[i] != b[i])
			return ((uint32_t)line);
		if (a[i] == '\n')
			line++;
	}
	return (alen == blen ? 0 : (uint32_t)line);
}

/* Which outside merger this run has, and what it may be asked. */
struct or_auth {
	const char	*oa_diff3;	/* the path, or NULL */
	const char	*oa_git;	/* the path, or NULL */
	uint32_t	oa_version;	/* FreeBSD diff3's date, or 0 */
	int		oa_gnu;		/* GNU diffutils' diff3 */
	int		oa_bytes;	/* OR_BY_* */
};

/* The eight digits of "FreeBSD diff3 20260213", or 0 for anything else. */
static uint32_t
or_version(const char *text)
{
	size_t i, j;

	if (strstr(text, "FreeBSD") == NULL)
		return (0);
	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] < '0' || text[i] > '9')
			continue;
		for (j = i; text[j] >= '0' && text[j] <= '9'; j++)
			continue;
		if (j - i == 8)
			return ((uint32_t)strtoul(text + i, NULL, 10));
		i = j - 1;
	}
	return (0);
}

static void
or_authority(struct or_auth *a, const char *dir)
{
	static const char *const gits[] = { "/usr/bin/git",
		"/usr/local/bin/git" };
	const char *env;
	char cmd[1024], path[512], *text;
	size_t len, i;

	memset(a, 0, sizeof (*a));
	env = getenv("DIFF3");
	a->oa_diff3 = env != NULL ? env : "/usr/bin/diff3";
	if (access(a->oa_diff3, X_OK) != 0)
		a->oa_diff3 = NULL;
	env = getenv("GIT");
	if (env != NULL) {
		a->oa_git = access(env, X_OK) == 0 ? env : NULL;
	} else {
		for (i = 0; i < sizeof (gits) / sizeof (gits[0]); i++)
			if (access(gits[i], X_OK) == 0) {
				a->oa_git = gits[i];
				break;
			}
	}
	if (a->oa_diff3 != NULL) {
		(void) snprintf(path, sizeof (path), "%s/version", dir);
		(void) snprintf(cmd, sizeof (cmd),
		    "%s --version > %s 2>/dev/null", a->oa_diff3, path);
		if (system(cmd) == 0) {
			text = or_slurp(path, &len);
			if (text != NULL) {
				a->oa_version = or_version(text);
				a->oa_gnu = strstr(text, "GNU diffutils") !=
				    NULL;
				free(text);
			}
		}
	}
	if (a->oa_diff3 != NULL &&
	    (a->oa_gnu || a->oa_version >= OR_DIFF3_FIXED))
		a->oa_bytes = OR_BY_DIFF3;
	else if (a->oa_git != NULL)
		a->oa_bytes = OR_BY_GIT;
	else
		a->oa_bytes = OR_BY_NONE;
}

/* What one case came to, for the counts the run prints. */
#define	OR_BOX_A	0	/* both clean, and the bytes agree */
#define	OR_BOX_B	1	/* ours both-same where diff3 brackets */
#define	OR_BOX_C	2	/* both conflicted */
#define	OR_BOX_ADD	3	/* no base: the properties instead */
#define	OR_BOX_ALIGN	4	/* both clean, different bytes, both sound */
#define	OR_BOX_N	5

/*
 * One generated case, through both implementations. Returns the box it
 * fell in, and does not return at all where it fell outside them.
 */
static int
or_one(uint32_t seed, uint32_t idx, uint32_t nlines, int shape, int addadd,
    const char *dir, const struct or_auth *a)
{
	char bp[512], fp[512], op[512], outp[512], cmd[2048];
	struct or_text base, frm, onto;
	unsigned char *ours = NULL;
	const char *who = "";
	struct or_gen g;
	uint32_t i, same = 0;
	char err[256], *theirs = NULL;
	size_t len = 0, tlen = 0;
	struct zr_m3 m;
	int marks = 0, status;

	g.og_state = seed + idx * 2654435761u;
	if (g.og_state == 0)
		g.og_state = 1;
	(void) or_case(&g, nlines, shape, &base, &frm, &onto);
	if (addadd != 0) {
		/*
		 * No base at all: there is no third file to hand the
		 * outside merger, and an empty base FILE is a different
		 * input. The form's own properties stand in (ZP95,
		 * v4-merge3.md section 7).
		 */
		check_add_add_props("oracle add/add", frm.ot_buf, frm.ot_len,
		    onto.ot_buf, onto.ot_len);
		or_free(&base);
		or_free(&frm);
		or_free(&onto);
		return (OR_BOX_ADD);
	}
	or_write(dir, "base", &base, bp, sizeof (bp));
	or_write(dir, "from", &frm, fp, sizeof (fp));
	or_write(dir, "onto", &onto, op, sizeof (op));
	(void) snprintf(outp, sizeof (outp), "%s/merged", dir);
	if (a->oa_bytes == OR_BY_GIT) {
		who = "git merge-file";
		(void) snprintf(cmd, sizeof (cmd), "%s merge-file -p --diff3 "
		    "%s %s %s > %s 2>/dev/null", a->oa_git, fp, bp, op, outp);
	} else {
		who = "diff3 -m";
		(void) snprintf(cmd, sizeof (cmd), "%s -m %s %s %s > %s "
		    "2>/dev/null", a->oa_diff3, fp, bp, op, outp);
	}
	status = system(cmd);
	if (!WIFEXITED(status) || WEXITSTATUS(status) > 1) {
		printf("oracle: seed %u case %u: %s would not run "
		    "(status %d)\n", (unsigned)seed, (unsigned)idx, who,
		    status);
		exit(1);
	}
	theirs = or_slurp(outp, &tlen);
	CHECK(theirs != NULL);
	marks = or_bracketed(theirs, tlen);
	CHECK(zr_m3_open(&m, (const unsigned char *)base.ot_buf, base.ot_len,
	    (const unsigned char *)frm.ot_buf, frm.ot_len,
	    (const unsigned char *)onto.ot_buf, onto.ot_len, err,
	    sizeof (err)) == 0);
	/*
	 * The properties, on every case and before any comparison: they
	 * are what a differing box A falls back on, so they are never
	 * the thing that was skipped.
	 */
	check_partition(&m, "oracle");
	check_kinds(&m, "oracle");
	for (i = 0; i < m.nchunks; i++)
		if (m.chunks[i].kind == ZR_M3_SAME)
			same++;
	if (m.nconflict == 0 && same == 0 && marks == 0) {
		int box = OR_BOX_A;

		if (a->oa_bytes == OR_BY_NONE) {
			checks++;	/* the classification alone */
		} else {
			CHECK(zr_m3_result(&m, &ours, &len, err,
			    sizeof (err)) == 0);
			if (len != tlen || memcmp(ours, theirs, len) != 0) {
				/*
				 * Both merged cleanly and the bytes
				 * differ: an alignment both may be right
				 * about. The properties above are what
				 * says ours is sound, and they have run.
				 */
				printf("oracle: seed %u case %u (%u lines): "
				    "aligned differently from %s, at line "
				    "%u; ours %zu bytes, theirs %zu\n",
				    (unsigned)seed, (unsigned)idx,
				    (unsigned)nlines, who,
				    (unsigned)or_firstdiff((const char *)ours,
				    len, theirs, tlen), len, tlen);
				box = OR_BOX_ALIGN;
			}
			checks++;
			free(ours);
		}
		free(theirs);
		zr_m3_fini(&m);
		or_free(&base);
		or_free(&frm);
		or_free(&onto);
		return (box);
	}
	if (m.nconflict == 0 && same != 0 && marks != 0) {
		checks++;		/* the one known translation */
		free(theirs);
		zr_m3_fini(&m);
		or_free(&base);
		or_free(&frm);
		or_free(&onto);
		return (OR_BOX_B);
	}
	if (m.nconflict != 0 && marks != 0) {
		checks++;		/* both conflicted */
		free(theirs);
		zr_m3_fini(&m);
		or_free(&base);
		or_free(&frm);
		or_free(&onto);
		return (OR_BOX_C);
	}
	if (m.nconflict == 0 && same != 0 && marks == 0 &&
	    a->oa_bytes == OR_BY_GIT) {
		/*
		 * git at its eager level merges an identical change on
		 * both sides silently, as we do, so box B is empty
		 * under git and this is box A with the bytes compared.
		 */
		CHECK(zr_m3_result(&m, &ours, &len, err, sizeof (err)) == 0);
		if (len != tlen || memcmp(ours, theirs, len) != 0) {
			printf("oracle: seed %u case %u (%u lines): aligned "
			    "differently from %s over a both-same stretch, "
			    "at line %u\n", (unsigned)seed, (unsigned)idx,
			    (unsigned)nlines, who,
			    (unsigned)or_firstdiff((const char *)ours, len,
			    theirs, tlen));
			free(ours);
			free(theirs);
			zr_m3_fini(&m);
			or_free(&base);
			or_free(&frm);
			or_free(&onto);
			return (OR_BOX_ALIGN);
		}
		checks++;
		free(ours);
		free(theirs);
		zr_m3_fini(&m);
		or_free(&base);
		or_free(&frm);
		or_free(&onto);
		return (OR_BOX_A);
	}
	printf("oracle: seed %u case %u (%u lines): a disagreement that is "
	    "not the known translation\n", (unsigned)seed, (unsigned)idx,
	    (unsigned)nlines);
	printf("  ours: %u chunks, %u conflicts, %u both-same\n",
	    (unsigned)m.nchunks, (unsigned)m.nconflict, (unsigned)same);
	printf("  %s: %s\n", who, marks != 0 ? "bracketed a conflict" :
	    "merged cleanly");
	printf("  base %s\n  from %s\n  onto %s\n  theirs %s\n", bp, fp, op,
	    outp);
	exit(1);
	return (-1);
}

/*
 * The corpus. The shapes cycle so that the three boxes each see about
 * a third of it; the sizes climb so that the small cases, where a hand
 * can read the three files, come first. The big sizes are the last
 * argument's, since tens of thousands of lines is the box's run and
 * not make check's.
 */
static int
run_oracle(uint32_t seed, uint32_t cases, uint32_t big)
{
	static const uint32_t bigsizes[] = { 4000, 8000, 16000, 32000 };
	const char *dir = getenv("ZR_ORACLE_DIR");
	uint32_t i, box[OR_BOX_N], shape[3], nlines;
	struct or_auth a;

	if (dir == NULL)
		dir = OR_DIR_DEFAULT;
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
		printf("oracle: %s: %s\n", dir, strerror(errno));
		return (1);
	}
	or_authority(&a, dir);
	if (a.oa_diff3 == NULL && a.oa_git == NULL) {
		printf("skip ZP141: no diff3 and no git to hold the merge "
		    "against\n");
		return (0);
	}
	printf("oracle: %s", a.oa_diff3 != NULL ? a.oa_diff3 : "no diff3");
	if (a.oa_version != 0)
		printf(" (FreeBSD diff3 %u)", (unsigned)a.oa_version);
	else if (a.oa_gnu)
		printf(" (GNU diffutils)");
	else if (a.oa_diff3 != NULL)
		printf(" (version unknown: neither FreeBSD's nor GNU's)");
	printf(", git %s\n", a.oa_git != NULL ? a.oa_git : "absent");
	switch (a.oa_bytes) {
	case OR_BY_DIFF3:
		if (a.oa_gnu)
			printf("oracle: the bytes are diff3's: GNU diffutils, "
			    "the reference the theory's battery was recorded "
			    "against\n");
		else
			printf("oracle: the bytes are diff3's, whose merge "
			    "mode is right at %u and later\n",
			    (unsigned)OR_DIFF3_FIXED);
		break;
	case OR_BY_GIT:
		printf("oracle: the bytes are git merge-file's; this diff3 "
		    "is older than %u, whose merge mode lost lines\n",
		    (unsigned)OR_DIFF3_FIXED);
		break;
	default:
		printf("oracle: no byte authority here: the classification "
		    "alone is asserted\n");
		break;
	}
	memset(box, 0, sizeof (box));
	memset(shape, 0, sizeof (shape));
	for (i = 0; i < cases; i++) {
		int addadd = (i % 5) == 4;

		nlines = 3 + (i * 7) % 398;
		if (addadd == 0)
			shape[i % 3]++;
		box[or_one(seed, i, nlines, (int)(i % 3), addadd, dir, &a)]++;
	}
	for (i = 0; i < big && i < sizeof (bigsizes) / sizeof (bigsizes[0]);
	    i++)
		box[or_one(seed, 1000 + i, bigsizes[i], (int)(i % 3), 0, dir,
		    &a)]++;
	printf("oracle: seed %u, %u cases of 3 to 400 lines", (unsigned)seed,
	    (unsigned)cases);
	if (big != 0)
		printf(" and %u of 4000 to 32000", (unsigned)big);
	printf("\n");
	printf("oracle: shapes asked for: %u clean, %u mixed, %u "
	    "conflicted, and %u with no base\n", (unsigned)shape[OR_CLEAN],
	    (unsigned)shape[OR_MIXED], (unsigned)shape[OR_CONFLICT],
	    (unsigned)box[OR_BOX_ADD]);
	printf("oracle: %u merged alike, %u both-same bracketed, %u "
	    "conflicted on both sides, %u add/add by the properties, %u "
	    "aligned differently\n", (unsigned)box[OR_BOX_A],
	    (unsigned)box[OR_BOX_B], (unsigned)box[OR_BOX_C],
	    (unsigned)box[OR_BOX_ADD], (unsigned)box[OR_BOX_ALIGN]);
	return (0);
}

int
main(int argc, char **argv)
{
	/*
	 * --oracle [SEED [CASES [BIG]]] is the generated corpus held
	 * against diff3 (ZP141); with no argument, or with a path, this
	 * is the battery and the shapes beside it, as it has always
	 * been. The two do not run together, so that make check can
	 * take the corpus at its own size and the box at another.
	 */
	if (argc > 1 && strcmp(argv[1], "--oracle") == 0) {
		uint32_t seed = argc > 2 ? (uint32_t)strtoul(argv[2], NULL,
		    10) : OR_SEED;
		uint32_t cases = argc > 3 ? (uint32_t)strtoul(argv[3], NULL,
		    10) : OR_CASES;
		uint32_t big = argc > 4 ? (uint32_t)strtoul(argv[4], NULL,
		    10) : OR_BIG;
		int rc = run_oracle(seed, cases, big);

		printf("check_merge --oracle: %d checks passed\n", checks);
		return (rc);
	}
	run_battery(argc > 1 ? argv[1] : BATTERY_DEFAULT);
	check_text();
	check_ceiling();
	check_write_rule();
	check_final_newline();
	check_add_add();
	check_delete_edit();
	check_hint_marks();
	printf("check_merge: %d checks passed\n", checks);
	return (0);
}
