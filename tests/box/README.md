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
clean fixture, "conflicts" for a conflicted one. Every one of them
must have source local: before the run the harness sets a bogus
zfs_rebase:tag and zfs_rebase:manifest on the pool root, because user
properties inherit down the naming tree, and it also creates a plain
dataset there, on which --abort must exit 2 and change nothing, since
that dataset only inherits the properties and is no result of ours.
For a clean fixture the harness also checks that rebasing from onto
the result again is a no-op. Step 4 aborts the run and checks that
the holds, the dataset, the manifest file the run recorded and the
directory under /var/run are all gone, and that a second --abort
exits 2. For probe.zrt there are two more: -n --verify creates
nothing, holds nothing and leaves no run directory, and a real run
given --verify records zfs_rebase:verify yes under a tag of its own.
KEEP=1 leaves the pool for inspection. Its header says what it does
not exercise yet.
