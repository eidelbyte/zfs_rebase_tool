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
# A bogus zfs_rebase:tag and zfs_rebase:manifest are set on the pool
# root before the run, because user properties inherit down the naming
# tree: every property the tool reads back must be the result's own
# local value, and a dataset that only inherits them is not a result.
#
# What is checked, in order:
#   0. the derivation refuses what it should: a linear pair, where
#      one side is an ancestor of the other, and a pair that shares
#      no origin at all -- both exit 2;
#   1. -n: the manifest equals the fixture's expect block from the
#      #mode line on (the header names datasets here), and its #base
#      line is the snapshot the two sides were cloned from; and for
#      probe.zrt, that -n --verify still creates nothing, holds
#      nothing and leaves no run directory;
#   2. the real run, with --off-of for --from: exit 0 for a clean
#      fixture, 1 for a conflicted one, the manifest again equal and
#      the same #base derived; then the holds -- none at all for a
#      clean fixture, which released them when it reached done, and
#      exactly one per input snapshot for a conflicted one, under
#      the tag the record carries, since a stopped rebase holds on
#      purpose;
#   3. the result and its record: exactly $POOL/result, read-only,
#      mounted at /var/run/zfs_rebase/$POOL/result/mnt; the three
#      snapshot names, the three guids as zfs prints them, form
#      clone, the mode the flag asked for, verify no, the tag, and
#      every one of them a local value that beats the bogus one on
#      the parent; zfs_rebase:state "done" for a clean fixture and
#      "conflicts" for a conflicted one; --abort on a plain dataset
#      under the same parent refused with exit 2, because its
#      properties are inherited and not its own; and for a clean
#      fixture, that the manifest file is where -o put it and that
#      rebasing from onto the result again in --posix mode yields a
#      manifest with zero actions and zero conflicts (idempotence:
#      the result already holds from's changes);
#   4. --abort: exit 0, every hold released, the dataset gone, the
#      recorded manifest unlinked, the run directory gone down to
#      /var/run/zfs_rebase, and a second --abort exit 2 because there
#      is no such run;
#   5. for probe.zrt, a real run given --verify: zfs_rebase:verify is
#      "yes" in its record and its tag is a new one, and --abort
#      takes it away again.
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
	# A failed step can leave the result clone, its persistent
	# holds and its directory under /var/run; --abort is what
	# gives back all three, and zpool destroy -f would not touch
	# the directory.
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
# one record property of a result, by value and by source
recval() { zfs get -H -o value "$1" "$2"; }
recsrc() { zfs get -H -o source "$1" "$2"; }
# every hold on a snapshot, as "tag" lines
holdtags() { zfs holds -H "$1" | cut -f2; }

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

# The inheritance trap. A user property set here shows up on every
# dataset under it, $POOL/result included, so the tool must set its
# record locally and read it locally: a run that read these would
# report the wrong snapshots, and an --abort that believed them would
# destroy a dataset no run ever made.
zfs set zfs_rebase:tag=bogus "$POOL" || exit 2
zfs set zfs_rebase:manifest=/nonexistent/manifest "$POOL" || exit 2
zfs create "$POOL/plain" || exit 2

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
case "$fixture" in
*/probe.zrt|probe.zrt)
	# -n ignores --result and --verify has nothing to record yet:
	# together they must still create nothing and hold nothing.
	"$bin" -n --verify $flag --from "$POOL/from@work" \
	    --onto "$POOL/onto@work" --result "$POOL/vresult" >/dev/null 2>&1
	if zfs list -H -o name "$POOL/vresult" > /dev/null 2>&1; then
		fail "-n --verify created $POOL/vresult"
	fi
	[ -e "/var/run/zfs_rebase/$POOL/vresult" ] && \
	    fail "-n --verify left a run directory"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "-n --verify held $s: $held"
	done
	echo "ok   -n --verify creates nothing and holds nothing"
	;;
esac

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

say "2a. the holds"
tag=$(recval zfs_rebase:tag "$POOL/result")
if [ $clean -eq 1 ]; then
	# done releases them, and it writes the state before it does.
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is still held after done: $held"
	done
	echo "ok   every hold released at done"
else
	case "$tag" in
	zr-*) ;;
	*) fail "the record's tag is '$tag', want zr-<12 hex>" ;;
	esac
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(holdtags "$s") || fail "zfs holds $s"
		n=$(printf '%s\n' "$held" | grep -c . )
		[ "$n" -eq 1 ] || fail "$s has $n holds, want 1: $held"
		[ "$held" = "$tag" ] || \
		    fail "$s is held under '$held', want '$tag'"
	done
	echo "ok   each input held once, under the record's tag $tag"
fi

say "3. the result and its record"
[ "$(zfs list -H -o name "$POOL/result" 2>/dev/null)" = "$POOL/result" ] \
    || fail "no result dataset $POOL/result"
[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || fail "the result is not read-only"
cmnt=$(zfs get -H -o value mountpoint "$POOL/result")
[ "$cmnt" = "$RUNDIR/mnt" ] || fail "the result is at $cmnt, want $RUNDIR/mnt"

# Every property of the record is the result's own, not the pool's.
for prop in base base_guid from from_guid onto onto_guid made mode \
    form tag verify manifest; do
	src=$(recsrc "zfs_rebase:$prop" "$POOL/result")
	[ "$src" = local ] || \
	    fail "zfs_rebase:$prop has source $src, want local"
done
[ "$(recval zfs_rebase:tag "$POOL/result")" != bogus ] || \
    fail "the result inherited the parent's zfs_rebase:tag"
[ "$(recval zfs_rebase:form "$POOL/result")" = clone ] || \
    fail "zfs_rebase:form is not clone"
[ "$(recval zfs_rebase:made "$POOL/result")" = "" ] || \
    fail "zfs_rebase:made is not empty; the tool took no snapshots"
[ "$(recval zfs_rebase:verify "$POOL/result")" = no ] || \
    fail "zfs_rebase:verify is not no"
want=strict
[ -n "$flag" ] && want=permissive
[ "$(recval zfs_rebase:mode "$POOL/result")" = "$want" ] || \
    fail "zfs_rebase:mode is not $want"
[ "$(recval zfs_rebase:manifest "$POOL/result")" = "$tmp/got" ] || \
    fail "zfs_rebase:manifest is not $tmp/got"
for side in base:$POOL/base@base from:$POOL/from@work onto:$POOL/onto@work; do
	which=${side%%:*}
	snap=${side#*:}
	[ "$(recval "zfs_rebase:$which" "$POOL/result")" = "$snap" ] || \
	    fail "zfs_rebase:$which is not $snap"
	guid=$(zfs get -H -o value guid "$snap")
	got=$(recval "zfs_rebase:${which}_guid" "$POOL/result")
	[ "$got" = "$guid" ] || \
	    fail "zfs_rebase:${which}_guid is $got, want $guid"
done
echo "ok   record: the three snapshots and guids, form, mode, verify,"
echo "     tag and manifest, every one of them local"

# A dataset that only inherits the properties is not a result.
"$bin" --abort --result "$POOL/plain" > /dev/null 2>&1
st=$?
[ $st -eq 2 ] || fail "--abort on an inheriting dataset exited $st, want 2"
[ "$(zfs list -H -o name "$POOL/plain" 2>/dev/null)" = "$POOL/plain" ] \
    || fail "--abort destroyed $POOL/plain, which is no result of ours"
echo "ok   an inherited record is no record: abort refused (exit 2)"

state=$(recval zfs_rebase:state "$POOL/result")
if [ $clean -eq 1 ]; then
	[ "$state" = done ] || fail "zfs_rebase:state is $state, want done"
	[ -f "$tmp/got" ] || fail "no manifest at $tmp/got"
	"$bin" --posix $flag -o "$tmp/again" "$tmp/base" "$tmp/from" "$cmnt"
	st=$?
	grep -q '^#actions 0$' "$tmp/again" && grep -q '^#conflicts 0$' "$tmp/again" \
	    || { sed -n '1,30p' "$tmp/again"; fail "rebasing onto the result is not a no-op (exit $st)"; }
	echo "ok   result: done, and rebasing from onto it again is a no-op"
else
	[ "$state" = conflicts ] || fail "zfs_rebase:state is $state, want conflicts"
	echo "ok   result: kept at state conflicts"
fi

say "4. abort"
"$bin" --abort --result "$POOL/result" || fail "abort exited $?"
for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
	held=$(zfs holds -H "$s") || fail "zfs holds $s"
	[ -z "$held" ] || fail "$s is still held after the abort: $held"
done
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
echo "ok   abort: the holds, the result, its manifest and its"
echo "     directory are all gone"

case "$fixture" in
*/probe.zrt|probe.zrt)
	say "5. --verify is recorded"
	"$bin" --verify $flag -o "$tmp/got-v" --from "$POOL/from@work" \
	    --onto "$POOL/onto@work" --result "$POOL/result"
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || fail "the --verify run exited $st"
	[ "$(recval zfs_rebase:verify "$POOL/result")" = yes ] || \
	    fail "zfs_rebase:verify is not yes"
	vtag=$(recval zfs_rebase:tag "$POOL/result")
	case "$vtag" in
	zr-*) ;;
	*) fail "the second run's tag is '$vtag', want zr-<12 hex>" ;;
	esac
	[ "$vtag" != "$tag" ] || fail "the second run reused the tag $tag"
	"$bin" --abort --result "$POOL/result" || fail "abort exited $?"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is still held: $held"
	done
	echo "ok   --verify recorded as yes, under its own tag $vtag"
	;;
esac
rc=0
exit 0
