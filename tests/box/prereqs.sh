#!/bin/sh
# What the FreeBSD box needs before make freebsd and the box fixtures.
# Prints one line per check; changes nothing. Run as any user.
set -u
ok=0; bad=0
check() {
	if eval "$2" > /dev/null 2>&1; then
		echo "ok      $1"; ok=$((ok + 1))
	else
		echo "MISSING $1  ($3)"; bad=$((bad + 1))
	fi
}
check "cc"                 "command -v cc"                    "base system"
check "make"               "command -v make"                  "base system"
check "git"                "command -v git"                   "pkg install git"
check "perl (gate only)"   "command -v perl"                  "pkg install perl5, or run make gate on the Mac"
check "zfs and zpool"      "command -v zfs && command -v zpool" "base system"
check "mdconfig"           "command -v mdconfig"              "base system"
check "libzfs.h"           "test -r /usr/include/libzfs.h"    "point ZFS_INCLUDE at the src tree's openzfs include dir"
check "libzfs_core.h"      "test -r /usr/include/libzfs_core.h" "same"
check "libnvpair.h"        "test -r /usr/include/libnvpair.h" "same"
check "libzfs_core.so"     "ls /lib/libzfs_core.so* /usr/lib/libzfs_core.so* 2>/dev/null | grep -q ." "base system"
check "libzfs.so"          "ls /lib/libzfs.so* /usr/lib/libzfs.so* 2>/dev/null | grep -q ." "base system"
check "libnvpair.so"       "ls /lib/libnvpair.so* /usr/lib/libnvpair.so* 2>/dev/null | grep -q ." "base system"
check "copy_file_range"    "grep -q copy_file_range /usr/include/unistd.h" "FreeBSD 13 or later"
check "securelevel is 0 or less" "test \"\$(sysctl -n kern.securelevel)\" -le 0" "schg/sappnd cannot be cleared above 0; see the plan"
echo "prereqs: $ok ok, $bad missing"
[ $bad -eq 0 ]
