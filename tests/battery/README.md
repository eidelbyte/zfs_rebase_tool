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
Only small batteries are stored; larger ones (green names 4, yellow
names 3) are generated on demand with --export DIR, never checked in.
