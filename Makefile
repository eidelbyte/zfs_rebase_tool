# zfs_rebase: portable core by default, freebsd target adds the ZFS
# layer. Plain POSIX make; no GNU-only functions, so bmake and gmake
# both work. Object lists are explicit on purpose.

CC ?= cc
# -DNDEBUG is in the flags that ship, in both flavors (the review of
# 2026-09-11, S2). assert(3) is live in two places here: the carried
# libdiff and the adapted diff3 walk. Every one of those asserts is on
# the picker's Enter path, where SIGABRT takes the terminal down with
# it -- raw, and on the alternate screen -- and the walk has a
# graceful refusal written two lines below one of them, which the
# abort reaches first. The macro is in the base CFLAGS rather than in
# the two flavor targets so that no build of ours can miss it, the
# test programs included: no test of ours calls assert(3), so nothing
# here loses an assertion by it. The walk's own assert above that
# refusal is turned into the refusal beside this change, in the
# picker's half of the same review.
CFLAGS = -std=c99 -Wall -Wextra -Werror -Wcast-qual -O2 -g -DNDEBUG -Isrc
LDFLAGS =

# The standalone picker's link flags, which are the tool's minus the
# ZFS layer: ground rule 1 says the helper links no libzfs, on the mac
# and in a build jail alike, and one LDFLAGS override used to reach
# both link rules and put three DT_NEEDED entries on it that no object
# of it references (the review of 2026-09-11, B9). The default is
# LDFLAGS, so a hand's "make LDFLAGS=..." still reaches both binaries;
# the freebsd targets pass this one through unchanged and add the ZFS
# libraries to LDFLAGS alone. It cannot be left to inherit: a make
# macro is expanded where it is used, so an override of LDFLAGS in the
# recursion would reach a PICKER_LDFLAGS written as $(LDFLAGS) here.
PICKER_LDFLAGS = $(LDFLAGS)
# The FreeBSD build needs the OpenZFS source tree: FreeBSD installs
# libzfs.h, libzfs_core.h, libnvpair.h and sys/nvpair.h, but not
# sys/avl.h, sys/fs/zfs.h, nor libspl's sys/mnttab.h and the Solaris
# types (uint_t, boolean_t, ...) those headers assume. This is the
# include set cddl/lib/libzfs/Makefile uses, minus the kernel-only
# parts. Override ZFS_SRC when the tree is elsewhere. Only zfsops.o
# gets these flags: libspl ships its own sys/acl.h, which would
# shadow FreeBSD's for walk.c and apply.c.
ZFS_SRC = /usr/src
ZFS_TOP = $(ZFS_SRC)/sys/contrib/openzfs
ZFS_CFLAGS = -I$(ZFS_TOP)/lib/libspl/include/os/freebsd \
	-I$(ZFS_TOP)/lib/libspl/include \
	-I$(ZFS_TOP)/include/os/freebsd -I$(ZFS_TOP)/include \
	-include $(ZFS_TOP)/include/os/freebsd/spl/sys/ccompile.h \
	-include $(ZFS_SRC)/sys/modules/zfs/zfs_config.h \
	-DNEED_SOLARIS_BOOLEAN -DHAVE_ISSETUGID -DHAVE_STRLCAT -DHAVE_STRLCPY
ZFS_LIBS = -lzfs_core -lzfs -lnvpair
ZFSOPS_CFLAGS =

# The picker knob (tracker issue port-picker-option, plan section 8
# item 14). PICKER=yes, the default, is the tool as it has been: the
# built-in picker inside it, a curses library on every link line, and
# the standalone zfs_rebase-picker beside it. PICKER=no puts
# src/plugins/picker/stub.c in the picker's place -- zr_picker_main
# says this build has no picker and returns 2 -- and links neither the
# model, nor the screens, nor the merge, nor the carried libdiff, nor
# any curses library, and builds no standalone binary. The tool is the
# same tool otherwise: -i CMD forks the command it names exactly as
# before, and -i alone is a child that exits 2, which is a non-zero
# exit like any other and leaves the gate standing.
#
# It is selected by computed macro names -- $(NAME_$(PICKER)) -- and
# not by .if or ifeq, because this file is plain POSIX make and must
# stay so: bmake and gmake both expand a nested reference, no target
# has to recurse for it, and one variable per thing the knob changes
# is the least code that says it. A PICKER that is neither yes nor no
# expands to nothing and fails at the link with the picker's own
# symbols undefined, rather than quietly building something half
# chosen.
#
# The knob is deliberately NOT part of the flavor stamp below, and
# does not empty $(BUILD) when it is turned. Both settings compile
# every object they share with the same flags, so the objects of one
# build are the objects of the other and only the link line differs.
# What the knob needs is therefore a relink and nothing more -- but it
# needs that relink, and until the review of 2026-09-11 (B1) it did
# not get one.
#
# Why the directory prerequisite was not enough: $(BUILD) is a real
# directory, and its recipe is mkdir -p, which does not change the
# mtime of a directory that is already there. A directory's mtime
# moves only when a NEW name appears in it. So the only knob switch
# "zfs_rebase: build ..." could ever notice was the first one, the one
# that compiled pickerstub.o -- and even that noticed it for the wrong
# binary: with make, make PICKER=no, make, the second step's fresh
# pickerstub.o bumped build/ and got zfs_rebase-picker relinked in the
# third, while zfs_rebase, linked after that bump, was left as the
# no-picker binary. make check then ran green over it, and make
# install and the port's do-install would have shipped it.
#
# $(BUILD)/.picker is the fix: a one-word file holding the PICKER this
# build directory was last linked under, rewritten only when the word
# differs, so its mtime moves on a knob switch and on no other run,
# and named as a prerequisite of both binaries. The objects are left
# alone, which is the whole point of keeping the knob out of the
# flavor stamp: a switch costs two links and no compile.
PICKER = yes

# The picker's screens are curses (plan section 3.1). The mac has
# ncurses and links it as -lncurses; FreeBSD base has ncursesw, which
# is what its own base programs link and what the port declares, so
# the freebsd targets override this the way they override the ZFS
# flags. Nothing wide-character is used -- every byte drawn is ASCII
# or an ACS macro -- so the two are interchangeable here, and the
# name is a variable only because the two systems spell it
# differently. The whole tool links it, not only the picker binary:
# --interactive forks and calls zr_picker_main in its own child
# (ruling 4), so screen.o is inside the tool. Every byte of that is
# the picker's own, so a PICKER=no line carries no curses library at
# all and the empty setting below is what says so.
CURSES_LIBS_yes = -lncurses
CURSES_LIBS_no =
CURSES_LIBS = $(CURSES_LIBS_$(PICKER))

# Which flavor build/ holds: portable, or freebsd (the ZFS layer built
# against the OpenZFS headers and linked against the libraries). The
# two do not share objects and a timestamp cannot tell them apart, so
# build/.flavor records which one is there: asking for the other
# empties build/ first, instead of relinking a portable zfsops.o into
# what was meant to be a FreeBSD binary.
FLAVOR = portable

# Where the objects go, and it is build/ for every build a person asks
# for by hand. tools/gate.sh overrides it, to build/nopicker, for the
# PICKER=no build it makes: that build may neither empty the
# developer's build/ nor relink ./zfs_rebase, since the box builds the
# freebsd flavor and runs the gate after it, and a gate that left a
# portable binary in the tree would break every harness that follows.
# Under build/ on purpose, so that make clean and .gitignore already
# cover it.
BUILD = build

# The binaries a build in $(BUILD) leaves in the tree, which the
# flavor wipe below removes along with the objects. The default build
# links its two beside the Makefile; a build with a directory of its
# own (make nopicker) links its one binary inside that directory and
# must not touch the tree's, which is what this variable is for.
BUILD_BINS = zfs_rebase zfs_rebase-picker

# The carried copy of FreeBSD's contrib/libdiff, foreign code under
# src/plugins/picker/libdiff (plan section 3.6, and that directory's
# UPSTREAM): the two-way diff the merge will call. Nothing calls it
# yet; it is linked in so that both flavors prove it builds. Not one
# byte of it is edited here, so an adaptation is a flag on these
# lines and never a change to a file.
#
# The include paths are the two FreeBSD's own lib/libdiff/Makefile
# passes: include/ for <arraylist.h> and <diff_main.h>, compat/include
# for the stdlib.h wrapper that declares reallocarray(3) and
# recallocarray(3) on a libc without them. The lib sources reach
# "diff_internal.h" and "diff_debug.h" beside themselves, so the
# source layout is kept and neither needs a path.
LIBDIFF = src/plugins/picker/libdiff
LIBDIFF_INCS = -I$(LIBDIFF)/include -I$(LIBDIFF)/compat/include

# FreeBSD builds libdiff with WARNS= -- no warning set at all. Ours is
# -Wall -Wextra -Werror -Wcast-qual, and three of its warnings fire on
# this code, so three are turned back off for these objects alone.
# The full set was tried first and each of these was added on its own:
#
#   -Wno-sign-compare	  diff_main.c:62, diff_myers.c:300,
#			  diff_patience.c:484, recallocarray.c:60, and
#			  many more, "comparison of integers of
#			  different signs": the arraylist lengths are
#			  unsigned and the algorithms' indices are int.
#   -Wno-unused-parameter diff_myers.c:792, diff_patience.c:380,
#			  diff_atomize_text.c:221: the algorithm and
#			  atomizer entry points take the whole
#			  signature and ignore what they do not need.
#   -Wno-cast-qual	  diff_patience.c:191 and :192, "cast from
#			  'const void *' to 'struct diff_atom **' drops
#			  const qualifier": the mergesort(3) comparison
#			  casts its arguments back to atom pointers.
LIBDIFF_CFLAGS = $(CFLAGS) $(LIBDIFF_INCS) \
	-Wno-sign-compare -Wno-unused-parameter -Wno-cast-qual

# libdiff's two compat allocators, compiled in every build -- both
# flavors, both knob settings that carry libdiff.
#
# They used to be cleared for FreeBSD, on the premise that FreeBSD's
# libc has both functions and its own lib/libdiff builds neither. That
# premise is only two months old and only true on the newest branches
# (the review of 2026-09-11, B2, and the FreeBSD tree it was measured
# in): reallocarray(3) is FBSD_1.4 and is everywhere, but
# recallocarray(3) landed in libc on main on 2025-10-03 (42664610795b,
# Symbol.map's FBSD_1.9 block, HISTORY "first appeared in ... FreeBSD
# 15.1"), and the merge back reached stable/15 and releng/15.1 and no
# further. releng/15.0, stable/14 and releng/14.3 have no such symbol
# -- 15.0's own lib/libdiff still builds the shim -- and the carried
# libdiff calls recallocarray from arraylist.h's ARRAYLIST grow, which
# is on every path, our own merge.c among the callers. So the cleared
# variable meant an undefined recallocarray at the link on every
# supported release but 15.1 and later, invisible on a box that tracks
# main.
#
# Compiling them everywhere is the whole fix and needs no OSVERSION
# conditional in the port: a definition in our own objects satisfies
# the reference at link time, and the shared library's export is never
# reached for it where libc has one. Neither file carries an #ifndef
# guard of its own -- both are plain definitions -- so this is the
# only shape that works without editing a carried file.
LIBDIFF_COMPAT_OBJS = $(BUILD)/reallocarray.o $(BUILD)/recallocarray.o

# recallocarray.c wipes the old allocation with explicit_bzero(3),
# which the mac's libc does not have. The substitute is a flag and not
# an edit: bzero(3) has the same signature and <string.h> reaches it
# on both of this Makefile's platforms. It is the weaker of the two --
# a compiler may elide bzero, where explicit_bzero may not be elided
# -- which costs nothing here: libdiff holds file text and no secrets.
#
# It does no harm on FreeBSD, which has both functions and now
# compiles this file too (see LIBDIFF_COMPAT_OBJS above). The macro is
# object-like on purpose: a function-like one would rewrite nothing
# but a call, and an object-like one rewrites the declaration in
# FreeBSD's <strings.h> as well -- "void explicit_bzero(void *,
# size_t)" becomes "void bzero(void *, size_t)", which is the very
# signature the line above it already declares, so it is a compatible
# redeclaration and not a conflict. What it costs there is the elision
# guarantee, which this file does not need.
LIBDIFF_BZERO_CFLAGS = -Dexplicit_bzero=bzero

LIBDIFF_OBJS = $(BUILD)/diff_main.o $(BUILD)/diff_myers.o \
	$(BUILD)/diff_patience.o $(BUILD)/diff_atomize_text.o \
	$(LIBDIFF_COMPAT_OBJS)

# The built-in picker, an internal plugin of its own (plan section
# 3.1). It goes into LIB_OBJS, so the tool and the tests reach it the
# way they reach every other object.
PICKER_OBJS = $(BUILD)/picker.o $(BUILD)/model.o $(BUILD)/screen.o \
	$(BUILD)/altscreen.o

# What the tool links for -i's built-in child, which is what the knob
# chooses: the picker whole -- its three objects, the merge and the
# carried libdiff -- or the stub entry on its own. Nothing else in
# LIB_OBJS moves, because nothing else of the tool is the picker's.
PICKER_LIB_yes = $(PICKER_OBJS) $(BUILD)/diff3.o $(BUILD)/merge.o \
	$(LIBDIFF_OBJS)
PICKER_LIB_no = $(BUILD)/pickerstub.o
PICKER_LIB = $(PICKER_LIB_$(PICKER))

# The standalone binary, which is a target and not an object: PICKER=no
# builds none. It stands in for the name wherever a target list would
# have named it -- all, check, and the freebsd recursion.
PICKER_BIN_yes = zfs_rebase-picker
PICKER_BIN_no =
PICKER_BIN = $(PICKER_BIN_$(PICKER))

# The standalone binary's main, which is NOT in LIB_OBJS: src/main.c
# is out of it for the same reason, and two mains in one link is two
# mains. The object is not build/main.o because that name is already
# the tool's; the source is src/plugins/picker/main.c all the same,
# since a program's main belongs in main.c.
PICKERMAIN_OBJS = $(BUILD)/pickermain.o
# Library objects are everything but main.o; tests link against them.
LIB_OBJS = $(BUILD)/vis.o $(BUILD)/name.o $(BUILD)/decide.o $(BUILD)/fixture.o \
	$(BUILD)/manifest.o $(BUILD)/walk.o $(BUILD)/yellow.o $(BUILD)/verify.o \
	$(BUILD)/apply.o $(BUILD)/zfsops.o $(BUILD)/run.o $(BUILD)/args.o \
	$(BUILD)/launch.o $(PICKER_LIB)
CORE_OBJS = $(BUILD)/main.o $(LIB_OBJS)

# What the standalone picker links: itself, the two document parsers,
# the name codec and the classifier the records name -- ground rule
# 1's whole dependency list -- and never the driver or the ZFS layer.
# That is what keeps it linkable with no libzfs on the line, on the
# mac and in a build jail alike.
# The merge is on the line since picker-merge: screen 2 calls it, it
# is the picker's own, and it brings the carried libdiff with it. It
# is still the least that links -- no driver, no ZFS layer.
PICKER_BIN_OBJS = $(PICKERMAIN_OBJS) $(PICKER_OBJS) $(BUILD)/manifest.o \
	$(BUILD)/decide.o $(BUILD)/name.o $(BUILD)/vis.o $(BUILD)/merge.o $(BUILD)/diff3.o \
	$(LIBDIFF_OBJS)

# check_picker and check_merge call what a PICKER=no build does not
# carry, so the knob picks the list too. Every other test links the
# same objects either way, check_run's built-in child (ZI22) with
# them: the picker refuses that test's scratch before it touches a
# terminal, the stub says there is no picker, and both are one line
# and an exit of 2.
PICKER_TESTS_yes = check_picker check_merge
PICKER_TESTS_no =
TESTS = check_vis check_name check_fixture check_manifest check_walk \
	check_yellow check_roundtrip check_apply check_verify check_args \
	check_run $(PICKER_TESTS_$(PICKER))

all: build zfs_rebase $(PICKER_BIN)

build: flavor
	mkdir -p $(BUILD)

# Just the directory, with no flavor check and no wipe: the knob's
# gate links out of whatever flavor $(BUILD) already holds and must
# never empty it.
builddir:
	@mkdir -p $(BUILD)

flavor:
	@mkdir -p $(BUILD); \
	if [ "$$(cat $(BUILD)/.flavor 2>/dev/null)" != "$(FLAVOR)" ]; then \
	    rm -f $(BUILD)/*.o $(BUILD_BINS); \
	    echo "$(FLAVOR)" > $(BUILD)/.flavor; \
	fi

# The knob stamp (see PICKER above). flavor is phony, so this recipe
# runs on every invocation; it writes only when the word on disk is
# not the word asked for, so the file's mtime moves on a knob switch
# and on no other run, and the two binaries below name it as a
# prerequisite. echo rather than touch, so that the file also says
# which setting the tree's binaries were linked under.
#
# The objects are left alone: both settings compile them with the same
# flags, so a switch costs two links and no compile. That is why this
# is a stamp and not a second flavor.
#
# One limit, measured rather than assumed: GNU make 3.81, the make on
# the mac, compares whole seconds, so a stamp rewritten inside the
# same second as the last link is not newer than it and that one
# relink is missed. A hand at the keyboard is never that fast, and a
# script that wants the switch inside a second should sleep 1 -- which
# is what the review's own reproduction did. The stamp is not made to
# remove the binaries instead: make has already read their timestamps
# by the time a prerequisite's recipe runs, so removing them there
# leaves no binary at all rather than a relinked one (measured; the
# flavor wipe above gets away with it only because it takes the
# objects too, and those are read afterwards).
$(BUILD)/.picker: flavor
	@if [ "$$(cat $@ 2>/dev/null)" != "$(PICKER)" ]; then \
	    echo "$(PICKER)" > $@; \
	fi

zfs_rebase: build $(BUILD)/.picker $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(LDFLAGS) $(CURSES_LIBS)

zfs_rebase-picker: build $(BUILD)/.picker $(PICKER_BIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PICKER_BIN_OBJS) $(PICKER_LDFLAGS) \
	    $(CURSES_LIBS)

# PICKER goes through by name like every other override, and the
# curses library is overridden as CURSES_LIBS_yes and not as
# CURSES_LIBS: the knob is what computes the one from the other, and
# an override of the computed name would put ncursesw back on a
# PICKER=no line.
freebsd:
	$(MAKE) FLAVOR=freebsd CFLAGS="$(CFLAGS) -DZR_FREEBSD" \
	    ZFSOPS_CFLAGS="$(ZFS_CFLAGS)" \
	    CURSES_LIBS_yes="-lncursesw" PICKER="$(PICKER)" \
	    PICKER_LDFLAGS="$(LDFLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" zfs_rebase $(PICKER_BIN)

# The gates for the freebsd flavor. check links the test programs and
# relinks zfs_rebase against LIB_OBJS, which includes zfsops.o, so on
# FreeBSD it needs the same flags the freebsd target uses: plain check
# would compile zfsops.c without the OpenZFS headers and link without
# the ZFS libraries. The two flavors do not share build/, because the
# objects differ, and the flavor stamp is what keeps them apart: the
# inner make finds a portable build/ and empties it first.
check-freebsd:
	$(MAKE) FLAVOR=freebsd CFLAGS="$(CFLAGS) -DZR_FREEBSD" \
	    ZFSOPS_CFLAGS="$(ZFS_CFLAGS)" \
	    CURSES_LIBS_yes="-lncursesw" PICKER="$(PICKER)" \
	    PICKER_LDFLAGS="$(LDFLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" check

# Every rule below names its source's true include closure, and not
# its direct includes alone: the transitive reach of one of ours,
# measured with the compiler's own -MM and then written out by hand in
# this file's style. Eleven of the twenty-two were short of it before
# the review of 2026-09-11 (B3) -- src/name.h missed seven objects,
# every picker object among them -- and a header change then rebuilt
# some objects and not others and linked both binaries out of the
# mixture, with make check running against the stale half.
#
# By hand and not generated on purpose: -MMD -MP plus an -include of
# the .d files is not POSIX make, and this file must stay POSIX so
# that bmake and gmake both read it. The closures are small and the
# header graph is ours (apply.h -> manifest, verify, walk;
# args.h -> decide; decide.h -> name; manifest.h -> decide;
# picker.h -> manifest, merge; verify.h -> manifest, name, walk,
# yellow; walk.h -> name; yellow.h -> walk), so re-measuring is one
# cc -MM per source when a new include lands.
$(BUILD)/main.o: src/main.c src/args.h src/decide.h src/fixture.h \
	src/manifest.h src/name.h src/run.h src/walk.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/main.c

$(BUILD)/args.o: src/args.c src/args.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/args.c

$(BUILD)/launch.o: src/launch.c src/launch.h src/plugins/picker/picker.h \
	src/plugins/picker/merge.h src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/launch.c

# The built-in picker, an internal plugin of its own: it depends on
# the documents and the name codec and never on the driver, so it
# builds with the same flags and no include path of its own -- src is
# already on it, and launch.c names the header by its path under it.
$(BUILD)/picker.o: src/plugins/picker/picker.c src/plugins/picker/picker.h \
	src/plugins/picker/merge.h src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/picker.c

# The stub entry, which is the whole of the picker in a PICKER=no
# build. It includes picker.h like the real one, so that the
# declaration the launcher calls through and the definition it
# reaches are held to each other in both builds.
$(BUILD)/pickerstub.o: src/plugins/picker/stub.c src/plugins/picker/picker.h \
	src/plugins/picker/merge.h src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/stub.c

# The carried libdiff, one object per source, in the source's own
# layout. LIBDIFF_HDRS is every header of the copy, so that a refresh
# rebuilds all of it: these are not our files and the exact reach of
# each include is upstream's business, not this Makefile's.
LIBDIFF_HDRS = $(LIBDIFF)/include/arraylist.h $(LIBDIFF)/include/diff_main.h \
	$(LIBDIFF)/lib/diff_internal.h $(LIBDIFF)/lib/diff_debug.h \
	$(LIBDIFF)/compat/include/stdlib.h

$(BUILD)/diff_main.o: $(LIBDIFF)/lib/diff_main.c $(LIBDIFF_HDRS)
	$(CC) $(LIBDIFF_CFLAGS) -c -o $@ $(LIBDIFF)/lib/diff_main.c

$(BUILD)/diff_myers.o: $(LIBDIFF)/lib/diff_myers.c $(LIBDIFF_HDRS)
	$(CC) $(LIBDIFF_CFLAGS) -c -o $@ $(LIBDIFF)/lib/diff_myers.c

$(BUILD)/diff_patience.o: $(LIBDIFF)/lib/diff_patience.c $(LIBDIFF_HDRS)
	$(CC) $(LIBDIFF_CFLAGS) -c -o $@ $(LIBDIFF)/lib/diff_patience.c

$(BUILD)/diff_atomize_text.o: $(LIBDIFF)/lib/diff_atomize_text.c $(LIBDIFF_HDRS)
	$(CC) $(LIBDIFF_CFLAGS) -c -o $@ $(LIBDIFF)/lib/diff_atomize_text.c

$(BUILD)/reallocarray.o: $(LIBDIFF)/compat/reallocarray.c \
	$(LIBDIFF)/compat/include/stdlib.h
	$(CC) $(LIBDIFF_CFLAGS) -c -o $@ $(LIBDIFF)/compat/reallocarray.c

$(BUILD)/recallocarray.o: $(LIBDIFF)/compat/recallocarray.c \
	$(LIBDIFF)/compat/include/stdlib.h
	$(CC) $(LIBDIFF_CFLAGS) $(LIBDIFF_BZERO_CFLAGS) -c -o $@ \
	    $(LIBDIFF)/compat/recallocarray.c
$(BUILD)/model.o: src/plugins/picker/model.c src/plugins/picker/picker.h \
	src/plugins/picker/merge.h src/manifest.h src/decide.h src/name.h \
	src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/model.c

# The three-way merge. diff3.c is the walk, adapted from FreeBSD's
# usr.bin/diff3/diff3.c and a file of ours: it knows nothing of libdiff
# and builds with the plain flags. merge.c is the glue and gets
# libdiff's own include paths, plus the copy's lib/ directory for
# diff_internal.h, which is where struct diff_chunk is defined -- the
# installed diff_main.h keeps it opaque and the accessors for it live
# in the diff_output.c the copy does not carry. Reading a carried
# header is not editing one, and the alternative would be a second
# copy of the struct here.
$(BUILD)/diff3.o: src/plugins/picker/diff3.c src/plugins/picker/merge.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/diff3.c

$(BUILD)/merge.o: src/plugins/picker/merge.c src/plugins/picker/merge.h \
	$(LIBDIFF_HDRS)
	$(CC) $(CFLAGS) $(LIBDIFF_INCS) -I$(LIBDIFF)/lib -c -o $@ \
	    src/plugins/picker/merge.c

$(BUILD)/screen.o: src/plugins/picker/screen.c src/plugins/picker/picker.h \
	src/plugins/picker/merge.h src/manifest.h src/decide.h src/name.h \
	src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/screen.c

$(BUILD)/altscreen.o: src/plugins/picker/altscreen.c \
	src/plugins/picker/picker.h src/plugins/picker/merge.h \
	src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/altscreen.c

$(BUILD)/pickermain.o: src/plugins/picker/main.c src/plugins/picker/picker.h \
	src/plugins/picker/merge.h src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/main.c

$(BUILD)/vis.o: src/vis.c src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/vis.c

$(BUILD)/name.o: src/name.c src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/name.c

$(BUILD)/decide.o: src/decide.c src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/decide.c

$(BUILD)/fixture.o: src/fixture.c src/fixture.h src/name.h src/vis.h src/walk.h
	$(CC) $(CFLAGS) -c -o $@ src/fixture.c

$(BUILD)/manifest.o: src/manifest.c src/manifest.h src/decide.h src/name.h \
	src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/manifest.c

$(BUILD)/walk.o: src/walk.c src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/walk.c

$(BUILD)/yellow.o: src/yellow.c src/yellow.h src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/yellow.c

$(BUILD)/verify.o: src/verify.c src/verify.h src/manifest.h src/decide.h \
	src/walk.h src/name.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/verify.c

$(BUILD)/apply.o: src/apply.c src/apply.h src/verify.h src/manifest.h \
	src/decide.h src/walk.h src/name.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/apply.c

$(BUILD)/zfsops.o: src/zfsops.c src/zfsops.h
	$(CC) $(CFLAGS) $(ZFSOPS_CFLAGS) -c -o $@ src/zfsops.c

$(BUILD)/run.o: src/run.c src/run.h src/apply.h src/decide.h src/launch.h \
	src/manifest.h src/name.h src/verify.h src/walk.h src/yellow.h \
	src/zfsops.h
	$(CC) $(CFLAGS) -c -o $@ src/run.c

# zfs_rebase-picker is built here because check_picker's pty tests run
# it as their child: the standalone binary and the tool's own child
# are the same objects, and ZP114 asserts it.
check: $(PICKER_BIN) unit battery fixtures replay-expect-check

unit: build $(LIB_OBJS)
	@for t in $(TESTS); do \
	    $(CC) $(CFLAGS) -o $(BUILD)/$$t tests/$$t.c $(LIB_OBJS) \
		$(LDFLAGS) $(CURSES_LIBS) || exit 1; \
	    ./$(BUILD)/$$t || { echo "FAIL $$t"; exit 1; }; \
	    echo "ok   $$t"; \
	done

# The M2 gate: every fixture built as directories, run through --posix,
# compared with its expect block.
fixtures: zfs_rebase
	sh tests/run-fixtures.sh

# The M1 gate: every committed battery, both modes.
battery: build $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $(BUILD)/check_battery tests/check_battery.c $(LIB_OBJS) \
	    $(LDFLAGS) $(CURSES_LIBS)
	./$(BUILD)/check_battery tests/battery/*.txt

# tests/box/replay-expect.txt is what tests/box/run-replay.sh asserts
# the tool's "N pools unchanged" line against, one line per fixture.
# Regenerate it here; check fails when the committed file is stale. The
# box may have no python3, so the check says so and passes.
replay-expect:
	python3 tools/replay-expect.py > tests/box/replay-expect.txt

replay-expect-check: build
	@if command -v python3 > /dev/null 2>&1; then \
	    python3 tools/replay-expect.py > $(BUILD)/replay-expect.txt || exit 1; \
	    if cmp -s $(BUILD)/replay-expect.txt tests/box/replay-expect.txt; then \
		echo "ok   replay-expect.txt"; \
	    else \
		echo "FAIL tests/box/replay-expect.txt is stale: make replay-expect"; \
		exit 1; \
	    fi; \
	else echo "skip replay-expect.txt: no python3"; fi

gate:
	sh tools/gate.sh

# The knob's own gate, which tools/gate.sh runs: the tool linked with
# the stub in the picker's place, so that the setting cannot rot
# unnoticed. A curses call, a picker symbol or a libdiff symbol
# reached from anywhere outside the picker fails this link, with
# nothing on the line to satisfy it, and that is the whole of the
# check.
#
# It links out of $(BUILD), the objects the tree already has. The
# knob's own premise says it may: both settings compile every object
# they share with the same flags, so the only object this needs that a
# default build has not got is the stub's, and the only difference is
# the link line. It used to recompile the whole tree into a directory
# of its own on every cold gate -- fifteen compiles to prove a claim
# that costs one (the review of 2026-09-11, B11) -- and worse than
# wasteful on the box, where the second tree was a portable one and so
# not the artifact under test at all (B5).
#
# The flavor is whatever $(BUILD) already holds, read off the stamp
# rather than assumed, because tools/gate.sh runs this target with no
# flags of its own on both machines. On the box that makes the gate's
# artifact the PICKER=no freebsd tool, which is exactly what the
# port's option-off package contains and what no gate built before.
# Nothing here touches ./zfs_rebase or $(BUILD)/.picker: the binary it
# writes is inside $(BUILD) under a name of its own, and the object
# list is explicit rather than $(PICKER_LIB), so the knob variable
# itself is left at whatever the caller had.
NOPICKER_OBJS = $(BUILD)/main.o $(BUILD)/vis.o $(BUILD)/name.o \
	$(BUILD)/decide.o $(BUILD)/fixture.o $(BUILD)/manifest.o \
	$(BUILD)/walk.o $(BUILD)/yellow.o $(BUILD)/verify.o \
	$(BUILD)/apply.o $(BUILD)/zfsops.o $(BUILD)/run.o $(BUILD)/args.o \
	$(BUILD)/launch.o $(BUILD)/pickerstub.o

nopicker:
	@if [ "$$(cat $(BUILD)/.flavor 2>/dev/null)" = freebsd ]; then \
	    echo "nopicker: the freebsd flavor in $(BUILD)"; \
	    $(MAKE) CFLAGS="$(CFLAGS) -DZR_FREEBSD" \
		ZFSOPS_CFLAGS="$(ZFS_CFLAGS)" \
		LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" nopicker-link; \
	else \
	    echo "nopicker: the portable flavor in $(BUILD)"; \
	    $(MAKE) nopicker-link; \
	fi

nopicker-link: builddir $(NOPICKER_OBJS)
	$(CC) $(CFLAGS) -o $(BUILD)/zfs_rebase-nopicker $(NOPICKER_OBJS) \
	    $(LDFLAGS)

# Install whatever zfs_rebase the build left in place: the portable
# core from "make", or the real tool from "make freebsd". Build the
# flavor you mean first -- this target has no prerequisite on purpose,
# so that it can never relink a freebsd binary without the ZFS flags,
# and install(1) says so plainly when there is nothing to install.
#
# sbin, because the tool must run as root. share/man, because that is
# where FreeBSD's own bsd.man.mk puts a page (MANDIR = ${SHAREDIR}/man/
# man, SHAREDIR = /usr/share) and where the ports tree has kept them
# since they moved out of ${PREFIX}/man.
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/sbin
MANDIR = $(PREFIX)/share/man/man8

# The standalone binary is installed by a step of its own, which
# PICKER=no leaves out of the prerequisites; the port's own do-install
# passes the knob through, so a package built with the option off
# installs the tool and the page and nothing else. The step makes the
# directory itself rather than leaning on an order make does not
# promise, and install -d is idempotent.
PICKER_INSTALL_yes = install-picker
PICKER_INSTALL_no =

install: $(PICKER_INSTALL_$(PICKER))
	install -d $(DESTDIR)$(BINDIR)
	install -m 0555 zfs_rebase $(DESTDIR)$(BINDIR)/zfs_rebase
	install -d $(DESTDIR)$(MANDIR)
	install -m 0444 zfs_rebase.8 $(DESTDIR)$(MANDIR)/zfs_rebase.8

install-picker:
	install -d $(DESTDIR)$(BINDIR)
	install -m 0555 zfs_rebase-picker $(DESTDIR)$(BINDIR)/zfs_rebase-picker

# A box probe, not part of the tool: the four questions the private
# mount raises, with libzfs's own words for what went wrong. It does
# not go through the flavor stamp, so it leaves a freebsd build/ as
# it found it; tests/box/run-probe.sh makes a pool and runs it.
probe-mount:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -DZR_FREEBSD $(ZFS_CFLAGS) -o $(BUILD)/probe-mount \
	    tools/probe-mount.c $(ZFS_LIBS)

clean:
	rm -rf $(BUILD) zfs_rebase zfs_rebase-picker

.PHONY: all flavor builddir freebsd check check-freebsd unit battery \
	fixtures gate nopicker nopicker-link probe-mount \
	install install-picker replay-expect replay-expect-check clean
