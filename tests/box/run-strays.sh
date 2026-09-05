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
# nothing after that gate writes at all. Four cases per fixture and
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
#    nothing outside the manifest, and a --continue --verify changes
#    nothing, because there is nothing left to change.
#
# 2. A stray delete, which the same self-check catches: the name list
#    is over the shared name table and not over what the result
#    holds, so a name onto had that the result has lost is gone and
#    is restored out of onto with its bytes. The run does not stop
#    for it -- it goes on to its branch's gate as if the delete had
#    never happened -- and --verify afterwards has nothing to say.
#
# 3. Drift after the stage, which is what --verify is for and what
#    nothing repairs: an edit to a file a clean action made, with
#    readonly off and back on behind the tool's back, is drifted 1
#    naming that file. --verify fixes nothing and neither does
#    --continue --verify: from the conflicts gate on the tree is
#    being edited by hand, an edit cannot be told from a stray, and
#    the gate reports and passes rather than blocking done for good.
#    Then the same edit to a conflicted name, which is never
#    classified at all: exit 0, no drift, nothing outside the
#    manifest, and the edit still there, because answering a conflict
#    is the conflict manager's and not a rebase's.
#
# 4. A stray write into the live from or onto dataset while the run
#    is reading, at the read gate: the tool reads snapshots, so the
#    manifest is the expect block to the byte and the verify is
#    clean. In the dataset form onto is not even where it lives just
#    then -- it is at the run's private mount -- so its own directory
#    is empty and a write there lands in the pool's root dataset and
#    is hidden the moment the dataset comes home.
#
# 5. A stray edit at the conflicts gate, which --continue --verify
#    writes into the resolution rather than into the tree. The edit
#    is to a file the manifest says nothing about, so it is one entry
#    of the name list; the gate turns every such entry into a drift
#    line with the choice keep, writes the document back and goes on.
#    The rebase reaches done with the edit still there -- keep means
#    the result stands -- and a --verify afterwards has nothing
#    outside the manifest to say about the name, because the name is
#    the resolution's now. Conflicted fixtures only: a clean rebase
#    never stops at that gate.
#
# Every case ends in --abort, and the pool is proved to be the
# fixture again before the next one starts.
#
# The dataset form is given from as a snapshot here rather than as a
# dataset, so that the from tree is still there after done: a rebase
# that reached done has destroyed a snapshot it took itself, and a
# verify that cannot read from can only say unchecked.
# run-fixture.sh and run-kills.sh take the dataset spelling.
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

	# The self-check ended all three: the edit to the untouched name
	# is onto's bytes again, the name no tree had is gone, and the
	# name the manifest wrote is done, since the action ran after
	# the edit and the apply is what the name holds.
	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify"; fail "--verify exited $st, want 0"; }
	grep -q 'drifted 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found drift"; }
	grep -q 'pending 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found pending actions"; }
	no_outside "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify still names the strays"; }
	[ -f "$hmnt$wtgt" ] || fail "$wtgt is not there after the apply"
	grep -q stray "$hmnt$wtgt" && \
	    fail "the apply did not overwrite the stray edit to $wtgt"
	grep -q stray "$hmnt$kept" && \
	    fail "the self-check did not put the untouched $kept back"
	[ -e "$hmnt/zr-new" ] && fail "the self-check left the extra /zr-new"

	# And there is nothing left for a --continue to do: it reports
	# the same clean report and writes nothing.
	"$bin" --continue --verify --result "$rds" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq $wrun ] || \
	    { cat "$tmp/cont"; fail "--continue --verify exited $st, want $wrun"; }
	"$bin" --verify --result "$rds" > "$tmp/verify2" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify2"; fail "--verify after it exited $st"; }
	no_outside "$tmp/verify2" || \
	    { cat "$tmp/verify2"; fail "--continue --verify made a difference"; }
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
	[ "$(statenow "$rds")" = "$wend" ] || \
	    fail "the state is $(statenow "$rds"), want $wend"
	[ -e "$hmnt$kept" ] || fail "the self-check did not put $kept back"
	cmp -s "$tmp/kept.before" "$hmnt$kept" || \
	    fail "$kept came back with other bytes than onto's"

	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify"; fail "--verify exited $st, want 0"; }
	grep -q 'drifted 0' "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify found drift"; }
	no_outside "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify still sees the deletion"; }
	echo "ok   $case_id: restored by the self-check, the run went on to $wend"
	end_case
}

# --- 3. drift after the stage, reported and never repaired ---
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
	# And neither does a --continue --verify: past applying1 an
	# edit cannot be told from a stray, so the gate reports it and
	# passes rather than mending it or blocking on it.
	"$bin" --continue --verify --result "$rds" > "$tmp/cont" 2>&1
	st=$?
	[ $st -eq $wrun ] || \
	    { cat "$tmp/cont"; fail "--continue --verify exited $st, want $wrun"; }
	grep -q "drifted 1, first $tgt" "$tmp/cont" || \
	    { cat "$tmp/cont"; fail "--continue --verify did not report the drift"; }
	grep -q drift "$hmnt$tgt" || fail "--continue --verify wrote to the result"
	[ "$(statenow "$rds")" = "$wend" ] || \
	    fail "the state is $(statenow "$rds"), want $wend"
	"$bin" --verify --result "$rds" > "$tmp/verify2" 2>&1
	st=$?
	[ $st -eq 3 ] || \
	    { cat "$tmp/verify2"; fail "--verify after it exited $st, want 3"; }
	grep -q "drifted 1, first $tgt" "$tmp/verify2" || \
	    { cat "$tmp/verify2"; fail "the drift is not reported any more"; }
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
		"$bin" --verify --result "$rds" > "$tmp/verify3" 2>&1
		st=$?
		[ $st -eq 0 ] || \
		    { cat "$tmp/verify3"; fail "--verify over a conflicted name exited $st, want 0"; }
		no_outside "$tmp/verify3" || \
		    { cat "$tmp/verify3"; fail "a conflicted name reached the name list"; }
		"$bin" --continue --verify --result "$rds" > "$tmp/cont2" 2>&1
		st=$?
		[ $st -eq $wrun ] || \
		    { cat "$tmp/cont2"; fail "--continue --verify exited $st, want $wrun"; }
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
	no_outside "$tmp/verify" || \
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

# --- 5. a stray edit at the conflicts gate, written into the
#        resolution as a drift line and never into the tree ---
case_driftline() {
	case_id="$fixture $form a drift line at the conflicts gate"
	run_fg
	st=$?
	[ $st -eq $wrun ] || { cat "$log"; fail "the run exited $st, want $wrun"; }
	[ "$(statenow "$rds")" = conflicts ] || \
	    fail "the run is at $(statenow "$rds"), want conflicts"
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
	"$bin" --continue --verify --result "$rds" > "$tmp/drift" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/drift"; fail "--continue --verify exited $st, want 0"; }
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
	# the rebase went on to done with the edit still in it.
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is $(statenow "$rds"), want done"
	grep -q drift "$hmnt$kept" || fail "a verb wrote over the edit to $kept"

	# A verify afterwards has nothing outside the manifest to say:
	# the name is the resolution's now, and a keep is never
	# compared.
	"$bin" --verify --result "$rds" > "$tmp/driftv" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/driftv"; fail "--verify exited $st, want 0"; }
	no_outside "$tmp/driftv" || \
	    { cat "$tmp/driftv"; fail "the drift is still outside the manifest"; }
	grep -q 'the resolution: drifted 0' "$tmp/driftv" || \
	    { cat "$tmp/driftv"; fail "--verify did not report the resolution"; }
	grep -q drift "$hmnt$kept" || fail "--verify wrote over the edit"
	echo "ok   $case_id: $kept is a drift keep line, the edit stands"
	end_case
}

# ---------------------------------------------------------------
# One fixture in one form: the cases above, each ending in --abort.
# The last of them is the conflicted fixtures' alone.
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
	res=$rundir/resolution
	log=$tmp/pass.log
	prog_step "$(basename "$fixture" .zrt), the $form form"
	say "$fixture, the $form form"
	case_strays
	case_delete
	case_drift
	case_live
	# The conflicts gate is the only place a drift line is written,
	# and a clean rebase never stops there.
	[ $clean -eq 0 ] && case_driftline
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
