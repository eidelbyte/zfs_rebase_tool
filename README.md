# zfs_rebase

A standalone command that rebases one ZFS filesystem onto another:
given two sides that diverged from a common base, it replays the
changes of one (from) onto the other (onto), or tells you exactly
which files it could not decide and why.

    zfs_rebase [-p] [-v] [-q] [--manifest FILE] \
        [--allow-unrelated --base SNAP] \
        [--take-onto | --take-from] [--interactive [CMD]] [--no-merge] \
        --from SNAP|DATASET --onto SNAP|DATASET --result NAME
    zfs_rebase --dry-run [-p] [--manifest FILE] \
        --from SNAP|DATASET --onto SNAP|DATASET
    zfs_rebase --continue [--interactive [CMD]] [--no-merge] \
        [--from SNAP] [--onto SNAP] IDENT
    zfs_rebase --restart IDENT
    zfs_rebase --abort IDENT
    zfs_rebase --verify IDENT

Every flag has a long form and a short form, and the two are the same
flag: the table below gives both. --from may also be spelled --off-of
and --onto --to, neither with a letter of its own. --dry-run (-n)
writes the manifest, creates nothing and holds nothing: to the file
-o names, or to standard output when there is none. --verify is a
verb and only a verb: the checks run on a schedule of their own, no
flag asks for one, and beside anything that starts or moves a rebase
the word is a usage error.

A verb names the rebase it acts on with IDENT, the one thing a
command line of this tool takes that is not a flag. It is resolved in
five steps, the first that matches winning:

 1. an absolute path: the manifest at it, whose header names the
    result. A path with no file at it is refused there and never
    looked for as a name;
 2. a dataset carrying the record whose name is IDENT, or ends in
    `/IDENT`: the clone form's result, in full or by the last part of
    its name;
 3. a snapshot of a dataset carrying the record, named `IDENT` after
    the `@` or spelled out as `pool/fs@IDENT`: the dataset form's
    pre-apply snapshot;
 4. a relative path to a manifest;
 5. a run directory, `/var/db/zfs_rebase/IDENT`, which is what a
    crash before the record leaves for --abort to remove.

Steps 2 and 3 search every imported pool. Two rebases that answer to
one IDENT are refused with both printed and never chosen between.
Whatever step matched, the identifier is held against ZFS's own naming
rule before it is ever made into a path, and as what it is spelled as:
a word with an `@` in it is held against the snapshot rule, so a
well-formed snapshot name that names no rebase is refused as that and
not as no name at all. --result is a start's flag and no verb takes
it; a start writes a manifest and reads none, so it takes no IDENT.

## Options

| long form | short | what it does |
|-----------|-------|--------------|
| `--from`, `--off-of` | `-f` | the side whose changes are replayed: a snapshot, or a dataset the tool snapshots itself and destroys at done or --abort. On a verb it is optional and names no rebase: it is checked against the header, by name and by guid |
| `--onto`, `--to` | `-t` | the side they are replayed onto, and the form of the run: a snapshot is cloned as --result, a dataset is rebased in place |
| `--result` | `-r` | the clone's name in one form and the pre-apply snapshot's in the other. A start's flag only: a verb names its rebase with IDENT, and --result beside one is a usage error |
| `--permissive-merge` | `-p` | a hard link on one side that crosses the other side's opinion of the same file is followed rather than raised as a conflict: two pools a pivot links are joined, and a name one side made a link of a file the other side edited adopts that file's outcome. Strict, the default, raises both as conflicts (unexpressed-sharing, disagree). The mode is recorded in the manifest's `#mode` |
| `--verbose` | `-v` | counts and steps on stderr |
| `--manifest` | `-o` | where the manifest is written; the resolution goes beside it, and the record names the manifest. A start option, and a dry run's: the record names the path from then on, and done acts on it, so no later verb can choose |
| `--verify` | `-V` | a verb: report one rebase and write nothing anywhere; never a repair. It goes with no flag that starts or moves a rebase, since the checks are standard |
| `--quiet` | `-q` | a start option: latched in the record for the whole run, and it silences the final check's report and nothing else -- not the check, not its verdict, not the exit status |
| `--take-onto` | `-O` | write the skeleton with every conflict answered onto |
| `--take-from` | `-F` | write the skeleton with every conflict answered from; the two exclude each other |
| `--interactive` | `-i` | open a child on the resolution at the conflicts gate, wait for it, and read the document back when it exits 0; anything else leaves the gate standing. Its optional value is the command that edits the file -- run as `sh -c 'CMD "$@"' CMD PATH`, so the command's own flags are part of it (`"code --wait"`) and the path is its last argument -- and with no value the built-in picker opens (this build has none yet: it says so and exits 2, which leaves the gate standing like any other non-zero exit). A bare word after -i is that command unless it is the only bare word on a verb's line, where it is IDENT; `--interactive=CMD` always names the command, and an empty value there is refused. For this invocation only (not latched: say it on each --continue) |
| `--no-merge` | `-M` | stop at the conflicts gate however the resolution reads; an error once the gate is passed |
| `--continue` | `-c` | take the rebase on from the gate its record names |
| `--restart` | `-R` | the result back as onto was, the manifest applied again from the first gate, the resolution back to its skeleton |
| `--abort` | `-A` | holds released, tool-made snapshots destroyed, the clone destroyed or the dataset rolled back, the run directory and the documents in it removed (a `-o` pair stays) |
| `--dry-run` | `-n` | decide and write the manifest, then tear down: nothing held, nothing created, --result ignored. It reaches no gate, so -q, -O, -F, -M and -i are all usage errors beside it: there is nothing for any of them to act on and no record to latch one in |
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
`canmount` back to what the header says they were, mounted where
its `mountpoint` property says -- that property is never touched, so
home is where it always was -- and then asked whether it is there. A
rebase waiting for its conflicts to be
answered is a half rebased tree, and a half rebased tree is not put
back into service: holding a filesystem in that state where anyone
can reach it is worse than holding it out of service while it is
settled. A caught signal at a gate leaves it privately mounted, as a
hard kill does, and the next --continue, --verify or --abort takes it
from there.

That hand-back is the first thing done and --abort do and not the
last. The order both keep is the order a run took things in,
reversed, with the bookkeeping last: the walks closed, the result
put back into service, then the holds released, the record taken
off, the snapshots the rebase owned destroyed and the run directory
removed. So nothing is released until the tree is back where it
belongs, and the record -- which is the only thing that names the
manifest, and through it the tag, the pre-apply snapshot and the two
property values -- is the last of it to go.

An unmount refuses when anybody holds the mount: an open file, a
working directory, a child mount. At the conflicts gate a person is
expected to be working inside the private mount, so a shell left
there is the ordinary case. When that happens nothing after the
unmount is done: the record and the holds stay, the run directory
stays, the mount path and the reason are printed, and the exit is 3.
The next --continue or --abort finishes the settle once the mount is
free. done never blocks on drift; this is the one thing it does
block on, because giving the dataset back is the promise the dataset
form makes.

A dataset whose `canmount` is `off` has no home to be handed back to,
and the dataset form refuses it at precondition with exit 2, before
anything at all is taken or written.

A reboot in the middle of a rebase leaves the result unmounted in
either form: nothing at boot mounts a dataset whose `mountpoint` is
`none`, and nothing mounts one whose `canmount` is `noauto`. So a
half rebased tree is never in service after a reboot either, and the
next --continue or --abort mounts it privately again from there.

What every verb works from is the dataset carrying the rebase's
record -- the clone in one form, onto itself in the other -- and
IDENT is how it is found, by the five steps above. A path names it
through the header, which names its run: #result in the clone form
and the dataset of #onto in the dataset form, where #result is the
pre-apply snapshot as --result spelled it. Whichever step matched,
the two halves are then held against each other -- the record's
`zfs_rebase:manifest` must be the file, and that file's header must
name this result back -- and a mismatch is refused with exit 2,
saying both sides. So two runs given the same `-o` path cannot be
applied to each other's result. A manifest whose result carries no
record is "not a zfs_rebase result", as a name that found no record
is.

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
those walks, decides by a small rule over names, hardlink pools and
content, writes a manifest of actions and conflicts, and applies the
actions to the result, which it puts back read-only. All ZFS
operations go through libzfs_core and libzfs; nothing is exec'd.

The unchanged set is the prune. A pool of either side is declared to
hold what base holds -- and is then never read -- when the object
number, the type, the link count, the number of names, the
generation number and the change time to the nanosecond all agree
with base's, every one of its names is in that same base pool, and
the extended attributes and both ACLs compare equal.

What the object number, the generation and the change time are
trusted for is the bytes of the object and the attributes its znode
keeps -- the mode, the owner, the group, the flags, the size: ZFS
moves a ctime for a write and for every one of those, and an object
number is only ever reused by an object of a later generation. The
extended attributes are compared rather than trusted because one
storage escapes the ctime: an attribute written into an object's
hidden directory (xattr=dir, and the fallback an SA write takes
whenever it fails) never touches the object's own znode, so a from
side whose only change was such an attribute would otherwise be
dropped in silence. The two ACLs are compared beside them because it
costs nothing and it keeps the answer from ever being looser than a
read's. Every field is already in memory from the walks, so the
prune reads nothing from disk and asks ZFS nothing. The OpenZFS
source line for each of those facts is listed above zr_oracle_prune
in src/yellow.h, and that list is meant to be checked against rather
than rediscovered. --allow-unrelated turns the prune off, because an
object number in one lineage means nothing in another.

The names in the manifest, in the resolution and in the name table
are the tree's own: tree-relative, written with a leading slash, and
never a path on the running system. Where the result is mounted is
the run's business and no document's, and no mount prefix ever
reaches the engine or either document.

Special files are carried like everything else. A fifo or a device
node is made with mkfifoat(2) or mknodat(2), and a unix-domain
socket the one way one can be made, by binding one; the mode, the
owner, the times and the rest go on afterwards as for any other
object, and an address too long for a socket is refused in words
rather than truncated. On FreeBSD the bind is bindat(2), which takes
the parent's descriptor, so only the leaf name is measured and the
depth of the run directory is out of it. Elsewhere it is bind(2) at
the root's path with the action's appended, both lengths are in the
measurement, and the refusal says which of the two is at fault: the
path in the tree, which no root would make room for, or the root
this result happens to be written at.

An object already standing where an action must act, and carrying an
immutable, append-only or no-unlink flag of either the system or the
user family, has those flags taken off before it is removed,
rewritten or given new attributes, and what the decision asks for
goes back on last. ZFS refuses the unlink, the write and the
attribute change alike on such an object, so an apply that did not
clear them would stop part way through the tree. Above securelevel 0
the system three cannot be cleared at all, so two kinds of object
are refused at precondition with exit 2, naming the first such name
and the side it is on, before anything at all is touched: one of
onto's carrying schg, sappnd or sunlnk at a name the decision would
remove, rewrite or re-pool, which the apply would have to unlock and
could not; and one of from's carrying one of them that the decision
would write into the result, which the apply would lock by writing
the flag on and which a --continue redoing the action, or the
applying1 self-check putting it back, would then be unable to touch.
The user three (uchg, uappnd, uunlnk) are not in that guard: they
come off for the owner at any securelevel, and ZFS refuses to set
them at all.

A rebase outlives the process that started it. While it is open the
result carries a record of four user properties -- set by the create
itself in the clone form, and on the dataset before anything is
touched in the other -- read back as local values only:

    zfs_rebase:phase       the last gate the run passed: decided,
                           applying1, conflicts or applying2
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

The file that record names exists from the record's first instant.
The run writes the whole header, with `#actions 0` and `#conflicts
0` and no body, before it writes the record, and writes the decision
over it later; so the order a run acquires things in is the pre-apply
snapshot (dataset form), the run directory, that header, the record,
the holds, the snapshot it takes of from, the take, and then the
walks, the decision and the skeleton. Every document is written to a
sibling `<file>.tmp` in the destination's own directory -- the
destination's, because rename(2) cannot cross a filesystem and a
`-o` file on another dataset is the ordinary case -- and renamed
over the destination, so a reader finds the document whole or as it
was and never half of either. A write that fails takes its own
`.tmp` away, and on success nothing is left behind; a `.tmp` beside
a `-o` file is what a crash in the middle of one write leaves, and
it is yours to remove.

There is one persistent hold per input snapshot under that tag, so that
none of the three can be destroyed while the rebase is open: zfs
holds shows the tag, and zfs destroy refuses with "dataset is busy".
A stranded rebase holds on purpose, because it is meant to be
resumed.

Its progress is a sequence of gates:

    decided -> applying1 -> conflicts -> applying2 -> done
    decided -> applying1 -> done               (no conflicts)

decided is written the moment the decision has been renamed over the
header the run was born with, and before the skeleton beside it: it
is what says the manifest is a decision and not that header. A record
with no phase at all is therefore a rebase born and never decided,
which is nothing to carry out: --continue and --restart refuse it in
those words, before they take the result over, and --abort takes it
away with everything the header gave it.
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
instead, and is refused once the gate is passed. The gate is
headless by default: this is a system tool, and it opens nothing of
its own unless it is asked to.
--interactive asks: at the gate the tool forks a child on the
resolution -- the command -i names, or the built-in picker where it
names none, which this build has only as a stub that says so and
exits 2 -- ignores SIGINT and SIGQUIT while it waits the way
system(3) does, forwards a SIGTERM of its own to it, puts the
terminal's termios back if the child left them changed, and reads
the document again when the child exits 0, with every refusal
--continue makes of one. Anything else and the gate stands with the
file as the child left it, exit 1, and nothing reopens the child by
itself: `zfs_rebase -c IDENT -i` carries on. The child opens
whenever the gate is reached, complete document or not, so -O -i
opens it on the answered skeleton and -M -i opens it and then holds
the gate; a rebase whose decision declared no conflict reaches no
gate and opens nothing. The gate's own work comes first -- the phase
written, and on a --continue the check whose drift lines go into the
document -- so the child sees the resolution as it stands, and the
line saying how many names are unanswered is printed after the child
and not before it. The flag is
not latched: it belongs to the invocation, so a --continue that
should open a child says -i each time, and a start given -i that
reaches the gate in the same process is interactive there.
applying2 carries the choices out. done is no phase and is never
written: when the result has verified and is read-only again, it is
handed back -- home, or to the void -- and then the holds are given
back and then every zfs_rebase: property is taken off, in that
order, since the tag is the only handle on those holds.
A result that carries any of them is therefore an open rebase, and
one that carries none has no rebase to move, whatever its history:
--continue, --restart and --abort all say so and touch nothing --
except that --abort, given a result with no record but a run
directory of its own, takes that directory away: it is what a crash
before the record left.
--verify is the exception, and only by the manifest, which is the
one thing a settled rebase left behind that still names it. What a
kill leaves is the last gate reached, there is no phase at all until
the decision, and a stop writes none: --continue resumes from the
gate, --abort takes the rebase away.

**The checks.** One verify at every gate, on a schedule no flag
changes and none can skip:

| when | what becomes of the drift |
|------|---------------------------|
| end of applying1 | fixed, by the stage's own self-check: up to the conflicts gate the result is the run's own, so a name that is not what the expected tree says is a stray, and no line is written |
| entering conflicts, and every --continue that arrives at that gate | written into the resolution as lines with the choice keep, printed, and never fixed: from this gate on the tree is being edited by hand, and nothing can tell that work from a stray |
| the done gate, after applying2 and before anything is given back | reported, exit 3, and the gate passed all the same -- done is no phase and is never written; what was found is written into the resolution with the choice `-`, which is the record of it, and a conflict line the manifest marks that the document lost is put back with `-` beside them; --quiet prints nothing and the exit status stands |
| a settled result, --verify with its manifest's path | reported, exit 3; the same check with no gate, against the header's identity, and nothing written |

Wherever it is made, a --verify writes nothing to the tree and moves
nothing that is where it should be: it reads the result where it is
mounted, mounts it only where it is mounted nowhere, and sets
readonly on nothing in either form.

The resolution is the authority from the conflicts gate on. Every
line is carried out by its path and its choice, whoever wrote it: a
line the tool wrote, a line the person changed and a line the person
added alike, and a conflict line added by hand for a name the
manifest never marked has its group number ignored, since it belongs
to no group. applying2 leaves every `keep` line exactly as it is and
carries out every `onto` and every `from` one, and it touches a name
the document speaks for by no other means: a conflict mark in the
manifest is a line the apply skips. What a hand edit cannot do is
take a conflict away: a conflict line the manifest marks that the
document no longer has is put back, at the conflicts gate with the
take mode's answer -- `onto` under `--take-onto`, `from` under
`--take-from`, `-` where the run was given neither -- and at the
done gate with `-`, since by then it is a record and not a question.
Two lines for one name are refused when the file is read, since they
are two instructions for one object. The verdict of every check then
reads two documents and nothing else: an action of the manifest not
done, or a line of the resolution not done, is drift.

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
for X is in place" -- or, where that result carries no record, it is
what a crash left before the record, and --abort removes it.
Removing it at done is what frees the name again.

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
with IDENT and takes -v and the two sides;
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
side that does not match is exit 2 with nothing touched. One or both
of them stand beside IDENT; --result is what a start calls the thing
it makes, and beside any verb it is refused, exit 2, saying what a
verb takes instead.

    zfs_rebase --continue [--interactive [CMD]] [--no-merge] \
        [--from SNAP] [--onto SNAP] IDENT

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
document it writes is the resolution: at the conflicts gate, where
the drift it found becomes lines with the choice keep for the person
to answer, and at the done gate, where what the last check found is
written with the choice `-` as the record of it. --no-merge stops it
at that gate however the resolution reads, and is refused from a
record already past the merge.

    zfs_rebase --restart IDENT

puts the result back as onto was -- destroying the clone and making
it again from the onto snapshot the header names, with the same
record, or rolling the dataset back to its pre-apply snapshot -- and
then applies the recorded manifest from the first gate, with the
resolution put back to its skeleton: the one the run wrote, which
the header's #take line says was answered onto, from or not at all. Nothing
is decided again: the manifest is the decision, a resolution's edits
are discarded by definition, and the instruction the rebase was
started with is not an edit.

    zfs_rebase --verify IDENT

reports and writes nothing at all, so a deliberate edit to a rebased
file is shown and never overwritten: it writes nothing to the tree
and moves nothing that is where it should be. It is a verb and only a
verb:
beside a start, beside --dry-run and beside --continue, --restart or
--abort it is a usage error, because the checks are standard and
there is nothing left for the word to ask for. It exits 0 when
nothing has drifted and 3 when something has -- an action pending or
drifted, a line of the resolution pending or drifted, or a name the
manifest never spoke for that the result no longer holds as onto had
it; blocked and unchecked are states and not faults, and a keep is
never compared. On a rebase in flight it is best effort: each input
is looked for by name and then by guid across the pool, which is
what survives a rename or a promote, each one it finds is held for
the length of the report and not a moment longer, and what it cannot
find it names -- with every action that would have had to be read
against that tree reported unchecked rather than guessed at. It
reads the result where it is mounted, which for an open rebase is the
run's private mount; where the result is mounted nowhere, which is
what a reboot or a hand leaves, it mounts it at the run directory's
`mnt` for the length of the read and unmounts it again, leaving the
run directory to the run. It never sets `readonly`, in either form:
the flag the stages flip is not a report's to touch.

A result whose rebase reached done carries no record, so its name
answers to no step of the resolution: the tool says it is not a
zfs_rebase result and asks for the manifest instead. The manifest is
what names a settled rebase, and only a `--manifest` one can: the
manifest a run writes for itself is unlinked at done with the run
directory. Given its path, `--verify` makes the final check over
again, with no gate to make it at:

- it wants both documents, the manifest and the resolution beside
  it, since the expected tree is onto's names with the manifest's
  actions and that resolution's choices applied. A resolution that
  is not there, or that cannot be read, is exit 2 saying so.
- the inputs are the header's. Each of base, from and onto is looked
  up by the name the header kept and its guid must be the guid the
  header kept; a name that is gone, or that another snapshot wears
  now, is exit 2 naming the snapshot and printing both numbers. No
  pool is searched: a settled rebase is checked against the identity
  it wrote down, or not at all.
- where a side was given as a dataset, `#made` says the tool took
  that snapshot itself and done destroyed it. That one is gone by
  design: the check covers the names and onto's bytes, says which
  actions it leaves unchecked, and exits 0 all the same.
- the result is read wherever it is mounted -- a dataset is at home,
  and so is a clone you have placed. A settled clone in the void is
  mounted nowhere, so the check mounts it at the run directory's
  `mnt` for the length of the read and takes that mount and the
  directory away again, whatever it found. No property of the result
  is touched either way, and nothing is fixed.
- exit 0 clean and 3 with drift, by the same rule as every other
  check.

    zfs_rebase --abort IDENT

puts the result back first -- destroying the clone, or rolling the
dataset back to its pre-apply snapshot and mounting it where it
belongs again with `readonly` and `canmount` as the header kept them
-- and only then releases the holds, destroys the snapshots the
rebase owned, takes every zfs_rebase: property off the result,
unlinks the two documents the run wrote into its own directory and
removes that directory: as if the run never happened. A --manifest
pair is the exception, and is left where you asked for it, here
exactly as at done.

It stops where a step refuses and does nothing after it, so that
what is left is a rebase a second --abort can find: a rollback that
cannot be made -- the snapshot gone, or a newer one in the way -- and
a private mount somebody is standing in both leave the record, the
holds and the documents exactly as they were, with the reason
printed and a non-zero exit.

A run directory whose result carries no record, or whose result is
not there at all, is the other half of --abort, and what the
directory holds says which of three cases it is. A directory with no
manifest in it is the window before the first write: it is removed,
and saying so is all there is to say. A birth manifest -- a header
with `#actions 0` and `#conflicts 0` -- is the window between that
write and the record: nothing was held and nothing was written to
the result, so what goes with the directory is the snapshot the tool
took of from and, because this window proves the run never reached a
record, the dataset form's pre-apply snapshot too. A decision
manifest is a rebase whose result was destroyed under it -- a
--restart whose second clone failed, or a `zfs destroy` by hand --
and its holds are the thing that must not be left behind: the tag is
released on the three snapshots the header names, the tool's own
snapshot destroyed, the documents unlinked and the directory
removed. Before this the holds under that tag were unreachable,
since the record that named the tag went with the dataset. The
pre-apply snapshot is named and left in that last case: a rebase
that lost its result and a done whose unlink failed have the same
shape, and one of them means to keep it. A manifest that will not
parse is the one thing --abort refuses, since a file that says a
rebase was here and cannot be read is not a directory to remove
quietly: it says so, releases nothing, removes nothing and exits 2.

Which of those it does is the manifest's to say, and the file is
there at every gate, since the run writes its header before its
record. Where it has been lost all the same -- removed by a hand --
--abort gives the holds back by walking the result's pool
for the record's tag, destroys the snapshot the run took for itself
where it took one -- that walk finds it held under the tag and named
with it -- undoes the private mount and takes the record off, and
then says plainly what it cannot do without the manifest: the form,
the pre-apply snapshot and the two property values went with the
file, so it destroys nothing
else and rolls nothing back, and it cannot put readonly or
canmount back -- it prints the two `zfs set` commands for that. The
one thing it can still read is the `mountpoint` property, which the
two forms never share: a path is a dataset of yours and is mounted at
it, `none` is a clone of the tool's and is left unmounted for you to
destroy or to place. It prints the command for each form and leaves the
choice to you.

That leftover is --abort's alone among the verbs, and step 5 of
IDENT's resolution is how a name reaches it: a run directory of that
name with no record on any dataset. The other three verbs say there
is no rebase to move and name --abort.

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
