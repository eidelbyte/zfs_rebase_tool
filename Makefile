# zfs_rebase: portable core by default, freebsd target adds the ZFS
# layer. Plain POSIX make; no GNU-only functions, so bmake and gmake
# both work. Object lists are explicit on purpose.

CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -Wcast-qual -O2 -g -Isrc
LDFLAGS =
ZFS_INCLUDE = /usr/include
ZFS_LIBS = -lzfs_core -lzfs -lnvpair

# Library objects are everything but main.o; tests link against them.
LIB_OBJS = build/vis.o build/name.o build/decide.o build/fixture.o \
	build/manifest.o
CORE_OBJS = build/main.o $(LIB_OBJS)
FREEBSD_OBJS =
TESTS = check_vis check_name check_fixture check_manifest

all: build zfs_rebase

build:
	mkdir -p build

zfs_rebase: $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(LDFLAGS)

freebsd: build
	$(MAKE) CFLAGS="$(CFLAGS) -DZR_FREEBSD -I$(ZFS_INCLUDE)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" \
	    CORE_OBJS="$(CORE_OBJS) $(FREEBSD_OBJS)" zfs_rebase

build/main.o: src/main.c
	$(CC) $(CFLAGS) -c -o $@ src/main.c

build/vis.o: src/vis.c src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/vis.c

build/name.o: src/name.c src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/name.c

build/decide.o: src/decide.c src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/decide.c

build/fixture.o: src/fixture.c src/fixture.h src/name.h src/vis.h
	$(CC) $(CFLAGS) -c -o $@ src/fixture.c

build/manifest.o: src/manifest.c src/manifest.h src/decide.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/manifest.c

check: unit battery

unit: build $(LIB_OBJS)
	@for t in $(TESTS); do \
	    $(CC) $(CFLAGS) -o build/$$t tests/$$t.c $(LIB_OBJS) || exit 1; \
	    ./build/$$t || { echo "FAIL $$t"; exit 1; }; \
	    echo "ok   $$t"; \
	done

# The M1 gate: every committed battery, both modes.
battery: build $(LIB_OBJS)
	$(CC) $(CFLAGS) -o build/check_battery tests/check_battery.c $(LIB_OBJS)
	./build/check_battery tests/battery/*.txt

gate:
	sh tools/gate.sh

clean:
	rm -rf build zfs_rebase

.PHONY: all freebsd check unit battery gate clean
