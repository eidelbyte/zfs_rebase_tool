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
# A bogus zfs_rebase:tag, zfs_rebase:manifest and zfs_rebase:phase are
# set on the pool root before the run, because user properties inherit
# down the naming tree: every property the tool reads back must be the
# result's own local value, and a dataset that only inherits them is
# not a result.
#
# What is checked, in order:
#   0. the derivation refuses what it should: a linear pair, where
#      one side is an ancestor of the other, and a pair that shares
#      no origin at all -- both exit 2 -- and --base without
#      --allow-unrelated is a usage error, since a derived base is
#      not open to a second opinion;
#  0a. for probe.zrt, --allow-unrelated: that same unrelated pair
#      goes through with no base at all, its manifest saying
#      "#base - 0"; it goes through with --base given, whose snapshot
#      is walked like the other two; and a --base newer than a side
#      is refused with exit 2;
#   1. -n: the manifest equals the fixture's expect block from the
#      #mode line on, which is where the decision starts -- above it
#      the header is this run's own and the fixture's is --posix's
#      (v4-manifest.md, section 6) -- and its #base line names the
#      snapshot the two sides were cloned from, with that snapshot's
#      guid beside it; and for
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
#      its mountpoint property none and the clone itself mounted at
#      /var/db/zfs_rebase/$POOL/result/mnt while the rebase is open,
#      or unmounted with that property still none once the rebase
#      has reached done, which is the void the tool hands it to and
#      the placement line on stderr says how to leave (the harness
#      places it with zfs set mountpoint to read its tree); the record
#      itself, which is zfs_rebase:manifest and zfs_rebase:tag and
#      (at a gate) zfs_rebase:phase and nothing else, every one of
#      them a local value that beats the bogus one on the parent,
#      with no zfs_rebase:quiet since no --quiet was given; the three
#      snapshot names with their guids, the form, the mode and the
#      rest read out of the manifest's header, which is where they
#      live now; zfs_rebase:phase "conflicts" for a conflicted
#      fixture and no property at all for a clean one, which reached
#      done and cleared the record; --abort on a plain dataset
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
#  3a. the verbs on that result. On a conflicted fixture the rebase
#      is open: --verify exits 0 and prints the counts, having
#      written nothing and moved no phase -- a conflicted run applied
#      its clean actions too, so every action is done and only the
#      conflicts are outstanding -- and --continue exits 1, naming
#      the resolution it waits for, and leaves the phase and the tree
#      exactly as they were. On a clean fixture the rebase reached
#      done and took its record off, so there is no rebase there any
#      more: every verb exits 2 saying so and touches nothing, which
#      is what a settled result is. (Naming a settled result by its
#      manifest is verify-settled's, two issues on.) Either way every
#      verb refuses the plain dataset that only inherits the record
#      properties, with exit 2 and no harm to it; and --result
#      spelled as a snapshot of the result, one that does not even
#      exist, finds the same rebase, since the name is taken as its
#      dataset;
#  3b. for probe.zrt, drift and its repair: /n, which the manifest
#      copied, is edited behind the tool's back with readonly off
#      and on again, --verify then exits 3 naming "drifted 1, first
#      /n" and fixes nothing, --continue --verify puts it back and
#      exits per the branch, and --verify is clean again with the
#      result read-only;
#  3c. for probe.zrt, --restart: the clone is destroyed and made
#      again from the onto snapshot the header names, with the same
#      record, the manifest is applied from the first gate, and the
#      run lands at the same phase under the same tag with the same
#      three holds and the same tree;
#   4. the end of the rebase. Where one is still open, --abort:
#      exit 0, every hold released, the dataset gone, the recorded
#      manifest unlinked, the run directory gone down to
#      /var/db/zfs_rebase, and a second --abort exit 2 because there
#      is no such run. Where it reached done there is nothing to
#      abort -- the record is off, so --abort exits 2 and touches
#      nothing -- and the result is the user's, unmounted and
#      unplaced: the harness destroys it and removes the run
#      directory itself, which is what done-cleanup will do at done;
#   5. a second real run given --verify -- every clean fixture, and
#      probe.zrt as the conflicted one: its tag is a new one, and on
#      a clean fixture the final check runs at the done gate (its
#      report is printed, the record is cleared and the holds are
#      released), which is where --verify is due. Nothing is
#      recorded: the flag belongs to the invocation that reaches the
#      gate;
#
# and then, in the dataset form:
#  D0. for probe.zrt, exclusivity: a file held open under onto makes
#      the unmount fail, the run exits 2 saying onto is in use, and
#      it takes back everything it had made -- its record, the
#      pre-apply snapshot, the snapshot it took of from, and its run
#      directory -- leaving onto mounted where it was with canmount
#      untouched, since the unmount is the first thing the take does
#      and nothing after it ran. (The other refusal of the take,
#      a dataset whose canmount is off, is run-precond.sh's: it needs
#      a dataset built for it and no fixture of ours has one.)
#  D1. the run itself: the manifest is the clone form's manifest
#      exactly and derives the same base; the record on $POOL/onto is
#      the manifest's path and the tag, local against the bogus ones
#      on the pool root, and its header says form dataset, made from,
#      the readonly it found, the pre-apply snapshot as #presnap and
#      #onto and the tool's own snapshot of from as #from; the
#      dataset is where the rebase has it -- at the private mount
#      with canmount noauto and readonly off while the rebase is
#      open, the conflicts gate included, and home at its own
#      mountpoint with the readonly and the canmount the fixture
#      built once the rebase has reached done -- with the mountpoint
#      property untouched throughout; the pre-apply
#      snapshot is there; and then per branch -- a clean fixture at
#      done with no record left, every hold released, the tool's own
#      snapshot destroyed and the live tree the rebased tree, a
#      conflicted one at phase conflicts with the clean actions
#      applied, the three holds under the record's tag and the same
#      conflicts declared by a second rebase. --verify and --continue
#      then behave as they do in the clone form and leave the dataset
#      at the private mount each time, since home is reached at done
#      and at --abort and nowhere else, an open rebase is refused
#      (there is no flag that would overrule it any more), and
#      --abort rolls onto back to what it was, destroys both
#      snapshots, takes every zfs_rebase: property off it and leaves
#      it mounted at home with canmount and readonly as they were,
#      holding the tree the fixture built;
#  D2. for a clean fixture, a settled dataset: the rebase that
#      reached done took its record off, so a second run over that
#      same dataset is taken with no flag at all, and the
#      before-image of the first is still there afterwards, because a
#      rebase that finished keeps it. (Its run directory is still
#      there too, which is what blocks a second run until
#      done-cleanup removes it at done; the harness removes it here
#      and says so.);
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
# The portable flavor answers every ZFS call with this line; the box
# wants the freebsd flavor, and the Makefile keeps the two apart.
if "$bin" --abort --result zr-flavor-probe/none 2>&1 |
    grep -q 'not built with ZR_FREEBSD'; then
	echo "$bin is the portable build: make clean && make freebsd"
	exit 2
fi

POOL=zrtbox
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
RUNDIR=/var/db/zfs_rebase/$POOL/result
MD=
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-box.XXXXXX") || exit 2
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
# Every zfs_rebase: property that is this dataset's own. The pool root
# carries bogus ones, so an inherited value must never be counted: a
# record is two properties at birth, three at a gate, and none at all
# once the rebase has reached done.
localprops() {
	zfs get -H -o property,source all "$1" 2>/dev/null | \
	    awk '$1 ~ /^zfs_rebase:/ && $2 == "local" { print $1 }'
}
# One line of a manifest's header, which is where a rebase's identity
# lives: the three snapshots and their guids, the form, the mode, what
# the tool snapshotted itself, the pre-apply snapshot and the two
# properties the dataset form gives back.
hdr() { sed -n "s/^#$1 //p" "$2"; }
# Is that snapshot there at all?
hassnap() { zfs list -H -o name -t snapshot "$1" > /dev/null 2>&1; }
# Where the clone's tree is to be read just now, with the assertion
# that it is there. While the rebase is open the clone is at the run's
# private mount and its mountpoint property is none, which it is from
# the create on and stays; done unmounts it and leaves the property
# none, which is the void the tool hands it to, and placing it is the
# user's work -- the tool's last line says how. The harness does
# exactly what that line says and reads the tree where it lands.
clone_open() {
	mp=$(zfs get -H -o value mountpoint "$POOL/result")
	[ "$mp" = none ] || fail "the clone's mountpoint is $mp, want none"
	mount | grep -q " on $RUNDIR/mnt " || \
	    fail "the clone is not at the private mount $RUNDIR/mnt"
	cmnt=$RUNDIR/mnt
}
clone_placed() {
	mp=$(zfs get -H -o value mountpoint "$POOL/result")
	[ "$mp" = none ] || \
	    fail "a settled clone's mountpoint is $mp, want none"
	[ "$(zfs get -H -o value mounted "$POOL/result")" = no ] || \
	    fail "a settled clone is still mounted"
	[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || \
	    fail "a settled clone is not read-only"
	zfs set mountpoint="$MNT/result" "$POOL/result" || \
	    fail "cannot place the settled clone"
	mount | grep -q " on $MNT/result " || \
	    fail "placing the clone did not mount it at $MNT/result"
	cmnt=$MNT/result
}
# And the line the tool prints at done, which is the whole of what it
# says about a clone it has finished with.
placement_line() {		# LOGFILE
	grep -q "$POOL/result is the rebased tree, unmounted; place it with zfs inherit mountpoint $POOL/result or zfs set mountpoint=PATH $POOL/result" "$1" || \
	    { cat "$1"; fail "done did not say how to place the clone"; }
}

# A rebase that reached done left no record, so --abort has nothing to
# find and says so; what is left is the result itself and, until
# done-cleanup lands, the run directory. The harness takes both away.
settled_clone() {
	"$bin" --abort --result "$POOL/result" > "$tmp/settled" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/settled"; fail "--abort on a settled result exited $st, want 2"; }
	[ "$(zfs list -H -o name "$POOL/result" 2>/dev/null)" = "$POOL/result" ] || \
	    fail "the refused --abort took $POOL/result away"
	zfs destroy "$POOL/result" || fail "cannot destroy the settled result"
	rmdir "$MNT/result" 2>/dev/null
	rmdir "$RUNDIR/mnt" "$RUNDIR" || \
	    fail "cannot remove the run directory of a settled rebase"
	rmdir "/var/db/zfs_rebase/$POOL" 2>/dev/null
	rm -f "$tmp/got" "$RES"
}
# every hold on a snapshot, as "tag" lines
holdtags() { zfs holds -H "$1" | cut -f2; }
# and every hold in the pool, counted: a rebase that is open holds
# three, and one that is not holds none.
holdcount() {
	n=0
	for hs in $(zfs list -H -o name -t snapshot -r "$POOL"); do
		c=$(zfs holds -H "$hs" | grep -c .)
		n=$((n + c))
	done
	printf '%s' "$n"
}
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
zfs set zfs_rebase:phase=bogus "$POOL" || exit 2
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
	grep -q '^#base - 0$' "$tmp/got-u" || \
	    { head -5 "$tmp/got-u"; fail "the empty base is not '#base - 0'"; }
	echo "ok   --allow-unrelated with no base (exit $st), #base - 0"
	# A base given by hand: older than both sides, in one pool
	# with them, and its dataset mounted, so its tree is walked
	# like the other two.
	"$bin" --allow-unrelated --base "$POOL/base@base" -n \
	    -o "$tmp/got-ub" --from "$POOL/other@x" --onto "$POOL/onto@work"
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || \
	    fail "--allow-unrelated --base exited $st, want 0 or 1"
	grep -q "^#base $POOL/base@base [0-9][0-9]*\$" "$tmp/got-ub" || \
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
grep -q "^#base $POOL/base@base [0-9][0-9]*\$" "$tmp/got-n" || { head -5 "$tmp/got-n"; fail "the dry run did not derive $POOL/base@base"; }
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
    --onto "$POOL/onto@work" --result "$POOL/result" > "$tmp/run2" 2>&1
st=$?
cat "$tmp/run2"
sed -n '/^#mode/,$p' "$tmp/got" > "$tmp/got.body"
cmp -s "$tmp/expect.body" "$tmp/got.body" || { diff "$tmp/expect.body" "$tmp/got.body" | head -20; fail "real-run manifest differs"; }
grep -q "^#base $POOL/base@base [0-9][0-9]*\$" "$tmp/got" || { head -5 "$tmp/got"; fail "the real run did not derive $POOL/base@base"; }
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
# settled is 1 once this rebase has reached done, which is where the
# record goes: a clean fixture is there already, a conflicted one only
# after its resolution is answered (3d).
settled=$clean
tag=$(recval zfs_rebase:tag "$POOL/result")
if [ $clean -eq 1 ]; then
	# done releases them, and takes the record off after it.
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
# The clone's mountpoint property is none from the create on and
# stays none: while the rebase is open the clone is at the run's own
# place, mounted there with zfs_mount_at, and at done it is unmounted
# and handed to the user to place.
if [ $clean -eq 1 ]; then
	placement_line "$tmp/run2"
	clone_placed
	echo "ok   done handed the clone to the void: unmounted, readonly"
	echo "     on, mountpoint none; the harness placed it at $cmnt"
else
	clone_open
	echo "ok   the clone is at the private mount $cmnt with mountpoint"
	echo "     none, which it never leaves while the rebase is open"
fi

# The record is four properties at most, and here it is two or three:
# the manifest and the tag from birth, the phase once a gate has been
# passed, and no quiet, since no --quiet was given. A rebase that
# reached done has none of them at all.
RES=$tmp/got.resolution
if [ $settled -eq 1 ]; then
	left=$(localprops "$POOL/result")
	[ -z "$left" ] || \
	    fail "a rebase that reached done left $left on $POOL/result"
	echo "ok   record: none at all, the rebase reached done"
else
	for prop in manifest tag phase; do
		src=$(recsrc "zfs_rebase:$prop" "$POOL/result")
		[ "$src" = local ] || \
		    fail "zfs_rebase:$prop has source $src, want local"
	done
	[ "$(recval zfs_rebase:tag "$POOL/result")" != bogus ] || \
	    fail "the result inherited the parent's zfs_rebase:tag"
	[ "$(recval zfs_rebase:manifest "$POOL/result")" = "$tmp/got" ] || \
	    fail "zfs_rebase:manifest is not $tmp/got"
	[ "$(recsrc zfs_rebase:quiet "$POOL/result")" != local ] || \
	    fail "zfs_rebase:quiet is set although no --quiet was given"
	left=$(localprops "$POOL/result")
	n=$(printf '%s\n' "$left" | grep -c .)
	[ "$n" -eq 3 ] || \
	    fail "the record is $n propert$(if [ "$n" = 1 ]; then echo y; else echo ies; fi), want 3: $left"
	echo "ok   record: the manifest, the tag and the phase, every one"
	echo "     of them local, and nothing else"
fi
# And everything else about the rebase is in the manifest's header,
# which is the one place it lives now.
[ "$(hdr form "$tmp/got")" = clone ] || \
    fail "the header's #form is not clone"
[ "$(hdr result "$tmp/got")" = "$POOL/result" ] || \
    fail "the header's #result is not $POOL/result"
[ "$(hdr made "$tmp/got")" = "-" ] || \
    fail "#made is not \"-\"; the tool took no snapshots"
# No --take flag was given, so the skeleton was written unanswered
# and the header says so; --restart reads this back.
[ "$(hdr take "$tmp/got")" = "-" ] || fail "#take is not \"-\""
want=strict
[ -n "$flag" ] && want=permissive-merge
[ "$(hdr mode "$tmp/got")" = "$want" ] || fail "#mode is not $want"
case "$(hdr tag "$tmp/got")" in
zr-*) ;;
*) fail "#tag is $(hdr tag "$tmp/got"), want zr-<12 hex>" ;;
esac
# -o names the manifest and the resolution goes beside it, which is
# that name and .resolution: nothing records that path, and every
# verb derives it from the manifest's the way the run did.
[ -f "$RES" ] || fail "the run wrote no resolution at $RES"
grep -q '^#rebase-resolution 5$' "$RES" || \
    { head -3 "$RES"; fail "$RES is no resolution"; }
grep -q "^#onto $POOL/onto@work [0-9][0-9]*\$" "$RES" || \
    { head -8 "$RES"; fail "the resolution names other snapshots"; }
# One line per conflicted name of the manifest, and every one of
# them unanswered: that is what a skeleton is.
want_names=$(grep -c ' conflict [0-9][0-9]*$' "$tmp/expect" || true)
[ "$(sed -n 's/^#names //p' "$RES")" = "$want_names" ] || \
    { head -8 "$RES"; fail "the resolution has not $want_names names"; }
[ "$(sed -n 's/^#unanswered //p' "$RES")" = "$want_names" ] || \
    { head -8 "$RES"; fail "the skeleton is not wholly unanswered"; }
# The three snapshots and their guids, as the header names them: the
# name is what the user called the snapshot and the guid is what it
# is, and a verb holds both against the pool before it moves.
for side in base:$POOL/base@base from:$POOL/from@work onto:$POOL/onto@work; do
	which=${side%%:*}
	snap=${side#*:}
	guid=$(zfs get -H -o value guid "$snap")
	[ "$(hdr "$which" "$tmp/got")" = "$snap $guid" ] || \
	    fail "#$which is $(hdr "$which" "$tmp/got"), want $snap $guid"
done
echo "ok   the header: the three snapshots and guids, the result,"
echo "     the form, the mode, made, take and the tag"

# A dataset that only inherits the properties is not a result.
"$bin" --abort --result "$POOL/plain" > /dev/null 2>&1
st=$?
[ $st -eq 2 ] || fail "--abort on an inheriting dataset exited $st, want 2"
[ "$(zfs list -H -o name "$POOL/plain" 2>/dev/null)" = "$POOL/plain" ] \
    || fail "--abort destroyed $POOL/plain, which is no result of ours"
echo "ok   an inherited record is no record: abort refused (exit 2)"

phase=$(recval zfs_rebase:phase "$POOL/result")
[ -f "$tmp/got" ] || fail "no manifest at $tmp/got"
want_conf=$(sed -n 's/^#conflicts //p' "$tmp/expect")
if [ $clean -eq 1 ]; then
	# done is no phase: it is the absence of the whole record,
	# which the property list above has already shown.
	[ "$phase" = "-" ] || \
	    fail "zfs_rebase:phase is $phase on a rebase that reached done"
	again "$tmp/again"
	idempotent "$tmp/again" 0
	echo "ok   result: done, and rebasing from onto it again is a no-op"
else
	[ "$phase" = conflicts ] || \
	    fail "zfs_rebase:phase is $phase, want conflicts"
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
if [ $settled -eq 1 ]; then
	# A rebase that reached done took its record off, so there is
	# no rebase here for a verb to find: each of them says so and
	# leaves the result standing. Naming a settled result by its
	# manifest is verify-settled's, two issues on.
	for verb in --verify --continue --restart --abort; do
		"$bin" $verb --result "$POOL/result" > "$tmp/settled1" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/settled1"; fail "$verb on a settled result exited $st, want 2"; }
		grep -q 'not a zfs_rebase result' "$tmp/settled1" || \
		    { cat "$tmp/settled1"; fail "$verb did not say there is no record"; }
	done
	[ "$(zfs list -H -o name "$POOL/result" 2>/dev/null)" = "$POOL/result" ] || \
	    fail "a refused verb took the settled result away"
	[ -z "$(localprops "$POOL/result")" ] || \
	    fail "a refused verb wrote a property on the settled result"
	again "$tmp/again2"
	idempotent "$tmp/again2" "$want_conf"
	echo "ok   a settled result: every verb exits 2, nothing touched"
else
	# --verify reports and writes nothing. Every action of the
	# manifest must be done by now, because a conflicted run
	# applies its clean actions too and the conflicts themselves
	# are not actions; blocked is possible and is not a failure.
	"$bin" --verify --result "$POOL/result" > "$tmp/verify1" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/verify1"; fail "--verify exited $st, want 0"; }
	grep -q 'done [0-9]' "$tmp/verify1" || \
	    { cat "$tmp/verify1"; fail "--verify printed no counts"; }
	grep -q 'drifted 0' "$tmp/verify1" || \
	    { cat "$tmp/verify1"; fail "--verify found drift"; }
	grep -q 'pending 0' "$tmp/verify1" || \
	    { cat "$tmp/verify1"; fail "--verify found pending actions"; }
	[ "$(recval zfs_rebase:phase "$POOL/result")" = "$phase" ] || \
	    fail "--verify moved the phase"
	echo "ok   --verify: exit 0, counts printed, still at $phase"

	# --continue on a rebase that is where it should be changes
	# nothing: it waits at its skeleton, which nobody has
	# answered, and says how much of it is unanswered.
	"$bin" --continue --result "$POOL/result" > "$tmp/cont1" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/cont1"; fail "--continue at conflicts exited $st, want 1"; }
	grep -q "$RES" "$tmp/cont1" || \
	    { cat "$tmp/cont1"; fail "--continue did not name the resolution"; }
	grep -q "$want_names of $want_names name"  "$tmp/cont1" || \
	    { cat "$tmp/cont1"; fail "--continue did not count the unanswered"; }
	[ "$(recval zfs_rebase:phase "$POOL/result")" = "$phase" ] || \
	    fail "--continue moved the phase"
	# and the tree it leaves is still the tree stage 1 made
	again "$tmp/again2"
	idempotent "$tmp/again2" "$want_conf"
	echo "ok   --continue: exit $st, the phase and the tree unchanged"

	# --result names the dataset carrying the record, and a
	# snapshot name is taken as its dataset: this one does not
	# even exist, and the verb still finds the rebase.
	"$bin" --verify --result "$POOL/result@nosuch" > /dev/null 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    fail "--verify on a snapshot spelling exited $st, want 0"
	echo "ok   --result $POOL/result@nosuch is the same rebase"
fi

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
	[ "$(recval zfs_rebase:phase "$POOL/result")" = "$phase" ] || \
	    fail "--restart did not land at $phase"
	[ "$(recval zfs_rebase:tag "$POOL/result")" = "$tag" ] || \
	    fail "--restart changed the tag"
	[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || \
	    fail "--restart left the result writable"
	# The clone was destroyed and made again, so it is at the
	# private mount again, with the mountpoint property none.
	clone_open
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
	echo "ok   --restart: rebuilt, at $phase, held under $tag, tree equal"

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
	# done left no record: the four properties are the rebase, and
	# the rebase is over.
	left=$(localprops "$POOL/result")
	[ -z "$left" ] || \
	    fail "the answered rebase reached done and left $left"
	settled=1
	# done by a --continue hands the clone to the void exactly as
	# done by the run itself does, and says the same line.
	placement_line "$tmp/cont3"
	clone_placed
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is still held after done: $held"
	done
	[ "$(zfs get -H -o value readonly "$POOL/result")" = on ] || \
	    fail "done left the result writable"
	# This case answers every conflict with keep, the one choice
	# that changes nothing, so the tree is still the one stage 1
	# made: a second rebase has no action and the same conflicts.
	# The choices that do change something are box-resolution's.
	again "$tmp/again4"
	idempotent "$tmp/again4" "$want_conf"
	echo "ok   answered: --continue -> done, the record off, the holds"
	echo "     released, the tree equal"
	;;
esac

say "4. the end of the rebase"
if [ $settled -eq 1 ]; then
	# It reached done, so there is no rebase to abort: --abort
	# says so and touches nothing, and the result is the user's.
	# Everything else done leaves -- the manifest, the resolution
	# and the run directory -- is done-cleanup's to take away; the
	# harness does it here so that the pool is ready for step 5.
	settled_clone
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is held after done: $held"
	done
	echo "ok   settled: --abort exits 2, and the result was the"
	echo "     harness's to take away"
else
	"$bin" --abort --result "$POOL/result" || fail "abort exited $?"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || \
		    fail "$s is still held after the abort: $held"
	done
	if zfs list -H -o name "$POOL/result" > /dev/null 2>&1; then
		fail "$POOL/result survived the abort"
	fi
	if [ -e "$tmp/got" ]; then
		fail "the recorded manifest $tmp/got survived the abort"
	fi
	if [ -e "$RES" ]; then
		fail "the resolution $RES survived the abort"
	fi
	if [ -e "/var/db/zfs_rebase/$POOL" ]; then
		fail "/var/db/zfs_rebase/$POOL survived the abort"
	fi
	"$bin" --abort --result "$POOL/result" 2>/dev/null
	st=$?
	[ $st -eq 2 ] || fail "a second abort exited $st, want 2"
	echo "ok   abort: the holds, the result, its manifest and its"
	echo "     directory are all gone"
fi

# A second run, given --verify: nothing is recorded -- the flag
# belongs to the invocation that reaches the gate -- and on a clean
# fixture the check is carried out at the done gate, which is where
# the final check belongs: after the last apply verified and before
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
	if [ $clean -eq 1 ]; then
		[ $st -eq 0 ] || fail "the clean --verify run exited $st"
		# It reached done, so the record is off: the check ran
		# before the release and the clearing, which is the
		# order the done gate has.
		left=$(localprops "$POOL/result")
		[ -z "$left" ] || \
		    fail "the --verify run reached done and left $left"
		grep -q 'drifted 0' "$tmp/verify5" || \
		    fail "the --verify run printed no final check"
		for s in "$POOL/base@base" "$POOL/from@work" \
		    "$POOL/onto@work"; do
			held=$(zfs holds -H "$s") || fail "zfs holds $s"
			[ -z "$held" ] || \
			    fail "$s is held after a --verify run reached done"
		done
		placement_line "$tmp/verify5"
		echo "ok   --verify: the check at the done gate, then the"
		echo "     holds and then the record, the clone unmounted"
		echo "     with its placement line, and nothing recorded"
		settled_clone
	else
		# It stopped at conflicts, so the rebase is open and
		# its record is there under a tag of its own.
		vtag=$(recval zfs_rebase:tag "$POOL/result")
		case "$vtag" in
		zr-*) ;;
		*) fail "the second run's tag is '$vtag', want zr-<12 hex>" ;;
		esac
		[ "$vtag" != "$tag" ] || \
		    fail "the second run reused the tag $tag"
		[ "$(recsrc zfs_rebase:quiet "$POOL/result")" != local ] || \
		    fail "zfs_rebase:quiet is set although no --quiet was given"
		"$bin" --abort --result "$POOL/result" || fail "abort exited $?"
		echo "ok   --verify: a conflicted run stops before the check"
		echo "     is due, under its own tag $vtag"
	fi
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is still held: $held"
	done
fi

case "$fixture" in
*/probe.zrt|probe.zrt)
	say "5a. --abort with the manifest gone (probe.zrt)"
	# The record is the manifest's path and the tag. Take the
	# manifest away and the tag is all that is left: the holds are
	# given back by walking the pool for it, the record is
	# cleared, and nothing is destroyed or rolled back, because
	# the form of the run was one of the things the manifest was
	# carrying. probe.zrt stops at conflicts, so there is a rebase
	# standing here to lose the manifest of.
	"$bin" $flag -o "$tmp/got-l" --from "$POOL/from@work" \
	    --onto "$POOL/onto@work" --result "$POOL/result" \
	    > "$tmp/l1" 2>&1
	st=$?
	[ $st -eq 1 ] || { cat "$tmp/l1"; fail "the run for 5a exited $st, want 1"; }
	ltag=$(recval zfs_rebase:tag "$POOL/result")
	[ "$(holdcount)" = 3 ] || fail "5a: $(holdcount) holds, want 3"
	rm -f "$tmp/got-l" "$tmp/got-l.resolution" || fail "cannot unlink the manifest"
	"$bin" --abort --result "$POOL/result" > "$tmp/l2" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/l2"; fail "--abort without the manifest exited $st, want 0"; }
	[ "$(holdcount)" = 0 ] || \
	    { cat "$tmp/l2"; fail "--abort without the manifest left $(holdcount) holds"; }
	grep -q "released $ltag on 3 snapshots" "$tmp/l2" || \
	    { cat "$tmp/l2"; fail "--abort did not say it released the tag by walking the pool"; }
	[ -z "$(localprops "$POOL/result")" ] || \
	    { cat "$tmp/l2"; fail "--abort without the manifest left a record"; }
	[ "$(zfs list -H -o name "$POOL/result" 2>/dev/null)" = "$POOL/result" ] || \
	    fail "--abort destroyed a result whose form it could not know"
	grep -q 'cannot tell the clone form from the dataset form' "$tmp/l2" || \
	    { cat "$tmp/l2"; fail "--abort did not say what it could not do"; }
	# The mountpoint property is the one thing left that tells the
	# two forms apart: none is a clone of the tool's, and a clone
	# has no home to be put at, so it is left unmounted.
	[ "$(zfs get -H -o value mountpoint "$POOL/result")" = none ] || \
	    fail "--abort changed the clone's mountpoint property"
	[ "$(zfs get -H -o value mounted "$POOL/result")" = no ] || \
	    fail "--abort left the clone mounted"
	grep -q "no mountpoint of its own and is left unmounted" "$tmp/l2" || \
	    { cat "$tmp/l2"; fail "--abort did not say the clone is unplaced"; }
	grep -q "zfs destroy $POOL/result" "$tmp/l2" || \
	    { cat "$tmp/l2"; fail "--abort did not print the clone form's command"; }
	grep -q "zfs rollback $POOL/result@PRE" "$tmp/l2" || \
	    { cat "$tmp/l2"; fail "--abort did not print the dataset form's command"; }
	# And the result is ours to take away, which is what the
	# message says.
	zfs destroy "$POOL/result" || fail "cannot destroy the result of 5a"
	rmdir "$RUNDIR/mnt" "$RUNDIR" 2>/dev/null
	rmdir "/var/db/zfs_rebase/$POOL" 2>/dev/null
	echo "ok   --abort without the manifest: the tag released by a"
	echo "     walk of the pool, the record cleared, nothing destroyed"
	;;
esac

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
	grep -q "^#base $POOL/base@base [0-9][0-9]*\$" "$tmp/got-d" || \
	    { head -5 "$tmp/got-d"; dfail "did not derive $POOL/base@base"; }

	dsay "the record on $POOL/onto"
	# dsettled is 1 once this pass's rebase has reached done,
	# which is where the record goes.
	dsettled=$clean
	dtag=$(recval zfs_rebase:tag "$POOL/onto")
	dfrom=$(hdr from "$tmp/got-d")
	dfrom=${dfrom%% *}
	if [ $dsettled -eq 1 ]; then
		left=$(localprops "$POOL/onto")
		[ -z "$left" ] || \
		    dfail "a rebase that reached done left $left on $POOL/onto"
	else
		for prop in manifest tag phase; do
			src=$(recsrc "zfs_rebase:$prop" "$POOL/onto")
			[ "$src" = local ] || \
			    dfail "zfs_rebase:$prop has source $src, want local"
		done
		[ "$dtag" != bogus ] || \
		    dfail "the dataset inherited the parent's zfs_rebase:tag"
		[ "$(recval zfs_rebase:manifest "$POOL/onto")" = "$tmp/got-d" ] || \
		    dfail "zfs_rebase:manifest is not $tmp/got-d"
		left=$(localprops "$POOL/onto")
		n=$(printf '%s\n' "$left" | grep -c .)
		[ "$n" -eq 3 ] || \
		    dfail "the record is $left, want the manifest, the tag and the phase"
	fi
	# The rest is the header's, in this form as in the other, with
	# the three lines only the dataset form writes.
	[ "$(hdr form "$tmp/got-d")" = dataset ] || \
	    dfail "#form is not dataset"
	[ "$(hdr result "$tmp/got-d")" = "$dspec" ] || \
	    dfail "#result is $(hdr result "$tmp/got-d"), want $dspec"
	[ "$(hdr made "$tmp/got-d")" = from ] || dfail "#made is not from"
	[ "$(hdr readonly "$tmp/got-d")" = off ] || \
	    dfail "#readonly did not record what it was"
	[ "$(hdr canmount "$tmp/got-d")" = on ] || \
	    dfail "#canmount did not record what it was"
	[ "$(hdr presnap "$tmp/got-d")" = "$POOL/onto@$dname" ] || \
	    dfail "#presnap is not $POOL/onto@$dname"
	[ "$(hdr take "$tmp/got-d")" = "-" ] || dfail "#take is not \"-\""
	[ "$(hdr onto "$tmp/got-d")" = "$POOL/onto@$dname $(zfs get -H -o value guid "$POOL/onto@$dname")" ] || \
	    dfail "#onto is $(hdr onto "$tmp/got-d")"
	[ "$(hdr base "$tmp/got-d")" = "$POOL/base@base $(zfs get -H -o value guid "$POOL/base@base")" ] || \
	    dfail "#base is $(hdr base "$tmp/got-d")"
	dhtag=$(hdr tag "$tmp/got-d")
	case "$dhtag" in
	zr-*) ;;
	*) dfail "#tag is '$dhtag', want zr-<12 hex>" ;;
	esac
	[ $dsettled -eq 1 ] || [ "$dtag" = "$dhtag" ] || \
	    dfail "zfs_rebase:tag is $dtag and #tag is $dhtag"
	# from was a dataset, so the tool took its snapshot and named
	# it after its own tag.
	case "$dfrom" in
	"$POOL/from@zfs_rebase-$dhtag"*) ;;
	*) dfail "#from is $dfrom, want $POOL/from@zfs_rebase-$dhtag" ;;
	esac
	hassnap "$POOL/onto@$dname" || \
	    dfail "the pre-apply snapshot $POOL/onto@$dname is not there"
	echo "ok   record: the manifest, the tag and the phase, local; the"
	echo "     header: form dataset, made from, readonly off, presnap"
	echo "     $POOL/onto@$dname"

	# -o named the manifest, so the resolution is beside it by
	# rule: an unanswered skeleton, one line per conflicted name,
	# whatever form the run was made in.
	DRES=$tmp/got-d.resolution
	[ -f "$DRES" ] || dfail "the run wrote no resolution at $DRES"
	[ "$(sed -n 's/^#unanswered //p' "$DRES")" = "$want_names" ] || \
	    { head -8 "$DRES"; dfail "the skeleton is not wholly unanswered"; }

	dsay "where the dataset is"
	# The mountpoint property is untouched from first to last: the
	# private mount is made with zfs_mount_at, which takes the path
	# as an argument, so what the property says is where the
	# dataset goes home to.
	[ "$(zfs get -H -o value mountpoint "$POOL/onto")" = "$MNT/onto" ] || \
	    dfail "the mountpoint property was changed"
	if [ $dsettled -eq 1 ]; then
		# done is one of the two moments a rebase puts the
		# dataset home, and it puts both properties back to
		# what the fixture built them as.
		[ "$(zfs get -H -o value mounted "$POOL/onto")" = yes ] || \
		    dfail "onto is not mounted after a run that reached done"
		mount | grep -q " on $MNT/onto " || \
		    dfail "onto is not mounted at $MNT/onto after done"
		[ "$(zfs get -H -o value readonly "$POOL/onto")" = off ] || \
		    dfail "readonly was not put back to off"
		[ "$(zfs get -H -o value canmount "$POOL/onto")" = on ] || \
		    dfail "canmount was not put back to on"
		cmnt=$MNT/onto
		echo "ok   done: home at $MNT/onto, readonly off, canmount on,"
		echo "     the mountpoint property untouched"
	else
		# And an open rebase holds it at the private mount, the
		# conflicts gate included: a half rebased tree is not
		# handed back into service while it waits to be
		# answered. canmount noauto is what keeps a reboot from
		# mounting it there either.
		mount | grep -q " on $DRUN/mnt " || \
		    dfail "onto is not at the private mount $DRUN/mnt"
		if mount | grep -q " on $MNT/onto "; then
			dfail "onto is at home while its rebase is open"
		fi
		[ "$(zfs get -H -o value canmount "$POOL/onto")" = noauto ] || \
		    dfail "canmount is not noauto while the run has the dataset"
		[ "$(zfs get -H -o value readonly "$POOL/onto")" = off ] || \
		    dfail "the private mount is not writable"
		cmnt=$DRUN/mnt
		echo "ok   at conflicts: held at $DRUN/mnt, canmount noauto,"
		echo "     readonly off, the mountpoint property untouched"
	fi
	[ -d "$DRUN/mnt" ] || dfail "no run directory at $DRUN"

	dphase=$(recval zfs_rebase:phase "$POOL/onto")
	if [ $clean -eq 1 ]; then
		[ "$dphase" = "-" ] || \
		    dfail "zfs_rebase:phase is $dphase on a rebase at done"
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
		[ "$dphase" = conflicts ] || \
		    dfail "the phase is $dphase, want conflicts"
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
	if [ $dsettled -eq 1 ]; then
		# The rebase reached done and took its record off, so
		# there is no rebase on this dataset for a verb to
		# find, and each of them says so and touches nothing.
		for verb in --verify --continue --restart --abort; do
			"$bin" $verb --result "$POOL/onto" > "$tmp/d-set" 2>&1
			dst=$?
			[ $dst -eq 2 ] || \
			    { cat "$tmp/d-set"; dfail "$verb on a settled dataset exited $dst, want 2"; }
		done
		mount | grep -q " on $MNT/onto " || \
		    dfail "a refused verb left onto unmounted"
		[ -z "$(localprops "$POOL/onto")" ] || \
		    dfail "a refused verb wrote a property on the dataset"
		echo "ok   a settled dataset: every verb exits 2, nothing"
		echo "     touched, onto still at home"
	else
		"$bin" --verify --result "$POOL/onto" > "$tmp/d-verify" 2>&1
		dst=$?
		[ $dst -eq 0 ] || \
		    { cat "$tmp/d-verify"; dfail "--verify exited $dst"; }
		grep -q 'drifted 0' "$tmp/d-verify" || \
		    { cat "$tmp/d-verify"; dfail "--verify found drift"; }
		grep -q 'pending 0' "$tmp/d-verify" || \
		    { cat "$tmp/d-verify"; dfail "--verify found pending actions"; }
		mount | grep -q " on $DRUN/mnt " || \
		    dfail "--verify did not leave the dataset at the private mount"
		[ "$(recval zfs_rebase:phase "$POOL/onto")" = "$dphase" ] || \
		    dfail "--verify moved the phase"
		"$bin" --continue --result "$POOL/onto" > "$tmp/d-cont" 2>&1
		dst=$?
		[ $dst -eq 1 ] || \
		    { cat "$tmp/d-cont"; dfail "--continue exited $dst, want 1"; }
		grep -q "$DRES" "$tmp/d-cont" || \
		    { cat "$tmp/d-cont"; dfail "--continue named no resolution"; }
		grep -q "$want_names of $want_names name" "$tmp/d-cont" || \
		    { cat "$tmp/d-cont"; dfail "--continue did not count the unanswered"; }
		[ "$(recval zfs_rebase:phase "$POOL/onto")" = "$dphase" ] || \
		    dfail "--continue moved the phase"
		mount | grep -q " on $DRUN/mnt " || \
		    dfail "--continue did not leave the dataset at the private mount"
		[ "$(zfs get -H -o value canmount "$POOL/onto")" = noauto ] || \
		    dfail "a verb changed canmount while the rebase is open"
		echo "ok   --verify and --continue: exit 0 and $dst, the phase"
		echo "     unmoved, the dataset still at $DRUN/mnt each time"

		# An open rebase is not rebased over, and there is no
		# flag left that would overrule it: a dataset carrying
		# any zfs_rebase: property of its own is an open
		# rebase and --continue or --abort settles it.
		"$bin" --from "$POOL/from" --onto "$POOL/onto" \
		    --result second > "$tmp/d-open" 2>&1
		dst=$?
		[ $dst -eq 2 ] || \
		    { cat "$tmp/d-open"; dfail "a run over an open rebase exited $dst, want 2"; }
		grep -q -- '--continue or --abort' "$tmp/d-open" || \
		    { cat "$tmp/d-open"; dfail "the refusal did not name --continue or --abort"; }
		hassnap "$POOL/onto@second" && \
		    dfail "the refused run took $POOL/onto@second"
		[ "$(recval zfs_rebase:phase "$POOL/onto")" = "$dphase" ] || \
		    dfail "a refused run moved the phase"
		echo "ok   an open rebase is refused (exit 2), and nothing"
		echo "     was touched"
	fi

	dsay "the end of the rebase"
	if [ $dsettled -eq 1 ]; then
		# Nothing to abort: the record is off. What done left
		# is the rebased tree, the pre-apply snapshot and the
		# run directory, and the harness puts the dataset back
		# itself, which is what --abort would have done.
		zfs rollback "$POOL/onto@$dname" || \
		    dfail "cannot roll onto back to @$dname"
		zfs destroy "$POOL/onto@$dname" || \
		    dfail "cannot destroy @$dname"
		rmdir "$DRUN/mnt" "$DRUN" || \
		    dfail "cannot remove the run directory $DRUN"
		rmdir "/var/db/zfs_rebase/$POOL" 2>/dev/null
		rm -f "$tmp/got-d" "$DRES"
	else
		"$bin" --abort --result "$POOL/onto" > "$tmp/d-abort" 2>&1
		dst=$?
		[ $dst -eq 0 ] || \
		    { cat "$tmp/d-abort"; dfail "--abort exited $dst"; }
		hassnap "$POOL/onto@$dname" && \
		    dfail "the pre-apply snapshot survived the abort"
		hassnap "$dfrom" && dfail "$dfrom survived the abort"
		[ -e "$DRES" ] && \
		    dfail "the resolution $DRES survived the abort"
		[ -e "$DRUN" ] && dfail "$DRUN survived the abort"
	fi
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || dfail "still local on $POOL/onto: $left"
	mount | grep -q " on $MNT/onto " || \
	    dfail "onto is not at $MNT/onto at the end of the pass"
	[ "$(zfs get -H -o value readonly "$POOL/onto")" = off ] || \
	    dfail "the end of the pass left readonly on"
	[ "$(zfs get -H -o value canmount "$POOL/onto")" = on ] || \
	    dfail "the end of the pass left canmount noauto"
	onto_is_the_fixture "$tmp/d-after"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || dfail "zfs holds $s"
		[ -z "$held" ] || dfail "$s is still held: $held"
	done
	echo "ok   the end: onto is what the fixture built, the snapshots"
	echo "     and the record gone, mounted at home, no hold left"
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
	grep -q "^#base $POOL/base@base [0-9][0-9]*\$" "$tmp/dry-d" || \
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
	# The unmount is the first thing the take does and it failed,
	# so neither property was ever written.
	[ "$(zfs get -H -o value canmount "$POOL/onto")" = on ] || \
	    fail "the refused run left canmount noauto on $POOL/onto"
	[ "$(zfs get -H -o value readonly "$POOL/onto")" = off ] || \
	    fail "the refused run left readonly on on $POOL/onto"
	echo "ok   a file open under onto: exit 2, and the run took"
	echo "     back its snapshot, its record and its directory,"
	echo "     with canmount and readonly untouched"
	;;
esac

say "D1. the dataset form, the short spelling"
dataset_pass pre pre

# A dataset whose rebase reached done carries no record, so it is
# free: a second run over it is taken with no flag at all, which is
# what replaced --overwrite. A clean fixture is the one that gets to
# done; the open-record half of the rule is in the pass above, on
# every conflicted fixture.
if [ $clean -eq 1 ]; then
	dspec="pre, then a second run"
	say "D2. a second run over a dataset whose rebase reached done"
	"$bin" $flag -o "$tmp/got-o1" --from "$POOL/from" \
	    --onto "$POOL/onto" --result pre > "$tmp/o1" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/o1"; fail "the first run exited $st"; }
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || fail "that run reached done and left $left"
	# The run directory is what done-cleanup will take away at
	# done; until it lands it is still here and would refuse the
	# second run ("a run for ... is in place"), so the harness
	# removes it, which is exactly what that issue automates.
	rmdir /var/db/zfs_rebase/"$POOL"/onto/mnt \
	    /var/db/zfs_rebase/"$POOL"/onto || \
	    fail "cannot remove the run directory the first run left"
	"$bin" $flag -o "$tmp/got-o2" --from "$POOL/from" \
	    --onto "$POOL/onto" --result second > "$tmp/o2" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/o2"; fail "a run over a settled dataset exited $st, want 0"; }
	[ "$(hdr presnap "$tmp/got-o2")" = "$POOL/onto@second" ] || \
	    fail "the second run's #presnap is not $POOL/onto@second"
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || fail "the second run reached done and left $left"
	# The second run rebased onto again over @second, which was
	# taken after the first had already rebased it; @pre is the
	# fixture's own tree and is still here, unowned, because a
	# rebase that reaches done keeps its before-image.
	hassnap "$POOL/onto@second" || fail "@second is not there"
	hassnap "$POOL/onto@pre" || \
	    fail "@pre did not survive a rebase that reached done"
	zfs rollback -r "$POOL/onto@pre" || fail "cannot roll back to @pre"
	zfs destroy "$POOL/onto@pre" || fail "cannot destroy @pre"
	rmdir /var/db/zfs_rebase/"$POOL"/onto/mnt \
	    /var/db/zfs_rebase/"$POOL"/onto || \
	    fail "cannot remove the second run's directory"
	rmdir "/var/db/zfs_rebase/$POOL" 2>/dev/null
	dspec=pre
	onto_is_the_fixture "$tmp/o-after"
	echo "ok   a settled dataset is free: the second run was taken"
	echo "     with no flag, and the first's before-image is kept"
fi

# The same pass again with --result spelled in full, which must name
# the same snapshot of the same dataset.
say "D3. the dataset form again, the full spelling"
dataset_pass "$POOL/onto@pre" pre
rc=0
exit 0
