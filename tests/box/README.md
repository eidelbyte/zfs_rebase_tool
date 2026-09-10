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

The whole order, and why it is that order, is at the end of this
file.

Every script here builds its trees under mktemp -d in TMPDIR, or
/tmp without it, and so do the unit tests and run-fixtures.sh that
make check-freebsd runs. The fixtures in tests/fixtures/freebsd/
carry NFSv4 ACLs and system-namespace extended attributes, and
check_fixture builds user attributes and file flags, none of which a
tmpfs /tmp can hold: setting one there fails with EOPNOTSUPP and the
build fails before any pool is made (check_fixture prints the
builder's reason). The box wants /tmp on ZFS (or UFS), or TMPDIR
pointed at a directory on one, for make check-freebsd and for every
run-*.sh alike. The system attribute namespace is root's, so those
fixtures are root's. The pool image and its mount point are under
/tmp in every harness but run-probe.sh, which follows TMPDIR for
those too.

Each harness makes and destroys its own pool on a memory disk, and
the pool names are distinct -- zrtbox, zrtkill, zrtstray, zrtreplay,
zrtres, zrtprecond, zrtprobe, zrtscale, zrtlf -- so any two of them
can be run in either order. None of them takes a pool name or a
binary path from the environment: they run ./zfs_rebase from the
checkout, and they refuse to start unless it is the FreeBSD flavor
(the portable build answers every ZFS call with "not built with
ZR_FREEBSD", which is what each script probes for), the caller is
root, and the system is FreeBSD.

KEEP=1 leaves the pool, its image and the scratch directory behind
for inspection; every harness that makes a pool honours it. run-
replay.sh is the exception with a rule of its own: over its whole
fixture set it refuses KEEP=1, because the next fixture needs the
pool name back, so KEEP=1 there wants one fixture named on the
command line.

The long harnesses (run-suite, run-replay, run-kills, run-strays,
run-resolution) take --pretty, which keeps a progress display on the
terminal's bottom two rows: the fixture and heading they are on
above, and a bar the whole width less the count done of the total
and the elapsed time below, drawn through a scroll region so that
every line they print still scrolls above it and stays in the
scrollback. run-fixture.sh, run-precond.sh and run-probe.sh do not
source the helper and take no such flag. Without --pretty the run is
plain text with no terminal sequences in it, and so is a run sent to
a file. ZR_PROGRESS=1 forces the display on and ZR_PROGRESS=0 keeps
it off; a TERM of dumb or a window under six rows keeps it off too.
tests/box/progress.sh is the helper; it takes --pretty out of the
script's arguments itself, and its five functions -- prog_reset,
prog_start, prog_step, prog_note, prog_end -- are safe to call
whether or not it is drawing. The display starts by putting the
terminal back into a known state (attributes, character set, modes,
the scroll region, the tty's line discipline), since a run that
died badly, by SIGKILL or a dropped connection, leaves its region
and its two rows behind; the same reset by hand, for a terminal in
that state, is

    sh -c '. tests/box/progress.sh; prog_reset'

    sudo sh tests/box/run-suite.sh --pretty

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
objects differ; start each of them from make clean. make gate is the
ASCII and cstyle pass over the sources and the documents.

Before a box trip, tools/xcheck-freebsd.sh cross-checks every source
with clang targeting FreeBSD against a FreeBSD source tree's
headers, with the same two flag sets the Makefile uses; it catches
declaration and type errors in the FreeBSD sections without a box.

## run-fixture.sh

    sudo sh tests/box/run-fixture.sh FIXTURE.zrt

builds one fixture as real datasets on a throwaway pool and runs the
tool for real -- twice over, once in each form.

base is created as a dataset of its own and built inside it with
--build-fixture; from and onto are clones of base@base brought to
their own trees with --edit-fixture, which touches only what the two
trees disagree about. That is how run-replay.sh has always built,
and run-fixture.sh joined it: tar does not carry a socket, and rm
cannot clear a tree that holds an schg object, so the two fixtures
review-flags added could not be built any other way. The side effect
is that real objects come through with real object numbers and
unmoved ctimes, so the pruning can fire here as well; run-replay.sh
is still the harness that holds the count against a number.

The clone form first. The harness takes the snapshots -- base@base,
from@work and onto@work -- and hands the run only the two sides and
the name of the clone:

    zfs_rebase -n -o FILE --from POOL/from@work --onto POOL/onto@work
    zfs_rebase -v -o FILE --off-of POOL/from@work \
        --onto POOL/onto@work --result POOL/result
    zfs_rebase --abort FILE

Then the dataset form, after that pass has been aborted and the pool
is back to base, from and onto with their snapshots. Both sides are
given as datasets, so the tool takes its own snapshot of from and
--result names the pre-apply snapshot of onto:

    zfs_rebase -v -o FILE --from POOL/from --onto POOL/onto --result pre
    zfs_rebase --verify POOL/onto@pre
    zfs_rebase --continue POOL/onto
    zfs_rebase --abort POOL/onto

and then the whole pass again with --result POOL/onto@pre, since the
short name and the full one must name the same snapshot of the same
dataset.

What the steps are, in the order the script runs them. Step 0 checks
the derivation's refusals: a linear pair, where one side is an
ancestor of the other, and a pair sharing no origin, each exit 2,
and --base without --allow-unrelated as a usage error. Step 0a, on
probe.zrt, checks --allow-unrelated: a usage error without --base,
through with it, and a --base newer than a side refused. Step 0b, on
probe.zrt, checks that --verify is a verb and only a verb: beside a
start, beside --dry-run and beside each of --continue, --restart and
--abort it is exit 2, with nothing read, created or held.

Step 1 is the dry run: the manifest equals the fixture's expect
block from the #mode line on -- above it the header is this run's
own and the fixture's is --posix's -- and its #base line names the
snapshot the two sides were cloned from, with that snapshot's guid
beside it. On probe.zrt it also checks that a dry run creates
nothing under the --result it ignores, holds nothing and leaves no
run directory.

Step 2 is the real run, spelling --from as --off-of so the alias is
exercised: exit 0 for a clean fixture and 1 for a conflicted one,
the manifest equal again and the same base derived, and then the
holds -- none at all for a clean fixture, which released them at
done, and exactly one per input snapshot for a conflicted one, under
the tag the record carries.

Step 3 is the result and its record. The record is
zfs_rebase:manifest and zfs_rebase:tag and, at a gate,
zfs_rebase:phase, and nothing else: no zfs_rebase:quiet, since no
--quiet was given, and every one of the three read back as the
result's own local value, which is why the harness plants bogus
zfs_rebase:tag, zfs_rebase:manifest and zfs_rebase:phase values on
the pool root first -- user properties inherit down the naming tree,
and an inherited value is no record. Everything else the harness
wants to know -- the three snapshot names with their guids, the
form, the mode, #made, #take, and in the dataset form #presnap,
#readonly and #canmount -- is read out of the manifest's header,
which is where it lives now. zfs_rebase:phase reads conflicts for a
conflicted fixture and is not there at all for a clean one, which
reached done and cleared the record; the harness asks for the
property's source, not its value, because the bogus one on the pool
root is what a cleared record inherits. The clone is read-only with
its mountpoint property none, mounted at
/var/db/zfs_rebase/POOL/result/mnt while the rebase is open and
unmounted with that property still none once it has reached done --
the void, which the tool's last line says how to leave, and which
the harness leaves by setting a mountpoint so it can read the tree.
--abort on a plain dataset under the same parent is refused with
exit 2, since its properties are inherited and not its own. Then the
manifest is where -o put it, and a --posix rebase of from onto the
result declares zero actions, stage 1 being idempotent, with zero
conflicts for a clean fixture and the expect block's own count for a
conflicted one.

Step 3a is the verbs on that result. On a conflicted fixture the
rebase is open: --verify exits 0 -- a conflicted run applies its
clean actions too, so every action is done and only the conflicts
are outstanding -- having written nothing and moved no phase, and
--continue exits 1 naming the resolution it waits for and leaving
the phase and the tree as they were. On a clean fixture the rebase
reached done and took its record off, so the three motional verbs
exit 2 and --verify by name says to give the manifest instead. Given
the manifest, --verify makes the final check over again: exit 0 with
the counts, nothing written and no run directory left, first where
the harness has placed the clone and then with the clone back in the
void, where the check has to mount it privately and take that mount
and the directory away again. Step 3b, on probe.zrt, is drift: a
byte appended to /n behind the tool's back, with readonly off and on
again, must make --verify exit 3 naming "drifted 1, first /n" and
fix nothing; a plain --continue checks at the gate under no flag,
reports the same drift, writes nothing into the tree and exits per
the branch; and --verify still reports it with the result read-only.
Step 3c, on probe.zrt, is --restart: the clone destroyed and made
again from the onto snapshot the header names, with the same record,
the manifest applied from the first gate, and the run landing at the
same phase under the same tag with the same three holds and the same
tree.

Step 4 ends the rebase. Where one is still open, --abort by its
manifest exits 0, releases every hold, takes the dataset away,
leaves the -o manifest and its resolution standing because they are
the user's, removes the run directory down to /var/db/zfs_rebase,
and a second --abort exits 2 because there is no such run. Where it
reached done there is nothing to abort: the record is off, --abort
exits 2 and touches nothing, and what done left is the result alone,
unmounted and unplaced, with the run directory already gone.

Step 5 is a second real run, plain, on every clean fixture and on
probe.zrt as the conflicted one: a new tag, and on a clean fixture
the final check made at the done gate under no flag at all, its
report printed, the record cleared and the holds released. Then the
same run under -q: the check still runs, the exit status is the same
and done is the same, and the report is not printed. Step 5a, on
probe.zrt, is --abort with the manifest gone. Step 5b is two cases:
--abort over what a crash left before the record -- a run directory
with no manifest in it, which is removed with "holds no manifest",
and then the same directory with a birth manifest in it, which is
removed with "killed before it wrote one" -- and, on probe.zrt, the
pruning against a change ZFS does not report in the ctime. That last
one builds three datasets of its own, base with xattr=dir and two
clones, where from's only change is one extended attribute of one
file: the harness first asserts that the attribute set moved no
ctime, which is the ZFS fact the case exists for, and then the run
must declare that one write, report three pools unchanged and not
four, and leave from's value on the result.

Then the dataset form. D0, on probe.zrt, is exclusivity: a file held
open under onto makes the unmount fail, the run exits 2 saying onto
is in use, and it takes back everything it had made -- its record,
the pre-apply snapshot, the snapshot it took of from and its run
directory -- leaving onto mounted where it was with canmount
untouched, since the unmount is the first thing the take does and
nothing after it ran. D1 is the run: the manifest is the clone
form's manifest exactly and derives the same base; the record on
POOL/onto is the manifest's path and the tag, local against the
bogus ones on the pool root, and its header says form dataset, made
from, the readonly and the canmount it found, the pre-apply snapshot
as #presnap and #onto, and the tool's own snapshot of from as #from.
The dataset is where the rebase has it -- at the private mount with
canmount noauto and readonly off for the whole of the open rebase,
the conflicts gate included, and home at its own mountpoint with the
readonly and the canmount the fixture built once the rebase has
reached done -- with the mountpoint property untouched throughout. A
verb leaves it at the private mount every time: home is reached at
done and at --abort and nowhere between them, because a rebase
waiting for its conflicts to be answered is a half rebased tree and
a half rebased tree is not put back into service. --abort rolls onto
back, destroys both snapshots, takes every zfs_rebase: property off
it and leaves it mounted at home with canmount and readonly as they
were, holding the tree the fixture built. D2, on a clean fixture, is
a settled dataset: the rebase that reached done took its record off
and its run directory with it, so a second run over that same
dataset is taken with no flag at all and nothing is in its way, and
the before-image of the first is still there afterwards. D3 is the
whole pass again with --result spelled POOL/onto@pre.

## run-suite.sh

    sudo sh tests/box/run-suite.sh [--pretty]

walks tests/fixtures/ and tests/fixtures/freebsd/ and runs every one
of them through run-fixture.sh, in both forms; a failure prints the
log's last "==" heading, which names the form and the step it
stopped at.

## run-replay.sh

    sudo sh tests/box/run-replay.sh [FIXTURE.zrt]

is the pruning in the positive. The tool never asks ZFS what
changed: a side pool whose object number, generation, ctime to the
nanosecond, link count, type, name count and name set are all base's
-- and whose extended attributes and both ACLs compare equal -- is
taken to be what base holds and is never read. "zfs_rebase: N pools
unchanged" on stderr is the only thing that reports it, and this
harness holds that number against what the fixture predicts.

Each side is a clone of base edited in place by --edit-fixture, so
real objects come through with real object numbers and unmoved
ctimes. N is then the count computed from the fixture alone by
tools/replay-expect.py and committed in tests/box/replay-expect.txt,
one row per fixture with the from count and the onto count; the
harness asserts their sum on the dry run and on the real run alike,
and, over the whole set, that something was pruned somewhere -- a
suite that pruned nothing anywhere proves nothing and fails. make
check fails when replay-expect.txt is stale.

ctime is stored at tick resolution, so an object changed inside the
same tick as its previous change keeps the ctime it had; every side
therefore sleeps past that tick before it is edited, and a count
that is too high is a real fault and not a race. Each fixture gets
its own pool, made and destroyed here, which is why KEEP=1 over the
whole set is refused.

Two things it leans on and cannot prove: that --edit-fixture touches
nothing the two trees agree on -- if that slips, the count reads too
low and the pruning takes the blame, and the first place to look is
the ACL round trip -- and that the eleven rows for tests/fixtures/
freebsd/ are right, which is tools/replay-expect.py's model's word
alone until this harness runs on a box.

## run-kills.sh

    sudo sh tests/box/run-kills.sh [FIXTURE.zrt ...]

crosses every gate with SIGINT, SIGTERM and SIGKILL, in both forms,
on a conflicted fixture (probe.zrt) and a clean one
(h-yw-row19.zrt), and takes each of them on with --continue
afterwards. The gates it crosses are held, cloned, read, decided,
applying1 and action:2, plus conflicts and applying2 on a conflicted
fixture, and done on both; a fixture that declares fewer than two
actions is refused, since action:2 wants two. One pool per fixture,
--abort as the reset between cases, and the invariants the reset
must leave checked before the next case starts. It prints one line
per case and exits 0 only when every one of them passed.

What a stop leaves is not one rule but three, and the script is
built around them:

- SIGINT and SIGTERM before applying1 -- at held, cloned, read and
  decided -- take the whole run away. Nothing has been written to
  the result yet, so a stop there is a failure before the apply and
  is treated as one: exit 3, no record, no holds, no run directory,
  no snapshot of the tool's own, and in the dataset form onto back
  at its own mount point holding the tree the fixture built. There
  is nothing to continue and --continue says so, exit 2. (This is
  where the plan's "nothing is destroyed or released before done or
  --abort" and the code part company; the code is deliberate and
  run.c says so.)
- Every SIGKILL, and SIGINT and SIGTERM from applying1 on, leave the
  rebase standing at the gate it had reached: the record with that
  phase -- or no phase at all before the decision, when the record
  is the manifest's path and the tag alone -- the three holds, the
  manifest, which is there at every gate since the run writes its
  whole header before the record, and the result at the private
  mount in both forms, the clone with its mountpoint property none
  and the dataset with canmount noauto. The one exception is the
  held gate, which is before the take: there the dataset is still at
  home with canmount on. In the clone form readonly is back on
  wherever the tool was given the chance to put it back, and off
  after a SIGKILL inside an applying stage. In the dataset form
  readonly is what the header says it was, at the private mount and
  at home alike, since the tool changes it only while the dataset is
  off its mountpoint.
- SIGINT and SIGTERM at done are no stop at all: nothing looks at
  the flag past that gate, so the run finishes, releases the holds,
  takes the record off -- in that order, since the tag is the only
  handle on those holds -- and exits 0. A SIGKILL at that gate stops
  before either, so what it leaves is the phase of the stage that
  ran before it and the three holds, and the --continue after it
  redoes that stage and finishes.

Then --verify says what the kill left without touching it: an action
is pending until the stage that makes it has run, so a rebase
stopped before or inside applying1 exits 3 with pending actions and
one past it exits 0, and neither moves the gate, the holds, the
tree, the mount or readonly -- the report reads the result where the
kill left it, which is where a kill inside a stage shows the
difference, since the stage left readonly off. --continue then takes
the rebase to its branch's gate, making the final check itself if it
reaches done, under no flag at all; after it readonly is on, the
holds are gone at done and there at conflicts, and a --posix rebase
of the fixture's from onto the result declares zero actions, which
is stage 1 idempotence. A kill before the decision is the exception:
what the record names is the header the run was born with, so there
is nothing to apply, --continue and --restart both refuse in those
words and exit 2, and that case ends in --abort instead, which has
the whole header from the birth manifest -- it releases the three
holds, destroys the clone or rolls the dataset back to the pre-apply
snapshot and destroys that, puts readonly and canmount back as the
header kept them, destroys the snapshot the run took of from, and
takes the record and the run directory away. Nothing is put back by
hand.

While the tool is stopped at the held gate, zfs destroy of each held
input must fail and leave the snapshot standing: that is what the
holds are for.

After the gates come the settle cases, lettered (a) to (f) here and
in tests/MATRIX.md as ZX213 to ZX217 and ZX219: what the order of
the hand-back looks like from outside at the done gate and after it,
what a working directory left inside the private mount does to done
and to --abort, and what --abort makes of a run directory whose
result was destroyed under it.

The conflicted fixture reaches applying2 and done only through an
answered resolution. The tool writes the skeleton itself when it
writes the manifest, so the harness answers it: every "-" becomes
keep, which leaves the conflicted names as they stand, and the
header's count of what is unanswered goes to zero with them -- a
hand edit has to change the count too, since the parser refuses a
header that does not match its lines. An unanswered skeleton stops
at conflicts, which is what every gate before applying2 relies on.

Every run in this harness is given no --take flag, so every skeleton
it writes is unanswered and every run stops at the gate. The three
things that change that are all run-resolution.sh's: --take-onto and
--take-from, which write the skeleton answered and make it complete
from the start; --no-merge, which stops a run or a --continue at the
gate however the resolution reads; and --no-merge on a --continue
whose record is already past the merge, which is refused. The gate
is headless under no flag at all, which is what every run here
relies on; --interactive is what breaks that, forking a child on the
resolution at the gate and reading the document back when the child
exits 0, and no run in this harness gives it yet.

## run-strays.sh

    sudo sh tests/box/run-strays.sh [FIXTURE.zrt ...]

puts edits where the tool is not looking and asks what happens to
them. The same default pair, both forms, and every case ends by
taking the rebase away with the pool proved to be the fixture again.
The claim under test is that the apply pass is the only writer, and
that a difference is repaired where the result is the run's own and
reported everywhere else.

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
outside the manifest, and a --continue has nothing left to do.

A stray delete is caught by that same self-check: the name list is
over the shared name table and not over what the result holds, so a
name onto had that the result has lost is gone, and gone is restored
out of onto. The run does not stop for it and reaches its branch's
gate as if it had not happened.

Then drift after the stage, which is what a check is for and what
nothing repairs -- an edit to a file a clean action made is drifted
1 naming it; --verify fixes nothing and neither does a --continue,
because past applying1 an edit made while the conflicts are being
answered cannot be told from a stray and a gate that failed on it
would block done for good -- and, on the conflicted fixture, an edit
to a conflicted name, which is never classified and never touched,
because answering a conflict is the conflict manager's work.

Then a stray write into the live from and onto datasets while the
run is reading, at the read gate: the tool reads snapshots, so the
manifest is the expect block to the byte. In the dataset form onto
is not even where it lives just then -- it is at the private mount
for the whole of the open rebase -- so its own mount point is an
empty directory of the pool's root dataset, and a write there is
hidden the moment the dataset comes home.

Case 5 is the one place a drift line is written by a plain command:
an edit to a clean file while the rebase waits at the conflicts
gate, then --continue, which checks first under no flag, turns every
entry of the name list into a drift line with the choice keep,
writes the resolution back and goes on. The document gains one name,
the count of unanswered stays at zero because a keep is an answer,
the rebase reaches done with the edit still in the tree, and a
--verify afterwards has nothing outside the manifest to say about
that name: it is the resolution's now, and a keep is never compared.
Case 5b is the result unmounted by hand at the conflicts gate, which
is what a reboot leaves: --verify finds it mounted nowhere, mounts
it at the run directory's mnt for the read, reads it there and
leaves it mounted nowhere again, with the run directory, its mount
point and its two documents untouched and readonly as it found it,
and the --continue after it takes the result over as usual.

Case 6 is a stray between the last two gates. The conflicts are
answered, a --continue is stopped at the applying2 gate -- past the
conflicts gate's own check, with readonly already off -- and a file
a clean action made is edited there. The check at the done gate
finds it: drifted 1 naming that file, exit 3, and done reached all
the same, the record off, the run directory gone, the result settled
and the edit standing. The same question is then put to the settled
result, by --verify of the manifest done left behind, which must say
what the done gate said. That same window is where the other half of
the case is made: the resolution on disk is rewritten there, every
keep flipped to onto, and this invocation must not see it -- the
document was read when the verb opened the rebase, and the copy
applied is the copy checked.

Case 7 is the onto snapshot destroyed after done, which a settled
check has no answer for: gone by name is exit 2 naming it, and there
under the name with another snapshot's guid is exit 2 with both
numbers. Case 8 is a from side given as a dataset, so that the
snapshot read is one the tool took itself and destroyed at done: the
settled check says so, calls every action that reads from unchecked
and exits 0, and, being the clone form's, it also proves that done
leaves the clone unmounted with no mountpoint of its own, so the
check mounts it at the run directory's mnt itself and takes the
mount and the directory away again. Cases 9 and 10 are the
identifier: a second dataset given the record and a name whose last
part is the result's, so the short name answers to two rebases and
the verb prints both and refuses while the whole name still finds
one; and a second run given the first one's -o path, so that the
first run's record names a document describing another rebase, which
every verb refuses by holding the two halves against each other.

Cases 1 to 7 give the dataset form's from as a snapshot rather than
as a dataset, because a verify that cannot read from can only say
unchecked and a rebase that reaches done destroys a snapshot it took
itself; case 8 is the other spelling.

## run-precond.sh

    sudo sh tests/box/run-precond.sh

is the box cells that are not fixtures. run-fixture.sh proves what
three trees and a manifest can say; four things they cannot say are
here.

Check 1 is a nested mount. The walk refuses any entry whose st_dev
is not the walk root's, because a rebase that walked into a second
filesystem would decide over objects it can neither hold nor clone:
the harness builds POOL/from with a child dataset mounted in it and
walks the three live mount points in --posix form, and the walk of
from must exit 2 naming /inner. The same pool then shows the other
half of the fact: a child dataset is not in its parent's snapshot,
so what the snapshot holds at the child's name is the empty
directory the child is mounted over, and a dry run over the two
snapshots does not refuse and must not.

Check 1c is canmount=off in the dataset form. A rebase in place ends
by mounting the dataset where it belongs, and a dataset whose
canmount is off has no such place: it is refused at precondition
with exit 2, before its pre-apply snapshot is taken and before
anything is written, and the property is read before the mounted
question so that the refusal names the property and not the symptom.
No fixture builds such a dataset, so the check makes one.

Check 1d is an schg file the manifest removes, at securelevel 0. The
apply takes the immutable, append-only and no-unlink flags off an
object before it removes, rewrites or changes it, because ZFS
refuses the unlink, the truncate and every other attribute change on
one that carries them. Only root can set the system three and only
securelevel 0 or less lets them off again, so this is the box's cell
and not the Mac's: schg goes on an onto file the manifest removes
and one it rewrites, the rebase runs for real, and the run must exit
0 with both objects as the manifest said. Where the box is already
above securelevel 0 the check says so and skips -- that is section
2's refusal and not this one.

Section 2 is the securelevel refusal itself, and this script raises
nothing and restores nothing: it prints the procedure and leaves it
to a human. Above securelevel 0 the system flags cannot be cleared
at all, so src/run.c's securelevel_guard refuses before anything is
written, naming the first object that carries schg, sappnd or
sunlnk and the side it is on. Both sides are read: onto's, where
the apply would have to take the flag off and could not, and
from's, where the apply would write the flag on and lock the object
against whatever has to touch it next -- a --continue redoing the
action, or the applying1 self-check putting it back. The rule
itself, given a level, is check_run.c's on any machine (ZX242);
what this section is about is the sysctl and the real run.
Two trees are wanted for it, then, not one: an schg file on onto
that the manifest removes, and an schg file on from that the
manifest copies in, each of which must be refused on its own.
It cannot be checked in a reusable box
session: securelevel can be raised at any time and only a reboot
lowers it, and an schg file made under it cannot be removed again
either. The printed procedure is a throwaway VM or a jail with its
own securelevel -- build the trees first, since a builder cannot set
schg on a file it then has to write; raise the level; run the rebase
for real, not with -n, since the guard runs after the decision and
before the first write; expect exit 2 and the one-line refusal; then
destroy the VM or the jail, because the schg file cannot be unlinked
until securelevel is back to 0, which is a reboot. The cell is ZX23
in tests/MATRIX.md and is deferred there for that reason;
prereqs.sh gates the whole box on securelevel being 0 or less for
the same reason. sprints/future-features.md carries the bhyve guest
that would automate it.

Section 3 is a snapshot destroyed during a run, which this script
does not repeat: the persistent hold is what stops it, and
run-kills.sh proves it at the held gate in both forms.

Check 0 is a note rather than a verdict: whether this box's /tmp, or
TMPDIR, can hold an NFSv4 ACL and a system-namespace extended
attribute. If it cannot, every fixture under tests/fixtures/freebsd/
fails to build and the failure is the filesystem's and not the
tool's.

## run-resolution.sh

    sudo sh tests/box/run-resolution.sh [FIXTURE.zrt ...]

is where a choice of onto or from is carried out, and where
--interactive is proven on real datasets. Its default set is
tests/fixtures/probe.zrt, tests/fixtures/h-s2-two-conflicts.zrt and
tests/fixtures/freebsd/acl-conflict.zrt, each in both forms, and
every fixture it is given must declare a conflict: a rebase with
none never reaches the gate this script is about, and it says so
rather than passing vacuously. One pool per fixture, --abort as the
reset between cases, and the pool proved to be the fixture again
before the next one starts.

What it needs beyond what the other harnesses need: /tmp on ZFS or
UFS, because its default set carries an NFSv4 ACL fixture; the pause
hook, for the two gates it is the only user of; and getfacl,
lsextattr and getextattr, which is how a name in the result is held
against the side's own object. Both sides are given as snapshots in
both forms, so that from's tree is still there to compare against
after done, and onto's own tree is read out of the pre-apply
snapshot in the dataset form and out of onto@work in the clone form.
The --interactive cases want one thing more: a scratch directory a
script can be executed from, since the editor they hand -i is a
shell script the harness writes there.

The cases, in the order they run, per fixture and form:

- headless to done under --take-onto and then under --take-from.
  Each writes its skeleton answered, which makes it complete from
  the start, so the run passes its own conflicts gate and reaches
  done in one process: exit 0 and not 1. The manifest's header then
  reads #take onto or #take from; every line of the document reads
  that side and none is left to answer; every conflicted name in the
  result is that side's object -- type, mode, ownership, bytes or
  link target, ACL and both namespaces of extended attributes -- or
  is gone where that side has no such name; the names of one group
  that side pools together are one object here too; a second --posix
  rebase declares no action; and the run's own final check, which is
  standard and asks for no flag, reports every line of the
  resolution done.
- --no-merge, which stops the same run at the gate with the document
  complete, stops a --continue there again, and, once done has taken
  the record off, is refused with "not a zfs_rebase result". In
  between, a plain --continue passes the gate and reaches done.
- an incomplete skeleton, which stops the fresh run with a count of
  what is unanswered and stops a --continue with the same count of
  the same total; answering one line of it and no more stops the
  next --continue with what is left.
- a hand-edited choice of each kind, on a fixture with more than one
  conflicted name. keep, over a conflicted file merged by hand in
  the result while the rebase waits: at done the merge stands and
  the final check reports the name under the resolution as keep and
  never as drift. onto and from: the name is that side's object at
  done, pooled as that side pools it.
- --restart under a --take record: the answers somebody wrote
  afterwards are discarded and the document the run was started with
  comes back, answered onto, and the restart then goes on through
  the gate to done by itself.
- drift lines. A clean file is edited while the rebase waits; the
  --continue checks at the gate under no flag, writes it into the
  document as a drift line with the choice keep and then stops,
  because the conflicts are still unanswered and a document written
  to is not a document answered. Answering them reaches done with
  the edit intact and the name the resolution's. The same again with
  that line's choice flipped to onto puts the name back as onto had
  it. run-strays.sh case 5 is the neighbouring case: there the
  conflicts are answered before the --continue, so the gate writes
  the line and passes in one command.
- two kills. At the manifest gate, where the rebase has one document
  and not the other: --abort takes it away, and --restart writes the
  skeleton again from the recorded manifest and stops at the
  conflicts gate with a whole unanswered document. The resolution is
  beside the manifest by rule and by no record of its own, so what
  the kill leaves is the rule without the file, and --abort takes a
  file that is not there in its stride. At choice:1, inside
  applying2: the phase stays applying2 with readonly off, --no-merge
  is refused from there, and --continue redoes the whole document --
  which is idempotent -- and reaches done with nothing left for a
  second pass to do.
- the ACL strip under a choice. A non-trivial NFSv4 ACL is put on a
  clean directory of the result while the rebase waits; the gate
  writes it into the document as a drift line; the line is flipped
  to onto, and the choice must put the directory back as onto had
  it, which means stripping the ACL, since a directory that is
  already there is the one thing a choice rewrites in place. The
  stage's own second pass says whether it happened: a strip that did
  not would change the directory again there and fail the run at
  applying2. That is the hole apply-choices recorded -- macOS writes
  an ACL and never strips one -- answered on the platform whose
  za_setacl strips.
- the blocked directory line. A directory the person makes in the
  result with a file in it becomes two drift lines at the gate; the
  directory is flipped to onto, which has no such name, and the file
  under it is left at keep. The removal cannot be made -- the kept
  name holds the directory open -- and the apply's pre-scan is what
  knows it, so nothing is asked of the disk and the run does not die
  on an ENOTEMPTY. The check after the choices passes it and the
  done gate is where it is counted: exit 3, done reached all the
  same, the directory and its file still there, and the line written
  back as "-", which is what the done gate records of a choice that
  was not carried out.
- the resolution as the authority. A conflict line the manifest
  marks that a hand edit removed is put back by the next gate with
  the take mode's answer -- onto under --take-onto, from under
  --take-from, and "-" where the run was given neither -- and the
  header's counts move with it: a hand edit cannot take a conflict
  away by deleting the line that speaks for it. And a conflict line
  for a name the manifest never marked is the person's own
  instruction, carried out like a drift line with that choice, its
  group number never read.
- a run directory with no record on any dataset, which is the fifth
  and last step of the identifier's resolution and what a crash
  before the record leaves. --continue, --restart and --verify each
  exit 2, name the directory, name --abort and leave the directory
  standing; --abort takes it away.
- --interactive, twelve cases of it, with a shell script as the
  editor. The script is written into the scratch directory once and
  ZR_ED_MODE chooses what it does with the document it is handed;
  the tool passes its environment to the child, so a case sets that
  variable before it runs the tool. The script appends a line every
  time it runs, which is how a case says that no child opened at
  all, and keeps a copy of the document as it found it, which is how
  the cases about what the child sees are made. Both value forms are
  used: the bare word after -i, which on a verb's line is the
  command only because an identifier is there too (-c IDENT -i CMD),
  and the attached --interactive=CMD. In order: the child answers the
  whole document and the one process goes on to done, having opened
  one child and not two; it answers one line and exits 1, and the
  gate stands with what it saved while the next --continue is
  refused with the count; it leaves one name and exits 0, and the
  count is printed after the child and never before it; it writes a
  duplicate line, which is refused the way --continue refuses one,
  exit 2, with the file left as the child saved it; -c IDENT -i at
  the gate, where the verify's drift line is in the document before
  the fork; a --continue from applying1, where the stage finishes
  and the gate's line is printed before the child opens, which the
  order of the two lines in the one log they share says; -O -i,
  which opens on a skeleton that needs nothing (ruling 2), and -i
  -M, which opens and then holds the gate; a rebase whose decision
  declares no conflict, which reaches no such gate and opens
  nothing; -i with no command, which is the built-in picker, of
  which this build has a stub that says so and exits 2; the tool
  killed with SIGKILL while the child runs, where the child is
  orphaned and finishes and the gate stands with what it saved; and
  --restart and then -c IDENT -i, which opens on the skeleton the
  restart wrote.

A case that wants more of a fixture than it has says so and is
passed over: the hand-edited choices want a second conflicted name,
so does the --interactive case that answers one line and leaves the
rest; the drift lines and the --interactive case that opens on one
want a name the manifest says nothing about; and the ACL strip wants
a directory of the same kind. A fixture whose every name is
conflicted has neither.

The conflict-free rebase is the one case whose onto is not the
fixture's: it clones base@base into a dataset of its own, whose tree
is base's exactly, so that every action from's tree asks for applies
clean and the decision declares no conflict at all. It makes and
destroys those datasets itself rather than going through the
harness's own reset, and proves the pool is back to its three
snapshots afterwards.

## run-probe.sh

    sudo sh tests/box/run-probe.sh

is the mount probe on a pool of its own: an md-backed pool like the
other harnesses make, a scratch dataset in it, tools/probe-mount.c
run against it, and the pool gone again. The probe changes and
restores the scratch dataset's mountpoint, canmount, readonly and
sharenfs, and answers the four questions of documents-design.md
section 5 in a lettered transcript; nothing of the tool runs. make
probe-mount builds the probe off the flavor stamp, so a freebsd
build/ is left as it was, and the probe is always rebuilt, since a
stale one answers the old questions. Its answers are recorded in
sprints/sprint-5/probe-mount.txt.

## The pause hook

    ZFS_REBASE_PAUSE=<gate> zfs_rebase ...

is how the harness gets inside a run. At the gate it names the tool
raises SIGSTOP on itself and stops there; the harness waits for the
process to go into the T state, does what it came to do -- signal
it, edit the result behind its back, look at the record, try to
destroy a held snapshot -- and sends SIGCONT, and the run goes on
from exactly that point. It is a test aid: it is in no usage text,
an unknown gate name is ignored in silence, and --posix reaches no
gate at all and ignores the variable.

A gate is a point where the thing it names has just happened and the
next thing has not started:

    held        the three holds are taken (a fresh run). The
                pre-apply snapshot in the dataset form, the run
                directory, the whole header of the manifest and the
                record are already there -- the header goes down
                before the record, so every gate from this one on
                has a manifest to read. In the clone form the clone
                is there too, with the record the create wrote on
                it, and already at its private mount; in the dataset
                form the take has not happened, and the dataset is
                still at its own mount point with canmount on
    cloned      the take is done in both forms: the result is at the
                run's private mount, the dataset with canmount
                noauto and readonly off, the clone with its
                mountpoint property none, and no walk has started
    read        the walks and the pruning are done, before anything
                is decided
    manifest    the decision is written over the header the run was
                born with and zfs_rebase:phase says "decided",
                before the skeleton of the resolution is written
                beside it: the one window in which a rebase has one
                of its two documents and not the other
    decided     the manifest and the resolution are both written and
                the phase is "decided", before applying1 is written
    applying1   that phase is written and readonly is off, before
                the first action (a fresh run or --continue)
    conflicts   that phase is written, before the message that says
                what the run is waiting for. Nothing is handed back
                here: the result stays at the private mount
    applying2   that phase is written and readonly is off, before
                the choices of the resolution are carried out
                (--continue). A document answered keep throughout
                carries out to nothing, which is what the conflicted
                fixtures answer in every harness but
                run-resolution.sh
    done        the final check has been made, before readonly goes
                back on and before the settle: the result is still
                at the private mount, the record and the three holds
                are still there, and the run directory is still
                there with its documents in it. done is no phase and
                is never written; what says a rebase reached it is
                that the result carries no zfs_rebase: property at
                all
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
and nothing applied yet, action:2 stops it with the first action
made and the second not, and done stops it with the check made and
nothing given back. The verbs read the variable too, so a --continue
can be stopped at applying1, conflicts, applying2 or done; --verify
alone and --abort pass no gate and never stop.

manifest and choice:<n> are run-resolution.sh's alone. The first is
the only moment at which a rebase has a manifest and no resolution,
and what a kill there leaves is a rebase whose exits are --restart,
which writes the skeleton again from the recorded manifest, and
--abort. The second wants something to carry out, so the run that is
stopped at it is given --take-from: a conflicted name holds onto's
object when applying2 begins, so a document answered onto is already
true everywhere and the stage reaches no line.

The shape of every use of it is the same:

    ZFS_REBASE_PAUSE=applying1 zfs_rebase ... &
    pid=$!
    until [ "$(ps -o stat= -p $pid | cut -c1)" = T ]; do sleep 0.2; done
    ... whatever the test came to do ...
    kill -CONT $pid
    wait $pid

A SIGKILL needs no CONT; every other signal is delivered when the
process is continued, so the CONT comes after it.

## The identifier, across the harnesses

A verb takes IDENT and no --result, and IDENT is resolved in five
steps: an absolute path to the manifest, a dataset carrying the
record by its whole name or by the last part of it, the pre-apply
snapshot as "snap" or "pool/fs@snap", a relative path to the
manifest, and a run directory under /var/db/zfs_rebase. The
harnesses spread those between them, so that no step is only ever
exercised by one:

- run-fixture.sh gives the whole dataset name, the pre-apply
  snapshot short and in full, an absolute manifest path, a relative
  one (from another directory, with the binary named absolutely),
  and, in step 5b, a run directory with nothing behind it. It also
  checks the refusal a well-formed snapshot spelling that names no
  rebase gets: "no rebase answers to", and not the refusal a word
  that is no name at all gets.
- run-kills.sh gives the last part of the result's name alone --
  "result" in the clone form and "onto" in the dataset form -- which
  is the walk of every imported pool.
- run-strays.sh names its rebase by the -o manifest's absolute path
  and by the result's whole dataset name, and its case 10 is the
  short name answering to two rebases at once, which is refused with
  both printed.
- run-resolution.sh names by the whole dataset name throughout, and
  its case 11 puts a run directory with no record behind it to
  --continue, --restart and --verify -- each exits 2, names the
  directory and names --abort -- before --abort removes it.

## The measurements and the port

Three scripts are not part of the pass-or-fail order. They produce
numbers, or they build the port, and each is run when it is wanted.

run-scale.sh takes the scale-timing numbers sprints/sprint-5/
scale-timing.md still owes, over ZFS in the real mode:

    sudo sh tests/box/run-scale.sh [--names N] [--before COMMIT] \
        [--repeats R]

The tree comes from tools/gen-big-tree.py, which is deterministic
from its seed, and lands in three datasets: base copied in with tar,
from and onto as clones of base@base brought to their trees by rsync
in place, so that an object neither side changed keeps base's object
number and ctime and the prune has something to prune -- copying the
sides in whole would give every object a new inode and measure the
prune at zero. Then, for the binary in this checkout and, with
--before, for one built from COMMIT in a throwaway worktree, R warm
runs each of two commands: the fresh run to the conflicts gate (the
walks, the compare, applying1 and its self-check) and the --continue
that takes an answered resolution through applying2 and the final
check to done. Every run goes under /usr/bin/time -l, and the
medians are printed at the end and left with the logs in the scratch
directory, which is kept. --names defaults to 50000; the 200000
scenario of the note wants IMGSIZE=16g. It needs python3 and rsync.

run-largefile.sh answers the one hazard review-cost named, and the
syscall cell ZW35:

    sudo sh tests/box/run-largefile.sh [--before COMMIT] \
        [--dense-mb N] [--sparse-gb N]

The byte comparison skips the holes two large files share by asking
SEEK_DATA of both. ZFS answers SEEK_DATA for a file whose dnode is
dirty by waiting for a txg to sync when vfs.zfs.dmu_offset_next_sync
is 1, the default, and with EBUSY when it is 0, which the tool takes
as "read it all"; the applying1 self-check compares a result the
apply wrote a moment before, so its large files are dirty exactly
then. The script builds base with one dense file and one sparse one,
edits both on from, and times the fresh clone-form run -- which
reaches done, so the self-check and the final check are both in it
-- once under each setting of the sysctl, and once more with
--before against a binary that has no SEEK_DATA path at all. The
sysctl is put back to what it was. Then ZW35: a dry run under truss,
with the counts of the attribute calls that should be per walk
rather than per file. --dense-mb defaults to 1024 and --sparse-gb to
4; the image is IMGSIZE, 12g by default.

run-port.sh is the port-test issue:

    sudo sh tests/box/run-port.sh [COMMIT]

With COMMIT the port is built from that commit's GitHub tarball
(GH_TAGNAME), which is how the port is tested before a release is
tagged; without it, from the tag DISTVERSION names. Either way make
makesum fetches the tarball and writes distinfo first, so the box
needs the network for that one step. The port directory under
PORTSDIR is ours and is replaced by the script's copy every run, and
the steps -- each stopping the script where it fails and leaving its
output in the scratch directory -- are makesum, stage, check-plist,
stage-qa, package, install, a smoke test of the installed binary and
page, deinstall, and portlint -A where ports-mgmt/portlint is
installed. At the end the port directory is cleaned and the distinfo
it wrote is printed, since that is what goes back into
ports/sysutils/zfs_rebase/distinfo after the tag. PORTSDIR defaults
to /usr/ports and SRC_BASE to /usr/src; KEEP=1 skips the final make
clean.

## What the box still owes

- ZX23, the securelevel refusal: run-precond.sh prints the
  procedure and runs nothing, because securelevel can be raised and
  only a reboot lowers it. It wants a throwaway VM or a jail, which
  sprints/future-features.md carries as a bhyve guest for the
  harness. Both halves of it are owed now, onto's and from's.
- ZA70, the socket made with bindat(2): the FreeBSD branch of
  za_mksock has never been run. make check-freebsd reaches the
  refusal and the leaf that fits (check_apply.c, check_sock_long),
  and a fixture carrying a socket run from a real run directory --
  /var/db/zfs_rebase/<pool>/<dataset>/mnt, which is what used to be
  in the measurement -- is the rest of it.
- the scale-timing numbers of sprints/sprint-5/scale-timing.md,
  which run-scale.sh takes but which no trip has run.
- the SEEK_DATA timing over large freshly written files, which
  run-largefile.sh takes; the fallback is correct either way, so
  what is owed is the number and not a verdict.
- port-test, which run-port.sh carries out.
- the eleven tests/fixtures/freebsd/ rows of replay-expect.txt,
  which are tools/replay-expect.py's model's word until run-replay.sh
  has run over them on a box.
- the deferred cells of tests/MATRIX.md, which is where the list
  lives; nothing in these scripts holds it.

## The order on a box trip

    sh tests/box/prereqs.sh
    make clean && make freebsd && make check-freebsd && make gate
    sudo sh tests/box/run-fixture.sh tests/fixtures/probe.zrt
    sudo sh tests/box/run-suite.sh          # every fixture, both forms
    sudo sh tests/box/run-replay.sh         # the pruning, in the positive
    sudo sh tests/box/run-kills.sh          # every gate, three signals
    sudo sh tests/box/run-strays.sh         # edits the tool did not make
    sudo sh tests/box/run-precond.sh        # the cells no fixture states
    sudo sh tests/box/run-resolution.sh     # the choices, and -i

run-fixture.sh on probe.zrt first, because it is the shortest way to
find out that the box, the build and the pool are working at all;
then the suite, which is the long one; then the three that need the
pause hook or a doctored pool, which assume everything before them
passes; then the preconditions, which leave nothing behind; and last
the resolution, which leans on every one of them -- the record, the
gates, the pause hook, the drift lines and the apply -- and whose
failures are only worth reading when they all passed. Each of them
makes and destroys its own pool, so they can be run in any order and
one at a time, but a failure in an earlier one usually explains
every failure after it.

run-probe.sh is beside that order rather than in it: it runs no part
of the tool and answers the mount questions once, and it is worth
rerunning when libzfs changes under us. run-scale.sh, run-largefile.
sh and run-port.sh are the measurements and the port, run when a
number or a package is wanted and not as a gate on the trip.

run-suite.sh is not the place for run-resolution.sh: that target
walks every fixture through run-fixture.sh, in both forms, and this
harness is one of the standalone ones beside run-kills.sh and
run-strays.sh, with its own pool discipline and its own fixture set.
