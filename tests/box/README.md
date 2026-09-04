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
pool on a memory disk and runs the tool for real. The tool takes no
snapshots, so the harness takes them -- base@base, from@work and
onto@work -- but hands the run only the two sides and names the
result clone:

    zfs_rebase -n -o FILE --from POOL/from@work --onto POOL/onto@work
    zfs_rebase -v -o FILE --off-of POOL/from@work \
        --onto POOL/onto@work --result POOL/result
    zfs_rebase --abort --result POOL/result

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
and leave no run directory. KEEP=1 leaves the pool for inspection.
Its header says what it does not exercise yet.
