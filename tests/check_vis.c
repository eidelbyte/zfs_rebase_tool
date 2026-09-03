/*
 * Tests for the vis codec: the escaping rule for names, both
 * directions, over every byte value, the documented examples, the
 * malformed escapes and the buffer-sizing contract.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vis.h"

#define	GUARD	0x55

static unsigned long checks;

static void
check_at(int cond, const char *file, int line, const char *expr)
{
	checks++;
	if (!cond) {
		fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
		exit(1);
	}
}

#define	CHECK(x)	check_at((x) ? 1 : 0, __FILE__, __LINE__, #x)

/*
 * Encode in[0..inlen) into a roomy buffer and compare it, NUL and all,
 * with the expected text.
 */
static void
check_encode(const unsigned char *in, size_t inlen, const char *want)
{
	char buf[256];
	size_t n;

	n = zr_vis_encode(in, inlen, buf, sizeof (buf));
	CHECK(n == strlen(want));
	CHECK(n < sizeof (buf));
	CHECK(memcmp(buf, want, n + 1) == 0);
}

/*
 * Decode the NUL-terminated text in and compare the bytes it yields
 * with the expected name.
 */
static void
check_decode(const char *in, const unsigned char *want, size_t wantlen)
{
	unsigned char buf[256];
	size_t n = 0;

	CHECK(zr_vis_decode(in, strlen(in), buf, sizeof (buf), &n) == 0);
	CHECK(n == wantlen);
	CHECK(memcmp(buf, want, wantlen) == 0);
}

/*
 * Every one of the 256 byte values survives an encode and a decode,
 * and encodes to either itself or a four-character escape.
 */
static void
test_all_bytes(void)
{
	unsigned int v;

	for (v = 0; v < 256; v++) {
		unsigned char in = (unsigned char)v;
		unsigned char back[8];
		char enc[8];
		size_t n, m;

		n = zr_vis_encode(&in, 1, enc, sizeof (enc));
		CHECK(n == 1 || n == 4);
		CHECK(zr_vis_decode(enc, n, back, sizeof (back), &m) == 0);
		CHECK(m == 1);
		CHECK(back[0] == in);
	}
}

/*
 * The four examples the format documents, both directions.
 */
static void
test_examples(void)
{
	static const unsigned char space[] = { 'a', ' ', 'b' };
	static const unsigned char eacute[] = { 0xc3, 0xa9 };
	static const unsigned char bslash[] = { 0x5c };
	static const unsigned char hash[] = { 0x23 };

	check_encode(space, sizeof (space), "a\\040b");
	check_decode("a\\040b", space, sizeof (space));
	check_encode(eacute, sizeof (eacute), "\\303\\251");
	check_decode("\\303\\251", eacute, sizeof (eacute));
	check_encode(bslash, sizeof (bslash), "\\134");
	check_decode("\\134", bslash, sizeof (bslash));
	check_encode(hash, sizeof (hash), "\\043");
	check_decode("\\043", hash, sizeof (hash));
}

/*
 * One name mixing every class: plain printables, both ends of the
 * printable range, the two reserved bytes, a control byte, NUL, DEL
 * and a high pair.
 */
static void
test_mixed(void)
{
	static const unsigned char name[] = {
		'd', 'i', 'r', '/', 'n', 'a', ' ', 'm', 'e',
		0xc3, 0xa9, '#', '\\', 0x00, 0x01, 0x7f, '~', '!',
		'.', 't', 'x', 't'
	};
	static const char enc[] =
	    "dir/na\\040me\\303\\251\\043\\134\\000\\001\\177~!.txt";

	check_encode(name, sizeof (name), enc);
	check_decode(enc, name, sizeof (name));
}

/*
 * A backslash must carry exactly three octal digits worth at most
 * 0377. Everything else after one is an error, and a failed decode
 * writes nothing and leaves *outlen alone.
 */
static void
test_bad_escapes(void)
{
	static const char *bad[] = {
		"\\04",		/* short: input ends inside the escape */
		"\\04x",	/* 'x' is not an octal digit */
		"\\400",	/* 0400 is above 0377 */
		"\\",		/* a backslash alone */
		"\\777",	/* the largest three-digit overflow */
		"ok\\09"	/* '9' is not an octal digit */
	};
	size_t i;

	for (i = 0; i < sizeof (bad) / sizeof (bad[0]); i++) {
		unsigned char buf[16];
		size_t n = 12345;
		size_t k;

		memset(buf, GUARD, sizeof (buf));
		CHECK(zr_vis_decode(bad[i], strlen(bad[i]), buf,
		    sizeof (buf), &n) == -1);
		CHECK(n == 12345);
		for (k = 0; k < sizeof (buf); k++)
			CHECK(buf[k] == GUARD);
	}
}

/*
 * Both directions report the size they need when the buffer is too
 * small, and neither writes past the buffer it was handed.
 */
static void
test_sizing(void)
{
	static const unsigned char in[] = { 'a', ' ', 'b' };
	static const char enc[] = "a\\040b";
	char ebuf[16];
	unsigned char dbuf[16];
	size_t n;
	size_t i;

	/* No buffer at all: encode still reports the need. */
	CHECK(zr_vis_encode(in, sizeof (in), NULL, 0) == 6);

	/* Four bytes hold three characters and a NUL; the rest is safe. */
	memset(ebuf, GUARD, sizeof (ebuf));
	CHECK(zr_vis_encode(in, sizeof (in), ebuf, 4) == 6);
	CHECK(ebuf[3] == '\0');
	for (i = 4; i < sizeof (ebuf); i++)
		CHECK(ebuf[i] == GUARD);

	/* An exact fit, NUL included, is not truncation. */
	memset(ebuf, GUARD, sizeof (ebuf));
	CHECK(zr_vis_encode(in, sizeof (in), ebuf, 7) == 6);
	CHECK(strcmp(ebuf, enc) == 0);
	CHECK(ebuf[7] == GUARD);

	/* No buffer at all: decode reports the need and fails. */
	n = 0;
	CHECK(zr_vis_decode(enc, strlen(enc), NULL, 0, &n) == -1);
	CHECK(n == 3);

	/* One byte short: same answer, and nothing written. */
	memset(dbuf, GUARD, sizeof (dbuf));
	n = 0;
	CHECK(zr_vis_decode(enc, strlen(enc), dbuf, 2, &n) == -1);
	CHECK(n == 3);
	for (i = 0; i < sizeof (dbuf); i++)
		CHECK(dbuf[i] == GUARD);

	/* An exact fit succeeds and touches nothing beyond it. */
	memset(dbuf, GUARD, sizeof (dbuf));
	n = 0;
	CHECK(zr_vis_decode(enc, strlen(enc), dbuf, 3, &n) == 0);
	CHECK(n == 3);
	CHECK(memcmp(dbuf, in, sizeof (in)) == 0);
	CHECK(dbuf[3] == GUARD);
}

/*
 * The empty name encodes to the empty string and decodes to nothing.
 */
static void
test_empty(void)
{
	static const unsigned char none[1] = { 0 };
	char ebuf[4];
	unsigned char dbuf[4];
	size_t n = 12345;

	memset(ebuf, GUARD, sizeof (ebuf));
	CHECK(zr_vis_encode(none, 0, ebuf, sizeof (ebuf)) == 0);
	CHECK(ebuf[0] == '\0');
	CHECK(ebuf[1] == GUARD);
	CHECK(zr_vis_encode(none, 0, NULL, 0) == 0);

	memset(dbuf, GUARD, sizeof (dbuf));
	CHECK(zr_vis_decode("", 0, dbuf, sizeof (dbuf), &n) == 0);
	CHECK(n == 0);
	CHECK(dbuf[0] == GUARD);
}

int
main(void)
{
	test_all_bytes();
	test_examples();
	test_mixed();
	test_bad_escapes();
	test_sizing();
	test_empty();
	printf("check_vis: %lu checks passed\n", checks);
	return (0);
}
