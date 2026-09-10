#!/bin/sh
# Cross syntax check of every source -- the plugins under src with
# them, the carried libdiff excepted, see the loop -- and of the box
# probe, as the
# freebsd target compiles them, on a machine that is not FreeBSD:
# clang targeting FreeBSD, with a FreeBSD source tree's headers
# standing in for /usr/include and the OpenZFS include set applied to
# zfsops.c and probe-mount.c alone, exactly as the Makefile does.
# Catches type and declaration errors in the blind FreeBSD sections
# before the box sees them; it is not a build.
# usage: FREEBSD_SRC=/path/to/freebsd tools/xcheck-freebsd.sh
set -u
cd "$(dirname "$0")/.." || exit 2
F=${FREEBSD_SRC:?set FREEBSD_SRC to a FreeBSD source tree}
T=$(mktemp -d "${TMPDIR:-/tmp}/zr-xinc.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
# what the tree installs into /usr/include by symlink or copy
ln -s "$F/sys/amd64/include" "$T/machine"
ln -s "$F/sys/x86/include" "$T/x86"
for h in stdint.h stdatomic.h stdbool.h; do
	[ -f "$F/sys/sys/$h" ] && ln -s "$F/sys/sys/$h" "$T/$h"
done
printf '#define __FreeBSD_version 1400097\n' > "$T/osreldate.h"
R=$(cc -print-resource-dir)/include
ZT=$F/sys/contrib/openzfs
SYS="-isystem $T -isystem $F/include -isystem $F/sys -isystem $F/sys/sys -isystem $R"
ZFS="-I$ZT/lib/libspl/include/os/freebsd -I$ZT/lib/libspl/include \
-I$ZT/include/os/freebsd -I$ZT/include \
-include $ZT/include/os/freebsd/spl/sys/ccompile.h \
-include $F/sys/modules/zfs/zfs_config.h \
-DNEED_SOLARIS_BOOLEAN -DHAVE_ISSETUGID -DHAVE_STRLCAT -DHAVE_STRLCPY"
# The picker's merge glue is the one source of ours that includes the
# carried libdiff, so it gets the same include paths the Makefile
# passes for build/merge.o: include/ for <arraylist.h> and
# <diff_main.h>, compat/include for the stdlib.h wrapper, and lib/ for
# diff_internal.h, where struct diff_chunk is defined. Grow this the
# way ZFS above is grown, if another picker source ever needs a path.
LDIR=src/plugins/picker/libdiff
LIBDIFF="-I$LDIR/include -I$LDIR/compat/include -I$LDIR/lib"
rc=0
for f in src/*.c src/plugins/*/*.c tools/probe-mount.c; do
	# The carried copy of FreeBSD's contrib/libdiff in
	# src/plugins/picker/libdiff is foreign code (plan section 3.6,
	# and that directory's UPSTREAM). It is exempt: it is not ours
	# to fix, it needs its own include paths and its own warning
	# exemptions, and it is FreeBSD's own code to begin with, so a
	# cross check of it against FreeBSD's headers would say nothing
	# this repository could act on. The box's make freebsd is what
	# confirms it builds there. The glob above stops one level
	# above it today; this keeps the exemption true if it is ever
	# widened.
	case "$f" in
	src/plugins/picker/libdiff/*) continue ;;
	esac
	extra=""
	case "$f" in
	src/zfsops.c|tools/probe-mount.c) extra="$ZFS" ;;
	src/plugins/picker/merge.c) extra="$LIBDIFF" ;;
	esac
	if cc -fsyntax-only -target x86_64-unknown-freebsd14.0 -nostdinc \
	    -std=c99 -Wall -Wextra -Wcast-qual -Werror -DZR_FREEBSD -Isrc \
	    $extra $SYS "$f" > "$T/log" 2>&1; then
		echo "ok   $f"
	else
		echo "FAIL $f"; grep -v "^In file included" "$T/log" | head -8; rc=1
	fi
done
[ $rc -eq 0 ] && echo "xcheck-freebsd: every source and the probe pass"
exit $rc
