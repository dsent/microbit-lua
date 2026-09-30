// -*- mode: c; indent-tabs-mode: nil; -*-
//
// How full the C stack is, for Lua (luai_cstack in luaconf.h).
//
// Every fiber runs in the one stack region of 8 KB, and nothing guards its
// bottom: a call that goes past it writes over the top of the heap, and the
// scheduler notices only at the next switch, with a 045 panic. Lua checks
// before each call it makes from C, and at each level of the parser: past
// CSTACK_FULL it raises "C stack overflow", which a pcall catches, and past
// CSTACK_OVERFULL, where even the message could overflow, it throws with
// none. What lies between the limits and the region's end is left for the
// error's own frames and for interrupts.

#include <stdint.h>

#include "luaconf.h"
#include "stack-probe.h"

#define CSTACK_FULL     7168
#define CSTACK_OVERFULL 7680

int luai_cstack(void) {
  uint32_t used = stack_probe_current();
  if (used > CSTACK_OVERFULL)
    return 2;
  if (used > CSTACK_FULL)
    return 1;
  return 0;
}
