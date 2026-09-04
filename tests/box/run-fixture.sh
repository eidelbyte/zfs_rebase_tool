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
#      mounted at /var/db/zfs_rebase/$POOL/result/mnt; the three
#      snapshot names, the three guids as zfs prints them, form
#      clone, the mode the flag asked for, verify no, the tag, and
#      every one of them a local value that beats the bogus one on
#      the parent; zfs_rebase:state "done" for a clean fixture and
#      "conflicts" for a conflicted one; --abort on a plain dataset
#      under the same parent refused with exit 2, because its
#      properties are inherited and not its own; and, for either
#      fixture, that the manifest file is where -o put it and that
#      rebasing from onto the result again in --posix mode declares
#      zero actions -- stage 1 is idempotent, and a conflicted run
#      applies its clean actions too, so the result already holds
#      from's clean changes either way -- and the conflicts the
#      fixture expects, zero for a clean one and the expect block's
#      own count for a conflicted one, since a conflict is answered
#      by the conflict manager and not by a second rebase;
#  3a. the verbs on that result: --verify exits 0 and prints the
#      counts, having written nothing and moved no state, on either
#      branch -- a conflicted run applied its clean actions too, so
#      every action is done and only the conflicts are outstanding;
#      --continue exits 0 on a done result and 1 on one at
#      conflicts, naming the resolution it waits for, and leaves the
#      state and the tree exactly as they were; every verb refuses
#      the plain dataset that only inherits the record properties,
#      with exit 2 and no harm to it; and --result spelled as a
#      snapshot of the result, one that does not even exist, finds
#      the same rebase, since the name is taken as its dataset;
#  3b. for probe.zrt, drift and its repair: /n, which the manifest
#      copied, is edited behind the tool's back with readonly off
#      and on again, --verify then exits 3 naming "drifted 1, first
#      /n" and fixes nothing, --continue --verify puts it back and
#      exits per the branch, and --verify is clean again with the
#      result read-only;
#  3c. for probe.zrt, --restart: the clone is destroyed and made
#      again from the recorded onto snapshot with the same record,
#      the manifest is applied from the first gate, and the run
#      lands at the same state under the same tag with the same
#      three holds and the same tree;
#   4. --abort: exit 0, every hold released, the dataset gone, the
#      recorded manifest unlinked, the run directory gone down to
#      /var/db/zfs_rebase, and a second --abort exit 2 because there
#      is no such run;
#   5. a second real run given --verify -- every clean fixture, and
#      probe.zrt as the conflicted one: zfs_rebase:verify is "yes"
#      in its record and its tag is a new one, and on a clean
#      fixture the final check runs at the done gate (its report is
#      printed, the state is done and the holds are released before
#      the abort), which is where --verify is due;
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
RUNDIR=/var/db/zfs_rebase/$POOL/result
MD=
tmp=$(mktemp -d /tmp/zr-box.XXXXXX) || exit 2
rc=1

cleanup() {
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	# A failed step can leave the result clone, its persistent
	# holds and its directory under /var/db; --abort is what
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
# Rebase from onto the result again, over three plain directories:
# the fixture's own base and from, and the clone at its mountpoint.
# Stage 1 is idempotent, so this must have nothing left to do.
again() {
	"$bin" --posix $flag -o "$1" "$tmp/base" "$tmp/from" "$cmnt"
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || fail "the --posix re-run exited $st"
}
# That manifest declares no actions and the conflicts named in $2.
idempotent() {
	grep -q '^#actions 0$' "$1" || \
	    { sed -n '1,30p' "$1"; fail "rebasing onto the result declares actions"; }
	grep -q "^#conflicts $2\$" "$1" || \
	    { grep '^#conflicts' "$1"; fail "rebasing onto the result wants $2 conflicts"; }
}

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
	[ -e "/var/db/zfs_rebase/$POOL/vresult" ] && \
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
[ -f "$tmp/got" ] || fail "no manifest at $tmp/got"
want_conf=$(sed -n 's/^#conflicts //p' "$tmp/expect")
if [ $clean -eq 1 ]; then
	[ "$state" = done ] || fail "zfs_rebase:state is $state, want done"
	again "$tmp/again"
	idempotent "$tmp/again" 0
	echo "ok   result: done, and rebasing from onto it again is a no-op"
else
	[ "$state" = conflicts ] || fail "zfs_rebase:state is $state, want conflicts"
	# The clean actions are applied under applying1 before the run
	# stops here, so a second rebase has no action left to name --
	# and the conflicts are still the conflicts, because answering
	# one is the conflict manager's work and not a rebase's.
	again "$tmp/again"
	idempotent "$tmp/again" "$want_conf"
	echo "ok   result: at conflicts, clean actions applied; a second"
	echo "     rebase declares 0 actions and $want_conf conflicts again"
fi

say "3a. the verbs on the result"
# --verify reports and writes nothing. Every action of the manifest
# must be done by now on either branch, because a conflicted run
# applies its clean actions too and the conflicts themselves are not
# actions; blocked is possible and is not a failure.
"$bin" --verify --result "$POOL/result" > "$tmp/verify1" 2>&1
st=$?
[ $st -eq 0 ] || { cat "$tmp/verify1"; fail "--verify exited $st, want 0"; }
grep -q 'done [0-9]' "$tmp/verify1" || \
    { cat "$tmp/verify1"; fail "--verify printed no counts"; }
grep -q 'drifted 0' "$tmp/verify1" || \
    { cat "$tmp/verify1"; fail "--verify found drift"; }
grep -q 'pending 0' "$tmp/verify1" || \
    { cat "$tmp/verify1"; fail "--verify found pending actions"; }
[ "$(recval zfs_rebase:state "$POOL/result")" = "$state" ] || \
    fail "--verify moved the state"
echo "ok   --verify: exit 0, counts printed, still at $state"

# --continue on a rebase that is where it should be changes nothing:
# a done one is done, and a conflicted one waits for a resolution
# that nothing in this sprint writes.
"$bin" --continue --result "$POOL/result" > "$tmp/cont1" 2>&1
st=$?
if [ $clean -eq 1 ]; then
	[ $st -eq 0 ] || \
	    { cat "$tmp/cont1"; fail "--continue on done exited $st, want 0"; }
else
	[ $st -eq 1 ] || \
	    { cat "$tmp/cont1"; fail "--continue at conflicts exited $st, want 1"; }
	grep -q "$RUNDIR/resolution" "$tmp/cont1" || \
	    { cat "$tmp/cont1"; fail "--continue did not name the resolution"; }
fi
[ "$(recval zfs_rebase:state "$POOL/result")" = "$state" ] || \
    fail "--continue moved the state"
# and the tree it leaves is still the tree stage 1 made
again "$tmp/again2"
idempotent "$tmp/again2" "$want_conf"
echo "ok   --continue: exit $st, the state and the tree unchanged"

# A dataset that only inherits the record's properties is no result
# of ours, whatever the verb is, and none of them may touch it.
for verb in --verify --continue --restart; do
	"$bin" $verb --result "$POOL/plain" > /dev/null 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    fail "$verb on an inheriting dataset exited $st, want 2"
done
[ "$(zfs list -H -o name "$POOL/plain" 2>/dev/null)" = "$POOL/plain" ] || \
    fail "a verb destroyed $POOL/plain, which is no result of ours"
echo "ok   every verb refuses a dataset with no record (exit 2)"

# --result names the dataset carrying the record, and a snapshot
# name is taken as its dataset: this one does not even exist, and
# the verb still finds the rebase.
"$bin" --verify --result "$POOL/result@nosuch" > /dev/null 2>&1
st=$?
[ $st -eq 0 ] || fail "--verify on a snapshot spelling exited $st, want 0"
echo "ok   --result $POOL/result@nosuch is the same rebase"

case "$fixture" in
*/probe.zrt|probe.zrt)
	say "3b. a stray edit, reported and repaired (probe.zrt)"
	# /n is a cp of the manifest, so an edit to it matches neither
	# what the rebase made nor what onto had: that is drift, and
	# --verify says so and fixes nothing.
	# It is there at all only because applying1 ran before the run
	# stopped at conflicts, which is the whole point of the stage.
	[ -f "$cmnt/n" ] || fail "the clean action n cp /n was not applied"
	zfs set readonly=off "$POOL/result" || fail "readonly=off"
	printf 'stray\n' >> "$cmnt/n" || fail "cannot edit $cmnt/n"
	zfs set readonly=on "$POOL/result" || fail "readonly=on"
	"$bin" --verify --result "$POOL/result" > "$tmp/verify2" 2>&1
	st=$?
	[ $st -eq 3 ] || \
	    { cat "$tmp/verify2"; fail "--verify over drift exited $st, want 3"; }
	grep -q 'drifted 1, first /n' "$tmp/verify2" || \
	    { cat "$tmp/verify2"; fail "--verify did not name the drifted /n"; }
	echo "ok   --verify: exit 3, drifted 1 first /n, nothing written"
	# --continue --verify is the repair: the clean action is made
	# true again, up to the gate the rebase is at.
	"$bin" --continue --verify --result "$POOL/result" > "$tmp/cont2" 2>&1
	st=$?
	want=1
	[ $clean -eq 1 ] && want=0
	[ $st -eq $want ] || \
	    { cat "$tmp/cont2"; fail "--continue --verify exited $st, want $want"; }
	"$bin" --verify --result "$POOL/result" > "$tmp/verify3" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/verify3"; fail "--verify after the repair exited $st"; }
	grep -q 'drifted 0' "$tmp/verify3" || \
	    { cat "$tmp/verify3"; fail "the repair left drift behind"; }
	[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || \
	    fail "the repair left the result writable"
	echo "ok   --continue --verify repaired it; --verify is clean again"

	say "3c. restart (probe.zrt)"
	# The clone goes and is made again from the recorded onto
	# snapshot with the same record; the holds are on the
	# snapshots and are not touched by any of it.
	"$bin" --restart --result "$POOL/result" > "$tmp/rest" 2>&1
	st=$?
	want=1
	[ $clean -eq 1 ] && want=0
	[ $st -eq $want ] || \
	    { cat "$tmp/rest"; fail "--restart exited $st, want $want"; }
	[ "$(recval zfs_rebase:state "$POOL/result")" = "$state" ] || \
	    fail "--restart did not land at $state"
	[ "$(recval zfs_rebase:tag "$POOL/result")" = "$tag" ] || \
	    fail "--restart changed the tag"
	[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || \
	    fail "--restart left the result writable"
	if [ $clean -eq 0 ]; then
		for s in "$POOL/base@base" "$POOL/from@work" \
		    "$POOL/onto@work"; do
			held=$(holdtags "$s") || fail "zfs holds $s"
			[ "$held" = "$tag" ] || \
			    fail "$s is held under '$held' after the restart"
		done
	fi
	again "$tmp/again3"
	idempotent "$tmp/again3" "$want_conf"
	echo "ok   --restart: rebuilt, at $state, held under $tag, tree equal"
	;;
esac

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
if [ -e "/var/db/zfs_rebase/$POOL" ]; then
	fail "/var/db/zfs_rebase/$POOL survived the abort"
fi
"$bin" --abort --result "$POOL/result" 2>/dev/null
st=$?
[ $st -eq 2 ] || fail "a second abort exited $st, want 2"
echo "ok   abort: the holds, the result, its manifest and its"
echo "     directory are all gone"

# A second run, given --verify: recorded in the record, and on a
# clean fixture carried out at the done gate, which is where the
# final check belongs -- after the last apply verified and before
# anything is released. Every clean fixture takes this, and probe.zrt
# takes it as the conflicted one, where the run stops at conflicts
# before the check is due.
do5=$clean
case "$fixture" in */probe.zrt|probe.zrt) do5=1 ;; esac
if [ $do5 -eq 1 ]; then
	say "5. a run given --verify"
	"$bin" --verify $flag -o "$tmp/got-v" --from "$POOL/from@work" \
	    --onto "$POOL/onto@work" --result "$POOL/result" \
	    2> "$tmp/verify5"
	st=$?
	cat "$tmp/verify5"
	[ $st -eq 0 ] || [ $st -eq 1 ] || fail "the --verify run exited $st"
	[ "$(recval zfs_rebase:verify "$POOL/result")" = yes ] || \
	    fail "zfs_rebase:verify is not yes"
	vtag=$(recval zfs_rebase:tag "$POOL/result")
	case "$vtag" in
	zr-*) ;;
	*) fail "the second run's tag is '$vtag', want zr-<12 hex>" ;;
	esac
	[ "$vtag" != "$tag" ] || fail "the second run reused the tag $tag"
	if [ $clean -eq 1 ]; then
		[ $st -eq 0 ] || fail "the clean --verify run exited $st"
		[ "$(recval zfs_rebase:state "$POOL/result")" = done ] || \
		    fail "the --verify run did not reach done"
		grep -q 'drifted 0' "$tmp/verify5" || \
		    fail "the --verify run printed no final check"
		for s in "$POOL/base@base" "$POOL/from@work" \
		    "$POOL/onto@work"; do
			held=$(zfs holds -H "$s") || fail "zfs holds $s"
			[ -z "$held" ] || \
			    fail "$s is held after a --verify run reached done"
		done
	fi
	"$bin" --abort --result "$POOL/result" || fail "abort exited $?"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is still held: $held"
	done
	echo "ok   --verify recorded as yes, under its own tag $vtag"
fi
rc=0
exit 0
