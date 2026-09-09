#!/bin/sh
# Box timing: the one hazard review-cost named, and the syscall cell
# ZW35. FreeBSD, root, after make freebsd. Usage:
#
#	run-largefile.sh [--before COMMIT] [--dense-mb N] [--sparse-gb N]
#
# The byte comparison skips the holes two large files share by
# asking SEEK_DATA of both (src/yellow.c). ZFS answers SEEK_DATA for
# a file whose dnode is dirty by waiting for a txg to sync when
# vfs.zfs.dmu_offset_next_sync is 1 (the default), and with EBUSY
# when it is 0, which the tool takes as "read it all". The applying1
# self-check compares a result the apply wrote a moment before, so
# its large files are dirty exactly then. This script builds base
# with one dense file and one sparse one, edits both on from, and
# times the fresh clone-form run -- which reaches done, so the
# self-check and the final check are both in it -- once under each
# setting of the sysctl, and with --before once more with a binary
# built from COMMIT, which has no SEEK_DATA path at all. The sysctl
# is put back to what it was. Then ZW35: a dry run under truss, with
# the counts of the attribute calls that should be per walk
# (lpathconf) and per file (the descriptor variants) rather than per
# file for both. KEEP=1 leaves the pool; the scratch directory with
# the logs is kept either way. --dense-mb defaults to 1024 and
# --sparse-gb to 4; the image is IMGSIZE (default 12g).
set -u
cd "$(dirname "$0")/../.." || exit 2
bin=./zfs_rebase
before=
dense=1024
sparse=4
while [ $# -gt 0 ]; do
	case "$1" in
	--before) before=$2; shift 2 ;;
	--dense-mb) dense=$2; shift 2 ;;
	--sparse-gb) sparse=$2; shift 2 ;;
	*) echo "usage: run-largefile.sh [--before COMMIT] [--dense-mb N] [--sparse-gb N]"; exit 2 ;;
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

POOL=zrtlf
IMG=${TMPDIR:-/tmp}/${POOL}.img
IMGSIZE=${IMGSIZE:-12g}
MNT=${TMPDIR:-/tmp}/${POOL}-mnt
MD=
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-largefile.XXXXXX") || exit 2
here=$(pwd)
sysctl_was=$(sysctl -n vfs.zfs.dmu_offset_next_sync 2>/dev/null)

cleanup() {
	if [ -n "$sysctl_was" ]; then
		sysctl "vfs.zfs.dmu_offset_next_sync=$sysctl_was" > /dev/null 2>&1
	fi
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
	if [ -d "$tmp/before" ]; then
		git worktree remove --force "$tmp/before" 2>/dev/null
	fi
	echo "logs and results: $tmp"
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
# A pool of this name left by a run that was interrupted before its
# cleanup ran is ours, and goes first, with the md its image is on.
leftover() {
	zpool destroy -f "$POOL" 2>/dev/null
	for u in $(mdconfig -lv 2>/dev/null | \
	    awk -v img="$IMG" '$NF == img { sub(/^md/, "", $1); print $1 }'); do
		mdconfig -d -u "$u" 2>/dev/null
	done
	rm -f "$IMG"
}
say() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*"; exit 1; }
timed() {	# LOG CMD...
	log=$1
	shift
	/usr/bin/time -l "$@" > "$log" 2>&1
}
numbers() {	# LOG
	awk '/ real / { r=$1; u=$3; s=$5 }
	    /maximum resident set size/ { rss=$1 }
	    END { printf "%s %s %s %s\n", r, u, s, rss }' "$1"
}
[ -n "$sysctl_was" ] || \
    fail "no vfs.zfs.dmu_offset_next_sync on this kernel"

say "the binaries"
if [ -n "$before" ]; then
	git worktree add "$tmp/before" "$before" > "$tmp/worktree.log" 2>&1 || \
	    { cat "$tmp/worktree.log"; fail "cannot make a worktree at $before"; }
	(cd "$tmp/before" && make freebsd) > "$tmp/before-build.log" 2>&1 || \
	    { tail -20 "$tmp/before-build.log"; fail "the build at $before failed"; }
	echo "before: $before"
fi
echo "current: $(git rev-parse --short HEAD)"

say "the pool, and base made in its dataset"
leftover
truncate -s "$IMGSIZE" "$IMG" || exit 2
MD=$(mdconfig -a -t vnode -f "$IMG") || exit 2
mkdir -p "$MNT"
zpool create -m "$MNT" -O casesensitivity=sensitive -O normalization=none \
    "$POOL" "/dev/$MD" || exit 2
zfs create "$POOL/base" || exit 2
b=$MNT/base
dd if=/dev/urandom of="$b/dense" bs=1m count="$dense" > /dev/null 2>&1 || \
    fail "dense"
truncate -s "${sparse}g" "$b/sparse" || fail "sparse"
dd if=/dev/urandom of="$b/sparse" bs=1m count=8 conv=notrunc \
    > /dev/null 2>&1 || fail "sparse island 1"
dd if=/dev/urandom of="$b/sparse" bs=1m count=8 seek=$((sparse * 1024 - 100)) \
    conv=notrunc > /dev/null 2>&1 || fail "sparse island 2"
echo "beside them" > "$b/small" || fail "small"
zfs snapshot "$POOL/base@base" || exit 2
for side in from onto; do
	zfs clone "$POOL/base@base" "$POOL/$side" || exit 2
done
# from edits the last block of each; onto is base
f=$MNT/from
dd if=/dev/urandom of="$f/dense" bs=1m count=1 seek=$((dense - 1)) \
    conv=notrunc > /dev/null 2>&1 || fail "edit dense"
dd if=/dev/urandom of="$f/sparse" bs=1m count=1 seek=$((sparse * 1024 - 96)) \
    conv=notrunc > /dev/null 2>&1 || fail "edit sparse"
zfs snapshot "$POOL/from@work" "$POOL/onto@work" || exit 2
echo "ok   dense ${dense}m and sparse ${sparse}g, both edited on from"

# One fresh clone-form run, which reaches done: the apply writes the
# two files and the self-check reads them back while they are dirty.
one_run() {	# LABEL BIN
	timed "$tmp/$1.log" "$2" -v --from "$POOL/from@work" \
	    --onto "$POOL/onto@work" --result "$POOL/res"
	st=$?
	[ $st -eq 0 ] || { tail -20 "$tmp/$1.log"; fail "$1 exited $st"; }
	printf '%-22s %s\n' "$1" "$(numbers "$tmp/$1.log")" >> "$tmp/results.txt"
	grep -h 'bytes compared' "$tmp/$1.log" | sed "s/^/     $1: /"
	zfs destroy "$POOL/res" || fail "cannot destroy $POOL/res"
}

printf '%-22s %s\n' run "real user sys rss" > "$tmp/results.txt"
say "warming: one untimed run"
one_run warm "$here/$bin" > /dev/null
sed -i '' '/^warm /d' "$tmp/results.txt"
for setting in 1 0; do
	say "current binary, vfs.zfs.dmu_offset_next_sync=$setting"
	sysctl "vfs.zfs.dmu_offset_next_sync=$setting" > /dev/null || \
	    fail "cannot set the sysctl"
	one_run "current-sync$setting" "$here/$bin"
done
sysctl "vfs.zfs.dmu_offset_next_sync=$sysctl_was" > /dev/null
if [ -n "$before" ]; then
	say "before binary ($before), sysctl as it was ($sysctl_was)"
	one_run "before" "$tmp/before/zfs_rebase"
fi

say "ZW35: the attribute calls of a dry run, under truss"
truss -f -o "$tmp/truss.out" "$bin" -n --from "$POOL/from@work" \
    --onto "$POOL/onto@work" > "$tmp/truss-run.log" 2>&1 || \
    { tail -5 "$tmp/truss-run.log"; fail "the dry run under truss"; }
{
	for call in lpathconf extattr_list_fd extattr_get_fd acl_get_fd_np \
	    extattr_list_link extattr_get_link acl_get_link_np openat fstatat; do
		printf '%-18s %6s\n' "$call" "$(grep -c "^[0-9]*: $call(\|[ :]$call(" "$tmp/truss.out")"
	done
} | tee "$tmp/truss-counts.txt"
echo "     (three trees of three names each: lpathconf should be a few, not dozens)"

say "results (seconds; RSS in bytes)"
cat "$tmp/results.txt"
echo "sysctl vfs.zfs.dmu_offset_next_sync is back at $sysctl_was"
echo "ok   run-largefile: results in $tmp/results.txt"
exit 0
