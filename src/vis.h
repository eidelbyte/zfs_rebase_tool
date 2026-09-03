/*
 * vis: the one escaping rule the manifest uses for names. A byte from
 * 0x21 to 0x7e stands for itself, except backslash and hash; every
 * other byte is a backslash and exactly three octal digits.
 */

#ifndef	ZR_VIS_H
#define	ZR_VIS_H

#include <stddef.h>

/*
 * Encode in[0..inlen) into out. Returns the length the full encoding
 * needs, not counting the terminating NUL. When outcap is 0 out may be
 * NULL and nothing is written; otherwise out is always NUL-terminated
 * and the encoding is truncated to fit, the way snprintf() does it.
 * Nothing is ever written at or past out + outcap.
 */
size_t zr_vis_encode(const unsigned char *in, size_t inlen, char *out,
    size_t outcap);

/*
 * Decode in[0..inlen) into out, which is not NUL-terminated: a name may
 * hold any byte. Returns 0 on success with *outlen set to the decoded
 * length. Returns -1 on a bad escape (short at the end of input, a
 * non-octal digit, or a value above 0377), leaving *outlen alone and
 * out untouched, and -1 with *outlen set to the length needed if outcap
 * is too small. When outcap is 0 out may be NULL, which sizes the
 * decode without writing.
 */
int zr_vis_decode(const char *in, size_t inlen, unsigned char *out,
    size_t outcap, size_t *outlen);

#endif	/* ZR_VIS_H */
