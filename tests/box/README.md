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
    sudo sh tests/box/run-precond.sh   # the cells no fixture states

run-fixture.sh builds its trees under mktemp -d /tmp/zr-box.XXXXXX,
and the fixtures in tests/fixtures/freebsd/ carry NFSv4 ACLs and
system-namespace extended attributes, which a tmpfs /tmp cannot hold
at all: setting one there fails with EOPNOTSUPP and the fixture fails
before the pool is even made. The box wants /tmp on ZFS (or UFS), or
TMPDIR pointed at a directory on one. Those fixtures are root's for
the same reason: the system attribute namespace is root's.

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
pool on a memory disk and runs the tool for real -- twice over, once
in each form.

The clone form first. The harness takes the snapshots -- base@base,
from@work and onto@work -- and hands the run only the two sides and
the name of the clone:

    zfs_rebase -n -o FILE --from POOL/from@work --onto POOL/onto@work
    zfs_rebase -v -o FILE --off-of POOL/from@work \
        --onto POOL/onto@work --result POOL/result
    zfs_rebase --abort --result POOL/result

Then the dataset form, after that pass has been aborted and the pool
is back to base, from and onto with their snapshots. Both sides are
given as datasets, so the tool takes its own snapshot of from and
--result names the pre-apply snapshot of onto:

    zfs_rebase -v -o FILE --from POOL/from --onto POOL/onto --result pre
    zfs_rebase --verify --result POOL/onto
    zfs_rebase --continue --result POOL/onto
    zfs_rebase --abort --result POOL/onto

and then the whole pass again with --result POOL/onto@pre, since the
short name and the full one must name the same snapshot of the same
dataset.

What that pass shows. The manifest is the clone form's manifest
exactly, from the #mode line on, and derives the same base: one
decision, two ways of carrying it out. The record is on POOL/onto
itself with zfs_rebase:form dataset, :made from, :readonly recording
what readonly was, :onto the pre-apply snapshot and :from the
snapshot the tool took of the from dataset, named after the run's own
tag; every one of them is local, against the bogus tag and manifest
on the pool root. The dataset is mounted at its own mountpoint again
when the run stops, readonly as it was, with the mountpoint property
untouched -- that is the hand-back, and it happens at conflicts, at
done, at a failure and after every verb, because a rebase waiting for
a conflict to be answered must not hold a filesystem out of service
while it waits. A clean fixture is at done with every hold released,
the tool's own snapshot of from destroyed, and the live tree the
rebased tree; a conflicted one is at conflicts with the clean actions
applied, the three holds under the record's tag, and the tool's own
snapshot still there. --abort rolls onto back to the pre-apply
snapshot, destroys that and the tool's own, leaves no zfs_rebase:
property local, and hands the dataset back holding exactly the tree
the fixture built -- which the harness proves by rebasing over it in
--posix form and getting the expect block back.

Two more things belong to this form. The exclusivity is the unmount,
so probe.zrt holds a file open under POOL/onto and runs the tool: it
exits 2 saying onto is in use, and takes back its record, both
snapshots and its run directory, leaving the dataset mounted where it
was. And --overwrite, on a clean fixture, whose dataset-form run
reaches done: a second run is refused without the flag and replaces
the record with it, and the finished rebase's before-image is still
there afterwards, since a rebase that reached done keeps it.

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
zfs_rebase:take "-" (no --take flag was given, so the skeleton was
written unanswered, and --restart reads this back to write the same
one again), the tag, the manifest path, the resolution path --
FILE.resolution beside the -o manifest, and the file itself an
unanswered skeleton with one line per conflicted name -- and
zfs_rebase:state -- "done" for a
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
and how many of its names are still unanswered, and leaves the tree
as stage 1 made it, which the --posix re-run is asked again to
confirm. For probe.zrt there are three more: a byte appended to /n, which the manifest copied,
behind the tool's back with readonly off and on again, must make
--verify exit 3 naming "drifted 1, first /n" and change nothing,
and --continue --verify must put it back and leave --verify clean
and the result read-only; and --restart must destroy the clone,
make it again from the recorded onto snapshot with the same record,
apply the manifest from the first gate and land at the same state,
under the same tag, with the same three holds and the same tree, with
the resolution put back to its skeleton; and then that skeleton
answered -- every "-" to keep, and #unanswered to 0 with them -- must
take --continue to done and release the holds, leaving the tree as it
was, since keep is the one choice that changes nothing. A choice of
onto or from does change something, and that is run-resolution.sh's.

Step 4 aborts the run and checks that the holds, the dataset, the
two documents the run recorded and the directory under /var/db are
all gone, and that a second --abort exits 2. Then a second real run
given --verify -- every clean fixture takes this, and probe.zrt takes
it as the conflicted one: zfs_rebase:verify is "yes" in its record
and its tag is a new one, and on a clean fixture the final check runs
at the done gate, so its report is printed, the state is done and the
holds are released before the abort. For probe.zrt there is also the
dry run given --verify, which must still create nothing, hold nothing
and leave no run directory. The dataset form's pass runs after all of
that. KEEP=1 leaves the pool for inspection. Its header says what it
does not exercise yet.

run-suite.sh walks tests/fixtures/ and tests/fixtures/freebsd/ and
runs every one of them through both forms; a failure prints the log's
last "==" heading, which names the form and the step it stopped at.

## The pause hook

    ZFS_REBASE_PAUSE=<gate> zfs_rebase ...

is how the harness gets inside a run. At the gate it names the tool
raises SIGSTOP on itself and stops there; the harness waits for the
process to go into the T state, does what it came to do -- signal it,
edit the result behind its back, look at the record, try to destroy a
held snapshot -- and sends SIGCONT, and the run goes on from exactly
that point. It is a test aid: it is in no usage text, an unknown gate
name is ignored in silence, and --posix reaches no gate at all and
ignores the variable.

A gate is a point where the thing it names has just happened and the
next thing has not started:

    held        the three holds are taken (a fresh run)
    cloned      the clone is there, or the dataset is the run's own
                at its private mount, before any walk
    read        the walks and the pruning are done, before anything
                is decided
    manifest    the manifest is written and recorded, before the
                skeleton of the resolution is written beside it:
                the one window in which a rebase has one of its two
                documents and not the other
    decided     the manifest and the resolution are written and
                recorded, before applying1 is written
    applying1   that state is written and readonly is off, before
                the first action (a fresh run or --continue)
    conflicts   that state is written, before the hand-back and the
                message (a fresh run or --continue)
    applying2   that state is written and readonly is off, before
                the choices of the resolution are carried out
                (--continue). A document answered keep throughout
                carries out to nothing, which is what the conflicted
                fixtures answer today
    done        that state is written, before the holds are released
    action:<n>  inside the apply, before the n'th action it performs,
                counting the ones it performs and not the ones a
                report let it leave alone
    choice:<n>  inside applying2, before the n'th line of the
                resolution the stage carries out, counting the makes,
                the links and the removals and neither the keeps nor
                a make it found already true. It is counted per call,
                so the stage's own second pass over the document
                reaches no line at all

So ZFS_REBASE_PAUSE=applying1 stops a run with the result writable
and nothing applied yet, action:2 stops it with the first action made
and the second not, and done stops it with the state written and the
holds still there. The verbs read the variable too, so a --continue
can be stopped at applying1, conflicts, applying2 or done; --verify
alone and --abort pass no gate and never stop.

manifest and choice:<n> are run-resolution.sh's. The first is the
only moment at which a rebase has a manifest and no resolution, and
what a kill there leaves is a rebase whose exits are --restart, which
writes the skeleton again from the recorded manifest, and --abort.
The second wants something to carry out, so the run that is stopped
at it is given --take-from: a conflicted name holds onto's object
when applying2 begins, so a document answered onto is already true
everywhere and the stage reaches no line.

The shape of every use of it is the same:

    ZFS_REBASE_PAUSE=applying1 zfs_rebase ... &
    pid=$!
    until [ "$(ps -o stat= -p $pid | cut -c1)" = T ]; do sleep 0.2; done
    ... whatever the test came to do ...
    kill -CONT $pid
    wait $pid

A SIGKILL needs no CONT; every other signal is delivered when the
process is continued, so the CONT comes after it.

## run-kills.sh

    sudo sh tests/box/run-kills.sh [FIXTURE.zrt ...]

crosses every gate with SIGINT, SIGTERM and SIGKILL, in both forms,
on a conflicted fixture (probe.zrt) and a clean one
(h-yw-row19.zrt), and takes each of them on with --continue
afterwards. One pool per fixture, --abort as the reset between
cases, and the invariants the reset must leave checked before the
next case starts. It prints one line per case and exits 0 only when
every one of them passed.

What a stop leaves is not one rule but three, and the script is
built around them:

- SIGINT and SIGTERM before applying1 -- at held, cloned, read and
  decided -- take the whole run away. Nothing has been written to
  the result yet, so a stop there is a failure before the apply and
  is treated as one: exit 3, the clone destroyed or the dataset's
  record taken off and the dataset put back, the holds released, the
  manifest unlinked, the run directory gone. There is nothing to
  continue and --continue says so. (This is where the plan's
  "nothing is destroyed or released before done or --abort" and the
  code part company; the code is deliberate and run.c says so.)
- Every SIGKILL, and SIGINT and SIGTERM from applying1 on, leave the
  rebase standing at the gate it had reached, with its record, its
  three holds, its manifest from "decided" on, and its result.
  readonly is back on wherever the tool was given the chance to put
  it back, and off after a SIGKILL inside an applying stage; in the
  dataset form a caught signal hands the dataset back home and a
  SIGKILL leaves it at the private mount, where the next verb takes
  it from.
- SIGINT and SIGTERM at done are no stop at all: nothing looks at
  the flag past that gate, so the run finishes and releases.

Then --verify says what the kill left without touching it (pending
before and inside applying1, nothing pending past it), --continue
takes the rebase to its branch's gate -- with --verify after a
SIGKILL and without after a caught signal -- and a --posix rebase of
the fixture's from onto the result declares zero actions, which is
stage 1 idempotence. While the tool is stopped at the held gate, zfs
destroy of each held input must fail and leave the snapshot
standing.

The conflicted fixture reaches applying2 and done only through an
answered resolution. The tool writes the skeleton itself when it
writes the manifest, so the harness answers it: every "-" becomes
keep, which leaves the conflicted names as they stand, and the
header's #unanswered goes to zero with them -- a hand edit has to
change the count too, since the parser refuses a header that does
not match its lines. An unanswered skeleton stops at conflicts,
which is what every gate before applying2 relies on.

The gate keys on that completeness and on the command, and never on
the file being there. Every run in this harness is given no --take
flag, so every skeleton it writes is unanswered and every run stops
at the gate as it always did. Three things change that, and all
three are run-resolution.sh's: --take-onto and --take-from write
the skeleton answered, which makes it complete from the start, so
the fresh run hands the result back and goes on to done in the same
process (its record then reads zfs_rebase:take onto or from, and
--restart writes that same answered skeleton again); --no-merge
stops a run or a --continue at the gate however the resolution
reads, and leaves the gate where it is for the next --continue
without it; and --no-merge on a --continue whose record is already
at applying2 or done is refused with "past the merge", exit 2, the
gate unmoved. --no-gui is accepted on a fresh run and on --continue
and changes nothing while there is no picker.

One thing this harness does say about --verify: a --continue given
it from before or inside applying1 records zfs_rebase:verify on the
result, which is all the flag means at that gate, and the done gate
of the same rebase makes the check.

## run-strays.sh

    sudo sh tests/box/run-strays.sh [FIXTURE.zrt ...]

puts edits where the tool is not looking and asks what happens to
them. Same fixtures, both forms, four cases each and a fifth on the
conflicted ones, every one ending in --abort with the pool proved to
be the fixture again.

Paused at action:1, with the result writable and no action
performed, the harness edits a file the manifest keeps untouched,
creates a name no tree had, and edits a file the manifest is about
to write. The run finishes at its branch's gate with all three
undone: the name the manifest wrote is the manifest's, because the
action ran after the edit, and the other two are put back by the
self-check the applying1 stage makes on itself -- the edited name
out of onto, the name no tree had taken away. Up to the conflicts
gate the result is the run's own, so anything that is not what the
expected tree says is a stray. --verify afterwards reports nothing
outside the manifest, and a --continue --verify has nothing left to
do.

A stray delete is caught by that same self-check: the name list is
over the shared name table and not over what the result holds, so a
name onto had that the result has lost is gone, and gone is
restored out of onto. The run does not stop for it and reaches its
branch's gate as if it had not happened.

Then drift after the stage, which is what --verify is for and what
nothing repairs -- an edit to a file a clean action made is drifted
1 naming it, --verify fixes nothing, and --continue --verify
reports it and fixes nothing either, because past applying1 an edit
made while the conflicts are being answered cannot be told from a
stray and a gate that failed on it would block done for good --
and, on the conflicted fixture, an edit to a conflicted name, which
is never classified and never touched, because answering a conflict
is the conflict manager's work.

Last, a stray write into the live from and onto datasets while the
run is reading, at the read gate: the tool reads snapshots, so the
manifest is the expect block to the byte. In the dataset form onto
is not even where it lives just then, so its own mount point is an
empty directory of the pool's root dataset, and a write there is
hidden the moment the dataset comes home.

And, on the conflicted fixtures, the one place a drift line is
written: an edit to a clean file while the rebase waits at the
conflicts gate, then --continue --verify, which turns every entry of
the name list into a drift line with the choice keep, writes the
resolution back and goes on. The document gains one name, the count
of unanswered stays at zero because a keep is an answer, the rebase
reaches done with the edit still in the tree, and a --verify
afterwards has nothing outside the manifest to say about that name:
it is the resolution's now, and a keep is never compared. Answering
the skeleton first is a hand edit, so the header's count is edited
with the lines.

The dataset form is given from as a snapshot here rather than as a
dataset, because a verify that cannot read from can only say
unchecked and a rebase that reaches done destroys a snapshot it
took itself.

## run-resolution.sh

    sudo sh tests/box/run-resolution.sh [FIXTURE.zrt ...]

is where a choice of onto or from is carried out for the first time.
Its default set is tests/fixtures/probe.zrt,
tests/fixtures/h-s2-two-conflicts.zrt and
tests/fixtures/freebsd/acl-conflict.zrt, each in both forms, and
every fixture it is given must declare a conflict: a rebase with none
never reaches the gate this script is about, and it says so rather
than passing vacuously. One pool per fixture, --abort as the reset
between cases, and the pool proved to be the fixture again before the
next one starts.

What it needs beyond what the other harnesses need: /tmp on ZFS or
UFS, because its default set carries an NFSv4 ACL fixture and the
note at the top of this file applies to it; the pause hook, for the
two gates named above; and getfacl, lsextattr and getextattr, which
is how a name in the result is held against the side's own object.
Both sides are given as snapshots in both forms, so that from's tree
is still there to compare against after done, and onto's own tree is
read out of the pre-apply snapshot in the dataset form and out of
onto@work in the clone form -- through .zfs/snapshot, which is where
the tool reads them too.

The cases, in the order they run, per fixture and form:

- headless to done under --take-onto --no-gui and then under
  --take-from --no-gui. Each writes its skeleton answered, which
  makes it complete from the start, so the run passes its own
  conflicts gate and reaches done in one process: exit 0 and not 1.
  The record then reads zfs_rebase:take onto or from, as its own
  local value against a bogus one on the pool root; every line of the
  document reads that side and none is left to answer; every
  conflicted name in the result is that side's object -- type, mode,
  ownership, bytes or link target, ACL and both namespaces of
  extended attributes -- or is gone where that side has no such name;
  the names of one group that side pools together are one object here
  too; a second --posix rebase declares no action, since the clean
  names are as stage 1 left them; and --verify reports every line of
  the resolution done.
- --no-merge, which stops the same run at the gate with the document
  complete, stops a --continue there again, and is refused outright
  once the record is past the merge -- exit 2, "past the merge", the
  state and readonly unmoved. In between, a --continue --no-gui
  passes the gate and reaches done.
- an incomplete skeleton, which stops the fresh run with a count of
  what is unanswered and stops a --continue with the same count of
  the same total; answering one line of it and no more stops the next
  --continue with what is left.
- a hand-edited choice of each kind, on a fixture with more than one
  conflicted name. keep, over a conflicted file merged by hand in the
  result while the rebase waits: at done the merge stands and
  --verify reports the name under the resolution as keep and never as
  drift. onto and from: the name is that side's object at done,
  pooled as that side pools it.
- --restart under a --take record: the answers somebody wrote
  afterwards are discarded and the document the run was started with
  comes back, answered onto, and the restart then goes on through the
  gate to done by itself.
- drift lines. A clean file is edited while the rebase waits;
  --continue --verify writes it into the document as a drift line
  with the choice keep and then stops, because the conflicts are
  still unanswered and a document written to is not a document
  answered. Answering them reaches done with the edit intact and the
  name the resolution's. The same again with that line's choice
  flipped to onto puts the name back as onto had it. run-strays.sh
  case 5 is the neighbouring case: there the conflicts are answered
  before the --continue --verify, so the gate writes the line and
  passes in one command.
- two kills. At the manifest gate, where the rebase has one document
  and not the other: --abort takes it away and --restart writes the
  skeleton again and goes on. At choice:1, inside applying2: the
  state stays applying2 with readonly off, --no-merge is refused from
  there, and --continue redoes the whole document -- which is
  idempotent -- and reaches done with nothing left for a second pass
  to do.
- the ACL strip under a choice. A non-trivial NFSv4 ACL is put on a
  clean directory of the result while the rebase waits; the gate
  writes it into the document as a drift line; the line is flipped to
  onto, and the choice must put the directory back as onto had it,
  which means stripping the ACL, since a directory that is already
  there is the one thing a choice rewrites in place. The stage's own
  second pass says whether it happened: a strip that did not would
  change the directory again there and fail the run at applying2.

A case that wants more of a fixture than it has says so and is passed
over: the hand-edited choices want a second conflicted name, the
drift lines want a name the manifest says nothing about, and the ACL
strip wants a directory of the same kind. A fixture whose every name
is conflicted has none of the last two.

One thing to watch on the trip. --restart after a kill at the
manifest gate writes the skeleton and records no path for it, because
the record has carried no zfs_rebase:resolution since the kill; a
later --abort then cannot unlink the file and the run directory stays
with it. The script takes the file away itself and prints a "note"
line saying so. That is a finding for the author, not a failure of
the harness.

## The order on a box trip

    sh tests/box/prereqs.sh
    make clean && make freebsd && make check-freebsd && make gate
    sudo sh tests/box/run-fixture.sh tests/fixtures/probe.zrt
    sudo sh tests/box/run-suite.sh          # every fixture, both forms
    sudo sh tests/box/run-replay.sh         # the pruning, in the positive
    sudo sh tests/box/run-kills.sh          # every gate, three signals
    sudo sh tests/box/run-strays.sh         # edits the tool did not make
    sudo sh tests/box/run-precond.sh        # the cells no fixture states
    sudo sh tests/box/run-resolution.sh     # the choices carried out

run-fixture.sh on probe.zrt first, because it is the shortest way to
find out that the box, the build and the pool are working at all;
then the suite, which is the long one; then the three that need the
pause hook or a doctored pool, which assume everything before them
passes; then the preconditions, which leave nothing behind; and last
the resolution, which leans on every one of them -- the record, the
gates, the pause hook, the drift lines and the apply -- and whose
failures are only worth reading when they all passed. Each of them
makes and destroys its own pool, so they can be run in any order and
one at a time, but a failure in an earlier one usually explains every
failure after it.

run-suite.sh is not the place for run-resolution.sh: that target
walks every fixture through run-fixture.sh, in both forms, and this
harness is one of the standalone ones beside run-kills.sh and
run-strays.sh, with its own pool discipline and its own fixture set.
