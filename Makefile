# zfs_rebase: portable core by default, freebsd target adds the ZFS
# layer. Plain POSIX make; no GNU-only functions, so bmake and gmake
# both work. Object lists are explicit on purpose.

CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -Wcast-qual -O2 -g -Isrc
LDFLAGS =
ZFS_INCLUDE = /usr/include
ZFS_LIBS = -lzfs_core -lzfs -lnvpair

# Library objects are everything but main.o; tests link against them.
LIB_OBJS =
CORE_OBJS = build/main.o $(LIB_OBJS)
FREEBSD_OBJS =
TESTS = check_empty

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

check: build $(LIB_OBJS)
	@for t in $(TESTS); do \
	    $(CC) $(CFLAGS) -o build/$$t tests/$$t.c $(LIB_OBJS) || exit 1; \
	    ./build/$$t || { echo "FAIL $$t"; exit 1; }; \
	    echo "ok   $$t"; \
	done

gate:
	sh tools/gate.sh

clean:
	rm -rf build zfs_rebase

.PHONY: all freebsd check gate clean
