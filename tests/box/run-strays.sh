#!/bin/sh
# Box harness: stray edits during and after the apply. FreeBSD, root,
# after make freebsd. Usage:
#
#	run-strays.sh [FIXTURE.zrt ...]
#
# with tests/fixtures/probe.zrt (conflicted) and
# tests/fixtures/h-yw-row19.zrt (clean) as the default pair, each run
# in both forms of the tool. The way in is the pause hook
# (tests/box/README.md).
#
# The claim under test is that the apply pass is the only writer, and
# that everything else is either reported or repaired, exactly as the
# verb asked for. Four cases per fixture and form:
#
# 1. Strays into the result while the tool has it open, at action:1,
#    where the result is writable and no action has been performed
#    yet. Three of them survive the run's own re-walk, which checks
#    the names and the pooling of every result pool the decision
#    made and not their bytes:
#
#      an edit to a file the manifest keeps untouched, which the
#      re-walk does not look at and --verify reports as an
#      information line, since it is neither the manifest's outcome
#      nor onto's own;
#      a name no tree had, likewise an information line;
#      an edit to a file the manifest will write or copy, which the
#      action overwrites when it runs, so it ends up done: the apply
#      came after the edit and the apply is what the name holds.
#
#    --verify then names two information lines and no drift and no
#    pending action, and --continue --verify leaves both of them
#    exactly where they are. That is deliberate and not a gap: an
#    untouched name is not the manifest's to repair -- no action of
#    the manifest names it -- and a repair that put onto's bytes back
#    over an operator's edit would be destroying work the rebase
#    never asked about. What the verb owes is the report, and it
#    gives it.
#
# 2. A stray delete, which is the one the re-walk does catch: the
#    decision says that name survives with that pool, and it is not
#    there. The run fails with exit 3 at the re-walk, keeps the
#    result at applying1 with its record and its holds, and
#    --continue takes it on to its branch's gate. Afterwards the
#    deletion is invisible: --verify's information lines are over
#    the names the result holds, so a name it does not hold has
#    nothing to be held against, and no action names it either. It
#    is reported by the run that saw it happen and by nothing after
#    that.
#
# 3. Drift after the stage, which is what --verify is for: an edit to
#    a file a clean action made, with readonly off and back on
#    behind the tool's back, is drifted 1 naming that file, --verify
#    fixes nothing, --continue --verify puts it back, and --verify is
#    clean afterwards. Then the same edit to a conflicted name, which
#    is never classified and never touched: exit 0, no drift, and the
#    edit still there after a repair, because answering a conflict is
#    the conflict manager's and not a rebase's.
#
# 4. A stray write into the live from or onto dataset while the run
#    is reading, at the read gate: the tool reads snapshots, so the
#    manifest is the expect block to the byte and the verify is
#    clean. In the dataset form onto is not even where it lives just
#    then -- it is at the run's private mount -- so its own directory
#    is empty and a write there lands in the pool's root dataset and
#    is hidden the moment the dataset comes home.
#
# Every case ends in --abort, and the pool is proved to be the
# fixture again before the next one starts.
#
# The dataset form is given from as a snapshot here rather than as a
# dataset, so that the from tree is still there after done: a rebase
# that reached done has destroyed a snapshot it took itself, and the
# repair in case 3 has to read from. run-fixture.sh and run-kills.sh
# take the dataset spelling.
set -u
cd "$(dirname "$0")/../.." || exit 2
bin=./zfs_rebase
[ -x "$bin" ] || { echo "build first: make freebsd"; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 2; }
[ "$(uname)" = FreeBSD ] || { echo "FreeBSD only"; exit 2; }
fixtures=${*:-tests/fixtures/probe.zrt tests/fixtures/h-yw-row19.zrt}

POOL=zrtstray
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
MD=
pid=
cases=0
case_id=setup
tmp=$(mktemp -d /tmp/zr-stray.XXXXXX) || exit 2

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
statenow() {
	v=$(zfs get -H -o value zfs_rebase:state "$1" 2>/dev/null)
	[ "$v" = - ] && v=""
	printf '%s' "$v"
}
localprops() {
	zfs get -H -o property,source all "$1" 2>/dev/null | \
	    awk '$1 ~ /^zfs_rebase:/ && $2 == "local" { print $1 }'
}
allsnaps() { zfs list -H -o name -t snapshot -r "$POOL"; }
holdcount() {
	n=0
	for s in $(allsnaps); do
		c=$(zfs holds -H "$s" | grep -c .)
		n=$((n + c))
	done
	printf '%s' "$n"
}
mounted_at() { mount | grep -q " on $1 "; }
procstat() { ps -o stat= -p "$1" 2>/dev/null | tr -d ' \t' | cut -c1; }
wait_stop() {
	i=0
	while [ $i -lt 300 ]; do
		st=$(procstat "$1")
		case "$st" in
		T) return 0 ;;
		Z|"") return 1 ;;
		esac
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}

# Every action of a manifest as "path kind isdir", with the paths
# rebuilt from the indentation of the tree section, which the two
# dots that close the root end. The fixtures used here spell no name
# that needs escaping, so a manifest path is a path on disk as it
# stands; one that did would have to be unescaped first.
actions() {
	awk '
	/^#/ { next }
	{
		n = match($0, /[^ ]/)
		if (n == 0) next
		ind = int((n - 1) / 4)
		s = substr($0, n)
		if (s == "/") next
		if ($1 == "..") { if (ind <= 1) exit; next }
		name = $1
		isdir = 0
		if (substr(name, length(name), 1) == "/") {
			name = substr(name, 1, length(name) - 1)
			isdir = 1
		}
		path = ""
		for (i = 1; i < ind; i++)
			path = path "/" dirs[i]
		path = path "/" name
		if (isdir)
			dirs[ind] = name
		print path, $2, isdir
	}' "$1"
}

# A file of the tree at $2 that the manifest at $1 says nothing about
# and that shares its object with no other name, so that an edit to
# it is an edit to nothing the manifest speaks for.
kept_name() {
	actions "$1" | awk '{ print $1 }' | sort -u > "$tmp/acted"
	(cd "$2" && find . -type f -links 1 | sed 's/^\.//') | sort \
	    > "$tmp/files"
	comm -23 "$tmp/files" "$tmp/acted" | head -1
}

# The first file the manifest at $1 writes or copies whose parent
# directory is already there in the tree at $2, so that the harness
# can put its own bytes at that name before the action does.
write_target() {
	target=""
	actions "$1" > "$tmp/acts"
	while read -r apath akind adir; do
		[ "$adir" = 0 ] || continue
		case "$akind" in
		cp|write) ;;
		*) continue ;;
		esac
		[ -d "$(dirname "$2$apath")" ] || continue
		target=$apath
		break
	done < "$tmp/acts"
	printf '%s' "$target"
}

# The first name the manifest marks conflict, which no verb may
# classify or touch.
conflict_name() {
	actions "$1" | awk '$2 == "conflict" { print $1; exit }'
}

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

# --abort, and the proof that the pool is the fixture again with no
# rebase left in it.
end_case() {
	"$bin" --abort --result "$rds" > "$tmp/abort" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/abort"; fail "--abort exited $st"; }
	[ "$(holdcount)" = 0 ] || fail "--abort left holds behind"
	zfs list -H -o name "$POOL/result" >/dev/null 2>&1 && \
	    fail "--abort left $POOL/result behind"
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || fail "--abort left $left on $POOL/onto"
	[ -e "/var/db/zfs_rebase/$POOL" ] && \
	    fail "--abort left /var/db/zfs_rebase/$POOL behind"
	n=$(allsnaps | grep -c .)
	[ "$n" -eq 3 ] || { allsnaps; fail "the pool has $n snapshots, want 3"; }
	mounted_at "$MNT/onto" || fail "--abort left onto unmounted"
	cases=$((cases + 1))
}

# The run this pass makes: paused at a gate, or straight through.
run_bg() {
	if [ "$form" = clone ]; then
		ZFS_REBASE_PAUSE=$1 "$bin" $flag -v \
		    --off-of "$POOL/from@work" --onto "$POOL/onto@work" \
		    --result "$POOL/result" > "$log" 2>&1 &
	else
		ZFS_REBASE_PAUSE=$1 "$bin" $flag -v \
		    --from "$POOL/from@work" --onto "$POOL/onto" \
		    --result pre > "$log" 2>&1 &
	fi
	pid=$!
	wait_stop "$pid" || { cat "$log"; fail "never stopped at $1"; }
}
run_fg() {
	if [ "$form" = clone ]; then
		"$bin" $flag -v --off-of "$POOL/from@work" \
		    --onto "$POOL/onto@work" --result "$POOL/result" \
		    > "$log" 2>&1
	else
		"$bin" $flag -v --from "$POOL/from@work" \
		    --onto "$POOL/onto" --result pre > "$log" 2>&1
	fi
}
# Let it go and take its exit status.
finish() {
	kill -CONT "$pid" || fail "cannot continue the stopped tool"
	wait "$pid"
	st=$?
	pid=
	[ $st -eq $wrun ] || { cat "$log"; fail "the run exited $st, want $wrun"; }
}

# --- 1. strays into the result the apply is writing ---
case_strays() {
	case_id="$fixture $form strays at action:1"
	run_bg action:1
	kept=$(kept_name "$man" "$wmnt")
	[ -n "$kept" ] || fail "the fixture has no untouched file to edit"
	wtgt=$(write_target "$man" "$wmnt")
	[ -n "$wtgt" ] || fail "the manifest writes or copies nothing"
	printf 'stray\n' >> "$wmnt$kept" || fail "cannot edit $kept"
	printf 'stray\n' > "$wmnt/zr-new" || fail "cannot create /zr-new"
	printf 'stray\n' >> "$wmnt$wtgt" || fail "cannot edit $wtgt"
	finish

	# The two names outside the manifest are information lines and
	# the name the manifest wrote is done: the action ran after the
	# edit, so what is there is the rebase's and not the stray's.
	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify"; fail "--verify exited $st, want 0"; }
	grep -q 'drifted 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found drift"; }
	grep -q 'pending 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found pending actions"; }
	grep -q "2 names outside the manifest changed, first $kept" \
	    "$tmp/verify" || { cat "$tmp/verify"; fail "--verify did not name the two strays"; }
	[ -f "$hmnt$wtgt" ] || fail "$wtgt is not there after the apply"
	grep -q stray "$hmnt$wtgt" && \
	    fail "the apply did not overwrite the stray edit to $wtgt"
	grep -q stray "$hmnt$kept" || \
	    fail "the edit to the untouched $kept is gone"

	# A repair does not touch them: no action of the manifest
	# names an untouched file, so there is nothing there to make
	# true again, and the report is what the operator gets.
	"$bin" --continue --verify --result "$rds" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq $wrun ] || \
	    { cat "$tmp/cont"; fail "--continue --verify exited $st, want $wrun"; }
	"$bin" --verify --result "$rds" > "$tmp/verify2" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify2"; fail "--verify after the repair exited $st"; }
	grep -q "2 names outside the manifest changed, first $kept" \
	    "$tmp/verify2" || { cat "$tmp/verify2"; fail "the repair changed the information lines"; }
	grep -q stray "$hmnt$kept" || fail "the repair overwrote the untouched $kept"
	[ -f "$hmnt/zr-new" ] || fail "the repair removed the new name"
	echo "ok   $case_id: $wtgt done, $kept and /zr-new reported and left"
	end_case
}

# --- 2. a stray delete, which the re-walk catches ---
case_delete() {
	case_id="$fixture $form a stray delete at action:1"
	run_bg action:1
	kept=$(kept_name "$man" "$wmnt")
	[ -n "$kept" ] || fail "the fixture has no untouched file to delete"
	rm "$wmnt$kept" || fail "cannot delete $kept"
	kill -CONT "$pid" || fail "cannot continue the stopped tool"
	wait "$pid"
	st=$?
	pid=
	# The re-walk holds every result pool the decision made against
	# the tree: a name that is gone is one name short.
	[ $st -eq 3 ] || { cat "$log"; fail "the run exited $st, want 3"; }
	grep -q 'after apply' "$log" || \
	    { cat "$log"; fail "the run did not fail in the re-walk"; }
	[ "$(statenow "$rds")" = applying1 ] || \
	    fail "the state is $(statenow "$rds"), want applying1"
	[ "$(holdcount)" = 3 ] || fail "the kept run does not hold its inputs"

	"$bin" --continue --result "$rds" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq $wrun ] || \
	    { cat "$tmp/cont"; fail "--continue exited $st, want $wrun"; }
	# And now nobody can see it: an information line is over the
	# names the result holds, and this one it does not hold.
	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify"; fail "--verify exited $st, want 0"; }
	grep -q 'drifted 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found drift"; }
	grep -q '0 names outside the manifest changed' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify made an information line of the deletion"; }
	[ -e "$hmnt$kept" ] && fail "$kept came back; nothing puts it back"
	echo "ok   $case_id: the re-walk caught it (exit 3), --continue went on, no verb sees it after"
	end_case
}

# --- 3. drift after the stage, and a conflicted name ---
case_drift() {
	case_id="$fixture $form drift after the stage"
	run_fg
	st=$?
	[ $st -eq $wrun ] || { cat "$log"; fail "the run exited $st, want $wrun"; }
	state=$(statenow "$rds")
	tgt=$(write_target "$man" "$hmnt")
	[ -n "$tgt" ] || fail "the manifest writes or copies nothing"
	ro0=$(recval readonly "$rds")
	zfs set readonly=off "$rds" || fail "readonly=off"
	printf 'drift\n' >> "$hmnt$tgt" || fail "cannot edit $tgt"
	zfs set "readonly=$ro0" "$rds" || fail "readonly=$ro0"
	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq 3 ] || { cat "$tmp/verify"; fail "--verify over drift exited $st, want 3"; }
	grep -q "drifted 1, first $tgt" "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify did not name the drifted $tgt"; }
	grep -q drift "$hmnt$tgt" || fail "--verify wrote to the result"
	[ "$(statenow "$rds")" = "$state" ] || fail "--verify moved the state"
	"$bin" --continue --verify --result "$rds" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq $wrun ] || \
	    { cat "$tmp/cont"; fail "--continue --verify exited $st, want $wrun"; }
	grep -q drift "$hmnt$tgt" && fail "the repair left the drift in $tgt"
	"$bin" --verify --result "$rds" > "$tmp/verify2" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify2"; fail "--verify after the repair exited $st"; }
	grep -q 'drifted 0' "$tmp/verify2" || \
	    { cat "$tmp/verify2"; fail "the repair left drift behind"; }
	[ "$(recval readonly "$rds")" = "$ro0" ] || \
	    fail "the repair left readonly $(recval readonly "$rds"), want $ro0"
	echo "ok   $case_id: $tgt drifted 1, repaired, clean again"

	# A conflicted name is not the rebase's to classify or to
	# mend: no action names it, and the operator's answer to it
	# stands.
	if [ $clean -eq 0 ]; then
		case_id="$fixture $form an edit to a conflicted name"
		cname=$(conflict_name "$man")
		[ -n "$cname" ] || fail "the manifest marks no conflict"
		zfs set readonly=off "$rds" || fail "readonly=off"
		printf 'mine\n' >> "$hmnt$cname" || fail "cannot edit $cname"
		zfs set "readonly=$ro0" "$rds" || fail "readonly=$ro0"
		"$bin" --verify --result "$rds" > "$tmp/verify3" 2>&1
		st=$?
		[ $st -eq 0 ] || \
		    { cat "$tmp/verify3"; fail "--verify over a conflicted name exited $st, want 0"; }
		grep -q 'drifted 0' "$tmp/verify3" || \
		    { cat "$tmp/verify3"; fail "a conflicted name was classified as drift"; }
		grep -q '0 names outside the manifest changed' "$tmp/verify3" || \
		    { cat "$tmp/verify3"; fail "a conflicted name became an information line"; }
		"$bin" --continue --verify --result "$rds" > "$tmp/cont2" 2>&1
		st=$?
		[ $st -eq $wrun ] || \
		    { cat "$tmp/cont2"; fail "--continue --verify exited $st, want $wrun"; }
		grep -q mine "$hmnt$cname" || \
		    fail "the repair overwrote the conflicted $cname"
		echo "ok   $case_id: $cname neither classified nor touched"
	fi
	end_case
}

# --- 4. a stray write into the live datasets while the run reads ---
case_live() {
	case_id="$fixture $form a stray write to the live datasets"
	run_bg read
	printf 'live\n' > "$MNT/from/zr-live" || fail "cannot write into the live from"
	if [ "$form" = dataset ]; then
		# onto is the run's own just now, mounted at the run's
		# private place: what is left at its own mount point is
		# an empty directory of the pool's root dataset.
		[ -z "$(ls -A "$MNT/onto")" ] || \
		    fail "onto's own mount point is not empty while the run has it"
		mounted_at "$wmnt" || fail "onto is not at $wmnt during the run"
	fi
	printf 'live\n' > "$MNT/onto/zr-live" || \
	    fail "cannot write at onto's mount point"
	finish

	# The tool read snapshots, so none of that is in the decision.
	sed -n '/^#mode/,$p' "$fdir/expect" > "$tmp/expect.body"
	sed -n '/^#mode/,$p' "$man" > "$tmp/got.body"
	cmp -s "$tmp/expect.body" "$tmp/got.body" || \
	    { diff "$tmp/expect.body" "$tmp/got.body" | head -20; \
	      fail "a live write changed the manifest"; }
	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify"; fail "--verify exited $st, want 0"; }
	grep -q 'drifted 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found drift"; }
	grep -q '0 names outside the manifest changed' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "a live write reached the result"; }
	if [ "$form" = dataset ]; then
		[ -e "$MNT/onto/zr-live" ] && \
		    fail "the write at onto's mount point is visible in onto"
		echo "ok   $case_id: it landed in the pool root and is hidden"
	else
		[ -f "$MNT/onto/zr-live" ] || \
		    fail "the write into the live onto did not land"
		echo "ok   $case_id: the live edits are in the datasets and in no rebase"
	fi
	end_case

	# Take the two strays away again, so the pool is the fixture
	# for the next form. The one under onto's own mount point is
	# hidden by the dataset and needs the dataset out of the way.
	rm -f "$MNT/from/zr-live"
	if [ "$form" = dataset ]; then
		zfs unmount "$POOL/onto" || fail "cannot unmount onto to clean up"
		rm -f "$MNT/onto/zr-live"
		zfs mount "$POOL/onto" || fail "cannot mount onto again"
	else
		rm -f "$MNT/onto/zr-live"
	fi
	[ -e "$MNT/from/zr-live" ] && fail "the live strays are still there"
	[ -e "$MNT/onto/zr-live" ] && fail "the live strays are still there"
	return 0
}

# ---------------------------------------------------------------
# One fixture in one form: the four cases, each ending in --abort.
# ---------------------------------------------------------------
stray_pass() {
	form=$1
	if [ "$form" = clone ]; then
		rds=$POOL/result
	else
		rds=$POOL/onto
	fi
	rundir=/var/db/zfs_rebase/$rds
	man=$rundir/manifest
	wmnt=$rundir/mnt		# where the tree is while the tool has it
	if [ "$form" = clone ]; then
		hmnt=$rundir/mnt	# and where it is when it does not
	else
		hmnt=$MNT/onto
	fi
	log=$tmp/pass.log
	say "$fixture, the $form form"
	case_strays
	case_delete
	case_drift
	case_live
}

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
	if [ "$want_conf" = 0 ]; then
		clean=1
		wrun=0			# the exit of a run that reaches done
	else
		clean=0
		wrun=1			# and of one that stops at conflicts
	fi
	make_pool
	stray_pass clone
	stray_pass dataset
	drop_pool
	echo "ok   $fixture: strays reported, repaired or overwritten as the verb says"
}

for f in $fixtures; do
	one_fixture "$f"
done
echo "run-strays: $cases cases passed"
exit 0
