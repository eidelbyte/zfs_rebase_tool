#!/bin/sh
# Pre-commit gate: ASCII everywhere, cstyle over C sources.
# cstyle.pl is OpenZFS's, copied verbatim (CDDL, header intact).
# Portable: no grep -P, since BSD grep on macOS and FreeBSD lacks it.
set -u
cd "$(dirname "$0")/.." || exit 1
rc=0
nonascii=$(printf '[^\t -~]')
if LC_ALL=C grep -rn "$nonascii" Makefile README.md zfs_rebase.8 ports src \
    tests tools/xcheck-stub tools/*.sh tools/*.py tools/*.c; then
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
[ $rc -eq 0 ] && echo "gate: clean"
exit $rc
