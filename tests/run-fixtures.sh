#!/bin/sh
# End to end on any POSIX system: build every fixture as directories,
# run zfs_rebase --posix over them, and compare the manifest with the
# fixture's expect block from the #mode line on (the header's dataset
# lines name directories here, and the note's example names datasets).
# A fixture whose name ends in -permissive.zrt runs with -p.
#
# A fixture with a "platform" line is that platform's alone, and
# tests/fixtures/freebsd/ is where those live -- out of the flat
# directory, which every host builds whole. One is skipped here
# unless this host is the platform it names and the run is root's:
# an ACL and the system extended-attribute namespace need both to be
# set, and a walk that may not read an attribute reads no attributes
# rather than failing, which would leave the fixture saying nothing.
# Skips are counted apart from passes.
set -u
cd "$(dirname "$0")/.." || exit 1
bin=./zfs_rebase
[ -x "$bin" ] || { echo "run-fixtures: build first (make)"; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-fixtures.XXXXXX") || exit 2
host=$(uname -s | tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz')
rc=0
n=0
skipped=0
for f in tests/fixtures/*.zrt tests/fixtures/freebsd/*.zrt; do
	[ -f "$f" ] || continue
	name=$(basename "$f" .zrt)
	plat=$(sed -n \
	    's/^[[:space:]]*platform[[:space:]][[:space:]]*\([a-z][a-z]*\).*/\1/p' \
	    "$f")
	if [ -n "$plat" ]; then
		why=""
		if [ "$plat" != "$host" ]; then
			why="platform $plat, this host is $host"
		elif [ "$(id -u)" != 0 ]; then
			why="platform $plat, and its attributes need root"
		fi
		if [ -n "$why" ]; then
			echo "skip $f ($why)"
			skipped=$((skipped + 1))
			continue
		fi
	fi
	d="$tmp/$name"
	mkdir -p "$d" || { rc=1; continue; }
	if ! "$bin" --build-fixture "$f" "$d"; then
		echo "FAIL build $f"; rc=1; continue
	fi
	if [ ! -f "$d/expect" ]; then
		echo "skip $f (no expect block)"
		skipped=$((skipped + 1))
		continue
	fi
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
# A fixture may have set an immutable or append-only flag, and rm(1)
# cannot remove what those hold down. The names are the same on
# FreeBSD and on macOS.
chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null
rm -rf "$tmp"
[ $rc -eq 0 ] && echo "run-fixtures: $n fixtures passed, $skipped skipped"
exit $rc
