// -*- mode: c; indent-tabs-mode: nil; -*-
//
// Host test for source/lua-cstack.c: Lua may go deeper while the C stack in
// use leaves LUA_CSTACK_MARGIN of the region free, and stops past that. A
// stand-in stack probe gives the region's size and the stack in use.
//
// Usage: cstack-host-test

#include <stdio.h>

#include "stack-probe.h"

int luai_cstack(void);

static uint32_t used, region;

uint32_t stack_probe_current(void) { return used; }
uint32_t stack_probe_region(void) { return region; }

static int failures, checks;

static void check(uint32_t size, uint32_t in_use, int full) {
  region = size;
  used = in_use;
  checks++;
  if (luai_cstack() != full)
    failures++;
  printf("%s %u bytes in use of %u: %s\n",
         luai_cstack() == full ? "ok  " : "FAIL", (unsigned)in_use,
         (unsigned)size, full ? "full" : "room");
}

int main(void) {
  // the board's 8 KB region: 5,888 bytes in use is the most that goes on
  check(8192, 5888, 0);
  check(8192, 5889, 1);
  // a larger region moves the limit with it
  check(16384, 14080, 0);
  check(16384, 14081, 1);
  printf("%d stack checks, %d failed\n", checks, failures);
  return failures != 0;
}
