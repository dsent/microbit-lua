#!/usr/bin/env bash
# Host tests that need this firmware's Lua, built for the host from the Lua
# tarball a firmware build leaves in libraries/, with the patches
# CMakeLists.txt applies to it:
# - tpbot-host-test: the frames source/tpbot.c writes and what it says,
#   case by case;
# - cstack-host-test: source/lua-cstack.c, whose limit follows the stack
#   region's size.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARBALL="$ROOT/libraries/lua-5.1.5.tar.gz"
if [ ! -f "$TARBALL" ]; then
  echo "No Lua tarball in libraries/: build the firmware once first." >&2
  exit 1
fi
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
tar -xzf "$TARBALL" -C "$DIR"
LUA="$DIR/lua-5.1.5/src"
# lua_patch(FILE PATCH), in the order the build applies them
sed -n 's/^lua_patch(\([^ ]*\) \([^ )]*\))$/\1 \2/p' "$ROOT/CMakeLists.txt" |
  while read -r file patch; do
    patch --forward --batch --quiet "$LUA/$file" "$ROOT/source/$patch"
  done

CORE=(lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
      lparser lstate lstring ltable ltm lundump lvm lzio
      lauxlib lbaselib lmathlib lstrlib ltablib)
FLAGS=(-std=gnu99 -O2 -DLUA_NUMBER_IS_FLOAT=1 -I"$LUA" -I"$ROOT/source")
# Lua's own sources as the firmware takes them, their warnings aside
(cd "$DIR" && cc "${FLAGS[@]}" -w -c $(printf "$LUA/%s.c " "${CORE[@]}") \
  "$ROOT/source/lua-number.c" "$ROOT/source/lua-number2str.c" \
  "$ROOT/source/lua-strip-debug.c")
# and ours, with every warning
STRICT=("${FLAGS[@]}" -I"$ROOT/tests" -Wall -Wextra -Werror)
cc "${STRICT[@]}" -o "$DIR/tpbot" "$ROOT/tests/tpbot-host-test.c" \
  "$ROOT/tests/host-cstack.c" "$ROOT/source/tpbot.c" "$DIR"/*.o -lm
"$DIR/tpbot"
cc "${STRICT[@]}" -o "$DIR/cstack" "$ROOT/tests/cstack-host-test.c" \
  "$ROOT/source/lua-cstack.c"
"$DIR/cstack"
