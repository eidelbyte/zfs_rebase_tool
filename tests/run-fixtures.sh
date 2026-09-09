#!/bin/sh
# End to end on any POSIX system: build every fixture as directories,
# run zfs_rebase --posix over them, and compare the manifest with the
# fixture's expect block from the #mode line on, which is where the
# decision starts: above it is the run, and a real run on the box
# writes another one there (v4-manifest.md, section 6). The run is
# made from the built directory, so the three names are base, from
# and onto, which is what tools/regen-expect.sh wrote into the block.
# A fixture whose name ends in -permissive.zrt runs with -p.
#
# Two things are held against the expect block, not one (ZF63): the
# manifest's body and the exit status the block implies. A --posix
# run exits 1 where the decision has a conflict and 0 where it has
# none (main.c), and the block's own "#conflicts" line says which,
# so a run that emits exactly the right document with the wrong
# status fails the fixture rather than passing it.
#
# One more thing the driver is held to here, and not the engine
# (ZF64): --build-fixture over a fixture of another platform prints
# the builder's own worded reason, naming the line and the platform,
# rather than the bare errno of ENOTSUP. That case only exists off
# the platform the fixture names, so on FreeBSD it is a skip.
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
root=$(pwd)
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
	(cd "$d" && "$root/$bin" --posix $flag -o got base from onto)
	st=$?
	conf=$(sed -n 's/^#conflicts  *\([0-9][0-9]*\).*/\1/p' "$d/expect")
	if [ -z "$conf" ]; then
		echo "FAIL $f (the expect block has no #conflicts line)"
		rc=1
		continue
	fi
	if [ "$conf" = 0 ]; then
		want=0
	else
		want=1
	fi
	sed -n '/^#mode/,$p' "$d/expect" > "$d/expect.body"
	sed -n '/^#mode/,$p' "$d/got" > "$d/got.body"
	if ! cmp -s "$d/expect.body" "$d/got.body"; then
		echo "FAIL $f (exit $st)"
		diff "$d/expect.body" "$d/got.body" | head -20
		rc=1
	elif [ "$st" -ne "$want" ]; then
		echo "FAIL $f (exit $st, want $want)"
		rc=1
	else
		echo "ok   $f (exit $st)"
		n=$((n + 1))
	fi
done
# ZF64: the off-platform refusal, in the builder's words.
p=tests/fixtures/freebsd/acl-nfsv4.zrt
if [ ! -f "$p" ]; then
	echo "FAIL $p is not there (ZF64)"; rc=1
elif [ "$host" = freebsd ]; then
	echo "skip ZF64 (it wants a host no fixture's platform line names)"
else
	d="$tmp/off-platform"
	mkdir -p "$d/base" || rc=1
	msg=$("$bin" --build-fixture "$p" "$d" 2>&1)
	st=$?
	case "$msg" in
	*"platform freebsd"*"builds on no other platform"*)
		if [ "$st" -eq 2 ]; then
			echo "ok   $p (off-platform refusal)"
		else
			echo "FAIL $p (off-platform refusal, exit $st, want 2)"
			rc=1
		fi
		;;
	*)
		echo "FAIL $p (off-platform refusal said: $msg)"
		rc=1
		;;
	esac
fi

# A fixture may have set an immutable or append-only flag, and rm(1)
# cannot remove what those hold down. The names are the same on
# FreeBSD and on macOS.
chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null
rm -rf "$tmp"
[ $rc -eq 0 ] && echo "run-fixtures: $n fixtures passed, $skipped skipped"
exit $rc
