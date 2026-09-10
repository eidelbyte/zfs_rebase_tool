The rule this file follows: plot each phase's problem space before
writing its tests. Enumerate the dimensions, cross them into cells,
and give every cell a disposition: planned (names the test that will
close it), covered (the test exists and names the cell), or deferred
(with the reason and the unblocking issue). A cell with no row is a
hole. Each test file's header names the cells it closes. When a cell
flips from planned to covered, the row changes with it.

Families S through V in the author's zfs-rebase-testing repository
belong to the retired kernel engines and close nothing here.

# Test matrix for zfs_rebase (family Z)

Plotted 2026-09-02, before its tests, per the rule at the top of
this document. The family covers the standalone userspace tool of
sprints/sprint-4/implementation-plan.md, which decides by the v4
notes (v4-green-pooling.md, frozen; v4-yellow-content.md;
v4-permissive-merge.md) and writes v4-manifest.md. It shares no
code and no cell with the revision-2 families (S through AS) or
with the V revision-3 engine: nothing transfers, and a Z cell is
never closed by an older test.

## Observation mechanism

No dbgmsg ring and no accessor here. The tool is a userspace
program, so a cell is closed by a return value, a built directory,
or the manifest text. Three levels, in order of preference:

- unit: tests/check_*.c, run by make check on the
  Mac, with no ZFS of any kind linked in.
- end to end on the Mac: --posix mode over a fixture's three
  directories, the emitted manifest compared with the fixture's
  expect block from the #mode line on, which is the decision
  (tests/run-fixtures.sh); the header above it is the run, and
  family ZH is where it is proved.
- box: tests/box/run-fixture.sh on FreeBSD, the only
  place snapshots, holds and clones exist.

Fixtures are tests/fixtures/*.zrt (issue fixture-format): one
three-tree spec built as plain directories on the Mac and as real
base/from/onto datasets on the box, so a cell reads the same in
both places. Decide's cells are reached instead by the battery
files the Python checkers export (issue battery-export).

## Dispositions in this family

No test in this family exists yet, so every row is planned or
deferred. A planned row names the file that will close it. A row
whose only reachable level is FreeBSD says "box" in its reason and
stays planned: the box exists, the test does not. A deferred row
names a concrete reason and the unblocking issue key from
sprints/sprint-4/zfs-rebase-sprint-4.json, or from sprint 5's
tracker where the work is sprint 5's.

## ZV -- vis codec (check_vis.c)

Dimensions: byte class {0x21-0x7e literal, backslash, hash,
space, control, 0x7f, high}; direction {encode, decode, round
trip}; buffer sizing {exact fit, one short, worst case};
malformed escape {short, non-octal, trailing, over 0377, NUL,
an unescaped byte that encode would have escaped}.

| cell | scenario | disposition |
|------|----------|-------------|
| ZV1 | every byte 0-255 round trips | planned: check_vis.c |
| ZV2 | 0x21-0x7e literal, backslash and hash out | planned: check_vis.c |
| ZV3 | backslash encodes as `\134` | planned: check_vis.c |
| ZV4 | hash encodes as `\043` | planned: check_vis.c |
| ZV5 | space as `\040` (the `a\040b` example) | planned: check_vis.c |
| ZV6 | tab, newline, 0x7f, other controls | planned: check_vis.c |
| ZV7 | high bytes: `caf\303\251` | planned: check_vis.c |
| ZV8 | one name mixing every byte class | planned: check_vis.c |
| ZV9 | worst case is 4x the input plus a NUL | planned: check_vis.c |
| ZV10 | encode into an exact-fit buffer | planned: check_vis.c |
| ZV11 | one byte short: overflow, no truncation | planned: check_vis.c |
| ZV12 | decode rejects a short escape (`\04` at end) | planned: check_vis.c |
| ZV13 | decode rejects a non-octal digit (`\09a`) | planned: check_vis.c |
| ZV14 | decode rejects a trailing bare backslash | planned: check_vis.c |
| ZV15 | decode rejects `\400`, over one byte | planned: check_vis.c |
| ZV16 | decode rejects `\000`, no NUL in a path | planned: check_vis.c |
| ZV17 | decode rejects a raw space, hash or high byte | planned: check_vis.c |
| ZV18 | the empty string and a one-byte name | planned: check_vis.c |
| ZV19 | encode(decode(s)) == s on fixture names | covered: check_roundtrip.c |

## ZN -- name table (check_name.c)

Dimensions: path validity {absolute, relative, empty, "//",
trailing slash, "." or "..", over MAXPATHLEN}; interning {new,
repeat, cross-tree, ancestors}; parent lookup {leaf, root};
pool ops {find-or-create by st_ino, add, seal, verify}; the four
add rejections; table state {initial, grown, rehashed}.

| cell | scenario | disposition |
|------|----------|-------------|
| ZN1 | interning one path twice gives one id | planned: check_name.c |
| ZN2 | one id space across the three trees | planned: check_name.c |
| ZN3 | lookup by path and by id, both ways | planned: check_name.c |
| ZN4 | the root "/" interns; its parent is none | planned: check_name.c |
| ZN5 | parent lookup walks /a/b/c up to / | planned: check_name.c |
| ZN6 | ancestors are interned on the way in | planned: check_name.c |
| ZN7 | rejected: a relative path | planned: check_name.c |
| ZN8 | rejected: empty, "//", a trailing slash | planned: check_name.c |
| ZN9 | rejected: a "." or ".." component | planned: check_name.c |
| ZN10 | rejected: longer than MAXPATHLEN | planned: check_name.c |
| ZN11 | pool find-or-create keyed by st_ino | planned: check_name.c |
| ZN12 | a second name on one ino joins the pool | planned: check_name.c |
| ZN13 | equal ino in two trees: two pools | planned: check_name.c |
| ZN14 | add rejection 1: the name is already held | planned: check_name.c |
| ZN15 | add rejection 2: the path never interned | planned: check_name.c |
| ZN16 | add rejection 3: type differs from the pool | planned: check_name.c |
| ZN17 | add rejection 4: the tree is sealed | planned: check_name.c |
| ZN18 | seal freezes adds; lookups still answer | planned: check_name.c |
| ZN19 | verify: names fewer than st_nlink | planned: check_name.c |
| ZN20 | verify: names more than st_nlink | planned: check_name.c |
| ZN21 | verify: a directory pool with two names | planned: check_name.c |
| ZN22 | the failure names the pool | planned: check_name.c |
| ZN23 | a directory's nlink is not a name count | planned: check_name.c |
| ZN24 | pools within one tree are name-disjoint | planned: check_name.c |
| ZN25 | growth past the initial bucket count | planned: check_name.c |
| ZN26 | rehash keeps every id valid and stable | planned: check_name.c |
| ZN27 | a pool with five names in four directories | planned: check_name.c |

## ZW -- walk (check_walk.c)

Dimensions: type {file, dir, symlink, chr, blk, fifo, sock};
nlink {1, 2, 3+}; placement {one dir, across dirs}; depth {1, 64};
name bytes {every class, NAME_MAX, path near MAXPATHLEN}; empty
directory; xattrs {none, one namespace, several, empty value,
binary}; ACL {absent, present}; ACL equality {both absent, one
absent, alike, unlike, reordered, one a prefix of the other};
symlink target; rdev {0, large}; completeness against st_nlink;
the generation number; the change time; the root's .zfs; entry
order {readdir's, sorted}; faults.

| cell | scenario | disposition |
|------|----------|-------------|
| ZW1 | regular file, nlink 1 | planned: check_walk.c |
| ZW2 | directory: one-name pool, descended | planned: check_walk.c |
| ZW3 | symlink: target read, never followed | planned: check_walk.c |
| ZW4 | character device: rdev recorded | planned: check_walk.c |
| ZW5 | block device: rdev recorded | planned: check_walk.c |
| ZW6 | fifo | planned: check_walk.c |
| ZW7 | socket (bound; mknod is not portable) | planned: check_walk.c |
| ZW8 | hardlink pair in one directory, nlink 2 | planned: check_walk.c |
| ZW9 | three links across three directories | planned: check_walk.c |
| ZW10 | depth 64: iterative, path from a stack | planned: check_walk.c |
| ZW11 | a name holding every byte class | planned: check_walk.c |
| ZW12 | a name at NAME_MAX, path near MAXPATHLEN | planned: check_walk.c |
| ZW13 | an empty directory | planned: check_walk.c |
| ZW14 | no xattrs: an empty set, not absent | planned: check_walk.c |
| ZW15 | several user-namespace xattrs | planned: check_walk.c |
| ZW16 | a second namespace | deferred: no namespaces on macOS; box-probe |
| ZW17 | an empty xattr value and a binary one | planned: check_walk.c |
| ZW18 | an ACL present | planned (box: freebsd/acl-kept.zrt and the other acl-*.zrt through run-suite.sh) |
| ZW19 | no ACL: absent, not an empty blob | planned: check_walk.c |
| ZW20 | mode, uid, gid, flags, size, times read | planned: check_walk.c |
| ZW21 | completeness: st_nlink over names found | planned: check_walk.c |
| ZW22 | completeness: st_nlink under names found | planned: check_walk.c |
| ZW23 | a dangling symlink walks as a symlink | planned: check_walk.c |
| ZW24 | an unreadable directory errors, not silence | planned: check_walk.c |
| ZW25 | the root is the pool named "/" | planned: check_walk.c |
| ZW26 | an empty root | planned: check_walk.c |
| ZW27 | rdev 0 and a large rdev both survive | planned: check_walk.c |
| ZW28 | the root's .zfs is skipped, a nested one is not | planned: check_walk.c |
| ZW29 | entries interned in sorted order, not readdir's | planned: check_walk.c |
| ZW30 | zr_acl_equal: absent, alike, unlike, reordered, shorter | planned: check_walk.c |
| ZW31 | za_gen is st_gen, and 0 where the platform has none | covered: check_walk.c |
| ZW32 | za_ctime is st_ctim, seconds and nanoseconds | covered: check_walk.c |
| ZW33 | the archive bit ZFS keeps for itself (UF_ARCHIVE, set on every new object and every write) is masked out of za_flags: on ZFS the walk's word lacks the bit st_flags shows, ZW20's lstat compare with the mask | planned: box (check_walk on a ZFS TMPDIR); the Mac has no such bit |
| ZW34 | the FreeBSD walk reads the extended attributes and the ACL of a file or a directory through a descriptor opened from the directory it was found in, and reads a symlink, a device, a fifo and a socket by path as before; the same attributes and the same ACLs come back either way | planned: box, the freebsd/ fixtures through run-suite.sh (sysxattr*.zrt for both namespaces, acl-*.zrt for both ACL kinds, mixed-attrs.zrt for the types with no descriptor); nothing on the Mac, whose section is unchanged |
| ZW35 | the ACL flavor is asked once, of the root, and not of every entry: a tree on ZFS reads NFSv4 for every file and a tree on UFS POSIX.1e, and a walk of n files makes one lpathconf rather than 2n | planned: box, by truss or ktrace over a walk of a fixture tree (R15 of the code review); the answer itself is ZW18's and ZC9's |

ZW31 and ZW32 are the two fields the pruning of ZC26 below reads,
and the walk pays nothing for them: it lstat'd every object
already. They are checked against a second lstat the test makes
itself, on every pool of the probe trees. On FreeBSD st_gen is the
ZPL's z_gen and st_ctim is z_ctime, which is what makes the rule
mean what zfs diff meant (sprints/sprint-5/string-audit.md); macOS
has both fields too, so the rule can be tested here; Linux has no
st_gen, and there za_gen is 0 for every object, which is one more
reason pruning belongs to the real mode alone.

ZW29 is the cell the two platforms found: readdir's order is the
filesystem's, so the same three trees were interned in one order on
APFS and another on UFS or ZFS, the name ids differed, and a why
line -- which names the first two culprits a conflict met, in id
order -- came out with its two names swapped. The walk now sorts
each directory's entries bytewise before interning any of them, so
the ids follow the manifest's own order on every filesystem and the
why line is a property of the trees, not of the disk they sit on.

ZW30 is where the two platforms part. On FreeBSD, the target, the
walk keeps the acl_t libc handed it and the comparison is binary:
the brand, then the entries in step, each one's tag, qualifier,
permission mask and -- for an NFSv4 ACL -- entry type and
inheritance flags, with two lists parting company at the step where
one cursor runs out. Order is meaning there, so the same two
entries the other way round are a different ACL, and the row is
checked on ACLs built in memory with acl_init, which needs no
filesystem and so runs on the box with every other unit test.
Everywhere else an ACL is the text acl_to_text printed and the row
is a string comparison. An ACL read off a real pool and written
back is ZW18, which the box fixtures of tests/fixtures/freebsd/
close, and ZC9 below, which they close beside it.

## ZC -- content oracle (check_yellow.c)

Dimensions: compared aspect {type, mode, uid, gid, flags, size,
bytes, xattrs, ACL, symlink target, rdev, times}; kind {file,
dir, symlink, device}; verdict {equal, differ}; handle source
{base, via the pruning, by comparison}; handle scope {one face
local group}. The pruning adds its own: the field that moved
{object number, generation number, ctime seconds, ctime
nanoseconds, link count, type, name count, a name in another
pool, an extended attribute, an ACL, a default ACL, none}; and
what the change was {bytes through a name, a name added, an
object added, an entry added to a directory, an attribute set
where ZFS moves no ctime}.

| cell | scenario | disposition |
|------|----------|-------------|
| ZC1 | equal bytes, mode differs | planned: check_yellow.c |
| ZC2 | uid only; gid only; flags only | planned: check_yellow.c |
| ZC3 | type differs at one name | planned: check_yellow.c |
| ZC4 | size differs, before a byte is read | planned: check_yellow.c |
| ZC5 | equal bytes and equal attributes | planned: check_yellow.c |
| ZC6 | an xattr value differs | planned: check_yellow.c |
| ZC7 | an xattr on one side only | planned: check_yellow.c |
| ZC8 | the same xattrs in a different order | planned: check_yellow.c |
| ZC9 | an ACL differs | deferred: a real ACL needs ZFS; box, attr-cells (ZW30 proves the comparison) |
| ZC10 | symlink targets equal, then differing | planned: check_yellow.c |
| ZC11 | device numbers differ | planned: check_yellow.c |
| ZC12 | two default directories compare equal | planned: check_yellow.c |
| ZC13 | a directory whose entries differ | planned: check_yellow.c |
| ZC14 | a directory whose mode differs | planned: check_yellow.c |
| ZC15 | times differ and nothing else | planned: check_yellow.c |
| ZC16 | bytes differ at the same length | planned: check_yellow.c |
| ZC17 | two empty files | planned: check_yellow.c |
| ZC18 | a multi-block file, identical | planned: check_yellow.c |
| ZC19 | a hole against explicit zeros | planned: check_yellow.c |
| ZC20 | unchanged from base: base's handle, no read | covered: check_yellow.c |
| ZC21 | --posix reads every byte, same verdicts | planned: run-fixtures.sh |
| ZC22 | handles are scoped to one face local group | planned: check_yellow.c |
| ZC23 | transitivity: a=b, b=c, one handle | planned: check_yellow.c |
| ZC24 | a read error is an error, never "equal" | planned: check_yellow.c |
| ZC25 | the pair entry point: the memo answers, unread | covered: check_verify.c |
| ZC26 | nothing moved: every pool of both sides prunes, no byte read | covered: check_yellow.c |
| ZC27 | bytes written through a name: that pool does not prune | covered: check_yellow.c |
| ZC28 | a second name linked on: that pool does not prune | covered: check_yellow.c |
| ZC29 | a directory that gained an entry does not prune, and is equal anyway | covered: check_yellow.c |
| ZC30 | a pool base never had is passed over, not pruned | covered: check_yellow.c |
| ZC31 | the object number alone | covered: check_yellow.c |
| ZC32 | the generation number alone | covered: check_yellow.c |
| ZC33 | the ctime seconds alone | covered: check_yellow.c |
| ZC34 | the ctime nanoseconds alone | covered: check_yellow.c |
| ZC35 | the link count alone | covered: check_yellow.c |
| ZC36 | the type alone | covered: check_yellow.c |
| ZC37 | the name count alone, and a name in another base pool | covered: check_yellow.c |
| ZC38 | pruning refused: --posix, and an unrelated base | planned: run.c has the flag; box, allow-unrelated |
| ZC39 | the pruning over real snapshots, positively | planned: box, box/run-replay.sh |
| ZC40 | the count that harness asserts, held against the tool itself | covered: tools/replay-expect.py --check |
| ZC41 | a pruned pair differs only in an extended attribute | covered: check_yellow.c |
| ZC42 | a pruned pair differs only in an ACL, and only in a default ACL | covered: check_yellow.c |
| ZC43 | a from side on xattr=dir whose only change is an attribute | planned: box, box/run-fixture.sh case 5b |
| ZC44 | two files with the same hole map: equal, and the hole is not read | covered: check_yellow.c |
| ZC45 | a hole against written zeros at chunk scale, and a difference on the far side of a shared hole: both read and both judged | covered: check_yellow.c |

ZC20 and ZC26 to ZC37 are the unchanged set, which sprint 5 took
off zfs diff and put on the walk (sprints/sprint-5/string-audit.md
section 2): a side pool is what its base pool is when the object
number, the generation number, the ctime to the nanosecond, the
link count, the type, the name count and every name agree. ZC26 to
ZC30 are that rule over one directory walked as base and then as
both sides, with the change falling between the walks -- the only
way a filesystem a test may write to offers one object twice.
ZC31 to ZC37 are the conditions one at a time, and they cannot be
reached that way at all: no call moves a ctime without moving what
caused it, and none moves an object number or a generation number
without making another object. Those rows move the field in the
walk the rule reads, which is where a wrong condition would show.
ZC39 is the positive proof on real ZFS, and box/run-replay.sh is
where it is made: each side is a clone of base edited in place with
--edit-fixture, so that real objects survive unmoved, and the run's
unchanged count must equal what the fixture leaves alone. ZC40 is that
expectation itself, which tools/replay-expect.py computes from the
fixture and --check holds against the tool on the Mac -- base built,
edited into the side, every name lstat'd before and after, and the
rule applied to what came through. run-fixture.sh builds each side
from nothing and so exercises the rule in its negative direction
only.

The oracle asks zr_acl_equal for za_acl and za_dacl, so ZC9 is that
function on two pools of a live filesystem: the comparison itself is
ZW30's, and what attr-cells adds is a non-trivial NFSv4 ACL that the
walk really read and apply really wrote back.

ZC44 and ZC45 are R27's: the byte comparison asks both files where
their next data is and skips a stretch that is a hole in both,
since the sizes are equal already and a hole reads as zeros. The
two must agree about where the hole ends -- a hole in one and
written zeros in the other are the same bytes under a different map
-- and a file smaller than one chunk is read straight through, so
every other row of this family reads exactly what it read before.
Where the platform has no SEEK_DATA, or the filesystem declines to
answer (ZFS does for a file whose dnode is dirty, unless
zfs_dmu_offset_next_sync is on, in which case it waits for a txg:
module/zfs/dmu.c, dmu_offset_next), the pair is read. ZC44's byte
count is asserted only where the filesystem under TMPDIR really
kept the hole, and loosely: what it proves is that the megabytes of
the hole were not read, not where one filesystem or another puts
the edges.

ZC41 to ZC43 are the other half of the rule, added with the review's
R1 (sprints/sprint-5/code-review-2026-09-07.md): the pruning
compares the extended attributes and the two ACLs as well, because
the ctime does not report a change to every one of them. An
extended attribute in the directory storage is set by writing a
child of a hidden directory and the file's own znode is never
touched (module/os/freebsd/zfs/zfs_vnops_os.c, zfs_setextattr_dir),
so a from side whose only change was such an attribute used to be
declared unchanged and dropped in silence; src/yellow.h lists the
sources for that and for what the ctime is still trusted with.
ZC41 sets a real attribute between the base walk and the side walks
and then puts the ctime back in the walk the rule reads, which is
the one thing the Mac cannot reproduce -- there setxattr does move
the ctime. ZC42 plants an ACL in that same walk, as ZW30 builds its
ACLs in memory, because no filesystem the unit tests may write to
hands an ACL back the same way on all three platforms. ZC43 is the
whole of it on ZFS, where nothing has to be simulated: a dataset
with xattr=dir, a clone of it with one attribute changed and
nothing else touched, and the run must emit the attrs action and
put the from value in the result.

## ZD -- decide (check_battery.c)

A tree is written as its pools juxtaposed, {AB}{C}, with content
letters where yellow is at stake; a triple is base, from, onto in
that order. Dimensions: conflict class {healed split, orphaned
add, contested home, unexpressed sharing, changed-both, disagree}
crossed with mode {strict, permissive}; clean shapes {passive
side, symmetry, fresh pool, contested placed}; permissive
adoption {kept plus adopted, adoption meets an edit, two
adoptions, cycle}. The named rows are the worked verdicts of the
three notes, kept as readable cases; ZD35 to ZD37 are the
exhaustive row, and they are what M1 means, with ZD40 saying which
of those batteries the gate actually runs. The Python checkers
already prove these properties by enumeration -- the battery is
how that proof transfers to C, not a second opinion. One thing
they cannot prove: a battery line records the set of classes that
fired and not how many groups carry them, so "every instance is
reported" is a fixture's job, which is ZD38 and ZD39.

| cell | scenario | disposition |
|------|----------|-------------|
| ZD1 | healed split {ABC} {ABCD} {A}{B}{C} | planned: check_battery.c |
| ZD2 | healed split is unchanged by permissive | planned: check_battery.c |
| ZD3 | merge heals: {AB}{C} {A}{B}{C} {ABC} | planned: check_battery.c |
| ZD4 | orphaned add {AB}{C} {ABX}{C} {C} | planned: check_battery.c |
| ZD5 | anchored add clean: onto {A}{C} | planned: check_battery.c |
| ZD6 | a fresh pool {XY} needs no anchor | planned: check_battery.c |
| ZD7 | contested home {A}{B} {AX}{B} {A}{BX} | planned: check_battery.c |
| ZD8 | contested name agreed: clean, add/add | planned: check_battery.c |
| ZD9 | contested co-members form a fresh pool | planned: check_battery.c |
| ZD10 | unexpressed sharing, the pivot, strict | planned: check_battery.c |
| ZD11 | the same triple clean in permissive | planned: check_battery.c |
| ZD12 | it never fires on a split, add or delete | planned: check_battery.c |
| ZD13 | changed-both {A}x {A}y {A}z | planned: check_battery.c |
| ZD14 | changed-both by modify/delete | planned: check_battery.c |
| ZD15 | disagree {A}x{B}y {AB}x {A}z{B}y | planned: check_battery.c |
| ZD16 | that triple permissive: {AB}z | planned: check_battery.c |
| ZD17 | disagree both modes: dedup versus edit | planned: check_battery.c |
| ZD18 | both merged, kept differently | planned: check_battery.c |
| ZD19 | delete wins over a passive side | planned: check_battery.c |
| ZD20 | an added link follows its pool | planned: check_battery.c |
| ZD21 | split, then an edit lands on both halves | planned: check_battery.c |
| ZD22 | passive: from = base gives onto | planned: check_battery.c |
| ZD23 | passive: onto = base, and from = onto | planned: check_battery.c |
| ZD24 | symmetry: swapping the sides is a no-op | planned: check_battery.c |
| ZD25 | permissive: kept plus adopted names | planned: check_battery.c |
| ZD26 | adoption meeting an edit: changed-both | planned: check_battery.c |
| ZD27 | two adoptions, two bytes: changed-both | planned: check_battery.c |
| ZD28 | a cycle of adoptions: changed-both | planned: check_battery.c |
| ZD29 | a kept name edited by the other: disagree | planned: check_battery.c |
| ZD30 | permissive pivot, distinct bytes | planned: check_battery.c |
| ZD31 | every permissive conflict is a strict one | planned: check_battery.c |
| ZD32 | one verdict per face local group | planned: check_battery.c |
| ZD33 | classes overlap: the checker's wins | planned: check_battery.c |
| ZD34 | the culprit names for the why line | planned: check_manifest.c |
| ZD35 | green battery, 3 and 4 names, both modes | covered: check_battery.c over tests/battery/green-3-*.txt and green-4-*.txt |
| ZD36 | yellow battery 2n2c 3n2c 3n3c, both modes | covered for 2n2c and 3n2c: check_battery.c over tests/battery/yellow-2-2-*.txt and yellow-3-2-*.txt; 3n3c deferred: 88 MB a file, over GitHub's 50 MB warning, so it is exported on demand (review Q6) and not checked in |
| ZD37 | green battery, 5 names | planned: check_battery.c, make battery-full |
| ZD38 | two disjoint healed splits, one base pool each, are both reported: two conflict groups of class healed-split | covered: run-fixtures.sh over two-healed-splits.zrt |
| ZD39 | one base pool fanning out into two face local groups: both healed splits are reported | covered: run-fixtures.sh over healed-split-two-groups.zrt |
| ZD40 | the batteries the gate runs, both modes: green 3 and 4 names, yellow 2n2c and 3n2c, 498,274 cases in all | covered: make battery, which globs tests/battery/*.txt |

## ZM -- manifest emit and parse (check_manifest.c)

Dimensions: action {rm, ln, cp, write, conflict}; scoping {root
only, nested, empty directory, on-the-way directory, two dots
close}; trailing slash; escaping {tree section, conflict record};
anchor {first name in manifest order, onto-created}; record parts
{legend once, class, why, three tree lines, letters}; header
{version, datasets, mode, counts}; parse rejections; round trip.

| cell | scenario | disposition |
|------|----------|-------------|
| ZM1 | rm on a leaf | planned: check_manifest.c |
| ZM2 | rm on a directory, at its close | planned: check_manifest.c |
| ZM3 | children removed before their directory | planned: check_manifest.c |
| ZM4 | ln PATH to an earlier name | planned: check_manifest.c |
| ZM5 | cp of a file | planned: check_manifest.c |
| ZM6 | cp of a directory, children after | planned: check_manifest.c |
| ZM7 | write PATH in place | planned: check_manifest.c |
| ZM8 | conflict N, and record N exists | planned: check_manifest.c |
| ZM9 | the root line "/" and its closing dots | planned: check_manifest.c |
| ZM10 | nested scoping; indent is for eyes | planned: check_manifest.c |
| ZM11 | an on-the-way directory has no action | planned: check_manifest.c |
| ZM12 | an empty directory: line, then two dots | planned: check_manifest.c |
| ZM13 | directories carry a trailing slash | planned: check_manifest.c |
| ZM14 | two dots close every open directory | planned: check_manifest.c |
| ZM15 | walk order, not strcmp: /a/b before /a-1 | planned: check_manifest.c |
| ZM16 | escaping torture in the tree section | planned: check_manifest.c |
| ZM17 | escaping torture inside a record | planned: check_manifest.c |
| ZM18 | the anchor is the pool's first name | planned: check_manifest.c |
| ZM19 | every other name of the pool is an ln | planned: check_manifest.c |
| ZM20 | an ln argument always came earlier | planned: check_manifest.c |
| ZM21 | anchor created on onto: a foreign path | planned: check_manifest.c |
| ZM22 | the legend line is emitted once | planned: check_manifest.c |
| ZM23 | records numbered by first mention | planned: check_manifest.c |
| ZM24 | each of the six class names renders | planned: check_manifest.c |
| ZM25 | the why line names the names at fault | planned: check_manifest.c |
| ZM26 | three tree lines: base, from, onto | planned: check_manifest.c |
| ZM27 | letters in order of appearance | planned: check_manifest.c |
| ZM28 | a tree with no pool of the group: empty | planned: check_manifest.c |
| ZM29 | header: version, datasets, mode | planned: check_manifest.c; the whole header is family ZH |
| ZM30 | #actions and #conflicts match | planned: check_manifest.c |
| ZM31 | a clean run: no legend, no section 2 | planned: check_manifest.c |
| ZM32 | the probe scenario, byte for byte | planned: check_manifest.c |
| ZM33 | parse rebuilds paths from the scoping | covered: check_manifest.c |
| ZM34 | parse rejects an unknown action | covered: check_manifest.c |
| ZM35 | parse rejects a bad escape | covered: check_manifest.c |
| ZM36 | parse rejects unbalanced scoping | covered: check_manifest.c |
| ZM37 | parse rejects a missing root line | covered: check_manifest.c |
| ZM38 | parse rejects an ln before its anchor | covered: check_manifest.c |
| ZM39 | parse ignores comments and blank lines | covered: check_manifest.c |
| ZM40 | emit, parse, emit is byte identical | covered: check_manifest.c |
| ZM41 | the expect block parses equal | covered: check_manifest.c |
| ZM42 | escapes, dir-rm, wide-pool, type-change .zrt | covered: check_roundtrip.c |
| ZM43 | parse rejects a first line that is not version 5 | covered: check_manifest.c, and ZH23 |
| ZM44 | parse rejects an ln naming its own path | covered: check_manifest.c |
| ZM45 | parse rejects a child under a leaf line | covered: check_manifest.c |
| ZM46 | parse rejects a leaf with no slash, no action | covered: check_manifest.c |
| ZM47 | parse rejects #actions that miscounts | covered: check_manifest.c |
| ZM48 | parse rejects a conflict mark with no record | covered: check_manifest.c |
| ZM49 | parse rejects an unknown class | covered: check_manifest.c |
| ZM50 | parse rejects records out of order | covered: check_manifest.c |
| ZM51 | a type change: rm of onto's directory, then cp | covered: check_roundtrip.c |
| ZM52 | dup: a severed half copies onto's own bytes | covered: check_apply.c, h-th-op1-edit-vs-split.zrt |

The resolution of section 8 shares this family: it is the same tree
grammar read and written by the same machinery, with a choice per
name where the manifest has an action. Dimensions: line kind
{conflict N, drift, a directory that only scopes}; choice {-, keep,
onto, from}; scoping {root only, nested, a directory carrying a
choice, on-the-way directories}; escaping; header {version,
datasets, mode, #names, #unanswered}; builders {skeleton with each
default, add_drift, add_conflict}; the name a line spells {one
plain component, ".", "..", empty, one holding a slash};
repeated names; parse rejections; round trip.

| cell | scenario | disposition |
|------|----------|-------------|
| ZM60 | the section 8 example parses, field by field | covered: check_manifest.c |
| ZM61 | parse then write is byte identical | covered: check_manifest.c |
| ZM62 | skeleton: every conflict mark, in manifest order, group and directory flag kept | covered: check_manifest.c |
| ZM63 | skeleton of a manifest with no conflicts: #names 0 | covered: check_manifest.c |
| ZM64 | skeleton with the default choice onto | covered: check_manifest.c |
| ZM65 | add_drift, then write and parse again | covered: check_manifest.c |
| ZM66 | the unanswered count is the "-" lines | covered: check_manifest.c |
| ZM67 | the run path's skeleton and the library's are the same bytes | covered: check_manifest.c |
| ZM68 | a space, a hash and a high byte in a resolution line | covered: check_manifest.c |
| ZM69 | on-the-way directories are derived by the writer | covered: check_manifest.c |
| ZM70 | parse refuses a manifest header | covered: check_manifest.c |
| ZM71 | parse refuses a version that is not 5 | covered: check_manifest.c, and ZH23 |
| ZM72 | parse refuses a raw action word | covered: check_manifest.c |
| ZM73 | parse refuses a word that is neither conflict nor drift | covered: check_manifest.c |
| ZM74 | parse refuses a choice outside the four | covered: check_manifest.c |
| ZM75 | a drift line may read "-": only a conflict line starts that way, and the done gate writes it over what it found | covered: check_manifest.c |
| ZM76 | parse refuses a conflict line with no group | covered: check_manifest.c |
| ZM77 | parse refuses #names that miscounts | covered: check_manifest.c |
| ZM78 | parse refuses #unanswered that miscounts | covered: check_manifest.c |
| ZM79 | parse refuses a name with no choice and no slash | covered: check_manifest.c |
| ZM80 | parse refuses anything after the tree section | covered: check_manifest.c |
| ZM81 | parse refuses a fifth field on a line | covered: check_manifest.c |
| ZM82 | the skeleton beside the manifest, the record naming it, --restart putting it back | planned: box, tests/box/run-fixture.sh for the unanswered skeleton, tests/box/run-resolution.sh case 5 for the answered one a --take record puts back |
| ZM83 | an unanswered skeleton stops at conflicts, an answered one goes on | planned: box, tests/box/run-fixture.sh and run-kills.sh, and run-resolution.sh cases 1 and 3, which add the count the stop names and a document answered in part |
| ZM84 | a conflicted name only from holds is a conflict mark in the tree section, with the directories on the way opened; the skeleton then has it to answer, and an rm above it is blocked | covered: run-fixtures.sh over h-s2-trap-dead-vs-edit.zrt and the five fixtures regenerated with it (tools/regen-expect.sh); box: run-replay.sh case 2 |
| ZM85 | two lines for one name in a resolution are refused at parse, as a repeated name is in a manifest: they are two instructions for one object, and the writer would fold them into a file whose #names miscounts | covered: check_manifest.c |
| ZM86 | both parsers refuse a decoded name that is ".", ".." or empty, or that holds a "/": the escaping can spell all four and the tree section means none of them | covered: check_manifest.c |
| ZM87 | both parsers validate the argument of an ln, a cp and a write the same way -- absolute, no trailing slash, no empty, "." or ".." component -- so the apply never stops part way for a reason the parse could have given | covered: check_manifest.c |
| ZM88 | add_conflict: a conflict line put back with its group, its directory flag and the take mode's answer, and the two header counts moving with it | covered: check_manifest.c |

## ZH -- the manifest header, the rebase's identity (check_manifest.c, check_roundtrip.c)

The header of v4-manifest.md section 6, version 5. Dimensions: form
{clone, dataset, posix}; line {version, result, form, base, from,
onto, presnap, readonly, canmount, made, tag, take, written, mode,
actions, conflicts}; value shape {a name, a name and a guid, a word
out of a list, a tag, a UTC time, a count}; guid {0, the largest a
uint64 holds, past it, signed, hexadecimal, absent}; failure
{missing, out of order, one too many, malformed, a dataset-form line
in another form}; document {manifest, resolution}; round trip {emit
then parse, parse then write, skeleton}.

Above #mode is the run and from #mode on is the decision. The
harnesses -- tests/run-fixtures.sh, tests/box/run-replay.sh,
tests/box/run-fixture.sh -- compare from #mode on, so nothing here
is proved by them; the run part is proved by the cells below and, on
the box, by the verbs that read a header back.

Two more dimensions came in with the birth manifest of
documents-design.md section 11.1: which of a run's two writes made
the document {birth, decision}, and how it reached the disk {the
atomic write's sibling and rename, or a stream the caller owns}.
They are ZH39 to ZH45. A birth document is a header with "#actions
0", "#conflicts 0" and the empty tree section, so every row above
holds of it as it holds of any manifest; what is new is that it
parses and round trips with no body at all, and that both writes go
through zr_doc_write.

| cell | scenario | disposition |
|------|----------|-------------|
| ZH1 | a fixture's expect block and an emitted manifest are one decision under two headers | covered: check_manifest.c |
| ZH2 | #result is the name as given | covered: check_manifest.c |
| ZH3 | #form clone | covered: check_manifest.c |
| ZH4 | #form dataset | covered: check_manifest.c |
| ZH5 | #form posix | covered: check_manifest.c |
| ZH6 | #base NAME GUID | covered: check_manifest.c |
| ZH7 | #from NAME GUID | covered: check_manifest.c |
| ZH8 | #onto NAME GUID | covered: check_manifest.c |
| ZH9 | #base - 0, a header naming no base | covered: check_manifest.c; the box half is retired: -u requires -b, ruled 2026-09-06, so no run writes such a header any more -- the parse still reads one, for a document written before the ruling or by hand |
| ZH10 | #made from, and #made - | covered: check_manifest.c |
| ZH11 | #tag zr- and hex digits, and #tag - | covered: check_manifest.c |
| ZH12 | #take onto, from, - | covered: check_manifest.c |
| ZH13 | #written an ISO 8601 UTC time, and #written - | covered: check_manifest.c |
| ZH14 | #mode, #actions and #conflicts, the decision's own three | covered: check_manifest.c |
| ZH15 | parse then write is byte identical, both forms | covered: check_manifest.c |
| ZH16 | #presnap in the dataset form | covered: check_manifest.c |
| ZH17 | #readonly on, off | covered: check_manifest.c |
| ZH18 | #canmount on, off, noauto | covered: check_manifest.c |
| ZH19 | a guid of 0 and one of 18446744073709551615 | covered: check_manifest.c |
| ZH20 | the posix form's placeholders, which make one fixture one document | covered: check_manifest.c, run-fixtures.sh |
| ZH21 | every line missing is refused, naming a line | covered: check_manifest.c |
| ZH22 | every pair of lines swapped is refused, naming a line | covered: check_manifest.c |
| ZH23 | a version that is not 5 is refused in those words | covered: check_manifest.c |
| ZH24 | a guid past a uint64, signed, hexadecimal or absent | covered: check_manifest.c |
| ZH25 | #form, #made, #take: a word outside the list | covered: check_manifest.c |
| ZH26 | #tag without its prefix, or with a digit that is not hex | covered: check_manifest.c |
| ZH27 | #written of another shape | covered: check_manifest.c |
| ZH28 | #readonly and #canmount: a word outside the list | covered: check_manifest.c |
| ZH29 | a dataset-form line in a clone or posix header | covered: check_manifest.c |
| ZH30 | a dataset-form line missing from a dataset header, and a line the header has no room for | covered: check_manifest.c |
| ZH31 | the stamp the writer makes is one the parse accepts | covered: check_manifest.c |
| ZH32 | the resolution header's three guids | covered: check_manifest.c |
| ZH33 | the skeleton copies the manifest's three names and three guids | covered: check_manifest.c, check_roundtrip.c |
| ZH34 | every field out through the emitter and back through the parse | covered: check_roundtrip.c |
| ZH35 | the skeleton's six through a write and a parse | covered: check_roundtrip.c |
| ZH36 | the emitter refuses a dataset form with no #presnap, #readonly or #canmount | covered: check_roundtrip.c |
| ZH37 | a run's own header: the result, the tag it holds under, the time it wrote, the guids of the three snapshots | planned: box, tests/box/run-fixture.sh cases 1 and 2 (the #base line and its guid) and run-replay.sh |
| ZH38 | a resolution whose name matches and whose guid does not is refused, with both guids | planned: box, tests/box/run-resolution.sh; the check is read_resolution in src/run.c, which only a real record reaches |
| ZH39 | the birth document: the whole header, #actions 0, #conflicts 0, the empty tree section, and a parse that finds no action and no record | covered: check_manifest.c |
| ZH40 | a birth document round trips -- emit, parse, write back, byte for byte -- in the clone form and in the dataset form | covered: check_manifest.c, check_roundtrip.c |
| ZH41 | the birth document carries #written like any other, and the decision written over it carries the time of that write | covered: check_manifest.c; box, tests/box/run-kills.sh, where the two writes are two gates |
| ZH42 | a dataset-form birth document carries #presnap, #readonly and #canmount, and the writer refuses one that does not | covered: check_manifest.c |
| ZH43 | the atomic write: the destination holds the bytes the emitter wrote and no .tmp is left beside it, on the first write and on the write that goes over it | covered: check_roundtrip.c |
| ZH44 | a write that cannot be made leaves the destination as it was and no .tmp behind, whether the emitter failed or the file could not be opened at all | covered: check_roundtrip.c, which fails an emitter and then makes the destination's directory unwritable; the second half is skipped with a line when the tests run as root, since root writes anyway |
| ZH45 | the sibling is in the destination's own directory, so a -o manifest on another filesystem renames without EXDEV | covered: check_roundtrip.c, which writes into a directory of its own and looks for the sibling there and nowhere else; the cross-filesystem half is box, tests/box/run-fixture.sh's -o passes, whose -o pair is under TMPDIR and whose run directory is under /var/db |

## ZA -- apply (check_apply.c)

Dimensions: action {cp, write, ln, rm, conflict}; cp type {file,
dir, symlink, chr, blk, fifo, sock}; attribute order {chown,
chmod, xattrs, ACL, times, flags last}; rm timing {leaf, at
directory close}; ln {new name, replacing a name, bad
destination}; write {in place, through every name}; the re-stat
check; the copy path {copy_file_range, read/write}; and the flags
the live object carries before the action {none, immutable,
append-only, no-unlink} crossed with what the action does to it
{removed, rewritten, attributes alone} and with the family the
flag is in {user, system}, since only the system ones are beyond
clearing above securelevel 0.

And, from ZA40 on, the other document: choice {keep, onto, from,
"-"}; the chosen side {has the name, has it not}; line kind
{conflict with a group, drift with none}; the group's pooling
{one side and one pool there, one side and two, two sides}; the
chosen type {file, directory, symlink}; the blocked directory
removal {freed by the choices, held by one of them}; a directory
line the chosen side lacks {empty in the result, held open by a
keep line under it}; the line the manifest never marked; the pass
{first, second}.

And three the reading of 2026-09-08 added: the umask the apply
inherits {0, 077}, since every object it makes is created under it
and given its mode afterwards; the socket address {the tree path
alone too long, the root and the tree path together too long, the
descriptor form FreeBSD's bindat(2) takes}; and the removal that
removed nothing {the name gone before the stat, gone between the
stat and the unlink}.

| cell | scenario | disposition |
|------|----------|-------------|
| ZA1 | cp of a regular file: bytes and type | covered: check_apply.c |
| ZA2 | cp of a directory: empty, children after | covered: check_apply.c |
| ZA3 | cp of a symlink: the target, not the file | covered: check_apply.c |
| ZA4 | cp of a character device: mknod, rdev | deferred: mknod needs root; box-probe |
| ZA5 | cp of a block device | deferred: mknod needs root; box-probe |
| ZA6 | cp of a fifo | covered: check_apply.c |
| ZA7 | cp of a socket | covered: check_apply.c, which binds an AF_UNIX socket at the path |
| ZA8 | write in place: same object, new bytes | covered: check_apply.c |
| ZA9 | write is seen through every name | covered: check_apply.c |
| ZA10 | write preserves st_ino and st_nlink | covered: check_apply.c |
| ZA11 | ln: a second name on the anchor | covered: check_apply.c |
| ZA12 | ln replacing an existing name | covered: check_apply.c |
| ZA13 | ln where the destination is a directory | covered: check_apply.c |
| ZA14 | rm of a leaf | covered: check_apply.c |
| ZA15 | rm of a directory at its close | covered: check_apply.c |
| ZA16 | rm of a directory with a child left: loud | covered: check_apply.c |
| ZA17 | conflict does nothing to the name | covered: check_apply.c |
| ZA18 | order: chown before chmod, setuid survives | deferred: an apply run by the tree's owner skips the chown; box-probe |
| ZA19 | order: xattrs and ACL before times | covered: check_apply.c, the xattr half; the ACL half is box-probe |
| ZA20 | order: times before flags | covered: check_apply.c |
| ZA21 | flags last: an immutable copied file | deferred: schg needs root; box-probe |
| ZA22 | every action re-stats its target | covered: check_apply.c, every action here passed its re-stat |
| ZA23 | a re-stat mismatch fails the apply | deferred: forcing one needs root; box-probe |
| ZA24 | copy_file_range absent: the fallback | covered: check_apply.c, which is the macOS path |
| ZA25 | openat-relative: no path leaves the root | covered: check_apply.c |
| ZA26 | an action naming a path outside the root | covered: check_apply.c |
| ZA27 | actions run in manifest order | covered: check_apply.c |
| ZA28 | xattrs applied, the user namespace | covered: check_apply.c |
| ZA29 | an ACL applied | deferred: the two ACL models differ; box-probe |
| ZA30 | the whole probe manifest applies | covered: check_apply.c |
| ZA40 | choice keep: an edited name is left exactly as it is | covered: check_apply.c |
| ZA41 | choice onto: onto's bytes and attributes back over an edit | covered: check_apply.c |
| ZA42 | choice onto where onto has no such name: the name goes | covered: check_apply.c |
| ZA43 | choice from: from's object made at the name | covered: check_apply.c |
| ZA44 | choice from where from has no such name: the name goes | covered: check_apply.c |
| ZA45 | one group, one side, one pool there: one object here | covered: check_apply.c |
| ZA46 | one group, one side, two pools there: two objects here | covered: check_apply.c |
| ZA47 | one group, two sides: two objects, neither pooled | covered: check_apply.c |
| ZA48 | a directory chosen, with names under it | covered: check_apply.c |
| ZA49 | a symlink chosen: the target, not a copy of the file | covered: check_apply.c |
| ZA50 | a drift line: no group, so it pools with nobody | covered: check_apply.c |
| ZA51 | a name already holding the side's object is left alone | covered: check_apply.c |
| ZA52 | a second pass over the same document changes nothing | covered: check_apply.c |
| ZA53 | the blocked rm goes through once the choices empty it | covered: check_apply.c |
| ZA54 | the blocked rm stays when a choice leaves a name under it | covered: check_apply.c |
| ZA55 | a choice still "-": refused, and nothing is written | covered: check_apply.c |
| ZA56 | applying2 end to end: the gate, readonly, the self-check | planned: box, box/run-resolution.sh, cases 1 and 7 (the kill at choice:1 leaves applying2 with readonly off, and the --continue after it redoes the whole document) |
| ZA57 | a choice over a name carrying an ACL | planned: box, box/run-resolution.sh, case 1 over tests/fixtures/freebsd/acl-conflict.zrt, where the name's ACL is held against the chosen side's, and case 8, which is the strip |
| ZA58 | rm of an object carrying an immutable flag: the flag comes off first | covered: check_apply.c (uchg, the Mac's stand-in for schg); box for schg, tests/fixtures/freebsd/flags-onto-rm.zrt |
| ZA59 | a write over an object carrying an immutable flag | covered: check_apply.c; box for schg, run-precond.sh 1d |
| ZA60 | an attributes-only write over an object carrying an immutable flag (a fifo, which has no bytes) | covered: check_apply.c |
| ZA61 | an append-only flag on a rewrite | covered: check_apply.c (uappnd where a user one exists, else a skip line) |
| ZA62 | the guard refuses when a system flag on onto's side cannot be cleared | deferred: securelevel cannot be raised without a reboot; ZX23 and run-precond.sh 2 hold the procedure |
| ZA63 | a socket put back by the repair | covered: check_apply.c, the repair over an onto tree holding one |
| ZA64 | a directory line whose chosen side has no such directory while a line under it says keep: the pre-scan marks it blocked and the drop loop skips it, counted as left alone, so the apply asks nothing of the disk while it acts and the run does not die on the ENOTEMPTY | covered: check_apply.c; box: run-resolution.sh case 9 |
| ZA65 | applying2 leaves every keep line exactly as it is and carries out every onto and from line, in one document | covered: check_apply.c |
| ZA66 | a conflict line for a name the manifest never marked is the person's instruction and is carried out like a drift line with that choice: its group number is not read, so it pools with nobody | covered: check_apply.c; box: run-resolution.sh case 10 |
| ZA67 | a drop line for a name the result no longer holds: the removal removed nothing, so it is counted as left alone and not as dropped | covered: check_apply.c |
| ZA68 | the apply's own umask: every object it creates is created with the mode the manifest asked for and not that mode under the umask the caller happened to leave behind | covered: check_apply.c, which runs one apply under umask 077 and reads the modes back, and reads the umask again afterwards |
| ZA69 | a socket the address cannot hold: the refusal says which of the two lengths is at fault, the path in the tree or the root the result is written at | covered: check_apply.c, over the path form, which is every platform but FreeBSD |
| ZA70 | the same socket on FreeBSD, where bindat(2) takes the parent descriptor and only the leaf goes into sun_path, so the depth of the run directory is out of the measurement and a socket that fits in the source dataset fits in the result | planned: box, box/run-fixture.sh over tests/fixtures/sock-copy.zrt, whose run directory is /var/db/zfs_rebase/<pool>/<dataset>/mnt |
| ZA71 | one group whose names are many: every line after the first is pooled onto the first, whatever the order the pre-scan reads them in | covered: check_apply.c, six names of one pool in one group |

## ZX -- the ZFS layer (zfs ops, driver, guards)

Dimensions: the unchanged set {pruned, not pruned, not attempted};
zfs ops {hold, clone, mount, prop flip, release, destroy}; the
record {every property, guids, local against inherited, the states};
the gates {held, cloned, read, manifest, decided, applying1,
conflicts, applying2, done, an action of the apply, a line of the
choices, and what a stop leaves};
the stop {SIGINT, SIGTERM, SIGKILL}; the input forms {from a snapshot or a dataset, onto a
snapshot or a dataset, --result as a clone name or as a snapshot
name, short and full}; the dataset form's own {the unmount, the
private mount, the readonly flips, the hand-back, the rollback};
guards {securelevel, private mountpoint, readonly flip, the
self-check after an apply}; the run directory {made at start, held
through every gate, gone at done, gone at --abort} crossed with
where the documents live {inside it, or where -o put them};
driver {flags, preconditions, exit status}; how a run is named to a
verb {IDENT in each of its five shapes, and no identifier at all};
whether the rebase a verb is given is in flight or settled, crossed
with what a settled check finds {the documents both there or one of
them gone, the inputs all there or one gone or one worn by another
snapshot, the result mounted at home, mounted where a hand put it or
mounted nowhere, the tree clean or drifted}, which are ZX193 to
ZX204; where an open rebase's result is mounted when a report
arrives {at the private mount, nowhere} crossed with what the report
leaves {the mount, the readonly flag, the run directory}, which are
ZX237 to ZX240; and, from ZX122 on, the
resolution as the driver carries it {the --take flag given or not,
the gate flag given or not, the document complete or not, the choice
answered by a flag or by hand, the verb that meets it}. Every row up
to ZX95 is box only; the command line's own dimensions -- spelling
{long, short, alias}, value form {separate, joined by =}, command
{fresh run, the four verbs, the three harness aids}, and refusal
{both --take flags, a gate flag on a verb, --base without
--allow-unrelated, --allow-unrelated without --base, an unknown word,
a missing value, a missing operand, a second one} -- are ZX100 to
ZX121 and ZX180 to ZX187, and are read off struct zr_args on any
machine, since the parse opens nothing. ZX122 onward are box rows
again.

| cell | scenario | disposition |
|------|----------|-------------|
| ZX1 | the unchanged count over snapshots the fixture left alone | planned: box, box/run-replay.sh |
| ZX2 | an edited object is not in that count, and the manifest still matches | planned: box, box/run-replay.sh |
| ZX3 | a side rebuilt from nothing prunes not one pool | planned: box, box/run-fixture.sh |
| ZX4 | pruning is not attempted at all in --posix | covered: run.c reaches read_trees only in the real mode |
| ZX5 | pruning off with an unrelated base | planned: box, box/run-fixture.sh step 0a |
| ZX6 | (retired with zfs diff) | -- |
| ZX7 | (retired with zfs diff) | -- |
| ZX8 | (retired with zfs diff) | -- |
| ZX9 | (retired with zfs diff) | -- |
| ZX10 | (retired with zfs diff) | -- |
| ZX11 | (retired with zfs diff) | -- |
| ZX12 | (retired with zfs diff) | -- |
| ZX13 | a side given as a dataset is snapshotted as <dataset>@zfs_rebase-<tag> and recorded made=from | planned: box, box/run-fixture.sh |
| ZX14 | one hold per input, under the record's tag | planned: box, box/run-fixture.sh |
| ZX15 | the holds outlive the process; done and --abort release them | planned: box, box/run-fixture.sh |
| ZX16 | the clone is created readonly=on with mountpoint=none, and that property is never a path | planned: box, box/run-fixture.sh step 3 |
| ZX17 | the clone is mounted at <rundir>/mnt with zfs_mount_at, which is the only place it is mounted while the rebase is open | planned: box, box/run-fixture.sh step 3 |
| ZX18 | readonly off to apply, on after | planned: box, box/run-fixture.sh |
| ZX19 | the failure path destroys it | planned: box, box/run-fixture.sh |
| ZX20 | preconditions: mounted, one pool | planned: box, box/run-fixture.sh |
| ZX21 | name semantics on all three | planned: box, box/run-fixture.sh |
| ZX22 | refusal when not run as root | planned: box, box/run-fixture.sh |
| ZX23 | securelevel refusal | deferred: a reboot; see the note |
| ZX24 | (retired with the clone's own mountpoint: a result mounted anywhere but the private mount is now taken back by an unmount rather than refused, which is ZX163) | -- |
| ZX25 | the self-check after applying1 finds every action done or blocked and no name outside the manifest | planned: box, box/run-fixture.sh |
| ZX26 | a stray write at applying1 caught and put back by the self-check | planned: box, box/run-strays.sh, which is where the pause hook can put one there mid-apply |
| ZX27 | exit 0: clean and applied | planned: box, box/run-fixture.sh |
| ZX28 | exit 1: conflicts, clone left | planned: box, box/run-fixture.sh |
| ZX29 | exit 2: precondition failure | planned: box, box/run-fixture.sh |
| ZX30 | exit 3: internal | planned: box, box/run-fixture.sh |
| ZX31 | -n: manifest only, no clone, no hold | planned: box, box/run-fixture.sh |
| ZX32 | -o FILE against stdout | planned: box, box/run-fixture.sh |
| ZX33 | -p sets the mode and the header | planned: box, box/run-fixture.sh |
| ZX34 | the record is there from the create: zfs_rebase:manifest and zfs_rebase:tag, and nothing else | planned: box, box/run-fixture.sh |
| ZX35 | the three guids of the manifest's header equal zfs get guid on the snapshots | planned: box, box/run-fixture.sh |
| ZX36 | every record property has source local | planned: box, box/run-fixture.sh |
| ZX37 | an inherited record is none: --abort exits 2, touches nothing | planned: box, box/run-fixture.sh |
| ZX38 | the phases: decided, applying1, conflicts, applying2, none at birth, and no property at all at done | planned: box, box/run-fixture.sh and box/run-kills.sh |
| ZX39 | --abort releases the holds, and runs again after a half abort | planned: box, box/run-fixture.sh |
| ZX40 | the final check is made by the invocation that reaches done, under no flag, and records nothing | planned: box, box/run-fixture.sh steps 2 and 5 |
| ZX41 | applying1 applies the clean actions before the conflicts gate | planned: box, box/run-fixture.sh |
| ZX42 | a conflicted run: state conflicts, the holds kept, the clean actions in the tree | planned: box, box/run-fixture.sh |
| ZX43 | stage 1 idempotence: a second rebase declares 0 actions and the same conflicts | planned: box, box/run-fixture.sh |
| ZX44 | a directory rm blocked by a conflicted child survives applying1, and the self-check passes over it | planned: box, box/run-fixture.sh |
| ZX45 | the hand-off names <rundir>/resolution at the conflicts gate | planned: box, box/run-fixture.sh |
| ZX46 | the run directory is /var/db/zfs_rebase/<result>, mount point and manifest under it | planned: box, box/run-fixture.sh |
| ZX47 | a manifest at that path survives a reboot, which /var/run would not | deferred: a reboot of the box; run by hand with a conflicted fixture |
| ZX48 | --continue at applying1: the manifest applied again, then done or conflicts | planned: box, box/run-fixture.sh |
| ZX49 | --continue at conflicts with no resolution: exit 1, the state unmoved, the path named | planned: box, box/run-fixture.sh |
| ZX50 | --continue at conflicts with a resolution: applying2 and then done | planned: box, box/run-kills.sh, which writes the resolution the conflict manager will: the recorded header, no actions and no conflicts |
| ZX51 | --continue on a result whose rebase reached done: exit 2, since done left no record; the settled result is untouched | planned: box, box/run-fixture.sh |
| ZX52 | --verify alone: exit 0 over a clean result and over a conflicted one | planned: box, box/run-fixture.sh |
| ZX53 | --verify over a stray edit: exit 3, the drifted action named, nothing written | planned: box, box/run-fixture.sh |
| ZX54 | a plain --continue reports that edit at the gate it arrives at and repairs nothing; --verify still reports it afterwards | planned: box, box/run-fixture.sh 3b |
| ZX55 | --restart: destroyed, cloned again, same record and tag, same gate, same tree | planned: box, box/run-fixture.sh |
| ZX56 | a recorded snapshot that exists with another guid: every verb exits 2 | deferred: needs a destroy and a re-snapshot under the name; box, kill-tests |
| ZX57 | a recorded snapshot gone: exit 2 for --continue and --restart, found by guid for --verify | deferred: needs a destroyed input, which the holds prevent until done; box, stray-tests |
| ZX58 | the report's temporary hold is there while it runs and gone after, under its own tag | deferred: needs the pause hook to look during the run; box, pause-hook |
| ZX59 | (retired with --result on a verb: a snapshot spelling is step 3 of the identifier's resolution now and the snapshot has to be there, which is ZX229 and ZX230) | -- |
| ZX60 | a result left unmounted (a reboot) is mounted again by a verb | deferred: a reboot, or zfs unmount by hand; box |
| ZX61 | a dataset carrying no record, never rebased or settled alike: every verb exits 2 and touches nothing | planned: box, box/run-fixture.sh |
| ZX62 | a fresh run's final check at the done gate, before the release, under no flag | planned: box, box/run-fixture.sh step 5 |
| ZX63 | a tool-made from snapshot goes at done and at --abort, and never at --restart | planned: box, box/run-fixture.sh |
| ZX64 | -n with a dataset side takes a snapshot, reads it, destroys it and holds nothing | planned: box, box/run-fixture.sh |
| ZX65 | the dataset form's --result: the short name and the full name are the same snapshot; a full name of another dataset exits 2 | planned: box, box/run-fixture.sh |
| ZX66 | the dataset form's record lives on onto: form=dataset, readonly recorded, every property local | planned: box, box/run-fixture.sh |
| ZX67 | the pre-apply snapshot already exists: exit 2, since the user chose the name | planned: box, box/run-fixture.sh |
| ZX68 | exclusivity: onto is unmounted from its own place and mounted at <rundir>/mnt, with the mountpoint property untouched | planned: box, box/run-fixture.sh |
| ZX69 | a file held open under onto: the unmount refuses, exit 2, nothing touched | planned: box, box/run-fixture.sh |
| ZX70 | readonly on outside the apply, off during it, and the recorded value back at the hand-back | planned: box, box/run-fixture.sh |
| ZX71 | home is reached exactly twice, at done and at --abort, and by a run that takes itself away whole; no gate between them hands the dataset back | planned: box, box/run-fixture.sh dataset pass and box/run-kills.sh |
| ZX72 | a kill in the dataset form leaves it privately mounted, and the next verb takes it from there | planned: box, box/run-kills.sh |
| ZX73 | --restart in the dataset form: rolled back to the pre-apply snapshot, applied again, same gate and tag | planned: box, box/run-fixture.sh |
| ZX74 | --abort in the dataset form: rolled back, the pre-apply snapshot destroyed, no zfs_rebase: property left local, mounted at home | planned: box, box/run-fixture.sh |
| ZX75 | --verify alone in the dataset form: the live tree walked privately, left at the private mount, nothing written | planned: box, box/run-fixture.sh dataset pass |
| ZX76 | (retired with --overwrite: a rebase that reached done leaves no record to replace, and the rule that took its place is ZX144) | -- |
| ZX77 | a base that is a snapshot of onto is read through the private mount | deferred: needs a from cloned out of onto, which the fixtures do not build; by hand on the box |
| ZX78 | a snapshot newer than the pre-apply one: --restart and --abort refuse rather than destroy it | deferred: needs a snapshot taken during a rebase; by hand on the box |
| ZX84 | the pause hook stops the tool at every gate and SIGCONT takes it on from exactly there | planned: box, box/run-kills.sh |
| ZX85 | SIGINT and SIGTERM before applying1 (held, cloned, read, decided): nothing has been applied, so the run takes itself away whole -- exit 3, no record, no hold, no run directory, neither of its documents, the dataset home -- and there is nothing left to continue | planned: box, box/run-kills.sh |
| ZX86 | SIGKILL at held, cloned or read: the record, the three holds and the birth manifest stand with no phase at all, and --continue exits 2 saying the run never reached its decision | planned: box, box/run-kills.sh; ZX205 and ZX206 are the two halves of it now |
| ZX87 | SIGKILL at decided: the phase "decided", the manifest written and recorded, and --continue applies it from the first gate | planned: box, box/run-kills.sh |
| ZX88 | a stop inside applying1, at the gate and before the second action alike: the state is applying1, readonly is back on after a caught signal and off after a SIGKILL, and the holds are all three | planned: box, box/run-kills.sh |
| ZX89 | a stop at conflicts or at applying2 leaves that state, the three holds and the manifest; a caught signal at conflicts is no stop at all, since nothing looks at the flag past that gate | planned: box, box/run-kills.sh |
| ZX90 | a stop at the done gate: SIGKILL leaves the phase of the stage that ran before it and the three holds, and --continue redoes that stage and finishes; a caught signal lets the run finish, release the holds and clear the record | planned: box, box/run-kills.sh |
| ZX91 | --verify over what a kill left: pending before and inside applying1, nothing pending past it, no drift, and the gate, the holds and the tree unmoved | planned: box, box/run-kills.sh |
| ZX92 | --continue after every kill, under no flag, reaches the branch's end: readonly as the form has it, the result settled where it reached done and at the private mount where it stopped at conflicts, the holds gone and the record cleared at done and both there at conflicts, stage 1 idempotent over the result | planned: box, box/run-kills.sh |
| ZX93 | zfs destroy of a held input, while the run is stopped, fails and leaves the snapshot standing; where nothing is cloned from it the hold is the only reason and the message says busy | planned: box, box/run-kills.sh |
| ZX94 | a stray write into the live from or onto while the run is reading changes nothing: the tool reads snapshots, so the manifest is the expect block to the byte and the verify is clean | planned: box, box/run-strays.sh |
| ZX95 | in the dataset form onto's own mount point is an empty directory while the run has the dataset, and a write there lands in the pool's root dataset and is hidden the moment the dataset comes home | planned: box, box/run-strays.sh |
| ZX100 | --from, --onto and --result parse as -f, -t and -r | covered: check_args.c |
| ZX101 | --off-of is --from and --to is --onto; neither has a letter | covered: check_args.c |
| ZX102 | -p, -v and -o parse as --permissive-merge, --verbose and --manifest | covered: check_args.c |
| ZX103 | -V, -q, -u and -b parse as --verify, --quiet, --allow-unrelated and --base | covered: check_args.c |
| ZX104 | -n parses as --dry-run, and a dry run needs no --result | covered: check_args.c |
| ZX105 | -O and -F parse as --take-onto and --take-from | covered: check_args.c |
| ZX106 | -i and -M parse as --interactive and --no-merge | covered: check_args.c |
| ZX107 | -c parses as --continue, with the gate flags and -v on it | covered: check_args.c |
| ZX108 | -R parses as --restart | covered: check_args.c |
| ZX109 | -a parses as --abort | covered: check_args.c |
| ZX110 | --verify is the verb always, in every shape it names a run: by an identifier of any shape, and with one side or both given beside it | covered: check_args.c |
| ZX111 | --name VALUE and --name=VALUE are one flag; a flag that takes none refuses one | covered: check_args.c |
| ZX112 | --posix, --build-fixture and --edit-fixture: long only, their operands, their counts, and --posix taking -p and -o alone | covered: check_args.c |
| ZX113 | --take-onto with --take-from is refused, either spelling | covered: check_args.c |
| ZX114 | a --take flag with --continue, --restart, --abort or the --verify verb is refused | covered: check_args.c |
| ZX115 | --interactive with --restart, --abort or the --verify verb is refused; accepted on a fresh run and on --continue | covered: check_args.c |
| ZX116 | --no-merge with --restart, --abort or the --verify verb is refused; accepted on a fresh run and on --continue | covered: check_args.c |
| ZX117 | --base without --allow-unrelated is refused; with it, it parses | covered: check_args.c |
| ZX118 | an unknown word, a bundled -nv, a bare - and --, an attached -fVALUE, a flag with no value, no command at all, and the retired --no-gui and -G | covered: check_args.c |
| ZX119 | --take-onto reads onto, --take-from reads from, neither reads as "-" | covered: check_args.c |
| ZX120 | a verb takes IDENT, the gate flags and -v; --result, --manifest, --quiet, -p, -n and two verbs at once are refused | covered: check_args.c |
| ZX121 | a fresh run needs --from and --onto, and --result unless -n | covered: check_args.c |
| ZX122 | a fresh run with --take-onto over a conflicted fixture writes a complete skeleton and reaches done in one process | planned: box, box/run-resolution.sh case 1, under both --take flags |
| ZX123 | --no-merge stops at the conflicts gate with a complete resolution, and the next --continue without it passes the gate | planned: box, box/run-resolution.sh case 2 |
| ZX124 | --restart under a manifest whose #take is onto or from rebuilds an answered skeleton, not an unanswered one, and the same run goes on through the gate to done, as the fresh run did | planned: box, box/run-resolution.sh case 5, which answers it another way first so that what comes back is the instruction and not yesterday's answers |
| ZX125 | the header's #take reads onto, from or "-", and is what --restart writes the skeleton from | planned: box, box/run-resolution.sh case 1 for onto and from; box/run-fixture.sh for the "-" of a run with no --take flag |
| ZX126 | --no-merge on a --continue whose record is at applying2 or done is refused, exit 2, the gate unmoved | planned: box, box/run-resolution.sh case 2 at done and case 7 at applying2 |
| ZX130 | a hand-edited choice of each kind carried out at applying2 and verified by its side: keep leaves a hand merge standing and is never compared, onto and from make the name that side's object and pool it as that side pools it | planned: box, box/run-resolution.sh case 4, on a fixture with more than one conflicted name |
| ZX131 | an incomplete resolution stops, and says by how much: the fresh run's count, the same count of the same total from a --continue, and what is left after one line of it is answered | planned: box, box/run-resolution.sh case 3 |
| ZX132 | a plain --continue at the conflicts gate with the conflicts still unanswered: the drift line is written into the document and the gate stops all the same, and the line survives the answering to be the name's word at done | planned: box, box/run-resolution.sh case 6 |
| ZX133 | a drift line whose choice is flipped from keep to onto puts the name back as onto had it | planned: box, box/run-resolution.sh case 6 |
| ZX134 | the manifest gate: a SIGKILL between the manifest and the skeleton leaves a manifest, no resolution, the phase "decided" and three holds, and the rebase's exits are --abort and --restart | planned: box, box/run-resolution.sh case 7; the gate is run.c's zr_pause("manifest") |
| ZX135 | the choice:<n> gate: a SIGKILL inside applying2's choices leaves applying2 with readonly off, and --continue redoes the whole document and reaches done with nothing for a second pass to do | planned: box, box/run-resolution.sh case 7; the gate is apply.c's zr_apply_choice_pause_at |
| ZX136 | an ACL put on a clean directory at the conflicts gate and chosen onto: the choice strips it back, and the stage's own second pass finds the directory unchanged | planned: box, box/run-resolution.sh case 8, which is the box's answer to the macOS-only strip hole apply-choices recorded beside ZA52 |
| ZX137 | -q parses as --quiet on a fresh run, both spellings, and neither spelling given reads as 0 | covered: check_args.c |
| ZX138 | -q on --continue, --restart, --abort, the --verify verb and --dry-run is refused, both spellings | covered: check_args.c |
| ZX139 | -w and --overwrite are unknown options wherever they are given | covered: check_args.c |
| ZX140 | the record at birth is zfs_rebase:manifest and zfs_rebase:tag and nothing else, both of them the result's own local values, in both forms | planned: box, box/run-fixture.sh step 3 and its dataset pass |
| ZX141 | zfs_rebase:phase reads decided, applying1, conflicts and applying2 at those gates and is absent before the decision | planned: box, box/run-kills.sh, which stops at every gate, and box/run-fixture.sh at conflicts |
| ZX142 | zfs_rebase:quiet is absent from a record no --quiet asked for, and present as a local value where -q was given | planned: box, box/run-fixture.sh step 3 for the absence and step 5's -q run for the presence |
| ZX143 | at done no zfs_rebase: property is left on the result, in either form and by either path (the run's own done and a --continue's) | planned: box, box/run-fixture.sh, box/run-kills.sh and box/run-resolution.sh, each of which now asserts the empty list where it asserted state=done |
| ZX144 | a settled result is free: a second run over that dataset is taken with no flag, and --continue, --restart, --abort and --verify on it exit 2 as on any dataset with no record | planned: box, box/run-fixture.sh D2 and step 3a; done-cleanup took away the run directory that used to stand in D2's way |
| ZX145 | --abort with the manifest gone: the holds are released by walking the pool for the tag, the private mount is undone, the record is cleared, the one snapshot the run took for itself -- held under the tag and named with it -- is destroyed, the result is not destroyed and nothing is rolled back, and the two commands are printed | planned: box, box/run-fixture.sh 5a, which unlinks the -o manifest by hand: with the header written before the record there is no gate that leaves a record without its file, so a file somebody took away is the only way into this path, and the kills harness no longer reaches it; nothing on the Mac reaches zr_zfs_release_tag |
| ZX158 | the mountpoint property of the result is never a path in the clone form: none at the create, none at every gate, none at done and none after --abort without a manifest | planned: box, box/run-fixture.sh steps 3, 3c and 5a |
| ZX159 | the clone stays at the private mount through every gate, the conflicts gate included, since it has no home to be handed to | planned: box, box/run-fixture.sh step 3b and box/run-kills.sh |
| ZX160 | the per-stage readonly flips of the clone form reach the kernel at the private mount, with no remount attempted at a stale path | planned: box; the probe of 2026-09-06 answered it directly (tools/probe-mount.c 2a-2d, sprints/sprint-5/probe-mount.txt), and every harness reads readonly per gate after it |
| ZX161 | done hands the clone to the void: unmounted, readonly on, mountpoint none, and one line on stderr saying how to place it | planned: box, box/run-fixture.sh steps 3, 3d and 5, which place it with zfs set mountpoint to read its tree |
| ZX162 | a reboot leaves the clone unmounted, and the next verb's take_over mounts it privately again; --restart's fresh clone takes the same path | planned: box, box/run-fixture.sh 3c (the fresh clone); the reboot itself is by hand on the box |
| ZX163 | a result mounted anywhere but the private mount is unmounted and taken back, and one somebody is using refuses with the take's own message | deferred: needs a result mounted by hand and held open; by hand on the box, as ZX69 is for the take |
| ZX164 | --abort in the clone form undoes the private mount and destroys the clone, whatever the mountpoint property says | planned: box, box/run-fixture.sh step 4 |
| ZX165 | the take sets canmount=noauto while the dataset is unmounted, and the header's #canmount is what it was before the take and not what the take wrote | planned: box, box/run-fixture.sh dataset pass |
| ZX166 | at every gate and after every kill the dataset is at the private mount with canmount noauto and readonly off, never at home | planned: box, box/run-kills.sh, box/run-strays.sh and box/run-resolution.sh |
| ZX167 | the hand-back: both properties put back while the dataset is unmounted, then mounted home only where it is not there already | planned: box, box/run-fixture.sh dataset pass and its --abort |
| ZX168 | a caught signal at a gate leaves the dataset privately mounted, as a SIGKILL does, and --continue finds it there | planned: box, box/run-kills.sh, every gate crossed with INT and TERM |
| ZX169 | a run that gives up before it has written anything puts the dataset home with both properties back, as an --abort would | planned: box, box/run-kills.sh, the torn cases |
| ZX170 | a dataset form onto whose canmount is off is refused at precondition, exit 2, with nothing taken and nothing written; the property is read before the mounted question so the message names it | planned: box, box/run-precond.sh 1c |
| ZX171 | --abort with the manifest gone tells the forms apart by the mountpoint property alone: a path is mounted home, none is left unmounted, and neither readonly nor canmount is guessed at | planned: box, box/run-fixture.sh 5a for the "none" half, which is a clone; the "path" half is deferred, since the dataset-form case that used to reach it -- a kill before the manifest in box/run-kills.sh -- now finds the birth manifest and takes the whole abort, and reaching abort_lost in that form means unlinking a dataset-form run's -o manifest by hand |
| ZX172 | an edit made at the conflicts gate in the dataset form is made at the private mount, where only root can reach it | planned: box, box/run-strays.sh and box/run-resolution.sh, whose gate cases now edit there |
| ZX173 | done removes the run directory: the two documents the run wrote there unlinked, then mnt, the directory and every empty parent up to /var/db/zfs_rebase by rmdir and never recursively, in both forms | planned: box, box/run-fixture.sh (clone_placed, settled_clone and the dataset pass), box/run-replay.sh step 3 |
| ZX174 | a --continue that reaches done removes it exactly as the fresh run's own done does, and a verb that stops short of done leaves every bit of it, since that is where the next verb reads the rebase from | planned: box, box/run-fixture.sh 3d through clone_placed, box/run-kills.sh reset_pool and box/run-strays.sh and box/run-resolution.sh end_case, each of which now asserts the directory rather than removing it |
| ZX175 | a -o manifest and the resolution beside it survive done: only documents inside the run directory are unlinked, and the decision is that directory's path as a prefix of the recorded one | planned: box, box/run-fixture.sh step 3 and the dataset pass, both of which run with -o; the no--o half is ZX173, where the directory could not go if the two were still in it |
| ZX176 | a -o manifest and its resolution survive --abort too, which still removes the run directory and the two documents the run wrote into it | planned: box, box/run-fixture.sh step 4 and the dataset pass's --abort |
| ZX177 | a second rebase of the same result after done finds no run directory: the dataset form goes through, and the clone form is refused before make_rundir because the clone is still there | planned: box, box/run-fixture.sh D2 for the dataset form; the clone form's refusal is ZX16's result_ok and is asserted in step 3 |
| ZX178 | make_rundir still refuses an EEXIST leaf: the directory is the lock for as long as a run is open, and the refusal names the other thing such a leaf can be -- a crash before the record, which --abort removes | deferred for the refusal itself: with done removing the directory, the only way to a leftover leaf under an open rebase is to make one by hand; the crash-leftover half is ZX210 and ZX211 |
| ZX179 | the two documents a run wrote into its own directory are unlinked at done, in the no--o form, before the directory goes | planned: box, box/run-kills.sh, whose caught signal at the done gate lets the run finish, and which then asserts no manifest, no resolution and no run directory |
| ZX180 | each of the four verbs names its run by IDENT, and by no identifier at all, which is refused | covered: check_args.c; box, box/run-fixture.sh 3a for --continue and --verify by a manifest's path and step 4 for --abort by one |
| ZX181 | the operand stands anywhere among the flags, and a second one is refused: one identifier names one rebase | covered: check_args.c |
| ZX182 | the cross-check, both halves: the record must name the file given, and the manifest the record names must name this result back, and either mismatch is exit 2 with both sides in the message | covered: read_manifest and check_given in src/run.c; box, box/run-fixture.sh 3a, which gives a copy of this rebase's own manifest and a manifest whose header names another run, and box/run-strays.sh case 9 for the swapped -o path |
| ZX183 | -o on each of the four verbs is refused, both spellings, beside a name and beside a manifest's path alike; on a start and on a dry run it is the flag it is | covered: check_args.c |
| ZX184 | --allow-unrelated without --base is refused, both spellings, on a start and on a dry run | covered: check_args.c; box, box/run-fixture.sh 0a, which also asserts that the refused run wrote no manifest |
| ZX185 | a start takes no operand, and neither does a dry run, wherever it is written | covered: check_args.c |
| ZX186 | --from and --onto are accepted on a verb and name no rebase: each is checked against the header by name and by guid, and a side that is not this rebase's is exit 2 with nothing touched | covered: check_args.c for the parse; box, box/run-fixture.sh 3a for the check and its refusal |
| ZX187 | the rule that reads a run's dataset out of a header: #result in the clone form, the dataset of #onto in the dataset form with --result spelled short and spelled full, and a refusal for a --posix document and for a dry run's "-" | covered: check_args.c, over a parsed header zr_run_dataset takes and nothing else |
| ZX188 | --verify beside --continue, --restart or --abort is refused, exit 2: two verbs are two commands, and the check the flag used to ask for is standard | covered: check_args.c; box, box/run-fixture.sh 0b |
| ZX189 | what starts a rebase is refused beside any verb, exit 2 -- --result, which names what a start makes, and for --verify also --dry-run, the other spelling of a start -- while --from and --onto stand on the verb and are checked against the header | covered: check_args.c; box, box/run-fixture.sh 0b, which also asserts the refused command read nothing, created nothing and held nothing |
| ZX190 | every --continue that arrives at the conflicts gate checks first, under no flag: the drift it finds becomes lines of the resolution with the choice keep, printed and never fixed; a --continue that arrives there from applying1 in the same invocation is not checked twice, since the self-check it has just made is that check | planned: box, box/run-strays.sh case 5 and box/run-resolution.sh case 6, which are the drift-line cases with the flag dropped; the writing itself is src/run.c's add_drift, which wants a real resolution beside a real record |
| ZX191 | the final check at the done gate in every invocation that reaches it, fresh run or --continue: drift is reported and the exit status is 3, and done is written all the same -- the record cleared, the result settled and the run directory gone -- while a check that cannot be made at all leaves the gate unpassed and the rebase standing | planned: box, box/run-strays.sh case 6 (a stray made at the applying2 gate, exit 3 with done reached) and box/run-fixture.sh step 5 (the clean pass, exit 0 with the report printed); the unmakeable check is ZX30's, and the verdict rule itself -- an action pending or drifted, a line of the resolution pending or drifted, or any entry of the name list -- is found_drift in src/run.c, static and read by the done gate and the --verify verb alike, deferred on the Mac because nothing here builds a report against a real result |
| ZX192 | -q silences the final check's report and nothing else: the check is still made, the exit status still stands, done is still done, and the report of the conflicts gate and of the --verify verb are printed as always | planned: box, box/run-fixture.sh step 5, which runs the same rebase twice, once plain and once under -q |
| ZX193 | --verify given the name of a settled result: no dataset carries a record under that name and no run directory is left, so the resolution says it is not a zfs_rebase result and to give the manifest instead, exit 2, and touches nothing | planned: box, box/run-fixture.sh 3a and its dataset pass, box/run-strays.sh verify_open and case 5 |
| ZX194 | --verify MANIFEST on a settled result: the record is what tells a rebase in flight from one that is over, and a settled result takes its inputs from the header alone -- exit 0 with the counts on a tree that is what the manifest says | planned: box, box/run-fixture.sh 3a (the clone form) and its dataset pass (the dataset form), box/run-strays.sh cases 5, 7 and 8 |
| ZX195 | a settled result that is mounted -- a dataset at home, or a clone a hand has placed -- is read where it stands, and the check makes no run directory at all | planned: box, box/run-fixture.sh 3a and its dataset pass, box/run-strays.sh cases 5, 6 and 7 |
| ZX196 | a settled clone in the void is mounted at the run directory's mnt with the run's own zfs_mount_at, read there, then unmounted, with mnt, the directory and every empty parent taken away again -- whatever the check found | planned: box, box/run-fixture.sh 3a, which puts the placed clone back in the void for it, and box/run-strays.sh case 8, where done left it there |
| ZX197 | the settled check changes no property of the result: readonly, mountpoint and canmount as it found them, and no zfs_rebase: property written; a clone at done is read-only and the private mount is read-only with it, which is all a check wants | planned: box, box/run-fixture.sh 3a and its dataset pass, box/run-strays.sh cases 6, 7 and 8 |
| ZX198 | a settled result whose tree drifted: the check says what the done gate said of it, the same drifted name and exit 3, and mends nothing | planned: box, box/run-strays.sh case 6, whose --continue reached done with a stray and exit 3 |
| ZX199 | the inputs of a settled check are looked up by the header's name with the guid checked, and no pool is searched: a name that is gone stops the check with exit 2 naming it | planned: box, box/run-strays.sh case 7, the dataset form's onto snapshot destroyed after done |
| ZX200 | and a name another snapshot wears now stops it with exit 2 and both guids, since a name is what a snapshot is called and the guid is what it is | planned: box, box/run-strays.sh case 7, which takes the snapshot again under the old name |
| ZX201 | a from side #made says the tool took itself is gone at done on purpose: the check covers the names and onto's bytes, says so in the wording a post-done report has always used, calls every action that reads from unchecked and exits 0 | planned: box, box/run-strays.sh case 8; the classification behind it is ZY101 |
| ZX202 | a settled check wants both documents: the resolution gone from beside the manifest is exit 2 saying a settled result is checked against both, and one that cannot be parsed is exit 2 with the parse's own reason | deferred: by hand on the box -- every harness keeps the -o pair its run wrote, and removing or corrupting the resolution after done is a step no case has a use for otherwise |
| ZX203 | the result the header names destroyed after done: exit 2 naming the dataset, before any document is read against it | deferred: by hand on the box; every harness's settled result is the one it goes on to take away, so destroying it early would end the pass |
| ZX204 | the dataset form's rule for the run a manifest names, made against a real pool in both branches: #result is the pre-apply snapshot and the dataset carrying the rebase is #onto's, in flight and settled alike | planned: box, box/run-fixture.sh dataset pass, both branches of its verbs section |
| ZX146 | zfs_rebase:base and :base_guid are written by nothing: the branch point is #base in the header | covered: the grep over src, tests and the docs; box, the empty property list after a run |
| ZX147 | zfs_rebase:from and :from_guid likewise: #from | covered: the same |
| ZX148 | zfs_rebase:onto and :onto_guid likewise: #onto | covered: the same |
| ZX149 | zfs_rebase:made likewise: #made | covered: the same |
| ZX150 | zfs_rebase:mode likewise: #mode | covered: the same |
| ZX151 | zfs_rebase:form likewise: #form | covered: the same |
| ZX152 | zfs_rebase:take likewise: #take, which --restart reads for the skeleton | covered: the same; box, box/run-resolution.sh case 5 |
| ZX153 | zfs_rebase:readonly likewise: #readonly, which the hand-back reads | covered: the same; box, box/run-fixture.sh dataset pass |
| ZX154 | zfs_rebase:resolution is written by nothing: the resolution is beside the manifest by rule, FILE.resolution beside a -o FILE and <rundir>/resolution beside the run directory's | covered: resolution_of in src/run.c, read by every verb; box, both -o and no -o passes -- box/run-kills.sh is the no--o one, since done-cleanup gave the three harnesses that read a document after done a -o pair, which is the only kind that survives done |
| ZX155 | zfs_rebase:verify is written by nothing and there is no request at all, recorded or on the command line: the invocation that reaches done makes the check | covered: the grep over src, tests and the two documents; box, box/run-fixture.sh step 5 and box/run-kills.sh |
| ZX156 | zfs_rebase:state is written by nothing: zfs_rebase:phase replaces it and never takes the value done | covered: the grep; box, every harness |
| ZX157 | the pre-apply snapshot a --restart or an --abort rolls back to is the header's #presnap and no property | covered: src/run.c; box, box/run-fixture.sh dataset pass |
| ZX205 | the birth manifest is there before the record: at held, cloned and read the manifest the record names exists and declares no action and no conflict, and no resolution is beside it yet | planned: box, box/run-kills.sh, whose three pre-decision gates now assert the file and its two counts |
| ZX206 | a record with no phase is a rebase born and not decided: --continue and --restart refuse it with that reason, exit 2, before either takes the result over, and name --abort | planned: box, box/run-kills.sh at held, cloned and read; the refusal is undecided() in src/run.c, made in resume_open before resume_trees |
| ZX207 | zfs_rebase:phase reads "decided" from the moment the decision manifest is renamed into place: at the manifest gate, which is before the skeleton, and at the decided gate | planned: box, box/run-kills.sh's decided gate and box/run-resolution.sh case 7, the window between the two documents |
| ZX208 | --abort at held, cloned or read in the dataset form, with the header in hand: the three holds released, the dataset rolled back to #presnap and that snapshot destroyed, readonly and canmount put back as the header kept them, the dataset mounted home, the record off, the tool's own from snapshot destroyed and the run directory gone | planned: box, box/run-kills.sh; it is what replaces the by-hand "zfs set canmount=on" that the manifest-less abort needed before the birth manifest |
| ZX209 | --abort at held, cloned or read in the clone form: the clone destroyed whatever gate it was at, the holds released and the run directory gone | planned: box, box/run-kills.sh |
| ZX210 | --abort on a run directory whose result carries no record and which holds no manifest -- the window before the birth manifest -- removes mnt and the directory, says what it found and exits 0 | planned: box, box/run-fixture.sh 5b, which makes that directory by hand: no kill can be timed inside a mkdir |
| ZX211 | --abort on a run directory holding a birth manifest with no record on the result -- the window between the two writes -- destroys the from snapshot #made names, leaves the dataset form's #presnap and says so, unlinks both documents and removes the directory | planned: box, box/run-fixture.sh 5b, which builds the document from the header of a manifest the tool wrote; a kill in the live window is deferred, since no pause gate sits between the birth manifest and the record and adding one is a change to the gate vocabulary |
| ZX212 | every document a run writes lands whole: the manifest at birth and at the decision, the skeleton, the drift lines and --restart's rewrite all go through the sibling and the rename, and no .tmp is left in the run directory or beside a -o file at any gate | planned: box, box/run-kills.sh and box/run-resolution.sh, whose gate assertions now hold over the directory's contents; the primitive itself is ZH43 to ZH45 on the Mac |
| ZX213 | the done gate's order in the dataset form: the final check made and reported first, then the walks closed, the dataset unmounted from the private mount, readonly and canmount put back, the dataset mounted at its own mountpoint and asked whether it is there, and only after all of that the holds released, the record taken off, the tool's own from snapshot destroyed and the run directory removed | planned: box, box/run-kills.sh's settle case (c), which asserts the observable end of the order -- canmount and readonly as the fixture built them, the dataset at home, then no record, no hold and no directory -- and box/run-fixture.sh's dataset pass, which crosses it every trip |
| ZX214 | done with the private mount busy: the unmount refuses, nothing after it happens -- the record and the three holds stay, the run directory stays, the result stays at the private mount -- the message names the mount path and the exit is 3; and a --continue once the mount is free settles it and reaches done | planned: box, box/run-kills.sh's settle case (a), which holds a shell's working directory inside <rundir>/mnt at the done gate, in both forms |
| ZX215 | --abort with the private mount busy: the same stop in the same order -- the dataset rolled back and still at the private mount, or the clone still there, with the record, the holds and the directory kept -- exit non-zero, and a second --abort once the mount is free takes the rebase away | planned: box, box/run-kills.sh's settle case (b) |
| ZX216 | --abort with the result dataset gone and a decision manifest in its run directory: the tag released on the three snapshots the header names, the #made snapshot destroyed, the two documents unlinked and the directory removed, exit 0 -- the state a --restart whose second clone failed leaves, and the one where the holds used to be stranded (R2) | planned: box, box/run-kills.sh's settle case (d), which destroys the clone by hand at the conflicts gate |
| ZX217 | --abort on a run directory holding a manifest that will not parse: the file says a rebase was here and nothing can be read out of it, so nothing is released, nothing is removed, the parse's reason is printed and the exit is 2 | planned: box, box/run-kills.sh's settle case (e), which truncates the manifest before the abort. A directory holding no manifest at all is the other thing and stays ZX210: exit 0 and removed, since that is the window before the birth write and there is nothing in it to read |
| ZX218 | --abort on the birth-manifest window in the dataset form destroys #presnap: a run directory with a birth manifest and no record can only be the window between the two writes, because a done takes its directory away, so the pre-apply snapshot there is the run's and not a user's before-image | deferred: by hand on the box, as ZX211 is -- run-fixture.sh 5b builds the leftover from a clone-form header, which has no #presnap, and building a dataset-form one wants a dataset-form run's manifest kept back the same way; the branch is one line beside the branch 5b crosses |
| ZX219 | every name a verb is given is held against zfs_name_valid before it builds a path: --abort "../../../../tmp/x" is exit 2 naming the name, and the directory that spells reachable is still there | planned: box, box/run-kills.sh's settle case (f), which makes that directory as a decoy and gives --abort four names ZFS would not take; the check is rundir_of in src/run.c, through which every WORKDIR path is built, and the portable build cannot answer it at all (the stub says "not built with ZR_FREEBSD") |
| ZX220 | WORKDIR is resolved with realpath once and the resolved prefix is what a recorded manifest path is compared against, so a symlink in /var/db would not make a run's own documents read as a -o pair | deferred: /var/db is a real directory on FreeBSD and making it a symlink is a box change no case can undo cleanly; the fix is workdir() in src/run.c and the every-trip proof is ZX173's no--o pass, whose documents are unlinked at done |
| ZX221 | each of the four verbs takes IDENT as its one operand, in both spellings of the verb, and the parse hands it on whole | covered: check_args.c |
| ZX222 | --result beside every verb is refused, both spellings, alone and beside a side: it names the result of a run you are starting, and the refusal says what a verb takes instead | covered: check_args.c |
| ZX223 | step 1, an absolute path: the manifest at it, resolved with realpath, parsed, and its header read for the dataset that carries the record | covered: check_args.c, over documents the test writes; box, box/run-strays.sh, box/run-fixture.sh 3a |
| ZX224 | step 1 is an end and not a fall-through: an absolute path with no file at it, or with a file that is no manifest, is refused there and never looked for as a name | covered: check_args.c; box, box/run-strays.sh |
| ZX225 | a document that names no rebase -- a --posix one, or a dry run's "-" -- is refused with the header's own reason, whichever path step found it | covered: check_args.c |
| ZX226 | step 2, the clone form in full: the result's whole dataset name, asked of that dataset alone and no pool walked | planned: box, box/run-fixture.sh 3a and step 4, box/run-resolution.sh |
| ZX227 | step 2, the short name: a dataset carrying the record whose name ends in "/IDENT", found by the walk of every imported pool | planned: box, box/run-kills.sh, which names every verb's rebase by the last part of its name |
| ZX228 | step 3, the dataset form: the pre-apply snapshot named by its short name alone, whose dataset carries the record | planned: box, box/run-fixture.sh dataset pass |
| ZX229 | step 3 spelled out: DS@SNAP, the dataset part matched as in step 2, and a snapshot that is not there is no match rather than a rebase | planned: box, box/run-fixture.sh dataset pass and 3a |
| ZX230 | step 4, a relative path to a manifest of the user's, which is looked at only after the pools have answered | planned: box, box/run-strays.sh |
| ZX231 | step 5, a run directory of that name: --abort reaches abort_leftover through it, and --continue, --restart and --verify say there is no rebase to move and name --abort | planned: box, box/run-resolution.sh case 10 and box/run-fixture.sh 5b |
| ZX232 | two rebases answering to one identifier at one step: refused with every match printed and never chosen between, whatever the verb | planned: box, box/run-strays.sh case 10, which gives a second dataset the record and the same short name |
| ZX233 | an identifier that answers to nothing at all: refused naming the three places looked in; where a dataset of that name is there and carries no record, that is said instead and the manifest is asked for | planned: box, box/run-fixture.sh 3a and box/run-resolution.sh |
| ZX234 | the identifier and every dataset name a header hands back are held against zfs_name_valid before either becomes a path | covered: ident_result_ok in src/run.c, the one gate every step leaves through; box, the name steps of every harness |
| ZX235 | the manifest an identifier named is parsed once: the resolution's parse is what the verb reads, and --abort's too | covered: src/run.c, read_manifest and zr_abort adopting it (R12); box, every verb given a path |
| ZX236 | no message of the tool tells a person to write --result on a verb: every printed command names IDENT | covered: the grep over src, README.md and zfs_rebase.8 |
| ZX237 | --verify on an open rebase reads the result where it is mounted and takes nothing over: in both forms and at every gate the mount is where the check found it, no zfs op moves it, and the phase, the holds and the two documents are untouched | planned: box, box/run-strays.sh's verify_open cases, which assert the mount before and after, and box/run-kills.sh, which verifies after every kill |
| ZX238 | --verify never sets readonly, in either form: a clone a kill left writable inside a stage is still writable after the report, and one outside a stage is still read-only, since the report flips nothing either way | planned: box, box/run-strays.sh case 3, which reads readonly before the report and asserts it after, and its new unmounted case |
| ZX239 | --verify on an open rebase whose result a hand unmounted -- the mounted-nowhere branch, which is also what a reboot leaves: the check mounts it at the run directory's mnt with the run's own zfs_mount_at, reads it there and unmounts it again, leaving it mounted nowhere and the run directory, its mnt and its documents exactly as they were | planned: box, box/run-strays.sh's unmounted case |
| ZX240 | and the --continue after that report takes the result over as usual, mounting it privately again and reaching its gate: a report that mounted for itself leaves nothing for the next verb to work around | planned: box, box/run-strays.sh's unmounted case |
| ZX241 | a dry run refuses -O, -F, -M and -i, both spellings: it writes a manifest and stops, so there is no skeleton for a --take flag to answer and no conflicts gate for --interactive or --no-merge to hold -- nothing for the flag to act on and no record to latch it in, which is the reasoning -q meets there (ZX138) | covered: check_args.c |
| ZX242 | the flag guard reads from's side as well: an object of from's carrying schg, sappnd or sunlnk that the decision would write into the result is refused above securelevel 0 too, since za_attrs would stamp the flag on and a --continue or the self-check's put-back would then meet EPERM with the tree part written | covered: check_run.c, which asks the question with the level as an argument over a synthetic decision and two synthetic walks; the sysctl that reads the real level is the box's, ZX23 |

ZX96 to ZX99 are no cells: the numbering skips to a round one so
that the command line's own rows read as the block they are. ZX100
to ZX121, with ZX137 to ZX139 and ZX188 and ZX189 after them, are
the only rows of this family that are read off the parse, because
src/args.c decides nothing and opens nothing: it fills struct
zr_args, and tests/check_args.c reads it. ZX122 to ZX126 are
what the parse cannot show -- the header's #take line, a run passing
its own conflicts gate, and the two ways --no-merge holds it. ZX127 to ZX129 are no cells either: the
numbering skips again so that the resolution's own rows read as the
block they are. ZX130 to ZX136 are what the resolution does to a real
tree -- the choices by hand, the count a stop names, the drift lines,
the two new pause gates and the ACL strip -- and every one of them,
with ZX122 to ZX126, is box/run-resolution.sh's.

ZX158 to ZX172 are private-mount's: the clone form on the private
mount and the dataset form's canmount. Every one of them is box only,
because every one of them is a mount or a property on a real dataset,
and the four questions they rest on were answered on the box before
the code was written -- tools/probe-mount.c, run through
tests/box/run-probe.sh, transcript in
sprints/sprint-5/probe-mount.txt. ZX24 is retired by the same work:
a result mounted somewhere else is no longer refused but taken back,
which is ZX163, and that row is deferred with ZX69 because both want
a mount held open by hand. Nothing here is reachable on the Mac: the
portable build answers every one of these calls with "not built with
ZR_FREEBSD".

ZX173 to ZX179 are done-cleanup's: the run directory is born at the
start of a run and gone at done, so what a finished rebase leaves is
the result and, where -o asked for it, a manifest and a resolution
of the user's. Every one of them is box only. The decision the whole
family turns on -- is this manifest inside the run directory? -- is
one strncmp against WORKDIR/<result>/ (in_rundir in src/run.c), the
same prefix compare resolution_of has always made under ZX154, and
it stays static in run.c rather than being exported for a Mac unit
test: the paths it compares only mean anything against a real
WORKDIR and a real record, and both -o and no--o passes cross it on
the box every trip.

That split moved with this issue. A harness that reads a document
after the rebase has reached done can only read a -o one from now
on, so box/run-strays.sh and box/run-resolution.sh give -o where
they gave none -- their post-done reads are the manifest against the
expect block, the header's #take, and the resolution's drift line --
and box/run-kills.sh keeps the no--o placement whole: it asserts
<rundir>/manifest and <rundir>/resolution at every gate from
"decided" on and their absence at done, which is ZX179.

ZX188 to ZX192 are verify-schedule's: the checks on a schedule no
flag changes, and --verify a verb and only a verb. The two refusals
are read off the parse and are check_args.c's; the three that are
about a real gate are box only, because a check is a walk of three
trees against a document and nothing on the Mac has any of them.
The drift-line writing at the conflicts gate is src/run.c's, and so
is the verdict rule the done gate and the --verify verb share: both
are static in run.c, where the fresh-run path and the verb path
already meet, and both are crossed by every conflicted pass on the
box.

ZX180 to ZX187 are cli-shape's: a run named to a verb by its
manifest as well as by its result, the two cross-checked, --from and
--onto taken on a verb and checked against the header, -o at the
start alone, and --allow-unrelated needing --base. ZX213 to ZX228
took the naming itself over: what stands here is the shape of the
command line. All but two are read off the parse. The two that are
not are ZX182, the cross-check, which wants a record on a real
dataset and a manifest the record names, and the box half of ZX180
and ZX186; and
ZX187, the rule that turns a header into the dataset that carries
its record, is on the Mac because it is a function over a parsed
header -- zr_run_dataset, exported for exactly that -- and opens no
file and no pool. The empty-tree base went with ZX184: the box case
that read the two sides against it is gone from
box/run-fixture.sh 0a, and the half of ZH9 that said a run writes
"#base - 0" is retired with it, the parse's half standing, since a
document written before the ruling still has to be readable.

ZX137 to ZX157 are record-slim's: the command line's two changes
(--quiet added, --overwrite gone), the four properties the record
still has, and one row per property it no longer has, which is where
the fact it carried lives now. The removed ones are covered on the
Mac only as an absence -- grep over src, tests, README.md and
zfs_rebase.8 finds no writer and no reader -- and on the box as the
empty property list every harness asserts at done. ZX145 is the one
that needs a real pool and cannot be reached any other way: releasing
by tag walks the pool with libzfs, which exists only in the FreeBSD
build.

ZX1 to ZX5 replace the diff parser's cells, which went out with the
text: ZX1 to ZX12 used to be the "zfs diff -F -H" lines, their
escaping, the derived sets and the captured tests/data/probe.diff,
and the whole of that -- src/diff.c, src/diff.h, tests/check_diff.c,
the file and zr_zfs_diff -- was removed in sprint 5 for the walk's
own rule (ZC26 and the note under it). ZX6 to ZX12 are left standing
empty so that ZX13 and everything after it still mean what they
meant. ZX1 and ZX2 are the positive proof that the pruning does
anything at all on real ZFS, and box/run-replay.sh makes it: sides
edited in place so that objects come through unmoved, the unchanged
count held against tests/box/replay-expect.txt, and the manifest still
equal to the fixture's expect block. ZX3 is what run-fixture.sh says
beside it, which is only that a tree built from nothing prunes
nothing.

ZX213 to ZX220 are the settle's, and they are the shutdown order
made observable. The order itself -- the walks, the result, the
holds, the record, the snapshots, the directory -- cannot be watched
from outside a running process, so what the rows assert is its two
ends and its one refusal: that the dataset is at home with its
properties before the record is gone (ZX213), and that a mount
somebody is standing in stops everything after it and leaves a
rebase the next verb can finish (ZX214, ZX215). ZX216 is R2, where
the holds outlived the only thing that named them; ZX217 is the one
shape --abort refuses. The last two are guards rather than
behaviour: ZX219 is reachable on the box in one command, against a
decoy directory the case makes for it, and ZX220 is not reachable at
all without rebuilding /var/db, so it is carried by the code and by
the no--o pass that would break the moment it were wrong.

ZX221 to ZX236 are the identifier's: --result becomes a start flag
only and the four verbs take IDENT, which is resolved in five steps
(documents-design.md, section 11.4). The parse's own rows -- the
operand, the refusal of --result beside a verb -- are check_args.c's,
and so are the two path steps, because zr_ident_manifest opens a file
and no pool: the test writes a manifest of its own and holds the
resolution against it. Everything from step 2 on wants a real record
on a real dataset and is box only, the two-match refusal included,
which the harness builds by giving a second dataset in the same pool
the record and a name ending in the same word. ZX59 retires into
ZX229: a snapshot spelling used to be read as its dataset whatever it
named, and now it is step 3 and the snapshot has to exist. ZX234 and
the settle's own ZX219 meet at zfs_name_valid: the resolution holds
the identifier and the name a header hands back against it, and
rundir_of holds whatever is left against it again where a path is
built.

ZX205 to ZX212 are the birth manifest's: the header written before
the record, the phase "decided" that says a decision is in place,
what the verbs do with a record that has neither, and the two
windows before the record that --abort now clears. All but the two
leftover rows are reached by the pause hook at gates the harnesses
already stop at, so they cost no new machinery; the leftover rows
are made by hand in run-fixture.sh because the window they name is
between two system calls. The atomic write under all of it is the
Mac's, ZH43 to ZH45: it is a file in a directory and wants no pool.

Note on ZX23: securelevel cannot be raised without a reboot of the
box, so the refusal path stays deferred. Unblocking work is a
box-probe run started at securelevel 1, which is also the only way
to prove the schg/sappnd/sunlnk name listing. The guard reads the
three system flags off both sides against the decision (the user
flags are always clearable by root, so they never enter it) --
onto's, where the flag stops the apply from clearing it, and
from's, where the flag is written by the apply and stops whatever
has to touch that object next -- and what it refuses is a run the
apply could not finish; at securelevel 0 or less the apply clears
them itself, which is ZA58 to ZA61 and run-precond.sh 1d. The
question the guard asks is ZX242 and is check_run.c's, since it
takes the level as an argument; only the sysctl that answers "what
level is this" is the box's.

ZX237 to ZX241 are review-verify-verb's: --verify reads in place
(documents-design.md, section 11.6). Where the verb used to call
take_over, which unmounts a result from wherever it is, mounts it at
the private mount and sets readonly on in the clone form, it now
reads the result where it is mounted and mounts it only where it is
mounted nowhere -- the settled clone's branch, widened to serve an
open rebase a reboot or a hand left unmounted, and giving back the
mount without the run directory, which is the run's. So the sentence
"writes nothing to the tree and moves nothing that is where it
should be" is true of the verb again: the readonly flip after a kill
inside a stage was the one real property write left, and it is gone.
The four are box rows because a mount and a property on a real
dataset are all they are about; ZX241 is the parse's and is
check_args.c's, with ZX138.

## ZF -- the fixture format (check_fixture.c, run-fixtures.sh)

Not an engine phase but the input every other family's end to end
level reads: one .zrt file, parsed, built as three directory trees
and built again as pools in memory, where the two builders must
agree. Dimensions: syntax element {tree lines, the five types, the
escapes, mode, uid, gid, flags, xattr, acl, the platform line,
expect}; what it acts on {file, dir, symlink, sock, a link line,
the pool two names share}; builder {directories, pools, one
directory edited into another}; the edit's decision {removed,
created, relinked, rewritten, attrs, untouched}; rejection {every
rule the format names}; platform {portable, box only}; and what the
runner holds a run against {the manifest's body from #mode on, the
exit status}.

Rows for the attributes were added with the attributes themselves
(issue fixture-attrs). A row that only FreeBSD can reach names the
box-only fixture that closes it (issue attr-cells wrote them) and
stays planned until the box has run it, which is the disposition
the top of this file gives a row whose only reachable level is
FreeBSD.

| cell | scenario | disposition |
|------|----------|-------------|
| ZF1 | the types, tokens, links and escapes parse | covered: check_fixture.c (file, link, dir, symlink; sock is ZF61) |
| ZF2 | mode, uid and gid on a file, a dir and a symlink | covered: check_fixture.c |
| ZF3 | a build into a directory that is not empty is refused | covered: check_fixture.c |
| ZF4 | to_tree: pools, synthetic inos, nlink, handles | covered: check_fixture.c |
| ZF5 | the expect block, and a fixture without one | covered: check_fixture.c |
| ZF6 | the rejections the format named before attributes | covered: check_fixture.c |
| ZF7 | flags=NAMES parses to the number lchflags takes | covered: check_fixture.c |
| ZF8 | a flag name chflags(1) does not know is rejected | covered: check_fixture.c |
| ZF9 | flags on a file, walked back as za_flags | covered: check_fixture.c |
| ZF10 | the immutable flag on a directory (uchg where a user one exists, schg on ZFS as root): set after its children exist; skipped with a line where neither can be set | covered: check_fixture.c (immutable half box-root) |
| ZF11 | xattr=NAME:VALUE parses; the walk reads that name | covered: check_fixture.c |
| ZF12 | an empty value, and a value needing an escape | covered: check_fixture.c |
| ZF13 | two xattrs out of bytewise name order: rejected | covered: check_fixture.c |
| ZF14 | one xattr name twice on a line: rejected | covered: check_fixture.c |
| ZF15 | an xattr with no colon, no namespace, or a bad escape | covered: check_fixture.c |
| ZF16 | a system-namespace xattr with no platform line: rejected | covered: check_fixture.c |
| ZF17 | a system-namespace xattr set and walked back | planned (box: freebsd/sysxattr-add.zrt, sysxattr-conflict.zrt, mixed-attrs.zrt) |
| ZF18 | acl=TEXT parses and reaches the pool's handle | covered: check_fixture.c |
| ZF19 | acl= with no platform line: rejected | covered: check_fixture.c |
| ZF20 | an ACL set with acl_from_text and walked back | planned (box: freebsd/acl-kept.zrt and the other acl-*.zrt) |
| ZF21 | the platform line: parsed, and the build off it refused | covered: check_fixture.c |
| ZF22 | platform after a tree line, twice, or unknown: rejected | covered: check_fixture.c |
| ZF23 | attributes on a link line land on the shared pool | covered: check_fixture.c |
| ZF24 | the handle folds mode, uid, gid, flags, xattrs and ACL | covered: check_fixture.c |
| ZF25 | a symlink's mode= is passed by; defaults resolve equal | covered: check_fixture.c |
| ZF26 | flags cleared before a built tree is removed | covered: check_fixture.c, run-fixtures.sh |
| ZF27 | xattr-edit.zrt: a user xattr edited on from -> write | covered: run-fixtures.sh |
| ZF28 | xattr-add.zrt: an xattr added -> write, and a new file -> cp | covered: run-fixtures.sh |
| ZF29 | xattr-conflict.zrt: one xattr, both sides -> changed-both | covered: run-fixtures.sh |
| ZF30 | flags-copy.zrt: a from-only file with nodump -> cp | covered: run-fixtures.sh |
| ZF31 | acl-nfsv4.zrt: an NFSv4 ACL added on from -> write | planned (box: run-suite.sh) |
| ZF32 | sysxattr.zrt: a system xattr edited on from -> write | planned (box: run-suite.sh) |
| ZF33 | --edit-fixture: base edited into a side walks equal to a fresh build of it | covered: check_fixture.c |
| ZF34 | the names the fixture leaves alone keep their inode and their ctime; the rest keep neither | covered: check_fixture.c |
| ZF35 | the six counts are one decision per name and add up to the union of the two trees' names | covered: check_fixture.c |
| ZF36 | a file's bytes rewritten through the name it had, the inode kept | covered: check_fixture.c |
| ZF37 | a name linked onto a pool that stays; every name of it relinked, the object kept | covered: check_fixture.c |
| ZF38 | a pool broken in two: the survivor keeps the object, the other name is made afresh | covered: check_fixture.c |
| ZF39 | a rename, which the format has no word for: one removed, one created | covered: check_fixture.c |
| ZF40 | a directory that loses its child and stays keeps its inode | covered: check_fixture.c |
| ZF41 | a directory that becomes a file: emptied, removed, made again | covered: check_fixture.c |
| ZF42 | a symlink retargeted: made again, since no filesystem retargets one | covered: check_fixture.c |
| ZF43 | one extended attribute changed and nothing else: attrs, the inode kept | covered: check_fixture.c |
| ZF44 | flags: one changed alone (nodump to hidden); the immutable flag off a directory to empty it and on again; an immutable file edited (the last two skipped with a line where no immutable flag can be set) | covered: check_fixture.c |
| ZF45 | the same edit twice: everything untouched, nothing else, nothing moved | covered: check_fixture.c |
| ZF46 | a platform fixture edited off its platform is refused in words | covered: check_fixture.c |
| ZF47 | escapes.zrt edited: every byte the encoding has a rule for, and a name leaving one pool for another | covered: check_fixture.c |
| ZF48 | wide-pool.zrt edited: five names on one object gaining a sixth, the write on the object they share | covered: check_fixture.c |
| ZF49 | dir-rm.zrt edited: a directory three deep removed, children before parents | covered: check_fixture.c |
| ZF50 | an ACL changed alone: attrs, the inode kept | planned: box, check_fixture.c on FreeBSD (attr-cells builds these fixtures, it does not edit them) |
| ZF51 | a system-namespace xattr changed alone | planned: box, check_fixture.c on FreeBSD (attr-cells builds these fixtures, it does not edit them) |
| ZF52 | acl-kept.zrt: one ACL in all three trees and the bytes edited on from -> write, the ACL still there after it | planned (box: run-suite.sh) |
| ZF53 | acl-copy.zrt: a from-only file carrying an ACL -> cp, the copy carrying it | planned (box: run-suite.sh) |
| ZF54 | acl-conflict.zrt: from and onto gave one file different ACLs, bytes unchanged -> changed-both | planned (box: run-suite.sh) |
| ZF55 | acl-same-both.zrt: both sides added the identical ACL -> clean, and no action at all | planned (box: run-suite.sh) |
| ZF56 | acl-strip.zrt: from took a non-trivial ACL off again -> write, and apply's strip path | planned (box: run-suite.sh) |
| ZF57 | sysxattr-add.zrt: a system-namespace xattr added on from -> write | planned (box: run-suite.sh) |
| ZF58 | sysxattr-conflict.zrt: a system-namespace xattr set differently on both sides -> changed-both | planned (box: run-suite.sh) |
| ZF59 | mixed-attrs.zrt: a user xattr, a system xattr, an ACL and a flag on one object, through a write and through a cp | planned (box: run-suite.sh) |
| ZF60 | flags-conflict.zrt: hidden on from and nodump on onto -> changed-both (uchg is not a ZFS flag: EOPNOTSUPP) | planned (box: run-suite.sh) |
| ZF61 | sock: the fifth type parses, builds with bind(2), walks back as a socket, and its absent mode= resolves to what bind leaves (0777 under the umask) | covered: run-fixtures.sh and check_roundtrip.c over sock-copy.zrt, which build the three trees and walk them; the apply of one is ZA7 |
| ZF62 | flags-onto-rm.zrt: schg on an onto file an action removes -> the apply clears it and removes the file | planned (box: run-suite.sh; schg needs root, and comes off again only while securelevel is 0 or less) |
| ZF63 | the runner asserts the exit status the expect block implies, and not the manifest alone: 0 where the block's #conflicts line is 0 and 1 where it is not, so a run that emits the right document with the wrong status fails the fixture (R22) | covered: run-fixtures.sh, over every fixture of the flat directory |
| ZF64 | --build-fixture over a fixture of another platform prints the builder's own reason, naming the platform line, and not the bare errno of it | covered: run-fixtures.sh, which runs the driver over tests/fixtures/freebsd/ off the platform those name |

ZF33 to ZF49 are --edit-fixture, added with the mode itself (issue
fixture-edit) and all of them Mac cells: the mode is plain POSIX
plus the same two platform sections the builder has, so the only
rows the Mac cannot reach are the two attributes it has no form of,
which are ZF50 and ZF51. Those two are the box's, and the box's
alone: they are edits and not builds, so no fixture run closes
them and check_fixture.c has to grow them and be run on FreeBSD.
Every one of the seventeen runs the same way: base is built into one
directory, edited into a side, and that side is built into another
from nothing, and the two must then be equal name for name, pool for
pool, attribute for attribute and byte for byte. That equality is
the positive proof of the mode -- an edit and a build must land in
the same place -- and the inode and ctime rows are the proof that it
got there without touching what it did not have to.

ZF17 and ZF20 are the two the Mac cannot reach at all: it has no
extended-attribute namespaces and no NFSv4 ACLs, so a fixture
carrying either says "platform freebsd" and lives in
tests/fixtures/freebsd/, where run-fixtures.sh skips it and counts
the skip. What the Mac does prove of them is ZF18 and ZF21: the
fixture parses, the attribute reaches the handle, and the expect
block is held against the theory by hand rather than by the tool.

ZF52 to ZF60 are the nine that issue attr-cells wrote, and they
are where the two attributes meet the rest of the engine rather
than the format alone: an ACL and a system-namespace attribute
kept, copied, stripped, agreed on and disagreed over, and one
fixture carrying all four attribute kinds at once. Their expect
blocks were derived by hand from v4-yellow-content.md -- an
attribute is content, so a one-sided change is a write or a cp, a
two-sided differing change is changed-both, and an identical
change on both sides is clean with nothing to do, since onto
already holds the result's content. run-suite.sh walks
tests/fixtures/freebsd/ as well as the flat directory, so the box
runs them with everything else. The Mac runs none of it and, as
things stand, parses none of it either: check_roundtrip.c scans
the flat directory alone and run-fixtures.sh skips a platform
fixture without loading it, so of tests/fixtures/freebsd/ only
acl-nfsv4.zrt and sysxattr.zrt are parsed on the Mac at all, by
name, in check_fixture.c's check_boxonly. The nine were parsed and
their manifests emitted by hand when they were written, through
zr_fixture_load and zr_fixture_to_tree, which touch no filesystem
and so agree with the theory without a box. Making check_roundtrip
or check_boxonly scan the directory would close that gap and is
not attr-cells's to do.
The cells that are not fixtures at all -- a nested mount inside an
input, the securelevel refusal, a snapshot destroyed under a
running rebase -- are tests/box/run-precond.sh, which closes the
first and documents the other two.

## ZY -- verify and idempotent apply (check_verify.c)

Plotted 2026-09-03, before its tests, for the verify-mode issue of
sprints/sprint-5. Dimensions: action {rm, ln, cp, dup, write,
conflict}; what onto held at the name {an object, absence}; what
the result holds {the action's own outcome, onto's original,
neither, nothing}; pooling in the result {one pool with the anchor,
its own, torn from its pool}; outcome {done, pending, blocked,
drifted, unchecked}; the trees available {all three, from gone, onto
gone}; the name no action spoke for {edited, added, deleted, covered
by a conflict, under a removed directory, torn out of its pool, still
onto's, another name of an object an action made}; the apply's input
{no report, a report}; the self-check {with the fix, which is
applying1, and without it}; repetition {a first run, a second run
over what the first left}.

Rows ZY60 and up were plotted 2026-09-04, before their tests, for
the one-verify issue: the second pass became a per-name list over
the whole name table, and applying1 gained the repair over it.

Rows ZY109 and ZY110 were plotted 2026-09-08, before their tests,
for the review-verify-verb issue. One dimension comes with them: the
pooling of a name an action made {an object of its own, one object
with the name the action names as its source, one object with an
untouched onto name}, crossed with the second pass's exemptions
{the name acted on, the names of a marked result pool}.

Rows ZY80 and up were plotted 2026-09-04, before their tests, for
the verify-choices issue, which gave the classifier its third
input. Two dimensions come with it: the choice on a line {keep,
onto, from, "-"} and what the side it names holds at that name {an
object, absence}, crossed with what the result holds {the chosen
object, onto's original, neither, nothing} and with the pooling of a
group that chose one side {one object as that side holds them,
torn}. The name a line covers is crossed with the second pass as
well: a covered name is exempt from it on both axes, and a
directory line covers the names beneath it.

The whole grid runs on the Mac: three directories, a manifest as
text or one the pipeline emitted, and a result doctored by hand,
which is the --posix form of everything but the attributes only ZFS
has. What is deferred is deferred for the reason every other family
defers: a real ACL, the two extended-attribute namespaces, a
snapshot, a clone and a kill need the box.

| cell | scenario | disposition |
|------|----------|-------------|
| ZY1 | rm: the name is gone | covered: check_verify.c |
| ZY2 | rm: the name is still onto's | covered: check_verify.c |
| ZY3 | rm: the name holds something else | covered: check_verify.c |
| ZY4 | rm of a directory a conflict holds open: blocked | covered: check_verify.c |
| ZY5 | ln: the name is the anchor's own object | covered: check_verify.c |
| ZY6 | ln: the name is not there yet | covered: check_verify.c |
| ZY7 | ln: the name is still onto's | covered: check_verify.c |
| ZY8 | ln: the name is some third object | covered: check_verify.c |
| ZY9 | cp: the name equals from's path | covered: check_verify.c |
| ZY10 | cp of a new name, still absent: pending, not done | covered: check_verify.c |
| ZY11 | cp over a name onto had, still onto's | covered: check_verify.c |
| ZY12 | cp: the name is neither | covered: check_verify.c |
| ZY13 | dup: severed and equal to onto's anchor | covered: check_verify.c |
| ZY14 | dup: still one file with the anchor: pending | covered: check_verify.c |
| ZY15 | dup: severed with an attribute changed | covered: check_verify.c |
| ZY16 | write: from's bytes, and the pool the manifest implies | covered: check_verify.c |
| ZY17 | write: still onto's bytes | covered: check_verify.c |
| ZY18 | write: bytes nobody asked for | covered: check_verify.c |
| ZY19 | write: from's bytes but the pool torn | covered: check_verify.c |
| ZY20 | a conflict mark: classified as nothing, counted nowhere | covered: check_verify.c |
| ZY21 | a name under a conflicted directory: never an entry of the list | covered: check_verify.c |
| ZY22 | an untouched name edited: an entry, of the changed kind | covered: check_verify.c |
| ZY23 | a name onto never had: an entry, of the extra kind | covered: check_verify.c |
| ZY24 | no entry: an untouched name that still matches | covered: check_verify.c |
| ZY25 | no entry: another name of an object an action made | covered: check_verify.c |
| ZY26 | the counts and the firsts, in manifest order | covered: check_verify.c |
| ZY27 | applied twice over a pristine tree: one place | covered: check_verify.c |
| ZY28 | idempotence: rm of a name already gone | covered: check_verify.c |
| ZY29 | idempotence: cp over the object an earlier run made | covered: check_verify.c |
| ZY30 | idempotence: a directory to create that is there | covered: check_verify.c |
| ZY31 | idempotence: an ln already standing | covered: check_verify.c |
| ZY32 | a report: done and blocked left alone, in zs_skipped | covered: check_verify.c |
| ZY33 | a report: pending and drifted performed | covered: check_verify.c |
| ZY34 | a report: a done ln whose anchor was rebuilt, done again | covered: check_verify.c |
| ZY35 | the blocked removal left alone with no report at all | covered: check_verify.c |
| ZY36 | a directory rm not empty and not conflicted: still loud | covered: check_verify.c |
| ZY37 | an NFSv4 ACL or a system xattr told apart in a classification | deferred: needs ZFS and root; box, attr-cells |
| ZY38 | a kill at a gate, then --verify and a plain --continue | planned: box, box/run-kills.sh, which verifies after every kill and then continues under no flag |
| ZY39 | a stray edit and a stray delete in a real result, both forms | planned: box, box/run-strays.sh |
| ZY40 | cp with the from tree gone: unchecked | covered: check_verify.c |
| ZY41 | write with the from tree gone: unchecked | covered: check_verify.c |
| ZY42 | rm with onto gone: unchecked while the name is there, done once it is not | covered: check_verify.c |
| ZY43 | dup with onto gone: unchecked | covered: check_verify.c |
| ZY44 | ln standing on its anchor, and a cp the result already holds: done with onto gone | covered: check_verify.c |
| ZY45 | no name list at all with onto gone | covered: check_verify.c |
| ZY46 | the name list over real trees: an untouched file edited and a name no tree had, both put back by applying1's self-check while the result is still the run's own, and --verify afterwards reports neither | planned: box, box/run-strays.sh |
| ZY47 | a stray edit to a name an action will make: the action runs after it and overwrites it, and the name classifies done | planned: box, box/run-strays.sh |
| ZY48 | a stray delete of an untouched name at applying1: reported gone -- the pass is over the name table and not over what the result holds -- restored from onto by the self-check, and the run goes on to its normal end | planned: box, box/run-strays.sh |
| ZY49 | an edit to a conflicted name in a real result: never classified and never repaired | planned: box, box/run-strays.sh |
| ZY50 | drift after the stage, in both forms and on both branches: --verify exits 3 naming it and writes nothing, a plain --continue reports it at the gate it arrives at and writes nothing into the tree either, and the done gate reports it, exits 3 and passes rather than failing | planned: box, box/run-strays.sh cases 3 and 6 |
| ZY60 | gone: a name onto had that the result does not, which the second pass sees because it is over the name table and not over what the result holds | covered: check_verify.c |
| ZY61 | extra: a name the result holds that nothing expected | covered: check_verify.c |
| ZY62 | changed: a name there, but not the object onto had | covered: check_verify.c |
| ZY63 | unpooled: a pool of untouched names torn in two, one entry on the second name with the first as its anchor | covered: check_verify.c |
| ZY64 | the applying1 repair over one of each kind: restored, removed and relinked, the result onto again and the list empty after it | covered: check_verify.c |
| ZY65 | a conflicted name edited or deleted: no entry on either axis | covered: check_verify.c |
| ZY66 | a name under a directory the manifest removes: expected to be nothing, so not gone | covered: check_verify.c |
| ZY67 | the check without the fix, which is applying2 and done: the names are not looked at and nothing is written | covered: check_verify.c |
| ZY68 | onto gone: the list is empty, as the info count was | covered: check_verify.c |
| ZY69 | one doctored tree down both applying1 paths, a fresh run's and a --continue's: one function, one repair, one tree | covered: check_verify.c |
| ZY80 | a keep line: the name is never compared and never an entry of the name list, however it was edited | covered: check_verify.c |
| ZY81 | an onto line: the result holds onto's object, which is done | covered: check_verify.c |
| ZY82 | an onto line: the result holds anything else, which is drifted -- the expected and the original are one object here, so pending cannot arise | covered: check_verify.c |
| ZY83 | a from line before applying2: the result still holds onto's own and from's differs, which is pending | covered: check_verify.c |
| ZY84 | a from line: the result holds from's object, which is done | covered: check_verify.c |
| ZY85 | a from line: the result holds neither side's object, which is drifted | covered: check_verify.c |
| ZY86 | a from line where from has no such name: the expected is absence, so pending while onto's object is still there, done once it is gone, drifted where a third object stands | covered: check_verify.c |
| ZY87 | an onto line where onto has no such name: absence again, done only where the result does not hold it | covered: check_verify.c |
| ZY88 | a group of two names that chose from: one object in the result, as from pools them, is done; two copies of the right bytes is drifted | covered: check_verify.c |
| ZY89 | a "-" line and a keep line: classified as nothing and counted in no outcome, so the counts and the firsts are the onto and the from lines alone | covered: check_verify.c |
| ZY90 | a directory line covers the names beneath it, which is the manifest's own scoping | covered: check_verify.c |
| ZY91 | a line whose side is one of the trees that is not there: unchecked, for either side | covered: check_verify.c |
| ZY92 | the drift round trip through the library: every entry of the name list becomes a drift keep line, written and parsed back, and the classification with that document says nothing about those names | covered: check_verify.c |
| ZY93 | a name a resolution line covers, deleted or added: no entry on the name axis either | covered: check_verify.c |
| ZY94 | a stray edit to a clean name at the conflicts gate: a plain --continue writes it into the resolution as a drift keep line, the rebase reaches done with the edit intact, and --verify afterwards reports the name under the resolution and not as drift | planned: box, box/run-strays.sh case 5, which asks by the kept manifest now that a settled result answers |
| ZY95 | the invocation that reaches the done gate makes the final check there, whether it is the fresh run or a --continue, and no flag is given or needed: there is no recorded request (record-slim took the property away) and no request form left (verify-schedule took the flag away) | planned: box, box/run-kills.sh, whose --continue after every kill reaches it, and box/run-resolution.sh cases 1, 4 and 6, which read the report out of the invocation that reaches done |
| ZY96 | a type change (rm and a make on one name): before the apply both lines pending | covered: check_verify.c |
| ZY97 | a type change after the apply: the name holds the later line's product, the removal reads done and not drifted; a name with no later line that holds something else stays drifted (ZY2) | covered: check_verify.c; box: run-replay.sh over type-change.zrt |
| ZY100 | the shape a settled check exits 0 on: every action of the manifest done, the one name no action spoke for as onto had it, and the conflicted name the person's by a keep -- the verdict rule's four inputs all at zero | covered: check_verify.c |
| ZY101 | the same with the from side gone, which is what #made from leaves at done: the cp comes back unchecked rather than drifted, the rm still reads done, the name list is unchanged, and the verdict does not move | covered: check_verify.c |
| ZY102 | the first shape a settled check exits 3 on: an edit to a name the manifest never spoke for is one entry of the name list, and onto is what says so, so the from side being gone takes nothing from it | covered: check_verify.c |
| ZY103 | the second: an edit over an action's own name is that action drifted -- and there the answer rests on from's bytes, so with from gone it is unchecked and nobody can say, which is the limit of a check made long after the rebase | covered: check_verify.c |
| ZY104 | the verdict reads the resolution's outcomes as well: a resolved name edited by hand after applying2 is one drifted line and nothing else -- no action pending or drifted, no entry of the name list -- so a verdict blind to that fourth input calls a drifted result clean | covered: check_verify.c; box: run-resolution.sh case 4, which asks it of the settled result |
| ZY105 | what a new line is written as, by phase: applying1 resets the drift and writes none, the conflicts gate writes keep, the done gate writes "-" | planned: box, run-strays.sh cases 5 and 6 and run-resolution.sh case 6 for the first two, case 9 for the done gate |
| ZY106 | a conflict line the manifest marks that a hand edit removed is put back at the conflicts gate with the take mode's answer -- onto, from, or "-" in standard mode -- and the header counts move with it | planned: box, run-resolution.sh case 10, under each take mode |
| ZY107 | a conflict line for a name the manifest never marked is carried out like a drift line with that choice, and the classification holds it against the side it names | covered: check_apply.c and check_verify.c; box: run-resolution.sh case 10 |
| ZY108 | a directory line the chosen side lacks that a keep line under it holds open reads pending or drifted after the choices: choices_hold passes it, the done gate counts it, the exit is 3 and done is reached all the same | covered: check_verify.c; box: run-resolution.sh case 9 |
| ZY109 | a cp whose target the result holds as the very object it holds at the cp's source name: the copy was never made, so the classification goes on to ask onto and answers drifted, where the same tree with the link cut is done; a cp of a from name to that same path has no second name and is asked nothing, which is ZY9 and ZY11 standing | covered: check_verify.c |
| ZY110 | the result pool is marked for a write and for no other action: an untouched onto name a hand linked to a cp or a dup target is not exempted from the second pass, so the tear is an entry of the name list where before both halves went unseen | covered: check_verify.c |
| ZY111 | the walk a stage's self-check ends with is the walk the stage goes on with: zr_apply_check hands it and the oracle over it back and rescan_result adopts them, and the classification after the stage is the one it was before | covered: check_apply.c and check_verify.c, which pass NULL and so still walk twice, and the whole fixture suite through run-fixtures.sh; box: run-fixture.sh steps 2 and 5 and run-kills.sh, where the stage really runs |
| ZY112 | applying2's idempotence check over one walk: the first pass is made over the walk the verb already held, the tree is walked once between the passes, and the second pass -- which must change nothing -- leaves that walk true for the done gate | planned: box, box/run-resolution.sh cases 1, 4, 6 and 9; a second pass that did change something is the internal failure the case's message names |
| ZY113 | the fresh run's done gate is handed the run's own three trees and its libzfs handle, and makes the same check the same function makes for a --continue: from and onto are held snapshots, the result is read-only and unwritten since the self-check walked it, and where any of that has moved -- a snapshot renamed, a tree missing, the result mounted elsewhere -- the gate reads everything for itself | planned: box, box/run-fixture.sh step 5 under -v ("the final check reads the three trees this run walked"), and box/run-kills.sh, whose --continue reaches the same gate with no lending at all |

ZY40 to ZY45 are the post-done verify's: a tree that is not there
any more is walked as the empty tree and named in the missing mask,
and every action that would have had to read it is unchecked. The
box rows for a really destroyed snapshot are ZX56 and ZX57.

ZY19's drift is reported and not repaired: a re-write mends the
bytes of the name it is on and cannot rejoin a pool somebody tore,
so an apply will make the write true again and verify will still
say drifted. The repair of ZY64 does not reach it either -- it puts
back the names no action spoke for, and the torn name here is the
write's own pool -- so a stray that tears a written file's pool at
applying1 stops the run as an internal failure, which is what it
did before this list existed.

ZY60 to ZY69 are the one verify of the plan's 2026-09-04 revision:
one algorithm at every gate, a per-name list beside the per-action
outcomes, and a fix only at applying1, where the result is the
run's own and anything off the expected tree is a stray.

ZY109 and ZY110 are review-verify-verb's, and they are R21: one
edit made through two blind spots at once. zv_marks marked the
result pool of every action's name and zv_untouched skipped every
other name in a marked pool, which is right for a write -- the
object is onto's own and every name onto gave it sees the new bytes
through it, and zv_kept_pool polices the shape of that sharing --
and wrong for everything else, since a cp, a dup and an ln make an
object of their own at the name. And cp, alone of the two makes,
was never asked whether it had severed. So a cp target somebody
hard-linked onto an untouched onto name read done, and the untouched
name left the second pass: the mark is now a write's alone and cp is
asked the question dup is, with the one guard dup never needs -- a
source name that is the target name has no second name to be one
object with, which is what a cp of a from file to its own path is.

ZY111 to ZY113 are review-cost's, and all three are one rule: a walk
of a tree nothing has written to since is an input a later phase may
be handed, never an answer it may skip asking. What is passed is
the walk; the oracle goes with it only inside one stage, where the
review's R13 asks for it by name, and never across the gate to the
done check, which builds its own so that it compares what it is
given rather than inheriting a memo of comparisons somebody else
made. A clean clone-form run made seven full tree walks and now
makes four; a --continue through applying2 made six and now makes
four.

ZY104 to ZY108 are review-resolution's: the resolution as the
authority (documents-design.md, section 11.5). The verdict has four
inputs, not three -- an action pending or drifted, a line of the
resolution pending or drifted, and any entry of the name list -- and
the resolution's own outcomes are the only place a resolved name can
show at all, since a conflict mark is counted in no action outcome
and a chosen name is in no entry of the name list. What a new line
is written as goes by phase and not by the take mode: reset at
applying1, keep at the conflicts gate, "-" at the done gate; the
take mode is read for one thing only, the conflict line a hand edit
removed and the gate puts back. The Mac rows are the classifier's;
the writing is run.c's and is the box's, since a gate is a walk of
three real trees.

## ZI -- the interactive launch (check_args.c, check_run.c, box/run-resolution.sh)

Plotted 2026-09-10, before its tests, for sprint 6's launcher
(sprints/sprint-6/implementation-plan.md section 2): -i takes an
optional value naming a command, the tool forks a child on the
resolution at the conflicts gate and reads the document back when
the child exits 0.

Dimensions: the value form {none, a bare word after -i, attached
with =}; the command {a fresh run, --continue, the verbs that refuse
-i, a dry run}; the bare words on the line {none, one, two, three}
crossed with where -i stands among them; what the child does {exits
0 with the document complete, exits 0 with a name unanswered, exits
0 with a document the parser refuses, exits non-zero, dies of a
signal, cannot be run at all}; the document when the child opens
{unanswered, complete by a --take flag, complete by hand, drift
lines just written by the gate's verify}; the flags beside -i
{--no-merge, --take-onto, --take-from}; the path to the gate {the
fresh run, --continue from applying1, --continue at conflicts,
--restart then --continue}; the child's arguments {the resolution's
path, the value's own flags, the four tree paths of the built-in};
signals while the tool waits {SIGINT to the group, SIGTERM to the
tool, SIGKILL to the tool}; the terminal {left as found, left
changed by the child}; and the built-in {no picker in the build}.
ZI1 to ZI12 are read off struct zr_args on any machine. ZI13 to
ZI23 run the launcher on the mac with shell scripts as the child:
a script records its arguments, exits as told, signals its parent,
or changes the termios of a pty the test opened. The pty is opened
with posix_openpt(3) and not openpty(3), which lives in -lutil on
FreeBSD and would put a library on the unit tests' link line for one
cell; the slave goes on the test's own standard input, which is what
the launcher's isatty reads.
ZI24 onward are box rows in run-resolution.sh case 12, whose editor
is one script written into the scratch directory, with ZR_ED_MODE
saying what it does to the document it is handed: answer it whole,
answer one line and stop, leave one name, write a duplicate line,
copy it and touch nothing, or answer it and then linger so that the
tool can be killed while it runs. It appends a line per run, so a
cell can say that no child opened at all, and it keeps a copy of
what it was handed, which is how the cells about what the child sees
are made.

| cell | scenario | disposition |
|------|----------|-------------|
| ZI1 | -i alone on a fresh run: interactive, and no command (the built-in) | covered: check_args.c |
| ZI2 | -i CMD on a fresh run: the command is CMD | covered: check_args.c |
| ZI3 | --interactive=CMD: the command is CMD, on a fresh run and on --continue | covered: check_args.c |
| ZI4 | -c -i IDENT: the only bare word on a verb's line is IDENT, and the built-in | covered: check_args.c |
| ZI5 | -c -i CMD IDENT: the command is CMD and IDENT the other | covered: check_args.c |
| ZI6 | -c IDENT -i CMD: the same; the position of -i does not matter | covered: check_args.c |
| ZI7 | -c -i CMD IDENT EXTRA: refused as a second identifier | covered: check_args.c |
| ZI8 | -i --from A: the next word is a flag, so the built-in, and --from is parsed | covered: check_args.c |
| ZI9 | --interactive= with an empty value: refused | covered: check_args.c |
| ZI10 | -i on --restart, --abort and --verify: refused, with or without a value | covered: check_args.c (the rule of ZX gate flags, carried) |
| ZI11 | -i beside --dry-run: refused, with or without a value | covered: check_args.c (ZX241, carried) |
| ZI12 | a value with its own flags, "CMD --flag", is one command string, never split | covered: check_args.c |
| ZI13 | the child receives the resolution's path as its last argument, byte for byte | covered: check_run.c |
| ZI14 | the value's own flags reach the child before the path, in order | covered: check_run.c |
| ZI15 | the child exits 0: the launcher reports 0 | covered: check_run.c |
| ZI16 | the child exits N: the launcher reports N, and the file is as the child left it | covered: check_run.c |
| ZI17 | the child dies of a signal: reported non-zero, with the signal named | covered: check_run.c |
| ZI18 | the command cannot be run: reported non-zero, with the command named | covered: check_run.c |
| ZI19 | SIGINT while the tool waits reaches the child and not the tool: a child that sends INT to its parent and exits 0 is reported 0, and the tool is still there | covered: check_run.c |
| ZI20 | SIGTERM to the tool while it waits is forwarded to the child, and the launch reports non-zero, naming the signal, even when the child catches it and exits 0 | covered: check_run.c |
| ZI21 | termios saved before the fork are restored after a child that changed them, on a pty | covered: check_run.c, posix_openpt(3) rather than openpty(3), which wants -lutil |
| ZI22 | the built-in child on a document it cannot open: in a PICKER=no build the stub says there is no picker, in the default build the picker refuses before it touches a terminal; either way one line, exit 2, reported non-zero | covered: check_run.c, in both builds (make check and make PICKER=no check) |
| ZI23 | the built-in child's argv is RESOLUTION BASE FROM ONTO RESULT, in that order | covered: check_run.c |
| ZI24 | a fresh run with -i and a script that answers every line: done in one process, exit 0 | covered: box, box/run-resolution.sh case 12a |
| ZI25 | the script exits 1: the gate stands, exit 1, the resolution as the script left it with its partial answers | covered: box, box/run-resolution.sh case 12b |
| ZI26 | the script leaves a name unanswered and exits 0: the count printed, exit 1, the gate stands | covered: box, box/run-resolution.sh case 12c |
| ZI27 | the script writes a document the parser refuses (a duplicate line): refused as --continue refuses it, the gate stands | covered: box, box/run-resolution.sh case 12d |
| ZI28 | -c IDENT -i CMD at conflicts: the drift lines the gate's verify wrote are in the file when the script opens it | covered: box, box/run-resolution.sh case 12e |
| ZI29 | -c -i from applying1 (killed before the gate): the apply finishes, then the script opens | covered: box, box/run-resolution.sh case 12f |
| ZI30 | -O -i: the script opens on the complete skeleton | covered: box, box/run-resolution.sh case 12g |
| ZI31 | -i -M: the script opens, then --no-merge holds the gate | covered: box, box/run-resolution.sh case 12h |
| ZI32 | a rebase whose decision has no conflict, with -i: nothing opens, done | covered: box, box/run-resolution.sh case 12i |
| ZI33 | -i alone on the box with no picker in the build: the note, exit 1, the gate stands | covered: box, box/run-resolution.sh case 12j |
| ZI34 | the tool killed with SIGKILL while the script runs: the gate stands with the file as last saved, and -c continues | covered: box, box/run-resolution.sh case 12k |
| ZI35 | --restart IDENT then -c IDENT -i CMD: the skeleton again, and the script opens on it | covered: box, box/run-resolution.sh case 12l |

## ZP -- the built-in picker (check_picker.c, the box by hand)

Plotted 2026-09-10, before its tests, for sprint 6's picker and
merge epics (sprints/sprint-6/implementation-plan.md sections 3.1
to 3.6): the child -i runs when it names no command, given
RESOLUTION BASE FROM ONTO RESULT and answering with the three
statuses of section 3.2 -- 0 written with nothing unanswered, 1
saved and stop, 2 abandoned or no terminal. The picker depends on
the documents and the name codec and on nothing else (ground rule
1), so every cell here is a resolution, a manifest beside it, three
tree paths and a result path, and no pool of any kind.

Dimensions, in seven crossings.

The model's input, the resolution as zr_resolution_parse hands it
over: the line kind {a conflict line the skeleton wrote, a drift
line a gate wrote, a conflict line a hand added for a name the
manifest never marked} crossed with the choice it opens at {-,
keep, onto, from}; the group {a number the manifest has a record
for, a number it has none for, none at all}; the document's state
at open {a name unanswered, #unanswered 0 by a --take flag, 0 by a
hand, no lines at all}; and the documents the parse refuses before
the picker sees a line {two lines for one name, a name that is ".",
".." or holds a "/", a header count that misses, a header naming
another rebase, a snapshot name whose guid differs}.

The join with the manifest beside it, found by the sibling rule:
the group found or not found, which is what tells a hand-added line
from the tool's own; the object's kind read off the three trees
{text, binary, directory, link, socket, fifo, absent on a side, the
three disagreeing}; and the text rule of section 3.4 {a NUL in the
first 8000 bytes, a NUL after them, all three text, any one of the
three binary, a side that is not there at all}.

The cursor and the keys of screen 1: the key {up, down, f, o, k, -,
g, Enter, s, w, q, one the picker does not know} crossed with the
row it lands on {each line kind, each object kind, the first row,
the last row, no rows at all} and with the document's state
{something unanswered, nothing unanswered, saved once already,
never saved}.

The write: the path it takes {zr_doc_write, the .tmp sibling and
the rename}; what the header carries out {#names and #unanswered
recomputed by the library's writer}; the invariants {the file's
order, every line kept, byte equality with the emitter}; the
failures {a read-only directory, ENOSPC on the .tmp}; and the
moment {a crash between the rename and the exit}.

The terminal, ground rule 6: the way out {the normal return, an
error return, exit() through the atexit hook, SIGINT, SIGTERM,
SIGHUP, SIGSEGV, SIGBUS} crossed with what must hold after it {the
termios as they were found, endwin called, the signal re-raised
with its default disposition}; what the terminal is when the picker
starts {a pty, none at all, TERM unset, TERM a name terminfo does
not know, a window smaller than the layout}; and SIGWINCH while it
is up.

Screen 2 and the merge, over the chunk record diff3-walk produces:
the chunk kind {stable, from-only, onto-only, both-same, conflict}
crossed with the answer it takes {its own side, either side, the
person's pick, none yet}; the key {1, 2, b, n, p, the conflicts-only
toggle, w, Esc} crossed with where the cursor stands {the first
conflict, the last, a stable stretch, nothing unpicked left}; the
triple's shape {a base and both sides, add/add with no base,
delete/edit, an empty side, no final newline in each of the three
positions}; and the write {every conflict picked, one still
unpicked}.

Last, the merge's properties against merge-theory's exported
battery, and the standalone binary: the same five arguments and the
same three statuses with no tool around it.

ZP1 to ZP60, ZP69, ZP70 and ZP78 to ZP99 are the model and the
merge, which check_picker.c drives on the mac with key codes over a
resolution and a manifest from the fixtures' expected output: no
curses is linked and no terminal is opened, which is what plan
section 3.5 buys by keeping model.c and merge.c free of it. The
merge library's own cells -- the chunk kinds and their answers, the
write rule, the hint, the two shapes that are not merges, and the
battery of ZP104 to ZP109 -- are check_merge.c's, a second check
program of the same kind that needs no key and no row: the merge is
a library over three buffers of bytes and is tested as one, and what
is left over for check_picker.c is the keys and the screen. The
terminal cells a pty can show are check_picker.c's too, with the
standalone binary as the child on a pty from posix_openpt(3) rather
than openpty(3), which lives in -lutil on FreeBSD -- the trick ZI21
uses for the launcher's termios -- and they closed at picker-list,
which built the binary. The pty child dups the slave onto its three
standard descriptors and does not take it as a controlling terminal,
so that the slave this program reads the termios through is not
revoked when the child exits, and TERM is xterm, whose entry both
this machine and the box have. What is left is the drawing itself:
the colors, the ACS line drawing, a resize that stays above the
floor, TERM under sudo, and a merge written into a real result tree.
Those are done by hand on the box and recorded in the worklog of
picker-list or of picker-merge, which is the rule of plan section
3.5 -- every TUI session on the box goes into the worklog.

| cell | scenario | disposition |
|------|----------|-------------|
| ZP1 | one row per resolution line, in the file's order, and nothing else is a row | covered: check_picker.c |
| ZP2 | a conflict line at each of the four choices opens at that choice | covered: check_picker.c |
| ZP3 | a drift line: no group, the GRP column says drift, the choice as the gate wrote it | covered: check_picker.c |
| ZP4 | a hand-added conflict line, whose group no record of the manifest answers to: a row like any other | covered: check_picker.c |
| ZP5 | a conflict line whose group the manifest has: the why line, the class and the three trees under the list | covered: check_picker.c |
| ZP6 | a drift line and a hand-added line have no detail to show, and the picker says so rather than leaving the last row's detail up | covered: check_picker.c (the row carries no record; the detail pane draws that row's own words on a pty) |
| ZP7 | two names of one group: both rows carry the number, and the detail says how many names the group holds | covered: check_picker.c |
| ZP8 | the object's kind per row off the three trees: text, binary, directory, link, socket, fifo | covered: check_picker.c |
| ZP9 | the text rule: a NUL within the first 8000 bytes is binary, a NUL after them is not | covered: check_picker.c |
| ZP10 | any one of the three binary makes the row binary, and no merge view exists for it | covered: check_picker.c |
| ZP11 | the three disagree on the kind (a file on from, a directory on onto): the row says so and Enter opens nothing | covered: check_picker.c |
| ZP12 | a side absent (add/add, delete/edit, deleted on both): the kind comes from the sides that hold the name | covered: check_picker.c |
| ZP13 | BASE given as "" (a run with no base): the detail says there is none and every merge is the two-way compare | covered: check_picker.c (base "" is absent on every row, so every text name is an add/add and opens on the two-way compare; a binary one still does not) |
| ZP14 | a side path given as "": the kind is read from the trees there are, and Enter says why not | covered: check_picker.c |
| ZP15 | a hand-added line for a name none of the three trees holds: the row draws with no kind and opens nothing | covered: check_picker.c |
| ZP16 | a document complete at open by --take-onto: every conflict row at onto, and the picker opens all the same (ruling 2) | covered: check_picker.c |
| ZP17 | the same by --take-from, and the same for a document a hand answered before -i was given | covered: check_picker.c |
| ZP18 | an empty resolution: no rows, the counts 0, and w writes it and exits 0 | covered: check_picker.c |
| ZP19 | a resolution of drift lines only: rows, no groups, nothing unanswered | covered: check_picker.c |
| ZP20 | a conflict line the manifest marks that the document lacks: no row for it and nothing added, since the re-add is the gate's (documents-design.md 11.5) | covered: check_picker.c |
| ZP21 | two lines for one name: the parse refuses, its reason is printed, exit 2, and nothing is drawn | covered: check_picker.c |
| ZP22 | a name that is ".", ".." or holds a "/": the same refusal | covered: check_picker.c |
| ZP23 | a header naming another rebase: refused before anything is drawn, exit 2 | covered: check_picker.c |
| ZP24 | a snapshot name whose guid differs from the manifest's: refused with both numbers, exit 2 | covered: check_picker.c |
| ZP25 | a header count that does not match the lines: the parse's refusal, exit 2 | covered: check_picker.c |
| ZP26 | the manifest missing beside the resolution, or unreadable: exit 2 naming the path, before the terminal is touched | covered: check_picker.c |
| ZP27 | a directory line's trailing slash: a row of kind D whose isdir survives to the write | covered: check_picker.c |
| ZP28 | a name whose bytes want the escaping (a space, a newline, a high byte): the row shows the decoded bytes and the write puts the escaping back | covered: check_picker.c |
| ZP29 | up on the first row and down on the last stay where they are: no wrap | covered: check_picker.c |
| ZP30 | up and down over a document of one row, and over none | covered: check_picker.c |
| ZP31 | f, o and k on a conflict line set from, onto and keep | covered: check_picker.c |
| ZP32 | f, o and k on a drift line: the same three, a drift line being answered like any other | covered: check_picker.c |
| ZP33 | f, o and k on a hand-added conflict line: the same three (v4-manifest.md 8: carried out like a drift line with that choice) | covered: check_picker.c |
| ZP34 | "-" on a conflict line clears it back to unanswered | covered: check_picker.c |
| ZP35 | "-" on a drift line: refused, with a line saying only a conflict line starts unanswered | covered: check_picker.c |
| ZP36 | "-" on a hand-added conflict line clears it, and it counts unanswered | covered: check_picker.c |
| ZP37 | g moves to the next row of the same group and wraps within the group | covered: check_picker.c |
| ZP38 | g on a drift line or a hand-added line: nothing moves, and it says the line has no group | covered: check_picker.c |
| ZP39 | g on a group of one name: the cursor stays put | covered: check_picker.c |
| ZP40 | Enter on a text row opens screen 2 | covered: check_picker.c (the model answers ZR_PK_OPEN and zr_pk_merge_open reads the three objects; the screen drawn on a pty, with the row and "hunk 1 of 1" in its rule) |
| ZP41 | Enter on a binary, a directory, a link, a socket or a fifo: nothing opens and the line says why not | covered: check_picker.c |
| ZP42 | Enter on a delete/edit row: nothing opens, it being a choice and not a merge | covered: check_picker.c |
| ZP43 | the counts in the header after each change: conflicts, groups, unanswered, drift | covered: check_picker.c |
| ZP44 | s writes the document and stays on the list, with the rows as they were | covered: check_picker.c |
| ZP45 | s then q: exit 1, with the file holding what s wrote | covered: check_picker.c |
| ZP46 | q with nothing saved: exit 2, and the file untouched byte for byte | covered: check_picker.c |
| ZP47 | w with nothing unanswered: written, exit 0 | covered: check_picker.c |
| ZP48 | w with something unanswered: refused, the count and the first unanswered name said, nothing written, the picker still up | covered: check_picker.c |
| ZP49 | w again after the last unanswered name is answered: written, exit 0 | covered: check_picker.c |
| ZP50 | a key the picker does not know: ignored, and nothing changes | covered: check_picker.c |
| ZP51 | every key on an empty document: only w and q do anything | covered: check_picker.c |
| ZP52 | the write goes through zr_doc_write: a .tmp sibling and a rename, so a reader finds one whole document or the other and never half | covered: check_picker.c |
| ZP53 | the document written after a key sequence differs from the one opened at exactly the lines those keys touched, and nowhere else | covered: check_picker.c |
| ZP54 | #names and #unanswered in the written header are the library writer's own count of the lines, never a number the picker carried | covered: check_picker.c |
| ZP55 | the order out is the order in, for a sequence that answers the rows back to front | covered: check_picker.c |
| ZP56 | every line it opened with is in the file it writes: never one fewer, whatever was pressed (v4-manifest.md 8: a hand cannot take a conflict away) | covered: check_picker.c |
| ZP57 | byte equality: what the picker writes for a set of choices is what zr_resolution_write writes for the same lines | covered: check_picker.c |
| ZP58 | an unchanged document written by w is byte-identical to the one opened | covered: check_picker.c |
| ZP59 | a write that fails (a read-only directory, ENOSPC on the .tmp): the message after endwin, the destination as it was, no .tmp left, a non-zero exit | covered: check_picker.c |
| ZP60 | s twice and then w: one document with the last choices, and no line doubled | covered: check_picker.c |
| ZP61 | a crash between the rename and the exit: the written document stands, no .tmp beside it, and the next --continue reads it | planned: box, by hand, in the worklog of picker-list |
| ZP62 | the termios saved before initscr are back on the normal return (w, exit 0) | covered: check_picker.c on a pty, with the standalone binary as the child |
| ZP63 | back on the error return: a document the parse refuses, exit 2 | covered: check_picker.c on a pty |
| ZP64 | back on exit() through the atexit hook | planned: no path of the picker's own reaches exit(3) while curses is up -- it returns its status and the launcher _exits it -- so the hook is a belt against a library that does (ncurses exits on its own errors), and forcing one would mean test-only code in the product. The hook is installed before newterm and calls the same idempotent restore every other way out calls, which check_picker.c's other pty cells exercise |
| ZP65 | back on SIGINT, and the picker dies of SIGINT: the handler calls endwin, restores and re-raises with the default disposition | covered: check_picker.c on a pty |
| ZP66 | the same for SIGTERM and for SIGHUP | covered: check_picker.c on a pty, with SIGQUIT beside them |
| ZP67 | the same for SIGSEGV and SIGBUS, whose default disposition still takes what it takes | covered: check_picker.c on a pty |
| ZP68 | nothing of ours reaches stdout or stderr while curses is up: a pty recording every byte sees no message until after endwin | covered: check_picker.c on a pty (a message the open queued, held against the offset of exit_ca_mode). A refusal the person's own key makes is drawn in the key bar through curses, which is what zr_pk_last is for; the rule is about a write that goes around curses to the stream |
| ZP69 | the queued messages are printed after endwin, in the order they were queued | covered: check_picker.c (the queue's order and its oldest dropped as a unit; the printing after endwin on a pty) |
| ZP70 | no terminal at all: refused with a line and exit 2, before initscr is called | covered: check_picker.c, the standalone binary with /dev/null on its standard input and a pipe for its output |
| ZP71 | TERM unset: refused, exit 2, nothing drawn | covered: check_picker.c on a pty |
| ZP72 | TERM a name terminfo does not know: the same | covered: check_picker.c on a pty |
| ZP73 | SIGWINCH redraws and keeps the cursor on its row | planned: box, by hand, in the worklog of picker-list. A resize that leaves the window at or above the floor redraws; one that takes it below is ZP75's refusal |
| ZP74 | the shell's own stty after every way out on the box: echo and canonical mode as they were found | planned: box, by hand, in the worklog of picker-list |
| ZP75 | a window below the floor of 80 by 24 is refused and never drawn into, at startup and on a resize alike: at startup one line naming the size and exit 2 with curses never opened; a shrink while the picker is up ends curses, puts the termios back, prints the same line and exits 2 with nothing written, dropping what was not saved (ruled 2026-09-10: "refuse and exit without continuing the merge (even if resolution file is complete, consider it a user-kill on gui)"). Between the floor and the mockup's 100 columns the name column is truncated and then the detail lines are dropped, and nothing is ever written outside the window | covered: check_picker.c on a pty (one column short, one row short, and a shrink under a running child) |
| ZP76 | the colors and the ACS line drawing, and the fallback on a terminal that has neither | planned: box, by hand, in the worklog of picker-list; the source's ASCII half is tools/gate.sh, which make gate runs |
| ZP77 | curses under sudo with the person's TERM and TERMINFO: it opens, or it refuses cleanly, and never draws garbage (plan section 6) | planned: box, by hand, in the worklog of picker-list |
| ZP78 | a stable chunk: shown in all three panes, with no answer to give | covered: check_merge.c, the record's half -- three ranges of one length and no answer to give; check_picker.c on a pty for the three panes, where a stable line stands in FROM, in ONTO and in RESULT |
| ZP79 | a from-only chunk: the answer is from, with no key pressed | covered: check_merge.c |
| ZP80 | an onto-only chunk: the answer is onto, with no key pressed | covered: check_merge.c |
| ZP81 | a both-same chunk: not a conflict, and one copy of it in the result | covered: check_merge.c |
| ZP82 | a conflict chunk: unanswered until 1 or 2, the two halves between markers on the screen until then | covered: check_merge.c, the record's half -- unanswered until zr_m3_pick, and the last pick stands; check_picker.c on a pty for the markers, which are drawn and reach no file |
| ZP83 | 1 takes from and 2 takes onto for the chunk under the cursor, and for no other chunk | covered: check_picker.c |
| ZP84 | 1 then 2 on one chunk: the last pick stands | covered: check_picker.c |
| ZP85 | b shows the chunk's base range, which every chunk keeps, conflict chunks included (ruling 5; eager is the ceiling) | covered: check_picker.c (the toggle, with the picks unchanged) and on a pty (base's own line drawn in the result pane's place) |
| ZP86 | n and p move between conflict chunks and stop at the ends | covered: check_picker.c |
| ZP87 | n and p with one conflict chunk, and with none at all | covered: check_picker.c |
| ZP88 | the conflicts-only toggle hides the stable stretches and shows them again, with the picks unchanged | covered: check_picker.c (the toggle and the picks) and on a pty (each stable chunk one dim line, in all three panes) |
| ZP89 | the inside-conflict diff marks lines within a conflict chunk and never splits it: the chunk sequence is the same with the hint computed and without it | covered: check_merge.c, zr_m3_hint over every conflict chunk of every battery case, the sequence held against a copy taken before |
| ZP90 | w with every conflict chunk picked: the merged bytes go into the result's object at that name, in place | covered: check_picker.c (the bytes compared, and the inode and the mode unchanged across the write) and on a pty end to end |
| ZP91 | w with one conflict chunk unpicked: refused, naming the first, and nothing written (ruling 7) | covered: check_merge.c, zr_m3_result refuses and names the first unpicked chunk; check_picker.c, the screen queues that line, moves to that hunk, stays on screen 2 and leaves the object as it was |
| ZP92 | no marker ever reaches the bytes written, by any path through the screen | covered: check_picker.c, over a merge with one hunk taken from from and one from onto |
| ZP93 | a write from screen 2 sets the row's choice to keep, and the list shows it | covered: check_picker.c (the row reads keep, the unanswered count falls, and the resolution s writes carries it) |
| ZP94 | Esc goes back to the list with the row's choice as it was: only w changes it | covered: check_picker.c (the picks go with the merge and the result's object is untouched) |
| ZP95 | add/add with no base: the two-way compare with a pick, under the same write rule | covered: check_merge.c (the chunks) and check_picker.c (the row opens with no base object, takes a pick and writes) |
| ZP96 | delete/edit: nothing opens, and the line says it is a choice and not a merge | covered: check_merge.c, zr_m3_open refuses with that line; check_picker.c, Enter says which tree is missing and no merge opens |
| ZP97 | no final newline on base, on from, on onto, and in the merged result: the fact is kept and never invented | covered: check_merge.c |
| ZP98 | an empty from, an empty onto, an empty base | covered: check_merge.c |
| ZP99 | a merge whose result is byte-identical to one side is written all the same, and the row still set to keep | covered: check_merge.c, the result is produced all the same; check_picker.c, a merge with nothing to answer is written and the row set to keep |
| ZP100 | a text object of tens of megabytes: the merge finishes and the panes scroll, libdiff's divide-and-conquer path taken | planned: box, by hand, in the worklog of picker-merge |
| ZP101 | the write is in place, so the object's mode, owner, times and extended attributes stand | planned: box, by hand, in the worklog of picker-merge, where the extended attributes and the owner are read with ls -lo and lsextattr; check_picker.c already holds the inode and the mode across a write on the mac |
| ZP102 | the two panes scroll together: a filler line in one is a real line in the other, and the chunk under the cursor is the same in both | planned: box, by hand, in the worklog of picker-merge |
| ZP103 | a text conflict merged on the box, the rebase taken to done, and the final check reading that name at keep | planned: box, by hand, in the worklog of picker-merge |
| ZP104 | every triple of merge-theory's exported battery gives the chunk sequence the battery states | covered: check_merge.c over tests/battery/ |
| ZP105 | from unchanged merges to onto | covered: check_merge.c over tests/battery/ |
| ZP106 | onto unchanged merges to from | covered: check_merge.c over tests/battery/ |
| ZP107 | swapping from and onto swaps the conflict halves and nothing else | covered: check_merge.c over tests/battery/ |
| ZP108 | partition: the chunks' base ranges cover base once, in order, with no gap and no overlap | covered: check_merge.c over tests/battery/, and the from and onto ranges with them |
| ZP109 | the counterexamples of Khanna, Kunal and Pierce come out as the battery states, and a disagreement of tools/merge-oracle.sh with diff3 -m or git merge-file is recorded rather than accepted in silence | covered: check_merge.c over tests/battery/ -- they are property-only cases and are asserted as such; the oracle was run and its disagreements are recorded in the worklog of diff3-walk |
| ZP110 | the standalone binary takes RESOLUTION BASE FROM ONTO RESULT, the same five, and exits 0, 1 and 2 as the entry does | covered: check_picker.c on a pty |
| ZP111 | too few or too many arguments: usage on stderr and exit 2, before the terminal is touched | covered: check_picker.c (zr_pk_open's refusal as a unit; the binary's own usage line on a pty, with not one escape byte written) |
| ZP112 | "" for a tree with no path is not a path of "" and is never opened | covered: check_picker.c |
| ZP113 | the standalone binary run with no tool around it, on a --posix fixture's resolution and manifest: the same screens and the same statuses | planned: box, by hand, in the worklog of picker-list |
| ZP114 | the tool's child and the standalone binary are the same objects: one key sequence through both gives one file | covered: check_picker.c on a pty, the binary and zr_picker_main in a forked child of the test |

## Positive-proof cells

One per subsection, the cell that proves the phase ran at all
rather than that it was silent -- the vacuous-fixture lesson the
V family learned:

  ZV1   every byte survives, so the codec is not a memcpy
  ZN12  two names reach one pool, so pooling is by ino
  ZW9   names outnumber pools, so the walk joins links
  ZC1   equal bytes still differ, so attributes are read
  ZD35  the battery has verdicts of every class, both modes
  ZM32  a manifest matches the note byte for byte
  ZA9   a write is seen through a second name, so it was in place
  ZX25  the self-check after the apply has nothing to say, so
        the apply was complete
  ZY14  a dup with the right bytes is still pending, so the
        classifier reads the pooling and not the bytes alone
  ZI13  the child received the resolution's path, so a process
        was really run on the document
  ZP53  the file came back changed at exactly the lines the keys
        touched, so the picker wrote and did not copy
