#!/bin/sh
# Box harness: kill a run at every gate, then continue it. FreeBSD,
# root, after make freebsd. Usage:
#
#	run-kills.sh [FIXTURE.zrt ...]
#
# with tests/fixtures/probe.zrt (conflicted) and
# tests/fixtures/h-yw-row19.zrt (clean) as the default pair, so that
# both branches of the stages are exercised: the one that stops at
# conflicts and the one that runs through to done. The clean fixture
# is that one and not escapes.zrt, which declares a conflict; it
# wants two actions, since one gate is "before the second action the
# apply performs".
#
# The way in is the pause hook (tests/box/README.md): the tool is
# started with ZFS_REBASE_PAUSE=<gate>, it stops itself with SIGSTOP
# when it gets there, and the harness -- which polls for the T state
# -- signals it and sends SIGCONT. Every gate is crossed with SIGINT,
# SIGTERM and SIGKILL, in both forms of the tool, and every case ends
# in --continue and --abort. One pool per fixture; --abort is the
# reset between cases, and the invariants it must leave are checked
# before the next one starts.
#
# What each case asserts, which is not one rule but three, because
# what a stop leaves depends on whether the tool had written anything
# yet and on whether it was given the chance to tidy up:
#
#   torn   -- SIGINT and SIGTERM at held, cloned, read and decided.
#      Nothing has been written to the result yet, so a stop there is
#      a failure before the apply and the run takes itself away
#      whole: exit 3, no record, no holds, no run directory, no
#      snapshot of the tool's own, and in the dataset form onto back
#      at its own mount point holding the tree the fixture built.
#      There is nothing to continue and --continue says so (exit 2).
#      This is the one place the plan's "nothing is destroyed before
#      done or --abort" and the code part company, and the code is
#      deliberate: run.c's header says a stop before the apply
#      destroys the clone as any other failure before the apply does.
#
#   kept   -- every SIGKILL, and SIGINT and SIGTERM from applying1
#      on. The rebase is left standing at the gate it had reached:
#      the record with that phase (or no phase at all before the
#      decision, when the record is the manifest's path and the tag
#      alone), the three holds, the manifest -- which is there at
#      every gate, since the run writes its whole header before the
#      record and the decision over that header later -- and
#      the result, which is at the run's private mount in both forms:
#      the clone with its mountpoint property none, the dataset with
#      canmount noauto. In the clone form readonly is back on
#      wherever the tool had the chance to put it back (every SIGINT
#      and SIGTERM, and a SIGKILL at a gate where readonly was
#      already on) and off after a SIGKILL inside an applying stage.
#      In the dataset form the stop leaves the dataset privately
#      mounted whatever the signal was -- home is reached at done and
#      at --abort and at no gate between them, because a half rebased
#      tree waiting for its conflicts to be answered is not put back
#      into service (documents-design.md, section 5) -- except at the
#      held gate, which is before the take, where it is still at
#      home with canmount on. Its readonly property is what the
#      record says it was, at the private mount and at home alike,
#      since the tool changes it only while the dataset is off its
#      mountpoint (libzfs remounts at the mountpoint property on a
#      readonly change, which cannot land while the dataset sits at
#      the private mount) -- the private mount is root's alone and
#      writable for its life.
#
#   finished -- SIGINT and SIGTERM at done. Nothing looks at the flag
#      after that gate, so the run finishes: the holds released, the
#      record taken off -- in that order, since the tag is the only
#      handle on those holds -- and exit 0. done is no phase: what
#      says a rebase reached it is that the result carries no
#      zfs_rebase: property at all. A SIGKILL at that gate stops
#      before either, so what it leaves is the phase of the stage
#      that ran before it and the three holds, and the --continue
#      after it redoes that stage, which is idempotent, and
#      finishes.
#
# Then, in every case that left a rebase behind:
#
#   --continue --result reaches the gate the fixture's branch ends
#   in -- done for a clean one, conflicts for a conflicted one -- and
#   after it readonly is on, the holds are gone at done and there at
#   conflicts, and a --posix rebase of the fixture's from onto the
#   result declares zero actions, which is stage 1 idempotence. Where
#   it reached done the result is settled: the dataset home with
#   canmount and readonly as the fixture built them, the clone
#   unmounted with mountpoint none, which the harness places to read
#   its tree the way the tool's own last line says. Where it stopped
#   at conflicts the result is at the private mount, in both forms. A
#   kill before the decision is the exception: what the record names
#   is the header the run was born with, so there is nothing to
#   apply, --continue and --restart both refuse in those words and
#   exit 2, and the result is left exactly where the kill left it.
#   That case ends in --abort instead, which has the whole header
#   from the birth manifest: it releases the three holds, destroys
#   the clone or rolls the dataset back to the pre-apply snapshot
#   and destroys that, puts readonly and canmount back as the header
#   kept them, destroys the snapshot the run took of from, and takes
#   the record and the run directory away. Nothing is put back by
#   hand any more.
#
#   A --continue makes the final check itself if it reaches the done
#   gate, under no flag at all: the check is standard, made by
#   whichever invocation gets there, and its report is printed unless
#   the start was given -q.
#
#   Before that, --verify says what the kill left without touching
#   it: an action is pending until the stage that makes it has run,
#   so a rebase stopped before or inside applying1 exits 3 with
#   pending actions and one past it exits 0, and neither moves the
#   gate, the holds or the tree. Nor the mount and nor readonly: the
#   report reads the result where the kill left it, which is what a
#   verb that writes nothing has to do (documents-design.md, section
#   11.6), and a kill inside a stage is where the difference shows,
#   since the stage left readonly off.
#
#   At the held gate, while the tool is stopped, zfs destroy of each
#   held snapshot must fail and leave the snapshot standing: that is
#   what the holds are for. Where nothing is cloned from the
#   snapshot the hold is the only thing in the way and the message
#   must say busy.
#
# After the gates, for each form, come the settle cases: what the
# order of the hand-back looks like from outside at the done gate and
# after it, what a working directory left inside the private mount
# does to done and to --abort, and what --abort makes of a run
# directory whose result was destroyed under it. They are lettered
# (a) to (f) below and in tests/MATRIX.md, ZX213 to ZX217 and
# ZX219.
#
# The conflicted fixture reaches applying2 and done only through an
# answered resolution. The tool writes the skeleton itself when it
# writes the manifest, so the harness answers it: every "-" becomes
# keep, which leaves the conflicted names as they stand, and the
# header's count of what is unanswered goes to zero with them. An
# unanswered skeleton stops at conflicts, which is what every gate
# before applying2 relies on.
set -u
cd "$(dirname "$0")/../.." || exit 2
. tests/box/progress.sh
bin=./zfs_rebase
[ -x "$bin" ] || { echo "build first: make freebsd"; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 2; }
[ "$(uname)" = FreeBSD ] || { echo "FreeBSD only"; exit 2; }
# The portable flavor answers every ZFS call with this line; the box
# wants the freebsd flavor, and the Makefile keeps the two apart.
if "$bin" --abort zr-flavor-probe/none 2>&1 |
    grep -q 'not built with ZR_FREEBSD'; then
	echo "$bin is the portable build: make clean && make freebsd"
	exit 2
fi
fixtures=${*:-tests/fixtures/probe.zrt tests/fixtures/h-yw-row19.zrt}

POOL=zrtkill
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
MD=
pid=
cases=0
case_id=setup
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-kill.XXXXXX") || exit 2

cleanup() {
	prog_end
	[ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null
	[ -n "${busy_pid:-}" ] && kill -KILL "$busy_pid" 2>/dev/null
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	"$bin" --abort "$POOL/result" >/dev/null 2>&1
	"$bin" --abort "$POOL/onto" >/dev/null 2>&1
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	rm -f "$IMG"
	chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null
	rm -rf "$tmp"
	rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT
say() { printf '\n== %s\n' "$*"; prog_note "$*"; }
fail() { echo "FAIL: $case_id: $*"; exit 1; }
recval() { zfs get -H -o value "$1" "$2" 2>/dev/null; }
# The phase: decided, applying1, conflicts or applying2, with "" for a
# record that has passed no gate yet -- born and not decided, before
# the decision manifest was renamed into place -- and for a result
# with no record at all, which is what done leaves.
phasenow() {
	v=$(zfs get -H -o value zfs_rebase:phase "$1" 2>/dev/null)
	[ "$v" = - ] && v=""
	printf '%s' "$v"
}
# One line of a manifest's header: the rebase's identity lives there
# now, and the harness reads what it needs from the file the record
# names rather than from a property.
hdr() { sed -n "s/^#$1 //p" "$2"; }
# Every snapshot of the pool that is held, one line each.
heldsnaps() {
	for hs in $(allsnaps); do
		[ -n "$(zfs holds -H "$hs" | cut -f2)" ] && printf '%s\n' "$hs"
	done
	return 0
}
# Every zfs_rebase: property that is this dataset's own. A record is
# read as local values only, so an inherited one is no record.
localprops() {
	zfs get -H -o property,source all "$1" 2>/dev/null | \
	    awk '$1 ~ /^zfs_rebase:/ && $2 == "local" { print $1 }'
}
hassnap() { zfs list -H -o name -t snapshot "$1" > /dev/null 2>&1; }
allsnaps() { zfs list -H -o name -t snapshot -r "$POOL"; }
# Every hold on every snapshot of the pool. A rebase that is open
# holds three, and one that is not holds none: no other number is a
# state this tool leaves.
holdcount() {
	n=0
	for s in $(allsnaps); do
		c=$(zfs holds -H "$s" | grep -c .)
		n=$((n + c))
	done
	printf '%s' "$n"
}
holdtags() { zfs holds -H "$1" | cut -f2; }
mounted_at() { mount | grep -q " on $1 "; }
# Where a dataset is mounted just now, or nothing where it is
# mounted nowhere: "DATASET on PATH (zfs, ...)" is the mount(8) line.
mountpt() { mount | awk -v d="$1" '$1 == d { print $3 }'; }
# A clone whose rebase reached done is unmounted with its mountpoint
# property still none -- the void the tool hands it to -- so reading
# its tree means placing it first, which is exactly what the tool's
# own last line says to do. reset_pool destroys it again.
place_clone() {
	mp=$(recval mountpoint "$POOL/result")
	[ "$mp" = none ] || fail "a settled clone's mountpoint is $mp, want none"
	if ! mounted_at "$MNT/result"; then
		[ "$(recval mounted "$POOL/result")" = no ] || \
		    fail "a settled clone is mounted somewhere else"
		zfs set mountpoint="$MNT/result" "$POOL/result" || \
		    fail "cannot place the settled clone"
		mounted_at "$MNT/result" || \
		    fail "placing the clone did not mount it at $MNT/result"
	fi
	return 0
}
# Where the result is, as an assertion. "priv" is the run's own mount
# point, which is where both forms hold it for the whole of an open
# rebase; "home" is where a dataset goes at done and at --abort; and
# "void" is where a clone goes at done, which is nowhere at all --
# unmounted, with the mountpoint property still none, for the user to
# place. A clone has no home and a dataset never sees the void.
where_is() {			# priv | home | void
	if [ "$1" = void ]; then
		mp=$(recval mountpoint "$POOL/result")
		[ "$mp" = none ] || \
		    fail "a settled clone's mountpoint is $mp, want none"
		[ "$(recval mounted "$POOL/result")" = no ] || \
		    fail "a settled clone is still mounted"
		return 0
	fi
	if [ "$1" = priv ]; then
		mounted_at "$rundir/mnt" || \
		    fail "$rds is not at the private mount $rundir/mnt"
		[ "$form" = clone ] || \
		    [ "$(recval canmount "$POOL/onto")" = noauto ] || \
		    fail "canmount is $(recval canmount "$POOL/onto") at the private mount, want noauto"
		[ "$form" = dataset ] || \
		    [ "$(recval mountpoint "$POOL/result")" = none ] || \
		    fail "the clone's mountpoint is $(recval mountpoint "$POOL/result"), want none"
		return 0
	fi
	mounted_at "$MNT/onto" || fail "$rds is not at home $MNT/onto"
	[ "$(recval canmount "$POOL/onto")" = on ] || \
	    fail "canmount is $(recval canmount "$POOL/onto") at home, want on"
	return 0
}
# Rebase the fixture's from onto the result again, over three plain
# directories: stage 1 is idempotent, so this must have nothing left
# to do and must name the same conflicts.
again() {
	"$bin" --posix $flag -o "$1" "$fdir/base" "$fdir/from" "$2" \
	    > /dev/null 2>&1
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || fail "the --posix re-run exited $st"
	grep -q '^#actions 0$' "$1" || \
	    { sed -n '1,20p' "$1"; fail "rebasing onto the result declares actions"; }
	grep -q "^#conflicts $want_conf\$" "$1" || \
	    { grep '^#conflicts' "$1"; fail "rebasing onto the result wants $want_conf conflicts"; }
}

# Wait for the tool to stop itself at its gate. Returns 1 if it
# exited instead of stopping, which means the gate was never reached.
procstat() { ps -o stat= -p "$1" 2>/dev/null | tr -d ' \t' | cut -c1; }
wait_stop() {
	i=0
	while [ $i -lt 300 ]; do
		st=$(procstat "$1")
		case "$st" in
		T) return 0 ;;
		Z|"") return 1 ;;	# it exited instead of stopping
		esac
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

# The pool: base with the fixture's base tree, from and onto cleared
# clones of it with the fixture's own trees copied in with tar, and a
# snapshot of each side. run-fixture.sh builds its trees in place
# with the fixture builder since 2026-09-08; this harness runs
# fixtures with neither a socket nor a flag, which is where tar is
# still sound, and keeps the copy.
make_pool() {
	truncate -s 512m "$IMG" || exit 2
	MD=$(mdconfig -a -t vnode -f "$IMG") || exit 2
	mkdir -p "$MNT"
	zpool create -m "$MNT" -O casesensitivity=sensitive \
	    -O normalization=none "$POOL" "/dev/$MD" || exit 2
	zfs create "$POOL/base" || exit 2
	(cd "$fdir/base" && tar -cf - .) | (cd "$MNT/base" && tar -xpf -) || \
	    exit 2
	zfs snapshot "$POOL/base@base" || exit 2
	for side in from onto; do
		zfs clone "$POOL/base@base" "$POOL/$side" || exit 2
		(cd "$MNT/$side" && \
		    find . -mindepth 1 -maxdepth 1 -exec rm -rf {} +) || exit 2
		(cd "$fdir/$side" && tar -cf - .) | \
		    (cd "$MNT/$side" && tar -xpf -) || exit 2
	done
	zfs snapshot "$POOL/from@work" "$POOL/onto@work" || exit 2
}

drop_pool() {
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	MD=
	rm -f "$IMG"
	rmdir "$MNT" 2>/dev/null
}

# Between two cases: take away whatever the last one left, and prove
# the pool is back to the fixture with no rebase anywhere in it.
#
# A rebase that reached done took its record off, so --abort finds
# nothing to undo and says so: what it left is the result itself and
# the pre-apply snapshot, and the reset takes those away by hand. The
# run directory is not among them -- done took it -- so the reset
# asserts it is gone instead of removing it.
reset_pool() {
	"$bin" --abort "$POOL/result" >/dev/null 2>&1
	"$bin" --abort "$POOL/onto" >/dev/null 2>&1
	if zfs list -H -o name "$POOL/result" >/dev/null 2>&1; then
		zfs destroy "$POOL/result" || \
		    fail "the reset cannot destroy the settled $POOL/result"
		rmdir "$MNT/result" 2>/dev/null
	fi
	if hassnap "$POOL/onto@pre"; then
		zfs rollback -r "$POOL/onto@pre" || \
		    fail "the reset cannot roll onto back to @pre"
		zfs destroy "$POOL/onto@pre" || \
		    fail "the reset cannot destroy @pre"
	fi
	for d in result onto; do
		[ ! -d "/var/db/zfs_rebase/$POOL/$d" ] || \
		    fail "a rebase that ended left /var/db/zfs_rebase/$POOL/$d"
	done
	[ "$(holdcount)" = 0 ] || fail "the reset left holds behind"
	zfs list -H -o name "$POOL/result" >/dev/null 2>&1 && \
	    fail "the reset left $POOL/result behind"
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || fail "the reset left $left on $POOL/onto"
	[ -e "/var/db/zfs_rebase/$POOL" ] && \
	    fail "the reset left /var/db/zfs_rebase/$POOL behind"
	n=$(allsnaps | grep -c .)
	[ "$n" -eq 3 ] || \
	    { allsnaps; fail "the pool has $n snapshots, want 3"; }
	mounted_at "$MNT/onto" || fail "the reset left onto unmounted"
	[ "$(recval readonly "$POOL/onto")" = off ] || \
	    fail "the reset left onto read-only"
	[ "$(recval canmount "$POOL/onto")" = on ] || \
	    fail "the reset left onto at canmount noauto"
	return 0
}

# The skeleton the run wrote, answered the way a person would answer
# it: one field per line changed, "-" to keep, and the header's count
# of what is unanswered changed with them. Lines are never added and
# never removed. It must have been the tool's own unanswered document
# before this touched it.
answer_resolution() {
	[ -f "$1" ] || fail "the run wrote no resolution at $1"
	grep -q '^#rebase-resolution 5$' "$1" || \
	    { head -3 "$1"; fail "$1 is no resolution"; }
	left=$(sed -n 's/^#unanswered //p' "$1")
	[ "${left:-0}" -gt 0 ] || \
	    { head -8 "$1"; fail "$1 was already answered"; }
	sed -e 's/ -$/ keep/' -e 's/^#unanswered .*$/#unanswered 0/' \
	    "$1" > "$1.answered" || fail "cannot answer $1"
	mv "$1.answered" "$1" || fail "cannot answer $1"
}

# ---------------------------------------------------------------
# One case: one form, one gate, one signal.
# ---------------------------------------------------------------
kill_case() {
	form=$1
	gate=$2
	sig=$3
	case_id="$form $gate $sig"
	cases=$((cases + 1))

	if [ "$form" = clone ]; then
		rds=$POOL/result
	else
		rds=$POOL/onto
	fi
	# How this harness names the rebase to a verb: the last part
	# of the result's name alone, which is step 2 of the
	# identifier -- a dataset carrying the record whose name ends
	# in "/result" -- found by the walk of every imported pool
	# (ZX227). The other harnesses give the whole name, the
	# pre-apply snapshot and a manifest's path.
	ident=${rds##*/}
	rundir=/var/db/zfs_rebase/$rds
	man=$rundir/manifest
	res=$rundir/resolution
	log=$tmp/case.log

	# What this gate and this signal must leave. wman is the
	# manifest, which the run writes before the record and so has
	# at every gate; wres is the resolution, written at the
	# decision; and wdecided says whether the decision is in
	# place, which is what the phase says and what --continue
	# needs.
	resumed=no
	wres=yes
	wdecided=yes
	case "$gate" in
	held|cloned|read|decided)
		if [ "$sig" = KILL ]; then
			out=kept; wexit=137; wman=yes
			case "$gate" in
			decided) wstate=decided ;;
			*) wstate=""; wres=no; wdecided=no ;;
			esac
			# The dataset is not the run's own until the
			# clone gate: at held it is still at home with
			# the readonly it had.
			if [ "$gate" = held ] && [ "$form" = dataset ]; then
				wro=off; wmnt=home
			else
				wro=on; wmnt=priv
			fi
		else
			out=torn; wexit=3; wstate=""; wman=no; wres=no
			wdecided=no; wro=off; wmnt=home
		fi ;;
	applying1|action:*)
		# A kept rebase holds the result at the private mount
		# whatever the signal was: only done and --abort hand
		# it back, and a caught signal at a gate is neither.
		out=kept; wstate=applying1; wman=yes; wmnt=priv
		if [ "$sig" = KILL ]; then
			wexit=137; wro=off
		else
			wexit=3; wro=on
		fi ;;
	conflicts)
		out=kept; wstate=conflicts; wman=yes; wro=on; wmnt=priv
		if [ "$sig" = KILL ]; then
			wexit=137
		else
			wexit=1
		fi ;;
	applying2)
		out=kept; wstate=applying2; wman=yes; resumed=yes
		wmnt=priv
		if [ "$sig" = KILL ]; then
			wexit=137; wro=off
		else
			wexit=3; wro=on
		fi ;;
	done)
		# done is no phase: a SIGKILL at that gate stops before
		# the release and the clearing, so what it leaves is
		# the phase of the stage that ran before it, and a
		# caught signal lets the run finish, which leaves no
		# record at all.
		if [ $clean -eq 1 ]; then
			wstate=applying1
		else
			wstate=applying2
			resumed=yes
		fi
		wman=yes; wro=on
		if [ "$sig" = KILL ]; then
			out=kept; wexit=137; wmnt=priv
		else
			out=finished; wstate=""; wexit=0; wmnt=home
		fi ;;
	*)
		fail "no such gate" ;;
	esac
	# wro so far is the working value: on outside an applying
	# stage and off inside one. That is the clone's own. The
	# dataset wears what the record says it had -- what the fixture
	# built it with -- at the private mount and at home alike: the
	# tool touches its readonly only while it is off its mountpoint.
	if [ "$form" = dataset ]; then
		wro=off
	fi
	# And the clone form has no home to be handed back to: what
	# done leaves it at is the void.
	if [ "$form" = clone ] && [ $wmnt = home ]; then
		wmnt=void
	fi

	# A gate past conflicts is reached by a --continue over an
	# answered resolution, since a fresh run stops at conflicts
	# with its skeleton unanswered.
	if [ $resumed = yes ]; then
		if [ "$form" = clone ]; then
			"$bin" $flag --off-of "$POOL/from@work" \
			    --onto "$POOL/onto@work" --result "$POOL/result" \
			    > "$log" 2>&1
		else
			"$bin" $flag --from "$POOL/from" --onto "$POOL/onto" \
			    --result pre > "$log" 2>&1
		fi
		st=$?
		[ $st -eq 1 ] || { cat "$log"; fail "the run before the resolution exited $st, want 1"; }
		[ "$(phasenow "$rds")" = conflicts ] || \
		    fail "that run did not stop at conflicts"
		answer_resolution "$res"
		ZFS_REBASE_PAUSE=$gate "$bin" --continue "$ident" \
		    > "$log" 2>&1 &
		pid=$!
	elif [ "$form" = clone ]; then
		ZFS_REBASE_PAUSE=$gate "$bin" $flag -v \
		    --off-of "$POOL/from@work" --onto "$POOL/onto@work" \
		    --result "$POOL/result" > "$log" 2>&1 &
		pid=$!
	else
		ZFS_REBASE_PAUSE=$gate "$bin" $flag -v --from "$POOL/from" \
		    --onto "$POOL/onto" --result pre > "$log" 2>&1 &
		pid=$!
	fi

	wait_stop "$pid" || { cat "$log"; fail "never stopped at the gate"; }

	# The holds are what makes a stopped rebase safe: while they
	# are there its inputs cannot be destroyed. Checked at the
	# first gate that has them, which is where they were taken.
	if [ "$gate" = held ]; then
		# The record names the manifest and the tag, and the
		# manifest at this gate is the header the run was born
		# with, which says nothing about what is held: that is
		# read off the pool instead, which is the only
		# question here anyway.
		tag=$(recval zfs_rebase:tag "$rds")
		[ "$(holdcount)" = 3 ] || \
		    fail "$(holdcount) holds at the held gate, want 3"
		for snap in $(heldsnaps); do
			[ "$(holdtags "$snap")" = "$tag" ] || \
			    fail "$snap is not held under $tag"
			if zfs destroy "$snap" > "$tmp/destroy" 2>&1; then
				fail "zfs destroy $snap succeeded while held"
			fi
			hassnap "$snap" || \
			    fail "$snap went away although the destroy failed"
			# Where nothing is cloned from it the hold is
			# the only thing in the way and libzfs says so:
			# "it's being held" (libzfs_dataset.c, on EBUSY
			# with holds), "dataset is busy" in older words.
			# base always has the two sides cloned from it,
			# and onto's snapshot has the result in the clone
			# form, and there ZFS's own rule about a clone's
			# origin answers first.
			cl=$(zfs get -H -o value clones "$snap")
			if [ "$cl" = "-" ] || [ -z "$cl" ]; then
				grep -qi 'busy\|being held' "$tmp/destroy" || \
				    { cat "$tmp/destroy"; fail "the destroy of the held $snap did not name the hold"; }
			fi
		done
		echo "ok   the three held snapshots refuse zfs destroy"
	fi

	kill -"$sig" "$pid" || fail "cannot signal the stopped tool"
	[ "$sig" = KILL ] || kill -CONT "$pid"
	wait "$pid"
	st=$?
	pid=
	[ "$st" -eq "$wexit" ] || { cat "$log"; fail "exited $st, want $wexit"; }

	# --- what the stop left ---
	if [ $out = torn ]; then
		[ "$(holdcount)" = 0 ] || fail "a torn run left holds"
		if [ "$form" = clone ]; then
			zfs list -H -o name "$POOL/result" >/dev/null 2>&1 && \
			    fail "a torn run left $POOL/result"
		else
			left=$(localprops "$POOL/onto")
			[ -z "$left" ] || fail "a torn run left $left on onto"
			hassnap "$POOL/onto@pre" && \
			    fail "a torn run left the pre-apply snapshot"
			where_is home
			[ "$(recval readonly "$POOL/onto")" = off ] || \
			    fail "a torn run left onto read-only"
		fi
		n=$(zfs list -H -o name -t snapshot -r "$POOL/from" | grep -c .)
		[ "$n" -eq 1 ] || fail "a torn run left a snapshot on from"
		[ -e "$rundir" ] && fail "a torn run left $rundir"
		"$bin" --continue "$ident" > "$tmp/cont" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/cont"; fail "--continue after a torn run exited $st, want 2"; }
		echo "ok   $case_id: torn down whole, nothing to continue"
		reset_pool
		return 0
	fi

	# A rebase is still there: the record, the phase it reached,
	# the three holds unless it reached done, and the manifest,
	# which the run wrote before the record.
	[ "$(phasenow "$rds")" = "$wstate" ] || \
	    fail "the phase is '$(phasenow "$rds")', want '$wstate'"
	if [ $out = finished ]; then
		whold_now=0
		[ "$(holdcount)" = 0 ] || \
		    fail "a run that reached done kept its holds"
		[ -z "$(localprops "$rds")" ] || \
		    fail "a run that reached done left $(localprops "$rds")"
	else
		whold_now=3
		[ "$(holdcount)" = 3 ] || \
		    fail "$(holdcount) holds in the pool, want 3"
		tag=$(recval zfs_rebase:tag "$rds")
		for snap in $(heldsnaps); do
			[ "$(holdtags "$snap")" = "$tag" ] || \
			    fail "$snap is not held under $tag after the stop"
		done
	fi
	[ "$(recval readonly "$rds")" = "$wro" ] || \
	    fail "readonly is $(recval readonly "$rds"), want $wro"
	if [ $out = finished ]; then
		# This harness gives no -o, so the two documents are
		# the run's own, in its own directory: done unlinks
		# them and takes the directory with them. What a rebase
		# that finished leaves is the result and nothing else.
		[ -e "$man" ] && fail "done left the manifest at $man"
		[ -e "$res" ] && fail "done left the resolution at $res"
		[ ! -d "$rundir" ] || fail "done left the run directory $rundir"
	elif [ $wman = yes ]; then
		[ -f "$man" ] || fail "no manifest at $man"
		if [ $wres = yes ]; then
			[ -f "$res" ] || fail "no resolution at $res"
		else
			# Before the decision the manifest is the header
			# the run was born with: everything that says
			# what this rebase is, and nothing to apply. The
			# skeleton is written at the decision, so there
			# is none beside it yet.
			[ -e "$res" ] && \
			    fail "a resolution at $res before the decision"
			grep -q '^#actions 0$' "$man" || \
			    { sed -n '1,20p' "$man"; fail "the birth manifest declares actions"; }
			grep -q '^#conflicts 0$' "$man" || \
			    { sed -n '1,20p' "$man"; fail "the birth manifest declares conflicts"; }
			[ "$(hdr tag "$man")" = "$(recval zfs_rebase:tag "$rds")" ] || \
			    fail "the birth manifest's #tag is not the record's"
		fi
	else
		[ -e "$man" ] && fail "a manifest at $man before the decision"
		[ -e "$res" ] && fail "a resolution at $res before the decision"
	fi
	# Every document is written to a sibling and renamed over its
	# destination, so a gate is never a moment at which one is
	# half written and never leaves a .tmp behind.
	[ -e "$man.tmp" ] && fail "a .tmp left beside the manifest $man"
	[ -e "$res.tmp" ] && fail "a .tmp left beside the resolution $res"
	# Where the stop left the result, which is the private mount in
	# both forms unless the run reached done or never took the
	# dataset over at all.
	where_is $wmnt
	if [ $out = finished ]; then
		# It reached done, so there is no rebase left: every
		# verb says so and the tree is the rebased tree.
		for verb in --verify --continue --abort; do
			"$bin" $verb "$ident" > "$tmp/cont" 2>&1
			st=$?
			[ $st -eq 2 ] || \
			    { cat "$tmp/cont"; fail "$verb on a settled result exited $st, want 2"; }
		done
		if [ "$form" = dataset ]; then
			cmnt=$MNT/onto
		else
			place_clone
			cmnt=$MNT/result
		fi
		again "$tmp/again" "$cmnt"
		echo "ok   $case_id: finished, the record off, the holds released"
		reset_pool
		return 0
	fi

	# --- and then --continue ---
	if [ $wdecided = no ]; then
		# Killed before the decision: the record, the holds and
		# the birth manifest are there, and that manifest is
		# the header alone. There is nothing to apply, so the
		# two verbs that would apply it refuse and say so, and
		# --abort is the way out.
		"$bin" --continue "$ident" > "$tmp/cont" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/cont"; fail "--continue before the decision exited $st, want 2"; }
		grep -q 'never reached its decision' "$tmp/cont" || \
		    { cat "$tmp/cont"; fail "--continue did not say the run never decided"; }
		grep -q -- '--abort' "$tmp/cont" || \
		    { cat "$tmp/cont"; fail "--continue did not point at --abort"; }
		"$bin" --restart "$ident" > "$tmp/rest" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/rest"; fail "--restart before the decision exited $st, want 2"; }
		grep -q 'never reached its decision' "$tmp/rest" || \
		    { cat "$tmp/rest"; fail "--restart did not say the run never decided"; }
		[ "$(phasenow "$rds")" = "$wstate" ] || \
		    fail "a refused verb moved the phase"
		[ "$(holdcount)" = 3 ] || \
		    fail "a refused verb changed the holds"
		# They were refused before they took the result over,
		# so the result is exactly where the kill left it.
		where_is $wmnt
		# And --abort, which the birth manifest gives every
		# fact it needs: the form, the pre-apply snapshot, the
		# two properties to put back and the snapshot the run
		# took of from, which only the dataset pass gives as a
		# dataset. Nothing is put back by hand.
		made=$POOL/from@zfs_rebase-$tag
		if [ "$form" = dataset ]; then
			hassnap "$made" || fail "the kill left no $made"
		fi
		"$bin" --abort "$ident" > "$tmp/abort" 2>&1
		st=$?
		[ $st -eq 0 ] || \
		    { cat "$tmp/abort"; fail "--abort before the decision exited $st, want 0"; }
		[ "$(holdcount)" = 0 ] || \
		    { cat "$tmp/abort"; fail "--abort left $(holdcount) holds"; }
		[ -e "$rundir" ] && \
		    { cat "$tmp/abort"; fail "--abort left the run directory $rundir"; }
		if [ "$form" = dataset ]; then
			hassnap "$made" && \
			    { cat "$tmp/abort"; fail "--abort left the run's own $made"; }
			hassnap "$POOL/onto@pre" && \
			    { cat "$tmp/abort"; fail "--abort left the pre-apply snapshot"; }
			left=$(localprops "$POOL/onto")
			[ -z "$left" ] || \
			    { cat "$tmp/abort"; fail "--abort left $left on onto"; }
			# The header kept both properties as the take
			# found them, so the abort puts both back and
			# the dataset goes home.
			where_is home
			[ "$(recval readonly "$POOL/onto")" = off ] || \
			    { cat "$tmp/abort"; fail "--abort left onto read-only"; }
			[ "$(recval canmount "$POOL/onto")" = on ] || \
			    { cat "$tmp/abort"; fail "--abort left canmount at $(recval canmount "$POOL/onto")"; }
			echo "ok   $case_id: born, not decided; --continue and --restart refused, --abort rolled onto back and put canmount and readonly back"
		else
			zfs list -H -o name "$POOL/result" >/dev/null 2>&1 && \
			    { cat "$tmp/abort"; fail "--abort left the clone $POOL/result"; }
			echo "ok   $case_id: born, not decided; --continue and --restart refused, --abort destroyed the clone"
		fi
		reset_pool
		return 0
	fi

	# What --verify says of what the kill left, before anything is
	# repaired: an action is pending until the stage that makes it
	# has run, so a rebase stopped before or inside applying1 has
	# pending actions and exits 3, and one past it has none. It
	# writes nothing and moves no gate either way.
	case "$wstate" in
	decided|applying1) wrep=3 ;;
	*) wrep=0 ;;
	esac
	# Except at the done gate itself, where every action has been
	# made and the phase is only the stage that made them.
	[ "$gate" = done ] && wrep=0
	# A caught signal at action:2 comes in at the pause before that
	# action, which is then performed, and the apply stops at the
	# next one: a manifest of exactly two actions has none, so the
	# apply finishes, the self-check passes, and the run still
	# leaves the gate because it was told to stop. Nothing is
	# pending then, and --continue walks through. A SIGKILL there
	# never performs the action it was stopped before.
	if [ "$gate" = action:2 ] && [ "$sig" != KILL ] && [ "$nact" -eq 2 ]; then
		wrep=0
	fi
	#
	# A kill inside a stage leaves the result where the stage had
	# it -- at the private mount, and in the clone form with
	# readonly off, which is what the stage flipped it to. The
	# report reads it there and leaves both alone: it reads in
	# place and sets readonly on nothing (ZX237, ZX238;
	# documents-design.md, section 11.6), where it used to take
	# the result over as a --continue does and flip readonly back
	# on, which was a property write by a verb that writes
	# nothing.
	vro=$(recval readonly "$rds")
	vat=$(mountpt "$rds")
	"$bin" --verify "$ident" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq $wrep ] || \
	    { cat "$tmp/verify"; fail "--verify after the stop exited $st, want $wrep"; }
	grep -q 'drifted 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found drift after a kill"; }
	[ "$(phasenow "$rds")" = "$wstate" ] || \
	    fail "--verify moved the phase"
	[ "$(holdcount)" = "$whold_now" ] || \
	    fail "--verify changed the holds"
	vat2=$(mountpt "$rds")
	[ "$vat2" = "$vat" ] || \
	    fail "--verify moved $rds from ${vat:-nowhere} to ${vat2:-nowhere}"
	vro2=$(recval readonly "$rds")
	[ "$vro2" = "$vro" ] || \
	    fail "--verify left readonly $vro2, want $vro"

	# Where the --continue lands: at done when there is nothing to
	# answer or the answers are in, and back at conflicts while the
	# skeleton the run wrote is still unanswered. The file being
	# there is not the signal; its being complete is. done leaves
	# no record, so what it lands in is an empty property list.
	if [ $clean -eq 1 ] || [ $resumed = yes ]; then
		wcont=0; wend=""; whold=0; wsettled=1
	else
		wcont=1; wend=conflicts; whold=3; wsettled=0
	fi
	# One --continue for every kind of stop, with no flag on it:
	# the checks of the schedule are made wherever it passes a
	# gate, and there is no flag left that would ask for one or
	# skip one.
	"$bin" --continue "$ident" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq $wcont ] || \
	    { cat "$tmp/cont"; fail "--continue exited $st, want $wcont"; }
	[ "$(phasenow "$rds")" = "$wend" ] || \
	    fail "--continue landed at '$(phasenow "$rds")', want ${wend:-done}"
	# A --continue that reaches the done gate makes the final
	# check there and prints its report, and records nothing at
	# all. What it leaves at done is what any done leaves, which
	# is no record.
	if [ $wsettled -eq 1 ]; then
		grep -q 'outside the manifest' "$tmp/cont" || \
		    { cat "$tmp/cont"; fail "--continue printed no final check at done"; }
		[ -z "$(localprops "$rds")" ] || \
		    fail "--continue reached done and left $(localprops "$rds")"
	fi
	[ "$(holdcount)" = $whold ] || \
	    fail "$(holdcount) holds after --continue, want $whold"
	if [ "$form" = clone ]; then
		endro=on
	else
		endro=off		# the record's own, as above
	fi
	[ "$(recval readonly "$rds")" = "$endro" ] || \
	    fail "readonly is $(recval readonly "$rds") after --continue, want $endro"
	# And where the --continue left the result: settled where it
	# reached done -- the dataset home with the properties the
	# fixture built, the clone unmounted for the user to place --
	# and at the private mount where it stopped at conflicts.
	if [ $wsettled -eq 1 ]; then
		if [ "$form" = dataset ]; then
			where_is home
			cmnt=$MNT/onto
		else
			place_clone
			cmnt=$MNT/result
		fi
	else
		where_is priv
		cmnt=$rundir/mnt
	fi
	again "$tmp/again" "$cmnt"
	echo "ok   $case_id: at ${wstate:-no gate}, readonly $wro; --continue -> ${wend:-done} (exit $st), stage 1 idempotent"
	reset_pool
}

# ---------------------------------------------------------------
# The settle cases: the order a rebase is given back in, and what a
# mount somebody is standing in does to it. They are lettered (a) to
# (f) here and in tests/MATRIX.md, ZX213 to ZX217 and ZX219.
#
# The order (documents-design.md, section 11.3) is: the final check,
# the walks closed, the result put back into service, the holds
# released, the record taken off, the tool's own snapshots destroyed,
# the run directory removed. None of that can be watched from
# outside a running process, so what these cases assert is its two
# ends and its one refusal.
#
# The pause hook stops the tool at the done gate after the check has
# been made and printed and before anything has been released, which
# is exactly the moment the order turns on: what is asserted there is
# that the record, the holds and the private mount are all still
# where they were.
#
# The refusal is the ordinary one. An unmount fails with EBUSY when
# anything holds the mount -- an open file, a working directory --
# and at the conflicts gate a person is expected to be working
# inside the private mount, so a shell left there is the common case
# and not an exotic one. Nothing is ever forced, so the tool has to
# stop; what it must not do is stop after it has taken the record
# off, which would leave a dataset off its mountpoint that nothing
# names (R3 of the code review).
# ---------------------------------------------------------------

# The names one form's rebase wears, as kill_case sets them for
# itself. Kept apart from kill_case so that this section can be read
# and moved on its own.
names_for_form() {
	if [ "$form" = clone ]; then
		rds=$POOL/result
	else
		rds=$POOL/onto
	fi
	rundir=/var/db/zfs_rebase/$rds
	man=$rundir/manifest
	res=$rundir/resolution
	log=$tmp/case.log
}

# A background shell whose working directory is inside the private
# mount. It creates nothing there -- a file would be drift, and the
# final check would say so -- and it says it has arrived by touching
# a file outside the mount, so that the tool is never signalled
# before the mount is really busy.
busy_pid=
hold_mount() {
	rm -f "$tmp/busy.ready"
	sh -c 'cd "$1" || exit 2; : > "$2"; exec sleep 300' sh "$1" \
	    "$tmp/busy.ready" &
	busy_pid=$!
	i=0
	while [ $i -lt 200 ]; do
		[ -f "$tmp/busy.ready" ] && return 0
		sleep 0.1
		i=$((i + 1))
	done
	fail "the background shell never took a working directory in $1"
}

free_mount() {
	if [ -n "$busy_pid" ]; then
		kill -KILL "$busy_pid" 2>/dev/null
		wait "$busy_pid" 2>/dev/null
		busy_pid=
	fi
	rm -f "$tmp/busy.ready"
	return 0
}

# A run taken to the pause at the done gate, by the two ways
# kill_case takes one there: straight through for a clean fixture,
# and through an answered resolution for a conflicted one. The tool
# is stopped at the gate when this returns, with its report already
# printed and nothing released.
run_to_done() {
	if [ $clean -eq 1 ]; then
		if [ "$form" = clone ]; then
			ZFS_REBASE_PAUSE=done "$bin" $flag -v \
			    --off-of "$POOL/from@work" \
			    --onto "$POOL/onto@work" \
			    --result "$POOL/result" > "$log" 2>&1 &
		else
			ZFS_REBASE_PAUSE=done "$bin" $flag -v \
			    --from "$POOL/from" --onto "$POOL/onto" \
			    --result pre > "$log" 2>&1 &
		fi
		pid=$!
	else
		if [ "$form" = clone ]; then
			"$bin" $flag --off-of "$POOL/from@work" \
			    --onto "$POOL/onto@work" \
			    --result "$POOL/result" > "$log" 2>&1
		else
			"$bin" $flag --from "$POOL/from" --onto "$POOL/onto" \
			    --result pre > "$log" 2>&1
		fi
		st=$?
		[ $st -eq 1 ] || \
		    { cat "$log"; fail "the run before the resolution exited $st, want 1"; }
		answer_resolution "$res"
		ZFS_REBASE_PAUSE=done "$bin" --continue "$rds" \
		    > "$log" 2>&1 &
		pid=$!
	fi
	wait_stop "$pid" || { cat "$log"; fail "never stopped at the done gate"; }
}

# A rebase left standing at a gate, which is what --abort is for. A
# conflicted fixture stops at its own conflicts gate; a clean one is
# stopped inside applying1 with the pause hook and a SIGKILL, which
# leaves the same thing: the record with a phase, the three holds,
# both documents and the result at the private mount.
leave_rebase_standing() {
	if [ $clean -eq 0 ]; then
		if [ "$form" = clone ]; then
			"$bin" $flag --off-of "$POOL/from@work" \
			    --onto "$POOL/onto@work" \
			    --result "$POOL/result" > "$log" 2>&1
		else
			"$bin" $flag --from "$POOL/from" --onto "$POOL/onto" \
			    --result pre > "$log" 2>&1
		fi
		st=$?
		[ $st -eq 1 ] || \
		    { cat "$log"; fail "the run for this case exited $st, want 1"; }
		[ "$(phasenow "$rds")" = conflicts ] || \
		    { cat "$log"; fail "that run did not stop at conflicts"; }
	else
		if [ "$form" = clone ]; then
			ZFS_REBASE_PAUSE=applying1 "$bin" $flag \
			    --off-of "$POOL/from@work" \
			    --onto "$POOL/onto@work" \
			    --result "$POOL/result" > "$log" 2>&1 &
		else
			ZFS_REBASE_PAUSE=applying1 "$bin" $flag \
			    --from "$POOL/from" --onto "$POOL/onto" \
			    --result pre > "$log" 2>&1 &
		fi
		pid=$!
		wait_stop "$pid" || \
		    { cat "$log"; fail "never stopped at applying1"; }
		kill -KILL "$pid" || fail "cannot kill the stopped tool"
		wait "$pid"
		pid=
		[ "$(phasenow "$rds")" = applying1 ] || \
		    fail "the kill did not leave the rebase at applying1"
	fi
	[ "$(holdcount)" = 3 ] || \
	    fail "$(holdcount) holds on the standing rebase, want 3"
	[ -f "$man" ] || fail "the standing rebase has no manifest at $man"
	where_is priv
	return 0
}

# (a) done with the private mount busy. The unmount refuses, and
# nothing the settle does after it happens: the record, the holds and
# the run directory are where they were and the result is still at
# the private mount. The exit is 3, which is what the drift verdict
# is too, and the report tells them apart: drift says the rebase is
# over, and this says it is not. Then the mount is freed and a plain
# --continue settles it. ZX214.
settle_busy_done() {
	case_id="$fixture $form done with the private mount busy"
	cases=$((cases + 1))
	names_for_form
	run_to_done
	hold_mount "$rundir/mnt"
	kill -CONT "$pid" || fail "cannot continue the stopped tool"
	wait "$pid"
	st=$?
	pid=
	[ $st -eq 3 ] || \
	    { cat "$log"; fail "done with the mount busy exited $st, want 3"; }
	grep -q "$rundir/mnt" "$log" || \
	    { cat "$log"; fail "the message does not name the mount path"; }
	grep -q -- '--continue or --abort' "$log" || \
	    { cat "$log"; fail "the message does not say what finishes the settle"; }
	[ -n "$(localprops "$rds")" ] || \
	    { cat "$log"; fail "the settle took the record off although it could not give the result back"; }
	[ "$(holdcount)" = 3 ] || \
	    { cat "$log"; fail "$(holdcount) holds after the refused settle, want 3"; }
	[ -d "$rundir" ] || \
	    { cat "$log"; fail "the refused settle removed the run directory $rundir"; }
	[ -f "$man" ] || \
	    { cat "$log"; fail "the refused settle removed the manifest $man"; }
	where_is priv
	free_mount
	"$bin" --continue "$rds" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/cont"; fail "the --continue after the refused settle exited $st, want 0"; }
	[ -z "$(localprops "$rds")" ] || \
	    { cat "$tmp/cont"; fail "that --continue reached done and left a record"; }
	[ "$(holdcount)" = 0 ] || \
	    { cat "$tmp/cont"; fail "that --continue reached done and left holds"; }
	[ ! -e "$rundir" ] || \
	    { cat "$tmp/cont"; fail "that --continue reached done and left $rundir"; }
	if [ "$form" = dataset ]; then
		where_is home
	else
		where_is void
	fi
	echo "ok   $case_id: exit 3 with the rebase kept whole, and the"
	echo "     next --continue settled it"
	reset_pool
}

# (b) --abort with the private mount busy, which stops in the same
# place for the same reason: in the dataset form the rollback is
# made and the hand-back refuses, and in the clone form the private
# mount is undone before the destroy so that a busy mount refuses as
# an unmount and not as a destroy. Either way the record and the
# holds stay and a second --abort finishes it. ZX215.
settle_busy_abort() {
	case_id="$fixture $form --abort with the private mount busy"
	cases=$((cases + 1))
	names_for_form
	leave_rebase_standing
	hold_mount "$rundir/mnt"
	"$bin" --abort "$rds" > "$tmp/abort" 2>&1
	st=$?
	[ $st -ne 0 ] || \
	    { cat "$tmp/abort"; fail "--abort with the mount busy exited 0"; }
	grep -q "$rundir/mnt" "$tmp/abort" || \
	    { cat "$tmp/abort"; fail "--abort did not name the mount path"; }
	[ "$(holdcount)" = 3 ] || \
	    { cat "$tmp/abort"; fail "--abort released holds although it could not give the result back"; }
	[ -n "$(localprops "$rds")" ] || \
	    { cat "$tmp/abort"; fail "--abort took the record off although it could not give the result back"; }
	[ -d "$rundir" ] || \
	    { cat "$tmp/abort"; fail "--abort removed the run directory it could not empty"; }
	where_is priv
	free_mount
	"$bin" --abort "$rds" > "$tmp/abort2" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/abort2"; fail "the second --abort exited $st, want 0"; }
	[ "$(holdcount)" = 0 ] || \
	    { cat "$tmp/abort2"; fail "the second --abort left $(holdcount) holds"; }
	[ ! -e "$rundir" ] || \
	    { cat "$tmp/abort2"; fail "the second --abort left $rundir"; }
	if [ "$form" = dataset ]; then
		hassnap "$POOL/onto@pre" && \
		    { cat "$tmp/abort2"; fail "the second --abort left the pre-apply snapshot"; }
		[ -z "$(localprops "$POOL/onto")" ] || \
		    { cat "$tmp/abort2"; fail "the second --abort left a record on onto"; }
		where_is home
		[ "$(recval readonly "$POOL/onto")" = off ] || \
		    { cat "$tmp/abort2"; fail "the second --abort left onto read-only"; }
		[ "$(recval canmount "$POOL/onto")" = on ] || \
		    { cat "$tmp/abort2"; fail "the second --abort left canmount at $(recval canmount "$POOL/onto")"; }
	else
		zfs list -H -o name "$POOL/result" >/dev/null 2>&1 && \
		    { cat "$tmp/abort2"; fail "the second --abort left the clone"; }
	fi
	echo "ok   $case_id: refused with the rebase kept whole, and the"
	echo "     second --abort took it away"
	reset_pool
}

# (c) the done sequence, at the two points it can be seen from
# outside: at the gate, where the check has been made and printed and
# nothing has been released; and after it, where the result is in
# service and the record, the holds and the directory are gone.
# Between the two lies the order the settle keeps. ZX213.
settle_done_order() {
	case_id="$fixture $form the done sequence"
	cases=$((cases + 1))
	names_for_form
	run_to_done
	# At the gate: the check is made and reported before one thing
	# is given back.
	grep -q 'outside the manifest' "$log" || \
	    { cat "$log"; fail "the final check was not made before the settle"; }
	[ -n "$(localprops "$rds")" ] || \
	    fail "the record was off before the settle"
	[ "$(holdcount)" = 3 ] || \
	    fail "$(holdcount) holds at the done gate, want 3"
	[ -d "$rundir" ] || fail "no run directory at the done gate"
	where_is priv
	kill -CONT "$pid" || fail "cannot continue the stopped tool"
	wait "$pid"
	st=$?
	pid=
	[ $st -eq 0 ] || { cat "$log"; fail "done exited $st, want 0"; }
	# And after it, in the order the settle made them true: the
	# properties and the mount first, and the bookkeeping after.
	if [ "$form" = dataset ]; then
		[ "$(recval canmount "$POOL/onto")" = on ] || \
		    { cat "$log"; fail "done left canmount at $(recval canmount "$POOL/onto")"; }
		[ "$(recval readonly "$POOL/onto")" = off ] || \
		    { cat "$log"; fail "done left onto read-only"; }
		where_is home
		hassnap "$POOL/onto@pre" || \
		    { cat "$log"; fail "done destroyed the pre-apply snapshot, which is the user's"; }
	else
		where_is void
	fi
	[ -z "$(localprops "$rds")" ] || \
	    { cat "$log"; fail "done left $(localprops "$rds")"; }
	[ "$(holdcount)" = 0 ] || \
	    { cat "$log"; fail "done left $(holdcount) holds"; }
	[ ! -e "$rundir" ] || { cat "$log"; fail "done left $rundir"; }
	echo "ok   $case_id: checked and reported at the gate with nothing"
	echo "     released, then in service with the bookkeeping gone"
	reset_pool
}

# (d) and (e): the result destroyed under a standing rebase, which
# is what a --restart whose second clone failed leaves and what a
# zfs destroy by hand leaves. The record went with it, so the tag it
# named is only in the manifest the run directory still holds, and
# before this that manifest was never opened and the holds were
# stranded (R2). Clone form only: the dataset form's result is the
# user's own dataset, and destroying it would take the fixture's onto
# with it.
#
# (e) is the same state with the manifest made unreadable: nothing
# can be released and nothing is removed, and the exit says so.
# ZX216 and ZX217.
settle_result_gone() {
	case_id="$fixture $form the result destroyed under the rebase"
	cases=$((cases + 1))
	names_for_form
	leave_rebase_standing
	tag=$(recval zfs_rebase:tag "$rds")
	cp "$man" "$tmp/gone-man" || fail "cannot keep a copy of $man"
	zfs destroy -f "$rds" || fail "cannot destroy the clone by hand"
	[ "$(holdcount)" = 3 ] || \
	    fail "the destroy of the clone released the holds by itself"
	# (e) first, over the same directory: a manifest that will not
	# parse is the one thing this abort refuses.
	printf 'this is not a manifest\n' > "$man" || fail "cannot spoil $man"
	"$bin" --abort "$rds" > "$tmp/bad" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/bad"; fail "--abort on an unreadable manifest exited $st, want 2"; }
	[ "$(holdcount)" = 3 ] || \
	    { cat "$tmp/bad"; fail "--abort released holds off a manifest it could not read"; }
	[ -d "$rundir" ] || \
	    { cat "$tmp/bad"; fail "--abort removed a run directory it could not read"; }
	[ -f "$man" ] || \
	    { cat "$tmp/bad"; fail "--abort unlinked a manifest it could not read"; }
	echo "ok   $case_id: an unreadable manifest is exit 2, nothing"
	echo "     released and nothing removed"
	cases=$((cases + 1))
	# (d) with the header back: the tag it names is released on the
	# three snapshots, the documents go and the directory goes.
	cp "$tmp/gone-man" "$man" || fail "cannot put the manifest back"
	"$bin" --abort "$rds" > "$tmp/gone" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/gone"; fail "--abort with the result gone exited $st, want 0"; }
	[ "$(holdcount)" = 0 ] || \
	    { cat "$tmp/gone"; fail "--abort left $(holdcount) holds the header named under $tag"; }
	[ ! -e "$rundir" ] || \
	    { cat "$tmp/gone"; fail "--abort left the run directory $rundir"; }
	grep -q 'is not there' "$tmp/gone" || \
	    { cat "$tmp/gone"; fail "--abort did not say what it found"; }
	echo "ok   $case_id: the holds the header named are released and"
	echo "     the directory is gone"
	reset_pool
}

# (f) the name a verb is given, held against ZFS's own rule before it
# builds a path. --abort is the one verb that goes on where the
# dataset does not exist, so a name that is no dataset name used to
# read as "no such run" only after it had been made into a path:
# --abort "../../../../tmp/x" reached rmdir_run, which contains by
# a prefix compare and not by a parse, and took an empty /tmp/x/mnt
# and /tmp/x with it. The decoy below is exactly that shape. The
# identifier's resolution says it first now, with ZFS's own reason,
# and rundir_of says it again where a path is built. ZX219, ZX234.
settle_bad_name() {
	case_id="$fixture a name that is no dataset name"
	cases=$((cases + 1))
	rm -rf "$tmp/outside"
	mkdir -p "$tmp/outside/mnt" || fail "cannot make the decoy directory"
	# Enough .. to climb out of /var/db/zfs_rebase, so the first of
	# these spells $tmp/outside as a path and nothing at all as a
	# dataset name. The third carries a character ZFS's rule leaves
	# out: a space is NOT one of them -- valid_char in
	# module/zcommon/zfs_namecheck.c allows letters, digits, "-",
	# "_", ".", ":" and " ", so "result with a space" is a name a
	# dataset could have and the box said so on 2026-09-08 -- while
	# "?" is.
	for bad in "../../../..$tmp/outside" "$POOL/../../etc" \
	    "$POOL/result?no"; do
		"$bin" --abort "$bad" > "$tmp/badname" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/badname"; fail "--abort '$bad' exited $st, want 2"; }
		grep -q 'no name for a dataset' "$tmp/badname" || \
		    { cat "$tmp/badname"; fail "--abort '$bad' did not say the name is no dataset name"; }
	done
	[ -d "$tmp/outside/mnt" ] || \
	    fail "--abort removed $tmp/outside/mnt, which is no run directory"
	[ "$(holdcount)" = 0 ] || fail "a refused name changed the holds"
	rm -rf "$tmp/outside"
	echo "ok   $case_id: exit 2, and no path was built from it"
}

# Every case of these, for one form of one fixture. The two that do
# not depend on the form run in the clone pass alone: (d) and (e)
# destroy the result under the rebase, which in the dataset form
# would be the fixture's own onto, and (f) opens no pool at all.
settle_cases() {
	settle_done_order
	settle_busy_done
	settle_busy_abort
	if [ "$form" = clone ]; then
		settle_result_gone
		settle_bad_name
	fi
	return 0
}

# ---------------------------------------------------------------
# One fixture, both forms, every gate, three signals.
# ---------------------------------------------------------------
one_fixture() {
	fixture=$1
	say "fixture $fixture"
	fdir=$tmp/$(basename "$fixture" .zrt)
	rm -rf "$fdir"
	mkdir -p "$fdir" || exit 2
	"$bin" --build-fixture "$fixture" "$fdir" || exit 2
	[ -f "$fdir/expect" ] || exit 2
	flag=""
	case "$fixture" in *-permissive.zrt) flag="-p" ;; esac
	want_conf=$(sed -n 's/^#conflicts //p' "$fdir/expect")
	if [ "$want_conf" = 0 ]; then clean=1; else clean=0; fi
	nact=$(sed -n 's/^#actions //p' "$fdir/expect")
	[ "$nact" -ge 2 ] || \
	    { echo "FAIL: $fixture declares $nact actions; the action:2 gate wants two"; exit 1; }

	# The gates a fresh run passes, in the order it passes them,
	# and then the two a conflicted rebase reaches only through an
	# answered resolution.
	gates="held cloned read decided applying1 action:2"
	if [ $clean -eq 1 ]; then
		gates="$gates done"
	else
		gates="$gates conflicts applying2 done"
	fi

	make_pool
	for form in clone dataset; do
		prog_step "$(basename "$fixture" .zrt), the $form form"
		for gate in $gates; do
			for sig in INT TERM KILL; do
				kill_case "$form" "$gate" "$sig"
			done
		done
		settle_cases
	done
	drop_pool
	echo "ok   $fixture: every gate killed and continued"
}

nfx=0
for f in $fixtures; do nfx=$((nfx + 1)); done
prog_start $((nfx * 2)) "fixture forms"
for f in $fixtures; do
	one_fixture "$f"
done
echo "run-kills: $cases cases passed"
exit 0
