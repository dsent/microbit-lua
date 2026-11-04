#!/usr/bin/env bash
# The C stack limits (source/lua-cstack.c) at four stack sizes, which they
# follow, and at sizes too small for them, where none wraps round; needs cc.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
cc -std=gnu99 -O2 -Wall -Wextra -Werror -I"$ROOT/source" -I"$ROOT/libraries/lua-5.1.5/src" \
  -o "$DIR/cstack" "$ROOT/tests/cstack-host-test.c" "$ROOT/source/lua-cstack.c"
"$DIR/cstack"
