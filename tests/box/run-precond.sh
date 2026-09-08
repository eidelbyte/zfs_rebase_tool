#!/bin/sh
# The box cells that are not fixtures. FreeBSD, root, after
# make freebsd. Usage: run-precond.sh   (KEEP=1 leaves the pool
# behind for inspection).
#
# run-fixture.sh proves what three trees and a manifest can say.
# Four things they cannot say are here: a nested mount inside an
# input, a dataset form onto whose canmount is off, the securelevel
# refusal, and a snapshot destroyed under a running rebase. This
# script closes the first two, prints the third as a procedure to run
# by hand, and says why the fourth has to wait.
#
# 1. A nested mount. The walk refuses any entry whose st_dev is not
#    the walk root's (src/walk.c, zw_entry: "nested mount at PATH"),
#    because a rebase that walked into a second filesystem would
#    decide over objects it can neither hold nor clone. The check
#    builds $POOL/from with a child dataset $POOL/from/inner mounted
#    in it and walks the three live mountpoints in --posix form: the
#    walk of from must exit 2 and name /inner.
#
#    The same pool then shows the other half of the fact, which is
#    the one a reader of the manifest has to know. A rebase in the
#    clone form reads .zfs/snapshot/NAME, and a child dataset is not
#    in its parent's snapshot: what the snapshot holds at the child's
#    name is the empty directory the child is mounted over, an object
#    of the parent like any other and on the snapshot's own device.
#    So -n over the two snapshots does not refuse and must not: there
#    is no nested mount in a snapshot. The guard is for the walk of a
#    live tree -- --posix here, and the dataset input form when it
#    lands. A rebase of a dataset with children under it therefore
#    passes over their contents, and the child's mountpoint arrives
#    in the result as the empty directory it is in the snapshot.
#
# 1c. canmount=off in the dataset form. A rebase in place ends by
#    mounting the dataset where it belongs, at done and at --abort,
#    and a dataset whose canmount is off has no such place: it is
#    refused at precondition with exit 2, before its pre-apply
#    snapshot is taken and before anything at all is written
#    (sprints/sprint-5/documents-design.md, section 5). The property
#    is read before the mounted question, which such a dataset fails
#    too, so that the refusal names the property and not the symptom.
#    No fixture builds a dataset like that, so the check makes one:
#    zfs create -o canmount=off, which leaves it unmounted, and runs
#    the tool over it in the dataset form.
#
# 1d. an schg file the manifest removes, at securelevel 0. The apply
#    takes the immutable, append-only and no-unlink flags off an
#    object before it removes, rewrites or changes it (src/apply.c,
#    za_unlock_st), because ZFS refuses the unlink, the truncate and
#    every other attribute change on one that carries them. Only
#    root can set the system three and only securelevel 0 or less
#    lets them off again, so this is the box's cell and not the
#    Mac's: the check puts schg on an onto file the manifest removes
#    and one it rewrites, runs the rebase for real, and wants exit 0
#    with both objects as the manifest said.
#
# 2. securelevel. Above securelevel 0 the system flags cannot be
#    cleared at all, so src/run.c's securelevel_guard refuses before
#    anything is written, naming the first object that carries schg,
#    sappnd or sunlnk and would change. It cannot be checked in a
#    reusable box session: securelevel can be raised at any time and
#    only a reboot lowers it, and an schg file made under it cannot
#    be removed again either. This script prints the procedure and
#    raises nothing.
#
# 3. A snapshot destroyed during a run. The persistent hold is what
#    stops it, and proving that means destroying the snapshot while
#    the run is between two gates. That is the pause hook's work
#    (ZFS_REBASE_PAUSE names a gate at which the tool raises SIGSTOP
#    on itself), and run-kills.sh does it at the held gate in both
#    forms: zfs destroy on each held input is refused and the
#    snapshot stands. run-fixture.sh checks the holds at rest. This
#    script repeats neither and says so.
#
# Three notes the box needs, which are not this script's to check.
#
#  - run-suite.sh now walks tests/fixtures/freebsd/*.zrt as well as
#    the flat directory. Those fixtures are the box's alone and are
#    root's: the system extended-attribute namespace is root's to
#    write, and a walk that may not read an attribute reads none at
#    all rather than failing.
#
#  - --build-fixture writes the three trees under a scratch
#    directory, and run-fixture.sh takes that from mktemp -d in
#    /tmp. A fixture carrying acl= or a system-namespace xattr can
#    only be built on a filesystem that has NFSv4 ACLs and both
#    extattr namespaces: ZFS does, tmpfs does not, and a /tmp on
#    tmpfs fails the build of every acl-*.zrt, mixed-attrs.zrt and
#    sysxattr-*.zrt with EOPNOTSUPP. Check 0 below says which /tmp
#    this box has.
#
#  - flags-conflict.zrt sets hidden (uchg is not a ZFS flag: the
#    builder gets EOPNOTSUPP). A built tree carrying a system flag
#    cannot be cleared or removed until the flag comes off, so a
#    harness that removes its scratch directory clears the flags
#    first (chflags -R nouchg,nouappnd,noschg,nosappnd DIR), as
#    tests/run-fixtures.sh does and as tests/fixtures/FORMAT.md
#    tells it to. Nothing the nine set holds anything down:
#    nodump is the flag mixed-attrs.zrt uses for that reason.
#
# The ACL fixtures name uid 1 (daemon) and gid 5 (operator), which
# every FreeBSD has, so they apply on a box with no accounts of its
# own.
set -u
cd "$(dirname "$0")/../.." || exit 2
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

POOL=zrtprecond
IMG=/tmp/${POOL}.img
MNT=/tmp/${POOL}-mnt
FIXTURE=tests/fixtures/probe.zrt
MD=
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-precond.XXXXXX") || exit 2

cleanup() {
	if [ "${KEEP:-0}" = 1 ]; then
		echo "KEEP=1: pool $POOL, $IMG and $tmp left in place"
		return
	fi
	zpool destroy -f "$POOL" 2>/dev/null
	[ -n "$MD" ] && mdconfig -d -u "$MD" 2>/dev/null
	rm -f "$IMG"
	chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null
	rm -rf "$tmp"
	rmdir "$MNT" 2>/dev/null
}
trap cleanup EXIT
say() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*"; exit 1; }

say "0. the scratch filesystem the box-only fixtures need"
# Not a verdict: a note, so that a build that fails later is read as
# the filesystem it is and not as a fixture that is wrong.
mkdir -p "$tmp/scratch" || exit 2
if "$bin" --build-fixture tests/fixtures/freebsd/acl-kept.zrt \
    "$tmp/scratch" > "$tmp/scratch.log" 2>&1; then
	echo "ok   $tmp is a filesystem an ACL fixture builds on"
else
	cat "$tmp/scratch.log"
	echo "note $tmp cannot hold an NFSv4 ACL or a system xattr."
	echo "     run-fixture.sh builds its trees under /tmp, so every"
	echo "     fixture in tests/fixtures/freebsd/ will fail to build"
	echo "     on this box until /tmp is a filesystem that has them."
fi
chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp/scratch" 2>/dev/null
rm -rf "$tmp/scratch"

say "1. a pool with a child dataset inside from"
"$bin" --build-fixture "$FIXTURE" "$tmp" || fail "build-fixture"
truncate -s 512m "$IMG" || exit 2
MD=$(mdconfig -a -t vnode -f "$IMG") || exit 2
mkdir -p "$MNT"
zpool create -m "$MNT" -O casesensitivity=sensitive -O normalization=none \
    "$POOL" "/dev/$MD" || exit 2
zfs create "$POOL/base" || exit 2
(cd "$tmp/base" && tar -cf - .) | (cd "$MNT/base" && tar -xpf -) || \
    fail "populate base"
zfs snapshot "$POOL/base@base" || exit 2
for side in from onto; do
	zfs clone "$POOL/base@base" "$POOL/$side" || exit 2
	(cd "$MNT/$side" && find . -mindepth 1 -maxdepth 1 -exec rm -rf {} +) \
	    || fail "clear $side"
	(cd "$tmp/$side" && tar -cf - .) | (cd "$MNT/$side" && tar -xpf -) || \
	    fail "populate $side"
done
# The nested mount: a dataset of its own, mounted inside from's tree.
zfs create "$POOL/from/inner" || fail "cannot create $POOL/from/inner"
printf 'inner\n' > "$MNT/from/inner/f" || fail "cannot write in the child"
[ "$(zfs get -H -o value mounted "$POOL/from/inner")" = yes ] || \
    fail "$POOL/from/inner is not mounted"
zfs snapshot "$POOL/from@work" "$POOL/onto@work" || exit 2
echo "ok   $POOL/from/inner is mounted at $MNT/from/inner"

say "1a. the walk of a live tree refuses it"
"$bin" --posix -o "$tmp/precond-manifest" "$MNT/base" "$MNT/from" \
    "$MNT/onto" > "$tmp/posix.out" 2> "$tmp/posix.err"
st=$?
[ $st -eq 2 ] || { cat "$tmp/posix.err"; fail "--posix over a nested mount exited $st, want 2"; }
# The walk names the mount by its full path, the one a person can
# act on: the tree's root and /inner under it.
grep -q 'nested mount at .*/inner$' "$tmp/posix.err" || \
    { cat "$tmp/posix.err"; fail "the refusal did not name the nested mount at /inner"; }
grep -q '^zfs_rebase: from:' "$tmp/posix.err" || \
    { cat "$tmp/posix.err"; fail "the refusal did not name the from tree"; }
echo "ok   exit 2, '$(cat "$tmp/posix.err")'"

say "1b. a snapshot has no nested mount in it, and is not refused"
# probe.zrt is the conflicted fixture, so the dry run exits 1; what
# matters is that it is not 2 and says nothing about a nested mount.
"$bin" -n -o "$tmp/got-n" --from "$POOL/from@work" \
    --onto "$POOL/onto@work" > "$tmp/dry.out" 2> "$tmp/dry.err"
st=$?
[ $st -eq 0 ] || [ $st -eq 1 ] || \
    { cat "$tmp/dry.err"; fail "the dry run over the snapshots exited $st"; }
if grep -q 'nested mount' "$tmp/dry.err"; then
	cat "$tmp/dry.err"
	fail "the snapshot form refused a nested mount, which a snapshot has none of"
fi
grep -q '/inner' "$tmp/got-n" || \
    { sed -n '1,40p' "$tmp/got-n"; fail "the snapshot did not hold /inner at all"; }
if grep -q '/inner/' "$tmp/got-n"; then
	grep -n inner "$tmp/got-n"
	fail "the snapshot held the child dataset's contents, which it cannot"
fi
echo "ok   exit $st: the child's mountpoint is in the snapshot as the"
echo "     empty directory it is, its contents are not, and the walk"
echo "     saw one filesystem"

say "1c. the dataset form refuses canmount=off"
# A dataset with no home to be handed back to. It is created
# unmounted, which is what canmount=off means, and the tool must say
# so about the property rather than about the mount.
zfs create -o canmount=off "$POOL/nohome" || \
    fail "cannot create $POOL/nohome"
[ "$(zfs get -H -o value canmount "$POOL/nohome")" = off ] || \
    fail "$POOL/nohome is not canmount=off"
"$bin" --from "$POOL/from@work" --onto "$POOL/nohome" --result pre \
    > "$tmp/nohome.out" 2> "$tmp/nohome.err"
st=$?
[ $st -eq 2 ] || \
    { cat "$tmp/nohome.err"; fail "canmount=off exited $st, want 2"; }
grep -q "canmount=off and no place to be handed back to" \
    "$tmp/nohome.err" || \
    { cat "$tmp/nohome.err"; fail "the refusal did not name canmount"; }
# And it was refused before it took anything: no snapshot of onto, no
# snapshot of from, no record and no run directory.
n=$(zfs list -H -o name -t snapshot -r "$POOL/nohome" | grep -c .)
[ "$n" -eq 0 ] || fail "the refused run left a snapshot on $POOL/nohome"
n=$(zfs list -H -o name -t snapshot -r "$POOL/from" | grep -c .)
[ "$n" -eq 1 ] || fail "the refused run left a snapshot on $POOL/from"
left=$(zfs get -H -o property,source all "$POOL/nohome" 2>/dev/null | \
    awk '$1 ~ /^zfs_rebase:/ && $2 == "local" { print $1 }')
[ -z "$left" ] || fail "the refused run left $left on $POOL/nohome"
[ -e "/var/db/zfs_rebase/$POOL/nohome" ] && \
    fail "the refused run left a run directory"
zfs destroy "$POOL/nohome" || fail "cannot destroy $POOL/nohome"
echo "ok   canmount=off refused (exit 2), naming the property, with"
echo "     no snapshot, no record and no run directory left"

say "1d. schg on onto objects the manifest removes and rewrites"
# The trees are made here rather than out of a fixture, because a
# fixture builder cannot set schg on a file it then has to edit: the
# base carries the flag, the two sides are clones of it, and the from
# side has the flag taken off again by hand along with its edits.
# base   /keep k  /gone g (schg)  /w x (schg)
# from   /keep k                  /w y          (gone removed)
# onto   base's, untouched, so nothing conflicts: the decision is one
#        rm and one write, both over an object the result holds under
#        schg, and the apply has to take it off to do either.
if [ "$(sysctl -n kern.securelevel)" -gt 0 ]; then
	echo "skip securelevel is above 0: schg cannot be cleared, which"
	echo "     is section 2's refusal and not this check"
else
	zfs create "$POOL/fbase" || fail "cannot create $POOL/fbase"
	printf 'k\n' > "$MNT/fbase/keep" || fail "write keep"
	printf 'g\n' > "$MNT/fbase/gone" || fail "write gone"
	printf 'x\n' > "$MNT/fbase/w" || fail "write w"
	chflags schg "$MNT/fbase/gone" "$MNT/fbase/w" || \
	    fail "cannot set schg (root and securelevel 0 are needed)"
	zfs snapshot "$POOL/fbase@fbase" || exit 2
	for side in ffrom fonto; do
		zfs clone "$POOL/fbase@fbase" "$POOL/$side" || exit 2
	done
	# from's edits: the flag off first, since nothing else can be
	# done to an object that carries it -- which is the whole of
	# what the apply now does for itself.
	chflags noschg "$MNT/ffrom/gone" "$MNT/ffrom/w" || fail "chflags noschg"
	rm "$MNT/ffrom/gone" || fail "rm gone"
	printf 'y\n' > "$MNT/ffrom/w" || fail "write w on from"
	zfs snapshot "$POOL/ffrom@work" "$POOL/fonto@work" || exit 2
	"$bin" -v -o "$tmp/flags-manifest" --off-of "$POOL/ffrom@work" \
	    --onto "$POOL/fonto@work" --result "$POOL/fresult" \
	    > "$tmp/flags.run" 2>&1
	st=$?
	[ $st -eq 0 ] || { cat "$tmp/flags.run"; fail "the schg run exited $st, want 0"; }
	grep -q '^#conflicts 0$' "$tmp/flags-manifest" || \
	    { fail "the schg run found conflicts, which these trees have none of"; }
	grep -q 'rm$' "$tmp/flags-manifest" || \
	    { cat "$tmp/flags-manifest"; fail "the manifest has no rm"; }
	grep -q 'write /w$' "$tmp/flags-manifest" || \
	    { cat "$tmp/flags-manifest"; fail "the manifest has no write of /w"; }
	# done left the clone unmounted with mountpoint none; place it
	# the way the tool's own last line says to, and read the tree.
	zfs set mountpoint="$MNT/fresult" "$POOL/fresult" || \
	    fail "cannot place $POOL/fresult"
	[ -e "$MNT/fresult/gone" ] && \
	    fail "the apply left /gone, which the manifest removes"
	[ "$(cat "$MNT/fresult/w")" = y ] || \
	    fail "/w does not hold from's bytes"
	fl=$(stat -f %Sf "$MNT/fresult/w")
	case "$fl" in
	*schg*) fail "/w still carries schg, which from does not have: $fl" ;;
	esac
	[ "$(cat "$MNT/fresult/keep")" = k ] || fail "/keep was disturbed"
	fl=$(stat -f %Sf "$MNT/fresult/keep")
	echo "ok   exit 0: the immutable /gone removed, the immutable /w"
	echo "     rewritten as from has it and without the flag, /keep"
	echo "     untouched (flags now '$fl')"
fi

say "2. securelevel: the manual procedure, not run here"
cat <<'PROCEDURE'
securelevel can be raised and not lowered, and an schg file made
above 0 cannot be removed until a reboot, so this cell belongs to a
throwaway VM or a jail that is destroyed afterwards. Nothing below
is run by this script.

  1. In a throwaway VM, or a jail with its own securelevel:
         sysctl kern.securelevel=1
     (in a jail, raise the jail's own; on a VM, set kern_securelevel
     in /etc/rc.conf and reboot, since only init lowers it).
  2. Build a fixture whose ONTO tree carries schg on a file the
     manifest would rewrite, remove or re-pool -- the guard reads
     the onto side's attributes against the decision, so schg on
     the from side is not it. The one-line tree is
         /A file x flags=schg
     in onto, /A file x in base, and /A file y in from: a write.
     Build the trees before the securelevel goes up, since a
     builder cannot set schg on a file it then has to write.
  3. Run the rebase for real, not with -n: the guard runs after the
     decision and before the first write, and -n never reaches it.
         zfs_rebase --from POOL/from@work --onto POOL/onto@work \
             --result POOL/result
  4. Expect exit 2 and, on stderr,
         zfs_rebase: precondition: securelevel 1: /A carries schg,
         sappnd or sunlnk and would change
     as one line, naming the first such object. Nothing is created,
     nothing is held, and the result dataset does not exist.
  5. Destroy the VM or the jail. The schg file cannot be unlinked
     until securelevel is back to 0, which is a reboot.

The cell is ZX23 in tests/MATRIX.md, deferred there for this reason.
PROCEDURE

say "3. a snapshot destroyed during a run: run-kills.sh has it"
echo "The hold is what refuses the destroy, and catching it means"
echo "destroying the input while the run is between two gates. That"
echo "is the pause hook's -- ZFS_REBASE_PAUSE names a gate at which"
echo "the tool raises SIGSTOP on itself -- and run-kills.sh does it"
echo "at the held gate in both forms: zfs destroy on each held input"
echo "is refused and the snapshot stands. run-fixture.sh checks the"
echo "holds at rest: none after done, one per input under the"
echo "record's tag at conflicts. Nothing is repeated here."

echo
echo "run-precond: the nested-mount and canmount=off refusals passed;"
echo "securelevel and the destroy-under-a-hold are documented, not run"
exit 0
