#!/bin/sh
# Refresh the carried copy of FreeBSD's contrib/libdiff that lives in
# src/plugins/picker/libdiff (implementation-plan.md section 3.6, and
# that directory's UPSTREAM file). The copy is foreign code and is
# never edited here, so a refresh is a plain copy of the same file
# list plus a rewrite of UPSTREAM; make check then says whether the
# merge's answers changed.
#
# usage: refresh-libdiff.sh [FREEBSD_TREE] [COMMIT]
#
#   FREEBSD_TREE  a FreeBSD src checkout. Default: $FREEBSD_SRC if it
#                 is set, else /usr/src.
#   COMMIT        the commit to take the files from. Default: the one
#                 the tree is at. The files come out of the object
#                 store with git show, not out of the work tree, so
#                 the tree does not have to be checked out at COMMIT
#                 and is left exactly as it was found.
#
# It refuses a tree with no contrib/libdiff, and a tree that is not a
# git checkout, since the commit is what the copy is pinned to.
set -u

self=refresh-libdiff.sh

usage() {
	echo "usage: $self [FREEBSD_TREE] [COMMIT]" >&2
	exit 2
}

die() {
	echo "$self: $*" >&2
	exit 1
}

if [ $# -gt 2 ]; then
	usage
fi

cd "$(dirname "$0")/.." || exit 2

dir=src/plugins/picker/libdiff

# The file list, one path relative to contrib/libdiff in the source
# and to $dir here: the four algorithm sources the merge needs, the
# headers they include, the licence, and the two compat allocators
# with the stdlib.h wrapper that declares them. The output side of
# libdiff (diff_output*.c, diff_output.h) is not carried: the merge
# walks the chunk list itself and prints nothing.
files="lib/diff_main.c
lib/diff_myers.c
lib/diff_patience.c
lib/diff_atomize_text.c
lib/diff_internal.h
lib/diff_debug.h
include/diff_main.h
include/arraylist.h
compat/reallocarray.c
compat/recallocarray.c
compat/include/stdlib.h
LICENCE"

tree=${1:-${FREEBSD_SRC:-/usr/src}}
[ -d "$tree" ] || die "no such tree: $tree"
[ -d "$tree/contrib/libdiff" ] ||
    die "$tree has no contrib/libdiff: give a FreeBSD src checkout"
git -C "$tree" rev-parse --git-dir > /dev/null 2>&1 ||
    die "$tree is not a git checkout: the copy is pinned to a commit"

want=${2:-HEAD}
commit=$(git -C "$tree" rev-parse "$want^{commit}" 2> /dev/null) ||
    die "no such commit in $tree: $want"
git -C "$tree" cat-file -e "$commit:contrib/libdiff/LICENCE" 2> /dev/null ||
    die "commit $commit has no contrib/libdiff"
date=$(git -C "$tree" show -s --format=%cd --date=short "$commit") ||
    die "cannot read the date of $commit"

for f in $files; do
	d=$(dirname "$dir/$f")
	mkdir -p "$d" || die "cannot make $d"
	git -C "$tree" show "$commit:contrib/libdiff/$f" > "$dir/$f" ||
	    die "cannot read contrib/libdiff/$f at $commit"
done

# UPSTREAM is generated here so that the copy and this script cannot
# drift apart: running the script twice against one commit must leave
# the directory untouched, which is the check that they agree.
{
	cat <<EOF
libdiff, carried from FreeBSD
=============================

Source	FreeBSD src, contrib/libdiff -- the Game of Trees project's
	diff library (Neels Hofmeyr), which FreeBSD builds as the
	internal lib/libdiff and installs neither the archive nor the
	headers of.
Commit	$commit
Date	$date
Licence	ISC. LICENCE below is upstream's; the two compat allocators
	carry the same terms in their own headers (Otto Moerbeek,
	OpenBSD).

Carried files, each byte for byte out of that commit:

EOF
	for f in $files; do
		printf '\t%-26s contrib/libdiff/%s\n' "$f" "$f"
	done
	cat <<'EOF'

The layout of the source is kept, because it is what makes the
includes work unchanged: the lib sources reach "diff_internal.h" and
"diff_debug.h" beside themselves, and <arraylist.h> and <diff_main.h>
through an include path. The two include paths the Makefile passes
are the two FreeBSD's own lib/libdiff/Makefile passes.

Not carried: diff_output.c, diff_output_plain.c, diff_output_unidiff.c,
diff_output_edscript.c and include/diff_output.h, which print diffs --
the merge walks the chunk list itself; compat/strlcpy.c, strlcat.c,
merge.c and getprogname_linux.c, which only the output side and the
sample program need; the test suite and the makefiles.

Rules
-----

Nothing in this directory is edited locally, ever. Not a byte, not a
trailing space. Every adaptation lives in the picker's own files, so
that a refresh stays a plain copy. A carried file that will not build
under the tool's warning set gets a flag on its object's line in the
Makefile, with a comment naming the file and the diagnostic, and the
file itself is left alone.

Every carried file is ASCII, so the directory stays inside the ASCII
check that tools/gate.sh runs over the tree. It is exempt from cstyle
and from tools/xcheck-freebsd.sh: foreign code keeps its own style,
and the FreeBSD build of it is the box's to confirm.

Refresh
-------

	sh tools/refresh-libdiff.sh [FREEBSD_TREE] [COMMIT]

with FREEBSD_TREE defaulting to $FREEBSD_SRC or /usr/src and COMMIT
to the commit that tree is at. Then make check: the battery is what
says whether the merge's answers moved. One commit for the refresh.
EOF
} > "$dir/UPSTREAM" || die "cannot write $dir/UPSTREAM"

echo "$self: $dir is at $commit ($date)"
if git rev-parse --git-dir > /dev/null 2>&1; then
	changed=$(git status --short -- "$dir")
	if [ -n "$changed" ]; then
		echo "$changed"
	else
		echo "$self: nothing changed"
	fi
else
	echo "$self: not a git checkout here, no change list"
fi
exit 0
