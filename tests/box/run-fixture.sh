#!/bin/sh
# Box harness: build one fixture as real datasets on a throwaway pool,
# run zfs_rebase for real, and check it. FreeBSD, root, after
# make freebsd. Usage: run-fixture.sh FIXTURE.zrt   (KEEP=1 to leave
# the pool behind for inspection).
#
# Every fixture is run twice over, once in each form of the tool.
#
# The clone form first (steps 0 to 5): the harness takes the
# snapshots -- base@base when base is populated, from@work and
# onto@work when the two sides are -- and gives the run the last two
# and the name of the clone, $POOL/result. The run derives base@base
# for itself as the point the two sides branched from.
#
# Then the dataset form (steps D0 to D2), after the clone form has
# been aborted and the pool is back to base, from and onto with their
# snapshots: from is given as the DATASET, so the tool takes its own
# snapshot of it and destroys it again when the rebase ends, and onto
# is given as the dataset too, so the rebase is made in it and
# --result names the pre-apply snapshot. That pass is run twice, once
# with --result spelled short and once in full, since both must name
# one snapshot of one dataset.
#
# A bogus zfs_rebase:tag and zfs_rebase:manifest are set on the pool
# root before the run, because user properties inherit down the naming
# tree: every property the tool reads back must be the result's own
# local value, and a dataset that only inherits them is not a result.
#
# What is checked, in order:
#   0. the derivation refuses what it should: a linear pair, where
#      one side is an ancestor of the other, and a pair that shares
#      no origin at all -- both exit 2 -- and --base without
#      --allow-unrelated is a usage error, since a derived base is
#      not open to a second opinion;
#  0a. for probe.zrt, --allow-unrelated: that same unrelated pair
#      goes through with no base at all, its manifest saying
#      "#base -"; it goes through with --base given, whose snapshot
#      is walked like the other two; and a --base newer than a side
#      is refused with exit 2;
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
# and then, in the dataset form:
#  D0. for probe.zrt, exclusivity: a file held open under onto makes
#      the unmount fail, the run exits 2 saying onto is in use, and
#      it takes back everything it had made -- its record, the
#      pre-apply snapshot, the snapshot it took of from, and its run
#      directory -- leaving onto mounted where it was;
#  D1. the run itself: the manifest is the clone form's manifest
#      exactly and derives the same base; the record is on
#      $POOL/onto with form dataset, made from, readonly recording
#      what it was, the pre-apply snapshot as :onto and the tool's
#      own snapshot of from as :from, every property local against
#      the bogus ones on the pool root; the dataset is mounted at its
#      own mountpoint again with readonly as it was and the
#      mountpoint property untouched; the pre-apply snapshot is
#      there; and then per branch -- a clean fixture at done with
#      every hold released and the tool's own snapshot destroyed and
#      the live tree the rebased tree, a conflicted one at conflicts
#      with the clean actions applied, the three holds under the
#      record's tag and the same conflicts declared by a second
#      rebase. --verify and --continue then behave as they do in the
#      clone form and hand the dataset back each time, an open
#      rebase is refused with and without --overwrite, and --abort
#      rolls onto back to what it was, destroys both snapshots, takes
#      every zfs_rebase: property off it and leaves it mounted at
#      home holding the tree the fixture built;
#  D2. for a clean fixture, --overwrite: a record that reached done
#      is refused without the flag and replaced with it, and the
#      before-image of the done rebase is still there afterwards,
#      because a rebase that finished keeps it;
#  D3. the whole pass again with --result spelled as
#      $POOL/onto@pre, which must be the same rebase of the same
#      snapshot;
#
# The from and onto datasets are made by clearing a clone of base and
# extracting the fixture's tree with tar, so every object is new and
# none of them prunes; the unchanged-pool pruning is exercised only
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
	# A failed step can leave the result clone or the onto dataset
	# taken over, either one's persistent holds, and a directory
	# under /var/db; --abort is what gives all of that back, and
	# zpool destroy -f would not touch the directory. The dataset
	# form's --result is the dataset itself.
	"$bin" --abort --result "$POOL/result" >/dev/null 2>&1
	"$bin" --abort --result "$POOL/onto" >/dev/null 2>&1
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	rm -f "$IMG"
	# a fixture can leave uchg or schg behind (flags-conflict.zrt),
	# and rm would refuse it; tests/run-fixtures.sh does the same.
	chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null
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
zfs set zfs_rebase:resolution=/nonexistent/resolution "$POOL" || exit 2
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
# --base is a base given by hand, and there is no place for one where
# the branch point is derived.
"$bin" -n --base "$POOL/base@base" --from "$POOL/from@work" \
    --onto "$POOL/onto@work" > /dev/null 2>&1
st=$?
[ $st -eq 2 ] || fail "--base without --allow-unrelated exited $st, want 2"
echo "ok   --base without --allow-unrelated refused (exit 2)"

case "$fixture" in
*/probe.zrt|probe.zrt)
	say "0a. --allow-unrelated"
	# The same unrelated pair the derivation just refused, taken
	# with the flag: no derivation, no pruning, and no base at
	# all, so the manifest's header says so with a "-" -- every
	# name of either side is an add and the decision is their
	# union. $POOL/other is an empty dataset, so onto's whole
	# tree is added on onto's side alone: 0 or 1, never 2.
	"$bin" --allow-unrelated -n -o "$tmp/got-u" \
	    --from "$POOL/other@x" --onto "$POOL/onto@work"
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || \
	    fail "--allow-unrelated with no base exited $st, want 0 or 1"
	grep -q '^#base -$' "$tmp/got-u" || \
	    { head -5 "$tmp/got-u"; fail "the empty base is not '#base -'"; }
	echo "ok   --allow-unrelated with no base (exit $st), #base -"
	# A base given by hand: older than both sides, in one pool
	# with them, and its dataset mounted, so its tree is walked
	# like the other two.
	"$bin" --allow-unrelated --base "$POOL/base@base" -n \
	    -o "$tmp/got-ub" --from "$POOL/other@x" --onto "$POOL/onto@work"
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || \
	    fail "--allow-unrelated --base exited $st, want 0 or 1"
	grep -q "^#base $POOL/base@base\$" "$tmp/got-ub" || \
	    { head -5 "$tmp/got-ub"; fail "--base is not in the header"; }
	echo "ok   --allow-unrelated --base $POOL/base@base (exit $st)"
	# A base newer than a side. $POOL/other@x was taken above,
	# after from@work and onto@work, so its createtxg is higher
	# than from@work's for certain: as a base it is newer than
	# from and the run is refused before anything is read.
	"$bin" --allow-unrelated --base "$POOL/other@x" -n \
	    --from "$POOL/from@work" --onto "$POOL/onto@work" \
	    > /dev/null 2>&1
	st=$?
	[ $st -eq 2 ] || fail "a base newer than from exited $st, want 2"
	echo "ok   a base newer than from refused (exit 2)"
	;;
esac

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
    form tag verify manifest resolution; do
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
# -o names the manifest and the resolution goes beside it, which is
# that name and .resolution: the record says so and every verb reads
# the record rather than guessing a path.
RES=$tmp/got.resolution
[ "$(recval zfs_rebase:resolution "$POOL/result")" = "$RES" ] || \
    fail "zfs_rebase:resolution is not $RES"
[ -f "$RES" ] || fail "the run wrote no resolution at $RES"
grep -q '^#rebase-resolution 4$' "$RES" || \
    { head -3 "$RES"; fail "$RES is no resolution"; }
grep -q "^#onto $POOL/onto@work\$" "$RES" || \
    { head -8 "$RES"; fail "the resolution names other snapshots"; }
# One line per conflicted name of the manifest, and every one of
# them unanswered: that is what a skeleton is.
want_names=$(grep -c ' conflict [0-9][0-9]*$' "$tmp/expect" || true)
[ "$(sed -n 's/^#names //p' "$RES")" = "$want_names" ] || \
    { head -8 "$RES"; fail "the resolution has not $want_names names"; }
[ "$(sed -n 's/^#unanswered //p' "$RES")" = "$want_names" ] || \
    { head -8 "$RES"; fail "the skeleton is not wholly unanswered"; }
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
# a done one is done, and a conflicted one waits at its skeleton,
# which nobody has answered, and says how much of it is unanswered.
"$bin" --continue --result "$POOL/result" > "$tmp/cont1" 2>&1
st=$?
if [ $clean -eq 1 ]; then
	[ $st -eq 0 ] || \
	    { cat "$tmp/cont1"; fail "--continue on done exited $st, want 0"; }
else
	[ $st -eq 1 ] || \
	    { cat "$tmp/cont1"; fail "--continue at conflicts exited $st, want 1"; }
	grep -q "$RES" "$tmp/cont1" || \
	    { cat "$tmp/cont1"; fail "--continue did not name the resolution"; }
	grep -q "$want_names of $want_names name"  "$tmp/cont1" || \
	    { cat "$tmp/cont1"; fail "--continue did not count the unanswered"; }
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
	say "3b. a stray edit, reported and never repaired (probe.zrt)"
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
	# And --continue --verify does not mend it either: the rebase
	# is at the conflicts gate, where the tree is being edited by
	# hand and an edit cannot be told from a stray. It reports and
	# passes; --restart below is what puts the result back.
	"$bin" --continue --verify --result "$POOL/result" > "$tmp/cont2" 2>&1
	st=$?
	want=1
	[ $clean -eq 1 ] && want=0
	[ $st -eq $want ] || \
	    { cat "$tmp/cont2"; fail "--continue --verify exited $st, want $want"; }
	grep -q 'drifted 1, first /n' "$tmp/cont2" || \
	    { cat "$tmp/cont2"; fail "--continue --verify did not report the drift"; }
	"$bin" --verify --result "$POOL/result" > "$tmp/verify3" 2>&1
	st=$?
	[ $st -eq 3 ] || \
	    { cat "$tmp/verify3"; fail "--verify after it exited $st, want 3"; }
	grep -q 'drifted 1, first /n' "$tmp/verify3" || \
	    { cat "$tmp/verify3"; fail "the drift is not reported any more"; }
	[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || \
	    fail "a verb left the result writable"
	echo "ok   --continue --verify reported it and wrote nothing"

	say "3c. restart (probe.zrt)"
	# The clone goes and is made again from the recorded onto
	# snapshot with the same record; the holds are on the
	# snapshots and are not touched by any of it. The stray edit
	# of 3b goes with the clone, which is what --restart is for.
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

	say "3d. the resolution answered (probe.zrt)"
	# The restart above put the skeleton back: the tool wrote it
	# again from the recorded manifest, every line unanswered,
	# whatever had been written into it before.
	[ "$(sed -n 's/^#unanswered //p' "$RES")" = "$want_names" ] || \
	    { head -8 "$RES"; fail "--restart did not put the skeleton back"; }
	# Answering is one field per line -- "-" becomes keep -- and the
	# header's count goes with them. Lines are never added, never
	# removed: the file is the record of what was chosen.
	sed -e 's/ -$/ keep/' -e 's/^#unanswered .*$/#unanswered 0/' \
	    "$RES" > "$RES.answered" || fail "cannot answer $RES"
	mv "$RES.answered" "$RES" || fail "cannot answer $RES"
	[ "$(grep -c ' keep$' "$RES")" = "$want_names" ] || \
	    { cat "$RES"; fail "the answers did not take"; }
	[ "$(sed -n 's/^#names //p' "$RES")" = "$want_names" ] || \
	    { cat "$RES"; fail "answering changed the count of names"; }
	"$bin" --continue --result "$POOL/result" > "$tmp/cont3" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/cont3"; fail "--continue over an answered resolution exited $st, want 0"; }
	nowstate=$(recval zfs_rebase:state "$POOL/result")
	[ "$nowstate" = done ] || \
	    fail "the answered rebase is at $nowstate, want done"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is still held after done: $held"
	done
	[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || \
	    fail "done left the result writable"
	# Every choice is keep for now, so applying2 wrote nothing and
	# the tree is still the one stage 1 made: a second rebase has no
	# action and the same conflicts. apply-choices changes this.
	again "$tmp/again4"
	idempotent "$tmp/again4" "$want_conf"
	echo "ok   answered: --continue -> done, holds released, tree equal"
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
if [ -e "$RES" ]; then
	fail "the recorded resolution $RES survived the abort"
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

# ---------------------------------------------------------------
# The dataset form. Everything above ran onto as a snapshot and put
# the rebase in a clone; the same fixture is now rebased in place,
# with from given as the dataset too so that the tool takes its own
# snapshot of it. The pool is back to base, from and onto with their
# snapshots, which is what this form starts from.
# ---------------------------------------------------------------
DRUN=/var/db/zfs_rebase/$POOL/onto
dsay() { printf '\n== the dataset form (--result %s): %s\n' "$dspec" "$*"; }
dfail() { fail "the dataset form (--result $dspec): $*"; }
# Every zfs_rebase: property that is the dataset's own. The pool root
# carries a bogus tag and manifest, so an inherited one must never be
# counted: after --abort this list has to be empty.
localprops() {
	zfs get -H -o property,source all "$1" 2>/dev/null | \
	    awk '$1 ~ /^zfs_rebase:/ && $2 == "local" { print $1 }'
}
# Is that snapshot there at all?
hassnap() { zfs list -H -o name -t snapshot "$1" > /dev/null 2>&1; }
# The tree onto holds now, held against the fixture: a --posix rebase
# over the fixture's own base and from and the live onto has to
# declare exactly what the expect block declares, which it can only
# do if onto is back to the tree the fixture built.
onto_is_the_fixture() {
	"$bin" --posix $flag -o "$1" "$tmp/base" "$tmp/from" "$MNT/onto"
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || dfail "the --posix re-run exited $st"
	sed -n '/^#mode/,$p' "$1" > "$1.body"
	cmp -s "$tmp/expect.body" "$1.body" || \
	    { diff "$tmp/expect.body" "$1.body" | head -20; \
	      dfail "onto is not the tree the fixture built"; }
}

# One whole pass in the dataset form. $1 is how --result is spelled
# and $2 is the short name it must come to, so that both spellings
# are shown to name one snapshot. The pass ends with --abort, which
# puts the dataset back as it was and leaves the pool ready for the
# next one.
dataset_pass() {
	dspec=$1
	dname=$2
	cmnt=$MNT/onto

	dsay "the run"
	"$bin" $flag -v -o "$tmp/got-d" --from "$POOL/from" \
	    --onto "$POOL/onto" --result "$dspec" > "$tmp/d.log" 2>&1
	dst=$?
	cat "$tmp/d.log"
	if [ $clean -eq 1 ]; then
		[ $dst -eq 0 ] || dfail "exited $dst, want 0"
	else
		[ $dst -eq 1 ] || dfail "exited $dst, want 1"
	fi
	sed -n '/^#mode/,$p' "$tmp/got-d" > "$tmp/got-d.body"
	cmp -s "$tmp/expect.body" "$tmp/got-d.body" || \
	    { diff "$tmp/expect.body" "$tmp/got-d.body" | head -20; \
	      dfail "the manifest differs from the clone form's"; }
	grep -q "^#base $POOL/base@base\$" "$tmp/got-d" || \
	    { head -5 "$tmp/got-d"; dfail "did not derive $POOL/base@base"; }

	dsay "the record on $POOL/onto"
	dtag=$(recval zfs_rebase:tag "$POOL/onto")
	dfrom=$(recval zfs_rebase:from "$POOL/onto")
	for prop in base base_guid from from_guid onto onto_guid made mode \
	    form tag verify manifest resolution readonly; do
		src=$(recsrc "zfs_rebase:$prop" "$POOL/onto")
		[ "$src" = local ] || \
		    dfail "zfs_rebase:$prop has source $src, want local"
	done
	[ "$(recval zfs_rebase:form "$POOL/onto")" = dataset ] || \
	    dfail "zfs_rebase:form is not dataset"
	[ "$(recval zfs_rebase:made "$POOL/onto")" = from ] || \
	    dfail "zfs_rebase:made is not from"
	[ "$(recval zfs_rebase:readonly "$POOL/onto")" = off ] || \
	    dfail "zfs_rebase:readonly did not record what it was"
	[ "$(recval zfs_rebase:onto "$POOL/onto")" = "$POOL/onto@$dname" ] || \
	    dfail "zfs_rebase:onto is not $POOL/onto@$dname"
	[ "$(recval zfs_rebase:base "$POOL/onto")" = "$POOL/base@base" ] || \
	    dfail "zfs_rebase:base is not $POOL/base@base"
	case "$dtag" in
	zr-*) ;;
	*) dfail "the tag is '$dtag', want zr-<12 hex>" ;;
	esac
	# from was a dataset, so the tool took its snapshot and named
	# it after its own tag.
	case "$dfrom" in
	"$POOL/from@zfs_rebase-$dtag"*) ;;
	*) dfail "zfs_rebase:from is $dfrom, want $POOL/from@zfs_rebase-$dtag" ;;
	esac
	hassnap "$POOL/onto@$dname" || \
	    dfail "the pre-apply snapshot $POOL/onto@$dname is not there"
	echo "ok   record: form dataset, made from, readonly off, the"
	echo "     pre-apply snapshot $dname, every property local"

	# -o named the manifest, so the resolution is beside it, and
	# the record says so: an unanswered skeleton, one line per
	# conflicted name, whatever form the run was made in.
	DRES=$tmp/got-d.resolution
	[ "$(recval zfs_rebase:resolution "$POOL/onto")" = "$DRES" ] || \
	    dfail "zfs_rebase:resolution is not $DRES"
	[ -f "$DRES" ] || dfail "the run wrote no resolution at $DRES"
	[ "$(sed -n 's/^#unanswered //p' "$DRES")" = "$want_names" ] || \
	    { head -8 "$DRES"; dfail "the skeleton is not wholly unanswered"; }

	dsay "the dataset is back in service"
	[ "$(zfs get -H -o value mountpoint "$POOL/onto")" = "$MNT/onto" ] || \
	    dfail "the mountpoint property was changed"
	[ "$(zfs get -H -o value mounted "$POOL/onto")" = yes ] || \
	    dfail "onto is not mounted after the run"
	mount | grep -q " on $MNT/onto " || \
	    dfail "onto is not mounted at $MNT/onto"
	[ "$(zfs get -H -o value readonly "$POOL/onto")" = off ] || \
	    dfail "readonly was not put back to off"
	[ -d "$DRUN/mnt" ] || dfail "no run directory at $DRUN"
	echo "ok   handed back: at $MNT/onto, readonly off, mountpoint kept"

	dstate=$(recval zfs_rebase:state "$POOL/onto")
	if [ $clean -eq 1 ]; then
		[ "$dstate" = done ] || dfail "the state is $dstate, want done"
		for s in "$POOL/base@base" "$POOL/onto@$dname"; do
			held=$(zfs holds -H "$s") || dfail "zfs holds $s"
			[ -z "$held" ] || dfail "$s is still held: $held"
		done
		hassnap "$dfrom" && \
		    dfail "$dfrom survived done; the tool made it"
		again "$tmp/d-again"
		idempotent "$tmp/d-again" 0
		echo "ok   done: holds released, $dfrom destroyed, and the"
		echo "     live tree is the rebased tree"
	else
		[ "$dstate" = conflicts ] || \
		    dfail "the state is $dstate, want conflicts"
		for s in "$POOL/base@base" "$dfrom" "$POOL/onto@$dname"; do
			held=$(holdtags "$s") || dfail "zfs holds $s"
			[ "$held" = "$dtag" ] || \
			    dfail "$s is held under '$held', want '$dtag'"
		done
		again "$tmp/d-again"
		idempotent "$tmp/d-again" "$want_conf"
		echo "ok   conflicts: the clean actions are in the live tree,"
		echo "     each input held once under $dtag, $dfrom kept"
	fi

	dsay "the verbs"
	"$bin" --verify --result "$POOL/onto" > "$tmp/d-verify" 2>&1
	dst=$?
	[ $dst -eq 0 ] || { cat "$tmp/d-verify"; dfail "--verify exited $dst"; }
	grep -q 'drifted 0' "$tmp/d-verify" || \
	    { cat "$tmp/d-verify"; dfail "--verify found drift"; }
	grep -q 'pending 0' "$tmp/d-verify" || \
	    { cat "$tmp/d-verify"; dfail "--verify found pending actions"; }
	mount | grep -q " on $MNT/onto " || \
	    dfail "--verify did not hand the dataset back"
	[ "$(recval zfs_rebase:state "$POOL/onto")" = "$dstate" ] || \
	    dfail "--verify moved the state"
	"$bin" --continue --result "$POOL/onto" > "$tmp/d-cont" 2>&1
	dst=$?
	if [ $clean -eq 1 ]; then
		[ $dst -eq 0 ] || \
		    { cat "$tmp/d-cont"; dfail "--continue on done exited $dst"; }
	else
		[ $dst -eq 1 ] || \
		    { cat "$tmp/d-cont"; dfail "--continue exited $dst, want 1"; }
		grep -q "$DRES" "$tmp/d-cont" || \
		    { cat "$tmp/d-cont"; dfail "--continue named no resolution"; }
		grep -q "$want_names of $want_names name" "$tmp/d-cont" || \
		    { cat "$tmp/d-cont"; dfail "--continue did not count the unanswered"; }
	fi
	[ "$(recval zfs_rebase:state "$POOL/onto")" = "$dstate" ] || \
	    dfail "--continue moved the state"
	mount | grep -q " on $MNT/onto " || \
	    dfail "--continue did not hand the dataset back"
	echo "ok   --verify and --continue: exit 0 and $dst, the state"
	echo "     unmoved, the dataset handed back each time"

	# An open rebase is not rebased over, flag or no flag. Only a
	# conflicted fixture is open here; a clean one reached done,
	# which is the other half and is checked in the --overwrite
	# pass below.
	if [ $clean -eq 0 ]; then
		for extra in "" --overwrite; do
			"$bin" $extra --from "$POOL/from" --onto "$POOL/onto" \
			    --result second > /dev/null 2>&1
			dst=$?
			[ $dst -eq 2 ] || dfail \
			    "a run over an open rebase with '$extra' exited $dst, want 2"
		done
		hassnap "$POOL/onto@second" && \
		    dfail "the refused run took $POOL/onto@second"
		[ "$(recval zfs_rebase:state "$POOL/onto")" = "$dstate" ] || \
		    dfail "a refused run moved the state"
		echo "ok   an open rebase is refused with and without"
		echo "     --overwrite (exit 2), and nothing was touched"
	fi

	dsay "abort"
	"$bin" --abort --result "$POOL/onto" > "$tmp/d-abort" 2>&1
	dst=$?
	[ $dst -eq 0 ] || { cat "$tmp/d-abort"; dfail "--abort exited $dst"; }
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || dfail "still local on $POOL/onto: $left"
	hassnap "$POOL/onto@$dname" && \
	    dfail "the pre-apply snapshot survived the abort"
	hassnap "$dfrom" && dfail "$dfrom survived the abort"
	[ -e "$DRES" ] && dfail "the resolution $DRES survived the abort"
	[ -e "$DRUN" ] && dfail "$DRUN survived the abort"
	mount | grep -q " on $MNT/onto " || \
	    dfail "onto is not at $MNT/onto after the abort"
	[ "$(zfs get -H -o value readonly "$POOL/onto")" = off ] || \
	    dfail "the abort left readonly on"
	onto_is_the_fixture "$tmp/d-after"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || dfail "zfs holds $s"
		[ -z "$held" ] || dfail "$s is still held: $held"
	done
	echo "ok   abort: rolled back to what onto was, the snapshots and"
	echo "     the record gone, mounted at home, no hold left"
}

# What the dataset form refuses, on probe.zrt alone: none of these
# answers can depend on the fixture, and what they prove is that a
# run which is refused gives back everything it had made. The last
# of them is the exclusivity itself -- a file held open under onto
# means the unmount fails, and the unmount is the whole of it.
case "$fixture" in
*/probe.zrt|probe.zrt)
	say "D0. the dataset form's refusals (probe.zrt)"
	# A dry run over two datasets must read something, so it takes
	# a snapshot of each side, and must leave nothing at all: no
	# snapshot, no record, no run directory. --result is ignored.
	"$bin" -n $flag -o "$tmp/dry-d" --from "$POOL/from" \
	    --onto "$POOL/onto" > /dev/null 2>&1
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || fail "-n over two datasets exited $st"
	sed -n '/^#mode/,$p' "$tmp/dry-d" > "$tmp/dry-d.body"
	cmp -s "$tmp/expect.body" "$tmp/dry-d.body" || \
	    { diff "$tmp/expect.body" "$tmp/dry-d.body" | head -20; \
	      fail "the dry run over two datasets decided something else"; }
	grep -q "^#base $POOL/base@base\$" "$tmp/dry-d" || \
	    fail "the dry run over two datasets did not derive the base"
	for d in from onto; do
		n=$(zfs list -H -o name -t snapshot -r "$POOL/$d" | \
		    wc -l | tr -d ' ')
		[ "$n" -eq 1 ] || fail "-n left a snapshot on $POOL/$d"
	done
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || fail "-n left $left on $POOL/onto"
	[ -e "$DRUN" ] && fail "-n left a run directory"
	echo "ok   -n over two datasets: the same manifest, and its own"
	echo "     snapshots taken and destroyed again"

	# --result in the dataset form names a snapshot of onto and of
	# nothing else.
	"$bin" $flag --from "$POOL/from" --onto "$POOL/onto" \
	    --result "$POOL/other@pre" > "$tmp/badresult" 2>&1
	st=$?
	[ $st -eq 2 ] || { cat "$tmp/badresult"; fail "--result naming another dataset's snapshot exited $st, want 2"; }
	hassnap "$POOL/other@pre" && fail "that run took $POOL/other@pre"
	echo "ok   --result $POOL/other@pre refused (exit 2)"

	busy=$(find "$MNT/onto" -type f | head -1)
	[ -n "$busy" ] || busy=$MNT/onto
	sleep 30 < "$busy" &
	sleeper=$!
	"$bin" $flag --from "$POOL/from" --onto "$POOL/onto" \
	    --result pre > "$tmp/busy" 2>&1
	st=$?
	kill "$sleeper" 2>/dev/null
	wait "$sleeper" 2>/dev/null
	[ $st -eq 2 ] || { cat "$tmp/busy"; fail "a busy onto exited $st, want 2"; }
	grep -q 'in use' "$tmp/busy" || \
	    { cat "$tmp/busy"; fail "the refusal did not say onto is in use"; }
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || fail "the refused run left $left on $POOL/onto"
	hassnap "$POOL/onto@pre" && fail "the refused run left $POOL/onto@pre"
	n=$(zfs list -H -o name -t snapshot -r "$POOL/from" | wc -l | tr -d ' ')
	[ "$n" -eq 1 ] || fail "the refused run left a snapshot on $POOL/from"
	[ -e "$DRUN" ] && fail "the refused run left a run directory"
	mount | grep -q " on $MNT/onto " || \
	    fail "the refused run left onto unmounted"
	echo "ok   a file open under onto: exit 2, and the run took"
	echo "     back its snapshot, its record and its directory"
	;;
esac

say "D1. the dataset form, the short spelling"
dataset_pass pre pre

# --overwrite replaces a record whose rebase reached done, and only
# that. A clean fixture is the one that gets there, so this is where
# the other half of the rule is shown; the open-record half is in the
# pass above, on every conflicted fixture.
if [ $clean -eq 1 ]; then
	dspec="pre, then --overwrite"
	say "D2. --overwrite over a record that reached done"
	"$bin" $flag -o "$tmp/got-o1" --from "$POOL/from" \
	    --onto "$POOL/onto" --result pre > "$tmp/o1" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/o1"; fail "the run before the --overwrite run exited $st"; }
	[ "$(recval zfs_rebase:state "$POOL/onto")" = done ] || \
	    fail "that run did not reach done"
	"$bin" $flag -o "$tmp/got-o2" --from "$POOL/from" \
	    --onto "$POOL/onto" --result second > "$tmp/o2" 2>&1
	st=$?
	[ $st -eq 2 ] || { cat "$tmp/o2"; fail "a done record without --overwrite exited $st, want 2"; }
	grep -q -- '--overwrite' "$tmp/o2" || \
	    { cat "$tmp/o2"; fail "the refusal did not name --overwrite"; }
	[ "$(recval zfs_rebase:onto "$POOL/onto")" = "$POOL/onto@pre" ] || \
	    fail "the refused run changed the record"
	"$bin" $flag --overwrite -o "$tmp/got-o3" --from "$POOL/from" \
	    --onto "$POOL/onto" --result second > "$tmp/o3" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/o3"; fail "the --overwrite run exited $st"; }
	[ "$(recval zfs_rebase:onto "$POOL/onto")" = "$POOL/onto@second" ] || \
	    fail "the --overwrite run did not replace the record"
	[ "$(recval zfs_rebase:state "$POOL/onto")" = done ] || \
	    fail "the --overwrite run did not reach done"
	"$bin" --abort --result "$POOL/onto" > "$tmp/o-abort" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/o-abort"; fail "the abort after --overwrite exited $st"; }
	# That abort rolled onto back to @second, which was taken
	# after the first run had already rebased it; @pre is the
	# fixture's own tree and is still here, unowned, because a
	# rebase that reaches done keeps its before-image.
	hassnap "$POOL/onto@pre" || \
	    fail "@pre did not survive a rebase that reached done"
	zfs rollback "$POOL/onto@pre" || fail "cannot roll back to @pre"
	zfs destroy "$POOL/onto@pre" || fail "cannot destroy @pre"
	dspec=pre
	onto_is_the_fixture "$tmp/o-after"
	echo "ok   --overwrite: refused without it, taken with it, and"
	echo "     the done rebase's before-image kept until asked for"
fi

# The same pass again with --result spelled in full, which must name
# the same snapshot of the same dataset.
say "D3. the dataset form again, the full spelling"
dataset_pass "$POOL/onto@pre" pre
rc=0
exit 0
