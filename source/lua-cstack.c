// -*- mode: c; indent-tabs-mode: nil; -*-
//
// How full the C stack is, for Lua (luai_cstack in luaconf.h).
//
// Every fiber runs in the one stack region of 8 KB, and nothing guards its
// bottom: a call that goes past it writes over the top of the heap, and the
// scheduler notices only at the next switch, with a 045 panic. Lua checks
// before each call it makes from C, where it resumes a coroutine, and at
// each level of the parser: past CSTACK_FULL it raises "C stack overflow",
// which a pcall catches, and past CSTACK_OVERFULL, where even the message
// could overflow, it throws with none. The 2,304 bytes left above
// CSTACK_FULL are what one Lua call can take without passing a check again,
// from the ARM build's -fstack-usage and call graph: luaD_call,
// luaD_precall, luaV_execute and luaD_precall again (216 bytes; a resumed
// coroutine's, from resume on, 208); the deepest C function below them,
// string.format stopping on a bad argument, with a 256-byte luaL_Buffer
// (1,584 bytes, an upper bound); and an interrupt that raises an event, the
// UART's through MessageBus to a new fiber, with the exception frame (about
// 500 bytes). Past CSTACK_OVERFULL there is room for the error's own frames
// (480 bytes) and an interrupt.

#include <stdint.h>

#include "luaconf.h"
#include "stack-probe.h"

#define CSTACK_FULL     5888
#define CSTACK_OVERFULL 6400

int luai_cstack(void) {
  uint32_t used = stack_probe_current();
  if (used > CSTACK_OVERFULL)
    return 2;
  if (used > CSTACK_FULL)
    return 1;
  return 0;
}
