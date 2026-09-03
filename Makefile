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
CORE_OBJS = build/main.o $(LIB_OBJS)
FREEBSD_OBJS = 
TESTS = check_vis check_name check_fixture check_manifest check_walk \
	check_yellow check_roundtrip check_apply check_diff

all: build zfs_rebase

build:
	mkdir -p build

zfs_rebase: $(CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(CORE_OBJS) $(LDFLAGS)

freebsd: build
	$(MAKE) CFLAGS="$(CFLAGS) -DZR_FREEBSD -I$(ZFS_INCLUDE)" \
	    LDFLAGS="$(LDFLAGS) $(ZFS_LIBS)" \
	    CORE_OBJS="$(CORE_OBJS) $(FREEBSD_OBJS)" zfs_rebase

build/main.o: src/main.c src/decide.h src/fixture.h src/manifest.h \
	src/name.h src/walk.h src/yellow.h
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

build/walk.o: src/walk.c src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/walk.c

build/yellow.o: src/yellow.c src/yellow.h src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/yellow.c

build/apply.o: src/apply.c src/apply.h src/manifest.h src/walk.h src/name.h
	$(CC) $(CFLAGS) -c -o $@ src/apply.c

build/diff.o: src/diff.c src/diff.h src/name.h src/walk.h src/yellow.h
	$(CC) $(CFLAGS) -c -o $@ src/diff.c

check: unit battery fixtures

unit: build $(LIB_OBJS)
	@for t in $(TESTS); do \
	    $(CC) $(CFLAGS) -o build/$$t tests/$$t.c $(LIB_OBJS) || exit 1; \
	    ./build/$$t || { echo "FAIL $$t"; exit 1; }; \
	    echo "ok   $$t"; \
	done

# The M2 gate: every fixture built as directories, run through --posix,
# compared with its expect block.
fixtures: zfs_rebase
	sh tests/run-fixtures.sh

# The M1 gate: every committed battery, both modes.
battery: build $(LIB_OBJS)
	$(CC) $(CFLAGS) -o build/check_battery tests/check_battery.c $(LIB_OBJS)
	./build/check_battery tests/battery/*.txt

gate:
	sh tools/gate.sh

clean:
	rm -rf build zfs_rebase

.PHONY: all freebsd check unit battery fixtures gate clean
