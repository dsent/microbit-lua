#!/usr/bin/env bash
# connect()'s calls after a lost link, on the host: builds the Lua tarball
# with every patch CMakeLists.txt lists, as lua-limits-host-tests.sh does,
# and runs tests/connect-test.lua over the firmware's own script. Needs cc
# and the tarball a firmware build leaves in libraries/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
tar -xzf "$ROOT/libraries/lua-5.1.5.tar.gz" -C "$DIR"
SRC="$DIR/lua-5.1.5/src"
sed -n -E 's/^lua_patch\([^ ]+ ([^)]+)\)$/\1/p' "$ROOT/CMakeLists.txt" |
  while read -r patch; do
    patch -s -p0 -F0 -d "$SRC" < "$ROOT/source/$patch"
  done
# The host's stack lies far above this, so the C stack check never fires.
printf 'char stand_in[16] __asm__("__StackLimit");\n' > "$DIR/stack-limit.c"
extra=()
for f in lua-number2str.c lua-format.c; do
  [ -f "$ROOT/source/$f" ] && extra+=("$ROOT/source/$f")
done
cc -std=gnu99 -O2 -DLUA_USE_POSIX -I"$SRC" -o "$DIR/lua" \
  $(ls "$SRC"/*.c | grep -v -e '/luac\.c$' -e '/print\.c$') \
  "$ROOT/source/lua-cstack.c" "${extra[@]}" "$DIR/stack-limit.c" -lm
"$DIR/lua" "$ROOT/tests/connect-test.lua" "$ROOT/source/lua-script.lua"
