#!/bin/sh
# Every fixture through run-fixture.sh, which runs each of them in
# both forms of the tool -- onto as a snapshot and the rebase in a
# clone, then onto as the live dataset and the rebase in it. FreeBSD,
# root, after make freebsd.
set -u
cd "$(dirname "$0")/../.." || exit 2
rc=0; n=0
for f in tests/fixtures/*.zrt tests/fixtures/freebsd/*.zrt; do
	log=/tmp/zr-suite-$(basename "$f").log
	if sh tests/box/run-fixture.sh "$f" > "$log" 2>&1; then
		echo "ok   $f"; n=$((n + 1))
	else
		echo "FAIL $f  (see $log)"
		# The last step's own heading, which names the form
		# the failure was in.
		sed -n 's/^== /     at: /p' "$log" | tail -1
		rc=1
	fi
done
[ $rc -eq 0 ] && echo "run-suite: $n fixtures passed in both forms"
exit $rc
