#!/bin/sh
# Box harness: build one fixture as real datasets on a throwaway pool,
# run zfs_rebase for real, and check it. FreeBSD, root, after
# make freebsd. Usage: run-fixture.sh FIXTURE.zrt   (KEEP=1 to leave
# the pool behind for inspection).
#
# What is checked, in order:
#   1. -n: the manifest equals the fixture's expect block from the
#      #mode line on (the header names datasets here);
#   2. the real run: exit 0 for a clean fixture, 1 for a conflicted
#      one, and the manifest again equal;
#   3. for a clean fixture the working clone exists, is read-only,
#      and rebasing from onto it again in --posix mode yields a
#      manifest with zero actions and zero conflicts (idempotence:
#      the clone already holds from's changes).
#
# The from and onto datasets are made by clearing a clone of base and
# extracting the fixture's tree with tar, so every object looks
# changed to zfs diff; the unchanged-pool pruning is exercised only
# in its negative direction here. A replay that edits in place is
# the next harness.
set -u
fixture=${1:?usage: run-fixture.sh FIXTURE.zrt}
cd "$(dirname "$0")/../.." || exit 2
bin=./zfs_rebase
[ -x "$bin" ] || { echo "build first: make freebsd"; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 2; }
[ "$(uname)" = FreeBSD ] || { echo "FreeBSD only"; exit 2; }

POOL=zrtbox
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
MD=
tmp=$(mktemp -d /tmp/zr-box.XXXXXX) || exit 2
rc=1

cleanup() {
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	rm -f "$IMG"
	rm -rf "$tmp"
	rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT
say() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*"; exit 1; }

say "fixture $fixture"
"$bin" --build-fixture "$fixture" "$tmp" || fail "build-fixture"
[ -f "$tmp/expect" ] || fail "no expect block"
flag=""
case "$fixture" in *-permissive.zrt) flag="-p" ;; esac

say "pool"
truncate -s 512m "$IMG" || exit 2
MD=$(mdconfig -a -t vnode -f "$IMG") || exit 2
mkdir -p "$MNT"
zpool create -m "$MNT" -O casesensitivity=sensitive -O normalization=none \
    "$POOL" "/dev/$MD" || exit 2
zfs create "$POOL/base" || exit 2
(cd "$tmp/base" && tar -cf - .) | (cd "$MNT/base" && tar -xpf -) || fail "populate base"
zfs snapshot "$POOL/base@base" || exit 2
for side in from onto; do
	zfs clone "$POOL/base@base" "$POOL/$side" || exit 2
	# clear, then extract the fixture's tree for this side
	(cd "$MNT/$side" && find . -mindepth 1 -maxdepth 1 -exec rm -rf {} +) || fail "clear $side"
	(cd "$tmp/$side" && tar -cf - .) | (cd "$MNT/$side" && tar -xpf -) || fail "populate $side"
done

say "1. dry run"
"$bin" -n $flag -o "$tmp/got-n" "$POOL/base@base" "$POOL/from" "$POOL/onto"
st=$?
sed -n '/^#mode/,$p' "$tmp/expect" > "$tmp/expect.body"
sed -n '/^#mode/,$p' "$tmp/got-n" > "$tmp/got-n.body"
cmp -s "$tmp/expect.body" "$tmp/got-n.body" || { diff "$tmp/expect.body" "$tmp/got-n.body" | head -20; fail "dry-run manifest differs"; }
echo "ok   dry run (exit $st)"

say "2. real run"
"$bin" $flag -v -o "$tmp/got" "$POOL/base@base" "$POOL/from" "$POOL/onto"
st=$?
sed -n '/^#mode/,$p' "$tmp/got" > "$tmp/got.body"
cmp -s "$tmp/expect.body" "$tmp/got.body" || { diff "$tmp/expect.body" "$tmp/got.body" | head -20; fail "real-run manifest differs"; }
if grep -q '^#conflicts 0$' "$tmp/expect"; then
	[ $st -eq 0 ] || fail "clean fixture exited $st"
	echo "ok   real run applied (exit 0)"
else
	[ $st -eq 1 ] || fail "conflicted fixture exited $st, want 1"
	echo "ok   real run stopped on conflicts (exit 1)"
	rc=0
	exit 0
fi

say "3. the result"
clone=$(zfs list -H -o name -t filesystem | grep "^$POOL/onto-rebase-" | head -1)
[ -n "$clone" ] || fail "no working clone"
[ "$(zfs get -H -o value readonly "$clone")" = on ] || fail "clone is not read-only"
cmnt=$(zfs get -H -o value mountpoint "$clone")
"$bin" --posix $flag -o "$tmp/again" "$tmp/base" "$tmp/from" "$cmnt"
st=$?
grep -q '^#actions 0$' "$tmp/again" && grep -q '^#conflicts 0$' "$tmp/again" \
    || { sed -n '1,30p' "$tmp/again"; fail "rebasing onto the result is not a no-op (exit $st)"; }
echo "ok   result: rebasing from onto the clone again is a no-op"
rc=0
exit 0
