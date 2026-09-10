# Exported decision batteries
Every case of the v4 green (pool) and yellow (pool + content) rules,
one line per case, written by v4-greencheck.py and v4-yellowcheck.py
in freebsd-development/zfs-rebase-theory.  The C engine's test gate
reads these files; those two checkers remain the specification.

Files are green-N-MODE.txt and yellow-N-K-MODE.txt: N names from A..,
K contents from x, y, z, MODE strict or permissive.  Two header lines,
then one line per case:

  base ({A},{B C}) from ({A B C}) onto ({A},{B C}) => ({A B C})
  base ({A}) from ({A B C}) onto ({B}) => - | conflict orphaned-add

A tree is its pools in parentheses, a pool is its names sorted inside
braces; in the yellow files every pool carries its content letter
({A B}x), and () is the empty tree.  The result is the merged tree, or
- followed by " | conflict CLASS[,CLASS...]" when the case conflicts.

Classes, in the order they are written: healed-split, orphaned-add,
contested-home, unexpressed-sharing (green); the green classes when
green conflicted, else changed-both or disagree (yellow).

Recheck a file with v4-greencheck.py --verify FILE (yellow likewise).

## What is checked in, and why

Stored here: green-3 and green-4, yellow-2-2 and yellow-3-2, both
modes each.  make check globs tests/battery/*.txt, so all eight run,
498,274 cases in under two seconds.

Until 2026-09-08 only green-3 and yellow-2-2 were stored, so the gate
ran three names and two contents and no more.  That was too little on
two counts.  v4-permissive-merge.md section 4 cites green-4's
strict-only conflicts and yellow-3-3's as checked, and every worked
row of its section 3 but one is out of reach at two names and two
contents: the pivot rows need five names and the adoption rows three
or four contents (the 2026-09-07 code review, R16).  And the review's
R5 was a decide bug living in shapes no stored battery reached.

What a battery cannot do, so that nobody expects it to: a line
records the set of classes that fired, not how many groups carry
them, so it reads the same whether one healed split fired or five.
R5 was exactly that, and it is a fixture that holds it down --
tests/fixtures/two-healed-splits.zrt and healed-split-two-groups.zrt.

## The larger sizes, exported when wanted

    python3 v4-yellowcheck.py --export tests/battery --names 3 --contents 3
    python3 v4-greencheck.py --export tests/battery --names 5

yellow-3-3 is 1,092,727 cases and 88 MB a file, over the 50 MB
GitHub warns above, so it is not committed.  The engine does pass it:
2,185,454 cases in both modes, 6 seconds, on 2026-09-08.  Exporting
it takes 48 seconds, against 4 or 5 for green-4 and yellow-3-2.

green-5 is 203 trees cubed, 8,365,427 cases a mode, and larger again;
nothing has run it yet (ZD37).

## The merge battery, a second format

tests/battery/merge/merge3.txt is the three-way text merge's battery,
written by v4-merge3check.py in freebsd-development/zfs-rebase-theory
and read by tests/check_picker.c (issue diff3-walk).  The theory it
holds up is v4-merge3.md beside that checker.

It sits in a subdirectory on purpose.  The green and yellow batteries
above are cases of the decide rule, one line each, and make check runs
check_battery over tests/battery/*.txt; a merge case is three files and
a chunk sequence and would be nonsense to that reader, so the glob does
not reach it.

The format is line oriented, ASCII, and meant to be read by a C loop
with strncmp and sscanf and nothing more.  Comment lines begin with #
and blank lines separate cases.  A case is

    case NAME exact|property
    base N
    L a
    L b
    from N
    ...
    onto N
    ...
    chunks N                 (an exact case only)
    KIND blo bhi flo fhi olo ohi
    ...
    end

base, from and onto each announce their line count and are followed by
exactly that many line records.  A line record is one letter, then a
space, then the line's text verbatim:

    L text      a line that ends with a newline
    X text      a line that does not, which is only ever a file's last
    L           an empty line ("L" alone, with no trailing space)

So the text begins at offset 2 and runs to the end of the record, and a
record of length 1 is an empty line.  A file of no lines is "base 0"
with no records after it.  The missing final newline is the letter at
the front of the record rather than an absence at the back, because it
is a fact the merge has to tell apart and trailing bytes do not survive
editors.

A chunk line is a kind and three half-open zero-based ranges, into
base, from and onto in that order.  The kinds are stable, from-only,
onto-only, both-same and conflict (v4-merge3.md section 1).  Any range
may be empty.  The base ranges of a case's chunks partition base in
order, and so do the from and onto ranges of their files, which is a
cheap thing for a reader to assert.

exact means the chunk sequence is pinned: a two-way diff of those two
files can only align them one way, so any difference is a failure.
property means it is not pinned -- the files hold repeated lines and
another correct diff could align them differently -- and only the
properties of v4-merge3.md section 7 are asserted.  Such a case has no
chunks section at all and "end" follows the onto lines.  The checker
refuses to mark a case exact whose alignment is not forced, so the flag
cannot drift.

Recheck the file with v4-merge3check.py --verify FILE, and rewrite it
with --export tests/battery/merge.  tools/merge-oracle.sh reads the
same file with awk and runs every case through git merge-file and,
on FreeBSD, diff3 -m; it is for reading and not a gate.
