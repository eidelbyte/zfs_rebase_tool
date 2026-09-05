#!/bin/sh
# Regenerate the expect block of every fixture whose manifest the tool
# now writes differently: the same build and --posix run as
# tests/run-fixtures.sh, and where the body from #mode on differs, the
# fixture's expect block takes the new body under its own four header
# lines. Fixtures another platform owns are skipped as the runner
# skips them. Review the diff: this rewrites files under tests/.
#
#   sh tools/regen-expect.sh [FIXTURE.zrt ...]
set -u
cd "$(dirname "$0")/.." || exit 2
bin=./zfs_rebase
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
	"$bin" --posix $flag -o "$d/got" "$d/base" "$d/from" "$d/onto" > /dev/null 2>&1
	sed -n '/^#mode/,$p' "$f" > "$d/expect.body"
	sed -n '/^#mode/,$p' "$d/got" > "$d/got.body"
	cmp -s "$d/expect.body" "$d/got.body" && continue
	head=$(awk '/^expect$/{print NR; exit}' "$f")
	{ sed -n "1,$((head + 4))p" "$f"; cat "$d/got.body"; } > "$d/new" || exit 2
	cp "$d/new" "$f" || exit 2
	echo "regenerated $f"
	changed=$((changed + 1))
done
echo "regen-expect: $changed fixture(s) rewritten"
