#!/usr/bin/env bash
# Every binding in source/codal-lua.cpp that waits, letting other fibers
# run, calls lua_events_before_wait() first: an on_event a program has just
# set then gets the events that come while it waits. A binding is an
# F(name, { ... }) in one of the function lists; it waits when it names
# SYNC_SLEEP, a sleep, a display's print, scroll or animate, or getPulseUs.
# Waits inside the functions a binding calls are for their own reading.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
awk -v WAITS='SYNC_SLEEP|[.]sleep[(]|display[.](print|scroll|animate)[(]|getPulseUs[(]' '
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
