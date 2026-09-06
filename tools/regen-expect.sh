#!/bin/sh
# Regenerate the expect block of every fixture whose manifest the tool
# now writes differently: the same build and --posix run as
# tests/run-fixtures.sh, and where anything differs -- the header or
# the body -- the fixture's expect block becomes the tool's whole
# --posix output. That output is one document per fixture: the posix
# form writes the placeholders of v4-manifest.md section 6 above
# #mode, and the run is made from the built directory so that the
# three names are always base, from and onto. Fixtures another
# platform owns are skipped as the runner skips them. Review the
# diff: this rewrites files under tests/.
#
#   sh tools/regen-expect.sh [FIXTURE.zrt ...]
set -u
cd "$(dirname "$0")/.." || exit 2
root=$(pwd)
bin=$root/zfs_rebase
[ -x "$bin" ] || { echo "build first: make"; exit 2; }
host=$(uname | tr 'A-Z' 'a-z')
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zr-regen.XXXXXX") || exit 2
trap 'chflags -R nouchg,nouappnd,noschg,nosappnd "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT
if [ $# -gt 0 ]; then set -- "$@"; else set -- tests/fixtures/*.zrt tests/fixtures/freebsd/*.zrt; fi
changed=0
for f in "$@"; do
	name=$(basename "$f" .zrt)
	plat=$(sed -n \
	    's/^[[:space:]]*platform[[:space:]][[:space:]]*\([a-z][a-z]*\).*/\1/p' \
	    "$f")
	if [ -n "$plat" ] && { [ "$plat" != "$host" ] || [ "$(id -u)" != 0 ]; }; then
		echo "skip $f (platform $plat)"
		continue
	fi
	grep -q '^expect$' "$f" || { echo "skip $f (no expect block)"; continue; }
	d="$tmp/$name"
	mkdir -p "$d" || exit 2
	"$bin" --build-fixture "$f" "$d" || { echo "FAIL build $f"; exit 1; }
	flag=""
	case "$name" in *-permissive) flag="-p" ;; esac
	(cd "$d" && "$bin" --posix $flag -o got base from onto) > /dev/null 2>&1
	sed -n '/^expect$/,$p' "$f" | sed '1d' > "$d/expect.block"
	cmp -s "$d/expect.block" "$d/got" && continue
	head=$(awk '/^expect$/{print NR; exit}' "$f")
	{ sed -n "1,${head}p" "$f"; cat "$d/got"; } > "$d/new" || exit 2
	cp "$d/new" "$f" || exit 2
	echo "regenerated $f"
	changed=$((changed + 1))
done
echo "regen-expect: $changed fixture(s) rewritten"
