# zfs_rebase

A standalone command that rebases one ZFS filesystem onto another:
given two sides that diverged from a common base, it replays the
changes of one (from) onto the other (onto), or tells you exactly
which files it could not decide and why.

    zfs_rebase [-p] [-v] [--manifest FILE] [--verify] [--overwrite] \
        [--allow-unrelated [--base SNAP]] \
        [--take-onto | --take-from] [--no-gui] [--no-merge] \
        --from SNAP|DATASET --onto SNAP|DATASET --result NAME
    zfs_rebase --dry-run [-p] [--manifest FILE] \
        --from SNAP|DATASET --onto SNAP|DATASET
    zfs_rebase --continue [--verify] [--no-gui] [--no-merge] \
        --result DATASET
    zfs_rebase --restart --result DATASET
    zfs_rebase --abort --result DATASET
    zfs_rebase --verify --result DATASET

Every flag has a long form and a short form, and the two are the same
flag: the table below gives both. --from may also be spelled --off-of
and --onto --to, neither with a letter of its own. --dry-run (-n)
writes the manifest, creates nothing and holds nothing. --verify at
the start is recorded and honoured at the last gate.

## Options

| long form | short | what it does |
|-----------|-------|--------------|
| `--from`, `--off-of` | `-f` | the side whose changes are replayed: a snapshot, or a dataset the tool snapshots itself and destroys at done or --abort |
| `--onto`, `--to` | `-t` | the side they are replayed onto, and the form of the run: a snapshot is cloned as --result, a dataset is rebased in place |
| `--result` | `-r` | the clone's name in one form and the pre-apply snapshot's in the other; for every verb, the dataset carrying the record |
| `--permissive-merge` | `-p` | permissive merge; strict is the default, and the mode is recorded |
| `--verbose` | `-v` | counts and steps on stderr |
| `--manifest` | `-o` | where the manifest is written; the resolution goes beside it, and both paths are recorded |
| `--verify` | `-V` | ask for the final check, or, alone on a result, report and write nothing; never a repair |
| `--take-onto` | `-O` | write the skeleton with every conflict answered onto |
| `--take-from` | `-F` | write the skeleton with every conflict answered from; the two exclude each other |
| `--no-gui` | `-G` | at the conflicts gate, go on without the picker when the resolution is complete and stop when it is not -- the only behavior while there is no picker |
| `--no-merge` | `-M` | stop at the conflicts gate however the resolution reads; an error once the gate is passed |
| `--continue` | `-c` | take the rebase on from the gate its record names |
| `--restart` | `-R` | the result back as onto was, the manifest applied again from the first gate, the resolution back to its skeleton |
| `--abort` | `-a` | holds released, tool-made snapshots destroyed, the clone destroyed or the dataset rolled back, manifest, resolution and run directory removed |
| `--dry-run` | `-n` | decide and write the manifest, then tear down: nothing held, nothing created, --result ignored |
| `--overwrite` | `-w` | dataset form: replace a record whose rebase reached done |
| `--allow-unrelated` | `-u` | no derivation of the base, and no pruning |
| `--base` | `-b` | with --allow-unrelated only: the base, no newer than either side; without it, the empty tree |

A long form takes its value as `--from SNAP` or as `--from=SNAP`; a
short form takes the argument after it, and short flags do not bundle
(`-nv` is not `-n -v`). `--posix`, `--build-fixture` and
`--edit-fixture` are the project's own test aids: they are long only
and take the first argument position.

Each side is a snapshot of yours or a dataset the tool snapshots for
itself, and --onto decides the form of the run.

**The clone form**, --onto a snapshot. --result names a new dataset,
created as a read-only clone of that snapshot at the run's own
mountpoint, and the rebase is made in it. Nothing of yours is
written to at all, and the clone is yours to promote, rename or
inherit when it is done.

**The dataset form**, --onto a dataset. The rebase is made in that
dataset, and --result names the snapshot the tool takes of it before
anything is applied -- the short name, or a full name whose dataset
part is onto itself. That snapshot is yours: it stays after done as
the before-image, and only --abort takes it away, rolling the dataset
back to it first.

    zfs_rebase --from tank/topic --onto tank/main --result pre

Exclusivity in the dataset form is the unmount. The tool unmounts
onto from its own mountpoint and mounts it at the run's private
directory instead, without touching the mountpoint property, so that
nothing else can be in the tree while the rebase writes to it. A
dataset somebody is using will not unmount, and then the tool says
"onto is in use; unmount it or give a snapshot" and exits 2. Nothing
is ever forced.

Wherever the run stops -- at conflicts, at done, at a failure, at a
signal -- the dataset is handed back: off the private mount, readonly
as it was before, mounted where its mountpoint property says. A
rebase that is waiting for a conflict to be answered can wait for
days, and it does not hold a filesystem out of service while it
waits. Only a hard kill leaves it privately mounted, and the next
--continue, --verify or --abort takes it from there. The private
mount is root's alone and writable for its whole life: a dataset that
was read-only is made writable once, while it is off its mountpoint,
and the record remembers what it was for the hand-back. (libzfs
answers a readonly change on a mounted dataset with a remount at the
mountpoint property, which cannot be done while the dataset sits at
the private mount, so the property is only ever touched unmounted.)

--result for every verb is the dataset carrying the rebase's record
-- the clone in one form, onto itself in the other -- and a snapshot
name is taken as its dataset, so both spellings find the same rebase.

A dataset given as a side is snapshotted by the tool under a
generated name, recorded as tool-made, and destroyed when the rebase
ends, at done or at --abort. If you wanted that snapshot kept you
would have passed one. A dataset onto never carries a tool-made
snapshot: its before-image is the one you named.

The exit status is 0 when the rebase is done, 1 when it stopped at
conflicts, 2 when it was refused before anything was touched, and 3
when something failed part way.

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

**Unrelated sides**, --allow-unrelated. Two trees that never shared a
lineage have no branch point to work out, and this is the flag that
says so and takes them anyway. There is no derivation, and there is
no pruning either: the unchanged set is read off object numbers, and
an object number in one lineage means nothing in another. --base
names the base to rebase from, a snapshot no newer than either side
by createtxg -- a base taken after a side describes a state that side
never passed through -- and its dataset is read through
.zfs/snapshot like the other two. Without --base there is no base
snapshot at all: the base is the empty tree, every name of either
side is an add on that side, and the decision is the union of the
two with a conflict wherever they disagree. The record and the
manifest header then carry "-" for the base and the guid 0, which
every verb reads as "there was no base". --base without
--allow-unrelated is a usage error: where the branch point is
derived, a base given by hand could only agree with it or be wrong.

It is not a zfs(8) verb. It destroys no snapshot of yours and takes
one only where you gave it a dataset instead. It reads the three
through their .zfs/snapshot directories, takes the unchanged set off
those walks -- an object whose number, generation number and change
time all stood still since base is the object base holds, and is
never read -- decides by a small rule over names, hardlink pools and
content,
writes a manifest of actions and conflicts, and applies the actions
to the result clone, which it creates read-only and puts back that
way. All ZFS operations go through libzfs_core and libzfs; nothing is
exec'd.

A rebase outlives the process that started it. The result carries a
record, in user properties -- set by the create itself in the clone
form, and on the dataset before anything is touched in the other --
and read back as local values only:

    zfs_rebase:base        the branch point, and :base_guid
    zfs_rebase:from        the side replayed, and :from_guid
    zfs_rebase:onto        the side replayed onto, and :onto_guid
    zfs_rebase:made        which inputs the tool snapshotted itself
    zfs_rebase:mode        strict or permissive
    zfs_rebase:form        clone or dataset
    zfs_rebase:tag         the tag its holds are filed under
    zfs_rebase:verify      whether --verify was asked for
    zfs_rebase:take        onto, from or "-": how the skeleton was
                           answered when it was written
    zfs_rebase:manifest    where the manifest was written
    zfs_rebase:resolution  where the resolution was written
    zfs_rebase:readonly    what readonly was before (dataset form)
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
written after that apply verified, and is the hand-off: the run wrote
a resolution beside the manifest when it wrote the manifest -- one
line per conflicted name, each with a choice of "-", keep, onto or
from, in the manifest's own tree grammar (v4-manifest.md section 8)
-- and the gate is passed by answering every "-" and running
--continue. The gate keys on that completeness and on the command,
never on the file being there: the tool wrote it, so it always is.
A fresh run is such a command too, so a run whose own skeleton came
out complete -- which is what --take-onto and --take-from make it --
hands the result back and goes on to done in the same process, by
the one code path a --continue uses. --no-merge holds it at the gate
instead, and is refused once the gate is passed. --no-gui asks for
what the gate does anyway while there is no picker.
applying2 carries the choices out. done is
written after the result verified and is read-only again, and before
the holds are released. What a kill leaves is the last gate reached,
there is no state at all until the first one, and a stop writes none:
--continue resumes from the gate, --abort takes the rebase away.

Each run keeps its own directory, 0700 throughout:

    /var/db/zfs_rebase/<result as a path>/mnt          the result
    /var/db/zfs_rebase/<result as a path>/manifest     unless --manifest
    /var/db/zfs_rebase/<result as a path>/resolution   the choices

With --manifest FILE (-o FILE) the manifest is FILE and the
resolution is FILE.resolution, beside it. Either way the record names both, and
every verb finds them there and never by guessing a path.

Not /var/run: FreeBSD's cleanvar deletes every regular file there at
boot, and a rebase stopped at conflicts can outlast one.

The clone form's result is yours to promote, rename or inherit as you
see fit: the tool never promotes. The dataset form's result is the
dataset you already had, back at its own mountpoint with the rebase
in it.

Four verbs work on a rebase that already exists. Each takes --result
and -v, --continue takes the flags of the gate as well, and nothing
else is theirs; every one of them keys on the record and on nothing
else: a dataset that carries none is refused untouched, and an
inherited value is no record, since user properties inherit down the
naming tree and only a local one is ours. Each also checks that every snapshot the record names is still
the snapshot it named, by guid, since a snapshot destroyed and taken
again under the same name is another snapshot and these answers do
not describe it.

    zfs_rebase --continue [--verify] [--no-gui] [--no-merge] \
        --result DATASET

takes the rebase on from the gate its record names, through the
gates that are left, in one process. Applying is idempotent -- every
action means "make this true" -- so what is already true is left
alone and what is not is made, which is why a fresh run and a resumed
one are one code path. With --verify it prints how every action of
the manifest and every answered name of the resolution stands --
done, pending, blocked, drifted or unchecked -- and what the result
holds outside them both. It repairs nothing: the one fix in the tool
is the applying1 stage's own self-check, which is always on and is no
flag's. The one document it writes is the resolution, at the
conflicts gate, where the drift it found becomes lines with the
choice keep for the person to answer. --no-merge stops it at that
gate however the resolution reads, and is refused from a record
already past the merge.

    zfs_rebase --restart --result DATASET

puts the result back as onto was -- destroying the clone and making
it again from the recorded onto snapshot with the same record, or
rolling the dataset back to its pre-apply snapshot -- and then
applies the recorded manifest from the first gate, with the
resolution put back to its skeleton: the one the run wrote, which
zfs_rebase:take says was answered onto, from or not at all. Nothing
is decided again: the manifest is the decision, a resolution's edits
are discarded by definition, and the instruction the rebase was
started with is not an edit.

    zfs_rebase --verify --result DATASET

reports and writes nothing at all, so a deliberate edit to a rebased
file is shown and never overwritten. It exits 0 when nothing is
pending or drifted and 3 when something is; blocked and unchecked
are states and not faults. After done it is best effort: each input
is looked for by name and then by guid across the pool, which is
what survives a rename or a promote, each one it finds is held for
the length of the report and not a moment longer, and what it cannot
find it names -- with every action that would have had to be read
against that tree reported unchecked rather than guessed at.

    zfs_rebase --abort --result DATASET

releases the holds, puts the result back -- destroying the clone, or
rolling the dataset back to its pre-apply snapshot, destroying that
snapshot, taking every zfs_rebase: property off it and mounting it
where it belongs again -- destroys any snapshot the tool took for
itself, unlinks the manifest the record names and removes the run
directory: as if the run never happened.

A dataset that already carries a record is not rebased over. If its
rebase reached done, --overwrite replaces the record with the new
run's; without the flag the tool says so and exits 2. A record in
any other state is an open rebase, and no flag overrides that:
--continue or --abort settles it first.

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

Installing:

    make            # or make freebsd, for the real tool
    make install    # PREFIX=/usr/local unless you say otherwise

install puts whatever zfs_rebase the build left in place into
$(DESTDIR)$(PREFIX)/sbin -- sbin because the tool must run as root --
and zfs_rebase.8 into $(DESTDIR)$(PREFIX)/share/man/man8, which is
where FreeBSD's base system and its ports tree both keep man pages
today. It builds nothing itself, so build the flavour you mean first:
a portable core installed on a FreeBSD box would refuse every real
run. ports/sysutils/zfs_rebase is the port skeleton that wraps the
same target.

License: BSD 3-Clause (see LICENSE). tools/cstyle.pl is OpenZFS's
and remains under CDDL-1.0, as its header says.
