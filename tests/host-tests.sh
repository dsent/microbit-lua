#!/usr/bin/env bash
# Host tests that need this firmware's Lua, built for the host from the Lua
# tarball a firmware build leaves in libraries/, with the patches
# CMakeLists.txt applies to it:
# - tpbot-host-test: source/tpbot.c beside the Lua it replaced
#   (tests/tpbot-reference.lua), call by call;
# - runtime-host-test: source/lua-script.lua, and programs in its place,
#   over a stand-in board, with events through source/lua-events.c,
#   modules through source/lua-modules.c, and radio links through
#   source/radio-link.c, both ends of them;
# - source/radio-link.c, which the fiber carrying the radio's event runs,
#   built without Lua's headers and calling nothing of Lua's;
# - cstack-host-test: source/lua-cstack.c, whose limits follow the stack
#   region's size;
# - wait-audit.sh: every binding that waits looks for on_event first;
# - lua-patch-test.cmake: the build's Lua patches, on Lua sources in each
#   state libraries/ can hold.
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
  "$ROOT/source/lua-number.c" "$ROOT/source/lua-strip-debug.c")
# and ours, with every warning
STRICT=("${FLAGS[@]}" -I"$ROOT/tests" -Wall -Wextra -Werror)
cc "${STRICT[@]}" -o "$DIR/tpbot" "$ROOT/tests/tpbot-host-test.c" \
  "$ROOT/tests/host-cstack.c" "$ROOT/source/tpbot.c" "$DIR"/*.o -lm
cc "${STRICT[@]}" -o "$DIR/runtime" "$ROOT/tests/runtime-host-test.c" \
  "$ROOT/tests/host-cstack.c" \
  "$ROOT/source/tpbot.c" "$ROOT/source/lua-events.c" \
  "$ROOT/source/lua-modules.c" "$ROOT/source/radio-link.c" \
  "$ROOT/source/link-words.c" "$DIR"/*.o -lm
"$DIR/tpbot" "$ROOT/tests/tpbot-reference.lua"
"$DIR/runtime" "$ROOT/source/lua-script.lua"
mkdir "$DIR/alone"
cc -std=gnu99 -O2 -Wall -Wextra -Werror -I"$ROOT/source" -c \
  -o "$DIR/alone/radio-link.o" "$ROOT/source/radio-link.c"
if nm -u "$DIR/alone/radio-link.o" | grep -i lua; then
  echo "source/radio-link.c calls into Lua" >&2
  exit 1
fi
echo "source/radio-link.c builds without Lua and calls nothing of Lua's"
cc "${STRICT[@]}" -o "$DIR/cstack" "$ROOT/tests/cstack-host-test.c" \
  "$ROOT/source/lua-cstack.c"
"$DIR/cstack"
bash "$ROOT/tests/wait-audit.sh"
mkdir "$DIR/patch-test"
cmake -DROOT="$ROOT" -DWORK="$DIR/patch-test" \
  -P "$ROOT/tests/lua-patch-test.cmake" | grep -v "^patching file"
