// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/lua-cstack.c: the C stack limits Lua checks against
// are the stack region's size less their margins, so they move with it. A
// stand-in stack probe gives the region's size and the stack in use.
//
// Usage: cstack-host-test

#include <stdio.h>

#include "lua-cstack.h"
#include "stack-probe.h"

int luai_cstack(void);

static uint32_t used, region;

uint32_t stack_probe_current(void) { return used; }
uint32_t stack_probe_region(void) { return region; }

static int failures, checks;

// Where luai_cstack turns 1, then 2, and where a sleep stops running
// handlers, for a region of this size
static void limits(uint32_t size, uint32_t full, uint32_t overfull,
                   uint32_t safe_point) {
  char what[160];
  int ok;
  region = size;
  used = full;
  ok = luai_cstack() == 0;
  used = full + 1;
  ok = ok && luai_cstack() == 1;
  used = overfull;
  ok = ok && luai_cstack() == 1;
  used = overfull + 1;
  ok = ok && luai_cstack() == 2;
  ok = ok && lua_safe_point_stack(size) == safe_point;
  snprintf(what, sizeof what, "a %u-byte stack: \"C stack overflow\" past "
           "%u, no message past %u, handlers at sleeps up to %u",
           (unsigned)size, (unsigned)full, (unsigned)overfull,
           (unsigned)safe_point);
  checks++;
  if (!ok)
    failures++;
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// A region too small for a margin: each limit it does not fit is 0, never
// a number wrapped round
static void too_small(uint32_t size, uint32_t full, uint32_t overfull,
                      uint32_t safe_point) {
  char what[160];
  int ok = lua_cstack_full(size) == full
           && lua_cstack_overfull(size) == overfull
           && lua_safe_point_stack(size) == safe_point;
  snprintf(what, sizeof what, "a %u-byte stack: limits %u, %u, %u, none "
           "wrapped round", (unsigned)size, (unsigned)full,
           (unsigned)overfull, (unsigned)safe_point);
  checks++;
  if (!ok)
    failures++;
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

int main(void) {
  limits(8192, 5888, 6400, 4352);
  limits(6144, 3840, 4352, 2304);
  limits(16384, 14080, 14592, 12544);
  limits(LUA_CSTACK_REGION_MIN, 1792, 2304, 256);
  too_small(3840, 1536, 2048, 0);
  too_small(3839, 1535, 2047, 0);
  too_small(2304, 0, 512, 0);
  too_small(2048, 0, 256, 0);
  too_small(1792, 0, 0, 0);
  too_small(0, 0, 0, 0);
  // and on a 2 KB stack, CODAL's own, every call stops
  region = 2048;
  used = 100;
  checks++;
  if (luai_cstack() == 0)
    failures++;
  printf("%s on a 2 KB stack every call stops\n",
         luai_cstack() != 0 ? "ok  " : "FAIL");
  printf("%d stack checks, %d failed\n", checks, failures);
  return failures != 0;
}
