#!/usr/bin/env bash
# Host test for source/lua-number.c: builds it with the host's C compiler and
# checks what it writes against the host's printf.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN=$(mktemp)
trap 'rm -f "$BIN"' EXIT

cc -std=c99 -O2 -Wall -Wextra -o "$BIN" \
  "$ROOT/tests/lua-number-test.c" "$ROOT/source/lua-number.c" -lm
"$BIN"
