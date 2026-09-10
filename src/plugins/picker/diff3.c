/* BEGIN CSTYLED */
/*	$OpenBSD: diff3prog.c,v 1.11 2009/10/27 23:59:37 deraadt Exp $	*/

/*
 * SPDX-License-Identifier: Caldera-no-preamble AND BSD-3-Clause
 *
 * Copyright (C) Caldera International Inc.  2001-2002.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code and documentation must retain the above
 *    copyright notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed or owned by Caldera
 *	International, Inc.
 * 4. Neither the name of Caldera International, Inc. nor the names of other
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * USE OF THE SOFTWARE PROVIDED FOR UNDER THIS LICENSE BY CALDERA
 * INTERNATIONAL, INC. AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL CALDERA INTERNATIONAL, INC. BE LIABLE FOR ANY DIRECT,
 * INDIRECT INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
/* END CSTYLED */

/*
 * Derived from FreeBSD's usr.bin/diff3/diff3.c at commit
 * 37d8a26e0e07a4a0743176d1fa897e34816f34d7 (2026-09-06), the OpenBSD
 * diff3prog lineage. Kept: merge(), the loop that reads two edit
 * scripts together; duplicate(), the test that decides whether both
 * sides made the same change; and the arithmetic of change() and
 * keep(), which is what fills a chunk's range into the file the
 * change did not touch. Dropped: diffexec() and the spawn of diff(1);
 * readin(), getchange() and get_line(), the parse of diff's hunk
 * headers, which libdiff's chunk list replaces (merge.c builds the
 * two scripts); edscript(), Ascript(), mergescript(), separate(),
 * prange(), printrange(), skip(), repos() and every other print, since
 * what this produces is a chunk sequence and never a text with markers
 * in it; edit(), increase(), the option parsing and main(). No global
 * state survives: the scripts, the files and the output are arguments.
 *
 * Two deliberate departures from the original, both recorded in
 * v4-merge3.md section 3.4:
 *
 *  - duplicate() compares line records rather than bytes read back
 *    through the two open files with getc. The original treats an end
 *    of file inside a line as a "logic error" and exits, which is what
 *    happens when one side's last line ends without a newline and the
 *    other's ends with one. Here the newline is part of the record, so
 *    "b" and "b\n" are two different lines, the two ranges are
 *    unequal, and the chunk is a conflict.
 *
 *  - the slot assignment. diff3 runs "diff file1 file3" and "diff
 *    file2 file3", so both scripts share file3 as their axis; we put
 *    BASE there -- from is file1, onto is file2, base is file3 -- so
 *    that every chunk the walk emits carries a base range. That is the
 *    ruling of 2026-09-09 made operational. diff3's "peculiar to the
 *    first file" is then from-only, its "peculiar to the second" is
 *    onto-only, and its duplicate() test in the type-3 branch is
 *    both-same.
 *
 * The merging of overlapping entries within one script (step 3 below)
 * is transcribed as the original writes it: it pulls the next entry's
 * start back and leaves its end alone, so an entry widened past its
 * successor loses the tail of its coverage for a pass, and the next
 * pass puts it right. A local improvement there would be a local
 * divergence to explain forever, and the reference implementation's
 * exhaustive enumeration says the outcome is sound.
 *
 * Ranges here are zero-based and half-open, where the original's are
 * one-based with to = last + 1. Every comparison and every piece of
 * arithmetic in merge() is a difference of positions, so the shift
 * changes nothing.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "merge.h"

static void
m3_fail(char *err, size_t errlen, const char *what)
{
	if (err != NULL && errlen > 0)
		(void) snprintf(err, errlen, "merge: %s", what);
}

/*
 * duplicate(): do the two side ranges hold the same lines? The same
 * number of them, equal one by one, and a record is equal to another
 * when its bytes are -- no locale, no encoding, no case folding, and
 * the newline counted in, so a line that ends the file without one is
 * a different line from the same text with one.
 */
static int
m3_duplicate(const struct zr_m3_file *f1, uint32_t a0, uint32_t a1,
    const struct zr_m3_file *f2, uint32_t b0, uint32_t b1)
{
	uint32_t n, i;

	if (a1 - a0 != b1 - b0)
		return (0);
	n = a1 - a0;
	for (i = 0; i < n; i++) {
		const struct zr_m3_line *x = &f1->lines[a0 + i];
		const struct zr_m3_line *y = &f2->lines[b0 + i];

		if (x->len != y->len)
			return (0);
		if (x->len != 0 && memcmp(f1->bytes + x->off,
		    f2->bytes + y->off, (size_t)x->len) != 0)
			return (0);
	}
	return (1);
}

/*
 * merge(): the walk. While either script has an entry left, with d1
 * the from script's next entry and d2 the onto script's next, the
 * changed stretches are recorded into raw in order. The stable
 * stretches between them are not the walk's to know; zr_m3_walk fills
 * them in afterwards.
 */
static int
m3_merge(const struct zr_m3_file *from, const struct zr_m3_file *onto,
    struct zr_m3_entry *d13, uint32_t m1, struct zr_m3_entry *d23,
    uint32_t m2, struct zr_m3_chunk *raw, uint32_t *nraw, char *err,
    size_t errlen)
{
	struct zr_m3_entry *d1, *d2;
	uint32_t i = 0, j = 0, n = 0;
	uint32_t back, out;
	int t1, t2, dup;

	for (;;) {
		t1 = (i < m1);
		t2 = (j < m2);
		if (!t1 && !t2)
			break;
		d1 = t1 ? &d13[i] : NULL;
		d2 = t2 ? &d23[j] : NULL;

		/*
		 * Stuff peculiar to the first file. The test is strict:
		 * the two base ranges must have at least one base line
		 * between them, and two changes that merely touch fall
		 * through to the widening below and become one chunk.
		 */
		if (!t2 || (t1 && d1->base_hi < d2->base_lo)) {
			raw[n].kind = ZR_M3_FROM;
			raw[n].base_lo = d1->base_lo;
			raw[n].base_hi = d1->base_hi;
			raw[n].from_lo = d1->side_lo;
			raw[n].from_hi = d1->side_hi;
			n++;
			i++;
			continue;
		}

		/* Stuff peculiar to the second file. */
		if (!t1 || (t2 && d2->base_hi < d1->base_lo)) {
			raw[n].kind = ZR_M3_ONTO;
			raw[n].base_lo = d2->base_lo;
			raw[n].base_hi = d2->base_hi;
			raw[n].onto_lo = d2->side_lo;
			raw[n].onto_hi = d2->side_hi;
			n++;
			j++;
			continue;
		}

		/*
		 * Merge overlapping changes in the first script; this
		 * happens only after the widening below has reached one
		 * of them.
		 */
		if (i + 1 < m1 && d1->base_hi >= d13[i + 1].base_lo) {
			d13[i + 1].side_lo = d1->side_lo;
			d13[i + 1].base_lo = d1->base_lo;
			i++;
			continue;
		}

		/* And in the second. */
		if (j + 1 < m2 && d2->base_hi >= d23[j + 1].base_lo) {
			d23[j + 1].side_lo = d2->side_lo;
			d23[j + 1].base_lo = d2->base_lo;
			j++;
			continue;
		}

		/*
		 * The two changes cover one stretch of base, so both
		 * sides moved it. duplicate() says whether they moved it
		 * to the same lines: yes and the chunk is both-same, no
		 * and it is a conflict. That one clause is git's eager
		 * level and it is the whole of the refinement we do.
		 */
		if (d1->base_lo == d2->base_lo && d1->base_hi == d2->base_hi) {
			dup = m3_duplicate(from, d1->side_lo, d1->side_hi,
			    onto, d2->side_lo, d2->side_hi);
			raw[n].kind = dup ? ZR_M3_SAME : ZR_M3_CONFLICT;
			raw[n].base_lo = d1->base_lo;
			raw[n].base_hi = d1->base_hi;
			raw[n].from_lo = d1->side_lo;
			raw[n].from_hi = d1->side_hi;
			raw[n].onto_lo = d2->side_lo;
			raw[n].onto_hi = d2->side_hi;
			n++;
			i++;
			j++;
			continue;
		}

		/*
		 * Overlapping changes from the two sides: widen them
		 * until they coincide. Each entry's side range moves by
		 * exactly the number of base lines its base range
		 * gained, since over the lines being swallowed that side
		 * agrees with base line for line.
		 */
		if (d1->base_lo < d2->base_lo) {
			back = d2->base_lo - d1->base_lo;
			if (d2->side_lo < back) {
				m3_fail(err, errlen, "the onto script "
				    "widens past the start of its file");
				return (-1);
			}
			d2->side_lo -= back;
			d2->base_lo = d1->base_lo;
		} else if (d2->base_lo < d1->base_lo) {
			back = d1->base_lo - d2->base_lo;
			if (d1->side_lo < back) {
				m3_fail(err, errlen, "the from script "
				    "widens past the start of its file");
				return (-1);
			}
			d1->side_lo -= back;
			d1->base_lo = d2->base_lo;
		}
		if (d1->base_hi > d2->base_hi) {
			out = d1->base_hi - d2->base_hi;
			d2->side_hi += out;
			d2->base_hi = d1->base_hi;
		} else if (d2->base_hi > d1->base_hi) {
			out = d2->base_hi - d1->base_hi;
			d1->side_hi += out;
			d1->base_hi = d2->base_hi;
		}
	}
	*nraw = n;
	return (0);
}

/*
 * The chunk sequence: the walk's changed stretches with the stable
 * stretches filled in between them.
 *
 * What lies between two changed stretches is at least one base line,
 * matched on both sides, so all three files agree there line for line
 * and the three ranges have the same length. Three cursors run
 * together over base, from and onto; the gap before a changed stretch
 * is a stable chunk, and the same cursors give a from-only chunk its
 * onto range and an onto-only chunk its from range, which is what the
 * original's keep() computes with its delta. That the three stay in
 * step is the cheapest possible check on the walk, so it is asserted
 * as it goes, and checked again in a build with assertions compiled
 * out.
 */
int
zr_m3_walk(const struct zr_m3_file *base, const struct zr_m3_file *from,
    const struct zr_m3_file *onto, struct zr_m3_entry *d13, uint32_t m1,
    struct zr_m3_entry *d23, uint32_t m2, struct zr_m3_chunk **out,
    uint32_t *nout, char *err, size_t errlen)
{
	struct zr_m3_chunk *raw = NULL, *ch = NULL, *c;
	size_t rawcap, chcap;
	uint32_t nraw = 0, nch = 0, k;
	uint32_t bp = 0, fp = 0, op = 0, gap, span, tail;

	*out = NULL;
	*nout = 0;
	rawcap = (size_t)m1 + (size_t)m2;
	chcap = 2 * rawcap + 1;
	raw = calloc(rawcap != 0 ? rawcap : 1, sizeof (*raw));
	ch = calloc(chcap, sizeof (*ch));
	if (raw == NULL || ch == NULL) {
		m3_fail(err, errlen, "out of memory");
		goto fail;
	}
	if (m3_merge(from, onto, d13, m1, d23, m2, raw, &nraw, err,
	    errlen) != 0)
		goto fail;

	for (k = 0; k < nraw; k++) {
		struct zr_m3_chunk *r = &raw[k];

		if (r->base_lo < bp || r->base_hi < r->base_lo ||
		    r->base_hi > base->nlines) {
			m3_fail(err, errlen, "the walk left the chunks out "
			    "of order");
			goto fail;
		}
		gap = r->base_lo - bp;
		if (gap != 0) {
			c = &ch[nch++];
			c->kind = ZR_M3_STABLE;
			c->base_lo = bp;
			c->base_hi = r->base_lo;
			c->from_lo = fp;
			c->from_hi = fp + gap;
			c->onto_lo = op;
			c->onto_hi = op + gap;
			fp += gap;
			op += gap;
			bp = r->base_lo;
		}
		span = r->base_hi - r->base_lo;
		if (r->kind == ZR_M3_ONTO) {
			r->from_lo = fp;
			r->from_hi = fp + span;
		} else if (r->kind == ZR_M3_FROM) {
			r->onto_lo = op;
			r->onto_hi = op + span;
		}
		assert(r->from_lo == fp && r->onto_lo == op);
		if (r->from_lo != fp || r->onto_lo != op ||
		    r->from_hi < r->from_lo || r->onto_hi < r->onto_lo ||
		    r->from_hi > from->nlines || r->onto_hi > onto->nlines) {
			m3_fail(err, errlen, "a chunk does not abut the one "
			    "before it");
			goto fail;
		}
		ch[nch++] = *r;
		bp = r->base_hi;
		fp = r->from_hi;
		op = r->onto_hi;
	}

	tail = base->nlines - bp;
	assert(from->nlines - fp == tail && onto->nlines - op == tail);
	if (from->nlines - fp != tail || onto->nlines - op != tail) {
		m3_fail(err, errlen, "the three files do not end together");
		goto fail;
	}
	if (tail != 0) {
		c = &ch[nch++];
		c->kind = ZR_M3_STABLE;
		c->base_lo = bp;
		c->base_hi = base->nlines;
		c->from_lo = fp;
		c->from_hi = from->nlines;
		c->onto_lo = op;
		c->onto_hi = onto->nlines;
	}
	free(raw);
	*out = ch;
	*nout = nch;
	return (0);
fail:
	free(raw);
	free(ch);
	return (-1);
}
