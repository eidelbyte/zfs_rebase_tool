#!/bin/sh
#
# merge-oracle.sh -- run the merge battery through outside oracles and
# print where they disagree with our reference walk.
#
# For every case in tests/battery/merge/merge3.txt this writes the three
# files out, asks the reference implementation
# (zfs-rebase-theory/v4-merge3check.py, the checker that exported the
# battery) for the merged text with every conflict left as a conflict,
# and compares that against
#
#	git merge-file -p --diff3 FROM BASE ONTO
#	diff3 -m FROM BASE ONTO		(on FreeBSD only)
#
# git merge-file takes its files as current, base, other and diff3 -m
# takes mine, older, yours, so from base onto is the right order for
# both. --diff3 is not only how base is made to show: it is also what
# clamps git to its "eager" level, which is our ceiling (v4-merge3.md
# section 4), so without it the comparison would be against a different
# algorithm.
#
# This is FOR READING, NOT A GATE. It exits 0 whether or not the oracles
# agree, and nothing in the build depends on it. The battery in
# tests/battery/merge/ is the acceptance bar; this script is a second
# opinion.
#
# Two disagreements with diff3 -m are expected, by design, and are not
# bugs (v4-merge3.md sections 3.5 and 4):
#
#   An identical change on both sides. diff3 -m implies -A, which
#   brackets it as a conflict; git at its eager level and we do not.
#   Every both-same case in the battery will say diff3 DIFFERS.
#
#   A conflict whose chosen side ends without a final newline. diff3
#   -m copies the incomplete line as it finds it, so the next marker
#   is appended to it rather than starting a line of its own; git
#   terminates the line first, and so do we, since a marker that is
#   not at the start of a line cannot be read back at all.
#
# The tool itself may never do what this script does. The sprint's
# ground rules forbid a temp file and forbid parsing another program's
# output, and both oracles need both: files on disk to hand them and a
# marker-laden text to read back. The tool merges in process, over
# libdiff's chunk lists and the adapted diff3 walk, and its answer is a
# chunk sequence rather than a text. A test may shell out where the tool
# may not, which is the whole reason this file exists.
#
# usage: sh tools/merge-oracle.sh [BATTERY]
# env:   MERGE3CHECK        path to v4-merge3check.py, if it is not
#                           beside the repository in
#                           ../zfs-rebase-theory or ../../
#        MERGE_ORACLE_DIFF3 a diff3 to run even off FreeBSD. macOS 13
#                           and later ship FreeBSD's diff3 as
#                           /usr/bin/diff3, so the second oracle can be
#                           read on the mac with
#                           MERGE_ORACLE_DIFF3=/usr/bin/diff3.

set -u

cd "$(dirname "$0")/.." || exit 0
root=$(pwd)

battery=${1:-tests/battery/merge/merge3.txt}
if [ ! -f "$battery" ]; then
	echo "merge-oracle: no battery at $battery"
	exit 0
fi

checker=${MERGE3CHECK:-}
if [ -z "$checker" ]; then
	for c in ../zfs-rebase-theory/v4-merge3check.py \
	    ../../zfs-rebase-theory/v4-merge3check.py \
	    ../../../zfs-rebase-theory/v4-merge3check.py; do
		if [ -f "$root/$c" ]; then
			checker=$root/$c
			break
		fi
	done
fi
if [ -z "$checker" ] || [ ! -f "$checker" ]; then
	echo "merge-oracle: v4-merge3check.py not found; set MERGE3CHECK."
	echo "merge-oracle: it lives in the freebsd-development repository,"
	echo "merge-oracle: which a clone of this one does not carry."
	exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "merge-oracle: no python3 on this host; nothing to compare."
	exit 0
fi

scratch=${TMPDIR:-/tmp}/merge-oracle.$$
trap 'rm -rf "$scratch"' 0 1 2 3 15
mkdir -p "$scratch" || exit 0

# Split the battery into one base, from and onto file per case, and
# print the case names. The format is line oriented on purpose: this
# awk and tests/check_picker.c read the same file.
cases=$(awk -v dir="$scratch" '
	/^#/ { next }
	$1 == "case" { name = $2; print name; next }
	$1 == "base" || $1 == "from" || $1 == "onto" {
		f = dir "/" name "." $1
		printf("") > f
		n = $2 + 0
		for (i = 0; i < n; i++) {
			getline
			t = (length($0) > 1) ? substr($0, 3) : ""
			if (substr($0, 1, 1) == "L")
				printf("%s\n", t) > f
			else
				printf("%s", t) > f
		}
		close(f)
		next
	}
' "$battery")

have_git=no
command -v git >/dev/null 2>&1 && have_git=yes
diff3prog=${MERGE_ORACLE_DIFF3:-}
if [ -z "$diff3prog" ] && [ "$(uname)" = "FreeBSD" ]; then
	command -v diff3 >/dev/null 2>&1 && diff3prog=diff3
fi
have_diff3=no
[ -n "$diff3prog" ] && have_diff3=yes

echo "merge-oracle: battery $battery on $(uname)"
echo "merge-oracle: git merge-file $have_git,"\
    "diff3 -m $have_diff3 ${diff3prog:+($diff3prog)}"
echo

ngit_ok=0; ngit_bad=0; nd3_ok=0; nd3_bad=0; ncase=0
for name in $cases; do
	ncase=$((ncase + 1))
	b=$scratch/$name.base
	f=$scratch/$name.from
	o=$scratch/$name.onto
	ref=$scratch/$name.ref
	python3 "$checker" --merged "$name" > "$ref" || {
		echo "$name: the reference walk failed"
		continue
	}
	line="$name:"
	if [ "$have_git" = yes ]; then
		git merge-file -p --diff3 -L from -L base -L onto \
		    "$f" "$b" "$o" > "$scratch/$name.git" 2>/dev/null
		if cmp -s "$ref" "$scratch/$name.git"; then
			line="$line git ok"
			ngit_ok=$((ngit_ok + 1))
		else
			line="$line git DIFFERS"
			ngit_bad=$((ngit_bad + 1))
		fi
	fi
	if [ "$have_diff3" = yes ]; then
		"$diff3prog" -m -L from -L base -L onto \
		    "$f" "$b" "$o" > "$scratch/$name.d3" 2>/dev/null
		if cmp -s "$ref" "$scratch/$name.d3"; then
			line="$line diff3 ok"
			nd3_ok=$((nd3_ok + 1))
		else
			line="$line diff3 DIFFERS"
			nd3_bad=$((nd3_bad + 1))
		fi
	fi
	echo "$line"
	for which in git d3; do
		out=$scratch/$name.$which
		[ -f "$out" ] || continue
		cmp -s "$ref" "$out" && continue
		echo "  --- ours against $which ---"
		diff -u "$ref" "$out" | sed 's/^/  /'
	done
done

echo
echo "merge-oracle: $ncase cases;" \
    "git $ngit_ok agree $ngit_bad differ;" \
    "diff3 $nd3_ok agree $nd3_bad differ"
exit 0
