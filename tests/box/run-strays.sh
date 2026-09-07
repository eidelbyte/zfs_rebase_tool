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
# that a difference is repaired where the result is the run's own and
# reported everywhere else. The one fix in the tool is the self-check
# the applying1 stage makes on itself: the document held against the
# result, every action redone that did not land, and every name no
# action spoke for put back as onto had it. It is no flag's, and
# nothing after that gate writes at all. The cases, per fixture and
# form:
#
# 1. Strays into the result while the tool has it open, at action:1,
#    where the result is writable, no action has been performed yet,
#    and the result is still nobody's but the run's. Three of them,
#    and the self-check ends all three:
#
#      an edit to a file the manifest keeps untouched, which is
#      changed on the name list and is put back out of onto;
#      a name no tree had, which is extra and is taken away;
#      an edit to a file the manifest will write or copy, which the
#      action overwrites when it runs, so it ends up done: the apply
#      came after the edit and the apply is what the name holds.
#
#    --verify afterwards reports no drift, no pending action and
#    nothing outside the manifest, and a --continue changes nothing,
#    because there is nothing left to change.
#
# 2. A stray delete, which the same self-check catches: the name list
#    is over the shared name table and not over what the result
#    holds, so a name onto had that the result has lost is gone and
#    is restored out of onto with its bytes. The run does not stop
#    for it -- it goes on to its branch's gate as if the delete had
#    never happened -- and --verify afterwards has nothing to say.
#
# 3. Drift after the stage, which is what a check is for and what
#    nothing repairs: an edit to a file a clean action made, with
#    readonly off and back on behind the tool's back, is drifted 1
#    naming that file. --verify fixes nothing and neither does a
#    --continue: from the conflicts gate on the tree is
#    being edited by hand, an edit cannot be told from a stray, and
#    the gate reports and passes rather than blocking done for good.
#    Then the same edit to a conflicted name, which is never
#    classified at all: the report is what it was (the one clean
#    drift, still standing, still exit 3), nothing outside the
#    manifest, and the edit still there, because answering a
#    conflict is the conflict manager's and not a rebase's.
#
# 4. A stray write into the live from or onto dataset while the run
#    is reading, at the read gate: the tool reads snapshots, so the
#    manifest is the expect block to the byte and the verify is
#    clean. In the dataset form onto is not even where it lives just
#    then -- it is at the run's private mount, and stays there for
#    the whole of an open rebase -- so its own directory is empty and
#    a write there lands in the pool's root dataset and is hidden the
#    moment the dataset comes home at done or at --abort.
#
# 5. A stray edit at the conflicts gate, which a plain --continue
#    writes into the resolution rather than into the tree. Every
#    --continue that arrives at that gate checks first, under no flag
#    (documents-design.md, section 7). The edit
#    is to a file the manifest says nothing about, so it is one entry
#    of the name list; the gate turns every such entry into a drift
#    line with the choice keep, writes the document back and goes on.
#    The rebase reaches done with the edit still there -- keep means
#    the result stands -- and a --verify afterwards has nothing
#    outside the manifest to say about the name, because the name is
#    the resolution's now. Conflicted fixtures only: a clean rebase
#    never stops at that gate.
#
# 6. A stray between the last two gates, which the final check
#    reports and done does not block on. The conflicts are answered,
#    a --continue is stopped at the applying2 gate -- past the
#    conflicts gate's own check, with readonly already off -- and a
#    file a clean action made is edited there. The check at the done
#    gate finds it: the report says drifted 1 naming that file, the
#    exit status is 3, and done is reached all the same -- the record
#    off, the run directory gone, the result settled and the edit
#    still standing, because nothing past applying1 mends anything.
#    Then the same question put to the settled result -- --verify of
#    the manifest done left behind, which has no record to read and
#    takes the inputs from the header -- which must say what the done
#    gate said, drifted 1 with that name and exit 3, and leave the
#    result exactly as it stands. Conflicted fixtures only, for the
#    gate it stops at.
#
# 7. The onto snapshot destroyed after done, which is the one thing a
#    settled check has no answer for: gone by name is exit 2 naming
#    it, and there under the name with another snapshot's guid is
#    exit 2 with both numbers. The dataset form's alone -- the clone
#    form's onto snapshot is the result's origin and cannot be
#    destroyed -- and clean fixtures' alone, for the run that reaches
#    done in one invocation. A safety snapshot taken before the run
#    is what puts the fixture back.
#
# 8. A from side given as a dataset, so that the snapshot read is one
#    the tool took itself and destroyed at done. The settled check
#    says so in the words a post-done report has always used, calls
#    every action that reads from unchecked and exits 0: a snapshot
#    gone on purpose is no loss. The clone form's, for the second
#    thing it proves -- done leaves that clone unmounted with no
#    mountpoint of its own, so the check mounts it at the run
#    directory's mnt itself and takes the mount and the directory
#    away again.
#
# Every case ends by taking the rebase away -- --abort where one is
# still open, and by hand where it reached done, which leaves no
# record for --abort to find -- and the pool is proved to be the
# fixture again before the next one starts.
#
# Where the tree is to be read or edited moves with the rebase, and
# sethere is what says so. An open rebase holds the result at the
# run's private mount in both forms, the conflicts gate included: a
# dataset whose conflicts are unanswered is a half rebased tree, and
# a half rebased tree is not handed back into service
# (sprints/sprint-5/documents-design.md, section 5). done puts the
# dataset home and hands the clone to the void -- unmounted, with the
# mountpoint property still none -- and placing that clone is the
# user's work, which the tool's own last line spells out; the harness
# does exactly what it says.
#
# A clean fixture reaches done inside the run itself, and done takes
# the record off, so from that moment there is no rebase on the
# result for a verb named by --result to be asked about: the cases
# assert what the tree holds instead, and that every motional verb
# exits 2. The manifest is what names a settled rebase, and cases 5
# to 8 ask by it.
#
# Cases 1 to 7 give the dataset form from as a snapshot rather than
# as a dataset, so that the from tree is still there after done; case
# 8 is the other spelling, where a verify that cannot read from can
# only say unchecked. run-fixture.sh and run-kills.sh take the
# dataset spelling too.
set -u
cd "$(dirname "$0")/../.." || exit 2
. tests/box/progress.sh
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
fixtures=${*:-tests/fixtures/probe.zrt tests/fixtures/h-yw-row19.zrt}

POOL=zrtstray
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
MD=
pid=
cases=0
case_id=setup
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-stray.XXXXXX") || exit 2

cleanup() {
	prog_end
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
say() { printf '\n== %s\n' "$*"; prog_note "$*"; }
fail() { echo "FAIL: $case_id: $*"; exit 1; }
recval() { zfs get -H -o value "$1" "$2" 2>/dev/null; }
phasenow() {
	v=$(zfs get -H -o value zfs_rebase:phase "$1" 2>/dev/null)
	[ "$v" = - ] && v=""
	printf '%s' "$v"
}
localprops() {
	zfs get -H -o property,source all "$1" 2>/dev/null | \
	    awk '$1 ~ /^zfs_rebase:/ && $2 == "local" { print $1 }'
}
allsnaps() { zfs list -H -o name -t snapshot -r "$POOL"; }
hassnap() { zfs list -H -o name -t snapshot "$1" > /dev/null 2>&1; }
# One line of a manifest's header, which is where a rebase's identity
# lives once its record is off: the three snapshots with their guids,
# the form, the result and what the tool snapshotted itself.
hdr() { sed -n "s/^#$1 //p" "$2"; }
holdcount() {
	n=0
	for s in $(allsnaps); do
		c=$(zfs holds -H "$s" | grep -c .)
		n=$((n + c))
	done
	printf '%s' "$n"
}
mounted_at() { mount | grep -q " on $1 "; }
# Every kind of the name list at zero in one report: gone, extra,
# changed and unpooled, each on its own line with its count.
no_outside() {
	for k in gone extra changed unpooled; do
		grep -q "outside the manifest: $k 0" "$1" || return 1
	done
	return 0
}
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

# A clone whose rebase reached done is unmounted with its mountpoint
# property still none, which is the void the tool hands it to;
# placing it is the user's work and the tool's last line says how.
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

# Where the result's tree is to be read or edited just now, with the
# assertion that it is there: hmnt after this. An open rebase holds
# it at the run's private mount in both forms; done puts the dataset
# home and leaves the clone in the void, which the harness places.
sethere() {
	if [ -n "$(localprops "$rds")" ]; then
		hmnt=$rundir/mnt
		mounted_at "$hmnt" || \
		    fail "an open rebase does not hold $rds at $hmnt"
		[ "$form" = clone ] || \
		    [ "$(recval canmount "$POOL/onto")" = noauto ] || \
		    fail "canmount is not noauto while the rebase is open"
		return 0
	fi
	if [ "$form" = dataset ]; then
		hmnt=$MNT/onto
		mounted_at "$hmnt" || fail "done left onto away from home"
		[ "$(recval canmount "$POOL/onto")" = on ] || \
		    fail "done did not put canmount back to on"
		return 0
	fi
	place_clone
	hmnt=$MNT/result
	return 0
}

# Where the branch ends, as an assertion: a conflicted rebase waits
# at the conflicts gate with its record on the result, and a clean one
# reached done, which took every zfs_rebase: property off it. Either
# way it says where the tree is to be read from here on.
at_end() {
	if [ $clean -eq 1 ]; then
		[ -z "$(localprops "$rds")" ] || \
		    fail "the rebase reached done and left $(localprops "$rds")"
	else
		[ "$(phasenow "$rds")" = conflicts ] || \
		    fail "the phase is $(phasenow "$rds"), want conflicts"
	fi
	sethere
}

# --verify on the result, where there is still a rebase to ask about.
# Returns 0 with the report in $1 when there was, and 1 after
# checking that a settled result has no record for --result to read
# and says to give the manifest instead, which cases 5 to 8 do.
verify_open() {			# OUT WANTEXIT
	"$bin" --verify --result "$rds" > "$1" 2>&1
	st=$?
	if [ $clean -eq 1 ]; then
		[ $st -eq 2 ] || \
		    { cat "$1"; fail "--verify on a settled result exited $st, want 2"; }
		grep -q 'give the manifest' "$1" || \
		    { cat "$1"; fail "--verify did not ask for the manifest"; }
		return 1
	fi
	[ $st -eq "$2" ] || \
	    { cat "$1"; fail "--verify exited $st, want $2"; }
	return 0
}

# The same for a --continue, which checks at the gate it arrives at
# under no flag: on a settled result it is refused for want of a
# record, and there is nothing left for it to do in any case.
continue_open() {		# OUT WANTEXIT
	"$bin" --continue --result "$rds" > "$1" 2>&1
	st=$?
	if [ $clean -eq 1 ]; then
		[ $st -eq 2 ] || \
		    { cat "$1"; fail "--continue on a settled result exited $st, want 2"; }
		return 1
	fi
	[ $st -eq "$2" ] || \
	    { cat "$1"; fail "--continue exited $st, want $2"; }
	return 0
}

# The end of a case: --abort where a rebase is still open, and by
# hand where it reached done, since done left no record for --abort
# to find. What done leaves by hand is the result alone: the run
# directory went with it, and the case asserts that. Then the proof
# that the pool is the fixture again with no rebase left in it.
#
# $1 is the snapshot the dataset form rolls back to and destroys,
# which is the run's own pre-apply snapshot for every case but the
# one that destroys it: that one takes a snapshot of the fixture
# before its run and names it here instead.
end_case() {
	back=${1:-$POOL/onto@pre}
	if [ -n "$(localprops "$rds")" ]; then
		"$bin" --abort --result "$rds" > "$tmp/abort" 2>&1
		st=$?
		[ $st -eq 0 ] || { cat "$tmp/abort"; fail "--abort exited $st"; }
	else
		"$bin" --abort --result "$rds" > "$tmp/abort" 2>&1
		st=$?
		[ $st -eq 2 ] || \
		    { cat "$tmp/abort"; fail "--abort on a settled result exited $st, want 2"; }
		if [ "$form" = clone ]; then
			zfs destroy "$POOL/result" || \
			    fail "cannot destroy the settled result"
			rmdir "$MNT/result" 2>/dev/null
		else
			zfs rollback -r "$back" || \
			    fail "cannot roll onto back to $back"
			zfs destroy "$back" || fail "cannot destroy $back"
		fi
		[ ! -d "$rundir" ] || \
		    fail "done left the run directory $rundir"
	fi
	[ "$(holdcount)" = 0 ] || fail "the end of the case left holds behind"
	zfs list -H -o name "$POOL/result" >/dev/null 2>&1 && \
	    fail "the end of the case left $POOL/result behind"
	left=$(localprops "$POOL/onto")
	[ -z "$left" ] || fail "the end of the case left $left on $POOL/onto"
	[ -e "/var/db/zfs_rebase/$POOL" ] && \
	    fail "the end of the case left /var/db/zfs_rebase/$POOL behind"
	n=$(allsnaps | grep -c .)
	[ "$n" -eq 3 ] || { allsnaps; fail "the pool has $n snapshots, want 3"; }
	mounted_at "$MNT/onto" || fail "the end of the case left onto unmounted"
	# The -o pair is the user's and neither done nor --abort takes
	# it away, so the harness does: the next case asserts what its
	# own run wrote and not what the last one left.
	[ -f "$man" ] || fail "the end of the case has no -o manifest at $man"
	rm -f "$man" "$res"
	cases=$((cases + 1))
}

# The run this pass makes: paused at a gate, or straight through.
run_bg() {
	if [ "$form" = clone ]; then
		ZFS_REBASE_PAUSE=$1 "$bin" $flag -v -o "$man" \
		    --off-of "$POOL/from@work" --onto "$POOL/onto@work" \
		    --result "$POOL/result" > "$log" 2>&1 &
	else
		ZFS_REBASE_PAUSE=$1 "$bin" $flag -v -o "$man" \
		    --from "$POOL/from@work" --onto "$POOL/onto" \
		    --result pre > "$log" 2>&1 &
	fi
	pid=$!
	wait_stop "$pid" || { cat "$log"; fail "never stopped at $1"; }
}
run_fg() {
	if [ "$form" = clone ]; then
		"$bin" $flag -v -o "$man" --off-of "$POOL/from@work" \
		    --onto "$POOL/onto@work" --result "$POOL/result" \
		    > "$log" 2>&1
	else
		"$bin" $flag -v -o "$man" --from "$POOL/from@work" \
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
	at_end

	# The self-check ended all three: the edit to the untouched name
	# is onto's bytes again, the name no tree had is gone, and the
	# name the manifest wrote is done, since the action ran after
	# the edit and the apply is what the name holds.
	if verify_open "$tmp/verify" 0; then
		grep -q 'drifted 0' "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify found drift"; }
		grep -q 'pending 0' "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify found pending actions"; }
		no_outside "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify still names the strays"; }
	fi
	[ -f "$hmnt$wtgt" ] || fail "$wtgt is not there after the apply"
	grep -q stray "$hmnt$wtgt" && \
	    fail "the apply did not overwrite the stray edit to $wtgt"
	grep -q stray "$hmnt$kept" && \
	    fail "the self-check did not put the untouched $kept back"
	[ -e "$hmnt/zr-new" ] && fail "the self-check left the extra /zr-new"

	# And there is nothing left for a --continue to do: it reports
	# the same clean report and writes nothing.
	if continue_open "$tmp/cont" $wrun; then
		if verify_open "$tmp/verify2" 0; then
			no_outside "$tmp/verify2" || \
			    { cat "$tmp/verify2"; fail "--continue made a difference"; }
		fi
	fi
	grep -q stray "$hmnt$kept" && fail "$kept is the stray's again"
	echo "ok   $case_id: $wtgt done, $kept put back, /zr-new taken away"
	end_case
}

# --- 2. a stray delete, which the self-check puts back ---
case_delete() {
	case_id="$fixture $form a stray delete at action:1"
	run_bg action:1
	kept=$(kept_name "$man" "$wmnt")
	[ -n "$kept" ] || fail "the fixture has no untouched file to delete"
	cp "$wmnt$kept" "$tmp/kept.before" || fail "cannot read $kept"
	rm "$wmnt$kept" || fail "cannot delete $kept"
	# The name list is over the shared name table, so a name onto
	# had that the result has lost is gone, and gone is restored:
	# the run reaches its branch's gate as if nothing had happened.
	finish
	at_end
	[ -e "$hmnt$kept" ] || fail "the self-check did not put $kept back"
	cmp -s "$tmp/kept.before" "$hmnt$kept" || \
	    fail "$kept came back with other bytes than onto's"

	if verify_open "$tmp/verify" 0; then
		grep -q 'drifted 0' "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify found drift"; }
		no_outside "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify still sees the deletion"; }
	fi
	echo "ok   $case_id: restored by the self-check, the run went on to $wend"
	end_case
}

# --- 3. drift after the stage, reported and never repaired ---
case_drift() {
	case_id="$fixture $form drift after the stage"
	run_fg
	st=$?
	[ $st -eq $wrun ] || { cat "$log"; fail "the run exited $st, want $wrun"; }
	at_end
	tgt=$(write_target "$man" "$hmnt")
	[ -n "$tgt" ] || fail "the manifest writes or copies nothing"
	ro0=$(recval readonly "$rds")
	zfs set readonly=off "$rds" || fail "readonly=off"
	printf 'drift\n' >> "$hmnt$tgt" || fail "cannot edit $tgt"
	zfs set "readonly=$ro0" "$rds" || fail "readonly=$ro0"
	if verify_open "$tmp/verify" 3; then
		grep -q "drifted 1, first $tgt" "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify did not name the drifted $tgt"; }
		grep -q drift "$hmnt$tgt" || fail "--verify wrote to the result"
		at_end
	fi
	# And neither does a --continue: past applying1 an edit cannot
	# be told from a stray, so the gate reports it and passes
	# rather than mending it or blocking on it.
	if continue_open "$tmp/cont" $wrun; then
		grep -q "drifted 1, first $tgt" "$tmp/cont" || \
		    { cat "$tmp/cont"; fail "--continue did not report the drift"; }
		at_end
	fi
	grep -q drift "$hmnt$tgt" || fail "a verb wrote over the drift in $tgt"
	if verify_open "$tmp/verify2" 3; then
		grep -q "drifted 1, first $tgt" "$tmp/verify2" || \
		    { cat "$tmp/verify2"; fail "the drift is not reported any more"; }
	fi
	[ "$(recval readonly "$rds")" = "$ro0" ] || \
	    fail "the verb left readonly $(recval readonly "$rds"), want $ro0"
	echo "ok   $case_id: $tgt drifted 1, reported at every gate, never mended"

	# A conflicted name is not the rebase's to classify at all: no
	# action names it, and the operator's answer to it stands.
	if [ $clean -eq 0 ]; then
		case_id="$fixture $form an edit to a conflicted name"
		cname=$(conflict_name "$man")
		[ -n "$cname" ] || fail "the manifest marks no conflict"
		zfs set readonly=off "$rds" || fail "readonly=off"
		printf 'mine\n' >> "$hmnt$cname" || fail "cannot edit $cname"
		zfs set "readonly=$ro0" "$rds" || fail "readonly=$ro0"
		# The clean drift above still stands, since nothing past
		# applying1 mends it, so the verb still exits 3. What the
		# edit to the conflicted name must not do is add to that:
		# the same one drifted name, and the name list still empty.
		"$bin" --verify --result "$rds" > "$tmp/verify3" 2>&1
		st=$?
		[ $st -eq 3 ] || \
		    { cat "$tmp/verify3"; fail "--verify over a conflicted name exited $st, want 3 (the clean drift stands)"; }
		grep -q "drifted 1, first $tgt" "$tmp/verify3" || \
		    { cat "$tmp/verify3"; fail "the edit to the conflicted name changed the drift report"; }
		no_outside "$tmp/verify3" || \
		    { cat "$tmp/verify3"; fail "a conflicted name reached the name list"; }
		"$bin" --continue --result "$rds" > "$tmp/cont2" 2>&1
		st=$?
		[ $st -eq $wrun ] || \
		    { cat "$tmp/cont2"; fail "--continue exited $st, want $wrun"; }
		grep -q mine "$hmnt$cname" || \
		    fail "a verb overwrote the conflicted $cname"
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
	at_end

	# The tool read snapshots, so none of that is in the decision.
	sed -n '/^#mode/,$p' "$fdir/expect" > "$tmp/expect.body"
	sed -n '/^#mode/,$p' "$man" > "$tmp/got.body"
	cmp -s "$tmp/expect.body" "$tmp/got.body" || \
	    { diff "$tmp/expect.body" "$tmp/got.body" | head -20; \
	      fail "a live write changed the manifest"; }
	if verify_open "$tmp/verify" 0; then
		grep -q 'drifted 0' "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify found drift"; }
		no_outside "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "a live write reached the result"; }
	fi
	if [ "$form" = dataset ]; then
		# The write landed in the pool's root dataset, under
		# the directory onto is mounted over. While the rebase
		# is open onto is at the private mount and that
		# directory is uncovered, so the write is there to be
		# seen; the moment onto comes home, at done or at the
		# --abort end_case makes, it is hidden again. Either
		# way it is nowhere in the rebase.
		if [ -n "$(localprops "$rds")" ]; then
			[ -f "$MNT/onto/zr-live" ] || \
			    fail "the write at onto's uncovered mount point is gone"
		elif [ -e "$MNT/onto/zr-live" ]; then
			fail "the write at onto's mount point is visible in onto"
		fi
		[ -e "$hmnt/zr-live" ] && \
		    fail "the write at onto's mount point reached the rebase"
		echo "ok   $case_id: it landed in the pool root, not in the rebase"
	else
		[ -f "$MNT/onto/zr-live" ] || \
		    fail "the write into the live onto did not land"
		echo "ok   $case_id: the live edits are in the datasets and in no rebase"
	fi
	end_case
	# And with onto home again, which is what end_case leaves, the
	# pool root's copy is under the dataset and out of sight.
	if [ "$form" = dataset ] && [ -e "$MNT/onto/zr-live" ]; then
		fail "the write is still visible with onto back at home"
	fi

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

# --- 5. a stray edit at the conflicts gate, written into the
#        resolution as a drift line and never into the tree ---
case_driftline() {
	case_id="$fixture $form a drift line at the conflicts gate"
	run_fg
	st=$?
	[ $st -eq $wrun ] || { cat "$log"; fail "the run exited $st, want $wrun"; }
	[ "$(phasenow "$rds")" = conflicts ] || \
	    fail "the run is at $(phasenow "$rds"), want conflicts"
	sethere
	kept=$(kept_name "$man" "$hmnt")
	[ -n "$kept" ] || fail "the fixture has no untouched file to edit"
	names0=$(sed -n 's/^#names //p' "$res")
	[ -n "$names0" ] || { head -8 "$res"; fail "$res has no #names"; }
	# The edit the person makes while answering the conflicts. It
	# is to a name the manifest says nothing about, so nothing but
	# the second pass can see it.
	ro0=$(recval readonly "$rds")
	zfs set readonly=off "$rds" || fail "readonly=off"
	printf 'drift\n' >> "$hmnt$kept" || fail "cannot edit $kept"
	zfs set "readonly=$ro0" "$rds" || fail "readonly=$ro0"
	# Answering is one field per line and the header's count with
	# them, as tests/box/README.md says a hand edit must do.
	sed -e 's/ -$/ keep/' -e 's/^#unanswered .*$/#unanswered 0/' \
	    "$res" > "$res.answered" || fail "cannot answer $res"
	mv "$res.answered" "$res" || fail "cannot answer $res"
	"$bin" --continue --result "$rds" > "$tmp/drift" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/drift"; fail "--continue exited $st, want 0"; }
	grep -q '1 drift line added to the resolution' "$tmp/drift" || \
	    { cat "$tmp/drift"; fail "the gate added no drift line"; }
	# The document says so, in the tree grammar: the leaf of the
	# name, the word drift and the choice keep.
	leaf=$(basename "$kept")
	grep -q "^ *$leaf drift keep\$" "$res" || \
	    { cat "$res"; fail "$res has no drift line for $kept"; }
	names1=$(sed -n 's/^#names //p' "$res")
	[ "$names1" = "$((names0 + 1))" ] || \
	    { head -8 "$res"; fail "#names is $names1, want $((names0 + 1))"; }
	[ "$(sed -n 's/^#unanswered //p' "$res")" = 0 ] || \
	    { head -8 "$res"; fail "a drift keep line is not answered"; }
	# And the tree is untouched: keep means the result stands, so
	# the rebase went on to done with the edit still in it. done
	# is no phase: what says it got there is the record being off.
	[ -z "$(localprops "$rds")" ] || \
	    fail "the rebase reached done and left $(localprops "$rds")"
	# done settled the result: the dataset is home, the clone is
	# in the void and the harness places it to look at the tree.
	sethere
	grep -q drift "$hmnt$kept" || fail "a verb wrote over the edit to $kept"

	# The rebase is settled and carries no record, so --result has
	# nothing to read and says to give the manifest.
	"$bin" --verify --result "$rds" > "$tmp/driftv" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/driftv"; fail "--verify on a settled result exited $st, want 2"; }
	grep -q 'give the manifest' "$tmp/driftv" || \
	    { cat "$tmp/driftv"; fail "--verify did not ask for the manifest"; }
	# And given it, the check has nothing outside the manifest to
	# say about the name: it is the resolution's now, and a keep is
	# never compared, so the edit that was drift at the gate is
	# clean here.
	"$bin" --verify "$man" > "$tmp/driftvm" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/driftvm"; fail "--verify of the settled result exited $st, want 0"; }
	no_outside "$tmp/driftvm" || \
	    { cat "$tmp/driftvm"; fail "the settled check counts the kept name"; }
	[ ! -d "$rundir" ] || \
	    fail "the settled check left the run directory $rundir"
	grep -q drift "$hmnt$kept" || fail "--verify wrote over the edit"
	echo "ok   $case_id: $kept is a drift keep line, the edit stands,"
	echo "     and the settled check by $man says nothing of it"
	end_case
}

# --- 6. a stray between the last two gates: reported at done,
#        which is reached all the same ---
case_donedrift() {
	case_id="$fixture $form a stray before the done gate"
	run_fg
	st=$?
	[ $st -eq $wrun ] || { cat "$log"; fail "the run exited $st, want $wrun"; }
	[ "$(phasenow "$rds")" = conflicts ] || \
	    fail "the run is at $(phasenow "$rds"), want conflicts"
	sethere
	tgt=$(write_target "$man" "$hmnt")
	[ -n "$tgt" ] || fail "the manifest writes or copies nothing"
	# Answered in full, so this --continue passes the gate and
	# goes on to applying2, where the pause hook stops it.
	sed -e 's/ -$/ keep/' -e 's/^#unanswered .*$/#unanswered 0/' \
	    "$res" > "$res.answered" || fail "cannot answer $res"
	mv "$res.answered" "$res" || fail "cannot answer $res"
	ZFS_REBASE_PAUSE=applying2 "$bin" --continue -v --result "$rds" \
	    > "$tmp/late" 2>&1 &
	pid=$!
	wait_stop "$pid" || { cat "$tmp/late"; fail "never stopped at applying2"; }
	# The gate is written and readonly is off, so the edit goes in
	# without touching a property behind the tool's back; and the
	# conflicts gate's own check has already been made, so this is
	# a stray no check but the last one can see.
	printf 'late\n' >> "$wmnt$tgt" || fail "cannot edit $tgt"
	kill -CONT "$pid" || fail "cannot continue the stopped tool"
	wait "$pid"
	st=$?
	pid=
	[ $st -eq 3 ] || \
	    { cat "$tmp/late"; fail "the --continue exited $st, want 3"; }
	grep -q "drifted 1, first $tgt" "$tmp/late" || \
	    { cat "$tmp/late"; fail "the final check did not name the drifted $tgt"; }
	grep -q 'done does not block on' "$tmp/late" || \
	    { cat "$tmp/late"; fail "the run did not say done was reached all the same"; }
	# And done was reached: no record, no run directory, and the
	# result settled where its form puts it.
	[ -z "$(localprops "$rds")" ] || \
	    fail "the exit 3 left $(localprops "$rds") on $rds"
	[ ! -d "$rundir" ] || fail "the exit 3 left the run directory $rundir"
	sethere
	grep -q late "$hmnt$tgt" || fail "something mended the stray in $tgt"
	echo "ok   $case_id: drifted 1 at done, exit 3, done all the same"

	# And the same question asked again of the settled result,
	# which is what --verify MANIFEST is: no record to read, the
	# inputs taken from the header by name with their guids, the
	# same document held against the same tree. It must say what
	# the done gate said -- the one drifted name, exit 3 -- fix
	# nothing and leave the result exactly as it stands.
	ro0=$(recval readonly "$rds")
	"$bin" --verify "$man" > "$tmp/setv" 2>&1
	st=$?
	[ $st -eq 3 ] || \
	    { cat "$tmp/setv"; fail "--verify of the settled result exited $st, want 3"; }
	grep -q "drifted 1, first $tgt" "$tmp/setv" || \
	    { cat "$tmp/setv"; fail "the settled check did not name the drifted $tgt"; }
	grep -q late "$hmnt$tgt" || fail "the settled check mended the stray"
	[ -z "$(localprops "$rds")" ] || \
	    fail "the settled check wrote a property on $rds"
	[ "$(recval readonly "$rds")" = "$ro0" ] || \
	    fail "the settled check flipped readonly"
	[ ! -d "$rundir" ] || \
	    fail "the settled check left the run directory $rundir"
	mounted_at "$hmnt" || fail "the settled check moved the result off $hmnt"
	echo "ok   $case_id: --verify $man says the same, exit 3, nothing"
	echo "     touched"
	end_case
}

# --- 7. the onto snapshot destroyed after done: the settled check
#        refuses, and says what it looked for ---
#
# The dataset form's alone, because the clone form's onto snapshot is
# the result's origin and cannot be destroyed while the clone stands.
# The run's own pre-apply snapshot is the header's #onto here, and
# nothing depends on it once the rebase is over, so a hand can take
# it away -- which is the one thing the settled check has no answer
# for. A safety snapshot taken before the run is what puts the
# fixture back afterwards, since the snapshot end_case rolls back to
# is the one this case destroys.
case_ontogone() {
	case_id="$fixture $form the onto snapshot destroyed after done"
	zfs snapshot "$POOL/onto@zrsafe" || \
	    fail "cannot take the safety snapshot"
	run_fg
	st=$?
	[ $st -eq 0 ] || { cat "$log"; fail "the run exited $st, want 0"; }
	[ -z "$(localprops "$rds")" ] || \
	    fail "the run did not reach done: $(localprops "$rds") is still on $rds"
	sethere
	# Clean first, so that what the two refusals below prove is
	# the missing input and not something else.
	"$bin" --verify "$man" > "$tmp/og0" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/og0"; fail "--verify of the settled result exited $st, want 0"; }
	og=$(hdr onto "$man")
	osnap=${og%% *}
	oguid=${og##* }
	[ "$osnap" = "$POOL/onto@pre" ] || \
	    fail "#onto is $osnap, want $POOL/onto@pre"
	# Gone by name, which stops the check with the name.
	zfs destroy "$osnap" || fail "cannot destroy $osnap"
	"$bin" --verify "$man" > "$tmp/og1" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/og1"; fail "--verify with $osnap gone exited $st, want 2"; }
	grep -q "$osnap is gone" "$tmp/og1" || \
	    { cat "$tmp/og1"; fail "the refusal did not name $osnap"; }
	# And there under the name with another snapshot's guid, which
	# is the other half: a name is what a snapshot is called and
	# the guid is what it is, and the refusal prints both numbers.
	zfs snapshot "$osnap" || fail "cannot take $osnap again"
	nguid=$(recval guid "$osnap")
	[ -n "$nguid" ] && [ "$nguid" != "$oguid" ] || \
	    fail "the new $osnap has the guid the header kept"
	"$bin" --verify "$man" > "$tmp/og2" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/og2"; fail "--verify with another $osnap exited $st, want 2"; }
	grep -q "$osnap" "$tmp/og2" || \
	    { cat "$tmp/og2"; fail "the refusal did not name $osnap"; }
	grep -q "$oguid" "$tmp/og2" && grep -q "$nguid" "$tmp/og2" || \
	    { cat "$tmp/og2"; fail "the refusal did not print both guids"; }
	[ ! -d "$rundir" ] || \
	    fail "a refused settled check left the run directory $rundir"
	[ -z "$(localprops "$rds")" ] || \
	    fail "a refused settled check wrote a property on $rds"
	echo "ok   $case_id: exit 2 by name and by guid, both numbers said"
	end_case "$POOL/onto@zrsafe"
}

# --- 8. a from side given as a dataset: gone at done on purpose, and
#        the settled check says so rather than calling it a loss ---
#
# The clone form's, for the second thing it proves: done leaves the
# clone unmounted with no mountpoint of its own, so the check has
# nowhere to read it and mounts it at the run directory's mnt itself,
# then takes the mount and the directory away again.
case_madefrom() {
	case_id="$fixture $form a from side the tool made"
	"$bin" $flag -v -o "$man" --from "$POOL/from" \
	    --onto "$POOL/onto@work" --result "$POOL/result" > "$log" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$log"; fail "the run exited $st, want 0"; }
	[ -z "$(localprops "$rds")" ] || \
	    fail "the run did not reach done: $(localprops "$rds") is still on $rds"
	[ "$(hdr made "$man")" = from ] || \
	    { head -14 "$man"; fail "#made is $(hdr made "$man"), want from"; }
	fsnap=$(hdr from "$man")
	fsnap=${fsnap%% *}
	hassnap "$fsnap" && fail "$fsnap survived done; the tool made it"
	# The clone as done left it: in the void, which is where the
	# check has to place it for itself.
	[ "$(recval mountpoint "$POOL/result")" = none ] || \
	    fail "a settled clone's mountpoint is not none"
	[ "$(recval mounted "$POOL/result")" = no ] || \
	    fail "a settled clone is mounted somewhere"
	"$bin" --verify -v "$man" > "$tmp/mf" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/mf"; fail "--verify with the made from gone exited $st, want 0"; }
	grep -q 'the snapshot the tool took of it is not there any more' \
	    "$tmp/mf" || \
	    { cat "$tmp/mf"; fail "the check did not say the from side is gone by design"; }
	grep -q 'every action that reads from is unchecked' "$tmp/mf" || \
	    { cat "$tmp/mf"; fail "the check did not say what that leaves unchecked"; }
	grep -q 'drifted 0' "$tmp/mf" || \
	    { cat "$tmp/mf"; fail "the settled check found drift"; }
	grep -q "mounts it at $rundir/mnt" "$tmp/mf" || \
	    { cat "$tmp/mf"; fail "the check did not mount the clone privately"; }
	# And it is back in the void, with the directory gone again.
	[ ! -d "$rundir" ] || \
	    fail "the settled check left the run directory $rundir"
	[ "$(recval mounted "$POOL/result")" = no ] || \
	    fail "the settled check left the clone mounted"
	[ "$(recval mountpoint "$POOL/result")" = none ] || \
	    fail "the settled check changed the mountpoint property"
	[ "$(recval readonly "$POOL/result")" = on ] || \
	    fail "the settled check changed readonly"
	[ -z "$(localprops "$POOL/result")" ] || \
	    fail "the settled check wrote a property on the result"
	echo "ok   $case_id: the from side unchecked, exit 0, the clone"
	echo "     mounted and unmounted and the directory gone"
	end_case
}

# ---------------------------------------------------------------
# One fixture in one form: the cases above, each ending in --abort.
# The last four of them belong to one kind of fixture or one form.
# ---------------------------------------------------------------
stray_pass() {
	form=$1
	if [ "$form" = clone ]; then
		rds=$POOL/result
	else
		rds=$POOL/onto
	fi
	rundir=/var/db/zfs_rebase/$rds
	wmnt=$rundir/mnt		# where the tree is while the run has it
	hmnt=$wmnt			# and where it is between commands
	# The two documents go where -o says. This harness reads them
	# after the rebase has reached done -- the manifest against
	# the expect block, the resolution for its drift line -- and
	# done unlinks the two a run wrote into its own directory,
	# taking that directory with them. A -o pair is the user's and
	# stays, at done and at --abort alike (documents-design.md,
	# section 4). The no--o placement, <rundir>/manifest and
	# <rundir>/resolution, is box/run-kills.sh's: it asserts both
	# at every gate and their absence at done.
	man=$tmp/manifest
	res=$man.resolution
	log=$tmp/pass.log
	prog_step "$fixture, the $form form"
	say "$fixture, the $form form"
	case_strays
	case_delete
	case_drift
	case_live
	# The conflicts gate is the only place a drift line is written,
	# and a clean rebase never stops there; the applying2 gate case
	# 6 stops at is the conflicted fixtures' too.
	if [ $clean -eq 0 ]; then
		case_driftline
		case_donedrift
	fi
	# And the two settled checks, each of which wants a rebase
	# that reached done in one invocation: the destroyed onto
	# snapshot is the dataset form's, since the clone form's onto
	# is the result's origin, and the tool-made from side is the
	# clone form's, where done also leaves the result unmounted.
	if [ $clean -eq 1 ]; then
		if [ "$form" = clone ]; then
			case_madefrom
		else
			case_ontogone
		fi
	fi
	return 0
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
		wend=done		# and the gate it ends at
	else
		clean=0
		wrun=1			# and of one that stops at conflicts
		wend=conflicts
	fi
	make_pool
	stray_pass clone
	stray_pass dataset
	drop_pool
	echo "ok   $fixture: strays reported, repaired or overwritten as the verb says"
}

nfx=0
for f in $fixtures; do nfx=$((nfx + 1)); done
prog_start $((nfx * 2)) "fixture forms"
for f in $fixtures; do
	one_fixture "$f"
done
echo "run-strays: $cases cases passed"
exit 0
