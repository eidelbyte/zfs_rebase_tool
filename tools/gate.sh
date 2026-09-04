#!/bin/sh
# Pre-commit gate: ASCII everywhere, cstyle over C sources.
# cstyle.pl is OpenZFS's, copied verbatim (CDDL, header intact).
# Portable: no grep -P, since BSD grep on macOS and FreeBSD lacks it.
set -u
cd "$(dirname "$0")/.." || exit 1
rc=0
nonascii=$(printf '[^\t -~]')
if LC_ALL=C grep -rn "$nonascii" Makefile README.md zfs_rebase.8 src tests \
    tools/gate.sh tools/xcheck-freebsd.sh tools/*.py; then
	echo "gate: non-ASCII bytes above"; rc=1
fi
srcs=$(ls src/*.c src/*.h tests/*.c 2>/dev/null || true)
if [ -n "$srcs" ]; then
	if perl tools/cstyle.pl -cpP $srcs; then
		echo "gate: cstyle ok on $(echo $srcs | wc -w | tr -d ' ') files"
	else
		rc=1
	fi
fi
[ $rc -eq 0 ] && echo "gate: clean"
exit $rc
