# The box

Everything FreeBSD-specific gets its first real compile and run here:
the FreeBSD sections of walk.c and apply.c, all of zfsops.c, and the
real mode of the driver.

    git clone https://github.com/eidelbyte/zfs_rebase_tool
    cd zfs_rebase_tool
    sh tests/box/prereqs.sh            # what is missing, and how to get it
    make clean                         # the flavors do not share build/
    make freebsd                       # ZFS_SRC=/path/to/src if not /usr/src
    make check-freebsd                 # the Mac-side gates, now on FreeBSD
    make gate                          # needs perl
    sudo sh tests/box/run-fixture.sh tests/fixtures/probe.zrt
    sudo sh tests/box/run-suite.sh
    sudo sh tests/box/run-precond.sh   # the cells no fixture states

run-fixture.sh builds its trees under mktemp -d /tmp/zr-box.XXXXXX,
and the fixtures in tests/fixtures/freebsd/ carry NFSv4 ACLs and
system-namespace extended attributes, which a tmpfs /tmp cannot hold
at all: setting one there fails with EOPNOTSUPP and the fixture fails
before the pool is even made. The box wants /tmp on ZFS (or UFS), or
TMPDIR pointed at a directory on one. Those fixtures are root's for
the same reason: the system attribute namespace is root's.

make freebsd builds against the OpenZFS headers in the FreeBSD source
tree, the way FreeBSD's own zfs(8) is built, because the installed
headers alone are incomplete (libzfs.h needs libspl's Solaris types
and sys/avl.h, sys/fs/zfs.h and sys/mnttab.h, none of which land in
/usr/include). Any checkout of the same FreeBSD release will do.

check-freebsd, not check: the check targets link the test programs
and re-link zfs_rebase against the library objects, and those now
include zfsops.o, so they need the same -DZR_FREEBSD, the same
OpenZFS include set and the same -lzfs_core -lzfs -lnvpair that the
freebsd target uses. Plain make check after make freebsd fails at
link time with every libzfs, libzfs_core and libnvpair symbol
zfsops.c calls. check-freebsd recurses with exactly those flags and
then runs check. The two flavors do not share build/, because the
objects differ; start each of them from make clean.

Before a box trip, tools/xcheck-freebsd.sh cross-checks every source
with clang targeting FreeBSD against a FreeBSD source tree's
headers, with the same two flag sets the Makefile uses; it catches
declaration and type errors in the FreeBSD sections without a box.

run-fixture.sh builds one fixture as real datasets on a throwaway
pool on a memory disk and runs the tool for real -- twice over, once
in each form.

The clone form first. The harness takes the snapshots -- base@base,
from@work and onto@work -- and hands the run only the two sides and
the name of the clone:

    zfs_rebase -n -o FILE --from POOL/from@work --onto POOL/onto@work
    zfs_rebase -v -o FILE --off-of POOL/from@work \
        --onto POOL/onto@work --result POOL/result
    zfs_rebase --abort --result POOL/result

Then the dataset form, after that pass has been aborted and the pool
is back to base, from and onto with their snapshots. Both sides are
given as datasets, so the tool takes its own snapshot of from and
--result names the pre-apply snapshot of onto:

    zfs_rebase -v -o FILE --from POOL/from --onto POOL/onto --result pre
    zfs_rebase --verify --result POOL/onto
    zfs_rebase --continue --result POOL/onto
    zfs_rebase --abort --result POOL/onto

and then the whole pass again with --result POOL/onto@pre, since the
short name and the full one must name the same snapshot of the same
dataset.

What that pass shows. The manifest is the clone form's manifest
exactly, from the #mode line on, and derives the same base: one
decision, two ways of carrying it out. The record is on POOL/onto
itself with zfs_rebase:form dataset, :made from, :readonly recording
what readonly was, :onto the pre-apply snapshot and :from the
snapshot the tool took of the from dataset, named after the run's own
tag; every one of them is local, against the bogus tag and manifest
on the pool root. The dataset is mounted at its own mountpoint again
when the run stops, readonly as it was, with the mountpoint property
untouched -- that is the hand-back, and it happens at conflicts, at
done, at a failure and after every verb, because a rebase waiting for
a conflict to be answered must not hold a filesystem out of service
while it waits. A clean fixture is at done with every hold released,
the tool's own snapshot of from destroyed, and the live tree the
rebased tree; a conflicted one is at conflicts with the clean actions
applied, the three holds under the record's tag, and the tool's own
snapshot still there. --abort rolls onto back to the pre-apply
snapshot, destroys that and the tool's own, leaves no zfs_rebase:
property local, and hands the dataset back holding exactly the tree
the fixture built -- which the harness proves by rebasing over it in
--posix form and getting the expect block back.

Two more things belong to this form. The exclusivity is the unmount,
so probe.zrt holds a file open under POOL/onto and runs the tool: it
exits 2 saying onto is in use, and takes back its record, both
snapshots and its run directory, leaving the dataset mounted where it
was. And --overwrite, on a clean fixture, whose dataset-form run
reaches done: a second run is refused without the flag and replaces
the record with it, and the finished rebase's before-image is still
there afterwards, since a rebase that reached done keeps it.

Both runs must derive POOL/base@base for themselves, and the harness
checks the #base line of each manifest for it; the real run spells
--from as --off-of, so that alias is exercised too. Step 0 checks the
two refusals first: a linear pair (base@base against onto@work, where
one side is an ancestor of the other) and a pair sharing no origin
(a dataset created on its own), each exit 2. The rest checks the
manifest against the expect block and the exit status for each run,
and then the two things that make a rebase outlive its process: the
holds and the record.

The holds are persistent, one per input snapshot under the tag in the
record, so what the harness expects depends on where the run stopped:
a clean fixture reaches done, which releases all three, and zfs holds
-H prints nothing; a conflicted fixture stops at conflicts and keeps
them, so each of the three prints exactly one line whose tag is the
result's zfs_rebase:tag. After --abort all three are empty again
either way.

The record is read back with zfs get: the three snapshot names, their
guids against zfs get guid on the snapshots themselves,
zfs_rebase:form clone, the mode the flag asked for, zfs_rebase:made
empty (the tool snapshots nothing of its own), zfs_rebase:verify no,
the tag, the manifest path, and zfs_rebase:state -- "done" for a
clean fixture, "conflicts" for a conflicted one, the gates being
applying1, conflicts, applying2 and done. The run directory is
/var/db/zfs_rebase/<result>, not /var/run: cleanvar empties /var/run
at boot, and a rebase held at conflicts can outlast a reboot. Every one of them
must have source local: before the run the harness sets a bogus
zfs_rebase:tag and zfs_rebase:manifest on the pool root, because user
properties inherit down the naming tree, and it also creates a plain
dataset there, on which --abort must exit 2 and change nothing, since
that dataset only inherits the properties and is no result of ours.
For either fixture the harness then rebases from onto the result
again, in --posix mode over the fixture's own base and from
directories and the clone at its mountpoint: stage 1 is idempotent,
so the manifest that comes back must declare zero actions. A clean
fixture wants zero conflicts with it; a conflicted one wants exactly
the count its expect block names, because the run applies its clean
actions before it stops at conflicts, and answering a conflict is
the conflict manager's work rather than a second rebase's.

Then the verbs. --verify on the result exits 0 on either branch --
every action of the manifest is done by now, since a conflicted run
applies its clean actions before it stops -- prints one line per
outcome with its count and first name, and writes nothing at all:
the state after it is the state before it. --continue exits 0 on a
done result and 1 on one at conflicts, where it names the resolution
file it is waiting for, and leaves the tree as stage 1 made it, which
the --posix re-run is asked again to confirm. For probe.zrt there
are two more: a byte appended to /n, which the manifest copied,
behind the tool's back with readonly off and on again, must make
--verify exit 3 naming "drifted 1, first /n" and change nothing,
and --continue --verify must put it back and leave --verify clean
and the result read-only; and --restart must destroy the clone,
make it again from the recorded onto snapshot with the same record,
apply the manifest from the first gate and land at the same state,
under the same tag, with the same three holds and the same tree.

Step 4 aborts the run and checks that the holds, the dataset, the
manifest file the run recorded and the directory under /var/db are
all gone, and that a second --abort exits 2. Then a second real run
given --verify -- every clean fixture takes this, and probe.zrt takes
it as the conflicted one: zfs_rebase:verify is "yes" in its record
and its tag is a new one, and on a clean fixture the final check runs
at the done gate, so its report is printed, the state is done and the
holds are released before the abort. For probe.zrt there is also the
dry run given --verify, which must still create nothing, hold nothing
and leave no run directory. The dataset form's pass runs after all of
that. KEEP=1 leaves the pool for inspection. Its header says what it
does not exercise yet.

run-suite.sh walks tests/fixtures/ and tests/fixtures/freebsd/ and
runs every one of them through both forms; a failure prints the log's
last "==" heading, which names the form and the step it stopped at.
