#!/usr/bin/env bash
# hextract test harness - runs all tests and reports results
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HEXTRACT="$PROJECT_ROOT/utils/hextract"
FIRMWARE="$PROJECT_ROOT/MICROBIT.hex"
TMPDIR=$(mktemp -d)

trap 'rm -rf "$TMPDIR"' EXIT

PASS=0
FAIL=0
TOTAL=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() {
  PASS=$((PASS + 1))
  TOTAL=$((TOTAL + 1))
  echo -e "  ${GREEN}PASS${NC} $1"
}

fail() {
  FAIL=$((FAIL + 1))
  TOTAL=$((TOTAL + 1))
  echo -e "  ${RED}FAIL${NC} $1: $2"
}

section() {
  echo ""
  echo -e "${YELLOW}=== $1 ===${NC}"
}

# Generate deterministic text data from a fixed seed
generate_data() {
  local size=$1
  local output=$2
  lua5.1 -e "
    math.randomseed(42)
    local f = io.open('$output', 'wb')
    for i = 1, $size do
      f:write(string.char(math.random(32, 126)))
      if i % 64 == 0 then f:write('\n') end
    end
    f:close()
  "
}

# Generate exactly <size> bytes; generate_data also writes a newline after
# every 64 bytes
generate_exact_data() {
  local size=$1
  local output=$2
  generate_data "$size" "$output.full"
  head -c "$size" "$output.full" > "$output"
}

# Succeed when structure reports every record of a hex valid and no errors
checksums_valid() {
  local output
  output=$("$HEXTRACT" structure "$1" 2>&1)
  ! echo "$output" | grep -q "^=== CHECKSUM / FORMAT ERRORS ===$" \
    && echo "$output" | grep -qE "^  Checksums: ([0-9]+)/\1 valid$"
}

# Verify prerequisites
if [ ! -f "$FIRMWARE" ]; then
  echo "Error: $FIRMWARE not found. Build the project first."
  exit 1
fi

if [ ! -f "$HEXTRACT" ]; then
  echo "Error: $HEXTRACT not found."
  exit 1
fi

# Generate test data
generate_data 50   "$TMPDIR/tiny.dat"
generate_data 200  "$TMPDIR/small.dat"
generate_data 2048 "$TMPDIR/medium.dat"
generate_data 10240 "$TMPDIR/large.dat"

# Extract original script for comparison
"$HEXTRACT" extract "$FIRMWARE" "$TMPDIR/original.dat"
ORIGINAL_SIZE=$(wc -c < "$TMPDIR/original.dat")

section "Structure Command"

# Test 1: Structure parses metadata
OUTPUT=$("$HEXTRACT" structure "$FIRMWARE" 2>&1)
if echo "$OUTPUT" | grep -q "LUA METADATA"; then
  pass "structure detects metadata"
else
  fail "structure detects metadata" "no metadata section found"
fi

# Test 2: Structure shows magic
if echo "$OUTPUT" | grep -q "LUA1"; then
  pass "structure shows LUA1 magic"
else
  fail "structure shows LUA1 magic" "magic not found"
fi

# Test 3: Structure shows script preview
if echo "$OUTPUT" | grep -q "Script preview"; then
  pass "structure shows script preview"
else
  fail "structure shows script preview" "preview not found"
fi

section "Extract Command"

# Test 4: Extract to file produces output
if [ -s "$TMPDIR/original.dat" ]; then
  pass "extract to file produces output"
else
  fail "extract to file produces output" "file empty or missing"
fi

# Test 5: Extract to stdout matches file output
STDOUT_EXTRACT=$("$HEXTRACT" extract "$FIRMWARE" - 2>/dev/null)
FILE_EXTRACT=$(cat "$TMPDIR/original.dat")
if [ "$STDOUT_EXTRACT" = "$FILE_EXTRACT" ]; then
  pass "extract to stdout matches file output"
else
  fail "extract to stdout matches file output" "outputs differ"
fi

section "Embed Command"

# Test 6: Embed same script (idempotent)
cp "$FIRMWARE" "$TMPDIR/fw-idem.hex"
"$HEXTRACT" embed "$TMPDIR/fw-idem.hex" "$TMPDIR/original.dat" --overwrite
cp "$TMPDIR/fw-idem.hex" "$TMPDIR/fw-idem-2.hex"
"$HEXTRACT" embed "$TMPDIR/fw-idem-2.hex" "$TMPDIR/original.dat" --overwrite
if cmp -s "$TMPDIR/fw-idem.hex" "$TMPDIR/fw-idem-2.hex"; then
  pass "embed idempotent (same data twice)"
else
  fail "embed idempotent (same data twice)" "hex files differ"
fi

# Test 7: Embed smaller data
cp "$FIRMWARE" "$TMPDIR/fw-small.hex"
"$HEXTRACT" embed "$TMPDIR/fw-small.hex" "$TMPDIR/tiny.dat" --overwrite
OUTPUT=$("$HEXTRACT" structure "$TMPDIR/fw-small.hex" 2>&1)
if echo "$OUTPUT" | grep -q "Size:"; then
  pass "embed smaller data updates metadata"
else
  fail "embed smaller data updates metadata" "metadata not updated"
fi

# Test 8: Extract after smaller embed matches source
"$HEXTRACT" extract "$TMPDIR/fw-small.hex" "$TMPDIR/extracted-tiny.dat"
if cmp -s "$TMPDIR/tiny.dat" "$TMPDIR/extracted-tiny.dat"; then
  pass "extract after smaller embed matches source"
else
  fail "extract after smaller embed matches source" "data differs"
fi

# Test 9: Embed larger data
cp "$FIRMWARE" "$TMPDIR/fw-large.hex"
"$HEXTRACT" embed "$TMPDIR/fw-large.hex" "$TMPDIR/large.dat" --overwrite
"$HEXTRACT" extract "$TMPDIR/fw-large.hex" "$TMPDIR/extracted-large.dat"
if cmp -s "$TMPDIR/large.dat" "$TMPDIR/extracted-large.dat"; then
  pass "embed larger data round-trip"
else
  fail "embed larger data round-trip" "data differs"
fi

# Test 10: Embed too large data (should fail)
generate_data 400000 "$TMPDIR/huge.dat"
if ! "$HEXTRACT" embed "$FIRMWARE" "$TMPDIR/huge.dat" "$TMPDIR/fw-huge.hex" 2>/dev/null; then
  pass "embed rejects data too large"
else
  fail "embed rejects data too large" "should have failed"
fi

# The bounds embed enforces must match the ones metadata detection checks
# (0 < Size < Space), or embed writes metadata that detection skips, and
# the hex then reads back only if it holds another valid candidate.
SPACE=$("$HEXTRACT" structure "$FIRMWARE" | sed -n 's/^  Space: 0x[0-9A-F]* (\([0-9]*\))$/\1/p')
if [ -z "$SPACE" ]; then
  echo "Error: cannot read Space from $FIRMWARE."
  exit 1
fi

# Test 10a: Embed a script of Space - 1 bytes, the largest that fits
generate_exact_data $((SPACE - 1)) "$TMPDIR/space-1.dat"
if "$HEXTRACT" embed "$FIRMWARE" "$TMPDIR/space-1.dat" "$TMPDIR/fw-space-1.hex" > /dev/null \
   && "$HEXTRACT" extract "$TMPDIR/fw-space-1.hex" "$TMPDIR/extracted-space-1.dat" \
   && cmp -s "$TMPDIR/space-1.dat" "$TMPDIR/extracted-space-1.dat" \
   && checksums_valid "$TMPDIR/fw-space-1.hex"; then
  pass "embed Space - 1 bytes round-trip"
else
  fail "embed Space - 1 bytes round-trip" "embed or extract failed, data differs, or a checksum is bad"
fi

# Test 10b: Embed a script of exactly Space bytes (should fail)
generate_exact_data "$SPACE" "$TMPDIR/space.dat"
if ! "$HEXTRACT" embed "$FIRMWARE" "$TMPDIR/space.dat" "$TMPDIR/fw-space.hex" 2>/dev/null \
   && [ ! -e "$TMPDIR/fw-space.hex" ]; then
  pass "embed rejects Space bytes"
else
  fail "embed rejects Space bytes" "should have failed without writing a hex"
fi

# Test 10c: Embed an empty script (should fail)
: > "$TMPDIR/empty.dat"
if ! "$HEXTRACT" embed "$FIRMWARE" "$TMPDIR/empty.dat" "$TMPDIR/fw-empty.hex" 2>/dev/null \
   && [ ! -e "$TMPDIR/fw-empty.hex" ]; then
  pass "embed rejects an empty script"
else
  fail "embed rejects an empty script" "should have failed without writing a hex"
fi

# Test 10d: Embed a one-byte script, the smallest that fits
printf 'x' > "$TMPDIR/one.dat"
if "$HEXTRACT" embed "$FIRMWARE" "$TMPDIR/one.dat" "$TMPDIR/fw-one.hex" > /dev/null \
   && "$HEXTRACT" extract "$TMPDIR/fw-one.hex" "$TMPDIR/extracted-one.dat" \
   && cmp -s "$TMPDIR/one.dat" "$TMPDIR/extracted-one.dat" \
   && checksums_valid "$TMPDIR/fw-one.hex"; then
  pass "embed one byte round-trip"
else
  fail "embed one byte round-trip" "embed or extract failed, data differs, or a checksum is bad"
fi

# Test 11: All checksums valid after embed
OUTPUT=$("$HEXTRACT" structure "$TMPDIR/fw-large.hex" 2>&1)
if echo "$OUTPUT" | grep -q "Checksums: [0-9]*/[0-9]* valid"; then
  CKSUM_LINE=$(echo "$OUTPUT" | grep "Checksums:")
  TOTAL_CK=$(echo "$CKSUM_LINE" | grep -o '[0-9]*/[0-9]*')
  VALID_CK=$(echo "$TOTAL_CK" | cut -d/ -f1)
  ALL_CK=$(echo "$TOTAL_CK" | cut -d/ -f2)
  if [ "$VALID_CK" = "$ALL_CK" ]; then
    pass "all checksums valid after embed"
  else
    fail "all checksums valid after embed" "$VALID_CK/$ALL_CK valid"
  fi
else
  fail "all checksums valid after embed" "no checksum line found"
fi

# Test 12: Sequential embeds (shrink -> grow -> shrink)
cp "$FIRMWARE" "$TMPDIR/fw-seq.hex"
"$HEXTRACT" embed "$TMPDIR/fw-seq.hex" "$TMPDIR/tiny.dat" --overwrite
"$HEXTRACT" embed "$TMPDIR/fw-seq.hex" "$TMPDIR/large.dat" --overwrite
"$HEXTRACT" embed "$TMPDIR/fw-seq.hex" "$TMPDIR/small.dat" --overwrite
"$HEXTRACT" extract "$TMPDIR/fw-seq.hex" "$TMPDIR/extracted-seq.dat"
if cmp -s "$TMPDIR/small.dat" "$TMPDIR/extracted-seq.dat"; then
  pass "sequential embeds (shrink->grow->shrink)"
else
  fail "sequential embeds (shrink->grow->shrink)" "final data differs"
fi

# Test 13: Round-trip integrity
cp "$FIRMWARE" "$TMPDIR/fw-rt.hex"
"$HEXTRACT" embed "$TMPDIR/fw-rt.hex" "$TMPDIR/medium.dat" --overwrite
"$HEXTRACT" extract "$TMPDIR/fw-rt.hex" "$TMPDIR/rt-extracted.dat"
if cmp -s "$TMPDIR/medium.dat" "$TMPDIR/rt-extracted.dat"; then
  pass "round-trip integrity (embed->extract)"
else
  fail "round-trip integrity (embed->extract)" "data differs"
fi

# Test 14: --overwrite flag works silently
cp "$FIRMWARE" "$TMPDIR/fw-ow.hex"
OUTPUT=$("$HEXTRACT" embed "$TMPDIR/fw-ow.hex" "$TMPDIR/tiny.dat" --overwrite 2>&1)
if echo "$OUTPUT" | grep -q "Embedded"; then
  pass "--overwrite flag works silently"
else
  fail "--overwrite flag works silently" "unexpected output: $OUTPUT"
fi

# Test 15: Output to different file preserves original
cp "$FIRMWARE" "$TMPDIR/fw-orig.hex"
cp "$FIRMWARE" "$TMPDIR/fw-copy.hex"
"$HEXTRACT" embed "$TMPDIR/fw-orig.hex" "$TMPDIR/small.dat" "$TMPDIR/fw-out.hex" --overwrite
if cmp -s "$TMPDIR/fw-orig.hex" "$TMPDIR/fw-copy.hex"; then
  pass "output to different file preserves original"
else
  fail "output to different file preserves original" "original was modified"
fi

# Test 16: LF line endings in output hex
if grep -qP '\r\n' "$TMPDIR/fw-out.hex"; then
  fail "output hex uses LF line endings" "CRLF found"
else
  pass "output hex uses LF line endings"
fi

# Test 17: The script's Start address unchanged after embed
cp "$FIRMWARE" "$TMPDIR/fw-meta.hex"
"$HEXTRACT" embed "$TMPDIR/fw-meta.hex" "$TMPDIR/small.dat" --overwrite
ORIG_META=$("$HEXTRACT" structure "$FIRMWARE" 2>&1 | grep "Start:" | head -1)
NEW_META=$("$HEXTRACT" structure "$TMPDIR/fw-meta.hex" 2>&1 | grep "Start:" | head -1)
if [ "$ORIG_META" = "$NEW_META" ]; then
  pass "script Start address unchanged after embed"
else
  fail "script Start address unchanged after embed" "start changed: $ORIG_META -> $NEW_META"
fi

# Test 18: Round-trip produces identical hex file
cp "$FIRMWARE" "$TMPDIR/fw-rt-hex.hex"
"$HEXTRACT" extract "$FIRMWARE" "$TMPDIR/rt-script.dat"
"$HEXTRACT" embed "$TMPDIR/fw-rt-hex.hex" "$TMPDIR/rt-script.dat" "$TMPDIR/fw-rt-out.hex" --overwrite
if diff -wq "$FIRMWARE" "$TMPDIR/fw-rt-out.hex" > /dev/null 2>&1; then
  pass "round-trip produces identical hex file"
else
  DIFF_LINES=$(diff -w "$FIRMWARE" "$TMPDIR/fw-rt-out.hex" | wc -l || true)
  fail "round-trip produces identical hex file" "$DIFF_LINES diff lines"
fi

# Summary
section "Results"
echo -e "  ${GREEN}Passed: $PASS${NC}"
if [ $FAIL -gt 0 ]; then
  echo -e "  ${RED}Failed: $FAIL${NC}"
fi
echo "  Total:  $TOTAL"

if [ $FAIL -gt 0 ]; then
  exit 1
fi
