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
