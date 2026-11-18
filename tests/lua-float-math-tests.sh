#!/usr/bin/env bash
# Lua's math in float (source/lua-float-math.h), on the host: checks that
# the header maps every call in Lua's sources to its float form, runs
# tests/lua-float-math-test.lua on Lua built as the firmware builds its
# numbers, and measures each float function against the double one rounded
# to float (tests/lua-float-math-test.c). The bound is 3 ulp: newlib's
# sinhf and coshf, which the firmware links, reach 3 near where they
# overflow, and its tanhf and log10f 2; the host's libm may do better. Needs cc, nm,
# patch and the tarball a firmware build leaves in libraries/.
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
# -fno-builtin, so that the host's compiler calls floor and the rest as
# the firmware's does rather than inlining them out of the check below
FLAGS=(-std=gnu99 -O2 -fno-builtin -DLUA_USE_POSIX -DLUA_NUMBER_IS_FLOAT=1
       -I"$SRC")
(cd "$DIR" && cc "${FLAGS[@]}" -include "$ROOT/source/lua-float-math.h" -w \
  -c $(ls "$SRC"/*.c | grep -v -e '/luac\.c$' -e '/print\.c$'))
# Lua's sources call no double math function: every one is mapped
nm -u "$DIR"/*.o > "$DIR/undefined.txt"
double=$(grep -wE \
  'fabs|sin|sinh|cos|cosh|tan|tanh|asin|acos|atan|atan2|ceil|floor|fmod|sqrt|pow|log|log10|exp|frexp|ldexp' \
  "$DIR/undefined.txt" || true)
if [ -n "$double" ]; then
  echo "FAIL Lua's sources still call double math:"
  echo "$double"
  exit 1
fi
echo "ok   Lua's sources call no double math function but modf"
(cd "$DIR" && cc "${FLAGS[@]}" -w -c "$ROOT/source/lua-cstack.c" \
  "$ROOT/source/lua-number2str.c" "$ROOT/source/lua-format.c" \
  "$DIR/stack-limit.c")
cc -o "$DIR/lua" "$DIR"/*.o -lm
"$DIR/lua" "$ROOT/tests/lua-float-math-test.lua"
cc -std=gnu99 -O2 -Wall -Wextra -Werror -o "$DIR/float-math" \
  "$ROOT/tests/lua-float-math-test.c" -lm
"$DIR/float-math" 3
