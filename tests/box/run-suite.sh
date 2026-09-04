#!/bin/sh
# Every fixture through run-fixture.sh. FreeBSD, root, after make freebsd.
set -u
cd "$(dirname "$0")/../.." || exit 2
rc=0; n=0
for f in tests/fixtures/*.zrt tests/fixtures/freebsd/*.zrt; do
	if sh tests/box/run-fixture.sh "$f" > "/tmp/zr-suite-$(basename "$f").log" 2>&1; then
		echo "ok   $f"; n=$((n + 1))
	else
		echo "FAIL $f  (see /tmp/zr-suite-$(basename "$f").log)"; rc=1
	fi
done
[ $rc -eq 0 ] && echo "run-suite: $n fixtures passed"
exit $rc
