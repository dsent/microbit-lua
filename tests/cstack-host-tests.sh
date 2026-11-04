#!/usr/bin/env bash
# The C stack limit (source/lua-cstack.c) at two stack sizes; needs cc and
# the Lua tarball a firmware build leaves in libraries/, for luaconf.h.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
tar -xzf "$ROOT/libraries/lua-5.1.5.tar.gz" -C "$DIR"
cc -std=gnu99 -O2 -Wall -Wextra -Werror -I"$ROOT/source" -I"$DIR/lua-5.1.5/src" \
  -o "$DIR/cstack" "$ROOT/tests/cstack-host-test.c" "$ROOT/source/lua-cstack.c"
"$DIR/cstack"
