#!/bin/sh
# Box timing: the numbers sprints/sprint-5/scale-timing.md section 7
# still owes, over ZFS in the real mode. FreeBSD, root, after
# make freebsd. Usage:
#
#	run-scale.sh [--names N] [--before COMMIT] [--repeats R]
#
# The tree comes from tools/gen-big-tree.py, which is deterministic
# from its seed, and lands in three datasets: base copied in with
# tar, from and onto as clones of base@base brought to their trees by
# rsync in place, so that an object neither side changed keeps
# base's object number and ctime and the prune has something to
# prune. Copying the sides in whole would give every object a new
# inode and measure the prune at zero.
#
# Then, for the binary in this checkout and, with --before, for one
# built from COMMIT in a throwaway worktree, R warm runs each of two
# commands: the fresh run to the conflicts gate (the walks, the
# compare, applying1 and its self-check), and the --continue that
# takes an answered resolution through applying2 and the final check
# to done. Every run goes under /usr/bin/time -l. The medians are
# printed at the end and left with the logs in the scratch
# directory, which is kept. --names defaults to 50000; the 200000
# scenario of the note wants an image of 16g (IMGSIZE=16g). KEEP=1
# leaves the pool. Needs python3 and rsync (pkg install python3
# rsync).
set -u
cd "$(dirname "$0")/../.." || exit 2
bin=./zfs_rebase
names=50000
before=
repeats=3
while [ $# -gt 0 ]; do
	case "$1" in
	--names) names=$2; shift 2 ;;
	--before) before=$2; shift 2 ;;
	--repeats) repeats=$2; shift 2 ;;
	*) echo "usage: run-scale.sh [--names N] [--before COMMIT] [--repeats R]"; exit 2 ;;
	esac
done
[ -x "$bin" ] || { echo "build first: make freebsd"; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 2; }
[ "$(uname)" = FreeBSD ] || { echo "FreeBSD only"; exit 2; }
if "$bin" --abort zr-flavor-probe/none 2>&1 |
    grep -q 'not built with ZR_FREEBSD'; then
	echo "$bin is the portable build: make clean && make freebsd"
	exit 2
fi
command -v python3 > /dev/null 2>&1 || { echo "pkg install python3"; exit 2; }
command -v rsync > /dev/null 2>&1 || { echo "pkg install rsync"; exit 2; }

POOL=zrtscale
IMG=${TMPDIR:-/tmp}/${POOL}.img
IMGSIZE=${IMGSIZE:-8g}
MNT=${TMPDIR:-/tmp}/${POOL}-mnt
MD=
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-scale.XXXXXX") || exit 2
here=$(pwd)

cleanup() {
	"$bin" --abort "$POOL/res" > /dev/null 2>&1
	zfs destroy "$POOL/res" > /dev/null 2>&1
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	rm -f "$IMG"
	rmdir "$MNT" 2>/dev/null
	rm -rf "$tmp/big"
	if [ -d "$tmp/before" ]; then
		git worktree remove --force "$tmp/before" 2>/dev/null
	fi
	echo "logs and results: $tmp"
}
trap cleanup EXIT
say() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*"; exit 1; }

# One timed command: the whole of its output and time's report go to
# LOG, and its exit status comes back.
timed() {	# LOG CMD...
	log=$1
	shift
	/usr/bin/time -l "$@" > "$log" 2>&1
}
# The four numbers out of a log: real, user, sys and the peak RSS.
numbers() {	# LOG
	awk '/ real / { r=$1; u=$3; s=$5 }
	    /maximum resident set size/ { rss=$1 }
	    END { printf "%s %s %s %s\n", r, u, s, rss }' "$1"
}
# The median of the numbers on stdin, one per line.
median() {
	sort -n | awk '{ a[NR]=$1 } END {
	    if (NR == 0) { print "-"; exit }
	    if (NR % 2) print a[(NR+1)/2]; else print (a[NR/2]+a[NR/2+1])/2 }'
}

say "the binaries"
bins="current:$here/$bin"
if [ -n "$before" ]; then
	git worktree add "$tmp/before" "$before" > "$tmp/worktree.log" 2>&1 || \
	    { cat "$tmp/worktree.log"; fail "cannot make a worktree at $before"; }
	(cd "$tmp/before" && make freebsd) > "$tmp/before-build.log" 2>&1 || \
	    { tail -20 "$tmp/before-build.log"; fail "the build at $before failed"; }
	bins="before:$tmp/before/zfs_rebase $bins"
	echo "before: $before"
fi
echo "current: $(git rev-parse --short HEAD)"

say "the tree: $names names, seed 1"
python3 tools/gen-big-tree.py --names "$names" --seed 1 "$tmp/big" \
    > "$tmp/gen.log" 2>&1 || { cat "$tmp/gen.log"; fail "gen-big-tree"; }
tail -3 "$tmp/gen.log"

say "the pool"
truncate -s "$IMGSIZE" "$IMG" || exit 2
MD=$(mdconfig -a -t vnode -f "$IMG") || exit 2
mkdir -p "$MNT"
zpool create -m "$MNT" -O casesensitivity=sensitive -O normalization=none \
    "$POOL" "/dev/$MD" || exit 2
zfs create "$POOL/base" || exit 2
(cd "$tmp/big/base" && tar -cf - .) | (cd "$MNT/base" && tar -xpf -) || \
    fail "populate base"
zfs snapshot "$POOL/base@base" || exit 2
for side in from onto; do
	zfs clone "$POOL/base@base" "$POOL/$side" || exit 2
	rsync -aHc --inplace --delete "$tmp/big/$side/" "$MNT/$side/" \
	    > "$tmp/rsync-$side.log" 2>&1 || \
	    { tail -5 "$tmp/rsync-$side.log"; fail "rsync $side"; }
done
zfs snapshot "$POOL/from@work" "$POOL/onto@work" || exit 2
echo "ok   base, from and onto as datasets; $(zfs list -H -o used "$POOL") used"

# One rebase, fresh to the gate and --continue to done, timed as two.
# The fresh run is 1 with conflicts, 0 where it reached done on its
# own (a tree with no overlapping edits); the resolution is answered
# keep by hand, as the harnesses do.
# (sh functions have no locals: the names here are the function's
# own, and the loop below keeps label, b and i for itself.)
one_run() {	# LABEL BIN N
	r_label=$1
	r_bin=$2
	r_n=$3
	timed "$tmp/$r_label-fresh-$r_n.log" "$r_bin" -v --from "$POOL/from@work" \
	    --onto "$POOL/onto@work" --result "$POOL/res"
	r_st=$?
	[ $r_st -eq 0 ] || [ $r_st -eq 1 ] || \
	    { tail -20 "$tmp/$r_label-fresh-$r_n.log"; fail "$r_label run $r_n exited $r_st"; }
	numbers "$tmp/$r_label-fresh-$r_n.log" >> "$tmp/$r_label-fresh.numbers"
	grep -h 'pools unchanged\|bytes compared' "$tmp/$r_label-fresh-$r_n.log" | \
	    sed "s/^/     $r_label fresh $r_n: /"
	if [ $r_st -eq 1 ]; then
		r_res=/var/db/zfs_rebase/$POOL/res/resolution
		[ -f "$r_res" ] || fail "no resolution at $r_res"
		sed -i '' -e 's/ -$/ keep/' -e 's/^#unanswered .*$/#unanswered 0/' \
		    "$r_res" || fail "cannot answer $r_res"
		timed "$tmp/$r_label-cont-$r_n.log" "$r_bin" -v --continue "$POOL/res"
		r_st=$?
		[ $r_st -eq 0 ] || \
		    { tail -20 "$tmp/$r_label-cont-$r_n.log"; fail "$r_label --continue $r_n exited $r_st"; }
		numbers "$tmp/$r_label-cont-$r_n.log" >> "$tmp/$r_label-cont.numbers"
	fi
	zfs destroy "$POOL/res" || fail "cannot destroy $POOL/res"
}

for entry in $bins; do
	label=${entry%%:*}
	b=${entry#*:}
	say "$label: one untimed run to warm the cache, then $repeats timed"
	one_run "$label-warm" "$b" 0 > /dev/null
	rm -f "$tmp/$label-warm-fresh.numbers" "$tmp/$label-warm-cont.numbers"
	i=1
	while [ $i -le "$repeats" ]; do
		one_run "$label" "$b" $i
		i=$((i + 1))
	done
done

say "medians of $repeats (seconds; RSS in bytes)"
{
	printf '%-8s %-6s %10s %10s %10s %14s\n' binary run real user sys rss
	for entry in $bins; do
		label=${entry%%:*}
		for phase in fresh cont; do
			f=$tmp/$label-$phase.numbers
			[ -f "$f" ] || continue
			printf '%-8s %-6s %10s %10s %10s %14s\n' "$label" "$phase" \
			    "$(awk '{print $1}' "$f" | median)" \
			    "$(awk '{print $2}' "$f" | median)" \
			    "$(awk '{print $3}' "$f" | median)" \
			    "$(awk '{print $4}' "$f" | median)"
		done
	done
	echo
	echo "names $names, seed 1, repeats $repeats, pool $POOL on $IMGSIZE"
	echo "current $(git rev-parse --short HEAD)${before:+, before $before}"
} | tee "$tmp/results.txt"
echo "ok   run-scale: results in $tmp/results.txt"
exit 0
