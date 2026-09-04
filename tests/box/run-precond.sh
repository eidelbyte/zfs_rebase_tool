#!/bin/sh
# The box cells that are not fixtures. FreeBSD, root, after
# make freebsd. Usage: run-precond.sh   (KEEP=1 leaves the pool
# behind for inspection).
#
# run-fixture.sh proves what three trees and a manifest can say.
# Three things they cannot say are here: a nested mount inside an
# input, the securelevel refusal, and a snapshot destroyed under a
# running rebase. This script closes the first, prints the second as
# a procedure to run by hand, and says why the third has to wait.
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
# 2. securelevel. Above securelevel 0 the system immutable and
#    append-only flags cannot be cleared, so src/run.c's
#    securelevel_guard refuses before anything is written, naming the
#    first object that carries schg or sappnd and would change. It
#    cannot be checked in a reusable box session: securelevel can be
#    raised at any time and only a reboot lowers it, and an schg file
#    made under it cannot be removed again either. This script prints
#    the procedure and raises nothing.
#
# 3. A snapshot destroyed during a run. The persistent hold is what
#    stops it, and proving that means destroying the snapshot while
#    the run is between two gates. Nothing here can stop a run in the
#    middle: that is the pause hook (an environment variable naming a
#    gate at which the tool raises SIGSTOP on itself), which sprint 5
#    has not built yet. Until it exists, what can be checked is what
#    run-fixture.sh already checks -- that the holds are there at the
#    conflicts gate and gone at done -- and the destroy-under-a-hold
#    cell waits for the hook.
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
#  - flags-conflict.zrt sets uchg. A built tree carrying it cannot
#    be cleared or removed until the flag comes off, so a harness
#    that removes its scratch directory must clear the flags first
#    (chflags -R nouchg,nouappnd,noschg,nosappnd DIR), as
#    tests/run-fixtures.sh does and as tests/fixtures/FORMAT.md
#    tells it to. Nothing else the nine set holds anything down:
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
grep -q 'nested mount at /inner' "$tmp/posix.err" || \
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
         zfs_rebase: precondition: securelevel 1: /A carries schg
         or sappnd and would change
     as one line, naming the first such object. Nothing is created,
     nothing is held, and the result dataset does not exist.
  5. Destroy the VM or the jail. The schg file cannot be unlinked
     until securelevel is back to 0, which is a reboot.

The cell is ZX23 in tests/MATRIX.md, deferred there for this reason.
PROCEDURE

say "3. a snapshot destroyed during a run: not yet testable"
echo "The hold is what refuses the destroy, and catching it means"
echo "destroying the input while the run is between two gates. That"
echo "needs the pause hook -- an environment variable naming a gate"
echo "at which the tool raises SIGSTOP on itself -- which sprint 5"
echo "has not built. run-fixture.sh checks the holds at rest: none"
echo "after done, one per input under the record's tag at conflicts."

echo
echo "run-precond: the nested-mount refusal passed; securelevel and"
echo "the destroy-under-a-hold are documented, not run"
exit 0
