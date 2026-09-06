# zfs_rebase

A standalone command that rebases one ZFS filesystem onto another:
given two sides that diverged from a common base, it replays the
changes of one (from) onto the other (onto), or tells you exactly
which files it could not decide and why.

    zfs_rebase [-p] [-v] [-q] [--manifest FILE] \
        [--allow-unrelated --base SNAP] \
        [--take-onto | --take-from] [--no-gui] [--no-merge] \
        --from SNAP|DATASET --onto SNAP|DATASET --result NAME
    zfs_rebase --dry-run [-p] [--manifest FILE] \
        --from SNAP|DATASET --onto SNAP|DATASET
    zfs_rebase --continue [--no-gui] [--no-merge] \
        [--from SNAP] [--onto SNAP] (--result DATASET | MANIFEST)
    zfs_rebase --restart (--result DATASET | MANIFEST)
    zfs_rebase --abort (--result DATASET | MANIFEST)
    zfs_rebase --verify (--result DATASET | MANIFEST)

Every flag has a long form and a short form, and the two are the same
flag: the table below gives both. --from may also be spelled --off-of
and --onto --to, neither with a letter of its own. --dry-run (-n)
writes the manifest, creates nothing and holds nothing: to the file
-o names, or to standard output when there is none. --verify is a
verb and only a verb: the checks run on a schedule of their own, no
flag asks for one, and beside anything that starts or moves a rebase
the word is a usage error.

A verb names the rebase it acts on by --result, the dataset carrying
the record, or by MANIFEST, the path of that rebase's manifest, which
is the one thing a command line of this tool takes that is not a
flag. A start writes a manifest and reads none, so it takes no
MANIFEST at all.

## Options

| long form | short | what it does |
|-----------|-------|--------------|
| `--from`, `--off-of` | `-f` | the side whose changes are replayed: a snapshot, or a dataset the tool snapshots itself and destroys at done or --abort. On a verb it is optional and names no rebase: it is checked against the header, by name and by guid |
| `--onto`, `--to` | `-t` | the side they are replayed onto, and the form of the run: a snapshot is cloned as --result, a dataset is rebased in place |
| `--result` | `-r` | the clone's name in one form and the pre-apply snapshot's in the other; for every verb, the dataset carrying the record, which MANIFEST names instead |
| `--permissive-merge` | `-p` | permissive merge; strict is the default, and the mode is recorded |
| `--verbose` | `-v` | counts and steps on stderr |
| `--manifest` | `-o` | where the manifest is written; the resolution goes beside it, and the record names the manifest. A start option, and a dry run's: the record names the path from then on, and done acts on it, so no later verb can choose |
| `--verify` | `-V` | a verb: report one rebase and write nothing anywhere; never a repair. It goes with no flag that starts or moves a rebase, since the checks are standard |
| `--quiet` | `-q` | a start option: latched in the record for the whole run, and it silences the final check's report and nothing else -- not the check, not its verdict, not the exit status |
| `--take-onto` | `-O` | write the skeleton with every conflict answered onto |
| `--take-from` | `-F` | write the skeleton with every conflict answered from; the two exclude each other |
| `--no-gui` | `-G` | at the conflicts gate, go on without the picker when the resolution is complete and stop when it is not -- the only behavior while there is no picker |
| `--no-merge` | `-M` | stop at the conflicts gate however the resolution reads; an error once the gate is passed |
| `--continue` | `-c` | take the rebase on from the gate its record names |
| `--restart` | `-R` | the result back as onto was, the manifest applied again from the first gate, the resolution back to its skeleton |
| `--abort` | `-a` | holds released, tool-made snapshots destroyed, the clone destroyed or the dataset rolled back, the run directory and the documents in it removed (a `-o` pair stays) |
| `--dry-run` | `-n` | decide and write the manifest, then tear down: nothing held, nothing created, --result ignored |
| `--allow-unrelated` | `-u` | no derivation of the base, and no pruning; it needs --base |
| `--base` | `-b` | with --allow-unrelated only, and it needs one: the base, no newer than either side |

A long form takes its value as `--from SNAP` or as `--from=SNAP`; a
short form takes the argument after it, and short flags do not bundle
(`-nv` is not `-n -v`). `--posix`, `--build-fixture` and
`--edit-fixture` are the project's own test aids: they are long only
and take the first argument position.

Each side is a snapshot of yours or a dataset the tool snapshots for
itself, and --onto decides the form of the run.

**The clone form**, --onto a snapshot. --result names a new dataset,
created as a read-only clone of that snapshot with its `mountpoint`
property `none`, and mounted at the run's own private directory,
which is the only place it is ever mounted while the rebase is open.
The rebase is made in it and nothing of yours is written to at all.
At done the clone is handed to the void: unmounted, read-only, its
`mountpoint` still `none`, and the tool says how to place it --

    zfs_rebase: tank/rebased is the rebased tree, unmounted; place it
    with zfs inherit mountpoint tank/rebased or zfs set
    mountpoint=PATH tank/rebased

-- because where a rebased tree belongs is yours to say and not the
tool's. It is yours to promote, rename or inherit as well; the tool
never promotes.

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

The take also sets `canmount` to `noauto` and puts `readonly` to off,
both of them while the dataset is off its mountpoint; the manifest's
header remembers what each of them was. (libzfs answers a readonly
change on a mounted dataset with a remount at the mountpoint
property, which cannot be done while the dataset sits at the private
mount, so the property is only ever touched unmounted. The private
mount is root's alone and writable for its whole life.)

The dataset then stays at the private mount for the whole of the
rebase, the conflicts gate included, and is handed home exactly
twice: at done and at --abort (and by a run that gives up before it
has written anything, which takes itself away as an --abort would).
Off the private mount, `readonly` and
`canmount` back to what the header says they were, and mounted where
its `mountpoint` property says -- that property is never touched, so
home is where it always was. A rebase waiting for its conflicts to be
answered is a half rebased tree, and a half rebased tree is not put
back into service: holding a filesystem in that state where anyone
can reach it is worse than holding it out of service while it is
settled. A caught signal at a gate leaves it privately mounted, as a
hard kill does, and the next --continue, --verify or --abort takes it
from there.

A dataset whose `canmount` is `off` has no home to be handed back to,
and the dataset form refuses it at precondition with exit 2, before
anything at all is taken or written.

A reboot in the middle of a rebase leaves the result unmounted in
either form: nothing at boot mounts a dataset whose `mountpoint` is
`none`, and nothing mounts one whose `canmount` is `noauto`. So a
half rebased tree is never in service after a reboot either, and the
next --continue or --abort mounts it privately again from there.

--result for every verb is the dataset carrying the rebase's record
-- the clone in one form, onto itself in the other -- and a snapshot
name is taken as its dataset, so both spellings find the same rebase.
MANIFEST names the same rebase the other way about: the header names
its run, which is #result in the clone form and the dataset of #onto
in the dataset form, where #result is the pre-apply snapshot as
--result spelled it. Given both, the two must name each other -- the
record's `zfs_rebase:manifest` must be that file, and the header must
name that dataset -- and a mismatch is refused with exit 2, saying
both sides. A manifest whose result has no record is "not a
zfs_rebase result", as a --result naming that dataset would be.

A dataset given as a side is snapshotted by the tool under a
generated name, named as tool-made in the manifest's header, and
destroyed when the rebase
ends, at done or at --abort. If you wanted that snapshot kept you
would have passed one. A dataset onto never carries a tool-made
snapshot: its before-image is the one you named.

The exit status is 0 when the rebase is done, 1 when it stopped at
conflicts, 2 when it was refused before anything was touched, and 3
when something failed part way or when the final check found drift.
Those last two are told apart by what is left behind: a rebase whose
final check found drift has reached done all the same, and carries
no record any more.

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
.zfs/snapshot like the other two. The two flags go together: --base
without --allow-unrelated is a usage error, because where the branch
point is derived a base given by hand could only agree with it or be
wrong, and --allow-unrelated without --base is one too, because with
nothing to derive and nothing given there is no third tree to read
the two sides against. (Reading them against the empty tree instead
made every name of either side an add and every disagreement a
conflict, which is a decision about two trees that were never
compared; that is gone, ruled 2026-09-06.)

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

A rebase outlives the process that started it. While it is open the
result carries a record of four user properties -- set by the create
itself in the clone form, and on the dataset before anything is
touched in the other -- read back as local values only:

    zfs_rebase:phase       the last gate the run passed: applying1,
                           conflicts or applying2
    zfs_rebase:manifest    where the manifest was written
    zfs_rebase:tag         the tag its holds are filed under
    zfs_rebase:quiet       "yes", where the start was given --quiet

Everything else about the rebase is in the header of that manifest:
the three snapshots and their guids, the form, the mode, which side
the tool snapshotted itself, how the skeleton was answered, and in
the dataset form the pre-apply snapshot and the readonly and canmount
values to give back. Either document names the run -- the property
points at the manifest, the header names the result -- and nothing
else is ever written to a dataset by this tool.

There is one persistent hold per input snapshot under that tag, so that
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
applying2 carries the choices out. done is no phase and is never
written: when the result has verified and is read-only again, the
holds are given back and then every zfs_rebase: property is taken
off, in that order, since the tag is the only handle on those holds.
A result that carries any of them is therefore an open rebase, and
one that carries none has no rebase, whatever its history. What a
kill leaves is the last gate reached, there is no phase at all until
the first one, and a stop writes none: --continue resumes from the
gate, --abort takes the rebase away.

**The checks.** One verify at every gate, on a schedule no flag
changes and none can skip:

| when | what becomes of the drift |
|------|---------------------------|
| end of applying1 | fixed, by the stage's own self-check: up to the conflicts gate the result is the run's own, so a name that is not what the expected tree says is a stray |
| entering conflicts, and every --continue that arrives at that gate | written into the resolution as lines with the choice keep, printed, and never fixed: from this gate on the tree is being edited by hand, and nothing can tell that work from a stray |
| end of applying2, before done is written | reported, exit 3, and done written all the same; --quiet prints nothing and the exit status stands |
| a settled result, --verify MANIFEST | reported, exit 3 (verify-settled's work; today a settled result has no record and every verb on it exits 2) |

done never blocks on drift. What the last check finds is said and
carried out in the exit status, and the gate is passed regardless:
the record cleared, the result settled and the run directory taken
away, exactly as on a clean pass. A rebase that could not be closed
because somebody edited a file in it would be a rebase nothing could
ever end. A check that cannot be made at all is the other thing --
that is the tool failing, not the tree drifting -- and the gate is
not passed, so a --continue can try again.

Each run keeps its own directory, 0700 throughout: born at start,
gone at done.

    /var/db/zfs_rebase/<result as a path>/mnt          the private mount
    /var/db/zfs_rebase/<result as a path>/manifest     unless --manifest
    /var/db/zfs_rebase/<result as a path>/resolution   the choices

`mnt` is where the result lives for the whole of the rebase, in both
forms: the clone is mounted there at birth and the dataset the moment
it is taken over, neither of them through the `mountpoint` property,
which is what lets that property stay `none` in one form and stay
where it always pointed in the other. The rebase ends by undoing that
mount.

Making the directory is the lock: a leaf that is already there is
another run of the same result, and the run is refused with "a run
for X is in place". Removing it at done is what frees the name again.

At done the run unlinks the two documents it wrote into that
directory, and then `mnt`, the directory and every empty parent up
to /var/db/zfs_rebase go by rmdir -- never recursively, so a
directory another run shares simply stays. This is the same in both
forms and whichever invocation reaches done, the fresh run's own or a
--continue's. A failure to remove is printed and changes nothing
else: the rebase is done.

What a rebase that reached done leaves is the result -- the clone,
unmounted, for you to place, or the dataset back at home with its
before-image beside it -- and a --manifest pair if you asked for one.
Nothing else.

With --manifest FILE (-o FILE) the manifest is FILE and the
resolution is FILE.resolution, beside it. Either way the record names
the manifest and the resolution is beside it by that rule, so every
verb finds both and never by guessing a path. A pair you named that
way is yours: the tool records it and never removes it, at done or at
--abort, where its own two go with the run directory. The tool
removes no file outside that directory.

Not /var/run: FreeBSD's cleanvar deletes every regular file there at
boot, and a rebase stopped at conflicts can outlast one.

The clone form's result is yours to promote, rename, place or inherit
as you see fit: the tool never promotes, and at done it leaves the
clone unmounted with no mountpoint of its own. The dataset form's
result is the dataset you already had, back at its own mountpoint
with the rebase in it, its `readonly` and `canmount` as they were.

Four verbs work on a rebase that already exists. Each names its run
-- --result, or MANIFEST, or both -- and takes -v and the two sides;
--continue takes the flags of the gate as well, and nothing else is
theirs. -o is not theirs: the start chose where the manifest goes,
the record names it from then on and done acts on it, so there is no
later moment at which the choice could be made. Every one of them
keys on the record and on the manifest it names, and on nothing else:
a dataset that carries no record is refused untouched, and an
inherited value is no record, since user properties inherit down the
naming tree and only a local one is ours. Each also checks that every
snapshot the header names is still the snapshot it named, by guid,
since a snapshot destroyed and taken again under the same name is
another snapshot and these answers do not describe it.

--from and --onto are optional on all four and change nothing: a verb
reads the two sides out of the header. What they do is say which
rebase the person thinks this is, and each is checked against the
header by name and by guid, both numbers printed on a mismatch. A
side that does not match is exit 2 with nothing touched.

    zfs_rebase --continue [--no-gui] [--no-merge] \
        [--from SNAP] [--onto SNAP] (--result DATASET | MANIFEST)

takes the rebase on from the gate its record names, through the
gates that are left, in one process. Applying is idempotent -- every
action means "make this true" -- so what is already true is left
alone and what is not is made, which is why a fresh run and a resumed
one are one code path. It checks at every gate it passes, under no
flag, by the schedule below, and it prints what it found: how every
action of the manifest and every answered name of the resolution
stands -- done, pending, blocked, drifted or unchecked -- and what
the result holds outside them both. It repairs nothing: the one fix
in the tool is the applying1 stage's own self-check. The one
document it writes is the resolution, at the conflicts gate, where
the drift it found becomes lines with the choice keep for the person
to answer. --no-merge stops it at that gate however the resolution
reads, and is refused from a record already past the merge.

    zfs_rebase --restart (--result DATASET | MANIFEST)

puts the result back as onto was -- destroying the clone and making
it again from the onto snapshot the header names, with the same
record, or rolling the dataset back to its pre-apply snapshot -- and
then applies the recorded manifest from the first gate, with the
resolution put back to its skeleton: the one the run wrote, which
the header's #take line says was answered onto, from or not at all. Nothing
is decided again: the manifest is the decision, a resolution's edits
are discarded by definition, and the instruction the rebase was
started with is not an edit.

    zfs_rebase --verify (--result DATASET | MANIFEST)

reports and writes nothing at all, so a deliberate edit to a rebased
file is shown and never overwritten. It is a verb and only a verb:
beside a start, beside --dry-run and beside --continue, --restart or
--abort it is a usage error, because the checks are standard and
there is nothing left for the word to ask for. It exits 0 when
nothing has drifted and 3 when something has -- an action pending or
drifted, or a name the manifest never spoke for that the result no
longer holds as onto had it; blocked and unchecked are states and
not faults. After done it is best effort: each input
is looked for by name and then by guid across the pool, which is
what survives a rename or a promote, each one it finds is held for
the length of the report and not a moment longer, and what it cannot
find it names -- with every action that would have had to be read
against that tree reported unchecked rather than guessed at. A
result whose rebase reached done carries no record and no verb finds
it; naming a settled result by its manifest is verify-settled's.

    zfs_rebase --abort (--result DATASET | MANIFEST)

releases the holds, puts the result back -- destroying the clone, or
rolling the dataset back to its pre-apply snapshot, destroying that
snapshot, taking every zfs_rebase: property off it and mounting it
where it belongs again -- destroys any snapshot the tool took for
itself, unlinks the two documents the run wrote into its own
directory, and removes that directory: as if the run never happened.
A --manifest pair is the exception, and is left where you asked for
it, here exactly as at done.

Which of those it does is the manifest's to say. Where that file has
been lost, --abort gives the holds back by walking the result's pool
for the record's tag, undoes the private mount and takes the record
off, and then says plainly what it cannot do without the manifest:
it cannot tell the clone form from the dataset form, so it destroys
nothing and rolls nothing back, and it cannot put readonly or
canmount back -- it prints the two `zfs set` commands for that. The
one thing it can still read is the `mountpoint` property, which the
two forms never share: a path is a dataset of yours and is mounted at
it, `none` is a clone of the tool's and is left unmounted for you to
destroy or to place. It prints the command for each form and leaves the
choice to you.

A dataset that carries any zfs_rebase: property of its own is an open
rebase and is not rebased over: the tool says so and exits 2, and
--continue or --abort settles it first. A dataset that carries none
is free, whatever its history -- a rebase that reached done took its
record off.

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
