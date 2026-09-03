/*
 * vis: encode and decode names for the manifest. Printable bytes go
 * through unchanged, the two bytes the format reserves (backslash and
 * hash) and everything unprintable become three octal digits behind a
 * backslash. Decoding is the exact inverse. No allocation, libc only.
 */

#include "vis.h"

#define	VIS_ESCAPE	'\\'
#define	VIS_COMMENT	'#'

/*
 * Store one output byte if it lands inside the caller's buffer. Sizing
 * calls pass outcap 0 and get the length without a buffer at all.
 */
static void
vis_put(char *out, size_t outcap, size_t pos, char c)
{
	if (pos < outcap)
		out[pos] = c;
}

/*
 * Read the escape whose backslash sits at in[i]. Returns the byte it
 * names, or -1 if the escape runs off the end of the input, holds a
 * digit outside 0-7, or names a value above 0377.
 */
static int
vis_unescape(const char *in, size_t inlen, size_t i)
{
	size_t k;
	int v = 0;

	if (inlen - i < 4)
		return (-1);
	for (k = i + 1; k <= i + 3; k++) {
		char c = in[k];

		if (c < '0' || c > '7')
			return (-1);
		v = v * 8 + (c - '0');
	}
	if (v > 0377)
		return (-1);
	return (v);
}

size_t
zr_vis_encode(const unsigned char *in, size_t inlen, char *out, size_t outcap)
{
	size_t i;
	size_t n = 0;

	for (i = 0; i < inlen; i++) {
		unsigned char c = in[i];

		if (c >= 0x21 && c <= 0x7e && c != VIS_ESCAPE &&
		    c != VIS_COMMENT) {
			vis_put(out, outcap, n++, (char)c);
			continue;
		}
		vis_put(out, outcap, n++, VIS_ESCAPE);
		vis_put(out, outcap, n++, (char)('0' + ((c >> 6) & 07)));
		vis_put(out, outcap, n++, (char)('0' + ((c >> 3) & 07)));
		vis_put(out, outcap, n++, (char)('0' + (c & 07)));
	}
	if (outcap > 0)
		out[n < outcap ? n : outcap - 1] = '\0';
	return (n);
}

int
zr_vis_decode(const char *in, size_t inlen, unsigned char *out, size_t outcap,
    size_t *outlen)
{
	size_t i;
	size_t n = 0;

	/*
	 * First pass: validate every escape and count the decoded
	 * length, so that a decode which fails writes nothing at all.
	 */
	for (i = 0; i < inlen; n++) {
		if (in[i] == VIS_ESCAPE) {
			if (vis_unescape(in, inlen, i) < 0)
				return (-1);
			i += 4;
		} else {
			i++;
		}
	}
	if (outlen != NULL)
		*outlen = n;
	if (n > outcap)
		return (-1);

	for (i = 0, n = 0; i < inlen; n++) {
		if (in[i] == VIS_ESCAPE) {
			out[n] = (unsigned char)vis_unescape(in, inlen, i);
			i += 4;
		} else {
			out[n] = (unsigned char)in[i];
			i++;
		}
	}
	return (0);
}
