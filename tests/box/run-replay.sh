#!/bin/sh
# Box harness: replay a fixture over sides edited in place, and hold
# the tool's unchanged count against what the fixture says it must be.
# FreeBSD, root, after make freebsd. Usage: run-replay.sh [FIXTURE.zrt]
# -- every fixture of both directories when none is named (KEEP=1 to
# leave the pool behind, which wants one fixture, since the next needs
# the pool name back).
#
# What this proves, and run-fixture.sh cannot
# -------------------------------------------
#
# The tool never asks ZFS what changed. It walks base and the two
# sides, and a side pool whose object number, generation number, ctime
# to the nanosecond, link count, type and name set are all base's is
# taken to be what base holds and is never read at all
# (sprints/sprint-5/string-audit.md section 2, src/yellow.c). That is
# the pruning, and "zfs_rebase: N pools unchanged" is the only thing
# that reports it.
#
# run-fixture.sh builds each side by clearing a clone and untarring the
# fixture's tree into it, so every object is newly made and N is zero:
# the rule is exercised there in its negative direction only, and a
# pruning that never fired would look exactly the same. Here each side
# is a clone of base edited in place by --edit-fixture, which touches
# only what the two trees disagree about, so real objects come through
# with real object numbers and real unmoved ctimes. N is then the count
# the fixture predicts, computed from the fixture alone by
# tools/replay-expect.py and committed in tests/box/replay-expect.txt:
# for each side, the pools that keep base's object with base's names
# and have nothing written or set on them, plus, for a directory, no
# entry of it made or unmade -- a directory keeps its inode when its
# children change but not its ctime. Matrix rows ZX1, ZX2 and ZC39.
#
# The blind spot, and the sleep
# -----------------------------
#
# ctime is stored at tick resolution: gethrestime is getnanotime on
# FreeBSD (include/os/freebsd/spl/sys/time.h), so an object changed
# inside the same tick as its previous change keeps the ctime it had,
# and the rule -- and zfs diff before it, which printed nothing for
# such an object either -- would call a changed object unchanged. The
# window is the tick the base objects were made in. So every side
# sleeps 1 past that tick before it is edited: every object the editor
# then touches takes a ctime the base snapshot cannot be holding, and
# a count that is too high is a real fault and not a race.
#
# What it depends on
# ------------------
#
#  1. --edit-fixture touching nothing the two trees agree on: not
#     opened, not chmod'd, not relinked (tests/fixtures/FORMAT.md,
#     "Editing a built tree"; tests/check_fixture.c asserts the inodes
#     and the ctimes on the Mac). If that guarantee slips, this harness
#     reports a count too low and blames the pruning. The first place
#     to look is the ACL: the editor holds the fixture's ACL text
#     against the walk's acl_t, and if the kernel gives back an ACL it
#     was handed as text in another form, no acl= pool will ever
#     compare equal and every freebsd/acl-*.zrt count here will be one
#     too high. sprints/sprint-5/issue-docs/fixture-edit.md says so and
#     leaves it to the box.
#  2. tests/box/replay-expect.txt being current with the fixtures and
#     with src/fixture.c. make check fails when it is stale, and
#     tools/replay-expect.py --check holds its model against the tool
#     itself on every fixture the Mac can build -- which is every one
#     but the eleven in freebsd/, whose rows are the model's word
#     alone until this harness runs. This script only checks that the
#     file has a row for every fixture on disk.
#  3. /tmp being a real filesystem, not tmpfs: the fixture's other two
#     trees are built there, and the freebsd/ fixtures carry NFSv4 ACLs
#     and system-namespace extended attributes that tmpfs has not got.
#     The pool image lives there too.
#
# Each fixture gets its own pool, made and destroyed here. The manifest
# is checked as run-fixture.sh checks it, from the #mode line on, so a
# pruned pool that was not really unchanged has to show up as a wrong
# manifest as well as a wrong count.
set -u
cd "$(dirname "$0")/../.." || exit 2
. tests/box/progress.sh
bin=./zfs_rebase
EXPECT=tests/box/replay-expect.txt
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
[ -f "$EXPECT" ] || { echo "no $EXPECT: make replay-expect"; exit 2; }

POOL=zrtreplay
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
MD=
tmp=
suite=1
fixtures=0
pruned=0

say() { printf '\n== %s\n' "$*"; prog_note "$*"; }
fail() { echo "FAIL: $*"; exit 1; }

teardown() {
	# A failed step can leave the result clone, its persistent holds
	# and its run directory; --abort is what gives back all three.
	"$bin" --abort --result "$POOL/result" >/dev/null 2>&1
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	MD=
	rm -f "$IMG"
	if [ -n "$tmp" ]; then
		# A built tree can carry the file flags, and rm(1) cannot
		# take an immutable name away until they are off.
		chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null
		rm -rf "$tmp"
	fi
	tmp=
	rmdir "$MNT" 2>/dev/null
	return 0
}

cleanup() {
	prog_end
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	teardown
}
trap cleanup EXIT

# The verbose line, which is the whole point of the harness.
unchanged() {
	got=$(sed -n 's/^zfs_rebase: \([0-9][0-9]*\) pools unchanged$/\1/p' \
	    "$1")
	[ -n "$got" ] || { cat "$1"; fail "$3: no unchanged line"; }
	[ "$got" = "$2" ] || \
	    { cat "$1"; fail "$3: $got pools unchanged, want $2"; }
}

# One fixture: FIXTURE, its from count, its onto count.
one() {
	fixture=$1
	fc=$2
	oc=$3
	want=$(($2 + $3))

	prog_step "$(basename "$fixture")"
	say "$fixture (from $fc, onto $oc, $want unchanged)"
	[ -f "$fixture" ] || fail "no such fixture"
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-replay.XXXXXX") || exit 2
	flag=""
	case "$fixture" in *-permissive.zrt) flag="-p" ;; esac

	truncate -s 512m "$IMG" || exit 2
	MD=$(mdconfig -a -t vnode -f "$IMG") || exit 2
	mkdir -p "$MNT"
	zpool create -m "$MNT" -O casesensitivity=sensitive \
	    -O normalization=none "$POOL" "/dev/$MD" || exit 2

	# base is born in the dataset. --build-fixture makes DIR/base,
	# DIR/from, DIR/onto and DIR/expect, so the base dataset is
	# mounted at $tmp/base for the length of the build and every
	# object of base is created by the builder inside it -- nothing
	# is copied in, which would have given each one a new object
	# number and a new ctime and left nothing to prune. The other
	# two trees land on /tmp, where they are only the expect block's
	# company.
	zfs create -o mountpoint="$tmp/base" "$POOL/base" || exit 2
	"$bin" --build-fixture "$fixture" "$tmp" || fail "build-fixture"
	[ -f "$tmp/expect" ] || fail "no expect block"
	zfs snapshot "$POOL/base@base" || exit 2
	zfs set mountpoint="$MNT/base" "$POOL/base" || exit 2

	for side in from onto; do
		zfs clone "$POOL/base@base" "$POOL/$side" || exit 2
		# past the tick the base objects were made in
		sleep 1
		"$bin" --edit-fixture "$fixture" "$side" "$MNT/$side" \
		    > "$tmp/edit-$side" || fail "edit-fixture $side"
		echo "     $side: $(cat "$tmp/edit-$side")"
	done
	zfs snapshot "$POOL/from@work" "$POOL/onto@work" || exit 2

	sed -n '/^#mode/,$p' "$tmp/expect" > "$tmp/expect.body"

	say "1. dry run"
	"$bin" -n $flag -v -o "$tmp/got-n" --from "$POOL/from@work" \
	    --onto "$POOL/onto@work" 2> "$tmp/err-n"
	st=$?
	sed -n '/^#mode/,$p' "$tmp/got-n" > "$tmp/got-n.body"
	cmp -s "$tmp/expect.body" "$tmp/got-n.body" || \
	    { diff "$tmp/expect.body" "$tmp/got-n.body" | head -20;
	      fail "dry-run manifest differs"; }
	grep -q "^#base $POOL/base@base\$" "$tmp/got-n" || \
	    { head -5 "$tmp/got-n"; fail "the dry run did not derive base"; }
	unchanged "$tmp/err-n" "$want" "the dry run"
	echo "ok   dry run (exit $st): manifest equal, $want pools unchanged"

	say "2. real run"
	"$bin" $flag -v -o "$tmp/got" --off-of "$POOL/from@work" \
	    --onto "$POOL/onto@work" --result "$POOL/result" 2> "$tmp/err"
	st=$?
	sed -n '/^#mode/,$p' "$tmp/got" > "$tmp/got.body"
	cmp -s "$tmp/expect.body" "$tmp/got.body" || \
	    { diff "$tmp/expect.body" "$tmp/got.body" | head -20;
	      fail "real-run manifest differs"; }
	grep -q "^#base $POOL/base@base\$" "$tmp/got" || \
	    { head -5 "$tmp/got"; fail "the real run did not derive base"; }
	unchanged "$tmp/err" "$want" "the real run"
	if grep -q '^#conflicts 0$' "$tmp/expect"; then
		[ $st -eq 0 ] || { cat "$tmp/err"; fail "clean fixture exited $st"; }
	else
		[ $st -eq 1 ] || \
		    { cat "$tmp/err"; fail "conflicted fixture exited $st, want 1"; }
	fi
	echo "ok   real run (exit $st): manifest equal, $want pools unchanged"

	# The positive proof. A fixture that leaves nothing alone is
	# still an assertion -- the count must be zero and not one --
	# but it is the fixtures with something left alone that say the
	# pruning fires at all on real snapshots.
	if [ "$want" -ne 0 ]; then
		echo "ok   the pruning fired: $want pools never read"
	fi

	say "3. abort"
	"$bin" --abort --result "$POOL/result" || fail "abort exited $?"
	for s in "$POOL/base@base" "$POOL/from@work" "$POOL/onto@work"; do
		held=$(zfs holds -H "$s") || fail "zfs holds $s"
		[ -z "$held" ] || fail "$s is still held after the abort"
	done
	echo "ok   aborted, holds released"

	fixtures=$((fixtures + 1))
	pruned=$((pruned + want))
	[ "${KEEP:-0}" = 1 ] || teardown
}

# The fixtures to run, as rows of the expectation file: it is the
# index, so a fixture with no row there is a fixture whose count
# nothing computed.
if [ $# -ge 1 ]; then
	suite=0
	arg=${1#./}
	rows=$(awk -v f="$arg" '$1 == f || $1 == "tests/fixtures/" f ||
	    $1 == "tests/fixtures/freebsd/" f' "$EXPECT")
	[ -n "$rows" ] || \
	    { echo "no row for $1 in $EXPECT: make replay-expect"; exit 2; }
else
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1 wants one fixture: the next run needs the"
		echo "pool name back, and $POOL would still be there"
		exit 2
	fi
	rows=$(cat "$EXPECT")
	ondisk=$(ls tests/fixtures/*.zrt tests/fixtures/freebsd/*.zrt |
	    wc -l | tr -d ' ')
	listed=$(grep -c . "$EXPECT")
	if [ "$ondisk" -ne "$listed" ]; then
		echo "$listed rows in $EXPECT, $ondisk fixtures on disk"
		fail "make replay-expect"
	fi
fi

# A here-document and not a pipe: a pipe would run the loop in a
# subshell, where the counters die and a fail would not stop the run.
prog_start "$(printf '%s\n' "$rows" | grep -c .)" fixtures
while read -r f fc oc; do
	[ -n "$f" ] || continue
	one "$f" "$fc" "$oc"
done <<ROWS
$rows
ROWS

say "summary"
echo "$fixtures fixtures replayed, $pruned pools pruned across the suite"
# Over the whole suite something must have been pruned, or the run
# says nothing at all about the rule it exists for. One fixture on its
# own is allowed to prune nothing: 40 of them leave nothing alone.
if [ "$suite" -eq 1 ] && [ "$pruned" -eq 0 ]; then
	fail "nothing was pruned anywhere: this harness proves nothing"
fi
exit 0
