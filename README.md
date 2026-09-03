# zfs_rebase

A standalone command that rebases one ZFS filesystem onto another:
given a base snapshot and two datasets cloned from it, it replays the
changes of one (from) onto a working clone of the other (onto), or
tells you exactly which files it could not decide and why.

It is not a zfs(8) verb and it changes nothing in ZFS. It reads three
snapshots through their .zfs/snapshot directories, asks zfs diff for
what is unchanged, decides by a small rule over names, hardlink pools
and content, writes a manifest of actions and conflicts, and applies
the actions to a clone it created read-only for the purpose. All ZFS
operations go through libzfs_core and libzfs; nothing is exec'd.

Two build modes:

    make            portable core, any POSIX system with cc
    make check      build and run the core's tests
    make freebsd    the real tool: adds the ZFS layer and links
                    libzfs_core, libzfs, libnvpair
    make gate       ASCII and style checks over the sources

The core alone runs end to end in --posix mode over three ordinary
directories, which is how it is tested where there is no ZFS.

The theory behind the decision rule, the manifest format, and the
sprint plan live in the author's freebsd-development notes (the v4
set under zfs-rebase-theory/ and sprints/sprint-4/). The manifest
format will be copied into doc/ here when the emitter lands.

License: BSD 3-Clause (see LICENSE). tools/cstyle.pl is OpenZFS's
and remains under CDDL-1.0, as its header says.
