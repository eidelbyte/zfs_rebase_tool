# The box

Everything FreeBSD-specific gets its first real compile and run here:
the FreeBSD sections of walk.c and apply.c, all of zfsops.c, and the
real mode of the driver.

    git clone https://github.com/eidelbyte/zfs_rebase_tool
    cd zfs_rebase_tool
    sh tests/box/prereqs.sh            # what is missing, and how to get it
    make freebsd                       # ZFS_INCLUDE=... if the headers are elsewhere
    make check                         # the Mac-side gates, now on FreeBSD
    make gate                          # needs perl
    sudo sh tests/box/run-fixture.sh tests/fixtures/probe.zrt
    sudo sh tests/box/run-suite.sh

run-fixture.sh builds one fixture as real datasets on a throwaway
pool on a memory disk, runs the tool for real, and checks the
manifest against the expect block, the exit status, and for a clean
fixture that the working clone is read-only and that rebasing from
onto it again is a no-op. KEEP=1 leaves the pool for inspection.
Its header says what it does not exercise yet.
