#!/bin/sh
# The port, on the box: tracker issues port-test and
# port-picker-option. FreeBSD, root, with a ports tree and the
# FreeBSD source tree. Usage:
#
#	run-port.sh [--without-picker] [COMMIT]
#
# --without-picker builds the same port with its PICKER option off,
# which is the second pass and a run of its own: OPTIONS_UNSET=PICKER
# goes to every make, and the stage is then asserted to hold no
# sbin/zfs_rebase-picker and a zfs_rebase whose ldd names no curses
# library, with the installed tool's usage still naming
# --interactive. What -i does at a conflicts gate is not asserted
# here -- a port test makes no pool and reaches no gate -- so the
# usage line is what says the flag is still there; the tool's own
# tests are what say the child exits 2 with the stub's line.
#
# With COMMIT, the port is built from that commit's GitHub tarball
# (GH_TAGNAME), which is how the port is tested before a release is
# tagged; without it, from the tag DISTVERSION names, which is the
# real thing once the tag exists. Either way make makesum fetches
# the tarball and writes distinfo first, so the box needs the
# network for that one step.
#
# The steps, each stopping the script where it fails and leaving
# its output in the scratch directory: the options read back from the
# port, makesum, stage, what the option left in the stage,
# check-plist, stage-qa, package, install, a smoke test of the
# installed binary and page, deinstall, and portlint -A where
# ports-mgmt/portlint is installed. At the end the port directory is cleaned and the
# distinfo it wrote is printed, since that is what goes back into
# ports/sysutils/zfs_rebase/distinfo after the tag.
#
# PORTSDIR (default /usr/ports) must hold a ports tree -- git clone
# --depth 1 https://git.FreeBSD.org/ports.git /usr/ports makes one --
# and SRC_BASE (default /usr/src) the source tree with
# sys/contrib/openzfs in it, which the port compiles against. The
# port directory under PORTSDIR is ours and is replaced by this
# script's copy every run. KEEP=1 skips the final make clean.
set -u
cd "$(dirname "$0")/../.." || exit 2
commit=""
picker=yes
optargs=""
while [ $# -gt 0 ]; do
	case "$1" in
	--without-picker) picker=no; optargs="OPTIONS_UNSET=PICKER" ;;
	-*) echo "usage: run-port.sh [--without-picker] [COMMIT]"; exit 2 ;;
	*) commit=$1 ;;
	esac
	shift
done
PORTSDIR=${PORTSDIR:-/usr/ports}
SRC_BASE=${SRC_BASE:-/usr/src}
[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 2; }
[ "$(uname)" = FreeBSD ] || { echo "FreeBSD only"; exit 2; }
[ -f "$PORTSDIR/Mk/bsd.port.mk" ] || \
    { echo "no ports tree at $PORTSDIR: git clone --depth 1 https://git.FreeBSD.org/ports.git $PORTSDIR, or set PORTSDIR"; exit 2; }
[ -f "$SRC_BASE/sys/contrib/openzfs/include/libzfs.h" ] || \
    { echo "no source tree at $SRC_BASE: install src.txz there, or set SRC_BASE"; exit 2; }
[ -d ports/sysutils/zfs_rebase ] || { echo "no ports/sysutils/zfs_rebase here"; exit 2; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-port.XXXXXX") || exit 2
dest=$PORTSDIR/sysutils/zfs_rebase
say() { printf '\n== %s\n' "$*"; }
fail() { echo "FAIL: $*"; echo "logs: $tmp"; exit 1; }
# One make step in the port directory, its output kept.
step() {	# NAME [MAKE ARGS...]
	name=$1
	shift
	say "$name"
	if (cd "$dest" && make SRC_BASE="$SRC_BASE" ${commit:+GH_TAGNAME="$commit"} \
	    $optargs "$@") \
	    > "$tmp/port-$name.log" 2>&1; then
		echo "ok   make $*"
	else
		cat "$tmp/port-$name.log"
		fail "make $* (see $tmp/port-$name.log)"
	fi
}

# One make -V in the port directory, with the run's own options on it.
value() {	# VARIABLE
	(cd "$dest" && make SRC_BASE="$SRC_BASE" ${commit:+GH_TAGNAME="$commit"} \
	    $optargs -V "$1")
}

say "the port directory"
if [ -d "$dest" ]; then
	rm -rf "$dest" || fail "cannot replace $dest"
fi
cp -R ports/sysutils/zfs_rebase "$dest" || fail "cannot copy the port to $dest"
if [ -n "$commit" ]; then
	echo "from commit $commit (GH_TAGNAME)"
else
	echo "from the tag DISTVERSION names:"
	grep -n '^DISTVERSION' "$dest/Makefile"
fi
if pkg info zfs_rebase > /dev/null 2>&1; then
	pkg delete -y zfs_rebase > "$tmp/pkg-delete.log" 2>&1 || \
	    fail "an installed zfs_rebase would not deinstall"
	echo "     (an installed zfs_rebase was removed first)"
fi

# The options this run builds with, before anything is built with
# them. A saved options file -- what make config writes under
# /var/db/ports -- is read after OPTIONS_UNSET and would quietly
# override it, so the answer is read back rather than assumed, and
# make rmconfig is the fix the failure names.
say "the options"
opts=$(value PORT_OPTIONS) || fail "make -V PORT_OPTIONS"
echo "PORT_OPTIONS: $opts"
case " $opts " in
*" PICKER "*) got=yes ;;
*) got=no ;;
esac
[ "$got" = "$picker" ] || \
    fail "this run wants PICKER=$picker and the port says $got: a saved options file may be overriding it (cd $dest && make rmconfig)"

step makesum makesum
cat "$dest/distinfo"
step stage stage

# What the option did to the stage. ldd is the question that matters
# for the option's whole purpose -- a package that does not pull in
# ncurses -- and the staged binary is the one to ask, since it is the
# file the package will carry.
say "the stage, with PICKER $picker"
stagedir=$(value STAGEDIR) || fail "make -V STAGEDIR"
prefix=$(value PREFIX) || fail "make -V PREFIX"
tool=$stagedir$prefix/sbin/zfs_rebase
pickerbin=$stagedir$prefix/sbin/zfs_rebase-picker
[ -f "$tool" ] || fail "no $tool staged"
if [ "$picker" = yes ]; then
	[ -f "$pickerbin" ] || fail "PICKER is on and no $pickerbin was staged"
	ldd "$tool" | grep -i curses || \
	    fail "PICKER is on and ldd of the staged tool names no curses library"
	echo "ok   the picker binary is staged and the tool links curses"
else
	[ ! -e "$pickerbin" ] || fail "PICKER is off and $pickerbin was staged"
	if ldd "$tool" | grep -i curses; then
		fail "PICKER is off and ldd of the staged tool names the curses library above"
	fi
	echo "ok   no picker binary staged, and the staged tool names no curses library"
fi
step check-plist check-plist
cat "$tmp/port-check-plist.log"
step stage-qa stage-qa
cat "$tmp/port-stage-qa.log"
step package package
step install install

say "the installed tool"
/usr/local/sbin/zfs_rebase > "$tmp/smoke.log" 2>&1
st=$?
[ $st -eq 2 ] || { cat "$tmp/smoke.log"; fail "zfs_rebase with no arguments exited $st, want 2 (usage)"; }
head -2 "$tmp/smoke.log"
grep -q -- '--interactive' "$tmp/smoke.log" || \
    fail "the installed tool's usage does not name --interactive"
page=$(man -w zfs_rebase 2>/dev/null)
[ -n "$page" ] || fail "man -w finds no zfs_rebase page"
echo "ok   sbin/zfs_rebase prints its usage, -i in it; the page is $page"

step deinstall deinstall
if pkg info zfs_rebase > /dev/null 2>&1; then
	fail "zfs_rebase is still installed after deinstall"
fi

say "portlint"
if command -v portlint > /dev/null 2>&1; then
	(cd "$dest" && portlint -A) > "$tmp/portlint.log" 2>&1
	st=$?
	cat "$tmp/portlint.log"
	[ $st -eq 0 ] || echo "note portlint exited $st: read the lines above"
else
	echo "note portlint is not installed (pkg install portlint); skipped"
fi

say "distinfo, to carry back into ports/sysutils/zfs_rebase/distinfo"
cat "$dest/distinfo"
cp "$dest/distinfo" "$tmp/distinfo"
if [ "${KEEP:-0}" != 1 ]; then
	(cd "$dest" && make SRC_BASE="$SRC_BASE" ${commit:+GH_TAGNAME="$commit"} clean) \
	    > "$tmp/port-clean.log" 2>&1
fi
echo "ok   run-port: every step passed with PICKER $picker; logs and distinfo in $tmp"
exit 0
