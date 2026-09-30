// -*- mode: c; indent-tabs-mode: nil; -*-
//
// How full the C stack is, for Lua (luai_cstack in luaconf.h).
//
// Every fiber runs in the one stack region, 8 KB (__StackSize), and nothing
// guards its bottom: a call that goes past it writes over the top of the
// heap, and the scheduler notices only at the next switch, with a 045
// panic. Lua checks before each call it makes from C, where it resumes a
// coroutine, and at each level of the parser: past the first limit it
// raises "C stack overflow", which a pcall catches, and past the second,
// where even the message could overflow, it throws with none. A pattern's
// match checks at each of its levels, 64 bytes at most apart, and says
// "pattern too complex". Each limit is the region's size less a margin
// (lua-cstack.h), read at run time, so the limits follow the region.
//
// LUA_CSTACK_FULL_MARGIN, 2,304 bytes, is what one Lua call can take
// without passing a check again, from the ARM build's -fstack-usage and
// call graph: luaD_call, luaD_precall, luaV_execute and luaD_precall again
// (216 bytes, and as many from a coroutine's resume on); the deepest C
// function below them, string.format stopping on a bad argument, with a
// 256-byte luaL_Buffer (1,584 bytes, an upper bound); and an interrupt that
// raises an event, the UART's through MessageBus to a new fiber, with the
// exception frame (about 500 bytes). LUA_CSTACK_OVERFULL_MARGIN, 1,792
// bytes, is room past the second limit for the error's own frames (480
// bytes) and an interrupt, with the rest of a pattern's level to spare.
// LUA_SAFE_POINT_MARGIN, 1,536 bytes below the first limit, is what the
// handlers a sleep runs have before it: a handler's chain from the sleep
// (cpcall, the dispatch, a CODAL scroll) and its own calls.

#include <stdint.h>

#include "luaconf.h"
#include "lua-cstack.h"
#include "stack-probe.h"

int luai_cstack(void) {
  return lua_cstack_level(stack_probe_current(), stack_probe_region());
}
