# zfs_rebase: portable core by default, freebsd target adds the ZFS
# layer. Plain POSIX make; no GNU-only functions, so bmake and gmake
# both work. Object lists are explicit on purpose.

CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -Wcast-qual -O2 -g -Isrc
LDFLAGS =
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
# The knob is deliberately NOT part of the flavor stamp below. Both
# settings compile every object they share with the same flags, so the
# objects of one build are the objects of the other and only the link
# line differs: switching the knob in one build directory relinks and
# can never miscompile. The stamp is for the portable/freebsd split,
# where the objects themselves are different objects.
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

# libdiff's two compat allocators. Neither file carries an #ifndef
# guard of its own -- both are plain definitions -- so they cannot be
# compiled everywhere and left to vanish on a libc that has the
# functions. They are gated instead, the way the ZFS include set is:
# a variable the freebsd targets clear. FreeBSD's libc has
# reallocarray(3) and recallocarray(3), and its own lib/libdiff builds
# neither file; the mac's libc has neither function, so the portable
# flavor builds both.
LIBDIFF_COMPAT_OBJS = $(BUILD)/reallocarray.o $(BUILD)/recallocarray.o

# recallocarray.c wipes the old allocation with explicit_bzero(3),
# which the mac's libc does not have either. The substitute is a flag
# and not an edit: bzero(3) has the same signature and <string.h>
# reaches it on both of this Makefile's platforms. It is the weaker of
# the two -- a compiler may elide bzero, where explicit_bzero may not
# be elided -- which costs nothing here: libdiff holds file text and
# no secrets, and the shipped FreeBSD build does not compile this file
# at all. An object-like macro on purpose: a function-like one would
# also rewrite the declaration in FreeBSD's <strings.h>.
LIBDIFF_BZERO_CFLAGS = -Dexplicit_bzero=bzero

LIBDIFF_OBJS = $(BUILD)/diff_main.o $(BUILD)/diff_myers.o \
	$(BUILD)/diff_patience.o $(BUILD)/diff_atomize_text.o \
	$(LIBDIFF_COMPAT_OBJS)

# The built-in picker, an internal plugin of its own (plan section
# 3.1). It goes into LIB_OBJS, so the tool and the tests reach it the
# way they reach every other object.
PICKER_OBJS = $(BUILD)/picker.o $(BUILD)/model.o $(BUILD)/screen.o

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
# It is the least that links today. picker-merge adds build/merge.o,
# build/diff3.o and $(LIBDIFF_OBJS) here when screen 2 calls them: the
# merge is the picker's own and belongs in the picker's own binary,
# and nothing of it is reached from screen 1.
PICKER_BIN_OBJS = $(PICKERMAIN_OBJS) $(PICKER_OBJS) $(BUILD)/manifest.o \
	$(BUILD)/decide.o $(BUILD)/name.o $(BUILD)/vis.o

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

flavor:
	@mkdir -p $(BUILD); \
	if [ "$$(cat $(BUILD)/.flavor 2>/dev/null)" != "$(FLAVOR)" ]; then \
	    rm -f $(BUILD)/*.o $(BUILD_BINS); \
	    echo "$(FLAVOR)" > $(BUILD)/.flavor; \
	fi

zfs_rebase: build $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(LDFLAGS) $(CURSES_LIBS)

zfs_rebase-picker: build $(PICKER_BIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PICKER_BIN_OBJS) $(LDFLAGS) $(CURSES_LIBS)

# PICKER goes through by name like every other override, and the
# curses library is overridden as CURSES_LIBS_yes and not as
# CURSES_LIBS: the knob is what computes the one from the other, and
# an override of the computed name would put ncursesw back on a
# PICKER=no line.
freebsd:
	$(MAKE) FLAVOR=freebsd CFLAGS="$(CFLAGS) -DZR_FREEBSD" \
	    ZFSOPS_CFLAGS="$(ZFS_CFLAGS)" LIBDIFF_COMPAT_OBJS="" \
	    CURSES_LIBS_yes="-lncursesw" PICKER="$(PICKER)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" zfs_rebase $(PICKER_BIN)

# The gates for the freebsd flavor. check links the test programs and
# relinks zfs_rebase against LIB_OBJS, which includes zfsops.o, so on
# FreeBSD it needs the same flags the freebsd target uses: plain check
# would compile zfsops.c without the OpenZFS headers and link without
# the ZFS libraries. The two flavors do not share build/, because the
# objects differ, and the flavor stamp is what keeps them apart: the
# inner make finds a portable build/ and empties it first. It clears
# LIBDIFF_COMPAT_OBJS for the same reason the freebsd target does.
check-freebsd:
	$(MAKE) FLAVOR=freebsd CFLAGS="$(CFLAGS) -DZR_FREEBSD" \
	    ZFSOPS_CFLAGS="$(ZFS_CFLAGS)" LIBDIFF_COMPAT_OBJS="" \
	    CURSES_LIBS_yes="-lncursesw" PICKER="$(PICKER)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" check

$(BUILD)/main.o: src/main.c src/args.h src/decide.h src/fixture.h \
	src/manifest.h src/name.h src/run.h src/walk.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/main.c

$(BUILD)/args.o: src/args.c src/args.h src/decide.h
	$(CC) $(CFLAGS) -c -o $@ src/args.c

$(BUILD)/launch.o: src/launch.c src/launch.h src/plugins/picker/picker.h
	$(CC) $(CFLAGS) -c -o $@ src/launch.c

# The built-in picker, an internal plugin of its own: it depends on
# the documents and the name codec and never on the driver, so it
# builds with the same flags and no include path of its own -- src is
# already on it, and launch.c names the header by its path under it.
$(BUILD)/picker.o: src/plugins/picker/picker.c src/plugins/picker/picker.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/picker.c

# The stub entry, which is the whole of the picker in a PICKER=no
# build. It includes picker.h like the real one, so that the
# declaration the launcher calls through and the definition it
# reaches are held to each other in both builds.
$(BUILD)/pickerstub.o: src/plugins/picker/stub.c src/plugins/picker/picker.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/stub.c

# The carried libdiff, one object per source, in the source's own
# layout. LIBDIFF_HDRS is every header of the copy, so that a refresh
# rebuilds all of it: these are not our files and the exact reach of
# each include is upstream's business, not this Makefile's.
LIBDIFF_HDRS = $(LIBDIFF)/include/arraylist.h $(LIBDIFF)/include/diff_main.h \
	$(LIBDIFF)/lib/diff_internal.h $(LIBDIFF)/lib/diff_debug.h

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
	src/manifest.h src/vis.h
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
	src/manifest.h src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/screen.c

$(BUILD)/pickermain.o: src/plugins/picker/main.c src/plugins/picker/picker.h
	$(CC) $(CFLAGS) -c -o $@ src/plugins/picker/main.c

$(BUILD)/vis.o: src/vis.c src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/vis.c

$(BUILD)/name.o: src/name.c src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/name.c

$(BUILD)/decide.o: src/decide.c src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/decide.c

$(BUILD)/fixture.o: src/fixture.c src/fixture.h src/name.h src/vis.h src/walk.h
	$(CC) $(CFLAGS) -c -o $@ src/fixture.c

$(BUILD)/manifest.o: src/manifest.c src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/manifest.c

$(BUILD)/walk.o: src/walk.c src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/walk.c

$(BUILD)/yellow.o: src/yellow.c src/yellow.h src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/yellow.c

$(BUILD)/verify.o: src/verify.c src/verify.h src/manifest.h src/walk.h \
	src/name.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/verify.c

$(BUILD)/apply.o: src/apply.c src/apply.h src/verify.h src/manifest.h \
	src/walk.h src/name.h
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

# The knob's own gate, which tools/gate.sh runs: the PICKER=no tool,
# compiled and linked whole so that the setting cannot rot unnoticed
# -- a curses call or a picker symbol reached from anywhere outside
# the picker fails this link, with nothing on the line to satisfy it.
# It recurses the way freebsd does, so the knob is no rather than
# whatever the caller's PICKER said, and it builds into a directory
# of its own (BUILD) under a name of its own, so that neither the
# developer's build/ nor ./zfs_rebase is touched. Costs one compile
# of each object the first time and a link after that.
NOPICKER_BUILD = build/nopicker

nopicker:
	$(MAKE) PICKER=no BUILD=$(NOPICKER_BUILD) \
	    BUILD_BINS="$(NOPICKER_BUILD)/zfs_rebase-nopicker" \
	    $(NOPICKER_BUILD)/zfs_rebase-nopicker

$(BUILD)/zfs_rebase-nopicker: build $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(LDFLAGS) $(CURSES_LIBS)

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

.PHONY: all flavor freebsd check check-freebsd unit battery fixtures gate \
	nopicker probe-mount \
	install install-picker replay-expect replay-expect-check clean
