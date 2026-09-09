#!/bin/sh
# The port, on the box: tracker issue port-test. FreeBSD, root, with
# a ports tree and the FreeBSD source tree. Usage:
#
#	run-port.sh [COMMIT]
#
# With COMMIT, the port is built from that commit's GitHub tarball
# (GH_TAGNAME), which is how the port is tested before a release is
# tagged; without it, from the tag DISTVERSION names, which is the
# real thing once the tag exists. Either way make makesum fetches
# the tarball and writes distinfo first, so the box needs the
# network for that one step.
#
# The steps, each stopping the script where it fails and leaving
# its output in the scratch directory: makesum, stage, check-plist,
# stage-qa, package, install, a smoke test of the installed binary
# and page, deinstall, and portlint -A where ports-mgmt/portlint is
# installed. At the end the port directory is cleaned and the
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
commit=${1:-}
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
	if (cd "$dest" && make SRC_BASE="$SRC_BASE" ${commit:+GH_TAGNAME="$commit"} "$@") \
	    > "$tmp/port-$name.log" 2>&1; then
		echo "ok   make $*"
	else
		cat "$tmp/port-$name.log"
		fail "make $* (see $tmp/port-$name.log)"
	fi
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

step makesum makesum
cat "$dest/distinfo"
step stage stage
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
page=$(man -w zfs_rebase 2>/dev/null)
[ -n "$page" ] || fail "man -w finds no zfs_rebase page"
echo "ok   sbin/zfs_rebase prints its usage; the page is $page"

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
echo "ok   run-port: every step passed; logs and distinfo in $tmp"
exit 0
