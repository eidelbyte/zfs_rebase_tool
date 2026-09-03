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
pool on a memory disk, runs the tool for real, and checks the
manifest against the expect block, the exit status, and for a clean
fixture that the working clone is read-only and that rebasing from
onto it again is a no-op. KEEP=1 leaves the pool for inspection.
Its header says what it does not exercise yet.
