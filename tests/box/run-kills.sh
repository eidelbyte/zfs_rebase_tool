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
#      the record with that state (or no state at all before
#      applying1), the three holds, the manifest from "decided" on,
#      and the result. readonly is back on wherever the tool had the
#      chance to put it back (every SIGINT and SIGTERM, and a SIGKILL
#      at a gate where readonly was already on) and off after a
#      SIGKILL inside an applying stage. In the dataset form a caught
#      signal hands the dataset back to its own mount point and a
#      SIGKILL leaves it at the run's private one, where the next
#      verb takes it from. readonly reads differently in the two
#      forms and says the same thing: the clone is the tool's and is
#      read-only whenever it is not being written to, while the
#      dataset is the user's and wears what the record says it wore
#      the moment it is handed back home -- readonly=on belongs to
#      the private mount, not to the dataset.
#
#   finished -- SIGINT and SIGTERM at done. Nothing looks at the flag
#      after that gate, so the run finishes: done, holds released,
#      exit 0.
#
# Then, in every case that left a rebase behind:
#
#   --continue --result reaches the gate the fixture's branch ends
#   in -- done for a clean one, conflicts for a conflicted one -- and
#   after it readonly is on, the dataset is home, the holds are gone
#   at done and there at conflicts, and a --posix rebase of the
#   fixture's from onto the result declares zero actions, which is
#   stage 1 idempotence. A kill before the manifest was written is
#   the exception: there is nothing to continue from and --continue
#   exits 2 naming the file it wanted.
#
#   Before that, --verify says what the kill left without touching
#   it: an action is pending until the stage that makes it has run,
#   so a rebase stopped before or inside applying1 exits 3 with
#   pending actions and one past it exits 0, and neither moves the
#   gate, the holds or the tree. The SIGKILL cases then continue
#   with --verify and the caught-signal ones without, so both paths
#   through the stages are walked and both must land in the same
#   place.
#
#   At the held gate, while the tool is stopped, zfs destroy of each
#   held snapshot must fail and leave the snapshot standing: that is
#   what the holds are for. Where nothing is cloned from the
#   snapshot the hold is the only thing in the way and the message
#   must say busy.
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
bin=./zfs_rebase
[ -x "$bin" ] || { echo "build first: make freebsd"; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 2; }
[ "$(uname)" = FreeBSD ] || { echo "FreeBSD only"; exit 2; }
fixtures=${*:-tests/fixtures/probe.zrt tests/fixtures/h-yw-row19.zrt}

POOL=zrtkill
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
MD=
pid=
cases=0
case_id=setup
tmp=$(mktemp -d /tmp/zr-kill.XXXXXX) || exit 2

cleanup() {
	[ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	"$bin" --abort --result "$POOL/result" >/dev/null 2>&1
	"$bin" --abort --result "$POOL/onto" >/dev/null 2>&1
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	rm -f "$IMG"
	chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null
	rm -rf "$tmp"
	rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT
say() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $case_id: $*"; exit 1; }
recval() { zfs get -H -o value "$1" "$2" 2>/dev/null; }
# The state, with "" for a record that has passed no gate yet.
statenow() {
	v=$(zfs get -H -o value zfs_rebase:state "$1" 2>/dev/null)
	[ "$v" = - ] && v=""
	printf '%s' "$v"
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

# The pool, as run-fixture.sh builds it: base with the fixture's base
# tree, from and onto cleared clones of it with the fixture's own
# trees, and a snapshot of each side.
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
reset_pool() {
	"$bin" --abort --result "$POOL/result" >/dev/null 2>&1
	"$bin" --abort --result "$POOL/onto" >/dev/null 2>&1
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
	return 0
}

# The skeleton the run wrote, answered the way a person would answer
# it: one field per line changed, "-" to keep, and the header's count
# of what is unanswered changed with them. Lines are never added and
# never removed. It must have been the tool's own unanswered document
# before this touched it.
answer_resolution() {
	[ -f "$1" ] || fail "the run wrote no resolution at $1"
	grep -q '^#rebase-resolution 4$' "$1" || \
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
	rundir=/var/db/zfs_rebase/$rds
	man=$rundir/manifest
	res=$rundir/resolution
	log=$tmp/case.log

	# What this gate and this signal must leave.
	resumed=no
	case "$gate" in
	held|cloned|read|decided)
		if [ "$sig" = KILL ]; then
			out=kept; wstate=""; wexit=137
			case "$gate" in
			decided) wman=yes ;;
			*) wman=no ;;
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
			out=torn; wexit=3; wstate=""; wman=no
			wro=off; wmnt=home
		fi ;;
	applying1|action:*)
		out=kept; wstate=applying1; wman=yes
		if [ "$sig" = KILL ]; then
			wexit=137; wro=off; wmnt=priv
		else
			wexit=3; wro=on; wmnt=home
		fi ;;
	conflicts)
		out=kept; wstate=conflicts; wman=yes; wro=on
		if [ "$sig" = KILL ]; then
			wexit=137; wmnt=priv
		else
			wexit=1; wmnt=home
		fi ;;
	applying2)
		out=kept; wstate=applying2; wman=yes; resumed=yes
		if [ "$sig" = KILL ]; then
			wexit=137; wro=off; wmnt=priv
		else
			wexit=3; wro=on; wmnt=home
		fi ;;
	done)
		wstate=done; wman=yes; wro=on
		[ $clean -eq 1 ] || resumed=yes
		if [ "$sig" = KILL ]; then
			out=kept; wexit=137; wmnt=priv
		else
			out=finished; wexit=0; wmnt=home
		fi ;;
	*)
		fail "no such gate" ;;
	esac
	# wro so far is the working value: on outside an applying
	# stage and off inside one. That is the clone's own, and it is
	# the dataset's while the run holds it at the private mount --
	# but a dataset handed back home is the user's again and wears
	# the readonly the record says it had, which is what the
	# fixture built it with.
	if [ "$form" = dataset ] && [ $wmnt = home ]; then
		wro=off
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
		[ "$(statenow "$rds")" = conflicts ] || \
		    fail "that run did not stop at conflicts"
		answer_resolution "$res"
		ZFS_REBASE_PAUSE=$gate "$bin" --continue --result "$rds" \
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
		tag=$(recval zfs_rebase:tag "$rds")
		for which in base from onto; do
			snap=$(recval "zfs_rebase:$which" "$rds")
			[ -n "$snap" ] || fail "the record names no $which"
			[ "$(holdtags "$snap")" = "$tag" ] || \
			    fail "$snap is not held under $tag"
			if zfs destroy "$snap" > "$tmp/destroy" 2>&1; then
				fail "zfs destroy $snap succeeded while held"
			fi
			hassnap "$snap" || \
			    fail "$snap went away although the destroy failed"
			# Where nothing is cloned from it the hold is
			# the only thing in the way and the kernel says
			# so; base always has the two sides cloned from
			# it, and onto's snapshot has the result in the
			# clone form, and there ZFS's own rule about a
			# clone's origin answers first.
			cl=$(zfs get -H -o value clones "$snap")
			if [ "$cl" = "-" ] || [ -z "$cl" ]; then
				grep -qi 'busy' "$tmp/destroy" || \
				    { cat "$tmp/destroy"; fail "the destroy of the held $snap did not say busy"; }
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
			mounted_at "$MNT/onto" || \
			    fail "a torn run left onto away from home"
			[ "$(recval readonly "$POOL/onto")" = off ] || \
			    fail "a torn run left onto read-only"
		fi
		n=$(zfs list -H -o name -t snapshot -r "$POOL/from" | grep -c .)
		[ "$n" -eq 1 ] || fail "a torn run left a snapshot on from"
		[ -e "$rundir" ] && fail "a torn run left $rundir"
		"$bin" --continue --result "$rds" > "$tmp/cont" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/cont"; fail "--continue after a torn run exited $st, want 2"; }
		echo "ok   $case_id: torn down whole, nothing to continue"
		reset_pool
		return 0
	fi

	# A rebase is still there: the record, the state it reached,
	# the three holds unless it reached done, and the manifest
	# from the decision on.
	[ "$(statenow "$rds")" = "$wstate" ] || \
	    fail "the state is '$(statenow "$rds")', want '$wstate'"
	if [ $out = finished ]; then
		whold_now=0
		[ "$(holdcount)" = 0 ] || \
		    fail "a run that reached done kept its holds"
	else
		whold_now=3
		[ "$(holdcount)" = 3 ] || \
		    fail "$(holdcount) holds in the pool, want 3"
		tag=$(recval zfs_rebase:tag "$rds")
		for which in base from onto; do
			snap=$(recval "zfs_rebase:$which" "$rds")
			[ "$(holdtags "$snap")" = "$tag" ] || \
			    fail "$snap is not held under $tag after the stop"
		done
	fi
	[ "$(recval readonly "$rds")" = "$wro" ] || \
	    fail "readonly is $(recval readonly "$rds"), want $wro"
	if [ $wman = yes ]; then
		[ -f "$man" ] || fail "no manifest at $man"
		[ -f "$res" ] || fail "no resolution at $res"
	else
		[ -e "$man" ] && fail "a manifest at $man before the decision"
		[ -e "$res" ] && fail "a resolution at $res before the decision"
	fi
	if [ "$form" = dataset ]; then
		if [ $wmnt = home ]; then
			mounted_at "$MNT/onto" || \
			    fail "onto was not handed back to $MNT/onto"
		else
			mounted_at "$rundir/mnt" || \
			    fail "onto is not at the private mount $rundir/mnt"
		fi
	fi

	# --- and then --continue ---
	if [ $wman = no ]; then
		# Killed before the decision: the record and the holds
		# are there, but the manifest the record names is not,
		# and that is the one thing no verb can do without.
		"$bin" --continue --result "$rds" > "$tmp/cont" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/cont"; fail "--continue without a manifest exited $st, want 2"; }
		grep -q "$man" "$tmp/cont" || \
		    { cat "$tmp/cont"; fail "--continue did not name the manifest"; }
		[ "$(statenow "$rds")" = "$wstate" ] || \
		    fail "the failed --continue moved the state"
		[ "$form" = dataset ] && { mounted_at "$MNT/onto" || \
		    fail "the failed --continue did not hand onto back"; }
		echo "ok   $case_id: at ${wstate:-no gate}, readonly $wro, 3 holds; no manifest to continue from"
		reset_pool
		return 0
	fi

	# What --verify says of what the kill left, before anything is
	# repaired: an action is pending until the stage that makes it
	# has run, so a rebase stopped before or inside applying1 has
	# pending actions and exits 3, and one past it has none. It
	# writes nothing and moves no gate either way.
	case "$wstate" in
	""|applying1) wrep=3 ;;
	*) wrep=0 ;;
	esac
	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq $wrep ] || \
	    { cat "$tmp/verify"; fail "--verify after the stop exited $st, want $wrep"; }
	grep -q 'drifted 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found drift after a kill"; }
	[ "$(statenow "$rds")" = "$wstate" ] || \
	    fail "--verify moved the state"
	[ "$(holdcount)" = "$whold_now" ] || \
	    fail "--verify changed the holds"

	# Where the --continue lands: at done when there is nothing to
	# answer or the answers are in, and back at conflicts while the
	# skeleton the run wrote is still unanswered. The file being
	# there is not the signal; its being complete is.
	if [ $clean -eq 1 ] || [ $resumed = yes ]; then
		wcont=0; wend=done; whold=0
	else
		wcont=1; wend=conflicts; whold=3
	fi
	# A SIGKILL is where a repair earns its keep, so those cases
	# continue with --verify and the caught-signal ones without:
	# both must land in the same place, since --verify only adds
	# the report and the mending of drift there is none of.
	vflag=""
	[ "$sig" = KILL ] && vflag="--verify"
	"$bin" --continue $vflag --result "$rds" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq $wcont ] || \
	    { cat "$tmp/cont"; fail "--continue $vflag exited $st, want $wcont"; }
	[ "$(statenow "$rds")" = "$wend" ] || \
	    fail "--continue landed at '$(statenow "$rds")', want $wend"
	[ "$(holdcount)" = $whold ] || \
	    fail "$(holdcount) holds after --continue, want $whold"
	if [ "$form" = clone ]; then
		endro=on
	else
		endro=off		# the record's own, as above
	fi
	[ "$(recval readonly "$rds")" = "$endro" ] || \
	    fail "readonly is $(recval readonly "$rds") after --continue, want $endro"
	if [ "$form" = dataset ]; then
		mounted_at "$MNT/onto" || \
		    fail "--continue did not hand onto back"
		cmnt=$MNT/onto
	else
		cmnt=$rundir/mnt
	fi
	again "$tmp/again" "$cmnt"
	echo "ok   $case_id: at $wstate, readonly $wro; --continue -> $wend (exit $st), stage 1 idempotent"
	reset_pool
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
		for gate in $gates; do
			for sig in INT TERM KILL; do
				kill_case "$form" "$gate" "$sig"
			done
		done
	done
	drop_pool
	echo "ok   $fixture: every gate killed and continued"
}

for f in $fixtures; do
	one_fixture "$f"
done
echo "run-kills: $cases cases passed"
exit 0
