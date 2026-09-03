#!/bin/sh
# Box harness: build one fixture as real datasets on a throwaway pool,
# run zfs_rebase for real, and check it. FreeBSD, root, after
# make freebsd. Usage: run-fixture.sh FIXTURE.zrt   (KEEP=1 to leave
# the pool behind for inspection).
#
# The tool takes no snapshots, so the harness takes them: base@base
# when base is populated, and from@work and onto@work when the two
# sides are. Only the last two are given to the run, which derives
# base@base for itself as the point the two sides branched from; the
# result clone is named too, as $POOL/result.
#
# What is checked, in order:
#   0. the derivation refuses what it should: a linear pair, where
#      one side is an ancestor of the other, and a pair that shares
#      no origin at all -- both exit 2;
#   1. -n: the manifest equals the fixture's expect block from the
#      #mode line on (the header names datasets here), and its #base
#      line is the snapshot the two sides were cloned from;
#   2. the real run, with --off-of for --from: exit 0 for a clean
#      fixture, 1 for a conflicted one, the manifest again equal and
#      the same #base derived; then that no hold is left on any of
#      the three snapshots, since they die with the cleanup
#      descriptor at exit;
#   3. the result: it is exactly $POOL/result, read-only, mounted at
#      /var/run/zfs_rebase/$POOL/result/mnt, carrying zfs_rebase:state
#      "applied" for a clean fixture and "conflicts" for a conflicted
#      one; and for a clean fixture, that the manifest file is where
#      -o put it and that rebasing from onto the result again in
#      --posix mode yields a manifest with zero actions and zero
#      conflicts (idempotence: the result already holds from's
#      changes);
#   4. --abort: exit 0, the dataset gone, the recorded manifest
#      unlinked, the run directory gone down to /var/run/zfs_rebase,
#      and a second --abort exit 2 because there is no such run.
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
RUNDIR=/var/run/zfs_rebase/$POOL/result
MD=
tmp=$(mktemp -d /tmp/zr-box.XXXXXX) || exit 2
rc=1

cleanup() {
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	# A failed step can leave the result clone holding onto@work,
	# and its directory under /var/run; --abort is what removes
	# both, and zpool destroy -f would not touch the directory.
	"$bin" --abort --result "$POOL/result" >/dev/null 2>&1
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
zfs snapshot "$POOL/from@work" "$POOL/onto@work" || exit 2

say "0. the base derivation refuses what it should"
# base@base is an ancestor of onto@work: onto already contains it.
"$bin" -n --from "$POOL/base@base" --onto "$POOL/onto@work" \
    > /dev/null 2>&1
st=$?
[ $st -eq 2 ] || fail "a linear pair exited $st, want 2"
# a dataset of its own, sharing no origin with onto at all
zfs create "$POOL/other" || exit 2
zfs snapshot "$POOL/other@x" || exit 2
"$bin" -n --from "$POOL/other@x" --onto "$POOL/onto@work" \
    > /dev/null 2>&1
st=$?
[ $st -eq 2 ] || fail "an unrelated pair exited $st, want 2"
echo "ok   linear and unrelated pairs both refused (exit 2)"

say "1. dry run"
"$bin" -n $flag -o "$tmp/got-n" --from "$POOL/from@work" \
    --onto "$POOL/onto@work"
st=$?
sed -n '/^#mode/,$p' "$tmp/expect" > "$tmp/expect.body"
sed -n '/^#mode/,$p' "$tmp/got-n" > "$tmp/got-n.body"
cmp -s "$tmp/expect.body" "$tmp/got-n.body" || { diff "$tmp/expect.body" "$tmp/got-n.body" | head -20; fail "dry-run manifest differs"; }
grep -q "^#base $POOL/base@base\$" "$tmp/got-n" || { head -5 "$tmp/got-n"; fail "the dry run did not derive $POOL/base@base"; }
echo "ok   dry run (exit $st), base derived"

say "2. real run"
"$bin" $flag -v -o "$tmp/got" --off-of "$POOL/from@work" \
    --onto "$POOL/onto@work" --result "$POOL/result"
st=$?
sed -n '/^#mode/,$p' "$tmp/got" > "$tmp/got.body"
cmp -s "$tmp/expect.body" "$tmp/got.body" || { diff "$tmp/expect.body" "$tmp/got.body" | head -20; fail "real-run manifest differs"; }
grep -q "^#base $POOL/base@base\$" "$tmp/got" || { head -5 "$tmp/got"; fail "the real run did not derive $POOL/base@base"; }
if grep -q '^#conflicts 0$' "$tmp/expect"; then
	clean=1
	[ $st -eq 0 ] || fail "clean fixture exited $st"
	echo "ok   real run applied (exit 0)"
else
	clean=0
	[ $st -eq 1 ] || fail "conflicted fixture exited $st, want 1"
	echo "ok   real run stopped on conflicts (exit 1)"
fi

say "2a. the holds are gone"
for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
	held=$(zfs holds -H "$s") || fail "zfs holds $s"
	[ -z "$held" ] || fail "$s is still held: $held"
done
echo "ok   every hold released at exit"

say "3. the result"
[ "$(zfs list -H -o name "$POOL/result" 2>/dev/null)" = "$POOL/result" ] \
    || fail "no result dataset $POOL/result"
[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || fail "the result is not read-only"
cmnt=$(zfs get -H -o value mountpoint "$POOL/result")
[ "$cmnt" = "$RUNDIR/mnt" ] || fail "the result is at $cmnt, want $RUNDIR/mnt"
state=$(zfs get -H -o value zfs_rebase:state "$POOL/result")
if [ $clean -eq 1 ]; then
	[ "$state" = applied ] || fail "zfs_rebase:state is $state, want applied"
	[ -f "$tmp/got" ] || fail "no manifest at $tmp/got"
	"$bin" --posix $flag -o "$tmp/again" "$tmp/base" "$tmp/from" "$cmnt"
	st=$?
	grep -q '^#actions 0$' "$tmp/again" && grep -q '^#conflicts 0$' "$tmp/again" \
	    || { sed -n '1,30p' "$tmp/again"; fail "rebasing onto the result is not a no-op (exit $st)"; }
	echo "ok   result: applied, and rebasing from onto it again is a no-op"
else
	[ "$state" = conflicts ] || fail "zfs_rebase:state is $state, want conflicts"
	echo "ok   result: kept at state conflicts"
fi

say "4. abort"
"$bin" --abort --result "$POOL/result" || fail "abort exited $?"
if zfs list -H -o name "$POOL/result" > /dev/null 2>&1; then
	fail "$POOL/result survived the abort"
fi
if [ -e "$tmp/got" ]; then
	fail "the recorded manifest $tmp/got survived the abort"
fi
if [ -e "/var/run/zfs_rebase/$POOL" ]; then
	fail "/var/run/zfs_rebase/$POOL survived the abort"
fi
"$bin" --abort --result "$POOL/result" 2>/dev/null
st=$?
[ $st -eq 2 ] || fail "a second abort exited $st, want 2"
echo "ok   abort: the result, its manifest and its directory are gone"
rc=0
exit 0
