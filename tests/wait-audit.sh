#!/usr/bin/env bash
# Every binding in source/codal-lua.cpp that waits, letting other fibers
# run, calls lua_events_before_wait() first: an on_event a program has just
# set then gets the events that come while it waits. A binding is an
# F(name, { ... }) in one of the function lists; it waits when it names
# SYNC_SLEEP, a sleep, a display's print, scroll or animate, getPulseUs,
# readLightLevel, whose first reading sleeps, or one of the functions below
# that sleep for it: the radio link's call and frame, the color sensor's
# reading, and nezha2's travel.
# Waits inside other functions a binding calls are for their own reading.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
awk -v WAITS='SYNC_SLEEP|[.]sleep[(]|display[.](print|scroll|animate)[(]|getPulseUs[(]|readLightLevel[(]|radio_called[(]|radio_one[(]|color_light[(]|color_hsv[(]|nezha2_motor_travel[(]' '
function check() {
  if (body ~ WAITS && body !~ /lua_events_before_wait/) {
    printf "%s:%d: %s waits without lua_events_before_wait()\n", FILENAME, start, name
    bad = 1
  }
  name = ""
}
/^[ \t]*F\(/ {
  if (name != "") check()
  name = $0; sub(/^[ \t]*F\([ \t]*/, "", name); sub(/[ \t]*,.*/, "", name)
  start = FNR; body = ""; bindings++
}
name != "" { body = body $0 "\n" }
name != "" && !/\\[ \t]*$/ { check() }
END {
  if (name != "") check()
  if (!bad) printf "%d bindings, each that waits looks for on_event first\n", bindings
  exit bad
}' "$ROOT/source/codal-lua.cpp"
