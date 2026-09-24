#!/bin/sh
# Wipe what the box's hand sessions and scripts leave behind, so the
# next session starts on a clean box.
#
# usage: sh tests/box/wipe-hand.sh [-y]
#
# With no argument it prints what it would do and changes nothing.
# With -y it does it. Run it as root from the tool checkout: it takes
# each rebase away with the tool's own --abort.
#
# What it touches is only what these sessions make, by name, the way
# the box scripts' own leftover() does:
#
#   - the pools zrm (the hand sessions of box-trip-2026-09-16.md) and
#     zrtbox, zrtkill, zrtres, zrtprobe, zrtreplay, zrtprecond,
#     zrtstray, zrtlf and zrtscale (the tests/box scripts);
#   - the md device of each, found by its backing file, /tmp/POOL.img
#     or $TMPDIR/POOL.img, and never any other md device;
#   - the run directories under /var/db/zfs_rebase/POOL;
#   - the tmpfs at /tmp/zrp-tiny (hand session 4);
#   - the hand sessions' working directories under /tmp, named below.
#
# Order: every rebase in a pool is aborted first, so holds, private
# mounts and run directories go the tool's way; then the pool is
# destroyed with -f, which takes whatever an abort was refused (a
# zfs_rebase still running, say); then its md device and image; then
# any run directory the abort did not reach, with --abort on the
# directory itself, which takes one with nothing behind it; then the
# empty directories left.
set -u

POOLS="zrm zrtbox zrtkill zrtres zrtprobe zrtreplay zrtprecond zrtstray \
zrtlf zrtscale"
WORKDIR=/var/db/zfs_rebase
TINY=/tmp/zrp-tiny
MNTS="/tmp/zrm-mnt"
DIRS="/tmp/zrm /tmp/zrp /tmp/zrc /tmp/zrm-run /tmp/zrm-run-links \
/tmp/zrm-run-meta /tmp/zrm-run-app /tmp/zrm-run-r /tmp/zrm-run7 \
/tmp/zrm-run20"

go=0
case "${1:-}" in
-y)	go=1 ;;
"")	;;
*)	echo "usage: sh tests/box/wipe-hand.sh [-y]" >&2; exit 2 ;;
esac

cd "$(dirname "$0")/../.." || exit 2
[ "$(id -u)" -eq 0 ] || { echo "wipe-hand: run it as root" >&2; exit 2; }
[ -x ./zfs_rebase ] || { echo "wipe-hand: no ./zfs_rebase here" >&2; exit 2; }

# Say it, and do it only under -y.
run() {
	echo "  $*"
	[ $go -eq 1 ] || return 0
	"$@"
}

# Is anything mounted at or under this path?
mounted_under() {
	mount -p | awk -v p="$1" '$2 == p || index($2, p "/") == 1 \
	    { found = 1 } END { exit !found }'
}

# The md units whose backing file is this image.
md_units() {
	mdconfig -lv 2>/dev/null | \
	    awk -v img="$1" '$NF == img { sub(/^md/, "", $1); print $1 }'
}

[ $go -eq 1 ] || echo "wipe-hand: dry run; nothing changes without -y"

for pool in $POOLS; do
	if zpool list -H -o name "$pool" >/dev/null 2>&1; then
		echo "pool $pool"
		for ds in $(zfs get -H -r -s local -o name zfs_rebase:tag \
		    "$pool" 2>/dev/null); do
			run ./zfs_rebase --abort "$ds" || \
			    echo "  (the abort of $ds failed; the destroy takes it)"
		done
		run zpool destroy -f "$pool"
	fi
	for img in "/tmp/$pool.img" "${TMPDIR:-/tmp}/$pool.img"; do
		for u in $(md_units "$img"); do
			echo "md$u on $img"
			run mdconfig -d -u "$u"
		done
		if [ -f "$img" ]; then
			echo "image $img"
			run rm -f "$img"
		fi
	done
	if [ -d "$WORKDIR/$pool" ]; then
		echo "run directories under $WORKDIR/$pool"
		# a run directory is one with a private mount point in it
		for m in $(find "$WORKDIR/$pool" -type d -name mnt \
		    2>/dev/null); do
			d=${m%/mnt}
			[ -d "$d" ] || continue
			run ./zfs_rebase --abort "$d" || \
			    echo "  (the abort of $d failed)"
		done
		run find "$WORKDIR/$pool" -depth -type d -empty -delete
	fi
done

if mounted_under "$TINY"; then
	echo "tmpfs $TINY"
	run umount "$TINY"
fi
[ -d "$TINY" ] && run rmdir "$TINY"

for d in $DIRS; do
	[ -e "$d" ] || continue
	if mounted_under "$d"; then
		echo "left $d: something is mounted under it"
		continue
	fi
	echo "directory $d"
	run rm -rf "$d"
done

# Mount point directories: only the empty ones go.
for d in $MNTS; do
	[ -d "$d" ] || continue
	if mounted_under "$d"; then
		echo "left $d: something is mounted under it"
		continue
	fi
	echo "empty directories under $d"
	run find "$d" -depth -type d -empty -delete
done

if [ $go -eq 1 ]; then
	echo "wipe-hand: done"
	for pool in $POOLS; do
		zpool list -H -o name "$pool" >/dev/null 2>&1 && \
		    echo "wipe-hand: pool $pool is still here"
	done
	for d in $DIRS $MNTS $TINY; do
		[ -e "$d" ] && echo "wipe-hand: $d is still here"
	done
	left=$(mdconfig -l 2>/dev/null)
	[ -n "$left" ] && echo "wipe-hand: md devices not ours, left alone: $left"
fi
exit 0
