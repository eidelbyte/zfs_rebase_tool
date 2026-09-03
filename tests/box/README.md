# The box

Everything FreeBSD-specific gets its first real compile and run here:
the FreeBSD sections of walk.c and apply.c, all of zfsops.c, and the
real mode of the driver.

    git clone https://github.com/eidelbyte/zfs_rebase_tool
    cd zfs_rebase_tool
    sh tests/box/prereqs.sh            # what is missing, and how to get it
    make freebsd                       # ZFS_SRC=/path/to/src if not /usr/src
    make check                         # the Mac-side gates, now on FreeBSD
    make gate                          # needs perl
    sudo sh tests/box/run-fixture.sh tests/fixtures/probe.zrt
    sudo sh tests/box/run-suite.sh

make freebsd builds against the OpenZFS headers in the FreeBSD source
tree, the way FreeBSD's own zfs(8) is built, because the installed
headers alone are incomplete (libzfs.h needs libspl's Solaris types
and sys/avl.h, sys/fs/zfs.h and sys/mnttab.h, none of which land in
/usr/include). Any checkout of the same FreeBSD release will do.

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
