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
  place snapshots, holds, clones and zfs diff exist.

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
sprints/sprint-4/zfs-rebase-sprint-4.json.

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
| ZV19 | encode(decode(s)) == s on fixture names | planned: check_roundtrip.c |

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
binary}; ACL {absent, present}; symlink target; rdev {0, large};
completeness against st_nlink; faults.

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
| ZW18 | an ACL present | deferred: the two ACL models differ; box-probe |
| ZW19 | no ACL: absent, not an empty blob | planned: check_walk.c |
| ZW20 | mode, uid, gid, flags, size, times read | planned: check_walk.c |
| ZW21 | completeness: st_nlink over names found | planned: check_walk.c |
| ZW22 | completeness: st_nlink under names found | planned: check_walk.c |
| ZW23 | a dangling symlink walks as a symlink | planned: check_walk.c |
| ZW24 | an unreadable directory errors, not silence | planned: check_walk.c |
| ZW25 | the root is the pool named "/" | planned: check_walk.c |
| ZW26 | an empty root | planned: check_walk.c |
| ZW27 | rdev 0 and a large rdev both survive | planned: check_walk.c |

## ZC -- content oracle (check_yellow.c)

Dimensions: compared aspect {type, mode, uid, gid, flags, size,
bytes, xattrs, ACL, symlink target, rdev, times}; kind {file,
dir, symlink, device}; verdict {equal, differ}; handle source
{base, via the diff fast path, by comparison}; handle scope {one
face local group}.

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
| ZC9 | an ACL differs | deferred: ACL models differ; box-probe |
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
| ZC20 | unchanged from base: base's handle, no read | planned: check_diff.c |
| ZC21 | --posix reads every byte, same verdicts | planned: run-fixtures.sh |
| ZC22 | handles are scoped to one face local group | planned: check_yellow.c |
| ZC23 | transitivity: a=b, b=c, one handle | planned: check_yellow.c |
| ZC24 | a read error is an error, never "equal" | planned: check_yellow.c |

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
| ZM33 | parse rebuilds paths from the scoping | planned: check_manifest.c |
| ZM34 | parse rejects an unknown action | planned: check_manifest.c |
| ZM35 | parse rejects a bad escape | planned: check_manifest.c |
| ZM36 | parse rejects unbalanced scoping | planned: check_manifest.c |
| ZM37 | parse rejects a missing root line | planned: check_manifest.c |
| ZM38 | parse rejects an ln before its anchor | planned: check_manifest.c |
| ZM39 | parse ignores comments and blank lines | planned: check_manifest.c |
| ZM40 | emit, parse, emit is byte identical | planned: check_roundtrip.c |
| ZM41 | the expect block parses equal | planned: check_roundtrip.c |
| ZM42 | escapes.zrt, dir-rm.zrt, wide-pool.zrt | planned: check_roundtrip.c |

## ZA -- apply (check_apply.c)

Dimensions: action {cp, write, ln, rm, conflict}; cp type {file,
dir, symlink, chr, blk, fifo, sock}; attribute order {chown,
chmod, xattrs, ACL, times, flags last}; rm timing {leaf, at
directory close}; ln {new name, replacing a name, bad
destination}; write {in place, through every name}; the re-stat
check; the copy path {copy_file_range, read/write}.

| cell | scenario | disposition |
|------|----------|-------------|
| ZA1 | cp of a regular file: bytes and type | planned: check_apply.c |
| ZA2 | cp of a directory: empty, children after | planned: check_apply.c |
| ZA3 | cp of a symlink: the target, not the file | planned: check_apply.c |
| ZA4 | cp of a character device: mknod, rdev | planned: check_apply.c |
| ZA5 | cp of a block device | planned: check_apply.c |
| ZA6 | cp of a fifo | planned: check_apply.c |
| ZA7 | cp of a socket | deferred: no portable socket create; box-probe |
| ZA8 | write in place: same object, new bytes | planned: check_apply.c |
| ZA9 | write is seen through every name | planned: check_apply.c |
| ZA10 | write preserves st_ino and st_nlink | planned: check_apply.c |
| ZA11 | ln: a second name on the anchor | planned: check_apply.c |
| ZA12 | ln replacing an existing name | planned: check_apply.c |
| ZA13 | ln where the destination is a directory | planned: check_apply.c |
| ZA14 | rm of a leaf | planned: check_apply.c |
| ZA15 | rm of a directory at its close | planned: check_apply.c |
| ZA16 | rm of a directory with a child left: loud | planned: check_apply.c |
| ZA17 | conflict does nothing to the name | planned: check_apply.c |
| ZA18 | order: chown before chmod, setuid survives | planned: check_apply.c |
| ZA19 | order: xattrs and ACL before times | planned: check_apply.c |
| ZA20 | order: times before flags | planned: check_apply.c |
| ZA21 | flags last: an immutable copied file | planned: check_apply.c |
| ZA22 | every action re-stats its target | planned: check_apply.c |
| ZA23 | a re-stat mismatch fails the apply | planned: check_apply.c |
| ZA24 | copy_file_range absent: the fallback | planned: check_apply.c |
| ZA25 | openat-relative: no path leaves the root | planned: check_apply.c |
| ZA26 | an action naming a path outside the root | planned: check_apply.c |
| ZA27 | actions run in manifest order | planned: check_apply.c |
| ZA28 | xattrs applied, the user namespace | planned: check_apply.c |
| ZA29 | an ACL applied | deferred: the two ACL models differ; box-probe |
| ZA30 | the whole probe manifest applies | planned: run-fixtures.sh |

## ZX -- the ZFS layer (diff parse, zfs ops, driver, guards)

Dimensions: diff line {M, -, +, R}; link delta; zfs diff's own
escaping; derived sets {unchanged, renamed dirs, modified dirs};
zfs ops {snapshot, hold, clone, mount, prop flip, release,
destroy}; guards {securelevel, private mountpoint, readonly flip,
re-walk}; driver {flags, preconditions, exit status}. The parser
rows run on the Mac over a captured file; everything from ZX13 on
is box only.

| cell | scenario | disposition |
|------|----------|-------------|
| ZX1 | an M line: a changed object, one path | planned: check_diff.c |
| ZX2 | a "-" line: a removed name | planned: check_diff.c |
| ZX3 | a "+" line: an added name | planned: check_diff.c |
| ZX4 | an R line: the renamed pair | planned: check_diff.c |
| ZX5 | the (+N)/(-N) link delta on an M line | planned: check_diff.c |
| ZX6 | zfs diff's own escaping, a torture name | planned: check_diff.c |
| ZX7 | unreported paths inherit base's handle | planned: check_diff.c |
| ZX8 | under a renamed dir is not unchanged | planned: check_diff.c |
| ZX9 | the modified-directory list is produced | planned: check_diff.c |
| ZX10 | a malformed diff line errors | planned: check_diff.c |
| ZX11 | the parser over tests/data/probe.diff | planned: check_diff.c |
| ZX12 | the escaping rules confirmed live | planned: box, box/run-fixture.sh |
| ZX13 | snapshot from and onto @rebase-ID | planned: box, box/run-fixture.sh |
| ZX14 | holds on all three snapshots | planned: box, box/run-fixture.sh |
| ZX15 | cleanup fd drops holds on death | planned: box, box/run-fixture.sh |
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
| ZX31 | -n: manifest only, no clone | planned: box, box/run-fixture.sh |
| ZX32 | -o FILE against stdout | planned: box, box/run-fixture.sh |
| ZX33 | -p sets the mode and the header | planned: box, box/run-fixture.sh |

Note on ZX23: securelevel cannot be raised without a reboot of the
box, so the refusal path stays deferred. Unblocking work is a
box-probe run started at securelevel 1, which is also the only way
to prove the schg/sappnd name listing.

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
