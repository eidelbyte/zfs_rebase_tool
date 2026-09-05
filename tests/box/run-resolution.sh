#!/bin/sh
# Box harness: the resolution carried out. FreeBSD, root, after
# make freebsd. Usage:
#
#	run-resolution.sh [FIXTURE.zrt ...]
#
# with tests/fixtures/probe.zrt, tests/fixtures/h-s2-two-conflicts.zrt
# and tests/fixtures/freebsd/acl-conflict.zrt as the default set, each
# run in both forms of the tool. Every fixture given must declare a
# conflict: a rebase with none never reaches the gate this script is
# about, and the script says so rather than passing vacuously.
#
# The other harnesses answer every conflict with keep, the one choice
# that changes nothing. This one is where a choice of onto or from is
# carried out for the first time, where the two --take flags and the
# two gate flags are exercised on real datasets, and where a kill
# lands inside the applying2 stage. What it asserts, case by case:
#
# 1. Headless to done. A fresh run given --take-onto --no-gui writes
#    its skeleton answered, which makes it complete from the start,
#    so the run passes its own conflicts gate and reaches done in one
#    process (exit 0, not 1). The record then reads
#    zfs_rebase:take onto, locally; the document has nothing left to
#    answer and every line reads onto; every conflicted name in the
#    result is onto's object at that name -- type, mode, ownership,
#    bytes or link target, the ACL and both namespaces of extended
#    attributes -- or is gone where onto has no such name; the names
#    of one group that onto pools together are one object here too;
#    the clean names are untouched, which a second --posix rebase
#    declaring no action says; and --verify afterwards reports every
#    line of the resolution done. Then the same with --take-from.
#    Cells: ZX122, ZX125, ZA56, ZA57 (on an acl fixture), ZM83.
#
# 2. --no-merge. The same run given --no-merge stops at the gate with
#    the document complete, since the flag is the command saying not
#    yet; a --continue --no-merge stops there again and moves
#    nothing; a --continue --no-gui then passes the gate and reaches
#    done; and --no-merge on a --continue whose record is at done is
#    refused before any stage runs -- exit 2, "past the merge", the
#    state and readonly unmoved. Cells: ZX123, ZX126.
#
# 3. An incomplete skeleton stops. A plain fresh run says how many
#    names are unanswered and exits 1; a --continue over the
#    untouched document says the same count of the same total and
#    exits 1; and answering one line of it is not answering it, so
#    the next --continue names what is left. Cells: ZX131, ZM83.
#
# 4. A hand-edited choice of each kind, on a fixture with more than
#    one conflicted name. keep: the conflicted file is merged by hand
#    in the result while the rebase waits, the choice keep is written
#    into the document, and at done the hand merge stands and
#    --verify reports the name under the resolution as keep and never
#    as drift. onto and from: the name is that side's object at done,
#    pooled as that side pools it. Answering by hand is one field per
#    line and the header's count with them, since the parser refuses
#    a count that does not match its lines. Cells: ZX130.
#
# 5. --restart under a --take record. The rebase is stopped at the
#    gate with --no-merge, its answers are changed to something else,
#    and --restart puts back the document the run was started with --
#    every line onto again, nothing left to answer -- and then goes
#    on through the gate to done by itself, as the fresh run did:
#    what a restart discards is the answering somebody did
#    afterwards, not the instruction the rebase was started with.
#    Cells: ZX124, ZM82.
#
# 6. Drift lines. A clean file is edited while the rebase waits at
#    the gate; --continue --verify writes it into the resolution as a
#    drift line with the choice keep and then stops, because the
#    conflicts are still unanswered and a document written to is not
#    a document answered. Answering them takes the rebase to done
#    with the edit intact and the name the resolution's. The second
#    half is the same up to the drift line and then flips its choice
#    to onto: at done the name is back as onto had it. run-strays.sh
#    case 5 is the neighbouring case -- there the conflicts are
#    answered before the --continue --verify, so the gate writes the
#    line and passes in one command; here it writes and waits.
#    Cells: ZX132, ZX133 (ZY94 is run-strays.sh's).
#
# 7. Kills. The pause hook (tests/box/README.md) has two gates this
#    script is the only user of. At "manifest" the manifest is
#    written and recorded and the skeleton is not: the one window in
#    which a rebase has one of its two documents, and a SIGKILL there
#    leaves a rebase whose exits are --restart, which writes the
#    skeleton again from the recorded manifest, and --abort. At
#    "choice:1" the applying2 stage is part way through carrying the
#    choices out: a SIGKILL there leaves applying2 with readonly off,
#    --no-merge is refused from there, and --continue redoes the
#    whole document -- which is idempotent -- and reaches done with
#    nothing left for a second pass to do. Cells: ZX134, ZX135,
#    ZA56, ZX126.
#
# 8. The ACL strip under a choice. A non-trivial NFSv4 ACL is put on
#    a clean directory of the result while the rebase waits; the gate
#    writes it into the document as a drift line; the line is flipped
#    to onto, and the choice must put the directory back as onto had
#    it, which means stripping the ACL, since a directory that is
#    already there is the one thing a choice rewrites in place. The
#    stage's own second pass must then find nothing to do: a strip
#    that did not happen fails the run there. That is the hole
#    apply-choices recorded -- macOS writes an ACL and never strips
#    one -- answered on the platform whose za_setacl strips.
#    Cells: ZX136, ZA57.
#
# Every case ends in --abort, and the pool is proved to be the
# fixture again before the next one starts. One pool per fixture,
# built and destroyed here, so the script runs alone.
#
# Both sides are given as snapshots, in both forms, so that from's
# tree is still there to compare against after done: a rebase that
# reached done has destroyed a snapshot it took itself, and a name
# held against a side that cannot be read can only be called
# unchecked. onto is the dataset in the dataset form, which is what
# makes it that form, and its pre-apply snapshot is where onto's own
# tree is read from there.
#
# The fixtures used here spell no name that needs escaping, so a
# manifest path is a path on disk as it stands; one that did would
# have to be unescaped first, as run-strays.sh says of its own
# helper.
set -u
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
fixtures=${*:-tests/fixtures/probe.zrt tests/fixtures/h-s2-two-conflicts.zrt tests/fixtures/freebsd/acl-conflict.zrt}

POOL=zrtres
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
MD=
pid=
cases=0
case_id=setup
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-res.XXXXXX") || exit 2

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
recsrc() { zfs get -H -o source "$1" "$2" 2>/dev/null; }
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
# The plural the tool's own messages take, so that a count of one
# reads as the tool prints it.
sfx() { if [ "$1" -eq 1 ]; then printf ''; else printf 's'; fi; }
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
# dots that close the root end. The expect block of a fixture is a
# manifest, so this reads that too, and the conflict section after it
# is past the close and is never looked at.
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

# Every name the manifest marks conflict, which is every name the
# skeleton has a line for.
conflict_names() { actions "$1" | awk '$2 == "conflict" { print $1 }'; }

# A file of the tree at $2 that the manifest at $1 says nothing about
# and that shares its object with no other name, so that an edit to
# it is an edit to nothing the manifest speaks for.
kept_name() {
	actions "$1" | awk '{ print $1 }' | sort -u > "$tmp/acted"
	(cd "$2" && find . -type f -links 1 | sed 's/^\.//') | sort \
	    > "$tmp/files"
	comm -23 "$tmp/files" "$tmp/acted" | head -1
}

# And a directory of it, for the one thing a choice rewrites in place
# rather than making anew. A directory above a name the manifest
# speaks for is spoken for too -- a conflict mark covers what is
# under it, and an action's parent is on the way to it -- so those
# are passed over. The root is not a candidate: no manifest line can
# name it.
kept_dir() {
	actions "$1" | awk '{ print $1 }' | sort -u > "$tmp/acted"
	(cd "$2" && find . -mindepth 1 -type d | sed 's/^\.//') | sort \
	    > "$tmp/dirs"
	comm -23 "$tmp/dirs" "$tmp/acted" > "$tmp/kdirs"
	while read -r d; do
		grep -q "^$d/" "$tmp/acted" && continue
		printf '%s' "$d"
		return 0
	done < "$tmp/kdirs"
	printf ''
}

# --- one name held against one side ---------------------------------
# The side is read through its own snapshot, which is where the tool
# reads it: absence is a first-class answer on both ends, and what is
# there is compared on every axis the oracle compares, so that an
# attribute a choice dropped is a failure here and not only in the
# second --posix rebase.
have() { [ -e "$1" ] || [ -L "$1" ]; }
# The type bits and the permission bits (both octal), the owner and
# the group. A directory's link count and size are its children's and
# are no part of the object a choice names, so they are asked of
# everything else only.
sig() {
	if [ -d "$1" ] && [ ! -L "$1" ]; then
		stat -f '%Hp %Lp %u %g' "$1"
	else
		stat -f '%Hp %Lp %l %u %g %z' "$1"
	fi
}
# -h, so that a symlink answers for itself and not for what it
# points at, as lsextattr and getextattr are asked below.
aclof() { getfacl -nqh "$1" 2>/dev/null; }
# Both namespaces, name and value. An attribute name with a space in
# it would need more than this; no fixture spells one.
xattrof() {
	for ns in user system; do
		for a in $(lsextattr -qh "$ns" "$1" 2>/dev/null); do
			printf '%s %s ' "$ns" "$a"
			getextattr -qhx "$ns" "$a" "$1" 2>/dev/null | \
			    tr -d ' \n'
			printf '\n'
		done
	done
}
same_as() {			# SIDEDIR RESULTMNT PATH
	sp=$1$3
	rp=$2$3
	if ! have "$sp"; then
		have "$rp" && \
		    fail "$3 is in the result and the side has no such name"
		return 0
	fi
	have "$rp" || fail "$3 is not in the result and the side has it"
	[ "$(sig "$sp")" = "$(sig "$rp")" ] || \
	    fail "$3: stat is '$(sig "$rp")', want the side's '$(sig "$sp")'"
	if [ -L "$sp" ]; then
		[ "$(readlink "$sp")" = "$(readlink "$rp")" ] || \
		    fail "$3: the link target is not the side's"
	elif [ -f "$sp" ]; then
		cmp -s "$sp" "$rp" || fail "$3: the bytes are not the side's"
	fi
	[ "$(aclof "$sp")" = "$(aclof "$rp")" ] || \
	    { aclof "$sp"; aclof "$rp"; fail "$3: the ACL is not the side's"; }
	[ "$(xattrof "$sp")" = "$(xattrof "$rp")" ] || \
	    fail "$3: the extended attributes are not the side's"
	return 0
}

# The names of one group that chose the same side and that that side
# holds in one pool are one object in the result too, and two names
# it keeps apart stay apart. Inode numbers say both.
pooled_like() {			# SIDEDIR RESULTMNT
	: > "$tmp/pool"
	for p in $names; do
		have "$1$p" || continue
		printf '%s %s %s\n' "$(stat -f %i "$1$p")" \
		    "$(stat -f %i "$2$p")" "$p" >> "$tmp/pool"
	done
	awk '
	{
		if (s[$1] == "") s[$1] = $2
		else if (s[$1] != $2) {
			print "the side pools " $3 " with a name the result " \
			    "keeps apart"
			bad = 1
		}
		if (r[$2] == "") r[$2] = $1
		else if (r[$2] != $1) {
			print "the result pools " $3 " with a name the side " \
			    "keeps apart"
			bad = 1
		}
	}
	END { exit bad ? 1 : 0 }' "$tmp/pool" > "$tmp/poolsay" || \
	    { cat "$tmp/poolsay"; fail "the pooling is not the side's"; }
	return 0
}

# --- the resolution, read and answered by hand ----------------------
res_names() { sed -n 's/^#names //p' "$1"; }
res_left() { sed -n 's/^#unanswered //p' "$1"; }
# Answering is one field per line and the header's count with them,
# as tests/box/README.md says a hand edit must do: the parser refuses
# a count that does not match its lines. Lines are never added and
# never removed.
answer_all() {			# FILE CHOICE
	sed -e "s/ -\$/ $2/" -e 's/^#unanswered .*$/#unanswered 0/' \
	    "$1" > "$1.new" || fail "cannot answer $1"
	mv "$1.new" "$1" || fail "cannot answer $1"
}
# One line of it, and the count with that one line.
answer_one() {
	left=$(res_left "$1")
	awk -v left="$left" '
	/^#unanswered / { print "#unanswered " (left - 1); next }
	!hit && / -$/ { sub(/ -$/, " keep"); hit = 1 }
	{ print }' "$1" > "$1.new" || fail "cannot answer one line of $1"
	mv "$1.new" "$1" || fail "cannot answer one line of $1"
}
# A choice already made, changed to another: what a picker does and
# what --restart discards.
rechoose() {			# FILE FROM TO
	sed "s/ $2\$/ $3/" "$1" > "$1.new" || fail "cannot edit $1"
	mv "$1.new" "$1" || fail "cannot edit $1"
}
# Every conflict line reads this choice, and nothing is left over.
answered_all_as() {		# FILE CHOICE COUNT
	[ "$(res_left "$1")" = 0 ] || \
	    { head -8 "$1"; fail "$1 has names left to answer"; }
	n=$(grep -c " conflict [0-9][0-9]* $2\$" "$1" || true)
	[ "$n" = "$3" ] || \
	    { cat "$1"; fail "$n of $3 lines read $2"; }
	return 0
}

# --- the pool -------------------------------------------------------
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
	# The inheritance trap. A user property set on the pool root
	# shows up on every dataset under it, so every property this
	# script reads back off a record must be that dataset's own
	# local value: zfs_rebase:take above all, which decides what
	# skeleton a --restart writes.
	zfs set zfs_rebase:tag=bogus "$POOL" || exit 2
	zfs set zfs_rebase:manifest=/nonexistent/manifest "$POOL" || exit 2
	zfs set zfs_rebase:resolution=/nonexistent/resolution "$POOL" || exit 2
	zfs set zfs_rebase:take=bogus "$POOL" || exit 2
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
	[ "$(recval readonly "$POOL/onto")" = off ] || \
	    fail "--abort left onto read-only"
	cases=$((cases + 1))
	return 0
}

# Rebase the fixture's from onto the result again, over three plain
# directories: stage 1 is idempotent, so this must have nothing left
# to do, and the conflicts it declares are what the choices left of
# them.
again() {			# OUT RESULTMNT WANTCONFLICTS
	"$bin" --posix $flag -o "$1" "$fdir/base" "$fdir/from" "$2" \
	    > /dev/null 2>&1
	st=$?
	[ $st -eq 0 ] || [ $st -eq 1 ] || fail "the --posix re-run exited $st"
	grep -q '^#actions 0$' "$1" || \
	    { sed -n '1,20p' "$1"; fail "rebasing onto the result declares actions"; }
	grep -q "^#conflicts $3\$" "$1" || \
	    { grep '^#conflicts' "$1"; fail "rebasing onto the result wants $3 conflicts"; }
	return 0
}

# The run this pass makes, with whatever flags the case adds, and the
# same one stopped at a gate.
fresh() {
	if [ "$form" = clone ]; then
		"$bin" $flag -v "$@" --off-of "$POOL/from@work" \
		    --onto "$POOL/onto@work" --result "$POOL/result" \
		    > "$log" 2>&1
	else
		"$bin" $flag -v "$@" --from "$POOL/from@work" \
		    --onto "$POOL/onto" --result pre > "$log" 2>&1
	fi
}
fresh_bg() {
	gate=$1
	shift
	if [ "$form" = clone ]; then
		ZFS_REBASE_PAUSE=$gate "$bin" $flag -v "$@" \
		    --off-of "$POOL/from@work" --onto "$POOL/onto@work" \
		    --result "$POOL/result" > "$log" 2>&1 &
	else
		ZFS_REBASE_PAUSE=$gate "$bin" $flag -v "$@" \
		    --from "$POOL/from@work" --onto "$POOL/onto" \
		    --result pre > "$log" 2>&1 &
	fi
	pid=$!
	wait_stop "$pid" || { cat "$log"; fail "never stopped at $gate"; }
}
# A fresh run that must stop at the gate with its skeleton
# unanswered, which is where most of the cases below begin.
at_conflicts() {
	fresh "$@"
	st=$?
	[ $st -eq 1 ] || { cat "$log"; fail "the run exited $st, want 1"; }
	[ "$(statenow "$rds")" = conflicts ] || \
	    fail "the run is at '$(statenow "$rds")', want conflicts"
	[ -f "$res" ] || fail "the run wrote no resolution at $res"
	return 0
}
# The result writable for one edit made behind the tool's back, and
# read-only again after it, as run-strays.sh does.
ro_off() {
	ro0=$(recval readonly "$rds")
	zfs set readonly=off "$rds" || fail "readonly=off"
}
ro_back() { zfs set "readonly=$ro0" "$rds" || fail "readonly=$ro0"; }
# Which side a choice names, as the directory to read it out of.
sidedir() {
	if [ "$1" = onto ]; then
		printf '%s' "$ontodir"
	else
		printf '%s' "$fromdir"
	fi
}

# --- 1. headless to done under each --take flag ---------------------
case_headless() {
	side=$1
	case_id="$fixture $form headless --take-$side"
	fresh "--take-$side" --no-gui
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$log"; fail "--take-$side --no-gui exited $st, want 0"; }
	# One process: the gate said the document was complete and
	# went on rather than waiting for a --continue.
	grep -q "the resolution $res is answered in full; going on" "$log" || \
	    { cat "$log"; fail "the run did not pass its own conflicts gate"; }
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	[ "$(recval zfs_rebase:take "$rds")" = "$side" ] || \
	    fail "zfs_rebase:take is $(recval zfs_rebase:take "$rds"), want $side"
	[ "$(recsrc zfs_rebase:take "$rds")" = local ] || \
	    fail "zfs_rebase:take is not the result's own value"
	[ "$(recval zfs_rebase:resolution "$rds")" = "$res" ] || \
	    fail "zfs_rebase:resolution is $(recval zfs_rebase:resolution "$rds"), want $res"
	answered_all_as "$res" "$side" "$nconf"
	# The tree: every conflicted name is that side's object, or
	# gone where that side has no such name, and pooled as that
	# side pools it.
	sdir=$(sidedir "$side")
	[ -d "$sdir" ] || fail "$sdir is not there; .zfs/snapshot did not mount"
	for p in $names; do
		same_as "$sdir" "$hmnt" "$p"
	done
	pooled_like "$sdir" "$hmnt"
	# The clean names are as stage 1 left them, so a second rebase
	# has no action to declare. Its conflicts are what the choices
	# left: taking from makes the result from's at those names, so
	# the disagreement is gone, and taking onto leaves it standing.
	if [ "$side" = from ]; then
		wconf=0
	else
		wconf=$want_conf
	fi
	again "$tmp/again" "$hmnt" "$wconf"
	# And the verb says the same of the document: every onto or
	# from line held against that side and found done.
	"$bin" --verify --result "$rds" > "$tmp/verify" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/verify"; fail "--verify exited $st, want 0"; }
	grep -q "the resolution: done $nconf, first " "$tmp/verify" || \
	    { cat "$tmp/verify"; fail "--verify does not call every choice done"; }
	for k in pending blocked drifted unchecked; do
		grep -q "the resolution: $k 0\$" "$tmp/verify" || \
		    { cat "$tmp/verify"; fail "--verify reports a $k choice"; }
	done
	echo "ok   $case_id: done in one process, $nconf name$(sfx "$nconf") ${side}'s"
	end_case
}

# --- 2. --no-merge holds the gate, and is refused past it -----------
case_nomerge() {
	case_id="$fixture $form --no-merge at the gate"
	fresh --take-onto --no-merge
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$log"; fail "--take-onto --no-merge exited $st, want 1"; }
	grep -q "the resolution $res is answered in full, and --no-merge leaves the merge to you" "$log" || \
	    { cat "$log"; fail "the run did not say why it stopped"; }
	[ "$(statenow "$rds")" = conflicts ] || \
	    fail "the run is at '$(statenow "$rds")', want conflicts"
	answered_all_as "$res" onto "$nconf"
	[ "$(holdcount)" = 3 ] || fail "$(holdcount) holds at the gate, want 3"
	# The flag says not yet as often as it is given, and leaves
	# the gate where it is for the next command without it.
	"$bin" --continue --no-merge --result "$rds" > "$tmp/nm2" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/nm2"; fail "--continue --no-merge exited $st, want 1"; }
	grep -q "$rds: the resolution is answered in full, and --no-merge leaves the merge to you" "$tmp/nm2" || \
	    { cat "$tmp/nm2"; fail "the verb did not say why it stopped"; }
	[ "$(statenow "$rds")" = conflicts ] || \
	    fail "--continue --no-merge moved the gate"
	# And without it the same document takes the same rebase on.
	"$bin" --continue --no-gui --result "$rds" > "$tmp/nm3" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/nm3"; fail "--continue --no-gui exited $st, want 0"; }
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	for p in $names; do
		same_as "$ontodir" "$hmnt" "$p"
	done
	# Past the merge there is no gate left for the flag to hold,
	# and it is refused before any stage runs: nothing moves.
	ro0=$(recval readonly "$rds")
	"$bin" --continue --no-merge --result "$rds" > "$tmp/nm4" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/nm4"; fail "--no-merge at done exited $st, want 2"; }
	grep -q 'past the merge' "$tmp/nm4" || \
	    { cat "$tmp/nm4"; fail "the refusal does not say past the merge"; }
	[ "$(statenow "$rds")" = done ] || fail "the refusal moved the state"
	[ "$(recval readonly "$rds")" = "$ro0" ] || \
	    fail "the refusal flipped readonly"
	echo "ok   $case_id: held at the gate twice, passed once, refused at done"
	end_case
}

# --- 3. an incomplete skeleton stops, and says by how much ----------
case_incomplete() {
	case_id="$fixture $form an incomplete skeleton stops"
	at_conflicts
	grep -q "^zfs_rebase: $nconf name$(sfx "$nconf") unanswered in the resolution $res\$" "$log" || \
	    { cat "$log"; fail "the run did not name what is unanswered"; }
	[ "$(res_left "$res")" = "$nconf" ] || \
	    { head -8 "$res"; fail "the skeleton is not wholly unanswered"; }
	# The file being there is not the signal; its being complete
	# is. A --continue over the untouched document says the same.
	"$bin" --continue --result "$rds" > "$tmp/inc1" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/inc1"; fail "--continue over a skeleton exited $st, want 1"; }
	grep -q "^zfs_rebase: $rds: conflicts unresolved\$" "$tmp/inc1" || \
	    { cat "$tmp/inc1"; fail "--continue did not say conflicts unresolved"; }
	grep -q "^zfs_rebase: $nconf of $nconf name$(sfx "$nconf") unanswered in the resolution $res\$" "$tmp/inc1" || \
	    { cat "$tmp/inc1"; fail "--continue did not name the count and the file"; }
	[ "$(statenow "$rds")" = conflicts ] || \
	    fail "the refused --continue moved the state"
	# And answering some of it is not answering it.
	if [ "$nconf" -gt 1 ]; then
		answer_one "$res"
		left=$((nconf - 1))
		[ "$(res_left "$res")" = "$left" ] || \
		    { head -8 "$res"; fail "answering one line left $(res_left "$res")"; }
		"$bin" --continue --result "$rds" > "$tmp/inc2" 2>&1
		st=$?
		[ $st -eq 1 ] || \
		    { cat "$tmp/inc2"; fail "a part-answered document exited $st, want 1"; }
		grep -q "^zfs_rebase: $left of $nconf name$(sfx "$nconf") unanswered in the resolution $res\$" "$tmp/inc2" || \
		    { cat "$tmp/inc2"; fail "--continue did not name what is left"; }
		echo "ok   $case_id: $nconf, then $left, and the gate held both times"
	else
		echo "ok   $case_id: $nconf unanswered at the run and at the verb"
		echo "     (one conflicted name: the part-answered document wants"
		echo "      a fixture with two)"
	fi
	end_case
}

# --- 4. a hand-edited choice of each kind ---------------------------
case_hand() {
	kind=$1
	case_id="$fixture $form a hand-edited choice of $kind"
	edited=""
	at_conflicts
	if [ "$kind" = keep ]; then
		# The hand merge: the person edits the conflicted file
		# in the result while the rebase waits, and keep is
		# the word for "the result stands, hand merges
		# included".
		ro_off
		for p in $names; do
			[ -f "$hmnt$p" ] || continue
			[ -L "$hmnt$p" ] && continue
			printf 'mine\n' >> "$hmnt$p" || fail "cannot edit $p"
			edited=$p
			break
		done
		ro_back
		[ -n "$edited" ] || \
		    fail "no conflicted name of this fixture is a plain file"
	fi
	answer_all "$res" "$kind"
	[ "$(res_left "$res")" = 0 ] || \
	    { head -8 "$res"; fail "the document is not answered"; }
	[ "$(res_names "$res")" = "$nconf" ] || \
	    { head -8 "$res"; fail "answering changed the count of names"; }
	"$bin" --continue --no-gui --result "$rds" > "$tmp/hand" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/hand"; fail "--continue over $kind exited $st, want 0"; }
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	case "$kind" in
	keep)
		grep -q mine "$hmnt$edited" || \
		    fail "the choice keep did not leave the hand merge alone"
		# The name is the resolution's now: it is under the
		# document with the choice the person made, it is no
		# drift, and it is in no list of names outside the
		# manifest.
		"$bin" --verify -v --result "$rds" > "$tmp/handv" 2>&1
		st=$?
		[ $st -eq 0 ] || \
		    { cat "$tmp/handv"; fail "--verify exited $st, want 0"; }
		grep -q "^zfs_rebase:     $edited keep done\$" "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "$edited is not under the resolution as keep"; }
		grep -q "the resolution: drifted 0\$" "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "a kept name is reported as drift"; }
		no_outside "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "a kept name reached the name list"; }
		grep -q mine "$hmnt$edited" || fail "--verify wrote over the merge"
		echo "ok   $case_id: $edited stands, and is the resolution's"
		;;
	*)
		sdir=$(sidedir "$kind")
		[ -d "$sdir" ] || fail "$sdir is not there"
		for p in $names; do
			same_as "$sdir" "$hmnt" "$p"
		done
		pooled_like "$sdir" "$hmnt"
		"$bin" --verify --result "$rds" > "$tmp/handv" 2>&1
		st=$?
		[ $st -eq 0 ] || \
		    { cat "$tmp/handv"; fail "--verify exited $st, want 0"; }
		grep -q "the resolution: done $nconf, first " "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "--verify does not call every choice done"; }
		echo "ok   $case_id: $nconf name$(sfx "$nconf") ${kind}'s, verified by that side"
		;;
	esac
	end_case
}

# --- 5. --restart under a --take record -----------------------------
case_restart() {
	case_id="$fixture $form --restart under a --take record"
	fresh --take-onto --no-merge
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$log"; fail "--take-onto --no-merge exited $st, want 1"; }
	answered_all_as "$res" onto "$nconf"
	# Somebody answers it another way afterwards. That is what a
	# restart discards; the instruction the rebase was started
	# with is what it puts back.
	rechoose "$res" onto keep
	[ "$(grep -c ' keep$' "$res" || true)" = "$nconf" ] || \
	    { cat "$res"; fail "the re-answering did not take"; }
	"$bin" --restart --result "$rds" > "$tmp/restart" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/restart"; fail "--restart exited $st, want 0"; }
	# The document is the run's own again, and a complete document
	# and a command that says to go on is the whole of the signal,
	# so the restart went through the gate to done by itself.
	answered_all_as "$res" onto "$nconf"
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	for p in $names; do
		same_as "$ontodir" "$hmnt" "$p"
	done
	again "$tmp/again" "$hmnt" "$want_conf"
	echo "ok   $case_id: the skeleton came back answered onto and went on"
	end_case
}

# --- 6. drift lines at the gate, kept and flipped -------------------
# The edit is to a name the manifest says nothing about, so nothing
# but the second pass of the verify can see it, and what the gate
# does with it is write it into the document rather than into the
# tree.
drift_line() {			# leaves $kept edited and its line written
	kept=$(kept_name "$man" "$hmnt")
	[ -n "$kept" ] || fail "the fixture has no untouched file to edit"
	names0=$(res_names "$res")
	[ -n "$names0" ] || { head -8 "$res"; fail "$res has no #names"; }
	ro_off
	printf 'drift\n' >> "$hmnt$kept" || fail "cannot edit $kept"
	ro_back
	# The conflicts are still unanswered, so the gate writes the
	# line and waits: a document written to is not a document
	# answered.
	"$bin" --continue --verify --result "$rds" > "$tmp/dr1" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/dr1"; fail "--continue --verify at the gate exited $st, want 1"; }
	grep -q '1 drift line added to the resolution' "$tmp/dr1" || \
	    { cat "$tmp/dr1"; fail "the gate added no drift line"; }
	leaf=$(basename "$kept")
	grep -q "^ *$leaf drift keep\$" "$res" || \
	    { cat "$res"; fail "$res has no drift line for $kept"; }
	[ "$(res_names "$res")" = "$((names0 + 1))" ] || \
	    { head -8 "$res"; fail "#names is $(res_names "$res"), want $((names0 + 1))"; }
	# A keep is an answer, so the count of what is unanswered is
	# the conflicts and nothing else, and the gate is still shut.
	[ "$(res_left "$res")" = "$nconf" ] || \
	    { head -8 "$res"; fail "a drift keep line changed what is unanswered"; }
	[ "$(statenow "$rds")" = conflicts ] || \
	    fail "the gate moved on an unanswered document"
	return 0
}

case_driftkeep() {
	case_id="$fixture $form a drift line written and kept"
	at_conflicts
	drift_line
	answer_all "$res" keep
	"$bin" --continue --result "$rds" > "$tmp/dr2" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/dr2"; fail "--continue over the answered document exited $st, want 0"; }
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	grep -q drift "$hmnt$kept" || fail "a verb wrote over the edit to $kept"
	# The name is the resolution's now, so it is in no list of
	# names outside the manifest and a keep is never compared.
	"$bin" --verify -v --result "$rds" > "$tmp/dr3" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/dr3"; fail "--verify exited $st, want 0"; }
	no_outside "$tmp/dr3" || \
	    { cat "$tmp/dr3"; fail "the drift is still outside the manifest"; }
	grep -q "^zfs_rebase:     $kept keep done\$" "$tmp/dr3" || \
	    { cat "$tmp/dr3"; fail "$kept is not under the resolution as keep"; }
	echo "ok   $case_id: $kept is a drift keep line and the edit stands"
	end_case
}

case_driftflip() {
	case_id="$fixture $form a drift line flipped to onto"
	at_conflicts
	drift_line
	# The picker's other answer: onto puts the name back as onto
	# had it, which is what the person asks for when the edit was
	# a mistake.
	rechoose "$res" "drift keep" "drift onto"
	grep -q "^ *$leaf drift onto\$" "$res" || \
	    { cat "$res"; fail "the flip to onto did not take"; }
	answer_all "$res" keep
	"$bin" --continue --result "$rds" > "$tmp/df2" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/df2"; fail "--continue over the flipped document exited $st, want 0"; }
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	grep -q drift "$hmnt$kept" && \
	    fail "the choice onto left the edit to $kept standing"
	same_as "$ontodir" "$hmnt" "$kept"
	"$bin" --verify -v --result "$rds" > "$tmp/df3" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/df3"; fail "--verify exited $st, want 0"; }
	grep -q "^zfs_rebase:     $kept onto done\$" "$tmp/df3" || \
	    { cat "$tmp/df3"; fail "$kept is not under the resolution as onto done"; }
	echo "ok   $case_id: $kept is onto's again"
	end_case
}

# --- 7. the two kills -----------------------------------------------
# The window between the two documents: the manifest is written and
# recorded and the skeleton is not.
case_killwindow() {
	how=$1
	case_id="$fixture $form SIGKILL between the two documents, then --$how"
	fresh_bg manifest
	kill -KILL "$pid" || fail "cannot signal the stopped tool"
	wait "$pid"
	st=$?
	pid=
	[ "$st" -eq 137 ] || { cat "$log"; fail "the kill left exit $st, want 137"; }
	[ -f "$man" ] || fail "no manifest at $man"
	[ -e "$res" ] && fail "a resolution at $res before the skeleton was written"
	[ -z "$(statenow "$rds")" ] || \
	    fail "the state is '$(statenow "$rds")', want none"
	[ "$(holdcount)" = 3 ] || fail "$(holdcount) holds after the kill, want 3"
	[ "$(recsrc zfs_rebase:resolution "$rds")" = local ] && \
	    fail "the record names a resolution the run never wrote"
	if [ "$form" = dataset ]; then
		mounted_at "$rundir/mnt" || \
		    fail "onto is not at the private mount $rundir/mnt"
	fi
	if [ "$how" = restart ]; then
		# --restart writes the skeleton again from the
		# recorded manifest, which is what makes it the way
		# out of this window.
		"$bin" --restart --result "$rds" > "$tmp/kw" 2>&1
		st=$?
		[ $st -eq 1 ] || \
		    { cat "$tmp/kw"; fail "--restart exited $st, want 1"; }
		[ -f "$res" ] || fail "--restart wrote no resolution at $res"
		[ "$(res_left "$res")" = "$nconf" ] || \
		    { head -8 "$res"; fail "--restart did not write a whole skeleton"; }
		[ "$(statenow "$rds")" = conflicts ] || \
		    fail "the restart is at '$(statenow "$rds")', want conflicts"
		echo "ok   $case_id: the skeleton was written again and the gate is shut"
		# One thing --restart does not do here is record the path
		# it just wrote to, since the record has carried no
		# zfs_rebase:resolution since the kill: reset_resolution
		# writes the file and no property. --abort then has
		# nothing to unlink, and the file it leaves keeps the run
		# directory from going. That is a finding for the author
		# and not this harness's to mend, so the file is taken
		# away here and the case says so; if the record does name
		# it, the tool has been fixed and --abort takes it.
		if [ "$(recsrc zfs_rebase:resolution "$rds")" = local ]; then
			echo "     and the record names it, so --abort takes it"
		else
			echo "note $case_id: --restart wrote $res and recorded no"
			echo "     path for it, so --abort cannot unlink it; the"
			echo "     harness does, to leave the pool as it found it"
			rm -f "$res"
		fi
	else
		echo "ok   $case_id: manifest yes, resolution no, three holds"
	fi
	end_case
}

# And a kill inside applying2, while the choices are being carried
# out. --take-from is what makes there be something to carry out: a
# conflicted name holds onto's object when the stage begins, so
# choosing onto is already true everywhere and the stage would reach
# no line at all.
case_killchoice() {
	case_id="$fixture $form SIGKILL inside applying2's choices"
	fresh_bg choice:1 --take-from --no-gui
	kill -KILL "$pid" || fail "cannot signal the stopped tool"
	wait "$pid"
	st=$?
	pid=
	[ "$st" -eq 137 ] || { cat "$log"; fail "the kill left exit $st, want 137"; }
	[ "$(statenow "$rds")" = applying2 ] || \
	    fail "the state is '$(statenow "$rds")', want applying2"
	[ "$(holdcount)" = 3 ] || fail "$(holdcount) holds after the kill, want 3"
	[ "$(recval readonly "$rds")" = off ] || \
	    fail "readonly is on after a kill inside an applying stage"
	if [ "$form" = dataset ]; then
		mounted_at "$rundir/mnt" || \
		    fail "onto is not at the private mount $rundir/mnt"
	fi
	# There is no gate left for --no-merge to stop at from here.
	"$bin" --continue --no-merge --result "$rds" > "$tmp/kc0" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/kc0"; fail "--no-merge at applying2 exited $st, want 2"; }
	grep -q 'past the merge' "$tmp/kc0" || \
	    { cat "$tmp/kc0"; fail "the refusal does not say past the merge"; }
	[ "$(statenow "$rds")" = applying2 ] || \
	    fail "the refusal moved the state"
	# The stage begins again over the whole document, which is
	# idempotent, and its own second pass must find nothing left:
	# a line it had to change would fail the run here.
	"$bin" --continue --result "$rds" > "$tmp/kc1" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/kc1"; fail "--continue after the kill exited $st, want 0"; }
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	for p in $names; do
		same_as "$fromdir" "$hmnt" "$p"
	done
	pooled_like "$fromdir" "$hmnt"
	# And nothing at all is left to do: the same call again reads
	# every line of the document done.
	"$bin" --continue --verify --result "$rds" > "$tmp/kc2" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/kc2"; fail "--continue at done exited $st, want 0"; }
	grep -q "the resolution: done $nconf, first " "$tmp/kc2" || \
	    { cat "$tmp/kc2"; fail "a choice is not done after the redo"; }
	grep -q "the resolution: drifted 0\$" "$tmp/kc2" || \
	    { cat "$tmp/kc2"; fail "a choice drifted after the redo"; }
	grep -q "the resolution: pending 0\$" "$tmp/kc2" || \
	    { cat "$tmp/kc2"; fail "a choice is still pending after the redo"; }
	echo "ok   $case_id: applying2 kept, redone whole, done, nothing left"
	end_case
}

# --- 8. the ACL strip under a choice --------------------------------
# A directory that is already there is the one thing a choice
# rewrites in place rather than making anew, so it is the one place
# an ACL the chosen side does not have has to be taken off rather
# than overwritten. The stage's own second pass is what says whether
# it was: a directory that still carried the ACL would be changed
# again on that pass and the run would fail at applying2.
case_aclstrip() {
	case_id="$fixture $form an ACL on a clean directory, chosen onto"
	at_conflicts
	kdir=$(kept_dir "$man" "$hmnt")
	if [ -z "$kdir" ]; then
		echo "skip $case_id: the fixture has no untouched directory"
		end_case
		return 0
	fi
	ro_off
	setfacl -a 0 'user:1:rwxp--aARWcCos:------:allow' "$hmnt$kdir" || \
	    { ro_back; fail "cannot put an ACL on $kdir"; }
	ro_back
	[ "$(aclof "$hmnt$kdir")" = "$(aclof "$ontodir$kdir")" ] && \
	    fail "the ACL on $kdir did not change anything"
	"$bin" --continue --verify --result "$rds" > "$tmp/ac1" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/ac1"; fail "--continue --verify at the gate exited $st, want 1"; }
	leaf=$(basename "$kdir")
	grep -q "^ *$leaf/ drift keep\$" "$res" || \
	    { cat "$res"; fail "$res has no drift line for the directory $kdir"; }
	rechoose "$res" "drift keep" "drift onto"
	answer_all "$res" keep
	"$bin" --continue --result "$rds" > "$tmp/ac2" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/ac2"; fail "--continue over the flipped document exited $st, want 0"; }
	[ "$(statenow "$rds")" = done ] || \
	    fail "the state is '$(statenow "$rds")', want done"
	same_as "$ontodir" "$hmnt" "$kdir"
	echo "ok   $case_id: $kdir is stripped back to onto's, and the second pass saw nothing"
	end_case
}

# ---------------------------------------------------------------
# One fixture in one form: the cases above, each ending in --abort.
# The ones that want more of a fixture than it has say so and are
# passed over.
# ---------------------------------------------------------------
res_pass() {
	form=$1
	if [ "$form" = clone ]; then
		rds=$POOL/result
		hmnt=/var/db/zfs_rebase/$POOL/result/mnt
		ontodir=$MNT/onto/.zfs/snapshot/work
	else
		rds=$POOL/onto
		hmnt=$MNT/onto
		ontodir=$MNT/onto/.zfs/snapshot/pre
	fi
	fromdir=$MNT/from/.zfs/snapshot/work
	rundir=/var/db/zfs_rebase/$rds
	man=$rundir/manifest
	res=$rundir/resolution
	log=$tmp/pass.log
	say "$fixture, the $form form"
	case_headless onto
	case_headless from
	case_nomerge
	case_incomplete
	# A choice of each kind by hand wants a document with more
	# than one line in it, so that answering is an edit to a file
	# and not to a single field.
	if [ "$nconf" -gt 1 ]; then
		case_hand keep
		case_hand onto
		case_hand from
	else
		echo "skip $fixture $form the hand-edited choices: one"
		echo "     conflicted name, and the case wants two"
	fi
	case_restart
	# A drift line is a clean name the manifest says nothing
	# about; a fixture whose every name is conflicted has none.
	if [ -n "$haskept" ]; then
		case_driftkeep
		case_driftflip
	else
		echo "skip $fixture $form the drift lines: every name of this"
		echo "     fixture is the manifest's or a conflict's"
	fi
	case_killwindow abort
	case_killwindow restart
	case_killchoice
	if [ -n "$hasdir" ]; then
		case_aclstrip
	else
		echo "skip $fixture $form the ACL strip: no untouched directory"
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
	[ "$want_conf" = 0 ] && \
	    { echo "FAIL: $fixture declares no conflict; this harness is the"; \
	      echo "      resolution's and every fixture it takes must have one"; \
	      exit 1; }
	# The names the skeleton will have a line for, read off the
	# fixture's own expect block, which is a manifest.
	names=$(conflict_names "$fdir/expect")
	nconf=$(printf '%s\n' "$names" | grep -c .)
	[ "$nconf" -gt 0 ] || { echo "FAIL: $fixture marks no name conflict"; exit 1; }
	# What the fixture has beyond its conflicts, which decides
	# which cases can run: a file no action names, and a directory
	# no action names.
	haskept=$(kept_name "$fdir/expect" "$fdir/onto")
	hasdir=$(kept_dir "$fdir/expect" "$fdir/onto")
	make_pool
	res_pass clone
	res_pass dataset
	drop_pool
	echo "ok   $fixture: the resolution carried out both ways, in both forms"
}

for f in $fixtures; do
	one_fixture "$f"
done
echo "run-resolution: $cases cases passed"
exit 0
