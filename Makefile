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

# Library objects are everything but main.o; tests link against them.
LIB_OBJS = build/vis.o build/name.o build/decide.o build/fixture.o \
	build/manifest.o build/walk.o build/yellow.o build/verify.o \
	build/apply.o build/zfsops.o build/run.o
CORE_OBJS = build/main.o $(LIB_OBJS)
TESTS = check_vis check_name check_fixture check_manifest check_walk \
	check_yellow check_roundtrip check_apply check_verify

all: build zfs_rebase

build:
	mkdir -p build

zfs_rebase: $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(LDFLAGS)

freebsd: build
	$(MAKE) CFLAGS="$(CFLAGS) -DZR_FREEBSD" \
	    ZFSOPS_CFLAGS="$(ZFS_CFLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" zfs_rebase

# The gates for the freebsd flavor. check links the test programs and
# relinks zfs_rebase against LIB_OBJS, which includes zfsops.o, so on
# FreeBSD it needs the same flags the freebsd target uses: plain check
# would compile zfsops.c without the OpenZFS headers and link without
# the ZFS libraries. The rule for the two flavors is that they do not
# share build/, because the objects differ; each flavor starts from
# make clean. This target therefore has no prerequisite that could
# build or reuse a portable object: it only recurses, and the inner
# make builds what the FreeBSD flags demand.
check-freebsd:
	$(MAKE) CFLAGS="$(CFLAGS) -DZR_FREEBSD" \
	    ZFSOPS_CFLAGS="$(ZFS_CFLAGS)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" check

build/main.o: src/main.c src/decide.h src/fixture.h src/manifest.h \
	src/name.h src/run.h src/walk.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/main.c

build/vis.o: src/vis.c src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/vis.c

build/name.o: src/name.c src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/name.c

build/decide.o: src/decide.c src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/decide.c

build/fixture.o: src/fixture.c src/fixture.h src/name.h src/vis.h src/walk.h
	$(CC) $(CFLAGS) -c -o $@ src/fixture.c

build/manifest.o: src/manifest.c src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/manifest.c

build/walk.o: src/walk.c src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/walk.c

build/yellow.o: src/yellow.c src/yellow.h src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/yellow.c

build/verify.o: src/verify.c src/verify.h src/manifest.h src/walk.h \
	src/name.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/verify.c

build/apply.o: src/apply.c src/apply.h src/verify.h src/manifest.h src/walk.h \
	src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/apply.c

build/zfsops.o: src/zfsops.c src/zfsops.h
	$(CC) $(CFLAGS) $(ZFSOPS_CFLAGS) -c -o $@ src/zfsops.c

build/run.o: src/run.c src/run.h src/apply.h src/decide.h \
	src/manifest.h src/name.h src/verify.h src/walk.h src/yellow.h \
	src/zfsops.h
	$(CC) $(CFLAGS) -c -o $@ src/run.c

check: unit battery fixtures

unit: build $(LIB_OBJS)
	@for t in $(TESTS); do \
	    $(CC) $(CFLAGS) -o build/$$t tests/$$t.c $(LIB_OBJS) \
		$(LDFLAGS) || exit 1; \
	    ./build/$$t || { echo "FAIL $$t"; exit 1; }; \
	    echo "ok   $$t"; \
	done

# The M2 gate: every fixture built as directories, run through --posix,
# compared with its expect block.
fixtures: zfs_rebase
	sh tests/run-fixtures.sh

# The M1 gate: every committed battery, both modes.
battery: build $(LIB_OBJS)
	$(CC) $(CFLAGS) -o build/check_battery tests/check_battery.c $(LIB_OBJS) \
	    $(LDFLAGS)
	./build/check_battery tests/battery/*.txt

gate:
	sh tools/gate.sh

clean:
	rm -rf build zfs_rebase

.PHONY: all freebsd check check-freebsd unit battery fixtures gate clean
