#!/bin/sh
# The mount probe on a pool of its own: an md-backed pool like the
# other harnesses make, a scratch dataset in it, tools/probe-mount.c
# run against it, and the pool gone again. The probe changes and
# restores the scratch dataset's mountpoint, canmount, readonly and
# sharenfs, and answers the four questions of documents-design.md
# section 5 in a lettered transcript; nothing of the tool runs.
#
#   sudo sh tests/box/run-probe.sh
#
# make probe-mount builds the probe on its own, off the flavor stamp,
# so a freebsd build/ is left as it was. KEEP=1 leaves the pool.
set -u
cd "$(dirname "$0")/../.." || exit 2
if [ "$(id -u)" != 0 ]; then
	echo "run-probe: must run as root"
	exit 2
fi
if [ "$(uname)" != FreeBSD ]; then
	echo "run-probe: FreeBSD only"
	exit 2
fi
[ -x build/probe-mount ] || make probe-mount || exit 2

POOL=zrtprobe
IMG=${TMPDIR:-/tmp}/${POOL}.img
MNT=${TMPDIR:-/tmp}/${POOL}-mnt
DIR=/var/db/zfs_rebase/probe
MD=
rc=2

cleanup() {
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL and $IMG left in place"
		return
	fi
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	rm -f "$IMG"
	rmdir "$MNT" 2>/dev/null
	rmdir "$DIR" 2>/dev/null
}
trap cleanup EXIT

truncate -s 128m "$IMG" || exit 2
MD=$(mdconfig -a -t vnode -f "$IMG") || exit 2
mkdir -p "$MNT"
zpool create -m "$MNT" "$POOL" "/dev/$MD" || exit 2
zfs create "$POOL/scratch" || exit 2

echo "== probe-mount $POOL/scratch $DIR"
build/probe-mount "$POOL/scratch" "$DIR"
rc=$?
echo "== probe-mount exited $rc (0: every property restored)"
exit $rc
