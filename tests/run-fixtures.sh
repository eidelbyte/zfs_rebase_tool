#!/bin/sh
# End to end on any POSIX system: build every fixture as directories,
# run zfs_rebase --posix over them, and compare the manifest with the
# fixture's expect block from the #mode line on (the header's dataset
# lines name directories here, and the note's example names datasets).
# A fixture whose name ends in -permissive.zrt runs with -p.
set -u
cd "$(dirname "$0")/.." || exit 1
bin=./zfs_rebase
[ -x "$bin" ] || { echo "run-fixtures: build first (make)"; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-fixtures.XXXXXX") || exit 2
rc=0
n=0
for f in tests/fixtures/*.zrt; do
	name=$(basename "$f" .zrt)
	d="$tmp/$name"
	mkdir -p "$d" || { rc=1; continue; }
	if ! "$bin" --build-fixture "$f" "$d"; then
		echo "FAIL build $f"; rc=1; continue
	fi
	[ -f "$d/expect" ] || { echo "skip $f (no expect block)"; continue; }
	flag=""
	case "$name" in *-permissive) flag="-p" ;; esac
	"$bin" --posix $flag -o "$d/got" "$d/base" "$d/from" "$d/onto"
	st=$?
	sed -n '/^#mode/,$p' "$d/expect" > "$d/expect.body"
	sed -n '/^#mode/,$p' "$d/got" > "$d/got.body"
	if cmp -s "$d/expect.body" "$d/got.body"; then
		echo "ok   $f (exit $st)"
		n=$((n + 1))
	else
		echo "FAIL $f (exit $st)"
		diff "$d/expect.body" "$d/got.body" | head -20
		rc=1
	fi
done
rm -rf "$tmp"
[ $rc -eq 0 ] && echo "run-fixtures: $n fixtures passed"
exit $rc
