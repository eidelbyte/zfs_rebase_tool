# zfs_rebase

A standalone command that rebases one ZFS filesystem onto another:
given three snapshots -- a base and the two sides that diverged from
it -- it replays the changes of one (from) onto a clone of the other
(onto), or tells you exactly which files it could not decide and why.

    zfs_rebase [-n] [-p] [-v] [-o FILE] [--verify] \
        --from SNAP --onto SNAP --result DATASET
    zfs_rebase --abort --result DATASET

--from may also be spelled --off-of. --result names the dataset the
rebased clone is created as; -n is a dry run, which writes the
manifest, creates nothing and holds nothing. --verify is recorded for
the final check a later version runs.

You name the two sides and not the base. The base is the branch
point, and the tool works it out: it walks each side's origin chain
-- the snapshot, then its dataset's origin, then that dataset's
origin -- back to the nearest dataset they both descend from, and
takes the older of the two snapshots they name there, by createtxg.
A base given by hand could only agree with that or disagree with it,
and a disagreement is not something the tool could act on: the two
sides are related the way the origin graph says, and no other
snapshot is the point they last had in common. Two sides that share
no origin are refused, and so is a pair where the base turns out to
be one of the arguments -- one side already contains the other, and
what is wanted there is not a rebase.

It is not a zfs(8) verb, and it takes no snapshots and destroys none
of yours: the three it reads are yours. It reads them through their
.zfs/snapshot directories, asks zfs diff for what is unchanged,
decides by a small rule over names, hardlink pools and content,
writes a manifest of actions and conflicts, and applies the actions
to the result clone, which it creates read-only and puts back that
way. All ZFS operations go through libzfs_core and libzfs; nothing is
exec'd.

A rebase outlives the process that started it. The result carries a
record, in user properties set by the create itself and read back as
local values only:

    zfs_rebase:base        the branch point, and :base_guid
    zfs_rebase:from        the side replayed, and :from_guid
    zfs_rebase:onto        the side replayed onto, and :onto_guid
    zfs_rebase:made        which inputs the tool snapshotted itself
    zfs_rebase:mode        strict or permissive
    zfs_rebase:form        clone
    zfs_rebase:tag         the tag its holds are filed under
    zfs_rebase:verify      whether --verify was asked for
    zfs_rebase:manifest    where the manifest was written
    zfs_rebase:state       the last gate the run passed

and one persistent hold per input snapshot under that tag, so that
none of the three can be destroyed while the rebase is open: zfs
holds shows the tag, and zfs destroy refuses with "dataset is busy".
A stranded rebase holds on purpose, because it is meant to be
resumed.

Its progress is a sequence of gates:

    applying1 -> conflicts -> applying2 -> done
    applying1 -> done                          (no conflicts)

applying1 is written immediately before the result stops being
read-only, and under it the clean actions of the manifest are applied
-- whether the decision had conflicts or not, since a conflict stops
the names it covers and nothing else, and conflicts are answered over
the tree the rest of the rebase has already made. conflicts is
written after that apply verified, and is the hand-off: the conflict
manager (a separate tool, a later sprint) leaves its answers in the
run directory as "resolution", a manifest in the same format holding
only actions, and applying2 applies that file the same way. done is
written after the result verified and is read-only again, and before
the holds are released. What a kill leaves is the last gate reached,
there is no state at all until the first one, and a stop writes none:
--continue resumes from the gate, --abort takes the rebase away.

Each run keeps its own directory, 0700 throughout:

    /var/db/zfs_rebase/<result as a path>/mnt          the clone
    /var/db/zfs_rebase/<result as a path>/manifest     unless -o
    /var/db/zfs_rebase/<result as a path>/resolution   the answers

Not /var/run: FreeBSD's cleanvar deletes every regular file there at
boot, and a rebase stopped at conflicts can outlast one.

The result is yours to promote, rename or inherit as you see fit:
the tool never promotes.

    zfs_rebase --abort --result DATASET

releases those holds, destroys the result, unlinks the manifest the
record names and removes the run directory: as if the run never
happened. It keys on the record and on nothing else, so a dataset
that carries none is refused untouched -- and an inherited value is
no record, since user properties inherit down the naming tree and
only a local one is ours.

Two build modes:

    make            portable core, any POSIX system with cc
    make check      build and run the core's tests
    make freebsd    the real tool: adds the ZFS layer and links
                    libzfs_core, libzfs, libnvpair
    make check-freebsd
                    the same tests and gates, built and linked that way
    make gate       ASCII and style checks over the sources

The core alone runs end to end in --posix mode over three ordinary
directories, which is how it is tested where there is no ZFS.

The theory behind the decision rule, the manifest format, and the
sprint plan live in the author's freebsd-development notes (the v4
set under zfs-rebase-theory/ and sprints/sprint-4/). The manifest
format will be copied into doc/ here when the emitter lands.

License: BSD 3-Clause (see LICENSE). tools/cstyle.pl is OpenZFS's
and remains under CDDL-1.0, as its header says.
