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
# carried out for the first time, where the two --take flags and
# --no-merge are exercised on real datasets, and where a kill lands
# inside the applying2 stage. What it asserts, case by case:
#
# 1. Headless to done. A fresh run given --take-onto writes its
#    skeleton answered, which makes it complete from the start, so
#    the run passes its own conflicts gate and reaches done in one
#    process (exit 0, not 1). The manifest's header then reads
#    #take onto; the document has nothing left to
#    answer and every line reads onto; every conflicted name in the
#    result is onto's object at that name -- type, mode, ownership,
#    bytes or link target, the ACL and both namespaces of extended
#    attributes -- or is gone where onto has no such name; the names
#    of one group that onto pools together are one object here too;
#    the clean names are untouched, which a second --posix rebase
#    declaring no action says; and the run's own final check, which
#    is standard and asks for no flag, reports every line of the
#    resolution done.
#    Then the same with --take-from.
#    Cells: ZX122, ZX125, ZX143, ZA56, ZA57 (on an acl fixture), ZM83.
#
# 2. --no-merge. The same run given --no-merge stops at the gate with
#    the document complete, since the flag is the command saying not
#    yet; a --continue --no-merge stops there again and moves
#    nothing; a plain --continue then passes the gate and reaches
#    done; and a --continue on the result afterwards is refused
#    before any stage runs -- exit 2, "not a zfs_rebase result",
#    readonly unmoved -- because done took the record off. ("past
#    the merge" is what a record at applying2 is told, which case 7
#    shows.) Cells: ZX123, ZX126, ZX143, ZX144.
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
#    into the document, and at done the hand merge stands and the
#    final check reports the name under the resolution as keep and
#    never as drift. onto and from: the name is that side's object at done,
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
#    the gate; the --continue checks there under no flag, writes it
#    into the resolution as a drift line with the choice keep and
#    then stops, because the conflicts are still unanswered and a
#    document written to is not a document answered. Answering them takes the rebase to done
#    with the edit intact and the name the resolution's. The second
#    half is the same up to the drift line and then flips its choice
#    to onto: at done the name is back as onto had it. run-strays.sh
#    case 5 is the neighbouring case -- there the conflicts are
#    answered before the --continue, so the gate writes the
#    line and passes in one command; here it writes and waits.
#    Cells: ZX132, ZX133 (ZY94 is run-strays.sh's).
#
# 7. Kills. The pause hook (tests/box/README.md) has two gates this
#    script is the only user of. At "manifest" the decision is
#    written, the phase says "decided" and the skeleton is not
#    written yet: the one window in which a rebase has one of its two
#    documents, and a SIGKILL there leaves a rebase whose exits are
#    --restart, which writes the skeleton again from the recorded
#    manifest, and --abort. At
#    "choice:1" the applying2 stage is part way through carrying the
#    choices out: a SIGKILL there leaves applying2 with readonly off,
#    --no-merge is refused from there, and --continue redoes the
#    whole document -- which is idempotent -- and reaches done with
#    nothing left for a second pass to do. Cells: ZX134, ZX135,
#    ZA56, ZX126, ZX207.
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
# 9. The blocked directory line. A directory the person makes in the
#    result with a file in it becomes two drift lines at the gate;
#    the directory is flipped to onto, which has no such name, and
#    the file under it is left at keep. The removal cannot be made --
#    the kept name holds the directory open -- and the apply's
#    pre-scan is what knows it, so nothing is asked of the disk and
#    the run does not die on an ENOTEMPTY. The check after the
#    choices passes it and the done gate is where it is counted: exit
#    3, done reached all the same, the directory and its file still
#    there, and the line written back as "-", which is what the done
#    gate records of a choice that was not carried out.
#    Cells: ZA64, ZY105, ZY108.
#
# 10. The resolution as the authority. A conflict line the manifest
#    marks that a hand edit removed is put back by the next gate with
#    the take mode's answer -- onto under --take-onto, from under
#    --take-from, and "-" where the run was given neither -- and the
#    header counts move with it: a hand edit cannot take a conflict
#    away by deleting the line that speaks for it. And a conflict
#    line for a name the manifest never marked is the person's own
#    instruction, carried out like a drift line with that choice,
#    its group number of no record never read.
#    Cells: ZY106, ZY107, ZA66.
#
# Every case ends by taking the rebase away -- --abort where one is
# still open, and by hand where it reached done, since done takes the
# record off and leaves --abort nothing to find -- and the pool is
# proved to be the fixture again before the next one starts. One pool
# per fixture, built and destroyed here, so the script runs alone.
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
recsrc() { zfs get -H -o source "$1" "$2" 2>/dev/null; }
# The phase, with "" for a record that has passed no gate yet and for
# a result with no record at all, which is what done leaves. The
# pool root carries a bogus phase, so the value counts only where it
# is the dataset's own: an inherited one is no record.
phasenow() {
	v=""
	[ "$(recsrc zfs_rebase:phase "$1")" = local ] && \
	    v=$(recval zfs_rebase:phase "$1")
	printf '%s' "$v"
}
# One line of a manifest's header, which is where the rebase's
# identity lives: #take above all here, which says how the skeleton
# was answered when it was written and which --restart reads back.
hdr() { sed -n "s/^#$1 //p" "$2"; }
# Nothing of a rebase left on the result, which is what done leaves
# and the only thing that says a rebase reached it.
at_done() {
	[ -z "$(localprops "$1")" ] || \
	    fail "the rebase reached done and left $(localprops "$1")"
	sethere
}
# A clone whose rebase reached done is unmounted with its mountpoint
# property still none, which is the void the tool hands it to;
# placing it is the user's work, and the tool's last line says how.
# A case that reaches done and looks again finds the clone where
# this placed it the first time, mountpoint and all: that is the
# harness's own placement standing, not the tool's doing.
place_clone() {
	mp=$(recval mountpoint "$POOL/result")
	if mounted_at "$MNT/result"; then
		[ "$mp" = "$MNT/result" ] || \
		    fail "the placed clone's mountpoint is $mp, want $MNT/result"
		return 0
	fi
	[ "$mp" = none ] || fail "a settled clone's mountpoint is $mp, want none"
	[ "$(recval mounted "$POOL/result")" = no ] || \
	    fail "a settled clone is mounted somewhere else"
	zfs set mountpoint="$MNT/result" "$POOL/result" || \
	    fail "cannot place the settled clone"
	mounted_at "$MNT/result" || \
	    fail "placing the clone did not mount it at $MNT/result"
	return 0
}
# Where the result's tree is to be read or edited just now, with the
# assertion that it is there, and where its own snapshots are read
# through in the dataset form. An open rebase holds the result at the
# run's private mount in both forms, the conflicts gate included: a
# half rebased tree is not handed back into service while its
# conflicts wait to be answered (documents-design.md, section 5).
# done puts the dataset home and leaves the clone in the void.
sethere() {
	if [ -n "$(localprops "$rds")" ]; then
		hmnt=$rundir/mnt
		mounted_at "$hmnt" || \
		    fail "an open rebase does not hold $rds at $hmnt"
		[ "$form" = clone ] || \
		    [ "$(recval canmount "$POOL/onto")" = noauto ] || \
		    fail "canmount is not noauto while the rebase is open"
	elif [ "$form" = dataset ]; then
		hmnt=$MNT/onto
		mounted_at "$hmnt" || fail "done left onto away from home"
		[ "$(recval canmount "$POOL/onto")" = on ] || \
		    fail "done did not put canmount back to on"
	else
		place_clone
		hmnt=$MNT/result
	fi
	# In the dataset form the recorded onto is a snapshot of the
	# result itself, so it is read through .zfs under wherever the
	# result is mounted just now and never through the mountpoint
	# property. In the clone form onto is another dataset, at home
	# throughout.
	[ "$form" = clone ] || ontodir=$hmnt/.zfs/snapshot/pre
	return 0
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

# One conflicted name whose line scopes nothing, so that taking that
# line out by hand leaves the tree section well formed: a directory
# line is followed by the two dots that close it, and removing the
# line alone would close the root early.
leaf_conflict() {
	actions "$1" | awk '$2 == "conflict" && $3 == 0 { print $1; exit }'
}

# A file at the root of the tree at $2 that the manifest at $1 says
# nothing about: the one shape a line can be added by hand without
# opening a scope for it.
kept_top() {
	actions "$1" | awk '{ print $1 }' | sort -u > "$tmp/acted"
	(cd "$2" && find . -maxdepth 1 -type f -links 1 | sed 's/^\.//') | \
	    sort > "$tmp/tops"
	comm -23 "$tmp/tops" "$tmp/acted" | head -1
}

# One line taken out by hand, with both header counts moved with it,
# which is what the parser demands of any hand edit. The count of
# what is unanswered moves only if the line that went was a "-".
drop_line() {			# FILE PATH
	leaf=$(basename "$2")
	awk -v leaf="$leaf" '
	{ lines[NR] = $0 }
	/^#names / { names = $2 }
	/^#unanswered / { unans = $2 }
	$1 == leaf && $2 == "conflict" { gone = NR; if ($NF == "-") u = 1 }
	END {
		if (!gone) exit 1
		for (i = 1; i <= NR; i++) {
			if (i == gone) continue
			if (lines[i] ~ /^#names /) {
				print "#names " names - 1
				continue
			}
			if (lines[i] ~ /^#unanswered /) {
				print "#unanswered " unans - u
				continue
			}
			print lines[i]
		}
	}' "$1" > "$1.new" || fail "no conflict line for $2 in $1"
	mv "$1.new" "$1" || fail "cannot rewrite $1"
}

# And one line put in by hand, at the root's own scope, with #names
# moved with it. The choice is made on it, so nothing is added to
# what is unanswered.
add_line() {			# FILE PATH GROUP CHOICE
	leaf=$(basename "$2")
	n=$(res_names "$1")
	awk -v leaf="$leaf" -v grp="$3" -v ch="$4" -v n="$((n + 1))" '
	/^#names / { print "#names " n; next }
	{ print }
	$1 == "/" && !added { print "    " leaf " conflict " grp " " ch; added = 1 }' \
	    "$1" > "$1.new" || fail "cannot add a line to $1"
	mv "$1.new" "$1" || fail "cannot add a line to $1"
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
	# local value, and a dataset that only inherits them is no
	# result of ours.
	zfs set zfs_rebase:tag=bogus "$POOL" || exit 2
	zfs set zfs_rebase:manifest=/nonexistent/manifest "$POOL" || exit 2
	zfs set zfs_rebase:phase=bogus "$POOL" || exit 2
}

drop_pool() {
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	MD=
	rm -f "$IMG"
	rmdir "$MNT" 2>/dev/null
}

# The end of a case: --abort where a rebase is still open, and by
# hand where it reached done, since done takes the record off and
# leaves --abort nothing to find. What done leaves by hand is the
# result alone: the run directory went with it, and the case asserts
# that. Then the proof that the pool is the fixture again with no
# rebase left in it.
end_case() {
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
			zfs rollback -r "$POOL/onto@pre" || \
			    fail "cannot roll onto back to @pre"
			zfs destroy "$POOL/onto@pre" || \
			    fail "cannot destroy @pre"
		fi
		[ ! -d "$rundir" ] || \
		    fail "done left the run directory $rundir"
	fi
	[ "$(holdcount)" = 0 ] || fail "the end of the case left holds behind"
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
	[ "$(recval canmount "$POOL/onto")" = on ] || \
	    fail "--abort left onto at canmount noauto"
	# The -o pair is the user's and neither done nor --abort takes
	# it away, so the harness does: the next case asserts what its
	# own run wrote and not what the last one left. case 7 asks
	# outright that there be no resolution before the skeleton.
	[ -f "$man" ] || fail "the end of the case has no -o manifest at $man"
	rm -f "$man" "$res"
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
		"$bin" $flag -v -o "$man" "$@" --off-of "$POOL/from@work" \
		    --onto "$POOL/onto@work" --result "$POOL/result" \
		    > "$log" 2>&1
	else
		"$bin" $flag -v -o "$man" "$@" --from "$POOL/from@work" \
		    --onto "$POOL/onto" --result pre > "$log" 2>&1
	fi
}
fresh_bg() {
	gate=$1
	shift
	if [ "$form" = clone ]; then
		ZFS_REBASE_PAUSE=$gate "$bin" $flag -v -o "$man" "$@" \
		    --off-of "$POOL/from@work" --onto "$POOL/onto@work" \
		    --result "$POOL/result" > "$log" 2>&1 &
	else
		ZFS_REBASE_PAUSE=$gate "$bin" $flag -v -o "$man" "$@" \
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
	[ "$(phasenow "$rds")" = conflicts ] || \
	    fail "the run is at '$(phasenow "$rds")', want conflicts"
	[ -f "$res" ] || fail "the run wrote no resolution at $res"
	sethere
	return 0
}
# The result writable for one edit made behind the tool's back, and
# read-only again after it, as run-strays.sh does. Only the clone
# form has readonly on to take off: the dataset form's private mount
# is writable for its whole life (private_rw in run.c), and a
# readonly flip on it is what must not be made -- libzfs answers one
# with a remount at the mountpoint property, where nothing is mounted
# while the dataset sits at the private mount, and the kernel says
# EINVAL (the box, 2026-09-07).
ro_off() {
	ro0=$(recval readonly "$rds")
	[ "$ro0" = on ] || return 0
	zfs set readonly=off "$rds" || fail "readonly=off"
}
ro_back() {
	[ "$ro0" = on ] || return 0
	zfs set readonly=on "$rds" || fail "readonly=on"
}
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
	# The final check is made by the invocation that reaches the
	# done gate, under no flag, and this run is it. Afterwards
	# there is no record to ask anything of.
	fresh "--take-$side"
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$log"; fail "--take-$side exited $st, want 0"; }
	# One process: the gate said the document was complete and
	# went on rather than waiting for a --continue.
	grep -q "the resolution $res is answered in full; going on" "$log" || \
	    { cat "$log"; fail "the run did not pass its own conflicts gate"; }
	at_done "$rds"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	# How the skeleton was answered when it was written is the
	# header's #take line now, and it is what --restart reads back.
	[ "$(hdr take "$man")" = "$side" ] || \
	    fail "#take is $(hdr take "$man"), want $side"
	# The resolution is beside the manifest by rule and by no
	# property: beside a -o FILE that is FILE.resolution, which is
	# the path the run named in its own message above.
	[ "$res" = "$man.resolution" ] || \
	    fail "the resolution is not beside the manifest"
	# And the pair is the user's: done unlinked neither, because
	# neither was in the run directory it took away.
	[ -f "$man" ] || fail "done removed the -o manifest $man"
	[ -f "$res" ] || fail "the run wrote no resolution at $res"
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
	# And the run's own final check says the same of the document:
	# every onto or from line held against that side and found
	# done, reported at the done gate before anything was released.
	grep -q "the resolution: done $nconf, first " "$log" || \
	    { cat "$log"; fail "the final check does not call every choice done"; }
	for k in pending blocked drifted unchecked; do
		grep -q "the resolution: $k 0\$" "$log" || \
		    { cat "$log"; fail "the final check reports a $k choice"; }
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
	[ "$(phasenow "$rds")" = conflicts ] || \
	    fail "the run is at '$(phasenow "$rds")', want conflicts"
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
	[ "$(phasenow "$rds")" = conflicts ] || \
	    fail "--continue --no-merge moved the gate"
	# And without it the same document takes the same rebase on.
	"$bin" --continue --result "$rds" > "$tmp/nm3" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/nm3"; fail "--continue exited $st, want 0"; }
	at_done "$rds"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	for p in $names; do
		same_as "$ontodir" "$hmnt" "$p"
	done
	# And a rebase that reached done is past every gate: its record
	# is off, so a --continue -- with the flag or without it --
	# finds no rebase at all and is refused before any stage runs.
	# ("past the merge" is what a record at applying2 is told, and
	# case 7 is where that is shown.)
	ro0=$(recval readonly "$rds")
	"$bin" --continue --no-merge --result "$rds" > "$tmp/nm4" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/nm4"; fail "--no-merge at done exited $st, want 2"; }
	grep -q 'not a zfs_rebase result' "$tmp/nm4" || \
	    { cat "$tmp/nm4"; fail "the refusal does not say there is no record"; }
	at_done "$rds"
	[ "$(recval readonly "$rds")" = "$ro0" ] || \
	    fail "the refusal flipped readonly"
	echo "ok   $case_id: held at the gate twice, passed once, and a"
	echo "     settled rebase is no rebase to continue"
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
	[ "$(phasenow "$rds")" = conflicts ] || \
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
	# The report is the done gate's own, made by the --continue
	# that reaches it: the check comes after the choices were
	# carried out and before the holds and the record go, under no
	# flag, and -v is what adds the per-line list to it. There is
	# no asking afterwards -- a settled result carries no record --
	# until verify-settled names it by its manifest.
	"$bin" --continue -v --result "$rds" \
	    > "$tmp/handv" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/handv"; fail "--continue over $kind exited $st, want 0"; }
	at_done "$rds"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	case "$kind" in
	keep)
		grep -q mine "$hmnt$edited" || \
		    fail "the choice keep did not leave the hand merge alone"
		# The name is the resolution's now: it is under the
		# document with the choice the person made, it is no
		# drift, and it is in no list of names outside the
		# manifest.
		grep -q "^zfs_rebase:     $edited keep done\$" "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "$edited is not under the resolution as keep"; }
		grep -q "the resolution: drifted 0\$" "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "a kept name is reported as drift"; }
		no_outside "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "a kept name reached the name list"; }
		grep -q mine "$hmnt$edited" || fail "the check wrote over the merge"
		echo "ok   $case_id: $edited stands, and is the resolution's"
		;;
	*)
		sdir=$(sidedir "$kind")
		[ -d "$sdir" ] || fail "$sdir is not there"
		for p in $names; do
			same_as "$sdir" "$hmnt" "$p"
		done
		pooled_like "$sdir" "$hmnt"
		grep -q "the resolution: done $nconf, first " "$tmp/handv" || \
		    { cat "$tmp/handv"; fail "the check does not call every choice done"; }
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
	at_done "$rds"
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
	"$bin" --continue --result "$rds" > "$tmp/dr1" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/dr1"; fail "--continue at the gate exited $st, want 1"; }
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
	[ "$(phasenow "$rds")" = conflicts ] || \
	    fail "the gate moved on an unanswered document"
	return 0
}

case_driftkeep() {
	case_id="$fixture $form a drift line written and kept"
	at_conflicts
	drift_line
	answer_all "$res" keep
	# The check goes on this --continue, which is the invocation
	# that reaches done: its report is the done gate's own, made
	# after the choices and before the record goes.
	"$bin" --continue -v --result "$rds" > "$tmp/dr3" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/dr3"; fail "--continue over the answered document exited $st, want 0"; }
	at_done "$rds"
	grep -q drift "$hmnt$kept" || fail "a verb wrote over the edit to $kept"
	# The name is the resolution's now, so it is in no list of
	# names outside the manifest and a keep is never compared.
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
	"$bin" --continue -v --result "$rds" > "$tmp/df3" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/df3"; fail "--continue over the flipped document exited $st, want 0"; }
	at_done "$rds"
	grep -q drift "$hmnt$kept" && \
	    fail "the choice onto left the edit to $kept standing"
	same_as "$ontodir" "$hmnt" "$kept"
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
	[ -e "$man.tmp" ] && fail "a .tmp left beside the manifest $man"
	# The decision is in place, and the phase says so: it goes down
	# the moment the manifest is renamed over the header the run
	# was born with, which is before the skeleton beside it.
	[ "$(phasenow "$rds")" = decided ] || \
	    fail "the phase is '$(phasenow "$rds")', want decided"
	[ "$(holdcount)" = 3 ] || fail "$(holdcount) holds after the kill, want 3"
	# The record is the manifest's path and the tag, and the
	# resolution is beside the manifest by rule: what the kill
	# leaves is the rule without the file, and --abort takes a
	# file that is not there in its stride.
	[ "$(recval zfs_rebase:manifest "$rds")" = "$man" ] || \
	    fail "the record does not name the manifest"
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
		[ "$(phasenow "$rds")" = conflicts ] || \
		    fail "the restart is at '$(phasenow "$rds")', want conflicts"
		echo "ok   $case_id: the skeleton was written again and the gate is shut"
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
	fresh_bg choice:1 --take-from
	kill -KILL "$pid" || fail "cannot signal the stopped tool"
	wait "$pid"
	st=$?
	pid=
	[ "$st" -eq 137 ] || { cat "$log"; fail "the kill left exit $st, want 137"; }
	[ "$(phasenow "$rds")" = applying2 ] || \
	    fail "the state is '$(phasenow "$rds")', want applying2"
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
	[ "$(phasenow "$rds")" = applying2 ] || \
	    fail "the refusal moved the state"
	# The stage begins again over the whole document, which is
	# idempotent, and its own second pass must find nothing left:
	# a line it had to change would fail the run here.
	"$bin" --continue --result "$rds" > "$tmp/kc1" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/kc1"; fail "--continue after the kill exited $st, want 0"; }
	at_done "$rds"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	for p in $names; do
		same_as "$fromdir" "$hmnt" "$p"
	done
	pooled_like "$fromdir" "$hmnt"
	# And nothing at all was left to do: the done gate's own final
	# check, made by that --continue before it released anything,
	# read every line of the document done. A rebase that reached
	# done has no record, so the same call again finds no rebase
	# and is refused before any stage runs.
	grep -q "the resolution: done $nconf, first " "$tmp/kc1" || \
	    { cat "$tmp/kc1"; fail "a choice is not done after the redo"; }
	grep -q "the resolution: drifted 0\$" "$tmp/kc1" || \
	    { cat "$tmp/kc1"; fail "a choice drifted after the redo"; }
	grep -q "the resolution: pending 0\$" "$tmp/kc1" || \
	    { cat "$tmp/kc1"; fail "a choice is still pending after the redo"; }
	"$bin" --continue --result "$rds" > "$tmp/kc2" 2>&1
	st=$?
	[ $st -eq 2 ] || \
	    { cat "$tmp/kc2"; fail "--continue on a settled result exited $st, want 2"; }
	grep -q 'not a zfs_rebase result' "$tmp/kc2" || \
	    { cat "$tmp/kc2"; fail "the refusal does not say there is no record"; }
	at_done "$rds"
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
	"$bin" --continue --result "$rds" > "$tmp/ac1" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/ac1"; fail "--continue at the gate exited $st, want 1"; }
	leaf=$(basename "$kdir")
	grep -q "^ *$leaf/ drift keep\$" "$res" || \
	    { cat "$res"; fail "$res has no drift line for the directory $kdir"; }
	rechoose "$res" "drift keep" "drift onto"
	answer_all "$res" keep
	"$bin" --continue --result "$rds" > "$tmp/ac2" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/ac2"; fail "--continue over the flipped document exited $st, want 0"; }
	at_done "$rds"
	same_as "$ontodir" "$hmnt" "$kdir"
	echo "ok   $case_id: $kdir is stripped back to onto's, and the second pass saw nothing"
	end_case
}

# --- 9. a directory line a keep holds open --------------------------
# The one removal a choice cannot ask for. The person makes a
# directory in the result with a file in it; the gate writes both as
# drift keep lines; the directory is flipped to onto, which has no
# such name, and the file under it is left the person's. The apply's
# pre-scan marks the removal blocked and skips it, so nothing is
# asked of the disk and no ENOTEMPTY reaches the run; the check after
# the choices passes it; and the done gate counts it -- exit 3, done
# all the same, and the line written back as "-".
case_blockeddir() {
	case_id="$fixture $form a directory line a keep holds open"
	at_conflicts
	ro_off
	mkdir "$hmnt/zrblocked" || { ro_back; fail "cannot make /zrblocked"; }
	printf 'mine\n' > "$hmnt/zrblocked/f" || \
	    { ro_back; fail "cannot make /zrblocked/f"; }
	ro_back
	"$bin" --continue --result "$rds" > "$tmp/bd1" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/bd1"; fail "--continue at the gate exited $st, want 1"; }
	grep -q "^ *zrblocked/ drift keep\$" "$res" || \
	    { cat "$res"; fail "$res has no drift line for the directory"; }
	grep -q "^ *f drift keep\$" "$res" || \
	    { cat "$res"; fail "$res has no drift line for /zrblocked/f"; }
	# onto has no such directory, and the file under it stays.
	sed 's|^\( *\)zrblocked/ drift keep$|\1zrblocked/ drift onto|' \
	    "$res" > "$res.new" || fail "cannot flip the directory line"
	mv "$res.new" "$res" || fail "cannot flip the directory line"
	answer_all "$res" keep
	"$bin" --continue -v --result "$rds" > "$tmp/bd2" 2>&1
	st=$?
	[ $st -eq 3 ] || \
	    { cat "$tmp/bd2"; fail "--continue over the blocked line exited $st, want 3"; }
	grep -q "the resolution: drifted 1, first /zrblocked\$" "$tmp/bd2" || \
	    { cat "$tmp/bd2"; fail "the done gate did not count the blocked line"; }
	grep -q 'done does not block on' "$tmp/bd2" || \
	    { cat "$tmp/bd2"; fail "the run did not say done was reached all the same"; }
	at_done "$rds"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	[ -d "$hmnt/zrblocked" ] || fail "the blocked directory went after all"
	grep -q mine "$hmnt/zrblocked/f" || \
	    fail "the kept name under the blocked directory did not stand"
	# What the done gate records of a choice not carried out.
	grep -q "^ *zrblocked/ drift -\$" "$res" || \
	    { cat "$res"; fail "the done gate did not write the line back as -"; }
	echo "ok   $case_id: it stayed, exit 3, done reached, the line reads -"
	# The harness made the directory, so the harness takes it away:
	# end_case proves the pool is the fixture again.
	ro_off
	rm -rf "$hmnt/zrblocked" || { ro_back; fail "cannot clear /zrblocked"; }
	ro_back
	end_case
}

# --- 10. the resolution as the authority ----------------------------
# A conflict line the manifest marks that a hand edit removed is put
# back by the next gate with the take mode's answer, and the header
# counts move with it. --no-merge holds the gate whatever the
# document says, so the check that writes runs and the case can then
# read what it wrote.
case_putback() {
	side=$1
	case_id="$fixture $form a conflict line removed, put back as $side"
	if [ "$side" = "-" ]; then
		at_conflicts
	else
		fresh "--take-$side" --no-merge
		st=$?
		[ $st -eq 1 ] || \
		    { cat "$log"; fail "--take-$side --no-merge exited $st, want 1"; }
		sethere
	fi
	one=$(leaf_conflict "$man")
	[ -n "$one" ] || fail "the fixture marks no conflict on a leaf"
	n0=$(res_names "$res")
	drop_line "$res" "$one"
	[ "$(res_names "$res")" = "$((n0 - 1))" ] || \
	    { head -8 "$res"; fail "the hand edit did not take"; }
	"$bin" --continue --no-merge --result "$rds" > "$tmp/pb" 2>&1
	st=$?
	[ $st -eq 1 ] || \
	    { cat "$tmp/pb"; fail "--continue at the gate exited $st, want 1"; }
	grep -q '1 conflict line the manifest marks put back' "$tmp/pb" || \
	    { cat "$tmp/pb"; fail "the gate did not put the line back"; }
	leaf=$(basename "$one")
	grep -q "^ *$leaf conflict [0-9][0-9]* $side\$" "$res" || \
	    { cat "$res"; fail "$one did not come back reading $side"; }
	[ "$(res_names "$res")" = "$n0" ] || \
	    { head -8 "$res"; fail "#names is $(res_names "$res"), want $n0"; }
	[ "$(phasenow "$rds")" = conflicts ] || \
	    fail "the gate moved while it was putting a line back"
	echo "ok   $case_id: $one came back reading $side"
	end_case
}

# And the other half: a conflict line for a name the manifest never
# marked, added by hand. It is the person's own instruction and is
# carried out like a drift line with that choice; the group number on
# it is of no record and is never read.
case_handadded() {
	case_id="$fixture $form a conflict line added by hand"
	at_conflicts
	top=$(kept_top "$man" "$hmnt")
	[ -n "$top" ] || fail "the fixture has no untouched file at the root"
	ro_off
	printf 'edited\n' >> "$hmnt$top" || { ro_back; fail "cannot edit $top"; }
	ro_back
	add_line "$res" "$top" 99 onto
	answer_all "$res" keep
	"$bin" --continue -v --result "$rds" > "$tmp/ha" 2>&1
	st=$?
	[ $st -eq 0 ] || \
	    { cat "$tmp/ha"; fail "--continue over the added line exited $st, want 0"; }
	at_done "$rds"
	[ "$(holdcount)" = 0 ] || fail "done left holds behind"
	same_as "$ontodir" "$hmnt" "$top"
	grep -q "^zfs_rebase:     $top onto done\$" "$tmp/ha" || \
	    { cat "$tmp/ha"; fail "$top is not under the resolution as onto done"; }
	echo "ok   $case_id: $top was carried out on the person's word alone"
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
	else
		rds=$POOL/onto
	fi
	rundir=/var/db/zfs_rebase/$rds
	# Where the result is while a rebase is open, which is where
	# every case starts; sethere moves both of these as the rebase
	# settles. In the clone form onto is a dataset of its own and
	# stays at home whatever the rebase does.
	hmnt=$rundir/mnt
	if [ "$form" = clone ]; then
		ontodir=$MNT/onto/.zfs/snapshot/work
	else
		ontodir=$hmnt/.zfs/snapshot/pre
	fi
	fromdir=$MNT/from/.zfs/snapshot/work
	# The two documents go where -o says. This harness reads them
	# after the rebase has reached done -- the header's #take, the
	# answered skeleton -- and done unlinks the two a run wrote
	# into its own directory, taking that directory with them. A -o
	# pair is the user's and stays, at done and at --abort alike
	# (documents-design.md, section 4). The no--o placement,
	# <rundir>/manifest and <rundir>/resolution, is
	# box/run-kills.sh's: it asserts both at every gate and their
	# absence at done.
	man=$tmp/manifest
	res=$man.resolution
	log=$tmp/pass.log
	prog_step "$fixture, the $form form"
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
	case_blockeddir
	case_putback onto
	case_putback from
	case_putback -
	if [ -n "$hastop" ]; then
		case_handadded
	else
		echo "skip $fixture $form the hand-added line: no untouched"
		echo "     file at the root of this fixture"
	fi
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
	hastop=$(kept_top "$fdir/expect" "$fdir/onto")
	make_pool
	res_pass clone
	res_pass dataset
	drop_pool
	echo "ok   $fixture: the resolution carried out both ways, in both forms"
}

nfx=0
for f in $fixtures; do nfx=$((nfx + 1)); done
prog_start $((nfx * 2)) "fixture forms"
for f in $fixtures; do
	one_fixture "$f"
done
echo "run-resolution: $cases cases passed"
exit 0
