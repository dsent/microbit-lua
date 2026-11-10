#!/usr/bin/env bash
# The event engine (source/lua-events.c) under the firmware's script
# (source/lua-script.lua), on the host: builds the firmware's Lua from the
# tarball with every patch CMakeLists.txt lists, in its order and with no
# fuzz, and runs tests/runtime-host-test.c over a stand-in board.
# Needs cc, patch and the tarball a firmware build leaves in libraries/.
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
FLAGS=(-std=gnu99 -O2 -DLUA_NUMBER_IS_FLOAT=1 -I"$SRC" -I"$ROOT/source")
# Lua's own sources, the ones the firmware builds, their warnings aside
LUA=(lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
     lparser lstate lstring ltable ltm lundump lvm lzio
     lauxlib lbaselib lmathlib lstrlib ltablib)
(cd "$DIR" && cc "${FLAGS[@]}" -w -c $(printf "$SRC/%s.c " "${LUA[@]}") \
  "$ROOT/source/lua-cstack.c" "$ROOT/source/lua-number2str.c" \
  "$ROOT/source/lua-format.c" "$ROOT/source/lua-strip-debug.c" \
  "$DIR/stack-limit.c")
# and the engine and its test, with every warning
cc "${FLAGS[@]}" -Wall -Wextra -Werror -o "$DIR/runtime" \
  "$ROOT/tests/runtime-host-test.c" "$ROOT/source/lua-events.c" \
  "$DIR"/*.o -lm
"$DIR/runtime" "$ROOT/source/lua-script.lua"
