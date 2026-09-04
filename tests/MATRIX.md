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
  expect block (tests/run-fixtures.sh).
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
pool, none}; and what the change was {bytes through a name, a
name added, an object added, an entry added to a directory}.

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
exhaustive row, and they are what M1 means. The Python checkers
already prove these properties by enumeration -- the battery is
how that proof transfers to C, not a second opinion.

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
| ZD35 | green battery, 3 and 4 names, both modes | planned: check_battery.c |
| ZD36 | yellow battery 2n2c 3n2c 3n3c, both modes | planned: check_battery.c |
| ZD37 | green battery, 5 names | planned: check_battery.c, make battery-full |

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
| ZM29 | header: version, datasets, mode | planned: check_manifest.c |
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
| ZM43 | parse rejects a first line that is not v4 | covered: check_manifest.c |
| ZM44 | parse rejects an ln naming its own path | covered: check_manifest.c |
| ZM45 | parse rejects a child under a leaf line | covered: check_manifest.c |
| ZM46 | parse rejects a leaf with no slash, no action | covered: check_manifest.c |
| ZM47 | parse rejects #actions that miscounts | covered: check_manifest.c |
| ZM48 | parse rejects a conflict mark with no record | covered: check_manifest.c |
| ZM49 | parse rejects an unknown class | covered: check_manifest.c |
| ZM50 | parse rejects records out of order | covered: check_manifest.c |
| ZM51 | a type change: rm of onto's directory, then cp | covered: check_roundtrip.c |
| ZM52 | dup: a severed half copies onto's own bytes | covered: check_apply.c, h-th-op1-edit-vs-split.zrt |

## ZA -- apply (check_apply.c)

Dimensions: action {cp, write, ln, rm, conflict}; cp type {file,
dir, symlink, chr, blk, fifo, sock}; attribute order {chown,
chmod, xattrs, ACL, times, flags last}; rm timing {leaf, at
directory close}; ln {new name, replacing a name, bad
destination}; write {in place, through every name}; the re-stat
check; the copy path {copy_file_range, read/write}.

| cell | scenario | disposition |
|------|----------|-------------|
| ZA1 | cp of a regular file: bytes and type | covered: check_apply.c |
| ZA2 | cp of a directory: empty, children after | covered: check_apply.c |
| ZA3 | cp of a symlink: the target, not the file | covered: check_apply.c |
| ZA4 | cp of a character device: mknod, rdev | deferred: mknod needs root; box-probe |
| ZA5 | cp of a block device | deferred: mknod needs root; box-probe |
| ZA6 | cp of a fifo | covered: check_apply.c |
| ZA7 | cp of a socket | deferred: no portable socket create; box-probe |
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

## ZX -- the ZFS layer (zfs ops, driver, guards)

Dimensions: the unchanged set {pruned, not pruned, not attempted};
zfs ops {hold, clone, mount, prop flip, release, destroy}; the
record {every property, guids, local against inherited, the states};
the gates {applying1, conflicts, applying2, done, and what a stop
leaves}; the input forms {from a snapshot or a dataset, onto a
snapshot or a dataset, --result as a clone name or as a snapshot
name, short and full}; the dataset form's own {the unmount, the
private mount, the readonly flips, the hand-back, the rollback};
guards {securelevel, private mountpoint, readonly flip, re-walk};
driver {flags, preconditions, exit status}. Every row here is box
only.

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
| ZX16 | clone readonly=on, private mount | planned: box, box/run-fixture.sh |
| ZX17 | the clone mounts under that root | planned: box, box/run-fixture.sh |
| ZX18 | readonly off to apply, on after | planned: box, box/run-fixture.sh |
| ZX19 | the failure path destroys it | planned: box, box/run-fixture.sh |
| ZX20 | preconditions: mounted, one pool | planned: box, box/run-fixture.sh |
| ZX21 | name semantics on all three | planned: box, box/run-fixture.sh |
| ZX22 | refusal when not run as root | planned: box, box/run-fixture.sh |
| ZX23 | securelevel refusal | deferred: a reboot; see the note |
| ZX24 | a foreign mountpoint is refused | planned: box, box/run-fixture.sh |
| ZX25 | the re-walk equals the decision | planned: box, box/run-fixture.sh |
| ZX26 | a stray write caught by re-walk | planned: box, box/run-fixture.sh |
| ZX27 | exit 0: clean and applied | planned: box, box/run-fixture.sh |
| ZX28 | exit 1: conflicts, clone left | planned: box, box/run-fixture.sh |
| ZX29 | exit 2: precondition failure | planned: box, box/run-fixture.sh |
| ZX30 | exit 3: internal | planned: box, box/run-fixture.sh |
| ZX31 | -n: manifest only, no clone, no hold | planned: box, box/run-fixture.sh |
| ZX32 | -o FILE against stdout | planned: box, box/run-fixture.sh |
| ZX33 | -p sets the mode and the header | planned: box, box/run-fixture.sh |
| ZX34 | the record is there from the create, every property | planned: box, box/run-fixture.sh |
| ZX35 | the three guids equal zfs get guid on the snapshots | planned: box, box/run-fixture.sh |
| ZX36 | every record property has source local | planned: box, box/run-fixture.sh |
| ZX37 | an inherited record is none: --abort exits 2, touches nothing | planned: box, box/run-fixture.sh |
| ZX38 | the states: applying1, conflicts, done, and none at birth | planned: box, box/run-fixture.sh |
| ZX39 | --abort releases the holds, and runs again after a half abort | planned: box, box/run-fixture.sh |
| ZX40 | --verify recorded; -n --verify still creates nothing | planned: box, box/run-fixture.sh |
| ZX41 | applying1 applies the clean actions before the conflicts gate | planned: box, box/run-fixture.sh |
| ZX42 | a conflicted run: state conflicts, the holds kept, the clean actions in the tree | planned: box, box/run-fixture.sh |
| ZX43 | stage 1 idempotence: a second rebase declares 0 actions and the same conflicts | planned: box, box/run-fixture.sh |
| ZX44 | a directory rm blocked by a conflicted child survives applying1, and the re-walk passes over it | planned: box, box/run-fixture.sh |
| ZX45 | the hand-off names <rundir>/resolution at the conflicts gate | planned: box, box/run-fixture.sh |
| ZX46 | the run directory is /var/db/zfs_rebase/<result>, mount point and manifest under it | planned: box, box/run-fixture.sh |
| ZX47 | a manifest at that path survives a reboot, which /var/run would not | deferred: a reboot of the box; run by hand with a conflicted fixture |
| ZX48 | --continue at applying1: the manifest applied again, then done or conflicts | planned: box, box/run-fixture.sh |
| ZX49 | --continue at conflicts with no resolution: exit 1, the state unmoved, the path named | planned: box, box/run-fixture.sh |
| ZX50 | --continue at conflicts with a resolution: applying2 and then done | deferred: nothing writes a resolution until the conflict manager; by hand with a written-out file |
| ZX51 | --continue at done: the holds released, and with --verify the repair | planned: box, box/run-fixture.sh |
| ZX52 | --verify alone: exit 0 over a clean result and over a conflicted one | planned: box, box/run-fixture.sh |
| ZX53 | --verify over a stray edit: exit 3, the drifted action named, nothing written | planned: box, box/run-fixture.sh |
| ZX54 | --continue --verify repairs that edit; --verify is clean after it | planned: box, box/run-fixture.sh |
| ZX55 | --restart: destroyed, cloned again, same record and tag, same gate, same tree | planned: box, box/run-fixture.sh |
| ZX56 | a recorded snapshot that exists with another guid: every verb exits 2 | deferred: needs a destroy and a re-snapshot under the name; box, kill-tests |
| ZX57 | a recorded snapshot gone: exit 2 for --continue and --restart, found by guid for --verify | deferred: needs a destroyed input, which the holds prevent until done; box, stray-tests |
| ZX58 | the report's temporary hold is there while it runs and gone after, under its own tag | deferred: needs the pause hook to look during the run; box, pause-hook |
| ZX59 | --result given as pool/fs@snap finds the same rebase as pool/fs | planned: box, box/run-fixture.sh |
| ZX60 | a result left unmounted (a reboot) is mounted again by a verb | deferred: a reboot, or zfs unmount by hand; box |
| ZX61 | a dataset carrying no record: every verb exits 2 and touches nothing | planned: box, box/run-fixture.sh |
| ZX62 | --verify on a fresh run: the final check at the done gate, before the release | planned: box, box/run-fixture.sh |
| ZX63 | a tool-made from snapshot goes at done and at --abort, and never at --restart | planned: box, box/run-fixture.sh |
| ZX64 | -n with a dataset side takes a snapshot, reads it, destroys it and holds nothing | planned: box, box/run-fixture.sh |
| ZX65 | the dataset form's --result: the short name and the full name are the same snapshot; a full name of another dataset exits 2 | planned: box, box/run-fixture.sh |
| ZX66 | the dataset form's record lives on onto: form=dataset, readonly recorded, every property local | planned: box, box/run-fixture.sh |
| ZX67 | the pre-apply snapshot already exists: exit 2, since the user chose the name | planned: box, box/run-fixture.sh |
| ZX68 | exclusivity: onto is unmounted from its own place and mounted at <rundir>/mnt, with the mountpoint property untouched | planned: box, box/run-fixture.sh |
| ZX69 | a file held open under onto: the unmount refuses, exit 2, nothing touched | planned: box, box/run-fixture.sh |
| ZX70 | readonly on outside the apply, off during it, and the recorded value back at the hand-back | planned: box, box/run-fixture.sh |
| ZX71 | the hand-back: at conflicts, at done and after a failure the dataset is mounted at home again | planned: box, box/run-fixture.sh |
| ZX72 | a kill in the dataset form leaves it privately mounted, and the next verb takes it from there | deferred: needs the pause hook; box, kill-tests |
| ZX73 | --restart in the dataset form: rolled back to the pre-apply snapshot, applied again, same gate and tag | planned: box, box/run-fixture.sh |
| ZX74 | --abort in the dataset form: rolled back, the pre-apply snapshot destroyed, no zfs_rebase: property left local, mounted at home | planned: box, box/run-fixture.sh |
| ZX75 | --verify alone in the dataset form: the live tree walked privately and handed back, nothing written | planned: box, box/run-fixture.sh |
| ZX76 | --overwrite: a done record replaced; without it exit 2; an open record exits 2 either way | planned: box, box/run-fixture.sh |
| ZX77 | a base that is a snapshot of onto is read through the private mount | deferred: needs a from cloned out of onto, which the fixtures do not build; by hand on the box |
| ZX78 | a snapshot newer than the pre-apply one: --restart and --abort refuse rather than destroy it | deferred: needs a snapshot taken during a rebase; by hand on the box |
| ZX79 | --allow-unrelated takes a pair that shares no origin, and the derivation is not run | planned: box, box/run-fixture.sh step 0a |
| ZX80 | no --base: the base is the empty tree, and the record and the manifest header carry "-" and the guid 0 | planned: box, box/run-fixture.sh step 0a |
| ZX81 | --base given: the snapshot is walked like the other two, and its dataset is checked like theirs | planned: box, box/run-fixture.sh step 0a |
| ZX82 | --base newer than a side by createtxg: exit 2; and --base without the flag is a usage error | planned: box, box/run-fixture.sh step 0a |
| ZX83 | every verb over a record with no base: the base is not looked for and not held, and the report says there was none | deferred: needs a real result of an unrelated run, which step 0a only dry-runs; box, by hand |

ZX79 to ZX83 are --allow-unrelated's own. The empty base cannot be
reached on the Mac: the only other way into a decision here is
--posix, which takes three directories and walks all three, so a
tree with no root at all is not something a portable test can make
without a driver of its own. The box closes them, and ZX5 with them.

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

Note on ZX23: securelevel cannot be raised without a reboot of the
box, so the refusal path stays deferred. Unblocking work is a
box-probe run started at securelevel 1, which is also the only way
to prove the schg/sappnd name listing.

## ZF -- the fixture format (check_fixture.c, run-fixtures.sh)

Not an engine phase but the input every other family's end to end
level reads: one .zrt file, parsed, built as three directory trees
and built again as pools in memory, where the two builders must
agree. Dimensions: syntax element {tree lines, the four types, the
escapes, mode, uid, gid, flags, xattr, acl, the platform line,
expect}; what it acts on {file, dir, symlink, a link line, the pool
two names share}; builder {directories, pools, one directory edited
into another}; the edit's decision {removed, created, relinked,
rewritten, attrs, untouched}; rejection {every rule the format
names}; platform {portable, box only}.

Rows for the attributes were added with the attributes themselves
(issue fixture-attrs). A row that only FreeBSD can reach names the
box-only fixture that closes it (issue attr-cells wrote them) and
stays planned until the box has run it, which is the disposition
the top of this file gives a row whose only reachable level is
FreeBSD.

| cell | scenario | disposition |
|------|----------|-------------|
| ZF1 | the four types, tokens, links and escapes parse | covered: check_fixture.c |
| ZF2 | mode, uid and gid on a file, a dir and a symlink | covered: check_fixture.c |
| ZF3 | a build into a directory that is not empty is refused | covered: check_fixture.c |
| ZF4 | to_tree: pools, synthetic inos, nlink, handles | covered: check_fixture.c |
| ZF5 | the expect block, and a fixture without one | covered: check_fixture.c |
| ZF6 | the rejections the format named before attributes | covered: check_fixture.c |
| ZF7 | flags=NAMES parses to the number lchflags takes | covered: check_fixture.c |
| ZF8 | a flag name chflags(1) does not know is rejected | covered: check_fixture.c |
| ZF9 | flags on a file, walked back as za_flags | covered: check_fixture.c |
| ZF10 | uchg on a directory: set after its children exist | covered: check_fixture.c |
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
| ZF44 | flags: one changed alone; uchg off a directory to empty it and on again; an immutable file edited | covered: check_fixture.c |
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
| ZF60 | flags-conflict.zrt: uchg on from and nodump on onto -> changed-both | planned (box: run-suite.sh) |

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
gone}; the information line {edited, added, covered by a conflict,
still onto's, another name of an object an action made}; the apply's
input {no report, a report}; repetition {a first run, a second run
over what the first left}.

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
| ZY21 | a name under a conflicted directory: never an info line | covered: check_verify.c |
| ZY22 | info: an untouched name edited | covered: check_verify.c |
| ZY23 | info: a name onto never had | covered: check_verify.c |
| ZY24 | no info: an untouched name that still matches | covered: check_verify.c |
| ZY25 | no info: another name of an object an action made | covered: check_verify.c |
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
| ZY38 | a kill at a gate, then --verify and --continue --verify | deferred: needs the stages and the pause hook; box, stages and kill-tests |
| ZY39 | a stray edit and a stray delete in a real result, both forms | deferred: needs snapshots and a clone; box, stray-tests and box-forms |
| ZY40 | cp with the from tree gone: unchecked | covered: check_verify.c |
| ZY41 | write with the from tree gone: unchecked | covered: check_verify.c |
| ZY42 | rm with onto gone: unchecked while the name is there, done once it is not | covered: check_verify.c |
| ZY43 | dup with onto gone: unchecked | covered: check_verify.c |
| ZY44 | ln standing on its anchor, and a cp the result already holds: done with onto gone | covered: check_verify.c |
| ZY45 | no information lines at all with onto gone | covered: check_verify.c |

ZY40 to ZY45 are the post-done verify's: a tree that is not there
any more is walked as the empty tree and named in the missing mask,
and every action that would have had to read it is unchecked. The
box rows for a really destroyed snapshot are ZX56 and ZX57.

ZY19's drift is reported and not repaired: a re-write mends the
bytes of the name it is on and cannot rejoin a pool somebody tore,
so --continue --verify will make the write true again and verify
will still say drifted. That is the honest answer and the reason
the information lines and this cell exist at all -- to be read, not
to be cleared.

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
  ZX25  the re-walk equals the decision, so apply was complete
  ZY14  a dup with the right bytes is still pending, so the
        classifier reads the pooling and not the bytes alone
