#!/usr/bin/env bash
# Host test for source/tpbot.c: builds this firmware's Lua for the host, with
# the TPBot commands in C and, beside them, as the Lua had them
# (tests/tpbot-reference.lua), and compares the two call by call. It needs
# cc and the patched Lua sources a firmware build leaves in libraries/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LUA="$ROOT/libraries/lua-5.1.5/src"
if ! grep -qs LUA_NUMBER_IS_FLOAT "$LUA/luaconf.h"; then
  echo "No patched Lua in libraries/lua-5.1.5: build the firmware once first." >&2
  exit 1
fi
BIN=$(mktemp)
trap 'rm -f "$BIN"' EXIT

CORE=(lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
      lparser lstate lstring ltable ltm lundump lvm lzio
      lauxlib lbaselib lmathlib lstrlib ltablib)
# Lua's own sources as the firmware takes them, their warnings aside
cc -std=gnu99 -O2 -w -DLUA_NUMBER_IS_FLOAT=1 -I"$LUA" -I"$ROOT/source" \
  -o "$BIN" "$ROOT/tests/tpbot-host-test.c" "$ROOT/source/tpbot.c" \
  "$ROOT/source/lua-number.c" "$ROOT/source/lua-strip-debug.c" \
  $(printf "$LUA/%s.c " "${CORE[@]}") -lm
# and ours, with every warning
cc -std=gnu99 -O2 -Wall -Wextra -Werror -DLUA_NUMBER_IS_FLOAT=1 -I"$LUA" \
  -I"$ROOT/source" -fsyntax-only "$ROOT/tests/tpbot-host-test.c" \
  "$ROOT/source/tpbot.c"
"$BIN" "$ROOT/tests/tpbot-reference.lua"
