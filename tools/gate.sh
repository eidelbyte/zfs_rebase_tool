#!/bin/sh
# Pre-commit gate, and exactly three checks: ASCII over every shipped
# file, cstyle over the C sources that are ours, and the PICKER=no
# link. It does NOT run tools/xcheck-freebsd.sh, which wants a FreeBSD
# source tree in FREEBSD_SRC and is run by hand; nor make check, which
# is a target of its own. What this says when it says "clean" is those
# three things and nothing more.
# cstyle.pl is OpenZFS's, copied verbatim (CDDL, header intact).
# Portable: no grep -P, since BSD grep on macOS and FreeBSD lacks it.
set -u
cd "$(dirname "$0")/.." || exit 1
rc=0
nonascii=$(printf '[^\t -~]')
# LICENSE is on the list because the port hands it to the framework as
# LICENSE_FILE_BSD3CLAUSE: it is shipped text of ours and the ASCII
# rule is unconditional (the review of 2026-09-11, B10, which found it
# outside the sweep). tools/cstyle.pl is the one shipped file
# deliberately left out, and it is left out by name here rather than
# by the accident of an extension list: it is carried OpenZFS code and
# it holds six lines with 0x01 bytes of its own, which it uses as a
# comment placeholder, so it cannot pass this check and must not be
# edited to. .gitignore is not shipped and is not swept.
if LC_ALL=C grep -rn "$nonascii" Makefile README.md LICENSE zfs_rebase.8 \
    ports src tests tools/xcheck-stub tools/*.sh tools/*.py tools/*.c; then
	echo "gate: non-ASCII bytes above"; rc=1
fi
# tools/xcheck-stub is stand-in headers for tools/xcheck-freebsd.sh and
# is compiled into nothing, but it is ours and it is read like any
# other header here, so it is held to the same style and the same
# ASCII rule as the rest.
srcs=$(ls src/*.c src/*.h src/plugins/*/*.c src/plugins/*/*.h tests/*.c \
    tools/probe-mount.c tools/xcheck-stub/*.h 2>/dev/null || true)
# The carried copy of FreeBSD's contrib/libdiff in
# src/plugins/picker/libdiff is foreign code (plan section 3.6, and
# that directory's UPSTREAM): it is never edited here, so cstyle does
# not judge it -- it keeps its own style. The globs above stop one
# level above it today; this filter keeps the exemption true if one of
# them is ever widened. The ASCII check above does still cover it, on
# purpose: every carried file is ASCII and must stay so.
srcs=$(printf '%s\n' $srcs | grep -v '^src/plugins/picker/libdiff/')
if [ -n "$srcs" ]; then
	if perl tools/cstyle.pl -cpP $srcs; then
		echo "gate: cstyle ok on $(echo $srcs | wc -w | tr -d ' ') files"
	else
		rc=1
	fi
fi
# The picker knob, so that PICKER=no cannot rot between one release
# and the next (tracker issue port-picker-option): the tool linked
# with src/plugins/picker/stub.c in the picker's place and no curses
# library on the line. A curses call, a picker symbol or a libdiff
# symbol reached from anywhere outside the picker fails that link,
# with nothing on it to satisfy them, which is the whole of the check.
# make nopicker links out of the tree's own build/ -- one compile, the
# stub's, and one link -- and writes build/zfs_rebase-nopicker, so it
# neither empties build/ nor touches ./zfs_rebase; the box builds the
# freebsd flavor and runs this gate after it, and there the artifact
# this gates is the freebsd PICKER=no tool, which is what the port's
# option-off package contains.
#
# This is a link and not a test run: no target runs the PICKER=no
# tests, and the box order (tests/box/README.md) is where that build
# is exercised, with make PICKER=no check-freebsd.
if out=$(${MAKE:-make} nopicker 2>&1); then
	echo "gate: PICKER=no builds, with no curses library on its link"
else
	printf '%s\n' "$out"
	echo "gate: the PICKER=no build failed (make nopicker)"; rc=1
fi
[ $rc -eq 0 ] && echo "gate: clean"
exit $rc
