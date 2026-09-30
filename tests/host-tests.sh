#!/usr/bin/env bash
# Host tests that need this firmware's Lua, built for the host with the
# patched sources a firmware build leaves in libraries/:
# - tpbot-host-test: source/tpbot.c beside the Lua it replaced
#   (tests/tpbot-reference.lua), call by call;
# - runtime-host-test: source/lua-script.lua, and programs in its place,
#   over a stand-in board, with events through source/lua-events.c and
#   modules through source/lua-modules.c.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LUA="$ROOT/libraries/lua-5.1.5/src"
if ! grep -qs LUA_NUMBER_IS_FLOAT "$LUA/luaconf.h"; then
  echo "No patched Lua in libraries/lua-5.1.5: build the firmware once first." >&2
  exit 1
fi
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

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
  "$ROOT/source/lua-modules.c" "$ROOT/source/radio-inbox.c" "$DIR"/*.o -lm
"$DIR/tpbot" "$ROOT/tests/tpbot-reference.lua"
"$DIR/runtime" "$ROOT/source/lua-script.lua"
